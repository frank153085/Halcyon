#include "Renderer/Scene/Sha256.h"
#include "Renderer/Scene/VirtualGeometry.h"
#include "Renderer/Scene/VirtualGeometryCache.h"
#include "Renderer/Scene/VirtualGeometryPages.h"
#include "Renderer/Scene/VirtualGeometryStreamer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <span>
#include <vector>

using namespace Halcyon::Renderer::Scene;

#ifndef HALCYON_SOURCE_DIR
#define HALCYON_SOURCE_DIR "."
#endif

namespace
{
int failures = 0;
void expect(bool condition, const char* expression, int line)
{
    if (!condition) { ++failures; std::cerr << "FAILED line " << line << ": " << expression << '\n'; }
}
#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

std::filesystem::path uniqueTempPath(const char* stem, const char* extension)
{
    static std::atomic<std::uint64_t> sequence{0u};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (std::string(stem) + "-" + std::to_string(ticks) + "-" +
            std::to_string(sequence.fetch_add(1u, std::memory_order_relaxed)) + extension);
}

StaticScene makeScene()
{
    StaticScene scene;
    scene.materials.emplace_back();
    StaticScenePrimitive primitive;
    primitive.vertices = {
        {{-1, -1, 0}, {0, 0, 1}}, {{1, -1, 0}, {0, 0, 1}},
        {{1, 1, 0}, {0, 0, 1}}, {{-1, 1, 0}, {0, 0, 1}}};
    primitive.indices = {0, 1, 2, 0, 2, 3};
    primitive.boundsMin = {-1, -1, 0}; primitive.boundsMax = {1, 1, 0};
    scene.primitives.push_back(std::move(primitive));
    return scene;
}

StaticScene makeGridScene(std::uint32_t cells)
{
    StaticScene scene;
    scene.materials.emplace_back();
    StaticScenePrimitive primitive;
    const std::uint32_t width = cells + 1u;
    primitive.vertices.reserve(static_cast<std::size_t>(width) * width);
    primitive.indices.reserve(static_cast<std::size_t>(cells) * cells * 6u);
    for (std::uint32_t y = 0; y < width; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            StaticSceneVertex vertex{};
            vertex.position = {static_cast<float>(x), static_cast<float>(y), 0.0f};
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.uv = {static_cast<float>(x) / cells, static_cast<float>(y) / cells};
            primitive.vertices.push_back(vertex);
        }
    }
    for (std::uint32_t y = 0; y < cells; ++y)
    {
        for (std::uint32_t x = 0; x < cells; ++x)
        {
            const std::uint32_t i = y * width + x;
            primitive.indices.insert(primitive.indices.end(),
                {i, i + 1u, i + width + 1u, i, i + width + 1u, i + width});
        }
    }
    primitive.boundsMin = {0.0f, 0.0f, 0.0f};
    primitive.boundsMax = {static_cast<float>(cells), static_cast<float>(cells), 0.0f};
    scene.primitives.push_back(std::move(primitive));
    return scene;
}

