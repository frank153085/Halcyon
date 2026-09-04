#include "StaticSceneLoader.h"

#define CGLTF_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cgltf.h>
#include <fastgltf/core.hpp>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <numeric>
#include <string_view>
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

[[nodiscard]] glm::mat4 toGlmMatrix(const cgltf_float* values) noexcept
{
    glm::mat4 result{1.0f};
    if (values == nullptr)
    {
        return result;
    }
    // glTF matrices are column-major, matching glm's memory layout.
    std::memcpy(&result[0][0], values, sizeof(float) * 16u);
    return result;
}

[[nodiscard]] std::string texturePath(const cgltf_texture_view& view,
    const std::filesystem::path& scenePath,
    const cgltf_data* data)
{
    if (view.texture == nullptr || view.texture->image == nullptr)
    {
        return {};
    }
    const auto* image = view.texture->image;
    const auto* imageBegin = data != nullptr ? data->images : nullptr;
    const auto* imageEnd = (data != nullptr && imageBegin != nullptr)
                               ? imageBegin + data->images_count
                               : nullptr;
    if (data == nullptr || imageBegin == nullptr || imageEnd == nullptr || image < imageBegin ||
        image >= imageEnd)
    {
        return {};
    }
    const std::size_t imageIndex = static_cast<std::size_t>(image - imageBegin);
    if (image->uri != nullptr)
    {
        const std::string uri{image->uri};
        // glTF permits data URIs for images.  Decode them once to a cache file
        // so the rest of the loader can use the same path-based texture
        // contract as external images and GLB buffer views.
        if (uri.rfind("data:", 0u) == 0u)
        {
            const std::size_t comma = uri.find(',');
            if (comma == std::string::npos) return {};
            const std::string metadata = uri.substr(5u, comma - 5u);
            const std::string payload = uri.substr(comma + 1u);
            std::vector<std::uint8_t> bytes;
            if (metadata.find(";base64") != std::string::npos)
            {
                static constexpr char alphabet[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::array<int, 256> reverse{};
                reverse.fill(-1);
                for (int i = 0; i < 64; ++i)
                    reverse[static_cast<unsigned char>(alphabet[i])] = i;
                std::uint32_t value = 0;
                int bits = -8;
                for (const unsigned char character : payload)
                {
                    if (std::isspace(character) != 0) continue;
                    if (character == '=') break;
                    const int decoded = reverse[character];
                    if (decoded < 0) return {};
                    value = (value << 6u) | static_cast<std::uint32_t>(decoded);
                    bits += 6;
                    if (bits >= 0)
                    {
                        bytes.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
                        bits -= 8;
                        // Keep only the carry bits so long data URIs cannot
                        // overflow the accumulator between emitted bytes.
                        value = bits > 0 ? value & ((1u << bits) - 1u) : 0u;
                    }
                }
            }
            else
            {
                // Non-base64 data URIs are percent-encoded byte strings.
                for (std::size_t i = 0; i < payload.size(); ++i)
                {
                    if (payload[i] == '%' && i + 2u < payload.size())
                    {
                        const auto hex = [](char c) -> int
                        {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            return -1;
                        };
                        const int high = hex(payload[i + 1u]);
                        const int low = hex(payload[i + 2u]);
                        if (high < 0 || low < 0) return {};
                        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
                        i += 2u;
                    }
                    else
                    {
                        bytes.push_back(static_cast<std::uint8_t>(payload[i]));
                    }
                }
            }
            if (bytes.empty()) return {};
            const std::filesystem::path cachePath = scenePath.parent_path() /
                (scenePath.stem().string() + ".embedded_" + std::to_string(imageIndex) + ".bin");
            std::error_code error;
            if (!std::filesystem::exists(cachePath, error))
            {
                std::ofstream output(cachePath, std::ios::binary);
                if (!output) return {};
                output.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            }
            return cachePath.generic_string();
        }
        // Resolve percent-escaped external filenames (spaces are common in
        // hand-authored scenes) before joining them to the scene directory.
        std::string decoded;
        decoded.reserve(uri.size());
        for (std::size_t i = 0; i < uri.size(); ++i)
        {
            if (uri[i] == '%' && i + 2u < uri.size())
            {
                const auto hex = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int high = hex(uri[i + 1u]);
                const int low = hex(uri[i + 2u]);
                if (high >= 0 && low >= 0)
                {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2u;
                    continue;
                }
            }
            decoded.push_back(uri[i]);
        }
        const std::filesystem::path relative{decoded};
        return (scenePath.parent_path() / relative).lexically_normal().generic_string();
    }
    // GLB images are stored in a buffer view.  stb_image can decode encoded
    // bytes without relying on a file extension, so persist a small cache file
    // next to the source and expose it through the same material contract.
    if (data == nullptr || image->buffer_view == nullptr || image->buffer_view->buffer == nullptr ||
        image->buffer_view->buffer->data == nullptr)
    {
        return {};
    }
    const std::filesystem::path cachePath = scenePath.parent_path() /
        (scenePath.stem().string() + ".embedded_" + std::to_string(imageIndex) + ".bin");
    std::error_code error;
    if (!std::filesystem::exists(cachePath, error))
    {
        const auto* bytes = static_cast<const std::byte*>(image->buffer_view->buffer->data) +
                            image->buffer_view->offset;
        std::ofstream output(cachePath, std::ios::binary);
        if (output)
        {
            output.write(reinterpret_cast<const char*>(bytes),
                static_cast<std::streamsize>(image->buffer_view->size));
        }
    }
    return cachePath.generic_string();
}

[[nodiscard]] const cgltf_accessor* findAttribute(
    const cgltf_primitive& primitive, cgltf_attribute_type type, int index = 0) noexcept
{
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
    {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.type == type && attribute.index == index)
        {
            return attribute.data;
        }
    }
    return nullptr;
}

