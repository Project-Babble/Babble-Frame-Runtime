#include "capture/hevc_encoder.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace babble::capture {

namespace {

constexpr uint32_t kOutBufs = 8, kCapBufs = 8;
constexpr int kStallMs = 3000;   // The encoder is stuck if no buffer comes back for this time.

int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

bool set_ctrl(int fd, uint32_t id, int32_t value) {
    v4l2_control c{};
    c.id = id;
    c.value = value;
    return xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

std::string sysfs_name(int n) {
    std::ifstream f("/sys/class/video4linux/video" + std::to_string(n) + "/name");
    std::string s;
    std::getline(f, s);
    return s;
}

}
HevcEncoder::~HevcEncoder() { close(); }

std::string HevcEncoder::find_device() {
    // Use the sysfs name so that the search never opens a camera node.
    for (int n = 0; n < 64; n++) {
        if (sysfs_name(n) != "qcom-iris-encoder")
            continue;
        const std::string path = "/dev/video" + std::to_string(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        bool hevc = false;
        v4l2_fmtdesc d{};
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        for (d.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
            hevc = hevc || d.pixelformat == V4L2_PIX_FMT_HEVC;
        ::close(fd);
        if (hevc)
            return path;
    }
    return "";
}

bool HevcEncoder::fail(std::string *err, const std::string &what) {
    if (err)
        *err = what;
    return false;
}

bool HevcEncoder::open(const Settings &s, PacketFn on_packet, std::string *err, std::string device) {
    close();
    s_ = s;
    if (s_.gop <= 0)
        s_.gop = std::max(1, int(2 * s_.fps + 0.5));
    on_packet_ = std::move(on_packet);
    if (device.empty())
        device = find_device();
    if (device.empty())
        return fail(err, "no hardware H.265 encoder on this machine");
    device_ = device;
    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0)
        return fail(err, device + ": " + strerror(errno));
    auto bad = [&](const char *what) {
        const std::string e = std::string(what) + ": " + strerror(errno);
        close();
        return fail(err, e);
    };

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) {
        char ver[32];
        snprintf(ver, sizeof(ver), " %u.%u.%u", (cap.version >> 16) & 0xff, (cap.version >> 8) & 0xff,
                 cap.version & 0xff);
        driver_ = std::string(reinterpret_cast<const char *>(cap.driver)) + ver;
    }

    // Set the stream format before the frame format because the driver derives one from the other.
    v4l2_format cf{};
    cf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cf.fmt.pix_mp.width = s_.width;
    cf.fmt.pix_mp.height = s_.height;
    cf.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    cf.fmt.pix_mp.num_planes = 1;
    cf.fmt.pix_mp.plane_fmt[0].sizeimage = 4u << 20;
    if (xioctl(fd_, VIDIOC_S_FMT, &cf) < 0)
        return bad("setting the stream format");
    v4l2_format of{};
    of.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    of.fmt.pix_mp.width = s_.width;
    of.fmt.pix_mp.height = s_.height;
    of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    of.fmt.pix_mp.num_planes = 1;
    // Gray goes into Y unchanged, so the stream is full range.
    of.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    of.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
    of.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    of.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (xioctl(fd_, VIDIOC_S_FMT, &of) < 0)
        return bad("setting the frame format");
    // Use the padded layout that the driver returns, for example 800x416 with an 896-byte stride for 800x400.
    stride_ = of.fmt.pix_mp.plane_fmt[0].bytesperline;
    padded_h_ = of.fmt.pix_mp.height;
    out_bytes_ = of.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (of.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 || stride_ < s_.width || padded_h_ < s_.height ||
        out_bytes_ < size_t(stride_) * padded_h_ * 3 / 2) {
        close();
        return fail(err, "the encoder chose an unexpected frame layout");
    }

    v4l2_streamparm sp{};
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sp.parm.output.timeperframe.numerator = 1000;
    sp.parm.output.timeperframe.denominator = uint32_t(s_.fps * 1000 + 0.5);
    xioctl(fd_, VIDIOC_S_PARM, &sp);   // This is only a rate hint.

    set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_GOP_SIZE, s_.gop);
    set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
    set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
    // Pin every QP limit under VBR for constant QP because the driver ignores FRAME_RC_ENABLE=0.
    bool qp_ok = set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1) &&
                 set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) &&
                 set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE, 240000000) &&
                 set_ctrl(fd_, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 245000000);
    for (uint32_t id : {V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
                        V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MIN_QP, V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MAX_QP,
                        V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_MIN_QP, V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_MAX_QP,
                        V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP})
        qp_ok = set_ctrl(fd_, id, s_.qp) && qp_ok;
    if (!qp_ok)
        return bad("setting constant QP");

    auto map_bufs = [&](uint32_t type, uint32_t count, std::vector<Buf> *bufs) {
        v4l2_requestbuffers rq{};
        rq.count = count;
        rq.type = type;
        rq.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &rq) < 0 || rq.count == 0)
            return false;
        for (uint32_t i = 0; i < rq.count; i++) {
            v4l2_plane pl[1]{};
            v4l2_buffer b{};
            b.type = type;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            b.m.planes = pl;
            b.length = 1;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0)
                return false;
            void *p = mmap(nullptr, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, pl[0].m.mem_offset);
            if (p == MAP_FAILED)
                return false;
            bufs->push_back({p, pl[0].length});
        }
        return true;
    };
    if (!map_bufs(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, kOutBufs, &out_))
        return bad("frame buffers");
    if (!map_bufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, kCapBufs, &cap_))
        return bad("stream buffers");
    for (size_t i = 0; i < out_.size(); i++) {
        // Fill the constant chroma one time.
        auto *y = static_cast<uint8_t *>(out_[i].p);
        const size_t luma = size_t(stride_) * padded_h_;
        memset(y, 0, luma);
        memset(y + luma, 128, std::min(out_[i].len, size_t(out_bytes_)) - luma);
        free_out_.push_back(int(i));
    }
    for (uint32_t i = 0; i < cap_.size(); i++) {
        v4l2_plane pl[1]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = pl;
        b.length = 1;
        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0)
            return bad("queueing stream buffers");
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &t) < 0)
        return bad("starting the stream");
    streaming_ = true;
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &t) < 0)
        return bad("starting the frames");
    return true;
}

