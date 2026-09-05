#pragma once

#include "DenseComponentManager.h"

#include <glm/glm.hpp>
#include <vector>

namespace Halcyon::Renderer::Scene::Ecs
{

struct TransformComponent
{
    Entity parent{};
    glm::mat4 localTransform{1.0f};
    glm::mat4 worldTransform{1.0f};
    bool dirty = true;
};

class TransformManager final
{
public:
    [[nodiscard]] std::uint32_t add(Entity entity, const TransformComponent& component = {})
    {
        hasDirtyTransforms_ = true;
        return storage_.add(entity, component);
    }
    [[nodiscard]] bool remove(Entity entity) noexcept
    {
        const bool removed = storage_.remove(entity);
        if (removed)
        {
            // Parent removal can turn any descendant into a local root.
            // Hierarchy edits are rare, so conservatively invalidate all
            // remaining transforms instead of maintaining a second graph.
            for (auto& component : storage_.components()) component.dirty = true;
            hasDirtyTransforms_ = true;
        }
        return removed;
    }
    [[nodiscard]] bool has(Entity entity) const noexcept
    {
        return storage_.has(entity);
    }
    [[nodiscard]] std::uint32_t instance(Entity entity) const noexcept
    {
        return storage_.instance(entity);
    }
    [[nodiscard]] TransformComponent* get(Entity entity) noexcept
    {
        TransformComponent* component = storage_.get(entity);
        // Mutable access is a write-intent signal. This keeps the delta
        // extractor correct for callers that edit localTransform directly.
        if (component != nullptr)
        {
            component->dirty = true;
            hasDirtyTransforms_ = true;
        }
        return component;
    }
    [[nodiscard]] const TransformComponent* get(Entity entity) const noexcept
    {
        return storage_.get(entity);
    }
    [[nodiscard]] std::span<const Entity> entities() const noexcept
    {
        return storage_.entities();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return storage_.size();
    }

    [[nodiscard]] std::span<const Entity> updatedEntities() const noexcept
    {
        return updatedEntities_;
    }

    void updateWorldTransforms();
    void clear() noexcept
    {
        storage_.clear();
        updatedEntities_.clear();
        state_.clear();
        parentChanged_.clear();
        hasDirtyTransforms_ = false;
    }

private:
    DenseComponentManager<TransformComponent> storage_;
    std::vector<Entity> updatedEntities_;
    std::vector<std::uint8_t> state_;
    std::vector<std::uint8_t> parentChanged_;
    bool hasDirtyTransforms_ = false;
};

} // namespace Halcyon::Renderer::Scene::Ecs
