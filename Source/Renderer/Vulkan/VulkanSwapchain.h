#pragma once

#include "Core/Result.h"
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace Halcyon::Vulkan
{

class VulkanSwapchain final
{
public:
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchainExtent{};
    // M3 scene depth is a FrameGraph resource. The swapchain only owns
    // presentation images; keeping a second depth target here would bypass
    // VulkanFrameGraphProvider's VMA ownership and resize lifecycle.
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkSemaphore> presentReadySemaphores;
    std::vector<bool> swapchainImageInitialized;
    VkExtent2D requestedExtent{};
    bool framebufferResized = false;
    bool deviceLost = false;

    [[nodiscard]] Halcyon::Result<void> initialize(VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkSurfaceKHR surface,
        GLFWwindow* window,
        std::uint32_t graphicsQueueFamily,
        std::uint32_t presentQueueFamily,
        VkExtent2D requestedExtent) noexcept;
    [[nodiscard]] Halcyon::Result<void> create();
    [[nodiscard]] Halcyon::Result<void> recreate();
    [[nodiscard]] VkResult acquire(
        VkSemaphore imageAvailable, std::uint32_t& imageIndex) const noexcept;
    void cleanup() noexcept;

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    GLFWwindow* window_ = nullptr;
    std::uint32_t graphicsQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
};

} // namespace Halcyon::Vulkan
