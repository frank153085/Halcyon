#pragma once

// Public scene facade.  The first implementation intentionally reuses the
// dense, generation-checked ECS storage while hiding its internal namespace.

#include "Renderer/Scene/Ecs/Ecs.h"
#include "Renderer/Scene/SceneDatabase.h"
#include "Renderer/Scene/StaticSceneLoader.h"
#include "Renderer/Scene/ProceduralStressScene.h"

namespace Halcyon
{

using Entity = Renderer::Scene::Ecs::Entity;
using Scene = Renderer::Scene::Ecs::Scene;
using TransformComponent = Renderer::Scene::Ecs::TransformComponent;
using RenderableComponent = Renderer::Scene::Ecs::RenderableComponent;
using RenderableFlags = Renderer::Scene::Ecs::RenderableFlags;
using LightComponent = Renderer::Scene::Ecs::LightComponent;
using LightType = Renderer::Scene::Ecs::LightType;
using StaticScene = Renderer::Scene::StaticScene;
using StaticSceneMaterial = Renderer::Scene::StaticSceneMaterial;
using StaticSceneNode = Renderer::Scene::StaticSceneNode;
using StaticScenePrimitive = Renderer::Scene::StaticScenePrimitive;
using StaticSceneVertex = Renderer::Scene::StaticSceneVertex;
using StaticSceneLoadOptions = Renderer::Scene::StaticSceneLoadOptions;
using FastGltfSceneLoader = Renderer::Scene::FastGltfSceneLoader;
using Renderer::Scene::loadGltfScene;
using Renderer::Scene::loadStaticScene;
using SceneDatabase = Renderer::Scene::SceneDatabase;
using SceneMesh = Renderer::Scene::SceneMesh;
using SceneMaterial = Renderer::Scene::SceneMaterial;
using SceneTexture = Renderer::Scene::SceneTexture;
using SceneImportResult = Renderer::Scene::SceneImportResult;
using SceneMeshHandle = Renderer::Resources::MeshHandle;
using SceneMaterialHandle = Renderer::Resources::MaterialHandle;
using SceneTextureHandle = Renderer::Resources::TextureHandle;
using ProceduralStressSceneConfig = Renderer::Scene::ProceduralStressSceneConfig;
using Renderer::Scene::makeProceduralStressScene;

} // namespace Halcyon
