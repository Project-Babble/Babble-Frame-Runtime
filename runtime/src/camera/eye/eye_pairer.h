#pragma once

#include <cstdint>

namespace babble {

// Decides when the two eye cameras' ring indices make a new stereo pair.
// dsp_service publishes the left and right index separately, a moment apart,
// so a pair is complete only once *both* have advanced since the last pair.
// Counting either change as a pair counted each camera frame up to twice and
// paired a fresh image with a stale one. If one side advances and the other
// doesn't follow within `wait_ns`, that side is given up on (a half pair).
//
// Pure: fed the indices from each poll. Indices are compared for change only;
// kNotReady (0xffffffff) means the ring hasn't started.
class EyePairer {
public:
    static constexpr uint32_t kNotReady = 0xffffffffu;

    enum class Result {
        None,   // nothing new, or waiting for the other side
        Pair,   // both sides advanced: (l, r) is a new pair
        Half,   // one side advanced and the other never followed; release it
    };

    struct Step {
        Result result = Result::None;
        bool new_l = false, new_r = false;   // that side's index changed since the previous poll
        bool half_l = false, half_r = false; // for Half: the side(s) given up on
        // For Pair: when each side was first seen advanced (detection time, to
        // the poll interval). Their difference is the pair's left/right skew.
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
            // Whatever is in the ring at start is old: the first pair is the
            // next one both cameras publish.
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
        // The other camera never followed: drop the lone side.
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
    uint64_t since_l_ = 0, since_r_ = 0;   // when each side was first seen advanced (0 = not yet)
};

} // namespace babble
