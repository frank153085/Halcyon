// Forward transparency vertex stage.  This stage intentionally has its own
// push-constant ABI: the fragment shader needs camera/light parameters in the
// same block, so reusing the opaque pbr.vert ABI would reinterpret those
// values as matrices and produce invalid transforms.
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
};

struct ForwardConstants
{
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightPositionOrDirection;
    float4 lightColorIntensity;
    float4 lightParameters;
    float4x4 model;
    float4x4 unusedPreviousModel;
};

[[vk::push_constant]] ConstantBuffer<ForwardConstants> constants;

VertexOut main(VertexIn input)
{
    VertexOut output;
    const float4 world = mul(constants.model, float4(input.position, 1.0));
    output.position = mul(constants.viewProjection, world);
    output.currentPosition = output.position;
    output.previousPosition = output.position;
    output.worldPosition = world.xyz;
    output.normal = normalize(mul((float3x3)constants.model, input.normal));
    output.uv = input.uv;
    output.tangent = float4(
        normalize(mul((float3x3)constants.model, input.tangent.xyz)), input.tangent.w);
    return output;
}
