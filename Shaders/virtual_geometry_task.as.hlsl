#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleCount;

[numthreads(32, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    if (groupThreadId.x == 0u)
        DispatchMesh(visibleCount[0], 1u, 1u);
}
