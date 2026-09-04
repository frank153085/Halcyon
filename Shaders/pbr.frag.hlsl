struct FragmentIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D gbuffer0;
[[vk::binding(1, 0)]] Texture2D gbuffer1;
[[vk::binding(2, 0)]] Texture2D gbuffer2;
[[vk::binding(3, 0)]] Texture2D sceneDepth;
[[vk::binding(4, 0)]] Texture2DArray shadowMap;
[[vk::binding(5, 0)]] TextureCube irradianceMap;
[[vk::binding(6, 0)]] TextureCube prefilteredEnvironment;
[[vk::binding(7, 0)]] Texture2D brdfLut;
[[vk::binding(10, 0)]] SamplerState linearSampler;

[[vk::binding(20, 0)]] StructuredBuffer<uint2> clusterRanges;
[[vk::binding(21, 0)]] StructuredBuffer<uint> clusterIndices;
[[vk::binding(22, 0)]] StructuredBuffer<float4> lights;

struct ShadowConstants
{
    float4x4 lightViewProjection[4];
    float4 cascadeSplits;
    float4 shadowParams;
};
[[vk::binding(23, 0)]] ConstantBuffer<ShadowConstants> shadowConstants;

struct DeferredConstants
{
    float4x4 inverseViewProjection;
    float4 cameraPosition;
    float4 viewportAndInvViewport;
    uint4 clusterParams;
    float4 depthParams;
};
[[vk::push_constant]] ConstantBuffer<DeferredConstants> constants;

static const float PI = 3.14159265359;

float distributionGgx(float nDotH, float roughness)
{
    const float a = max(0.045, roughness * roughness);
    const float a2 = a * a;
    const float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-6);
}

float geometrySchlick(float nDotV, float roughness)
{
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 1e-6);
}

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float3 decodeOctahedral(float2 encoded)
{
    float3 n = float3(encoded * 2.0 - 1.0, 1.0 - abs(encoded.x * 2.0 - 1.0) -
        abs(encoded.y * 2.0 - 1.0));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

float3 evaluatePointLight(float3 worldPosition, float3 n, float3 viewDirection,
    float3 albedo, float metallic, float roughness, float3 lightPosition,
    float lightRadius, float3 lightColor, float lightIntensity)
{
    const float3 toLight = lightPosition - worldPosition;
    const float distanceToLight = length(toLight);
    if (distanceToLight <= 1e-4 || distanceToLight > max(lightRadius, 1e-4))
        return 0.0.xxx;
    const float3 l = toLight / distanceToLight;
    const float3 h = normalize(viewDirection + l);
    const float nDotV = saturate(dot(n, viewDirection));
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float vDotH = saturate(dot(viewDirection, h));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 f = fresnelSchlick(vDotH, f0);
    const float specular = distributionGgx(nDotH, roughness) *
        geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness) /
        max(4.0 * nDotV * nDotL, 1e-5);
    const float3 kd = (1.0 - f) * (1.0 - metallic);
    const float attenuation = saturate(1.0 - distanceToLight / max(lightRadius, 1e-4));
    return (kd * albedo / PI + specular * f) * nDotL * lightColor * lightIntensity * attenuation * attenuation;
}

float3 evaluateDirectionalLight(float3 worldPosition, float3 n, float3 viewDirection,
    float3 albedo, float metallic, float roughness, float3 lightDirection,
    float3 lightColor, float lightIntensity)
{
    const float3 l = normalize(-lightDirection);
    const float3 h = normalize(viewDirection + l);
    const float nDotV = saturate(dot(n, viewDirection));
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 f = fresnelSchlick(saturate(dot(viewDirection, h)), f0);
    const float specular = distributionGgx(nDotH, roughness) *
        geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness) /
        max(4.0 * nDotV * nDotL, 1e-5);
    const float3 kd = (1.0 - f) * (1.0 - metallic);
    return (kd * albedo / PI + specular * f) * nDotL * lightColor * lightIntensity;
}

float3 evaluateSpotLight(float3 worldPosition, float3 n, float3 viewDirection,
    float3 albedo, float metallic, float roughness, float3 lightPosition, float lightRadius,
    float3 lightDirection, float innerCone, float outerCone, float3 lightColor,
    float lightIntensity)
{
    const float3 toSurface = worldPosition - lightPosition;
    const float distanceToLight = length(toSurface);
    if (distanceToLight <= 1e-4 || distanceToLight > max(lightRadius, 1e-4))
        return 0.0.xxx;
    const float cone = saturate((dot(normalize(toSurface), normalize(lightDirection)) - outerCone) /
        max(innerCone - outerCone, 1e-4));
    return evaluatePointLight(worldPosition, n, viewDirection, albedo, metallic, roughness,
        lightPosition, lightRadius, lightColor, lightIntensity * cone);
}

