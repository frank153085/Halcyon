#include "VulkanSceneResources.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace Halcyon::Vulkan
{
namespace
{

[[nodiscard]] Halcyon::Result<void> resourceError(Halcyon::ErrorCode code, std::string message)
{
    return Halcyon::Result<void>::failure(
        Halcyon::Error{code, std::move(message), "VulkanSceneResources"});
}

} // namespace

Halcyon::Result<void> VulkanSceneResources::initialize(VkDevice device,
    VkCommandPool uploadCommandPool,
    VkQueue graphicsQueue,
    GpuAllocator& allocator,
    GpuUploader& uploader)
{
    cleanup();
    if (device == VK_NULL_HANDLE || uploadCommandPool == VK_NULL_HANDLE ||
        graphicsQueue == VK_NULL_HANDLE)
    {
        return resourceError(Halcyon::ErrorCode::InvalidState,
            "cannot initialize scene resources without a Vulkan device and queue");
    }
    device_ = device;
    uploadCommandPool_ = uploadCommandPool;
    graphicsQueue_ = graphicsQueue;
    allocator_ = &allocator;
    uploader_ = &uploader;
    resourceManager_.initialize(device_, uploadCommandPool_, graphicsQueue_, allocator, uploader);
    const auto layout = createDescriptorLayout();
    if (!layout)
    {
        cleanup();
    }
    return layout;
}

Halcyon::Result<void> VulkanSceneResources::createDescriptorLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &textureSetLayout_) != VK_SUCCESS)
    {
        return resourceError(
            Halcyon::ErrorCode::Backend, "failed to create the scene material descriptor layout");
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<std::string> VulkanSceneResources::retainTexture(
    const Halcyon::Renderer::Scene::SceneTexture& textureRecord)
{
    const std::string key = (textureRecord.srgb ? "s:" : "l:") + textureRecord.path;
    const auto existing = sharedTextures_.find(key);
    if (existing != sharedTextures_.end())
    {
        ++existing->second.references;
        return Halcyon::Result<std::string>::success(key);
    }

    Halcyon::Result<TextureResource> loaded = Halcyon::Result<TextureResource>::failure(
        {Halcyon::ErrorCode::NotFound, "scene texture is unavailable"});
    if (textureRecord.generatedDefault || textureRecord.path.starts_with("__halcyon_default_"))
    {
        loaded =
            resourceManager_.loadSolidColorTexture(textureRecord.solidColor, textureRecord.srgb);
    }
    else
    {
        loaded = resourceManager_.loadTexture2D(textureRecord.path, textureRecord.srgb);
    }
    if (!loaded)
    {
        return Halcyon::Result<std::string>::failure(loaded.error());
    }
    try
    {
        sharedTextures_.emplace(key, SharedTexture{loaded.value(), 1});
    }
    catch (...)
    {
        TextureResource resource = loaded.value();
        resourceManager_.destroy(resource);
        return Halcyon::Result<std::string>::failure(
            {Halcyon::ErrorCode::OutOfMemory, "failed to index uploaded scene texture"});
    }
    return Halcyon::Result<std::string>::success(key);
}

const TextureResource* VulkanSceneResources::texture(std::uint32_t index) const noexcept
{
    const auto key = textureKeys_.find(index);
    if (key == textureKeys_.end())
    {
        return nullptr;
    }
    const auto found = sharedTextures_.find(key->second);
    return found != sharedTextures_.end() ? &found->second.resource : nullptr;
}

void VulkanSceneResources::releaseTexture(std::uint32_t index) noexcept
{
    const auto key = textureKeys_.find(index);
    if (key == textureKeys_.end())
    {
        return;
    }
    const auto shared = sharedTextures_.find(key->second);
    if (shared != sharedTextures_.end())
    {
        if (shared->second.references > 1)
        {
            --shared->second.references;
        }
        else
        {
            resourceManager_.destroy(shared->second.resource);
            sharedTextures_.erase(shared);
            textureKeys_.erase(key);
        }
    }
}

Halcyon::Result<void> VulkanSceneResources::uploadAsset(
    const Halcyon::Renderer::Scene::SceneDatabase& database,
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return resourceError(
            Halcyon::ErrorCode::InvalidState, "scene resources are not initialized");
    }

    std::vector<std::uint32_t> uploadedTextures;
    std::vector<std::uint32_t> uploadedMeshes;
    std::vector<std::uint32_t> uploadedMaterials;
    try
    {
        uploadedTextures.reserve(imported.textures.size());
        uploadedMeshes.reserve(imported.meshes.size());
        uploadedMaterials.reserve(imported.materials.size());
    }
    catch (...)
    {
        return resourceError(
            Halcyon::ErrorCode::OutOfMemory, "failed to allocate scene upload transaction state");
    }
    const auto rollback = [&]() noexcept
    {
        for (const std::uint32_t material : uploadedMaterials)
        {
            materials_.erase(material);
        }
        for (const std::uint32_t mesh : uploadedMeshes)
        {
            const auto found = meshes_.find(mesh);
            if (found != meshes_.end())
            {
                resourceManager_.destroy(found->second);
                meshes_.erase(found);
            }
        }
        for (const std::uint32_t textureIndex : uploadedTextures)
        {
            releaseTexture(textureIndex);
        }
        (void)rebuildMaterialDescriptors();
    };

    for (const Halcyon::Renderer::Resources::TextureHandle handle : imported.textures)
    {
        const auto* source = database.get(handle);
        if (source == nullptr)
        {
            rollback();
            return resourceError(Halcyon::ErrorCode::InvalidArgument,
                "scene upload contains an invalid texture handle");
        }
        const std::uint32_t index = handle.index();
        const auto retained = retainTexture(*source);
        if (!retained)
        {
            rollback();
            return Halcyon::Result<void>::failure(retained.error());
        }
        bool indexed = false;
        try
        {
            // A texture handle can be referenced by more than one asset. The
            // shared texture retain above accounts for each asset, while the
            // index-to-key map remains a single lookup entry.
            if (!textureKeys_.contains(index))
            {
                textureKeys_.emplace(index, retained.value());
                indexed = true;
            }
            uploadedTextures.push_back(index);
        }
        catch (...)
        {
            if (indexed)
            {
                releaseTexture(index);
            }
            else
            {
                auto shared = sharedTextures_.find(retained.value());
                if (shared != sharedTextures_.end() && shared->second.references > 0)
                {
                    if (--shared->second.references == 0)
                    {
                        resourceManager_.destroy(shared->second.resource);
                        sharedTextures_.erase(shared);
                    }
                }
            }
            rollback();
            return resourceError(
                Halcyon::ErrorCode::OutOfMemory, "failed to index uploaded scene texture");
        }
    }

    for (const Halcyon::Renderer::Resources::MeshHandle handle : imported.meshes)
    {
        const auto* source = database.get(handle);
        if (source == nullptr || meshes_.contains(handle.index()))
        {
            rollback();
            return resourceError(Halcyon::ErrorCode::InvalidArgument,
                "scene upload contains an invalid or duplicate mesh handle");
        }
        const auto uploaded = resourceManager_.uploadMesh(*source);
        if (!uploaded)
        {
            rollback();
            return Halcyon::Result<void>::failure(uploaded.error());
        }
        try
        {
            meshes_.emplace(handle.index(), uploaded.value());
            uploadedMeshes.push_back(handle.index());
        }
        catch (...)
        {
            MeshResource resource = uploaded.value();
            resourceManager_.destroy(resource);
            rollback();
            return resourceError(
                Halcyon::ErrorCode::OutOfMemory, "failed to index uploaded scene mesh");
        }
    }

    for (const Halcyon::Renderer::Resources::MaterialHandle handle : imported.materials)
    {
        const auto* source = database.get(handle);
        if (source == nullptr || materials_.contains(handle.index()))
        {
            rollback();
            return resourceError(Halcyon::ErrorCode::InvalidArgument,
                "scene upload contains an invalid or duplicate material handle");
        }
        if (!source->baseColorTexture.isValid() ||
            texture(source->baseColorTexture.index()) == nullptr)
        {
            rollback();
            return resourceError(Halcyon::ErrorCode::InvalidArgument,
                "scene material references an unavailable base-color texture");
        }
        try
        {
            materials_.emplace(handle.index(), MaterialResource{source->baseColorTexture.index()});
            uploadedMaterials.push_back(handle.index());
        }
        catch (...)
        {
            rollback();
            return resourceError(
                Halcyon::ErrorCode::OutOfMemory, "failed to index uploaded scene material");
        }
    }

    const auto descriptors = rebuildMaterialDescriptors();
    if (!descriptors)
    {
        rollback();
        return descriptors;
    }
    return Halcyon::Result<void>::success();
}

