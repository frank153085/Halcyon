#pragma once

#include "Entity.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <tsl/robin_map.h>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Scene::Ecs
{

// Dense storage keeps iteration cache-friendly while the map provides O(1)
// entity lookup.  Removing a component uses swap-remove, so instance indices
// are intentionally not stable across removal.
template <typename Component>
class DenseComponentManager
{
public:
    static constexpr std::uint32_t kInvalidInstance = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] std::uint32_t add(Entity entity, const Component& component = {})
    {
        if (!entity.isValid())
        {
            return kInvalidInstance;
        }

        const auto existing = entityToInstance_.find(entity);
        if (existing != entityToInstance_.end())
        {
            components_[existing->second] = component;
            return existing->second;
        }

        const auto instance = static_cast<std::uint32_t>(components_.size());
        entities_.push_back(entity);
        components_.push_back(component);
        entityToInstance_.emplace(entity, instance);
        return instance;
    }

    [[nodiscard]] bool remove(Entity entity) noexcept
    {
        const auto found = entityToInstance_.find(entity);
        if (found == entityToInstance_.end())
        {
            return false;
        }

        const std::uint32_t removed = found->second;
        const std::uint32_t last = static_cast<std::uint32_t>(components_.size() - 1u);
        if (removed != last)
        {
            entities_[removed] = entities_[last];
            components_[removed] = std::move(components_[last]);
            entityToInstance_[entities_[removed]] = removed;
        }
        entities_.pop_back();
        components_.pop_back();
        entityToInstance_.erase(found);
        return true;
    }

    [[nodiscard]] bool has(Entity entity) const noexcept
    {
        return entityToInstance_.find(entity) != entityToInstance_.end();
    }

    [[nodiscard]] std::uint32_t instance(Entity entity) const noexcept
    {
        const auto found = entityToInstance_.find(entity);
        return found == entityToInstance_.end() ? kInvalidInstance : found->second;
    }

    [[nodiscard]] Component* get(Entity entity) noexcept
    {
        const auto instanceIndex = instance(entity);
        return instanceIndex == kInvalidInstance ? nullptr : &components_[instanceIndex];
    }

    [[nodiscard]] const Component* get(Entity entity) const noexcept
    {
        const auto instanceIndex = instance(entity);
        return instanceIndex == kInvalidInstance ? nullptr : &components_[instanceIndex];
    }

    [[nodiscard]] std::span<const Entity> entities() const noexcept
    {
        return entities_;
    }

    [[nodiscard]] std::span<const Component> components() const noexcept
    {
        return components_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return components_.size();
    }

    void clear() noexcept
    {
        entityToInstance_.clear();
        entities_.clear();
        components_.clear();
    }

private:
    std::vector<Entity> entities_;
    std::vector<Component> components_;
    tsl::robin_map<Entity, std::uint32_t, Entity::Hasher> entityToInstance_;
};

} // namespace Halcyon::Renderer::Scene::Ecs
