// GPU-driven material variant. Textures are addressed through the global
// sampled-image table; the material row is selected by the instance slot.
struct MaterialGpuData
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 factors;
    uint4 textureIndices0;
    uint4 textureIndices1;
};

// Keep this count in sync with the Vulkan bindless ABI.  The runtime only
// enables this pipeline when the device can provide the complete array; a
// smaller device uses the legacy material-set pipeline instead.
#define HALCYON_BINDLESS_TEXTURE_CAPACITY 256
[[vk::binding(0, 0)]] Texture2D bindlessTextures[HALCYON_BINDLESS_TEXTURE_CAPACITY];
[[vk::binding(4, 0)]] SamplerState bindlessSamplers[16];
[[vk::binding(3, 1)]] StructuredBuffer<MaterialGpuData> materials;

struct FragmentIn
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousPosition : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 currentPosition : TEXCOORD5;
    uint materialIndex : TEXCOORD6;
    uint instanceId : TEXCOORD7;
};

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
    const MaterialGpuData material = materials[input.materialIndex];
    const uint baseColorIndex = NonUniformResourceIndex(material.textureIndices0.x);
    const uint normalIndex = NonUniformResourceIndex(material.textureIndices0.y);
    const uint metallicRoughnessIndex = NonUniformResourceIndex(material.textureIndices0.z);
    const uint emissiveIndex = NonUniformResourceIndex(material.textureIndices0.w);
    const uint occlusionIndex = NonUniformResourceIndex(material.textureIndices1.x);
    const SamplerState sampler = bindlessSamplers[0];
    const float4 base = bindlessTextures[baseColorIndex].Sample(sampler, input.uv) *
        material.baseColorFactor;
    if (((uint)material.factors.w & 4u) != 0u)
        clip(base.a - material.factors.z);
    const float4 metallicRoughness = bindlessTextures[metallicRoughnessIndex].Sample(sampler, input.uv);
    const float3 geometricNormal = normalize(input.normal);
    const float3 tangent = normalize(input.tangent.xyz);
    const float3 bitangent = normalize(cross(geometricNormal, tangent) * input.tangent.w);
    const float3 mappedNormal = bindlessTextures[normalIndex].Sample(sampler, input.uv).xyz * 2.0 - 1.0;
    const float3 worldNormal = normalize(tangent * mappedNormal.x + bitangent * mappedNormal.y +
        geometricNormal * mappedNormal.z);
    const float ao = bindlessTextures[occlusionIndex].Sample(sampler, input.uv).r *
        material.emissiveFactor.a;
    const float3 emissive = bindlessTextures[emissiveIndex].Sample(sampler, input.uv).rgb *
        material.emissiveFactor.rgb;
    const float roughness = saturate(metallicRoughness.g * material.factors.y);
    const float metallic = saturate(metallicRoughness.b * material.factors.x);
    float3 oct = worldNormal / (abs(worldNormal.x) + abs(worldNormal.y) + abs(worldNormal.z));
    if (oct.z < 0.0)
        oct.xy = (1.0 - abs(oct.yx)) * sign(oct.xy);
    const float2 octEncoded = oct.xy * 0.5 + 0.5;
    FragmentOut output;
    output.albedo = float4(base.rgb, ao);
    output.normalRoughness = float4(octEncoded, roughness, metallic);
    output.materialData = float4(emissive, material.factors.w);
    const float2 currentNdc = input.currentPosition.xy / max(abs(input.currentPosition.w), 1e-6);
    const float2 previousNdc = input.previousPosition.xy / max(abs(input.previousPosition.w), 1e-6);
    output.motion = (currentNdc - previousNdc) * 0.5;
    // Zero is reserved for clear/background pixels in the R32Uint debug
    // attachment; store slot+1 so slot zero remains observable.
    output.instanceId = input.instanceId + 1u;
    return output;
}
