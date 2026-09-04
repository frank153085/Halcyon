#pragma once

#include "DenseComponentManager.h"

#include <glm/glm.hpp>

namespace Halcyon::Renderer::Scene::Ecs
{

enum class LightType : std::uint8_t
{
    Point,
    Directional,
    Spot,
};

struct LightComponent
{
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float innerConeCos = 0.9f;
    float outerConeCos = 0.8f;
};

class LightManager final
{
public:
    [[nodiscard]] std::uint32_t add(Entity entity, const LightComponent& component = {})
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
    [[nodiscard]] const LightComponent* get(Entity entity) const noexcept
    {
        return storage_.get(entity);
    }
    [[nodiscard]] LightComponent* get(Entity entity) noexcept
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
    DenseComponentManager<LightComponent> storage_;
};

} // namespace Halcyon::Renderer::Scene::Ecs
