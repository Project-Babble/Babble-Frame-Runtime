#include <doctest/doctest.h>

#include "model/steamvr_gaze.h"

#include <cmath>

using namespace babble;

TEST_CASE("gaze conversion matches eye_osc_pc.py SteamLinkGaze.convert") {
    struct Case {
        double x, y, z, roll, yaw, pitch;
        bool flip_y, valid;
        double lx, rx, py;
    };
    // Expected values produced by the reference class itself (ipd 0.063, 45 deg).
    const Case cases[] = {
        {0.0, 0.0, -2.0, 0, 0, 0, false, true, 0.02005186490066484, -0.02005186490066484, 0.0},
        {0.5, 0.3, -1.0, 0, 0, 0, false, true, 0.6220143195789958, 0.5578464486092127, 0.3337834814780848},
        {-0.8, -0.4, -0.6, 0, 0, 0, false, true, -1.0, -1.0, -0.48447576636337364},
        {0.1, 0.0, -0.3, 0, 0, 0, false, true, 0.5259875822390145, 0.2858230303810862, 0.0},
        {3.0, -1.0, 2.0, 0, 0, 0, false, true, 1.0, 1.0, -0.34447465704304436},
        {0.4, 0.2, -1.5, 5, -3, 10, true, true, 0.2723296429298719, 0.22204079170398106, 0.026872905797580386},
        {0.001, 0.0, 0.002, 0, 0, 0, false, false, 0, 0, 0},
    };
    for (const auto &c : cases) {
        CAPTURE(c.x);
        CAPTURE(c.y);
        CAPTURE(c.z);
        auto g = gaze::convert(c.x, c.y, c.z, 0.063, 45.0, c.flip_y, {c.roll, c.yaw, c.pitch});
        REQUIRE(bool(g) == c.valid);
        if (!g)
            continue;
        CHECK((*g)[0] == doctest::Approx(c.lx).epsilon(1e-12));
        CHECK((*g)[1] == doctest::Approx(c.rx).epsilon(1e-12));
        CHECK((*g)[2] == doctest::Approx(c.py).epsilon(1e-12));
    }
}
