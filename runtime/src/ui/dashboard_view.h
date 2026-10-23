#pragma once

#include "platform/steamvr_client.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/image.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace babble::ui {

enum class Health { Off, Starting, Paused, Working, Fault };

// The config source names of the feeds on the front page, in order.
inline constexpr const char *kFeedSources[2] = {"mouth", "eye"};

// The status of one camera feed as the front page shows it.
struct FeedStatus {
    Health health = Health::Off;
    double hz = 0;           // The tracking rate in the Working state.
    std::string detail;      // The error for the Fault state.
    bool available = false;  // False hides the on/off switch.
    bool tracking = true;    // The on/off setting of the user.
};

// The state of the tracking visualizer app that the front page can start.
enum class AppState { Unavailable, Stopped, Running };

// The output for one eye of the wearer, without a mirror effect.
struct EyeReading {
    bool gaze = false, lid = false;   // False means that there is no recent value.
    double yaw_deg = 0, pitch_deg = 0;   // Positive values are right and up.
    double openness = 0;                 // The Lid output, from 0 to 1.
    double widen = 0;                    // The Widen output, from 0 to 1.
    bool raw = false;                    // True if raw_lid is recent.
    double raw_lid = 0;                  // The lid value before calibration.
    bool cal = false;                    // True if neutral and wide_raw are recent.
    double neutral = 0, wide_raw = 0;    // The neutral and wide ceiling of the calibration, in raw units.
    bool dilation_ok = false;            // False until the pupil baseline exists.
    double dilation = 0;                 // The Dilation output, from 0 to 1, where 0.5 is the baseline.
};

// The status of the output for the front page: the PC link, VRChat native face tracking or Babble OSC.
struct LinkReading {
    bool available = false;   // False hides the line because no output runs.
    bool linked = false;
    std::string path, pc, address;
    double rtt_ms = -1;       // The value is -1 until the first measurement.
    bool native = false;
    bool osc = false;         // True for Babble OSC, where address has the port.
    bool no_data = false;     // True if the PC is linked but gets none of our data.
    bool restart = false;     // True if VRCFaceTracking must restart to claim the tracking that is on.
    int more = 0;             // The number of other outputs that are on.
    bool operator==(const LinkReading &) const = default;
};

// One setting of an output module, which shows in the row of the module on the Outputs page.
struct ModuleOption {
    std::string id, label;    // The id is the JSON pointer of the setting.
    bool text = false;        // A text setting opens the keyboard, and the other settings are a row of choices.
    bool numeric = false;     // The keyboard text of a numeric setting is sent as a number.
    std::vector<std::string> values, labels;   // The choices as JSON text, with one label for each.
    std::string value;        // The current value: JSON text for a choice, or the text that the setting shows.
    bool operator==(const ModuleOption &) const = default;
};

// One output module on the Outputs page.
struct OutputModule {
    std::string id, label, hint;   // The id is the key under "outputs".
    bool on = false;
    bool ok = false;               // True if the output works, which shows as a green dot.
    std::string status;            // What the output does now, which replaces the hint while it is on.
    std::vector<ModuleOption> options;
    bool operator==(const OutputModule &) const = default;
};

// A USB camera that the mouth can use.
struct UsbCamera {
    std::string id;       // The value of usb::device_id(), which is also the setting value.
    std::string label;
    std::string detail;   // The USB ID and serial number.
};

// The row on the Diagnostics page for the optional dataset capture app.
struct CaptureRow {
    enum class State { Hidden, Checking, NotInstalled, Downloading, Installing, Installed, UpdateAvailable, Running, Failed };
    State state = State::Hidden;
    std::string version{}, latest{}, detail{};
    float progress = 0;           // The download progress, from 0 to 1.
    bool recording = false;
    int uploads_pending = 0;
    float upload_progress = 0;    // The progress of the active stage, from 0 to 1.
    std::string upload_stage{};   // The stage is packaging, encrypting or uploading.
    bool operator==(const CaptureRow &) const = default;
};

// A runtime update that the front page shows as a pill next to the version.
struct UpdateNotice {
    enum class State { Hidden, Available, Downloading, Installing, RestartNeeded, Failed };
    State state = State::Hidden;
    std::string version{}, detail{};   // The detail is the failure message.
    float progress = 0;                // The download progress, from 0 to 1.
    bool operator==(const UpdateNotice &) const = default;
};

