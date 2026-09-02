#pragma once

#include "Blackboard.h"
#include "Core/Result.h"
#include "details/DependencyGraph.h"
#include "details/Utilities.h"
#include "FrameGraphRenderPass.h"
#include "FrameGraphPass.h"
#include "FrameGraphResources.h"
#include "FrameGraphTexture.h"
#include "FrameGraphTypes.h"
#include "details/Resource.h"
#include "details/PassNode.h"
#include "details/ResourceNode.h"

#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Halcyon::Renderer::Graph
{

using BufferDesc = BufferDescriptor;
using TextureDesc = TextureDescriptor;
using BufferHandle = FrameGraphId<FrameGraphBuffer>;
using TextureHandle = FrameGraphId<FrameGraphTexture>;

struct ResourceAccess
{
    ResourceKind kind = ResourceKind::Buffer;
    std::uint32_t resourceIndex = kInvalidIndex;
    std::uint32_t resourceGeneration = 0;
    AccessMode mode = AccessMode::Read;
    ResourceUsage usage = ResourceUsage::None;
    // Keep the historical five-field aggregate initialization usable
    // (`kind, index, generation, mode, usage`) while retaining the explicit
    // version used by the graph compiler.  A zero value means "same as
    // resourceGeneration" and is filled in by FrameGraph itself for normal
    // declarations.
    std::uint32_t resourceVersion = 0;
    std::uint32_t effectiveVersion() const noexcept
    {
        return resourceVersion != 0 ? resourceVersion : resourceGeneration;
    }
    bool writes() const noexcept
    {
        return mode == AccessMode::Write || mode == AccessMode::ReadWrite;
    }
    bool reads() const noexcept
    {
        return mode == AccessMode::Read || mode == AccessMode::ReadWrite;
    }
};

struct ResourceLifetime
{
    std::int32_t firstUse = -1, lastUse = -1;
    FrameGraphHandle firstPass{}, lastPass{};
    bool used() const noexcept
    {
        return firstUse >= 0;
    }
};

struct ResourceRecord
{
    ResourceKind kind = ResourceKind::Buffer;
    std::uint32_t index = kInvalidIndex, generation = 0, version = 0;
    std::string name;
    bool imported = false, exported = false, detached = false;
    BufferDesc buffer{};
    TextureDesc texture{};
    ResourceLifetime lifetime{};
};

struct PassExecutionContext
{
    FrameGraphHandle handle{};
    std::string_view name{};
    std::uint32_t executionIndex = 0;
    void* userData = nullptr;
};
using PassExecuteCallback = std::function<void(const PassExecutionContext&)>;

enum class GraphErrorCode : std::uint8_t
{
    None,
    InvalidHandle,
    InvalidDeclaration,
    CycleDetected,
    ExecutionFailed
};
struct GraphError
{
    GraphErrorCode code = GraphErrorCode::None;
    std::string message;
    std::vector<FrameGraphHandle> cycle;
    explicit operator bool() const noexcept
    {
        return code != GraphErrorCode::None;
    }
};
struct ExecuteOptions;
struct CompileOptions
{
    bool cullDeadPasses = true;
};
struct CompileResult
{
    bool success = false;
    GraphError error{};
    std::vector<FrameGraphHandle> executionOrder;
    struct CompiledPass
    {
        FrameGraphHandle handle{};
        std::string name;
        QueueClass queue = QueueClass::Graphics;
        bool sideEffect = false, culled = false;
        std::vector<ResourceAccess> accesses;
        PassExecuteCallback execute;
    };
    std::vector<CompiledPass> passes;
    std::vector<ResourceRecord> resources;
    bool ok() const noexcept
    {
        return success;
    }
    explicit operator bool() const noexcept
    {
        return success;
    }
    const CompiledPass* pass(FrameGraphHandle) const noexcept;
    bool isCulled(FrameGraphHandle h) const noexcept
    {
        auto* p = pass(h);
        return p && p->culled;
    }
    const ResourceLifetime* lifetime(BufferHandle) const noexcept;
    const ResourceLifetime* lifetime(TextureHandle) const noexcept;
    struct ExecutionResult
    {
        bool success = false;
        std::size_t executedPasses = 0;
        GraphError error{};
        explicit operator bool() const noexcept
        {
            return success;
        }
    };
    ExecutionResult execute(const ExecuteOptions& options) const;
    ExecutionResult execute() const;
};
struct ExecuteOptions
{
    void* userData = nullptr;
    std::function<void(const struct PassExecutionContext&)> onBegin;
    std::function<void(const struct PassExecutionContext&)> onEnd;
};
using PassHandle = FrameGraphHandle;

using NodeKind = DependencyGraph::NodeKind;
using NodeId = DependencyGraph::NodeId;

class FrameGraph
{
public:
    class Builder
    {
    public:
        template <typename Resource>
        FrameGraphId<Resource> create(
            std::string_view name, const typename Resource::Descriptor& descriptor = {})
        {
            const auto h = graph_->createRaw(ResourceKindOf<Resource>::value, name, descriptor);
            return FrameGraphId<Resource>(h.index(), h.version(), graph_->epoch_);
        }
        template <typename Resource>
        FrameGraphId<Resource> createSubresource(FrameGraphId<Resource> parent,
            std::string_view name,
            const typename Resource::SubResourceDescriptor& descriptor = {})
        {
            const auto h = graph_->createSubresourceRaw(parent, name, descriptor);
            return FrameGraphId<Resource>(h.index(), h.version(), graph_->epoch_);
        }
        template <typename Resource>
        FrameGraphId<Resource> read(
            FrameGraphId<Resource> input, typename Resource::Usage usage = {})
        {
            const auto h = graph_->accessVersion(input, AccessMode::Read, usage);
            graph_->addAccessRaw(pass_, h, AccessMode::Read, usage);
            return FrameGraphId<Resource>(h.index(), h.version(), graph_->epoch_);
        }
        template <typename Resource>
        FrameGraphId<Resource> write(
            FrameGraphId<Resource> input, typename Resource::Usage usage = {})
        {
            const auto h = graph_->accessVersion(input, AccessMode::Write, usage);
            graph_->addAccessRaw(pass_, h, AccessMode::Write, usage);
            return FrameGraphId<Resource>(h.index(), h.version(), graph_->epoch_);
        }
        void sideEffect() noexcept
        {
            if (graph_)
            {
                graph_->setPassSideEffectInternal(pass_);
            }
        }
        std::string_view getName(FrameGraphHandle h) const noexcept;
        template <typename Resource>
        const typename Resource::Descriptor& getDescriptor(FrameGraphId<Resource> id) const
        {
            return graph_->getDescriptor(id);
        }
        template <typename Resource>
        const typename Resource::SubResourceDescriptor& getSubResourceDescriptor(
            FrameGraphId<Resource> id) const
        {
            return graph_->getSubResourceDescriptor(id);
        }
        std::uint32_t declareRenderPass(
            std::string_view name, const FrameGraphRenderPass::Descriptor& descriptor);
        TextureHandle declareRenderPass(TextureHandle color, std::uint32_t* index = nullptr);
        void reserveRenderTargets(std::size_t count) noexcept;
        template <typename Resource>
        FrameGraphId<Resource> sample(FrameGraphId<Resource> id)
            requires std::is_same_v<Resource, FrameGraphTexture>
        {
            return read(id, ResourceUsage::Sampled);
        }
        PassHandle handle() const noexcept
        {
            return pass_;
        }
        operator PassHandle() const noexcept
        {
            return pass_;
        }
        bool isInitialized() const noexcept
        {
            return lastAccess_.isInitialized();
        }
        std::uint32_t index() const noexcept
        {
            return lastAccess_.index();
        }
        std::uint32_t version() const noexcept
        {
            return lastAccess_.version();
        }
        std::uint32_t generation() const noexcept
        {
            return lastAccess_.version();
        }
        FrameGraphHandle resourceHandle() const noexcept
        {
            return lastAccess_;
        }
        template <typename Resource>
        operator FrameGraphId<Resource>() const noexcept
        {
            return FrameGraphId<Resource>(
                lastAccess_.index(), lastAccess_.version(), lastAccess_.epoch());
        }
        Builder& read(BufferHandle h, ResourceUsage u = ResourceUsage::None)
        {
            readRaw(h, u, AccessMode::Read);
            return *this;
        }
        Builder& read(TextureHandle h, ResourceUsage u = ResourceUsage::None)
        {
            readRaw(h, u, AccessMode::Read);
            return *this;
        }
        Builder& write(BufferHandle h, ResourceUsage u = ResourceUsage::None)
        {
            writeRaw(h, u, AccessMode::Write);
            return *this;
        }
        Builder& write(TextureHandle h, ResourceUsage u = ResourceUsage::None)
        {
            writeRaw(h, u, AccessMode::Write);
            return *this;
        }
        Builder& readWrite(BufferHandle h, ResourceUsage u = ResourceUsage::None)
        {
            writeRaw(h, u, AccessMode::ReadWrite);
            return *this;
        }
        Builder& readWrite(TextureHandle h, ResourceUsage u = ResourceUsage::None)
        {
            writeRaw(h, u, AccessMode::ReadWrite);
            return *this;
        }
        Builder& dependsOn(PassHandle h)
        {
            if (graph_)
            {
                graph_->dependencyRaw(pass_, h);
            }
            return *this;
        }
        Builder& setSideEffect(bool e = true)
        {
            if (graph_ && e)
            {
                graph_->setPassSideEffectInternal(pass_);
            }
            return *this;
        }
        Builder& output(BufferHandle h)
        {
            writeRaw(h, ResourceUsage::None, AccessMode::Read);
            if (graph_ && h.index() < graph_->resources_.size())
            {
                graph_->resources_[h.index()].exported = true;
            }
            sideEffect();
            return *this;
        }
        Builder& output(TextureHandle h)
        {
            writeRaw(h, ResourceUsage::None, AccessMode::Read);
            if (graph_ && h.index() < graph_->resources_.size())
            {
                graph_->resources_[h.index()].exported = true;
            }
            sideEffect();
            return *this;
        }
        Builder& setQueue(QueueClass q)
        {
            if (graph_)
            {
                graph_->setQueueRaw(pass_, q);
            }
            return *this;
        }
        Builder& setExecute(PassExecuteCallback cb)
        {
            if (graph_)
            {
                graph_->setExecuteRaw(pass_, std::move(cb));
            }
            return *this;
        }

    private:
        friend class FrameGraph;
        Builder(FrameGraph* g, PassHandle p)
                : graph_(g),
                  pass_(p)
        {
        }
        void readRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m);
        void writeRaw(FrameGraphHandle h, ResourceUsage u, AccessMode m);
        template <typename>
        struct ResourceKindOf;
        FrameGraph* graph_ = nullptr;
        PassHandle pass_{};
        FrameGraphHandle lastAccess_{};
    };
    struct Empty
    {
    };
    explicit FrameGraph(const FrameGraphConfig& config = {})
            : config_(config)
    {
    }
    FrameGraph(const FrameGraph& other)
            : executionOrder(other.executionOrder),
              error(other.error),
              config_(other.config_),
              compiled_(other.compiled_),
              lastError_(other.lastError_)
    {
    }
    FrameGraph& operator=(const FrameGraph& other)
    {
        if (this != &other)
        {
            compiled_ = other.compiled_;
            lastError_ = other.lastError_;
            executionOrder = other.executionOrder;
            error = other.error;
        }
        return *this;
    }
    FrameGraph(FrameGraph&&) noexcept = default;
    FrameGraph& operator=(FrameGraph&&) noexcept = default;
    ~FrameGraph() = default;

    template <typename Data, typename Setup, typename Execute>
    FrameGraphPass<Data, std::decay_t<Execute>>& addPass(
        std::string_view name, Setup&& setup, Execute&& execute)
    {
        using Pass = FrameGraphPass<Data, std::decay_t<Execute>>;
        auto pass = std::make_unique<Pass>(Data{}, std::forward<Execute>(execute));
        auto* result = pass.get();
        const auto handle = createPassRaw(name, std::move(pass));
        result->handle_ = handle;
        result->name_ = std::string(name);
        Builder builder(this, handle);
        if constexpr (std::is_invocable_v<Setup, Builder&, Data&>)
        {
            setup(builder, result->data_);
        }
        else if constexpr (std::is_invocable_v<Setup, Builder&>)
        {
            setup(builder);
        }
        return *result;
    }
    Builder addPass(std::string_view name, bool sideEffect = false);
    FrameGraphId<FrameGraphBuffer> createBuffer(BufferDesc d = {});
    FrameGraphId<FrameGraphTexture> createTexture(TextureDesc d = {});
    FrameGraphId<FrameGraphBuffer> importBuffer(BufferDesc d = {});
    FrameGraphId<FrameGraphTexture> importTexture(TextureDesc d = {});
    bool destroy(BufferHandle) noexcept;
    bool destroy(TextureHandle) noexcept;
    bool valid(BufferHandle h) const noexcept
    {
        return isValid(h);
    }
    bool valid(TextureHandle h) const noexcept
    {
        return isValid(h);
    }
    bool valid(PassHandle h) const noexcept
    {
        return h.epoch() == epoch_ && h.index() < passes_.size() &&
               passes_[h.index()].object->handle().version() == h.version();
    }
    FrameGraph& compile() noexcept;
    FrameGraph& compile(const CompileOptions& options) noexcept
    {
        config_.cullPasses = options.cullDeadPasses;
        return compile();
    }
    void execute(CommandContext&) noexcept;
    CompileResult::ExecutionResult execute(const ExecuteOptions& options = {}) const;
    operator CompileResult() const
    {
        return compiled_;
    }
    void reset() noexcept;
    void clear() noexcept
    {
        reset();
    }
    template <typename Resource>
    FrameGraphId<Resource> import(std::string_view name,
        const typename Resource::Descriptor& descriptor,
        typename Resource::Usage usage,
        const Resource& resource) noexcept
    {
        const auto h = createRaw(ResourceKindOf<Resource>::value, name, descriptor);
        auto id = FrameGraphId<Resource>(h.index(), h.version(), epoch_);
        importTokenRaw(id, resource.native);
        usageRaw(id, usage);
        return id;
    }
    template <typename Resource>
    void present(FrameGraphId<Resource> id) noexcept
    {
        presentRaw(id);
    }
    void present(const Builder& builder) noexcept
    {
        presentRaw(builder.lastAccess_);
    }
    template <typename Resource>
    FrameGraphId<Resource> forwardResource(FrameGraphId<Resource> a, FrameGraphId<Resource> b)
    {
        forwardRaw(a, b);
        return a;
    }
    template <typename Resource>
    FrameGraphId<Resource> forwardResource(
        std::string_view name,
        const typename Resource::Descriptor& descriptor,
        FrameGraphId<Resource> replaced)
    {
        const auto created = createRaw(ResourceKindOf<Resource>::value, name, descriptor);
        const auto result = FrameGraphId<Resource>(created.index(), created.version(), epoch_);
        forwardRaw(result, replaced);
        return result;
    }
    template <typename Resource>
    FrameGraphId<Resource> forwardResource(
        std::string_view name,
        const typename Resource::Descriptor& descriptor,
        const typename Resource::SubResourceDescriptor& subresource,
        FrameGraphId<Resource> replaced)
    {
        const auto created = createRaw(ResourceKindOf<Resource>::value, name, descriptor);
        const auto sub = createSubresourceRaw(created, name, subresource);
        const auto result = FrameGraphId<Resource>(sub.index(), sub.version(), epoch_);
        forwardRaw(result, replaced);
        return result;
    }
    FrameGraphId<FrameGraphTexture> import(
        std::string_view name,
        const FrameGraphRenderPass::ImportDescriptor& descriptor,
        FrameGraphNativeResource target) noexcept;
    bool isValid(FrameGraphHandle) const noexcept;
    bool isCulled(const FrameGraphPassBase&) const noexcept;
    bool isAcyclic() const noexcept;
    void exportGraphviz(std::ostream&, std::string_view = {}) const noexcept;
    void export_graphviz(std::ostream& out, const char* name = nullptr) const noexcept
    {
        exportGraphviz(out, name ? std::string_view{name} : std::string_view{});
    }
    template <typename Resource>
    const typename Resource::Descriptor& getDescriptor(FrameGraphId<Resource> id) const
    {
        const auto* raw = resourceRaw(id, ResourceKindOf<Resource>::value);
        if (raw == nullptr)
        {
            throw std::out_of_range("invalid frame graph resource handle");
        }
        return static_cast<const Resource*>(raw)->descriptor;
    }
    template <typename Resource>
    const typename Resource::SubResourceDescriptor& getSubResourceDescriptor(
        FrameGraphId<Resource> id) const
    {
        const auto* raw = resourceRaw(id, ResourceKindOf<Resource>::value);
        if (raw == nullptr)
        {
            throw std::out_of_range("invalid frame graph resource handle");
        }
        return static_cast<const Resource*>(raw)->subResourceDescriptor;
    }
    const CompileResult& compileResult() const noexcept
    {
        return compiled_;
    }
    const GraphError& lastError() const noexcept
    {
        return lastError_;
    }
    Blackboard& getBlackboard() noexcept
    {
        return blackboard_;
    }
    const Blackboard& getBlackboard() const noexcept
    {
        return blackboard_;
    }
    void setResourceProvider(FrameGraphResourceProvider* p) noexcept
    {
        provider_ = p;
    }
    std::size_t bufferCount() const noexcept;
    std::size_t textureCount() const noexcept;
    std::size_t passCount() const noexcept;
    std::vector<FrameGraphHandle> executionOrder;
    GraphError error{};
    explicit operator bool() const noexcept
    {
        return compiled_.success;
    }

private:
    template <typename>
    struct ResourceKindOf;
    friend class Builder;
    friend class FrameGraphResources;
    friend class FrameGraphPassBase;
    friend class PassNode;
    friend class RenderPassNode;
    friend class PresentPassNode;
    FrameGraphHandle createRaw(ResourceKind, std::string_view, const BufferDescriptor&);
    FrameGraphHandle createRaw(ResourceKind, std::string_view, const TextureDescriptor&);
    FrameGraphHandle createSubresourceRaw(
        FrameGraphHandle, std::string_view, const BufferSubresourceDescriptor&);
    FrameGraphHandle createSubresourceRaw(
        FrameGraphHandle, std::string_view, const TextureSubresourceDescriptor&);
    FrameGraphHandle accessVersion(FrameGraphHandle, AccessMode, ResourceUsage);
    PassHandle createPassRaw(std::string_view, std::unique_ptr<FrameGraphPassBase>);
    void addAccessRaw(PassHandle, FrameGraphHandle, AccessMode, ResourceUsage);
    void setProducerRaw(FrameGraphHandle, PassHandle);
    void dependencyRaw(PassHandle, PassHandle);
    void setPassSideEffectInternal(PassHandle, bool = true);
    void setQueueRaw(PassHandle, QueueClass);
    void setExecuteRaw(PassHandle, PassExecuteCallback);
    void importTokenRaw(FrameGraphHandle, FrameGraphNativeResource);
    void usageRaw(FrameGraphHandle, ResourceUsage);
    void presentRaw(FrameGraphHandle) noexcept;
    void forwardRaw(FrameGraphHandle, FrameGraphHandle);
    const void* resourceRaw(FrameGraphHandle, ResourceKind) const noexcept;
    ResourceUsage resourceUsageRaw(FrameGraphHandle) const noexcept;
    bool declaredRaw(FrameGraphHandle pass, FrameGraphHandle resource) const noexcept;
    struct Resource
    {
        ResourceKind kind;
        std::uint32_t index, version;
        std::uint32_t aliasOf = kInvalidIndex;
        std::uint32_t parent = kInvalidIndex;
        bool alive = true, current = true, imported = false, exported = false, detached = false;
        bool materialized = false;
        std::string name;
        BufferDescriptor buffer{};
        TextureDescriptor texture{};
        BufferSubresourceDescriptor bufferSubresource{};
        TextureSubresourceDescriptor textureSubresource{};
        FrameGraphBuffer bufferObject{};
        FrameGraphTexture textureObject{};
        FrameGraphNativeResource native{};
        bool importedRenderTarget = false;
        FrameGraphRenderPass::ImportDescriptor renderTargetImport{};
        ResourceUsage usage = ResourceUsage::None;
        int producer = -1;
        // Number of pass declarations against this version. Used to preserve
        // Filament's first-write-in-place behavior before creating a new
        // version for subsequent writes.
        std::uint32_t accessCount = 0;
        ResourceLifetime lifetime{};
    };
    struct Pass
    {
        std::unique_ptr<FrameGraphPassBase> object;
        std::vector<ResourceAccess> accesses;
        std::vector<PassHandle> deps;
        PassExecuteCallback legacyExecute;
        bool legacyFailed = false;
        std::string legacyError;
        bool alive = true;
    };
    FrameGraphConfig config_{};
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    // Filament-shaped nodes are the execution-facing representation. The
    // handle-indexed records above remain the compact public/result snapshot.
    std::vector<std::unique_ptr<PassNode>> passNodes_;
    std::vector<std::unique_ptr<ResourceNode>> resourceNodes_;
    CompileResult compiled_{};
    GraphError lastError_{};
    Blackboard blackboard_{};
    FrameGraphResourceProvider* provider_ = nullptr;
    std::uint32_t epoch_ = 1;
    std::uint32_t nextVersion_ = 1;
    DependencyGraph graph_{};
    DependencyGraph nodeGraph_{};
};

template <>
struct FrameGraph::ResourceKindOf<FrameGraphTexture>
{
    static constexpr ResourceKind value = ResourceKind::Texture;
};
template <>
struct FrameGraph::ResourceKindOf<FrameGraphBuffer>
{
    static constexpr ResourceKind value = ResourceKind::Buffer;
};
template <>
struct FrameGraph::Builder::ResourceKindOf<FrameGraphTexture>
{
    static constexpr ResourceKind value = ResourceKind::Texture;
};
template <>
struct FrameGraph::Builder::ResourceKindOf<FrameGraphBuffer>
{
    static constexpr ResourceKind value = ResourceKind::Buffer;
};

} // namespace Halcyon::Renderer::Graph
