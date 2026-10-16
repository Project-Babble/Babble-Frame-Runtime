#include <doctest/doctest.h>

#include "platform/steamvr_client.h"
#include "camera/tracking_source.h"
#if BABBLE_FRUNK_MOUTH
#include "platform/camera_arbiter_client.h"
#endif

using namespace babble;
using json = nlohmann::json;

TEST_CASE("the registry builds known source types and rejects unknown ones") {
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;

    // Disabled modules construct and do not touch the hardware.
    const json frunk{{"type", "frame_ov6211_stereo"}, {"enabled", false}, {"nodes", json::object()},
                     {"format", json::object()}, {"strobe", json::object()}};
#if BABBLE_FRUNK_MOUTH
    CameraArbiterClient arbiter(json::object());
    auto mouth = make_tracking_source(frunk, SourceDeps{"mouth", threads, slot, stats, &arbiter});
    REQUIRE(mouth);
    CHECK(std::string(mouth->state_name()) == "Disabled");
    CHECK_FALSE(mouth->streaming());
#endif
    // The Frame mouth cameras cannot work without the camera arbiter.
    CHECK_FALSE(make_tracking_source(frunk, SourceDeps{"mouth", threads, slot, stats, nullptr}));

    auto eye = make_tracking_source(json{{"type", "frame_dsp_eye"}, {"enabled", false}},
                                    SourceDeps{"eye", threads, slot, stats, nullptr});
    REQUIRE(eye);
    CHECK(std::string(eye->state_name()) == "Disabled");

    CHECK_FALSE(make_tracking_source(json{{"type", "uvc_not_yet"}}, SourceDeps{"x", threads, slot, stats, nullptr}));
    CHECK_FALSE(make_tracking_source(json::object(), SourceDeps{"y", threads, slot, stats, nullptr}));
}

TEST_CASE("stats use the configured source name as prefix") {
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    {
        auto eye = make_tracking_source(json{{"type", "frame_dsp_eye"}, {"enabled", false}},
                                        SourceDeps{"eyes2", threads, slot, stats, nullptr});
        CHECK(stats.render().find("\"eyes2.fps\"") != std::string::npos);
    }
    CHECK(stats.render().find("eyes2.") == std::string::npos);   // The source removes its stats when it is destroyed.
}

TEST_CASE("presence: short 'not worn' blips are ignored, a real take-off counts after the delay") {
    const uint64_t s = 1'000'000'000ull;
    PresenceFilter f(2 * s);
    CHECK(f.update(Worn::Unknown, 0) == Worn::Unknown);
    CHECK(f.update(Worn::No, 1) == Worn::No);       // There is no "worn" state to keep yet, so off applies immediately.
    CHECK(f.update(Worn::Yes, 2) == Worn::Yes);     // On applies immediately.
    // The sensor goes off for 0.5 s every few seconds while the headset is worn.
    for (uint64_t t = 10 * s; t < 30 * s; t += 3 * s) {
        for (uint64_t dt = 0; dt <= s / 2; dt += s / 10)
            CHECK(f.update(Worn::No, t + dt) == Worn::Yes);
        CHECK(f.update(Worn::Yes, t + s / 2 + 1) == Worn::Yes);
    }
    CHECK(f.blips() == 7);
    // After the user takes the headset off, the state becomes not worn when the off state lasts 2 s.
    CHECK(f.update(Worn::No, 40 * s) == Worn::Yes);
    CHECK(f.update(Worn::No, 41 * s) == Worn::Yes);
    CHECK(f.update(Worn::No, 42 * s) == Worn::No);
    CHECK(f.update(Worn::No, 43 * s) == Worn::No);
    CHECK(f.blips() == 7);                          // A real removal is not a blip.
    CHECK(f.update(Worn::Yes, 44 * s) == Worn::Yes);
    // Unknown (SteamVR gone) passes through and resets the filter.
    CHECK(f.update(Worn::Unknown, 45 * s) == Worn::Unknown);
    CHECK(f.update(Worn::No, 46 * s) == Worn::No);
}
