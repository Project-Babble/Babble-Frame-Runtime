#include "platform/eye_server_client.h"

#include "core/clock.h"
#include "core/log.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("eyesrv");
}

namespace babble {

namespace eye_server {

static_assert(sizeof(pthread_mutex_t) <= kMutexBytes, "the eye service's mutex slot is 48 bytes");

namespace {

template <typename T> T read_at(const uint8_t *p, size_t off) {
    T v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

void read_vecs(const uint8_t *p, size_t off, std::array<Vec3, 2> *out) {
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 3; k++)
            (*out)[e][k] = read_at<float>(p, off + 4 * (3 * e + k));
}

}

bool parse_record(const uint8_t *r, Sample *out) {
    if (read_at<uint32_t>(r, rec::kProducerState) != 1)
        return false;
    Sample s;
    s.sample_time = read_at<double>(r, rec::kSampleTime);
    read_vecs(r, rec::kGaze, &s.gaze);
    read_vecs(r, rec::kPreFusionGaze, &s.pre_fusion_gaze);
    for (int k = 0; k < 3; k++)
        s.fixation[k] = read_at<float>(r, rec::kFixation + 4 * k);
    for (int e = 0; e < 2; e++)
        s.openness[e] = read_at<float>(r, rec::kOpenness + 4 * e);

    bool finite = std::isfinite(s.sample_time);
    for (const auto &eye : {s.gaze[0], s.gaze[1], s.pre_fusion_gaze[0], s.pre_fusion_gaze[1], s.fixation})
        for (float v : eye)
            finite = finite && std::isfinite(v);
    for (float v : s.openness)
        finite = finite && std::isfinite(v);
    if (!finite)
        return false;
    *out = s;
    return true;
}

std::optional<int> parse_dominant_eye(const uint8_t *p) {
    const int32_t eye = read_at<int32_t>(p, 1);
    if (p[0] == 0 && eye >= -1 && eye <= 1)
        return -1;
    if (p[0] == 1 && (eye == kLeftEye || eye == kRightEye))
        return eye;
    return std::nullopt;
}

}

using namespace eye_server;

namespace {

pthread_mutex_t *shm_mutex(uint8_t *base) { return reinterpret_cast<pthread_mutex_t *>(base + kMutexOffset); }
uint32_t *shm_word(uint8_t *base, size_t off) { return reinterpret_cast<uint32_t *>(base + off); }

// Locks the robust mutex of the service and recovers it if its last owner died.
bool lock(uint8_t *base) {
    int rc = pthread_mutex_lock(shm_mutex(base));
    if (rc == EOWNERDEAD)
        rc = pthread_mutex_consistent(shm_mutex(base));
    if (rc != 0) {
        LOG.warn("eye-server: locking the shared mutex failed: %s", strerror(rc));
        return false;
    }
    return true;
}

}

EyeServerClient::~EyeServerClient() { stop(); }

void EyeServerClient::start(ThreadSpec thread, Options options, StatsRegistry *stats) {
    thread_spec_ = std::move(thread);
    options_ = std::move(options);
    stats_ = stats;
    if (stats_) {
        stats_->add("eyesrv.samples_per_s", &samples_);
        stats_->add("eyesrv.timeouts", &timeouts_);
        stats_->add("eyesrv.reopens", &reopens_);
    }
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void EyeServerClient::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    close_map();
    if (stats_) {
        stats_->remove_prefix("eyesrv.");
        stats_ = nullptr;
    }
}

void EyeServerClient::want(Consumer c, bool on) {
    if (on)
        wanted_.fetch_or(c);
    else
        wanted_.fetch_and(~uint32_t(c));
}

std::optional<Sample> EyeServerClient::latest(uint64_t max_age_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || mono_ns() - latest_->received_ns > max_age_ns)
        return std::nullopt;
    return latest_;
}

void EyeServerClient::set_listener(std::function<void(const Sample &)> fn) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    listener_ = std::move(fn);
}

