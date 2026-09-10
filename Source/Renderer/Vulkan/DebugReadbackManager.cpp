#include "DebugReadbackManager.h"

namespace Halcyon::Vulkan
{

void DebugReadbackManager::cleanup(GpuAllocator& allocator) noexcept
{
    for (auto& readback : clusterOverflowReadbacks)
    {
        allocator.destroy(readback);
    }
    clusterOverflowReadbacks.clear();
    for (auto& readback : gpuVisibilityReadbacks)
    {
        allocator.destroy(readback);
    }
    gpuVisibilityReadbacks.clear();
    gpuVisibilityValid.clear();
    for (auto& readback : virtualGeometryReadbacks)
        allocator.destroy(readback);
    virtualGeometryReadbacks.clear();
    virtualGeometryValid.clear();
    for (auto& readback : virtualPageRequestReadbacks)
        allocator.destroy(readback);
    virtualPageRequestReadbacks.clear();
    virtualPageRequestValid.clear();
    virtualPageRequestFrameIndices.clear();
    virtualPageRequestPageCounts.clear();
    virtualPageRequestMeshIds.clear();
    gpuReferenceVisible.clear();
    for (auto& readback : instanceIdReadbacks)
    {
        allocator.destroy(readback);
    }
    instanceIdReadbacks.clear();
    instanceIdReadbackValid.clear();
    instanceIdReadbackFrameIndices.clear();
    pendingScreenshotPath.clear();
}

} // namespace Halcyon::Vulkan
