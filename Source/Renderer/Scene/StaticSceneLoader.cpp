#include "StaticSceneLoader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Halcyon::Renderer::Scene
{
namespace
{

[[nodiscard]] Halcyon::Error loadError(
    Halcyon::ErrorCode code, std::string message, const std::filesystem::path& path)
{
    return Halcyon::MakeError(code, std::move(message), path.string());
}

template <typename Matrix>
[[nodiscard]] glm::mat4 toGlmMatrix(const Matrix& source) noexcept
{
    glm::mat4 result{1.0f};
    for (glm::length_t column = 0; column < 4u; ++column)
    {
        for (glm::length_t row = 0; row < 4u; ++row)
        {
            result[column][row] = static_cast<float>(source[column][row]);
        }
    }
    return result;
}

void generateNormals(StaticScenePrimitive& primitive) noexcept
{
    for (auto& vertex : primitive.vertices) vertex.normal = glm::vec3{0.0f};
    for (std::size_t i = 0; i + 2u < primitive.indices.size(); i += 3u)
    {
        const std::uint32_t ia = primitive.indices[i];
        const std::uint32_t ib = primitive.indices[i + 1u];
        const std::uint32_t ic = primitive.indices[i + 2u];
        if (ia >= primitive.vertices.size() || ib >= primitive.vertices.size() ||
            ic >= primitive.vertices.size()) continue;
        const glm::vec3 edgeA = primitive.vertices[ib].position - primitive.vertices[ia].position;
        const glm::vec3 edgeB = primitive.vertices[ic].position - primitive.vertices[ia].position;
        const glm::vec3 normal = glm::cross(edgeA, edgeB);
        primitive.vertices[ia].normal += normal;
        primitive.vertices[ib].normal += normal;
        primitive.vertices[ic].normal += normal;
    }
    for (auto& vertex : primitive.vertices)
    {
        const float length = glm::length(vertex.normal);
        vertex.normal = length > 1.0e-7f ? vertex.normal / length : glm::vec3{0, 0, 1};
    }
}

void generateTangents(StaticScenePrimitive& primitive) noexcept
{
    std::vector<glm::vec3> tangent(primitive.vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> bitangent(primitive.vertices.size(), glm::vec3{0.0f});
    for (std::size_t i = 0; i + 2u < primitive.indices.size(); i += 3u)
    {
        const auto ia = primitive.indices[i];
        const auto ib = primitive.indices[i + 1u];
        const auto ic = primitive.indices[i + 2u];
        if (ia >= primitive.vertices.size() || ib >= primitive.vertices.size() ||
            ic >= primitive.vertices.size()) continue;
        const glm::vec3 e1 = primitive.vertices[ib].position - primitive.vertices[ia].position;
        const glm::vec3 e2 = primitive.vertices[ic].position - primitive.vertices[ia].position;
        const glm::vec2 duv1 = primitive.vertices[ib].uv - primitive.vertices[ia].uv;
        const glm::vec2 duv2 = primitive.vertices[ic].uv - primitive.vertices[ia].uv;
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1.0e-8f) continue;
        const float inverse = 1.0f / determinant;
        const glm::vec3 t = (e1 * duv2.y - e2 * duv1.y) * inverse;
        const glm::vec3 b = (e2 * duv1.x - e1 * duv2.x) * inverse;
        tangent[ia] += t; tangent[ib] += t; tangent[ic] += t;
        bitangent[ia] += b; bitangent[ib] += b; bitangent[ic] += b;
    }
    for (std::size_t i = 0; i < primitive.vertices.size(); ++i)
    {
        const glm::vec3 normal = glm::normalize(primitive.vertices[i].normal);
        glm::vec3 t = tangent[i] - normal * glm::dot(normal, tangent[i]);
        if (glm::dot(t, t) < 1.0e-8f)
            t = std::abs(normal.z) < 0.999f ? glm::normalize(glm::cross(normal, {0, 0, 1}))
                                            : glm::vec3{1, 0, 0};
        else t = glm::normalize(t);
        const float handedness = glm::dot(glm::cross(normal, t), bitangent[i]) < 0.0f ? -1.0f : 1.0f;
        primitive.vertices[i].tangent = glm::vec4{t, handedness};
    }
}

[[nodiscard]] Halcyon::Result<std::string> writeEmbeddedImage(
    const std::filesystem::path& scenePath, std::size_t imageIndex,
    const std::byte* bytes, std::size_t byteCount)
{
    if (bytes == nullptr || byteCount == 0u)
        return Halcyon::Result<std::string>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "image data is empty", scenePath));
    const std::filesystem::path cachePath = scenePath.parent_path() /
        (scenePath.stem().string() + ".embedded_" + std::to_string(imageIndex) + ".bin");
    std::error_code error;
    if (!std::filesystem::exists(cachePath, error))
    {
        std::ofstream output(cachePath, std::ios::binary);
        if (!output)
            return Halcyon::Result<std::string>::failure(loadError(
                Halcyon::ErrorCode::Io, "failed to materialize embedded image", scenePath));
        output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(byteCount));
        if (!output)
            return Halcyon::Result<std::string>::failure(loadError(
                Halcyon::ErrorCode::Io, "failed to write embedded image", scenePath));
    }
    return Halcyon::Result<std::string>::success(cachePath.lexically_normal().generic_string());
}

