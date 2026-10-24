#pragma once

// A UVC camera from user space, over bulk or isochronous transfers. One thread must call open() and then read() in a
// loop.

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
    // Opens the device, sets the nearest mode that the camera offers, and starts the stream.
    bool open(const usb::Found &dev, const Mode &want, std::string *error);
    // Calls on_frame for each complete frame in format() and returns false if the camera is gone or failed.
    bool read(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_frame, std::string *error);
    void close();

    const Info &info() const { return info_; }
    const Probe &negotiated() const { return probe_; }
    const FrameSize &frame_size() const { return frame_; }
    FormatKind format() const { return format_; }
    bool iso() const { return iso_alt_ != 0; }
    uint8_t iso_alt() const { return iso_alt_; }   // The isochronous alternate setting, or 0 for bulk.
    uint64_t dropped() const { return assembler_.dropped(); }

private:
    usb::Device dev_;
    Info info_;
    Probe probe_;
    FrameSize frame_;
    FormatKind format_ = FormatKind::Mjpeg;
    uint8_t iso_alt_ = 0;
    FrameAssembler assembler_;
    size_t payload_ = 16384;   // The dwMaxPayloadTransferSize, where a bulk transfer splits into payloads.
    std::unique_ptr<usb::StreamReader> reader_;
};

}
