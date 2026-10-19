#pragma once

#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "model/postprocess/mouth_post.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace babble {

// The "fusionnet_mouth" pipeline runs the FusionNet stereo mouth model with the processing of mouth_osc_pc.py.
class FusionNetMouth : public PipelineBase {
public:
    FusionNetMouth(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infM") {}
    bool init() override;

    // Convert one stereo pair into the two model inputs, which is public for the tests.
    void prepare(const StereoFrame &sf, float *view0, float *view1);

private:
    void process(const StereoFrame &frames) override;
    std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now_ns) const override;
    void reset_session() override { reset_pending_ = true; }

    bool swap_ = false, flip_l_ = false, flip_r_ = true, area_ = false;
    int quarter_turns_ = 0;
    std::unique_ptr<ExecuTorchRunner> runner_;
    std::unique_ptr<mouth::Post> post_;
    std::array<SignalId, mouth::kNumShapes> ids_{};

    std::vector<uint8_t> tmp_a_, tmp_b_, small_;
    std::vector<float> in0_, in1_;
    std::array<float, mouth::kNumChannels> out_{};
    std::atomic<bool> reset_pending_{false};
    uint64_t last_capture_ns_ = 0;   // This is the capture time of the previous frame, to calculate the filter dt.

    void probe(const StereoFrame &sf);
    int probe_left_ = 0, probe_total_ = 0;
    std::array<double, 8> probe_sum_{};
};

}
