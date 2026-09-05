// Reversed-Z compatible sphere frustum culling. One invocation handles one
// persistent GPU-scene slot and appends visible slots atomically.
struct BoundsRow { float4 sphereCenterRadius; float4 aabbMin; float4 aabbMax; };
[[vk::binding(0, 0)]] StructuredBuffer<BoundsRow> bounds;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
struct FrustumConstants { float4 planes[6]; uint instanceCount; uint reserved0; uint reserved1; uint reserved2; };
[[vk::push_constant]] ConstantBuffer<FrustumConstants> constants;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint index = id.x;
    if (index >= constants.instanceCount) return;
    const float4 sphere = bounds[index].sphereCenterRadius;
    [unroll] for (uint plane = 0; plane < 6; ++plane)
    {
        if (dot(constants.planes[plane].xyz, sphere.xyz) + constants.planes[plane].w + sphere.w < 0.0)
            return;
    }
    uint outputIndex;
    InterlockedAdd(visibleCount[0], 1, outputIndex);
    visibleInstanceIndices[outputIndex] = index;
}
