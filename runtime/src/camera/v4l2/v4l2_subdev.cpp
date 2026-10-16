#include "camera/v4l2/v4l2_subdev.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace babble {

namespace fs = std::filesystem;

std::optional<std::string> find_subdev_by_name(const std::vector<std::string> &prefixes, std::string *matched_name) {
    std::vector<std::pair<std::string, std::string>> subdevs; // Each pair is a node and its entity name.
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/video4linux", ec)) {
        std::string node = entry.path().filename().string();
        if (node.rfind("v4l-subdev", 0) != 0)
            continue;
        std::ifstream in(entry.path() / "name");
        std::string name;
        std::getline(in, name);
        subdevs.emplace_back(node, name);
    }
    std::sort(subdevs.begin(), subdevs.end());
    for (const auto &prefix : prefixes) {
        for (const auto &[node, name] : subdevs) {
            if (name.rfind(prefix, 0) == 0) {
                if (matched_name)
                    *matched_name = name;
                return "/dev/" + node;
            }
        }
    }
    return std::nullopt;
}

}
