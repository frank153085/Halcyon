#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Halcyon::Renderer::Graph
{

inline constexpr std::uint32_t kInvalidIndex = 0xffffffffu;

enum class ResourceKind : std::uint8_t
{
    Buffer,
    Texture
};
enum class AccessMode : std::uint8_t
{
    Read,
    Write,
    ReadWrite
};
enum class QueueClass : std::uint8_t
{
    Graphics,
    Compute,
    Transfer
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
    D32Float
};

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
    Present = 1u << 10u
};

constexpr ResourceUsage operator|(ResourceUsage a, ResourceUsage b) noexcept
{
    return static_cast<ResourceUsage>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr ResourceUsage operator&(ResourceUsage a, ResourceUsage b) noexcept
{
    return static_cast<ResourceUsage>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr ResourceUsage& operator|=(ResourceUsage& a, ResourceUsage b) noexcept
{
    return a = a | b;
}
constexpr bool any(ResourceUsage u) noexcept
{
    return static_cast<std::uint32_t>(u) != 0;
}

struct BufferDescriptor
{
    std::string name;
    std::uint64_t size = 0;
    std::uint32_t stride = 0;
    bool transient = true;
};

struct TextureDescriptor
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

struct BufferSubresourceDescriptor
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
struct TextureSubresourceDescriptor
{
    std::uint32_t mipLevel = 0;
    std::uint32_t mipCount = 1;
    std::uint32_t baseLayer = 0;
    std::uint32_t layerCount = 1;
};

struct FrameGraphNativeResource
{
    void* token = nullptr;
};

struct FrameGraphTexture
{
    using Descriptor = TextureDescriptor;
    using SubResourceDescriptor = TextureSubresourceDescriptor;
    using Usage = ResourceUsage;
    TextureDescriptor descriptor{};
    FrameGraphNativeResource native{};
};

struct FrameGraphBuffer
{
    using Descriptor = BufferDescriptor;
    using SubResourceDescriptor = BufferSubresourceDescriptor;
    using Usage = ResourceUsage;
    BufferDescriptor descriptor{};
    FrameGraphNativeResource native{};
};

struct FrameGraphResourceCreateInfo
{
    ResourceKind kind = ResourceKind::Buffer;
    BufferDescriptor buffer{};
    TextureDescriptor texture{};
    bool imported = false;
};

struct FrameGraphConfig
{
    std::size_t arenaBytes = 1u << 20u;
    bool cullPasses = true;
};

class FrameGraphResourceProvider
{
public:
    virtual ~FrameGraphResourceProvider() noexcept = default;
    virtual bool create(
        const FrameGraphResourceCreateInfo&, FrameGraphNativeResource&) noexcept = 0;
    virtual void destroy(const FrameGraphNativeResource&) noexcept = 0;
};

struct CommandContext
{
    std::string_view passName() const noexcept
    {
        return name_;
    }
    QueueClass queue() const noexcept
    {
        return queue_;
    }
    std::uint32_t executionIndex() const noexcept
    {
        return executionIndex_;
    }
    void* nativeToken() const noexcept
    {
        return nativeToken_;
    }
    void setNativeToken(void* token) noexcept
    {
        nativeToken_ = token;
    }

private:
    friend class FrameGraph;
    std::string_view name_{};
    QueueClass queue_ = QueueClass::Graphics;
    std::uint32_t executionIndex_ = 0;
    void* nativeToken_ = nullptr;
};

} // namespace Halcyon::Renderer::Graph
