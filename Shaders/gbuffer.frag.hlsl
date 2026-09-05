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

struct FragmentOut
{
    float4 albedo : SV_Target0;
    float4 normalRoughness : SV_Target1;
    float4 materialData : SV_Target2;
    float2 motion : SV_Target3;
    uint instanceId : SV_Target4;
};

FragmentOut main(FragmentIn input)
{
    FragmentOut output;
    const float4 base = baseColorTexture.Sample(materialSampler, input.uv) *
        materialFactors.baseColorFactor;
    if (((uint)materialFactors.factors.w & 4u) != 0u)
        clip(base.a - materialFactors.factors.z);
    const float4 metallicRoughness = metallicRoughnessTexture.Sample(materialSampler, input.uv);
    const float3 geometricNormal = normalize(input.normal);
    const float3 tangent = normalize(input.tangent.xyz);
    const float3 bitangent = normalize(cross(geometricNormal, tangent) * input.tangent.w);
    const float3 mappedNormal = normalTexture.Sample(materialSampler, input.uv).xyz * 2.0 - 1.0;
    const float3 worldNormal = normalize(tangent * mappedNormal.x + bitangent * mappedNormal.y +
        geometricNormal * mappedNormal.z);
    const float ao = occlusionTexture.Sample(materialSampler, input.uv).r *
        materialFactors.emissiveFactor.a;
    const float3 emissive = emissiveTexture.Sample(materialSampler, input.uv).rgb *
        materialFactors.emissiveFactor.rgb;
    const float roughness = saturate(metallicRoughness.g * materialFactors.factors.y);
    const float metallic = saturate(metallicRoughness.b * materialFactors.factors.x);
    float3 oct = worldNormal / (abs(worldNormal.x) + abs(worldNormal.y) + abs(worldNormal.z));
    if (oct.z < 0.0)
        oct.xy = (1.0 - abs(oct.yx)) * sign(oct.xy);
    const float2 octEncoded = oct.xy * 0.5 + 0.5;
    output.albedo = float4(base.rgb, ao);
    output.normalRoughness = float4(octEncoded, roughness, metallic);
    output.materialData = float4(emissive, materialFactors.factors.w);
    const float2 currentNdc = input.currentPosition.xy / max(abs(input.currentPosition.w), 1e-6);
    const float2 previousNdc = input.previousPosition.xy / max(abs(input.previousPosition.w), 1e-6);
    output.motion = (currentNdc - previousNdc) * 0.5;
    // The legacy indexed path has no stable GPU-scene slot; zero is a valid
    // sentinel for its debug attachment.
    output.instanceId = 0u;
    return output;
}
