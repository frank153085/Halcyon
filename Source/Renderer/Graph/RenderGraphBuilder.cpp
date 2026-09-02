#include "RenderGraph.h"

#include <utility>

namespace Halcyon::Renderer::Graph
{

PassBuilder& PassBuilder::read(BufferHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Buffer,
            handle.index(),
            handle.generation(),
            AccessMode::Read,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::read(TextureHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Texture,
            handle.index(),
            handle.generation(),
            AccessMode::Read,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::write(BufferHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Buffer,
            handle.index(),
            handle.generation(),
            AccessMode::Write,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::write(TextureHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Texture,
            handle.index(),
            handle.generation(),
            AccessMode::Write,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::readWrite(BufferHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Buffer,
            handle.index(),
            handle.generation(),
            AccessMode::ReadWrite,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::readWrite(TextureHandle handle, ResourceUsage usage)
{
    if (graph_ != nullptr)
    {
        graph_->addAccess(pass_,
            ResourceKind::Texture,
            handle.index(),
            handle.generation(),
            AccessMode::ReadWrite,
            usage);
    }
    return *this;
}

PassBuilder& PassBuilder::dependsOn(PassHandle handle)
{
    if (graph_ != nullptr)
    {
        graph_->addDependency(pass_, handle);
    }
    return *this;
}

PassBuilder& PassBuilder::setSideEffect(bool enabled)
{
    if (graph_ != nullptr)
    {
        graph_->setPassSideEffectInternal(pass_, enabled);
    }
    return *this;
}

PassBuilder& PassBuilder::output(BufferHandle handle)
{
    if (graph_ != nullptr)
    {
        const bool valid =
            graph_->setResourceOutput(ResourceKind::Buffer, handle.index(), handle.generation());
        if (valid)
        {
            graph_->setPassSideEffectInternal(pass_, true);
        }
    }
    return *this;
}

PassBuilder& PassBuilder::output(TextureHandle handle)
{
    if (graph_ != nullptr)
    {
        const bool valid =
            graph_->setResourceOutput(ResourceKind::Texture, handle.index(), handle.generation());
        if (valid)
        {
            graph_->setPassSideEffectInternal(pass_, true);
        }
    }
    return *this;
}

PassBuilder& PassBuilder::setQueue(QueueClass queue)
{
    if (graph_ != nullptr)
    {
        graph_->setPassQueueInternal(pass_, queue);
    }
    return *this;
}

PassBuilder& PassBuilder::setExecute(PassExecuteCallback callback)
{
    if (graph_ != nullptr)
    {
        graph_->setPassExecuteInternal(pass_, std::move(callback));
    }
    return *this;
}

} // namespace Halcyon::Renderer::Graph