struct Status {
    bool steamvr = false;
    Worn worn = Worn::Unknown;
    FeedStatus mouth, eye;
    AppState visualizer = AppState::Unavailable;   // Unavailable hides the button.
    std::array<EyeReading, 2> eyes{};              // Index 0 is the left eye and index 1 is the right eye.
    LinkReading link{};
    // The mouth source is "frunk" or "usb", and camera "" means automatic.
    std::string mouth_source = "frunk";
    bool mouth_source_selectable = true;   // False hides the Frunk/USB row because the USB camera is the only mouth.
    std::vector<UsbCamera> cameras{};
    std::string camera{}, active_camera{};
    // The data for the Diagnostics page, where verbose_minutes_left is 0 if verbose logging is off.
    int verbose_minutes_left = 0;
    std::string last_diagnostics{}, log_file{}, stats_file{};
    // Live hints by setting id that replace the fixed hint of a setting row.
    std::map<std::string, std::string> choice_hints{};
    CaptureRow capture{};
    UpdateNotice update{};
    // The data for the Outputs page, where pc_address "" means automatic.
    std::vector<OutputModule> outputs{};
    std::string pc_address{}, output_error{};
};

// A user setting that shows as a row of choices or as a switch, where id is its JSON pointer in the config.
// The page that a setting shows on.
enum class ChoicePage { Settings, Eye, Mouth, Cameras, Diagnostics };

struct Choice {
    std::string id, label, hint, unit;
    std::vector<double> options;
    double value = 0;
    bool boolean = false;
    ChoicePage page = ChoicePage::Settings;
    std::vector<std::string> option_labels{};   // If set, one label for each option replaces the value and unit.
};

struct Point {
    float x = 0, y = 0;
};

// The optional debug dashboard that the CPU draws, without OpenVR so that tests can render it.
class DashboardView {
public:
    static constexpr int kWidth = 1200, kHeight = 750, kThumb = 128;
    enum class Page { Home, Settings, Cameras, Diagnostics, Outputs, Eye, Mouth };

    bool load_fonts(const std::filesystem::path &dir);   // Loads NotoSans-Regular.ttf and NotoSans-Medium.ttf.
    bool load_icons(const std::filesystem::path &dir);   // Loads the optional logo PNG files.

    // Draws the current page, with hover effects at pointer in canvas pixels from the top left.
    void render(Canvas &c, const Status &status, const std::vector<Choice> &choices, std::optional<Point> pointer);
    void render_thumbnail(Canvas &c);

    struct ClickResult {
        bool redraw = false;
        std::optional<std::pair<size_t, double>> setting;   // The choice index and the new value.
        std::optional<std::pair<size_t, bool>> toggle;      // The feed index and the tracking on/off value.
        bool visualizer = false;                            // Start or stop the visualizer.
        bool restart = false;                               // Restart the runtime after confirmation.
        std::optional<std::string> mouth_source;            // The value is "frunk" or "usb".
        std::optional<std::string> camera;                  // A UsbCamera id, or "" for automatic.
        bool refresh_cameras = false;
        std::optional<bool> verbose;                        // Turn verbose logging on for a time or off.
        bool write_diagnostics = false;
        bool opened_diagnostics = false;
        // The button that the user clicked on the dataset capture row.
        enum class CaptureAction { None, Primary, Remove } capture = CaptureAction::None;
        // True if the user clicked the update pill, and the update state sets what the click does.
        bool update = false;
        // A choice on the Outputs page as a setting id and the new value as JSON text.
        std::optional<std::pair<std::string, std::string>> output_setting;
        std::optional<std::string> edit_text;   // The id of the text setting to type in with the keyboard.
    };
    // Finds the element at p in the last render.
    ClickResult click(Point p);
    // Returns the clickable element under the pointer in the last render, or -1 for none.
    int hit_at(std::optional<Point> p) const;

    // Returns true while the restart button waits for a second click, which must come in less than 3 s.
    bool restart_armed(uint64_t now_ns) const { return now_ns < restart_armed_until_ns_; }
    // Returns true while the update pill waits for a second click to restart SteamVR.
    bool update_armed(uint64_t now_ns) const { return now_ns < update_armed_until_ns_; }

