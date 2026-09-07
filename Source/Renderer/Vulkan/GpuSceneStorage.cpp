#include "GpuSceneStorage.h"

#include "GpuSceneBufferUtil.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <span>
#include <utility>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> GpuSceneStorage::initialize(
    VkDevice device,
    GpuAllocator& allocator,
    std::uint32_t capacity)
{
    if (device_ != VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::AlreadyExists,
                "GPU scene storage is already initialized",
                "GpuSceneStorage"});
    }
    device_ = device;
    allocator_ = &allocator;
    capacity_ = std::max(1u, capacity);
    auto transforms = GpuSceneDetail::createBuffer(
        allocator, sizeof(Renderer::Scene::TransformRow), capacity_, "transforms");
    auto bounds = GpuSceneDetail::createBuffer(
        allocator, sizeof(Renderer::Scene::BoundsRow), capacity_, "bounds");
    auto meshMaterials = GpuSceneDetail::createBuffer(
        allocator,
        sizeof(Renderer::Scene::MeshMaterialRow),
        capacity_,
        "mesh materials");
    auto materials = GpuSceneDetail::createBuffer(
        allocator, sizeof(Renderer::Scene::MaterialGpuData), capacity_, "materials");
    if (!transforms || !bounds || !meshMaterials || !materials)
    {
        cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to allocate GPU scene storage",
                "GpuSceneStorage"});
    }
    transforms_ = transforms.value();
    bounds_ = bounds.value();
    meshMaterials_ = meshMaterials.value();
    materials_ = materials.value();
    return Halcyon::Result<void>::success();
}

void GpuSceneStorage::cleanup() noexcept
{
    pendingUploads_.clear();
    if (allocator_ != nullptr)
    {
        allocator_->destroy(transforms_);
        allocator_->destroy(bounds_);
        allocator_->destroy(meshMaterials_);
        allocator_->destroy(materials_);
    }
    transforms_ = {};
    bounds_ = {};
    meshMaterials_ = {};
    materials_ = {};
    device_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    capacity_ = 0;
}

Halcyon::Result<void> GpuSceneStorage::upload(
    const Halcyon::Renderer::Scene::GpuSceneSoA& scene)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState,
                "GPU scene storage is not initialized",
                "GpuSceneStorage"});
    }
    if (scene.transforms.size() > capacity_ || scene.bounds.size() > capacity_ ||
        scene.meshMaterials.size() > capacity_)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "GPU scene data exceeds allocated capacity",
                "GpuSceneStorage"});
    }
    const auto uploadOne = [&](BufferAllocation destination, const auto& values)
    {
        if (values.empty())
        {
            return Halcyon::Result<void>::success();
        }
        return queueUpload(destination, std::as_bytes(std::span{values}));
    };
    auto result = uploadOne(transforms_, scene.transforms);
    if (!result)
    {
        return result;
    }
    result = uploadOne(bounds_, scene.bounds);
    if (!result)
    {
        return result;
    }
    result = uploadOne(meshMaterials_, scene.meshMaterials);
    if (!result)
    {
        return result;
    }
    return uploadOne(materials_, scene.materials.materials);
}

Halcyon::Result<void> GpuSceneStorage::uploadDirty(
    const Halcyon::Renderer::Scene::GpuSceneSoA& scene,
    std::span<const Halcyon::Renderer::Scene::GpuSceneDirtyRange> ranges)
{
    if (device_ == VK_NULL_HANDLE || ranges.empty())
    {
        return Halcyon::Result<void>::success();
    }
    for (const auto& range : ranges)
    {
        if (range.empty() || range.first >= scene.transforms.size())
        {
            continue;
        }
        const std::uint32_t count = std::min<std::uint32_t>(
            range.count,
            static_cast<std::uint32_t>(scene.transforms.size() - range.first));
        const auto uploadRange = [&](BufferAllocation destination, const auto& values)
        {
            const auto bytes = std::as_bytes(std::span{values}.subspan(range.first, count));
            return queueUpload(
                destination,
                bytes,
                static_cast<VkDeviceSize>(range.first) * sizeof(values[0]));
        };
        auto result = uploadRange(transforms_, scene.transforms);
        if (!result)
        {
            return result;
        }
        result = uploadRange(bounds_, scene.bounds);
        if (!result)
        {
            return result;
        }
        result = uploadRange(meshMaterials_, scene.meshMaterials);
        if (!result)
        {
            return result;
        }
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> GpuSceneStorage::uploadMaterials(
    std::span<const Halcyon::Renderer::Scene::MaterialGpuData> materials)
{
    if (materials.empty())
    {
        return Halcyon::Result<void>::success();
    }
    if (materials.size() > capacity_)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "GPU material data exceeds allocated capacity",
                "GpuSceneStorage"});
    }
    return queueUpload(materials_, std::as_bytes(materials));
}

