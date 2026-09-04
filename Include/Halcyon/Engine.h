#pragma once

#include "Core/Result.h"
#include "RenderTypes.h"
#include "Scene.h"
#include "SceneManager.h"
#include "View.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#ifndef HALCYON_ENABLE_VALIDATION
#define HALCYON_ENABLE_VALIDATION 1
#endif

namespace Halcyon::Platform
{
class Window;
}

namespace Halcyon
{

namespace Internal
{
struct EngineAccess;
}

struct EngineConfig
{
    float targetFrameTimeMs = 16.667f;
    std::uint32_t framesInFlight = 3;
    bool enableValidation = HALCYON_ENABLE_VALIDATION != 0;
    FeatureMode rayQuery = FeatureMode::Disabled;
    SceneManagerConfig scene{};
    float fixedDeltaSeconds = 1.0f / 60.0f;
    float exposure = 0.0f;
    bool enableTaa = true;
    bool enableClusteredLighting = true;
    bool enableTransparency = true;
};

class Engine final
{
public:
    [[nodiscard]] static Result<std::unique_ptr<Engine>> create(
        Platform::Window& window, const EngineConfig& config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    void shutdown() noexcept;

    [[nodiscard]] Scene& scene() noexcept;
    [[nodiscard]] const Scene& scene() const noexcept;
    [[nodiscard]] SceneManager& sceneManager() noexcept;
    [[nodiscard]] const SceneManager& sceneManager() const noexcept;
    [[nodiscard]] View& defaultView() noexcept;
    [[nodiscard]] const View& defaultView() const noexcept;

    [[nodiscard]] Result<FrameStats> render(std::uint64_t frameIndex);
    [[nodiscard]] Result<void> resize(Extent2D extent);
    // Queue a PNG readback for the next rendered frame. The copy is recorded
    // before presentation and completion is reported by FrameStats.
    [[nodiscard]] Result<void> captureScreenshot(const std::filesystem::path& path);
    [[nodiscard]] const Capabilities& capabilities() const noexcept;

private:
    struct Impl;

    friend struct Internal::EngineAccess;

    explicit Engine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon
