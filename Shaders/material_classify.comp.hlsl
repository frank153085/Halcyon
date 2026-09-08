[[vk::binding(0, 0)]] StructuredBuffer<uint> visibilityIds;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> materialIds;
struct Constants { uint pixelCount; uint pad0; uint pad1; uint pad2; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { if (id.x < constants.pixelCount) materialIds[id.x] = (visibilityIds[id.x] >> 16) & 0xffu; }
