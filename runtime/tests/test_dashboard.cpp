#include <doctest/doctest.h>

#include "core/clock.h"

#include "ui/canvas.h"
#include "ui/dashboard_view.h"

#include <set>

using namespace babble;
using namespace babble::ui;

namespace {

const std::filesystem::path kFonts = std::filesystem::path(BABBLE_RESOURCES_DIR) / "fonts";

std::vector<Choice> rates() {
    return {{"/sources/mouth/process_fps", "Mouth Tracking Rate", "", "Hz", {30, 60, 90, 120}, 120},
            {"/sources/eye/rate_divisor", "Eye Tracking Rate", "", "", {1, 2, 3}, 1, false, false, {"1:1", "1:2", "1:3"}}};
}

FeedStatus on(Health h, double hz = 0) {
    FeedStatus f;
    f.health = h;
    f.hz = hz;
    f.available = true;
    return f;
}

// Gives the centre of the cog at the position where the view puts it.
Point cog() { return {84, DashboardView::kHeight - 84.f}; }

}

TEST_CASE("canvas anti-aliased shapes cover what they should") {
    Canvas c(64, 64);
    c.clear(Color::hex(0x000000));
    c.fill_circle(32, 32, 10.3f, Color::hex(0xffffff));
    auto at = [&](int x, int y) { return c.data()[(size_t(y) * 64 + x) * 4]; };
    CHECK(at(32, 32) == 255);
    CHECK(at(2, 2) == 0);
    const int edge = at(42, 32);   // This pixel is on the rim and has partial coverage.
    CHECK(edge > 0);
    CHECK(edge < 255);
    CHECK(c.data()[3] == 255);     // The pixel stays opaque.
}

TEST_CASE("the cog opens settings, a choice reports its value, the cog goes back") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto choices = rates();
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};

    view.render(c, s, choices, std::nullopt);
    CHECK(view.page() == DashboardView::Page::Home);
    CHECK_FALSE(view.click({600, 300}).redraw);   // There is nothing to click at that point.
    CHECK(view.click(cog()).redraw);
    CHECK(view.page() == DashboardView::Page::Settings);

    // Scan the first row for a hit to find its "60 Hz" segment.
    view.render(c, s, choices, std::nullopt);
    std::optional<std::pair<size_t, double>> picked;
    for (float x = 500; x < DashboardView::kWidth && !picked; x += 8)
        if (auto r = view.click({x, 222}); r.setting && r.setting->second == 60)
            picked = r.setting;
    REQUIRE(picked);
    CHECK(picked->first == 0);

    view.click(cog());
    CHECK(view.page() == DashboardView::Page::Home);
}

TEST_CASE("the eye rate row offers shares of the camera rate") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    s.choice_hints["/sources/eye/rate_divisor"] = "45 Hz now, from 90 Hz eye cameras.";
    view.render(c, s, rates(), std::nullopt);
    view.click(cog());
    view.render(c, s, rates(), std::nullopt);
    std::set<double> offered;
    for (float x = 500; x < DashboardView::kWidth; x += 8)
        if (auto r = view.click({x, 222 + 124}); r.setting && r.setting->first == 1)
            offered.insert(r.setting->second);
    CHECK(offered == std::set<double>{1, 2, 3});
}

TEST_CASE("text is drawn and long details are shortened to fit") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    FeedStatus broken = on(Health::Fault);
    broken.detail = std::string(400, 'x');
    Status s{true, Worn::Yes, on(Health::Working, 120), broken};
    view.render(c, s, rates(), std::nullopt);
    // The fault text must stay inside the right half of its card, before the right edge of the card.
    const int right_edge = DashboardView::kWidth - 56;
    int stray = 0;
    for (int y = 560; y < 600; y++)
        for (int x = right_edge + 2; x < DashboardView::kWidth; x++)
            stray += c.data()[(size_t(y) * DashboardView::kWidth + x) * 4] > 0x40;
    CHECK(stray == 0);
}

