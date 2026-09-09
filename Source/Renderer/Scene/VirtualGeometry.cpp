#include "VirtualGeometry.h"

#include "PlySceneLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <meshoptimizer.h>
#include <metis.h>
#include <limits>
#include <numeric>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

Halcyon::Result<void> dagFailure(std::string message)
{
    return Halcyon::Result<void>::failure(
        {Halcyon::ErrorCode::InvalidArgument, std::move(message), "VirtualGeometryDAG"});
}

glm::vec4 clusterSphere(const VirtualGeometryAsset& asset,
    const VirtualGeometryCluster& cluster) noexcept
{
    glm::vec3 center(0.0f);
    float radius = 0.0f;
    if (cluster.meshletIndices.empty()) return glm::vec4(0.0f);
    for (const auto meshletIndex : cluster.meshletIndices)
        center += glm::vec3(asset.meshlets[meshletIndex].sphere);
    center /= static_cast<float>(cluster.meshletIndices.size());
    for (const auto meshletIndex : cluster.meshletIndices)
    {
        const auto& sphere = asset.meshlets[meshletIndex].sphere;
        radius = std::max(radius, glm::length(glm::vec3(sphere) - center) + sphere.w);
    }
    return glm::vec4(center, radius);
}

bool finiteSelectionOptions(const VirtualGeometryBuildOptions& options) noexcept
{
    return options.maxClusterVertices >= 3u && options.maxClusterTriangles >= 3u &&
        options.maxDagDepth > 0u && options.metisUfactor > 0u && options.metisUfactor <= 1000u &&
        std::isfinite(options.lodRefineThresholdPixels) &&
        std::isfinite(options.lodCoarsenThresholdPixels) &&
        options.lodCoarsenThresholdPixels >= 0.0f &&
        options.lodRefineThresholdPixels > options.lodCoarsenThresholdPixels;
}

} // namespace

static Halcyon::Result<VirtualGeometryAsset> buildVirtualGeometryPass(
    const StaticScene& scene, const VirtualGeometryBuildOptions& options,
    const std::vector<std::vector<unsigned char>>* vertexLocks)
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
            std::size_t count = primitive.indices.size();
            if (lodIndex != 0u)
            {
                const unsigned char* lockData = nullptr;
                if (vertexLocks != nullptr && primitiveIndex < vertexLocks->size() &&
                    (*vertexLocks)[primitiveIndex].size() == primitive.vertices.size())
                    lockData = (*vertexLocks)[primitiveIndex].data();
                count = lockData == nullptr
                    ? meshopt_simplify(lodIndices.data(), primitive.indices.data(),
                        primitive.indices.size(), positions.data(), primitive.vertices.size(),
                        sizeof(float) * 3u, target, options.simplifyError,
                        meshopt_SimplifyLockBorder, &error)
                    : meshopt_simplifyWithAttributes(lodIndices.data(), primitive.indices.data(),
                        primitive.indices.size(), positions.data(), primitive.vertices.size(),
                        sizeof(float) * 3u, nullptr, 0u, nullptr, 0u, lockData, target,
                        options.simplifyError, meshopt_SimplifyLockBorder, &error);
            }
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
    const auto dag = buildVirtualGeometryDag(asset, options);
    if (!dag) return Halcyon::Result<VirtualGeometryAsset>::failure(dag.error());
    return Halcyon::Result<VirtualGeometryAsset>::success(std::move(asset));
    }
    catch (const std::bad_alloc&)
    {
        return allocationFailure();
    }
}

