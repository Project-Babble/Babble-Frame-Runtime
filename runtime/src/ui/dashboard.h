#pragma once

#include "core/threading.h"
#include "state/expression_state.h"
#include "ui/dashboard_view.h"
#include "ui/vk_uploader.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace babble {

class SteamVrClient;

namespace ui {

// The "Babble" tab in the SteamVR dashboard, which draws DashboardView on its own thread and turns clicks into
// setting changes.
class Dashboard {
public:
    struct Options {
        std::filesystem::path fonts_dir;
        std::filesystem::path icons_dir;   // The optional folder with the logo for the tab thumbnail and header.
        std::string key = "babble.dashboard";
        std::string name = "Babble";
        float width_m = 1.6f;
        ExpressionState *state = nullptr;   // The source of the eye panel data, or null for no data.
        std::string mouth_tracking_id = "/sources/mouth/tracking";   // The setting that the mouth card switch changes.
        // Fills a newer camera preview and returns true, or returns false if there is none. The dashboard thread
        // calls it only while the camera options page shows.
        std::function<bool(CameraPreview &)> preview;
    };
    ~Dashboard();

    // You can call start() and stop() more than one time.
    void start(SteamVrClient &vr, ThreadSpec thread, Options options, std::vector<Choice> choices);
    void stop();
    bool started() const { return thread_.joinable(); }
    // Frees the GPU objects, which OpenVR requires after SteamVR has shut down.
    void release_gpu();

    // Call these functions from the main thread only.
    void set_status(const Status &s);
    void set_choice(const std::string &id, double value);
    // Returns the settings that changed on the panel since the last call, as pairs of JSON pointer and new value.
    std::vector<std::pair<std::string, nlohmann::json>> take_changes();
    // Returns true one time for each click on the visualizer button.
    bool take_visualizer_request() { return visualizer_request_.exchange(false); }
    // Returns true if the user clicked and confirmed the Restart Runtime button.
    bool take_restart_request() { return restart_request_.exchange(false); }
    // Returns true if the runtime must read the USB camera list again.
    bool take_camera_refresh_request() { return camera_refresh_request_.exchange(false); }
    // Returns 1 for verbose logging on, 0 for off, or -1 for no change.
    int take_verbose_request() { return verbose_request_.exchange(-1); }
    bool take_diagnostics_request() { return diagnostics_request_.exchange(false); }
    // Returns the button that the user clicked on the dataset capture row, or None.
    DashboardView::ClickResult::CaptureAction take_capture_request() {
        return DashboardView::ClickResult::CaptureAction(capture_request_.exchange(0));
    }
    // Returns true if the user opened the Diagnostics page, so that the runtime can check for an app update.
    bool take_capture_check_request() { return capture_check_request_.exchange(false); }
    // Returns true if the user clicked the update pill.
    bool take_update_request() { return update_request_.exchange(false); }

private:
    void run();
    bool create_overlays();
    void destroy_overlays();
    void present();
    // Reads the eye panel values from ExpressionState and changes the gaze to degrees.
    std::array<EyeReading, 2> read_eyes() const;
    // The number of signals for each eye.
    static constexpr int kPerEye = 8;
    std::array<SignalId, 2 * kPerEye> eye_ids_{};
    std::array<EyeReading, 2> drawn_eyes_{};
    uint64_t last_present_ns_ = 0;
    CameraPreview preview_;          // Only the dashboard thread uses this.
    uint64_t last_preview_ns_ = 0;

    SteamVrClient *vr_ = nullptr;
    ThreadSpec thread_spec_;
    Options options_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;   // This mutex guards status_, choices_, changes_ and dirty_.
    Status status_;
    std::vector<Choice> choices_;
    std::vector<std::pair<std::string, nlohmann::json>> changes_;
    bool dirty_ = true;
    std::atomic<bool> visualizer_request_{false};
    std::atomic<bool> restart_request_{false};
    std::atomic<bool> camera_refresh_request_{false};
    std::atomic<int> verbose_request_{-1};
    std::atomic<bool> diagnostics_request_{false};
    std::atomic<int> capture_request_{0};
    std::atomic<bool> capture_check_request_{false};
    std::atomic<bool> update_request_{false};
    bool drawn_restart_armed_ = false;

    // Only the dashboard thread uses these members.
    DashboardView view_;
    Canvas canvas_{DashboardView::kWidth, DashboardView::kHeight};
    VkUploader gpu_;
    bool gpu_failed_ = false;
    uint64_t connection_ = 0;
    uint64_t main_ = 0, thumb_ = 0;
    bool visible_ = false;
    std::optional<Point> pointer_;
    int hover_ = -1;   // The value of view_.hit_at(pointer_) at the last draw.
    // The text setting that the SteamVR keyboard edits, or "" if the keyboard is closed.
    std::string editing_id_;
    bool editing_numeric_ = false;
    void open_keyboard(const std::string &id);
    void apply_output_setting(const std::string &id, const nlohmann::json &value);   // Call this with mutex_ held.
};

}
}
