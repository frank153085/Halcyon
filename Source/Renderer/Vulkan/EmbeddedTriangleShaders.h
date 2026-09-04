#pragma once

#include <array>
#include <cstdint>

namespace Halcyon::Vulkan::EmbeddedShaders
{

extern const std::array<std::uint32_t, 273> kTriangleVertexSpirv;
extern const std::array<std::uint32_t, 122> kTriangleFragmentSpirv;
extern const std::array<std::uint32_t, 390> kSceneVertexSpirv;
extern const std::array<std::uint32_t, 260> kSceneFragmentSpirv;

} // namespace Halcyon::Vulkan::EmbeddedShaders
