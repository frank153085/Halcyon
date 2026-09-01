#pragma once

#include "LinearAllocator.h"

#include <cstddef>
#include <vector>

namespace Halcyon::Core
{

class FrameArena final
{
public:
    FrameArena() = default;
    FrameArena(std::size_t frameCount, std::size_t bytesPerFrame);

    void initialize(std::size_t frameCount, std::size_t bytesPerFrame);
    [[nodiscard]] void* allocate(std::size_t frameIndex,
        std::size_t size,
        std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void reset(std::size_t frameIndex) noexcept;
    void resetAll() noexcept;

    [[nodiscard]] std::size_t frameCount() const noexcept
    {
        return arenas_.size();
    }

private:
    std::vector<LinearAllocator> arenas_;
};

} // namespace Halcyon::Core
