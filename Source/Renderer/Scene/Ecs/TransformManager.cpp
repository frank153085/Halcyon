#include "TransformManager.h"

#include <functional>

namespace Halcyon::Renderer::Scene::Ecs
{

void TransformManager::updateWorldTransforms()
{
    // 0 = unvisited, 1 = visiting, 2 = resolved.  A cycle is treated as a
    // root at the point where it is detected, keeping the update finite and
    // deterministic while still producing useful transforms.
    tsl::robin_map<Entity, std::uint8_t, Entity::Hasher> state;
    state.reserve(storage_.size());

    std::function<void(Entity)> resolve = [&](Entity entity)
    {
        TransformComponent* component = storage_.get(entity);
        if (component == nullptr)
        {
            return;
        }

        auto [stateIt, inserted] = state.emplace(entity, 0u);
        (void)inserted;
        if (stateIt->second == 2u)
        {
            return;
        }
        if (stateIt->second == 1u)
        {
            component->worldTransform = component->localTransform;
            component->dirty = false;
            stateIt.value() = 2u;
            return;
        }

        stateIt.value() = 1u;
        if (component->parent.isValid() && storage_.has(component->parent) &&
            component->parent != entity)
        {
            resolve(component->parent);
            if (const TransformComponent* parent = storage_.get(component->parent))
            {
                component->worldTransform = parent->worldTransform * component->localTransform;
            }
            else
            {
                component->worldTransform = component->localTransform;
            }
        }
        else
        {
            component->worldTransform = component->localTransform;
        }
        component->dirty = false;
        stateIt.value() = 2u;
    };

    for (const Entity entity : storage_.entities())
    {
        resolve(entity);
    }
}

} // namespace Halcyon::Renderer::Scene::Ecs
