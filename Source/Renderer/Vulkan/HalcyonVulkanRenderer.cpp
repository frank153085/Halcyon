#include "HalcyonVulkanRenderer.h"

#include "Core/Profiler.h"
#include "GpuAllocator.h"
#include "GpuUploader.h"
#include "VulkanBindlessTable.h"
#include "VulkanGpuSceneBuffers.h"
#include "VulkanCommon.h"
#include "VulkanDevice.h"
#include "VulkanFrameContext.h"
#include "VulkanFrameGraphProvider.h"
#include "VulkanM3FrameResources.h"
#include "VulkanPipeline.h"
#include "VulkanSceneResources.h"
#include "VulkanSwapchain.h"
#include "../Graph/FrameGraph.h"
#include "../Graph/BarrierPlanner.h"

#ifndef HALCYON_BUILD_EXPERIMENTAL_M2
#define HALCYON_BUILD_EXPERIMENTAL_M2 0
#endif

#if HALCYON_BUILD_EXPERIMENTAL_M2
#include "../Graph/BarrierPlanner.h"
#include "../Graph/FrameGraph.h"
#endif

// GLFW is included here (rather than in the public header) so applications
// can choose their own GLFW include policy.  The Vulkan include guard makes
// this safe when the caller included glfw3.h with GLFW_INCLUDE_VULKAN first.
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include <filesystem>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <new>
#include <limits>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace Graph = Halcyon::Renderer::Graph;
#if HALCYON_BUILD_EXPERIMENTAL_M2
#endif
namespace
{

[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23u) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu)
    {
        return static_cast<std::uint16_t>(sign | (mantissa == 0u ? 0x7c00u : 0x7e00u));
    }
    const int halfExponent = static_cast<int>(exponent) - 127 + 15;
    if (halfExponent >= 31)
    {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - halfExponent);
        const std::uint32_t rounded = (mantissa + (1u << (shift - 1u))) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    mantissa += 0x1000u;
    if ((mantissa & 0x800000u) != 0u)
    {
        mantissa = 0;
        if (halfExponent + 1 >= 31)
            return static_cast<std::uint16_t>(sign | 0x7c00u);
        return static_cast<std::uint16_t>(sign |
            (static_cast<std::uint32_t>(halfExponent + 1) << 10u));
    }
    return static_cast<std::uint16_t>(sign |
        (static_cast<std::uint32_t>(halfExponent) << 10u) | (mantissa >> 13u));
}

[[nodiscard]] glm::vec3 cubeDirection(std::uint32_t face, float u, float v) noexcept
{
    const glm::vec3 direction = face == 0u ? glm::vec3{1.0f, -v, -u}
        : face == 1u ? glm::vec3{-1.0f, -v, u}
        : face == 2u ? glm::vec3{u, 1.0f, v}
        : face == 3u ? glm::vec3{u, -1.0f, -v}
        : face == 4u ? glm::vec3{u, -v, 1.0f}
                     : glm::vec3{-u, -v, -1.0f};
    return glm::normalize(direction);
}

[[nodiscard]] glm::vec3 proceduralSky(glm::vec3 direction, float roughness) noexcept
{
    const float skyAmount = glm::smoothstep(-0.08f, 0.18f, direction.y);
    const float zenith = std::pow(std::max(direction.y, 0.0f), 0.35f);
    const glm::vec3 ground{0.018f, 0.014f, 0.012f};
    const glm::vec3 horizon{0.22f, 0.30f, 0.48f};
    const glm::vec3 top{0.025f, 0.075f, 0.19f};
    glm::vec3 radiance = glm::mix(ground, glm::mix(horizon, top, zenith), skyAmount);
    const glm::vec3 sunDirection = glm::normalize(glm::vec3{0.32f, 0.88f, 0.24f});
    const float sunExponent = glm::mix(768.0f, 4.0f, roughness);
    const float sun = std::pow(std::max(glm::dot(direction, sunDirection), 0.0f), sunExponent);
    radiance += glm::vec3{8.0f, 6.2f, 4.2f} * sun * (1.0f - roughness);
    const glm::vec3 average{0.085f, 0.105f, 0.15f};
    return glm::mix(radiance, average, roughness * roughness * 0.82f);
}

void appendRgba16(std::vector<std::uint16_t>& output, const glm::vec3& color)
{
    output.push_back(floatToHalf(color.r));
    output.push_back(floatToHalf(color.g));
    output.push_back(floatToHalf(color.b));
    output.push_back(floatToHalf(1.0f));
}

[[nodiscard]] float radicalInverse(std::uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

[[nodiscard]] glm::vec2 integrateBrdf(float nDotV, float roughness) noexcept
{
    constexpr std::uint32_t sampleCount = 64u;
    constexpr float pi = 3.14159265358979323846f;
    const glm::vec3 view{std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV};
    float scale = 0.0f;
    float bias = 0.0f;
    const float alpha = std::max(0.002f, roughness * roughness);
    for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
    {
        const float xiX = static_cast<float>(sample) / static_cast<float>(sampleCount);
        const float xiY = radicalInverse(sample);
        const float phi = 2.0f * pi * xiX;
        const float cosTheta = std::sqrt((1.0f - xiY) /
            std::max(1.0f + (alpha * alpha - 1.0f) * xiY, 1.0e-6f));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const glm::vec3 halfVector{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta,
            cosTheta};
        const glm::vec3 light = glm::normalize(2.0f * glm::dot(view, halfVector) * halfVector - view);
        const float nDotL = std::max(light.z, 0.0f);
        const float nDotH = std::max(halfVector.z, 0.0f);
        const float vDotH = std::max(glm::dot(view, halfVector), 0.0f);
        if (nDotL <= 0.0f || nDotH <= 0.0f)
            continue;
        const float k = roughness * roughness * 0.5f;
        const float gV = nDotV / std::max(nDotV * (1.0f - k) + k, 1.0e-6f);
        const float gL = nDotL / std::max(nDotL * (1.0f - k) + k, 1.0e-6f);
        const float visibility = gV * gL * vDotH /
            std::max(nDotH * nDotV, 1.0e-6f);
        const float fresnel = std::pow(1.0f - vDotH, 5.0f);
        scale += (1.0f - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    return {scale / static_cast<float>(sampleCount), bias / static_cast<float>(sampleCount)};
}

struct ProceduralIblData
{
    std::vector<std::uint16_t> irradiance;
    std::vector<VkBufferImageCopy> irradianceCopies;
    std::vector<std::uint16_t> prefiltered;
    std::vector<VkBufferImageCopy> prefilteredCopies;
    std::vector<std::uint16_t> brdf;
    std::vector<VkBufferImageCopy> brdfCopies;
};

[[nodiscard]] ProceduralIblData createProceduralIbl()
{
    ProceduralIblData result;
    constexpr std::uint32_t irradianceSize = 32u;
    for (std::uint32_t face = 0; face < 6u; ++face)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = result.irradiance.size() * sizeof(std::uint16_t);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
        copy.imageExtent = {irradianceSize, irradianceSize, 1};
        result.irradianceCopies.push_back(copy);
        for (std::uint32_t y = 0; y < irradianceSize; ++y)
            for (std::uint32_t x = 0; x < irradianceSize; ++x)
            {
                const float u = (2.0f * (static_cast<float>(x) + 0.5f) / irradianceSize) - 1.0f;
                const float v = (2.0f * (static_cast<float>(y) + 0.5f) / irradianceSize) - 1.0f;
                const glm::vec3 direction = cubeDirection(face, u, v);
                const glm::vec3 diffuse = proceduralSky(direction, 0.82f) * 3.14159265f;
                appendRgba16(result.irradiance, diffuse);
            }
    }
    constexpr std::uint32_t baseSize = 64u;
    constexpr std::uint32_t mipCount = 7u;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const std::uint32_t size = std::max(1u, baseSize >> mip);
        const float roughness = static_cast<float>(mip) / static_cast<float>(mipCount - 1u);
        for (std::uint32_t face = 0; face < 6u; ++face)
        {
            VkBufferImageCopy copy{};
            copy.bufferOffset = result.prefiltered.size() * sizeof(std::uint16_t);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
            copy.imageExtent = {size, size, 1};
            result.prefilteredCopies.push_back(copy);
            for (std::uint32_t y = 0; y < size; ++y)
                for (std::uint32_t x = 0; x < size; ++x)
                {
                    const float u = (2.0f * (static_cast<float>(x) + 0.5f) / size) - 1.0f;
                    const float v = (2.0f * (static_cast<float>(y) + 0.5f) / size) - 1.0f;
                    appendRgba16(result.prefiltered,
                        proceduralSky(cubeDirection(face, u, v), roughness));
                }
        }
    }
    constexpr std::uint32_t lutSize = 128u;
    VkBufferImageCopy lutCopy{};
    lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    lutCopy.imageExtent = {lutSize, lutSize, 1};
    result.brdfCopies.push_back(lutCopy);
    result.brdf.reserve(lutSize * lutSize * 2u);
    for (std::uint32_t y = 0; y < lutSize; ++y)
        for (std::uint32_t x = 0; x < lutSize; ++x)
        {
            const glm::vec2 integrated = integrateBrdf(
                (static_cast<float>(x) + 0.5f) / lutSize,
                (static_cast<float>(y) + 0.5f) / lutSize);
            result.brdf.push_back(floatToHalf(integrated.x));
            result.brdf.push_back(floatToHalf(integrated.y));
        }
    return result;
}

// The shader uses a fixed-size descriptor array because Vulkan pipeline
// layouts must agree with the statically reflected array length. Devices that
// cannot expose this complete table intentionally use the M3 material-set
// fallback below.
constexpr std::uint32_t kGpuDrivenSampledImageCapacity = 256u;
constexpr std::uint32_t kGpuDrivenSamplerCapacity = 16u;

#if HALCYON_BUILD_EXPERIMENTAL_M2
[[nodiscard]] std::uint32_t descriptorCapacity(
    std::uint32_t preferred, std::uint32_t perStageLimit, std::uint32_t setLimit) noexcept
{
    return std::max(1u, std::min({preferred, perStageLimit, setLimit}));
}

[[nodiscard]] Resources::BindlessTableConfig bindlessConfig(
    const VkPhysicalDeviceLimits& limits) noexcept
{
    Resources::BindlessTableConfig config;
    config.sampledImageCapacity = descriptorCapacity(
        kGpuDrivenSampledImageCapacity, limits.maxPerStageDescriptorSampledImages,
        limits.maxDescriptorSetSampledImages);
    config.storageImageCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageImages, limits.maxDescriptorSetStorageImages);
    config.uniformBufferCapacity = descriptorCapacity(
        12u, limits.maxPerStageDescriptorUniformBuffers, limits.maxDescriptorSetUniformBuffers);
    config.storageBufferCapacity = descriptorCapacity(
        8u, limits.maxPerStageDescriptorStorageBuffers, limits.maxDescriptorSetStorageBuffers);
    config.samplerCapacity = descriptorCapacity(
        kGpuDrivenSamplerCapacity, limits.maxPerStageDescriptorSamplers,
        limits.maxDescriptorSetSamplers);
    return config;
}

[[nodiscard]] VkPipelineStageFlags2 toVkStages(Graph::PipelineStage stages) noexcept
{
    VkPipelineStageFlags2 result = VK_PIPELINE_STAGE_2_NONE;
    if (Graph::any(stages & Graph::PipelineStage::VertexInput))
    {
        result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::VertexShader))
    {
        result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::FragmentShader))
    {
        result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::ComputeShader))
    {
        result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::ColorOutput))
    {
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::DepthTest))
    {
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::Transfer))
    {
        result |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::DrawIndirect))
    {
        result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::Host))
    {
        result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    }
    if (Graph::any(stages & Graph::PipelineStage::AllCommands))
    {
        result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return result;
}

[[nodiscard]] VkAccessFlags2 toVkAccess(Graph::AccessFlags access) noexcept
{
    VkAccessFlags2 result = VK_ACCESS_2_NONE;
    if (Graph::any(access & Graph::AccessFlags::VertexRead))
    {
        result |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::IndexRead))
    {
        result |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::UniformRead))
    {
        result |= VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderSampledRead))
    {
        result |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageRead))
    {
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ShaderStorageWrite))
    {
        result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::IndirectRead))
    {
        result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ColorWrite))
    {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::DepthRead))
    {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::DepthWrite))
    {
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::TransferRead))
    {
        result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::TransferWrite))
    {
        result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::HostWrite))
    {
        result |= VK_ACCESS_2_HOST_WRITE_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::ColorRead))
    {
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    }
    if (Graph::any(access & Graph::AccessFlags::PresentRead))
    {
        result |= VK_ACCESS_2_NONE;
    }
    return result;
}

[[nodiscard]] VkImageLayout toVkLayout(Graph::ImageLayout layout) noexcept
{
    switch (layout)
    {
        case Graph::ImageLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case Graph::ImageLayout::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        case Graph::ImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case Graph::ImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case Graph::ImageLayout::DepthAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case Graph::ImageLayout::TransferSource:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case Graph::ImageLayout::TransferDestination:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case Graph::ImageLayout::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}
#endif

} // namespace

struct Renderer::Impl
{
    static constexpr std::uint32_t VisibilityReadbackCapacity = 1u << 20;
    // frustum candidates, phase-1 visible, phase-2 visible, phase-1 command
    // count, and phase-2 command count.
    static constexpr std::uint32_t VisibilityReadbackHeaderCount = 5u;
    using FrameContext = VulkanFrame;

    RendererConfig config{};
    GLFWwindow* window = nullptr;
    std::string lastError;

    VulkanDevice deviceState;
    VkInstance& instance = deviceState.instance;
    VkDebugUtilsMessengerEXT& debugMessenger = deviceState.debugMessenger;
    VkSurfaceKHR& surface = deviceState.surface;
    VkPhysicalDevice& physicalDevice = deviceState.physicalDevice;
    VkPhysicalDeviceProperties& physicalProperties = deviceState.physicalProperties;
    VkPhysicalDeviceMemoryProperties& memoryProperties = deviceState.memoryProperties;
    VkDevice& device = deviceState.device;
    VkQueue& graphicsQueue = deviceState.graphicsQueue;
    VkQueue& presentQueue = deviceState.presentQueue;
    std::uint32_t& graphicsQueueFamily = deviceState.graphicsQueueFamily;
    std::uint32_t& presentQueueFamily = deviceState.presentQueueFamily;
    Capabilities& caps = deviceState.capabilities;
    std::uint64_t& deviceLocalBytes = deviceState.deviceLocalBytes;

    // Swapchain, depth target and presentation semaphores are owned by the
    // dedicated swapchain module.  References keep the frame code concise
    // while retaining a single source of truth for these handles.
    VulkanSwapchain swapchainState;
    VkSwapchainKHR& swapchain = swapchainState.swapchain;
    VkFormat& swapchainFormat = swapchainState.swapchainFormat;
    VkColorSpaceKHR& swapchainColorSpace = swapchainState.swapchainColorSpace;
    VkExtent2D& swapchainExtent = swapchainState.swapchainExtent;
    VkFormat& depthFormat = swapchainState.depthFormat;
    std::vector<VkImage>& swapchainImages = swapchainState.swapchainImages;
    std::vector<VkImageView>& swapchainImageViews = swapchainState.swapchainImageViews;
    std::vector<VkSemaphore>& presentReadySemaphores = swapchainState.presentReadySemaphores;
    std::vector<bool>& swapchainImageInitialized = swapchainState.swapchainImageInitialized;

    VulkanFrameContext frameContext;
    std::vector<FrameContext>& frames = frameContext.frames;
    std::uint32_t& currentFrame = frameContext.currentFrame;
    bool& timestampsEnabled = frameContext.timestampsEnabled;

    VulkanPipeline csmDepthPipeline;
    VulkanPipeline gbufferPipeline;
    VulkanPipeline gbufferDoubleSidedPipeline;
    VulkanPipeline deferredLightingPipeline;
    VulkanPipeline transparentPipeline;
    VulkanPipeline transparentDoubleSidedPipeline;
    VulkanPipeline taaPipeline;
    VulkanPipeline tonemapPipeline;
    VulkanPipeline clusterBuildPipeline;
    VulkanPipeline frustumCullPipeline;
    VulkanPipeline indirectBuildPipeline;
    VulkanPipeline gpuDrivenGbufferPipeline;
    bool gpuDrivenBindless = false;
    VulkanPipeline hizBuildPipeline;
    VulkanPipeline occlusionPhase1Pipeline;
    VulkanPipeline occlusionPhase2Pipeline;
    VkDescriptorSetLayout gpuSceneCullLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuSceneIndirectLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuSceneGraphicsLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout hizLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout occlusionPhase1Layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout occlusionPhase2Layout = VK_NULL_HANDLE;
    VkDescriptorPool gpuSceneDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3MaterialLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3LightingLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3TaaLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3ClusterLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m3TonemapLayout = VK_NULL_HANDLE;
    VkDescriptorPool m3DescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m3DescriptorPools;
    VkSampler m3LinearSampler = VK_NULL_HANDLE;

    VkExtent2D& requestedExtent = swapchainState.requestedExtent;
    bool& framebufferResized = swapchainState.framebufferResized;
    bool initialized = false;
    bool deviceLost = false;
    bool fatalError = false;
    bool& rayQueryEnabled = deviceState.rayQueryEnabled;
    VkDeviceSize deviceMemoryBytes = 0;
    GpuAllocator gpuAllocator;
    GpuUploader gpuUploader;
    std::vector<BufferAllocation> clusterOverflowReadbacks;
    std::vector<BufferAllocation> gpuVisibilityReadbacks;
    std::vector<bool> gpuVisibilityValid;
    std::vector<std::vector<std::uint32_t>> gpuReferenceVisible;
    std::vector<BufferAllocation> instanceIdReadbacks;
    std::vector<bool> instanceIdReadbackValid;
    std::vector<std::uint64_t> instanceIdReadbackFrameIndices;
    std::vector<std::vector<BufferAllocation>> frameUploadBuffers;
    VulkanFrameGraphProvider frameGraphProvider;
    VulkanM3FrameResources m3FrameResources;
    VulkanSceneResources sceneResources;
    VulkanGpuSceneBuffers gpuSceneBuffers;
    VulkanBindlessTable bindlessTable;
    OverlayCallback overlayCallback = nullptr;
    bool taaHistoryValid = false;
    bool taaHistoryFlip = false;
    bool taaHistoryInitializedA = false;
    bool taaHistoryInitializedB = false;
    bool iblInitialized = false;
    bool hasRenderedFrame = false;
    std::uint64_t lastFrameIndex = 0;
    std::uint64_t renderSerial = 0;
    std::vector<InstanceData> previousInstances;
    glm::mat4 previousViewProjection{1.0f};
    bool previousPacketValid = false;
    std::filesystem::path pendingScreenshotPath;
    std::uint64_t gpuSceneContentHash = 0;
    std::uint32_t gpuSceneInstanceCount = 0;
    bool gpuDrivenActive = false;
    std::uint32_t gpuFallbackInstanceCount = 0;
    std::uint32_t gpuMaterialCount = 0;
    std::uint32_t materialDescriptorBindCount = 0;
    std::vector<Resources::DescriptorHandle> bindlessTextureHandles;
    Resources::DescriptorHandle bindlessSamplerHandle{};
    std::vector<Halcyon::Renderer::Scene::MaterialGpuData> bindlessMaterialRows;
    Halcyon::Renderer::Scene::GpuSceneState gpuSceneState{131072};

    ~Impl()
    {
        cleanup();
    }

    void setError(std::string message)
    {
        lastError = std::move(message);
    }

