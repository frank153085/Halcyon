#include "FrameRecorder.h"

#include <algorithm>
#include <array>
#include <glm/gtc/type_ptr.hpp>
#include <vector>

namespace Halcyon::Vulkan
{

void FrameRecorder::recordMeshGroupedIndirectBuild(
    VkCommandBuffer commandBuffer,
    const VulkanPipeline& indirectBuildPipeline,
    const MeshGroupedIndirectBuildDesc& desc) const
{
    if (commandBuffer == VK_NULL_HANDLE || desc.set == VK_NULL_HANDLE ||
        indirectBuildPipeline.computePipeline() == VK_NULL_HANDLE)
    {
        return;
    }

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    if (desc.resetMeshHeads && desc.meshHeads != VK_NULL_HANDLE)
    {
        VkBufferMemoryBarrier2 meshHeadBeforeReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        meshHeadBeforeReset.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        meshHeadBeforeReset.srcAccessMask =
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        meshHeadBeforeReset.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        meshHeadBeforeReset.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        meshHeadBeforeReset.buffer = desc.meshHeads;
        meshHeadBeforeReset.size = VK_WHOLE_SIZE;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &meshHeadBeforeReset;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        vkCmdFillBuffer(
            commandBuffer, desc.meshHeads, 0, desc.meshHeadsBytes, 0xffffffffu);
        VkBufferMemoryBarrier2 meshHeadAfterReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        meshHeadAfterReset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        meshHeadAfterReset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        meshHeadAfterReset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        meshHeadAfterReset.dstAccessMask =
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        meshHeadAfterReset.buffer = desc.meshHeads;
        meshHeadAfterReset.size = VK_WHOLE_SIZE;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &meshHeadAfterReset;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    vkCmdBindPipeline(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, indirectBuildPipeline.computePipeline());
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        indirectBuildPipeline.layout(),
        0,
        1,
        &desc.set,
        0,
        nullptr);
    struct IndirectConstants
    {
        std::uint32_t instanceCount;
        std::uint32_t meshCount;
        std::uint32_t mode;
        std::uint32_t reserved;
    } indirect{};
    indirect.instanceCount = desc.instanceCount;
    indirect.meshCount = desc.meshCount;
    indirect.mode = 0;
    vkCmdPushConstants(
        commandBuffer,
        indirectBuildPipeline.layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(indirect),
        &indirect);
    vkCmdDispatch(commandBuffer, (std::max(1u, indirect.instanceCount) + 63u) / 64u, 1, 1);

    VkBufferMemoryBarrier2 groupBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    groupBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    groupBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    groupBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    groupBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    std::array<VkBufferMemoryBarrier2, 2> groupBarriers{groupBarrier, groupBarrier};
    groupBarriers[0].buffer = desc.meshHeads;
    groupBarriers[0].size = VK_WHOLE_SIZE;
    groupBarriers[1].buffer = desc.meshNext;
    groupBarriers[1].size = VK_WHOLE_SIZE;
    dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(groupBarriers.size());
    dependency.pBufferMemoryBarriers = groupBarriers.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    vkCmdFillBuffer(commandBuffer, desc.indirectCount, 0, sizeof(std::uint32_t), 0);
    vkCmdFillBuffer(commandBuffer, desc.groupedCount, 0, sizeof(std::uint32_t), 0);
    VkBufferMemoryBarrier2 fillBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    fillBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    fillBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fillBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    std::array<VkBufferMemoryBarrier2, 2> buildReset{fillBarrier, fillBarrier};
    buildReset[0].buffer = desc.indirectCount;
    buildReset[0].size = sizeof(std::uint32_t);
    buildReset[1].buffer = desc.groupedCount;
    buildReset[1].size = sizeof(std::uint32_t);
    dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buildReset.size());
    dependency.pBufferMemoryBarriers = buildReset.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    indirect.mode = 1;
    vkCmdPushConstants(
        commandBuffer,
        indirectBuildPipeline.layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(indirect),
        &indirect);
    vkCmdDispatch(
        commandBuffer, (std::max(1u, indirect.meshCount) + 63u) / 64u, 1, 1);

