#pragma once

// The One Euro filter (Casiez et al., CHI 2012) every pipeline smooths its
// outputs with: a low-pass whose cutoff rises with the signal's speed, so it
// is steady at rest and keeps up with fast movements (blinks, a jaw drop).
// The derivative is taken against the previous filtered value, as Baballonia
// runs it (Services/Inference/Filters/OneEuroFilter.cs).

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace babble {

struct OneEuroParams {
    double min_cutoff = 0.5;   // Hz at rest: lower is steadier
    double beta = 3.0;         // how fast the cutoff rises with speed (per unit/s): higher is snappier
    double d_cutoff = 1.0;     // Hz, the speed estimate's own low-pass
    bool operator==(const OneEuroParams &) const = default;
};

// A pipeline's filter option: an object (missing fields take `defaults`) or
// true for the defaults; false turns the filter off (nullopt). Anything else
// is the defaults.
std::optional<OneEuroParams> one_euro_params(const nlohmann::json &v, const OneEuroParams &defaults);

// One Euro over n channels sharing one set of parameters.
class OneEuro {
public:
    explicit OneEuro(size_t n = 1, OneEuroParams p = {});
    // dt_s: seconds since the previous sample. dt_s <= 0 (the first sample,
    // or a repeated timestamp) passes x through and starts from it.
    void apply(float *x, size_t n, double dt_s);
    void apply(std::vector<float> &x, double dt_s) { apply(x.data(), x.size(), dt_s); }
    double apply(double x, double dt_s);   // the one-channel form
    void reset();                          // forget the state; the next sample passes through
    const OneEuroParams &params() const { return p_; }

private:
    double step(size_t i, double x, double dt_s);
    OneEuroParams p_;
    bool init_ = false;
    std::vector<double> x_prev_, dx_prev_;
};

} // namespace babble
