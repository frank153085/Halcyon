struct VertexIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
};

// Camera and object transforms are supplied per draw.  DXC's -Zpc option
// keeps these matrices in column-major layout, matching glm::mat4 on the CPU.
struct PushConstants
{
    float4x4 viewProjection;
    float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> constants;

VertexOut main(VertexIn input)
{
    VertexOut output;
    const float4 worldPosition = mul(constants.model, float4(input.position, 1.0));
    output.position = mul(constants.viewProjection, worldPosition);
    output.uv = input.uv;
    // The demo uses rigid transforms only.  Transforming the normal here
    // keeps the simple fragment lighting coherent while preserving the
    // existing vertex format.
    output.normal = normalize(mul((float3x3)constants.model, input.normal));
    return output;
}
