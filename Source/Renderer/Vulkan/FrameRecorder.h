#pragma once

#include "../Scene/FramePacket.h"
#include "PipelineRegistry.h"
#include "VulkanPipeline.h"
#include "VulkanSceneResources.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{

using FramePacket = Halcyon::Renderer::Scene::FramePacket;

struct CpuDrawState
{
    VulkanSceneResources* sceneResources = nullptr;
    PipelineRegistry* pipelines = nullptr;
    std::uint32_t* materialDescriptorBindCount = nullptr;
    const std::vector<Halcyon::Renderer::Scene::InstanceData>* previousInstances = nullptr;
    bool previousPacketValid = false;
    glm::mat4 previousViewProjection{1.0f};
    bool gpuDrivenBindless = false;
};


// Parameters for the shared mesh-grouped indirect build (mode0 linked list,
// then mode1 compacted commands). Phase 2 sets resetMeshHeads because it
// reuses the main-path scratch lists.
struct MeshGroupedIndirectBuildDesc
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t instanceCount = 0;
    std::uint32_t meshCount = 0;
    VkBuffer meshHeads = VK_NULL_HANDLE;
    VkBuffer meshNext = VK_NULL_HANDLE;
    VkBuffer groupedVisible = VK_NULL_HANDLE;
    VkBuffer groupedCount = VK_NULL_HANDLE;
    VkBuffer indirectCommands = VK_NULL_HANDLE;
    VkBuffer indirectCount = VK_NULL_HANDLE;
    bool resetMeshHeads = false;
    VkDeviceSize meshHeadsBytes = 0;
};

class FrameRecorder final
{
public:
    void recordMeshGroupedIndirectBuild(
        VkCommandBuffer commandBuffer,
        const VulkanPipeline& indirectBuildPipeline,
        const MeshGroupedIndirectBuildDesc& desc) const;

    void bindInstanceMesh(VkCommandBuffer commandBuffer, const MeshResource& mesh) const;
    void drawShadowInstances(
        VkCommandBuffer commandBuffer,
        const FramePacket& packet,
        const glm::mat4& cascadeMatrix,
        const CpuDrawState& draw) const;
    void drawOpaqueGBufferInstances(
        VkCommandBuffer commandBuffer,
        const FramePacket& packet,
        const CpuDrawState& draw) const;
    void drawTransparentInstances(
        VkCommandBuffer commandBuffer,
        const FramePacket& packet,
        const CpuDrawState& draw) const;
    void drawGpuDrivenCpuFallback(
        VkCommandBuffer commandBuffer,
        const FramePacket& packet,
        std::uint32_t gpuMaterialId,
        const CpuDrawState& draw) const;
};

} // namespace Halcyon::Vulkan
