#include "FrameGraph.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <sstream>

namespace Halcyon::Renderer::Graph
{

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
    if (slot >= resourceNodes_.size())
    {
        resourceNodes_.resize(slot + 1u);
    }
    resourceNodes_[slot] = std::make_unique<ResourceNode>(
        FrameGraphHandle(static_cast<std::uint32_t>(slot), resources_[slot].version, epoch_));
    resourceNodes_[slot]->setName(resources_[slot].name);
    resourceNodes_[slot]->setKind(kind);
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
    if (slot >= resourceNodes_.size())
    {
        resourceNodes_.resize(slot + 1u);
    }
    resourceNodes_[slot] = std::make_unique<ResourceNode>(
        FrameGraphHandle(static_cast<std::uint32_t>(slot), resources_[slot].version, epoch_));
    resourceNodes_[slot]->setName(resources_[slot].name);
    resourceNodes_[slot]->setKind(kind);
    return FrameGraphHandle(static_cast<std::uint32_t>(slot), resources_[slot].version, epoch_);
}
FrameGraphHandle FrameGraph::createSubresourceRaw(
    FrameGraphHandle parent, std::string_view name, const BufferSubresourceDescriptor& descriptor)
{
    if (!isValid(parent))
    {
        return {};
    }
    const auto& p = resources_[parent.index()];
    BufferDescriptor d = p.buffer;
    if (descriptor.offset < d.size)
    {
        d.size = descriptor.size == 0 ? d.size - descriptor.offset
                                      : std::min(d.size - descriptor.offset, descriptor.size);
    }
    d.name = std::string(name);
    const auto h = createRaw(ResourceKind::Buffer, name, d);
    resources_[h.index()].parent = parent.index();
    resources_[h.index()].bufferSubresource = descriptor;
    if (h.index() < resourceNodes_.size() && resourceNodes_[h.index()])
    {
        resourceNodes_[h.index()]->setParentHandle(parent);
        if (parent.index() < resourceNodes_.size())
        {
            resourceNodes_[h.index()]->setParentNode(resourceNodes_[parent.index()].get());
        }
    }
    return h;
}
FrameGraphHandle FrameGraph::createSubresourceRaw(
    FrameGraphHandle parent, std::string_view name, const TextureSubresourceDescriptor& descriptor)
{
    if (!isValid(parent))
    {
        return {};
    }
    const auto& p = resources_[parent.index()];
    TextureDescriptor d = FrameGraphTexture::generateSubResourceDescriptor(p.texture, descriptor);
    d.name = std::string(name);
    const auto h = createRaw(ResourceKind::Texture, name, d);
    resources_[h.index()].parent = parent.index();
    resources_[h.index()].textureSubresource = descriptor;
    if (h.index() < resourceNodes_.size() && resourceNodes_[h.index()])
    {
        resourceNodes_[h.index()]->setParentHandle(parent);
        if (parent.index() < resourceNodes_.size())
        {
            resourceNodes_[h.index()]->setParentNode(resourceNodes_[parent.index()].get());
        }
    }
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
    auto node = std::make_unique<RenderPassNode>(this, std::string(name).c_str(),
        passes_.back().object.get());
    node->setHandle(passes_.back().object->handle_);
    node->setId(index);
    node->setName(name);
    passNodes_.push_back(std::move(node));
    return passes_.back().object->handle_;
}
void FrameGraph::Builder::readRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m)
{
    if (graph_)
    {
        const auto v = graph_->accessVersion(h, AccessMode::Read, u);
        graph_->addAccessRaw(pass_, v, m, u);
        lastAccess_ = v;
    }
}
void FrameGraph::Builder::writeRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m)
{
    if (graph_)
    {
        const auto v = graph_->accessVersion(h, m, u);
        // Read-write consumes the previous version and produces a new one.
        // Keeping the read explicit lets dependency analysis distinguish it
        // from a discardable write-only replacement.
        if (m == AccessMode::ReadWrite &&
            (v.index() != h.index() || v.version() != h.version()))
        {
            graph_->addAccessRaw(pass_, h, AccessMode::Read, u);
        }
        graph_->addAccessRaw(pass_, v, AccessMode::Write, u);
        lastAccess_ = v;
    }
}
std::string_view FrameGraph::Builder::getName(FrameGraphHandle h) const noexcept
{
    return graph_ && graph_->isValid(h) ? graph_->resources_[h.index()].name : std::string_view{};
}

std::uint32_t FrameGraph::Builder::declareRenderPass(
    std::string_view name, const FrameGraphRenderPass::Descriptor& descriptor)
{
    if (graph_ == nullptr || pass_.index() >= graph_->passNodes_.size())
    {
        return 0;
    }
    auto* node = dynamic_cast<RenderPassNode*>(graph_->passNodes_[pass_.index()].get());
    return node != nullptr ? node->declareRenderTarget(name, descriptor) : 0;
}

