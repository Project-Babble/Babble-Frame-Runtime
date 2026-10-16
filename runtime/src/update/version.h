#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace babble::update {

// A release version in the form "V<major>.<minor> <Alpha|Beta|RC|Release> [N]", where a missing N is 1.
struct Version {
    enum Stage { Alpha, Beta, RC, Release };
    int major = 0, minor = 0;
    Stage stage = Alpha;
    int n = 1;

    bool prerelease() const { return stage != Release; }
    std::string tag() const;   // Returns the git tag, for example "V1.0-Alpha-4".
    auto operator<=>(const Version &) const = default;
};

std::optional<Version> parse_version(std::string_view text);

}
