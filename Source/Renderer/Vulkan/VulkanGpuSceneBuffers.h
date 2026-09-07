#pragma once

#include "../Scene/GpuScene.h"
#include "FrustumCullBuffers.h"
#include "GpuAllocator.h"
#include "GpuSceneStorage.h"
#include "IndirectBuildDescriptorLayout.h"
#include "MeshGroupedIndirectBuffers.h"
#include "OcclusionPhaseBuffers.h"

#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Composition root for GPU-driven scene buffers. Persistent SoA, frustum
// outputs, occlusion phases, and mesh grouping are separate objects; this
// facade still exposes the aggregate getters used by recording code.
class VulkanGpuSceneBuffers final
{
public:
    VulkanGpuSceneBuffers() = default;
    VulkanGpuSceneBuffers(const VulkanGpuSceneBuffers&) = delete;
    VulkanGpuSceneBuffers& operator=(const VulkanGpuSceneBuffers&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(
        VkDevice device,
        GpuAllocator& allocator,
        std::uint32_t capacity = 1024,
        std::uint32_t frameCount = 3);
    void setFrameIndex(std::uint32_t frameIndex) noexcept;
    void cleanup() noexcept;
    [[nodiscard]] Halcyon::Result<void> ensureCapacity(std::uint32_t required);
    [[nodiscard]] Halcyon::Result<void> upload(
        const Halcyon::Renderer::Scene::GpuSceneSoA& scene);
    [[nodiscard]] Halcyon::Result<void> uploadDirty(
        const Halcyon::Renderer::Scene::GpuSceneSoA& scene,
        std::span<const Halcyon::Renderer::Scene::GpuSceneDirtyRange> ranges);
    [[nodiscard]] Halcyon::Result<void> uploadMaterials(
        std::span<const Halcyon::Renderer::Scene::MaterialGpuData> materials);
    [[nodiscard]] Halcyon::Result<void> recordPendingUploads(
        VkCommandBuffer commandBuffer,
        GpuAllocator& allocator,
        std::vector<BufferAllocation>& stagingKeepAlive);

    void writeIndirectBuildDescriptors(
        VkDevice device,
        VkDescriptorSet set,
        IndirectBuildPass pass,
        VkBuffer meshDrawBuffer,
        VkDeviceSize meshDrawSize) const;

    [[nodiscard]] GpuSceneStorage& storage() noexcept
    {
        return storage_;
    }
    [[nodiscard]] const GpuSceneStorage& storage() const noexcept
    {
        return storage_;
    }
    [[nodiscard]] FrustumCullBuffers& frustum() noexcept
    {
        return frustum_;
    }
    [[nodiscard]] const FrustumCullBuffers& frustum() const noexcept
    {
        return frustum_;
    }
    [[nodiscard]] OcclusionPhaseBuffers& phase1() noexcept
    {
        return phase1_;
    }
    [[nodiscard]] const OcclusionPhaseBuffers& phase1() const noexcept
    {
        return phase1_;
    }
    [[nodiscard]] OcclusionPhaseBuffers& phase2() noexcept
    {
        return phase2_;
    }
    [[nodiscard]] const OcclusionPhaseBuffers& phase2() const noexcept
    {
        return phase2_;
    }
    [[nodiscard]] MeshGroupedIndirectBuffers& grouping() noexcept
    {
        return grouping_;
    }
    [[nodiscard]] const MeshGroupedIndirectBuffers& grouping() const noexcept
    {
        return grouping_;
    }
    [[nodiscard]] MeshGroupedIndirectBuffers& phase2Grouping() noexcept
    {
        return phase2Grouping_;
    }
    [[nodiscard]] const MeshGroupedIndirectBuffers& phase2Grouping() const noexcept
    {
        return phase2Grouping_;
    }

    [[nodiscard]] VkBuffer transformBuffer() const noexcept
    {
        return storage_.transformBuffer();
    }
    [[nodiscard]] VkBuffer boundsBuffer() const noexcept
    {
        return storage_.boundsBuffer();
    }
    [[nodiscard]] VkBuffer meshMaterialBuffer() const noexcept
    {
        return storage_.meshMaterialBuffer();
    }
    [[nodiscard]] VkBuffer materialBuffer() const noexcept
    {
        return storage_.materialBuffer();
    }
    [[nodiscard]] VkBuffer visibleIndicesBuffer() const noexcept
    {
        return frustum_.visibleIndicesBuffer();
    }
    [[nodiscard]] VkBuffer visibleCountBuffer() const noexcept
    {
        return frustum_.visibleCountBuffer();
    }
    [[nodiscard]] VkBuffer occludedIndicesBuffer() const noexcept
    {
        return frustum_.occludedIndicesBuffer();
    }
    [[nodiscard]] VkBuffer occludedCountBuffer() const noexcept
    {
        return frustum_.occludedCountBuffer();
    }
    [[nodiscard]] VkBuffer indirectCommandsBuffer() const noexcept
    {
        return frustum_.indirectCommandsBuffer();
    }
    // Phase 1 reuses the main-path count buffer.
    [[nodiscard]] VkBuffer indirectDrawCountBuffer() const noexcept
    {
        return frustum_.indirectDrawCountBuffer();
    }
    [[nodiscard]] VkBuffer phase1VisibleIndicesBuffer() const noexcept
    {
        return phase1_.visibleIndicesBuffer();
    }
    [[nodiscard]] VkBuffer phase1VisibleCountBuffer() const noexcept
    {
        return phase1_.visibleCountBuffer();
    }
    [[nodiscard]] VkBuffer phase2VisibleIndicesBuffer() const noexcept
    {
        return phase2_.visibleIndicesBuffer();
    }
    [[nodiscard]] VkBuffer phase2VisibleCountBuffer() const noexcept
    {
        return phase2_.visibleCountBuffer();
    }
    [[nodiscard]] VkBuffer phase2IndirectCommandsBuffer() const noexcept
    {
        return phase2_.indirectCommandsBuffer();
    }
    [[nodiscard]] VkBuffer phase2IndirectDrawCountBuffer() const noexcept
    {
        return phase2_.indirectDrawCountBuffer();
    }
    [[nodiscard]] VkBuffer meshHeadsBuffer() const noexcept
    {
        return grouping_.meshHeadsBuffer();
    }
    [[nodiscard]] VkBuffer meshNextBuffer() const noexcept
    {
        return grouping_.meshNextBuffer();
    }
    [[nodiscard]] VkBuffer groupedVisibleIndicesBuffer() const noexcept
    {
        return grouping_.groupedVisibleIndicesBuffer();
    }
    [[nodiscard]] VkBuffer groupedVisibleCountBuffer() const noexcept
    {
        return grouping_.groupedVisibleCountBuffer();
    }
    [[nodiscard]] VkBuffer phase2GroupedVisibleIndicesBuffer() const noexcept
    {
        return phase2Grouping_.groupedVisibleIndicesBuffer();
    }
    [[nodiscard]] VkBuffer phase2GroupedVisibleCountBuffer() const noexcept
    {
        return phase2Grouping_.groupedVisibleCountBuffer();
    }
    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    GpuSceneStorage storage_;
    FrustumCullBuffers frustum_;
    OcclusionPhaseBuffers phase1_;
    OcclusionPhaseBuffers phase2_;
    MeshGroupedIndirectBuffers grouping_;
    MeshGroupedIndirectBuffers phase2Grouping_;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    std::uint32_t capacity_ = 0;
    std::uint32_t frameCount_ = 0;
    std::uint32_t activeFrame_ = 0;
};

} // namespace Halcyon::Vulkan
