// overlay_probe: create a "Babble" SteamVR dashboard tab with a test panel.
//
//   overlay_probe [seconds] [--raw]      (default 120 s, Vulkan upload)
//
// Panel: dark background, a yellow square that follows the laser pointer,
// and a green bar that grows per click. Prints dashboard/mouse events so we
// can see whether the Frame's shell hosts dashboard overlays and routes laser
// input to them.
//
// Upload paths: --raw uses SetOverlayRaw (flickers on the Frame on every
// update); the default draws on the CPU, copies into one of two Vulkan images
// and hands that image to SetOverlayTexture.
#include <openvr.h>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define VK(x)                                                                                                          \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_ != VK_SUCCESS) {                                                                                        \
            printf("%s failed: %d\n", #x, int(r_));                                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

namespace {

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

// Two device-local RGBA images, each with its own host-visible staging buffer.
struct VkUploader {
    VkInstance instance{};
    VkPhysicalDevice pdev{};
    VkDevice dev{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
    struct Slot {
        VkImage image{};
        VkDeviceMemory image_mem{};
        VkBuffer staging{};
        VkDeviceMemory staging_mem{};
        void *mapped = nullptr;
        VkCommandBuffer cmd{};
        VkFence fence{};
        bool initialized = false;
    } slots[2];
    int next = 0;
    uint32_t w = 0, h = 0;

    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties p;
        vkGetPhysicalDeviceMemoryProperties(pdev, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
                return i;
        printf("no memory type\n");
        exit(1);
    }

    void init(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        char buf[4096] = {};
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof(buf));
        auto iext_s = split(buf);
        std::vector<const char *> iext;
        for (auto &s : iext_s)
            iext.push_back(s.c_str());
        printf("vulkan instance extensions: %s\n", buf);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "babble-overlay-probe";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(iext.size());
        ici.ppEnabledExtensionNames = iext.data();
        VK(vkCreateInstance(&ici, nullptr, &instance));

        uint64_t out = 0;
        vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan, instance);
        pdev = reinterpret_cast<VkPhysicalDevice>(out);
        if (!pdev) {
            uint32_t n = 1;
            vkEnumeratePhysicalDevices(instance, &n, &pdev);
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pdev, &props);
        printf("vulkan device: %s\n", props.deviceName);

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf.data());
        for (uint32_t i = 0; i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                break;
            }

        memset(buf, 0, sizeof(buf));
        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(pdev, buf, sizeof(buf));
        auto dext_s = split(buf);
        std::vector<const char *> dext;
        for (auto &s : dext_s)
            dext.push_back(s.c_str());
        printf("vulkan device extensions: %s\n", buf);
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(dext.size());
        dci.ppEnabledExtensionNames = dext.data();
        VK(vkCreateDevice(pdev, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, family, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        VK(vkCreateCommandPool(dev, &pci, nullptr, &pool));

        for (auto &s : slots) {
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = {w, h, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK(vkCreateImage(dev, &ii, nullptr, &s.image));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev, s.image, &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK(vkAllocateMemory(dev, &ai, nullptr, &s.image_mem));
            VK(vkBindImageMemory(dev, s.image, s.image_mem, 0));

            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = VkDeviceSize(w) * h * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VK(vkCreateBuffer(dev, &bi, nullptr, &s.staging));
            vkGetBufferMemoryRequirements(dev, s.staging, &mr);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex =
                mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK(vkAllocateMemory(dev, &ai, nullptr, &s.staging_mem));
            VK(vkBindBufferMemory(dev, s.staging, s.staging_mem, 0));
            VK(vkMapMemory(dev, s.staging_mem, 0, bi.size, 0, &s.mapped));

            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VK(vkAllocateCommandBuffers(dev, &cai, &s.cmd));
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK(vkCreateFence(dev, &fci, nullptr, &s.fence));
        }
    }

    static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                        VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }

    // Copies pixels into the next image and gives it to the overlay.
    vr::EVROverlayError present(vr::VROverlayHandle_t overlay, const uint8_t *pixels) {
        Slot &s = slots[next];
        next ^= 1;
        VK(vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX));
        VK(vkResetFences(dev, 1, &s.fence));
        memcpy(s.mapped, pixels, size_t(w) * h * 4);

        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK(vkBeginCommandBuffer(s.cmd, &bb));
        barrier(s.cmd, s.image, s.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(s.cmd, s.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        // OpenVR expects submitted Vulkan images in TRANSFER_SRC_OPTIMAL.
        barrier(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VK(vkEndCommandBuffer(s.cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        VK(vkQueueSubmit(queue, 1, &si, s.fence));
        VK(vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX));
        s.initialized = true;

        vr::VRVulkanTextureData_t data{};
        data.m_nImage = uint64_t(s.image);
        data.m_pDevice = dev;
        data.m_pPhysicalDevice = pdev;
        data.m_pInstance = instance;
        data.m_pQueue = queue;
        data.m_nQueueFamilyIndex = family;
        data.m_nWidth = w;
        data.m_nHeight = h;
        data.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
        data.m_nSampleCount = 1;
        vr::Texture_t tex{&data, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        return vr::VROverlay()->SetOverlayTexture(overlay, &tex);
    }
};

} // namespace

int main(int argc, char **argv) {
    double seconds = 120.0;
    bool raw = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw"))
            raw = true;
        else
            seconds = atof(argv[i]);
    }
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) {
        printf("VR_Init(Overlay) failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::VROverlayHandle_t main = 0, thumb = 0;
    auto e = vr::VROverlay()->CreateDashboardOverlay("babble.probe", "Babble", &main, &thumb);
    printf("CreateDashboardOverlay = %d, upload via %s\n", int(e), raw ? "SetOverlayRaw" : "Vulkan");
    if (e != vr::VROverlayError_None)
        return 1;

    const uint32_t W = 640, H = 400;
    vr::VROverlay()->SetOverlayWidthInMeters(main, 1.6f);
    vr::VROverlay()->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{float(W), float(H)};
    vr::VROverlay()->SetOverlayMouseScale(main, &scale);

    std::vector<uint8_t> px(W * H * 4), th(64 * 64 * 4);
    for (size_t i = 0; i < th.size(); i += 4) { th[i] = 40; th[i + 1] = 160; th[i + 2] = 255; th[i + 3] = 255; }
    vr::VROverlay()->SetOverlayRaw(thumb, th.data(), 64, 64, 4);

    VkUploader vk;
    if (!raw)
        vk.init(W, H);

    float mx = W / 2.f, my = H / 2.f;
    int clicks = 0, uploads = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto draw = [&] {
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint8_t *p = &px[(y * W + x) * 4];
                bool cursor = std::abs(int(x) - int(mx)) < 20 && std::abs(int(y) - int(my)) < 20;
                bool bar = y > H - 40 && x < uint32_t(clicks * 40 % W);
                p[0] = cursor ? 255 : bar ? 60 : 24;
                p[1] = cursor ? 200 : bar ? 220 : 28;
                p[2] = cursor ? 40 : bar ? 90 : 36;
                p[3] = 255;
            }
        auto r = raw ? vr::VROverlay()->SetOverlayRaw(main, px.data(), W, H, 4) : vk.present(main, px.data());
        if (r != vr::VROverlayError_None)
            printf("upload failed: %d\n", int(r));
        uploads++;
    };
    draw();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        vr::VREvent_t ev;
        bool dirty = false;
        while (vr::VROverlay()->PollNextOverlayEvent(main, &ev, sizeof(ev))) {
            switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                mx = ev.data.mouse.x;
                my = H - ev.data.mouse.y;   // OpenVR mouse origin is bottom-left
                dirty = true;
                break;
            case vr::VREvent_MouseButtonDown:
                clicks++;
                printf("click %d at (%.0f, %.0f)\n", clicks, ev.data.mouse.x, ev.data.mouse.y);
                dirty = true;
                break;
            case vr::VREvent_DashboardActivated: printf("dashboard activated\n"); break;
            case vr::VREvent_DashboardDeactivated: printf("dashboard deactivated\n"); break;
            case vr::VREvent_OverlayShown: printf("our tab shown\n"); break;
            case vr::VREvent_OverlayHidden: printf("our tab hidden\n"); break;
            default: break;
            }
        }
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
            if (ev.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                goto done;
            }
        if (dirty)
            draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
done:
    printf("%d clicks, %d uploads\n", clicks, uploads);
    // OpenVR must shut down before the Vulkan objects go away (process exit here).
    vr::VR_Shutdown();
    return 0;
}
