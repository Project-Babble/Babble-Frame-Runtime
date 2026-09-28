#include "model/babble_mono_face.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/half.h"
#include "model/image_ops.h"

#include <algorithm>
#include <cmath>

namespace {
const babble::log::Logger LOG("pipe.face");
} // namespace

namespace babble {

namespace {

constexpr uint32_t kSize = 224;
constexpr size_t kMaxSide = 1024;   // largest camera image we preprocess

bool is_float(DType t) { return t == DType::F32 || t == DType::F16; }

} // namespace

bool BabbleMonoFace::init() {
    if (!bind_input())
        return false;

    const auto roi = cfg_.value("roi", nlohmann::json::array());
    if (roi.is_array() && roi.size() == 4)
        roi_ = {roi[0].get<uint32_t>(), roi[1].get<uint32_t>(), roi[2].get<uint32_t>(), roi[3].get<uint32_t>()};
    gain_ = cfg_.value("gain", 1.0f);
    const int rot = cfg_.value("rotate", 0);
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
        LOG.error("pipeline %s: rotate must be 0/90/180/270, got %d", name_.c_str(), rot);
        return false;
    }
    quarter_turns_ = rot / 90;
    const std::string flip = cfg_.value("flip", "none");
    flip_h_ = flip.find('h') != std::string::npos;
    flip_v_ = flip.find('v') != std::string::npos;

    runner_ = std::make_unique<ExecuTorchRunner>();
    if (!runner_->load(model_path("model")))
        return false;
    const auto in = runner_->inputs();
    const auto out = runner_->outputs();
    const std::vector<int64_t> want_in{1, 1, kSize, kSize};
    if (in.size() != 1 || in[0].shape != want_in || !is_float(in[0].dtype) || out.size() != 1 ||
        !is_float(out[0].dtype) || out[0].elements() != face::kShapes.size()) {
        LOG.error("pipeline %s: model does not match the mono face contract ([1,1,224,224] -> [1,45])", name_.c_str());
        return false;
    }

    const auto euro = cfg_.value("one_euro", nlohmann::json::object());
    if (!euro.is_boolean() || euro.get<bool>())
        filter_ = std::make_unique<face::OneEuro>(face::kShapes.size(), euro.is_object() ? euro.value("min_cutoff", 0.5f) : 0.5f,
                                                  euro.is_object() ? euro.value("beta", 3.0f) : 3.0f,
                                                  euro.is_object() ? euro.value("d_cutoff", 1.0f) : 1.0f);

    const auto cal = cfg_.value("calibration", nlohmann::json::object());
    face::CalibMode mode = face::CalibMode::Auto;
    if (!face::parse_mode(cal.value("mode", "auto"), &mode)) {
        LOG.error("pipeline %s: calibration mode must be auto, raw or manual", name_.c_str());
        return false;
    }
    calibration_ = std::make_unique<face::Calibration>(face::kShapes.size(), mode);
    const auto ranges = cal.value("ranges", nlohmann::json::object());
    for (size_t i = 0; i < face::kShapes.size(); i++) {
        const auto r = ranges.find(face::kShapes[i]);
        if (r != ranges.end() && r->is_array() && r->size() == 2)
            calibration_->set_range(i, (*r)[0].get<float>(), (*r)[1].get<float>());
    }

    for (size_t i = 0; i < face::kShapes.size(); i++)
        ids_[i] = deps_.state.add_signal(face::kShapes[i]);

    tmp_a_.resize(kMaxSide * kMaxSide);
    tmp_b_.resize(kMaxSide * kMaxSide);
    small_.resize(kSize * kSize);
    input_.resize(kSize * kSize);
    out_.resize(face::kShapes.size());
    values_.resize(face::kShapes.size());
    LOG.info("pipeline %s: mono face model on %s (%s, roi %s, rotate %d, flip %s, gain %.2f, calibration %s)",
             name_.c_str(), source_.c_str(), in[0].dtype == DType::F16 ? "fp16" : "fp32",
             roi_.w ? (std::to_string(roi_.x) + "," + std::to_string(roi_.y) + " " + std::to_string(roi_.w) + "x" +
                       std::to_string(roi_.h)).c_str()
                    : "full",
             rot, flip.c_str(), gain_, cal.value("mode", "auto").c_str());
    return true;
}

