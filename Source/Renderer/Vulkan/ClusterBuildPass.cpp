#include "M3PassContext.h"
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

void addClusterBuildPass(Graph::FrameGraph& graph, M3PassContext& ctx)
{
    VkDevice device = ctx.device;
    VkDescriptorPool m3DescriptorPool = ctx.descriptorPool;
    constexpr std::uint32_t csmResolution = VulkanM3FrameResources::CsmResolution;
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
    auto& m3FrameResources = *ctx.m3FrameResources;
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
    auto& m3LightingLayout = pipelines.m3LightingLayout;
    auto& m3TaaLayout = pipelines.m3TaaLayout;
    auto& m3ClusterLayout = pipelines.m3ClusterLayout;
    auto& m3TonemapLayout = pipelines.m3TonemapLayout;
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

    {
        const auto clusterRangesInput = clusterRanges;
        const auto clusterIndicesInput = clusterIndices;
        const auto clusterOverflowInput = clusterOverflow;
        const auto lightBufferInput = lightBuffer;
        const auto clusterCameraInput = clusterCamera;
        graph.addPass<Graph::FrameGraph::Empty>("Cluster Build",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            clusterRanges = builder.write(clusterRanges, Graph::ResourceUsage::Storage);
            clusterIndices = builder.write(clusterIndices, Graph::ResourceUsage::Storage);
            clusterOverflow = builder.write(clusterOverflow, Graph::ResourceUsage::Storage);
            builder.read(lightBuffer, Graph::ResourceUsage::Storage);
            builder.read(clusterCamera, Graph::ResourceUsage::Uniform);
            builder.sideEffect();
        },
        [&, clusterRangesInput, clusterIndicesInput, clusterOverflowInput, lightBufferInput,
            clusterCameraInput](
            const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                clusterBuildPipeline.computePipeline());
            const VkDescriptorSet descriptor = allocateSet(m3ClusterLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                const auto& ranges = resources.get<Graph::FrameGraphBuffer>(clusterRangesInput);
                const auto& indices = resources.get<Graph::FrameGraphBuffer>(clusterIndicesInput);
                const auto& overflow = resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput);
                const auto& lights = resources.get<Graph::FrameGraphBuffer>(lightBufferInput);
                const auto& cameraBuffer = resources.get<Graph::FrameGraphBuffer>(clusterCameraInput);
                writeStorageBuffer(descriptor, 0, frameGraphProvider.buffer(ranges.native), ranges.descriptor.size);
                writeStorageBuffer(descriptor, 1, frameGraphProvider.buffer(indices.native), indices.descriptor.size);
                writeStorageBuffer(descriptor, 2, frameGraphProvider.buffer(overflow.native), overflow.descriptor.size);
                writeStorageBuffer(descriptor, 3, frameGraphProvider.buffer(lights.native), lights.descriptor.size);
                writeUniformBuffer(descriptor, 4, frameGraphProvider.buffer(cameraBuffer.native),
                    cameraBuffer.descriptor.size);
                if (!packet.lights.empty())
                {
                    const auto* allocation = frameGraphProvider.nativeBufferAllocation(lights.native);
                    if (allocation != nullptr)
                    {
                        const auto bytes = std::span<const std::byte>{
                            reinterpret_cast<const std::byte*>(packet.lights.data()),
                            packet.lights.size_bytes()};
                        (void)gpuAllocator.writeBuffer(*allocation, bytes);
                    }
                }
                struct alignas(16) ClusterCameraData
                {
                    glm::mat4 view;
                    glm::mat4 inverseProjection;
                } cameraData{packet.camera.view, glm::inverse(packet.camera.projection)};
                if (const auto* allocation =
                        frameGraphProvider.nativeBufferAllocation(cameraBuffer.native);
                    allocation != nullptr)
                {
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(&cameraData), sizeof(cameraData)};
                    (void)gpuAllocator.writeBuffer(*allocation, bytes);
                }
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clusterBuildPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            struct ClusterConstants
            {
                std::uint32_t clusterCount, lightCount, maxLightsPerCluster, tilesX;
                std::uint32_t tilesY, slicesZ, clusteredLighting, reserved;
                glm::vec4 depthRange;
            } constants{tileCount, static_cast<std::uint32_t>(packet.lights.size()),
                VulkanM3FrameResources::MaxLightsPerCluster,
                m3FrameResources.tilesX(), m3FrameResources.tilesY(),
                VulkanM3FrameResources::ClusterSlices,
                config.enableClusteredLighting ? 1u : 0u, 0u,
                glm::vec4{
                    std::max(1.0e-4f, packet.camera.positionAndNear.w),
                    packet.camera.forwardAndFar.w > packet.camera.positionAndNear.w
                    ? packet.camera.forwardAndFar.w : 1000.0f,
                    0.0f, 0.0f}};
            static_assert(sizeof(ClusterConstants) == 48);
            const VkBuffer overflowBuffer = frameGraphProvider.buffer(
                resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput).native);
            vkCmdFillBuffer(frame.commandBuffer, overflowBuffer, 0, VK_WHOLE_SIZE, 0u);
            VkBufferMemoryBarrier2 overflowBarrier{};
            overflowBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            overflowBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            overflowBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            overflowBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            overflowBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            overflowBarrier.buffer = overflowBuffer;
            overflowBarrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo overflowDependency{};
            overflowDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            overflowDependency.bufferMemoryBarrierCount = 1;
            overflowDependency.pBufferMemoryBarriers = &overflowBarrier;
            vkCmdPipelineBarrier2(frame.commandBuffer, &overflowDependency);
             vkCmdPushConstants(frame.commandBuffer, clusterBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                 0, sizeof(constants), &constants);
             vkCmdDispatch(frame.commandBuffer,
                 std::max(1u, (tileCount + VulkanM3FrameResources::ClusterBuildGroupSize - 1u) /
                     VulkanM3FrameResources::ClusterBuildGroupSize), 1, 1);
             if (currentFrame < clusterOverflowReadbacks.size())
             {
                 const VkBuffer readback = clusterOverflowReadbacks[currentFrame].buffer;
                 VkBufferMemoryBarrier2 toCopy{};
                 toCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 toCopy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 toCopy.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                 toCopy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                 toCopy.buffer = overflowBuffer;
                 toCopy.size = sizeof(std::uint32_t);
                 VkDependencyInfo copyDependency{};
                 copyDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                 copyDependency.bufferMemoryBarrierCount = 1;
                 copyDependency.pBufferMemoryBarriers = &toCopy;
                 vkCmdPipelineBarrier2(frame.commandBuffer, &copyDependency);
                 VkBufferCopy copy{0, 0, sizeof(std::uint32_t)};
                 vkCmdCopyBuffer(frame.commandBuffer, overflowBuffer, readback, 1, &copy);
                 VkBufferMemoryBarrier2 toHost{};
                 toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 toHost.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                 toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                 toHost.buffer = readback;
                 toHost.size = sizeof(std::uint32_t);
                 copyDependency.pBufferMemoryBarriers = &toHost;
                 vkCmdPipelineBarrier2(frame.commandBuffer, &copyDependency);
             }
             std::array<VkBufferMemoryBarrier2, 3> clusterReadBarriers{};
            const std::array<VkBuffer, 3> clusterBuffers = {
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterRangesInput).native),
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterIndicesInput).native),
                frameGraphProvider.buffer(resources.get<Graph::FrameGraphBuffer>(clusterOverflowInput).native)};
            for (std::size_t i = 0; i < clusterReadBarriers.size(); ++i)
            {
                 clusterReadBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                 clusterReadBarriers[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 clusterReadBarriers[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                clusterReadBarriers[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                clusterReadBarriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                 clusterReadBarriers[i].buffer = clusterBuffers[i];
                 clusterReadBarriers[i].size = VK_WHOLE_SIZE;
                 if (i == 2 && currentFrame < clusterOverflowReadbacks.size())
                 {
                     clusterReadBarriers[i].srcStageMask |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                     clusterReadBarriers[i].srcAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT;
                 }
            }
            VkDependencyInfo clusterDependency{};
            clusterDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            clusterDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(clusterReadBarriers.size());
            clusterDependency.pBufferMemoryBarriers = clusterReadBarriers.data();
            vkCmdPipelineBarrier2(frame.commandBuffer, &clusterDependency);
            });
    }

}

} // namespace Halcyon::Vulkan

#ifdef _MSC_VER
#pragma warning(pop)
#endif
