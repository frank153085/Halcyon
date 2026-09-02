#pragma once

#include "Core/Result.h"
#include "Engine.h"
#include "Window.h"

#include <cstdint>
#include <functional>

namespace Halcyon
{

struct FrameInfo
{
    std::uint64_t frameIndex = 0;
    double deltaSeconds = 0.0;
    double elapsedSeconds = 0.0;
    Extent2D framebufferExtent{};
    Platform::InputSnapshot input{};
    bool minimized = false;
};

struct ApplicationCallbacks
{
    std::function<Result<void>(Engine&)> onInitialize;
    std::function<Result<void>(Engine&, const FrameInfo&)> onFrame;
    std::function<void(Engine&)> onShutdown;
};

struct ApplicationConfig
{
    Platform::WindowConfig window{};
    EngineConfig engine{};
    std::uint64_t frameLimit = 0;
    bool enableDiagnostics = false;
};

class Application final
{
public:
    [[nodiscard]] static int run(
        int argc, char** argv, ApplicationConfig config, ApplicationCallbacks callbacks);
};

} // namespace Halcyon
