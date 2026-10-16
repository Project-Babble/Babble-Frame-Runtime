#include "camera/v4l2/v4l2_capture.h"

#include "core/clock.h"
#include "core/log.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

std::string fourcc_str(uint32_t f) {
    char s[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0};
    return s;
}

}

V4l2Capture::V4l2Capture(Params params) : params_(std::move(params)) {}

V4l2Capture::~V4l2Capture() { stop(); }

void V4l2Capture::fail(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    error_ = buf;
    LOG.warn("%s: %s", params_.node.c_str(), buf);
    release_device();
    state_ = SourceState::Unavailable;
}

bool V4l2Capture::open() {
    stop();
    error_.clear();
    fd_ = ::open(params_.node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        fail("open failed: %s", strerror(errno));
        return false;
    }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
        fail("QUERYCAP failed: %s", strerror(errno));
        return false;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
        fail("not a multiplanar streaming capture node (caps 0x%08x)", caps);
        return false;
    }

    // Set the expected format and verify it, because with I/O MC the format follows the media routing.
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = params_.width;
    fmt.fmt.pix_mp.height = params_.height;
    fmt.fmt.pix_mp.pixelformat = params_.fourcc;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) != 0) {
        fail("S_FMT failed: %s", strerror(errno));
        return false;
    }
    const auto &pm = fmt.fmt.pix_mp;
    if (pm.width != params_.width || pm.height != params_.height || pm.pixelformat != params_.fourcc ||
        pm.num_planes != 1) {
        fail("format is %ux%u %s planes=%u, expected %ux%u %s", pm.width, pm.height, fourcc_str(pm.pixelformat).c_str(),
             pm.num_planes, params_.width, params_.height, fourcc_str(params_.fourcc).c_str());
        return false;
    }
    stride_ = pm.plane_fmt[0].bytesperline ? pm.plane_fmt[0].bytesperline : params_.width;
    frame_bytes_ = size_t(stride_) * params_.height;
    if (pm.plane_fmt[0].sizeimage < frame_bytes_) {
        fail("sizeimage %u smaller than %zu", pm.plane_fmt[0].sizeimage, frame_bytes_);
        return false;
    }

    v4l2_requestbuffers req{};
    req.count = params_.buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
        fail("REQBUFS(%u) failed: %s (got %u)", params_.buffers, strerror(errno), req.count);
        return false;
    }
    if (req.count > 64) {
        fail("driver allocated %u buffers, more than supported", req.count);
        return false;
    }

    std::vector<uint8_t *> ptrs;
    std::vector<std::pair<void *, size_t>> maps;
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) != 0) {
            for (auto &[p, len] : maps)
                munmap(p, len);
            fail("QUERYBUF(%u) failed: %s", i, strerror(errno));
            return false;
        }
        void *p = mmap(nullptr, planes[0].length, PROT_READ, MAP_SHARED, fd_, planes[0].m.mem_offset);
        if (p == MAP_FAILED) {
            for (auto &[m, len] : maps)
                munmap(m, len);
            fail("mmap(%u) failed: %s", i, strerror(errno));
            return false;
        }
        maps.emplace_back(p, planes[0].length);
        ptrs.push_back(static_cast<uint8_t *>(p));
    }

    released_ = std::make_shared<std::atomic<uint64_t>>(0);
    auto released = released_;
    // The pool unmaps the buffers when the last frame is released, which can be after the fd closes.
    pool_ = FramePool::create_external(
        ptrs, frame_bytes_, [released](uint32_t idx) { released->fetch_or(1ull << idx, std::memory_order_release); },
        [maps] {
            for (auto &[p, len] : maps)
                munmap(p, len);
        });
    generation_++;
    state_ = SourceState::Opened;
    LOG.info("%s: opened %ux%u %s stride %u, %u buffers", params_.node.c_str(), params_.width, params_.height,
             fourcc_str(params_.fourcc).c_str(), stride_, req.count);
    return true;
}

bool V4l2Capture::queue(uint32_t index) {
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.m.planes = planes;
    buf.length = 1;
    if (xioctl(fd_, VIDIOC_QBUF, &buf) != 0)
        return false;
    queued_++;
    return true;
}

