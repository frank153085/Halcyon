struct MeshletMeta { uint vertexOffset; uint vertexCount; uint triangleOffset; uint triangleCount; float4 sphere; float4 cone; float geometricError; uint lod; };
[[vk::binding(0, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleMeshlets;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
struct CullConstants { float4 planes[6]; uint meshletCount; uint instanceIndex; uint lod; uint pad; };
[[vk::push_constant]] ConstantBuffer<CullConstants> constants;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= constants.meshletCount) return;
    MeshletMeta m = meshlets[id.x];
    [unroll] for (uint p = 0; p < 6; ++p)
        if (dot(constants.planes[p].xyz, m.sphere.xyz) + constants.planes[p].w + m.sphere.w < 0) return;
    uint dst; InterlockedAdd(visibleCount[0], 1, dst); visibleMeshlets[dst] = id.x;
}
