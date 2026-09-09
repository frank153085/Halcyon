#include "virtual_geometry_ids.hlsli"

struct PSOut
{
    uint id : SV_Target0;
    uint primitive : SV_Target1;
    uint barycentrics : SV_Target2;
};

// Mesh pipelines carry the meshlet-local primitive through a per-primitive
// output. SV_PrimitiveID is not used here because Vulkan mesh pipelines do not
// guarantee that it contains the local meshlet primitive index.
PSOut main(uint visibility : TEXCOORD0, nointerpolation uint primitiveIndex : TEXCOORD1,
    float3 bary : SV_Barycentrics)
{
    PSOut o;
    o.id = visibility;
    o.barycentrics = vgPackBarycentrics(bary.xy);
    o.primitive = vgEncodePrimitive(primitiveIndex);
    return o;
}
