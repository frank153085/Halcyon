#include "FramePassContext.h"

#include "VulkanCommon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <new>
#include <utility>

namespace Halcyon::Vulkan
{

void FramePassContext::setError(std::string message)
{
    if (lastError != nullptr)
    {
        *lastError = std::move(message);
    }
    if (fatalError != nullptr)
    {
        *fatalError = true;
    }
}

VkDescriptorSet FramePassContext::allocateSet(VkDescriptorSetLayout layout)
{
    if (layout == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE)
    {
        setError("descriptor allocation requested with an invalid layout or pool");
        return VK_NULL_HANDLE;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult allocationResult = vkAllocateDescriptorSets(device, &allocate, &set);
    if (allocationResult != VK_SUCCESS)
    {
        setError(vkFailure("vkAllocateDescriptorSets", allocationResult));
        return VK_NULL_HANDLE;
    }
    return set;
}

void FramePassContext::writeSampled(
    VkDescriptorSet set,
    std::uint32_t binding,
    VkImageView view,
    VkImageLayout imageLayout)
{
    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, imageLayout};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void FramePassContext::writeSampler(VkDescriptorSet set)
{
    VkDescriptorImageInfo sampler{linearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 10;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &sampler;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void FramePassContext::writeStorage(VkDescriptorSet set, std::uint32_t binding, VkImageView view)
{
    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void FramePassContext::writeStorageBuffer(
    VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, VkDeviceSize size)
{
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void FramePassContext::writeUniformBuffer(
    VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, VkDeviceSize size)
{
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void FramePassContext::writeGpuStageTimestamp(std::uint32_t stage, bool begin)
{
    if (!timestampsEnabled || frame == nullptr || frameContext == nullptr || stage >= 4u)
    {
        return;
    }
    vkCmdWriteTimestamp2(
        commandBuffer(),
        begin ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        frameContext->timestampPool,
        frame->stageQueryBase + stage * 2u + (begin ? 0u : 1u));
}

void FramePassContext::transitionImage(
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2 dstAccess,
    VkImageAspectFlags aspect,
    std::uint32_t baseLayer,
    std::uint32_t layerCount)
{
    if (image == VK_NULL_HANDLE)
    {
        return;
    }
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, baseLayer, layerCount};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer(), &dependency);
}

Halcyon::Result<void> FramePassContext::recordImageUpload(
    VkImage image,
    std::span<const std::uint16_t> data,
    std::span<const VkBufferImageCopy> copies)
{
    if (commandBuffer() == VK_NULL_HANDLE || image == VK_NULL_HANDLE || data.empty() ||
        copies.empty() || frameUploadBuffers == nullptr ||
        currentFrame >= frameUploadBuffers->size() || gpuAllocator == nullptr)
    {
        return fail("invalid procedural IBL upload state", Halcyon::ErrorCode::InvalidState);
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = data.size_bytes();
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocation = gpuAllocator->createBuffer(info, MemoryUsage::CpuToGpu);
    if (!allocation)
    {
        return allocation.error();
    }
    BufferAllocation staging = allocation.value();
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data.data()), data.size_bytes()};
    const auto write = gpuAllocator->writeBuffer(staging, bytes);
    if (!write)
    {
        gpuAllocator->destroy(staging);
        return write.error();
    }
    try
    {
        (*frameUploadBuffers)[currentFrame].push_back(staging);
    }
    catch (...)
    {
        gpuAllocator->destroy(staging);
        return fail("failed to retain procedural IBL staging allocation",
            Halcyon::ErrorCode::OutOfMemory);
    }
    vkCmdCopyBufferToImage(
        commandBuffer(),
        staging.buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(copies.size()),
        copies.data());
    return ok();
}

void computeCascadeMatrices(
    const FramePacket& packet,
    std::array<glm::mat4, 4>& cascadeMatrices,
    glm::vec4& cascadeSplits)
{
    const float nearPlane = std::max(1.0e-3f, packet.camera.positionAndNear.w);
    const float cameraFar = packet.camera.forwardAndFar.w > nearPlane
                                ? packet.camera.forwardAndFar.w
                                : 1000.0f;
    // Cascades covering hundreds of metres starve Sponza-scale interiors of
    // shadow resolution. Keep the shadow distance in a useful courtyard range.
    const float farPlane = std::min(cameraFar, 48.0f);
    const glm::mat4 invViewProjection = packet.camera.inverseViewProjection;
    std::array<glm::vec3, 8> frustumCorners{};
    std::size_t corner = 0;
    for (float z : {1.0f, 0.0f})
    {
        for (float y : {-1.0f, 1.0f})
        {
            for (float x : {-1.0f, 1.0f})
            {
                const glm::vec4 clip{x, y, z, 1.0f};
                const glm::vec4 world = invViewProjection * clip;
                frustumCorners[corner++] = world.w != 0.0f
                                               ? glm::vec3(world) / world.w
                                               : glm::vec3(world);
            }
        }
    }
    glm::vec3 lightDirection{0.35f, 0.85f, 0.2f};
    for (const auto& light : packet.lights)
    {
        if (light.directionAndType[3] > 0.5f && light.directionAndType[3] < 1.5f)
        {
            const glm::vec3 candidate{light.directionAndType[0], light.directionAndType[1],
                light.directionAndType[2]};
            if (glm::dot(candidate, candidate) > 1.0e-6f)
            {
                lightDirection = candidate;
            }
            break;
        }
    }
    lightDirection = glm::normalize(lightDirection);
    const glm::vec3 up = std::abs(glm::dot(lightDirection, glm::vec3{0, 1, 0})) > 0.95f
                             ? glm::vec3{1, 0, 0}
                             : glm::vec3{0, 1, 0};
    const float lambda = 0.65f;
    float previousSplit = nearPlane;
    for (std::uint32_t cascade = 0; cascade < 4; ++cascade)
    {
        const float p = static_cast<float>(cascade + 1u) / 4.0f;
        const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, p);
        const float split = glm::mix(nearPlane + (farPlane - nearPlane) * p, logarithmic, lambda);
        const float startRatio = (previousSplit - nearPlane) / (farPlane - nearPlane);
        const float endRatio = (split - nearPlane) / (farPlane - nearPlane);
        std::array<glm::vec3, 8> sliceCorners{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            const glm::vec3 nearCorner = frustumCorners[i];
            const glm::vec3 farCorner = frustumCorners[i + 4];
            sliceCorners[i] = glm::mix(nearCorner, farCorner, startRatio);
            sliceCorners[i + 4] = glm::mix(nearCorner, farCorner, endRatio);
        }
        glm::vec3 center{0.0f};
        for (const auto& point : sliceCorners)
        {
            center += point;
        }
        center /= 8.0f;
        float radius = 0.0f;
        for (const auto& point : sliceCorners)
        {
            radius = std::max(radius, glm::length(point - center));
        }
        radius = std::max(radius, 1.0f);
        radius = std::ceil(radius * 16.0f) / 16.0f;
        glm::vec3 lightPosition = center - lightDirection * radius * 2.0f;
        glm::mat4 lightView = glm::lookAtRH(lightPosition, center, up);
        const glm::vec3 centerLight = glm::vec3(lightView * glm::vec4(center, 1.0f));
        const float texelSize = (2.0f * radius) / 2048.0f;
        const glm::vec2 snapped = glm::floor(glm::vec2(centerLight) / texelSize) * texelSize;
        const glm::vec2 delta = snapped - glm::vec2(centerLight);
        center += glm::vec3(lightView[0]) * delta.x + glm::vec3(lightView[1]) * delta.y;
        lightPosition = center - lightDirection * radius * 2.0f;
        lightView = glm::lookAtRH(lightPosition, center, up);
        const float depthNear = 0.1f;
        const float depthFar = radius * 4.0f + 10.0f;
        cascadeMatrices[cascade] = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                       depthFar, depthNear) *
                                   lightView;
        cascadeSplits[cascade] = split;
        previousSplit = split;
    }
}

} // namespace Halcyon::Vulkan