[[nodiscard]] bool readAttribute(const cgltf_accessor* accessor,
    cgltf_size index,
    float* values,
    cgltf_size elementSize) noexcept
{
    return accessor != nullptr && index < accessor->count &&
           cgltf_accessor_read_float(accessor, index, values, elementSize) != 0;
}

void generateNormals(StaticScenePrimitive& primitive) noexcept
{
    for (auto& vertex : primitive.vertices)
    {
        vertex.normal = glm::vec3{0.0f};
    }
    for (std::size_t i = 0; i + 2u < primitive.indices.size(); i += 3u)
    {
        const std::uint32_t ia = primitive.indices[i];
        const std::uint32_t ib = primitive.indices[i + 1u];
        const std::uint32_t ic = primitive.indices[i + 2u];
        if (ia >= primitive.vertices.size() || ib >= primitive.vertices.size() ||
            ic >= primitive.vertices.size())
        {
            continue;
        }
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
            ic >= primitive.vertices.size())
        {
            continue;
        }
        const glm::vec3 e1 = primitive.vertices[ib].position - primitive.vertices[ia].position;
        const glm::vec3 e2 = primitive.vertices[ic].position - primitive.vertices[ia].position;
        const glm::vec2 duv1 = primitive.vertices[ib].uv - primitive.vertices[ia].uv;
        const glm::vec2 duv2 = primitive.vertices[ic].uv - primitive.vertices[ia].uv;
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1.0e-8f)
        {
            continue;
        }
        const float inverse = 1.0f / determinant;
        const glm::vec3 t = (e1 * duv2.y - e2 * duv1.y) * inverse;
        const glm::vec3 b = (e2 * duv1.x - e1 * duv2.x) * inverse;
        tangent[ia] += t;
        tangent[ib] += t;
        tangent[ic] += t;
        bitangent[ia] += b;
        bitangent[ib] += b;
        bitangent[ic] += b;
    }
    for (std::size_t i = 0; i < primitive.vertices.size(); ++i)
    {
        const glm::vec3 normal = glm::normalize(primitive.vertices[i].normal);
        glm::vec3 t = tangent[i] - normal * glm::dot(normal, tangent[i]);
        if (glm::dot(t, t) < 1.0e-8f)
        {
            t = std::abs(normal.z) < 0.999f ? glm::normalize(glm::cross(normal, glm::vec3{0, 0, 1}))
                                            : glm::vec3{1, 0, 0};
        }
        else
        {
            t = glm::normalize(t);
        }
        const float handedness =
            glm::dot(glm::cross(normal, t), bitangent[i]) < 0.0f ? -1.0f : 1.0f;
        primitive.vertices[i].tangent = glm::vec4{t, handedness};
    }
}

[[nodiscard]] std::int32_t nodeIndex(const cgltf_data* data, const cgltf_node* node) noexcept
{
    if (data == nullptr || node == nullptr || data->nodes == nullptr)
    {
        return -1;
    }
    const auto* begin = data->nodes;
    const auto* end = data->nodes + data->nodes_count;
    return node >= begin && node < end ? static_cast<std::int32_t>(node - begin) : -1;
}

} // namespace

