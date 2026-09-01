#include "RenderGraph.h"

#include <algorithm>
#include <deque>
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
        // An output declaration is also a useful safety net for passes that
        // only consume an imported resource (there is no producer to root).
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

BufferHandle RenderGraph::createBuffer(BufferDesc desc)
{
    std::uint32_t index = kInvalidIndex;
    if (!freeBufferSlots_.empty())
    {
        index = freeBufferSlots_.back();
        freeBufferSlots_.pop_back();
        auto& node = resources_[index];
        node.kind = ResourceKind::Buffer;
        node.generation = allocateResourceGeneration();
        node.alive = true;
        node.imported = false;
        node.exported = false;
        node.buffer = std::move(desc);
        node.texture = {};
    }
    else
    {
        index = static_cast<std::uint32_t>(resources_.size());
        ResourceNode node;
        node.kind = ResourceKind::Buffer;
        node.generation = allocateResourceGeneration();
        node.alive = true;
        node.imported = false;
        node.exported = false;
        node.buffer = std::move(desc);
        resources_.push_back(std::move(node));
    }
    return BufferHandle{index, resources_[index].generation};
}

TextureHandle RenderGraph::createTexture(TextureDesc desc)
{
    std::uint32_t index = kInvalidIndex;
    if (!freeTextureSlots_.empty())
    {
        index = freeTextureSlots_.back();
        freeTextureSlots_.pop_back();
        auto& node = resources_[index];
        node.kind = ResourceKind::Texture;
        node.generation = allocateResourceGeneration();
        node.alive = true;
        node.imported = false;
        node.exported = false;
        node.texture = std::move(desc);
        node.buffer = {};
    }
    else
    {
        index = static_cast<std::uint32_t>(resources_.size());
        ResourceNode node;
        node.kind = ResourceKind::Texture;
        node.generation = allocateResourceGeneration();
        node.alive = true;
        node.imported = false;
        node.exported = false;
        node.texture = std::move(desc);
        resources_.push_back(std::move(node));
    }
    return TextureHandle{index, resources_[index].generation};
}

BufferHandle RenderGraph::importBuffer(BufferDesc desc)
{
    desc.transient = false;
    const auto handle = createBuffer(std::move(desc));
    resources_[handle.index()].imported = true;
    return handle;
}

TextureHandle RenderGraph::importTexture(TextureDesc desc)
{
    desc.transient = false;
    const auto handle = createTexture(std::move(desc));
    resources_[handle.index()].imported = true;
    return handle;
}

bool RenderGraph::destroy(BufferHandle handle)
{
    if (!valid(handle))
    {
        return false;
    }
    // Grow the free-list before invalidating the resource.  If allocation
    // fails, the caller still owns a valid handle and can retry safely.
    freeBufferSlots_.push_back(handle.index());
    auto& node = resources_[handle.index()];
    node.alive = false;
    node.exported = false;
    node.generation = allocateResourceGeneration();
    return true;
}

bool RenderGraph::destroy(TextureHandle handle)
{
    if (!valid(handle))
    {
        return false;
    }
    freeTextureSlots_.push_back(handle.index());
    auto& node = resources_[handle.index()];
    node.alive = false;
    node.exported = false;
    node.generation = allocateResourceGeneration();
    return true;
}

PassBuilder RenderGraph::addPass(std::string_view name, bool sideEffect)
{
    PassNode node;
    node.name = std::string(name);
    node.sideEffect = sideEffect;
    node.generation = allocatePassGeneration();
    node.alive = true;
    passes_.push_back(std::move(node));
    const auto handle = makePassHandle(static_cast<std::uint32_t>(passes_.size() - 1));
    return PassBuilder(this, handle);
}

PassHandle RenderGraph::addPass(
    std::string_view name, const SetupCallback& setup, PassExecuteCallback execute)
{
    auto builder = addPass(name);
    builder.setExecute(std::move(execute));
    if (setup)
    {
        setup(builder);
    }
    return builder.handle();
}

bool RenderGraph::markOutput(BufferHandle handle)
{
    return setResourceOutput(ResourceKind::Buffer, handle.index(), handle.generation());
}

bool RenderGraph::markOutput(TextureHandle handle)
{
    return setResourceOutput(ResourceKind::Texture, handle.index(), handle.generation());
}

bool RenderGraph::markOutput(PassHandle handle)
{
    return setPassSideEffectInternal(handle, true);
}

bool RenderGraph::setPassSideEffect(PassHandle handle, bool enabled)
{
    return setPassSideEffectInternal(handle, enabled);
}

bool RenderGraph::setPassExecute(PassHandle handle, PassExecuteCallback callback)
{
    return setPassExecuteInternal(handle, std::move(callback));
}

