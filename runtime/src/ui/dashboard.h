#pragma once

#include "core/threading.h"
#include "state/expression_state.h"
#include "ui/dashboard_view.h"
#include "ui/vk_uploader.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace babble {

class SteamVrClient;

namespace ui {

// The "Babble" tab in the SteamVR dashboard, on the runtime's shared SteamVR
// connection. Draws DashboardView on its own thread, only while the tab is
// open (plus a slow refresh), and turns laser clicks into setting changes the
// main thread applies.
class Dashboard {
public:
    struct Options {
        std::filesystem::path fonts_dir;
        std::filesystem::path icons_dir;   // the logo (tab thumbnail and header); optional
        std::string key = "babble.dashboard";
        std::string name = "Babble";
        float width_m = 1.6f;
        ExpressionState *state = nullptr;   // the eye panel's source; null = no panel data
    };
    ~Dashboard();

    // start()/stop() may repeat (the debug-overlay setting switches the tab live).
    void start(SteamVrClient &vr, ThreadSpec thread, Options options, std::vector<Choice> choices);
    void stop();
    bool started() const { return thread_.joinable(); }
    // Frees the GPU objects; OpenVR requires this after SteamVR has shut down.
    void release_gpu();

    // Main thread.
    void set_status(const Status &s);
    void set_choice(const std::string &id, double value);   // e.g. after a config reload
    // Settings changed on the panel since the last call: (JSON pointer, new
    // value), e.g. ("/sources/mouth/process_fps", 60), ("/sources/eye/tracking", false).
    std::vector<std::pair<std::string, nlohmann::json>> take_changes();
    // True once per click on the visualizer button since the last call.
    bool take_visualizer_request() { return visualizer_request_.exchange(false); }
    // The Restart Runtime button was clicked (and confirmed).
    bool take_restart_request() { return restart_request_.exchange(false); }
    // The USB camera list should be read again (Refresh, or its page opened).
    bool take_camera_refresh_request() { return camera_refresh_request_.exchange(false); }
    // The Diagnostics page: verbose logging switched (1 on, 0 off, -1 no change), a file wanted.
    int take_verbose_request() { return verbose_request_.exchange(-1); }
    bool take_diagnostics_request() { return diagnostics_request_.exchange(false); }

private:
    void run();
    bool create_overlays();
    void destroy_overlays();
    void present();
    // The eye panel's values from ExpressionState (gaze at +-45 deg = +-1 -> degrees).
    std::array<EyeReading, 2> read_eyes() const;
    // Per eye (left, right): X, Y, Lid, Widen, raw lid, neutral, wide ceiling, dilation.
    static constexpr int kPerEye = 8;
    std::array<SignalId, 2 * kPerEye> eye_ids_{};
    std::array<EyeReading, 2> drawn_eyes_{};
    uint64_t last_present_ns_ = 0;

    SteamVrClient *vr_ = nullptr;
    ThreadSpec thread_spec_;
    Options options_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;   // guards status_, choices_, changes_, dirty_
    Status status_;
    std::vector<Choice> choices_;
    std::vector<std::pair<std::string, nlohmann::json>> changes_;
    bool dirty_ = true;
    std::atomic<bool> visualizer_request_{false};
    std::atomic<bool> restart_request_{false};
    std::atomic<bool> camera_refresh_request_{false};
    std::atomic<int> verbose_request_{-1};
    std::atomic<bool> diagnostics_request_{false};
    bool drawn_restart_armed_ = false;

    // Dashboard thread only.
    DashboardView view_;
    Canvas canvas_{DashboardView::kWidth, DashboardView::kHeight};
    VkUploader gpu_;
    bool gpu_failed_ = false;
    uint64_t connection_ = 0;
    uint64_t main_ = 0, thumb_ = 0;   // overlay handles
    bool visible_ = false;
    std::optional<Point> pointer_;
    int hover_ = -1;   // view_.hit_at(pointer_) at the last draw
};

} // namespace ui
} // namespace babble
