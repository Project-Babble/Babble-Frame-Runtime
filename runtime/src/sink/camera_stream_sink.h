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

// The view numbers of the streamer wire format.
enum View : uint8_t { FaceL = 0, FaceR = 1, EyeL = 2, EyeR = 3, kViews = 4 };
const char *view_name(View v);

// One grayscale image to send, with the eye views cropped from 512x512 to 400x400.
struct Image {
    std::vector<uint8_t> pixels;   // The pixels are width * height bytes with no padding.
    uint32_t width = 0, height = 0;
    uint64_t stamp_ns = 0;         // The capture time on CLOCK_REALTIME.
    uint64_t seq = 0;              // A value of 0 means that no image is available yet.
};

// Copies the pixels of a frame for view `v` into `out` and crops the eye views.
void copy_view(View v, const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, Image *out);

// One part of a packet in the big-endian streamer wire format, which starts with the magic 0xFEEDFACE.
struct Part {
    View view;
    uint64_t stamp_ns;
    const std::vector<uint8_t> *jpeg;
};
std::vector<uint8_t> packet(const std::vector<Part> &parts);

// Encodes a grayscale JPEG and returns false if the encoder fails.
bool encode_jpeg(const Image &img, int quality, std::vector<uint8_t> *out);

}

// Config type "camera_stream" sends the camera frames over TCP for debug, and it never turns on a camera.
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
    void serve();                  // The thread that accepts the clients.
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
    std::mutex server_mutex_;      // This mutex protects start_server() and stop_server().
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

}