[[nodiscard]] Halcyon::Result<std::string> imagePath(
    const fastgltf::Asset& asset, std::size_t imageIndex, const std::filesystem::path& scenePath)
{
    if (imageIndex >= asset.images.size())
        return Halcyon::Result<std::string>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "texture references an invalid image", scenePath));
    const fastgltf::DataSource& source = asset.images[imageIndex].data;
    return std::visit([&](const auto& value) -> Halcyon::Result<std::string>
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, fastgltf::sources::URI>)
        {
            if (!value.uri.valid() || !value.uri.isLocalPath())
                return Halcyon::Result<std::string>::failure(loadError(
                    Halcyon::ErrorCode::Unsupported, "image URI is not a local file", scenePath));
            const std::filesystem::path resolved =
                (scenePath.parent_path() / value.uri.fspath()).lexically_normal();
            std::error_code error;
            if (!std::filesystem::is_regular_file(resolved, error))
            {
                return Halcyon::Result<std::string>::failure(loadError(
                    Halcyon::ErrorCode::NotFound,
                    "referenced image does not exist: " + resolved.string(), scenePath));
            }
            return Halcyon::Result<std::string>::success(resolved.generic_string());
        }
        else if constexpr (std::is_same_v<T, fastgltf::sources::BufferView>)
        {
            if (value.bufferViewIndex >= asset.bufferViews.size())
                return Halcyon::Result<std::string>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "image references an invalid buffer view", scenePath));
            const auto bytes = fastgltf::DefaultBufferDataAdapter{}(asset, value.bufferViewIndex);
            return writeEmbeddedImage(scenePath, imageIndex, bytes.data(), bytes.size());
        }
        else if constexpr (std::is_same_v<T, fastgltf::sources::Array>)
            return writeEmbeddedImage(scenePath, imageIndex, value.bytes.data(), value.bytes.size_bytes());
        else if constexpr (std::is_same_v<T, fastgltf::sources::Vector>)
            return writeEmbeddedImage(scenePath, imageIndex, value.bytes.data(), value.bytes.size());
        else if constexpr (std::is_same_v<T, fastgltf::sources::ByteView>)
            return writeEmbeddedImage(scenePath, imageIndex, value.bytes.data(), value.bytes.size());
        else
            return Halcyon::Result<std::string>::failure(loadError(
                Halcyon::ErrorCode::Unsupported, "unsupported image data source", scenePath));
    }, source);
}

template <typename TextureInfo>
[[nodiscard]] Halcyon::Result<std::string> texturePath(
    const fastgltf::Asset& asset, const TextureInfo& info,
    const std::filesystem::path& scenePath)
{
    if (!info.has_value()) return Halcyon::Result<std::string>::success(std::string{});
    if (info->textureIndex >= asset.textures.size())
        return Halcyon::Result<std::string>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "material references an invalid texture", scenePath));
    const auto& texture = asset.textures[info->textureIndex];
    if (!texture.imageIndex.has_value())
        return Halcyon::Result<std::string>::failure(loadError(
            Halcyon::ErrorCode::Unsupported, "texture has no image source", scenePath));
    return imagePath(asset, texture.imageIndex.value(), scenePath);
}

[[nodiscard]] bool validAccessor(const fastgltf::Asset& asset, std::size_t index) noexcept
{
    return index < asset.accessors.size();
}

} // namespace

