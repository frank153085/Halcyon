#pragma once

#include "Core/Result.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

struct VulkanFrame
{
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::uint64_t timelineValue = 0;
    std::uint32_t queryBase = 0;
    bool submitted = false;
};

class VulkanFrameContext final
{
public:
    std::vector<VulkanFrame> frames;
    std::uint32_t currentFrame = 0;
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    std::uint64_t nextTimelineValue = 1;
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    bool timestampsEnabled = false;
    float timestampPeriod = 1.0f;
    std::uint32_t presentTimestampValidBits = 0;

    [[nodiscard]] Halcyon::Result<void> initialize(VkDevice device,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceProperties& physicalProperties,
        std::uint32_t graphicsQueueFamily,
        std::uint32_t requestedFrameCount);
    void cleanup(VkDevice device) noexcept;

    [[nodiscard]] Halcyon::Result<void> createTimeline(VkDevice device);
    [[nodiscard]] Halcyon::Result<void> createResources(VkDevice device,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceProperties& physicalProperties,
        std::uint32_t graphicsQueueFamily,
        std::uint32_t requestedFrameCount);

    // Submit and present are kept with the per-frame synchronization state so
    // the renderer only orchestrates high-level frame flow.  The methods
    // return the raw Vulkan status to let the caller apply its device-loss and
    // swapchain recovery policy.
    [[nodiscard]] VkResult submit(
        VkQueue graphicsQueue, VulkanFrame& frame, VkSemaphore presentReady) noexcept;
    [[nodiscard]] VkResult wait(VkDevice device, VulkanFrame& frame) const noexcept;
    [[nodiscard]] VkResult resetCommandPool(VkDevice device, VulkanFrame& frame) const noexcept;
    [[nodiscard]] VkResult resetFence(VkDevice device, VulkanFrame& frame) const noexcept;
    [[nodiscard]] VkResult readGpuTime(
        VkDevice device, const VulkanFrame& frame, double& milliseconds) const noexcept;
    [[nodiscard]] VkResult present(VkQueue presentQueue,
        VkSwapchainKHR swapchain,
        VkSemaphore presentReady,
        std::uint32_t imageIndex) noexcept;
};

} // namespace Halcyon::Vulkan
