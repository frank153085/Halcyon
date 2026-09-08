#include "VirtualGeometry.h"

#include "PlySceneLoader.h"

#include <algorithm>
#include <cmath>
#include <meshoptimizer.h>
#include <numeric>

namespace Halcyon::Renderer::Scene
{
namespace
{

Halcyon::Result<VirtualGeometryAsset> failure(std::string message)
{
    return Halcyon::Result<VirtualGeometryAsset>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometry"});
}

} // namespace

Halcyon::Result<VirtualGeometryAsset> buildVirtualGeometry(
    const StaticScene& scene, const VirtualGeometryBuildOptions& options)
{
    if (scene.primitives.empty()) return failure("scene contains no primitives");
    if (options.maxVertices == 0 || options.maxVertices > 256u || options.maxTriangles == 0 ||
        options.maxTriangles > 512u) return failure("meshlet limits are outside meshoptimizer bounds");
    VirtualGeometryAsset asset;
    for (std::size_t primitiveIndex = 0; primitiveIndex < scene.primitives.size(); ++primitiveIndex)
    {
        const auto& primitive = scene.primitives[primitiveIndex];
        if (primitive.vertices.empty() || primitive.indices.empty() || primitive.indices.size() % 3u != 0u)
            return failure("primitive has invalid geometry");
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
            const float ratio = std::clamp(options.lodRatios[lodIndex], 0.0f, 1.0f);
            const std::size_t target = std::max<std::size_t>(3u,
                static_cast<std::size_t>(primitive.indices.size() * ratio));
            std::vector<std::uint32_t> lodIndices(primitive.indices.size());
            float error = 0.0f;
            const std::size_t count = lodIndex == 0
                ? primitive.indices.size()
                : meshopt_simplify(lodIndices.data(), primitive.indices.data(), primitive.indices.size(),
                    positions.data(), primitive.vertices.size(), sizeof(float) * 3u, target,
                    options.simplifyError, 0u, &error);
            if (count < 3u) continue;
            if (lodIndex == 0) lodIndices = primitive.indices;
            lodIndices.resize(count - count % 3u);
            const std::size_t bound = meshopt_buildMeshletsBound(lodIndices.size(), options.maxVertices, options.maxTriangles);
            std::vector<meshopt_Meshlet> meshlets(bound);
            std::vector<unsigned int> meshletVertices(lodIndices.size());
            std::vector<unsigned char> meshletTriangles(lodIndices.size());
            const std::size_t meshletCount = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
                lodIndices.data(), lodIndices.size(), positions.data(), primitive.vertices.size(), sizeof(float) * 3u,
                options.maxVertices, options.maxTriangles, 0.0f);
            VirtualGeometryLod lod{};
            lod.primitiveIndex = static_cast<std::uint32_t>(primitiveIndex);
            lod.meshletOffset = static_cast<std::uint32_t>(asset.meshlets.size());
            lod.meshletCount = static_cast<std::uint32_t>(meshletCount);
            lod.indexOffset = static_cast<std::uint32_t>(asset.indices.size());
            lod.geometricError = error;
            lod.ratio = ratio;
            for (std::size_t m = 0; m < meshletCount; ++m)
            {
                const auto& source = meshlets[m];
                const auto bounds = meshopt_computeMeshletBounds(meshletVertices.data() + source.vertex_offset,
                    meshletTriangles.data() + source.triangle_offset, source.triangle_count,
                    positions.data(), primitive.vertices.size(), sizeof(float) * 3u);
                VirtualGeometryMeshlet record{};
                record.vertexOffset = static_cast<std::uint32_t>(asset.meshletVertices.size());
                record.vertexCount = source.vertex_count;
                record.triangleOffset = static_cast<std::uint32_t>(asset.meshletTriangles.size());
                record.triangleCount = source.triangle_count;
                record.indexOffset = static_cast<std::uint32_t>(asset.indices.size());
                record.indexCount = source.triangle_count * 3u;
                record.primitiveIndex = static_cast<std::uint32_t>(primitiveIndex);
                record.lodIndex = static_cast<std::uint32_t>(lodIndex);
                record.sphere = {bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
                record.cone = {bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff};
                record.geometricError = error;
                asset.meshletVertices.insert(asset.meshletVertices.end(), meshletVertices.begin() + source.vertex_offset,
                    meshletVertices.begin() + source.vertex_offset + source.vertex_count);
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

Halcyon::Result<StaticScene> loadGeometrySource(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options)
{
    const auto extension = path.extension().string();
    if (extension == ".ply" || extension == ".PLY") return loadPlyScene(path, options);
    return loadStaticScene(path, options);
}

} // namespace Halcyon::Renderer::Scene
