#pragma once

// CPU-side bindless descriptor bookkeeping.  No Vulkan (or other graphics
// API) types appear here: a backend only needs to mirror the returned slot
// index and write its native descriptor when a handle is allocated/updated.

#include "../../Core/Handle.h"
#include "../../Core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Halcyon::Renderer::Resources
{

struct DescriptorSlotTag
{
};
using DescriptorHandle = Halcyon::Core::Handle<DescriptorSlotTag>;

enum class DescriptorType : std::uint8_t
{
    SampledImage = 0,
    StorageImage,
    UniformBuffer,
    StorageBuffer,
    Sampler,
    Count,
};

inline constexpr std::size_t kDescriptorTypeCount = static_cast<std::size_t>(DescriptorType::Count);

// Optional type-carrying wrapper for call sites that want compile/runtime
// protection against passing a sampled-image handle to a storage-buffer slot.
// The underlying generation-checked DescriptorHandle remains available for
// low-level allocators and compact FramePacket storage.
struct BindlessHandle
{
    DescriptorType type = DescriptorType::Count;
    DescriptorHandle slot{};

    [[nodiscard]] bool valid() const noexcept
    {
        return type != DescriptorType::Count && slot.valid();
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }
    [[nodiscard]] std::uint32_t index() const noexcept
    {
        return slot.index();
    }
    [[nodiscard]] std::uint32_t generation() const noexcept
    {
        return slot.generation();
    }
    [[nodiscard]] DescriptorHandle descriptor() const noexcept
    {
        return slot;
    }

    friend constexpr bool operator==(
        const BindlessHandle&, const BindlessHandle&) noexcept = default;
};

[[nodiscard]] constexpr const char* toString(DescriptorType type) noexcept
{
    switch (type)
    {
        case DescriptorType::SampledImage:
            return "sampled image";
        case DescriptorType::StorageImage:
            return "storage image";
        case DescriptorType::UniformBuffer:
            return "uniform buffer";
        case DescriptorType::StorageBuffer:
            return "storage buffer";
        case DescriptorType::Sampler:
            return "sampler";
        case DescriptorType::Count:
            break;
    }
    return "unknown descriptor type";
}

enum class DescriptorSlotState : std::uint8_t
{
    Default,
    Free,
    Live,
    PendingRelease,
};

struct DescriptorSlotAllocatorConfig
{
    // Capacity includes slot zero.  A capacity of one is valid and gives a
    // table containing only the default descriptor.
    std::uint32_t capacity = 1024;
    bool reserveDefaultSlot = true;
};

class DescriptorSlotAllocator
{
public:
    DescriptorSlotAllocator() = default;
    explicit DescriptorSlotAllocator(std::uint32_t capacity, bool reserveDefaultSlot = true);
    explicit DescriptorSlotAllocator(DescriptorSlotAllocatorConfig config);

    [[nodiscard]] Halcyon::Result<void> initialize(DescriptorSlotAllocatorConfig config);
    [[nodiscard]] Halcyon::Result<void> initialize(
        std::uint32_t capacity, bool reserveDefaultSlot = true)
    {
        return initialize(DescriptorSlotAllocatorConfig{capacity, reserveDefaultSlot});
    }

    // Slot zero is reserved for the default descriptor and is available for
    // the table's lifetime.  It is never returned by allocate().
    [[nodiscard]] DescriptorHandle defaultHandle() const noexcept;
    [[nodiscard]] bool hasDefaultSlot() const noexcept;

    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocate();
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocate(std::uint64_t completedTimeline);
    [[nodiscard]] Halcyon::Result<void> release(
        DescriptorHandle handle, std::uint64_t retireTimeline);
    [[nodiscard]] Halcyon::Result<void> touch(
        DescriptorHandle handle, std::uint64_t submittedTimeline);
    [[nodiscard]] Halcyon::Result<void> markUsed(
        DescriptorHandle handle, std::uint64_t submittedTimeline)
    {
        return touch(handle, submittedTimeline);
    }

    // Reclaims pending slots whose retire timeline has completed.  If the
    // optional output is supplied, it receives the handles (with their old
    // generations) that became reusable during this call.
    [[nodiscard]] std::size_t collect(
        std::uint64_t completedTimeline, std::vector<DescriptorHandle>* reclaimed = nullptr);

    [[nodiscard]] bool contains(DescriptorHandle handle) const noexcept;
    [[nodiscard]] bool valid(DescriptorHandle handle) const noexcept
    {
        return contains(handle);
    }
    [[nodiscard]] bool pending(DescriptorHandle handle) const noexcept;
    [[nodiscard]] DescriptorSlotState state(DescriptorHandle handle) const noexcept;
    [[nodiscard]] std::uint64_t retireTimeline(DescriptorHandle handle) const noexcept;
    [[nodiscard]] std::uint64_t lastUseTimeline(DescriptorHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return static_cast<std::uint32_t>(slots_.size());
    }
    [[nodiscard]] std::uint32_t slotCount() const noexcept
    {
        return capacity();
    }
    // liveCount includes the default slot when it is reserved.
    [[nodiscard]] std::uint32_t liveCount() const noexcept
    {
        return liveCount_;
    }
    [[nodiscard]] std::uint32_t userLiveCount() const noexcept
    {
        return liveCount_ - (hasDefaultSlot() ? 1u : 0u);
    }
    [[nodiscard]] std::uint32_t pendingCount() const noexcept
    {
        return pendingCount_;
    }
    [[nodiscard]] std::uint32_t availableCount() const noexcept
    {
        return static_cast<std::uint32_t>(freeSlots_.size());
    }
    [[nodiscard]] std::uint64_t completedTimeline() const noexcept
    {
        return completedTimeline_;
    }
    [[nodiscard]] bool initialized() const noexcept
    {
        return !slots_.empty();
    }

    // Invalidates all user handles and rebuilds the free list.  The default
    // slot (if configured) remains reserved with its current generation.
    void clear() noexcept;
    // Release all bookkeeping storage.  This is useful when a device is
    // torn down and the table may later be re-created with a different
    // descriptor capacity.
    void shutdown() noexcept;

private:
    struct Slot
    {
        std::uint32_t generation = 1;
        DescriptorSlotState state = DescriptorSlotState::Free;
        std::uint64_t retireTimeline = 0;
        std::uint64_t lastUseTimeline = 0;
    };

    [[nodiscard]] bool validIndex(DescriptorHandle handle) const noexcept;
    [[nodiscard]] static std::uint32_t nextGeneration(std::uint32_t generation) noexcept;
    [[nodiscard]] Halcyon::Core::Error invalidHandleError() const;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::vector<std::uint32_t> pendingSlots_;
    std::uint32_t liveCount_ = 0;
    std::uint32_t pendingCount_ = 0;
    std::uint64_t completedTimeline_ = 0;
    bool reserveDefaultSlot_ = true;
};

// A compact payload that is sufficient for a backend to identify a resource
// and view.  It intentionally contains no native handles.
struct DescriptorValue
{
    std::uint64_t resourceId = 0;
    std::uint64_t viewId = 0;
    std::uint32_t flags = 0;
    std::uint32_t _padding = 0;

    friend constexpr bool operator==(
        const DescriptorValue&, const DescriptorValue&) noexcept = default;
};

struct BindlessTableConfig
{
    std::uint32_t sampledImageCapacity = 4096;
    std::uint32_t storageImageCapacity = 1024;
    std::uint32_t uniformBufferCapacity = 1024;
    std::uint32_t storageBufferCapacity = 4096;
    std::uint32_t samplerCapacity = 256;
};

class BindlessTable
{
public:
    BindlessTable() = default;
    explicit BindlessTable(BindlessTableConfig config);

    [[nodiscard]] Halcyon::Result<void> initialize(BindlessTableConfig config);

    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocate(
        DescriptorType type, DescriptorValue value = {}, std::uint64_t completedTimeline = 0);
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateSampledImage(
        DescriptorValue value = {}, std::uint64_t completedTimeline = 0)
    {
        return allocate(DescriptorType::SampledImage, value, completedTimeline);
    }
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateStorageImage(
        DescriptorValue value = {}, std::uint64_t completedTimeline = 0)
    {
        return allocate(DescriptorType::StorageImage, value, completedTimeline);
    }
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateUniformBuffer(
        DescriptorValue value = {}, std::uint64_t completedTimeline = 0)
    {
        return allocate(DescriptorType::UniformBuffer, value, completedTimeline);
    }
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateStorageBuffer(
        DescriptorValue value = {}, std::uint64_t completedTimeline = 0)
    {
        return allocate(DescriptorType::StorageBuffer, value, completedTimeline);
    }
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateSampler(
        DescriptorValue value = {}, std::uint64_t completedTimeline = 0)
    {
        return allocate(DescriptorType::Sampler, value, completedTimeline);
    }

    [[nodiscard]] Halcyon::Result<BindlessHandle> allocateTyped(
        DescriptorType type, DescriptorValue value = {}, std::uint64_t completedTimeline = 0);
    [[nodiscard]] Halcyon::Result<DescriptorHandle> allocateDefault(
        DescriptorType type, std::uint64_t completedTimeline = 0)
    {
        return allocate(type, defaultValue(type), completedTimeline);
    }
    [[nodiscard]] Halcyon::Result<BindlessHandle> allocateTypedDefault(
        DescriptorType type, std::uint64_t completedTimeline = 0)
    {
        return allocateTyped(type, defaultValue(type), completedTimeline);
    }

    [[nodiscard]] Halcyon::Result<void> update(
        DescriptorType type, DescriptorHandle handle, DescriptorValue value);
    [[nodiscard]] Halcyon::Result<void> release(
        DescriptorType type, DescriptorHandle handle, std::uint64_t retireTimeline);
    [[nodiscard]] Halcyon::Result<void> release(BindlessHandle handle, std::uint64_t retireTimeline)
    {
        return release(handle.type, handle.slot, retireTimeline);
    }
    [[nodiscard]] Halcyon::Result<void> touch(
        DescriptorType type, DescriptorHandle handle, std::uint64_t submittedTimeline);
    [[nodiscard]] Halcyon::Result<void> touch(
        BindlessHandle handle, std::uint64_t submittedTimeline)
    {
        return touch(handle.type, handle.slot, submittedTimeline);
    }
    [[nodiscard]] std::size_t collect(std::uint64_t completedTimeline);

    [[nodiscard]] Halcyon::Result<void> setDefault(DescriptorType type, DescriptorValue value);
    [[nodiscard]] DescriptorHandle defaultHandle(DescriptorType type) const noexcept;
    [[nodiscard]] BindlessHandle defaultTypedHandle(DescriptorType type) const noexcept
    {
        return BindlessHandle{type, defaultHandle(type)};
    }
    [[nodiscard]] bool contains(DescriptorType type, DescriptorHandle handle) const noexcept;
    [[nodiscard]] bool pending(DescriptorType type, DescriptorHandle handle) const noexcept;
    [[nodiscard]] std::optional<DescriptorValue> get(
        DescriptorType type, DescriptorHandle handle) const noexcept;
    [[nodiscard]] std::optional<DescriptorValue> get(BindlessHandle handle) const noexcept
    {
        return get(handle.type, handle.slot);
    }
    [[nodiscard]] Halcyon::Result<DescriptorValue> getResult(
        DescriptorType type, DescriptorHandle handle) const;

    [[nodiscard]] DescriptorSlotAllocator& allocator(DescriptorType type) noexcept;
    [[nodiscard]] const DescriptorSlotAllocator& allocator(DescriptorType type) const noexcept;
    [[nodiscard]] const DescriptorValue& defaultValue(DescriptorType type) const noexcept;
    [[nodiscard]] std::uint32_t capacity(DescriptorType type) const noexcept;
    [[nodiscard]] std::uint32_t liveCount(DescriptorType type) const noexcept;
    [[nodiscard]] std::uint32_t pendingCount(DescriptorType type) const noexcept;
    [[nodiscard]] std::uint64_t completedTimeline() const noexcept
    {
        return completedTimeline_;
    }
    [[nodiscard]] bool initialized() const noexcept
    {
        return initialized_;
    }

    void clear() noexcept;
    void shutdown() noexcept;

private:
    [[nodiscard]] static std::size_t indexOf(DescriptorType type) noexcept
    {
        return static_cast<std::size_t>(type);
    }
    [[nodiscard]] Halcyon::Core::Error invalidTypeError() const;
    [[nodiscard]] bool validType(DescriptorType type) const noexcept
    {
        return indexOf(type) < kDescriptorTypeCount;
    }

    std::array<DescriptorSlotAllocator, kDescriptorTypeCount> allocators_{};
    std::array<std::vector<DescriptorValue>, kDescriptorTypeCount> values_{};
    std::array<DescriptorValue, kDescriptorTypeCount> defaults_{};
    std::uint64_t completedTimeline_ = 0;
    bool initialized_ = false;
};

} // namespace Halcyon::Renderer::Resources
