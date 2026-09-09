#include "VulkanSceneResources.h"

#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
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

Halcyon::Result<VulkanSceneResources::VirtualGeometryGpuBuffers>
VulkanSceneResources::uploadVirtualGeometry(
    const Halcyon::Renderer::Scene::VirtualGeometryAsset& asset)
{
    if (allocator_ == nullptr || uploader_ == nullptr)
        return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
            {Halcyon::ErrorCode::InvalidState, "virtual geometry uploader is unavailable"});
    // Cache readers validate these ranges, but upload is also reachable from
    // programmatic SceneDatabase users. Recheck the GPU-facing spans here so
    // a malformed asset cannot create descriptors whose shader-visible range
    // disagrees with its meshlet metadata.
    if (asset.vertices.empty() || asset.meshlets.empty() || asset.lods.empty() ||
        asset.clusters.empty() || asset.dagNodes.empty() ||
        (asset.dagNodes.size() > 1u && asset.dagEdges.empty()) ||
        asset.meshlets.size() > (1u << 20u) - 1u)
    {
        return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
            {Halcyon::ErrorCode::InvalidArgument,
                "virtual geometry asset has no uploadable meshlet tables"});
    }
    const auto inRange = [](std::uint32_t offset, std::uint32_t count,
        std::size_t size) noexcept
    {
        return static_cast<std::size_t>(offset) <= size &&
            static_cast<std::size_t>(count) <= size - offset;
    };
    for (const auto& meshlet : asset.meshlets)
    {
        if (meshlet.vertexCount == 0u || meshlet.vertexCount > 64u ||
            meshlet.triangleCount == 0u || meshlet.triangleCount > 124u ||
            meshlet.indexCount != meshlet.triangleCount * 3u ||
            !inRange(meshlet.vertexOffset, meshlet.vertexCount, asset.meshletVertices.size()) ||
            !inRange(meshlet.triangleOffset, meshlet.indexCount, asset.meshletTriangles.size()) ||
            !inRange(meshlet.indexOffset, meshlet.indexCount, asset.indices.size()))
        {
            return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "virtual geometry meshlet range is outside its upload table"});
        }
        for (std::uint32_t local = 0; local < meshlet.vertexCount; ++local)
        {
            if (asset.meshletVertices[meshlet.vertexOffset + local] >= asset.vertices.size())
                return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
                    {Halcyon::ErrorCode::InvalidArgument,
                        "virtual geometry meshlet vertex references outside the vertex table"});
        }
        for (std::uint32_t index = 0; index < meshlet.indexCount; ++index)
        {
            const auto local = asset.meshletTriangles[meshlet.triangleOffset + index];
            if (local >= meshlet.vertexCount ||
                asset.indices[meshlet.indexOffset + index] !=
                    asset.meshletVertices[meshlet.vertexOffset + local])
                return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
                    {Halcyon::ErrorCode::InvalidArgument,
                        "virtual geometry meshlet topology is inconsistent"});
        }
    }
    VirtualGeometryGpuBuffers result{};
    const auto createAndUpload = [&](const void* data, std::size_t size,
        VkBufferUsageFlags usage, BufferAllocation& destination) -> Halcyon::Result<void>
    {
        if (size == 0) return Halcyon::Result<void>::success();
        if (data == nullptr || size > std::numeric_limits<VkDeviceSize>::max())
            return Halcyon::Result<void>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "virtual geometry buffer size is outside the Vulkan device range"});
        VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size; info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const auto allocation = allocator_->createBuffer(info, MemoryUsage::GpuOnly);
        if (!allocation) return Halcyon::Result<void>::failure(allocation.error());
        const auto upload = uploader_->uploadBuffer(device_, uploadCommandPool_, graphicsQueue_,
            *allocator_, allocation.value(), std::span<const std::byte>{
                static_cast<const std::byte*>(data), size});
        if (!upload) { allocator_->destroy(allocation.value()); return upload; }
        destination = allocation.value(); return Halcyon::Result<void>::success();
    };
    const auto fail = [&](Halcyon::Result<void> error) {
        destroyVirtualGeometry(result); return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(error.error());
    };
    const auto byteSize = [](std::size_t count, std::size_t elementSize,
                             std::size_t& output) noexcept
    {
        if (elementSize != 0u && count > std::numeric_limits<std::size_t>::max() / elementSize)
            return false;
        output = count * elementSize;
        return true;
    };
    std::vector<VirtualGeometryGpuMeshlet> gpuMeshlets;
    try
    {
        gpuMeshlets.reserve(asset.meshlets.size());
        for (const auto& source : asset.meshlets)
        {
            VirtualGeometryGpuMeshlet destination{};
            destination.vertexOffset = source.vertexOffset;
            destination.vertexCount = source.vertexCount;
            destination.triangleOffset = source.triangleOffset;
            destination.triangleCount = source.triangleCount;
            destination.indexOffset = source.indexOffset;
            destination.indexCount = source.indexCount;
            destination.primitiveIndex = source.primitiveIndex;
            destination.lodIndex = source.lodIndex;
            destination.sphere = source.sphere;
            destination.cone = source.cone;
            destination.geometricError = source.geometricError;
            gpuMeshlets.push_back(destination);
        }
    }
    catch (...)
    {
        return Halcyon::Result<VirtualGeometryGpuBuffers>::failure(
            {Halcyon::ErrorCode::OutOfMemory, "failed to pack virtual geometry meshlets"});
    }
    std::size_t bytes = 0;
    if (!byteSize(asset.vertices.size(), sizeof(asset.vertices[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry vertex byte size overflow"}));
    auto upload = createAndUpload(asset.vertices.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, result.vertices);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(asset.indices.size(), sizeof(asset.indices[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry index byte size overflow"}));
    upload = createAndUpload(asset.indices.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, result.indices);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(asset.meshletVertices.size(), sizeof(asset.meshletVertices[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry meshlet vertex byte size overflow"}));
    upload = createAndUpload(asset.meshletVertices.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.meshletVertices);
    if (!upload) return fail(std::move(upload));
    // material_classify reads the packed meshoptimizer triangle stream through
    // ByteAddressBuffer.Load (4-byte words). Pad the device copy to a word
    // boundary so the final byte can be loaded without crossing the Vulkan
    // descriptor range; the cache/CPU representation remains byte-packed.
    if (asset.meshletTriangles.size() > std::numeric_limits<std::size_t>::max() - 3u)
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry meshlet triangle byte size overflow"}));
    const std::size_t paddedTriangleBytes =
        (asset.meshletTriangles.size() + 3u) & ~std::size_t(3u);
    std::vector<std::uint8_t> paddedTriangles;
    try
    {
        paddedTriangles.assign(paddedTriangleBytes, 0u);
        std::copy(asset.meshletTriangles.begin(), asset.meshletTriangles.end(), paddedTriangles.begin());
    }
    catch (const std::bad_alloc&)
    {
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "failed to pad virtual geometry meshlet triangle stream"}));
    }
    if (!byteSize(paddedTriangles.size(), sizeof(paddedTriangles[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry meshlet triangle byte size overflow"}));
    upload = createAndUpload(paddedTriangles.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.meshletTriangles);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(gpuMeshlets.size(), sizeof(gpuMeshlets[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry meshlet metadata byte size overflow"}));
    upload = createAndUpload(gpuMeshlets.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.meshlets);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(asset.lods.size(), sizeof(asset.lods[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry LOD byte size overflow"}));
    upload = createAndUpload(asset.lods.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.lods);
    if (!upload) return fail(std::move(upload));
    std::vector<VirtualGeometryGpuCluster> gpuClusters;
    std::vector<VirtualGeometryGpuDagNode> gpuDagNodes;
    std::vector<std::uint32_t> boundaryVertices;
    std::vector<std::uint32_t> clusterMeshletIndices;
    std::vector<std::uint32_t> meshletDagNodes;
    std::vector<std::uint32_t> clusterAdjacencyOffsets;
    std::vector<std::uint32_t> clusterAdjacencyIndices;
    try
    {
        meshletDagNodes.assign(asset.meshlets.size(), std::numeric_limits<std::uint32_t>::max());
        clusterAdjacencyOffsets.reserve(asset.clusters.size() + 1u);
        clusterAdjacencyOffsets.push_back(0u);
        for (const auto& cluster : asset.clusters)
        {
            VirtualGeometryGpuCluster packed{};
            packed.meshletOffset = static_cast<std::uint32_t>(clusterMeshletIndices.size());
            packed.meshletCount = cluster.meshletCount;
            packed.vertexOffset = cluster.vertexOffset;
            packed.vertexCount = cluster.vertexCount;
            packed.triangleCount = cluster.triangleCount;
            packed.lodDepth = cluster.lodDepth;
            packed.primitiveIndex = cluster.primitiveIndex;
            packed.sphere = cluster.sphere;
            packed.geometricError = cluster.geometricError;
            gpuClusters.push_back(packed);
            clusterMeshletIndices.insert(clusterMeshletIndices.end(),
                cluster.meshletIndices.begin(), cluster.meshletIndices.end());
            boundaryVertices.insert(boundaryVertices.end(), cluster.boundaryVertices.begin(),
                cluster.boundaryVertices.end());
            if (clusterAdjacencyIndices.size() >
                std::numeric_limits<std::uint32_t>::max() - cluster.adjacentClusters.size())
                return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
                    "virtual geometry adjacency table exceeds the GPU integer range"}));
            clusterAdjacencyIndices.insert(clusterAdjacencyIndices.end(),
                cluster.adjacentClusters.begin(), cluster.adjacentClusters.end());
            clusterAdjacencyOffsets.push_back(
                static_cast<std::uint32_t>(clusterAdjacencyIndices.size()));
        }
        for (const auto& node : asset.dagNodes)
        {
            VirtualGeometryGpuDagNode packed{};
            packed.clusterIndex = node.clusterIndex;
            packed.parentIndex = node.parentIndex;
            packed.firstChild = node.firstChild;
            packed.childCount = node.childCount;
            packed.lodDepth = node.lodDepth;
            packed.flags = node.flags;
            packed.sphere = node.sphere;
            packed.geometricError = node.geometricError;
            gpuDagNodes.push_back(packed);
        }
        for (std::uint32_t nodeIndex = 0; nodeIndex < asset.dagNodes.size(); ++nodeIndex)
        {
            const auto clusterIndex = asset.dagNodes[nodeIndex].clusterIndex;
            if (clusterIndex >= asset.clusters.size())
                return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
                    "virtual geometry DAG cluster reference is outside the cluster table"}));
            for (const auto meshletIndex : asset.clusters[clusterIndex].meshletIndices)
            {
                if (meshletIndex >= meshletDagNodes.size() ||
                    meshletDagNodes[meshletIndex] != std::numeric_limits<std::uint32_t>::max())
                    return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
                        "virtual geometry meshlet belongs to multiple DAG nodes"}));
                meshletDagNodes[meshletIndex] = nodeIndex;
            }
        }
        for (const auto nodeIndex : meshletDagNodes)
            if (nodeIndex == std::numeric_limits<std::uint32_t>::max())
                return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
                    "virtual geometry meshlet has no DAG node mapping"}));
    }
    catch (const std::bad_alloc&)
    {
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "failed to pack virtual geometry M6 tables"}));
    }
    if (!byteSize(gpuClusters.size(), sizeof(gpuClusters[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry cluster byte size overflow"}));
    upload = createAndUpload(gpuClusters.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.clusters);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(gpuDagNodes.size(), sizeof(gpuDagNodes[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry DAG node byte size overflow"}));
    upload = createAndUpload(gpuDagNodes.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.dagNodes);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(asset.dagEdges.size(), sizeof(asset.dagEdges[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry DAG edge byte size overflow"}));
    Halcyon::Renderer::Scene::VirtualGeometryDagEdge dummyEdge{};
    const void* dagEdgeData = asset.dagEdges.empty() ? static_cast<const void*>(&dummyEdge)
                                                     : static_cast<const void*>(asset.dagEdges.data());
    const std::size_t dagEdgeBytes = asset.dagEdges.empty() ? sizeof(dummyEdge) : bytes;
    upload = createAndUpload(dagEdgeData, dagEdgeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.dagEdges);
    if (!upload) return fail(std::move(upload));
    // One persistent hysteresis state per DAG node. A zero current decision
    // starts every node coarse; roots therefore form the initial frontier.
    std::vector<std::array<std::uint32_t, 4>> initialLodStates(asset.dagNodes.size());
    upload = createAndUpload(initialLodStates.data(),
        initialLodStates.size() * sizeof(initialLodStates[0]),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.lodStates);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(boundaryVertices.size(), sizeof(boundaryVertices[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry boundary byte size overflow"}));
    upload = createAndUpload(boundaryVertices.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.boundaryVertices);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(clusterMeshletIndices.size(), sizeof(clusterMeshletIndices[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry cluster index byte size overflow"}));
    upload = createAndUpload(clusterMeshletIndices.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.clusterMeshletIndices);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(meshletDagNodes.size(), sizeof(meshletDagNodes[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry meshlet DAG mapping byte size overflow"}));
    upload = createAndUpload(meshletDagNodes.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        result.meshletDagNodes);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(clusterAdjacencyOffsets.size(), sizeof(clusterAdjacencyOffsets[0]), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry adjacency offset byte size overflow"}));
    upload = createAndUpload(clusterAdjacencyOffsets.data(), bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.clusterAdjacencyOffsets);
    if (!upload) return fail(std::move(upload));
    if (!byteSize(clusterAdjacencyIndices.size(), sizeof(std::uint32_t), bytes))
        return fail(Halcyon::Result<void>::failure({Halcyon::ErrorCode::InvalidArgument,
            "virtual geometry adjacency index byte size overflow"}));
    const std::uint32_t emptyAdjacency = 0u;
    upload = createAndUpload(clusterAdjacencyIndices.empty()
            ? static_cast<const void*>(&emptyAdjacency)
            : static_cast<const void*>(clusterAdjacencyIndices.data()),
        clusterAdjacencyIndices.empty() ? sizeof(emptyAdjacency) : bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, result.clusterAdjacencyIndices);
    if (!upload) return fail(std::move(upload));
    return Halcyon::Result<VirtualGeometryGpuBuffers>::success(std::move(result));
}

void VulkanSceneResources::destroyVirtualGeometry(VirtualGeometryGpuBuffers& buffers) noexcept
{
    if (allocator_ == nullptr) return;
    allocator_->destroy(buffers.vertices); allocator_->destroy(buffers.indices);
    allocator_->destroy(buffers.meshletVertices); allocator_->destroy(buffers.meshletTriangles);
    allocator_->destroy(buffers.meshlets); allocator_->destroy(buffers.lods);
    allocator_->destroy(buffers.clusters); allocator_->destroy(buffers.dagNodes);
    allocator_->destroy(buffers.dagEdges); allocator_->destroy(buffers.boundaryVertices);
    allocator_->destroy(buffers.lodStates);
    allocator_->destroy(buffers.clusterMeshletIndices);
    allocator_->destroy(buffers.meshletDagNodes);
    allocator_->destroy(buffers.clusterAdjacencyOffsets);
    allocator_->destroy(buffers.clusterAdjacencyIndices);
    buffers = {};
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
            virtualGeometryByMesh_.erase(mesh);
            const auto virtualGpu = virtualGeometryGpuByMesh_.find(mesh);
            if (virtualGpu != virtualGeometryGpuByMesh_.end())
            {
                destroyVirtualGeometry(virtualGpu->second);
                virtualGeometryGpuByMesh_.erase(virtualGpu);
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
            if (source->virtualGeometry)
            {
                const auto virtualGpu = uploadVirtualGeometry(*source->virtualGeometry);
                if (virtualGpu)
                {
                    // BufferAllocation is an explicit handle, not an RAII
                    // owner. Keep the freshly uploaded set locally until both
                    // lookup tables have accepted it so a map allocation
                    // failure cannot leak Lucy-sized GPU allocations.
                    VirtualGeometryGpuBuffers pending = virtualGpu.value();
                    bool indexed = false;
                    try
                    {
                        const auto [assetIt, assetInserted] =
                            virtualGeometryByMesh_.emplace(
                                handle.index(), source->virtualGeometry);
                        if (assetInserted)
                        {
                            const auto [gpuIt, gpuInserted] =
                                virtualGeometryGpuByMesh_.emplace(handle.index(), pending);
                            (void)gpuIt;
                            if (gpuInserted)
                            {
                                pending = {};
                                indexed = true;
                            }
                            else
                            {
                                virtualGeometryByMesh_.erase(assetIt);
                            }
                        }
                    }
                    catch (...)
                    {
                        virtualGeometryByMesh_.erase(handle.index());
                    }
                    if (!indexed)
                    {
                        destroyVirtualGeometry(pending);
                        rollback();
                        return resourceError(Halcyon::ErrorCode::OutOfMemory,
                            "failed to index uploaded virtual geometry");
                    }
                }
                else
                {
                    // The traditional mesh upload above is sufficient for
                    // DeferredIndexed/GpuDrivenIndexed. Treat virtual buffer
                    // allocation or upload failure as an M5 capability miss
                    // instead of discarding an otherwise valid scene asset.
                    HALCYON_LOG_WARN("Virtual Geometry upload unavailable for mesh ",
                        handle.index(), ": ", virtualGpu.error().describe(),
                        "; retaining indexed fallback");
                }
            }
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
        const auto generatedDefault = [&](auto textureHandle)
        {
            const auto* textureSource = database.get(textureHandle);
            return textureSource != nullptr && textureSource->generatedDefault;
        };
        const bool virtualGeometryCompatible = !source->transparent &&
            !source->doubleSided && !source->alphaMasked &&
            generatedDefault(source->baseColorTexture) &&
            generatedDefault(source->normalTexture) &&
            generatedDefault(source->metallicRoughnessTexture) &&
            generatedDefault(source->emissiveTexture) &&
            generatedDefault(source->occlusionTexture);
        try
        {
            materials_.emplace(handle.index(), MaterialResource{
                source->baseColorTexture.index(),
                source->normalTexture.index(),
                source->metallicRoughnessTexture.index(),
                source->emissiveTexture.index(),
                source->occlusionTexture.index(),
                factorsBuffer.value(), bindlessRow, virtualGeometryCompatible});
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
        virtualGeometryByMesh_.erase(handle.index());
        const auto virtualGpu = virtualGeometryGpuByMesh_.find(handle.index());
        if (virtualGpu != virtualGeometryGpuByMesh_.end())
        {
            destroyVirtualGeometry(virtualGpu->second);
            virtualGeometryGpuByMesh_.erase(virtualGpu);
        }
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

bool VulkanSceneResources::virtualGeometryMaterialCompatible(
    std::uint32_t denseIndex) const noexcept
{
    if (denseIndex >= denseMaterialStable_.size()) return false;
    const auto found = materials_.find(denseMaterialStable_[denseIndex]);
    return found != materials_.end() && found->second.virtualGeometryCompatible;
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
        for (auto& [index, buffers] : virtualGeometryGpuByMesh_)
        {
            (void)index;
            destroyVirtualGeometry(buffers);
        }
    }
    gpuDrivenVertices_ = {};
    gpuDrivenIndices_ = {};
    meshDraws_ = {};
    meshDrawRows_.clear();
    virtualGeometryGpuByMesh_.clear();
    virtualGeometryByMesh_.clear();
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
