#include "camera/mouth/frame_mouth_source.h"

#include "camera/v4l2/v4l2_subdev.h"
#include "core/clock.h"
#include "core/log.h"
#include "platform/camera_arbiter_client.h"
#include "platform/steamvr_client.h"

#include <cstdlib>

namespace babble {

namespace {

uint32_t fourcc_from(const std::string &s) {
    if (s.size() != 4)
        return 0;
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

} // namespace

FrameMouthSource::FrameMouthSource(const nlohmann::json &cfg, SourceDeps deps)
    : cfg_(cfg), name_(deps.name), out_(deps.out), stats_(deps.stats), arbiter_(deps.arbiter), steamvr_(deps.steamvr),
      pause_when_absent_(cfg.value("pause_when_absent", true)),
      pairer_(uint64_t(cfg.value("pair_tolerance_us", 4000)) * 1000ull, FrameOrigin::MouthL, FrameOrigin::MouthR),
      limiter_(cfg.value("process_fps", 120.0)),
      ae_(make_exposure_controller(cfg.value("auto_exposure", nlohmann::json::object()))) {
    const nlohmann::json &threads_cfg = deps.threads;
    ae_interval_ = std::max<uint32_t>(1, cfg_.value("auto_exposure", nlohmann::json::object()).value("interval_frames", 8));
    auto backoff = cfg_.value("reopen_backoff_ms", nlohmann::json::object());
    backoff_min_ns_ = uint64_t(backoff.value("min", 500)) * 1000000ull;
    backoff_max_ns_ = uint64_t(backoff.value("max", 5000)) * 1000000ull;

    auto make = [&](const char *side, FrameOrigin origin, const char *thread_key, const char *thread_name) {
        V4l2Capture::Params p;
        p.node = cfg_["nodes"].value(side, "");
        p.width = cfg_["format"].value("width", 400);
        p.height = cfg_["format"].value("height", 400);
        p.fourcc = fourcc_from(cfg_["format"].value("fourcc", "BA81"));
        p.buffers = cfg_.value("buffers", 6);
        p.origin = origin;
        p.thread = thread_spec(threads_cfg, thread_key, thread_name);
        return std::make_unique<V4l2Capture>(p);
    };
    left_ = make("left", FrameOrigin::MouthL, "capL", "bbl-capL");
    right_ = make("right", FrameOrigin::MouthR, "capR", "bbl-capR");

    state_ = cfg_.value("enabled", true) ? State::WaitingForRoute : State::Disabled;

    stats_.add(name_ + ".left_fps", &left_->frames);
    stats_.add(name_ + ".right_fps", &right_->frames);
    stats_.add(name_ + ".pair_fps", &pairs_);
    stats_.add(name_ + ".process_fps", &processed_);
    stats_.add(name_ + ".unmatched_l", &unmatched_l_);
    stats_.add(name_ + ".unmatched_r", &unmatched_r_);
    stats_.add(name_ + ".driver_drops_l", &left_->driver_drops);
    stats_.add(name_ + ".driver_drops_r", &right_->driver_drops);
    stats_.add(name_ + ".restarts", &restarts_);
    stats_.add(name_ + ".skew", &skew_);
    stats_.add(name_ + ".capture_to_pair", &pair_latency_);
}

FrameMouthSource::~FrameMouthSource() {
    shutdown();
    stats_.remove_prefix(name_ + ".");
}

const char *FrameMouthSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::WaitingForRoute: return "WaitingForRoute";
    case State::Streaming: return "Streaming";
    case State::Backoff: return "Backoff";
    case State::Paused: return "Paused";
    }
    return "?";
}

void FrameMouthSource::set_process_fps(double fps) {
    limiter_.set_fps(fps);
    LOG_INFO("%s: process rate %.1f fps", name_.c_str(), fps);
}

void FrameMouthSource::apply_control_set(const char *label, const nlohmann::json &set) {
    if (!controls_.is_open() || !set.is_object())
        return;
    std::vector<std::pair<std::string, int64_t>> values;
    for (auto it = set.begin(); it != set.end(); ++it)
        if (it.value().is_number_integer() || it.value().is_boolean())
            values.emplace_back(it.key(), it.value().is_boolean() ? int64_t(it.value().get<bool>())
                                                                  : it.value().get<int64_t>());
    if (!controls_.set_many(values))
        LOG_WARN("mouth: some %s controls were not applied", label);
}

