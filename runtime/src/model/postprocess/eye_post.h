#pragma once

// Post-processing for the lid and pupil eye models, ported line for line from
// eye_osc_pc.py (the PC reference implementation). Arithmetic is double,
// matching the Python floats there. All frame-count constants are in updates;
// the reference runs at 90 updates/s, so the runtime runs the eye stream at 90.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace babble::eye {

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

// Hysteresis gate, blink hold, low-pass + rate limit, baseline ratio (class PupilTracker).
class PupilTracker {
public:
    struct Result {
        std::optional<double> px, ratio;
        bool live = false;
    };
    PupilTracker(double gate, double hys, int hold, double alpha, double rate, size_t base_window, size_t base_min);

    static constexpr double kPupilScale = 3000.0;   // model output = area / 3000 (400 px crop)
    static double px_of(double y);

    // openness: calibrated lid value; y: a NEW pupil output, or nullopt if none arrived since the last update.
    // k: reference (90 Hz) updates this update stands for (RateScale::k); 1 = the reference rate.
    Result update(double openness, std::optional<double> y, double k = 1.0);
    void set_hold(int hold) { hold_ = hold; }   // blink hold in updates (new update rate)
    // Baseline window and samples needed for the first baseline, in pupil samples. Keeps the samples.
    void set_base_window(size_t window, size_t min);
    bool open() const { return open_; }
    std::optional<double> base() const { return base_; }

private:
    double gate_, hys_, alpha_, rate_;
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
