#include "model/executorch_runner.h"

#include "core/log.h"

#include <executorch/extension/module/module.h>
#include <executorch/extension/tensor/tensor.h>
#include <executorch/extension/threadpool/threadpool.h>

#include <cstring>

namespace babble {

using executorch::aten::ScalarType;
using executorch::extension::Module;
using executorch::extension::TensorPtr;
using executorch::runtime::EValue;
using executorch::runtime::Error;

struct ExecuTorchRunner::Impl {
    std::unique_ptr<Module> module;
    std::string path;
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
    default: return false;
    }
}

ScalarType to_scalar(DType d) { return d == DType::F32 ? ScalarType::Float : ScalarType::Byte; }

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
        LOG_ERROR("executorch: cannot load %s (error %d)", impl_->path.c_str(), int(e));
        return false;
    }
    auto meta = impl_->module->method_meta("forward");
    if (!meta.ok()) {
        LOG_ERROR("executorch: no method metadata in %s", impl_->path.c_str());
        return false;
    }
    auto describe = [&](auto info, size_t i, const char *kind, std::vector<TensorSpec> &specs) {
        if (!info.ok()) {
            LOG_ERROR("executorch: %s %s %zu is not a tensor", impl_->path.c_str(), kind, i);
            return false;
        }
        TensorSpec s;
        s.name = std::string(kind) + std::to_string(i);
        for (int32_t d : info->sizes())
            s.shape.push_back(d);
        if (!to_dtype(info->scalar_type(), &s.dtype)) {
            LOG_ERROR("executorch: %s %s %zu has unsupported dtype %d", impl_->path.c_str(), kind, i,
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

    std::string shapes;
    for (const auto &s : inputs_) {
        shapes += shapes.empty() ? "[" : " [";
        for (size_t k = 0; k < s.shape.size(); k++)
            shapes += (k ? "," : "") + std::to_string(s.shape[k]);
        shapes += "]";
    }
    LOG_INFO("executorch: loaded %s: %zu inputs %s, %zu outputs", impl_->path.c_str(), inputs_.size(),
             shapes.c_str(), outputs_.size());
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
    auto result = impl_->module->execute("forward", impl_->in_values);
    if (!result.ok()) {
        LOG_WARN("executorch: %s forward failed (error %d)", impl_->path.c_str(), int(result.error()));
        return false;
    }
    if (result->size() != out.size())
        return false;
    for (size_t i = 0; i < out.size(); i++) {
        const auto &t = (*result)[i].toTensor();
        size_t bytes = t.nbytes();
        if (bytes != outputs_[i].bytes())
            return false;
        memcpy(out[i].data, t.const_data_ptr(), bytes);
    }
    return true;
}

} // namespace babble
