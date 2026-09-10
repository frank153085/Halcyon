#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleMeshlets;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] Texture2D<float> previousHiZ;
[[vk::binding(4, 0)]] StructuredBuffer<TransformRow> transforms;
struct CullFrame { float4x4 viewProjection; uint4 hiz; };
[[vk::binding(5, 0)]] ConstantBuffer<CullFrame> frame;
struct DagNode {
    uint clusterIndex;
    uint parentIndex;
    uint firstChild;
    uint childCount;
    uint lodDepth;
    uint flags;
    float4 sphere;
    float geometricError;
    float3 _padding;
    uint2 _stridePadding;
};
struct Cluster {
    uint meshletOffset;
    uint meshletCount;
    uint vertexOffset;
    uint vertexCount;
    uint triangleCount;
    uint lodDepth;
    uint primitiveIndex;
    float4 sphere;
    float geometricError;
    float3 _padding;
    uint _stridePadding;
};
[[vk::binding(6, 0)]] StructuredBuffer<DagNode> dagNodes;
[[vk::binding(7, 0)]] StructuredBuffer<Cluster> clusters;
[[vk::binding(8, 0)]] StructuredBuffer<uint> clusterMeshletIndices;
[[vk::binding(9, 0)]] StructuredBuffer<uint> selectedNodes;
[[vk::binding(10, 0)]] StructuredBuffer<uint> selectedCount;
[[vk::binding(11, 0)]] StructuredBuffer<uint> meshletDagNodes;
[[vk::binding(12, 0)]] StructuredBuffer<uint4> lodStates;
struct CullConstants { float4 planes[6]; float4 cameraPosition; uint meshletCount; uint instanceIndex; uint lodAndFlags; uint visibleCapacity; };
[[vk::push_constant]] ConstantBuffer<CullConstants> constants;

float meshletRadiusScale(float4x4 model)
{
    const float3 x = mul(model, float4(1.0, 0.0, 0.0, 0.0)).xyz;
    const float3 y = mul(model, float4(0.0, 1.0, 0.0, 0.0)).xyz;
    const float3 z = mul(model, float4(0.0, 0.0, 1.0, 0.0)).xyz;
    const float3 squaredLengths = float3(dot(x, x), dot(y, y), dot(z, z));
    const float maximumSquared = max(squaredLengths.x,
        max(squaredLengths.y, squaredLengths.z));
    const float crossAxisDot = max(abs(dot(x, y)),
        max(abs(dot(x, z)), abs(dot(y, z))));
    // For an orthogonal TRS basis the largest axis is the exact sphere scale.
    // Under shear, the Frobenius norm is a conservative upper bound for the
    // largest singular value and therefore for the transformed sphere.
    return crossAxisDot <= max(maximumSquared, 1.0e-12) * 1.0e-4
        ? sqrt(maximumSquared) : sqrt(dot(squaredLengths, 1.0.xxx));
}

bool survivesHiZ(MeshletMeta meshlet)
{
    const bool enabled = (constants.lodAndFlags & VG_CULL_HIZ_ENABLED_FLAG) != 0u && frame.hiz.w != 0u;
    if (!enabled) return true;

    const float4x4 model = transforms[constants.instanceIndex].model;
    const float3 center = mul(model, float4(meshlet.sphere.xyz, 1.0)).xyz;
    // Use the same conservative singular-value upper bound as the frustum
    // path. The largest basis length is exact for orthogonal TRS, but can
    // under-estimate a sheared transform and incorrectly reject a meshlet.
    const float scale = meshletRadiusScale(model);
    const float radius = meshlet.sphere.w * scale;
    float2 uvMin = 1.0.xx;
    float2 uvMax = 0.0.xx;
    float nearestDepth = 0.0;
    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 offset = float3((corner & 1u) != 0u ? radius : -radius,
            (corner & 2u) != 0u ? radius : -radius,
            (corner & 4u) != 0u ? radius : -radius);
        const float4 clip = mul(frame.viewProjection, float4(center + offset, 1.0));
        if (clip.w <= 1.0e-5) return true;
        const float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        nearestDepth = max(nearestDepth, saturate(clip.z / clip.w));
    }
    if (any(uvMax <= 0.0) || any(uvMin >= 1.0)) return true;
    uvMin = saturate(uvMin);
    uvMax = saturate(uvMax);
    const float footprint = max((uvMax.x - uvMin.x) * frame.hiz.x,
        (uvMax.y - uvMin.y) * frame.hiz.y);
    const uint mip = min(frame.hiz.z,
        (uint)max(0.0, ceil(log2(max(footprint, 2.0))) - 1.0));
    uint mipWidth, mipHeight, mipCount;
    previousHiZ.GetDimensions(mip, mipWidth, mipHeight, mipCount);
    const uint2 lastTexel = uint2(max(1u, mipWidth) - 1u, max(1u, mipHeight) - 1u);
    const uint2 first = min((uint2)(uvMin * float2(mipWidth, mipHeight)), lastTexel);
    const uint2 last = min((uint2)(uvMax * float2(mipWidth, mipHeight)), lastTexel);
    const float pyramidDepth = min(min(previousHiZ.Load(int3(first, mip)),
        previousHiZ.Load(int3(uint2(last.x, first.y), mip))),
        min(previousHiZ.Load(int3(uint2(first.x, last.y), mip)),
            previousHiZ.Load(int3(last, mip))));
    return pyramidDepth <= 0.0 || nearestDepth >= pyramidDepth - 0.0005;
}

