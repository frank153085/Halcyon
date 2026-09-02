#include "details/Resource.h"

#include "details/PassNode.h"

namespace Halcyon::Renderer::Graph
{

VirtualResource::~VirtualResource() noexcept = default;

void VirtualResource::neededByPass(PassNode* pass) noexcept
{
    if (pass == nullptr)
    {
        return;
    }

    ++refcount;
    if (first == nullptr)
    {
        first = pass;
    }
    last = pass;

    // A subresource keeps its parent alive for the same lifetime.  Extensions
    // should avoid cyclic parent chains; the self/null checks keep malformed
    // declarations from recursing forever.
    if (parent != nullptr && parent != this)
    {
        parent->neededByPass(pass);
    }
}

} // namespace Halcyon::Renderer::Graph

