#include "camera/eye/dsp_client.h"

#include "core/log.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace babble {

namespace {

uint32_t read_u32(const uint8_t *base, uint32_t off) {
    return __atomic_load_n(reinterpret_cast<const uint32_t *>(base + off), __ATOMIC_ACQUIRE);
}

} // namespace

DspClient::~DspClient() { disconnect(); }

bool DspClient::connect(uint32_t version) {
    disconnect();
    sock_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        LOG_WARN("dsp: socket failed: %s", strerror(errno));
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    static const char kName[] = "\0dsp_service";
    memcpy(addr.sun_path, kName, sizeof(kName) - 1);
    socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + sizeof(kName) - 1);
    if (::connect(sock_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
        LOG_WARN("dsp: connect to dsp_service failed: %s", strerror(errno));
        disconnect();
        return false;
    }

    // The service sends the shared-memory fd(s) first, then expects our version.
    timeval tv{2, 0};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char payload[256];
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    iovec iov{payload, sizeof(payload)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock_, &msg, MSG_CMSG_CLOEXEC) < 0) {
        LOG_WARN("dsp: recvmsg failed: %s", strerror(errno));
        disconnect();
        return false;
    }
    for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *fds = reinterpret_cast<const int *>(CMSG_DATA(c));
        for (size_t i = 0; i < n; i++) {
            if (shm_fd_ < 0)
                shm_fd_ = fds[i];
            else
                ::close(fds[i]);
        }
    }
    if (shm_fd_ < 0) {
        LOG_WARN("dsp: dsp_service sent no shared-memory fd");
        disconnect();
        return false;
    }
    if (send(sock_, &version, sizeof(version), MSG_NOSIGNAL) != sizeof(version)) {
        LOG_WARN("dsp: sending version failed: %s", strerror(errno));
        disconnect();
        return false;
    }

    void *d = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (d == MAP_FAILED) {
        LOG_WARN("dsp: mmap data region failed: %s", strerror(errno));
        disconnect();
        return false;
    }
    data_ = static_cast<uint8_t *>(d);
    void *c = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, kCtrlOffset);
    if (c == MAP_FAILED) {
        LOG_WARN("dsp: mmap control region failed: %s", strerror(errno));
        disconnect();
        return false;
    }
    ctrl_ = static_cast<uint8_t *>(c);
    return true;
}

void DspClient::disconnect() {
    if (ctrl_)
        munmap(ctrl_, kRegionBytes);
    if (data_)
        munmap(data_, kRegionBytes);
    ctrl_ = data_ = nullptr;
    if (shm_fd_ >= 0)
        ::close(shm_fd_);
    if (sock_ >= 0)
        ::close(sock_);
    shm_fd_ = sock_ = -1;
    saved_fps_bits_.reset();
}

bool DspClient::validate(const uint8_t *ctrl, size_t ctrl_size, const Layout &l, std::string *why) {
    auto fail = [&](const char *fmt, uint32_t a, uint32_t b = 0) {
        if (why) {
            char buf[160];
            snprintf(buf, sizeof(buf), fmt, a, b);
            *why = buf;
        }
        return false;
    };
    // Static bounds: every word we will touch, and every frame slot, must lie
    // inside the 16 MB mappings.
    uint64_t ctrl_end = uint64_t(l.ctrl_base) + std::max(l.locks_r, l.locks_l) + 4ull * l.slots;
    if (ctrl_end > ctrl_size)
        return fail("control words end at 0x%x beyond mapping", uint32_t(ctrl_end));
    uint64_t ring_end = uint64_t(l.ctrl_base) + std::max(l.ring_l, l.ring_r) +
                        uint64_t(l.slots - 1) * l.slot_stride + l.frame_bytes;
    if (ring_end > kRegionBytes)
        return fail("frame ring ends at 0x%x beyond mapping", uint32_t(ring_end));
    if (uint64_t(l.width) * l.height > l.frame_bytes)
        return fail("frame %u bytes larger than slot", l.width * l.height);

    const uint8_t *b = ctrl + l.ctrl_base;
    for (uint32_t off : {l.idx_l, l.idx_r}) {
        uint32_t v = read_u32(b, off);
        if (v >= l.slots && v != kNotReady)
            return fail("index word at +0x%x is 0x%x", off, v);
    }
    // While the eye cameras are idle these words hold small values such as
    // (4,4,1,8,0,0,0,0), not just 0/1; the start sequence then sets them to 1.
    // Small integers pass, pointers and floats (what a wrong base yields) don't.
    for (uint32_t base : {l.locks_l, l.locks_r}) {
        for (uint32_t i = 0; i < l.slots; i++) {
            uint32_t v = read_u32(b, base + 4 * i);
            if (v > 0xff)
                return fail("lock word at +0x%x is 0x%x", base + 4 * i, v);
        }
    }
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t v = read_u32(b, l.enables + 4 * i);
        if (v > 1)
            return fail("enable word at +0x%x is 0x%x", l.enables + 4 * i, v);
    }
    uint32_t fps_bits = read_u32(b, l.fps);
    float fps;
    memcpy(&fps, &fps_bits, sizeof(fps));
    if (!std::isfinite(fps) || fps <= 0.0f || fps > 240.0f)
        return fail("fps word is 0x%x", fps_bits);
    return true;
}

