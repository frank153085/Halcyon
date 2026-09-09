#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] Texture2D<uint> visibilityIds;
[[vk::binding(1, 0)]] Texture2D<uint> primitiveIds;
[[vk::binding(2, 0)]] Texture2D<uint> barycentricIds;
[[vk::binding(3, 0)]] StructuredBuffer<uint> materialIds;
[[vk::binding(4, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> hdr;
[[vk::binding(5, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(6, 0)]] StructuredBuffer<uint> indices;
[[vk::binding(7, 0)]] StructuredBuffer<Vertex> vertices;
[[vk::binding(8, 0)]] TextureCube irradianceMap;
[[vk::binding(9, 0)]] TextureCube prefilteredEnvironment;
[[vk::binding(10, 0)]] SamplerState linearSampler;
[[vk::binding(11, 0)]] Texture2D brdfLut;
[[vk::binding(12, 0)]] StructuredBuffer<MaterialGpuData> materials;
[[vk::binding(13, 0)]] StructuredBuffer<LightData> lights;
[[vk::binding(14, 0)]] StructuredBuffer<TransformRow> transforms;
[[vk::binding(15, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motionVectors;

struct Constants
{
    uint width;
    uint height;
    uint instanceCount;
    uint meshletCount;
    uint vertexCount;
    uint indexCount;
    uint materialCount;
    uint lightCount;
    float4 cameraAndAmbient;
    float4x4 previousViewProjection;
    float4 fallbackLight;
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

static const float PI = 3.14159265359;

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float distributionGgx(float nDotH, float roughness)
{
    const float a = max(0.045, roughness * roughness);
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float geometrySchlick(float nDot, float roughness)
{
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return nDot / max(nDot * (1.0 - k) + k, 1e-6);
}

float3 evaluateDirectional(float3 n, float3 viewDirection, float3 albedo,
    float metallic, float roughness, float3 direction, float3 color, float intensity)
{
    const float directionLength = length(direction);
    if (directionLength <= 1.0e-5)
        return 0.0.xxx;
    const float3 l = normalize(-direction);
    const float3 halfVector = viewDirection + l;
    const float halfLength = length(halfVector);
    const float3 h = halfLength > 1.0e-5 ? halfVector / halfLength : n;
    const float nDotV = saturate(dot(n, viewDirection));
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 f = fresnelSchlick(saturate(dot(viewDirection, h)), f0);
    const float specular = distributionGgx(nDotH, roughness) *
        geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness) /
        max(4.0 * nDotV * nDotL, 1e-5);
    const float3 kd = (1.0 - f) * (1.0 - metallic);
    return (kd * albedo / PI + specular * f) * nDotL * color * intensity;
}

float3 evaluatePoint(float3 worldPosition, float3 n, float3 viewDirection,
    float3 albedo, float metallic, float roughness, float3 lightPosition,
    float lightRadius, float3 color, float intensity)
{
    const float3 toLight = lightPosition - worldPosition;
    const float distanceToLight = length(toLight);
    if (distanceToLight <= 1.0e-4 || distanceToLight > max(lightRadius, 1.0e-4))
        return 0.0.xxx;
    const float3 l = toLight / distanceToLight;
    const float3 halfVector = viewDirection + l;
    const float halfLength = length(halfVector);
    const float3 h = halfLength > 1.0e-5 ? halfVector / halfLength : n;
    const float nDotV = saturate(dot(n, viewDirection));
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 f = fresnelSchlick(saturate(dot(viewDirection, h)), f0);
    const float specular = distributionGgx(nDotH, roughness) *
        geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness) /
        max(4.0 * nDotV * nDotL, 1e-5);
    const float3 kd = (1.0 - f) * (1.0 - metallic);
    const float attenuation = saturate(1.0 - distanceToLight / max(lightRadius, 1e-4));
    return (kd * albedo / PI + specular * f) * nDotL * color * intensity *
        attenuation * attenuation;
}

float3 evaluateSpot(float3 worldPosition, float3 n, float3 viewDirection,
    float3 albedo, float metallic, float roughness, float3 lightPosition,
    float lightRadius, float3 lightDirection, float innerCone, float outerCone,
    float3 color, float intensity)
{
    const float3 toSurface = worldPosition - lightPosition;
    const float distanceToLight = length(toSurface);
    if (distanceToLight <= 1.0e-4 || distanceToLight > max(lightRadius, 1.0e-4))
        return 0.0.xxx;
    const float cone = saturate((dot(normalize(toSurface), normalize(lightDirection)) -
        outerCone) / max(innerCone - outerCone, 1.0e-4));
    return evaluatePoint(worldPosition, n, viewDirection, albedo, metallic, roughness,
        lightPosition, lightRadius, color, intensity * cone);
}

float3 transformNormal(float3 objectNormal, float4x4 model)
{
    // Rebuild the inverse-transpose from the upper 3x3 so non-uniform Lucy
    // transforms keep the reconstructed normal orthogonal to the surface
    // without expanding the 128-byte push-constant ABI.
    const float3x3 linearMatrix = (float3x3)model;
    const float3 c0 = cross(linearMatrix[1], linearMatrix[2]);
    const float3 c1 = cross(linearMatrix[2], linearMatrix[0]);
    const float3 c2 = cross(linearMatrix[0], linearMatrix[1]);
    const float determinant = dot(linearMatrix[0], c0);
    if (abs(determinant) < 1e-6)
        return normalize(mul(linearMatrix, objectNormal));
    // linear[] addresses rows. These three cross products are the rows of
    // the cofactor matrix, which is inverse-transpose(linear) * determinant.
    const float3x3 normalMatrix = float3x3(c0, c1, c2) / determinant;
    return normalize(mul(normalMatrix, objectNormal));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.width || id.y >= constants.height) return;
    motionVectors[id.xy] = 0.0.xx;
    const uint pixel = id.y * constants.width + id.x;
    const uint visibility = visibilityIds.Load(int3(id.xy, 0));
    const uint triangleEncoded = primitiveIds.Load(int3(id.xy, 0));
    if (visibility == 0u || triangleEncoded == 0u)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }

    const uint meshletEncoded = vgVisibilityMeshletEncoded(visibility);
    if (meshletEncoded == 0u)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const uint meshletIndex = vgVisibilityMeshlet(visibility);
    const uint instanceIndex = vgVisibilityInstance(visibility);
    const uint triangleIndex = vgDecodePrimitive(triangleEncoded);
    if (meshletIndex >= constants.meshletCount || instanceIndex >= constants.instanceCount)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const uint visibilityClass = vgVisibilityMaterialClass(visibility);
    const uint classifiedMaterial = materialIds[pixel];
    const uint classifiedClass = vgVisibilityMaterialClass(classifiedMaterial);
    if (visibilityClass != VG_VISIBILITY_COMPATIBLE_MATERIAL_CLASS ||
        classifiedClass != visibilityClass)
    {
        // Classification rejects incompatible or malformed visibility IDs.
        // Keep those pixels as background instead of accidentally shading
        // them with material slot zero.
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const MeshletMeta meshlet = meshlets[meshletIndex];
    if (meshlet.vertexCount == 0u || meshlet.vertexCount > VG_MESHLET_MAX_VERTICES ||
        meshlet.triangleCount == 0u || meshlet.triangleCount > VG_MESHLET_MAX_TRIANGLES ||
        meshlet.indexCount != meshlet.triangleCount * 3u ||
        meshlet.indexOffset > constants.indexCount ||
        meshlet.indexCount > constants.indexCount - meshlet.indexOffset ||
        triangleIndex >= meshlet.triangleCount ||
        triangleIndex >= meshlet.indexCount / 3u)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const uint triangleBase = meshlet.indexOffset + triangleIndex * 3u;
    const uint i0 = indices[triangleBase + 0u];
    const uint i1 = indices[triangleBase + 1u];
    const uint i2 = indices[triangleBase + 2u];
    if (i0 >= constants.vertexCount || i1 >= constants.vertexCount || i2 >= constants.vertexCount)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }

    const uint packedBary = barycentricIds.Load(int3(id.xy, 0));
    const float3 weights = vgUnpackBarycentrics(packedBary);
    if (any(isnan(weights)) || any(isinf(weights)) ||
        any(weights < -1.0e-4) || abs(weights.x + weights.y + weights.z - 1.0) > 1.0e-3)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const Vertex v0 = vertices[i0];
    const Vertex v1 = vertices[i1];
    const Vertex v2 = vertices[i2];
    const float3 objectPosition = v0.position * weights.x + v1.position * weights.y +
        v2.position * weights.z;
    // Keep the complete triangle attribute reconstruction in the visibility
    // path. Lucy currently uses factor-only PBR materials, but the UV is
    // still reconstructed here so a future texture-backed material can bind
    // the same barycentric ABI without changing triangle lookup.
    const float2 uv = v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z;
    if (any(isnan(uv)) || any(isinf(uv)))
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const float4x4 model = transforms[instanceIndex].model;
    const float4 world = mul(model, float4(objectPosition, 1.0));
    const float3 worldPosition = world.xyz;
    if (any(isnan(worldPosition)) || any(isinf(worldPosition)) ||
        any(isnan(objectPosition)) || any(isinf(objectPosition)))
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const float4 previousClip = mul(constants.previousViewProjection, world);
    if (previousClip.w > 1e-6)
    {
        // The current NDC is known from the visibility-buffer pixel. This
        // avoids carrying a second matrix in the 128-byte push-constant ABI.
        const float2 currentNdc =
            ((float2(id.xy) + 0.5) / float2(constants.width, constants.height)) * 2.0 - 1.0;
        const float2 previousNdc = previousClip.xy / previousClip.w;
        motionVectors[id.xy] = (currentNdc - previousNdc) * 0.5;
    }
    float3 objectNormal = v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z;
    const float normalLength = length(objectNormal);
    objectNormal = normalLength > 1.0e-5 ? objectNormal / normalLength : float3(0.0, 0.0, 1.0);
    const float3 n = transformNormal(objectNormal, model);
    if (any(isnan(n)) || any(isinf(n)) || length(n) <= 1.0e-5)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    const uint materialIndex = vgVisibilityMaterialIndex(classifiedMaterial);
    if (materialIndex >= constants.materialCount)
    {
        hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0);
        return;
    }
    MaterialGpuData material = materials[materialIndex];
    if (material.baseColorFactor.a <= 0.0)
    {
        material.baseColorFactor = float4(0.72, 0.74, 0.78, 1.0);
        material.emissiveFactor.w = 1.0;
        material.factors = float4(0.0, 1.0, 0.5, 0.0);
    }
    const float3 albedo = max(material.baseColorFactor.rgb, 0.0);
    const float metallic = saturate(material.factors.x);
    const float roughness = max(0.045, saturate(material.factors.y));
    const float ao = saturate(material.emissiveFactor.w);
    const float3 toCamera = constants.cameraAndAmbient.xyz - worldPosition;
    const float cameraDistance = length(toCamera);
    const float3 viewDirection = cameraDistance > 1.0e-5
        ? toCamera / cameraDistance : float3(0.0, 0.0, 1.0);

    float3 direct = 0.0.xxx;
    bool hasLight = false;
    const uint count = min(constants.lightCount, 1024u);
    for (uint lightIndex = 0u; lightIndex < count; ++lightIndex)
    {
        const LightData light = lights[lightIndex];
        if (light.directionAndType.w > 1.5)
        {
            hasLight = true;
            direct += evaluateSpot(worldPosition, n, viewDirection, albedo, metallic,
                roughness, light.positionAndRadius.xyz, light.positionAndRadius.w,
                light.directionAndType.xyz, light.spotParams.x, light.spotParams.y,
                light.colorAndIntensity.rgb, light.colorAndIntensity.w);
        }
        else if (light.directionAndType.w > 0.5)
        {
            hasLight = true;
            direct += evaluateDirectional(n, viewDirection, albedo, metallic, roughness,
                light.directionAndType.xyz, light.colorAndIntensity.rgb,
                light.colorAndIntensity.w);
        }
        else
        {
            hasLight = true;
            direct += evaluatePoint(worldPosition, n, viewDirection, albedo, metallic,
                roughness, light.positionAndRadius.xyz, light.positionAndRadius.w,
                light.colorAndIntensity.rgb, light.colorAndIntensity.w);
        }
    }
    if (!hasLight)
        direct = evaluateDirectional(n, viewDirection, albedo, metallic, roughness,
            constants.fallbackLight.xyz, float3(1.0, 0.96, 0.9), constants.fallbackLight.w);

    const float nDotV = saturate(dot(n, viewDirection));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 fresnel = fresnelSchlick(nDotV, f0);
    const float3 reflected = reflect(-viewDirection, n);
    const float3 irradiance = irradianceMap.SampleLevel(linearSampler, n, 0).rgb;
    const float3 prefiltered = prefilteredEnvironment.SampleLevel(linearSampler, reflected,
        roughness * 6.0).rgb;
    const float2 brdf = brdfLut.SampleLevel(linearSampler, float2(nDotV, roughness), 0).rg;
    const float3 ambient = irradiance * albedo * (1.0 - fresnel) * (1.0 - metallic) / PI +
        prefiltered * (fresnel * brdf.x + brdf.y);
    hdr[id.xy] = float4(max(direct + ambient * ao * constants.cameraAndAmbient.w +
        material.emissiveFactor.rgb, 0.0), 1.0);
}
