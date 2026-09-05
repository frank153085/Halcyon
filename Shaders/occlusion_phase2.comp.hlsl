// Phase 2 re-tests only the objects rejected by the previous-frame pyramid
// against the current-frame pyramid. This is intentionally conservative for
// missing/undefined pyramid samples.
struct BoundsRow { float4 sphereCenterRadius; float4 aabbMin; float4 aabbMax; };
[[vk::binding(0, 0)]] StructuredBuffer<BoundsRow> bounds;
[[vk::binding(1, 0)]] StructuredBuffer<uint> occluded;
[[vk::binding(2, 0)]] StructuredBuffer<uint> occludedCount;
[[vk::binding(3, 0)]] Texture2D<float> currentHiZ;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> phase2Visible;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> phase2VisibleCount;

struct Constants
{
    float4x4 viewProjection;
    uint2 extent;
    uint maxMip;
    float depthBias;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

bool survivesOcclusion(BoundsRow instanceBounds)
{
    float2 uvMin = float2(1.0, 1.0);
    float2 uvMax = float2(0.0, 0.0);
    float nearestDepth = 0.0;
    [unroll] for (uint corner = 0; corner < 8; ++corner)
    {
        const float3 position = float3(
            (corner & 1) ? instanceBounds.aabbMax.x : instanceBounds.aabbMin.x,
            (corner & 2) ? instanceBounds.aabbMax.y : instanceBounds.aabbMin.y,
            (corner & 4) ? instanceBounds.aabbMax.z : instanceBounds.aabbMin.z);
        const float4 clip = mul(constants.viewProjection, float4(position, 1.0));
        if (clip.w <= 1e-5) return true;
        const float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        nearestDepth = max(nearestDepth, saturate(clip.z / clip.w));
    }
    if (any(uvMax <= 0.0) || any(uvMin >= 1.0)) return true;
    uvMin = saturate(uvMin);
    uvMax = saturate(uvMax);
    const float footprint = max((uvMax.x - uvMin.x) * constants.extent.x,
        (uvMax.y - uvMin.y) * constants.extent.y);
    const uint mip = min(constants.maxMip,
        (uint)max(0.0, ceil(log2(max(footprint, 2.0))) - 1.0));
    uint mipWidth, mipHeight, mipCount;
    currentHiZ.GetDimensions(mip, mipWidth, mipHeight, mipCount);
    const uint2 lastTexel = uint2(max(1u, mipWidth) - 1u, max(1u, mipHeight) - 1u);
    const uint2 first = min((uint2)(uvMin * float2(mipWidth, mipHeight)), lastTexel);
    const uint2 last = min((uint2)(uvMax * float2(mipWidth, mipHeight)), lastTexel);
    const float pyramidDepth = min(min(currentHiZ.Load(int3(first, mip)),
        currentHiZ.Load(int3(uint2(last.x, first.y), mip))),
        min(currentHiZ.Load(int3(uint2(first.x, last.y), mip)),
            currentHiZ.Load(int3(last, mip))));
    return pyramidDepth <= 0.0 || nearestDepth >= pyramidDepth - constants.depthBias;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint candidateIndex = id.x;
    const uint count = occludedCount[0];
    if (candidateIndex >= count) return;
    const uint instance = occluded[candidateIndex];
    const bool visible = survivesOcclusion(bounds[instance]);
    if (visible)
    {
        uint outputIndex;
        InterlockedAdd(phase2VisibleCount[0], 1, outputIndex);
        phase2Visible[outputIndex] = instance;
    }
}
