#include "cy_udp_posix.h"
#include <rapidhash.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

static uint64_t random_uid(void)
{
    const uint16_t vid = UINT16_MAX; // This is the reserved public VID.
    const uint16_t pid = (uint16_t)rand();
    const uint32_t iid = (uint32_t)rand();
    return (((uint64_t)vid) << 48U) | (((uint64_t)pid) << 32U) | iid;
}

static uint64_t arg_kv_hash(const char* s)
{
    return rapidhash(s, strlen(s));
}

/// The pointed strings have a static lifetime.
struct arg_kv_t
{
    size_t      index;    ///< Argument index, where 0 is the program name.
    const char* key;      ///< NULL key indicates that no more arguments are available.
    uint64_t    key_hash; ///< arg_kv_hash(key); 0 if no key.
    const char* value;    ///< NULL unless the argument matches "key=value". May be empty if "key=".
};

/// Returns the next argument key/value pair at every invocation. Returns NULL key when there are no more arguments.
/// Invokes exit(1) with a message if the arguments are malformed.
/// The argv array past the zeroth index may be mutated.
static struct arg_kv_t arg_kv_next(const int argc, char* argv[])
{
    if (argc <= 1) {
        fprintf(stderr,
                "Usage:\n\t%s key1[=value1] [key2[=value2] ...]\n"
                "No spaces around '=' are allowed.",
                argv[0]);
        exit(1);
    }
    static size_t   index = 1;
    struct arg_kv_t out   = { .index = index++, .key = NULL, .key_hash = 0, .value = NULL };
    if (((int)out.index) < argc) {
        out.key       = argv[out.index];
        char* const q = strchr(out.key, '=');
        if (q != NULL) {
            *q        = '\0';
            out.value = q + 1;
        }
        out.key_hash = arg_kv_hash(out.key);
    }
    return out;
}

struct config_topic_t
{
    const char* name;
    bool        pub;
    bool        sub;
};

struct config_t
{
    uint32_t iface_address[CY_UDP_POSIX_IFACE_COUNT_MAX];
    uint64_t local_uid;
    size_t   tx_queue_capacity_per_iface;

    const char* namespace;

    size_t                 topic_count;
    struct config_topic_t* topics;
};

static struct config_t load_config(const int argc, char* argv[])
{
    // Load default config.
    struct config_t cfg = {
        .local_uid                   = random_uid(),
        .tx_queue_capacity_per_iface = 1000,
        .namespace                   = NULL, // will use the default namespace by default.
        .topic_count                 = 0,
        .topics                      = calloc((size_t)(argc - 1), sizeof(struct config_topic_t)),
    };

    // Parse CLI args.
    size_t          iface_count = 0;
    struct arg_kv_t arg;
    while ((arg = arg_kv_next(argc, argv)).key_hash != 0) {
        if ((arg_kv_hash("iface") == arg.key_hash) && (iface_count < CY_UDP_POSIX_IFACE_COUNT_MAX)) {
            cfg.iface_address[iface_count++] = udp_wrapper_parse_iface_address(arg.value);
        } else if (arg_kv_hash("uid") == arg.key_hash) {
            cfg.local_uid = strtoull(arg.value, NULL, 0);
        } else if (arg_kv_hash("tx_queue_capacity") == arg.key_hash) {
            cfg.tx_queue_capacity_per_iface = strtoul(arg.value, NULL, 0);
        } else if (arg_kv_hash("ns") == arg.key_hash) {
            cfg.namespace = arg.value;
        } else if ((arg_kv_hash("pub") == arg.key_hash) || (arg_kv_hash("sub") == arg.key_hash)) {
            struct config_topic_t* topic = NULL;
            for (size_t i = 0; i < cfg.topic_count; i++) {
                if (strcmp(cfg.topics[i].name, arg.value) == 0) {
                    topic = &cfg.topics[i];
                }
            }
            if (topic == NULL) {
                topic = &cfg.topics[cfg.topic_count++];
            }
            topic->name = arg.value;
            topic->pub  = topic->pub || (arg_kv_hash("pub") == arg.key_hash);
            topic->sub  = topic->sub || (arg_kv_hash("sub") == arg.key_hash);
        } else {
            fprintf(stderr, "Unexpected key #%zu: '%s'\n", arg.index, arg.key);
            exit(1);
        }
    }

