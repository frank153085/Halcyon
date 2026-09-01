#include "BindlessTable.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <sstream>

namespace Halcyon::Renderer::Resources
{

namespace
{

[[nodiscard]] std::uint32_t nextGenerationValue(std::uint32_t generation) noexcept
{
    ++generation;
    return generation == 0 ? 1u : generation;
}

[[nodiscard]] Halcyon::Core::Error makeError(
    Halcyon::Core::ErrorCode code, std::string message, std::string context = {})
{
    return Halcyon::Core::Error{code, std::move(message), std::move(context)};
}

[[nodiscard]] std::uint32_t capacityFor(
    const BindlessTableConfig& config, DescriptorType type) noexcept
{
    switch (type)
    {
        case DescriptorType::SampledImage:
            return config.sampledImageCapacity;
        case DescriptorType::StorageImage:
            return config.storageImageCapacity;
        case DescriptorType::UniformBuffer:
            return config.uniformBufferCapacity;
        case DescriptorType::StorageBuffer:
            return config.storageBufferCapacity;
        case DescriptorType::Sampler:
            return config.samplerCapacity;
        case DescriptorType::Count:
            break;
    }
    return 0;
}

} // namespace

DescriptorSlotAllocator::DescriptorSlotAllocator(std::uint32_t capacity, bool reserveDefaultSlot)
{
    (void)initialize(capacity, reserveDefaultSlot);
}

DescriptorSlotAllocator::DescriptorSlotAllocator(DescriptorSlotAllocatorConfig config)
{
    (void)initialize(config);
}

Halcyon::Result<void> DescriptorSlotAllocator::initialize(DescriptorSlotAllocatorConfig config)
{
    if (config.capacity == 0 || config.capacity == DescriptorHandle::kInvalidIndex)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidArgument,
            "descriptor slot capacity must be between 1 and UINT32_MAX-1"));
    }
    if (!slots_.empty())
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::AlreadyExists,
            "descriptor slot allocator is already initialized"));
    }

    try
    {
        std::vector<Slot> slots(config.capacity);
        std::vector<std::uint32_t> freeSlots;
        freeSlots.reserve(config.capacity - (config.reserveDefaultSlot ? 1u : 0u));
        const std::uint32_t firstFree = config.reserveDefaultSlot ? 1u : 0u;
        for (std::uint32_t index = firstFree; index < config.capacity; ++index)
        {
            freeSlots.push_back(index);
        }
        if (config.reserveDefaultSlot)
        {
            slots[0].state = DescriptorSlotState::Default;
        }
        slots_ = std::move(slots);
        freeSlots_ = std::move(freeSlots);
        pendingSlots_.clear();
        liveCount_ = config.reserveDefaultSlot ? 1u : 0u;
        pendingCount_ = 0;
        completedTimeline_ = 0;
        reserveDefaultSlot_ = config.reserveDefaultSlot;
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure(makeError(
            Halcyon::Core::ErrorCode::OutOfMemory, "failed to allocate descriptor slot table"));
    }
    catch (const std::exception& exception)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidState,
            exception.what(),
            "descriptor slot allocator initialization"));
    }
    return Halcyon::Result<void>::success();
}

DescriptorHandle DescriptorSlotAllocator::defaultHandle() const noexcept
{
    if (!hasDefaultSlot())
    {
        return DescriptorHandle::invalid();
    }
    return DescriptorHandle{0u, slots_[0].generation};
}

bool DescriptorSlotAllocator::hasDefaultSlot() const noexcept
{
    return reserveDefaultSlot_ && !slots_.empty() &&
           slots_[0].state == DescriptorSlotState::Default;
}

Halcyon::Result<DescriptorHandle> DescriptorSlotAllocator::allocate()
{
    if (slots_.empty())
    {
        return Halcyon::Result<DescriptorHandle>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState,
                "descriptor slot allocator is not initialized"));
    }
    if (freeSlots_.empty())
    {
        return Halcyon::Result<DescriptorHandle>::failure(makeError(
            Halcyon::Core::ErrorCode::OutOfMemory, "bindless descriptor table capacity exhausted"));
    }
    const auto index = freeSlots_.back();
    freeSlots_.pop_back();
    auto& slot = slots_[index];
    slot.state = DescriptorSlotState::Live;
    slot.retireTimeline = 0;
    slot.lastUseTimeline = 0;
    ++liveCount_;
    return Halcyon::Result<DescriptorHandle>::success(DescriptorHandle{index, slot.generation});
}

