#pragma once
#include "Halcyon/FrameGraph.h"
namespace Halcyon::Renderer::Graph
{
class FrameGraphCompiler
{
public:
    static FrameGraph& compile(FrameGraph& graph) noexcept
    {
        return graph.compile();
    }
};
} // namespace Halcyon::Renderer::Graph
