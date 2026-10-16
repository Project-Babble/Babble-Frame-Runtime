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
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81;
constexpr uint8_t kToInterface = 0x21, kFromInterface = 0xa1;
constexpr uint16_t kProbeControl = 0x0100, kCommitControl = 0x0200;
constexpr unsigned kTimeoutMs = 1000;

}

Camera::~Camera() { close(); }

bool Camera::open(const usb::Found &found, uint16_t width, uint16_t height, double fps, std::string *error) {
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
    if (!info_.bulk) {
        // Isochronous streaming is not supported because the Babble tracker uses bulk transfers.
        if (error)
            *error = "isochronous UVC streaming is not supported";
        close();
        return false;
    }
    const FrameSize *f = pick_frame(info_, width, height);
    frame_ = *f;
    if (LOG.debug_enabled()) {
        std::string sizes;
        for (const auto &fs : info_.mjpeg_frames)
            sizes += (sizes.empty() ? "" : ", ") + std::to_string(fs.index) + ":" + std::to_string(fs.width) + "x" +
                     std::to_string(fs.height) + "@" + std::to_string(fs.default_interval ? 10000000 / fs.default_interval : 0);
        LOG.debug("%s: UVC %x.%02x, control if %u, stream if %u alt %u, endpoint 0x%02x %s (%u B), MJPEG format %u "
                  "frames [%s]; picked %u (%ux%u)",
                  found.node.c_str(), info_.bcd_uvc >> 8, info_.bcd_uvc & 0xff, info_.control_iface,
                  info_.stream_iface, info_.stream_alt, info_.endpoint, info_.bulk ? "bulk" : "iso",
                  info_.endpoint_max_packet, info_.mjpeg_format, sizes.c_str(), frame_.index, frame_.width,
                  frame_.height);
    }
    if (!dev_.claim(info_.control_iface, error) || !dev_.claim(info_.stream_iface, error)) {
        close();
        return false;
    }
    dev_.set_interface(info_.stream_iface, info_.stream_alt);

    // Send the probe, read the values that the device accepts, and then commit them.
    Probe want;
    want.format_index = info_.mjpeg_format;
    want.frame_index = frame_.index;
    want.frame_interval = fps > 0 ? uint32_t(1e7 / fps) : frame_.default_interval;
    auto block = encode_probe(want, info_.bcd_uvc);
    const uint16_t len = uint16_t(block.size());
    int r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0 && want.frame_interval != frame_.default_interval) {
        // If the camera refuses the rate, use its default rate.
        LOG.info("%s: the camera refused %.1f fps (%s); using its default %.1f fps", found.node.c_str(),
                 1e7 / want.frame_interval, strerror(-r), 1e7 / frame_.default_interval);
        want.frame_interval = frame_.default_interval;
        block = encode_probe(want, info_.bcd_uvc);
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
    block = encode_probe(probe_, info_.bcd_uvc);
    r = dev_.control(kToInterface, kSetCur, kCommitControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0) {
        if (error)
            *error = std::string("UVC commit: ") + strerror(-r);
        close();
        return false;
    }

    // Read large transfers for fewer wakeups, because each transfer holds only complete payloads.
    payload_ = probe_.max_payload_transfer_size ? probe_.max_payload_transfer_size : 16384;
    size_t transfer = std::max<size_t>(payload_, 16384 / payload_ * payload_);
    assembler_ = FrameAssembler(std::max<size_t>(probe_.max_video_frame_size, 64 * 1024) * 2);
    reader_ = std::make_unique<usb::BulkReader>(dev_, info_.endpoint, transfer, 4);
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
        // A halt on a bulk streaming endpoint stops the stream (UVC 1.1, 2.4.3.2.2).
        if (info_.endpoint)
            dev_.clear_halt(info_.endpoint);
        dev_.close();
    }
    assembler_.reset();
}

}
