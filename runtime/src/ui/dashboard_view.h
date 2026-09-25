#pragma once

#include "platform/steamvr_client.h"
#include "ui/canvas.h"
#include "ui/font.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace babble::ui {

enum class Health { Off, Starting, Paused, Working, Fault };

// The front page's feeds, in order: config source names.
inline constexpr const char *kFeedSources[2] = {"mouth", "eye"};

// One camera feed (mouth, eyes) as the front page shows it.
struct FeedStatus {
    Health health = Health::Off;
    double hz = 0;           // tracking rate while Working
    std::string detail;      // the error for Fault
    bool available = false;  // configured; false hides the on/off switch
    bool tracking = true;    // the user's on/off
};

struct Status {
    bool steamvr = false;
    Worn worn = Worn::Unknown;
    FeedStatus mouth, eye;
};

// The overall tracking state in words (the Babble tab's headline); tone
// picks the dot colour.
struct Headline {
    std::string word, sub;
    Health tone = Health::Off;
};
Headline headline(const Status &s);

// A user-facing setting shown as a row of choices. id is the setting's JSON
// pointer in the config ("/sources/mouth/process_fps").
struct Choice {
    std::string id, label, hint, unit;
    std::vector<double> options;
    double value = 0;
};

struct Point {
    float x = 0, y = 0;
};

// The dashboard's pages, drawn on the CPU: a front page with the feed
// indicators and tracking status, and a settings page behind the cog in the
// bottom-left corner. No OpenVR here, so it renders anywhere (tests, PNGs).
class DashboardView {
public:
    static constexpr int kWidth = 1200, kHeight = 750, kThumb = 128;
    enum class Page { Home, Settings };

    bool load_fonts(const std::filesystem::path &dir);   // NotoSans-Regular.ttf, NotoSans-Medium.ttf

    // Draws the current page; pointer (canvas pixels, top-left origin) drives hover.
    void render(Canvas &c, const Status &status, const std::vector<Choice> &choices, std::optional<Point> pointer);
    void render_thumbnail(Canvas &c);

    struct ClickResult {
        bool redraw = false;
        std::optional<std::pair<size_t, double>> setting;   // (choice index, new value)
        std::optional<std::pair<size_t, bool>> toggle;      // (feed index, tracking on)
    };
    // Hit-tests against the last render.
    ClickResult click(Point p);
    // Which clickable element the pointer is over (-1: none), per the last
    // render. Hover highlights only change when this does.
    int hit_at(std::optional<Point> p) const;

    Page page() const { return page_; }
    void set_page(Page p) { page_ = p; }

private:
    struct Hit {
        enum Kind { Cog, Back, Option, Toggle } kind;
        Rect rect;
        size_t choice = 0;   // Option: choice index; Toggle: feed index
        double value = 0;    // Option: value; Toggle: 1 = turn on
    };
    void render_home(Canvas &c, const Status &s);
    void render_settings(Canvas &c, const std::vector<Choice> &choices);
    void render_cog(Canvas &c);
    void render_switch(Canvas &c, Rect r, bool on, bool hover);
    bool hovered(const Rect &r) const;
    // Text shortened with an ellipsis to fit max_w.
    std::string fit(const Font &f, const std::string &text, float px, float max_w) const;

    Page page_ = Page::Home;
    Font regular_, medium_;
    std::vector<Hit> hits_;
    std::optional<Point> pointer_;
};

} // namespace babble::ui
