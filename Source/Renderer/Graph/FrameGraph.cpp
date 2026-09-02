#include "Halcyon/FrameGraph.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <sstream>

namespace Halcyon::Renderer::Graph
{

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
    std::vector<std::uint32_t> r;
    r.reserve(size());
    for (const auto& p : predecessors_)
    {
        r.push_back(static_cast<std::uint32_t>(p.size()));
    }
    return r;
}

namespace
{
template <typename T>
FrameGraphHandle makeId(const T& h)
{
    return FrameGraphHandle(h.index(), h.version(), h.epoch());
}
} // namespace

FrameGraphHandle FrameGraph::createRaw(
    ResourceKind kind, std::string_view name, const BufferDescriptor& d)
{
    std::size_t slot = resources_.size();
    for (std::size_t i = 0; i < resources_.size(); ++i)
    {
        if (!resources_[i].alive && resources_[i].kind == kind)
        {
            slot = i;
            break;
        }
    }
    Resource r;
    r.kind = kind;
    r.index = static_cast<std::uint32_t>(slot);
    r.version = nextVersion_++;
    r.name = name.empty() ? d.name : std::string(name);
    r.buffer = d;
    r.buffer.name = r.name;
    r.bufferObject.descriptor = r.buffer;
    if (slot == resources_.size())
    {
        resources_.push_back(std::move(r));
    }
    else
    {
        resources_[slot] = std::move(r);
    }
    return FrameGraphHandle(static_cast<std::uint32_t>(slot), resources_[slot].version, epoch_);
}
FrameGraphHandle FrameGraph::createRaw(
    ResourceKind kind, std::string_view name, const TextureDescriptor& d)
{
    std::size_t slot = resources_.size();
    for (std::size_t i = 0; i < resources_.size(); ++i)
    {
        if (!resources_[i].alive && resources_[i].kind == kind)
        {
            slot = i;
            break;
        }
    }
    Resource r;
    r.kind = kind;
    r.index = static_cast<std::uint32_t>(slot);
    r.version = nextVersion_++;
    r.name = name.empty() ? d.name : std::string(name);
    r.texture = d;
    r.texture.name = r.name;
    r.textureObject.descriptor = r.texture;
    if (slot == resources_.size())
    {
        resources_.push_back(std::move(r));
    }
    else
    {
        resources_[slot] = std::move(r);
    }
    return FrameGraphHandle(static_cast<std::uint32_t>(slot), resources_[slot].version, epoch_);
}
FrameGraphHandle FrameGraph::createSubresourceRaw(
    FrameGraphHandle parent, std::string_view name, const BufferSubresourceDescriptor&)
{
    if (!isValid(parent))
    {
        return {};
    }
    const auto& p = resources_[parent.index()];
    BufferDescriptor d = p.buffer;
    d.name = std::string(name);
    const auto h = createRaw(ResourceKind::Buffer, name, d);
    resources_[h.index()].parent = parent.index();
    return h;
}
FrameGraphHandle FrameGraph::createSubresourceRaw(
    FrameGraphHandle parent, std::string_view name, const TextureSubresourceDescriptor&)
{
    if (!isValid(parent))
    {
        return {};
    }
    const auto& p = resources_[parent.index()];
    TextureDescriptor d = p.texture;
    d.name = std::string(name);
    const auto h = createRaw(ResourceKind::Texture, name, d);
    resources_[h.index()].parent = parent.index();
    return h;
}

FrameGraphId<FrameGraphBuffer> FrameGraph::createBuffer(BufferDesc d)
{
    auto h = createRaw(ResourceKind::Buffer, d.name, d);
    return {h.index(), h.version(), epoch_};
}
FrameGraphId<FrameGraphTexture> FrameGraph::createTexture(TextureDesc d)
{
    auto h = createRaw(ResourceKind::Texture, d.name, d);
    return {h.index(), h.version(), epoch_};
}
FrameGraphId<FrameGraphBuffer> FrameGraph::importBuffer(BufferDesc d)
{
    d.transient = false;
    auto h = createBuffer(std::move(d));
    resources_[h.index()].imported = true;
    return h;
}
FrameGraphId<FrameGraphTexture> FrameGraph::importTexture(TextureDesc d)
{
    d.transient = false;
    auto h = createTexture(std::move(d));
    resources_[h.index()].imported = true;
    return h;
}
bool FrameGraph::destroy(BufferHandle h) noexcept
{
    if (!isValid(h) || resources_[h.index()].kind != ResourceKind::Buffer)
    {
        return false;
    }
    resources_[h.index()].alive = false;
    resources_[h.index()].current = false;
    return true;
}
bool FrameGraph::destroy(TextureHandle h) noexcept
{
    if (!isValid(h) || resources_[h.index()].kind != ResourceKind::Texture)
    {
        return false;
    }
    resources_[h.index()].alive = false;
    resources_[h.index()].current = false;
    return true;
}