TEST_CASE("each feed card has an on/off switch; unconfigured feeds have none") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.render(c, s, rates(), std::nullopt);

    // The switch is at the right end of each card, and the cards span y 470..602.
    auto r = view.click({540, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->first == 0);
    CHECK(r.toggle->second == false);   // The switch was on, so the click turns it off.
    r = view.click({1100, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->first == 1);

    s.eye.tracking = false;
    s.eye.health = Health::Off;
    view.render(c, s, rates(), std::nullopt);
    r = view.click({1100, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->second == true);    // The switch was off, so the click turns it on.

    s.mouth = FeedStatus{};            // The mouth feed is not configured.
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click({540, 536}).toggle);
    CHECK(view.page() == DashboardView::Page::Home);   // A switch never changes the page.
}

TEST_CASE("the visualizer button appears only when the visualizer is installed") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    // The button is at the bottom right.
    const Point button{DashboardView::kWidth - 100.f, DashboardView::kHeight - 84.f};

    view.render(c, s, rates(), std::nullopt);   // The default state is Unavailable.
    CHECK_FALSE(view.click(button).visualizer);

    s.visualizer = AppState::Stopped;
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).visualizer);
    s.visualizer = AppState::Running;   // The same button closes the visualizer.
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).visualizer);

    view.set_page(DashboardView::Page::Settings);   // The button is only on the front page.
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).visualizer);
}

TEST_CASE("the settings page switches the mouth source and picks a USB camera on its own page") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.set_page(DashboardView::Page::Settings);

    const Point usb_segment{DashboardView::kWidth - 56.f - 4 - 80, 408 + 62};
    s.cameras = {{"303a:8000:ABC", "ETVR openiristracker", "303a:8000"}, {"046d:0825:X", "Webcam", "046d:0825"}};
    DashboardView::ClickResult r;
    SUBCASE("the source is a choice (dev builds)") {
        // The mouth source row (Frunk | USB) is right-aligned under the two rate rows at y 160 + 2 x 124.
        view.render(c, s, rates(), std::nullopt);
        r = view.click(usb_segment);
        REQUIRE(r.mouth_source);
        CHECK(*r.mouth_source == "usb");

        // With USB, a camera row opens the camera page and asks for a new list.
        s.mouth_source = "usb";
        view.render(c, s, rates(), std::nullopt);
        r = view.click({600, 532 + 44});
        CHECK(r.refresh_cameras);
        CHECK(view.page() == DashboardView::Page::Cameras);
    }
    SUBCASE("the USB camera is the only mouth (release builds)") {
        // Without the Frunk/USB row, the camera row takes its place for all values of mouth_source.
        s.mouth_source_selectable = false;
        view.render(c, s, rates(), std::nullopt);
        r = view.click(usb_segment);
        CHECK_FALSE(r.mouth_source);
        CHECK(r.refresh_cameras);
        CHECK(view.page() == DashboardView::Page::Cameras);
    }

    // The rows are Automatic and then each camera, at y 150 + 92 per row.
    view.render(c, s, rates(), std::nullopt);
    r = view.click({600, 150 + 92 + 40});
    REQUIRE(r.camera);
    CHECK(*r.camera == "303a:8000:ABC");
    r = view.click({600, 150 + 40});
    REQUIRE(r.camera);
    CHECK(r.camera->empty());   // An empty value is Automatic.
    CHECK(view.click({DashboardView::kWidth - 56.f - 60, 74}).refresh_cameras);

    // Back goes to Settings and not to the front page.
    view.click({60, 70});
    CHECK(view.page() == DashboardView::Page::Settings);
}

TEST_CASE("the diagnostics page switches verbose logging and asks for a file") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    view.click({DashboardView::kWidth - 56.f - 60, 74});   // The Diagnostics button is at the top right.
    REQUIRE(view.page() == DashboardView::Page::Diagnostics);

    view.render(c, s, rates(), std::nullopt);
    auto r = view.click({DashboardView::kWidth - 56.f - 46, 160 + 62});   // This point is the verbose switch.
    REQUIRE(r.verbose);
    CHECK(*r.verbose);
    s.verbose_minutes_left = 9;
    view.render(c, s, rates(), std::nullopt);
    r = view.click({DashboardView::kWidth - 56.f - 46, 160 + 62});
    REQUIRE(r.verbose);
    CHECK_FALSE(*r.verbose);   // The switch is on, so the same switch turns it off.
    CHECK(view.click({DashboardView::kWidth - 56.f - 60, 284 + 62}).write_diagnostics);

    view.click({60, 70});   // Go back to Settings.
    CHECK(view.page() == DashboardView::Page::Settings);
}

