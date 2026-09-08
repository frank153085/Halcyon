#include "VirtualGeometryPass.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Halcyon::Vulkan
{

void addVirtualGeometryPasses(Graph::FrameGraph& graph, FramePassContext& ctx)
{
    if (ctx.config == nullptr || ctx.config->renderPath !=
            Halcyon::Renderer::Scene::RenderPathMode::VirtualGeometryIndexed)
        return;
    auto* passCtx = &ctx;
    const auto visibility = ctx.visibility;
    const auto classification = ctx.materialClassification;
    const auto visibleMeshlets = ctx.visibleMeshlets;
    const auto visibleMeshletCount = ctx.visibleMeshletCount;
    const auto indirect = ctx.meshletIndirect;
    graph.addPass<Graph::FrameGraph::Empty>("M5 meshlet cull and indirect",
        [visibility, classification, visibleMeshlets, visibleMeshletCount, indirect](Graph::FrameGraph::Builder& builder,
            Graph::FrameGraph::Empty&)
        {
            builder.write(visibility, Graph::ResourceUsage::Storage);
            builder.write(classification, Graph::ResourceUsage::Storage);
            builder.write(visibleMeshlets, Graph::ResourceUsage::Storage);
            builder.write(visibleMeshletCount, Graph::ResourceUsage::Storage);
            builder.write(indirect, Graph::ResourceUsage::Storage | Graph::ResourceUsage::Indirect);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            const auto& visible = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshlets);
            const auto& visibleCount = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshletCount);
            const auto& commands = resources.get<Graph::FrameGraphBuffer>(ctx.meshletIndirect);
            const VkBuffer visibleBuffer = ctx.frameGraphProvider->buffer(visible.native);
            const VkBuffer visibleCountBuffer = ctx.frameGraphProvider->buffer(visibleCount.native);
            const VkBuffer commandBuffer = ctx.frameGraphProvider->buffer(commands.native);
            if (visibleBuffer == VK_NULL_HANDLE || visibleCountBuffer == VK_NULL_HANDLE ||
                commandBuffer == VK_NULL_HANDLE || ctx.pipelines == nullptr ||
                ctx.sceneResources == nullptr || ctx.packet == nullptr)
                return;

            const Halcyon::Renderer::Scene::VirtualGeometryAsset* asset = nullptr;
            const Halcyon::Renderer::Scene::InstanceData* virtualInstance = nullptr;
            std::uint32_t meshId = 0;
            for (const auto& instance : ctx.packet->instances)
            {
                asset = ctx.sceneResources->virtualGeometryDense(instance.meshId);
                if (asset != nullptr)
                {
                    virtualInstance = &instance;
                    meshId = instance.meshId;
                    break;
                }
            }
            if (asset == nullptr || virtualInstance == nullptr || asset->meshlets.empty() ||
                ctx.pipelines->meshletCullPipeline.computePipeline() == VK_NULL_HANDLE ||
                ctx.pipelines->meshletIndirectPipeline.computePipeline() == VK_NULL_HANDLE)
                return;

            constexpr std::uint32_t kVisibleCapacity = 131072u;
            const std::uint32_t meshletCount = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(asset->meshlets.size()), kVisibleCapacity);
            vkCmdFillBuffer(ctx.commandBuffer(), visibleCountBuffer, 0, sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(ctx.commandBuffer(), visibleBuffer, 0, VK_WHOLE_SIZE, 0);
            vkCmdFillBuffer(ctx.commandBuffer(), commandBuffer, 0, VK_WHOLE_SIZE, 0);
            VkBufferMemoryBarrier2 resetBarriers[3]{};
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
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 3;
            dependency.pBufferMemoryBarriers = resetBarriers;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);

            const auto* gpu = ctx.sceneResources->virtualGeometryBuffersDense(meshId);
            if (gpu == nullptr || gpu->meshlets.buffer == VK_NULL_HANDLE)
                return;
            const VkDescriptorSet cullSet = ctx.allocateSet(ctx.pipelines->meshletCullLayout);
            if (cullSet == VK_NULL_HANDLE) return;
            ctx.writeStorageBuffer(cullSet, 0, gpu->meshlets.buffer,
                static_cast<VkDeviceSize>(asset->meshlets.size() *
                    sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
            ctx.writeStorageBuffer(cullSet, 1, visibleBuffer, visible.descriptor.size);
            ctx.writeStorageBuffer(cullSet, 2, visibleCountBuffer, sizeof(std::uint32_t));
            struct alignas(16) CullConstants
            {
                glm::vec4 planes[6];
                std::uint32_t meshletCount, instanceIndex, lod, pad;
            } constants{};
            const glm::mat4& vp = ctx.packet->camera.viewProjection;
            const glm::vec4 rows[4] = {{vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
                {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
                {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
                {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
            constants.planes[0] = rows[3] + rows[0]; constants.planes[1] = rows[3] - rows[0];
            constants.planes[2] = rows[3] + rows[1]; constants.planes[3] = rows[3] - rows[1];
            constants.planes[4] = rows[3] + rows[2]; constants.planes[5] = rows[3] - rows[2];
            const glm::mat4 model = glm::make_mat4(virtualInstance->transform.data());
            const glm::mat4 inverseTranspose = glm::transpose(glm::inverse(model));
            for (auto& plane : constants.planes)
            {
                plane = inverseTranspose * plane;
                const float length = glm::length(glm::vec3(plane));
                if (length > 1.0e-6f) plane /= length;
            }
            constants.meshletCount = meshletCount;
            constants.instanceIndex = 0;
            constants.lod = 0;
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletCullPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletCullPipeline.layout(), 0, 1, &cullSet, 0, nullptr);
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->meshletCullPipeline.layout(),
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(ctx.commandBuffer(), (meshletCount + 63u) / 64u, 1, 1);

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

            const VkDescriptorSet indirectSet = ctx.allocateSet(ctx.pipelines->meshletIndirectLayout);
            if (indirectSet == VK_NULL_HANDLE) return;
            ctx.writeStorageBuffer(indirectSet, 0, visibleBuffer, visible.descriptor.size);
            ctx.writeStorageBuffer(indirectSet, 1, gpu->meshlets.buffer,
                static_cast<VkDeviceSize>(asset->meshlets.size() *
                    sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet)));
            ctx.writeStorageBuffer(indirectSet, 2, visibleCountBuffer, sizeof(std::uint32_t));
            ctx.writeStorageBuffer(indirectSet, 3, commandBuffer, commands.descriptor.size);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletIndirectPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                ctx.pipelines->meshletIndirectPipeline.layout(), 0, 1, &indirectSet, 0, nullptr);
            vkCmdDispatch(ctx.commandBuffer(), (meshletCount + 63u) / 64u, 1, 1);

            VkBufferMemoryBarrier2 indirectBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            indirectBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            indirectBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            indirectBarrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            indirectBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            indirectBarrier.buffer = commandBuffer;
            indirectBarrier.size = VK_WHOLE_SIZE;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &indirectBarrier;
            vkCmdPipelineBarrier2(ctx.commandBuffer(), &dependency);
        });
}

} // namespace Halcyon::Vulkan
