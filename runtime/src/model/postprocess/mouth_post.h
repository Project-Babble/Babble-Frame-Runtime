#pragma once

// Post-processing for the FusionNet mouth model, ported line for line from
// mouth_osc_pc.py (the PC reference implementation). Arithmetic is float32,
// matching NumPy float32 arrays there.

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace babble::mouth {

// Model output order (mouth_osc_pc.py CHANNELS). Index 43 is the UserPresent detector.
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
inline constexpr size_t kPresenceIdx = 43;

using Vec = std::array<float, kNumChannels>;

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

// Adaptive running-max normalization with a ramping EMA (class AutoCalibrator).
class AutoCalibrator {
public:
    explicit AutoCalibrator(float max_seed = 0.5f, float ema_decay = 0.5f, float ema_gain = 0.25f);
    void reset();
    Vec process(const Vec &raw, bool present);

private:
    float max_seed_, ema_decay_, ema_gain_;
    Vec max_observed_, smoothed_, weight_acc_;
};

// Debounced UserPresent state machine (class PresenceGate).
class PresenceGate {
public:
    explicit PresenceGate(float threshold = 0.5f, int on_frames = 6, int off_frames = 20, int reset_after = 60);
    // Returns {active, should_reset}.
    std::pair<bool, bool> update(float value);
    bool active() const { return active_; }

private:
    float threshold_;
    int on_frames_, off_frames_, reset_after_;
    bool active_ = false, did_reset_ = false;
    int present_ = 0, absent_ = 0;
};

enum class CalibMode { Raw, Manual, Auto };
bool parse_mode(const std::string &s, CalibMode *out);

// The per-frame chain from mouth_osc_pc.py main loop (lines 539-552):
// EMA -> presence -> gate -> calibration -> zero when inactive.
class Post {
public:
    Post(float ema, float presence_threshold, bool use_presence, CalibMode mode, Calibration cal);
    // pred: model output for one frame. Returns the normalized channels, with
    // [kPresenceIdx] set to the (EMA'd, clipped) presence value.
    Vec process(const Vec &pred);
    bool active() const { return gate_.active(); }
    void set_mode(CalibMode m) { mode_ = m; }

private:
    float ema_;
    bool use_presence_;
    CalibMode mode_;
    Calibration cal_;
    AutoCalibrator auto_;
    PresenceGate gate_;
    Vec raw_state_{};
    bool have_state_ = false;
};

} // namespace babble::mouth
