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

void addHiZOcclusionPass(Graph::FrameGraph& graph, FramePassContext& ctx)
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

    if (config.enableTwoPhaseOcclusion && hizBuildPipeline.computePipeline() != VK_NULL_HANDLE)
    {
        // Phase 2 performs a LOAD/STORE overlay into the G-buffer. Keep every
        // attachment in the pass data so FrameGraphResources can validate the
        // native accesses and carry the resulting versions to deferred lighting.
        struct HiZPassData
        {
            Graph::TextureHandle depth{};
            Graph::TextureHandle hiz{};
            Graph::TextureHandle gbuffer0{};
            Graph::TextureHandle gbuffer1{};
            Graph::TextureHandle gbuffer2{};
            Graph::TextureHandle motion{};
            Graph::TextureHandle instanceId{};
        };
        graph.addPass<HiZPassData>("Hi-Z build",
            [&](Graph::FrameGraph::Builder& builder, HiZPassData& data)
            {
                builder.readWrite(depth, Graph::ResourceUsage::DepthAttachment |
                                         Graph::ResourceUsage::Sampled);
                data.depth = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                data.hiz = builder.write(hiz, Graph::ResourceUsage::Storage |
                                               Graph::ResourceUsage::Sampled);
                builder.readWrite(gbuffer0, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer0 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(gbuffer1, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer1 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(gbuffer2, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer2 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(motion, Graph::ResourceUsage::ColorAttachment);
                data.motion = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(instanceId, Graph::ResourceUsage::ColorAttachment |
                                           Graph::ResourceUsage::TransferSource);
                data.instanceId = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                depth = data.depth;
                hiz = data.hiz;
                gbuffer0 = data.gbuffer0;
                gbuffer1 = data.gbuffer1;
                gbuffer2 = data.gbuffer2;
                motion = data.motion;
                instanceId = data.instanceId;
                builder.dependsOn(ctx.gbufferPassHandle);
                builder.sideEffect();
            },
            [passCtx](const Graph::FrameGraphResources& resources, const HiZPassData& data,
                Graph::CommandContext&)
            {
                HALCYON_BIND_PASS_EXECUTE(passCtx);
                const auto& depthResource = resources.getTexture(data.depth);
                const auto& hizResource = resources.getTexture(data.hiz);
                const VkImage depthImage = frameGraphProvider.image(depthResource.native);
                const VkImage hizImage = frameGraphProvider.image(hizResource.native);
                if (depthImage == VK_NULL_HANDLE || hizImage == VK_NULL_HANDLE) return;
                writeGpuStageTimestamp(2, true);
                transitionImage(hizImage, hasRenderedFrame ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                             : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL,
                    hasRenderedFrame ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                     : VK_PIPELINE_STAGE_2_NONE,
                    hasRenderedFrame ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
                const auto writeImage = [&](VkDescriptorSet set, std::uint32_t binding,
                                            VkDescriptorType type, VkImageView view,
                                            VkImageLayout layout)
                {
                    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, layout};
                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = set;
                    write.dstBinding = binding;
                    write.descriptorCount = 1;
                    write.descriptorType = type;
                    write.pImageInfo = &image;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                };
                const auto buildHiZPyramid = [&]()
                {
                    // Phase 2 records an indirect-build dispatch between the
                    // two pyramid builds. Rebind the Hi-Z pipeline every time
                    // this helper is invoked so its descriptor layout and the
                    // active compute pipeline can never diverge.
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        hizBuildPipeline.computePipeline());
                    std::uint32_t sourceWidth = depthResource.descriptor.width;
                    std::uint32_t sourceHeight = depthResource.descriptor.height;
                    for (std::uint32_t mip = 0; mip < hizResource.descriptor.mipLevels; ++mip)
                    {
                        const VkImageView sourceView = mip == 0
                            ? frameGraphProvider.view(depthResource.native)
                            : frameGraphProvider.mipView(hizResource.native, mip - 1);
                        const VkImageView outputView = frameGraphProvider.mipView(hizResource.native, mip);
                        if (sourceView == VK_NULL_HANDLE || outputView == VK_NULL_HANDLE) break;
                        // A descriptor set may not be updated after it has
                        // been referenced by a recorded command buffer. Each
                        // mip dispatch gets its own set instead of rewriting
                        // the previous mip's sampled/storage views.
                        const auto descriptor = allocateSet(hizLayout);
                        if (descriptor == VK_NULL_HANDLE) return;
                        writeImage(descriptor, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceView,
                            mip == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_GENERAL);
                        writeImage(descriptor, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, outputView,
                            VK_IMAGE_LAYOUT_GENERAL);
                        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            hizBuildPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
                        struct HiZConstants { std::uint32_t sourceWidth, sourceHeight,
                            outputWidth, outputHeight; } constants{sourceWidth, sourceHeight,
                            std::max(1u, (sourceWidth + 1u) / 2u),
                            std::max(1u, (sourceHeight + 1u) / 2u)};
                        vkCmdPushConstants(frame.commandBuffer, hizBuildPipeline.layout(),
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                        vkCmdDispatch(frame.commandBuffer, (constants.outputWidth + 7u) / 8u,
                            (constants.outputHeight + 7u) / 8u, 1);
                        VkImageMemoryBarrier2 mipBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        mipBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        mipBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                        mipBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        mipBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                            VK_ACCESS_2_SHADER_WRITE_BIT;
                        mipBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        mipBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        mipBarrier.image = hizImage;
                        mipBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
                        VkDependencyInfo mipDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        mipDependency.imageMemoryBarrierCount = 1;
                        mipDependency.pImageMemoryBarriers = &mipBarrier;
                        vkCmdPipelineBarrier2(frame.commandBuffer, &mipDependency);
                        sourceWidth = constants.outputWidth;
                        sourceHeight = constants.outputHeight;
                    }
                };
                buildHiZPyramid();
                transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                writeGpuStageTimestamp(2, false);

                if (!config.enableTwoPhaseOcclusion || !gpuIndirectCompatible ||
                    occlusionPhase2Pipeline.computePipeline() == VK_NULL_HANDLE ||
                    !hasRenderedFrame)
                    return;
                writeGpuStageTimestamp(3, true);
                const VkDescriptorSet phaseSet = allocateSet(occlusionPhase2Layout);
                if (phaseSet == VK_NULL_HANDLE) return;
                const VkBuffer phase2Count = gpuSceneBuffers.phase2VisibleCountBuffer();
                vkCmdFillBuffer(frame.commandBuffer, phase2Count, 0, sizeof(std::uint32_t), 0);
                VkBufferMemoryBarrier2 reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                reset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                reset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                reset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                reset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                reset.buffer = phase2Count; reset.size = sizeof(std::uint32_t);
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = 1; dep.pBufferMemoryBarriers = &reset;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                writeStorageBuffer(phaseSet, 0, gpuSceneBuffers.boundsBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::BoundsRow));
                writeStorageBuffer(phaseSet, 1, gpuSceneBuffers.occludedIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(phaseSet, 2, gpuSceneBuffers.occludedCountBuffer(), sizeof(std::uint32_t));
                writeSampled(phaseSet, 3, frameGraphProvider.view(hizResource.native));
                writeStorageBuffer(phaseSet, 5, gpuSceneBuffers.phase2VisibleIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(phaseSet, 6, phase2Count, sizeof(std::uint32_t));
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    occlusionPhase2Pipeline.computePipeline());
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    occlusionPhase2Pipeline.layout(), 0, 1, &phaseSet, 0, nullptr);
                struct OcclusionConstants { glm::mat4 viewProjection; glm::uvec2 extent; std::uint32_t maxMip; float depthBias; } constants{};
                constants.viewProjection = packet.camera.viewProjection;
                constants.extent = {width, height};
                constants.maxMip = hizResource.descriptor.mipLevels > 0 ? hizResource.descriptor.mipLevels - 1 : 0;
                constants.depthBias = 0.001f;
                vkCmdPushConstants(frame.commandBuffer, occlusionPhase2Pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(constants), &constants);
                vkCmdDispatch(frame.commandBuffer, (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                VkBufferMemoryBarrier2 phaseBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                phaseBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                phaseBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                phaseBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                phaseBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                std::array<VkBufferMemoryBarrier2, 3> phaseBarriers{phaseBarrier, phaseBarrier, phaseBarrier};
                phaseBarriers[0].buffer = phase2Count; phaseBarriers[0].size = sizeof(std::uint32_t);
                phaseBarriers[1].buffer = gpuSceneBuffers.occludedCountBuffer(); phaseBarriers[1].size = sizeof(std::uint32_t);
                phaseBarriers[2].buffer = gpuSceneBuffers.occludedIndicesBuffer(); phaseBarriers[2].size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(phaseBarriers.size()); dep.pBufferMemoryBarriers = phaseBarriers.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);

                const VkDescriptorSet indirectSet = allocateSet(gpuSceneIndirectLayout);
                if (indirectSet == VK_NULL_HANDLE) return;
                gpuSceneBuffers.writeIndirectBuildDescriptors(device, indirectSet,
                    IndirectBuildPass::Phase2, gpuMeshDrawBuffer,
                    static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                        sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
                VkBufferMemoryBarrier2 visibleBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                visibleBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                visibleBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                visibleBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                visibleBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                visibleBarrier.buffer = gpuSceneBuffers.phase2VisibleIndicesBuffer();
                visibleBarrier.size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &visibleBarrier;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                MeshGroupedIndirectBuildDesc build{};
                build.set = indirectSet;
                build.instanceCount = gpuSceneInstanceCount;
                build.meshCount = sceneResources.meshDrawCount();
                build.meshHeads = gpuSceneBuffers.meshHeadsBuffer();
                build.meshNext = gpuSceneBuffers.meshNextBuffer();
                build.groupedVisible = gpuSceneBuffers.phase2GroupedVisibleIndicesBuffer();
                build.groupedCount = gpuSceneBuffers.phase2GroupedVisibleCountBuffer();
                build.indirectCommands = gpuSceneBuffers.phase2IndirectCommandsBuffer();
                build.indirectCount = gpuSceneBuffers.phase2IndirectDrawCountBuffer();
                build.resetMeshHeads = true;
                build.meshHeadsBytes = static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                    sizeof(std::uint32_t);
                frameRecorder.recordMeshGroupedIndirectBuild(
                    frame.commandBuffer, indirectBuildPipeline, build);

                // Overlay the objects that became visible after the current
                // frame's Hi-Z was built, preserving all existing G-buffer data.
                const VkDescriptorSet graphicsSet = gpuPhase2GraphicsSet;
                if (graphicsSet == VK_NULL_HANDLE) return;
                transitionImage(frameGraphProvider.image(resources.getTexture(data.gbuffer0).native), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.depth).native), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.instanceId).native),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                VkRenderingAttachmentInfo colors[5]{};
                const Graph::FrameGraphNativeResource colorTokens[5] = {
                    resources.getTexture(data.gbuffer0).native, resources.getTexture(data.gbuffer1).native,
                    resources.getTexture(data.gbuffer2).native, resources.getTexture(data.motion).native,
                    resources.getTexture(data.instanceId).native};
                for (int i = 0; i < 5; ++i) { colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colors[i].imageView = frameGraphProvider.view(colorTokens[i]); colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
                VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                depthAttachment.imageView = frameGraphProvider.view(resources.getTexture(data.depth).native);
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                 VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = swapchainExtent;
                 rendering.layerCount = 1; rendering.colorAttachmentCount = 5; rendering.pColorAttachments = colors;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuDrivenGbufferPipeline.pipeline());
                VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
                VkRect2D scissor{{0, 0}, swapchainExtent}; vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport); vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
                VkDescriptorSet sets[2] = {gpuDrivenBindless ? bindlessTable.descriptorSet() : sceneResources.materialDescriptor(gpuMaterialId), graphicsSet};
                if (sets[0] != VK_NULL_HANDLE)
                {
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuDrivenGbufferPipeline.layout(), 0, 2, sets, 0, nullptr);
                    const struct GpuDrivenConstants { glm::mat4 viewProjection; glm::mat4 previousViewProjection; } drawConstants{
                        packet.camera.viewProjection, previousPacketValid ? previousViewProjection : packet.camera.viewProjection};
                    vkCmdPushConstants(frame.commandBuffer, gpuDrivenGbufferPipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(drawConstants), &drawConstants);
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &gpuVertexBuffer, &offset);
                    vkCmdBindIndexBuffer(frame.commandBuffer, gpuIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexedIndirectCount(frame.commandBuffer, gpuSceneBuffers.phase2IndirectCommandsBuffer(), 0,
                        gpuSceneBuffers.phase2IndirectDrawCountBuffer(), 0,
                        std::max(1u, sceneResources.meshDrawCount()), sizeof(VkDrawIndexedIndirectCommand));
                }
                vkCmdEndRendering(frame.commandBuffer);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.depth).native), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

                // Phase 2 changed depth. Rebuild the complete pyramid so the
                // next frame never consumes the phase-1-only intermediate.
                transitionImage(hizImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
                buildHiZPyramid();
                transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                writeGpuStageTimestamp(3, false);
            });
    }

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
