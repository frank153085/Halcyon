[[vk::binding(0, 0)]] Texture2D<uint> visibilityIds;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> materialIds;
struct Constants { uint width; uint height; uint pad0; uint pad1; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.width || id.y >= constants.height) return;
    uint pixel = id.y * constants.width + id.x;
    materialIds[pixel] = (visibilityIds.Load(int3(id.xy, 0)) >> 28) & 0xfu;
}
