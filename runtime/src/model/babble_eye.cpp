#include "model/babble_eye.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/image_ops.h"
#include "platform/eye_server_client.h"

#include <algorithm>
#include <cmath>

namespace {
const babble::log::Logger LOG("pipe.eye");
}

namespace babble {

BabbleEye::~BabbleEye() { stop(); }

bool BabbleEye::init() {
    if (!bind_input())
        return false;

    if (cfg_.value("pupil_model", std::string()).empty()) {
        LOG.error("pipeline %s: pupil_model (eye kit stage 2) is not set", name_.c_str());
        return false;
    }
    kit_ = std::make_unique<EyeKit>();
    if (!kit_->load(model_path("model"), model_path("pupil_model"), cfg_.value("pupil_crop_px", 128u)))
        return false;
    widen_gain_ = cfg_.value("kit_widen_gain", widen_gain_);
    const std::string p = "pipe." + name_ + ".";
    deps_.stats.add(p + "pupil_preprocess", &pupil_preprocess_);
    deps_.stats.add(p + "pupil_inference", &pupil_inference_);

    const double hz = cfg_.value("update_hz", 90.0);
    tuned_ = eye::RateScale{hz, hz};
    update_rate_ = eye::UpdateRate(hz);
    use_calib_ = cfg_.value("calib", true);
    {
        const std::string src = cfg_.value("openness_source", "model");
        if (src != "model" && src != "native") {
            LOG.error("pipeline %s: openness_source must be model or native", name_.c_str());
            return false;
        }
        native_openness_ = src == "native";
        deps_.stats.add("pipe." + name_ + ".native_fallback", &native_fallback_);
    }
    babble_swap_ = cfg_.value("babble_swap", false);
    combine_widen_ = cfg_.value("combine_widen", true);
    combine_dilation_ = cfg_.value("combine_dilation", true);
    // The eyes use their own One Euro filters, which are tuned apart from the mouth filters.
    lid_filter_ = one_euro_params(cfg_.value("one_euro", nlohmann::json::object()), eye::kLidOneEuro);
    // The eye kit gives the pupil in mm, so its filter and rate limit are in mm.
    pupil_filter_ = one_euro_params(cfg_.value("pupil_one_euro_mm", nlohmann::json::object()), eye::kPupilOneEuroMm);
    pupil_gain_ = cfg_.value("pupil_gain", 2.0);
    {
        const std::string mode = cfg_.value("lid_calibration", "baseline");
        if (mode != "baseline" && mode != "median") {
            LOG.error("pipeline %s: lid_calibration must be baseline or median", name_.c_str());
            return false;
        }
        baseline_calib_ = mode == "baseline";
    }

    start_session();

    const char *names[3] = {"Lid", "Widen", "Dilation"};
    for (int k = 0; k < 3; k++) {
        left_ids_[k] = deps_.state.add_signal(std::string("LeftEye") + names[k]);
        right_ids_[k] = deps_.state.add_signal(std::string("RightEye") + names[k]);
    }
    // The lid value before calibration is for the debug overlay.
    left_raw_id_ = deps_.state.add_signal("_LeftEyeLidRaw");
    right_raw_id_ = deps_.state.add_signal("_RightEyeLidRaw");
    // These signals show the neutral and wide ceiling of the baseline calibrator as raw values.
    const char *cal_names[6] = {"_LeftEyeNeutral",  "_LeftEyeWideRaw",  "_LeftEyeClosedRaw",
                                "_RightEyeNeutral", "_RightEyeWideRaw", "_RightEyeClosedRaw"};
    for (int i = 0; i < 6; i++)
        cal_ids_[i] = deps_.state.add_signal(cal_names[i]);
    mm_ids_[0] = deps_.state.add_signal("_LeftEyePupilMm");
    mm_ids_[1] = deps_.state.add_signal("_RightEyePupilMm");

    flip_.resize(400 * 400);
    LOG.info("pipeline %s: eye kit, lid + pupil centre (%u px)%s and pupil mm from a %u px crop (%u px), %s, on %s, "
             "filters tuned at %.0f updates/s",
             name_.c_str(), kit_->res(), !kit_->has_widen() ? "" : widen_gain_ > 0 ? " + Widen head" : " (Widen head off)",
             cfg_.value("pupil_crop_px", 128u), kit_->crop_res(), kit_->fp16() ? "fp16" : "fp32", source_.c_str(), hz);
    return true;
}

// Make the per-person state again at init and when the wearer puts the headset back on.
void BabbleEye::start_session() {
    calib_.clear();
    openness_.clear();
    trackers_.clear();
    for (int e = 0; e < 2; e++) {
        openness_.emplace_back(openness_params(), lid_filter_);
        calib_.emplace_back(tuned_.count(cfg_.value("calib_window_s", 20.0) * tuned_.ref_hz),
                            tuned_.count(cfg_.value("calib_warmup_s", 3.0) * tuned_.ref_hz), 0.5, 50, tuned_.count(30));
        trackers_.emplace_back(cfg_.value("pupil_gate", 0.4), cfg_.value("pupil_gate_hys", 0.1),
                               int(tuned_.count(cfg_.value("pupil_hold_s", 0.1) * tuned_.ref_hz)),
                               pupil_filter_, cfg_.value("pupil_rate_mm", 0.06), 1, 1);
        lid_euro_[e].reset();
        if (lid_filter_)
            lid_euro_[e].emplace(1, *lid_filter_);
    }
    set_pupil_windows();
    last_capture_ns_ = 0;
    update_rate_.restart();   // The gap before this session is not an update interval.
}

// Set the pupil baseline window and the minimum samples for the first baseline, in pupil samples.
void BabbleEye::set_pupil_windows() {
    const size_t window = tuned_.count(cfg_.value("pupil_base_s", 60.0) * tuned_.ref_hz);
    const size_t min = tuned_.count(tuned_.ref_hz);
    for (auto &t : trackers_)
        t.set_base_window(window, min);
}

// Get the baseline calibrator settings, with the update counts for the current rate.
eye::OpennessCalibrator::Params BabbleEye::openness_params() const {
    eye::OpennessCalibrator::Params p;
    p.skip = tuned_.count(1.0 * tuned_.ref_hz);
    p.warmup = tuned_.count(cfg_.value("calib_warmup_s", 2.0) * tuned_.ref_hz);
    p.every = tuned_.count(12);
    p.peak_window = tuned_.count(9);
    p.trough_window = tuned_.count(5);
    p.closed_seed = std::clamp(cfg_.value("closed_seed", p.closed_seed), 0.0, 0.9);
    p.tol = cfg_.value("calib_tol", p.tol);
    p.widen_seed = std::max(0.01, cfg_.value("widen_seed", p.widen_seed));
    p.ceiling_max = std::max(1.0 + p.widen_seed, cfg_.value("widen_ceiling_max", p.ceiling_max));
    p.saturation = cfg_.value("lid_saturation", p.saturation);
    return p;
}

// Convert the frame-count constants for a new update rate and keep the learned state.
void BabbleEye::retune(double hz) {
    tuned_.hz = hz;
    for (auto &c : calib_)
        c.set_timing(tuned_.count(cfg_.value("calib_window_s", 20.0) * tuned_.ref_hz),
                     tuned_.count(cfg_.value("calib_warmup_s", 3.0) * tuned_.ref_hz), tuned_.count(30));
    for (auto &t : trackers_)
        t.set_hold(int(tuned_.count(cfg_.value("pupil_hold_s", 0.1) * tuned_.ref_hz)));
    const auto op = openness_params();
    for (auto &c : openness_)
        c.set_timing(op.skip, op.warmup, op.every, op.peak_window, op.trough_window);
    set_pupil_windows();
    LOG.info("pipeline %s: eye filters tuned for %.0f updates/s", name_.c_str(), hz);
}

bool BabbleEye::set_option(const std::string &key, const nlohmann::json &value) {
    if ((key == "combine_widen" || key == "combine_dilation") && value.is_boolean()) {
        auto &flag = key == "combine_widen" ? combine_widen_ : combine_dilation_;
        if (value.get<bool>() != flag.exchange(value.get<bool>()))
            LOG.info("pipeline %s: %s %s", name_.c_str(), key.c_str(), value.get<bool>() ? "on" : "off");
        return true;
    }
    if (key != "openness_source" || !value.is_string() || (value != "model" && value != "native"))
        return false;
    const bool native = value == "native";
    if (native != native_openness_.exchange(native)) {
        reset_pending_ = true;   // The two sources have different scales, so learn the lid reference again.
        LOG.info("pipeline %s: openness from %s", name_.c_str(), native ? "Valve's eye tracker" : "the model");
    }
    update_eye_server_want();
    return true;
}

void BabbleEye::set_tracking(bool on) {
    tracking_on_ = on;
    update_eye_server_want();
    PipelineBase::set_tracking(on);
}

void BabbleEye::update_eye_server_want() {
    if (deps_.eye_server)
        deps_.eye_server->want(EyeServerClient::Openness, native_openness_ && tracking_on_);
}

std::vector<std::pair<SignalId, Signal>> BabbleEye::neutral(uint64_t now) const {
    // Send open, relaxed eyes (the VRCFT defaults) and keep the last dilation value.
    return {{left_ids_[0], {1.0f, 0.0f, now}}, {left_ids_[1], {0.0f, 0.0f, now}},
            {right_ids_[0], {1.0f, 0.0f, now}}, {right_ids_[1], {0.0f, 0.0f, now}}};
}

std::array<img::View, 2> BabbleEye::canonical(const StereoFrame &sf) {
    const Frame *frames[2] = {sf.left.get(), sf.right.get()};
    std::array<img::View, 2> views;
    for (int v = 0; v < 2; v++) {
        const Frame &f = *frames[v];
        views[v] = {f.data, f.width, f.height, f.stride};
        if (v == 1) {   // For the "R" pane, rot90(2) and a mirror are the same as a vertical flip.
            if (flip_.size() < size_t(f.width) * f.height)   // Use the frame size, not the default crop.
                flip_.resize(size_t(f.width) * f.height);
            img::flip_vertical(views[v], {flip_.data(), f.width, f.height, f.width});
            views[v] = {flip_.data(), f.width, f.height, f.width};
        }
    }
    return views;
}

void BabbleEye::on_start() { update_eye_server_want(); }

void BabbleEye::on_stop() {
    if (deps_.eye_server)
        deps_.eye_server->want(EyeServerClient::Openness, false);
}

void BabbleEye::process(const StereoFrame &sf) {
    if (reset_pending_.exchange(false))
        start_session();
    // Retune after a 10% change in the real update rate, but not more than one time each second.
    update_rate_.tick(sf.capture_ns);
    // The One Euro filters use dt in seconds, which is 0 for the first update.
    const double dt = last_capture_ns_ && sf.capture_ns > last_capture_ns_
                          ? double(sf.capture_ns - last_capture_ns_) / 1e9
                          : 0.0;
    last_capture_ns_ = sf.capture_ns;
    {
        const double measured = update_rate_.hz();
        const double hz = std::fabs(measured - tuned_.ref_hz) < 0.03 * tuned_.ref_hz ? tuned_.ref_hz : measured;
        const uint64_t now = mono_ns();
        if (hz != tuned_.hz && (hz == tuned_.ref_hz || std::fabs(hz - tuned_.hz) > 0.1 * tuned_.hz) &&
            now - last_retune_ns_ > 1000000000ull) {
            retune(hz);
            last_retune_ns_ = now;
        }
    }
    EyeKit::Result kr;
    EyeKit::Timing kt;
    const uint64_t t0 = mono_ns();
    const auto views = canonical(sf);
    if (!kit_->run(views, kr, &kt))
        return;
    preprocess_.add(mono_ns() - t0 - kt.infer1 - kt.prep2 - kt.infer2);   // The flip and the stage 1 resize.
    inference_.add(kt.infer1);
    pupil_preprocess_.add(kt.prep2);
    pupil_inference_.add(kt.infer2);
    std::array<double, 2> lid = kr.lid;
    const std::array<double, 2> &pupil = kr.mm;
    std::optional<std::array<double, 2>> head_widen;   // The output of the Widen head of the eye kit.
    if (kit_->has_widen() && widen_gain_ > 0)
        head_widen = kr.widen;
    if (native_openness_ && deps_.eye_server) {
        // Index e=0 is view 2, which is the right eye of the wearer.
        if (auto s = deps_.eye_server->latest(200000000ull))
            lid = {s->openness[eye_server::kRightEye], s->openness[eye_server::kLeftEye]};
        else
            native_fallback_.add();
    }
    double o[2], lid_out[2], widen[2];
    std::optional<double> dil[2];
    for (int e = 0; e < 2; e++) {
        const double raw = lid[e];
        if (use_calib_ && baseline_calib_) {
            // The OpennessCalibrator applies its own filter.
            const auto r = openness_[e].update(raw, dt);
            o[e] = r.openness;
            lid_out[e] = r.lid;
            widen[e] = r.widen;
            continue;
        }
        // The reference uses a rolling-median neutral, and Widen is the part above it.
        double v = use_calib_ ? calib_[e].update(raw) : raw;
        if (lid_euro_[e])
            v = lid_euro_[e]->apply(v, dt);
        o[e] = std::max(v, 0.0);
        lid_out[e] = std::min(o[e], 1.0);
        widen[e] = std::clamp(o[e] - 1.0, 0.0, 1.0);
    }
    if (head_widen)   // The Widen head replaces the Widen from the openness.
        for (int e = 0; e < 2; e++)
            widen[e] = std::clamp((*head_widen)[e] / widen_gain_, 0.0, 1.0);
    std::optional<double> shown_mm[2];
    for (int e = 0; e < 2; e++) {
        // The tracker gates the pupil on the calibrated openness and holds a recent value through blinks.
        auto r = trackers_[e].update(o[e], pupil[e], tuned_.k(), dt);
        dil[e] = eye::dilation(r, pupil_gain_);
        shown_mm[e] = r.px;
    }
    if (combine_widen_.load(std::memory_order_relaxed))
        widen[0] = widen[1] = (widen[0] + widen[1]) / 2;
    if (combine_dilation_.load(std::memory_order_relaxed)) {
        // If only one eye has a pupil value yet, both eyes use it.
        if (dil[0] && dil[1])
            dil[0] = dil[1] = (*dil[0] + *dil[1]) / 2;
        else
            dil[0] = dil[1] = dil[0] ? dil[0] : dil[1];
    }

    // The module sides follow the avatar, so by default "Left" is view 3 and "Right" is view 2.
    const int left = babble_swap_ ? 0 : 1, right = babble_swap_ ? 1 : 0;
    std::vector<std::pair<SignalId, Signal>> updates;
    auto emit = [&](const std::array<SignalId, 3> &ids, int e) {
        updates.emplace_back(ids[0], Signal{float(lid_out[e]), 1.0f, sf.capture_ns});
        updates.emplace_back(ids[1], Signal{float(widen[e]), 1.0f, sf.capture_ns});
        if (dil[e])   // Send no dilation until the pupil baseline exists, as the reference does.
            updates.emplace_back(ids[2], Signal{float(std::clamp(*dil[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
    };
    emit(left_ids_, left);
    emit(right_ids_, right);
    updates.emplace_back(left_raw_id_, Signal{float(lid[left]), 1.0f, sf.capture_ns});
    updates.emplace_back(right_raw_id_, Signal{float(lid[right]), 1.0f, sf.capture_ns});
    if (shown_mm[left])
        updates.emplace_back(mm_ids_[0], Signal{float(*shown_mm[left]), 1.0f, sf.capture_ns});
    if (shown_mm[right])
        updates.emplace_back(mm_ids_[1], Signal{float(*shown_mm[right]), 1.0f, sf.capture_ns});
    if (use_calib_ && baseline_calib_) {
        for (int k = 0; k < 2; k++) {
            const auto &c = openness_[k == 0 ? left : right];
            if (!c.ready())
                continue;
            updates.emplace_back(cal_ids_[3 * k], Signal{float(c.baseline()), 1.0f, sf.capture_ns});
            updates.emplace_back(cal_ids_[3 * k + 1], Signal{float(c.baseline() * c.ceiling()), 1.0f, sf.capture_ns});
            updates.emplace_back(cal_ids_[3 * k + 2], Signal{float(c.baseline() * c.closed()), 1.0f, sf.capture_ns});
        }
    }
    publish(updates);
}

}