Halcyon::Result<StaticScene> loadStaticSceneCgltf(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    if (path.empty())
    {
        return Halcyon::Result<StaticScene>::failure(
            loadError(Halcyon::ErrorCode::InvalidArgument, "scene path is empty", path));
    }
    cgltf_options cgltfOptions{};
    cgltf_data* data = nullptr;
    const cgltf_result parseResult = cgltf_parse_file(&cgltfOptions, path.string().c_str(), &data);
    if (parseResult != cgltf_result_success || data == nullptr)
    {
        const auto code = parseResult == cgltf_result_file_not_found
                              ? Halcyon::ErrorCode::NotFound
                              : Halcyon::ErrorCode::InvalidArgument;
        return Halcyon::Result<StaticScene>::failure(
            loadError(code, "failed to parse glTF scene", path));
    }
    const auto cleanup = [&]() noexcept
    {
        cgltf_free(data);
    };
    const cgltf_result buffersResult =
        cgltf_load_buffers(&cgltfOptions, data, path.string().c_str());
    if (buffersResult != cgltf_result_success)
    {
        cleanup();
        return Halcyon::Result<StaticScene>::failure(
            loadError(Halcyon::ErrorCode::Io, "failed to load glTF buffers", path));
    }
    const cgltf_result validateResult = cgltf_validate(data);
    if (validateResult != cgltf_result_success)
    {
        cleanup();
        return Halcyon::Result<StaticScene>::failure(
            loadError(Halcyon::ErrorCode::InvalidArgument, "glTF validation failed", path));
    }
    if (data->animations_count != 0)
    {
        cleanup();
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument,
            "animations are not supported by the static M3 scene loader",
            path));
    }
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
    {
        const cgltf_mesh& mesh = data->meshes[meshIndex];
        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count;
            ++primitiveIndex)
        {
            if (mesh.primitives[primitiveIndex].targets_count != 0)
            {
                cleanup();
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument,
                    "morph targets are not supported by the static M3 scene loader",
                    path));
            }
        }
    }

    StaticScene scene;
    scene.sourcePath = path.lexically_normal().generic_string();
    scene.materials.reserve(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i)
    {
        const cgltf_material& source = data->materials[i];
        StaticSceneMaterial material;
        material.name = source.name != nullptr ? source.name : "material_" + std::to_string(i);
        if (source.has_pbr_metallic_roughness)
        {
            const auto& pbr = source.pbr_metallic_roughness;
            material.pbr.baseColor = glm::make_vec4(pbr.base_color_factor);
            material.pbr.metallic = pbr.metallic_factor;
            material.pbr.roughness = pbr.roughness_factor;
            material.baseColorTexture = texturePath(pbr.base_color_texture, path, data);
            material.metallicRoughnessTexture = texturePath(pbr.metallic_roughness_texture, path, data);
        }
        else
        {
            material.pbr.baseColor = glm::vec4{1.0f};
        }
        material.pbr.emissive = glm::make_vec3(source.emissive_factor);
        material.normalTexture = texturePath(source.normal_texture, path, data);
        material.occlusionTexture = texturePath(source.occlusion_texture, path, data);
        material.emissiveTexture = texturePath(source.emissive_texture, path, data);
        material.pbr.ambientOcclusion = source.occlusion_texture.texture != nullptr
                                            ? std::clamp(source.occlusion_texture.scale, 0.0f, 1.0f)
                                            : 1.0f;
        material.doubleSided = source.double_sided != 0;
        material.transparent = source.alpha_mode == cgltf_alpha_mode_blend;
        material.alphaMasked = source.alpha_mode == cgltf_alpha_mode_mask;
        material.alphaCutoff = source.alpha_cutoff;
        scene.materials.push_back(std::move(material));
    }
    if (scene.materials.empty())
    {
        scene.materials.push_back(StaticSceneMaterial{});
    }

    std::vector<std::vector<std::uint32_t>> meshPrimitiveIndices(data->meshes_count);
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
    {
        const cgltf_mesh& mesh = data->meshes[meshIndex];
        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count;
            ++primitiveIndex)
        {
            const cgltf_primitive& source = mesh.primitives[primitiveIndex];
            if (source.type != cgltf_primitive_type_triangles)
            {
                if (options.rejectUnsupportedPrimitives)
                {
                    cleanup();
                    return Halcyon::Result<StaticScene>::failure(
                        loadError(Halcyon::ErrorCode::InvalidArgument,
                            "scene contains a non-triangle primitive",
                            path));
                }
                continue;
            }
            const cgltf_accessor* positions = findAttribute(source, cgltf_attribute_type_position);
            if (positions == nullptr || positions->count == 0)
            {
                cleanup();
                return Halcyon::Result<StaticScene>::failure(loadError(
                    Halcyon::ErrorCode::InvalidArgument, "primitive has no positions", path));
            }
            StaticScenePrimitive primitive;
            primitive.vertices.resize(positions->count);
            const cgltf_accessor* normals = findAttribute(source, cgltf_attribute_type_normal);
            const cgltf_accessor* uvs = findAttribute(source, cgltf_attribute_type_texcoord, 0);
            const cgltf_accessor* tangents = findAttribute(source, cgltf_attribute_type_tangent);
            bool hasNormals = normals != nullptr;
            bool hasTangents = tangents != nullptr;
            primitive.boundsMin = glm::vec3{std::numeric_limits<float>::max()};
            primitive.boundsMax = glm::vec3{-std::numeric_limits<float>::max()};
            for (cgltf_size vertexIndex = 0; vertexIndex < positions->count; ++vertexIndex)
            {
                float position[4]{};
                if (!readAttribute(positions, vertexIndex, position, 3))
                {
                    cleanup();
                    return Halcyon::Result<StaticScene>::failure(
                        loadError(Halcyon::ErrorCode::InvalidArgument,
                            "failed to read primitive position",
                            path));
                }
                auto& vertex = primitive.vertices[vertexIndex];
                vertex.position = glm::vec3{position[0], position[1], position[2]};
                primitive.boundsMin = glm::min(primitive.boundsMin, vertex.position);
                primitive.boundsMax = glm::max(primitive.boundsMax, vertex.position);
                float normal[4]{};
                if (hasNormals && readAttribute(normals, vertexIndex, normal, 3))
                {
                    vertex.normal = glm::vec3{normal[0], normal[1], normal[2]};
                }
                else
                {
                    hasNormals = false;
                }
                float uv[4]{};
                if (uvs != nullptr && readAttribute(uvs, vertexIndex, uv, 2))
                {
                    vertex.uv = glm::vec2{uv[0], options.flipV ? 1.0f - uv[1] : uv[1]};
                }
                float tangentValue[4]{};
                if (hasTangents && readAttribute(tangents, vertexIndex, tangentValue, 4))
                {
                    vertex.tangent = glm::vec4{
                        tangentValue[0], tangentValue[1], tangentValue[2], tangentValue[3]};
                }
                else
                {
                    hasTangents = false;
                }
            }
            if (source.indices != nullptr)
            {
                primitive.indices.reserve(source.indices->count);
                for (cgltf_size index = 0; index < source.indices->count; ++index)
                {
                    const cgltf_size value = cgltf_accessor_read_index(source.indices, index);
                    if (value >= primitive.vertices.size() ||
                        value > std::numeric_limits<std::uint32_t>::max())
                    {
                        cleanup();
                        return Halcyon::Result<StaticScene>::failure(
                            loadError(Halcyon::ErrorCode::InvalidArgument,
                                "primitive index is out of range",
                                path));
                    }
                    primitive.indices.push_back(static_cast<std::uint32_t>(value));
                }
            }
            else
            {
                primitive.indices.resize(primitive.vertices.size());
                std::iota(primitive.indices.begin(), primitive.indices.end(), 0u);
            }
            if ((primitive.indices.size() % 3u) != 0u)
            {
                cleanup();
                return Halcyon::Result<StaticScene>::failure(
                    loadError(Halcyon::ErrorCode::InvalidArgument,
                        "triangle primitive has incomplete index data",
                        path));
            }
            if (!hasNormals && options.generateMissingNormals)
            {
                generateNormals(primitive);
            }
            if (!hasTangents && options.generateMissingTangents)
            {
                generateTangents(primitive);
            }
            if (source.material != nullptr)
            {
                const auto* begin = data->materials;
                const auto* end = begin + data->materials_count;
                if (source.material >= begin && source.material < end)
                {
                    primitive.materialIndex = static_cast<std::uint32_t>(source.material - begin);
                }
            }
            meshPrimitiveIndices[meshIndex].push_back(
                static_cast<std::uint32_t>(scene.primitives.size()));
            scene.primitives.push_back(std::move(primitive));
        }
    }

    scene.nodes.reserve(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i)
    {
        const cgltf_node& source = data->nodes[i];
        StaticSceneNode node;
        node.name = source.name != nullptr ? source.name : "node_" + std::to_string(i);
        cgltf_float localValues[16]{};
        cgltf_node_transform_local(&source, localValues);
        node.localTransform = toGlmMatrix(localValues);
        node.parent = nodeIndex(data, source.parent);
        node.worldTransform = node.localTransform;
        if (source.mesh != nullptr)
        {
            const auto* meshBegin = data->meshes;
            const auto* meshEnd = meshBegin + data->meshes_count;
            if (source.mesh >= meshBegin && source.mesh < meshEnd)
            {
                const auto meshIndex = static_cast<std::size_t>(source.mesh - meshBegin);
                if (meshIndex < meshPrimitiveIndices.size())
                {
                    for (const std::uint32_t primitiveIndex : meshPrimitiveIndices[meshIndex])
                    {
                        node.primitiveIndices.push_back(primitiveIndex);
                    }
                }
            }
        }
        scene.nodes.push_back(std::move(node));
    }
    // Resolve world transforms in a second pass so a valid glTF whose parent
    // node appears later in the array is handled identically to one using
    // depth-first ordering.  Malformed cycles are broken at the repeated node.
    std::vector<std::uint8_t> transformState(scene.nodes.size(), 0u);
    const auto resolveWorldTransform = [&](auto&& self, std::size_t index) -> glm::mat4
    {
        if (index >= scene.nodes.size())
        {
            return glm::mat4{1.0f};
        }
        if (transformState[index] == 2u)
        {
            return scene.nodes[index].worldTransform;
        }
        if (transformState[index] == 1u)
        {
            return scene.nodes[index].localTransform;
        }
        transformState[index] = 1u;
        const auto& node = scene.nodes[index];
        const glm::mat4 parent =
            node.parent >= 0 ? self(self, static_cast<std::size_t>(node.parent)) : glm::mat4{1.0f};
        scene.nodes[index].worldTransform = parent * node.localTransform;
        transformState[index] = 2u;
        return scene.nodes[index].worldTransform;
    };
    for (std::size_t i = 0; i < scene.nodes.size(); ++i)
    {
        (void)resolveWorldTransform(resolveWorldTransform, i);
    }
    // Materialise node transforms on primitive instances.  A mesh may be
    // referenced by multiple nodes; duplicate the primitive for subsequent
    // instances so every draw retains its authored world transform instead of
    // silently applying the last node's transform to all instances.
    std::vector<std::uint8_t> primitiveUsed(scene.primitives.size(), 0u);
    for (auto& node : scene.nodes)
    {
        const auto authoredIndices = node.primitiveIndices;
        node.primitiveIndices.clear();
        node.primitiveIndices.reserve(authoredIndices.size());
        for (const std::uint32_t primitiveIndex : authoredIndices)
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
                StaticScenePrimitive instance = scene.primitives[primitiveIndex];
                instance.worldTransform = node.worldTransform;
                node.primitiveIndices.push_back(
                    static_cast<std::uint32_t>(scene.primitives.size()));
                scene.primitives.push_back(std::move(instance));
                primitiveUsed.push_back(1u);
            }
        }
    }
    cleanup();
    if (scene.primitives.empty())
    {
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument, "scene contains no triangle primitives", path));
    }
    return Halcyon::Result<StaticScene>::success(std::move(scene));
}

