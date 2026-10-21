#include "model/fusionnet_mouth.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "model/image_ops.h"

#include <algorithm>

namespace {
const babble::log::Logger LOG("pipe.mouth");
}

namespace babble {

namespace {

constexpr uint32_t kSize = 224;

img::View view_of(const Frame &f) { return {f.data, f.width, f.height, f.stride}; }

}

bool FusionNetMouth::init() {
    if (!bind_input())
        return false;

    swap_ = cfg_.value("swap", false);
    std::string flip = cfg_.value("flip", "r");
    if (flip == "none")
        flip.clear();
    flip_l_ = flip.find('l') != std::string::npos;
    flip_r_ = flip.find('r') != std::string::npos;
    int rot = cfg_.value("rotate", 0);
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
        LOG.error("pipeline %s: rotate must be 0/90/180/270, got %d", name_.c_str(), rot);
        return false;
    }
    quarter_turns_ = rot / 90;
    area_ = cfg_.value("interp", "linear") == "area";

    runner_ = std::make_unique<ExecuTorchRunner>();
    if (!runner_->load(model_path("model")))
        return false;
    auto in = runner_->inputs();
    auto out = runner_->outputs();
    const std::vector<int64_t> want_in{1, 1, kSize, kSize};
    const size_t n_out = out.size() == 1 ? out[0].elements() : 0;
    if (in.size() != 2 || in[0].shape != want_in || in[1].shape != want_in || in[0].dtype != DType::F32 ||
        out.size() != 1 || out[0].dtype != DType::F32 || (n_out != mouth::kNumShapes && n_out != mouth::kNumChannels)) {
        LOG.error("pipeline %s: model does not match FusionNet's contract (2x[1,1,224,224] -> [1,43] or [1,44])",
                  name_.c_str());
        return false;
    }

    // A user copy of the calibration in the config dir has priority over the default file.
    const auto cal_cfg = cfg_.value("calibration", nlohmann::json::object());
    mouth::CalibMode mode = mouth::CalibMode::Manual;
    if (!mouth::parse_mode(cal_cfg.value("mode", "auto"), &mode)) {
        LOG.error("pipeline %s: unknown calibration mode", name_.c_str());
        return false;
    }
    const std::string file = cal_cfg.value("file", "calibration_mouth.json");
    std::filesystem::path cal_path = paths::config_dir() / file;
    std::error_code ec;
    if (!std::filesystem::exists(cal_path, ec))
        cal_path = paths::resource_dir() / file;
    std::string err;
    mouth::Calibration cal = mouth::Calibration::load(cal_path, &err);
    if (!err.empty())
        LOG.warn("pipeline %s: ignoring calibration %s: %s", name_.c_str(), cal_path.c_str(), err.c_str());

    post_ = std::make_unique<mouth::Post>(one_euro_params(cfg_.value("one_euro", nlohmann::json::object()), mouth::kOneEuro),
                                          mode, std::move(cal));

    for (size_t i = 0; i < mouth::kNumShapes; i++)
        ids_[i] = deps_.state.add_signal(mouth::camel(mouth::kChannels[i]));

    tmp_a_.resize(400 * 400);
    tmp_b_.resize(400 * 400);
    small_.resize(kSize * kSize);
    in0_.resize(kSize * kSize);
    in1_.resize(kSize * kSize);
    probe_left_ = probe_total_ = cfg_.value("probe_orientation", 0);
    if (probe_total_ > 0)
        LOG.info("pipeline %s: probing orientation over %d frames (wear the headset, face neutral)", name_.c_str(),
                 probe_total_);
    LOG.info("pipeline %s: FusionNet on %s (swap=%d flip=%s%s rotate=%d interp=%s calibration=%s from %s)",
             name_.c_str(), source_.c_str(), int(swap_), flip_l_ ? "l" : "", flip_r_ ? "r" : "", rot,
             area_ ? "area" : "linear", cal_cfg.value("mode", "auto").c_str(), cal_path.c_str());
    return true;
}

