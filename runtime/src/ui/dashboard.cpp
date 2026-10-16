#include "ui/dashboard.h"

#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <algorithm>
#include <cmath>

namespace {
const babble::log::Logger LOG("ui");
}

namespace babble::ui {

Dashboard::~Dashboard() {
    stop();
    release_gpu();
}

void Dashboard::start(SteamVrClient &vr, ThreadSpec thread, Options options, std::vector<Choice> choices) {
    vr_ = &vr;
    thread_spec_ = std::move(thread);
    options_ = std::move(options);
    choices_ = std::move(choices);
    if (options_.state) {
        const char *names[2 * kPerEye] = {
            "LeftEyeX",  "LeftEyeY",  "LeftEyeLid",  "LeftEyeWiden",  "_LeftEyeLidRaw",
            "_LeftEyeNeutral",  "_LeftEyeWideRaw",  "LeftEyeDilation",
            "RightEyeX", "RightEyeY", "RightEyeLid", "RightEyeWiden", "_RightEyeLidRaw",
            "_RightEyeNeutral", "_RightEyeWideRaw", "RightEyeDilation"};
        for (int i = 0; i < 2 * kPerEye; i++)   // add_signal is idempotent, so this gets the pipeline signals.
            eye_ids_[i] = options_.state->add_signal(names[i]);
    }
    if (!view_.load_fonts(options_.fonts_dir))
        LOG.warn("dashboard: fonts not found in %s; text won't be drawn", options_.fonts_dir.c_str());
    if (!options_.icons_dir.empty() && !view_.load_icons(options_.icons_dir))
        LOG.warn("dashboard: logo not found in %s; using the plain badge", options_.icons_dir.c_str());
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void Dashboard::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    destroy_overlays();
    // Keep the GPU objects because a later start() uses them again.
    connection_ = 0;
    pointer_.reset();
    hover_ = -1;
    view_.set_page(DashboardView::Page::Home);
}

void Dashboard::release_gpu() { gpu_.release(); }

void Dashboard::set_status(const Status &s) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto same_feed = [](const FeedStatus &a, const FeedStatus &b) {
        return a.health == b.health && std::lround(a.hz) == std::lround(b.hz) && a.detail == b.detail &&
               a.available == b.available && a.tracking == b.tracking;
    };
    if (s.steamvr != status_.steamvr || s.worn != status_.worn || !same_feed(s.mouth, status_.mouth) ||
        !same_feed(s.eye, status_.eye) || s.visualizer != status_.visualizer ||
        s.link.available != status_.link.available || s.link.linked != status_.link.linked ||
        s.link.path != status_.link.path || s.link.pc != status_.link.pc || s.link.address != status_.link.address ||
        std::lround(s.link.rtt_ms) != std::lround(status_.link.rtt_ms) || s.mouth_source != status_.mouth_source ||
        s.mouth_source_selectable != status_.mouth_source_selectable ||
        s.camera != status_.camera || s.active_camera != status_.active_camera ||
        s.verbose_minutes_left != status_.verbose_minutes_left || s.last_diagnostics != status_.last_diagnostics ||
        s.log_file != status_.log_file || s.choice_hints != status_.choice_hints || s.capture != status_.capture ||
        s.update != status_.update ||
        s.cameras.size() != status_.cameras.size() ||
        !std::equal(s.cameras.begin(), s.cameras.end(), status_.cameras.begin(),
                    [](const UsbCamera &a, const UsbCamera &b) { return a.id == b.id && a.label == b.label; }))
        dirty_ = true;
    status_ = s;
}

void Dashboard::set_choice(const std::string &id, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &c : choices_)
        if (c.id == id && c.value != value) {
            c.value = value;
            dirty_ = true;
        }
}

std::vector<std::pair<std::string, nlohmann::json>> Dashboard::take_changes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(changes_, {});
}

