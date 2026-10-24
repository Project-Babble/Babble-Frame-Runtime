#include "ui/dashboard_view.h"

#include "babble_version.h"
#include "core/clock.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace babble::ui {

namespace {

// These colours copy the dark Fluent palette of Baballonia.
constexpr Color kBg = Color::hex(0x181818);
constexpr Color kPanel = Color::hex(0x202020);
constexpr Color kCard = Color::hex(0x292929);
constexpr Color kCardHover = Color::hex(0x343434);
constexpr Color kOnCard = Color::hex(0x3a3a3a), kOnCardHover = Color::hex(0x454545);   // For secondary buttons on a card.
constexpr Color kLine = Color::hex(0x343434);
constexpr Color kText = Color::hex(0xffffff);
constexpr Color kMuted = Color::hex(0x9d9d9d);
constexpr Color kAccent = Color::hex(0x0073cf);
constexpr Color kAccentHover = Color::hex(0x1684db);
constexpr Color kGreen = Color::hex(0x3fcf73);
constexpr Color kAmber = Color::hex(0xf2b33d);
constexpr Color kRed = Color::hex(0xf0544f);
constexpr Color kGrey = Color::hex(0x6b6b6b);

constexpr float kMargin = 56;

// Each eye has a gaze plot and three output meters with labels.
constexpr float kPlot = 174, kEyeGroupW = 532;
constexpr Color kViolet = Color::hex(0xa78bfa);

Color health_color(Health h) {
    switch (h) {
    case Health::Working: return kGreen;
    case Health::Starting: return kAmber;
    case Health::Fault: return kRed;
    case Health::Paused:
    case Health::Off: return kGrey;
    }
    return kGrey;
}

const char *health_word(Health h) {
    switch (h) {
    case Health::Working: return "Tracking";
    case Health::Starting: return "Starting";
    case Health::Paused: return "Paused";
    case Health::Off: return "Off";   // The user turned the feed off, or the feed has no configuration.
    case Health::Fault: return "Error";
    }
    return "";
}

std::string format_value(double v, const std::string &unit) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g%s%s", v, unit.empty() ? "" : " ", unit.c_str());
    return buf;
}

std::string format_hz(double hz) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    return buf;
}

std::string size_label(int w, int h) { return std::to_string(w) + " \u00d7 " + std::to_string(h); }

// Returns the camera settings as the JSON object of the setting.
std::string settings_json(const UsbCamera::Settings &s) {
    return nlohmann::json{{"format", s.format}, {"width", s.width},   {"height", s.height}, {"fps", s.fps},
                          {"rotate", s.rotate}, {"flip_h", s.flip_h}, {"flip_v", s.flip_v}}
        .dump();
}

const UsbCamera::Format *find_format(const UsbCamera &cam, const std::string &key) {
    for (const auto &f : cam.formats)
        if (f.key == key)
            return &f;
    return nullptr;
}

// Moves the size and the rate of s to the nearest ones that its format has, so that the page always shows a mode
// that the camera offers.
void fit_mode(const UsbCamera &cam, UsbCamera::Settings &s) {
    const UsbCamera::Format *f = find_format(cam, s.format);
    if (!f || f->sizes.empty())
        return;
    const UsbCamera::Size *best = &f->sizes.front();
    for (const auto &z : f->sizes)
        if (std::abs(z.width - s.width) + std::abs(z.height - s.height) <
            std::abs(best->width - s.width) + std::abs(best->height - s.height))
            best = &z;
    s.width = best->width;
    s.height = best->height;
    if (best->fps.empty())
        return;
    int fps = best->fps.front();
    for (int r : best->fps)
        if (std::abs(r - s.fps) < std::abs(fps - s.fps))
            fps = r;
    s.fps = fps;
}

const UsbCamera::Size *find_size(const UsbCamera &cam, const UsbCamera::Settings &s) {
    if (const auto *f = find_format(cam, s.format))
        for (const auto &z : f->sizes)
            if (z.width == s.width && z.height == s.height)
                return &z;
    return nullptr;
}

}

std::string camera_setting_id(const std::string &camera_id) {
    std::string token;
    for (char ch : camera_id)
        token += ch == '~' ? "~0" : ch == '/' ? "~1" : std::string(1, ch);
    return "/sources/face/cameras/" + token;
}

bool DashboardView::load_fonts(const std::filesystem::path &dir) {
    return regular_.load(dir / "NotoSans-Regular.ttf") && medium_.load(dir / "NotoSans-Medium.ttf");
}

bool DashboardView::load_icons(const std::filesystem::path &dir) {
    logo_small_ = load_png(dir / "bfr_logo_48.png");
    logo_thumb_ = load_png(dir / "bfr_logo_128.png");
    constexpr const char *names[] = {"mouth", "eye", "rate", "camera", "logs", "document", "settings", "home", "capture"};
    for (size_t i = 0; i < icons_.size(); i++) {
        icons_[i] = load_png(dir / (std::string("fluent_") + names[i] + ".png"));
        if (icons_[i] && (icons_[i]->width != 40 || icons_[i]->height != 40))
            icons_[i].reset();
    }
    return logo_small_ && logo_thumb_;
}

void DashboardView::render_icon(Canvas &c, Icon icon, float x, float y, Color color) {
    const auto &img = icons_[size_t(icon)];
    if (!img)
        return;   // The assets are optional, and text and hit targets work without them.
    for (int iy = 0; iy < img->height; iy++)
        for (int ix = 0; ix < img->width; ix++) {
            const uint8_t alpha = img->rgba[(size_t(iy) * img->width + ix) * 4 + 3];
            if (alpha)
                c.blend(int(x) + ix, int(y) + iy, color, float(alpha) / 255);
        }
}

float DashboardView::render_block(Canvas &c, float y, Icon icon, float row_h) {
    c.fill_rect({kMargin, y + 4, float(c.width()) - 2 * kMargin, row_h - 12}, kCard, 6);
    render_icon(c, icon, kMargin + 20, y + row_h / 2 - 22, kMuted);
    return kMargin + 80;
}

bool DashboardView::hovered(const Rect &r) const { return pointer_ && r.contains(pointer_->x, pointer_->y); }

std::string DashboardView::fit(const Font &f, const std::string &text, float px, float max_w) const {
    if (f.measure(text, px) <= max_w)
        return text;
    std::string s = text;
    while (!s.empty()) {
        // Remove whole UTF-8 sequences.
        do
            s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xc0) == 0x80);
        if (f.measure(s + "…", px) <= max_w)
            return s + "…";
    }
    return "…";
}

void DashboardView::render(Canvas &c, const Status &status, const std::vector<Choice> &choices,
                           std::optional<Point> pointer) {
    pointer_ = pointer;
    hits_.clear();
    c.clear(kBg);
    if (page_ == Page::Home) {
        render_home(c, status);
        render_link(c, status.link, render_visualizer_button(c, status.visualizer) - 32);
    } else if (page_ == Page::Cameras) {
        render_cameras(c, choices, status);
    } else if (page_ == Page::Diagnostics) {
        render_diagnostics(c, choices, status);
    } else if (page_ == Page::Eye) {
        render_eye_page(c, choices, status);
    } else if (page_ == Page::Mouth) {
        render_mouth_page(c, choices, status);
    } else if (page_ == Page::Outputs) {
        render_outputs(c, status);
    } else if (page_ == Page::CameraOptions) {
        render_camera_options(c, status);
    } else if (page_ == Page::CameraResolution) {
        render_camera_resolution(c, status);
    } else {
        render_settings(c, choices, status);
    }
    render_cog(c);
}

