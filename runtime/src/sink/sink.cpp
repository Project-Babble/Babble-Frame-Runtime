#include "sink/sink.h"

#include "core/log.h"
#include "sink/babble_osc_sink.h"
#include "sink/camera_stream_sink.h"
#include "sink/debug_sink.h"
#include "sink/link_sink.h"
#include "sink/osc_sink.h"
#include "sink/plugin_sink.h"
#include "sink/record_sink.h"
#include "sink/vrc_native_sink.h"

namespace {
const babble::log::Logger LOG("sink");
}

namespace babble {

std::unique_ptr<ISink> make_sink(const nlohmann::json &cfg, SinkDeps deps) {
    const std::string type = cfg.value("type", "");
    if (type == "debug")
        return std::make_unique<DebugSink>(cfg, std::move(deps));
    if (type == "osc")
        return std::make_unique<OscSink>(cfg, std::move(deps));
    if (type == "vrcft_link")
        return std::make_unique<LinkSink>(cfg, std::move(deps));
    if (type == "plugin")
        return std::make_unique<PluginSink>(cfg, std::move(deps));
    if (type == "camera_stream")
        return std::make_unique<CameraStreamSink>(cfg, std::move(deps));
    if (type == "vrc_native")
        return std::make_unique<VrcNativeSink>(cfg, std::move(deps));
    if (type == "record")
        return std::make_unique<RecordSink>(cfg, std::move(deps));
    if (type == "babble_osc")
        return std::make_unique<BabbleOscSink>(cfg, std::move(deps));
    LOG.error("sink %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

}
