#include "sink/camera_stream_sink.h"

#include "core/clock.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <optional>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#pragma GCC diagnostic pop

namespace {
const babble::log::Logger LOG("camstream");
} // namespace

namespace babble {

namespace camera_stream {

const char *view_name(View v) {
    switch (v) {
    case FaceL: return "face_l";
    case FaceR: return "face_r";
    case EyeL: return "eye_l";
    case EyeR: return "eye_r";
    case kViews: break;
    }
    return "?";
}

void copy_view(View v, const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, Image *out) {
    // The DSP's 512x512 eye views, cropped as the streamer crops them:
    // eye_l [16:416, 56:456], eye_r [16:416, 0:400].
    uint32_t x0 = 0, y0 = 0, w = width, h = height;
    if ((v == EyeL || v == EyeR) && width == 512 && height == 512) {
        y0 = 16;
        x0 = v == EyeL ? 56 : 0;
        w = h = 400;
    }
    out->width = w;
    out->height = h;
    out->pixels.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
        memcpy(out->pixels.data() + size_t(y) * w, data + size_t(y0 + y) * stride + x0, w);
}

std::vector<uint8_t> packet(const std::vector<Part> &parts) {
    std::vector<uint8_t> out;
    auto put = [&](uint64_t v, int bytes) {
        for (int i = bytes - 1; i >= 0; i--)
            out.push_back(uint8_t(v >> (8 * i)));
    };
    size_t total = 5;
    for (const auto &p : parts)
        total += 13 + p.jpeg->size();
    out.reserve(total);
    put(0xFEEDFACE, 4);
    put(parts.size(), 1);
    for (const auto &p : parts) {
        put(p.view, 1);
        put(p.stamp_ns, 8);
        put(p.jpeg->size(), 4);
        out.insert(out.end(), p.jpeg->begin(), p.jpeg->end());
    }
    return out;
}

namespace {

// libjpeg-turbo, loaded at run time when the system has it: ~5x faster than
// stb (0.6 vs 3.4 ms per 400x400 on the Frame), and no build dependency.
struct TurboJpeg {
    using Init = void *(*)();
    using Compress = int (*)(void *, const unsigned char *, int, int, int, int, unsigned char **, unsigned long *,
                             int, int, int);
    using Free = void (*)(unsigned char *);
    using Destroy = int (*)(void *);
    static constexpr int kPixelGray = 6, kSampGray = 3, kFastDct = 2048;   // TJPF_GRAY, TJSAMP_GRAY, TJFLAG_FASTDCT

    Init init = nullptr;
    Compress compress = nullptr;
    Free free = nullptr;
    Destroy destroy = nullptr;

