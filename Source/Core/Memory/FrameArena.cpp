#include "FrameArena.h"

namespace Halcyon::Core
{

FrameArena::FrameArena(std::size_t frameCount, std::size_t bytesPerFrame)
{
    initialize(frameCount, bytesPerFrame);
}

void FrameArena::initialize(std::size_t frameCount, std::size_t bytesPerFrame)
{
    arenas_.clear();
    arenas_.reserve(frameCount);
    for (std::size_t index = 0; index < frameCount; ++index)
    {
        arenas_.emplace_back(bytesPerFrame);
    }
}

void* FrameArena::allocate(std::size_t frameIndex, std::size_t size, std::size_t alignment) noexcept
{
    return frameIndex < arenas_.size() ? arenas_[frameIndex].allocate(size, alignment) : nullptr;
}

void FrameArena::reset(std::size_t frameIndex) noexcept
{
    if (frameIndex < arenas_.size())
    {
        arenas_[frameIndex].reset();
    }
}

void FrameArena::resetAll() noexcept
{
    for (LinearAllocator& arena : arenas_)
    {
        arena.reset();
    }
}

} // namespace Halcyon::Core
