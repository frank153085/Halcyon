// GPU clustered-light build. One workgroup processes a contiguous batch of
// clusters; each invocation writes the range and a deterministic light list.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint2> clusterRanges;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> clusterIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> clusterOverflow;
[[vk::binding(3, 0)]] StructuredBuffer<float4> lights;

struct ClusterCamera
{
    float4x4 view;
    float4x4 inverseProjection;
};
[[vk::binding(4, 0)]] ConstantBuffer<ClusterCamera> camera;

struct ClusterConstants
{
    uint clusterCount;
    uint lightCount;
    uint maxLightsPerCluster;
    uint tilesX;
    uint tilesY;
    uint slicesZ;
    float nearPlane;
    float farPlane;
};
[[vk::push_constant]] ConstantBuffer<ClusterConstants> constants;

float3 viewPositionAtDepth(float2 ndc, float depth)
{
    const float4 unprojected = mul(camera.inverseProjection, float4(ndc, 1.0, 1.0));
    const float3 ray = unprojected.xyz / max(abs(unprojected.w), 1e-6);
    return ray * (depth / max(-ray.z, 1e-6));
}

bool sphereIntersectsAabb(float3 center, float radius, float3 minimum, float3 maximum)
{
    const float3 closest = clamp(center, minimum, maximum);
    const float3 delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}

bool spotConeIntersectsCluster(float3 apex, float3 direction, float range, float outerCos,
    float3 clusterCenter, float clusterRadius)
{
    const float3 toCenter = clusterCenter - apex;
    const float axialDistance = dot(toCenter, direction);
    if (axialDistance + clusterRadius < 0.0 || axialDistance - clusterRadius > range)
        return false;
    const float3 radial = toCenter - direction * axialDistance;
    const float safeCos = max(abs(outerCos), 1e-3);
    const float coneRadius = max(axialDistance, 0.0) *
        sqrt(max(0.0, 1.0 - outerCos * outerCos)) / safeCos;
    return length(radial) <= coneRadius + clusterRadius;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint cluster = dispatchId.x;
    if (cluster >= constants.clusterCount)
        return;
    const uint tileCount = constants.tilesX * constants.tilesY;
    const uint slice = cluster / tileCount;
    const uint tile = cluster - slice * tileCount;
    const uint tileX = tile % constants.tilesX;
    const uint tileY = tile / constants.tilesX;
    const float sliceNear = constants.nearPlane * pow(constants.farPlane / constants.nearPlane,
        (float)slice / (float)constants.slicesZ);
    const float sliceFar = constants.nearPlane * pow(constants.farPlane / constants.nearPlane,
        (float)(slice + 1) / (float)constants.slicesZ);
    const float2 ndcMinimum = float2(
        (float)tileX / (float)constants.tilesX,
        (float)tileY / (float)constants.tilesY) * 2.0 - 1.0;
    const float2 ndcMaximum = float2(
        (float)(tileX + 1) / (float)constants.tilesX,
        (float)(tileY + 1) / (float)constants.tilesY) * 2.0 - 1.0;
    float3 boundsMinimum = float3(1e30, 1e30, 1e30);
    float3 boundsMaximum = float3(-1e30, -1e30, -1e30);
    [unroll]
    for (uint depthIndex = 0; depthIndex < 2; ++depthIndex)
    {
        const float viewDepth = depthIndex == 0 ? sliceNear : sliceFar;
        [unroll]
        for (uint y = 0; y < 2; ++y)
        {
            [unroll]
            for (uint x = 0; x < 2; ++x)
            {
                const float2 ndc = float2(x == 0 ? ndcMinimum.x : ndcMaximum.x,
                    y == 0 ? ndcMinimum.y : ndcMaximum.y);
                const float3 corner = viewPositionAtDepth(ndc, viewDepth);
                boundsMinimum = min(boundsMinimum, corner);
                boundsMaximum = max(boundsMaximum, corner);
            }
        }
    }
    const float3 clusterCenter = (boundsMinimum + boundsMaximum) * 0.5;
    const float clusterRadius = length(boundsMaximum - boundsMinimum) * 0.5;
    uint count = 0;
    for (uint i = 0; i < constants.lightCount; ++i)
    {
        const float4 light = lights[i * 4u + 0u];
        const float lightType = lights[i * 4u + 2u].w;
        if (lightType == 1.0)
        {
            // Directional lights affect every screen tile and depth slice.
            if (count < constants.maxLightsPerCluster)
                clusterIndices[cluster * constants.maxLightsPerCluster + count++] = i;
            else
                InterlockedAdd(clusterOverflow[0], 1);
            continue;
        }
        const float3 viewLightPosition = mul(camera.view, float4(light.xyz, 1.0)).xyz;
        bool intersects = sphereIntersectsAabb(viewLightPosition, max(light.w, 0.0),
            boundsMinimum, boundsMaximum);
        if (intersects && lightType == 2.0)
        {
            const float3 viewDirection = normalize(mul((float3x3)camera.view,
                lights[i * 4u + 2u].xyz));
            intersects = spotConeIntersectsCluster(viewLightPosition, viewDirection, light.w,
                lights[i * 4u + 3u].y, clusterCenter, clusterRadius);
        }
        if (intersects)
        {
            if (count < constants.maxLightsPerCluster)
                clusterIndices[cluster * constants.maxLightsPerCluster + count++] = i;
            else
                InterlockedAdd(clusterOverflow[0], 1);
        }
    }
    clusterRanges[cluster] = uint2(cluster * constants.maxLightsPerCluster, count);
}