    static const TurboJpeg *get() {
        static const TurboJpeg *tj = [] () -> const TurboJpeg * {
            void *lib = dlopen("libturbojpeg.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!lib) {
                LOG.info("libturbojpeg not found; encoding with stb (slower)");
                return nullptr;
            }
            auto *t = new TurboJpeg;
            t->init = reinterpret_cast<Init>(dlsym(lib, "tjInitCompress"));
            t->compress = reinterpret_cast<Compress>(dlsym(lib, "tjCompress2"));
            t->free = reinterpret_cast<Free>(dlsym(lib, "tjFree"));
            t->destroy = reinterpret_cast<Destroy>(dlsym(lib, "tjDestroy"));
            if (!t->init || !t->compress || !t->free || !t->destroy) {
                LOG.warn("libturbojpeg lacks the expected functions; encoding with stb");
                delete t;
                return nullptr;
            }
            return t;
        }();
        return tj;
    }
};

// One compressor per thread (a handle isn't thread-safe), freed with the thread.
struct TurboHandle {
    void *h = nullptr;
    ~TurboHandle() {
        if (h)
            TurboJpeg::get()->destroy(h);
    }
};

} // namespace

bool encode_jpeg(const Image &img, int quality, std::vector<uint8_t> *out) {
    out->clear();
    if (img.pixels.empty())
        return false;
    if (const TurboJpeg *tj = TurboJpeg::get()) {
        thread_local TurboHandle handle;
        if (!handle.h)
            handle.h = tj->init();
        unsigned char *buf = nullptr;
        unsigned long size = 0;
        if (handle.h && tj->compress(handle.h, img.pixels.data(), int(img.width), int(img.width), int(img.height),
                                     TurboJpeg::kPixelGray, &buf, &size, TurboJpeg::kSampGray, quality,
                                     TurboJpeg::kFastDct) == 0 && buf) {
            out->assign(buf, buf + size);
            tj->free(buf);
            return !out->empty();
        }
        if (buf)
            tj->free(buf);
    }
    auto write = [](void *ctx, void *data, int size) {
        auto *v = static_cast<std::vector<uint8_t> *>(ctx);
        v->insert(v->end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
    };
    return stbi_write_jpg_to_func(write, out, int(img.width), int(img.height), 1, img.pixels.data(), quality) != 0 &&
           !out->empty();
}

} // namespace camera_stream

namespace {

std::optional<camera_stream::View> view_of(FrameOrigin o) {
    switch (o) {
    case FrameOrigin::MouthL: return camera_stream::FaceL;
    case FrameOrigin::MouthR: return camera_stream::FaceR;
    case FrameOrigin::Face: return camera_stream::FaceL;   // a USB face tracker: one view
    case FrameOrigin::EyeL: return camera_stream::EyeL;
    case FrameOrigin::EyeR: return camera_stream::EyeR;
    }
    return std::nullopt;
}

uint64_t realtime_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

// Sends all of `data`, giving up when told to stop or the peer goes away.
bool send_all(int fd, const uint8_t *data, size_t size, const std::atomic<bool> &stop) {
    while (size && !stop) {
        const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            size -= size_t(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;   // the send timeout: check stop and keep going
        } else {
            return false;
        }
    }
    return size == 0;
}

} // namespace

CameraStreamSink::CameraStreamSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "sink"), "bbl-camstream")),
      stats_(deps.stats), port_(cfg.value("port", 9099)), fps_(std::max(1.0, cfg.value("fps", 60.0))),
      quality_(std::clamp(cfg.value("quality", 80), 1, 100)), max_clients_(std::max(1, cfg.value("max_clients", 4))),
      active_(cfg.value("active", false)) {
    const auto views = cfg.value("views", std::vector<std::string>{"face_l", "face_r", "eye_l", "eye_r"});
    for (const auto &v : views) {
        bool known = false;
        for (int i = 0; i < camera_stream::kViews; i++)
            if (v == camera_stream::view_name(camera_stream::View(i)))
                want_[size_t(i)] = known = true;
        if (!known)
            LOG.warn("%s: unknown view '%s'", name_.c_str(), v.c_str());
    }
    stats_.add(name_ + ".packets_per_s", &sent_);
    stats_.add(name_ + ".encode_failures", &encode_failures_);
}

CameraStreamSink::~CameraStreamSink() {
    stop();
    stats_.remove_prefix(name_ + ".");
}

bool CameraStreamSink::start() {
    started_ = true;
    if (active_)
        start_server();
    return true;   // a port in use is logged; the switch can try again
}

void CameraStreamSink::stop() {
    stop_server();
    started_ = false;
}

bool CameraStreamSink::set_option(const std::string &key, const nlohmann::json &value) {
    if (key != "active" || !value.is_boolean())
        return false;
    const bool on = value.get<bool>();
    active_ = on;
    if (started_) {
        if (on)
            start_server();
        else
            stop_server();
    }
    return true;
}

void CameraStreamSink::offer_frames(const std::string &, const StereoFrame &frames) {
    if (!active_ || clients_ == 0)
        return;   // nobody watching: no copies
    // Both formats are one byte per pixel: the mouth cameras' raw Bayer goes
    // out as gray, as stream_daemon.py sends it.
    for (const FramePtr *f : {&frames.left, &frames.right})
        if (*f)
            if (auto v = view_of((*f)->origin); v && want_[*v])
                take(*v, **f);
}

void CameraStreamSink::take(camera_stream::View v, const Frame &f) {
    // Capture time on the streamer's clock (wall time), from our monotonic one.
    const uint64_t age = mono_ns() > f.capture_ns ? mono_ns() - f.capture_ns : 0;
    const uint64_t stamp = realtime_ns() - age;
    std::lock_guard lock(images_mutex_);
    camera_stream::Image &img = images_[v];
    if (img.stamp_ns == stamp)
        return;   // the same frame again (two pipelines on one source)
    camera_stream::copy_view(v, f.data, f.width, f.height, f.stride ? f.stride : f.width, &img);
    img.stamp_ns = stamp;
    img.seq = next_seq_++;
}

