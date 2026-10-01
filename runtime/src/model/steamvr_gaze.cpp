#include "model/steamvr_gaze.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/eye_server_client.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace {
const babble::log::Logger LOG("pipe.gaze");
} // namespace

namespace babble {

namespace gaze {

namespace {

// Applies the roll, yaw and pitch offsets to a head-space vector (forward = +z here).
void rotate(double &x, double &y, double &z, const Offsets &off) {
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
}

// Limits an angle to +-max_deg and scales it for the module.
double scale(double a, double max_deg, Scaling scaling) {
    const double max_rad = max_deg * M_PI / 180;
    a = std::max(-max_rad, std::min(max_rad, a));
    return scaling == Scaling::Tangent ? std::tan(a) : a / max_rad;
}

} // namespace

std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling) {
    if (std::sqrt(x * x + y * y + z * z) < 0.01)
        return std::nullopt;
    z = std::fabs(z);   // forward is -z in OpenVR; eyes never look backwards
    if (flip_y)
        y = -y;
    rotate(x, y, z, off);
    const double half_ipd = ipd / 2;
    return std::array<double, 3>{scale(std::atan2(x + half_ipd, z), max_deg, scaling),
                                 scale(std::atan2(x - half_ipd, z), max_deg, scaling),
                                 scale(std::atan2(y, z), max_deg, scaling)};
}

std::optional<std::array<double, 2>> from_direction(double x, double y, double z, double max_deg, bool flip_y,
                                                    const Offsets &off, Scaling scaling) {
    if (std::sqrt(x * x + y * y + z * z) < 1e-6)
        return std::nullopt;
    z = std::fabs(z);
    if (flip_y)
        y = -y;
    rotate(x, y, z, off);
    return std::array<double, 2>{scale(std::atan2(x, z), max_deg, scaling), scale(std::atan2(y, z), max_deg, scaling)};
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

namespace {

std::optional<bool> parse_gaze_source(const nlohmann::json &v) {
    if (!v.is_string())
        return std::nullopt;
    if (v == "independent")
        return true;
    if (v == "combined")
        return false;
    return std::nullopt;
}

} // namespace

bool SteamVrGaze::init() {
    if (!deps_.steamvr) {
        LOG.error("pipeline %s: needs the SteamVR client", name_.c_str());
        return false;
    }
    if (cfg_.contains("ipd") && cfg_["ipd"].is_number())
        ipd_override_ = cfg_["ipd"].get<double>();
    const std::string scaling = cfg_.value("scaling", "angle");
    if (scaling != "tangent" && scaling != "angle") {
        LOG.error("pipeline %s: scaling must be tangent or angle", name_.c_str());
        return false;
    }
    auto source = parse_gaze_source(cfg_.value("gaze_source", nlohmann::json("independent")));
    if (!source) {
        LOG.error("pipeline %s: gaze_source must be independent or combined", name_.c_str());
        return false;
    }
    independent_ = *source;
    scaling_ = scaling == "tangent" ? gaze::Scaling::Tangent : gaze::Scaling::Angle;
    max_deg_ = cfg_.value("max_deg", 45.0);
    flip_y_ = cfg_.value("flip_y", false);
    stale_ns_ = cfg_.value("stale_s", 0.5) * 1e9;
    off_ = {cfg_.value("roll_offset", 0.0), cfg_.value("yaw_offset", 0.0), cfg_.value("pitch_offset", 0.0)};

    const char *names[4] = {"LeftEyeX", "LeftEyeY", "RightEyeX", "RightEyeY"};
    for (int i = 0; i < 4; i++)
        ids_[i] = deps_.state.add_signal(names[i]);
    const std::string p = "pipe." + name_ + ".";
    deps_.stats.add(p + "samples_per_s", &samples_);
    deps_.stats.add(p + "invalid", &invalid_);
    deps_.stats.add(p + "reconnects", &reconnects_);
    deps_.stats.add(p + "fallback", &fallback_);
    stats_registered_ = true;
    LOG.info("pipeline %s: SteamVR eye gaze at %.0f Hz, %s, %s scaling", name_.c_str(), cfg_.value("rate_hz", 90.0),
             independent_ ? "independent per-eye" : "combined", scaling.c_str());
    return true;
}

bool SteamVrGaze::set_option(const std::string &key, const nlohmann::json &value) {
    if (key != "gaze_source")
        return false;
    auto source = parse_gaze_source(value);
    if (!source)
        return false;
    if (*source != independent_.exchange(*source))
        LOG.info("pipeline %s: gaze source %s", name_.c_str(), *source ? "independent per-eye" : "combined");
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
    if (deps_.eye_server)
        deps_.eye_server->want(EyeServerClient::Gaze, false);
}

void SteamVrGaze::publish(double lx, double ly, double rx, double ry, uint64_t now) {
    std::pair<SignalId, Signal> u[4] = {{ids_[0], {float(lx), 1.0f, now}},
                                        {ids_[1], {float(ly), 1.0f, now}},
                                        {ids_[2], {float(rx), 1.0f, now}},
                                        {ids_[3], {float(ry), 1.0f, now}}};
    deps_.state.update(u);
}

// Each eye's own direction from the eye-service shared memory.
bool SteamVrGaze::tick_independent(uint64_t now) {
    auto s = deps_.eye_server->latest(uint64_t(stale_ns_));
    if (!s)
        return false;
    const auto &l = s->pre_fusion_gaze[eye_server::kLeftEye], &r = s->pre_fusion_gaze[eye_server::kRightEye];
    auto gl = gaze::from_direction(l[0], l[1], l[2], max_deg_, flip_y_, off_, scaling_);
    auto gr = gaze::from_direction(r[0], r[1], r[2], max_deg_, flip_y_, off_, scaling_);
    if (!gl || !gr)
        return false;
    publish((*gl)[0], (*gl)[1], (*gr)[0], (*gr)[1], now);
    samples_.tick();
    return true;
}

// SteamVR's fused gaze through IVRInput, split per eye by the IPD.
bool SteamVrGaze::tick_combined(uint64_t now) {
    SteamVrClient &vrc = *deps_.steamvr;
    if (!vrc.connected())
        return false;
    if (setup_connection_ != vrc.connection()) {
        setup_connection_ = vrc.connection();
        reconnects_.add();
    }
    action_ = vrc.gaze_action();
    action_set_ = vrc.action_set();
    if (!action_)   // the client's input manifest isn't ready (yet)
        return false;
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
    if (!ok) {
        invalid_.add();
        return false;
    }
    // World -> head: p = R^T (target - position).
    const auto &m = hmd.mDeviceToAbsoluteTracking.m;
    const double t[3] = {d.vGazeTarget.v[0] - m[0][3], d.vGazeTarget.v[1] - m[1][3], d.vGazeTarget.v[2] - m[2][3]};
    double p[3];
    for (int c = 0; c < 3; c++)
        p[c] = m[0][c] * t[0] + m[1][c] * t[1] + m[2][c] * t[2];
    auto g = gaze::convert(p[0], p[1], p[2], ipd, max_deg_, flip_y_, off_, scaling_);
    if (!g)
        return false;
    const auto &[lx, rx, y] = *g;
    publish(lx, y, rx, y, now);
    samples_.tick();
    return true;
}

void SteamVrGaze::run() {
    const uint64_t period = uint64_t(1e9 / std::max(1.0, cfg_.value("rate_hz", 90.0)));
    uint64_t last_valid = 0;
    bool centred = true;
    enum class Using { None, Independent, Combined } using_ = Using::None;

    while (!stop_) {
        const uint64_t now = mono_ns();
        // The eye-service reader only runs while independent gaze is wanted.
        const bool want_independent = tracking_ && independent_ && deps_.eye_server;
        if (deps_.eye_server)
            deps_.eye_server->want(EyeServerClient::Gaze, want_independent);
        if (!tracking_) {   // eye feed turned off by the user
            if (!centred) {
                publish(0, 0, 0, 0, now);
                centred = true;
            }
            sleep_ns(100000000);
            continue;
        }

        Using used = Using::None;
        if (want_independent && tick_independent(now))
            used = Using::Independent;
        else if (tick_combined(now))
            used = Using::Combined;
        if (want_independent && used != Using::Independent)
            fallback_.add();

        if (used != Using::None) {
            if (used != using_ && independent_)
                LOG.info("pipeline %s: %s", name_.c_str(),
                         used == Using::Independent ? "independent per-eye gaze"
                                                    : "independent gaze unavailable; using SteamVR's combined gaze");
            using_ = used;
            last_valid = now;
            centred = false;
        } else if (!centred && now - last_valid > uint64_t(stale_ns_)) {   // back to centre once, like the reference
            publish(0, 0, 0, 0, now);
            centred = true;
        }
        sleep_ns(period);
    }
}

} // namespace babble