Halcyon::Result<void> VulkanSceneResources::releaseAsset(
    const Halcyon::Renderer::Scene::SceneImportResult& imported)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return resourceError(
            Halcyon::ErrorCode::InvalidState, "scene resources are not initialized");
    }
    for (const Halcyon::Renderer::Resources::MaterialHandle handle : imported.materials)
    {
        materials_.erase(handle.index());
    }
    for (const Halcyon::Renderer::Resources::MeshHandle handle : imported.meshes)
    {
        const auto found = meshes_.find(handle.index());
        if (found != meshes_.end())
        {
            resourceManager_.destroy(found->second);
            meshes_.erase(found);
        }
    }
    for (const Halcyon::Renderer::Resources::TextureHandle handle : imported.textures)
    {
        releaseTexture(handle.index());
    }
    return rebuildMaterialDescriptors();
}

Halcyon::Result<void> VulkanSceneResources::rebuildMaterialDescriptors()
{
    if (materials_.empty())
    {
        destroyDescriptorPool();
        materialDescriptors_.clear();
        return Halcyon::Result<void>::success();
    }

    VkDescriptorPoolSize poolSize{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<std::uint32_t>(materials_.size())};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(materials_.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool candidatePool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &candidatePool) != VK_SUCCESS)
    {
        return resourceError(
            Halcyon::ErrorCode::Backend, "failed to create the scene material descriptor pool");
    }

    std::vector<std::uint32_t> materialIndices;
    materialIndices.reserve(materials_.size());
    for (const auto& [index, material] : materials_)
    {
        (void)material;
        materialIndices.push_back(index);
    }
    std::sort(materialIndices.begin(), materialIndices.end());
    std::vector<VkDescriptorSetLayout> layouts(materialIndices.size(), textureSetLayout_);
    std::vector<VkDescriptorSet> sets(materialIndices.size());
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = candidatePool;
    allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_, &allocate, sets.data()) != VK_SUCCESS)
    {
        vkDestroyDescriptorPool(device_, candidatePool, nullptr);
        return resourceError(
            Halcyon::ErrorCode::Backend, "failed to allocate scene material descriptor sets");
    }

    std::unordered_map<std::uint32_t, VkDescriptorSet> candidateDescriptors;
    try
    {
        candidateDescriptors.reserve(materialIndices.size());
    }
    catch (...)
    {
        vkDestroyDescriptorPool(device_, candidatePool, nullptr);
        return resourceError(
            Halcyon::ErrorCode::OutOfMemory, "failed to index scene material descriptor sets");
    }
    for (std::size_t i = 0; i < materialIndices.size(); ++i)
    {
        const MaterialResource& material = materials_.at(materialIndices[i]);
        const TextureResource* source = texture(material.baseColorTexture);
        if (source == nullptr)
        {
            vkDestroyDescriptorPool(device_, candidatePool, nullptr);
            return resourceError(Halcyon::ErrorCode::InvalidState,
                "scene material texture disappeared during descriptor rebuild");
        }
        VkDescriptorImageInfo image{
            source->sampler, source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        candidateDescriptors.emplace(materialIndices[i], sets[i]);
    }
    destroyDescriptorPool();
    textureDescriptorPool_ = candidatePool;
    materialDescriptors_ = std::move(candidateDescriptors);
    return Halcyon::Result<void>::success();
}

