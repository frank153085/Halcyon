#include "GpuResourceManager.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stb_image.h>
#include <string_view>
#include <glm/gtc/matrix_inverse.hpp>

namespace Halcyon::Vulkan
{
namespace
{

struct FaceIndex
{
    int position = 0;
    int uv = 0;
    int normal = 0;
};

[[nodiscard]] FaceIndex parseFaceIndex(std::string_view token)
{
    FaceIndex result{};
    std::stringstream stream{std::string(token)};
    char slash = 0;
    stream >> result.position;
    if (stream >> slash && slash == '/')
    {
        if (stream.peek() != '/')
        {
            stream >> result.uv;
        }
        if (stream >> slash && slash == '/')
        {
            stream >> result.normal;
        }
    }
    return result;
}

template <typename T>
[[nodiscard]] std::size_t resolveIndex(int index, const std::vector<T>& values)
{
    if (index > 0 && static_cast<std::size_t>(index) <= values.size())
    {
        return static_cast<std::size_t>(index - 1);
    }
    if (index < 0 && static_cast<std::size_t>(-index) <= values.size())
    {
        return values.size() - static_cast<std::size_t>(-index);
    }
    return values.size();
}

} // namespace

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
    if (!imageResult) return imageResult.error();

    TextureResource texture{};
    texture.allocation = imageResult.value();
    texture.extent = imageInfo.extent;
    texture.format = imageInfo.format;
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(rgba.data()), rgba.size());
    const auto uploadResult = uploader_->uploadImage(device_, commandPool_, queue_, *allocator_,
        texture.allocation, texture.extent, texture.format, bytes);
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

Halcyon::Result<MeshResource> GpuResourceManager::loadObj(const std::string& path)
{
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || uploader_ == nullptr)
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidState, "GPU resource manager is not initialized"});
    }
    std::ifstream file(path);
    if (!file)
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::Io, "Failed to open model: " + path});
    }
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::string line;
    while (std::getline(file, line))
    {
        std::stringstream stream(line);
        std::string type;
        stream >> type;
        if (type == "v")
        {
            std::array<float, 3> value{};
            stream >> value[0] >> value[1] >> value[2];
            positions.push_back(value);
        }
        else if (type == "vn")
        {
            std::array<float, 3> value{};
            stream >> value[0] >> value[1] >> value[2];
            normals.push_back(value);
        }
        else if (type == "vt")
        {
            std::array<float, 2> value{};
            stream >> value[0] >> value[1];
            uvs.push_back(value);
        }
        else if (type == "f")
        {
            std::vector<FaceIndex> face;
            std::string token;
            while (stream >> token)
            {
                face.push_back(parseFaceIndex(token));
            }
            for (std::size_t i = 2; i < face.size(); ++i)
            {
                for (const FaceIndex index : {face[0], face[i - 1], face[i]})
                {
                    MeshVertex vertex{};
                    const std::size_t position = resolveIndex(index.position, positions);
                    const std::size_t normal = resolveIndex(index.normal, normals);
                    const std::size_t uv = resolveIndex(index.uv, uvs);
                    if (position >= positions.size())
                    {
                        return Halcyon::Result<MeshResource>::failure(
                            {Halcyon::ErrorCode::InvalidArgument, "OBJ face has invalid position"});
                    }
                    std::copy(
                        positions[position].begin(), positions[position].end(), vertex.position);
                    if (normal < normals.size())
                    {
                        std::copy(normals[normal].begin(), normals[normal].end(), vertex.normal);
                    }
                    if (uv < uvs.size())
                    {
                        vertex.uv[0] = uvs[uv][0];
                        vertex.uv[1] = 1.0f - uvs[uv][1];
                    }
                    indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                    vertices.push_back(vertex);
                }
            }
        }
    }
    if (vertices.empty())
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "OBJ contains no renderable faces"});
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
    indexInfo.size = indices.size() * sizeof(std::uint32_t);
    indexInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto indexResult = allocator_->createBuffer(indexInfo, MemoryUsage::GpuOnly);
    if (!indexResult)
    {
        destroy(mesh);
        return indexResult.error();
    }
    mesh.indexBuffer = indexResult.value();
    const auto vertexUpload = uploader_->uploadBuffer(device_,
        commandPool_,
        queue_,
        *allocator_,
        mesh.vertexBuffer,
        std::as_bytes(std::span(vertices)));
    const auto indexUpload = vertexUpload ? uploader_->uploadBuffer(device_,
                                                commandPool_,
                                                queue_,
                                                *allocator_,
                                                mesh.indexBuffer,
                                                std::as_bytes(std::span(indices)))
                                          : vertexUpload;
    if (!indexUpload)
    {
        destroy(mesh);
        return indexUpload.error();
    }
    mesh.indexCount = static_cast<std::uint32_t>(indices.size());
    return mesh;
}

Halcyon::Result<MeshResource> GpuResourceManager::uploadPrimitive(
    const Halcyon::Renderer::Scene::StaticScenePrimitive& primitive)
{
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || uploader_ == nullptr ||
        primitive.vertices.empty() || primitive.indices.empty())
    {
        return Halcyon::Result<MeshResource>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "Static primitive is empty"});
    }
    std::vector<MeshVertex> vertices;
    vertices.reserve(primitive.vertices.size());
    const glm::mat4 world = primitive.worldTransform;
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
    for (const auto& source : primitive.vertices)
    {
        MeshVertex value{};
        const glm::vec4 transformedPosition = world * glm::vec4(source.position, 1.0f);
        const glm::vec3 transformedNormal = glm::normalize(normalMatrix * source.normal);
        value.position[0] = transformedPosition.x;
        value.position[1] = transformedPosition.y;
        value.position[2] = transformedPosition.z;
        value.normal[0] = transformedNormal.x;
        value.normal[1] = transformedNormal.y;
        value.normal[2] = transformedNormal.z;
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
    if (!vertexResult) return vertexResult.error();
    mesh.vertexBuffer = vertexResult.value();
    VkBufferCreateInfo indexInfo = vertexInfo;
    indexInfo.size = primitive.indices.size() * sizeof(std::uint32_t);
    indexInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto indexResult = allocator_->createBuffer(indexInfo, MemoryUsage::GpuOnly);
    if (!indexResult) { destroy(mesh); return indexResult.error(); }
    mesh.indexBuffer = indexResult.value();
    auto upload = uploader_->uploadBuffer(device_, commandPool_, queue_, *allocator_,
        mesh.vertexBuffer, std::as_bytes(std::span(vertices)));
    if (upload)
    {
        upload = uploader_->uploadBuffer(device_, commandPool_, queue_, *allocator_,
            mesh.indexBuffer, std::as_bytes(std::span(primitive.indices)));
    }
    if (!upload) { destroy(mesh); return upload.error(); }
    mesh.indexCount = static_cast<std::uint32_t>(primitive.indices.size());
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
