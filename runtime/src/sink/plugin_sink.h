#pragma once

#include "sink/sink.h"
#include "state/expression_state.h"

#include <babble/output_plugin.h>

#include <filesystem>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace babble {

// Config type "plugin": an output module in a shared library, loaded with
// dlopen and driven through the C ABI in sdk/include/babble/output_plugin.h.
//
// Config:
//   library   a bare name ("babble_udp_json" -> <exe dir>/plugins/libbabble_udp_json.so)
//             or a path to the .so
//   config    object handed to the plugin as JSON text
//
// Plugins see the runtime's signals except internal ones (names starting with
// '_'), under their own ids: 0.. in registration order, stable for the run
// (pipelines only ever append signals). A library that can't be loaded, or
// speaks another ABI version, is logged and the module dropped.
class PluginSink : public ISink {
public:
    PluginSink(const nlohmann::json &cfg, SinkDeps deps);
    ~PluginSink() override;

    bool start() override;
    void stop() override;

    // Where a bare library name is looked up.
    static std::filesystem::path plugin_dir();

private:
    // Host table callbacks (ctx = this).
    static uint32_t host_signal_count(void *ctx);
    static const char *host_signal_name(void *ctx, uint32_t id);
    static uint64_t host_read(void *ctx, const uint32_t *ids, uint32_t n, float *values, uint64_t *t_ns);
    static uint64_t host_wait_update(void *ctx, uint64_t after_seq, uint32_t timeout_ms);
    static void host_log(void *ctx, int level, const char *message);
    static const char *host_state_dir(void *ctx);

    void refresh_visible();   // caller holds visible_mutex_

    std::string name_;
    nlohmann::json cfg_;
    const ExpressionState &state_;
    std::filesystem::path library_;
    std::string state_dir_;

    void *handle_ = nullptr;
    const babble_output_v1 *api_ = nullptr;
    void *instance_ = nullptr;
    bool started_ = false;
    babble_host_v1 host_{};

    std::mutex visible_mutex_;
    size_t seen_count_ = 0;               // ExpressionState::count() when visible_ was built
    std::vector<SignalId> visible_;       // plugin id -> runtime signal id
    std::deque<std::string> names_;       // plugin id -> name (deque: stable c_str() for signal_name)
};

} // namespace babble