const MeshResource* VulkanSceneResources::mesh(std::uint32_t index) const noexcept
{
    const auto found = meshes_.find(index);
    return found != meshes_.end() ? &found->second : nullptr;
}

VkDescriptorSet VulkanSceneResources::materialDescriptor(std::uint32_t index) const noexcept
{
    const auto found = materialDescriptors_.find(index);
    return found != materialDescriptors_.end() ? found->second : VK_NULL_HANDLE;
}

void VulkanSceneResources::destroyDescriptorPool() noexcept
{
    if (device_ != VK_NULL_HANDLE && textureDescriptorPool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device_, textureDescriptorPool_, nullptr);
    }
    textureDescriptorPool_ = VK_NULL_HANDLE;
}

void VulkanSceneResources::cleanup() noexcept
{
    destroyDescriptorPool();
    materialDescriptors_.clear();
    materials_.clear();
    for (auto& [index, meshResource] : meshes_)
    {
        (void)index;
        resourceManager_.destroy(meshResource);
    }
    meshes_.clear();
    textureKeys_.clear();
    for (auto& [key, shared] : sharedTextures_)
    {
        (void)key;
        resourceManager_.destroy(shared.resource);
    }
    sharedTextures_.clear();
    if (device_ != VK_NULL_HANDLE && textureSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device_, textureSetLayout_, nullptr);
    }
    textureSetLayout_ = VK_NULL_HANDLE;
    resourceManager_.shutdown();
    device_ = VK_NULL_HANDLE;
    uploadCommandPool_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    uploader_ = nullptr;
}

} // namespace Halcyon::Vulkan
