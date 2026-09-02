#include "RenderGraph.h"

#include <deque>

namespace Halcyon::Renderer::Graph
{

std::vector<bool> FrameGraph::cullPasses(
    const CompileOptions& options, const DependencyGraph& cullingDependencies) const
{
    const auto passCount = passes_.size();
    std::vector<bool> live(passCount, false);
    std::deque<std::uint32_t> work;
    for (std::uint32_t i = 0; i < passCount; ++i)
    {
        if (passes_[i].alive && (!options.cullDeadPasses || passes_[i].sideEffect))
        {
            live[i] = true;
            work.push_back(i);
        }
    }

    if (options.cullDeadPasses)
    {
        // Exporting a resource roots its latest writer.  Reverse traversal of
        // data dependencies then keeps every producer needed by that writer.
        for (std::uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex)
        {
            const auto& resource = resources_[resourceIndex];
            if (!resource.alive || !resource.exported)
            {
                continue;
            }
            std::int32_t latestWriter = -1;
            for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
            {
                if (!passes_[passIndex].alive)
                {
                    continue;
                }
                for (const auto& access : passes_[passIndex].accesses)
                {
                    if (access.resourceIndex == resourceIndex &&
                        access.resourceGeneration == resource.generation &&
                        access.kind == resource.kind && access.writes())
                    {
                        latestWriter = static_cast<std::int32_t>(passIndex);
                    }
                }
            }
            if (latestWriter >= 0 && !live[static_cast<std::uint32_t>(latestWriter)])
            {
                live[static_cast<std::uint32_t>(latestWriter)] = true;
                work.push_back(static_cast<std::uint32_t>(latestWriter));
            }
        }
        while (!work.empty())
        {
            const auto node = work.front();
            work.pop_front();
            for (const auto predecessor : cullingDependencies.predecessors(node))
            {
                if (!live[predecessor])
                {
                    live[predecessor] = true;
                    work.push_back(predecessor);
                }
            }
        }
    }
    else
    {
        for (std::uint32_t i = 0; i < passCount; ++i)
        {
            live[i] = passes_[i].alive;
        }
    }
    return live;
}

} // namespace Halcyon::Renderer::Graph
