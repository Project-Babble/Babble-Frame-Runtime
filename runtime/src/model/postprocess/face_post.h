#pragma once

// Post-processing for Baballonia's mono face model: its One Euro smoothing
// (Services/Inference/Filters/OneEuroFilter.cs, same defaults), then
// per-shape calibration.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace babble::face {

// One Euro filter over N channels (Casiez et al.), as Baballonia runs it:
// the derivative is taken against the previous filtered value.
class OneEuro {
public:
    OneEuro(size_t n, float min_cutoff = 0.5f, float beta = 3.0f, float d_cutoff = 1.0f);
    // dt_s <= 0 (the first sample, or a repeated timestamp) passes x through.
    void apply(std::vector<float> &x, float dt_s);
    void reset();

private:
    float min_cutoff_, beta_, d_cutoff_;
    bool init_ = false;
    std::vector<float> x_prev_, dx_prev_;
};

enum class CalibMode { Raw, Manual, Auto };
bool parse_mode(const std::string &s, CalibMode *out);

// Per-shape calibration. Raw clamps to 0..1. Manual maps [min, max] from a
// table to 0..1. Auto divides by the largest value seen this session once it
// passes `seed` (below that the raw value passes), so the wearer's own
// strongest expression reaches 1 (the mouth's AutoCalibrator rule).
class Calibration {
public:
    Calibration(size_t n, CalibMode mode, float seed = 0.5f);
    void set_range(size_t i, float min, float max);   // manual mode
    // present: only frames with the headset worn raise the running maximum.
    void apply(std::vector<float> &x, bool present = true);
    void reset();   // a new session: forget the maxima
    CalibMode mode() const { return mode_; }

private:
    CalibMode mode_;
    float seed_;
    std::vector<std::pair<float, float>> range_;
    std::vector<float> max_;
};

} // namespace babble::face
