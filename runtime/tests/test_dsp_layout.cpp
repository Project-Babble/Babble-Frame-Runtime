#include <doctest/doctest.h>

#include "camera/eye/dsp_client.h"

#include <cstring>
#include <vector>

using namespace babble;

namespace {
void put32(std::vector<uint8_t> &m, uint32_t off, uint32_t v) { memcpy(&m[off], &v, 4); }

std::vector<uint8_t> good_block(const DspClient::Layout &l) {
    std::vector<uint8_t> m(DspClient::kRegionBytes, 0);
    const uint32_t b = l.ctrl_base;
    put32(m, b + l.idx_l, 3);
    put32(m, b + l.idx_r, DspClient::kNotReady);
    // Observed idle contents on the headset: (4,4,1,8,0,0,0,0) on both sides.
    const uint32_t idle[8] = {4, 4, 1, 8, 0, 0, 0, 0};
    for (uint32_t i = 0; i < l.slots; i++) {
        put32(m, b + l.locks_l + 4 * i, idle[i]);
        put32(m, b + l.locks_r + 4 * i, idle[i]);
    }
    put32(m, b + l.enables, 1);
    put32(m, b + l.enables + 4, 1);
    float fps = 15.0f;
    memcpy(&m[b + l.fps], &fps, 4);
    return m;
}
}

TEST_CASE("accepts a plausible control block at both known bases") {
    for (uint32_t base : {0x234000u, 0x124000u}) {
        DspClient::Layout l;
        l.ctrl_base = base;
        auto m = good_block(l);
        std::string why;
        CHECK_MESSAGE(DspClient::validate(m.data(), m.size(), l, &why), why);
    }
}

TEST_CASE("rejects garbage words") {
    DspClient::Layout l;
    l.ctrl_base = 0x234000;
    std::string why;

    auto m = good_block(l);
    put32(m, l.ctrl_base + l.idx_l, 12);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.locks_r + 8, 0xdeadbeef);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.enables + 4, 5);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.fps, 0x7fc00000);   // NaN
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));
}

TEST_CASE("the wrong base is rejected when only the other one is valid") {
    DspClient::Layout real;
    real.ctrl_base = 0x234000;
    auto m = good_block(real);
    // Fill the old location with typical non-control data.
    for (uint32_t off = 0x124000; off < 0x124000 + 0x200; off += 4)
        put32(m, off, 0x41424344);
    DspClient::Layout old;
    old.ctrl_base = 0x124000;
    std::string why;
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), old, &why));
    CHECK(DspClient::validate(m.data(), m.size(), real, &why));
}

TEST_CASE("rejects layouts that would run past the mapping") {
    DspClient::Layout l;
    l.ctrl_base = 0xF00000;
    std::vector<uint8_t> m(DspClient::kRegionBytes, 0);
    std::string why;
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));
}

TEST_CASE("a blank control block (service just booted) is 'not set up yet', not an unknown layout") {
    const uint32_t bases[] = {0x234000u, 0x124000u};
    std::vector<uint8_t> blank(DspClient::kRegionBytes, 0);
    CHECK(DspClient::fps_unset(blank.data(), blank.size(), bases));

    // Streaming block: the fps word is set, so a failed probe would be a real layout problem.
    DspClient::Layout l;
    l.ctrl_base = 0x234000;
    auto m = good_block(l);
    CHECK_FALSE(DspClient::fps_unset(m.data(), m.size(), bases));

    // Garbage where the fps word should be: unknown layout, not "not ready".
    std::vector<uint8_t> junk(DspClient::kRegionBytes, 0);
    for (uint32_t base : bases)
        put32(junk, base + DspClient::Layout{}.fps, 0x41424344);
    CHECK_FALSE(DspClient::fps_unset(junk.data(), junk.size(), bases));
}