void DashboardView::render_home(Canvas &c, const Status &s) {
    const float W = float(c.width());
    float title_x = kMargin;
    if (logo_small_) {
        c.image(*logo_small_, int(kMargin), 48, 6);
        title_x += logo_small_->width + 16;
    }
    medium_.draw(c, title_x, 78, "Babble Frame Runtime", 32, kText);
    regular_.draw(c, title_x, 108, BABBLE_VERSION_DISPLAY, 20, kMuted);
    render_update_pill(c, title_x + medium_.measure("Babble Frame Runtime", 32) + 20, 78, s.update);

    render_eye(c, kMargin, 146, "Left Eye", s.eyes[0]);
    render_eye(c, W - kMargin - kEyeGroupW, 146, "Right Eye", s.eyes[1]);

    const FeedStatus *feeds[2] = {&s.mouth, &s.eye};
    const char *names[2] = {"Mouth Tracking", "Eye Tracking"};
    const float cw = (W - 2 * kMargin - 24) / 2, ch = 132, cy = 470;
    for (int i = 0; i < 2; i++) {
        const FeedStatus &f = *feeds[i];
        const Rect r{kMargin + i * (cw + 24), cy, cw, ch};
        c.fill_rect(r, kCard, 6);
        render_icon(c, i == 0 ? Icon::Mouth : Icon::Eye, r.x + 20, r.y + 30, kMuted);
        medium_.draw(c, r.x + 76, r.y + 47, names[i], 28, kText);
        std::string detail = health_word(f.health);
        if (!f.available)
            detail = "Not configured";
        else if (!f.tracking)
            detail = "Off";
        else if (f.health == Health::Working)
            detail = "Tracking" + (f.hz > 0 ? " \u00b7 " + format_hz(f.hz) : "");
        else if (f.health == Health::Paused)
            detail = "Headset not worn";
        else if (f.health == Health::Fault && !f.detail.empty())
            detail = f.detail;
        float text_right = r.x + r.w - 24;
        if (f.available) {
            const Rect sw{r.x + r.w - 32 - 92, r.y + (ch - 52) / 2, 92, 52};
            const Rect hit{sw.x - 24, r.y, r.x + r.w - (sw.x - 24), r.h};
            // When the switch is on, its colour shows the health of the feed.
            render_switch(c, sw, f.tracking, hovered(hit), health_color(f.health));
            hits_.push_back({Hit::Toggle, hit, size_t(i), f.tracking ? 0.0 : 1.0});
            text_right = hit.x - 12;
        }
        regular_.draw(c, r.x + 76, r.y + 79, fit(regular_, detail, 22, text_right - r.x - 76), 22, kMuted);
        const char *source = i == 1 ? "Headset eye tracking" : s.mouth_source == "usb" ? "USB camera" : "Headset mouth cameras";
        regular_.draw(c, r.x + 76, r.y + 111, source, 19, kMuted);
    }
}

void DashboardView::render_eye(Canvas &c, float x, float y, const char *name, const EyeReading &e) {
    constexpr float kRange = 45;
    c.fill_rect({x, y, kEyeGroupW, 300}, kPanel, 6);
    medium_.draw(c, x + 20, y + 36, name, 26, kText);
    const Rect plot{x + 20, y + 58, kPlot, kPlot};
    c.fill_rect(plot, kBg, 4);
    c.stroke_rect(plot, kLine, 4, 1);
    const float cx = plot.x + kPlot / 2, cy = plot.y + kPlot / 2, half = kPlot / 2 - 12;
    c.line(plot.x + 12, cy, plot.x + kPlot - 12, cy, 1, kLine);
    c.line(cx, plot.y + 12, cx, plot.y + kPlot - 12, 1, kLine);
    std::string caption = "No gaze data";
    if (e.gaze) {
        const float dx = float(std::clamp(e.yaw_deg / kRange, -1.0, 1.0)) * half;
        const float dy = float(std::clamp(e.pitch_deg / kRange, -1.0, 1.0)) * half;
        c.fill_circle(cx + dx, cy - dy, 8, kAccent);
        char buf[64];
        snprintf(buf, sizeof(buf), "%+.0f\u00b0  %+.0f\u00b0", e.yaw_deg, e.pitch_deg);
        caption = buf;
    }
    regular_.draw(c, plot.x + (kPlot - regular_.measure(caption, 20)) / 2, y + 259, caption, 20, kMuted);

    const float meter_x = x + 220, meter_w = kEyeGroupW - 240;
    auto meter = [&](float row, const char *label, bool available, double value, Color color, bool baseline) {
        regular_.draw(c, meter_x, row, label, 22, kText);
        char buf[32];
        if (available)
            snprintf(buf, sizeof(buf), "%.0f%%", value * 100);
        else
            snprintf(buf, sizeof(buf), "%s", baseline && e.raw ? "Calibrating" : "No data");
        regular_.draw(c, meter_x + meter_w - regular_.measure(buf, 20), row, buf, 20, kMuted);
        const Rect bar{meter_x, row + 13, meter_w, 14};
        c.fill_rect(bar, kLine, 3);
        const float frac = available ? float(std::clamp(value, 0.0, 1.0)) : 0;
        if (frac > 0)
            c.fill_rect({bar.x, bar.y, bar.w * frac, bar.h}, color, 3);
        if (baseline)
            c.fill_rect({bar.x + bar.w / 2 - 1, bar.y - 3, 2, bar.h + 6}, kText);
    };
    meter(y + 83, "Openness", e.lid, e.openness, kAccent, false);
    meter(y + 156, "Widen", e.lid, e.widen, kAccent, false);
    meter(y + 229, "Dilation", e.dilation_ok, e.dilation, kViolet, true);
    if (e.raw) {
        char buf[128];
        if (e.cal)
            snprintf(buf, sizeof(buf), "Raw %.3f   \u00b7   Neutral %.3f   \u00b7   Wide %.3f", e.raw_lid, e.neutral, e.wide_raw);
        else
            snprintf(buf, sizeof(buf), "Raw %.3f   \u00b7   Calibrating eyelid baseline", e.raw_lid);
        regular_.draw(c, x + 20, y + 285, fit(regular_, buf, 18, kEyeGroupW - 40), 18, kMuted);
    } else {
        regular_.draw(c, x + 20, y + 285, "Waiting for eye readings", 18, kMuted);
    }
}

float DashboardView::render_visualizer_button(Canvas &c, AppState state) {
    if (state == AppState::Unavailable)
        return float(c.width()) - kMargin;
    const Rect r = render_button(c, float(c.width()) - kMargin, float(c.height()) - 116,
                                 state == AppState::Running ? "Close Visualizer" : "Open Visualizer",
                                 state == AppState::Stopped);
    hits_.push_back({Hit::Visualizer, r});
    return r.x;
}

// Draws the restart button, which needs a second click in less than 3 s.
void DashboardView::render_restart_button(Canvas &c) {
    const bool armed = restart_armed(mono_ns());
    const std::string text = armed ? "Confirm Restart" : "Restart Runtime";
    const Rect r = render_button(c, float(c.width()) - kMargin, float(c.height()) - 116, text, false);
    if (armed) {
        c.fill_rect(r, kRed, 4);
        medium_.draw(c, r.x + 24, r.y + (r.h + medium_.cap_height(24)) / 2, text, 24, kText);
    }
    regular_.draw(c, 150, r.y + 39,
                  fit(regular_, "Restarts tracking. SteamVR stays open.", 22, r.x - 32 - 150), 22, kMuted);
    hits_.push_back({Hit::Restart, r});
}

