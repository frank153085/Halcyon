#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#include "Halcyon/Window.h"

#include "Core/Log.h"
#include "WindowInternal.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace Halcyon::Platform
{
namespace
{

[[nodiscard]] std::size_t keyIndex(Key key) noexcept
{
    return static_cast<std::size_t>(key);
}

[[nodiscard]] std::size_t mouseIndex(MouseButton button) noexcept
{
    return static_cast<std::size_t>(button);
}

[[nodiscard]] Key mapKey(int key) noexcept
{
    switch (key)
    {
        case GLFW_KEY_ESCAPE:
            return Key::Escape;
        case GLFW_KEY_W:
            return Key::W;
        case GLFW_KEY_A:
            return Key::A;
        case GLFW_KEY_S:
            return Key::S;
        case GLFW_KEY_D:
            return Key::D;
        case GLFW_KEY_Q:
            return Key::Q;
        case GLFW_KEY_E:
            return Key::E;
        default:
            return Key::Count;
    }
}

[[nodiscard]] MouseButton mapMouseButton(int button) noexcept
{
    switch (button)
    {
        case GLFW_MOUSE_BUTTON_LEFT:
            return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:
            return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            return MouseButton::Middle;
        default:
            return MouseButton::Count;
    }
}

void glfwErrorCallback(int error, const char* description)
{
    HALCYON_LOG_ERROR(
        "GLFW error ", error, ": ", description != nullptr ? description : "unknown error");
}

} // namespace

struct WindowState
{
    InputSnapshot input{};
    Extent2D framebufferExtent{};
    double previousCursorX = 0.0;
    double previousCursorY = 0.0;
    bool cursorInitialized = false;
};

struct Window::Impl
{
    GLFWwindow* handle = nullptr;
    WindowState state{};
    bool glfwInitialized = false;
};

namespace
{

void onKey(GLFWwindow* handle, int key, int /*scancode*/, int action, int /*mods*/)
{
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(handle));
    if (state == nullptr)
    {
        return;
    }
    const Key mapped = mapKey(key);
    if (mapped == Key::Count)
    {
        return;
    }
    const std::size_t index = keyIndex(mapped);
    if (action == GLFW_PRESS)
    {
        if (!state->input.keyDown[index])
        {
            state->input.keyPressed[index] = true;
        }
        state->input.keyDown[index] = true;
    }
    else if (action == GLFW_RELEASE)
    {
        state->input.keyDown[index] = false;
        state->input.keyReleased[index] = true;
    }
}

void onMouseButton(GLFWwindow* handle, int button, int action, int /*mods*/)
{
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(handle));
    if (state == nullptr)
    {
        return;
    }
    const MouseButton mapped = mapMouseButton(button);
    if (mapped == MouseButton::Count)
    {
        return;
    }
    const std::size_t index = mouseIndex(mapped);
    if (action == GLFW_PRESS)
    {
        if (!state->input.mouseDown[index])
        {
            state->input.mousePressed[index] = true;
        }
        state->input.mouseDown[index] = true;
    }
    else if (action == GLFW_RELEASE)
    {
        state->input.mouseDown[index] = false;
        state->input.mouseReleased[index] = true;
    }
}

void onCursorPosition(GLFWwindow* handle, double x, double y)
{
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(handle));
    if (state == nullptr)
    {
        return;
    }
    if (state->cursorInitialized)
    {
        state->input.cursorDeltaX += x - state->previousCursorX;
        state->input.cursorDeltaY += y - state->previousCursorY;
    }
    state->previousCursorX = x;
    state->previousCursorY = y;
    state->cursorInitialized = true;
    state->input.cursorX = x;
    state->input.cursorY = y;
}

void onFramebufferSize(GLFWwindow* handle, int width, int height)
{
    auto* state = static_cast<WindowState*>(glfwGetWindowUserPointer(handle));
    if (state != nullptr)
    {
        state->framebufferExtent = {width > 0 ? static_cast<std::uint32_t>(width) : 0u,
            height > 0 ? static_cast<std::uint32_t>(height) : 0u};
        state->input.minimized = width <= 0 || height <= 0;
    }
}

} // namespace

