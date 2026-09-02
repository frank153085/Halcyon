#include "FrameGraphNode.h"

#include <algorithm>

namespace Halcyon::Renderer::Graph
{

DependencyGraph::DependencyGraph(std::size_t nodeCount)
{
    reset(nodeCount);
}

void DependencyGraph::reset(std::size_t nodeCount)
{
    successors_.assign(nodeCount, {});
    predecessors_.assign(nodeCount, {});
}

void DependencyGraph::addEdge(std::uint32_t from, std::uint32_t to)
{
    if (from >= successors_.size() || to >= successors_.size())
    {
        return;
    }
    auto& outgoing = successors_[from];
    if (std::find(outgoing.begin(), outgoing.end(), to) == outgoing.end())
    {
        outgoing.push_back(to);
    }
    auto& incoming = predecessors_[to];
    if (std::find(incoming.begin(), incoming.end(), from) == incoming.end())
    {
        incoming.push_back(from);
    }
}

const std::vector<std::uint32_t>& DependencyGraph::successors(std::uint32_t node) const noexcept
{
    static const std::vector<std::uint32_t> empty;
    return node < successors_.size() ? successors_[node] : empty;
}

const std::vector<std::uint32_t>& DependencyGraph::predecessors(std::uint32_t node) const noexcept
{
    static const std::vector<std::uint32_t> empty;
    return node < predecessors_.size() ? predecessors_[node] : empty;
}

std::vector<std::uint32_t> DependencyGraph::indegrees() const
{
    std::vector<std::uint32_t> result;
    result.reserve(predecessors_.size());
    for (const auto& incoming : predecessors_)
    {
        result.push_back(static_cast<std::uint32_t>(incoming.size()));
    }
    return result;
}

std::size_t DependencyGraph::size() const noexcept
{
    return successors_.size();
}

} // namespace Halcyon::Renderer::Graph
