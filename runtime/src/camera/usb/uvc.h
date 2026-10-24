#pragma once

// The parts of USB Video Class that the runtime uses to stream a camera from user space, without I/O.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace babble::uvc {

// The video formats that the runtime can turn into grayscale frames.
enum class FormatKind { Mjpeg, Yuy2, Uyvy, Nv12, Grey, Unknown };
// Returns the setting name, such as "mjpeg" or "yuy2", or "unknown".
const char *format_key(FormatKind kind);
// Returns the name that the dashboard shows, such as "MJPEG" or "YUY2".
const char *format_label(FormatKind kind);
// Returns the kind for a setting name, or Unknown.
FormatKind format_from_key(const std::string &key);

struct FrameSize {
    uint8_t index = 0;                    // This is bFrameIndex.
    uint16_t width = 0, height = 0;
    uint32_t default_interval = 0;        // The unit is 100 ns.
    uint32_t max_frame_bytes = 0;
    // The frame intervals that the camera offers. For a continuous range, this holds the min and the max.
    std::vector<uint32_t> intervals;
    bool continuous = false;
};

// One format of the video streaming interface, with its frame sizes.
struct Format {
    uint8_t index = 0;                    // This is bFormatIndex.
    FormatKind kind = FormatKind::Unknown;
    uint8_t bits_per_pixel = 0;           // This is 0 for MJPEG.
    std::vector<FrameSize> frames;
};

// An alternate setting of the video streaming interface with an isochronous IN endpoint.
struct Alt {
    uint8_t alt = 0, endpoint = 0;
    uint32_t bytes = 0;                   // The bytes for each (micro)frame, with the high-bandwidth multiplier.
};

// The features of a UVC device, from its configuration descriptor.
struct Info {
    uint16_t bcd_uvc = 0;                 // The UVC version, for example 0x0110.
    uint8_t control_iface = 0xff, stream_iface = 0xff;
    uint8_t endpoint = 0;                 // The address of the streaming IN endpoint.
    uint16_t endpoint_max_packet = 0;     // The bulk packet size, or the largest isochronous alt in bytes.
    bool bulk = false;                    // True for a bulk endpoint on alt 0, false for isochronous alts.
    std::vector<Alt> alts;                // The isochronous alternate settings, smallest first.
    std::vector<Format> formats;          // The formats in descriptor order, Unknown ones included.
};

// Parses a full configuration descriptor and returns nullopt if it has no video streaming interface or no format
// that the runtime can decode.
std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error = nullptr);

// The stream that the user wants: the format, the frame size and the rate, where 0 means the camera default.
struct Mode {
    FormatKind format = FormatKind::Mjpeg;
    uint16_t width = 0, height = 0;
    double fps = 0;
    bool operator==(const Mode &) const = default;
};

// The format, the frame size and the interval that pick_mode() found.
struct Picked {
    const Format *format = nullptr;
    const FrameSize *frame = nullptr;
    uint32_t interval = 0;                // The unit is 100 ns.
};

// Returns the nearest mode that the camera offers. A missing format falls back to MJPEG, then to the first format
// that the runtime can decode. The nearest size has the smallest width and height difference, and the nearest rate
// has the smallest fps difference.
std::optional<Picked> pick_mode(const Info &info, const Mode &want);

// Returns the frame rates of a frame size for the dashboard, highest first, rounded to whole fps.
std::vector<int> frame_rates(const FrameSize &f);

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
// Writes the hint, the format, the frame and the interval of p into a probe block, and keeps its other fields.
void apply_probe(uint8_t *block, size_t size, const Probe &p);
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

// Converts one frame to 8-bit grayscale in out, and sets the size of the image. An uncompressed frame must have the
// exact size of a width x height frame. Returns false if the frame does not decode or does not fit in capacity.
bool to_gray(FormatKind kind, const uint8_t *data, size_t size, uint16_t width, uint16_t height, uint8_t *out,
             size_t capacity, int *out_width, int *out_height);

}
