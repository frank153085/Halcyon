#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Halcyon::Renderer::Graph
{

// A compact, index-based node identifier keeps the graph data-oriented while
// still making the pass/resource distinction explicit for diagnostics.
enum class NodeKind : std::uint8_t
{
    Pass,
    Resource,
};

struct NodeId
{
    std::uint32_t index = 0;
    NodeKind kind = NodeKind::Pass;

    [[nodiscard]] friend constexpr bool operator==(NodeId lhs, NodeId rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.kind == rhs.kind;
    }
};

// Explicit dependency storage used by the frame-graph compiler.  The graph
// owns no backend objects and can therefore be unit-tested independently.
class DependencyGraph final
{
public:
    explicit DependencyGraph(std::size_t nodeCount = 0);

    void reset(std::size_t nodeCount);
    void addEdge(std::uint32_t from, std::uint32_t to);
    void addEdge(NodeId from, NodeId to)
    {
        addEdge(from.index, to.index);
    }

    [[nodiscard]] const std::vector<std::uint32_t>& successors(std::uint32_t node) const noexcept;
    [[nodiscard]] const std::vector<std::uint32_t>& predecessors(std::uint32_t node) const noexcept;
    [[nodiscard]] std::vector<std::uint32_t> indegrees() const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<std::vector<std::uint32_t>> successors_;
    std::vector<std::vector<std::uint32_t>> predecessors_;
};

} // namespace Halcyon::Renderer::Graph
