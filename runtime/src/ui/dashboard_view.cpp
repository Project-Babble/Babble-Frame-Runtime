#include "ui/dashboard_view.h"

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
    if (page_ == Page::Home)
        render_home(c, status);
    else
        render_settings(c, choices);
    render_cog(c);
}

void DashboardView::render_home(Canvas &c, const Status &s) {
    const float W = float(c.width());
    medium_.draw(c, kMargin, 92, "Babble", 40, kText);

    // Headline: the overall tracking state and the live rates.
    const FeedStatus *feeds[2] = {&s.mouth, &s.eye};
    const char *names[2] = {"Mouth", "Eyes"};
    std::string word, sub;
    Color dot;
    bool any_working = false, any_starting = false, all_off = true;   // Off = turned off or not configured
    for (const FeedStatus *f : feeds) {
        any_working |= f->health == Health::Working;
        any_starting |= f->health == Health::Starting;
        all_off &= f->health == Health::Off;
    }
    if (!s.steamvr) {
        word = "Waiting for SteamVR";
        dot = kAmber;
    } else if (all_off) {
        word = "Tracking off";
        sub = s.mouth.available || s.eye.available ? "Turn a feed on below" : "No camera feeds are configured";
        dot = kGrey;
    } else if (s.worn == Worn::No) {
        word = "Paused";
        sub = "The headset isn't being worn";
        dot = kGrey;
    } else if (any_working) {
        word = "Tracking";
        dot = kGreen;
        for (int i = 0; i < 2; i++)
            if (feeds[i]->health == Health::Working && feeds[i]->hz > 0)
                sub += (sub.empty() ? "" : "   ·   ") + std::string(names[i]) + "  " + format_hz(feeds[i]->hz);
    } else if (any_starting) {
        word = "Starting";
        sub = "Waiting for the cameras";
        dot = kAmber;
    } else {
        word = "Not tracking";
        sub = "A camera feed has a problem";
        dot = kRed;
    }
    const float head_px = 76, dot_r = 15, gap = 28;
    const float tw = medium_.measure(word, head_px);
    const float x0 = (W - (2 * dot_r + gap + tw)) / 2, base = 318;
    c.fill_circle(x0 + dot_r, base - medium_.cap_height(head_px) / 2 - 4, dot_r, dot);
    medium_.draw(c, x0 + 2 * dot_r + gap, base, word, head_px, kText);
    if (!sub.empty())
        regular_.draw(c, (W - regular_.measure(sub, 34)) / 2, base + 70, sub, 34, kMuted);

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
    const float s = float(c.width());
    c.clear(kBg);
    c.fill_rect({s * 0.08f, s * 0.08f, s * 0.84f, s * 0.84f}, kAccent, s * 0.22f);
    const float px = s * 0.62f;
    medium_.draw(c, (s - medium_.measure("B", px)) / 2, (s + medium_.cap_height(px)) / 2 + s * 0.02f, "B", px, kText);
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
        }
        break;
    }
    return out;
}

} // namespace babble::ui