    // Print the actual configs we're using.
    fprintf(stderr, "ifaces:");
    for (size_t i = 0; i < CY_UDP_POSIX_IFACE_COUNT_MAX; i++) {
        fprintf(stderr, " 0x%08x", cfg.iface_address[i]);
    }
    fprintf(stderr, "\nuid: 0x%016llx\n", (unsigned long long)cfg.local_uid);
    fprintf(stderr, "tx_queue_capacity: %zu\n", cfg.tx_queue_capacity_per_iface);
    fprintf(stderr, "topics:\n");
    for (size_t i = 0; i < cfg.topic_count; i++) {
        fprintf(stderr, "\t%s\n", cfg.topics[i].name);
    }
    fprintf(stderr, "---\n");
    return cfg;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void on_msg_trace(struct cy_subscription_t* const subscription)
{
    struct cy_transfer_owned_t* const transfer = &subscription->topic->sub_last_transfer;
    CY_BUFFER_GATHER_ON_STACK(payload, transfer->payload.base)

    // Convert linearized payload to hex.
    char hex[payload.size * 2 + 1];
    for (size_t i = 0; i < payload.size; i++) {
        sprintf(hex + i * 2, "%02x", ((const uint8_t*)payload.data)[i]);
    }
    hex[sizeof(hex) - 1] = '\0';

    // Convert linearized payload to ASCII.
    char ascii[payload.size + 1];
    for (size_t i = 0; i < payload.size; i++) {
        const char ch = ((const char*)payload.data)[i];
        ascii[i]      = isprint(ch) ? ch : '.';
    }
    ascii[payload.size] = '\0';

    // Release the payload buffer memory.
    // This memory comes all the way from the bottom layer of the stack with zero copying.
    // If we don't release it now, it will be released only when the next message arrives, which is wasteful.
    cy_buffer_owned_release(subscription->topic->cy, &transfer->payload);

    // Log the message.
    CY_TRACE(subscription->topic->cy,
             "💬 [sid=%04x nid=%04x tid=%016llx sz=%06zu ts=%09llu] @ %s [age=%llu]:\n%s\n%s",
             cy_topic_get_subject_id(subscription->topic),
             transfer->metadata.remote_node_id,
             (unsigned long long)transfer->metadata.transfer_id,
             payload.size,
             (unsigned long long)transfer->timestamp,
             subscription->topic->name,
             (unsigned long long)subscription->topic->age,
             hex,
             ascii);

    // Optionally, send a direct p2p response to the publisher of this message.
    if (cy_has_node_id(subscription->topic->cy) && ((rand() % 2) == 0)) {
        const cy_err_t err = cy_respond(subscription->topic, //
                                        transfer->timestamp + 1000000,
                                        transfer->metadata,
                                        (struct cy_buffer_borrowed_t){ .view = { .data = ":3", .size = 2 } });
        if (err < 0) {
            fprintf(stderr, "cy_respond: %d\n", err);
        }
    }
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void on_response_trace(struct cy_future_t* const future)
{
    struct cy_topic_t* topic = future->topic;
    if (future->state == cy_future_success) {
        struct cy_transfer_owned_t* const transfer = &future->last_response;
        CY_BUFFER_GATHER_ON_STACK(payload, transfer->payload.base)

        // Convert payload to hex.
        char hex[payload.size * 2 + 1];
        for (size_t i = 0; i < payload.size; i++) {
            sprintf(hex + i * 2, "%02x", ((const uint8_t*)payload.data)[i]);
        }
        hex[sizeof(hex) - 1] = '\0';

        // Convert payload to ASCII.
        char ascii[payload.size + 1];
        for (size_t i = 0; i < payload.size; i++) {
            const char ch = ((const char*)payload.data)[i];
            ascii[i]      = isprint(ch) ? ch : '.';
        }
        ascii[payload.size] = '\0';

        // Release the payload buffer memory.
        // This memory comes all the way from the bottom layer of the stack with zero copying.
        // If we don't release it now, it will be released only when the next response arrives, which is wasteful.
        cy_buffer_owned_release(topic->cy, &transfer->payload);

        // Log the response.
        CY_TRACE(topic->cy,
                 "↩️ [sid=%04x nid=%04x tid=%016llx sz=%06zu ts=%09llu] @ %s [age=%llu]:\n%s\n%s",
                 cy_topic_get_subject_id(topic),
                 transfer->metadata.remote_node_id,
                 (unsigned long long)transfer->metadata.transfer_id,
                 payload.size,
                 (unsigned long long)transfer->timestamp,
                 topic->name,
                 (unsigned long long)topic->age,
                 hex,
                 ascii);
    } else if (future->state == cy_future_failure) {
        CY_TRACE(topic->cy,
                 "↩️⌛ Response to %s tid %016llx (masked) has timed out",
                 future->topic->name,
                 (unsigned long long)future->transfer_id_masked);
    } else {
        assert(false);
    }
}

int main(const int argc, char* argv[])
{
    srand((unsigned)time(NULL));
    const struct config_t cfg = load_config(argc, argv);

    // Set up the node instance. The initialization is the only platform-specific part.
    // The rest of the API is platform- and transport-agnostic.
    struct cy_udp_posix_t cy_udp_posix;
    {
        const cy_err_t res = cy_udp_posix_new(&cy_udp_posix, //
                                              cfg.local_uid,
                                              cfg.namespace,
                                              cfg.iface_address,
                                              cfg.tx_queue_capacity_per_iface);
        if (res < 0) {
            fprintf(stderr, "cy_udp_posix_new: %d\n", res);
            return 1;
        }
    }
    struct cy_t* const cy = &cy_udp_posix.base;

    // ------------------------------  End of the platform- and transport-specific part  ------------------------------

    // Create topics.
    struct cy_topic_t*       topics[cfg.topic_count];
    struct cy_subscription_t subs[cfg.topic_count];
    for (size_t i = 0; i < cfg.topic_count; i++) {
        topics[i] = cy_topic_new(cy, cfg.topics[i].name);
        if (topics[i] == NULL) {
            fprintf(stderr, "cy_topic_new: %p\n", (void*)topics[i]);
            return 1;
        }
        if (cfg.topics[i].sub) {
            const cy_err_t res = cy_subscribe(topics[i], &subs[i], 1024 * 1024, on_msg_trace);
            if (res < 0) {
                fprintf(stderr, "cy_subscribe: %d\n", res);
                return 1;
            }
        }
    }
    struct cy_future_t* futures = calloc(cfg.topic_count, sizeof(struct cy_future_t));
    for (size_t i = 0; i < cfg.topic_count; i++) {
        futures->state    = cy_future_success;
        futures->callback = on_response_trace;
    }

    // Spin the event loop and publish the topics.
    cy_us_t next_publish_at = cy_now(cy) + 1000000;
    while (true) {
        const cy_err_t err_spin = cy_udp_posix_spin_once(&cy_udp_posix);
        if (err_spin < 0) {
            fprintf(stderr, "cy_udp_posix_spin_once: %d\n", err_spin);
            break;
        }

        // Publish messages.
        // I'm thinking that it would be nice to have olga_scheduler ported to C11...
        // See https://github.com/Zubax/olga_scheduler
        const cy_us_t now = cy_now(cy);
        if (now >= next_publish_at) {
            if (cy_has_node_id(cy)) {
                for (size_t i = 0; i < cfg.topic_count; i++) {
                    if ((!cfg.topics[i].pub) || (futures[i].state == cy_future_pending)) {
                        continue;
                    }
                    char msg[256];
                    sprintf(msg,
                            "Hello from %016llx! The current time is %lld us.",
                            (unsigned long long)cy->uid,
                            (long long)now);
                    const cy_err_t pub_res =
                      cy_publish(topics[i],
                                 now + 100000,
                                 (struct cy_buffer_borrowed_t){ .view = { .data = msg, .size = strlen(msg) } },
                                 now + 1000000,
                                 &futures[i]);
                    if (pub_res < 0) {
                        fprintf(stderr, "cy_publish: %d\n", pub_res);
                        break;
                    }
                }
            }
            next_publish_at += 1000000U;

            // CY_TRACE(cy,
            //          "Heap: allocated %zu fragments, %llu OOMs",
            //          cy_udp_posix.mem_allocated_fragments,
            //          (unsigned long long)cy_udp_posix.mem_oom_count);
        }
    }

    return 0;
}
