// Virtual Geometry V1 visibility-buffer ABI.
// R32_UINT visibility ID:
//   bits  0.. 7  instance index
//   bits  8..27  meshlet index + 1 (zero is background)
//   bits 28..31  material class
// R32_UINT primitive ID stores triangle index + 1 (zero is background).
// R32_UINT barycentrics packs two 16-bit UNORM components (x in low bits,
// y in high bits); z is reconstructed as 1 - x - y.
static const uint VG_VISIBILITY_INSTANCE_MASK = 0xffu;
static const uint VG_VISIBILITY_MESHLET_MASK = 0xfffffu;
static const uint VG_VISIBILITY_MATERIAL_MASK = 0xfu;
static const uint VG_VISIBILITY_MATERIAL_INDEX_MASK = 0x0fffffffu;
static const uint VG_VISIBILITY_MESHLET_SHIFT = 8u;
static const uint VG_VISIBILITY_MATERIAL_SHIFT = 28u;
static const uint VG_VISIBILITY_COMPATIBLE_MATERIAL_CLASS = 1u;
// Bits 0, 1 and 4 are the ECS transparent/double-sided/alpha-masked flags.
// Bit 31 is renderer-owned and marks a slot that must stay on the CPU
// fallback path; keep it in the shader mask so malformed visibility tokens
// cannot accidentally shade that slot as a compatible PBR material.
static const uint VG_VISIBILITY_INCOMPATIBLE_MATERIAL_FLAGS = 0x80000013u;
static const uint VG_CULL_LOD_MASK = 0xffu;
static const uint VG_CULL_HIZ_ENABLED_FLAG = 0x80000000u;
static const uint VG_MESHLET_MAX_VERTICES = 64u;
static const uint VG_MESHLET_MAX_TRIANGLES = 124u;
// The compute cull stream packs an instance index above the 20-bit meshlet
// field. Keep this separate from the visibility attachment shifts: the token
// is an internal indirect-draw ABI and does not reserve a background value.
static const uint VG_DRAW_TOKEN_INSTANCE_SHIFT = 20u;

// Shared storage-buffer ABI. Keep virtual-geometry shaders on these single
// definitions; the runtime reflection layer verifies their decorated array
// strides against the corresponding C++ structures when pipelines are built.
struct MeshletMeta
{
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;
    uint indexOffset;
    uint indexCount;
    uint primitiveIndex;
    uint lodIndex;
    float4 sphere;
    float4 cone;
    float geometricError;
    uint pageIndex;
    float2 padding;
};

struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float4 tangent;
};

struct PageTableEntry
{
    uint physicalPage;
    uint generation;
    uint flags;
    uint lastRequestedFrame;
};

struct GeometryPageInfo
{
    uint pageSize;
    uint pageCount;
    uint physicalPageCount;
    uint vertexCount;
    uint meshletVertexCount;
    uint triangleByteCount;
    uint indexCount;
    uint verticesFirstPage;
    uint meshletVerticesFirstPage;
    uint meshletTrianglesFirstPage;
    uint indicesFirstPage;
    uint padding;
};

struct TransformRow
{
    float4x4 model;
};

struct MeshMaterialRow
{
    uint meshId;
    uint materialIndex;
    uint flags;
    uint lodState;
};

struct MaterialGpuData
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 factors;
    uint4 textureIndices0;
    uint4 textureIndices1;
};

struct LightData
{
    float4 positionAndRadius;
    float4 colorAndIntensity;
    float4 directionAndType;
    float4 spotParams;
};

struct DrawIndexedCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

uint vgVisibilityInstance(uint encoded)
{
    return encoded & VG_VISIBILITY_INSTANCE_MASK;
}

uint vgVisibilityMeshletEncoded(uint encoded)
{
    return (encoded >> VG_VISIBILITY_MESHLET_SHIFT) & VG_VISIBILITY_MESHLET_MASK;
}

uint vgVisibilityMeshlet(uint encoded)
{
    const uint meshlet = vgVisibilityMeshletEncoded(encoded);
    return meshlet == 0u ? 0xffffffffu : meshlet - 1u;
}

uint vgVisibilityMaterialClass(uint encoded)
{
    return (encoded >> VG_VISIBILITY_MATERIAL_SHIFT) & VG_VISIBILITY_MATERIAL_MASK;
}

uint vgVisibilityMaterialIndex(uint classified)
{
    return classified & VG_VISIBILITY_MATERIAL_INDEX_MASK;
}

uint vgPackDrawToken(uint instanceIndex, uint meshletIndex)
{
    return ((instanceIndex & VG_VISIBILITY_INSTANCE_MASK) <<
        VG_DRAW_TOKEN_INSTANCE_SHIFT) | (meshletIndex & VG_VISIBILITY_MESHLET_MASK);
}

uint vgDrawTokenMeshlet(uint token)
{
    return token & VG_VISIBILITY_MESHLET_MASK;
}

uint vgDrawTokenInstance(uint token)
{
    return (token >> VG_DRAW_TOKEN_INSTANCE_SHIFT) & VG_VISIBILITY_INSTANCE_MASK;
}

uint vgEncodePrimitive(uint triangleIndex)
{
    return triangleIndex + 1u;
}

uint vgDecodePrimitive(uint encoded)
{
    return encoded == 0u ? 0xffffffffu : encoded - 1u;
}

uint vgPackBarycentrics(float2 barycentrics)
{
    const uint bx = (uint)round(saturate(barycentrics.x) * 65535.0);
    const uint by = (uint)round(saturate(barycentrics.y) * 65535.0);
    return (bx & 0xffffu) | ((by & 0xffffu) << 16u);
}

float3 vgUnpackBarycentrics(uint packed)
{
    const float x = (packed & 0xffffu) / 65535.0;
    const float y = ((packed >> 16u) & 0xffffu) / 65535.0;
    float3 result = float3(x, y, max(0.0, 1.0 - x - y));
    result /= max(result.x + result.y + result.z, 1.0e-6);
    return result;
}
