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
#include "VulkanFrameResources.h"
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

// Per-frame recording state shared by render passes. Renderer::Impl fills this
// once per frame; each pass reads/writes graph handles through it.
struct FramePassContext
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
    VulkanFrameResources* frameResources = nullptr;
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
    std::uint32_t virtualIndirectDrawCapacity = 0;

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
    VkDescriptorSet gpuCsmCullSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuCsmIndirectSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuCsmGraphicsSet = VK_NULL_HANDLE;

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
    Graph::TextureHandle visibility{};
    Graph::TextureHandle visibilityPrimitive{};
    Graph::TextureHandle visibilityBarycentrics{};
    Graph::BufferHandle materialClassification{};
    Graph::BufferHandle virtualTransforms{};
    Graph::BufferHandle virtualMeshMaterials{};
    Graph::BufferHandle virtualCullFrame{};
    Graph::BufferHandle visibleMeshlets{};
    Graph::BufferHandle visibleMeshletCount{};
    Graph::BufferHandle meshletIndirect{};
    Graph::BufferHandle meshletIndirectCount{};
    Graph::BufferHandle virtualValidation{};
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
    // Tracks the persistent IBL image layout independently from whether a
    // complete payload has been uploaded. This lets a failed upload retry
    // from SHADER_READ_ONLY_OPTIMAL without assuming UNDEFINED.
    bool* iblImageInitialized = nullptr;
    bool* virtualHiZInitialized = nullptr;
    bool* virtualHiZImageInitialized = nullptr;
    // Set by visibility rasterization only after its clear/render sequence
    // completed. Later virtual passes use it to avoid consuming undefined
    // attachments when a transient setup step fails.
    bool* virtualVisibilityValid = nullptr;
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

