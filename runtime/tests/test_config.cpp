#include <doctest/doctest.h>

#include "core/config.h"
#include "defaults_json.h"

using namespace babble;

TEST_CASE("built-in defaults parse and carry the key settings") {
    std::string err;
    auto d = parse_json(kDefaultsJson, &err);
    REQUIRE_MESSAGE(d.has_value(), err);
    CHECK((*d)["sources"]["mouth"]["process_fps"] == 120);
    CHECK((*d)["sources"]["mouth"]["nodes"]["left"] == "/dev/video9");
    CHECK((*d)["sources"]["eye"]["ctrl_base_candidates"].size() == 2);
    CHECK((*d)["sources"]["eye"]["process_fps"] == 120);
    CHECK((*d)["sources"]["mouth"]["type"] == "frame_ov6211_stereo");
    CHECK((*d)["sources"]["eye"]["type"] == "frame_dsp_eye");
    // Every pipeline input names a configured source.
    for (auto &[name, pc] : (*d)["pipelines"].items())
        for (auto &in : pc["inputs"])
            CHECK_MESSAGE((*d)["sources"].contains(in["from"].get<std::string>()), name);
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
