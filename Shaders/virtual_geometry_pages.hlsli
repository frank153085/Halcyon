static const uint VG_PAGE_RESIDENT = 1u;
static const uint VG_INVALID_PAGE_ADDRESS = 0xffffffffu;

uint vgResolvePageAddress(uint firstPage, uint streamByteOffset)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    if (info.pageSize == 0u)
        return VG_INVALID_PAGE_ADDRESS;
    const uint pageOffset = streamByteOffset / info.pageSize;
    if (firstPage > info.pageCount || pageOffset >= info.pageCount - firstPage)
        return VG_INVALID_PAGE_ADDRESS;
    const PageTableEntry entry = geometryPageTable[firstPage + pageOffset];
    if ((entry.flags & VG_PAGE_RESIDENT) == 0u ||
        entry.physicalPage >= info.physicalPageCount)
        return VG_INVALID_PAGE_ADDRESS;
    return entry.physicalPage * info.pageSize + streamByteOffset % info.pageSize;
}

bool vgLoadPageUint(uint firstPage, uint streamByteOffset, out uint value)
{
    const uint address = vgResolvePageAddress(firstPage, streamByteOffset);
    if (address == VG_INVALID_PAGE_ADDRESS)
    {
        value = 0u;
        return false;
    }
    value = geometryPages.Load(address);
    return true;
}

bool vgLoadMeshletVertex(uint index, out uint value)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    if (index >= info.meshletVertexCount)
    {
        value = 0u;
        return false;
    }
    return vgLoadPageUint(info.meshletVerticesFirstPage, index * 4u, value);
}

bool vgLoadTriangleByte(uint byteOffset, out uint value)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    const uint address = vgResolvePageAddress(
        info.meshletTrianglesFirstPage, byteOffset);
    if (byteOffset >= info.triangleByteCount || address == VG_INVALID_PAGE_ADDRESS)
    {
        value = 0u;
        return false;
    }
    const uint packed = geometryPages.Load(address & ~3u);
    value = (packed >> ((address & 3u) * 8u)) & 0xffu;
    return true;
}

bool vgLoadIndex(uint index, out uint value)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    if (index >= info.indexCount)
    {
        value = 0u;
        return false;
    }
    return vgLoadPageUint(info.indicesFirstPage, index * 4u, value);
}

bool vgLoadVertex(uint index, out Vertex vertex)
{
    const GeometryPageInfo info = geometryPageInfoBuffer[0];
    if (index >= info.vertexCount)
        return false;
    const uint base = index * 48u;
    uint words[12];
    [unroll]
    for (uint word = 0u; word < 12u; ++word)
    {
        if (!vgLoadPageUint(info.verticesFirstPage, base + word * 4u, words[word]))
            return false;
    }
    vertex.position = asfloat(uint3(words[0], words[1], words[2]));
    vertex.normal = asfloat(uint3(words[3], words[4], words[5]));
    vertex.uv = asfloat(uint2(words[6], words[7]));
    vertex.tangent = asfloat(uint4(words[8], words[9], words[10], words[11]));
    return true;
}
