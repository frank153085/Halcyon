#pragma once

#include "Halcyon/Window.h"

struct GLFWwindow;

namespace Halcyon::Platform::Internal
{

struct WindowAccess
{
    [[nodiscard]] static GLFWwindow* nativeHandle(Window& window) noexcept;
};

} // namespace Halcyon::Platform::Internal
