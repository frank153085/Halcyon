#include "details/ResourceNode.h"

#include "details/PassNode.h"

#include <algorithm>

namespace Halcyon::Renderer::Graph
{

ResourceNode::ResourceNode(FrameGraph& /*graph*/, FrameGraphHandle handle,
    FrameGraphHandle parent) noexcept
    : resourceHandle(handle),
      handle(handle),
      parentHandle_(parent)
{
    pass = false;
    index = handle.index();
}

ResourceNode::ResourceNode(FrameGraphHandle handle,
    FrameGraphHandle parent) noexcept
    : resourceHandle(handle),
      handle(handle),
      parentHandle_(parent)
{
    pass = false;
    index = handle.index();
}

ResourceNode::~ResourceNode() noexcept
{
    clearEdges();
}

void ResourceNode::addOutgoingEdge(ResourceEdgeBase* edge) noexcept
{
    if (edge == nullptr)
    {
        return;
    }
    edge->resource = this;
    edge->writer = false;
    edge->next = readerEdgesHead_;
    readerEdgesHead_ = edge;
}

void ResourceNode::setIncomingEdge(ResourceEdgeBase* edge) noexcept
{
    if (edge == nullptr)
    {
        return;
    }
    delete writerEdge_;
    edge->resource = this;
    edge->writer = true;
    edge->next = nullptr;
    writerEdge_ = edge;
}

void ResourceNode::clearEdges() noexcept
{
    auto* reader = readerEdgesHead_;
    while (reader != nullptr)
    {
        auto* next = reader->next;
        delete reader;
        reader = next;
    }
    readerEdgesHead_ = nullptr;
    delete writerEdge_;
    writerEdge_ = nullptr;
}

bool ResourceNode::hasActiveWriters() const noexcept
{
    if (writerEdge_ == nullptr || writerEdge_->pass == nullptr)
    {
        return writerEdge_ != nullptr;
    }
    return !writerEdge_->pass->isCulled();
}

ResourceEdgeBase* ResourceNode::getWriterEdgeForPass(const PassNode* pass) const noexcept
{
    return writerEdge_ != nullptr && writerEdge_->pass == pass ? writerEdge_ : nullptr;
}

bool ResourceNode::hasActiveReaders() const noexcept
{
    for (auto* edge = readerEdgesHead_; edge != nullptr; edge = edge->next)
    {
        if (edge->pass == nullptr || !edge->pass->isCulled())
        {
            return true;
        }
    }
    return false;
}

ResourceEdgeBase* ResourceNode::getReaderEdgeForPass(const PassNode* pass) const noexcept
{
    for (auto* edge = readerEdgesHead_; edge != nullptr; edge = edge->next)
    {
        if (edge->pass == pass)
        {
            return edge;
        }
    }
    return nullptr;
}

void ResourceNode::resolveResourceUsage(DependencyGraph& graph) noexcept
{
    if (resource_ != nullptr)
    {
        resource_->resolveUsage(graph, readerEdgesHead_, writerEdge_);
    }
}

ResourceNode* ResourceNode::getAncestorNode(ResourceNode* node) noexcept
{
    ResourceNode* ancestor = node;
    for (std::size_t guard = 0; ancestor != nullptr && guard < 65536; ++guard)
    {
        if (ancestor->parentNode_ == nullptr || ancestor->parentNode_ == ancestor)
        {
            break;
        }
        ancestor = ancestor->parentNode_;
    }
    return ancestor;
}

const char* ResourceNode::getName() const noexcept
{
    if (resource_ != nullptr)
    {
        return resource_->name.c_str();
    }
    return name_.c_str();
}

std::string ResourceNode::graphvizify() const
{
    std::string result{"[label=\""};
    result += getName();
    result += "\\nrefs: ";
    result += std::to_string(resource_ != nullptr ? resource_->refcount : 0u);
    result += ", id: ";
    result += std::to_string(id_);
    result += "\"]";
    return result;
}

std::string ResourceNode::graphvizifyEdgeColor() const
{
    return "blue";
}

// Hooks used by Resource<RESOURCE> without including ResourceNode.h there.
ResourceEdgeBase* VirtualResource::findReaderEdge(ResourceNode* resourceNode,
    PassNode* pass) noexcept
{
    return resourceNode != nullptr ? resourceNode->getReaderEdgeForPass(pass) : nullptr;
}

ResourceEdgeBase* VirtualResource::findWriterEdge(ResourceNode* resourceNode,
    PassNode* pass) noexcept
{
    return resourceNode != nullptr ? resourceNode->getWriterEdgeForPass(pass) : nullptr;
}

void VirtualResource::addReaderEdge(ResourceNode* resourceNode,
    ResourceEdgeBase* edge) noexcept
{
    if (resourceNode != nullptr)
    {
        resourceNode->addOutgoingEdge(edge);
    }
}

void VirtualResource::setWriterEdge(ResourceNode* resourceNode,
    ResourceEdgeBase* edge) noexcept
{
    if (resourceNode != nullptr)
    {
        resourceNode->setIncomingEdge(edge);
    }
}

} // namespace Halcyon::Renderer::Graph
