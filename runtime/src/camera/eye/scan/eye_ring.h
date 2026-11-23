#pragma once

// Finds the ring of eye camera frames in the dsp_service buffer from the frame contents alone, without the control
// block. Ported from eyecam (tools/eyecam/src/ring.rs in https://github.com/sasaken1102r/frameeyeosc), which ports
// steps 1-6 from FrameEyeCameraFeed framestream.c.
#include "camera/eye/scan/dma_arena.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace babble::eyescan {

constexpr size_t kStride = 512;
constexpr size_t kHeight = 400;
constexpr size_t kWidth = 400;
constexpr size_t kFrameBytes = kStride * kHeight;
constexpr size_t kHeaderBytes = 64;
constexpr size_t kSlot = 262144;
constexpr size_t kExpectedPitch = kSlot + kHeaderBytes;
constexpr size_t kMaxRing = 32;
constexpr size_t kMarginRows = 16;      // How far (rows) the framing refinement can move a frame start.
constexpr uint32_t kMinRefreshes = 3;   // A slot refreshes at 1/4 of the camera rate: 3.75 times a second at 15 fps.
constexpr size_t kSlotsPerCamera = 4;

// Time and pacing for the search, so that tests can drive it with a simulated camera.
class ScanClock {
public:
    virtual ~ScanClock() = default;
    virtual double now() = 0;   // Seconds.
    virtual void wait() = 0;    // Pauses a few milliseconds between polls.
    virtual bool stopped() const { return false; }
};

// Where the last search stopped, for the log.
struct Look {
    size_t candidates = 0;
    double refresh_hz = 0;
    size_t slots = 0;
    bool both_groups = false;
    const char *stopped_at = "no_candidates";   // "" when found.
};

struct Ring {
    size_t pitch = kExpectedPitch;
    std::vector<size_t> off;          // The first pixel of each slot's frame, in memory order. The header is before it.
    std::vector<uint8_t> group;       // 0 = lower-address camera group, 1 = higher-address group.
    bool both_groups = false;
    std::vector<int64_t> shift;       // Bytes that each start moved from the ring walk.
    std::vector<double> pad_dirty;    // The share of padding bytes that were nonzero or changed (0 = clean).
    std::string reason;               // How the groups were decided.
};

// The lit share and roughness of a frame, sampled on a 4-pixel grid. p must hold kFrameBytes.
void frame_stats(const uint8_t *p, double *lit, double *rough);
// Real eye frames are 27-34% lit; the recycled heap that the search must not lock onto is 9-15% lit.
bool looks_like_eye_frame(const uint8_t *p);

// The exact first pixel of a frame. `region` holds the memory from kMarginRows rows before a guessed start (within
// a row or so of the truth) to kMarginRows + 1 rows after its end, and `changes` (optional, same size) how often each
// byte changed while it was watched. Returns the start relative to the guess, and the dirty share of the padding.
struct Refined {
    int64_t shift = 0;
    double pad_dirty = 0;
};
Refined refine_start(const uint8_t *region, size_t region_len, const uint16_t *changes);

// The group of each slot from the layout alone (offsets in memory order). Empty when the layout does not show two
// groups. `reason` gets a description.
std::vector<uint8_t> assign_groups(const std::vector<size_t> &off, size_t pitch, std::string *reason);
// The groups, whether both groups stream, and how that was decided. `similar` (optional): which slots look like
// slot 0 by their pictures; it never decides, only adds a warning.
std::vector<uint8_t> groups_for(const std::vector<size_t> &off, size_t pitch, const std::vector<uint8_t> *similar,
                                bool *both, std::string *reason);
bool each_group_full(const std::vector<uint8_t> &group);

// Looks for the ring in memory that changed recently (DmaArena::block_changed). Takes about 1.6 s when there are
// candidates. `log` gets one line for each decision.
bool discover(const DmaArena &arena, ScanClock &clock, const std::function<void(const std::string &)> &log,
              Look *look, Ring *out);

}
