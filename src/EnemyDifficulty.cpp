// READ-ONLY measurement for the planned player-count difficulty scaling.
// Nothing here writes to game memory. See UNITED_SKALIERUNG_PLAN.md.
//
// It answers three questions that have to be settled BEFORE a factor is
// applied anywhere, because getting them wrong means the factor silently
// multiplies itself:
//
//   1. Is DamageData::m_amount really at +0x50 on the PC?
//   2. Which machine runs the damage path for which victim: only the host,
//      only the owner, or everyone? A factor applied on two machines squares.
//   3. Do players and enemies come through the same code, so one scaling point
//      can serve both directions?
//
// How the addresses were found, symbols first (Xbox PDB deadspace_r.pdb):
//
//   Sentient::ApplyDamageModifiers(DamageData&)   protected virtual, slot 0x264
//     +- DeadSpaceNPC::ApplyDamageModifiers        enemy takes damage
//     +- Player::ApplyDamageModifiers              player takes damage
//
//   DamageData (Xbox): +0x50 m_amount, +0x54 m_type, +0x64 m_knockbackIntensity,
//                      +0x80 m_attacker (SafePtr), +0x88 m_attackerNetId
//
// The PC vtable slot is 0x260, not 0x264, a shift of -4, confirmed by reading
// the known Player vtable 0x00D283B0 out of a dump and checking the candidates
// against their own terminating `ret`:
//
//   +0x260 -> 0x00447E50  ret 4   <- a chain of fmul on [arg+0x50]
//   +0x264 -> 0x00458050  long, no ret 4
//   +0x268 -> 0x004398A0  ret 4, 23 bytes, far too short
//
// 0x00447E50 settles question 1 on its own; it multiplies [esi+0x50] four
// separate times, so +0x50 is m_amount on the PC exactly as on the Xbox:
//
//   0x00447E60  test dword ptr [esi+0x54], 0x4040000    ; damage type flags
//   0x00447E69  mov  ecx, [0x011D1A90 -> game mode]     ; per-mode multiplier
//   0x00447E74  fmul dword ptr [esi+0x50]
//   0x00447E77  fstp dword ptr [esi+0x50]
//
// The shared caller is 0x006C0FC0, located by searching for a vtable-slot load
// of 0x260 followed by an indirect call. It carries BOTH directions:
//
//   0x006C1345  mov edx, [esi]              ; esi = the Sentient being hit
//   0x006C1347  mov eax, [edx+0x260]
//   0x006C134D  push edi                    ; edi = DamageData
//   0x006C1350  call eax                    ; ApplyDamageModifiers
//
// and its own prologue proves the argument layout without guessing:
//
//   0x006C1140  push ebx / mov ebx,esp / sub esp,8 / and esp,-16 / add esp,4
//   0x006C1179  mov edi, [ebx+8]            ; arg0 = DamageData
//   0x006C117C  mov esi, ecx                ; this  = victim
//
// So a mid-hook on the FUNCTION ENTRY sees every damage event to every
// Sentient, with ECX = victim and the DamageData as the first stack argument.
// The entry is no jump target, which the two sites around the call at
// 0x006C1345 are, that is why the hook sits here and not next to the call.
//
// CORRECTED 2026-08-16 after the first live run. The function start was taken
// from a disassembler heuristic, which answered 0x006C0FC0, a DIFFERENT
// function. The hook then read a stack slot that happened to hold nothing
// useful, and the log showed 60 events on one and the same object with
// amount=0.000 and a stack pointer where the type flags belong. Scanning
// backwards for alignment padding gives the real boundary at 0x006C1140.
// The stack arithmetic was right all along; only the function was wrong.
//
// Note the prologue realigns the stack, so EBX rather than EBP carries the
// original ESP: [ebx+4] is the return address and [ebx+8] is arg0. At the
// FIRST instruction none of that has happened yet, so arg0 is still at
// [ESP+4], which is what the hook reads.

#include "Common.hpp"

