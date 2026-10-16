#pragma once

#include <openvr.h>

#include <cstdint>
#include <memory>
#include <string>

namespace babble::ui {

// Sends CPU-drawn RGBA frames to an overlay through two alternating Vulkan images because SetOverlayRaw flickers.
class VkUploader {
public:
    VkUploader();
    ~VkUploader();
    // Call init() inside SteamVrClient::with_vr because it asks SteamVR for the Vulkan extensions.
    bool init(uint32_t width, uint32_t height, std::string *error);
    bool ready() const;
    // Copies the pixels into the next image, waits for the copy, and fills out for SetOverlayTexture.
    bool upload(const uint8_t *rgba, vr::VRVulkanTextureData_t *out, std::string *error);
    // Call release() only after VR_Shutdown because OpenVR requires this order.
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
