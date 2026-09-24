#pragma once

#include <openvr.h>

#include <cstdint>
#include <memory>
#include <string>

namespace babble::ui {

// Hands CPU-drawn RGBA frames to an overlay as Vulkan images. SetOverlayRaw
// re-creates the overlay's texture on every call and flickers on the Frame;
// this copies into one of two device images (alternating, so the compositor
// never reads the one being written) instead.
//
// init() queries SteamVR for the required Vulkan extensions, so it must run
// while connected (inside SteamVrClient::with_vr). OpenVR requires VR_Shutdown
// before these Vulkan objects are destroyed: release() only after the SteamVR
// connection is gone. A new connection keeps using the same objects.
class VkUploader {
public:
    VkUploader();
    ~VkUploader();
    bool init(uint32_t width, uint32_t height, std::string *error);
    bool ready() const;
    // Copies pixels (width*height*4) into the next image and waits for the
    // copy; out is then ready for SetOverlayTexture.
    bool upload(const uint8_t *rgba, vr::VRVulkanTextureData_t *out, std::string *error);
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace babble::ui
