#include "RenderGraph.h"

#include <exception>

namespace Halcyon::Renderer::Graph
{

const ResourceLifetime* CompileResult::lifetime(BufferHandle handle) const noexcept
{
    if (!handle.valid() || handle.index() >= resources.size())
    {
        return nullptr;
    }
    const auto& resource = resources[handle.index()];
    if (resource.kind != ResourceKind::Buffer || resource.generation != handle.generation())
    {
        return nullptr;
    }
    return &resource.lifetime;
}

const ResourceLifetime* CompileResult::lifetime(TextureHandle handle) const noexcept
{
    if (!handle.valid() || handle.index() >= resources.size())
    {
        return nullptr;
    }
    const auto& resource = resources[handle.index()];
    if (resource.kind != ResourceKind::Texture || resource.generation != handle.generation())
    {
        return nullptr;
    }
    return &resource.lifetime;
}

const CompiledPass* CompileResult::pass(PassHandle handle) const noexcept
{
    if (!handle.valid() || handle.index() >= passes.size())
    {
        return nullptr;
    }
    const auto& candidate = passes[handle.index()];
    if (candidate.handle.generation() != handle.generation())
    {
        return nullptr;
    }
    return &candidate;
}

bool CompileResult::isCulled(PassHandle handle) const noexcept
{
    const auto* candidate = pass(handle);
    return candidate != nullptr && candidate->culled;
}

CompileResult::ExecutionResult CompileResult::execute(const ExecuteOptions& options) const
{
    ExecutionResult execution;
    if (!success)
    {
        execution.error = error;
        if (!execution.error)
        {
            execution.error.code = GraphErrorCode::InvalidDeclaration;
            execution.error.message = "cannot execute an unsuccessful render graph";
        }
        return execution;
    }

    try
    {
        std::uint32_t executionIndex = 0;
        for (const PassHandle handle : executionOrder)
        {
            const CompiledPass* candidate = pass(handle);
            if (candidate == nullptr || candidate->culled)
            {
                continue;
            }
            const PassExecutionContext context{
                candidate->handle, candidate->name, executionIndex, options.userData};
            if (options.onBegin)
            {
                options.onBegin(context);
            }
            if (candidate->execute)
            {
                candidate->execute(context);
            }
            if (options.onEnd)
            {
                options.onEnd(context);
            }
            ++execution.executedPasses;
            ++executionIndex;
        }
        execution.success = true;
    }
    catch (const std::exception& exception)
    {
        execution.error.code = GraphErrorCode::ExecutionFailed;
        execution.error.message = exception.what();
    }
    catch (...)
    {
        execution.error.code = GraphErrorCode::ExecutionFailed;
        execution.error.message = "render graph pass callback threw an unknown exception";
    }
    return execution;
}

} // namespace Halcyon::Renderer::Graph