// Draws the output status on one line between the cog and the visualizer button.
void DashboardView::render_link(Canvas &c, const LinkReading &link, float right) {
    if (!link.available)
        return;
    const float x = 150, y = float(c.height()) - 84, px = 26, dot_r = 9;
    c.fill_circle(x + dot_r, y, dot_r, link.linked && !link.no_data && !link.restart ? kGreen : kAmber);
    const float label_x = x + 2 * dot_r + 14;
    const float base = y + medium_.cap_height(px) / 2;
    const char *label = link.native ? "VRChat" : link.osc ? "OSC" : "PC";
    medium_.draw(c, label_x, base, label, px, kText);
    std::string text;
    if (link.osc) {
        text = link.address.empty() ? "Babble OSC, set a PC address" : "Babble OSC to " + link.address;
        if (!link.address.empty() && !link.linked)
            text += "  \u00b7  no data";
    } else if (link.native) {
        const std::string where = link.path == "pc"      ? "native, on the PC  \u00b7  " + link.address
                                  : link.path == "frame" ? std::string("native, on this headset")
                                  : link.path.empty()    ? std::string("native, waiting for Steam Link")
                                                         : "native  \u00b7  " + link.address;
        text = link.linked || link.path.empty() ? where : where + "  \u00b7  no data";
    } else if (link.linked) {
        const std::string path = link.path == "lan" ? "LAN" : link.path == "usb" ? "USB" : link.path;
        text = (link.pc.empty() ? std::string("VRCFaceTracking") : link.pc) + " via " + path + "  \u00b7  ";
        // A problem replaces the address because the line has no space for both.
        if (link.no_data)
            text += "no data reaching it";
        else if (link.restart)
            text += "Mode changed, restart VRCFT";
        else
            text += link.address;
        if (!link.no_data && !link.restart && link.rtt_ms >= 0) {
            char ms[32];
            snprintf(ms, sizeof(ms), "  \u00b7  %.0f ms", link.rtt_ms);
            text += ms;
        }
    } else {
        text = "Searching for VRCFaceTracking\u2026";
    }
    if (link.more > 0)
        text += "  \u00b7  +" + std::to_string(link.more) + " more";
    const float text_x = label_x + medium_.measure(label, px) + 16;
    regular_.draw(c, text_x, base, fit(regular_, text, px, right - text_x), px, link.linked ? kText : kMuted);
    hits_.push_back({Hit::OpenOutputs, {x - 12, y - 40, right - x + 12, 80}});
}

void DashboardView::render_switch(Canvas &c, Rect r, bool on, bool hover, std::optional<Color> on_color) {
    // Draw a compact Fluent switch inside the larger VR hit area.
    const Rect track{r.x + 8, r.y + 6, r.w - 16, r.h - 12};
    Color fill = hover ? kCardHover : kBg;
    if (on && on_color) {   // The colour is lighter during hover, as the accent is.
        const auto mix = [&](uint8_t v) { return uint8_t(hover ? v + (255 - v) * 0.15f : v); };
        fill = {mix(on_color->r), mix(on_color->g), mix(on_color->b), 255};
    } else if (on) {
        fill = hover ? kAccentHover : kAccent;
    }
    c.fill_rect(track, fill, track.h / 2);
    if (!on)
        c.stroke_rect(track, hover ? kText : kMuted, track.h / 2, 2);
    const float knob = track.h / 2 - 6;
    c.fill_circle(on ? track.x + track.w - track.h / 2 : track.x + track.h / 2,
                  track.y + track.h / 2, knob, on ? kText : kMuted);
}

void DashboardView::render_back(Canvas &c, const char *title) {
    const Rect back{kMargin - 20, 36, std::max(300.0f, medium_.measure(title, 32) + 76), 76};
    if (hovered(back))
        c.fill_rect(back, kCardHover, 4);
    c.line(kMargin + 14, 62, kMargin + 2, 74, 3, kText);
    c.line(kMargin + 2, 74, kMargin + 14, 86, 3, kText);
    medium_.draw(c, kMargin + 42, 86, title, 32, kText);
    hits_.push_back({Hit::Back, back});
}

float DashboardView::render_segments(Canvas &c, float right, float y, const std::vector<std::string> &labels,
                                     int selected, const std::function<void(size_t, const Rect &)> &hit, float seg_h,
                                     float seg_w) {
    const float pad = 4;
    const float ctl_w = seg_w * float(labels.size()) + 2 * pad;
    const Rect ctl{right - ctl_w, y, ctl_w, seg_h + 2 * pad};
    c.fill_rect(ctl, kPanel, 4);
    for (size_t k = 0; k < labels.size(); k++) {
        const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
        const bool on = int(k) == selected;
        if (on || hovered(seg))
            c.fill_rect(seg, on ? kAccent : kCardHover, 3);
        const float px = 24;
        medium_.draw(c, seg.x + (seg.w - medium_.measure(labels[k], px)) / 2,
                     seg.y + (seg.h + medium_.cap_height(px)) / 2, labels[k], px, on ? kText : kMuted);
        hit(k, seg);
    }
    return ctl.x;
}

// Gives the top and the height of the rows of a page. The rows get shorter when there are many, so that they stay
// above the space at the bottom. If rows of 92 px do not fit, the rows start higher and can be as short as 80 px.
DashboardView::RowLayout DashboardView::row_layout(const Canvas &c, size_t rows, float below) const {
    const bool tight = float(c.height()) - below - 160 < 92.0f * float(rows);
    const float top = tight ? 124 : 160, min_h = tight ? 80 : 92;
    const float avail = float(c.height()) - below - top;
    return {top, rows ? std::clamp(std::floor(avail / float(rows)), min_h, 124.0f) : 124};
}

// Draws a row that opens another page, with a hint and a chevron.
void DashboardView::render_link_row(Canvas &c, float y, float row_h, Icon icon, const std::string &label,
                                    const std::string &hint, Hit::Kind kind) {
    const float W = float(c.width()), d = (124 - row_h) / 2;
    const Rect row{kMargin, y + 4, W - 2 * kMargin, row_h - 12};
    c.fill_rect(row, hovered(row) ? kCardHover : kCard, 6);
    render_icon(c, icon, kMargin + 20, y + row_h / 2 - 22, kMuted);
    const float tx = kMargin + 80, chev_x = W - kMargin - 24, mid = y + row_h / 2 - 2;
    medium_.draw(c, tx, y + 48 - d, label, 26, kText);
    regular_.draw(c, tx, y + 83 - d, fit(regular_, hint, 21, chev_x - 28 - tx), 21, kMuted);
    c.line(chev_x - 10, mid - 10, chev_x, mid, 3, kMuted);
    c.line(chev_x, mid, chev_x - 10, mid + 10, 3, kMuted);
    hits_.push_back({kind, row});
}

// Draws the choices of a page from y, and returns the y after the last row.
float DashboardView::render_choice_rows(Canvas &c, float y, float row_h, const std::vector<Choice> &choices,
                                        ChoicePage page, const Status &s) {
    const float W = float(c.width()), d = (124 - row_h) / 2;
    const float seg_h = std::min(64.0f, row_h - 28), seg_y = 26 - d + (64 - seg_h) / 2;   // The controls of a row.
    for (size_t i = 0; i < choices.size(); i++) {
        const Choice &ch = choices[i];
        if (ch.page != page)
            continue;
        if (ch.boolean) {
            render_switch_row(c, y, i, ch, row_h);
            y += row_h;
            continue;
        }
        const float tx = render_block(c, y, Icon::Rate, row_h);
        const float seg_w = 128, pad = 4;
        const float ctl_w = seg_w * float(ch.options.size()) + 2 * pad;
        const Rect ctl{W - kMargin - 16 - ctl_w, y + seg_y, ctl_w, seg_h + 2 * pad};
        const auto live = s.choice_hints.find(ch.id);
        const std::string &hint = live != s.choice_hints.end() ? live->second : ch.hint;
        medium_.draw(c, tx, y + 48 - d, fit(medium_, ch.label, 26, ctl.x - tx - 24), 26, kText);
        regular_.draw(c, tx, y + 83 - d, fit(regular_, hint, 21, ctl.x - tx - 24), 21, kMuted);
        c.fill_rect(ctl, kPanel, 4);
        for (size_t k = 0; k < ch.options.size(); k++) {
            const double v = ch.options[k];
            const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
            const bool selected = std::fabs(v - ch.value) < 1e-6;
            if (selected || hovered(seg))
                c.fill_rect(seg, selected ? kAccent : kCardHover, 3);
            const std::string label = k < ch.option_labels.size() ? ch.option_labels[k] : format_value(v, ch.unit);
            const float px = 24;
            medium_.draw(c, seg.x + (seg.w - medium_.measure(label, px)) / 2,
                         seg.y + (seg.h + medium_.cap_height(px)) / 2, label, px, selected ? kText : kMuted);
            hits_.push_back({Hit::Option, seg, i, v});
        }
        y += row_h;
    }
    return y;
}