TextureHandle FrameGraph::Builder::declareRenderPass(
    TextureHandle color, std::uint32_t* index)
{
    const auto written = write(color, ResourceUsage::ColorAttachment);
    FrameGraphRenderPass::Descriptor descriptor{};
    descriptor.attachments.color[0] = written;
    const auto renderPass = declareRenderPass(getName(written), descriptor);
    if (index != nullptr)
    {
        *index = renderPass;
    }
    return written;
}

void FrameGraph::Builder::reserveRenderTargets(std::size_t count) noexcept
{
    if (graph_ != nullptr && pass_.index() < graph_->passNodes_.size())
    {
        if (auto* node = dynamic_cast<RenderPassNode*>(graph_->passNodes_[pass_.index()].get()))
        {
            node->reserveRenderTargets(count);
        }
    }
}

FrameGraphHandle FrameGraph::accessVersion(
    FrameGraphHandle input, AccessMode mode, ResourceUsage usage)
{
    if (!isValid(input))
    {
        lastError_ = {GraphErrorCode::InvalidHandle, "invalid frame graph resource handle", {}};
        return {};
    }
    if (mode == AccessMode::Read)
    {
        resources_[input.index()].usage |= usage;
        return input;
    }
    if (resources_[input.index()].accessCount == 0)
    {
        resources_[input.index()].usage |= usage;
        return input;
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
    copy.accessCount = 0;
    resources_.push_back(std::move(copy));
    resourceNodes_.push_back(std::make_unique<ResourceNode>(
        FrameGraphHandle(resources_.back().index, resources_.back().version, epoch_), input));
    resourceNodes_.back()->setName(resources_.back().name);
    resourceNodes_.back()->setKind(resources_.back().kind);
    resourceNodes_.back()->setParentHandle(input);
    if (input.index() < resourceNodes_.size() - 1u)
    {
        resourceNodes_.back()->setParentNode(resourceNodes_[input.index()].get());
    }
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
        mode,
        usage,
        h.version()});
    if (h.index() < resources_.size())
    {
        resources_[h.index()].usage |= usage;
        ++resources_[h.index()].accessCount;
        resources_[h.index()].bufferObject.native = resources_[h.index()].native;
        resources_[h.index()].textureObject.native = resources_[h.index()].native;
    }
    if (mode == AccessMode::Write || mode == AccessMode::ReadWrite)
    {
        setProducerRaw(h, pass);
    }
    if (pass.index() < passNodes_.size() && h.index() < resourceNodes_.size() &&
        resourceNodes_[h.index()])
    {
        auto* passNode = passNodes_[pass.index()].get();
        auto* resourceNode = resourceNodes_[h.index()].get();
        passNode->registerResource(h, nullptr);
        const bool writer = mode == AccessMode::Write || mode == AccessMode::ReadWrite;
        auto* edge = new ResourceEdgeBase(passNode, resourceNode, writer);
        if (writer)
        {
            resourceNode->setIncomingEdge(edge);
        }
        else
        {
            resourceNode->addOutgoingEdge(edge);
        }
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
        if (p.index() < passNodes_.size() && passNodes_[p.index()])
        {
            passNodes_[p.index()]->setSideEffect(enabled);
        }
    }
}
void FrameGraph::setQueueRaw(PassHandle p, QueueClass q)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].object->queue_ = q;
        if (p.index() < passNodes_.size() && passNodes_[p.index()])
        {
            passNodes_[p.index()]->setQueue(q);
        }
    }
}
void FrameGraph::setExecuteRaw(PassHandle p, PassExecuteCallback cb)
{
    if (p.index() < passes_.size())
    {
        passes_[p.index()].legacyExecute = cb;
        passes_[p.index()].legacyFailed = false;
        passes_[p.index()].legacyError.clear();
        if (p.index() < passNodes_.size() && passNodes_[p.index()])
        {
            auto callback = std::move(cb);
            auto* graph = this;
            const auto passIndex = p.index();
            passNodes_[p.index()]->setExecuteCallback(
                [callback = std::move(callback), graph, passIndex, p](
                    const FrameGraphResources&, CommandContext& commands)
                {
                    if (callback)
                    {
                        try
                        {
                            callback(PassExecutionContext{p, commands.passName(),
                                commands.executionIndex(), nullptr});
                        }
                        catch (const std::exception& e)
                        {
                            graph->passes_[passIndex].legacyFailed = true;
                            graph->passes_[passIndex].legacyError = e.what();
                        }
                        catch (...)
                        {
                            graph->passes_[passIndex].legacyFailed = true;
                            graph->passes_[passIndex].legacyError =
                                "frame graph pass execution failed";
                        }
                    }
                });
        }
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
    if (p.pass_ .index() < passNodes_.size())
    {
        auto presentNode = std::make_unique<PresentPassNode>(this);
        presentNode->setHandle(p.pass_);
        presentNode->setId(p.pass_.index());
        presentNode->resource = h;
        passNodes_[p.pass_.index()] = std::move(presentNode);
    }
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
        // Filament's contract keeps the forwarded resource (a) valid and
        // invalidates the replaced handle (b).
        resources_[b.index()].current = false;
        resources_[b.index()].aliasOf = a.index();
        resources_[a.index()].exported = resources_[a.index()].exported ||
                                         resources_[b.index()].exported;
    }
}

