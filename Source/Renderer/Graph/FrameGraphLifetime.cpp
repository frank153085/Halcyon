#include "RenderGraph.h"

namespace Halcyon::Renderer::Graph
{

void FrameGraph::analyzeResourceLifetimes(CompileResult& result) const
{
    // Culling has already been applied to executionOrder, so dead passes do
    // not artificially extend transient resource allocations.
    for (std::int32_t position = 0;
        position < static_cast<std::int32_t>(result.executionOrder.size());
        ++position)
    {
        const auto passHandle = result.executionOrder[static_cast<std::size_t>(position)];
        const auto* pass = result.pass(passHandle);
        if (pass == nullptr)
        {
            continue;
        }
        for (const auto& access : pass->accesses)
        {
            if (access.resourceIndex >= result.resources.size())
            {
                continue;
            }
            auto& lifetime = result.resources[access.resourceIndex].lifetime;
            if (lifetime.firstUse < 0)
            {
                lifetime.firstUse = position;
                lifetime.firstPass = passHandle;
            }
            lifetime.lastUse = position;
            lifetime.lastPass = passHandle;
        }
    }
}

} // namespace Halcyon::Renderer::Graph