void buildTests()
{
    const auto encoded = encodeVirtualVisibility(3u, 0u, 2u);
    EXPECT(encoded != 0u);
    EXPECT((encoded & kVirtualVisibilityInstanceMask) == 3u);
    EXPECT(((encoded >> kVirtualVisibilityMeshletShift) & kVirtualVisibilityMeshletMask) == 1u);
    EXPECT(((encoded >> kVirtualVisibilityMaterialShift) & kVirtualVisibilityMaterialMask) == 2u);
    const auto drawToken = encodeVirtualDrawToken(7u, 9u);
    EXPECT(decodeVirtualDrawTokenInstance(drawToken) == 7u);
    EXPECT(decodeVirtualDrawTokenMeshlet(drawToken) == 9u);
    const auto maxDrawToken = encodeVirtualDrawToken(
        kVirtualVisibilityInstanceMask, kVirtualVisibilityMeshletMask);
    EXPECT(decodeVirtualDrawTokenInstance(maxDrawToken) == kVirtualVisibilityInstanceMask);
    EXPECT(decodeVirtualDrawTokenMeshlet(maxDrawToken) == kVirtualVisibilityMeshletMask);
    const auto decoded = decodeVirtualVisibility(encoded);
    EXPECT(!decoded.background);
    EXPECT(decoded.instance == 3u);
    EXPECT(decoded.meshlet == 0u);
    EXPECT(decoded.materialClass == 2u);
    const auto maxEncoded = encodeVirtualVisibility(0u, kVirtualVisibilityMaxMeshlet,
        kVirtualVisibilityCompatibleMaterialClass);
    EXPECT(maxEncoded != 0u);
    const auto maxDecoded = decodeVirtualVisibility(maxEncoded);
    EXPECT(!maxDecoded.background);
    EXPECT(maxDecoded.meshlet == kVirtualVisibilityMaxMeshlet);
    EXPECT(maxDecoded.materialClass == kVirtualVisibilityCompatibleMaterialClass);
    EXPECT(decodeVirtualVisibility(0u).background);
    EXPECT(decodeVirtualPrimitive(0u) == std::numeric_limits<std::uint32_t>::max());
    EXPECT(decodeVirtualTriangle(0u) == std::numeric_limits<std::uint32_t>::max());
    EXPECT(encodeVirtualPrimitive(4u) == 5u);
    EXPECT(decodeVirtualPrimitive(encodeVirtualPrimitive(4u)) == 4u);
    EXPECT(encodeVirtualTriangle(9u) == 10u);
    EXPECT(decodeVirtualTriangle(encodeVirtualTriangle(9u)) == 9u);
    EXPECT(packVirtualBarycentrics(0x12345u, 0xabcdeu) == 0xbcde2345u);
    const auto result = buildVirtualGeometry(makeScene());
    EXPECT(result);
    if (!result) return;
    const auto& asset = result.value();
    EXPECT(!asset.meshlets.empty());
    EXPECT(asset.hasUniformPrimitiveMaterial());
    EXPECT(asset.hasUniformPrimitiveMaterial(0u));
    EXPECT(!asset.hasUniformPrimitiveMaterial(1u));
    EXPECT(!asset.clusters.empty());
    EXPECT(!asset.dagNodes.empty());
    EXPECT(validateVirtualGeometryDag(asset));
    // One cluster per LOD has no cross-cluster boundary. Vertices reused by a
    // different LOD must not be mistaken for neighbors in the same level.
    EXPECT(std::all_of(asset.clusters.begin(), asset.clusters.end(),
        [](const VirtualGeometryCluster& cluster)
        {
            return cluster.boundaryVertices.empty();
        }));

    VirtualGeometryBuildOptions splitOptions{};
    splitOptions.maxVertices = 3u;
    splitOptions.maxTriangles = 4u;
    splitOptions.maxClusterVertices = 3u;
    splitOptions.maxClusterTriangles = 3u;
    splitOptions.lodRatios = {1.0f, 0.5f, 0.0f};
    const auto splitResult = buildVirtualGeometry(makeScene(), splitOptions);
    EXPECT(splitResult);
    if (splitResult)
    {
        const auto& split = splitResult.value();
        std::vector<std::uint32_t> fineBoundary;
        for (const auto& cluster : split.clusters)
            if (cluster.lodDepth == 0u)
                fineBoundary.insert(fineBoundary.end(), cluster.boundaryVertices.begin(),
                    cluster.boundaryVertices.end());
        std::sort(fineBoundary.begin(), fineBoundary.end());
        fineBoundary.erase(std::unique(fineBoundary.begin(), fineBoundary.end()),
            fineBoundary.end());
        EXPECT(!fineBoundary.empty());
        bool hasAdjacency = false;
        for (std::uint32_t clusterIndex = 0u; clusterIndex < split.clusters.size();
            ++clusterIndex)
        {
            const auto& cluster = split.clusters[clusterIndex];
            EXPECT(std::is_sorted(cluster.adjacentClusters.begin(),
                cluster.adjacentClusters.end()));
            EXPECT(std::adjacent_find(cluster.adjacentClusters.begin(),
                cluster.adjacentClusters.end()) == cluster.adjacentClusters.end());
            for (const auto adjacent : cluster.adjacentClusters)
            {
                hasAdjacency = true;
                EXPECT(adjacent < split.clusters.size());
                if (adjacent < split.clusters.size())
                {
                    const auto& other = split.clusters[adjacent];
                    EXPECT(other.lodDepth == cluster.lodDepth);
                    EXPECT(other.primitiveIndex == cluster.primitiveIndex);
                    EXPECT(std::binary_search(other.adjacentClusters.begin(),
                        other.adjacentClusters.end(), clusterIndex));
                }
            }
        }
        EXPECT(hasAdjacency);
        for (const auto boundary : fineBoundary)
        {
            for (const auto& lod : split.lods)
            {
                bool retained = false;
                for (std::uint32_t i = 0u; i < lod.indexCount; ++i)
                    retained = retained || split.indices[lod.indexOffset + i] == boundary;
                EXPECT(retained);
            }
        }
    }
    EXPECT(std::abs(virtualGeometryScreenError(1.0f, 10.0f, 720.0f,
        1.5707963f) - 36.0f) < 0.01f);
    VirtualGeometryLodSelectionState selection{};
    const auto root = std::find_if(asset.dagNodes.begin(), asset.dagNodes.end(),
        [](const VirtualGeometryDagNode& node)
        {
            return node.parentIndex == std::numeric_limits<std::uint32_t>::max();
        });
    if (root != asset.dagNodes.end() && root->childCount != 0u)
    {
        selection.currentNode = static_cast<std::uint32_t>(std::distance(asset.dagNodes.begin(), root));
        EXPECT(!selectVirtualGeometryLod(asset, selection, 2.0f));
        EXPECT(selectVirtualGeometryLod(asset, selection, 2.0f));
        EXPECT(selection.currentNode != 0u);
    }
    VirtualGeometryAsset balanceAsset{};
    balanceAsset.clusters.resize(6u);
    for (std::uint32_t depth = 0u; depth < 3u; ++depth)
    {
        const std::uint32_t left = depth * 2u;
        const std::uint32_t right = left + 1u;
        balanceAsset.clusters[left].lodDepth = depth;
        balanceAsset.clusters[right].lodDepth = depth;
        balanceAsset.clusters[left].adjacentClusters = {right};
        balanceAsset.clusters[right].adjacentClusters = {left};
    }
    balanceAsset.dagNodes = {
        {0u, 2u, 0u, 0u, 0u, 1u, {}, 0.0f},
        {1u, 3u, 0u, 0u, 0u, 1u, {}, 0.0f},
        {2u, 4u, 0u, 1u, 1u, 0u, {}, 1.0f},
        {3u, 5u, 1u, 1u, 1u, 0u, {}, 1.0f},
        {4u, std::numeric_limits<std::uint32_t>::max(), 2u, 1u, 2u, 0u, {}, 2.0f},
        {5u, std::numeric_limits<std::uint32_t>::max(), 3u, 1u, 2u, 0u, {}, 2.0f},
    };
    balanceAsset.dagEdges = {{2u, 0u}, {3u, 1u}, {4u, 2u}, {5u, 3u}};
    EXPECT(validateVirtualGeometryDag(balanceAsset));
    std::array<std::uint32_t, 6> decisions{0u, 0u, 1u, 0u, 1u, 0u};
    EXPECT(balanceVirtualGeometryLodRefinement(balanceAsset, decisions));
    EXPECT(decisions[4] == 1u);
    EXPECT(decisions[5] == 1u);
    EXPECT(decisions[3] == 0u);
    auto mixedMaterialAsset = asset;
    if (!mixedMaterialAsset.primitives.empty())
    {
        auto mixedPrimitive = mixedMaterialAsset.primitives.front();
        mixedPrimitive.materialIndex += 1u;
        mixedMaterialAsset.primitives.push_back(mixedPrimitive);
        EXPECT(!mixedMaterialAsset.hasUniformPrimitiveMaterial());
    }
    for (const auto& meshlet : asset.meshlets)
    {
        EXPECT(meshlet.vertexCount <= kVirtualGeometryMaxVertices);
        EXPECT(meshlet.triangleCount <= kVirtualGeometryMaxTriangles);
        EXPECT(meshlet.geometricError >= 0.0f);
        EXPECT(std::isfinite(meshlet.sphere.w));
        EXPECT(std::isfinite(meshlet.cone.w));
        const auto vertexSpanInRange = static_cast<std::size_t>(meshlet.vertexOffset) <=
            asset.meshletVertices.size() && static_cast<std::size_t>(meshlet.vertexCount) <=
            asset.meshletVertices.size() - static_cast<std::size_t>(meshlet.vertexOffset);
        const auto triangleCountFitsSize = meshlet.triangleCount <=
            std::numeric_limits<std::size_t>::max() / 3u;
        const auto triangleElementCount = triangleCountFitsSize
            ? static_cast<std::size_t>(meshlet.triangleCount) * 3u : 0u;
        const auto triangleSpanInRange = static_cast<std::size_t>(meshlet.triangleOffset) <=
            asset.meshletTriangles.size() && triangleCountFitsSize && triangleElementCount <=
            asset.meshletTriangles.size() - static_cast<std::size_t>(meshlet.triangleOffset);
        if (vertexSpanInRange)
        {
            for (std::uint32_t local = 0; local < meshlet.vertexCount; ++local)
            {
                const auto source = asset.meshletVertices[meshlet.vertexOffset + local];
                EXPECT(source < asset.vertices.size());
                if (source < asset.vertices.size())
                {
                    const auto delta = asset.vertices[source].position -
                        glm::vec3(meshlet.sphere);
                    EXPECT(glm::length(delta) <= meshlet.sphere.w + 1.0e-4f);
                }
            }
        }
        if (triangleSpanInRange && vertexSpanInRange)
        {
            for (std::uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle)
            {
                const auto local0 = asset.meshletTriangles[meshlet.triangleOffset + triangle * 3u];
                const auto local1 = asset.meshletTriangles[meshlet.triangleOffset + triangle * 3u + 1u];
                const auto local2 = asset.meshletTriangles[meshlet.triangleOffset + triangle * 3u + 2u];
                EXPECT(local0 < meshlet.vertexCount && local1 < meshlet.vertexCount &&
                    local2 < meshlet.vertexCount);
                if (local0 < meshlet.vertexCount && local1 < meshlet.vertexCount &&
                    local2 < meshlet.vertexCount)
                {
                    const auto i0 = asset.meshletVertices[meshlet.vertexOffset + local0];
                    const auto i1 = asset.meshletVertices[meshlet.vertexOffset + local1];
                    const auto i2 = asset.meshletVertices[meshlet.vertexOffset + local2];
                    if (i0 < asset.vertices.size() && i1 < asset.vertices.size() &&
                        i2 < asset.vertices.size())
                    {
                        const auto face = glm::cross(asset.vertices[i1].position -
                            asset.vertices[i0].position, asset.vertices[i2].position -
                            asset.vertices[i0].position);
                        const auto faceLength = glm::length(face);
                        if (faceLength > 1.0e-6f && glm::length(glm::vec3(meshlet.cone)) >
                                1.0e-6f)
                        {
                            const auto axis = glm::normalize(glm::vec3(meshlet.cone));
                            // meshoptimizer stores sin(cone angle) as the
                            // culling cutoff. Convert it back to the minimum
                            // normal/axis cosine before checking containment.
                            const float minimumNormalDot = std::sqrt(std::max(0.0f,
                                1.0f - meshlet.cone.w * meshlet.cone.w));
                            EXPECT(glm::dot(axis, face / faceLength) >=
                                minimumNormalDot - 1.0e-4f);
                        }
                    }
                }
            }
        }
    }
    for (const auto index : asset.indices) EXPECT(index < asset.vertices.size());
    for (const auto& lod : asset.lods)
    {
        const auto lodMeshletsInRange = static_cast<std::size_t>(lod.meshletOffset) <=
            asset.meshlets.size() && static_cast<std::size_t>(lod.meshletCount) <=
            asset.meshlets.size() - static_cast<std::size_t>(lod.meshletOffset);
        const auto lodIndicesInRange = static_cast<std::size_t>(lod.indexOffset) <=
            asset.indices.size() && static_cast<std::size_t>(lod.indexCount) <=
            asset.indices.size() - static_cast<std::size_t>(lod.indexOffset);
        EXPECT(lodMeshletsInRange);
        EXPECT(lodIndicesInRange);
        EXPECT(lod.geometricError >= 0.0f);
        std::size_t covered = 0;
        if (lodMeshletsInRange)
        {
            const std::size_t triangleCount = static_cast<std::size_t>(lod.indexCount / 3u);
            std::vector<std::uint8_t> seen(triangleCount, 0u);
            std::size_t expectedIndexOffset = lod.indexOffset;
            for (std::uint32_t i = 0; i < lod.meshletCount; ++i)
            {
                const auto& meshlet = asset.meshlets[static_cast<std::size_t>(lod.meshletOffset) + i];
                covered += meshlet.triangleCount;
                // Meshlets are emitted in deterministic LOD order. Their
                // index ranges must form a contiguous partition of the LOD;
                // this catches duplicate or skipped source triangles even
                // when the aggregate triangle count still matches.
                EXPECT(meshlet.indexOffset == expectedIndexOffset);
                const std::uint64_t lodEnd = static_cast<std::uint64_t>(lod.indexOffset) +
                    lod.indexCount;
                const std::uint64_t meshletEnd = static_cast<std::uint64_t>(meshlet.indexOffset) +
                    meshlet.indexCount;
                if (meshlet.indexOffset >= lod.indexOffset &&
                    meshletEnd <= lodEnd)
                {
                    const std::size_t localOffset = static_cast<std::size_t>(
                        meshlet.indexOffset - lod.indexOffset);
                    const std::size_t localCount = meshlet.indexCount / 3u;
                    if (localOffset <= triangleCount && localCount <= triangleCount - localOffset)
                    {
                        for (std::size_t triangle = 0; triangle < localCount; ++triangle)
                        {
                            const std::size_t slot = localOffset + triangle;
                            EXPECT(seen[slot] == 0u);
                            seen[slot] = 1u;
                        }
                    }
                }
                expectedIndexOffset += meshlet.indexCount;
            }
            EXPECT(expectedIndexOffset == static_cast<std::size_t>(lod.indexOffset) +
                lod.indexCount);
            for (const auto value : seen) EXPECT(value == 1u);
            EXPECT((lod.indexCount % 3u) == 0u && covered == lod.indexCount / 3u);
        }
    }

    const auto repeated = buildVirtualGeometry(makeScene());
    EXPECT(repeated);
    if (repeated)
    {
        EXPECT(repeated.value().meshlets.size() == asset.meshlets.size());
        EXPECT(repeated.value().indices == asset.indices);
        EXPECT(repeated.value().meshletVertices == asset.meshletVertices);
        EXPECT(repeated.value().meshletTriangles == asset.meshletTriangles);
        EXPECT(repeated.value().vertices.size() == asset.vertices.size());
        for (std::size_t i = 0; i < std::min(asset.vertices.size(), repeated.value().vertices.size()); ++i)
        {
            EXPECT(repeated.value().vertices[i].position == asset.vertices[i].position);
            EXPECT(repeated.value().vertices[i].normal == asset.vertices[i].normal);
            EXPECT(repeated.value().vertices[i].uv == asset.vertices[i].uv);
            EXPECT(repeated.value().vertices[i].tangent == asset.vertices[i].tangent);
        }
        for (std::size_t i = 0; i < std::min(asset.meshlets.size(), repeated.value().meshlets.size()); ++i)
            EXPECT(repeated.value().meshlets[i].vertexOffset == asset.meshlets[i].vertexOffset &&
                repeated.value().meshlets[i].vertexCount == asset.meshlets[i].vertexCount &&
                repeated.value().meshlets[i].triangleOffset == asset.meshlets[i].triangleOffset &&
                repeated.value().meshlets[i].triangleCount == asset.meshlets[i].triangleCount &&
                repeated.value().meshlets[i].indexOffset == asset.meshlets[i].indexOffset &&
                repeated.value().meshlets[i].indexCount == asset.meshlets[i].indexCount &&
                repeated.value().meshlets[i].primitiveIndex == asset.meshlets[i].primitiveIndex &&
                repeated.value().meshlets[i].lodIndex == asset.meshlets[i].lodIndex &&
                repeated.value().meshlets[i].sphere == asset.meshlets[i].sphere &&
                repeated.value().meshlets[i].cone == asset.meshlets[i].cone &&
                repeated.value().meshlets[i].geometricError == asset.meshlets[i].geometricError);
        EXPECT(repeated.value().lods.size() == asset.lods.size());
        for (std::size_t i = 0; i < std::min(asset.lods.size(), repeated.value().lods.size()); ++i)
        {
            EXPECT(repeated.value().lods[i].geometricError == asset.lods[i].geometricError);
            EXPECT(repeated.value().lods[i].ratio == asset.lods[i].ratio);
        }
    }

    auto invalidOptions = VirtualGeometryBuildOptions{};
    invalidOptions.lodRatios[0] = 0.5f;
    EXPECT(!buildVirtualGeometry(makeScene(), invalidOptions));
    invalidOptions = VirtualGeometryBuildOptions{};
    invalidOptions.lodRatios[1] = 0.0f;
    invalidOptions.lodRatios[2] = 0.5f;
    EXPECT(!buildVirtualGeometry(makeScene(), invalidOptions));
    invalidOptions = VirtualGeometryBuildOptions{};
    invalidOptions.simplifyError = std::numeric_limits<float>::quiet_NaN();
    EXPECT(!buildVirtualGeometry(makeScene(), invalidOptions));

    auto invalidMaterialScene = makeScene();
    invalidMaterialScene.primitives.front().materialIndex =
        kVirtualVisibilityMaterialIndexMask + 1u;
    EXPECT(!buildVirtualGeometry(invalidMaterialScene));
}