FrameGraphId<FrameGraphTexture> FrameGraph::import(
    std::string_view name,
    const FrameGraphRenderPass::ImportDescriptor& descriptor,
    FrameGraphNativeResource target) noexcept
{
    TextureDescriptor texture{};
    texture.name = std::string(name);
    texture.transient = false;
    const auto raw = createRaw(ResourceKind::Texture, name, texture);
    const auto id = FrameGraphId<FrameGraphTexture>(raw.index(), raw.version(), epoch_);
    importTokenRaw(id, target);
    usageRaw(id, resourceUsageForAttachments(descriptor.attachments));
    resources_[id.index()].imported = true;
    resources_[id.index()].importedRenderTarget = true;
    resources_[id.index()].renderTargetImport = descriptor;
    return id;
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
    for (auto& node : passNodes_)
    {
        if (node)
        {
            node->resetCompileState();
        }
    }
    for (auto& node : resourceNodes_)
    {
        if (node)
        {
            node->clearEdges();
        }
    }
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
    // Versions of a virtual resource alias the same physical allocation.
    // Track hazards by the root resource so a write to a new version is
    // ordered after readers/writers of the previous version.
    auto rootResource = [&](std::uint32_t index)
    {
        std::size_t guard = 0;
        while (index < resources_.size() && resources_[index].aliasOf != kInvalidIndex &&
               guard++ < resources_.size())
        {
            index = resources_[index].aliasOf;
        }
        return index;
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
                resources_[a.resourceIndex].version != a.effectiveVersion())
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
            const auto hazardIndex = static_cast<std::uint32_t>(rootResource(a.resourceIndex));
            auto& state = states[hazardIndex];
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
    // Build the Filament-shaped bipartite node graph. The compact pass graph
    // above remains available for legacy graphviz output, while compilation
    // and culling below use PassNode/ResourceNode connectivity.
    const auto resourceOffset = static_cast<std::uint32_t>(passes_.size());
    nodeGraph_.reset(passes_.size() + resources_.size());
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        nodeGraph_.nodes()[i].pass = true;
        nodeGraph_.nodes()[i].kind = ResourceKind::Buffer;
        nodeGraph_.nodes()[i].name = passes_[i].object->name_;
    }
    for (std::uint32_t i = 0; i < resources_.size(); ++i)
    {
        auto& node = nodeGraph_.nodes()[resourceOffset + i];
        node.pass = false;
        node.kind = resources_[i].kind;
        node.name = resources_[i].name;
    }
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        for (const auto dep : passes_[i].deps)
        {
            if (dep.index() < passes_.size())
            {
                nodeGraph_.addEdge(dep.index(), i);
            }
        }
        for (const auto& access : passes_[i].accesses)
        {
            if (access.resourceIndex >= resources_.size())
            {
                continue;
            }
            const auto resourceNode = resourceOffset + access.resourceIndex;
            if (access.reads() && !access.writes())
            {
                nodeGraph_.addEdge(resourceNode, i);
            }
            if (access.writes())
            {
                nodeGraph_.addEdge(i, resourceNode);
            }
            if (i < passNodes_.size() && access.resourceIndex < resourceNodes_.size() &&
                passNodes_[i] && resourceNodes_[access.resourceIndex])
            {
                auto* passNode = passNodes_[i].get();
                auto* resourceNodeObject = resourceNodes_[access.resourceIndex].get();
                passNode->registerResource(
                    FrameGraphHandle(access.resourceIndex, access.effectiveVersion(), epoch_), nullptr);
                auto* edge = new ResourceEdgeBase(passNode, resourceNodeObject, access.writes());
                if (access.writes())
                {
                    resourceNodeObject->setIncomingEdge(edge);
                }
                else
                {
                    resourceNodeObject->addOutgoingEdge(edge);
                }
            }
        }
    }
    // Preserve all pass hazards in the Filament-shaped graph as pass-to-pass
    // edges. Resource nodes carry the richer attachment metadata, while these
    // edges provide an unambiguous topological order across resource versions.
    for (const auto& edge : graph_.edges())
    {
        nodeGraph_.addEdge(edge.from, edge.to);
    }
    {
        auto indeg = nodeGraph_.indegrees();
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
            for (auto s : nodeGraph_.successors(n))
            {
                if (--indeg[s] == 0)
                {
                    q.push_back(s);
                }
            }
        }
        if (visited != nodeGraph_.size())
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
    if (!config_.cullPasses)
    {
        std::fill(live.begin(), live.end(), true);
    }
    else
    {
        for (std::uint32_t i = 0; i < passes_.size(); ++i)
        {
            if (passes_[i].object->sideEffect_)
            {
                nodeGraph_.makeTarget(i);
            }
        }
        for (std::uint32_t i = 0; i < resources_.size(); ++i)
        {
            if (resources_[i].exported)
            {
                nodeGraph_.makeTarget(resourceOffset + i);
            }
        }
        nodeGraph_.cull();
        for (std::uint32_t i = 0; i < passes_.size(); ++i)
        {
            live[i] = !nodeGraph_.isCulled(i);
        }
    }
    for (std::uint32_t i = 0; i < passes_.size(); ++i)
    {
        compiled_.passes[i].culled = !live[i];
        passes_[i].object->culled_ = !live[i];
        if (i < passNodes_.size() && passNodes_[i])
        {
            passNodes_[i]->setCulled(!live[i]);
        }
    }
    for (std::uint32_t i = 0; i < resources_.size(); ++i)
    {
        if (i < resourceNodes_.size() && resourceNodes_[i])
        {
            resourceNodes_[i]->setCulled(nodeGraph_.isCulled(resourceOffset + i));
            resourceNodes_[i]->setProducer(resources_[i].producer);
        }
    }
    auto indegree = nodeGraph_.indegrees();
    std::vector<std::uint32_t> ready;
    for (std::uint32_t i = 0; i < nodeGraph_.size(); ++i)
    {
        if (indegree[i] == 0)
        {
            ready.push_back(i);
        }
    }
    while (!ready.empty())
    {
        auto it = std::min_element(ready.begin(), ready.end());
        auto n = *it;
        ready.erase(it);
        if (n < passes_.size() && live[n])
        {
            compiled_.executionOrder.push_back(passes_[n].object->handle_);
        }
        for (auto s : nodeGraph_.successors(n))
        {
            if (--indegree[s] == 0)
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
    // Resolve render-pass attachment metadata only after the final execution
    // order and culling state are known. This makes discard analysis reflect
    // explicit dependencies that reorder passes.
    for (std::size_t i = 0; i < passNodes_.size(); ++i)
    {
        if (i < passes_.size() && !passes_[i].object->culled_ && passNodes_[i])
        {
            passNodes_[i]->resolve();
        }
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
    execute(commands, ExecuteOptions{});
}

void FrameGraph::execute(CommandContext& commands, const ExecuteOptions& options) noexcept
{
    if (!compiled_.success)
    {
        return;
    }
    lastError_ = {};
    for (auto& pass : passes_)
    {
        pass.legacyFailed = false;
        pass.legacyError.clear();
    }
    ResourceCreationContext context{this, provider_, &commands};
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
        PassExecutionContext executionContext{h, p.object->name_,
            static_cast<std::uint32_t>(i), options.userData};
        if (options.onBegin)
        {
            options.onBegin(executionContext);
        }
        if (h.index() < passNodes_.size() && passNodes_[h.index()])
        {
            if (auto* renderPass = dynamic_cast<RenderPassNode*>(passNodes_[h.index()].get()))
            {
                renderPass->devirtualizeRenderTargets(context);
                if (!renderPass->renderTargetsReady())
                {
                    lastError_ = {GraphErrorCode::ExecutionFailed,
                        "render target creation failed", {}};
                    return;
                }
            }
            passNodes_[h.index()]->execute(resources, commands);
            if (auto* renderPass = dynamic_cast<RenderPassNode*>(passNodes_[h.index()].get()))
            {
                renderPass->destroyRenderTargets(context);
            }
        }
        else
        {
            p.object->executeInternal(resources, commands);
        }
        if (p.object->failed() || p.legacyFailed)
        {
            lastError_ = {GraphErrorCode::ExecutionFailed,
                std::string(p.legacyFailed
                                  ? p.legacyError
                                  : (p.object->errorMessage().empty()
                                          ? "frame graph pass execution failed"
                                          : p.object->errorMessage())),
                {}};
            return;
        }
        if (options.onEnd)
        {
            options.onEnd(executionContext);
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
    passNodes_.clear();
    resourceNodes_.clear();
    graph_.reset(0);
    nodeGraph_.reset(0);
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
            if (a.resourceIndex == resource.index() &&
                a.effectiveVersion() == resource.version())
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
