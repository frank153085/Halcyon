#pragma once

#include "../../Core/Result.h"
#include "GpuAllocator.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan::GpuSceneDetail
{

[[nodiscard]] inline Halcyon::Result<BufferAllocation> createBuffer(
    GpuAllocator& allocator,
    std::size_t stride,
    std::uint32_t count,
    const char* name)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(stride) * count;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (name != nullptr)
    {
        const std::string_view label{name};
        if (label.find("indirect") != std::string_view::npos ||
            label.find("count") != std::string_view::npos)
        {
            info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        }
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return allocator.createBuffer(info, MemoryUsage::GpuOnly);
}

[[nodiscard]] inline const BufferAllocation& active(
    const std::vector<BufferAllocation>& buffers,
    std::uint32_t frame) noexcept
{
    static const BufferAllocation empty{};
    if (buffers.empty())
    {
        return empty;
    }
    return buffers[std::min<std::size_t>(frame, buffers.size() - 1u)];
}

[[nodiscard]] inline Halcyon::Result<void> createPerFrame(
    GpuAllocator& allocator,
    std::vector<BufferAllocation>& output,
    std::uint32_t frameCount,
    std::size_t stride,
    std::uint32_t count,
    const char* name)
{
    output.clear();
    output.reserve(frameCount);
    for (std::uint32_t frame = 0; frame < frameCount; ++frame)
    {
        auto allocation = createBuffer(allocator, stride, count, name);
        if (!allocation)
        {
            return allocation.error();
        }
        output.push_back(allocation.value());
    }
    return Halcyon::Result<void>::success();
}

inline void destroyPerFrame(
    GpuAllocator* allocator,
    std::vector<BufferAllocation>& buffers) noexcept
{
    if (allocator != nullptr)
    {
        for (auto& buffer : buffers)
        {
            allocator->destroy(buffer);
        }
    }
    buffers.clear();
}

} // namespace Halcyon::Vulkan::GpuSceneDetail
