#include "GpuAllocator.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vk_mem_alloc.h>

namespace Halcyon::Vulkan
{
namespace
{

[[nodiscard]] Halcyon::Error vmaError(const char* operation, VkResult result)
{
    const Halcyon::ErrorCode code =
        result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY
            ? Halcyon::ErrorCode::OutOfMemory
            : (result == VK_ERROR_DEVICE_LOST ? Halcyon::ErrorCode::DeviceLost
                                              : Halcyon::ErrorCode::Backend);
    return Halcyon::Error{code, operation};
}

[[nodiscard]] VmaMemoryUsage toVmaUsage(MemoryUsage usage) noexcept
{
    switch (usage)
    {
        case MemoryUsage::GpuOnly:
            return VMA_MEMORY_USAGE_GPU_ONLY;
        case MemoryUsage::CpuToGpu:
            return VMA_MEMORY_USAGE_CPU_TO_GPU;
        case MemoryUsage::GpuToCpu:
            return VMA_MEMORY_USAGE_GPU_TO_CPU;
        case MemoryUsage::Auto:
            return VMA_MEMORY_USAGE_AUTO;
    }
    return VMA_MEMORY_USAGE_AUTO;
}

} // namespace

struct GpuAllocator::Impl
{
    struct Record
    {
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        bool isImage = false;
    };

