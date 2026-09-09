#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleCount;
struct DrawMeshTasksCommand
{
    uint groupCountX;
    uint groupCountY;
    uint groupCountZ;
};
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawMeshTasksCommand> commands;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> commandCount;
struct Constants { uint4 values; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint drawCount = min(visibleCount[0], constants.values.x);
    if (id.x != 0u)
        return;
    commands[0].groupCountX = drawCount;
    commands[0].groupCountY = 1u;
    commands[0].groupCountZ = 1u;
    commandCount[0] = drawCount == 0u ? 0u : 1u;
}
