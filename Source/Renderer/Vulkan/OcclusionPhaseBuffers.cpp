#include "OcclusionPhaseBuffers.h"

#include "GpuSceneBufferUtil.h"

#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> OcclusionPhaseBuffers::initialize(
    GpuAllocator& allocator,
    std::uint32_t capacity,
    std::uint32_t frameCount,
    bool includeIndirect,
    const char* visibleName,
    const char* visibleCountName)
{
    allocator_ = &allocator;
    activeFrame_ = 0;
    auto visible = GpuSceneDetail::createPerFrame(
        allocator, visibleIndices_, frameCount, sizeof(std::uint32_t), capacity, visibleName);
    auto visibleCount = GpuSceneDetail::createPerFrame(
        allocator,
        visibleCount_,
        frameCount,
        sizeof(std::uint32_t),
        1,
        visibleCountName);
    if (!visible || !visibleCount)
    {
        cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to allocate occlusion phase buffers",
                "OcclusionPhaseBuffers"});
    }
    if (includeIndirect)
    {
        auto commands = GpuSceneDetail::createPerFrame(
            allocator,
            indirectCommands_,
            frameCount,
            sizeof(VkDrawIndexedIndirectCommand),
            capacity,
            "phase2 indirect commands");
        auto count = GpuSceneDetail::createPerFrame(
            allocator,
            indirectCount_,
            frameCount,
            sizeof(std::uint32_t),
            1,
            "phase2 indirect draw count");
        if (!commands || !count)
        {
            cleanup();
            return Halcyon::Result<void>::failure(
                {Halcyon::ErrorCode::Backend,
                    "failed to allocate occlusion phase indirect buffers",
                    "OcclusionPhaseBuffers"});
        }
    }
    return Halcyon::Result<void>::success();
}

void OcclusionPhaseBuffers::cleanup() noexcept
{
    GpuSceneDetail::destroyPerFrame(allocator_, visibleIndices_);
    GpuSceneDetail::destroyPerFrame(allocator_, visibleCount_);
    GpuSceneDetail::destroyPerFrame(allocator_, indirectCommands_);
    GpuSceneDetail::destroyPerFrame(allocator_, indirectCount_);
    allocator_ = nullptr;
    activeFrame_ = 0;
}

VkBuffer OcclusionPhaseBuffers::visibleIndicesBuffer() const noexcept
{
    return GpuSceneDetail::active(visibleIndices_, activeFrame_).buffer;
}

VkBuffer OcclusionPhaseBuffers::visibleCountBuffer() const noexcept
{
    return GpuSceneDetail::active(visibleCount_, activeFrame_).buffer;
}

VkBuffer OcclusionPhaseBuffers::indirectCommandsBuffer() const noexcept
{
    return GpuSceneDetail::active(indirectCommands_, activeFrame_).buffer;
}

VkBuffer OcclusionPhaseBuffers::indirectDrawCountBuffer() const noexcept
{
    return GpuSceneDetail::active(indirectCount_, activeFrame_).buffer;
}

} // namespace Halcyon::Vulkan