Halcyon::Result<VirtualGeometryAsset> buildVirtualGeometry(
    const StaticScene& scene, const VirtualGeometryBuildOptions& options)
{
    std::vector<std::vector<unsigned char>> locks;
    try
    {
        locks.reserve(scene.primitives.size());
        for (const auto& primitive : scene.primitives)
            locks.emplace_back(primitive.vertices.size(), 0u);
        // Keep the provisional asset in a nested scope. Lucy-sized inputs can
        // otherwise retain the complete unlocked result while the locked pass
        // allocates another full set of vertices, LOD indices and meshlets.
        auto provisional = buildVirtualGeometryPass(scene, options, nullptr);
        if (!provisional) return provisional;
        bool hasClusterBoundary = false;
        const auto& asset = provisional.value();
        for (const auto& cluster : asset.clusters)
        {
            if (cluster.lodDepth != 0u || cluster.primitiveIndex >= locks.size()) continue;
            const auto& primitive = asset.primitives[cluster.primitiveIndex];
            for (const std::uint32_t vertex : cluster.boundaryVertices)
            {
                if (vertex < primitive.vertexOffset ||
                    vertex - primitive.vertexOffset >= locks[cluster.primitiveIndex].size())
                    return failure("fine cluster boundary vertex is outside its primitive");
                locks[cluster.primitiveIndex][vertex - primitive.vertexOffset] =
                    meshopt_SimplifyVertex_Lock;
                hasClusterBoundary = true;
            }
        }
        if (!hasClusterBoundary) return provisional;
    }
    catch (const std::bad_alloc&)
    {
        return allocationFailure();
    }
    return buildVirtualGeometryPass(scene, options, &locks);
}

