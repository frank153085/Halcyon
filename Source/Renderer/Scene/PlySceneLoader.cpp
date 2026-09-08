#define TINYPLY_IMPLEMENTATION
#include <tinyply.h>

#include "PlySceneLoader.h"

#include <algorithm>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace Halcyon::Renderer::Scene
{
namespace
{

float readNumber(const tinyply::PlyData& data, std::size_t index)
{
    const auto* bytes = data.buffer.get_const();
    switch (data.t)
    {
    case tinyply::Type::FLOAT32: return static_cast<float>(reinterpret_cast<const float*>(bytes)[index]);
    case tinyply::Type::FLOAT64: return static_cast<float>(reinterpret_cast<const double*>(bytes)[index]);
    case tinyply::Type::INT8: return static_cast<float>(reinterpret_cast<const std::int8_t*>(bytes)[index]);
    case tinyply::Type::UINT8: return static_cast<float>(bytes[index]);
    case tinyply::Type::INT16: return static_cast<float>(reinterpret_cast<const std::int16_t*>(bytes)[index]);
    case tinyply::Type::UINT16: return static_cast<float>(reinterpret_cast<const std::uint16_t*>(bytes)[index]);
    case tinyply::Type::INT32: return static_cast<float>(reinterpret_cast<const std::int32_t*>(bytes)[index]);
    case tinyply::Type::UINT32: return static_cast<float>(reinterpret_cast<const std::uint32_t*>(bytes)[index]);
    default: throw std::runtime_error("unsupported PLY numeric property type");
    }
}

std::uint32_t readIndex(const tinyply::PlyData& data, std::size_t index)
{
    const auto* bytes = data.buffer.get_const();
    std::uint64_t value = 0;
    switch (data.t)
    {
    case tinyply::Type::UINT8: value = bytes[index]; break;
    case tinyply::Type::INT8: value = static_cast<std::uint64_t>(reinterpret_cast<const std::int8_t*>(bytes)[index]); break;
    case tinyply::Type::UINT16: value = reinterpret_cast<const std::uint16_t*>(bytes)[index]; break;
    case tinyply::Type::INT16: value = static_cast<std::uint64_t>(reinterpret_cast<const std::int16_t*>(bytes)[index]); break;
    case tinyply::Type::UINT32: value = reinterpret_cast<const std::uint32_t*>(bytes)[index]; break;
    case tinyply::Type::INT32: value = static_cast<std::uint64_t>(reinterpret_cast<const std::int32_t*>(bytes)[index]); break;
    default: throw std::runtime_error("unsupported PLY index type");
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("PLY index exceeds uint32");
    return static_cast<std::uint32_t>(value);
}

} // namespace

Halcyon::Result<StaticScene> loadPlyScene(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::NotFound, "unable to open PLY scene", path.string()});
        tinyply::PlyFile file;
        if (!file.parse_header(stream)) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "invalid PLY header", path.string()});
        std::shared_ptr<tinyply::PlyData> positions;
        std::shared_ptr<tinyply::PlyData> normals;
        std::shared_ptr<tinyply::PlyData> faces;
        for (const auto& element : file.get_elements())
        {
            if (element.name == "vertex")
            {
                bool hasNormals = false;
                for (const auto& property : element.properties)
                    if (property.name == "nx" || property.name == "ny" || property.name == "nz") hasNormals = true;
                if (hasNormals)
                {
                    auto combined = file.request_properties_from_element("vertex", {"x", "y", "z", "nx", "ny", "nz"});
                    positions = combined;
                    normals = combined;
                }
                else positions = file.request_properties_from_element("vertex", {"x", "y", "z"});
            }
            else if (element.name == "face")
            {
                faces = file.request_properties_from_element("face", {"vertex_indices"}, 0);
            }
        }
        if (!positions || !faces) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "PLY has no vertex/face elements", path.string()});
        file.read(stream);
        if (positions->count == 0 || faces->count == 0) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "PLY has empty geometry", path.string()});
        StaticScene scene; scene.sourcePath = path.string();
        scene.materials.push_back(StaticSceneMaterial{});
        StaticScenePrimitive primitive; primitive.vertices.resize(positions->count);
        glm::vec3 minimum(std::numeric_limits<float>::max());
        glm::vec3 maximum(std::numeric_limits<float>::lowest());
        for (std::size_t i = 0; i < positions->count; ++i)
        {
            auto& vertex = primitive.vertices[i];
            vertex.position = {readNumber(*positions, i * 3u), readNumber(*positions, i * 3u + 1u), readNumber(*positions, i * 3u + 2u)};
            if (normals && normals->count >= positions->count)
                vertex.normal = glm::normalize(glm::vec3{readNumber(*normals, i * 3u), readNumber(*normals, i * 3u + 1u), readNumber(*normals, i * 3u + 2u)});
            minimum = glm::min(minimum, vertex.position); maximum = glm::max(maximum, vertex.position);
        }
        primitive.boundsMin = minimum; primitive.boundsMax = maximum;
        // tinyply stores variable lists consecutively; triangulate using the list offsets.
        std::size_t cursor = 0;
        for (std::size_t face = 0; face < faces->count; ++face)
        {
            const std::size_t count = faces->list_sizes.empty() ? 3u : faces->list_sizes[face];
            if (count >= 3u)
            {
                const auto first = readIndex(*faces, cursor);
                for (std::size_t corner = 1; corner + 1u < count; ++corner)
                    primitive.indices.insert(primitive.indices.end(), {first, readIndex(*faces, cursor + corner), readIndex(*faces, cursor + corner + 1u)});
            }
            cursor += count;
        }
        if (primitive.indices.empty()) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "PLY has no triangles", path.string()});
        if (!normals && options.generateMissingNormals)
        {
            std::vector<glm::vec3> accumulated(primitive.vertices.size(), glm::vec3(0));
            for (std::size_t i = 0; i < primitive.indices.size(); i += 3u)
            {
                const auto a = primitive.indices[i], b = primitive.indices[i + 1u], c = primitive.indices[i + 2u];
                const glm::vec3 n = glm::cross(primitive.vertices[b].position - primitive.vertices[a].position, primitive.vertices[c].position - primitive.vertices[a].position);
                accumulated[a] += n; accumulated[b] += n; accumulated[c] += n;
            }
            for (std::size_t i = 0; i < primitive.vertices.size(); ++i)
                primitive.vertices[i].normal = glm::dot(accumulated[i], accumulated[i]) > 1e-12f ? glm::normalize(accumulated[i]) : glm::vec3(0, 0, 1);
        }
        scene.primitives.push_back(std::move(primitive));
        scene.nodes.push_back(StaticSceneNode{"Lucy", glm::mat4(1), glm::mat4(1), -1, {0}});
        return Halcyon::Result<StaticScene>::success(std::move(scene));
    }
    catch (const std::exception& error)
    {
        return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, error.what(), path.string()});
    }
}

} // namespace Halcyon::Renderer::Scene
