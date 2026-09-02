#include "details/PassNode.h"

#include "details/Resource.h"
#include "details/ResourceCreationContext.h"
#include "FrameGraph.h"

#include <algorithm>
#include <cstdint>
#include <exception>

namespace Halcyon::Renderer::Graph
{

PassNode::PassNode(FrameGraph& graph) noexcept
        : frameGraph_(&graph)
{
}

PassNode::PassNode(FrameGraph* graph) noexcept
        : frameGraph_(graph)
{
}

PassNode::PassNode(PassNode&& other) noexcept
        : devirtualize(std::move(other.devirtualize)),
          destroy(std::move(other.destroy)),
          frameGraph_(other.frameGraph_),
          passBase_(other.passBase_),
          executeCallback_(std::move(other.executeCallback_)),
          handle_(other.handle_),
          id_(other.id_),
          name_(std::move(other.name_)),
          queue_(other.queue_),
          declaredHandles_(std::move(other.declaredHandles_)),
          refCount_(other.refCount_),
          target_(other.target_),
          culled_(other.culled_)
{
    other.frameGraph_ = nullptr;
    other.passBase_ = nullptr;
}

PassNode::~PassNode() noexcept = default;

void PassNode::registerResource(FrameGraphHandle resourceHandle,
    VirtualResource* resource) noexcept
{
    declaredHandles_.insert(resourceHandle.index());
    if (resource != nullptr)
    {
        resource->neededByPass(this);
    }
    ++refCount_;
}

std::string PassNode::graphvizify() const
{
    std::string result{"[label=\""};
    result += name_;
    result += "\\nrefs: ";
    result += std::to_string(refCount_);
    result += "\\\", style=filled, fillcolor=";
    result += culled_ ? "gray" : "darkorange";
    result += "]";
    return result;
}

std::string PassNode::graphvizifyEdgeColor() const
{
    return "darkorange";
}

RenderPassNode::RenderPassNode(FrameGraph& graph, const char* name,
    FrameGraphPassBase* base) noexcept
        : PassNode(graph)
{
    passBase_ = base;
    setName(name ? std::string_view{name} : std::string_view{});
}

RenderPassNode::RenderPassNode(FrameGraph* graph, std::string_view name,
    FrameGraphPassBase* base) noexcept
        : PassNode(graph)
{
    passBase_ = base;
    setName(name);
}

RenderPassNode::RenderPassNode(RenderPassNode&& other) noexcept
        : PassNode(std::move(other)),
          renderPasses_(std::move(other.renderPasses_))
{
}

RenderPassNode::~RenderPassNode() noexcept = default;

std::uint32_t RenderPassNode::declareRenderTarget(std::string_view name,
    const FrameGraphRenderPass::Descriptor& descriptor)
{
    (void)name;
    RenderPassData data{};
    data.name.assign(name.data(), name.size());
    data.descriptor = descriptor;
    data.attachments = descriptor.attachments;
    renderPasses_.push_back(std::move(data));
    return static_cast<std::uint32_t>(renderPasses_.size() - 1u);
}

void RenderPassNode::execute(const FrameGraphResources& resources,
    CommandContext& commands) noexcept
{
    if (passBase_ != nullptr)
    {
        passBase_->executeInternal(resources, commands);
    }
    if (!executeCallback_)
    {
        return;
    }
    try
    {
        executeCallback_(resources, commands);
    }
    catch (...)
    {
        // Private node callbacks are best-effort; FrameGraphPass captures
        // user exceptions at the public execution boundary.
    }
}

void RenderPassNode::resolve() noexcept
{
    auto* graph = frameGraph_;
    if (graph == nullptr)
    {
        return;
    }
    const auto passIndex = handle_.index();
    std::vector<std::int32_t> executionPosition(graph->passes_.size(), -1);
    for (std::size_t position = 0; position < graph->compiled_.executionOrder.size(); ++position)
    {
        const auto h = graph->compiled_.executionOrder[position];
        if (h.index() < executionPosition.size())
        {
            executionPosition[h.index()] = static_cast<std::int32_t>(position);
        }
    }
    const auto thisPosition = passIndex < executionPosition.size() ? executionPosition[passIndex]
                                                                     : -1;
    for (auto& renderPass : renderPasses_)
    {
        renderPass.discardStart = {};
        renderPass.discardEnd = {};
        renderPass.clearFlags = renderPass.descriptor.clearFlags;
        renderPass.readOnly = {};
        std::uint32_t minWidth = UINT32_MAX;
        std::uint32_t minHeight = UINT32_MAX;
        std::uint32_t maxWidth = 0;
        std::uint32_t maxHeight = 0;
        FrameGraphAttachmentFlags keepOverrideStart{};
        FrameGraphAttachmentFlags keepOverrideEnd{};
        FrameGraphAttachmentFlags targetFlags{};

        for (std::size_t slot = 0; slot < FrameGraphRenderPass::ATTACHMENT_COUNT; ++slot)
        {
            const auto attachment = renderPass.attachments[slot];
            if (!attachment || attachment.index() >= graph->resources_.size())
            {
                continue;
            }
            const auto bit = slot < FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS
                                 ? static_cast<FrameGraphAttachmentFlags>(
                                       1u << static_cast<unsigned>(slot))
                                 : (slot == FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS
                                         ? FrameGraphAttachmentFlags::Depth
                                         : FrameGraphAttachmentFlags::Stencil);
            targetFlags |= bit;
            bool priorWriter = false;
            bool laterReader = false;
            bool currentWriter = false;
            for (std::size_t i = 0; i < graph->passes_.size(); ++i)
            {
                const auto& pass = graph->passes_[i];
                if (pass.object == nullptr || pass.object->culled_)
                {
                    continue;
                }
                for (const auto& access : pass.accesses)
                {
                    bool matches = access.resourceIndex == attachment.index() &&
                                   access.effectiveVersion() == attachment.version();
                    std::uint32_t alias = attachment.index();
                    for (std::size_t guard = 0; !matches && guard < graph->resources_.size() &&
                                                   alias < graph->resources_.size() &&
                                                   graph->resources_[alias].aliasOf != kInvalidIndex;
                         ++guard)
                    {
                        alias = graph->resources_[alias].aliasOf;
                        matches = access.resourceIndex == alias;
                    }
                    if (!matches)
                    {
                        continue;
                    }
                    const auto position = i < executionPosition.size() ? executionPosition[i] : -1;
                    if (i == passIndex && access.writes())
                    {
                        currentWriter = true;
                    }
                    if (position >= 0 && thisPosition >= 0 && position < thisPosition &&
                        access.writes())
                    {
                        priorWriter = true;
                    }
                    if (position >= 0 && thisPosition >= 0 && position > thisPosition &&
                        access.reads())
                    {
                        laterReader = true;
                    }
                }
            }
            if (!priorWriter)
            {
                renderPass.discardStart |= bit;
            }
            if (currentWriter && !laterReader)
            {
                renderPass.discardEnd |= bit;
            }
            if ((slot == FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS ||
                    slot == FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS + 1) &&
                !currentWriter)
            {
                renderPass.readOnly |= bit;
            }
            const auto& resource = graph->resources_[attachment.index()];
            const auto& texture = resource.texture;
            minWidth = std::min(minWidth, texture.width);
            minHeight = std::min(minHeight, texture.height);
            maxWidth = std::max(maxWidth, texture.width);
            maxHeight = std::max(maxHeight, texture.height);
            if (graph->resources_[attachment.index()].importedRenderTarget)
            {
                const auto& imported = graph->resources_[attachment.index()].renderTargetImport;
                renderPass.imported = true;
                renderPass.native = graph->resources_[attachment.index()].native;
                renderPass.descriptor.viewport = imported.viewport;
                renderPass.descriptor.clearColor = imported.clearColor;
                renderPass.descriptor.samples = imported.samples;
                renderPass.descriptor.layerCount = imported.layerCount;
                renderPass.descriptor.clearFlags = imported.clearFlags;
                keepOverrideStart |= imported.keepOverrideStart;
                keepOverrideEnd |= imported.keepOverrideEnd;
                targetFlags |= imported.attachments;
            }
        }
        if (renderPass.descriptor.clearUsage != ResourceUsage::None)
        {
            if (any(renderPass.descriptor.clearUsage & ResourceUsage::ColorAttachment))
            {
                renderPass.clearFlags |= static_cast<FrameGraphAttachmentFlags>(
                    static_cast<std::uint16_t>(FrameGraphAttachmentFlags::AllColors));
            }
            if (any(renderPass.descriptor.clearUsage & ResourceUsage::DepthAttachment))
            {
                renderPass.clearFlags |= FrameGraphAttachmentFlags::Depth;
            }
            renderPass.discardStart |= renderPass.clearFlags;
        }
        renderPass.clearFlags = renderPass.clearFlags & targetFlags;
        renderPass.discardStart |= renderPass.clearFlags;
        renderPass.discardStart = renderPass.discardStart & ~keepOverrideStart;
        renderPass.discardEnd = renderPass.discardEnd & ~keepOverrideEnd;
        renderPass.descriptor.viewport.width = renderPass.descriptor.viewport.width == 0
                                                   ? maxWidth
                                                   : renderPass.descriptor.viewport.width;
        renderPass.descriptor.viewport.height = renderPass.descriptor.viewport.height == 0
                                                    ? maxHeight
                                                    : renderPass.descriptor.viewport.height;
        (void)minWidth;
        (void)minHeight;
        renderPass.resolved = true;
    }
}

void RenderPassNode::devirtualizeRenderTargets(
    const ResourceCreationContext& context) noexcept
{
    for (auto& renderPass : renderPasses_)
    {
        renderPass.devirtualize(context);
    }
}

void RenderPassNode::destroyRenderTargets(
    const ResourceCreationContext& context) noexcept
{
    for (auto& renderPass : renderPasses_)
    {
        renderPass.destroy(context);
    }
}

void RenderPassNode::RenderPassData::devirtualize(
    const ResourceCreationContext& context) noexcept
{
    if (imported || native.token != nullptr || context.graph == nullptr)
    {
        return;
    }
    FrameGraphRenderTargetCreateInfo info{};
    info.descriptor = descriptor;
    info.discardStart = discardStart;
    info.discardEnd = discardEnd;
    info.clearFlags = clearFlags;
    for (std::size_t i = 0; i < FrameGraphRenderPass::ATTACHMENT_COUNT; ++i)
    {
        const auto h = attachments[i];
        if (h && h.index() < context.graph->resources_.size())
        {
            auto index = h.index();
            if (context.graph->resources_[index].native.token == nullptr)
            {
                const auto alias = context.graph->resources_[index].aliasOf;
                if (alias != kInvalidIndex && alias < context.graph->resources_.size())
                {
                    index = alias;
                }
            }
            info.attachments[i] = context.graph->resources_[index].native;
        }
    }
    (void)context.createRenderTarget(info, native);
}

void RenderPassNode::RenderPassData::destroy(
    const ResourceCreationContext& context) noexcept
{
    if (!imported && native.token != nullptr)
    {
        context.destroyRenderTarget(native);
        native = {};
    }
}

std::string RenderPassNode::graphvizify() const
{
    auto result = PassNode::graphvizify();
    if (!result.empty() && result.back() == ']')
    {
        result.insert(result.size() - 1u, ", renderTargets=" + std::to_string(renderPasses_.size()));
    }
    return result;
}

PresentPassNode::PresentPassNode(FrameGraph& graph) noexcept
        : PassNode(graph)
{
    setName("Present");
    setSideEffect();
}

PresentPassNode::PresentPassNode(FrameGraph* graph) noexcept
        : PassNode(graph)
{
    setName("Present");
    setSideEffect();
}

PresentPassNode::PresentPassNode(PresentPassNode&& other) noexcept
        : PassNode(std::move(other)),
          resource(other.resource)
{
}

PresentPassNode::~PresentPassNode() noexcept = default;

void PresentPassNode::execute(const FrameGraphResources&, CommandContext&) noexcept
{
}

void PresentPassNode::resolve() noexcept
{
}

std::string PresentPassNode::graphvizify() const
{
    return "[label=\"Present\", style=filled, fillcolor=red3]";
}

} // namespace Halcyon::Renderer::Graph
