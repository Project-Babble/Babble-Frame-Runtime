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
    // Dataset recording ships in every profile: the recorder and the capture app's settings.
    CHECK((*d)["sinks"]["record"]["type"] == "record");
    CHECK((*d)["sinks"]["record"]["encoder"][0] == "ffmpeg");
    CHECK((*d)["capture"]["app"]["repo"] == "Project-Babble/Babble-Guided-Dataset-Capture");
    CHECK((*d)["capture"]["upload"]["bucket"] == "babbledata");
    CHECK_FALSE((*d)["sources"]["eye"].contains("process_fps"));   // the eye rate is a share of SteamVR's camera rate
    CHECK_FALSE((*d)["sources"]["eye"].contains("request_fps"));
    CHECK((*d)["sources"]["eye"]["type"] == "frame_dsp_eye");
    CHECK((*d)["sources"]["face"]["type"] == "babble_usb");
#if BABBLE_FRUNK_MOUTH
    CHECK((*d)["sources"]["mouth"]["process_fps"] == 120);
    CHECK((*d)["sources"]["mouth"]["nodes"]["left"] == "/dev/video9");
    CHECK((*d)["sources"]["mouth"]["type"] == "frame_ov6211_stereo");
#else
    // The release profile (cmake/babble_profile.cmake): no Frame mouth cameras, nothing that needs them.
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
    // Every user-facing setting points at something in the defaults (a bad
    // profile trim would otherwise be dropped silently with a warning).
    for (const auto &row : (*d)["dashboard"]["settings"])
        CHECK_MESSAGE(d->contains(json::json_pointer(row.value("id", ""))), row.value("id", ""));
    for (const auto &row : (*d)["steamvr_settings"]["values"])
        CHECK_MESSAGE(d->contains(json::json_pointer(row.value("id", ""))), row.value("id", ""));
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
    CHECK(cl.config["sources"]["eye"]["type"] == "frame_dsp_eye");   // defaults survive
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
        {"eyeTracking", {"/sources/eye/tracking", Type::Bool}},
        {"eyeRateDivisor", {"/sources/eye/rate_divisor", Type::Int}},
        {"opennessSource", {"/pipelines/eye/openness_source", Type::String}},
        {"gazeSource", {"/pipelines/gaze/gaze_source", Type::String}},
        {"debugOverlay", {"/dashboard/enabled", Type::Bool}},
    };
#if BABBLE_FRUNK_MOUTH
    // Mouth Tracking drives the mouth routing; the source and its rate are chosen too.
    want["mouthTracking"] = {"/sources/mouth/tracking", Type::Bool};
    want["mouthSource"] = {"/mouth_source", Type::String};
    want["mouthRate"] = {"/sources/mouth/process_fps", Type::Int};
#else
    // Release: Mouth Tracking switches the USB tracker itself; no source or rate choice.
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
    CHECK((*d)["dashboard"]["enabled"] == false);   // the debug overlay is opt-in
#if BABBLE_FRUNK_MOUTH
    CHECK((*d)["mouth_source"] == "frunk");         // the Frame's own mouth cameras unless USB is chosen
#endif
#if BABBLE_DEV_FEATURES
    CHECK((*d)["output"] == "vrcft");               // VRCFaceTracking unless VRChat Native is chosen
#endif

    // An id that isn't a bool/number/string setting is dropped rather than mirrored.
    json cfg = *d;
    cfg["steamvr_settings"]["values"].push_back({{"key", "bogus"}, {"id", "/sources/eye/ctrl_base_candidates"}});
    CHECK(SteamVrSettingsBridge::entries_from_config(cfg).size() == want.size());
}

TEST_CASE("values of the wrong type are reset to the default and reported") {
    json defaults = {{"sources", {{"mouth", {{"tracking", true}, {"process_fps", 120}}}}},
                     {"pipelines", {{"gaze", {{"ipd", "auto"}}}, {"face", {{"one_euro", {{"beta", 3.0}}}}}}},
                     {"sinks", {{"debug", {{"dump_dir", nullptr}}}}}};
    json cfg = defaults;
    cfg["sources"]["mouth"]["tracking"] = "yes";           // string for a boolean: reset
    cfg["sources"]["mouth"]["process_fps"] = 60.5;         // int vs float: both numbers, kept
    cfg["pipelines"]["gaze"]["ipd"] = 0.064;               // a flexible option: kept
    cfg["pipelines"]["face"]["one_euro"] = false;          // a flexible option: kept
    cfg["sinks"]["debug"]["dump_dir"] = "/tmp/x";          // a null default accepts anything
    cfg["extra"] = 1;                                      // keys the defaults don't have: untouched
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
    CHECK(section(cfg, "broken").is_object());   // not an object: an empty one instead
    CHECK_FALSE(cfg.contains("missing"));
}

TEST_CASE("a broken settings.json is moved aside, not lost, and overrides are listed") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("babble_cfg_bad_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path settings = dir / "settings.json";
    {
        std::ofstream(settings) << "{ \"sources\": { \"mouth\": ";   // truncated
    }
    ConfigLoad broken = load_config(dir / "none.json", settings);
    CHECK_FALSE(broken.settings_loaded);
    CHECK_FALSE(broken.settings_error.empty());

    std::string err;
    REQUIRE(save_setting(settings, json::json_pointer("/sources/eye/process_fps"), 60, &err));
    CHECK(fs::exists(dir / "settings.json.bad"));   // the broken file, kept for inspection
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
