#include <doctest/doctest.h>

#include "camera/eye/scan/dma_arena.h"
#include "camera/eye/scan/eye_ring.h"
#include "camera/eye/scan/slot_watcher.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace babble::eyescan;

namespace {

// These tests are ported from eyecam's ring.rs tests. eyecam calls the lower-address group eye 1 (R); here it is
// group 0.
constexpr size_t kOrigin = 0x512345;   // The header of slot 0, not aligned to a row on purpose.
constexpr size_t kGroupGap = 64;       // As on the device, the second group starts 64 bytes late.

size_t header_at(size_t slot) { return kOrigin + slot * kExpectedPitch + (slot >= 4 ? kGroupGap : 0); }

// A simulated camera: writes a new frame into the next slot of each group at `fps`, plus heap churn.
class Sim : public ScanClock {
public:
    Sim(std::vector<uint8_t> &buf, double fps, uint8_t pad, bool mirrored, size_t dark)
        : buf_(buf), fps_(fps), pad_(pad), mirrored_(mirrored), dark_(dark) {}

    double now() override { return t_; }

    void wait() override {
        t_ += 0.004;
        for (int g = 0; g < 2; g++) {
            while (next_[g] <= t_) {
                next_[g] += 1.0 / fps_;
                write_frame(g);
            }
        }
        if (uint64_t(t_ * 1000.0) % 20 < 4)
            churn();
    }

    void write_frame(int g) {
        const size_t slot = size_t(g) * 4 + count_[g] % 4;
        count_[g]++;
        const size_t hdr = header_at(slot);
        const size_t frame = hdr + kHeaderBytes;
        const uint64_t ns = uint64_t(t_ * 1e9);
        memcpy(&buf_[hdr], &ns, 8);
        const double cx = (g == 0) != mirrored_ ? 150.0 : 250.0;
        for (size_t y = 0; y < kHeight; y++) {
            for (size_t x = 0; x < kWidth; x++) {
                const uint32_t noise = rand() % 8;
                const double r2 = ((double(x) - cx) * (double(x) - cx) + (double(y) - 200.0) * (double(y) - 200.0)) / 1600.0;
                uint32_t v;
                if (y < 4 || slot < dark_)
                    v = 2 + noise % 3;
                else if (r2 < 1.0)
                    v = 12 + noise;
                else
                    v = 40 + uint32_t(60.0 * std::exp(-r2 / 30.0)) + noise;
                buf_[frame + y * kStride + x] = uint8_t(v);
            }
            for (size_t x = kWidth; x < kStride; x++)
                buf_[frame + y * kStride + x] = pad_;
        }
    }

    void fill_all() {
        for (int i = 0; i < 8; i++)
            for (int g = 0; g < 2; g++)
                write_frame(g);
    }

private:
    uint32_t rand() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return uint32_t(rng_ >> 32);
    }

    void churn() {
        // A mostly black block with a thin noisy band, written again and again.
        for (size_t i = 0; i < kFrameBytes; i++)
            buf_[(12u << 20) + i] = (i / kStride) % 100 < 8 ? uint8_t(rand() % 255) : 0;
    }

    std::vector<uint8_t> &buf_;
    double fps_;
    double t_ = 0;
    double next_[2] = {0.0, 0.005};
    size_t count_[2] = {0, 0};
    uint64_t rng_ = 0x9e3779b97f4a7c15ull;
    uint8_t pad_;
    bool mirrored_;
    size_t dark_;
};

struct SimLook {
    bool found = false;
    Ring ring;
    Look look;
    std::vector<std::string> lines;
};

