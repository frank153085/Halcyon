#pragma once
#include "Halcyon/FrameGraph.h"
namespace Halcyon::Renderer::Graph
{
struct ResourceNode
{
    FrameGraphHandle handle{};
    ResourceKind kind = ResourceKind::Buffer;
    std::int32_t producer = -1;
};
} // namespace Halcyon::Renderer::Graph
