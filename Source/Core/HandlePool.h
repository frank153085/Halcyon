#pragma once

// A compact generation-checked slot map.  Pools are intentionally single
// threaded; renderer code normally mutates them on the asset thread and uses
// immutable handles from the render thread.  Synchronisation, if needed, is
// kept at the owner so the pool does not hide lock costs.

#include "Handle.h"
#include "Result.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Halcyon::Core
{
namespace detail
{
template <typename HandleOrTag,
    typename IndexT,
    typename GenerationT,
    bool IsHandleType = IsHandle<HandleOrTag>::value>
struct pool_handle_selector;

template <typename HandleOrTag, typename IndexT, typename GenerationT>
struct pool_handle_selector<HandleOrTag, IndexT, GenerationT, false>
{
    using type = Handle<HandleOrTag, IndexT, GenerationT>;
};

template <typename HandleOrTag, typename IndexT, typename GenerationT>
struct pool_handle_selector<HandleOrTag, IndexT, GenerationT, true>
{
    using type = HandleOrTag;
};
} // namespace detail

/**
 * Dense slot storage with generation validation.
 *
 * The second template argument may be either a tag (the usual form) or an
 * already declared Handle type:
 *
 *   using TextureHandle = Handle<TextureTag>;
 *   HandlePool<Texture, TextureTag> textures;
 *   HandlePool<Texture, TextureHandle> also works.
 *
 * Calling reserveSlotZero() before emplacing reserves index zero for a default
 * resource.  The pool itself does not assign any special meaning to that
 * resource; callers can use defaultHandle() and keep it alive for the pool's
 * lifetime.
 */
template <typename T,
    typename HandleOrTag = DefaultHandleTag,
    typename IndexT = std::uint32_t,
    typename GenerationT = std::uint32_t>
class HandlePool
{
public:
    using value_type = T;
    using handle_type =
        typename detail::pool_handle_selector<HandleOrTag, IndexT, GenerationT>::type;
    using index_type = typename handle_type::index_type;
    using generation_type = typename handle_type::generation_type;

    static constexpr index_type kInvalidIndex = handle_type::kInvalidIndex;
    static constexpr index_type npos = kInvalidIndex;

    HandlePool() = default;

    explicit HandlePool(std::size_t initialCapacity)
    {
        slots_.reserve(initialCapacity);
    }

    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;
    HandlePool(HandlePool&& other) noexcept
            : slots_(std::move(other.slots_)),
              firstFree_(std::exchange(other.firstFree_, npos)),
              size_(std::exchange(other.size_, 0))
    {
    }

    HandlePool& operator=(HandlePool&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        slots_ = std::move(other.slots_);
        firstFree_ = std::exchange(other.firstFree_, npos);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }
    ~HandlePool() = default;

    void reserve(std::size_t count)
    {
        slots_.reserve(count);
    }

    /** Reserve index zero without constructing a value there. */
    bool reserveSlotZero()
    {
        if (slots_.empty())
        {
            appendSlot();
            slots_[0].reserved = true;
            return true;
        }

        Slot& slot = slots_[0];
        if (slot.reserved)
        {
            return true;
        }
        if (slot.value.has_value())
        {
            slot.reserved = true;
            return true;
        }

        // Slot zero may currently be on the free list after an erase.
        unlinkFree(static_cast<index_type>(0));
        slot.reserved = true;
        return true;
    }

    // Naming aliases used by resource managers that call the sentinel a
    // "default" slot rather than slot zero.
    bool reserveDefaultSlot()
    {
        return reserveSlotZero();
    }

    [[nodiscard]] bool isSlotZeroReserved() const noexcept
    {
        return !slots_.empty() && slots_[0].reserved;
    }

    /** Return the live handle at slot zero, or an invalid handle. */
    [[nodiscard]] handle_type defaultHandle() const noexcept
    {
        if (slots_.empty() || !slots_[0].reserved || !slots_[0].value.has_value())
        {
            return handle_type::invalid();
        }
        return handle_type{static_cast<index_type>(0), slots_[0].generation};
    }

    /** Construct the caller-owned default resource in slot zero. */
    template <typename... Args>
    [[nodiscard]] Result<handle_type> emplaceDefault(Args&&... args)
    {
        try
        {
            // reserveSlotZero() may need to grow the slot vector.  Keep that
            // allocation inside the Result-producing error boundary too.
            reserveSlotZero();
            Slot& slot = slots_[0];
            if (slot.value.has_value())
            {
                return Result<handle_type>::failure(
                    Error{ErrorCode::AlreadyExists, "default slot is already occupied"});
            }
            slot.value.emplace(std::forward<Args>(args)...);
            ++size_;
            return Result<handle_type>::success(
                handle_type{static_cast<index_type>(0), slot.generation});
        }
        catch (const std::bad_alloc&)
        {
            return Result<handle_type>::failure(
                Error{ErrorCode::OutOfMemory, "failed to construct default resource"});
        }
        catch (const std::exception& exception)
        {
            return Result<handle_type>::failure(
                Error{ErrorCode::InvalidState, exception.what(), "default resource"});
        }
        catch (...)
        {
            return Result<handle_type>::failure(Error{
                ErrorCode::InvalidState, "unknown exception while constructing default resource"});
        }
    }

    template <typename... Args>
    [[nodiscard]] handle_type emplace(Args&&... args)
    {
        const index_type index = acquireSlot();
        Slot& slot = slots_[static_cast<std::size_t>(index)];
        try
        {
            slot.value.emplace(std::forward<Args>(args)...);
        }
        catch (...)
        {
            // acquireSlot removes a free entry; put it back if construction
            // fails so a failed emplace has no observable pool side effect.
            pushFree(index);
            throw;
        }

        ++size_;
        return handle_type{index, slot.generation};
    }

    template <typename... Args>
    [[nodiscard]] Result<handle_type> tryEmplace(Args&&... args)
    {
        try
        {
            return Result<handle_type>::success(emplace(std::forward<Args>(args)...));
        }
        catch (const std::bad_alloc&)
        {
            return Result<handle_type>::failure(
                Error{ErrorCode::OutOfMemory, "failed to allocate pool slot"});
        }
        catch (const std::exception& exception)
        {
            return Result<handle_type>::failure(
                Error{ErrorCode::InvalidState, exception.what(), "pool emplace"});
        }
        catch (...)
        {
            return Result<handle_type>::failure(
                Error{ErrorCode::InvalidState, "unknown exception during pool emplace"});
        }
    }

    [[nodiscard]] bool erase(handle_type handle) noexcept
    {
        Slot* slot = slotFor(handle);
        if (slot == nullptr || slot->reserved || !slot->value.has_value())
        {
            return false;
        }

        slot->value.reset();
        slot->generation = nextGeneration(slot->generation);
        pushFree(handle.index());
        --size_;
        return true;
    }

    [[nodiscard]] bool remove(handle_type handle) noexcept
    {
        return erase(handle);
    }
    [[nodiscard]] bool release(handle_type handle) noexcept
    {
        return erase(handle);
    }
    [[nodiscard]] bool destroy(handle_type handle) noexcept
    {
        return erase(handle);
    }

    /** Invalidate all live handles while retaining capacity and slot-zero reservation. */
    void clear() noexcept
    {
        firstFree_ = npos;
        size_ = 0;
        for (std::size_t i = slots_.size(); i-- > 0;)
        {
            Slot& slot = slots_[i];
            if (slot.value.has_value())
            {
                slot.value.reset();
                slot.generation = nextGeneration(slot.generation);
            }
            slot.nextFree = npos;
            if (!slot.reserved)
            {
                pushFree(static_cast<index_type>(i));
            }
        }
    }

    [[nodiscard]] bool contains(handle_type handle) const noexcept
    {
        const Slot* slot = slotFor(handle);
        return slot != nullptr && slot->value.has_value();
    }
    [[nodiscard]] bool valid(handle_type handle) const noexcept
    {
        return contains(handle);
    }
    [[nodiscard]] bool isValid(handle_type handle) const noexcept
    {
        return contains(handle);
    }

    [[nodiscard]] T* get(handle_type handle) noexcept
    {
        Slot* slot = slotFor(handle);
        return slot != nullptr && slot->value.has_value() ? std::addressof(*slot->value) : nullptr;
    }

    [[nodiscard]] const T* get(handle_type handle) const noexcept
    {
        const Slot* slot = slotFor(handle);
        return slot != nullptr && slot->value.has_value() ? std::addressof(*slot->value) : nullptr;
    }

    [[nodiscard]] T* tryGet(handle_type handle) noexcept
    {
        return get(handle);
    }
    [[nodiscard]] const T* tryGet(handle_type handle) const noexcept
    {
        return get(handle);
    }

    [[nodiscard]] Result<T*> getResult(handle_type handle)
    {
        if (T* value = get(handle))
        {
            return Result<T*>::success(value);
        }
        return Result<T*>::failure(
            Error{ErrorCode::NotFound, "handle is invalid or has been released"});
    }

    [[nodiscard]] Result<const T*> getResult(handle_type handle) const
    {
        if (const T* value = get(handle))
        {
            return Result<const T*>::success(value);
        }
        return Result<const T*>::failure(
            Error{ErrorCode::NotFound, "handle is invalid or has been released"});
    }

    T& at(handle_type handle)
    {
        if (T* value = get(handle))
        {
            return *value;
        }
        throw std::out_of_range("HandlePool::at called with an invalid handle");
    }

    const T& at(handle_type handle) const
    {
        if (const T* value = get(handle))
        {
            return *value;
        }
        throw std::out_of_range("HandlePool::at called with an invalid handle");
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t slotCount() const noexcept
    {
        return slots_.size();
    }
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return slots_.capacity();
    }

    /** Invoke f(handle, value) (or f(value)) for every live slot. */
    template <typename F>
    void forEach(F&& function)
    {
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            Slot& slot = slots_[i];
            if (!slot.value.has_value())
            {
                continue;
            }
            const handle_type handle{static_cast<index_type>(i), slot.generation};
            if constexpr (std::invocable<F&, handle_type, T&>)
            {
                std::invoke(function, handle, *slot.value);
            }
            else if constexpr (std::invocable<F&, T&>)
            {
                std::invoke(function, *slot.value);
            }
            else
            {
                static_assert(std::invocable<F&, T&>,
                    "HandlePool::forEach callback must accept (handle, value) or value");
            }
        }
    }

    template <typename F>
    void forEach(F&& function) const
    {
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            const Slot& slot = slots_[i];
            if (!slot.value.has_value())
            {
                continue;
            }
            const handle_type handle{static_cast<index_type>(i), slot.generation};
            if constexpr (std::invocable<F&, handle_type, const T&>)
            {
                std::invoke(function, handle, *slot.value);
            }
            else if constexpr (std::invocable<F&, const T&>)
            {
                std::invoke(function, *slot.value);
            }
            else
            {
                static_assert(std::invocable<F&, const T&>,
                    "HandlePool::forEach callback must accept (handle, value) or value");
            }
        }
    }

