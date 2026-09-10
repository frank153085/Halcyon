#include "virtual_geometry_ids.hlsli"

[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<MeshletMeta> meshlets;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedCommand> commands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> commandCount;
struct Constants { uint meshletCount; uint commandCapacity; uint instanceCount; uint indexCount; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= visibleCount[0] || id.x >= constants.commandCapacity) return;
    const uint token = visibleMeshlets[id.x];
    const uint meshletIndex = vgDrawTokenMeshlet(token);
    const uint instanceIndex = vgDrawTokenInstance(token);
    if (meshletIndex >= constants.meshletCount || instanceIndex >= constants.instanceCount) return;
    MeshletMeta m = meshlets[meshletIndex];
    if (m.vertexCount == 0u || m.vertexCount > VG_MESHLET_MAX_VERTICES ||
        m.triangleCount == 0u || m.triangleCount > VG_MESHLET_MAX_TRIANGLES ||
        m.indexCount != m.triangleCount * 3u ||
        m.indexOffset > constants.indexCount ||
        m.indexCount > constants.indexCount - m.indexOffset)
        return;
    uint commandIndex = 0u;
    InterlockedAdd(commandCount[0], 1u, commandIndex);
    if (commandIndex >= constants.commandCapacity) return;
    DrawIndexedCommand command;
    command.indexCount = m.indexCount;
    command.instanceCount = 1;
    command.firstIndex = 0u;
    command.vertexOffset = int(m.indexOffset);
    // firstInstance carries the packed instance/meshlet token into the
    // visibility vertex shader through SV_InstanceID.
    command.firstInstance = token;
    commands[commandIndex] = command;
}
