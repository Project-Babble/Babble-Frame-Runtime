#include "model/postprocess/eye_post.h"

#include <algorithm>
#include <cmath>

namespace babble::eye {

double percentile(std::vector<double> v, double pct) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    // numpy "linear": position = (n - 1) * pct / 100, interpolate between neighbours.
    double pos = double(v.size() - 1) * pct / 100.0;
    size_t lo = size_t(std::floor(pos));
    size_t hi = std::min(lo + 1, v.size() - 1);
    double frac = pos - double(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}

double median(std::vector<double> v) { return percentile(std::move(v), 50.0); }

LidCalibrator::LidCalibrator(size_t window, size_t warmup, double floor, double pct)
    : window_(window), warmup_(warmup), floor_(floor), pct_(pct) {}

double LidCalibrator::update(double p) {
    if (p > 0.6) {
        buf_.push_back(p);
        if (buf_.size() > window_)
            buf_.pop_front();
        n_++;
        if (n_ <= warmup_ || n_ % 30 == 0)
            ref_ = std::max(floor_, percentile(std::vector<double>(buf_.begin(), buf_.end()), pct_));
    }
    return n_ >= warmup_ ? p / ref_ : std::min(p, 1.0);
}

PupilTracker::PupilTracker(double gate, double hys, int hold, double alpha, double rate, size_t base_window,
                           size_t base_min)
    : gate_(gate), hys_(hys), alpha_(alpha), rate_(rate), hold_(hold), base_window_(base_window), base_min_(base_min) {}

double PupilTracker::px_of(double y) { return 2.0 * std::sqrt(std::max(y, 0.0) * kPupilScale / M_PI); }

PupilTracker::Result PupilTracker::update(double openness, std::optional<double> y) {
    if (open_ && openness < gate_)
        open_ = false;
    else if (!open_ && openness >= gate_ + hys_)
        open_ = true;
    since_ = open_ ? since_ + 1 : 0;
    const bool live = open_ && since_ > hold_;
    if (live) {
        held_n_ = 0;
        if (y) {
            double px = px_of(*y);
            recent_.push_back(px);
            if (recent_.size() > 8)
                recent_.pop_front();
            base_buf_.push_back(px);
            if (base_buf_.size() > base_window_)
                base_buf_.pop_front();
            target_ = px;
            if (base_buf_.size() >= base_min_ && (base_buf_.size() % 10 == 0 || !base_))
                base_ = median(std::vector<double>(base_buf_.begin(), base_buf_.end()));
        }
    } else {   // lid shut or blinking: hold a robust recent value
        if (held_n_ == 0 && !recent_.empty())
            target_ = median(std::vector<double>(recent_.begin(), recent_.end()));
        held_n_++;
    }
    if (!target_)
        return {};
    if (!disp_)
        disp_ = target_;
    double step = alpha_ * (*target_ - *disp_);
    *disp_ += std::max(-rate_, std::min(rate_, step));
    Result r;
    r.px = disp_;
    // Python "if self.base" is falsy for a 0.0 baseline as well as None.
    if (base_ && *base_ != 0.0)
        r.ratio = *disp_ / *base_;
    r.live = live;
    return r;
}

std::optional<double> dilation(const PupilTracker::Result &r, double gain) {
    if (!r.ratio)
        return std::nullopt;
    return std::max(0.0, std::min(1.0, 0.5 + gain * (*r.ratio - 1.0)));
}

} // namespace babble::eye