bool DspClient::fps_unset(const uint8_t *ctrl, size_t ctrl_size, std::span<const uint32_t> candidates) {
    if (!ctrl || candidates.empty())
        return false;
    for (uint32_t base : candidates) {
        const uint64_t off = uint64_t(base) + Layout{}.fps;
        if (off + 4 > ctrl_size || read_u32(ctrl, uint32_t(off)) != 0)
            return false;
    }
    return true;
}

std::optional<DspClient::Layout> DspClient::probe(std::span<const uint32_t> candidates, std::string *why) {
    if (!ctrl_) {
        if (why)
            *why = "not connected";
        return std::nullopt;
    }
    std::string reasons;
    for (uint32_t base : candidates) {
        Layout l;
        l.ctrl_base = base;
        std::string r;
        if (validate(ctrl_, kRegionBytes, l, &r))
            return l;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s0x%x: %s", reasons.empty() ? "" : "; ", base, r.c_str());
        reasons += buf;
    }
    if (why)
        *why = reasons;
    return std::nullopt;
}

uint32_t DspClient::load32(uint32_t off) const { return read_u32(ctrl_, off); }

void DspClient::store32(uint32_t off, uint32_t v) {
    __atomic_store_n(reinterpret_cast<uint32_t *>(ctrl_ + off), v, __ATOMIC_RELEASE);
}

void DspClient::start_streaming(const Layout &l, float fps) {
    const uint32_t b = l.ctrl_base;
    for (uint32_t i = 0; i < l.slots; i++) {
        store32(b + l.locks_l + 4 * i, 1);
        store32(b + l.locks_r + 4 * i, 1);
    }
    store32(b + l.enables, 1);
    store32(b + l.enables + 4, 1);
    store32(b + l.transition, 1);
    if (fps > 0) {
        if (!saved_fps_bits_)
            saved_fps_bits_ = load32(b + l.fps);
        uint32_t bits;
        memcpy(&bits, &fps, sizeof(bits));
        store32(b + l.fps, bits);
    }
}

void DspClient::restore_fps(const Layout &l) {
    if (!ctrl_ || !saved_fps_bits_)
        return;
    store32(l.ctrl_base + l.fps, *saved_fps_bits_);
    float fps;
    memcpy(&fps, &*saved_fps_bits_, sizeof(fps));
    LOG_INFO("dsp: restored eye camera rate %.1f fps", fps);
    saved_fps_bits_.reset();
}

DspClient::Indices DspClient::read_indices(const Layout &l) const {
    return {load32(l.ctrl_base + l.idx_l), load32(l.ctrl_base + l.idx_r)};
}

const uint8_t *DspClient::slot_ptr(const Layout &l, bool right, uint32_t idx) const {
    if (idx >= l.slots)
        return nullptr;
    return data_ + l.ctrl_base + (right ? l.ring_r : l.ring_l) + size_t(idx) * l.slot_stride;
}

void DspClient::release(const Layout &l, bool right, uint32_t idx) {
    if (idx >= l.slots)
        return;
    store32(l.ctrl_base + (right ? l.locks_r : l.locks_l) + 4 * idx, 1);
}

} // namespace babble
