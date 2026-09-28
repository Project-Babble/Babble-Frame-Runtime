#include "sink/plugin_sink.h"

#include "core/log.h"
#include "core/paths.h"
#include "state/expression_state.h"

#include <chrono>

#include <dlfcn.h>

namespace {
const babble::log::Logger LOG("plugin");
} // namespace

namespace babble {

std::filesystem::path PluginSink::plugin_dir() { return paths::exe_path().parent_path() / "plugins"; }

PluginSink::PluginSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), cfg_(cfg), state_(deps.state) {
    const std::string lib = cfg.value("library", "");
    library_ = lib.find('/') == std::string::npos && !lib.ends_with(".so") ? plugin_dir() / ("lib" + lib + ".so")
                                                                            : std::filesystem::path(lib);
    std::error_code ec;
    const auto dir = paths::state_dir() / "plugins" / name_;
    std::filesystem::create_directories(dir, ec);
    state_dir_ = dir.string();

    host_.abi = BABBLE_OUTPUT_ABI;
    host_.ctx = this;
    host_.signal_count = host_signal_count;
    host_.signal_name = host_signal_name;
    host_.read = host_read;
    host_.wait_update = host_wait_update;
    host_.log = host_log;
    host_.state_dir = host_state_dir;
}

PluginSink::~PluginSink() {
    stop();
    if (handle_)
        dlclose(handle_);
}

bool PluginSink::start() {
    if (cfg_.value("library", "").empty()) {
        LOG.error("sink %s: plugin needs a \"library\"", name_.c_str());
        return false;
    }
    handle_ = dlopen(library_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        LOG.error("sink %s: cannot load %s: %s", name_.c_str(), library_.c_str(), dlerror());
        return false;
    }
    auto entry = reinterpret_cast<babble_output_plugin_v1_fn>(dlsym(handle_, BABBLE_OUTPUT_PLUGIN_ENTRY));
    api_ = entry ? entry() : nullptr;
    if (!api_ || api_->abi != BABBLE_OUTPUT_ABI || !api_->create || !api_->start || !api_->stop || !api_->destroy) {
        LOG.error("sink %s: %s is not an ABI %d output plugin", name_.c_str(), library_.c_str(), BABBLE_OUTPUT_ABI);
        api_ = nullptr;
        return false;
    }
    const std::string config = cfg_.value("config", nlohmann::json::object()).dump();
    instance_ = api_->create(&host_, name_.c_str(), config.c_str());
    if (!instance_) {
        LOG.error("sink %s: plugin %s refused its config", name_.c_str(), api_->name ? api_->name : "?");
        return false;
    }
    if (api_->start(instance_) != 0) {
        LOG.error("sink %s: plugin %s failed to start", name_.c_str(), api_->name ? api_->name : "?");
        api_->destroy(instance_);
        instance_ = nullptr;
        return false;
    }
    started_ = true;
    LOG.info("sink %s: plugin %s %s loaded from %s", name_.c_str(), api_->name ? api_->name : "?",
             api_->version ? api_->version : "", library_.c_str());
    return true;
}

void PluginSink::stop() {
    if (!instance_)
        return;
    if (started_)
        api_->stop(instance_);
    started_ = false;
    api_->destroy(instance_);
    instance_ = nullptr;
}

void PluginSink::refresh_visible() {
    const size_t n = state_.count();
    for (size_t id = seen_count_; id < n; id++) {
        std::string nm = state_.name(SignalId(id));
        if (nm.empty() || nm[0] == '_')   // internal signals stay inside the runtime
            continue;
        visible_.push_back(SignalId(id));
        names_.push_back(std::move(nm));
    }
    seen_count_ = n;
}

uint32_t PluginSink::host_signal_count(void *ctx) {
    auto *self = static_cast<PluginSink *>(ctx);
    std::lock_guard<std::mutex> lock(self->visible_mutex_);
    self->refresh_visible();
    return uint32_t(self->visible_.size());
}

const char *PluginSink::host_signal_name(void *ctx, uint32_t id) {
    auto *self = static_cast<PluginSink *>(ctx);
    std::lock_guard<std::mutex> lock(self->visible_mutex_);
    self->refresh_visible();
    return id < self->names_.size() ? self->names_[id].c_str() : nullptr;
}

uint64_t PluginSink::host_read(void *ctx, const uint32_t *ids, uint32_t n, float *values, uint64_t *t_ns) {
    auto *self = static_cast<PluginSink *>(ctx);
    std::vector<SignalId> mapped(n);
    {
        std::lock_guard<std::mutex> lock(self->visible_mutex_);
        self->refresh_visible();
        for (uint32_t i = 0; i < n; i++)   // unknown ids map past the end and read as 0
            mapped[i] = ids[i] < self->visible_.size() ? self->visible_[ids[i]] : SignalId(~0u);
    }
    std::vector<Signal> out(n);
    const uint64_t seq = self->state_.read(mapped, out.data());
    for (uint32_t i = 0; i < n; i++) {
        values[i] = out[i].value;
        if (t_ns)
            t_ns[i] = out[i].t_ns;
    }
    return seq;
}

uint64_t PluginSink::host_wait_update(void *ctx, uint64_t after_seq, uint32_t timeout_ms) {
    auto *self = static_cast<PluginSink *>(ctx);
    return self->state_.wait_update(after_seq, std::chrono::milliseconds(timeout_ms));
}

void PluginSink::host_log(void *ctx, int level, const char *message) {
    auto *self = static_cast<PluginSink *>(ctx);
    const char *m = message ? message : "";
    switch (level) {
    case BABBLE_LOG_DEBUG: LOG.debug("%s: %s", self->name_.c_str(), m); break;
    case BABBLE_LOG_WARN: LOG.warn("%s: %s", self->name_.c_str(), m); break;
    case BABBLE_LOG_ERROR: LOG.error("%s: %s", self->name_.c_str(), m); break;
    default: LOG.info("%s: %s", self->name_.c_str(), m); break;
    }
}

const char *PluginSink::host_state_dir(void *ctx) { return static_cast<PluginSink *>(ctx)->state_dir_.c_str(); }

} // namespace babble
