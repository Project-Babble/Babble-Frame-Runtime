#pragma once

#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "model/postprocess/mouth_post.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace babble {

// Config type "fusionnet_mouth": the FusionNet stereo mouth model, with the
// exact preprocessing and post-processing of mouth_osc_pc.py.
//
// Views: view 0 = left camera (face_l, /dev/video9), view 1 = right camera
// mirrored; order swap -> flip -> rotate (rotation after the mirror), then
// resize to 224 (INTER_LINEAR) -> equalizeHist -> /255. Signals: the 43
// blendshapes in camelCase (the Baballonia module's addresses). A model's
// 44th UserPresent output, if present, is ignored.
//
// Config: model, from, thread, swap (bool), flip ("r"|"l"|"lr"|"none"),
// rotate (0|90|180|270, counter-clockwise), interp ("linear"|"area"),
// one_euro ({min_cutoff 0.5, beta 3, d_cutoff 1} or false: the mouth's own
// smoothing, tuned apart from the eyes'). Presence comes from the headset
// (SteamVR proximity): the cameras pause while it's off and each new session
// resets the filters.
// calibration: {mode: "auto" (default)|"manual"|"raw", file: "calibration_mouth.json"},
// probe_orientation (N > 0: for the first N frames also run all 8 rotate x
// swap combinations and log them ranked by mean UserPresent, like
// mouth_osc_pc.py --probe-orientation; tracking output is unaffected).
class FusionNetMouth : public PipelineBase {
public:
    FusionNetMouth(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infM") {}
    bool init() override;

    // Preprocess one stereo pair into the two model inputs (exposed for tests).
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

    std::vector<uint8_t> tmp_a_, tmp_b_, small_, eq_;
    std::vector<float> in0_, in1_;
    std::array<float, mouth::kNumChannels> out_{};
    std::atomic<bool> reset_pending_{false};
    uint64_t last_capture_ns_ = 0;   // the previous frame's, for the filter's dt

    void probe(const StereoFrame &sf);
    int probe_left_ = 0, probe_total_ = 0;
    std::array<double, 8> probe_sum_{};
};

} // namespace babble
