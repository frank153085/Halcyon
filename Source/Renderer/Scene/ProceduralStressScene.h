#pragma once

#include "StaticSceneLoader.h"

#include <cstddef>
#include <cstdint>

namespace Halcyon::Renderer::Scene
{

struct ProceduralStressSceneConfig
{
    std::size_t instanceCount = 100'000;
    float gridSpacing = 3.0f;
    std::string baseMeshName = "unit-cube";
};

// Builds a deterministic grid of independent rigid instances. Geometry is
// duplicated per primitive intentionally: this exercises the same ECS and
// upload path as file-backed scenes while keeping the generator self-contained.
[[nodiscard]] StaticScene makeProceduralStressScene(
    const ProceduralStressSceneConfig& config = {});

} // namespace Halcyon::Renderer::Scene
