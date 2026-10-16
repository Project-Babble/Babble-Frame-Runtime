#include "update/version.h"

#include <cctype>
#include <vector>

namespace babble::update {

namespace {
const char *const kStages[] = {"Alpha", "Beta", "RC", "Release"};

std::optional<int> number(const std::string &s) {
    if (s.empty() || s.size() > 6)
        return std::nullopt;
    for (char c : s)
        if (!isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
    return std::stoi(s);
}
}

std::string Version::tag() const {
    std::string t = "V" + std::to_string(major) + "." + std::to_string(minor) + "-" + kStages[stage];
    return t + "-" + std::to_string(n);
}

std::optional<Version> parse_version(std::string_view text) {
    // Split the text into words at spaces and hyphens.
    std::vector<std::string> words;
    std::string w;
    for (char c : text) {
        if (c == ' ' || c == '-') {
            if (!w.empty())
                words.push_back(std::move(w));
            w.clear();
        } else {
            w += c;
        }
    }
    if (!w.empty())
        words.push_back(std::move(w));
    if (words.size() < 2 || words.size() > 3 || words[0].size() < 4 || words[0][0] != 'V')
        return std::nullopt;
    const size_t dot = words[0].find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    const auto major = number(words[0].substr(1, dot - 1)), minor = number(words[0].substr(dot + 1));
    if (!major || !minor)
        return std::nullopt;
    Version v;
    v.major = *major;
    v.minor = *minor;
    bool stage = false;
    for (int s = Version::Alpha; s <= Version::Release; s++)
        if (words[1] == kStages[s]) {
            v.stage = Version::Stage(s);
            stage = true;
        }
    if (!stage)
        return std::nullopt;
    if (words.size() == 3) {
        const auto n = number(words[2]);
        if (!n || *n < 1)
            return std::nullopt;
        v.n = *n;
    }
    return v;
}

}
