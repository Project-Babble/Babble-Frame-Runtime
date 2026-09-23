#pragma once

#include "camera/camera_source.h"

#include <map>
#include <mutex>
#include <string>

namespace babble {

// ICameraControls over a V4L2 device or subdev node. Controls are enumerated
// once at open() and addressed by v4l2-ctl style names ("exposure",
// "analogue_gain", "strobe_enable"...).
class V4l2Controls : public ICameraControls {
public:
    ~V4l2Controls() override;

    bool open(const std::string &node);
    void close();
    bool is_open() const { return fd_ >= 0; }
    const std::string &node() const { return node_; }
    std::vector<std::string> names() const;

    bool has(std::string_view name) const override;
    std::optional<int64_t> get(std::string_view name) const override;
    bool set(std::string_view name, int64_t value) override;
    bool set_many(const std::vector<std::pair<std::string, int64_t>> &values) override;
    std::optional<ControlRange> range(std::string_view name) const override;

    // Same conversion v4l2-ctl applies to control names.
    static std::string normalize_name(const char *raw);

private:
    struct Info {
        uint32_t id;
        uint32_t type;
        ControlRange range;
    };
    const Info *find(std::string_view name) const;

    int fd_ = -1;
    std::string node_;
    std::map<std::string, Info, std::less<>> controls_;
    mutable std::mutex mutex_;
};

} // namespace babble
