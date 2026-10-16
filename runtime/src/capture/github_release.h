#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace babble::capture {

struct Asset {
    std::string name, url;   // The url is the browser_download_url.
    uint64_t size = 0;
};

struct Release {
    std::string tag, name;
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
};

// One release that has everything an install needs.
struct Candidate {
    Release release;
    Asset tarball, sha256, manifest;
};

// The manifest.json of the capture app.
struct Manifest {
    std::string name, version, entry;
    int protocol = 0;
};

// Parses the reply of GET /repos/<owner>/<repo>/releases, which has the newest release first.
std::vector<Release> parse_releases(const nlohmann::json &api);

// Returns the releases that have the tarball, its .sha256 and manifest.json, with the newest first.
std::vector<Candidate> candidates(const std::vector<Release> &releases, const std::string &prefix, bool prereleases);

// Returns the manifest, or nullopt with *err if entry is not a relative path inside the app folder.
std::optional<Manifest> parse_manifest(const nlohmann::json &j, std::string *err);

// Returns the lowercase hex digest for `file` from the text of a .sha256 file, or nullopt.
std::optional<std::string> parse_sha256_file(const std::string &text, const std::string &file);

}
