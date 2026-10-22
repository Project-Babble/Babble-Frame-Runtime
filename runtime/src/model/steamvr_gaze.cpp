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
}

namespace babble {

namespace gaze {

namespace {

// Apply the roll, yaw and pitch offsets to a head-space vector with forward along +z.
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

// Limit an angle to +-max_deg and scale it for the module.
double scale(double a, double max_deg, Scaling scaling) {
    const double max_rad = max_deg * M_PI / 180;
    a = std::max(-max_rad, std::min(max_rad, a));
    return scaling == Scaling::Tangent ? std::tan(a) : a / max_rad;
}

}

std::optional<std::array<double, 3>> convert(double x, double y, double z, double ipd, double max_deg, bool flip_y,
                                             const Offsets &off, Scaling scaling) {
    if (std::sqrt(x * x + y * y + z * z) < 0.01)
        return std::nullopt;
    z = std::fabs(z);   // Use the absolute value because an eye never looks backwards.
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

}

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

}

void gaze::Smoother::apply(const float raw[4], float out[4], int level, uint64_t now_ns) {
    std::copy(raw, raw + 4, out);
    if (level <= 0 || level > levels()) {
        level_ = 0;
        return;
    }
    if (level_ != level) {   // The first sample of a new filter does not change.
        filter_ = OneEuro(4, presets_[size_t(level - 1)]);
        level_ = level;
        filter_.apply(out, 4, 0);
    } else {
        filter_.apply(out, 4, double(now_ns - last_ns_) * 1e-9);
    }
    last_ns_ = now_ns;
}

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
    std::vector<OneEuroParams> presets;
    for (const auto &p : cfg_.value("smoothing_presets", nlohmann::json::array()))
        if (auto params = one_euro_params(p, OneEuroParams{}))
            presets.push_back(*params);
    smoother_ = gaze::Smoother(std::move(presets));
    const int level = cfg_.value("smoothing", 0);
    if (level < 0 || level > smoother_.levels()) {
        LOG.error("pipeline %s: smoothing must be 0 to %d", name_.c_str(), smoother_.levels());
        return false;
    }
    smoothing_ = level;

    const char *names[4] = {"LeftEyeX", "LeftEyeY", "RightEyeX", "RightEyeY"};
    for (int i = 0; i < 4; i++)
        ids_[i] = deps_.state.add_signal(names[i]);
    const std::string p = "pipe." + name_ + ".";
    deps_.stats.add(p + "samples_per_s", &samples_);
    deps_.stats.add(p + "invalid", &invalid_);
    deps_.stats.add(p + "reconnects", &reconnects_);
    deps_.stats.add(p + "fallback", &fallback_);
    stats_registered_ = true;
    LOG.info("pipeline %s: SteamVR eye gaze at %.0f Hz, %s, %s scaling, smoothing %d", name_.c_str(),
             cfg_.value("rate_hz", 90.0), independent_ ? "independent per-eye" : "combined", scaling.c_str(), level);
    return true;
}

bool SteamVrGaze::set_option(const std::string &key, const nlohmann::json &value) {
    if (key == "smoothing") {
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > smoother_.levels())
            return false;
        if (value.get<int>() != smoothing_.exchange(value.get<int>()))
            LOG.info("pipeline %s: gaze smoothing %d", name_.c_str(), value.get<int>());
        return true;
    }
    if (key != "gaze_source")
        return false;
    auto source = parse_gaze_source(value);
    if (!source)
        return false;
    if (*source != independent_.exchange(*source)) {
        smoothing_reset_ = true;   // The two sources do not line up, so the filter starts again.
        LOG.info("pipeline %s: gaze source %s", name_.c_str(), *source ? "independent per-eye" : "combined");
    }
    return true;
}

void SteamVrGaze::set_listener(std::function<void(const gaze::Sample &)> fn) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    listener_ = std::move(fn);
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


