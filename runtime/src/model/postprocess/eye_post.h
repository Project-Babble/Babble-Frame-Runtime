#pragma once

// Post-processing for the lid and pupil eye models, ported from eye_osc_pc.py
// (the PC reference implementation), with One Euro filters in place of its
// EMAs. Arithmetic is double, matching the Python floats there. All
// frame-count constants are in updates; the reference runs at 90 updates/s,
// so the runtime runs the eye stream at 90.

#include "model/postprocess/one_euro.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace babble::eye {

// The eyes' smoothing defaults, tuned apart from the mouth's on a recorded
// session (eye routine, 72 Hz) against the EMAs they replace:
//  - openness (Lid, and Widen from it; a ratio, 1 = neutral open): ~25% less
//    jitter above 4 Hz at rest, closes and opens within a frame of the EMA,
//    blinks keep their full depth;
//  - the pupil diameter (px, noisy: ~7 px spread): ~45% less jitter above
//    4 Hz; a size change shows in ~125 ms (pupils take longer to change).
inline constexpr OneEuroParams kLidOneEuro{1.0, 5.0, 3.0};
inline constexpr OneEuroParams kPupilOneEuro{0.5, 0.01, 1.0};

// Median of a sample set, as numpy.percentile(x, 50) / numpy.median (linear interpolation).
double median(std::vector<double> v);
// numpy.percentile with linear interpolation, pct in 0..100.
double percentile(std::vector<double> v, double pct);

// The reference's filter constants are per update at 90 Hz. RateScale
// converts them for the rate the pipeline actually runs at, so filters behave
// the same over time: counts scale with the rate, per-update smoothing
// factors compound (1 - (1-a)^k), and per-update limits scale linearly. At
// the reference rate every conversion is the identity, bit for bit.
struct RateScale {
    double ref_hz = 90.0, hz = 90.0;
    double k() const { return ref_hz / hz; }   // reference updates per actual update
    size_t count(double ref_updates) const;    // >= 1
    double alpha(double a) const;
    double limit(double v) const { return hz == ref_hz ? v : v * k(); }
};

// Compounds a per-update smoothing factor over k reference updates.
double alpha_for(double a, double k);

// Estimates the update rate from frame capture times (EMA of intervals).
// Gaps over 0.5 s (a pause, a stall) are skipped rather than averaged in.
class UpdateRate {
public:
    explicit UpdateRate(double initial_hz) : dt_(1.0 / initial_hz) {}
    void tick(uint64_t capture_ns);
    void restart() { last_ns_ = 0; }   // next interval starts fresh (after a pause)
    double hz() const { return 1.0 / dt_; }

private:
    double dt_;
    uint64_t last_ns_ = 0;
};

// Per-eye neutral-open reference (class LidCalibrator).
class LidCalibrator {
public:
    LidCalibrator(size_t window, size_t warmup, double floor = 0.5, double pct = 50, size_t every = 30);
    double update(double p);
    // New update rate: window/warmup/recompute interval in updates. Keeps the samples (trimmed to the window).
    void set_timing(size_t window, size_t warmup, size_t every);
    bool ready() const { return n_ >= warmup_; }
    double ref() const { return ref_; }

private:
    std::deque<double> buf_;
    size_t window_, warmup_;
    double floor_, pct_;
    size_t every_;
    double ref_ = 1.0;
    size_t n_ = 0;
};

