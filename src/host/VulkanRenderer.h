// SPDX-License-Identifier: MIT
#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <vector>

struct SDL_Window;

namespace tgr::host
{

// Host side of the graphics path: instance, device and swapchain on top of an
// SDL3 window. For now it shows a finished picture (the GS display, read back
// as RGBA8) scaled to 4:3 in the window; the native GS replacement will draw
// through it.
class VulkanRenderer
{
public:
    bool init(SDL_Window* window, bool enableValidation);
    void shutdown();

    // Shows rgba (width x height, RGBA8 rows) letterboxed to 4:3, or a black
    // screen when rgba is null.
    void drawFrame(const std::uint8_t* rgba = nullptr, std::uint32_t width = 0, std::uint32_t height = 0);
    void requestSwapchainRebuild() { swapchainDirty_ = true; }

private:
    bool createInstance(bool enableValidation);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createFrameResources();
    bool createSwapchain();
    void destroySwapchain();
    std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool preparePicture(std::uint32_t width, std::uint32_t height);
    void destroyPicture();

    static constexpr int kFramesInFlight = 2;

    struct Frame
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        // The picture is copied here, then into pictureImage_.
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void* stagingData = nullptr;
    };

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> frames_{};
    std::uint32_t frameIndex_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkSemaphore> renderFinished_; // one per swapchain image
    bool swapchainDirty_ = false;

    VkImage pictureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory pictureMemory_ = VK_NULL_HANDLE;
    std::uint32_t pictureWidth_ = 0;
    std::uint32_t pictureHeight_ = 0;
};

} // namespace tgr::host
