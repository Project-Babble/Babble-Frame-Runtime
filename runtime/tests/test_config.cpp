#include <doctest/doctest.h>

#include "core/config.h"
#include "defaults_json.h"
#include "platform/diagnostics.h"
#include "platform/steamvr_settings.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

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
    CHECK_FALSE(d->contains("visualizer"));
    CHECK((*d)["sinks"]["vrcft"]["enabled"] == false);
#endif
    // Every profile ships all the output modules, and only VRCFaceTracking is on by default.
    CHECK_FALSE(d->contains("output"));
    CHECK((*d)["outputs"] == json{{"vrcft", true}, {"vrc_native", false}, {"babble_osc", false}});
    CHECK((*d)["pc_address"] == "");
    std::set<std::string> roles;
    for (auto &[name, sc] : (*d)["sinks"].items()) {
        if (!sc.contains("output"))
            continue;
        roles.insert(sc["output"].get<std::string>());
        // Each module names itself, and each of its settings is a key of the module with a value of the right kind.
        REQUIRE_MESSAGE(sc.contains("module"), name);
        CHECK_MESSAGE(!sc["module"].value("label", "").empty(), name);
        for (const auto &o : sc["module"].value("options", json::array())) {
            const std::string key = o.value("key", "");
            CAPTURE(name);
            CAPTURE(key);
            REQUIRE(sc.contains(key));
            if (o.value("kind", "") == "choice") {
                bool found = false;
                for (const auto &v : o["values"])
                    found |= v == sc[key];
                CHECK(found);
                CHECK(o["labels"].size() == o["values"].size());
            } else {
                CHECK(o.value("kind", "") == "text");
                CHECK((o.value("numeric", false) ? sc[key].is_number_integer() : sc[key].is_string()));
            }
        }
    }
    CHECK(roles == std::set<std::string>{"vrcft", "vrc_native", "babble_osc"});
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
    CHECK((*d)["pipelines"]["gaze"]["smoothing"] == 0);   // Gaze smoothing is off unless the user turns it on.
    CHECK((*d)["pipelines"]["eye"]["combine_widen"] == true);   // Both eyes send the mean Widen and dilation.
    CHECK((*d)["pipelines"]["eye"]["combine_dilation"] == true);
    CHECK((*d)["sources"]["face"]["tracker_flip_h"] == true);   // The Babble tracker image is flipped by default.
    CHECK_FALSE((*d)["pipelines"]["face"].contains("mirror"));
    CHECK((*d)["dashboard"]["enabled"] == true);    // The Babble tab is on after a new install.
#if BABBLE_FRUNK_MOUTH
    CHECK((*d)["mouth_source"] == "frunk");         // The default is the Frame cameras unless the user selects USB.
#endif

    // The loader drops an id that is not a bool, number or string setting and does not mirror it.
    json cfg = *d;
    cfg["steamvr_settings"]["values"].push_back({{"key", "bogus"}, {"id", "/sources/eye/ctrl_base_candidates"}});
    CHECK(SteamVrSettingsBridge::entries_from_config(cfg).size() == want.size());
}

TEST_CASE("the old Mirror Image switch becomes the default flip of the Babble tracker") {
    const json old_settings = {{"pipelines", {{"face", {{"mirror", false}}}}}};
    json cfg = {{"pipelines", {{"face", {{"mirror", false}, {"rotate", 0}}}}}, {"sources", {{"face", {{"tracker_flip_h", true}}}}}};
    CHECK(migrate_face_mirror(cfg, {&old_settings}));
    CHECK(cfg["sources"]["face"]["tracker_flip_h"] == false);
    CHECK_FALSE(cfg["pipelines"]["face"].contains("mirror"));
    CHECK(cfg["pipelines"]["face"]["rotate"] == 0);

    // A layer that already sets the new key wins, and without the old key nothing changes.
    const json both = {{"pipelines", {{"face", {{"mirror", false}}}}}, {"sources", {{"face", {{"tracker_flip_h", true}}}}}};
    cfg["sources"]["face"]["tracker_flip_h"] = true;
    CHECK_FALSE(migrate_face_mirror(cfg, {&both}));
    CHECK(cfg["sources"]["face"]["tracker_flip_h"] == true);
    const json none = json::object();
    CHECK_FALSE(migrate_face_mirror(cfg, {&none}));
}

TEST_CASE("the old single output choice becomes the output switches") {
    const json defaults = {{"outputs", {{"vrcft", true}, {"vrc_native", false}, {"babble_osc", false}}}};
    json cfg = defaults;
    cfg["output"] = "vrc_native";
    const json old_settings = {{"output", "vrc_native"}};
    CHECK(migrate_output_choice(cfg, {&old_settings}) == "vrc_native");
    CHECK_FALSE(cfg.contains("output"));
    CHECK(cfg["outputs"] == json{{"vrcft", false}, {"vrc_native", true}, {"babble_osc", false}});

    // A switch that a layer sets stays, and the newest layer with a choice wins.
    cfg = defaults;
    cfg["outputs"]["babble_osc"] = true;
    const json user = {{"output", "vrcft"}};
    const json settings = {{"output", "vrc_native"}, {"outputs", {{"babble_osc", true}}}};
    CHECK(migrate_output_choice(cfg, {&user, &settings}) == "vrc_native");
    CHECK(cfg["outputs"] == json{{"vrcft", false}, {"vrc_native", true}, {"babble_osc", true}});

    // Without an old choice nothing changes.
    cfg = defaults;
    const json none = json::object();
    CHECK(migrate_output_choice(cfg, {&none}).empty());
    CHECK(cfg == defaults);
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
