#pragma once

// Watches the slots of a found ring and copies each new frame only when it is complete.
// SlotWatcher is ported from eyecam's SlotStates (tools/eyecam/src/bin/eyecam-rec.rs) and CamRate
// (tools/eyecam/src/livesvc.rs) in https://github.com/sasaken1102r/frameeyeosc.
// StampPairer is new: eyecam processes each eye alone, but this runtime needs stereo pairs.

#include "camera/eye/scan/dma_arena.h"
#include "camera/eye/scan/eye_ring.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace babble::eyescan {

// For each slot: the last fingerprint and header seen, and since when the slot has changed.
class SlotWatcher {
public:
    enum class Event {
        None,      // No change.
        Changed,   // The slot is being written, or was written since the last poll.
        Torn,      // The slot changed during the copy, so the copy was dropped.
        Frame,     // A complete new frame is in `buf`. It started to change at `*first_ns`.
    };

    void reset(const DmaArena &arena, const std::vector<size_t> &off) {
        off_ = off;
        fp_.resize(off.size());
        header_.resize(off.size());
        dirty_since_.assign(off.size(), 0);
        for (size_t k = 0; k < off.size(); k++) {
            arena.copy(off[k] - kHeaderBytes, header_[k].data(), kHeaderBytes);
            fp_[k] = arena.fingerprint(off[k], kFrameBytes);
        }
    }

    size_t size() const { return off_.size(); }

    // A slot is copied when its frame and header did not change over one poll. The copy is kept only if the slot did
    // not change during the copy, so a frame that is being written is never sent half old. `buf` gets `copy_len`
    // bytes from the start of the slot header.
    Event poll(const DmaArena &arena, size_t k, uint64_t now_ns, uint8_t *buf, size_t copy_len, uint64_t *first_ns) {
        const size_t off = off_[k];
        const uint64_t fp = arena.fingerprint(off, kFrameBytes);
        std::array<uint8_t, kHeaderBytes> header;
        arena.copy(off - kHeaderBytes, header.data(), kHeaderBytes);
        if (fp != fp_[k] || header != header_[k]) {
            fp_[k] = fp;
            header_[k] = header;
            if (!dirty_since_[k])
                dirty_since_[k] = now_ns;
            return Event::Changed;
        }
        if (!dirty_since_[k])
            return Event::None;
        arena.copy(off - kHeaderBytes, buf, copy_len);
        if (arena.fingerprint(off, kFrameBytes) != fp || memcmp(buf, header.data(), kHeaderBytes) != 0)
            return Event::Torn;
        *first_ns = dirty_since_[k];
        dirty_since_[k] = 0;
        return Event::Frame;
    }

private:
    std::vector<size_t> off_;
    std::vector<uint64_t> fp_;
    std::vector<std::array<uint8_t, kHeaderBytes>> header_;
    std::vector<uint64_t> dirty_since_;   // 0 when the slot did not change.
};

// The camera timestamp at the start of a slot header (u64 LE, CLOCK_MONOTONIC_RAW ns).
inline uint64_t header_stamp(const uint8_t *header) {
    uint64_t t;
    memcpy(&t, header, sizeof(t));
    return t;
}

// One camera's frame rate from its timestamps over the last 2 s. Starts again when the frames stop for 1 s.
class CamRate {
public:
    static constexpr uint64_t kWindowNs = 2000000000ull;

    double push(uint64_t t) {
        if (!recent_.empty()) {
            if (t <= recent_.back())
                return rate();   // A frame out of order: skip it.
            if (t - recent_.back() > 1000000000ull)
                recent_.clear();
        }
        recent_.push_back(t);
        while (t - recent_.front() > kWindowNs)
            recent_.pop_front();
        return rate();
    }

    // Frames per second, or 0 with fewer than 2 frames.
    double rate() const {
        if (recent_.size() < 2 || recent_.back() <= recent_.front())
            return 0;
        return double(recent_.size() - 1) * 1e9 / double(recent_.back() - recent_.front());
    }

    void clear() { recent_.clear(); }

private:
    std::deque<uint64_t> recent_;
};

// Makes stereo pairs from the camera timestamps. Each side keeps its newest frame. When both sides have one and
// their timestamps are within `tol_ns`, they are a pair. Otherwise the older side is dropped, because it has no
// partner.
class StampPairer {
public:
    enum class Result { None, Pair, DroppedOne };

    // Records a new frame of side `g` (0 or 1). `replaced` is true when the side had an unpaired frame.
    void put(int g, uint64_t t, bool *replaced) {
        *replaced = have_[g];
        have_[g] = true;
        t_[g] = t;
    }

    Result match(uint64_t tol_ns, int *dropped_side) {
        if (!have_[0] || !have_[1])
            return Result::None;
        const uint64_t d = t_[0] > t_[1] ? t_[0] - t_[1] : t_[1] - t_[0];
        if (d <= tol_ns) {
            have_[0] = have_[1] = false;
            return Result::Pair;
        }
        const int older = t_[0] < t_[1] ? 0 : 1;
        have_[older] = false;
        *dropped_side = older;
        return Result::DroppedOne;
    }

    uint64_t stamp(int g) const { return t_[g]; }
    void reset() { have_[0] = have_[1] = false; }

private:
    bool have_[2] = {false, false};
    uint64_t t_[2] = {0, 0};
};

}
