#pragma once

#include "frame/frame.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace babble {

// Pairs frames from two capture streams by capture time, and drops and counts the frames that have no partner.
class StereoPairer {
public:
    struct Counts {
        uint64_t paired = 0, unmatched_left = 0, unmatched_right = 0;
    };

    StereoPairer(uint64_t tolerance_ns, FrameOrigin left, FrameOrigin right);

    // Both capture threads call this function, and it is thread-safe.
    std::optional<StereoFrame> push(FramePtr frame);

    void reset();
    Counts counts() const;

private:
    uint64_t tolerance_ns_;
    FrameOrigin left_, right_;
    mutable std::mutex mutex_;
    FramePtr pending_left_, pending_right_;
    Counts counts_;
};

}
