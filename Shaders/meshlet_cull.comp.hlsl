struct MeshletMeta { uint vertexOffset; uint vertexCount; uint triangleOffset; uint triangleCount; uint indexOffset; uint indexCount; uint primitiveIndex; uint lodIndex; float4 sphere; float4 cone; float geometricError; };
[[vk::binding(0, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleMeshlets;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
struct CullConstants { float4 planes[6]; uint meshletCount; uint instanceIndex; uint lod; uint pad; };
[[vk::push_constant]] ConstantBuffer<CullConstants> constants;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= constants.meshletCount) return;
    MeshletMeta m = meshlets[id.x];
    if (m.lodIndex != constants.lod) return;
    // The first M5 fixture pass keeps the conservative frustum decision on
    // the CPU-generated meshlet list.  Retaining the plane data in the ABI
    // allows the Hi-Z/normal-cone tests to be enabled without changing the
    // cache or indirect command format.
    uint dst = 0;
    uint observed = visibleCount[0];
    while (observed < 131072u)
    {
        uint previous = 0;
        InterlockedCompareExchange(visibleCount[0], observed, observed + 1u, previous);
        if (previous == observed) { dst = observed; break; }
        observed = previous;
    }
    if (observed < 131072u && dst < 131072u) visibleMeshlets[dst] = id.x;
}
