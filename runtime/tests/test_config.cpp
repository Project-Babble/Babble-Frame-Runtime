#include <doctest/doctest.h>

#include "core/config.h"
#include "defaults_json.h"
#include "platform/steamvr_settings.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include <unistd.h>

using namespace babble;

TEST_CASE("built-in defaults parse and carry the key settings") {
    std::string err;
    auto d = parse_json(kDefaultsJson, &err);
    REQUIRE_MESSAGE(d.has_value(), err);
    CHECK((*d)["sources"]["mouth"]["process_fps"] == 120);
    CHECK((*d)["sources"]["mouth"]["nodes"]["left"] == "/dev/video9");
    CHECK((*d)["sources"]["eye"]["ctrl_base_candidates"].size() == 2);
    CHECK((*d)["sources"]["eye"]["process_fps"] == 90);
    CHECK((*d)["sources"]["mouth"]["type"] == "frame_ov6211_stereo");
    CHECK((*d)["sources"]["eye"]["type"] == "frame_dsp_eye");
    // Every pipeline input names a configured source.
    for (auto &[name, pc] : (*d)["pipelines"].items())
        if (pc.contains("from"))   // gaze reads SteamVR, not a camera source
            CHECK_MESSAGE((*d)["sources"].contains(pc.value("from", "")), name);
}

TEST_CASE("user config merges over defaults") {
    json defaults = json::parse(R"({"mouth": {"process_fps": 120, "nodes": {"left": "a", "right": "b"}},
                                   "threads": {"capL": {"cpus": [0], "nice": -5}}})");
    json user = json::parse(R"({"mouth": {"process_fps": 60}, "threads": {"capL": {"cpus": [3]}}})");
    json m = merge_config(defaults, user);
    CHECK(m["mouth"]["process_fps"] == 60);
    CHECK(m["mouth"]["nodes"]["left"] == "a");          // untouched keys survive
    CHECK(m["threads"]["capL"]["cpus"] == json::array({3}));   // arrays replace
    CHECK(m["threads"]["capL"]["nice"] == -5);
}

TEST_CASE("comments are allowed and broken JSON is reported") {
    std::string err;
    CHECK(parse_json("{ // tracking rate\n \"a\": 1 }", &err).has_value());
    CHECK_FALSE(parse_json("{ \"a\": }", &err).has_value());
    CHECK_FALSE(err.empty());
}

TEST_CASE("settings.json layers over config.json and save_setting keeps other values") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("babble_cfg_test_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path user = dir / "config.json", settings = dir / "settings.json";
    std::ofstream(user) << R"({ // hand-edited
        "sources": {"mouth": {"process_fps": 90}, "eye": {"process_fps": 60}}})";

    std::string err;
    REQUIRE(save_setting(settings, json::json_pointer("/sources/mouth/process_fps"), 30, &err));
    REQUIRE(save_setting(settings, json::json_pointer("/sources/eye/process_fps"), 45, &err));
    REQUIRE(save_setting(settings, json::json_pointer("/sources/mouth/process_fps"), 60, &err));

    ConfigLoad cl = load_config(user, settings);
    CHECK(cl.user_loaded);
    CHECK(cl.settings_loaded);
    CHECK(cl.config["sources"]["mouth"]["process_fps"] == 60);   // settings win over config.json
    CHECK(cl.config["sources"]["eye"]["process_fps"] == 45);
    CHECK(cl.config["sources"]["mouth"]["type"] == "frame_ov6211_stereo");   // defaults survive
    // config.json is never rewritten.
    std::ifstream in(user);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text.find("hand-edited") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("the SteamVR Settings section maps to live runtime settings") {
    std::string err;
    auto d = parse_json(kDefaultsJson, &err);
    REQUIRE(d);
    CHECK((*d)["steamvr_settings"]["section"] == "driver_babble");
    using Type = SteamVrSettingsBridge::Entry::Type;
    std::map<std::string, std::pair<std::string, Type>> want = {
        {"mouthTracking", {"/sources/mouth/tracking", Type::Bool}},
        {"mouthSource", {"/mouth_source", Type::String}},
        {"mouthRate", {"/sources/mouth/process_fps", Type::Int}},
        {"eyeTracking", {"/sources/eye/tracking", Type::Bool}},
        {"eyeRate", {"/sources/eye/process_fps", Type::Int}},
        {"opennessSource", {"/pipelines/eye/openness_source", Type::String}},
        {"gazeSource", {"/pipelines/gaze/gaze_source", Type::String}},
        {"debugOverlay", {"/dashboard/enabled", Type::Bool}},
    };
    auto entries = SteamVrSettingsBridge::entries_from_config(*d);
    REQUIRE(entries.size() == want.size());
    for (const auto &e : entries) {
        CAPTURE(e.key);
        REQUIRE(want.count(e.key));
        CHECK(e.id == want[e.key].first);
        CHECK(e.type == want[e.key].second);
    }
    // The advanced options default to model openness and independent gaze.
    CHECK((*d)["pipelines"]["eye"]["openness_source"] == "model");
    CHECK((*d)["pipelines"]["gaze"]["gaze_source"] == "independent");
    CHECK((*d)["dashboard"]["enabled"] == false);   // the debug overlay is opt-in
    CHECK((*d)["mouth_source"] == "frunk");         // the Frame's own mouth cameras unless USB is chosen

    // An id that isn't a bool/number/string setting is dropped rather than mirrored.
    json cfg = *d;
    cfg["steamvr_settings"]["values"].push_back({{"key", "bogus"}, {"id", "/sources/mouth/nodes"}});
    CHECK(SteamVrSettingsBridge::entries_from_config(cfg).size() == want.size());
}