void cacheTests()
{
    const auto built = buildVirtualGeometry(makeScene());
    EXPECT(built);
    if (!built) return;
    const auto hash = sha256(std::as_bytes(std::span("virtual-geometry-test", 22)));
    const auto path = uniqueTempPath("halcyon-vg-test", ".halcyon.vgcache");
    const auto secondPath = uniqueTempPath("halcyon-vg-test-2", ".halcyon.vgcache");
    std::error_code error; std::filesystem::remove(path, error);
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    EXPECT(writeVirtualGeometryCache(secondPath, built.value(), hash));
    auto readBytes = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        const auto size = input.tellg(); std::vector<char> bytes(static_cast<std::size_t>(size));
        input.seekg(0); input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())); return bytes;
    };
    EXPECT(readBytes(path) == readBytes(secondPath));
    const auto pageMetadata = readVirtualGeometryCacheMetadata(path, &hash);
    EXPECT(pageMetadata);
    if (pageMetadata)
    {
        const auto expectedLayout = buildVirtualGeometryPageLayout(
            built.value(), kVirtualGeometryCachePageSize);
        EXPECT(expectedLayout);
        EXPECT(pageMetadata.value().options.pageSize == kVirtualGeometryCachePageSize);
        EXPECT(expectedLayout && pageMetadata.value().rootPageCount ==
            expectedLayout->rootPageIndices.size());
        EXPECT(expectedLayout && pageMetadata.value().rootPageIndices ==
            expectedLayout->rootPageIndices);
        EXPECT(pageMetadata.value().pageLayout.dependencyPageIndices ==
            (expectedLayout ? expectedLayout->dependencyPageIndices :
                std::vector<std::uint32_t>{}));
        EXPECT(!pageMetadata.value().pages.empty());
        for (std::uint32_t pageIndex = 0u;
            pageIndex < pageMetadata.value().pages.size(); ++pageIndex)
        {
            const bool root = std::binary_search(
                pageMetadata.value().rootPageIndices.begin(),
                pageMetadata.value().rootPageIndices.end(), pageIndex);
            EXPECT(((pageMetadata.value().pages[pageIndex].flags &
                VirtualGeometryCachePagePinnedRoot) != 0u) == root);
            const auto diskPage = readVirtualGeometryCachePage(
                path, pageMetadata.value(), pageIndex);
            EXPECT(diskPage);
            if (diskPage && expectedLayout)
            {
                const auto expectedPage = serializeVirtualGeometryPage(
                    built.value(), expectedLayout.value(), pageIndex);
                EXPECT(expectedPage);
                if (expectedPage)
                    EXPECT(diskPage.value() == expectedPage.value());
            }
        }
        EXPECT(!readVirtualGeometryCachePage(path, pageMetadata.value(),
            static_cast<std::uint32_t>(pageMetadata.value().pages.size())));
    }
    VirtualGeometryCacheOptions metadata;
    const auto roundTrip = readVirtualGeometryCache(path, &hash, &metadata);
    EXPECT(roundTrip);
    if (roundTrip) {
        EXPECT(roundTrip.value().vertices.size() == built.value().vertices.size());
        EXPECT(roundTrip.value().meshlets.size() == built.value().meshlets.size());
        EXPECT(roundTrip.value().clusters.size() == built.value().clusters.size());
        EXPECT(roundTrip.value().dagNodes.size() == built.value().dagNodes.size());
        EXPECT(roundTrip.value().dagEdges.size() == built.value().dagEdges.size());
        EXPECT(roundTrip.value().clusters.size() == built.value().clusters.size());
        for (std::size_t i = 0; i < built.value().clusters.size(); ++i)
            EXPECT(roundTrip.value().clusters[i].adjacentClusters ==
                built.value().clusters[i].adjacentClusters);
        EXPECT(validateVirtualGeometryDag(roundTrip.value()));
        EXPECT(metadata.build.maxVertices == kVirtualGeometryMaxVertices);
    }
    VirtualGeometryBuildOptions adjacencyBuildOptions{};
    adjacencyBuildOptions.maxVertices = 3u;
    adjacencyBuildOptions.maxTriangles = 4u;
    adjacencyBuildOptions.maxClusterVertices = 3u;
    adjacencyBuildOptions.maxClusterTriangles = 3u;
    adjacencyBuildOptions.lodRatios = {1.0f, 0.5f, 0.0f};
    const auto adjacencyBuilt = buildVirtualGeometry(makeScene(), adjacencyBuildOptions);
    EXPECT(adjacencyBuilt);
    if (adjacencyBuilt)
    {
        VirtualGeometryCacheOptions adjacencyCacheOptions{};
        adjacencyCacheOptions.build = adjacencyBuildOptions;
        EXPECT(writeVirtualGeometryCache(secondPath, adjacencyBuilt.value(), hash,
            adjacencyCacheOptions));
        const auto adjacencyRoundTrip = readVirtualGeometryCache(secondPath, &hash,
            nullptr, &adjacencyCacheOptions);
        EXPECT(adjacencyRoundTrip);
        if (adjacencyRoundTrip)
        {
            EXPECT(adjacencyRoundTrip.value().clusters.size() ==
                adjacencyBuilt.value().clusters.size());
            for (std::size_t i = 0; i < adjacencyBuilt.value().clusters.size(); ++i)
                EXPECT(adjacencyRoundTrip.value().clusters[i].adjacentClusters ==
                    adjacencyBuilt.value().clusters[i].adjacentClusters);
        }
    }
    const auto wrongHash = sha256(std::as_bytes(std::span("wrong", 5)));
    EXPECT(!readVirtualGeometryCache(path, &wrongHash));
    auto mismatchedOptions = VirtualGeometryCacheOptions{};
    mismatchedOptions.build.simplifyError = 0.25f;
    EXPECT(!readVirtualGeometryCache(path, &hash, nullptr, &mismatchedOptions));

    auto malformed = built.value();
    if (!malformed.meshletTriangles.empty())
    {
        malformed.meshletTriangles.front() =
            static_cast<std::uint8_t>(malformed.meshlets.front().vertexCount);
        EXPECT(!writeVirtualGeometryCache(secondPath, malformed, hash));
    }
    auto missingLod = built.value();
    if (!missingLod.lods.empty())
    {
        missingLod.lods.pop_back();
        EXPECT(!writeVirtualGeometryCache(secondPath, missingLod, hash));
    }
    auto oversizedMaterial = built.value();
    if (!oversizedMaterial.primitives.empty())
    {
        oversizedMaterial.primitives.front().materialIndex =
            kVirtualVisibilityMaterialIndexMask + 1u;
        EXPECT(!writeVirtualGeometryCache(secondPath, oversizedMaterial, hash));
    }
    auto malformedAdjacency = built.value();
    if (!malformedAdjacency.clusters.empty())
    {
        malformedAdjacency.clusters.front().adjacentClusters.push_back(0u);
        EXPECT(!writeVirtualGeometryCache(secondPath, malformedAdjacency, hash));
    }
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    char magic = 0; stream.read(&magic, 1); stream.seekp(0); magic ^= 0x7f; stream.write(&magic, 1); stream.close();
    EXPECT(!readVirtualGeometryCache(path));
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    {
        auto oldVersion = readBytes(path);
        oldVersion[8] = 3;
        oldVersion[9] = oldVersion[10] = oldVersion[11] = 0;
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy.write(oldVersion.data(), static_cast<std::streamsize>(oldVersion.size()));
    }
    const auto legacy = readVirtualGeometryCache(path);
    EXPECT(!legacy);
    if (!legacy)
        EXPECT(legacy.error().describe().find("re-run HalcyonCooker") != std::string::npos);
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    // Keep a header-only cache rejection explicit: the reader must reject the
    // short zstd frame before inspecting payload bytes.
    {
        auto headerOnly = readBytes(path);
        headerOnly.resize(128u);
        std::ofstream truncated(path, std::ios::binary | std::ios::trunc);
        truncated.write(headerOnly.data(), static_cast<std::streamsize>(headerOnly.size()));
    }
    EXPECT(!readVirtualGeometryCache(path));
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    auto bytes = readBytes(path); bytes.back() ^= 0x55;
    { std::ofstream damaged(path, std::ios::binary | std::ios::trunc); damaged.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    EXPECT(!readVirtualGeometryCache(path));
    std::filesystem::remove(path, error); std::filesystem::remove(secondPath, error);
}

