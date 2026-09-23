#pragma once

#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "model/postprocess/eye_post.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace babble {

// Config type "babble_eye": eyelid openness every eye frame plus pupil
// dilation at a low rate, with the exact processing of eye_osc_pc.py.
//
// Input: the eye source's stereo pair; left = view 2 (eye_l = the wearer's
// RIGHT eye), right = view 3. View 3 is flipped vertically (the reference's
// rot90(2) + mirror), both resized to 160 (INTER_AREA) and normalized to
// (x/255 - 0.5)/0.25, batched as [2,1,160,160] for both models.
//
// Signals follow the Baballonia module: LeftEyeLid, LeftEyeWiden,
// LeftEyeDilation and the Right twins, with the module's sides swapped
// relative to the cameras like the reference (babble_swap flips that).
//
// Config: from, thread, lid_model, pupil_model (empty = no dilation),
// pupil_thread, update_hz (90: the rate every frame-count constant assumes),
// pupil_hz (3), pupil_gain (2), lid_alpha (0.6), calib_window_s (20),
// calib_warmup_s (3), calib (true), pupil_gate (0.4), pupil_gate_hys (0.1),
// pupil_hold_s (0.1), pupil_alpha (0.2), pupil_rate (0.6), pupil_base_s (60),
// babble_swap (false).
class BabbleEye : public PipelineBase {
public:
    BabbleEye(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infE") {}
    ~BabbleEye() override;
    bool init() override;
    void on_presence(bool worn) override;

    // Preprocess one stereo pair into the [2,1,160,160] input (exposed for tests).
    void prepare(const StereoFrame &sf, float *x);

private:
    void process(const StereoFrame &frames) override;
    void on_start() override;
    void on_stop() override;
    void pupil_loop();

    std::unique_ptr<ExecuTorchRunner> lid_, pupil_;
    std::vector<float> x_;
    std::array<float, 2> lid_out_{};
    std::vector<uint8_t> flip_, small_;

    // Post-processing state, index 0 = view 2 ("L"), 1 = view 3 ("R").
    std::vector<eye::LidCalibrator> calib_;
    std::vector<eye::PupilTracker> trackers_;
    std::array<std::optional<double>, 2> smoothed_{};
    void reset_session();
    std::atomic<bool> reset_pending_{false};
    bool use_calib_ = true, babble_swap_ = false;
    double lid_alpha_ = 0.6, pupil_gain_ = 2.0;
    uint64_t pupil_every_ns_ = 0, last_pupil_submit_ns_ = 0;
    bool pupil_submitted_ = false;

    // Pupil worker: latest submitted input, one result per submission.
    ThreadSpec pupil_thread_spec_;
    std::thread pupil_thread_;
    std::mutex pupil_mutex_;
    std::condition_variable pupil_cv_;
    std::vector<float> pupil_in_;
    std::array<float, 2> pupil_out_{};
    uint64_t pupil_seq_in_ = 0, pupil_seq_out_ = 0, pupil_seq_taken_ = 0;
    bool pupil_busy_ = false, pupil_stop_ = false;

    std::array<SignalId, 3> left_ids_{}, right_ids_{};   // Lid, Widen, Dilation
    LatencyStat pupil_time_;
    RateStat pupil_rate_;
};

} // namespace babble
