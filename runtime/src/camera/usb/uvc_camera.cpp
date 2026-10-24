#include "camera/usb/uvc_camera.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace {
const babble::log::Logger LOG("usb");
}

namespace babble::uvc {

namespace {

// The class-specific requests to an interface.
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81, kGetDef = 0x87;
constexpr uint8_t kToInterface = 0x21, kFromInterface = 0xa1;
constexpr uint16_t kProbeControl = 0x0100, kCommitControl = 0x0200;
constexpr unsigned kTimeoutMs = 1000;

}

Camera::~Camera() { close(); }

bool Camera::open(const usb::Found &found, const Mode &mode, std::string *error) {
    close();
    if (!dev_.open(found, error))
        return false;
    const auto config = dev_.config_descriptor();
    auto info = parse_config(config.data(), config.size(), error);
    if (!info) {
        close();
        return false;
    }
    info_ = *info;
    if (!info_.bulk && info_.alts.empty()) {
        if (error)
            *error = "no bulk or isochronous streaming endpoint";
        close();
        return false;
    }
    const auto picked = pick_mode(info_, mode);
    if (!picked) {
        if (error)
            *error = "no format to stream";
        close();
        return false;
    }
    frame_ = *picked->frame;
    format_ = picked->format->kind;
    if (LOG.debug_enabled()) {
        std::string formats;
        for (const auto &fm : info_.formats) {
            formats += (formats.empty() ? "" : "; ") + std::string(format_label(fm.kind)) + " " +
                       std::to_string(fm.index) + ":";
            for (const auto &fs : fm.frames)
                formats += " " + std::to_string(fs.width) + "x" + std::to_string(fs.height);
        }
        LOG.debug("%s: UVC %x.%02x, control if %u, stream if %u, endpoint 0x%02x %s (%u B, %zu iso alts), formats "
                  "[%s]; picked %s %ux%u",
                  found.node.c_str(), info_.bcd_uvc >> 8, info_.bcd_uvc & 0xff, info_.control_iface,
                  info_.stream_iface, info_.endpoint, info_.bulk ? "bulk" : "iso", info_.endpoint_max_packet,
                  info_.alts.size(), formats.c_str(), format_label(format_), frame_.width, frame_.height);
    }
    if (!dev_.claim(info_.control_iface, error) || !dev_.claim(info_.stream_iface, error)) {
        close();
        return false;
    }
    // The probe and the commit happen on alt 0, which has no isochronous bandwidth.
    dev_.set_interface(info_.stream_iface, 0);

    // Send the probe, read the values that the device accepts, and then commit them.
    Probe want;
    want.format_index = picked->format->index;
    want.frame_index = frame_.index;
    want.frame_interval = picked->interval;
    // Start from the probe values of the camera and change only our fields, as the Linux uvcvideo driver does. Some
    // cameras stall a probe that has their other fields at 0.
    std::vector<uint8_t> base(probe_length(info_.bcd_uvc), 0);
    const uint16_t len = uint16_t(base.size());
    int r = dev_.control(kFromInterface, kGetCur, kProbeControl, info_.stream_iface, base.data(), len, kTimeoutMs);
    if (r < int(len))
        r = dev_.control(kFromInterface, kGetDef, kProbeControl, info_.stream_iface, base.data(), len, kTimeoutMs);
    if (r < int(len)) {
        LOG.debug("%s: the camera gave no probe values (%s); sending ours", found.node.c_str(),
                  r < 0 ? strerror(-r) : "short read");
        base = encode_probe(want, info_.bcd_uvc);
    }
    auto block = base;
    apply_probe(block.data(), block.size(), want);
    r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0 && want.frame_interval != frame_.default_interval) {
        // If the camera refuses the rate, use its default rate.
        LOG.info("%s: the camera refused %.1f fps (%s); using its default %.1f fps", found.node.c_str(),
                 1e7 / want.frame_interval, strerror(-r), 1e7 / frame_.default_interval);
        want.frame_interval = frame_.default_interval;
        block = base;
        apply_probe(block.data(), block.size(), want);
        r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    }
    if (r < 0) {
        if (error)
            *error = std::string("UVC probe: ") + strerror(-r);
        close();
        return false;
    }
    std::vector<uint8_t> got(len);
    r = dev_.control(kFromInterface, kGetCur, kProbeControl, info_.stream_iface, got.data(), len, kTimeoutMs);
    auto accepted = r > 0 ? decode_probe(got.data(), size_t(r)) : std::nullopt;
    probe_ = accepted ? *accepted : want;
    LOG.debug("%s: probe asked format %u frame %u interval %u; camera answered %s: interval %u, max frame %u B, "
              "max payload %u B",
              found.node.c_str(), want.format_index, want.frame_index, want.frame_interval,
              accepted ? "" : "nothing (using ours)", probe_.frame_interval, probe_.max_video_frame_size,
              probe_.max_payload_transfer_size);
    if (!probe_.max_video_frame_size)
        probe_.max_video_frame_size = frame_.max_frame_bytes;
    // Commit the block that the camera answered, so that its own fields stay as it set them.
    if (accepted)
        block = got;
    r = dev_.control(kToInterface, kSetCur, kCommitControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0) {
        if (error)
            *error = std::string("UVC commit: ") + strerror(-r);
        close();
        return false;
    }

