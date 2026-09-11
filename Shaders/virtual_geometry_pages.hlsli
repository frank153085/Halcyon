static const uint VG_PAGE_RESIDENT = 1u;
static const uint VG_INVALID_PAGE_ADDRESS = 0xffffffffu;
static const uint VG_PAGE_HEADER_SIZE = 16u;

bool vgPageResident(uint pageIndex, out uint pageBase)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    pageBase = 0u;
    if (pageIndex >= info.pageCount || info.pageSize < VG_PAGE_HEADER_SIZE)
        return false;
    const PageTableEntry entry = geometryPageTable[pageIndex];
    if ((entry.flags & VG_PAGE_RESIDENT) == 0u ||
        entry.physicalPage >= info.physicalPageCount)
        return false;
    pageBase = entry.physicalPage * info.pageSize;
    return true;
}

bool vgLoadPageHeader(uint pageBase, out uint vertexCount,
    out uint meshletVertexCount, out uint triangleByteCount)
{
    vertexCount = geometryPages.Load(pageBase + 4u);
    meshletVertexCount = geometryPages.Load(pageBase + 8u);
    triangleByteCount = geometryPages.Load(pageBase + 12u);
    return true;
}

// Packed vertices are 48 bytes; the remap table immediately after them is 4
// bytes per vertex, so Meshlet-local vertex indices start at 52 bytes each.
uint vgPageMeshletVertexOffset(uint vertexCount)
{
    return VG_PAGE_HEADER_SIZE + vertexCount * 52u;
}

uint vgPageTriangleOffset(uint vertexCount, uint meshletVertexCount)
{
    return vgPageMeshletVertexOffset(vertexCount) + meshletVertexCount * 4u;
}

uint vgPageIndexOffset(uint vertexCount, uint meshletVertexCount, uint triangleByteCount)
{
    return (vgPageTriangleOffset(vertexCount, meshletVertexCount) + triangleByteCount + 3u) & ~3u;
}

bool vgLoadAlignedUint(uint address, out uint value)
{
    if ((address & 3u) != 0u)
    {
        value = 0u;
        return false;
    }
    value = geometryPages.Load(address);
    return true;
}

bool vgLoadMeshletVertex(uint pageIndex, uint index, out uint value)
{
    uint pageBase = 0u;
    uint vertexCount = 0u;
    uint meshletVertexCount = 0u;
    uint triangleByteCount = 0u;
    value = 0u;
    if (!vgPageResident(pageIndex, pageBase) ||
        !vgLoadPageHeader(pageBase, vertexCount, meshletVertexCount, triangleByteCount) ||
        index >= meshletVertexCount)
        return false;
    return vgLoadAlignedUint(
        pageBase + vgPageMeshletVertexOffset(vertexCount) + index * 4u, value);
}

bool vgLoadTriangleByte(uint pageIndex, uint byteOffset, out uint value)
{
    uint pageBase = 0u;
    uint vertexCount = 0u;
    uint meshletVertexCount = 0u;
    uint triangleByteCount = 0u;
    value = 0u;
    if (!vgPageResident(pageIndex, pageBase) ||
        !vgLoadPageHeader(pageBase, vertexCount, meshletVertexCount, triangleByteCount) ||
        byteOffset >= triangleByteCount)
        return false;
    const uint address = pageBase + vgPageTriangleOffset(vertexCount, meshletVertexCount) +
        byteOffset;
    const uint packed = geometryPages.Load(address & ~3u);
    value = (packed >> ((address & 3u) * 8u)) & 0xffu;
    return true;
}

bool vgLoadIndex(uint pageIndex, uint index, out uint value)
{
    uint pageBase = 0u;
    uint vertexCount = 0u;
    uint meshletVertexCount = 0u;
    uint triangleByteCount = 0u;
    value = 0u;
    if (!vgPageResident(pageIndex, pageBase) ||
        !vgLoadPageHeader(pageBase, vertexCount, meshletVertexCount, triangleByteCount) ||
        index >= triangleByteCount)
        return false;
    return vgLoadAlignedUint(pageBase + vgPageIndexOffset(vertexCount, meshletVertexCount,
        triangleByteCount) + index * 4u, value);
}

bool vgLoadVertex(uint pageIndex, uint index, out Vertex vertex)
{
    uint pageBase = 0u;
    uint vertexCount = 0u;
    uint meshletVertexCount = 0u;
    uint triangleByteCount = 0u;
    if (!vgPageResident(pageIndex, pageBase) ||
        !vgLoadPageHeader(pageBase, vertexCount, meshletVertexCount, triangleByteCount) ||
        index >= vertexCount)
        return false;
    const uint base = pageBase + VG_PAGE_HEADER_SIZE + index * 48u;
    uint words[12];
    [unroll]
    for (uint word = 0u; word < 12u; ++word)
    {
        if (!vgLoadAlignedUint(base + word * 4u, words[word]))
            return false;
    }
    vertex.position = asfloat(uint3(words[0], words[1], words[2]));
    vertex.normal = asfloat(uint3(words[3], words[4], words[5]));
    vertex.uv = asfloat(uint2(words[6], words[7]));
    vertex.tangent = asfloat(uint4(words[8], words[9], words[10], words[11]));
    return true;
}
