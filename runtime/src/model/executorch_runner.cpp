#include "model/executorch_runner.h"

#include "core/clock.h"
#include "core/log.h"

#include <executorch/extension/module/module.h>
#include <executorch/extension/tensor/tensor.h>
#include <executorch/extension/threadpool/threadpool.h>

#include <cstring>

namespace {
const babble::log::Logger LOG("model");
} // namespace

namespace babble {

using executorch::aten::ScalarType;
using executorch::extension::Module;
using executorch::extension::TensorPtr;
using executorch::runtime::EValue;
using executorch::runtime::Error;

struct ExecuTorchRunner::Impl {
    std::unique_ptr<Module> module;
    std::string path;
    uint64_t failures = 0;   // run() failures so far (logged sparsely)
    bool first_done = false;
    // Input tensors wrap the caller's buffers; rebuilt only if a buffer moves.
    std::vector<TensorPtr> in_tensors;
    std::vector<const void *> in_ptrs;
    std::vector<EValue> in_values;
};

namespace {

bool to_dtype(ScalarType t, DType *out) {
    switch (t) {
    case ScalarType::Float: *out = DType::F32; return true;
    case ScalarType::Byte: *out = DType::U8; return true;
    case ScalarType::Half: *out = DType::F16; return true;
    default: return false;
    }
}

ScalarType to_scalar(DType d) {
    switch (d) {
    case DType::F32: return ScalarType::Float;
    case DType::F16: return ScalarType::Half;
    case DType::U8: return ScalarType::Byte;
    }
    return ScalarType::Float;
}

} // namespace

ExecuTorchRunner::ExecuTorchRunner() : impl_(std::make_unique<Impl>()) {}
ExecuTorchRunner::~ExecuTorchRunner() = default;

void ExecuTorchRunner::set_threads(uint32_t n) {
    if (auto *pool = executorch::extension::threadpool::get_threadpool())
        pool->_unsafe_reset_threadpool(n ? n : 1);
}

bool ExecuTorchRunner::load(const std::filesystem::path &model) {
    impl_->path = model.string();
    impl_->module = std::make_unique<Module>(impl_->path, Module::LoadMode::Mmap);
    if (Error e = impl_->module->load_method("forward"); e != Error::Ok) {
        LOG.error("cannot load %s (error %d)", impl_->path.c_str(), int(e));
        return false;
    }
    auto meta = impl_->module->method_meta("forward");
    if (!meta.ok()) {
        LOG.error("no method metadata in %s", impl_->path.c_str());
        return false;
    }
    auto describe = [&](auto info, size_t i, const char *kind, std::vector<TensorSpec> &specs) {
        if (!info.ok()) {
            LOG.error("%s %s %zu is not a tensor", impl_->path.c_str(), kind, i);
            return false;
        }
        TensorSpec s;
        s.name = std::string(kind) + std::to_string(i);
        for (int32_t d : info->sizes())
            s.shape.push_back(d);
        if (!to_dtype(info->scalar_type(), &s.dtype)) {
            LOG.error("%s %s %zu has unsupported dtype %d", impl_->path.c_str(), kind, i,
                      int(info->scalar_type()));
            return false;
        }
        specs.push_back(std::move(s));
        return true;
    };
    inputs_.clear();
    outputs_.clear();
    for (size_t i = 0; i < meta->num_inputs(); i++)
        if (!describe(meta->input_tensor_meta(i), i, "input", inputs_))
            return false;
    for (size_t i = 0; i < meta->num_outputs(); i++)
        if (!describe(meta->output_tensor_meta(i), i, "output", outputs_))
            return false;
    impl_->in_tensors.assign(inputs_.size(), nullptr);
    impl_->in_ptrs.assign(inputs_.size(), nullptr);
    impl_->in_values.assign(inputs_.size(), EValue());

    // The model's contract, for the log: "[1,1,224,224] f16 -> [1,45] f16".
    auto describe_all = [](const std::vector<TensorSpec> &specs) {
        std::string out;
        for (const auto &s : specs) {
            out += out.empty() ? "[" : " [";
            for (size_t k = 0; k < s.shape.size(); k++)
                out += (k ? "," : "") + std::to_string(s.shape[k]);
            out += s.dtype == DType::F16 ? "] f16" : s.dtype == DType::F32 ? "] f32" : "] u8";
        }
        return out;
    };
    LOG.info("loaded %s: %s -> %s", impl_->path.c_str(), describe_all(inputs_).c_str(), describe_all(outputs_).c_str());
    return true;
}

bool ExecuTorchRunner::run(std::span<const TensorView> in, std::span<TensorView> out) {
    if (!impl_->module || in.size() != inputs_.size() || out.size() != outputs_.size())
        return false;
    for (size_t i = 0; i < in.size(); i++) {
        if (impl_->in_ptrs[i] != in[i].data) {
            std::vector<executorch::aten::SizesType> sizes(inputs_[i].shape.begin(), inputs_[i].shape.end());
            impl_->in_tensors[i] = executorch::extension::from_blob(const_cast<void *>(in[i].data), sizes,
                                                                    to_scalar(inputs_[i].dtype));
            impl_->in_ptrs[i] = in[i].data;
            impl_->in_values[i] = EValue(*impl_->in_tensors[i]);
        }
    }
    // Failures log on the 1st, 2nd, 4th, 8th... occurrence: visible, never a flood at frame rate.
    auto fail = [&](const std::string &why) {
        const uint64_t n = ++impl_->failures;
        if ((n & (n - 1)) == 0)
            LOG.warn("%s: %s (failure #%llu)", impl_->path.c_str(), why.c_str(), (unsigned long long)n);
        return false;
    };
    const uint64_t t0 = impl_->first_done ? 0 : mono_ns();
    auto result = impl_->module->execute("forward", impl_->in_values);
    if (!impl_->first_done && result.ok()) {
        // The first run includes XNNPACK's weight packing: worth knowing when a model starts slowly.
        impl_->first_done = true;
        LOG.info("%s: first inference %.1f ms", impl_->path.c_str(), double(mono_ns() - t0) / 1e6);
    }
    if (!result.ok())
        return fail("forward failed (error " + std::to_string(int(result.error())) + ")");
    if (result->size() != out.size())
        return fail("forward returned " + std::to_string(result->size()) + " outputs, expected " +
                    std::to_string(out.size()));
    for (size_t i = 0; i < out.size(); i++) {
        const auto &t = (*result)[i].toTensor();
        size_t bytes = t.nbytes();
        if (bytes != outputs_[i].bytes())
            return fail("output " + std::to_string(i) + " is " + std::to_string(bytes) + " bytes, expected " +
                        std::to_string(outputs_[i].bytes()));
        memcpy(out[i].data, t.const_data_ptr(), bytes);
    }
    return true;
}

} // namespace babble
