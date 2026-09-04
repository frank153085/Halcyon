#include "VulkanFrameGraphProvider.h"

#include "VulkanCommon.h"

#include <algorithm>
#include <sstream>

namespace Halcyon::Vulkan
{
namespace
{
[[nodiscard]] std::string persistentKey(const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo& info)
{
    std::ostringstream key;
    // Usage contributes to the physical allocation contract for ordinary
    // persistent resources.  TAA history images are a deliberate exception:
    // the ping-pong direction flips every frame, so one logical image is
    // sampled on one frame and used as a storage target on the next.  The
    // image is always materialized with both capabilities in createImage();
    // keeping usage out of the cache key is what makes the physical image
    // stable across that flip (and preserves its contents/layout).
    key << static_cast<unsigned>(info.kind) << ':';
    if (info.kind == Halcyon::Renderer::Graph::ResourceKind::Texture)
    {
        const auto& d = info.texture;
        const bool pingPongHistory = d.name.rfind("TAAHistory", 0) == 0;
        if (!pingPongHistory)
            key << static_cast<std::uint32_t>(info.usage) << ':';
        key << d.name << ':' << d.width << 'x' << d.height << 'x' << d.depth << ':'
            << d.mipLevels << ':' << d.arrayLayers << ':' << static_cast<unsigned>(d.format);
        key << ':' << (d.cube ? 'C' : '2');
    }
    else
    {
        key << static_cast<std::uint32_t>(info.usage) << ':'
            << info.buffer.name << ':' << info.buffer.size << ':' << info.buffer.stride;
    }
    return key.str();
}
}

Halcyon::Result<void> VulkanFrameGraphProvider::initialize(
    VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator)
{
    shutdown();
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || !allocator.initialized())
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "invalid Vulkan FrameGraph provider handles"});
    }
    device_ = device;
    physicalDevice_ = physicalDevice;
    allocator_ = &allocator;
    return Halcyon::Result<void>::success();
}

