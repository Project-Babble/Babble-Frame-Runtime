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

// How angles become the values sent to the Baballonia module (which copies
// them into VRCFT's Eye.Gaze):
//  Tangent: tan(angle). VRCFT reads Gaze as tangents (Vector2.ToYaw =
//           atan(x), ToPitch = -atan(y), VRCFaceTracking.Core/Types/Vector2.cs),
//           so this is the documented meaning. Default.
//  Angle:   angle / max_deg, as eye_osc_pc.py sends it; VRCFT then over-reads
//           mid-range angles by up to ~4 deg (30 deg arrives as 33.7).
enum class Scaling { Tangent, Angle };

// eye_osc_pc.py SteamLinkGaze.convert: head-local look point (+x right, +y
// up, forward along -z in OpenVR, length = distance in m) -> (left yaw,
// right yaw, pitch); positive = right / up. Each eye sits ipd/2 from the head
// centre and gets its own yaw towards the point. Angles are limited to
// +-max_deg, then scaled per `scaling`. nullopt for a degenerate vector.
std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling = Scaling::Tangent);

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
// override; 0.063 is the fallback), scaling ("tangent" | "angle"),
// max_deg (45), flip_y (false),
// yaw_offset/pitch_offset/roll_offset (deg), stale_s (0.5). The input
// binding lives in the SteamVR client (config "steamvr").
class SteamVrGaze : public IPipeline {
public:
    SteamVrGaze(const nlohmann::json &cfg, PipelineDeps deps);
    ~SteamVrGaze() override;

    bool init() override;
    void start() override;
    void stop() override;

private:
    void run();

    std::string name_;
    nlohmann::json cfg_;
    PipelineDeps deps_;
    ThreadSpec thread_spec_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::optional<double> ipd_override_;
    uint64_t setup_connection_ = 0;
    uint64_t action_set_ = 0, action_ = 0;
    std::array<SignalId, 4> ids_{};   // LeftEyeX, LeftEyeY, RightEyeX, RightEyeY

    RateStat samples_;
    Counter invalid_, reconnects_;
    bool stats_registered_ = false;
};

} // namespace babble
