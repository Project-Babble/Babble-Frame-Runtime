#include <doctest/doctest.h>

#include "core/config.h"
#include "defaults_json.h"
#include "platform/diagnostics.h"
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
    CHECK((*d)["sources"]["eye"]["ctrl_base_candidates"].size() == 2);
    CHECK((*d)["sources"]["eye"]["rate_divisor"] == 1);
    // Every profile has dataset recording, with the recorder and the settings of the capture app.
    CHECK((*d)["sinks"]["record"]["type"] == "record");
    CHECK((*d)["sinks"]["record"]["encoder"][0] == "ffmpeg");
    CHECK((*d)["capture"]["app"]["repo"] == "Project-Babble/Babble-Guided-Dataset-Capture");
    CHECK((*d)["capture"]["upload"]["bucket"] == "babbledata");
    // The app does not offer an upload without these shipped keys, which are write-only in Baballonia.
    for (const char *k : {"endpoint", "access_key", "secret_key", "public_key"})
        CHECK_MESSAGE(!(*d)["capture"]["upload"].value(k, std::string()).empty(), k);
    CHECK((*d)["capture"]["upload"]["public_key"].get<std::string>().rfind("-----BEGIN PUBLIC KEY-----", 0) == 0);
    // The eye source has no dsp_service subscriptions (see DspClient::connect).
    CHECK((*d)["sources"]["eye"]["dsp_request"] == 0);
    CHECK_FALSE((*d)["sources"]["eye"].contains("process_fps"));   // The eye rate is a share of the camera rate.
    CHECK_FALSE((*d)["sources"]["eye"].contains("request_fps"));
    CHECK((*d)["sources"]["eye"]["type"] == "frame_dsp_eye");
    CHECK((*d)["sources"]["face"]["type"] == "babble_usb");
#if BABBLE_FRUNK_MOUTH
    CHECK((*d)["sources"]["mouth"]["process_fps"] == 120);
    CHECK((*d)["sources"]["mouth"]["nodes"]["left"] == "/dev/video9");
    CHECK((*d)["sources"]["mouth"]["type"] == "frame_ov6211_stereo");
#else
    // The release profile (cmake/babble_profile.cmake) has no Frame mouth cameras and nothing that needs them.
    for (const char *key : {"mouth_source", "arbiter"})
        CHECK_FALSE_MESSAGE(d->contains(key), key);
    CHECK_FALSE((*d)["sources"].contains("mouth"));
    CHECK_FALSE((*d)["pipelines"].contains("mouth"));
    CHECK_FALSE((*d)["threads"].contains("capL"));
    CHECK_FALSE((*d)["threads"].contains("capR"));
#endif
#if !BABBLE_DEV_FEATURES
    for (const char *key : {"output", "visualizer"})
        CHECK_FALSE_MESSAGE(d->contains(key), key);
    CHECK_FALSE((*d)["sinks"].contains("vrc_native"));
    CHECK((*d)["sinks"]["vrcft"]["enabled"] == false);
    CHECK_FALSE((*d)["pipelines"]["eye"].contains("lid_model"));
    CHECK_FALSE((*d)["pipelines"]["eye"].contains("pupil_model"));
#endif
    // Every user setting must point at a key in the defaults, because the loader drops a bad profile
    // trim with only a warning.
    for (const auto &row : (*d)["dashboard"]["settings"])
        CHECK_MESSAGE(d->contains(json::json_pointer(row.value("id", ""))), row.value("id", ""));
    for (const auto &row : (*d)["steamvr_settings"]["values"])
        CHECK_MESSAGE(d->contains(json::json_pointer(row.value("id", ""))), row.value("id", ""));
    // Every pipeline input must name a configured source.
    for (auto &[name, pc] : (*d)["pipelines"].items())
        if (pc.contains("from"))   // The gaze pipeline reads SteamVR and not a camera source.
            CHECK_MESSAGE((*d)["sources"].contains(pc.value("from", "")), name);
}

TEST_CASE("user config merges over defaults") {
    json defaults = json::parse(R"({"mouth": {"process_fps": 120, "nodes": {"left": "a", "right": "b"}},
                                   "threads": {"capL": {"cpus": [0], "nice": -5}}})");
    json user = json::parse(R"({"mouth": {"process_fps": 60}, "threads": {"capL": {"cpus": [3]}}})");
    json m = merge_config(defaults, user);
    CHECK(m["mouth"]["process_fps"] == 60);
    CHECK(m["mouth"]["nodes"]["left"] == "a");          // Keys that the merge does not touch stay.
    CHECK(m["threads"]["capL"]["cpus"] == json::array({3}));   // Arrays replace the old value.
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
    CHECK(cl.config["sources"]["mouth"]["process_fps"] == 60);   // Settings win over config.json.
    CHECK(cl.config["sources"]["eye"]["process_fps"] == 45);
    CHECK(cl.config["sources"]["eye"]["type"] == "frame_dsp_eye");   // Defaults stay.
    // The loader never writes config.json.
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
        {"eyeTracking", {"/sources/eye/tracking", Type::Bool}},
        {"eyeRateDivisor", {"/sources/eye/rate_divisor", Type::Int}},
        {"opennessSource", {"/pipelines/eye/openness_source", Type::String}},
        {"gazeSource", {"/pipelines/gaze/gaze_source", Type::String}},
        {"debugOverlay", {"/dashboard/enabled", Type::Bool}},
    };
