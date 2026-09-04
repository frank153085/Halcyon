#include "VulkanSceneResources.h"

#include <algorithm>
#include <array>
#include <limits>
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
    VkPhysicalDevice physicalDevice,
    VkCommandPool uploadCommandPool,
    VkQueue graphicsQueue,
    GpuAllocator& allocator,
    GpuUploader& uploader)
{
    cleanup();
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || uploadCommandPool == VK_NULL_HANDLE ||
        graphicsQueue == VK_NULL_HANDLE)
    {
        return resourceError(Halcyon::ErrorCode::InvalidState,
            "cannot initialize scene resources without a Vulkan device and queue");
    }
    device_ = device;
    physicalDevice_ = physicalDevice;
    uploadCommandPool_ = uploadCommandPool;
    graphicsQueue_ = graphicsQueue;
    allocator_ = &allocator;
    uploader_ = &uploader;
    resourceManager_.initialize(device_, physicalDevice_, uploadCommandPool_, graphicsQueue_, allocator, uploader);
    const auto layout = createDescriptorLayout();
    if (!layout)
    {
        cleanup();
    }
    return layout;
}

Halcyon::Result<void> VulkanSceneResources::createDescriptorLayout()
{
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (std::uint32_t i = 0; i < 5; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[5] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {30, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
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

Halcyon::Result<BufferAllocation> VulkanSceneResources::createMaterialBuffer(
    const Halcyon::Renderer::Scene::SceneMaterial& material)
{
    if (allocator_ == nullptr)
    {
        return Halcyon::Result<BufferAllocation>::failure(
            {Halcyon::ErrorCode::InvalidState, "scene allocator is unavailable"});
    }
    MaterialGpuData data{};
    data.baseColorFactor = {material.pbr.baseColor.r, material.pbr.baseColor.g,
        material.pbr.baseColor.b, material.pbr.baseColor.a};
    data.emissiveFactor = {material.pbr.emissive.r, material.pbr.emissive.g,
        material.pbr.emissive.b, std::clamp(material.pbr.ambientOcclusion, 0.0f, 1.0f)};
    data.factors = {std::clamp(material.pbr.metallic, 0.0f, 1.0f),
        std::clamp(material.pbr.roughness, 0.0f, 1.0f),
        std::clamp(material.alphaCutoff, 0.0f, 1.0f),
        static_cast<float>((material.transparent ? 1u : 0u) |
                           (material.doubleSided ? 2u : 0u) |
                           (material.alphaMasked ? 4u : 0u))};
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = sizeof(MaterialGpuData);
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto allocation = allocator_->createBuffer(info, MemoryUsage::CpuToGpu);
    if (!allocation)
        return allocation;
    const auto write = allocator_->writeBuffer(allocation.value(),
        std::as_bytes(std::span<const MaterialGpuData>{&data, 1}));
    if (!write)
    {
        allocator_->destroy(allocation.value());
        return Halcyon::Result<BufferAllocation>::failure(write.error());
    }
    return allocation;
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
            const auto dense = textureDenseByStable_.find(index);
            if (dense != textureDenseByStable_.end())
            {
                freeTextureDense_.push_back(dense->second);
                if (dense->second < denseTextureStable_.size())
                    denseTextureStable_[dense->second] = std::numeric_limits<std::uint32_t>::max();
                textureDenseByStable_.erase(dense);
            }
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
            const auto materialIt = materials_.find(material);
            if (materialIt != materials_.end() && allocator_ != nullptr)
            {
                allocator_->destroy(materialIt->second.factorsBuffer);
            }
            materials_.erase(material);
            const auto dense = materialDenseByStable_.find(material);
            if (dense != materialDenseByStable_.end())
            {
                freeMaterialDense_.push_back(dense->second);
                if (dense->second < denseMaterialStable_.size())
                    denseMaterialStable_[dense->second] = std::numeric_limits<std::uint32_t>::max();
                materialDenseByStable_.erase(dense);
            }
        }
        for (const std::uint32_t mesh : uploadedMeshes)
        {
            const auto found = meshes_.find(mesh);
            if (found != meshes_.end())
            {
                resourceManager_.destroy(found->second);
                meshes_.erase(found);
            }
            const auto dense = meshDenseByStable_.find(mesh);
            if (dense != meshDenseByStable_.end())
            {
                freeMeshDense_.push_back(dense->second);
                if (dense->second < denseMeshStable_.size())
                    denseMeshStable_[dense->second] = std::numeric_limits<std::uint32_t>::max();
                meshDenseByStable_.erase(dense);
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
                const std::uint32_t dense = freeTextureDense_.empty()
                    ? static_cast<std::uint32_t>(denseTextureStable_.size())
                    : freeTextureDense_.back();
                if (!freeTextureDense_.empty())
                    freeTextureDense_.pop_back();
                if (dense == denseTextureStable_.size())
                    denseTextureStable_.push_back(index);
                else
                    denseTextureStable_[dense] = index;
                textureDenseByStable_.emplace(index, dense);
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
            const std::uint32_t dense = freeMeshDense_.empty()
                ? static_cast<std::uint32_t>(denseMeshStable_.size())
                : freeMeshDense_.back();
            if (!freeMeshDense_.empty())
                freeMeshDense_.pop_back();
            if (dense == denseMeshStable_.size())
                denseMeshStable_.push_back(handle.index());
            else
                denseMeshStable_[dense] = handle.index();
            meshDenseByStable_.emplace(handle.index(), dense);
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
            !source->normalTexture.isValid() ||
            !source->metallicRoughnessTexture.isValid() ||
            !source->emissiveTexture.isValid() ||
            !source->occlusionTexture.isValid() ||
            texture(source->baseColorTexture.index()) == nullptr ||
            texture(source->normalTexture.index()) == nullptr ||
            texture(source->metallicRoughnessTexture.index()) == nullptr ||
            texture(source->emissiveTexture.index()) == nullptr ||
            texture(source->occlusionTexture.index()) == nullptr)
        {
            rollback();
            return resourceError(Halcyon::ErrorCode::InvalidArgument,
                "scene material references an unavailable base-color texture");
        }
        const auto factorsBuffer = createMaterialBuffer(*source);
        if (!factorsBuffer)
        {
            rollback();
            return Halcyon::Result<void>::failure(factorsBuffer.error());
        }
        try
        {
            materials_.emplace(handle.index(), MaterialResource{
                source->baseColorTexture.index(),
                source->normalTexture.index(),
                source->metallicRoughnessTexture.index(),
                source->emissiveTexture.index(),
                source->occlusionTexture.index(),
                factorsBuffer.value()});
            uploadedMaterials.push_back(handle.index());
            const std::uint32_t dense = freeMaterialDense_.empty()
                ? static_cast<std::uint32_t>(denseMaterialStable_.size())
                : freeMaterialDense_.back();
            if (!freeMaterialDense_.empty())
                freeMaterialDense_.pop_back();
            if (dense == denseMaterialStable_.size())
                denseMaterialStable_.push_back(handle.index());
            else
                denseMaterialStable_[dense] = handle.index();
            materialDenseByStable_.emplace(handle.index(), dense);
        }
        catch (...)
        {
            BufferAllocation buffer = factorsBuffer.value();
            if (allocator_ != nullptr)
                allocator_->destroy(buffer);
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
        const auto materialIt = materials_.find(handle.index());
        if (materialIt != materials_.end() && allocator_ != nullptr)
            allocator_->destroy(materialIt->second.factorsBuffer);
        materials_.erase(handle.index());
        const auto dense = materialDenseByStable_.find(handle.index());
        if (dense != materialDenseByStable_.end())
        {
            freeMaterialDense_.push_back(dense->second);
            if (dense->second < denseMaterialStable_.size())
                denseMaterialStable_[dense->second] = std::numeric_limits<std::uint32_t>::max();
            materialDenseByStable_.erase(dense);
        }
    }
    for (const Halcyon::Renderer::Resources::MeshHandle handle : imported.meshes)
    {
        const auto found = meshes_.find(handle.index());
        if (found != meshes_.end())
        {
            resourceManager_.destroy(found->second);
            meshes_.erase(found);
        }
        const auto dense = meshDenseByStable_.find(handle.index());
        if (dense != meshDenseByStable_.end())
        {
            freeMeshDense_.push_back(dense->second);
            if (dense->second < denseMeshStable_.size())
                denseMeshStable_[dense->second] = std::numeric_limits<std::uint32_t>::max();
            meshDenseByStable_.erase(dense);
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

    std::array<VkDescriptorPoolSize, 3> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            static_cast<std::uint32_t>(materials_.size() * 5u)},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER,
            static_cast<std::uint32_t>(materials_.size())},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            static_cast<std::uint32_t>(materials_.size())}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<std::uint32_t>(materials_.size());
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
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
        const std::array<const TextureResource*, 5> sources = {
            texture(material.baseColorTexture),
            texture(material.normalTexture),
            texture(material.metallicRoughnessTexture),
            texture(material.emissiveTexture),
            texture(material.occlusionTexture)};
        if (std::any_of(sources.begin(), sources.end(), [](const TextureResource* value) {
                return value == nullptr;
            }))
        {
            vkDestroyDescriptorPool(device_, candidatePool, nullptr);
            return resourceError(Halcyon::ErrorCode::InvalidState,
                "scene material texture disappeared during descriptor rebuild");
        }
        std::array<VkDescriptorImageInfo, 5> images{};
        for (std::size_t binding = 0; binding < images.size(); ++binding)
        {
            images[binding] = {VK_NULL_HANDLE, sources[binding]->view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (std::uint32_t binding = 0; binding < 5; ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = sets[i];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[binding].pImageInfo = &images[binding];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        VkDescriptorImageInfo sampler{sources[0]->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet samplerWrite{};
        samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        samplerWrite.dstSet = sets[i];
        samplerWrite.dstBinding = 10;
        samplerWrite.descriptorCount = 1;
        samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        samplerWrite.pImageInfo = &sampler;
        vkUpdateDescriptorSets(device_, 1, &samplerWrite, 0, nullptr);
        VkDescriptorBufferInfo factorsInfo{material.factorsBuffer.buffer, 0,
            sizeof(MaterialGpuData)};
        VkWriteDescriptorSet factorsWrite{};
        factorsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        factorsWrite.dstSet = sets[i];
        factorsWrite.dstBinding = 30;
        factorsWrite.descriptorCount = 1;
        factorsWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        factorsWrite.pBufferInfo = &factorsInfo;
        vkUpdateDescriptorSets(device_, 1, &factorsWrite, 0, nullptr);
        const auto dense = materialDenseByStable_.find(materialIndices[i]);
        if (dense == materialDenseByStable_.end())
        {
            vkDestroyDescriptorPool(device_, candidatePool, nullptr);
            return resourceError(Halcyon::ErrorCode::InvalidState,
                "scene material is missing a dense GPU index");
        }
        candidateDescriptors.emplace(dense->second, sets[i]);
    }
    destroyDescriptorPool();
    textureDescriptorPool_ = candidatePool;
    materialDescriptors_ = std::move(candidateDescriptors);
    return Halcyon::Result<void>::success();
}

const MeshResource* VulkanSceneResources::mesh(std::uint32_t index) const noexcept
{
    if (index >= denseMeshStable_.size())
        return nullptr;
    const std::uint32_t stable = denseMeshStable_[index];
    if (stable == std::numeric_limits<std::uint32_t>::max())
        return nullptr;
    const auto found = meshes_.find(stable);
    return found != meshes_.end() ? &found->second : nullptr;
}

VkDescriptorSet VulkanSceneResources::materialDescriptor(std::uint32_t index) const noexcept
{
    const auto found = materialDescriptors_.find(index);
    return found != materialDescriptors_.end() ? found->second : VK_NULL_HANDLE;
}

std::uint32_t VulkanSceneResources::meshDenseIndex(std::uint32_t stableIndex) const noexcept
{
    const auto found = meshDenseByStable_.find(stableIndex);
    return found != meshDenseByStable_.end() ? found->second : std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t VulkanSceneResources::materialDenseIndex(std::uint32_t stableIndex) const noexcept
{
    const auto found = materialDenseByStable_.find(stableIndex);
    return found != materialDenseByStable_.end() ? found->second : std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t VulkanSceneResources::textureDenseIndex(std::uint32_t stableIndex) const noexcept
{
    const auto found = textureDenseByStable_.find(stableIndex);
    return found != textureDenseByStable_.end() ? found->second : std::numeric_limits<std::uint32_t>::max();
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
    for (auto& [index, material] : materials_)
    {
        (void)index;
        if (allocator_ != nullptr)
            allocator_->destroy(material.factorsBuffer);
    }
    materials_.clear();
    for (auto& [index, meshResource] : meshes_)
    {
        (void)index;
        resourceManager_.destroy(meshResource);
    }
    meshes_.clear();
    meshDenseByStable_.clear();
    materialDenseByStable_.clear();
    denseMeshStable_.clear();
    denseMaterialStable_.clear();
    freeMeshDense_.clear();
    freeMaterialDense_.clear();
    textureKeys_.clear();
    textureDenseByStable_.clear();
    denseTextureStable_.clear();
    freeTextureDense_.clear();
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
    physicalDevice_ = VK_NULL_HANDLE;
    uploadCommandPool_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    uploader_ = nullptr;
}

} // namespace Halcyon::Vulkan
