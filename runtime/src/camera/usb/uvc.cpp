#include "camera/usb/uvc.h"

#include "camera/usb/jpeg.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace babble::uvc {

namespace {

constexpr uint8_t kInterface = 0x04, kEndpoint = 0x05, kClassInterface = 0x24;
constexpr uint8_t kVideoClass = 0x0e, kControlSub = 0x01, kStreamingSub = 0x02;
constexpr uint8_t kVcHeader = 0x01;
constexpr uint8_t kVsFormatUncompressed = 0x04, kVsFrameUncompressed = 0x05, kVsFormatMjpeg = 0x06,
                  kVsFrameMjpeg = 0x07;

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

// The first four bytes of the GUID of an uncompressed format are its FourCC.
FormatKind kind_of_guid(const uint8_t *guid) {
    const std::string cc(reinterpret_cast<const char *>(guid), 4);
    if (cc == "YUY2" || cc == "YUYV")
        return FormatKind::Yuy2;
    if (cc == "UYVY")
        return FormatKind::Uyvy;
    if (cc == "NV12")
        return FormatKind::Nv12;
    if (cc == "Y800" || cc == "GREY" || cc == std::string("Y8  ", 4))
        return FormatKind::Grey;
    return FormatKind::Unknown;
}

bool usable(const Format &f) { return f.kind != FormatKind::Unknown && !f.frames.empty(); }

}

const char *format_key(FormatKind kind) {
    switch (kind) {
    case FormatKind::Mjpeg: return "mjpeg";
    case FormatKind::Yuy2: return "yuy2";
    case FormatKind::Uyvy: return "uyvy";
    case FormatKind::Nv12: return "nv12";
    case FormatKind::Grey: return "grey";
    case FormatKind::Unknown: break;
    }
    return "unknown";
}

const char *format_label(FormatKind kind) {
    switch (kind) {
    case FormatKind::Mjpeg: return "MJPEG";
    case FormatKind::Yuy2: return "YUY2";
    case FormatKind::Uyvy: return "UYVY";
    case FormatKind::Nv12: return "NV12";
    case FormatKind::Grey: return "Grey";
    case FormatKind::Unknown: break;
    }
    return "Unknown";
}

FormatKind format_from_key(const std::string &key) {
    for (FormatKind k : {FormatKind::Mjpeg, FormatKind::Yuy2, FormatKind::Uyvy, FormatKind::Nv12, FormatKind::Grey})
        if (key == format_key(k))
            return k;
    return FormatKind::Unknown;
}

std::optional<Info> parse_config(const uint8_t *data, size_t size, std::string *error) {
    Info info;
    // The interface of the descriptors that follow.
    uint8_t cur_iface = 0xff, cur_alt = 0, cur_class = 0, cur_sub = 0;
    Format *cur_format = nullptr;   // The format that the frame descriptors that follow belong to.
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
            cur_format = nullptr;
            if (cur_class == kVideoClass && cur_sub == kControlSub && info.control_iface == 0xff)
                info.control_iface = cur_iface;
            if (cur_class == kVideoClass && cur_sub == kStreamingSub && info.stream_iface == 0xff)
                info.stream_iface = cur_iface;
        } else if (type == kEndpoint && len >= 7 && cur_class == kVideoClass && cur_sub == kStreamingSub &&
                   cur_iface == info.stream_iface && (d[2] & 0x80)) {
            const uint16_t w = u16(d + 4);
            if ((d[3] & 0x03) == 0x02) {   // A bulk endpoint streams on alt 0, and the runtime prefers it.
                info.bulk = true;
                info.endpoint = d[2];
                info.endpoint_max_packet = w & 0x7ff;
            } else if ((d[3] & 0x03) == 0x01) {
                // Bits 11 and 12 are the extra transactions in each microframe of a high-bandwidth endpoint.
                info.alts.push_back({cur_alt, d[2], uint32_t(w & 0x7ff) * (1 + ((w >> 11) & 3))});
            }
        } else if (type == kClassInterface && len >= 3 && cur_class == kVideoClass) {
            const uint8_t sub = d[2];
            if (cur_sub == kControlSub && sub == kVcHeader && len >= 5) {
                info.bcd_uvc = u16(d + 3);
            } else if (cur_sub == kStreamingSub && (sub == kVsFormatMjpeg || sub == kVsFormatUncompressed) && len >= 5) {
                Format f;
                f.index = d[3];
                if (sub == kVsFormatMjpeg) {
                    f.kind = FormatKind::Mjpeg;
                } else if (len >= 22) {
                    f.kind = kind_of_guid(d + 5);
                    f.bits_per_pixel = d[21];
                }
                info.formats.push_back(f);
                cur_format = &info.formats.back();
            } else if (cur_sub == kStreamingSub && (sub == kVsFrameMjpeg || sub == kVsFrameUncompressed) && len >= 26 &&
                       cur_format) {
                FrameSize f;
                f.index = d[3];
                f.width = u16(d + 5);
                f.height = u16(d + 7);
                f.max_frame_bytes = u32(d + 17);
                f.default_interval = u32(d + 21);
                const uint8_t n = d[25];
                if (n == 0 && len >= 38) {   // A continuous range has the min, the max and the step.
                    f.continuous = true;
                    f.intervals = {u32(d + 26), u32(d + 30)};
                } else {
                    for (size_t k = 0; k < n && 26 + 4 * (k + 1) <= len; k++)
                        f.intervals.push_back(u32(d + 26 + 4 * k));
                }
                cur_format->frames.push_back(f);
            } else if (cur_sub == kStreamingSub && sub != kVsFrameMjpeg && sub != kVsFrameUncompressed) {
                cur_format = nullptr;   // The frame descriptors of another format type follow.
            }
        }
        i += len;
    }
    std::sort(info.alts.begin(), info.alts.end(), [](const Alt &a, const Alt &b) { return a.bytes < b.bytes; });
    if (!info.bulk && !info.alts.empty()) {
        info.endpoint = info.alts.back().endpoint;
        info.endpoint_max_packet = uint16_t(std::min<uint32_t>(info.alts.back().bytes, 0xffff));
    }
    if (info.stream_iface == 0xff || !info.endpoint) {
        if (error)
            *error = "no video streaming interface";
        return std::nullopt;
    }
    if (std::none_of(info.formats.begin(), info.formats.end(), usable)) {
        if (error)
            *error = "no MJPEG, YUY2, UYVY, NV12 or grey format";
        return std::nullopt;
    }
    return info;
}