Halcyon::Result<DescriptorHandle> DescriptorSlotAllocator::allocate(std::uint64_t completedTimeline)
{
    (void)collect(completedTimeline);
    return allocate();
}

Halcyon::Result<void> DescriptorSlotAllocator::release(
    DescriptorHandle handle, std::uint64_t retireTimelineValue)
{
    if (!validIndex(handle))
    {
        return Halcyon::Result<void>::failure(invalidHandleError());
    }
    const auto index = handle.index();
    auto& slot = slots_[index];
    if (hasDefaultSlot() && index == 0u)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidArgument,
            "the default descriptor slot cannot be released"));
    }
    if (slot.state != DescriptorSlotState::Live)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidState,
            "descriptor slot is not live (already released or free)"));
    }

    // Grow bookkeeping before changing the slot state.  An allocation
    // failure must not strand a PendingRelease slot outside pendingSlots_.
    try
    {
        pendingSlots_.push_back(index);
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure(makeError(
            Halcyon::Core::ErrorCode::OutOfMemory, "failed to queue descriptor slot retirement"));
    }
    catch (const std::exception& exception)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidState,
            exception.what(),
            "descriptor slot retirement"));
    }
    slot.retireTimeline = std::max(retireTimelineValue, slot.lastUseTimeline);
    slot.state = DescriptorSlotState::PendingRelease;
    --liveCount_;
    ++pendingCount_;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> DescriptorSlotAllocator::touch(
    DescriptorHandle handle, std::uint64_t submittedTimeline)
{
    if (!validIndex(handle))
    {
        return Halcyon::Result<void>::failure(invalidHandleError());
    }
    auto& slot = slots_[handle.index()];
    if (slot.state != DescriptorSlotState::Live)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidState,
            "only a live descriptor slot can be marked in use"));
    }
    slot.lastUseTimeline = std::max(slot.lastUseTimeline, submittedTimeline);
    return Halcyon::Result<void>::success();
}

std::size_t DescriptorSlotAllocator::collect(
    std::uint64_t completedTimelineValue, std::vector<DescriptorHandle>* reclaimed)
{
    if (slots_.empty())
    {
        return 0;
    }
    completedTimeline_ = std::max(completedTimeline_, completedTimelineValue);
    std::size_t reclaimableCount = 0;
    for (const auto index : pendingSlots_)
    {
        const auto& slot = slots_[index];
        if (slot.state == DescriptorSlotState::PendingRelease &&
            slot.retireTimeline <= completedTimeline_)
        {
            ++reclaimableCount;
        }
    }
    // Reserve both outputs before mutating any slot.  All pushes in the
    // commit loop are then non-allocating, so an exception leaves allocator
    // state unchanged rather than losing a reusable descriptor index.
    freeSlots_.reserve(freeSlots_.size() + reclaimableCount);
    if (reclaimed != nullptr)
    {
        reclaimed->reserve(reclaimed->size() + reclaimableCount);
    }
    std::size_t reclaimedCount = 0;
    std::size_t writeIndex = 0;
    for (const auto index : pendingSlots_)
    {
        auto& slot = slots_[index];
        if (slot.state == DescriptorSlotState::PendingRelease &&
            slot.retireTimeline <= completedTimeline_)
        {
            slot.state = DescriptorSlotState::Free;
            slot.retireTimeline = 0;
            slot.lastUseTimeline = 0;
            slot.generation = nextGeneration(slot.generation);
            freeSlots_.push_back(index);
            --pendingCount_;
            if (reclaimed != nullptr)
            {
                // Return the new generation: this is the handle a caller may
                // use to identify the now-reusable slot without reviving the
                // stale handle that was released.
                reclaimed->push_back(DescriptorHandle{index, slot.generation});
            }
            ++reclaimedCount;
        }
        else
        {
            pendingSlots_[writeIndex++] = index;
        }
    }
    pendingSlots_.resize(writeIndex);
    return reclaimedCount;
}