    [[nodiscard]] VoidResult synchronizeBindlessMaterials()
    {
        if (!bindlessTable.initialized()) return ok();
        const auto sampledType = Resources::DescriptorType::SampledImage;
        bindlessTextureHandles.resize(sceneResources.textureCount());
        for (std::uint32_t dense = 0; dense < sceneResources.textureCount(); ++dense)
        {
            const TextureResource* texture = sceneResources.textureDense(dense);
            if (texture == nullptr) continue;
            auto& handle = bindlessTextureHandles[dense];
            if (!handle.valid())
            {
                const auto allocated = bindlessTable.allocate(sampledType);
                if (!allocated) return fail(allocated.error().describe());
                handle = allocated.value();
            }
            const VkDescriptorImageInfo image{VK_NULL_HANDLE, texture->view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const auto write = bindlessTable.writeImage(sampledType, handle, image);
            if (!write) return fail(write.error().describe());
        }
        if (const TextureResource* texture = sceneResources.textureDense(0); texture != nullptr)
        {
            const VkDescriptorImageInfo image{VK_NULL_HANDLE, texture->view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const auto write = bindlessTable.writeImage(sampledType,
                bindlessTable.table().defaultHandle(sampledType), image);
            if (!write) return fail(write.error().describe());
            const VkDescriptorImageInfo sampler{texture->sampler, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_UNDEFINED};
            const auto samplerWrite = bindlessTable.writeImage(Resources::DescriptorType::Sampler,
                bindlessTable.table().defaultHandle(Resources::DescriptorType::Sampler), sampler);
            if (!samplerWrite) return fail(samplerWrite.error().describe());
        }
        bindlessMaterialRows.resize(sceneResources.materialCount());
        for (std::uint32_t dense = 0; dense < sceneResources.materialCount(); ++dense)
        {
            auto row = sceneResources.materialRow(dense);
            for (std::size_t texture = 0; texture < 5; ++texture)
            {
                const auto denseTexture = row.textureIndices[texture];
                row.textureIndices[texture] = denseTexture < bindlessTextureHandles.size() &&
                        bindlessTextureHandles[denseTexture].valid()
                    ? bindlessTextureHandles[denseTexture].index()
                    : bindlessTable.table().defaultHandle(sampledType).index();
            }
            bindlessMaterialRows[dense] = row;
        }
        gpuMaterialCount = static_cast<std::uint32_t>(bindlessMaterialRows.size());
        if (!bindlessMaterialRows.empty())
        {
            const auto upload = gpuSceneBuffers.uploadMaterials(frames.front().commandPool,
                graphicsQueue, gpuUploader, bindlessMaterialRows);
            if (!upload) return fail(upload.error().describe());
        }
        return ok();
    }

    void cleanupSwapchain() noexcept
    {
        csmDepthPipeline.destroy();
        gbufferPipeline.destroy();
        gbufferDoubleSidedPipeline.destroy();
        deferredLightingPipeline.destroy();
        transparentPipeline.destroy();
        transparentDoubleSidedPipeline.destroy();
        taaPipeline.destroy();
        tonemapPipeline.destroy();
        clusterBuildPipeline.destroy();
        frustumCullPipeline.destroy();
        indirectBuildPipeline.destroy();
        gpuDrivenGbufferPipeline.destroy();
        gpuDrivenBindless = false;
        hizBuildPipeline.destroy();
        occlusionPhase1Pipeline.destroy();
        occlusionPhase2Pipeline.destroy();
        if (device != VK_NULL_HANDLE && gpuSceneDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, gpuSceneDescriptorPool, nullptr);
        if (device != VK_NULL_HANDLE && gpuSceneCullLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, gpuSceneCullLayout, nullptr);
        if (device != VK_NULL_HANDLE && gpuSceneIndirectLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, gpuSceneIndirectLayout, nullptr);
        if (device != VK_NULL_HANDLE && gpuSceneGraphicsLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, gpuSceneGraphicsLayout, nullptr);
        if (device != VK_NULL_HANDLE && hizLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, hizLayout, nullptr);
        if (device != VK_NULL_HANDLE && occlusionPhase1Layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, occlusionPhase1Layout, nullptr);
        if (device != VK_NULL_HANDLE && occlusionPhase2Layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, occlusionPhase2Layout, nullptr);
        gpuSceneDescriptorPool = VK_NULL_HANDLE;
        gpuSceneCullLayout = VK_NULL_HANDLE;
        gpuSceneIndirectLayout = VK_NULL_HANDLE;
        gpuSceneGraphicsLayout = VK_NULL_HANDLE;
        hizLayout = VK_NULL_HANDLE;
        occlusionPhase1Layout = VK_NULL_HANDLE;
        occlusionPhase2Layout = VK_NULL_HANDLE;
        m3FrameResources.reset();
        frameGraphProvider.recreatePersistent();
        swapchainState.cleanup();
    }

    void cleanupM3Descriptors() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            for (auto pool : m3DescriptorPools)
                if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
            if (m3LinearSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, m3LinearSampler, nullptr);
            if (m3MaterialLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3MaterialLayout, nullptr);
            if (m3LightingLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3LightingLayout, nullptr);
            if (m3TaaLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3TaaLayout, nullptr);
            if (m3ClusterLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3ClusterLayout, nullptr);
            if (m3TonemapLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, m3TonemapLayout, nullptr);
        }
        m3DescriptorPool = VK_NULL_HANDLE;
        m3DescriptorPools.clear();
        m3LinearSampler = VK_NULL_HANDLE;
        m3MaterialLayout = VK_NULL_HANDLE;
        m3LightingLayout = VK_NULL_HANDLE;
        m3TaaLayout = VK_NULL_HANDLE;
        m3ClusterLayout = VK_NULL_HANDLE;
        m3TonemapLayout = VK_NULL_HANDLE;
    }

    void cleanup() noexcept
    {
        if (device != VK_NULL_HANDLE)
        {
            // Waiting is best effort during error cleanup; every child object
            // is still destroyed even when the device has already been lost.
            (void)vkDeviceWaitIdle(device);
            cleanupSwapchain();
            cleanupM3Descriptors();
            frameGraphProvider.shutdown();
            sceneResources.cleanup();
            gpuSceneBuffers.cleanup();
            bindlessTable.shutdown();
            for (auto& readback : clusterOverflowReadbacks)
                gpuAllocator.destroy(readback);
            clusterOverflowReadbacks.clear();
            for (auto& readback : gpuVisibilityReadbacks)
                gpuAllocator.destroy(readback);
            gpuVisibilityReadbacks.clear();
            gpuVisibilityValid.clear();
            gpuReferenceVisible.clear();
            for (auto& readback : instanceIdReadbacks)
                gpuAllocator.destroy(readback);
            instanceIdReadbacks.clear();
            instanceIdReadbackValid.clear();
            instanceIdReadbackFrameIndices.clear();
            for (auto& frameUploads : frameUploadBuffers)
                for (auto& upload : frameUploads)
                    gpuAllocator.destroy(upload);
            frameUploadBuffers.clear();
            frameContext.cleanup(device);
            gpuAllocator.shutdown();
        }
        deviceState.cleanup();
        initialized = false;
        deviceLost = false;
        fatalError = false;
        deviceMemoryBytes = 0;
        timestampsEnabled = false;
        currentFrame = 0;
        framebufferResized = false;
        requestedExtent = {};
        window = nullptr;
        overlayCallback = nullptr;
        taaHistoryValid = false;
        taaHistoryFlip = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
        hasRenderedFrame = false;
        lastFrameIndex = 0;
        renderSerial = 0;
        previousInstances.clear();
        previousViewProjection = glm::mat4{1.0f};
        previousPacketValid = false;
        pendingScreenshotPath.clear();
        gpuSceneContentHash = 0;
        gpuSceneInstanceCount = 0;
        gpuMaterialCount = 0;
        materialDescriptorBindCount = 0;
        bindlessTextureHandles.clear();
        bindlessSamplerHandle = {};
        bindlessMaterialRows.clear();
        gpuSceneState.reset(131072);
    }

    [[nodiscard]] VoidResult createTimelineSemaphore()
    {
        return frameContext.createTimeline(device);
    }

