struct MeshletMeta { uint vertexOffset; uint vertexCount; uint triangleOffset; uint triangleCount; float4 sphere; float4 cone; float geometricError; uint lod; };
struct DrawIndexedCommand { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= visibleCount[0]) return;
    MeshletMeta m = meshlets[visibleMeshlets[id.x]];
    commands[id.x] = (DrawIndexedCommand)(m.triangleCount * 3, 1, m.triangleOffset, 0, m.lod);
}