void BabbleMonoFace::prepare(const Frame &f, float *input) {
    // Crop (a view into the frame), as Baballonia's ImageTransformer: an ROI
    // that doesn't fit, or is empty, means the whole image.
    img::View cur{f.data, f.width, f.height, f.stride};
    if (roi_.w && roi_.h && roi_.x + roi_.w <= f.width && roi_.y + roi_.h <= f.height)
        cur = {f.data + size_t(roi_.y) * f.stride + roi_.x, roi_.w, roi_.h, f.stride};
    const uint32_t w = std::min<uint32_t>(cur.width, kMaxSide), h = std::min<uint32_t>(cur.height, kMaxSide);
    cur.width = w;
    cur.height = h;

    if (gain_ != 1.0f) {   // saturating linear gain, rounded like OpenCV's convertTo
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                tmp_a_[size_t(y) * w + x] =
                    uint8_t(std::clamp(std::lround(cur.data[size_t(y) * cur.stride + x] * gain_), 0L, 255L));
        cur = {tmp_a_.data(), w, h, w};
    }
    if (flip_h_ || flip_v_) {
        uint8_t *dst = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
        if (flip_h_) {
            img::flip_horizontal(cur, {dst, w, h, w});
            cur = {dst, w, h, w};
        }
        if (flip_v_) {
            uint8_t *dst2 = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
            img::flip_vertical(cur, {dst2, w, h, w});
            cur = {dst2, w, h, w};
        }
    }
    if (quarter_turns_) {
        uint8_t *dst = cur.data == tmp_a_.data() ? tmp_b_.data() : tmp_a_.data();
        const bool swap_wh = quarter_turns_ % 2 == 1;
        img::Mut rotated{dst, swap_wh ? h : w, swap_wh ? w : h, swap_wh ? h : w};
        img::rotate(cur, rotated, quarter_turns_);
        cur = rotated;
    }
    img::Mut small{small_.data(), kSize, kSize, kSize};
    img::resize_linear(cur, small);
    for (size_t i = 0; i < small_.size(); i++)
        input[i] = float(small_[i]) / 255.0f;
}

std::vector<std::pair<SignalId, Signal>> BabbleMonoFace::neutral(uint64_t now) const {
    std::vector<std::pair<SignalId, Signal>> updates;
    for (SignalId id : ids_)
        updates.emplace_back(id, Signal{0.0f, 0.0f, now});
    return updates;
}

void BabbleMonoFace::process(const StereoFrame &sf) {
    if (!sf.left)
        return;
    if (reset_pending_.exchange(false)) {
        if (filter_)
            filter_->reset();
        calibration_->reset();
        last_capture_ns_ = 0;
    }
    const uint64_t t0 = mono_ns();
    prepare(*sf.left, input_.data());
    const uint64_t t1 = mono_ns();

    const TensorSpec &is = runner_->inputs()[0], &os = runner_->outputs()[0];
    void *in = input_.data(), *out = out_.data();
    if (is.dtype == DType::F16) {
        half_in_.resize(is.elements());
        to_half(input_.data(), half_in_.data(), is.elements());
        in = half_in_.data();
    }
    if (os.dtype == DType::F16) {
        half_out_.resize(os.elements());
        out = half_out_.data();
    }
    std::vector<TensorView> iv{{in, &is}};
    std::vector<TensorView> ov{{out, &os}};
    if (!runner_->run(iv, ov))
        return;
    if (os.dtype == DType::F16)
        from_half(half_out_.data(), out_.data(), os.elements());
    const uint64_t t2 = mono_ns();

    values_.assign(out_.begin(), out_.end());
    if (filter_) {
        const float dt = last_capture_ns_ && sf.capture_ns > last_capture_ns_
                             ? float(double(sf.capture_ns - last_capture_ns_) / 1e9)
                             : 0.0f;
        filter_->apply(values_, dt);
    }
    last_capture_ns_ = sf.capture_ns;
    calibration_->apply(values_);

    std::vector<std::pair<SignalId, Signal>> updates;
    updates.reserve(ids_.size());
    for (size_t i = 0; i < ids_.size(); i++)
        updates.emplace_back(ids_[i], Signal{values_[i], 1.0f, sf.capture_ns});
    publish(updates);

    preprocess_.add(t1 - t0);
    inference_.add(t2 - t1);
}

} // namespace babble
