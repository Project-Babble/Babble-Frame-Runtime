#include "model/null_pipeline.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/null_model_runner.h"
#include "model/preprocess.h"

namespace {
const babble::log::Logger LOG("pipeline");
}

namespace babble {

bool NullPipeline::init() {
    if (!bind_input())
        return false;
    const auto &inputs = cfg_.value("inputs", nlohmann::json::array());
    const auto size = inputs.empty() ? nlohmann::json::array() : inputs[0].value("size", nlohmann::json::array());
    in_h_ = size.size() == 2 ? size[0].get<uint32_t>() : 240;
    in_w_ = size.size() == 2 ? size[1].get<uint32_t>() : 480;

    std::vector<TensorSpec> in_specs{{"input", {1, 1, in_h_, in_w_}, DType::U8}};
    std::vector<TensorSpec> out_specs;
    for (const auto &o : cfg_.value("outputs", nlohmann::json::array())) {
        std::vector<SignalId> ids;
        for (const auto &s : o.value("signals", nlohmann::json::array()))
            ids.push_back(deps_.state.add_signal(s.get<std::string>()));
        out_specs.push_back({o.value("name", "output"), {1, int64_t(ids.size())}, DType::F32});
        out_signals_.push_back(std::move(ids));
    }
    runner_ = std::make_unique<NullModelRunner>(in_specs, out_specs);
    for (const auto &s : runner_->inputs())
        in_buffers_.emplace_back(s.bytes());
    for (const auto &s : runner_->outputs())
        out_buffers_.emplace_back(s.bytes());
    LOG.info("pipeline %s: null runner on source %s", name_.c_str(), source_.c_str());
    return true;
}

void NullPipeline::process(const StereoFrame &sf) {
    auto ri = runner_->inputs();
    auto ro = runner_->outputs();
    std::vector<TensorView> iv, ov;
    for (size_t i = 0; i < ri.size(); i++)
        iv.push_back({in_buffers_[i].data(), &ri[i]});
    for (size_t i = 0; i < ro.size(); i++)
        ov.push_back({out_buffers_[i].data(), &ro[i]});

    uint64_t t0 = mono_ns();
    preprocess_sbs_gray_u8(sf, in_buffers_[0].data(), in_h_, in_w_);
    uint64_t t1 = mono_ns();
    if (!runner_->run(iv, ov))
        return;
    inference_.add(mono_ns() - t1);
    preprocess_.add(t1 - t0);

    std::vector<std::pair<SignalId, Signal>> updates;
    for (size_t o = 0; o < out_signals_.size(); o++) {
        const float *vals = reinterpret_cast<const float *>(out_buffers_[o].data());
        for (size_t k = 0; k < out_signals_[o].size(); k++)
            updates.emplace_back(out_signals_[o][k], Signal{vals[k], 1.0f, sf.capture_ns});
    }
    publish(updates);
}

}