Halcyon::Result<void> GpuSceneStorage::queueUpload(
    BufferAllocation destination,
    std::span<const std::byte> bytes,
    VkDeviceSize destinationOffset)
{
    if (destination.buffer == VK_NULL_HANDLE || bytes.empty() ||
        destinationOffset > destination.size ||
        bytes.size_bytes() > destination.size - destinationOffset)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "invalid GPU scene upload range",
                "GpuSceneStorage"});
    }
    try
    {
        PendingUpload pending;
        pending.destination = destination.buffer;
        pending.destinationOffset = destinationOffset;
        pending.bytes.assign(bytes.begin(), bytes.end());
        pendingUploads_.push_back(std::move(pending));
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "failed to queue GPU scene upload",
                "GpuSceneStorage"});
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> GpuSceneStorage::recordPendingUploads(
    VkCommandBuffer commandBuffer,
    GpuAllocator& allocator,
    std::vector<BufferAllocation>& stagingKeepAlive)
{
    if (pendingUploads_.empty())
    {
        return Halcyon::Result<void>::success();
    }
    if (commandBuffer == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "GPU scene upload recording requires a command buffer",
                "GpuSceneStorage"});
    }

    auto alignUp = [](VkDeviceSize value, VkDeviceSize alignment) noexcept
    {
        return (value + alignment - 1u) & ~(alignment - 1u);
    };
    VkDeviceSize stagingSize = 0;
    for (const auto& pending : pendingUploads_)
    {
        stagingSize = alignUp(stagingSize, 16u) + pending.bytes.size();
    }
    if (stagingSize == 0)
    {
        pendingUploads_.clear();
        return Halcyon::Result<void>::success();
    }

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = stagingSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocation = allocator.createBuffer(info, MemoryUsage::CpuToGpu);
    if (!allocation)
    {
        return allocation.error();
    }
    const BufferAllocation staging = allocation.value();
    VkDeviceSize stagingOffset = 0;
    for (const auto& pending : pendingUploads_)
    {
        stagingOffset = alignUp(stagingOffset, 16u);
        const auto write = allocator.writeBuffer(
            staging,
            std::span<const std::byte>{pending.bytes.data(), pending.bytes.size()},
            stagingOffset);
        if (!write)
        {
            allocator.destroy(staging);
            return write;
        }
        VkBufferCopy copy{};
        copy.srcOffset = stagingOffset;
        copy.dstOffset = pending.destinationOffset;
        copy.size = pending.bytes.size();
        vkCmdCopyBuffer(commandBuffer, staging.buffer, pending.destination, 1, &copy);
        VkBufferMemoryBarrier2 ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        ready.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        ready.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ready.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ready.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ready.buffer = pending.destination;
        ready.offset = pending.destinationOffset;
        ready.size = pending.bytes.size();
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &ready;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        stagingOffset += pending.bytes.size();
    }
    try
    {
        stagingKeepAlive.push_back(staging);
    }
    catch (const std::bad_alloc&)
    {
        allocator.destroy(staging);
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "failed to retain GPU scene staging allocation",
                "GpuSceneStorage"});
    }
    pendingUploads_.clear();
    return Halcyon::Result<void>::success();
}

} // namespace Halcyon::Vulkan
