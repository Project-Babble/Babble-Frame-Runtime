#include "model/pipeline.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/null_model_runner.h"
#include "model/preprocess.h"
#include "sink/debug_sink.h"

#include <chrono>

namespace babble {

Pipeline::Pipeline(std::string name, const nlohmann::json &cfg, ThreadSpec thread, LatestSlot<StereoFrame> &input,
                   ExpressionState &state, StatsRegistry &stats, DebugSink *debug)
    : name_(std::move(name)), cfg_(cfg), thread_spec_(std::move(thread)), input_(input), state_(state),
      stats_(stats), debug_(debug) {}

Pipeline::~Pipeline() {
    stop();
    stats_.remove_prefix("pipe." + name_ + ".");
}

bool Pipeline::init() {
    const auto &inputs = cfg_.value("inputs", nlohmann::json::array());
    const auto &outputs = cfg_.value("outputs", nlohmann::json::array());
    if (inputs.size() != 1) {
        // Multi-input (merged) models need their own preprocessing; not in M1.
        LOG_ERROR("pipeline %s: expected exactly one input, got %zu", name_.c_str(), inputs.size());
        return false;
    }
    const auto &in = inputs[0];
    if (in.value("layout", "") != "sbs_gray_u8") {
        LOG_ERROR("pipeline %s: unsupported input layout '%s'", name_.c_str(), in.value("layout", "").c_str());
        return false;
    }
    auto size = in.value("size", nlohmann::json::array());
    if (size.size() != 2) {
        LOG_ERROR("pipeline %s: input size must be [height, width]", name_.c_str());
        return false;
    }
    in_h_ = size[0].get<uint32_t>();
    in_w_ = size[1].get<uint32_t>();

    std::vector<TensorSpec> in_specs{{in.value("name", "input"), {1, 1, in_h_, in_w_}, DType::U8}};
    std::vector<TensorSpec> out_specs;
    for (const auto &o : outputs) {
        std::vector<SignalId> ids;
        for (const auto &s : o.value("signals", nlohmann::json::array()))
            ids.push_back(state_.add_signal(s.get<std::string>()));
        out_specs.push_back({o.value("name", "output"), {1, int64_t(ids.size())}, DType::F32});
        out_signals_.push_back(std::move(ids));
    }

    std::string runner = cfg_.value("runner", "null");
    if (runner != "null") {
        LOG_WARN("pipeline %s: runner '%s' not available yet, using null", name_.c_str(), runner.c_str());
    }
    runner_ = std::make_unique<NullModelRunner>(in_specs, out_specs);
    if (!runner_->load(cfg_.value("model", ""))) {
        LOG_ERROR("pipeline %s: model failed to load", name_.c_str());
        return false;
    }

    // Validate that the runner's declared tensors match what we will feed and read.
    auto ri = runner_->inputs();
    auto ro = runner_->outputs();
    if (ri.size() != 1 || ri[0].dtype != DType::U8 || ri[0].elements() != size_t(in_h_) * in_w_) {
        LOG_ERROR("pipeline %s: model input does not match %ux%u u8", name_.c_str(), in_h_, in_w_);
        return false;
    }
    if (ro.size() != out_signals_.size()) {
        LOG_ERROR("pipeline %s: model has %zu outputs, config maps %zu", name_.c_str(), ro.size(), out_signals_.size());
        return false;
    }
    for (size_t i = 0; i < ro.size(); i++) {
        if (ro[i].dtype != DType::F32 || ro[i].elements() != out_signals_[i].size()) {
            LOG_ERROR("pipeline %s: output %zu has %zu values, config names %zu signals", name_.c_str(), i,
                      ro[i].elements(), out_signals_[i].size());
            return false;
        }
    }
    for (const auto &s : ri)
        in_buffers_.emplace_back(s.bytes());
    for (const auto &s : ro)
        out_buffers_.emplace_back(s.bytes());

    stats_.add("pipe." + name_ + ".fps", &rate_);
    stats_.add("pipe." + name_ + ".preprocess", &preprocess_);
    stats_.add("pipe." + name_ + ".inference", &inference_);
    stats_.add("pipe." + name_ + ".capture_to_state", &capture_to_state_);
    LOG_INFO("pipeline %s: %s runner, input %ux%u, %zu output signals", name_.c_str(), runner.c_str(), in_h_, in_w_,
             out_signals_.empty() ? size_t(0) : out_signals_[0].size());
    return true;
}

void Pipeline::start() {
    stop_ = false;
    input_.reopen();
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void Pipeline::stop() {
    stop_ = true;
    input_.close();
    if (thread_.joinable())
        thread_.join();
}

void Pipeline::run() {
    auto ri = runner_->inputs();
    auto ro = runner_->outputs();
    std::vector<TensorView> in_views, out_views;
    for (size_t i = 0; i < ri.size(); i++)
        in_views.push_back({in_buffers_[i].data(), &ri[i]});
    for (size_t i = 0; i < ro.size(); i++)
        out_views.push_back({out_buffers_[i].data(), &ro[i]});
    std::vector<std::pair<SignalId, Signal>> updates;

    while (!stop_) {
        auto sf = input_.wait_take(std::chrono::milliseconds(200));
        if (!sf)
            continue;
        if (debug_)
            debug_->offer(name_.c_str(), *sf);

        uint64_t t0 = mono_ns();
        preprocess_sbs_gray_u8(*sf, in_buffers_[0].data(), in_h_, in_w_);
        uint64_t t1 = mono_ns();
        if (!runner_->run(in_views, out_views)) {
            LOG_WARN("pipeline %s: inference failed", name_.c_str());
            continue;
        }
        uint64_t t2 = mono_ns();

        updates.clear();
        for (size_t o = 0; o < out_signals_.size(); o++) {
            const float *vals = reinterpret_cast<const float *>(out_buffers_[o].data());
            for (size_t k = 0; k < out_signals_[o].size(); k++)
                updates.emplace_back(out_signals_[o][k], Signal{vals[k], 1.0f, sf->capture_ns});
        }
        state_.update(updates);
        uint64_t t3 = mono_ns();

        preprocess_.add(t1 - t0);
        inference_.add(t2 - t1);
        capture_to_state_.add(t3 - sf->capture_ns);
        rate_.tick();
    }
}

} // namespace babble
