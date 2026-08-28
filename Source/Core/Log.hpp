#pragma once

// A tiny logging facility suitable for early engine bring-up.  It intentionally
// uses the standard library only; a richer sink (Tracy, file logging, etc.) can
// be installed later without changing call sites.

#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Halcyon::Core
{

enum class LogLevel : std::uint8_t
{
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

[[nodiscard]] constexpr std::string_view toString(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:    return "TRACE";
    case LogLevel::Debug:    return "DEBUG";
    case LogLevel::Info:     return "INFO";
    case LogLevel::Warn:     return "WARN";
    case LogLevel::Error:    return "ERROR";
    case LogLevel::Critical: return "CRITICAL";
    case LogLevel::Off:      return "OFF";
    }
    return "UNKNOWN";
}

class Logger
{
public:
    using Sink = std::function<void(LogLevel, std::string_view)>;

    static Logger& instance() noexcept
    {
        static Logger logger;
        return logger;
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void setLevel(LogLevel level) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex_);
            level_ = level;
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] LogLevel level() const noexcept
    {
        try
        {
            std::scoped_lock lock(mutex_);
            return level_;
        }
        catch (...)
        {
            return LogLevel::Off;
        }
    }

    [[nodiscard]] bool shouldLog(LogLevel level) const noexcept
    {
        try
        {
            std::scoped_lock lock(mutex_);
            return level != LogLevel::Off && level >= level_ && level_ != LogLevel::Off;
        }
        catch (...)
        {
            return false;
        }
    }

    /** Install a sink. Passing an empty function restores the stderr sink. */
    void setSink(Sink sink)
    {
        std::scoped_lock lock(mutex_);
        sink_ = sink ? std::move(sink) : Sink{&Logger::defaultSink};
    }

    void resetSink()
    {
        std::scoped_lock lock(mutex_);
        sink_ = Sink{&Logger::defaultSink};
    }

    void log(LogLevel level, std::string_view message) const noexcept
    {
        try
        {
            Sink sink;
            {
                std::scoped_lock lock(mutex_);
                if (level == LogLevel::Off || level_ == LogLevel::Off || level < level_)
                    return;
                sink = sink_;
            }

            // Keep an owning copy alive while a custom sink consumes
            // string_view. A sink must not retain the view after this call.
            const std::string ownedMessage(message);
            if (sink)
                sink(level, ownedMessage);
        }
        catch (...)
        {
            // Logging must never bring down the renderer.  The default sink is
            // noexcept in practice, while user sinks may throw.
        }
    }

    template <typename... Args>
    void logf(LogLevel level, Args&&... args) const noexcept
    {
        if (!shouldLog(level))
            return;
        try
        {
            std::ostringstream stream;
            (stream << ... << std::forward<Args>(args));
            log(level, stream.str());
        }
        catch (...)
        {
            // Formatting/allocation failures are deliberately swallowed; a
            // diagnostic path should not alter application control flow.
        }
    }

    template <typename... Args>
    void trace(Args&&... args) const noexcept
    {
        logf(LogLevel::Trace, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug(Args&&... args) const noexcept
    {
        logf(LogLevel::Debug, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info(Args&&... args) const noexcept
    {
        logf(LogLevel::Info, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(Args&&... args) const noexcept
    {
        logf(LogLevel::Warn, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(Args&&... args) const noexcept
    {
        logf(LogLevel::Error, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void critical(Args&&... args) const noexcept
    {
        logf(LogLevel::Critical, std::forward<Args>(args)...);
    }

    void flush() const noexcept
    {
        try
        {
            std::scoped_lock lock(mutex_);
            std::clog.flush();
        }
        catch (...)
        {
        }
    }

private:
    Logger() : sink_(&Logger::defaultSink) {}

    static void defaultSink(LogLevel level, std::string_view message) noexcept
    {
        try
        {
            const auto now = std::chrono::system_clock::now();
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch());
            std::clog << '[' << milliseconds.count() << "ms] [" << toString(level) << "] [tid "
                      << std::this_thread::get_id() << "] " << message << '\n';
        }
        catch (...)
        {
        }
    }

    mutable std::mutex mutex_;
    LogLevel level_{LogLevel::Info};
    Sink sink_;
};

template <typename... Args>
inline void LogTrace(Args&&... args) noexcept
{
    Logger::instance().trace(std::forward<Args>(args)...);
}
template <typename... Args>
inline void LogDebug(Args&&... args) noexcept
{
    Logger::instance().debug(std::forward<Args>(args)...);
}
template <typename... Args>
inline void LogInfo(Args&&... args) noexcept
{
    Logger::instance().info(std::forward<Args>(args)...);
}
template <typename... Args>
inline void LogWarn(Args&&... args) noexcept
{
    Logger::instance().warn(std::forward<Args>(args)...);
}
template <typename... Args>
inline void LogError(Args&&... args) noexcept
{
    Logger::instance().error(std::forward<Args>(args)...);
}
template <typename... Args>
inline void LogCritical(Args&&... args) noexcept
{
    Logger::instance().critical(std::forward<Args>(args)...);
}

} // namespace Halcyon::Core

// Optional macro façade.  The calls remain type-safe and arguments are only
// formatted when the corresponding level is enabled.
#ifndef HALCYON_LOG_TRACE
#    define HALCYON_LOG_TRACE(...) ::Halcyon::Core::LogTrace(__VA_ARGS__)
#    define HALCYON_LOG_DEBUG(...) ::Halcyon::Core::LogDebug(__VA_ARGS__)
#    define HALCYON_LOG_INFO(...)  ::Halcyon::Core::LogInfo(__VA_ARGS__)
#    define HALCYON_LOG_WARN(...)  ::Halcyon::Core::LogWarn(__VA_ARGS__)
#    define HALCYON_LOG_ERROR(...) ::Halcyon::Core::LogError(__VA_ARGS__)
#    define HALCYON_LOG_CRITICAL(...) ::Halcyon::Core::LogCritical(__VA_ARGS__)
#endif

namespace Halcyon
{
using Core::LogCritical;
using Core::LogDebug;
using Core::LogError;
using Core::Logger;
using Core::LogInfo;
using Core::LogLevel;
using Core::LogTrace;
using Core::LogWarn;
} // namespace Halcyon
