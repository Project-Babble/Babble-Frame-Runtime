#include "ui/dashboard_view.h"

#include "babble_version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace babble::ui {

namespace {

// Palette (dark, like the SteamVR dashboard around it).
constexpr Color kBg = Color::hex(0x14161b);
constexpr Color kCard = Color::hex(0x1e2129);
constexpr Color kCardHover = Color::hex(0x282c36);
constexpr Color kLine = Color::hex(0x2c3039);
constexpr Color kText = Color::hex(0xeceef2);
constexpr Color kMuted = Color::hex(0x9aa1ad);
constexpr Color kAccent = Color::hex(0x4d8dff);
constexpr Color kGreen = Color::hex(0x3fcf73);
constexpr Color kAmber = Color::hex(0xf2b33d);
constexpr Color kRed = Color::hex(0xf0544f);
constexpr Color kGrey = Color::hex(0x6b7280);

constexpr float kMargin = 56;

// Eye panel geometry: gaze plot, openness bar, widen bar.
constexpr float kPlot = 210, kBarGap = 24, kBarW = 36, kWidenGap = 10, kWidenW = 18;
constexpr float kEyeGroupW = kPlot + kBarGap + kBarW + 2 * (kWidenGap + kWidenW);   // openness, W, D
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
    case Health::Working: return "Working";
    case Health::Starting: return "Starting";
    case Health::Paused: return "Paused";
    case Health::Off: return "Off";   // FeedStatus::tracking false, or not configured
    case Health::Fault: return "Not working";
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

} // namespace

bool DashboardView::load_fonts(const std::filesystem::path &dir) {
    return regular_.load(dir / "NotoSans-Regular.ttf") && medium_.load(dir / "NotoSans-Medium.ttf");
}

bool DashboardView::load_icons(const std::filesystem::path &dir) {
    logo_small_ = load_png(dir / "bfr_logo_48.png");
    logo_thumb_ = load_png(dir / "bfr_logo_128.png");
    return logo_small_ && logo_thumb_;
}

bool DashboardView::hovered(const Rect &r) const { return pointer_ && r.contains(pointer_->x, pointer_->y); }

std::string DashboardView::fit(const Font &f, const std::string &text, float px, float max_w) const {
    if (f.measure(text, px) <= max_w)
        return text;
    std::string s = text;
    while (!s.empty()) {
        // Drop whole UTF-8 sequences.
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
        render_visualizer_button(c, status.visualizer);
    } else
        render_settings(c, choices);
    render_cog(c);
}

Headline headline(const Status &s) {
    const FeedStatus *feeds[2] = {&s.mouth, &s.eye};
    const char *names[2] = {"Mouth", "Eyes"};
    Headline h;
    bool any_working = false, any_starting = false, all_off = true;   // Off = turned off or not configured
    for (const FeedStatus *f : feeds) {
        any_working |= f->health == Health::Working;
        any_starting |= f->health == Health::Starting;
        all_off &= f->health == Health::Off;
    }
    if (!s.steamvr) {
        h = {"Waiting for SteamVR", "", Health::Starting};
    } else if (all_off) {
        h = {"Tracking off", s.mouth.available || s.eye.available ? "Turn a feed on below" : "No camera feeds are configured",
             Health::Off};
    } else if (s.worn == Worn::No) {
        h = {"Paused", "The headset isn't being worn", Health::Paused};
    } else if (any_working) {
        h = {"Tracking", "", Health::Working};
        for (int i = 0; i < 2; i++)
            if (feeds[i]->health == Health::Working && feeds[i]->hz > 0)
                h.sub += (h.sub.empty() ? "" : "   ·   ") + std::string(names[i]) + "  " + format_hz(feeds[i]->hz);
    } else if (any_starting) {
        h = {"Starting", "Waiting for the cameras", Health::Starting};
    } else {
        h = {"Not tracking", "A camera feed has a problem", Health::Fault};
    }
    return h;
}

