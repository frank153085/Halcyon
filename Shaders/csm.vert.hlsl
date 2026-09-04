// Minimal CSM depth vertex stage.  The production path can bind one cascade
// matrix per draw; keeping this shader independently compilable also makes
// depth-only pipeline validation deterministic.
struct CsmConstants
{
    float4x4 lightViewProjection;
    float4x4 model;
};

[[vk::push_constant]] ConstantBuffer<CsmConstants> constants;

struct VertexInput
{
    float3 position : POSITION;
};

float4 main(VertexInput input) : SV_Position
{
    return mul(constants.lightViewProjection, mul(constants.model, float4(input.position, 1.0)));
}