    [[nodiscard]] VoidResult createFrameResources()
    {
        const VoidResult result = frameContext.createResources(
            device, physicalDevice, physicalProperties, graphicsQueueFamily, config.framesInFlight);
        timestampsEnabled = result && physicalProperties.limits.timestampPeriod > 0.0f;
        if (!result) return result;
        clusterOverflowReadbacks.clear();
        clusterOverflowReadbacks.reserve(frames.size());
        gpuVisibilityReadbacks.clear();
        gpuVisibilityReadbacks.reserve(frames.size());
        gpuVisibilityValid.assign(frames.size(), false);
        gpuReferenceVisible.assign(frames.size(), {});
        instanceIdReadbacks.assign(frames.size(), {});
        instanceIdReadbackValid.assign(frames.size(), false);
        instanceIdReadbackFrameIndices.assign(frames.size(), 0);
        frameUploadBuffers.clear();
        frameUploadBuffers.resize(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = sizeof(std::uint32_t);
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto allocation = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!allocation)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                return allocation.error();
            }
            clusterOverflowReadbacks.push_back(allocation.value());
            // Header: frustum candidate count, phase-1 visible count, and
            // phase-2 visible count. The two following fixed-capacity regions
            // contain the slot indices, allowing a completed frame to be
            // compared against the CPU reference without stalling the
            // rendering frame.
            info.size = sizeof(std::uint32_t) *
                (VisibilityReadbackHeaderCount + 2u * VisibilityReadbackCapacity);
            const auto visibility = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
            if (!visibility)
            {
                for (auto& readback : clusterOverflowReadbacks)
                    gpuAllocator.destroy(readback);
                clusterOverflowReadbacks.clear();
                for (auto& readback : gpuVisibilityReadbacks)
                    gpuAllocator.destroy(readback);
                gpuVisibilityReadbacks.clear();
                return visibility.error();
            }
            gpuVisibilityReadbacks.push_back(visibility.value());
        }
        return ok();
    }

    [[nodiscard]] VoidResult createSwapchain()
    {
        const VoidResult result = swapchainState.create();
        if (!result)
        {
            return result;
        }
        const VoidResult resourceResult = m3FrameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
        const VoidResult pipelineResult = createGraphicsPipeline();
        if (pipelineResult)
        {
            const VoidResult gpuResult = createGpuDrivenPipelines();
            if (!gpuResult) return gpuResult;
        }
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        return pipelineResult;
    }

    [[nodiscard]] VoidResult recreateSwapchain()
    {
        const VoidResult result = swapchainState.recreate();
        deviceLost = deviceLost || swapchainState.deviceLost;
        if (!result)
        {
            return result;
        }
        // FrameGraph persistent images (TAA history and procedural IBL) are
        // keyed by their descriptor, which includes the swapchain extent for
        // history targets.  The swapchain recreation waits for the device,
        // so it is now safe to retire and rebuild those native allocations
        // before the next graph materialization.  Keeping the old cache here
        // would alias a resized history image with the previous extent.
        frameGraphProvider.recreatePersistent();
        const VoidResult resourceResult = m3FrameResources.recreate(swapchainExtent);
        if (!resourceResult)
        {
            return resourceResult;
        }
        const VoidResult pipelineResult = createGraphicsPipeline();
        if (pipelineResult)
        {
            const VoidResult gpuResult = createGpuDrivenPipelines();
            if (!gpuResult) return gpuResult;
        }
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        // A swapchain resize changes the sampling footprint, so any temporal
        // history must be discarded before the next rendered frame.
        taaHistoryValid = false;
        taaHistoryInitializedA = false;
        taaHistoryInitializedB = false;
        iblInitialized = false;
        hasRenderedFrame = false;
        previousPacketValid = false;
        previousInstances.clear();
        return pipelineResult;
    }

    [[nodiscard]] VoidResult createGraphicsPipeline()
    {
        const std::array<DescriptorBindingDesc, 7> materialAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 13> lightingAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 2> tonemapAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 5> taaAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const std::array<DescriptorBindingDesc, 5> clusterAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const std::array<VkFormat, 5> gbufferFormats = {
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16_SFLOAT,
            VK_FORMAT_R32_UINT};
        GraphicsPipelineDesc gbufferDesc{};
        gbufferDesc.colorFormats = gbufferFormats;
        gbufferDesc.depthFormat = depthFormat;
        gbufferDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        gbufferDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3MaterialLayout, 1};
        gbufferDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> gbufferPushRanges = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(M3PushConstants)}};
        gbufferDesc.pushConstants = gbufferPushRanges;
        gbufferDesc.vertexShader = "pbr.vert.spv";
        gbufferDesc.fragmentShader = "gbuffer.frag.spv";
        const auto gbufferResult = gbufferPipeline.createGraphics(device, gbufferDesc);
        if (!gbufferResult) return gbufferResult;
        GraphicsPipelineDesc gbufferDoubleDesc = gbufferDesc;
        gbufferDoubleDesc.cullMode = VK_CULL_MODE_NONE;
        const auto gbufferDoubleResult = gbufferDoubleSidedPipeline.createGraphics(device, gbufferDoubleDesc);
        if (!gbufferDoubleResult) return gbufferDoubleResult;

        const std::array<VkFormat, 1> hdrFormat = {VK_FORMAT_R16G16B16A16_SFLOAT};
        GraphicsPipelineDesc deferredDesc{};
        deferredDesc.colorFormats = hdrFormat;
        deferredDesc.depthFormat = VK_FORMAT_UNDEFINED;
        deferredDesc.depthTest = false;
        deferredDesc.depthWrite = false;
        deferredDesc.cullMode = VK_CULL_MODE_NONE;
        deferredDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3LightingLayout, 1};
        deferredDesc.descriptorBindings = lightingAbi;
        const std::array<VkPushConstantRange, 1> deferredPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128}};
        deferredDesc.pushConstants = deferredPushRange;
        deferredDesc.vertexShader = "fullscreen.vert.spv";
        deferredDesc.fragmentShader = "pbr.frag.spv";
        const auto deferredResult = deferredLightingPipeline.createGraphics(device, deferredDesc);
        if (!deferredResult) return deferredResult;

        GraphicsPipelineDesc transparentDesc = deferredDesc;
        transparentDesc.depthFormat = depthFormat;
        transparentDesc.depthTest = true;
        transparentDesc.depthWrite = false;
        transparentDesc.blendEnable = true;
        // Transparency has a distinct push-constant ABI (camera/light data
        // precedes the model matrix), so it must use its matching vertex
        // stage instead of reinterpreting the opaque pbr.vert constants.
        transparentDesc.vertexShader = "forward_transparent.vert.spv";
        transparentDesc.fragmentShader = "forward_transparent.frag.spv";
        transparentDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3MaterialLayout, 1};
        transparentDesc.descriptorBindings = materialAbi;
        const std::array<VkPushConstantRange, 1> transparentPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(M3TransparentPushConstants)}};
        transparentDesc.pushConstants = transparentPushRange;
        const auto transparentResult = transparentPipeline.createGraphics(device, transparentDesc);
        if (!transparentResult) return transparentResult;
        GraphicsPipelineDesc transparentDoubleDesc = transparentDesc;
        transparentDoubleDesc.cullMode = VK_CULL_MODE_NONE;
        const auto transparentDoubleResult = transparentDoubleSidedPipeline.createGraphics(device, transparentDoubleDesc);
        if (!transparentDoubleResult) return transparentDoubleResult;

        GraphicsPipelineDesc tonemapDesc{};
        tonemapDesc.colorFormats = std::span<const VkFormat>{&swapchainFormat, 1};
        tonemapDesc.depthFormat = VK_FORMAT_UNDEFINED;
        tonemapDesc.depthTest = false;
        tonemapDesc.depthWrite = false;
        tonemapDesc.cullMode = VK_CULL_MODE_NONE;
        tonemapDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3TonemapLayout, 1};
        tonemapDesc.descriptorBindings = tonemapAbi;
        const std::array<VkPushConstantRange, 1> tonemapPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16}};
        tonemapDesc.pushConstants = tonemapPushRange;
        tonemapDesc.vertexShader = "fullscreen.vert.spv";
        tonemapDesc.fragmentShader = "tonemap.frag.spv";
        const auto tonemapResult = tonemapPipeline.createGraphics(device, tonemapDesc);
        if (!tonemapResult) return tonemapResult;

        GraphicsPipelineDesc csmDesc{};
        csmDesc.depthOnly = true;
        csmDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
        csmDesc.depthTest = true;
        csmDesc.depthWrite = true;
        csmDesc.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        csmDesc.cullMode = VK_CULL_MODE_BACK_BIT;
        csmDesc.depthBiasEnable = true;
        csmDesc.depthBiasConstant = -1.25f;
        csmDesc.depthBiasSlope = -1.75f;
        const std::array<VkPushConstantRange, 1> csmPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4) * 2}};
        csmDesc.pushConstants = csmPushRange;
        csmDesc.vertexShader = "csm.vert.spv";
        const auto csmResult = csmDepthPipeline.createGraphics(device, csmDesc);
        if (!csmResult) return csmResult;

        ComputePipelineDesc taaDesc{};
        taaDesc.shader = "taa.comp.spv";
        taaDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3TaaLayout, 1};
        taaDesc.descriptorBindings = taaAbi;
        const std::array<VkPushConstantRange, 1> taaPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}};
        taaDesc.pushConstants = taaPushRange;
        const auto taaResult = taaPipeline.createCompute(device, taaDesc);
        if (!taaResult) return taaResult;
        ComputePipelineDesc clusterDesc{};
        clusterDesc.shader = "cluster_build.comp.spv";
        clusterDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&m3ClusterLayout, 1};
        clusterDesc.descriptorBindings = clusterAbi;
        const std::array<VkPushConstantRange, 1> clusterPushRange = {
            VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48}};
        clusterDesc.pushConstants = clusterPushRange;
        const auto clusterResult = clusterBuildPipeline.createCompute(device, clusterDesc);
        if (!clusterResult) return clusterResult;
        return ok();
    }

    [[nodiscard]] VoidResult createGpuDrivenPipelines()
    {
        if (!config.enableGpuDrivenScene)
            return ok();
        const auto makeLayout = [&](std::span<const VkDescriptorSetLayoutBinding> bindings,
                                    VkDescriptorSetLayout& output) -> VoidResult
        {
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            return vkCreateDescriptorSetLayout(device, &info, nullptr, &output) == VK_SUCCESS
                       ? ok()
                       : fail("failed to create GPU scene descriptor layout");
        };
        const std::array<VkDescriptorSetLayoutBinding, 5> cullBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const std::array<VkDescriptorSetLayoutBinding, 10> indirectBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        auto result = makeLayout(cullBindings, gpuSceneCullLayout);
        if (!result) return result;
        result = makeLayout(indirectBindings, gpuSceneIndirectLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 4> graphicsBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        result = makeLayout(graphicsBindings, gpuSceneGraphicsLayout);
        if (!result) return result;
        const std::array<VkDescriptorPoolSize, 1> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &gpuSceneDescriptorPool) != VK_SUCCESS)
            return fail("failed to create GPU scene descriptor pool");
        const std::array<DescriptorBindingDesc, 5> cullAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc cullDesc{};
        cullDesc.shader = "frustum_cull.comp.spv";
        cullDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&gpuSceneCullLayout, 1};
        cullDesc.descriptorBindings = cullAbi;
        const std::array<VkPushConstantRange, 1> cullPush = {{
            {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112}}};
        cullDesc.pushConstants = cullPush;
        result = frustumCullPipeline.createCompute(device, cullDesc);
        if (!result) return result;
        const bool useBindless = bindlessTable.initialized() &&
            bindlessTable.table().capacity(Resources::DescriptorType::SampledImage) >=
                kGpuDrivenSampledImageCapacity &&
            bindlessTable.table().capacity(Resources::DescriptorType::Sampler) >=
                kGpuDrivenSamplerCapacity;
        std::array<VkDescriptorSetLayout, 2> gpuGraphicsLayouts =
            {useBindless ? bindlessTable.layout() : m3MaterialLayout, gpuSceneGraphicsLayout};
        std::array<DescriptorBindingDesc, 10> gpuGraphicsAbi{};
        if (useBindless)
        {
            gpuGraphicsAbi = {
                DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    kGpuDrivenSampledImageCapacity,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_SAMPLER,
                    kGpuDrivenSamplerCapacity,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{1, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                DescriptorBindingDesc{1, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                DescriptorBindingDesc{1, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}},
                DescriptorBindingDesc{1, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}},
                {}, {}, {}, {}};
        }
        else
        {
            for (std::uint32_t i = 0; i < 7; ++i)
                gpuGraphicsAbi[i] = DescriptorBindingDesc{0,
                    {i < 5 ? i : (i == 5 ? 10u : 30u),
                        i == 5 ? VK_DESCRIPTOR_TYPE_SAMPLER
                               : (i == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                        1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
            gpuGraphicsAbi[7] = DescriptorBindingDesc{1,
                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
            gpuGraphicsAbi[8] = DescriptorBindingDesc{1,
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
            gpuGraphicsAbi[9] = DescriptorBindingDesc{1,
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        }
        GraphicsPipelineDesc gpuGraphics{};
        const std::array<VkFormat, 5> gpuFormats = {VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_UINT};
        gpuGraphics.colorFormats = gpuFormats;
        gpuGraphics.depthFormat = depthFormat;
        gpuGraphics.descriptorLayouts = gpuGraphicsLayouts;
        gpuGraphics.descriptorBindings = gpuGraphicsAbi;
        const std::array<VkPushConstantRange, 1> gpuPush = {{
            {VK_SHADER_STAGE_VERTEX_BIT, 0, 128}}};
        gpuGraphics.pushConstants = gpuPush;
        gpuGraphics.vertexShader = "gpu_driven.vert.spv";
        gpuGraphics.fragmentShader = useBindless ? "gpu_driven.frag.spv" : "gbuffer.frag.spv";
        result = gpuDrivenGbufferPipeline.createGraphics(device, gpuGraphics);
        if (!result) return result;
        gpuDrivenBindless = useBindless;
        const std::array<DescriptorBindingDesc, 10> indirectAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc indirectDesc{};
        indirectDesc.shader = "build_indirect_commands.comp.spv";
        indirectDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&gpuSceneIndirectLayout, 1};
        indirectDesc.descriptorBindings = indirectAbi;
        const std::array<VkPushConstantRange, 1> indirectPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        indirectDesc.pushConstants = indirectPush;
        result = indirectBuildPipeline.createCompute(device, indirectDesc);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> hizBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(hizBindings, hizLayout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 2> hizAbi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc hizDesc{};
        hizDesc.shader = "hiz_build.comp.spv";
        hizDesc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&hizLayout, 1};
        hizDesc.descriptorBindings = hizAbi;
        const std::array<VkPushConstantRange, 1> hizPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
        hizDesc.pushConstants = hizPush;
        result = hizBuildPipeline.createCompute(device, hizDesc);
        if (!result) return result;

        // Two-phase occlusion uses the frustum-visible list as candidates,
        // then classifies those candidates against the previous/current Hi-Z
        // pyramid.  Keep the layouts explicit so descriptor ABI validation
        // catches shader changes at pipeline creation time.
        const std::array<VkDescriptorSetLayoutBinding, 8> phase1Bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(phase1Bindings, occlusionPhase1Layout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 8> phase1Abi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc phase1Desc{};
        phase1Desc.shader = "occlusion_phase1.comp.spv";
        phase1Desc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&occlusionPhase1Layout, 1};
        phase1Desc.descriptorBindings = phase1Abi;
        const std::array<VkPushConstantRange, 1> occlusionPush = {{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96}}};
        phase1Desc.pushConstants = occlusionPush;
        result = occlusionPhase1Pipeline.createCompute(device, phase1Desc);
        if (!result) return result;

        const std::array<VkDescriptorSetLayoutBinding, 6> phase2Bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = makeLayout(phase2Bindings, occlusionPhase2Layout);
        if (!result) return result;
        const std::array<DescriptorBindingDesc, 6> phase2Abi = {
            DescriptorBindingDesc{0, {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}},
            DescriptorBindingDesc{0, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        ComputePipelineDesc phase2Desc{};
        phase2Desc.shader = "occlusion_phase2.comp.spv";
        phase2Desc.descriptorLayouts = std::span<const VkDescriptorSetLayout>{&occlusionPhase2Layout, 1};
        phase2Desc.descriptorBindings = phase2Abi;
        phase2Desc.pushConstants = occlusionPush;
        return occlusionPhase2Pipeline.createCompute(device, phase2Desc);
    }

    [[nodiscard]] VoidResult createM3Descriptors()
    {
        if (m3LightingLayout != VK_NULL_HANDLE)
            return ok();
        const auto createLayout = [&](std::span<const VkDescriptorSetLayoutBinding> bindings,
                                      VkDescriptorSetLayout& output) -> VoidResult
        {
            VkDescriptorSetLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr, &output);
            return result == VK_SUCCESS ? ok() : fail(vkFailure("vkCreateDescriptorSetLayout", result));
        };
        std::array<VkDescriptorSetLayoutBinding, 7> materialBindings{};
        for (std::uint32_t i = 0; i < 5; ++i)
            materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        materialBindings[5] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        materialBindings[6] = {30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        auto result = createLayout(materialBindings, m3MaterialLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 13> lightingBindings{};
        for (std::uint32_t i = 0; i < 8; ++i)
            lightingBindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[8] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[9] = {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[10] = {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[11] = {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lightingBindings[12] = {23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        result = createLayout(lightingBindings, m3LightingLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> taaBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(taaBindings, m3TaaLayout);
        if (!result) return result;
        std::array<VkDescriptorSetLayoutBinding, 5> clusterBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        result = createLayout(clusterBindings, m3ClusterLayout);
        if (!result) return result;
        const std::array<VkDescriptorSetLayoutBinding, 2> tonemapBindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        result = createLayout(tonemapBindings, m3TonemapLayout);
        if (!result) return result;
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &m3LinearSampler) != VK_SUCCESS)
            return fail("failed to create M3 linear sampler");
        const std::array<VkDescriptorPoolSize, 5> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 128},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        const std::uint32_t poolCount = std::max(1u, config.framesInFlight);
        m3DescriptorPools.reserve(poolCount);
        for (std::uint32_t i = 0; i < poolCount; ++i)
        {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            {
                cleanupM3Descriptors();
                return fail("failed to create M3 descriptor pool");
            }
            m3DescriptorPools.push_back(pool);
        }
        m3DescriptorPool = m3DescriptorPools.front();
        return ok();
    }

    [[nodiscard]] VoidResult recordM3Frame(
        FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet,
        VkBuffer screenshotReadback);

    [[nodiscard]] VoidResult recordImageUpload(VkCommandBuffer commandBuffer,
        VkImage image,
        std::span<const std::uint16_t> data,
        std::span<const VkBufferImageCopy> copies)
    {
        if (commandBuffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE || data.empty() ||
            copies.empty() || currentFrame >= frameUploadBuffers.size())
        {
            return fail("invalid procedural IBL upload state",
                Halcyon::ErrorCode::InvalidState);
        }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = data.size_bytes();
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const auto allocation = gpuAllocator.createBuffer(info, MemoryUsage::CpuToGpu);
        if (!allocation)
        {
            return allocation.error();
        }
        BufferAllocation staging = allocation.value();
        const auto bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(data.data()), data.size_bytes()};
        const auto write = gpuAllocator.writeBuffer(staging, bytes);
        if (!write)
        {
            gpuAllocator.destroy(staging);
            return write.error();
        }
        try
        {
            frameUploadBuffers[currentFrame].push_back(staging);
        }
        catch (...)
        {
            gpuAllocator.destroy(staging);
            return fail("failed to retain procedural IBL staging allocation",
                Halcyon::ErrorCode::OutOfMemory);
        }
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()),
            copies.data());
        return ok();
    }

    [[nodiscard]] VoidResult captureScreenshot(const std::filesystem::path& path)
    {
        if (!initialized || device == VK_NULL_HANDLE || path.empty())
        {
            return fail("A screenshot requires an initialized renderer and a non-empty path",
                Halcyon::ErrorCode::InvalidState);
        }
        if (!pendingScreenshotPath.empty())
        {
            return fail("A screenshot is already queued for the next rendered frame",
                Halcyon::ErrorCode::InvalidState);
        }
        pendingScreenshotPath = path;
        return ok();
    }

    [[nodiscard]] VoidResult writeScreenshot(
        const BufferAllocation& readback, const std::filesystem::path& path)
    {
        const VkDeviceSize size = static_cast<VkDeviceSize>(swapchainExtent.width) *
                                  static_cast<VkDeviceSize>(swapchainExtent.height) * 4u;
        auto bytes = gpuAllocator.readBuffer(readback, 0, size);
        if (!bytes)
        {
            return bytes.error();
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size));
        const auto* src = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
        for (std::uint32_t y = 0; y < swapchainExtent.height; ++y)
        {
            for (std::uint32_t x = 0; x < swapchainExtent.width; ++x)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(y) * swapchainExtent.width + x) * 4u;
                const bool bgra = swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                                  swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM;
                rgba[index + 0] = bgra ? src[index + 2] : src[index + 0];
                rgba[index + 1] = src[index + 1];
                rgba[index + 2] = bgra ? src[index + 0] : src[index + 2];
                rgba[index + 3] = src[index + 3];
            }
        }
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        if (stbi_write_png(path.string().c_str(),
                static_cast<int>(swapchainExtent.width),
                static_cast<int>(swapchainExtent.height),
                4,
                rgba.data(),
                static_cast<int>(swapchainExtent.width * 4u)) == 0)
        {
            return fail("Failed to write screenshot", Halcyon::ErrorCode::Io);
        }
        return ok();
    }

    [[nodiscard]] FrameStats render(const FramePacket& packet)
    {
        HALCYON_PROFILE_SCOPE("Renderer::render");
        FrameStats stats{};
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.quality.exposure = config.exposure;
        stats.quality.taaEnabled = config.enableTaa;
        stats.quality.clusteredLightingEnabled = config.enableClusteredLighting;
        stats.quality.transparencyEnabled = config.enableTransparency;
        // SceneManager resolves stable handles before submission. Rendering
        // only consumes contiguous resource-table indices.
        for (const auto& drawInstance : packet.instances)
        {
            if (const MeshResource* mesh = sceneResources.mesh(drawInstance.meshId); mesh != nullptr)
            {
                stats.primitiveCount += mesh->indexCount / 3u;
            }
        }
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (!stats.taaHistoryValid)
        {
            // The compute pass receives a zero history weight whenever the
            // frame stream is discontinuous, after a resize, or after scene
            // mutation. This is a deterministic history state transition.
            taaHistoryValid = false;
        }
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        const auto begin = std::chrono::steady_clock::now();
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr)
        {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (!initialized || device == VK_NULL_HANDLE || deviceLost)
        {
            if (deviceLost)
            {
                // Device loss is not recoverable for this renderer instance;
                // make the public state agree with the documented policy even
                // when the loss was observed while waiting for a resize.
                initialized = false;
                fatalError = true;
            }
            if (initialized && !deviceLost && swapchain == VK_NULL_HANDLE && framebufferWidth > 0 &&
                framebufferHeight > 0)
            {
                framebufferResized = true;
            }
            stats.minimized = framebufferWidth <= 0 || framebufferHeight <= 0;
            stats.deviceLost = deviceLost;
            stats.fatalError = fatalError;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        // Acquiring from or presenting to a zero-sized GLFW framebuffer is
        // neither useful nor portable.  Preserve the old swapchain while the
        // window is minimized and recreate it after the first non-zero frame.
        if (framebufferWidth <= 0 || framebufferHeight <= 0)
        {
            framebufferResized = true;
            stats.minimized = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        if (framebufferResized || swapchain == VK_NULL_HANDLE)
        {
            const VoidResult resizeResult = recreateSwapchain();
            if (!resizeResult)
            {
                setError(resizeResult.error().describe());
                stats.deviceLost = deviceLost;
                if (deviceLost)
                {
                    initialized = false;
                    fatalError = true;
                }
                stats.fatalError = fatalError;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            lastError.clear();
            stats.recreatedSwapchain = !swapchainImageViews.empty();
            if (swapchain == VK_NULL_HANDLE || swapchainExtent.width == 0 ||
                swapchainExtent.height == 0)
            {
                stats.minimized = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
        }

        auto& frame = frames[currentFrame];
        VkResult result = frameContext.wait(device, frame);
        if (result != VK_SUCCESS)
        {
            setError(vkFailure("vkWaitForFences", result));
            if (result == VK_ERROR_DEVICE_LOST)
            {
                deviceLost = true;
            }
            fatalError = true;
            initialized = false;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (currentFrame < frameUploadBuffers.size())
        {
            for (auto& upload : frameUploadBuffers[currentFrame])
                gpuAllocator.destroy(upload);
            frameUploadBuffers[currentFrame].clear();
        }
        // Resources retired by the previous command buffers become safe to
        // destroy once the frame-slot fence has signalled.  Keep the provider
        // on the same serial as the frame scheduler so transient images and
        // views cannot be freed while still referenced by the GPU.
        if (renderSerial >= config.framesInFlight)
        {
            frameGraphProvider.collectCompleted(renderSerial - config.framesInFlight);
        }
        frameGraphProvider.beginFrame(renderSerial);
        if (timestampsEnabled && frame.submitted)
        {
            double gpuMilliseconds = -1.0;
            result = frameContext.readGpuTime(device, frame, gpuMilliseconds);
            if (result == VK_SUCCESS)
            {
                stats.gpuFrameMs = gpuMilliseconds;
            }
            for (std::uint32_t passIndex = 0;
                passIndex < frame.passNames.size() && passIndex < frameContext.maxPassCount;
                ++passIndex)
            {
                double passMilliseconds = -1.0;
                if (frameContext.readPassTime(device, frame, passIndex, passMilliseconds) ==
                    VK_SUCCESS)
                {
                    stats.gpuPasses.push_back({frame.passNames[passIndex], passMilliseconds});
                    const auto& name = frame.passNames[passIndex];
                    if (name == "Hi-Z build") {
                        stats.gpuHiZBuildMs = passMilliseconds;
                        if (config.enableTwoPhaseOcclusion)
                            stats.gpuTwoPhaseMs = passMilliseconds;
                    }
                }
            }
            double stageMilliseconds = -1.0;
            if (frameContext.readStageTime(device, frame, 0, stageMilliseconds) == VK_SUCCESS)
                stats.gpuFrustumCullMs = stageMilliseconds;
            if (frameContext.readStageTime(device, frame, 1, stageMilliseconds) == VK_SUCCESS)
                stats.gpuIndirectBuildMs = stageMilliseconds;
            if (frameContext.readStageTime(device, frame, 2, stageMilliseconds) == VK_SUCCESS)
                stats.gpuHiZBuildMs = stageMilliseconds;
            if (config.enableTwoPhaseOcclusion &&
                frameContext.readStageTime(device, frame, 3, stageMilliseconds) == VK_SUCCESS)
                stats.gpuTwoPhaseMs = stageMilliseconds;
        }
        if (frame.submitted &&
            currentFrame < clusterOverflowReadbacks.size())
        {
            const auto overflowBytes = gpuAllocator.readBuffer(
                clusterOverflowReadbacks[currentFrame], 0, sizeof(std::uint32_t));
            if (overflowBytes && overflowBytes.value().size() >= sizeof(std::uint32_t))
            {
                std::uint32_t overflow = 0;
                std::memcpy(&overflow, overflowBytes.value().data(), sizeof(overflow));
                stats.clusterOverflowCount = overflow;
            }
        }
        if (frame.submitted && config.enableGpuDrivenScene &&
            currentFrame < gpuVisibilityReadbacks.size() &&
            currentFrame < gpuVisibilityValid.size() && gpuVisibilityValid[currentFrame])
        {
            gpuSceneBuffers.setFrameIndex(currentFrame);
            const auto counts = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame], 0,
                sizeof(std::uint32_t) * VisibilityReadbackHeaderCount);
            if (counts && counts.value().size() >=
                    sizeof(std::uint32_t) * VisibilityReadbackHeaderCount)
            {
                std::uint32_t frustumCount = 0, firstPhase = 0, secondPhase = 0;
                std::uint32_t indirectCount = 0, phase2IndirectCount = 0;
                std::memcpy(&frustumCount, counts.value().data(), sizeof(frustumCount));
                std::memcpy(&firstPhase, counts.value().data() + sizeof(frustumCount), sizeof(firstPhase));
                std::memcpy(&secondPhase, counts.value().data() + sizeof(frustumCount) * 2u,
                    sizeof(secondPhase));
                std::memcpy(&indirectCount, counts.value().data() + sizeof(frustumCount) * 3u,
                    sizeof(indirectCount));
                std::memcpy(&phase2IndirectCount, counts.value().data() + sizeof(frustumCount) * 4u,
                    sizeof(phase2IndirectCount));
                stats.frustumVisibleInstanceCount = frustumCount;
                stats.occludedInstanceCount = config.enableTwoPhaseOcclusion
                    ? frustumCount >= firstPhase ? frustumCount - firstPhase : 0u : 0u;
                stats.visibleInstanceCount = firstPhase + (config.enableTwoPhaseOcclusion ? secondPhase : 0u);
                stats.indirectDrawCount = indirectCount +
                    (config.enableTwoPhaseOcclusion ? phase2IndirectCount : 0u);

                const auto& expected = currentFrame < gpuReferenceVisible.size()
                    ? gpuReferenceVisible[currentFrame] : std::vector<std::uint32_t>{};
                const std::uint32_t firstCount = std::min(firstPhase, VisibilityReadbackCapacity);
                const std::uint32_t secondCount = std::min(secondPhase, VisibilityReadbackCapacity);
                std::vector<bool> present(gpuSceneInstanceCount, false);
                const auto firstBytes = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame],
                    sizeof(std::uint32_t) * VisibilityReadbackHeaderCount,
                    static_cast<VkDeviceSize>(firstCount) * sizeof(std::uint32_t));
                if (firstBytes)
                {
                    const auto* ids = reinterpret_cast<const std::uint32_t*>(firstBytes.value().data());
                    for (std::uint32_t i = 0; i < firstCount; ++i)
                    {
                        const auto slot = ids[i];
                        if (slot < present.size()) present[slot] = true;
                    }
                }
                if (config.enableTwoPhaseOcclusion)
                {
                    const auto secondBytes = gpuAllocator.readBuffer(gpuVisibilityReadbacks[currentFrame],
                        sizeof(std::uint32_t) *
                            (VisibilityReadbackHeaderCount + VisibilityReadbackCapacity),
                        static_cast<VkDeviceSize>(secondCount) * sizeof(std::uint32_t));
                    if (secondBytes)
                    {
                        const auto* ids = reinterpret_cast<const std::uint32_t*>(secondBytes.value().data());
                        for (std::uint32_t i = 0; i < secondCount; ++i)
                        {
                            const auto slot = ids[i];
                            if (slot < present.size()) present[slot] = true;
                        }
                    }
                }
                if (config.enableTwoPhaseOcclusion)
                {
                    // Hi-Z is allowed to remove genuinely occluded frustum
                    // candidates. Pixel-level reference-vs-occlusion checks
                    // are performed by the InstanceId comparison tool; the
                    // in-frame audit only reports structural readback health
                    // for this mode.
                    stats.gpuVisibilityMissingCount = 0;
                    stats.gpuVisibilityValidationPassed = true;
                }
                else
                {
                    for (const auto slot : expected)
                        if (slot >= present.size() || !present[slot])
                            ++stats.gpuVisibilityMissingCount;
                    stats.gpuVisibilityValidationPassed =
                        stats.gpuVisibilityMissingCount == 0;
                }
            }
        }
        if (frame.submitted && config.enableGpuDrivenScene &&
            currentFrame < instanceIdReadbacks.size() &&
            currentFrame < instanceIdReadbackValid.size() &&
            instanceIdReadbackValid[currentFrame])
        {
            const auto& readback = instanceIdReadbacks[currentFrame];
            const VkDeviceSize pixelBytes = sizeof(std::uint32_t);
            const VkDeviceSize pixelCount = readback.size / pixelBytes;
            if (pixelCount > 0)
            {
                const auto pixels = gpuAllocator.readBuffer(readback, 0,
                    pixelCount * pixelBytes);
                if (pixels)
                {
                    const auto* ids = reinterpret_cast<const std::uint32_t*>(pixels.value().data());
                    std::vector<std::uint32_t> presentIds;
                    if (!config.instanceIdReportPath.empty())
                        presentIds.reserve(static_cast<std::size_t>(pixelCount));
                    for (VkDeviceSize i = 0; i < pixelCount; ++i)
                    {
                        const std::uint32_t encoded = ids[i];
                        if (encoded != 0)
                        {
                            if (encoded - 1u >= gpuSceneInstanceCount)
                                ++stats.gpuInstanceIdInvalidPixelCount;
                            else if (!config.instanceIdReportPath.empty())
                                presentIds.push_back(encoded - 1u);
                        }
                    }
                    if (!config.instanceIdReportPath.empty())
                    {
                        std::sort(presentIds.begin(), presentIds.end());
                        presentIds.erase(std::unique(presentIds.begin(), presentIds.end()),
                            presentIds.end());
                        std::ofstream report(config.instanceIdReportPath, std::ios::app);
                        if (report)
                        {
                            report << instanceIdReadbackFrameIndices[currentFrame];
                            for (const std::uint32_t id : presentIds) report << ',' << id;
                            report << '\n';
                        }
                    }
                }
            }
            instanceIdReadbackValid[currentFrame] = false;
        }
        frame.submitted = false;

        // The frame timeline is also the lifetime source for GPU-scene slots,
        // even when a device uses the legacy material descriptor fallback.
        // Query it independently of bindless-table availability.
        {
            std::uint64_t completedTimeline = 0;
            if (vkGetSemaphoreCounterValue(
                    device, frameContext.timelineSemaphore, &completedTimeline) == VK_SUCCESS)
            {
                if (bindlessTable.initialized())
                    (void)bindlessTable.collect(completedTimeline);
                // GPU-scene slots follow the same submission timeline as
                // bindless descriptors. Reclaiming them here makes a
                // create/destroy-heavy scene safe without forcing a device
                // idle or leaking stable slots until shutdown.
                (void)gpuSceneState.collect(completedTimeline);
            }
        }

        result = swapchainState.acquire(frame.imageAvailable, stats.swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            framebufferResized = true;
            const VoidResult recreateResult = recreateSwapchain();
            if (!recreateResult)
            {
                setError(recreateResult.error().describe());
                if (deviceLost)
                {
                    initialized = false;
                    fatalError = true;
                }
            }
            else
            {
                lastError.clear();
            }
            stats.recreatedSwapchain = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = fatalError;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            setError(vkFailure("vkAcquireNextImageKHR", result));
            if (result == VK_ERROR_DEVICE_LOST)
            {
                deviceLost = true;
            }
            fatalError = true;
            initialized = false;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        stats.suboptimal = result == VK_SUBOPTIMAL_KHR;
        framebufferResized = framebufferResized || stats.suboptimal;
        if (stats.swapchainImageIndex >= presentReadySemaphores.size())
        {
            setError("Acquired swapchain image has no presentation semaphore");
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        // Keep the fence signaled until command recording has succeeded.  If
        // a validation/runtime error occurs while resetting the command pool
        // or recording, the next frame cannot deadlock waiting on an
        // unsignaled fence that was never submitted.
        result = frameContext.resetCommandPool(device, frame);
        if (result != VK_SUCCESS)
        {
            setError(vkFailure("vkResetCommandPool", result));
            stats.deviceLost = result == VK_ERROR_DEVICE_LOST;
            deviceLost = deviceLost || stats.deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        const std::filesystem::path screenshotPath =
            std::exchange(pendingScreenshotPath, std::filesystem::path{});
        BufferAllocation screenshotReadback{};
        if (!screenshotPath.empty())
        {
            VkBufferCreateInfo readbackInfo{};
            readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            readbackInfo.size = static_cast<VkDeviceSize>(swapchainExtent.width) *
                                static_cast<VkDeviceSize>(swapchainExtent.height) * 4u;
            readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            const auto allocation =
                gpuAllocator.createBuffer(readbackInfo, MemoryUsage::GpuToCpu);
            if (!allocation)
            {
                setError(allocation.error().describe());
                initialized = false;
                fatalError = true;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            screenshotReadback = allocation.value();
        }

        // The debug InstanceId attachment is copied asynchronously from the
        // Present pass. Allocate it lazily per frame slot because its size is
        // swapchain-dependent and keep it alive until that slot's fence has
        // completed on a later frame.
        if (config.enableGpuDrivenScene && currentFrame < instanceIdReadbacks.size())
        {
            const VkDeviceSize instanceIdBytes = static_cast<VkDeviceSize>(swapchainExtent.width) *
                static_cast<VkDeviceSize>(swapchainExtent.height) * sizeof(std::uint32_t);
            auto& allocation = instanceIdReadbacks[currentFrame];
            if (allocation.buffer == VK_NULL_HANDLE || allocation.size < instanceIdBytes)
            {
                if (allocation.buffer != VK_NULL_HANDLE) gpuAllocator.destroy(allocation);
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                info.size = instanceIdBytes;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                const auto created = gpuAllocator.createBuffer(info, MemoryUsage::GpuToCpu);
                if (!created)
                {
                    gpuAllocator.destroy(screenshotReadback);
                    setError(created.error().describe());
                    initialized = false;
                    fatalError = true;
                    stats.fatalError = true;
                    stats.cpuFrameMs = elapsedMilliseconds(begin);
                    return stats;
                }
                allocation = created.value();
            }
            instanceIdReadbackValid[currentFrame] = false;
        }

        const VoidResult recordResult = recordM3Frame(frame, stats.swapchainImageIndex, packet,
            screenshotReadback.buffer);
        if (!recordResult)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(recordResult.error().describe());
            stats.deviceLost = deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        stats.executedPasses = frame.passNames;
        stats.materialDescriptorBindCount = materialDescriptorBindCount;
        stats.gpuDrivenActive = gpuDrivenActive;
        stats.gpuFallbackInstanceCount = gpuFallbackInstanceCount;
        if (!config.enableGpuDrivenScene)
        {
            // The legacy path submits the extracted scene directly on the
            // CPU and has no GPU visibility result. Expose its submitted
            // instance count so stress CSVs remain useful for A/B analysis.
            stats.visibleInstanceCount = static_cast<std::uint32_t>(packet.instances.size());
            stats.frustumVisibleInstanceCount = stats.visibleInstanceCount;
            stats.indirectDrawCount = 0;
            stats.occludedInstanceCount = 0;
        }
        else if (!gpuDrivenActive)
        {
            stats.visibleInstanceCount = static_cast<std::uint32_t>(packet.instances.size());
            stats.indirectDrawCount = 0;
        }

        result = frameContext.resetFence(device, frame);
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(vkFailure("vkResetFences", result));
            stats.deviceLost = result == VK_ERROR_DEVICE_LOST;
            deviceLost = deviceLost || stats.deviceLost;
            initialized = false;
            fatalError = true;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }

        result = frameContext.submit(
            graphicsQueue, frame, presentReadySemaphores[stats.swapchainImageIndex]);
        if (result != VK_SUCCESS)
        {
            gpuAllocator.destroy(screenshotReadback);
            setError(vkFailure("vkQueueSubmit2", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            return stats;
        }
        ++renderSerial;
        swapchainImageInitialized[stats.swapchainImageIndex] = true;
        result = frameContext.present(presentQueue,
            swapchain,
            presentReadySemaphores[stats.swapchainImageIndex],
            stats.swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            framebufferResized = true;
            stats.suboptimal = true;
        }
        else if (result != VK_SUCCESS)
        {
            if (screenshotReadback.buffer != VK_NULL_HANDLE)
            {
                (void)frameContext.wait(device, frame);
                gpuAllocator.destroy(screenshotReadback);
            }
            setError(vkFailure("vkQueuePresentKHR", result));
            deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
            initialized = false;
            fatalError = true;
            stats.deviceLost = deviceLost;
            stats.fatalError = true;
            stats.cpuFrameMs = elapsedMilliseconds(begin);
            currentFrame = (currentFrame + 1) % static_cast<std::uint32_t>(frames.size());
            return stats;
        }

        if (screenshotReadback.buffer != VK_NULL_HANDLE)
        {
            result = frameContext.wait(device, frame);
            if (result != VK_SUCCESS)
            {
                gpuAllocator.destroy(screenshotReadback);
                setError(vkFailure("screenshot frame fence", result));
                deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
                initialized = false;
                fatalError = true;
                stats.deviceLost = deviceLost;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            const VoidResult screenshotResult =
                writeScreenshot(screenshotReadback, screenshotPath);
            gpuAllocator.destroy(screenshotReadback);
            if (!screenshotResult)
            {
                setError(screenshotResult.error().describe());
                initialized = false;
                fatalError = true;
                stats.fatalError = true;
                stats.cpuFrameMs = elapsedMilliseconds(begin);
                return stats;
            }
            stats.screenshotWritten = true;
        }

        currentFrame = (currentFrame + 1) % static_cast<std::uint32_t>(frames.size());
        stats.rendered = true;
        // A completed frame also establishes valid persistent Hi-Z history;
        // this is independent of whether temporal AA is enabled.
        hasRenderedFrame = true;
        // This field tracks renderer-owned VMA allocations, not the physical
        // heap capacity reported in Capabilities.
        deviceMemoryBytes = gpuAllocator.allocatedBytes();
        stats.deviceMemoryBytes = static_cast<std::uint64_t>(deviceMemoryBytes);
        stats.quality.rayQueryEnabled = rayQueryEnabled;
        stats.taaHistoryValid = config.enableTaa && taaHistoryValid && hasRenderedFrame &&
                                packet.frameIndex == lastFrameIndex + 1u;
        if (config.enableTaa)
        {
            taaHistoryValid = true;
            lastFrameIndex = packet.frameIndex;
        }
        previousInstances.assign(packet.instances.begin(), packet.instances.end());
        previousViewProjection = packet.camera.viewProjection;
        previousPacketValid = true;
        stats.cpuFrameMs = elapsedMilliseconds(begin);
        HALCYON_PROFILE_FRAME();
        return stats;
    }

    static double elapsedMilliseconds(const std::chrono::steady_clock::time_point& begin) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - begin).count();
    }
};

VoidResult Renderer::Impl::recordM3Frame(
    FrameContext& frame, std::uint32_t imageIndex, const FramePacket& packet,
    VkBuffer screenshotReadback)
{
    HALCYON_PROFILE_SCOPE("Renderer::recordM3Frame");
    materialDescriptorBindCount = 0;
    if (imageIndex >= swapchainImages.size() || imageIndex >= swapchainImageViews.size())
    {
        return fail("Acquired swapchain image index is out of range");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS)
    {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkBeginCommandBuffer", result));
    }
    if (timestampsEnabled)
    {
        vkCmdResetQueryPool(frame.commandBuffer, frameContext.timestampPool, frame.queryBase,
            2u + frameContext.maxPassCount * 2u + VulkanFrameContext::StageQueryCount);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase);
    }
    if (currentFrame >= frameUploadBuffers.size())
        return fail("GPU scene upload frame slot is out of range");
    const auto sceneUpload = gpuSceneBuffers.recordPendingUploads(
        frame.commandBuffer, gpuAllocator, frameUploadBuffers[currentFrame]);
    if (!sceneUpload) return sceneUpload;
    gpuSceneBuffers.setFrameIndex(currentFrame);

    struct ImportedTarget
    {
        VkImageView view = VK_NULL_HANDLE;
    } importedTarget{swapchainImageViews[imageIndex]};

    Graph::FrameGraph graph;
    graph.setResourceProvider(&frameGraphProvider);
        // renderFrame waited on this slot's fence above, so every descriptor
        // set recorded for the slot is no longer in use. Reusing the pool at
        // this point keeps per-frame compute/graphics allocations bounded.
        if (currentFrame < m3DescriptorPools.size())
        {
            m3DescriptorPool = m3DescriptorPools[currentFrame];
            (void)vkResetDescriptorPool(device, m3DescriptorPool, 0);
        }
    const auto allocateSet = [&](VkDescriptorSetLayout layout) -> VkDescriptorSet
    {
        if (layout == VK_NULL_HANDLE || m3DescriptorPool == VK_NULL_HANDLE)
        {
            setError("M3 descriptor allocation requested with an invalid layout or pool");
            fatalError = true;
            return VK_NULL_HANDLE;
        }
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = m3DescriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult allocationResult = vkAllocateDescriptorSets(device, &allocate, &set);
        if (allocationResult != VK_SUCCESS)
        {
            setError(vkFailure("vkAllocateDescriptorSets", allocationResult));
            fatalError = true;
            return VK_NULL_HANDLE;
        }
        return set;
    };
    const auto writeSampled = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view,
                                  VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
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
    };
    const auto writeSampler = [&](VkDescriptorSet set)
    {
        VkDescriptorImageInfo sampler{m3LinearSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 10;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &sampler;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    };
    const auto writeStorage = [&](VkDescriptorSet set, std::uint32_t binding, VkImageView view)
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
    };
    const auto writeStorageBuffer = [&](VkDescriptorSet set, std::uint32_t binding,
                                         VkBuffer buffer, VkDeviceSize size)
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
    };
    const auto writeGpuStageTimestamp = [&](std::uint32_t stage, bool begin)
    {
        if (timestampsEnabled && stage < 4u)
            vkCmdWriteTimestamp2(frame.commandBuffer,
                begin ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                frameContext.timestampPool, frame.stageQueryBase + stage * 2u + (begin ? 0u : 1u));
    };
    const auto writeUniformBuffer = [&](VkDescriptorSet set, std::uint32_t binding,
                                        VkBuffer buffer, VkDeviceSize size)
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
    };
    VkDescriptorSet gpuCullSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuIndirectSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuGraphicsSet = VK_NULL_HANDLE;
    VkDescriptorSet gpuPhase2GraphicsSet = VK_NULL_HANDLE;
    std::uint32_t gpuMaterialId = 0;
    constexpr std::uint32_t gpuUnsupportedFlags =
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::Transparent) |
        static_cast<std::uint32_t>(Halcyon::Renderer::Scene::Ecs::RenderableFlags::DoubleSided) |
        Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
    const VkBuffer gpuVertexBuffer = sceneResources.gpuDrivenVertexBuffer();
    const VkBuffer gpuIndexBuffer = sceneResources.gpuDrivenIndexBuffer();
    const VkBuffer gpuMeshDrawBuffer = sceneResources.meshDrawBuffer();
    bool gpuIndirectCompatible = config.enableGpuDrivenScene && !packet.instances.empty() &&
        gpuVertexBuffer != VK_NULL_HANDLE && gpuIndexBuffer != VK_NULL_HANDLE &&
        gpuMeshDrawBuffer != VK_NULL_HANDLE;
    std::uint32_t gpuDrivenInstanceCount = 0;
    std::uint32_t cpuFallbackInstanceCount = 0;
    if (gpuIndirectCompatible)
    {
        gpuMaterialId = packet.instances.front().materialId;
        for (const auto& instance : packet.instances)
        {
            const MeshResource* mesh = sceneResources.mesh(instance.meshId);
            const bool unsupportedFlags = (instance.flags & gpuUnsupportedFlags) != 0u;
            const bool materialMismatch = !gpuDrivenBindless &&
                instance.materialId != gpuMaterialId;
            const bool eligible = mesh != nullptr && mesh->indexCount != 0 &&
                !unsupportedFlags && !materialMismatch;
            if (eligible) ++gpuDrivenInstanceCount;
            else ++cpuFallbackInstanceCount;
        }
        gpuIndirectCompatible = gpuDrivenInstanceCount != 0;
    }
    gpuDrivenActive = gpuIndirectCompatible;
    gpuFallbackInstanceCount = cpuFallbackInstanceCount;
    const auto recordGpuDrivenCulling = [&]() -> VoidResult
    {
        if (!config.enableGpuDrivenScene || !gpuIndirectCompatible ||
            frustumCullPipeline.computePipeline() == VK_NULL_HANDLE)
            return ok();
        if (m3DescriptorPool == VK_NULL_HANDLE)
            return fail("per-frame descriptor pool is not initialized");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = m3DescriptorPool;
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
        writeStorageBuffer(gpuIndirectSet, 0, gpuSceneBuffers.visibleIndicesBuffer(), sceneBytes);
        writeStorageBuffer(gpuIndirectSet, 1, gpuSceneBuffers.meshMaterialBuffer(),
            std::max<VkDeviceSize>(4, static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) *
                sizeof(Halcyon::Renderer::Scene::MeshMaterialRow)));
        writeStorageBuffer(gpuIndirectSet, 2, gpuSceneBuffers.indirectCommandsBuffer(),
            std::max<VkDeviceSize>(4, static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(VkDrawIndexedIndirectCommand)));
        writeStorageBuffer(gpuIndirectSet, 3, gpuSceneBuffers.indirectDrawCountBuffer(), sizeof(std::uint32_t));
        writeStorageBuffer(gpuIndirectSet, 4, gpuMeshDrawBuffer,
            static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
        writeStorageBuffer(gpuIndirectSet, 5, gpuSceneBuffers.meshHeadsBuffer(), sceneBytes);
        writeStorageBuffer(gpuIndirectSet, 6, gpuSceneBuffers.meshNextBuffer(), sceneBytes);
        writeStorageBuffer(gpuIndirectSet, 7, gpuSceneBuffers.groupedVisibleIndicesBuffer(), sceneBytes);
        writeStorageBuffer(gpuIndirectSet, 8, gpuSceneBuffers.groupedVisibleCountBuffer(), sizeof(std::uint32_t));
        writeStorageBuffer(gpuIndirectSet, 9, gpuSceneBuffers.visibleCountBuffer(), sizeof(std::uint32_t));
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
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                indirectBuildPipeline.computePipeline());
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                indirectBuildPipeline.layout(), 0, 1, &gpuIndirectSet, 0, nullptr);
            struct IndirectConstants { std::uint32_t instanceCount; std::uint32_t meshCount;
                std::uint32_t mode; std::uint32_t reserved; } indirect{};
            indirect.instanceCount = constants.instanceCount;
            indirect.meshCount = sceneResources.meshDrawCount();
            indirect.mode = 0;
            vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(indirect), &indirect);
            vkCmdDispatch(frame.commandBuffer, (indirect.instanceCount + 63u) / 64u, 1, 1);
            VkBufferMemoryBarrier2 groupBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            groupBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            groupBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            groupBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            groupBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            std::array<VkBufferMemoryBarrier2, 2> groupBarriers{groupBarrier, groupBarrier};
            groupBarriers[0].buffer = gpuSceneBuffers.meshHeadsBuffer(); groupBarriers[0].size = VK_WHOLE_SIZE;
            groupBarriers[1].buffer = gpuSceneBuffers.meshNextBuffer(); groupBarriers[1].size = VK_WHOLE_SIZE;
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(groupBarriers.size());
            dependency.pBufferMemoryBarriers = groupBarriers.data();
            vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
            vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.indirectDrawCountBuffer(), 0,
                sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.groupedVisibleCountBuffer(), 0,
                sizeof(std::uint32_t), 0);
            std::array<VkBufferMemoryBarrier2, 2> buildReset{resetBarrier, resetBarrier};
            buildReset[0].buffer = gpuSceneBuffers.indirectDrawCountBuffer();
            buildReset[1].buffer = gpuSceneBuffers.groupedVisibleCountBuffer();
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buildReset.size());
            dependency.pBufferMemoryBarriers = buildReset.data();
            vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
            indirect.mode = 1;
            vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(indirect), &indirect);
            vkCmdDispatch(frame.commandBuffer,
                (std::max(1u, indirect.meshCount) + 63u) / 64u, 1, 1);
            VkBufferMemoryBarrier2 indirectBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            indirectBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            indirectBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            indirectBarrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            indirectBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                VK_ACCESS_2_SHADER_READ_BIT;
            std::array<VkBufferMemoryBarrier2, 4> indirectBarriers{indirectBarrier, indirectBarrier,
                indirectBarrier, indirectBarrier};
            indirectBarriers[0].buffer = gpuSceneBuffers.indirectCommandsBuffer();
            indirectBarriers[0].size = VK_WHOLE_SIZE;
            indirectBarriers[1].buffer = gpuSceneBuffers.indirectDrawCountBuffer();
            indirectBarriers[1].size = sizeof(std::uint32_t);
            indirectBarriers[2].buffer = gpuSceneBuffers.groupedVisibleIndicesBuffer();
            indirectBarriers[2].size = VK_WHOLE_SIZE;
            indirectBarriers[3].buffer = gpuSceneBuffers.groupedVisibleCountBuffer();
            indirectBarriers[3].size = sizeof(std::uint32_t);
            dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(indirectBarriers.size());
            dependency.pBufferMemoryBarriers = indirectBarriers.data();
            vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
            writeGpuStageTimestamp(1, false);
        }
        return ok();
    };
    const VoidResult gpuDrivenResult = recordGpuDrivenCulling();
    if (!gpuDrivenResult) return gpuDrivenResult;
    const std::uint32_t width = swapchainExtent.width;
    const std::uint32_t height = swapchainExtent.height;
    if (config.enableGpuDrivenScene && currentFrame < gpuReferenceVisible.size())
    {
        // Build the deterministic CPU reference from the same world-space
        // bounds and clip planes consumed by frustum_cull.comp. It is kept per
        // frame slot so readback validation never waits on the current frame.
        const glm::mat4& vp = packet.camera.viewProjection;
        const glm::vec4 rows[4] = {
            {vp[0][0], vp[1][0], vp[2][0], vp[3][0]},
            {vp[0][1], vp[1][1], vp[2][1], vp[3][1]},
            {vp[0][2], vp[1][2], vp[2][2], vp[3][2]},
            {vp[0][3], vp[1][3], vp[2][3], vp[3][3]}};
        std::array<glm::vec4, 6> planes = {
            rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
            rows[3] - rows[1], rows[3] + rows[2], rows[3] - rows[2]};
        for (auto& plane : planes)
        {
            const float length = glm::length(glm::vec3(plane));
            if (length > 1.0e-6f) plane /= length;
        }
        auto& reference = gpuReferenceVisible[currentFrame];
        reference.clear();
        const auto& bounds = gpuSceneState.soa().bounds;
        const auto& meshMaterials = gpuSceneState.soa().meshMaterials;
        const std::uint32_t count = std::min<std::uint32_t>(gpuSceneInstanceCount,
            static_cast<std::uint32_t>(bounds.size()));
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            if (slot >= meshMaterials.size()) continue;
            const auto& material = meshMaterials[slot];
            if ((material.flags & gpuUnsupportedFlags) != 0u ||
                (!gpuDrivenBindless && material.materialIndex != gpuMaterialId))
                continue;
            const auto& sphere = bounds[slot].sphereCenterRadius;
            const glm::vec4 value{sphere[0], sphere[1], sphere[2], sphere[3]};
            if (value.w > 0.0f &&
                Halcyon::Renderer::Scene::sphereInsideFrustum(planes, value))
                reference.push_back(slot);
        }
    }
    constexpr std::uint32_t csmResolution = VulkanM3FrameResources::CsmResolution;
    const auto transitionImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                      std::uint32_t baseLayer = 0, std::uint32_t layerCount = 1)
    {
        if (image == VK_NULL_HANDLE) return;
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
        vkCmdPipelineBarrier2(frame.commandBuffer, &dependency);
    };
    auto declaredResources = m3FrameResources.declare(
        graph, static_cast<std::uint32_t>(packet.lights.size()));
    if (!declaredResources)
    {
        return VoidResult::failure(declaredResources.error());
    }
    auto m3 = declaredResources.value();
    auto shadow = m3.csm;
    auto gbuffer0 = m3.gbuffer0;
    auto gbuffer1 = m3.gbuffer1;
    auto gbuffer2 = m3.gbuffer2;
    auto motion = m3.motion;
    auto instanceId = m3.instanceId;
    auto depth = m3.depth;
    auto hiz = m3.hiz;
    auto hdr = m3.hdr;
    auto historyA = m3.historyA;
    auto historyB = m3.historyB;
    auto irradiance = m3.irradiance;
    auto prefiltered = m3.prefiltered;
    auto brdfLut = m3.brdfLut;
    auto clusterRanges = m3.clusterRanges;
    auto clusterIndices = m3.clusterIndices;
    auto clusterOverflow = m3.clusterOverflow;
    auto lightBuffer = m3.lights;
    auto clusterCamera = m3.clusterCamera;
    auto shadowConstantsBuffer = m3.shadowConstants;
    const std::uint32_t tileCount = m3.clusterCount;

    Graph::FrameGraphRenderPass::ImportDescriptor swapImport{};
    swapImport.attachments = Graph::FrameGraphAttachmentFlags::Color0;
    swapImport.viewport.width = width;
    swapImport.viewport.height = height;
    swapImport.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
    auto output = graph.import("Swapchain", swapImport, {&importedTarget});

    std::array<glm::mat4, 4> cascadeMatrices{};
    glm::vec4 cascadeSplits{0.0f};
    {
        const float nearPlane = std::max(1.0e-3f, packet.camera.positionAndNear.w);
        const float farPlane = packet.camera.forwardAndFar.w > nearPlane
                                   ? packet.camera.forwardAndFar.w
                                   : 1000.0f;
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
                    lightDirection = candidate;
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
            const float split = glm::mix(nearPlane + (farPlane - nearPlane) * p,
                logarithmic, lambda);
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
            for (const auto& point : sliceCorners) center += point;
            center /= 8.0f;
            float radius = 0.0f;
            for (const auto& point : sliceCorners)
                radius = std::max(radius, glm::length(point - center));
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
                depthFar, depthNear) * lightView;
            cascadeSplits[cascade] = split;
            previousSplit = split;
        }
    }

    const auto drawScene = [&](VkPipelineLayout layout,
                                bool shadowPass, bool transparentPass,
                                std::uint32_t cascade = 0u,
                                bool fallbackOnly = false)
    {
        std::vector<const InstanceData*> drawItems;
        drawItems.reserve(packet.instances.size());
        for (const auto& instance : packet.instances)
            drawItems.push_back(&instance);
        if (transparentPass)
        {
            const glm::vec3 cameraPosition = glm::vec3(packet.camera.positionAndNear);
            std::stable_sort(drawItems.begin(), drawItems.end(), [&](const InstanceData* a,
                const InstanceData* b)
            {
                const glm::vec3 aPosition{a->transform[12], a->transform[13], a->transform[14]};
                const glm::vec3 bPosition{b->transform[12], b->transform[13], b->transform[14]};
                return glm::dot(aPosition - cameraPosition, aPosition - cameraPosition) >
                       glm::dot(bPosition - cameraPosition, bPosition - cameraPosition);
            });
        }
        for (const InstanceData* instancePointer : drawItems)
        {
            const auto& instance = *instancePointer;
            const bool isTransparent = (instance.flags & 1u) != 0u;
            const bool isDoubleSided = (instance.flags & (1u << 1u)) != 0u;
            const bool castsShadow = (instance.flags & (1u << 2u)) != 0u;
            if (shadowPass && (!castsShadow || isTransparent)) continue;
            if (!shadowPass && transparentPass != isTransparent) continue;
            if (fallbackOnly)
            {
                const bool materialMismatch = !gpuDrivenBindless &&
                    instance.materialId != gpuMaterialId;
                if (!isTransparent && !isDoubleSided && !materialMismatch) continue;
            }
            const MeshResource* mesh = sceneResources.mesh(instance.meshId);
            if (mesh == nullptr || mesh->indexCount == 0) continue;
            VkBuffer vertex = mesh->vertexBuffer.buffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &vertex, &offset);
            vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            VkPipelineLayout drawLayout = layout;
            if (!shadowPass)
            {
                const bool doubleSided = isDoubleSided;
                if (doubleSided && !transparentPass && gbufferDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
                {
                    drawLayout = gbufferDoubleSidedPipeline.layout();
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        gbufferDoubleSidedPipeline.pipeline());
                }
                else if (doubleSided && transparentPass && transparentDoubleSidedPipeline.pipeline() != VK_NULL_HANDLE)
                {
                    drawLayout = transparentDoubleSidedPipeline.layout();
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentDoubleSidedPipeline.pipeline());
                }
                else
                {
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentPass ? transparentPipeline.pipeline() : gbufferPipeline.pipeline());
                }
                const VkDescriptorSet material = sceneResources.materialDescriptor(instance.materialId);
                if (material != VK_NULL_HANDLE)
                {
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        drawLayout, 0, 1, &material, 0, nullptr);
                    ++materialDescriptorBindCount;
                }
            }
            const glm::mat4 model = glm::make_mat4(instance.transform.data());
            if (shadowPass)
            {
                struct CsmConstants { glm::mat4 lightViewProjection; glm::mat4 model; } constants{
                    cascadeMatrices[std::min<std::uint32_t>(cascade, 3u)], model};
                vkCmdPushConstants(frame.commandBuffer, drawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                    sizeof(constants), &constants);
            }
            else
            {
                const std::size_t instanceIndex = packet.instances.data() != nullptr
                    ? static_cast<std::size_t>(instancePointer - packet.instances.data())
                    : 0u;
                const bool hasPrevious = previousPacketValid && instanceIndex < previousInstances.size() &&
                    previousInstances[instanceIndex].meshId == instance.meshId &&
                    previousInstances[instanceIndex].materialId == instance.materialId;
                const glm::mat4 previousModel = hasPrevious
                    ? glm::make_mat4(previousInstances[instanceIndex].transform.data())
                    : model;
                const glm::mat4 previousVp = previousPacketValid ? previousViewProjection
                                                                  : packet.camera.viewProjection;
                if (transparentPass)
                {
                    M3TransparentPushConstants constants{};
                    constants.viewProjection = packet.camera.viewProjection;
                    constants.cameraPosition = glm::vec4{
                        glm::vec3(packet.camera.positionAndNear), 1.0f};
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
                            light.colorAndIntensity[0], light.colorAndIntensity[1],
                            light.colorAndIntensity[2], light.colorAndIntensity[3]};
                        constants.lightParameters = glm::vec4{light.positionAndRadius[3],
                            light.spotParams[0], light.spotParams[1], 0.04f};
                    }
                    vkCmdPushConstants(frame.commandBuffer, drawLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(constants), &constants);
                }
                else
                {
                    M3PushConstants constants{};
                    constants.viewProjection = packet.camera.viewProjection;
                    constants.previousViewProjection = previousVp;
                    constants.model = model;
                    constants.previousModel = previousModel;
                    vkCmdPushConstants(frame.commandBuffer, drawLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(constants), &constants);
                }
            }
            vkCmdDrawIndexed(frame.commandBuffer, mesh->indexCount, 1, 0, 0, 0);
        }
    };

    // Acquire returns an image in PRESENT_SRC_KHR after the previous frame.
    // Explicitly transition it before the tonemap scope; relying on the
    // dynamic-rendering begin operation is invalid Vulkan and crashes on
    // stricter drivers.
    VkImageMemoryBarrier2 swapchainBeginBarrier{};
    swapchainBeginBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    swapchainBeginBarrier.srcStageMask = swapchainImageInitialized[imageIndex]
                                             ? VK_PIPELINE_STAGE_2_NONE
                                             : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    swapchainBeginBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    swapchainBeginBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapchainBeginBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapchainBeginBarrier.oldLayout = swapchainImageInitialized[imageIndex]
                                          ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
    swapchainBeginBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapchainBeginBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapchainBeginBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapchainBeginBarrier.image = swapchainImages[imageIndex];
    swapchainBeginBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    swapchainBeginBarrier.subresourceRange.levelCount = 1;
    swapchainBeginBarrier.subresourceRange.layerCount = 1;
    VkDependencyInfo swapchainBeginDependency{};
    swapchainBeginDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    swapchainBeginDependency.imageMemoryBarrierCount = 1;
    swapchainBeginDependency.pImageMemoryBarriers = &swapchainBeginBarrier;
    vkCmdPipelineBarrier2(frame.commandBuffer, &swapchainBeginDependency);

    auto addDepthPass = [&](const char* name, std::uint32_t cascade)
    {
        graph.addPass<Graph::FrameGraph::Empty>(name,
            [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
            {
                shadow = builder.write(shadow, Graph::ResourceUsage::DepthAttachment);
                Graph::FrameGraphRenderPass::Descriptor descriptor{};
                descriptor.attachments.depth = shadow;
                descriptor.viewport.width = csmResolution;
                descriptor.viewport.height = csmResolution;
                descriptor.layerCount = 1;
                descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Depth;
                builder.declareRenderPass(name, descriptor);
                builder.sideEffect();
            },
            [&, cascade](const Graph::FrameGraphResources& resources,
                          const Graph::FrameGraph::Empty&, Graph::CommandContext&)
            {
                const auto info = resources.getRenderPassInfo(0);
                const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
                const VkImageView view = target != nullptr
                                              ? frameGraphProvider.layerView(
                                                    target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS], cascade)
                                              : VK_NULL_HANDLE;
                 if (view == VK_NULL_HANDLE) return;
                 const VkImage shadowImage = frameGraphProvider.image(
                     target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]);
                 transitionImage(shadowImage, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, cascade, 1);
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = view;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depthAttachment.clearValue.depthStencil.depth = 0.0f;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = {csmResolution, csmResolution};
                rendering.layerCount = 1;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    csmDepthPipeline.pipeline());
                VkViewport viewport{0.0f, 0.0f, static_cast<float>(csmResolution),
                    static_cast<float>(csmResolution), 0.0f, 1.0f};
                VkRect2D scissor{{0, 0}, {csmResolution, csmResolution}};
                 vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
                 vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
                 drawScene(csmDepthPipeline.layout(), true, false, cascade);
                 vkCmdEndRendering(frame.commandBuffer);
                 if (cascade == 3)
                 {
                     transitionImage(shadowImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT, 0, 4);
                 }
            });
    };
    addDepthPass("CSM shadows 0", 0);
    addDepthPass("CSM shadows 1", 1);
    addDepthPass("CSM shadows 2", 2);
    addDepthPass("CSM shadows 3", 3);

    const auto hizPhase1Input = hiz;
    auto& gbufferPass = graph.addPass<Graph::FrameGraph::Empty>("G-buffer",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            gbuffer0 = builder.write(gbuffer0, Graph::ResourceUsage::ColorAttachment);
            gbuffer1 = builder.write(gbuffer1, Graph::ResourceUsage::ColorAttachment);
            gbuffer2 = builder.write(gbuffer2, Graph::ResourceUsage::ColorAttachment);
            motion = builder.write(motion, Graph::ResourceUsage::ColorAttachment);
            instanceId = builder.write(instanceId, Graph::ResourceUsage::ColorAttachment);
            depth = builder.write(depth, Graph::ResourceUsage::DepthAttachment);
            if (config.enableTwoPhaseOcclusion)
                builder.read(hiz, Graph::ResourceUsage::Sampled);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = gbuffer0;
            descriptor.attachments.color[1] = gbuffer1;
            descriptor.attachments.color[2] = gbuffer2;
            descriptor.attachments.color[3] = motion;
            descriptor.attachments.color[4] = instanceId;
            descriptor.attachments.depth = depth;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::AllColors |
                                    Graph::FrameGraphAttachmentFlags::Depth;
            builder.declareRenderPass("G-buffer", descriptor);
            builder.sideEffect();
        },
        [&, hizPhase1Input](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            const auto info = resources.getRenderPassInfo(0);
             const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
             if (target == nullptr) return;

             // Phase 1 reclassifies the frustum-visible list using the
             // previous frame's Hi-Z.  The first frame has no valid history,
             // so it falls back to the regular visible list.
             if (config.enableTwoPhaseOcclusion && gpuIndirectCompatible &&
                 occlusionPhase1Pipeline.computePipeline() != VK_NULL_HANDLE &&
                 indirectBuildPipeline.computePipeline() != VK_NULL_HANDLE)
             {
                 const VkBuffer candidateCount = gpuSceneBuffers.visibleCountBuffer();
                 const VkBuffer phase1Count = gpuSceneBuffers.phase1VisibleCountBuffer();
                 const VkBuffer occludedCount = gpuSceneBuffers.occludedCountBuffer();
                 vkCmdFillBuffer(frame.commandBuffer, phase1Count, 0, sizeof(std::uint32_t), 0);
                 vkCmdFillBuffer(frame.commandBuffer, occludedCount, 0, sizeof(std::uint32_t), 0);
                 VkBufferMemoryBarrier2 reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                 reset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 reset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 reset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 reset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                 std::array<VkBufferMemoryBarrier2, 2> resets{reset, reset};
                 resets[0].buffer = phase1Count; resets[0].size = sizeof(std::uint32_t);
                 resets[1].buffer = occludedCount; resets[1].size = sizeof(std::uint32_t);
                 VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                 dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(resets.size());
                 dep.pBufferMemoryBarriers = resets.data();
                 vkCmdPipelineBarrier2(frame.commandBuffer, &dep);

                 const auto& hizResource = resources.getTexture(hizPhase1Input);
                 const VkImageView hizView = frameGraphProvider.mipView(hizResource.native, 0);
                 const VkDescriptorSet phaseSet = allocateSet(occlusionPhase1Layout);
                 if (phaseSet != VK_NULL_HANDLE && hasRenderedFrame && hizView != VK_NULL_HANDLE)
                 {
                     writeStorageBuffer(phaseSet, 0, gpuSceneBuffers.boundsBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::BoundsRow));
                     writeStorageBuffer(phaseSet, 1, gpuSceneBuffers.visibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 2, candidateCount, sizeof(std::uint32_t));
                     writeSampled(phaseSet, 3, frameGraphProvider.view(hizResource.native));
                     writeStorageBuffer(phaseSet, 5, gpuSceneBuffers.phase1VisibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 6, phase1Count, sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 7, gpuSceneBuffers.occludedIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(phaseSet, 8, occludedCount, sizeof(std::uint32_t));
                     vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         occlusionPhase1Pipeline.computePipeline());
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         occlusionPhase1Pipeline.layout(), 0, 1, &phaseSet, 0, nullptr);
                     struct OcclusionConstants { glm::mat4 viewProjection; glm::uvec2 extent; std::uint32_t maxMip; float depthBias; } constants{};
                     constants.viewProjection = packet.camera.viewProjection;
                     constants.extent = {width, height};
                     constants.maxMip = hizResource.descriptor.mipLevels > 0 ? hizResource.descriptor.mipLevels - 1 : 0;
                     constants.depthBias = 0.001f;
                     vkCmdPushConstants(frame.commandBuffer, occlusionPhase1Pipeline.layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                     vkCmdDispatch(frame.commandBuffer,
                         (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                 }
                 else
                 {
                     // No history: phase1 list is the frustum list.
                     VkBufferMemoryBarrier2 copyBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                     copyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                     copyBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                     copyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                     copyBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                     std::array<VkBufferMemoryBarrier2, 2> copyBarriers{copyBarrier, copyBarrier};
                     copyBarriers[0].buffer = gpuSceneBuffers.visibleIndicesBuffer(); copyBarriers[0].size = VK_WHOLE_SIZE;
                     copyBarriers[1].buffer = candidateCount; copyBarriers[1].size = sizeof(std::uint32_t);
                     dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = copyBarriers.data();
                     vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                     VkBufferCopy copy{0, 0, static_cast<VkDeviceSize>(gpuSceneInstanceCount) * sizeof(std::uint32_t)};
                     vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.visibleIndicesBuffer(),
                         gpuSceneBuffers.phase1VisibleIndicesBuffer(), 1, &copy);
                     VkBufferCopy countCopy{0, 0, sizeof(std::uint32_t)};
                     vkCmdCopyBuffer(frame.commandBuffer, candidateCount, phase1Count, 1, &countCopy);
                 }
                 VkBufferMemoryBarrier2 phaseBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                 phaseBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                 phaseBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                 phaseBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                 phaseBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                 std::array<VkBufferMemoryBarrier2, 2> phaseBarriers{phaseBarrier, phaseBarrier};
                 phaseBarriers[0].buffer = phase1Count; phaseBarriers[0].size = sizeof(std::uint32_t);
                 phaseBarriers[1].buffer = gpuSceneBuffers.phase1VisibleIndicesBuffer(); phaseBarriers[1].size = VK_WHOLE_SIZE;
                 dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = phaseBarriers.data();
                 vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                 const VkDescriptorSet indirectSet = allocateSet(gpuSceneIndirectLayout);
                 if (indirectSet != VK_NULL_HANDLE)
                 {
                     writeGpuStageTimestamp(1, true);
                     writeStorageBuffer(indirectSet, 0, gpuSceneBuffers.phase1VisibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 1, gpuSceneBuffers.meshMaterialBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
                     writeStorageBuffer(indirectSet, 2, gpuSceneBuffers.indirectCommandsBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(VkDrawIndexedIndirectCommand));
                     writeStorageBuffer(indirectSet, 3, gpuSceneBuffers.indirectDrawCountBuffer(), sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 4, gpuMeshDrawBuffer,
                         static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                             sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
                     writeStorageBuffer(indirectSet, 5, gpuSceneBuffers.meshHeadsBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 6, gpuSceneBuffers.meshNextBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 7, gpuSceneBuffers.groupedVisibleIndicesBuffer(),
                         static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 8, gpuSceneBuffers.groupedVisibleCountBuffer(), sizeof(std::uint32_t));
                     writeStorageBuffer(indirectSet, 9, phase1Count, sizeof(std::uint32_t));
                     vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         indirectBuildPipeline.computePipeline());
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                         indirectBuildPipeline.layout(), 0, 1, &indirectSet, 0, nullptr);
                     struct IndirectConstants { std::uint32_t instanceCount; std::uint32_t meshCount;
                         std::uint32_t mode; std::uint32_t reserved; } indirect{};
                     indirect.instanceCount = gpuSceneInstanceCount;
                     indirect.meshCount = sceneResources.meshDrawCount();
                     indirect.mode = 0;
                     vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                         0, sizeof(indirect), &indirect);
                     vkCmdDispatch(frame.commandBuffer, (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                     VkBufferMemoryBarrier2 groupBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                     groupBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                     groupBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                     groupBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                     groupBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                     std::array<VkBufferMemoryBarrier2, 2> groupBarriers{groupBarrier, groupBarrier};
                     groupBarriers[0].buffer = gpuSceneBuffers.meshHeadsBuffer(); groupBarriers[0].size = VK_WHOLE_SIZE;
                     groupBarriers[1].buffer = gpuSceneBuffers.meshNextBuffer(); groupBarriers[1].size = VK_WHOLE_SIZE;
                     dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(groupBarriers.size());
                     dep.pBufferMemoryBarriers = groupBarriers.data();
                     vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                     vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.indirectDrawCountBuffer(), 0,
                         sizeof(std::uint32_t), 0);
                     vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.groupedVisibleCountBuffer(), 0,
                         sizeof(std::uint32_t), 0);
                     std::array<VkBufferMemoryBarrier2, 2> buildReset{groupBarrier, groupBarrier};
                     buildReset[0].buffer = gpuSceneBuffers.indirectDrawCountBuffer();
                     buildReset[0].size = sizeof(std::uint32_t);
                     buildReset[1].buffer = gpuSceneBuffers.groupedVisibleCountBuffer();
                     buildReset[1].size = sizeof(std::uint32_t);
                     dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buildReset.size());
                     dep.pBufferMemoryBarriers = buildReset.data();
                     vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                     indirect.mode = 1;
                     vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                         0, sizeof(indirect), &indirect);
                     vkCmdDispatch(frame.commandBuffer,
                         (std::max(1u, indirect.meshCount) + 63u) / 64u, 1, 1);
                     VkBufferMemoryBarrier2 drawBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                     drawBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                     drawBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                     drawBarrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                     drawBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                         VK_ACCESS_2_SHADER_READ_BIT;
                     std::array<VkBufferMemoryBarrier2, 4> drawBarriers{drawBarrier, drawBarrier,
                         drawBarrier, drawBarrier};
                     drawBarriers[0].buffer = gpuSceneBuffers.indirectCommandsBuffer(); drawBarriers[0].size = VK_WHOLE_SIZE;
                     drawBarriers[1].buffer = gpuSceneBuffers.indirectDrawCountBuffer(); drawBarriers[1].size = sizeof(std::uint32_t);
                     drawBarriers[2].buffer = gpuSceneBuffers.groupedVisibleIndicesBuffer(); drawBarriers[2].size = VK_WHOLE_SIZE;
                     drawBarriers[3].buffer = gpuSceneBuffers.groupedVisibleCountBuffer(); drawBarriers[3].size = sizeof(std::uint32_t);
                     dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(drawBarriers.size()); dep.pBufferMemoryBarriers = drawBarriers.data();
                     vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                     writeGpuStageTimestamp(1, false);
                 }
             }
              transitionImage(frameGraphProvider.image(target->resources[0]), VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[1]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[2]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[3]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             transitionImage(frameGraphProvider.image(target->resources[4]), VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
             // The scene depth target is transient and is materialized for
             // this graph execution. Its first use is always a discard/clear,
             // regardless of whether the renderer has submitted older frames.
             transitionImage(frameGraphProvider.image(target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
            std::array<VkRenderingAttachmentInfo, 5> colors{};
            for (std::size_t i = 0; i < colors.size(); ++i)
            {
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colors[i].imageView = target->views[i];
                  colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                 colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                 colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                 colors[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
             }
             colors[4].clearValue.color.uint32[0] = 0u;
            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = target->depthView;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil.depth = 0.0f;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                gbufferPipeline.pipeline());
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
             vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
             vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
             if (config.enableGpuDrivenScene && gpuIndirectCompatible &&
                 gpuDrivenGbufferPipeline.pipeline() != VK_NULL_HANDLE &&
                 gpuGraphicsSet != VK_NULL_HANDLE)
             {
                 vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     gpuDrivenGbufferPipeline.pipeline());
                 VkDescriptorSet sets[2] = {VK_NULL_HANDLE, gpuGraphicsSet};
                 if (gpuDrivenBindless)
                 {
                     sets[0] = bindlessTable.descriptorSet();
                 }
                 else
                 {
                     sets[0] = sceneResources.materialDescriptor(
                         packet.instances.front().materialId);
                 }
                 if (sets[0] != VK_NULL_HANDLE)
                 {
                     vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         gpuDrivenGbufferPipeline.layout(), 0, 2, sets, 0, nullptr);
                     const struct GpuDrivenConstants
                     {
                         glm::mat4 viewProjection;
                         glm::mat4 previousViewProjection;
                     } constants{packet.camera.viewProjection,
                         previousPacketValid ? previousViewProjection : packet.camera.viewProjection};
                     vkCmdPushConstants(frame.commandBuffer, gpuDrivenGbufferPipeline.layout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
                     const VkDeviceSize vertexOffset = 0;
                     vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &gpuVertexBuffer, &vertexOffset);
                     vkCmdBindIndexBuffer(frame.commandBuffer, gpuIndexBuffer, 0,
                         VK_INDEX_TYPE_UINT32);
                     vkCmdDrawIndexedIndirectCount(frame.commandBuffer,
                         gpuSceneBuffers.indirectCommandsBuffer(), 0,
                         config.enableTwoPhaseOcclusion
                             ? gpuSceneBuffers.phase1IndirectDrawCountBuffer()
                             : gpuSceneBuffers.indirectDrawCountBuffer(), 0,
                         std::max<std::uint32_t>(1u, sceneResources.meshDrawCount()),
                         sizeof(VkDrawIndexedIndirectCommand));
                     // Transparent, double-sided, and non-bindless material
                     // mismatches stay on the existing CPU path, but they no
                     // longer force compatible opaque instances to leave the
                     // GPU-driven submission path.
                     drawScene(gbufferPipeline.layout(), false, false, 0u, true);
                 }
                 else
                 {
                     drawScene(gbufferPipeline.layout(), false, false);
                 }
             }
             else
             {
                 drawScene(gbufferPipeline.layout(), false, false);
             }
             vkCmdEndRendering(frame.commandBuffer);
             // Depth is consumed immediately by the Hi-Z compute pass. End
             // the render pass in the sampled layout so the next pass has a
             // single, explicit starting state on every frame.
             transitionImage(frameGraphProvider.image(target->resources[Graph::FrameGraphRenderPass::MAX_COLOR_ATTACHMENTS]),
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
        });

    if (config.enableGpuDrivenScene && hizBuildPipeline.computePipeline() != VK_NULL_HANDLE)
    {
        // Phase 2 performs a LOAD/STORE overlay into the G-buffer. Keep every
        // attachment in the pass data so FrameGraphResources can validate the
        // native accesses and carry the resulting versions to deferred lighting.
        struct HiZPassData
        {
            Graph::TextureHandle depth{};
            Graph::TextureHandle hiz{};
            Graph::TextureHandle gbuffer0{};
            Graph::TextureHandle gbuffer1{};
            Graph::TextureHandle gbuffer2{};
            Graph::TextureHandle motion{};
            Graph::TextureHandle instanceId{};
        };
        graph.addPass<HiZPassData>("Hi-Z build",
            [&](Graph::FrameGraph::Builder& builder, HiZPassData& data)
            {
                builder.readWrite(depth, Graph::ResourceUsage::DepthAttachment |
                                         Graph::ResourceUsage::Sampled);
                data.depth = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                data.hiz = builder.write(hiz, Graph::ResourceUsage::Storage |
                                               Graph::ResourceUsage::Sampled);
                builder.readWrite(gbuffer0, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer0 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(gbuffer1, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer1 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(gbuffer2, Graph::ResourceUsage::ColorAttachment);
                data.gbuffer2 = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(motion, Graph::ResourceUsage::ColorAttachment);
                data.motion = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                builder.readWrite(instanceId, Graph::ResourceUsage::ColorAttachment |
                                           Graph::ResourceUsage::TransferSource);
                data.instanceId = Graph::TextureHandle(builder.resourceHandle().index(),
                    builder.resourceHandle().version(), builder.resourceHandle().epoch());
                depth = data.depth;
                hiz = data.hiz;
                gbuffer0 = data.gbuffer0;
                gbuffer1 = data.gbuffer1;
                gbuffer2 = data.gbuffer2;
                motion = data.motion;
                instanceId = data.instanceId;
                builder.dependsOn(gbufferPass.handle());
                builder.sideEffect();
            },
            [&](const Graph::FrameGraphResources& resources, const HiZPassData& data,
                Graph::CommandContext&)
            {
                const auto& depthResource = resources.getTexture(data.depth);
                const auto& hizResource = resources.getTexture(data.hiz);
                const VkImage depthImage = frameGraphProvider.image(depthResource.native);
                const VkImage hizImage = frameGraphProvider.image(hizResource.native);
                if (depthImage == VK_NULL_HANDLE || hizImage == VK_NULL_HANDLE) return;
                writeGpuStageTimestamp(2, true);
                transitionImage(hizImage, hasRenderedFrame ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                             : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL,
                    hasRenderedFrame ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                     : VK_PIPELINE_STAGE_2_NONE,
                    hasRenderedFrame ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
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
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                };
                const auto buildHiZPyramid = [&]()
                {
                    // Phase 2 records an indirect-build dispatch between the
                    // two pyramid builds. Rebind the Hi-Z pipeline every time
                    // this helper is invoked so its descriptor layout and the
                    // active compute pipeline can never diverge.
                    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        hizBuildPipeline.computePipeline());
                    std::uint32_t sourceWidth = depthResource.descriptor.width;
                    std::uint32_t sourceHeight = depthResource.descriptor.height;
                    for (std::uint32_t mip = 0; mip < hizResource.descriptor.mipLevels; ++mip)
                    {
                        const VkImageView sourceView = mip == 0
                            ? frameGraphProvider.view(depthResource.native)
                            : frameGraphProvider.mipView(hizResource.native, mip - 1);
                        const VkImageView outputView = frameGraphProvider.mipView(hizResource.native, mip);
                        if (sourceView == VK_NULL_HANDLE || outputView == VK_NULL_HANDLE) break;
                        // A descriptor set may not be updated after it has
                        // been referenced by a recorded command buffer. Each
                        // mip dispatch gets its own set instead of rewriting
                        // the previous mip's sampled/storage views.
                        const auto descriptor = allocateSet(hizLayout);
                        if (descriptor == VK_NULL_HANDLE) return;
                        writeImage(descriptor, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sourceView,
                            mip == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_GENERAL);
                        writeImage(descriptor, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, outputView,
                            VK_IMAGE_LAYOUT_GENERAL);
                        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            hizBuildPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
                        struct HiZConstants { std::uint32_t sourceWidth, sourceHeight,
                            outputWidth, outputHeight; } constants{sourceWidth, sourceHeight,
                            std::max(1u, (sourceWidth + 1u) / 2u),
                            std::max(1u, (sourceHeight + 1u) / 2u)};
                        vkCmdPushConstants(frame.commandBuffer, hizBuildPipeline.layout(),
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
                        vkCmdDispatch(frame.commandBuffer, (constants.outputWidth + 7u) / 8u,
                            (constants.outputHeight + 7u) / 8u, 1);
                        VkImageMemoryBarrier2 mipBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        mipBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        mipBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                        mipBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        mipBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                            VK_ACCESS_2_SHADER_WRITE_BIT;
                        mipBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        mipBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        mipBarrier.image = hizImage;
                        mipBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
                        VkDependencyInfo mipDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        mipDependency.imageMemoryBarrierCount = 1;
                        mipDependency.pImageMemoryBarriers = &mipBarrier;
                        vkCmdPipelineBarrier2(frame.commandBuffer, &mipDependency);
                        sourceWidth = constants.outputWidth;
                        sourceHeight = constants.outputHeight;
                    }
                };
                buildHiZPyramid();
                transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                writeGpuStageTimestamp(2, false);

                if (!config.enableTwoPhaseOcclusion || !gpuIndirectCompatible ||
                    occlusionPhase2Pipeline.computePipeline() == VK_NULL_HANDLE ||
                    !hasRenderedFrame)
                    return;
                writeGpuStageTimestamp(3, true);
                const VkDescriptorSet phaseSet = allocateSet(occlusionPhase2Layout);
                if (phaseSet == VK_NULL_HANDLE) return;
                const VkBuffer phase2Count = gpuSceneBuffers.phase2VisibleCountBuffer();
                vkCmdFillBuffer(frame.commandBuffer, phase2Count, 0, sizeof(std::uint32_t), 0);
                VkBufferMemoryBarrier2 reset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                reset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                reset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                reset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                reset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                reset.buffer = phase2Count; reset.size = sizeof(std::uint32_t);
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = 1; dep.pBufferMemoryBarriers = &reset;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                writeStorageBuffer(phaseSet, 0, gpuSceneBuffers.boundsBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::BoundsRow));
                writeStorageBuffer(phaseSet, 1, gpuSceneBuffers.occludedIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(phaseSet, 2, gpuSceneBuffers.occludedCountBuffer(), sizeof(std::uint32_t));
                writeSampled(phaseSet, 3, frameGraphProvider.view(hizResource.native));
                writeStorageBuffer(phaseSet, 5, gpuSceneBuffers.phase2VisibleIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(phaseSet, 6, phase2Count, sizeof(std::uint32_t));
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    occlusionPhase2Pipeline.computePipeline());
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    occlusionPhase2Pipeline.layout(), 0, 1, &phaseSet, 0, nullptr);
                struct OcclusionConstants { glm::mat4 viewProjection; glm::uvec2 extent; std::uint32_t maxMip; float depthBias; } constants{};
                constants.viewProjection = packet.camera.viewProjection;
                constants.extent = {width, height};
                constants.maxMip = hizResource.descriptor.mipLevels > 0 ? hizResource.descriptor.mipLevels - 1 : 0;
                constants.depthBias = 0.001f;
                vkCmdPushConstants(frame.commandBuffer, occlusionPhase2Pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(constants), &constants);
                vkCmdDispatch(frame.commandBuffer, (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                VkBufferMemoryBarrier2 phaseBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                phaseBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                phaseBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                phaseBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                phaseBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                std::array<VkBufferMemoryBarrier2, 3> phaseBarriers{phaseBarrier, phaseBarrier, phaseBarrier};
                phaseBarriers[0].buffer = phase2Count; phaseBarriers[0].size = sizeof(std::uint32_t);
                phaseBarriers[1].buffer = gpuSceneBuffers.occludedCountBuffer(); phaseBarriers[1].size = sizeof(std::uint32_t);
                phaseBarriers[2].buffer = gpuSceneBuffers.occludedIndicesBuffer(); phaseBarriers[2].size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(phaseBarriers.size()); dep.pBufferMemoryBarriers = phaseBarriers.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);

                const VkDescriptorSet indirectSet = allocateSet(gpuSceneIndirectLayout);
                if (indirectSet == VK_NULL_HANDLE) return;
                writeStorageBuffer(indirectSet, 0, gpuSceneBuffers.phase2VisibleIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 1, gpuSceneBuffers.meshMaterialBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(Halcyon::Renderer::Scene::MeshMaterialRow));
                writeStorageBuffer(indirectSet, 2, gpuSceneBuffers.phase2IndirectCommandsBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(VkDrawIndexedIndirectCommand));
                writeStorageBuffer(indirectSet, 3, gpuSceneBuffers.phase2IndirectDrawCountBuffer(), sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 4, gpuMeshDrawBuffer,
                    static_cast<VkDeviceSize>(sceneResources.meshDrawCount()) *
                        sizeof(Halcyon::Renderer::Scene::MeshDrawRow));
                writeStorageBuffer(indirectSet, 5, gpuSceneBuffers.meshHeadsBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 6, gpuSceneBuffers.meshNextBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 7, gpuSceneBuffers.phase2GroupedVisibleIndicesBuffer(),
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 8, gpuSceneBuffers.phase2GroupedVisibleCountBuffer(), sizeof(std::uint32_t));
                writeStorageBuffer(indirectSet, 9, phase2Count, sizeof(std::uint32_t));
                VkBufferMemoryBarrier2 visibleBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                visibleBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                visibleBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                visibleBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                visibleBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                visibleBarrier.buffer = gpuSceneBuffers.phase2VisibleIndicesBuffer(); visibleBarrier.size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = 1; dep.pBufferMemoryBarriers = &visibleBarrier;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                // meshHeads is reused by the second grouping pass.  Clear it
                // after the first pass has finished reading it, then make the
                // transfer visible to the mode-0 compute dispatch below.
                VkBufferMemoryBarrier2 meshHeadBeforeReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                meshHeadBeforeReset.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                meshHeadBeforeReset.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_SHADER_WRITE_BIT;
                meshHeadBeforeReset.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                meshHeadBeforeReset.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                meshHeadBeforeReset.buffer = gpuSceneBuffers.meshHeadsBuffer();
                meshHeadBeforeReset.size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &meshHeadBeforeReset;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.meshHeadsBuffer(), 0,
                    static_cast<VkDeviceSize>(gpuSceneBuffers.capacity()) * sizeof(std::uint32_t),
                    0xffffffffu);
                VkBufferMemoryBarrier2 meshHeadAfterReset{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                meshHeadAfterReset.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                meshHeadAfterReset.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                meshHeadAfterReset.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                meshHeadAfterReset.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                    VK_ACCESS_2_SHADER_WRITE_BIT;
                meshHeadAfterReset.buffer = gpuSceneBuffers.meshHeadsBuffer();
                meshHeadAfterReset.size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &meshHeadAfterReset;
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    indirectBuildPipeline.computePipeline());
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    indirectBuildPipeline.layout(), 0, 1, &indirectSet, 0, nullptr);
                struct IndirectConstants { std::uint32_t instanceCount; std::uint32_t meshCount;
                    std::uint32_t mode; std::uint32_t reserved; } indirect{};
                indirect.instanceCount = gpuSceneInstanceCount;
                indirect.meshCount = sceneResources.meshDrawCount();
                indirect.mode = 0;
                vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(indirect), &indirect);
                vkCmdDispatch(frame.commandBuffer, (std::max(1u, gpuSceneInstanceCount) + 63u) / 64u, 1, 1);
                VkBufferMemoryBarrier2 groupBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                groupBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                groupBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                groupBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                groupBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                std::array<VkBufferMemoryBarrier2, 2> groupBarriers{groupBarrier, groupBarrier};
                groupBarriers[0].buffer = gpuSceneBuffers.meshHeadsBuffer(); groupBarriers[0].size = VK_WHOLE_SIZE;
                groupBarriers[1].buffer = gpuSceneBuffers.meshNextBuffer(); groupBarriers[1].size = VK_WHOLE_SIZE;
                dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(groupBarriers.size()); dep.pBufferMemoryBarriers = groupBarriers.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.phase2IndirectDrawCountBuffer(), 0,
                    sizeof(std::uint32_t), 0);
                vkCmdFillBuffer(frame.commandBuffer, gpuSceneBuffers.phase2GroupedVisibleCountBuffer(), 0,
                    sizeof(std::uint32_t), 0);
                std::array<VkBufferMemoryBarrier2, 2> buildReset{groupBarrier, groupBarrier};
                buildReset[0].buffer = gpuSceneBuffers.phase2IndirectDrawCountBuffer();
                buildReset[0].size = sizeof(std::uint32_t);
                buildReset[1].buffer = gpuSceneBuffers.phase2GroupedVisibleCountBuffer();
                buildReset[1].size = sizeof(std::uint32_t);
                dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buildReset.size()); dep.pBufferMemoryBarriers = buildReset.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);
                indirect.mode = 1;
                vkCmdPushConstants(frame.commandBuffer, indirectBuildPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(indirect), &indirect);
                vkCmdDispatch(frame.commandBuffer,
                    (std::max(1u, indirect.meshCount) + 63u) / 64u, 1, 1);
                VkBufferMemoryBarrier2 commandBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                commandBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                commandBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                commandBarrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                commandBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_2_SHADER_READ_BIT;
                std::array<VkBufferMemoryBarrier2, 4> commandBarriers{commandBarrier, commandBarrier,
                    commandBarrier, commandBarrier};
                commandBarriers[0].buffer = gpuSceneBuffers.phase2IndirectCommandsBuffer(); commandBarriers[0].size = VK_WHOLE_SIZE;
                commandBarriers[1].buffer = gpuSceneBuffers.phase2IndirectDrawCountBuffer(); commandBarriers[1].size = sizeof(std::uint32_t);
                commandBarriers[2].buffer = gpuSceneBuffers.phase2GroupedVisibleIndicesBuffer(); commandBarriers[2].size = VK_WHOLE_SIZE;
                commandBarriers[3].buffer = gpuSceneBuffers.phase2GroupedVisibleCountBuffer(); commandBarriers[3].size = sizeof(std::uint32_t);
                dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(commandBarriers.size()); dep.pBufferMemoryBarriers = commandBarriers.data();
                vkCmdPipelineBarrier2(frame.commandBuffer, &dep);

                // Overlay the objects that became visible after the current
                // frame's Hi-Z was built, preserving all existing G-buffer data.
                const VkDescriptorSet graphicsSet = gpuPhase2GraphicsSet;
                if (graphicsSet == VK_NULL_HANDLE) return;
                transitionImage(frameGraphProvider.image(resources.getTexture(data.gbuffer0).native), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.depth).native), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.instanceId).native),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                VkRenderingAttachmentInfo colors[5]{};
                const Graph::FrameGraphNativeResource colorTokens[5] = {
                    resources.getTexture(data.gbuffer0).native, resources.getTexture(data.gbuffer1).native,
                    resources.getTexture(data.gbuffer2).native, resources.getTexture(data.motion).native,
                    resources.getTexture(data.instanceId).native};
                for (int i = 0; i < 5; ++i) { colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colors[i].imageView = frameGraphProvider.view(colorTokens[i]); colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
                VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                depthAttachment.imageView = frameGraphProvider.view(resources.getTexture(data.depth).native);
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                 VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = swapchainExtent;
                 rendering.layerCount = 1; rendering.colorAttachmentCount = 5; rendering.pColorAttachments = colors;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuDrivenGbufferPipeline.pipeline());
                VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
                VkRect2D scissor{{0, 0}, swapchainExtent}; vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport); vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
                VkDescriptorSet sets[2] = {gpuDrivenBindless ? bindlessTable.descriptorSet() : sceneResources.materialDescriptor(gpuMaterialId), graphicsSet};
                if (sets[0] != VK_NULL_HANDLE)
                {
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gpuDrivenGbufferPipeline.layout(), 0, 2, sets, 0, nullptr);
                    const struct GpuDrivenConstants { glm::mat4 viewProjection; glm::mat4 previousViewProjection; } drawConstants{
                        packet.camera.viewProjection, previousPacketValid ? previousViewProjection : packet.camera.viewProjection};
                    vkCmdPushConstants(frame.commandBuffer, gpuDrivenGbufferPipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(drawConstants), &drawConstants);
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &gpuVertexBuffer, &offset);
                    vkCmdBindIndexBuffer(frame.commandBuffer, gpuIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexedIndirectCount(frame.commandBuffer, gpuSceneBuffers.phase2IndirectCommandsBuffer(), 0,
                        gpuSceneBuffers.phase2IndirectDrawCountBuffer(), 0,
                        std::max(1u, sceneResources.meshDrawCount()), sizeof(VkDrawIndexedIndirectCommand));
                }
                vkCmdEndRendering(frame.commandBuffer);
                transitionImage(frameGraphProvider.image(resources.getTexture(data.depth).native), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

                // Phase 2 changed depth. Rebuild the complete pyramid so the
                // next frame never consumes the phase-1-only intermediate.
                transitionImage(hizImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
                buildHiZPyramid();
                transitionImage(hizImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                writeGpuStageTimestamp(3, false);
            });
    }

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

    const auto deferredGbuffer0 = gbuffer0;
    const auto deferredGbuffer1 = gbuffer1;
    const auto deferredGbuffer2 = gbuffer2;
    const auto deferredDepth = depth;
    const auto deferredShadow = shadow;
    const auto deferredIrradiance = irradiance;
    const auto deferredPrefiltered = prefiltered;
    const auto deferredBrdf = brdfLut;
    const auto deferredClusterRanges = clusterRanges;
    const auto deferredClusterIndices = clusterIndices;
    const auto deferredLightBuffer = lightBuffer;
    const auto deferredShadowConstants = shadowConstantsBuffer;
    graph.addPass<Graph::FrameGraph::Empty>("Clustered deferred lighting",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(gbuffer0, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer1, Graph::ResourceUsage::Sampled);
            builder.read(gbuffer2, Graph::ResourceUsage::Sampled);
            builder.read(depth, Graph::ResourceUsage::Sampled);
            builder.read(shadow, Graph::ResourceUsage::Sampled);
            builder.read(irradiance, Graph::ResourceUsage::Sampled);
            builder.read(prefiltered, Graph::ResourceUsage::Sampled);
            builder.read(brdfLut, Graph::ResourceUsage::Sampled);
            builder.read(clusterRanges, Graph::ResourceUsage::Storage);
            builder.read(clusterIndices, Graph::ResourceUsage::Storage);
            builder.read(lightBuffer, Graph::ResourceUsage::Storage);
            builder.read(shadowConstantsBuffer, Graph::ResourceUsage::Uniform);
            // HDR is both a render target and a legal storage image.  The
            // latter is part of the M3 resource contract even though the
            // current deferred implementation writes it through dynamic
            // rendering; keeping the capability in the graph prevents a
            // later compute pass from silently requiring a reallocation.
            hdr = builder.write(hdr, Graph::ResourceUsage::ColorAttachment |
                                     Graph::ResourceUsage::Storage);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = hdr;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
            builder.declareRenderPass("Clustered deferred lighting", descriptor);
            builder.sideEffect();
        },
        [&, hdr, deferredGbuffer0, deferredGbuffer1, deferredGbuffer2, deferredDepth, deferredShadow,
            deferredIrradiance, deferredPrefiltered, deferredBrdf, deferredClusterRanges,
            deferredClusterIndices, deferredLightBuffer, deferredShadowConstants](
            const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
            Graph::CommandContext&)
        {
            const auto info = resources.getRenderPassInfo(0);
            const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
            if (target == nullptr) return;
             transitionImage(frameGraphProvider.image(target->resources[0]),
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer0).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer1).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(deferredGbuffer2).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
             transitionImage(frameGraphProvider.image(resources.getTexture(deferredDepth).native),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!iblInitialized)
            {
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                static const ProceduralIblData proceduralIbl = createProceduralIbl();
                const VoidResult irradianceUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    proceduralIbl.irradiance, proceduralIbl.irradianceCopies);
                const VoidResult prefilteredUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    proceduralIbl.prefiltered, proceduralIbl.prefilteredCopies);
                const VoidResult brdfUpload = recordImageUpload(frame.commandBuffer,
                    frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    proceduralIbl.brdf, proceduralIbl.brdfCopies);
                if (!irradianceUpload || !prefilteredUpload || !brdfUpload)
                {
                    const auto& error = !irradianceUpload ? irradianceUpload.error()
                        : !prefilteredUpload ? prefilteredUpload.error()
                                             : brdfUpload.error();
                    setError(error.describe());
                    fatalError = true;
                    return;
                }
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredIrradiance).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredPrefiltered).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 6);
                transitionImage(frameGraphProvider.image(resources.getTexture(deferredBrdf).native),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                iblInitialized = true;
            }
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = target->views[0];
             color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                deferredLightingPipeline.pipeline());
            const VkDescriptorSet descriptor = allocateSet(m3LightingLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(deferredGbuffer0).native));
                writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(deferredGbuffer1).native));
                writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(deferredGbuffer2).native));
                writeSampled(descriptor, 3, frameGraphProvider.view(resources.getTexture(deferredDepth).native));
                writeSampled(descriptor, 4, frameGraphProvider.view(resources.getTexture(deferredShadow).native));
                writeSampled(descriptor, 5, frameGraphProvider.view(resources.getTexture(deferredIrradiance).native));
                 writeSampled(descriptor, 6, frameGraphProvider.view(resources.getTexture(deferredPrefiltered).native));
                 writeSampled(descriptor, 7, frameGraphProvider.view(resources.getTexture(deferredBrdf).native));
                const auto& ranges = resources.get<Graph::FrameGraphBuffer>(deferredClusterRanges);
                const auto& indices = resources.get<Graph::FrameGraphBuffer>(deferredClusterIndices);
                const auto& lights = resources.get<Graph::FrameGraphBuffer>(deferredLightBuffer);
                const auto& shadowData = resources.get<Graph::FrameGraphBuffer>(deferredShadowConstants);
                if (!packet.lights.empty())
                {
                    if (const auto* allocation =
                            frameGraphProvider.nativeBufferAllocation(lights.native);
                        allocation != nullptr)
                    {
                        const auto bytes = std::span<const std::byte>{
                            reinterpret_cast<const std::byte*>(packet.lights.data()),
                            packet.lights.size_bytes()};
                        (void)gpuAllocator.writeBuffer(*allocation, bytes);
                    }
                }
                writeStorageBuffer(descriptor, 20, frameGraphProvider.buffer(ranges.native), ranges.descriptor.size);
                writeStorageBuffer(descriptor, 21, frameGraphProvider.buffer(indices.native), indices.descriptor.size);
                writeStorageBuffer(descriptor, 22, frameGraphProvider.buffer(lights.native), lights.descriptor.size);
                writeUniformBuffer(descriptor, 23, frameGraphProvider.buffer(shadowData.native),
                    shadowData.descriptor.size);
                struct alignas(16) ShadowData
                {
                    std::array<glm::mat4, 4> lightViewProjection;
                    glm::vec4 splits;
                    glm::vec4 params;
                } shadowUpload{cascadeMatrices, cascadeSplits,
                    glm::vec4{1.0f / static_cast<float>(csmResolution), 0.00035f, 0.0015f, 0.0f}};
                if (const auto* allocation =
                        frameGraphProvider.nativeBufferAllocation(shadowData.native);
                    allocation != nullptr)
                {
                    const auto bytes = std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(&shadowUpload), sizeof(shadowUpload)};
                    (void)gpuAllocator.writeBuffer(*allocation, bytes);
                }
                writeSampler(descriptor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    deferredLightingPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
            struct DeferredConstants
            {
                glm::mat4 inverseViewProjection;
                glm::vec4 cameraPosition;
                glm::vec4 viewportAndInvViewport;
                glm::uvec4 clusterParams;
                glm::vec4 depthParams;
            } constants{};
            constants.inverseViewProjection = packet.camera.inverseViewProjection;
            constants.cameraPosition = packet.camera.positionAndNear;
            constants.viewportAndInvViewport = packet.camera.viewportAndInvViewport;
            constants.clusterParams = glm::uvec4{
                m3FrameResources.tilesX(),
                m3FrameResources.tilesY(),
                VulkanM3FrameResources::ClusterSlices,
                static_cast<std::uint32_t>(packet.lights.size())};
            constants.depthParams = glm::vec4{
                std::max(1.0e-4f, packet.camera.positionAndNear.w),
                packet.camera.forwardAndFar.w > packet.camera.positionAndNear.w
                    ? packet.camera.forwardAndFar.w : 1000.0f,
                1.0f, 0.0f};
            vkCmdPushConstants(frame.commandBuffer, deferredLightingPipeline.layout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(frame.commandBuffer);
        });

    if (config.enableTransparency)
    {
        graph.addPass<Graph::FrameGraph::Empty>("Forward transparency",
            [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
            {
                builder.read(depth, Graph::ResourceUsage::DepthAttachment);
                hdr = builder.write(hdr, Graph::ResourceUsage::ColorAttachment);
                Graph::FrameGraphRenderPass::Descriptor descriptor{};
                descriptor.attachments.color[0] = hdr;
                descriptor.attachments.depth = depth;
                descriptor.viewport.width = width;
                descriptor.viewport.height = height;
                descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::None;
                builder.declareRenderPass("Forward transparency", descriptor);
                builder.sideEffect();
            },
            [&, hdr](const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
                Graph::CommandContext&)
            {
                const auto info = resources.getRenderPassInfo(0);
                const auto* target = static_cast<const VulkanFrameGraphRenderTarget*>(info.target.token);
                if (target == nullptr) return;
                transitionImage(frameGraphProvider.image(resources.getTexture(depth).native),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo color{};
                color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color.imageView = target->views[0];
                 color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo depthAttachment{};
                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = target->depthView;
                 depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea.extent = swapchainExtent;
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &color;
                rendering.pDepthAttachment = &depthAttachment;
                vkCmdBeginRendering(frame.commandBuffer, &rendering);
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    transparentPipeline.pipeline());
                drawScene(transparentPipeline.layout(), false, true);
                vkCmdEndRendering(frame.commandBuffer);
            });
    }

    auto& historyRead = taaHistoryFlip ? historyB : historyA;
    auto& historyWrite = taaHistoryFlip ? historyA : historyB;
    const bool frameHistoryFlip = taaHistoryFlip;
    const auto taaHdr = hdr;
    const auto taaMotion = motion;
    const auto taaHistoryRead = historyRead;
    graph.addPass<Graph::FrameGraph::Empty>("TAA resolve",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(hdr, Graph::ResourceUsage::Sampled);
            builder.read(motion, Graph::ResourceUsage::Sampled);
            builder.read(historyRead, Graph::ResourceUsage::Sampled);
            historyWrite = builder.write(historyWrite, Graph::ResourceUsage::Storage);
            builder.sideEffect();
        },
        [&, taaHdr, taaMotion, taaHistoryRead](const Graph::FrameGraphResources& resources,
            const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            transitionImage(frameGraphProvider.image(resources.getTexture(taaHdr).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(frameGraphProvider.image(resources.getTexture(taaMotion).native),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            const VkImage historyReadImage = frameGraphProvider.image(resources.getTexture(taaHistoryRead).native);
            const VkImage historyWriteImage = frameGraphProvider.image(resources.getTexture(historyWrite).native);
             const bool readInitialized = frameHistoryFlip ? taaHistoryInitializedB : taaHistoryInitializedA;
             const bool writeInitialized = frameHistoryFlip ? taaHistoryInitializedA : taaHistoryInitializedB;
            transitionImage(historyReadImage,
                readInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            transitionImage(historyWriteImage,
                writeInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    taaPipeline.computePipeline());
                const VkDescriptorSet descriptor = allocateSet(m3TaaLayout);
                if (descriptor != VK_NULL_HANDLE)
                {
                    writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(taaHdr).native));
                    writeSampled(descriptor, 1, frameGraphProvider.view(resources.getTexture(taaHistoryRead).native));
                    writeSampled(descriptor, 2, frameGraphProvider.view(resources.getTexture(taaMotion).native));
                    writeSampler(descriptor);
                    writeStorage(descriptor, 20, frameGraphProvider.view(resources.getTexture(historyWrite).native));
                    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        taaPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
                }
                struct TaaConstants { std::uint32_t extent[2]; float historyWeight; float sharpen; } constants{{width, height},
                    (config.enableTaa && taaHistoryValid) ? 0.9f : 0.0f,
                    config.enableTaa ? 0.05f : 0.0f};
                vkCmdPushConstants(frame.commandBuffer, taaPipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(constants), &constants);
                vkCmdDispatch(frame.commandBuffer, (width + 7u) / 8u, (height + 7u) / 8u, 1);
                transitionImage(historyWriteImage, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (frameHistoryFlip) taaHistoryInitializedA = true;
                else taaHistoryInitializedB = true;
            }
        });

    const auto tonemapInput = historyWrite;
    graph.addPass<Graph::FrameGraph::Empty>("ACES tonemap",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(tonemapInput, Graph::ResourceUsage::Sampled);
            output = builder.write(output, Graph::ResourceUsage::ColorAttachment);
            Graph::FrameGraphRenderPass::Descriptor descriptor{};
            descriptor.attachments.color[0] = output;
            descriptor.viewport.width = width;
            descriptor.viewport.height = height;
            descriptor.clearFlags = Graph::FrameGraphAttachmentFlags::Color0;
            builder.declareRenderPass("ACES tonemap", descriptor);
            builder.sideEffect();
        },
        [&, tonemapInput](const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&, Graph::CommandContext&)
        {
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = importedTarget.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.018f, 0.028f, 0.055f, 1.0f}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = swapchainExtent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            vkCmdBeginRendering(frame.commandBuffer, &rendering);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                tonemapPipeline.pipeline());
            const VkDescriptorSet descriptor = allocateSet(m3TonemapLayout);
            if (descriptor != VK_NULL_HANDLE)
            {
                writeSampled(descriptor, 0, frameGraphProvider.view(resources.getTexture(tonemapInput).native));
                writeSampler(descriptor);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    tonemapPipeline.layout(), 0, 1, &descriptor, 0, nullptr);
            }
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, swapchainExtent};
            vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
            struct TonemapConstants
            {
                float exposure;
                float outputIsSrgb;
                float padding[2];
            } tonemapConstants{config.exposure,
                (swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                    swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB) ? 1.0f : 0.0f,
                {0.0f, 0.0f}};
            vkCmdPushConstants(frame.commandBuffer, tonemapPipeline.layout(),
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tonemapConstants), &tonemapConstants);
            vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRendering(frame.commandBuffer);
        });
    const auto presentOutput = output;
    const auto presentInstanceId = instanceId;
    graph.addPass<Graph::FrameGraph::Empty>("Present",
        [&](Graph::FrameGraph::Builder& builder, Graph::FrameGraph::Empty&)
        {
            builder.read(presentOutput, screenshotReadback != VK_NULL_HANDLE
                    ? Graph::ResourceUsage::TransferSource
                    : Graph::ResourceUsage::Present);
            if (config.enableGpuDrivenScene)
                builder.read(presentInstanceId, Graph::ResourceUsage::TransferSource);
            builder.sideEffect();
        },
        [&, presentInstanceId](const Graph::FrameGraphResources& resources, const Graph::FrameGraph::Empty&,
            Graph::CommandContext&)
        {
            if (config.enableGpuDrivenScene && currentFrame < instanceIdReadbacks.size() &&
                instanceIdReadbacks[currentFrame].buffer != VK_NULL_HANDLE)
            {
                const VkImage instanceImage = frameGraphProvider.image(
                    resources.getTexture(presentInstanceId).native);
                if (instanceImage != VK_NULL_HANDLE)
                {
                    transitionImage(instanceImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                    VkBufferImageCopy idCopy{};
                    idCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    idCopy.imageExtent = {width, height, 1};
                    vkCmdCopyImageToBuffer(frame.commandBuffer, instanceImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        instanceIdReadbacks[currentFrame].buffer, 1, &idCopy);
                    instanceIdReadbackFrameIndices[currentFrame] = packet.frameIndex;
                    instanceIdReadbackValid[currentFrame] = true;
                }
            }
            if (screenshotReadback != VK_NULL_HANDLE)
            {
                transitionImage(swapchainImages[imageIndex],
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(frame.commandBuffer, swapchainImages[imageIndex],
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotReadback, 1, &copy);
                transitionImage(swapchainImages[imageIndex],
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
            }
            else
            {
                transitionImage(swapchainImages[imageIndex],
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
            }
        });

    graph.compile(Graph::CompileOptions{false});
    if (!graph.compileResult())
    {
        return fail(graph.lastError().message, Halcyon::ErrorCode::InvalidState);
    }
    frame.passNames.clear();
    for (const auto handle : graph.compileResult().executionOrder)
    {
        if (const auto* pass = graph.compileResult().pass(handle); pass != nullptr)
            frame.passNames.push_back(pass->name);
    }
    Graph::CommandContext commands;
    graph.execute(commands,
        Graph::ExecuteOptions{
            &frame,
            [this, &frame](const Graph::PassExecutionContext& context)
            {
                (void)frameContext.writePassTimestamp(frame.commandBuffer, frame,
                    context.executionIndex, true);
            },
            [this, &frame](const Graph::PassExecutionContext& context)
            {
                (void)frameContext.writePassTimestamp(frame.commandBuffer, frame,
                    context.executionIndex, false);
            }});
    if (graph.lastError())
    {
        return fail(graph.lastError().message, Halcyon::ErrorCode::InvalidState);
    }
    if (fatalError)
    {
        return fail(lastError.empty() ? "Vulkan M3 pass recording failed" : lastError,
            Halcyon::ErrorCode::Backend);
    }
    if (currentFrame < gpuVisibilityValid.size())
        gpuVisibilityValid[currentFrame] = config.enableGpuDrivenScene && gpuIndirectCompatible;
    if (config.enableGpuDrivenScene && gpuIndirectCompatible &&
        currentFrame < gpuVisibilityReadbacks.size())
    {
        const VkBuffer visibleCount = config.enableTwoPhaseOcclusion
            ? gpuSceneBuffers.phase1VisibleCountBuffer()
            : gpuSceneBuffers.visibleCountBuffer();
        const VkBuffer phase2Count = gpuSceneBuffers.phase2VisibleCountBuffer();
        const VkBuffer readback = gpuVisibilityReadbacks[currentFrame].buffer;
        VkBufferMemoryBarrier2 sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        sourceBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        sourceBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        sourceBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        sourceBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        std::array<VkBufferMemoryBarrier2, 7> sourceBarriers{
            sourceBarrier, sourceBarrier, sourceBarrier, sourceBarrier, sourceBarrier,
            sourceBarrier, sourceBarrier};
        const VkBuffer firstIndices = config.enableTwoPhaseOcclusion
            ? gpuSceneBuffers.phase1VisibleIndicesBuffer()
            : gpuSceneBuffers.visibleIndicesBuffer();
        sourceBarriers[0].buffer = visibleCount; sourceBarriers[0].size = sizeof(std::uint32_t);
        sourceBarriers[1].buffer = phase2Count; sourceBarriers[1].size = sizeof(std::uint32_t);
        sourceBarriers[2].buffer = firstIndices; sourceBarriers[2].size = VK_WHOLE_SIZE;
        sourceBarriers[3].buffer = gpuSceneBuffers.phase2VisibleIndicesBuffer(); sourceBarriers[3].size = VK_WHOLE_SIZE;
        sourceBarriers[4].buffer = gpuSceneBuffers.visibleIndicesBuffer(); sourceBarriers[4].size = VK_WHOLE_SIZE;
        sourceBarriers[5].buffer = gpuSceneBuffers.indirectDrawCountBuffer(); sourceBarriers[5].size = sizeof(std::uint32_t);
        sourceBarriers[6].buffer = gpuSceneBuffers.phase2IndirectDrawCountBuffer(); sourceBarriers[6].size = sizeof(std::uint32_t);
        VkDependencyInfo sourceDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        sourceDependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(sourceBarriers.size());
        sourceDependency.pBufferMemoryBarriers = sourceBarriers.data();
        vkCmdPipelineBarrier2(frame.commandBuffer, &sourceDependency);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(std::min<std::uint32_t>(
            gpuSceneBuffers.capacity(), VisibilityReadbackCapacity)) * sizeof(std::uint32_t);
        const VkBufferCopy frustumCountCopy{0, 0, sizeof(std::uint32_t)};
        const VkBufferCopy countCopy{0, sizeof(std::uint32_t), sizeof(std::uint32_t)};
        const VkBufferCopy firstIndicesCopy{0,
            sizeof(std::uint32_t) * VisibilityReadbackHeaderCount, indexBytes};
        const VkBufferCopy secondCountCopy{0, sizeof(std::uint32_t) * 2u,
            sizeof(std::uint32_t)};
        const VkBufferCopy indirectCountCopy{0, sizeof(std::uint32_t) * 3u,
            sizeof(std::uint32_t)};
        const VkBufferCopy phase2IndirectCountCopy{0, sizeof(std::uint32_t) * 4u,
            sizeof(std::uint32_t)};
        const VkBufferCopy secondIndicesCopy{0,
            sizeof(std::uint32_t) * (VisibilityReadbackHeaderCount + VisibilityReadbackCapacity),
            indexBytes};
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.visibleCountBuffer(), readback,
            1, &frustumCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, visibleCount, readback, 1, &countCopy);
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.indirectDrawCountBuffer(), readback,
            1, &indirectCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.phase2IndirectDrawCountBuffer(), readback,
            1, &phase2IndirectCountCopy);
        vkCmdCopyBuffer(frame.commandBuffer, firstIndices, readback, 1, &firstIndicesCopy);
        if (config.enableTwoPhaseOcclusion && hasRenderedFrame)
        {
            vkCmdCopyBuffer(frame.commandBuffer, phase2Count, readback, 1, &secondCountCopy);
            vkCmdCopyBuffer(frame.commandBuffer, gpuSceneBuffers.phase2VisibleIndicesBuffer(),
                readback, 1, &secondIndicesCopy);
        }
        else
        {
            // Keep the phase-1 indirect command count at header slot 3;
            // only the phase-2 fields are absent in a frustum-only frame.
            vkCmdFillBuffer(frame.commandBuffer, readback, sizeof(std::uint32_t) * 2u,
                sizeof(std::uint32_t), 0);
            vkCmdFillBuffer(frame.commandBuffer, readback, sizeof(std::uint32_t) * 4u,
                sizeof(std::uint32_t), 0);
        }
    }
    if (timestampsEnabled)
    {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            frameContext.timestampPool, frame.queryBase + 1);
    }
    taaHistoryFlip = !frameHistoryFlip;
    result = vkEndCommandBuffer(frame.commandBuffer);
    if (result != VK_SUCCESS)
    {
        deviceLost = deviceLost || result == VK_ERROR_DEVICE_LOST;
        return fail(vkFailure("vkEndCommandBuffer", result));
    }
    return ok();
}