void DashboardView::render_settings(Canvas &c, const std::vector<Choice> &choices, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Settings");
    render_restart_button(c);
    hits_.push_back({Hit::OpenDiagnostics, render_button(c, W - kMargin, 42, "Diagnostics", false)});

    // The Eye Tracking and Mouth Tracking rows show the current value of each setting on their page.
    std::string eye, mouth;
    for (const Choice &ch : choices) {
        std::string *summary = ch.page == ChoicePage::Eye ? &eye : ch.page == ChoicePage::Mouth ? &mouth : nullptr;
        if (!summary)
            continue;
        std::string value = ch.boolean ? (ch.value != 0 ? "On" : "Off") : format_value(ch.value, ch.unit);
        for (size_t k = 0; k < ch.options.size() && k < ch.option_labels.size(); k++)
            if (std::fabs(ch.options[k] - ch.value) < 1e-6)
                value = ch.option_labels[k];
        *summary += (summary->empty() ? "" : "  ·  ") + ch.label + " " + value;
    }
    if (s.mouth_source_selectable)
        mouth += std::string(mouth.empty() ? "" : "  ·  ") + (s.mouth_source == "usb" ? "USB camera" : "Headset");

    size_t rows = (s.outputs.empty() ? 0 : 1) + (eye.empty() ? 0 : 1) + 1;
    for (const Choice &ch : choices)
        rows += ch.page == ChoicePage::Settings;
    const RowLayout l = row_layout(c, rows, 130);
    float y = l.top;
    if (!s.outputs.empty()) {   // The Outputs row lists the outputs that are on.
        std::string on;
        for (const OutputModule &m : s.outputs)
            if (m.on)
                on += (on.empty() ? "" : ", ") + m.label;
        render_link_row(c, y, l.row_h, Icon::Settings, "Outputs", on.empty() ? "All outputs are off." : "On: " + on,
                        Hit::OpenOutputs);
        y += l.row_h;
    }
    if (!eye.empty()) {
        render_link_row(c, y, l.row_h, Icon::Eye, "Eye Tracking", eye, Hit::OpenEye);
        y += l.row_h;
    }
    render_link_row(c, y, l.row_h, Icon::Mouth, "Mouth Tracking", mouth.empty() ? "USB camera" : mouth,
                    Hit::OpenMouth);
    y += l.row_h;
    render_choice_rows(c, y, l.row_h, choices, ChoicePage::Settings, s);
}

void DashboardView::render_eye_page(Canvas &c, const std::vector<Choice> &choices, const Status &s) {
    render_back(c, "Eye Tracking");
    const size_t rows = size_t(std::count_if(choices.begin(), choices.end(),
                                             [](const Choice &ch) { return ch.page == ChoicePage::Eye; }));
    const RowLayout l = row_layout(c, rows, 130);
    render_choice_rows(c, l.top, l.row_h, choices, ChoicePage::Eye, s);
}

void DashboardView::render_mouth_page(Canvas &c, const std::vector<Choice> &choices, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Mouth Tracking");
    const bool selectable = s.mouth_source_selectable;
    const bool usb = !selectable || s.mouth_source == "usb";
    size_t rows = selectable ? 1 : 0;
    for (const Choice &ch : choices)
        rows += ch.page == ChoicePage::Mouth;
    const RowLayout l = row_layout(c, rows, 130 + (usb ? 84 : 0));
    const float row_h = l.row_h, d = (124 - row_h) / 2;
    const float seg_h = std::min(64.0f, row_h - 28), seg_y = 26 - d + (64 - seg_h) / 2;
    float y = render_choice_rows(c, l.top, row_h, choices, ChoicePage::Mouth, s);

    // Show the mouth source if there is a choice, and for USB a link to the camera page.
    if (selectable) {
        const float tx = render_block(c, y, Icon::Mouth, row_h);
        const float ctl_x = render_segments(c, W - kMargin - 16, y + seg_y, {"Headset", "USB"}, usb ? 1 : 0,
                                           [&](size_t k, const Rect &r) {
                                               Hit h{Hit::MouthSource, r};
                                               h.text = k == 1 ? "usb" : "frunk";
                                               hits_.push_back(h);
                                           },
                                           seg_h);
        medium_.draw(c, tx, y + 48 - d, "Mouth Tracking Source", 26, kText);
        regular_.draw(c, tx, y + 83 - d,
                      fit(regular_, "Use the headset cameras or a USB camera.", 21, ctl_x - tx - 24), 21, kMuted);
        y += row_h;
    }
    if (usb) {
        const Rect row{kMargin, y + 4, W - 2 * kMargin, 80};
        c.fill_rect(row, hovered(row) ? kCardHover : kCard, 6);
        render_icon(c, Icon::Camera, kMargin + 20, y + 24, kMuted);
        medium_.draw(c, kMargin + 80, y + 54, "USB Camera", 26, kText);
        std::string current = "Automatic";
        for (const auto &cam : s.cameras)
            if (cam.id == s.camera)
                current = cam.label;
        if (!s.camera.empty() && current == "Automatic")
            current = "Not connected";
        const float chev_x = W - kMargin - 24;
        const std::string label = fit(regular_, current, 24, 520);
        regular_.draw(c, chev_x - 28 - regular_.measure(label, 24), y + 54, label, 24, kMuted);
        c.line(chev_x - 10, y + 34, chev_x, y + 44, 3, kMuted);
        c.line(chev_x, y + 44, chev_x - 10, y + 54, 3, kMuted);
        hits_.push_back({Hit::OpenCameras, row});
    }
}

// Draws the runtime update as a small button next to the title, with its left edge at x.
void DashboardView::render_update_pill(Canvas &c, float x, float baseline, const UpdateNotice &u) {
    using S = UpdateNotice::State;
    char pct[64];
    std::string text;
    bool primary = true, clickable = true;
    switch (u.state) {
    case S::Hidden: return;
    case S::Available: text = "Update to " + u.version; break;
    case S::Downloading:
        snprintf(pct, sizeof(pct), "Downloading %.0f%% \u00b7 Cancel", double(u.progress) * 100.0);
        text = pct;
        primary = false;
        break;
    case S::Installing:
        text = "Installing " + u.version + "...";
        primary = clickable = false;
        break;
    case S::RestartNeeded:
        text = update_armed(mono_ns()) ? "Click again to restart SteamVR" : "Restart SteamVR to finish";
        break;
    case S::Failed:
        text = "Update failed \u00b7 Retry";
        primary = false;
        break;
    }
    const float px = 20, pad = 18, h = 40;
    const Rect r{x, baseline - medium_.cap_height(px) / 2 - h / 2, medium_.measure(text, px) + 2 * pad, h};
    const bool hover = clickable && hovered(r);
    c.fill_rect(r, primary ? (hover ? kAccentHover : kAccent) : (hover ? kCardHover : kCard), 4);
    medium_.draw(c, r.x + pad, r.y + (h + medium_.cap_height(px)) / 2, text, px, kText);
    if (clickable) {
        Hit hit{Hit::Update, r};
        hit.text = u.state == S::RestartNeeded ? "restart" : "";
        hits_.push_back(hit);
    }
}

