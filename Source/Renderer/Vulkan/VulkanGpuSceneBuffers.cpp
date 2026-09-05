#include "VulkanGpuSceneBuffers.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace Halcyon::Vulkan
{

Halcyon::Result<BufferAllocation> VulkanGpuSceneBuffers::create(
    std::size_t stride, std::uint32_t count, const char* name)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(stride) * count;
    // GPU-driven scratch buffers are copied for first-frame phase seeding and
    // asynchronous counter readback. Keeping the persistent SoA allocations
    // copy-capable as well makes growth/debug readback use the same contract.
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (name != nullptr)
    {
        const std::string_view label{name};
        if (label.find("indirect") != std::string_view::npos ||
            label.find("count") != std::string_view::npos)
            info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    (void)name;
    return allocator_->createBuffer(info, MemoryUsage::GpuOnly);
}

Halcyon::Result<void> VulkanGpuSceneBuffers::initialize(
    VkDevice device, GpuAllocator& allocator, std::uint32_t capacity, std::uint32_t frameCount)
{
    if (device_ != VK_NULL_HANDLE)
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::AlreadyExists,
            "GPU scene buffers are already initialized", "VulkanGpuSceneBuffers"});
    device_ = device;
    allocator_ = &allocator;
    capacity_ = std::max(1u, capacity);
    frameCount_ = std::clamp(frameCount, 1u, 8u);
    activeFrame_ = 0;
    auto t = create(sizeof(Renderer::Scene::TransformRow), capacity_, "transforms");
    auto b = create(sizeof(Renderer::Scene::BoundsRow), capacity_, "bounds");
    auto m = create(sizeof(Renderer::Scene::MeshMaterialRow), capacity_, "mesh materials");
    auto material = create(sizeof(Renderer::Scene::MaterialGpuData), capacity_, "materials");
    auto makeOutputs = [&](std::vector<BufferAllocation>& output, std::size_t stride,
                           const char* name, std::uint32_t count)
    {
        output.reserve(frameCount_);
        for (std::uint32_t frame = 0; frame < frameCount_; ++frame)
        {
            auto allocation = create(stride, count, name);
            if (!allocation) return false;
            output.push_back(allocation.value());
        }
        return true;
    };
    const bool outputsCreated =
        makeOutputs(visibleIndices_, sizeof(std::uint32_t), "visible indices", capacity_) &&
        makeOutputs(indirectCommands_, sizeof(VkDrawIndexedIndirectCommand), "indirect commands", capacity_) &&
        makeOutputs(visibleCount_, sizeof(std::uint32_t), "visible count", 1) &&
        makeOutputs(phase1Visible_, sizeof(std::uint32_t), "phase1 visible indices", capacity_) &&
        makeOutputs(phase1VisibleCount_, sizeof(std::uint32_t), "phase1 visible count", 1) &&
        makeOutputs(occluded_, sizeof(std::uint32_t), "occluded indices", capacity_) &&
        makeOutputs(occludedCount_, sizeof(std::uint32_t), "occluded count", 1) &&
        makeOutputs(phase2Visible_, sizeof(std::uint32_t), "phase2 visible indices", capacity_) &&
        makeOutputs(phase2VisibleCount_, sizeof(std::uint32_t), "phase2 visible count", 1) &&
        makeOutputs(phase2Indirect_, sizeof(VkDrawIndexedIndirectCommand), "phase2 indirect commands", capacity_) &&
        makeOutputs(meshHeads_, sizeof(std::uint32_t), "mesh grouping heads", capacity_) &&
        makeOutputs(meshNext_, sizeof(std::uint32_t), "mesh grouping next", capacity_) &&
        makeOutputs(groupedVisible_, sizeof(std::uint32_t), "grouped visible indices", capacity_) &&
        makeOutputs(groupedCount_, sizeof(std::uint32_t), "grouped visible count", 1) &&
        makeOutputs(phase2GroupedVisible_, sizeof(std::uint32_t), "phase2 grouped visible indices", capacity_) &&
        makeOutputs(phase2GroupedCount_, sizeof(std::uint32_t), "phase2 grouped visible count", 1) &&
        makeOutputs(indirectCount_, sizeof(std::uint32_t), "indirect draw count", 1) &&
        makeOutputs(phase2IndirectCount_, sizeof(std::uint32_t), "phase2 indirect draw count", 1);
    if (!t || !b || !m || !material || !outputsCreated)
    {
        cleanup();
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
            "failed to allocate GPU scene buffers", "VulkanGpuSceneBuffers"});
    }
    transforms_ = t.value(); bounds_ = b.value(); meshMaterials_ = m.value(); materials_ = material.value();
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanGpuSceneBuffers::ensureCapacity(std::uint32_t required)
{
    if (required <= capacity_) return Halcyon::Result<void>::success();
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
            "GPU scene buffers are not initialized", "VulkanGpuSceneBuffers"});
    const std::uint32_t doubled = capacity_ > std::numeric_limits<std::uint32_t>::max() / 2u
        ? std::numeric_limits<std::uint32_t>::max()
        : capacity_ * 2u;
    const std::uint32_t next = std::max(required, doubled);
    VulkanGpuSceneBuffers replacement;
    auto result = replacement.initialize(device_, *allocator_, next, frameCount_);
    if (!result) return result;

    // Growth is rare. Waiting here keeps destruction of the old persistent
    // buffers correct even when another frame-in-flight still references them.
    // The caller performs a full re-upload when capacity changes.
    const VkResult waitResult = vkDeviceWaitIdle(device_);
    if (waitResult != VK_SUCCESS)
    {
        replacement.cleanup();
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
            "failed to wait for GPU scene buffer growth", "VulkanGpuSceneBuffers"});
    }
    cleanup();
    device_ = replacement.device_;
    allocator_ = replacement.allocator_;
    transforms_ = replacement.transforms_; bounds_ = replacement.bounds_;
    meshMaterials_ = replacement.meshMaterials_; materials_ = replacement.materials_;
    visibleIndices_ = std::move(replacement.visibleIndices_);
    indirectCommands_ = std::move(replacement.indirectCommands_);
    visibleCount_ = std::move(replacement.visibleCount_);
    phase1Visible_ = std::move(replacement.phase1Visible_);
    phase1VisibleCount_ = std::move(replacement.phase1VisibleCount_);
    occluded_ = std::move(replacement.occluded_);
    occludedCount_ = std::move(replacement.occludedCount_);
    phase2Visible_ = std::move(replacement.phase2Visible_);
    phase2VisibleCount_ = std::move(replacement.phase2VisibleCount_);
    phase2Indirect_ = std::move(replacement.phase2Indirect_);
    meshHeads_ = std::move(replacement.meshHeads_);
    meshNext_ = std::move(replacement.meshNext_);
    groupedVisible_ = std::move(replacement.groupedVisible_);
    groupedCount_ = std::move(replacement.groupedCount_);
    phase2GroupedVisible_ = std::move(replacement.phase2GroupedVisible_);
    phase2GroupedCount_ = std::move(replacement.phase2GroupedCount_);
    indirectCount_ = std::move(replacement.indirectCount_);
    phase2IndirectCount_ = std::move(replacement.phase2IndirectCount_);
    capacity_ = replacement.capacity_;
    frameCount_ = replacement.frameCount_;
    activeFrame_ = replacement.activeFrame_;
    replacement.transforms_ = {}; replacement.bounds_ = {}; replacement.meshMaterials_ = {};
    replacement.materials_ = {};
    replacement.meshHeads_ = {}; replacement.meshNext_ = {};
    replacement.groupedVisible_ = {}; replacement.groupedCount_ = {};
    replacement.phase2GroupedVisible_ = {}; replacement.phase2GroupedCount_ = {};
    replacement.indirectCount_ = {}; replacement.phase2IndirectCount_ = {};
    replacement.device_ = VK_NULL_HANDLE;
    replacement.allocator_ = nullptr;
    replacement.capacity_ = 0;
    replacement.frameCount_ = 0;
    replacement.activeFrame_ = 0;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanGpuSceneBuffers::upload(VkCommandPool commandPool, VkQueue queue,
    GpuUploader& uploader, const Halcyon::Renderer::Scene::GpuSceneSoA& scene)
{
    if (device_ == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
            "GPU scene upload requires an initialized device and queue", "VulkanGpuSceneBuffers"});
    if (scene.transforms.size() > capacity_ || scene.bounds.size() > capacity_ ||
        scene.meshMaterials.size() > capacity_)
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "GPU scene data exceeds allocated capacity", "VulkanGpuSceneBuffers"});
    (void)commandPool;
    (void)queue;
    (void)uploader;
    const auto uploadOne = [&](BufferAllocation destination, const auto& values)
    {
        if (values.empty()) return Halcyon::Result<void>::success();
        const auto bytes = std::as_bytes(std::span{values});
        return queueUpload(destination, bytes);
    };
    auto result = uploadOne(transforms_, scene.transforms);
    if (!result) return result;
    result = uploadOne(bounds_, scene.bounds);
    if (!result) return result;
    result = uploadOne(meshMaterials_, scene.meshMaterials);
    if (!result) return result;
    return uploadOne(materials_, scene.materials.materials);
}

