#pragma once

#include "FrameGraphId.h"
#include "FrameGraphRenderPass.h"
#include "FrameGraphTexture.h"

#include <memory>
#include <stdexcept>
#include <string_view>

namespace Halcyon::Renderer::Graph
{
class FrameGraph;
class FrameGraphResources final
{
public:
    // A resource view is created for one pass during execution.  Its
    // constructor is intentionally private; FrameGraph owns the lifetime,
    // just like Filament's FrameGraphResources.
    FrameGraphResources(const FrameGraphResources&) = delete;
    FrameGraphResources& operator=(const FrameGraphResources&) = delete;

    std::string_view passName() const noexcept;
    // Filament spells this accessor getPassName(); keep both spellings so
    // code can be ported between the two implementations without adapters.
    std::string_view getPassName() const noexcept
    {
        return passName();
    }

    struct RenderPassInfo
    {
        FrameGraphNativeResource target{};
        FrameGraphRenderPass::Descriptor descriptor{};
        FrameGraphAttachmentFlags discardStart{};
        FrameGraphAttachmentFlags discardEnd{};
        FrameGraphAttachmentFlags clearFlags{};
        FrameGraphAttachmentFlags readOnly{};
    };

    template <typename Resource>
    const Resource& get(FrameGraphId<Resource> id) const
    {
        const auto* value =
            static_cast<const Resource*>(getRaw(id, ResourceKindOf<Resource>::value));
        if (value == nullptr)
        {
            throw std::out_of_range("FrameGraph resource was not declared by this pass");
        }
        return *value;
    }
    template <typename Resource>
    const typename Resource::Descriptor& getDescriptor(FrameGraphId<Resource> id) const
    {
        return get(id).descriptor;
    }
    template <typename Resource>
    const typename Resource::SubResourceDescriptor& getSubResourceDescriptor(
        FrameGraphId<Resource> id) const
    {
        (void)get(id);
        const auto* raw = subresourceRaw(
            id, ResourceKindOf<Resource>::value);
        if (raw == nullptr)
        {
            throw std::out_of_range("invalid frame graph subresource handle");
        }
        return *static_cast<const typename Resource::SubResourceDescriptor*>(raw);
    }
    template <typename Resource>
    const typename Resource::Usage& getUsage(FrameGraphId<Resource> id) const
    {
        if (!declared(id))
        {
            throw std::out_of_range("FrameGraph resource was not declared by this pass");
        }
        usage_ = usageRaw(id);
        return usage_;
    }
    template <typename Resource>
    void detach(FrameGraphId<Resource> id,
        Resource* output,
        typename Resource::Descriptor* descriptor) const
    {
        const auto* value = std::addressof(get(id));
        if (output)
        {
            *output = *value;
        }
        if (descriptor)
        {
            *descriptor = value->descriptor;
        }
        detachRaw(id);
    }

    const FrameGraphTexture& getTexture(FrameGraphId<FrameGraphTexture> id) const
    {
        return get(id);
    }

    RenderPassInfo getRenderPassInfo(std::uint32_t = 0) const noexcept;

private:
    template <typename>
    struct ResourceKindOf;
    friend class FrameGraph;
    FrameGraphResources(const FrameGraph* graph, FrameGraphHandle pass) noexcept;
    const void* getRaw(FrameGraphHandle, ResourceKind) const noexcept;
    const void* subresourceRaw(FrameGraphHandle, ResourceKind) const noexcept;
    ResourceUsage usageRaw(FrameGraphHandle) const noexcept;
    bool declared(FrameGraphHandle) const noexcept;
    void detachRaw(FrameGraphHandle) const noexcept;
    mutable ResourceUsage usage_ = ResourceUsage::None;
    const FrameGraph* graph_ = nullptr;
    FrameGraphHandle pass_{};
};
template <>
struct FrameGraphResources::ResourceKindOf<FrameGraphTexture>
{
    static constexpr ResourceKind value = ResourceKind::Texture;
};
template <>
struct FrameGraphResources::ResourceKindOf<FrameGraphBuffer>
{
    static constexpr ResourceKind value = ResourceKind::Buffer;
};
} // namespace Halcyon::Renderer::Graph
