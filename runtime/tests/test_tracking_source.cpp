#include <doctest/doctest.h>

#include "platform/steamvr_client.h"
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

TEST_CASE("presence: short 'not worn' blips are ignored, a real take-off counts after the delay") {
    const uint64_t s = 1'000'000'000ull;
    PresenceFilter f(2 * s);
    CHECK(f.update(Worn::Unknown, 0) == Worn::Unknown);
    CHECK(f.update(Worn::No, 1) == Worn::No);       // no "worn" yet to hold on to: off at once
    CHECK(f.update(Worn::Yes, 2) == Worn::Yes);     // on: at once
    // The sensor blips off for 0.5 s every few seconds while worn.
    for (uint64_t t = 10 * s; t < 30 * s; t += 3 * s) {
        for (uint64_t dt = 0; dt <= s / 2; dt += s / 10)
            CHECK(f.update(Worn::No, t + dt) == Worn::Yes);
        CHECK(f.update(Worn::Yes, t + s / 2 + 1) == Worn::Yes);
    }
    CHECK(f.blips() == 7);
    // Taken off: not worn once it has lasted 2 s.
    CHECK(f.update(Worn::No, 40 * s) == Worn::Yes);
    CHECK(f.update(Worn::No, 41 * s) == Worn::Yes);
    CHECK(f.update(Worn::No, 42 * s) == Worn::No);
    CHECK(f.update(Worn::No, 43 * s) == Worn::No);
    CHECK(f.blips() == 7);                          // a real removal is no blip
    CHECK(f.update(Worn::Yes, 44 * s) == Worn::Yes);
    // Unknown (SteamVR gone) passes through and resets.
    CHECK(f.update(Worn::Unknown, 45 * s) == Worn::Unknown);
    CHECK(f.update(Worn::No, 46 * s) == Worn::No);
}
