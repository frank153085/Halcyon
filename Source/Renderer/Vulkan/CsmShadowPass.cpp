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

void addCsmShadowPasses(Graph::FrameGraph& graph, FramePassContext& ctx)
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

    auto addDepthPass = [&](const char* name, std::uint32_t cascade)
    {
        graph.addPass<Graph::FrameGraph::Empty>(name,
            [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
            {
                shadow = builder.write(shadow, Graph::ResourceUsage::DepthAttachment);
                Graph::FrameGraphRenderPass::Descriptor descriptor{};
                descriptor.attachments.depth = shadow;
                descriptor.viewport.width = csmResolution;
                descriptor.viewport.height = csmResolution;
                descriptor.layerCount = 1;
                descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Depth;
                builder.declareRenderPass(name, descriptor);
                builder.sideEffect();
            },
            [passCtx, cascade](const Graph::FrameGraphResources& resources,
                          const Graph::FrameGraph::Empty&, Graph::CommandContext&)
            {
                HALCYON_BIND_PASS_EXECUTE(passCtx);
                constexpr std::uint32_t csmResolution = VulkanFrameResources::CsmResolution;
                const auto info = resources.getRenderPassInfo(0);
                const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
                const VkImageView view = target != nullptr
                                              ? frameGraphProvider.layerView(
                                                    target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS], cascade)
                                              : VK_NULL_HANDLE;
                 if (view == VK_NULL_HANDLE) return;
                 const VkImage shadowImage = frameGraphProvider.image(
                     target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]);
                 const bool gpuCsm = config.enableGpuDrivenScene && gpuIndirectCompatible &&
                     pipelines.gpuDrivenCsmPipeline.pipeline() != VK_NULL_HANDLE &&
                     gpuVertexBuffer != VK_NULL_HANDLE && gpuIndexBuffer != VK_NULL_HANDLE &&
                     gpuMeshDrawBuffer != VK_NULL_HANDLE &&
                     pipelines.frustumCullPipeline.computePipeline() != VK_NULL_HANDLE &&
                     pipelines.indirectBuildPipeline.computePipeline() != VK_NULL_HANDLE;
                 bool gpuCsmReady = false;
                 if (gpuCsm)
                 {
                     if (cascade == 0)
                     {
                         VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                         alloc.descriptorPool = ctx.descriptorPool;
                         alloc.descriptorSetCount = 1;
                         alloc.pSetLayouts = &pipelines.gpuSceneCullLayout;
                         if (vkAllocateDescriptorSets(device, &alloc, &ctx.gpuCsmCullSet) != VK_SUCCESS)
                             ctx.gpuCsmCullSet = VK_NULL_HANDLE;
                         alloc.pSetLayouts = &pipelines.gpuSceneIndirectLayout;
                         if (vkAllocateDescriptorSets(device, &alloc, &ctx.gpuCsmIndirectSet) != VK_SUCCESS)
                             ctx.gpuCsmIndirectSet = VK_NULL_HANDLE;
                         alloc.pSetLayouts = &pipelines.gpuCsmGraphicsLayout;
                         if (vkAllocateDescriptorSets(device, &alloc, &ctx.gpuCsmGraphicsSet) != VK_SUCCESS)
                             ctx.gpuCsmGraphicsSet = VK_NULL_HANDLE;
                         if (ctx.gpuCsmCullSet != VK_NULL_HANDLE)
                         {
                             const VkDeviceSize sceneBytes = std::max<VkDeviceSize>(4,
                                 static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                             writeStorageBuffer(ctx.gpuCsmCullSet, 0, gpuSceneBuffers.boundsBuffer(),
                                 std::max<VkDeviceSize>(4, static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                                     sizeof(Halcyon::Renderer::Scene::BoundsRow)));
                             writeStorageBuffer(ctx.gpuCsmCullSet, 1, gpuSceneBuffers.shadowVisibleIndicesBuffer(), sceneBytes);
                             writeStorageBuffer(ctx.gpuCsmCullSet, 2, gpuSceneBuffers.shadowVisibleCountBuffer(), sizeof(std::uint32_t));
                             writeStorageBuffer(ctx.gpuCsmCullSet, 3, gpuSceneBuffers.meshMaterialBuffer(),
                                 static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                                     sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
                             writeStorageBuffer(ctx.gpuCsmCullSet, 4, gpuSceneBuffers.shadowFrustum().occludedIndicesBuffer(), sceneBytes);
                         }
                         if (ctx.gpuCsmIndirectSet != VK_NULL_HANDLE)
                         {
                             gpuSceneBuffers.writeIndirectBuildDescriptors(device, ctx.gpuCsmIndirectSet,
                                 IndirectBuildPass::Shadow, gpuMeshDrawBuffer,
                                 static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                                     sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
                         }
                         if (ctx.gpuCsmGraphicsSet != VK_NULL_HANDLE)
                         {
                             const VkDeviceSize sceneBytes = std::max<VkDeviceSize>(4,
                                 static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                             writeStorageBuffer(ctx.gpuCsmGraphicsSet, 0, gpuSceneBuffers.transformBuffer(),
                                 static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                                     sizeof(Halcyon::Renderer::Scene::TransformRow));
                             const VkBuffer visible = sceneResources.meshDrawCount() <= 1u
                                 ? gpuSceneBuffers.shadowVisibleIndicesBuffer()
                                 : gpuSceneBuffers.shadowGroupedVisibleIndicesBuffer();
                             writeStorageBuffer(ctx.gpuCsmGraphicsSet, 1, visible, sceneBytes);
                         }
                     }
                     gpuCsmReady = ctx.gpuCsmCullSet != VK_NULL_HANDLE &&
                         ctx.gpuCsmIndirectSet != VK_NULL_HANDLE &&
                         ctx.gpuCsmGraphicsSet != VK_NULL_HANDLE;
                     if (gpuCsmReady)
                     {
                         vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.shadowVisibleCountBuffer(),
                             0, sizeof(std::uint32_t), 0);
                         VkBufferMemoryBarrier2 reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                         reset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                         reset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                         reset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                         reset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                         reset.buffer = gpuSceneBuffers.shadowVisibleCountBuffer();
                         reset.size = sizeof(std::uint32_t);
                         VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                         dependency.bufferMemoryBarrierCount = 1;
                         dependency.pBufferMemoryBarriers = &reset;
                         vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);

                         vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipelines.frustumCullPipeline.computePipeline());
                         vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipelines.frustumCullPipeline.layout(), 0, 1, &ctx.gpuCsmCullSet, 0, nullptr);
                         struct FrustumConstants
                         {
                             glm::vec4 planes[6];
                             std::uint32_t instanceCount;
                             std::uint32_t excludedFlags;
                             std::uint32_t materialFilter;
                             std::uint32_t requiredFlags;
                         } cullConstants{};
                         const glm::mat4& vp = cascadeMatrices[cascade];
                         const glm::vec4 rows[4] = {
                             {vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
                             {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
                             {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
                             {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
                         cullConstants.planes[0] = rows[3] + rows[0];
                         cullConstants.planes[1] = rows[3] - rows[0];
                         cullConstants.planes[2] = rows[3] + rows[1];
                         cullConstants.planes[3] = rows[3] - rows[1];
                         cullConstants.planes[4] = rows[3] + rows[2];
                         cullConstants.planes[5] = rows[3] - rows[2];
                         for (auto& plane : cullConstants.planes)
                         {
                             const float length = glm::length(glm::vec3(plane));
                             if (length > 1.0e-6f) plane /= length;
                         }
                         cullConstants.instanceCount = gpuSceneInstanceCount != 0 ? gpuSceneInstanceCount
                             : static_cast<std::uint32_t>(packet.instances.size());
                         cullConstants.excludedFlags = static_cast<std::uint32_t>(
                             Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent);
                         cullConstants.materialFilter = std::numeric_limits<std::uint32_t>::max();
                         cullConstants.requiredFlags = static_cast<std::uint32_t>(
                             Halcyon::Renderer::Scene::Ecs::RenderableFlags::CastShadow);
                         vkCmdPushConstants(frame.commandBuffer, pipelines.frustumCullPipeline.layout(),
                             VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cullConstants), &cullConstants);
                         vkCmdDispatch(frame.commandBuffer, (cullConstants.instanceCount + 63u) / 64u, 1, 1);

                         VkBufferMemoryBarrier2 cullBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                         cullBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                         cullBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                         cullBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                         cullBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                         std::array<VkBufferMemoryBarrier2, 2> cullBarriers{cullBarrier, cullBarrier};
                         cullBarriers[0].buffer = gpuSceneBuffers.shadowVisibleCountBuffer();
                         cullBarriers[0].size = sizeof(std::uint32_t);
                         cullBarriers[1].buffer = gpuSceneBuffers.shadowVisibleIndicesBuffer();
                         cullBarriers[1].size = VK_WHOLE_SIZE;
                         dependency.bufferMemoryBarrierCount = 2;
                         dependency.pBufferMemoryBarriers = cullBarriers.data();
                         vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);

                         MeshGroupedIndirectBuildDesc build{};
                         build.set = ctx.gpuCsmIndirectSet;
                         build.instanceCount = cullConstants.instanceCount;
                         build.meshCount = sceneResources.meshDrawCount();
                         build.meshHeads = gpuSceneBuffers.shadowMeshHeadsBuffer();
                         build.meshNext = gpuSceneBuffers.shadowMeshNextBuffer();
                         build.groupedVisible = gpuSceneBuffers.shadowGroupedVisibleIndicesBuffer();
                         build.groupedCount = gpuSceneBuffers.shadowGroupedVisibleCountBuffer();
                         build.indirectCommands = gpuSceneBuffers.shadowIndirectCommandsBuffer();
                         build.indirectCount = gpuSceneBuffers.shadowIndirectDrawCountBuffer();
                         build.resetMeshHeads = true;
                         build.meshHeadsBytes = static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                             sizeof(std::uint32_t);
                         frameRecorder.recordMeshGroupedIndirectBuild(
                             frame.commandBuffer, pipelines.indirectBuildPipeline, build);
                     }
                 }
                 transitionImage(shadowImage, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, cascade, 1);
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = view;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depthAttachment.clearValue.depthStencil.depth = 0.0f;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = {csmResolution, csmResolution};
                rendering.layerCount = 1;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    csmDepthPipeline.pipeline());
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(csmResolution),
                    static_cast<float>(csmResolution), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {csmResolution, csmResolution}};
                 vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
                 vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
                 if (gpuCsmReady)
                 {
                     vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         pipelines.gpuDrivenCsmPipeline.pipeline());
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         pipelines.gpuDrivenCsmPipeline.layout(), 0, 1, &ctx.gpuCsmGraphicsSet, 0, nullptr);
                     vkCmdPushConstants(frame.commandBuffer, pipelines.gpuDrivenCsmPipeline.layout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &cascadeMatrices[cascade]);
                     const VkDeviceSize vertexOffset = 0;
                     vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &gpuVertexBuffer, &vertexOffset);
                     vkCmdBindIndexBuffer(frame.commandBuffer, gpuIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                     vkCmdDrawIndexedIndirectCount(frame.commandBuffer,
                         gpuSceneBuffers.shadowIndirectCommandsBuffer(), 0,
                         gpuSceneBuffers.shadowIndirectDrawCountBuffer(), 0,
                         std::max<std::uint32_t>(1u, sceneResources.meshDrawCount()),
                         sizeof(VkDrawIndexedIndirectCommand));
                 }
                 else
                 {
                     frameRecorder.drawShadowInstances(frame.commandBuffer, packet,
                         cascadeMatrices[cascade], ctx.cpuDraw);
                 }
                 vkCmdEndRendering(frame.commandBuffer);
                 if (cascade == 3)
                 {
                     transitionImage(shadowImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4);
                 }
            });
    };
    addDepthPass("CSM shadows 0", 0);
    addDepthPass("CSM shadows 1", 1);
    addDepthPass("CSM shadows 2", 2);
    addDepthPass("CSM shadows 3", 3);

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