FrameGraph::Builder FrameGraph::addPass(std::string_view name, bool sideEffect)
{
    auto p = std::make_unique<FrameGraphPass<Empty, std::function<void()>>>(Empty{}, [] {});
    const auto h = createPassRaw(name, std::move(p));
    setPassSideEffectInternal(h, sideEffect);
    return Builder(this, h);
}
FrameGraphHandle FrameGraph::createPassRaw(
    std::string_view name, std::unique_ptr<FrameGraphPassBase> object)
{
    const auto index = static_cast<std::uint32_t>(passes_.size());
    Pass p;
    p.object = std::move(object);
    p.object->name_ = std::string(name);
    p.object->handle_ = FrameGraphHandle(index, index + 1, epoch_);
    passes_.push_back(std::move(p));
    return passes_.back().object->handle_;
}
void FrameGraph::Builder::readRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m)
{
    if (graph_)
    {
        graph_->addAccessRaw(pass_, h, m, u);
        const auto v = graph_->accessVersion(h, AccessMode::Read, u);
        lastAccess_ = v;
    }
}
void FrameGraph::Builder::writeRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m)
{
    if (graph_)
    {
        graph_->addAccessRaw(pass_, h, m, u);
        const auto v = graph_->accessVersion(h, m, u);
        graph_->setProducerRaw(v, pass_);
        lastAccess_ = v;
    }
}
std::string_view FrameGraph::Builder::getName(FrameGraphHandle h) const noexcept
{
    return graph_ && graph_->isValid(h) ? graph_->resources_[h.index()].name : std::string_view{};
}

FrameGraphHandle FrameGraph::accessVersion(
    FrameGraphHandle input, AccessMode mode, ResourceUsage usage)
{
    if (!isValid(input))
    {
        lastError_ = {GraphErrorCode::InvalidHandle, "invalid frame graph resource handle", {}};
        return {};
    }
    resources_[input.index()].current = false;
    Resource copy = resources_[input.index()];
    copy.index = static_cast<std::uint32_t>(resources_.size());
    copy.aliasOf = input.index();
    copy.version = nextVersion_++;
    copy.current = true;
    copy.usage = usage;
    copy.producer = mode == AccessMode::Write ? -1 : copy.producer;
    copy.exported = false;
    resources_.push_back(std::move(copy));
    return FrameGraphHandle(resources_.back().index, resources_.back().version, epoch_);
}
void FrameGraph::addAccessRaw(
    PassHandle pass, FrameGraphHandle h, AccessMode mode, ResourceUsage usage)
{
    if (pass.index() >= passes_.size())
    {
        return;
    }
    passes_[pass.index()].accesses.push_back(ResourceAccess{
        h.index() < resources_.size() ? resources_[h.index()].kind : ResourceKind::Buffer,
        h.index(),
        h.generation(),
        h.version(),
        mode,
        usage});
    if (h.index() < resources_.size())
    {
        resources_[h.index()].usage |= usage;
        resources_[h.index()].bufferObject.native = resources_[h.index()].native;
        resources_[h.index()].textureObject.native = resources_[h.index()].native;
    }
    if (mode == AccessMode::Write || mode == AccessMode::ReadWrite)
    {
        setProducerRaw(h, pass);
    }
}
void FrameGraph::setProducerRaw(FrameGraphHandle h, PassHandle p)
{
    if (h.index() < resources_.size())
    {
        resources_[h.index()].producer = static_cast<int>(p.index());
    }
}
void FrameGraph::dependencyRaw(PassHandle p, PassHandle d)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].deps.push_back(d);
    }
}
void FrameGraph::setPassSideEffectInternal(PassHandle p, bool enabled)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].object->sideEffect_ = enabled;
    }
}
void FrameGraph::setQueueRaw(PassHandle p, QueueClass q)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].object->queue_ = q;
    }
}
void FrameGraph::setExecuteRaw(PassHandle p, PassExecuteCallback cb)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].legacyExecute = std::move(cb);
    }
}
void FrameGraph::importTokenRaw(FrameGraphHandle h, FrameGraphNativeResource token)
{
    if (h.index() < resources_.size())
    {
        resources_[h.index()].native = token;
        resources_[h.index()].imported = true;
        resources_[h.index()].bufferObject.native = token;
        resources_[h.index()].textureObject.native = token;
    }
}
void FrameGraph::usageRaw(FrameGraphHandle h, ResourceUsage u)
{
    if (h.index() < resources_.size())
    {
        resources_[h.index()].usage = u;
    }
}
void FrameGraph::presentRaw(FrameGraphHandle h) noexcept
{
    if (!isValid(h))
    {
        return;
    }
    resources_[h.index()].exported = true;
    auto p = addPass("Present", true);
    if (resources_[h.index()].kind == ResourceKind::Texture)
    {
        p.read(TextureHandle(h.index(), h.version(), epoch_), ResourceUsage::Present);
    }
    else
    {
        p.read(BufferHandle(h.index(), h.version(), epoch_), ResourceUsage::Present);
    }
}
void FrameGraph::forwardRaw(FrameGraphHandle a, FrameGraphHandle b)
{
    if (isValid(a) && isValid(b))
    {
        resources_[a.index()].current = false;
        resources_[a.index()].aliasOf = b.index();
        resources_[a.index()].exported = resources_[b.index()].exported;
    }
}

