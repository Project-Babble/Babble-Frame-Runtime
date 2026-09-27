#pragma once

// The parts of USB Video Class (UVC 1.0-1.5) the runtime needs to stream an
// MJPEG camera from user space (the Frame's kernel has no uvcvideo driver):
// reading the descriptors, the probe/commit control block, and reassembling
// frames from payload transfers. Pure logic, no I/O (usb_device.h does that).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace babble::uvc {

struct FrameSize {
    uint8_t index = 0;                    // bFrameIndex
    uint16_t width = 0, height = 0;
    uint32_t default_interval = 0;        // 100 ns units
    uint32_t max_frame_bytes = 0;
};

// What a UVC device offers, from its configuration descriptor.
struct Info {
    uint16_t bcd_uvc = 0;                 // e.g. 0x0110, 0x0150
    uint8_t control_iface = 0xff, stream_iface = 0xff;
    uint8_t stream_alt = 0;               // bulk streaming: alternate setting 0
    uint8_t endpoint = 0;                 // streaming IN endpoint address
    uint16_t endpoint_max_packet = 0;
    bool bulk = false;                    // bulk (true) or isochronous endpoint
    uint8_t mjpeg_format = 0;             // bFormatIndex of the MJPEG format, 0 = none
    std::vector<FrameSize> mjpeg_frames;
};

// Parses a full configuration descriptor (as in sysfs "descriptors", after the
// 18-byte device descriptor, or as returned by GET_DESCRIPTOR). nullopt if it
// has no video streaming interface.
std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error = nullptr);

// The frame size closest to width x height (exact match first, else the first).
const FrameSize *pick_frame(const Info &info, uint16_t width, uint16_t height);

// VS_PROBE_CONTROL / VS_COMMIT_CONTROL block. Its length depends on the UVC
// version: 26 bytes (1.0), 34 (1.1), 48 (1.5).
struct Probe {
    uint16_t hint = 1;                    // bmHint: dwFrameInterval fixed
    uint8_t format_index = 0, frame_index = 0;
    uint32_t frame_interval = 0;          // 100 ns units
    uint32_t max_video_frame_size = 0;    // from the device
    uint32_t max_payload_transfer_size = 0;
};
size_t probe_length(uint16_t bcd_uvc);
std::vector<uint8_t> encode_probe(const Probe &p, uint16_t bcd_uvc);
std::optional<Probe> decode_probe(const uint8_t *data, size_t size);

// Rebuilds frames from payload transfers. Each payload starts with a header
// (bHeaderLength, bmHeaderInfo: FID bit 0, EOF bit 1, ERR bit 6); a frame
// ends at EOF or when FID toggles. A frame with an error bit, or that grows
// past max_bytes, is dropped.
class FrameAssembler {
public:
    explicit FrameAssembler(size_t max_bytes = 512 * 1024) : max_bytes_(max_bytes) {}
    // Adds one payload; returns true when a frame completed (take it with frame()).
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

// A JPEG that starts with SOI (FF D8) and ends with EOI (FF D9).
bool looks_like_jpeg(const uint8_t *data, size_t size);

} // namespace babble::uvc