void DashboardView::render_home(Canvas &c, const Status &s) {
    const float W = float(c.width());
    float title_x = kMargin;
    if (logo_small_) {
        c.image(*logo_small_, int(kMargin), 52, 12);
        title_x += logo_small_->width + 18;
    }
    medium_.draw(c, title_x, 92, "Babble", 40, kText);
    regular_.draw(c, title_x, 124, BABBLE_VERSION_DISPLAY, 22, kMuted);

    // Headline, compact at the top right: the overall state and the live rates.
    const FeedStatus *feeds[2] = {&s.mouth, &s.eye};
    const char *names[2] = {"Mouth", "Eyes"};
    const Headline h = headline(s);
    {
        const float px = 34, dot_r = 10, gap = 16;
        const float tw = medium_.measure(h.word, px), right = W - kMargin;
        c.fill_circle(right - tw - gap - dot_r, 80 - medium_.cap_height(px) / 2, dot_r, health_color(h.tone));
        medium_.draw(c, right - tw, 80, h.word, px, kText);
        if (!h.sub.empty())
            regular_.draw(c, right - regular_.measure(h.sub, 24), 116, h.sub, 24, kMuted);
    }

    // Eye panel: gaze and openness per eye, as sent.
    const float groups_gap = 110, gx = (W - 2 * kEyeGroupW - groups_gap) / 2;
    render_eye(c, gx, 150, "Left eye", s.eyes[0]);
    render_eye(c, gx + kEyeGroupW + groups_gap, 150, "Right eye", s.eyes[1]);

    // Feed indicators, each with its on/off switch.
    const float cw = (W - 2 * kMargin - 24) / 2, ch = 132, cy = 470;
    for (int i = 0; i < 2; i++) {
        const FeedStatus &f = *feeds[i];
        const Rect r{kMargin + i * (cw + 24), cy, cw, ch};
        c.fill_rect(r, kCard, 22);
        c.fill_circle(r.x + 44, r.y + 50, 11, health_color(f.health));
        medium_.draw(c, r.x + 72, r.y + 62, names[i], 36, kText);
        std::string detail = health_word(f.health);
        if (!f.available)
            detail = "Not available";
        else if (!f.tracking)
            detail = "Turned off";
        else if (f.health == Health::Paused)
            detail = "Paused while the headset is off";
        else if (f.health == Health::Fault && !f.detail.empty())
            detail = f.detail;
        float text_w = r.w - 66;
        if (f.available) {
            const Rect sw{r.x + r.w - 32 - 92, r.y + (ch - 52) / 2, 92, 52};
            // Generous hit area: the right part of the card, full height.
            const Rect hit{sw.x - 24, r.y, r.x + r.w - (sw.x - 24), r.h};
            render_switch(c, sw, f.tracking, hovered(hit));
            hits_.push_back({Hit::Toggle, hit, size_t(i), f.tracking ? 0.0 : 1.0});
            text_w = hit.x - r.x - 33 - 8;
        }
        regular_.draw(c, r.x + 33, r.y + 104, fit(regular_, detail, 28, text_w), 28, kMuted);
    }
}