Halcyon::Result<StaticScene> loadStaticScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    // fastgltf performs the canonical format/URI validation (including GLB
    // chunk handling and external buffer/image resolution).  The compact
    // cgltf decoder below is retained as the byte-to-GLM adapter for now; the
    // preflight guarantees both loaders agree on supported static assets and
    // gives callers fastgltf's actionable error category when a file is bad.
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (!data)
    {
        const auto code = data.error() == fastgltf::Error::InvalidPath
                              ? Halcyon::ErrorCode::NotFound
                              : Halcyon::ErrorCode::Io;
        return Halcyon::Result<StaticScene>::failure(
            loadError(code, std::string("fastgltf failed to open scene: ") +
                                  std::string(fastgltf::getErrorName(data.error())),
                path));
    }
    fastgltf::Parser parser;
    auto loaded = parser.loadGltf(data.get(),
        path.parent_path(),
        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages |
            fastgltf::Options::GenerateMeshIndices);
    if (!loaded)
    {
        return Halcyon::Result<StaticScene>::failure(loadError(
            Halcyon::ErrorCode::InvalidArgument,
            std::string("fastgltf failed to parse scene: ") +
                std::string(fastgltf::getErrorName(loaded.error())),
            path));
    }
    if (fastgltf::validate(loaded.get()) != fastgltf::Error::None)
    {
        return Halcyon::Result<StaticScene>::failure(
            loadError(Halcyon::ErrorCode::InvalidArgument, "fastgltf scene validation failed", path));
    }
    return loadStaticSceneCgltf(path, options);
}

} // namespace Halcyon::Renderer::Scene
