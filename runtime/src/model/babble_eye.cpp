#include "model/babble_eye.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/half.h"
#include "model/image_ops.h"
#include "platform/eye_server_client.h"

#include <algorithm>
#include <cmath>

namespace {
const babble::log::Logger LOG("pipe.eye");
}

namespace babble {

namespace {

// Make sure that the model has one [2,1,R,R] float input and one output of `outputs` elements.
bool eye_contract(const ExecuTorchRunner &r, size_t outputs, uint32_t *res) {
    auto in = r.inputs();
    auto out = r.outputs();
    if (in.size() != 1 || out.size() != 1 || out[0].elements() != outputs)
        return false;
    const auto &sh = in[0].shape;
    if (sh.size() != 4 || sh[0] != 2 || sh[1] != 1 || sh[2] != sh[3] || sh[2] < 16 || sh[2] > 400)
        return false;
    auto is_float = [](DType d) { return d == DType::F32 || d == DType::F16; };
    if (!is_float(in[0].dtype) || !is_float(out[0].dtype))
        return false;
    *res = uint32_t(sh[2]);
    return true;
}

}

BabbleEye::~BabbleEye() { stop(); }

bool BabbleEye::init() {
    if (!bind_input())
        return false;

    model_ = std::make_unique<ExecuTorchRunner>();
    if (!model_->load(model_path("model")))
        return false;
    if (!eye_contract(*model_, 4, &res_)) {
        LOG.error("pipeline %s: eye model does not match [2,1,R,R] -> [2,2]", name_.c_str());
        return false;
    }
    plane_ = size_t(res_) * res_;

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
    // The eyes use their own One Euro filters, which are tuned apart from the mouth filters.
    lid_filter_ = one_euro_params(cfg_.value("one_euro", nlohmann::json::object()), eye::kLidOneEuro);
    pupil_filter_ = one_euro_params(cfg_.value("pupil_one_euro", nlohmann::json::object()), eye::kPupilOneEuro);
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
    const char *cal_names[4] = {"_LeftEyeNeutral", "_LeftEyeWideRaw", "_RightEyeNeutral", "_RightEyeWideRaw"};
    for (int i = 0; i < 4; i++)
        cal_ids_[i] = deps_.state.add_signal(cal_names[i]);

    x_.resize(2 * plane_);
    flip_.resize(400 * 400);
    small_.resize(plane_);
    LOG.info("pipeline %s: lid + pupil model (%u px, %s) on %s, filters tuned at %.0f updates/s", name_.c_str(),
             res_, model_->inputs()[0].dtype == DType::F16 ? "fp16" : "fp32", source_.c_str(), hz);
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
                               pupil_filter_, cfg_.value("pupil_rate", 0.6), 1, 1);
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
        c.set_timing(op.skip, op.warmup, op.every, op.peak_window);
    set_pupil_windows();
    LOG.info("pipeline %s: eye filters tuned for %.0f updates/s", name_.c_str(), hz);
}

