#include "VirtualGeometry.h"

#include "PlySceneLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <meshoptimizer.h>
#include <limits>
#include <numeric>
#include <new>
#include <string>
#include <utility>

namespace Halcyon::Renderer::Scene
{
namespace
{

// Visibility IDs reserve zero for background and carry meshletIndex + 1 in
// twenty bits.  Reject oversized assets during cooking instead of silently
// dropping meshlets at the raster submission boundary.
constexpr std::size_t kMaxVisibilityMeshlets = kVirtualVisibilityMeshletMask;

Halcyon::Result<VirtualGeometryAsset> failure(std::string message)
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometry"});
}

Halcyon::Result<VirtualGeometryAsset> allocationFailure()
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate virtual geometry tables", "VirtualGeometry"});
}

} // namespace

Halcyon::Result<VirtualGeometryAsset> buildVirtualGeometry(
    const StaticScene& scene, const VirtualGeometryBuildOptions& options)
{
    if (scene.primitives.empty()) return failure("scene contains no primitives");
    if (scene.primitives.size() > std::numeric_limits<std::uint32_t>::max())
        return failure("scene primitive count exceeds the virtual geometry integer range");
    if (options.maxVertices < 3u || options.maxVertices > kVirtualGeometryMaxVertices ||
        options.maxTriangles == 0u || options.maxTriangles > kVirtualGeometryMaxTriangles ||
        options.maxTriangles % 4u != 0u)
        return failure("meshlet limits are outside the M5 64/124 bounds");
    if (!std::isfinite(options.simplifyError) || options.simplifyError < 0.0f)
        return failure("LOD simplify error must be finite and non-negative");
    bool disabledLodSeen = false;
    float previousRatio = 1.0f;
    for (std::size_t lodIndex = 0; lodIndex < options.lodRatios.size(); ++lodIndex)
    {
        const float ratio = options.lodRatios[lodIndex];
        if (!std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f ||
            (lodIndex == 0u && ratio != 1.0f) || ratio > previousRatio ||
            (disabledLodSeen && ratio != 0.0f))
            return failure("LOD ratios must start at 1, descend, and use only trailing zeros");
        disabledLodSeen = disabledLodSeen || ratio == 0.0f;
        previousRatio = ratio;
    }
    try
    {
    VirtualGeometryAsset asset;
    for (std::size_t primitiveIndex = 0; primitiveIndex < scene.primitives.size(); ++primitiveIndex)
    {
        const auto& primitive = scene.primitives[primitiveIndex];
        if (primitive.vertices.empty() || primitive.indices.empty() || primitive.indices.size() % 3u != 0u)
            return failure("primitive has invalid geometry");
        if (primitive.materialIndex > kVirtualVisibilityMaterialIndexMask)
            return failure("primitive material index exceeds the visibility ABI range");
        if (primitive.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            primitive.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            primitive.vertices.size() > std::numeric_limits<std::size_t>::max() / 3u ||
            asset.vertices.size() > std::numeric_limits<std::uint32_t>::max() -
                primitive.vertices.size())
            return failure("primitive geometry exceeds the virtual geometry integer range");
        const auto finiteVec3 = [](const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        };
        const auto finiteVec4 = [](const glm::vec4& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z) && std::isfinite(value.w);
        };
        if (!finiteVec3(primitive.boundsMin) || !finiteVec3(primitive.boundsMax) ||
            glm::any(glm::greaterThan(primitive.boundsMin, primitive.boundsMax)) ||
            std::any_of(primitive.vertices.begin(), primitive.vertices.end(),
                [&](const StaticSceneVertex& vertex)
                {
                    return !finiteVec3(vertex.position) || !finiteVec3(vertex.normal) ||
                        !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y) ||
                        !finiteVec4(vertex.tangent);
                }))
            return failure("primitive contains invalid bounds or non-finite vertex data");
        if (std::any_of(primitive.indices.begin(), primitive.indices.end(),
                [&](std::uint32_t index) { return index >= primitive.vertices.size(); }))
            return failure("primitive index references a missing vertex");
        const std::uint32_t vertexBase = static_cast<std::uint32_t>(asset.vertices.size());
        asset.vertices.insert(asset.vertices.end(), primitive.vertices.begin(), primitive.vertices.end());
        asset.primitives.push_back({vertexBase, static_cast<std::uint32_t>(primitive.vertices.size()),
            primitive.materialIndex, primitive.boundsMin, primitive.boundsMax});
        std::vector<float> positions(primitive.vertices.size() * 3u);
        for (std::size_t i = 0; i < primitive.vertices.size(); ++i)
        {
            positions[i * 3u] = primitive.vertices[i].position.x;
            positions[i * 3u + 1u] = primitive.vertices[i].position.y;
            positions[i * 3u + 2u] = primitive.vertices[i].position.z;
        }
        for (std::size_t lodIndex = 0; lodIndex < options.lodRatios.size(); ++lodIndex)
        {
            const float ratio = options.lodRatios[lodIndex];
            if (ratio == 0.0f) continue;
            const std::size_t target = std::max<std::size_t>(3u,
                static_cast<std::size_t>(primitive.indices.size() * ratio));
            std::vector<std::uint32_t> lodIndices(primitive.indices.size());
            float error = 0.0f;
            const std::size_t count = lodIndex == 0
                ? primitive.indices.size()
                : meshopt_simplify(lodIndices.data(), primitive.indices.data(), primitive.indices.size(),
                    positions.data(), primitive.vertices.size(), sizeof(float) * 3u, target,
                    options.simplifyError, 0u, &error);
            if (count < 3u)
                return failure("meshoptimizer could not produce the requested LOD");
            if (!std::isfinite(error) || error < 0.0f)
                return failure("meshoptimizer returned an invalid LOD error");
            if (lodIndex == 0) lodIndices = primitive.indices;
            lodIndices.resize(count - count % 3u);
            const std::size_t bound = meshopt_buildMeshletsBound(lodIndices.size(), options.maxVertices, options.maxTriangles);
            if (bound == 0u ||
                bound > std::numeric_limits<std::size_t>::max() / options.maxVertices ||
                bound > std::numeric_limits<std::size_t>::max() /
                    (static_cast<std::size_t>(options.maxTriangles) * 3u))
                return failure("meshlet output capacity exceeds the host address range");
            std::vector<meshopt_Meshlet> meshlets(bound);
            // meshopt_buildMeshlets may pad each meshlet's triangle stream to
            // a four-byte boundary. Its output contract therefore requires
            // capacity based on the meshlet bound and configured limits, not
            // merely the input index count.
            std::vector<unsigned int> meshletVertices(
                bound * static_cast<std::size_t>(options.maxVertices));
            std::vector<unsigned char> meshletTriangles(bound *
                static_cast<std::size_t>(options.maxTriangles) * 3u);
            const std::size_t meshletCount = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
                lodIndices.data(), lodIndices.size(), positions.data(), primitive.vertices.size(), sizeof(float) * 3u,
                options.maxVertices, options.maxTriangles, 0.0f);
            if (meshletCount == 0u)
                return failure("meshoptimizer could not build a meshlet for the requested LOD");
            std::size_t coveredTriangles = 0;
            for (std::size_t m = 0; m < meshletCount; ++m)
            {
                const auto& source = meshlets[m];
                if (source.vertex_offset > meshletVertices.size() ||
                    source.vertex_count > meshletVertices.size() - source.vertex_offset ||
                    source.triangle_offset > meshletTriangles.size() ||
                    static_cast<std::size_t>(source.triangle_count) * 3u >
                        meshletTriangles.size() - source.triangle_offset)
                    return failure("meshoptimizer returned an out-of-range meshlet span");
                if (source.triangle_count > lodIndices.size() / 3u -
                        std::min(coveredTriangles, lodIndices.size() / 3u))
                    return failure("meshlet triangle coverage exceeds the LOD input");
                coveredTriangles += source.triangle_count;
                for (std::size_t i = 0; i < static_cast<std::size_t>(source.triangle_count) * 3u; ++i)
                {
                    if (meshletTriangles[source.triangle_offset + i] >= source.vertex_count)
                        return failure("meshoptimizer returned an invalid local triangle index");
                }
            }
            if (coveredTriangles != lodIndices.size() / 3u)
                return failure("meshlet build did not cover every LOD triangle exactly once");
            if (asset.meshlets.size() > kMaxVisibilityMeshlets ||
                meshletCount > kMaxVisibilityMeshlets - asset.meshlets.size())
                return failure("meshoptimizer generated more meshlets than the M5 visibility ABI can encode");
            if (asset.lods.size() >= std::numeric_limits<std::uint32_t>::max())
                return failure("LOD table exceeds the virtual geometry integer range");
            VirtualGeometryLod lod{};
            if (asset.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
                asset.meshletVertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                asset.meshletTriangles.size() > std::numeric_limits<std::uint32_t>::max())
                return failure("virtual geometry table exceeds the cache integer range");
            lod.primitiveIndex = static_cast<std::uint32_t>(primitiveIndex);
            lod.meshletOffset = static_cast<std::uint32_t>(asset.meshlets.size());
            lod.meshletCount = static_cast<std::uint32_t>(meshletCount);
            lod.indexOffset = static_cast<std::uint32_t>(asset.indices.size());
            lod.geometricError = error;
            lod.ratio = ratio;
            for (std::size_t m = 0; m < meshletCount; ++m)
            {
                const auto& source = meshlets[m];
                const std::size_t triangleIndexCount =
                    static_cast<std::size_t>(source.triangle_count) * 3u;
                if (source.vertex_count > options.maxVertices ||
                    source.triangle_count > options.maxTriangles ||
                    asset.meshletVertices.size() >
                        std::numeric_limits<std::uint32_t>::max() - source.vertex_count ||
                    asset.meshletTriangles.size() >
                        std::numeric_limits<std::uint32_t>::max() - triangleIndexCount ||
                    asset.indices.size() >
                        std::numeric_limits<std::uint32_t>::max() - triangleIndexCount)
                    return failure("meshlet table exceeds the virtual geometry integer range");
                const auto bounds = meshopt_computeMeshletBounds(meshletVertices.data() + source.vertex_offset,
                    meshletTriangles.data() + source.triangle_offset, source.triangle_count,
                    positions.data(), primitive.vertices.size(), sizeof(float) * 3u);
                VirtualGeometryMeshlet record{};
                record.vertexOffset = static_cast<std::uint32_t>(asset.meshletVertices.size());
                record.vertexCount = source.vertex_count;
                record.triangleOffset = static_cast<std::uint32_t>(asset.meshletTriangles.size());
                record.triangleCount = source.triangle_count;
                record.indexOffset = static_cast<std::uint32_t>(asset.indices.size());
                record.indexCount = static_cast<std::uint32_t>(triangleIndexCount);
                record.primitiveIndex = static_cast<std::uint32_t>(primitiveIndex);
                record.lodIndex = static_cast<std::uint32_t>(lodIndex);
                record.sphere = {bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
                record.cone = {bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff};
                record.geometricError = error;
                for (std::size_t vertex = 0; vertex < source.vertex_count; ++vertex)
                    asset.meshletVertices.push_back(
                        vertexBase + meshletVertices[source.vertex_offset + vertex]);
                asset.meshletTriangles.insert(asset.meshletTriangles.end(), meshletTriangles.begin() + source.triangle_offset,
                    meshletTriangles.begin() + source.triangle_offset + source.triangle_count * 3u);
                for (std::size_t i = 0; i < source.triangle_count * 3u; ++i)
                    asset.indices.push_back(vertexBase + meshletVertices[source.vertex_offset + meshletTriangles[source.triangle_offset + i]]);
                lod.indexCount += record.indexCount;
                asset.meshlets.push_back(record);
            }
            asset.lods.push_back(lod);
        }
    }
    if (asset.meshlets.empty()) return failure("meshoptimizer generated no meshlets");
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
    }
    catch (const std::bad_alloc&)
    {
        return allocationFailure();
    }
}

Halcyon::Result<StaticScene> loadGeometrySource(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".ply") return loadPlyScene(path, options);
    return loadStaticScene(path, options);
}

} // namespace Halcyon::Renderer::Scene
