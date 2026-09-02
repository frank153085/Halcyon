#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace Halcyon::Renderer::Scene::Ecs
{

// A compact, generation-checked identity used by the scene ECS.  Index zero
// and generation zero are reserved for the invalid entity.
class Entity final
{
public:
    constexpr Entity() noexcept = default;
    constexpr Entity(std::uint32_t indexValue, std::uint32_t generationValue) noexcept
            : index_(indexValue),
              generation_(generationValue)
    {
    }

    [[nodiscard]] static constexpr Entity invalid() noexcept
    {
        return {};
    }

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return index_ != 0 && generation_ != 0;
    }

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return isValid();
    }

    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }

    [[nodiscard]] constexpr std::uint32_t index() const noexcept
    {
        return index_;
    }

    [[nodiscard]] constexpr std::uint32_t generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] constexpr std::uint64_t packed() const noexcept
    {
        return (static_cast<std::uint64_t>(generation_) << 32u) | index_;
    }

    friend constexpr bool operator==(Entity, Entity) noexcept = default;
    friend constexpr auto operator<=>(Entity, Entity) noexcept = default;

    struct Hasher
    {
        [[nodiscard]] std::size_t operator()(Entity value) const noexcept
        {
            const std::size_t indexHash = static_cast<std::size_t>(value.index());
            const std::size_t generationHash = static_cast<std::size_t>(value.generation());
            return indexHash ^ (generationHash + static_cast<std::size_t>(0x9e3779b9u) +
                                   (indexHash << 6u) + (indexHash >> 2u));
        }
    };

private:
    std::uint32_t index_ = 0;
    std::uint32_t generation_ = 0;
};

} // namespace Halcyon::Renderer::Scene::Ecs

namespace std
{
template <>
struct hash<Halcyon::Renderer::Scene::Ecs::Entity>
{
    [[nodiscard]] std::size_t operator()(Halcyon::Renderer::Scene::Ecs::Entity value) const noexcept
    {
        return Halcyon::Renderer::Scene::Ecs::Entity::Hasher{}(value);
    }
};
} // namespace std
