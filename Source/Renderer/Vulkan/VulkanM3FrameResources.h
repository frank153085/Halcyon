#pragma once

#include "Core/Result.h"
#include "Renderer/Graph/FrameGraph.h"

#include <cstdint>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

// Central declaration of every image and buffer used by the Vulkan M3 path.
// FrameGraph owns transient lifetimes and VulkanFrameGraphProvider owns native
// allocations; this object owns the extent-dependent resource specification.
class VulkanM3FrameResources final
{
public:
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
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> depth;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> hdr;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> historyA;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> historyB;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> irradiance;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> prefiltered;
        Halcyon::Renderer::Graph::FrameGraphId<Halcyon::Renderer::Graph::FrameGraphTexture> brdfLut;
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
