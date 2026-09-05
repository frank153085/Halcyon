#include "Renderer/Scene/GpuScene.h"

#include <iostream>

int main()
{
    Halcyon::Renderer::Scene::GpuSceneSlotAllocator slots(2);
    const auto first = slots.allocate();
    const auto second = slots.allocate();
    if (first == Halcyon::Renderer::Scene::GpuSceneSlotAllocator::invalidSlot ||
        second == Halcyon::Renderer::Scene::GpuSceneSlotAllocator::invalidSlot ||
        slots.allocate() != Halcyon::Renderer::Scene::GpuSceneSlotAllocator::invalidSlot)
        return 1;
    if (!slots.release(first, 4) || slots.collect(3) != 0 || slots.collect(4) != 1)
        return 2;
    const auto reused = slots.allocate();
    if (reused != first)
        return 3;
    const auto bounds = Halcyon::Renderer::Scene::computeWorldBounds(
        {-1, -1, -1}, {1, 1, 1}, glm::mat4(1.0f));
    if (bounds.sphereCenterRadius[3] < 1.7f)
        return 4;
    Halcyon::Renderer::Scene::GpuSceneState state(4);
    Halcyon::Renderer::Scene::Ecs::Entity entity{1, 1};
    Halcyon::Renderer::Scene::InstanceData instance{};
    if (!state.applyCreated(entity, instance) || state.slot(entity) ==
            Halcyon::Renderer::Scene::GpuSceneSlotAllocator::invalidSlot ||
        !state.applyUpdated(entity, instance) || !state.applyDestroyed(entity, 2) ||
        state.soa().bounds[state.dirtyRanges().back().first].sphereCenterRadius[3] >= 0.0f ||
        state.collect(1) != 0 || state.collect(2) != 1)
        return 5;

    // CPU scene storage grows geometrically without invalidating live slots.
    Halcyon::Renderer::Scene::GpuSceneState growing(1);
    const Halcyon::Renderer::Scene::Ecs::Entity growA{2, 1};
    const Halcyon::Renderer::Scene::Ecs::Entity growB{3, 1};
    if (!growing.applyCreated(growA, instance) || !growing.applyCreated(growB, instance) ||
        growing.slot(growA) != 0u || growing.slot(growB) != 1u ||
        growing.soa().transforms.size() < 2u || growing.dirtyRanges().size() != 1u ||
        growing.dirtyRanges()[0].first != 0u || growing.dirtyRanges()[0].count != 2u)
        return 6;
    std::cout << "GPU scene tests passed\n";
    return 0;
}
