#pragma once

#include "DenseComponentManager.h"

#include <glm/glm.hpp>

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
        return storage_.add(entity, component);
    }
    [[nodiscard]] bool remove(Entity entity) noexcept
    {
        return storage_.remove(entity);
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
        return storage_.get(entity);
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

    void updateWorldTransforms();
    void clear() noexcept
    {
        storage_.clear();
    }

private:
    DenseComponentManager<TransformComponent> storage_;
};

} // namespace Halcyon::Renderer::Scene::Ecs