// A gaze plot (+-45 deg square, crosshair, dot), an openness bar and a slim widen bar.
void DashboardView::render_eye(Canvas &c, float x, float y, const char *name, const EyeReading &e) {
    constexpr float kRange = 45;
    medium_.draw(c, x, y + 22, name, 28, kText);
    // The value going into calibration, and the calibration's neutral (n) and
    // wide ceiling (w), all raw units.
    if (e.raw) {
        char buf[64];
        if (e.cal)
            snprintf(buf, sizeof(buf), "raw %.3f   n %.3f   w %.3f", e.raw_lid, e.neutral, e.wide_raw);
        else
            snprintf(buf, sizeof(buf), "raw %.3f   calibrating", e.raw_lid);
        regular_.draw(c, x, y + 50, buf, 20, kMuted);
    }
    const Rect plot{x, y + 62, kPlot, kPlot};
    c.fill_rect(plot, kCard, 18);
    const float cx = plot.x + kPlot / 2, cy = plot.y + kPlot / 2, half = kPlot / 2 - 14;
    c.line(plot.x + 14, cy, plot.x + kPlot - 14, cy, 2, kLine);
    c.line(cx, plot.y + 14, cx, plot.y + kPlot - 14, 2, kLine);
    std::string caption = "no gaze";
    if (e.gaze) {
        auto clampd = [](double v) { return std::max(-1.0, std::min(1.0, v)); };
        const float dx = float(clampd(e.yaw_deg / kRange)) * half, dy = float(clampd(e.pitch_deg / kRange)) * half;
        c.fill_circle(cx + dx, cy - dy, 13, kAccent);
        char buf[64];
        snprintf(buf, sizeof(buf), "%+.0f°  %+.0f°", e.yaw_deg, e.pitch_deg);
        caption = buf;
    }
    regular_.draw(c, plot.x + (kPlot - regular_.measure(caption, 22)) / 2, plot.y + kPlot + 30, caption, 22, kMuted);

    const Rect bar{x + kPlot + kBarGap, plot.y, kBarW, kPlot};
    c.fill_rect(bar, kCard, kBarW / 2);
    std::string value = "--";
    if (e.lid) {
        const float frac = float(std::max(0.0, std::min(1.0, e.openness)));
        if (frac > 0.01f)
            c.fill_rect({bar.x, bar.y + bar.h * (1 - frac), bar.w, bar.h * frac}, kGreen, kBarW / 2);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", e.openness);
        value = buf;
    }
    regular_.draw(c, bar.x + (kBarW - regular_.measure(value, 22)) / 2, plot.y + kPlot + 30, value, 22, kMuted);

    const Rect wbar{bar.x + kBarW + kWidenGap, plot.y, kWidenW, kPlot};
    c.fill_rect(wbar, kCard, kWidenW / 2);
    if (e.lid && e.widen > 0.01) {
        const float frac = float(std::min(1.0, e.widen));
        c.fill_rect({wbar.x, wbar.y + wbar.h * (1 - frac), wbar.w, wbar.h * frac}, kAmber, kWidenW / 2);
    }
    regular_.draw(c, wbar.x + (kWidenW - regular_.measure("W", 20)) / 2, plot.y + kPlot + 30, "W", 20, kMuted);

    // Dilation: 0.5 is the person's baseline, marked with a tick.
    const Rect dbar{wbar.x + kWidenW + kWidenGap, plot.y, kWidenW, kPlot};
    c.fill_rect(dbar, kCard, kWidenW / 2);
    if (e.dilation_ok && e.dilation > 0.01) {
        const float frac = float(std::min(1.0, e.dilation));
        c.fill_rect({dbar.x, dbar.y + dbar.h * (1 - frac), dbar.w, dbar.h * frac}, kViolet, kWidenW / 2);
    }
    c.fill_rect({dbar.x - 3, dbar.y + dbar.h / 2 - 1, dbar.w + 6, 2}, kText);
    regular_.draw(c, dbar.x + (kWidenW - regular_.measure("D", 20)) / 2, plot.y + kPlot + 30, "D", 20, kMuted);
}

void DashboardView::render_visualizer_button(Canvas &c, AppState state) {
    if (state == AppState::Unavailable)
        return;
    const std::string text = state == AppState::Running ? "Close Visualizer" : "Open Visualizer";
    const float px = 28, pad = 30, h = 64;
    const float w = medium_.measure(text, px) + 2 * pad;
    const Rect r{float(c.width()) - kMargin - w, float(c.height()) - 84 - h / 2, w, h};
    const bool hover = hovered(r);
    const Color fill = state == AppState::Running ? (hover ? kCardHover : kCard)
                                                  : (hover ? Color::hex(0x6aa0ff) : kAccent);
    c.fill_rect(r, fill, h / 2);
    medium_.draw(c, r.x + pad, r.y + (h + medium_.cap_height(px)) / 2, text, px, kText);
    hits_.push_back({Hit::Visualizer, r});
}