bool FrameGraph::isValid(FrameGraphHandle h) const noexcept
{
    if (h.epoch() != epoch_ || !h.isInitialized())
    {
        return false;
    }
    return h.index() < resources_.size() && resources_[h.index()].alive &&
           resources_[h.index()].current && resources_[h.index()].version == h.version();
}
bool FrameGraph::isCulled(const FrameGraphPassBase& p) const noexcept
{
    return p.culled_;
}
std::size_t FrameGraph::bufferCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const Resource& r)
        {
            return r.alive && r.kind == ResourceKind::Buffer;
        }));
}
std::size_t FrameGraph::textureCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const Resource& r)
        {
            return r.alive && r.kind == ResourceKind::Texture;
        }));
}
std::size_t FrameGraph::passCount() const noexcept
{
    return passes_.size();
}

FrameGraph& FrameGraph::compile() noexcept
{
    compiled_ = {};
    lastError_ = {};
    graph_.reset(passes_.size());
    compiled_.passes.resize(passes_.size());
    compiled_.resources.resize(resources_.size());
    for (std::size_t i = 0; i < resources_.size(); ++i)
    {
        const auto& r = resources_[i];
        auto& d = compiled_.resources[i];
        d.kind = r.kind;
        d.index = r.index;
        d.generation = r.version;
        d.version = r.version;
        d.name = r.name;
        d.imported = r.imported;
        d.exported = r.exported;
        d.buffer = r.buffer;
        d.texture = r.texture;
    }
    for (std::size_t i = 0; i < passes_.size(); ++i)
    {
        auto& d = compiled_.passes[i];
        const auto& p = passes_[i];
        d.handle = p.object->handle_;
        d.name = std::string(p.object->name_);
        d.queue = p.object->queue_;
        d.sideEffect = p.object->sideEffect_;
        d.accesses = p.accesses;
        d.execute = p.legacyExecute;
    }
    auto add = [&](std::uint32_t a, std::uint32_t b)
    {
        graph_.addEdge(a, b);
    };
    struct AccessState
    {
        int writer = -1;
        std::vector<std::uint32_t> readers;
    };
    std::vector<AccessState> states(resources_.size());
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        for (auto dep : passes_[i].deps)
        {
            if (dep.index() >= passes_.size() || dep.epoch() != epoch_ ||
                passes_[dep.index()].object->handle().version() != dep.version())
            {
                lastError_ = {GraphErrorCode::InvalidHandle, "invalid pass dependency", {}};
                compiled_.error = lastError_;
                error = lastError_;
                return *this;
            }
            add(dep.index(), i);
        }
        for (const auto& a : passes_[i].accesses)
        {
            if (a.resourceIndex >= resources_.size() ||
                resources_[a.resourceIndex].version != a.resourceVersion)
            {
                lastError_ = {GraphErrorCode::InvalidHandle, "invalid resource declaration", {}};
                compiled_.error = lastError_;
                error = lastError_;
                return *this;
            }
            const int producer = resources_[a.resourceIndex].producer;
            if (producer >= 0 && producer != static_cast<int>(i))
            {
                add(static_cast<std::uint32_t>(producer), i);
            }
            auto& state = states[a.resourceIndex];
            if (a.reads() && state.writer >= 0 && state.writer != static_cast<int>(i))
            {
                add(static_cast<std::uint32_t>(state.writer), i);
            }
            if (a.writes())
            {
                if (state.writer >= 0 && state.writer != static_cast<int>(i))
                {
                    add(static_cast<std::uint32_t>(state.writer), i);
                }
                for (const auto reader : state.readers)
                {
                    if (reader != i)
                    {
                        add(reader, i);
                    }
                }
                state.readers.clear();
                state.writer = static_cast<int>(i);
            }
            else if (a.reads() && std::find(state.readers.begin(), state.readers.end(), i) ==
                                      state.readers.end())
            {
                state.readers.push_back(i);
            }
        }
    }
    {
        auto indeg = graph_.indegrees();
        std::deque<std::uint32_t> q;
        for (std::uint32_t i = 0; i < indeg.size(); ++i)
        {
            if (indeg[i] == 0)
            {
                q.push_back(i);
            }
        }
        std::size_t visited = 0;
        while (!q.empty())
        {
            auto n = q.front();
            q.pop_front();
            ++visited;
            for (auto s : graph_.successors(n))
            {
                if (--indeg[s] == 0)
                {
                    q.push_back(s);
                }
            }
        }
        if (visited != passes_.size())
        {
            lastError_ = {GraphErrorCode::CycleDetected, "frame graph contains a cycle", {}};
            if (!passes_.empty())
            {
                lastError_.cycle.push_back(passes_.front().object->handle_);
            }
            compiled_.error = lastError_;
            error = lastError_;
            return *this;
        }
    }
    std::vector<bool> live(passes_.size(), false);
    std::deque<std::uint32_t> work;
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        if (passes_[i].object->sideEffect_)
        {
            live[i] = true;
            work.push_back(i);
        }
    }
    for (std::uint32_t i = 0; i < resources_.size(); ++i)
    {
        if (resources_[i].exported && resources_[i].producer >= 0 && !live[resources_[i].producer])
        {
            live[resources_[i].producer] = true;
            work.push_back(static_cast<std::uint32_t>(resources_[i].producer));
        }
    }
    while (!work.empty())
    {
        auto n = work.front();
        work.pop_front();
        for (auto p : graph_.predecessors(n))
        {
            if (!live[p])
            {
                live[p] = true;
                work.push_back(p);
            }
        }
    }
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        compiled_.passes[i].culled = !live[i];
        passes_[i].object->culled_ = !live[i];
    }
    std::vector<std::uint32_t> indegree(passes_.size());
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        if (live[i])
        {
            for (auto n : graph_.successors(i))
            {
                if (live[n])
                {
                    ++indegree[n];
                }
            }
        }
    }
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        if (live[i] && indegree[i] == 0)
        {
            ready.push_back(i);
        }
    }
    while (!ready.empty())
    {
        auto it = std::min_element(ready.begin(), ready.end());
        auto n = *it;
        ready.erase(it);
        compiled_.executionOrder.push_back(passes_[n].object->handle_);
        for (auto s : graph_.successors(n))
        {
            if (live[s] && --indegree[s] == 0)
            {
                ready.push_back(s);
            }
        }
    }
    if (compiled_.executionOrder.size() !=
        static_cast<std::size_t>(std::count(live.begin(), live.end(), true)))
    {
        lastError_ = {GraphErrorCode::CycleDetected, "frame graph contains a cycle", {}};
        if (!passes_.empty())
        {
            lastError_.cycle.push_back(passes_.front().object->handle_);
        }
        compiled_.error = lastError_;
        error = lastError_;
        return *this;
    }
    for (std::size_t pos = 0; pos < compiled_.executionOrder.size(); ++pos)
    {
        auto h = compiled_.executionOrder[pos];
        for (const auto& a : passes_[h.index()].accesses)
        {
            auto& l = compiled_.resources[a.resourceIndex].lifetime;
            if (l.firstUse < 0)
            {
                l.firstUse = static_cast<int>(pos);
                l.firstPass = h;
            }
            l.lastUse = static_cast<int>(pos);
            l.lastPass = h;
            resources_[a.resourceIndex].lifetime = l;
        }
    }
    compiled_.success = true;
    compiled_.error = {};
    executionOrder = compiled_.executionOrder;
    error = compiled_.error;
    return *this;
}

