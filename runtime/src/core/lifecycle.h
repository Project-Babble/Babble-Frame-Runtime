#pragma once

#include <cstdint>

namespace babble {

// Link to the driver shim (driver/src/driver_babble.cpp). Today it is a pipe
// whose read end is fd 3: EOF means vrserver is gone. The message header is
// defined now so the same fd can become a socketpair carrying standby/resume
// and gaze samples later without changing the framing.
class ShimLink {
public:
    static constexpr int kFd = 3;

    struct MsgHeader {
        uint16_t type;
        uint16_t len;   // payload bytes following the header
        uint32_t seq;
    };
    enum MsgType : uint16_t {
        Msg_Hello = 1,
        Msg_Standby = 2,
        Msg_Resume = 3,
        Msg_Gaze = 4,
    };

    // True if fd 3 is a pipe or socket the shim handed us.
    static bool present();

    // Reads and discards whatever is readable; returns false on EOF/error.
    static bool drain();
};

// Signals handled by the runtime, blocked process-wide and delivered through signalfd.
int make_signalfd();

} // namespace babble
