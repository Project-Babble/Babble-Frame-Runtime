#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace babble::update {

// A release version, "V<major>.<minor> <Alpha|Beta|RC|Release> [N]" (CMakeLists.txt);
// the git-tag spelling with hyphens ("V1.0-Alpha-4") parses the same. A missing
// N is the stage's first build ("V0.1 Alpha" = "V0.1 Alpha 1").
struct Version {
    enum Stage { Alpha, Beta, RC, Release };
    int major = 0, minor = 0;
    Stage stage = Alpha;
    int n = 1;

    bool prerelease() const { return stage != Release; }
    std::string tag() const;   // "V1.0-Alpha-4"
    auto operator<=>(const Version &) const = default;
};

std::optional<Version> parse_version(std::string_view text);

} // namespace babble::update
