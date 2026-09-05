#pragma once

#include "../FramePacket.h"
#include "Scene.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Halcyon::Renderer::Scene::Ecs
{

class RenderExtractor final
{
public:
    struct InstanceDelta
    {
        Entity entity{};
        InstanceData instance{};
    };

    struct Delta
    {
        std::uint64_t frameIndex = 0;
        CameraData camera{};
        std::vector<InstanceDelta> created;
        std::vector<InstanceDelta> updated;
        std::vector<Entity> destroyed;

        [[nodiscard]] bool empty() const noexcept
        {
            return created.empty() && updated.empty() && destroyed.empty();
        }
    };

    // Stateful extractor used by incremental render submission. The first
    // call reports all current renderables as created; subsequent calls only
    // report additions, value changes, and removals.
    [[nodiscard]] Delta extractDelta(
        const Scene& scene, const CameraData& camera, std::uint64_t frameIndex = 0);

    void resetDeltaState() noexcept
    {
        previousInstances_.clear();
        lastRenderableRevision_ = 0;
        validationFrame_ = 0;
        hasState_ = false;
    }

    // Convenience overload for callers that keep extractor state externally.
    [[nodiscard]] static Delta extractDelta(const Scene& scene,
        const CameraData& camera,
        std::uint64_t frameIndex,
        RenderExtractor& state)
    {
        return state.extractDelta(scene, camera, frameIndex);
    }

    [[nodiscard]] static OwnedFramePacket extract(
        const Scene& scene, const CameraData& camera, std::uint64_t frameIndex = 0);

private:
    std::unordered_map<Entity, InstanceData, Entity::Hasher> previousInstances_;
    std::uint64_t lastRenderableRevision_ = 0;
    std::uint32_t validationFrame_ = 0;
    bool hasState_ = false;
};

} // namespace Halcyon::Renderer::Scene::Ecs
