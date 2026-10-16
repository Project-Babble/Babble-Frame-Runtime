#pragma once

// This is the post-processing of mouth_osc_pc.py for the FusionNet mouth model, with a One Euro filter.

#include "model/postprocess/one_euro.h"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace babble::mouth {

// The runtime ignores the 44th output, UserPresent, because the headset proximity sensor gives the presence.
inline constexpr std::array<const char *, 44> kChannels = {
    "CheekPuffLeft", "CheekPuffRight", "CheekSuckLeft", "CheekSuckRight",
    "JawOpen", "JawForward", "JawLeft", "JawRight",
    "NoseSneerLeft", "NoseSneerRight",
    "MouthFunnel", "MouthPucker", "MouthLeft", "MouthRight",
    "MouthRollUpper", "MouthRollLower", "MouthShrugUpper", "MouthShrugLower", "MouthClose",
    "MouthSmileLeft", "MouthSmileRight", "MouthFrownLeft", "MouthFrownRight",
    "MouthDimpleLeft", "MouthDimpleRight", "MouthUpperUpLeft", "MouthUpperUpRight",
    "MouthLowerDownLeft", "MouthLowerDownRight", "MouthPressLeft", "MouthPressRight",
    "MouthStretchLeft", "MouthStretchRight",
    "TongueOut", "TongueUp", "TongueDown", "TongueLeft", "TongueRight",
    "TongueRoll", "TongueSquish", "TongueFlat", "TongueTwistLeft", "TongueTwistRight",
    "UserPresent",
};
inline constexpr size_t kNumChannels = kChannels.size();
inline constexpr size_t kNumShapes = 43;   // This count does not include UserPresent.

using Vec = std::array<float, kNumChannels>;

// These mouth filter defaults are the Baballonia defaults for the USB face tracker.
inline constexpr OneEuroParams kOneEuro{0.5, 3.0, 1.0};

// Change "JawOpen" to "jawOpen", which is the spelling of the Baballonia module.
std::string camel(const char *name);

// This is the min/max normalization for each channel, as in the reference Calibration class.
class Calibration {
public:
    static constexpr float kEps = 0.05f;
    Calibration();
    // A missing file gives a calibration that does not change the values.
    static Calibration load(const std::filesystem::path &path, std::string *error = nullptr);
    static Calibration from_json(const nlohmann::json &j);
    Vec normalize(const Vec &raw) const;
    Vec mins, maxs;
};

// This is the running-max normalization of the reference AutoCalibrator, without its EMA.
class AutoCalibrator {
public:
    explicit AutoCalibrator(float max_seed = 0.5f);
    void reset();
    Vec process(const Vec &raw, bool present);

private:
    float max_seed_;
    Vec max_observed_;
};

enum class CalibMode { Raw, Manual, Auto };
bool parse_mode(const std::string &s, CalibMode *out);

// Post applies the One Euro filter and then the calibration to each frame.
class Post {
public:
    Post(std::optional<OneEuroParams> filter, CalibMode mode, Calibration cal);
    // The dt_s value is the time in seconds since the previous frame, and 0 or less for the first frame.
    Vec process(const Vec &pred, double dt_s);
    void set_mode(CalibMode m) { mode_ = m; }
    void reset();   // Call this for a new session to forget the filter state and the auto-calibration.

private:
    std::optional<OneEuro> filter_;
    CalibMode mode_;
    Calibration cal_;
    AutoCalibrator auto_;
};

}