    VkBufferMemoryBarrier2 drawBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    drawBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    drawBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    drawBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    drawBarrier.dstAccessMask =
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    std::array<VkBufferMemoryBarrier2, 4> drawBarriers{
        drawBarrier, drawBarrier, drawBarrier, drawBarrier};
    drawBarriers[0].buffer = desc.indirectCommands;
    drawBarriers[0].size = VK_WHOLE_SIZE;
    drawBarriers[1].buffer = desc.indirectCount;
    drawBarriers[1].size = sizeof(std::uint32_t);
    drawBarriers[2].buffer = desc.groupedVisible;
    drawBarriers[2].size = VK_WHOLE_SIZE;
    drawBarriers[3].buffer = desc.groupedCount;
    drawBarriers[3].size = sizeof(std::uint32_t);
    dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(drawBarriers.size());
    dependency.pBufferMemoryBarriers = drawBarriers.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

namespace
{

using InstanceData = Halcyon::Renderer::Scene::InstanceData;

[[nodiscard]] bool instanceIsTransparent(const InstanceData& instance) noexcept
{
    return (instance.flags & 1u) != 0u;
}

[[nodiscard]] bool instanceIsDoubleSided(const InstanceData& instance) noexcept
{
    return (instance.flags & (1u << 1u)) != 0u;
}

[[nodiscard]] bool instanceCastsShadow(const InstanceData& instance) noexcept
{
    return (instance.flags & (1u << 2u)) != 0u;
}

[[nodiscard]] std::vector<const InstanceData*> collectDrawItems(
    const FramePacket& packet,
    bool sortBackToFront)
{
    std::vector<const InstanceData*> drawItems;
    drawItems.reserve(packet.instances.size());
    for (const auto& instance : packet.instances)
    {
        drawItems.push_back(&instance);
    }
    if (!sortBackToFront)
    {
        return drawItems;
    }
    const glm::vec3 cameraPosition = glm::vec3(packet.camera.positionAndNear);
    std::stable_sort(
        drawItems.begin(),
        drawItems.end(),
        [&](const InstanceData* a, const InstanceData* b)
        {
            const glm::vec3 aPosition{a->transform[12], a->transform[13], a->transform[14]};
            const glm::vec3 bPosition{b->transform[12], b->transform[13], b->transform[14]};
            return glm::dot(aPosition - cameraPosition, aPosition - cameraPosition) >
                   glm::dot(bPosition - cameraPosition, bPosition - cameraPosition);
        });
    return drawItems;
}

} // namespace

void FrameRecorder::bindInstanceMesh(
    VkCommandBuffer commandBuffer,
    const MeshResource& mesh) const
{
    VkBuffer vertex = mesh.vertexBuffer.buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertex, &offset);
    vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
}

void FrameRecorder::drawShadowInstances(
    VkCommandBuffer commandBuffer,
    const FramePacket& packet,
    const glm::mat4& cascadeMatrix,
    const CpuDrawState& draw) const
{
    if (draw.sceneResources == nullptr || draw.pipelines == nullptr)
    {
        return;
    }
    auto& sceneResources = *draw.sceneResources;
    auto& csmDepthPipeline = draw.pipelines->csmDepthPipeline;
    const auto drawItems = collectDrawItems(packet, false);
    for (const InstanceData* instancePointer : drawItems)
    {
        const auto& drawInstance = *instancePointer;
        if (!instanceCastsShadow(drawInstance) || instanceIsTransparent(drawInstance))
        {
            continue;
        }
        const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }
        bindInstanceMesh(commandBuffer, *mesh);
        const glm::mat4 model = glm::make_mat4(drawInstance.transform.data());
        struct CsmConstants
        {
            glm::mat4 lightViewProjection;
            glm::mat4 model;
        } constants{cascadeMatrix, model};
        vkCmdPushConstants(
            commandBuffer,
            csmDepthPipeline.layout(),
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(constants),
            &constants);
        vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
    }
}

