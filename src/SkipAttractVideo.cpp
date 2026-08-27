// Skips the attract video, the loop that plays when you leave the game
// sitting in the main menu.
//
// Not to be confused with the publisher logos at launch; those go through a
// completely different path (see SkipStartupLogos.cpp). A trace on the movie
// dispatcher recorded zero calls during startup, and the executable names only
// three movies at all: attract.vp6, attract_eu.vp6 and previously.vp6.
//
// There are two call sites, picked by a region branch. Both call the movie
// dispatcher and then push the frontend into its "movie active" state. Leave
// the state write alone and only neutralise the call. The next tick sees no
// movie running and carries on.
//
// The call can't just be NOPed. The dispatcher at 0x0050B190 ends in ret 0xC,
// so it cleans its own three arguments, and neither call site touches ESP:
//
//     00516B0B  50              push eax          ; eax is 0 on this path
//     00516B0C  50              push eax
//     00516B0D  68 4841D400     push "attract.vp6"
//     00516B12  E8 7946FFFF     call 0x0050B190   ; cleans 12 bytes on return
//     ...
//     00516B29  5F 5E 5D C2 04  pop edi; pop esi; pop ebp; ret 4
//
// Five NOPs would leave those 12 bytes on the stack, the three pops would read
// the wrong slots and `ret 4` would jump into whatever was there. So the call
// becomes `add esp,0xC` plus two NOPs: same five bytes, balanced stack, and
// the return value isn't used afterwards.

#include "Common.hpp"

static bool NeutralizeMovieCall(const char* name, const char* pattern,
                         size_t callOffset)
{
    const DWORD site = Find(pattern, name);
    if (!site)
        return false;

    const DWORD call = site + static_cast<DWORD>(callOffset);
    if (mem::Read<uint8_t>(call) != 0xE8)
    {
        Log("Attract video: %s has %02X at %08X, expected E8",
            name, mem::Read<uint8_t>(call), call);
        return false;
    }


    // add esp,0x0C ; nop ; nop does the cleanup the callee would have done.
    constexpr uint8_t kPatch[5] = { 0x83, 0xC4, 0x0C, 0x90, 0x90 };
    for (size_t i = 0; i < sizeof(kPatch); ++i)
    {
        if (!mem::Write<uint8_t>(call + i, kPatch[i]))
        {
            Log("Attract video: could not patch %s at %08X", name, call + (DWORD)i);
            return false;
        }
    }

    Log("Attract video: skipped %-20s (movie call %08X)", name, call);
    return true;
}

void ApplySkipAttractVideo()
{
    if (!cfg::skipAttractVideo)
    {
        Log("Attract video skip: disabled");
        return;
    }

    // The trailing state writes are what make these unique. Without them the
    // pattern would match any call to the movie dispatcher.
    const bool normal = NeutralizeMovieCall(
        "attract.vp6",
        // Two `push eax` here (eax is 0), not `push 0` like the EU branch.
        "50 50 68 48 41 D4 00 E8 79 46 FF FF 8D 8E 6C FF FF FF "
        "C7 46 10 02 00 00 00 E8 77 91 FE FF",
        7);
    const bool european = NeutralizeMovieCall(
        "attract_eu.vp6",
        "6A 01 6A 00 68 38 41 D4 00 E8 53 46 FF FF 8D 8E 6C FF FF FF "
        "C7 46 10 02 00 00 00 E8 51 91 FE FF",
        9);

    Log("Attract video skip: %s", normal && european ? "enabled" :
        "incomplete (see signatures above)");
}