// Rebuild the pass-local aliases inside a deferred execute lambda. Capture
// `passCtx` (FramePassContext*) by value; do not capture function-scope
// references with [&] or they dangle after the add* function returns.
#define HALCYON_BIND_PASS_EXECUTE(passCtx) \
    FramePassContext& ctx = *(passCtx); \
    VkDevice device = ctx.device; \
    auto& frame = *ctx.frame; \
    const auto& packet = *ctx.packet; \
    const auto& config = *ctx.config; \
    auto& gpuSceneBuffers = *ctx.gpuSceneBuffers; \
    auto& sceneResources = *ctx.sceneResources; \
    auto& frameGraphProvider = *ctx.frameGraphProvider; \
    auto& pipelines = *ctx.pipelines; \
    auto& gpuAllocator = *ctx.gpuAllocator; \
    auto& frameResources = *ctx.frameResources; \
    auto& bindlessTable = *ctx.bindlessTable; \
    auto& frameRecorder = *ctx.frameRecorder; \
    auto& debugReadbacks = *ctx.debugReadbacks; \
    auto& csmDepthPipeline = pipelines.csmDepthPipeline; \
    auto& gbufferPipeline = pipelines.gbufferPipeline; \
    auto& deferredLightingPipeline = pipelines.deferredLightingPipeline; \
    auto& transparentPipeline = pipelines.transparentPipeline; \
    auto& taaPipeline = pipelines.taaPipeline; \
    auto& tonemapPipeline = pipelines.tonemapPipeline; \
    auto& clusterBuildPipeline = pipelines.clusterBuildPipeline; \
    auto& frustumCullPipeline = pipelines.frustumCullPipeline; \
    auto& indirectBuildPipeline = pipelines.indirectBuildPipeline; \
    auto& gpuDrivenGbufferPipeline = pipelines.gpuDrivenGbufferPipeline; \
    auto& hizBuildPipeline = pipelines.hizBuildPipeline; \
    auto& occlusionPhase1Pipeline = pipelines.occlusionPhase1Pipeline; \
    auto& occlusionPhase2Pipeline = pipelines.occlusionPhase2Pipeline; \
    auto& gpuSceneCullLayout = pipelines.gpuSceneCullLayout; \
    auto& gpuSceneIndirectLayout = pipelines.gpuSceneIndirectLayout; \
    auto& gpuSceneGraphicsLayout = pipelines.gpuSceneGraphicsLayout; \
    auto& hizLayout = pipelines.hizLayout; \
    auto& occlusionPhase1Layout = pipelines.occlusionPhase1Layout; \
    auto& occlusionPhase2Layout = pipelines.occlusionPhase2Layout; \
    auto& lightingLayout = pipelines.lightingLayout; \
    auto& taaLayout = pipelines.taaLayout; \
    auto& clusterLayout = pipelines.clusterLayout; \
    auto& tonemapLayout = pipelines.tonemapLayout; \
    auto& gpuDrivenBindless = ctx.gpuDrivenBindless; \
    auto& gpuMaterialId = ctx.gpuMaterialId; \
    auto& gpuIndirectCompatible = ctx.gpuIndirectCompatible; \
    auto& gpuGraphicsSet = ctx.gpuGraphicsSet; \
    auto& gpuPhase2GraphicsSet = ctx.gpuPhase2GraphicsSet; \
    auto& gpuIndirectSet = ctx.gpuIndirectSet; \
    auto& gpuVertexBuffer = ctx.gpuVertexBuffer; \
    auto& gpuIndexBuffer = ctx.gpuIndexBuffer; \
    auto& gpuMeshDrawBuffer = ctx.gpuMeshDrawBuffer; \
    auto& gpuSceneInstanceCount = ctx.gpuSceneInstanceCount; \
    auto& gpuMaterialCount = ctx.gpuMaterialCount; \
    auto& cascadeMatrices = ctx.cascadeMatrices; \
    auto& cascadeSplits = ctx.cascadeSplits; \
    auto& depth = ctx.depth; \
    auto& hdr = ctx.hdr; \
    auto& historyA = ctx.historyA; \
    auto& historyB = ctx.historyB; \
    auto& output = ctx.output; \
    auto& tileCount = ctx.tileCount; \
    const std::uint32_t width = ctx.width; \
    const std::uint32_t height = ctx.height; \
    const VkExtent2D swapchainExtent = ctx.swapchainExtent; \
    const VkFormat swapchainFormat = ctx.swapchainFormat; \
    const std::uint32_t currentFrame = ctx.currentFrame; \
    const std::uint32_t imageIndex = ctx.imageIndex; \
    const bool previousPacketValid = ctx.previousPacketValid; \
    const bool hasRenderedFrame = ctx.hasRenderedFrame; \
    const bool taaHistoryValid = ctx.taaHistoryValid; \
    const glm::mat4 previousViewProjection = ctx.previousViewProjection; \
    auto& clusterOverflowReadbacks = debugReadbacks.clusterOverflowReadbacks; \
    auto& instanceIdReadbacks = debugReadbacks.instanceIdReadbacks; \
    auto& instanceIdReadbackValid = debugReadbacks.instanceIdReadbackValid; \
    auto& instanceIdReadbackFrameIndices = debugReadbacks.instanceIdReadbackFrameIndices; \
    auto& screenshotReadback = ctx.screenshotReadback; \
    auto& swapchainImages = *ctx.swapchainImages; \
    struct ImportedTarget { VkImageView view = VK_NULL_HANDLE; } importedTarget{ctx.swapchainView}; \
    bool& iblInitialized = *ctx.iblInitialized; \
    bool& taaHistoryFlip = *ctx.taaHistoryFlip; \
    bool& taaHistoryInitializedA = *ctx.taaHistoryInitializedA; \
    bool& taaHistoryInitializedB = *ctx.taaHistoryInitializedB; \
    bool& fatalError = *ctx.fatalError; \
    const auto allocateSet = [&](VkDescriptorSetLayout layout) { return ctx.allocateSet(layout); }; \
    const auto writeSampled = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view, \
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) { \
        ctx.writeSampled(set, binding, view, imageLayout); \
    }; \
    const auto writeSampler = [&](VkDescriptorSet set) { ctx.writeSampler(set); }; \
    const auto writeStorage = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view) { \
        ctx.writeStorage(set, binding, view); \
    }; \
    const auto writeStorageBuffer = [&](VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, \
        VkDeviceSize size) { ctx.writeStorageBuffer(set, binding, buffer, size); }; \
    const auto writeUniformBuffer = [&](VkDescriptorSet set, std::uint32_t binding, VkBuffer buffer, \
        VkDeviceSize size) { ctx.writeUniformBuffer(set, binding, buffer, size); }; \
    const auto writeGpuStageTimestamp = [&](std::uint32_t stage, bool begin) { \
        ctx.writeGpuStageTimestamp(stage, begin); \
    }; \
    const auto transitionImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, \
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, \
        VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, \
        std::uint32_t baseLayer = 0, std::uint32_t layerCount = 1) { \
        ctx.transitionImage(image, oldLayout, newLayout, srcStage, srcAccess, dstStage, dstAccess, \
            aspect, baseLayer, layerCount); \
    }; \
    const auto recordImageUpload = [&](VkCommandBuffer, VkImage image, auto data, auto copies) { \
        return ctx.recordImageUpload(image, data, copies); \
    }; \
    const auto setError = [&](std::string message) { ctx.setError(std::move(message)); }

[[nodiscard]] Halcyon::Result<void> recordGpuDrivenCulling(FramePassContext& ctx);
void addCsmShadowPasses(Graph::FrameGraph& graph, FramePassContext& ctx);
void addGBufferPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addHiZOcclusionPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addClusterBuildPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addDeferredLightingPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addTransparencyPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addTaaResolvePass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addTonemapPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void addPresentPass(Graph::FrameGraph& graph, FramePassContext& ctx);
void recordVisibilityReadback(FramePassContext& ctx);

} // namespace Halcyon::Vulkan
