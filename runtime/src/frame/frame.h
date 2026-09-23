#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace babble {

enum class PixelFormat { Gray8, Bayer8 /* raw Bayer treated as gray */ };

enum class FrameOrigin { MouthL, MouthR, EyeL, EyeR };

inline const char *to_string(FrameOrigin o) {
    switch (o) {
    case FrameOrigin::MouthL: return "mouthL";
    case FrameOrigin::MouthR: return "mouthR";
    case FrameOrigin::EyeL: return "eyeL";
    case FrameOrigin::EyeR: return "eyeR";
    }
    return "?";
}

// A captured image. Memory belongs to a FramePool; the Frame is only valid
// while a FramePtr to it is held.
struct Frame {
    FrameOrigin origin = FrameOrigin::MouthL;
    PixelFormat fmt = PixelFormat::Gray8;
    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    uint64_t capture_ns = 0;   // CLOCK_MONOTONIC: V4L2 buffer timestamp, or detection time for DSP frames
    uint64_t arrival_ns = 0;   // when the runtime wrapped it
    uint32_t seq = 0;          // driver sequence number or the source's own counter
    uint32_t generation = 0;   // bumped by the source on every reopen
    uint32_t pool_index = 0;
};

using FramePtr = std::shared_ptr<const Frame>;

struct StereoFrame {
    FramePtr left, right;
    uint64_t capture_ns = 0;   // midpoint of the two capture times
    int64_t skew_ns = 0;       // left.capture_ns - right.capture_ns
};

} // namespace babble
