#pragma once

#include "FrameGraphId.h"

#include <cstddef>
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
class FrameGraph;

// Matches Filament's small executor/base split.  Keeping the execution
// interface in its own type makes it possible for tooling and internal pass
// nodes to refer to an executor without knowing the concrete pass payload.
class FrameGraphPassExecutor
{
protected:
    virtual void execute(const FrameGraphResources&, CommandContext&) noexcept = 0;

public:
    FrameGraphPassExecutor() noexcept = default;
    virtual ~FrameGraphPassExecutor() noexcept;
    FrameGraphPassExecutor(const FrameGraphPassExecutor&) = delete;
    FrameGraphPassExecutor& operator=(const FrameGraphPassExecutor&) = delete;
};

class FrameGraphPassBase : protected FrameGraphPassExecutor
{
public:
    using FrameGraphPassExecutor::FrameGraphPassExecutor;
    ~FrameGraphPassBase() noexcept override;
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

    // Internal dispatch hook used by RenderPassNode.  Keeping the executor
    // method protected preserves the public API while allowing the Filament-
    // style node to own pass invocation.
    void executeInternal(const FrameGraphResources& resources,
        CommandContext& commands) noexcept
    {
        execute(resources, commands);
    }

private:
    friend class RenderPassNode;
    friend class FrameGraph;
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

    // Filament exposes the payload size for arena-backed pass nodes.  Halcyon
    // currently uses std::unique_ptr, but retaining the query keeps the same
    // vocabulary for future arena allocation.
    std::size_t getSize() const noexcept
    {
        return sizeof(FrameGraphPass);
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