bool Dashboard::create_overlays() {
    bool ok = false;
    vr::EVROverlayError err = vr::VROverlayError_None;
    vr_->with_vr([&] {
        auto *ov = vr::VROverlay();
        vr::VROverlayHandle_t main = 0, thumb = 0;
        err = ov->CreateDashboardOverlay(options_.key.c_str(), options_.name.c_str(), &main, &thumb);
        if (err == vr::VROverlayError_KeyInUse) {
            // The overlay can remain from the previous connection because SteamVR can keep the key.
            ov->FindOverlay(options_.key.c_str(), &main);
            ov->DestroyOverlay(main);
            err = ov->CreateDashboardOverlay(options_.key.c_str(), options_.name.c_str(), &main, &thumb);
        }
        if (err != vr::VROverlayError_None)
            return;
        ov->SetOverlayWidthInMeters(main, options_.width_m);
        ov->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
        vr::HmdVector2_t scale{float(DashboardView::kWidth), float(DashboardView::kHeight)};
        ov->SetOverlayMouseScale(main, &scale);
        main_ = main;
        thumb_ = thumb;
        visible_ = ov->IsOverlayVisible(main);
        ok = true;
    });
    if (!ok) {
        LOG.error("dashboard: creating the overlay failed (%d)", int(err));
        return false;
    }

    // A single raw upload is sufficient because the thumbnail does not change.
    Canvas thumb(DashboardView::kThumb, DashboardView::kThumb);
    view_.render_thumbnail(thumb);
    vr_->with_vr([&] {
        vr::VROverlay()->SetOverlayRaw(thumb_, const_cast<uint8_t *>(thumb.data()), DashboardView::kThumb,
                                       DashboardView::kThumb, 4);
    });

    if (!gpu_.ready() && !gpu_failed_) {
        std::string error;
        bool gpu_ok = false;
        vr_->with_vr([&] { gpu_ok = gpu_.init(DashboardView::kWidth, DashboardView::kHeight, &error); });
        if (!gpu_ok) {
            gpu_failed_ = true;
            LOG.warn("dashboard: Vulkan upload unavailable (%s); falling back to SetOverlayRaw", error.c_str());
        }
    }
    LOG.info("dashboard: \"%s\" tab ready", options_.name.c_str());
    present();   // Draw now so that the tab does not open blank.
    return true;
}

void Dashboard::destroy_overlays() {
    if (!main_)
        return;
    vr_->with_vr([&] { vr::VROverlay()->DestroyOverlay(main_); });   // This also removes the thumbnail.
    main_ = thumb_ = 0;
    visible_ = false;
}

std::array<EyeReading, 2> Dashboard::read_eyes() const {
    std::array<EyeReading, 2> out{};
    if (!options_.state)
        return out;
    Signal v[2 * kPerEye];
    options_.state->read(eye_ids_, v);
    const uint64_t now = mono_ns(), fresh = 1000000000ull;   // A value that is older than 1 s counts as no data.
    auto recent = [&](const Signal &s) { return s.t_ns != 0 && now - s.t_ns < fresh; };
    for (int e = 0; e < 2; e++) {
        const Signal *s = v + kPerEye * e;
        const Signal &x = s[0], &y = s[1], &lid = s[2], &widen = s[3], &raw = s[4], &neutral = s[5], &wide = s[6],
                     &dil = s[7];
        EyeReading &r = out[e];
        r.gaze = recent(x) || recent(y);
        // The gaze outputs are angles with a scale where +-1 is +-45 deg.
        r.yaw_deg = double(x.value) * 45;
        r.pitch_deg = double(y.value) * 45;
        r.lid = recent(lid);
        r.openness = lid.value;
        r.widen = widen.value;
        r.raw = recent(raw);
        r.raw_lid = raw.value;
        r.cal = recent(neutral) && recent(wide);
        r.neutral = neutral.value;
        r.wide_raw = wide.value;
        r.dilation_ok = recent(dil);
        r.dilation = dil.value;
    }
    return out;
}

void Dashboard::present() {
    Status status;
    std::vector<Choice> choices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = status_;
        choices = choices_;
        dirty_ = false;
    }
    status.eyes = drawn_eyes_ = read_eyes();
    last_present_ns_ = mono_ns();
    view_.render(canvas_, status, choices, pointer_);

    std::string error;
    vr::VRVulkanTextureData_t data{};
    if (gpu_.ready() && gpu_.upload(canvas_.data(), &data, &error)) {
        vr_->with_vr([&] {
            vr::Texture_t tex{&data, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
            vr::VROverlay()->SetOverlayTexture(main_, &tex);
        });
        return;
    }
    if (gpu_.ready()) {
        LOG.warn("dashboard: Vulkan upload failed (%s); falling back to SetOverlayRaw", error.c_str());
        gpu_failed_ = true;
        gpu_.release();
    }
    vr_->with_vr([&] {
        vr::VROverlay()->SetOverlayRaw(main_, const_cast<uint8_t *>(canvas_.data()), DashboardView::kWidth,
                                       DashboardView::kHeight, 4);
    });
}

