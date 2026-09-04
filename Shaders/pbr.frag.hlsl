struct FragmentIn
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

Texture2D baseColorTexture : register(t0);
Texture2D metallicRoughnessTexture : register(t1);
TextureCube irradianceMap : register(t3);
TextureCube prefilteredEnvironment : register(t4);
Texture2D brdfLut : register(t5);
SamplerState linearSampler : register(s0);

struct PbrConstants
{
    float4 baseColorFactor;
    float3 cameraPosition;
    float metallicFactor;
    float roughnessFactor;
    float exposure;
};

[[vk::push_constant]] ConstantBuffer<PbrConstants> constants;

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

float4 main(FragmentIn input) : SV_Target0
{
    const float3 n = normalize(input.normal);
    const float3 v = normalize(constants.cameraPosition - input.worldPosition);
    const float3 l = normalize(float3(0.35, 0.85, 0.2));
    const float3 h = normalize(v + l);
    const float3 albedo = baseColorTexture.Sample(linearSampler, input.uv).rgb * constants.baseColorFactor.rgb;
    const float2 metallicRoughness = metallicRoughnessTexture.Sample(linearSampler, input.uv).bg;
    const float metallic = saturate(constants.metallicFactor * metallicRoughness.x);
    const float roughness = saturate(constants.roughnessFactor * metallicRoughness.y);
    const float nDotV = saturate(dot(n, v));
    const float nDotL = saturate(dot(n, l));
    const float nDotH = saturate(dot(n, h));
    const float vDotH = saturate(dot(v, h));
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float3 f = fresnelSchlick(vDotH, f0);
    const float specular = distributionGgx(nDotH, roughness) * geometrySchlick(nDotV, roughness) *
                           geometrySchlick(nDotL, roughness) / max(4.0 * nDotV * nDotL, 1e-5);
    const float3 kd = (1.0 - f) * (1.0 - metallic);
    const float3 direct = (kd * albedo / PI + specular * f) * nDotL;
    const float3 reflected = reflect(-v, n);
    const float3 irradiance = irradianceMap.SampleLevel(linearSampler, n, 0).rgb;
    const float3 prefiltered = prefilteredEnvironment.SampleLevel(linearSampler, reflected, roughness * 8.0).rgb;
    const float2 brdf = brdfLut.SampleLevel(linearSampler, float2(nDotV, roughness), 0).rg;
    const float3 ambient = irradiance * albedo * kd / PI + prefiltered * (f * brdf.x + brdf.y);
    return float4(max(direct + ambient, 0.0) * exp2(constants.exposure), 1.0);
}
