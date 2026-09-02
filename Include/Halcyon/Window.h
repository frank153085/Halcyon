#pragma once

#include "Core/Result.h"
#include "RenderTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace Halcyon::Platform
{

namespace Internal
{
struct WindowAccess;
}

enum class Key : std::uint8_t
{
    Escape,
    W,
    A,
    S,
    D,
    Q,
    E,
    Count,
};

enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle,
    Count,
};

struct InputSnapshot
{
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyDown{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyPressed{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyReleased{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> mouseDown{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> mousePressed{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> mouseReleased{};
    double cursorX = 0.0;
    double cursorY = 0.0;
    double cursorDeltaX = 0.0;
    double cursorDeltaY = 0.0;
    bool windowCloseRequested = false;
    bool minimized = false;

    [[nodiscard]] bool isKeyDown(Key key) const noexcept
    {
        return keyDown[static_cast<std::size_t>(key)];
    }
    [[nodiscard]] bool wasKeyPressed(Key key) const noexcept
    {
        return keyPressed[static_cast<std::size_t>(key)];
    }
    [[nodiscard]] bool wasKeyReleased(Key key) const noexcept
    {
        return keyReleased[static_cast<std::size_t>(key)];
    }
    [[nodiscard]] bool isMouseDown(MouseButton button) const noexcept
    {
        return mouseDown[static_cast<std::size_t>(button)];
    }
    [[nodiscard]] bool wasMousePressed(MouseButton button) const noexcept
    {
        return mousePressed[static_cast<std::size_t>(button)];
    }
    [[nodiscard]] bool wasMouseReleased(MouseButton button) const noexcept
    {
        return mouseReleased[static_cast<std::size_t>(button)];
    }
};

struct WindowConfig
{
    std::string title = "Halcyon";
    Extent2D initialExtent{};
    bool resizable = true;
};

class Window final
{
public:
    static Result<std::unique_ptr<Window>> create(const WindowConfig& config = {});
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void pollEvents() noexcept;
    [[nodiscard]] bool shouldClose() const noexcept;
    void requestClose() noexcept;
    [[nodiscard]] Extent2D framebufferExtent() const noexcept;
    [[nodiscard]] const InputSnapshot& input() const noexcept;
    void waitEventsTimeout(double seconds) noexcept;
    void setTitle(const std::string& title) noexcept;

private:
    struct Impl;

    friend struct Internal::WindowAccess;

    explicit Window(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace Halcyon::Platform

namespace Halcyon
{
using WindowConfig = Platform::WindowConfig;
using InputSnapshot = Platform::InputSnapshot;
using Key = Platform::Key;
using MouseButton = Platform::MouseButton;
} // namespace Halcyon
