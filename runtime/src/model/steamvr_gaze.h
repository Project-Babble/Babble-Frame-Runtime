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

// Angle sends angle / max_deg as Steam Link does, and Tangent sends tan(angle) as VRCFT expects.
enum class Scaling { Tangent, Angle };

// Convert a head-local look point in m to the left yaw, right yaw and pitch, as SteamLinkGaze.convert does.
std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling = Scaling::Angle);

// Convert the head-space gaze direction of one eye to the yaw and pitch of that eye.
std::optional<std::array<double, 2>> from_direction(double x, double y, double z, double max_deg, bool flip_y,
                                                    const Offsets &off, Scaling scaling = Scaling::Angle);

}

// The "steamvr_gaze" pipeline gets eye gaze from the SteamVR eye tracking and sends it for each eye.
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
    std::atomic<bool> tracking_{true};   // When this is false, the pipeline centres the gaze one time and stops.
    std::atomic<bool> independent_{true};   // This is true if gaze_source is "independent".

    // Each tick function returns true if it published a valid sample.
    bool tick_independent(uint64_t now);
    bool tick_combined(uint64_t now);
    void publish(double lx, double ly, double rx, double ry, uint64_t now);

    std::optional<double> ipd_override_;
    uint64_t setup_connection_ = 0;
    uint64_t action_set_ = 0, action_ = 0;
    std::array<SignalId, 4> ids_{};   // The order is LeftEyeX, LeftEyeY, RightEyeX, RightEyeY.

    RateStat samples_;
    Counter invalid_, reconnects_, fallback_;
    double max_deg_ = 45, stale_ns_ = 5e8;
    bool flip_y_ = false;
    gaze::Scaling scaling_ = gaze::Scaling::Angle;
    gaze::Offsets off_;
    bool stats_registered_ = false;
};

}
