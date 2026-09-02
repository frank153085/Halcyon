#pragma once

// Resource-node metadata and edge bookkeeping.  The file path follows
// Filament's private fg/details layout; types remain in the public Graph
// namespace so existing PassNode/ResourceNode references continue to work.

#include "DependencyGraph.h"
#include "Resource.h"
#include "../FrameGraphId.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Halcyon::Renderer::Graph
{

class FrameGraph;
class PassNode;

class ResourceNode final : public DependencyGraph::Node
{
public:
    ResourceNode() noexcept
            : resourceHandle{}
    {
    }
    ResourceNode(FrameGraph& graph, FrameGraphHandle handle,
        FrameGraphHandle parent = {}) noexcept;
    explicit ResourceNode(FrameGraphHandle handle,
        FrameGraphHandle parent = {}) noexcept;
    ~ResourceNode() noexcept override;

    ResourceNode(const ResourceNode&) = delete;
    ResourceNode& operator=(const ResourceNode&) = delete;
    ResourceNode(ResourceNode&&) = delete;
    ResourceNode& operator=(ResourceNode&&) = delete;

    // Filament names this member resourceHandle.  `handle` is retained as a
    // compatibility spelling from Halcyon's earlier flat node structure.
    FrameGraphHandle resourceHandle;
    FrameGraphHandle handle{};

    void setResource(VirtualResource* resource) noexcept
    {
        resource_ = resource;
        if (resource != nullptr)
        {
            name_ = resource->name;
            kind_ = resource->kind;
            DependencyGraph::Node::name = name_;
            DependencyGraph::Node::kind = kind_;
        }
    }
    VirtualResource* resource() noexcept
    {
        return resource_;
    }
    const VirtualResource* resource() const noexcept
    {
        return resource_;
    }

    void setName(std::string_view name)
    {
        name_.assign(name.data(), name.size());
        DependencyGraph::Node::name = name_;
    }
    const std::string& name() const noexcept
    {
        return name_;
    }

    void addOutgoingEdge(ResourceEdgeBase* edge) noexcept;
    void setIncomingEdge(ResourceEdgeBase* edge) noexcept;
    void clearEdges() noexcept;

    bool hasWriterPass() const noexcept
    {
        return writerEdge_ != nullptr;
    }
    bool hasActiveWriters() const noexcept;
    ResourceEdgeBase* getWriterEdgeForPass(const PassNode* pass) const noexcept;
    bool hasWriteFrom(const PassNode* pass) const noexcept
    {
        return getWriterEdgeForPass(pass) != nullptr;
    }

    bool hasReaders() const noexcept
    {
        return readerEdgesHead_ != nullptr;
    }
    bool hasActiveReaders() const noexcept;
    ResourceEdgeBase* getReaderEdgeForPass(const PassNode* pass) const noexcept;

    void resolveResourceUsage(DependencyGraph& graph) noexcept;

    FrameGraphHandle getParentHandle() const noexcept
    {
        return parentHandle_;
    }
    void setParentHandle(FrameGraphHandle parent) noexcept
    {
        parentHandle_ = parent;
    }

    ResourceNode* getParentNode() const noexcept
    {
        return parentNode_;
    }
    void setParentNode(ResourceNode* parent) noexcept
    {
        parentNode_ = parent;
    }
    static ResourceNode* getAncestorNode(ResourceNode* node) noexcept;

    void setParentReadDependency(ResourceNode* parent) noexcept
    {
        parentReadDependency_ = parent;
    }
    void setParentWriteDependency(ResourceNode* parent) noexcept
    {
        parentWriteDependency_ = parent;
    }
    void setForwardResourceDependency(ResourceNode* source) noexcept
    {
        forwardedDependency_ = source;
    }
    ResourceNode* parentReadDependency() const noexcept
    {
        return parentReadDependency_;
    }
    ResourceNode* parentWriteDependency() const noexcept
    {
        return parentWriteDependency_;
    }
    ResourceNode* forwardedDependency() const noexcept
    {
        return forwardedDependency_;
    }

    ResourceKind kind() const noexcept
    {
        return kind_;
    }
    void setKind(ResourceKind kind) noexcept
    {
        kind_ = kind;
        DependencyGraph::Node::kind = kind;
    }
    std::int32_t producer() const noexcept
    {
        return producer_;
    }
    void setProducer(std::int32_t producer) noexcept
    {
        producer_ = producer;
    }

    std::uint32_t id() const noexcept
    {
        return id_;
    }
    void setId(std::uint32_t id) noexcept
    {
        id_ = id;
        DependencyGraph::Node::index = id;
    }
    bool isCulled() const noexcept
    {
        return culled_;
    }
    void setCulled(bool culled) noexcept
    {
        culled_ = culled;
        DependencyGraph::Node::culled = culled;
    }

    const char* getName() const noexcept;
    std::string graphvizify() const;
    std::string graphvizifyEdgeColor() const;

private:
    VirtualResource* resource_ = nullptr;
    ResourceEdgeBase* readerEdgesHead_ = nullptr;
    ResourceEdgeBase* writerEdge_ = nullptr;
    FrameGraphHandle parentHandle_{};
    ResourceNode* parentNode_ = nullptr;
    ResourceNode* parentReadDependency_ = nullptr;
    ResourceNode* parentWriteDependency_ = nullptr;
    ResourceNode* forwardedDependency_ = nullptr;
    std::string name_;
    ResourceKind kind_ = ResourceKind::Buffer;
    std::int32_t producer_ = -1;
    std::uint32_t id_ = 0;
    bool culled_ = false;
};

} // namespace Halcyon::Renderer::Graph
