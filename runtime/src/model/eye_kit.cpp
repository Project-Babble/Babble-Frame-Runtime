#include "model/eye_kit.h"

#include "core/clock.h"
#include "core/log.h"
#include "model/half.h"

namespace {
const babble::log::Logger LOG("eye.kit");
}

namespace babble {

namespace {

bool is_float(DType d) { return d == DType::F32 || d == DType::F16; }

// Make sure that a stage takes [2,1,S,S] (and, for stage 2, a [2,2] centre) and gives `outputs` floats. S goes to *side.
bool stage_contract(const ExecuTorchRunner &r, size_t inputs, size_t outputs, uint32_t *side) {
    auto in = r.inputs();
    auto out = r.outputs();
    if (in.size() != inputs || out.size() != 1 || out[0].elements() != outputs || !is_float(out[0].dtype))
        return false;
    const auto &sh = in[0].shape;
    if (sh.size() != 4 || sh[0] != 2 || sh[1] != 1 || sh[2] != sh[3] || sh[2] < 16 || sh[2] > 400 || !is_float(in[0].dtype))
        return false;
    if (inputs == 2 && (in[1].elements() != 4 || in[1].shape.size() != 2 || in[1].shape[0] != 2 || !is_float(in[1].dtype)))
        return false;
    *side = uint32_t(sh[2]);
    return true;
}

// Run a model on float buffers, with fp16 conversion for each tensor that uses fp16.
bool run_float(ExecuTorchRunner &m, const std::vector<const float *> &x, float *y,
               std::vector<std::vector<uint16_t>> &half_in, std::vector<uint16_t> &half_out) {
    auto ins = m.inputs();
    const TensorSpec &os = m.outputs()[0];
    half_in.resize(ins.size());
    std::vector<TensorView> iv;
    for (size_t i = 0; i < ins.size(); i++) {
        const void *p = x[i];
        if (ins[i].dtype == DType::F16) {
            half_in[i].resize(ins[i].elements());
            to_half(x[i], half_in[i].data(), ins[i].elements());
            p = half_in[i].data();
        }
        iv.push_back({const_cast<void *>(p), &ins[i]});
    }
    void *out = y;
    if (os.dtype == DType::F16) {
        half_out.resize(os.elements());
        out = half_out.data();
    }
    std::vector<TensorView> ov{{out, &os}};
    if (!m.run(iv, ov))
        return false;
    if (os.dtype == DType::F16)
        from_half(half_out.data(), y, os.elements());
    return true;
}

void normalise(const uint8_t *px, float *dst, size_t n) {
    for (size_t i = 0; i < n; i++)
        dst[i] = (float(px[i]) / 255.0f - 0.5f) / 0.25f;
}

}

bool EyeKit::load(const std::filesystem::path &stage1, const std::filesystem::path &stage2, uint32_t crop_px) {
    s1_ = std::make_unique<ExecuTorchRunner>();
    s2_ = std::make_unique<ExecuTorchRunner>();
    if (!s1_->load(stage1) || !s2_->load(stage2))
        return false;
    per_view_ = stage_contract(*s1_, 1, 8, &res_) ? 4 : 3;
    if (per_view_ == 3 && !stage_contract(*s1_, 1, 6, &res_)) {
        LOG.error("%s does not match the eye kit stage 1 contract [2,1,R,R] -> [2,3] or [2,4]", stage1.c_str());
        return false;
    }
    if (!stage_contract(*s2_, 2, 2, &crop_res_)) {
        LOG.error("%s does not match the eye kit stage 2 contract ([2,1,C,C], [2,2]) -> [2,1]", stage2.c_str());
        return false;
    }
    if (crop_px < crop_res_) {
        LOG.error("eye kit crop of %u px is smaller than the stage 2 input of %u px", crop_px, crop_res_);
        return false;
    }
    crop_px_ = crop_px;
    x1_.resize(2 * size_t(res_) * res_);
    x2_.resize(2 * size_t(crop_res_) * crop_res_);
    pos_.resize(4);
    small_.resize(size_t(res_) * res_);
    crop_.resize(size_t(crop_px_) * crop_px_);
    crop_small_.resize(size_t(crop_res_) * crop_res_);
    return true;
}

bool EyeKit::fp16() const { return s1_ && s1_->inputs()[0].dtype == DType::F16; }

bool EyeKit::run(const std::array<img::View, 2> &views, Result &r, Timing *t) {
    const uint64_t t0 = mono_ns();
    const size_t plane1 = size_t(res_) * res_, plane2 = size_t(crop_res_) * crop_res_;
    for (int v = 0; v < 2; v++) {
        img::resize_area_linear(views[v], {small_.data(), res_, res_, res_}, mid_);
        normalise(small_.data(), x1_.data() + v * plane1, plane1);
    }
    const uint64_t t1 = mono_ns();
    if (!run_float(*s1_, {x1_.data()}, out1_.data(), half_in_, half_out_))
        return false;
    const uint64_t t2 = mono_ns();
    for (int v = 0; v < 2; v++) {
        const img::View &f = views[v];
        const float *o = out1_.data() + per_view_ * v;
        r.lid[v] = o[0];
        // The model gives the centre in edge coordinates, and getRectSubPix takes index coordinates.
        r.x[v] = double(f.width) * o[1] - 0.5;
        r.y[v] = double(f.height) * o[2] - 0.5;
        r.widen[v] = per_view_ == 4 ? o[3] : 0.0;
        img::get_rect_sub_pix(f, {crop_.data(), crop_px_, crop_px_, crop_px_}, float(r.x[v]), float(r.y[v]));
        img::resize_area({crop_.data(), crop_px_, crop_px_, crop_px_}, {crop_small_.data(), crop_res_, crop_res_, crop_res_});
        normalise(crop_small_.data(), x2_.data() + v * plane2, plane2);
        pos_[2 * v] = float((r.x[v] - f.width / 2.0) / (f.width / 4.0));
        pos_[2 * v + 1] = float((r.y[v] - f.height / 2.0) / (f.height / 4.0));
    }
    const uint64_t t3 = mono_ns();
    if (!run_float(*s2_, {x2_.data(), pos_.data()}, out2_.data(), half_in_, half_out_))
        return false;
    const uint64_t t4 = mono_ns();
    for (int v = 0; v < 2; v++)
        r.mm[v] = kMmOffset + kMmScale * out2_[v];
    if (t)
        *t = {t1 - t0, t2 - t1, t3 - t2, t4 - t3};
    return true;
}

}
