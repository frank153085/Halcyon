struct FragmentIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D hdrTexture;
[[vk::binding(10, 0)]] SamplerState linearSampler;
struct TonemapConstants
{
    float exposure;
    float3 _padding;
};
[[vk::push_constant]] ConstantBuffer<TonemapConstants> constants;

float3 aces(float3 color)
{
    const float3 a = 2.51;
    const float3 b = 0.03;
    const float3 c = 2.43;
    const float3 d = 0.59;
    const float3 e = 0.14;
    return saturate((color * (a * color + b)) / max(color * (c * color + d) + e, 1e-6));
}

float3 linearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return lerp(12.92 * color, 1.055 * pow(color, 1.0 / 2.4) - 0.055,
        step(0.0031308, color));
}

float4 main(FragmentIn input) : SV_Target0
{
    const float3 hdr = hdrTexture.Sample(linearSampler, input.uv).rgb * exp2(constants.exposure);
    return float4(linearToSrgb(aces(hdr)), 1.0);
}
