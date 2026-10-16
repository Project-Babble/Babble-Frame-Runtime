#include "camera/usb/uvc.h"

#include <algorithm>

namespace babble::uvc {

namespace {

constexpr uint8_t kInterface = 0x04, kEndpoint = 0x05, kClassInterface = 0x24;
constexpr uint8_t kVideoClass = 0x0e, kControlSub = 0x01, kStreamingSub = 0x02;
constexpr uint8_t kVcHeader = 0x01, kVsFormatMjpeg = 0x06, kVsFrameMjpeg = 0x07;

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void put16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = uint8_t(v >> (8 * i));
}

}

std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error) {
    Info info;
    // The interface of the descriptors that follow.
    uint8_t cur_iface = 0xff, cur_alt = 0, cur_class = 0, cur_sub = 0;
    uint8_t cur_mjpeg = 0;   // The format index inside the frame descriptors of an MJPEG format.
    for (size_t i = 0; i + 2 <= size;) {
        const uint8_t len = data[i], type = data[i + 1];
        if (len < 2 || i + len > size)
            break;
        const uint8_t *d = data + i;
        if (type == kInterface && len >= 9) {
            cur_iface = d[2];
            cur_alt = d[3];
            cur_class = d[5];
            cur_sub = d[6];
            cur_mjpeg = 0;
            if (cur_class == kVideoClass && cur_sub == kControlSub && info.control_iface == 0xff)
                info.control_iface = cur_iface;
            if (cur_class == kVideoClass && cur_sub == kStreamingSub && info.stream_iface == 0xff)
                info.stream_iface = cur_iface;
        } else if (type == kEndpoint && len >= 7 && cur_class == kVideoClass && cur_sub == kStreamingSub &&
                   cur_iface == info.stream_iface && (d[2] & 0x80)) {
            // Use bulk if possible, or else the isochronous endpoint with the largest packet.
            const bool bulk = (d[3] & 0x03) == 0x02;
            const uint16_t max_packet = u16(d + 4) & 0x7ff;
            if (!info.endpoint || (bulk && !info.bulk) || (!bulk && !info.bulk && max_packet > info.endpoint_max_packet)) {
                info.endpoint = d[2];
                info.endpoint_max_packet = max_packet;
                info.bulk = bulk;
                info.stream_alt = cur_alt;
            }
        } else if (type == kClassInterface && len >= 3 && cur_class == kVideoClass) {
            const uint8_t sub = d[2];
            if (cur_sub == kControlSub && sub == kVcHeader && len >= 5) {
                info.bcd_uvc = u16(d + 3);
            } else if (cur_sub == kStreamingSub && sub == kVsFormatMjpeg && len >= 5) {
                cur_mjpeg = d[3];
                if (!info.mjpeg_format)
                    info.mjpeg_format = cur_mjpeg;
            } else if (cur_sub == kStreamingSub && sub == kVsFrameMjpeg && len >= 26 && cur_mjpeg &&
                       cur_mjpeg == info.mjpeg_format) {
                FrameSize f;
                f.index = d[3];
                f.width = u16(d + 5);
                f.height = u16(d + 7);
                f.max_frame_bytes = u32(d + 17);
                f.default_interval = u32(d + 21);
                info.mjpeg_frames.push_back(f);
            } else if (cur_sub == kStreamingSub && sub != kVsFrameMjpeg) {
                if (sub != kVsFormatMjpeg)
                    cur_mjpeg = 0;   // These descriptors are for a different format.
            }
        }
        i += len;
    }
    if (info.stream_iface == 0xff || !info.endpoint) {
        if (error)
            *error = "no video streaming interface";
        return std::nullopt;
    }
    if (!info.mjpeg_format || info.mjpeg_frames.empty()) {
        if (error)
            *error = "no MJPEG format";
        return std::nullopt;
    }
    return info;
}

const FrameSize *pick_frame(const Info &info, uint16_t width, uint16_t height) {
    for (const auto &f : info.mjpeg_frames)
        if (f.width == width && f.height == height)
            return &f;
    return info.mjpeg_frames.empty() ? nullptr : &info.mjpeg_frames.front();
}

size_t probe_length(uint16_t bcd_uvc) {
    if (bcd_uvc >= 0x0150)
        return 48;
    if (bcd_uvc >= 0x0110)
        return 34;
    return 26;
}

std::vector<uint8_t> encode_probe(const Probe &p, uint16_t bcd_uvc) {
    std::vector<uint8_t> b(probe_length(bcd_uvc), 0);
    put16(&b[0], p.hint);
    b[2] = p.format_index;
    b[3] = p.frame_index;
    put32(&b[4], p.frame_interval);
    put32(&b[18], p.max_video_frame_size);
    put32(&b[22], p.max_payload_transfer_size);
    return b;
}

std::optional<Probe> decode_probe(const uint8_t *data, size_t size) {
    if (size < 26)
        return std::nullopt;
    Probe p;
    p.hint = u16(data);
    p.format_index = data[2];
    p.frame_index = data[3];
    p.frame_interval = u32(data + 4);
    p.max_video_frame_size = u32(data + 18);
    p.max_payload_transfer_size = u32(data + 22);
    return p;
}

void FrameAssembler::reset() {
    building_.clear();
    fid_ = -1;
    bad_ = false;
}

bool FrameAssembler::finish() {
    const bool ok = !bad_ && !building_.empty();
    if (ok)
        done_.swap(building_);
    else if (!building_.empty() || bad_)
        dropped_++;
    building_.clear();
    bad_ = false;
    return ok;
}

bool FrameAssembler::push(const uint8_t *payload, size_t size) {
    if (size < 2)
        return false;
    const uint8_t header_len = payload[0], info = payload[1];
    if (header_len < 2 || header_len > size) {
        bad_ = true;   // A corrupt header makes the full frame bad.
        return false;
    }
    const int fid = info & 0x01;
    const bool eof = info & 0x02, err = info & 0x40;
    bool completed = false;
    // A changed frame ID starts a new frame, because some devices do not send EOF.
    if (fid_ >= 0 && fid != fid_ && (!building_.empty() || bad_))
        completed = finish();
    fid_ = fid;
    if (err)
        bad_ = true;
    const size_t data_len = size - header_len;
    if (building_.size() + data_len > max_bytes_) {
        bad_ = true;
        building_.clear();
    } else {
        building_.insert(building_.end(), payload + header_len, payload + size);
    }
    if (eof) {
        completed = finish() || completed;
        fid_ = -1;   // After EOF, the next payload starts a frame with any FID.
    }
    return completed;
}

bool looks_like_jpeg(const uint8_t *data, size_t size) {
    return size >= 4 && data[0] == 0xff && data[1] == 0xd8 && data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

}
