#pragma once

// Backend-neutral resource identities shared by the scene, graph and Vulkan
// layers.  A handle is never a Vulkan object: this keeps FramePacket and
// capture files portable while the backend is free to rebuild resources.

#include "../../Core/Handle.h"

#include <cstdint>
#include <string>

namespace Halcyon::Renderer::Resources
{

struct MeshTag
{
};
struct MaterialTag
{
};
struct TextureTag
{
};
struct BufferTag
{
};
struct PipelineTag
{
};
struct SamplerTag
{
};

using MeshHandle = Core::Handle<MeshTag>;
using MaterialHandle = Core::Handle<MaterialTag>;
using TextureHandle = Core::Handle<TextureTag>;
using BufferHandle = Core::Handle<BufferTag>;
using PipelineHandle = Core::Handle<PipelineTag>;
using SamplerHandle = Core::Handle<SamplerTag>;

inline constexpr std::uint32_t kDefaultResourceSlot = 0u;

enum class MemoryClass : std::uint8_t
{
    DeviceLocal,
    Upload,
    Readback,
};

enum class TextureDimension : std::uint8_t
{
    D1,
    D2,
    D3,
    Cube,
};

enum class PixelFormat : std::uint16_t
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
    std::string debugName;
    std::uint64_t byteSize = 0;
    std::uint32_t stride = 0;
    MemoryClass memory = MemoryClass::DeviceLocal;
    std::uint32_t usageMask = 0;
    bool persistent = false;
};

struct TextureDesc
{
    std::string debugName;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1;
    TextureDimension dimension = TextureDimension::D2;
    PixelFormat format = PixelFormat::Unknown;
    std::uint32_t usageMask = 0;
    bool persistent = false;
};

struct MeshDesc
{
    std::string debugName;
    BufferHandle vertexBuffer{};
    BufferHandle indexBuffer{};
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t lodCount = 1;
};

struct MaterialDesc
{
    std::string debugName;
    TextureHandle baseColor{};
    TextureHandle normal{};
    TextureHandle metallicRoughness{};
    TextureHandle emissive{};
    float metallic = 0.0f;
    float roughness = 1.0f;
};

// A stable, serialisable reference used by capture/replay and diagnostics.
struct ResourceRef
{
    std::uint64_t packedHandle = 0;
    std::uint32_t type = 0;

    template <typename HandleT>
    [[nodiscard]] static constexpr ResourceRef from(
        HandleT handle, std::uint32_t resourceType) noexcept
    {
        return ResourceRef{handle.packed(), resourceType};
    }
};

} // namespace Halcyon::Renderer::Resources