void FrameGraph::execute(CommandContext& commands) noexcept
{
    if (!compiled_.success)
    {
        return;
    }
    lastError_ = {};
    for (std::size_t i = 0; i < compiled_.executionOrder.size(); ++i)
    {
        for (auto& r : resources_)
        {
            if (r.lifetime.firstUse == static_cast<std::int32_t>(i) && !r.imported && !r.detached &&
                provider_)
            {
                FrameGraphResourceCreateInfo ci;
                ci.kind = r.kind;
                ci.buffer = r.buffer;
                ci.texture = r.texture;
                if (!provider_->create(ci, r.native))
                {
                    lastError_ = {
                        GraphErrorCode::ExecutionFailed, "transient resource creation failed", {}};
                    return;
                }
                r.bufferObject.native = r.native;
                r.textureObject.native = r.native;
                r.materialized = true;
            }
        }
        auto h = compiled_.executionOrder[i];
        if (h.index() >= passes_.size() || compiled_.passes[h.index()].culled)
        {
            continue;
        }
        auto& p = passes_[h.index()];
        commands.name_ = p.object->name_;
        commands.queue_ = p.object->queue_;
        commands.executionIndex_ = static_cast<std::uint32_t>(i);
        FrameGraphResources resources(this, h);
        p.object->execute(resources, commands);
        if (p.object->failed())
        {
            lastError_ = {GraphErrorCode::ExecutionFailed,
                std::string(p.object->errorMessage().empty() ? "frame graph pass execution failed"
                                                             : p.object->errorMessage()),
                {}};
            return;
        }
        for (auto& r : resources_)
        {
            if (r.lifetime.lastUse == static_cast<std::int32_t>(i) && !r.imported && !r.detached &&
                provider_)
            {
                provider_->destroy(r.native);
                r.materialized = false;
            }
        }
    }
}
CompileResult::ExecutionResult FrameGraph::execute(const ExecuteOptions& options) const
{
    return compiled_.execute(options);
}
void FrameGraph::reset() noexcept
{
    if (provider_)
    {
        for (const auto& resource : resources_)
        {
            if (!resource.imported && !resource.detached && resource.materialized)
            {
                provider_->destroy(resource.native);
            }
        }
    }
    resources_.clear();
    passes_.clear();
    compiled_ = {};
    executionOrder.clear();
    error = {};
    blackboard_.clear();
    ++epoch_;
    if (epoch_ == 0)
    {
        epoch_ = 1;
    }
}
bool FrameGraph::isAcyclic() const noexcept
{
    auto* self = const_cast<FrameGraph*>(this);
    self->compile();
    return self->compiled_.error.code != GraphErrorCode::CycleDetected;
}
void FrameGraph::exportGraphviz(std::ostream& out, std::string_view name) const noexcept
{
    out << "digraph " << (name.empty() ? "FrameGraph" : name) << " {\n";
    for (std::size_t i = 0; i < passes_.size(); ++i)
    {
        out << "  p" << i << " [shape=box,label=\"" << passes_[i].object->name_ << "\"];\n";
    }
    for (std::size_t i = 0; i < resources_.size(); ++i)
    {
        out << "  r" << i << " [shape=ellipse,label=\"" << resources_[i].name << "\"];\n";
    }
    for (std::size_t i = 0; i < passes_.size(); ++i)
    {
        for (auto s : graph_.successors(static_cast<std::uint32_t>(i)))
        {
            out << "  p" << i << " -> p" << s << ";\n";
        }
        for (const auto& a : passes_[i].accesses)
        {
            if (a.resourceIndex < resources_.size())
            {
                if (a.reads())
                {
                    out << "  r" << a.resourceIndex << " -> p" << i << ";\n";
                }
                if (a.writes())
                {
                    out << "  p" << i << " -> r" << a.resourceIndex << ";\n";
                }
            }
        }
    }
    out << "}\n";
}

