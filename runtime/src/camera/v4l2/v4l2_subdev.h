#pragma once

#include <optional>
#include <string>
#include <vector>

namespace babble {

// Finds a V4L2 subdev node by its entity name prefix, because the subdev numbers change between boots.
std::optional<std::string> find_subdev_by_name(const std::vector<std::string> &prefixes,
                                               std::string *matched_name = nullptr);

}