void Dashboard::run() {
    while (!stop_) {
        if (!vr_->connected()) {
            main_ = thumb_ = 0;   // The handles are not valid after the connection stops.
            visible_ = false;
            connection_ = 0;
            sleep_ns(200000000);
            continue;
        }
        if (connection_ != vr_->connection()) {
            main_ = thumb_ = 0;
            if (!create_overlays()) {
                sleep_ns(2000000000);
                continue;
            }
            connection_ = vr_->connection();
        }

        bool redraw = false;
        std::vector<Point> clicks;
        vr_->with_vr([&] {
            vr::VREvent_t ev;
            while (vr::VROverlay()->PollNextOverlayEvent(main_, &ev, sizeof(ev))) {
                switch (ev.eventType) {
                case vr::VREvent_MouseMove:
                    // Mouse events are in texture pixels with the origin at the bottom left.
                    pointer_ = Point{ev.data.mouse.x, DashboardView::kHeight - ev.data.mouse.y};
                    break;
                case vr::VREvent_MouseButtonDown:
                    if (ev.data.mouse.button == vr::VRMouseButton_Left)
                        clicks.push_back({ev.data.mouse.x, DashboardView::kHeight - ev.data.mouse.y});
                    break;
                case vr::VREvent_FocusLeave:
                    pointer_.reset();
                    break;
                case vr::VREvent_OverlayShown:
                    visible_ = true;
                    redraw = true;
                    break;
                case vr::VREvent_OverlayHidden:
                    visible_ = false;
                    pointer_.reset();
                    view_.set_page(DashboardView::Page::Home);   // The tab opens again on the front page.
                    break;
                default: break;
                }
            }
        });

        for (const Point &p : clicks) {
            auto r = view_.click(p);
            redraw |= r.redraw;
            if (r.visualizer)
                visualizer_request_ = true;
            if (r.restart)
                restart_request_ = true;
            if (r.refresh_cameras)
                camera_refresh_request_ = true;
            if (r.verbose)
                verbose_request_ = *r.verbose ? 1 : 0;
            if (r.write_diagnostics)
                diagnostics_request_ = true;
            if (r.opened_diagnostics)
                capture_check_request_ = true;
            if (r.capture != DashboardView::ClickResult::CaptureAction::None)
                capture_request_ = int(r.capture);
            if (r.update)
                update_request_ = true;
            std::lock_guard<std::mutex> lock(mutex_);
            // Show string settings immediately, and let the main thread apply them.
            if (r.mouth_source && *r.mouth_source != status_.mouth_source) {
                status_.mouth_source = *r.mouth_source;
                changes_.emplace_back("/mouth_source", *r.mouth_source);
            }
            if (r.camera && *r.camera != status_.camera) {
                status_.camera = *r.camera;
                changes_.emplace_back("/sources/face/camera", *r.camera);
            }
            if (r.setting) {
                Choice &c = choices_[r.setting->first];
                if (c.value != r.setting->second) {
                    c.value = r.setting->second;
                    changes_.emplace_back(c.id, c.boolean ? nlohmann::json(c.value != 0) : nlohmann::json(c.value));
                }
            }
            if (r.toggle) {
                // Show the new position immediately because the next status from the main thread confirms it.
                const auto [feed, on] = *r.toggle;
                FeedStatus &f = feed == 0 ? status_.mouth : status_.eye;
                f.tracking = on;
                changes_.emplace_back(feed == 0 ? options_.mouth_tracking_id
                                                : std::string("/sources/") + kFeedSources[1] + "/tracking",
                                      on);
            }
        }

        // Draw again only when something visible changed.
        if (visible_) {
            const int hover = view_.hit_at(pointer_);
            redraw |= hover != hover_;
            // The armed state of the restart button stops after a time without a click.
            const bool armed = view_.restart_armed(mono_ns()) || view_.update_armed(mono_ns());
            redraw |= armed != drawn_restart_armed_;
            drawn_restart_armed_ = armed;
            // Draw the eye panel again when a value visibly changed, at a maximum of about 30 times a second.
            if (!redraw && mono_ns() - last_present_ns_ >= 33000000ull) {
                const auto eyes = read_eyes();
                for (int e = 0; e < 2; e++) {
                    const EyeReading &a = eyes[e], &b = drawn_eyes_[e];
                    redraw |= a.gaze != b.gaze || a.lid != b.lid || std::fabs(a.yaw_deg - b.yaw_deg) > 0.3 ||
                              std::fabs(a.pitch_deg - b.pitch_deg) > 0.3 || std::fabs(a.openness - b.openness) > 0.005 ||
                              std::fabs(a.widen - b.widen) > 0.005 || a.raw != b.raw ||
                              std::fabs(a.raw_lid - b.raw_lid) > 0.002 || a.cal != b.cal ||
                              std::fabs(a.neutral - b.neutral) > 0.002 || std::fabs(a.wide_raw - b.wide_raw) > 0.002 ||
                              a.dilation_ok != b.dilation_ok || std::fabs(a.dilation - b.dilation) > 0.005;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                redraw |= dirty_;
            }
            if (redraw) {
                present();
                hover_ = view_.hit_at(pointer_);   // The hit areas move if the page changed.
            }
        }
        // Run at about 60 Hz when the tab is open and at 5 Hz when it is closed.
        sleep_ns(visible_ ? 16000000 : 200000000);
    }
}

}
