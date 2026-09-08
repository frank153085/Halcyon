#pragma once

#include "StaticSceneLoader.h"

namespace Halcyon::Renderer::Scene
{

[[nodiscard]] Halcyon::Result<StaticScene> loadPlyScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options = {});

} // namespace Halcyon::Renderer::Scene
