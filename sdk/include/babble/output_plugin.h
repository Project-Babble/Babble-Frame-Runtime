/* This plain C header is the full ABI (version 1) for an output plugin,
 * which is a shared library that the runtime loads to send tracked values. */
#ifndef BABBLE_OUTPUT_PLUGIN_H
#define BABBLE_OUTPUT_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BABBLE_OUTPUT_ABI 1

/* Log levels for babble_host_v1.log. */
enum { BABBLE_LOG_DEBUG = 0, BABBLE_LOG_INFO = 1, BABBLE_LOG_WARN = 2, BABBLE_LOG_ERROR = 3 };

/* The runtime gives this function table to a plugin,
 * and you can call each function from any thread between create() and destroy(). */
typedef struct babble_host_v1 {
    uint32_t abi; /* The value is BABBLE_OUTPUT_ABI. */
    void *ctx;    /* Give this pointer back in each call. */

    /* Returns the number of signals, whose ids are 0 to count-1 and do not change during the run. */
    uint32_t (*signal_count)(void *ctx);
    /* Returns the name of a signal (valid until destroy()), or NULL for an invalid id. */
    const char *(*signal_name)(void *ctx, uint32_t id);
    /* Writes the values of n signals into values (and their CLOCK_MONOTONIC times into t_ns if it is not NULL)
     * and returns the update sequence number. */
    uint64_t (*read)(void *ctx, const uint32_t *ids, uint32_t n, float *values, uint64_t *t_ns);
    /* Waits until the sequence number is more than after_seq or timeout_ms passes, and returns the sequence number. */
    uint64_t (*wait_update)(void *ctx, uint64_t after_seq, uint32_t timeout_ms);
    /* Writes a line to the runtime log with the name of the plugin instance before it. */
    void (*log)(void *ctx, int level, const char *message);
    /* Returns a writable directory for the persistent files of this plugin. */
    const char *(*state_dir)(void *ctx);
} babble_host_v1;

/* A plugin gives this function table to the runtime, which calls all of the functions on its main thread. */
typedef struct babble_output_v1 {
    uint32_t abi;        /* The value is BABBLE_OUTPUT_ABI. */
    const char *name;    /* A short identifier, for example "udp_json". */
    const char *version; /* The runtime writes this free-form text to the log at load. */

    /* Makes an instance from its config key and "config" JSON text, or logs the cause and returns NULL on failure. */
    void *(*create)(const babble_host_v1 *host, const char *instance, const char *config_json);
    /* Starts the plugin threads that send values, and returns 0 on success. */
    int (*start)(void *self);
    /* Stops the sends and joins the plugin threads, and must return quickly because shutdown has about 2 s. */
    void (*stop)(void *self);
    /* Frees the instance. */
    void (*destroy)(void *self);
} babble_output_v1;

/* A plugin exports only this function, which the runtime calls one time after dlopen. */
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

#endif
