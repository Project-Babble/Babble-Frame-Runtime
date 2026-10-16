#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace babble::capture {

// Uses the Qualcomm iris H.265 encoder through V4L2 directly because ffmpeg hevc_v4l2m2m hangs on this driver.
class HevcEncoder {
public:
    struct Settings {
        uint32_t width = 0, height = 0;
        double fps = 30;
        int qp = 20;
        int gop = 0;   // The number of frames between keyframes, where 0 means 2 s.
    };
    // Receives one packet of the stream and returns false to stop the encode.
    using PacketFn = std::function<bool(const uint8_t *data, size_t n)>;

    HevcEncoder() = default;
    ~HevcEncoder();
    HevcEncoder(const HevcEncoder &) = delete;
    HevcEncoder &operator=(const HevcEncoder &) = delete;

    // Returns the /dev/videoN node of the encoder, or "" if this machine has no encoder.
    static std::string find_device();

    bool open(const Settings &s, PacketFn on_packet, std::string *err, std::string device = "");
    // Encodes one gray frame of width x height and waits for a free input buffer.
    bool encode(const uint8_t *gray, std::string *err);
    // Drains the encoder and returns true when it has sent every packet.
    bool finish(std::string *err);
    void close();

    uint64_t frames_in() const { return frames_in_; }
    uint64_t packets_out() const { return packets_out_; }
    const std::string &device() const { return device_; }
    // The name and version of the driver for the package manifest.
    const std::string &driver() const { return driver_; }

private:
    struct Buf {
        void *p = nullptr;
        size_t len = 0;
    };
    // Takes back finished frame buffers and sends packets, and waits up to timeout_ms for an event.
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

}