VkFormat VulkanFrameGraphProvider::toVkFormat(Halcyon::Renderer::Graph::TextureFormat format) noexcept
{
    using F = Halcyon::Renderer::Graph::TextureFormat;
    switch (format)
    {
        case F::R8Unorm: return VK_FORMAT_R8_UNORM;
        case F::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
        case F::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case F::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case F::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case F::R16Float: return VK_FORMAT_R16_SFLOAT;
        case F::D32Float: return VK_FORMAT_D32_SFLOAT;
        case F::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
        case F::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case F::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageUsageFlags VulkanFrameGraphProvider::imageUsage(Halcyon::Renderer::Graph::ResourceUsage usage) noexcept
{
    using U = Halcyon::Renderer::Graph::ResourceUsage;
    VkImageUsageFlags flags = 0;
    if (Halcyon::Renderer::Graph::any(usage & U::Sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::ColorAttachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::DepthAttachment)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::TransferSource)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::TransferDestination)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkBufferUsageFlags VulkanFrameGraphProvider::bufferUsage(Halcyon::Renderer::Graph::ResourceUsage usage) noexcept
{
    using U = Halcyon::Renderer::Graph::ResourceUsage;
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (Halcyon::Renderer::Graph::any(usage & U::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return flags;
}

bool VulkanFrameGraphProvider::createImage(
    const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo& info, VulkanFrameGraphResource& out) noexcept
{
    const auto& d = info.texture;
    const VkFormat format = toVkFormat(d.format);
    if (format == VK_FORMAT_UNDEFINED || d.width == 0 || d.height == 0 || d.depth == 0 ||
        d.mipLevels == 0 || d.arrayLayers == 0 || allocator_ == nullptr ||
        (d.cube && (d.width != d.height || d.arrayLayers != 6u)))
    {
        return false;
    }
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &formatProperties);
    const VkFormatFeatureFlags features = formatProperties.optimalTilingFeatures;
    const auto usage = info.usage;
    using U = Halcyon::Renderer::Graph::ResourceUsage;
    if (Halcyon::Renderer::Graph::any(usage & U::ColorAttachment) &&
        (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
        return false;
    if (Halcyon::Renderer::Graph::any(usage & U::DepthAttachment) &&
        (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        return false;
    if (Halcyon::Renderer::Graph::any(usage & U::Sampled) &&
        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
        return false;
    const bool needsStorage = Halcyon::Renderer::Graph::any(usage & U::Storage) ||
        (!d.transient && d.name.find("TAAHistory") != std::string::npos);
    if (needsStorage &&
        (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
        return false;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = d.depth > 1 && !d.cube ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {d.width, d.height, d.depth};
    imageInfo.mipLevels = d.mipLevels;
    imageInfo.arrayLayers = d.cube ? 6u : (d.depth > 1 ? 1u : d.arrayLayers);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = imageUsage(info.usage);
    // Persistent TAA ping-pong images are sampled on one frame and written
    // as storage on the next.  Materialize both capabilities up front so the
    // stable persistent cache remains valid when the ping-pong direction
    // flips.
    if (!d.transient && d.name.find("TAAHistory") != std::string::npos)
    {
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (!d.transient && d.name.find("IBL_") != std::string::npos)
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (imageInfo.usage == 0) imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto allocation = allocator_->createImage(imageInfo, MemoryUsage::GpuOnly);
    if (!allocation) return false;
    out.image = allocation.value();
    out.device = device_;
    out.allocator = allocator_;
    out.format = format;
    out.extent = imageInfo.extent;
    out.samples = imageInfo.samples;
    out.mipLevels = d.mipLevels;
    out.arrayLayers = imageInfo.arrayLayers;
    out.isBuffer = false;
    out.persistent = !d.transient;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image.image;
    viewInfo.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE :
        (imageInfo.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = format == VK_FORMAT_D32_SFLOAT
                                               ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = d.mipLevels;
    viewInfo.subresourceRange.layerCount = imageInfo.arrayLayers;
    if (vkCreateImageView(device_, &viewInfo, nullptr, &out.view) != VK_SUCCESS)
    {
        allocator_->destroy(out.image);
        out.image = {};
        return false;
    }
    if (imageInfo.arrayLayers > 1 && !d.cube)
    {
        out.layerViews.resize(imageInfo.arrayLayers, VK_NULL_HANDLE);
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange.layerCount = 1;
        for (std::uint32_t layer = 0; layer < imageInfo.arrayLayers; ++layer)
        {
            viewInfo.subresourceRange.baseArrayLayer = layer;
            if (vkCreateImageView(device_, &viewInfo, nullptr, &out.layerViews[layer]) != VK_SUCCESS)
            {
                release(out);
                return false;
            }
        }
    }
    return true;
}

bool VulkanFrameGraphProvider::createBuffer(
    const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo& info, VulkanFrameGraphResource& out) noexcept
{
    if (allocator_ == nullptr || info.buffer.size == 0) return false;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = info.buffer.size;
    bufferInfo.usage = bufferUsage(info.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const MemoryUsage memoryUsage =
        Halcyon::Renderer::Graph::any(info.usage &
            Halcyon::Renderer::Graph::ResourceUsage::Uniform) || info.buffer.name == "Lights"
            ? MemoryUsage::CpuToGpu
            : MemoryUsage::GpuOnly;
    const auto allocation = allocator_->createBuffer(bufferInfo, memoryUsage);
    if (!allocation) return false;
    out.buffer = allocation.value();
    out.device = device_;
    out.allocator = allocator_;
    out.isBuffer = true;
    out.persistent = !info.buffer.transient;
    return true;
}

bool VulkanFrameGraphProvider::create(const Halcyon::Renderer::Graph::FrameGraphResourceCreateInfo& info,
    Halcyon::Renderer::Graph::FrameGraphNativeResource& output) noexcept
{
    output = {};
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || info.imported) return false;
    if ((info.kind == Halcyon::Renderer::Graph::ResourceKind::Texture && !info.texture.transient) ||
        (info.kind == Halcyon::Renderer::Graph::ResourceKind::Buffer && !info.buffer.transient))
    {
        const std::string key = persistentKey(info);
        if (const auto it = persistent_.find(key); it != persistent_.end())
        {
            output.token = it->second;
            return true;
        }
    }
    auto resource = std::make_unique<VulkanFrameGraphResource>();
    const bool created = info.kind == Halcyon::Renderer::Graph::ResourceKind::Texture
                             ? createImage(info, *resource)
                             : createBuffer(info, *resource);
    if (!created) return false;
    VulkanFrameGraphResource* token = resource.get();
    try
    {
        resources_.push_back(std::move(resource));
        if (token->persistent) persistent_.emplace(persistentKey(info), token);
    }
    catch (...)
    {
        release(*token);
        return false;
    }
    output.token = token;
    return true;
}

void VulkanFrameGraphProvider::release(VulkanFrameGraphResource& resource) noexcept
{
    if (resource.device != VK_NULL_HANDLE)
    {
        for (auto view : resource.layerViews)
            if (view != VK_NULL_HANDLE) vkDestroyImageView(resource.device, view, nullptr);
        resource.layerViews.clear();
        if (resource.view != VK_NULL_HANDLE) vkDestroyImageView(resource.device, resource.view, nullptr);
    }
    if (resource.allocator != nullptr)
    {
        if (resource.isBuffer) resource.allocator->destroy(resource.buffer);
        else resource.allocator->destroy(resource.image);
    }
    resource = {};
}

void VulkanFrameGraphProvider::destroy(const Halcyon::Renderer::Graph::FrameGraphNativeResource& native) noexcept
{
    auto* token = static_cast<VulkanFrameGraphResource*>(native.token);
    if (token == nullptr || token->persistent) return;
    // Command buffers are recorded ahead of submission.  Releasing the
    // VkImage/VkImageView here is invalid because the GPU may still execute
    // the commands that reference them.  Keep the owner in resources_ and
    // retire it after the frame fence has completed.
    const auto it = std::find_if(resources_.begin(), resources_.end(),
        [token](const auto& value) { return value.get() == token; });
    if (it == resources_.end()) return;
    const auto pending = std::find_if(deferred_.begin(), deferred_.end(),
        [token](const DeferredResource& value) { return value.resource == token; });
    if (pending == deferred_.end()) deferred_.push_back({token, activeSerial_});
}

void VulkanFrameGraphProvider::collectCompleted(std::uint64_t serial) noexcept
{
    for (auto it = deferred_.begin(); it != deferred_.end();)
    {
        if (it->serial > serial)
        {
            ++it;
            continue;
        }
        auto resourceIt = std::find_if(resources_.begin(), resources_.end(),
            [resource = it->resource](const auto& value) { return value.get() == resource; });
        if (resourceIt != resources_.end())
        {
            release(*(*resourceIt));
            resources_.erase(resourceIt);
        }
        it = deferred_.erase(it);
    }
}

void VulkanFrameGraphProvider::flushDeferred() noexcept
{
    for (auto& pending : deferred_)
    {
        auto it = std::find_if(resources_.begin(), resources_.end(),
            [resource = pending.resource](const auto& value) { return value.get() == resource; });
        if (it != resources_.end())
        {
            release(*(*it));
            resources_.erase(it);
        }
    }
    deferred_.clear();
}

bool VulkanFrameGraphProvider::createRenderTarget(
    const Halcyon::Renderer::Graph::FrameGraphRenderTargetCreateInfo& info,
    Halcyon::Renderer::Graph::FrameGraphNativeResource& output) noexcept
{
    output = {};
    auto target = std::make_unique<VulkanFrameGraphRenderTarget>();
    bool missingColor = false;
    for (std::size_t i = 0; i < Halcyon::Renderer::Graph::FrameGraphRenderPass::ATTACHMENT_COUNT; ++i)
    {
        if (info.attachments[i].token == nullptr) continue;
        target->resources[i] = info.attachments[i];
        auto* resource = static_cast<VulkanFrameGraphResource*>(info.attachments[i].token);
        if (resource == nullptr || resource->isBuffer) return false;
        if (info.descriptor.layerCount > resource->arrayLayers) return false;
        if (info.descriptor.samples != 0 &&
            static_cast<VkSampleCountFlagBits>(info.descriptor.samples) != resource->samples)
            return false;
        if (info.descriptor.viewport.width != 0 &&
            (resource->extent.width != info.descriptor.viewport.width ||
                resource->extent.height != info.descriptor.viewport.height))
            return false;
        if (i < Halcyon::Renderer::Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS)
        {
            if (resource->image.image == VK_NULL_HANDLE) return false;
        }
        target->views[i] = resource->view;
        if (i < Halcyon::Renderer::Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS) ++target->colorCount;
        if (i == Halcyon::Renderer::Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS)
        {
            target->depthView = resource->view;
            target->depthFormat = resource->format;
        }
    }
    for (std::size_t i = 0; i < target->colorCount; ++i)
    {
        if (target->views[i] == VK_NULL_HANDLE)
        {
            missingColor = true;
            break;
        }
    }
    if (missingColor || (target->colorCount == 0 && target->depthView == VK_NULL_HANDLE))
        return false;
    if (info.descriptor.attachments.depth && target->depthView == VK_NULL_HANDLE)
        return false;
    try
    {
        auto* token = target.get();
        renderTargets_.push_back(std::move(target));
        output.token = token;
        return true;
    }
    catch (...) { return false; }
}

void VulkanFrameGraphProvider::destroyRenderTarget(
    const Halcyon::Renderer::Graph::FrameGraphNativeResource& native) noexcept
{
    auto* token = static_cast<VulkanFrameGraphRenderTarget*>(native.token);
    if (token == nullptr) return;
    const auto it = std::find_if(renderTargets_.begin(), renderTargets_.end(),
        [token](const auto& value) { return value.get() == token; });
    if (it != renderTargets_.end()) renderTargets_.erase(it);
}

VkImage VulkanFrameGraphProvider::image(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    return resource != nullptr && !resource->isBuffer ? resource->image.image : VK_NULL_HANDLE;
}
VkBuffer VulkanFrameGraphProvider::buffer(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    return resource != nullptr && resource->isBuffer ? resource->buffer.buffer : VK_NULL_HANDLE;
}
VkImageView VulkanFrameGraphProvider::view(Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    return resource != nullptr && !resource->isBuffer ? resource->view : VK_NULL_HANDLE;
}
const ImageAllocation* VulkanFrameGraphProvider::nativeAllocation(
    Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    return resource != nullptr && !resource->isBuffer ? &resource->image : nullptr;
}
const BufferAllocation* VulkanFrameGraphProvider::nativeBufferAllocation(
    Halcyon::Renderer::Graph::FrameGraphNativeResource token) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    return resource != nullptr && resource->isBuffer ? &resource->buffer : nullptr;
}
VkImageView VulkanFrameGraphProvider::layerView(
    Halcyon::Renderer::Graph::FrameGraphNativeResource token, std::uint32_t layer) const noexcept
{
    auto* resource = static_cast<const VulkanFrameGraphResource*>(token.token);
    if (resource == nullptr || resource->isBuffer) return VK_NULL_HANDLE;
    if (layer < resource->layerViews.size()) return resource->layerViews[layer];
    return layer == 0 ? resource->view : VK_NULL_HANDLE;
}

void VulkanFrameGraphProvider::resetTransient() noexcept
{
    // Callers use this during a resize after waiting for the device.  Flush
    // queued retirements first so no stale owners survive a swapchain rebuild.
    flushDeferred();
    for (auto it = resources_.begin(); it != resources_.end();)
    {
        if (!(*it)->persistent)
        {
            release(*(*it));
            it = resources_.erase(it);
        }
        else ++it;
    }
}

void VulkanFrameGraphProvider::recreatePersistent() noexcept
{
    flushDeferred();
    for (auto it = resources_.begin(); it != resources_.end();)
    {
        if ((*it)->persistent)
        {
            release(*(*it));
            it = resources_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    persistent_.clear();
}

void VulkanFrameGraphProvider::shutdown() noexcept
{
    flushDeferred();
    for (auto& target : renderTargets_) target.reset();
    renderTargets_.clear();
    for (auto& resource : resources_)
        if (resource) release(*resource);
    resources_.clear();
    deferred_.clear();
    persistent_.clear();
    allocator_ = nullptr;
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    activeSerial_ = 0;
}

} // namespace Halcyon::Vulkan