bool FrameMouthSource::start() {
    // Controls are optional: without the subdev the cameras still stream at
    // whatever exposure the driver holds, which beats no tracking at all.
    std::vector<std::string> prefixes;
    for (const auto &p : cfg_.value("subdev_name_prefixes", nlohmann::json::array()))
        prefixes.push_back(p.get<std::string>());
    std::string entity;
    if (auto node = find_subdev_by_name(prefixes, &entity)) {
        if (controls_.open(*node))
            LOG_INFO("mouth: controls via %s (%s)", node->c_str(), entity.c_str());
    } else {
        LOG_WARN("mouth: no sensor subdev matching the configured names; running without controls");
    }
    apply_control_set("start", cfg_.value("controls_on_start", nlohmann::json::object()));
    // Remember what the strobe set overwrites so stop() can put it back.
    // Some of these pins double as sensor enables (pwm_pad_mode on the
    // primary powers the secondary sensor), so nothing may stay changed.
    saved_strobe_.clear();
    const auto strobe_on = cfg_["strobe"].value("on", nlohmann::json::object());
    for (auto it = strobe_on.begin(); it != strobe_on.end(); ++it)
        if (auto v = controls_.get(it.key()))
            saved_strobe_.emplace_back(it.key(), *v);
    apply_control_set("strobe on", strobe_on);

    pairer_.reset();
    {
        std::lock_guard<std::mutex> lock(pair_mutex_);
        limiter_.reset();
        ae_counter_ = 0;
    }
    lost_ = false;
    FrameSink sink{[this](FramePtr f) { on_frame(std::move(f)); }, [this](std::string_view) { lost_ = true; }};
    if (!left_->open() || !right_->open() || !left_->start(sink) || !right_->start(sink)) {
        last_error_ = !left_->last_error().empty() ? left_->last_error() : right_->last_error();
        stop("start failed");
        return false;
    }
    last_error_.clear();
    LOG_INFO("mouth: streaming (process %.1f fps)", limiter_.fps());
    return true;
}

void FrameMouthSource::stop(const char *why) {
    left_->stop();
    right_->stop();
    pairer_.reset();
    if (controls_.is_open() && !saved_strobe_.empty() && !controls_.set_many(saved_strobe_))
        LOG_WARN("mouth: could not restore all strobe controls");
    saved_strobe_.clear();
    apply_control_set("strobe off", cfg_["strobe"].value("off", nlohmann::json::object()));
    controls_.close();
    LOG_INFO("mouth: stopped (%s)", why);
}

void FrameMouthSource::tick(uint64_t now) {
    const bool routed = arbiter_.state() == ArbiterState::Routed;
    // Only an explicit "not worn" pauses; unknown presence keeps streaming.
    const bool absent = pause_when_absent_ && steamvr_ && steamvr_->worn() == Worn::No;
    // Mirror the pairer's totals into the stats counters (main thread only).
    auto counts = pairer_.counts();
    if (counts.unmatched_left > unmatched_l_.get())
        unmatched_l_.add(counts.unmatched_left - unmatched_l_.get());
    if (counts.unmatched_right > unmatched_r_.get())
        unmatched_r_.add(counts.unmatched_right - unmatched_r_.get());

    switch (state_) {
    case State::Disabled:
        return;
    case State::Paused:
        if (!absent) {
            LOG_INFO("%s: headset worn; resuming", name_.c_str());
            state_ = State::WaitingForRoute;
            retry_at_ns_ = 0;
        }
        return;
    case State::Streaming:
        if (absent) {
            stop("headset not worn");
            state_ = State::Paused;
        } else if (!routed) {
            stop("cameras no longer routed");
            state_ = State::WaitingForRoute;
        } else if (lost_) {
            last_error_ = !left_->last_error().empty() ? left_->last_error() : right_->last_error();
            stop("capture lost");
            restarts_.add();
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
        }
        return;
    case State::WaitingForRoute:
    case State::Backoff:
        if (absent) {
            state_ = State::Paused;
            return;
        }
        if (!routed) {
            state_ = State::WaitingForRoute;
            return;
        }
        if (now < retry_at_ns_)
            return;
        if (start()) {
            state_ = State::Streaming;
            backoff_ns_ = 0;
        } else {
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
        }
        return;
    }
}

void FrameMouthSource::shutdown() {
    if (state_ == State::Streaming)
        stop("shutdown");
    if (state_ != State::Disabled)
        state_ = State::WaitingForRoute;
}

void FrameMouthSource::on_frame(FramePtr frame) {
    auto pair = pairer_.push(std::move(frame));
    if (!pair)
        return;

    uint64_t now = mono_ns();
    pairs_.tick();
    skew_.add(uint64_t(std::llabs(pair->skew_ns)));
    pair_latency_.add(now - std::min(pair->left->capture_ns, pair->right->capture_ns));

    std::lock_guard<std::mutex> lock(pair_mutex_);
    if (!limiter_.accept(now))
        return;
    if (++ae_counter_ >= ae_interval_) {
        ae_counter_ = 0;
        ae_->observe(compute_frame_stats(*pair->right));
        if (auto cmd = ae_->next()) {
            if (cmd->exposure_lines)
                controls_.set("exposure", *cmd->exposure_lines);
            if (cmd->analogue_gain)
                controls_.set("analogue_gain", *cmd->analogue_gain);
        }
    }
    processed_.tick();
    out_.put(std::move(*pair));
}

} // namespace babble
