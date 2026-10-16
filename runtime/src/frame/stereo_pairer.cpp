#include "frame/stereo_pairer.h"

#include <cstdlib>

namespace babble {

StereoPairer::StereoPairer(uint64_t tolerance_ns, FrameOrigin left, FrameOrigin right)
    : tolerance_ns_(tolerance_ns), left_(left), right_(right) {}

std::optional<StereoFrame> StereoPairer::push(FramePtr frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool is_left = frame->origin == left_;
    FramePtr &mine = is_left ? pending_left_ : pending_right_;
    FramePtr &other = is_left ? pending_right_ : pending_left_;
    uint64_t &my_unmatched = is_left ? counts_.unmatched_left : counts_.unmatched_right;
    uint64_t &other_unmatched = is_left ? counts_.unmatched_right : counts_.unmatched_left;

    if (other) {
        int64_t d = int64_t(frame->capture_ns) - int64_t(other->capture_ns);
        if (std::llabs(d) <= int64_t(tolerance_ns_)) {
            StereoFrame sf;
            sf.left = is_left ? frame : other;
            sf.right = is_left ? other : frame;
            sf.skew_ns = int64_t(sf.left->capture_ns) - int64_t(sf.right->capture_ns);
            sf.capture_ns = (sf.left->capture_ns + sf.right->capture_ns) / 2;
            other.reset();
            mine.reset();
            counts_.paired++;
            return sf;
        }
        if (d > 0) {
            // Drop the waiting frame because it is too old to match.
            other.reset();
            other_unmatched++;
        } else {
            // Drop this frame because it arrived late and is older than the waiting frame.
            my_unmatched++;
            return std::nullopt;
        }
    }
    if (mine)
        my_unmatched++;
    mine = std::move(frame);
    return std::nullopt;
}

void StereoPairer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_left_.reset();
    pending_right_.reset();
}

StereoPairer::Counts StereoPairer::counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

}