    Page page() const { return page_; }
    void set_page(Page p) { page_ = p; }

private:
    struct Hit {
        enum Kind {
            Cog, Back, Option, Toggle, Visualizer, Restart, MouthSource, OpenCameras, Camera, Refresh,
            OpenDiagnostics, Verbose, WriteDiagnostics, CapturePrimary, CaptureRemove, Update, OpenOutputs,
            OutputSetting, EditText, OpenEye, OpenMouth
        } kind;
        Rect rect;
        size_t choice = 0;   // The choice index for Option, or the feed index for Toggle.
        double value = 0;    // The value for Option, or 1 to turn on for Toggle.
        std::string text{};  // The value for MouthSource, the camera id for Camera, or the setting id.
        std::string json{};  // The new value as JSON text for OutputSetting.
    };
    void render_home(Canvas &c, const Status &s);
    void render_settings(Canvas &c, const std::vector<Choice> &choices, const Status &s);
    void render_eye_page(Canvas &c, const std::vector<Choice> &choices, const Status &s);
    void render_mouth_page(Canvas &c, const std::vector<Choice> &choices, const Status &s);
    void render_cameras(Canvas &c, const std::vector<Choice> &choices, const Status &s);
    void render_diagnostics(Canvas &c, const std::vector<Choice> &choices, const Status &s);
    void render_outputs(Canvas &c, const Status &s);
    // Draws a text setting as a button with its value, and returns its left edge.
    float render_text_field(Canvas &c, float right, float y, const std::string &id, const std::string &shown);
    void render_switch_row(Canvas &c, float y, size_t index, const Choice &ch, float row_h = 124);
    void render_capture_row(Canvas &c, float y, float row_h, const CaptureRow &row);
    void render_update_pill(Canvas &c, float x, float baseline, const UpdateNotice &u);
    Rect render_button(Canvas &c, float right, float y, const std::string &text, bool primary,
                       bool on_card = false);   // Returns the rect of the button.
    void render_back(Canvas &c, const char *title);
    float render_segments(Canvas &c, float right, float y, const std::vector<std::string> &labels, int selected,
                          const std::function<void(size_t, const Rect &)> &hit,
                          float seg_h = 64);   // Returns the left edge.
    void render_cog(Canvas &c);
    void render_switch(Canvas &c, Rect r, bool on, bool hover,
                       std::optional<Color> on_color = std::nullopt);   // on_color replaces the accent colour.
    float render_visualizer_button(Canvas &c, AppState state);   // Returns the left edge.
    void render_restart_button(Canvas &c);
    void render_link(Canvas &c, const LinkReading &link, float right);
    void render_eye(Canvas &c, float x, float y, const char *name, const EyeReading &e);
    enum class Icon { Mouth, Eye, Rate, Camera, Logs, Document, Settings, Home, Capture, Count };
    struct RowLayout {
        float top, row_h;
    };
    RowLayout row_layout(const Canvas &c, size_t rows, float below) const;
    void render_link_row(Canvas &c, float y, float row_h, Icon icon, const std::string &label, const std::string &hint,
                         Hit::Kind kind);
    float render_choice_rows(Canvas &c, float y, float row_h, const std::vector<Choice> &choices, ChoicePage page,
                             const Status &s);
    void render_icon(Canvas &c, Icon icon, float x, float y, Color color);
    float render_block(Canvas &c, float y, Icon icon, float row_h = 124);   // Returns the left edge of the text.
    bool hovered(const Rect &r) const;
    // Returns the text with an ellipsis so that it fits in max_w.
    std::string fit(const Font &f, const std::string &text, float px, float max_w) const;

    Page page_ = Page::Home;
    uint64_t restart_armed_until_ns_ = 0;   // The end of the confirmation time for the restart button.
    uint64_t update_armed_until_ns_ = 0;    // The end of the confirmation time for the update pill.
    Font regular_, medium_;
    std::optional<Image> logo_small_, logo_thumb_;
    std::array<std::optional<Image>, size_t(Icon::Count)> icons_{};
    std::vector<Hit> hits_;
    std::optional<Point> pointer_;
};

}
