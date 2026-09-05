#pragma once

#include "../Scene/GpuScene.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"

#include <vulkan/vulkan.h>
#include <algorithm>
#include <vector>

namespace Halcyon::Vulkan
{

// Persistent device-local SoA buffers used by the GPU-driven path. Uploads
// are deliberately explicit so callers can batch dirty ranges per frame.
class VulkanGpuSceneBuffers final
{
public:
    VulkanGpuSceneBuffers() = default;
    VulkanGpuSceneBuffers(const VulkanGpuSceneBuffers&) = delete;
    VulkanGpuSceneBuffers& operator=(const VulkanGpuSceneBuffers&) = delete;

    [[nodiscard]] Halcyon::Result<void> initialize(VkDevice device, GpuAllocator& allocator,
        std::uint32_t capacity = 1024, std::uint32_t frameCount = 3);
    void setFrameIndex(std::uint32_t frameIndex) noexcept
    {
        activeFrame_ = frameCount_ == 0 ? 0 : frameIndex % frameCount_;
    }
    void cleanup() noexcept;
    [[nodiscard]] Halcyon::Result<void> ensureCapacity(std::uint32_t required);
    [[nodiscard]] Halcyon::Result<void> upload(VkCommandPool commandPool, VkQueue queue,
        GpuUploader& uploader, const Halcyon::Renderer::Scene::GpuSceneSoA& scene);
    [[nodiscard]] Halcyon::Result<void> uploadDirty(VkCommandPool commandPool, VkQueue queue,
        GpuUploader& uploader, const Halcyon::Renderer::Scene::GpuSceneSoA& scene,
        std::span<const Halcyon::Renderer::Scene::GpuSceneDirtyRange> ranges);
    [[nodiscard]] Halcyon::Result<void> uploadMaterials(VkCommandPool commandPool, VkQueue queue,
        GpuUploader& uploader,
        std::span<const Halcyon::Renderer::Scene::MaterialGpuData> materials);

    [[nodiscard]] VkBuffer transformBuffer() const noexcept { return transforms_.buffer; }
    [[nodiscard]] VkBuffer boundsBuffer() const noexcept { return bounds_.buffer; }
    [[nodiscard]] VkBuffer meshMaterialBuffer() const noexcept { return meshMaterials_.buffer; }
    [[nodiscard]] VkBuffer visibleIndicesBuffer() const noexcept { return active(visibleIndices_).buffer; }
    [[nodiscard]] VkBuffer indirectCommandsBuffer() const noexcept { return active(indirectCommands_).buffer; }
    [[nodiscard]] VkBuffer visibleCountBuffer() const noexcept { return active(visibleCount_).buffer; }
    [[nodiscard]] VkBuffer phase1VisibleIndicesBuffer() const noexcept { return active(phase1Visible_).buffer; }
    [[nodiscard]] VkBuffer phase1VisibleCountBuffer() const noexcept { return active(phase1VisibleCount_).buffer; }
    [[nodiscard]] VkBuffer occludedIndicesBuffer() const noexcept { return active(occluded_).buffer; }
    [[nodiscard]] VkBuffer occludedCountBuffer() const noexcept { return active(occludedCount_).buffer; }
    [[nodiscard]] VkBuffer phase2VisibleIndicesBuffer() const noexcept { return active(phase2Visible_).buffer; }
    [[nodiscard]] VkBuffer phase2VisibleCountBuffer() const noexcept { return active(phase2VisibleCount_).buffer; }
    [[nodiscard]] VkBuffer phase2IndirectCommandsBuffer() const noexcept { return active(phase2Indirect_).buffer; }
    [[nodiscard]] VkBuffer materialBuffer() const noexcept { return materials_.buffer; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] const BufferAllocation& active(const std::vector<BufferAllocation>& buffers) const noexcept
    {
        static const BufferAllocation empty{};
        return buffers.empty() ? empty : buffers[std::min<std::size_t>(activeFrame_, buffers.size() - 1u)];
    }
    [[nodiscard]] Halcyon::Result<BufferAllocation> create(std::size_t stride, std::uint32_t count,
        const char* name);
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    BufferAllocation transforms_{}, bounds_{}, meshMaterials_{}, materials_{};
    std::vector<BufferAllocation> visibleIndices_, indirectCommands_, visibleCount_;
    std::vector<BufferAllocation> phase1Visible_, phase1VisibleCount_, occluded_, occludedCount_;
    std::vector<BufferAllocation> phase2Visible_, phase2VisibleCount_, phase2Indirect_;
    std::uint32_t capacity_ = 0;
    std::uint32_t frameCount_ = 0;
    std::uint32_t activeFrame_ = 0;
};

} // namespace Halcyon::Vulkan
