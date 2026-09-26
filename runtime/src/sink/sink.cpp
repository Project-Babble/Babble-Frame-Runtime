#include "sink/sink.h"

#include "core/log.h"
#include "sink/debug_sink.h"
#include "sink/osc_sink.h"
#include "sink/plugin_sink.h"

namespace babble {

std::unique_ptr<ISink> make_sink(const nlohmann::json &cfg, SinkDeps deps) {
    const std::string type = cfg.value("type", "");
    if (type == "debug")
        return std::make_unique<DebugSink>(cfg, std::move(deps));
    if (type == "osc")
        return std::make_unique<OscSink>(cfg, std::move(deps));
    if (type == "plugin")
        return std::make_unique<PluginSink>(cfg, std::move(deps));
    LOG_ERROR("sink %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

} // namespace babble
