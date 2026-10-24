#pragma once

#include "model/eye_kit.h"
#include "model/pipeline.h"
#include "model/postprocess/eye_post.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace babble {

// The "babble_eye" pipeline gets eyelid openness, Widen and pupil dilation for each eye from the stereo eye cameras.
// It runs the two-stage EyeKit: "model" is stage 1 (lid, pupil centre and Widen) and "pupil_model" is stage 2 (pupil
// diameter in mm from a crop at that centre).
class BabbleEye : public PipelineBase {
public:
    BabbleEye(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infE") {}
    ~BabbleEye() override;
    bool init() override;
    void set_tracking(bool on) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;

private:
    void process(const StereoFrame &frames) override;
    void on_start() override;
    void on_stop() override;
    // Get the two views in the canonical orientation, with the "R" pane flipped into flip_.
    std::array<img::View, 2> canonical(const StereoFrame &sf);

    std::unique_ptr<EyeKit> kit_;
    double widen_gain_ = 0.8;   // Widen is the eye kit Widen head / this gain, if the kit has the head and it is > 0.
    LatencyStat pupil_preprocess_, pupil_inference_;   // These are the stage 2 parts of the eye kit.
    std::array<SignalId, 2> mm_ids_{};                 // These debug signals are the shown pupil diameter in mm.
    std::vector<uint8_t> flip_;

    // Index 0 is view 2 ("L") and index 1 is view 3 ("R").
    std::vector<eye::LidCalibrator> calib_;           // These are the calibrators for lid_calibration "median".
    std::vector<eye::OpennessCalibrator> openness_;   // These are the calibrators for lid_calibration "baseline".
    bool baseline_calib_ = true;
    eye::OpennessCalibrator::Params openness_params() const;
    std::vector<eye::PupilTracker> trackers_;
    std::optional<OneEuroParams> lid_filter_, pupil_filter_;
    std::array<std::optional<OneEuro>, 2> lid_euro_{};         // These are the lid filters for lid_calibration "median".
    uint64_t last_capture_ns_ = 0;                             // This is the capture time of the previous frame, to calculate dt.
    void start_session();   // Reset the calibration, filters and pupil trackers on the pipeline thread.
    std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now_ns) const override;
    void reset_session() override { reset_pending_ = true; }
    // The pipeline measures the update rate from frame times and tunes the filters for tuned_.hz.
    void retune(double hz);
    void set_pupil_windows();
    eye::UpdateRate update_rate_{90.0};
    eye::RateScale tuned_;
    uint64_t last_retune_ns_ = 0;
    std::atomic<bool> reset_pending_{false};
    std::atomic<bool> native_openness_{false};   // This is true if openness_source is "native".
    std::atomic<bool> tracking_on_{true};
    // These are "combine_widen" and "combine_dilation": both eyes then get the mean of the two eyes.
    std::atomic<bool> combine_widen_{false}, combine_dilation_{false};
    void update_eye_server_want();
    Counter native_fallback_;
    bool use_calib_ = true, babble_swap_ = false;
    double pupil_gain_ = 2.0;

    std::array<SignalId, 3> left_ids_{}, right_ids_{};   // The order is Lid, Widen, Dilation.
    SignalId left_raw_id_ = 0, right_raw_id_ = 0;        // These debug signals are not sent.
    std::array<SignalId, 6> cal_ids_{};   // These debug signals show the neutral, wide and closed values of each eye.
};

}
