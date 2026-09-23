#include "sink/sink.h"

#include "core/log.h"
#include "sink/debug_sink.h"

namespace babble {

std::unique_ptr<ISink> make_sink(const nlohmann::json &cfg, SinkDeps deps) {
    const std::string type = cfg.value("type", "");
    if (type == "debug")
        return std::make_unique<DebugSink>(cfg, std::move(deps));
    LOG_ERROR("sink %s: unknown type '%s'", deps.name.c_str(), type.c_str());
    return nullptr;
}

} // namespace babble
