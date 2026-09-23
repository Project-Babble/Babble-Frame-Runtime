#include <doctest/doctest.h>

#include "frame/rate_limiter.h"

#include <initializer_list>

using namespace babble;

namespace {
int accepted(RateLimiter &l, double in_fps, double seconds) {
    int n = 0;
    int frames = int(in_fps * seconds);
    for (int i = 0; i < frames; i++)
        n += l.accept(1 + uint64_t(double(i) * 1e9 / in_fps));
    return n;
}
}

TEST_CASE("full rate passes everything") {
    RateLimiter l(120);
    CHECK(accepted(l, 120, 1.0) == 120);
}

TEST_CASE("subsamples 120 fps input to the requested rate") {
    for (double target : {60.0, 45.0, 30.0}) {
        RateLimiter l(target);
        int n = accepted(l, 120, 2.0);
        CHECK(n >= int(target * 2) - 2);
        CHECK(n <= int(target * 2) + 2);
    }
}

TEST_CASE("zero means unlimited, and the rate can change live") {
    RateLimiter l(0);
    CHECK(accepted(l, 120, 1.0) == 120);
    l.set_fps(30);
    l.reset();
    int n = accepted(l, 120, 1.0);
    CHECK(n >= 29);
    CHECK(n <= 31);
}
