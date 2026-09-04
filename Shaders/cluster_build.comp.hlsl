// Cluster assignment contract: one invocation owns one 64x64x24 cluster.
// Light list construction is backend-selected; this no-op kernel is used by
// capability probes and keeps the descriptor/thread-group ABI validated.
[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    (void)dispatchId;
}
