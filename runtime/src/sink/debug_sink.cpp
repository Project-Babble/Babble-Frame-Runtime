#include "sink/debug_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "state/expression_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace babble {

DebugSink::DebugSink(ThreadSpec thread, double rate_hz, std::filesystem::path dump_dir, int dump_count)
    : thread_spec_(std::move(thread)), rate_hz_(rate_hz > 0 ? rate_hz : 1), dump_dir_(std::move(dump_dir)) {
    int n = dump_dir_.empty() ? 0 : dump_count;
    mouth_.remaining = n;
    eye_.remaining = n;
    if (n > 0) {
        std::error_code ec;
        std::filesystem::create_directories(dump_dir_, ec);
        LOG_INFO("debug: dumping %d stereo pairs per camera to %s", n, dump_dir_.c_str());
    }
}

DebugSink::~DebugSink() { stop(); }

void DebugSink::start(const ExpressionState &state, StatsRegistry &) {
    state_ = &state;
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void DebugSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void DebugSink::offer(const char *tag, const StereoFrame &sf) {
    Channel &ch = strcmp(tag, "eye") == 0 ? eye_ : mouth_;
    if (ch.remaining.load(std::memory_order_relaxed) > 0)
        ch.slot.put(sf);
}

void DebugSink::write_pgm(const char *tag, Channel &ch, const StereoFrame &sf) {
    // Side by side, left view first; both views share a height.
    const Frame &l = *sf.left, &r = *sf.right;
    uint32_t h = std::min(l.height, r.height), w = l.width + r.width;
    std::vector<uint8_t> img(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++) {
        memcpy(&img[size_t(y) * w], l.data + size_t(y) * l.stride, l.width);
        memcpy(&img[size_t(y) * w + l.width], r.data + size_t(y) * r.stride, r.width);
    }
    char name[64];
    snprintf(name, sizeof(name), "%s_%03d.pgm", tag, ch.written);
    auto path = dump_dir_ / name;
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fprintf(f, "P5\n# capture_ns %llu skew_ns %lld\n%u %u\n255\n", (unsigned long long)sf.capture_ns,
                (long long)sf.skew_ns, w, h);
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
    } else {
        LOG_WARN("debug: cannot write %s", path.c_str());
    }
    ch.written++;
    if (ch.remaining.fetch_sub(1) == 1)
        LOG_INFO("debug: wrote %d %s pairs to %s", ch.written, tag, dump_dir_.c_str());
}

void DebugSink::run() {
    const uint64_t period = uint64_t(1e9 / rate_hz_);
    uint64_t next_log = mono_ns() + period;
    while (!stop_) {
        for (auto [tag, ch] : {std::pair{"mouth", &mouth_}, std::pair{"eye", &eye_}})
            if (ch->remaining > 0)
                if (auto sf = ch->slot.take())
                    write_pgm(tag, *ch, *sf);

        uint64_t now = mono_ns();
        if (now >= next_log && state_ && log::level() <= log::Level::Debug) {
            auto snap = state_->snapshot();
            std::string line;
            char buf[64];
            for (size_t i = 0; i < snap.names.size(); i++) {
                snprintf(buf, sizeof(buf), "%s%s=%.2f", i ? " " : "", snap.names[i].c_str(), snap.signals[i].value);
                line += buf;
            }
            LOG_DEBUG("signals: %s", line.c_str());
            next_log = now + period;
        }
        sleep_ns(5000000);
    }
}

} // namespace babble
