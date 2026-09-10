#include "VirtualGeometryPass.h"
#include "../Scene/Ecs/RenderableManager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <span>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Halcyon::Vulkan
{

namespace
{

constexpr std::uint32_t kVirtualGeometryUnsupportedFlags =
    static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent) |
    static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::DoubleSided) |
    static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::AlphaMasked) |
    Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;

struct VirtualGeometrySelection
{
    const Halcyon::Renderer::Scene::VirtualGeometryAsset* asset = nullptr;
    const Halcyon::Renderer::Scene::InstanceData* instance = nullptr;
    std::uint32_t meshId = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return asset != nullptr && instance != nullptr;
    }
};

// All M5 V1 passes bind one consolidated virtual asset table. Keep this
// validation in one place so a late packet mutation cannot make rasterization,
// classification, and shading disagree about meshlet/index-buffer ownership.
[[nodiscard]] VirtualGeometrySelection selectVirtualGeometry(
    const FramePassContext& ctx) noexcept
{
    VirtualGeometrySelection selection{};
    if (ctx.packet == nullptr || ctx.sceneResources == nullptr || ctx.packet->instances.empty())
        return selection;

    for (const auto& candidate : ctx.packet->instances)
    {
        if ((candidate.flags & kVirtualGeometryUnsupportedFlags) != 0u ||
            !ctx.sceneResources->virtualGeometryMaterialCompatible(candidate.materialId))
            return {};
        const auto* candidateAsset =
            ctx.sceneResources->virtualGeometryDense(candidate.meshId);
        if (candidateAsset == nullptr || !candidateAsset->hasUniformPrimitiveMaterial())
            return {};
        if (selection.asset == nullptr)
        {
            selection.asset = candidateAsset;
            selection.instance = &candidate;
            selection.meshId = candidate.meshId;
            continue;
        }
        if (candidate.meshId != selection.meshId || candidateAsset != selection.asset)
            return {};
    }
    return selection;
}

} // namespace

