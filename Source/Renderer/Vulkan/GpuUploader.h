#pragma once

#include "GpuAllocator.h"

#include <cstddef>
#include <span>

namespace Halcyon::Vulkan
{

class GpuUploader final
{
public:
    GpuUploader() noexcept = default;
    GpuUploader(const GpuUploader&) = delete;
    GpuUploader& operator=(const GpuUploader&) = delete;

    [[nodiscard]] Halcyon::Result<void> uploadBuffer(VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        GpuAllocator& allocator,
        BufferAllocation destination,
        std::span<const std::byte> data);

    [[nodiscard]] Halcyon::Result<void> uploadImage(VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        GpuAllocator& allocator,
        ImageAllocation destination,
        VkExtent3D extent,
        VkFormat format,
        std::span<const std::byte> data,
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Upload level zero and generate the complete mip chain on the graphics
    // queue.  Unlike the legacy single-level helper this path validates the
    // physical-device linear-blit feature and fails explicitly when mip
    // generation is not supported.
    [[nodiscard]] Halcyon::Result<void> uploadImageWithMips(VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkCommandPool commandPool,
        VkQueue queue,
        GpuAllocator& allocator,
        ImageAllocation destination,
        VkExtent3D extent,
        VkFormat format,
        std::span<const std::byte> data,
        std::uint32_t mipLevels,
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
};

} // namespace Halcyon::Vulkan
