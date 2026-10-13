#include "capture/github_release.h"

#include <cctype>
#include <sstream>

namespace babble::capture {

using json = nlohmann::json;

std::vector<Release> parse_releases(const json &api) {
    std::vector<Release> out;
    if (!api.is_array())
        return out;
    for (const auto &r : api) {
        if (!r.is_object())
            continue;
        Release rel;
        rel.tag = r.value("tag_name", "");
        rel.name = r.value("name", rel.tag);
        rel.draft = r.value("draft", false);
        rel.prerelease = r.value("prerelease", false);
        for (const auto &a : r.value("assets", json::array())) {
            if (!a.is_object())
                continue;
            Asset as;
            as.name = a.value("name", "");
            as.url = a.value("browser_download_url", "");
            as.size = a.value("size", uint64_t(0));
            if (!as.name.empty() && !as.url.empty())
                rel.assets.push_back(std::move(as));
        }
        out.push_back(std::move(rel));
    }
    return out;
}

std::vector<Candidate> candidates(const std::vector<Release> &releases, const std::string &prefix, bool prereleases) {
    const std::string suffix = "-linuxarm64.tar.gz";
    std::vector<Candidate> out;
    for (const auto &r : releases) {
        if (r.draft || (r.prerelease && !prereleases))
            continue;
        const Asset *tar = nullptr, *manifest = nullptr;
        for (const auto &a : r.assets) {
            if (a.name.size() > prefix.size() + suffix.size() && a.name.rfind(prefix, 0) == 0 &&
                a.name.compare(a.name.size() - suffix.size(), suffix.size(), suffix) == 0)
                tar = &a;
            if (a.name == "manifest.json")
                manifest = &a;
        }
        if (!tar || !manifest)
            continue;
        const Asset *sha = nullptr;
        for (const auto &a : r.assets)
            if (a.name == tar->name + ".sha256")
                sha = &a;
        if (!sha)
            continue;
        out.push_back({r, *tar, *sha, *manifest});
    }
    return out;
}

std::optional<Manifest> parse_manifest(const json &j, std::string *err) {
    auto fail = [&](const std::string &why) -> std::optional<Manifest> {
        if (err)
            *err = why;
        return std::nullopt;
    };
    if (!j.is_object())
        return fail("manifest.json is not an object");
    Manifest m;
    if (!j.contains("protocol") || !j["protocol"].is_number_integer())
        return fail("manifest.json has no protocol number");
    m.protocol = j["protocol"].get<int>();
    m.name = j.value("name", "Dataset Capture");
    m.version = j.value("version", "");
    m.entry = j.value("entry", "");
    if (m.version.empty())
        return fail("manifest.json has no version");
    if (m.entry.empty())
        return fail("manifest.json has no entry");
    if (m.entry[0] == '/')
        return fail("manifest.json entry must be relative");
    std::stringstream parts(m.entry);
    for (std::string p; std::getline(parts, p, '/');)
        if (p == "..")
            return fail("manifest.json entry leaves the app folder");
    return m;
}

std::optional<std::string> parse_sha256_file(const std::string &text, const std::string &file) {
    auto is_hex64 = [](const std::string &s) {
        if (s.size() != 64)
            return false;
        for (char c : s)
            if (!isxdigit(static_cast<unsigned char>(c)))
                return false;
        return true;
    };
    auto lower = [](std::string s) {
        for (char &c : s)
            c = char(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::stringstream lines(text);
    std::optional<std::string> bare;
    for (std::string line; std::getline(lines, line);) {
        std::stringstream words(line);
        std::string hex, name;
        words >> hex >> name;
        if (!is_hex64(hex))
            continue;
        if (!name.empty() && name[0] == '*')   // sha256sum's binary-mode marker
            name.erase(0, 1);
        if (name == file)
            return lower(hex);
        if (name.empty())
            bare = lower(hex);
    }
    return bare;
}

} // namespace babble::capture
