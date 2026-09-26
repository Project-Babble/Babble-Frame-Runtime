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

double alpha_for(double a, double k) { return k == 1.0 ? a : 1.0 - std::pow(1.0 - a, k); }

size_t RateScale::count(double ref_updates) const {
    if (hz == ref_hz)
        return std::max<size_t>(1, size_t(ref_updates));
    return std::max<size_t>(1, size_t(std::lround(ref_updates / k())));
}

double RateScale::alpha(double a) const { return hz == ref_hz ? a : alpha_for(a, k()); }

void UpdateRate::tick(uint64_t capture_ns) {
    if (last_ns_ && capture_ns > last_ns_) {
        const double dt = double(capture_ns - last_ns_) * 1e-9;
        if (dt < 0.5)
            dt_ += 0.05 * (dt - dt_);
    }
    last_ns_ = capture_ns;
}

LidCalibrator::LidCalibrator(size_t window, size_t warmup, double floor, double pct, size_t every)
    : window_(window), warmup_(warmup), floor_(floor), pct_(pct), every_(std::max<size_t>(1, every)) {}

double LidCalibrator::update(double p) {
    if (p > 0.6) {
        buf_.push_back(p);
        if (buf_.size() > window_)
            buf_.pop_front();
        n_++;
        if (n_ <= warmup_ || n_ % every_ == 0)
            ref_ = std::max(floor_, percentile(std::vector<double>(buf_.begin(), buf_.end()), pct_));
    }
    return n_ >= warmup_ ? p / ref_ : std::min(p, 1.0);
}

void LidCalibrator::set_timing(size_t window, size_t warmup, size_t every) {
    window_ = std::max<size_t>(1, window);
    warmup_ = warmup;
    every_ = std::max<size_t>(1, every);
    while (buf_.size() > window_)
        buf_.pop_front();
}

OpennessCalibrator::OpennessCalibrator(Params p) : p_(p), c_(1.0 + p.widen_seed) {}

void OpennessCalibrator::set_timing(size_t skip, size_t warmup, size_t every, size_t peak_window) {
    p_.skip = skip;
    p_.warmup = std::max<size_t>(1, warmup);
    p_.every = std::max<size_t>(1, every);
    p_.peak_window = std::max<size_t>(1, peak_window);
    while (seed_.size() > p_.warmup)
        seed_.pop_front();
    while (recent_.size() > p_.peak_window)
        recent_.pop_front();
}

OpennessCalibrator::Out OpennessCalibrator::update(double x, double alpha) {
    n_++;
    double value;
    if (!ready_) {
        // Seed: the median of non-blink samples after the first second.
        if (n_ > p_.skip && x > p_.floor) {
            seed_.push_back(x);
            if (seed_.size() > p_.warmup)
                seed_.pop_front();
        }
        if (n_ >= p_.skip + p_.warmup && seed_.size() >= std::max<size_t>(1, p_.warmup / 4)) {
            b_ = std::max(p_.floor, median(std::vector<double>(seed_.begin(), seed_.end())));
            acc_ = p_.acc0;
            ready_ = true;
            seed_.clear();
            sm_.reset();   // the ratio starts clean rather than blending with the pass-through
        }
        value = std::min(x, 1.0);   // uncalibrated until the seed exists, like the reference
    } else {
        // SRanipal _update_calibration: every `every` updates, a guarded
        // Kalman-like step towards the chunk's median.
        chunk_.push_back(x);
        if (chunk_.size() >= p_.every) {
            const double xm = median(std::vector<double>(chunk_.begin(), chunk_.end()));
            chunk_.clear();
            if (std::fabs(xm - b_) < p_.tol * b_) {
                const double w = p_.gain + acc_;
                const double a = w / (w + p_.decay);
                b_ += a * (xm - b_);
                acc_ = (1.0 - a) * w;
            }
        }
        value = x / b_;
    }
    sm_ = sm_ ? *sm_ + alpha * (value - *sm_) : value;
    Out out;
    out.openness = std::max(*sm_, 0.0);
    out.ready = ready_;
    if (ready_) {
        // Ceiling: the largest sustained ratio (short median), seeded and capped.
        recent_.push_back(out.openness);
        if (recent_.size() > p_.peak_window)
            recent_.pop_front();
        if (recent_.size() == p_.peak_window)
            c_ = std::min(p_.ceiling_max, std::max(c_, median(std::vector<double>(recent_.begin(), recent_.end()))));
        out.widen = std::clamp((out.openness - 1.0) / (c_ - 1.0), 0.0, 1.0);
    }
    return out;
}

PupilTracker::PupilTracker(double gate, double hys, int hold, double alpha, double rate, size_t base_window,
                           size_t base_min)
    : gate_(gate), hys_(hys), alpha_(alpha), rate_(rate), hold_(hold), base_window_(base_window), base_min_(base_min) {}

void PupilTracker::set_base_window(size_t window, size_t min) {
    base_window_ = std::max<size_t>(1, window);
    base_min_ = std::max<size_t>(1, min);
    while (base_buf_.size() > base_window_)
        base_buf_.pop_front();
}

double PupilTracker::px_of(double y) { return 2.0 * std::sqrt(std::max(y, 0.0) * kPupilScale / M_PI); }

PupilTracker::Result PupilTracker::update(double openness, std::optional<double> y, double k) {
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
    const double alpha = alpha_for(alpha_, k), rate = k == 1.0 ? rate_ : rate_ * k;
    double step = alpha * (*target_ - *disp_);
    *disp_ += std::max(-rate, std::min(rate, step));
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
