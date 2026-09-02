#pragma once

#include "Core/Result.h"
#include "GpuAllocator.h"

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
    VkImage depthImage = VK_NULL_HANDLE;
    ImageAllocation depthAllocation{};
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkDeviceSize depthMemorySize = 0;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool depthImageInitialized = false;
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
        GpuAllocator& allocator,
        VkExtent2D requestedExtent) noexcept;
    [[nodiscard]] Halcyon::Result<void> create();
    [[nodiscard]] Halcyon::Result<void> recreate();
    [[nodiscard]] VkResult acquire(
        VkSemaphore imageAvailable, std::uint32_t& imageIndex) const noexcept;
    void cleanup() noexcept;

private:
    [[nodiscard]] Halcyon::Result<void> createDepthResources(VkExtent2D extent,
        VkImage& outImage,
        ImageAllocation& outAllocation,
        VkImageView& outView,
        VkDeviceSize& outMemorySize);
    void destroyDepthResources() noexcept;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    GLFWwindow* window_ = nullptr;
    std::uint32_t graphicsQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
    GpuAllocator* allocator_ = nullptr;
};

} // namespace Halcyon::Vulkan
