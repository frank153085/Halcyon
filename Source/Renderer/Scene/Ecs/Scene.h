#pragma once

#include "EntityManager.h"
#include "LightManager.h"
#include "RenderableManager.h"
#include "TransformManager.h"

#include <span>
#include <vector>

namespace Halcyon::Renderer::Scene::Ecs
{

// Owns scene membership and the small set of component managers used by the
// first ECS implementation.  All operations are single-threaded by design.
class Scene final
{
public:
    [[nodiscard]] Entity createEntity();
    void destroyEntity(Entity entity) noexcept;

    // Membership is separate from entity lifetime so callers can stage an
    // entity before adding it to a particular scene.
    void add(Entity entity);
    void remove(Entity entity) noexcept;
    [[nodiscard]] bool contains(Entity entity) const noexcept;
    [[nodiscard]] std::span<const Entity> members() const noexcept
    {
        return members_;
    }

    [[nodiscard]] EntityManager& entities() noexcept
    {
        return entityManager_;
    }
    [[nodiscard]] const EntityManager& entities() const noexcept
    {
        return entityManager_;
    }
    [[nodiscard]] TransformManager& transforms() noexcept
    {
        return transforms_;
    }
    [[nodiscard]] const TransformManager& transforms() const noexcept
    {
        return transforms_;
    }
    [[nodiscard]] RenderableManager& renderables() noexcept
    {
        return renderables_;
    }
    [[nodiscard]] const RenderableManager& renderables() const noexcept
    {
        return renderables_;
    }
    [[nodiscard]] LightManager& lights() noexcept
    {
        return lights_;
    }
    [[nodiscard]] const LightManager& lights() const noexcept
    {
        return lights_;
    }

    void updateTransforms();
    void clear() noexcept;

private:
    EntityManager entityManager_;
    TransformManager transforms_;
    RenderableManager renderables_;
    LightManager lights_;
    std::vector<Entity> members_;
};

} // namespace Halcyon::Renderer::Scene::Ecs
