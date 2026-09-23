#pragma once

#include "model/model_runner.h"

namespace babble {

// Stand-in until ExecuTorch lands: declares the configured tensor shapes and
// outputs zeros, so the whole pipeline (preprocess, timing, state, sinks)
// runs for real.
class NullModelRunner : public IModelRunner {
public:
    NullModelRunner(std::vector<TensorSpec> inputs, std::vector<TensorSpec> outputs);

    bool load(const std::filesystem::path &) override { return true; }
    std::span<const TensorSpec> inputs() const override { return inputs_; }
    std::span<const TensorSpec> outputs() const override { return outputs_; }
    bool run(std::span<const TensorView> in, std::span<TensorView> out) override;

private:
    std::vector<TensorSpec> inputs_, outputs_;
};

} // namespace babble
