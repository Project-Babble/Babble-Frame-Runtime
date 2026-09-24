#pragma once

#include "core/threading.h"
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
        std::string key = "babble.dashboard";
        std::string name = "Babble";
        float width_m = 1.6f;
    };
    ~Dashboard();

    void start(SteamVrClient &vr, ThreadSpec thread, Options options, std::vector<Choice> choices);
    void stop();
    // Frees the GPU objects; OpenVR requires this after SteamVR has shut down.
    void release_gpu();

    // Main thread.
    void set_status(const Status &s);
    void set_choice(const std::string &id, double value);   // e.g. after a config reload
    // Settings changed on the panel since the last call: (JSON pointer, new
    // value), e.g. ("/sources/mouth/process_fps", 60), ("/sources/eye/tracking", false).
    std::vector<std::pair<std::string, nlohmann::json>> take_changes();

private:
    void run();
    bool create_overlays();
    void destroy_overlays();
    void present();

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
