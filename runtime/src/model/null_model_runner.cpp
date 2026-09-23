#include "model/null_model_runner.h"

#include <cstring>

namespace babble {

NullModelRunner::NullModelRunner(std::vector<TensorSpec> inputs, std::vector<TensorSpec> outputs)
    : inputs_(std::move(inputs)), outputs_(std::move(outputs)) {}

bool NullModelRunner::run(std::span<const TensorView> in, std::span<TensorView> out) {
    if (in.size() != inputs_.size() || out.size() != outputs_.size())
        return false;
    for (auto &t : out)
        memset(t.data, 0, t.spec->bytes());
    return true;
}

} // namespace babble
