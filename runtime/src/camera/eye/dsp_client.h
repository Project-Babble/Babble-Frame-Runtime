#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace babble {

// Client for Valve's dsp_service eye-camera shared memory on the Steam Frame.
// Reverse-engineered (reference: ~/streamer/final_recording_daemon.py on the
// headset) and already moved once in a SteamVR update, so nothing is written
// until the control-block layout has been validated read-only.
class DspClient {
public:
    static constexpr size_t kRegionBytes = 0x1000000;       // each mapping is 16 MB
    static constexpr off_t kCtrlOffset = 0x80000000LL;       // control-block mapping offset
    static constexpr uint32_t kNotReady = 0xffffffffu;

    // Offsets are relative to ctrl_base. Indices/locks/fps/enables live in the
    // control mapping; the frame rings live in the data mapping.
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

    // The service's abstract socket name (without the leading NUL).
    static constexpr std::string_view kServiceName = "dsp_service";

    ~DspClient();

    // `request` is the word sent after the fds, which dsp_service reads as the
    // client's request type, a bit set: 1 IMU Streaming, 2 ET Camera Streaming,
    // 4 CDSP Operations, 8 Audio Streaming, 16 CDSP Gazenet, 32 Charger.
    // Clients asking for 1 or 2 count towards keeping the IMU or the eye
    // cameras streaming, and every client with a matching bit gets a
    // "service_notification" message per DSP event, on a non-blocking socket.
    // We only read the shared memory, which the fds give whatever the type,
    // so 0: nothing is sent to us and we change no streaming state.
    bool connect(uint32_t request, std::string_view name = kServiceName);
    void disconnect();
    bool connected() const { return ctrl_ != nullptr; }
    // Reads and discards anything the service sent since; returns the bytes.
    // Unread messages fill the socket and dsp_service logs an error per message
    // it can no longer send.
    size_t drain();

    // Finds the first candidate base whose control words look sane. Reads only.
    std::optional<Layout> probe(std::span<const uint32_t> candidates, std::string *why);

    // True if the frame-rate word is 0 at every candidate: the control block
    // hasn't been set up yet (the service just started and no one has asked
    // for the eye cameras), as opposed to a layout we don't recognise.
    static bool fps_unset(const uint8_t *ctrl, size_t ctrl_size, std::span<const uint32_t> candidates);

    // Pure validator over a control-block image, shared with the unit tests.
    static bool validate(const uint8_t *ctrl, size_t ctrl_size, const Layout &layout, std::string *why);

    // Replicates the known start sequence (locks free, enables, trigger).
    // The frame rate is left alone: SteamVR's eye service owns it and sets it
    // from the display refresh rate on every headset-on and display mode change.
    void start_streaming(const Layout &layout);
    // The camera frame rate SteamVR's eye service set, in fps; 0 if unset or insane.
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

} // namespace babble