Renderer::Renderer() noexcept
        : impl_(new (std::nothrow) Impl{})
{
}

Renderer::~Renderer()
{
    if (impl_ != nullptr)
    {
        impl_->cleanup();
        delete impl_;
        impl_ = nullptr;
    }
}

Renderer::Renderer(Renderer&& other) noexcept
        : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

Renderer& Renderer::operator=(Renderer&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    if (impl_ != nullptr)
    {
        impl_->cleanup();
        delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

Halcyon::Result<void> Renderer::initialize(GLFWwindow* window, const RendererConfig& config)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::OutOfMemory, "failed to allocate Vulkan renderer state"});
    }
    impl_->cleanup();
    impl_->lastError.clear();
    if (window == nullptr)
    {
        impl_->setError("Renderer::initialize received a null GLFWwindow");
        return Halcyon::Result<void>::failure(Halcyon::Error{Halcyon::ErrorCode::InvalidArgument,
            impl_->lastError,
            "Vulkan renderer initialization"});
    }
    impl_->config = config;
    impl_->window = window;
    impl_->requestedExtent = {config.initialExtent.width, config.initialExtent.height};
    if (impl_->config.targetFrameTimeMs <= 0.0f)
    {
        impl_->config.targetFrameTimeMs = 16.667f;
    }
    try
    {
        VoidResult result = impl_->deviceState.initialize(window, impl_->config);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createM3Descriptors();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result =
            impl_->gpuAllocator.initialize(impl_->instance, impl_->physicalDevice, impl_->device);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->frameGraphProvider.initialize(
            impl_->device, impl_->physicalDevice, impl_->gpuAllocator);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->gpuSceneBuffers.initialize(impl_->device, impl_->gpuAllocator,
            131072u, impl_->config.framesInFlight);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
#if HALCYON_BUILD_EXPERIMENTAL_M2
        if (impl_->caps.descriptorIndexing)
        {
            const auto bindlessResult = impl_->bindlessTable.initialize(
                impl_->device, bindlessConfig(impl_->physicalProperties.limits));
            impl_->caps.bindlessTable = static_cast<bool>(bindlessResult);
        }
#endif
        result = impl_->swapchainState.initialize(impl_->physicalDevice,
            impl_->device,
            impl_->surface,
            window,
            impl_->graphicsQueueFamily,
            impl_->presentQueueFamily,
            VkExtent2D{impl_->requestedExtent.width, impl_->requestedExtent.height});
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createTimelineSemaphore();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->createFrameResources();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        result = impl_->sceneResources.initialize(impl_->device,
            impl_->physicalDevice,
            impl_->frames.front().commandPool,
            impl_->graphicsQueue,
            impl_->gpuAllocator,
            impl_->gpuUploader,
            impl_->config.enableGpuDrivenScene);
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        if (window != nullptr)
        {
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        }
        if (framebufferWidth > 0 && framebufferHeight > 0)
        {
            impl_->requestedExtent = {static_cast<std::uint32_t>(framebufferWidth),
                static_cast<std::uint32_t>(framebufferHeight)};
        }
        result = impl_->createSwapchain();
        if (!result)
        {
            impl_->setError(result.error().describe());
            impl_->cleanup();
            return result;
        }
        impl_->initialized = true;
        impl_->framebufferResized = false;
        return Halcyon::Result<void>::success();
    }
    catch (const std::exception& exception)
    {
        impl_->setError(exception.what());
        impl_->cleanup();
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::Backend, impl_->lastError, "Vulkan renderer initialization"});
    }
    catch (...)
    {
        impl_->setError("unknown exception during Vulkan renderer initialization");
        impl_->cleanup();
        return Halcyon::Result<void>::failure(Halcyon::Error{
            Halcyon::ErrorCode::Backend, impl_->lastError, "Vulkan renderer initialization"});
    }
}

