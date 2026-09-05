#pragma once

#include "Renderer/Scene/GpuScene.h"

namespace Halcyon
{
using GpuSceneSlotAllocator = Renderer::Scene::GpuSceneSlotAllocator;
using GpuSceneSlot = Renderer::Scene::GpuSceneSlotAllocator::SlotHandle;
using GpuSceneSoA = Renderer::Scene::GpuSceneSoA;
using GpuSceneDirtyRange = Renderer::Scene::GpuSceneDirtyRange;
using TransformRow = Renderer::Scene::TransformRow;
using BoundsRow = Renderer::Scene::BoundsRow;
using MeshMaterialRow = Renderer::Scene::MeshMaterialRow;
using RenderPathMode = Renderer::Scene::RenderPathMode;
using Renderer::Scene::computeWorldBounds;
} // namespace Halcyon
