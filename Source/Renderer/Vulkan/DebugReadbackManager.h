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

    DebugReadbackManager() = default;
    DebugReadbackManager(const DebugReadbackManager&) = delete;
    DebugReadbackManager& operator=(const DebugReadbackManager&) = delete;

    void cleanup(GpuAllocator& allocator) noexcept;

    std::vector<BufferAllocation> clusterOverflowReadbacks;
    std::vector<BufferAllocation> gpuVisibilityReadbacks;
    std::vector<bool> gpuVisibilityValid;
    std::vector<BufferAllocation> virtualGeometryReadbacks;
    std::vector<bool> virtualGeometryValid;
    std::vector<std::vector<std::uint32_t>> gpuReferenceVisible;
    std::vector<BufferAllocation> instanceIdReadbacks;
    std::vector<bool> instanceIdReadbackValid;
    std::vector<std::uint64_t> instanceIdReadbackFrameIndices;
    std::filesystem::path pendingScreenshotPath;
};

} // namespace Halcyon::Vulkan
