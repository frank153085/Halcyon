#pragma once

#include "../Scene/GpuScene.h"
#include "GpuAllocator.h"

#include <cstddef>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Persistent device-local SoA storage for GPU-driven transforms, bounds,
// mesh/material rows, and material GPU data. Uploads are queued and later
// recorded into the current frame command buffer.
class GpuSceneStorage final
{
public:
    GpuSceneStorage() = default;
    GpuSceneStorage(const GpuSceneStorage&) = delete;
    GpuSceneStorage& operator=(const GpuSceneStorage&) = delete;
    GpuSceneStorage(GpuSceneStorage&&) noexcept = default;
    GpuSceneStorage& operator=(GpuSceneStorage&&) noexcept = default;

    [[nodiscard]] Halcyon::Result<void> initialize(
        VkDevice device,
        GpuAllocator& allocator,
        std::uint32_t capacity);
    void cleanup() noexcept;

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

    [[nodiscard]] VkBuffer transformBuffer() const noexcept
    {
        return transforms_.buffer;
    }
    [[nodiscard]] VkBuffer boundsBuffer() const noexcept
    {
        return bounds_.buffer;
    }
    [[nodiscard]] VkBuffer meshMaterialBuffer() const noexcept
    {
        return meshMaterials_.buffer;
    }
    [[nodiscard]] VkBuffer materialBuffer() const noexcept
    {
        return materials_.buffer;
    }
    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    [[nodiscard]] Halcyon::Result<void> queueUpload(
        BufferAllocation destination,
        std::span<const std::byte> bytes,
        VkDeviceSize destinationOffset = 0);

    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    BufferAllocation transforms_{};
    BufferAllocation bounds_{};
    BufferAllocation meshMaterials_{};
    BufferAllocation materials_{};
    struct PendingUpload
    {
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize destinationOffset = 0;
        std::vector<std::byte> bytes;
    };
    std::vector<PendingUpload> pendingUploads_;
    std::uint32_t capacity_ = 0;
};

} // namespace Halcyon::Vulkan
