#include "details/ResourceCreationContext.h"

namespace Halcyon::Renderer::Graph
{

bool ResourceCreationContext::canCreate() const noexcept
{
    return provider != nullptr;
}

bool ResourceCreationContext::create(
    const FrameGraphResourceCreateInfo& info,
    FrameGraphNativeResource& output) const noexcept
{
    if (provider == nullptr)
    {
        output = {};
        return false;
    }
    if (!provider->create(info, output))
    {
        output = {};
        return false;
    }
    return true;
}

bool ResourceCreationContext::createRenderTarget(
    const FrameGraphRenderTargetCreateInfo& info,
    FrameGraphNativeResource& output) const noexcept
{
    if (provider == nullptr)
    {
        output = {};
        return false;
    }
    if (!provider->createRenderTarget(info, output))
    {
        output = {};
        return false;
    }
    return true;
}

void ResourceCreationContext::destroy(const FrameGraphNativeResource& resource) const noexcept
{
    if (provider != nullptr && resource.token != nullptr)
    {
        provider->destroy(resource);
    }
}

void ResourceCreationContext::destroyRenderTarget(
    const FrameGraphNativeResource& resource) const noexcept
{
    if (provider != nullptr && resource.token != nullptr)
    {
        provider->destroyRenderTarget(resource);
    }
}

} // namespace Halcyon::Renderer::Graph
