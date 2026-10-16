#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace babble {

enum class PixelFormat { Gray8, Bayer8 /* The runtime treats raw Bayer as gray. */ };

enum class FrameOrigin { MouthL, MouthR, EyeL, EyeR, Face };   // Face is an external mono face tracker.

inline const char *to_string(FrameOrigin o) {
    switch (o) {
    case FrameOrigin::MouthL: return "mouthL";
    case FrameOrigin::MouthR: return "mouthR";
    case FrameOrigin::EyeL: return "eyeL";
    case FrameOrigin::EyeR: return "eyeR";
    case FrameOrigin::Face: return "face";
    }
    return "?";
}

// A captured image that is valid only while a FramePtr to it exists, because a FramePool owns its memory.
struct Frame {
    FrameOrigin origin = FrameOrigin::MouthL;
    PixelFormat fmt = PixelFormat::Gray8;
    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint64_t capture_ns = 0;   // The CLOCK_MONOTONIC time of the V4L2 buffer, or the detection time for DSP frames.
    uint64_t arrival_ns = 0;   // The time when the runtime wrapped the frame.
    uint32_t seq = 0;          // The driver sequence number or the counter of the source.
    uint32_t generation = 0;   // The source increments this value each time it opens again.
    uint32_t pool_index = 0;
};

using FramePtr = std::shared_ptr<const Frame>;

struct StereoFrame {
    FramePtr left, right;
    uint64_t capture_ns = 0;   // The midpoint of the two capture times.
    int64_t skew_ns = 0;       // The value is left.capture_ns minus right.capture_ns.
};

}
