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

void addTaaResolvePass(Graph::FrameGraph& graph, FramePassContext& ctx)
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

    auto& historyRead = taaHistoryFlip ? historyB : historyA;
    auto& historyWrite = taaHistoryFlip ? historyA : historyB;
    const bool frameHistoryFlip = taaHistoryFlip;
    const auto taaHdr = hdr;
    const auto taaMotion = motion;
    const auto taaHistoryRead = historyRead;
    graph.addPass<Graph::FrameGraph::Empty>("TAA resolve",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(hdr, Graph::ResourceUsage::Sampled);
            builder.read(motion, Graph::ResourceUsage::Sampled);
            builder.read(historyRead, Graph::ResourceUsage::Sampled);
            historyWrite = builder.write(historyWrite, Graph::ResourceUsage::Storage);
            builder.sideEffect();
        },
        [passCtx, taaHdr, taaMotion, taaHistoryRead](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            HALCYON_BIND_PASS_EXECUTE(passCtx);
            auto& historyWrite = taaHistoryFlip ? historyA : historyB;
            const bool frameHistoryFlip = taaHistoryFlip;
            const bool virtualHdr = config.renderPath == Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed;
            const bool virtualMotion = virtualHdr;
            // Virtual shading may either dispatch a storage-image write or
            // clear the target through TRANSFER when a descriptor/resource
            // setup fails. Include both producer classes in the acquire
            // barrier so fallback clears are synchronized as well.
            transitionImage(frameGraphProvider.image(resources.getTexture(taaHdr).native),
                virtualHdr ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                virtualHdr ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                virtualHdr ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                    VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(taaMotion).native),
                virtualMotion ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                virtualMotion ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                virtualMotion ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                    VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            const VkImage historyReadImage = frameGraphProvider.image(resources.getTexture(taaHistoryRead).native);
            const VkImage historyWriteImage = frameGraphProvider.image(resources.getTexture(historyWrite).native);
             const bool readInitialized = frameHistoryFlip ? taaHistoryInitializedB : taaHistoryInitializedA;
             const bool writeInitialized = frameHistoryFlip ? taaHistoryInitializedA : taaHistoryInitializedB;
            transitionImage(historyReadImage,
                readInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(historyWriteImage,
                writeInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL,
                writeInitialized
                    ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                    : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                writeInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    taaPipeline.computePipeline());
                const VkDescriptorSet descriptor = allocateSet(taaLayout);
                const bool descriptorReady = descriptor != VK_NULL_HANDLE;
                if (descriptorReady)
                {
                    writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(taaHdr).native));
                    writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(taaHistoryRead).native));
                    writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(taaMotion).native));
                    writeSampler(descriptor);
                    writeStorage(descriptor, 20, frameGraphProvider.view(resources.getTexture(historyWrite).native));
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        taaPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
                }
                struct TaaConstants { std::uint32_t extent[2]; float historyWeight; float sharpen; } constants{{width, height},
                    (config.enableTaa && taaHistoryValid) ? 0.9f : 0.0f,
                    config.enableTaa ? 0.05f : 0.0f};
                if (descriptorReady)
                {
                    vkCmdPushConstants(frame.commandBuffer, taaPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(constants), &constants);
                    vkCmdDispatch(frame.commandBuffer, (width + 7u) / 8u, (height + 7u) / 8u, 1);
                }
                else
                {
                    const VkImage historyImage = frameGraphProvider.image(resources.getTexture(historyWrite).native);
                    const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
                    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(frame.commandBuffer, historyImage,
                        VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
                }
                transitionImage(historyWriteImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    descriptorReady ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                     : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    descriptorReady ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                     : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (frameHistoryFlip) taaHistoryInitializedA = true;
                else taaHistoryInitializedB = true;
            }
        });

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
