#include "model/postprocess/one_euro.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace babble {

std::optional<OneEuroParams> one_euro_params(const nlohmann::json &v, const OneEuroParams &defaults) {
    if (v.is_boolean())
        return v.get<bool>() ? std::optional(defaults) : std::nullopt;
    OneEuroParams p = defaults;
    if (v.is_object()) {
        const auto num = [&](const char *key, double fallback) {
            const auto it = v.find(key);
            return it != v.end() && it->is_number() ? it->get<double>() : fallback;
        };
        p.min_cutoff = num("min_cutoff", p.min_cutoff);
        p.beta = num("beta", p.beta);
        p.d_cutoff = num("d_cutoff", p.d_cutoff);
    }
    return p;
}

OneEuro::OneEuro(size_t n, OneEuroParams p) : p_(p), x_prev_(n, 0.0), dx_prev_(n, 0.0) {}

void OneEuro::reset() {
    init_ = false;
    std::fill(dx_prev_.begin(), dx_prev_.end(), 0.0);
}

double OneEuro::step(size_t i, double x, double dt_s) {
    const double tau = 2.0 * std::numbers::pi;
    const double r_d = tau * p_.d_cutoff * dt_s, a_d = r_d / (r_d + 1.0);
    const double dx = (x - x_prev_[i]) / dt_s;
    const double dx_hat = a_d * dx + (1.0 - a_d) * dx_prev_[i];
    const double cutoff = p_.min_cutoff + p_.beta * std::fabs(dx_hat);
    const double r = tau * cutoff * dt_s, a = r / (r + 1.0);
    const double x_hat = a * x + (1.0 - a) * x_prev_[i];
    x_prev_[i] = x_hat;
    dx_prev_[i] = dx_hat;
    return x_hat;
}

void OneEuro::apply(float *x, size_t n, double dt_s) {
    n = std::min(n, x_prev_.size());
    if (!init_ || dt_s <= 0.0) {
        std::copy(x, x + n, x_prev_.begin());
        init_ = true;
        return;
    }
    for (size_t i = 0; i < n; i++)
        x[i] = float(step(i, x[i], dt_s));
}

double OneEuro::apply(double x, double dt_s) {
    if (x_prev_.empty())
        return x;
    if (!init_ || dt_s <= 0.0) {
        x_prev_[0] = x;
        init_ = true;
        return x;
    }
    return step(0, x, dt_s);
}

}
