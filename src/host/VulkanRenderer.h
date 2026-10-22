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
// SDL3 window. For now it only clears the screen; the native GS replacement
// will draw through it.
class VulkanRenderer
{
public:
    bool init(SDL_Window* window, bool enableValidation);
    void shutdown();

    void drawFrame(float r, float g, float b);
    void requestSwapchainRebuild() { swapchainDirty_ = true; }

private:
    bool createInstance(bool enableValidation);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createFrameResources();
    bool createSwapchain();
    void destroySwapchain();

    static constexpr int kFramesInFlight = 2;

    struct Frame
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
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
};

} // namespace tgr::host
