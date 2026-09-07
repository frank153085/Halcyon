#include "MeshGroupedIndirectBuffers.h"

#include "GpuSceneBufferUtil.h"

namespace Halcyon::Vulkan
{

Halcyon::Result<void> MeshGroupedIndirectBuffers::initialize(
    GpuAllocator& allocator,
    std::uint32_t capacity,
    std::uint32_t frameCount,
    bool includeScratch,
    const char* groupedVisibleName,
    const char* groupedCountName)
{
    allocator_ = &allocator;
    activeFrame_ = 0;
    if (includeScratch)
    {
        auto heads = GpuSceneDetail::createPerFrame(
            allocator, meshHeads_, frameCount, sizeof(std::uint32_t), capacity,
            "mesh grouping heads");
        auto next = GpuSceneDetail::createPerFrame(
            allocator, meshNext_, frameCount, sizeof(std::uint32_t), capacity,
            "mesh grouping next");
        if (!heads || !next)
        {
            cleanup();
            return Halcyon::Result<void>::failure(
                {Halcyon::ErrorCode::Backend,
                    "failed to allocate mesh grouping scratch buffers",
                    "MeshGroupedIndirectBuffers"});
        }
    }
    auto grouped = GpuSceneDetail::createPerFrame(
        allocator,
        groupedVisible_,
        frameCount,
        sizeof(std::uint32_t),
        capacity,
        groupedVisibleName);
    auto groupedCount = GpuSceneDetail::createPerFrame(
        allocator, groupedCount_, frameCount, sizeof(std::uint32_t), 1, groupedCountName);
    if (!grouped || !groupedCount)
    {
        cleanup();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend,
                "failed to allocate mesh grouping output buffers",
                "MeshGroupedIndirectBuffers"});
    }
    return Halcyon::Result<void>::success();
}

void MeshGroupedIndirectBuffers::cleanup() noexcept
{
    GpuSceneDetail::destroyPerFrame(allocator_, meshHeads_);
    GpuSceneDetail::destroyPerFrame(allocator_, meshNext_);
    GpuSceneDetail::destroyPerFrame(allocator_, groupedVisible_);
    GpuSceneDetail::destroyPerFrame(allocator_, groupedCount_);
    allocator_ = nullptr;
    activeFrame_ = 0;
}

VkBuffer MeshGroupedIndirectBuffers::meshHeadsBuffer() const noexcept
{
    return GpuSceneDetail::active(meshHeads_, activeFrame_).buffer;
}

VkBuffer MeshGroupedIndirectBuffers::meshNextBuffer() const noexcept
{
    return GpuSceneDetail::active(meshNext_, activeFrame_).buffer;
}

VkBuffer MeshGroupedIndirectBuffers::groupedVisibleIndicesBuffer() const noexcept
{
    return GpuSceneDetail::active(groupedVisible_, activeFrame_).buffer;
}

VkBuffer MeshGroupedIndirectBuffers::groupedVisibleCountBuffer() const noexcept
{
    return GpuSceneDetail::active(groupedCount_, activeFrame_).buffer;
}

} // namespace Halcyon::Vulkan