bool DescriptorSlotAllocator::validIndex(DescriptorHandle handle) const noexcept
{
    if (!handle.valid() || handle.index() >= slots_.size())
    {
        return false;
    }
    return slots_[handle.index()].generation == handle.generation();
}

std::uint32_t DescriptorSlotAllocator::nextGeneration(std::uint32_t generation) noexcept
{
    return nextGenerationValue(generation);
}

Halcyon::Core::Error DescriptorSlotAllocator::invalidHandleError() const
{
    return makeError(Halcyon::Core::ErrorCode::NotFound,
        "descriptor handle is invalid or has a stale generation");
}

bool DescriptorSlotAllocator::contains(DescriptorHandle handle) const noexcept
{
    if (!validIndex(handle))
    {
        return false;
    }
    const auto stateValue = slots_[handle.index()].state;
    return stateValue == DescriptorSlotState::Live || stateValue == DescriptorSlotState::Default;
}

bool DescriptorSlotAllocator::pending(DescriptorHandle handle) const noexcept
{
    return validIndex(handle) &&
           slots_[handle.index()].state == DescriptorSlotState::PendingRelease;
}

DescriptorSlotState DescriptorSlotAllocator::state(DescriptorHandle handle) const noexcept
{
    if (!validIndex(handle))
    {
        return DescriptorSlotState::Free;
    }
    return slots_[handle.index()].state;
}

std::uint64_t DescriptorSlotAllocator::retireTimeline(DescriptorHandle handle) const noexcept
{
    return validIndex(handle) ? slots_[handle.index()].retireTimeline : 0;
}

std::uint64_t DescriptorSlotAllocator::lastUseTimeline(DescriptorHandle handle) const noexcept
{
    return validIndex(handle) ? slots_[handle.index()].lastUseTimeline : 0;
}

void DescriptorSlotAllocator::clear() noexcept
{
    if (slots_.empty())
    {
        return;
    }
    freeSlots_.clear();
    pendingSlots_.clear();
    pendingCount_ = 0;
    liveCount_ = 0;
    for (std::uint32_t index = 0; index < slots_.size(); ++index)
    {
        auto& slot = slots_[index];
        if (reserveDefaultSlot_ && index == 0u)
        {
            slot.state = DescriptorSlotState::Default;
            slot.retireTimeline = 0;
            slot.lastUseTimeline = 0;
            ++liveCount_;
            continue;
        }
        if (slot.state == DescriptorSlotState::Live ||
            slot.state == DescriptorSlotState::PendingRelease)
        {
            slot.generation = nextGeneration(slot.generation);
        }
        slot.state = DescriptorSlotState::Free;
        slot.retireTimeline = 0;
        slot.lastUseTimeline = 0;
        freeSlots_.push_back(index);
    }
    completedTimeline_ = 0;
}

void DescriptorSlotAllocator::shutdown() noexcept
{
    slots_.clear();
    freeSlots_.clear();
    pendingSlots_.clear();
    liveCount_ = 0;
    pendingCount_ = 0;
    completedTimeline_ = 0;
    reserveDefaultSlot_ = true;
}

BindlessTable::BindlessTable(BindlessTableConfig config)
{
    (void)initialize(config);
}

