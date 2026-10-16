#pragma once

// The One Euro filter is a low-pass filter with a cutoff that increases with the speed of the signal.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace babble {

struct OneEuroParams {
    double min_cutoff = 0.5;   // The cutoff at rest is in Hz, and a lower value gives less jitter.
    double beta = 3.0;         // A higher beta makes the cutoff increase faster with speed.
    double d_cutoff = 1.0;     // The cutoff of the speed estimate is in Hz.
    bool operator==(const OneEuroParams &) const = default;
};

// Get the filter parameters from a pipeline option, or nullopt if the option is false.
std::optional<OneEuroParams> one_euro_params(const nlohmann::json &v, const OneEuroParams &defaults);

// This filter applies one set of parameters to n channels.
class OneEuro {
public:
    explicit OneEuro(size_t n = 1, OneEuroParams p = {});
    // If dt_s is 0 or less, x does not change and the filter starts from it.
    void apply(float *x, size_t n, double dt_s);
    void apply(std::vector<float> &x, double dt_s) { apply(x.data(), x.size(), dt_s); }
    double apply(double x, double dt_s);   // This is the one-channel form.
    void reset();                          // After a reset, the next sample does not change.
    const OneEuroParams &params() const { return p_; }

private:
    double step(size_t i, double x, double dt_s);
    OneEuroParams p_;
    bool init_ = false;
    std::vector<double> x_prev_, dx_prev_;
};

}