void qualityControlTests()
{
    VirtualGeometryQualityState state{};
    updateVirtualGeometryQuality(state, 20.0f, 0.0f);
    EXPECT(std::abs(state.gpuTimeEmaMs - 20.0f) < 0.0001f);
    EXPECT(std::abs(state.qualityScale - 1.25f) < 0.0001f);

    VirtualGeometryQualityState recovery{};
    recovery.qualityScale = 2.0f;
    recovery.gpuTimeEmaMs = 10.0f;
    recovery.gpuSampleCount = 8u;
    for (std::uint32_t frame = 0u; frame < 59u; ++frame)
        updateVirtualGeometryQuality(recovery, 10.0f, 0.0f);
    EXPECT(std::abs(recovery.qualityScale - 2.0f) < 0.0001f);
    updateVirtualGeometryQuality(recovery, 10.0f, 0.0f);
    EXPECT(std::abs(recovery.qualityScale - 1.8f) < 0.0001f);

    VirtualGeometryQualityState pressure{};
    updateVirtualGeometryQuality(pressure,
        std::numeric_limits<float>::quiet_NaN(), 0.76f);
    EXPECT(std::abs(pressure.qualityScale - 1.25f) < 0.0001f);
    for (std::uint32_t frame = 0u; frame < 20u; ++frame)
        updateVirtualGeometryQuality(pressure, 100.0f, 1.0f);
    EXPECT(std::abs(pressure.qualityScale - 8.0f) < 0.0001f);
}

