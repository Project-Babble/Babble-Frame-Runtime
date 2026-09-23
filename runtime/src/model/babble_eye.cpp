#include "model/babble_eye.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/image_ops.h"

#include <algorithm>
#include <cmath>

namespace babble {

namespace {

constexpr uint32_t kRes = 160;
constexpr size_t kPlane = size_t(kRes) * kRes;

bool eye_contract(const ExecuTorchRunner &r) {
    auto in = r.inputs();
    auto out = r.outputs();
    return in.size() == 1 && in[0].shape == std::vector<int64_t>{2, 1, kRes, kRes} && in[0].dtype == DType::F32 &&
           out.size() == 1 && out[0].elements() == 2 && out[0].dtype == DType::F32;
}

} // namespace

BabbleEye::~BabbleEye() { stop(); }

bool BabbleEye::init() {
    if (!bind_input())
        return false;

    lid_ = std::make_unique<ExecuTorchRunner>();
    if (!lid_->load(model_path("lid_model")))
        return false;
    if (!eye_contract(*lid_)) {
        LOG_ERROR("pipeline %s: lid model does not match [2,1,160,160] -> [2]", name_.c_str());
        return false;
    }
    if (!cfg_.value("pupil_model", "").empty()) {
        pupil_ = std::make_unique<ExecuTorchRunner>();
        if (!pupil_->load(model_path("pupil_model")) || !eye_contract(*pupil_)) {
            LOG_WARN("pipeline %s: pupil model unusable; running without dilation", name_.c_str());
            pupil_.reset();
        }
    }

    const double hz = cfg_.value("update_hz", 90.0);
    const double pupil_hz = std::min(cfg_.value("pupil_hz", 3.0), hz);
    use_calib_ = cfg_.value("calib", true);
    babble_swap_ = cfg_.value("babble_swap", false);
    lid_alpha_ = cfg_.value("lid_alpha", 0.6);
    pupil_gain_ = cfg_.value("pupil_gain", 2.0);
    pupil_every_ns_ = uint64_t(1e9 / pupil_hz);

    for (int e = 0; e < 2; e++) {
        calib_.emplace_back(size_t(cfg_.value("calib_window_s", 20.0) * hz), size_t(cfg_.value("calib_warmup_s", 3.0) * hz));
        trackers_.emplace_back(cfg_.value("pupil_gate", 0.4), cfg_.value("pupil_gate_hys", 0.1),
                               int(cfg_.value("pupil_hold_s", 0.1) * hz), cfg_.value("pupil_alpha", 0.2),
                               cfg_.value("pupil_rate", 0.6),
                               size_t(std::max(10, int(cfg_.value("pupil_base_s", 60.0) * pupil_hz))),
                               size_t(std::max(5, int(pupil_hz))));
    }

    const char *names[3] = {"Lid", "Widen", "Dilation"};
    for (int k = 0; k < 3; k++) {
        left_ids_[k] = deps_.state.add_signal(std::string("LeftEye") + names[k]);
        right_ids_[k] = deps_.state.add_signal(std::string("RightEye") + names[k]);
    }

    x_.resize(2 * kPlane);
    pupil_in_.resize(2 * kPlane);
    flip_.resize(400 * 400);
    small_.resize(kPlane);
    pupil_thread_spec_ = thread_spec(deps_.threads, cfg_.value("pupil_thread", "pupil"), "bbl-pupil");
    deps_.stats.add("pipe." + name_ + ".pupil_fps", &pupil_rate_);
    deps_.stats.add("pipe." + name_ + ".pupil_inference", &pupil_time_);
    LOG_INFO("pipeline %s: lid%s on %s at %.0f updates/s%s", name_.c_str(), pupil_ ? " + pupil" : "",
             source_.c_str(), hz, pupil_ ? (", pupil " + std::to_string(int(pupil_hz)) + " Hz").c_str() : "");
    return true;
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
        img::resize_area(cur, {small_.data(), kRes, kRes, kRes});
        float *dst = x + v * kPlane;
        for (size_t i = 0; i < kPlane; i++)
            dst[i] = (float(small_[i]) / 255.0f - 0.5f) / 0.25f;
    }
}

void BabbleEye::on_start() {
    if (!pupil_)
        return;
    {
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        pupil_stop_ = false;
    }
    pupil_thread_ = start_thread(pupil_thread_spec_, [this] { pupil_loop(); });
}

void BabbleEye::on_stop() {
    {
        std::lock_guard<std::mutex> lock(pupil_mutex_);
        pupil_stop_ = true;
    }
    pupil_cv_.notify_all();
    if (pupil_thread_.joinable())
        pupil_thread_.join();
}

void BabbleEye::pupil_loop() {
    std::vector<float> in(2 * kPlane);
    std::array<float, 2> out{};
    auto ri = pupil_->inputs();
    auto ro = pupil_->outputs();
    std::vector<TensorView> iv{{in.data(), &ri[0]}};
    std::vector<TensorView> ov{{out.data(), &ro[0]}};
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
        bool ok = pupil_->run(iv, ov);
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
    uint64_t t0 = mono_ns();
    prepare(sf, x_.data());
    uint64_t t1 = mono_ns();

    auto ri = lid_->inputs();
    auto ro = lid_->outputs();
    std::vector<TensorView> iv{{x_.data(), &ri[0]}};
    std::vector<TensorView> ov{{lid_out_.data(), &ro[0]}};
    if (!lid_->run(iv, ov))
        return;
    uint64_t t2 = mono_ns();
    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);

    // Pupil: submit while an eye is open, the worker is idle and the rate allows; take each result once.
    std::optional<std::array<float, 2>> py;
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

    double o[2];
    std::optional<double> dil[2];
    for (int e = 0; e < 2; e++) {
        double raw = lid_out_[e];
        double v = use_calib_ ? calib_[e].update(raw) : raw;
        smoothed_[e] = smoothed_[e] ? *smoothed_[e] + lid_alpha_ * (v - *smoothed_[e]) : v;
        o[e] = std::max(*smoothed_[e], 0.0);
    }
    for (int e = 0; e < 2; e++) {
        auto r = trackers_[e].update(o[e], py ? std::optional<double>((*py)[e]) : std::nullopt);
        dil[e] = eye::dilation(r, pupil_gain_);
    }

    // The module's sides follow the avatar's, not the camera order: by default
    // its "Left" is view 3 and its "Right" is view 2.
    const int left = babble_swap_ ? 0 : 1, right = babble_swap_ ? 1 : 0;
    std::vector<std::pair<SignalId, Signal>> updates;
    auto emit = [&](const std::array<SignalId, 3> &ids, int e) {
        updates.emplace_back(ids[0], Signal{float(std::clamp(o[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
        updates.emplace_back(ids[1], Signal{float(std::clamp(o[e] - 1.0, 0.0, 1.0)), 1.0f, sf.capture_ns});
        if (dil[e])   // nothing until the pupil baseline exists, like the reference
            updates.emplace_back(ids[2], Signal{float(std::clamp(*dil[e], 0.0, 1.0)), 1.0f, sf.capture_ns});
    };
    emit(left_ids_, left);
    emit(right_ids_, right);
    deps_.state.update(updates);
}

} // namespace babble