// Draws a button, which uses the accent colour if it is primary.
Rect DashboardView::render_button(Canvas &c, float right, float y, const std::string &text, bool primary, bool on_card) {
    const float px = 24, pad = 24, h = 64;
    const float w = medium_.measure(text, px) + 2 * pad;
    const Rect r{right - w, y, w, h};
    const Color base = on_card ? kOnCard : kCard, hover = on_card ? kOnCardHover : kCardHover;
    c.fill_rect(r, primary ? (hovered(r) ? kAccentHover : kAccent) : (hovered(r) ? hover : base), 4);
    medium_.draw(c, r.x + pad, r.y + (h + medium_.cap_height(px)) / 2, text, px, kText);
    return r;
}

// Draws an on/off setting row with a switch that a click near the switch changes.
void DashboardView::render_switch_row(Canvas &c, float y, size_t index, const Choice &ch, float row_h) {
    const float W = float(c.width());
    const float tx = render_block(c, y, ch.page == ChoicePage::Eye ? Icon::Eye : Icon::Camera, row_h);
    const bool on = ch.value != 0;
    const float d = (124 - row_h) / 2;   // This offset keeps the content of short rows in the centre.
    const Rect sw{W - kMargin - 92, y + 36 - d, 92, 52};
    const Rect hit{sw.x - 24, y, W - kMargin - (sw.x - 24), row_h};
    render_switch(c, sw, on, hovered(hit));
    hits_.push_back({Hit::Option, hit, index, on ? 0.0 : 1.0});
    medium_.draw(c, tx, y + 48 - d, fit(medium_, ch.label, 26, hit.x - tx - 24), 26, kText);
    regular_.draw(c, tx, y + 83 - d, fit(regular_, ch.hint, 21, hit.x - tx - 24), 21, kMuted);
}

// Draws the dataset capture row with its status, its main button and the Remove button.
void DashboardView::render_capture_row(Canvas &c, float y, float row_h, const CaptureRow &row) {
    using S = CaptureRow::State;
    const float W = float(c.width());
    const float d = (124 - row_h) / 2;
    char pct[64];
    std::string hint, primary, remove;
    switch (row.state) {
    case S::Hidden: return;
    case S::Checking: hint = "Checking for the app..."; break;
    case S::NotInstalled:
        hint = row.detail.empty() ? "A guided recording session for the dataset. Optional; not installed." : row.detail;
        primary = "Install";
        break;
    case S::Downloading:
        snprintf(pct, sizeof(pct), "Downloading %.0f%%", double(row.progress) * 100.0);
        hint = pct;
        primary = "Cancel";
        break;
    case S::Installing: hint = "Installing..."; break;
    case S::Installed:
        hint = "v" + row.version + " installed";
        primary = "Launch";
        remove = "Remove";
        break;
    case S::UpdateAvailable:
        hint = "v" + row.version + " installed; v" + row.latest + " available";
        primary = "Update";
        remove = "Remove";
        break;
    case S::Running:
        hint = row.recording ? "Running \u00b7 recording" : "Running";
        primary = "Stop";
        break;
    case S::Failed:
        hint = row.detail.empty() ? "Something went wrong." : row.detail;
        primary = "Retry";
        break;
    }
    if (row.uploads_pending > 0) {
        snprintf(pct, sizeof(pct), " \u00b7 %d upload%s pending", row.uploads_pending, row.uploads_pending == 1 ? "" : "s");
        hint += pct;
        if (!row.upload_stage.empty()) {
            snprintf(pct, sizeof(pct), " (%s %.0f%%)", row.upload_stage.c_str(), double(row.upload_progress) * 100.0);
            hint += pct;
        }
    }
    const float tx = render_block(c, y, Icon::Capture, row_h);
    float left = W - kMargin - 16;
    if (!primary.empty()) {
        const Rect b = render_button(c, left, y + 28 - d, primary, true);
        hits_.push_back({Hit::CapturePrimary, b});
        left = b.x - 16;
    }
    if (!remove.empty()) {
        const Rect b = render_button(c, left, y + 28 - d, remove, false, true);
        hits_.push_back({Hit::CaptureRemove, b});
        left = b.x;
    }
    medium_.draw(c, tx, y + 48 - d, "Dataset Capture", 26, kText);
    regular_.draw(c, tx, y + 83 - d, fit(regular_, hint, 21, left - tx - 24), 21, kMuted);
}

void DashboardView::render_diagnostics(Canvas &c, const std::vector<Choice> &choices, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Diagnostics");
    // Use shorter rows than Settings so that four rows and the log paths fit.
    constexpr float kRow = 112, d = (124 - kRow) / 2;
    float y = 160;
    {
        const float tx = render_block(c, y, Icon::Logs, kRow);
        const bool on = s.verbose_minutes_left > 0;
        const Rect sw{W - kMargin - 92, y + 36 - d, 92, 52};
        const Rect hit{sw.x - 24, y, W - kMargin - (sw.x - 24), kRow};
        render_switch(c, sw, on, hovered(hit));
        Hit h{Hit::Verbose, hit};
        h.value = on ? 0.0 : 1.0;
        hits_.push_back(h);
        medium_.draw(c, tx, y + 48 - d, "Verbose Logging", 26, kText);
        const std::string hint = on ? "Detailed logs enabled for " + std::to_string(s.verbose_minutes_left) + " more minutes."
                                    : "Record detailed logs for 10 minutes.";
        regular_.draw(c, tx, y + 83 - d, fit(regular_, hint, 21, hit.x - tx - 24), 21, kMuted);
        y += kRow;
    }
    {   // The user can send a diagnostics report with a problem.
        const float tx = render_block(c, y, Icon::Document, kRow);
        const Rect b = render_button(c, W - kMargin - 16, y + 28 - d, "Save Report", true);
        hits_.push_back({Hit::WriteDiagnostics, b});
        medium_.draw(c, tx, y + 48 - d, "Diagnostics Report", 26, kText);
        const std::string hint = s.last_diagnostics.empty() ? "Save the current state to share with a bug report."
                                                          : "Saved: " + s.last_diagnostics;
        regular_.draw(c, tx, y + 83 - d, fit(regular_, hint, 21, b.x - tx - 24), 21, kMuted);
        y += kRow;
    }
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i].page != ChoicePage::Diagnostics || !choices[i].boolean)
            continue;
        render_switch_row(c, y, i, choices[i], kRow);
        y += kRow;
    }
    if (s.capture.state != CaptureRow::State::Hidden) {
        render_capture_row(c, y, kRow, s.capture);
        y += kRow;
    }
    y += 38;
    // Put the paths to the right of the Dashboard button if they are low on the page.
    const float x0 = y + 38 > 620 ? 150 : kMargin;
    for (const auto &[label, path] : {std::pair<const char *, const std::string &>{"Log File", s.log_file},
                                      {"Stats File", s.stats_file}}) {
        if (path.empty())
            continue;
        medium_.draw(c, x0, y, label, 22, kText);
        regular_.draw(c, x0 + 130, y, fit(regular_, path, 20, W - kMargin - x0 - 130), 20, kMuted);
        y += 38;
    }
}

float DashboardView::render_text_field(Canvas &c, float right, float y, const std::string &id, const std::string &shown) {
    const Rect r = render_button(c, right, y, shown, false, true);
    Hit h{Hit::EditText, r};
    h.text = id;
    hits_.push_back(h);
    return r.x;
}

