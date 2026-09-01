#pragma once

// Translation of RenderGraph's semantic usages into synchronization intent.
// This layer deliberately stops before Vulkan enums: a backend can map these
// compact masks to VkPipelineStageFlags2/VkAccessFlags2 and image layouts,
// while unit tests can validate hazard decisions without a GPU.

#include "RenderGraph.hpp"

#include <cstdint>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Halcyon::Renderer::Graph
{

enum class PipelineStage : std::uint64_t
{
    None = 0,
    VertexInput = 1ull << 0u,
    VertexShader = 1ull << 1u,
    FragmentShader = 1ull << 2u,
    ComputeShader = 1ull << 3u,
    ColorOutput = 1ull << 4u,
    DepthTest = 1ull << 5u,
    Transfer = 1ull << 6u,
    DrawIndirect = 1ull << 7u,
    Host = 1ull << 8u,
    AllCommands = 1ull << 63u,
};

enum class AccessFlags : std::uint64_t
{
    None = 0,
    VertexRead = 1ull << 0u,
    IndexRead = 1ull << 1u,
    UniformRead = 1ull << 2u,
    ShaderSampledRead = 1ull << 3u,
    ShaderStorageRead = 1ull << 4u,
    ShaderStorageWrite = 1ull << 5u,
    IndirectRead = 1ull << 6u,
    ColorWrite = 1ull << 7u,
    DepthRead = 1ull << 8u,
    DepthWrite = 1ull << 9u,
    TransferRead = 1ull << 10u,
    TransferWrite = 1ull << 11u,
    HostWrite = 1ull << 12u,
    ColorRead = 1ull << 13u,
    PresentRead = 1ull << 14u,
};

template <typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr Enum operator|(Enum lhs, Enum rhs) noexcept
{
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));
}

template <typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr Enum operator&(Enum lhs, Enum rhs) noexcept
{
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));
}

template <typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr bool any(Enum value) noexcept
{
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Underlying>(value) != 0;
}

enum class ImageLayout : std::uint8_t
{
    Undefined,
    General,
    ShaderReadOnly,
    ColorAttachment,
    DepthAttachment,
    TransferSource,
    TransferDestination,
    Present,
};

struct UsageInfo
{
    PipelineStage stage = PipelineStage::None;
    AccessFlags access = AccessFlags::None;
    ImageLayout layout = ImageLayout::General;
    bool writes = false;
};

[[nodiscard]] UsageInfo describeUsage(
    ResourceKind kind, ResourceUsage usage, AccessMode mode) noexcept;

struct BarrierState
{
    PipelineStage stage = PipelineStage::None;
    AccessFlags access = AccessFlags::None;
    ImageLayout layout = ImageLayout::Undefined;
    QueueClass queue = QueueClass::Graphics;
    bool writes = false;
};

struct ResourceBarrier
{
    ResourceAccess access{};
    BarrierState before{};
    BarrierState after{};
    bool required = false;
    bool queueOwnershipTransfer = false;
};

// Stateful planner used while executing a compiled graph.  Calling begin()
// resets all tracked states; plan() returns one barrier per unique resource
// in the pass, including read-after-read transitions only when a layout or
// queue changes.  Repeated declarations are merged into ReadWrite access.
//
// The planner is single-owner state: callers must externally synchronise
// begin(), plan() and state().  A pointer returned by state() remains valid
// only until the next non-const planner operation.
class BarrierPlanner
{
public:
    void begin() noexcept;
    [[nodiscard]] std::vector<ResourceBarrier> plan(
        std::span<const ResourceAccess> accesses, QueueClass queue);
    [[nodiscard]] const BarrierState* state(
        ResourceKind kind, std::uint32_t index, std::uint32_t generation) const noexcept;
    [[nodiscard]] std::size_t trackedResourceCount() const noexcept
    {
        return states_.size();
    }

private:
    struct Key
    {
        ResourceKind kind = ResourceKind::Buffer;
        std::uint32_t index = kInvalidIndex;
        std::uint32_t generation = 0;
        friend bool operator==(const Key&, const Key&) noexcept = default;
    };
    struct KeyHash
    {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    std::unordered_map<Key, BarrierState, KeyHash> states_;
};

} // namespace Halcyon::Renderer::Graph