void FusionNetMouth::prepare(const StereoFrame &sf, float *view0, float *view1) {
    const Frame *f0 = sf.left.get(), *f1 = sf.right.get();
    if (swap_)
        std::swap(f0, f1);
    const bool flips[2] = {flip_l_, flip_r_};
    const Frame *frames[2] = {f0, f1};
    float *outs[2] = {view0, view1};

    for (int v = 0; v < 2; v++) {
        img::View cur = view_of(*frames[v]);
        const uint32_t w = cur.width, h = cur.height;
        // The frame sets the buffer size because the configured format can be larger than 400x400.
        if (tmp_a_.size() < size_t(w) * h) {
            tmp_a_.resize(size_t(w) * h);
            tmp_b_.resize(size_t(w) * h);
        }
        if (flips[v]) {
            img::flip_horizontal(cur, {tmp_a_.data(), w, h, w});
            cur = {tmp_a_.data(), w, h, w};
        }
        if (quarter_turns_) {
            const bool swap_wh = quarter_turns_ % 2 == 1;
            img::Mut dst{tmp_b_.data(), swap_wh ? h : w, swap_wh ? w : h, swap_wh ? h : w};
            img::rotate(cur, dst, quarter_turns_);
            cur = dst;
        }
        img::Mut small{small_.data(), kSize, kSize, kSize};
        if (area_)
            img::resize_area(cur, small);
        else
            img::resize_linear(cur, small);
        img::equalize_hist(small, small);
        for (size_t i = 0; i < small_.size(); i++)
            outs[v][i] = float(small_[i]) / 255.0f;
    }
}

void FusionNetMouth::probe(const StereoFrame &sf) {
    // Try each rotate and swap combination and rank them by the UserPresent output of the model.
    if (runner_->outputs()[0].elements() != mouth::kNumChannels) {
        LOG.warn("pipeline %s: orientation probe needs a model with a UserPresent output", name_.c_str());
        probe_left_ = 0;
        return;
    }
    const bool saved_swap = swap_;
    const int saved_turns = quarter_turns_;
    auto ri = runner_->inputs();
    auto ro = runner_->outputs();
    TensorView iv[] = {{in0_.data(), &ri[0]}, {in1_.data(), &ri[1]}};
    std::array<float, mouth::kNumChannels> out{};
    TensorView ov[] = {{out.data(), &ro[0]}};
    for (int c = 0; c < 8; c++) {
        swap_ = c >= 4;
        quarter_turns_ = c % 4;
        prepare(sf, in0_.data(), in1_.data());
        if (runner_->run(iv, ov))
            probe_sum_[c] += std::clamp(out[mouth::kNumChannels - 1], 0.0f, 1.0f);
    }
    swap_ = saved_swap;
    quarter_turns_ = saved_turns;
    if (--probe_left_ > 0)
        return;
    std::array<int, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return probe_sum_[a] > probe_sum_[b]; });
    LOG.info("pipeline %s: orientation probe over %d frames (mean UserPresent, best first):", name_.c_str(), probe_total_);
    for (int c : order)
        LOG.info("  swap=%-5s rotate=%3d  presence %.3f%s", c >= 4 ? "true" : "false", (c % 4) * 90,
                 probe_sum_[c] / probe_total_, (c >= 4) == swap_ && c % 4 == quarter_turns_ ? "   <- configured" : "");
}

std::vector<std::pair<SignalId, Signal>> FusionNetMouth::neutral(uint64_t now) const {
    // Send a relaxed face because the cameras pause and send no more frames.
    std::vector<std::pair<SignalId, Signal>> updates;
    for (size_t i = 0; i < mouth::kNumShapes; i++)
        updates.emplace_back(ids_[i], Signal{0.0f, 0.0f, now});
    return updates;
}

void FusionNetMouth::process(const StereoFrame &sf) {
    if (reset_pending_.exchange(false)) {
        post_->reset();
        last_capture_ns_ = 0;
    }
    if (probe_left_ > 0)
        probe(sf);
    uint64_t t0 = mono_ns();
    prepare(sf, in0_.data(), in1_.data());
    uint64_t t1 = mono_ns();

    auto ri = runner_->inputs();
    auto ro = runner_->outputs();
    TensorView iv[] = {{in0_.data(), &ri[0]}, {in1_.data(), &ri[1]}};
    TensorView ov[] = {{out_.data(), &ro[0]}};
    if (!runner_->run(iv, ov))
        return;
    uint64_t t2 = mono_ns();

    const double dt = last_capture_ns_ && sf.capture_ns > last_capture_ns_
                          ? double(sf.capture_ns - last_capture_ns_) / 1e9
                          : 0.0;
    last_capture_ns_ = sf.capture_ns;
    mouth::Vec norm = post_->process(out_, dt);

    std::vector<std::pair<SignalId, Signal>> updates;
    updates.reserve(mouth::kNumShapes);
    for (size_t i = 0; i < mouth::kNumShapes; i++)
        updates.emplace_back(ids_[i], Signal{norm[i], 1.0f, sf.capture_ns});
    publish(updates);

    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);
}

}
