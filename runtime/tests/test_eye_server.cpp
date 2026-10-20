#include <doctest/doctest.h>

#include "core/clock.h"
#include "platform/eye_server_client.h"

#include <atomic>
#include <climits>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace babble;
using namespace babble::eye_server;

namespace {

template <typename T> void put(uint8_t *p, size_t off, T v) { memcpy(p + off, &v, sizeof(v)); }

// Writes a record in which each value identifies its own field.
void write_record(uint8_t *r, uint32_t state) {
    put<uint32_t>(r, rec::kProducerState, state);
    put<double>(r, rec::kSampleTime, 123.5);
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 3; k++) {
            put<float>(r, rec::kGaze + 4 * (3 * e + k), float(10 * e + k));
            put<float>(r, rec::kPreFusionGaze + 4 * (3 * e + k), float(100 + 10 * e + k));
        }
    for (int k = 0; k < 3; k++)
        put<float>(r, rec::kFixation + 4 * k, float(200 + k));
    put<float>(r, rec::kOpenness, 0.25f);
    put<float>(r, rec::kOpenness + 4, 0.75f);
}

// Acts as the eye service with a version-N file and a robust process-shared mutex.
struct FakeService {
    std::string path;
    int fd = -1;
    uint8_t *base = nullptr;
    size_t size = 0x4f21f, record = 0x157;   // An unknown version gets the version-5 shape.

    explicit FakeService(uint32_t version) {
        for (const auto &l : kLayouts)
            if (l.version == version) {
                size = l.file_size;
                record = l.record_offset;
            }
        path = (std::filesystem::temp_directory_path() / ("babble_eyesrv_" + std::to_string(getpid()) + "_" +
                                                          std::to_string(version)))
                   .string();
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        REQUIRE(fd >= 0);
        REQUIRE(ftruncate(fd, off_t(size)) == 0);
        base = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        REQUIRE(base != MAP_FAILED);
        put<uint32_t>(base, kVersionOffset, version);
        put<uint32_t>(base, kInitializedOffset, 1);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(mutex(), &a);
        pthread_mutexattr_destroy(&a);
    }
    ~FakeService() {
        munmap(base, size);
        close(fd);
        unlink(path.c_str());
    }
    pthread_mutex_t *mutex() { return reinterpret_cast<pthread_mutex_t *>(base + kMutexOffset); }
    uint32_t *word(size_t off) { return reinterpret_cast<uint32_t *>(base + off); }

