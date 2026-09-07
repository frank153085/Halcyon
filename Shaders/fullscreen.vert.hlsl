struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut main(uint vertexId : SV_VertexID)
{
    // Vulkan NDC y=-1 is the top of a positive-height viewport, and texture
    // (0, 0) is the top-left texel. Map them directly so deferred lighting,
    // TAA, and tonemap sample the G-buffer pixel they are shading.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOut output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
