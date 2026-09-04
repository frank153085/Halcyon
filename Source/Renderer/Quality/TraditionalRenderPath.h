#pragma once

#include "../Graph/FrameGraph.h"

#include <cstdint>

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
    Graph::TextureHandle hdrColor{};
    Graph::TextureHandle taaColor{};
    Graph::TextureHandle output{};
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
    resources.shadowMap = graph.createTexture(TextureDesc{.name = "CSM shadow map",
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers =
            static_cast<std::uint32_t>(config.cascadeCount == 0 ? 1 : config.cascadeCount),
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
    resources.hdrColor = graph.createTexture(TextureDesc{.name = "HDR lighting",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = true});
    resources.taaColor = graph.createTexture(TextureDesc{.name = "TAA history",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA16Float,
        .transient = false});
    resources.output = graph.createTexture(TextureDesc{.name = "Tonemapped output",
        .width = width,
        .height = height,
        .format = TextureFormat::RGBA8Unorm,
        .transient = false});

    auto shadow = graph.addPass("CSM shadows");
    shadow.write(resources.shadowMap, ResourceUsage::DepthAttachment);
    auto gbuffer = graph.addPass("G-buffer");
    gbuffer.write(resources.gbufferAlbedo, ResourceUsage::ColorAttachment)
        .write(resources.gbufferNormalRoughness, ResourceUsage::ColorAttachment)
        .write(resources.gbufferMaterial, ResourceUsage::ColorAttachment)
        .write(resources.motionVectors, ResourceUsage::ColorAttachment);
    auto lighting = graph.addPass(
        config.enableClusteredLighting ? "Clustered deferred lighting" : "Deferred lighting");
    lighting.read(resources.gbufferAlbedo, ResourceUsage::Sampled)
        .read(resources.gbufferNormalRoughness, ResourceUsage::Sampled)
        .read(resources.gbufferMaterial, ResourceUsage::Sampled)
        .read(resources.shadowMap, ResourceUsage::Sampled)
        .write(resources.hdrColor, ResourceUsage::Storage);
    if (config.enableTransparency)
    {
        auto transparency = graph.addPass("Forward transparency");
        transparency.read(resources.hdrColor, ResourceUsage::Sampled)
            .write(resources.hdrColor, ResourceUsage::ColorAttachment);
    }
    auto taa = graph.addPass(config.enableTaa ? "TAA resolve" : "Copy HDR");
    taa.read(resources.hdrColor, ResourceUsage::Sampled)
        .read(resources.motionVectors, ResourceUsage::Sampled)
        .read(resources.taaColor, ResourceUsage::Sampled)
        .write(resources.taaColor, ResourceUsage::Storage);
    auto tonemap = graph.addPass("ACES tonemap");
    tonemap.read(resources.taaColor, ResourceUsage::Sampled)
        .write(resources.output, ResourceUsage::ColorAttachment)
        .setSideEffect();
    (void)config.enableIbl; // IBL is a lighting input, not a separate pass.
    return resources;
}

} // namespace Halcyon::Renderer::Quality
