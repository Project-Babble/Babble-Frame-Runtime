#pragma once

// The parts of USB Video Class that the runtime uses to stream an MJPEG camera from user space, without I/O.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace babble::uvc {

struct FrameSize {
    uint8_t index = 0;                    // This is bFrameIndex.
    uint16_t width = 0, height = 0;
    uint32_t default_interval = 0;        // The unit is 100 ns.
    uint32_t max_frame_bytes = 0;
};

// The features of a UVC device, from its configuration descriptor.
struct Info {
    uint16_t bcd_uvc = 0;                 // The UVC version, for example 0x0110.
    uint8_t control_iface = 0xff, stream_iface = 0xff;
    uint8_t stream_alt = 0;               // Bulk streaming uses alternate setting 0.
    uint8_t endpoint = 0;                 // The address of the streaming IN endpoint.
    uint16_t endpoint_max_packet = 0;
    bool bulk = false;                    // True for a bulk endpoint, false for an isochronous endpoint.
    uint8_t mjpeg_format = 0;             // The bFormatIndex of the MJPEG format, or 0 if there is none.
    std::vector<FrameSize> mjpeg_frames;
};

// Parses a full configuration descriptor and returns nullopt if it has no video streaming interface.
std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error = nullptr);

// Returns the frame size that matches width x height, or else the first frame size.
const FrameSize *pick_frame(const Info &info, uint16_t width, uint16_t height);

// The VS_PROBE_CONTROL and VS_COMMIT_CONTROL block, with a length that changes with the UVC version.
struct Probe {
    uint16_t hint = 1;                    // This bmHint keeps dwFrameInterval fixed.
    uint8_t format_index = 0, frame_index = 0;
    uint32_t frame_interval = 0;          // The unit is 100 ns.
    uint32_t max_video_frame_size = 0;    // The device sets this value.
    uint32_t max_payload_transfer_size = 0;
};
size_t probe_length(uint16_t bcd_uvc);
std::vector<uint8_t> encode_probe(const Probe &p, uint16_t bcd_uvc);
std::optional<Probe> decode_probe(const uint8_t *data, size_t size);

// Builds frames from payload transfers and drops a frame with an error bit or more than max_bytes.
class FrameAssembler {
public:
    explicit FrameAssembler(size_t max_bytes = 512 * 1024) : max_bytes_(max_bytes) {}
    // Adds one payload and returns true when a frame is complete in frame().
    bool push(const uint8_t *payload, size_t size);
    std::vector<uint8_t> &frame() { return done_; }
    uint64_t dropped() const { return dropped_; }
    void reset();

private:
    bool finish();
    size_t max_bytes_;
    std::vector<uint8_t> building_, done_;
    int fid_ = -1;
    bool bad_ = false;
    uint64_t dropped_ = 0;
};

// Returns true if the data starts with SOI (FF D8) and ends with EOI (FF D9).
bool looks_like_jpeg(const uint8_t *data, size_t size);

}
