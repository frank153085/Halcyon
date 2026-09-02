#include "Scene.h"

#include <algorithm>

namespace Halcyon::Renderer::Scene::Ecs
{

Entity Scene::createEntity()
{
    const Entity entity = entityManager_.create();
    if (entity.isValid())
    {
        members_.push_back(entity);
    }
    return entity;
}

void Scene::destroyEntity(Entity entity) noexcept
{
    if (!entityManager_.isAlive(entity))
    {
        return;
    }
    remove(entity);
    (void)transforms_.remove(entity);
    (void)renderables_.remove(entity);
    (void)lights_.remove(entity);
    entityManager_.destroy(entity);
}

void Scene::add(Entity entity)
{
    if (!entityManager_.isAlive(entity) || contains(entity))
    {
        return;
    }
    members_.push_back(entity);
}

void Scene::remove(Entity entity) noexcept
{
    const auto found = std::find(members_.begin(), members_.end(), entity);
    if (found != members_.end())
    {
        *found = members_.back();
        members_.pop_back();
    }
}

bool Scene::contains(Entity entity) const noexcept
{
    return std::find(members_.begin(), members_.end(), entity) != members_.end();
}

void Scene::updateTransforms()
{
    transforms_.updateWorldTransforms();
}

void Scene::clear() noexcept
{
    members_.clear();
    transforms_.clear();
    renderables_.clear();
    lights_.clear();
    entityManager_.clear();
}

} // namespace Halcyon::Renderer::Scene::Ecs
