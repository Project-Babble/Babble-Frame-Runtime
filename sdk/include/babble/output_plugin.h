/*
 * Babble Frame Runtime: output plugin ABI, version 1.
 *
 * An output plugin is a shared library that the runtime loads at startup
 * (config: sinks.<name> = {"type": "plugin", "library": "...", "config": {...}})
 * and that sends tracked values somewhere: VRChat, a network service, a file.
 * This header is the whole contract. It is plain C so plugins may be written
 * in any language and built with any compiler; nothing else from the runtime
 * is needed to build one.
 *
 * Lifecycle, all on the runtime's main thread:
 *   babble_output_plugin_v1()  once, after dlopen
 *   create(host, instance, config_json)
 *   start(self)                the plugin starts its own thread(s) here
 *   ...
 *   stop(self)                 must return promptly (the runtime's whole
 *                              shutdown budget is about 2 s)
 *   destroy(self)
 *
 * Tracked values are "signals": named floats such as "jawOpen",
 * "LeftEyeLid" or "LeftEyeX" (see docs/output-modules.md for the list).
 * Plugins read them through the host table, typically by waiting for the next
 * update and reading the signals they care about. Every host function is safe
 * to call from any thread between create() and destroy().
 */
#ifndef BABBLE_OUTPUT_PLUGIN_H
#define BABBLE_OUTPUT_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BABBLE_OUTPUT_ABI 1

/* Log levels for babble_host_v1.log. */
enum { BABBLE_LOG_DEBUG = 0, BABBLE_LOG_INFO = 1, BABBLE_LOG_WARN = 2, BABBLE_LOG_ERROR = 3 };

/* What the runtime offers a plugin. */
typedef struct babble_host_v1 {
    uint32_t abi; /* BABBLE_OUTPUT_ABI */
    void *ctx;    /* pass back to every call */

    /* Number of signals; ids are 0 .. count-1 and stable for the whole run. */
    uint32_t (*signal_count)(void *ctx);
    /* A signal's name, or NULL for an invalid id. The pointer stays valid until destroy(). */
    const char *(*signal_name)(void *ctx, uint32_t id);
    /* Current values of n signals (values[i] for ids[i]); t_ns may be NULL,
     * otherwise it receives each value's CLOCK_MONOTONIC time (0 = never set).
     * Returns the update sequence number the values belong to. */
    uint64_t (*read)(void *ctx, const uint32_t *ids, uint32_t n, float *values, uint64_t *t_ns);
    /* Blocks until the sequence number exceeds after_seq or timeout_ms passes;
     * returns the current sequence number either way. */
    uint64_t (*wait_update)(void *ctx, uint64_t after_seq, uint32_t timeout_ms);
    /* Writes a line to the runtime log, prefixed with the plugin instance's name. */
    void (*log)(void *ctx, int level, const char *message);
    /* A writable directory for this plugin's persistent files. */
    const char *(*state_dir)(void *ctx);
} babble_host_v1;

/* What a plugin offers the runtime. */
typedef struct babble_output_v1 {
    uint32_t abi;        /* BABBLE_OUTPUT_ABI */
    const char *name;    /* short identifier, e.g. "udp_json" */
    const char *version; /* free-form, logged at load */

    /* Makes an instance. instance: the config key; config_json: the entry's
     * "config" object as JSON text ("{}" if absent). The host table outlives
     * the instance. Returns NULL on failure (log why first). */
    void *(*create)(const babble_host_v1 *host, const char *instance, const char *config_json);
    /* Starts sending; returns 0 on success. */
    int (*start)(void *self);
    /* Stops sending and joins the plugin's threads; must return promptly. */
    void (*stop)(void *self);
    /* Frees the instance. */
    void (*destroy)(void *self);
} babble_output_v1;

/* The one symbol a plugin exports. */
typedef const babble_output_v1 *(*babble_output_plugin_v1_fn)(void);
#define BABBLE_OUTPUT_PLUGIN_ENTRY "babble_output_plugin_v1"

#if defined(__GNUC__)
#define BABBLE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define BABBLE_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
}
#endif

#endif /* BABBLE_OUTPUT_PLUGIN_H */
