#pragma once

// Small, dependency-free result/error types used by the engine.  The types
// deliberately do not depend on Vulkan (or any other backend), so they can be
// used by tools and unit tests as well as the renderer.

#include <cstdint>
#include <concepts>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Halcyon::Core
{

template <typename T>
class Result;
template <>
class Result<void>;

enum class ErrorCode : std::uint32_t
{
    Unknown = 0,
    InvalidArgument,
    InvalidState,
    NotFound,
    AlreadyExists,
    OutOfMemory,
    Io,
    Unsupported,
    Timeout,
    Cancelled,
    DeviceLost,
    Backend,
};

[[nodiscard]] constexpr std::string_view toString(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::Unknown:         return "unknown";
    case ErrorCode::InvalidArgument: return "invalid argument";
    case ErrorCode::InvalidState:    return "invalid state";
    case ErrorCode::NotFound:        return "not found";
    case ErrorCode::AlreadyExists:   return "already exists";
    case ErrorCode::OutOfMemory:     return "out of memory";
    case ErrorCode::Io:              return "I/O error";
    case ErrorCode::Unsupported:     return "unsupported";
    case ErrorCode::Timeout:         return "timeout";
    case ErrorCode::Cancelled:       return "cancelled";
    case ErrorCode::DeviceLost:      return "device lost";
    case ErrorCode::Backend:         return "backend error";
    }
    return "unknown";
}

/** A value object describing an operation failure. */
struct Error
{
    ErrorCode code{ErrorCode::Unknown};
    std::string message;
    std::string context;

    Error() = default;

    Error(ErrorCode errorCode,
          std::string errorMessage = {},
          std::string errorContext = {})
        : code(errorCode), message(std::move(errorMessage)), context(std::move(errorContext))
    {
    }

    explicit Error(std::string errorMessage)
        : code(ErrorCode::Unknown), message(std::move(errorMessage))
    {
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return message.empty() && context.empty();
    }

    /** Return a copy with an additional operation/context prefix. */
    [[nodiscard]] Error withContext(std::string_view prefix) const
    {
        Error copy = *this;
        if (prefix.empty())
            return copy;

        if (copy.context.empty())
            copy.context.assign(prefix);
        else
        {
            std::string combined;
            combined.reserve(prefix.size() + 2u + copy.context.size());
            combined.assign(prefix);
            combined.append(": ");
            combined.append(copy.context);
            copy.context = std::move(combined);
        }
        return copy;
    }

    /** Human-readable representation suitable for a log line. */
    [[nodiscard]] std::string describe() const
    {
        std::string result;
        const std::string_view codeText = toString(code);
        result.reserve(codeText.size() + message.size() + context.size() + 4u);
        result.append(codeText);
        if (!message.empty())
        {
            result.append(": ");
            result.append(message);
        }
        if (!context.empty())
        {
            result.append(" [");
            result.append(context);
            result.push_back(']');
        }
        return result;
    }

    friend bool operator==(const Error&, const Error&) = default;
};

namespace detail
{
template <typename T>
struct is_result : std::false_type
{
};
template <typename T>
struct is_result<Result<T>> : std::true_type
{
};
template <typename T>
inline constexpr bool is_result_v = is_result<T>::value;
} // namespace detail

/** void specialization: success carries no value, only an optional Error. */
template <>
class Result<void>
{
public:
    using value_type = void;
    using error_type = Error;

    constexpr Result() noexcept = default;
    Result(const Error& error) : error_(error) {}
    Result(Error&& error) noexcept : error_(std::move(error)) {}

    static Result success() noexcept { return Result{}; }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    [[nodiscard]] bool has_value() const noexcept { return hasValue(); }
    [[nodiscard]] bool isOk() const noexcept { return hasValue(); }
    [[nodiscard]] bool is_ok() const noexcept { return isOk(); }
    [[nodiscard]] bool isError() const noexcept { return !hasValue(); }
    [[nodiscard]] bool is_error() const noexcept { return isError(); }
    explicit operator bool() const noexcept { return hasValue(); }

    void value() const { ensureValue(); }

    Error& error() &
    {
        ensureError();
        return *error_;
    }
    const Error& error() const&
    {
        ensureError();
        return *error_;
    }
    Error&& error() &&
    {
        ensureError();
        return std::move(*error_);
    }
    const Error&& error() const&&
    {
        ensureError();
        return std::move(*error_);
    }

    template <typename F>
        requires std::invocable<F>
    [[nodiscard]] auto map(F&& function) const
    {
        using Mapped = std::invoke_result_t<F>;
        using MappedValue = std::conditional_t<std::is_void_v<Mapped>, void,
                                               std::remove_cvref_t<Mapped>>;
        if (isError())
            return Result<MappedValue>::failure(*error_);
        if constexpr (std::is_void_v<Mapped>)
        {
            std::invoke(std::forward<F>(function));
            return Result<void>::success();
        }
        else
        {
            return Result<MappedValue>::success(std::invoke(std::forward<F>(function)));
        }
    }

private:
    void ensureValue() const
    {
        if (isError())
            throw std::logic_error("attempted to access value of a failed Result: " +
                                   error_->describe());
    }
    void ensureError() const
    {
        if (hasValue())
            throw std::logic_error("attempted to access error of a successful Result");
    }

    std::optional<Error> error_;
};

/**
 * A move-friendly expected-like type.
 *
 * Result has no implicit conversion from arbitrary values other than T and
 * Error, which prevents accidental success results when an error is intended.
 */
template <typename T>
class Result
{
public:
    static_assert(!std::is_reference_v<T> && !std::is_void_v<T> &&
                      !std::is_same_v<std::remove_cv_t<T>, Error>,
                  "Result<T> requires a non-reference, non-void value type");
    using value_type = T;
    using error_type = Error;

