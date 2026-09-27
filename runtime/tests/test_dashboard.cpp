#include <doctest/doctest.h>

#include "core/clock.h"

#include "ui/canvas.h"
#include "ui/dashboard_view.h"

using namespace babble;
using namespace babble::ui;

namespace {

const std::filesystem::path kFonts = std::filesystem::path(BABBLE_RESOURCES_DIR) / "fonts";

std::vector<Choice> rates() {
    return {{"/sources/mouth/process_fps", "Mouth tracking rate", "", "Hz", {30, 60, 90, 120}, 120},
            {"/sources/eye/process_fps", "Eye tracking rate", "", "Hz", {30, 45, 60, 90}, 90}};
}

FeedStatus on(Health h, double hz = 0) {
    FeedStatus f;
    f.health = h;
    f.hz = hz;
    f.available = true;
    return f;
}

// Centre of the cog, as the view places it.
Point cog() { return {84, DashboardView::kHeight - 84.f}; }

} // namespace

TEST_CASE("canvas anti-aliased shapes cover what they should") {
    Canvas c(64, 64);
    c.clear(Color::hex(0x000000));
    c.fill_circle(32, 32, 10.3f, Color::hex(0xffffff));
    auto at = [&](int x, int y) { return c.data()[(size_t(y) * 64 + x) * 4]; };
    CHECK(at(32, 32) == 255);
    CHECK(at(2, 2) == 0);
    const int edge = at(42, 32);   // on the rim: partial coverage
    CHECK(edge > 0);
    CHECK(edge < 255);
    CHECK(c.data()[3] == 255);     // stays opaque
}

TEST_CASE("the cog opens settings, a choice reports its value, the cog goes back") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    auto choices = rates();
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};

    view.render(c, s, choices, std::nullopt);
    CHECK(view.page() == DashboardView::Page::Home);
    CHECK_FALSE(view.click({600, 300}).redraw);   // nothing clickable in the headline
    CHECK(view.click(cog()).redraw);
    CHECK(view.page() == DashboardView::Page::Settings);

    // Find the "60 Hz" segment of the first row by scanning its row for a hit.
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

TEST_CASE("text is drawn and long details are shortened to fit") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    FeedStatus broken = on(Health::Fault);
    broken.detail = std::string(400, 'x');
    Status s{true, Worn::Yes, on(Health::Working, 120), broken};
    view.render(c, s, rates(), std::nullopt);
    // The fault text must stay inside its card (right half, above the card's right edge).
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

    // The switch sits at the right end of each card (cards span y 470..602).
    auto r = view.click({540, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->first == 0);
    CHECK(r.toggle->second == false);   // on -> turn off
    r = view.click({1100, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->first == 1);

    s.eye.tracking = false;
    s.eye.health = Health::Off;
    view.render(c, s, rates(), std::nullopt);
    r = view.click({1100, 536});
    REQUIRE(r.toggle);
    CHECK(r.toggle->second == true);    // off -> turn on

    s.mouth = FeedStatus{};            // not configured
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click({540, 536}).toggle);
    CHECK(view.page() == DashboardView::Page::Home);   // switches never change the page
}

TEST_CASE("the headline summarises tracking, pauses and feeds that are off") {
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    Headline h = headline(s);
    CHECK(h.word == "Tracking");
    CHECK(h.tone == Health::Working);
    CHECK(h.sub == "Mouth  120 Hz   ·   Eyes  90 Hz");

    s.eye.health = Health::Off;
    s.eye.tracking = false;
    CHECK(headline(s).sub == "Mouth  120 Hz");

    s.worn = Worn::No;
    CHECK(headline(s).word == "Paused");

    s.mouth.health = Health::Off;
    s.mouth.tracking = false;
    CHECK(headline(s).word == "Tracking off");
    CHECK(headline(s).sub == "Turn a feed on below");

    s.steamvr = false;
    CHECK(headline(s).word == "Waiting for SteamVR");
}

TEST_CASE("the visualizer button appears only when the visualizer is installed") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    const Point button{DashboardView::kWidth - 100.f, DashboardView::kHeight - 84.f};   // bottom right

    view.render(c, s, rates(), std::nullopt);   // Unavailable by default
    CHECK_FALSE(view.click(button).visualizer);

    s.visualizer = AppState::Stopped;
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).visualizer);
    s.visualizer = AppState::Running;   // same button closes it
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).visualizer);

    view.set_page(DashboardView::Page::Settings);   // front page only
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).visualizer);
}

