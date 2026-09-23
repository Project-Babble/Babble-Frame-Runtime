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

bool parse_mode(const std::string &s, CalibMode *out) {
    if (s == "raw") *out = CalibMode::Raw;
    else if (s == "manual") *out = CalibMode::Manual;
    else if (s == "auto") *out = CalibMode::Auto;
    else return false;
    return true;
}

Post::Post(float ema, CalibMode mode, Calibration cal) : ema_(ema), mode_(mode), cal_(std::move(cal)) {}

void Post::reset() {
    auto_.reset();
    have_state_ = false;
}

Vec Post::process(const Vec &pred) {
    // raw_state = pred if first else ema * pred + (1 - ema) * raw_state
    if (!have_state_) {
        raw_state_ = pred;
        have_state_ = true;
    } else {
        for (size_t i = 0; i < kNumChannels; i++)
            raw_state_[i] = ema_ * pred[i] + (1.0f - ema_) * raw_state_[i];
    }
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
        norm = auto_.process(pred, true);   // the reference feeds the unsmoothed prediction here
        break;
    }
    return norm;
}

} // namespace babble::mouth
