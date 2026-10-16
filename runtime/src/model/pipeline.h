#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame.h"
#include "frame/mailbox.h"
#include "state/expression_state.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <span>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class EyeServerClient;
class ISink;
class SteamVrClient;

// A pipeline runs its models on the latest frames from its source and writes named signals into ExpressionState.
class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual bool init() = 0;    // A false return value removes the pipeline.
    virtual void start() = 0;
    virtual void stop() = 0;
    // The main thread calls this when SteamVR reports that the wearer put the headset on or took it off.
    virtual void on_presence(bool worn) {}
    // The status shows the source of this pipeline and the number of frames that it processed.
    virtual const std::string &source() const;
    virtual uint64_t processed() const { return 0; }
    // The dashboard switch for this feed also switches this pipeline on or off.
    virtual const std::string &feed() const { return source(); }
    // If the user turns the feed off, the pipeline acts as if the headset is off.
    virtual void set_tracking(bool on) {
        if (!on)
            on_presence(false);
    }
    // Live options use this config key to find the pipeline.
    virtual const std::string &name() const = 0;
    // Return false if the pipeline does not have the option or does not accept the value.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }
};

struct PipelineDeps {
    std::string name;                         // The config key is also the stats prefix "pipe.<name>.".
    const nlohmann::json &threads;            // This is config["threads"].
    std::function<LatestSlot<StereoFrame> *(const std::string &source)> slot;   // This returns null for an unknown source.
    ExpressionState &state;
    StatsRegistry &stats;
    std::vector<ISink *> frame_taps;          // These output modules get the camera frames.
    std::filesystem::path models_dir;         // Relative "model" paths start from this directory.
    SteamVrClient *steamvr = nullptr;         // The shared SteamVR connection can be null.
    EyeServerClient *eye_server = nullptr;    // The SteamVR eye-service shared memory can be null.
};

std::unique_ptr<IPipeline> make_pipeline(const nlohmann::json &cfg, PipelineDeps deps);

// One pinned thread waits for the newest frame, gives it to the frame taps and calls process().
class PipelineBase : public IPipeline {
public:
    ~PipelineBase() override;
    void start() override;
    void stop() override;
    const std::string &source() const override { return source_; }
    const std::string &feed() const override { return feed_.empty() ? source_ : feed_; }
    const std::string &name() const override { return name_; }
    uint64_t processed() const override { return rate_.total(); }

    // Hold the pipeline after neutral() so that a late frame cannot overwrite the neutral values.
    void on_presence(bool worn) override;
    void set_tracking(bool on) override;

protected:
    PipelineBase(const nlohmann::json &cfg, PipelineDeps deps, const char *default_thread_key);

    // Find the input slot from cfg["from"] or cfg["inputs"][0]["from"].
    bool bind_input();
    std::filesystem::path model_path(const std::string &key) const;

    virtual void process(const StereoFrame &frames) = 0;   // The pipeline thread calls this.
    virtual void on_start() {}
    virtual void on_stop() {}
    // The pipeline publishes these values when it holds.
    virtual std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now_ns) const { return {}; }
    // The main thread calls this when a hold starts, so set a flag for the pipeline thread to reset its state.
    virtual void reset_session() {}
    // The pipeline thread publishes the updates, but drops them during a hold.
    void publish(std::span<const std::pair<SignalId, Signal>> updates);

    std::string name_;
    nlohmann::json cfg_;
    PipelineDeps deps_;
    ThreadSpec thread_spec_;
    std::string source_, feed_;
    LatestSlot<StereoFrame> *input_ = nullptr;

    RateStat rate_;
    LatencyStat preprocess_, inference_, capture_to_state_;

private:
    enum Hold : uint8_t { kHeadsetOff = 1, kFeedOff = 2 };
    void hold(Hold reason, bool on);
    std::mutex publish_mutex_;   // This mutex keeps publish() in order with the neutral values.
    uint8_t held_ = 0;           // Lock publish_mutex_ before you use the Hold bits.

    void run();
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool stats_registered_ = false;
};

}
