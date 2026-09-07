#pragma once

#include "GpuAllocator.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Per-phase occlusion outputs. Phase 1 and phase 2 each own an instance so
// compacted slot streams stay independent when both draws share a command
// buffer. Phase 2 also owns its overlay indirect command stream.
class OcclusionPhaseBuffers final
{
public:
    OcclusionPhaseBuffers() = default;
    OcclusionPhaseBuffers(const OcclusionPhaseBuffers&) = delete;
    OcclusionPhaseBuffers& operator=(const OcclusionPhaseBuffers&) = delete;
    OcclusionPhaseBuffers(OcclusionPhaseBuffers&&) noexcept = default;
    OcclusionPhaseBuffers& operator=(OcclusionPhaseBuffers&&) noexcept = default;

    [[nodiscard]] Halcyon::Result<void> initialize(
        GpuAllocator& allocator,
        std::uint32_t capacity,
        std::uint32_t frameCount,
        bool includeIndirect,
        const char* visibleName,
        const char* visibleCountName);
    void cleanup() noexcept;
    void setFrameIndex(std::uint32_t frameIndex) noexcept
    {
        activeFrame_ = frameIndex;
    }

    [[nodiscard]] VkBuffer visibleIndicesBuffer() const noexcept;
    [[nodiscard]] VkBuffer visibleCountBuffer() const noexcept;
    [[nodiscard]] VkBuffer indirectCommandsBuffer() const noexcept;
    [[nodiscard]] VkBuffer indirectDrawCountBuffer() const noexcept;

private:
    GpuAllocator* allocator_ = nullptr;
    std::vector<BufferAllocation> visibleIndices_;
    std::vector<BufferAllocation> visibleCount_;
    std::vector<BufferAllocation> indirectCommands_;
    std::vector<BufferAllocation> indirectCount_;
    std::uint32_t activeFrame_ = 0;
};

} // namespace Halcyon::Vulkan
