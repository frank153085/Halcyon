#pragma once

#include "Core/Result.h"
#include "HalcyonVulkanRenderer.h"

#include <cstdint>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace Halcyon::Vulkan
{

class VulkanDevice final
{
public:
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalProperties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentTimestampValidBits = 0;
    Capabilities capabilities{};
    std::uint64_t deviceLocalBytes = 0;
    bool rayQueryEnabled = false;

    [[nodiscard]] Halcyon::Result<void> initialize(
        GLFWwindow* window, const RendererConfig& config);
    void cleanup() noexcept;

private:
    [[nodiscard]] Halcyon::Result<void> createInstance();
    [[nodiscard]] Halcyon::Result<void> createSurface();
    [[nodiscard]] Halcyon::Result<void> pickPhysicalDevice();
    [[nodiscard]] Halcyon::Result<void> createDevice();

    GLFWwindow* window_ = nullptr;
    RendererConfig config_{};
    bool descriptorIndexingNonUniform_ = false;
};

} // namespace Halcyon::Vulkan