bool CameraStreamSink::start_server() {
    std::lock_guard lock(server_mutex_);
    if (listen_fd_ >= 0)
        return true;
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port_));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listen_fd_ < 0 || bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd_, 4) != 0) {
        LOG.warn("%s: cannot listen on tcp %d: %s", name_.c_str(), port_, strerror(errno));
        if (listen_fd_ >= 0)
            close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    stop_server_ = false;
    server_ = start_thread(thread_spec_, [this] { serve(); });
    std::string views;
    for (int i = 0; i < camera_stream::kViews; i++)
        if (want_[size_t(i)])
            views += (views.empty() ? "" : ",") + std::string(camera_stream::view_name(camera_stream::View(i)));
    LOG.info("%s: streaming %s on tcp %d (MJPEG, up to %.0f fps, quality %d)", name_.c_str(), views.c_str(), port_,
             fps_, quality_);
    return true;
}

void CameraStreamSink::stop_server() {
    std::lock_guard lock(server_mutex_);
    if (listen_fd_ < 0)
        return;
    stop_server_ = true;
    if (server_.joinable())
        server_.join();   // it joins its clients
    close(listen_fd_);
    listen_fd_ = -1;
    std::lock_guard images(images_mutex_);
    for (auto &img : images_)
        img = {};
    LOG.info("%s: stopped streaming", name_.c_str());
}

void CameraStreamSink::serve() {
    while (!stop_server_) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0)
            continue;
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int fd = accept4(listen_fd_, reinterpret_cast<sockaddr *>(&from), &len, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        const std::string peer = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
        if (clients_ >= max_clients_) {
            LOG.warn("%s: refusing %s: already %d clients", name_.c_str(), peer.c_str(), int(clients_));
            close(fd);
            continue;
        }
        clients_++;
        std::lock_guard lock(clients_mutex_);
        // Same cores as the other sink threads: encoding never lands on a tracking core.
        client_threads_.push_back(start_thread(thread_spec_, [this, fd, peer] { client_loop(fd, peer); }));
    }
    std::lock_guard lock(clients_mutex_);
    for (auto &t : client_threads_)
        if (t.joinable())
            t.join();
    client_threads_.clear();
}

void CameraStreamSink::client_loop(int fd, std::string peer) {
    pthread_setname_np(pthread_self(), "bbl-camclient");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval tv{0, 200'000};   // sends wake up to check for stop
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    LOG.info("%s: client %s connected", name_.c_str(), peer.c_str());

    const auto period = std::chrono::nanoseconds(uint64_t(1e9 / fps_));
    auto next = std::chrono::steady_clock::now() + period;
    std::array<uint64_t, camera_stream::kViews> last_seq{};
    std::array<std::vector<uint8_t>, camera_stream::kViews> jpegs;
    camera_stream::Image img;
    uint64_t packets = 0;
    while (!stop_server_) {
        std::vector<camera_stream::Part> parts;
        for (int i = 0; i < camera_stream::kViews; i++) {
            if (!want_[size_t(i)])
                continue;
            {
                std::lock_guard lock(images_mutex_);
                const camera_stream::Image &cur = images_[size_t(i)];
                if (cur.seq == 0 || cur.seq == last_seq[size_t(i)])
                    continue;   // nothing new from this camera
                img = cur;
            }
            last_seq[size_t(i)] = img.seq;
            if (!camera_stream::encode_jpeg(img, quality_, &jpegs[size_t(i)])) {
                encode_failures_.add();
                continue;
            }
            parts.push_back({camera_stream::View(i), img.stamp_ns, &jpegs[size_t(i)]});
        }
        if (!parts.empty()) {
            const auto pkt = camera_stream::packet(parts);
            if (!send_all(fd, pkt.data(), pkt.size(), stop_server_))
                break;
            sent_.tick();
            packets++;
        }
        std::this_thread::sleep_until(next);
        next = std::max(next + period, std::chrono::steady_clock::now());
    }
    close(fd);
    clients_--;
    LOG.info("%s: client %s disconnected after %llu packets", name_.c_str(), peer.c_str(),
             (unsigned long long)packets);
}

} // namespace babble
