#pragma once

#include "frame/frame.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>

namespace babble {

struct FrameStats {
    float mean = 0, p05 = 0, p95 = 0;
    float clipped_frac = 0;   // The fraction of samples at 255.
    uint64_t capture_ns = 0;
};

// Samples every `step`-th pixel in both directions.
FrameStats compute_frame_stats(const Frame &frame, uint32_t step = 8);

struct ExposureCmd {
    std::optional<int64_t> exposure_lines;
    std::optional<int64_t> analogue_gain;
};

// An internal auto-exposure hook that gets statistics from the mouth source and returns the next settings.
class IExposureController {
public:
    virtual ~IExposureController() = default;
    virtual void observe(const FrameStats &stats) = 0;
    virtual std::optional<ExposureCmd> next() = 0;
};

// Keeps the configured exposure and gain.
class FixedExposure : public IExposureController {
public:
    void observe(const FrameStats &) override {}
    std::optional<ExposureCmd> next() override { return std::nullopt; }
};

std::unique_ptr<IExposureController> make_exposure_controller(const nlohmann::json &cfg);

}
