#pragma once

// Public umbrella include for backend-independent Halcyon core utilities.

#include "Handle.h"
#include "HandlePool.h"
#include "Log.h"
#include "Memory/FrameArena.h"
#include "Memory/HeapAllocator.h"
#include "Memory/LinearAllocator.h"
#include "Memory/MemoryStats.h"
#include "Result.h"

namespace Halcyon
{
using Core::FrameArena;
using Core::HeapAllocator;
using Core::LinearAllocator;
using Core::MemoryStats;
} // namespace Halcyon
