#pragma once

#include <cstddef>
#include <vector>

namespace Halcyon::Core
{

class LinearAllocator final
{
public:
    explicit LinearAllocator(std::size_t capacity = 0);

    [[nodiscard]] void* allocate(
        std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void reset() noexcept;
    void reserve(std::size_t capacity);

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return storage_.size();
    }
    [[nodiscard]] std::size_t used() const noexcept
    {
        return offset_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return capacity() - offset_;
    }

private:
    std::vector<std::byte> storage_;
    std::size_t offset_ = 0;
};

} // namespace Halcyon::Core
