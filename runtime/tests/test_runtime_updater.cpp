#include <doctest/doctest.h>

#include "core/sha256.h"
#include "platform/process.h"
#include "update/runtime_updater.h"
#include "update/version.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <unistd.h>

using namespace babble;
using namespace babble::update;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json release(const std::string &tag, bool draft, bool pre, bool with_sha = true) {
    const std::string name = "babble-frame-runtime.linuxarm64." + tag + ".tar.xz";
    json a = json::array({{{"name", name}, {"browser_download_url", "https://example/" + name}, {"size", 10}}});
    if (with_sha)
        a.push_back({{"name", name + ".sha256"}, {"browser_download_url", "https://example/" + name + ".sha256"}, {"size", 1}});
    return {{"tag_name", tag}, {"draft", draft}, {"prerelease", pre}, {"assets", a}};
}

void write(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

template <typename Pred> bool wait_for(Pred p, int ms = 20000) {
    for (int i = 0; i < ms / 20; i++) {
        if (p())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p();
}

}

TEST_CASE("versions: both spellings parse, and order by number, stage, build") {
    const auto a = parse_version("V1.0 Alpha 4");
    REQUIRE(a);
    CHECK(a->major == 1);
    CHECK(a->minor == 0);
    CHECK(a->stage == Version::Alpha);
    CHECK(a->n == 4);
    CHECK(a->prerelease());
    CHECK(a->tag() == "V1.0-Alpha-4");
    CHECK(parse_version("V1.0-Alpha-4") == a);
    CHECK(parse_version("V0.1 Alpha")->n == 1);   // Without N, the version is the first build of the stage.
    CHECK_FALSE(parse_version("V1.0 Release")->prerelease());

    CHECK(*parse_version("V1.0 Alpha 4") < *parse_version("V1.0 Alpha 10"));
    CHECK(*parse_version("V1.0 Alpha 10") < *parse_version("V1.0 Beta"));
    CHECK(*parse_version("V1.0 RC 3") < *parse_version("V1.0 Release"));
    CHECK(*parse_version("V1.0 Release 2") < *parse_version("V1.1 Alpha"));
    CHECK(*parse_version("V0.9 Release") < *parse_version("V1.0 Alpha"));

    for (const char *bad : {"", "1.0 Alpha", "V1 Alpha", "V1.0", "V1.0 Gamma", "V1.0 Alpha 0", "V1.0 Alpha x",
                            "V1.0 Alpha 4 (dev 1e20047)", "v0.2.0"})
        CHECK_MESSAGE(!parse_version(bad), bad);
}

TEST_CASE("runtime updates: the newest newer release with a Steam Frame build") {
    const json api = json::array({release("V1.0-Alpha-6", true, true),   // This release is a draft.
                                  release("V1.0-Alpha-5", false, true),
                                  release("V1.0-Beta-1", false, true, false),   // This release has no .sha256 file.
                                  release("V0.9-Release", false, false),
                                  release("not-a-version", false, false)});
    const Version alpha4 = *parse_version("V1.0 Alpha 4");
    auto u = pick_update(api, alpha4, true);
    REQUIRE(u);
    CHECK(u->tag == "V1.0-Alpha-5");
    CHECK(u->prerelease);
    CHECK(u->tarball.name == "babble-frame-runtime.linuxarm64.V1.0-Alpha-5.tar.xz");
    CHECK(u->sha256.name == u->tarball.name + ".sha256");
    CHECK_FALSE(pick_update(api, alpha4, false));                       // Only prereleases are newer.
    CHECK_FALSE(pick_update(api, *parse_version("V1.0 Alpha 5"), true));   // No release is newer.
    // The order in the reply does not matter, only the version does.
    const json shuffled = json::array({release("V1.0-Alpha-5", false, true), release("V1.0-Release", false, false),
                                       release("V1.0-RC-2", false, true)});
    CHECK(pick_update(shuffled, alpha4, true)->tag == "V1.0-Release");
    CHECK(pick_update(shuffled, alpha4, false)->tag == "V1.0-Release");
}

TEST_CASE("runtime updates: only release builds that install.sh put in place") {
    const fs::path root = fs::temp_directory_path() / ("babble_upd_kind_" + std::to_string(getpid())) / "babble";
    fs::remove_all(root.parent_path());
    RuntimeUpdater::Options o{"V1.0 Alpha 4", false, root};
    CHECK(RuntimeUpdater::unsupported_reason(o).find("driver folder") != std::string::npos);
    write(root / "driver.vrdrivermanifest", "{}");
    // Without a marker, the install source is Steam.
    CHECK(RuntimeUpdater::unsupported_reason(o).find("Steam") != std::string::npos);
    write(root / ".install-source", "install.sh\n");
    CHECK(RuntimeUpdater::unsupported_reason(o) == "");
    CHECK(RuntimeUpdater::unsupported_reason({"V1.0 Alpha 4", true, root}).find("dev") != std::string::npos);
    CHECK_FALSE(RuntimeUpdater::unsupported_reason({"V1.0", false, root}).empty());
    RuntimeUpdater off(json{{"enabled", false}}, o, json::object());
    CHECK(off.snapshot().state == RuntimeUpdater::State::Unsupported);
    fs::remove_all(root.parent_path());
}

TEST_CASE("runtime updates: download, check and run the release's install.sh from file:// URLs") {
    if (!Process::find_in_path("curl") || !Process::find_in_path("tar") || !Process::find_in_path("xz"))
        return;   // The updater needs curl, tar and xz.
    const fs::path base = fs::temp_directory_path() / ("babble_upd_" + std::to_string(getpid()));
    fs::remove_all(base);
    const fs::path root = base / "install" / "babble", rel = base / "release", pkg = base / "pkg";
    write(root / "driver.vrdrivermanifest", "{}");
    write(root / ".install-source", "install.sh\n");
    // This fake install.sh records how the updater called it.
    write(pkg / "install.sh", "#!/bin/bash\necho \"args: $*\" > \"$3/installed-by-update\"\necho done\n");
    write(pkg / "babble" / "driver.vrdrivermanifest", "{}");
    const std::string tag = "V1.0-Alpha-5", name = "babble-frame-runtime.linuxarm64." + tag + ".tar.xz";
    fs::create_directories(rel);
    REQUIRE(Process::run({"tar", "-cJf", (rel / name).string(), "-C", pkg.string(), "install.sh", "babble"}, nullptr,
                         nullptr, 20000) == 0);
    auto publish = [&](const std::string &digest) {
        write(rel / (name + ".sha256"), digest + "  " + name + "\n");
        json assets = json::array();
        for (const auto &n : {name, name + ".sha256"})
            assets.push_back({{"name", n}, {"browser_download_url", "file://" + (rel / n).string()},
                              {"size", fs::file_size(rel / n)}});
        write(rel / "releases.json",
              json::array({{{"tag_name", tag}, {"draft", false}, {"prerelease", true}, {"assets", assets}}}).dump());
    };
    const json cfg = {{"api_url", "file://" + (rel / "releases.json").string()}};
    const RuntimeUpdater::Options o{"V1.0 Alpha 4", false, root};

    publish(std::string(64, '0'));   // The digest is wrong.
    {
        RuntimeUpdater bad(cfg, o, json::object());
        bad.start();
        bad.install();
        REQUIRE(wait_for([&] { return bad.snapshot().state == RuntimeUpdater::State::Failed; }));
        CHECK(bad.snapshot().detail.find("checksum") != std::string::npos);
        CHECK_FALSE(fs::exists(root / "installed-by-update"));
    }

    publish(*Sha256::file_hex(rel / name));
    RuntimeUpdater up(cfg, o, json::object());
    CHECK(up.snapshot().state == RuntimeUpdater::State::Idle);
    up.start();
    up.check();
    REQUIRE(wait_for([&] { return up.snapshot().state == RuntimeUpdater::State::Available; }));
    CHECK(up.snapshot().latest == "V1.0 Alpha 5");
    up.install();
    REQUIRE(wait_for([&] { return up.snapshot().state == RuntimeUpdater::State::Installed; }));
    std::ifstream f(root / "installed-by-update");
    std::string line;
    std::getline(f, line);
    CHECK(line.find("--dir " + root.string() + " --no-restart") != std::string::npos);
    CHECK_FALSE(fs::exists(base / "install" / ".babble-runtime-update"));
    up.check();   // After an install, the updater does nothing more until SteamVR restarts into the new version.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(up.snapshot().state == RuntimeUpdater::State::Installed);
    fs::remove_all(base);
}
