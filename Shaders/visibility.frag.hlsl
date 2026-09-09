#include "virtual_geometry_ids.hlsli"

struct PSOut
{
    uint id : SV_Target0;
    uint primitive : SV_Target1;
    uint barycentrics : SV_Target2;
};

PSOut main(uint visibility : TEXCOORD0, float3 bary : SV_Barycentrics,
    uint triangleId : SV_PrimitiveID)
{
    PSOut o;
    o.id = visibility;
    // Two 16-bit UNORM components are sufficient for attribute
    // reconstruction and keep the visibility attachments integer-only.
    o.barycentrics = vgPackBarycentrics(bary.xy);
    // Zero is reserved for a background pixel.
    // PrimitiveID is local to the indexed draw (one draw per meshlet), so it
    // directly identifies the triangle within the meshlet's flattened index
    // range. +1 keeps triangle zero distinct from the cleared background.
    o.primitive = vgEncodePrimitive(triangleId);
    return o;
}