Halcyon::Result<StaticScene> loadStaticScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    if (path.empty())
        return Halcyon::Result<StaticScene>::failure(
            loadError(Halcyon::ErrorCode::InvalidArgument, "scene path is empty", path));
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (!data)
    {
        const auto code = data.error() == fastgltf::Error::InvalidPath
                              ? Halcyon::ErrorCode::NotFound : Halcyon::ErrorCode::Io;
        return Halcyon::Result<StaticScene>::failure(loadError(code,
            std::string("fastgltf failed to open scene: ") +
                std::string(fastgltf::getErrorMessage(data.error())), path));
    }
    fastgltf::Parser parser;
    constexpr auto parserOptions = fastgltf::Options::LoadExternalBuffers |
        fastgltf::Options::GenerateMeshIndices;
    auto parsed = parser.loadGltf(data.get(), path.parent_path(), parserOptions);
    if (!parsed)
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument,
            std::string("fastgltf failed to parse scene: ") +
                std::string(fastgltf::getErrorMessage(parsed.error())), path));
    fastgltf::Asset& asset = parsed.get();
    if (fastgltf::validate(asset) != fastgltf::Error::None)
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "fastgltf scene validation failed", path));
    if (!asset.animations.empty() || !asset.skins.empty())
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::Unsupported,
            "animations and skins are not supported by the static scene loader", path));
    for (const auto& node : asset.nodes)
        if (node.skinIndex.has_value() || !node.weights.empty())
            return Halcyon::Result<StaticScene>::failure(loadError(
                Halcyon::ErrorCode::Unsupported,
                "skinned or morph-target nodes are not supported by the static scene loader", path));
    for (const auto& mesh : asset.meshes)
    {
        if (!mesh.weights.empty())
            return Halcyon::Result<StaticScene>::failure(loadError(
                Halcyon::ErrorCode::Unsupported, "morph-target meshes are not supported by the static scene loader", path));
        for (const auto& primitive : mesh.primitives)
            if (!primitive.targets.empty())
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::Unsupported, "morph-target primitives are not supported by the static scene loader", path));
    }

    StaticScene scene;
    scene.sourcePath = path.lexically_normal().generic_string();
    scene.materials.reserve(asset.materials.size());
    for (std::size_t i = 0; i < asset.materials.size(); ++i)
    {
        const auto& source = asset.materials[i];
        StaticSceneMaterial material;
        material.name = source.name.empty() ? "material_" + std::to_string(i) : std::string(source.name);
        material.pbr.baseColor = {static_cast<float>(source.pbrData.baseColorFactor[0]),
            static_cast<float>(source.pbrData.baseColorFactor[1]), static_cast<float>(source.pbrData.baseColorFactor[2]),
            static_cast<float>(source.pbrData.baseColorFactor[3])};
        material.pbr.metallic = static_cast<float>(source.pbrData.metallicFactor);
        material.pbr.roughness = static_cast<float>(source.pbrData.roughnessFactor);
        material.pbr.emissive = {static_cast<float>(source.emissiveFactor[0]),
            static_cast<float>(source.emissiveFactor[1]), static_cast<float>(source.emissiveFactor[2])};
        auto baseColor = texturePath(asset, source.pbrData.baseColorTexture, path);
        auto metallicRoughness = texturePath(asset, source.pbrData.metallicRoughnessTexture, path);
        auto normal = texturePath(asset, source.normalTexture, path);
        auto occlusion = texturePath(asset, source.occlusionTexture, path);
        auto emissive = texturePath(asset, source.emissiveTexture, path);
        if (!baseColor || !metallicRoughness || !normal || !occlusion || !emissive)
        {
            const Halcyon::Error error = !baseColor ? baseColor.error() : !metallicRoughness ? metallicRoughness.error()
                : !normal ? normal.error() : !occlusion ? occlusion.error() : emissive.error();
            return Halcyon::Result<StaticScene>::failure(error.withContext("material " + material.name));
        }
        material.baseColorTexture = std::move(baseColor).value();
        material.metallicRoughnessTexture = std::move(metallicRoughness).value();
        material.normalTexture = std::move(normal).value();
        material.occlusionTexture = std::move(occlusion).value();
        material.emissiveTexture = std::move(emissive).value();
        material.pbr.ambientOcclusion = source.occlusionTexture.has_value()
            ? std::clamp(static_cast<float>(source.occlusionTexture->strength), 0.0f, 1.0f) : 1.0f;
        material.doubleSided = source.doubleSided;
        material.transparent = source.alphaMode == fastgltf::AlphaMode::Blend;
        material.alphaMasked = source.alphaMode == fastgltf::AlphaMode::Mask;
        material.alphaCutoff = static_cast<float>(source.alphaCutoff);
        scene.materials.push_back(std::move(material));
    }
    if (scene.materials.empty()) scene.materials.push_back(StaticSceneMaterial{});

    std::vector<std::vector<std::uint32_t>> meshPrimitiveIndices(asset.meshes.size());
    for (std::size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        for (const auto& source : asset.meshes[meshIndex].primitives)
        {
            if (source.type != fastgltf::PrimitiveType::Triangles)
            {
                if (options.rejectUnsupportedPrimitives)
                    return Halcyon::Result<StaticScene>::failure(loadError(
                        Halcyon::ErrorCode::Unsupported, "scene contains a non-triangle primitive", path));
                continue;
            }
            const auto positionIt = source.findAttribute("POSITION");
            if (positionIt == source.attributes.end() || !validAccessor(asset, positionIt->accessorIndex))
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "primitive has no valid positions", path));
            const auto& positions = asset.accessors[positionIt->accessorIndex];
            if (positions.type != fastgltf::AccessorType::Vec3 || positions.count == 0u)
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "POSITION accessor is not a non-empty VEC3", path));
            StaticScenePrimitive primitive;
            primitive.vertices.resize(positions.count);
            primitive.boundsMin = glm::vec3{std::numeric_limits<float>::max()};
            primitive.boundsMax = glm::vec3{-std::numeric_limits<float>::max()};
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, positions,
                [&](const auto& value, std::size_t index)
                {
                    auto& vertex = primitive.vertices[index];
                    vertex.position = {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
                    primitive.boundsMin = glm::min(primitive.boundsMin, vertex.position);
                    primitive.boundsMax = glm::max(primitive.boundsMax, vertex.position);
                });
            bool hasNormals = false;
            if (const auto it = source.findAttribute("NORMAL"); it != source.attributes.end() && validAccessor(asset, it->accessorIndex))
            {
                const auto& normals = asset.accessors[it->accessorIndex];
                hasNormals = normals.type == fastgltf::AccessorType::Vec3 && normals.count == positions.count;
                if (hasNormals) fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, normals,
                    [&](const auto& value, std::size_t index) { primitive.vertices[index].normal = {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])}; });
            }
            if (const auto it = source.findAttribute("TEXCOORD_0"); it != source.attributes.end() && validAccessor(asset, it->accessorIndex))
            {
                const auto& uvs = asset.accessors[it->accessorIndex];
                if (uvs.type == fastgltf::AccessorType::Vec2 && uvs.count == positions.count)
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, uvs,
                        [&](const auto& value, std::size_t index) { primitive.vertices[index].uv = {static_cast<float>(value[0]), options.flipV ? 1.0f - static_cast<float>(value[1]) : static_cast<float>(value[1])}; });
            }
            bool hasTangents = false;
            if (const auto it = source.findAttribute("TANGENT"); it != source.attributes.end() && validAccessor(asset, it->accessorIndex))
            {
                const auto& tangents = asset.accessors[it->accessorIndex];
                hasTangents = tangents.type == fastgltf::AccessorType::Vec4 && tangents.count == positions.count;
                if (hasTangents) fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, tangents,
                    [&](const auto& value, std::size_t index) { primitive.vertices[index].tangent = {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2]), static_cast<float>(value[3])}; });
            }
            if (source.indicesAccessor.has_value())
            {
                if (!validAccessor(asset, source.indicesAccessor.value()) || asset.accessors[source.indicesAccessor.value()].type != fastgltf::AccessorType::Scalar)
                    return Halcyon::Result<StaticScene>::failure(loadError(
                        Halcyon::ErrorCode::InvalidArgument, "primitive indices are invalid", path));
                const auto& indices = asset.accessors[source.indicesAccessor.value()];
                primitive.indices.reserve(indices.count);
                bool validIndices = true;
                fastgltf::iterateAccessor<std::uint32_t>(asset, indices,
                    [&](std::uint32_t value) { if (value >= primitive.vertices.size()) validIndices = false; primitive.indices.push_back(value); });
                if (!validIndices)
                    return Halcyon::Result<StaticScene>::failure(loadError(
                        Halcyon::ErrorCode::InvalidArgument, "primitive index is out of range", path));
            }
            else
            {
                primitive.indices.resize(primitive.vertices.size());
                std::iota(primitive.indices.begin(), primitive.indices.end(), 0u);
            }
            if (primitive.indices.size() % 3u != 0u)
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "triangle primitive has incomplete index data", path));
            if (!hasNormals && options.generateMissingNormals) generateNormals(primitive);
            if (!hasTangents && options.generateMissingTangents) generateTangents(primitive);
            if (source.materialIndex.has_value())
            {
                if (source.materialIndex.value() >= scene.materials.size())
                    return Halcyon::Result<StaticScene>::failure(loadError(
                        Halcyon::ErrorCode::InvalidArgument, "primitive material index is out of range", path));
                primitive.materialIndex = static_cast<std::uint32_t>(source.materialIndex.value());
            }
            meshPrimitiveIndices[meshIndex].push_back(static_cast<std::uint32_t>(scene.primitives.size()));
            scene.primitives.push_back(std::move(primitive));
        }
    }

    scene.nodes.resize(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
    {
        const auto& source = asset.nodes[i];
        auto& node = scene.nodes[i];
        node.name = source.name.empty() ? "node_" + std::to_string(i) : std::string(source.name);
        node.localTransform = toGlmMatrix(fastgltf::getTransformMatrix(source));
        if (source.meshIndex.has_value())
        {
            if (source.meshIndex.value() >= meshPrimitiveIndices.size())
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "node mesh index is out of range", path));
            node.primitiveIndices = meshPrimitiveIndices[source.meshIndex.value()];
        }
        for (const std::size_t child : source.children)
        {
            if (child >= scene.nodes.size() || (scene.nodes[child].parent >= 0 && scene.nodes[child].parent != static_cast<std::int32_t>(i)))
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "node child index is invalid or duplicated", path));
            scene.nodes[child].parent = static_cast<std::int32_t>(i);
        }
    }
    std::vector<std::uint8_t> state(scene.nodes.size(), 0u);
    bool cycleDetected = false;
    const auto resolve = [&](auto&& self, std::size_t index) -> glm::mat4
    {
        if (state[index] == 2u) return scene.nodes[index].worldTransform;
        if (state[index] == 1u)
        {
            // A parent cycle is invalid for a static scene.  Do not silently
            // manufacture a transform: report it to the caller after the
            // traversal so malformed assets cannot produce nondeterministic
            // world matrices.
            cycleDetected = true;
            return scene.nodes[index].worldTransform;
        }
        state[index] = 1u;
        const auto& node = scene.nodes[index];
        const glm::mat4 parent = node.parent >= 0 ? self(self, static_cast<std::size_t>(node.parent)) : glm::mat4{1.0f};
        scene.nodes[index].worldTransform = parent * node.localTransform;
        state[index] = 2u;
        return scene.nodes[index].worldTransform;
    };
    for (std::size_t i = 0; i < scene.nodes.size(); ++i) (void)resolve(resolve, i);
    if (cycleDetected)
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "scene node hierarchy contains a cycle", path));
    std::vector<std::uint8_t> primitiveUsed(scene.primitives.size(), 0u);
    for (auto& node : scene.nodes)
    {
        const auto authored = node.primitiveIndices;
        node.primitiveIndices.clear();
        for (const std::uint32_t primitiveIndex : authored)
        {
            if (primitiveIndex >= scene.primitives.size()) continue;
            if (primitiveUsed[primitiveIndex] == 0u)
            {
                primitiveUsed[primitiveIndex] = 1u;
                scene.primitives[primitiveIndex].worldTransform = node.worldTransform;
                node.primitiveIndices.push_back(primitiveIndex);
            }
            else
            {
                StaticScenePrimitive copy = scene.primitives[primitiveIndex];
                copy.worldTransform = node.worldTransform;
                node.primitiveIndices.push_back(static_cast<std::uint32_t>(scene.primitives.size()));
                scene.primitives.push_back(std::move(copy));
                primitiveUsed.push_back(1u);
            }
        }
    }
    if (scene.primitives.empty())
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "scene contains no triangle primitives", path));
    return Halcyon::Result<StaticScene>::success(std::move(scene));
}

} // namespace Halcyon::Renderer::Scene
