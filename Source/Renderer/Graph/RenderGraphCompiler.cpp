#include "FrameGraphNode.h"
#include "RenderGraph.h"

#include <algorithm>
#include <functional>
#include <sstream>

namespace Halcyon::Renderer::Graph
{

namespace
{

[[nodiscard]] const char* kindName(ResourceKind kind) noexcept
{
    return kind == ResourceKind::Buffer ? "buffer" : "texture";
}

[[nodiscard]] std::string handleString(
    ResourceKind kind, std::uint32_t index, std::uint32_t generation)
{
    std::ostringstream stream;
    stream << kindName(kind) << '[' << index << ':' << generation << ']';
    return stream.str();
}

} // namespace

CompileResult FrameGraph::compile(const CompileOptions& options) const
{
    CompileResult result;
    result.resources.resize(resources_.size());
    result.passes.resize(passes_.size());

    // Copy the declarative records first.  Keeping the slot index stable makes
    // handles cheap to query in tools and tests, even when a resource is dead.
    for (std::uint32_t i = 0; i < resources_.size(); ++i)
    {
        const auto& source = resources_[i];
        auto& destination = result.resources[i];
        destination.kind = source.kind;
        destination.index = i;
        destination.generation = source.generation;
        destination.imported = source.imported;
        destination.exported = source.exported;
        destination.buffer = source.buffer;
        destination.texture = source.texture;
        destination.name =
            source.kind == ResourceKind::Buffer ? source.buffer.name : source.texture.name;
        if (destination.name.empty())
        {
            destination.name =
                std::string(source.kind == ResourceKind::Buffer ? "Buffer#" : "Texture#") +
                std::to_string(i);
        }
    }
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        const auto& source = passes_[i];
        auto& destination = result.passes[i];
        destination.handle = PassHandle{i, source.generation};
        destination.name = source.name;
        destination.queue = source.queue;
        destination.sideEffect = source.sideEffect;
        destination.accesses = source.accesses;
        destination.execute = source.execute;
    }

    const std::size_t passCount = passes_.size();
    DependencyGraph dependencies(passCount);
    // A WAR edge is required for execution ordering, but it does not mean
    // that the reader produces data needed by the writer.  Keep a second
    // predecessor list containing only data/explicit dependencies so dead
    // pass culling does not accidentally retain such readers.
    DependencyGraph cullingDependencies(passCount);

    auto addEdge = [&](std::uint32_t from, std::uint32_t to, bool keepsAlive)
    {
        if (from == to || from >= passCount || to >= passCount || !passes_[from].alive ||
            !passes_[to].alive)
        {
            return;
        }
        dependencies.addEdge(from, to);
        if (keepsAlive)
        {
            cullingDependencies.addEdge(from, to);
        }
    };
    // Explicit dependencies are user-authored and a self dependency is a
    // genuine cycle (unlike an access merged within one pass, which is
    // intentionally handled without a self edge).
    auto addExplicitEdge = [&](std::uint32_t from, std::uint32_t to)
    {
        if (from == to && from < passCount && passes_[from].alive)
        {
            dependencies.addEdge(from, to);
            cullingDependencies.addEdge(from, to);
        }
        else
        {
            addEdge(from, to, true);
        }
    };

    // Validate declarations before constructing hazards.  An invalid handle
    // is retained by PassBuilder so this path produces an actionable error.
    for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
    {
        const auto& pass = passes_[passIndex];
        if (!pass.alive)
        {
            continue;
        }
        for (const auto& dependency : pass.explicitDependencies)
        {
            if (!validPass(dependency))
            {
                result.error.code = GraphErrorCode::InvalidHandle;
                result.error.message = "Pass '" + pass.name + "' references an invalid dependency";
                return result;
            }
            addExplicitEdge(dependency.index(), passIndex);
        }
        for (const auto& access : pass.accesses)
        {
            if (!validResource(access.kind, access.resourceIndex, access.resourceGeneration))
            {
                result.error.code = GraphErrorCode::InvalidHandle;
                result.error.message =
                    "Pass '" + pass.name + "' references invalid " +
                    handleString(access.kind, access.resourceIndex, access.resourceGeneration);
                return result;
            }
        }
    }

    struct ResourceState
    {
        std::int32_t lastWriter = -1;
        std::vector<std::uint32_t> readers;
    };
    std::vector<ResourceState> states(resources_.size());