    VmaAllocator allocator = VK_NULL_HANDLE;
    std::unordered_map<std::uint64_t, Record> records;
    std::uint64_t nextId = 1;
    VkDeviceSize bytes = 0;
};

GpuAllocator::GpuAllocator() noexcept = default;
GpuAllocator::~GpuAllocator()
{
    shutdown();
}
GpuAllocator::GpuAllocator(GpuAllocator&&) noexcept = default;
GpuAllocator& GpuAllocator::operator=(GpuAllocator&&) noexcept = default;

Halcyon::Result<void> GpuAllocator::initialize(
    VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device)
{
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Vulkan handles must be valid"});
    }
    shutdown();
    impl_ = std::make_unique<Impl>();
    VmaAllocatorCreateInfo info{};
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    const VkResult result = vmaCreateAllocator(&info, &impl_->allocator);
    if (result != VK_SUCCESS)
    {
        impl_.reset();
        return Halcyon::Result<void>::failure(vmaError("vmaCreateAllocator", result));
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<BufferAllocation> GpuAllocator::createBuffer(
    const VkBufferCreateInfo& createInfo, MemoryUsage usage)
{
    if (!impl_ || impl_->allocator == VK_NULL_HANDLE)
    {
        return Halcyon::Result<BufferAllocation>::failure(
            {Halcyon::ErrorCode::InvalidState, "GPU allocator is not initialized"});
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = toVmaUsage(usage);
    const VkResult result = vmaCreateBuffer(
        impl_->allocator, &createInfo, &allocationInfo, &buffer, &allocation, nullptr);
    if (result != VK_SUCCESS)
    {
        return Halcyon::Result<BufferAllocation>::failure(vmaError("vmaCreateBuffer", result));
    }
    VmaAllocationInfo details{};
    vmaGetAllocationInfo(impl_->allocator, allocation, &details);
    const std::uint64_t id = impl_->nextId++;
    try
    {
        impl_->records.emplace(
            id, Impl::Record{allocation, buffer, VK_NULL_HANDLE, details.size, false});
    }
    catch (...)
    {
        vmaDestroyBuffer(impl_->allocator, buffer, allocation);
        return Halcyon::Result<BufferAllocation>::failure(
            {Halcyon::ErrorCode::OutOfMemory, "GPU allocation bookkeeping failed"});
    }
    impl_->bytes += details.size;
    return BufferAllocation{buffer, details.size, id};
}

Halcyon::Result<ImageAllocation> GpuAllocator::createImage(
    const VkImageCreateInfo& createInfo, MemoryUsage usage)
{
    if (!impl_ || impl_->allocator == VK_NULL_HANDLE)
    {
        return Halcyon::Result<ImageAllocation>::failure(
            {Halcyon::ErrorCode::InvalidState, "GPU allocator is not initialized"});
    }
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = toVmaUsage(usage);
    const VkResult result = vmaCreateImage(
        impl_->allocator, &createInfo, &allocationInfo, &image, &allocation, nullptr);
    if (result != VK_SUCCESS)
    {
        return Halcyon::Result<ImageAllocation>::failure(vmaError("vmaCreateImage", result));
    }
    VmaAllocationInfo details{};
    vmaGetAllocationInfo(impl_->allocator, allocation, &details);
    const std::uint64_t id = impl_->nextId++;
    try
    {
        impl_->records.emplace(
            id, Impl::Record{allocation, VK_NULL_HANDLE, image, details.size, true});
    }
    catch (...)
    {
        vmaDestroyImage(impl_->allocator, image, allocation);
        return Halcyon::Result<ImageAllocation>::failure(
            {Halcyon::ErrorCode::OutOfMemory, "GPU allocation bookkeeping failed"});
    }
    impl_->bytes += details.size;
    return ImageAllocation{image, details.size, id};
}

Halcyon::Result<void> GpuAllocator::writeBuffer(
    BufferAllocation allocation, std::span<const std::byte> data, VkDeviceSize offset)
{
    if (!impl_ || impl_->allocator == VK_NULL_HANDLE || allocation.allocationId == 0 ||
        data.empty())
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid buffer upload request"});
    }
    const auto it = impl_->records.find(allocation.allocationId);
    if (it == impl_->records.end() || it->second.isImage || offset > it->second.size ||
        data.size_bytes() > it->second.size - offset)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Buffer upload exceeds allocation"});
    }
    void* mapped = nullptr;
    const VkResult mapResult = vmaMapMemory(impl_->allocator, it->second.allocation, &mapped);
    if (mapResult != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(vmaError("vmaMapMemory", mapResult));
    }
    std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size_bytes());
    const VkResult flushResult =
        vmaFlushAllocation(impl_->allocator, it->second.allocation, offset, data.size_bytes());
    vmaUnmapMemory(impl_->allocator, it->second.allocation);
    if (flushResult != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(vmaError("vmaFlushAllocation", flushResult));
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<std::vector<std::byte>> GpuAllocator::readBuffer(
    BufferAllocation allocation, VkDeviceSize offset, VkDeviceSize size)
{
    if (impl_ == nullptr || allocation.buffer == VK_NULL_HANDLE || size == 0 ||
        offset > allocation.size || size > allocation.size - offset)
    {
        return Halcyon::Result<std::vector<std::byte>>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Invalid buffer read parameters"});
    }
    auto it = impl_->records.find(allocation.allocationId);
    if (it == impl_->records.end())
    {
        return Halcyon::Result<std::vector<std::byte>>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Unknown buffer allocation"});
    }
    void* mapped = nullptr;
    const VkResult mapResult = vmaMapMemory(impl_->allocator, it->second.allocation, &mapped);
    if (mapResult != VK_SUCCESS)
    {
        return Halcyon::Result<std::vector<std::byte>>::failure(vmaError("vmaMapMemory", mapResult));
    }
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    std::memcpy(result.data(), static_cast<const std::byte*>(mapped) + offset, result.size());
    vmaUnmapMemory(impl_->allocator, it->second.allocation);
    return Halcyon::Result<std::vector<std::byte>>::success(std::move(result));
}

void GpuAllocator::destroy(BufferAllocation allocation) noexcept
{
    if (!impl_ || allocation.allocationId == 0 || allocation.buffer == VK_NULL_HANDLE)
    {
        return;
    }
    const auto it = impl_->records.find(allocation.allocationId);
    if (it != impl_->records.end() && !it->second.isImage)
    {
        vmaDestroyBuffer(impl_->allocator, allocation.buffer, it->second.allocation);
        impl_->bytes -= it->second.size;
        impl_->records.erase(it);
    }
}

void GpuAllocator::destroy(ImageAllocation allocation) noexcept
{
    if (!impl_ || allocation.allocationId == 0 || allocation.image == VK_NULL_HANDLE)
    {
        return;
    }
    const auto it = impl_->records.find(allocation.allocationId);
    if (it != impl_->records.end() && it->second.isImage)
    {
        vmaDestroyImage(impl_->allocator, allocation.image, it->second.allocation);
        impl_->bytes -= it->second.size;
        impl_->records.erase(it);
    }
}

void GpuAllocator::shutdown() noexcept
{
    if (!impl_)
    {
        return;
    }
    if (impl_->allocator != VK_NULL_HANDLE)
    {
        for (const auto& [id, record] : impl_->records)
        {
            (void)id;
            if (record.isImage)
            {
                vmaDestroyImage(impl_->allocator, record.image, record.allocation);
            }
            else
            {
                vmaDestroyBuffer(impl_->allocator, record.buffer, record.allocation);
            }
        }
        vmaDestroyAllocator(impl_->allocator);
    }
    impl_.reset();
}

VkDeviceSize GpuAllocator::allocatedBytes() const noexcept
{
    return impl_ ? impl_->bytes : 0;
}

bool GpuAllocator::initialized() const noexcept
{
    return impl_ && impl_->allocator != VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