Halcyon::Result<void> VulkanGpuSceneBuffers::uploadDirty(VkCommandPool commandPool, VkQueue queue,
    GpuUploader& uploader, const Halcyon::Renderer::Scene::GpuSceneSoA& scene,
    std::span<const Halcyon::Renderer::Scene::GpuSceneDirtyRange> ranges)
{
    if (device_ == VK_NULL_HANDLE || ranges.empty())
        return Halcyon::Result<void>::success();
    (void)commandPool;
    (void)queue;
    (void)uploader;
    for (const auto& range : ranges)
    {
        if (range.empty() || range.first >= scene.transforms.size()) continue;
        const std::uint32_t count = std::min<std::uint32_t>(range.count,
            static_cast<std::uint32_t>(scene.transforms.size() - range.first));
        const auto uploadRange = [&](BufferAllocation destination, const auto& values)
        {
            const auto bytes = std::as_bytes(std::span{values}.subspan(range.first, count));
            return queueUpload(destination, bytes,
                static_cast<VkDeviceSize>(range.first) * sizeof(values[0]));
        };
        auto result = uploadRange(transforms_, scene.transforms);
        if (!result) return result;
        result = uploadRange(bounds_, scene.bounds);
        if (!result) return result;
        result = uploadRange(meshMaterials_, scene.meshMaterials);
        if (!result) return result;
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanGpuSceneBuffers::uploadMaterials(VkCommandPool commandPool,
    VkQueue queue, GpuUploader& uploader,
    std::span<const Halcyon::Renderer::Scene::MaterialGpuData> materials)
{
    if (materials.empty()) return Halcyon::Result<void>::success();
    if (materials.size() > capacity_)
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "GPU material data exceeds allocated capacity", "VulkanGpuSceneBuffers"});
    (void)commandPool;
    (void)queue;
    (void)uploader;
    return queueUpload(materials_, std::as_bytes(materials));
}

