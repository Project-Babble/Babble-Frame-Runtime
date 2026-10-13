#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace babble::capture {

// Picking the dataset capture app's release from GitHub's releases API, and
// checking what it ships. Pure (no network): tested on canned JSON.

struct Asset {
    std::string name, url;   // url: browser_download_url
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

// The app's manifest.json: {"name", "version", "protocol", "entry"}.
struct Manifest {
    std::string name, version, entry;
    int protocol = 0;
};

// GET /repos/<owner>/<repo>/releases (newest first).
std::vector<Release> parse_releases(const nlohmann::json &api);

// Releases with a tarball named <prefix><version>-linuxarm64.tar.gz, its
// .sha256, and manifest.json, newest first. Drafts never; prereleases only if asked.
std::vector<Candidate> candidates(const std::vector<Release> &releases, const std::string &prefix, bool prereleases);

// A manifest, or nullopt with *err. entry must be a relative path inside the
// app folder (no "..", not absolute).
std::optional<Manifest> parse_manifest(const nlohmann::json &j, std::string *err);

// The digest for `file` from a .sha256 file ("<hex>  <name>" lines, or a bare
// hex digest); lowercase hex, or nullopt.
std::optional<std::string> parse_sha256_file(const std::string &text, const std::string &file);

} // namespace babble::capture
