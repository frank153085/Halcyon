#include "VulkanPipeline.h"

#include "ShaderLibrary.h"
#include "VulkanCommon.h"

#include <array>
#include <algorithm>
#include <string>
#include <utility>

namespace Halcyon::Vulkan
{
namespace
{

[[nodiscard]] Halcyon::Renderer::Shaders::ResourceType resourceType(
    VkDescriptorType type) noexcept
{
    using ResourceType = Halcyon::Renderer::Shaders::ResourceType;
    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            return ResourceType::SampledImage;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return ResourceType::StorageImage;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return ResourceType::Sampler;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return ResourceType::UniformBuffer;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return ResourceType::StorageBuffer;
        default:
            return ResourceType::Unknown;
    }
}

[[nodiscard]] Halcyon::Result<void> validateReflection(
    const Halcyon::Renderer::Shaders::ShaderReflection& reflection,
    std::span<const VkDescriptorSetLayout> layouts,
    std::span<const DescriptorBindingDesc> bindings,
    std::span<const VkPushConstantRange> pushConstants,
    VkShaderStageFlagBits stage,
    std::string_view shader)
{
    for (const auto& resource : reflection.resources)
    {
        if (resource.set >= layouts.size())
        {
            return fail("shader '" + std::string(shader) +
                    "' references descriptor set " + std::to_string(resource.set) +
                    " outside the pipeline layout",
                Halcyon::ErrorCode::InvalidArgument);
        }
        const auto declaration = std::find_if(bindings.begin(), bindings.end(),
            [&](const DescriptorBindingDesc& value)
            {
                return value.set == resource.set &&
                       value.binding.binding == resource.binding;
            });
        if (declaration == bindings.end())
        {
            return fail("shader '" + std::string(shader) + "' descriptor set " +
                    std::to_string(resource.set) + " binding " +
                    std::to_string(resource.binding) + " is not declared by the pipeline",
                Halcyon::ErrorCode::InvalidArgument);
        }
        if (resourceType(declaration->binding.descriptorType) != resource.type ||
            (resource.arraySize != 0u &&
                declaration->binding.descriptorCount < resource.arraySize) ||
            (declaration->binding.stageFlags & stage) == 0)
        {
            return fail("shader '" + std::string(shader) + "' descriptor set " +
                    std::to_string(resource.set) + " binding " +
                    std::to_string(resource.binding) + " has an incompatible type, count, or stage",
                Halcyon::ErrorCode::InvalidArgument);
        }
    }
    for (const auto& reflected : reflection.pushConstants)
    {
        const auto declaration = std::find_if(pushConstants.begin(), pushConstants.end(),
            [&](const VkPushConstantRange& value)
            {
                return value.offset == 0 && value.size >= reflected.size &&
                       (value.stageFlags & stage) != 0;
            });
        if (reflected.size == 0u || (reflected.size % 16u) != 0u ||
            declaration == pushConstants.end())
        {
            return fail("shader '" + std::string(shader) +
                    "' has an incompatible or non-16-byte-aligned push-constant block",
                Halcyon::ErrorCode::InvalidArgument);
        }
    }
    return Halcyon::Result<void>::success();
}

} // namespace

Halcyon::Result<void> VulkanPipeline::createGraphics(
    VkDevice device, const GraphicsPipelineDesc& desc)
{
    VulkanPipeline candidate;
    const auto result = candidate.createGraphicsInternal(device, desc);
    if (!result) return result;
    swap(candidate);
    return result;
}

Halcyon::Result<void> VulkanPipeline::createCompute(
    VkDevice device, const ComputePipelineDesc& desc)
{
    VulkanPipeline candidate;
    const auto result = candidate.createComputeInternal(device, desc);
    if (!result) return result;
    swap(candidate);
    return result;
}

