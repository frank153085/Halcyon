#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Read-only Vulkan handles for diagnostics and overlay backends. Renderer
// keeps its public surface on lifecycle, recording, and scene updates; tools
// that must attach ImGui or a profiler go through this escape hatch.
struct RendererNativeHandles
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::uint32_t swapchainImageCount = 0;
};

} // namespace Halcyon::Vulkan
