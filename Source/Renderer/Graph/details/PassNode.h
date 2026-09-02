#pragma once

// Filament-style pass node declarations.  The implementation is intentionally
// backend-neutral: command submission is represented by CommandContext and a
// small optional callback, while render-target materialization is left to the
// resource provider.

#include "DependencyGraph.h"
#include "../FrameGraphRenderPass.h"
#include "../FrameGraphPass.h"
#include "../FrameGraphResources.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Graph
{

class FrameGraph;
class VirtualResource;
class ResourceNode;
struct ResourceCreationContext;

class PassNode
{
public:
    using NodeID = std::uint32_t;
    using ExecuteCallback = std::function<void(
        const FrameGraphResources&, CommandContext&)>;

    explicit PassNode(FrameGraph& graph) noexcept;
    explicit PassNode(FrameGraph* graph = nullptr) noexcept;
    PassNode(PassNode&&) noexcept;
    PassNode(const PassNode&) = delete;
    PassNode& operator=(const PassNode&) = delete;
    PassNode& operator=(PassNode&&) = delete;
    virtual ~PassNode() noexcept;

    void setFrameGraph(FrameGraph* graph) noexcept
    {
        frameGraph_ = graph;
    }
    FrameGraph* frameGraph() const noexcept
    {
        return frameGraph_;
    }

    void setHandle(FrameGraphHandle handle) noexcept
    {
        handle_ = handle;
    }
    FrameGraphHandle handle() const noexcept
    {
        return handle_;
    }
    NodeID getId() const noexcept
    {
        return id_;
    }
    void setId(NodeID id) noexcept
    {
        id_ = id;
    }

    void setName(std::string_view name)
    {
        name_.assign(name.data(), name.size());
    }
    const char* getName() const noexcept
    {
        return name_.c_str();
    }
    std::string_view name() const noexcept
    {
        return name_;
    }

    void setQueue(QueueClass queue) noexcept
    {
        queue_ = queue;
    }
    QueueClass queue() const noexcept
    {
        return queue_;
    }
    void makeTarget() noexcept
    {
        target_ = true;
    }
    bool isTarget() const noexcept
    {
        return target_;
    }
    void setSideEffect(bool value = true) noexcept
    {
        target_ = value || target_;
    }
    bool sideEffect() const noexcept
    {
        return target_;
    }
    void setCulled(bool value) noexcept
    {
        culled_ = value;
    }
    bool isCulled() const noexcept
    {
        return culled_;
    }
    std::uint32_t getRefCount() const noexcept
    {
        return refCount_;
    }
    void resetCompileState() noexcept
    {
        declaredHandles_.clear();
        refCount_ = 0;
        culled_ = false;
        devirtualize.clear();
        destroy.clear();
    }

    void registerResource(FrameGraphHandle resourceHandle,
        VirtualResource* resource = nullptr) noexcept;
    bool declares(FrameGraphHandle resourceHandle) const noexcept
    {
        return declaredHandles_.find(resourceHandle.index()) != declaredHandles_.end();
    }

    void setExecuteCallback(ExecuteCallback callback)
    {
        executeCallback_ = std::move(callback);
    }

    virtual void execute(const FrameGraphResources& resources,
        CommandContext& commands) noexcept = 0;
    virtual void resolve() noexcept = 0;
    virtual std::size_t getSize() const noexcept = 0;

    virtual std::string graphvizify() const;
    virtual std::string graphvizifyEdgeColor() const;

    // Resources created/destroyed at this pass boundary.  These vectors are
    // public to mirror Filament's node contract and simplify compiler passes.
    std::vector<VirtualResource*> devirtualize;
    std::vector<VirtualResource*> destroy;

protected:
    FrameGraph* frameGraph_ = nullptr;
    FrameGraphPassBase* passBase_ = nullptr;
    ExecuteCallback executeCallback_;
    FrameGraphHandle handle_{};
    NodeID id_ = 0;
    std::string name_;
    QueueClass queue_ = QueueClass::Graphics;
    std::unordered_set<FrameGraphHandle::Index> declaredHandles_;
    std::uint32_t refCount_ = 0;
    bool target_ = false;
    bool culled_ = false;
};

class RenderPassNode final : public PassNode
{
public:
    struct RenderPassData
    {
        std::string name;
        FrameGraphRenderPass::Descriptor descriptor{};
        FrameGraphRenderPass::Attachments attachments{};
        FrameGraphNativeResource native{};
        bool imported = false;
        FrameGraphAttachmentFlags discardStart{};
        FrameGraphAttachmentFlags discardEnd{};
        FrameGraphAttachmentFlags clearFlags{};
        FrameGraphAttachmentFlags readOnly{};
        bool resolved = false;

        void devirtualize(const ResourceCreationContext&) noexcept;
        void destroy(const ResourceCreationContext&) noexcept;
    };

    RenderPassNode(FrameGraph& graph, const char* name,
        FrameGraphPassBase* base = nullptr) noexcept;
    RenderPassNode(FrameGraph* graph = nullptr, std::string_view name = {},
        FrameGraphPassBase* base = nullptr) noexcept;
    RenderPassNode(RenderPassNode&&) noexcept;
    RenderPassNode(const RenderPassNode&) = delete;
    RenderPassNode& operator=(const RenderPassNode&) = delete;
    RenderPassNode& operator=(RenderPassNode&&) = delete;
    ~RenderPassNode() noexcept override;

    std::uint32_t declareRenderTarget(std::string_view name,
        const FrameGraphRenderPass::Descriptor& descriptor);

    // Generic Builder overload matching Filament's signature without naming
    // the incomplete nested FrameGraph::Builder type in this private header.
    template <typename Builder>
    std::uint32_t declareRenderTarget(FrameGraph&, Builder&,
        std::string_view name,
        const FrameGraphRenderPass::Descriptor& descriptor)
    {
        return declareRenderTarget(name, descriptor);
    }

    const RenderPassData* getRenderPassData(std::uint32_t id) const noexcept
    {
        return id < renderPasses_.size() ? &renderPasses_[id] : nullptr;
    }
    std::size_t getRenderTargetCount() const noexcept
    {
        return renderPasses_.size();
    }
    bool renderTargetsReady() const noexcept
    {
        for (const auto& renderPass : renderPasses_)
        {
            if (!renderPass.imported && renderPass.native.token == nullptr)
            {
                return false;
            }
        }
        return true;
    }
    void reserveRenderTargets(std::size_t count)
    {
        renderPasses_.reserve(count);
    }

    void execute(const FrameGraphResources& resources,
        CommandContext& commands) noexcept override;
    void resolve() noexcept override;
    void devirtualizeRenderTargets(const ResourceCreationContext& context) noexcept;
    void destroyRenderTargets(const ResourceCreationContext& context) noexcept;
    std::size_t getSize() const noexcept override
    {
        return sizeof(RenderPassNode);
    }
    std::string graphvizify() const override;

private:
    std::vector<RenderPassData> renderPasses_;
};

class PresentPassNode final : public PassNode
{
public:
    explicit PresentPassNode(FrameGraph& graph) noexcept;
    explicit PresentPassNode(FrameGraph* graph = nullptr) noexcept;
    PresentPassNode(PresentPassNode&&) noexcept;
    PresentPassNode(const PresentPassNode&) = delete;
    PresentPassNode& operator=(const PresentPassNode&) = delete;
    PresentPassNode& operator=(PresentPassNode&&) = delete;
    ~PresentPassNode() noexcept override;

    FrameGraphHandle resource{};

    void execute(const FrameGraphResources&, CommandContext&) noexcept override;
    void resolve() noexcept override;
    std::size_t getSize() const noexcept override
    {
        return sizeof(PresentPassNode);
    }
    std::string graphvizify() const override;
};

} // namespace Halcyon::Renderer::Graph
