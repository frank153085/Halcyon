#include "virtual_geometry_ids.hlsli"

struct PSOut
{
    uint id : SV_Target0;
    uint primitive : SV_Target1;
    uint barycentrics : SV_Target2;
};

// Mesh pipelines use the fragment primitive built-in directly. This avoids
// the DXC HLSL limitation that otherwise emits PerPrimitiveEXT on the mesh
// output but cannot reproduce it on the fragment input.
PSOut main(uint visibility : TEXCOORD0, uint primitiveIndex : SV_PrimitiveID,
    float3 bary : SV_Barycentrics)
{
    PSOut o;
    o.id = visibility;
    o.barycentrics = vgPackBarycentrics(bary.xy);
    o.primitive = vgEncodePrimitive(primitiveIndex);
    return o;
}
