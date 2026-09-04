struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut main(uint vertexId : SV_VertexID)
{
    // A single fullscreen triangle avoids vertex/index buffers for post and
    // deferred passes and is stable for every viewport size.
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)};
    const float2 uvs[3] = {
        float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0)};
    VertexOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