private:
    struct Slot
    {
        std::optional<T> value;
        generation_type generation{generation_type{1}};
        index_type nextFree{npos};
        bool reserved{false};
    };

    [[nodiscard]] static generation_type nextGeneration(generation_type current) noexcept
    {
        ++current;
        if (current == handle_type::kInvalidGeneration)
        {
            current = generation_type{1};
        }
        return current;
    }

    void appendSlot()
    {
        if (slots_.size() >= static_cast<std::size_t>(kInvalidIndex))
        {
            throw std::length_error("HandlePool exhausted handle index space");
        }
        slots_.emplace_back();
    }

    [[nodiscard]] index_type acquireSlot()
    {
        if (firstFree_ != npos)
        {
            const index_type index = firstFree_;
            Slot& slot = slots_[static_cast<std::size_t>(index)];
            firstFree_ = slot.nextFree;
            slot.nextFree = npos;
            return index;
        }

        appendSlot();
        return static_cast<index_type>(slots_.size() - 1u);
    }

    void pushFree(index_type index) noexcept
    {
        Slot& slot = slots_[static_cast<std::size_t>(index)];
        if (slot.reserved)
        {
            return;
        }
        slot.nextFree = firstFree_;
        firstFree_ = index;
    }

    void unlinkFree(index_type index) noexcept
    {
        if (firstFree_ == npos)
        {
            return;
        }
        if (firstFree_ == index)
        {
            firstFree_ = slots_[static_cast<std::size_t>(index)].nextFree;
            slots_[static_cast<std::size_t>(index)].nextFree = npos;
            return;
        }

        index_type previous = firstFree_;
        while (previous != npos)
        {
            Slot& previousSlot = slots_[static_cast<std::size_t>(previous)];
            if (previousSlot.nextFree == index)
            {
                previousSlot.nextFree = slots_[static_cast<std::size_t>(index)].nextFree;
                slots_[static_cast<std::size_t>(index)].nextFree = npos;
                return;
            }
            previous = previousSlot.nextFree;
        }
    }

    [[nodiscard]] Slot* slotFor(handle_type handle) noexcept
    {
        if (!handle.isValid())
        {
            return nullptr;
        }
        const std::size_t index = static_cast<std::size_t>(handle.index());
        if (index >= slots_.size())
        {
            return nullptr;
        }
        Slot& slot = slots_[index];
        return slot.generation == handle.generation() ? &slot : nullptr;
    }

    [[nodiscard]] const Slot* slotFor(handle_type handle) const noexcept
    {
        if (!handle.isValid())
        {
            return nullptr;
        }
        const std::size_t index = static_cast<std::size_t>(handle.index());
        if (index >= slots_.size())
        {
            return nullptr;
        }
        const Slot& slot = slots_[index];
        return slot.generation == handle.generation() ? &slot : nullptr;
    }

    std::vector<Slot> slots_;
    index_type firstFree_{npos};
    std::size_t size_{0};
};

template <typename T,
    typename Tag = DefaultHandleTag,
    typename IndexT = std::uint32_t,
    typename GenerationT = std::uint32_t>
using TypedHandlePool = HandlePool<T, Handle<Tag, IndexT, GenerationT>>;

} // namespace Halcyon::Core

namespace Halcyon
{
using Core::HandlePool;
using Core::TypedHandlePool;
} // namespace Halcyon
