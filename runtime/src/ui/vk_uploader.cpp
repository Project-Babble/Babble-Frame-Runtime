#include "ui/vk_uploader.h"

#include <vulkan/vulkan.h>

#include <cstring>
#include <sstream>
#include <vector>

// Vulkan structs are initialised as {sType} with the rest zeroed on purpose.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace babble::ui {

namespace {

std::vector<std::string> split(const char *s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

std::vector<const char *> c_strs(const std::vector<std::string> &v) {
    std::vector<const char *> out;
    for (auto &s : v)
        out.push_back(s.c_str());
    return out;
}

} // namespace

struct VkUploader::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool initialized = false;
    } slots[2];
    int next = 0;
    uint32_t w = 0, h = 0;
    bool ready = false;

    bool mem_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) const {
        VkPhysicalDeviceMemoryProperties p;
        vkGetPhysicalDeviceMemoryProperties(pdev, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) {
                *out = i;
                return true;
            }
        return false;
    }

    bool fail(const char *what, VkResult r, std::string *error) {
        if (error)
            *error = std::string(what) + " failed (" + std::to_string(int(r)) + ")";
        return false;
    }

    bool init(uint32_t width, uint32_t height, std::string *error) {
        w = width;
        h = height;
        char buf[4096] = {};
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof(buf));
        auto iext_s = split(buf);
        auto iext = c_strs(iext_s);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "babble-runtime";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(iext.size());
        ici.ppEnabledExtensionNames = iext.data();
        if (VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS)
            return fail("vkCreateInstance", r, error);

        uint64_t out = 0;
        vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan, instance);
        pdev = reinterpret_cast<VkPhysicalDevice>(out);
        if (!pdev) {
            uint32_t n = 1;
            vkEnumeratePhysicalDevices(instance, &n, &pdev);
            if (!pdev)
                return fail("finding a Vulkan device", VK_ERROR_INITIALIZATION_FAILED, error);
        }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf.data());
        bool found = false;
        for (uint32_t i = 0; i < nq && !found; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                found = true;
            }
        if (!found)
            return fail("finding a graphics queue", VK_ERROR_INITIALIZATION_FAILED, error);

        memset(buf, 0, sizeof(buf));
        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(pdev, buf, sizeof(buf));
        auto dext_s = split(buf);
        auto dext = c_strs(dext_s);
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
        if (VkResult r = vkCreateDevice(pdev, &dci, nullptr, &dev); r != VK_SUCCESS)
            return fail("vkCreateDevice", r, error);
        vkGetDeviceQueue(dev, family, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        if (VkResult r = vkCreateCommandPool(dev, &pci, nullptr, &pool); r != VK_SUCCESS)
            return fail("vkCreateCommandPool", r, error);

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
            if (VkResult r = vkCreateImage(dev, &ii, nullptr, &s.image); r != VK_SUCCESS)
                return fail("vkCreateImage", r, error);
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev, s.image, &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            if (!mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex))
                return fail("finding image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY, error);
            if (VkResult r = vkAllocateMemory(dev, &ai, nullptr, &s.image_mem); r != VK_SUCCESS)
                return fail("vkAllocateMemory(image)", r, error);
            vkBindImageMemory(dev, s.image, s.image_mem, 0);

            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = VkDeviceSize(w) * h * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if (VkResult r = vkCreateBuffer(dev, &bi, nullptr, &s.staging); r != VK_SUCCESS)
                return fail("vkCreateBuffer", r, error);
            vkGetBufferMemoryRequirements(dev, s.staging, &mr);
            ai.allocationSize = mr.size;
            if (!mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &ai.memoryTypeIndex))
                return fail("finding staging memory", VK_ERROR_OUT_OF_HOST_MEMORY, error);
            if (VkResult r = vkAllocateMemory(dev, &ai, nullptr, &s.staging_mem); r != VK_SUCCESS)
                return fail("vkAllocateMemory(staging)", r, error);
            vkBindBufferMemory(dev, s.staging, s.staging_mem, 0);
            if (VkResult r = vkMapMemory(dev, s.staging_mem, 0, bi.size, 0, &s.mapped); r != VK_SUCCESS)
                return fail("vkMapMemory", r, error);

            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            if (VkResult r = vkAllocateCommandBuffers(dev, &cai, &s.cmd); r != VK_SUCCESS)
                return fail("vkAllocateCommandBuffers", r, error);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (VkResult r = vkCreateFence(dev, &fci, nullptr, &s.fence); r != VK_SUCCESS)
                return fail("vkCreateFence", r, error);
        }
        ready = true;
        return true;
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

    bool upload(const uint8_t *rgba, vr::VRVulkanTextureData_t *out, std::string *error) {
        Slot &s = slots[next];
        next ^= 1;
        if (VkResult r = vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS)
            return fail("vkWaitForFences", r, error);
        vkResetFences(dev, 1, &s.fence);
        memcpy(s.mapped, rgba, size_t(w) * h * 4);

        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmd, &bb);
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
        vkEndCommandBuffer(s.cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        if (VkResult r = vkQueueSubmit(queue, 1, &si, s.fence); r != VK_SUCCESS)
            return fail("vkQueueSubmit", r, error);
        if (VkResult r = vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS)
            return fail("vkWaitForFences", r, error);
        s.initialized = true;

        *out = {};
        out->m_nImage = uint64_t(s.image);
        out->m_pDevice = dev;
        out->m_pPhysicalDevice = pdev;
        out->m_pInstance = instance;
        out->m_pQueue = queue;
        out->m_nQueueFamilyIndex = family;
        out->m_nWidth = w;
        out->m_nHeight = h;
        out->m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
        out->m_nSampleCount = 1;
        return true;
    }

    void release() {
        ready = false;
        if (dev) {
            vkDeviceWaitIdle(dev);
            for (auto &s : slots) {
                if (s.fence) vkDestroyFence(dev, s.fence, nullptr);
                if (s.staging) vkDestroyBuffer(dev, s.staging, nullptr);
                if (s.staging_mem) vkFreeMemory(dev, s.staging_mem, nullptr);
                if (s.image) vkDestroyImage(dev, s.image, nullptr);
                if (s.image_mem) vkFreeMemory(dev, s.image_mem, nullptr);
                s = Slot{};
            }
            if (pool) vkDestroyCommandPool(dev, pool, nullptr);
            vkDestroyDevice(dev, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        pool = VK_NULL_HANDLE;
        dev = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
        pdev = VK_NULL_HANDLE;
    }
};

VkUploader::VkUploader() : impl_(std::make_unique<Impl>()) {}
VkUploader::~VkUploader() { release(); }

bool VkUploader::init(uint32_t width, uint32_t height, std::string *error) {
    release();
    if (impl_->init(width, height, error))
        return true;
    release();
    return false;
}

bool VkUploader::ready() const { return impl_->ready; }

bool VkUploader::upload(const uint8_t *rgba, vr::VRVulkanTextureData_t *out, std::string *error) {
    return impl_->ready && impl_->upload(rgba, out, error);
}

void VkUploader::release() { impl_->release(); }

} // namespace babble::ui