TEST_CASE("the settings page switches the mouth source and picks a USB camera on its own page") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    view.set_page(DashboardView::Page::Settings);

    // The mouth source row sits under the two rate rows (y 160 + 2 x 124): Frunk | USB, right-aligned.
    view.render(c, s, rates(), std::nullopt);
    const Point usb_segment{DashboardView::kWidth - 56.f - 4 - 80, 408 + 62};
    auto r = view.click(usb_segment);
    REQUIRE(r.mouth_source);
    CHECK(*r.mouth_source == "usb");

    // With USB, a camera row opens the camera page and asks for a fresh list.
    s.mouth_source = "usb";
    s.cameras = {{"303a:8000:ABC", "ETVR openiristracker", "303a:8000"}, {"046d:0825:X", "Webcam", "046d:0825"}};
    view.render(c, s, rates(), std::nullopt);
    r = view.click({600, 532 + 44});
    CHECK(r.refresh_cameras);
    CHECK(view.page() == DashboardView::Page::Cameras);

    // Rows: Automatic, then each camera (150 + 92 per row).
    view.render(c, s, rates(), std::nullopt);
    r = view.click({600, 150 + 92 + 40});
    REQUIRE(r.camera);
    CHECK(*r.camera == "303a:8000:ABC");
    r = view.click({600, 150 + 40});
    REQUIRE(r.camera);
    CHECK(r.camera->empty());   // automatic
    CHECK(view.click({DashboardView::kWidth - 56.f - 60, 74}).refresh_cameras);

    // Back goes to Settings, not the front page.
    view.click({60, 70});
    CHECK(view.page() == DashboardView::Page::Settings);
}

TEST_CASE("the restart button on the settings page needs a second click") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    const Point button{DashboardView::kWidth - 100.f, DashboardView::kHeight - 84.f};   // bottom right

    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    auto first = view.click(button);
    CHECK_FALSE(first.restart);
    CHECK(first.redraw);   // shows "Click again to restart"
    CHECK(view.restart_armed(babble::mono_ns()));
    view.render(c, s, rates(), std::nullopt);
    CHECK(view.click(button).restart);
    CHECK_FALSE(view.restart_armed(babble::mono_ns()));

    // Leaving the page disarms it: one click after coming back only arms it again.
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).restart);
    view.render(c, s, rates(), std::nullopt);
    view.click({60, 70});   // Back
    view.set_page(DashboardView::Page::Settings);
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(view.click(button).restart);
}

TEST_CASE("the eye panel places the gaze dot and fills the openness bar") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas c(DashboardView::kWidth, DashboardView::kHeight);
    Status s{true, Worn::Yes, on(Health::Working, 120), on(Health::Working, 90)};
    auto px = [&](float x, float y) {
        const uint8_t *p = c.data() + (size_t(y) * DashboardView::kWidth + size_t(x)) * 4;
        return Color{p[0], p[1], p[2]};
    };
    auto is = [](Color a, uint32_t rgb) { return a.r == uint8_t(rgb >> 16) && a.g == uint8_t(rgb >> 8) && a.b == uint8_t(rgb); };
    // Left eye group: 326 px wide, groups 110 apart, so it starts at (1200 - 762) / 2 = 219; plot 210 px at y 212.
    const float gx = 219, top = 212, cx = gx + 105, cy = top + 105, half = 105 - 14, bar_x = gx + 210 + 24 + 18;

    view.render(c, s, rates(), std::nullopt);   // no data: no dot
    CHECK_FALSE(is(px(cx + half, cy), 0x4d8dff));

    s.eyes[0] = {true, true, 45, 0, 1.0};   // full right, eye open
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(cx + half, cy), 0x4d8dff));            // dot at the right edge
    CHECK(is(px(bar_x, top + 10), 0x3fcf73));   // bar filled to the top

    s.eyes[0].openness = 0.2;                          // mostly closed: top of the bar empty
    view.render(c, s, rates(), std::nullopt);
    CHECK_FALSE(is(px(bar_x, top + 10), 0x3fcf73));
    CHECK(is(px(bar_x, top + 205), 0x3fcf73));

    const float widen_x = gx + 210 + 24 + 36 + 10 + 9;   // the slim widen bar
    CHECK_FALSE(is(px(widen_x, top + 190), 0xf2b33d));
    s.eyes[0].openness = 1.0;
    s.eyes[0].widen = 0.5;                             // half wide: lower half amber
    view.render(c, s, rates(), std::nullopt);
    CHECK(is(px(widen_x, top + 190), 0xf2b33d));
    CHECK_FALSE(is(px(widen_x, top + 30), 0xf2b33d));
}

TEST_CASE("the logo becomes the tab thumbnail, with a badge fallback") {
    DashboardView view;
    REQUIRE(view.load_fonts(kFonts));
    Canvas thumb(DashboardView::kThumb, DashboardView::kThumb);
    view.render_thumbnail(thumb);
    const uint8_t *centre = thumb.data() + (size_t(64) * DashboardView::kThumb + 20) * 4;
    CHECK((centre[0] == 0x4d && centre[1] == 0x8d && centre[2] == 0xff));   // no icons: the blue "B" badge

    REQUIRE(view.load_icons(std::filesystem::path(BABBLE_RESOURCES_DIR) / "icons"));
    view.render_thumbnail(thumb);
    CHECK(centre[1] > centre[0]);   // the logo's green background
    CHECK(centre[1] > centre[2]);
}