Window::Window(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
{
}

Window::~Window()
{
    if (impl_ == nullptr)
    {
        return;
    }
    if (impl_->handle != nullptr)
    {
        glfwSetWindowUserPointer(impl_->handle, nullptr);
        glfwDestroyWindow(impl_->handle);
    }
    if (impl_->glfwInitialized)
    {
        glfwTerminate();
    }
}

Result<std::unique_ptr<Window>> Window::create(const WindowConfig& config)
{
    if (config.initialExtent.empty())
    {
        return Result<std::unique_ptr<Window>>::failure(MakeError(
            ErrorCode::InvalidArgument, "window extent must be non-zero", "Window::create"));
    }
    glfwSetErrorCallback(glfwErrorCallback);
    if (glfwInit() != GLFW_TRUE)
    {
        return Result<std::unique_ptr<Window>>::failure(
            MakeError(ErrorCode::Backend, "GLFW initialization failed", "Window::create"));
    }
    if (glfwVulkanSupported() != GLFW_TRUE)
    {
        glfwTerminate();
        return Result<std::unique_ptr<Window>>::failure(MakeError(ErrorCode::Unsupported,
            "the active driver/loader does not support Vulkan",
            "Window::create"));
    }
    auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl{});
    if (impl == nullptr)
    {
        glfwTerminate();
        return Result<std::unique_ptr<Window>>::failure(
            MakeError(ErrorCode::OutOfMemory, "window state allocation failed", "Window::create"));
    }
    impl->glfwInitialized = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
    impl->handle = glfwCreateWindow(static_cast<int>(config.initialExtent.width),
        static_cast<int>(config.initialExtent.height),
        config.title.c_str(),
        nullptr,
        nullptr);
    if (impl->handle == nullptr)
    {
        glfwTerminate();
        return Result<std::unique_ptr<Window>>::failure(
            MakeError(ErrorCode::Backend, "window creation failed", "Window::create"));
    }
    glfwSetWindowUserPointer(impl->handle, &impl->state);
    glfwSetKeyCallback(impl->handle, onKey);
    glfwSetMouseButtonCallback(impl->handle, onMouseButton);
    glfwSetCursorPosCallback(impl->handle, onCursorPosition);
    glfwSetFramebufferSizeCallback(impl->handle, onFramebufferSize);
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(impl->handle, &framebufferWidth, &framebufferHeight);
    impl->state.framebufferExtent = {
        framebufferWidth > 0 ? static_cast<std::uint32_t>(framebufferWidth) : 0u,
        framebufferHeight > 0 ? static_cast<std::uint32_t>(framebufferHeight) : 0u};
    impl->state.input.minimized = impl->state.framebufferExtent.empty();
    Window* window = new (std::nothrow) Window(std::move(impl));
    if (window == nullptr)
    {
        glfwTerminate();
        return Result<std::unique_ptr<Window>>::failure(
            MakeError(ErrorCode::OutOfMemory, "window allocation failed", "Window::create"));
    }
    return Result<std::unique_ptr<Window>>::success(std::unique_ptr<Window>(window));
}

void Window::pollEvents() noexcept
{
    if (impl_ == nullptr || impl_->handle == nullptr)
    {
        return;
    }
    impl_->state.input.keyPressed.fill(false);
    impl_->state.input.keyReleased.fill(false);
    impl_->state.input.mousePressed.fill(false);
    impl_->state.input.mouseReleased.fill(false);
    impl_->state.input.cursorDeltaX = 0.0;
    impl_->state.input.cursorDeltaY = 0.0;
    glfwPollEvents();
    impl_->state.input.windowCloseRequested = glfwWindowShouldClose(impl_->handle) != GLFW_FALSE;
    impl_->state.input.minimized = impl_->state.framebufferExtent.empty();
}

bool Window::shouldClose() const noexcept
{
    return impl_ == nullptr || impl_->handle == nullptr ||
           glfwWindowShouldClose(impl_->handle) != GLFW_FALSE;
}

void Window::requestClose() noexcept
{
    if (impl_ != nullptr && impl_->handle != nullptr)
    {
        glfwSetWindowShouldClose(impl_->handle, GLFW_TRUE);
        impl_->state.input.windowCloseRequested = true;
    }
}

Extent2D Window::framebufferExtent() const noexcept
{
    return impl_ != nullptr ? impl_->state.framebufferExtent : Extent2D{};
}

const InputSnapshot& Window::input() const noexcept
{
    static const InputSnapshot empty{};
    return impl_ != nullptr ? impl_->state.input : empty;
}

void Window::waitEventsTimeout(double seconds) noexcept
{
    if (impl_ != nullptr && impl_->handle != nullptr)
    {
        glfwWaitEventsTimeout(std::max(0.0, seconds));
    }
}

void Window::setTitle(const std::string& title) noexcept
{
    if (impl_ != nullptr && impl_->handle != nullptr)
    {
        glfwSetWindowTitle(impl_->handle, title.c_str());
    }
}

GLFWwindow* Internal::WindowAccess::nativeHandle(Window& window) noexcept
{
    return window.impl_ != nullptr ? window.impl_->handle : nullptr;
}

} // namespace Halcyon::Platform
