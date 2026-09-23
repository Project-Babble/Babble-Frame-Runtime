#pragma once

#include <optional>
#include <string>
#include <vector>

namespace babble {

// Finds a V4L2 subdev node (/dev/v4l-subdevN) whose entity name starts with
// one of the prefixes, tried in order. Subdev numbers change between boots,
// so sensors are always located by name.
std::optional<std::string> find_subdev_by_name(const std::vector<std::string> &prefixes,
                                               std::string *matched_name = nullptr);

} // namespace babble
