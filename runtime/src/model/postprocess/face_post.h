#pragma once

// This is the per-shape calibration for the Baballonia mono face model, after the One Euro filter.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace babble::face {

enum class CalibMode { Raw, Manual, Auto };
bool parse_mode(const std::string &s, CalibMode *out);

// In auto mode, the strongest expression of the wearer in the session becomes 1.
class Calibration {
public:
    Calibration(size_t n, CalibMode mode, float seed = 0.5f);
    void set_range(size_t i, float min, float max);   // This is for the manual mode.
    // Only frames with present set to true raise the running maximum.
    void apply(std::vector<float> &x, bool present = true);
    void reset();   // Call this for a new session to forget the maxima.
    CalibMode mode() const { return mode_; }

private:
    CalibMode mode_;
    float seed_;
    std::vector<std::pair<float, float>> range_;
    std::vector<float> max_;
};

}
