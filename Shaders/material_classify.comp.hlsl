#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] Texture2D<uint> visibilityIds;
[[vk::binding(1, 0)]] Texture2D<uint> primitiveIds;
[[vk::binding(2, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> materialIds;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> invalidVisibilityCount;
[[vk::binding(5, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(6, 0)]] ByteAddressBuffer geometryPages;
[[vk::binding(7, 0)]] StructuredBuffer<PageTableEntry> geometryPageTable;
[[vk::binding(8, 0)]] StructuredBuffer<GeometryPageInfo> geometryPageInfoBuffer;
#include "virtual_geometry_pages.hlsli"
struct Constants
{
    uint width;
    uint height;
    uint instanceCount;
    uint materialCount;
    uint meshletCount;
    uint meshletVertexCount;
    uint indexCount;
    uint expectedMeshId;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.width || id.y >= constants.height) return;
    uint pixel = id.y * constants.width + id.x;
    const uint visibility = visibilityIds.Load(int3(id.xy, 0));
    const uint triangleEncoded = primitiveIds.Load(int3(id.xy, 0));
    const uint instanceIndex = vgVisibilityInstance(visibility);
    const uint meshletEncoded = vgVisibilityMeshletEncoded(visibility);
    const uint materialClass = vgVisibilityMaterialClass(visibility);
    if (visibility == 0u && triangleEncoded == 0u)
    {
        materialIds[pixel] = 0xffffffffu;
        return;
    }
    // Class 1 is the only material family supported by the M5 virtual path
    // (static, opaque, single-sided PBR). Reject malformed or incompatible
    // non-background records before they reach compute shading, and expose
    // them through the asynchronous acceptance counter.
    const uint meshletIndex = vgVisibilityMeshlet(visibility);
    if (visibility == 0u || triangleEncoded == 0u ||
        triangleEncoded > VG_MESHLET_MAX_TRIANGLES || meshletEncoded == 0u ||
        meshletIndex >= constants.meshletCount ||
        instanceIndex >= constants.instanceCount ||
        materialClass != VG_VISIBILITY_COMPATIBLE_MATERIAL_CLASS)
    {
        materialIds[pixel] = 0xffffffffu;
        InterlockedAdd(invalidVisibilityCount[0], 1u);
        return;
    }
    const MeshletMeta meshlet = meshlets[meshletIndex];
    if (meshlet.vertexCount == 0u || meshlet.vertexCount > VG_MESHLET_MAX_VERTICES ||
        meshlet.triangleCount == 0u || meshlet.triangleCount > VG_MESHLET_MAX_TRIANGLES ||
        meshlet.indexCount != meshlet.triangleCount * 3u ||
        triangleEncoded > meshlet.triangleCount)
    {
        materialIds[pixel] = 0xffffffffu;
        InterlockedAdd(invalidVisibilityCount[0], 1u);
        return;
    }
    // Keep the compact visibility primitive ID tied to the meshlet's local
    // triangle table. The CPU cache validator performs the same check, but
    // validating it here prevents malformed GPU data from becoming an
    // out-of-range vertex lookup in compute shading.
    const uint triangleBase = meshlet.triangleOffset + (triangleEncoded - 1u) * 3u;
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        uint localIndex = 0u;
        if (!vgLoadTriangleByte(meshlet.pageIndex, triangleBase + corner, localIndex) ||
            localIndex >= meshlet.vertexCount)
        {
            materialIds[pixel] = 0xffffffffu;
            InterlockedAdd(invalidVisibilityCount[0], 1u);
            return;
        }
    }
    // High four bits retain the material class used for classification;
    // the lower 28 bits carry the full material table index for shading.
    const MeshMaterialRow meshMaterial = meshMaterials[instanceIndex];
    if (meshMaterial.meshId != constants.expectedMeshId)
    {
        materialIds[pixel] = 0xffffffffu;
        InterlockedAdd(invalidVisibilityCount[0], 1u);
        return;
    }
    if ((meshMaterial.flags & VG_VISIBILITY_INCOMPATIBLE_MATERIAL_FLAGS) != 0u)
    {
        materialIds[pixel] = 0xffffffffu;
        InterlockedAdd(invalidVisibilityCount[0], 1u);
        return;
    }
    const uint materialIndex = meshMaterial.materialIndex;
    if (materialIndex >= constants.materialCount ||
        materialIndex > VG_VISIBILITY_MATERIAL_INDEX_MASK)
    {
        materialIds[pixel] = 0xffffffffu;
        InterlockedAdd(invalidVisibilityCount[0], 1u);
        return;
    }
    materialIds[pixel] = (visibility &
        (VG_VISIBILITY_MATERIAL_MASK << VG_VISIBILITY_MATERIAL_SHIFT)) | materialIndex;
}
