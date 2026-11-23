#pragma once

#include "model/executorch_runner.h"
#include "model/image_ops.h"

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <vector>

namespace babble {

// The two-stage eye model. Stage 1 reads each whole eye for the lid openness and the pupil centre. Stage 2 reads a
// full-resolution crop around that centre for the pupil diameter in mm, corrected for gaze.
//   stage 1: [2,1,R,R] -> [2,3] = lid, cx, cy for each view, [2,4] with a Widen head as the fourth value, or [2,5]
//            with an EyeSquint head (the lower-lid and cheek raise, 0..1) as the fifth value. The input
//            is each view after img::resize_area_linear, and the centre is in 0..1 of the view, in edge coordinates
//            (pixel i spans [i, i+1) / width).
//            A temporal stage 1 has a second input, the history [2,L*(D+2)], and gives [2,5+D]: the five values, then
//            the frame's embedding e (D values). For each lag in `lags` (seconds), the history holds the stored e
//            nearest to that lag ago, its age / 0.1 s and 1, if the age is 0.5..1.5 x the lag. Else it holds zeros.
//   stage 2: [2,1,C,C] and [2,2] -> [2,1]. The first input is img::get_rect_sub_pix(crop_px x crop_px) at the centre,
//            then INTER_AREA to C. The second is the centre as ((x - W/2) / (W/4), (y - H/2) / (H/4)) in index
//            coordinates. The diameter is kMmOffset + kMmScale * output.
// Every image input is (px / 255 - 0.5) / 0.25. The views must be canonical, with the "R" pane flipped vertically.
// The models and the training contract are in the SAM3-Frame repo (EyeExpr/code/kit.py, out/eyeexpr/field/eye_kit).
class EyeKit {
public:
    static constexpr double kMmOffset = 2.0, kMmScale = 5.0;

    struct Result {
        std::array<double, 2> lid{}, x{}, y{}, mm{};   // x and y are the pupil centre in view pixels (index coordinates).
        std::array<double, 2> widen{};                 // This is the raw Widen head output, or 0 without the head.
        std::array<double, 2> squint{};                // This is the raw EyeSquint head output, or 0 without the head.
    };
    // These are nanoseconds for each part of the last run.
    struct Timing {
        uint64_t prep1 = 0, infer1 = 0, prep2 = 0, infer2 = 0;
    };

    // Load both models and check their contracts. crop_px is the side of the stage 2 crop in view pixels, or 0 for
    // twice the stage 2 input. lags are the history lags in seconds of a temporal stage 1.
    bool load(const std::filesystem::path &stage1, const std::filesystem::path &stage2, uint32_t crop_px = 0,
              std::vector<double> lags = {0.1, 0.25});
    // Run both stages on two canonical views of the same size. t_ns is the capture time, for the history.
    bool run(const std::array<img::View, 2> &views, Result &r, uint64_t t_ns, Timing *t = nullptr);
    // Forget the history of a temporal stage 1, for example when the wearer changes.
    void reset_history();

    uint32_t res() const { return res_; }
    bool has_widen() const { return per_view_ >= 4; }
    bool has_squint() const { return per_view_ == 5; }
    uint32_t crop_res() const { return crop_res_; }
    uint32_t crop_px() const { return crop_px_; }
    bool has_history() const { return hist_dim_ > 0; }
    bool fp16() const;

private:
    std::unique_ptr<ExecuTorchRunner> s1_, s2_;
    uint32_t res_ = 96, crop_res_ = 64, crop_px_ = 128;
    size_t per_view_ = 3;   // The stage 1 head outputs for each view, without the embedding.
    size_t emb_ = 0, hist_dim_ = 0;   // These are D and L*(D+2) of a temporal stage 1, else 0.
    std::vector<double> lags_;
    struct Past {
        uint64_t t_ns;
        std::vector<float> e;
    };
    std::array<std::deque<Past>, 2> past_;   // This is the recent embeddings of each view, oldest first.
    void make_history(uint64_t t_ns);
    std::vector<float> x1_, x2_, pos_, hist_, out1_;
    std::array<float, 2> out2_{};
    std::vector<uint8_t> small_, mid_, crop_, crop_small_;
    std::vector<std::vector<uint16_t>> half_in_;
    std::vector<uint16_t> half_out_;
};

}
