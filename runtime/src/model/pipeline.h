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
#include <string>
#include <thread>
#include <vector>

namespace babble {

class EyeServerClient;
class ISink;
class SteamVrClient;

// A model pipeline module: takes the latest frames from its source's slot,
// runs its model(s) with the model-specific pre- and post-processing, and
// writes named signals into ExpressionState. Registered by config "type" in
// make_pipeline(): "fusionnet_mouth", "babble_eye", "steamvr_gaze", "null".
class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual bool init() = 0;    // load models, validate tensors, register signals; false = drop it
    virtual void start() = 0;
    virtual void stop() = 0;
    // Main thread, when SteamVR reports the headset put on (true) or taken
    // off (false): publish neutral values on removal and reset per-session
    // state (calibration, filters) before the next session's first frame.
    virtual void on_presence(bool worn) {}
    // For status: the source this pipeline reads ("" if none) and how many
    // frames it has processed so far (the tracking rate is its derivative).
    virtual const std::string &source() const;
    virtual uint64_t processed() const { return 0; }
    // The user-facing feed this pipeline belongs to ("mouth", "eye"): the
    // dashboard's on/off for that feed also switches this pipeline. Defaults
    // to the source it reads; config "feed" overrides (gaze reads SteamVR but
    // is part of the eye feed).
    virtual const std::string &feed() const { return source(); }
    // Feed turned off (false) or back on (true) by the user. Off publishes
    // neutral values and resets per-session state, like taking the headset off.
    virtual void set_tracking(bool on) {
        if (!on)
            on_presence(false);
    }
    // The config key ("eye", "gaze"); live options address the pipeline by it.
    virtual const std::string &name() const = 0;
    // A user-facing option changed live (SteamVR Settings, config reload).
    // False = not an option this pipeline has, or a value it rejects.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }
};

struct PipelineDeps {
    std::string name;                         // config key, also the stats prefix ("pipe.<name>.")
    const nlohmann::json &threads;            // config["threads"]
    std::function<LatestSlot<StereoFrame> *(const std::string &source)> slot;   // null if not configured
    ExpressionState &state;
    StatsRegistry &stats;
    std::vector<ISink *> frame_taps;          // output modules that asked for camera frames
    std::filesystem::path models_dir;         // relative "model" paths resolve here
    SteamVrClient *steamvr = nullptr;         // shared SteamVR connection (gaze, presence); may be null
    EyeServerClient *eye_server = nullptr;    // SteamVR's eye-service shared memory; may be null
};

std::unique_ptr<IPipeline> make_pipeline(const nlohmann::json &cfg, PipelineDeps deps);

// Shared plumbing: one pinned thread that waits for the newest frame from the
// input slot, hands it to frame taps, and calls process(). Subclasses do the
// model work.
class PipelineBase : public IPipeline {
public:
    ~PipelineBase() override;
    void start() override;
    void stop() override;
    const std::string &source() const override { return source_; }
    const std::string &feed() const override { return feed_.empty() ? source_ : feed_; }
    const std::string &name() const override { return name_; }
    uint64_t processed() const override { return rate_.total(); }

protected:
    PipelineBase(const nlohmann::json &cfg, PipelineDeps deps, const char *default_thread_key);

    // Resolves the input slot from cfg["from"] (or cfg["inputs"][0]["from"]); logs on failure.
    bool bind_input();
    std::filesystem::path model_path(const std::string &key) const;

    virtual void process(const StereoFrame &frames) = 0;   // pipeline thread
    virtual void on_start() {}
    virtual void on_stop() {}

    std::string name_;
    nlohmann::json cfg_;
    PipelineDeps deps_;
    ThreadSpec thread_spec_;
    std::string source_, feed_;
    LatestSlot<StereoFrame> *input_ = nullptr;

    RateStat rate_;
    LatencyStat preprocess_, inference_, capture_to_state_;

private:
    void run();
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool stats_registered_ = false;
};

} // namespace babble
