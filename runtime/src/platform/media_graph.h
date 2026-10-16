#pragma once

#include <string>
#include <vector>

namespace babble {

// The result of a read-only check that each named sensor has enabled data links to a V4L2 capture node.
struct RouteCheck {
    bool ok = false;
    std::vector<std::string> unrouted;   // The sensors that do not have a complete route.
    std::string error;                   // This is not empty if the check could not read the graph.
};

RouteCheck check_sensor_routes(const std::string &media_dev, const std::vector<std::string> &sensor_prefixes);

}
