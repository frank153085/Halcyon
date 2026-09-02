#pragma once

// Filament uses a zero-state dummy resource to express ordering dependencies
// that do not otherwise involve a texture or buffer.  Keep the same contract
// in a backend-neutral form so it can be enabled by the graph implementation
// without introducing another native-resource type.

#include "FrameGraphTypes.h"

namespace Halcyon::Renderer::Graph
{

struct FrameGraphDummyLink
{
    struct Descriptor
    {
    };
    struct SubResourceDescriptor
    {
    };

    using Usage = ResourceUsage;
    static constexpr Usage DEFAULT_R_USAGE = Usage::None;
    static constexpr Usage DEFAULT_W_USAGE = Usage::None;

    // Kept for uniformity with FrameGraphTexture/FrameGraphBuffer.  A dummy
    // link never materializes this token.
    FrameGraphNativeResource native{};
};

} // namespace Halcyon::Renderer::Graph
