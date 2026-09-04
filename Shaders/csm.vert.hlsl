// CSM depth vertex stage. The renderer binds the snapped light-space matrix
// for the active cascade and the current model transform for every draw.
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
