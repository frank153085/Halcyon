struct FragmentIn
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousPosition : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 currentPosition : TEXCOORD5;
};

[[vk::binding(0, 0)]] Texture2D baseColorTexture;
[[vk::binding(1, 0)]] Texture2D normalTexture;
[[vk::binding(2, 0)]] Texture2D metallicRoughnessTexture;
[[vk::binding(3, 0)]] Texture2D emissiveTexture;
[[vk::binding(4, 0)]] Texture2D occlusionTexture;
[[vk::binding(10, 0)]] SamplerState materialSampler;
struct MaterialFactors
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 factors;
};
[[vk::binding(30, 0)]] ConstantBuffer<MaterialFactors> materialFactors;

struct ForwardConstants
{
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightPositionOrDirection;
    float4 lightColorIntensity;
    float4 lightParameters;
    float4x4 model;
    float4x4 unusedPreviousModel;
};
[[vk::push_constant]] ConstantBuffer<ForwardConstants> constants;

static const float PI = 3.14159265359;

float distributionGgx(float nDotH, float roughness)
{
    const float alpha = max(0.045, roughness * roughness);
    const float alphaSquared = alpha * alpha;
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 1e-6);
}

float geometrySchlick(float nDotV, float roughness)
{
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 1e-6);
}

float3 fresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosine), 5.0);
}

float4 main(FragmentIn input) : SV_Target0
{
    const float4 base = baseColorTexture.Sample(materialSampler, input.uv) *
        materialFactors.baseColorFactor;
    const uint flags = (uint)materialFactors.factors.w;
    if ((flags & 4u) != 0u)
        clip(base.a - materialFactors.factors.z);
    const float3 geometricNormal = normalize(input.normal);
    const float3 tangent = normalize(input.tangent.xyz);
    const float3 bitangent = normalize(cross(geometricNormal, tangent) * input.tangent.w);
    const float3 mappedNormal = normalTexture.Sample(materialSampler, input.uv).xyz * 2.0 - 1.0;
    const float3 n = normalize(tangent * mappedNormal.x + bitangent * mappedNormal.y +
        geometricNormal * mappedNormal.z);
    const float4 metallicRoughness = metallicRoughnessTexture.Sample(materialSampler, input.uv);
    const float metallic = saturate(metallicRoughness.b * materialFactors.factors.x);
    const float roughness = max(0.045,
        saturate(metallicRoughness.g * materialFactors.factors.y));
    const float3 viewDirection = normalize(constants.cameraPosition.xyz - input.worldPosition);
    float3 lightDirection;
    float attenuation = 1.0;
    if (constants.lightPositionOrDirection.w > 0.5)
    {
        lightDirection = normalize(-constants.lightPositionOrDirection.xyz);
    }
    else
    {
        const float3 toLight = constants.lightPositionOrDirection.xyz - input.worldPosition;
        const float distanceToLight = length(toLight);
        lightDirection = toLight / max(distanceToLight, 1e-5);
        attenuation = pow(saturate(1.0 - distanceToLight /
            max(constants.lightParameters.x, 1e-4)), 2.0);
    }
    const float3 halfVector = normalize(viewDirection + lightDirection);
    const float nDotV = saturate(dot(n, viewDirection));
    const float nDotL = saturate(dot(n, lightDirection));
    const float nDotH = saturate(dot(n, halfVector));
    const float3 f0 = lerp(0.04.xxx, base.rgb, metallic);
    const float3 fresnel = fresnelSchlick(saturate(dot(viewDirection, halfVector)), f0);
    const float specular = distributionGgx(nDotH, roughness) *
        geometrySchlick(nDotV, roughness) * geometrySchlick(nDotL, roughness) /
        max(4.0 * nDotV * nDotL, 1e-5);
    const float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base.rgb / PI;
    const float ao = occlusionTexture.Sample(materialSampler, input.uv).r *
        materialFactors.emissiveFactor.a;
    const float3 emissive = emissiveTexture.Sample(materialSampler, input.uv).rgb *
        materialFactors.emissiveFactor.rgb;
    const float3 direct = (diffuse + specular * fresnel) * nDotL *
        constants.lightColorIntensity.rgb * constants.lightColorIntensity.w * attenuation;
    const float3 ambient = base.rgb * ao * constants.lightParameters.w;
    return float4(max(direct + ambient + emissive, 0.0), base.a);
}
