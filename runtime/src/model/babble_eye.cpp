#include "model/babble_eye.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/half.h"
#include "model/image_ops.h"
#include "platform/eye_server_client.h"

#include <algorithm>
#include <cmath>

namespace babble {

namespace {

// One [2,1,R,R] float input (fp32 or fp16), one output of `outputs` elements.
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

} // namespace

BabbleEye::~BabbleEye() { stop(); }

bool BabbleEye::init() {
    if (!bind_input())
        return false;

    combined_ = !cfg_.value("model", "").empty();
    lid_ = std::make_unique<ExecuTorchRunner>();
    if (!lid_->load(model_path(combined_ ? "model" : "lid_model")))
        return false;
    if (!eye_contract(*lid_, combined_ ? 4 : 2, &res_)) {
        LOG_ERROR("pipeline %s: %s model does not match [2,1,R,R] -> %s", name_.c_str(), combined_ ? "eye" : "lid",
                  combined_ ? "[2,2]" : "[2]");
        return false;
    }
    if (!combined_ && !cfg_.value("pupil_model", "").empty()) {
        pupil_ = std::make_unique<ExecuTorchRunner>();
        uint32_t pupil_res = 0;
        if (!pupil_->load(model_path("pupil_model")) || !eye_contract(*pupil_, 2, &pupil_res) ||
            pupil_res != res_) {
            LOG_WARN("pipeline %s: pupil model unusable; running without dilation", name_.c_str());
            pupil_.reset();
        }
    }
    plane_ = size_t(res_) * res_;

    const double hz = cfg_.value("update_hz", 90.0);
    const double pupil_hz = std::min(cfg_.value("pupil_hz", 3.0), hz);
    tuned_ = eye::RateScale{hz, hz};
    update_rate_ = eye::UpdateRate(hz);
    use_calib_ = cfg_.value("calib", true);
    {
        const std::string src = cfg_.value("openness_source", "model");
        if (src != "model" && src != "native") {
            LOG_ERROR("pipeline %s: openness_source must be model or native", name_.c_str());
            return false;
        }
        native_openness_ = src == "native";
        deps_.stats.add("pipe." + name_ + ".native_fallback", &native_fallback_);
    }
    babble_swap_ = cfg_.value("babble_swap", false);
    lid_alpha_ = cfg_.value("lid_alpha", 0.6);
    pupil_gain_ = cfg_.value("pupil_gain", 2.0);
    {
        const std::string mode = cfg_.value("lid_calibration", "baseline");
        if (mode != "baseline" && mode != "median") {
            LOG_ERROR("pipeline %s: lid_calibration must be baseline or median", name_.c_str());
            return false;
        }
        baseline_calib_ = mode == "baseline";
    }
    pupil_every_ns_ = uint64_t(1e9 / pupil_hz);

    reset_session();

    const char *names[3] = {"Lid", "Widen", "Dilation"};
    for (int k = 0; k < 3; k++) {
        left_ids_[k] = deps_.state.add_signal(std::string("LeftEye") + names[k]);
        right_ids_[k] = deps_.state.add_signal(std::string("RightEye") + names[k]);
    }
    // The lid value before calibration (model or native), for the debug overlay.
    left_raw_id_ = deps_.state.add_signal("_LeftEyeLidRaw");
    right_raw_id_ = deps_.state.add_signal("_RightEyeLidRaw");
    // The baseline calibrator's neutral and wide ceiling, as raw values.
    const char *cal_names[4] = {"_LeftEyeNeutral", "_LeftEyeWideRaw", "_RightEyeNeutral", "_RightEyeWideRaw"};
    for (int i = 0; i < 4; i++)
        cal_ids_[i] = deps_.state.add_signal(cal_names[i]);

    x_.resize(2 * plane_);
    pupil_in_.resize(2 * plane_);
    flip_.resize(400 * 400);
    small_.resize(plane_);
    pupil_thread_spec_ = thread_spec(deps_.threads, cfg_.value("pupil_thread", "pupil"), "bbl-pupil");
    deps_.stats.add("pipe." + name_ + ".pupil_fps", &pupil_rate_);
    deps_.stats.add("pipe." + name_ + ".pupil_inference", &pupil_time_);
    if (combined_)
        LOG_INFO("pipeline %s: combined lid + pupil model (%u px, %s) on %s, filters tuned at %.0f updates/s",
                 name_.c_str(), res_, lid_->inputs()[0].dtype == DType::F16 ? "fp16" : "fp32", source_.c_str(), hz);
    else
        LOG_INFO("pipeline %s: lid%s on %s at %.0f updates/s%s", name_.c_str(), pupil_ ? " + pupil" : "",
                 source_.c_str(), hz, pupil_ ? (", pupil " + std::to_string(int(pupil_hz)) + " Hz").c_str() : "");
    return true;
}

// Per-person state: lid reference, pupil baseline, smoothing. Rebuilt at init
// and for every new session (headset put back on).
void BabbleEye::reset_session() {
    calib_.clear();
    openness_.clear();
    trackers_.clear();
    for (int e = 0; e < 2; e++) {
        openness_.emplace_back(openness_params());
        calib_.emplace_back(tuned_.count(cfg_.value("calib_window_s", 20.0) * tuned_.ref_hz),
                            tuned_.count(cfg_.value("calib_warmup_s", 3.0) * tuned_.ref_hz), 0.5, 50, tuned_.count(30));
        trackers_.emplace_back(cfg_.value("pupil_gate", 0.4), cfg_.value("pupil_gate_hys", 0.1),
                               int(tuned_.count(cfg_.value("pupil_hold_s", 0.1) * tuned_.ref_hz)),
                               cfg_.value("pupil_alpha", 0.2), cfg_.value("pupil_rate", 0.6), 1, 1);
    }
    set_pupil_windows();
    smoothed_ = {};
    update_rate_.restart();   // the gap before this session isn't an update interval
}

// The pupil baseline window (pupil_base_s) and the samples needed before the
// first baseline (1 s worth), in pupil samples: a sample per pupil_hz tick in
// the split layout, a sample per update with the combined model.
void BabbleEye::set_pupil_windows() {
    const double base_s = cfg_.value("pupil_base_s", 60.0);
    size_t window, min;
    if (combined_) {
        window = tuned_.count(base_s * tuned_.ref_hz);
        min = tuned_.count(tuned_.ref_hz);
    } else {
        const double pupil_hz = std::min(cfg_.value("pupil_hz", 3.0), tuned_.ref_hz);
        window = size_t(std::max(10, int(base_s * pupil_hz)));
        min = size_t(std::max(5, int(pupil_hz)));
    }
    for (auto &t : trackers_)
        t.set_base_window(window, min);
}

// The baseline calibrator's settings, with its update counts at the current rate.
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

// Converts the frame-count constants for a new update rate, keeping the
// learned state (lid reference samples, pupil baseline).
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
    LOG_INFO("pipeline %s: eye filters tuned for %.0f updates/s", name_.c_str(), hz);
}

