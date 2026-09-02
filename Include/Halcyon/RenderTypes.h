#pragma once

// Backend-neutral types shared by the application and engine layers.
// Vulkan-specific handles and structures stay in Source/Renderer.

#include <cstdint>
#include <string>
#include <vector>

namespace Halcyon
{

struct Extent2D
{
    std::uint32_t width = 1280;
    std::uint32_t height = 720;

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }
};

enum class FeatureMode : std::uint8_t
{
    Disabled,
    Auto,
    Required,
};

struct QualityState
{
    float internalResolutionScale = 1.0f;
    float shadowResolutionScale = 1.0f;
    float lodBias = 0.0f;
    bool rayQueryEnabled = false;
};

struct FrameStats
{
    struct PassTiming
    {
        std::string name;
        double gpuFrameMs = -1.0;
    };

    double cpuFrameMs = 0.0;
    double gpuFrameMs = -1.0;
    std::vector<PassTiming> gpuPasses;
    std::uint64_t deviceMemoryBytes = 0;
    QualityState quality{};
    std::uint32_t swapchainImageIndex = 0;
    bool rendered = false;
    bool recreatedSwapchain = false;
    bool suboptimal = false;
    bool minimized = false;
    bool deviceLost = false;
    bool fatalError = false;
};

struct Capabilities
{
    std::uint32_t instanceApiVersion = 0;
    std::uint32_t deviceApiVersion = 0;
    std::string deviceName;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t deviceLocalMemoryBytes = 0;
    bool validationEnabled = false;
    bool debugUtils = false;
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool timelineSemaphore = false;
    bool descriptorIndexing = false;
    bool bindlessTable = false;
    bool bufferDeviceAddress = false;
    bool indirectCount = false;
    bool fragmentBarycentric = false;
    bool rayQuery = false;
    bool depthD32 = false;
    bool reversedZ = false;
    bool swapchain = false;
    std::uint32_t graphicsQueueFamily = 0xffffffffu;
    std::uint32_t presentQueueFamily = 0xffffffffu;
};

} // namespace Halcyon