    // Build RAW/WAR/WAW edges.  Accesses on one pass are merged first, which
    // prevents a pass that readWrite()s a resource from acquiring a self-edge.
    for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
    {
        const auto& pass = passes_[passIndex];
        if (!pass.alive)
        {
            continue;
        }

        std::vector<ResourceAccess> merged;
        merged.reserve(pass.accesses.size());
        for (const auto& access : pass.accesses)
        {
            auto existing = std::find_if(merged.begin(),
                merged.end(),
                [&](const ResourceAccess& candidate)
                {
                    return candidate.kind == access.kind &&
                           candidate.resourceIndex == access.resourceIndex &&
                           candidate.resourceGeneration == access.resourceGeneration;
                });
            if (existing == merged.end())
            {
                merged.push_back(access);
            }
            else
            {
                const bool reads = existing->reads() || access.reads();
                const bool writes = existing->writes() || access.writes();
                existing->mode = reads && writes ? AccessMode::ReadWrite
                                                 : (writes ? AccessMode::Write : AccessMode::Read);
                existing->usage |= access.usage;
            }
        }

        for (const auto& access : merged)
        {
            auto& state = states[access.resourceIndex];
            if (access.reads() && state.lastWriter >= 0)
            {
                addEdge(static_cast<std::uint32_t>(state.lastWriter), passIndex, true);
            }
            if (access.writes())
            {
                // WAW and WAR dependencies preserve the declaration order of
                // writes and prevent a write from overtaking active readers.
                if (state.lastWriter >= 0)
                {
                    // A pure overwrite needs WAW ordering while both passes
                    // are live, but the previous value is not input data and
                    // therefore must not keep an otherwise dead writer alive.
                    addEdge(static_cast<std::uint32_t>(state.lastWriter), passIndex, false);
                }
                for (const auto reader : state.readers)
                {
                    addEdge(reader, passIndex, false);
                }
                state.readers.clear();
                state.lastWriter = static_cast<std::int32_t>(passIndex);
            }
            else if (access.reads())
            {
                if (std::find(state.readers.begin(), state.readers.end(), passIndex) ==
                    state.readers.end())
                {
                    state.readers.push_back(passIndex);
                }
            }
        }
    }

    // A DFS gives both a cycle error and a useful cycle path for diagnostics.
    std::vector<std::uint8_t> colour(passCount, 0);
    std::vector<std::uint32_t> stack;
    std::vector<PassHandle> cycle;
    std::function<bool(std::uint32_t)> visit = [&](std::uint32_t node)
    {
        colour[node] = 1;
        stack.push_back(node);
        for (const auto next : dependencies.successors(node))
        {
            if (!passes_[next].alive)
            {
                continue;
            }
            if (colour[next] == 0)
            {
                if (visit(next))
                {
                    return true;
                }
            }
            else if (colour[next] == 1)
            {
                const auto begin = std::find(stack.begin(), stack.end(), next);
                for (auto it = begin; it != stack.end(); ++it)
                {
                    cycle.push_back(makePassHandle(*it));
                }
                cycle.push_back(makePassHandle(next));
                return true;
            }
        }
        stack.pop_back();
        colour[node] = 2;
        return false;
    };
    for (std::uint32_t i = 0; i < passCount; ++i)
    {
        if (passes_[i].alive && colour[i] == 0 && visit(i))
        {
            result.error.code = GraphErrorCode::CycleDetected;
            result.error.cycle = cycle;
            std::ostringstream message;
            message << "Render graph contains a dependency cycle";
            if (!cycle.empty())
            {
                message << ": ";
                for (std::size_t n = 0; n < cycle.size(); ++n)
                {
                    if (n != 0)
                    {
                        message << " -> ";
                    }
                    const auto index = cycle[n].index();
                    message << (index < passes_.size() ? passes_[index].name : "<invalid>");
                }
            }
            result.error.message = message.str();
            return result;
        }
    }

    const std::vector<bool> live = cullPasses(options, cullingDependencies);

    for (std::uint32_t i = 0; i < passCount; ++i)
    {
        result.passes[i].culled = passes_[i].alive && !live[i];
    }

    // Kahn topological sort with an index-ordered ready set keeps output
    // deterministic, which is important for captures and image tests.
    std::vector<std::uint32_t> indegree(passCount, 0);
    for (std::uint32_t from = 0; from < passCount; ++from)
    {
        if (!live[from])
        {
            continue;
        }
        for (const auto to : dependencies.successors(from))
        {
            if (live[to])
            {
                ++indegree[to];
            }
        }
    }
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < passCount; ++i)
    {
        if (live[i] && indegree[i] == 0)
        {
            ready.push_back(i);
        }
    }
    while (!ready.empty())
    {
        const auto minimum = std::min_element(ready.begin(), ready.end());
        const auto node = *minimum;
        ready.erase(minimum);
        result.executionOrder.push_back(makePassHandle(node));
        for (const auto next : dependencies.successors(node))
        {
            if (live[next] && --indegree[next] == 0)
            {
                ready.push_back(next);
            }
        }
    }
    if (result.executionOrder.size() !=
        static_cast<std::size_t>(std::count(live.begin(), live.end(), true)))
    {
        // This should be unreachable because the complete graph was checked
        // above, but protects callers if a future culling rule changes edges.
        result.success = false;
        result.error.code = GraphErrorCode::CycleDetected;
        result.error.message = "Unable to topologically order live render passes";
        return result;
    }

    analyzeResourceLifetimes(result);

    result.success = true;
    result.error = {};
    return result;
}

} // namespace Halcyon::Renderer::Graph
