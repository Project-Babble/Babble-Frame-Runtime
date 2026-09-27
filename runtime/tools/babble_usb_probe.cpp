// Streams a Babble face tracker (or any MJPEG UVC camera) over usbfs for a few
// seconds and reports what arrives; optionally saves frames.
//
//   babble_usb_probe [--seconds N] [--save DIR] [--count N] [--vid 303a] [--pid 8000] [--fps 60]
//
// Saves DIR/frame_NNN.jpg as received and DIR/frame_NNN.pgm decoded to grayscale.
#include "camera/usb/jpeg.h"
#include "camera/usb/uvc_camera.h"
#include "core/clock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace babble;

int main(int argc, char **argv) {
    double seconds = 5, fps = 0;
    int save_count = 0;
    std::string save_dir;
    unsigned vid = 0x303a, pid = 0x8000;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--seconds") seconds = atof(argv[i + 1]);
        else if (a == "--save") save_dir = argv[i + 1];
        else if (a == "--count") save_count = atoi(argv[i + 1]);
        else if (a == "--vid") vid = unsigned(strtoul(argv[i + 1], nullptr, 16));
        else if (a == "--pid") pid = unsigned(strtoul(argv[i + 1], nullptr, 16));
        else if (a == "--fps") fps = atof(argv[i + 1]);
    }
    if (!save_dir.empty() && !save_count)
        save_count = 10;

    auto found = usb::find(uint16_t(vid), uint16_t(pid));
    if (found.empty()) {
        fprintf(stderr, "no USB device %04x:%04x\n", vid, pid);
        return 1;
    }
    const auto &dev = found.front();
    printf("device %s  %s / %s  serial %s\n", dev.node.c_str(), dev.manufacturer.c_str(), dev.product_name.c_str(),
           dev.serial.c_str());

    uvc::Camera cam;
    std::string err;
    if (!cam.open(dev, 240, 240, fps, &err)) {
        fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    const auto &info = cam.info();
    const auto &p = cam.negotiated();
    printf("UVC %x.%02x  control if %u, stream if %u, endpoint 0x%02x (%s, %u B packets)\n", info.bcd_uvc >> 8,
           info.bcd_uvc & 0xff, info.control_iface, info.stream_iface, info.endpoint, info.bulk ? "bulk" : "iso",
           info.endpoint_max_packet);
    printf("negotiated MJPEG %ux%u, %.1f fps, max frame %u B, max payload %u B\n", cam.frame_size().width,
           cam.frame_size().height, p.frame_interval ? 1e7 / p.frame_interval : 0.0, p.max_video_frame_size,
           p.max_payload_transfer_size);
    if (!save_dir.empty())
        std::filesystem::create_directories(save_dir);

    std::vector<uint8_t> gray(1024 * 1024);
    int frames = 0, bad = 0, saved = 0;
    size_t bytes = 0;
    const uint64_t t0 = mono_ns(), end = t0 + uint64_t(seconds * 1e9);
    uint64_t first = 0, last = 0;
    while (mono_ns() < end) {
        bool ok = cam.read(200, [&](const uint8_t *d, size_t n) {
            const uint64_t now = mono_ns();
            if (!first)
                first = now;
            last = now;
            frames++;
            bytes += n;
            int w = 0, h = 0;
            if (!uvc::looks_like_jpeg(d, n) || !jpeg::decode_gray(d, n, gray.data(), gray.size(), &w, &h)) {
                bad++;
                return;
            }
            if (saved < save_count) {
                char path[512];
                snprintf(path, sizeof(path), "%s/frame_%03d.jpg", save_dir.c_str(), saved);
                if (FILE *f = fopen(path, "wb")) {
                    fwrite(d, 1, n, f);
                    fclose(f);
                }
                snprintf(path, sizeof(path), "%s/frame_%03d.pgm", save_dir.c_str(), saved);
                if (FILE *f = fopen(path, "wb")) {
                    fprintf(f, "P5\n%d %d\n255\n", w, h);
                    fwrite(gray.data(), 1, size_t(w) * h, f);
                    fclose(f);
                }
                saved++;
            }
        }, &err);
        if (!ok) {
            fprintf(stderr, "stream: %s\n", err.c_str());
            break;
        }
    }
    const double span = frames > 1 ? double(last - first) / 1e9 : 0;
    printf("%d frames in %.1f s: %.1f fps, avg %.1f KB, %d undecodable, %llu dropped by reassembly, %d saved\n", frames,
           span, span > 0 ? (frames - 1) / span : 0.0, frames ? bytes / 1024.0 / frames : 0.0, bad,
           (unsigned long long)cam.dropped(), saved);
    cam.close();
    return frames > 0 && bad == 0 ? 0 : 2;
}
