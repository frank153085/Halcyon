#include "VulkanM3FrameResources.h"

#include "Renderer/Scene/FramePacket.h"

#include <algorithm>
#include <cstddef>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;

Halcyon::Result<void> VulkanM3FrameResources::recreate(VkExtent2D extent) noexcept
{
    if (extent.width == 0 || extent.height == 0)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "Vulkan M3 frame resources require a non-zero swapchain extent"});
    }
    extent_ = extent;
    return Halcyon::Result<void>::success();
}

std::uint32_t VulkanM3FrameResources::tilesX() const noexcept
{
    return std::max(1u, (extent_.width + ClusterTileSize - 1u) / ClusterTileSize);
}

std::uint32_t VulkanM3FrameResources::tilesY() const noexcept
{
    return std::max(1u, (extent_.height + ClusterTileSize - 1u) / ClusterTileSize);
}

std::uint32_t VulkanM3FrameResources::clusterCount() const noexcept
{
    return tilesX() * tilesY() * ClusterSlices;
}

Halcyon::Result<VulkanM3FrameResources::Handles> VulkanM3FrameResources::declare(
    Graph::FrameGraph& graph, std::uint32_t lightCount) const
{
    if (extent_.width == 0 || extent_.height == 0)
    {
        return Halcyon::Result<Handles>::failure(
            {Halcyon::ErrorCode::InvalidState,
                "Vulkan M3 frame resources were not recreated for the current swapchain"});
    }

    Handles result{};
    result.csm = graph.createTexture({"CSM", CsmResolution, CsmResolution, 1, 1, 4,
        Graph::TextureFormat::D32Float, true});
    result.gbuffer0 = graph.createTexture({"GBuffer0", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA8Srgb, true});
    result.gbuffer1 = graph.createTexture({"GBuffer1", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA16Float, true});
    result.gbuffer2 = graph.createTexture({"GBuffer2", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA8Unorm, true});
    result.motion = graph.createTexture({"Motion", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RG16Float, true});
    result.instanceId = graph.createTexture({"InstanceId", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::R32Uint, true});
    result.depth = graph.createTexture({"SceneDepth", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::D32Float, true});
    const std::uint32_t hizWidth = std::max(1u, (extent_.width + 1u) / 2u);
    const std::uint32_t hizHeight = std::max(1u, (extent_.height + 1u) / 2u);
    std::uint32_t hizMips = 1;
    for (std::uint32_t size = std::max(hizWidth, hizHeight); size > 1; size >>= 1)
        ++hizMips;
    result.hiz = graph.createTexture({"HiZ", hizWidth, hizHeight, 1, hizMips, 1,
        Graph::TextureFormat::R32Float, false});
    result.hdr = graph.createTexture({"HDR", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA16Float, true});
    result.historyA = graph.createTexture({"TAAHistoryA", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA16Float, false});
    result.historyB = graph.createTexture({"TAAHistoryB", extent_.width, extent_.height, 1, 1, 1,
        Graph::TextureFormat::RGBA16Float, false});
    result.irradiance = graph.createTexture({"IBL_Irradiance", 32, 32, 1, 1, 6,
        Graph::TextureFormat::RGBA16Float, false, true});
    result.prefiltered = graph.createTexture({"IBL_Prefiltered", 64, 64, 1, 7, 6,
        Graph::TextureFormat::RGBA16Float, false, true});
    result.brdfLut = graph.createTexture({"IBL_BrdfLut", 128, 128, 1, 1, 1,
        Graph::TextureFormat::RG16Float, false});

    result.clusterCount = clusterCount();
    result.clusterRanges = graph.createBuffer(
        {"ClusterRanges", result.clusterCount * 8u, 8u, true});
    result.clusterIndices = graph.createBuffer({"ClusterIndices",
        result.clusterCount * MaxLightsPerCluster * 4u, 4u, true});
    result.clusterOverflow = graph.createBuffer({"ClusterOverflow", 4u, 4u, true});
    result.lights = graph.createBuffer({"Lights",
        std::max<std::size_t>(1, lightCount) * sizeof(Halcyon::Renderer::Scene::LightData),
        static_cast<std::uint32_t>(sizeof(Halcyon::Renderer::Scene::LightData)), true});
    result.clusterCamera = graph.createBuffer(
        {"ClusterCamera", sizeof(float) * 16u * 2u, 16u, true});
    result.shadowConstants = graph.createBuffer(
        {"ShadowConstants", sizeof(float) * (16u * 4u + 4u * 2u), 16u, true});
    return Halcyon::Result<Handles>::success(result);
}

} // namespace Halcyon::Vulkan
