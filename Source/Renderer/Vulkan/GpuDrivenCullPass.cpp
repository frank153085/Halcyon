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

Halcyon::Result<void> recordGpuDrivenCulling(FramePassContext& ctx)
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

    if (!config.enableGpuDrivenScene || !gpuIndirectCompatible ||
        frustumCullPipeline.computePipeline() == VK_NULL_HANDLE)
        return ok();
    if (frameDescriptorPool == VK_NULL_HANDLE)
        return fail("per-frame descriptor pool is not initialized");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = frameDescriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &gpuSceneCullLayout;
    if (vkAllocateDescriptorSets(device, &alloc, &gpuCullSet) != VK_SUCCESS)
        return fail("failed to allocate GPU culling descriptor set");
    alloc.pSetLayouts = &gpuSceneIndirectLayout;
    if (vkAllocateDescriptorSets(device, &alloc, &gpuIndirectSet) != VK_SUCCESS)
        return fail("failed to allocate indirect descriptor set");
    alloc.pSetLayouts = &gpuSceneGraphicsLayout;
    if (vkAllocateDescriptorSets(device, &alloc, &gpuGraphicsSet) != VK_SUCCESS)
        return fail("failed to allocate GPU graphics descriptor set");
    if (config.enableTwoPhaseOcclusion &&
        vkAllocateDescriptorSets(device, &alloc, &gpuPhase2GraphicsSet) != VK_SUCCESS)
        return fail("failed to allocate phase-2 GPU graphics descriptor set");
    const VkDeviceSize sceneBytes = std::max<VkDeviceSize>(4,
        static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
    writeStorageBuffer(gpuCullSet, 0, gpuSceneBuffers.boundsBuffer(),
        std::max<VkDeviceSize>(4, static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
            sizeof(Halcyon::Renderer::Scene::BoundsRow)));
    writeStorageBuffer(gpuCullSet, 1, gpuSceneBuffers.visibleIndicesBuffer(), sceneBytes);
    writeStorageBuffer(gpuCullSet, 2, gpuSceneBuffers.visibleCountBuffer(), sizeof(std::uint32_t));
    writeStorageBuffer(gpuCullSet, 3, gpuSceneBuffers.meshMaterialBuffer(),
        static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
            sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
    writeStorageBuffer(gpuCullSet, 4, gpuSceneBuffers.occludedIndicesBuffer(), sceneBytes);
    writeStorageBuffer(gpuGraphicsSet, 0, gpuSceneBuffers.transformBuffer(),
        static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::TransformRow));
    writeStorageBuffer(gpuGraphicsSet, 1, gpuSceneBuffers.meshMaterialBuffer(),
        static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
    writeStorageBuffer(gpuGraphicsSet, 2, gpuSceneBuffers.groupedVisibleIndicesBuffer(), sceneBytes);
    if (gpuDrivenBindless && gpuMaterialCount != 0)
        writeStorageBuffer(gpuGraphicsSet, 3, gpuSceneBuffers.materialBuffer(),
            static_cast<VkDeviceSize>(gpuMaterialCount) *
                sizeof(Halcyon::Renderer::Scene::MaterialGpuData));
    if (gpuPhase2GraphicsSet != VK_NULL_HANDLE)
    {
        writeStorageBuffer(gpuPhase2GraphicsSet, 0, gpuSceneBuffers.transformBuffer(),
            static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                sizeof(Halcyon::Renderer::Scene::TransformRow));
        writeStorageBuffer(gpuPhase2GraphicsSet, 1, gpuSceneBuffers.meshMaterialBuffer(),
            static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
        writeStorageBuffer(gpuPhase2GraphicsSet, 2,
            gpuSceneBuffers.phase2GroupedVisibleIndicesBuffer(), sceneBytes);
        if (gpuDrivenBindless && gpuMaterialCount != 0)
            writeStorageBuffer(gpuPhase2GraphicsSet, 3, gpuSceneBuffers.materialBuffer(),
                static_cast<VkDeviceSize>(gpuMaterialCount) *
                    sizeof(Halcyon::Renderer::Scene::MaterialGpuData));
    }
    gpuSceneBuffers.writeIndirectBuildDescriptors(device, gpuIndirectSet, IndirectBuildPass::Main,
        gpuMeshDrawBuffer, static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
            sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
    vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.visibleCountBuffer(), 0,
        sizeof(std::uint32_t), 0);
    vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.meshHeadsBuffer(), 0,
        static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t), 0xffffffffu);
    vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.groupedVisibleCountBuffer(), 0,
        sizeof(std::uint32_t), 0);
    vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.indirectDrawCountBuffer(), 0,
        sizeof(std::uint32_t), 0);
    VkBufferMemoryBarrier2 resetBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    resetBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    resetBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    resetBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    resetBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    resetBarrier.buffer = gpuSceneBuffers.visibleCountBuffer();
    resetBarrier.size = sizeof(std::uint32_t);
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    std::array<VkBufferMemoryBarrier2, 4> resetBarriers{resetBarrier, resetBarrier,
        resetBarrier, resetBarrier};
    resetBarriers[1].buffer = gpuSceneBuffers.meshHeadsBuffer();
    resetBarriers[1].size = VK_WHOLE_SIZE;
    resetBarriers[2].buffer = gpuSceneBuffers.groupedVisibleCountBuffer();
    resetBarriers[3].buffer = gpuSceneBuffers.indirectDrawCountBuffer();
    dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(resetBarriers.size());
    dependency.pBufferMemoryBarriers = resetBarriers.data();
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
    writeGpuStageTimestamp(0, true);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        frustumCullPipeline.computePipeline());
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        frustumCullPipeline.layout(), 0, 1, &gpuCullSet, 0, nullptr);
    struct FrustumConstants
    {
        glm::vec4 planes[6];
        std::uint32_t instanceCount;
        std::uint32_t excludedFlags;
        std::uint32_t materialFilter;
        std::uint32_t reserved;
    } constants{};
    const glm::mat4& vp = packet.camera.viewProjection;
    const glm::vec4 rows[4] = {
        {vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
        {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
        {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
        {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
    constants.planes[0] = rows[3] + rows[0];
    constants.planes[1] = rows[3] - rows[0];
    constants.planes[2] = rows[3] + rows[1];
    constants.planes[3] = rows[3] - rows[1];
    constants.planes[4] = rows[3] + rows[2];
    constants.planes[5] = rows[3] - rows[2];
    for (auto& plane : constants.planes)
    {
        const float length = glm::length(glm::vec3(plane));
        if (length > 1.0e-6f) plane /= length;
    }
    constants.instanceCount = gpuSceneInstanceCount != 0 ? gpuSceneInstanceCount
        : static_cast<std::uint32_t>(packet.instances.size());
    constants.excludedFlags = gpuUnsupportedFlags;
    constants.materialFilter = gpuDrivenBindless
        ? std::numeric_limits<std::uint32_t>::max() : gpuMaterialId;
    vkCmdPushConstants(frame.commandBuffer, frustumCullPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(constants), &constants);
    vkCmdDispatch(frame.commandBuffer, (constants.instanceCount + 63u) / 64u, 1, 1);
    VkBufferMemoryBarrier2 cullBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    cullBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    cullBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    cullBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    cullBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    std::array<VkBufferMemoryBarrier2, 2> cullBarriers{cullBarrier, cullBarrier};
    cullBarriers[0].buffer = gpuSceneBuffers.visibleCountBuffer();
    cullBarriers[0].size = sizeof(std::uint32_t);
    cullBarriers[1].buffer = gpuSceneBuffers.visibleIndicesBuffer();
    cullBarriers[1].size = VK_WHOLE_SIZE;
    dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(cullBarriers.size());
    dependency.pBufferMemoryBarriers = cullBarriers.data();
    vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
    writeGpuStageTimestamp(0, false);
    if (!config.enableTwoPhaseOcclusion)
    {
        writeGpuStageTimestamp(1, true);
        MeshGroupedIndirectBuildDesc build{};
        build.set = gpuIndirectSet;
        build.instanceCount = constants.instanceCount;
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
    return ok();

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
