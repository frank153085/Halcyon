#include "LinearAllocator.h"

#include <cstdint>

namespace Halcyon::Core
{

LinearAllocator::LinearAllocator(std::size_t capacity)
        : storage_(capacity)
{
}

void* LinearAllocator::allocate(std::size_t size, std::size_t alignment) noexcept
{
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0)
    {
        return nullptr;
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(storage_.data());
    const std::uintptr_t current = base + offset_;
    const std::uintptr_t aligned = (current + alignment - 1u) & ~(alignment - 1u);
    const std::size_t padding = static_cast<std::size_t>(aligned - current);
    if (padding > storage_.size() - offset_ || size > storage_.size() - offset_ - padding)
    {
        return nullptr;
    }
    offset_ += padding;
    void* result = storage_.data() + offset_;
    offset_ += size;
    return result;
}

void LinearAllocator::reset() noexcept
{
    offset_ = 0;
}

void LinearAllocator::reserve(std::size_t capacity)
{
    if (capacity > storage_.size())
    {
        storage_.resize(capacity);
        offset_ = 0;
    }
}

} // namespace Halcyon::Core
