#pragma once

// Public scene facade.  The first implementation intentionally reuses the
// dense, generation-checked ECS storage while hiding its internal namespace.

#include "Renderer/Scene/Ecs/Ecs.h"

namespace Halcyon
{

using Entity = Renderer::Scene::Ecs::Entity;
using Scene = Renderer::Scene::Ecs::Scene;
using TransformComponent = Renderer::Scene::Ecs::TransformComponent;
using RenderableComponent = Renderer::Scene::Ecs::RenderableComponent;
using LightComponent = Renderer::Scene::Ecs::LightComponent;
using LightType = Renderer::Scene::Ecs::LightType;

} // namespace Halcyon