Halcyon::Result<void> VulkanGpuSceneBuffers::queueUpload(
    BufferAllocation destination,
    std::span<const std::byte> bytes,
    VkDeviceSize destinationOffset)
{
    if (destination.buffer == VK_NULL_HANDLE || bytes.empty() ||
        destinationOffset > destination.size || bytes.size_bytes() > destination.size - destinationOffset)
    {
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "invalid GPU scene upload range", "VulkanGpuSceneBuffers"});
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
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "failed to queue GPU scene upload", "VulkanGpuSceneBuffers"});
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanGpuSceneBuffers::recordPendingUploads(
    VkCommandBuffer commandBuffer,
    GpuAllocator& allocator,
    std::vector<BufferAllocation>& stagingKeepAlive)
{
    if (pendingUploads_.empty()) return Halcyon::Result<void>::success();
    if (commandBuffer == VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "GPU scene upload recording requires a command buffer", "VulkanGpuSceneBuffers"});
    }

    auto alignUp = [](VkDeviceSize value, VkDeviceSize alignment) noexcept
    {
        return (value + alignment - 1u) & ~(alignment - 1u);
    };
    VkDeviceSize stagingSize = 0;
    for (const auto& pending : pendingUploads_)
        stagingSize = alignUp(stagingSize, 16u) + pending.bytes.size();
    if (stagingSize == 0) {
        pendingUploads_.clear();
        return Halcyon::Result<void>::success();
    }

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = stagingSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocation = allocator.createBuffer(info, MemoryUsage::CpuToGpu);
    if (!allocation) return allocation.error();
    const BufferAllocation staging = allocation.value();
    VkDeviceSize stagingOffset = 0;
    for (const auto& pending : pendingUploads_)
    {
        stagingOffset = alignUp(stagingOffset, 16u);
        const auto write = allocator.writeBuffer(staging,
            std::span<const std::byte>{pending.bytes.data(), pending.bytes.size()}, stagingOffset);
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
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "failed to retain GPU scene staging allocation", "VulkanGpuSceneBuffers"});
    }
    pendingUploads_.clear();
    return Halcyon::Result<void>::success();
}

void VulkanGpuSceneBuffers::cleanup() noexcept
{
    pendingUploads_.clear();
    if (allocator_ != nullptr)
    {
        allocator_->destroy(transforms_); allocator_->destroy(bounds_); allocator_->destroy(meshMaterials_);
        allocator_->destroy(materials_);
        const auto destroyAll = [&](std::vector<BufferAllocation>& buffers)
        {
            for (auto& buffer : buffers) allocator_->destroy(buffer);
            buffers.clear();
        };
        destroyAll(visibleIndices_); destroyAll(indirectCommands_); destroyAll(visibleCount_);
        destroyAll(phase1Visible_); destroyAll(phase1VisibleCount_);
        destroyAll(occluded_); destroyAll(occludedCount_);
        destroyAll(phase2Visible_); destroyAll(phase2VisibleCount_);
        destroyAll(phase2Indirect_); destroyAll(meshHeads_); destroyAll(meshNext_);
        destroyAll(groupedVisible_); destroyAll(groupedCount_);
        destroyAll(phase2GroupedVisible_); destroyAll(phase2GroupedCount_);
        destroyAll(indirectCount_); destroyAll(phase2IndirectCount_);
    }
    transforms_ = {}; bounds_ = {}; meshMaterials_ = {}; materials_ = {}; visibleIndices_ = {};
    visibleIndices_.clear(); indirectCommands_.clear(); visibleCount_.clear(); capacity_ = 0; device_ = VK_NULL_HANDLE;
    allocator_ = nullptr; frameCount_ = 0; activeFrame_ = 0;
    phase1Visible_.clear(); phase1VisibleCount_.clear(); occluded_.clear(); occludedCount_.clear();
    phase2Visible_.clear(); phase2VisibleCount_.clear(); phase2Indirect_.clear();
    meshHeads_.clear(); meshNext_.clear(); groupedVisible_.clear(); groupedCount_.clear();
    phase2GroupedVisible_.clear(); phase2GroupedCount_.clear();
    indirectCount_.clear(); phase2IndirectCount_.clear();
}

} // namespace Halcyon::Vulkan
