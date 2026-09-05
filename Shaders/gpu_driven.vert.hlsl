// GPU-driven vertex stage. firstInstance supplied by an indirect command is
// the persistent GPU Scene slot, so no model push constant is required.
struct MeshMaterialRow { uint meshIndex; uint materialIndex; uint flags; uint lodState; };
[[vk::binding(0, 1)]] StructuredBuffer<float4x4> transforms;
[[vk::binding(1, 1)]] StructuredBuffer<MeshMaterialRow> meshMaterials;

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
    float4 tangent : TEXCOORD4;
    float4 currentPosition : TEXCOORD5;
    uint materialIndex : TEXCOORD6;
    uint instanceId : TEXCOORD7;
};
struct Constants { float4x4 viewProjection; float4x4 previousViewProjection; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;

VertexOut main(VertexIn input, uint instanceId : SV_InstanceID)
{
    VertexOut output;
    const float4x4 model = transforms[instanceId];
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(constants.viewProjection, world);
    output.currentPosition = output.position;
    output.previousPosition = mul(constants.previousViewProjection, world);
    output.worldPosition = world.xyz;
    output.normal = normalize(mul((float3x3)model, input.normal));
    output.uv = input.uv;
    output.tangent = float4(normalize(mul((float3x3)model, input.tangent.xyz)), input.tangent.w);
    output.materialIndex = meshMaterials[instanceId].materialIndex;
    output.instanceId = instanceId;
    return output;
}