namespace
{
// Same constants the end-round module uses; duplicated rather than shared
// because this module is meant to be removable in one piece.
constexpr uintptr_t kOnlineServicesGlobalRva = 0x00DD1A1C;
constexpr uintptr_t kOnlineSessionOffset     = 0x28;
constexpr uintptr_t kIsHostVtableOffset      = 0x54;

constexpr uintptr_t kPlayerManagerGlobalRva = 0x00DD3B78;
constexpr uintptr_t kPlayerFromBaseOffset   = 0x3A0;
constexpr uintptr_t kPlayerVtable           = 0x00D283B0;

// DamageData, PC layout. m_amount is proven by 0x00447E50; the rest is the
// Xbox layout and is only ever REPORTED, never relied on.
constexpr uintptr_t kDamageAmount = 0x50;
constexpr uintptr_t kDamageType   = 0x54;

static safetyhook::MidHook gDamagePipeline;
static safetyhook::MidHook gPlayerModifiers;

static long gPipelineLines = 0;
static long gPlayerLines = 0;

static bool Readable(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= end && size <= end - address;
}

// -1 unknown, 1 host, 0 client.
static int LocalIsHost()
{
    if (!g.module)
        return -1;

    const uintptr_t global = reinterpret_cast<uintptr_t>(g.module) + kOnlineServicesGlobalRva;
    if (!Readable(global, sizeof(uintptr_t)))
        return -1;
    const uintptr_t services = mem::Read<uintptr_t>(global);
    if (!services || !Readable(services + kOnlineSessionOffset, sizeof(uintptr_t)))
        return -1;
    const uintptr_t session = mem::Read<uintptr_t>(services + kOnlineSessionOffset);
    if (!session || !Readable(session, sizeof(uintptr_t)))
        return -1;
    const uintptr_t vtable = mem::Read<uintptr_t>(session);
    if (!vtable || !Readable(vtable + kIsHostVtableOffset, sizeof(uintptr_t)))
        return -1;
    const uintptr_t fn = mem::Read<uintptr_t>(vtable + kIsHostVtableOffset);
    if (!fn)
        return -1;

    typedef bool(SAFETYHOOK_THISCALL* IsHostFn)(void*);
    return reinterpret_cast<IsHostFn>(fn)(reinterpret_cast<void*>(session)) ? 1 : 0;
}

static const char* RoleName(int role)
{
    return role < 0 ? "unknown" : (role ? "HOST" : "client");
}

// Roster slot of a Player object, or -1 when it is not a roster player.
static int RosterSlot(uintptr_t player)
{
    if (!player || !g.module)
        return -1;

    const uintptr_t global = reinterpret_cast<uintptr_t>(g.module) + kPlayerManagerGlobalRva;
    if (!Readable(global, sizeof(uintptr_t)))
        return -1;
    const uintptr_t manager = mem::Read<uintptr_t>(global);
    if (!manager || !Readable(manager, 0x10))
        return -1;

    const uintptr_t array = mem::Read<uintptr_t>(manager + 0x04);
    const unsigned count = mem::Read<unsigned>(manager + 0x0C);
    if (!array || count > 16 || !Readable(array, count * sizeof(uintptr_t)))
        return -1;

    for (unsigned i = 0; i < count; ++i)
    {
        const uintptr_t base = mem::Read<uintptr_t>(array + i * sizeof(uintptr_t));
        if (base && base - kPlayerFromBaseOffset == player)
            return static_cast<int>(i);
    }
    return -1;
}

// Classifies the victim without needing every NPC vtable: a roster player is
// recognised by its slot, anything else is reported with its vtable so the
// enemy classes can be identified from the log afterwards.
static void DescribeVictim(uintptr_t victim, char* out, size_t size)
{
    const uintptr_t vtable =
        (victim && Readable(victim, sizeof(uintptr_t))) ? mem::Read<uintptr_t>(victim) : 0;
    const int slot = RosterSlot(victim);

    if (slot >= 0)
        _snprintf_s(out, size, _TRUNCATE, "PLAYER roster slot %d (vtable %08X%s)",
                    slot, static_cast<unsigned>(vtable),
                    vtable == kPlayerVtable ? "" : " UNEXPECTED");
    else
        _snprintf_s(out, size, _TRUNCATE, "non-player (vtable %08X)",
                    static_cast<unsigned>(vtable));
}

// 0x006C0FC0, function entry: ECX = victim, first stack argument = DamageData.
// At the very first instruction the return address is at [ESP], so arg0 is at
// [ESP+4], because the prologue has not pushed anything yet.
// ---------------------------------------------------------------------------
// Difficulty factors by player count.
//
// The one real danger is a factor that multiplies itself because two machines
// scale the same logical damage: 2.0x at eight players instead of the
// intended 1.4x. Which machine actually runs the damage path is not measured;
// the probe was deliberately off during the three-player test so the logs
// stayed readable.
//
// So the scaling is host-only by default. That makes doubling impossible by
// construction, whatever the answer turns out to be. If player damage does
// get calculated on the victim, the player factor will apply incompletely on
// the host. That is the harmless outcome, and visible in the log because every
// application records which side it was on.
//
// The curve is the same as everywhere else in the patch: one step per player
// from three upwards. Both factors are hard-clamped so a typo in the INI
// cannot produce an unplayable game.
constexpr unsigned kDifficultyMinPercent = 10;
constexpr unsigned kDifficultyMaxPercent = 500;

// Player count from the PlayerManager. Falls back to 1 while the manager is
// not up yet, which leaves the curve flat and only applies the base value -
// the safe outcome.
static unsigned LivePlayerCountForScaling()
{
    if (!g.module)
        return 1;
    const uintptr_t global = reinterpret_cast<uintptr_t>(g.module) + kPlayerManagerGlobalRva;
    if (!Readable(global, sizeof(uintptr_t)))
        return 1;
    const uintptr_t manager = mem::Read<uintptr_t>(global);
    if (!manager || !Readable(manager, 0x10))
        return 1;
    const unsigned count = mem::Read<unsigned>(manager + 0x0C);
    return count >= 1 && count <= 16 ? count : 1;
}

static long gEnemyScaled = 0;
static long gPlayerScaled = 0;
static long gDifficultyLines = 0;

// Percentage of the base value at this player count. `perPlayer` is the
// increment per player from the third onwards, in percentage points.
// Hard lower bound on enemy damage taken.
//
// Proven on the Guardian: at 90% it does not die, not even with a very strong
// weapon. Add 1.10x from the trainer and it dies immediately, even with a weak
// revolver, purely through the tentacles.
//
// The reason is not a health calculation but a threshold: a tentacle or a limb
// comes off when a single hit exceeds a fixed value. Pushing damage below that
// does not make the enemy tougher, it makes it unkillable, and weapon power
// does not help, because what counts is the individual hit, not the total.
//
// A warning in the INI is not enough, because DifficultyPerPlayer lowers the
// enemy value further per player, so 100 becomes 90 at three players. The
// bound lives here in code where nobody can undercut it by accident. Anyone
// who genuinely wants tougher enemies has to raise their health, not lower
// the damage they take.
// Which of the two directions is being scaled. This used to be inferred from
// the value (`basePercent <= 100`), which was wrong: the bound below then also
// caught the player direction, so PlayerDamageTakenPercent=80 was silently
// pulled back up to 100.
enum class Direction { Enemy, Player };

static unsigned DifficultyPercent(unsigned playerCount, unsigned basePercent,
                                  unsigned perPlayer, Direction direction)
{
    // Damage taken by enemies is never pushed below 100, whatever the INI says
    // and however many players join.
    //
    // Dismemberment needs a single hit above a fixed threshold; it is not a
    // percentage of health. Below 100 the hits fall under that threshold, limbs
    // stop coming off, and a Guardian, whose tentacles are its only weak
    // points, cannot be killed at all. Verified in game.
    if (direction == Direction::Enemy && basePercent < 100)
        return 100;

    if (playerCount < 3 || perPlayer == 0)
        return basePercent;
    const unsigned extra = (playerCount - 2) * perPlayer;
    unsigned result = basePercent > 100
        ? basePercent + extra          // the player takes MORE damage
        : (basePercent > extra ? basePercent - extra : kDifficultyMinPercent);
    if (result < kDifficultyMinPercent)
        result = kDifficultyMinPercent;
    if (result > kDifficultyMaxPercent)
        result = kDifficultyMaxPercent;
    return result;
}

// Applies the factor to m_amount. Returns true if anything was written.
static bool ScaleDamageAmount(uintptr_t damage, unsigned percent)
{
    if (percent == 100 || !Readable(damage + kDamageAmount, sizeof(float)))
        return false;
    const float before = mem::Read<float>(damage + kDamageAmount);
    // Only touch real, finite, positive damage values. A NaN or a zero here
    // would mean the pointer is wrong, and then doing nothing beats writing.
    if (!(before > 0.001f && before < 100000.0f))
        return false;
    const float after = before * (static_cast<float>(percent) / 100.0f);
    if (!(after > 0.0f && after < 1000000.0f))
        return false;
    return mem::Write<float>(damage + kDamageAmount, after);
}

static void EnemyDamageScaling_Hook(safetyhook::Context& ctx);
static void PlayerDamageScaling_Hook(safetyhook::Context& ctx);


static void DamagePipeline_Hook(safetyhook::Context& ctx)
{
    // Scale and learn first, before the diagnostic line limit. Otherwise the
    // limit would switch both off after 60 events. That would hurt here: the
    // damage path is the only place an enemy class gets confirmed, and in a
    // longer fight the interesting classes turn up late.
    EnemyDamageScaling_Hook(ctx);

    // The full object walk was originally tied to the known-family switch.
    // That made regular MoreEnemies play inspect memory on every bullet hit.
    // The shipped allow-list is static and needs no damage-path work; only
    // explicit runtime learning or the research probe may enter this block.
    const bool inspectMemory = cfg::fourPlayerRuntimeSpawnClass ||
                               cfg::fourPlayerDamageMemoryProbe;
    if (inspectMemory && ctx.esi && RosterSlot(ctx.esi) < 0 &&
        Readable(ctx.esi, sizeof(uintptr_t)))
    {
        const uintptr_t opferVtable = mem::Read<uintptr_t>(ctx.esi);

        // The entity GUID sits at +0x24 in the object header. The same GUID
        // showed up as `target=` in the spawn log during the same run. It is
        // the more precise identifier, because the vtable lumps many enemy types
        // into a handful of classes.
        constexpr unsigned kEntityGuidOffset = 0x24;
        if (Readable(ctx.esi + kEntityGuidOffset, 16))
        {
            const uint32_t g[4] = {
                mem::Read<uint32_t>(ctx.esi + kEntityGuidOffset + 0),
                mem::Read<uint32_t>(ctx.esi + kEntityGuidOffset + 4),
                mem::Read<uint32_t>(ctx.esi + kEntityGuidOffset + 8),
                mem::Read<uint32_t>(ctx.esi + kEntityGuidOffset + 12),
            };
            // The GUID names the individual spawner, not the enemy type -
            // the same snow slasher from another corner has a different one.
            // What we want is a field that is identical for every enemy of the
            // same type and differs between types. Candidates are values
            // pointing into the module (class-wide tables) or small numbers
            // (type ids); heap pointers differ per instance and drop out.
            if (cfg::fourPlayerDamageMemoryProbe)
            {
                char fp[220] = {};
                // `g` is the local GUID field here, so the global state has
                // to be addressed explicitly.
                const uintptr_t base = reinterpret_cast<uintptr_t>(::g.module);
                size_t len = 0;
                for (unsigned off = 0x04; off <= 0x80 && len < sizeof(fp) - 20;
                     off += 4)
                {
                    if (off >= 0x24 && off <= 0x30)   // the GUID itself
                        continue;
                    if (!Readable(ctx.esi + off, sizeof(uint32_t)))
                        break;
                    const uint32_t v = mem::Read<uint32_t>(ctx.esi + off);
                    const bool imModul = (v > base && v < base + 0x01700000);
                    const bool klein   = (v != 0 && v < 0x10000);
                    if (!imModul && !klein)
                        continue;
                    len += static_cast<size_t>(std::snprintf(
                        fp + len, sizeof(fp) - len, "%s%02X=%X",
                        len ? "," : "", off, v));
                }
                char fpZeile[300];
                std::snprintf(fpZeile, sizeof(fpZeile), "vtable %08X  %s",
                              static_cast<unsigned>(opferVtable), fp);
                damagetakers::FingerprintOnce(g, fpZeile);
            }

            if (cfg::fourPlayerRuntimeSpawnClass)
            {
                char notiz[300];
                std::snprintf(notiz, sizeof(notiz), "vtable %08X  %s",
                              static_cast<unsigned>(opferVtable),
                              damagetakers::WasSpawnTarget(g)
                                  ? "SPAWNED - real enemy"
                                  : "not spawned - no effect");
                if (damagetakers::voll)
                {
                    damagetakers::voll = false;
                    Log("FourPlayerSpawns: ACHTUNG - Lernliste ist voll (%u "
                        "entries). Nothing more will be learned from here.",
                        damagetakers::kGuidCapacity);
                }
                if (damagetakers::GuidRemember(g, notiz))
                    Log("EnemySpawns: enemy learned: "
                        "%08X-%08X-%08X-%08X (vtable %08X)",
                        g[0], g[1], g[2], g[3],
                        static_cast<unsigned>(opferVtable));
            }
        }

        // The Vtable list is persistent runtime learning, not normal gameplay.
        if (cfg::fourPlayerRuntimeSpawnClass)
        {
            char name[64] = {};
            unsigned nameOffset = 0xFFFFFFFFu;
            damagetakers::Remember(opferVtable,
                                   reinterpret_cast<uintptr_t>(g.module),
                                   name, sizeof(name), &nameOffset);
        }
    }

    if (!cfg::fourPlayerScalingDiagnostics)
        return;

    const long line = InterlockedIncrement(&gPipelineLines);
    if (line > 60)
        return;

    // Second attempt. Two tries at the function entry failed the same way:
    // ECX was right, the argument was not. First 0x006C0FC0 (wrong function,
    // guessed by a heuristic), then 0x006C1140, which fired 60 times in two
    // seconds before the player had even spawned, so also not the damage path.
    //
    // Hunting for the function boundary was the mistake. The call site itself
    // is unambiguous and has both values sitting in registers:
    //
    //   0x006C1343  jne  0x006C1354        ; jump target is past the call
    //   0x006C1345  mov  edx, [esi]        ; <- hook goes here
    //   0x006C1347  mov  eax, [edx+0x260]  ; ApplyDamageModifiers
    //   0x006C134D  push edi               ; edi = DamageData
    //   0x006C134E  mov  ecx, esi          ; esi = the Sentient being hit
    //   0x006C1350  call eax
    //
    // Nothing jumps into the range between 0x006C1345 and the call, so a
    // mid-hook is safe there. No more guessing at the stack.
    //
    // That +0x260 really is ApplyDamageModifiers is confirmed three ways: the
    // Player vtable holds 0x00447E50 there (which demonstrably produces real
    // damage values), and two non-player vtables hold 0x00A52A60, a function
    // that returns immediately, i.e. the empty base stub
    // Sentient::ApplyDamageModifiers from the PDB.
    const uintptr_t victim = ctx.esi;
    const uintptr_t damage = ctx.edi;

    if (!damage || !Readable(damage + kDamageType, sizeof(uint32_t)))
        return;

    char who[96];
    DescribeVictim(victim, who, sizeof(who));

    Log("EnemyDifficulty: [%ld] damage on this machine (%s): victim=%08X %s; "
        "amount=%.3f typeFlags=%08X",
        line, RoleName(LocalIsHost()), static_cast<unsigned>(victim), who,
        mem::Read<float>(damage + kDamageAmount),
        mem::Read<unsigned>(damage + kDamageType));
}

// Enemy damage. This path sees every Sentient that gets hit, players included,
// so it only scales when the victim is not a roster player. Players go through
// their own hook further down, with their own factor and direction.
static void EnemyDamageScaling_Hook(safetyhook::Context& ctx)
{
    if (!cfg::fourPlayerDifficultyScaling ||
        cfg::fourPlayerEnemyDamageTakenPercent == 100)
        return;
    if (cfg::fourPlayerDifficultyHostOnly && LocalIsHost() != 1)
        return;

    const uintptr_t victim = ctx.esi;
    const uintptr_t damage = ctx.edi;
    if (!victim || !damage)
        return;
    if (RosterSlot(victim) >= 0)
        return;                     // a player, not handled here

    const unsigned players = LivePlayerCountForScaling();
    const unsigned percent = DifficultyPercent(
        players, cfg::fourPlayerEnemyDamageTakenPercent,
        cfg::fourPlayerDifficultyPerPlayer, Direction::Enemy);

    const float before = Readable(damage + kDamageAmount, sizeof(float))
        ? mem::Read<float>(damage + kDamageAmount) : 0.0f;
    if (!ScaleDamageAmount(damage, percent))
        return;

    if (InterlockedIncrement(&gEnemyScaled) <= 20)
        Log("EnemyDifficulty: enemy damage %.2f -> %.2f (%u%%, %u players, %s)",
            before, mem::Read<float>(damage + kDamageAmount), percent, players,
            RoleName(LocalIsHost()));
}

// 0x00447E50, Player::ApplyDamageModifiers. Same argument shape: ECX = the
// Player, first stack argument = DamageData. Logging the amount here and in
// the pipeline above shows what the stock modifier chain does to it, which is
// where our own factor would have to slot in.
static void PlayerModifiers_Hook(safetyhook::Context& ctx)
{
    PlayerDamageScaling_Hook(ctx);

    if (!cfg::fourPlayerScalingDiagnostics)
        return;

    const long line = InterlockedIncrement(&gPlayerLines);
    if (line > 60)
        return;

    if (!Readable(ctx.esp + 4, sizeof(uintptr_t)))
        return;
    const uintptr_t damage = mem::Read<uintptr_t>(ctx.esp + 4);
    if (!damage || !Readable(damage + kDamageType, sizeof(uint32_t)))
        return;

    Log("EnemyDifficulty: [%ld] Player::ApplyDamageModifiers on this machine (%s): "
        "player=%08X (roster slot %d); amount BEFORE stock modifiers=%.3f typeFlags=%08X",
        line, RoleName(LocalIsHost()), static_cast<unsigned>(ctx.ecx),
        RosterSlot(ctx.ecx), mem::Read<float>(damage + kDamageAmount),
        mem::Read<unsigned>(damage + kDamageType));
}

// Player damage. Separate hook on Player::ApplyDamageModifiers because the
// direction is inverted here: with more players each one should take more.
// The factor sits ahead of the game's own multiplier chain, so it is just
// another link in it rather than a special case.
static void PlayerDamageScaling_Hook(safetyhook::Context& ctx)
{
    if (!cfg::fourPlayerDifficultyScaling ||
        cfg::fourPlayerPlayerDamageTakenPercent == 100)
        return;
    if (cfg::fourPlayerDifficultyHostOnly && LocalIsHost() != 1)
        return;
    if (!Readable(ctx.esp + 4, sizeof(uintptr_t)))
        return;

    const uintptr_t damage = mem::Read<uintptr_t>(ctx.esp + 4);
    if (!damage)
        return;

    const unsigned players = LivePlayerCountForScaling();
    const unsigned percent = DifficultyPercent(
        players, cfg::fourPlayerPlayerDamageTakenPercent,
        cfg::fourPlayerDifficultyPerPlayer, Direction::Player);

    const float before = Readable(damage + kDamageAmount, sizeof(float))
        ? mem::Read<float>(damage + kDamageAmount) : 0.0f;
    if (!ScaleDamageAmount(damage, percent))
        return;

    if (InterlockedIncrement(&gPlayerScaled) <= 20)
        Log("EnemyDifficulty: player damage %.2f -> %.2f (%u%%, %u players, "
            "roster slot %d, %s)",
            before, mem::Read<float>(damage + kDamageAmount), percent, players,
            RosterSlot(ctx.ecx), RoleName(LocalIsHost()));
}

// ---------------------------------------------------------------------------
// Is the AI Director present in retail, and does the campaign use it?
//
// Checked 2026-08-16 after the suspicion that it might have been cut from the
// retail build. It was not. The evidence, in order:
//
//   1. The class-name strings are all in the PC binary:
//        0x00DA90D8 AIDirectorEncounter    0x00DA90EC AIDirectorSettings
//        0x00DA9100 AIDirectorSpawnPoint   0x00DA9118 AIDirectorSpawnPointList
//        0x00DA9010 AIDirectorRosterBase   + five more Roster* classes
//
//   2. Each has a real registration block, same shape as any other class:
//        push <descriptor ctor> / push flags / push size / push name
//        mov ecx, <descriptor> / call 0x00A6EAE0
//
//   3. Their code references match a class known to be live one for one. Every
//      AIDirector class has FOUR references: own code, registration sweep and
//      two tables, which is exactly what Elevator (0x01220020) and
//      RevolvingDoor (0x01222468) have. Nothing is stripped or folded away.
//
//   4. Read out of live memory, the descriptors are actually REGISTERED, with
//      the same descriptor vtable as Elevator and a working factory:
//
//        Elevator             vtable 00E26EDC   factory 0x00424900
//        AIDirectorSpawnPoint vtable 00E26EDC   factory 0x0041F3E0
//        AIDirectorEncounter  vtable 00E26EDC   factory 0x0041F380
//        AIDirectorSettings   vtable 00E26EDC   factory 0x0041F190
//
// A dead end worth writing down so nobody repeats it: reading those same
// descriptors out of a MINIDUMP shows all zeroes, but so does Elevator's,
// and Elevator demonstrably works. The dumps simply do not cover that range.
// The zeroes were an artefact of the instrument, not a finding.
//
// So the system ships. What is still open is whether the CAMPAIGN uses it, and
// that cannot be answered statically: level data references classes by index,
// not by name, so the unpacked assets cannot be searched for it either (a
// control search for "Elevator" and "Player" finds nothing at all).
//
// Hence these three probes. They sit on the object factories, so:
//
//   * SpawnPoint firing  -> the loaded level actually places Director spawn
//                           points, i.e. there is data for it to work with
//   * Settings firing    -> the level brings its own Director tuning
//   * Encounter firing   -> the Director is genuinely RUNNING and starting
//                           encounters, which is the interesting one
//
// If none of them fires during a normal chapter, the campaign does not use the
// Director and "more enemies" has to come from somewhere else. That would
// match the observation that DS3's spawns feel fixed and repeat run to run.
//
// The three factories are template instantiations and share their first 29
// bytes, so each signature has to reach past them into the object-size push
// (0x7C / 0x58 / 0xC0) to stay unique. Verified: exactly one match each.
static safetyhook::MidHook gDirectorSpawnPoint;
static safetyhook::MidHook gDirectorSettings;
static safetyhook::MidHook gDirectorEncounter;
// This sits immediately after the Settings constructor returns from its
// factory. EAX is the fully constructed Settings object at that instruction.
// It is deliberately a separate read-only probe: the factory-entry hook only
// tells us that a Settings object exists, not which authored values it got.
static safetyhook::MidHook gDirectorSettingsReady;
// The Director itself, caught where it reads its own numbers. Measured on
// 26.08.2026: patching only the settings objects we see being constructed is
// not enough. On the first run through a level the Director is already
// holding one we never saw, and only after a checkpoint restart does it pick
// up a freshly built (and therefore scaled) one. That is exactly the "works
// only after a restart" behaviour. Reading the object out of ECX here gets us
// the packet actually in use.
static safetyhook::MidHook gDirectorInUse;
static safetyhook::MidHook gEncounterReady;

// maxPopulation is a points budget, not a headcount. EnemyRecord carries a
// populationScore next to its difficultyScore, and AdjustCurrentPopulation
// sums those into m_currentPopulation as enemies come and go; the Director
// keeps spawning while that sum stays under the setting. A necromorph costs
// several points, so a budget of 16 is roughly four or five of them alive at
// once, not sixteen.
//
// The 16-entry StaticArray<EnemyRecord> in AIDirector is a different thing:
// that is the pool of enemy types ChooseEnemy picks from, and nothing indexes
// it with the population budget. An earlier version of this file capped the
// budget at 16 for that reason, which was a misreading. The cap that remains
// is a sanity limit against a runaway INI, matching the quota cap.
constexpr unsigned kDirectorPopulationCeiling = 64;
constexpr uintptr_t kDirectorSettingsVtableRva = 0x009A29B0;

// AIDirectorSettings::Attributes, straight out of the PDB:
//   0x00 maxPopulation        0x04 encMinExcludeTime   0x08 encMaxExcludeTime
//   0x0C encMinDurationTime   0x10 encMaxDurationTime
//   0x14 minTimeBetweenSpawn  0x18 maxTimeBetweenSpawn
//   0x1C maxDiffScoreArr[5]   one ceiling per difficulty level
//
// maxDiffScoreArr is a second budget running alongside the population, and it
// is the one that was actually binding: measured on 27.08.2026 the Director
// sat at five enemies while maxPopulation said 32, with the population and
// difficulty counters climbing in lockstep. Scaling one without the other
// achieves nothing.
constexpr uintptr_t kSettingsDiffScores = 0x1C;
constexpr unsigned kDiffScoreCount = 4;   // four on PC, measured

struct DirectorSettingsBaseline
{
    uintptr_t settings = 0;
    uintptr_t attributes = 0;
    unsigned population = 0;
    unsigned diffScores[kDiffScoreCount]{};
    unsigned minimum = 0;
    unsigned maximum = 0;
    unsigned appliedPercent = 0;
};

static DirectorSettingsBaseline gDirectorSettingsBaselines[4]{};

static long gDirectorLines[3] = {0, 0, 0};
static const char* const kDirectorNames[3] = {
    "AIDirectorSpawnPoint", "AIDirectorSettings", "AIDirectorEncounter"
};

static void DirectorSeen(unsigned which)
{
    const long line = InterlockedIncrement(&gDirectorLines[which]);
    // The first one is the answer; a few more show whether it is one object or
    // a whole level full of them. After that it would only spam the log.
    if (line > 8)
        return;

    Log("EnemyDifficulty: AI DIRECTOR IS IN USE - %s created (#%ld on this "
        "machine, %s)", kDirectorNames[which], line, RoleName(LocalIsHost()));
}

static void DirectorSpawnPoint_Hook(safetyhook::Context&) { DirectorSeen(0); }
static void DirectorSettings_Hook(safetyhook::Context&)   { DirectorSeen(1); }
static void DirectorEncounter_Hook(safetyhook::Context&)  { DirectorSeen(2); }

static unsigned ScaleDirectorValue(unsigned baseline, unsigned percent,
                                   unsigned ceiling)
{
    unsigned long long scaled =
        (static_cast<unsigned long long>(baseline) * percent + 50ull) / 100ull;
    if (baseline && !scaled)
        scaled = 1;
    if (scaled > ceiling)
        scaled = ceiling;
    return static_cast<unsigned>(scaled);
}

static unsigned ShortenDirectorInterval(unsigned baseline, unsigned percent)
{
    // Nearest-integer millisecond rounding. A non-zero authored interval must
    // never turn into zero, even with a future player-cap experiment.
    unsigned long long shortened =
        (static_cast<unsigned long long>(baseline) * 100ull + percent / 2u) / percent;
    return baseline && !shortened ? 1u : static_cast<unsigned>(shortened);
}

static DirectorSettingsBaseline* RememberDirectorSettings(uintptr_t settings,
                                                           uintptr_t attributes)
{
    // Key on the attribute block, not on the pair with the settings object:
    // a checkpoint restart gives the settings a new address while the
    // attributes survive, still holding the values we scaled last time.
    // Otherwise our own output gets remembered as the baseline and doubled
    // again on the next pass.
    for (DirectorSettingsBaseline& state : gDirectorSettingsBaselines)
    {
        if (state.attributes == attributes)
        {
            state.settings = settings;
            return &state;
        }
    }

    DirectorSettingsBaseline* slot = nullptr;
    for (DirectorSettingsBaseline& state : gDirectorSettingsBaselines)
    {
        if (!state.settings)
        {
            slot = &state;
            break;
        }
    }
    if (!slot)
        return nullptr;

    slot->settings = settings;
    slot->attributes = attributes;
    slot->population = mem::Read<unsigned>(attributes + 0x00);
    for (unsigned i = 0; i < kDiffScoreCount; ++i)
        slot->diffScores[i] = mem::Read<unsigned>(
            attributes + kSettingsDiffScores + i * sizeof(unsigned));
    slot->minimum = mem::Read<unsigned>(attributes + 0x14);
    slot->maximum = mem::Read<unsigned>(attributes + 0x18);
    slot->appliedPercent = 0;
    return slot;
}

// this = ECX. Layout read off the retail code at 0x00741AD0:
//   [this+0x1C] m_currentPopulation, [this+0x20] m_currentDifficulty,
//   [this+0x24] SafePtr to the settings, pointing 0x10 past the object.
static void DirectorInUse_Hook(safetyhook::Context& ctx)
{
    if (!cfg::fourPlayerDynamicDirectorScaling || !ctx.ecx)
        return;
    if (!Readable(ctx.ecx + 0x24, sizeof(uintptr_t)))
        return;
    const uintptr_t handle = mem::Read<uintptr_t>(ctx.ecx + 0x24);
    if (!handle || handle < 0x10)
        return;
    const uintptr_t settings = handle - 0x10;
    if (!Readable(settings + 0x0C, sizeof(uintptr_t)))
        return;
    const uintptr_t attributes = mem::Read<uintptr_t>(settings + 0x0C);
    if (!attributes || !Readable(attributes, 0x1C))
        return;
    // The Director runs this constantly, which makes it the right place to
    // make sure every encounter's quota is scaled before any of them starts.
    ScaleKnownEncounterQuotas(LivePlayerCountForScaling());

    // Deliberately read-only. Writing from here was a regression: this runs on
    // every Director pass, and it also hands over settings objects we never
    // saw being constructed, so their current contents, our own output
    // included, got cached as the baseline and scaled again on top. In game
    // that stopped the spawning after the first wave.
    (void)settings; (void)attributes;

    // Measured by diffing the object before and after a checkpoint restart:
    // on the first run through a level [this+0x444] is null and the routine at
    // 0x00741B0F skips its whole second half, so the Director works without a
    // current encounter: no roster, no quota, and a quarantine door that
    // waits for something that never started. After a restart the field is
    // set. Report the transition once so the log says which of the two states
    // a session is actually in.
    static uintptr_t lastEncounter = ~uintptr_t(0);
    static long encounterLines = 0;
    if (Readable(ctx.ecx + 0x450, sizeof(uintptr_t)))
    {
        const uintptr_t encounter = mem::Read<uintptr_t>(ctx.ecx + 0x444);
        if (encounter != lastEncounter && encounterLines < 24)
        {
            ++encounterLines;
            lastEncounter = encounter;
            // Also say which attribute packet that encounter runs on and
            // what wave size it carries. Scaling every packet we can find is
            // worth nothing if the one actually in play is a different object.
            const uintptr_t object = encounter > 0x10 ? encounter - 0x10 : 0;
            if (object)
                ScaleEncounterNow(object, LivePlayerCountForScaling());
            const uintptr_t packet =
                object && Readable(object + 0x0C, sizeof(uintptr_t))
                    ? mem::Read<uintptr_t>(object + 0x0C) : 0;
            Log("EnemyDifficulty: Director %08X current encounter %08X %s "
                "(flag %u, population %u, spawned %u) packet %08X "
                "totalToSpawn %d",
                static_cast<unsigned>(ctx.ecx), static_cast<unsigned>(object),
                encounter ? "SET" : "NULL - running without an encounter",
                mem::Read<unsigned>(ctx.ecx + 0x450),
                mem::Read<unsigned>(ctx.ecx + 0x1C),
                mem::Read<unsigned>(ctx.ecx + 0x44C),
                static_cast<unsigned>(packet),
                packet && Readable(packet, 4)
                    ? static_cast<int>(mem::Read<unsigned>(packet)) : -1);
        }
    }
}

// Post-constructor of the AIDirectorEncounter factory, 0x40 bytes in, EAX
// holding the finished object. It has to be installed from here: the entry
// probe above already rewrote the factory's first bytes, so searching for the
// signature a second time from another translation unit finds nothing.
static void EncounterReady_Hook(safetyhook::Context& ctx)
{
    NoteDirectorEncounter(ctx.eax);
    // Encounters arrive in batches at level load, so each new one is a chance
    // to catch the rosters of those built moments earlier.
    ScaleKnownEncounterQuotas(LivePlayerCountForScaling());
}

static void DirectorSettingsReady_Hook(safetyhook::Context& ctx)
{
    static long lines = 0;
    const long line = InterlockedIncrement(&lines);
    const bool report = line <= 8;

    // AttributeHandler's packet pointer is at +0x0C on the retail object
    // family (also independently proven for AIDirectorRosterQuota). The PDB
    // gives the packet's Settings fields: maxPopulation +0x00,
    // minTimeBetweenSpawn +0x14, maxTimeBetweenSpawn +0x18. All are uint32
    // milliseconds, not floats.
    const uintptr_t settings = ctx.eax;
    if (!settings || !Readable(settings + 0x0C, sizeof(uintptr_t)))
    {
        if (report)
            Log("EnemyDifficulty: AIDirectorSettings post-ctor #%ld: invalid "
                "object %08X", line, static_cast<unsigned>(settings));
        return;
    }

    const uintptr_t attributes = mem::Read<uintptr_t>(settings + 0x0C);
    if (!attributes || !Readable(attributes, 0x1C))
    {
        if (report)
            Log("EnemyDifficulty: AIDirectorSettings post-ctor #%ld: "
                "object=%08X attributes=%08X (packet not readable yet)", line,
                static_cast<unsigned>(settings), static_cast<unsigned>(attributes));
        return;
    }

    DirectorSettingsBaseline* state = RememberDirectorSettings(settings, attributes);
    const unsigned population = mem::Read<unsigned>(attributes + 0x00);
    const unsigned minimum    = mem::Read<unsigned>(attributes + 0x14);
    const unsigned maximum    = mem::Read<unsigned>(attributes + 0x18);
    if (report)
        Log("EnemyDifficulty: AIDirectorSettings live #%ld: object=%08X "
            "attributes=%08X maxPopulation=%u minSpawnMs=%u maxSpawnMs=%u "
            "baseline=%s", line, static_cast<unsigned>(settings),
            static_cast<unsigned>(attributes), population, minimum, maximum,
            state ? "cached" : "not cached");

    if (!state && report)
        Log("EnemyDifficulty: AIDirectorSettings baseline cache is full; "
            "dynamic scaling will ignore this extra Settings object");

    // Apply as soon as the authoritative settings object exists. Relying only
    // on the quota reader misses Director encounters which configure their
    // population/timers before the first roster query.
    if (state && cfg::fourPlayerDynamicDirectorScaling)
        ApplyDynamicDirectorSettings(LivePlayerCountForScaling());
    ScaleKnownEncounterQuotas(LivePlayerCountForScaling());
}

} // namespace

