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
VertexOut main(VertexIn input)
{
    VertexOut output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    output.normal = input.normal;
    return output;
}