const void* FrameGraph::resourceRaw(FrameGraphHandle h, ResourceKind kind) const noexcept
{
    if (h.epoch() != epoch_ || !h.isInitialized() || h.index() >= resources_.size() ||
        !resources_[h.index()].alive || resources_[h.index()].version != h.version() ||
        resources_[h.index()].kind != kind)
    {
        return nullptr;
    }
    return kind == ResourceKind::Texture
               ? static_cast<const void*>(&resources_[h.index()].textureObject)
               : static_cast<const void*>(&resources_[h.index()].bufferObject);
}
ResourceUsage FrameGraph::resourceUsageRaw(FrameGraphHandle h) const noexcept
{
    return h.index() < resources_.size() && resources_[h.index()].version == h.version()
               ? resources_[h.index()].usage
               : ResourceUsage::None;
}
const void* FrameGraphResources::getRaw(FrameGraphHandle h, ResourceKind kind) const noexcept
{
    return graph_ && graph_->declaredRaw(pass_, h) ? graph_->resourceRaw(h, kind) : nullptr;
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
bool FrameGraph::declaredRaw(FrameGraphHandle pass, FrameGraphHandle resource) const noexcept
{
    if (pass.index() >= passes_.size())
    {
        return false;
    }
    const auto& accesses = passes_[pass.index()].accesses;
    return std::any_of(accesses.begin(),
        accesses.end(),
        [&](const ResourceAccess& a)
        {
            if (a.resourceIndex == resource.index() && a.resourceVersion == resource.version())
            {
                return true;
            }
            std::uint32_t alias = resource.index();
            for (std::size_t depth = 0; depth < resources_.size() && alias < resources_.size() &&
                                        resources_[alias].aliasOf != kInvalidIndex;
                ++depth)
            {
                alias = resources_[alias].aliasOf;
                if (a.resourceIndex == alias)
                {
                    return true;
                }
            }
            return false;
        });
}

const CompileResult::CompiledPass* CompileResult::pass(FrameGraphHandle h) const noexcept
{
    return h.index() < passes.size() && passes[h.index()].handle.version() == h.version()
               ? &passes[h.index()]
               : nullptr;
}
const ResourceLifetime* CompileResult::lifetime(BufferHandle h) const noexcept
{
    return h.index() < resources.size() && resources[h.index()].kind == ResourceKind::Buffer &&
                   resources[h.index()].generation == h.version()
               ? &resources[h.index()].lifetime
               : nullptr;
}
const ResourceLifetime* CompileResult::lifetime(TextureHandle h) const noexcept
{
    return h.index() < resources.size() && resources[h.index()].kind == ResourceKind::Texture &&
                   resources[h.index()].generation == h.version()
               ? &resources[h.index()].lifetime
               : nullptr;
}
CompileResult::ExecutionResult CompileResult::execute(const ExecuteOptions& options) const
{
    ExecutionResult r;
    if (!success)
    {
        r.error = error;
        return r;
    }
    try
    {
        for (std::size_t i = 0; i < executionOrder.size(); ++i)
        {
            auto h = executionOrder[i];
            const auto* p = pass(h);
            if (!p || p->culled)
            {
                continue;
            }
            PassExecutionContext c{h, p->name, static_cast<std::uint32_t>(i), options.userData};
            if (options.onBegin)
            {
                options.onBegin(c);
            }
            if (p->execute)
            {
                p->execute(c);
            }
            if (options.onEnd)
            {
                options.onEnd(c);
            }
            ++r.executedPasses;
        }
        r.success = true;
    }
    catch (const std::exception& e)
    {
        r.error = {GraphErrorCode::ExecutionFailed, e.what(), {}};
    }
    catch (...)
    {
        r.error = {GraphErrorCode::ExecutionFailed, "frame graph execution failed", {}};
    }
    return r;
}
CompileResult::ExecutionResult CompileResult::execute() const
{
    return execute(ExecuteOptions{});
}

} // namespace Halcyon::Renderer::Graph
