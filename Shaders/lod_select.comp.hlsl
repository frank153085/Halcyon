// M6 GPU LOD selection. The selected list is intentionally independent from
// the draw command format so Indexed and Mesh Shader builders consume the
// exact same node IDs.
struct DagNode
{
    uint clusterIndex;
    uint parentIndex;
    uint firstChild;
    uint childCount;
    uint lodDepth;
    uint flags;
    float4 sphere;
    float geometricError;
    float3 _padding;
    uint2 _stridePadding;
};
struct DagEdge { uint parent; uint child; };
struct LodState { uint currentNode; uint candidateNode; uint pendingFrames; uint pad; };
struct PageTableEntry
{
    uint physicalPage;
    uint generation;
    uint flags;
    uint lastRequestedFrame;
};
struct PageDependencyRange { uint offset; uint count; };
struct LodFrame
{
    float4x4 viewProjection;
    float4 cameraAndFov;
    uint4 counts;
    uint4 outputAndRoot;
    float4 thresholds;
};

[[vk::binding(0, 0)]] StructuredBuffer<DagNode> dagNodes;
[[vk::binding(1, 0)]] StructuredBuffer<DagEdge> dagEdges;
[[vk::binding(2, 0)]] RWStructuredBuffer<LodState> lodStates;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> selectedNodes;
// [0] active frontier count, [1] completed hysteresis transitions this frame.
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> selectionCounters;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> balanceDepth;
[[vk::binding(6, 0)]] StructuredBuffer<uint> adjacencyOffsets;
[[vk::binding(7, 0)]] StructuredBuffer<uint> adjacencyIndices;
[[vk::binding(8, 0)]] RWStructuredBuffer<PageTableEntry> pageTable;
[[vk::binding(9, 0)]] StructuredBuffer<PageDependencyRange> nodePageRanges;
[[vk::binding(10, 0)]] StructuredBuffer<uint> pageDependencies;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> pageRequests;
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> pageRequestCount;
[[vk::push_constant]] ConstantBuffer<LodFrame> frame;

static const uint VG_PAGE_RESIDENT = 1u;

bool nodePagesResident(uint nodeIndex)
{
    const PageDependencyRange range = nodePageRanges[nodeIndex];
    bool resident = true;
    [loop]
    for (uint dependency = 0u; dependency < range.count; ++dependency)
    {
        const uint pageIndex = pageDependencies[range.offset + dependency];
        if (pageIndex >= frame.outputAndRoot.y)
        {
            resident = false;
            continue;
        }
        if ((pageTable[pageIndex].flags & VG_PAGE_RESIDENT) != 0u)
            continue;
        resident = false;
        uint previousFrame = 0u;
        InterlockedExchange(pageTable[pageIndex].lastRequestedFrame,
            frame.outputAndRoot.w, previousFrame);
        if (previousFrame != frame.outputAndRoot.w)
        {
            uint requestIndex = 0u;
            InterlockedAdd(pageRequestCount[0], 1u, requestIndex);
            if (requestIndex < frame.outputAndRoot.x)
                pageRequests[requestIndex] = pageIndex;
        }
    }
    return resident;
}