    Result(const T& value) : storage_(value) {}
    Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_(std::move(value))
    {
    }
    Result(const Error& error) : storage_(error) {}
    Result(Error&& error) noexcept : storage_(std::move(error)) {}

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    static Result success(Args&&... args)
    {
        return Result(T(std::forward<Args>(args)...));
    }

    static Result failure(Error error)
    {
        return Result(std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] bool has_value() const noexcept { return hasValue(); }
    [[nodiscard]] bool isOk() const noexcept { return hasValue(); }
    [[nodiscard]] bool is_ok() const noexcept { return isOk(); }
    [[nodiscard]] bool isError() const noexcept { return !hasValue(); }
    [[nodiscard]] bool is_error() const noexcept { return isError(); }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() &
    {
        ensureValue();
        return std::get<T>(storage_);
    }
    const T& value() const&
    {
        ensureValue();
        return std::get<T>(storage_);
    }
    T&& value() &&
    {
        ensureValue();
        return std::get<T>(std::move(storage_));
    }
    const T&& value() const&&
    {
        ensureValue();
        return std::get<T>(std::move(storage_));
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(value()); }
    const T&& operator*() const&& { return std::move(value()); }
    T* operator->() { return std::addressof(value()); }
    const T* operator->() const { return std::addressof(value()); }

    Error& error() &
    {
        ensureError();
        return std::get<Error>(storage_);
    }
    const Error& error() const&
    {
        ensureError();
        return std::get<Error>(storage_);
    }
    Error&& error() &&
    {
        ensureError();
        return std::get<Error>(std::move(storage_));
    }
    const Error&& error() const&&
    {
        ensureError();
        return std::get<Error>(std::move(storage_));
    }

    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const&
    {
        return hasValue() ? std::get<T>(storage_)
                          : static_cast<T>(std::forward<U>(fallback));
    }

    template <typename U>
    [[nodiscard]] T value_or(U&& fallback) const&
    {
        return valueOr(std::forward<U>(fallback));
    }

    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) &&
    {
        return hasValue() ? std::move(std::get<T>(storage_))
                          : static_cast<T>(std::forward<U>(fallback));
    }

    template <typename U>
    [[nodiscard]] T value_or(U&& fallback) &&
    {
        return std::move(*this).valueOr(std::forward<U>(fallback));
    }

    /** Apply a function to the value while propagating this Result's error. */
    template <typename F>
        requires std::invocable<F, T&>
    [[nodiscard]] auto map(F&& function) &
    {
        using Mapped = std::invoke_result_t<F, T&>;
        using MappedValue = std::conditional_t<std::is_void_v<Mapped>, void,
                                               std::remove_cvref_t<Mapped>>;
        if (!hasValue())
            return Result<MappedValue>::failure(error());
        if constexpr (std::is_void_v<Mapped>)
        {
            std::invoke(std::forward<F>(function), value());
            return Result<void>::success();
        }
        else
        {
            return Result<MappedValue>::success(
                std::invoke(std::forward<F>(function), value()));
        }
    }

    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] auto map(F&& function) const&
    {
        using Mapped = std::invoke_result_t<F, const T&>;
        using MappedValue = std::conditional_t<std::is_void_v<Mapped>, void,
                                               std::remove_cvref_t<Mapped>>;
        if (!hasValue())
            return Result<MappedValue>::failure(error());
        if constexpr (std::is_void_v<Mapped>)
        {
            std::invoke(std::forward<F>(function), value());
            return Result<void>::success();
        }
        else
        {
            return Result<MappedValue>::success(
                std::invoke(std::forward<F>(function), value()));
        }
    }

    /** Invoke a function returning Result<U>, propagating errors. */
    template <typename F>
        requires std::invocable<F, T&>
    [[nodiscard]] auto andThen(F&& function) &
    {
        using Next = std::invoke_result_t<F, T&>;
        static_assert(detail::is_result_v<Next>, "andThen callable must return Result<U>");
        if (!hasValue())
            return Next::failure(error());
        return std::invoke(std::forward<F>(function), value());
    }

    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] auto andThen(F&& function) const&
    {
        using Next = std::invoke_result_t<F, const T&>;
        static_assert(detail::is_result_v<Next>, "andThen callable must return Result<U>");
        if (!hasValue())
            return Next::failure(error());
        return std::invoke(std::forward<F>(function), value());
    }

private:
    void ensureValue() const
    {
        if (!hasValue())
            throw std::logic_error("attempted to access value of a failed Result: " +
                                   std::get<Error>(storage_).describe());
    }
    void ensureError() const
    {
        if (hasValue())
            throw std::logic_error("attempted to access error of a successful Result");
    }

    std::variant<T, Error> storage_;
};

template <typename T>
Result(T) -> Result<T>;
Result(Error) -> Result<void>;

template <typename T>
[[nodiscard]] Result<std::decay_t<T>> Ok(T&& value)
{
    return Result<std::decay_t<T>>::success(std::forward<T>(value));
}

[[nodiscard]] inline Result<void> Ok() noexcept
{
    return Result<void>::success();
}

[[nodiscard]] inline Result<void> Err(Error error)
{
    return Result<void>::failure(std::move(error));
}

[[nodiscard]] inline Error MakeError(ErrorCode code,
                                     std::string message,
                                     std::string context = {})
{
    return Error{code, std::move(message), std::move(context)};
}

} // namespace Halcyon::Core

// Root-level aliases keep call sites concise while the canonical definitions
// remain in Halcyon::Core.
namespace Halcyon
{
using Core::Err;
using Core::Error;
using Core::ErrorCode;
using Core::MakeError;
using Core::Ok;
template <typename T>
using Result = Core::Result<T>;
} // namespace Halcyon
