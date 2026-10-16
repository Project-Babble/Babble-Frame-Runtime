#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace babble {

// Client for Valve's dsp_service eye-camera shared memory on the Steam Frame.
class DspClient {
public:
    static constexpr size_t kRegionBytes = 0x1000000;       // Each mapping is 16 MB.
    static constexpr off_t kCtrlOffset = 0x80000000LL;       // The file offset of the control-block mapping.
    static constexpr uint32_t kNotReady = 0xffffffffu;

    // All offsets are relative to ctrl_base, and the frame rings are in the data mapping.
    struct Layout {
        uint32_t ctrl_base = 0;
        uint32_t transition = 0x04, fps = 0x10, enables = 0x48;
        uint32_t idx_l = 0x80, locks_l = 0x84;
        uint32_t idx_r = 0x1001c0, locks_r = 0x1001c4;
        uint32_t ring_l = 0xc0, ring_r = 0x100200;
        uint32_t slot_stride = 0x40040, frame_bytes = 0x40000, slots = 8;
        uint32_t width = 512, height = 512;
    };

    struct Indices {
        uint32_t l = kNotReady, r = kNotReady;
    };

    // The abstract socket name of the service, without the leading NUL.
    static constexpr std::string_view kServiceName = "dsp_service";

    ~DspClient();

    // Use a `request` type of 0 to get the shared memory without messages and without a change to the streaming state.
    bool connect(uint32_t request, std::string_view name = kServiceName);
    void disconnect();
    bool connected() const { return ctrl_ != nullptr; }
    // Reads and discards all messages from the service because unread messages fill the socket and cause errors.
    size_t drain();

    // Finds the first candidate base with valid control words, and does not write to the memory.
    std::optional<Layout> probe(std::span<const uint32_t> candidates, std::string *why);

    // Returns true if the frame-rate word is 0 at all candidates, because the service did not set up the cameras yet.
    static bool fps_unset(const uint8_t *ctrl, size_t ctrl_size, std::span<const uint32_t> candidates);

    // Validates a control-block image, and the unit tests also use this function.
    static bool validate(const uint8_t *ctrl, size_t ctrl_size, const Layout &layout, std::string *why);

    // Does the known start sequence but does not change the frame rate, because SteamVR's eye service sets it.
    void start_streaming(const Layout &layout);
    // Returns the camera frame rate in fps that SteamVR's eye service set, or 0 if it is not valid.
    float read_fps(const Layout &layout) const;

    Indices read_indices(const Layout &layout) const;
    const uint8_t *slot_ptr(const Layout &layout, bool right, uint32_t idx) const;
    void release(const Layout &layout, bool right, uint32_t idx);

    const uint8_t *ctrl() const { return ctrl_; }

private:
    uint32_t load32(uint32_t off) const;
    void store32(uint32_t off, uint32_t v);

    int sock_ = -1;
    int shm_fd_ = -1;
    uint8_t *data_ = nullptr;
    uint8_t *ctrl_ = nullptr;
};

}
