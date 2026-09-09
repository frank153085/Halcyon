#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(2, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(4, 0)]] ByteAddressBuffer meshletTriangles;
[[vk::binding(5, 0)]] StructuredBuffer<Vertex> vertices;
[[vk::binding(6, 0)]] StructuredBuffer<TransformRow> transforms;
[[vk::binding(7, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;

struct VisibilityConstants
{
    float4x4 viewProjection;
    uint meshletCount;
    uint vertexCount;
    uint instanceCount;
    uint meshletVertexTableCount;
};
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> constants;

struct MeshVertex
{
    float4 position : SV_Position;
    nointerpolation uint visibility : TEXCOORD0;
};

struct MeshPrimitive
{
    nointerpolation uint primitive : TEXCOORD1;
};

uint triangleByte(uint byteOffset)
{
    const uint word = meshletTriangles.Load(byteOffset & ~3u);
    return (word >> ((byteOffset & 3u) * 8u)) & 0xffu;
}

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID,
    out vertices MeshVertex outputVertices[64],
    out indices uint3 outputTriangles[124],
    out primitives MeshPrimitive outputPrimitives[124])
{
    const uint drawToken = groupId.x < visibleCount[0] ? visibleMeshlets[groupId.x] : 0u;
    const uint meshletIndex = vgDrawTokenMeshlet(drawToken);
    const uint instanceIndex = vgDrawTokenInstance(drawToken);
    if (meshletIndex >= constants.meshletCount || instanceIndex >= constants.instanceCount)
    {
        SetMeshOutputCounts(0u, 0u);
        return;
    }
    const MeshletMeta meshlet = meshlets[meshletIndex];
    const uint vertexCount = min(meshlet.vertexCount, VG_MESHLET_MAX_VERTICES);
    const uint triangleCount = min(meshlet.triangleCount, VG_MESHLET_MAX_TRIANGLES);
    SetMeshOutputCounts(vertexCount, triangleCount);
    const float4x4 model = transforms[instanceIndex].model;
    // Mesh shader workgroups are required to support 32 invocations on the
    // devices accepted by the renderer, while a meshlet may contain up to 64
    // vertices and 124 triangles. Each invocation therefore emits a strided
    // subset of the output arrays instead of leaving the second half of a
    // meshlet uninitialized.
    for (uint vertexIndex = groupThreadId.x; vertexIndex < vertexCount; vertexIndex += 32u)
    {
        const uint sourceIndex = meshletVertices[meshlet.vertexOffset + vertexIndex];
        MeshVertex output;
        output.position = sourceIndex < constants.vertexCount
            ? mul(constants.viewProjection, mul(model, float4(vertices[sourceIndex].position, 1.0)))
            : float4(0.0, 0.0, 0.0, 0.0);
        const uint materialClass = (meshMaterials[instanceIndex].flags &
            VG_VISIBILITY_INCOMPATIBLE_MATERIAL_FLAGS) == 0u
            ? VG_VISIBILITY_COMPATIBLE_MATERIAL_CLASS : VG_VISIBILITY_MATERIAL_MASK;
        output.visibility = (instanceIndex & VG_VISIBILITY_INSTANCE_MASK) |
            (((meshletIndex + 1u) & VG_VISIBILITY_MESHLET_MASK) << VG_VISIBILITY_MESHLET_SHIFT) |
            ((materialClass & VG_VISIBILITY_MATERIAL_MASK) << VG_VISIBILITY_MATERIAL_SHIFT);
        outputVertices[vertexIndex] = output;
    }
    for (uint triangleIndex = groupThreadId.x; triangleIndex < triangleCount; triangleIndex += 32u)
    {
        const uint triangleOffset = meshlet.triangleOffset + triangleIndex * 3u;
        outputTriangles[triangleIndex] = uint3(
            triangleByte(triangleOffset), triangleByte(triangleOffset + 1u),
            triangleByte(triangleOffset + 2u));
        outputPrimitives[triangleIndex].primitive = triangleIndex;
    }
}
