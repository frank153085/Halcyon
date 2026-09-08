[[vk::binding(0, 0)]] Texture2D<uint> visibilityIds;
[[vk::binding(1, 0)]] StructuredBuffer<uint> materialIds;
[[vk::binding(2, 0)]] RWTexture2D<unorm float4> hdr;
struct Constants { uint width; uint height; uint pixelCount; uint pad; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.width || id.y >= constants.height) return;
    uint i = id.y * constants.width + id.x;
    uint visibility = visibilityIds.Load(int3(id.xy, 0));
    uint material = materialIds[i];
    if (visibility == 0u) { hdr[id.xy] = float4(0.012, 0.018, 0.028, 1.0); return; }
    // Lucy uses the default PBR material.  Keep the shading deterministic and
    // HDR-safe while preserving the material-class ABI for future texture
    // table lookup and IBL integration.
    float3 base = float3(0.72, 0.74, 0.78) * (1.0 - 0.08 * material);
    float lobe = 0.65 + 0.35 * ((visibility >> 8) & 0xffu) / 255.0;
    hdr[id.xy] = float4(base * lobe, 1.0);
}