// Draws the PC address and one row for each output module, with its switch, its settings and its status.
void DashboardView::render_outputs(Canvas &c, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Outputs");
    if (!s.output_error.empty())
        regular_.draw(c, kMargin, 142, fit(regular_, s.output_error, 21, W - 2 * kMargin), 21, kRed);

    const float bottom = float(c.height()) - 130;   // Keep the corner of the cog clear.
    const float row_h = std::clamp(std::floor((bottom - 160) / float(s.outputs.size() + 1)), 92.0f, 116.0f);
    const float d = (124 - row_h) / 2;
    auto hit_setting = [&](const Rect &r, const std::string &id, const std::string &json) {
        Hit h{Hit::OutputSetting, r};
        h.text = id;
        h.json = json;
        hits_.push_back(h);
    };

    float y = 160;
    {
        const float tx = render_block(c, y, Icon::Settings, row_h);
        float left = render_text_field(c, W - kMargin - 16, y + 30 - d, "/pc_address",
                                       s.pc_address.empty() ? "Steam Link Host" : s.pc_address);
        if (!s.pc_address.empty()) {
            const Rect clear = render_button(c, left - 16, y + 30 - d, "Clear", false, true);
            hit_setting(clear, "/pc_address", "\"\"");
            left = clear.x;
        }
        medium_.draw(c, tx, y + 48 - d, "PC Address", 26, kText);
        regular_.draw(c, tx, y + 83 - d,
                      fit(regular_,
                          s.pc_address.empty() ? "Found from Steam Link. For WiVRn, ALVR or others, enter your PC's IP."
                                               : "Used instead of the Steam Link host.",
                          21,
                          left - tx - 24),
                      21, kMuted);
        y += row_h;
    }
    for (const OutputModule &m : s.outputs) {
        if (y + row_h > bottom + 12)
            break;
        c.fill_rect({kMargin, y + 4, W - 2 * kMargin, row_h - 12}, kCard, 6);
        const float tx = kMargin + 28;
        const Rect sw{W - kMargin - 16 - 92, y + 36 - d, 92, 52};
        const Rect sw_hit{sw.x - 12, y + 4, W - kMargin - (sw.x - 12), row_h - 12};
        // When the switch is on, its colour shows if the output works, as for the eye and mouth feeds.
        render_switch(c, sw, m.on, hovered(sw_hit), m.ok ? kGreen : kAmber);
        hit_setting(sw_hit, "/outputs/" + m.id, m.on ? "false" : "true");
        float left = sw_hit.x - 12;
        for (auto it = m.options.rbegin(); it != m.options.rend(); ++it) {
            const ModuleOption &o = *it;
            if (o.text) {
                left = render_text_field(c, left, y + 30 - d, o.id, o.value) - 16;
                continue;
            }
            const float seg_w = 96, seg_h = 56, pad = 4;
            const float ctl_w = seg_w * float(o.values.size()) + 2 * pad;
            const Rect ctl{left - ctl_w, y + 30 - d, ctl_w, seg_h + 2 * pad};
            c.fill_rect(ctl, kPanel, 4);
            for (size_t k = 0; k < o.values.size(); k++) {
                const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
                const bool on = o.values[k] == o.value;
                if (on || hovered(seg))
                    c.fill_rect(seg, on ? kAccent : kCardHover, 3);
                const std::string label = k < o.labels.size() ? o.labels[k] : o.values[k];
                const float px = 22;
                medium_.draw(c, seg.x + (seg.w - medium_.measure(label, px)) / 2,
                             seg.y + (seg.h + medium_.cap_height(px)) / 2, label, px, on ? kText : kMuted);
                hit_setting(seg, o.id, o.values[k]);
            }
            left = ctl.x - 16;
        }
        medium_.draw(c, tx, y + 48 - d, fit(medium_, m.label, 26, left - tx - 8), 26, kText);
        const std::string &line = m.on && !m.status.empty() ? m.status : m.hint;
        regular_.draw(c, tx, y + 83 - d, fit(regular_, line, 21, left - tx - 8), 21, kMuted);
        y += row_h;
    }
}

// Draws the USB camera page with the automatic choice and each camera from the last refresh.
void DashboardView::render_cameras(Canvas &c, const std::vector<Choice> &choices, const Status &s) {
    const float W = float(c.width());
    render_back(c, "USB Camera");

    hits_.push_back({Hit::Refresh, render_button(c, W - kMargin, 42, "Refresh", false)});

    float y = 150;
    for (size_t i = 0; i < choices.size(); i++)   // The camera settings, such as the mirror, come first.
        if (choices[i].page == ChoicePage::Cameras && choices[i].boolean) {
            render_switch_row(c, y, i, choices[i], 104);
            y += 104;
        }

    struct Row {
        std::string id, label, detail;
        bool options = false;   // True if the row has a button that opens the options of the camera.
    };
    std::vector<Row> rows{{"", "Automatic", "Use a connected Babble tracker, or the first available camera."}};
    for (const auto &cam : s.cameras)
        rows.push_back({cam.id, cam.label, cam.detail + (cam.iso ? " \u00b7 isochronous" : ""), true});
    const bool selected_missing =
        !s.camera.empty() &&
        std::none_of(s.cameras.begin(), s.cameras.end(), [&](const UsbCamera &cam) { return cam.id == s.camera; });
    if (selected_missing)
        rows.push_back({s.camera, "Selected camera", s.camera + " \u00b7 not connected"});

    const float row_h = 92;
    for (const Row &r : rows) {
        if (y + row_h > float(c.height()) - 150)
            break;   // Keep the corner of the cog clear.
        const Rect box{kMargin, y, W - 2 * kMargin, row_h - 12};
        const bool on = r.id == s.camera;
        c.fill_rect(box, on ? kCardHover : hovered(box) ? kCardHover : kCard, 6);
        if (on) {   // Draw a check mark.
            c.line(box.x + 26, box.y + 42, box.x + 38, box.y + 54, 5, kAccent);
            c.line(box.x + 38, box.y + 54, box.x + 60, box.y + 28, 5, kAccent);
        }
        const float tx = box.x + 84;
        float right = box.x + box.w - 24;
        if (r.options) {   // The options button comes first, so that its hit wins over the row.
            const Rect opt{box.x + box.w - 96, box.y + 8, 88, box.h - 16};
            if (hovered(opt))
                c.fill_rect(opt, kOnCardHover, 4);
            const float cx = opt.x + opt.w / 2 + 4, cy = opt.y + opt.h / 2;
            c.line(cx - 6, cy - 12, cx + 6, cy, 3, kMuted);
            c.line(cx + 6, cy, cx - 6, cy + 12, 3, kMuted);
            Hit h{Hit::OpenCameraOptions, opt};
            h.text = r.id;
            hits_.push_back(h);
            right = opt.x - 16;
        }
        const bool streaming = !r.id.empty() && r.id == s.active_camera;
        const bool auto_streaming = r.id.empty() && s.camera.empty() && !s.active_camera.empty();
        if (streaming || auto_streaming) {
            const std::string tag = "Active";
            const float tw = regular_.measure(tag, 24);
            c.fill_circle(right - tw - 22, box.y + 38, 8, kGreen);
            regular_.draw(c, right - tw, box.y + 46, tag, 24, kMuted);
            right -= tw + 48;
        }
        medium_.draw(c, tx, box.y + 36, fit(medium_, r.label, 30, right - tx), 30, kText);
        regular_.draw(c, tx, box.y + 68, fit(regular_, r.detail, 23, right - tx), 23, kMuted);
        Hit h{Hit::Camera, box};
        h.text = r.id;
        hits_.push_back(h);
        y += row_h;
    }
    if (s.cameras.empty())
        regular_.draw(c, kMargin, y + 40, "No USB cameras found. Connect a camera, then select Refresh.", 26, kMuted);
}

