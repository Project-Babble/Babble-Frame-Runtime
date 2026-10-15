#pragma once

#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "model/postprocess/face_post.h"
#include "model/postprocess/one_euro.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace babble {

namespace face {
// Baballonia's mono face model outputs ("prod FT layout",
// Services/DefaultInferenceRunner.cs), also the signal names.
inline constexpr std::array<const char *, 45> kShapes = {
    "cheekPuffLeft",   "cheekPuffRight",   "cheekSuckLeft",     "cheekSuckRight",     "jawOpen",
    "jawForward",      "jawLeft",          "jawRight",          "noseSneerLeft",      "noseSneerRight",
    "mouthFunnel",     "mouthPucker",      "mouthLeft",         "mouthRight",         "mouthRollUpper",
    "mouthRollLower",  "mouthShrugUpper",  "mouthShrugLower",   "mouthClose",         "mouthSmileLeft",
    "mouthSmileRight", "mouthFrownLeft",   "mouthFrownRight",   "mouthDimpleLeft",    "mouthDimpleRight",
    "mouthUpperUpLeft", "mouthUpperUpRight", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft",
    "mouthPressRight", "mouthStretchLeft", "mouthStretchRight", "tongueOut",          "tongueUp",
    "tongueDown",      "tongueLeft",       "tongueRight",       "tongueRoll",         "tongueBendDown",
    "tongueCurlUp",    "tongueSquish",     "tongueFlat",        "tongueTwistLeft",    "tongueTwistRight",
};
// Baballonia's smoothing for this model (its OneEuroFilter defaults).
inline constexpr OneEuroParams kOneEuro{0.5, 3.0, 1.0};
} // namespace face

// Config type "babble_mono_face": Baballonia's single-camera face model on a
// Babble face tracker (source type "babble_usb"). One grayscale 224x224
// input, pixel/255, 45 blendshapes out, as Baballonia runs it
// (Services/Inference/Transformers/ImageTransformer.cs).
//
// Config: model, from, thread,
//   roi [x, y, w, h]        crop of the camera image before anything else (default: all of it)
//   gain (1.0)              Baballonia's "Gamma": a linear brightness gain, saturating
//   rotate (0|90|180|270)   counter-clockwise
//   flip ("none"|"h"|"v"|"hv")
//   one_euro {min_cutoff 0.5, beta 3, d_cutoff 1} or false
//   calibration {mode: "auto" (default)|"raw"|"manual", ranges: {"jawOpen": [min, max], ...}}
class BabbleMonoFace : public PipelineBase {
public:
    BabbleMonoFace(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infM") {}
    bool init() override;

    // Preprocess one camera image into the model input (exposed for tests).
    void prepare(const Frame &f, float *input);

private:
    void process(const StereoFrame &frames) override;
    std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now_ns) const override;
    void reset_session() override { reset_pending_ = true; }

    struct Roi {
        uint32_t x = 0, y = 0, w = 0, h = 0;
    };
    Roi roi_;
    float gain_ = 1.0f;
    int quarter_turns_ = 0;
    bool flip_h_ = false, flip_v_ = false;
    std::unique_ptr<ExecuTorchRunner> runner_;
    std::unique_ptr<OneEuro> filter_;
    std::unique_ptr<face::Calibration> calibration_;
    std::array<SignalId, face::kShapes.size()> ids_{};

    std::vector<uint8_t> tmp_a_, tmp_b_, small_;
    std::vector<float> input_, out_;
    std::vector<uint16_t> half_in_, half_out_;
    std::vector<float> values_;
    uint64_t last_capture_ns_ = 0;
    std::atomic<bool> reset_pending_{false};
};

} // namespace babble