float projectedError(DagNode node)
{
    float distance = max(length(node.sphere.xyz - frame.cameraAndFov.xyz), 1.0e-5);
    return node.geometricError * frame.counts.x /
        (2.0 * tan(max(frame.cameraAndFov.w, 1.0e-4) * 0.5) * distance);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= frame.counts.w) return;
    const uint phase = frame.outputAndRoot.z;
    if (phase == 4u)
    {
        if (id.x == 0u)
        {
            selectionCounters[2] = (min(selectionCounters[0],
                frame.outputAndRoot.x) + 63u) / 64u;
            selectionCounters[3] = 1u;
            selectionCounters[4] = 1u;
        }
        return;
    }
    LodState state = lodStates[id.x];
    DagNode current = dagNodes[id.x];

    // Phase 0 updates a persistent refine/coarsen decision for every node.
    // currentNode/candidateNode store boolean refinement decisions in the GPU
    // ABI; the CPU selection state retains its node-index interpretation.
    if (phase == 0u)
    {
        const float error = projectedError(current);
        uint candidate = state.currentNode;
        if (current.childCount == 0u || error < frame.thresholds.y)
            candidate = 0u;
        else if (error > frame.thresholds.x)
            candidate = 1u;
        if (candidate == state.currentNode)
        {
            state.candidateNode = candidate;
            state.pendingFrames = 0u;
        }
        else if (state.candidateNode != candidate)
        {
            state.candidateNode = candidate;
            state.pendingFrames = 1u;
        }
        else if (++state.pendingFrames >= 2u)
        {
            state.currentNode = candidate;
            state.pendingFrames = 0u;
            InterlockedAdd(selectionCounters[1], 1u);
        }
        lodStates[id.x] = state;
        return;
    }

    // pad bit 0 is the effective refinement decision for this frame. It starts
    // with the hysteresis decision and can only move from coarse to fine while
    // balancing, so repeated dispatches converge without an auxiliary copy.
    if (phase == 1u)
    {
        uint decision = state.currentNode & 1u;
        if (decision != 0u)
        {
            [loop]
            for (uint childEdge = current.firstChild;
                childEdge < current.firstChild + current.childCount &&
                    childEdge < frame.counts.z; ++childEdge)
            {
                const uint child = dagEdges[childEdge].child;
                if (child >= frame.counts.y || !nodePagesResident(child))
                {
                    decision = 0u;
                    break;
                }
            }
        }
        lodStates[id.x].pad = decision;
        return;
    }

    // Phase 2 performs one monotonic local-balance iteration. A selected node
    // is refined when an adjacent same-depth region is already refined for two
    // levels. Repeating this phase once per DAG level propagates the finer
    // choice until every adjacent frontier pair differs by at most one level.
    if (phase == 2u)
    {
        if ((state.pad & 1u) != 0u || current.childCount == 0u)
            return;
        uint ancestor = current.parentIndex;
        [loop]
        for (uint guard = 0u; ancestor != 0xffffffffu && guard < frame.counts.y; ++guard)
        {
            if ((lodStates[ancestor].pad & 1u) == 0u)
                return;
            ancestor = dagNodes[ancestor].parentIndex;
        }
        const uint first = adjacencyOffsets[current.clusterIndex];
        const uint last = adjacencyOffsets[current.clusterIndex + 1u];
        [loop]
        for (uint edge = first; edge < last; ++edge)
        {
            const uint neighbor = adjacencyIndices[edge];
            if (neighbor >= frame.counts.y || (lodStates[neighbor].pad & 1u) == 0u)
                continue;
            const DagNode neighborNode = dagNodes[neighbor];
            [loop]
            for (uint childEdge = neighborNode.firstChild;
                childEdge < neighborNode.firstChild + neighborNode.childCount &&
                    childEdge < frame.counts.z; ++childEdge)
            {
                const uint child = dagEdges[childEdge].child;
                if (child < frame.counts.y && (lodStates[child].pad & 1u) != 0u)
                {
                    InterlockedOr(lodStates[id.x].pad, 1u);
                    InterlockedAdd(balanceDepth[0], 1u);
                    return;
                }
            }
        }
        return;
    }

    // Phase 3 publishes pad bit 1 as the active frontier. Keeping the effective
    // decision in bit 0 makes ancestor reads race-free within this dispatch.
    const uint decision = state.pad & 1u;
    bool active = decision == 0u;
    uint ancestor = current.parentIndex;
    [loop]
    for (uint guard = 0u; ancestor != 0xffffffffu && guard < frame.counts.y; ++guard)
    {
        if ((lodStates[ancestor].pad & 1u) == 0u)
        {
            active = false;
            break;
        }
        ancestor = dagNodes[ancestor].parentIndex;
    }
    lodStates[id.x].pad = decision | (active ? 2u : 0u);
    if (!active) return;
    uint output = 0u;
    InterlockedAdd(selectionCounters[0], 1u, output);
    if (output < frame.outputAndRoot.x) selectedNodes[output] = id.x;
}
