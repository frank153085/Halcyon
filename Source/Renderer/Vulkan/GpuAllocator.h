#pragma once

#include "../../Core/Result.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

enum class MemoryUsage : std::uint8_t
{
    GpuOnly,
    CpuToGpu,
    GpuToCpu,
    Auto,
};

struct BufferAllocation
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    std::uint64_t allocationId = 0;
};

struct ImageAllocation
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    std::uint64_t allocationId = 0;
};

class GpuAllocator final
{
public:
    GpuAllocator() noexcept;
    ~GpuAllocator();

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;
    GpuAllocator(GpuAllocator&&) noexcept;
    GpuAllocator& operator=(GpuAllocator&&) noexcept;

    [[nodiscard]] Halcyon::Result<void> initialize(
        VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
    [[nodiscard]] Halcyon::Result<BufferAllocation> createBuffer(
        const VkBufferCreateInfo& createInfo, MemoryUsage usage);
    [[nodiscard]] Halcyon::Result<ImageAllocation> createImage(
        const VkImageCreateInfo& createInfo, MemoryUsage usage);
    [[nodiscard]] Halcyon::Result<void> writeBuffer(
        BufferAllocation allocation, std::span<const std::byte> data, VkDeviceSize offset = 0);
    [[nodiscard]] Halcyon::Result<std::vector<std::byte>> readBuffer(
        BufferAllocation allocation, VkDeviceSize offset, VkDeviceSize size);

    void destroy(BufferAllocation allocation) noexcept;
    void destroy(ImageAllocation allocation) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] VkDeviceSize allocatedBytes() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon::Vulkan
