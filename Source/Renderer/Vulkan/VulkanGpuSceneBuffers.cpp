#include "VulkanGpuSceneBuffers.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Halcyon::Vulkan
{
namespace
{

void writeStorageBuffer(
    VkDevice device,
    VkDescriptorSet set,
    std::uint32_t binding,
    VkBuffer buffer,
    VkDeviceSize size)
{
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

} // namespace

Halcyon::Result<void> VulkanGpuSceneBuffers::initialize(
    VkDevice device,
    GpuAllocator& allocator,
    std::uint32_t capacity,
    std::uint32_t frameCount)
{
    if (device_ != VK_NULL_HANDLE)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::AlreadyExists,
                "GPU scene buffers are already initialized",
                "VulkanGpuSceneBuffers"});
    }
    device_ = device;
    allocator_ = &allocator;
    capacity_ = std::max(1u, capacity);
    frameCount_ = std::clamp(frameCount, 1u, 8u);
    activeFrame_ = 0;
    auto storage = storage_.initialize(device, allocator, capacity_);
    auto frustum = frustum_.initialize(allocator, capacity_, frameCount_);
    auto phase1 = phase1_.initialize(
        allocator,
        capacity_,
        frameCount_,
        false,
        "phase1 visible indices",
        "phase1 visible count");
    auto phase2 = phase2_.initialize(
        allocator,
        capacity_,
        frameCount_,
        true,
        "phase2 visible indices",
        "phase2 visible count");
    auto grouping = grouping_.initialize(
        allocator,
        capacity_,
        frameCount_,
        true,
        "grouped visible indices",
        "grouped visible count");
    auto phase2Grouping = phase2Grouping_.initialize(
        allocator,
        capacity_,
        frameCount_,
        false,
        "phase2 grouped visible indices",
        "phase2 grouped visible count");
    auto shadowFrustum = shadowFrustum_.initialize(allocator, capacity_, frameCount_);
    auto shadowGrouping = shadowGrouping_.initialize(
        allocator,
        capacity_,
        frameCount_,
        true,
        "shadow grouped visible indices",
        "shadow grouped visible count");
    if (!storage || !frustum || !phase1 || !phase2 || !grouping || !phase2Grouping ||
        !shadowFrustum || !shadowGrouping)
    {
        cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to allocate GPU scene buffers",
                "VulkanGpuSceneBuffers"});
    }
    return Halcyon::Result<void>::success();
}

void VulkanGpuSceneBuffers::setFrameIndex(std::uint32_t frameIndex) noexcept
{
    activeFrame_ = frameCount_ == 0 ? 0 : frameIndex % frameCount_;
    frustum_.setFrameIndex(activeFrame_);
    phase1_.setFrameIndex(activeFrame_);
    phase2_.setFrameIndex(activeFrame_);
    grouping_.setFrameIndex(activeFrame_);
    phase2Grouping_.setFrameIndex(activeFrame_);
    shadowFrustum_.setFrameIndex(activeFrame_);
    shadowGrouping_.setFrameIndex(activeFrame_);
}

void VulkanGpuSceneBuffers::cleanup() noexcept
{
    storage_.cleanup();
    frustum_.cleanup();
    phase1_.cleanup();
    phase2_.cleanup();
    grouping_.cleanup();
    phase2Grouping_.cleanup();
    shadowFrustum_.cleanup();
    shadowGrouping_.cleanup();
    device_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    capacity_ = 0;
    frameCount_ = 0;
    activeFrame_ = 0;
}

