#pragma once

// An MJPEG UVC camera streamed from user space: usb_device.h for the I/O,
// uvc.h for the protocol. One thread calls open(), then read() in a loop.

#include "camera/usb/usb_device.h"
#include "camera/usb/uvc.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace babble::uvc {

class Camera {
public:
    ~Camera();
    // Opens the device, negotiates MJPEG at the frame size closest to
    // width x height and the given rate (0, or a rate it refuses: the
    // device's default), and starts streaming.
    bool open(const usb::Found &dev, uint16_t width, uint16_t height, double fps, std::string *error);
    // Waits up to timeout_ms; calls on_frame for every complete JPEG. False when
    // the camera is gone or failed (see the error).
    bool read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error);
    void close();

    const Info &info() const { return info_; }
    const Probe &negotiated() const { return probe_; }
    const FrameSize &frame_size() const { return frame_; }
    uint64_t dropped() const { return assembler_.dropped(); }

private:
    usb::Device dev_;
    Info info_;
    Probe probe_;
    FrameSize frame_;
    FrameAssembler assembler_;
    size_t payload_ = 16384;   // dwMaxPayloadTransferSize: where a bulk transfer splits into payloads
    std::unique_ptr<usb::BulkReader> reader_;
};

} // namespace babble::uvc