void FrameRecorder::drawOpaqueGBufferInstances(
    VkCommandBuffer commandBuffer,
    const FramePacket& packet,
    const CpuDrawState& draw) const
{
    if (draw.sceneResources == nullptr || draw.pipelines == nullptr ||
        draw.materialDescriptorBindCount == nullptr)
    {
        return;
    }
    auto& sceneResources = *draw.sceneResources;
    auto& gbufferPipeline = draw.pipelines->gbufferPipeline;
    auto& gbufferDoubleSidedPipeline = draw.pipelines->gbufferDoubleSidedPipeline;
    const auto& previousInstances = draw.previousInstances != nullptr
        ? *draw.previousInstances
        : std::vector<InstanceData>{};
    const auto drawItems = collectDrawItems(packet, false);
    for (const InstanceData* instancePointer : drawItems)
    {
        const auto& drawInstance = *instancePointer;
        if (instanceIsTransparent(drawInstance))
        {
            continue;
        }
        const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }
        bindInstanceMesh(commandBuffer, *mesh);
        VkPipelineLayout drawLayout = gbufferPipeline.layout();
        if (instanceIsDoubleSided(drawInstance) &&
            gbufferDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
        {
            drawLayout = gbufferDoubleSidedPipeline.layout();
            vkCmdBindPipeline(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                gbufferDoubleSidedPipeline.pipeline());
        }
        else
        {
            vkCmdBindPipeline(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeline.pipeline());
        }
        const VkDescriptorSet material = sceneResources.materialDescriptor(drawInstance.materialId);
        if (material != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &material, 0,
                nullptr);
            ++(*draw.materialDescriptorBindCount);
        }
        const glm::mat4 model = glm::make_mat4(drawInstance.transform.data());
        const std::size_t instanceIndex = packet.instances.data() != nullptr
            ? static_cast<std::size_t>(instancePointer - packet.instances.data())
            : 0u;
        const bool hasPrevious = draw.previousPacketValid &&
            instanceIndex < previousInstances.size() &&
            previousInstances[instanceIndex].meshId == drawInstance.meshId &&
            previousInstances[instanceIndex].materialId == drawInstance.materialId;
        const glm::mat4 previousModel = hasPrevious
            ? glm::make_mat4(previousInstances[instanceIndex].transform.data())
            : model;
        const glm::mat4 previousVp = draw.previousPacketValid
            ? draw.previousViewProjection
            : packet.camera.viewProjection;
        OpaquePushConstants constants{};
        constants.viewProjection = packet.camera.viewProjection;
        constants.previousViewProjection = previousVp;
        constants.model = model;
        constants.previousModel = previousModel;
        vkCmdPushConstants(
            commandBuffer,
            drawLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(constants),
            &constants);
        vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
    }
}

void FrameRecorder::drawTransparentInstances(
    VkCommandBuffer commandBuffer,
    const FramePacket& packet,
    const CpuDrawState& draw) const
{
    if (draw.sceneResources == nullptr || draw.pipelines == nullptr ||
        draw.materialDescriptorBindCount == nullptr)
    {
        return;
    }
    auto& sceneResources = *draw.sceneResources;
    auto& transparentPipeline = draw.pipelines->transparentPipeline;
    auto& transparentDoubleSidedPipeline = draw.pipelines->transparentDoubleSidedPipeline;
    const auto drawItems = collectDrawItems(packet, true);
    for (const InstanceData* instancePointer : drawItems)
    {
        const auto& drawInstance = *instancePointer;
        if (!instanceIsTransparent(drawInstance))
        {
            continue;
        }
        const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }
        bindInstanceMesh(commandBuffer, *mesh);
        VkPipelineLayout drawLayout = transparentPipeline.layout();
        if (instanceIsDoubleSided(drawInstance) &&
            transparentDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
        {
            drawLayout = transparentDoubleSidedPipeline.layout();
            vkCmdBindPipeline(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                transparentDoubleSidedPipeline.pipeline());
        }
        else
        {
            vkCmdBindPipeline(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline.pipeline());
        }
        const VkDescriptorSet material = sceneResources.materialDescriptor(drawInstance.materialId);
        if (material != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &material, 0,
                nullptr);
            ++(*draw.materialDescriptorBindCount);
        }
        const glm::mat4 model = glm::make_mat4(drawInstance.transform.data());
        TransparentPushConstants constants{};
        constants.viewProjection = packet.camera.viewProjection;
        constants.cameraPosition = glm::vec4{glm::vec3(packet.camera.positionAndNear), 1.0f};
        constants.model = model;
        if (!packet.lights.empty())
        {
            const auto& light = packet.lights.front();
            constants.lightPositionOrDirection = glm::vec4{
                light.directionAndType[3] > 0.5f
                    ? glm::vec3{light.directionAndType[0], light.directionAndType[1],
                          light.directionAndType[2]}
                    : glm::vec3{light.positionAndRadius[0], light.positionAndRadius[1],
                          light.positionAndRadius[2]},
                light.directionAndType[3]};
            constants.lightColorIntensity = glm::vec4{
                light.colorAndIntensity[0],
                light.colorAndIntensity[1],
                light.colorAndIntensity[2],
                light.colorAndIntensity[3]};
            constants.lightParameters = glm::vec4{
                light.positionAndRadius[3], light.spotParams[0], light.spotParams[1], 0.04f};
        }
        vkCmdPushConstants(
            commandBuffer,
            drawLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(constants),
            &constants);
        vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
    }
}

