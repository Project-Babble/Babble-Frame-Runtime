#include <doctest/doctest.h>

#include "camera/tracking_source.h"
#include "platform/camera_arbiter_client.h"

using namespace babble;
using json = nlohmann::json;

TEST_CASE("the registry builds known source types and rejects unknown ones") {
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    CameraArbiterClient arbiter(json::object());

    // Disabled modules construct without touching hardware.
    auto mouth = make_tracking_source(json{{"type", "frame_ov6211_stereo"}, {"enabled", false},
                                           {"nodes", json::object()}, {"format", json::object()},
                                           {"strobe", json::object()}},
                                      SourceDeps{"mouth", threads, slot, stats, arbiter});
    REQUIRE(mouth);
    CHECK(std::string(mouth->state_name()) == "Disabled");
    CHECK_FALSE(mouth->streaming());

    auto eye = make_tracking_source(json{{"type", "frame_dsp_eye"}, {"enabled", false}},
                                    SourceDeps{"eye", threads, slot, stats, arbiter});
    REQUIRE(eye);
    CHECK(std::string(eye->state_name()) == "Disabled");

    CHECK_FALSE(make_tracking_source(json{{"type", "uvc_not_yet"}}, SourceDeps{"x", threads, slot, stats, arbiter}));
    CHECK_FALSE(make_tracking_source(json::object(), SourceDeps{"y", threads, slot, stats, arbiter}));
}

TEST_CASE("stats use the configured source name as prefix") {
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    CameraArbiterClient arbiter(json::object());
    {
        auto eye = make_tracking_source(json{{"type", "frame_dsp_eye"}, {"enabled", false}},
                                        SourceDeps{"eyes2", threads, slot, stats, arbiter});
        CHECK(stats.render().find("\"eyes2.fps\"") != std::string::npos);
    }
    CHECK(stats.render().find("eyes2.") == std::string::npos);   // unregistered on destruction
}
