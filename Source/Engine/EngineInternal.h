#pragma once

#include "Halcyon/Engine.h"

namespace Halcyon::Vulkan
{
class Renderer;
}

namespace Halcyon::Internal
{

struct EngineAccess
{
    [[nodiscard]] static Vulkan::Renderer* renderer(Engine& engine) noexcept;
};

} // namespace Halcyon::Internal
