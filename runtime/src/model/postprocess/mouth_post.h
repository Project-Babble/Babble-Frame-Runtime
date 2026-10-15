#pragma once

// Post-processing for the FusionNet mouth model, after mouth_osc_pc.py (the
// PC reference implementation), with a One Euro filter in place of its EMAs.
// Arithmetic is float32, matching NumPy float32 arrays there.

#include "model/postprocess/one_euro.h"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace babble::mouth {

// Model output order (mouth_osc_pc.py CHANNELS). The prototype model has a
// 44th output, UserPresent; presence now comes from the headset's proximity
// sensor, so that output is ignored and new models may omit it.
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
inline constexpr size_t kNumShapes = 43;   // blendshapes, excluding UserPresent

using Vec = std::array<float, kNumChannels>;

// The mouth's smoothing defaults (Baballonia's, as the USB face tracker uses).
inline constexpr OneEuroParams kOneEuro{0.5, 3.0, 1.0};

// "JawOpen" -> "jawOpen", the spelling the Baballonia module uses.
std::string camel(const char *name);

// Per-channel min/max normalization (class Calibration).
class Calibration {
public:
    static constexpr float kEps = 0.05f;
    Calibration();
    // Missing channels default to 0/1; a missing file gives the pass-through calibration.
    static Calibration load(const std::filesystem::path &path, std::string *error = nullptr);
    static Calibration from_json(const nlohmann::json &j);
    Vec normalize(const Vec &raw) const;
    Vec mins, maxs;
};

// Adaptive running-max normalization (class AutoCalibrator, without its
// ramping EMA: Post's One Euro filter smooths before it).
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

// The per-frame chain: the One Euro filter (nullopt: off), then the
// calibration. Presence is handled outside the model: frames only arrive
// while SteamVR reports the headset worn, and reset() starts each new
// session (the reference's --no-presence path). Output: the 43 blendshapes.
class Post {
public:
    Post(std::optional<OneEuroParams> filter, CalibMode mode, Calibration cal);
    // dt_s: seconds since the previous frame (<= 0: the first). Uses channels
    // 0..42; any UserPresent value is ignored.
    Vec process(const Vec &pred, double dt_s);
    void set_mode(CalibMode m) { mode_ = m; }
    void reset();   // new session: forget the filter state and auto-calibration

private:
    std::optional<OneEuro> filter_;
    CalibMode mode_;
    Calibration cal_;
    AutoCalibrator auto_;
};

} // namespace babble::mouth
