#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "sink/sink.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace babble {

namespace camera_stream {

// The views, as ~/streamer/stream_daemon.py numbers them.
enum View : uint8_t { FaceL = 0, FaceR = 1, EyeL = 2, EyeR = 3, kViews = 4 };
const char *view_name(View v);

// One grayscale image to send. Eye views are cropped to the 400x400
// geometry the streamer's recordings use (from the DSP's 512x512).
struct Image {
    std::vector<uint8_t> pixels;   // width * height, tightly packed
    uint32_t width = 0, height = 0;
    uint64_t stamp_ns = 0;         // capture time, CLOCK_REALTIME (the streamer's time.time_ns)
    uint64_t seq = 0;              // 0 = none yet
};

// Copies a frame's pixels for view `v` (cropping eyes) into `out`.
void copy_view(View v, const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, Image *out);

// One packet of the streamer's wire format (big-endian):
//   u32 magic 0xFEEDFACE, u8 n_views, then per view:
//   u8 view id, u64 capture stamp, u32 jpeg length, jpeg bytes
struct Part {
    View view;
    uint64_t stamp_ns;
    const std::vector<uint8_t> *jpeg;
};
std::vector<uint8_t> packet(const std::vector<Part> &parts);

// Grayscale JPEG (stb_image_write); false if encoding failed.
bool encode_jpeg(const Image &img, int quality, std::vector<uint8_t> *out);

} // namespace camera_stream

// Config type "camera_stream". A debug restream of the cameras over TCP, in
// the format of ~/streamer/stream_daemon.py, so its clients (infer.py
// --source net:<headset>:<port>) work unchanged. It uses the frames the
// runtime already captures, so it streams only while tracking runs; it never
// turns cameras on.
//
// Off until switched on (live: /sinks/<name>/active, the debug panel's
// Diagnostics page). Each client gets its own loop: newest frame per view ->
// JPEG -> blocking send, so a slow network lowers its frame rate instead of
// queueing.
//
// Config:
//   active    stream (default false)
//   port      TCP port (default 9099)
//   views     ["face_l", "face_r", "eye_l", "eye_r"] (the mouth cameras are
//             face_l/face_r; a USB face tracker is face_l)
//   fps       per-client send rate cap (default 60)
//   quality   JPEG quality (default 80)
//   max_clients
class CameraStreamSink : public ISink {
public:
    CameraStreamSink(const nlohmann::json &cfg, SinkDeps deps);
    ~CameraStreamSink() override;

    bool start() override;
    void stop() override;
    bool wants_frames() const override { return true; }
    void offer_frames(const std::string &source, const StereoFrame &frames) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;

    bool active() const { return active_; }
    int port() const { return port_; }
    int clients() const { return clients_; }

private:
    void serve();                  // the listening thread, while active
    void client_loop(int fd, std::string peer);
    bool start_server();
    void stop_server();
    void take(camera_stream::View v, const Frame &f);

    std::string name_;
    ThreadSpec thread_spec_;
    StatsRegistry &stats_;
    int port_ = 9099;
    double fps_ = 60;
    int quality_ = 80, max_clients_ = 4;
    std::array<bool, camera_stream::kViews> want_{};

    std::atomic<bool> active_{false}, started_{false};
    std::mutex server_mutex_;      // start_server / stop_server
    int listen_fd_ = -1;
    std::thread server_;
    std::atomic<bool> stop_server_{false};
    std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
    std::atomic<int> clients_{0};

    std::mutex images_mutex_;
    std::array<camera_stream::Image, camera_stream::kViews> images_;
    uint64_t next_seq_ = 1;

    RateStat sent_;
    Counter encode_failures_;
};

} // namespace babble