Halcyon::Result<void> VulkanGpuSceneBuffers::ensureCapacity(std::uint32_t required)
{
    if (required <= capacity_)
    {
        return Halcyon::Result<void>::success();
    }
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState,
                "GPU scene buffers are not initialized",
                "VulkanGpuSceneBuffers"});
    }
    const std::uint32_t doubled = capacity_ > std::numeric_limits<std::uint32_t>::max() / 2u
        ? std::numeric_limits<std::uint32_t>::max()
        : capacity_ * 2u;
    const std::uint32_t next = std::max(required, doubled);
    VulkanGpuSceneBuffers replacement;
    auto result = replacement.initialize(device_, *allocator_, next, frameCount_);
    if (!result)
    {
        return result;
    }

    const VkResult waitResult = vkDeviceWaitIdle(device_);
    if (waitResult != VK_SUCCESS)
    {
        replacement.cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to wait for GPU scene buffer growth",
                "VulkanGpuSceneBuffers"});
    }
    cleanup();
    device_ = replacement.device_;
    allocator_ = replacement.allocator_;
    capacity_ = replacement.capacity_;
    frameCount_ = replacement.frameCount_;
    activeFrame_ = replacement.activeFrame_;
    storage_ = std::move(replacement.storage_);
    frustum_ = std::move(replacement.frustum_);
    phase1_ = std::move(replacement.phase1_);
    phase2_ = std::move(replacement.phase2_);
    grouping_ = std::move(replacement.grouping_);
    phase2Grouping_ = std::move(replacement.phase2Grouping_);
    shadowFrustum_ = std::move(replacement.shadowFrustum_);
    shadowGrouping_ = std::move(replacement.shadowGrouping_);
    replacement.device_ = VK_NULL_HANDLE;
    replacement.allocator_ = nullptr;
    replacement.capacity_ = 0;
    replacement.frameCount_ = 0;
    replacement.activeFrame_ = 0;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanGpuSceneBuffers::upload(
    const Halcyon::Renderer::Scene::GpuSceneSoA& scene)
{
    return storage_.upload(scene);
}

Halcyon::Result<void> VulkanGpuSceneBuffers::uploadDirty(
    const Halcyon::Renderer::Scene::GpuSceneSoA& scene,
    std::span<const Halcyon::Renderer::Scene::GpuSceneDirtyRange> ranges)
{
    return storage_.uploadDirty(scene, ranges);
}

Halcyon::Result<void> VulkanGpuSceneBuffers::uploadMaterials(
    std::span<const Halcyon::Renderer::Scene::MaterialGpuData> materials)
{
    return storage_.uploadMaterials(materials);
}

Halcyon::Result<void> VulkanGpuSceneBuffers::recordPendingUploads(
    VkCommandBuffer commandBuffer,
    GpuAllocator& allocator,
    std::vector<BufferAllocation>& stagingKeepAlive)
{
    return storage_.recordPendingUploads(commandBuffer, allocator, stagingKeepAlive);
}

