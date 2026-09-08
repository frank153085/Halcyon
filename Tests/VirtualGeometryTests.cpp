#include "Renderer/Scene/Sha256.h"
#include "Renderer/Scene/VirtualGeometry.h"
#include "Renderer/Scene/VirtualGeometryCache.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    const auto result = buildVirtualGeometry(makeScene());
    EXPECT(result);
    if (!result) return;
    const auto& asset = result.value();
    EXPECT(!asset.meshlets.empty());
    for (const auto& meshlet : asset.meshlets)
    {
        EXPECT(meshlet.vertexCount <= kVirtualGeometryMaxVertices);
        EXPECT(meshlet.triangleCount <= kVirtualGeometryMaxTriangles);
        EXPECT(meshlet.geometricError >= 0.0f);
        EXPECT(std::isfinite(meshlet.sphere.w));
    }
    for (const auto index : asset.indices) EXPECT(index < asset.vertices.size());
    for (const auto& lod : asset.lods)
    {
        EXPECT(lod.meshletOffset + lod.meshletCount <= asset.meshlets.size());
        EXPECT(lod.indexOffset + lod.indexCount <= asset.indices.size());
        EXPECT(lod.geometricError >= 0.0f);
    }
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
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    char magic = 0; stream.read(&magic, 1); stream.seekp(0); magic ^= 0x7f; stream.write(&magic, 1); stream.close();
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
