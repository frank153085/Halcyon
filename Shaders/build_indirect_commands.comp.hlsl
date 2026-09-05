// First M4 implementation: one indexed indirect command per visible instance.
struct MeshMaterialRow { uint meshIndex; uint materialIndex; uint flags; uint lodState; };
struct DrawIndexedCommand { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(1, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> drawCount;
struct IndirectConstants { uint visibleCount; uint indexCount; uint firstIndex; int vertexOffset; };
[[vk::push_constant]] ConstantBuffer<IndirectConstants> constants;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= constants.visibleCount) return;
    const uint instance = visibleInstanceIndices[id.x];
    DrawIndexedCommand command = { constants.indexCount, 1, constants.firstIndex,
        constants.vertexOffset, instance };
    commands[id.x] = command;
    if (id.x == 0) drawCount[0] = constants.visibleCount;
}
