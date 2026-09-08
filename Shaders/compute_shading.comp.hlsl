[[vk::binding(0, 0)]] StructuredBuffer<uint> materialIds;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibilityIds;
[[vk::binding(2, 0)]] RWTexture2D<float4> hdr;
struct Constants { uint width; uint height; uint pixelCount; uint pad; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { if (id.x >= constants.width || id.y >= constants.height) return; uint i = id.y * constants.width + id.x; float v = (visibilityIds[i] & 0xffu) / 255.0; hdr[id.xy] = float4(v, materialIds[i] / 255.0, 0.0, 1.0); }