bool BabbleEye::set_option(const std::string &key, const nlohmann::json &value) {
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

void BabbleEye::prepare(const StereoFrame &sf, float *x) {
    const Frame *views[2] = {sf.left.get(), sf.right.get()};
    for (int v = 0; v < 2; v++) {
        const Frame &f = *views[v];
        img::View cur{f.data, f.width, f.height, f.stride};
        if (v == 1) {   // For the "R" pane, rot90(2) and a mirror are the same as a vertical flip.
            if (flip_.size() < size_t(f.width) * f.height)   // Use the frame size, not the default crop.
                flip_.resize(size_t(f.width) * f.height);
            img::flip_vertical(cur, {flip_.data(), f.width, f.height, f.width});
            cur = {flip_.data(), f.width, f.height, f.width};
        }
        img::resize_area(cur, {small_.data(), res_, res_, res_});
        float *dst = x + v * plane_;
        for (size_t i = 0; i < plane_; i++)
            dst[i] = (float(small_[i]) / 255.0f - 0.5f) / 0.25f;
    }
}

bool BabbleEye::run_model(const float *x, float *y) {
    const TensorSpec &is = model_->inputs()[0], &os = model_->outputs()[0];
    const void *in = x;
    if (is.dtype == DType::F16) {
        half_in_.resize(is.elements());
        to_half(x, half_in_.data(), is.elements());
        in = half_in_.data();
    }
    void *out = y;
    if (os.dtype == DType::F16) {
        half_out_.resize(os.elements());
        out = half_out_.data();
    }
    std::vector<TensorView> iv{{const_cast<void *>(in), &is}};
    std::vector<TensorView> ov{{out, &os}};
    if (!model_->run(iv, ov))
        return false;
    if (os.dtype == DType::F16)
        from_half(half_out_.data(), y, os.elements());
    return true;
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
    uint64_t t0 = mono_ns();
    prepare(sf, x_.data());
    uint64_t t1 = mono_ns();

    if (!run_model(x_.data(), out_.data()))
        return;
    uint64_t t2 = mono_ns();
    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);

    std::array<double, 2> lid = {out_[0], out_[2]};
    const std::array<double, 2> pupil = {out_[1], out_[3]};
    if (native_openness_ && deps_.eye_server) {
        // Index e=0 is view 2, which is the right eye of the wearer.
        if (auto s = deps_.eye_server->latest(200000000ull))
            lid = {s->openness[eye_server::kRightEye], s->openness[eye_server::kLeftEye]};
        else
            native_fallback_.add();
    }
    double o[2], widen[2];
    std::optional<double> dil[2];
    for (int e = 0; e < 2; e++) {
        const double raw = lid[e];
        if (use_calib_ && baseline_calib_) {
            // The OpennessCalibrator applies its own filter.
            const auto r = openness_[e].update(raw, dt);
            o[e] = r.openness;
            widen[e] = r.widen;
            continue;
        }
        // The reference uses a rolling-median neutral, and Widen is the part above it.
        double v = use_calib_ ? calib_[e].update(raw) : raw;
        if (lid_euro_[e])
            v = lid_euro_[e]->apply(v, dt);
        o[e] = std::max(v, 0.0);
        widen[e] = std::clamp(o[e] - 1.0, 0.0, 1.0);
    }
    for (int e = 0; e < 2; e++) {
        auto r = trackers_[e].update(o[e], pupil[e], tuned_.k(), dt);
        dil[e] = eye::dilation(r, pupil_gain_);
    }

    // The module sides follow the avatar, so by default "Left" is view 3 and "Right" is view 2.
    const int left = babble_swap_ ? 0 : 1, right = babble_swap_ ? 1 : 0;
    std::vector<std::pair<SignalId, Signal>> updates;
    auto emit = [&](const std::array<SignalId, 3> &ids, int e) {
        updates.emplace_back(ids[0], Signal{float(std::clamp(o[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
        updates.emplace_back(ids[1], Signal{float(widen[e]), 1.0f, sf.capture_ns});
        if (dil[e])   // Send no dilation until the pupil baseline exists, as the reference does.
            updates.emplace_back(ids[2], Signal{float(std::clamp(*dil[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
    };
    emit(left_ids_, left);
    emit(right_ids_, right);
    updates.emplace_back(left_raw_id_, Signal{float(lid[left]), 1.0f, sf.capture_ns});
    updates.emplace_back(right_raw_id_, Signal{float(lid[right]), 1.0f, sf.capture_ns});
    if (use_calib_ && baseline_calib_) {
        for (int k = 0; k < 2; k++) {
            const auto &c = openness_[k == 0 ? left : right];
            if (!c.ready())
                continue;
            updates.emplace_back(cal_ids_[2 * k], Signal{float(c.baseline()), 1.0f, sf.capture_ns});
            updates.emplace_back(cal_ids_[2 * k + 1], Signal{float(c.baseline() * c.ceiling()), 1.0f, sf.capture_ns});
        }
    }
    publish(updates);
}

}
