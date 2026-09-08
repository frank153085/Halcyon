#pragma once

#include "FramePassContext.h"

namespace Halcyon::Vulkan
{
void addVirtualGeometryPasses(Graph::FrameGraph& graph, FramePassContext& ctx);
}