// Draws the options of one USB camera: a live preview at the left, and rotation, flips, format, resolution and frame
// rate at the right.
void DashboardView::render_camera_options(Canvas &c, const Status &s) {
    const float W = float(c.width());
    const auto it = std::find_if(s.cameras.begin(), s.cameras.end(),
                                 [&](const UsbCamera &cam) { return cam.id == options_camera_; });
    if (it == s.cameras.end()) {
        render_back(c, "Camera Options");
        regular_.draw(c, kMargin, 200, "This camera is not connected. Connect it, then select Refresh on the camera list.",
                      26, kMuted);
        return;
    }
    const UsbCamera &cam = *it;
    render_back(c, fit(medium_, cam.label, 32, W - 2 * kMargin - 120).c_str());
    const std::string id = camera_setting_id(cam.id);
    const UsbCamera::Settings &cur = cam.settings;

    // The preview box has the 4:3 shape of most cameras, and the image keeps its own shape inside it.
    const Rect box{kMargin, 124, 400, 300};
    c.fill_rect(box, kPanel, 6);
    const CameraPreview *pv = s.preview && s.preview->camera == cam.id && !s.preview->gray.empty() ? s.preview : nullptr;
    if (pv) {
        Image img;
        img.width = pv->width;
        img.height = pv->height;
        img.rgba.resize(size_t(pv->width) * pv->height * 4);
        for (size_t i = 0; i < pv->gray.size(); i++) {
            uint8_t *px = &img.rgba[i * 4];
            px[0] = px[1] = px[2] = pv->gray[i];
            px[3] = 255;
        }
        c.image(img, int(box.x + (box.w - float(pv->width)) / 2), int(box.y + (box.h - float(pv->height)) / 2), 4);
    } else {
        const std::string note = cam.id == s.active_camera ? "Waiting for the camera\u2026" : "No preview";
        medium_.draw(c, box.x + (box.w - medium_.measure(note, 24)) / 2, box.y + box.h / 2 + 8, note, 24, kMuted);
    }
    std::string info = cur.width ? size_label(cur.width, cur.height) : "";
    if (const auto *f = find_format(cam, cur.format))
        info += (info.empty() ? "" : "  \u00b7  ") + f->label;
    if (cur.fps)
        info += "  \u00b7  " + std::to_string(cur.fps) + " fps";
    regular_.draw(c, box.x, box.y + box.h + 34, fit(regular_, info, 22, box.w), 22, kText);
    const std::string hint = cam.id == s.active_camera
                                 ? "The image after rotation and flips, as the model sees it."
                                 : "The preview shows while this camera is in use. Select it on the camera list.";
    // Break the hint into lines that fit under the box.
    float hy = box.y + box.h + 66;
    std::string line;
    size_t pos = 0;
    while (pos <= hint.size()) {
        size_t next = hint.find(' ', pos);
        if (next == std::string::npos)
            next = hint.size();
        const std::string word = hint.substr(pos, next - pos);
        const std::string tryline = line.empty() ? word : line + " " + word;
        if (!line.empty() && regular_.measure(tryline, 20) > box.w) {
            regular_.draw(c, box.x, hy, line, 20, kMuted);
            hy += 28;
            line = word;
        } else {
            line = tryline;
        }
        pos = next + 1;
    }
    if (!line.empty())
        regular_.draw(c, box.x, hy, line, 20, kMuted);

    auto emit = [&](const Rect &r, UsbCamera::Settings next) {
        fit_mode(cam, next);
        Hit h{Hit::OutputSetting, r};
        h.text = id;
        h.json = settings_json(next);
        hits_.push_back(h);
    };
    const UsbCamera::Size *size = find_size(cam, cur);
    // The rows fill the space at the right of the preview.
    const float left = box.x + box.w + 20, right = W - kMargin, row_h = 82, seg_h = 54;
    float y = 124;
    auto card = [&] {
        c.fill_rect({left, y + 4, right - left, row_h - 12}, kCard, 6);
    };
    auto label = [&](const std::string &text, float max_right) {
        medium_.draw(c, left + 20, y + row_h / 2 + 8, fit(medium_, text, 26, max_right - left - 32), 26, kText);
    };
    auto segment_row = [&](const std::string &name, const std::vector<std::string> &labels, int selected, float seg_w,
                           const std::function<UsbCamera::Settings(size_t)> &pick) {
        card();
        const float l = render_segments(c, right - 12, y + (row_h - 8 - seg_h) / 2 - 2, labels, selected,
                                        [&](size_t k, const Rect &r) { emit(r, pick(k)); }, seg_h, seg_w);
        label(name, l);
        y += row_h;
    };
    auto switch_row = [&](const std::string &name, bool on, const std::function<UsbCamera::Settings(bool)> &pick) {
        card();
        const Rect sw{right - 12 - 92, y + (row_h - 8 - 52) / 2, 92, 52};
        const Rect hit{sw.x - 24, y, right - (sw.x - 24), row_h};
        render_switch(c, sw, on, hovered(hit));
        emit(hit, pick(!on));
        label(name, hit.x);
        y += row_h;
    };

    const int rotations[] = {0, 90, 180, 270};
    segment_row("Rotation", {"0\u00b0", "90\u00b0", "180\u00b0", "270\u00b0"},
                int(std::find(std::begin(rotations), std::end(rotations), cur.rotate) - std::begin(rotations)), 96,
                [&](size_t k) {
                    auto n = cur;
                    n.rotate = rotations[k];
                    return n;
                });
    switch_row("Horizontal Flip", cur.flip_h, [&](bool on) {
        auto n = cur;
        n.flip_h = on;
        return n;
    });
    switch_row("Vertical Flip", cur.flip_v, [&](bool on) {
        auto n = cur;
        n.flip_v = on;
        return n;
    });
    std::vector<std::string> formats;
    int selected = -1;
    for (size_t k = 0; k < cam.formats.size(); k++) {
        formats.push_back(cam.formats[k].label);
        if (cam.formats[k].key == cur.format)
            selected = int(k);
    }
    segment_row("Format", formats, selected, 96, [&](size_t k) {
        auto n = cur;
        n.format = cam.formats[k].key;
        return n;
    });
    {
        const Rect row{left, y + 4, right - left, row_h - 12};
        c.fill_rect(row, hovered(row) ? kCardHover : kCard, 6);
        const float chev_x = right - 24, mid = y + row_h / 2 - 2;
        const std::string value = cur.width ? size_label(cur.width, cur.height) : "Unknown";
        const float vw = medium_.measure(value, 24);
        medium_.draw(c, chev_x - 28 - vw, mid + medium_.cap_height(24) / 2, value, 24, kText);
        label("Resolution", chev_x - 28 - vw);
        c.line(chev_x - 10, mid - 10, chev_x, mid, 3, kMuted);
        c.line(chev_x, mid, chev_x - 10, mid + 10, 3, kMuted);
        hits_.push_back({Hit::OpenResolution, row});
        y += row_h;
    }
    std::vector<std::string> rates;
    std::vector<int> fps = size ? size->fps : std::vector<int>{};
    if (fps.size() > 5)
        fps.resize(5);
    selected = -1;
    for (size_t k = 0; k < fps.size(); k++) {
        rates.push_back(std::to_string(fps[k]));
        if (fps[k] == cur.fps)
            selected = int(k);
    }
    segment_row("Frame Rate", rates, selected, 80, [&](size_t k) {
        auto n = cur;
        n.fps = fps[k];
        return n;
    });
}

