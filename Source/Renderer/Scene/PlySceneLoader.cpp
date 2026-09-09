#define TINYPLY_IMPLEMENTATION
#include <tinyply.h>

#include "PlySceneLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>

namespace Halcyon::Renderer::Scene
{
namespace
{

template <typename T>
T readScalar(const std::uint8_t* bytes, std::size_t index)
{
    T value{};
    std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
    return value;
}

float readNumber(const tinyply::PlyData& data, std::size_t index)
{
    const auto* bytes = data.buffer.get_const();
    switch (data.t)
    {
    case tinyply::Type::FLOAT32: return readScalar<float>(bytes, index);
    case tinyply::Type::FLOAT64: return static_cast<float>(readScalar<double>(bytes, index));
    case tinyply::Type::INT8: return static_cast<float>(readScalar<std::int8_t>(bytes, index));
    case tinyply::Type::UINT8: return static_cast<float>(bytes[index]);
    case tinyply::Type::INT16: return static_cast<float>(readScalar<std::int16_t>(bytes, index));
    case tinyply::Type::UINT16: return static_cast<float>(readScalar<std::uint16_t>(bytes, index));
    case tinyply::Type::INT32: return static_cast<float>(readScalar<std::int32_t>(bytes, index));
    case tinyply::Type::UINT32: return static_cast<float>(readScalar<std::uint32_t>(bytes, index));
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
    case tinyply::Type::INT8: value = static_cast<std::uint64_t>(readScalar<std::int8_t>(bytes, index)); break;
    case tinyply::Type::UINT16: value = readScalar<std::uint16_t>(bytes, index); break;
    case tinyply::Type::INT16: value = static_cast<std::uint64_t>(readScalar<std::int16_t>(bytes, index)); break;
    case tinyply::Type::UINT32: value = readScalar<std::uint32_t>(bytes, index); break;
    case tinyply::Type::INT32: value = static_cast<std::uint64_t>(readScalar<std::int32_t>(bytes, index)); break;
    default: throw std::runtime_error("unsupported PLY index type");
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("PLY index exceeds uint32");
    return static_cast<std::uint32_t>(value);
}

std::size_t indexStride(tinyply::Type type)
{
    switch (type)
    {
    case tinyply::Type::INT8:
    case tinyply::Type::UINT8: return 1u;
    case tinyply::Type::INT16:
    case tinyply::Type::UINT16: return 2u;
    case tinyply::Type::INT32:
    case tinyply::Type::UINT32: return 4u;
    default: throw std::runtime_error("unsupported PLY index type");
    }
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
                bool hasNormalX = false;
                bool hasNormalY = false;
                bool hasNormalZ = false;
                for (const auto& property : element.properties)
                {
                    hasNormalX = hasNormalX || property.name == "nx";
                    hasNormalY = hasNormalY || property.name == "ny";
                    hasNormalZ = hasNormalZ || property.name == "nz";
                }
                // Request position and normal properties separately. tinyply
                // packs a multi-property request into one interleaved stream;
                // treating that stream as two independent xyz arrays would
                // make every normal read from the wrong byte offset.
                positions = file.request_properties_from_element("vertex", {"x", "y", "z"});
                if (hasNormalX && hasNormalY && hasNormalZ)
                {
                    normals = file.request_properties_from_element("vertex", {"nx", "ny", "nz"});
                }
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
            if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
                !std::isfinite(vertex.position.z))
                return Halcyon::Result<StaticScene>::failure(
                    {Halcyon::ErrorCode::InvalidArgument, "PLY contains a non-finite vertex", path.string()});
            if (normals && normals->count >= positions->count)
            {
                const glm::vec3 sourceNormal{
                    readNumber(*normals, i * 3u),
                    readNumber(*normals, i * 3u + 1u),
                    readNumber(*normals, i * 3u + 2u)};
                if (!std::isfinite(sourceNormal.x) || !std::isfinite(sourceNormal.y) ||
                    !std::isfinite(sourceNormal.z))
                    return Halcyon::Result<StaticScene>::failure(
                        {Halcyon::ErrorCode::InvalidArgument,
                            "PLY contains a non-finite normal", path.string()});
                vertex.normal = glm::dot(sourceNormal, sourceNormal) > 1.0e-12f
                    ? glm::normalize(sourceNormal) : glm::vec3{0.0f, 0.0f, 1.0f};
            }
            minimum = glm::min(minimum, vertex.position); maximum = glm::max(maximum, vertex.position);
        }
        primitive.boundsMin = minimum; primitive.boundsMax = maximum;
        // tinyply stores variable lists consecutively; triangulate using the list offsets.
        const std::size_t faceStride = indexStride(faces->t);
        if (faces->buffer.size_bytes() % faceStride != 0u)
            return Halcyon::Result<StaticScene>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "PLY face buffer has a partial index", path.string()});
        const std::size_t faceIndexCount = faces->buffer.size_bytes() / faceStride;
        if (!faces->list_sizes.empty() && faces->list_sizes.size() != faces->count)
            return Halcyon::Result<StaticScene>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "PLY face list metadata is incomplete", path.string()});
        const std::size_t fixedFaceSize = faces->list_sizes.empty()
            ? (faces->count != 0u && faceIndexCount % faces->count == 0u
                    ? faceIndexCount / faces->count : 0u)
            : 0u;
        if (faces->list_sizes.empty() && fixedFaceSize == 0u)
            return Halcyon::Result<StaticScene>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "PLY fixed face list has an invalid size", path.string()});
        std::size_t cursor = 0;
        for (std::size_t face = 0; face < faces->count; ++face)
        {
            const std::size_t count = faces->list_sizes.empty()
                ? fixedFaceSize : faces->list_sizes[face];
            if (count > faceIndexCount - cursor)
                return Halcyon::Result<StaticScene>::failure(
                    {Halcyon::ErrorCode::InvalidArgument,
                        "PLY face list exceeds its index buffer", path.string()});
            if (count >= 3u)
            {
                const auto first = readIndex(*faces, cursor);
                for (std::size_t corner = 1; corner + 1u < count; ++corner)
                {
                    const auto second = readIndex(*faces, cursor + corner);
                    const auto third = readIndex(*faces, cursor + corner + 1u);
                    if (first >= primitive.vertices.size() ||
                        second >= primitive.vertices.size() || third >= primitive.vertices.size())
                        return Halcyon::Result<StaticScene>::failure(
                            {Halcyon::ErrorCode::InvalidArgument,
                                "PLY face references a missing vertex", path.string()});
                    primitive.indices.insert(primitive.indices.end(), {first, second, third});
                }
            }
            cursor += count;
        }
        if (cursor != faceIndexCount)
            return Halcyon::Result<StaticScene>::failure(
                {Halcyon::ErrorCode::InvalidArgument,
                    "PLY face index buffer has trailing values", path.string()});
        if (primitive.indices.empty()) return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "PLY has no triangles", path.string()});
        if ((!normals || normals->count < positions->count) && options.generateMissingNormals)
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
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::OutOfMemory,
                "unable to allocate PLY geometry", path.string()});
    }
    catch (const std::exception& error)
    {
        return Halcyon::Result<StaticScene>::failure(
            {Halcyon::ErrorCode::InvalidArgument, error.what(), path.string()});
    }
}

} // namespace Halcyon::Renderer::Scene