Halcyon::Result<void> buildVirtualGeometryDag(
    VirtualGeometryAsset& asset, const VirtualGeometryBuildOptions& options)
{
    if (!finiteSelectionOptions(options))
        return dagFailure("M6 cluster and screen-error options are invalid");
    if (asset.meshlets.empty()) return dagFailure("cannot build a DAG without meshlets");

    try
    {
        asset.clusters.clear();
        asset.dagNodes.clear();
        asset.dagEdges.clear();

        // Partition each contiguous M5 LOD range. The meshlet adjacency graph
        // is deterministic: vertices are inserted in table order and edges
        // are emitted in sorted meshlet order.
        for (std::uint32_t lodIndex = 0; lodIndex < asset.lods.size(); ++lodIndex)
        {
            const auto& lod = asset.lods[lodIndex];
            const std::uint32_t count = lod.meshletCount;
            if (count == 0u) continue;
            std::vector<std::vector<idx_t>> adjacency(count);
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> owners;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto& meshlet = asset.meshlets[lod.meshletOffset + i];
                for (std::uint32_t v = 0; v < meshlet.vertexCount; ++v)
                    owners[asset.meshletVertices[meshlet.vertexOffset + v]].push_back(i);
            }
            for (const auto& [vertex, users] : owners)
            {
                (void)vertex;
                for (std::size_t a = 0; a < users.size(); ++a)
                    for (std::size_t b = a + 1; b < users.size(); ++b)
                    {
                        adjacency[users[a]].push_back(static_cast<idx_t>(users[b]));
                        adjacency[users[b]].push_back(static_cast<idx_t>(users[a]));
                    }
            }
            std::vector<idx_t> xadj(count + 1u, 0), adjncy;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto& row = adjacency[i];
                std::sort(row.begin(), row.end());
                row.erase(std::unique(row.begin(), row.end()), row.end());
                xadj[i + 1u] = xadj[i] + static_cast<idx_t>(row.size());
                adjncy.insert(adjncy.end(), row.begin(), row.end());
            }
            const std::uint32_t targetTriangles = std::max(1u, options.maxClusterTriangles);
            const std::uint32_t estimatedParts = std::max(1u,
                (lod.indexCount / 3u + targetTriangles - 1u) / targetTriangles);
            const idx_t nparts = static_cast<idx_t>(std::min<std::uint32_t>(count, estimatedParts));
            std::vector<idx_t> partition(count, 0);
            idx_t vertices = static_cast<idx_t>(count);
            idx_t parts = nparts;
            idx_t status = METIS_OK;
            // METIS cannot bisect a singleton graph or produce one partition
            // per vertex. Keep those deterministic cases on the modulo path
            // to avoid noisy diagnostics and undefined partition quality.
            if (nparts > 1 && nparts < vertices && !adjncy.empty())
            {
                idx_t optionsArray[METIS_NOPTIONS];
                METIS_SetDefaultOptions(optionsArray);
                optionsArray[METIS_OPTION_SEED] = static_cast<idx_t>(options.metisSeed);
                optionsArray[METIS_OPTION_UFACTOR] = static_cast<idx_t>(options.metisUfactor);
                optionsArray[METIS_OPTION_NUMBERING] = 0;
                idx_t edgecut = 0;
                idx_t constraints = 1;
                status = METIS_PartGraphRecursive(&vertices, &constraints, xadj.data(),
                    adjncy.data(), nullptr, nullptr, nullptr, &parts, nullptr, nullptr,
                    optionsArray, &edgecut, partition.data());
            }
            if (status != METIS_OK)
                for (std::uint32_t i = 0; i < count; ++i)
                    partition[i] = static_cast<idx_t>(i % static_cast<std::uint32_t>(nparts));
            std::vector<std::vector<std::uint32_t>> groups(static_cast<std::size_t>(nparts));
            for (std::uint32_t i = 0; i < count; ++i)
                groups[static_cast<std::size_t>(partition[i]) % groups.size()].push_back(i);
            std::vector<std::vector<std::uint32_t>> boundedGroups;
            for (const auto& group : groups)
            {
                std::vector<std::uint32_t> bounded;
                std::unordered_set<std::uint32_t> boundedVertices;
                std::uint32_t boundedTriangles = 0;
                for (const auto local : group)
                {
                    const auto& meshlet = asset.meshlets[lod.meshletOffset + local];
                    std::unordered_set<std::uint32_t> candidateVertices = boundedVertices;
                    for (std::uint32_t v = 0; v < meshlet.vertexCount; ++v)
                        candidateVertices.insert(asset.meshletVertices[meshlet.vertexOffset + v]);
                    const bool exceeds = !bounded.empty() &&
                        (boundedTriangles + meshlet.triangleCount > options.maxClusterTriangles ||
                         candidateVertices.size() > options.maxClusterVertices);
                    if (exceeds)
                    {
                        boundedGroups.push_back(std::move(bounded));
                        bounded.clear();
                        boundedVertices.clear();
                        boundedTriangles = 0;
                    }
                    bounded.push_back(local);
                    boundedTriangles += meshlet.triangleCount;
                    for (std::uint32_t v = 0; v < meshlet.vertexCount; ++v)
                        boundedVertices.insert(asset.meshletVertices[meshlet.vertexOffset + v]);
                }
                if (!bounded.empty()) boundedGroups.push_back(std::move(bounded));
            }
            const std::uint32_t clusterBegin =
                static_cast<std::uint32_t>(asset.clusters.size());
            std::vector<std::uint32_t> meshletClusters(count,
                std::numeric_limits<std::uint32_t>::max());
            for (const auto& group : boundedGroups)
            {
                VirtualGeometryCluster cluster{};
                cluster.meshletOffset = 0u;
                cluster.meshletCount = static_cast<std::uint32_t>(group.size());
                cluster.lodDepth = lodIndex;
                cluster.primitiveIndex = lod.primitiveIndex;
                cluster.geometricError = lod.geometricError;
                cluster.meshletIndices.reserve(group.size());
                std::unordered_set<std::uint32_t> verticesInCluster;
                for (const auto local : group)
                {
                    meshletClusters[local] = static_cast<std::uint32_t>(asset.clusters.size());
                    cluster.meshletIndices.push_back(lod.meshletOffset + local);
                    const auto& meshlet = asset.meshlets[lod.meshletOffset + local];
                    cluster.triangleCount += meshlet.triangleCount;
                    for (std::uint32_t v = 0; v < meshlet.vertexCount; ++v)
                        verticesInCluster.insert(asset.meshletVertices[meshlet.vertexOffset + v]);
                }
                cluster.meshletOffset = cluster.meshletIndices.front();
                cluster.vertexCount = static_cast<std::uint32_t>(verticesInCluster.size());
                if (!verticesInCluster.empty())
                    cluster.vertexOffset = *std::min_element(verticesInCluster.begin(), verticesInCluster.end());
                cluster.sphere = clusterSphere(asset, cluster);
                asset.clusters.push_back(std::move(cluster));
            }
            // Reuse the meshlet vertex-owner table to derive final bounded
            // cluster boundaries and adjacency. This avoids two additional
            // Lucy-sized unordered maps and keeps only one LOD resident.
            for (const auto& [vertex, users] : owners)
            {
                std::vector<std::uint32_t> sharingClusters;
                sharingClusters.reserve(users.size());
                for (const auto local : users)
                    if (local < meshletClusters.size() &&
                        meshletClusters[local] != std::numeric_limits<std::uint32_t>::max())
                        sharingClusters.push_back(meshletClusters[local]);
                std::sort(sharingClusters.begin(), sharingClusters.end());
                sharingClusters.erase(std::unique(sharingClusters.begin(),
                    sharingClusters.end()), sharingClusters.end());
                if (sharingClusters.size() < 2u) continue;
                for (const auto clusterIndex : sharingClusters)
                    asset.clusters[clusterIndex].boundaryVertices.push_back(vertex);
                for (std::size_t a = 0; a < sharingClusters.size(); ++a)
                    for (std::size_t b = a + 1u; b < sharingClusters.size(); ++b)
                    {
                        asset.clusters[sharingClusters[a]].adjacentClusters.push_back(
                            sharingClusters[b]);
                        asset.clusters[sharingClusters[b]].adjacentClusters.push_back(
                            sharingClusters[a]);
                    }
            }
            for (std::uint32_t clusterIndex = clusterBegin;
                clusterIndex < asset.clusters.size(); ++clusterIndex)
            {
                auto& cluster = asset.clusters[clusterIndex];
                std::sort(cluster.boundaryVertices.begin(), cluster.boundaryVertices.end());
                cluster.boundaryVertices.erase(std::unique(cluster.boundaryVertices.begin(),
                    cluster.boundaryVertices.end()), cluster.boundaryVertices.end());
                std::sort(cluster.adjacentClusters.begin(), cluster.adjacentClusters.end());
                cluster.adjacentClusters.erase(std::unique(cluster.adjacentClusters.begin(),
                    cluster.adjacentClusters.end()), cluster.adjacentClusters.end());
            }
        }

        for (std::uint32_t i = 0; i < asset.clusters.size(); ++i)
        {
            const auto& cluster = asset.clusters[i];
            asset.dagNodes.push_back({i, std::numeric_limits<std::uint32_t>::max(), 0u, 0u,
                cluster.lodDepth, 0u, cluster.sphere, cluster.geometricError});
        }
        std::vector<std::vector<std::uint32_t>> children(asset.dagNodes.size());
        for (std::uint32_t childIndex = 0; childIndex < asset.clusters.size(); ++childIndex)
        {
            const auto& child = asset.clusters[childIndex];
            if (child.lodDepth + 1u >= options.maxDagDepth) continue;
            std::uint32_t bestParent = std::numeric_limits<std::uint32_t>::max();
            float bestDistance = std::numeric_limits<float>::infinity();
            for (std::uint32_t parentIndex = childIndex + 1u;
                parentIndex < asset.clusters.size(); ++parentIndex)
            {
                const auto& parent = asset.clusters[parentIndex];
                if (parent.primitiveIndex != child.primitiveIndex ||
                    parent.lodDepth != child.lodDepth + 1u)
                    continue;
                const glm::vec3 delta = glm::vec3(parent.sphere) - glm::vec3(child.sphere);
                const float distance = glm::dot(delta, delta);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestParent = parentIndex;
                }
            }
            if (bestParent != std::numeric_limits<std::uint32_t>::max())
            {
                asset.dagNodes[childIndex].parentIndex = bestParent;
                children[bestParent].push_back(childIndex);
            }
        }
        for (std::uint32_t parentIndex = 0; parentIndex < asset.dagNodes.size(); ++parentIndex)
        {
            auto& node = asset.dagNodes[parentIndex];
            node.firstChild = static_cast<std::uint32_t>(asset.dagEdges.size());
            node.childCount = static_cast<std::uint32_t>(children[parentIndex].size());
            node.flags = node.childCount == 0u ? 1u : 0u;
            for (const auto childIndex : children[parentIndex])
            {
                node.geometricError = std::max(node.geometricError,
                    asset.dagNodes[childIndex].geometricError);
                asset.dagEdges.push_back({parentIndex, childIndex});
            }
        }
        return validateVirtualGeometryDag(asset)
            ? Halcyon::Result<void>::success()
            : dagFailure("generated M6 DAG failed validation");
    }
    catch (const std::bad_alloc&)
    {
        return Halcyon::Result<void>::failure({Halcyon::ErrorCode::OutOfMemory,
            "unable to allocate M6 DAG tables", "VirtualGeometryDAG"});
    }
}

