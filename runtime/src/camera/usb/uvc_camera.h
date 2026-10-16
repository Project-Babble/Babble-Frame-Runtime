#pragma once

// An MJPEG UVC camera from user space, and one thread must call open() and then read() in a loop.

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
    // Opens the device, sets MJPEG at the nearest frame size and rate, and starts the stream.
    bool open(const usb::Found &dev, uint16_t width, uint16_t height, double fps, std::string *error);
    // Calls on_frame for each complete JPEG and returns false if the camera is gone or failed.
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
    size_t payload_ = 16384;   // The dwMaxPayloadTransferSize, where a bulk transfer splits into payloads.
    std::unique_ptr<usb::BulkReader> reader_;
};

}
