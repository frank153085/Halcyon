#include "Halcyon/Engine.h"

#include "Application/WindowInternal.h"
#include "EngineInternal.h"
#include "Halcyon/Window.h"
#include "Renderer/Scene/Ecs/RenderExtractor.h"
#include "Renderer/Vulkan/HalcyonVulkanRenderer.h"

#include <exception>
#include <new>
#include <utility>

namespace Halcyon
{

struct Engine::Impl
{
    Platform::Window* window = nullptr;
    Vulkan::Renderer renderer{};
    Scene scene{};
    SceneDatabase sceneDatabase{};
    View view{};
    Capabilities capabilities{};
    bool initialized = false;
    std::filesystem::path loadedScenePath{};
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
    result.gpuFrameMs = source.gpuFrameMs;
    result.deviceMemoryBytes = source.deviceMemoryBytes;
    result.primitiveCount = source.primitiveCount;
    result.clusterOverflowCount = source.clusterOverflowCount;
    result.taaHistoryValid = source.taaHistoryValid;
    result.screenshotWritten = source.screenshotWritten;
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
    backendConfig.startupTexturePath = config.startupTexturePath.empty()
                                           ? nullptr
                                           : config.startupTexturePath.c_str();
    backendConfig.startupMeshPath = config.startupMeshPath.empty()
                                        ? nullptr
                                        : config.startupMeshPath.c_str();
    // Engine owns the scene database/ECS population and performs the upload
    // through loadStaticScene below.  Do not ask the renderer to upload the
    // same asset during initialization and then immediately tear it down for
    // a second upload; some Vulkan drivers report heap corruption when image
    // views and descriptor pools are recreated in that back-to-back pattern.
    backendConfig.startupScenePath = nullptr;
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
        return Result<std::unique_ptr<Engine>>::failure(
            MakeError(ErrorCode::OutOfMemory, "engine allocation failed", "Engine::create"));
    }
    auto resultEngine = std::unique_ptr<Engine>(engine);
    if (!config.startupScenePath.empty())
    {
        const auto sceneResult = resultEngine->loadStaticScene(config.startupScenePath);
        if (!sceneResult)
        {
            resultEngine->shutdown();
            return Result<std::unique_ptr<Engine>>::failure(
                sceneResult.error().withContext("Engine::create"));
        }
    }
    return Result<std::unique_ptr<Engine>>::success(std::move(resultEngine));
}

void Engine::shutdown() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->renderer.shutdown();
        impl_->initialized = false;
        impl_->window = nullptr;
    }
}

Scene& Engine::scene() noexcept
{
    static Scene empty{};
    return impl_ != nullptr ? impl_->scene : empty;
}

const Scene& Engine::scene() const noexcept
{
    static const Scene empty{};
    return impl_ != nullptr ? impl_->scene : empty;
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
        impl_->scene.updateTransforms();
        auto packet = Renderer::Scene::Ecs::RenderExtractor::extract(
            impl_->scene, impl_->view.camera().data(), frameIndex);
        const Vulkan::FrameStats backendStats = impl_->renderer.render(packet.view());
        const FrameStats stats = translateStats(backendStats);
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

Result<void> Engine::loadStaticScene(const std::filesystem::path& path)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<void>::failure(
            MakeError(ErrorCode::InvalidState, "engine is not initialized", "Engine::loadStaticScene"));
    }
    if (path.empty())
    {
        return Result<void>::failure(
            MakeError(ErrorCode::InvalidArgument, "scene path is empty", "Engine::loadStaticScene"));
    }
    const std::filesystem::path normalized = path.lexically_normal();
    if (impl_->loadedScenePath == normalized && !impl_->scene.members().empty())
    {
        return Result<void>::success();
    }
    const auto loaded = Renderer::Scene::loadStaticScene(normalized);
    if (!loaded)
    {
        return Result<void>::failure(loaded.error().withContext("Engine::loadStaticScene"));
    }
    impl_->sceneDatabase.clear();
    const auto imported = impl_->sceneDatabase.importStaticScene(loaded.value());
    if (!imported)
    {
        return Result<void>::failure(imported.error().withContext("Engine::loadStaticScene"));
    }
    const auto rendererResult = impl_->renderer.loadStaticScene(normalized.string().c_str());
    if (!rendererResult)
    {
        return rendererResult.error().withContext("Engine::loadStaticScene");
    }
    impl_->scene.clear();
    for (std::size_t i = 0; i < loaded.value().primitives.size(); ++i)
    {
        const auto entity = impl_->scene.createEntity();
        (void)impl_->scene.transforms().add(entity);
        auto* transform = impl_->scene.transforms().get(entity);
        if (transform != nullptr)
        {
            transform->localTransform = loaded.value().primitives[i].worldTransform;
            transform->dirty = true;
        }
        RenderableComponent renderable{};
        if (i < imported.value().meshes.size()) renderable.mesh = imported.value().meshes[i];
        const auto materialIndex = loaded.value().primitives[i].materialIndex;
        if (materialIndex < imported.value().materials.size())
        {
            renderable.material = imported.value().materials[materialIndex];
        }
        renderable.flags = static_cast<std::uint32_t>(RenderableFlags::CastShadow) |
                           static_cast<std::uint32_t>(RenderableFlags::ReceiveShadow);
        if (materialIndex < loaded.value().materials.size())
        {
            const auto& material = loaded.value().materials[materialIndex];
            if (material.transparent)
            {
                renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::Transparent);
            }
            if (material.doubleSided)
            {
                renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::DoubleSided);
            }
            if (material.alphaMasked)
            {
                renderable.flags |= static_cast<std::uint32_t>(RenderableFlags::AlphaMasked);
            }
        }
        (void)impl_->scene.renderables().add(entity, renderable);
    }
    impl_->loadedScenePath = normalized;
    return Result<void>::success();
}

Result<void> Engine::captureScreenshot(const std::filesystem::path& path)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Result<void>::failure(
            MakeError(ErrorCode::InvalidState, "engine is not initialized", "Engine::captureScreenshot"));
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
