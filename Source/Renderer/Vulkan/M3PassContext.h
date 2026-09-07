#pragma once

#include "../Graph/FrameGraph.h"
#include "../Scene/FramePacket.h"
#include "DebugReadbackManager.h"
#include "FrameRecorder.h"
#include "GpuAllocator.h"
#include "HalcyonVulkanRenderer.h"
#include "PipelineRegistry.h"
#include "VulkanBindlessTable.h"
#include "VulkanFrameContext.h"
#include "VulkanFrameGraphProvider.h"
#include "VulkanGpuSceneBuffers.h"
#include "VulkanM3FrameResources.h"
#include "VulkanSceneResources.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;

// Per-frame recording state shared by M3 passes. Renderer::Impl fills this
// once per frame; each pass reads/writes graph handles through it.
struct M3PassContext
{
    VkDevice device = VK_NULL_HANDLE;
    VulkanFrame* frame = nullptr;
    const FramePacket* packet = nullptr;
    const RendererConfig* config = nullptr;

    PipelineRegistry* pipelines = nullptr;
    VulkanGpuSceneBuffers* gpuSceneBuffers = nullptr;
    VulkanSceneResources* sceneResources = nullptr;
    VulkanFrameGraphProvider* frameGraphProvider = nullptr;
    VulkanBindlessTable* bindlessTable = nullptr;
    FrameRecorder* frameRecorder = nullptr;
    VulkanM3FrameResources* m3FrameResources = nullptr;
    VulkanFrameContext* frameContext = nullptr;
    DebugReadbackManager* debugReadbacks = nullptr;
    GpuAllocator* gpuAllocator = nullptr;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkExtent2D swapchainExtent{};
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t currentFrame = 0;
    std::uint32_t imageIndex = 0;

    bool gpuDrivenBindless = false;
    bool timestampsEnabled = false;
    bool previousPacketValid = false;
    bool hasRenderedFrame = false;
    bool taaHistoryValid = false;
    glm::mat4 previousViewProjection{1.0f};
    CpuDrawState cpuDraw{};

    std::uint32_t gpuSceneInstanceCount = 0;
    std::uint32_t gpuMaterialCount = 0;
    std::uint32_t gpuMaterialId = 0;
    bool gpuIndirectCompatible = false;
    VkBuffer gpuVertexBuffer = VK_NULL_HANDLE;
    VkBuffer gpuIndexBuffer = VK_NULL_HANDLE;
    VkBuffer gpuMeshDrawBuffer = VK_NULL_HANDLE;
    VkDescriptorSet gpuCullSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuIndirectSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuGraphicsSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuPhase2GraphicsSet = VK_NULL_HANDLE;

    std::array<glm::mat4, 4> cascadeMatrices{};
    glm::vec4 cascadeSplits{0.0f};

    Graph::TextureHandle shadow{};
    Graph::TextureHandle gbuffer0{};
    Graph::TextureHandle gbuffer1{};
    Graph::TextureHandle gbuffer2{};
    Graph::TextureHandle motion{};
    Graph::TextureHandle instanceId{};
    Graph::TextureHandle depth{};
    Graph::TextureHandle hiz{};
    Graph::TextureHandle hdr{};
    Graph::TextureHandle historyA{};
    Graph::TextureHandle historyB{};
    Graph::TextureHandle irradiance{};
    Graph::TextureHandle prefiltered{};
    Graph::TextureHandle brdfLut{};
    Graph::BufferHandle clusterRanges{};
    Graph::BufferHandle clusterIndices{};
    Graph::BufferHandle clusterOverflow{};
    Graph::BufferHandle lightBuffer{};
    Graph::BufferHandle clusterCamera{};
    Graph::BufferHandle shadowConstants{};
    Graph::TextureHandle output{};
    Graph::PassHandle gbufferPassHandle{};
    std::uint32_t tileCount = 0;

    VkImageView swapchainView = VK_NULL_HANDLE;
    VkBuffer screenshotReadback = VK_NULL_HANDLE;
    std::vector<VkImage>* swapchainImages = nullptr;
    std::vector<bool>* swapchainImageInitialized = nullptr;
    std::vector<std::vector<BufferAllocation>>* frameUploadBuffers = nullptr;

    bool* iblInitialized = nullptr;
    bool* taaHistoryFlip = nullptr;
    bool* taaHistoryInitializedA = nullptr;
    bool* taaHistoryInitializedB = nullptr;
    bool* fatalError = nullptr;
    bool* deviceLost = nullptr;
    std::string* lastError = nullptr;

    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept
    {
        return frame != nullptr ? frame->commandBuffer : VK_NULL_HANDLE;
    }

    void setError(std::string message);
    [[nodiscard]] VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
    void writeSampled(
        VkDescriptorSet set,
        std::uint32_t binding,
        VkImageView view,
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    void writeSampler(VkDescriptorSet set);
    void writeStorage(VkDescriptorSet set, std::uint32_t binding, VkImageView view);
    void writeStorageBuffer(
        VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, VkDeviceSize size);
    void writeUniformBuffer(
        VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, VkDeviceSize size);
    void writeGpuStageTimestamp(std::uint32_t stage, bool begin);
    void transitionImage(
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage,
        VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        std::uint32_t baseLayer = 0,
        std::uint32_t layerCount = 1);
    [[nodiscard]] Halcyon::Result<void> recordImageUpload(
        VkImage image,
        std::span<const std::uint16_t> data,
        std::span<const VkBufferImageCopy> copies);
};

struct ProceduralIblData
{
    std::vector<std::uint16_t> irradiance;
    std::vector<VkBufferImageCopy> irradianceCopies;
    std::vector<std::uint16_t> prefiltered;
    std::vector<VkBufferImageCopy> prefilteredCopies;
    std::vector<std::uint16_t> brdf;
    std::vector<VkBufferImageCopy> brdfCopies;
};

[[nodiscard]] ProceduralIblData createProceduralIbl();

void computeCascadeMatrices(
    const FramePacket& packet,
    std::array<glm::mat4, 4>& cascadeMatrices,
    glm::vec4& cascadeSplits);

[[nodiscard]] Halcyon::Result<void> recordGpuDrivenCulling(M3PassContext& ctx);
void addCsmShadowPasses(Graph::FrameGraph& graph, M3PassContext& ctx);
void addGBufferPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addHiZOcclusionPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addClusterBuildPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addDeferredLightingPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addTransparencyPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addTaaResolvePass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addTonemapPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void addPresentPass(Graph::FrameGraph& graph, M3PassContext& ctx);
void recordVisibilityReadback(M3PassContext& ctx);

} // namespace Halcyon::Vulkan
