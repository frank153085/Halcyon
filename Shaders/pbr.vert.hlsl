struct VertexIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT0;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousPosition : TEXCOORD3;
};

struct Constants
{
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4x4 model;
    float4x4 previousModel;
};

[[vk::push_constant]] ConstantBuffer<Constants> constants;

VertexOut main(VertexIn input)
{
    VertexOut output;
    const float4 world = mul(constants.model, float4(input.position, 1.0));
    output.position = mul(constants.viewProjection, world);
    output.previousPosition = mul(constants.previousViewProjection,
        mul(constants.previousModel, float4(input.position, 1.0)));
    output.worldPosition = world.xyz;
    output.normal = normalize(mul((float3x3)constants.model, input.normal));
    output.uv = input.uv;
    return output;
}
