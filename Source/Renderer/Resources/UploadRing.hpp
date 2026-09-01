#pragma once

// A small persistently mapped upload-ring model.  The class is backend
// neutral on purpose: Vulkan's mapped pointer can be supplied by replacing the
// byte storage in a future allocator, while all wrap-around and timeline
// lifetime rules remain testable on the CPU.

#include "../../Core/Result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Resources
{

struct UploadAllocation
{
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint64_t retireValue = 0;
    std::span<std::byte> bytes{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return size != 0 && !bytes.empty();
    }
};

class UploadRing
{
public:
    explicit UploadRing(std::size_t capacityBytes = 4u * 1024u * 1024u)
            : storage_(std::max<std::size_t>(capacityBytes, 1u))
    {
    }

    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;
    UploadRing(UploadRing&&) = delete;
    UploadRing& operator=(UploadRing&&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return storage_.size();
    }
    [[nodiscard]] std::size_t usedBytes() const noexcept
    {
        return static_cast<std::size_t>(head_ - tail_);
    }
    [[nodiscard]] std::size_t freeBytes() const noexcept
    {
        return capacity() - std::min(usedBytes(), capacity());
    }
    [[nodiscard]] std::size_t pendingAllocations() const noexcept
    {
        return inFlight_.size();
    }
    [[nodiscard]] std::size_t frameUploadedBytes() const noexcept
    {
        return frameBytes_;
    }
    [[nodiscard]] std::size_t frameBudgetBytes() const noexcept
    {
        return frameBudget_;
    }
    [[nodiscard]] std::size_t frameRemainingBytes() const noexcept
    {
        return frameBytes_ >= frameBudget_ ? 0u : frameBudget_ - frameBytes_;
    }

    // Limit work submitted by one frame.  This is independent from ring
    // capacity and is useful for preventing a large asset upload from
    // starving rendering.
    void beginFrame(
        std::size_t uploadBudgetBytes = std::numeric_limits<std::size_t>::max()) noexcept
    {
        frameBytes_ = 0;
        frameBudget_ = uploadBudgetBytes;
    }

    // Retire allocations whose GPU timeline value has completed.  Values are
    // expected to be submitted monotonically, as Vulkan timeline semaphores
    // guarantee.
    void collect(std::uint64_t completedTimelineValue) noexcept
    {
        completedTimelineValue_ = std::max(completedTimelineValue_, completedTimelineValue);
        while (!inFlight_.empty() && inFlight_.front().retireValue <= completedTimelineValue_)
        {
            tail_ = std::max(tail_, inFlight_.front().end);
            inFlight_.pop_front();
        }
        if (inFlight_.empty())
        {
            // Keep cursors bounded and make a completely idle ring intuitive
            // to inspect in a debugger.
            tail_ = head_;
        }
    }

    [[nodiscard]] Core::Result<UploadAllocation> allocate(
        std::size_t size, std::size_t alignment = 16u, std::uint64_t retireValue = 0u)
    {
        if (size == 0u)
        {
            return Core::Result<UploadAllocation>::failure(
                Core::MakeError(Core::ErrorCode::InvalidArgument, "upload size must be non-zero"));
        }
        if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
        {
            return Core::Result<UploadAllocation>::failure(Core::MakeError(
                Core::ErrorCode::InvalidArgument, "upload alignment must be a power of two"));
        }
        if (size > capacity())
        {
            return Core::Result<UploadAllocation>::failure(Core::MakeError(
                Core::ErrorCode::OutOfMemory, "upload does not fit in ring capacity"));
        }
        if (size > frameRemainingBytes())
        {
            return Core::Result<UploadAllocation>::failure(
                Core::MakeError(Core::ErrorCode::Timeout, "per-frame upload budget exhausted"));
        }

        const std::uint64_t aligned = alignUp(head_, alignment);
        const std::uint64_t capacity64 = static_cast<std::uint64_t>(capacity());
        // Do not split one upload over the physical end of the ring.  Consume
        // the padding and start at offset zero on the next lap.
        const std::uint64_t offset = aligned % capacity64;
        const std::uint64_t candidate = offset + static_cast<std::uint64_t>(size) > capacity64
                                            ? aligned + (capacity64 - offset)
                                            : aligned;
        const std::uint64_t end = candidate + static_cast<std::uint64_t>(size);
        if (end - tail_ > capacity64)
        {
            return Core::Result<UploadAllocation>::failure(Core::MakeError(
                Core::ErrorCode::Timeout, "upload ring is still in use by the GPU"));
        }

        // A zero retire value means "retire on the next collect(0)" and is
        // convenient for CPU-only users.  The renderer passes its actual
        // submitted timeline value for asynchronous uploads.
        inFlight_.push_back(InFlight{candidate, end, retireValue});
        head_ = end;
        frameBytes_ += size;
        const auto byteOffset = static_cast<std::size_t>(candidate % capacity64);
        UploadAllocation allocation;
        allocation.offset = byteOffset;
        allocation.size = size;
        allocation.retireValue = retireValue;
        allocation.bytes = std::span<std::byte>(storage_.data() + byteOffset, size);
        return Core::Result<UploadAllocation>::success(std::move(allocation));
    }

    template <typename T>
    [[nodiscard]] Core::Result<UploadAllocation> write(std::span<const T> source,
        std::size_t alignment = alignof(T),
        std::uint64_t retireValue = 0u)
    {
        const auto allocation = allocate(source.size_bytes(), alignment, retireValue);
        if (!allocation)
        {
            return allocation;
        }
        std::copy_n(reinterpret_cast<const std::byte*>(source.data()),
            source.size_bytes(),
            allocation.value().bytes.data());
        return allocation;
    }

private:
    struct InFlight
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t retireValue = 0;
    };

    [[nodiscard]] static std::uint64_t alignUp(std::uint64_t value, std::size_t alignment) noexcept
    {
        const auto mask = static_cast<std::uint64_t>(alignment - 1u);
        return (value + mask) & ~mask;
    }

    std::vector<std::byte> storage_;
    std::deque<InFlight> inFlight_;
    std::uint64_t head_ = 0;
    std::uint64_t tail_ = 0;
    std::uint64_t completedTimelineValue_ = 0;
    std::size_t frameBytes_ = 0;
    std::size_t frameBudget_ = std::numeric_limits<std::size_t>::max();
};

} // namespace Halcyon::Renderer::Resources
