// Reversed-Z compatible sphere frustum culling. One invocation handles one
// persistent GPU-scene slot and appends visible slots atomically.
struct BoundsRow { float4 sphereCenterRadius; float4 aabbMin; float4 aabbMax; };
struct MeshMaterialRow { uint meshIndex; uint materialIndex; uint flags; uint lodState; };
[[vk::binding(0, 0)]] StructuredBuffer<BoundsRow> bounds;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
// Reserved output ABI for M5 LOD selection. The current builder still uses
// the slot index list, but preserves the corresponding state in parallel.
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> visibleLodStates;
struct FrustumConstants { float4 planes[6]; uint instanceCount; uint reserved0; uint reserved1; uint reserved2; };
[[vk::push_constant]] ConstantBuffer<FrustumConstants> constants;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint index = id.x;
    if (index >= constants.instanceCount) return;
    const float4 sphere = bounds[index].sphereCenterRadius;
    if (sphere.w <= 0.0) return;
    [unroll] for (uint plane = 0; plane < 6; ++plane)
    {
        if (dot(constants.planes[plane].xyz, sphere.xyz) + constants.planes[plane].w + sphere.w < 0.0)
            return;
    }
    uint outputIndex;
    InterlockedAdd(visibleCount[0], 1, outputIndex);
    visibleInstanceIndices[outputIndex] = index;
    visibleLodStates[outputIndex] = meshMaterials[index].lodState;
}
