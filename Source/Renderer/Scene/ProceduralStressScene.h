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

// Builds a deterministic grid of independent rigid instances sharing one
// immutable primitive. This keeps the stress case focused on instance,
// upload, and visibility throughput instead of duplicating vertex data.
[[nodiscard]] StaticScene makeProceduralStressScene(
    const ProceduralStressSceneConfig& config = {});

} // namespace Halcyon::Renderer::Scene
