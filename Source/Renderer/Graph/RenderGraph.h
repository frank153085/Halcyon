#pragma once

// A backend-independent render-graph description and compiler.
//
// The graph deliberately contains no API objects (Vulkan, D3D, ...).  It is
// therefore useful in unit tests and can be compiled before a graphics backend
// exists.  A backend can consume CompileResult::executionOrder and translate
// the declared accesses into native barriers at execution time.

#include "../../Core/Handle.h"
#include "FrameGraphNode.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Halcyon::Renderer::Graph
{

inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

// The common Core::Handle gives all renderer resources the same generation
// checked semantics while the tag keeps buffer, texture and pass handles
// strongly typed at compile time.
struct BufferTag
{
};
struct TextureTag
{
};
struct PassTag
{
};
using BufferHandle = Halcyon::Core::Handle<BufferTag>;
using TextureHandle = Halcyon::Core::Handle<TextureTag>;
using PassHandle = Halcyon::Core::Handle<PassTag>;

enum class ResourceKind : std::uint8_t
{
    Buffer,
    Texture,
};

enum class AccessMode : std::uint8_t
{
    Read,
    Write,
    ReadWrite,
};

// These are semantic usages only.  They intentionally do not mirror a
// particular graphics API's enum values.
enum class ResourceUsage : std::uint32_t
{
    None = 0,
    Vertex = 1u << 0u,
    Index = 1u << 1u,
    Uniform = 1u << 2u,
    Storage = 1u << 3u,
    Indirect = 1u << 4u,
    Sampled = 1u << 5u,
    ColorAttachment = 1u << 6u,
    DepthAttachment = 1u << 7u,
    TransferSource = 1u << 8u,
    TransferDestination = 1u << 9u,
    Present = 1u << 10u,
};

[[nodiscard]] constexpr ResourceUsage operator|(ResourceUsage lhs, ResourceUsage rhs) noexcept
{
    return static_cast<ResourceUsage>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
[[nodiscard]] constexpr ResourceUsage operator&(ResourceUsage lhs, ResourceUsage rhs) noexcept
{
    return static_cast<ResourceUsage>(
        static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}
constexpr ResourceUsage& operator|=(ResourceUsage& lhs, ResourceUsage rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}
[[nodiscard]] constexpr bool any(ResourceUsage usage) noexcept
{
    return static_cast<std::uint32_t>(usage) != 0u;
}

enum class QueueClass : std::uint8_t
{
    Graphics,
    Compute,
    Transfer,
};

enum class TextureFormat : std::uint8_t
{
    Unknown,
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA16Float,
    R16Float,
    D32Float,
};

struct BufferDesc
{
    std::string name;
    std::uint64_t size = 0;
    std::uint32_t stride = 0;
    bool transient = true;
};

struct TextureDesc
{
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1;
    TextureFormat format = TextureFormat::Unknown;
    bool transient = true;
};

struct ResourceAccess
{
    ResourceKind kind = ResourceKind::Buffer;
    std::uint32_t resourceIndex = kInvalidIndex;
    std::uint32_t resourceGeneration = 0;
    AccessMode mode = AccessMode::Read;
    ResourceUsage usage = ResourceUsage::None;

    [[nodiscard]] bool writes() const noexcept
    {
        return mode == AccessMode::Write || mode == AccessMode::ReadWrite;
    }
    [[nodiscard]] bool reads() const noexcept
    {
        return mode == AccessMode::Read || mode == AccessMode::ReadWrite;
    }
};

struct ResourceLifetime
{
    // Pass positions are positions in CompileResult::executionOrder.  -1
    // denotes a resource that is not used by a live pass.
    std::int32_t firstUse = -1;
    std::int32_t lastUse = -1;
    PassHandle firstPass{};
    PassHandle lastPass{};

    [[nodiscard]] bool used() const noexcept
    {
        return firstUse >= 0;
    }
};

struct ResourceRecord
{
    ResourceKind kind = ResourceKind::Buffer;
    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;
    std::string name;
    bool imported = false;
    bool exported = false;
    BufferDesc buffer{};
    TextureDesc texture{};
    ResourceLifetime lifetime{};
};

struct PassExecutionContext
{
    PassHandle handle{};
    std::string_view name{};
    // Position in CompileResult::executionOrder.  This is stable for the
    // lifetime of a compiled graph and lets backend adapters assign per-pass
    // timestamp slots without coupling the graph to a graphics API.
    std::uint32_t executionIndex = 0;
    // Optional caller-owned context.  The graph never dereferences this
    // pointer; Vulkan and other backends may use it to record commands.
    void* userData = nullptr;
};

using PassExecuteCallback = std::function<void(const PassExecutionContext&)>;
using PassBoundaryCallback = std::function<void(const PassExecutionContext&)>;

struct ExecuteOptions
{
    void* userData = nullptr;
    PassBoundaryCallback onBegin;
    PassBoundaryCallback onEnd;
};

struct CompiledPass
{
    PassHandle handle{};
    std::string name;
    QueueClass queue = QueueClass::Graphics;
    bool sideEffect = false;
    bool culled = false;
    std::vector<ResourceAccess> accesses;
    PassExecuteCallback execute;
};

enum class GraphErrorCode : std::uint8_t
{
    None,
    InvalidHandle,
    InvalidDeclaration,
    CycleDetected,
    ExecutionFailed,
};

struct GraphError
{
    GraphErrorCode code = GraphErrorCode::None;
    std::string message;
    std::vector<PassHandle> cycle;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != GraphErrorCode::None;
    }
};

struct CompileOptions
{
    bool cullDeadPasses = true;
};

struct CompileResult
{
    bool success = false;
    GraphError error{};
    std::vector<PassHandle> executionOrder;
    std::vector<CompiledPass> passes;
    std::vector<ResourceRecord> resources;

    [[nodiscard]] bool ok() const noexcept
    {
        return success;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }
    [[nodiscard]] const std::vector<PassHandle>& order() const noexcept
    {
        return executionOrder;
    }
    [[nodiscard]] const std::vector<PassHandle>& orderedPasses() const noexcept
    {
        return executionOrder;
    }

    [[nodiscard]] const ResourceLifetime* lifetime(BufferHandle handle) const noexcept;
    [[nodiscard]] const ResourceLifetime* lifetime(TextureHandle handle) const noexcept;
    [[nodiscard]] const CompiledPass* pass(PassHandle handle) const noexcept;
    [[nodiscard]] bool isCulled(PassHandle handle) const noexcept;

    struct ExecutionResult
    {
        bool success = false;
        std::size_t executedPasses = 0;
        GraphError error{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return success;
        }
    };

    // Execute callbacks in the compiled topological order.  This is the
    // backend-neutral execution hook used by tests and by Vulkan command
    // recording adapters; culled passes are never invoked. Optional boundary
    // callbacks are called immediately before and after each pass callback.
    [[nodiscard]] ExecutionResult execute(const ExecuteOptions& options = {}) const;
};

class FrameGraph;
using RenderGraph = FrameGraph;

class PassBuilder
{
public:
    PassBuilder() = default;

    [[nodiscard]] PassHandle handle() const noexcept
    {
        return pass_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return graph_ != nullptr && pass_.valid();
    }
    [[nodiscard]] operator PassHandle() const noexcept
    {
        return pass_;
    }

    PassBuilder& read(BufferHandle handle, ResourceUsage usage = ResourceUsage::None);
    PassBuilder& read(TextureHandle handle, ResourceUsage usage = ResourceUsage::None);
    PassBuilder& write(BufferHandle handle, ResourceUsage usage = ResourceUsage::None);
    PassBuilder& write(TextureHandle handle, ResourceUsage usage = ResourceUsage::None);
    PassBuilder& readWrite(BufferHandle handle, ResourceUsage usage = ResourceUsage::None);
    PassBuilder& readWrite(TextureHandle handle, ResourceUsage usage = ResourceUsage::None);

    PassBuilder& dependsOn(PassHandle handle);
    PassBuilder& setSideEffect(bool enabled = true);
    PassBuilder& output(BufferHandle handle);
    PassBuilder& output(TextureHandle handle);
    PassBuilder& setOutput(BufferHandle handle)
    {
        return output(handle);
    }
    PassBuilder& setOutput(TextureHandle handle)
    {
        return output(handle);
    }
    PassBuilder& setQueue(QueueClass queue);
    PassBuilder& setExecute(PassExecuteCallback callback);

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph* graph, PassHandle pass)
            : graph_(graph),
              pass_(pass)
    {
    }

    FrameGraph* graph_ = nullptr;
    PassHandle pass_{};
};

class FrameGraph
{
public:
    using SetupCallback = std::function<void(PassBuilder&)>;

    FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    FrameGraph(FrameGraph&&) noexcept = default;
    FrameGraph& operator=(FrameGraph&&) noexcept = default;
    ~FrameGraph() = default;

    [[nodiscard]] BufferHandle createBuffer(BufferDesc desc = {});
    [[nodiscard]] TextureHandle createTexture(TextureDesc desc = {});
    [[nodiscard]] BufferHandle importBuffer(BufferDesc desc = {});
    [[nodiscard]] TextureHandle importTexture(TextureDesc desc = {});
    bool destroy(BufferHandle handle);
    bool destroy(TextureHandle handle);

    // The builder overload is convenient for a fluent, imperative setup:
    //   auto p = graph.addPass("Lighting"); p.read(gbuffer).write(hdr);
    [[nodiscard]] PassBuilder addPass(std::string_view name, bool sideEffect = false);

    // The callback overload is useful when a pass is described in one place.
    [[nodiscard]] PassHandle addPass(
        std::string_view name, const SetupCallback& setup, PassExecuteCallback execute = {});

    bool markOutput(BufferHandle handle);
    bool markOutput(TextureHandle handle);
    bool markOutput(PassHandle handle);
    bool exportResource(BufferHandle handle)
    {
        return markOutput(handle);
    }
    bool exportResource(TextureHandle handle)
    {
        return markOutput(handle);
    }
    bool setPassSideEffect(PassHandle handle, bool enabled = true);
    bool setPassExecute(PassHandle handle, PassExecuteCallback callback);
    bool setPassQueue(PassHandle handle, QueueClass queue);
    [[nodiscard]] PassBuilder editPass(PassHandle handle);

    [[nodiscard]] CompileResult compile(const CompileOptions& options = {}) const;
    void clear();

    [[nodiscard]] bool valid(BufferHandle handle) const noexcept;
    [[nodiscard]] bool valid(TextureHandle handle) const noexcept;
    [[nodiscard]] bool valid(PassHandle handle) const noexcept;
    [[nodiscard]] std::size_t bufferCount() const noexcept;
    [[nodiscard]] std::size_t textureCount() const noexcept;
    [[nodiscard]] std::size_t passCount() const noexcept;

private:
    friend class PassBuilder;

    struct ResourceNode
    {
        NodeId nodeId{0, NodeKind::Resource};
        ResourceKind kind = ResourceKind::Buffer;
        std::uint32_t generation = 1;
        bool alive = true;
        bool imported = false;
        bool exported = false;
        BufferDesc buffer{};
        TextureDesc texture{};
    };
    struct PassNode
    {
        NodeId nodeId{0, NodeKind::Pass};
        std::uint32_t generation = 1;
        bool alive = true;
        std::string name;
        QueueClass queue = QueueClass::Graphics;
        bool sideEffect = false;
        std::vector<ResourceAccess> accesses;
        std::vector<PassHandle> explicitDependencies;
        PassExecuteCallback execute;
    };

    [[nodiscard]] bool validResource(
        ResourceKind kind, std::uint32_t index, std::uint32_t generation) const noexcept;
    [[nodiscard]] bool validPass(PassHandle handle) const noexcept;
    bool addAccess(PassHandle pass,
        ResourceKind kind,
        std::uint32_t index,
        std::uint32_t generation,
        AccessMode mode,
        ResourceUsage usage);
    bool addDependency(PassHandle pass, PassHandle dependency);
    bool setResourceOutput(ResourceKind kind, std::uint32_t index, std::uint32_t generation);
    bool setPassSideEffectInternal(PassHandle pass, bool enabled);
    bool setPassQueueInternal(PassHandle pass, QueueClass queue);
    bool setPassExecuteInternal(PassHandle pass, PassExecuteCallback callback);

    [[nodiscard]] PassHandle makePassHandle(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t allocateResourceGeneration() noexcept;
    [[nodiscard]] std::uint32_t allocatePassGeneration() noexcept;

    [[nodiscard]] std::vector<bool> cullPasses(
        const CompileOptions& options, const DependencyGraph& cullingDependencies) const;
    void analyzeResourceLifetimes(CompileResult& result) const;

    std::vector<ResourceNode> resources_;
    std::vector<std::uint32_t> freeBufferSlots_;
    std::vector<std::uint32_t> freeTextureSlots_;
    std::vector<PassNode> passes_;
    // These counters deliberately survive clear().  Otherwise an old handle
    // could alias a newly-created slot after a graph is rebuilt in-place.
    std::uint32_t nextResourceGeneration_ = 1;
    std::uint32_t nextPassGeneration_ = 1;
};

} // namespace Halcyon::Renderer::Graph
