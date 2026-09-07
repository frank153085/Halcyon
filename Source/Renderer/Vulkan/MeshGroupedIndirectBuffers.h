#pragma once

#include "GpuAllocator.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Linked-list scratch and compacted per-mesh visible lists produced by
// build_indirect_commands. Phase 1/main and phase 2 keep separate grouped
// outputs; meshHeads/meshNext are shared scratch reset between phases.
class MeshGroupedIndirectBuffers final
{
public:
    MeshGroupedIndirectBuffers() = default;
    MeshGroupedIndirectBuffers(const MeshGroupedIndirectBuffers&) = delete;
    MeshGroupedIndirectBuffers& operator=(const MeshGroupedIndirectBuffers&) = delete;
    MeshGroupedIndirectBuffers(MeshGroupedIndirectBuffers&&) noexcept = default;
    MeshGroupedIndirectBuffers& operator=(MeshGroupedIndirectBuffers&&) noexcept =
        default;

    [[nodiscard]] Halcyon::Result<void> initialize(
        GpuAllocator& allocator,
        std::uint32_t capacity,
        std::uint32_t frameCount,
        bool includeScratch,
        const char* groupedVisibleName,
        const char* groupedCountName);
    void cleanup() noexcept;
    void setFrameIndex(std::uint32_t frameIndex) noexcept
    {
        activeFrame_ = frameIndex;
    }

    [[nodiscard]] VkBuffer meshHeadsBuffer() const noexcept;
    [[nodiscard]] VkBuffer meshNextBuffer() const noexcept;
    [[nodiscard]] VkBuffer groupedVisibleIndicesBuffer() const noexcept;
    [[nodiscard]] VkBuffer groupedVisibleCountBuffer() const noexcept;

private:
    GpuAllocator* allocator_ = nullptr;
    std::vector<BufferAllocation> meshHeads_;
    std::vector<BufferAllocation> meshNext_;
    std::vector<BufferAllocation> groupedVisible_;
    std::vector<BufferAllocation> groupedCount_;
    std::uint32_t activeFrame_ = 0;
};

} // namespace Halcyon::Vulkan
