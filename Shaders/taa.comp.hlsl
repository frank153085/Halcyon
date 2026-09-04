struct TaaConstants
{
    uint2 extent;
    float historyWeight;
    float sharpen;
};

[[vk::push_constant]] ConstantBuffer<TaaConstants> constants;
[[vk::binding(0, 0)]] Texture2D currentFrame;
[[vk::binding(1, 0)]] Texture2D historyFrame;
[[vk::binding(2, 0)]] Texture2D motionVectors;
[[vk::binding(20, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outputFrame : register(u0);
[[vk::binding(10, 0)]] SamplerState linearSampler;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= constants.extent.x || dispatchId.y >= constants.extent.y)
        return;
    const float2 uv = (float2(dispatchId.xy) + 0.5) / float2(constants.extent);
    const float4 current = currentFrame.Load(int3(dispatchId.xy, 0));
    const float2 motion = motionVectors.Load(int3(dispatchId.xy, 0)).xy;
    const float2 historyUv = saturate(uv - motion);
    float3 neighborhoodMin = current.rgb;
    float3 neighborhoodMax = current.rgb;
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 sampleCoord = clamp(int2(dispatchId.xy) + int2(ox, oy),
                int2(0, 0), int2(constants.extent) - 1);
            const float3 sampleColor = currentFrame.Load(int3(sampleCoord, 0)).rgb;
            neighborhoodMin = min(neighborhoodMin, sampleColor);
            neighborhoodMax = max(neighborhoodMax, sampleColor);
        }
    }
    const float3 history = clamp(historyFrame.SampleLevel(linearSampler, historyUv, 0).rgb,
        neighborhoodMin, neighborhoodMax);
    float3 result = lerp(current.rgb, history, saturate(constants.historyWeight));
    if (constants.sharpen > 0.0)
    {
        const int2 p = int2(dispatchId.xy);
        const int2 maximum = int2(constants.extent) - 1;
        const float3 crossAverage =
            currentFrame.Load(int3(clamp(p + int2(-1, 0), 0, maximum), 0)).rgb +
            currentFrame.Load(int3(clamp(p + int2(1, 0), 0, maximum), 0)).rgb +
            currentFrame.Load(int3(clamp(p + int2(0, -1), 0, maximum), 0)).rgb +
            currentFrame.Load(int3(clamp(p + int2(0, 1), 0, maximum), 0)).rgb;
        result += (current.rgb - crossAverage * 0.25) * saturate(constants.sharpen);
    }
    outputFrame[dispatchId.xy] = float4(max(result, 0.0), 1.0);
}
