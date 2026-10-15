#include "model/postprocess/face_post.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace babble::face {

bool parse_mode(const std::string &s, CalibMode *out) {
    if (s == "raw") *out = CalibMode::Raw;
    else if (s == "manual") *out = CalibMode::Manual;
    else if (s == "auto") *out = CalibMode::Auto;
    else return false;
    return true;
}

Calibration::Calibration(size_t n, CalibMode mode, float seed)
    : mode_(mode), seed_(seed), range_(n, {0.0f, 1.0f}), max_(n, 0.0f) {}

void Calibration::set_range(size_t i, float min, float max) {
    if (i < range_.size())
        range_[i] = {min, max};
}

void Calibration::reset() { std::fill(max_.begin(), max_.end(), 0.0f); }

void Calibration::apply(std::vector<float> &x, bool present) {
    const size_t n = std::min(x.size(), max_.size());
    for (size_t i = 0; i < n; i++) {
        float v = x[i];
        switch (mode_) {
        case CalibMode::Raw: break;
        case CalibMode::Manual: {
            const auto [lo, hi] = range_[i];
            v = hi - lo > 1e-6f ? (v - lo) / (hi - lo) : v;
            break;
        }
        case CalibMode::Auto:
            if (present)
                max_[i] = std::max(max_[i], v);
            if (max_[i] > seed_)
                v /= max_[i];
            break;
        }
        x[i] = std::clamp(v, 0.0f, 1.0f);
    }
}

} // namespace babble::face
