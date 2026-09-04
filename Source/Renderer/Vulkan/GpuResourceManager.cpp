#include "GpuResourceManager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stb_image.h>

namespace Halcyon::Vulkan
{
void GpuResourceManager::initialize(VkDevice device,
    VkCommandPool commandPool,
    VkQueue queue,
    GpuAllocator& allocator,
    GpuUploader& uploader) noexcept
{
    shutdown();
    device_ = device;
    commandPool_ = commandPool;
    queue_ = queue;
    allocator_ = &allocator;
    uploader_ = &uploader;
}

Halcyon::Result<TextureResource> GpuResourceManager::loadTexture2D(
    const std::string& path, bool srgb)
{
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || uploader_ == nullptr)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::InvalidState, "GPU resource manager is not initialized"});
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::Io, "Failed to load texture: " + path});
    }
    const std::size_t byteCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(pixels), byteCount);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto imageResult = allocator_->createImage(imageInfo, MemoryUsage::GpuOnly);
    if (!imageResult)
    {
        stbi_image_free(pixels);
        return imageResult.error();
    }
    TextureResource texture{};
    texture.allocation = imageResult.value();
    texture.extent = imageInfo.extent;
    texture.format = imageInfo.format;
    const auto uploadResult = uploader_->uploadImage(device_,
        commandPool_,
        queue_,
        *allocator_,
        texture.allocation,
        texture.extent,
        texture.format,
        bytes);
    stbi_image_free(pixels);
    if (!uploadResult)
    {
        destroy(texture);
        return uploadResult.error();
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.allocation.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, &texture.view);
    if (result != VK_SUCCESS)
    {
        destroy(texture);
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::Backend, "Failed to create texture image view"});
    }
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 1.0f;
    result = vkCreateSampler(device_, &samplerInfo, nullptr, &texture.sampler);
    if (result != VK_SUCCESS)
    {
        destroy(texture);
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::Backend, "Failed to create texture sampler"});
    }
    return texture;
}

Halcyon::Result<TextureResource> GpuResourceManager::loadSolidColorTexture(
    std::array<std::uint8_t, 4> rgba, bool srgb)
{
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || uploader_ == nullptr)
    {
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::InvalidState, "GPU resource manager is not initialized"});
    }
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {1, 1, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto imageResult = allocator_->createImage(imageInfo, MemoryUsage::GpuOnly);
    if (!imageResult)
    {
        return imageResult.error();
    }

    TextureResource texture{};
    texture.allocation = imageResult.value();
    texture.extent = imageInfo.extent;
    texture.format = imageInfo.format;
    const auto bytes =
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(rgba.data()), rgba.size());
    const auto uploadResult = uploader_->uploadImage(device_,
        commandPool_,
        queue_,
        *allocator_,
        texture.allocation,
        texture.extent,
        texture.format,
        bytes);
    if (!uploadResult)
    {
        destroy(texture);
        return uploadResult.error();
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.allocation.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = texture.format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, &texture.view);
    if (result != VK_SUCCESS)
    {
        destroy(texture);
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::Backend, "Failed to create fallback texture image view"});
    }
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 1.0f;
    result = vkCreateSampler(device_, &samplerInfo, nullptr, &texture.sampler);
    if (result != VK_SUCCESS)
    {
        destroy(texture);
        return Halcyon::Result<TextureResource>::failure(
            {Halcyon::ErrorCode::Backend, "Failed to create fallback texture sampler"});
    }
    return texture;
}

Halcyon::Result<MeshResource> GpuResourceManager::uploadMesh(
    const Halcyon::Renderer::Scene::SceneMesh& sourceMesh)
{
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || uploader_ == nullptr ||
        sourceMesh.vertexData.empty() || sourceMesh.indices.empty() ||
        sourceMesh.vertexData.size() % sizeof(Halcyon::Renderer::Scene::StaticSceneVertex) != 0)
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Scene mesh data is empty or malformed"});
    }
    const std::size_t vertexCount =
        sourceMesh.vertexData.size() / sizeof(Halcyon::Renderer::Scene::StaticSceneVertex);
    std::vector<MeshVertex> vertices;
    vertices.reserve(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i)
    {
        Halcyon::Renderer::Scene::StaticSceneVertex source{};
        std::memcpy(&source, sourceMesh.vertexData.data() + i * sizeof(source), sizeof(source));
        MeshVertex value{};
        value.position[0] = source.position.x;
        value.position[1] = source.position.y;
        value.position[2] = source.position.z;
        value.normal[0] = source.normal.x;
        value.normal[1] = source.normal.y;
        value.normal[2] = source.normal.z;
        value.uv[0] = source.uv.x;
        value.uv[1] = source.uv.y;
        vertices.push_back(value);
    }
    MeshResource mesh{};
    VkBufferCreateInfo vertexInfo{};
    vertexInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexInfo.size = vertices.size() * sizeof(MeshVertex);
    vertexInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vertexInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const auto vertexResult = allocator_->createBuffer(vertexInfo, MemoryUsage::GpuOnly);
    if (!vertexResult)
    {
        return vertexResult.error();
    }
    mesh.vertexBuffer = vertexResult.value();
    VkBufferCreateInfo indexInfo = vertexInfo;
    indexInfo.size = sourceMesh.indices.size() * sizeof(std::uint32_t);
    indexInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto indexResult = allocator_->createBuffer(indexInfo, MemoryUsage::GpuOnly);
    if (!indexResult)
    {
        destroy(mesh);
        return indexResult.error();
    }
    mesh.indexBuffer = indexResult.value();
    auto upload = uploader_->uploadBuffer(device_,
        commandPool_,
        queue_,
        *allocator_,
        mesh.vertexBuffer,
        std::as_bytes(std::span(vertices)));
    if (upload)
    {
        upload = uploader_->uploadBuffer(device_,
            commandPool_,
            queue_,
            *allocator_,
            mesh.indexBuffer,
            std::as_bytes(std::span(sourceMesh.indices)));
    }
    if (!upload)
    {
        destroy(mesh);
        return upload.error();
    }
    mesh.indexCount = static_cast<std::uint32_t>(sourceMesh.indices.size());
    return mesh;
}

void GpuResourceManager::destroy(TextureResource& texture) noexcept
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (texture.sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device_, texture.sampler, nullptr);
        }
        if (texture.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, texture.view, nullptr);
        }
    }
    if (allocator_ != nullptr)
    {
        allocator_->destroy(texture.allocation);
    }
    texture = {};
}

void GpuResourceManager::destroy(MeshResource& mesh) noexcept
{
    if (allocator_ != nullptr)
    {
        allocator_->destroy(mesh.vertexBuffer);
        allocator_->destroy(mesh.indexBuffer);
    }
    mesh = {};
}

void GpuResourceManager::shutdown() noexcept
{
    device_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    uploader_ = nullptr;
}

} // namespace Halcyon::Vulkan
