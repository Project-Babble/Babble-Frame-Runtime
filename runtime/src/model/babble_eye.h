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

// Config type "babble_eye": eyelid openness and pupil dilation per eye, with
// the processing of eye_osc_pc.py. Two model layouts:
//   - combined ("model"): one network, [2,1,R,R] -> [2,2] = per eye
//     (lid openness, pupil size), every frame. fp16 or fp32.
//   - split ("lid_model" + optional "pupil_model"): [2,1,R,R] -> [2] each;
//     lid every frame, pupil at pupil_hz on its own thread (it's slow).
// Pupil size is area / 3000 px of the 400 px crop in both layouts.
//
// Input: the eye source's stereo pair; left = view 2 (eye_l = the wearer's
// RIGHT eye), right = view 3. View 3 is flipped vertically (the reference's
// rot90(2) + mirror), both resized to the model's R (INTER_AREA) and
// normalized to (x/255 - 0.5)/0.25, batched as [2,1,R,R].
//
// Signals follow the Baballonia module: LeftEyeLid, LeftEyeWiden,
// LeftEyeDilation and the Right twins, with the module's sides swapped
// relative to the cameras like the reference (babble_swap flips that).
//
// Config: from, thread, model (combined; wins over the split keys),
// lid_model, pupil_model (empty = no dilation),
// pupil_thread, update_hz (90: the rate the per-update constants below are
// tuned at, as in the reference; the pipeline measures its real update rate
// and converts them, see eye::RateScale),
// pupil_hz (3; split layout only), pupil_gain (2), lid_alpha (0.6), calib_window_s (20),
// calib_warmup_s (3), calib (true), pupil_gate (0.4), pupil_gate_hys (0.1),
// pupil_hold_s (0.1), pupil_alpha (0.2), pupil_rate (0.6), pupil_base_s (60),
// babble_swap (false).
class BabbleEye : public PipelineBase {
public:
    BabbleEye(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infE") {}
    ~BabbleEye() override;
    bool init() override;
    void on_presence(bool worn) override;

    // Preprocess one stereo pair into the [2,1,R,R] float input.
    void prepare(const StereoFrame &sf, float *x);

private:
    void process(const StereoFrame &frames) override;
    void on_start() override;
    void on_stop() override;
    void pupil_loop();
    // Runs a one-input, one-output eye model on float data, converting to and
    // from fp16 when the model uses it.
    bool run_model(ExecuTorchRunner &m, const float *x, float *y, std::vector<uint16_t> &half_in,
                   std::vector<uint16_t> &half_out);

    std::unique_ptr<ExecuTorchRunner> lid_, pupil_;   // lid_ is the combined model in that layout
    bool combined_ = false;
    uint32_t res_ = 160;
    size_t plane_ = 160 * 160;
    std::vector<float> x_;
    std::array<float, 4> out_{};   // lid: [2] (split) or [2,2] (combined)
    std::vector<uint16_t> half_in_, half_out_;
    std::vector<uint8_t> flip_, small_;

    // Post-processing state, index 0 = view 2 ("L"), 1 = view 3 ("R").
    std::vector<eye::LidCalibrator> calib_;
    std::vector<eye::PupilTracker> trackers_;
    std::array<std::optional<double>, 2> smoothed_{};
    void reset_session();
    // Update rate: measured from frame times; the filters are tuned for tuned_.hz.
    void retune(double hz);
    void set_pupil_windows();
    eye::UpdateRate update_rate_{90.0};
    eye::RateScale tuned_;
    uint64_t last_retune_ns_ = 0;
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
