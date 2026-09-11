#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(1, 0)]] ByteAddressBuffer geometryPages;
[[vk::binding(2, 0)]] StructuredBuffer<PageTableEntry> geometryPageTable;
[[vk::binding(3, 0)]] StructuredBuffer<GeometryPageInfo> geometryPageInfoBuffer;
[[vk::binding(4, 0)]] StructuredBuffer<TransformRow> transforms;
[[vk::binding(5, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
#include "virtual_geometry_pages.hlsli"
struct VisibilityConstants
{
    float4x4 viewProjection;
    uint meshletCount;
    uint vertexCount;
    uint instanceCount;
    uint meshletVertexTableCount;
};
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> constants;
struct VSOut
{
    float4 position : SV_Position;
    nointerpolation uint visibility : TEXCOORD0;
};
VSOut main(uint vertexId : SV_VertexID, uint drawToken : SV_InstanceID)
{
    VSOut o;
    o.position = float4(0.0, 0.0, 0.0, 0.0);
    o.visibility = 0u;
    const uint meshletId = vgDrawTokenMeshlet(drawToken);
    const uint instanceIndex = vgDrawTokenInstance(drawToken);
    const GeometryPageInfo pageInfo = geometryPageInfoBuffer[0];
    if (meshletId >= constants.meshletCount || instanceIndex >= constants.instanceCount)
        return o;
    MeshletMeta m = meshlets[meshletId];
    if (m.vertexCount == 0u || m.vertexCount > VG_MESHLET_MAX_VERTICES ||
        m.triangleCount == 0u || m.triangleCount > VG_MESHLET_MAX_TRIANGLES ||
        m.indexCount != m.triangleCount * 3u)
        return o;
    (void)pageInfo;
    // The bound index buffer is a shared 0..371 sequence. vertexOffset carries
    // the meshlet's page-local index-stream offset, so SV_VertexID is translated
    // through the resident page table here.
    uint vertexIndex = 0u;
    Vertex v;
    if (!vgLoadIndex(m.pageIndex, vertexId, vertexIndex) ||
        !vgLoadVertex(m.pageIndex, vertexIndex, v))
        return o;
    const float4x4 model = transforms[instanceIndex].model;
    o.position = mul(constants.viewProjection, mul(model, float4(v.position, 1.0)));
    // R32 visibility ABI: zero is the clear/background value. Instance is
    // bits 0..7, (meshlet + 1) is bits 8..27, and material class is bits
    // 28..31. M5's compatible opaque PBR work is class 1; the classification
    // pass resolves the full material index from the instance table.
    // Transparent (bit 0), double-sided (bit 1), and alpha-masked (bit 4)
    // materials stay on the established fallback paths.
    const uint materialClass = (meshMaterials[instanceIndex].flags &
        VG_VISIBILITY_INCOMPATIBLE_MATERIAL_FLAGS) == 0u
        ? VG_VISIBILITY_COMPATIBLE_MATERIAL_CLASS : VG_VISIBILITY_MATERIAL_MASK;
    o.visibility = (instanceIndex & VG_VISIBILITY_INSTANCE_MASK) |
        (((meshletId + 1u) & VG_VISIBILITY_MESHLET_MASK) <<
            VG_VISIBILITY_MESHLET_SHIFT) |
        ((materialClass & VG_VISIBILITY_MATERIAL_MASK) <<
            VG_VISIBILITY_MATERIAL_SHIFT);
    return o;
}
