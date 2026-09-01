#include "Core/Memory/FrameArena.h"
#include "Core/Memory/HeapAllocator.h"
#include "Core/Memory/LinearAllocator.h"

#include <cassert>
#include <cstdint>

int main()
{
    Halcyon::Core::HeapAllocator heap;
    void* first = heap.allocate(32, 32);
    assert(first != nullptr);
    assert(reinterpret_cast<std::uintptr_t>(first) % 32 == 0);
    heap.deallocate(first, 32, 32);
    assert(heap.stats().currentBytes == 0);

    Halcyon::Core::LinearAllocator linear(64);
    assert(linear.allocate(1, 16) != nullptr);
    assert(linear.allocate(128) == nullptr);
    linear.reset();
    assert(linear.used() == 0);

    Halcyon::Core::FrameArena arena(2, 64);
    assert(arena.allocate(0, 16) != nullptr);
    assert(arena.allocate(3, 1) == nullptr);
    arena.reset(0);
    assert(arena.allocate(0, 64) != nullptr);
    return 0;
}
