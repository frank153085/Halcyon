#include "RenderExtractor.h"

#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Halcyon::Renderer::Scene::Ecs
{

namespace
{
[[nodiscard]] InstanceData instanceData(const TransformComponent& transform,
    const RenderableComponent& renderable)
{
    InstanceData instance{};
    static_assert(sizeof(glm::mat4) == sizeof(instance.transform));
    std::memcpy(instance.transform.data(),
        glm::value_ptr(transform.worldTransform),
        sizeof(instance.transform));
    instance.meshId = renderable.mesh.isValid() ? renderable.mesh.index() : 0u;
    instance.materialId = renderable.material.isValid() ? renderable.material.index() : 0u;
    instance.flags = renderable.flags;
    return instance;
}
} // namespace

RenderExtractor::Delta RenderExtractor::extractDelta(
    const Scene& scene, const CameraData& camera, std::uint64_t frameIndex)
{
    Delta delta;
    delta.frameIndex = frameIndex;
    delta.camera = camera;

    std::unordered_map<Entity, InstanceData, Entity::Hasher> current;
    current.reserve(scene.renderables().size());
    for (const Entity entity : scene.members())
    {
        if (!scene.entities().isAlive(entity))
        {
            continue;
        }
        const TransformComponent* transform = scene.transforms().get(entity);
        const RenderableComponent* renderable = scene.renderables().get(entity);
        if (transform != nullptr && renderable != nullptr)
        {
            current.emplace(entity, instanceData(*transform, *renderable));
        }
    }

    delta.created.reserve(current.size());
    delta.updated.reserve(current.size());
    for (const auto& [entity, instance] : current)
    {
        const auto previous = previousInstances_.find(entity);
        if (previous == previousInstances_.end())
        {
            delta.created.push_back({entity, instance});
        }
        else if (std::memcmp(previous->second.transform.data(),
                       instance.transform.data(),
                       sizeof(instance.transform)) != 0 ||
            previous->second.meshId != instance.meshId ||
            previous->second.materialId != instance.materialId ||
            previous->second.flags != instance.flags)
        {
            delta.updated.push_back({entity, instance});
        }
    }
    delta.destroyed.reserve(previousInstances_.size());
    for (const auto& [entity, ignored] : previousInstances_)
    {
        (void)ignored;
        if (!current.contains(entity))
        {
            delta.destroyed.push_back(entity);
        }
    }
    previousInstances_ = std::move(current);
    return delta;
}

OwnedFramePacket RenderExtractor::extract(
    const Scene& scene, const CameraData& camera, std::uint64_t frameIndex)
{
    OwnedFramePacket packet;
    packet.frameIndex = frameIndex;
    packet.camera = camera;
    packet.instances.reserve(scene.renderables().size());
    packet.lights.reserve(scene.lights().size());

    for (const Entity entity : scene.members())
    {
        if (!scene.entities().isAlive(entity))
        {
            continue;
        }

        const TransformComponent* transform = scene.transforms().get(entity);
        const RenderableComponent* renderable = scene.renderables().get(entity);
        if (renderable != nullptr && transform != nullptr)
        {
            packet.instances.push_back(instanceData(*transform, *renderable));
        }

        const LightComponent* light = scene.lights().get(entity);
        if (light != nullptr)
        {
            glm::vec3 position = light->position;
            if (transform != nullptr)
            {
                position = glm::vec3{transform->worldTransform[3]};
            }
            const float type = static_cast<float>(light->type == LightType::Directional ? 1u
                                      : (light->type == LightType::Spot ? 2u : 0u));
            packet.lights.push_back(LightData{{position.x, position.y, position.z, light->range},
                {light->color.r, light->color.g, light->color.b, light->intensity},
                {light->direction.x, light->direction.y, light->direction.z, type},
                {light->innerConeCos, light->outerConeCos, 0.0f, 0.0f}});
        }
    }

    return packet;
}

} // namespace Halcyon::Renderer::Scene::Ecs
