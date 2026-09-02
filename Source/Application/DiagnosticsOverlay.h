#pragma once

#include "Core/Result.h"
#include "Halcyon/Engine.h"
#include "Halcyon/Window.h"

#include <cstdint>

namespace Halcyon::ApplicationInternal
{

class DiagnosticsOverlay final
{
public:
    struct Impl;

    DiagnosticsOverlay() noexcept = default;
    ~DiagnosticsOverlay();

    DiagnosticsOverlay(const DiagnosticsOverlay&) = delete;
    DiagnosticsOverlay& operator=(const DiagnosticsOverlay&) = delete;

    [[nodiscard]] Result<void> initialize(Platform::Window& window, Engine& engine) noexcept;
    void beginFrame(const FrameStats& stats,
        std::uint64_t frameIndex,
        const Capabilities& capabilities) noexcept;
    void endFrame() noexcept;
    void shutdown() noexcept;

private:
    Impl* impl_ = nullptr;
};

} // namespace Halcyon::ApplicationInternal
