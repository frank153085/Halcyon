#pragma once

#include "FrameGraphId.h"

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Halcyon::Renderer::Graph
{
class FrameGraphResources;

class FrameGraphPassBase
{
public:
    virtual ~FrameGraphPassBase() noexcept = default;
    std::string_view name() const noexcept
    {
        return name_;
    }
    QueueClass queue() const noexcept
    {
        return queue_;
    }
    bool sideEffect() const noexcept
    {
        return sideEffect_;
    }
    bool culled() const noexcept
    {
        return culled_;
    }
    FrameGraphHandle handle() const noexcept
    {
        return handle_;
    }
    bool failed() const noexcept
    {
        return failedImpl();
    }
    std::string_view errorMessage() const noexcept
    {
        return errorImpl();
    }

private:
    friend class FrameGraph;
    virtual void execute(const FrameGraphResources&, CommandContext&) noexcept = 0;
    virtual bool failedImpl() const noexcept = 0;
    virtual std::string_view errorImpl() const noexcept = 0;
    std::string name_;
    QueueClass queue_ = QueueClass::Graphics;
    bool sideEffect_ = false;
    bool culled_ = false;
    FrameGraphHandle handle_{};
};

template <typename Data, typename Execute>
class FrameGraphPass final : public FrameGraphPassBase
{
public:
    FrameGraphPass(Data data, Execute execute)
            : data_(std::move(data)),
              execute_(std::move(execute))
    {
    }
    Data& data() noexcept
    {
        return data_;
    }
    const Data& data() const noexcept
    {
        return data_;
    }
    Data& getData() noexcept
    {
        return data_;
    }
    const Data& getData() const noexcept
    {
        return data_;
    }

private:
    void execute(const FrameGraphResources& resources, CommandContext& commands) noexcept override
    {
        try
        {
            if constexpr (std::is_invocable_v<Execute,
                              const FrameGraphResources&,
                              const Data&,
                              CommandContext&>)
            {
                execute_(resources, data_, commands);
            }
            else if constexpr (std::is_invocable_v<Execute,
                                   FrameGraphResources&,
                                   Data&,
                                   CommandContext&>)
            {
                execute_(const_cast<FrameGraphResources&>(resources), data_, commands);
            }
            else if constexpr (std::is_invocable_v<Execute, CommandContext&>)
            {
                execute_(commands);
            }
            else if constexpr (std::is_invocable_v<Execute>)
            {
                execute_();
            }
        }
        catch (const std::exception& e)
        {
            // The graph boundary is noexcept. Errors are captured by FrameGraph.
            failed_ = true;
            failure_ = e.what();
        }
        catch (...)
        {
            failed_ = true;
            failure_ = "frame graph pass execution failed";
        }
    }
    Data data_{};
    Execute execute_;
    bool failed_ = false;
    bool failedImpl() const noexcept override
    {
        return failed_;
    }
    std::string_view errorImpl() const noexcept override
    {
        return failure_;
    }
    std::string failure_;
    friend class FrameGraph;
};

} // namespace Halcyon::Renderer::Graph
