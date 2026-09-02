#include "details/DependencyGraph.h"

#include <algorithm>

namespace Halcyon::Renderer::Graph
{

DependencyGraph::DependencyGraph(std::size_t count)
{
    reset(count);
}

void DependencyGraph::reset(std::size_t count)
{
    successors_.assign(count, {});
    predecessors_.assign(count, {});
    nodes_.resize(count);
    edges_.clear();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        nodes_[i].index = i;
        nodes_[i].refCount = 0;
        nodes_[i].target = false;
        nodes_[i].culled = false;
    }
}

void DependencyGraph::addEdge(std::uint32_t from, std::uint32_t to)
{
    if (from >= size() || to >= size())
    {
        return;
    }

    auto& out = successors_[from];
    if (std::find(out.begin(), out.end(), to) == out.end())
    {
        out.push_back(to);
        predecessors_[to].push_back(from);
        edges_.push_back({from, to});
    }
}

const std::vector<std::uint32_t>& DependencyGraph::successors(std::uint32_t n) const noexcept
{
    static const std::vector<std::uint32_t> empty;
    return n < size() ? successors_[n] : empty;
}

const std::vector<std::uint32_t>& DependencyGraph::predecessors(std::uint32_t n) const noexcept
{
    static const std::vector<std::uint32_t> empty;
    return n < size() ? predecessors_[n] : empty;
}

std::vector<std::uint32_t> DependencyGraph::indegrees() const
{
    std::vector<std::uint32_t> result;
    result.reserve(size());
    for (const auto& predecessors : predecessors_)
    {
        result.push_back(static_cast<std::uint32_t>(predecessors.size()));
    }
    return result;
}

void DependencyGraph::makeTarget(std::uint32_t node) noexcept
{
    if (node < nodes_.size())
    {
        nodes_[node].target = true;
    }
}

bool DependencyGraph::isCulled(std::uint32_t node) const noexcept
{
    return node >= nodes_.size() || nodes_[node].culled;
}

void DependencyGraph::cull() noexcept
{
    std::vector<bool> live(nodes_.size(), false);
    std::vector<std::uint32_t> work;
    for (std::uint32_t i = 0; i < nodes_.size(); ++i)
    {
        nodes_[i].refCount = 0;
        nodes_[i].culled = true;
        if (nodes_[i].target)
        {
            live[i] = true;
            work.push_back(i);
        }
    }
    while (!work.empty())
    {
        const auto node = work.back();
        work.pop_back();
        for (const auto predecessor : predecessors(node))
        {
            ++nodes_[predecessor].refCount;
            if (!live[predecessor])
            {
                live[predecessor] = true;
                work.push_back(predecessor);
            }
        }
    }
    for (std::uint32_t i = 0; i < nodes_.size(); ++i)
    {
        nodes_[i].culled = !live[i];
    }
}

bool DependencyGraph::isEdgeValid(const Edge& edge) const noexcept
{
    return edge.from < nodes_.size() && edge.to < nodes_.size() &&
           !nodes_[edge.from].culled && !nodes_[edge.to].culled;
}

} // namespace Halcyon::Renderer::Graph
