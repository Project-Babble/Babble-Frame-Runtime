#pragma once

#include "core/threading.h"
#include "frame/frame.h"
#include "frame/mailbox.h"
#include "sink/sink.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace babble {

// Development sink: logs signal snapshots at debug level and optionally dumps
// the first N stereo pairs from each pipeline as PGM files. Pipelines offer
// frames; all disk I/O happens on the sink thread.
class DebugSink : public ISink {
public:
    DebugSink(ThreadSpec thread, double rate_hz, std::filesystem::path dump_dir, int dump_count);
    ~DebugSink() override;

    void start(const ExpressionState &state, StatsRegistry &stats) override;
    void stop() override;

    // Cheap when no dumps are pending. tag: "mouth" or "eye".
    void offer(const char *tag, const StereoFrame &sf);

private:
    struct Channel {
        LatestSlot<StereoFrame> slot;
        std::atomic<int> remaining{0};
        int written = 0;
    };
    void run();
    void write_pgm(const char *tag, Channel &ch, const StereoFrame &sf);

    ThreadSpec thread_spec_;
    double rate_hz_;
    std::filesystem::path dump_dir_;
    Channel mouth_, eye_;
    const ExpressionState *state_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace babble
