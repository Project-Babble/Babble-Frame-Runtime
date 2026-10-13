#include "model/pipeline.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/babble_eye.h"
#include "model/babble_mono_face.h"
#include "model/null_pipeline.h"
#include "model/steamvr_gaze.h"
#include "sink/sink.h"
#if BABBLE_FRUNK_MOUTH
#include "model/fusionnet_mouth.h"
#endif

#include <chrono>

namespace {
const babble::log::Logger LOG("pipeline");
} // namespace

namespace babble {

const std::string &IPipeline::source() const {
    static const std::string none;
    return none;
}

std::unique_ptr<IPipeline> make_pipeline(const nlohmann::json &cfg, PipelineDeps deps) {
    const std::string type = cfg.value("type", "null");
#if BABBLE_FRUNK_MOUTH
    if (type == "fusionnet_mouth")
        return std::make_unique<FusionNetMouth>(cfg, std::move(deps));
#endif
    if (type == "babble_mono_face")
        return std::make_unique<BabbleMonoFace>(cfg, std::move(deps));
    if (type == "babble_eye")
        return std::make_unique<BabbleEye>(cfg, std::move(deps));
    if (type == "steamvr_gaze")
        return std::make_unique<SteamVrGaze>(cfg, std::move(deps));
    if (type == "null")
        return std::make_unique<NullPipeline>(cfg, std::move(deps));
    LOG.error("pipeline %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

PipelineBase::PipelineBase(const nlohmann::json &cfg, PipelineDeps deps, const char *default_thread_key)
    : name_(deps.name), cfg_(cfg), deps_(std::move(deps)), feed_(cfg.value("feed", "")) {
    std::string key = cfg_.value("thread", default_thread_key);
    thread_spec_ = thread_spec(deps_.threads, key, ("bbl-" + key).c_str());
}

PipelineBase::~PipelineBase() {
    stop();
    if (stats_registered_)
        deps_.stats.remove_prefix("pipe." + name_ + ".");
}

bool PipelineBase::bind_input() {
    source_ = cfg_.value("from", "");
    if (source_.empty() && cfg_.contains("inputs") && cfg_["inputs"].is_array() && !cfg_["inputs"].empty())
        source_ = cfg_["inputs"][0].value("from", "");
    input_ = deps_.slot ? deps_.slot(source_) : nullptr;
    if (!input_) {
        LOG.error("pipeline %s: input source '%s' is not configured", name_.c_str(), source_.c_str());
        return false;
    }
    const std::string p = "pipe." + name_ + ".";
    deps_.stats.add(p + "fps", &rate_);
    deps_.stats.add(p + "preprocess", &preprocess_);
    deps_.stats.add(p + "inference", &inference_);
    deps_.stats.add(p + "capture_to_state", &capture_to_state_);
    stats_registered_ = true;
    return true;
}

std::filesystem::path PipelineBase::model_path(const std::string &key) const {
    std::filesystem::path p = cfg_.value(key, "");
    return p.is_absolute() ? p : deps_.models_dir / p;
}

void PipelineBase::hold(Hold reason, bool on) {
    std::lock_guard lock(publish_mutex_);
    const uint8_t before = held_;
    held_ = on ? uint8_t(held_ | reason) : uint8_t(held_ & ~reason);
    if (on && !(before & reason)) {
        const auto values = neutral(mono_ns());
        if (!values.empty())
            deps_.state.update(values);
        reset_session();
    }
}

void PipelineBase::on_presence(bool worn) { hold(kHeadsetOff, !worn); }

void PipelineBase::set_tracking(bool on) { hold(kFeedOff, !on); }

void PipelineBase::publish(std::span<const std::pair<SignalId, Signal>> updates) {
    std::lock_guard lock(publish_mutex_);
    if (!held_)
        deps_.state.update(updates);
}

void PipelineBase::start() {
    stop_ = false;
    input_->reopen();
    on_start();
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void PipelineBase::stop() {
    stop_ = true;
    if (input_)
        input_->close();
    if (thread_.joinable())
        thread_.join();
    on_stop();
}

void PipelineBase::run() {
    while (!stop_) {
        auto sf = input_->wait_take(std::chrono::milliseconds(200));
        if (!sf)
            continue;
        for (ISink *tap : deps_.frame_taps)
            tap->offer_frames(source_, *sf);
        process(*sf);
        capture_to_state_.add(mono_ns() - sf->capture_ns);
        rate_.tick();
    }
}

} // namespace babble
