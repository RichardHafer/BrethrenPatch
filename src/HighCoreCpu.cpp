// Optional guard for machines with a lot of logical processors, mirroring what
// the Dead Space (2008) fixes do.
//
// The DS1/DS2 array overflow does not appear to exist here, though: DS3's
// CPUID enumeration at 0x00AA2BB0 walks a 32-bit affinity mask, so it runs at
// most 32 times into 256-byte arrays, and the worker pool it feeds is allocated
// dynamically. Capping also costs throughput, because the pool is sized from the
// processor count. Off by default; enable it only if a many-core machine
// actually crashes.

#include "Common.hpp"

void ApplyHighCoreCpu()
{
    if (!cfg::highCoreCpu)
    {
        Log("HighCoreCpu: disabled");
        return;
    }

    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const int cap = cfg::cpuCoreCap > 0 ? cfg::cpuCoreCap : 8;

    if ((int)count <= cap)
    {
        Log("HighCoreCpu: %lu logical processors, cap %d, nothing to do", count, cap);
        return;
    }

    DWORD_PTR mask = 0;
    for (int i = 0; i < cap && i < (int)(sizeof(DWORD_PTR) * 8); ++i)
        mask |= (DWORD_PTR)1 << i;

    const BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), mask);
    Log("HighCoreCpu: %lu logical processors, affinity capped to %08X (%s)",
        count, (unsigned)mask, ok ? "ok" : "failed");
}
