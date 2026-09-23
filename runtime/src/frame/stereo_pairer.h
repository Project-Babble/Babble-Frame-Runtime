#pragma once

#include "frame/frame.h"

#include <cstdint>
#include <mutex>
#include <optional>

namespace babble {

// Pairs frames from two independent capture streams by capture timestamp.
// A frame waits for its counterpart; when the counterpart arrives within the
// tolerance they are emitted as one StereoFrame. Frames that never find a
// partner are dropped and counted, never emitted alone.
class StereoPairer {
public:
    struct Counts {
        uint64_t paired = 0, unmatched_left = 0, unmatched_right = 0;
    };

    StereoPairer(uint64_t tolerance_ns, FrameOrigin left, FrameOrigin right);

    // Thread-safe; called from both capture threads.
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

} // namespace babble