float sampleCascadeShadow(float3 worldPosition, float viewDistance, float nDotL)
{
    uint cascade = 0;
    cascade += viewDistance > shadowConstants.cascadeSplits.x;
    cascade += viewDistance > shadowConstants.cascadeSplits.y;
    cascade += viewDistance > shadowConstants.cascadeSplits.z;
    cascade = min(cascade, 3u);
    const float4 lightClip = mul(shadowConstants.lightViewProjection[cascade],
        float4(worldPosition, 1.0));
    if (abs(lightClip.w) < 1e-6)
        return 1.0;
    const float3 lightNdc = lightClip.xyz / lightClip.w;
    const float2 shadowUv = lightNdc.xy * 0.5 + 0.5;
    if (any(shadowUv < 0.0) || any(shadowUv > 1.0) || lightNdc.z < 0.0 || lightNdc.z > 1.0)
        return 1.0;
    const float texelSize = shadowConstants.shadowParams.x;
    const float bias = max(shadowConstants.shadowParams.y,
        shadowConstants.shadowParams.z * (1.0 - saturate(nDotL)));
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float storedDepth = shadowMap.SampleLevel(linearSampler,
                float3(shadowUv + float2(x, y) * texelSize, (float)cascade), 0).r;
            visibility += lightNdc.z + bias >= storedDepth ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

float4 main(FragmentIn input) : SV_Target0
{
    const float4 packed0 = gbuffer0.SampleLevel(linearSampler, input.uv, 0);
    const float4 packed1 = gbuffer1.SampleLevel(linearSampler, input.uv, 0);
    const float4 packed2 = gbuffer2.SampleLevel(linearSampler, input.uv, 0);
    const float depth = sceneDepth.SampleLevel(linearSampler, input.uv, 0).r;
    const float3 n = decodeOctahedral(packed1.xy);
    const float4 clip = float4(input.uv * 2.0 - 1.0, depth, 1.0);
    const float4 worldH = mul(constants.inverseViewProjection, clip);
    const float3 worldPosition = worldH.xyz / max(abs(worldH.w), 1e-6);
    const float3 viewDirection = normalize(constants.cameraPosition.xyz - worldPosition);
    const float3 albedo = packed0.rgb;
    const float ao = saturate(packed0.a);
    const float metallic = saturate(packed1.a);
    const float roughness = max(0.045, saturate(packed1.b));

    const uint tilesX = max(1u, constants.clusterParams.x);
    const uint tilesY = max(1u, constants.clusterParams.y);
    const uint slicesZ = max(1u, constants.clusterParams.z);
    const uint lightCount = constants.clusterParams.w;
    const uint tileX = min((uint)input.position.x / 64u, tilesX - 1u);
    const uint tileY = min((uint)input.position.y / 64u, tilesY - 1u);
    const float viewDistance = max(length(worldPosition - constants.cameraPosition.xyz), constants.depthParams.x);
    const float logarithmicSlice = log(viewDistance / constants.depthParams.x) /
        max(log(constants.depthParams.y / constants.depthParams.x), 1e-4);
    const uint slice = min((uint)max(0.0, logarithmicSlice * slicesZ), slicesZ - 1u);
    const uint clusterIndex = slice * tilesX * tilesY + tileY * tilesX + tileX;
    const bool clusteredLighting = constants.depthParams.z > 0.5;
    const uint2 range = clusteredLighting ? clusterRanges[clusterIndex] : uint2(0u, lightCount);

    float3 direct = 0.0.xxx;
    for (uint i = 0; i < range.y; ++i)
    {
        const uint lightIndex = clusteredLighting ? clusterIndices[range.x + i] : i;
        if (lightIndex >= lightCount)
            continue;
        const float4 lightPositionRadius = lights[lightIndex * 4u + 0u];
        const float4 lightColorIntensity = lights[lightIndex * 4u + 1u];
        const float4 lightDirectionType = lights[lightIndex * 4u + 2u];
        const float4 spotParams = lights[lightIndex * 4u + 3u];
        if (lightDirectionType.w > 1.5)
        {
            direct += evaluateSpotLight(worldPosition, n, viewDirection, albedo, metallic,
                roughness, lightPositionRadius.xyz, lightPositionRadius.w,
                lightDirectionType.xyz, spotParams.x, spotParams.y,
                lightColorIntensity.rgb, lightColorIntensity.w);
        }
        else if (lightDirectionType.w > 0.5)
        {
            const float nDotL = saturate(dot(n, normalize(-lightDirectionType.xyz)));
            const float visibility = sampleCascadeShadow(worldPosition, viewDistance, nDotL);
            direct += visibility * evaluateDirectionalLight(worldPosition, n, viewDirection, albedo, metallic,
                roughness, lightDirectionType.xyz, lightColorIntensity.rgb,
                lightColorIntensity.w);
        }
        else
        {
            direct += evaluatePointLight(worldPosition, n, viewDirection, albedo, metallic, roughness,
                lightPositionRadius.xyz, lightPositionRadius.w,
                lightColorIntensity.rgb, lightColorIntensity.w);
        }
    }

    const float nDotV = saturate(dot(n, viewDirection));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 fresnel = fresnelSchlick(nDotV, f0);
    const float3 reflected = reflect(-viewDirection, n);
    const float3 irradiance = irradianceMap.SampleLevel(linearSampler, n, 0).rgb;
    const float3 prefiltered = prefilteredEnvironment.SampleLevel(linearSampler, reflected,
        roughness * 6.0).rgb;
    const float2 brdf = brdfLut.SampleLevel(linearSampler,
        float2(nDotV, roughness), 0).rg;
    const float3 ambient = irradiance * albedo * (1.0 - fresnel) * (1.0 - metallic) / PI +
        prefiltered * (fresnel * brdf.x + brdf.y);
    return float4(max(direct + ambient * ao + packed2.rgb, 0.0), 1.0);
}