bool validateVirtualGeometryDag(const VirtualGeometryAsset& asset) noexcept
{
    if (asset.dagNodes.empty() || asset.dagNodes.size() != asset.clusters.size()) return false;
    std::vector<std::uint32_t> incomingEdges;
    try { incomingEdges.assign(asset.dagNodes.size(), 0u); } catch (...) { return false; }
    std::uint32_t roots = 0;
    for (std::uint32_t i = 0; i < asset.dagNodes.size(); ++i)
    {
        const auto& node = asset.dagNodes[i];
        if (node.parentIndex == std::numeric_limits<std::uint32_t>::max()) ++roots;
        if (node.parentIndex != std::numeric_limits<std::uint32_t>::max() &&
            (node.parentIndex >= asset.dagNodes.size() || node.parentIndex <= i)) return false;
        // The GPU adjacency table indexes both arrays with the same ID.
        if (node.clusterIndex != i) return false;
        if (node.firstChild > asset.dagEdges.size() || node.childCount > asset.dagEdges.size() - node.firstChild)
            return false;
        for (std::uint32_t e = 0; e < node.childCount; ++e)
        {
            const auto& edge = asset.dagEdges[node.firstChild + e];
            if (edge.parent != i || edge.child >= asset.dagNodes.size() ||
                edge.child >= i ||
                asset.dagNodes[edge.child].parentIndex != i ||
                asset.dagNodes[i].geometricError + 1.0e-6f < asset.dagNodes[edge.child].geometricError)
                return false;
            if (incomingEdges[edge.child] == std::numeric_limits<std::uint32_t>::max())
                return false;
            ++incomingEdges[edge.child];
        }
    }
    if (roots == 0u) return false;
    for (std::uint32_t i = 0; i < asset.dagNodes.size(); ++i)
    {
        const bool root = asset.dagNodes[i].parentIndex ==
            std::numeric_limits<std::uint32_t>::max();
        if (incomingEdges[i] != (root ? 0u : 1u)) return false;
    }
    // Every parent index is strictly greater than its child index. A finite
    // parent chain therefore cannot cycle, and the one-incoming-edge rule
    // above guarantees that every non-root node is connected to a root.
    return true;
}

