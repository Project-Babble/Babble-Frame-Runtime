#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace babble {

enum class DType { U8, F16, F32 };

struct TensorSpec {
    std::string name;
    std::vector<int64_t> shape;
    DType dtype = DType::F32;

    size_t elements() const {
        size_t n = 1;
        for (int64_t d : shape)
            n *= size_t(d);
        return n;
    }
    size_t bytes() const { return elements() * (dtype == DType::U8 ? 1 : dtype == DType::F16 ? 2 : 4); }
};

struct TensorView {
    void *data = nullptr;
    const TensorSpec *spec = nullptr;
};

// One inference model. Milestone 2 adds an ExecuTorch implementation; the
// pipeline only sees this interface, so separate eye/mouth models and a
// future merged model are just different pipeline configurations.
class IModelRunner {
public:
    virtual ~IModelRunner() = default;
    virtual bool load(const std::filesystem::path &model) = 0;
    virtual std::span<const TensorSpec> inputs() const = 0;
    virtual std::span<const TensorSpec> outputs() const = 0;
    virtual bool run(std::span<const TensorView> in, std::span<TensorView> out) = 0;
};

} // namespace babble