void streamerTests()
{
    using namespace std::chrono_literals;
    const auto path = uniqueTempPath("halcyon-vg-streamer-test", ".cache");
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto built = buildVirtualGeometry(makeGridScene(20u));
    EXPECT(built);
    if (!built)
        return;
    const auto pageLayout = buildVirtualGeometryPageLayout(built.value(), 4096u);
    EXPECT(pageLayout);
    if (!pageLayout)
        return;
    EXPECT(pageLayout->pageCount > 1u);
    EXPECT(pageLayout->nodeDependencies.size() == built->dagNodes.size());
    EXPECT(!pageLayout->rootPageIndices.empty());
    EXPECT(!buildVirtualGeometryPageLayout(built.value(), 5000u));

    std::vector<std::byte> pageableBytes(
        static_cast<std::size_t>(pageLayout->rawSize), std::byte{0});
    for (std::uint32_t pageIndex = 0u; pageIndex < pageLayout->pageCount; ++pageIndex)
    {
        const auto page = serializeVirtualGeometryPage(
            built.value(), pageLayout.value(), pageIndex);
        EXPECT(page);
        if (page)
            std::copy(page->begin(), page->end(),
                pageableBytes.begin() + static_cast<std::size_t>(pageIndex) *
                    pageLayout->pageSize);
    }
    const auto matchesStream = [&](VirtualGeometryStreamRange range,
                                   const void* source, std::size_t size)
    {
        return range.size == size &&
            std::memcmp(pageableBytes.data() + static_cast<std::size_t>(range.offset),
                source, size) == 0;
    };
    EXPECT(matchesStream(pageLayout->vertices, built->vertices.data(),
        built->vertices.size() * sizeof(built->vertices[0])));
    EXPECT(matchesStream(pageLayout->meshletVertices, built->meshletVertices.data(),
        built->meshletVertices.size() * sizeof(built->meshletVertices[0])));
    EXPECT(matchesStream(pageLayout->meshletTriangles, built->meshletTriangles.data(),
        built->meshletTriangles.size() * sizeof(built->meshletTriangles[0])));
    EXPECT(matchesStream(pageLayout->indices, built->indices.data(),
        built->indices.size() * sizeof(built->indices[0])));
    EXPECT(!serializeVirtualGeometryPage(
        built.value(), pageLayout.value(), pageLayout->pageCount));
    for (std::uint32_t nodeIndex = 0u; nodeIndex < built->dagNodes.size(); ++nodeIndex)
    {
        const auto dependency = pageLayout->nodeDependencies[nodeIndex];
        EXPECT(dependency.count > 0u);
        EXPECT(static_cast<std::size_t>(dependency.offset) + dependency.count <=
            pageLayout->dependencyPageIndices.size());
        for (std::uint32_t i = 0u; i < dependency.count; ++i)
        {
            const auto page = pageLayout->dependencyPageIndices[dependency.offset + i];
            EXPECT(page < pageLayout->pageCount);
            EXPECT(virtualGeometryNodeDependsOnPage(
                pageLayout.value(), nodeIndex, page));
            if (i != 0u)
                EXPECT(pageLayout->dependencyPageIndices[dependency.offset + i - 1u] < page);
        }
    }
    for (std::uint32_t nodeIndex = 0u; nodeIndex < built->dagNodes.size(); ++nodeIndex)
    {
        if (built->dagNodes[nodeIndex].parentIndex !=
            std::numeric_limits<std::uint32_t>::max())
            continue;
        const auto dependency = pageLayout->nodeDependencies[nodeIndex];
        for (std::uint32_t i = 0u; i < dependency.count; ++i)
            EXPECT(std::binary_search(pageLayout->rootPageIndices.begin(),
                pageLayout->rootPageIndices.end(),
                pageLayout->dependencyPageIndices[dependency.offset + i]));
    }
    VirtualGeometryCacheOptions cacheOptions{};
    cacheOptions.pageSize = 4096u;
    const Sha256Digest hash{};
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash, cacheOptions));
    const auto metadata = readVirtualGeometryCacheMetadata(path, &hash, &cacheOptions);
    EXPECT(metadata);
    if (!metadata)
        return;
    EXPECT(metadata->pages.size() > 1u);

    VirtualGeometryStreamingConfig streamingConfig{};
    streamingConfig.cpuStagingBudgetBytes = 16u * 4096u;
    streamingConfig.maxUploadBytesPerFrame = 4096u;
    streamingConfig.maxUploadPagesPerFrame = 1u;
    auto streamer = VirtualGeometryStreamer::open(
        path, &hash, &cacheOptions, streamingConfig);
    EXPECT(streamer);
    if (!streamer)
        return;
    for (const auto rootPage : metadata->rootPageIndices)
        EXPECT(streamer.value()->pageState(rootPage) == VirtualGeometryPageState::ReadyCPU);
    for (std::size_t rootIndex = 0u; rootIndex < metadata->rootPageIndices.size(); ++rootIndex)
    {
        auto root = streamer.value()->takeReadyPages();
        EXPECT(root.size() == 1u);
        if (!root.empty())
        {
            EXPECT(std::binary_search(metadata->rootPageIndices.begin(),
                metadata->rootPageIndices.end(), root.front().pageIndex));
            EXPECT(streamer.value()->markResident(root.front().pageIndex,
                root.front().generation, 0u, 1u));
        }
    }
    const auto nonRoot = std::find_if(metadata->pages.begin(), metadata->pages.end(),
        [](const VirtualGeometryCachePageEntry& page)
        {
            return (page.flags & VirtualGeometryCachePagePinnedRoot) == 0u;
        });
    EXPECT(nonRoot != metadata->pages.end());
    if (nonRoot == metadata->pages.end())
        return;
    const std::uint32_t nonRootPage = static_cast<std::uint32_t>(
        std::distance(metadata->pages.begin(), nonRoot));
    EXPECT(!streamer.value()->requestPage(metadata->rootPageIndices.front(), 1.0f, 1u));
    EXPECT(streamer.value()->requestPage(nonRootPage, 10.0f, 1u));
    EXPECT(!streamer.value()->requestPage(nonRootPage, 20.0f, 1u));
    EXPECT(streamer.value()->waitForPageState(
        nonRootPage, VirtualGeometryPageState::ReadyCPU, 5s));
    auto ready = streamer.value()->takeReadyPages();
    EXPECT(ready.size() == 1u);
    if (!ready.empty())
    {
        const auto generation = ready.front().generation;
        EXPECT(ready.front().pageIndex == nonRootPage);
        EXPECT(!streamer.value()->markResident(
            nonRootPage, generation + 1u, 1u, 3u));
        EXPECT(streamer.value()->markResident(nonRootPage, generation, 1u, 3u));
        EXPECT(streamer.value()->beginEviction(2u, 10u) ==
            std::numeric_limits<std::uint32_t>::max());
        EXPECT(streamer.value()->beginEviction(3u, 10u) == nonRootPage);
        EXPECT(streamer.value()->finishEviction(nonRootPage, generation));
        EXPECT(streamer.value()->pageGeneration(nonRootPage) == generation + 1u);
        EXPECT(streamer.value()->pageState(nonRootPage) ==
            VirtualGeometryPageState::Unloaded);
    }
    EXPECT(streamer.value()->requestPage(nonRootPage, 5.0f, 11u));
    EXPECT(streamer.value()->waitForPageState(
        nonRootPage, VirtualGeometryPageState::ReadyCPU, 5s));
    auto abandoned = streamer.value()->takeReadyPages();
    EXPECT(abandoned.size() == 1u);
    if (!abandoned.empty())
    {
        EXPECT(streamer.value()->pageState(nonRootPage) ==
            VirtualGeometryPageState::Uploading);
        EXPECT(streamer.value()->abandonUpload(
            abandoned.front().pageIndex, abandoned.front().generation));
        EXPECT(streamer.value()->pageState(nonRootPage) ==
            VirtualGeometryPageState::Unloaded);
        EXPECT(streamer.value()->requestPage(nonRootPage, 4.0f, 12u));
    }
    EXPECT(streamer.value()->beginEviction(
        std::numeric_limits<std::uint64_t>::max(), 100u) ==
        std::numeric_limits<std::uint32_t>::max());
    const auto stats = streamer.value()->stats();
    EXPECT(stats.requestedPages == 3u);
    EXPECT(stats.loadedPages >= metadata->rootPageIndices.size() + 1u);
    EXPECT(stats.evictedPages == 1u);
    std::filesystem::remove(path, error);
}

