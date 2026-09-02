#pragma once

#include "../../Resources/ResourceTypes.h"
#include "DenseComponentManager.h"

namespace Halcyon::Renderer::Scene::Ecs
{

struct RenderableComponent
{
    Resources::MeshHandle mesh{};
    Resources::MaterialHandle material{};
    std::uint32_t flags = 0;
};

class RenderableManager final
{
public:
    [[nodiscard]] std::uint32_t add(Entity entity, const RenderableComponent& component = {})
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
    [[nodiscard]] RenderableComponent* get(Entity entity) noexcept
    {
        return storage_.get(entity);
    }
    [[nodiscard]] const RenderableComponent* get(Entity entity) const noexcept
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
    void clear() noexcept
    {
        storage_.clear();
    }

private:
    DenseComponentManager<RenderableComponent> storage_;
};

} // namespace Halcyon::Renderer::Scene::Ecs
