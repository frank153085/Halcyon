#pragma once

// Backend-independent dependency graph used by the frame-graph compiler.
//
// This header intentionally lives in the Graph/details directory, mirroring
// Filament's frame-graph layout.  The public FrameGraph header includes it for
// the DependencyGraph type, while the compatibility header one directory up
// keeps the historical include path working for existing users.

#include "../FrameGraphTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Halcyon::Renderer::Graph
{

class DependencyGraph
{
public:
    enum class NodeKind : std::uint8_t
    {
        Pass,
        Resource
    };

    struct NodeId
    {
        std::uint32_t index = 0;
        NodeKind kind = NodeKind::Pass;

        friend constexpr bool operator==(NodeId, NodeId) = default;
    };

    struct Node
    {
        virtual ~Node() noexcept = default;
        std::uint32_t index = 0;
        ResourceKind kind = ResourceKind::Buffer;
        bool pass = false;
        std::string name;
        std::uint32_t refCount = 0;
        bool target = false;
        bool culled = false;
    };

    struct Edge
    {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
    };

    explicit DependencyGraph(std::size_t count = 0);

    void reset(std::size_t count);

    void addEdge(std::uint32_t from, std::uint32_t to);

    void addEdge(NodeId from, NodeId to)
    {
        addEdge(from.index, to.index);
    }

    std::size_t size() const noexcept
    {
        return successors_.size();
    }

    const std::vector<Node>& nodes() const noexcept
    {
        return nodes_;
    }
    std::vector<Node>& nodes() noexcept
    {
        return nodes_;
    }

    const std::vector<Edge>& edges() const noexcept
    {
        return edges_;
    }

    const std::vector<std::uint32_t>& successors(std::uint32_t n) const noexcept;

    const std::vector<std::uint32_t>& predecessors(std::uint32_t n) const noexcept;

    std::vector<std::uint32_t> indegrees() const;

    void makeTarget(std::uint32_t node) noexcept;
    bool isCulled(std::uint32_t node) const noexcept;
    void cull() noexcept;
    bool isEdgeValid(const Edge& edge) const noexcept;

private:
    std::vector<std::vector<std::uint32_t>> successors_;
    std::vector<std::vector<std::uint32_t>> predecessors_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

using NodeKind = DependencyGraph::NodeKind;
using NodeId = DependencyGraph::NodeId;

} // namespace Halcyon::Renderer::Graph