FrameStats Renderer::render(const FramePacket& packet)
{
    if (impl_ == nullptr)
    {
        FrameStats stats{};
        stats.deviceLost = true;
        stats.fatalError = true;
        return stats;
    }
    return impl_->render(packet);
}

Halcyon::Result<void> Renderer::resize(Extent2D extent)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(
            Halcyon::Error{Halcyon::ErrorCode::InvalidState, "renderer state is not allocated"});
    }
    impl_->requestedExtent = {extent.width, extent.height};
    impl_->framebufferResized = true;
    // Swapchain recreation is deliberately deferred to render().  GLFW can
    // invoke resize callbacks while the framebuffer is transiently zero-sized
    // or while the platform is still processing its window event.
    return Halcyon::Result<void>::success();
}

void Renderer::shutdown() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->cleanup();
    }
}

const Capabilities& Renderer::capabilities() const noexcept
{
    static const Capabilities empty{};
    return impl_ != nullptr ? impl_->caps : empty;
}

const std::string& Renderer::lastError() const noexcept
{
    static const std::string empty;
    return impl_ != nullptr ? impl_->lastError : empty;
}

bool Renderer::initialized() const noexcept
{
    return impl_ != nullptr && impl_->initialized;
}

Halcyon::Result<void> Renderer::uploadSceneAsset(
    const Halcyon::Renderer::Scene::SceneDatabase& database,
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    if (idle != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend, "failed to synchronize scene resource upload"});
    }
    auto result = impl_->sceneResources.uploadAsset(database, imported);
    if (result)
    {
        const auto bindless = impl_->synchronizeBindlessMaterials();
        if (!bindless) result = bindless;
    }
    impl_->deviceMemoryBytes = impl_->gpuAllocator.allocatedBytes();
    return result;
}

