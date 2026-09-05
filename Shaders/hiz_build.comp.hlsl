// Reversed-Z Hi-Z reduction: the conservative reduction is minimum depth.
[[vk::binding(0, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> hizOutput;
struct HiZConstants { uint2 sourceSize; uint2 outputSize; };
[[vk::push_constant]] ConstantBuffer<HiZConstants> constants;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.outputSize.x || id.y >= constants.outputSize.y) return;
    const uint2 source = id.xy * 2;
    const uint2 maxSource = max(constants.sourceSize, uint2(1, 1)) - 1;
    const uint2 a = min(source + uint2(0, 0), maxSource);
    const uint2 b = min(source + uint2(1, 0), maxSource);
    const uint2 c = min(source + uint2(0, 1), maxSource);
    const uint2 d = min(source + uint2(1, 1), maxSource);
    hizOutput[id.xy] = min(min(sceneDepth[a], sceneDepth[b]), min(sceneDepth[c], sceneDepth[d]));
}
