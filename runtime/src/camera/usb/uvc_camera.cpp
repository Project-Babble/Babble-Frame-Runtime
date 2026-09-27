#include "camera/usb/uvc_camera.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace babble::uvc {

namespace {

// Class-specific requests to an interface.
constexpr uint8_t kSetCur = 0x01, kGetCur = 0x81;
constexpr uint8_t kToInterface = 0x21, kFromInterface = 0xa1;
constexpr uint16_t kProbeControl = 0x0100, kCommitControl = 0x0200;
constexpr unsigned kTimeoutMs = 1000;

} // namespace

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
        // Isochronous streaming needs per-packet transfers; the Babble tracker is bulk.
        if (error)
            *error = "isochronous UVC streaming is not supported";
        close();
        return false;
    }
    const FrameSize *f = pick_frame(info_, width, height);
    frame_ = *f;
    if (!dev_.claim(info_.control_iface, error) || !dev_.claim(info_.stream_iface, error)) {
        close();
        return false;
    }
    dev_.set_interface(info_.stream_iface, info_.stream_alt);

    // Probe with our choice, read back what the device accepts, then commit it.
    Probe want;
    want.format_index = info_.mjpeg_format;
    want.frame_index = frame_.index;
    want.frame_interval = fps > 0 ? uint32_t(1e7 / fps) : frame_.default_interval;
    auto block = encode_probe(want, info_.bcd_uvc);
    const uint16_t len = uint16_t(block.size());
    int r = dev_.control(kToInterface, kSetCur, kProbeControl, info_.stream_iface, block.data(), len, kTimeoutMs);
    if (r < 0 && want.frame_interval != frame_.default_interval) {
        // A rate the camera refuses: fall back to its default rate.
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

    // Payloads are at most dwMaxPayloadTransferSize long, and one shorter than
    // that ends with a short packet, which also ends the bulk transfer. So a
    // transfer holds whole payloads back to back: read big transfers (fewer
    // wakeups) and split them at the payload size (read()). The Babble tracker
    // sends 64-byte payloads, one per full-speed packet.
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
        // Stop the stream: a halt on a bulk streaming endpoint ends it (UVC 1.1, 2.4.3.2.2).
        if (info_.endpoint)
            dev_.clear_halt(info_.endpoint);
        dev_.close();
    }
    assembler_.reset();
}

} // namespace babble::uvc
