#include "VulkanDemoResources.h"

#include "VulkanCommon.h"

#include <array>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>
#include <limits>
#include <span>
#include <unordered_map>

namespace Halcyon::Vulkan
{

Halcyon::Result<void> VulkanDemoResources::initialize(VkDevice device,
    VkCommandPool uploadCommandPool,
    VkQueue graphicsQueue,
    GpuAllocator& allocator,
    GpuUploader& uploader,
    const char* startupTexturePath,
    const char* startupMeshPath,
    const char* startupScenePath)
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
    if (startupScenePath != nullptr)
    {
        const auto sceneResult = loadStaticScene(startupScenePath);
        if (!sceneResult)
        {
            cleanup();
            return sceneResult;
        }
    }
    if (demoMesh_.indexCount == 0)
    {
        loadStartupResources(startupTexturePath, startupMeshPath);
    }
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
    if (texturePath == nullptr || (meshPath == nullptr && demoMesh_.indexCount == 0))
    {
        return;
    }
    auto textureResult = resourceManager_.loadTexture2D(texturePath, true);
    auto meshResult = meshPath != nullptr
                          ? resourceManager_.loadObj(meshPath)
                          : Halcyon::Result<MeshResource>::success(demoMesh_);
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
    if (demoMesh_.indexCount == 0) demoMesh_ = meshResult.value();

    std::vector<TextureResource> textures;
    textures.push_back(demoTexture_);
    if (!createTextureDescriptors(textures))
    {
        resourceManager_.destroy(demoTexture_);
        if (demoMesh_.indexCount != 0 && meshResult.value().indexCount != 0 &&
            demoMesh_.vertexBuffer.buffer == meshResult.value().vertexBuffer.buffer)
        {
            resourceManager_.destroy(demoMesh_);
        }
        demoTexture_ = {};
        return;
    }
    sceneTextures_ = std::move(textures);
    // sceneTextures_ owns the allocation from this point on.  demoTexture_
    // remains an empty compatibility slot so cleanup cannot double-free it.
    demoTexture_ = {};
    sceneDraws_.clear();
    sceneDraws_.push_back(SceneDraw{0, demoMesh_.indexCount, 0, 0, 0});
    primitiveCount_ = 1;
}

bool VulkanDemoResources::createTextureDescriptors(
    std::vector<TextureResource>& textures) noexcept
{
    if (device_ == VK_NULL_HANDLE || textures.empty()) return false;
    destroyDescriptorResources();
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
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        static_cast<std::uint32_t>(textures.size())};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(textures.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &textureDescriptorPool_) != VK_SUCCESS)
    {
        destroyDescriptorResources();
        return false;
    }

    sceneTextureDescriptorSets_.resize(textures.size());
    std::vector<VkDescriptorSetLayout> layouts(textures.size(), textureSetLayout_);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = textureDescriptorPool_;
    allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocateInfo, sceneTextureDescriptorSets_.data()) !=
        VK_SUCCESS)
    {
        sceneTextureDescriptorSets_.clear();
        destroyDescriptorResources();
        return false;
    }
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        VkDescriptorImageInfo imageInfo{
            textures[i].sampler, textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sceneTextureDescriptorSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    textureDescriptorSet_ = sceneTextureDescriptorSets_.front();
    textured_ = true;
    return true;
}

