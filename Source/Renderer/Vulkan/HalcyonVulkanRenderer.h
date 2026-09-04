#pragma once

// A small, deliberately explicit Vulkan 1.3 renderer used by the Halcyon
// learning project.  The public header does not expose implementation
// objects (command pools, swapchain images, etc.); callers submit an
// immutable FramePacket and receive diagnostic frame statistics.

#include "../../Core/Result.h"
#include "../Scene/Camera.h"
#include "../Scene/FramePacket.h"
#include "../Scene/SceneDatabase.h"
#include "Halcyon/RenderTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace Halcyon::Vulkan
{

using FeatureMode = Halcyon::FeatureMode;
using Extent2D = Halcyon::Extent2D;

struct RendererConfig
{
    Extent2D initialExtent{};
    float targetFrameTimeMs = 16.667f;
    std::uint32_t framesInFlight = 3;
    bool enableValidation = true;
    FeatureMode rayQuery = FeatureMode::Auto;
    const char* applicationName = "Halcyon";
    std::uint32_t applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    float exposure = 0.0f;
    bool enableTaa = true;
    bool enableClusteredLighting = true;
    bool enableTransparency = true;
};

// The Vulkan backend consumes the canonical packet types from the
// backend-neutral scene module.
using CameraData = Halcyon::Renderer::Scene::CameraData;
using InstanceData = Halcyon::Renderer::Scene::InstanceData;
using LightData = Halcyon::Renderer::Scene::LightData;
using FramePacket = Halcyon::Renderer::Scene::FramePacket;

using QualityState = Halcyon::QualityState;
using FrameStats = Halcyon::FrameStats;
using Capabilities = Halcyon::Capabilities;

class Renderer final
{
public:
    // An optional overlay callback is invoked by the application while the
    // current pass scope is active. Applications can use it for diagnostics
    // (for example Dear ImGui) without taking ownership of the frame command
    // buffer.
    using OverlayCallback = void (*)(VkCommandBuffer commandBuffer) noexcept;

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

    // SceneManager is the only producer of backend resources. These methods
    // consume and release stable SceneDatabase records without parsing files.
    [[nodiscard]] Halcyon::Result<void> uploadSceneAsset(
        const Halcyon::Renderer::Scene::SceneDatabase& database,
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    [[nodiscard]] Halcyon::Result<void> releaseSceneAsset(
        const Halcyon::Renderer::Scene::SceneImportResult& imported);
    // Scene topology or material changes invalidate temporal history before
    // the next frame is submitted.
    void invalidateTaaHistory() noexcept;
    [[nodiscard]] Halcyon::Result<void> captureScreenshot(const std::filesystem::path& path);

    // Read-only escape hatches for tools that need to attach a profiler or
    // RenderDoc marker.  They may return VK_NULL_HANDLE before initialization.
    [[nodiscard]] VkInstance instance() const noexcept;
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept;
    [[nodiscard]] VkDevice device() const noexcept;
    [[nodiscard]] VkQueue graphicsQueue() const noexcept;
    [[nodiscard]] VkQueue presentQueue() const noexcept;
    [[nodiscard]] VkFormat swapchainFormat() const noexcept;
    [[nodiscard]] VkFormat depthFormat() const noexcept;
    [[nodiscard]] VkExtent2D swapchainExtent() const noexcept;
    [[nodiscard]] std::uint32_t swapchainImageCount() const noexcept;

    void setOverlayCallback(OverlayCallback callback) noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace Halcyon::Vulkan
