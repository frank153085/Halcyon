#pragma once

#include "GpuAllocator.h"

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Per-frame frustum cull outputs and the main-path indirect command stream.
// Phase 1 of two-phase occlusion reuses the same indirect buffers.
class FrustumCullBuffers final
{
public:
    FrustumCullBuffers() = default;
    FrustumCullBuffers(const FrustumCullBuffers&) = delete;
    FrustumCullBuffers& operator=(const FrustumCullBuffers&) = delete;
    FrustumCullBuffers(FrustumCullBuffers&&) noexcept = default;
    FrustumCullBuffers& operator=(FrustumCullBuffers&&) noexcept = default;

    [[nodiscard]] Halcyon::Result<void> initialize(
        GpuAllocator& allocator,
        std::uint32_t capacity,
        std::uint32_t frameCount);
    void cleanup() noexcept;
    void setFrameIndex(std::uint32_t frameIndex) noexcept
    {
        activeFrame_ = frameIndex;
    }

    [[nodiscard]] VkBuffer visibleIndicesBuffer() const noexcept;
    [[nodiscard]] VkBuffer visibleCountBuffer() const noexcept;
    [[nodiscard]] VkBuffer occludedIndicesBuffer() const noexcept;
    [[nodiscard]] VkBuffer occludedCountBuffer() const noexcept;
    [[nodiscard]] VkBuffer indirectCommandsBuffer() const noexcept;
    // Phase 1 reuses the main-path count buffer by design.
    [[nodiscard]] VkBuffer indirectDrawCountBuffer() const noexcept;

private:
    GpuAllocator* allocator_ = nullptr;
    std::vector<BufferAllocation> visibleIndices_;
    std::vector<BufferAllocation> visibleCount_;
    std::vector<BufferAllocation> occludedIndices_;
    std::vector<BufferAllocation> occludedCount_;
    std::vector<BufferAllocation> indirectCommands_;
    std::vector<BufferAllocation> indirectCount_;
    std::uint32_t activeFrame_ = 0;
};

} // namespace Halcyon::Vulkan
