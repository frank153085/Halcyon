#pragma once

#include "Entity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Halcyon::Renderer::Scene::Ecs
{

class EntityManager final
{
public:
    EntityManager();

    [[nodiscard]] Entity create();
    void destroy(Entity entity) noexcept;
    [[nodiscard]] bool isAlive(Entity entity) const noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t aliveCount() const noexcept
    {
        return aliveCount_;
    }

private:
    std::vector<std::uint32_t> generations_;
    std::vector<std::uint8_t> alive_;
    std::vector<std::uint32_t> freeIndices_;
    std::size_t aliveCount_ = 0;
};

} // namespace Halcyon::Renderer::Scene::Ecs
