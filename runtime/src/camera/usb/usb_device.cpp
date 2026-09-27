#include "camera/usb/usb_device.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace babble::usb {

namespace {

std::string read_line(const std::filesystem::path &p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

// Every USB device in sysfs, optionally filtered.
std::vector<Found> scan(const std::function<bool(const std::filesystem::path &, const Found &)> &want) {
    namespace fs = std::filesystem;
    std::vector<Found> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator("/sys/bus/usb/devices", ec)) {
        const auto vid = read_line(e.path() / "idVendor"), pid = read_line(e.path() / "idProduct");
        if (vid.empty() || pid.empty())
            continue;   // an interface, not a device
        Found f;
        f.vendor = uint16_t(std::stoul(vid, nullptr, 16));
        f.product = uint16_t(std::stoul(pid, nullptr, 16));
        const auto bus = read_line(e.path() / "busnum"), dev = read_line(e.path() / "devnum");
        if (bus.empty() || dev.empty())
            continue;
        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", std::stoi(bus), std::stoi(dev));
        f.sysfs = e.path().string();
        f.node = node;
        f.manufacturer = read_line(e.path() / "manufacturer");
        f.product_name = read_line(e.path() / "product");
        f.serial = read_line(e.path() / "serial");
        if (want(e.path(), f))
            out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Found &a, const Found &b) { return a.sysfs < b.sysfs; });
    return out;
}

} // namespace

std::vector<Found> find(uint16_t vendor, uint16_t product) {
    return scan([&](const std::filesystem::path &, const Found &f) {
        return f.vendor == vendor && (!product || f.product == product);
    });
}

std::vector<Found> list_uvc() {
    namespace fs = std::filesystem;
    return scan([](const fs::path &dev, const Found &) {
        // Interfaces are siblings named "<device>:<config>.<interface>".
        const std::string prefix = dev.filename().string() + ":";
        std::error_code ec;
        for (const auto &i : fs::directory_iterator(dev, ec)) {
            const std::string name = i.path().filename().string();
            if (name.rfind(prefix, 0) == 0 && read_line(i.path() / "bInterfaceClass") == "0e" &&
                read_line(i.path() / "bInterfaceSubClass") == "02")
                return true;   // a video streaming interface
        }
        return false;
    });
}

std::string device_id(const Found &f) {
    char vp[16];
    snprintf(vp, sizeof(vp), "%04x:%04x", f.vendor, f.product);
    if (!f.serial.empty())
        return std::string(vp) + ":" + f.serial;
    return std::string(vp) + "@" + std::filesystem::path(f.sysfs).filename().string();
}

std::string device_label(const Found &f) {
    std::string s = f.manufacturer;
    if (!f.product_name.empty())
        s += (s.empty() ? "" : " ") + f.product_name;
    if (s.empty()) {
        char vp[16];
        snprintf(vp, sizeof(vp), "%04x:%04x", f.vendor, f.product);
        s = std::string("USB camera ") + vp;
    }
    return s;
}


Device::~Device() { close(); }

bool Device::open(const Found &f, std::string *error) {
    close();
    fd_ = ::open(f.node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (error)
            *error = f.node + ": " + strerror(errno);
        return false;
    }
    sysfs_ = f.sysfs;
    return true;
}

void Device::close() {
    if (fd_ < 0)
        return;
    for (uint8_t iface : claimed_) {
        unsigned int i = iface;
        ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &i);
    }
    claimed_.clear();
    ::close(fd_);
    fd_ = -1;
}

std::vector<uint8_t> Device::config_descriptor() const {
    // sysfs "descriptors": the 18-byte device descriptor, then the configuration(s).
    std::ifstream in(sysfs_ + "/descriptors", std::ios::binary);
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (all.size() <= 18)
        return {};
    return std::vector<uint8_t>(all.begin() + 18, all.end());
}

bool Device::claim(uint8_t iface, std::string *error) {
    unsigned int i = iface;
    if (ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &i) != 0) {
        if (error)
            *error = "claim interface " + std::to_string(iface) + ": " + strerror(errno);
        return false;
    }
    claimed_.push_back(iface);
    return true;
}

void Device::release(uint8_t iface) {
    unsigned int i = iface;
    ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &i);
    std::erase(claimed_, iface);
}

bool Device::set_interface(uint8_t iface, uint8_t alt) {
    usbdevfs_setinterface s{iface, alt};
    return ioctl(fd_, USBDEVFS_SETINTERFACE, &s) == 0;
}

