#include <doctest/doctest.h>

#include "model/steamvr_gaze.h"

#include <cmath>

using namespace babble;

TEST_CASE("angle scaling matches eye_osc_pc.py SteamLinkGaze.convert") {
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
        auto g = gaze::convert(c.x, c.y, c.z, 0.063, 45.0, c.flip_y, {c.roll, c.yaw, c.pitch}, gaze::Scaling::Angle);
        REQUIRE(bool(g) == c.valid);
        if (!g)
            continue;
        CHECK((*g)[0] == doctest::Approx(c.lx).epsilon(1e-12));
        CHECK((*g)[1] == doctest::Approx(c.rx).epsilon(1e-12));
        CHECK((*g)[2] == doctest::Approx(c.py).epsilon(1e-12));
    }
}

TEST_CASE("tangent scaling gives VRCFT's Gaze (tan of the angle; atan() recovers it)") {
    const double h = 0.063 / 2;
    // Unclamped points: each value is the tangent of that eye's angle to the point.
    struct P { double x, y, z; } pts[] = {{0.0, 0.0, -2.0}, {0.5, 0.3, -1.0}, {-0.3, -0.2, -0.8}, {0.1, 0.0, -0.3}};
    for (const auto &p : pts) {
        auto g = gaze::convert(p.x, p.y, p.z, 0.063, 45.0, false, {});
        REQUIRE(g);
        const double z = std::fabs(p.z);
        CHECK((*g)[0] == doctest::Approx((p.x + h) / z).epsilon(1e-12));
        CHECK((*g)[1] == doctest::Approx((p.x - h) / z).epsilon(1e-12));
        CHECK((*g)[2] == doctest::Approx(p.y / std::hypot(p.x, z)).epsilon(1e-12));
    }
    // Looking right and up are positive; VRCFT's ToYaw = atan(x) recovers 30 degrees exactly.
    const double t30 = std::tan(30.0 * M_PI / 180);
    auto g = gaze::convert(t30 * 10, 0, -10, 0.0, 45.0, false, {});
    REQUIRE(g);
    CHECK(std::atan((*g)[0]) * 180 / M_PI == doctest::Approx(30.0).epsilon(1e-9));
    auto up = gaze::convert(0, 1, -2, 0.063, 45.0, false, {});
    REQUIRE(up);
    CHECK((*up)[2] > 0);
    // Beyond max_deg the angle is limited: tan(45 deg) = 1.
    auto far = gaze::convert(10, 0, -1, 0.0, 45.0, false, {});
    REQUIRE(far);
    CHECK((*far)[0] == doctest::Approx(1.0));
}
