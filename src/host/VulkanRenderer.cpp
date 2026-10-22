// SPDX-License-Identifier: MIT
#include "host/VulkanRenderer.h"

#include "app/Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

namespace tgr::host
{

namespace
{
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool check(VkResult result, const char* what)
{
    if (result == VK_SUCCESS)
        return true;
    log::error("{} failed (VkResult {})", what, static_cast<int>(result));
    return false;
}

bool hasLayer(const char* name)
{
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [name](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool hasDeviceExtension(VkPhysicalDevice gpu, const char* name)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, exts.data());
    return std::any_of(exts.begin(), exts.end(),
                       [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
} // namespace

bool VulkanRenderer::init(SDL_Window* window, bool enableValidation)
{
    window_ = window;

    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!getInstanceProcAddr)
    {
        log::error("No Vulkan driver found: {}", SDL_GetError());
        return false;
    }
    volkInitializeCustom(getInstanceProcAddr);

    return createInstance(enableValidation) && pickPhysicalDevice() && createDevice() && createFrameResources() &&
           createSwapchain();
}

bool VulkanRenderer::createInstance(bool enableValidation)
{
    Uint32 sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts)
    {
        log::error("SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);

    std::vector<const char*> layers;
    if (enableValidation)
    {
        if (hasLayer(kValidationLayer))
            layers.push_back(kValidationLayer);
        else
            log::warn("--validation needs {} from the Vulkan SDK; running without it", kValidationLayer);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TheGetawayRecomp";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "TheGetawayRecomp";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();

    if (!check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance"))
        return false;
    volkLoadInstance(instance_);

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_))
    {
        log::error("SDL_Vulkan_CreateSurface: {}", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice()
{
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance_, &count, gpus.data());

    int bestScore = -1;
    for (VkPhysicalDevice gpu : gpus)
    {
        if (!hasDeviceExtension(gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());

        for (std::uint32_t i = 0; i < familyCount; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;

            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu, &props);
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore)
            {
                bestScore = score;
                gpu_ = gpu;
                queueFamily_ = i;
            }
            break;
        }
    }

    if (!gpu_)
    {
        log::error("No Vulkan GPU with graphics + present support was found");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    log::info("GPU: {} (Vulkan {}.{}.{})", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
              VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    return true;
}

bool VulkanRenderer::createDevice()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;

    if (!check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice"))
        return false;
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool VulkanRenderer::createFrameResources()
{
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queueFamily_;
    if (!check(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool"))
        return false;

    for (Frame& frame : frames_)
    {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = commandPool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (!check(vkAllocateCommandBuffers(device_, &alloc, &frame.cmd), "vkAllocateCommandBuffers") ||
            !check(vkCreateSemaphore(device_, &sem, nullptr, &frame.imageAvailable), "vkCreateSemaphore") ||
            !check(vkCreateFence(device_, &fence, nullptr, &frame.inFlight), "vkCreateFence"))
            return false;
    }
    return true;
}

bool VulkanRenderer::createSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        log::error("The swapchain does not support transfer writes");
        return false;
    }

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &formatCount, formats.data());
    if (formats.empty())
        return false;
    VkSurfaceFormatKHR format = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            format = f;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent.width = std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height =
            std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return true; // minimized; try again later

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount != 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainKHR oldSwapchain = swapchain_;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; always available
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (!check(vkCreateSwapchainKHR(device_, &info, nullptr, &newSwapchain), "vkCreateSwapchainKHR"))
        return false;
    destroySwapchain();
    swapchain_ = newSwapchain;
    swapchainFormat_ = format.format;
    swapchainExtent_ = extent;

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());

    renderFinished_.resize(count);
    for (VkSemaphore& sem : renderFinished_)
    {
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!check(vkCreateSemaphore(device_, &semInfo, nullptr, &sem), "vkCreateSemaphore"))
            return false;
    }

    swapchainDirty_ = false;
    return true;
}

void VulkanRenderer::destroySwapchain()
{
    for (VkSemaphore sem : renderFinished_)
        vkDestroySemaphore(device_, sem, nullptr);
    renderFinished_.clear();
    swapchainImages_.clear();
    if (swapchain_)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::drawFrame(float r, float g, float b)
{
    if (!device_)
        return;

    if (swapchainDirty_ || !swapchain_)
    {
        vkDeviceWaitIdle(device_);
        if (!createSwapchain() || swapchainDirty_ || !swapchain_)
            return;
    }

    Frame& frame = frames_[frameIndex_];
    vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    std::uint32_t imageIndex = 0;
    VkResult acquired =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
    {
        swapchainDirty_ = true;
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
    {
        check(acquired, "vkAcquireNextImageKHR");
        return;
    }
    vkResetFences(device_, 1, &frame.inFlight);

    VkCommandBuffer cmd = frame.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImage image = swapchainImages_[imageIndex];
    transition(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue color{{r, g, b, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_[imageIndex];
    if (!check(vkQueueSubmit(queue_, 1, &submit, frame.inFlight), "vkQueueSubmit"))
        return;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        swapchainDirty_ = true;

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void VulkanRenderer::shutdown()
{
    if (device_)
    {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (Frame& frame : frames_)
        {
            vkDestroyFence(device_, frame.inFlight, nullptr);
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        }
        frames_ = {};
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_)
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (instance_)
        vkDestroyInstance(instance_, nullptr);
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

} // namespace tgr::host
