#pragma once

// Direct USB access through usbfs for devices that have no kernel driver on the Frame.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct usbdevfs_urb;

namespace babble::usb {

struct Found {
    std::string sysfs;      // An example is /sys/bus/usb/devices/1-1.
    std::string node;       // An example is /dev/bus/usb/001/003.
    uint16_t vendor = 0, product = 0;
    std::string manufacturer, product_name, serial;
};

// Returns the USB devices with this vendor:product id, and product 0 matches all products.
std::vector<Found> find(uint16_t vendor, uint16_t product);

// Returns all connected devices that have a UVC video streaming interface.
std::vector<Found> list_uvc();
// Returns all connected USB devices for diagnostics.
std::vector<Found> list_all();

// Returns a name for the device that does not change when the user plugs it in again.
std::string device_id(const Found &f);

// Returns a human-readable name as "Manufacturer Product".
std::string device_label(const Found &f);

class Device {
public:
    ~Device();
    bool open(const Found &f, std::string *error);
    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // Returns the full active configuration descriptor from sysfs.
    std::vector<uint8_t> config_descriptor() const;

    bool claim(uint8_t iface, std::string *error);
    void release(uint8_t iface);
    bool set_interface(uint8_t iface, uint8_t alt);
    bool clear_halt(uint8_t endpoint);
    // Does a control transfer and returns the number of bytes, or -errno.
    int control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data,
                uint16_t length, unsigned timeout_ms);

private:
    int fd_ = -1;
    std::string sysfs_;
    std::vector<uint8_t> claimed_;
};

// Keeps several bulk IN transfers in the queue of one endpoint, so that the device can always send.
class BulkReader {
public:
    BulkReader(Device &dev, uint8_t endpoint, size_t transfer_bytes, int count = 4);
    ~BulkReader();
    bool start(std::string *error);
    // Sends the data of completed transfers to on_data, and returns false if the device is gone or failed.
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

}