Halcyon::Result<void> Renderer::releaseSceneAsset(
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    const VkResult idle = vkDeviceWaitIdle(impl_->device);
    if (idle != VK_SUCCESS)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend, "failed to synchronize scene resource release"});
    }
    auto result = impl_->sceneResources.releaseAsset(imported);
    if (result)
    {
        const auto bindless = impl_->synchronizeBindlessMaterials();
        if (!bindless) result = bindless;
    }
    impl_->deviceMemoryBytes = impl_->gpuAllocator.allocatedBytes();
    return result;
}

Halcyon::Result<void> Renderer::remapFramePacket(
    Halcyon::Renderer::Scene::OwnedFramePacket& packet) const
{
    if (impl_ == nullptr || !impl_->initialized)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    }
    for (auto& instance : packet.instances)
    {
        const std::uint32_t stableMesh = instance.meshId;
        const std::uint32_t stableMaterial = instance.materialId;
        const auto mesh = impl_->sceneResources.meshDenseIndex(stableMesh);
        const auto material = impl_->sceneResources.materialDenseIndex(stableMaterial);
        if (mesh == std::numeric_limits<std::uint32_t>::max() ||
            material == std::numeric_limits<std::uint32_t>::max())
        {
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
                "frame packet references an unmapped scene resource (mesh slot " +
                    std::to_string(stableMesh) + ", material slot " +
                    std::to_string(stableMaterial) + ")"});
        }
        instance.meshId = mesh;
        instance.materialId = material;
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> Renderer::updateGpuScene(
    std::span<const InstanceData> instances)
{
    if (impl_ == nullptr || !impl_->initialized || impl_->frames.empty())
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    // Avoid touching device-local memory for unchanged frames. This is the
    // temporary bridge until RenderExtractor deltas are uploaded by range.
    std::uint64_t hash = 1469598103934665603ull;
    for (const InstanceData& instance : instances)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&instance);
        for (std::size_t i = 0; i < sizeof(InstanceData); ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    if (hash == impl_->gpuSceneContentHash)
        return Halcyon::Result<void>::success();
    Halcyon::Renderer::Scene::GpuSceneSoA scene;
    scene.transforms.resize(instances.size());
    scene.bounds.resize(instances.size());
    scene.meshMaterials.resize(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        scene.transforms[i].model = instances[i].transform;
        const glm::mat4 model = glm::make_mat4(instances[i].transform.data());
        const MeshResource* mesh = impl_->sceneResources.mesh(instances[i].meshId);
        if (mesh == nullptr)
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidState,
                "GPU scene references an unmapped mesh"});
        scene.bounds[i] = Halcyon::Renderer::Scene::computeWorldBounds(
            mesh->boundsMin, mesh->boundsMax, model);
        const std::uint32_t cpuFallback = mesh->indexCount == 0
            ? Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag : 0u;
        scene.meshMaterials[i] = {instances[i].meshId, instances[i].materialId,
            instances[i].flags | cpuFallback, 0};
    }
    const std::uint32_t requiredCapacity = static_cast<std::uint32_t>(std::max({
        instances.size(),
        impl_->bindlessMaterialRows.size(),
        static_cast<std::size_t>(impl_->sceneResources.meshDrawCount())}));
    auto result = impl_->gpuSceneBuffers.ensureCapacity(requiredCapacity);
    if (!result) return result;
    impl_->gpuSceneInstanceCount = static_cast<std::uint32_t>(instances.size());
    if (impl_->gpuDrivenBindless && !impl_->bindlessMaterialRows.empty())
    {
        result = impl_->gpuSceneBuffers.uploadMaterials(impl_->frames.front().commandPool,
            impl_->graphicsQueue, impl_->gpuUploader, impl_->bindlessMaterialRows);
        if (!result) return result;
    }
    auto upload = impl_->gpuSceneBuffers.upload(impl_->frames.front().commandPool,
        impl_->graphicsQueue, impl_->gpuUploader, scene);
    if (upload) impl_->gpuSceneContentHash = hash;
    return upload;
}

