#pragma once

// Timeline-aware deferred destruction for GPU objects.
//
// A renderer submits work with a monotonically increasing timeline value and
// retires resources only after the GPU reports that value complete.  This
// queue deliberately has no Vulkan dependency: a backend only needs to feed
// the value read from its timeline semaphore to collect().

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Halcyon::Renderer::Resources
{

class DeferredDeletionQueue
{
public:
    using TimelineValue = std::uint64_t;

private:
    // std::function is copyable, which prevents callbacks that own a unique
    // GPU allocation.  This small type-erased wrapper is move-only and keeps
    // enqueue useful for such resources without a third-party dependency.
    class Callback
    {
        struct Concept
        {
            virtual ~Concept() = default;
            virtual void invoke() = 0;
        };

        template <typename F>
        struct Model final : Concept
        {
            template <typename U>
            explicit Model(U&& function) : function_(std::forward<U>(function)) {}

            void invoke() override { std::invoke(function_); }

            F function_;
        };

    public:
        Callback() = delete;
        Callback(const Callback&) = delete;
        Callback& operator=(const Callback&) = delete;
        Callback(Callback&&) noexcept = default;
        Callback& operator=(Callback&&) noexcept = default;

        template <typename F>
            requires(!std::same_as<std::decay_t<F>, Callback> &&
                     std::invocable<std::remove_reference_t<F>&>)
        explicit Callback(F&& function)
            : implementation_(makeImplementation(std::forward<F>(function)))
        {
        }

        void operator()() { implementation_->invoke(); }

    private:
        template <typename F>
        [[nodiscard]] static std::unique_ptr<Concept> makeImplementation(F&& function)
        {
            // decay_t also turns a free-function reference into a function
            // pointer, which is a valid storable callback type.
            using Function = std::decay_t<F>;
            // A null function pointer is almost always a programming error;
            // report it at enqueue time instead of invoking undefined code.
            if constexpr (std::is_pointer_v<Function>)
            {
                if (function == nullptr)
                    throw std::invalid_argument("DeferredDeletionQueue callback is null");
            }
            return std::make_unique<Model<Function>>(std::forward<F>(function));
        }

        std::unique_ptr<Concept> implementation_;
    };

    using Entries = std::multimap<TimelineValue, Callback>;

public:
    DeferredDeletionQueue() = default;
    DeferredDeletionQueue(const DeferredDeletionQueue&) = delete;
    DeferredDeletionQueue& operator=(const DeferredDeletionQueue&) = delete;
    DeferredDeletionQueue(DeferredDeletionQueue&&) = delete;
    DeferredDeletionQueue& operator=(DeferredDeletionQueue&&) = delete;

    /**
     * Queue a callback until completed >= retireValue.
     *
     * Callbacks execute synchronously on the thread calling collect/flush,
     * outside the queue mutex.  The callback may therefore enqueue additional
     * work or query size().  Allocation/constructor failures leave the queue
     * unchanged and propagate to the caller.
     */
    template <typename F>
        requires std::invocable<std::remove_reference_t<F>&>
    void enqueue(TimelineValue retireValue, F&& callback)
    {
        Callback erased(std::forward<F>(callback));
        std::scoped_lock lock(mutex_);
        entries_.emplace(retireValue, std::move(erased));
    }

    /** Non-throwing convenience wrapper for callers on a teardown path. */
    template <typename F>
        requires std::invocable<std::remove_reference_t<F>&>
    [[nodiscard]] bool tryEnqueue(TimelineValue retireValue, F&& callback) noexcept
    {
        try
        {
            enqueue(retireValue, std::forward<F>(callback));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    /**
     * Execute callbacks whose retire value is at most completed.
     *
     * A throwing callback is caught, recorded, and removed so one broken
     * destructor cannot block all later resources.  The returned count is the
     * number of callbacks attempted (including callbacks that threw).
     */
    [[nodiscard]] std::size_t collect(TimelineValue completed) noexcept
    {
        std::size_t attempted = 0;
        for (;;)
        {
            auto node = extractReady(completed);
            if (!node)
                break;

            try
            {
                node.mapped()();
            }
            catch (...)
            {
                recordException(std::current_exception());
            }
            ++attempted;
        }
        return attempted;
    }

    /** Execute every currently queued callback regardless of timeline value. */
    [[nodiscard]] std::size_t flush() noexcept
    {
        std::size_t attempted = 0;
        for (;;)
        {
            auto node = extractAny();
            if (!node)
                break;

            try
            {
                node.mapped()();
            }
            catch (...)
            {
                recordException(std::current_exception());
            }
            ++attempted;
        }
        return attempted;
    }

    /**
     * Destruction is normally performed after the device has become idle.
     * Flush here as a last-resort leak guard; callback exceptions are already
     * contained by flush().
     */
    ~DeferredDeletionQueue() noexcept { (void)flush(); }

    [[nodiscard]] std::size_t size() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return entries_.size();
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /** Number of callbacks that threw since the last clearErrors() call. */
    [[nodiscard]] std::size_t errorCount() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return errorCount_;
    }

    [[nodiscard]] bool hasErrors() const noexcept { return errorCount() != 0; }

    /** Return the most recently captured exception for diagnostics, if any. */
    [[nodiscard]] std::exception_ptr lastException() const noexcept
    {
        std::scoped_lock lock(mutex_);
        return lastException_;
    }

    void clearErrors() noexcept
    {
        std::scoped_lock lock(mutex_);
        errorCount_ = 0;
        lastException_ = nullptr;
    }

private:
    [[nodiscard]] Entries::node_type extractReady(TimelineValue completed) noexcept
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = entries_.begin();
        if (iterator == entries_.end() || iterator->first > completed)
            return {};
        return entries_.extract(iterator);
    }

    [[nodiscard]] Entries::node_type extractAny() noexcept
    {
        std::scoped_lock lock(mutex_);
        if (entries_.empty())
            return {};
        return entries_.extract(entries_.begin());
    }

    void recordException(std::exception_ptr exception) noexcept
    {
        std::scoped_lock lock(mutex_);
        ++errorCount_;
        lastException_ = std::move(exception);
    }

    mutable std::mutex mutex_;
    Entries entries_;
    std::size_t errorCount_ = 0;
    std::exception_ptr lastException_;
};

} // namespace Halcyon::Renderer::Resources

namespace Halcyon::Renderer
{
using Resources::DeferredDeletionQueue;
} // namespace Halcyon::Renderer