bool EyeServerClient::open_map() {
    auto problem = [&](std::string why) {
        if (why != last_problem_)   // Log each different reason only once.
            LOG.warn("eye-server: %s: %s; independent gaze and native openness unavailable", options_.path.c_str(),
                     why.c_str());
        last_problem_ = std::move(why);
        close_map();
        return false;
    };
    fd_ = ::open(options_.path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
        return problem(strerror(errno));
    struct stat st{};
    if (fstat(fd_, &st) != 0 || size_t(st.st_size) < kMinFileSize)
        return problem("file is smaller than any known layout");
    void *p = mmap(nullptr, kMinFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        return problem(std::string("mmap: ") + strerror(errno));
    base_ = static_cast<uint8_t *>(p);
    map_bytes_ = kMinFileSize;
    const uint32_t version = __atomic_load_n(shm_word(base_, kVersionOffset), __ATOMIC_ACQUIRE);
    const Layout *layout = nullptr;
    for (const auto &l : kLayouts)
        if (l.version == version)
            layout = &l;
    if (!layout)
        return problem("unknown layout version " + std::to_string(version) + " (this runtime reads 4 and 5)");
    if (size_t(st.st_size) < layout->file_size)
        return problem("file is smaller than the version-" + std::to_string(version) + " layout");
    if (layout->file_size != map_bytes_) {   // Map the full layout.
        munmap(base_, map_bytes_);
        p = mmap(nullptr, layout->file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
        map_bytes_ = base_ ? layout->file_size : 0;
        if (!base_)
            return problem(std::string("mmap: ") + strerror(errno));
    }
    record_offset_ = layout->record_offset;
    dominant_eye_offset_ = layout->dominant_eye_offset;
    dominant_eye_ = -1;
    if (__atomic_load_n(shm_word(base_, kInitializedOffset), __ATOMIC_ACQUIRE) != 1)
        return problem("not initialized");
    if (!announced_ || !last_problem_.empty())
        LOG.info("eye-server: reading %s (layout version %u)", options_.path.c_str(), version);
    announced_ = true;
    last_problem_.clear();
    version_ = version;
    available_ = true;
    return true;
}

void EyeServerClient::close_map() {
    available_ = false;
    version_ = 0;
    if (base_)
        munmap(base_, map_bytes_);
    base_ = nullptr;
    map_bytes_ = 0;
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

bool EyeServerClient::read_one() {
    uint32_t *seq_word = shm_word(base_, kSequenceOffset);
    if (!lock(base_))
        return false;
    const uint32_t seq = __atomic_load_n(seq_word, __ATOMIC_ACQUIRE);
    __atomic_store_n(shm_word(base_, kRequestedOffset), 1u, __ATOMIC_RELEASE);
    pthread_mutex_unlock(shm_mutex(base_));

    // Do not use FUTEX_PRIVATE because the sequence word is shared with the service process.
    timespec ts{0, 200000000};
    if (syscall(SYS_futex, seq_word, FUTEX_WAIT, seq, &ts, nullptr, 0) != 0 && errno == ETIMEDOUT)
        timeouts_.add();

    uint8_t rec[kRecordBytes], dom[2][kDominantEyeBytes];
    if (!lock(base_))
        return false;
    const bool fresh = __atomic_load_n(seq_word, __ATOMIC_ACQUIRE) != seq;
    if (fresh) {
        memcpy(rec, base_ + record_offset_, kRecordBytes);
        // A client sets the setting without the lock, so read it twice and keep the last value if a change tore it.
        if (dominant_eye_offset_) {
            memcpy(dom[0], base_ + dominant_eye_offset_, kDominantEyeBytes);
            memcpy(dom[1], base_ + dominant_eye_offset_, kDominantEyeBytes);
        }
    }
    pthread_mutex_unlock(shm_mutex(base_));

    Sample s;
    if (!fresh || !parse_record(rec, &s))
        return false;
    if (dominant_eye_offset_ && memcmp(dom[0], dom[1], kDominantEyeBytes) == 0)
        if (auto eye = parse_dominant_eye(dom[0]))
            dominant_eye_ = *eye;
    s.dominant_eye = dominant_eye_;
    s.received_ns = mono_ns();
    s.layout = version_.load(std::memory_order_relaxed);
    s.seq = ++seq_;
    {
        std::lock_guard<std::mutex> l(mutex_);
        latest_ = s;
    }
    {
        std::lock_guard<std::mutex> l(listener_mutex_);
        if (listener_)
            listener_(s);
    }
    samples_.tick();
    return true;
}

void EyeServerClient::run() {
    constexpr uint64_t kRetryNs = 5000000000ull, kReopenAfterNs = 3000000000ull;
    uint64_t next_open = 0, last_sample = 0;
    while (!stop_) {
        if (!wanted_) {
            close_map();   // Do not keep the file open when no consumer needs it.
            sleep_ns(200000000);
            continue;
        }
        const uint64_t now = mono_ns();
        if (!base_) {
            if (now < next_open || !open_map()) {
                if (now >= next_open)
                    next_open = now + kRetryNs;
                sleep_ns(200000000);
                continue;
            }
            last_sample = now;
        }
        if (read_one()) {
            last_sample = mono_ns();
        } else if (mono_ns() - last_sample > kReopenAfterNs) {
            // Map the file again because a SteamVR restart can make the service create a new file.
            reopens_.add();
            close_map();
            next_open = 0;
            sleep_ns(200000000);
        }
    }
}

}
