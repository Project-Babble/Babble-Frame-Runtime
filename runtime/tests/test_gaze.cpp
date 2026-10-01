#include <doctest/doctest.h>

#include "model/steamvr_gaze.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace babble;

TEST_CASE("angle scaling: eye_osc_pc.py's per-eye yaw, gaze_angles() pitch") {
    struct Case {
        double x, y, z, roll, yaw, pitch;
        bool flip_y, valid;
        double lx, rx, py;
    };
    // Yaw: the reference SteamLinkGaze.convert's own values (ipd 0.063, 45
    // deg). Pitch: atan2(y, -z) / 45 deg, as frameeyeosc's gaze_angles().
    const Case cases[] = {
        {0.0, 0.0, -2.0, 0, 0, 0, false, true, 0.02005186490066484, -0.02005186490066484, 0.0},
        {0.5, 0.3, -1.0, 0, 0, 0, false, true, 0.6220143195789958, 0.5578464486092127, 0.3710943163109694},
        {-0.8, -0.4, -0.6, 0, 0, 0, false, true, -1.0, -1.0, -0.7486681672439954},
        {0.1, 0.0, -0.3, 0, 0, 0, false, true, 0.5259875822390145, 0.2858230303810862, 0.0},
        {3.0, -1.0, 2.0, 0, 0, 0, false, true, 1.0, 1.0, -0.590334470601733},
        {0.4, 0.2, -1.5, 5, -3, 10, true, true, 0.2723296429298719, 0.22204079170398106, 0.02738765650615556},
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
    const auto tangent = gaze::Scaling::Tangent;
    for (const auto &p : pts) {
        auto g = gaze::convert(p.x, p.y, p.z, 0.063, 45.0, false, {}, tangent);
        REQUIRE(g);
        const double z = std::fabs(p.z);
        CHECK((*g)[0] == doctest::Approx((p.x + h) / z).epsilon(1e-12));
        CHECK((*g)[1] == doctest::Approx((p.x - h) / z).epsilon(1e-12));
        CHECK((*g)[2] == doctest::Approx(p.y / z).epsilon(1e-12));
    }
    // Looking right and up are positive; VRCFT's ToYaw = atan(x) recovers 30 degrees exactly.
    const double t30 = std::tan(30.0 * M_PI / 180);
    auto g = gaze::convert(t30 * 10, 0, -10, 0.0, 45.0, false, {}, tangent);
    REQUIRE(g);
    CHECK(std::atan((*g)[0]) * 180 / M_PI == doctest::Approx(30.0).epsilon(1e-9));
    auto up = gaze::convert(0, 1, -2, 0.063, 45.0, false, {}, tangent);
    REQUIRE(up);
    CHECK((*up)[2] > 0);
    // Beyond max_deg the angle is limited: tan(45 deg) = 1.
    auto far = gaze::convert(10, 0, -1, 0.0, 45.0, false, {}, tangent);
    REQUIRE(far);
    CHECK((*far)[0] == doctest::Approx(1.0));
}

TEST_CASE("per-eye gaze directions convert like frameeyeosc's gaze_angles()") {
    const gaze::Offsets none;
    // Straight ahead (-z forward) is centre.
    auto c = gaze::from_direction(0, 0, -1, 45, false, none);
    REQUIRE(c);
    CHECK((*c)[0] == doctest::Approx(0));
    CHECK((*c)[1] == doctest::Approx(0));
    // gaze_angles(): atan2(x, -z) and atan2(y, -z), times 4/pi (45 deg -> 1).
    auto gaze_angles = [](double x, double y, double z) {
        const double k = 4 / M_PI;
        return std::array<double, 2>{std::clamp(std::atan2(x, -z) * k, -1.0, 1.0),
                                     std::clamp(std::atan2(y, -z) * k, -1.0, 1.0)};
    };
    for (const auto &v : {std::array<double, 3>{0.5, 0.17, -0.85}, {-0.2, -0.3, -0.9}, {0.9, 0.6, -0.3},
                          {-0.05, 0.4, -0.7}}) {
        CAPTURE(v[0]);
        auto d = gaze::from_direction(v[0], v[1], v[2], 45, false, none);
        REQUIRE(d);
        const auto want = gaze_angles(v[0], v[1], v[2]);
        CHECK((*d)[0] == doctest::Approx(want[0]).epsilon(1e-12));
        CHECK((*d)[1] == doctest::Approx(want[1]).epsilon(1e-12));
    }
    // Pitch is the vertical angle on its own: 10 deg up stays 10 deg however far right the eye looks.
    const double up10 = std::tan(10 * M_PI / 180);
    auto side = gaze::from_direction(0.8, up10, -1, 45, false, none);
    CHECK((*side)[1] == doctest::Approx(10.0 / 45));
    // The tangent scaling gives tan of those angles: x/z, y/z.
    auto t = gaze::from_direction(0.5, 0.2, -1, 45, false, none, gaze::Scaling::Tangent);
    CHECK((*t)[0] == doctest::Approx(0.5));
    CHECK((*t)[1] == doctest::Approx(0.2));
    // Past the limit: clamped to +-45 deg, which is +-1 either way.
    auto far = gaze::from_direction(-1, 0, -0.1, 45, false, none);
    REQUIRE(far);
    CHECK((*far)[0] == doctest::Approx(-1.0));
    auto far_tan = gaze::from_direction(-1, 0, -0.1, 45, false, none, gaze::Scaling::Tangent);
    CHECK((*far_tan)[0] == doctest::Approx(-1.0));
    // flip_y and a yaw offset apply like in convert().
    auto f = gaze::from_direction(0, 0.2, -1, 45, true, none);
    CHECK((*f)[1] < 0);
    gaze::Offsets yaw10;
    yaw10.yaw_deg = 10;
    auto o = gaze::from_direction(0, 0, -1, 45, false, yaw10);
    CHECK((*o)[0] == doctest::Approx(10.0 / 45));
    // Degenerate direction.
    CHECK_FALSE(gaze::from_direction(0, 0, 0, 45, false, none));
}
