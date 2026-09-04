// Minimal CSM depth vertex stage.  The production path can bind one cascade
// matrix per draw; keeping this shader independently compilable also makes
// depth-only pipeline validation deterministic.
cbuffer CsmConstants : register(b0)
{
    float4x4 lightViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 main(VertexInput input) : SV_Position
{
    return mul(lightViewProjection, float4(input.position, 1.0));
}
