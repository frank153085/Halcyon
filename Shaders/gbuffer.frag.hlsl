struct FragmentIn
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousPosition : TEXCOORD3;
};

Texture2D baseColorTexture : register(t0);
Texture2D metallicRoughnessTexture : register(t1);
Texture2D normalTexture : register(t2);
SamplerState materialSampler : register(s0);

struct MaterialConstants
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    uint flags;
};

[[vk::push_constant]] ConstantBuffer<MaterialConstants> material;

struct FragmentOut
{
    float4 albedo : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 materialData : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4;
};

FragmentOut main(FragmentIn input)
{
    FragmentOut output;
    const float4 base = baseColorTexture.Sample(materialSampler, input.uv) * material.baseColorFactor;
    const float4 metallicRoughness = metallicRoughnessTexture.Sample(materialSampler, input.uv);
    output.albedo = base;
    output.normalRoughness = float4(normalize(input.normal) * 0.5 + 0.5,
        saturate(material.roughnessFactor * metallicRoughness.g));
    output.materialData = float4(saturate(material.metallicFactor * metallicRoughness.b),
        1.0, 0.0, 0.0);
    output.emissive = float4(material.emissiveFactor.rgb, 1.0);
    const float2 currentUv = input.position.xy;
    const float2 previousUv = input.previousPosition.xy / max(abs(input.previousPosition.w), 1e-6);
    output.motion = currentUv - previousUv;
    return output;
}
