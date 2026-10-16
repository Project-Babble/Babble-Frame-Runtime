#include "camera/tracking_source.h"

#include "camera/eye/dsp_eye_source.h"
#include "camera/usb/babble_usb_source.h"
#if BABBLE_FRUNK_MOUTH
#include "camera/mouth/frame_mouth_source.h"
#endif
#include "core/log.h"

namespace {
const babble::log::Logger LOG("source");
}

namespace babble {

std::unique_ptr<ITrackingSource> make_tracking_source(const nlohmann::json &cfg, SourceDeps deps) {
    const std::string type = cfg.value("type", "");
#if BABBLE_FRUNK_MOUTH
    if (type == "frame_ov6211_stereo") {
        if (!deps.arbiter) {
            LOG.error("source %s: frame_ov6211_stereo needs the camera arbiter", deps.name.c_str());
            return nullptr;
        }
        return std::make_unique<FrameMouthSource>(cfg, std::move(deps));
    }
#endif
    if (type == "frame_dsp_eye")
        return std::make_unique<DspEyeSource>(cfg, std::move(deps));
    if (type == "babble_usb")
        return std::make_unique<BabbleUsbSource>(cfg, std::move(deps));
    LOG.error("source %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

}