bool RenderGraph::setPassQueue(PassHandle handle, QueueClass queue)
{
    return setPassQueueInternal(handle, queue);
}

PassBuilder RenderGraph::editPass(PassHandle handle)
{
    if (!valid(handle))
    {
        return {};
    }
    return PassBuilder(this, handle);
}

bool RenderGraph::validResource(
    ResourceKind kind, std::uint32_t index, std::uint32_t generation) const noexcept
{
    if (index == kInvalidIndex || index >= resources_.size() || generation == 0)
    {
        return false;
    }
    const auto& node = resources_[index];
    return node.alive && node.kind == kind && node.generation == generation;
}

bool RenderGraph::valid(BufferHandle handle) const noexcept
{
    return validResource(ResourceKind::Buffer, handle.index(), handle.generation());
}

bool RenderGraph::valid(TextureHandle handle) const noexcept
{
    return validResource(ResourceKind::Texture, handle.index(), handle.generation());
}

bool RenderGraph::validPass(PassHandle handle) const noexcept
{
    return handle.valid() && handle.index() < passes_.size() && passes_[handle.index()].alive &&
           passes_[handle.index()].generation == handle.generation();
}

bool RenderGraph::valid(PassHandle handle) const noexcept
{
    return validPass(handle);
}

std::size_t RenderGraph::bufferCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const ResourceNode& node)
        {
            return node.alive && node.kind == ResourceKind::Buffer;
        }));
}

std::size_t RenderGraph::textureCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const ResourceNode& node)
        {
            return node.alive && node.kind == ResourceKind::Texture;
        }));
}

std::size_t RenderGraph::passCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(passes_.begin(),
        passes_.end(),
        [](const PassNode& node)
        {
            return node.alive;
        }));
}

bool RenderGraph::addAccess(PassHandle pass,
    ResourceKind kind,
    std::uint32_t index,
    std::uint32_t generation,
    AccessMode mode,
    ResourceUsage usage)
{
    if (!validPass(pass))
    {
        return false;
    }
    // Keep invalid handles in the declaration so compile() can return a
    // useful diagnostic instead of silently dropping a pass access.  Valid
    // duplicate declarations are merged here: barriers are emitted before a
    // pass executes, so treating read() followed by write() as two sequential
    // operations would describe an impossible intra-pass transition.
    auto& accesses = passes_[pass.index()].accesses;
    const auto existing = std::find_if(accesses.begin(),
        accesses.end(),
        [&](const ResourceAccess& access)
        {
            return access.kind == kind && access.resourceIndex == index &&
                   access.resourceGeneration == generation;
        });
    if (existing != accesses.end())
    {
        const bool reads =
            existing->reads() || mode == AccessMode::Read || mode == AccessMode::ReadWrite;
        const bool writes =
            existing->writes() || mode == AccessMode::Write || mode == AccessMode::ReadWrite;
        existing->mode = reads && writes ? AccessMode::ReadWrite
                                         : (writes ? AccessMode::Write : AccessMode::Read);
        existing->usage |= usage;
        return true;
    }
    accesses.push_back(ResourceAccess{kind, index, generation, mode, usage});
    return true;
}

bool RenderGraph::addDependency(PassHandle pass, PassHandle dependency)
{
    if (!validPass(pass))
    {
        return false;
    }
    // Invalid dependencies are retained for compile-time diagnostics.
    passes_[pass.index()].explicitDependencies.push_back(dependency);
    return true;
}

bool RenderGraph::setResourceOutput(
    ResourceKind kind, std::uint32_t index, std::uint32_t generation)
{
    if (!validResource(kind, index, generation))
    {
        return false;
    }
    resources_[index].exported = true;
    return true;
}

bool RenderGraph::setPassSideEffectInternal(PassHandle pass, bool enabled)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].sideEffect = enabled;
    return true;
}

bool RenderGraph::setPassQueueInternal(PassHandle pass, QueueClass queue)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].queue = queue;
    return true;
}

bool RenderGraph::setPassExecuteInternal(PassHandle pass, PassExecuteCallback callback)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].execute = std::move(callback);
    return true;
}

PassHandle RenderGraph::makePassHandle(std::uint32_t index) const noexcept
{
    if (index >= passes_.size())
    {
        return {};
    }
    return PassHandle{index, passes_[index].generation};
}

std::uint32_t RenderGraph::allocateResourceGeneration() noexcept
{
    const auto generation = nextResourceGeneration_;
    ++nextResourceGeneration_;
    if (nextResourceGeneration_ == 0)
    {
        nextResourceGeneration_ = 1;
    }
    return generation;
}