TEST_CASE("the dataset capture row: hidden, or its button and Remove by state") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.set_page(DashboardView::Page::Diagnostics);
    using A = DashboardView::ClickResult::CaptureAction;
    using S = CaptureRow::State;
    // The buttons are right-aligned under the verbose logging row and the file row, with 112 px rows from y 160.
    const Point primary{DashboardView::kWidth - 56.f - 40, 160 + 2 * 112 + 56};
    auto scan_remove = [&] {
        for (float x = 600; x < DashboardView::kWidth - 56.f - 40; x += 8)
            if (view.click({x, primary.y}).capture == A::Remove)
                return true;
        return false;
    };

    view.render(c, s, rates(), std::nullopt);   // The default state is Hidden.
    CHECK(view.click(primary).capture == A::None);

    for (S st : {S::NotInstalled, S::Installed, S::UpdateAvailable, S::Running, S::Downloading, S::Failed}) {
        CAPTURE(int(st));
        s.capture = {};
        s.capture.state = st;
        s.capture.version = "0.1.0";
        s.capture.latest = "0.2.0";
        view.render(c, s, rates(), std::nullopt);
        CHECK(view.click(primary).capture == A::Primary);
        view.render(c, s, rates(), std::nullopt);
        CHECK(scan_remove() == (st == S::Installed || st == S::UpdateAvailable));
    }
    for (S st : {S::Checking, S::Installing}) {   // The installer is busy, so there is nothing to press.
        s.capture.state = st;
        view.render(c, s, rates(), std::nullopt);
        CHECK(view.click(primary).capture == A::None);
    }
}

TEST_CASE("opening the diagnostics page is reported") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click({DashboardView::kWidth - 56.f - 60, 74}).opened_diagnostics);
}

TEST_CASE("the restart button on the settings page needs a second click") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    // The button is at the bottom right.
    const Point button{DashboardView::kWidth - 100.f, DashboardView::kHeight - 84.f};

    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    auto first = view.click(button);
    CHECK_FALSE(first.restart);
    CHECK(first.redraw);   // The view shows "Confirm Restart".
    CHECK(view.restart_armed(babble::mono_ns()));
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).restart);
    CHECK_FALSE(view.restart_armed(babble::mono_ns()));

    // When the user leaves the page, the view disarms the restart.
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).restart);
    view.render(c, s, rates(), std::nullopt);
    view.click({60, 70});   // Go back.
    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).restart);

    // All other routes away from Settings must also cancel the confirmation.
    view.render(c, s, rates(), std::nullopt);
    view.click({60, 70});
    s.mouth_source = "usb";
    for (const Point leave : {cog(), Point{1084, 74}, Point{600, 576}}) {
        view.set_page(DashboardView::Page::Settings);
        view.render(c, s, rates(), std::nullopt);
        CHECK_FALSE(view.click(button).restart);
        REQUIRE(view.restart_armed(babble::mono_ns()));
        view.render(c, s, rates(), std::nullopt);
        view.click(leave);
        CHECK_FALSE(view.restart_armed(babble::mono_ns()));
        if (view.page() == DashboardView::Page::Cameras || view.page() == DashboardView::Page::Diagnostics) {
            view.render(c, s, rates(), std::nullopt);
            CHECK(view.click(cog()).redraw);   // This is the Dashboard button in the footer.
            CHECK(view.page() == DashboardView::Page::Home);
            CHECK_FALSE(view.restart_armed(babble::mono_ns()));
        }
        view.set_page(DashboardView::Page::Settings);
        view.render(c, s, rates(), std::nullopt);
        CHECK_FALSE(view.click(button).restart);
        view.render(c, s, rates(), std::nullopt);
        view.click({60, 70});
    }
}