bool BabbleEye::set_option(const std::string &key, const nlohmann::json &value) {
    if (key != "openness_source" || !value.is_string() || (value != "model" && value != "native"))
        return false;
    const bool native = value == "native";
    if (native != native_openness_.exchange(native)) {
        reset_pending_ = true;   // the two sources have different scales: relearn the lid reference
        LOG_INFO("pipeline %s: openness from %s", name_.c_str(), native ? "Valve's eye tracker" : "the model");
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

void BabbleEye::on_presence(bool worn) {
    if (worn)
        return;
    // Headset off: open, relaxed eyes once (VRCFT's defaults); dilation keeps
    // its last value. The next session relearns the lid reference and baseline.
    const uint64_t now = mono_ns();
    std::pair<SignalId, Signal> u[4] = {{left_ids_[0], {1.0f, 0.0f, now}}, {left_ids_[1], {0.0f, 0.0f, now}},
                                        {right_ids_[0], {1.0f, 0.0f, now}}, {right_ids_[1], {0.0f, 0.0f, now}}};
    deps_.state.update(u);
    reset_pending_ = true;
}

void BabbleEye::prepare(const StereoFrame &sf, float *x) {
    const Frame *views[2] = {sf.left.get(), sf.right.get()};
    for (int v = 0; v < 2; v++) {
        const Frame &f = *views[v];
        img::View cur{f.data, f.width, f.height, f.stride};
        if (v == 1) {   // "R" pane: rot90(2) then mirror == vertical flip
            img::flip_vertical(cur, {flip_.data(), f.width, f.height, f.width});
            cur = {flip_.data(), f.width, f.height, f.width};
        }
        img::resize_area(cur, {small_.data(), res_, res_, res_});
        float *dst = x + v * plane_;
        for (size_t i = 0; i < plane_; i++)
            dst[i] = (float(small_[i]) / 255.0f - 0.5f) / 0.25f;
    }
}

bool BabbleEye::run_model(ExecuTorchRunner &m, const float *x, float *y, std::vector<uint16_t> &half_in,
                          std::vector<uint16_t> &half_out) {
    const TensorSpec &is = m.inputs()[0], &os = m.outputs()[0];
    const void *in = x;
    if (is.dtype == DType::F16) {
        half_in.resize(is.elements());
        to_half(x, half_in.data(), is.elements());
        in = half_in.data();
    }
    void *out = y;
    if (os.dtype == DType::F16) {
        half_out.resize(os.elements());
        out = half_out.data();
    }
    std::vector<TensorView> iv{{const_cast<void *>(in), &is}};
    std::vector<TensorView> ov{{out, &os}};
    if (!m.run(iv, ov))
        return false;
    if (os.dtype == DType::F16)
        from_half(half_out.data(), y, os.elements());
    return true;
}

void BabbleEye::on_start() {
    update_eye_server_want();
    if (!pupil_)
        return;
    {
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        pupil_stop_ = false;
    }
    pupil_thread_ = start_thread(pupil_thread_spec_, [this] { pupil_loop(); });
}

void BabbleEye::on_stop() {
    if (deps_.eye_server)
        deps_.eye_server->want(EyeServerClient::Openness, false);
    {
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        pupil_stop_ = true;
    }
    pupil_cv_.notify_all();
    if (pupil_thread_.joinable())
        pupil_thread_.join();
}

void BabbleEye::pupil_loop() {
    std::vector<float> in(2 * plane_);
    std::array<float, 2> out{};
    std::vector<uint16_t> half_in, half_out;
    uint64_t done_seq = 0;
    while (true) {
        uint64_t seq;
        {
            std::unique_lock<std::mutex> lock(pupil_mutex_);
            pupil_cv_.wait(lock, [&] { return pupil_stop_ || pupil_seq_in_ != done_seq; });
            if (pupil_stop_)
                return;
            seq = pupil_seq_in_;
            in = pupil_in_;
            pupil_busy_ = true;
        }
        uint64_t t0 = mono_ns();
        bool ok = run_model(*pupil_, in.data(), out.data(), half_in, half_out);
        pupil_time_.add(mono_ns() - t0);
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        if (ok) {
            pupil_out_ = out;
            pupil_seq_out_ = seq;
            pupil_rate_.tick();
        }
        done_seq = seq;
        pupil_busy_ = false;
    }
}

void BabbleEye::process(const StereoFrame &sf) {
    if (reset_pending_.exchange(false))
        reset_session();
    // Follow the real update rate (the user's eye rate, or less if the
    // cameras deliver less). Within 3% of the reference counts as the
    // reference; otherwise retune after a 10% move, at most once a second.
    update_rate_.tick(sf.capture_ns);
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

    if (!run_model(*lid_, x_.data(), out_.data(), half_in_, half_out_))
        return;
    uint64_t t2 = mono_ns();
    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);

    // Lid openness per eye; the pupil comes with it (combined model) or from
    // the pupil worker: submit while an eye is open, the worker is idle and
    // the rate allows; take each result once.
    std::array<double, 2> lid;
    std::optional<std::array<float, 2>> py;
    if (combined_) {
        lid = {out_[0], out_[2]};
        py = std::array<float, 2>{out_[1], out_[3]};
    } else {
        lid = {out_[0], out_[1]};
    }
    if (native_openness_ && deps_.eye_server) {
        // Our e=0 is view 2 = the wearer's right eye, e=1 the left.
        if (auto s = deps_.eye_server->latest(200000000ull))
            lid = {s->openness[eye_server::kRightEye], s->openness[eye_server::kLeftEye]};
        else
            native_fallback_.add();
    }
    if (pupil_) {
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        const uint64_t now = mono_ns();
        const bool any_open = trackers_[0].open() || trackers_[1].open();
        if (any_open && !pupil_busy_ && (!pupil_submitted_ || now - last_pupil_submit_ns_ >= pupil_every_ns_)) {
            last_pupil_submit_ns_ = now;
            pupil_submitted_ = true;
            pupil_in_ = x_;
            pupil_seq_in_++;
            pupil_cv_.notify_one();
        }
        if (pupil_seq_out_ != 0 && pupil_seq_out_ != pupil_seq_taken_) {
            pupil_seq_taken_ = pupil_seq_out_;
            py = pupil_out_;
        }
    }

    double o[2], widen[2];
    std::optional<double> dil[2];
    for (int e = 0; e < 2; e++) {
        const double raw = lid[e];
        if (use_calib_ && baseline_calib_) {
            // Neutral baseline + wide ceiling (OpennessCalibrator); smooths internally.
            const auto r = openness_[e].update(raw, tuned_.alpha(lid_alpha_));
            o[e] = r.openness;
            widen[e] = r.widen;
            continue;
        }
        // The reference: rolling-median neutral; Widen is whatever exceeds it.
        double v = use_calib_ ? calib_[e].update(raw) : raw;
        smoothed_[e] = smoothed_[e] ? *smoothed_[e] + tuned_.alpha(lid_alpha_) * (v - *smoothed_[e]) : v;
        o[e] = std::max(*smoothed_[e], 0.0);
        widen[e] = std::clamp(o[e] - 1.0, 0.0, 1.0);
    }
    for (int e = 0; e < 2; e++) {
        auto r = trackers_[e].update(o[e], py ? std::optional<double>((*py)[e]) : std::nullopt, tuned_.k());
        dil[e] = eye::dilation(r, pupil_gain_);
    }

    // The module's sides follow the avatar's, not the camera order: by default
    // its "Left" is view 3 and its "Right" is view 2.
    const int left = babble_swap_ ? 0 : 1, right = babble_swap_ ? 1 : 0;
    std::vector<std::pair<SignalId, Signal>> updates;
    auto emit = [&](const std::array<SignalId, 3> &ids, int e) {
        updates.emplace_back(ids[0], Signal{float(std::clamp(o[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
        updates.emplace_back(ids[1], Signal{float(widen[e]), 1.0f, sf.capture_ns});
        if (dil[e])   // nothing until the pupil baseline exists, like the reference
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
    deps_.state.update(updates);
}

} // namespace babble
