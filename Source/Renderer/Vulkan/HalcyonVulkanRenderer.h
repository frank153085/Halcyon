#pragma once

// A small, deliberately explicit Vulkan 1.3 renderer used by the Halcyon
// learning project.  The public header does not expose implementation
// objects (command pools, swapchain images, etc.); callers submit an
// immutable FramePacket and receive diagnostic frame statistics.

#include "../../Core/Result.h"
#include "../Scene/Camera.h"
#include "../Scene/FramePacket.h"
#include "GpuResourceManager.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace Halcyon::Vulkan
{

enum class FeatureMode : std::uint8_t
{
    Disabled,
    Auto,
    Required,
};

struct Extent2D
{
    std::uint32_t width = 1280;
    std::uint32_t height = 720;

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }
};

struct RendererConfig
{
    Extent2D initialExtent{};
    float targetFrameTimeMs = 16.667f;
    std::uint32_t framesInFlight = 3;
    bool enableValidation = true;
    FeatureMode rayQuery = FeatureMode::Auto;
    const char* applicationName = "Halcyon";
    std::uint32_t applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    // Optional startup resources.  The renderer treats these as external
    // inputs; applications may leave them null and provide resources through
    // a different loading path.
    const char* startupTexturePath = nullptr;
    const char* startupMeshPath = nullptr;
};

// Compatibility aliases keep the original M1 public names source-compatible
// while the canonical packet types live in the backend-neutral Scene module.
using CameraData = Halcyon::Renderer::Scene::CameraData;
using InstanceData = Halcyon::Renderer::Scene::InstanceData;
using LightData = Halcyon::Renderer::Scene::LightData;
using FramePacket = Halcyon::Renderer::Scene::FramePacket;

struct QualityState
{
    float internalResolutionScale = 1.0f;
    float shadowResolutionScale = 1.0f;
    float lodBias = 0.0f;
    bool rayQueryEnabled = false;
};

struct FrameStats
{
    double cpuFrameMs = 0.0;
    double gpuFrameMs = -1.0;
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
    std::uint32_t instanceApiVersion = VK_API_VERSION_1_0;
    std::uint32_t deviceApiVersion = VK_API_VERSION_1_0;
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
    bool bufferDeviceAddress = false;
    bool indirectCount = false;
    bool fragmentBarycentric = false;
    bool rayQuery = false;
    bool depthD32 = false;
    bool reversedZ = false;
    bool swapchain = false;
    std::uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t presentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
};

class Renderer final
{
public:
    Renderer() noexcept;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    // GLFW owns the window.  It must outlive the Renderer and have been
    // created with GLFW_CLIENT_API = GLFW_NO_API.
    [[nodiscard]] Halcyon::Result<void> initialize(
        GLFWwindow* window, const RendererConfig& config = {});

    // Records one frame from the supplied immutable packet and presents it.
    // A minimized/out-of-date surface is reported in FrameStats instead of
    // being treated as a fatal error.
    [[nodiscard]] FrameStats render(const FramePacket& packet = {});

    // The actual recreation is deferred until a non-zero framebuffer extent
    // is available.  This makes GLFW minimize/restore and live resize safe.
    [[nodiscard]] Halcyon::Result<void> resize(Extent2D extent);

    void shutdown() noexcept;

    [[nodiscard]] const Capabilities& capabilities() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

    // Resource helpers keep VMA and staging details inside the Vulkan backend.
    [[nodiscard]] Halcyon::Result<TextureResource> loadTexture2D(const char* path);
    [[nodiscard]] Halcyon::Result<MeshResource> loadObj(const char* path);
    void destroy(TextureResource& texture) noexcept;
    void destroy(MeshResource& mesh) noexcept;

    // Read-only escape hatches for tools that need to attach a profiler or
    // RenderDoc marker.  They may return VK_NULL_HANDLE before initialization.
    [[nodiscard]] VkInstance instance() const noexcept;
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept;
    [[nodiscard]] VkDevice device() const noexcept;
    [[nodiscard]] VkQueue graphicsQueue() const noexcept;
    [[nodiscard]] VkQueue presentQueue() const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace Halcyon::Vulkan
