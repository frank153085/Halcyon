#pragma once

// Backend-neutral counterpart of Filament's ResourceCreationContext.  The
// Halcyon graph allocates through FrameGraphResourceProvider instead of a
// DriverApi, so the context carries optional graph/provider/command pointers.

#include "../FrameGraphTypes.h"

#include <string_view>

namespace Halcyon::Renderer::Graph
{

class FrameGraph;

struct ResourceCreationContext final
{
    FrameGraph* graph = nullptr;
    FrameGraphResourceProvider* provider = nullptr;
    CommandContext* commands = nullptr;
    bool useProtectedMemory = false;

    constexpr ResourceCreationContext() noexcept = default;
    constexpr ResourceCreationContext(
        FrameGraph* graph_,
        FrameGraphResourceProvider* provider_,
        CommandContext* commands_ = nullptr,
        bool useProtectedMemory_ = false) noexcept
        : graph(graph_),
          provider(provider_),
          commands(commands_),
          useProtectedMemory(useProtectedMemory_)
    {
    }

    /** True when a provider is available to materialize resources. */
    bool canCreate() const noexcept;

    /** Dispatch a provider allocation, returning false when unavailable. */
    bool create(const FrameGraphResourceCreateInfo& info,
        FrameGraphNativeResource& output) const noexcept;

    bool createRenderTarget(const FrameGraphRenderTargetCreateInfo& info,
        FrameGraphNativeResource& output) const noexcept;

    /** Dispatch provider destruction; a missing provider is a no-op. */
    void destroy(const FrameGraphNativeResource& resource) const noexcept;
    void destroyRenderTarget(const FrameGraphNativeResource& resource) const noexcept;
};

} // namespace Halcyon::Renderer::Graph
