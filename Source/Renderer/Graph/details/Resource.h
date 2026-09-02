#pragma once

// Private resource nodes for the Halcyon frame graph.  The layout mirrors
// Filament's fg/details/Resource.h, but all concrete allocation is delegated
// to the backend-neutral ResourceCreationContext/ResourceAllocator pair.

#include "DependencyGraph.h"
#include "ResourceAllocator.h"
#include "ResourceCreationContext.h"
#include "../FrameGraphRenderPass.h"
#include "../FrameGraphId.h"
#include "../FrameGraphTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace Halcyon::Renderer::Graph
{

class PassNode;
class ResourceNode;

// Resource kinds are specialized by FrameGraph.h for the built-in texture
// and buffer contracts.  This fallback keeps the private resource template
// usable by small backend-neutral extensions that do not need a distinct kind.
template <typename RESOURCE>
struct ResourceKindOf
{
    static constexpr ResourceKind value = ResourceKind::Buffer;
};

template <>
struct ResourceKindOf<FrameGraphTexture>
{
    static constexpr ResourceKind value = ResourceKind::Texture;
};

template <>
struct ResourceKindOf<FrameGraphBuffer>
{
    static constexpr ResourceKind value = ResourceKind::Buffer;
};

/**
 * Base edge type used to keep resource-specific edge payloads type-safe.
 *
 * Edges are intentionally independent from the storage representation of
 * DependencyGraph.  This lets the graph stay a compact index-based DAG while
 * resource nodes retain the richer reader/writer metadata expected by the
 * Filament-style implementation.
 */
class ResourceEdgeBase
{
public:
    ResourceEdgeBase(PassNode* pass, ResourceNode* resource, bool writer) noexcept
        : pass(pass),
          resource(resource),
          writer(writer)
    {
    }
    ResourceEdgeBase(const ResourceEdgeBase&) = delete;
    ResourceEdgeBase& operator=(const ResourceEdgeBase&) = delete;
    virtual ~ResourceEdgeBase() noexcept = default;

    PassNode* pass = nullptr;
    ResourceNode* resource = nullptr;
    bool writer = false;
    ResourceEdgeBase* next = nullptr;
};

/** Generic parts shared by every virtual resource. */
class VirtualResource
{
public:
    VirtualResource* parent = this;
    std::string name;

    // Computed while compiling the graph.
    std::uint32_t refcount = 0;
    PassNode* first = nullptr;
    PassNode* last = nullptr;
    ResourceKind kind = ResourceKind::Buffer;
    bool imported = false;
    bool detached = false;
    // Legacy flat-resource fields retained for source compatibility.  The
    // richer allocator contract above is canonical; these values mirror the
    // lifetime/token data used by the original Halcyon prototype.
    bool transient = true;
    FrameGraphNativeResource native{};
    std::int32_t firstPass = -1;
    std::int32_t lastPass = -1;

    explicit VirtualResource(std::string_view resourceName = {})
        : name(resourceName)
    {
    }
    VirtualResource(VirtualResource* parentResource, std::string_view resourceName = {})
        : parent(parentResource ? parentResource : this),
          name(resourceName)
    {
    }
    VirtualResource(const VirtualResource&) = delete;
    VirtualResource& operator=(const VirtualResource&) = delete;
    virtual ~VirtualResource() noexcept;

    /** Updates reference count and first/last users, propagating to parents. */
    void neededByPass(PassNode* pass) noexcept;

    bool isSubResource() const noexcept
    {
        return parent != this;
    }

    VirtualResource* getResource() noexcept
    {
        VirtualResource* resource = this;
        // Be defensive about malformed parent chains supplied by extensions.
        for (std::size_t guard = 0; resource->parent != resource && guard < 65536; ++guard)
        {
            resource = resource->parent;
            if (resource == nullptr)
            {
                return this;
            }
        }
        return resource;
    }
    const VirtualResource* getResource() const noexcept
    {
        return const_cast<VirtualResource*>(this)->getResource();
    }

    /** Resolve effective usage after culling and before materialization. */
    virtual void resolveUsage(DependencyGraph& graph,
        const ResourceEdgeBase* readers,
        const ResourceEdgeBase* writer) noexcept = 0;

    /** Materialize/destroy the concrete object for an active lifetime. */
    virtual void devirtualize(const ResourceCreationContext& context) noexcept = 0;
    virtual void destroy(const ResourceCreationContext& context) noexcept = 0;

    /** Destroy an edge allocated by this resource. */
    virtual void destroyEdge(ResourceEdgeBase* edge) noexcept = 0;

    virtual std::string usageString() const
    {
        return {};
    }
    virtual bool isImported() const noexcept
    {
        return imported;
    }
    virtual std::size_t getSize() const noexcept = 0;

protected:
    // These free-function hooks are defined by ResourceNode.cpp.  Keeping
    // them out of this header avoids a circular dependency on ResourceNode.h.
    static ResourceEdgeBase* findReaderEdge(ResourceNode* resourceNode,
        PassNode* pass) noexcept;
    static ResourceEdgeBase* findWriterEdge(ResourceNode* resourceNode,
        PassNode* pass) noexcept;
    static void addReaderEdge(ResourceNode* resourceNode,
        ResourceEdgeBase* edge) noexcept;
    static void setWriterEdge(ResourceNode* resourceNode,
        ResourceEdgeBase* edge) noexcept;
};

namespace detail
{

template <typename Usage>
constexpr void mergeUsage(Usage& destination, Usage value) noexcept
{
    if constexpr (requires { destination |= value; })
    {
        destination |= value;
    }
    else if constexpr (requires { destination = destination | value; })
    {
        destination = destination | value;
    }
    else
    {
        (void)destination;
        (void)value;
    }
}

template <typename Usage>
constexpr bool usageContains(Usage available, Usage requested) noexcept
{
    if constexpr (requires { available & requested; })
    {
        if constexpr (requires { static_cast<bool>(available & requested); })
        {
            return static_cast<bool>((available & requested) == requested);
        }
        else
        {
            return (available & requested) == requested;
        }
    }
    else
    {
        return available == requested;
    }
}

} // namespace detail

/** Resource-specific virtual resource payload. */
template <typename RESOURCE>
class Resource : public VirtualResource
{
public:
    using Descriptor = typename RESOURCE::Descriptor;
    using SubResourceDescriptor = typename RESOURCE::SubResourceDescriptor;
    using Usage = typename RESOURCE::Usage;

    RESOURCE resource{};
    Descriptor descriptor{};
    SubResourceDescriptor subResourceDescriptor{};
    Usage usage{};

    class ResourceEdge final : public ResourceEdgeBase
    {
    public:
        Usage usage{};

        ResourceEdge(PassNode* pass, ResourceNode* resourceNode,
            bool writer, Usage edgeUsage) noexcept
            : ResourceEdgeBase(pass, resourceNode, writer),
              usage(edgeUsage)
        {
        }
    };

    explicit Resource(std::string_view resourceName, const Descriptor& resourceDescriptor = {})
        : VirtualResource(resourceName),
          descriptor(resourceDescriptor)
    {
        kind = ResourceKindOf<RESOURCE>::value;
    }

    Resource(Resource* parentResource, std::string_view resourceName,
        const SubResourceDescriptor& subresourceDescriptor = {})
        : VirtualResource(parentResource, resourceName),
          subResourceDescriptor(subresourceDescriptor)
    {
        kind = ResourceKindOf<RESOURCE>::value;
        if (parentResource != nullptr)
        {
            descriptor = parentResource->descriptor;
            if constexpr (requires {
                              RESOURCE::generateSubResourceDescriptor(
                                  descriptor, subresourceDescriptor);
                          })
            {
                descriptor = RESOURCE::generateSubResourceDescriptor(
                    descriptor, subresourceDescriptor);
            }
        }
    }

    ~Resource() noexcept override = default;

    // Pass -> resource edge (write).
    bool connect(DependencyGraph& graph, PassNode* pass,
        ResourceNode* resourceNode, Usage edgeUsage)
    {
        (void)graph;
        if (auto* edge = findWriterEdge(resourceNode, pass))
        {
            detail::mergeUsage(static_cast<ResourceEdge*>(edge)->usage, edgeUsage);
            return true;
        }
        auto* edge = new ResourceEdge(pass, resourceNode, true, edgeUsage);
        setWriterEdge(resourceNode, edge);
        return true;
    }

    // Resource -> pass edge (read).
    bool connect(DependencyGraph& graph, ResourceNode* resourceNode,
        PassNode* pass, Usage edgeUsage)
    {
        (void)graph;
        if (auto* edge = findReaderEdge(resourceNode, pass))
        {
            detail::mergeUsage(static_cast<ResourceEdge*>(edge)->usage, edgeUsage);
            return true;
        }
        auto* edge = new ResourceEdge(pass, resourceNode, false, edgeUsage);
        addReaderEdge(resourceNode, edge);
        return true;
    }

    std::size_t getSize() const noexcept override
    {
        return sizeof(Resource);
    }

protected:
    void resolveUsage(DependencyGraph& graph,
        const ResourceEdgeBase* readers,
        const ResourceEdgeBase* writer) noexcept override
    {
        usage = {};
        for (auto* edge = readers; edge != nullptr; edge = edge->next)
        {
            if (edge->writer)
            {
                continue;
            }
            auto* typed = static_cast<const ResourceEdge*>(edge);
            detail::mergeUsage(usage, typed->usage);
        }
        if (writer != nullptr)
        {
            auto* typed = static_cast<const ResourceEdge*>(writer);
            detail::mergeUsage(usage, typed->usage);
        }
        (void)graph;

        // A subresource contributes its usage to every ancestor resource.
        auto* ancestor = this;
        while (ancestor->parent != ancestor && ancestor->parent != nullptr)
        {
            ancestor = static_cast<Resource*>(ancestor->parent);
            detail::mergeUsage(ancestor->usage, usage);
        }
    }

    void devirtualize(const ResourceCreationContext& context) noexcept override
    {
        if (isSubResource())
        {
            if (parent != nullptr)
            {
                resource = static_cast<const Resource*>(parent)->resource;
            }
            return;
        }
        (void)ResourceAllocator<RESOURCE>::create(
            resource, context, name, descriptor, usage);
    }

    void destroy(const ResourceCreationContext& context) noexcept override
    {
        if (!detached && !isSubResource())
        {
            ResourceAllocator<RESOURCE>::destroy(resource, context);
        }
    }

    void destroyEdge(ResourceEdgeBase* edge) noexcept override
    {
        delete static_cast<ResourceEdge*>(edge);
    }
};

/** Imported resources carry a concrete object and never allocate/destroy it. */
template <typename RESOURCE>
class ImportedResource : public Resource<RESOURCE>
{
public:
    using Base = Resource<RESOURCE>;
    using Descriptor = typename RESOURCE::Descriptor;
    using Usage = typename RESOURCE::Usage;

    ImportedResource(std::string_view resourceName, const Descriptor& resourceDescriptor,
        Usage resourceUsage, const RESOURCE& concrete)
        : Base(resourceName, resourceDescriptor)
    {
        this->resource = concrete;
        this->usage = resourceUsage;
        this->imported = true;
    }

    std::size_t getSize() const noexcept override
    {
        return sizeof(ImportedResource);
    }

protected:
    void devirtualize(const ResourceCreationContext&) noexcept override
    {
        // Imported resources are already concrete.
    }
    void destroy(const ResourceCreationContext&) noexcept override
    {
        // Ownership remains with the caller.
    }

    bool connect(DependencyGraph& graph, PassNode* pass,
        ResourceNode* resourceNode, Usage edgeUsage)
    {
        if (!detail::usageContains(this->usage, edgeUsage))
        {
            return false;
        }
        return Base::connect(graph, pass, resourceNode, edgeUsage);
    }
    bool connect(DependencyGraph& graph, ResourceNode* resourceNode,
        PassNode* pass, Usage edgeUsage)
    {
        if (!detail::usageContains(this->usage, edgeUsage))
        {
            return false;
        }
        return Base::connect(graph, resourceNode, pass, edgeUsage);
    }

};

/**
 * Portable imported render-target metadata.  Native render-target creation is
 * intentionally outside this class; the token is supplied by the provider.
 */
class ImportedRenderTarget final : public ImportedResource<FrameGraphTexture>
{
public:
    FrameGraphNativeResource target{};
    FrameGraphRenderPass::ImportDescriptor importedDescriptor{};

    ImportedRenderTarget(std::string_view resourceName,
        const FrameGraphTexture::Descriptor& descriptor,
        const FrameGraphRenderPass::ImportDescriptor& importDescriptor,
        const FrameGraphTexture& texture)
        : ImportedResource<FrameGraphTexture>(resourceName, descriptor,
              resourceUsageForAttachments(importDescriptor.attachments), texture),
          target(texture.native),
          importedDescriptor(importDescriptor)
    {
    }

    std::size_t getSize() const noexcept override
    {
        return sizeof(ImportedRenderTarget);
    }
};

} // namespace Halcyon::Renderer::Graph
