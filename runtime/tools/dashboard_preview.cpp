// dashboard_preview: renders the dashboard pages to PNG without SteamVR.
//
//   dashboard_preview OUT_DIR [FONTS_DIR]
//
// Writes one image per front-page state, the settings page (with a hovered
// option) and the thumbnail. For reviewing the UI off the headset.
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
    if (!view.load_fonts(fonts)) {
        fprintf(stderr, "fonts not found in %s\n", fonts.c_str());
        return 1;
    }
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto save = [&](const Canvas &img, const std::string &name) {
        std::string path = (out / (name + ".png")).string();
        stbi_write_png(path.c_str(), img.width(), img.height(), 4, img.data(), img.width() * 4);
        printf("%s\n", path.c_str());
    };

    std::vector<Choice> choices = {
        {"/sources/mouth/process_fps", "Mouth tracking rate", "Updates per second. Lower rates save battery.", "Hz",
         {30, 60, 90, 120}, 120},
        {"/sources/eye/process_fps", "Eye tracking rate", "Updates per second. Lower rates save battery.", "Hz",
         {30, 45, 60, 90}, 90},
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
    tracking.eyes[0] = {true, true, -18, 6, 1.0, 0.6, true, 0.843};   // looking left and up, eyes wide
    tracking.eyes[1] = {true, true, -21, 5, 0.35, 0, true, 0.251};   // right eye half closed
    Status paused{true, Worn::No, feed(Health::Paused), feed(Health::Paused)};
    Status fault{true, Worn::Yes, feed(Health::Working, 120),
                 feed(Health::Fault, 0, "cannot connect to dsp_service (is the eye tracking service running?)")};
    Status starting{true, Worn::Yes, feed(Health::Starting), feed(Health::Starting)};
    Status no_vr{false, Worn::Unknown, feed(Health::Starting), feed(Health::Off)};
    Status eyes_off{true, Worn::Yes, feed(Health::Working, 120), feed(Health::Off, 0, "", false)};
    Status all_off{true, Worn::Yes, feed(Health::Off, 0, "", false), feed(Health::Off, 0, "", false)};

    view.render(c, tracking, choices, std::nullopt);
    save(c, "home_tracking");
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
    view.render(c, eyes_off, choices, Point{1080, 536});   // hovering the eyes switch
    save(c, "home_eyes_off");
    view.render(c, all_off, choices, std::nullopt);
    save(c, "home_all_off");

    view.set_page(DashboardView::Page::Settings);
    choices[0].value = 60;
    view.render(c, tracking, choices, Point{1000, 222});
    save(c, "settings");

    // Render cost (what the dashboard thread pays per redraw).
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
