#pragma once

#include "Core/Memory/FrameArena.h"

#include <functional>
#include <new>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Graph
{
class FrameGraphArena final
{
public:
    FrameGraphArena(const FrameGraphArena&) = delete;
    FrameGraphArena& operator=(const FrameGraphArena&) = delete;
    explicit FrameGraphArena(std::size_t bytes = 1u << 20u)
            : arena_(1, bytes)
    {
    }
    ~FrameGraphArena()
    {
        reset();
    }
    template <typename T, typename... Args>
    T* make(Args&&... args)
    {
        void* memory = arena_.allocate(0, sizeof(T), alignof(T));
        if (!memory)
        {
            return nullptr;
        }
        T* value = ::new (memory) T(std::forward<Args>(args)...);
        destructors_.push_back(
            [value]
            {
                value->~T();
            });
        return value;
    }
    void reset() noexcept
    {
        for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it)
        {
            (*it)();
        }
        destructors_.clear();
        arena_.resetAll();
    }

private:
    Core::FrameArena arena_;
    std::vector<std::function<void()>> destructors_;
};
} // namespace Halcyon::Renderer::Graph