bool HevcEncoder::service(int timeout_ms, std::string *err) {
    pollfd p{fd_, POLLIN | POLLOUT | POLLPRI, 0};
    const int pr = poll(&p, 1, timeout_ms);
    if (pr == 0)
        return timeout_ms == 0 || fail(err, "the hardware encoder stopped responding");
    if (pr < 0 && errno != EINTR)
        return fail(err, std::string("poll: ") + strerror(errno));
    bool progress = false;
    for (;;) {   // Take back the frames that the encoder has finished.
        v4l2_plane pl[1]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = 1;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0)
            break;
        free_out_.push_back(int(b.index));
        progress = true;
    }
    for (;;) {   // Send the stream packets.
        v4l2_plane pl[1]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = 1;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0)
            break;
        progress = true;
        if (b.index >= cap_.size() || pl[0].bytesused > cap_[b.index].len)
            return fail(err, "the encoder returned a bad stream buffer");
        const size_t n = pl[0].bytesused - std::min(pl[0].bytesused, pl[0].data_offset);
        if (n) {
            packets_out_++;
            if (!packet_failed_ && !on_packet_(static_cast<const uint8_t *>(cap_[b.index].p) + pl[0].data_offset, n))
                packet_failed_ = true;
        }
        if (b.flags & V4L2_BUF_FLAG_LAST) {
            last_ = true;
            break;
        }
        pl[0].bytesused = 0;
        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0)
            return fail(err, std::string("requeueing a stream buffer: ") + strerror(errno));
    }
    if (packet_failed_)
        return fail(err, "writing the stream failed");
    if ((p.revents & POLLERR) && !progress && !last_)
        return fail(err, "the hardware encoder reported an error");
    return true;
}

bool HevcEncoder::encode(const uint8_t *gray, std::string *err) {
    if (fd_ < 0 || last_)
        return fail(err, "the encoder isn't open");
    while (free_out_.empty())
        if (!service(kStallMs, err))
            return false;
    const int i = free_out_.back();
    free_out_.pop_back();
    auto *y = static_cast<uint8_t *>(out_[size_t(i)].p);
    for (uint32_t r = 0; r < s_.height; r++)
        memcpy(y + size_t(r) * stride_, gray + size_t(r) * s_.width, s_.width);
    v4l2_plane pl[1]{};
    pl[0].bytesused = out_bytes_;
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = uint32_t(i);
    b.m.planes = pl;
    b.length = 1;
    const uint64_t us = uint64_t(double(frames_in_) * 1e6 / s_.fps);
    b.timestamp.tv_sec = time_t(us / 1000000);
    b.timestamp.tv_usec = suseconds_t(us % 1000000);
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0)
        return fail(err, std::string("queueing a frame: ") + strerror(errno));
    frames_in_++;
    return service(0, err);   // Collect the ready packets so that they do not accumulate.
}

bool HevcEncoder::finish(std::string *err) {
    if (fd_ < 0)
        return fail(err, "the encoder isn't open");
    while (free_out_.size() < out_.size())
        if (!service(kStallMs, err))
            return false;
    if (frames_in_ > 0 && !last_) {
        v4l2_encoder_cmd ec{};
        ec.cmd = V4L2_ENC_CMD_STOP;
        if (xioctl(fd_, VIDIOC_ENCODER_CMD, &ec) < 0)
            return fail(err, std::string("draining: ") + strerror(errno));
        while (!last_)
            if (!service(kStallMs, err))
                return false;
    }
    return true;
}

void HevcEncoder::close() {
    if (fd_ >= 0) {
        if (streaming_) {
            int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            xioctl(fd_, VIDIOC_STREAMOFF, &t);
            t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(fd_, VIDIOC_STREAMOFF, &t);
        }
        for (auto *bufs : {&out_, &cap_})
            for (const Buf &b : *bufs)
                munmap(b.p, b.len);
        for (uint32_t type : {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE}) {
            v4l2_requestbuffers rq{};
            rq.type = type;
            rq.memory = V4L2_MEMORY_MMAP;
            xioctl(fd_, VIDIOC_REQBUFS, &rq);   // A count of 0 frees the buffers.
        }
        ::close(fd_);
    }
    fd_ = -1;
    out_.clear();
    cap_.clear();
    free_out_.clear();
    streaming_ = last_ = packet_failed_ = false;
    frames_in_ = packets_out_ = 0;
}

}
