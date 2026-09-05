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

enum class RenderableFlags : std::uint32_t
{
    None = 0,
    Transparent = 1u << 0u,
    DoubleSided = 1u << 1u,
    CastShadow = 1u << 2u,
    ReceiveShadow = 1u << 3u,
    AlphaMasked = 1u << 4u,
};

[[nodiscard]] constexpr std::uint32_t operator|(RenderableFlags lhs, RenderableFlags rhs) noexcept
{
    return static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs);
}

[[nodiscard]] constexpr bool hasFlag(std::uint32_t value, RenderableFlags flag) noexcept
{
    return (value & static_cast<std::uint32_t>(flag)) != 0u;
}

class RenderableManager final
{
public:
    [[nodiscard]] std::uint32_t add(Entity entity, const RenderableComponent& component = {})
    {
        const auto instance = storage_.add(entity, component);
        if (instance != DenseComponentManager<RenderableComponent>::kInvalidInstance)
            ++revision_;
        return instance;
    }
    [[nodiscard]] bool remove(Entity entity) noexcept
    {
        const bool removed = storage_.remove(entity);
        if (removed) ++revision_;
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
    [[nodiscard]] RenderableComponent* get(Entity entity) noexcept
    {
        RenderableComponent* component = storage_.get(entity);
        // Mutable access is a write-intent signal, matching TransformManager.
        // This makes direct mesh/material/flag edits visible to extractDelta.
        if (component != nullptr) ++revision_;
        return component;
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
        ++revision_;
    }

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    DenseComponentManager<RenderableComponent> storage_;
    std::uint64_t revision_ = 0;
};

} // namespace Halcyon::Renderer::Scene::Ecs
