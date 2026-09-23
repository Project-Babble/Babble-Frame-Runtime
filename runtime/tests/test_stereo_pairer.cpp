#include <doctest/doctest.h>

#include "frame/stereo_pairer.h"

using namespace babble;

namespace {
FramePtr make(FrameOrigin o, uint64_t t) {
    auto f = std::make_shared<Frame>();
    f->origin = o;
    f->capture_ns = t;
    return f;
}
constexpr uint64_t ms = 1000000;
}

TEST_CASE("pairs frames within tolerance, in either arrival order") {
    StereoPairer p(4 * ms, FrameOrigin::MouthL, FrameOrigin::MouthR);
    CHECK_FALSE(p.push(make(FrameOrigin::MouthL, 100 * ms)).has_value());
    auto a = p.push(make(FrameOrigin::MouthR, 101 * ms));
    REQUIRE(a.has_value());
    CHECK(a->left->origin == FrameOrigin::MouthL);
    CHECK(a->skew_ns == -int64_t(ms));

    CHECK_FALSE(p.push(make(FrameOrigin::MouthR, 108 * ms)).has_value());
    auto b = p.push(make(FrameOrigin::MouthL, 109 * ms));
    REQUIRE(b.has_value());
    CHECK(b->skew_ns == int64_t(ms));
    CHECK(p.counts().paired == 2);
}

TEST_CASE("drops a stale counterpart instead of emitting a bad pair") {
    StereoPairer p(4 * ms, FrameOrigin::MouthL, FrameOrigin::MouthR);
    p.push(make(FrameOrigin::MouthL, 100 * ms));            // right frame for this one never arrives
    CHECK_FALSE(p.push(make(FrameOrigin::MouthR, 120 * ms)).has_value());
    CHECK(p.counts().unmatched_left == 1);
    auto s = p.push(make(FrameOrigin::MouthL, 121 * ms));
    REQUIRE(s.has_value());
    CHECK(s->right->capture_ns == 120 * ms);
}

TEST_CASE("a newer frame from the same side replaces the waiting one") {
    StereoPairer p(4 * ms, FrameOrigin::MouthL, FrameOrigin::MouthR);
    p.push(make(FrameOrigin::MouthL, 100 * ms));
    p.push(make(FrameOrigin::MouthL, 108 * ms));
    CHECK(p.counts().unmatched_left == 1);
    auto s = p.push(make(FrameOrigin::MouthR, 109 * ms));
    REQUIRE(s.has_value());
    CHECK(s->left->capture_ns == 108 * ms);
}
