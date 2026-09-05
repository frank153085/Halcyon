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
    GpuUploader& uploader,
    bool enableGpuDrivenMeshes)
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
    gpuDrivenMeshesEnabled_ = enableGpuDrivenMeshes;
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
        Halcyon::Renderer::Scene::MaterialGpuData bindlessRow{};
        bindlessRow.baseColorFactor = {source->pbr.baseColor.r, source->pbr.baseColor.g,
            source->pbr.baseColor.b, source->pbr.baseColor.a};
        bindlessRow.emissiveFactor = {source->pbr.emissive.r, source->pbr.emissive.g,
            source->pbr.emissive.b, std::clamp(source->pbr.ambientOcclusion, 0.0f, 1.0f)};
        bindlessRow.factors = {std::clamp(source->pbr.metallic, 0.0f, 1.0f),
            std::clamp(source->pbr.roughness, 0.0f, 1.0f),
            std::clamp(source->alphaCutoff, 0.0f, 1.0f),
            static_cast<float>((source->transparent ? 1u : 0u) |
                (source->doubleSided ? 2u : 0u) | (source->alphaMasked ? 4u : 0u))};
        const std::array<std::uint32_t, 5> textureStable = {
            source->baseColorTexture.index(), source->normalTexture.index(),
            source->metallicRoughnessTexture.index(), source->emissiveTexture.index(),
            source->occlusionTexture.index()};
        for (std::size_t textureIndex = 0; textureIndex < textureStable.size(); ++textureIndex)
            bindlessRow.textureIndices[textureIndex] = textureDenseIndex(textureStable[textureIndex]);
        try
        {
            materials_.emplace(handle.index(), MaterialResource{
                source->baseColorTexture.index(),
                source->normalTexture.index(),
                source->metallicRoughnessTexture.index(),
                source->emissiveTexture.index(),
                source->occlusionTexture.index(),
                factorsBuffer.value(), bindlessRow});
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
    const auto gpuDrivenMeshes = rebuildGpuDrivenMeshes();
    if (!gpuDrivenMeshes)
    {
        rollback();
        return gpuDrivenMeshes;
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
    auto result = rebuildGpuDrivenMeshes();
    if (!result) return result;
    return rebuildMaterialDescriptors();
}

Halcyon::Result<void> VulkanSceneResources::rebuildGpuDrivenMeshes()
{
    if (!gpuDrivenMeshesEnabled_) return Halcyon::Result<void>::success();
    if (allocator_ == nullptr || uploader_ == nullptr || device_ == VK_NULL_HANDLE ||
        uploadCommandPool_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE)
        return resourceError(Halcyon::ErrorCode::InvalidState,
            "GPU-driven mesh storage is not initialized");

    VkDeviceSize vertexBytes = 0;
    VkDeviceSize indexBytes = 0;
    std::vector<Halcyon::Renderer::Scene::MeshDrawRow> rows(denseMeshStable_.size());
    for (std::size_t dense = 0; dense < denseMeshStable_.size(); ++dense)
    {
        const auto found = meshes_.find(denseMeshStable_[dense]);
        if (found == meshes_.end()) continue;
        if (vertexBytes / sizeof(MeshVertex) >
                static_cast<VkDeviceSize>(std::numeric_limits<std::int32_t>::max()) ||
            indexBytes / sizeof(std::uint32_t) >
                static_cast<VkDeviceSize>(std::numeric_limits<std::uint32_t>::max()))
            return resourceError(Halcyon::ErrorCode::OutOfMemory,
                "consolidated GPU mesh stream exceeds indirect draw limits");
        rows[dense] = {found->second.indexCount,
            static_cast<std::uint32_t>(indexBytes / sizeof(std::uint32_t)),
            static_cast<std::int32_t>(vertexBytes / sizeof(MeshVertex)), 0u};
        vertexBytes += found->second.vertexBuffer.size;
        indexBytes += found->second.indexBuffer.size;
    }

    if (vertexBytes == 0 || indexBytes == 0 || rows.empty())
    {
        allocator_->destroy(gpuDrivenVertices_);
        allocator_->destroy(gpuDrivenIndices_);
        allocator_->destroy(meshDraws_);
        gpuDrivenVertices_ = {};
        gpuDrivenIndices_ = {};
        meshDraws_ = {};
        meshDrawRows_.clear();
        return Halcyon::Result<void>::success();
    }

    const auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return allocator_->createBuffer(info, MemoryUsage::GpuOnly);
    };
    auto vertices = createBuffer(vertexBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!vertices) return Halcyon::Result<void>::failure(vertices.error());
    auto indices = createBuffer(indexBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!indices)
    {
        allocator_->destroy(vertices.value());
        return Halcyon::Result<void>::failure(indices.error());
    }
    auto draws = createBuffer(rows.size() * sizeof(rows[0]),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!draws)
    {
        allocator_->destroy(vertices.value());
        allocator_->destroy(indices.value());
        return Halcyon::Result<void>::failure(draws.error());
    }
    const auto destroyCandidates = [&]() noexcept
    {
        allocator_->destroy(vertices.value());
        allocator_->destroy(indices.value());
        allocator_->destroy(draws.value());
    };

    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = uploadCommandPool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkResult vkResult = vkAllocateCommandBuffers(device_, &allocation, &commandBuffer);
    if (vkResult != VK_SUCCESS)
    {
        destroyCandidates();
        return resourceError(Halcyon::ErrorCode::Backend,
            "failed to allocate consolidated mesh copy command");
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResult = vkBeginCommandBuffer(commandBuffer, &begin);
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset = 0;
    if (vkResult == VK_SUCCESS)
    {
        for (const std::uint32_t stable : denseMeshStable_)
        {
            const auto found = meshes_.find(stable);
            if (found == meshes_.end()) continue;
            const VkBufferCopy vertexCopy{0, vertexOffset, found->second.vertexBuffer.size};
            vkCmdCopyBuffer(commandBuffer, found->second.vertexBuffer.buffer,
                vertices.value().buffer, 1, &vertexCopy);
            const VkBufferCopy indexCopy{0, indexOffset, found->second.indexBuffer.size};
            vkCmdCopyBuffer(commandBuffer, found->second.indexBuffer.buffer,
                indices.value().buffer, 1, &indexCopy);
            vertexOffset += found->second.vertexBuffer.size;
            indexOffset += found->second.indexBuffer.size;
        }
        std::array<VkBufferMemoryBarrier2, 2> ready{};
        for (auto& barrier : ready)
        {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        ready[0].buffer = vertices.value().buffer;
        ready[0].dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        ready[1].buffer = indices.value().buffer;
        ready[1].dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(ready.size());
        dependency.pBufferMemoryBarriers = ready.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        vkResult = vkEndCommandBuffer(commandBuffer);
    }
    if (vkResult == VK_SUCCESS)
    {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        vkResult = vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
        if (vkResult == VK_SUCCESS) vkResult = vkQueueWaitIdle(graphicsQueue_);
    }
    vkFreeCommandBuffers(device_, uploadCommandPool_, 1, &commandBuffer);
    if (vkResult != VK_SUCCESS)
    {
        destroyCandidates();
        return resourceError(Halcyon::ErrorCode::Backend,
            "failed to build consolidated GPU mesh stream");
    }
    const auto uploadRows = uploader_->uploadBuffer(device_, uploadCommandPool_, graphicsQueue_,
        *allocator_, draws.value(), std::as_bytes(std::span{rows}));
    if (!uploadRows)
    {
        destroyCandidates();
        return uploadRows;
    }

    allocator_->destroy(gpuDrivenVertices_);
    allocator_->destroy(gpuDrivenIndices_);
    allocator_->destroy(meshDraws_);
    gpuDrivenVertices_ = vertices.value();
    gpuDrivenIndices_ = indices.value();
    meshDraws_ = draws.value();
    meshDrawRows_ = std::move(rows);
    return Halcyon::Result<void>::success();
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

Halcyon::Renderer::Scene::MaterialGpuData VulkanSceneResources::materialRow(
    std::uint32_t denseIndex) const noexcept
{
    if (denseIndex >= denseMaterialStable_.size()) return {};
    const auto found = materials_.find(denseMaterialStable_[denseIndex]);
    return found != materials_.end() ? found->second.bindlessRow
                                     : Halcyon::Renderer::Scene::MaterialGpuData{};
}

const TextureResource* VulkanSceneResources::textureDense(
    std::uint32_t denseIndex) const noexcept
{
    if (denseIndex >= denseTextureStable_.size()) return nullptr;
    const auto stable = denseTextureStable_[denseIndex];
    return stable == std::numeric_limits<std::uint32_t>::max() ? nullptr : texture(stable);
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
    if (allocator_ != nullptr)
    {
        allocator_->destroy(gpuDrivenVertices_);
        allocator_->destroy(gpuDrivenIndices_);
        allocator_->destroy(meshDraws_);
    }
    gpuDrivenVertices_ = {};
    gpuDrivenIndices_ = {};
    meshDraws_ = {};
    meshDrawRows_.clear();
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
    gpuDrivenMeshesEnabled_ = false;
}

} // namespace Halcyon::Vulkan
