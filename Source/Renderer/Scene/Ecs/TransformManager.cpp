#include "TransformManager.h"

#include <functional>

namespace Halcyon::Renderer::Scene::Ecs
{

void TransformManager::updateWorldTransforms()
{
    updatedEntities_.clear();
    if (!hasDirtyTransforms_) return;
    if (updatedEntities_.capacity() < storage_.size())
        updatedEntities_.reserve(storage_.size());
    // Dense indices avoid a per-frame hash allocation.  0 = unvisited,
    // 1 = visiting, 2 = resolved; parentChanged propagates dirty transforms
    // to descendants even when only the parent was edited.
    state_.assign(storage_.size(), 0u);
    parentChanged_.assign(storage_.size(), 0u);

    std::function<bool(std::uint32_t)> resolve = [&](std::uint32_t index) -> bool
    {
        if (index >= storage_.size()) return false;
        if (state_[index] == 2u) return parentChanged_[index] != 0u;
        TransformComponent& component = storage_.components()[index];
        if (state_[index] == 1u)
        {
            component.worldTransform = component.localTransform;
            component.dirty = false;
            state_[index] = 2u;
            parentChanged_[index] = 1u;
            updatedEntities_.push_back(storage_.entities()[index]);
            return true;
        }
        state_[index] = 1u;
        bool inheritedChange = false;
        std::uint32_t parentIndex = DenseComponentManager<TransformComponent>::kInvalidInstance;
        if (component.parent.isValid() && component.parent != storage_.entities()[index])
        {
            parentIndex = storage_.instance(component.parent);
            if (parentIndex != DenseComponentManager<TransformComponent>::kInvalidInstance)
            {
                // Break a malformed parent cycle at the edge that reaches an
                // already-visiting node. Every component is still resolved
                // exactly once and remains usable as a local root.
                if (state_[parentIndex] == 1u)
                    parentIndex = DenseComponentManager<TransformComponent>::kInvalidInstance;
                else
                    inheritedChange = resolve(parentIndex);
            }
        }
        const bool changed = component.dirty || inheritedChange;
        if (changed)
        {
            if (parentIndex == DenseComponentManager<TransformComponent>::kInvalidInstance)
                component.worldTransform = component.localTransform;
            else
                component.worldTransform = storage_.components()[parentIndex].worldTransform *
                    component.localTransform;
            updatedEntities_.push_back(storage_.entities()[index]);
        }
        component.dirty = false;
        state_[index] = 2u;
        parentChanged_[index] = changed ? 1u : 0u;
        return changed;
    };
    for (std::uint32_t index = 0; index < storage_.size(); ++index) (void)resolve(index);
    hasDirtyTransforms_ = false;
}

} // namespace Halcyon::Renderer::Scene::Ecs
