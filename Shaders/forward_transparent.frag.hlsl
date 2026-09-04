struct FragmentIn
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

Texture2D baseColorTexture : register(t0);
SamplerState materialSampler : register(s0);

float4 main(FragmentIn input) : SV_Target0
{
    const float3 n = normalize(input.normal);
    const float ndotl = saturate(dot(n, normalize(float3(0.4, 0.8, 0.2))));
    const float4 base = baseColorTexture.Sample(materialSampler, input.uv);
    return float4(base.rgb * (0.05 + ndotl), base.a);
}
