#include "VulkanDemoResources.h"

#include "VulkanCommon.h"

#include <array>
#include <span>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> VulkanDemoResources::initialize(VkDevice device,
    VkCommandPool uploadCommandPool,
    VkQueue graphicsQueue,
    GpuAllocator& allocator,
    GpuUploader& uploader,
    const char* startupTexturePath,
    const char* startupMeshPath)
{
    cleanup();
    if (device == VK_NULL_HANDLE || uploadCommandPool == VK_NULL_HANDLE ||
        graphicsQueue == VK_NULL_HANDLE)
    {
        return fail("Cannot initialize demo resources without a Vulkan device and queue",
            Halcyon::ErrorCode::InvalidState);
    }
    device_ = device;
    uploadCommandPool_ = uploadCommandPool;
    graphicsQueue_ = graphicsQueue;
    allocator_ = &allocator;
    uploader_ = &uploader;
    resourceManager_.initialize(device_, uploadCommandPool, graphicsQueue_, allocator, uploader);
    const auto geometryResult = createTriangleGeometry();
    if (!geometryResult)
    {
        cleanup();
        return geometryResult;
    }
    loadStartupResources(startupTexturePath, startupMeshPath);
    return ok();
}

Halcyon::Result<void> VulkanDemoResources::createTriangleGeometry()
{
    constexpr std::array<float, 15> vertices = {0.0f,
        -0.72f,
        1.0f,
        0.25f,
        0.20f,
        0.72f,
        0.72f,
        0.20f,
        1.0f,
        0.35f,
        -0.72f,
        0.72f,
        0.20f,
        0.45f,
        1.0f};
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(vertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto bufferResult = allocator_->createBuffer(bufferInfo, MemoryUsage::GpuOnly);
    if (!bufferResult)
    {
        return bufferResult.error();
    }
    triangleVertexBuffer_ = bufferResult.value();
    const auto uploadResult = uploader_->uploadBuffer(device_,
        uploadCommandPool_,
        graphicsQueue_,
        *allocator_,
        triangleVertexBuffer_,
        std::as_bytes(std::span(vertices)));
    if (!uploadResult)
    {
        allocator_->destroy(triangleVertexBuffer_);
        triangleVertexBuffer_ = {};
        return uploadResult;
    }
    return ok();
}

void VulkanDemoResources::loadStartupResources(
    const char* texturePath, const char* meshPath) noexcept
{
    if (texturePath == nullptr || meshPath == nullptr)
    {
        return;
    }
    auto textureResult = resourceManager_.loadTexture2D(texturePath);
    auto meshResult = resourceManager_.loadObj(meshPath);
    if (!textureResult || !meshResult)
    {
        if (textureResult)
        {
            auto texture = textureResult.value();
            resourceManager_.destroy(texture);
        }
        if (meshResult)
        {
            auto mesh = meshResult.value();
            resourceManager_.destroy(mesh);
        }
        return;
    }
    demoTexture_ = textureResult.value();
    demoMesh_ = meshResult.value();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &textureSetLayout_) !=
        VK_SUCCESS)
    {
        resourceManager_.destroy(demoTexture_);
        resourceManager_.destroy(demoMesh_);
        return;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &textureDescriptorPool_) != VK_SUCCESS)
    {
        destroyDescriptorResources();
        resourceManager_.destroy(demoTexture_);
        resourceManager_.destroy(demoMesh_);
        return;
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = textureDescriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &textureSetLayout_;
    if (vkAllocateDescriptorSets(device_, &allocateInfo, &textureDescriptorSet_) != VK_SUCCESS)
    {
        destroyDescriptorResources();
        resourceManager_.destroy(demoTexture_);
        resourceManager_.destroy(demoMesh_);
        return;
    }

    VkDescriptorImageInfo imageInfo{
        demoTexture_.sampler, demoTexture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = textureDescriptorSet_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    textured_ = true;
}

Halcyon::Result<TextureResource> VulkanDemoResources::loadTexture2D(const char* path)
{
    if (path == nullptr)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Texture path is null"});
    }
    return resourceManager_.loadTexture2D(path);
}

Halcyon::Result<MeshResource> VulkanDemoResources::loadObj(const char* path)
{
    if (path == nullptr)
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Model path is null"});
    }
    return resourceManager_.loadObj(path);
}

void VulkanDemoResources::destroy(TextureResource& texture) noexcept
{
    resourceManager_.destroy(texture);
}

void VulkanDemoResources::destroy(MeshResource& mesh) noexcept
{
    resourceManager_.destroy(mesh);
}

void VulkanDemoResources::destroyDescriptorResources() noexcept
{
    if (device_ != VK_NULL_HANDLE && textureDescriptorPool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device_, textureDescriptorPool_, nullptr);
    }
    textureDescriptorPool_ = VK_NULL_HANDLE;
    textureDescriptorSet_ = VK_NULL_HANDLE;
    if (device_ != VK_NULL_HANDLE && textureSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device_, textureSetLayout_, nullptr);
    }
    textureSetLayout_ = VK_NULL_HANDLE;
    textured_ = false;
}

void VulkanDemoResources::cleanup() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        destroyDescriptorResources();
        resourceManager_.destroy(demoTexture_);
        resourceManager_.destroy(demoMesh_);
        if (allocator_ != nullptr)
        {
            allocator_->destroy(triangleVertexBuffer_);
        }
    }
    triangleVertexBuffer_ = {};
    demoTexture_ = {};
    demoMesh_ = {};
    resourceManager_.shutdown();
    device_ = VK_NULL_HANDLE;
    uploadCommandPool_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    uploader_ = nullptr;
}

} // namespace Halcyon::Vulkan
