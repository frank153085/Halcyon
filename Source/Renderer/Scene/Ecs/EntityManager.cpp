#include "EntityManager.h"

#include <limits>

namespace Halcyon::Renderer::Scene::Ecs
{

EntityManager::EntityManager()
{
    // Slot zero remains permanently reserved for the invalid entity.
    generations_.push_back(0);
    alive_.push_back(0);
}

Entity EntityManager::create()
{
    std::uint32_t index = 0;
    if (!freeIndices_.empty())
    {
        index = freeIndices_.back();
        freeIndices_.pop_back();
    }
    else
    {
        index = static_cast<std::uint32_t>(generations_.size());
        if (index == 0 || index == std::numeric_limits<std::uint32_t>::max())
        {
            return {};
        }
        generations_.push_back(1);
        alive_.push_back(0);
    }

    if (generations_[index] == 0)
    {
        generations_[index] = 1;
    }
    alive_[index] = 1;
    ++aliveCount_;
    return Entity{index, generations_[index]};
}

void EntityManager::destroy(Entity entity) noexcept
{
    if (!isAlive(entity))
    {
        return;
    }

    alive_[entity.index()] = 0;
    std::uint32_t nextGeneration = generations_[entity.index()] + 1u;
    if (nextGeneration == 0)
    {
        nextGeneration = 1;
    }
    generations_[entity.index()] = nextGeneration;
    freeIndices_.push_back(entity.index());
    --aliveCount_;
}

bool EntityManager::isAlive(Entity entity) const noexcept
{
    return entity.isValid() && entity.index() < generations_.size() &&
           alive_[entity.index()] != 0 && generations_[entity.index()] == entity.generation();
}

void EntityManager::clear() noexcept
{
    freeIndices_.clear();
    for (std::uint32_t index = 1; index < generations_.size(); ++index)
    {
        std::uint32_t nextGeneration = generations_[index] + 1u;
        if (nextGeneration == 0)
        {
            nextGeneration = 1;
        }
        generations_[index] = nextGeneration;
        alive_[index] = 0;
        freeIndices_.push_back(index);
    }
    aliveCount_ = 0;
}

} // namespace Halcyon::Renderer::Scene::Ecs
