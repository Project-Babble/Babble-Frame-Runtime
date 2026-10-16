#pragma once

#include <cstdint>

namespace babble {

// The link to the driver shim is a pipe on fd 3, and EOF on it means that vrserver is gone.
class ShimLink {
public:
    static constexpr int kFd = 3;

    struct MsgHeader {
        uint16_t type;
        uint16_t len;   // The number of payload bytes after the header.
        uint32_t seq;
    };
    enum MsgType : uint16_t {
        Msg_Hello = 1,
        Msg_Standby = 2,
        Msg_Resume = 3,
        Msg_Gaze = 4,
    };

    // Returns true if fd 3 is a pipe or socket from the shim.
    static bool present();

    // Reads and discards all readable data, and returns false on EOF or an error.
    static bool drain();
};

// Blocks the runtime signals in the process and returns a signalfd that receives them.
int make_signalfd();

}
