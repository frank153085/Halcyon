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

void recordVisibilityReadback(FramePassContext& ctx)
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

    if (currentFrame < gpuVisibilityValid.size())
        gpuVisibilityValid[currentFrame] = config.enableGpuDrivenScene && gpuIndirectCompatible;
    if (config.enableGpuDrivenScene && gpuIndirectCompatible &&
        currentFrame < gpuVisibilityReadbacks.size())
    {
        const VkBuffer visibleCount = config.enableTwoPhaseOcclusion
            ? gpuSceneBuffers.phase1VisibleCountBuffer()
            : gpuSceneBuffers.visibleCountBuffer();
        const VkBuffer phase2Count = gpuSceneBuffers.phase2VisibleCountBuffer();
        const VkBuffer readback = gpuVisibilityReadbacks[currentFrame].buffer;
        VkBufferMemoryBarrier2 sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        sourceBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        sourceBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        sourceBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        std::array<VkBufferMemoryBarrier2, 7> sourceBarriers{
            sourceBarrier, sourceBarrier, sourceBarrier, sourceBarrier, sourceBarrier,
            sourceBarrier, sourceBarrier};
        const VkBuffer firstIndices = config.enableTwoPhaseOcclusion
            ? gpuSceneBuffers.phase1VisibleIndicesBuffer()
            : gpuSceneBuffers.visibleIndicesBuffer();
        sourceBarriers[0].buffer = visibleCount; sourceBarriers[0].size = sizeof(std::uint32_t);
        sourceBarriers[1].buffer = phase2Count; sourceBarriers[1].size = sizeof(std::uint32_t);
        sourceBarriers[2].buffer = firstIndices; sourceBarriers[2].size = VK_WHOLE_SIZE;
        sourceBarriers[3].buffer = gpuSceneBuffers.phase2VisibleIndicesBuffer(); sourceBarriers[3].size = VK_WHOLE_SIZE;
        sourceBarriers[4].buffer = gpuSceneBuffers.visibleIndicesBuffer(); sourceBarriers[4].size = VK_WHOLE_SIZE;
        sourceBarriers[5].buffer = gpuSceneBuffers.indirectDrawCountBuffer(); sourceBarriers[5].size = sizeof(std::uint32_t);
        sourceBarriers[6].buffer = gpuSceneBuffers.phase2IndirectDrawCountBuffer(); sourceBarriers[6].size = sizeof(std::uint32_t);
        VkDependencyInfo sourceDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        sourceDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(sourceBarriers.size());
        sourceDependency.pBufferMemoryBarriers = sourceBarriers.data();
        vkCmdPipelineBarrier2(frame.commandBuffer, &sourceDependency);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(std::min<std::uint32_t>(
            gpuSceneBuffers.capacity(), VisibilityReadbackCapacity)) * sizeof(std::uint32_t);
        const VkBufferCopy frustumCountCopy{0, 0, sizeof(std::uint32_t)};
        const VkBufferCopy countCopy{0, sizeof(std::uint32_t), sizeof(std::uint32_t)};
        const VkBufferCopy firstIndicesCopy{0,
            sizeof(std::uint32_t) * VisibilityReadbackHeaderCount, indexBytes};
        const VkBufferCopy secondCountCopy{0, sizeof(std::uint32_t) * 2u,
            sizeof(std::uint32_t)};
        const VkBufferCopy indirectCountCopy{0, sizeof(std::uint32_t) * 3u,
            sizeof(std::uint32_t)};
        const VkBufferCopy phase2IndirectCountCopy{0, sizeof(std::uint32_t) * 4u,
            sizeof(std::uint32_t)};
        const VkBufferCopy secondIndicesCopy{0,
            sizeof(std::uint32_t) * (VisibilityReadbackHeaderCount + VisibilityReadbackCapacity),
            indexBytes};
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.visibleCountBuffer(), readback,
            1, &frustumCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, visibleCount, readback, 1, &countCopy);
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.indirectDrawCountBuffer(), readback,
            1, &indirectCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.phase2IndirectDrawCountBuffer(), readback,
            1, &phase2IndirectCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, firstIndices, readback, 1, &firstIndicesCopy);
        if (config.enableTwoPhaseOcclusion && hasRenderedFrame)
        {
            vkCmdCopyBuffer(frame.commandBuffer, phase2Count, readback, 1, &secondCountCopy);
            vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.phase2VisibleIndicesBuffer(),
                readback, 1, &secondIndicesCopy);
        }
        else
        {
            // Keep the phase-1 indirect command count at header slot 3;
            // only the phase-2 fields are absent in a frustum-only frame.
            vkCmdFillBuffer(frame.commandBuffer, readback, sizeof(std::uint32_t) * 2u,
                sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(frame.commandBuffer, readback, sizeof(std::uint32_t) * 4u,
                sizeof(std::uint32_t), 0);
        }
    }

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