// Draws the frame sizes of the format that the camera uses, as a list with a check mark at the current size.
void DashboardView::render_camera_resolution(Canvas &c, const Status &s) {
    const float W = float(c.width());
    render_back(c, "Resolution");
    const auto it = std::find_if(s.cameras.begin(), s.cameras.end(),
                                 [&](const UsbCamera &cam) { return cam.id == options_camera_; });
    if (it == s.cameras.end())
        return;
    const UsbCamera &cam = *it;
    const UsbCamera::Format *f = find_format(cam, cam.settings.format);
    if (!f)
        return;
    const float row_h = 80;
    float y = 140;
    for (const auto &z : f->sizes) {
        if (y + row_h > float(c.height()) - 130)
            break;   // Keep the corner of the cog clear.
        const Rect box{kMargin, y, W - 2 * kMargin, row_h - 10};
        const bool on = z.width == cam.settings.width && z.height == cam.settings.height;
        c.fill_rect(box, on || hovered(box) ? kCardHover : kCard, 6);
        if (on) {
            c.line(box.x + 26, box.y + 36, box.x + 38, box.y + 48, 5, kAccent);
            c.line(box.x + 38, box.y + 48, box.x + 60, box.y + 22, 5, kAccent);
        }
        medium_.draw(c, box.x + 84, box.y + 46, size_label(z.width, z.height), 28, kText);
        std::string rates;
        for (int r : z.fps)
            rates += (rates.empty() ? "" : ", ") + std::to_string(r);
        const std::string detail = rates.empty() ? "" : rates + " fps";
        regular_.draw(c, box.x + box.w - 24 - regular_.measure(detail, 22), box.y + 44, detail, 22, kMuted);
        auto next = cam.settings;
        next.width = z.width;
        next.height = z.height;
        fit_mode(cam, next);
        Hit h{Hit::OutputSetting, box};
        h.text = camera_setting_id(cam.id);
        h.json = settings_json(next);
        hits_.push_back(h);
        y += row_h;
    }
}

void DashboardView::render_cog(Canvas &c) {
    const float cx = 84, cy = float(c.height()) - 84;
    const Rect hit{cx - 48, cy - 48, 96, 96};
    const bool active = page_ != Page::Home;
    if (hovered(hit) || active)
        c.fill_rect({cx - 32, cy - 32, 64, 64}, kCardHover, 4);
    const Icon icon = active ? Icon::Home : Icon::Settings;
    if (icons_[size_t(icon)])
        render_icon(c, icon, cx - 20, cy - 20, active ? kAccent : hovered(hit) ? kText : kMuted);
    else
        c.cog(cx, cy, 22, active ? kAccent : hovered(hit) ? kText : kMuted);
    const char *label = active ? "Dashboard" : "Settings";
    regular_.draw(c, cx - regular_.measure(label, 18) / 2, cy + 51, label, 18, kMuted);
    hits_.push_back({Hit::Cog, hit});
}

void DashboardView::render_thumbnail(Canvas &c) {
    const float sz = float(c.width());
    c.clear(kBg);
    if (logo_thumb_ && logo_thumb_->width == c.width() && logo_thumb_->height == c.height()) {
        c.image(*logo_thumb_, 0, 0, sz * 0.22f);
        return;
    }
    // Draw a "B" badge if the icon files are not available.
    c.fill_rect({sz * 0.08f, sz * 0.08f, sz * 0.84f, sz * 0.84f}, kAccent, sz * 0.22f);
    const float px = sz * 0.62f;
    medium_.draw(c, (sz - medium_.measure("B", px)) / 2, (sz + medium_.cap_height(px)) / 2 + sz * 0.02f, "B", px, kText);
}

int DashboardView::hit_at(std::optional<Point> p) const {
    if (!p)
        return -1;
    for (size_t i = 0; i < hits_.size(); i++)
        if (hits_[i].rect.contains(p->x, p->y))
            return int(i);
    return -1;
}

DashboardView::ClickResult DashboardView::click(Point p) {
    ClickResult out;
    for (const Hit &h : hits_) {
        if (!h.rect.contains(p.x, p.y))
            continue;
        switch (h.kind) {
        case Hit::Cog:
            restart_armed_until_ns_ = 0;
            page_ = page_ == Page::Home ? Page::Settings : Page::Home;
            out.redraw = true;
            break;
        case Hit::Back:
            restart_armed_until_ns_ = 0;
            page_ = page_ == Page::CameraResolution ? Page::CameraOptions
                    : page_ == Page::CameraOptions  ? Page::Cameras
                    : page_ == Page::Cameras        ? Page::Mouth
                    : page_ == Page::Settings       ? Page::Home
                                                    : Page::Settings;
            out.redraw = true;
            break;
        case Hit::OpenOutputs:
        case Hit::OpenEye:
        case Hit::OpenMouth:
            restart_armed_until_ns_ = 0;
            page_ = h.kind == Hit::OpenEye ? Page::Eye : h.kind == Hit::OpenMouth ? Page::Mouth : Page::Outputs;
            out.redraw = true;
            break;
        case Hit::OutputSetting:
            out.output_setting = std::make_pair(h.text, h.json);
            out.redraw = true;
            break;
        case Hit::EditText:
            out.edit_text = h.text;
            break;
        case Hit::OpenDiagnostics:
            restart_armed_until_ns_ = 0;
            page_ = Page::Diagnostics;
            out.opened_diagnostics = true;
            out.redraw = true;
            break;
        case Hit::CapturePrimary:
            out.capture = ClickResult::CaptureAction::Primary;
            out.redraw = true;
            break;
        case Hit::CaptureRemove:
            out.capture = ClickResult::CaptureAction::Remove;
            out.redraw = true;
            break;
        case Hit::Update: {
            // A restart of SteamVR needs two clicks.
            const uint64_t now = mono_ns();
            if (h.text != "restart" || update_armed(now)) {
                out.update = true;
                update_armed_until_ns_ = 0;
            } else {
                update_armed_until_ns_ = now + 3'000'000'000ull;
            }
            out.redraw = true;
            break;
        }
        case Hit::Verbose:
            out.verbose = h.value > 0.5;
            out.redraw = true;
            break;
        case Hit::WriteDiagnostics:
            out.write_diagnostics = true;
            out.redraw = true;
            break;
        case Hit::MouthSource:
            out.mouth_source = h.text;
            out.redraw = true;
            break;
        case Hit::OpenCameras:
            restart_armed_until_ns_ = 0;
            page_ = Page::Cameras;
            out.refresh_cameras = true;   // Show the cameras that are connected now.
            out.redraw = true;
            break;
        case Hit::Camera:
            out.camera = h.text;
            out.redraw = true;
            break;
        case Hit::OpenCameraOptions:
            options_camera_ = h.text;
            page_ = Page::CameraOptions;
            out.redraw = true;
            break;
        case Hit::OpenResolution:
            page_ = Page::CameraResolution;
            out.redraw = true;
            break;
        case Hit::Refresh:
            out.refresh_cameras = true;
            out.redraw = true;
            break;
        case Hit::Option:
            out.setting = std::make_pair(h.choice, h.value);
            out.redraw = true;
            break;
        case Hit::Toggle:
            out.toggle = std::make_pair(h.choice, h.value > 0.5);
            out.redraw = true;
            break;
        case Hit::Visualizer:
            out.visualizer = true;
            break;
        case Hit::Restart: {
            const uint64_t now = mono_ns();
            if (restart_armed(now)) {
                out.restart = true;
                restart_armed_until_ns_ = 0;
            } else {
                restart_armed_until_ns_ = now + 3'000'000'000ull;
            }
            out.redraw = true;
            break;
        }
        }
        break;
    }
    return out;
}

}