std::optional<Picked> pick_mode(const Info &info, const Mode &want) {
    const Format *fmt = nullptr;
    for (FormatKind k : {want.format, FormatKind::Mjpeg})
        for (const Format &f : info.formats)
            if (!fmt && f.kind == k && usable(f))
                fmt = &f;
    for (const Format &f : info.formats)
        if (!fmt && usable(f))
            fmt = &f;
    if (!fmt)
        return std::nullopt;
    const FrameSize *frame = &fmt->frames.front();
    if (want.width && want.height) {
        int best = -1;
        for (const FrameSize &f : fmt->frames) {
            const int diff = std::abs(int(f.width) - int(want.width)) + std::abs(int(f.height) - int(want.height));
            if (best < 0 || diff < best) {
                best = diff;
                frame = &f;
            }
        }
    }
    uint32_t interval = frame->default_interval ? frame->default_interval
                        : frame->intervals.empty()  ? 333333
                                                    : frame->intervals.front();
    if (want.fps > 0 && !frame->intervals.empty()) {
        const uint32_t target = uint32_t(std::lround(1e7 / want.fps));
        if (frame->continuous) {
            interval = std::clamp(target, frame->intervals[0], std::max(frame->intervals[0], frame->intervals[1]));
        } else {
            double best = -1;
            for (uint32_t i : frame->intervals) {
                const double diff = i ? std::fabs(1e7 / i - want.fps) : 1e9;
                if (best < 0 || diff < best) {
                    best = diff;
                    interval = i;
                }
            }
        }
    }
    return Picked{fmt, frame, interval};
}

std::vector<int> frame_rates(const FrameSize &f) {
    std::vector<int> fps;
    auto add = [&](uint32_t interval) {
        if (interval)
            fps.push_back(int(std::lround(1e7 / interval)));
    };
    if (f.continuous && f.intervals.size() == 2) {
        const double lo = f.intervals[1] ? 1e7 / f.intervals[1] : 0, hi = f.intervals[0] ? 1e7 / f.intervals[0] : 0;
        for (int v : {120, 90, 60, 50, 30, 25, 20, 15, 10, 5})
            if (v >= lo - 0.5 && v <= hi + 0.5)
                fps.push_back(v);
    } else {
        for (uint32_t i : f.intervals)
            add(i);
    }
    if (fps.empty())
        add(f.default_interval);
    std::sort(fps.begin(), fps.end(), std::greater<>());
    fps.erase(std::unique(fps.begin(), fps.end()), fps.end());
    return fps;
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

void apply_probe(uint8_t *block, size_t size, const Probe &p) {
    if (size < 8)
        return;
    put16(block, p.hint);
    block[2] = p.format_index;
    block[3] = p.frame_index;
    put32(block + 4, p.frame_interval);
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

bool to_gray(FormatKind kind, const uint8_t *data, size_t size, uint16_t width, uint16_t height, uint8_t *out,
             size_t capacity, int *out_width, int *out_height) {
    if (kind == FormatKind::Mjpeg)
        return looks_like_jpeg(data, size) && jpeg::decode_gray(data, size, out, capacity, out_width, out_height);
    const size_t px = size_t(width) * height;
    if (!px || px > capacity)
        return false;
    switch (kind) {
    case FormatKind::Yuy2:   // Y0 U Y1 V
    case FormatKind::Uyvy:   // U Y0 V Y1
        if (size != px * 2)
            return false;
        for (size_t i = 0, o = kind == FormatKind::Yuy2 ? 0 : 1; i < px; i++)
            out[i] = data[2 * i + o];
        break;
    case FormatKind::Nv12:   // The Y plane comes first, and the UV plane follows.
        if (size != px * 3 / 2)
            return false;
        memcpy(out, data, px);
        break;
    case FormatKind::Grey:
        if (size != px)
            return false;
        memcpy(out, data, px);
        break;
    default: return false;
    }
    *out_width = width;
    *out_height = height;
    return true;
}

}
