struct VSOut { float4 position : SV_Position; uint visibility : TEXCOORD0; };
struct VisibilityConstants { uint instance; uint meshlet; uint material; uint pad; };
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> constants;
VSOut main(uint vertexId : SV_VertexID) { VSOut o; o.position = float4(0, 0, 0, 1); o.visibility = (constants.instance & 0xffu) | ((constants.meshlet & 0xffffu) << 8) | ((vertexId & 0xffu) << 24); return o; }
