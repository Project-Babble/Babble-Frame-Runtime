#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace babble::capture {

// The headset's hardware H.265 encoder (Qualcomm iris, driver qcom-iris)
// through the V4L2 stateful encoder interface. ffmpeg's hevc_v4l2m2m hangs
// on this driver, so the packager drives it directly.
//
// Input is 8-bit gray frames: they go into Y unchanged, with chroma at 128,
// and the stream is tagged full range. Constant QP is set by pinning every
// QP limit, because the driver ignores FRAME_RC_ENABLE=0
// (docs/dataset-format.md, "The hardware encoder"). Output is an Annex B
// elementary stream, handed over packet by packet.
//
// One owner thread. close() (also run by the destructor) always stops
// streaming before closing the device, so the firmware sees a clean end.
class HevcEncoder {
public:
    struct Settings {
        uint32_t width = 0, height = 0;
        double fps = 30;
        int qp = 20;
        int gop = 0;   // frames between keyframes; 0 = 2 s
    };
    // A packet of the stream; false stops the encode (encode() then fails).
    using PacketFn = std::function<bool(const uint8_t *data, size_t n)>;

    HevcEncoder() = default;
    ~HevcEncoder();
    HevcEncoder(const HevcEncoder &) = delete;
    HevcEncoder &operator=(const HevcEncoder &) = delete;

    // The encoder's device node (/dev/videoN), found by driver and card name;
    // "" if this machine has none.
    static std::string find_device();

    bool open(const Settings &s, PacketFn on_packet, std::string *err, std::string device = "");
    // One width x height gray frame. Waits for a free input buffer.
    bool encode(const uint8_t *gray, std::string *err);
    // Drains the encoder: every packet has been delivered when it returns true.
    bool finish(std::string *err);
    void close();

    uint64_t frames_in() const { return frames_in_; }
    uint64_t packets_out() const { return packets_out_; }
    const std::string &device() const { return device_; }
    // The driver's name and version (VIDIOC_QUERYCAP), for the package manifest.
    const std::string &driver() const { return driver_; }

private:
    struct Buf {
        void *p = nullptr;
        size_t len = 0;
    };
    // Takes back finished frame buffers and delivers packets; waits up to
    // timeout_ms for something to happen (0: just collect what's ready).
    bool service(int timeout_ms, std::string *err);
    bool fail(std::string *err, const std::string &what);

    int fd_ = -1;
    Settings s_;
    PacketFn on_packet_;
    std::string device_, driver_;
    uint32_t stride_ = 0, padded_h_ = 0, out_bytes_ = 0;
    std::vector<Buf> out_, cap_;
    std::vector<int> free_out_;
    bool streaming_ = false, last_ = false, packet_failed_ = false;
    uint64_t frames_in_ = 0, packets_out_ = 0;
};

} // namespace babble::capture
