#pragma once

#include "../FramePacket.h"
#include "Scene.h"

#include <cstdint>

namespace Halcyon::Renderer::Scene::Ecs
{

class RenderExtractor final
{
public:
    [[nodiscard]] static OwnedFramePacket extract(
        const Scene& scene, const CameraData& camera, std::uint64_t frameIndex = 0);
};

} // namespace Halcyon::Renderer::Scene::Ecs
