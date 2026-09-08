struct MeshletMeta { uint vertexOffset; uint vertexCount; uint triangleOffset; uint triangleCount; uint indexOffset; uint indexCount; uint primitiveIndex; uint lodIndex; float4 sphere; float4 cone; float geometricError; };
struct DrawIndexedCommand { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= visibleCount[0]) return;
    MeshletMeta m = meshlets[visibleMeshlets[id.x]];
    DrawIndexedCommand command;
    command.indexCount = m.indexCount;
    command.instanceCount = 1;
    command.firstIndex = m.indexOffset;
    command.vertexOffset = 0;
    command.firstInstance = m.primitiveIndex;
    commands[id.x] = command;
}
