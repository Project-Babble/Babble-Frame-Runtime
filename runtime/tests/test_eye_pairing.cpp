#include <doctest/doctest.h>

#include "camera/eye/eye_pairer.h"

using namespace babble;

namespace {
constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kWait = 5 * kMs;
using R = EyePairer::Result;
} // namespace

TEST_CASE("eye pairer: a pair needs both sides to advance") {
    EyePairer p;
    CHECK(p.feed(0, 0, 1 * kMs, kWait).result == R::None);   // what was in the ring at start
    auto s = p.feed(1, 0, 2 * kMs, kWait);                   // left first...
    CHECK(s.result == R::None);
    CHECK(s.new_l);
    CHECK_FALSE(s.new_r);
    CHECK(p.waiting());
    s = p.feed(1, 1, 3 * kMs, kWait);                        // ...then right: one pair, not two
    CHECK(s.result == R::Pair);
    CHECK_FALSE(s.new_l);
    CHECK(s.new_r);
    CHECK_FALSE(p.waiting());
    CHECK(p.feed(1, 1, 4 * kMs, kWait).result == R::None);
}

TEST_CASE("eye pairer: both sides in one poll make one pair") {
    EyePairer p;
    p.feed(5, 5, 0, kWait);
    int pairs = 0;
    uint64_t t = 0;
    for (uint32_t i = 6; i < 6 + 90; i++) {
        t += 11 * kMs;
        pairs += p.feed(i % 8, i % 8, t, kWait).result == R::Pair;
    }
    CHECK(pairs == 90);
}

TEST_CASE("eye pairer: staggered sides count each camera frame once") {
    // The old detector counted a change on either side, so this sequence made
    // 2 "pairs" per frame. 90 frames, right always 1 ms behind left.
    EyePairer p;
    p.feed(0, 0, 0, kWait);
    int pairs = 0, new_l = 0, new_r = 0;
    uint64_t t = 0;
    for (uint32_t i = 1; i <= 90; i++) {
        t += 10 * kMs;
        auto a = p.feed(i % 8, (i - 1) % 8, t, kWait);
        auto b = p.feed(i % 8, i % 8, t + kMs, kWait);
        pairs += (a.result == R::Pair) + (b.result == R::Pair);
        new_l += a.new_l + b.new_l;
        new_r += a.new_r + b.new_r;
    }
    CHECK(pairs == 90);
    CHECK(new_l == 90);
    CHECK(new_r == 90);
}

TEST_CASE("eye pairer: a side that never follows is given up after the wait") {
    EyePairer p;
    p.feed(0, 0, 0, kWait);
    CHECK(p.feed(1, 0, 10 * kMs, kWait).result == R::None);
    CHECK(p.feed(1, 0, 14 * kMs, kWait).result == R::None);   // within the wait
    auto s = p.feed(1, 0, 16 * kMs, kWait);
    CHECK(s.result == R::Half);
    CHECK(s.half_l);
    CHECK_FALSE(s.half_r);
    CHECK_FALSE(p.waiting());
    // The late right frame alone doesn't make a pair with the given-up left.
    CHECK(p.feed(1, 1, 17 * kMs, kWait).result == R::None);
    CHECK(p.feed(2, 1, 20 * kMs, kWait).result == R::Pair);
}

TEST_CASE("eye pairer: not-ready rings and reset") {
    EyePairer p;
    CHECK(p.feed(EyePairer::kNotReady, 3, 0, kWait).result == R::None);
    CHECK(p.feed(3, 3, kMs, kWait).result == R::None);   // first sane read only primes
    CHECK(p.feed(4, 4, 2 * kMs, kWait).result == R::Pair);
    p.reset();
    CHECK(p.feed(5, 5, 3 * kMs, kWait).result == R::None);
    CHECK(p.feed(6, 6, 4 * kMs, kWait).result == R::Pair);
}

TEST_CASE("eye pairer: a pair carries when each side was first seen") {
    EyePairer p;
    p.feed(0, 0, 0, kWait);
    auto s = p.feed(1, 0, 10 * kMs, kWait);   // left seen at 10 ms
    CHECK(s.result == R::None);
    s = p.feed(1, 0, 11 * kMs, kWait);        // still waiting: left's time stays 10
    s = p.feed(1, 1, 12 * kMs, kWait);        // right at 12
    REQUIRE(s.result == R::Pair);
    CHECK(s.l_ns == 10 * kMs);
    CHECK(s.r_ns == 12 * kMs);

    s = p.feed(2, 2, 20 * kMs, kWait);        // both in one poll: no measurable skew
    REQUIRE(s.result == R::Pair);
    CHECK(s.l_ns == 20 * kMs);
    CHECK(s.r_ns == 20 * kMs);

    s = p.feed(2, 3, 30 * kMs, kWait);        // right leads this time
    s = p.feed(3, 3, 31 * kMs, kWait);
    REQUIRE(s.result == R::Pair);
    CHECK(s.l_ns == 31 * kMs);
    CHECK(s.r_ns == 30 * kMs);

    p.feed(4, 3, 40 * kMs, kWait);            // a half pair doesn't leak its time into the next pair
    CHECK(p.feed(4, 3, 50 * kMs, kWait).result == R::Half);
    p.feed(5, 3, 60 * kMs, kWait);
    s = p.feed(5, 4, 61 * kMs, kWait);
    REQUIRE(s.result == R::Pair);
    CHECK(s.l_ns == 60 * kMs);
    CHECK(s.r_ns == 61 * kMs);
}
