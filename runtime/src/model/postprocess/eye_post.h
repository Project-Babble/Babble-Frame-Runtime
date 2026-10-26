#pragma once

// This is the post-processing of eye_osc_pc.py for the lid and pupil models, with One Euro filters.

#include "model/postprocess/one_euro.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace babble::eye {

// These eye filter defaults were tuned on a recorded session to give less jitter than the old EMAs.
inline constexpr OneEuroParams kLidOneEuro{1.0, 5.0, 3.0};
// The pupil filter is for a diameter in mm. A pixel of the 400 px pane is about 0.1 mm at the pupil, so beta is 10 times
// the old pixel filter (0.01) to adapt at the same speed.
inline constexpr OneEuroParams kPupilOneEuroMm{0.5, 0.1, 1.0};

// Get the median of a sample set, as numpy.median does.
double median(std::vector<double> v);
// This is numpy.percentile with linear interpolation, and pct is in the range 0..100.
double percentile(std::vector<double> v, double pct);

// RateScale converts the per-update constants of the 90 Hz reference to the real rate of the pipeline.
struct RateScale {
    double ref_hz = 90.0, hz = 90.0;
    double k() const { return ref_hz / hz; }   // This is the number of reference updates for each real update.
    size_t count(double ref_updates) const;    // The result is 1 or more.
    double alpha(double a) const;
    double limit(double v) const { return hz == ref_hz ? v : v * k(); }
};

// Compound a per-update smoothing factor over k reference updates.
double alpha_for(double a, double k);

// Estimate the update rate from frame capture times, and ignore gaps of more than 0.5 s.
class UpdateRate {
public:
    explicit UpdateRate(double initial_hz) : dt_(1.0 / initial_hz) {}
    void tick(uint64_t capture_ns);
    void restart() { last_ns_ = 0; }   // Call this after a pause so that the gap is not an interval.
    double hz() const { return 1.0 / dt_; }

private:
    double dt_;
    uint64_t last_ns_ = 0;
};

// This is the neutral-open reference for one eye, as in the reference LidCalibrator.
class LidCalibrator {
public:
    LidCalibrator(size_t window, size_t warmup, double floor = 0.5, double pct = 50, size_t every = 30);
    double update(double p);
    // Set new update counts for a new update rate and keep the samples.
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

// This calibrator splits the openness of one eye into Lid and Widen around a neutral baseline, as SRanipal does.
class OpennessCalibrator {
public:
    struct Params {
        size_t skip = 90, warmup = 180;        // The seed ignores skip updates and then uses warmup updates.
        size_t every = 12, peak_window = 9;    // The units are updates.
        double floor = 0.2;                    // Seed samples below this value are blinks.
        double tol = 0.15;                     // Only samples in this relative band around the baseline move it.
        double gain = 0.001, decay = 20.5, acc0 = 0.5;   // The SRanipal smoother uses these values.
        double widen_seed = 0.15, ceiling_max = 1.6;
        double saturation = 1.0;               // This is the practical maximum of the raw output.
        // The closed level starts at closed_seed. Then it is the median of the bottoms of the last closed_events
        // closes, so that it moves up and down and one deep squeeze does not set it. A close starts below
        // closed_enter and ends above closed_exit, and its bottom is the lowest openness held for trough_window
        // updates (at least 2). A seed of 0 turns it off, so that Lid is the openness.
        double closed_seed = 0.0;
        size_t trough_window = 5;
        double closed_enter = 0.5, closed_exit = 0.7;
        size_t closed_events = 7;
    };
    struct Out {
        double openness = 0;   // The filtered ratio to the neutral is 0 or more.
        double lid = 0;        // The openness from the closed level (0) to the neutral (1), in the range 0..1.
        double widen = 0;      // The range is 0..1.
        bool ready = false;
    };

    OpennessCalibrator() : OpennessCalibrator(Params{}) {}
    // Set filter to nullopt for no One Euro filter on the openness.
    explicit OpennessCalibrator(Params p, std::optional<OneEuroParams> filter = std::nullopt);
    // The dt_s value is the time in seconds since the previous update, and 0 or less for the first update.
    Out update(double x, double dt_s);
    void set_timing(size_t skip, size_t warmup, size_t every, size_t peak_window, size_t trough_window = 5);
    bool ready() const { return ready_; }
    double baseline() const { return b_; }
    double ceiling() const { return c_; }   // The ceiling is a ratio to the baseline.
    double closed() const { return f_; }    // The closed level is a ratio to the baseline.

private:
    double cap() const;   // This is the upper limit of the ceiling for the current baseline.
    Params p_;
    size_t n_ = 0;
    bool ready_ = false;
    double b_ = 1.0, acc_ = 0, c_ = 1.0, f_ = 0;
    bool closing_ = false;
    double close_min_ = 0;
    std::deque<double> seed_, chunk_, recent_, lows_, troughs_;
    std::optional<OneEuro> filter_;
};

// This tracker filters the pupil size with a gate, a blink hold and a rate limit.
class PupilTracker {
public:
    struct Result {
        std::optional<double> px, ratio;
        bool live = false;
    };
    // The rate value is the maximum change of the shown diameter (in mm for the eye kit) for each reference update.
    PupilTracker(double gate, double hys, int hold, std::optional<OneEuroParams> filter, double rate,
                 size_t base_window, size_t base_min);

    // Set d (the pupil diameter, in mm from the eye kit) to nullopt if no new pupil output arrived after the last
    // update. Result::px and the rate are in the units of d.
    Result update(double openness, std::optional<double> d, double k = 1.0, double dt_s = 0.0);
    void set_hold(int hold) { hold_ = hold; }   // The blink hold is in updates.
    // Set the baseline window and the minimum samples for the first baseline, and keep the samples.
    void set_base_window(size_t window, size_t min);
    bool open() const { return open_; }
    std::optional<double> base() const { return base_; }

private:
    double gate_, hys_, rate_;
    std::optional<OneEuro> filter_;
    int hold_;
    bool open_ = true;
    int since_ = 0, held_n_ = 0;
    std::deque<double> recent_;           // The maximum length is 8.
    std::deque<double> base_buf_;         // The maximum length is base_window.
    size_t base_window_, base_min_;
    std::optional<double> target_, disp_, base_;
};

// Get the dilation in the range 0..1, with 0.5 at the baseline, or nullopt if no baseline exists.
std::optional<double> dilation(const PupilTracker::Result &r, double gain);

}
