#pragma once

// Resource allocation policy corresponding to Filament's details/
// ResourceAllocator.h.  It intentionally knows only Halcyon's provider and
// value types; backend-specific allocators can specialize this template in a
// downstream module.

#include "../FrameGraphDummyLink.h"
#include "../FrameGraphTexture.h"
#include "../FrameGraphTypes.h"
#include "ResourceCreationContext.h"

#include <string_view>
#include <type_traits>
#include <utility>

namespace Halcyon::Renderer::Graph
{

namespace detail
{

template <typename Result>
bool normalizeCreateResult(Result&& result) noexcept
{
    using R = std::remove_cvref_t<Result>;
    if constexpr (std::is_same_v<R, bool>)
    {
        return result;
    }
    else if constexpr (std::is_convertible_v<R, bool>)
    {
        return static_cast<bool>(result);
    }
    else
    {
        return true;
    }
}

} // namespace detail

/**
 * Default allocator for user-defined, backend-neutral graph resources.
 *
 * A resource may provide one of the following create signatures:
 *   create(ResourceCreationContext const&, string_view, Descriptor const&, Usage)
 *   create(string_view, Descriptor const&, Usage)
 *   create(Descriptor const&)
 * The corresponding destroy signature may take the context or no arguments.
 * If no signature is available, creation reports false and destruction is a
 * no-op.  FrameGraphTexture/Buffer and FrameGraphDummyLink are specialized
 * below because they use the provider token contract.
 */
template <typename Resource>
struct ResourceAllocator
{
    using Descriptor = typename Resource::Descriptor;
    using Usage = typename Resource::Usage;

    static bool create(Resource& resource,
        const ResourceCreationContext& context,
        std::string_view name,
        const Descriptor& descriptor,
        Usage usage = {}) noexcept
    {
        if constexpr (requires { resource.create(context, name, descriptor, usage); })
        {
            using Return = decltype(resource.create(context, name, descriptor, usage));
            if constexpr (std::is_void_v<Return>)
            {
                resource.create(context, name, descriptor, usage);
                return true;
            }
            else
            {
                return detail::normalizeCreateResult(
                    resource.create(context, name, descriptor, usage));
            }
        }
        else if constexpr (requires { resource.create(name, descriptor, usage); })
        {
            using Return = decltype(resource.create(name, descriptor, usage));
            if constexpr (std::is_void_v<Return>)
            {
                resource.create(name, descriptor, usage);
                return true;
            }
            else
            {
                return detail::normalizeCreateResult(resource.create(name, descriptor, usage));
            }
        }
        else if constexpr (requires { resource.create(descriptor); })
        {
            using Return = decltype(resource.create(descriptor));
            if constexpr (std::is_void_v<Return>)
            {
                resource.create(descriptor);
                return true;
            }
            else
            {
                return detail::normalizeCreateResult(resource.create(descriptor));
            }
        }
        else
        {
            (void)resource;
            (void)context;
            (void)name;
            (void)descriptor;
            (void)usage;
            return false;
        }
    }

    static void destroy(Resource& resource,
        const ResourceCreationContext& context) noexcept
    {
        if constexpr (requires { resource.destroy(context); })
        {
            resource.destroy(context);
        }
        else if constexpr (requires { resource.destroy(); })
        {
            resource.destroy();
        }
        else
        {
            (void)resource;
            (void)context;
        }
    }
};

template <>
struct ResourceAllocator<FrameGraphTexture>
{
    static bool create(FrameGraphTexture& resource,
        const ResourceCreationContext& context,
        std::string_view name,
        const FrameGraphTexture::Descriptor& descriptor,
        FrameGraphTexture::Usage usage = {}) noexcept
    {
        (void)usage;
        const auto info = makeFrameGraphTextureCreateInfo(name, descriptor);
        resource.descriptor = info.texture;
        return context.create(info, resource.native);
    }

    static void destroy(FrameGraphTexture& resource,
        const ResourceCreationContext& context) noexcept
    {
        context.destroy(resource.native);
        resource.native = {};
    }
};

template <>
struct ResourceAllocator<FrameGraphBuffer>
{
    static bool create(FrameGraphBuffer& resource,
        const ResourceCreationContext& context,
        std::string_view name,
        const FrameGraphBuffer::Descriptor& descriptor,
        FrameGraphBuffer::Usage usage = {}) noexcept
    {
        (void)usage;
        FrameGraphResourceCreateInfo info{};
        info.kind = ResourceKind::Buffer;
        info.buffer = descriptor;
        if (!name.empty())
        {
            info.buffer.name.assign(name.data(), name.size());
        }
        info.imported = false;
        resource.descriptor = info.buffer;
        return context.create(info, resource.native);
    }

    static void destroy(FrameGraphBuffer& resource,
        const ResourceCreationContext& context) noexcept
    {
        context.destroy(resource.native);
        resource.native = {};
    }
};

template <>
struct ResourceAllocator<FrameGraphDummyLink>
{
    static bool create(FrameGraphDummyLink&,
        const ResourceCreationContext&,
        std::string_view,
        const FrameGraphDummyLink::Descriptor&,
        FrameGraphDummyLink::Usage = {}) noexcept
    {
        return true;
    }

    static void destroy(FrameGraphDummyLink& resource,
        const ResourceCreationContext&) noexcept
    {
        resource.native = {};
    }
};

} // namespace Halcyon::Renderer::Graph