void SteamVrGaze::publish(double lx, double ly, double rx, double ry, uint64_t now, gaze::Sample::Source source) {
    gaze::Sample g;
    g.ns = now;
    g.source = source;
    g.dominant_eye = int8_t(source == gaze::Sample::Source::Independent ? dominant_eye_ : -1);
    g.raw[0] = float(lx);
    g.raw[1] = float(ly);
    g.raw[2] = float(rx);
    g.raw[3] = float(ry);
    if (source == gaze::Sample::Source::Centre) {
        smoothing_reset_ = true;   // The gaze comes back from the centre without a slow glide.
        std::copy(g.raw, g.raw + 4, g.out);
    } else {
        if (smoothing_reset_.exchange(false))
            smoother_.reset();
        g.smoothing = uint8_t(smoothing_.load());
        smoother_.apply(g.raw, g.out, g.smoothing, now);
    }
    std::pair<SignalId, Signal> u[4] = {{ids_[0], {g.out[0], 1.0f, now}},
                                        {ids_[1], {g.out[1], 1.0f, now}},
                                        {ids_[2], {g.out[2], 1.0f, now}},
                                        {ids_[3], {g.out[3], 1.0f, now}}};
    deps_.state.update(u);
    std::lock_guard<std::mutex> lock(listener_mutex_);
    if (listener_)
        listener_(g);
}

// Get the direction of each eye from the eye-service shared memory. While "Track Dominant Eye Only" is on, the gaze
// before fusion still has both eyes, so both eyes get the fused gaze of the tracked eye, as SteamVR apps do.
bool SteamVrGaze::tick_independent(uint64_t now) {
    auto s = deps_.eye_server->latest(uint64_t(stale_ns_));
    if (!s)
        return false;
    if (s->dominant_eye != dominant_eye_) {
        dominant_eye_ = s->dominant_eye;
        if (dominant_eye_ < 0)
            LOG.info("pipeline %s: Track Dominant Eye Only is off; gaze for each eye", name_.c_str());
        else
            LOG.info("pipeline %s: Track Dominant Eye Only is on; both eyes follow the %s eye", name_.c_str(),
                     dominant_eye_ == eye_server::kLeftEye ? "left" : "right");
    }
    const bool one_eye = dominant_eye_ >= 0;
    const auto &l = one_eye ? s->gaze[dominant_eye_] : s->pre_fusion_gaze[eye_server::kLeftEye];
    const auto &r = one_eye ? s->gaze[dominant_eye_] : s->pre_fusion_gaze[eye_server::kRightEye];
    auto gl = gaze::from_direction(l[0], l[1], l[2], max_deg_, flip_y_, off_, scaling_);
    auto gr = gaze::from_direction(r[0], r[1], r[2], max_deg_, flip_y_, off_, scaling_);
    if (!gl || !gr)
        return false;
    publish((*gl)[0], (*gl)[1], (*gr)[0], (*gr)[1], now, gaze::Sample::Source::Independent);
    samples_.tick();
    return true;
}

// Get the fused SteamVR gaze through IVRInput and split it for each eye with the IPD.
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
    if (!action_)   // The input manifest of the client is not ready.
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
    // Move the target from world space to head space with p = R^T (target - position).
    const auto &m = hmd.mDeviceToAbsoluteTracking.m;
    const double t[3] = {d.vGazeTarget.v[0] - m[0][3], d.vGazeTarget.v[1] - m[1][3], d.vGazeTarget.v[2] - m[2][3]};
    double p[3];
    for (int c = 0; c < 3; c++)
        p[c] = m[0][c] * t[0] + m[1][c] * t[1] + m[2][c] * t[2];
    auto g = gaze::convert(p[0], p[1], p[2], ipd, max_deg_, flip_y_, off_, scaling_);
    if (!g)
        return false;
    const auto &[lx, rx, y] = *g;
    publish(lx, y, rx, y, now, gaze::Sample::Source::Combined);
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
        // The eye-service reader runs only while the pipeline needs independent gaze.
        const bool want_independent = tracking_ && independent_ && deps_.eye_server;
        if (deps_.eye_server)
            deps_.eye_server->want(EyeServerClient::Gaze, want_independent);
        if (!tracking_) {   // The user turned the eye feed off.
            if (!centred) {
                centre(now);
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
        } else if (!centred && now - last_valid > uint64_t(stale_ns_)) {   // Go back to centre one time, as the reference does.
            centre(now);
            centred = true;
        }
        sleep_ns(period);
    }
}

}