void VulkanGpuSceneBuffers::writeIndirectBuildDescriptors(
    VkDevice device,
    VkDescriptorSet set,
    IndirectBuildPass pass,
    VkBuffer meshDrawBuffer,
    VkDeviceSize meshDrawSize) const
{
    if (device == VK_NULL_HANDLE || set == VK_NULL_HANDLE)
    {
        return;
    }
    const VkDeviceSize indexBytes = std::max<VkDeviceSize>(
        4, static_cast<VkDeviceSize>(capacity_) * sizeof(std::uint32_t));
    const VkDeviceSize materialBytes = std::max<VkDeviceSize>(
        4,
        static_cast<VkDeviceSize>(capacity_) *
            sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
    const VkDeviceSize commandBytes = std::max<VkDeviceSize>(
        4, static_cast<VkDeviceSize>(capacity_) * sizeof(VkDrawIndexedIndirectCommand));

    IndirectBuildBuffers buffers;
    buffers.meshMaterials = meshMaterialBuffer();
    buffers.meshMaterialsSize = materialBytes;
    buffers.meshDraws = meshDrawBuffer;
    buffers.meshDrawsSize = meshDrawSize;
    buffers.meshHeads = meshHeadsBuffer();
    buffers.meshHeadsSize = indexBytes;
    buffers.meshNext = meshNextBuffer();
    buffers.meshNextSize = indexBytes;

    switch (pass)
    {
        case IndirectBuildPass::Main:
            buffers.visibleIndices = visibleIndicesBuffer();
            buffers.visibleIndicesSize = indexBytes;
            buffers.indirectCommands = indirectCommandsBuffer();
            buffers.indirectCommandsSize = commandBytes;
            buffers.indirectCount = indirectDrawCountBuffer();
            buffers.groupedVisible = groupedVisibleIndicesBuffer();
            buffers.groupedVisibleSize = indexBytes;
            buffers.groupedCount = groupedVisibleCountBuffer();
            buffers.visibleCount = visibleCountBuffer();
            break;
        case IndirectBuildPass::Phase1:
            buffers.visibleIndices = phase1VisibleIndicesBuffer();
            buffers.visibleIndicesSize = indexBytes;
            buffers.indirectCommands = indirectCommandsBuffer();
            buffers.indirectCommandsSize = commandBytes;
            buffers.indirectCount = indirectDrawCountBuffer();
            buffers.groupedVisible = groupedVisibleIndicesBuffer();
            buffers.groupedVisibleSize = indexBytes;
            buffers.groupedCount = groupedVisibleCountBuffer();
            buffers.visibleCount = phase1VisibleCountBuffer();
            break;
        case IndirectBuildPass::Phase2:
            buffers.visibleIndices = phase2VisibleIndicesBuffer();
            buffers.visibleIndicesSize = indexBytes;
            buffers.indirectCommands = phase2IndirectCommandsBuffer();
            buffers.indirectCommandsSize = commandBytes;
            buffers.indirectCount = phase2IndirectDrawCountBuffer();
            buffers.groupedVisible = phase2GroupedVisibleIndicesBuffer();
            buffers.groupedVisibleSize = indexBytes;
            buffers.groupedCount = phase2GroupedVisibleCountBuffer();
            buffers.visibleCount = phase2VisibleCountBuffer();
            break;
        case IndirectBuildPass::Shadow:
            buffers.visibleIndices = shadowVisibleIndicesBuffer();
            buffers.visibleIndicesSize = indexBytes;
            buffers.indirectCommands = shadowIndirectCommandsBuffer();
            buffers.indirectCommandsSize = commandBytes;
            buffers.indirectCount = shadowIndirectDrawCountBuffer();
            buffers.groupedVisible = shadowGroupedVisibleIndicesBuffer();
            buffers.groupedVisibleSize = indexBytes;
            buffers.groupedCount = shadowGroupedVisibleCountBuffer();
            buffers.visibleCount = shadowVisibleCountBuffer();
            buffers.meshHeads = shadowMeshHeadsBuffer();
            buffers.meshHeadsSize = indexBytes;
            buffers.meshNext = shadowMeshNextBuffer();
            buffers.meshNextSize = indexBytes;
            break;
    }

    writeStorageBuffer(
        device, set, 0, buffers.visibleIndices, buffers.visibleIndicesSize);
    writeStorageBuffer(
        device, set, 1, buffers.meshMaterials, buffers.meshMaterialsSize);
    writeStorageBuffer(
        device, set, 2, buffers.indirectCommands, buffers.indirectCommandsSize);
    writeStorageBuffer(device, set, 3, buffers.indirectCount, sizeof(std::uint32_t));
    writeStorageBuffer(device, set, 4, buffers.meshDraws, buffers.meshDrawsSize);
    writeStorageBuffer(device, set, 5, buffers.meshHeads, buffers.meshHeadsSize);
    writeStorageBuffer(device, set, 6, buffers.meshNext, buffers.meshNextSize);
    writeStorageBuffer(
        device, set, 7, buffers.groupedVisible, buffers.groupedVisibleSize);
    writeStorageBuffer(device, set, 8, buffers.groupedCount, sizeof(std::uint32_t));
    writeStorageBuffer(device, set, 9, buffers.visibleCount, sizeof(std::uint32_t));
}

} // namespace Halcyon::Vulkan
