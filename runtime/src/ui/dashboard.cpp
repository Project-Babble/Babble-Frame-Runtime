#include "ui/dashboard.h"

#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <cmath>

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
    if (!view_.load_fonts(options_.fonts_dir))
        LOG_WARN("dashboard: fonts not found in %s; text won't be drawn", options_.fonts_dir.c_str());
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void Dashboard::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    destroy_overlays();
}

void Dashboard::release_gpu() { gpu_.release(); }

void Dashboard::set_status(const Status &s) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto same_feed = [](const FeedStatus &a, const FeedStatus &b) {
        return a.health == b.health && std::lround(a.hz) == std::lround(b.hz) && a.detail == b.detail &&
               a.available == b.available && a.tracking == b.tracking;
    };
    if (s.steamvr != status_.steamvr || s.worn != status_.worn || !same_feed(s.mouth, status_.mouth) ||
        !same_feed(s.eye, status_.eye))
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
            // Left over from our previous connection (e.g. SteamVR kept the key).
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
        LOG_ERROR("dashboard: creating the overlay failed (%d)", int(err));
        return false;
    }

    // The thumbnail never changes: one raw upload is fine.
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
            LOG_WARN("dashboard: Vulkan upload unavailable (%s); falling back to SetOverlayRaw", error.c_str());
        }
    }
    LOG_INFO("dashboard: \"%s\" tab ready", options_.name.c_str());
    present();   // so the tab never opens blank
    return true;
}

void Dashboard::destroy_overlays() {
    if (!main_)
        return;
    vr_->with_vr([&] { vr::VROverlay()->DestroyOverlay(main_); });   // also removes the thumbnail
    main_ = thumb_ = 0;
    visible_ = false;
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
        LOG_WARN("dashboard: Vulkan upload failed (%s); falling back to SetOverlayRaw", error.c_str());
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
            main_ = thumb_ = 0;   // handles die with the connection
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
                    // Mouse events are in texture pixels with a bottom-left origin.
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
                    view_.set_page(DashboardView::Page::Home);   // reopen on the front page
                    break;
                default: break;
                }
            }
        });

        for (const Point &p : clicks) {
            auto r = view_.click(p);
            redraw |= r.redraw;
            std::lock_guard<std::mutex> lock(mutex_);
            if (r.setting) {
                Choice &c = choices_[r.setting->first];
                if (c.value != r.setting->second) {
                    c.value = r.setting->second;
                    changes_.emplace_back(c.id, c.value);
                }
            }
            if (r.toggle) {
                // Show the new position right away; the main thread's next status confirms it.
                const auto [feed, on] = *r.toggle;
                FeedStatus &f = feed == 0 ? status_.mouth : status_.eye;
                f.tracking = on;
                changes_.emplace_back(std::string("/sources/") + kFeedSources[feed] + "/tracking", on);
            }
        }

        // Redraw only when something visible changed: the hovered element,
        // a click, or the status/settings (the rates change about once a second).
        if (visible_) {
            const int hover = view_.hit_at(pointer_);
            redraw |= hover != hover_;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                redraw |= dirty_;
            }
            if (redraw) {
                present();
                hover_ = view_.hit_at(pointer_);   // hits moved if the page changed
            }
        }
        // ~60 Hz while the tab is open (pointer feedback), 5 Hz otherwise.
        sleep_ns(visible_ ? 16000000 : 200000000);
    }
}

} // namespace babble::ui