SimLook look_at_sim(uint8_t pad, bool mirrored, double fps, size_t dark) {
    std::vector<uint8_t> buf(16u << 20, 0);
    Sim sim(buf, fps, pad, mirrored, dark);
    // An old but eye-like picture in the heap, which the search must not pick.
    for (size_t y = 0; y < kHeight; y++)
        for (size_t x = 0; x < kWidth; x++)
            buf[0x100000 + y * kStride + x] = uint8_t(50 + (x + y) % 7);
    sim.fill_all();
    DmaArena arena(buf.data(), buf.size());
    for (size_t i = 0; i < size_t(50.0 * 90.0 / fps); i++)
        sim.wait();
    arena.note_block_changes();
    SimLook r;
    r.found = discover(arena, sim, [&](const std::string &l) { r.lines.push_back(l); }, &r.look, &r.ring);
    return r;
}

void find_ring(uint8_t pad, bool mirrored, double fps) {
    const SimLook r = look_at_sim(pad, mirrored, fps, 0);
    REQUIRE(r.found);
    CHECK(r.ring.pitch == kExpectedPitch);
    std::vector<size_t> expected;
    for (size_t k = 0; k < 8; k++)
        expected.push_back(header_at(k) + kHeaderBytes);
    CHECK(r.ring.off == expected);
    CHECK(r.ring.group == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(r.ring.both_groups);
    CHECK(r.look.candidates >= 8);
    CHECK(r.look.refresh_hz >= (fps < 20 ? 3.0 : 5.0));
    CHECK(r.look.slots == 8);
    CHECK(std::string(r.look.stopped_at).empty());
    CHECK(r.ring.reason.rfind("layout: group boundary before slot 4 (spacing 262208+64)", 0) == 0);
    CHECK(r.ring.reason.find("WARNING") == std::string::npos);
    const double clean = pad == 0 ? 0.001 : 0.51;
    for (double d : r.ring.pad_dirty)
        CHECK(d < clean);
}

}

TEST_CASE("eye scan: finds the ring and the exact frame starts") { find_ring(0, false, 90.0); }

TEST_CASE("eye scan: finds the exact frame starts with old data in the padding") { find_ring(0x0c, false, 90.0); }

TEST_CASE("eye scan: the groups follow the layout, not the pictures") { find_ring(0, true, 90.0); }

TEST_CASE("eye scan: finds the ring of cameras at 15 fps") { find_ring(0, false, 15.0); }

TEST_CASE("eye scan: a search of memory that did not change finds no candidates") {
    std::vector<uint8_t> buf(4u << 20, 0);
    DmaArena arena(buf.data(), buf.size());
    arena.note_block_changes();
    struct Still : ScanClock {
        double t = 0;
        double now() override { return t; }
        void wait() override { t += 0.01; }
    } clock;
    Look look;
    Ring ring;
    CHECK_FALSE(discover(arena, clock, [](const std::string &) {}, &look, &ring));
    CHECK(look.candidates == 0);
    CHECK(std::string(look.stopped_at) == "no_candidates");
}

TEST_CASE("eye scan: a ring without all slots of a camera is not taken") {
    // With the headset off, one camera showed a picture in one slot only, and the walk stopped there: 5 slots.
    const SimLook r = look_at_sim(0, false, 90.0, 3);
    CHECK_FALSE(r.found);
    CHECK(r.look.slots == 5);
    CHECK(std::string(r.look.stopped_at) == "few_slots");
    auto has = [&](const std::string &prefix) {
        return std::any_of(r.lines.begin(), r.lines.end(), [&](const std::string &l) { return l.rfind(prefix, 0) == 0; });
    };
    CHECK(has("groups [0,1,1,1,1]: layout: group boundary before slot 1"));
    CHECK(has("not taken: each camera needs 4 slots"));
}