Halcyon::Result<void> Renderer::updateGpuSceneDelta(
    const Halcyon::Renderer::Scene::Ecs::RenderExtractor::Delta& delta)
{
    if (impl_ == nullptr || !impl_->initialized || impl_->frames.empty())
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer is not initialized"});
    const auto remap = [&](InstanceData instance) -> Halcyon::Result<InstanceData>
    {
        const auto mesh = impl_->sceneResources.meshDenseIndex(instance.meshId);
        const auto material = impl_->sceneResources.materialDenseIndex(instance.materialId);
        if (mesh == std::numeric_limits<std::uint32_t>::max() ||
            material == std::numeric_limits<std::uint32_t>::max())
            return Halcyon::Result<InstanceData>::failure({Halcyon::ErrorCode::InvalidState,
                "GPU scene delta references an unmapped resource"});
        instance.meshId = mesh;
        instance.materialId = material;
        return Halcyon::Result<InstanceData>::success(instance);
    };
    const auto boundsFor = [&](const InstanceData& instance)
    {
        const MeshResource* mesh = impl_->sceneResources.mesh(instance.meshId);
        if (mesh == nullptr)
            return Halcyon::Result<Halcyon::Renderer::Scene::BoundsRow>::failure(
                {Halcyon::ErrorCode::InvalidState, "GPU scene references an unmapped mesh"});
        const glm::mat4 model = glm::make_mat4(instance.transform.data());
        return Halcyon::Result<Halcyon::Renderer::Scene::BoundsRow>::success(
            Halcyon::Renderer::Scene::computeWorldBounds(
                mesh->boundsMin, mesh->boundsMax, model));
    };
    for (const auto& item : delta.created)
    {
        auto mapped = remap(item.instance);
        if (!mapped) return mapped.error();
        auto bounds = boundsFor(mapped.value());
        if (!bounds) return bounds.error();
        if (const MeshResource* mesh = impl_->sceneResources.mesh(mapped.value().meshId);
            mesh != nullptr && mesh->indexCount == 0)
        {
            mapped.value().flags |= Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
        }
        if (!impl_->gpuSceneState.applyCreated(item.entity, mapped.value(), bounds.value()))
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
                "failed to apply GPU scene create delta"});
    }
    for (const auto& item : delta.updated)
    {
        auto mapped = remap(item.instance);
        if (!mapped) return mapped.error();
        auto bounds = boundsFor(mapped.value());
        if (!bounds) return bounds.error();
        if (const MeshResource* mesh = impl_->sceneResources.mesh(mapped.value().meshId);
            mesh != nullptr && mesh->indexCount == 0)
        {
            mapped.value().flags |= Halcyon::Renderer::Scene::kGpuSceneCpuFallbackFlag;
        }
        if (!impl_->gpuSceneState.applyUpdated(item.entity, mapped.value(), bounds.value()))
            return Halcyon::Result<void>::failure({Halcyon::ErrorCode::Backend,
                "failed to apply GPU scene update delta"});
    }
    for (const auto entity : delta.destroyed)
    {
        (void)impl_->gpuSceneState.applyDestroyed(entity,
            impl_->renderSerial + impl_->frames.size());
    }
    const auto& dirty = impl_->gpuSceneState.dirtyRanges();
    if (dirty.empty()) return Halcyon::Result<void>::success();
    std::uint32_t required = 0;
    for (const auto& range : dirty)
        required = std::max(required, range.first + range.count);
    impl_->gpuSceneInstanceCount = std::max(impl_->gpuSceneInstanceCount, required);
    required = std::max({required,
        static_cast<std::uint32_t>(impl_->bindlessMaterialRows.size()),
        impl_->sceneResources.meshDrawCount()});
    const bool requiresFullUpload = required > impl_->gpuSceneBuffers.capacity();
    auto result = impl_->gpuSceneBuffers.ensureCapacity(required);
    if (!result) return result;
    if (impl_->gpuDrivenBindless && !impl_->bindlessMaterialRows.empty())
    {
        result = impl_->gpuSceneBuffers.uploadMaterials(impl_->frames.front().commandPool,
            impl_->graphicsQueue, impl_->gpuUploader, impl_->bindlessMaterialRows);
        if (!result) return result;
    }
    result = requiresFullUpload
        ? impl_->gpuSceneBuffers.upload(impl_->frames.front().commandPool,
              impl_->graphicsQueue, impl_->gpuUploader, impl_->gpuSceneState.soa())
        : impl_->gpuSceneBuffers.uploadDirty(impl_->frames.front().commandPool,
              impl_->graphicsQueue, impl_->gpuUploader, impl_->gpuSceneState.soa(), dirty);
    if (result) impl_->gpuSceneState.clearDirtyRanges();
    return result;
}