void FrameRecorder::drawGpuDrivenCpuFallback(
    VkCommandBuffer commandBuffer,
    const FramePacket& packet,
    std::uint32_t gpuMaterialId,
    const CpuDrawState& draw) const
{
    if (draw.sceneResources == nullptr || draw.pipelines == nullptr ||
        draw.materialDescriptorBindCount == nullptr)
    {
        return;
    }
    auto& sceneResources = *draw.sceneResources;
    auto& gbufferPipeline = draw.pipelines->gbufferPipeline;
    auto& gbufferDoubleSidedPipeline = draw.pipelines->gbufferDoubleSidedPipeline;
    const auto& previousInstances = draw.previousInstances != nullptr
        ? *draw.previousInstances
        : std::vector<InstanceData>{};
    const auto drawItems = collectDrawItems(packet, false);
    for (const InstanceData* instancePointer : drawItems)
    {
        const auto& drawInstance = *instancePointer;
        if (instanceIsTransparent(drawInstance))
        {
            continue;
        }
        const bool materialMismatch =
            !draw.gpuDrivenBindless && drawInstance.materialId != gpuMaterialId;
        if (!instanceIsDoubleSided(drawInstance) && !materialMismatch)
        {
            continue;
        }
        const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }
        bindInstanceMesh(commandBuffer, *mesh);
        VkPipelineLayout drawLayout = gbufferPipeline.layout();
        if (instanceIsDoubleSided(drawInstance) &&
            gbufferDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
        {
            drawLayout = gbufferDoubleSidedPipeline.layout();
            vkCmdBindPipeline(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                gbufferDoubleSidedPipeline.pipeline());
        }
        else
        {
            vkCmdBindPipeline(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeline.pipeline());
        }
        const VkDescriptorSet material = sceneResources.materialDescriptor(drawInstance.materialId);
        if (material != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &material, 0,
                nullptr);
            ++(*draw.materialDescriptorBindCount);
        }
        const glm::mat4 model = glm::make_mat4(drawInstance.transform.data());
        const std::size_t instanceIndex = packet.instances.data() != nullptr
            ? static_cast<std::size_t>(instancePointer - packet.instances.data())
            : 0u;
        const bool hasPrevious = draw.previousPacketValid &&
            instanceIndex < previousInstances.size() &&
            previousInstances[instanceIndex].meshId == drawInstance.meshId &&
            previousInstances[instanceIndex].materialId == drawInstance.materialId;
        const glm::mat4 previousModel = hasPrevious
            ? glm::make_mat4(previousInstances[instanceIndex].transform.data())
            : model;
        const glm::mat4 previousVp = draw.previousPacketValid
            ? draw.previousViewProjection
            : packet.camera.viewProjection;
        OpaquePushConstants constants{};
        constants.viewProjection = packet.camera.viewProjection;
        constants.previousViewProjection = previousVp;
        constants.model = model;
        constants.previousModel = previousModel;
        vkCmdPushConstants(
            commandBuffer,
            drawLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(constants),
            &constants);
        vkCmdDrawIndexed(commandBuffer, mesh->indexCount, 1, 0, 0, 0);
    }
}

} // namespace Halcyon::Vulkan
