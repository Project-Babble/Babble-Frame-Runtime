#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "model/pipeline.h"

#include <array>
#include <atomic>
#include <optional>
#include <thread>

namespace babble {

namespace gaze {

struct Offsets {
    double roll_deg = 0, yaw_deg = 0, pitch_deg = 0;
};

// Angles, as frameeyeosc's gaze_angles() takes them (konsti219/frameeyeosc,
// src/main.rs): yaw = atan2(x, -z) and pitch = atan2(y, -z), each axis on its
// own (pitch is the vertical angle in the y-z plane, not the elevation).
//
// How angles become the values sent to the Baballonia module (which copies
// them into VRCFT's Eye.Gaze):
//  Angle:   angle / max_deg, so +-45 deg is +-1: gaze_angles() and Steam
//           Link's own OSC sender. Default.
//  Tangent: tan(angle), what VRCFT's Vector2.ToYaw/ToPitch strictly assume
//           (atan(x) / -atan(y)); with these angles that is x/z and y/z.
enum class Scaling { Tangent, Angle };

// eye_osc_pc.py SteamLinkGaze.convert: head-local look point (+x right, +y
// up, forward along -z in OpenVR, length = distance in m) -> (left yaw,
// right yaw, pitch); positive = right / up. Each eye sits ipd/2 from the head
// centre and gets its own yaw towards the point; pitch as gaze_angles().
// Angles are limited to +-max_deg, then scaled per `scaling`. nullopt for a
// degenerate vector.
std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling = Scaling::Angle);

// One eye's own gaze direction (head space, +x right, +y up, forward along
// -z) -> (yaw, pitch) for that eye: gaze_angles(), limited and scaled like
// convert(). nullopt for a degenerate vector.
std::optional<std::array<double, 2>> from_direction(double x, double y, double z, double max_deg, bool flip_y,
                                                    const Offsets &off, Scaling scaling = Scaling::Angle);

} // namespace gaze

// Config type "steamvr_gaze": eye gaze from SteamVR's own eye tracking,
// read through the runtime's shared SteamVR client (IVRInput eye-tracking
// action; background client, never starts SteamVR, never a running app). The world-space
// gaze target is moved into head space with the HMD pose and converted like
// the reference (per-eye yaw with the IPD split, shared pitch).
//
// Signals: LeftEyeX, LeftEyeY, RightEyeX, RightEyeY (-1..1, not side-swapped,
// as the reference sends them). After stale_s without a tracked sample the
// values go back to centre once.
//
// The IPD used for the per-eye split follows the headset's IPD setting in
// SteamVR (the shared client re-reads it on VREvent_IpdChanged).
//
// Config: thread, rate_hz (90), ipd ("auto" = from SteamVR, or metres to
// override; 0.063 is the fallback), scaling ("angle" | "tangent"),
// max_deg (45), flip_y (false),
// yaw_offset/pitch_offset/roll_offset (deg), stale_s (0.5), feed ("eye":
// the eye feed's on/off also switches gaze), gaze_source (live option):
//   "independent" (default): each eye's own gaze before SteamVR fuses them,
//                 from the eye-service shared memory (EyeServerClient);
//                 per-eye yaw and pitch, no IPD split. Falls back to
//                 "combined" for as long as that data is unavailable.
//   "combined":   SteamVR's fused gaze through IVRInput, split per eye by IPD.
// The input binding lives in the SteamVR client (config "steamvr").
class SteamVrGaze : public IPipeline {
public:
    SteamVrGaze(const nlohmann::json &cfg, PipelineDeps deps);
    ~SteamVrGaze() override;

    bool init() override;
    void start() override;
    void stop() override;
    const std::string &feed() const override { return feed_; }
    const std::string &name() const override { return name_; }
    void set_tracking(bool on) override { tracking_ = on; }
    bool set_option(const std::string &key, const nlohmann::json &value) override;

private:
    void run();

    std::string name_;
    nlohmann::json cfg_;
    PipelineDeps deps_;
    ThreadSpec thread_spec_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::string feed_;
    std::atomic<bool> tracking_{true};   // off: centre once, then stop reading gaze
    std::atomic<bool> independent_{true};   // gaze_source

    // One tick of each source; true if a valid sample was published.
    bool tick_independent(uint64_t now);
    bool tick_combined(uint64_t now);
    void publish(double lx, double ly, double rx, double ry, uint64_t now);

    std::optional<double> ipd_override_;
    uint64_t setup_connection_ = 0;
    uint64_t action_set_ = 0, action_ = 0;
    std::array<SignalId, 4> ids_{};   // LeftEyeX, LeftEyeY, RightEyeX, RightEyeY

    RateStat samples_;
    Counter invalid_, reconnects_, fallback_;
    double max_deg_ = 45, stale_ns_ = 5e8;
    bool flip_y_ = false;
    gaze::Scaling scaling_ = gaze::Scaling::Angle;
    gaze::Offsets off_;
    bool stats_registered_ = false;
};

} // namespace babble
