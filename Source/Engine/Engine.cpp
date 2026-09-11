#include "Halcyon/Engine.h"

#include "Application/WindowInternal.h"
#include "EngineInternal.h"
#include "Halcyon/Window.h"
#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"
#include "Core/Profiler.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <exception>
#include <glm/gtc/quaternion.hpp>
#include <new>
#include <utility>

namespace Halcyon
{

struct Engine::Impl
{
    Platform::Window* window = nullptr;
    Vulkan::Renderer renderer{};
    SceneManager sceneManager{};
    View view{};
    Capabilities capabilities{};
    bool initialized = false;
    bool enableTransparency = true;
    bool virtualGeometryRequested = false;
    float fixedDeltaSeconds = 1.0f / 60.0f;
    bool previousCameraValid = false;
    std::uint64_t previousCameraFrame = 0u;
    glm::vec3 previousCameraPosition{0.0f};
    glm::vec3 previousCameraForward{0.0f, 0.0f, -1.0f};
    glm::vec2 previousViewport{0.0f};

    void updateCameraMotion(OwnedSceneFramePacket& packet)
    {
        packet.cameraMotion = {};
        const glm::vec3 position = glm::vec3(packet.camera.positionAndNear);
        const glm::vec3 rawForward = glm::vec3(packet.camera.forwardAndFar);
        const float forwardLength = glm::length(rawForward);
        const glm::vec3 forward = forwardLength > 1.0e-5f
            ? rawForward / forwardLength : glm::vec3{0.0f, 0.0f, -1.0f};
        const glm::vec2 viewport = glm::vec2(packet.camera.viewportAndInvViewport);
        const float dt = fixedDeltaSeconds;
        const bool finitePose = std::all_of(&position.x, &position.x + 3,
            [](float value) { return std::isfinite(value); }) &&
            std::all_of(&forward.x, &forward.x + 3,
                [](float value) { return std::isfinite(value); });
        const bool finiteViewport = std::isfinite(viewport.x) &&
            std::isfinite(viewport.y) && viewport.x > 0.0f && viewport.y > 0.0f;
        const bool contiguous = previousCameraValid &&
            packet.frameIndex > previousCameraFrame &&
            packet.frameIndex - previousCameraFrame <= 2u &&
            glm::all(glm::equal(viewport, previousViewport));
        if (finitePose && finiteViewport && contiguous && std::isfinite(dt) && dt > 1.0e-5f)
        {
            const glm::vec3 displacement = position - previousCameraPosition;
            const float distance = glm::length(displacement);
            const float cosine = std::clamp(glm::dot(forward, previousCameraForward), -1.0f, 1.0f);
            const float angle = std::acos(cosine);
            if (distance <= 25.0f && angle <= glm::radians(90.0f))
            {
                const glm::vec3 rotationAxis = glm::cross(previousCameraForward, forward);
                const float axisLength = glm::length(rotationAxis);
                const glm::vec3 angularVelocity = axisLength > 1.0e-5f
                    ? rotationAxis * (angle / (dt * axisLength)) : glm::vec3{0.0f};
                packet.cameraMotion.linearVelocityAndDt =
                    glm::vec4(displacement / dt, dt);
                packet.cameraMotion.angularVelocityAndValid =
                    glm::vec4(angularVelocity, 1.0f);
            }
        }
        previousCameraPosition = position;
        previousCameraForward = finitePose ? forward : glm::vec3{0.0f, 0.0f, -1.0f};
        previousViewport = viewport;
        previousCameraFrame = packet.frameIndex;
        previousCameraValid = finitePose && finiteViewport;
    }
};

namespace
{

[[nodiscard]] Capabilities translateCapabilities(const Vulkan::Capabilities& source) noexcept
{
    Capabilities result{};
    result.instanceApiVersion = source.instanceApiVersion;
    result.deviceApiVersion = source.deviceApiVersion;
    result.deviceName = source.deviceName;
    result.vendorId = source.vendorId;
    result.deviceId = source.deviceId;
    result.driverVersion = source.driverVersion;
    result.deviceLocalMemoryBytes = source.deviceLocalMemoryBytes;
    result.validationEnabled = source.validationEnabled;
    result.debugUtils = source.debugUtils;
    result.dynamicRendering = source.dynamicRendering;
    result.synchronization2 = source.synchronization2;
    result.timelineSemaphore = source.timelineSemaphore;
    result.descriptorIndexing = source.descriptorIndexing;
    result.bindlessTable = source.bindlessTable;
    result.bufferDeviceAddress = source.bufferDeviceAddress;
    result.indirectCount = source.indirectCount;
    result.scalarBlockLayout = source.scalarBlockLayout;
    result.geometryShader = source.geometryShader;
    result.fragmentBarycentric = source.fragmentBarycentric;
    result.rayQuery = source.rayQuery;
    result.depthD32 = source.depthD32;
    result.reversedZ = source.reversedZ;
    result.swapchain = source.swapchain;
    result.graphicsQueueFamily = source.graphicsQueueFamily;
    result.presentQueueFamily = source.presentQueueFamily;
    return result;
}

[[nodiscard]] FrameStats translateStats(const Vulkan::FrameStats& source) noexcept
{
    FrameStats result{};
    result.renderPath = source.renderPath;
    result.cpuFrameMs = source.cpuFrameMs;
    result.cpuVisibilityMs = source.cpuVisibilityMs;
    result.gpuFrustumCullMs = source.gpuFrustumCullMs;
    result.gpuIndirectBuildMs = source.gpuIndirectBuildMs;
    result.gpuHiZBuildMs = source.gpuHiZBuildMs;
    result.gpuTwoPhaseMs = source.gpuTwoPhaseMs;
    result.visibleInstanceCount = source.visibleInstanceCount;
    result.indirectDrawCount = source.indirectDrawCount;
    result.virtualVisibleMeshletCount = source.virtualVisibleMeshletCount;
    result.virtualIndirectCommandCount = source.virtualIndirectCommandCount;
    result.virtualDagNodeCount = source.virtualDagNodeCount;
    result.virtualSelectedNodeCount = source.virtualSelectedNodeCount;
    result.virtualLodSwitchCount = source.virtualLodSwitchCount;
    result.virtualPageRequestCount = source.virtualPageRequestCount;
    result.virtualPageRequestOverflowCount = source.virtualPageRequestOverflowCount;
    result.virtualPageUsageTouchCount = source.virtualPageUsageTouchCount;
    result.virtualPagePrefetchCount = source.virtualPagePrefetchCount;
    result.virtualGeometryQualityScale = source.virtualGeometryQualityScale;
    result.virtualGeometryStreamingPressure = source.virtualGeometryStreamingPressure;
    result.virtualGeometryResidentPages = source.virtualGeometryResidentPages;
    result.virtualGeometryEvictedPages = source.virtualGeometryEvictedPages;
    result.virtualGeometryUploadedBytes = source.virtualGeometryUploadedBytes;
    result.virtualPagePoolPeakPages = source.virtualPagePoolPeakPages;
    result.virtualPageEvictionFrameProtected = source.virtualPageEvictionFrameProtected;
    result.virtualPageEvictionTimelineBlocked = source.virtualPageEvictionTimelineBlocked;
    result.meshShaderActive = source.meshShaderActive;
    result.meshShaderFallbackReason = source.meshShaderFallbackReason;
    result.virtualInvalidVisibilityCount = source.virtualInvalidVisibilityCount;
    result.frustumVisibleInstanceCount = source.frustumVisibleInstanceCount;
    result.occludedInstanceCount = source.occludedInstanceCount;
    result.gpuDrivenActive = source.gpuDrivenActive;
    result.gpuFallbackInstanceCount = source.gpuFallbackInstanceCount;
    result.gpuVisibilityMissingCount = source.gpuVisibilityMissingCount;
    result.gpuVisibilityValidationPassed = source.gpuVisibilityValidationPassed;
    result.gpuInstanceIdInvalidPixelCount = source.gpuInstanceIdInvalidPixelCount;
    result.materialDescriptorBindCount = source.materialDescriptorBindCount;
    result.gpuFrameMs = source.gpuFrameMs;
    result.deviceMemoryBytes = source.deviceMemoryBytes;
    result.primitiveCount = source.primitiveCount;
    result.clusterOverflowCount = source.clusterOverflowCount;
    result.taaHistoryValid = source.taaHistoryValid;
    result.screenshotWritten = source.screenshotWritten;
    result.goldenImageCompared = source.goldenImageCompared;
    result.goldenImagePassed = source.goldenImagePassed;
    result.performanceCsvWritten = source.performanceCsvWritten;
    result.quality.internalResolutionScale = source.quality.internalResolutionScale;
    result.quality.shadowResolutionScale = source.quality.shadowResolutionScale;
    result.quality.lodBias = source.quality.lodBias;
    result.quality.exposure = source.quality.exposure;
    result.quality.rayQueryEnabled = source.quality.rayQueryEnabled;
    result.quality.taaEnabled = source.quality.taaEnabled;
    result.quality.clusteredLightingEnabled = source.quality.clusteredLightingEnabled;
    result.quality.transparencyEnabled = source.quality.transparencyEnabled;
    result.swapchainImageIndex = source.swapchainImageIndex;
    result.rendered = source.rendered;
    result.recreatedSwapchain = source.recreatedSwapchain;
    result.suboptimal = source.suboptimal;
    result.minimized = source.minimized;
    result.deviceLost = source.deviceLost;
    result.fatalError = source.fatalError;
    result.executedPasses = source.executedPasses;
    result.gpuPasses.reserve(source.gpuPasses.size());
    for (const auto& pass : source.gpuPasses)
    {
        result.gpuPasses.push_back({pass.name, pass.gpuFrameMs});
    }
    return result;
}

} // namespace

Engine::Engine(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
{
}

Engine::~Engine()
{
    shutdown();
}

Engine::Engine(Engine&& other) noexcept
        : impl_(std::move(other.impl_))
{
}

Engine& Engine::operator=(Engine&& other) noexcept
{
    if (this != &other)
    {
        shutdown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<std::unique_ptr<Engine>> Engine::create(Platform::Window& window, const EngineConfig& config)
{
    auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl{});
    if (impl == nullptr)
    {
        return Result<std::unique_ptr<Engine>>::failure(
            MakeError(ErrorCode::OutOfMemory, "engine state allocation failed", "Engine::create"));
    }

    const Extent2D extent = window.framebufferExtent();
    Vulkan::RendererConfig backendConfig{};
    backendConfig.initialExtent = {extent.width, extent.height};
    backendConfig.targetFrameTimeMs = config.targetFrameTimeMs;
    backendConfig.framesInFlight = config.framesInFlight;
    backendConfig.enableValidation = config.enableValidation;
    backendConfig.rayQuery = static_cast<Vulkan::FeatureMode>(config.rayQuery);
    backendConfig.meshShader = static_cast<Vulkan::FeatureMode>(config.meshShader);
    backendConfig.exposure = config.exposure;
    backendConfig.enableTaa = config.enableTaa;
    backendConfig.enableClusteredLighting = config.enableClusteredLighting;
    backendConfig.enableTransparency = config.enableTransparency;
    backendConfig.enableGpuDrivenScene = config.enableGpuDrivenScene ||
        config.enableTwoPhaseOcclusion || config.renderPath != RenderPathMode::DeferredIndexed;
    backendConfig.renderPath = config.renderPath;
    backendConfig.enableTwoPhaseOcclusion = config.enableTwoPhaseOcclusion;
    backendConfig.enableVsync = config.enableVsync;
    backendConfig.instanceIdReportPath = config.instanceIdReportPath;

    impl->enableTransparency = config.enableTransparency;
    impl->fixedDeltaSeconds = config.fixedDeltaSeconds;
    impl->virtualGeometryRequested =
        config.renderPath == RenderPathMode::VirtualGeometryIndexed ||
        config.renderPath == RenderPathMode::VirtualGeometryMeshShader;
    impl->window = &window;
    const auto initializeResult = impl->renderer.initialize(
        Platform::Internal::WindowAccess::nativeHandle(window), backendConfig);
    if (!initializeResult)
    {
        return Result<std::unique_ptr<Engine>>::failure(
            initializeResult.error().withContext("Engine::create"));
    }

    try
    {
        impl->capabilities = translateCapabilities(impl->renderer.capabilities());
    }
    catch (const std::bad_alloc&)
    {
        impl->renderer.shutdown();
        return Result<std::unique_ptr<Engine>>::failure(
            MakeError(ErrorCode::OutOfMemory, "capability allocation failed", "Engine::create"));
    }
    if (!extent.empty())
    {
        const auto viewportResult = impl->view.setViewport(extent);
        if (!viewportResult)
        {
            impl->renderer.shutdown();
            return Result<std::unique_ptr<Engine>>::failure(
                viewportResult.error().withContext("Engine::create"));
        }
    }
    impl->initialized = true;
    Engine* engine = new (std::nothrow) Engine(std::move(impl));
    if (engine == nullptr)
    {
        // The renderer is already initialized at this point; release it
        // before returning so an allocation failure cannot leak the Vulkan
        // device and its surface. A nothrow new-expression does not run the
        // initializer when allocation fails, so local ownership is retained.
        if (impl != nullptr)
        {
            impl->renderer.shutdown();
        }
        return Result<std::unique_ptr<Engine>>::failure(
            MakeError(ErrorCode::OutOfMemory, "engine allocation failed", "Engine::create"));
    }
    auto resultEngine = std::unique_ptr<Engine>(engine);
    const auto sceneResult =
        resultEngine->impl_->sceneManager.initialize(config.scene, resultEngine->impl_->renderer);
    if (!sceneResult)
    {
        resultEngine->shutdown();
        return Result<std::unique_ptr<Engine>>::failure(
            sceneResult.error().withContext("Engine::create"));
    }
    return Result<std::unique_ptr<Engine>>::success(std::move(resultEngine));
}

void Engine::shutdown() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->sceneManager.shutdown();
        impl_->renderer.shutdown();
        impl_->initialized = false;
        impl_->window = nullptr;
    }
}

Scene& Engine::scene() noexcept
{
    static Scene empty{};
    return impl_ != nullptr ? impl_->sceneManager.scene() : empty;
}

const Scene& Engine::scene() const noexcept
{
    static const Scene empty{};
    return impl_ != nullptr ? impl_->sceneManager.scene() : empty;
}

SceneManager& Engine::sceneManager() noexcept
{
    static SceneManager empty{};
    return impl_ != nullptr ? impl_->sceneManager : empty;
}

const SceneManager& Engine::sceneManager() const noexcept
{
    static const SceneManager empty{};
    return impl_ != nullptr ? impl_->sceneManager : empty;
}

View& Engine::defaultView() noexcept
{
    static View empty{};
    return impl_ != nullptr ? impl_->view : empty;
}

const View& Engine::defaultView() const noexcept
{
    static const View empty{};
    return impl_ != nullptr ? impl_->view : empty;
}

Result<FrameStats> Engine::render(std::uint64_t frameIndex)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<FrameStats>::failure(
            MakeError(ErrorCode::InvalidState, "engine is not initialized", "Engine::render"));
    }

    try
    {
        HALCYON_PROFILE_SCOPE("CPU visibility extraction");
        const auto visibilityBegin = std::chrono::steady_clock::now();
        impl_->sceneManager.scene().updateTransforms();
        if (impl_->renderer.gpuDrivenSceneEnabled())
        {
            auto delta = impl_->sceneManager.extractDelta(impl_->view.camera().data(), frameIndex);
            if (!delta)
                return Result<FrameStats>::failure(delta.error().withContext(
                    "Engine::render GPU scene delta"));
            const auto gpuSceneUpdate = impl_->renderer.updateGpuSceneDelta(delta.value());
            if (!gpuSceneUpdate)
                return Result<FrameStats>::failure(
                    gpuSceneUpdate.error().withContext("Engine::render GPU scene delta upload"));
        }
        const double cpuVisibilityMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - visibilityBegin).count();
        auto packet = !impl_->virtualGeometryRequested &&
                impl_->renderer.gpuDrivenSceneEnabled() &&
                impl_->renderer.gpuDrivenBindlessEnabled()
            ? impl_->sceneManager.extractGpuDrivenCpu(impl_->view.camera().data(), frameIndex,
                  impl_->enableTransparency)
            : impl_->sceneManager.extract(impl_->view.camera().data(), frameIndex);
        if (!packet)
        {
            return Result<FrameStats>::failure(packet.error().withContext("Engine::render"));
        }
        impl_->updateCameraMotion(packet.value());
        const Vulkan::FrameStats backendStats = impl_->renderer.render(packet.value().view());
        FrameStats stats = translateStats(backendStats);
        stats.cpuVisibilityMs = cpuVisibilityMs;
        if (stats.deviceLost)
        {
            impl_->initialized = false;
            return Result<FrameStats>::failure(
                MakeError(ErrorCode::DeviceLost, impl_->renderer.lastError(), "Engine::render"));
        }
        if (stats.fatalError)
        {
            impl_->initialized = false;
            return Result<FrameStats>::failure(
                MakeError(ErrorCode::Backend, impl_->renderer.lastError(), "Engine::render"));
        }
        return Result<FrameStats>::success(stats);
    }
    catch (const std::bad_alloc&)
    {
        impl_->initialized = false;
        return Result<FrameStats>::failure(MakeError(
            ErrorCode::OutOfMemory, "frame extraction allocation failed", "Engine::render"));
    }
    catch (const std::exception& exception)
    {
        impl_->initialized = false;
        return Result<FrameStats>::failure(
            MakeError(ErrorCode::Backend, exception.what(), "Engine::render"));
    }
    catch (...)
    {
        impl_->initialized = false;
        return Result<FrameStats>::failure(
            MakeError(ErrorCode::Backend, "unknown frame submission failure", "Engine::render"));
    }
}

Result<void> Engine::resize(Extent2D extent)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<void>::failure(
            MakeError(ErrorCode::InvalidState, "engine is not initialized", "Engine::resize"));
    }
    const auto result = impl_->renderer.resize({extent.width, extent.height});
    if (!result)
    {
        return Result<void>::failure(result.error().withContext("Engine::resize"));
    }
    if (!extent.empty())
    {
        const auto viewportResult = impl_->view.setViewport(extent);
        if (!viewportResult)
        {
            return viewportResult;
        }
    }
    return Result<void>::success();
}

Result<void> Engine::captureScreenshot(const std::filesystem::path& path)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<void>::failure(MakeError(
            ErrorCode::InvalidState, "engine is not initialized", "Engine::captureScreenshot"));
    }
    return impl_->renderer.captureScreenshot(path);
}

const Capabilities& Engine::capabilities() const noexcept
{
    static const Capabilities empty{};
    return impl_ != nullptr ? impl_->capabilities : empty;
}

namespace Internal
{

Vulkan::Renderer* EngineAccess::renderer(Engine& engine) noexcept
{
    return engine.impl_ != nullptr ? &engine.impl_->renderer : nullptr;
}

} // namespace Internal

} // namespace Halcyon
