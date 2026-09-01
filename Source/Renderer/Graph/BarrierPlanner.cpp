#include "BarrierPlanner.h"

#include <algorithm>

namespace Halcyon::Renderer::Graph
{

namespace
{

[[nodiscard]] constexpr bool isBufferVertex(ResourceUsage usage) noexcept
{
    return any(usage & ResourceUsage::Vertex);
}

[[nodiscard]] constexpr bool reads(AccessMode mode) noexcept
{
    return mode == AccessMode::Read || mode == AccessMode::ReadWrite;
}

[[nodiscard]] constexpr bool writes(AccessMode mode) noexcept
{
    return mode == AccessMode::Write || mode == AccessMode::ReadWrite;
}

[[nodiscard]] constexpr AccessMode mergeAccessMode(AccessMode lhs, AccessMode rhs) noexcept
{
    const bool mergedReads = reads(lhs) || reads(rhs);
    const bool mergedWrites = writes(lhs) || writes(rhs);
    return mergedReads && mergedWrites ? AccessMode::ReadWrite
                                       : (mergedWrites ? AccessMode::Write : AccessMode::Read);
}

[[nodiscard]] constexpr bool hasReadAccess(AccessFlags access) noexcept
{
    constexpr auto readMask = AccessFlags::VertexRead | AccessFlags::IndexRead |
                              AccessFlags::UniformRead | AccessFlags::ShaderSampledRead |
                              AccessFlags::ShaderStorageRead | AccessFlags::IndirectRead |
                              AccessFlags::ColorRead | AccessFlags::DepthRead |
                              AccessFlags::TransferRead | AccessFlags::PresentRead;
    return any(access & readMask);
}

[[nodiscard]] constexpr bool hasWriteAccess(AccessFlags access) noexcept
{
    constexpr auto writeMask = AccessFlags::ShaderStorageWrite | AccessFlags::ColorWrite |
                               AccessFlags::DepthWrite | AccessFlags::TransferWrite |
                               AccessFlags::HostWrite;
    return any(access & writeMask);
}

} // namespace

UsageInfo describeUsage(ResourceKind kind, ResourceUsage usage, AccessMode mode) noexcept
{
    UsageInfo info{};
    const bool accessReads = reads(mode);
    info.writes = writes(mode);

    // No explicit usage is treated as a generic shader access.  This makes a
    // declaration useful even when a prototype has not selected a final
    // Vulkan layout yet.
    if (!any(usage))
    {
        info.stage = PipelineStage::ComputeShader;
        if (accessReads)
        {
            info.access = info.access | AccessFlags::ShaderStorageRead;
        }
        if (info.writes)
        {
            info.access = info.access | AccessFlags::ShaderStorageWrite;
        }
        info.layout = kind == ResourceKind::Texture
                          ? (info.writes ? ImageLayout::General : ImageLayout::ShaderReadOnly)
                          : ImageLayout::General;
        return info;
    }

    if (any(usage & ResourceUsage::Vertex))
    {
        info.stage = info.stage | PipelineStage::VertexInput;
        info.access = info.access | AccessFlags::VertexRead;
    }
    if (any(usage & ResourceUsage::Index))
    {
        info.stage = info.stage | PipelineStage::VertexInput;
        info.access = info.access | AccessFlags::IndexRead;
    }
    if (any(usage & ResourceUsage::Uniform))
    {
        info.stage = info.stage | PipelineStage::VertexShader | PipelineStage::FragmentShader |
                     PipelineStage::ComputeShader;
        info.access = info.access | AccessFlags::UniformRead;
    }
    if (any(usage & ResourceUsage::Sampled))
    {
        // ResourceUsage does not encode a shader stage, so use every stage
        // supported by the current abstraction rather than under-synchronise
        // vertex texture fetches.
        info.stage = info.stage | PipelineStage::VertexShader | PipelineStage::FragmentShader |
                     PipelineStage::ComputeShader;
        info.access = info.access | AccessFlags::ShaderSampledRead;
        info.layout = ImageLayout::ShaderReadOnly;
    }
    if (any(usage & ResourceUsage::Storage))
    {
        info.stage = info.stage | PipelineStage::VertexShader | PipelineStage::ComputeShader |
                     PipelineStage::FragmentShader;
        if (accessReads)
        {
            info.access = info.access | AccessFlags::ShaderStorageRead;
        }
        if (info.writes)
        {
            info.access = info.access | AccessFlags::ShaderStorageWrite;
        }
        info.layout = ImageLayout::General;
    }
    if (any(usage & ResourceUsage::Indirect))
    {
        info.stage = info.stage | PipelineStage::DrawIndirect;
        info.access = info.access | AccessFlags::IndirectRead;
    }
    if (any(usage & ResourceUsage::ColorAttachment))
    {
        info.stage = info.stage | PipelineStage::ColorOutput;
        if (accessReads)
        {
            info.access = info.access | AccessFlags::ColorRead;
        }
        if (info.writes)
        {
            info.access = info.access | AccessFlags::ColorWrite;
        }
        info.layout = ImageLayout::ColorAttachment;
    }
    if (any(usage & ResourceUsage::DepthAttachment))
    {
        info.stage = info.stage | PipelineStage::DepthTest;
        if (accessReads)
        {
            info.access = info.access | AccessFlags::DepthRead;
        }
        if (info.writes)
        {
            info.access = info.access | AccessFlags::DepthWrite;
        }
        info.layout = ImageLayout::DepthAttachment;
    }
    if (any(usage & ResourceUsage::TransferSource))
    {
        info.stage = info.stage | PipelineStage::Transfer;
        info.access = info.access | AccessFlags::TransferRead;
        info.layout = ImageLayout::TransferSource;
    }
    if (any(usage & ResourceUsage::TransferDestination))
    {
        info.stage = info.stage | PipelineStage::Transfer;
        info.access = info.access | AccessFlags::TransferWrite;
        info.layout = ImageLayout::TransferDestination;
    }
    if (any(usage & ResourceUsage::Present))
    {
        info.stage = info.stage | PipelineStage::ColorOutput;
        info.access = info.access | AccessFlags::PresentRead;
        info.layout = ImageLayout::Present;
    }

    if (!any(info.stage))
    {
        info.stage = kind == ResourceKind::Buffer && isBufferVertex(usage)
                         ? PipelineStage::VertexInput
                         : PipelineStage::AllCommands;
    }
    // Contradictory prototype declarations (for example write + sampled) are
    // represented conservatively instead of silently omitting one half of the
    // requested access.  A later validation layer can provide a richer error.
    if (accessReads && !hasReadAccess(info.access))
    {
        info.access = info.access | AccessFlags::ShaderStorageRead;
    }
    if (info.writes && !hasWriteAccess(info.access))
    {
        info.access = info.access | AccessFlags::ShaderStorageWrite;
    }
    if (kind == ResourceKind::Buffer)
    {
        // Buffer barriers have no image layout.  Keeping a single semantic
        // value avoids false read-after-read transitions for texel buffers.
        info.layout = ImageLayout::General;
    }
    return info;
}

void BarrierPlanner::begin() noexcept
{
    states_.clear();
}

std::vector<ResourceBarrier> BarrierPlanner::plan(
    std::span<const ResourceAccess> accesses, QueueClass queue)
{
    // Multiple declarations for the same resource describe the whole pass,
    // not sequential operations inside the pass.  Merge them before looking
    // at prior state so every returned barrier can safely execute up front.
    std::vector<ResourceAccess> merged;
    merged.reserve(accesses.size());
    for (const auto& access : accesses)
    {
        const auto existing = std::find_if(merged.begin(),
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
            existing->mode = mergeAccessMode(existing->mode, access.mode);
            existing->usage |= access.usage;
        }
    }

    std::vector<ResourceBarrier> barriers;
    barriers.reserve(merged.size());
    std::vector<Key> missingKeys;
    missingKeys.reserve(merged.size());
    for (const auto& access : merged)
    {
        const Key key{access.kind, access.resourceIndex, access.resourceGeneration};
        const UsageInfo usage = describeUsage(access.kind, access.usage, access.mode);
        const BarrierState after{usage.stage, usage.access, usage.layout, queue, usage.writes};
        auto found = states_.find(key);
        BarrierState before{};
        if (found != states_.end())
        {
            before = found->second;
        }
        const bool queueTransfer = found != states_.end() && before.queue != queue;
        const bool alreadyTracked = found != states_.end();
        const bool hazard = alreadyTracked && (before.writes || after.writes ||
                                                  before.layout != after.layout || queueTransfer);
        // An image needs its initial Undefined -> desired layout transition.
        // A buffer has no layout and no prior access to make visible, so its
        // first use does not need a memory barrier.
        const bool initialImageTransition = !alreadyTracked &&
                                            access.kind == ResourceKind::Texture &&
                                            after.layout != ImageLayout::Undefined;
        barriers.push_back(ResourceBarrier{
            access, before, after, hazard || initialImageTransition, queueTransfer});
        if (!alreadyTracked)
        {
            missingKeys.push_back(key);
        }
    }

    // Commit only after all result allocations succeed.  If a node allocation
    // fails, remove nodes inserted by this call before rethrowing; existing
    // tracked states remain untouched (strong semantic exception guarantee).
    states_.reserve(states_.size() + missingKeys.size());
    std::vector<Key> insertedKeys;
    insertedKeys.reserve(missingKeys.size());
    try
    {
        for (const auto& key : missingKeys)
        {
            const auto [unused, inserted] = states_.try_emplace(key, BarrierState{});
            (void)unused;
            if (inserted)
            {
                insertedKeys.push_back(key);
            }
        }
    }
    catch (...)
    {
        for (const auto& key : insertedKeys)
        {
            states_.erase(key);
        }
        throw;
    }
    for (const auto& barrier : barriers)
    {
        const Key key{
            barrier.access.kind, barrier.access.resourceIndex, barrier.access.resourceGeneration};
        states_.find(key)->second = barrier.after;
    }
    return barriers;
}

const BarrierState* BarrierPlanner::state(
    ResourceKind kind, std::uint32_t index, std::uint32_t generation) const noexcept
{
    const auto found = states_.find(Key{kind, index, generation});
    return found == states_.end() ? nullptr : &found->second;
}

std::size_t BarrierPlanner::KeyHash::operator()(const Key& key) const noexcept
{
    std::size_t value = static_cast<std::size_t>(key.index);
    value ^= static_cast<std::size_t>(key.generation) + static_cast<std::size_t>(0x9e3779b9u) +
             (value << 6u) + (value >> 2u);
    value ^= static_cast<std::size_t>(key.kind) + static_cast<std::size_t>(0x85ebca6bu) +
             (value << 6u) + (value >> 2u);
    return value;
}

} // namespace Halcyon::Renderer::Graph