    // Writes a record, increments the sequence and wakes the waiters, as the service does.
    void produce(uint32_t state) {
        pthread_mutex_lock(mutex());
        write_record(base + record, state);
        __atomic_add_fetch(word(kSequenceOffset), 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(mutex());
        syscall(SYS_futex, word(kSequenceOffset), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }
    uint32_t requested() { return __atomic_load_n(word(kRequestedOffset), __ATOMIC_ACQUIRE); }
    // Sets "Track Dominant Eye Only" at the version-5 offset, as the headset's settings do.
    void set_dominant_eye(uint8_t on, int32_t eye) {
        base[0x152] = on;
        put<int32_t>(base, 0x153, eye);
    }
};

std::array<uint8_t, kDominantEyeBytes> dominant_bytes(uint8_t on, int32_t eye) {
    std::array<uint8_t, kDominantEyeBytes> b{on};
    put<int32_t>(b.data(), 1, eye);
    return b;
}

// Runs the producer until the client has a sample or the timeout passes.
std::optional<Sample> wait_for_sample(FakeService &svc, EyeServerClient &c, uint32_t state, uint64_t timeout_ns) {
    const uint64_t end = mono_ns() + timeout_ns;
    while (mono_ns() < end) {
        svc.produce(state);
        if (auto s = c.latest(1000000000ull))
            return s;
        sleep_ns(10000000);
    }
    return std::nullopt;
}

}

TEST_CASE("an eye-service record decodes field by field") {
    std::vector<uint8_t> r(kRecordBytes, 0);
    write_record(r.data(), 1);
    Sample s;
    REQUIRE(parse_record(r.data(), &s));
    CHECK(s.sample_time == 123.5);
    CHECK(s.gaze[0] == Vec3{0, 1, 2});
    CHECK(s.gaze[1] == Vec3{10, 11, 12});
    CHECK(s.pre_fusion_gaze[0] == Vec3{100, 101, 102});
    CHECK(s.pre_fusion_gaze[1] == Vec3{110, 111, 112});
    CHECK(s.fixation == Vec3{200, 201, 202});
    CHECK(s.openness[0] == 0.25f);
    CHECK(s.openness[1] == 0.75f);

    write_record(r.data(), 0);   // The producer marks the record as invalid.
    CHECK_FALSE(parse_record(r.data(), &s));
    write_record(r.data(), 1);
    put<float>(r.data(), rec::kOpenness, std::nanf(""));
    CHECK_FALSE(parse_record(r.data(), &s));
}

TEST_CASE("the client follows the service's protocol and publishes samples") {
    FakeService svc(4);
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    CHECK_FALSE(c.latest(1000000000ull));   // No consumer wants the data yet.
    c.want(EyeServerClient::Gaze, true);
    auto s = wait_for_sample(svc, c, 1, 3000000000ull);
    REQUIRE(s);
    CHECK(c.available());
    CHECK(s->pre_fusion_gaze[1] == Vec3{110, 111, 112});
    CHECK(s->openness[1] == 0.75f);
    CHECK(svc.requested() == 1);   // The client asked the service for records.
    c.stop();
}

TEST_CASE("the recorder consumer gets every published sample through the listener") {
    FakeService svc(4);
    EyeServerClient c;
    std::atomic<int> heard{0};
    std::atomic<uint64_t> last_seq{0};
    c.set_listener([&](const Sample &s) {
        heard++;
        last_seq = s.seq;
    });
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    CHECK(c.layout_version() == 0);   // The client has not opened a file yet.
    c.want(EyeServerClient::Record, true);   // The recorder alone keeps the reader active.
    auto s = wait_for_sample(svc, c, 1, 3000000000ull);
    REQUIRE(s);
    CHECK(c.layout_version() == 4);
    CHECK(s->layout == 4);
    CHECK(heard.load() >= 1);
    CHECK(last_seq.load() == c.latest(1000000000ull)->seq);
    c.set_listener(nullptr);
    c.stop();
}

TEST_CASE("layout version 5: the record sits 5 bytes later") {
    FakeService svc(5);
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    c.want(EyeServerClient::Gaze, true);
    auto s = wait_for_sample(svc, c, 1, 3000000000ull);
    REQUIRE(s);
    CHECK(c.layout_version() == 5);
    CHECK(s->layout == 5);
    CHECK(s->pre_fusion_gaze[1] == Vec3{110, 111, 112});
    CHECK(s->openness[1] == 0.75f);
    c.stop();
}

TEST_CASE("an unknown layout version is left alone") {
    FakeService svc(6);
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    c.want(EyeServerClient::Openness, true);
    CHECK_FALSE(wait_for_sample(svc, c, 1, 500000000ull));
    CHECK_FALSE(c.available());
    CHECK(svc.requested() == 0);   // The client never wrote to the file.
    c.stop();
}

TEST_CASE("records the producer marks invalid are not published") {
    FakeService svc(4);
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    c.want(EyeServerClient::Gaze, true);
    CHECK_FALSE(wait_for_sample(svc, c, 0, 500000000ull));
    CHECK(c.available());
    c.stop();
}

TEST_CASE("the dominant-eye setting decodes and rejects values it does not know") {
    CHECK(parse_dominant_eye(dominant_bytes(0, -1).data()) == -1);
    CHECK(parse_dominant_eye(dominant_bytes(0, 1).data()) == -1);   // Off, with the eye that was chosen last.
    CHECK(parse_dominant_eye(dominant_bytes(1, 0).data()) == kLeftEye);
    CHECK(parse_dominant_eye(dominant_bytes(1, 1).data()) == kRightEye);
    CHECK_FALSE(parse_dominant_eye(dominant_bytes(1, -1).data()));   // A read torn by a change.
    CHECK_FALSE(parse_dominant_eye(dominant_bytes(2, 0).data()));
    CHECK_FALSE(parse_dominant_eye(dominant_bytes(0, 7).data()));
}

TEST_CASE("layout version 5: samples carry the Track Dominant Eye Only setting") {
    FakeService svc(5);
    svc.set_dominant_eye(1, 1);
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    c.want(EyeServerClient::Gaze, true);
    auto s = wait_for_sample(svc, c, 1, 3000000000ull);
    REQUIRE(s);
    CHECK(s->dominant_eye == kRightEye);

    svc.set_dominant_eye(1, -1);   // A value the client does not know keeps the last one.
    uint64_t seq = s->seq;
    for (int i = 0; i < 300 && s->seq == seq; i++) {
        svc.produce(1);
        sleep_ns(10000000);
        s = c.latest(1000000000ull);
        REQUIRE(s);
    }
    CHECK(s->dominant_eye == kRightEye);

    svc.set_dominant_eye(0, -1);
    for (int i = 0; i < 300 && s->dominant_eye != -1; i++) {
        svc.produce(1);
        sleep_ns(10000000);
        s = c.latest(1000000000ull);
        REQUIRE(s);
    }
    CHECK(s->dominant_eye == -1);
    c.stop();
}

TEST_CASE("layout version 4 has no dominant-eye setting and reads the record bytes as nothing") {
    FakeService svc(4);
    svc.set_dominant_eye(1, 0);   // In version 4 these bytes are the start of the record.
    EyeServerClient c;
    c.start(ThreadSpec{"t-eyesrv", {}, 0}, {svc.path}, nullptr);
    c.want(EyeServerClient::Gaze, true);
    auto s = wait_for_sample(svc, c, 1, 3000000000ull);
    REQUIRE(s);
    CHECK(s->dominant_eye == -1);
    c.stop();
}
