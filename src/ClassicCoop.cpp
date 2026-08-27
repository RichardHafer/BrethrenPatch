// Native Classic-Coop enablement for Dead Space 3's existing Classic online
// mode.  The retail online builder already serialises GAME_MODE=3 and calls
// the session "Dead Space 3 (classic)"; these are the three PC-only guards
// that otherwise keep the menu and the host capacity single-player.

#include "Common.hpp"

static safetyhook::InlineHook gClassicModeUnlock;

// Retail asks the GameProgressManager whether mode 4 (the endgame/Suvival
// completion flag) is available before it enables the New Game+ modes.  A
// Classic save selected this way still uses DS3's ordinary save flow; no save
// file is ever changed by this hook.
static bool __fastcall ClassicModeUnlock_Hook(void* self, int, unsigned mode)
{
    if (mode == 4)
        return true;
    return gClassicModeUnlock.thiscall<bool>(self, mode);
}

struct BranchPatch
{
    const char* name;
    const char* pattern;
    size_t branchOffset;
};

// Turn a verified JNE into JMP.  Each site is only reached for GameMode 3;
// normal, Hardcore and Survival paths retain their retail code unchanged.
static bool PatchClassicBranch(const BranchPatch& patch)
{
    const DWORD match = Find(patch.pattern, patch.name);
    if (!match)
        return false;

    const DWORD branch = match + (DWORD)patch.branchOffset;
    const uint8_t before = mem::Read<uint8_t>(branch);
    if (before == 0xEB)
    {
        Log("  %-34s already enabled", patch.name);
        return true;
    }
    if (before != 0x75)
    {
        Log("  MISMATCH %-31s %08X has %02X, expected 75", patch.name, branch, before);
        return false;
    }
    if (!mem::Write<uint8_t>(branch, 0xEB))
    {
        Log("  FAILED   %-33s (could not write %08X)", patch.name, branch);
        return false;
    }

    Log("  patched %-33s %08X: 75 -> EB", patch.name, branch);
    return true;
}

void ApplyClassicCoop()
{
    if (!cfg::classicCoop)
    {
        Log("ClassicCoop: disabled");
        return;
    }

    // The patterns deliberately include both the mode comparison and the
    // following retail instructions.  This prevents an accidental nearby
    // branch from being modified after a game update.
    const BranchPatch patches[] = {
        { "Classic coop UI state",
          "83 BA 3C 80 00 00 03 75 2C 50 E8 ?? ?? ?? ?? 89 06 5E 5D C2 10 00", 7 },
        { "Classic coop selection handler",
          "83 BA 3C 80 00 00 03 75 24 5F 5E 5B 5D C2 04 00 F6 83 20 08 00 00 02", 7 },
        { "Classic coop lobby capacity",
          "83 FF 03 75 16 C7 85 6E FE FF FF 00 00 01 00 C7 85 94 FE FF FF 01 00 00 00 EB 2C 83 7D 08 02 75 16", 3 },
    };

    bool complete = true;
    for (const auto& patch : patches)
        complete = PatchClassicBranch(patch) && complete;

    Log("ClassicCoop: %s", complete ? "enabled" : "incomplete (see signatures above)");
}

void ApplyClassicModeUnlock()
{
    if (!cfg::classicModeUnlock)
    {
        Log("ClassicModeUnlock: disabled");
        return;
    }

    const DWORD gate = Find("55 8B EC 51 56 8B B1 B4 80 00 00 33 C0 B2 01",
                            "Classic/NG+ completion gate");
    if (!gate)
        return;

    gClassicModeUnlock = Hook((void*)gate, &ClassicModeUnlock_Hook);
    Log("ClassicModeUnlock: enabled (runtime only)");
}
