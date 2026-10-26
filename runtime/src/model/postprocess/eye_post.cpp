#include "model/postprocess/eye_post.h"

#include <algorithm>
#include <cmath>

namespace babble::eye {

double percentile(std::vector<double> v, double pct) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    // Interpolate between neighbours at position (n - 1) * pct / 100, as numpy "linear" does.
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

OpennessCalibrator::OpennessCalibrator(Params p, std::optional<OneEuroParams> filter)
    : p_(p), c_(1.0 + p.widen_seed), f_(std::clamp(p.closed_seed, 0.0, 0.9)) {
    if (filter)
        filter_.emplace(1, *filter);
}

// Keep the ceiling at or below saturation / baseline, because a higher ceiling makes Widen unreachable.
double OpennessCalibrator::cap() const {
    return std::min(p_.ceiling_max, std::max(1.02, p_.saturation / b_));
}

void OpennessCalibrator::set_timing(size_t skip, size_t warmup, size_t every, size_t peak_window, size_t trough_window) {
    p_.skip = skip;
    p_.warmup = std::max<size_t>(1, warmup);
    p_.every = std::max<size_t>(1, every);
    p_.peak_window = std::max<size_t>(1, peak_window);
    while (seed_.size() > p_.warmup)
        seed_.pop_front();
    while (recent_.size() > p_.peak_window)
        recent_.pop_front();
    p_.trough_window = std::max<size_t>(2, trough_window);
    while (lows_.size() > p_.trough_window)
        lows_.pop_front();
}

OpennessCalibrator::Out OpennessCalibrator::update(double x, double dt_s) {
    n_++;
    double value;
    if (!ready_) {
        // The seed is the median of the non-blink samples after the first second.
        if (n_ > p_.skip && x > p_.floor) {
            seed_.push_back(x);
            if (seed_.size() > p_.warmup)
                seed_.pop_front();
        }
        if (n_ >= p_.skip + p_.warmup && seed_.size() >= std::max<size_t>(1, p_.warmup / 4)) {
            b_ = std::max(p_.floor, median(std::vector<double>(seed_.begin(), seed_.end())));
            acc_ = p_.acc0;
            c_ = std::min(1.0 + p_.widen_seed, cap());
            ready_ = true;
            seed_.clear();
            if (filter_)
                filter_->reset();   // Reset the filter so that the ratio does not blend with the earlier raw values.
        }
        value = std::min(x, 1.0);   // The value has no calibration until the seed exists, as in the reference.
    } else {
        // Move the baseline toward the chunk median after each `every` updates, as SRanipal _update_calibration does.
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
    if (filter_)
        value = filter_->apply(value, dt_s);
    Out out;
    out.openness = std::max(value, 0.0);
    out.lid = std::min(out.openness, 1.0);
    out.ready = ready_;
    if (ready_) {
        // The ceiling is the largest sustained ratio.
        recent_.push_back(out.openness);
        if (recent_.size() > p_.peak_window)
            recent_.pop_front();
        if (recent_.size() == p_.peak_window)
            c_ = std::max(c_, median(std::vector<double>(recent_.begin(), recent_.end())));
        c_ = std::min(c_, cap());   // Apply the cap again because the baseline drifts.
        out.widen = std::clamp((out.openness - 1.0) / (c_ - 1.0), 0.0, 1.0);
        // The bottom of a close is the lowest openness that every update in the window stays at or below, and the
        // closed level is the median of the last bottoms.
        const size_t window = std::max<size_t>(2, p_.trough_window);
        lows_.push_back(out.openness);
        while (lows_.size() > window)
            lows_.pop_front();
        if (p_.closed_seed > 0) {
            if (!closing_ && out.openness < p_.closed_enter) {
                closing_ = true;
                close_min_ = p_.closed_enter;
            }
            if (closing_ && lows_.size() == window)
                close_min_ = std::min(close_min_, *std::max_element(lows_.begin(), lows_.end()));
            if (closing_ && out.openness > p_.closed_exit) {
                closing_ = false;
                if (close_min_ < p_.closed_enter) {
                    troughs_.push_back(close_min_);
                    while (troughs_.size() > std::max<size_t>(1, p_.closed_events))
                        troughs_.pop_front();
                    f_ = std::clamp(median(std::vector<double>(troughs_.begin(), troughs_.end())), 0.0, 0.9);
                }
            }
        }
        out.lid = std::clamp((out.openness - f_) / (1.0 - f_), 0.0, 1.0);
    }
    return out;
}

PupilTracker::PupilTracker(double gate, double hys, int hold, std::optional<OneEuroParams> filter, double rate,
                           size_t base_window, size_t base_min)
    : gate_(gate), hys_(hys), rate_(rate), hold_(hold), base_window_(base_window), base_min_(base_min) {
    if (filter)
        filter_.emplace(1, *filter);
}

void PupilTracker::set_base_window(size_t window, size_t min) {
    base_window_ = std::max<size_t>(1, window);
    base_min_ = std::max<size_t>(1, min);
    while (base_buf_.size() > base_window_)
        base_buf_.pop_front();
}

PupilTracker::Result PupilTracker::update(double openness, std::optional<double> d, double k, double dt_s) {
    if (open_ && openness < gate_)
        open_ = false;
    else if (!open_ && openness >= gate_ + hys_)
        open_ = true;
    since_ = open_ ? since_ + 1 : 0;
    const bool live = open_ && since_ > hold_;
    if (live) {
        held_n_ = 0;
        if (d) {
            const double px = *d;
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
    } else {   // Hold a recent value while the lid is shut.
        if (held_n_ == 0 && !recent_.empty())
            target_ = median(std::vector<double>(recent_.begin(), recent_.end()));
        held_n_++;
    }
    if (!target_)
        return {};
    // Follow the filtered target at no more than `rate` for each reference update.
    const double want = filter_ ? filter_->apply(*target_, dt_s) : *target_;
    if (!disp_)
        disp_ = want;
    const double rate = k == 1.0 ? rate_ : rate_ * k;
    *disp_ += std::max(-rate, std::min(rate, want - *disp_));
    Result r;
    r.px = disp_;
    // A 0.0 baseline counts as no baseline, as in the Python "if self.base".
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

}