void DashboardView::render_switch(Canvas &c, Rect r, bool on, bool hover) {
    constexpr Color kTrackOff = Color::hex(0x3a3f4a), kTrackOffHover = Color::hex(0x4a505c);
    constexpr Color kAccentHover = Color::hex(0x6aa0ff);
    c.fill_rect(r, on ? (hover ? kAccentHover : kAccent) : (hover ? kTrackOffHover : kTrackOff), r.h / 2);
    const float knob = r.h / 2 - 5;
    c.fill_circle(on ? r.x + r.w - r.h / 2 : r.x + r.h / 2, r.y + r.h / 2, knob, on ? kText : Color::hex(0xc5cad3));
}

void DashboardView::render_settings(Canvas &c, const std::vector<Choice> &choices) {
    const float W = float(c.width());
    // Back: chevron + title.
    const Rect back{kMargin - 20, 36, 300, 76};
    if (hovered(back))
        c.fill_rect(back, kCardHover, 20);
    c.line(kMargin + 14, 58, kMargin, 74, 5, kText);
    c.line(kMargin, 74, kMargin + 14, 90, 5, kText);
    medium_.draw(c, kMargin + 36, 90, "Settings", 40, kText);
    hits_.push_back({Hit::Back, back});

    float y = 160;
    for (size_t i = 0; i < choices.size(); i++) {
        const Choice &ch = choices[i];
        const float row_h = 124;
        if (i > 0)
            c.fill_rect({kMargin, y, W - 2 * kMargin, 2}, kLine);
        // Segmented control, right-aligned.
        const float seg_w = 128, seg_h = 64, pad = 4;
        const float ctl_w = seg_w * float(ch.options.size()) + 2 * pad;
        const Rect ctl{W - kMargin - ctl_w, y + (row_h - seg_h - 2 * pad) / 2, ctl_w, seg_h + 2 * pad};
        medium_.draw(c, kMargin, y + 56, fit(medium_, ch.label, 32, ctl.x - kMargin - 24), 32, kText);
        regular_.draw(c, kMargin, y + 94, fit(regular_, ch.hint, 25, ctl.x - kMargin - 24), 25, kMuted);
        c.fill_rect(ctl, kCard, ctl.h / 2);
        for (size_t k = 0; k < ch.options.size(); k++) {
            const double v = ch.options[k];
            const Rect seg{ctl.x + pad + float(k) * seg_w, ctl.y + pad, seg_w, seg_h};
            const bool selected = std::fabs(v - ch.value) < 1e-6;
            if (selected)
                c.fill_rect(seg, kAccent, seg_h / 2);
            else if (hovered(seg))
                c.fill_rect(seg, kCardHover, seg_h / 2);
            const std::string label = format_value(v, ch.unit);
            const float px = 27;
            medium_.draw(c, seg.x + (seg.w - medium_.measure(label, px)) / 2,
                         seg.y + (seg.h + medium_.cap_height(px)) / 2, label, px, selected ? kText : kMuted);
            hits_.push_back({Hit::Option, seg, i, v});
        }
        y += row_h;
    }
}

void DashboardView::render_cog(Canvas &c) {
    const float cx = 84, cy = float(c.height()) - 84, r = 30;
    const Rect hit{cx - 48, cy - 48, 96, 96};
    const bool active = page_ == Page::Settings;
    if (hovered(hit) || active)
        c.fill_circle(cx, cy, 46, active ? kCardHover : kCard);
    c.cog(cx, cy, r, active ? kAccent : hovered(hit) ? kText : kMuted);
    hits_.push_back({Hit::Cog, hit});
}

void DashboardView::render_thumbnail(Canvas &c) {
    const float sz = float(c.width());
    c.clear(kBg);
    if (logo_thumb_ && logo_thumb_->width == c.width() && logo_thumb_->height == c.height()) {
        c.image(*logo_thumb_, 0, 0, sz * 0.22f);
        return;
    }
    // Fallback without the icon files: a "B" badge.
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
            page_ = page_ == Page::Home ? Page::Settings : Page::Home;
            out.redraw = true;
            break;
        case Hit::Back:
            page_ = Page::Home;
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
        }
        break;
    }
    return out;
}

} // namespace babble::ui
