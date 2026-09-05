// First M4 implementation: one indexed indirect command per visible instance.
struct MeshMaterialRow { uint meshIndex; uint materialIndex; uint flags; uint lodState; };
struct MeshDrawRow { uint indexCount; uint firstIndex; int vertexOffset; uint reserved; };
struct DrawIndexedCommand { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(1, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> drawCount;
[[vk::binding(4, 0)]] StructuredBuffer<MeshDrawRow> meshDraws;
struct IndirectConstants { uint instanceCount; uint reserved0; uint reserved1; uint reserved2; };
[[vk::push_constant]] ConstantBuffer<IndirectConstants> constants;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint visibleCount = min(drawCount[0], constants.instanceCount);
    if (id.x >= visibleCount) return;
    const uint instance = visibleInstanceIndices[id.x];
    const MeshDrawRow mesh = meshDraws[meshMaterials[instance].meshIndex];
    DrawIndexedCommand command = { mesh.indexCount, 1, mesh.firstIndex,
        mesh.vertexOffset, instance };
    commands[id.x] = command;
    if (id.x == 0) drawCount[0] = visibleCount;
}
