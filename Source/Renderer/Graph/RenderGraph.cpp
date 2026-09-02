#include "RenderGraph.h"

#include <algorithm>
#include <utility>

namespace Halcyon::Renderer::Graph
{

BufferHandle FrameGraph::createBuffer(BufferDesc desc)
{
    std::uint32_t index = kInvalidIndex;
    if (!freeBufferSlots_.empty())
    {
        index = freeBufferSlots_.back();
        freeBufferSlots_.pop_back();
        auto& node = resources_[index];
        node.nodeId = NodeId{index, NodeKind::Resource};
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
        node.nodeId = NodeId{index, NodeKind::Resource};
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

TextureHandle FrameGraph::createTexture(TextureDesc desc)
{
    std::uint32_t index = kInvalidIndex;
    if (!freeTextureSlots_.empty())
    {
        index = freeTextureSlots_.back();
        freeTextureSlots_.pop_back();
        auto& node = resources_[index];
        node.nodeId = NodeId{index, NodeKind::Resource};
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
        node.nodeId = NodeId{index, NodeKind::Resource};
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

BufferHandle FrameGraph::importBuffer(BufferDesc desc)
{
    desc.transient = false;
    const auto handle = createBuffer(std::move(desc));
    resources_[handle.index()].imported = true;
    return handle;
}

TextureHandle FrameGraph::importTexture(TextureDesc desc)
{
    desc.transient = false;
    const auto handle = createTexture(std::move(desc));
    resources_[handle.index()].imported = true;
    return handle;
}

bool FrameGraph::destroy(BufferHandle handle)
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

bool FrameGraph::destroy(TextureHandle handle)
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

PassBuilder FrameGraph::addPass(std::string_view name, bool sideEffect)
{
    PassNode node;
    node.nodeId = NodeId{static_cast<std::uint32_t>(passes_.size()), NodeKind::Pass};
    node.name = std::string(name);
    node.sideEffect = sideEffect;
    node.generation = allocatePassGeneration();
    node.alive = true;
    passes_.push_back(std::move(node));
    const auto handle = makePassHandle(static_cast<std::uint32_t>(passes_.size() - 1));
    return PassBuilder(this, handle);
}

PassHandle FrameGraph::addPass(
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

bool FrameGraph::markOutput(BufferHandle handle)
{
    return setResourceOutput(ResourceKind::Buffer, handle.index(), handle.generation());
}

bool FrameGraph::markOutput(TextureHandle handle)
{
    return setResourceOutput(ResourceKind::Texture, handle.index(), handle.generation());
}

bool FrameGraph::markOutput(PassHandle handle)
{
    return setPassSideEffectInternal(handle, true);
}

bool FrameGraph::setPassSideEffect(PassHandle handle, bool enabled)
{
    return setPassSideEffectInternal(handle, enabled);
}

bool FrameGraph::setPassExecute(PassHandle handle, PassExecuteCallback callback)
{
    return setPassExecuteInternal(handle, std::move(callback));
}

bool FrameGraph::setPassQueue(PassHandle handle, QueueClass queue)
{
    return setPassQueueInternal(handle, queue);
}

PassBuilder FrameGraph::editPass(PassHandle handle)
{
    if (!valid(handle))
    {
        return {};
    }
    return PassBuilder(this, handle);
}

bool FrameGraph::validResource(
    ResourceKind kind, std::uint32_t index, std::uint32_t generation) const noexcept
{
    if (index == kInvalidIndex || index >= resources_.size() || generation == 0)
    {
        return false;
    }
    const auto& node = resources_[index];
    return node.alive && node.kind == kind && node.generation == generation;
}

bool FrameGraph::valid(BufferHandle handle) const noexcept
{
    return validResource(ResourceKind::Buffer, handle.index(), handle.generation());
}

bool FrameGraph::valid(TextureHandle handle) const noexcept
{
    return validResource(ResourceKind::Texture, handle.index(), handle.generation());
}

bool FrameGraph::validPass(PassHandle handle) const noexcept
{
    return handle.valid() && handle.index() < passes_.size() && passes_[handle.index()].alive &&
           passes_[handle.index()].generation == handle.generation();
}

bool FrameGraph::valid(PassHandle handle) const noexcept
{
    return validPass(handle);
}

std::size_t FrameGraph::bufferCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const ResourceNode& node)
        {
            return node.alive && node.kind == ResourceKind::Buffer;
        }));
}

std::size_t FrameGraph::textureCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(resources_.begin(),
        resources_.end(),
        [](const ResourceNode& node)
        {
            return node.alive && node.kind == ResourceKind::Texture;
        }));
}

std::size_t FrameGraph::passCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(passes_.begin(),
        passes_.end(),
        [](const PassNode& node)
        {
            return node.alive;
        }));
}

bool FrameGraph::addAccess(PassHandle pass,
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

bool FrameGraph::addDependency(PassHandle pass, PassHandle dependency)
{
    if (!validPass(pass))
    {
        return false;
    }
    // Invalid dependencies are retained for compile-time diagnostics.
    passes_[pass.index()].explicitDependencies.push_back(dependency);
    return true;
}

bool FrameGraph::setResourceOutput(ResourceKind kind, std::uint32_t index, std::uint32_t generation)
{
    if (!validResource(kind, index, generation))
    {
        return false;
    }
    resources_[index].exported = true;
    return true;
}

bool FrameGraph::setPassSideEffectInternal(PassHandle pass, bool enabled)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].sideEffect = enabled;
    return true;
}

bool FrameGraph::setPassQueueInternal(PassHandle pass, QueueClass queue)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].queue = queue;
    return true;
}

bool FrameGraph::setPassExecuteInternal(PassHandle pass, PassExecuteCallback callback)
{
    if (!validPass(pass))
    {
        return false;
    }
    passes_[pass.index()].execute = std::move(callback);
    return true;
}

PassHandle FrameGraph::makePassHandle(std::uint32_t index) const noexcept
{
    if (index >= passes_.size())
    {
        return {};
    }
    return PassHandle{index, passes_[index].generation};
}

std::uint32_t FrameGraph::allocateResourceGeneration() noexcept
{
    const auto generation = nextResourceGeneration_;
    ++nextResourceGeneration_;
    if (nextResourceGeneration_ == 0)
    {
        nextResourceGeneration_ = 1;
    }
    return generation;
}

std::uint32_t FrameGraph::allocatePassGeneration() noexcept
{
    const auto generation = nextPassGeneration_;
    ++nextPassGeneration_;
    if (nextPassGeneration_ == 0)
    {
        nextPassGeneration_ = 1;
    }
    return generation;
}

void FrameGraph::clear()
{
    resources_.clear();
    freeBufferSlots_.clear();
    freeTextureSlots_.clear();
    passes_.clear();
}

} // namespace Halcyon::Renderer::Graph
