#pragma once

#include "StaticSceneLoader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace Halcyon::Renderer::Scene
{

inline constexpr std::uint32_t kVirtualGeometryMaxVertices = 64u;
inline constexpr std::uint32_t kVirtualGeometryMaxTriangles = 124u;

// Visibility attachment ABI shared by the cooker-facing CPU checks and the
// Vulkan M5 shaders. Zero is reserved for a cleared/background pixel.
inline constexpr std::uint32_t kVirtualVisibilityInstanceMask = 0xffu;
inline constexpr std::uint32_t kVirtualVisibilityMeshletMask = 0xfffffu;
inline constexpr std::uint32_t kVirtualVisibilityMaterialMask = 0xfu;
inline constexpr std::uint32_t kVirtualVisibilityMaterialIndexMask = 0x0fffffffu;
inline constexpr std::uint32_t kVirtualVisibilityMeshletShift = 8u;
inline constexpr std::uint32_t kVirtualVisibilityMaterialShift = 28u;
inline constexpr std::uint32_t kVirtualDrawTokenInstanceShift = 20u;
inline constexpr std::uint32_t kVirtualCullHiZEnabledFlag = 1u << 31u;
// The encoded meshlet field stores meshletIndex + 1, so the highest valid
// meshlet index is one less than the all-ones field value.
inline constexpr std::uint32_t kVirtualVisibilityMaxMeshlet =
    kVirtualVisibilityMeshletMask - 1u;
inline constexpr std::uint32_t kVirtualVisibilityCompatibleMaterialClass = 1u;
static_assert(kVirtualVisibilityMeshletMask == (1u << 20u) - 1u);
static_assert(kVirtualVisibilityMaterialIndexMask ==
    (1u << kVirtualVisibilityMaterialShift) - 1u);

[[nodiscard]] constexpr std::uint32_t encodeVirtualDrawToken(
    std::uint32_t instance, std::uint32_t meshlet) noexcept
{
    return ((instance & kVirtualVisibilityInstanceMask) <<
        kVirtualDrawTokenInstanceShift) | (meshlet & kVirtualVisibilityMeshletMask);
}

[[nodiscard]] constexpr std::uint32_t decodeVirtualDrawTokenInstance(
    std::uint32_t token) noexcept
{
    return (token >> kVirtualDrawTokenInstanceShift) & kVirtualVisibilityInstanceMask;
}

[[nodiscard]] constexpr std::uint32_t decodeVirtualDrawTokenMeshlet(
    std::uint32_t token) noexcept
{
    return token & kVirtualVisibilityMeshletMask;
}

struct VirtualVisibilityDecoded
{
    std::uint32_t instance = 0;
    std::uint32_t meshlet = 0;
    std::uint32_t materialClass = 0;
    bool background = true;
};

[[nodiscard]] constexpr std::uint32_t encodeVirtualVisibility(
    std::uint32_t instance, std::uint32_t meshlet, std::uint32_t material) noexcept
{
    return (instance & kVirtualVisibilityInstanceMask) |
        (((meshlet + 1u) & kVirtualVisibilityMeshletMask) <<
            kVirtualVisibilityMeshletShift) |
        ((material & kVirtualVisibilityMaterialMask) <<
            kVirtualVisibilityMaterialShift);
}

[[nodiscard]] constexpr std::uint32_t encodeVirtualPrimitive(std::uint32_t primitive) noexcept
{
    return primitive + 1u;
}

// The second visibility attachment stores the triangle's local primitive ID
// with the same +1 background reservation as the meshlet field. Keep the
// primitive spelling as a compatibility alias for older CPU readback tools.
[[nodiscard]] constexpr std::uint32_t encodeVirtualTriangle(std::uint32_t triangle) noexcept
{
    return encodeVirtualPrimitive(triangle);
}

[[nodiscard]] constexpr VirtualVisibilityDecoded decodeVirtualVisibility(
    std::uint32_t encoded) noexcept
{
    if (encoded == 0u)
        return {};
    const std::uint32_t meshletEncoded =
        (encoded >> kVirtualVisibilityMeshletShift) & kVirtualVisibilityMeshletMask;
    if (meshletEncoded == 0u)
        return {};
    return {encoded & kVirtualVisibilityInstanceMask,
        meshletEncoded - 1u,
        (encoded >> kVirtualVisibilityMaterialShift) & kVirtualVisibilityMaterialMask,
        false};
}

[[nodiscard]] constexpr std::uint32_t decodeVirtualPrimitive(
    std::uint32_t encoded) noexcept
{
    return encoded == 0u ? std::numeric_limits<std::uint32_t>::max() : encoded - 1u;
}

[[nodiscard]] constexpr std::uint32_t decodeVirtualTriangle(
    std::uint32_t encoded) noexcept
{
    return decodeVirtualPrimitive(encoded);
}

[[nodiscard]] constexpr std::uint32_t packVirtualBarycentrics(
    std::uint32_t x, std::uint32_t y) noexcept
{
    return (x & 0xffffu) | ((y & 0xffffu) << 16u);
}

static_assert(encodeVirtualVisibility(0u, 0u, 0u) == (1u << kVirtualVisibilityMeshletShift));
static_assert(encodeVirtualVisibility(0u, kVirtualVisibilityMaxMeshlet, 0u) != 0u);
static_assert(encodeVirtualDrawToken(7u, 9u) == ((7u << kVirtualDrawTokenInstanceShift) | 9u));
static_assert(decodeVirtualDrawTokenInstance(encodeVirtualDrawToken(7u, 9u)) == 7u);
static_assert(decodeVirtualDrawTokenMeshlet(encodeVirtualDrawToken(7u, 9u)) == 9u);
static_assert(encodeVirtualPrimitive(0u) == 1u);
static_assert(encodeVirtualTriangle(7u) == 8u);
static_assert(decodeVirtualVisibility(0u).background);
static_assert(decodeVirtualVisibility(encodeVirtualVisibility(3u, 7u, 2u)).instance == 3u);
static_assert(decodeVirtualVisibility(encodeVirtualVisibility(3u, 7u, 2u)).meshlet == 7u);
static_assert(decodeVirtualPrimitive(encodeVirtualPrimitive(4u)) == 4u);
static_assert(decodeVirtualTriangle(encodeVirtualTriangle(4u)) == 4u);

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

    // V1 classification carries the instance material index rather than a
    // primitive-table reference.  Keep this invariant explicit so assets with
    // mixed primitive materials are rejected before entering the virtual path.
    [[nodiscard]] bool hasUniformPrimitiveMaterial() const noexcept
    {
        if (primitives.empty())
            return false;
        const std::uint32_t material = primitives.front().materialIndex;
        for (const auto& primitive : primitives)
        {
            if (primitive.materialIndex != material)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool hasUniformPrimitiveMaterial(
        std::uint32_t expectedMaterial) const noexcept
    {
        return hasUniformPrimitiveMaterial() &&
            primitives.front().materialIndex == expectedMaterial;
    }
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
