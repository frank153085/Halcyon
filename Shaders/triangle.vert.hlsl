// Minimal no-vertex-buffer triangle used by the Vulkan M1 vertical slice.
// The coordinates are already in Vulkan clip space; later passes can replace
// this pipeline without changing the swapchain/synchronisation code.
struct VertexOut
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOut main(uint vertexId : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(0.0, -0.72),
        float2(0.72, 0.72),
        float2(-0.72, 0.72),
    };
    static const float3 colors[3] = {
        float3(1.0, 0.25, 0.20),
        float3(0.20, 1.0, 0.35),
        float3(0.20, 0.45, 1.0),
    };

    VertexOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.color = colors[vertexId];
    return output;
}
