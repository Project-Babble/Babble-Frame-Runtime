#include "camera/mouth/exposure_controller.h"

#include "core/log.h"

#include <array>

namespace {
const babble::log::Logger LOG("mouth");
} // namespace

namespace babble {

FrameStats compute_frame_stats(const Frame &frame, uint32_t step) {
    FrameStats s;
    s.capture_ns = frame.capture_ns;
    std::array<uint32_t, 256> hist{};
    uint64_t sum = 0, n = 0;
    for (uint32_t y = 0; y < frame.height; y += step) {
        const uint8_t *row = frame.data + size_t(y) * frame.stride;
        for (uint32_t x = 0; x < frame.width; x += step) {
            hist[row[x]]++;
            sum += row[x];
            n++;
        }
    }
    if (!n)
        return s;
    s.mean = float(sum) / float(n);
    s.clipped_frac = float(hist[255]) / float(n);
    uint64_t acc = 0;
    bool have_p05 = false;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (!have_p05 && acc * 20 >= n) {
            s.p05 = float(v);
            have_p05 = true;
        }
        if (acc * 20 >= n * 19) {
            s.p95 = float(v);
            break;
        }
    }
    return s;
}

std::unique_ptr<IExposureController> make_exposure_controller(const nlohmann::json &cfg) {
    std::string mode = cfg.value("mode", "fixed");
    if (mode != "fixed")
        LOG.warn("auto_exposure mode '%s' not implemented yet, using fixed", mode.c_str());
    return std::make_unique<FixedExposure>();
}

} // namespace babble
