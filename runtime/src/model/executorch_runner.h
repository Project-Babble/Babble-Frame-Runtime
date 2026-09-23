#pragma once

#include "model/model_runner.h"

#include <memory>
#include <string>
#include <vector>

namespace babble {

// IModelRunner over an ExecuTorch .pte (XNNPACK delegate + a few portable
// kernels). The file is memory-mapped; tensor specs come from the model's
// own metadata, so the pipeline can validate its preprocessing against them.
//
// XNNPACK's threadpool is process-global and does not follow per-thread
// affinity, so the runtime keeps it at one thread (set_threads) and runs each
// model on its own pinned pipeline thread instead.
class ExecuTorchRunner : public IModelRunner {
public:
    ExecuTorchRunner();
    ~ExecuTorchRunner() override;

    bool load(const std::filesystem::path &model) override;
    std::span<const TensorSpec> inputs() const override { return inputs_; }
    std::span<const TensorSpec> outputs() const override { return outputs_; }
    bool run(std::span<const TensorView> in, std::span<TensorView> out) override;

    // Size of the process-wide XNNPACK threadpool (call before the first run).
    static void set_threads(uint32_t n);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<TensorSpec> inputs_, outputs_;
};

} // namespace babble