TEST_CASE("the eye panel places gaze and fills labelled output meters") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    auto px = [&](float x, float y) {
        const uint8_t *p = c.data() + (size_t(y) * DashboardView::kWidth + size_t(x)) * 4;
        return Color{p[0], p[1], p[2]};
    };
    auto is = [](Color a, uint32_t rgb) { return a.r == uint8_t(rgb >> 16) && a.g == uint8_t(rgb >> 8) && a.b == uint8_t(rgb); };
    // The left panel has the plot at (76, 204), 174 px wide, and the labelled meters start at x 276.
    const float cx = 163, cy = 291, half = 75, meter_left = 276, meter_right = 568;

    view.render(c, s, rates(), std::nullopt);   // Without data, the view shows no dot.
    CHECK_FALSE(is(px(cx + half, cy), 0x0073cf));

    s.eyes[0] = {true, true, 45, 0, 1.0};   // The eye looks fully right and is open.
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(cx + half, cy), 0x0073cf));         // The dot is at the right edge.
    CHECK(is(px(meter_right - 10, 249), 0x0073cf)); // The openness fills the full meter.

    s.eyes[0].openness = 0.2;                      // The eye is mostly closed, so only the left fifth fills.
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(is(px(meter_right - 10, 249), 0x0073cf));
    CHECK(is(px(meter_left + 10, 249), 0x0073cf));

    CHECK_FALSE(is(px(meter_left + 10, 322), 0x0073cf));
    s.eyes[0].openness = 1.0;
    s.eyes[0].widen = 0.5;
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(meter_left + 10, 322), 0x0073cf));
    CHECK_FALSE(is(px(meter_right - 10, 322), 0x0073cf));

    // The view hides dilation until its baseline is ready, but the 50% baseline tick stays visible.
    s.eyes[0].dilation = 0.8;
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(is(px(480, 395), 0xa78bfa));
    CHECK(is(px(422, 395), 0xffffff));
    s.eyes[0].dilation_ok = true;
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(480, 395), 0xa78bfa));
    CHECK_FALSE(is(px(meter_right - 10, 395), 0xa78bfa));

    // Readings out of range never go outside a meter or plot.
    s.eyes[0].yaw_deg = 90;
    s.eyes[0].openness = 2;
    s.eyes[0].widen = -1;
    s.eyes[0].dilation = -1;
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(cx + half, cy), 0x0073cf));
    CHECK(is(px(meter_right - 10, 249), 0x0073cf));
    CHECK_FALSE(is(px(meter_right + 2, 249), 0x0073cf));
    CHECK_FALSE(is(px(meter_left + 10, 322), 0x0073cf));
    CHECK_FALSE(is(px(meter_left + 10, 395), 0xa78bfa));
}

TEST_CASE("the logo becomes the tab thumbnail, with a badge fallback") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas thumb(DashboardView::kThumb, DashboardView::kThumb);
    view.render_thumbnail(thumb);
    const uint8_t *centre = thumb.data() + (size_t(64) * DashboardView::kThumb + 20) * 4;
    // Without icons, the view uses the accent colour of Baballonia.
    CHECK((centre[0] == 0x00 && centre[1] == 0x73 && centre[2] == 0xcf));

    REQUIRE(view.load_icons(std::filesystem::path(BABBLE_RESOURCES_DIR) / "icons"));
    view.render_thumbnail(thumb);
    CHECK(centre[1] > centre[0]);   // This is the green background of the logo.
    CHECK(centre[1] > centre[2]);
}

TEST_CASE("an on/off setting shows as a switch that reports the flipped value") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto choices = rates();
    Choice force{"/sinks/camera_stream/active", "Camera stream", "", "", {0, 1}, 0};
    force.boolean = true;
    force.diagnostics = true;
    choices.push_back(force);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.render(c, s, choices, std::nullopt);
    view.click(cog());
    view.render(c, s, choices, std::nullopt);
    // On the Settings page, the third row is Mouth source in the default selectable layout.
    const Point sw{DashboardView::kWidth - 56.f - 46, 160 + 2 * 124 + 62};
    CHECK_FALSE(view.click(sw).setting);

    // On Diagnostics, the switch is after the verbose logging row and the file row.
    view.click({DashboardView::kWidth - 56.f - 60, 74});
    view.render(c, s, choices, std::nullopt);
    REQUIRE(view.page() == DashboardView::Page::Diagnostics);
    auto r = view.click(sw);
    REQUIRE(r.setting);
    CHECK(r.setting->first == 2);
    CHECK(r.setting->second == 1);

    choices[2].value = 1;
    view.render(c, s, choices, std::nullopt);
    r = view.click(sw);
    REQUIRE(r.setting);
    CHECK(r.setting->second == 0);
}