bool V4l2Capture::start(FrameSink sink) {
    if (state_ != SourceState::Opened && !open())
        return false;
    sink_ = std::move(sink);
    queued_ = 0;
    for (uint32_t i = 0; i < pool_->capacity(); i++) {
        if (!queue(i)) {
            fail("QBUF(%u) failed: %s", i, strerror(errno));
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
        fail("STREAMON failed: %s", strerror(errno));
        return false;
    }
    streaming_ = true;
    have_seq_ = false;
    stop_ = false;
    state_ = SourceState::Streaming;
    thread_ = start_thread(params_.thread, [this] { run(); });
    return true;
}

void V4l2Capture::requeue_released() {
    uint64_t mask = released_->exchange(0, std::memory_order_acquire);
    while (mask) {
        uint32_t idx = uint32_t(__builtin_ctzll(mask));
        mask &= mask - 1;
        if (!queue(idx))
            LOG.warn("%s: re-QBUF(%u) failed: %s", params_.node.c_str(), idx, strerror(errno));
    }
}

void V4l2Capture::lost(const std::string &reason) {
    error_ = reason;
    state_ = SourceState::Lost;
    LOG.warn("%s: lost: %s", params_.node.c_str(), reason.c_str());
    if (sink_.on_lost)
        sink_.on_lost(reason);
}

void V4l2Capture::run() {
    uint64_t last_frame = mono_ns();
    bool logged_ts = false;
    while (!stop_) {
        requeue_released();
        if (queued_ == 0) {
            // Do not poll now because all buffers are downstream and poll then reports an error.
            sleep_ns(1000000);
            if (mono_ns() - last_frame > params_.silence_timeout_ns * 2) {
                lost("all buffers held downstream");
                return;
            }
            continue;
        }

        pollfd pfd{fd_, POLLIN | POLLPRI, 0};
        int r = poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            lost(std::string("poll: ") + strerror(errno));
            return;
        }
        if (r == 0) {
            if (mono_ns() - last_frame > params_.silence_timeout_ns) {
                lost("no frames");
                return;
            }
            continue;
        }
        if (pfd.revents & POLLERR) {
            lost("POLLERR (stream stopped by driver)");
            return;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd_, VIDIOC_DQBUF, &buf) != 0) {
            if (errno == EAGAIN)
                continue;
            lost(std::string("DQBUF: ") + strerror(errno));
            return;
        }
        queued_--;
        uint64_t now = mono_ns();

        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            error_buffers.add();
            queue(buf.index);
            continue;
        }
        const uint64_t raw_now = mono_raw_ns();
        const uint64_t buf_ts = timeval_ns(buf.timestamp);
        if (!logged_ts) {
            // Find the timestamp clock because Qualcomm CAMSS uses CLOCK_MONOTONIC_RAW but sets the MONOTONIC flag.
            uint32_t flag = buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
            auto age = [&](uint64_t clk) { return buf_ts <= clk ? clk - buf_ts : UINT64_MAX; };
            const uint64_t limit = 500000000ull;
            if (flag != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
                ts_clock_ = TsClock::Arrival;
            else if (age(now) <= limit && age(now) <= age(raw_now))
                ts_clock_ = TsClock::Monotonic;
            else if (age(raw_now) <= limit)
                ts_clock_ = TsClock::MonotonicRaw;
            else
                ts_clock_ = TsClock::Arrival;
            static const char *names[] = {"CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW (converted)", "arrival time"};
            LOG.info("%s: timestamps %s, source %s", params_.node.c_str(), names[int(ts_clock_)],
                     (buf.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) == V4L2_BUF_FLAG_TSTAMP_SRC_SOE ? "start of exposure"
                                                                                                : "end of frame");
            logged_ts = true;
        }
        if (have_seq_ && buf.sequence > last_seq_ + 1)
            driver_drops.add(buf.sequence - last_seq_ - 1);
        last_seq_ = buf.sequence;
        have_seq_ = true;
        last_frame = now;

        auto frame = pool_->acquire(buf.index);
        if (!frame) {
            queue(buf.index);
            continue;
        }
        frame->origin = params_.origin;
        frame->fmt = PixelFormat::Bayer8;
        frame->width = params_.width;
        frame->height = params_.height;
        frame->stride = stride_;
        frame->size = frame_bytes_;
        switch (ts_clock_) {
        case TsClock::Monotonic: frame->capture_ns = buf_ts; break;
        case TsClock::MonotonicRaw: frame->capture_ns = buf_ts - (raw_now - now); break;
        case TsClock::Arrival: frame->capture_ns = now; break;
        }
        frame->arrival_ns = now;
        frame->seq = buf.sequence;
        frame->generation = generation_;
        frames.tick();
        if (sink_.on_frame)
            sink_.on_frame(std::move(frame));
    }
}

void V4l2Capture::release_device() {
    if (fd_ >= 0) {
        if (streaming_) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        ::close(fd_);
        fd_ = -1;
    }
    if (pool_) {
        // Frames that are downstream keep the buffers, but their release does not queue them again.
        pool_->detach();
        pool_.reset();
    }
    queued_ = 0;
}

void V4l2Capture::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    release_device();
    if (state_ != SourceState::Unavailable)
        state_ = SourceState::Closed;
}

}
