// Renders the pages of the debug dashboard to PNG with synthetic readings and without SteamVR.
#include "ui/dashboard_view.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace babble;
using namespace babble::ui;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dashboard_preview OUT_DIR [FONTS_DIR]\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    const std::filesystem::path fonts =
        argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path(BABBLE_RESOURCES_DIR) / "fonts";
    std::filesystem::create_directories(out);
    DashboardView view;
    view.load_icons(fonts.parent_path() / "icons");
    if (!view.load_fonts(fonts)) {
        fprintf(stderr, "fonts not found in %s\n", fonts.string().c_str());
        return 1;
    }
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto save = [&](const Canvas &img, const std::string &name) {
        std::string path = (out / (name + ".png")).string();
        stbi_write_png(path.c_str(), img.width(), img.height(), 4, img.data(), img.width() * 4);
        printf("%s\n", path.c_str());
    };

    std::vector<Choice> choices = {
        {"/sources/mouth/process_fps", "Mouth Tracking Rate", "Lower rates save battery.", "Hz",
         {30, 60, 90, 120}, 120},
        {"/sources/eye/rate_divisor", "Eye Tracking Rate",
         "A share of the eye camera rate. Lower rates save battery.", "", {1, 2, 3}, 1,
         false, false, {"1:1", "1:2", "1:3"}},
        {"/sinks/camera_stream/active", "Camera Stream",
         "Stream camera frames on TCP 9099 for diagnostics. Turns off after a restart.", "", {0, 1}, 1, true, true},
    };
    auto feed = [](Health h, double hz = 0, std::string detail = "", bool tracking = true) {
        FeedStatus f;
        f.health = h;
        f.hz = hz;
        f.detail = std::move(detail);
        f.available = true;
        f.tracking = tracking;
        return f;
    };
    Status tracking{true, Worn::Yes, feed(Health::Working, 119.8), feed(Health::Working, 90.1)};
    tracking.visualizer = AppState::Stopped;
    // This eye looks left and up, and is wide open.
    tracking.eyes[0] = {true, true, -18, 6, 1.0, 0.6, true, 0.843, true, 0.742, 0.941, true, 0.62};
    // This eye is half closed and the calibration is not complete.
    tracking.eyes[1] = {true, true, -21, 5, 0.35, 0, true, 0.251, false, 0, 0, false, 0};
    tracking.link = {true, true, "lan", "Demo PC", "192.0.2.10", 3.6};
    Status paused{true, Worn::No, feed(Health::Paused), feed(Health::Paused)};
    Status fault{true, Worn::Yes, feed(Health::Working, 120),
                 feed(Health::Fault, 0, "cannot connect to dsp_service (is the eye tracking service running?)")};
    Status starting{true, Worn::Yes, feed(Health::Starting), feed(Health::Starting)};
    starting.visualizer = AppState::Stopped;
    starting.link = {true, false, "", "", "", -1};   // The link searches for the PC.
    Status no_vr{false, Worn::Unknown, feed(Health::Starting), feed(Health::Off)};
    Status eyes_off{true, Worn::Yes, feed(Health::Working, 120), feed(Health::Off, 0, "", false)};
    Status all_off{true, Worn::Yes, feed(Health::Off, 0, "", false), feed(Health::Off, 0, "", false)};

    view.render(c, tracking, choices, std::nullopt);
    save(c, "home_tracking");
    // Show VRChat Native on the PC that Steam Link streams from, and on this headset with no data yet.
    Status native_pc = tracking;
    native_pc.link = {true, true, "pc", "", "192.0.2.10", -1, true};
    view.render(c, native_pc, choices, std::nullopt);
    save(c, "home_native_pc");
    Status native_frame = tracking;
    native_frame.link = {true, false, "frame", "", "127.0.0.1", -1, true};
    view.render(c, native_frame, choices, std::nullopt);
    save(c, "home_native_frame");
    view.render(c, tracking, choices, Point{84, DashboardView::kHeight - 84.f});
    save(c, "home_cog_hover");
    view.render(c, paused, choices, std::nullopt);
    save(c, "home_paused");
    view.render(c, fault, choices, std::nullopt);
    save(c, "home_fault");
    view.render(c, starting, choices, std::nullopt);
    save(c, "home_starting");
    view.render(c, no_vr, choices, std::nullopt);
    save(c, "home_no_steamvr");
    view.render(c, eyes_off, choices, Point{1080, 536});   // The pointer is on the eyes switch.
    save(c, "home_eyes_off");
    view.render(c, all_off, choices, std::nullopt);
    save(c, "home_all_off");
    {   // Show the runtime update pill next to the version.
        Status u = tracking;
        u.update = {UpdateNotice::State::Available, "V1.0 Alpha 5", "", 0};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_available");
        u.update = {UpdateNotice::State::Downloading, "V1.0 Alpha 5", "", 0.42f};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_downloading");
        u.update = {UpdateNotice::State::RestartNeeded, "V1.0 Alpha 5", "", 0};
        view.render(c, u, choices, std::nullopt);
        save(c, "home_update_restart");
    }

    view.set_page(DashboardView::Page::Settings);
    choices[0].value = 60;
    view.render(c, tracking, choices, Point{1000, 222});
    save(c, "settings");

    // Show the camera row for a USB mouth source, and then its page with two cameras and an active tracker.
    Status usb = tracking;
    usb.mouth_source = "usb";
    usb.cameras = {{"303a:8000:DEMO01", "Babble USB Tracker", "303a:8000 \u00b7 demo tracker"},
                   {"046d:0825:DEMO02", "USB Webcam", "046d:0825 \u00b7 demo camera"}};
    usb.camera = "303a:8000:DEMO01";
    usb.active_camera = usb.camera;
    view.render(c, usb, choices, std::nullopt);
    save(c, "settings_usb");
    // In release builds, the USB tracker is the only mouth source, so there is no Frunk/USB row and no mouth rate.
    Status release = usb;
    release.mouth_source_selectable = false;
    view.render(c, release, {choices[1]}, std::nullopt);
    save(c, "settings_release");
    view.set_page(DashboardView::Page::Cameras);
    view.render(c, usb, choices, Point{600, 340});   // The pointer is on the second camera.
    save(c, "cameras");
    Status none = usb;
    none.cameras.clear();
    none.camera.clear();
    none.active_camera.clear();
    view.render(c, none, choices, std::nullopt);
    save(c, "cameras_none");
    Status missing = usb;
    missing.cameras.clear();
    missing.active_camera.clear();
    view.render(c, missing, choices, std::nullopt);
    save(c, "cameras_missing");
    Status diag = tracking;
    diag.verbose_minutes_left = 7;
    diag.last_diagnostics = "diagnostics-demo.txt";
    diag.log_file = "/home/steamos/.local/state/babble-frame-runtime/runtime.log";
    diag.stats_file = "/home/steamos/.local/state/babble-frame-runtime/stats.jsonl";
    view.set_page(DashboardView::Page::Diagnostics);
    view.render(c, diag, choices, std::nullopt);
    save(c, "diagnostics");
    // Show the dataset capture row in a few states.
    for (const auto &[name, st] : {std::pair<const char *, CaptureRow::State>{"not_installed", CaptureRow::State::NotInstalled},
                                   {"downloading", CaptureRow::State::Downloading},
                                   {"installed", CaptureRow::State::Installed},
                                   {"update", CaptureRow::State::UpdateAvailable},
                                   {"running", CaptureRow::State::Running}}) {
        Status d = diag;
        d.capture.state = st;
        d.capture.version = "0.1.0";
        d.capture.latest = "0.2.0";
        d.capture.progress = 0.42f;
        d.capture.recording = st == CaptureRow::State::Running;
        d.capture.uploads_pending = st == CaptureRow::State::Running ? 2 : 0;
        d.capture.upload_progress = 0.4f;
        d.capture.upload_stage = "uploading";
        view.render(c, d, choices, std::nullopt);
        save(c, std::string("diagnostics_capture_") + name);
    }
    diag.verbose_minutes_left = 0;
    diag.last_diagnostics.clear();
    choices[2].value = 0;
    view.render(c, diag, choices, Point{1100, 222});
    save(c, "diagnostics_off");
    view.set_page(DashboardView::Page::Settings);
    view.render(c, tracking, choices, std::nullopt);
    view.click({1100, 666});
    view.render(c, tracking, choices, std::nullopt);
    save(c, "settings_restart_confirmation");

    // Measure the render cost for each redraw on the dashboard thread.
    {
        view.set_page(DashboardView::Page::Home);
        const int n = 50;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++)
            view.render(c, tracking, choices, Point{540, 536});
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        printf("home page render: %.2f ms\n", ms);
        view.set_page(DashboardView::Page::Settings);
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++)
            view.render(c, tracking, choices, Point{1000, 222});
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        printf("settings page render: %.2f ms\n", ms);
    }

    Canvas thumb(DashboardView::kThumb, DashboardView::kThumb);
    view.render_thumbnail(thumb);
    save(thumb, "thumbnail");
    return 0;
}