std::uint32_t RenderGraph::allocatePassGeneration() noexcept
{
    const auto generation = nextPassGeneration_;
    ++nextPassGeneration_;
    if (nextPassGeneration_ == 0)
    {
        nextPassGeneration_ = 1;
    }
    return generation;
}

CompileResult RenderGraph::compile(const CompileOptions& options) const
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
    std::vector<std::vector<std::uint32_t>> successors(passCount);
    // A WAR edge is required for execution ordering, but it does not mean
    // that the reader produces data needed by the writer.  Keep a second
    // predecessor list containing only data/explicit dependencies so dead
    // pass culling does not accidentally retain such readers.
    std::vector<std::vector<std::uint32_t>> cullingPredecessors(passCount);

    auto addEdge = [&](std::uint32_t from, std::uint32_t to, bool keepsAlive)
    {
        if (from == to || from >= passCount || to >= passCount || !passes_[from].alive ||
            !passes_[to].alive)
        {
            return;
        }
        auto& outgoing = successors[from];
        if (std::find(outgoing.begin(), outgoing.end(), to) == outgoing.end())
        {
            outgoing.push_back(to);
        }
        if (keepsAlive &&
            std::find(cullingPredecessors[to].begin(), cullingPredecessors[to].end(), from) ==
                cullingPredecessors[to].end())
        {
            cullingPredecessors[to].push_back(from);
        }
    };
    // Explicit dependencies are user-authored and a self dependency is a
    // genuine cycle (unlike an access merged within one pass, which is
    // intentionally handled without a self edge).
    auto addExplicitEdge = [&](std::uint32_t from, std::uint32_t to)
    {
        if (from == to && from < passCount && passes_[from].alive)
        {
            successors[from].push_back(to);
            cullingPredecessors[to].push_back(from);
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
        for (const auto next : successors[node])
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

    std::vector<bool> live(passCount, false);
    std::deque<std::uint32_t> work;
    for (std::uint32_t i = 0; i < passCount; ++i)
    {
        if (passes_[i].alive && (!options.cullDeadPasses || passes_[i].sideEffect))
        {
            live[i] = true;
            work.push_back(i);
        }
    }

    if (options.cullDeadPasses)
    {
        // Exporting a resource roots its latest writer.  Reverse traversal of
        // the hazard graph then keeps every producer needed by that writer.
        for (std::uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex)
        {
            const auto& resource = resources_[resourceIndex];
            if (!resource.alive || !resource.exported)
            {
                continue;
            }
            std::int32_t latestWriter = -1;
            for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
            {
                if (!passes_[passIndex].alive)
                {
                    continue;
                }
                for (const auto& access : passes_[passIndex].accesses)
                {
                    if (access.resourceIndex == resourceIndex &&
                        access.resourceGeneration == resource.generation &&
                        access.kind == resource.kind && access.writes())
                    {
                        latestWriter = static_cast<std::int32_t>(passIndex);
                    }
                }
            }
            if (latestWriter >= 0 && !live[static_cast<std::uint32_t>(latestWriter)])
            {
                live[static_cast<std::uint32_t>(latestWriter)] = true;
                work.push_back(static_cast<std::uint32_t>(latestWriter));
            }
        }
        while (!work.empty())
        {
            const auto node = work.front();
            work.pop_front();
            for (const auto predecessor : cullingPredecessors[node])
            {
                if (!live[predecessor])
                {
                    live[predecessor] = true;
                    work.push_back(predecessor);
                }
            }
        }
    }
    else
    {
        for (std::uint32_t i = 0; i < passCount; ++i)
        {
            live[i] = passes_[i].alive;
        }
    }

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
        for (const auto to : successors[from])
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
        for (const auto next : successors[node])
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

    // Compute first/last use in execution order.  Culling is applied before
    // lifetime analysis so dead passes cannot artificially extend allocations.
    for (std::int32_t position = 0;
        position < static_cast<std::int32_t>(result.executionOrder.size());
        ++position)
    {
        const auto passHandle = result.executionOrder[static_cast<std::size_t>(position)];
        const auto& pass = passes_[passHandle.index()];
        for (const auto& access : pass.accesses)
        {
            auto& resource = result.resources[access.resourceIndex];
            auto& lifetime = resource.lifetime;
            if (lifetime.firstUse < 0)
            {
                lifetime.firstUse = position;
                lifetime.firstPass = passHandle;
            }
            lifetime.lastUse = position;
            lifetime.lastPass = passHandle;
        }
    }

    result.success = true;
    result.error = {};
    return result;
}

void RenderGraph::clear()
{
    resources_.clear();
    freeBufferSlots_.clear();
    freeTextureSlots_.clear();
    passes_.clear();
}

} // namespace Halcyon::Renderer::Graph