float virtualGeometryScreenError(float geometricError, float distance,
    float viewportHeight, float verticalFov) noexcept
{
    if (!std::isfinite(geometricError) || !std::isfinite(distance) ||
        !std::isfinite(viewportHeight) || !std::isfinite(verticalFov) ||
        distance <= 1.0e-6f || viewportHeight <= 0.0f || verticalFov <= 0.0f)
        return std::numeric_limits<float>::infinity();
    return geometricError * viewportHeight /
        (2.0f * std::tan(verticalFov * 0.5f) * distance);
}

bool selectVirtualGeometryLod(const VirtualGeometryAsset& asset,
    VirtualGeometryLodSelectionState& state, float projectedError,
    const VirtualGeometryBuildOptions& options) noexcept
{
    if (asset.dagNodes.empty() || state.currentNode >= asset.dagNodes.size() ||
        !std::isfinite(projectedError)) return false;
    const auto current = asset.dagNodes[state.currentNode];
    const bool refine = projectedError > options.lodRefineThresholdPixels && current.childCount != 0u;
    const bool coarsen = projectedError < options.lodCoarsenThresholdPixels &&
        current.parentIndex != std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t candidate = refine ? asset.dagEdges[current.firstChild].child :
        (coarsen ? current.parentIndex : state.currentNode);
    if (candidate == state.currentNode) { state.candidateNode = candidate; state.pendingFrames = 0; return false; }
    if (state.candidateNode != candidate) { state.candidateNode = candidate; state.pendingFrames = 1; return false; }
    if (++state.pendingFrames < 2u) return false;
    state.currentNode = candidate; state.pendingFrames = 0; return true;
}