#if BABBLE_FRUNK_MOUTH
    // Mouth Tracking controls the mouth routing, and the user also selects the source and its rate.
    want["mouthTracking"] = {"/sources/mouth/tracking", Type::Bool};
    want["mouthSource"] = {"/mouth_source", Type::String};
    want["mouthRate"] = {"/sources/mouth/process_fps", Type::Int};
#else
    // In the release profile, Mouth Tracking switches the USB tracker and there is no source or rate choice.
    want["mouthTracking"] = {"/sources/face/tracking", Type::Bool};
#endif
#if BABBLE_DEV_FEATURES
    want["output"] = {"/output", Type::String};
#endif
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
    CHECK((*d)["dashboard"]["enabled"] == false);   // The debug overlay is off by default.
#if BABBLE_FRUNK_MOUTH
    CHECK((*d)["mouth_source"] == "frunk");         // The default is the Frame cameras unless the user selects USB.
#endif
#if BABBLE_DEV_FEATURES
    CHECK((*d)["output"] == "vrcft");               // VRCFaceTracking is the default output.
#endif

    // The loader drops an id that is not a bool, number or string setting and does not mirror it.
    json cfg = *d;
    cfg["steamvr_settings"]["values"].push_back({{"key", "bogus"}, {"id", "/sources/eye/ctrl_base_candidates"}});
    CHECK(SteamVrSettingsBridge::entries_from_config(cfg).size() == want.size());
}

TEST_CASE("values of the wrong type are reset to the default and reported") {
    json defaults = {{"sources", {{"mouth", {{"tracking", true}, {"process_fps", 120}}}}},
                     {"pipelines", {{"gaze", {{"ipd", "auto"}}}, {"face", {{"one_euro", {{"beta", 3.0}}}}}}},
                     {"sinks", {{"debug", {{"dump_dir", nullptr}}}}}};
    json cfg = defaults;
    cfg["sources"]["mouth"]["tracking"] = "yes";           // A string for a boolean gets a reset.
    cfg["sources"]["mouth"]["process_fps"] = 60.5;         // An int and a float are both numbers, so the value stays.
    cfg["pipelines"]["gaze"]["ipd"] = 0.064;               // A flexible option stays.
    cfg["pipelines"]["face"]["one_euro"] = false;          // A flexible option stays.
    cfg["sinks"]["debug"]["dump_dir"] = "/tmp/x";          // A null default accepts all values.
    cfg["extra"] = 1;                                      // Keys that are not in the defaults stay.
    const auto fixed = fix_types(cfg, defaults);
    REQUIRE(fixed.size() == 1);
    CHECK(fixed[0] == "/sources/mouth/tracking: string, expected boolean");
    CHECK(cfg["sources"]["mouth"]["tracking"] == true);
    CHECK(cfg["sources"]["mouth"]["process_fps"] == 60.5);
    CHECK(cfg["pipelines"]["gaze"]["ipd"] == 0.064);
    CHECK(cfg["pipelines"]["face"]["one_euro"] == false);
    CHECK(cfg["sinks"]["debug"]["dump_dir"] == "/tmp/x");
    CHECK(cfg["extra"] == 1);
}

TEST_CASE("section() reads a config section without inserting it") {
    const json cfg = {{"arbiter", {{"root", "/etc"}}}, {"broken", 5}};
    CHECK(section(cfg, "arbiter").value("root", "") == "/etc");
    CHECK(section(cfg, "missing").empty());
    CHECK(section(cfg, "broken").is_object());   // A value that is not an object becomes an empty object.
    CHECK_FALSE(cfg.contains("missing"));
}

TEST_CASE("a broken settings.json is moved aside, not lost, and overrides are listed") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("babble_cfg_bad_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path settings = dir / "settings.json";
    {
        std::ofstream(settings) << "{ \"sources\": { \"mouth\": ";   // The file is truncated.
    }
    ConfigLoad broken = load_config(dir / "none.json", settings);
    CHECK_FALSE(broken.settings_loaded);
    CHECK_FALSE(broken.settings_error.empty());

    std::string err;
    REQUIRE(save_setting(settings, json::json_pointer("/sources/eye/process_fps"), 60, &err));
    CHECK(fs::exists(dir / "settings.json.bad"));   // The loader keeps the broken file for inspection.
    ConfigLoad cl = load_config(dir / "none.json", settings);
    REQUIRE(cl.settings_loaded);
    CHECK(cl.config["sources"]["eye"]["process_fps"] == 60);
    REQUIRE(cl.settings_overrides.size() == 1);
    CHECK(cl.settings_overrides[0] == "/sources/eye/process_fps");
    CHECK(cl.type_fixes.empty());
    fs::remove_all(dir);
}

TEST_CASE("the session log shows config changes but never upload credentials") {
    std::string err;
    auto cfg = parse_json(kDefaultsJson, &err);
    REQUIRE(cfg.has_value());
    (*cfg)["capture"]["upload"]["access_key"] = "GKaccess-value";
    (*cfg)["capture"]["upload"]["secret_key"] = "secret-value";
    (*cfg)["capture"]["upload"]["public_key"] = "-----BEGIN PUBLIC KEY-----";
    (*cfg)["capture"]["app"]["api_url"] = "http://example/releases.json";
    std::string all;
    for (const auto &line : diag::session_info(*cfg, "/nonexistent"))
        all += line + "\n";
    CHECK(all.find("access-value") == std::string::npos);
    CHECK(all.find("secret-value") == std::string::npos);
    CHECK(all.find("BEGIN PUBLIC KEY") == std::string::npos);
    CHECK(all.find("config: /capture/upload/secret_key set (not logged)") != std::string::npos);
    CHECK(all.find("http://example/releases.json") != std::string::npos);
}