Halcyon::Result<void> VulkanPipeline::createGraphicsInternal(
    VkDevice device, const GraphicsPipelineDesc& desc)
{
    if (device == VK_NULL_HANDLE || desc.vertexShader.empty() ||
        (!desc.depthOnly && desc.colorFormats.empty()) ||
        (desc.depthOnly && (desc.depthFormat == VK_FORMAT_UNDEFINED ||
                               !desc.colorFormats.empty())) ||
        (desc.depthFormat == VK_FORMAT_UNDEFINED && (desc.depthTest || desc.depthWrite)))
    {
        return fail("invalid graphics pipeline description", Halcyon::ErrorCode::InvalidArgument);
    }
    device_ = device;
    ShaderLibrary shaderLibrary{device};
    Halcyon::Renderer::Shaders::ShaderReflection vertexReflection;
    const auto vertex = shaderLibrary.create(desc.vertexShader, &vertexReflection);
    if (!vertex) return Halcyon::Result<void>::failure(vertex.error());
    vertexShader_ = vertex.value();
    const auto vertexAbi = validateReflection(vertexReflection, desc.descriptorLayouts,
        desc.descriptorBindings, desc.pushConstants, VK_SHADER_STAGE_VERTEX_BIT,
        desc.vertexShader);
    if (!vertexAbi)
    {
        destroy();
        return vertexAbi;
    }
    if (!desc.depthOnly)
    {
        Halcyon::Renderer::Shaders::ShaderReflection fragmentReflection;
        const auto fragment = shaderLibrary.create(desc.fragmentShader, &fragmentReflection);
        if (!fragment)
        {
            destroy();
            return Halcyon::Result<void>::failure(fragment.error());
        }
        fragmentShader_ = fragment.value();
        const auto fragmentAbi = validateReflection(fragmentReflection, desc.descriptorLayouts,
            desc.descriptorBindings, desc.pushConstants, VK_SHADER_STAGE_FRAGMENT_BIT,
            desc.fragmentShader);
        if (!fragmentAbi)
        {
            destroy();
            return fragmentAbi;
        }
        if (fragmentReflection.outputLocations.size() != desc.colorFormats.size())
        {
            destroy();
            return fail("shader '" + std::string(desc.fragmentShader) + "' exports " +
                    std::to_string(fragmentReflection.outputLocations.size()) +
                    " color locations, but the pipeline declares " +
                    std::to_string(desc.colorFormats.size()) + " attachments",
                Halcyon::ErrorCode::InvalidArgument);
        }
        for (std::uint32_t location = 0;
            location < fragmentReflection.outputLocations.size(); ++location)
        {
            if (fragmentReflection.outputLocations[location] != location)
            {
                destroy();
                return fail("shader '" + std::string(desc.fragmentShader) +
                        "' color outputs are not contiguous from location zero",
                    Halcyon::ErrorCode::InvalidArgument);
            }
        }
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(desc.descriptorLayouts.size());
    layoutInfo.pSetLayouts = desc.descriptorLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<std::uint32_t>(desc.pushConstants.size());
    layoutInfo.pPushConstantRanges = desc.pushConstants.data();
    VkResult result = vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_);
    if (result != VK_SUCCESS)
    {
        destroy();
        return fail(vkFailure("vkCreatePipelineLayout", result));
    }

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexShader_;
    vertexStage.pName = "main";
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertexStage, {}};
    std::uint32_t stageCount = 1;
    if (!desc.depthOnly)
    {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentShader_;
        stages[1].pName = "main";
        stageCount = 2;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    const bool fullscreen = desc.vertexShader.find("fullscreen") != std::string_view::npos;
    VkVertexInputBindingDescription binding{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 4> attributes = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}};
    if (!fullscreen)
    {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = desc.depthOnly ? 1u :
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = desc.cullMode;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = desc.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterization.depthBiasConstantFactor = desc.depthBiasConstant;
    rasterization.depthBiasSlopeFactor = desc.depthBiasSlope;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(desc.colorFormats.size());
    for (auto& blend : blendAttachments)
    {
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
    blending.pAttachments = blendAttachments.data();
    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthTest && desc.depthFormat != VK_FORMAT_UNDEFINED;
    depthStencil.depthWriteEnable = desc.depthWrite && desc.depthFormat != VK_FORMAT_UNDEFINED;
    depthStencil.depthCompareOp = desc.depthCompare;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(desc.colorFormats.size());
    renderingInfo.pColorAttachmentFormats = desc.colorFormats.data();
    renderingInfo.depthAttachmentFormat = desc.depthFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout_;
    result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    if (result != VK_SUCCESS)
    {
        destroy();
        return fail(vkFailure("vkCreateGraphicsPipelines", result));
    }
    return VoidResult::success();
}

Halcyon::Result<void> VulkanPipeline::createComputeInternal(
    VkDevice device, const ComputePipelineDesc& desc)
{
    if (device == VK_NULL_HANDLE || desc.shader.empty())
        return fail("invalid compute pipeline description", Halcyon::ErrorCode::InvalidArgument);
    device_ = device;
    ShaderLibrary shaderLibrary{device};
    Halcyon::Renderer::Shaders::ShaderReflection reflection;
    const auto shader = shaderLibrary.create(desc.shader, &reflection);
    if (!shader) return Halcyon::Result<void>::failure(shader.error());
    computeShader_ = shader.value();
    const auto abi = validateReflection(reflection, desc.descriptorLayouts,
        desc.descriptorBindings, desc.pushConstants, VK_SHADER_STAGE_COMPUTE_BIT, desc.shader);
    if (!abi)
    {
        destroy();
        return abi;
    }
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(desc.descriptorLayouts.size());
    layoutInfo.pSetLayouts = desc.descriptorLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<std::uint32_t>(desc.pushConstants.size());
    layoutInfo.pPushConstantRanges = desc.pushConstants.data();
    VkResult result = vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_);
    if (result != VK_SUCCESS) { destroy(); return fail(vkFailure("vkCreatePipelineLayout", result)); }
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = computeShader_;
    stage.pName = "main";
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = layout_;
    result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &computePipeline_);
    if (result != VK_SUCCESS) { destroy(); return fail(vkFailure("vkCreateComputePipelines", result)); }
    return VoidResult::success();
}

void VulkanPipeline::swap(VulkanPipeline& other) noexcept
{
    using std::swap;
    swap(device_, other.device_);
    swap(layout_, other.layout_);
    swap(pipeline_, other.pipeline_);
    swap(computePipeline_, other.computePipeline_);
    swap(vertexShader_, other.vertexShader_);
    swap(fragmentShader_, other.fragmentShader_);
    swap(computeShader_, other.computeShader_);
}

void VulkanPipeline::destroy() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (pipeline_ != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (computePipeline_ != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device_, computePipeline_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        }
        if (vertexShader_ != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(device_, vertexShader_, nullptr);
        }
        if (fragmentShader_ != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(device_, fragmentShader_, nullptr);
        }
        if (computeShader_ != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(device_, computeShader_, nullptr);
        }
    }
    pipeline_ = VK_NULL_HANDLE;
    computePipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vertexShader_ = VK_NULL_HANDLE;
    fragmentShader_ = VK_NULL_HANDLE;
    computeShader_ = VK_NULL_HANDLE;
}

} // namespace Halcyon::Vulkan
