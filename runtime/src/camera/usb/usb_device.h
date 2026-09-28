#pragma once

// Direct USB access through the kernel's usbfs (/dev/bus/usb/BBB/DDD), for
// devices that have no kernel driver on the Frame (UVC cameras, CDC serial).
// The device nodes are group "input", which the steamos user is in, so no
// root, udev rule or libusb is needed.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct usbdevfs_urb;

namespace babble::usb {

struct Found {
    std::string sysfs;      // /sys/bus/usb/devices/1-1
    std::string node;       // /dev/bus/usb/001/003
    uint16_t vendor = 0, product = 0;
    std::string manufacturer, product_name, serial;
};

// USB devices with this vendor:product id (product 0 = any product).
std::vector<Found> find(uint16_t vendor, uint16_t product);

// Every connected USB video (UVC) camera: devices with a video streaming interface.
std::vector<Found> list_uvc();
// Every connected USB device (for diagnostics).
std::vector<Found> list_all();

// A stable name for a device across replugging: "vvvv:pppp:serial" (or
// "vvvv:pppp@port" for a device without a serial number).
std::string device_id(const Found &f);

// A human-readable name: "Manufacturer Product".
std::string device_label(const Found &f);

class Device {
public:
    ~Device();
    bool open(const Found &f, std::string *error);
    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // The active configuration descriptor (all of it), from sysfs.
    std::vector<uint8_t> config_descriptor() const;

    bool claim(uint8_t iface, std::string *error);
    void release(uint8_t iface);
    bool set_interface(uint8_t iface, uint8_t alt);
    bool clear_halt(uint8_t endpoint);
    // A control transfer; returns the bytes transferred, or -errno.
    int control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data,
                uint16_t length, unsigned timeout_ms);

private:
    int fd_ = -1;
    std::string sysfs_;
    std::vector<uint8_t> claimed_;
};

// Keeps several bulk IN transfers queued on one endpoint, so the device can
// always send (full-speed bulk leaves no time for a gap between reads).
class BulkReader {
public:
    BulkReader(Device &dev, uint8_t endpoint, size_t transfer_bytes, int count = 4);
    ~BulkReader();
    bool start(std::string *error);
    // Waits up to timeout_ms for completed transfers and hands each one's data
    // to on_data. False when the device is gone (unplugged) or failed.
    bool wait(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_data, std::string *error);
    void stop();

private:
    bool submit(usbdevfs_urb *urb);
    Device &dev_;
    uint8_t endpoint_;
    size_t bytes_;
    std::vector<std::unique_ptr<usbdevfs_urb>> urbs_;
    std::vector<std::vector<uint8_t>> buffers_;
    int in_flight_ = 0;
};

} // namespace babble::usb