Halcyon::Result<void> BindlessTable::initialize(BindlessTableConfig config)
{
    if (initialized_)
    {
        return Halcyon::Result<void>::failure(makeError(
            Halcyon::Core::ErrorCode::AlreadyExists, "bindless table is already initialized"));
    }
    for (std::size_t i = 0; i < kDescriptorTypeCount; ++i)
    {
        const auto type = static_cast<DescriptorType>(i);
        if (capacityFor(config, type) == 0 ||
            capacityFor(config, type) == DescriptorHandle::kInvalidIndex)
        {
            return Halcyon::Result<void>::failure(
                makeError(Halcyon::Core::ErrorCode::InvalidArgument,
                    std::string("capacity for ") + toString(type) +
                        " must be between 1 and UINT32_MAX-1"));
        }
    }

    try
    {
        std::array<DescriptorSlotAllocator, kDescriptorTypeCount> allocators;
        std::array<std::vector<DescriptorValue>, kDescriptorTypeCount> values;
        for (std::size_t i = 0; i < kDescriptorTypeCount; ++i)
        {
            const auto type = static_cast<DescriptorType>(i);
            auto initialized = allocators[i].initialize(capacityFor(config, type), true);
            if (!initialized)
            {
                return Halcyon::Result<void>::failure(initialized.error());
            }
            values[i].resize(capacityFor(config, type));
        }
        allocators_ = std::move(allocators);
        values_ = std::move(values);
        defaults_.fill(DescriptorValue{});
        completedTimeline_ = 0;
        initialized_ = true;
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::OutOfMemory,
            "failed to allocate bindless descriptor tables"));
    }
    catch (const std::exception& exception)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidState,
            exception.what(),
            "bindless table initialization"));
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<DescriptorHandle> BindlessTable::allocate(
    DescriptorType type, DescriptorValue value, std::uint64_t completedTimelineValue)
{
    if (!validType(type))
    {
        return Halcyon::Result<DescriptorHandle>::failure(invalidTypeError());
    }
    if (!initialized_)
    {
        return Halcyon::Result<DescriptorHandle>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState, "bindless table is not initialized"));
    }
    completedTimeline_ = std::max(completedTimeline_, completedTimelineValue);
    const auto i = indexOf(type);
    // Use the table-wide monotonic value.  A previous allocation/collect may
    // have observed a newer completion value for another descriptor type;
    // passing the raw (possibly older) argument here would make the public
    // completedTimeline() disagree with this allocator's reclamation state.
    auto slotResult = allocators_[i].allocate(completedTimeline_);
    if (!slotResult)
    {
        return Halcyon::Result<DescriptorHandle>::failure(slotResult.error());
    }
    const auto handle = slotResult.value();
    values_[i][handle.index()] = value;
    return Halcyon::Result<DescriptorHandle>::success(handle);
}

Halcyon::Result<BindlessHandle> BindlessTable::allocateTyped(
    DescriptorType type, DescriptorValue value, std::uint64_t completedTimelineValue)
{
    auto result = allocate(type, value, completedTimelineValue);
    if (!result)
    {
        return Halcyon::Result<BindlessHandle>::failure(result.error());
    }
    return Halcyon::Result<BindlessHandle>::success(BindlessHandle{type, result.value()});
}

Halcyon::Result<void> BindlessTable::update(
    DescriptorType type, DescriptorHandle handle, DescriptorValue value)
{
    if (!validType(type))
    {
        return Halcyon::Result<void>::failure(invalidTypeError());
    }
    if (!initialized_)
    {
        return Halcyon::Result<void>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState, "bindless table is not initialized"));
    }
    const auto i = indexOf(type);
    if (!allocators_[i].contains(handle))
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::NotFound,
            "cannot update an invalid, pending, or stale descriptor handle"));
    }
    if (handle.index() == 0u)
    {
        return Halcyon::Result<void>::failure(makeError(Halcyon::Core::ErrorCode::InvalidArgument,
            "slot zero must be updated with setDefault()"));
    }
    values_[i][handle.index()] = value;
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> BindlessTable::release(
    DescriptorType type, DescriptorHandle handle, std::uint64_t retireTimelineValue)
{
    if (!validType(type))
    {
        return Halcyon::Result<void>::failure(invalidTypeError());
    }
    if (!initialized_)
    {
        return Halcyon::Result<void>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState, "bindless table is not initialized"));
    }
    return allocators_[indexOf(type)].release(handle, retireTimelineValue);
}

Halcyon::Result<void> BindlessTable::touch(
    DescriptorType type, DescriptorHandle handle, std::uint64_t submittedTimeline)
{
    if (!validType(type))
    {
        return Halcyon::Result<void>::failure(invalidTypeError());
    }
    if (!initialized_)
    {
        return Halcyon::Result<void>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState, "bindless table is not initialized"));
    }
    return allocators_[indexOf(type)].touch(handle, submittedTimeline);
}

std::size_t BindlessTable::collect(std::uint64_t completedTimelineValue)
{
    if (!initialized_)
    {
        return 0;
    }
    completedTimeline_ = std::max(completedTimeline_, completedTimelineValue);
    std::size_t reclaimedCount = 0;
    for (std::size_t i = 0; i < kDescriptorTypeCount; ++i)
    {
        std::vector<DescriptorHandle> reclaimed;
        reclaimedCount += allocators_[i].collect(completedTimeline_, &reclaimed);
        for (const auto handle : reclaimed)
        {
            if (handle.index() < values_[i].size())
            {
                values_[i][handle.index()] = defaults_[i];
            }
        }
    }
    return reclaimedCount;
}

