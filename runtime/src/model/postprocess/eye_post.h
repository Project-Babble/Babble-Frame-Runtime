#pragma once

// Post-processing for the lid and pupil eye models, ported line for line from
// eye_osc_pc.py (the PC reference implementation). Arithmetic is double,
// matching the Python floats there. All frame-count constants are in updates;
// the reference runs at 90 updates/s, so the runtime runs the eye stream at 90.

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace babble::eye {

// Median of a sample set, as numpy.percentile(x, 50) / numpy.median (linear interpolation).
double median(std::vector<double> v);
// numpy.percentile with linear interpolation, pct in 0..100.
double percentile(std::vector<double> v, double pct);

// Per-eye neutral-open reference (class LidCalibrator).
class LidCalibrator {
public:
    LidCalibrator(size_t window, size_t warmup, double floor = 0.5, double pct = 50);
    double update(double p);
    bool ready() const { return n_ >= warmup_; }
    double ref() const { return ref_; }

private:
    std::deque<double> buf_;
    size_t window_, warmup_;
    double floor_, pct_;
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
    Result update(double openness, std::optional<double> y);
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