    // An uncompressed frame has a fixed size, which some cameras do not report in the probe.
    const size_t raw_bytes = format_ == FormatKind::Mjpeg
                                 ? 0
                                 : size_t(frame_.width) * frame_.height * std::max<uint8_t>(picked->format->bits_per_pixel, 8) / 8;
    assembler_ = FrameAssembler(std::max<size_t>({probe_.max_video_frame_size, raw_bytes, 64 * 1024}) * 2);
    payload_ = probe_.max_payload_transfer_size ? probe_.max_payload_transfer_size : 16384;
    if (info_.bulk) {
        // Read large transfers for fewer wakeups, because each transfer holds only complete payloads.
        iso_alt_ = 0;
        const size_t transfer = std::max<size_t>(payload_, 16384 / payload_ * payload_);
        reader_ = std::make_unique<usb::BulkReader>(dev_, info_.endpoint, transfer, 4);
    } else {
        // Use the smallest alt that carries one payload in each (micro)frame, or else the largest alt.
        const Alt *alt = &info_.alts.back();
        for (const Alt &a : info_.alts)
            if (a.bytes >= payload_) {
                alt = &a;
                break;
            }
        if (!dev_.set_interface(info_.stream_iface, alt->alt)) {
            if (error)
                *error = "cannot select isochronous alt " + std::to_string(alt->alt) + ": " + strerror(errno);
            close();
            return false;
        }
        iso_alt_ = alt->alt;
        LOG.debug("%s: isochronous alt %u, endpoint 0x%02x, %u B packets for %u B payloads", found.node.c_str(),
                  alt->alt, alt->endpoint, alt->bytes, unsigned(payload_));
        reader_ = std::make_unique<usb::IsoReader>(dev_, alt->endpoint, alt->bytes, 32, 4);
    }
    if (!reader_->start(error)) {
        close();
        return false;
    }
    return true;
}

bool Camera::read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error) {
    if (!reader_)
        return false;
    return reader_->wait(
        timeout_ms,
        [&](const uint8_t *data, size_t size) {
            if (iso_alt_) {   // Each isochronous packet is one payload.
                if (assembler_.push(data, size)) {
                    auto &frame = assembler_.frame();
                    on_frame(frame.data(), frame.size());
                }
                return;
            }
            for (size_t at = 0; at < size; at += payload_) {
                if (assembler_.push(data + at, std::min(payload_, size - at))) {
                    auto &frame = assembler_.frame();
                    on_frame(frame.data(), frame.size());
                }
            }
        },
        error);
}

void Camera::close() {
    if (reader_) {
        reader_->stop();
        reader_.reset();
    }
    if (dev_.is_open()) {
        if (iso_alt_) {
            dev_.set_interface(info_.stream_iface, 0);   // Alt 0 stops an isochronous stream.
        } else if (info_.endpoint) {
            dev_.clear_halt(info_.endpoint);   // A halt on a bulk streaming endpoint stops the stream (UVC 1.1, 2.4.3.2.2).
        }
        dev_.close();
    }
    iso_alt_ = 0;
    assembler_.reset();
}

}
