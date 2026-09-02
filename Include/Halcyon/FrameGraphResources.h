#pragma once

#include "FrameGraphId.h"

#include <memory>
#include <stdexcept>
#include <string_view>

namespace Halcyon::Renderer::Graph
{
class FrameGraph;
class FrameGraphResources final
{
public:
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

private:
    template <typename>
    struct ResourceKindOf;
    friend class FrameGraph;
    FrameGraphResources(const FrameGraph* graph, FrameGraphHandle pass)
            : graph_(graph),
              pass_(pass)
    {
    }
    const void* getRaw(FrameGraphHandle, ResourceKind) const noexcept;
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
