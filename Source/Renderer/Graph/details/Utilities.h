#pragma once

// Shared private aliases matching Filament's fg/details/Utilities.h while
// reusing Halcyon's existing arena implementation.

#include "../FrameGraphArena.h"

#include <memory>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Graph
{

using FrameGraphAllocator = FrameGraphArena;

template <typename T>
using Allocator = std::allocator<T>;

template <typename T>
using Vector = std::vector<T, Allocator<T>>;

template <typename T>
using UniquePtr = std::unique_ptr<T>;

template <typename T, typename... Args>
T* make(FrameGraphAllocator& arena, Args&&... args)
{
    return arena.make<T>(std::forward<Args>(args)...);
}

} // namespace Halcyon::Renderer::Graph

