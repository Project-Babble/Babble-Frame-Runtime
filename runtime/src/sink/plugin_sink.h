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

// Config type "plugin" loads an output module from a shared library through the C ABI in babble/output_plugin.h.
class PluginSink : public ISink {
public:
    PluginSink(const nlohmann::json &cfg, SinkDeps deps);
    ~PluginSink() override;

    bool start() override;
    void stop() override;

    // The directory where the sink looks for a library that has only a name.
    static std::filesystem::path plugin_dir();

private:
    // The callbacks of the host table, where ctx is this.
    static uint32_t host_signal_count(void *ctx);
    static const char *host_signal_name(void *ctx, uint32_t id);
    static uint64_t host_read(void *ctx, const uint32_t *ids, uint32_t n, float *values, uint64_t *t_ns);
    static uint64_t host_wait_update(void *ctx, uint64_t after_seq, uint32_t timeout_ms);
    static void host_log(void *ctx, int level, const char *message);
    static const char *host_state_dir(void *ctx);

    void refresh_visible();   // The caller must hold visible_mutex_.

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
    size_t seen_count_ = 0;               // The value of ExpressionState::count() when the sink made visible_.
    std::vector<SignalId> visible_;       // The runtime signal id for each plugin id.
    std::deque<std::string> names_;       // A deque keeps each c_str() pointer valid for signal_name.
};

}
