#pragma once

#include <cstdint>

namespace babble::gaze {

// One gaze update of the steamvr_gaze pipeline, which the recorder writes to gaze.bin.
struct Sample {
    enum class Source : uint8_t { Centre = 0, Independent = 1, Combined = 2 };
    uint64_t ns = 0;                 // The CLOCK_MONOTONIC time of the update.
    Source source = Source::Centre;  // Centre is the 0,0,0,0 that the pipeline sends when the gaze goes stale.
    int8_t dominant_eye = -1;        // The "Track Dominant Eye Only" eye, or -1 if it is off or unknown.
    uint8_t smoothing = 0;           // The smoothing level, where 0 is off.
    float raw[4] = {};               // LeftEyeX, LeftEyeY, RightEyeX and RightEyeY before the smoothing.
    float out[4] = {};               // The same values as the outputs got them.
};

}
