#pragma once

#include <cstdint>

namespace babble {

// Makes a stereo pair only when both ring indices changed, because dsp_service writes them at different times.
class EyePairer {
public:
    static constexpr uint32_t kNotReady = 0xffffffffu;

    enum class Result {
        None,   // There is no new pair yet.
        Pair,   // Both sides changed, so (l, r) is a new pair.
        Half,   // One side changed and the other side did not follow, so release the lone side.
    };

    struct Step {
        Result result = Result::None;
        bool new_l = false, new_r = false;   // True if the index of that side changed after the previous poll.
        bool half_l = false, half_r = false; // For Half, these are the lone sides.
        // For Pair, these are the detection times of each side.
        uint64_t l_ns = 0, r_ns = 0;
    };

    Step feed(uint32_t l, uint32_t r, uint64_t now_ns, uint64_t wait_ns) {
        Step s;
        if (l == kNotReady || r == kNotReady)
            return s;
        s.new_l = seen_l_ != kNotReady && l != seen_l_;
        s.new_r = seen_r_ != kNotReady && r != seen_r_;
        const bool first = seen_l_ == kNotReady || seen_r_ == kNotReady;
        seen_l_ = l;
        seen_r_ = r;
        if (first) {
            // The frames in the ring at the start are old, so wait for the next pair.
            paired_l_ = l;
            paired_r_ = r;
            return s;
        }
        const bool adv_l = l != paired_l_, adv_r = r != paired_r_;
        if (adv_l && !since_l_)
            since_l_ = now_ns;
        if (adv_r && !since_r_)
            since_r_ = now_ns;
        if (adv_l && adv_r) {
            s.l_ns = since_l_;
            s.r_ns = since_r_;
            paired_l_ = l;
            paired_r_ = r;
            pending_since_ = since_l_ = since_r_ = 0;
            s.result = Result::Pair;
            return s;
        }
        if (!adv_l && !adv_r) {
            pending_since_ = since_l_ = since_r_ = 0;
            return s;
        }
        if (pending_since_ == 0) {
            pending_since_ = now_ns;
            return s;
        }
        if (now_ns - pending_since_ <= wait_ns)
            return s;
        // The other camera did not follow, so drop the lone side.
        s.result = Result::Half;
        s.half_l = adv_l;
        s.half_r = adv_r;
        paired_l_ = l;
        paired_r_ = r;
        pending_since_ = since_l_ = since_r_ = 0;
        return s;
    }

    bool waiting() const { return pending_since_ != 0; }

    void reset() {
        seen_l_ = seen_r_ = paired_l_ = paired_r_ = kNotReady;
        pending_since_ = since_l_ = since_r_ = 0;
    }

private:
    uint32_t seen_l_ = kNotReady, seen_r_ = kNotReady;
    uint32_t paired_l_ = kNotReady, paired_r_ = kNotReady;
    uint64_t pending_since_ = 0;
    uint64_t since_l_ = 0, since_r_ = 0;   // The time when each side changed, or 0 if it did not change yet.
};

}
