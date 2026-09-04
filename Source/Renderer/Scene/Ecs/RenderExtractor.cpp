#include "RenderExtractor.h"

#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Halcyon::Renderer::Scene::Ecs
{

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
            InstanceData instance{};
            static_assert(sizeof(glm::mat4) == sizeof(instance.transform));
            std::memcpy(instance.transform.data(),
                glm::value_ptr(transform->worldTransform),
                sizeof(instance.transform));
            instance.meshId = renderable->mesh.isValid() ? renderable->mesh.index() : 0u;
            instance.materialId =
                renderable->material.isValid() ? renderable->material.index() : 0u;
            instance.flags = renderable->flags;
            packet.instances.push_back(instance);
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
