#include "model/postprocess/mouth_post.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace babble::mouth {

std::string camel(const char *name) {
    std::string s(name);
    if (!s.empty())
        s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

Calibration::Calibration() {
    mins.fill(0.0f);
    maxs.fill(1.0f);
}

Calibration Calibration::from_json(const nlohmann::json &j) {
    Calibration c;
    for (size_t i = 0; i < kNumChannels; i++) {
        if (!j.is_object() || !j.contains(kChannels[i]))
            continue;
        const auto &e = j[kChannels[i]];
        c.mins[i] = e.value("min", 0.0f);
        c.maxs[i] = e.value("max", 1.0f);
    }
    return c;
}

Calibration Calibration::load(const std::filesystem::path &path, std::string *error) {
    std::ifstream in(path);
    if (!in)
        return Calibration();
    try {
        return from_json(nlohmann::json::parse(in));
    } catch (const nlohmann::json::exception &e) {
        if (error)
            *error = e.what();
        return Calibration();
    }
}

Vec Calibration::normalize(const Vec &raw) const {
    Vec out;
    for (size_t i = 0; i < kNumChannels; i++)
        out[i] = std::clamp((raw[i] - mins[i]) / std::max(maxs[i] - mins[i], kEps), 0.0f, 1.0f);
    return out;
}

AutoCalibrator::AutoCalibrator(float max_seed, float ema_decay, float ema_gain)
    : max_seed_(max_seed), ema_decay_(ema_decay), ema_gain_(ema_gain) {
    reset();
}

void AutoCalibrator::reset() {
    max_observed_.fill(max_seed_);
    smoothed_.fill(0.0f);
    weight_acc_.fill(0.0f);
}

Vec AutoCalibrator::process(const Vec &raw, bool present) {
    for (size_t i = 0; i < kNumChannels; i++) {
        if (present)
            max_observed_[i] = std::max(max_observed_[i], raw[i]);
        bool seen = max_observed_[i] > max_seed_;
        float normed = std::clamp(seen ? raw[i] / std::max(max_observed_[i], 1e-6f) : raw[i], 0.0f, 1.0f);
        float weight = ema_gain_ + weight_acc_[i];
        float alpha = weight / (weight + ema_decay_);
        smoothed_[i] += (normed - smoothed_[i]) * alpha;
        weight_acc_[i] = (1.0f - alpha) * weight;
    }
    return smoothed_;
}

PresenceGate::PresenceGate(float threshold, int on_frames, int off_frames, int reset_after)
    : threshold_(threshold), on_frames_(on_frames), off_frames_(off_frames), reset_after_(reset_after) {}

std::pair<bool, bool> PresenceGate::update(float value) {
    if (value >= threshold_) {
        present_++;
        absent_ = 0;
        did_reset_ = false;
        if (!active_ && present_ >= on_frames_)
            active_ = true;
    } else {
        absent_++;
        present_ = 0;
        if (active_ && absent_ >= off_frames_)
            active_ = false;
    }
    bool should_reset = absent_ >= reset_after_ && !did_reset_;
    if (should_reset)
        did_reset_ = true;
    return {active_, should_reset};
}

bool parse_mode(const std::string &s, CalibMode *out) {
    if (s == "raw") *out = CalibMode::Raw;
    else if (s == "manual") *out = CalibMode::Manual;
    else if (s == "auto") *out = CalibMode::Auto;
    else return false;
    return true;
}

Post::Post(float ema, float presence_threshold, bool use_presence, CalibMode mode, Calibration cal)
    : ema_(ema), use_presence_(use_presence), mode_(mode), cal_(std::move(cal)), gate_(presence_threshold) {}

Vec Post::process(const Vec &pred) {
    // raw_state = pred if first else ema * pred + (1 - ema) * raw_state
    if (!have_state_) {
        raw_state_ = pred;
        have_state_ = true;
    } else {
        for (size_t i = 0; i < kNumChannels; i++)
            raw_state_[i] = ema_ * pred[i] + (1.0f - ema_) * raw_state_[i];
    }
    const float present = std::clamp(raw_state_[kPresenceIdx], 0.0f, 1.0f);
    auto [active, should_reset] = gate_.update(present);
    if (!use_presence_)
        active = true;
    else if (should_reset)
        auto_.reset();

    Vec norm;
    switch (mode_) {
    case CalibMode::Raw:
        for (size_t i = 0; i < kNumChannels; i++)
            norm[i] = std::clamp(raw_state_[i], 0.0f, 1.0f);
        break;
    case CalibMode::Manual:
        norm = cal_.normalize(raw_state_);
        break;
    case CalibMode::Auto:
        norm = auto_.process(pred, active);   // the reference feeds the unsmoothed prediction here
        break;
    }
    norm[kPresenceIdx] = present;
    if (!active)
        std::fill(norm.begin(), norm.begin() + kPresenceIdx, 0.0f);
    return norm;
}

} // namespace babble::mouth