bool supportsObjectSpaceCone(float4x4 model)
{
    const float3 x = mul(model, float4(1.0, 0.0, 0.0, 0.0)).xyz;
    const float3 y = mul(model, float4(0.0, 1.0, 0.0, 0.0)).xyz;
    const float3 z = mul(model, float4(0.0, 0.0, 1.0, 0.0)).xyz;
    const float sx = length(x);
    const float sy = length(y);
    const float sz = length(z);
    const float maximum = max(sx, max(sy, sz));
    const float minimum = min(sx, min(sy, sz));
    // A normal cone remains valid in object space only for an approximately
    // uniform, orthogonal scale. Skip it for non-uniform or sheared transforms
    // to stay conservative.
    const float orthogonality = max(abs(dot(x, y)),
        max(abs(dot(x, z)), abs(dot(y, z))));
    return maximum > 1.0e-5 && (maximum - minimum) <= maximum * 1.0e-3 &&
        orthogonality <= maximum * maximum * 1.0e-4;
}

void cullMeshlet(uint meshletIndex)
{
    if (meshletIndex >= constants.meshletCount) return;
    MeshletMeta m = meshlets[meshletIndex];
    const float4x4 model = transforms[constants.instanceIndex].model;
    // Frustum planes stay in world space so non-uniform and sheared instance
    // transforms can use a conservative transformed sphere radius.
    const float3 worldCenter = mul(model, float4(m.sphere.xyz, 1.0)).xyz;
    const float worldRadius = m.sphere.w * meshletRadiusScale(model);
    bool inside = true;
    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        if (dot(constants.planes[planeIndex].xyz, worldCenter) +
            constants.planes[planeIndex].w + worldRadius < 0.0)
        {
            inside = false;
            break;
        }
    }
    if (!inside) return;
    if (!survivesHiZ(m)) return;
    // meshopt stores a conservative normal cone as axis.xyz and cutoff.w.
    // A cone with a negative cutoff is effectively two-sided and is retained.
    const float coneAxisLength = length(m.cone.xyz);
    if (supportsObjectSpaceCone(model) && coneAxisLength > 1.0e-5 && m.cone.w >= -0.9999)
    {
        // meshoptimizer's conservative no-apex test is:
        // dot(center - camera, axis) >= cutoff * distance + radius.
        // Keep the calculation in object space because both the cached
        // bounds and the camera push constant are object-space values.
        const float3 centerFromCamera = m.sphere.xyz - constants.cameraPosition.xyz;
        const float distance = length(centerFromCamera);
        if (distance > 1e-5 && dot(normalize(m.cone.xyz), centerFromCamera) >=
            m.cone.w * distance + m.sphere.w)
            return;
    }
    uint dst = 0u;
    InterlockedAdd(visibleCount[0], 1u, dst);
    if (dst < constants.visibleCapacity)
    {
        // The upper byte identifies the instance; the low 20 bits identify
        // the meshlet in the shared virtual asset table.
        visibleMeshlets[dst] = vgPackDrawToken(constants.instanceIndex, meshletIndex);
    }
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    // The LOD pass publishes a compact frontier. Expanding only its clusters
    // avoids the previous O(total asset meshlets) scan, which dominated Lucy.
    const uint frontierCount = min(selectedCount[0], constants.visibleCapacity);
    if (id.x >= frontierCount) return;
    const uint nodeIndex = selectedNodes[id.x];
    const DagNode node = dagNodes[nodeIndex];
    const Cluster cluster = clusters[node.clusterIndex];
    [loop]
    for (uint localIndex = 0u; localIndex < cluster.meshletCount; ++localIndex)
    {
        const uint meshletListIndex = cluster.meshletOffset + localIndex;
        cullMeshlet(clusterMeshletIndices[meshletListIndex]);
    }
}
