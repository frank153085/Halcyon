#pragma once

// A backend-neutral counterpart of Filament's FrameGraphRenderPass descriptor.
// Halcyon does not expose backend TargetBufferFlags or Viewport types in its
// public graph API, so the descriptor stores only portable attachment and
// viewport data.  It is intentionally a value-only declaration; creation of a
// native render target remains the responsibility of a resource provider.

#include "FrameGraphId.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace Halcyon::Renderer::Graph
{

// Attachment-role mask corresponding to Filament's TargetBufferFlags. Color
// attachments are represented independently so MRT discard/clear analysis is
// precise.
enum class FrameGraphAttachmentFlags : std::uint16_t
{
    None = 0,
    Color0 = 1u << 0u,
    Color1 = 1u << 1u,
    Color2 = 1u << 2u,
    Color3 = 1u << 3u,
    Color4 = 1u << 4u,
    Color5 = 1u << 5u,
    Color6 = 1u << 6u,
    Color7 = 1u << 7u,
    Depth = 1u << 8u,
    Stencil = 1u << 9u,
    AllColors = 0x00ffu,
    All = 0x03ffu
};

constexpr FrameGraphAttachmentFlags operator|(
    FrameGraphAttachmentFlags a, FrameGraphAttachmentFlags b) noexcept
{
    return static_cast<FrameGraphAttachmentFlags>(
        static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
constexpr FrameGraphAttachmentFlags operator&(
    FrameGraphAttachmentFlags a, FrameGraphAttachmentFlags b) noexcept
{
    return static_cast<FrameGraphAttachmentFlags>(
        static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
constexpr FrameGraphAttachmentFlags& operator|=(
    FrameGraphAttachmentFlags& a, FrameGraphAttachmentFlags b) noexcept
{
    return a = a | b;
}
constexpr FrameGraphAttachmentFlags operator~(FrameGraphAttachmentFlags flags) noexcept
{
    return static_cast<FrameGraphAttachmentFlags>(
        static_cast<std::uint16_t>(~static_cast<std::uint16_t>(flags)));
}
constexpr bool any(FrameGraphAttachmentFlags flags) noexcept
{
    return static_cast<std::uint16_t>(flags) != 0;
}

constexpr ResourceUsage resourceUsageForAttachments(FrameGraphAttachmentFlags flags) noexcept
{
    ResourceUsage usage = ResourceUsage::None;
    if (any(flags & FrameGraphAttachmentFlags::AllColors))
    {
        usage |= ResourceUsage::ColorAttachment;
    }
    if (any(flags & FrameGraphAttachmentFlags::Depth))
    {
        usage |= ResourceUsage::DepthAttachment;
    }
    return usage;
}

using AttachmentFlags = FrameGraphAttachmentFlags;

struct FrameGraphRenderPass
{
    using TextureId = FrameGraphId<FrameGraphTexture>;

    // Filament derives this from backend::MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT.
    // Keep the same useful shape without taking a backend dependency.
    static constexpr std::size_t MAX_COLOR_ATTACHMENTS = 8;
    static constexpr std::size_t ATTACHMENT_COUNT = MAX_COLOR_ATTACHMENTS + 2;

    struct Attachments
    {
        std::array<TextureId, MAX_COLOR_ATTACHMENTS> color{};
        TextureId depth{};
        TextureId stencil{};

        TextureId& operator[](std::size_t index) noexcept
        {
            return const_cast<TextureId&>(
                static_cast<const Attachments*>(this)->operator[](index));
        }

        const TextureId& operator[](std::size_t index) const noexcept
        {
            assert(index < ATTACHMENT_COUNT);
            if (index < MAX_COLOR_ATTACHMENTS)
            {
                return color[index];
            }
            if (index == MAX_COLOR_ATTACHMENTS)
            {
                return depth;
            }
            return stencil;
        }
    };

    struct Viewport
    {
        std::int32_t left = 0;
        std::int32_t top = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct ClearColor
    {
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 0.0F;
    };

    struct Descriptor
    {
        Attachments attachments{};
        Viewport viewport{};
        ClearColor clearColor{};
        std::uint32_t samples = 1;
        std::uint32_t layerCount = 1;
        FrameGraphAttachmentFlags clearFlags{};
        // Kept as a convenience for callers that only know resource usage
        // roles. RenderPassNode maps these roles to concrete attachment bits.
        ResourceUsage clearUsage = ResourceUsage::None;
    };

    struct ImportDescriptor
    {
        // The usage mask describes which attachment roles are supplied by the
        // imported target.  It replaces backend-specific TargetBufferFlags.
        FrameGraphAttachmentFlags attachments = FrameGraphAttachmentFlags::Color0;
        Viewport viewport{};
        ClearColor clearColor{};
        std::uint32_t samples = 1;
        std::uint32_t layerCount = 1;
        FrameGraphAttachmentFlags clearFlags{};
        FrameGraphAttachmentFlags keepOverrideStart{};
        FrameGraphAttachmentFlags keepOverrideEnd{};
    };

    std::uint32_t id = 0;
};

struct FrameGraphRenderTargetCreateInfo
{
    FrameGraphRenderPass::Descriptor descriptor{};
    FrameGraphAttachmentFlags discardStart = FrameGraphAttachmentFlags::None;
    FrameGraphAttachmentFlags discardEnd = FrameGraphAttachmentFlags::None;
    FrameGraphAttachmentFlags clearFlags = FrameGraphAttachmentFlags::None;
    std::array<FrameGraphNativeResource, FrameGraphRenderPass::ATTACHMENT_COUNT> attachments{};
};

} // namespace Halcyon::Renderer::Graph
