#pragma once
#include "Halcyon/FrameGraph.h"
namespace Halcyon::Renderer::Graph
{
class FrameGraphExecutor
{
public:
    static void execute(FrameGraph& graph, CommandContext& context) noexcept
    {
        graph.execute(context);
    }
};
} // namespace Halcyon::Renderer::Graph
