#pragma once

// Filament-style naming for the backend-neutral frame graph.  The canonical
// class is FrameGraph; RenderGraph remains an alias in RenderGraph.h so
// existing callers remain source compatible during the migration.

#include "RenderGraph.h"

namespace Halcyon::Renderer::Graph
{

using FrameGraphBuilder = PassBuilder;

} // namespace Halcyon::Renderer::Graph