TEST_CASE("eye scan: groups from the layout") {
    const size_t p = kExpectedPitch;
    std::vector<size_t> off;
    for (size_t k = 0; k < 8; k++)
        off.push_back(0x234100 + k * p + (k >= 4 ? 64 : 0));
    std::string why;
    CHECK(assign_groups(off, p, &why) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(why.find("before slot 4") != std::string::npos);
    // A boundary in another position (a slot that did not refresh was dropped) still splits there.
    CHECK(assign_groups({off[1], off[2], off[3], off[4], off[5]}, p, nullptr) == std::vector<uint8_t>{0, 0, 0, 1, 1});
    // No boundary: 4 + 4 by address for a full ring, else nothing.
    std::vector<size_t> even;
    for (size_t k = 0; k < 8; k++)
        even.push_back(0x100000 + k * p);
    CHECK(assign_groups(even, p, nullptr) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(assign_groups({even.begin(), even.begin() + 4}, p, nullptr).empty());
    CHECK(assign_groups({off[0], off[1], off[4], off[5] + 128}, p, nullptr).empty());
}

TEST_CASE("eye scan: a spacing measured one byte off still finds the boundary") {
    const std::vector<size_t> off = {0x234100, 0x274140, 0x2b4180, 0x2f41c0, 0x334240, 0x374280, 0x3b42c0, 0x3f4300};
    std::string why;
    CHECK(assign_groups(off, kExpectedPitch + 1, &why) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(why.find("spacing 262208+64") != std::string::npos);
    CHECK(assign_groups(off, kExpectedPitch - 1, nullptr) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(assign_groups({off[2], off[3], off[4]}, kExpectedPitch + 1, nullptr) == std::vector<uint8_t>{0, 0, 1});
}

TEST_CASE("eye scan: a full ring is always four and four") {
    const size_t p = kExpectedPitch;
    std::vector<size_t> jumbled, late;
    for (size_t k = 0; k < 8; k++) {
        jumbled.push_back(0x100000 + k * p + k * k);
        late.push_back(0x100000 + k * p + (k >= 5 ? 64 : 0));
    }
    CHECK(assign_groups(jumbled, p, nullptr).empty());
    const std::vector<uint8_t> sim = {0, 1, 0, 0, 1, 1, 1, 1};
    bool both = false;
    std::string why;
    CHECK(groups_for(jumbled, p, &sim, &both, &why) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(both);
    CHECK(why.rfind("4 + 4 slots by address", 0) == 0);
    CHECK(why.find("WARNING") != std::string::npos);
    CHECK(groups_for(late, p, nullptr, &both, &why) == std::vector<uint8_t>{0, 0, 0, 0, 1, 1, 1, 1});
    CHECK(why.find("is not 4 + 4") != std::string::npos);
    // One camera streaming: one group.
    std::vector<size_t> four(late.begin(), late.begin() + 4);
    CHECK(groups_for(four, p, nullptr, &both, &why) == std::vector<uint8_t>{0, 0, 0, 0});
    CHECK_FALSE(both);
    CHECK(each_group_full({0, 0, 0, 0, 1, 1, 1, 1}));
    CHECK_FALSE(each_group_full({0, 1, 1, 1, 1}));
}

TEST_CASE("eye scan: the slot watcher copies a frame only after it is stable") {
    std::vector<uint8_t> buf(4u << 20, 0);
    const size_t off = 0x1000 + kHeaderBytes;   // One slot.
    DmaArena arena(buf.data(), buf.size());
    SlotWatcher w;
    w.reset(arena, {off});
    std::vector<uint8_t> copy(kHeaderBytes + kFrameBytes);
    uint64_t first = 0;
    CHECK(w.poll(arena, 0, 1000, copy.data(), copy.size(), &first) == SlotWatcher::Event::None);
    // A new frame: the header (timestamp) and the pixels change.
    const uint64_t stamp = 123456789;
    memcpy(&buf[off - kHeaderBytes], &stamp, 8);
    for (size_t i = 0; i < kFrameBytes; i++)
        buf[off + i] = uint8_t(40 + i % 13);
    CHECK(w.poll(arena, 0, 2000, copy.data(), copy.size(), &first) == SlotWatcher::Event::Changed);
    // Still changing: no copy yet, and the first change time stays.
    buf[off + 5000] ^= 0xff;
    buf[off + kFrameBytes / 512 * 10] ^= 0xff;   // A sampled byte, so that the fingerprint sees it.
    CHECK(w.poll(arena, 0, 3000, copy.data(), copy.size(), &first) == SlotWatcher::Event::Changed);
    // Stable for one poll: the frame comes out with its header.
    CHECK(w.poll(arena, 0, 4000, copy.data(), copy.size(), &first) == SlotWatcher::Event::Frame);
    CHECK(first == 2000);
    CHECK(header_stamp(copy.data()) == stamp);
    CHECK(memcmp(copy.data() + kHeaderBytes, &buf[off], kFrameBytes) == 0);
    // The same frame does not come out again.
    CHECK(w.poll(arena, 0, 5000, copy.data(), copy.size(), &first) == SlotWatcher::Event::None);
}

TEST_CASE("eye scan: the camera rate comes from the timestamps and follows a change") {
    CamRate r;
    uint64_t t = 1000000000ull;
    for (int i = 0; i < 120; i++, t += 16666667ull)
        r.push(t);
    CHECK(r.rate() == doctest::Approx(60.0).epsilon(0.01));
    // The camera goes to 10 fps: after 2 s, only the 10 fps frames are in the window.
    for (int i = 0; i < 21; i++, t += 100000000ull)
        r.push(t);
    CHECK(r.rate() == doctest::Approx(10.0).epsilon(0.05));
    // A frame out of order is skipped.
    CHECK(r.push(t - 500000000ull) == doctest::Approx(r.rate()));
    // A gap of more than 1 s starts again.
    r.push(t + 3000000000ull);
    CHECK(r.rate() == 0);
}

TEST_CASE("eye scan: pairs by timestamp") {
    StampPairer p;
    bool replaced = false;
    int dropped = -1;
    const uint64_t tol = 8000000;   // Half of a 60 fps period.
    // In sync, left first.
    p.put(0, 100000000, &replaced);
    CHECK(p.match(tol, &dropped) == StampPairer::Result::None);
    p.put(1, 100200000, &replaced);
    CHECK_FALSE(replaced);
    CHECK(p.match(tol, &dropped) == StampPairer::Result::Pair);
    CHECK(p.stamp(0) == 100000000);
    // The right camera skips a frame: the left frame is replaced without a pair.
    p.put(0, 116666667, &replaced);
    p.put(0, 133333333, &replaced);
    CHECK(replaced);
    // A right frame that is too far from the left one: the older side is dropped.
    p.put(1, 150000000, &replaced);
    CHECK(p.match(tol, &dropped) == StampPairer::Result::DroppedOne);
    CHECK(dropped == 0);
    p.put(0, 150100000, &replaced);
    CHECK(p.match(tol, &dropped) == StampPairer::Result::Pair);
}

TEST_CASE("eye scan: refine_start finds the first pixel behind a header") {
    // One frame with its 64-byte header, guessed one row and 64 bytes off, as the old crop did.
    std::vector<uint8_t> mem(2u << 20, 0);
    const size_t frame = 0x40000 + 64;
    for (size_t y = 0; y < kHeight; y++)
        for (size_t x = 0; x < kWidth; x++)
            mem[frame + y * kStride + x] = uint8_t(20 + (x * 3 + y * 7) % 50);
    const uint64_t stamp = 42;
    memcpy(&mem[frame - kHeaderBytes], &stamp, 8);
    const size_t guess = frame - kStride - kHeaderBytes;
    const size_t start = guess - kMarginRows * kStride;
    const size_t len = (kHeight + 2 * kMarginRows + 1) * kStride;
    const Refined r = refine_start(&mem[start], len, nullptr);
    CHECK(int64_t(guess) + r.shift == int64_t(frame));
    CHECK(r.pad_dirty < 0.001);
}