Halcyon::Result<void> BindlessTable::setDefault(DescriptorType type, DescriptorValue value)
{
    if (!validType(type))
    {
        return Halcyon::Result<void>::failure(invalidTypeError());
    }
    if (!initialized_)
    {
        return Halcyon::Result<void>::failure(
            makeError(Halcyon::Core::ErrorCode::InvalidState, "bindless table is not initialized"));
    }
    const auto i = indexOf(type);
    defaults_[i] = value;
    const auto handle = allocators_[i].defaultHandle();
    if (handle.valid())
    {
        values_[i][0] = value;
    }
    return Halcyon::Result<void>::success();
}

DescriptorHandle BindlessTable::defaultHandle(DescriptorType type) const noexcept
{
    if (!initialized_ || !validType(type))
    {
        return DescriptorHandle::invalid();
    }
    return allocators_[indexOf(type)].defaultHandle();
}

bool BindlessTable::contains(DescriptorType type, DescriptorHandle handle) const noexcept
{
    return initialized_ && validType(type) && allocators_[indexOf(type)].contains(handle);
}

bool BindlessTable::pending(DescriptorType type, DescriptorHandle handle) const noexcept
{
    return initialized_ && validType(type) && allocators_[indexOf(type)].pending(handle);
}

std::optional<DescriptorValue> BindlessTable::get(
    DescriptorType type, DescriptorHandle handle) const noexcept
{
    if (!contains(type, handle))
    {
        return std::nullopt;
    }
    return values_[indexOf(type)][handle.index()];
}

Halcyon::Result<DescriptorValue> BindlessTable::getResult(
    DescriptorType type, DescriptorHandle handle) const
{
    if (const auto value = get(type, handle); value.has_value())
    {
        return Halcyon::Result<DescriptorValue>::success(*value);
    }
    return Halcyon::Result<DescriptorValue>::failure(makeError(Halcyon::Core::ErrorCode::NotFound,
        "descriptor handle is invalid, stale, or pending release"));
}

DescriptorSlotAllocator& BindlessTable::allocator(DescriptorType type) noexcept
{
    return allocators_[validType(type) ? indexOf(type) : 0u];
}

const DescriptorSlotAllocator& BindlessTable::allocator(DescriptorType type) const noexcept
{
    return allocators_[validType(type) ? indexOf(type) : 0u];
}

const DescriptorValue& BindlessTable::defaultValue(DescriptorType type) const noexcept
{
    return defaults_[validType(type) ? indexOf(type) : 0u];
}

std::uint32_t BindlessTable::capacity(DescriptorType type) const noexcept
{
    return initialized_ && validType(type) ? allocators_[indexOf(type)].capacity() : 0u;
}

std::uint32_t BindlessTable::liveCount(DescriptorType type) const noexcept
{
    return initialized_ && validType(type) ? allocators_[indexOf(type)].liveCount() : 0u;
}

std::uint32_t BindlessTable::pendingCount(DescriptorType type) const noexcept
{
    return initialized_ && validType(type) ? allocators_[indexOf(type)].pendingCount() : 0u;
}

Halcyon::Core::Error BindlessTable::invalidTypeError() const
{
    return makeError(Halcyon::Core::ErrorCode::InvalidArgument, "unknown descriptor type");
}

void BindlessTable::clear() noexcept
{
    if (!initialized_)
    {
        return;
    }
    for (std::size_t i = 0; i < kDescriptorTypeCount; ++i)
    {
        allocators_[i].clear();
        std::fill(values_[i].begin(), values_[i].end(), defaults_[i]);
    }
    completedTimeline_ = 0;
}

void BindlessTable::shutdown() noexcept
{
    for (auto& allocator : allocators_)
    {
        allocator = DescriptorSlotAllocator{};
    }
    for (auto& values : values_)
    {
        values.clear();
    }
    defaults_.fill(DescriptorValue{});
    completedTimeline_ = 0;
    initialized_ = false;
}

} // namespace Halcyon::Renderer::Resources
