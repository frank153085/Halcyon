#pragma once

#include "GpuAllocator.h"

#include <cstdint>
#include <filesystem>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Owns GPU-to-CPU debug buffers: cluster overflow, visibility comparison,
// instance-id capture, and the queued screenshot path.
class DebugReadbackManager final
{
public:
    static constexpr std::uint32_t VisibilityReadbackCapacity = 1u << 20;
    static constexpr std::uint32_t VisibilityReadbackHeaderCount = 5u;
    static constexpr std::uint32_t VirtualPageRequestCapacity = 1u << 20;

    DebugReadbackManager() = default;
    DebugReadbackManager(const DebugReadbackManager&) = delete;
    DebugReadbackManager& operator=(const DebugReadbackManager&) = delete;

    void cleanup(GpuAllocator& allocator) noexcept;

    std::vector<BufferAllocation> clusterOverflowReadbacks;
    std::vector<BufferAllocation> gpuVisibilityReadbacks;
    std::vector<bool> gpuVisibilityValid;
    std::vector<BufferAllocation> virtualGeometryReadbacks;
    std::vector<bool> virtualGeometryValid;
    // Count followed by VirtualPageRequestCapacity virtual page indices.
    // Each slot is consumed only after its frame fence has completed.
    std::vector<BufferAllocation> virtualPageRequestReadbacks;
    std::vector<bool> virtualPageRequestValid;
    std::vector<std::uint64_t> virtualPageRequestFrameIndices;
    std::vector<std::uint32_t> virtualPageRequestPageCounts;
    std::vector<std::uint32_t> virtualPageRequestMeshIds;
    std::vector<std::vector<std::uint32_t>> gpuReferenceVisible;
    std::vector<BufferAllocation> instanceIdReadbacks;
    std::vector<bool> instanceIdReadbackValid;
    std::vector<std::uint64_t> instanceIdReadbackFrameIndices;
    std::filesystem::path pendingScreenshotPath;
};

} // namespace Halcyon::Vulkan