// Per-eye openness calibration for models whose lid output isn't capped at
// 1, after the VIVE SRanipal lip post-processing (blendshape_post_processing.py,
// reverse-engineered from LPredictionModule.dll):
//  - a baseline (the person's neutral open) seeded from a warm-up median and
//    then tracked by SRanipal's guarded Kalman-like EMA: only inputs within
//    `tol` of the baseline move it, so blinks and held wide eyes don't;
//  - one axis split into two outputs around it: the ratio x / baseline
//    (Lid, 1 = neutral) and Widen = (ratio - 1) / (ceiling - 1);
//  - the wide ceiling normalised by the largest sustained ratio seen, seeded
//    at 1 + widen_seed (SRanipal's max-observed normalisation), and never
//    above what the model can output: saturation / baseline (the lid output
//    saturates near 1.0 even uncapped), nor ceiling_max.
// All state resets per session. Counts are in updates (convert with RateScale).
class OpennessCalibrator {
public:
    struct Params {
        size_t skip = 90, warmup = 180;        // updates ignored, then used for the seed (1 s, 2 s at 90 Hz)
        size_t every = 12, peak_window = 9;    // baseline update interval; median length for the ceiling
        double floor = 0.2;                    // seed samples below this are blinks
        double tol = 0.15;                     // relative band around the baseline that may move it
        double gain = 0.001, decay = 20.5, acc0 = 0.5;   // SRanipal's calibration smoother
        double widen_seed = 0.15, ceiling_max = 1.6;
        double saturation = 1.0;               // the raw output's practical maximum
    };
    struct Out {
        double openness = 0;   // filtered ratio to the neutral (>= 0; Lid = min(openness, 1))
        double widen = 0;      // 0..1
        bool ready = false;
    };

    OpennessCalibrator() : OpennessCalibrator(Params{}) {}
    // filter: the One Euro filter on the openness (nullopt: none).
    explicit OpennessCalibrator(Params p, std::optional<OneEuroParams> filter = std::nullopt);
    // x: this update's raw openness; dt_s: seconds since the previous update (<= 0: the first).
    Out update(double x, double dt_s);
    void set_timing(size_t skip, size_t warmup, size_t every, size_t peak_window);
    bool ready() const { return ready_; }
    double baseline() const { return b_; }
    double ceiling() const { return c_; }   // as a ratio to the baseline (raw: ceiling() * baseline())

private:
    double cap() const;   // the ceiling's upper limit for the current baseline
    Params p_;
    size_t n_ = 0;
    bool ready_ = false;
    double b_ = 1.0, acc_ = 0, c_ = 1.0;
    std::deque<double> seed_, chunk_, recent_;
    std::optional<OneEuro> filter_;
};

// Hysteresis gate, blink hold, low-pass + rate limit, baseline ratio (class PupilTracker).
class PupilTracker {
public:
    struct Result {
        std::optional<double> px, ratio;
        bool live = false;
    };
    // filter: the One Euro filter on the pupil diameter (nullopt: none);
    // rate: the most the shown diameter moves per reference update (px).
    PupilTracker(double gate, double hys, int hold, std::optional<OneEuroParams> filter, double rate,
                 size_t base_window, size_t base_min);

    static constexpr double kPupilScale = 3000.0;   // model output = area / 3000 (400 px crop)
    static double px_of(double y);

    // openness: calibrated lid value; y: a NEW pupil output, or nullopt if none arrived since the last update.
    // k: reference (90 Hz) updates this update stands for (RateScale::k); 1 = the reference rate.
    // dt_s: seconds since the previous update, for the filter (<= 0: the first).
    Result update(double openness, std::optional<double> y, double k = 1.0, double dt_s = 0.0);
    void set_hold(int hold) { hold_ = hold; }   // blink hold in updates (new update rate)
    // Baseline window and samples needed for the first baseline, in pupil samples. Keeps the samples.
    void set_base_window(size_t window, size_t min);
    bool open() const { return open_; }
    std::optional<double> base() const { return base_; }

private:
    double gate_, hys_, rate_;
    std::optional<OneEuro> filter_;
    int hold_;
    bool open_ = true;
    int since_ = 0, held_n_ = 0;
    std::deque<double> recent_;           // maxlen 8
    std::deque<double> base_buf_;         // maxlen base_window
    size_t base_window_, base_min_;
    std::optional<double> target_, disp_, base_;
};

// Dilation 0..1 around the person's baseline (0.5); nullopt before a baseline exists.
std::optional<double> dilation(const PupilTracker::Result &r, double gain);

} // namespace babble::eye
