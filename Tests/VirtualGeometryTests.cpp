#include "Renderer/Scene/Sha256.h"
#include "Renderer/Scene/VirtualGeometry.h"
#include "Renderer/Scene/VirtualGeometryCache.h"

#include <algorithm>
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
    const auto path = std::filesystem::temp_directory_path() / "halcyon-vg-test.halcyon.vgcache";
    const auto secondPath = std::filesystem::temp_directory_path() / "halcyon-vg-test-2.halcyon.vgcache";
    std::error_code error; std::filesystem::remove(path, error);
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    EXPECT(writeVirtualGeometryCache(secondPath, built.value(), hash));
    auto readBytes = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        const auto size = input.tellg(); std::vector<char> bytes(static_cast<std::size_t>(size));
        input.seekg(0); input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())); return bytes;
    };
    EXPECT(readBytes(path) == readBytes(secondPath));
    VirtualGeometryCacheOptions metadata;
    const auto roundTrip = readVirtualGeometryCache(path, &hash, &metadata);
    EXPECT(roundTrip);
    if (roundTrip) {
        EXPECT(roundTrip.value().vertices.size() == built.value().vertices.size());
        EXPECT(roundTrip.value().meshlets.size() == built.value().meshlets.size());
        EXPECT(metadata.build.maxVertices == kVirtualGeometryMaxVertices);
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
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    char magic = 0; stream.read(&magic, 1); stream.seekp(0); magic ^= 0x7f; stream.write(&magic, 1); stream.close();
    EXPECT(!readVirtualGeometryCache(path));
    EXPECT(writeVirtualGeometryCache(path, built.value(), hash));
    // Keep a header-only cache rejection explicit: the reader must reject the
    // short zstd frame before inspecting payload bytes.
    {
        auto headerOnly = readBytes(path);
        headerOnly.resize(104u);
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

void plyTests()
{
    const auto path = std::filesystem::temp_directory_path() / "halcyon-vg-test.ply";
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
    const auto normalPath = std::filesystem::temp_directory_path() / "halcyon-vg-test-normals.ply";
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
    plyTests();
    if (failures != 0) { std::cerr << failures << " Virtual Geometry test(s) failed\n"; return 1; }
    std::cout << "All Virtual Geometry tests passed\n";
    return 0;
}
