#include "FramePassContext.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4189)
#endif


#include "IndirectBuildDescriptorLayout.h"
#include "VulkanCommon.h"
#include "../Graph/FrameGraph.h"
#include "../Scene/Ecs/RenderableManager.h"
#include "../Scene/GpuScene.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;

void addDeferredLightingPass(Graph::FrameGraph& graph, FramePassContext& ctx)
{
    FramePassContext* const passCtx = &ctx;
    VkDevice device = ctx.device;
    VkDescriptorPool frameDescriptorPool = ctx.descriptorPool;
    constexpr std::uint32_t csmResolution = VulkanFrameResources::CsmResolution;
    constexpr std::uint32_t gpuUnsupportedFlags =
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent) |
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::DoubleSided) |
        Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
    auto& frame = *ctx.frame;
    const auto& packet = *ctx.packet;
    const auto& config = *ctx.config;
    auto& gpuSceneBuffers = *ctx.gpuSceneBuffers;
    auto& sceneResources = *ctx.sceneResources;
    auto& frameGraphProvider = *ctx.frameGraphProvider;
    auto& pipelines = *ctx.pipelines;
    auto& gpuAllocator = *ctx.gpuAllocator;
    auto& frameResources = *ctx.frameResources;
    auto& bindlessTable = *ctx.bindlessTable;
    auto& frameRecorder = *ctx.frameRecorder;
    auto& debugReadbacks = *ctx.debugReadbacks;
    auto& csmDepthPipeline = pipelines.csmDepthPipeline;
    auto& gbufferPipeline = pipelines.gbufferPipeline;
    auto& deferredLightingPipeline = pipelines.deferredLightingPipeline;
    auto& transparentPipeline = pipelines.transparentPipeline;
    auto& taaPipeline = pipelines.taaPipeline;
    auto& tonemapPipeline = pipelines.tonemapPipeline;
    auto& clusterBuildPipeline = pipelines.clusterBuildPipeline;
    auto& frustumCullPipeline = pipelines.frustumCullPipeline;
    auto& indirectBuildPipeline = pipelines.indirectBuildPipeline;
    auto& gpuDrivenGbufferPipeline = pipelines.gpuDrivenGbufferPipeline;
    auto& hizBuildPipeline = pipelines.hizBuildPipeline;
    auto& occlusionPhase1Pipeline = pipelines.occlusionPhase1Pipeline;
    auto& occlusionPhase2Pipeline = pipelines.occlusionPhase2Pipeline;
    auto& gpuSceneCullLayout = pipelines.gpuSceneCullLayout;
    auto& gpuSceneIndirectLayout = pipelines.gpuSceneIndirectLayout;
    auto& gpuSceneGraphicsLayout = pipelines.gpuSceneGraphicsLayout;
    auto& hizLayout = pipelines.hizLayout;
    auto& occlusionPhase1Layout = pipelines.occlusionPhase1Layout;
    auto& occlusionPhase2Layout = pipelines.occlusionPhase2Layout;
    auto& lightingLayout = pipelines.lightingLayout;
    auto& taaLayout = pipelines.taaLayout;
    auto& clusterLayout = pipelines.clusterLayout;
    auto& tonemapLayout = pipelines.tonemapLayout;
    auto& gpuDrivenBindless = ctx.gpuDrivenBindless;
    auto& gpuMaterialId = ctx.gpuMaterialId;
    auto& gpuIndirectCompatible = ctx.gpuIndirectCompatible;
    auto& gpuCullSet = ctx.gpuCullSet;
    auto& gpuIndirectSet = ctx.gpuIndirectSet;
    auto& gpuGraphicsSet = ctx.gpuGraphicsSet;
    auto& gpuPhase2GraphicsSet = ctx.gpuPhase2GraphicsSet;
    auto& gpuVertexBuffer = ctx.gpuVertexBuffer;
    auto& gpuIndexBuffer = ctx.gpuIndexBuffer;
    auto& gpuMeshDrawBuffer = ctx.gpuMeshDrawBuffer;
    auto& gpuSceneInstanceCount = ctx.gpuSceneInstanceCount;
    auto& gpuMaterialCount = ctx.gpuMaterialCount;
    auto& cascadeMatrices = ctx.cascadeMatrices;
    auto& cascadeSplits = ctx.cascadeSplits;
    auto& shadow = ctx.shadow;
    auto& gbuffer0 = ctx.gbuffer0;
    auto& gbuffer1 = ctx.gbuffer1;
    auto& gbuffer2 = ctx.gbuffer2;
    auto& motion = ctx.motion;
    auto& instanceId = ctx.instanceId;
    auto& depth = ctx.depth;
    auto& hiz = ctx.hiz;
    const auto hizPhase1Input = hiz;
    auto& hdr = ctx.hdr;
    auto& historyA = ctx.historyA;
    auto& historyB = ctx.historyB;
    auto& irradiance = ctx.irradiance;
    auto& prefiltered = ctx.prefiltered;
    auto& brdfLut = ctx.brdfLut;
    auto& clusterRanges = ctx.clusterRanges;
    auto& clusterIndices = ctx.clusterIndices;
    auto& clusterOverflow = ctx.clusterOverflow;
    auto& lightBuffer = ctx.lightBuffer;
    auto& clusterCamera = ctx.clusterCamera;
    auto& shadowConstantsBuffer = ctx.shadowConstants;
    auto& output = ctx.output;
    auto& tileCount = ctx.tileCount;
    const std::uint32_t width = ctx.width;
    const std::uint32_t height = ctx.height;
    const VkExtent2D swapchainExtent = ctx.swapchainExtent;
    const VkFormat swapchainFormat = ctx.swapchainFormat;
    const std::uint32_t currentFrame = ctx.currentFrame;
    const std::uint32_t imageIndex = ctx.imageIndex;
    const bool previousPacketValid = ctx.previousPacketValid;
    const bool hasRenderedFrame = ctx.hasRenderedFrame;
    const bool taaHistoryValid = ctx.taaHistoryValid;
    const glm::mat4 previousViewProjection = ctx.previousViewProjection;
    auto& clusterOverflowReadbacks = debugReadbacks.clusterOverflowReadbacks;
    auto& instanceIdReadbacks = debugReadbacks.instanceIdReadbacks;
    auto& instanceIdReadbackValid = debugReadbacks.instanceIdReadbackValid;
    auto& instanceIdReadbackFrameIndices = debugReadbacks.instanceIdReadbackFrameIndices;
    auto& gpuVisibilityReadbacks = debugReadbacks.gpuVisibilityReadbacks;
    auto& gpuVisibilityValid = debugReadbacks.gpuVisibilityValid;
    constexpr std::uint32_t VisibilityReadbackCapacity =
        DebugReadbackManager::VisibilityReadbackCapacity;
    constexpr std::uint32_t VisibilityReadbackHeaderCount =
        DebugReadbackManager::VisibilityReadbackHeaderCount;
    auto& screenshotReadback = ctx.screenshotReadback;
    auto& swapchainImages = *ctx.swapchainImages;
    struct ImportedTarget { VkImageView view = VK_NULL_HANDLE; } importedTarget{ctx.swapchainView};
    bool& iblInitialized = *ctx.iblInitialized;
    bool& taaHistoryFlip = *ctx.taaHistoryFlip;
    bool& taaHistoryInitializedA = *ctx.taaHistoryInitializedA;
    bool& taaHistoryInitializedB = *ctx.taaHistoryInitializedB;
    bool& fatalError = *ctx.fatalError;
    const auto allocateSet = [&](VkDescriptorSetLayout layout) { return ctx.allocateSet(layout); };
    const auto writeSampled = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view,
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        ctx.writeSampled(set, binding, view, imageLayout);
    };
    const auto writeSampler = [&](VkDescriptorSet set) { ctx.writeSampler(set); };
    const auto writeStorage = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view) {
        ctx.writeStorage(set, binding, view);
    };
    const auto writeStorageBuffer = [&](VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer,
        VkDeviceSize size) { ctx.writeStorageBuffer(set, binding, buffer, size); };
    const auto writeUniformBuffer = [&](VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer,
        VkDeviceSize size) { ctx.writeUniformBuffer(set, binding, buffer, size); };
    const auto writeGpuStageTimestamp = [&](std::uint32_t stage, bool begin) {
        ctx.writeGpuStageTimestamp(stage, begin);
    };
    const auto transitionImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        std::uint32_t baseLayer = 0, std::uint32_t layerCount = 1) {
        ctx.transitionImage(image, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess,
            aspect, baseLayer, layerCount);
    };
    const auto recordImageUpload = [&](VkCommandBuffer, VkImage image, auto data, auto copies) {
        return ctx.recordImageUpload(image, data, copies);
    };
    const auto setError = [&](std::string message) { ctx.setError(std::move(message)); };

    const auto deferredGbuffer0 = gbuffer0;
    const auto deferredGbuffer1 = gbuffer1;
    const auto deferredGbuffer2 = gbuffer2;
    const auto deferredDepth = depth;
    const auto deferredShadow = shadow;
    const auto deferredIrradiance = irradiance;
    const auto deferredPrefiltered = prefiltered;
    const auto deferredBrdf = brdfLut;
    const auto deferredClusterRanges = clusterRanges;
    const auto deferredClusterIndices = clusterIndices;
    const auto deferredLightBuffer = lightBuffer;
    const auto deferredShadowConstants = shadowConstantsBuffer;
    graph.addPass<Graph::FrameGraph::Empty>("Clustered deferred lighting",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(gbuffer0, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer1, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer2, Graph::ResourceUsage::Sampled);
            builder.read(depth, Graph::ResourceUsage::Sampled);
            builder.read(shadow, Graph::ResourceUsage::Sampled);
            builder.read(irradiance, Graph::ResourceUsage::Sampled);
            builder.read(prefiltered, Graph::ResourceUsage::Sampled);
            builder.read(brdfLut, Graph::ResourceUsage::Sampled);
            builder.read(clusterRanges, Graph::ResourceUsage::Storage);
            builder.read(clusterIndices, Graph::ResourceUsage::Storage);
            builder.read(lightBuffer, Graph::ResourceUsage::Storage);
            builder.read(shadowConstantsBuffer, Graph::ResourceUsage::Uniform);
            // HDR is both a render target and a legal storage image.  The
            // latter is part of the frame resource contract even though the
            // current deferred implementation writes it through dynamic
            // rendering; keeping the capability in the graph prevents a
            // later compute pass from silently requiring a reallocation.
            hdr = builder.write(hdr, Graph::ResourceUsage::ColorAttachment |
                                     Graph::ResourceUsage::Storage);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = hdr;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
            builder.declareRenderPass("Clustered deferred lighting", descriptor);
            builder.sideEffect();
        },
        [passCtx, hdr, deferredGbuffer0, deferredGbuffer1, deferredGbuffer2, deferredDepth, deferredShadow,
            deferredIrradiance, deferredPrefiltered, deferredBrdf, deferredClusterRanges,
            deferredClusterIndices, deferredLightBuffer, deferredShadowConstants](
            const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
            Graph::CommandContext&)
        {
            HALCYON_BIND_PASS_EXECUTE(passCtx);
            const auto info = resources.getRenderPassInfo(0);
            const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
            if (target == nullptr) return;
             transitionImage(frameGraphProvider.image(target->resources[0]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer0).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer1).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer2).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
             transitionImage(frameGraphProvider.image(resources.getTexture(deferredDepth).native),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!iblInitialized)
            {
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                static const ProceduralIblData proceduralIbl = createProceduralIbl();
                const VoidResult irradianceUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    proceduralIbl.irradiance, proceduralIbl.irradianceCopies);
                const VoidResult prefilteredUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    proceduralIbl.prefiltered, proceduralIbl.prefilteredCopies);
                const VoidResult brdfUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    proceduralIbl.brdf, proceduralIbl.brdfCopies);
                if (!irradianceUpload || !prefilteredUpload || !brdfUpload)
                {
                    const auto& error = !irradianceUpload ? irradianceUpload.error()
                        : !prefilteredUpload ? prefilteredUpload.error()
                                             : brdfUpload.error();
                    setError(error.describe());
                    fatalError = true;
                    return;
                }
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                iblInitialized = true;
            }
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = target->views[0];
             color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                deferredLightingPipeline.pipeline());
            const VkDescriptorSet descriptor = allocateSet(lightingLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(deferredGbuffer0).native));
                writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(deferredGbuffer1).native));
                writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(deferredGbuffer2).native));
                writeSampled(descriptor, 3, frameGraphProvider.view(resources.getTexture(deferredDepth).native));
                writeSampled(descriptor, 4, frameGraphProvider.view(resources.getTexture(deferredShadow).native));
                writeSampled(descriptor, 5, frameGraphProvider.view(resources.getTexture(deferredIrradiance).native));
                 writeSampled(descriptor, 6, frameGraphProvider.view(resources.getTexture(deferredPrefiltered).native));
                 writeSampled(descriptor, 7, frameGraphProvider.view(resources.getTexture(deferredBrdf).native));
                const auto& ranges = resources.get<Graph::FrameGraphBuffer>(deferredClusterRanges);
                const auto& indices = resources.get<Graph::FrameGraphBuffer>(deferredClusterIndices);
                const auto& lights = resources.get<Graph::FrameGraphBuffer>(deferredLightBuffer);
                const auto& shadowData = resources.get<Graph::FrameGraphBuffer>(deferredShadowConstants);
                if (!packet.lights.empty())
                {
                    if (const auto* allocation =
                            frameGraphProvider.nativeBufferAllocation(lights.native);
                        allocation != nullptr)
                    {
                        const auto bytes = std::span<const std::byte>{
                            reinterpret_cast<const std::byte*>(packet.lights.data()),
                            packet.lights.size_bytes()};
                        (void)gpuAllocator.writeBuffer(*allocation, bytes);
                    }
                }
                writeStorageBuffer(descriptor, 20, frameGraphProvider.buffer(ranges.native), ranges.descriptor.size);
                writeStorageBuffer(descriptor, 21, frameGraphProvider.buffer(indices.native), indices.descriptor.size);
                writeStorageBuffer(descriptor, 22, frameGraphProvider.buffer(lights.native), lights.descriptor.size);
                writeUniformBuffer(descriptor, 23, frameGraphProvider.buffer(shadowData.native),
                    shadowData.descriptor.size);
                struct alignas(16) ShadowData
                {
                    std::array<glm::mat4, 4> lightViewProjection;
                    glm::vec4 splits;
                    glm::vec4 params;
                } shadowUpload{cascadeMatrices, cascadeSplits,
                    glm::vec4{1.0f / static_cast<float>(csmResolution), 0.00035f, 0.0015f, 0.0f}};
                if (const auto* allocation =
                        frameGraphProvider.nativeBufferAllocation(shadowData.native);
                    allocation != nullptr)
                {
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(&shadowUpload), sizeof(shadowUpload)};
                    (void)gpuAllocator.writeBuffer(*allocation, bytes);
                }
                writeSampler(descriptor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    deferredLightingPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
            struct DeferredConstants
            {
                glm::mat4 inverseViewProjection;
                glm::vec4 cameraPosition;
                glm::vec4 viewportAndInvViewport;
                glm::uvec4 clusterParams;
                glm::vec4 depthParams;
            } constants{};
            constants.inverseViewProjection = packet.camera.inverseViewProjection;
            constants.cameraPosition = packet.camera.positionAndNear;
            constants.viewportAndInvViewport = packet.camera.viewportAndInvViewport;
            constants.clusterParams = glm::uvec4{
                frameResources.tilesX(),
                frameResources.tilesY(),
                VulkanFrameResources::ClusterSlices,
                static_cast<std::uint32_t>(packet.lights.size())};
            constants.depthParams = glm::vec4{
                std::max(1.0e-4f, packet.camera.positionAndNear.w),
                packet.camera.forwardAndFar.w > packet.camera.positionAndNear.w
                    ? packet.camera.forwardAndFar.w : 1000.0f,
                1.0f, 0.0f};
            vkCmdPushConstants(frame.commandBuffer, deferredLightingPipeline.layout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(frame.commandBuffer);
        });

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
