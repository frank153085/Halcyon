#include "FrustumCullBuffers.h"

#include "GpuSceneBufferUtil.h"

#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> FrustumCullBuffers::initialize(
    GpuAllocator& allocator,
    std::uint32_t capacity,
    std::uint32_t frameCount)
{
    allocator_ = &allocator;
    activeFrame_ = 0;
    const bool ok =
        GpuSceneDetail::createPerFrame(
            allocator, visibleIndices_, frameCount, sizeof(std::uint32_t), capacity,
            "visible indices") &&
        GpuSceneDetail::createPerFrame(
            allocator, visibleCount_, frameCount, sizeof(std::uint32_t), 1, "visible count") &&
        GpuSceneDetail::createPerFrame(
            allocator, occludedIndices_, frameCount, sizeof(std::uint32_t), capacity,
            "occluded indices") &&
        GpuSceneDetail::createPerFrame(
            allocator, occludedCount_, frameCount, sizeof(std::uint32_t), 1, "occluded count") &&
        GpuSceneDetail::createPerFrame(
            allocator,
            indirectCommands_,
            frameCount,
            sizeof(VkDrawIndexedIndirectCommand),
            capacity,
            "indirect commands") &&
        GpuSceneDetail::createPerFrame(
            allocator, indirectCount_, frameCount, sizeof(std::uint32_t), 1,
            "indirect draw count");
    if (!ok)
    {
        cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to allocate frustum cull buffers",
                "FrustumCullBuffers"});
    }
    return Halcyon::Result<void>::success();
}

void FrustumCullBuffers::cleanup() noexcept
{
    GpuSceneDetail::destroyPerFrame(allocator_, visibleIndices_);
    GpuSceneDetail::destroyPerFrame(allocator_, visibleCount_);
    GpuSceneDetail::destroyPerFrame(allocator_, occludedIndices_);
    GpuSceneDetail::destroyPerFrame(allocator_, occludedCount_);
    GpuSceneDetail::destroyPerFrame(allocator_, indirectCommands_);
    GpuSceneDetail::destroyPerFrame(allocator_, indirectCount_);
    allocator_ = nullptr;
    activeFrame_ = 0;
}

VkBuffer FrustumCullBuffers::visibleIndicesBuffer() const noexcept
{
    return GpuSceneDetail::active(visibleIndices_, activeFrame_).buffer;
}

VkBuffer FrustumCullBuffers::visibleCountBuffer() const noexcept
{
    return GpuSceneDetail::active(visibleCount_, activeFrame_).buffer;
}

VkBuffer FrustumCullBuffers::occludedIndicesBuffer() const noexcept
{
    return GpuSceneDetail::active(occludedIndices_, activeFrame_).buffer;
}

VkBuffer FrustumCullBuffers::occludedCountBuffer() const noexcept
{
    return GpuSceneDetail::active(occludedCount_, activeFrame_).buffer;
}

VkBuffer FrustumCullBuffers::indirectCommandsBuffer() const noexcept
{
    return GpuSceneDetail::active(indirectCommands_, activeFrame_).buffer;
}

VkBuffer FrustumCullBuffers::indirectDrawCountBuffer() const noexcept
{
    return GpuSceneDetail::active(indirectCount_, activeFrame_).buffer;
}

} // namespace Halcyon::Vulkan
