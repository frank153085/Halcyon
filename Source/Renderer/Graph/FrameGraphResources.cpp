#include "FrameGraphResources.h"

#include "FrameGraph.h"
#include "details/PassNode.h"

namespace Halcyon::Renderer::Graph
{

FrameGraphResources::FrameGraphResources(
    const FrameGraph* graph, FrameGraphHandle pass) noexcept
        : graph_(graph),
          pass_(pass)
{
}

std::string_view FrameGraphResources::passName() const noexcept
{
    if (!graph_ || pass_.index() >= graph_->passes_.size())
    {
        return {};
    }
    return graph_->passes_[pass_.index()].object
               ? graph_->passes_[pass_.index()].object->name()
               : std::string_view{};
}

const void* FrameGraphResources::getRaw(FrameGraphHandle h, ResourceKind kind) const noexcept
{
    return graph_ && graph_->declaredRaw(pass_, h) ? graph_->resourceRaw(h, kind) : nullptr;
}

const void* FrameGraphResources::subresourceRaw(
    FrameGraphHandle h, ResourceKind kind) const noexcept
{
    if (!graph_ || !graph_->declaredRaw(pass_, h) || h.index() >= graph_->resources_.size())
    {
        return nullptr;
    }
    const auto& resource = graph_->resources_[h.index()];
    if (resource.kind != kind || resource.version != h.version())
    {
        return nullptr;
    }
    return kind == ResourceKind::Texture
               ? static_cast<const void*>(&resource.textureSubresource)
               : static_cast<const void*>(&resource.bufferSubresource);
}

ResourceUsage FrameGraphResources::usageRaw(FrameGraphHandle h) const noexcept
{
    return graph_ ? graph_->resourceUsageRaw(h) : ResourceUsage::None;
}

bool FrameGraphResources::declared(FrameGraphHandle h) const noexcept
{
    return graph_ && graph_->declaredRaw(pass_, h);
}

void FrameGraphResources::detachRaw(FrameGraphHandle h) const noexcept
{
    if (graph_ && h.index() < graph_->resources_.size())
    {
        const_cast<FrameGraph*>(graph_)->resources_[h.index()].detached = true;
    }
}

FrameGraphResources::RenderPassInfo FrameGraphResources::getRenderPassInfo(
    std::uint32_t id) const noexcept
{
    RenderPassInfo result{};
    if (graph_ == nullptr || pass_.index() >= graph_->passNodes_.size() ||
        graph_->passNodes_[pass_.index()] == nullptr)
    {
        return result;
    }
    const auto* node = dynamic_cast<const RenderPassNode*>(
        graph_->passNodes_[pass_.index()].get());
    const auto* data = node != nullptr ? node->getRenderPassData(id) : nullptr;
    if (data == nullptr)
    {
        return result;
    }
    result.target = data->native;
    result.descriptor = data->descriptor;
    result.discardStart = data->discardStart;
    result.discardEnd = data->discardEnd;
    result.clearFlags = data->clearFlags;
    result.readOnly = data->readOnly;
    return result;
}

} // namespace Halcyon::Renderer::Graph
