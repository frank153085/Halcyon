#pragma once

#include "MemoryStats.h"

#include <cstddef>

namespace Halcyon::Core
{

class HeapAllocator final
{
public:
    HeapAllocator() noexcept = default;
    ~HeapAllocator() = default;

    HeapAllocator(const HeapAllocator&) = delete;
    HeapAllocator& operator=(const HeapAllocator&) = delete;

    [[nodiscard]] void* allocate(
        std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void* pointer,
        std::size_t size = 0,
        std::size_t alignment = alignof(std::max_align_t)) noexcept;

    [[nodiscard]] const MemoryStats& stats() const noexcept
    {
        return stats_;
    }
    void resetStats() noexcept
    {
        stats_ = {};
    }

private:
    MemoryStats stats_{};
};

} // namespace Halcyon::Core