void plyTests()
{
    const auto path = uniqueTempPath("halcyon-vg-test", ".ply");
    { std::ofstream stream(path, std::ios::binary | std::ios::trunc);
      stream << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"; }
    const auto loaded = loadGeometrySource(path);
    EXPECT(loaded); if (!loaded) std::cerr << loaded.error().describe() << '\n'; if (loaded) {
        EXPECT(loaded.value().primitives.size() == 1);
        EXPECT(loaded.value().primitives.front().indices.size() == 3);
        for (const auto& vertex : loaded.value().primitives.front().vertices)
            EXPECT(std::abs(vertex.normal.z - 1.0f) < 0.0001f);
    }
    std::error_code error; std::filesystem::remove(path, error);

    // Position and normal properties are requested as separate tinyply
    // buffers. Keep an explicit-normal fixture here so an interleaved
    // property regression cannot silently pass the generated-normal test.
    const auto normalPath = uniqueTempPath("halcyon-vg-test-normals", ".ply");
    { std::ofstream stream(normalPath, std::ios::binary | std::ios::trunc);
      stream << "ply\nformat ascii 1.0\nelement vertex 3\n"
                "property float x\nproperty float y\nproperty float z\n"
                "property float nx\nproperty float ny\nproperty float nz\n"
                "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                "0 0 0 0 0 -1\n1 0 0 0 0 -1\n0 1 0 0 0 -1\n"
                "3 0 1 2\n"; }
    const auto withNormals = loadGeometrySource(normalPath);
    EXPECT(withNormals);
    if (withNormals)
    {
        EXPECT(withNormals.value().primitives.size() == 1);
        for (const auto& vertex : withNormals.value().primitives.front().vertices)
            EXPECT(std::abs(vertex.normal.z + 1.0f) < 0.0001f);
    }
    std::filesystem::remove(normalPath, error);
}
} // namespace

int main()
{
    buildTests();
    cacheTests();
    qualityControlTests();
    streamerTests();
    plyTests();
    if (failures != 0) { std::cerr << failures << " Virtual Geometry test(s) failed\n"; return 1; }
    std::cout << "All Virtual Geometry tests passed\n";
    return 0;
}