void addVirtualGeometryPasses(Graph::FrameGraph& graph, FramePassContext& ctx)
{
    if (ctx.config == nullptr ||
        (ctx.config->renderPath != Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed &&
         ctx.config->renderPath != Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader))
        return;
    auto* passCtx = &ctx;
    const auto previousHiZHandle = ctx.hiz;
    const bool meshShaderPath = ctx.config->renderPath ==
        Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryMeshShader;
    graph.addPass<Graph::FrameGraph::Empty>("M6 GPU LOD selection",
        [passCtx](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            passCtx->selectedLodNodes = builder.write(passCtx->selectedLodNodes,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            passCtx->selectedLodCount = builder.write(passCtx->selectedLodCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            passCtx->lodBalanceDepth = builder.write(passCtx->lodBalanceDepth,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            passCtx->virtualPageRequests = builder.write(passCtx->virtualPageRequests,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination |
                    Graph::ResourceUsage::TransferSource);
            passCtx->virtualPageRequestCount = builder.write(
                passCtx->virtualPageRequestCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination |
                    Graph::ResourceUsage::TransferSource);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            const auto selection = selectVirtualGeometry(ctx);
            if (!selection || ctx.pipelines == nullptr || ctx.sceneResources == nullptr ||
                ctx.frameGraphProvider == nullptr ||
                ctx.pipelines->lodSelectPipeline.computePipeline() == VK_NULL_HANDLE)
                return;
            const auto* asset = selection.asset;
            const auto* gpu = ctx.sceneResources->virtualGeometryBuffersDense(selection.meshId);
            if (gpu == nullptr || gpu->dagNodes.buffer == VK_NULL_HANDLE ||
                gpu->dagEdges.buffer == VK_NULL_HANDLE || gpu->lodStates.buffer == VK_NULL_HANDLE ||
                gpu->pageTable.buffer == VK_NULL_HANDLE ||
                gpu->nodePageRanges.buffer == VK_NULL_HANDLE ||
                gpu->pageDependencies.buffer == VK_NULL_HANDLE)
                return;
            const auto& selectedNodes = resources.get<Graph::FrameGraphBuffer>(ctx.selectedLodNodes);
            const auto& selectedCount = resources.get<Graph::FrameGraphBuffer>(ctx.selectedLodCount);
            const auto& balanceDepth = resources.get<Graph::FrameGraphBuffer>(ctx.lodBalanceDepth);
            const auto& pageRequests = resources.get<Graph::FrameGraphBuffer>(ctx.virtualPageRequests);
            const auto& pageRequestCount = resources.get<Graph::FrameGraphBuffer>(ctx.virtualPageRequestCount);
            const VkBuffer statesBuffer = gpu->lodStates.buffer;
            const VkBuffer selectedBuffer = ctx.frameGraphProvider->buffer(selectedNodes.native);
            const VkBuffer countBuffer = ctx.frameGraphProvider->buffer(selectedCount.native);
            const VkBuffer balanceBuffer = ctx.frameGraphProvider->buffer(balanceDepth.native);
            const VkBuffer pageRequestBuffer = ctx.frameGraphProvider->buffer(pageRequests.native);
            const VkBuffer pageRequestCountBuffer =
                ctx.frameGraphProvider->buffer(pageRequestCount.native);
            if (statesBuffer == VK_NULL_HANDLE || selectedBuffer == VK_NULL_HANDLE ||
                countBuffer == VK_NULL_HANDLE || balanceBuffer == VK_NULL_HANDLE ||
                pageRequestBuffer == VK_NULL_HANDLE || pageRequestCountBuffer == VK_NULL_HANDLE)
                return;
            vkCmdFillBuffer(ctx.commandBuffer(), selectedBuffer, 0, VK_WHOLE_SIZE, 0u);
            vkCmdFillBuffer(ctx.commandBuffer(), countBuffer, 0,
                sizeof(std::uint32_t) * 5u, 0u);
            vkCmdFillBuffer(ctx.commandBuffer(), balanceBuffer, 0, sizeof(std::uint32_t),
                0u);
            vkCmdFillBuffer(ctx.commandBuffer(), pageRequestBuffer, 0, VK_WHOLE_SIZE, 0u);
            vkCmdFillBuffer(ctx.commandBuffer(), pageRequestCountBuffer, 0,
                sizeof(std::uint32_t), 0u);
            VkBufferMemoryBarrier2 reset[5]{};
            for (auto& barrier : reset)
            {
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            }
            reset[0].buffer = selectedBuffer; reset[0].size = VK_WHOLE_SIZE;
            reset[1].buffer = countBuffer; reset[1].size = sizeof(std::uint32_t) * 5u;
            reset[2].buffer = balanceBuffer; reset[2].size = sizeof(std::uint32_t);
            reset[3].buffer = pageRequestBuffer; reset[3].size = VK_WHOLE_SIZE;
            reset[4].buffer = pageRequestCountBuffer; reset[4].size = sizeof(std::uint32_t);
            VkBufferMemoryBarrier2 stateBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            stateBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            stateBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            stateBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            stateBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            stateBarrier.buffer = statesBuffer;
            stateBarrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            std::array<VkBufferMemoryBarrier2, 6> barriers = {
                reset[0], reset[1], reset[2], reset[3], reset[4], stateBarrier};
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
            dependency.pBufferMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
            const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->lodSelectLayout);
            if (set == VK_NULL_HANDLE) return;
            ctx.writeStorageBuffer(set, 0, gpu->dagNodes.buffer,
                asset->dagNodes.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuDagNode));
            ctx.writeStorageBuffer(set, 1, gpu->dagEdges.buffer,
                std::max<VkDeviceSize>(sizeof(Halcyon::Renderer::Scene::VirtualGeometryDagEdge),
                    asset->dagEdges.size() * sizeof(Halcyon::Renderer::Scene::VirtualGeometryDagEdge)));
            ctx.writeStorageBuffer(set, 2, statesBuffer,
                asset->dagNodes.size() * sizeof(std::uint32_t) * 4u);
            ctx.writeStorageBuffer(set, 3, selectedBuffer, selectedNodes.descriptor.size);
            ctx.writeStorageBuffer(set, 4, countBuffer, sizeof(std::uint32_t) * 5u);
            ctx.writeStorageBuffer(set, 5, balanceBuffer, sizeof(std::uint32_t));
            ctx.writeStorageBuffer(set, 6, gpu->clusterAdjacencyOffsets.buffer,
                (asset->clusters.size() + 1u) * sizeof(std::uint32_t));
            ctx.writeStorageBuffer(set, 7, gpu->clusterAdjacencyIndices.buffer,
                std::max<VkDeviceSize>(sizeof(std::uint32_t),
                    gpu->clusterAdjacencyIndices.size));
            ctx.writeStorageBuffer(set, 8, gpu->pageTable.buffer,
                gpu->pageTable.size);
            ctx.writeStorageBuffer(set, 9, gpu->nodePageRanges.buffer,
                gpu->nodePageRanges.size);
            ctx.writeStorageBuffer(set, 10, gpu->pageDependencies.buffer,
                gpu->pageDependencies.size);
            ctx.writeStorageBuffer(set, 11, pageRequestBuffer,
                pageRequests.descriptor.size);
            ctx.writeStorageBuffer(set, 12, pageRequestCountBuffer,
                sizeof(std::uint32_t));
            const float projectionY = std::abs(ctx.packet->camera.projection[1][1]);
            struct alignas(16) LodFrame
            {
                glm::mat4 viewProjection;
                glm::vec4 cameraAndFov;
                glm::uvec4 counts;
                glm::uvec4 outputAndRoot;
                glm::vec4 thresholds;
            } frame{};
            frame.viewProjection = ctx.packet->camera.viewProjection;
            const glm::mat4 firstModel = glm::make_mat4(selection.instance->transform.data());
            const glm::vec4 cameraObject = glm::inverse(firstModel) *
                glm::vec4(glm::vec3(ctx.packet->camera.positionAndNear), 1.0f);
            frame.cameraAndFov = glm::vec4(glm::vec3(cameraObject),
                projectionY > 1.0e-6f ? 2.0f * std::atan(1.0f / projectionY) : glm::radians(60.0f));
            frame.counts = glm::uvec4(
                static_cast<std::uint32_t>(std::max(1.0f, ctx.packet->camera.viewportAndInvViewport.y)),
                static_cast<std::uint32_t>(asset->dagNodes.size()),
                static_cast<std::uint32_t>(asset->dagEdges.size()),
                static_cast<std::uint32_t>(asset->dagNodes.size()));
            frame.outputAndRoot = glm::uvec4(
                std::min<std::uint32_t>(VulkanFrameResources::MaxVirtualGeometryMeshlets,
                    static_cast<std::uint32_t>(asset->dagNodes.size())),
                gpu->virtualPageCount, 0u,
                static_cast<std::uint32_t>(ctx.packet->frameIndex));
            const float qualityScale = std::clamp(
                ctx.virtualGeometryQualityScale, 1.0f, 8.0f);
            frame.thresholds = glm::vec4(
                1.0f * qualityScale, 0.75f * qualityScale, 0.0f, 0.0f);
            static_assert(sizeof(LodFrame) == 128);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->lodSelectPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->lodSelectPipeline.layout(), 0, 1, &set, 0, nullptr);
            const std::uint32_t groupCount =
                (static_cast<std::uint32_t>(asset->dagNodes.size()) + 63u) / 64u;
            const auto dispatchPhase = [&](std::uint32_t phase)
            {
                frame.outputAndRoot.z = phase;
                vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->lodSelectPipeline.layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(frame), &frame);
                vkCmdDispatch(ctx.commandBuffer(), groupCount, 1u, 1u);
            };
            const auto phaseBarrier = [&]()
            {
                std::array<VkBufferMemoryBarrier2, 4> phaseBarriers{};
                for (auto& barrier : phaseBarriers)
                {
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
                }
                phaseBarriers[0].buffer = statesBuffer;
                phaseBarriers[0].size = VK_WHOLE_SIZE;
                phaseBarriers[1].buffer = balanceBuffer;
                phaseBarriers[1].size = sizeof(std::uint32_t);
                phaseBarriers[2].buffer = countBuffer;
                phaseBarriers[2].size = sizeof(std::uint32_t) * 5u;
                phaseBarriers[3].buffer = selectedBuffer;
                phaseBarriers[3].size = VK_WHOLE_SIZE;
                VkDependencyInfo phaseDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                phaseDependency.bufferMemoryBarrierCount =
                    static_cast<std::uint32_t>(phaseBarriers.size());
                phaseDependency.pBufferMemoryBarriers = phaseBarriers.data();
                vkCmdPipelineBarrier2(ctx.commandBuffer(), &phaseDependency);
            };
            dispatchPhase(0u);
            phaseBarrier();
            dispatchPhase(1u);
            phaseBarrier();
            std::uint32_t balanceIterations = 1u;
            for (const auto& node : asset->dagNodes)
                balanceIterations = std::max(balanceIterations, node.lodDepth + 1u);
            for (std::uint32_t iteration = 0u; iteration < balanceIterations; ++iteration)
            {
                dispatchPhase(2u);
                phaseBarrier();
            }
            dispatchPhase(3u);
            phaseBarrier();
            frame.outputAndRoot.z = 4u;
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->lodSelectPipeline.layout(),
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(frame), &frame);
            vkCmdDispatch(ctx.commandBuffer(), 1u, 1u, 1u);
            phaseBarrier();
            if (ctx.debugReadbacks != nullptr &&
                ctx.currentFrame < ctx.debugReadbacks->virtualPageRequestReadbacks.size() &&
                ctx.currentFrame < ctx.debugReadbacks->virtualPageRequestValid.size())
            {
                const VkBuffer readback =
                    ctx.debugReadbacks->virtualPageRequestReadbacks[ctx.currentFrame].buffer;
                const std::uint32_t requestCapacity = std::min<std::uint32_t>(
                    gpu->virtualPageCount,
                    DebugReadbackManager::VirtualPageRequestCapacity);
                if (readback != VK_NULL_HANDLE && requestCapacity != 0u)
                {
                    std::array<VkBufferMemoryBarrier2, 2> requestBarriers{};
                    for (auto& barrier : requestBarriers)
                    {
                        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                    }
                    requestBarriers[0].buffer = pageRequestCountBuffer;
                    requestBarriers[0].size = sizeof(std::uint32_t);
                    requestBarriers[1].buffer = pageRequestBuffer;
                    requestBarriers[1].size =
                        static_cast<VkDeviceSize>(requestCapacity) * sizeof(std::uint32_t);
                    VkDependencyInfo requestDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    requestDependency.bufferMemoryBarrierCount =
                        static_cast<std::uint32_t>(requestBarriers.size());
                    requestDependency.pBufferMemoryBarriers = requestBarriers.data();
                    vkCmdPipelineBarrier2(ctx.commandBuffer(), &requestDependency);
                    const VkBufferCopy countCopy{0u, 0u, sizeof(std::uint32_t)};
                    const VkBufferCopy pagesCopy{0u, sizeof(std::uint32_t),
                        static_cast<VkDeviceSize>(requestCapacity) * sizeof(std::uint32_t)};
                    vkCmdCopyBuffer(ctx.commandBuffer(), pageRequestCountBuffer,
                        readback, 1u, &countCopy);
                    vkCmdCopyBuffer(ctx.commandBuffer(), pageRequestBuffer,
                        readback, 1u, &pagesCopy);
                    ctx.debugReadbacks->virtualPageRequestValid[ctx.currentFrame] = true;
                    ctx.debugReadbacks->virtualPageRequestFrameIndices[ctx.currentFrame] =
                        ctx.packet->frameIndex;
                    ctx.debugReadbacks->virtualPageRequestPageCounts[ctx.currentFrame] =
                        gpu->virtualPageCount;
                    ctx.debugReadbacks->virtualPageRequestMeshIds[ctx.currentFrame] =
                        selection.meshId;
                }
            }
        });
    graph.addPass<Graph::FrameGraph::Empty>("M5 meshlet cull and indirect",
        [passCtx, previousHiZHandle](Graph::FrameGraph::Builder& builder,
            Graph::FrameGraph::Empty&)
        {
            passCtx->virtualTransforms = builder.write(passCtx->virtualTransforms,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Uniform);
            passCtx->virtualMeshMaterials = builder.write(passCtx->virtualMeshMaterials,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Uniform);
            passCtx->virtualCullFrame = builder.write(passCtx->virtualCullFrame,
                Graph::ResourceUsage::Uniform);
            builder.read(previousHiZHandle, Graph::ResourceUsage::Sampled);
            passCtx->visibleMeshlets = builder.write(passCtx->visibleMeshlets,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            passCtx->visibleMeshletCount = builder.write(passCtx->visibleMeshletCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            builder.read(passCtx->selectedLodNodes, Graph::ResourceUsage::Storage);
            builder.read(passCtx->selectedLodCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect);
            passCtx->meshletIndirect = builder.write(passCtx->meshletIndirect,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            passCtx->meshletIndirectCount = builder.write(passCtx->meshletIndirectCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            passCtx->meshletMeshIndirect = builder.write(passCtx->meshletMeshIndirect,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            passCtx->meshletMeshIndirectCount = builder.write(passCtx->meshletMeshIndirectCount,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect |
                    Graph::ResourceUsage::TransferDestination);
            builder.sideEffect();
        },
        [passCtx, meshShaderPath, previousHiZHandle](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.virtualVisibilityValid != nullptr)
                *ctx.virtualVisibilityValid = false;
            if (ctx.frameGraphProvider == nullptr || ctx.gpuAllocator == nullptr ||
                ctx.pipelines == nullptr || ctx.sceneResources == nullptr || ctx.packet == nullptr)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            const auto& visible = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshlets);
            const auto& visibleCount = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshletCount);
            const auto& commands = resources.get<Graph::FrameGraphBuffer>(ctx.meshletIndirect);
            const auto& indirectCount =
                resources.get<Graph::FrameGraphBuffer>(ctx.meshletIndirectCount);
            const auto& meshCommands = resources.get<Graph::FrameGraphBuffer>(ctx.meshletMeshIndirect);
            const auto& meshCommandCount = resources.get<Graph::FrameGraphBuffer>(ctx.meshletMeshIndirectCount);
            const auto& transformRows = resources.get<Graph::FrameGraphBuffer>(ctx.virtualTransforms);
            const auto& materialRows = resources.get<Graph::FrameGraphBuffer>(ctx.virtualMeshMaterials);
            const auto& cullFrame = resources.get<Graph::FrameGraphBuffer>(ctx.virtualCullFrame);
            const auto& previousHiZ = resources.getTexture(previousHiZHandle);
            const VkBuffer visibleBuffer = ctx.frameGraphProvider->buffer(visible.native);
            const VkBuffer visibleCountBuffer = ctx.frameGraphProvider->buffer(visibleCount.native);
            const VkBuffer commandBuffer = ctx.frameGraphProvider->buffer(commands.native);
            const VkBuffer indirectCountBuffer =
                ctx.frameGraphProvider->buffer(indirectCount.native);
            const VkBuffer meshCommandBuffer = ctx.frameGraphProvider->buffer(meshCommands.native);
            const VkBuffer meshCommandCountBuffer = ctx.frameGraphProvider->buffer(meshCommandCount.native);
            const auto* transformAllocation =
                ctx.frameGraphProvider->nativeBufferAllocation(transformRows.native);
            const auto* materialAllocation =
                ctx.frameGraphProvider->nativeBufferAllocation(materialRows.native);
            const auto* cullFrameAllocation =
                ctx.frameGraphProvider->nativeBufferAllocation(cullFrame.native);
            if (transformAllocation == nullptr || materialAllocation == nullptr ||
                cullFrameAllocation == nullptr ||
                ctx.packet->instances.size() > VulkanFrameResources::MaxVirtualGeometryInstances)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }

            std::array<Halcyon::Renderer::Scene::TransformRow,
                VulkanFrameResources::MaxVirtualGeometryInstances> transforms{};
            std::array<Halcyon::Renderer::Scene::MeshMaterialRow,
                VulkanFrameResources::MaxVirtualGeometryInstances> materials{};
            for (std::size_t index = 0; index < ctx.packet->instances.size(); ++index)
            {
                const auto& source = ctx.packet->instances[index];
                transforms[index].model = source.transform;
                materials[index] = {source.meshId, source.materialId, source.flags, 0u};
            }
            const auto transformBytes = std::as_bytes(std::span{transforms}.first(
                ctx.packet->instances.size()));
            const auto materialBytes = std::as_bytes(std::span{materials}.first(
                ctx.packet->instances.size()));
            const auto transformUpload = ctx.gpuAllocator->writeBuffer(
                *transformAllocation, transformBytes);
            const auto materialUpload = ctx.gpuAllocator->writeBuffer(
                *materialAllocation, materialBytes);
            struct alignas(16) CullFrameData
            {
                glm::mat4 viewProjection{1.0f};
                glm::uvec4 hiz{0u};
            } cullFrameData{};
            static_assert(sizeof(CullFrameData) == 80,
                "Virtual Geometry cull-frame ABI must match meshlet_cull.comp.hlsl");
            cullFrameData.viewProjection = ctx.packet->camera.viewProjection;
            cullFrameData.hiz = glm::uvec4{ctx.width, ctx.height,
                previousHiZ.descriptor.mipLevels > 0u
                    ? previousHiZ.descriptor.mipLevels - 1u : 0u,
                ctx.virtualHiZInitialized != nullptr && *ctx.virtualHiZInitialized ? 1u : 0u};
            const auto cullFrameUpload = ctx.gpuAllocator->writeBuffer(*cullFrameAllocation,
                std::as_bytes(std::span{&cullFrameData, 1u}));
            if (!transformUpload || !materialUpload || !cullFrameUpload)
            {
                ctx.setError("Virtual Geometry instance upload failed");
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            if (visibleBuffer == VK_NULL_HANDLE || visibleCountBuffer == VK_NULL_HANDLE ||
                commandBuffer == VK_NULL_HANDLE || indirectCountBuffer == VK_NULL_HANDLE ||
                meshCommandBuffer == VK_NULL_HANDLE || meshCommandCountBuffer == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }

            const auto selection = selectVirtualGeometry(ctx);
            const auto* asset = selection.asset;
            const std::uint32_t meshId = selection.meshId;
            if (!selection || asset->meshlets.empty() ||
                ctx.pipelines->meshletCullPipeline.computePipeline() == VK_NULL_HANDLE ||
                (!meshShaderPath && ctx.pipelines->meshletIndirectPipeline.computePipeline() == VK_NULL_HANDLE) ||
                (meshShaderPath && (ctx.pipelines->meshletMeshIndirectPipeline.computePipeline() == VK_NULL_HANDLE ||
                    ctx.pipelines->virtualGeometryMeshPipeline.pipeline() == VK_NULL_HANDLE)))
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }

            const std::uint32_t kVisibleCapacity = std::min(
                VulkanFrameResources::MaxVirtualGeometryMeshlets,
                meshShaderPath ? ctx.virtualMeshWorkGroupCapacity
                               : ctx.virtualIndirectDrawCapacity);
            if (kVisibleCapacity == 0u)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            // Path selection rejects assets whose complete meshlet table cannot
            // fit in the indirect stream. Keep this recording-side guard
            // explicit as well: truncating the table here could drop a later
            // LOD range while still producing a seemingly valid frame.
            if (asset->meshlets.size() > kVisibleCapacity)
            {
                ctx.setError("Virtual Geometry meshlet table exceeds indirect capacity");
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            const std::uint32_t meshletCount = static_cast<std::uint32_t>(asset->meshlets.size());
            const std::uint64_t possibleEntries = static_cast<std::uint64_t>(meshletCount) *
                static_cast<std::uint64_t>(ctx.packet->instances.size());
            if (possibleEntries > kVisibleCapacity)
            {
                // Path selection normally catches this before recording. Keep
                // the pass defensive so a future caller cannot silently lose
                // meshlets when the packed visibility ID buffer saturates.
                ctx.setError("Virtual Geometry visible meshlet capacity exceeded");
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            const std::uint32_t visibleEntryCapacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                possibleEntries, kVisibleCapacity));
            vkCmdFillBuffer(ctx.commandBuffer(), visibleCountBuffer, 0, sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(ctx.commandBuffer(), visibleBuffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(ctx.commandBuffer(), commandBuffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(ctx.commandBuffer(), indirectCountBuffer, 0, sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(ctx.commandBuffer(), meshCommandBuffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(ctx.commandBuffer(), meshCommandCountBuffer, 0, sizeof(std::uint32_t), 0);
            VkBufferMemoryBarrier2 resetBarriers[6]{};
            for (auto& barrier : resetBarriers)
            {
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            }
            resetBarriers[0].buffer = visibleBuffer; resetBarriers[0].size = VK_WHOLE_SIZE;
            resetBarriers[1].buffer = visibleCountBuffer; resetBarriers[1].size = sizeof(std::uint32_t);
            resetBarriers[2].buffer = commandBuffer; resetBarriers[2].size = VK_WHOLE_SIZE;
            resetBarriers[3].buffer = indirectCountBuffer;
            resetBarriers[3].size = sizeof(std::uint32_t);
            resetBarriers[4].buffer = meshCommandBuffer; resetBarriers[4].size = VK_WHOLE_SIZE;
            resetBarriers[5].buffer = meshCommandCountBuffer; resetBarriers[5].size = sizeof(std::uint32_t);
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 6;
            dependency.pBufferMemoryBarriers = resetBarriers;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);

            const auto* gpu = ctx.sceneResources->virtualGeometryBuffersDense(meshId);
            if (gpu == nullptr || gpu->meshlets.buffer == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            const VkDescriptorSet cullSet = ctx.allocateSet(ctx.pipelines->meshletCullLayout);
            if (cullSet == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            ctx.writeStorageBuffer(cullSet, 0, gpu->meshlets.buffer,
                static_cast<VkDeviceSize>(asset->meshlets.size() *
                    sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
            ctx.writeStorageBuffer(cullSet, 1, visibleBuffer, visible.descriptor.size);
            ctx.writeStorageBuffer(cullSet, 2, visibleCountBuffer, sizeof(std::uint32_t));
            const VkImage previousHiZImage =
                ctx.frameGraphProvider->image(previousHiZ.native);
            if (previousHiZImage == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                if (ctx.virtualHiZImageInitialized != nullptr)
                    *ctx.virtualHiZImageInitialized = false;
                return;
            }
            const bool hizLayoutInitialized = ctx.virtualHiZImageInitialized != nullptr &&
                *ctx.virtualHiZImageInitialized;
            ctx.transitionImage(previousHiZImage,
                hizLayoutInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                hizLayoutInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                     : VK_PIPELINE_STAGE_2_NONE,
                hizLayoutInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                     : VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (ctx.virtualHiZImageInitialized != nullptr)
                *ctx.virtualHiZImageInitialized = true;
            ctx.writeSampled(cullSet, 3,
                ctx.frameGraphProvider->view(previousHiZ.native));
            ctx.writeStorageBuffer(cullSet, 4,
                ctx.frameGraphProvider->buffer(transformRows.native), transformRows.descriptor.size);
            ctx.writeUniformBuffer(cullSet, 5,
                ctx.frameGraphProvider->buffer(cullFrame.native), sizeof(CullFrameData));
            ctx.writeStorageBuffer(cullSet, 6, gpu->dagNodes.buffer,
                asset->dagNodes.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuDagNode));
            ctx.writeStorageBuffer(cullSet, 7, gpu->clusters.buffer,
                asset->clusters.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuCluster));
            ctx.writeStorageBuffer(cullSet, 8, gpu->clusterMeshletIndices.buffer,
                gpu->clusterMeshletIndices.size);
            const auto& selectedNodes = resources.get<Graph::FrameGraphBuffer>(ctx.selectedLodNodes);
            const auto& selectedCount = resources.get<Graph::FrameGraphBuffer>(ctx.selectedLodCount);
            const VkBuffer selectionCounterBuffer =
                ctx.frameGraphProvider->buffer(selectedCount.native);
            ctx.writeStorageBuffer(cullSet, 9,
                ctx.frameGraphProvider->buffer(selectedNodes.native), selectedNodes.descriptor.size);
            ctx.writeStorageBuffer(cullSet, 10,
                ctx.frameGraphProvider->buffer(selectedCount.native), sizeof(std::uint32_t));
            ctx.writeStorageBuffer(cullSet, 11, gpu->meshletDagNodes.buffer,
                asset->meshlets.size() * sizeof(std::uint32_t));
            ctx.writeStorageBuffer(cullSet, 12, gpu->lodStates.buffer,
                asset->dagNodes.size() * sizeof(std::uint32_t) * 4u);
            struct alignas(16) CullConstants
            {
                glm::vec4 planes[6];
                glm::vec4 cameraPosition;
                std::uint32_t meshletCount, instanceIndex, lodAndFlags;
                std::uint32_t visibleCapacity;
            } constants{};
            static_assert(sizeof(CullConstants) == 128,
                "Virtual Geometry cull push constants must match meshlet_cull.comp.hlsl");
            const glm::mat4& vp = ctx.packet->camera.viewProjection;
            const glm::vec4 rows[4] = {{vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
                {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
                {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
                {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
            constants.planes[0] = rows[3] + rows[0]; constants.planes[1] = rows[3] - rows[0];
            constants.planes[2] = rows[3] + rows[1]; constants.planes[3] = rows[3] - rows[1];
            constants.planes[4] = rows[3] + rows[2]; constants.planes[5] = rows[3] - rows[2];
            for (auto& plane : constants.planes)
            {
                const float length = glm::length(glm::vec3(plane));
                if (length > 1.0e-6f)
                    plane /= length;
            }
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletCullPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletCullPipeline.layout(), 0, 1, &cullSet, 0, nullptr);
            const glm::vec4 cameraWorld{
                glm::vec3(ctx.packet->camera.positionAndNear), 1.0f};
            glm::vec3 boundsMin(std::numeric_limits<float>::max());
            glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
            for (const auto& primitive : asset->primitives)
            {
                boundsMin = glm::min(boundsMin, primitive.boundsMin);
                boundsMax = glm::max(boundsMax, primitive.boundsMax);
            }
            const bool hasBounds = !asset->primitives.empty();
            const glm::vec3 boundsCenter = hasBounds
                ? (boundsMin + boundsMax) * 0.5f : glm::vec3(0.0f);
            const float boundsRadius = hasBounds
                ? std::max(glm::length(boundsMax - boundsCenter), 1.0e-4f) : 1.0f;
            const std::uint32_t lodLevelCount = asset->primitives.empty()
                ? 1u : std::max(1u, static_cast<std::uint32_t>(
                    asset->lods.size() / asset->primitives.size()));
            const bool sameCamera = ctx.previousPacketValid &&
                std::memcmp(glm::value_ptr(ctx.previousViewProjection),
                    glm::value_ptr(ctx.packet->camera.viewProjection), sizeof(glm::mat4)) == 0;
            const auto* previousInstances = ctx.cpuDraw.previousInstances;
            for (std::uint32_t candidateIndex = 0;
                candidateIndex < ctx.packet->instances.size(); ++candidateIndex)
            {
                const auto& candidate = ctx.packet->instances[candidateIndex];
                // The M5 V1 draw stream binds one consolidated virtual asset
                // (and therefore one index/vertex table) for the whole pass.
                // Path selection normally enforces this invariant, but keep
                // the recording path defensive so a late packet mutation or
                // an unsupported material cannot emit tokens that reference
                // the wrong GPU table.
                if (candidateIndex >= VulkanFrameResources::MaxVirtualGeometryInstances ||
                    candidate.meshId != meshId ||
                    (candidate.flags & kVirtualGeometryUnsupportedFlags) != 0u ||
                    !ctx.sceneResources->virtualGeometryMaterialCompatible(candidate.materialId) ||
                    !asset->hasUniformPrimitiveMaterial())
                    continue;
                const glm::mat4 model = glm::make_mat4(candidate.transform.data());
                constants.cameraPosition = glm::inverse(model) * cameraWorld;
                constants.meshletCount = meshletCount;
                constants.instanceIndex = candidateIndex;
                const float distance = std::max(1.0e-4f,
                    glm::length(glm::vec3(constants.cameraPosition) - boundsCenter) - boundsRadius);
                // M6 uses projected geometric error instead of fixed distance
                // bands. Derive the vertical FOV from the projection matrix
                // so the CPU selection stays consistent with the camera ABI.
                const float projectionY = std::abs(ctx.packet->camera.projection[1][1]);
                const float verticalFov = projectionY > 1.0e-6f
                    ? 2.0f * std::atan(1.0f / projectionY) : glm::radians(60.0f);
                const float viewportHeight = std::max(1.0f, ctx.packet->camera.viewportAndInvViewport.y);
                std::uint32_t lod = 0u;
                for (std::uint32_t candidateLod = lodLevelCount; candidateLod-- > 0u;)
                {
                    const std::size_t lodIndex = static_cast<std::size_t>(candidateLod);
                    const std::size_t primitiveLodIndex = lodIndex;
                    if (primitiveLodIndex >= asset->lods.size() ||
                        asset->lods[primitiveLodIndex].primitiveIndex !=
                            asset->lods.front().primitiveIndex)
                        continue;
                    const float screenError = Halcyon::Renderer::Scene::virtualGeometryScreenError(
                        asset->lods[primitiveLodIndex].geometricError, distance,
                        viewportHeight, verticalFov);
                    if (screenError <= 1.0f || candidateLod == 0u)
                    {
                        lod = candidateLod;
                        break;
                    }
                }
                const bool sameInstance = previousInstances != nullptr &&
                    candidateIndex < previousInstances->size() &&
                    (*previousInstances)[candidateIndex].meshId == candidate.meshId &&
                    (*previousInstances)[candidateIndex].transform == candidate.transform;
                const bool allowHiZ = cullFrameData.hiz.w != 0u && sameCamera && sameInstance;
                constants.lodAndFlags = lod |
                    (allowHiZ ? Halcyon::Renderer::Scene::kVirtualCullHiZEnabledFlag : 0u);
                constants.visibleCapacity = visibleEntryCapacity;
                vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->meshletCullPipeline.layout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                vkCmdDispatchIndirect(ctx.commandBuffer(),
                    ctx.frameGraphProvider->buffer(selectedCount.native),
                    sizeof(std::uint32_t) * 2u);
                if (candidateIndex + 1u < ctx.packet->instances.size())
                {
                    VkBufferMemoryBarrier2 instanceBarrier[2]{};
                    for (auto& barrier : instanceBarrier)
                    {
                        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_SHADER_WRITE_BIT;
                    }
                    instanceBarrier[0].buffer = visibleBuffer;
                    instanceBarrier[0].size = VK_WHOLE_SIZE;
                    instanceBarrier[1].buffer = visibleCountBuffer;
                    instanceBarrier[1].size = sizeof(std::uint32_t);
                    dependency.bufferMemoryBarrierCount = 2;
                    dependency.pBufferMemoryBarriers = instanceBarrier;
                    vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
                }
            }

            VkBufferMemoryBarrier2 cullBarrier[2]{};
            for (auto& barrier : cullBarrier)
            {
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            }
            cullBarrier[0].buffer = visibleBuffer; cullBarrier[0].size = VK_WHOLE_SIZE;
            cullBarrier[1].buffer = visibleCountBuffer; cullBarrier[1].size = sizeof(std::uint32_t);
            dependency.bufferMemoryBarrierCount = 2;
            dependency.pBufferMemoryBarriers = cullBarrier;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);

            if (meshShaderPath)
            {
                const VkDescriptorSet meshSet = ctx.allocateSet(ctx.pipelines->meshletMeshIndirectLayout);
                if (meshSet == VK_NULL_HANDLE) return;
                ctx.writeStorageBuffer(meshSet, 0, visibleBuffer, visible.descriptor.size);
                ctx.writeStorageBuffer(meshSet, 1, visibleCountBuffer, sizeof(std::uint32_t));
                ctx.writeStorageBuffer(meshSet, 2, meshCommandBuffer, meshCommands.descriptor.size);
                ctx.writeStorageBuffer(meshSet, 3, meshCommandCountBuffer, sizeof(std::uint32_t));
                vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                    ctx.pipelines->meshletMeshIndirectPipeline.computePipeline());
                vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                    ctx.pipelines->meshletMeshIndirectPipeline.layout(), 0, 1, &meshSet, 0, nullptr);
                const glm::uvec4 commandCapacity{visibleEntryCapacity, 0u, 0u, 0u};
                vkCmdPushConstants(ctx.commandBuffer(),
                    ctx.pipelines->meshletMeshIndirectPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(commandCapacity), &commandCapacity);
                vkCmdDispatch(ctx.commandBuffer(), 1u, 1u, 1u);
                std::array<VkBufferMemoryBarrier2, 3> meshBarriers{};
                for (auto& barrier : meshBarriers)
                {
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                        VK_ACCESS_2_TRANSFER_READ_BIT;
                }
                meshBarriers[0].buffer = meshCommandBuffer; meshBarriers[0].size = VK_WHOLE_SIZE;
                meshBarriers[1].buffer = meshCommandCountBuffer; meshBarriers[1].size = sizeof(std::uint32_t);
                meshBarriers[2].buffer = selectionCounterBuffer;
                meshBarriers[2].size = sizeof(std::uint32_t) * 2u;
                dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(meshBarriers.size());
                dependency.pBufferMemoryBarriers = meshBarriers.data();
                vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
                if (ctx.virtualVisibilityValid != nullptr)
                {
                    *ctx.virtualVisibilityValid = true;
                }
                if (ctx.debugReadbacks != nullptr &&
                    ctx.currentFrame < ctx.debugReadbacks->virtualGeometryReadbacks.size())
                {
                    const VkBuffer readback =
                        ctx.debugReadbacks->virtualGeometryReadbacks[ctx.currentFrame].buffer;
                    if (readback != VK_NULL_HANDLE)
                    {
                        const VkBufferCopy visibleCopy{0, 0, sizeof(std::uint32_t)};
                        const VkBufferCopy commandCopy{
                            0, sizeof(std::uint32_t), sizeof(std::uint32_t)};
                        const VkBufferCopy selectionCopy{
                            0, sizeof(std::uint32_t) * 3u, sizeof(std::uint32_t) * 2u};
                        vkCmdCopyBuffer(ctx.commandBuffer(), visibleCountBuffer,
                            readback, 1, &visibleCopy);
                        vkCmdCopyBuffer(ctx.commandBuffer(), meshCommandCountBuffer,
                            readback, 1, &commandCopy);
                        vkCmdCopyBuffer(ctx.commandBuffer(), selectionCounterBuffer,
                            readback, 1, &selectionCopy);
                    }
                }
                return;
            }

            const VkDescriptorSet indirectSet = ctx.allocateSet(ctx.pipelines->meshletIndirectLayout);
            if (indirectSet == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            ctx.writeStorageBuffer(indirectSet, 0, visibleBuffer, visible.descriptor.size);
            ctx.writeStorageBuffer(indirectSet, 1, gpu->meshlets.buffer,
                static_cast<VkDeviceSize>(asset->meshlets.size() *
                    sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
            ctx.writeStorageBuffer(indirectSet, 2, visibleCountBuffer, sizeof(std::uint32_t));
            ctx.writeStorageBuffer(indirectSet, 3, commandBuffer, commands.descriptor.size);
            ctx.writeStorageBuffer(indirectSet, 4, indirectCountBuffer, sizeof(std::uint32_t));
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletIndirectPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletIndirectPipeline.layout(), 0, 1, &indirectSet, 0, nullptr);
            struct IndirectConstants
            {
                std::uint32_t meshletCount;
                std::uint32_t commandCapacity;
                std::uint32_t instanceCount;
                std::uint32_t indexCount;
            } indirectConstants{meshletCount, visibleEntryCapacity,
                static_cast<std::uint32_t>(ctx.packet->instances.size()),
                static_cast<std::uint32_t>(asset->indices.size())};
            static_assert(sizeof(IndirectConstants) == 16,
                "Virtual Geometry indirect push constants must match meshlet_build_indirect.comp.hlsl");
            vkCmdPushConstants(ctx.commandBuffer(),
                ctx.pipelines->meshletIndirectPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(indirectConstants), &indirectConstants);
            vkCmdDispatch(ctx.commandBuffer(), (visibleEntryCapacity + 63u) / 64u, 1, 1);

            std::array<VkBufferMemoryBarrier2, 4> indirectBarriers{};
            for (auto& barrier : indirectBarriers)
            {
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_2_TRANSFER_READ_BIT;
            }
            indirectBarriers[0].buffer = commandBuffer;
            indirectBarriers[0].size = VK_WHOLE_SIZE;
            indirectBarriers[1].buffer = visibleCountBuffer;
            indirectBarriers[1].size = sizeof(std::uint32_t);
            indirectBarriers[2].buffer = indirectCountBuffer;
            indirectBarriers[2].size = sizeof(std::uint32_t);
            indirectBarriers[3].buffer = selectionCounterBuffer;
            indirectBarriers[3].size = sizeof(std::uint32_t) * 2u;
            dependency.bufferMemoryBarrierCount = 4;
            dependency.pBufferMemoryBarriers = indirectBarriers.data();
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
            // Hand a fully initialized indirect stream to visibility. The
            // visibility pass consumes this token and only publishes it again
            // after all three ID attachments have been rendered.
            if (ctx.virtualVisibilityValid != nullptr)
                *ctx.virtualVisibilityValid = true;
            if (ctx.debugReadbacks != nullptr &&
                ctx.currentFrame < ctx.debugReadbacks->virtualGeometryReadbacks.size())
            {
                const VkBuffer readback =
                    ctx.debugReadbacks->virtualGeometryReadbacks[ctx.currentFrame].buffer;
                if (readback != VK_NULL_HANDLE)
                {
                    const VkBufferCopy first{0, 0, sizeof(std::uint32_t)};
                    const VkBufferCopy second{0, sizeof(std::uint32_t), sizeof(std::uint32_t)};
                    const VkBufferCopy selectionCopy{
                        0, sizeof(std::uint32_t) * 3u, sizeof(std::uint32_t) * 2u};
                    vkCmdCopyBuffer(ctx.commandBuffer(), visibleCountBuffer, readback, 1, &first);
                    vkCmdCopyBuffer(ctx.commandBuffer(), indirectCountBuffer, readback, 1, &second);
                    vkCmdCopyBuffer(ctx.commandBuffer(), selectionCounterBuffer,
                        readback, 1, &selectionCopy);
                }
            }
        });

    // Visibility rasterization consumes the GPU-generated indexed indirect
    // stream.  The vertex shader reconstructs each triangle from the virtual
    // geometry tables, so no CPU draw loop or per-meshlet descriptor switch is
    // involved in the M5 path.
    const auto depth = ctx.depth;
    graph.addPass<Graph::FrameGraph::Empty>("M5 visibility rasterization",
            [passCtx, depth, meshShaderPath](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            if (meshShaderPath)
            {
                builder.read(passCtx->meshletMeshIndirect, Graph::ResourceUsage::Indirect);
                builder.read(passCtx->meshletMeshIndirectCount, Graph::ResourceUsage::Indirect);
                builder.read(passCtx->visibleMeshlets, Graph::ResourceUsage::Storage);
                builder.read(passCtx->visibleMeshletCount, Graph::ResourceUsage::Storage);
            }
            else
            {
                builder.read(passCtx->meshletIndirect, Graph::ResourceUsage::Indirect);
                builder.read(passCtx->meshletIndirectCount, Graph::ResourceUsage::Indirect);
            }
            builder.read(passCtx->virtualTransforms, Graph::ResourceUsage::Storage);
            builder.read(passCtx->virtualMeshMaterials, Graph::ResourceUsage::Storage);
            passCtx->visibility = builder.write(passCtx->visibility, Graph::ResourceUsage::ColorAttachment);
            passCtx->visibilityPrimitive = builder.write(passCtx->visibilityPrimitive,
                Graph::ResourceUsage::ColorAttachment);
            passCtx->visibilityBarycentrics = builder.write(passCtx->visibilityBarycentrics,
                Graph::ResourceUsage::ColorAttachment);
            passCtx->depth = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = passCtx->visibility;
            descriptor.attachments.color[1] = passCtx->visibilityPrimitive;
            descriptor.attachments.color[2] = passCtx->visibilityBarycentrics;
            descriptor.attachments.depth = passCtx->depth;
            descriptor.viewport.width = passCtx->width;
            descriptor.viewport.height = passCtx->height;
             descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::AllColors |
                 Graph::FrameGraphAttachmentFlags::Depth;
            builder.declareRenderPass("M5 visibility rasterization", descriptor);
            builder.sideEffect();
        },
        [passCtx, meshShaderPath](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.virtualVisibilityValid == nullptr)
                return;
            const bool indirectValid = *ctx.virtualVisibilityValid;
            *ctx.virtualVisibilityValid = false;
            if (!indirectValid)
                return;
            if (ctx.frameGraphProvider == nullptr || ctx.pipelines == nullptr || ctx.gpuSceneBuffers == nullptr ||
                ctx.sceneResources == nullptr || ctx.packet == nullptr ||
                (!meshShaderPath && ctx.pipelines->visibilityPipeline.pipeline() == VK_NULL_HANDLE) ||
                (meshShaderPath && (ctx.pipelines->virtualGeometryMeshPipeline.pipeline() == VK_NULL_HANDLE ||
                    ctx.cmdDrawMeshTasksIndirectCount == nullptr)))
                return;
            const auto info = resources.getRenderPassInfo(0);
            const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
            if (target == nullptr) return;
            const auto selection = selectVirtualGeometry(ctx);
            const auto* asset = selection.asset;
            const auto* instance = selection.instance;
            const std::uint32_t meshId = selection.meshId;
            const auto* gpu = instance == nullptr ? nullptr :
                ctx.sceneResources->virtualGeometryBuffersDense(meshId);
            if (!selection || gpu == nullptr ||
                gpu->meshlets.buffer == VK_NULL_HANDLE ||
                gpu->geometryPagePool.buffer == VK_NULL_HANDLE ||
                gpu->pageTable.buffer == VK_NULL_HANDLE ||
                gpu->pageInfo.buffer == VK_NULL_HANDLE ||
                (!meshShaderPath && gpu->rasterIndices.buffer == VK_NULL_HANDLE))
                return;
             const auto& visibilityResource = resources.getTexture(passCtx->visibility);
             const auto& primitiveResource = resources.getTexture(passCtx->visibilityPrimitive);
             const auto& barycentricResource = resources.getTexture(passCtx->visibilityBarycentrics);
            const auto& transformsResource =
                resources.get<Graph::FrameGraphBuffer>(ctx.virtualTransforms);
            const auto& meshMaterialsResource =
                resources.get<Graph::FrameGraphBuffer>(ctx.virtualMeshMaterials);
             const VkImage visibilityImage = ctx.frameGraphProvider->image(visibilityResource.native);
            const VkImage primitiveImage = ctx.frameGraphProvider->image(primitiveResource.native);
            const VkImage barycentricImage = ctx.frameGraphProvider->image(barycentricResource.native);
            const auto& depthResource = resources.getTexture(passCtx->depth);
            (void)depthResource;
            const VkImage depthImage = ctx.frameGraphProvider->image(
                target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]);
            const VkImageView visibilityView = ctx.frameGraphProvider->view(visibilityResource.native);
            const VkImageView primitiveView = ctx.frameGraphProvider->view(primitiveResource.native);
            const VkImageView barycentricView = ctx.frameGraphProvider->view(barycentricResource.native);
            const auto* indirectResources = meshShaderPath
                ? &resources.get<Graph::FrameGraphBuffer>(ctx.meshletMeshIndirect)
                : &resources.get<Graph::FrameGraphBuffer>(ctx.meshletIndirect);
            const auto* countResources = meshShaderPath
                ? &resources.get<Graph::FrameGraphBuffer>(ctx.meshletMeshIndirectCount)
                : &resources.get<Graph::FrameGraphBuffer>(ctx.meshletIndirectCount);
            const VkBuffer indirectBuffer = ctx.frameGraphProvider->buffer(indirectResources->native);
            const VkBuffer countBuffer = ctx.frameGraphProvider->buffer(countResources->native);
            if (visibilityImage == VK_NULL_HANDLE || primitiveImage == VK_NULL_HANDLE ||
                barycentricImage == VK_NULL_HANDLE || depthImage == VK_NULL_HANDLE ||
                target->views[0] == VK_NULL_HANDLE || target->views[1] == VK_NULL_HANDLE ||
                target->views[2] == VK_NULL_HANDLE || target->depthView == VK_NULL_HANDLE ||
                visibilityView == VK_NULL_HANDLE || primitiveView == VK_NULL_HANDLE ||
                barycentricView == VK_NULL_HANDLE || indirectBuffer == VK_NULL_HANDLE ||
                countBuffer == VK_NULL_HANDLE)
                return;
             ctx.transitionImage(visibilityImage, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             ctx.transitionImage(primitiveImage, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             ctx.transitionImage(barycentricImage, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            ctx.transitionImage(depthImage, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
             std::array<VkRenderingAttachmentInfo, 3> colors{};
             for (auto& color : colors)
             {
                 color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                 color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                 color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                 color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                 color.clearValue.color.uint32[0] = 0u;
             }
             colors[0].imageView = target->views[0];
             colors[1].imageView = target->views[1];
             colors[2].imageView = target->views[2];
            VkRenderingAttachmentInfo z{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            z.imageView = target->depthView; z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            z.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; z.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            z.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = ctx.swapchainExtent; rendering.layerCount = 1;
             rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
             rendering.pColorAttachments = colors.data(); rendering.pDepthAttachment = &z;
            vkCmdBeginRendering(ctx.commandBuffer(), &rendering);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                meshShaderPath ? ctx.pipelines->virtualGeometryMeshPipeline.pipeline()
                                : ctx.pipelines->visibilityPipeline.pipeline());
            const VkDescriptorSet set = ctx.allocateSet(meshShaderPath
                ? ctx.pipelines->virtualGeometryMeshLayout : ctx.pipelines->visibilityLayout);
            if (set == VK_NULL_HANDLE)
            {
                vkCmdEndRendering(ctx.commandBuffer());
                ctx.transitionImage(visibilityImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                ctx.transitionImage(primitiveImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                ctx.transitionImage(barycentricImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                return;
            }
            if (meshShaderPath)
            {
                const auto& visible = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshlets);
                const auto& visibleCount = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshletCount);
                ctx.writeStorageBuffer(set, 0, ctx.frameGraphProvider->buffer(visible.native), visible.descriptor.size);
                ctx.writeStorageBuffer(set, 1, ctx.frameGraphProvider->buffer(visibleCount.native), sizeof(std::uint32_t));
                ctx.writeStorageBuffer(set, 2, gpu->meshlets.buffer,
                    asset->meshlets.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet));
                ctx.writeStorageBuffer(set, 3, gpu->geometryPagePool.buffer,
                    gpu->geometryPagePool.size);
                ctx.writeStorageBuffer(set, 4, gpu->pageTable.buffer,
                    gpu->pageTable.size);
                ctx.writeStorageBuffer(set, 5, gpu->pageInfo.buffer,
                    sizeof(VulkanSceneResources::VirtualGeometryGpuPageInfo));
                ctx.writeStorageBuffer(set, 6,
                    ctx.frameGraphProvider->buffer(transformsResource.native),
                    transformsResource.descriptor.size);
                ctx.writeStorageBuffer(set, 7,
                    ctx.frameGraphProvider->buffer(meshMaterialsResource.native),
                    meshMaterialsResource.descriptor.size);
            }
            else
            {
                ctx.writeStorageBuffer(set, 0, gpu->meshlets.buffer,
                    asset->meshlets.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet));
                ctx.writeStorageBuffer(set, 1, gpu->geometryPagePool.buffer,
                    gpu->geometryPagePool.size);
                ctx.writeStorageBuffer(set, 2, gpu->pageTable.buffer,
                    gpu->pageTable.size);
                ctx.writeStorageBuffer(set, 3, gpu->pageInfo.buffer,
                    sizeof(VulkanSceneResources::VirtualGeometryGpuPageInfo));
                ctx.writeStorageBuffer(set, 4,
                    ctx.frameGraphProvider->buffer(transformsResource.native),
                    transformsResource.descriptor.size);
                ctx.writeStorageBuffer(set, 5,
                    ctx.frameGraphProvider->buffer(meshMaterialsResource.native),
                    meshMaterialsResource.descriptor.size);
            }
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                meshShaderPath ? ctx.pipelines->virtualGeometryMeshPipeline.layout()
                                : ctx.pipelines->visibilityPipeline.layout(), 0, 1, &set, 0, nullptr);
             struct alignas(16) VisibilityConstants
             {
                 glm::mat4 viewProjection{1.0f};
                 glm::uvec4 counts{0u};
             } constants{};
             static_assert(sizeof(VisibilityConstants) == 80,
                 "Virtual Geometry visibility push constants must match visibility.vert.hlsl");
             constants.viewProjection = ctx.packet->camera.viewProjection;
             constants.counts = glm::uvec4{
                 static_cast<std::uint32_t>(asset->meshlets.size()),
                 static_cast<std::uint32_t>(asset->vertices.size()),
                 static_cast<std::uint32_t>(ctx.packet->instances.size()),
                 static_cast<std::uint32_t>(asset->meshletVertices.size())};
            vkCmdPushConstants(ctx.commandBuffer(), meshShaderPath
                ? ctx.pipelines->virtualGeometryMeshPipeline.layout()
                : ctx.pipelines->visibilityPipeline.layout(),
                meshShaderPath ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(constants), &constants);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(ctx.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, ctx.swapchainExtent};
            vkCmdSetViewport(ctx.commandBuffer(), 0, 1, &viewport);
            vkCmdSetScissor(ctx.commandBuffer(), 0, 1, &scissor);
             if (meshShaderPath)
             {
                 ctx.cmdDrawMeshTasksIndirectCount(ctx.commandBuffer(), indirectBuffer, 0,
                     countBuffer, 0, 1u,
                     sizeof(VkDrawMeshTasksIndirectCommandEXT));
             }
             else
             {
                 vkCmdBindIndexBuffer(ctx.commandBuffer(), gpu->rasterIndices.buffer, 0,
                     VK_INDEX_TYPE_UINT32);
                 vkCmdDrawIndexedIndirectCount(ctx.commandBuffer(),
                     indirectBuffer, 0, countBuffer, 0,
                     std::min<std::uint32_t>(
                         ctx.virtualIndirectDrawCapacity,
                         static_cast<std::uint32_t>(indirectResources->descriptor.size /
                             sizeof(VkDrawIndexedIndirectCommand))),
                     sizeof(VkDrawIndexedIndirectCommand));
             }
             vkCmdEndRendering(ctx.commandBuffer());
             if (ctx.virtualVisibilityValid != nullptr)
             {
                 *ctx.virtualVisibilityValid = true;
             }
             ctx.transitionImage(visibilityImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
             ctx.transitionImage(primitiveImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
             ctx.transitionImage(barycentricImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });

    graph.addPass<Graph::FrameGraph::Empty>("M5 Hi-Z build",
        [passCtx](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(passCtx->depth, Graph::ResourceUsage::Sampled);
            passCtx->hiz = builder.write(passCtx->hiz,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::Sampled);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.virtualVisibilityValid == nullptr || !*ctx.virtualVisibilityValid)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            if (ctx.pipelines == nullptr || ctx.frameGraphProvider == nullptr ||
                ctx.pipelines->hizBuildPipeline.computePipeline() == VK_NULL_HANDLE ||
                ctx.pipelines->hizLayout == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            const auto& depth = resources.getTexture(ctx.depth);
            const auto& hiz = resources.getTexture(ctx.hiz);
            const VkImage depthImage = ctx.frameGraphProvider->image(depth.native);
            const VkImage hizImage = ctx.frameGraphProvider->image(hiz.native);
            if (depthImage == VK_NULL_HANDLE || hizImage == VK_NULL_HANDLE)
            {
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                if (ctx.virtualHiZImageInitialized != nullptr)
                    *ctx.virtualHiZImageInitialized = false;
                return;
            }

            ctx.transitionImage(depthImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            const bool hizImageInitialized = ctx.virtualHiZImageInitialized != nullptr &&
                *ctx.virtualHiZImageInitialized;
            ctx.transitionImage(hizImage,
                hizImageInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL,
                hizImageInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                    : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                hizImageInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                    : VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            if (ctx.virtualHiZImageInitialized != nullptr)
                *ctx.virtualHiZImageInitialized = true;

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
                vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
            };
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->hizBuildPipeline.computePipeline());
            std::uint32_t sourceWidth = depth.descriptor.width;
            std::uint32_t sourceHeight = depth.descriptor.height;
            bool buildComplete = true;
            for (std::uint32_t mip = 0; mip < hiz.descriptor.mipLevels; ++mip)
            {
                const VkImageView sourceView = mip == 0u
                    ? ctx.frameGraphProvider->view(depth.native)
                    : ctx.frameGraphProvider->mipView(hiz.native, mip - 1u);
                const VkImageView outputView =
                    ctx.frameGraphProvider->mipView(hiz.native, mip);
                if (sourceView == VK_NULL_HANDLE || outputView == VK_NULL_HANDLE)
                {
                    buildComplete = false;
                    break;
                }
                const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->hizLayout);
                if (set == VK_NULL_HANDLE)
                {
                    buildComplete = false;
                    break;
                }
                writeImage(set, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceView,
                    mip == 0u ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_GENERAL);
                writeImage(set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, outputView,
                    VK_IMAGE_LAYOUT_GENERAL);
                vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                    ctx.pipelines->hizBuildPipeline.layout(), 0, 1, &set, 0, nullptr);
                struct HiZConstants
                {
                    std::uint32_t sourceWidth, sourceHeight, outputWidth, outputHeight;
                } constants{sourceWidth, sourceHeight,
                    std::max(1u, (sourceWidth + 1u) / 2u),
                    std::max(1u, (sourceHeight + 1u) / 2u)};
                vkCmdPushConstants(ctx.commandBuffer(),
                    ctx.pipelines->hizBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(constants), &constants);
                vkCmdDispatch(ctx.commandBuffer(), (constants.outputWidth + 7u) / 8u,
                    (constants.outputHeight + 7u) / 8u, 1u);

                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                    VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.image = hizImage;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, 0u, 1u};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1u;
                dependency.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
                sourceWidth = constants.outputWidth;
                sourceHeight = constants.outputHeight;
            }
            if (!buildComplete)
            {
                // Keep the persistent image in the layout expected by next
                // frame's culling even when a mip view or descriptor pool
                // allocation fails halfway through the reduction.
                ctx.transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (ctx.virtualHiZInitialized != nullptr)
                    *ctx.virtualHiZInitialized = false;
                return;
            }
            ctx.transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (ctx.virtualHiZInitialized != nullptr)
                *ctx.virtualHiZInitialized = true;
        });

    graph.addPass<Graph::FrameGraph::Empty>("M5 material classification",
        [passCtx](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(passCtx->visibility, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->visibilityPrimitive, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->virtualMeshMaterials, Graph::ResourceUsage::Storage);
            passCtx->materialClassification = builder.write(passCtx->materialClassification,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            passCtx->virtualValidation = builder.write(passCtx->virtualValidation,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.frameGraphProvider == nullptr || ctx.packet == nullptr ||
                ctx.sceneResources == nullptr) return;
            const auto& output = resources.get<Graph::FrameGraphBuffer>(ctx.materialClassification);
            const auto& validation =
                resources.get<Graph::FrameGraphBuffer>(ctx.virtualValidation);
            const VkBuffer outputBuffer = ctx.frameGraphProvider->buffer(output.native);
            const VkBuffer validationBuffer =
                ctx.frameGraphProvider->buffer(validation.native);
            if (outputBuffer == VK_NULL_HANDLE || validationBuffer == VK_NULL_HANDLE)
            {
                if (ctx.virtualVisibilityValid != nullptr)
                    *ctx.virtualVisibilityValid = false;
                return;
            }
            vkCmdFillBuffer(ctx.commandBuffer(), validationBuffer, 0,
                sizeof(std::uint32_t), 0u);
            VkBufferMemoryBarrier2 validationReset{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            validationReset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            validationReset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            validationReset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            validationReset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                VK_ACCESS_2_SHADER_WRITE_BIT;
            validationReset.buffer = validationBuffer;
            validationReset.size = sizeof(std::uint32_t);
            VkDependencyInfo validationResetDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            validationResetDependency.bufferMemoryBarrierCount = 1u;
            validationResetDependency.pBufferMemoryBarriers = &validationReset;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &validationResetDependency);
            const auto clearOutput = [&]()
            {
                if (ctx.virtualVisibilityValid != nullptr)
                    *ctx.virtualVisibilityValid = false;
                vkCmdFillBuffer(ctx.commandBuffer(), outputBuffer, 0,
                    output.descriptor.size, 0xffffffffu);
                VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.buffer = outputBuffer;
                barrier.size = output.descriptor.size;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
            };
            if (ctx.virtualVisibilityValid == nullptr || !*ctx.virtualVisibilityValid ||
                ctx.pipelines == nullptr || ctx.packet == nullptr || ctx.sceneResources == nullptr ||
                ctx.pipelines->materialClassifyPipeline.computePipeline() == VK_NULL_HANDLE)
            {
                clearOutput();
                return;
            }
             const auto& image = resources.getTexture(ctx.visibility);
             const auto& primitive = resources.getTexture(ctx.visibilityPrimitive);
             const auto& meshMaterials =
                 resources.get<Graph::FrameGraphBuffer>(ctx.virtualMeshMaterials);
             const VkImageView visibilityView = ctx.frameGraphProvider->view(image.native);
             const VkImageView primitiveView = ctx.frameGraphProvider->view(primitive.native);
             const VkBuffer meshMaterialsBuffer =
                 ctx.frameGraphProvider->buffer(meshMaterials.native);
             const auto selection = selectVirtualGeometry(ctx);
             const auto* asset = selection.asset;
             const std::uint32_t meshId = selection.meshId;
             const auto* gpu = asset == nullptr ? nullptr :
                 ctx.sceneResources->virtualGeometryBuffersDense(meshId);
             if (!selection || visibilityView == VK_NULL_HANDLE || primitiveView == VK_NULL_HANDLE ||
                 meshMaterialsBuffer == VK_NULL_HANDLE || gpu == nullptr ||
                 gpu->meshlets.buffer == VK_NULL_HANDLE ||
                 gpu->geometryPagePool.buffer == VK_NULL_HANDLE ||
                 gpu->pageTable.buffer == VK_NULL_HANDLE ||
                 gpu->pageInfo.buffer == VK_NULL_HANDLE)
             {
                 clearOutput();
                 return;
             }
             const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->materialClassifyLayout);
             if (set == VK_NULL_HANDLE)
             {
                 clearOutput();
                 return;
             }
             ctx.writeSampled(set, 0, visibilityView);
             ctx.writeSampled(set, 1, primitiveView);
             ctx.writeStorageBuffer(set, 2,
                 meshMaterialsBuffer, meshMaterials.descriptor.size);
             ctx.writeStorageBuffer(set, 3,
                 ctx.frameGraphProvider->buffer(output.native), output.descriptor.size);
             ctx.writeStorageBuffer(set, 4, validationBuffer, sizeof(std::uint32_t));
             ctx.writeStorageBuffer(set, 5, gpu->meshlets.buffer,
                 static_cast<VkDeviceSize>(asset->meshlets.size() *
                     sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
             ctx.writeStorageBuffer(set, 6, gpu->geometryPagePool.buffer,
                 gpu->geometryPagePool.size);
             ctx.writeStorageBuffer(set, 7, gpu->pageTable.buffer,
                 gpu->pageTable.size);
             ctx.writeStorageBuffer(set, 8, gpu->pageInfo.buffer,
                 sizeof(VulkanSceneResources::VirtualGeometryGpuPageInfo));
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->materialClassifyPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->materialClassifyPipeline.layout(), 0, 1, &set, 0, nullptr);
            struct alignas(16) ClassificationConstants
            {
                std::uint32_t width, height, instanceCount, materialCount;
                std::uint32_t meshletCount, meshletVertexCount, indexCount, expectedMeshId;
            } constants{ctx.width, ctx.height,
                static_cast<std::uint32_t>(ctx.packet->instances.size()),
                ctx.sceneResources->materialCount(), 0u, 0u, 0u, 0u};
            // The descriptor at binding 5 is the same asset selected above;
            // do not derive the count from an unrelated first packet entry.
            constants.meshletCount = static_cast<std::uint32_t>(asset->meshlets.size());
            constants.meshletVertexCount =
                static_cast<std::uint32_t>(asset->meshletVertices.size());
            constants.indexCount = static_cast<std::uint32_t>(asset->indices.size());
            constants.expectedMeshId = meshId;
            static_assert(sizeof(ClassificationConstants) == 32,
                "Virtual Geometry classification push constants must match material_classify.comp.hlsl");
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->materialClassifyPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(ctx.commandBuffer(), (ctx.width + 7u) / 8u, (ctx.height + 7u) / 8u, 1);
            VkBufferMemoryBarrier2 classifyBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            classifyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            classifyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            classifyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            classifyBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            classifyBarrier.buffer = outputBuffer;
            classifyBarrier.size = output.descriptor.size;
            VkDependencyInfo classifyDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            classifyDependency.bufferMemoryBarrierCount = 1u;
            classifyDependency.pBufferMemoryBarriers = &classifyBarrier;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &classifyDependency);
            VkBufferMemoryBarrier2 validationReadbackBarrier{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            validationReadbackBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            validationReadbackBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            validationReadbackBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            validationReadbackBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            validationReadbackBarrier.buffer = validationBuffer;
            validationReadbackBarrier.size = sizeof(std::uint32_t);
            VkDependencyInfo validationReadbackDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            validationReadbackDependency.bufferMemoryBarrierCount = 1u;
            validationReadbackDependency.pBufferMemoryBarriers = &validationReadbackBarrier;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &validationReadbackDependency);
            if (ctx.debugReadbacks != nullptr &&
                ctx.currentFrame < ctx.debugReadbacks->virtualGeometryReadbacks.size())
            {
                const VkBuffer readback =
                    ctx.debugReadbacks->virtualGeometryReadbacks[ctx.currentFrame].buffer;
                if (readback != VK_NULL_HANDLE)
                {
                    const VkBufferCopy copy{0, sizeof(std::uint32_t) * 2u,
                        sizeof(std::uint32_t)};
                    vkCmdCopyBuffer(ctx.commandBuffer(), validationBuffer, readback, 1, &copy);
                }
            }
        });

    graph.addPass<Graph::FrameGraph::Empty>("M5 compute shading",
        [passCtx](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(passCtx->visibility, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->visibilityPrimitive, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->visibilityBarycentrics, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->materialClassification, Graph::ResourceUsage::Storage);
            builder.read(passCtx->virtualTransforms, Graph::ResourceUsage::Storage);
            builder.read(passCtx->irradiance, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->prefiltered, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->brdfLut, Graph::ResourceUsage::Sampled);
            builder.read(passCtx->lightBuffer, Graph::ResourceUsage::Storage);
            // Virtual shading reconstructs both HDR and camera motion from
            // the visibility buffer, keeping the established TAA chain live.
            // Transfer usage remains available for deterministic failure
            // clears before a compute dispatch can be recorded.
            passCtx->virtualShadingMotion = builder.write(passCtx->motion,
                Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
             passCtx->virtualShadingHdr = builder.write(passCtx->hdr,
                 Graph::ResourceUsage::Storage | Graph::ResourceUsage::TransferDestination);
            passCtx->motion = passCtx->virtualShadingMotion;
            passCtx->hdr = passCtx->virtualShadingHdr;
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.frameGraphProvider == nullptr)
                return;
             const auto& image = resources.getTexture(ctx.visibility);
             const auto& primitive = resources.getTexture(ctx.visibilityPrimitive);
             const auto& barycentric = resources.getTexture(ctx.visibilityBarycentrics);
             const auto& ids = resources.get<Graph::FrameGraphBuffer>(ctx.materialClassification);
             const auto& output = resources.getTexture(ctx.virtualShadingHdr);
             const auto& irradiance = resources.getTexture(ctx.irradiance);
             const auto& prefiltered = resources.getTexture(ctx.prefiltered);
            const auto& brdf = resources.getTexture(ctx.brdfLut);
            const auto& lights = resources.get<Graph::FrameGraphBuffer>(ctx.lightBuffer);
            const auto& motion = resources.getTexture(ctx.virtualShadingMotion);
            const VkImage hdrImage = ctx.frameGraphProvider->image(output.native);
            const VkImage motionImage = ctx.frameGraphProvider->image(motion.native);
            if (hdrImage == VK_NULL_HANDLE || motionImage == VK_NULL_HANDLE)
                return;

            // Keep the downstream TAA/tonemap chain deterministic even when
            // a virtual-geometry pipeline or transient descriptor is missing.
            // The pass owns both images in the virtual path, so every early
            // exit after this point must leave them in sampled-read layout.
            const auto clearMotion = [&]()
            {
                ctx.transitionImage(motionImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkClearColorValue zeroMotion{{0.0f, 0.0f, 0.0f, 0.0f}};
                const VkImageSubresourceRange motionRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(ctx.commandBuffer(), motionImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zeroMotion, 1, &motionRange);
                ctx.transitionImage(motionImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            };
            const auto clearOutput = [&]()
            {
                if (ctx.virtualVisibilityValid != nullptr)
                    *ctx.virtualVisibilityValid = false;
                const VkClearColorValue clear{{0.012f, 0.018f, 0.028f, 1.0f}};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vkCmdClearColorImage(ctx.commandBuffer(), hdrImage, VK_IMAGE_LAYOUT_GENERAL,
                    &clear, 1, &range);
                ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            };
            clearMotion();
            if (ctx.pipelines == nullptr || ctx.gpuSceneBuffers == nullptr ||
                ctx.sceneResources == nullptr || ctx.gpuAllocator == nullptr ||
                ctx.packet == nullptr ||
                ctx.linearSampler == VK_NULL_HANDLE ||
                ctx.pipelines->computeShadingPipeline.computePipeline() == VK_NULL_HANDLE)
            {
                clearOutput();
                return;
            }
             if (ctx.virtualVisibilityValid == nullptr || !*ctx.virtualVisibilityValid)
             {
                 clearOutput();
                 return;
             }
             // Virtual Geometry bypasses deferred lighting, so it owns the
             // one-time procedural IBL initialization normally performed by
             // that pass.
             if (ctx.iblInitialized != nullptr && !*ctx.iblInitialized)
             {
                 const auto& irradianceImage = ctx.frameGraphProvider->image(irradiance.native);
                 const auto& prefilteredImage = ctx.frameGraphProvider->image(prefiltered.native);
                 const auto& brdfImage = ctx.frameGraphProvider->image(brdf.native);
                 if (irradianceImage == VK_NULL_HANDLE || prefilteredImage == VK_NULL_HANDLE ||
                     brdfImage == VK_NULL_HANDLE)
                 {
                     ctx.setError("Virtual Geometry IBL image allocation failed");
                     clearOutput();
                     return;
                 }
                 const bool imageInitialized = ctx.iblImageInitialized != nullptr &&
                     *ctx.iblImageInitialized;
                 const auto beginUpload = [&](VkImage image, std::uint32_t layers)
                 {
                     ctx.transitionImage(image,
                         imageInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         imageInitialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                           : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         imageInitialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                           : VK_ACCESS_2_NONE,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, 0, layers);
                 };
                 beginUpload(irradianceImage, 6);
                 beginUpload(prefilteredImage, 6);
                 beginUpload(brdfImage, 1);
                 static const ProceduralIblData proceduralIbl = createProceduralIbl();
                 const auto a = ctx.recordImageUpload(irradianceImage,
                     proceduralIbl.irradiance, proceduralIbl.irradianceCopies);
                 const auto b = ctx.recordImageUpload(prefilteredImage,
                     proceduralIbl.prefiltered, proceduralIbl.prefilteredCopies);
                 const auto c = ctx.recordImageUpload(brdfImage,
                     proceduralIbl.brdf, proceduralIbl.brdfCopies);
                 if (!a || !b || !c)
                 {
                     ctx.setError("Virtual Geometry procedural IBL upload failed");
                     ctx.transitionImage(irradianceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                     ctx.transitionImage(prefilteredImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                     ctx.transitionImage(brdfImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                     if (ctx.iblImageInitialized != nullptr)
                         *ctx.iblImageInitialized = false;
                     if (ctx.iblInitialized != nullptr)
                         *ctx.iblInitialized = false;
                     clearOutput();
                     return;
                 }
                 ctx.transitionImage(irradianceImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                 ctx.transitionImage(prefilteredImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                 ctx.transitionImage(brdfImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                 if (ctx.iblImageInitialized != nullptr)
                     *ctx.iblImageInitialized = true;
                 *ctx.iblInitialized = true;
             }
            ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const auto finishWithBackground = [&]()
            {
                const VkClearColorValue clear{{0.012f, 0.018f, 0.028f, 1.0f}};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(ctx.commandBuffer(), hdrImage, VK_IMAGE_LAYOUT_GENERAL,
                    &clear, 1, &range);
                ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            };

             const auto selection = selectVirtualGeometry(ctx);
             const auto* asset = selection.asset;
             const auto* instance = selection.instance;
             const std::uint32_t meshId = selection.meshId;
             const auto* gpu = instance == nullptr ? nullptr :
                 ctx.sceneResources->virtualGeometryBuffersDense(meshId);
             if (!selection || gpu == nullptr)
            {
                finishWithBackground();
                return;
            }

            const VkImageView visibilityView = ctx.frameGraphProvider->view(image.native);
            const VkImageView primitiveView = ctx.frameGraphProvider->view(primitive.native);
            const VkImageView barycentricView = ctx.frameGraphProvider->view(barycentric.native);
            const VkImageView hdrView = ctx.frameGraphProvider->view(output.native);
            const VkImageView irradianceView = ctx.frameGraphProvider->view(irradiance.native);
            const VkImageView prefilteredView = ctx.frameGraphProvider->view(prefiltered.native);
            const VkImageView brdfView = ctx.frameGraphProvider->view(brdf.native);
            const VkImageView motionView = ctx.frameGraphProvider->view(motion.native);
            const VkBuffer materialBuffer = ctx.gpuSceneBuffers->materialBuffer();
            const VkBuffer materialIdsBuffer = ctx.frameGraphProvider->buffer(ids.native);
            const VkBuffer lightBuffer = ctx.frameGraphProvider->buffer(lights.native);
            const auto& transforms = resources.get<Graph::FrameGraphBuffer>(ctx.virtualTransforms);
            const VkBuffer transformBuffer = ctx.frameGraphProvider->buffer(transforms.native);
            const bool validViews = visibilityView != VK_NULL_HANDLE &&
                primitiveView != VK_NULL_HANDLE && barycentricView != VK_NULL_HANDLE &&
                hdrView != VK_NULL_HANDLE && irradianceView != VK_NULL_HANDLE &&
                prefilteredView != VK_NULL_HANDLE && brdfView != VK_NULL_HANDLE &&
                motionView != VK_NULL_HANDLE;
            const bool validBuffers = gpu->meshlets.buffer != VK_NULL_HANDLE &&
                gpu->geometryPagePool.buffer != VK_NULL_HANDLE &&
                gpu->pageTable.buffer != VK_NULL_HANDLE &&
                gpu->pageInfo.buffer != VK_NULL_HANDLE &&
                materialBuffer != VK_NULL_HANDLE && materialIdsBuffer != VK_NULL_HANDLE &&
                lightBuffer != VK_NULL_HANDLE &&
                transformBuffer != VK_NULL_HANDLE;
            if (!validViews || !validBuffers)
            {
                finishWithBackground();
                return;
            }

            const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->computeShadingLayout);
            if (set == VK_NULL_HANDLE)
            {
                finishWithBackground();
                return;
            }
            ctx.writeSampled(set, 0, visibilityView);
            ctx.writeSampled(set, 1, primitiveView);
            ctx.writeSampled(set, 2, barycentricView);
            ctx.writeStorageBuffer(set, 3, materialIdsBuffer, ids.descriptor.size);
            ctx.writeStorage(set, 4, hdrView);
             ctx.writeStorageBuffer(set, 5, gpu->meshlets.buffer,
                 static_cast<VkDeviceSize>(asset->meshlets.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
             ctx.writeStorageBuffer(set, 6, gpu->geometryPagePool.buffer,
                 gpu->geometryPagePool.size);
             ctx.writeStorageBuffer(set, 7, gpu->pageTable.buffer,
                 gpu->pageTable.size);
             ctx.writeSampled(set, 8, irradianceView);
             ctx.writeSampled(set, 9, prefilteredView);
             ctx.writeSampled(set, 11, brdfView);
             const std::uint32_t materialCount = std::max(1u,
                 ctx.sceneResources->materialCount());
             ctx.writeStorageBuffer(set, 12, materialBuffer,
                 static_cast<VkDeviceSize>(materialCount) *
                     sizeof(Halcyon::Renderer::Scene::MaterialGpuData));
             if (!ctx.packet->lights.empty())
             {
                 const auto* allocation = ctx.frameGraphProvider->nativeBufferAllocation(lights.native);
                 if (allocation == nullptr)
                 {
                     finishWithBackground();
                     return;
                 }
                 const auto bytes = std::span<const std::byte>{
                     reinterpret_cast<const std::byte*>(ctx.packet->lights.data()),
                     ctx.packet->lights.size_bytes()};
                 const auto upload = ctx.gpuAllocator->writeBuffer(*allocation, bytes);
                 if (!upload)
                 {
                     ctx.setError("Virtual Geometry light-buffer upload failed");
                     finishWithBackground();
                     return;
                 }
             }
             ctx.writeStorageBuffer(set, 13, lightBuffer, lights.descriptor.size);
             ctx.writeStorageBuffer(set, 14, transformBuffer, transforms.descriptor.size);
             ctx.writeStorage(set, 15, motionView);
             ctx.writeStorageBuffer(set, 16, gpu->pageInfo.buffer,
                 sizeof(VulkanSceneResources::VirtualGeometryGpuPageInfo));
             ctx.writeSampler(set);
            ctx.transitionImage(motionImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->computeShadingPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->computeShadingPipeline.layout(), 0, 1, &set, 0, nullptr);
             struct alignas(16) ShadingConstants
             {
                 std::uint32_t width, height, instanceCount, meshletCount;
                 std::uint32_t vertexCount, indexCount, materialCount, lightCount;
                 glm::vec4 cameraAndAmbient;
                 glm::mat4 previousViewProjection;
                 glm::vec4 fallbackLight;
             } constants{};
             static_assert(sizeof(ShadingConstants) == 128,
                 "Virtual Geometry shading push constants must match compute_shading.comp.hlsl");
             constants.width = ctx.width;
             constants.height = ctx.height;
             constants.instanceCount = static_cast<std::uint32_t>(ctx.packet->instances.size());
             constants.meshletCount = static_cast<std::uint32_t>(asset->meshlets.size());
             constants.vertexCount = static_cast<std::uint32_t>(asset->vertices.size());
             constants.indexCount = static_cast<std::uint32_t>(asset->indices.size());
             constants.materialCount = materialCount;
             constants.lightCount = std::min<std::uint32_t>(
                 static_cast<std::uint32_t>(ctx.packet->lights.size()), 1024u);
             constants.cameraAndAmbient = glm::vec4{
                 glm::vec3(ctx.packet->camera.positionAndNear), 1.0f};
             constants.previousViewProjection = ctx.previousPacketValid
                 ? ctx.previousViewProjection : ctx.packet->camera.viewProjection;
             constants.fallbackLight = glm::vec4{0.35f, -0.8f, 0.25f, 2.0f};
             for (const auto& light : ctx.packet->lights)
             {
                 if (light.directionAndType[3] > 0.5f && light.directionAndType[3] < 1.5f)
                 {
                     constants.fallbackLight = glm::vec4{
                         light.directionAndType[0], light.directionAndType[1],
                         light.directionAndType[2], light.colorAndIntensity[3]};
                     break;
                 }
             }
             vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->computeShadingPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(ctx.commandBuffer(), (ctx.width + 7u) / 8u, (ctx.height + 7u) / 8u, 1);
            ctx.transitionImage(motionImage, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (ctx.debugReadbacks != nullptr &&
                ctx.currentFrame < ctx.debugReadbacks->virtualGeometryValid.size())
            {
                ctx.debugReadbacks->virtualGeometryValid[ctx.currentFrame] = true;
            }
        });
}

} // namespace Halcyon::Vulkan
