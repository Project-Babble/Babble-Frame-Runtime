#include "camera/tracking_source.h"

#include "camera/eye/dsp_eye_source.h"
#include "camera/mouth/frame_mouth_source.h"
#include "core/log.h"

namespace babble {

std::unique_ptr<ITrackingSource> make_tracking_source(const nlohmann::json &cfg, SourceDeps deps) {
    const std::string type = cfg.value("type", "");
    if (type == "frame_ov6211_stereo")
        return std::make_unique<FrameMouthSource>(cfg, std::move(deps));
    if (type == "frame_dsp_eye")
        return std::make_unique<DspEyeSource>(cfg, std::move(deps));
    LOG_ERROR("source %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

} // namespace babble