TEST_CASE("the outputs page switches modules, picks their settings and asks for the keyboard") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    OutputModule link{"vrcft", "VRCFaceTracking", "The module on your PC.", true, true, "Linked to PC via LAN", {}};
    OutputModule native{"vrc_native", "VRChat Native", "Into VRChat.", false, false, "", {}};
    native.options.push_back({"/sinks/vrc_native/target", "VRChat On", false, false,
                              {"\"auto\"", "\"pc\"", "\"frame\""}, {"Auto", "PC", "Frame"}, "\"auto\""});
    OutputModule osc{"babble_osc", "Babble OSC", "OSC to your PC.", true, false, "Set a PC address", {}};
    osc.options.push_back({"/sinks/babble_osc/port", "Port", true, true, {}, {}, "8888"});
    s.outputs = {link, native, osc};

    // Settings has an Outputs button next to Diagnostics, and Back goes to Settings.
    bool opened = false;
    for (float x = DashboardView::kWidth - 56.f; x > 600 && !opened; x -= 8) {
        view.set_page(DashboardView::Page::Settings);
        view.render(c, s, rates(), std::nullopt);
        view.click({x, 74});
        opened = view.page() == DashboardView::Page::Outputs;
    }
    REQUIRE(opened);
    view.render(c, s, rates(), std::nullopt);

    // Scan each row from the right for what its controls report.
    const float row_h = 115;   // There are four rows from y 160.
    auto scan = [&](int row) {
        std::vector<DashboardView::ClickResult> found;
        for (float x = DashboardView::kWidth - 60.f; x > 300; x -= 6) {
            // The controls in the rows do not change the page.
            auto r = view.click({x, 160 + row_h * float(row) + row_h / 2});
            if (r.output_setting || r.edit_text)
                found.push_back(r);
        }
        return found;
    };
    std::set<std::pair<std::string, std::string>> settings;
    std::set<std::string> edits;
    for (int row = 0; row < 4; row++)
        for (const auto &r : scan(row)) {
            if (r.output_setting)
                settings.insert(*r.output_setting);
            if (r.edit_text)
                edits.insert(*r.edit_text);
        }
    CHECK(settings.count({"/outputs/vrcft", "false"}));       // The switch of a module that is on turns it off.
    CHECK(settings.count({"/outputs/vrc_native", "true"}));
    CHECK(settings.count({"/outputs/babble_osc", "false"}));
    CHECK(settings.count({"/sinks/vrc_native/target", "\"frame\""}));
    CHECK(edits == std::set<std::string>{"/pc_address", "/sinks/babble_osc/port"});
    CHECK_FALSE(settings.count({"/pc_address", "\"\""}));      // Clear shows only with an address.

    s.pc_address = "192.168.1.20";
    s.output_error = "Port: abc is not a number";
    view.render(c, s, rates(), std::nullopt);
    bool cleared = false;
    for (const auto &r : scan(0))
        cleared |= r.output_setting && *r.output_setting == std::make_pair(std::string("/pc_address"), std::string("\"\""));
    CHECK(cleared);

    view.click({60, 70});
    CHECK(view.page() == DashboardView::Page::Settings);
}

TEST_CASE("a camera setting shows as a switch on the USB camera page and not on Settings") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto choices = rates();
    Choice mirror{"/pipelines/face/mirror", "Mirror Image", "", "", {0, 1}, 1};
    mirror.boolean = true;
    mirror.cameras = true;
    choices.push_back(mirror);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    s.mouth_source = "usb";
    s.cameras = {{"303a:8000:ABC", "Babble tracker", "303a:8000"}};
    view.set_page(DashboardView::Page::Cameras);
    view.render(c, s, choices, std::nullopt);
    auto r = view.click({DashboardView::kWidth - 56.f - 46, 150 + 52});
    REQUIRE(r.setting);
    CHECK(r.setting->first == 2);
    CHECK(r.setting->second == 0);
    // The camera rows start below the switch.
    r = view.click({600, 150 + 104 + 40});
    REQUIRE(r.camera);
    CHECK(r.camera->empty());

    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, choices, std::nullopt);
    CHECK_FALSE(view.click({DashboardView::kWidth - 56.f - 46, 160 + 2 * 124 + 62}).setting);
}
