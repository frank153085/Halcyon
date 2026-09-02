#include "VulkanPipeline.h"

#include "EmbeddedTriangleShaders.h"
#include "ShaderLibrary.h"
#include "VulkanCommon.h"

#include <array>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> VulkanPipeline::create(VkDevice device,
    VkFormat colorFormat,
    VkFormat depthFormat,
    VkExtent2D extent,
    VkDescriptorSetLayout textureSetLayout,
    bool texturedRequested)
{
    destroy();
    if (device == VK_NULL_HANDLE || colorFormat == VK_FORMAT_UNDEFINED || extent.width == 0 ||
        extent.height == 0)
    {
        return VoidResult::success();
    }
    device_ = device;
    ShaderLibrary shaderLibrary{device};
    Halcyon::Result<VkShaderModule> vertex = Halcyon::Result<VkShaderModule>::failure(
        {Halcyon::ErrorCode::NotFound, "vertex shader is unavailable"});
    Halcyon::Result<VkShaderModule> fragment = Halcyon::Result<VkShaderModule>::failure(
        {Halcyon::ErrorCode::NotFound, "fragment shader is unavailable"});
    if (texturedRequested && textureSetLayout != VK_NULL_HANDLE)
    {
        vertex = shaderLibrary.create("textured.vert.spv");
        fragment = shaderLibrary.create("textured.frag.spv");
        textured_ = vertex && fragment;
        if (!textured_)
        {
            if (vertex)
            {
                VkShaderModule module = vertex.value();
                shaderLibrary.destroy(module);
            }
            if (fragment)
            {
                VkShaderModule module = fragment.value();
                shaderLibrary.destroy(module);
            }
        }
    }
    if (!textured_)
    {
        vertex = shaderLibrary.create("triangle.vert.spv", EmbeddedShaders::kTriangleVertexSpirv);
        fragment =
            shaderLibrary.create("triangle.frag.spv", EmbeddedShaders::kTriangleFragmentSpirv);
    }
    if (!vertex)
    {
        if (fragment)
        {
            VkShaderModule module = fragment.value();
            shaderLibrary.destroy(module);
        }
        return Halcyon::Result<void>::failure(vertex.error());
    }
    if (!fragment)
    {
        VkShaderModule module = vertex.value();
        shaderLibrary.destroy(module);
        return Halcyon::Result<void>::failure(fragment.error());
    }
    vertexShader_ = vertex.value();
    fragmentShader_ = fragment.value();

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPushConstantRange pushConstantRange{};
    if (textured_)
    {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &textureSetLayout;
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstantRange.size = sizeof(TexturedPushConstants);
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;
    }
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
    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentShader_;
    fragmentStage.pName = "main";
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {vertexStage, fragmentStage};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkVertexInputBindingDescription vertexBinding{
        0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> vertexAttributes = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
    if (textured_)
    {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(vertexAttributes.size());
        vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();
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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.logicOp = VK_LOGIC_OP_COPY;
    blending.attachmentCount = 1;
    blending.pAttachments = &blendAttachment;
    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat = depthFormat;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
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
    result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    if (result != VK_SUCCESS)
    {
        destroy();
        return fail(vkFailure("vkCreateGraphicsPipelines", result));
    }
    return VoidResult::success();
}

void VulkanPipeline::destroy() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (pipeline_ != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device_, pipeline_, nullptr);
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
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vertexShader_ = VK_NULL_HANDLE;
    fragmentShader_ = VK_NULL_HANDLE;
    textured_ = false;
}

} // namespace Halcyon::Vulkan
