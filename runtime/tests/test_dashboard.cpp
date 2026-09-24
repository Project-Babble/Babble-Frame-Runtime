#include <doctest/doctest.h>

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
