#pragma once

#include "Core/Result.h"
#include "Renderer/Graph/FrameGraph.h"

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Central declaration of every image and buffer used by the deferred frame path.
// FrameGraph owns transient lifetimes and VulkanFrameGraphProvider owns native
// allocations; this object owns the extent-dependent resource specification.
class VulkanFrameResources final
{
public:
    // Lucy's three fixed LODs fit below one million meshlets.  Keeping this
    // cap explicit makes the indirect count ABI and shader saturation bound
    // identical across frame resources and culling.
    // Visibility IDs reserve zero for background and store meshletIndex + 1
    // in 20 bits, so the representable meshlet count is 2^20 - 1.
    static constexpr std::uint32_t MaxVirtualGeometryMeshlets = (1u << 20u) - 1u;
    static constexpr std::uint32_t MaxVirtualGeometryInstances = 256u;
    static constexpr std::uint32_t CsmResolution = 2048;
    static constexpr std::uint32_t ClusterTileSize = 64;
    static constexpr std::uint32_t ClusterSlices = 24;
    static constexpr std::uint32_t MaxLightsPerCluster = 128;
    static constexpr std::uint32_t ClusterBuildGroupSize = 64;

    struct Handles
    {
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> csm;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> gbuffer0;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> gbuffer1;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> gbuffer2;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> motion;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> instanceId;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> depth;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> hiz;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> hdr;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> historyA;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> historyB;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> irradiance;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> prefiltered;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> brdfLut;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> visibility;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> visibilityPrimitive;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> visibilityBarycentrics;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> materialClassification;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualTransforms;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualMeshMaterials;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualCullFrame;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> visibleMeshlets;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> visibleMeshletCount;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> selectedLodNodes;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> selectedLodCount;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> lodBalanceDepth;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualPageRequests;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualPageRequestCount;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> meshletIndirect;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> meshletIndirectCount;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> meshletMeshIndirect;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> meshletMeshIndirectCount;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> virtualValidation;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> clusterRanges;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> clusterIndices;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> clusterOverflow;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> lights;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> clusterCamera;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphBuffer> shadowConstants;
        std::uint32_t clusterCount = 0;
    };

    [[nodiscard]] Halcyon::Result<void> recreate(VkExtent2D extent) noexcept;
    void reset() noexcept { extent_ = {}; }

    [[nodiscard]] Halcyon::Result<Handles> declare(
        Halcyon::Renderer::Graph::FrameGraph& graph, std::uint32_t lightCount) const;

    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] std::uint32_t tilesX() const noexcept;
    [[nodiscard]] std::uint32_t tilesY() const noexcept;
    [[nodiscard]] std::uint32_t clusterCount() const noexcept;

private:
    VkExtent2D extent_{};
};

} // namespace Halcyon::Vulkan
