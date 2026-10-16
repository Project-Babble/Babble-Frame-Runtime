#include "camera/v4l2/v4l2_controls.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("v4l2");
}

namespace babble {

namespace {

int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

}

V4l2Controls::~V4l2Controls() { close(); }

std::string V4l2Controls::normalize_name(const char *raw) {
    std::string out;
    for (const char *p = raw; *p; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c))
            out += char(std::tolower(c));
        else if (!out.empty() && out.back() != '_')
            out += '_';
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out;
}

bool V4l2Controls::open(const std::string &node) {
    close();
    int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG.warn("controls: cannot open %s: %s", node.c_str(), strerror(errno));
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    fd_ = fd;
    node_ = node;
    controls_.clear();

    v4l2_query_ext_ctrl q{};
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (xioctl(fd_, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED) && q.type != V4L2_CTRL_TYPE_CTRL_CLASS) {
            Info info{q.id, q.type, {q.minimum, q.maximum, int64_t(q.step), q.default_value}};
            controls_.emplace(normalize_name(q.name), info);
        }
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    LOG.info("controls: %s has %zu controls", node.c_str(), controls_.size());
    return true;
}

void V4l2Controls::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    controls_.clear();
}

std::vector<std::string> V4l2Controls::names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto &[name, _] : controls_)
        out.push_back(name);
    return out;
}

const V4l2Controls::Info *V4l2Controls::find(std::string_view name) const {
    auto it = controls_.find(name);
    return it == controls_.end() ? nullptr : &it->second;
}

bool V4l2Controls::has(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return find(name) != nullptr;
}

std::optional<ControlRange> V4l2Controls::range(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Info *info = find(name);
    if (!info)
        return std::nullopt;
    return info->range;
}

std::optional<int64_t> V4l2Controls::get(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Info *info = find(name);
    if (!info || fd_ < 0)
        return std::nullopt;
    v4l2_ext_control c{};
    c.id = info->id;
    v4l2_ext_controls cs{};
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    if (xioctl(fd_, VIDIOC_G_EXT_CTRLS, &cs) != 0)
        return std::nullopt;
    return info->type == V4L2_CTRL_TYPE_INTEGER64 ? c.value64 : int64_t(c.value);
}

bool V4l2Controls::set(std::string_view name, int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Info *info = find(name);
    if (!info || fd_ < 0) {
        LOG.warn("controls: %s has no control '%.*s'", node_.c_str(), int(name.size()), name.data());
        return false;
    }
    int64_t v = std::clamp(value, info->range.min, info->range.max);
    if (v != value)
        LOG.warn("controls: %.*s=%lld clamped to %lld", int(name.size()), name.data(), (long long)value,
                 (long long)v);
    v4l2_ext_control c{};
    c.id = info->id;
    if (info->type == V4L2_CTRL_TYPE_INTEGER64)
        c.value64 = v;
    else
        c.value = int32_t(v);
    v4l2_ext_controls cs{};
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    if (xioctl(fd_, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        LOG.warn("controls: set %.*s=%lld failed: %s", int(name.size()), name.data(), (long long)v, strerror(errno));
        return false;
    }
    return true;
}

bool V4l2Controls::set_many(const std::vector<std::pair<std::string, int64_t>> &values) {
    bool ok = true;
    for (const auto &[name, value] : values)
        ok = set(name, value) && ok;
    return ok;
}

}
