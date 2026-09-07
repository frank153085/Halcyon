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

void addGBufferPass(Graph::FrameGraph& graph, FramePassContext& ctx)
{
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

    auto& gbufferPass = graph.addPass<Graph::FrameGraph::Empty>("G-buffer",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            gbuffer0 = builder.write(gbuffer0, Graph::ResourceUsage::ColorAttachment);
            gbuffer1 = builder.write(gbuffer1, Graph::ResourceUsage::ColorAttachment);
            gbuffer2 = builder.write(gbuffer2, Graph::ResourceUsage::ColorAttachment);
            motion = builder.write(motion, Graph::ResourceUsage::ColorAttachment);
            instanceId = builder.write(instanceId, Graph::ResourceUsage::ColorAttachment);
            depth = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            if (config.enableTwoPhaseOcclusion)
                builder.read(hiz, Graph::ResourceUsage::Sampled);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = gbuffer0;
            descriptor.attachments.color[1] = gbuffer1;
            descriptor.attachments.color[2] = gbuffer2;
            descriptor.attachments.color[3] = motion;
            descriptor.attachments.color[4] = instanceId;
            descriptor.attachments.depth = depth;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::AllColors |
                                    Graph::FrameGraphAttachmentFlags::Depth;
            builder.declareRenderPass("G-buffer", descriptor);
            builder.sideEffect();
        },
        [&, hizPhase1Input](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            const auto info = resources.getRenderPassInfo(0);
             const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
             if (target == nullptr) return;

             // Phase 1 reclassifies the frustum-visible list using the
             // previous frame's Hi-Z.  The first frame has no valid history,
             // so it falls back to the regular visible list.
             if (config.enableTwoPhaseOcclusion && gpuIndirectCompatible &&
                 occlusionPhase1Pipeline.computePipeline() != VK_NULL_HANDLE &&
                 indirectBuildPipeline.computePipeline() != VK_NULL_HANDLE)
             {
                 const VkBuffer candidateCount = gpuSceneBuffers.visibleCountBuffer();
                 const VkBuffer phase1Count = gpuSceneBuffers.phase1VisibleCountBuffer();
                 const VkBuffer occludedCount = gpuSceneBuffers.occludedCountBuffer();
                 vkCmdFillBuffer(frame.commandBuffer, phase1Count, 0, sizeof(std::uint32_t), 0);
                 vkCmdFillBuffer(frame.commandBuffer, occludedCount, 0, sizeof(std::uint32_t), 0);
                 VkBufferMemoryBarrier2 reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                 reset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 reset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 reset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 reset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                 std::array<VkBufferMemoryBarrier2, 2> resets{reset, reset};
                 resets[0].buffer = phase1Count; resets[0].size = sizeof(std::uint32_t);
                 resets[1].buffer = occludedCount; resets[1].size = sizeof(std::uint32_t);
                 VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                 dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(resets.size());
                 dep.pBufferMemoryBarriers = resets.data();
                 vkCmdPipelineBarrier2(frame.commandBuffer, &dep);

                 const auto& hizResource = resources.getTexture(hizPhase1Input);
                 const VkImageView hizView = frameGraphProvider.mipView(hizResource.native, 0);
                 const VkDescriptorSet phaseSet = allocateSet(occlusionPhase1Layout);
                 if (phaseSet != VK_NULL_HANDLE && hasRenderedFrame && hizView != VK_NULL_HANDLE)
                 {
                     writeStorageBuffer(phaseSet, 0, gpuSceneBuffers.boundsBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::BoundsRow));
                     writeStorageBuffer(phaseSet, 1, gpuSceneBuffers.visibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 2, candidateCount, sizeof(std::uint32_t));
                     writeSampled(phaseSet, 3, frameGraphProvider.view(hizResource.native));
                     writeStorageBuffer(phaseSet, 5, gpuSceneBuffers.phase1VisibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 6, phase1Count, sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 7, gpuSceneBuffers.occludedIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 8, occludedCount, sizeof(std::uint32_t));
                     vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         occlusionPhase1Pipeline.computePipeline());
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         occlusionPhase1Pipeline.layout(), 0, 1, &phaseSet, 0, nullptr);
                     struct OcclusionConstants { glm::mat4 viewProjection; glm::uvec2 extent; std::uint32_t maxMip; float depthBias; } constants{};
                     constants.viewProjection = packet.camera.viewProjection;
                     constants.extent = {width, height};
                     constants.maxMip = hizResource.descriptor.mipLevels > 0 ? hizResource.descriptor.mipLevels - 1 : 0;
                     constants.depthBias = 0.001f;
                     vkCmdPushConstants(frame.commandBuffer, occlusionPhase1Pipeline.layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                     vkCmdDispatch(frame.commandBuffer,
                         (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                 }
                 else
                 {
                     // No history: phase1 list is the frustum list.
                     VkBufferMemoryBarrier2 copyBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                     copyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                     copyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                     copyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                     copyBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                     std::array<VkBufferMemoryBarrier2, 2> copyBarriers{copyBarrier, copyBarrier};
                     copyBarriers[0].buffer = gpuSceneBuffers.visibleIndicesBuffer(); copyBarriers[0].size = VK_WHOLE_SIZE;
                     copyBarriers[1].buffer = candidateCount; copyBarriers[1].size = sizeof(std::uint32_t);
                     dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = copyBarriers.data();
                     vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                     VkBufferCopy copy{0, 0, static_cast<VkDeviceSize>(gpuSceneInstanceCount) * sizeof(std::uint32_t)};
                     vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.visibleIndicesBuffer(),
                         gpuSceneBuffers.phase1VisibleIndicesBuffer(), 1, &copy);
                     VkBufferCopy countCopy{0, 0, sizeof(std::uint32_t)};
                     vkCmdCopyBuffer(frame.commandBuffer, candidateCount, phase1Count, 1, &countCopy);
                 }
                 VkBufferMemoryBarrier2 phaseBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                 phaseBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 phaseBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 phaseBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 phaseBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                 std::array<VkBufferMemoryBarrier2, 2> phaseBarriers{phaseBarrier, phaseBarrier};
                 phaseBarriers[0].buffer = phase1Count; phaseBarriers[0].size = sizeof(std::uint32_t);
                 phaseBarriers[1].buffer = gpuSceneBuffers.phase1VisibleIndicesBuffer(); phaseBarriers[1].size = VK_WHOLE_SIZE;
                 dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = phaseBarriers.data();
                 vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                 const VkDescriptorSet indirectSet = allocateSet(gpuSceneIndirectLayout);
                 if (indirectSet != VK_NULL_HANDLE)
                 {
                     writeGpuStageTimestamp(1, true);
                     gpuSceneBuffers.writeIndirectBuildDescriptors(device, indirectSet,
                         IndirectBuildPass::Phase1, gpuMeshDrawBuffer,
                         static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                             sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
                     MeshGroupedIndirectBuildDesc build{};
                     build.set = indirectSet;
                     build.instanceCount = gpuSceneInstanceCount;
                     build.meshCount = sceneResources.meshDrawCount();
                     build.meshHeads = gpuSceneBuffers.meshHeadsBuffer();
                     build.meshNext = gpuSceneBuffers.meshNextBuffer();
                     build.groupedVisible = gpuSceneBuffers.groupedVisibleIndicesBuffer();
                     build.groupedCount = gpuSceneBuffers.groupedVisibleCountBuffer();
                     build.indirectCommands = gpuSceneBuffers.indirectCommandsBuffer();
                     build.indirectCount = gpuSceneBuffers.indirectDrawCountBuffer();
                     frameRecorder.recordMeshGroupedIndirectBuild(
                         frame.commandBuffer, indirectBuildPipeline, build);
                     writeGpuStageTimestamp(1, false);
                 }
             }
              transitionImage(frameGraphProvider.image(target->resources[0]), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[1]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[2]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[3]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[4]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             // The scene depth target is transient and is materialized for
             // this graph execution. Its first use is always a discard/clear,
             // regardless of whether the renderer has submitted older frames.
             transitionImage(frameGraphProvider.image(target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
            std::array<VkRenderingAttachmentInfo, 5> colors{};
            for (std::size_t i = 0; i < colors.size(); ++i)
            {
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[i].imageView = target->views[i];
                  colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                 colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                 colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                 colors[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
             }
             colors[4].clearValue.color.uint32[0] = 0u;
            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = target->depthView;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                gbufferPipeline.pipeline());
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
             vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
             vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
             if (config.enableGpuDrivenScene && gpuIndirectCompatible &&
                 gpuDrivenGbufferPipeline.pipeline() != VK_NULL_HANDLE &&
                 gpuGraphicsSet != VK_NULL_HANDLE)
             {
                 vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     gpuDrivenGbufferPipeline.pipeline());
                 VkDescriptorSet sets[2] = {VK_NULL_HANDLE, gpuGraphicsSet};
                 if (gpuDrivenBindless)
                 {
                     sets[0] = bindlessTable.descriptorSet();
                 }
                 else
                 {
                     sets[0] = sceneResources.materialDescriptor(
                         packet.instances.front().materialId);
                 }
                 if (sets[0] != VK_NULL_HANDLE)
                 {
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         gpuDrivenGbufferPipeline.layout(), 0, 2, sets, 0, nullptr);
                     const struct GpuDrivenConstants
                     {
                         glm::mat4 viewProjection;
                         glm::mat4 previousViewProjection;
                     } constants{packet.camera.viewProjection,
                         previousPacketValid ? previousViewProjection : packet.camera.viewProjection};
                     vkCmdPushConstants(frame.commandBuffer, gpuDrivenGbufferPipeline.layout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
                     const VkDeviceSize vertexOffset = 0;
                     vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &gpuVertexBuffer, &vertexOffset);
                     vkCmdBindIndexBuffer(frame.commandBuffer, gpuIndexBuffer, 0,
                         VK_INDEX_TYPE_UINT32);
                     vkCmdDrawIndexedIndirectCount(frame.commandBuffer,
                         gpuSceneBuffers.indirectCommandsBuffer(), 0,
                         config.enableTwoPhaseOcclusion
                             ? gpuSceneBuffers.indirectDrawCountBuffer()
                             : gpuSceneBuffers.indirectDrawCountBuffer(), 0,
                         std::max<std::uint32_t>(1u, sceneResources.meshDrawCount()),
                         sizeof(VkDrawIndexedIndirectCommand));
                     // Transparent, double-sided, and non-bindless material
                     // mismatches stay on the existing CPU path, but they no
                     // longer force compatible opaque instances to leave the
                     // GPU-driven submission path.
                     frameRecorder.drawGpuDrivenCpuFallback(frame.commandBuffer, packet, gpuMaterialId, ctx.cpuDraw);
                 }
                 else
                 {
                     frameRecorder.drawOpaqueGBufferInstances(frame.commandBuffer, packet, ctx.cpuDraw);
                 }
             }
             else
             {
                 frameRecorder.drawOpaqueGBufferInstances(frame.commandBuffer, packet, ctx.cpuDraw);
             }
             vkCmdEndRendering(frame.commandBuffer);
             // Depth is consumed immediately by the Hi-Z compute pass. End
             // the render pass in the sampled layout so the next pass has a
             // single, explicit starting state on every frame.
             transitionImage(frameGraphProvider.image(target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]),
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
        });

    ctx.gbufferPassHandle = gbufferPass.handle();

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