Halcyon::Result<void> VulkanDemoResources::loadStaticScene(const char* scenePath) noexcept
{
    if (scenePath == nullptr)
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Scene path is null"});
    }
    const auto loaded = Halcyon::Renderer::Scene::loadStaticScene(scenePath);
    if (!loaded)
    {
        return Halcyon::Result<void>::failure(loaded.error());
    }
    if (loaded.value().primitives.empty())
    {
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Static scene contains no primitives"});
    }
    const auto& sourceScene = loaded.value();
    // Flatten the static node hierarchy into one uploadable mesh.  The live
    // M1 renderer submits one indexed draw, while the loader still preserves
    // all primitive/material/node records for CPU-side inspection.  Applying
    // each primitive's node transform here makes large scenes such as Sponza
    // render at their authored scale (its root node is scaled by 0.008).
    Halcyon::Renderer::Scene::StaticScenePrimitive combined;
    combined.worldTransform = glm::mat4{1.0f};
    combined.boundsMin = glm::vec3{std::numeric_limits<float>::max()};
    combined.boundsMax = glm::vec3{-std::numeric_limits<float>::max()};
    sceneDraws_.clear();
    std::vector<std::string> materialTexturePaths;
    materialTexturePaths.reserve(sourceScene.materials.size());
    // Keep missing material images deterministic and independent of the
    // process working directory.  The compatibility pipeline samples only
    // base color today, but the same keys are used by the scene database for
    // normal/metallic/emissive fallbacks.
    const std::string fallbackTexture = "__halcyon_default_white_srgb__";
    const auto loadMaterialTexture = [&](const std::string& key)
        -> Halcyon::Result<TextureResource>
    {
        if (key == "__halcyon_default_white_srgb__")
            return resourceManager_.loadSolidColorTexture({255, 255, 255, 255}, true);
        if (key == "__halcyon_default_normal__")
            return resourceManager_.loadSolidColorTexture({128, 128, 255, 255}, false);
        if (key == "__halcyon_default_black__")
            return resourceManager_.loadSolidColorTexture({0, 0, 0, 255}, false);
        if (key == "__halcyon_default_black_srgb__")
            return resourceManager_.loadSolidColorTexture({0, 0, 0, 255}, true);
        return resourceManager_.loadTexture2D(key, true);
    };
    for (const auto& source : sourceScene.primitives)
    {
        const std::uint32_t base = static_cast<std::uint32_t>(combined.vertices.size());
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(combined.indices.size());
        const glm::mat4 world = source.worldTransform;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
        for (const auto& vertex : source.vertices)
        {
            Halcyon::Renderer::Scene::StaticSceneVertex transformed = vertex;
            const glm::vec4 position = world * glm::vec4(vertex.position, 1.0f);
            transformed.position = glm::vec3(position);
            transformed.normal = glm::normalize(normalMatrix * vertex.normal);
            combined.boundsMin = glm::min(combined.boundsMin, transformed.position);
            combined.boundsMax = glm::max(combined.boundsMax, transformed.position);
            combined.vertices.push_back(transformed);
        }
        combined.indices.reserve(combined.indices.size() + source.indices.size());
        for (const std::uint32_t index : source.indices)
        {
            if (index < source.vertices.size()) combined.indices.push_back(base + index);
        }
        const std::uint32_t indexCount = static_cast<std::uint32_t>(combined.indices.size()) - firstIndex;
        if (indexCount == 0) continue;
        SceneDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = indexCount;
        draw.vertexOffset = 0;
        draw.materialIndex = source.materialIndex;
        sceneDraws_.push_back(draw);
        std::string texture = fallbackTexture;
        if (source.materialIndex < sourceScene.materials.size() &&
            !sourceScene.materials[source.materialIndex].baseColorTexture.empty())
        {
            texture = sourceScene.materials[source.materialIndex].baseColorTexture;
        }
        materialTexturePaths.push_back(std::move(texture));
    }
    const auto mesh = resourceManager_.uploadPrimitive(combined);
    if (!mesh)
    {
        return Halcyon::Result<void>::failure(mesh.error());
    }
    demoMesh_ = mesh.value();
    primitiveCount_ = static_cast<std::uint32_t>(sourceScene.primitives.size());

    // Deduplicate material textures and upload only those referenced by a
    // primitive.  The compatibility pipeline still uses one descriptor set
    // layout, but each indexed range now binds its own material texture.
    std::unordered_map<std::string, std::uint32_t> textureIndices;
    std::vector<TextureResource> textures;
    textures.reserve(materialTexturePaths.size());
    for (std::size_t i = 0; i < materialTexturePaths.size(); ++i)
    {
        const auto& pathString = materialTexturePaths[i];
        auto found = textureIndices.find(pathString);
        std::uint32_t textureIndex = 0;
        if (found != textureIndices.end())
        {
            textureIndex = found->second;
        }
        else
        {
            auto textureResult = loadMaterialTexture(pathString);
            if (!textureResult && pathString != fallbackTexture)
                textureResult = loadMaterialTexture(fallbackTexture);
            if (!textureResult) continue;
            textureIndex = static_cast<std::uint32_t>(textures.size());
            textures.push_back(textureResult.value());
            textureIndices.emplace(pathString, textureIndex);
        }
        if (i < sceneDraws_.size()) sceneDraws_[i].textureIndex = textureIndex;
    }
    if (textures.empty() || !createTextureDescriptors(textures))
    {
        for (auto& texture : textures) resourceManager_.destroy(texture);
        sceneTextures_.clear();
        sceneDraws_.clear();
        return Halcyon::Result<void>::failure(
            {Halcyon::ErrorCode::Backend, "Failed to create static-scene texture descriptors"});
    }
    sceneTextures_ = std::move(textures);
    demoTexture_ = {};
    return ok();
}

Halcyon::Result<TextureResource> VulkanDemoResources::loadTexture2D(const char* path)
{
    if (path == nullptr)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Texture path is null"});
    }
    return resourceManager_.loadTexture2D(path, true);
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
    sceneTextureDescriptorSets_.clear();
}

void VulkanDemoResources::cleanup() noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        destroyDescriptorResources();
        for (auto& texture : sceneTextures_)
        {
            resourceManager_.destroy(texture);
        }
        sceneTextures_.clear();
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
    sceneTextureDescriptorSets_.clear();
    sceneDraws_.clear();
    sceneTextures_.clear();
    primitiveCount_ = 0;
    resourceManager_.shutdown();
    device_ = VK_NULL_HANDLE;
    uploadCommandPool_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    uploader_ = nullptr;
}

} // namespace Halcyon::Vulkan
