#pragma once

#include "core/threading.h"
#include "frame/frame.h"
#include "frame/mailbox.h"
#include "sink/sink.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace babble {

// Config type "debug" logs the signals and can write the first N stereo frames of each source as PGM files.
class DebugSink : public ISink {
public:
    DebugSink(const nlohmann::json &cfg, SinkDeps deps);
    ~DebugSink() override;

    bool start() override;
    void stop() override;

    bool wants_frames() const override { return dump_count_ > 0; }
    void offer_frames(const std::string &source, const StereoFrame &frames) override;

private:
    struct Channel {
        LatestSlot<StereoFrame> slot;
        std::atomic<int> remaining{0};
        int written = 0;
    };
    Channel &channel(const std::string &source);
    void run();
    void write_pgm(const std::string &source, Channel &ch, const StereoFrame &sf);

    std::string name_;
    ThreadSpec thread_spec_;
    const ExpressionState &state_;
    double rate_hz_;
    std::filesystem::path dump_dir_;
    int dump_count_ = 0;

    std::mutex channels_mutex_;
    std::map<std::string, std::unique_ptr<Channel>> channels_;
    std::atomic<int> pending_{0};   // The number of channels that still want frames.
    std::atomic<bool> any_channel_{false};

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}
