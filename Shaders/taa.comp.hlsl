struct TaaConstants
{
    uint2 extent;
    float historyWeight;
    float sharpen;
};

[[vk::push_constant]] ConstantBuffer<TaaConstants> constants;
Texture2D currentFrame : register(t0);
Texture2D historyFrame : register(t1);
Texture2D motionVectors : register(t2);
RWTexture2D<float4> outputFrame : register(u0);
SamplerState linearSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= constants.extent.x || dispatchId.y >= constants.extent.y)
        return;
    const float2 uv = (float2(dispatchId.xy) + 0.5) / float2(constants.extent);
    const float4 current = currentFrame.Load(int3(dispatchId.xy, 0));
    const float2 motion = motionVectors.Load(int3(dispatchId.xy, 0)).xy;
    const float2 historyUv = saturate(uv - motion);
    const float3 history = historyFrame.SampleLevel(linearSampler, historyUv, 0).rgb;
    const float3 result = lerp(current.rgb, history, saturate(constants.historyWeight));
    outputFrame[dispatchId.xy] = float4(max(result, 0.0), 1.0);
}
