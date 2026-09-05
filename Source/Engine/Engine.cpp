#include "Halcyon/Engine.h"

#include "Application/WindowInternal.h"
#include "EngineInternal.h"
#include "Halcyon/Window.h"
#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"
#include "Core/Profiler.h"

#include <chrono>
#include <exception>
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
    result.cpuFrameMs = source.cpuFrameMs;
    result.cpuVisibilityMs = source.cpuVisibilityMs;
    result.gpuFrustumCullMs = source.gpuFrustumCullMs;
    result.gpuIndirectBuildMs = source.gpuIndirectBuildMs;
    result.gpuHiZBuildMs = source.gpuHiZBuildMs;
    result.gpuTwoPhaseMs = source.gpuTwoPhaseMs;
    result.visibleInstanceCount = source.visibleInstanceCount;
    result.indirectDrawCount = source.indirectDrawCount;
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
    backendConfig.exposure = config.exposure;
    backendConfig.enableTaa = config.enableTaa;
    backendConfig.enableClusteredLighting = config.enableClusteredLighting;
    backendConfig.enableTransparency = config.enableTransparency;

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
        auto packet = impl_->sceneManager.extract(impl_->view.camera().data(), frameIndex);
        if (!packet)
        {
            return Result<FrameStats>::failure(packet.error().withContext("Engine::render"));
        }
        const Vulkan::FrameStats backendStats = impl_->renderer.render(packet.value().view());
        FrameStats stats = translateStats(backendStats);
        stats.cpuVisibilityMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - visibilityBegin).count();
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
