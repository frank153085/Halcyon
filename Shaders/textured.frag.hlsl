Texture2D baseColor : register(t0);
SamplerState baseSampler : register(s0);
struct FragmentIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
};
float4 main(FragmentIn input) : SV_Target0
{
    float lighting = 0.55 + 0.45 * abs(input.normal.z);
    return baseColor.Sample(baseSampler, input.uv) * lighting;
}
