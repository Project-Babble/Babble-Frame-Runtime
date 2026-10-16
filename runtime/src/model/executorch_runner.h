#pragma once

#include "model/model_runner.h"

#include <memory>
#include <string>
#include <vector>

namespace babble {

// This IModelRunner runs an ExecuTorch .pte file and gets the tensor specs from the model metadata.
class ExecuTorchRunner : public IModelRunner {
public:
    ExecuTorchRunner();
    ~ExecuTorchRunner() override;

    bool load(const std::filesystem::path &model) override;
    std::span<const TensorSpec> inputs() const override { return inputs_; }
    std::span<const TensorSpec> outputs() const override { return outputs_; }
    bool run(std::span<const TensorView> in, std::span<TensorView> out) override;

    // Set the size of the process-wide XNNPACK threadpool, which ignores the CPU pinning of each model thread.
    static void set_threads(uint32_t n);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<TensorSpec> inputs_, outputs_;
};

}
