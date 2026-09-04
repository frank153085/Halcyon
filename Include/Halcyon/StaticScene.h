#pragma once

#include "Renderer/Scene/StaticSceneLoader.h"

namespace Halcyon
{
using StaticScene = Renderer::Scene::StaticScene;
using StaticSceneMaterial = Renderer::Scene::StaticSceneMaterial;
using StaticSceneNode = Renderer::Scene::StaticSceneNode;
using StaticScenePrimitive = Renderer::Scene::StaticScenePrimitive;
using StaticSceneVertex = Renderer::Scene::StaticSceneVertex;
using StaticSceneLoadOptions = Renderer::Scene::StaticSceneLoadOptions;
using FastGltfSceneLoader = Renderer::Scene::FastGltfSceneLoader;
using Renderer::Scene::loadGltfScene;
using Renderer::Scene::loadStaticScene;
using SceneTexture = Renderer::Scene::SceneTexture;
} // namespace Halcyon
