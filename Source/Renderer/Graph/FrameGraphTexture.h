#pragma once

#include "FrameGraphTypes.h"

#include <string_view>
#include <utility>

namespace Halcyon::Renderer::Graph
{

/**
 * Backend-neutral texture resource contract.
 *
 * This is deliberately shaped like Filament's FrameGraphTexture: descriptors,
 * sub-resource descriptors and usage live with the resource rather than in
 * the FrameGraph container.  A provider owns the native allocation represented
 * by `native`.
 */
struct FrameGraphTexture
{
    using Descriptor = TextureDescriptor;
    using SubResourceDescriptor = TextureSubresourceDescriptor;
    using Usage = ResourceUsage;

    static constexpr Usage DEFAULT_R_USAGE = Usage::Sampled;
    static constexpr Usage DEFAULT_W_USAGE = Usage::ColorAttachment;

    TextureDescriptor descriptor{};
    FrameGraphNativeResource native{};

    bool create(FrameGraphResourceProvider& provider, std::string_view name = {}) noexcept;
    void destroy(FrameGraphResourceProvider& provider) noexcept;

    static TextureDescriptor generateSubResourceDescriptor(
        TextureDescriptor descriptor, TextureSubresourceDescriptor subresource) noexcept;
    // Alternate spelling used by a few downstream integrations.
    static TextureDescriptor generateSubresourceDescriptor(
        TextureDescriptor descriptor, TextureSubresourceDescriptor subresource) noexcept
    {
        return generateSubResourceDescriptor(std::move(descriptor), subresource);
    }
};

/** Buffer counterpart kept beside the texture contract for symmetry. */
struct FrameGraphBuffer
{
    using Descriptor = BufferDescriptor;
    using SubResourceDescriptor = BufferSubresourceDescriptor;
    using Usage = ResourceUsage;

    static constexpr Usage DEFAULT_R_USAGE = Usage::None;
    static constexpr Usage DEFAULT_W_USAGE = Usage::Storage;

    BufferDescriptor descriptor{};
    FrameGraphNativeResource native{};

    bool create(FrameGraphResourceProvider& provider, std::string_view name = {}) noexcept;
    void destroy(FrameGraphResourceProvider& provider) noexcept;
};

// Small traits facade used by backend-neutral code that wants the same
// vocabulary as Filament without depending on the concrete resource type.
struct FrameGraphTextureTraits final
{
    using Resource = FrameGraphTexture;
    using Descriptor = Resource::Descriptor;
    using SubResourceDescriptor = Resource::SubResourceDescriptor;
    using Usage = Resource::Usage;

    static constexpr ResourceKind kind = ResourceKind::Texture;
    static constexpr Usage DEFAULT_R_USAGE = Usage::Sampled;
    static constexpr Usage DEFAULT_W_USAGE = Usage::ColorAttachment;
};

inline FrameGraphResourceCreateInfo makeFrameGraphTextureCreateInfo(
    std::string_view name, FrameGraphTexture::Descriptor descriptor) noexcept
{
    if (!name.empty())
    {
        descriptor.name.assign(name.data(), name.size());
    }

    FrameGraphResourceCreateInfo info{};
    info.kind = ResourceKind::Texture;
    info.texture = std::move(descriptor);
    info.imported = false;
    return info;
}

} // namespace Halcyon::Renderer::Graph
