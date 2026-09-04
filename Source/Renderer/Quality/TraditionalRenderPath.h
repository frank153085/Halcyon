#pragma once

#include "../Graph/FrameGraph.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace Halcyon::Renderer::Quality
{

struct TraditionalPathConfig
{
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t cascadeCount = 4;
    bool enableIbl = true;
    bool enableClusteredLighting = true;
    bool enableTransparency = true;
    bool enableTaa = true;
};

struct TraditionalPathResources
{
    Graph::TextureHandle shadowMap{};
    Graph::TextureHandle gbufferAlbedo{};
    Graph::TextureHandle gbufferNormalRoughness{};
    Graph::TextureHandle gbufferMaterial{};
    Graph::TextureHandle motionVectors{};
    Graph::TextureHandle sceneDepth{};
    Graph::TextureHandle hdrColor{};
    Graph::TextureHandle taaHistoryA{};
    Graph::TextureHandle taaHistoryB{};
    Graph::TextureHandle taaColor{};
    Graph::TextureHandle output{};
    Graph::TextureHandle irradiance{};
    Graph::TextureHandle prefilteredEnvironment{};
    Graph::TextureHandle brdfLut{};
    Graph::BufferHandle clusterRanges{};
    Graph::BufferHandle clusterIndices{};
    Graph::BufferHandle clusterOverflow{};
    Graph::BufferHandle lightBuffer{};
    Graph::BufferHandle clusterCamera{};
    Graph::BufferHandle shadowConstants{};
};

// Build the canonical M3 pass sequence.  Backend code supplies callbacks for
// the actual Vulkan work; this helper centralises resource declarations and
// ordering so the CPU reference path and GPU path cannot silently diverge.
[[nodiscard]] inline TraditionalPathResources buildTraditionalRenderPath(
    Graph::FrameGraph& graph, const TraditionalPathConfig& config = {})
{
    using namespace Graph;
    const std::uint32_t width = config.width == 0 ? 1u : config.width;
    const std::uint32_t height = config.height == 0 ? 1u : config.height;
    TraditionalPathResources resources;
    // M3 always uses four stable cascades.  Keep the legacy configuration
    // field source-compatible, but do not let a caller create an array whose
    // layer count disagrees with the fixed pass topology below.
    constexpr std::uint32_t kCascadeCount = 4u;
    resources.shadowMap = graph.createTexture(TextureDesc{.name = "CSM shadow map",
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = kCascadeCount,
        .format = TextureFormat::D32Float,
        .transient = true});
    resources.gbufferAlbedo = graph.createTexture(TextureDesc{.name = "GBuffer albedo",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA8Srgb,
        .transient = true});
    resources.gbufferNormalRoughness = graph.createTexture(TextureDesc{.name = "GBuffer normal",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = true});
    resources.gbufferMaterial = graph.createTexture(TextureDesc{.name = "GBuffer material",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA8Unorm,
        .transient = true});
    resources.motionVectors = graph.createTexture(TextureDesc{.name = "Motion vectors",
        .width = width,
        .height = height,
        .format = TextureFormat::RG16Float,
        .transient = true});
    resources.sceneDepth = graph.createTexture(TextureDesc{.name = "Scene depth",
        .width = width,
        .height = height,
        .format = TextureFormat::D32Float,
        .transient = true});
    resources.hdrColor = graph.createTexture(TextureDesc{.name = "HDR lighting",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = true});
    resources.taaHistoryA = graph.createTexture(TextureDesc{.name = "TAA history A",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = false});
    resources.taaHistoryB = graph.createTexture(TextureDesc{.name = "TAA history B",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = false});
    resources.taaColor = resources.taaHistoryB;
    resources.output = graph.createTexture(TextureDesc{.name = "Tonemapped output",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA8Unorm,
        .transient = false});
    resources.irradiance = graph.createTexture(TextureDesc{.name = "IBL irradiance",
        .width = 32,
        .height = 32,
        .mipLevels = 1,
        .arrayLayers = 6,
        .format = TextureFormat::RGBA16Float,
        .transient = false,
        .cube = true});
    resources.prefilteredEnvironment = graph.createTexture(TextureDesc{
        .name = "IBL prefiltered environment",
        .width = 64,
        .height = 64,
        .mipLevels = 7,
        .arrayLayers = 6,
        .format = TextureFormat::RGBA16Float,
        .transient = false,
        .cube = true});
    resources.brdfLut = graph.createTexture(TextureDesc{.name = "IBL BRDF LUT",
        .width = 128,
        .height = 128,
        .format = TextureFormat::RG16Float,
        .transient = false});
    const std::uint32_t tilesX = (width + 63u) / 64u;
    const std::uint32_t tilesY = (height + 63u) / 64u;
    const std::uint32_t clusterCount = std::max(1u, tilesX * tilesY * 24u);
    resources.clusterRanges = graph.createBuffer(BufferDesc{.name = "Cluster ranges",
        .size = static_cast<std::uint64_t>(clusterCount) * 8u,
        .stride = 8u,
        .transient = true});
    resources.clusterIndices = graph.createBuffer(BufferDesc{.name = "Cluster indices",
        .size = static_cast<std::uint64_t>(clusterCount) * 128u * 4u,
        .stride = 4u,
        .transient = true});
    resources.clusterOverflow = graph.createBuffer(BufferDesc{.name = "Cluster overflow",
        .size = 4u,
        .stride = 4u,
        .transient = true});
    resources.lightBuffer = graph.createBuffer(BufferDesc{.name = "Lights",
        .size = 64u * 1024u,
        .stride = 64u,
        .transient = true});
    resources.clusterCamera = graph.createBuffer(BufferDesc{.name = "Cluster camera",
        .size = 128u,
        .stride = 16u,
        .transient = true});
    resources.shadowConstants = graph.createBuffer(BufferDesc{.name = "Shadow constants",
        .size = 288u,
        .stride = 16u,
        .transient = true});

    // CSM is four independent depth scopes over one array resource.  Keeping
    // the four declarations in the backend-neutral graph makes the public
    // quality path describe the same execution topology as the Vulkan
    // recorder; there is no single-scope shadow compatibility pass.
    PassHandle lastShadowPass{};
    for (std::uint32_t cascade = 0; cascade < 4u; ++cascade)
    {
        auto shadow = graph.addPass("CSM shadows " + std::to_string(cascade));
        shadow.write(resources.shadowMap, ResourceUsage::DepthAttachment);
        // Carry the produced version into the next cascade so the graph's
        // version chain and lifetime metadata match the physical CSM array.
        resources.shadowMap = shadow;
        lastShadowPass = shadow;
    }
    auto gbuffer = graph.addPass("G-buffer");
    gbuffer.dependsOn(lastShadowPass);
    gbuffer.write(resources.gbufferAlbedo, ResourceUsage::ColorAttachment);
    resources.gbufferAlbedo = gbuffer;
    gbuffer.write(resources.gbufferNormalRoughness, ResourceUsage::ColorAttachment);
    resources.gbufferNormalRoughness = gbuffer;
    gbuffer.write(resources.gbufferMaterial, ResourceUsage::ColorAttachment);
    resources.gbufferMaterial = gbuffer;
    gbuffer.write(resources.motionVectors, ResourceUsage::ColorAttachment);
    resources.motionVectors = gbuffer;
    gbuffer.write(resources.sceneDepth, ResourceUsage::DepthAttachment);
    resources.sceneDepth = gbuffer;
    // Cluster construction is a mandatory GPU pass in M3.  The quality flag
    // controls the light-list policy in the pass implementation, never the
    // existence of the pass (and never a CPU fallback path).
    auto cluster = graph.addPass("Cluster Build");
    cluster.dependsOn(gbuffer);
    cluster.write(resources.clusterRanges, ResourceUsage::Storage);
    resources.clusterRanges = cluster;
    cluster.write(resources.clusterIndices, ResourceUsage::Storage);
    resources.clusterIndices = cluster;
    cluster.write(resources.clusterOverflow, ResourceUsage::Storage);
    resources.clusterOverflow = cluster;
    cluster.read(resources.lightBuffer, ResourceUsage::Storage)
        .read(resources.clusterCamera, ResourceUsage::Uniform)
        .setSideEffect();
    auto lighting = graph.addPass("Clustered deferred lighting");
    lighting.read(resources.gbufferAlbedo, ResourceUsage::Sampled)
        .read(resources.gbufferNormalRoughness, ResourceUsage::Sampled)
        .read(resources.gbufferMaterial, ResourceUsage::Sampled)
        .read(resources.sceneDepth, ResourceUsage::Sampled)
        .read(resources.shadowMap, ResourceUsage::Sampled)
        .read(resources.irradiance, ResourceUsage::Sampled)
        .read(resources.prefilteredEnvironment, ResourceUsage::Sampled)
        .read(resources.brdfLut, ResourceUsage::Sampled)
        .read(resources.clusterRanges, ResourceUsage::Storage)
        .read(resources.clusterIndices, ResourceUsage::Storage)
        .read(resources.lightBuffer, ResourceUsage::Storage)
        .read(resources.shadowConstants, ResourceUsage::Uniform)
        .write(resources.hdrColor,
            ResourceUsage::ColorAttachment | ResourceUsage::Storage);
    resources.hdrColor = lighting;
    if (config.enableTransparency)
    {
        auto transparency = graph.addPass("Forward transparency");
        transparency.read(resources.sceneDepth, ResourceUsage::DepthAttachment)
            .write(resources.hdrColor, ResourceUsage::ColorAttachment);
        resources.hdrColor = transparency;
    }
    // The compute resolve is always part of the graph.  Disabling TAA only
    // changes its history weight; it must not switch to a legacy copy path.
    auto taa = graph.addPass("TAA resolve");
    taa.read(resources.hdrColor, ResourceUsage::Sampled)
        .read(resources.motionVectors, ResourceUsage::Sampled)
        .read(resources.taaHistoryA, ResourceUsage::Sampled)
        .write(resources.taaHistoryB, ResourceUsage::Storage);
    resources.taaHistoryB = taa;
    resources.taaColor = resources.taaHistoryB;
    auto tonemap = graph.addPass("ACES tonemap");
    tonemap.read(resources.taaColor, ResourceUsage::Sampled)
        .write(resources.output, ResourceUsage::ColorAttachment)
        .setSideEffect();
    resources.output = tonemap;
    (void)config.enableIbl; // IBL is a lighting input, not a separate pass.
    return resources;
}

} // namespace Halcyon::Renderer::Quality
