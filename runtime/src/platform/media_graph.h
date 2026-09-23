#pragma once

#include <string>
#include <vector>

namespace babble {

// Read-only check of the media-controller graph: does each named sensor have
// a chain of enabled data links down to a V4L2 capture node? The camera
// arbiter's streams unit stays "active" after routing, even if something
// (e.g. XRService restarting with SteamVR) later disables our links, so the
// unit state alone can't be trusted.
struct RouteCheck {
    bool ok = false;
    std::vector<std::string> unrouted;   // sensors without a complete route
    std::string error;                   // non-empty if the graph could not be read
};

RouteCheck check_sensor_routes(const std::string &media_dev, const std::vector<std::string> &sensor_prefixes);

} // namespace babble
