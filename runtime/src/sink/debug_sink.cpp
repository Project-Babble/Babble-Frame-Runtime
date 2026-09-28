#include "sink/debug_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "state/expression_state.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
const babble::log::Logger LOG("debug");
} // namespace

namespace babble {

DebugSink::DebugSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "sink"), "bbl-sink")),
      state_(deps.state), rate_hz_(cfg.value("rate_hz", 2.0)) {
    if (rate_hz_ <= 0)
        rate_hz_ = 1;
    if (cfg.contains("dump_dir") && cfg["dump_dir"].is_string()) {
        dump_dir_ = cfg["dump_dir"].get<std::string>();
        dump_count_ = cfg.value("dump_count", 0);
    }
    if (dump_count_ > 0) {
        std::error_code ec;
        std::filesystem::create_directories(dump_dir_, ec);
        LOG.info("%s: dumping %d stereo frames per source to %s", name_.c_str(), dump_count_, dump_dir_.c_str());
    }
}

DebugSink::~DebugSink() { stop(); }

bool DebugSink::start() {
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    return true;
}

void DebugSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

DebugSink::Channel &DebugSink::channel(const std::string &source) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    auto &ch = channels_[source];
    if (!ch) {
        ch = std::make_unique<Channel>();
        ch->remaining = dump_count_;
        pending_++;
        any_channel_ = true;
    }
    return *ch;
}

void DebugSink::offer_frames(const std::string &source, const StereoFrame &sf) {
    if (dump_count_ <= 0)
        return;
    Channel &ch = channel(source);
    if (ch.remaining.load(std::memory_order_relaxed) > 0)
        ch.slot.put(sf);
}

void DebugSink::write_pgm(const std::string &source, Channel &ch, const StereoFrame &sf) {
    // Side by side, left view first; both views share a height.
    const Frame &l = *sf.left, &r = *sf.right;
    uint32_t h = std::min(l.height, r.height), w = l.width + r.width;
    std::vector<uint8_t> img(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++) {
        memcpy(&img[size_t(y) * w], l.data + size_t(y) * l.stride, l.width);
        memcpy(&img[size_t(y) * w + l.width], r.data + size_t(y) * r.stride, r.width);
    }
    char file[128];
    snprintf(file, sizeof(file), "%s_%03d.pgm", source.c_str(), ch.written);
    auto path = dump_dir_ / file;
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fprintf(f, "P5\n# capture_ns %llu skew_ns %lld\n%u %u\n255\n", (unsigned long long)sf.capture_ns,
                (long long)sf.skew_ns, w, h);
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
    } else {
        LOG.warn("%s: cannot write %s", name_.c_str(), path.c_str());
    }
    ch.written++;
    if (ch.remaining.fetch_sub(1) == 1) {
        pending_--;
        LOG.info("%s: wrote %d %s frames to %s", name_.c_str(), ch.written, source.c_str(), dump_dir_.c_str());
    }
}

void DebugSink::run() {
    const uint64_t period = uint64_t(1e9 / rate_hz_);
    uint64_t next_log = mono_ns() + period;
    while (!stop_) {
        if (pending_ > 0) {
            std::vector<std::pair<std::string, Channel *>> chans;
            {
                std::lock_guard<std::mutex> lock(channels_mutex_);
                for (auto &[src, ch] : channels_)
                    chans.emplace_back(src, ch.get());
            }
            for (auto &[src, ch] : chans)
                if (ch->remaining > 0)
                    if (auto sf = ch->slot.take())
                        write_pgm(src, *ch, *sf);
        }

        uint64_t now = mono_ns();
        if (now >= next_log && LOG.debug_enabled()) {
            auto snap = state_.snapshot();
            std::string line;
            char buf[64];
            for (size_t i = 0; i < snap.names.size(); i++) {
                snprintf(buf, sizeof(buf), "%s%s=%.2f", i ? " " : "", snap.names[i].c_str(), snap.signals[i].value);
                line += buf;
            }
            LOG.debug("signals: %s", line.c_str());
            next_log = now + period;
        }
        // Poll quickly only while dumps are pending (or may start); otherwise the log rate.
        bool dumping = dump_count_ > 0 && (pending_ > 0 || !any_channel_);
        sleep_ns(dumping ? 5000000ull : std::min<uint64_t>(period, 200000000ull));
    }
}

} // namespace babble
