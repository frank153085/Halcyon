#include "FrameGraphTexture.h"

#include "FrameGraphTypes.h"

#include <algorithm>
#include <utility>

namespace Halcyon::Renderer::Graph
{

bool FrameGraphTexture::create(FrameGraphResourceProvider& provider, std::string_view name) noexcept
{
    FrameGraphResourceCreateInfo info{};
    info.kind = ResourceKind::Texture;
    info.texture = descriptor;
    if (!name.empty())
    {
        info.texture.name.assign(name.data(), name.size());
        descriptor.name = info.texture.name;
    }
    info.imported = false;
    return provider.create(info, native);
}

void FrameGraphTexture::destroy(FrameGraphResourceProvider& provider) noexcept
{
    if (native.token != nullptr)
    {
        provider.destroy(native);
        native = {};
    }
}

TextureDescriptor FrameGraphTexture::generateSubResourceDescriptor(
    TextureDescriptor descriptor, TextureSubresourceDescriptor subresource) noexcept
{
    descriptor.mipLevels = 1;
    descriptor.width = std::max(1u, descriptor.width >> subresource.mipLevel);
    descriptor.height = std::max(1u, descriptor.height >> subresource.mipLevel);
    return descriptor;
}

bool FrameGraphBuffer::create(FrameGraphResourceProvider& provider, std::string_view name) noexcept
{
    FrameGraphResourceCreateInfo info{};
    info.kind = ResourceKind::Buffer;
    info.buffer = descriptor;
    if (!name.empty())
    {
        info.buffer.name.assign(name.data(), name.size());
        descriptor.name = info.buffer.name;
    }
    info.imported = false;
    return provider.create(info, native);
}

void FrameGraphBuffer::destroy(FrameGraphResourceProvider& provider) noexcept
{
    if (native.token != nullptr)
    {
        provider.destroy(native);
        native = {};
    }
}

} // namespace Halcyon::Renderer::Graph