bool Renderer::gpuDrivenSceneEnabled() const noexcept
{
    return impl_ != nullptr && impl_->initialized && impl_->config.enableGpuDrivenScene;
}

void Renderer::invalidateTaaHistory() noexcept
{
    if (impl_ != nullptr)
    {
        impl_->taaHistoryValid = false;
        impl_->hasRenderedFrame = false;
        impl_->previousPacketValid = false;
        impl_->previousInstances.clear();
    }
}

Halcyon::Result<void> Renderer::captureScreenshot(const std::filesystem::path& path)
{
    if (impl_ == nullptr)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidState, "renderer state is not allocated"});
    }
    return impl_->captureScreenshot(path);
}

VkInstance Renderer::instance() const noexcept
{
    return impl_ != nullptr ? impl_->instance : VK_NULL_HANDLE;
}

VkPhysicalDevice Renderer::physicalDevice() const noexcept
{
    return impl_ != nullptr ? impl_->physicalDevice : VK_NULL_HANDLE;
}

VkDevice Renderer::device() const noexcept
{
    return impl_ != nullptr ? impl_->device : VK_NULL_HANDLE;
}

VkQueue Renderer::graphicsQueue() const noexcept
{
    return impl_ != nullptr ? impl_->graphicsQueue : VK_NULL_HANDLE;
}

VkQueue Renderer::presentQueue() const noexcept
{
    return impl_ != nullptr ? impl_->presentQueue : VK_NULL_HANDLE;
}

VkFormat Renderer::swapchainFormat() const noexcept
{
    return impl_ != nullptr ? impl_->swapchainFormat : VK_FORMAT_UNDEFINED;
}

VkFormat Renderer::depthFormat() const noexcept
{
    return impl_ != nullptr ? impl_->depthFormat : VK_FORMAT_UNDEFINED;
}

VkExtent2D Renderer::swapchainExtent() const noexcept
{
    return impl_ != nullptr ? impl_->swapchainExtent : VkExtent2D{};
}

std::uint32_t Renderer::swapchainImageCount() const noexcept
{
    return impl_ != nullptr ? static_cast<std::uint32_t>(impl_->swapchainImages.size()) : 0u;
}

void Renderer::setOverlayCallback(OverlayCallback callback) noexcept
{
    if (impl_ != nullptr)
    {
        impl_->overlayCallback = callback;
    }
}

} // namespace Halcyon::Vulkan
