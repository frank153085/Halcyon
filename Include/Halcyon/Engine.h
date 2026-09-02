#pragma once

#include "Core/Result.h"
#include "RenderTypes.h"
#include "Scene.h"
#include "View.h"

#include <cstdint>
#include <memory>

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
    const char* startupTexturePath = nullptr;
    const char* startupMeshPath = nullptr;
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
    [[nodiscard]] View& defaultView() noexcept;
    [[nodiscard]] const View& defaultView() const noexcept;

    [[nodiscard]] Result<FrameStats> render(std::uint64_t frameIndex);
    [[nodiscard]] Result<void> resize(Extent2D extent);
    [[nodiscard]] const Capabilities& capabilities() const noexcept;

private:
    struct Impl;

    friend struct Internal::EngineAccess;

    explicit Engine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon
