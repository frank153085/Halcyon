#pragma once

#include <cstddef>

namespace Halcyon::Core
{

struct MemoryStats
{
    std::size_t allocationCount = 0;
    std::size_t deallocationCount = 0;
    std::size_t currentBytes = 0;
    std::size_t peakBytes = 0;

    void recordAllocation(std::size_t bytes) noexcept
    {
        ++allocationCount;
        currentBytes += bytes;
        if (currentBytes > peakBytes)
        {
            peakBytes = currentBytes;
        }
    }

    void recordDeallocation(std::size_t bytes) noexcept
    {
        ++deallocationCount;
        currentBytes = bytes > currentBytes ? 0 : currentBytes - bytes;
    }
};

} // namespace Halcyon::Core