void ApplyDynamicDirectorSettings(unsigned playerCount)
{
    if (!cfg::fourPlayerDynamicDirectorScaling || !g.module)
        return;

    // A client may construct a replica of the same settings object. In solo
    // there is no reliable online-role object, so permit the one-player case;
    // in co-op only a positively identified non-client may write it.
    if (playerCount > 1 && LocalIsHost() == 0)
        return;

    const unsigned percent = MoreEnemiesPercentForPlayers(playerCount);
    for (DirectorSettingsBaseline& state : gDirectorSettingsBaselines)
    {
        // Deliberately no "already applied" shortcut. Measured on 26.08.2026:
        // the Director keeps its population at the authored baseline even
        // after the packet was written, so the game puts the level's own
        // values back when the encounter starts. Writing once and then
        // trusting appliedPercent meant losing every time without noticing.
        // Now the packet is checked against what it should hold and corrected
        // whenever it drifted back.
        if (!state.settings || !state.attributes)
            continue;
        if (!Readable(state.settings + 0x0C, sizeof(uintptr_t)) ||
            mem::Read<uintptr_t>(state.settings) !=
                reinterpret_cast<uintptr_t>(g.module) + kDirectorSettingsVtableRva ||
            mem::Read<uintptr_t>(state.settings + 0x0C) != state.attributes ||
            !Readable(state.attributes, 0x1C))
        {
            state = {};
            continue;
        }

        const unsigned population = ScaleDirectorValue(
            state.population, percent, kDirectorPopulationCeiling);
        // maxDiffScoreArr is deliberately left alone. Writing it crashed the
        // game on every checkpoint restart: the array holds four entries on
        // PC, not the five the Xbox structure suggests, so the fifth write
        // landed in the message id behind it. The game later looked that id up
        // in a registry, found nothing and dereferenced null at 0x00A7D4F8.
        // The baseline is still recorded so the log can show the real values.
        const unsigned minimum = ShortenDirectorInterval(state.minimum, percent);
        const unsigned maximum = ShortenDirectorInterval(state.maximum, percent);
        if (mem::Read<unsigned>(state.attributes + 0x00) == population &&
            mem::Read<unsigned>(state.attributes + 0x14) == minimum &&
            mem::Read<unsigned>(state.attributes + 0x18) == maximum)
        {
            state.appliedPercent = percent;
            continue;
        }
        if (!mem::Write<unsigned>(state.attributes + 0x00, population) ||
            !mem::Write<unsigned>(state.attributes + 0x14, minimum) ||
            !mem::Write<unsigned>(state.attributes + 0x18, maximum))
        {
            Log("EnemyDifficulty: dynamic Director settings write failed at %08X",
                static_cast<unsigned>(state.attributes));
            continue;
        }

        const bool erstesMal = state.appliedPercent != percent;
        state.appliedPercent = percent;
        if (!erstesMal)
            continue;               // only re-applied, not worth a line
        // Call the number what it is. Reading "population=32" and then counting
        // eight necromorphs in the room looks like a broken patch until you
        // know the value is a points budget.
        Log("EnemyDifficulty: dynamic Director host scaling players=%u factor=%u%% "
            "populationBudget=%u/%u intervals=%u-%u ms "
            "(baseline %u, %u-%u) diffScores %u/%u/%u/%u",
            playerCount, percent, population, kDirectorPopulationCeiling,
            minimum, maximum, state.population, state.minimum, state.maximum,
            mem::Read<unsigned>(state.attributes + kSettingsDiffScores + 0),
            mem::Read<unsigned>(state.attributes + kSettingsDiffScores + 4),
            mem::Read<unsigned>(state.attributes + kSettingsDiffScores + 8),
            mem::Read<unsigned>(state.attributes + kSettingsDiffScores + 12));
    }
}

