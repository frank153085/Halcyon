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
        [visibleMeshlets, visibleMeshletCount, indirect](Graph::FrameGraph::Builder& builder,
            Graph::FrameGraph::Empty&)
        {
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

    // Visibility rasterization consumes the GPU-generated indexed indirect
    // stream.  The vertex shader reconstructs each triangle from the virtual
    // geometry tables, so no CPU draw loop or per-meshlet descriptor switch is
    // involved in the M5 path.
    const auto depth = ctx.depth;
    graph.addPass<Graph::FrameGraph::Empty>("M5 visibility rasterization",
        [passCtx, visibility, depth, indirect](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(indirect, Graph::ResourceUsage::Indirect);
            passCtx->visibility = builder.write(visibility, Graph::ResourceUsage::ColorAttachment);
            passCtx->depth = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = passCtx->visibility;
            descriptor.attachments.depth = passCtx->depth;
            descriptor.viewport.width = passCtx->width;
            descriptor.viewport.height = passCtx->height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0 |
                Graph::FrameGraphAttachmentFlags::Depth;
            builder.declareRenderPass("M5 visibility rasterization", descriptor);
            builder.sideEffect();
        },
        [passCtx, indirect](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.pipelines == nullptr || ctx.sceneResources == nullptr || ctx.packet == nullptr ||
                ctx.pipelines->visibilityPipeline.pipeline() == VK_NULL_HANDLE)
                return;
            const auto info = resources.getRenderPassInfo(0);
            const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
            if (target == nullptr) return;
            const Halcyon::Renderer::Scene::VirtualGeometryAsset* asset = nullptr;
            const Halcyon::Renderer::Scene::InstanceData* instance = nullptr;
            std::uint32_t meshId = 0;
            for (const auto& candidate : ctx.packet->instances)
            {
                asset = ctx.sceneResources->virtualGeometryDense(candidate.meshId);
                if (asset != nullptr) { instance = &candidate; meshId = candidate.meshId; break; }
            }
            const auto* gpu = instance == nullptr ? nullptr :
                ctx.sceneResources->virtualGeometryBuffersDense(meshId);
            if (asset == nullptr || gpu == nullptr || instance == nullptr || gpu->indices.buffer == VK_NULL_HANDLE)
                return;
            const auto& visibilityResource = resources.getTexture(passCtx->visibility);
            const auto& depthResource = resources.getTexture(passCtx->depth);
            const VkImage visibilityImage = ctx.frameGraphProvider->image(visibilityResource.native);
            const VkImage depthImage = ctx.frameGraphProvider->image(depthResource.native);
            ctx.transitionImage(visibilityImage, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            ctx.transitionImage(depthImage, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = target->views[0]; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color.uint32[0] = 0u;
            VkRenderingAttachmentInfo z{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            z.imageView = target->depthView; z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            z.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; z.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            z.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = ctx.swapchainExtent; rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color; rendering.pDepthAttachment = &z;
            vkCmdBeginRendering(ctx.commandBuffer(), &rendering);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.pipelines->visibilityPipeline.pipeline());
            const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->visibilityLayout);
            if (set != VK_NULL_HANDLE)
            {
                ctx.writeStorageBuffer(set, 0, gpu->meshlets.buffer,
                    asset->meshlets.size() * sizeof(VulkanSceneResources::VirtualGeometryGpuMeshlet));
                ctx.writeStorageBuffer(set, 1, gpu->meshletVertices.buffer,
                    asset->meshletVertices.size() * sizeof(std::uint32_t));
                ctx.writeStorageBuffer(set, 2, gpu->vertices.buffer,
                    asset->vertices.size() * sizeof(asset->vertices[0]));
                ctx.writeStorageBuffer(set, 3, gpu->indices.buffer,
                    asset->indices.size() * sizeof(std::uint32_t));
                vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                    ctx.pipelines->visibilityPipeline.layout(), 0, 1, &set, 0, nullptr);
            }
            struct alignas(16) VisibilityConstants
            {
                glm::mat4 viewProjection{1.0f};
                glm::mat4 model{1.0f};
                glm::mat4 previousModel{1.0f};
                glm::uvec4 ids{0u};
            } constants{};
            constants.viewProjection = ctx.packet->camera.viewProjection;
            constants.model = glm::make_mat4(instance->transform.data());
            constants.ids = glm::uvec4{0u, 0u, 0u, 0u};
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->visibilityPipeline.layout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(ctx.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, ctx.swapchainExtent};
            vkCmdSetViewport(ctx.commandBuffer(), 0, 1, &viewport);
            vkCmdSetScissor(ctx.commandBuffer(), 0, 1, &scissor);
            vkCmdBindIndexBuffer(ctx.commandBuffer(), gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            const auto& commands = resources.get<Graph::FrameGraphBuffer>(indirect);
            const auto& count = resources.get<Graph::FrameGraphBuffer>(ctx.visibleMeshletCount);
            vkCmdDrawIndexedIndirectCount(ctx.commandBuffer(),
                ctx.frameGraphProvider->buffer(commands.native), 0,
                ctx.frameGraphProvider->buffer(count.native), 0, 131072u,
                sizeof(VkDrawIndexedIndirectCommand));
            // Keep a one-command fallback for drivers that expose indirect
            // count but return zero while the counter is being initialized.
            vkCmdDrawIndexedIndirect(ctx.commandBuffer(),
                ctx.frameGraphProvider->buffer(commands.native), 0, 1,
                sizeof(VkDrawIndexedIndirectCommand));
            vkCmdEndRendering(ctx.commandBuffer());
            ctx.transitionImage(visibilityImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });

    graph.addPass<Graph::FrameGraph::Empty>("M5 material classification",
        [passCtx, visibility, classification](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(visibility, Graph::ResourceUsage::Sampled);
            passCtx->materialClassification = builder.write(classification, Graph::ResourceUsage::Storage);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.pipelines == nullptr || ctx.pipelines->materialClassifyPipeline.computePipeline() == VK_NULL_HANDLE) return;
            const auto& image = resources.getTexture(ctx.visibility);
            const auto& output = resources.get<Graph::FrameGraphBuffer>(ctx.materialClassification);
            const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->materialClassifyLayout);
            if (set == VK_NULL_HANDLE) return;
            ctx.writeSampled(set, 0, ctx.frameGraphProvider->view(image.native));
            ctx.writeStorageBuffer(set, 1, ctx.frameGraphProvider->buffer(output.native), output.descriptor.size);
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->materialClassifyPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->materialClassifyPipeline.layout(), 0, 1, &set, 0, nullptr);
            struct { std::uint32_t width, height, pad0, pad1; } constants{ctx.width, ctx.height, 0, 0};
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->materialClassifyPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(ctx.commandBuffer(), (ctx.width + 7u) / 8u, (ctx.height + 7u) / 8u, 1);
        });

    graph.addPass<Graph::FrameGraph::Empty>("M5 compute shading",
        [passCtx, visibility, classification](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(visibility, Graph::ResourceUsage::Sampled);
            builder.read(classification, Graph::ResourceUsage::Storage);
            passCtx->hdr = builder.write(passCtx->hdr, Graph::ResourceUsage::Storage);
            builder.sideEffect();
        },
        [passCtx](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            auto& ctx = *passCtx;
            if (ctx.pipelines == nullptr || ctx.pipelines->computeShadingPipeline.computePipeline() == VK_NULL_HANDLE) return;
            const auto& image = resources.getTexture(ctx.visibility);
            const auto& ids = resources.get<Graph::FrameGraphBuffer>(ctx.materialClassification);
            const auto& output = resources.getTexture(ctx.hdr);
            const VkImage hdrImage = ctx.frameGraphProvider->image(output.native);
            ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            const VkDescriptorSet set = ctx.allocateSet(ctx.pipelines->computeShadingLayout);
            if (set == VK_NULL_HANDLE) return;
            ctx.writeSampled(set, 0, ctx.frameGraphProvider->view(image.native));
            ctx.writeStorageBuffer(set, 1, ctx.frameGraphProvider->buffer(ids.native), ids.descriptor.size);
            ctx.writeStorage(set, 2, ctx.frameGraphProvider->view(output.native));
            vkCmdBindPipeline(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->computeShadingPipeline.computePipeline());
            vkCmdBindDescriptorSets(ctx.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, ctx.pipelines->computeShadingPipeline.layout(), 0, 1, &set, 0, nullptr);
            struct { std::uint32_t width, height, pixelCount, pad; } constants{ctx.width, ctx.height, ctx.width * ctx.height, 0};
            vkCmdPushConstants(ctx.commandBuffer(), ctx.pipelines->computeShadingPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(ctx.commandBuffer(), (ctx.width + 7u) / 8u, (ctx.height + 7u) / 8u, 1);
            ctx.transitionImage(hdrImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
}

} // namespace Halcyon::Vulkan