bool Device::clear_halt(uint8_t endpoint) {
    unsigned int ep = endpoint;
    return ioctl(fd_, USBDEVFS_CLEAR_HALT, &ep) == 0;
}

int Device::control(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data,
                    uint16_t length, unsigned timeout_ms) {
    usbdevfs_ctrltransfer t{};
    t.bRequestType = request_type;
    t.bRequest = request;
    t.wValue = value;
    t.wIndex = index;
    t.wLength = length;
    t.timeout = timeout_ms;
    t.data = data;
    const int r = ioctl(fd_, USBDEVFS_CONTROL, &t);
    return r < 0 ? -errno : r;
}

BulkReader::BulkReader(Device &dev, uint8_t endpoint, size_t transfer_bytes, int count)
    : dev_(dev), endpoint_(endpoint), bytes_(transfer_bytes) {
    for (int i = 0; i < count; i++) {
        urbs_.push_back(std::make_unique<usbdevfs_urb>());
        buffers_.emplace_back(transfer_bytes);
    }
}

BulkReader::~BulkReader() { stop(); }

bool BulkReader::submit(usbdevfs_urb *urb) {
    const size_t i = size_t(reinterpret_cast<uintptr_t>(urb->usercontext));
    *urb = {};
    urb->type = USBDEVFS_URB_TYPE_BULK;
    urb->endpoint = endpoint_;
    urb->buffer = buffers_[i].data();
    urb->buffer_length = int(bytes_);
    urb->usercontext = reinterpret_cast<void *>(i);
    if (ioctl(dev_.fd(), USBDEVFS_SUBMITURB, urb) != 0)
        return false;
    in_flight_++;
    return true;
}

bool BulkReader::start(std::string *error) {
    for (size_t i = 0; i < urbs_.size(); i++) {
        urbs_[i]->usercontext = reinterpret_cast<void *>(i);
        if (!submit(urbs_[i].get())) {
            if (error)
                *error = std::string("submit bulk transfer: ") + strerror(errno);
            stop();
            return false;
        }
    }
    return true;
}

bool BulkReader::wait(int timeout_ms, const std::function<void(const uint8_t *, size_t)> &on_data,
                      std::string *error) {
    // usbfs signals completed transfers as POLLOUT.
    pollfd p{dev_.fd(), POLLOUT, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno != EINTR) {
        if (error)
            *error = std::string("poll: ") + strerror(errno);
        return false;
    }
    if (p.revents & (POLLERR | POLLHUP)) {
        if (error)
            *error = "device disconnected";
        return false;
    }
    for (;;) {
        usbdevfs_urb *urb = nullptr;
        if (ioctl(dev_.fd(), USBDEVFS_REAPURBNDELAY, &urb) != 0) {
            if (errno == EAGAIN)
                return true;
            if (error)
                *error = errno == ENODEV ? std::string("device disconnected") : std::string("reap: ") + strerror(errno);
            return false;
        }
        in_flight_--;
        if (urb->status == -ENODEV || urb->status == -ESHUTDOWN) {
            if (error)
                *error = "device disconnected";
            return false;
        }
        if (urb->status == 0 && urb->actual_length > 0)
            on_data(static_cast<const uint8_t *>(urb->buffer), size_t(urb->actual_length));
        // -EPIPE: the endpoint stalled; clear it and keep reading.
        if (urb->status == -EPIPE)
            dev_.clear_halt(endpoint_);
        if (!submit(urb)) {
            if (error)
                *error = errno == ENODEV ? std::string("device disconnected")
                                         : std::string("resubmit bulk transfer: ") + strerror(errno);
            return false;
        }
    }
}

void BulkReader::stop() {
    if (!dev_.is_open()) {
        in_flight_ = 0;
        return;
    }
    for (auto &u : urbs_)
        ioctl(dev_.fd(), USBDEVFS_DISCARDURB, u.get());
    // Reap everything we submitted, so the kernel no longer writes into our buffers.
    for (int tries = 0; in_flight_ > 0 && tries < 50; tries++) {
        usbdevfs_urb *urb = nullptr;
        if (ioctl(dev_.fd(), USBDEVFS_REAPURBNDELAY, &urb) == 0) {
            in_flight_--;
            continue;
        }
        if (errno != EAGAIN)
            break;
        usleep(2000);
    }
    in_flight_ = 0;
}

} // namespace babble::usb