void ApplyFourPlayerScalingDiagnostics()
{
    // Install this for the learned-class list too: the damage path is the only
    // place an enemy class is ever confirmed. Without it the patch learns
    // nothing, even with diagnostics and scaling switched off.
    const bool needDamagePipeline = cfg::fourPlayerScalingDiagnostics ||
                                    cfg::fourPlayerDifficultyScaling ||
                                    cfg::fourPlayerRuntimeSpawnClass ||
                                    cfg::fourPlayerDamageMemoryProbe;
    if (needDamagePipeline)
    {

    // 0x006C0FC0, the shared damage path. Pinned on the prologue plus the two
    // loads that prove the argument layout, so a same-prologue neighbour cannot
    // match: mov ebx,[ebp+8] / mov eax,[ebx] / push esi / push edi / mov esi,ecx.
    // 0x006C1345, right at the virtual call rather than the function entry.
    // Pinned on the call itself; exactly one match in the binary.
    const DWORD pipeline = Find(
        "8B 16 8B 82 60 02 00 00 57 8B CE FF D0",
        "FourPlayerScaling damage call site");
    if (pipeline)
        gDamagePipeline = safetyhook::create_mid(reinterpret_cast<void*>(pipeline),
                                                 &DamagePipeline_Hook);

    // 0x00447E50, Player::ApplyDamageModifiers, PC vtable slot +0x260.
    // Pinned through the first multiplier, which is what identifies it.
    if (cfg::fourPlayerScalingDiagnostics || cfg::fourPlayerDifficultyScaling)
    {
        const DWORD playerModifiers = Find(
            "55 8B EC 56 8B 75 08 57 56 8B F9 E8 ?? ?? ?? ?? "
            "F7 46 54 00 00 04 04",
            "FourPlayerScaling Player::ApplyDamageModifiers");
        if (playerModifiers)
            gPlayerModifiers = safetyhook::create_mid(reinterpret_cast<void*>(playerModifiers),
                                                      &PlayerModifiers_Hook);
    }

    Log("EnemyDifficulty: damage pipeline %s, player modifiers %s "
        "(diagnostics=%d, runtimeLearning=%d, memoryProbe=%d)",
        gDamagePipeline ? "hooked" : "NOT installed",
        gPlayerModifiers ? "hooked" : "NOT installed",
        (int)cfg::fourPlayerScalingDiagnostics,
        (int)cfg::fourPlayerRuntimeSpawnClass,
        (int)cfg::fourPlayerDamageMemoryProbe);

    if (cfg::fourPlayerDifficultyScaling)
    {
        Log("EnemyDifficulty: active%s. Enemy damage base %u%%, "
            "player damage base %u%%, %u points per player from the third on, "
            "Grenzen %u..%u%%",
            cfg::fourPlayerDifficultyHostOnly ? " (host only)" : " (EVERY machine)",
            cfg::fourPlayerEnemyDamageTakenPercent,
            cfg::fourPlayerPlayerDamageTakenPercent,
            cfg::fourPlayerDifficultyPerPlayer,
            kDifficultyMinPercent, kDifficultyMaxPercent);
        if (!cfg::fourPlayerDifficultyHostOnly)
            Log("EnemyDifficulty: WARNING - HostOnly is off. Until it has been "
                "measured which machine actually runs the damage path, both ends "
                "of a session may scale the same hit, which doubles the factor.");
    }
    else
    {
        Log("EnemyDifficulty: off, no damage value is touched.");
    }
    }

    struct DirectorProbe { const char* sig; const char* name; safetyhook::MidHookFn hook;
                           safetyhook::MidHook* slot; };
    const DirectorProbe probes[3] = {
        { "55 8B EC 6A FF 68 C1 DE CC 00 64 A1 00 00 00 00 50 64 89 25 "
          "00 00 00 00 51 8B 45 0C 50 68 C0 00 00 00 E8 59 5D 68 00",
          "FourPlayerScaling AIDirectorSpawnPoint factory",
          &DirectorSpawnPoint_Hook, &gDirectorSpawnPoint },
        { "55 8B EC 6A FF 68 C1 DE CC 00 64 A1 00 00 00 00 50 64 89 25 "
          "00 00 00 00 51 8B 45 0C 50 6A 58 E8 AC 5F 68 00",
          "FourPlayerScaling AIDirectorSettings factory",
          &DirectorSettings_Hook, &gDirectorSettings },
        { "55 8B EC 6A FF 68 C1 DE CC 00 64 A1 00 00 00 00 50 64 89 25 "
          "00 00 00 00 51 8B 45 0C 50 6A 7C E8 BC 5D 68 00",
          "FourPlayerScaling AIDirectorEncounter factory",
          &DirectorEncounter_Hook, &gDirectorEncounter },
    };

    unsigned directorInstalled = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        const DWORD site = Find(probes[i].sig, probes[i].name);
        if (!site)
            continue;
        *probes[i].slot = safetyhook::create_mid(reinterpret_cast<void*>(site), probes[i].hook);
        if (*probes[i].slot)
            ++directorInstalled;

        // 0x0041F1D0 for the retail Settings factory: directly after its
        // `call AIDirectorSettings::AIDirectorSettings`. This offset is safe
        // only relative to the unique factory signature above; do not scan the
        // function a second time after the entry hook has changed its prologue.
        if (i == 2)
        {
            gEncounterReady = safetyhook::create_mid(
                reinterpret_cast<void*>(site + 0x40), &EncounterReady_Hook);
            Log("EnemyDifficulty: encounter roster probe %s at %08X",
                gEncounterReady ? "active" : "NOT installed", site + 0x40);
        }
        if (i == 1)
        {
            const DWORD postConstructor = site + 0x40;
            gDirectorSettingsReady = safetyhook::create_mid(
                reinterpret_cast<void*>(postConstructor), &DirectorSettingsReady_Hook);
            Log("EnemyDifficulty: AIDirectorSettings live-value probe %s at %08X",
                gDirectorSettingsReady ? "traced" : "NOT installed", postConstructor);
        }
    }

    // AIDirector, caught in the routine that works out how much room it still
    // has: it loads its settings through the SafePtr at +0x24 and subtracts
    // its own counters. Unique enough on its own: the 0x24 handle load, the
    // -0x10 SafePtr adjustment and the difficulty-table read sit together
    // nowhere else.
    if (const DWORD inUse = Find(
            "55 8B EC 53 56 8B F1 8B 4E 24 57 33 C0 33 FF 33 DB 85 C9 74 ?? "
            "83 C1 F0 74 ?? 8B 49 0C 8B 15 ?? ?? ?? ?? 8B 52 08 8B 39 8B 5C 91 1C",
            "EnemyDifficulty AIDirector in-use settings"))
    {
        gDirectorInUse = safetyhook::create_mid(
            reinterpret_cast<void*>(inUse), &DirectorInUse_Hook);
        Log("EnemyDifficulty: Director in-use settings probe %s at %08X",
            gDirectorInUse ? "active" : "NOT installed", inUse);
    }

    Log("EnemyDifficulty: AI Director probes %u of 3 installed; the classes ARE "
        "registered in retail, the open question is only whether the campaign uses them",
        directorInstalled);
}
