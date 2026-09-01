#include "HeapAllocator.h"

#include <new>

namespace Halcyon::Core
{

void* HeapAllocator::allocate(std::size_t size, std::size_t alignment) noexcept
{
    const std::size_t actualSize = size == 0 ? 1 : size;
    if (alignment < alignof(void*) || (alignment & (alignment - 1u)) != 0)
    {
        return nullptr;
    }
    try
    {
        void* pointer = ::operator new(actualSize, std::align_val_t(alignment));
        stats_.recordAllocation(actualSize);
        return pointer;
    }
    catch (...)
    {
        return nullptr;
    }
}

void HeapAllocator::deallocate(void* pointer, std::size_t size, std::size_t alignment) noexcept
{
    if (pointer == nullptr)
    {
        return;
    }
    const std::size_t actualSize = size == 0 ? 1 : size;
    if (alignment < alignof(void*) || (alignment & (alignment - 1u)) != 0)
    {
        alignment = alignof(std::max_align_t);
    }
    ::operator delete(pointer, std::align_val_t(alignment));
    stats_.recordDeallocation(actualSize);
}

} // namespace Halcyon::Core
