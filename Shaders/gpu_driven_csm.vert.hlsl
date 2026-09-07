// GPU-driven CSM depth. Instance transforms live in the GPU scene table;
// SV_InstanceID indexes the cascade-compacted visible slot list.
[[vk::binding(0, 0)]] StructuredBuffer<float4x4> transforms;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleInstanceIndices;

struct CsmGpuConstants
{
    float4x4 lightViewProjection;
};

[[vk::push_constant]] ConstantBuffer<CsmGpuConstants> constants;

struct VertexInput
{
    float3 position : POSITION;
};

float4 main(VertexInput input, uint instanceId : SV_InstanceID) : SV_Position
{
    const uint slot = visibleInstanceIndices[instanceId];
    const float4x4 model = transforms[slot];
    return mul(constants.lightViewProjection, mul(model, float4(input.position, 1.0)));
}
