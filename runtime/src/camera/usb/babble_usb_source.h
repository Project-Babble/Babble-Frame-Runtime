#pragma once

#include "camera/tracking_source.h"
#include "camera/usb/usb_device.h"
#include "camera/usb/uvc.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"
#include "frame/rate_limiter.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

// A USB mouth camera that the source streams through usbfs, because the kernel of the Frame has no uvcvideo
// driver. Each camera has its own settings under "cameras", keyed by usb::device_id().
class BabbleUsbSource : public ITrackingSource {
public:
    enum class State { Disabled, Waiting, Streaming, Backoff, Paused };

    // The settings of one camera. The mode is what the user asked for, and the camera streams the nearest mode it has.
    struct CameraSettings {
        uvc::Mode mode;
        int rotate = 0;   // Clockwise, in degrees: 0, 90, 180 or 270.
        bool flip_h = false, flip_v = false;
        bool operator==(const CameraSettings &) const = default;
    };
    // Returns the settings of a camera, with the defaults for the keys that it does not have.
    CameraSettings settings_for(const usb::Found &dev) const;
    static nlohmann::json to_json(const CameraSettings &s);
    // Flips and then rotates (clockwise) a w x h grayscale frame in scratch[0] into dst, and sets the size of the
    // result. It uses scratch[1] as a work buffer, and it is public for the tests.
    static void transform(std::vector<uint8_t> (&scratch)[2], int w, int h, int rotate, bool flip_h, bool flip_v,
                          uint8_t *dst, int *out_w, int *out_h);

    // A small copy of the latest frame after the rotation and flips, for the camera options page.
    struct Preview {
        std::string camera;   // The usb::device_id() of the camera, or empty when no camera streams.
        int width = 0, height = 0;
        std::vector<uint8_t> gray;
        uint64_t seq = 0;     // This goes up for each new preview.
    };
    static constexpr int kPreviewWidth = 400, kPreviewHeight = 300, kPreviewFps = 15;
    // Copies the latest preview into out if it is newer than out.seq. A call also asks the camera thread to make
    // previews for the next second, so the camera thread makes none while nobody looks. Any thread can call it.
    bool take_preview(Preview &out);

    BabbleUsbSource(const nlohmann::json &cfg, SourceDeps deps);
    ~BabbleUsbSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool tracking() const override { return tracking_; }
    bool streaming() const override { return state_ == State::Streaming; }
    bool set_option(const std::string &key, const nlohmann::json &value) override;
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }
    // Returns the camera setting, which is empty for the automatic choice.
    const std::string &camera() const { return camera_; }
    const std::string &active_camera() const { return active_id_; }
    SourceActivity activity() const override;
    const char *state_name() const override;
    const std::string &last_error() const override { return last_error_; }

private:
    // Returns the camera for the setting, or nullopt if that camera is not plugged in.
    std::optional<usb::Found> pick() const;
    bool start(const usb::Found &dev);
    void stop(const char *why);
    void run(usb::Found dev, uvc::Mode mode);
    // Applies one camera setting, such as "cameras/<id>" with an object or "cameras/<id>/rotate" with a value.
    bool set_camera_option(const std::string &key, const nlohmann::json &value);

    std::string name_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // Only the main thread uses this.
    std::string camera_;       // Only the main thread uses this.
    std::string active_id_;    // The main thread sets this on start and clears it on stop.
    uint16_t vendor_ = 0x303a, product_ = 0x8000;
    uint16_t width_ = 240, height_ = 240;
    double fps_ = 30;
    bool tracker_flip_h_ = true;   // The default horizontal flip of the Babble tracker.
    nlohmann::json cameras_;       // The "cameras" settings, which only the main thread uses.
    usb::Found active_dev_;        // The camera that streams, which the main thread sets on start.
    uvc::Mode active_mode_;        // The mode that the streaming camera was asked for.
    // The transform of the streaming camera, which the camera thread reads for each frame.
    std::atomic<int> rotate_{0};
    std::atomic<bool> flip_h_{false}, flip_v_{false};
    std::vector<uint8_t> scratch_[2];   // Only the camera thread uses these.
    size_t pool_pixels_ = 0;
    void make_preview(const std::string &camera, const uint8_t *gray, int w, int h);
    void clear_preview();
    std::mutex preview_mutex_;
    Preview preview_;
    std::atomic<uint64_t> preview_until_ns_{0};
    uint64_t preview_next_ns_ = 0;   // Only the camera thread uses this.

    std::shared_ptr<FramePool> pool_;
    RateLimiter limiter_;
    std::atomic<IFrameTap *> tap_{nullptr};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> opened_{false};   // True after the thread opened the camera.
    std::string thread_error_;   // The thread writes this before it sets failed_.

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    bool logged_waiting_ = false;

    RateStat frames_, processed_;
    Counter restarts_, pool_exhausted_, undecodable_, reassembly_dropped_;
    LatencyStat decode_time_;
};

}
