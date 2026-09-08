struct MeshletMeta { uint vertexOffset; uint vertexCount; uint triangleOffset; uint triangleCount; uint indexOffset; uint indexCount; uint primitiveIndex; uint lodIndex; float4 sphere; float4 cone; float geometricError; };
struct Vertex { float3 position; float3 normal; float2 uv; float4 tangent; };
[[vk::binding(0, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(2, 0)]] StructuredBuffer<Vertex> vertices;
[[vk::binding(3, 0)]] StructuredBuffer<uint> indices;
struct VisibilityConstants { float4x4 viewProjection; float4x4 model; float4x4 previousModel; uint instance; uint material; uint pad0; uint pad1; };
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> constants;
struct VSOut { float4 position : SV_Position; uint visibility : TEXCOORD0; };
VSOut main(uint vertexId : SV_VertexID, uint meshletId : SV_InstanceID)
{
    VSOut o;
    MeshletMeta m = meshlets[meshletId];
    // Indexed draws expose the fetched index as SV_VertexID.  firstIndex is
    // already applied by the fixed-function input assembler, so indexing the
    // virtual index table a second time would address unrelated vertices.
    uint vertexIndex = vertexId;
    Vertex v = vertices[vertexIndex];
    float4 world = mul(constants.model, float4(v.position, 1.0));
    o.position = mul(constants.viewProjection, world);
    // R32 visibility ABI: instance in bits 0..7, meshlet in 8..27,
    // material class in bits 28..31.  The triangle is recovered from the
    // rasterized primitive and meshlet metadata in compute shading.
    o.visibility = (constants.instance & 0xffu) |
        ((meshletId & 0xfffffu) << 8) | ((constants.material & 0xfu) << 28);
    return o;
}
