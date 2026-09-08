#pragma once

#include "StaticSceneLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryMaxVertices = 64u;
inline constexpr std::uint32_t kVirtualGeometryMaxTriangles = 124u;

struct VirtualGeometryMeshlet
{
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t primitiveIndex = 0;
    std::uint32_t lodIndex = 0;
    glm::vec4 sphere{0.0f};
    glm::vec4 cone{0.0f};
    float geometricError = 0.0f;
};
static_assert(sizeof(VirtualGeometryMeshlet) == 68,
    "VirtualGeometryMeshlet is part of the cache serialization ABI");

struct VirtualGeometryLod
{
    std::uint32_t primitiveIndex = 0;
    std::uint32_t meshletOffset = 0;
    std::uint32_t meshletCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    float geometricError = 0.0f;
    float ratio = 1.0f;
};

struct VirtualGeometryPrimitive
{
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t materialIndex = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

struct VirtualGeometryAsset
{
    std::vector<StaticSceneVertex> vertices;
    std::vector<std::uint32_t> meshletVertices;
    std::vector<std::uint8_t> meshletTriangles;
    std::vector<std::uint32_t> indices;
    std::vector<VirtualGeometryMeshlet> meshlets;
    std::vector<VirtualGeometryLod> lods;
    std::vector<VirtualGeometryPrimitive> primitives;

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3u; }
};

struct VirtualGeometryBuildOptions
{
    std::array<float, 3> lodRatios{1.0f, 0.5f, 0.25f};
    std::uint32_t maxVertices = kVirtualGeometryMaxVertices;
    std::uint32_t maxTriangles = kVirtualGeometryMaxTriangles;
    float simplifyError = 1.0f;
};

[[nodiscard]] Halcyon::Result<VirtualGeometryAsset> buildVirtualGeometry(
    const StaticScene& scene, const VirtualGeometryBuildOptions& options = {});

[[nodiscard]] Halcyon::Result<StaticScene> loadGeometrySource(
    const std::filesystem::path& path, const StaticSceneLoadOptions& options = {});

} // namespace Halcyon::Renderer::Scene