bool balanceVirtualGeometryLodRefinement(const VirtualGeometryAsset& asset,
    std::span<std::uint32_t> refinementDecisions) noexcept
{
    if (refinementDecisions.size() != asset.dagNodes.size() ||
        !validateVirtualGeometryDag(asset))
        return false;
    std::vector<std::uint8_t> force;
    try { force.resize(asset.dagNodes.size()); }
    catch (...) { return false; }
    std::uint32_t maxDepth = 0u;
    for (std::size_t i = 0; i < asset.dagNodes.size(); ++i)
    {
        const auto& node = asset.dagNodes[i];
        refinementDecisions[i] = node.childCount != 0u && refinementDecisions[i] != 0u;
        maxDepth = std::max(maxDepth, node.lodDepth);
        const auto& cluster = asset.clusters[node.clusterIndex];
        for (const auto neighbor : cluster.adjacentClusters)
            if (neighbor >= asset.dagNodes.size() ||
                asset.dagNodes[neighbor].lodDepth != node.lodDepth)
                return false;
    }
    for (std::uint32_t iteration = 0u; iteration <= maxDepth; ++iteration)
    {
        std::fill(force.begin(), force.end(), 0u);
        bool changed = false;
        for (std::uint32_t nodeIndex = 0u; nodeIndex < asset.dagNodes.size(); ++nodeIndex)
        {
            const auto& node = asset.dagNodes[nodeIndex];
            if (refinementDecisions[nodeIndex] != 0u || node.childCount == 0u)
                continue;
            bool onFrontier = true;
            for (std::uint32_t ancestor = node.parentIndex, guard = 0u;
                ancestor != std::numeric_limits<std::uint32_t>::max() &&
                    guard < asset.dagNodes.size(); ++guard)
            {
                if (refinementDecisions[ancestor] == 0u)
                {
                    onFrontier = false;
                    break;
                }
                ancestor = asset.dagNodes[ancestor].parentIndex;
            }
            if (!onFrontier) continue;
            const auto& cluster = asset.clusters[node.clusterIndex];
            for (const auto neighbor : cluster.adjacentClusters)
            {
                if (refinementDecisions[neighbor] == 0u) continue;
                const auto& neighborNode = asset.dagNodes[neighbor];
                for (std::uint32_t edgeOffset = 0u;
                    edgeOffset < neighborNode.childCount; ++edgeOffset)
                {
                    const auto child =
                        asset.dagEdges[neighborNode.firstChild + edgeOffset].child;
                    if (refinementDecisions[child] != 0u)
                    {
                        force[nodeIndex] = 1u;
                        break;
                    }
                }
                if (force[nodeIndex] != 0u) break;
            }
        }
        for (std::size_t i = 0; i < force.size(); ++i)
        {
            if (force[i] == 0u || refinementDecisions[i] != 0u) continue;
            refinementDecisions[i] = 1u;
            changed = true;
        }
        if (!changed) break;
    }
    return true;
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
