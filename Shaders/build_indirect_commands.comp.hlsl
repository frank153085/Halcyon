// Mesh-batched indirect builder.
//
// Dispatch mode 0 links each frustum/phase-visible slot into a per-mesh list.
// Mode 1 walks those lists and emits one command per mesh. The linked-list
// pass avoids a meshCount * instanceCount bucket allocation while keeping the
// visible slot list compact for the vertex shader.
struct MeshMaterialRow { uint meshIndex; uint materialIndex; uint flags; uint lodState; };
struct MeshDrawRow { uint indexCount; uint firstIndex; int vertexOffset; uint reserved; };
struct DrawIndexedCommand { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(1, 0)]] StructuredBuffer<MeshMaterialRow> meshMaterials;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> drawCount;
[[vk::binding(4, 0)]] StructuredBuffer<MeshDrawRow> meshDraws;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> meshHeads;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> meshNext;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> groupedVisibleIndices;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> groupedVisibleCount;
[[vk::binding(9, 0)]] StructuredBuffer<uint> visibleCount;

struct IndirectConstants
{
    uint instanceCount;
    uint meshCount;
    uint mode;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<IndirectConstants> constants;

static const uint kInvalid = 0xffffffffu;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (constants.mode == 0u)
    {
        const uint candidateCount = min(visibleCount[0], constants.instanceCount);
        if (id.x >= candidateCount) return;
        const uint instance = visibleInstanceIndices[id.x];
        const uint mesh = meshMaterials[instance].meshIndex;
        if (mesh >= constants.meshCount) return;
        uint previous;
        InterlockedExchange(meshHeads[mesh], instance, previous);
        meshNext[instance] = previous;
        return;
    }

    const uint mesh = id.x;
    if (mesh >= constants.meshCount) return;
    uint instance = meshHeads[mesh];
    if (instance == kInvalid) return;

    uint count = 0u;
    // A slot is linked once by mode 0. The instance-count bound is a safety
    // guard against malformed/cyclic data during development.
    while (instance != kInvalid && count < constants.instanceCount)
    {
        ++count;
        instance = meshNext[instance];
    }
    if (count == 0u) return;

    uint commandIndex;
    InterlockedAdd(drawCount[0], 1u, commandIndex);
    uint firstInstance;
    InterlockedAdd(groupedVisibleCount[0], count, firstInstance);

    instance = meshHeads[mesh];
    for (uint i = 0u; i < count; ++i)
    {
        groupedVisibleIndices[firstInstance + i] = instance;
        instance = meshNext[instance];
    }
    const MeshDrawRow draw = meshDraws[mesh];
    DrawIndexedCommand command = {draw.indexCount, count, draw.firstIndex,
        draw.vertexOffset, firstInstance};
    commands[commandIndex] = command;
}
