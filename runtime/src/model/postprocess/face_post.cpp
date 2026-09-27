#include "model/postprocess/face_post.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace babble::face {

OneEuro::OneEuro(size_t n, float min_cutoff, float beta, float d_cutoff)
    : min_cutoff_(min_cutoff), beta_(beta), d_cutoff_(d_cutoff), x_prev_(n, 0.0f), dx_prev_(n, 0.0f) {}

void OneEuro::reset() {
    init_ = false;
    std::fill(dx_prev_.begin(), dx_prev_.end(), 0.0f);
}

void OneEuro::apply(std::vector<float> &x, float dt_s) {
    const size_t n = std::min(x.size(), x_prev_.size());
    if (!init_ || dt_s <= 0.0f) {
        std::copy(x.begin(), x.begin() + long(n), x_prev_.begin());
        init_ = true;
        return;
    }
    const float tau = 2.0f * std::numbers::pi_v<float>;
    const float r_d = tau * d_cutoff_ * dt_s, a_d = r_d / (r_d + 1.0f);
    for (size_t i = 0; i < n; i++) {
        const float dx = (x[i] - x_prev_[i]) / dt_s;
        const float dx_hat = a_d * dx + (1.0f - a_d) * dx_prev_[i];
        const float cutoff = min_cutoff_ + beta_ * std::fabs(dx_hat);
        const float r = tau * cutoff * dt_s, a = r / (r + 1.0f);
        const float x_hat = a * x[i] + (1.0f - a) * x_prev_[i];
        x_prev_[i] = x_hat;
        dx_prev_[i] = dx_hat;
        x[i] = x_hat;
    }
}

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
