// Crash guard for checkpoint restarts.
//
// Loading a checkpoint could die on a null pointer. Two identical dumps on
// 17.08.2026 (38464 and 22740): 0xC0000005 at 0x005F9BC0, reading 0x0000000C.
//
// A message handler on an NPC class reads a singleton without a null check.
// The function at 0x005F9B70 walks an array (stride 0x20), compares a time per
// entry, and when one expires it takes the action branch:
//
//     005F9BBA  mov edx,[0x011D3B88]        ; singleton
//     005F9BC0  mov eax,[edx+0x0C]          ; <- crash, edx is null
//     005F9BC3  mov ecx,[eax+0x0F50]
//     005F9BDA  call 0xA7DE60               ; fire the event
//     005F9BE8  mov byte [esi+eax+0x1C],1   ; mark the entry done
//
// The first attempt skipped the whole function when the singleton was empty,
// which also threw away the event and the done-flag. It fired eight times
// during level load (18:53:09-18:53:13, before IN_ACTIVE_GAMEPLAY) and the
// enemies that should have appeared there never did. The crash guard was
// costing us the spawns.
//
// So this version only replaces the missing pointer. A mid-hook on 0x005F9BC0
// swaps EDX for a substitute block when it is null, and the function runs to
// completion.
//
// The block only has to survive one read at [[singleton+0x0C]+0x0F50]. While
// the singleton exists we remember the real intermediate pointer and hand that
// back, so the event gets the last real value rather than an invented zero.
// Only if a valid singleton was never seen does the block point at itself.
//
// Hook goes on 0x005F9BC0, not 0x005F9BBA: EDX can only be checked after the
// load, and 0x005F9BBA is the target of the loop's `ja`, no mid-hook there.

#include "Common.hpp"

namespace
{
constexpr uintptr_t kGuardedSingletonRva = 0x00DD3B88;
constexpr uintptr_t kIntermediateOffset  = 0x0C;
constexpr uintptr_t kReadOffset          = 0x0F50;

static safetyhook::MidHook gActionBranch;
static long gPatched = 0;
static long gRemembered = 0;

// Substitute block. Large enough that a read at +0x0F50 still lands inside it
// when the intermediate pointer points at the block itself.
alignas(16) static unsigned char gFallbackBlock[0x1100]{};
// The last real intermediate pointer, seen while the singleton was there.
static volatile LONG gLastGoodIntermediate = 0;

static bool Readable(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= end && size <= end - address;
}

// Sets the block up so `[block + 0x0C]` points at something usable: the real
// pointer we remembered if we have one, otherwise the block itself.
static uintptr_t PrepareFallback()
{
    const uintptr_t block = reinterpret_cast<uintptr_t>(gFallbackBlock);
    const uintptr_t remembered =
        static_cast<uintptr_t>(InterlockedCompareExchange(&gLastGoodIntermediate, 0, 0));

    uintptr_t intermediate = block;      // Selbstverweis -> liest Null
    if (remembered && Readable(remembered + kReadOffset, sizeof(uintptr_t)))
        intermediate = remembered;

    *reinterpret_cast<uintptr_t*>(block + kIntermediateOffset) = intermediate;
    return block;
}

// Mid-hook on 0x005F9BC0, where EDX holds the singleton that was just read.
static void ActionBranch_Hook(safetyhook::Context& ctx)
{
    const uintptr_t singleton = ctx.edx;

    if (singleton && Readable(singleton + kIntermediateOffset, sizeof(uintptr_t)))
    {
        // Normal case. Remember the pointer in case it goes missing later.
        const uintptr_t intermediate =
            mem::Read<uintptr_t>(singleton + kIntermediateOffset);
        if (intermediate && Readable(intermediate + kReadOffset, sizeof(uintptr_t)))
        {
            if (InterlockedExchange(&gLastGoodIntermediate,
                                    static_cast<LONG>(intermediate)) == 0 &&
                InterlockedIncrement(&gRemembered) == 1)
                Log("CheckpointGuard: remembered fallback value "
                    "(intermediate %08X)", static_cast<unsigned>(intermediate));
        }
        return;                          // nothing to do
    }

    // The crash case: EDX is null. Point it at the substitute block so the
    // function runs to completion instead of dying.
    ctx.edx = PrepareFallback();

    if (InterlockedIncrement(&gPatched) <= 8)
        Log("CheckpointGuard: singleton empty, EDX redirected to substitute "
            "block %08X (#%ld). Event and done-flag are preserved.",
            static_cast<unsigned>(ctx.edx),
            InterlockedCompareExchange(&gPatched, 0, 0));
}
} // namespace

void ApplyFourPlayerCheckpointGuard()
{
    if (!cfg::fourPlayerCheckpointGuard)
        return;

    // Auf den Aktionszweig gepinnt: `mov eax,[edx+0x0C]` gefolgt von
    // `mov ecx,[eax+0x0F50]`. Beides zusammen ist eindeutig.
    const DWORD site = Find(
        "8B 42 0C 8B 88 50 0F 00 00 33 C0 50",
        "FourPlayerCheckpointGuard singleton read");
    if (site)
        gActionBranch = safetyhook::create_mid(reinterpret_cast<void*>(site),
                                              &ActionBranch_Hook);

    Log("FourPlayerCheckpointGuard: %s. On an empty singleton it only redirects "
        "the pointer (0x005F9BC0) instead of skipping the function, so the event "
        "and its done flag survive, and with them the spawns.",
        gActionBranch ? "active" : "NOT installed");
}
