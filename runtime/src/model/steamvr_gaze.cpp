#include "model/steamvr_gaze.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace babble {

namespace gaze {

std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling) {
    if (std::sqrt(x * x + y * y + z * z) < 0.01)
        return std::nullopt;
    z = std::fabs(z);   // forward is -z in OpenVR; eyes never look backwards
    if (flip_y)
        y = -y;
    const double ro = off.roll_deg * M_PI / 180, yo = off.yaw_deg * M_PI / 180, po = off.pitch_deg * M_PI / 180;
    const double cr = std::cos(ro), sr = std::sin(ro), cy = std::cos(yo), sy = std::sin(yo), cp = std::cos(po),
                 sp = std::sin(po);
    double nx = x * cr + y * sr, ny = y * cr - x * sr;
    x = nx, y = ny;
    nx = x * cy + z * sy;
    double nz = z * cy - x * sy;
    x = nx, z = nz;
    ny = y * cp + z * sp;
    nz = z * cp - y * sp;
    y = ny, z = nz;
    const double max_rad = max_deg * M_PI / 180, half_ipd = ipd / 2;
    auto norm = [&](double a) {
        a = std::max(-max_rad, std::min(max_rad, a));
        return scaling == Scaling::Tangent ? std::tan(a) : a / max_rad;
    };
    return std::array<double, 3>{norm(std::atan2(x + half_ipd, z)), norm(std::atan2(x - half_ipd, z)),
                                 norm(std::atan2(y, std::hypot(x, z)))};
}

} // namespace gaze

SteamVrGaze::SteamVrGaze(const nlohmann::json &cfg, PipelineDeps deps)
    : name_(deps.name), cfg_(cfg), deps_(std::move(deps)), feed_(cfg.value("feed", "eye")) {
    std::string key = cfg_.value("thread", "gaze");
    thread_spec_ = thread_spec(deps_.threads, key, ("bbl-" + key).c_str());
}

SteamVrGaze::~SteamVrGaze() {
    stop();
    if (stats_registered_)
        deps_.stats.remove_prefix("pipe." + name_ + ".");
}

bool SteamVrGaze::init() {
    if (!deps_.steamvr) {
        LOG_ERROR("pipeline %s: needs the SteamVR client", name_.c_str());
        return false;
    }
    if (cfg_.contains("ipd") && cfg_["ipd"].is_number())
        ipd_override_ = cfg_["ipd"].get<double>();
    const char *names[4] = {"LeftEyeX", "LeftEyeY", "RightEyeX", "RightEyeY"};
    for (int i = 0; i < 4; i++)
        ids_[i] = deps_.state.add_signal(names[i]);
    const std::string p = "pipe." + name_ + ".";
    deps_.stats.add(p + "samples_per_s", &samples_);
    deps_.stats.add(p + "invalid", &invalid_);
    deps_.stats.add(p + "reconnects", &reconnects_);
    stats_registered_ = true;
    const std::string scaling = cfg_.value("scaling", "tangent");
    if (scaling != "tangent" && scaling != "angle") {
        LOG_ERROR("pipeline %s: scaling must be tangent or angle", name_.c_str());
        return false;
    }
    LOG_INFO("pipeline %s: SteamVR eye gaze at %.0f Hz, %s scaling", name_.c_str(), cfg_.value("rate_hz", 90.0),
             scaling.c_str());
    return true;
}

void SteamVrGaze::start() {
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void SteamVrGaze::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void SteamVrGaze::run() {
    const uint64_t period = uint64_t(1e9 / std::max(1.0, cfg_.value("rate_hz", 90.0)));
    const uint64_t stale_ns = uint64_t(cfg_.value("stale_s", 0.5) * 1e9);
    const double max_deg = cfg_.value("max_deg", 45.0);
    const bool flip_y = cfg_.value("flip_y", false);
    const gaze::Scaling scaling =
        cfg_.value("scaling", "tangent") == "angle" ? gaze::Scaling::Angle : gaze::Scaling::Tangent;
    const gaze::Offsets off{cfg_.value("roll_offset", 0.0), cfg_.value("yaw_offset", 0.0),
                            cfg_.value("pitch_offset", 0.0)};
    uint64_t last_valid = 0;
    bool centred = true;
    SteamVrClient &vrc = *deps_.steamvr;

    auto centre = [&](uint64_t now) {
        std::pair<SignalId, Signal> u[4] = {{ids_[0], {0.f, 0.f, now}}, {ids_[1], {0.f, 0.f, now}},
                                            {ids_[2], {0.f, 0.f, now}}, {ids_[3], {0.f, 0.f, now}}};
        deps_.state.update(u);
        centred = true;
    };

    while (!stop_) {
        const uint64_t now = mono_ns();
        if (!tracking_) {   // eye feed turned off by the user
            if (!centred)
                centre(now);
            sleep_ns(100000000);
            continue;
        }
        if (!vrc.connected()) {
            sleep_ns(100000000);
            continue;
        }
        if (setup_connection_ != vrc.connection()) {
            setup_connection_ = vrc.connection();
            reconnects_.add();
        }
        action_ = vrc.gaze_action();
        action_set_ = vrc.action_set();
        if (!action_) {   // the client's input manifest isn't ready (yet)
            sleep_ns(200000000);
            continue;
        }
        const double ipd = ipd_override_ ? *ipd_override_ : (vrc.ipd() > 0 ? vrc.ipd() : 0.063);

        vr::VREyeTrackingData_t d{};
        vr::TrackedDevicePose_t hmd{};
        bool ok = false;
        vrc.with_vr([&] {
            vr::VRActiveActionSet_t active{};
            active.ulActionSet = action_set_;
            vr::VRInput()->UpdateActionState(&active, sizeof(active), 1);
            ok = vr::VRInput()->GetEyeTrackingDataRelativeToNow(action_, vr::TrackingUniverseStanding, 0.f, &d,
                                                                sizeof(d)) == vr::VRInputError_None &&
                 d.bValid && d.bTracked;
            if (ok) {
                vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, &hmd, 1);
                ok = hmd.bPoseIsValid;
            }
        });
        if (ok) {
            // World -> head: p = R^T (target - position).
            const auto &m = hmd.mDeviceToAbsoluteTracking.m;
            const double t[3] = {d.vGazeTarget.v[0] - m[0][3], d.vGazeTarget.v[1] - m[1][3],
                                 d.vGazeTarget.v[2] - m[2][3]};
            double p[3];
            for (int c = 0; c < 3; c++)
                p[c] = m[0][c] * t[0] + m[1][c] * t[1] + m[2][c] * t[2];
            if (auto g = gaze::convert(p[0], p[1], p[2], ipd, max_deg, flip_y, off, scaling)) {
                const auto &[lx, rx, y] = *g;
                std::pair<SignalId, Signal> u[4] = {{ids_[0], {float(lx), 1.0f, now}},
                                                    {ids_[1], {float(y), 1.0f, now}},
                                                    {ids_[2], {float(rx), 1.0f, now}},
                                                    {ids_[3], {float(y), 1.0f, now}}};
                deps_.state.update(u);
                samples_.tick();
                last_valid = now;
                centred = false;
            }
        } else {
            invalid_.add();
            if (!centred && now - last_valid > stale_ns)   // back to centre once, like the reference
                centre(now);
        }
        sleep_ns(period);
    }
}

} // namespace babble
