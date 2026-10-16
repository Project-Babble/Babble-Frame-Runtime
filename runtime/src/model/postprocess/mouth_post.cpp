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

AutoCalibrator::AutoCalibrator(float max_seed) : max_seed_(max_seed) { reset(); }

void AutoCalibrator::reset() { max_observed_.fill(max_seed_); }

Vec AutoCalibrator::process(const Vec &raw, bool present) {
    Vec out;
    for (size_t i = 0; i < kNumChannels; i++) {
        if (present)
            max_observed_[i] = std::max(max_observed_[i], raw[i]);
        bool seen = max_observed_[i] > max_seed_;
        out[i] = std::clamp(seen ? raw[i] / std::max(max_observed_[i], 1e-6f) : raw[i], 0.0f, 1.0f);
    }
    return out;
}

bool parse_mode(const std::string &s, CalibMode *out) {
    if (s == "raw") *out = CalibMode::Raw;
    else if (s == "manual") *out = CalibMode::Manual;
    else if (s == "auto") *out = CalibMode::Auto;
    else return false;
    return true;
}

Post::Post(std::optional<OneEuroParams> filter, CalibMode mode, Calibration cal)
    : mode_(mode), cal_(std::move(cal)) {
    if (filter)
        filter_.emplace(kNumShapes, *filter);
}

void Post::reset() {
    auto_.reset();
    if (filter_)
        filter_->reset();
}

Vec Post::process(const Vec &pred, double dt_s) {
    Vec x = pred;
    if (filter_)
        filter_->apply(x.data(), kNumShapes, dt_s);
    Vec norm;
    switch (mode_) {
    case CalibMode::Raw:
        for (size_t i = 0; i < kNumChannels; i++)
            norm[i] = std::clamp(x[i], 0.0f, 1.0f);
        break;
    case CalibMode::Manual:
        norm = cal_.normalize(x);
        break;
    case CalibMode::Auto:
        norm = auto_.process(x, true);
        break;
    }
    return norm;
}

}
