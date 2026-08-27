// Enemy spawns: measure first, then turn the dial.
//
// DS3 spawns along two paths. The scripted one carries the bulk of the
// campaign: `CMD_SetSpawnTarget` wires a spawner to exactly one authored
// actor, and `CMD_SetMaxSpawn` occurs 66 times across the whole corpus and is
// always 1. The AI Director runs the waves in arena sections (mass driver,
// armory, disposal, basement, drill room), where it works through an authored
// roster whose `m_npcTargetSpawnCount` goes up to 64.
//
// The old SimGroup blob hook at 0x00A766A0 is deliberately not installed any
// more; the retail load path never reaches it. This module watches the
// finished AI Director objects in the process instead, which needs no edited
// level files and makes the diagnostics say something about the encounter
// that is actually running.

#include "Common.hpp"

namespace
{
static bool Readable(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;

    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= end && size <= end - address;
}

// 0x00A766A0 was a dead end. Written down so nobody repeats it.
//
// The function checks the SimGroup signature at blob+0x24 and the version
// 0x00010003 at +0x28, exactly as our decompiler describes, and it is the only
// place in the entire binary that knows the constant 0xCCB51828. It looked
// like the parser. It is not: across a full session with level loads the hook
// never fired once, neither a hit nor its own built-in warning. Its two
// handlers 0x00A72070 and 0x00A73BA0 are only ever called by it, and its sole
// caller 0x00A77230 has no direct callers at all. The whole branch is dead on
// the retail load path; the game reads level data elsewhere and does not
// validate the signature while doing so.
//
// So instead of hunting for the reader, this module measures the result. The
// AI Director builds roster objects out of the level data, and their factories
// are known from the class descriptors in memory, whose m_factoryFN sits at
// +0x18 per the PDB:
//
//     AIDirectorRoster       descriptor 0x01229A40   factory 0x0041F240
//     AIDirectorRosterQuota  descriptor 0x01229CC0   factory 0x0041F2A0
//
// If one of them fires, the loaded level has a Director roster and therefore
// the spawn counts we care about. If neither fires, the section does not use
// the Director, which is a real finding rather than a measurement error.
static safetyhook::MidHook gRosterFactory;
static safetyhook::MidHook gRosterQuotaFactory;
static safetyhook::MidHook gRosterQuotaUse;
static safetyhook::MidHook gRosterQuotaReady;

// Every AIDirectorEncounter the level builds. Measured 26.08.2026: the quota
// objects are NOT created through the factory in the class descriptor, so the
// post-constructor probe there never fires and the only thing left patching
// them was the first-read fallback, which runs after the encounter has
// already read its target. That is the whole "scaling only works after a
// checkpoint restart" story: the restart simply inherits the values the
// previous run wrote too late.
//
// The encounters do come from their factory, and each one keeps a SafePtr to
// its quota at +0x70 (verified against three encounter/quota pairs from the
// log). Remembering them at load lets us scale every quota up front.
constexpr uintptr_t kEncounterRoster = 0x70;

// AIDirectorEncounter::Attributes from the PDB:
//   0x00 totalToSpawn   0x04 ventGroup   0x08 roster (guid128)
//   0x18 timeout        0x1C maxVentDistanceSq   then the messages
//
// totalToSpawn is how many enemies one wave puts out, and it is the number
// that was actually holding every quarantine at four or five. The roster
// quota we spent the night raising only caps how many of each *type* may
// appear; it never decided the size of a wave.
// A checkpoint restart frees every encounter and builds new ones, so the
// remembered pointers go stale. Freed heap often stays mapped and readable,
// which is how a stale entry turned into a crash on 27.08.2026: the walk
// happily wrote into memory that had already been handed to something else.
// Nothing is touched now unless the vtable still says AIDirectorEncounter.
constexpr uintptr_t kEncounterVtableRva = 0x009A43B4;

constexpr uintptr_t kEncounterAttributes = 0x0C;
constexpr uintptr_t kEncounterTotalToSpawn = 0x00;
constexpr unsigned kEncounterTotalCeiling = 64;

struct EncounterTotalBaseline
{
    uintptr_t attributes = 0;
    unsigned total = 0;
    unsigned appliedPercent = 0;
};
static EncounterTotalBaseline gEncounterTotals[32];


constexpr unsigned kMaxEncounters = 32;
static uintptr_t gEncounters[kMaxEncounters];
static unsigned gEncounterCount = 0;
static safetyhook::MidHook gFuncSpawnPrepare;
static safetyhook::MidHook gFuncSpawnGuidRestore;
static safetyhook::MidHook gFuncSpawnResult;
using FuncSpawnQueueRequestFn = void(SAFETYHOOK_THISCALL*)(void*);
static FuncSpawnQueueRequestFn gFuncSpawnQueueRequest = nullptr;
// SimManager::SpawnEntity, taken from the call FuncSpawn::HandleEvents makes.
// Argument order is read off that call site: push 0x2000, push &spawnInfo,
// ecx = the manager. So the packet is the first argument and the flag word
// the second, and the count comes from the pushes rather than from a guess.
using SimSpawnEntityFn = void*(SAFETYHOOK_THISCALL*)(void*, void*, unsigned);
static SimSpawnEntityFn gSimSpawnEntity = nullptr;
static long gBurstSpawned = 0;
static long gBurstLines = 0;
static long gRosterLines = 0;
static long gQuotaLines = 0;
static long gScriptedSpawnLines = 0;

// PC retail layout, independently confirmed in the method that consumes it:
//
//   AIDirectorRosterQuota::GetValidRosterEntries at 0x00742010:
//     attrs = [quota + 0x0C]
//     target[i] = [attrs + i * 0x34 + 0x10]
//     spawned[i] = [quota + i * 4 + 0x5C]
//
// The semantic names and the Xbox version of the structure come from the
// August `deadspace_r.pdb`; the PC offsets above are from the retail code
// itself. This is deliberately observation-only for now.
constexpr uintptr_t kQuotaAttributes       = 0x0C;
constexpr uintptr_t kQuotaValidEntries     = 0x58;
constexpr uintptr_t kQuotaSpawnCount       = 0x5C;
constexpr unsigned  kQuotaEntries          = 16;
constexpr size_t    kQuotaAttributeStride  = 0x34;
constexpr size_t    kQuotaTargetCount      = 0x10;
constexpr unsigned  kQuotaTargetCeiling    = 64;

// `FuncSpawn::HandleEvents` is the genuine authored spawn path. These two
// offsets are not inferred from the console PDB: the retail PC method itself
// reads `attrs + 0x1C`, compares `spawner + 0xC0`, and then resolves the GUID
// at `attrs + 0x0C` before it calls SimManager::SpawnEntity.
constexpr uintptr_t kFuncSpawnAttributes   = 0x0C;
constexpr uintptr_t kFuncSpawnSpawnedCount = 0xC0;
constexpr uintptr_t kFuncSpawnQueuedCount  = 0xC8;
constexpr uintptr_t kFuncSpawnRetryPending = 0xCC;
constexpr uintptr_t kFuncSpawnRetryMessage = 0xD0;
constexpr uintptr_t kFuncSpawnTargetGuid   = 0x0C;
constexpr uintptr_t kFuncSpawnMaxSpawn     = 0x1C;
constexpr uintptr_t kFuncSpawnGroup        = 0x2C;
constexpr uintptr_t kFuncSpawnOptions      = 0x30;
constexpr uintptr_t kFuncSpawnPopThreshold = 0x54;
constexpr unsigned  kFuncSpawnRetryOption  = 0x08;

// This is a compiled copy of the proven, non-human authored combat bindings
// produced by the existing SimGroup research. It is a filter only: a target
// absent from this list is never scaled. The source files are build-time data;
// the released ASI has no runtime file dependency on the trainer directory.
struct AuthoredCombatTarget
{
    unsigned requestId;
    const char* displayName;
    const char* resourcePath;
    uint32_t streamGuid;
    uint32_t sourceEntityCount;
    const char* dependencyPath;
    uint32_t dependencyGuid;
    uint32_t targetGuid[4];
    uint32_t templateGuid;
    uint32_t entityIndex;
    uint32_t attributePacket;
    uint32_t componentList;
    uint32_t componentCount;
    uint32_t spawnFlags;
};

static const AuthoredCombatTarget kAuthoredCombatTargets[] = {
#include "generated/ds3_authored_spawn_bindings_078.inc"
#include "generated/ds3_authored_necro_spawn_bindings_079.inc"
#include "generated/ds3_authored_necro_spawn_bindings_0710.inc"
#include "generated/ds3_authored_necro_spawn_bindings_0711.inc"
};

static const AuthoredCombatTarget* FindAuthoredCombatTarget(
    uint32_t guid0, uint32_t guid1, uint32_t guid2, uint32_t guid3)
{
    for (const AuthoredCombatTarget& target : kAuthoredCombatTargets)
    {
        if (target.targetGuid[0] == guid0 && target.targetGuid[1] == guid1 &&
            target.targetGuid[2] == guid2 && target.targetGuid[3] == guid3)
            return &target;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Spawn target classification from the full PC corpus.
//
// The list above (`kAuthoredCombatTargets`) knows 53 targets. The tracer sees
// over 400 distinct ones, and a good share of those are not enemies at all:
// props, triggers, boss stage pieces, and `spawn_carver_rosetta`, which is
// Carver himself. All of it goes through the same FuncSpawn path. Without a
// distinction, scaling would produce several Carvers, rappel hooks and
// leviathan tentacles.
//
// Built with trainer/spawn_edges.py: a spawn target GUID is an entity's
// uid128. Across the 1786 unpacked .str files the entities were indexed, then
// every `CMD_SetSpawnTarget` edge resolved and the target entity checked for
// combat attributes (pain threshold, ranged attack, dodging, charge). That
// needs neither an actor catalogue nor the Xbox PDB.
//
// Cross-check: every target the older index had named exactly and that also
// appears here comes out classified as an enemy: feeder, twitcher, super
// puker, super feeder, pregnant, with no misclassification.
//
// The limit worth knowing: only a fraction of the targets seen at runtime have
// a `CMD_SetSpawnTarget` edge in the level data at all. The rest come from the
// AI Director or from levels whose SimGroups are compressed. So this table is
// a blocklist, not full coverage: it reliably says no, and where it knows
// nothing it also says no. An unknown target is never scaled.
enum SpawnClass : uint8_t
{
    SPAWN_CLASS_COMBAT = 0,
    SPAWN_CLASS_NON_COMBAT,
    SPAWN_CLASS_SCRIPTED_STAGE,
    SPAWN_CLASS_UNRESOLVED,
};

struct SpawnTargetEntry
{
    uint32_t guid[4];
    uint8_t  klass;
    uint8_t  archetype;      // index into kSpawnArchetypes, 0xFF = none
};

static const SpawnTargetEntry kSpawnTargets[] = {
#include "generated/pc_spawn_targets.inc"
};

// The same list as in Common.hpp. One source, so the sliders and the table
// cannot drift apart again.
static const char* const* const kSpawnArchetypes = cfg::kSpawnTypeNames;
constexpr unsigned kSpawnArchetypeCount = cfg::kSpawnTypeCount;

static const SpawnTargetEntry* FindSpawnTarget(
    uint32_t guid0, uint32_t guid1, uint32_t guid2, uint32_t guid3)
{
    for (const SpawnTargetEntry& entry : kSpawnTargets)
    {
        if (entry.guid[0] == guid0 && entry.guid[1] == guid1 &&
            entry.guid[2] == guid2 && entry.guid[3] == guid3)
            return &entry;
    }
    return nullptr;
}

static const char* SpawnArchetypeName(uint8_t index);
static bool MayScaleSpawns();

// Offline-generated authored targets. They identify enemy definitions, but a
// definition is only usable while the current SimManager has it registered.
struct RandomizerTarget
{
    uint32_t guid[4];
    unsigned group;
    unsigned candidate;
    unsigned contextMask;
    unsigned compatibilityFamily;
};

static const RandomizerTarget kRandomizerTargets[] = {
#include "generated/pc_enemy_randomizer_pools.inc"
};

static const RandomizerTarget* FindRandomizerTarget(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    for (const RandomizerTarget& target : kRandomizerTargets)
        if (target.guid[0] == a && target.guid[1] == b &&
            target.guid[2] == c && target.guid[3] == d)
            return &target;
    return nullptr;
}

static bool RandomizerIsHuman(const RandomizerTarget& target)
{
    const SpawnTargetEntry* known = FindSpawnTarget(target.guid[0], target.guid[1],
                                                     target.guid[2], target.guid[3]);
    return known && known->archetype < kSpawnArchetypeCount &&
           std::strncmp(SpawnArchetypeName(known->archetype), "human_", 6) == 0;
}

static uint32_t gRandomizerState = 0x4D534433u; // stable non-zero seed

static uint32_t NextRandomizerValue()
{
    uint32_t value = gRandomizerState;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return gRandomizerState = value ? value : 0x4D534433u;
}

static bool RandomizerHasSingleContext(const RandomizerTarget& target)
{
    return target.contextMask &&
           (target.contextMask & (target.contextMask - 1u)) == 0;
}

static const char* RandomizerContextName(unsigned mask)
{
    switch (mask)
    {
    case 0x01u: return "ground";
    case 0x02u: return "vent";
    case 0x04u: return "ceiling";
    case 0x08u: return "wall";
    case 0x10u: return "burst";
    case 0x20u: return "drop";
    default: return "mixed/unknown";
    }
}

// FuncSpawn targets that have already resolved in this session.  This is the
// essential runtime filter: an offline GUID may exist in a level archive but
// still be absent from the current streaming set.  Passing such a GUID to
// GetSpawnInfo yields an empty packet and silently drops the spawn.
static const RandomizerTarget* gLiveRandomizerTargets[256]{};
static unsigned gLiveRandomizerTargetCount = 0;

// A native FuncSpawn gives us stronger information than the offline corpus:
// its resolved SpawnInfo contains the actor's real component family. Retain a
// small per-process cache of such already-spawned, already-streamed targets.
// The production randomizer can use these directly; it never needs to load a
// foreign STR/Actor just to manufacture a possible replacement.
struct NativeRandomizerProfile
{
    const RandomizerTarget* target = nullptr;
    unsigned componentFamily = 0;
};
static NativeRandomizerProfile gNativeRandomizerProfiles[128]{};
static unsigned gNativeRandomizerProfileCount = 0;
static uintptr_t gNativeRandomizerProfileWorld = 0;

static void EnsureNativeRandomizerProfileWorld(uintptr_t simManager)
{
    if (simManager && simManager != gNativeRandomizerProfileWorld)
    {
        gNativeRandomizerProfileWorld = simManager;
        gNativeRandomizerProfileCount = 0;
        std::memset(gNativeRandomizerProfiles, 0,
                    sizeof(gNativeRandomizerProfiles));
    }
}

static unsigned RandomizerEffectiveFamily(const RandomizerTarget& target)
{
    if (target.compatibilityFamily)
        return target.compatibilityFamily;
    for (unsigned i = 0; i < gNativeRandomizerProfileCount; ++i)
        if (gNativeRandomizerProfiles[i].target == &target)
            return gNativeRandomizerProfiles[i].componentFamily;
    return 0;
}

static void RememberNativeRandomizerProfile(const RandomizerTarget& target,
                                            unsigned componentFamily)
{
    if (!target.candidate || !componentFamily ||
        !RandomizerHasSingleContext(target) ||
        (!cfg::enemyRandomizerAllowHumans && RandomizerIsHuman(target)))
        return;
    for (unsigned i = 0; i < gNativeRandomizerProfileCount; ++i)
    {
        if (gNativeRandomizerProfiles[i].target == &target)
        {
            gNativeRandomizerProfiles[i].componentFamily = componentFamily;
            return;
        }
    }
    if (gNativeRandomizerProfileCount < _countof(gNativeRandomizerProfiles))
    {
        NativeRandomizerProfile& profile =
            gNativeRandomizerProfiles[gNativeRandomizerProfileCount++];
        profile.target = &target;
        profile.componentFamily = componentFamily;
    }
}

// This is intentionally a one-actor experiment. The character STR contributes
// the Super Stalker model, rig and animation dependency; the small authored
// sidecar contributes a complete *upright Stalker* Actor blueprint whose
// model/model-stream fields were redirected to that retail Super Stalker
// package.  The only recovered native Super Stalker Actor was explicitly a
// "legless" crawl encounter, so it is not suitable for global replacements.
// A controller by itself can damage and dismember, but has no complete
// animated Actor contract.
static const RandomizerTarget kStreamedSuperStalkerController = {
    { 0x58CB2E99u, 0x9E5B5AB7u, 0x4E4F574Bu, 0x35373230u },
    0u, 0u, 0x01u, 0xCDE8738Du
};
static const RandomizerTarget kStreamedSuperStalker = {
    { 0x4ECDB2E0u, 0xF1A6C789u, 0x53315357u, 0x4952414Du },
    0u, 1u, 0x01u, 0xCDE8738Du
};
// Native full Actor recovered from a small, reviewed 28-entity mini-global.
// Unlike the first recovered Super Stalker actor, this is an ordinary upright
// combat actor rather than a scene-specific legless or scripted variant.
static const RandomizerTarget kStreamedSuperSlasherController = {
    { 0x30C86A97u, 0x87B5C449u, 0x4E484F4Au, 0x324E4F53u },
    0u, 0u, 0x01u, 0xAF2BAF1Du
};
static const RandomizerTarget kStreamedSuperSlasher = {
    { 0x45CDAD68u, 0x5BDBE848u, 0x4A315357u, 0x48475548u },
    0u, 1u, 0x01u, 0xAF2BAF1Du
};

enum StreamedTemplateState : LONG
{
    STREAMED_TEMPLATE_WAITING = 0,
    STREAMED_TEMPLATE_LOADING_CHARACTER,
    STREAMED_TEMPLATE_LOADING_ACTOR,
    STREAMED_TEMPLATE_READY,
    STREAMED_TEMPLATE_FAILED,
};

static volatile LONG gStreamedTemplateState = STREAMED_TEMPLATE_WAITING;
static volatile LONG gStreamedSuperSlasherState = STREAMED_TEMPLATE_WAITING;

static void RememberLiveRandomizerTarget(const RandomizerTarget& target)
{
    // A non-zero component family is the one compatibility fact we can prove
    // from the authored actor data on PC. The broad family-0 pool was useful
    // for discovery, but cannot establish matching NPC animation/vent
    // contracts and is deliberately outside the safe runtime picker.
    if (!target.candidate || !target.compatibilityFamily ||
        !RandomizerHasSingleContext(target) ||
        (!cfg::enemyRandomizerAllowHumans && RandomizerIsHuman(target)))
        return;
    for (unsigned i = 0; i < gLiveRandomizerTargetCount; ++i)
        if (gLiveRandomizerTargets[i] == &target)
            return;
    if (gLiveRandomizerTargetCount < _countof(gLiveRandomizerTargets))
        gLiveRandomizerTargets[gLiveRandomizerTargetCount++] = &target;
}

// Exact read-only equivalent of the lookup portion of
// SimManager::GetSpawnInfo (00A6F780 in this retail executable).  It lets us
// reject stale entries after a stream transition without calling into game
// code from the hook.
static bool RandomizerTargetIsLive(uintptr_t simManager,
                                   const RandomizerTarget& target)
{
    if (!simManager)
        return false;
    uint32_t hash = target.guid[0];
    hash = hash * 33u + target.guid[1];
    hash = hash * 33u + target.guid[2];
    hash = hash * 33u + target.guid[3];
    const uintptr_t bucket = simManager + 0x80u + ((hash & 0xFFFu) * sizeof(uintptr_t));
    if (!Readable(bucket, sizeof(uintptr_t)))
        return false;
    uintptr_t entry = mem::Read<uintptr_t>(bucket);
    for (unsigned hops = 0; entry && hops < 1024; ++hops)
    {
        if (!Readable(entry + 0x34, sizeof(uintptr_t)))
            return false;
        if (mem::Read<uint32_t>(entry + 0x1C) == hash)
            return true;
        entry = mem::Read<uintptr_t>(entry + 0x34);
    }
    return false;
}

static bool RandomizerIsOrdinarySlasherGround(const RandomizerTarget& target)
{
    if (target.contextMask != 0x01u)
        return false;
    const SpawnTargetEntry* known = FindSpawnTarget(target.guid[0], target.guid[1],
                                                     target.guid[2], target.guid[3]);
    if (!known || known->archetype >= kSpawnArchetypeCount)
        return false;
    const char* name = SpawnArchetypeName(known->archetype);
    // This covers the authored normal/frozen/berserk Slasher entries, but
    // deliberately excludes an existing Super Slasher and every other ground
    // family. Their animation and event contracts are not interchangeable.
    return std::strncmp(name, "slasher", 7) == 0 &&
           std::strcmp(name, "super_slasher") != 0;
}

static bool GlobalTemplateMatchesSource(const RandomizerTarget& source,
                                        const RandomizerTarget& candidate)
{
    if (!candidate.compatibilityFamily || !RandomizerHasSingleContext(source) ||
        !RandomizerHasSingleContext(candidate))
        return false;
    if (source.compatibilityFamily &&
        source.compatibilityFamily == candidate.compatibilityFamily)
        return true;

    // This deliberately is not a generic mixed-family pool. Some ordinary PC
    // Slasher spawns lack a recovered component-family tag, so the explicit
    // opt-in admits only those ground entries and only into Super Slasher.
    // Super Stalker remains native-Stalker-family-only. Vent/ceiling/mixed
    // contexts and every non-Slasher ground enemy keep the strict check.
    return cfg::enemyRandomizerCrossFamilyGround &&
        &candidate == &kStreamedSuperSlasher &&
        RandomizerIsOrdinarySlasherGround(source);
}

static bool RandomizerSourceIsEligible(const RandomizerTarget& source)
{
    if (!source.candidate || !RandomizerHasSingleContext(source) ||
        (!cfg::enemyRandomizerAllowHumans && RandomizerIsHuman(source)))
        return false;

    if (RandomizerEffectiveFamily(source))
        return true;

    // Family-zero records cannot participate in the ordinary live pool. The
    // only reviewed exception is an ordinary ground Slasher feeding the Super
    // Slasher template when the separate opt-in was enabled by the player.
    return cfg::enemyRandomizerCrossFamilyGround &&
        RandomizerIsOrdinarySlasherGround(source);
}

static const RandomizerTarget* PickRandomizerTarget(const RandomizerTarget& original,
                                                     uintptr_t simManager)
{
    const RandomizerTarget* choices[96]{};
    unsigned count = 0;
    const unsigned originalFamily = RandomizerEffectiveFamily(original);

    // Native profiles are the fast path: all their models, animations and
    // actor contracts are already registered by this exact level. A decision
    // therefore remains only the proven temporary 16-byte GUID redirect.
    if (originalFamily)
    {
        for (unsigned i = 0; i < gNativeRandomizerProfileCount; ++i)
        {
            const NativeRandomizerProfile& profile = gNativeRandomizerProfiles[i];
            const RandomizerTarget* candidate = profile.target;
            if (!candidate || candidate == &original ||
                profile.componentFamily != originalFamily ||
                !RandomizerHasSingleContext(original) ||
                !(candidate->contextMask & original.contextMask) ||
                !RandomizerTargetIsLive(simManager, *candidate))
                continue;
            if (count < _countof(choices))
                choices[count++] = candidate;
        }
    }
    for (unsigned i = 0; i < gLiveRandomizerTargetCount; ++i)
    {
        const RandomizerTarget* candidate = gLiveRandomizerTargets[i];
        if (!candidate || candidate == &original ||
            !original.compatibilityFamily || !candidate->compatibilityFamily ||
            !RandomizerHasSingleContext(original) ||
            !(candidate->contextMask & original.contextMask) ||
            original.compatibilityFamily != candidate->compatibilityFamily ||
            !RandomizerTargetIsLive(simManager, *candidate))
            continue;
        if (count < _countof(choices))
            choices[count++] = candidate;
    }
    // A globally streamed template is not observed as an authored FuncSpawn,
    // so it never enters gLiveRandomizerTargets. Keep this separate and use
    // the same live-hash proof as ordinary cache entries.
    if (cfg::enemyRandomizerGlobalTemplates &&
        InterlockedCompareExchange(&gStreamedTemplateState, 0, 0) ==
            STREAMED_TEMPLATE_READY &&
        GlobalTemplateMatchesSource(original, kStreamedSuperStalker) &&
        RandomizerTargetIsLive(simManager, kStreamedSuperStalker) &&
        count < _countof(choices))
    {
        choices[count++] = &kStreamedSuperStalker;
    }
    if (cfg::enemyRandomizerGlobalTemplates &&
        InterlockedCompareExchange(&gStreamedSuperSlasherState, 0, 0) ==
            STREAMED_TEMPLATE_READY &&
        GlobalTemplateMatchesSource(original, kStreamedSuperSlasher) &&
        RandomizerTargetIsLive(simManager, kStreamedSuperSlasher) &&
        count < _countof(choices))
    {
        choices[count++] = &kStreamedSuperSlasher;
    }
    return count ? choices[NextRandomizerValue() % count] : nullptr;
}

struct RandomizerPending
{
    uintptr_t attributes = 0;
    uint32_t original[4]{};
};
struct RandomizerAttempt
{
    uintptr_t attributes = 0;
    uint32_t original[4]{};
    uint32_t replacement[4]{};
    unsigned contextMask = 0;
};
static RandomizerPending gRandomizerPending;
static RandomizerAttempt gRandomizerAttempt;
static long gRandomizerLines = 0;
static long gRandomizerResultLines = 0;
static long gRandomizerDiagnosticLines = 0;
static long gRandomizerFuncSpawnSeen = 0;
static long gRandomizerUnknownSource = 0;
static long gRandomizerIneligibleSource = 0;
static long gRandomizerNoLiveChoice = 0;
static long gRandomizerAttempts = 0;
static long gRandomizerSpawned = 0;
static long gRandomizerRejected = 0;

static const char* RandomizerTargetName(const RandomizerTarget& target)
{
    if (&target == &kStreamedSuperStalker)
        return "super-stalker (streamed)";
    if (&target == &kStreamedSuperSlasher)
        return "super-slasher (streamed)";
    const SpawnTargetEntry* known = FindSpawnTarget(target.guid[0], target.guid[1],
                                                     target.guid[2], target.guid[3]);
    if (known)
        return SpawnArchetypeName(known->archetype);
    switch (target.compatibilityFamily)
    {
    case 0x06233BCCu: return "exploder-family";
    case 0x0D585E3Fu: return "puker-family";
    case 0x33A00EA8u: return "divider-head-family";
    case 0x3F6541B6u: return "lurker-family";
    case 0x4424CDDCu: return "leaper-family";
    case 0x5A23975Eu: return "crawler-family";
    case 0x7DA162ACu: return "feeder-family";
    case 0xAF2BAF1Du: return "slasher-family";
    case 0xCDE8738Du: return "stalker-family";
    case 0xE16F113Du: return "alien-necromorph-family";
    case 0xEA0411A8u: return "pregnant-family";
    default: return "unclassified";
    }
}

// The randomizer runs on the game's spawn thread.  Keep the diagnostics
// bounded and buffered: detailed enough to reconstruct a failed decision,
// but never an unbounded synchronous disk logger during a combat wave.
static bool RandomizerMayLogDiagnostic()
{
    return cfg::enemyRandomizerDiagnostics &&
        static_cast<unsigned>(InterlockedIncrement(&gRandomizerDiagnosticLines)) <=
            cfg::enemyRandomizerLogLimit;
}

static void RandomizerLogSummary(const char* reason)
{
    if (!cfg::enemyRandomizerDiagnostics)
        return;
    Log("EnemyRandomizer summary (%s): FuncSpawn=%ld unknown=%ld ineligible=%ld "
        "noLiveChoice=%ld attempts=%ld spawned=%ld rejected=%ld liveCache=%u nativeCache=%u",
        reason, gRandomizerFuncSpawnSeen, gRandomizerUnknownSource,
        gRandomizerIneligibleSource, gRandomizerNoLiveChoice,
        gRandomizerAttempts, gRandomizerSpawned, gRandomizerRejected,
        gLiveRandomizerTargetCount, gNativeRandomizerProfileCount);
}

static void RestoreRandomizerTarget(uintptr_t attributes)
{
    if (!gRandomizerPending.attributes || gRandomizerPending.attributes != attributes)
        return;
    for (unsigned i = 0; i < 4; ++i)
        mem::Write<uint32_t>(attributes + kFuncSpawnTargetGuid + i * sizeof(uint32_t),
                             gRandomizerPending.original[i]);
    gRandomizerPending = {};
}

// Runs immediately before FuncSpawn asks SimManager to resolve its GUID into
// a SpawnInfo packet.  SpawnEntity later consumes that already-resolved packet,
// so changing the GUID immediately before SpawnEntity is too late.
static void FuncSpawnPrepare_Hook(safetyhook::Context& ctx)
{
    if (!cfg::enemyRandomizer || !MayScaleSpawns() || !ctx.esi ||
        !Readable(ctx.esi + kFuncSpawnTargetGuid, 16))
        return;

    // At this point ESI is the FuncSpawn object. Its authored target GUID is
    // at +0x0C and is passed to SimManager::GetSpawnInfo immediately after
    // this instruction.
    const uintptr_t attributes = ctx.esi;
    uint32_t original[4]{};
    for (unsigned i = 0; i < 4; ++i)
        original[i] = mem::Read<uint32_t>(attributes + kFuncSpawnTargetGuid + i * sizeof(uint32_t));

    const long seen = InterlockedIncrement(&gRandomizerFuncSpawnSeen);
    if ((seen & 63) == 0 && RandomizerMayLogDiagnostic())
        RandomizerLogSummary("periodic");

    if (cfg::enemyRandomizerPercent < 100 &&
        (NextRandomizerValue() % 100) >= cfg::enemyRandomizerPercent)
    {
        if (RandomizerMayLogDiagnostic())
            Log("EnemyRandomizer skip: percent gate %08X-%08X-%08X-%08X",
                original[0], original[1], original[2], original[3]);
        return;
    }

    const RandomizerTarget* source =
        FindRandomizerTarget(original[0], original[1], original[2], original[3]);
    // Guardians are deliberately not randomised in either direction: their
    // placements are wall-bound encounter objects rather than ordinary NPC
    // spawn locations.
    if (!source)
    {
        InterlockedIncrement(&gRandomizerUnknownSource);
        if (RandomizerMayLogDiagnostic())
            Log("EnemyRandomizer skip: source outside authored pool %08X-%08X-%08X-%08X",
                original[0], original[1], original[2], original[3]);
        return;
    }
    EnsureNativeRandomizerProfileWorld(ctx.ebx);
    if (!RandomizerSourceIsEligible(*source))
    {
        InterlockedIncrement(&gRandomizerIneligibleSource);
        if (RandomizerMayLogDiagnostic())
            Log("EnemyRandomizer skip: %s candidate=%u context=%s human=%u",
                RandomizerTargetName(*source), source->candidate,
                RandomizerContextName(source->contextMask),
                RandomizerIsHuman(*source) ? 1u : 0u);
        return;
    }

    RememberLiveRandomizerTarget(*source);
    const RandomizerTarget* replacement = PickRandomizerTarget(*source, ctx.ebx);
    if (!replacement)
    {
        InterlockedIncrement(&gRandomizerNoLiveChoice);
        if (RandomizerMayLogDiagnostic())
            Log("EnemyRandomizer wait: %s (%s), compatible live targets=%u",
                RandomizerTargetName(*source), RandomizerContextName(source->contextMask),
                gLiveRandomizerTargetCount);
        return;
    }

    InterlockedIncrement(&gRandomizerAttempts);
    gRandomizerPending.attributes = attributes;
    std::memcpy(gRandomizerPending.original, original, sizeof(original));
    gRandomizerAttempt.attributes = attributes;
    std::memcpy(gRandomizerAttempt.original, original, sizeof(original));
    std::memcpy(gRandomizerAttempt.replacement, replacement->guid,
                sizeof(replacement->guid));
    gRandomizerAttempt.contextMask = source->contextMask;
    for (unsigned i = 0; i < 4; ++i)
        mem::Write<uint32_t>(attributes + kFuncSpawnTargetGuid + i * sizeof(uint32_t),
                             replacement->guid[i]);

    const long line = InterlockedIncrement(&gRandomizerLines);
    if (line <= 12 || RandomizerMayLogDiagnostic())
        Log("EnemyRandomizer attempt: %s %08X-%08X-%08X-%08X -> %s "
            "%08X-%08X-%08X-%08X (%s, live targets %u)",
            RandomizerTargetName(*source), original[0], original[1], original[2], original[3],
            RandomizerTargetName(*replacement), replacement->guid[0], replacement->guid[1],
            replacement->guid[2], replacement->guid[3],
            RandomizerContextName(source->contextMask), gLiveRandomizerTargetCount);
}

// GetSpawnInfo has copied/resolved the temporary GUID into its local SpawnInfo
// packet by this point. Restore the authored FuncSpawn before any other game
// code can observe the randomised value.
static void FuncSpawnGuidRestore_Hook(safetyhook::Context&)
{
    if (gRandomizerPending.attributes)
        RestoreRandomizerTarget(gRandomizerPending.attributes);
}

// ---------------------------------------------------------------------------
// Optional global actor stream, game-thread only.
//
// The debug menu proved this exact retail path for character STR packages:
// StreamManager::Load(path, 0, LOAD_CHAR_PACKAGE=4, nullptr, nullptr).  It is
// not the menu's direct SpawnEntity path.  We use it only after GameMainLoop's
// original Poll returns RESULT_NO_CHANGE, then wait for the engine to publish
// the controller UID into SimManager before the randomizer may use it.

constexpr uintptr_t kGameMainLoopPollImplRva = 0x00074AB0u;
constexpr uintptr_t kGameMainLoopVtableRva   = 0x009277F0u;
constexpr uintptr_t kGameMainLoopTransitionFlags = 0x20Cu;
constexpr uintptr_t kGameMainLoopQuickRestartState = 0x214u;
constexpr uintptr_t kGameMainLoopGlobalRva   = 0x00DD3B5Cu;
constexpr uintptr_t kMainLoopManagerGlobalRva = 0x00DD1948u;
constexpr uintptr_t kMainLoopManagerVtableRva = 0x00920130u;
constexpr uintptr_t kMainLoopManagerCurrentLoop = 0x04u;
constexpr uintptr_t kLevelManagerGlobalRva   = 0x00DD1A48u;
constexpr uintptr_t kLevelManagerVtableRva   = 0x00928C50u;
constexpr uintptr_t kLevelManagerState       = 0x18u;
constexpr uint32_t  kLevelManagerRunning     = 19u;
constexpr uintptr_t kTransitionBlockedRva    = 0x000027A0u;
constexpr uintptr_t kStreamManagerGlobalRva  = 0x00DD1ED8u;
constexpr uintptr_t kStreamManagerVtableRva  = 0x00A26F14u;
constexpr uintptr_t kSimManagerGlobalRva     = 0x00DD1908u;
constexpr uintptr_t kSimManagerVtableRva     = 0x00A270E4u;
constexpr uintptr_t kStreamLoadRva           = 0x00677530u;
constexpr uintptr_t kStreamFromHandleRva     = 0x00053870u;
constexpr uintptr_t kStreamGetStatusRva      = 0x000538F0u;
constexpr uintptr_t kStreamGetHandleByGuidRva = 0x0066A5B0u;
constexpr uint32_t  kStreamLoadCharPackage   = 0x00000004u;
// Confirmed in the debug menu's successful retail normal-load log for this
// exact original resource. This is the package stream GUID, not the catalog's
// SimGroup GUID hash (D65FB0C4).
constexpr uint32_t  kSuperStalkerCharacterStreamGuid = 0x8B01CF03u;
// The compact sidecar is generated from the reviewed upright Stalker actor.
// Its model and model-stream remain retail Super Stalker resources; only the
// actor identity and its stream identity are synthetic, so it cannot collide
// with the plaza template it was derived from.
constexpr uint32_t  kSuperStalkerActorSidecarStreamGuid = 0xD378F107u;
constexpr uint32_t  kSuperSlasherCharacterStreamGuid = 0x494EAD7Cu;
constexpr uint32_t  kSuperSlasherActorSidecarStreamGuid = 0xD378F013u;
constexpr ULONGLONG kStreamedTemplateTimeoutMs = 10000ull;

static const char kSuperStalkerResourcePath[] =
    "global_assets\\global_assets\\ds_assets\\char_str\\npc\\"
    "stalker_cct\\super_stalker.str";
static const char kSuperStalkerActorSidecarPath[] =
    "ds3_debug_assets\\authored\\super_stalker_upright.str";
static const char kSuperSlasherResourcePath[] =
    "global_assets\\global_assets\\ds_assets\\char_str\\npc\\"
    "zomb_cct\\super_slasher.str";
static const char kSuperSlasherActorSidecarPath[] =
    "ds3_debug_assets\\authored\\necro_078_19.str";

using GameMainLoopPollFn = uint32_t(SAFETYHOOK_THISCALL*)(void*);
using StreamLoadFn = uint32_t(SAFETYHOOK_THISCALL*)(
    void*, const char*, float, uint32_t, const uint32_t*, const char*);
using StreamFromHandleFn = void*(SAFETYHOOK_THISCALL*)(void*, uint32_t);
using StreamGetStatusFn = int(SAFETYHOOK_THISCALL*)(void*, uint32_t);
using StreamGetHandleByGuidFn = uint32_t(SAFETYHOOK_THISCALL*)(void*, uint32_t);
using TransitionBlockedFn = bool(__cdecl*)();

static void* gStreamedTemplatePollOriginal = nullptr;
static uint32_t gStreamedTemplateHandle = 0;
static uintptr_t gStreamedTemplateManager = 0;
static ULONGLONG gStreamedTemplateStarted = 0;
static uint32_t gStreamedSuperSlasherHandle = 0;
static uintptr_t gStreamedSuperSlasherManager = 0;
static ULONGLONG gStreamedSuperSlasherStarted = 0;
static bool gStreamedTemplateInstallFailed = false;

static bool BytesMatch(uintptr_t address, const uint8_t* bytes, size_t size)
{
    return address && bytes && size && Readable(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

static bool ResolveStreamedTemplateManagers(uintptr_t& streamManager,
                                            uintptr_t& simManager)
{
    streamManager = 0;
    simManager = 0;
    if (!g.module)
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
    const uintptr_t streamGlobal = base + kStreamManagerGlobalRva;
    const uintptr_t simGlobal = base + kSimManagerGlobalRva;
    if (!Readable(streamGlobal, sizeof(uintptr_t)) ||
        !Readable(simGlobal, sizeof(uintptr_t)))
        return false;

    const uintptr_t stream = mem::Read<uintptr_t>(streamGlobal);
    const uintptr_t sim = mem::Read<uintptr_t>(simGlobal);
    if (!stream || !sim || !Readable(stream, sizeof(uintptr_t)) ||
        !Readable(sim, sizeof(uintptr_t)) ||
        mem::Read<uintptr_t>(stream) != base + kStreamManagerVtableRva ||
        mem::Read<uintptr_t>(sim) != base + kSimManagerVtableRva)
        return false;

    // These two bucket arrays are the exact structural checks used by the
    // debug menu before it calls StreamManager::Load on this executable.
    if (!Readable(stream + 0x60u, sizeof(uint32_t)) ||
        !Readable(sim + 0x5Cu, sizeof(uint32_t)))
        return false;
    const uintptr_t streamBuckets = mem::Read<uintptr_t>(stream + 0x5Cu);
    const uintptr_t simBuckets = mem::Read<uintptr_t>(sim + 0x58u);
    const uint32_t streamBucketCount = mem::Read<uint32_t>(stream + 0x60u);
    const uint32_t simBucketCount = mem::Read<uint32_t>(sim + 0x5Cu);
    if (!streamBuckets || !simBuckets || !streamBucketCount || !simBucketCount ||
        streamBucketCount > 0x100000u || simBucketCount > 0x100000u ||
        !Readable(streamBuckets,
                  static_cast<size_t>(streamBucketCount) * sizeof(uintptr_t)) ||
        !Readable(simBuckets,
                  static_cast<size_t>(simBucketCount) * sizeof(uintptr_t)) ||
        mem::Read<uintptr_t>(streamGlobal) != stream ||
        mem::Read<uintptr_t>(simGlobal) != sim)
        return false;

    streamManager = stream;
    simManager = sim;
    return true;
}

// Character packages must not be requested from a menu or while a checkpoint
// transition owns the main loop. The debug menu uses these same retail
// identities before it calls StreamManager::Load; without this preflight the
// menu may return a small unrelated handle which looks superficially valid.
static bool StreamedTemplateWorldReady(void* gameMainLoop)
{
    if (!g.module || !gameMainLoop)
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
    const uintptr_t loop = reinterpret_cast<uintptr_t>(gameMainLoop);
    if (!Readable(base + kGameMainLoopGlobalRva, sizeof(uintptr_t)) ||
        !Readable(loop, sizeof(uintptr_t)) ||
        mem::Read<uintptr_t>(base + kGameMainLoopGlobalRva) != loop ||
        mem::Read<uintptr_t>(loop) != base + kGameMainLoopVtableRva ||
        !Readable(loop + kGameMainLoopTransitionFlags, sizeof(uint32_t) * 3u) ||
        mem::Read<uint32_t>(loop + kGameMainLoopTransitionFlags) != 0u ||
        mem::Read<uint32_t>(loop + kGameMainLoopQuickRestartState) != 0u)
        return false;

    const uintptr_t mainLoopGlobal = base + kMainLoopManagerGlobalRva;
    if (!Readable(mainLoopGlobal, sizeof(uintptr_t)))
        return false;
    const uintptr_t mainLoopManager = mem::Read<uintptr_t>(mainLoopGlobal);
    if (!mainLoopManager || !Readable(mainLoopManager + kMainLoopManagerCurrentLoop,
                                      sizeof(uintptr_t)) ||
        mem::Read<uintptr_t>(mainLoopManager) != base + kMainLoopManagerVtableRva ||
        mem::Read<uintptr_t>(mainLoopManager + kMainLoopManagerCurrentLoop) != loop ||
        mem::Read<uintptr_t>(mainLoopGlobal) != mainLoopManager)
        return false;

    bool transitionBlocked = true;
    bool queryCompleted = false;
    __try
    {
        transitionBlocked = reinterpret_cast<TransitionBlockedFn>(
            base + kTransitionBlockedRva)();
        queryCompleted = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        queryCompleted = false;
    }
    if (!queryCompleted || transitionBlocked)
        return false;

    const uintptr_t levelGlobal = base + kLevelManagerGlobalRva;
    if (!Readable(levelGlobal, sizeof(uintptr_t)))
        return false;
    const uintptr_t levelManager = mem::Read<uintptr_t>(levelGlobal);
    return levelManager &&
        Readable(levelManager + kLevelManagerState, sizeof(uint32_t)) &&
        mem::Read<uintptr_t>(levelManager) == base + kLevelManagerVtableRva &&
        mem::Read<uint32_t>(levelManager + kLevelManagerState) ==
            kLevelManagerRunning &&
        mem::Read<uintptr_t>(levelGlobal) == levelManager;
}

static bool ValidateStreamedTemplateHandle(uintptr_t streamManager,
                                           uint32_t handle, uint32_t expectedGuid,
                                           int& status,
                                           uint32_t& observedGuid,
                                           uint32_t& reverseHandle)
{
    status = -1;
    observedGuid = 0;
    reverseHandle = 0;
    if (!g.module || !streamManager || !handle || !expectedGuid)
        return false;

    void* stream = nullptr;
    bool completed = false;
    __try
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
        stream = reinterpret_cast<StreamFromHandleFn>(base + kStreamFromHandleRva)(
            reinterpret_cast<void*>(streamManager), handle);
        status = reinterpret_cast<StreamGetStatusFn>(base + kStreamGetStatusRva)(
            reinterpret_cast<void*>(streamManager), handle);
        reverseHandle = reinterpret_cast<StreamGetHandleByGuidFn>(
            base + kStreamGetHandleByGuidRva)(reinterpret_cast<void*>(streamManager),
                                              expectedGuid);
        completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }
    const uintptr_t streamAddress = reinterpret_cast<uintptr_t>(stream);
    if (streamAddress && Readable(streamAddress + 0x2Cu, sizeof(uint32_t)))
        observedGuid = mem::Read<uint32_t>(streamAddress + 0x2Cu);
    return completed && streamAddress && observedGuid == expectedGuid &&
        reverseHandle == handle;
}

static bool BuildGameLoadRoot(char root[MAX_PATH])
{
    if (!root)
        return false;
    root[0] = 0;
    const DWORD length = GetModuleFileNameA(nullptr, root, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;
    char* const slash = strrchr(root, '\\');
    char* const forward = strrchr(root, '/');
    char* const cut = forward && (!slash || forward > slash) ? forward : slash;
    if (!cut)
        return false;
    *(cut + 1) = 0;
    return strlen(root) + strlen(kSuperStalkerActorSidecarPath) < 0xA0u;
}

static bool RequestSuperStalkerActorSidecar(uintptr_t streamManager)
{
    char root[MAX_PATH]{};
    if (!BuildGameLoadRoot(root))
        return false;
    uint32_t handle = 0;
    bool completed = false;
    __try
    {
        const StreamLoadFn load = reinterpret_cast<StreamLoadFn>(
            reinterpret_cast<uintptr_t>(g.module) + kStreamLoadRva);
        handle = load(reinterpret_cast<void*>(streamManager),
                      kSuperStalkerActorSidecarPath, 0.0f,
                      kStreamLoadCharPackage, &kSuperStalkerActorSidecarStreamGuid,
                      root);
        completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }
    if (!completed || !handle)
    {
        Log("EnemyRandomizer stream: Super Stalker Actor sidecar load rejected "
            "(handle=%08X call=%u)", handle, completed ? 1u : 0u);
        return false;
    }
    gStreamedTemplateHandle = handle;
    gStreamedTemplateManager = streamManager;
    gStreamedTemplateStarted = GetTickCount64();
    InterlockedExchange(&gStreamedTemplateState,
                        STREAMED_TEMPLATE_LOADING_ACTOR);
    Log("EnemyRandomizer stream: requested Super Stalker Actor sidecar "
        "(handle=%08X); waiting for full Actor registration", handle);
    return true;
}

static bool RequestSuperSlasherActorSidecar(uintptr_t streamManager)
{
    char root[MAX_PATH]{};
    if (!BuildGameLoadRoot(root))
        return false;
    uint32_t handle = 0;
    bool completed = false;
    __try
    {
        const StreamLoadFn load = reinterpret_cast<StreamLoadFn>(
            reinterpret_cast<uintptr_t>(g.module) + kStreamLoadRva);
        handle = load(reinterpret_cast<void*>(streamManager),
                      kSuperSlasherActorSidecarPath, 0.0f,
                      kStreamLoadCharPackage, &kSuperSlasherActorSidecarStreamGuid,
                      root);
        completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }
    if (!completed || !handle)
    {
        Log("EnemyRandomizer stream: Super Slasher Actor sidecar load rejected "
            "(handle=%08X call=%u)", handle, completed ? 1u : 0u);
        return false;
    }
    gStreamedSuperSlasherHandle = handle;
    gStreamedSuperSlasherManager = streamManager;
    gStreamedSuperSlasherStarted = GetTickCount64();
    InterlockedExchange(&gStreamedSuperSlasherState,
                        STREAMED_TEMPLATE_LOADING_ACTOR);
    Log("EnemyRandomizer stream: requested Super Slasher Actor sidecar "
        "(handle=%08X); waiting for full Actor registration", handle);
    return true;
}

static void ProcessStreamedTemplateTick(void* gameMainLoop)
{
    if (!cfg::enemyRandomizerGlobalTemplates || gStreamedTemplateInstallFailed ||
        InterlockedCompareExchange(&gStreamedTemplateState, 0, 0) ==
            STREAMED_TEMPLATE_FAILED)
        return;

    if (!StreamedTemplateWorldReady(gameMainLoop))
        return;

    uintptr_t streamManager = 0;
    uintptr_t simManager = 0;
    if (!ResolveStreamedTemplateManagers(streamManager, simManager))
        return;

    // LevelManager RUNNING also describes DS3's rendered menu scene. A real
    // FuncSpawn is the cheap, native proof that this SimManager is processing
    // an authored gameplay level rather than that menu scene. Do not ask the
    // StreamManager for an actor package before that proof exists.
    if (InterlockedCompareExchange(&gStreamedTemplateState, 0, 0) ==
            STREAMED_TEMPLATE_WAITING &&
        InterlockedCompareExchange(&gRandomizerFuncSpawnSeen, 0, 0) == 0)
        return;

    LONG state = InterlockedCompareExchange(&gStreamedTemplateState, 0, 0);
    if (state == STREAMED_TEMPLATE_READY)
    {
        // A level transition may replace SimManager.  Do not blindly retain a
        // prior world's proof; make the feature dormant rather than issuing
        // repeated Load calls during the transition.
        if (!RandomizerTargetIsLive(simManager, kStreamedSuperStalker))
        {
            InterlockedExchange(&gStreamedTemplateState,
                                STREAMED_TEMPLATE_WAITING);
            gStreamedTemplateHandle = 0;
            gStreamedTemplateManager = 0;
            Log("EnemyRandomizer stream: Super Stalker registration lost; "
                "waiting for the next stable world");
        }
        return;
    }

    if (state == STREAMED_TEMPLATE_LOADING_CHARACTER ||
        state == STREAMED_TEMPLATE_LOADING_ACTOR)
    {
        if (streamManager != gStreamedTemplateManager)
        {
            InterlockedExchange(&gStreamedTemplateState,
                                STREAMED_TEMPLATE_WAITING);
            gStreamedTemplateHandle = 0;
            gStreamedTemplateManager = 0;
            return;
        }
        int status = -1;
        uint32_t observedGuid = 0;
        uint32_t reverseHandle = 0;
        const bool loadingActor = state == STREAMED_TEMPLATE_LOADING_ACTOR;
        const uint32_t expectedGuid = loadingActor
            ? kSuperStalkerActorSidecarStreamGuid
            : kSuperStalkerCharacterStreamGuid;
        if (!ValidateStreamedTemplateHandle(streamManager, gStreamedTemplateHandle,
                                            expectedGuid, status, observedGuid,
                                            reverseHandle))
        {
            InterlockedExchange(&gStreamedTemplateState,
                                STREAMED_TEMPLATE_FAILED);
            Log("EnemyRandomizer stream: Super Stalker %s handle validation failed "
                "(handle=%08X guid=%08X reverse=%08X status=%d)",
                loadingActor ? "Actor sidecar" : "character package",
                gStreamedTemplateHandle, observedGuid, reverseHandle, status);
            return;
        }
        if (status == 3 && !loadingActor &&
            RandomizerTargetIsLive(simManager, kStreamedSuperStalkerController))
        {
            if (!RequestSuperStalkerActorSidecar(streamManager))
                InterlockedExchange(&gStreamedTemplateState,
                                    STREAMED_TEMPLATE_FAILED);
            return;
        }
        if (status == 3 && loadingActor &&
            RandomizerTargetIsLive(simManager, kStreamedSuperStalker))
        {
            InterlockedExchange(&gStreamedTemplateState,
                                STREAMED_TEMPLATE_READY);
            Log("EnemyRandomizer stream: Super Stalker full Actor registered; "
                "native Stalker replacements may now select it");
            return;
        }
        if (GetTickCount64() - gStreamedTemplateStarted >=
            kStreamedTemplateTimeoutMs)
        {
            InterlockedExchange(&gStreamedTemplateState,
                                STREAMED_TEMPLATE_FAILED);
            Log("EnemyRandomizer stream: Super Stalker %s did not register "
                "within %llu ms (stream status %d)",
                loadingActor ? "Actor sidecar" : "character package",
                static_cast<unsigned long long>(kStreamedTemplateTimeoutMs), status);
        }
        return;
    }

    uint32_t handle = 0;
    bool completed = false;
    __try
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
        // Load performs its own path normalization and mounted-TOC lookup.
        // Calling the separate lookup helper first was redundant and, unlike
        // Load, does not resolve this package in all level states.
        const StreamLoadFn load =
            reinterpret_cast<StreamLoadFn>(base + kStreamLoadRva);
        handle = load(reinterpret_cast<void*>(streamManager),
                      kSuperStalkerResourcePath, 0.0f,
                      kStreamLoadCharPackage, nullptr, nullptr);
        completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }

    if (!completed || !handle)
    {
        InterlockedExchange(&gStreamedTemplateState, STREAMED_TEMPLATE_FAILED);
        Log("EnemyRandomizer stream: Super Stalker load rejected "
            "(handle=%08X call=%u)", handle, completed ? 1u : 0u);
        return;
    }
    gStreamedTemplateHandle = handle;
    gStreamedTemplateManager = streamManager;
    gStreamedTemplateStarted = GetTickCount64();
    InterlockedExchange(&gStreamedTemplateState,
                        STREAMED_TEMPLATE_LOADING_CHARACTER);
    Log("EnemyRandomizer stream: requested Super Stalker character package "
        "(handle=%08X); waiting for controller registration", handle);
}

// The second template follows the same retail StreamManager route as Super
// Stalker.  It remains a separate state machine: one bad sidecar must not
// invalidate an already-ready template from another family.
static void ProcessSuperSlasherTemplateTick(void* gameMainLoop)
{
    if (!cfg::enemyRandomizerGlobalTemplates || gStreamedTemplateInstallFailed ||
        InterlockedCompareExchange(&gStreamedSuperSlasherState, 0, 0) ==
            STREAMED_TEMPLATE_FAILED)
        return;
    if (!StreamedTemplateWorldReady(gameMainLoop))
        return;

    uintptr_t streamManager = 0;
    uintptr_t simManager = 0;
    if (!ResolveStreamedTemplateManagers(streamManager, simManager))
        return;

    if (InterlockedCompareExchange(&gStreamedSuperSlasherState, 0, 0) ==
            STREAMED_TEMPLATE_WAITING &&
        InterlockedCompareExchange(&gRandomizerFuncSpawnSeen, 0, 0) == 0)
        return;

    LONG state = InterlockedCompareExchange(&gStreamedSuperSlasherState, 0, 0);
    if (state == STREAMED_TEMPLATE_READY)
    {
        if (!RandomizerTargetIsLive(simManager, kStreamedSuperSlasher))
        {
            InterlockedExchange(&gStreamedSuperSlasherState,
                                STREAMED_TEMPLATE_WAITING);
            gStreamedSuperSlasherHandle = 0;
            gStreamedSuperSlasherManager = 0;
            Log("EnemyRandomizer stream: Super Slasher registration lost; "
                "waiting for the next stable world");
        }
        return;
    }

    if (state == STREAMED_TEMPLATE_LOADING_CHARACTER ||
        state == STREAMED_TEMPLATE_LOADING_ACTOR)
    {
        if (streamManager != gStreamedSuperSlasherManager)
        {
            InterlockedExchange(&gStreamedSuperSlasherState,
                                STREAMED_TEMPLATE_WAITING);
            gStreamedSuperSlasherHandle = 0;
            gStreamedSuperSlasherManager = 0;
            return;
        }
        const bool loadingActor = state == STREAMED_TEMPLATE_LOADING_ACTOR;
        const uint32_t expectedGuid = loadingActor
            ? kSuperSlasherActorSidecarStreamGuid
            : kSuperSlasherCharacterStreamGuid;
        int status = -1;
        uint32_t observedGuid = 0;
        uint32_t reverseHandle = 0;
        if (!ValidateStreamedTemplateHandle(streamManager,
                                            gStreamedSuperSlasherHandle,
                                            expectedGuid, status, observedGuid,
                                            reverseHandle))
        {
            InterlockedExchange(&gStreamedSuperSlasherState,
                                STREAMED_TEMPLATE_FAILED);
            Log("EnemyRandomizer stream: Super Slasher %s handle validation "
                "failed (handle=%08X guid=%08X reverse=%08X status=%d)",
                loadingActor ? "Actor sidecar" : "character package",
                gStreamedSuperSlasherHandle, observedGuid, reverseHandle, status);
            return;
        }
        if (status == 3 && !loadingActor &&
            RandomizerTargetIsLive(simManager, kStreamedSuperSlasherController))
        {
            if (!RequestSuperSlasherActorSidecar(streamManager))
                InterlockedExchange(&gStreamedSuperSlasherState,
                                    STREAMED_TEMPLATE_FAILED);
            return;
        }
        if (status == 3 && loadingActor &&
            RandomizerTargetIsLive(simManager, kStreamedSuperSlasher))
        {
            InterlockedExchange(&gStreamedSuperSlasherState,
                                STREAMED_TEMPLATE_READY);
            Log("EnemyRandomizer stream: Super Slasher full Actor registered; "
                "native Slasher replacements may now select it");
            return;
        }
        if (GetTickCount64() - gStreamedSuperSlasherStarted >=
            kStreamedTemplateTimeoutMs)
        {
            InterlockedExchange(&gStreamedSuperSlasherState,
                                STREAMED_TEMPLATE_FAILED);
            Log("EnemyRandomizer stream: Super Slasher %s did not register "
                "within %llu ms (stream status %d)",
                loadingActor ? "Actor sidecar" : "character package",
                static_cast<unsigned long long>(kStreamedTemplateTimeoutMs), status);
        }
        return;
    }

    uint32_t handle = 0;
    bool completed = false;
    __try
    {
        const StreamLoadFn load = reinterpret_cast<StreamLoadFn>(
            reinterpret_cast<uintptr_t>(g.module) + kStreamLoadRva);
        handle = load(reinterpret_cast<void*>(streamManager),
                      kSuperSlasherResourcePath, 0.0f,
                      kStreamLoadCharPackage, nullptr, nullptr);
        completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        completed = false;
    }
    if (!completed || !handle)
    {
        InterlockedExchange(&gStreamedSuperSlasherState,
                            STREAMED_TEMPLATE_FAILED);
        Log("EnemyRandomizer stream: Super Slasher load rejected "
            "(handle=%08X call=%u)", handle, completed ? 1u : 0u);
        return;
    }
    gStreamedSuperSlasherHandle = handle;
    gStreamedSuperSlasherManager = streamManager;
    gStreamedSuperSlasherStarted = GetTickCount64();
    InterlockedExchange(&gStreamedSuperSlasherState,
                        STREAMED_TEMPLATE_LOADING_CHARACTER);
    Log("EnemyRandomizer stream: requested Super Slasher character package "
        "(handle=%08X); waiting for controller registration", handle);
}

static uint32_t __fastcall StreamedTemplatePoll_Hook(void* self, void*)
{
    const GameMainLoopPollFn original =
        reinterpret_cast<GameMainLoopPollFn>(gStreamedTemplatePollOriginal);
    if (!original)
        return 0;
    const uint32_t result = original(self);
    if (result == 0)
    {
        // Cross-family mode is currently a deliberately narrow Slasher ->
        // Super Slasher experiment. Do not keep the unrelated full Stalker
        // sidecar resident in that mode: it cannot be selected by the narrow
        // picker and needlessly adds actor/update work to every frame.
        if (!cfg::enemyRandomizerCrossFamilyGround)
            ProcessStreamedTemplateTick(self);
        ProcessSuperSlasherTemplateTick(self);
    }
    return result;
}

static void InstallStreamedTemplateLoader()
{
    if (!cfg::enemyRandomizerGlobalTemplates || !g.module ||
        gStreamedTemplateInstallFailed || gStreamedTemplatePollOriginal)
        return;

    static const uint8_t kPollSignature[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x56, 0x57,
        0x8B, 0xF9, 0xE8, 0xC1, 0xDC, 0xF8, 0xFF, 0x84,
        0xC0, 0x74, 0x1F, 0xA1, 0xA8, 0x1A, 0x1D, 0x01,
        0x85, 0xC0, 0x74, 0x06, 0x83, 0x78, 0x0C, 0x03,
    };
    const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
    void** const slot = reinterpret_cast<void**>(
        base + kGameMainLoopVtableRva + 0x24u);
    const void* expected = reinterpret_cast<const void*>(
        base + kGameMainLoopPollImplRva);
    if (!BytesMatch(base + kGameMainLoopPollImplRva, kPollSignature,
                    sizeof(kPollSignature)) ||
        !Readable(reinterpret_cast<uintptr_t>(slot), sizeof(*slot)) ||
        *slot != expected)
    {
        gStreamedTemplateInstallFailed = true;
        Log("EnemyRandomizer stream: disabled (GameMainLoop Poll binding mismatch)");
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        gStreamedTemplateInstallFailed = true;
        Log("EnemyRandomizer stream: disabled (Poll vtable is not writable)");
        return;
    }
    gStreamedTemplatePollOriginal = const_cast<void*>(expected);
    void* const observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&StreamedTemplatePoll_Hook),
        const_cast<void*>(expected));
    DWORD ignored = 0;
    (void)VirtualProtect(slot, sizeof(*slot), oldProtect, &ignored);
    if (observed != expected)
    {
        gStreamedTemplatePollOriginal = nullptr;
        gStreamedTemplateInstallFailed = true;
        Log("EnemyRandomizer stream: disabled (Poll slot changed during install)");
        return;
    }
    Log("EnemyRandomizer stream: game-thread loader installed; "
        "reviewed templates are Super Stalker and Super Slasher");
}

// ---------------------------------------------------------------------------
// Runtime classification of the object that was created.
//
// The table alone is not enough. Of eight targets that actually spawned, not
// one was in the corpus table; all eight came through as blocked/unknown.
// That is the limit called out when it was built: only a fraction of the
// targets seen at runtime have a `CMD_SetSpawnTarget` edge in the level data,
// and these eight were not among them. The table does the right thing (no when
// in doubt), but in practice the unknown case is the majority, so nothing
// scales.
//
// A bigger table does not help: those targets are not wired up through any
// readable path in the level data.
//
// The way out: the FuncSpawn hook has the finished object in hand, and its
// vtable says what it is, using the same evidence as the static check,
// only taken from the object itself. Slot +0x260 is
// `Sentient::ApplyDamageModifiers`, and it has three cases:
//
//     0x00A52A60   the empty base stub -> processes no damage
//     0x00447E50   Player::...         -> a player, never scale
//     anything else -> a class with its own damage handling, i.e. an enemy
//                      with combat values
//
// That covers every target, independent of any table.
//
// Not every object has a vtable reaching as far as +0x260; on a prop the slot
// points into unrelated data. So both the vtable and the slot contents are
// checked to be inside the module. If either check fails, the object counts as
// not scalable.
constexpr uintptr_t kSentientDamageStubRva = 0x00652A60;   // 0x00A52A60
constexpr uintptr_t kPlayerDamageModRva    = 0x00047E50;   // 0x00447E50
constexpr uintptr_t kDamageModifierSlot    = 0x260;

// Coarse but sufficient: the game's code ends well before 0xD40000 and the
// data holding the vtables sits after it. Both are only a plausibility guard;
// the real decision is made from the slot contents.
constexpr uintptr_t kModuleSpan = 0x01700000;

enum RuntimeClass : uint8_t
{
    RUNTIME_COMBAT = 0,     // has its own damage handling, so an enemy
    RUNTIME_NO_DAMAGE,      // leerer Basis-Stub -> Requisite/Trigger
    RUNTIME_PLAYER,         // a player
    RUNTIME_UNKNOWN,        // undecidable, treated as blocked
};

static RuntimeClass ClassifySpawnedObject(uintptr_t spawned, uintptr_t& outSlot,
                                          uintptr_t& outVtable)
{
    outSlot = 0;
    outVtable = 0;
    if (!spawned || !g.module || !Readable(spawned, sizeof(uintptr_t)))
        return RUNTIME_UNKNOWN;

    const uintptr_t base = reinterpret_cast<uintptr_t>(g.module);
    const uintptr_t vtable = mem::Read<uintptr_t>(spawned);
    outVtable = vtable;
    if (vtable <= base || vtable >= base + kModuleSpan)
        return RUNTIME_UNKNOWN;
    if (!Readable(vtable + kDamageModifierSlot, sizeof(uintptr_t)))
        return RUNTIME_UNKNOWN;

    const uintptr_t fn = mem::Read<uintptr_t>(vtable + kDamageModifierSlot);
    if (fn <= base || fn >= base + kModuleSpan)
        return RUNTIME_UNKNOWN;      // kein plausibler Codezeiger
    outSlot = fn;

    if (fn == base + kSentientDamageStubRva)
        return RUNTIME_NO_DAMAGE;
    if (fn == base + kPlayerDamageModRva)
        return RUNTIME_PLAYER;
    return RUNTIME_COMBAT;
}

static const char* RuntimeClassName(RuntimeClass rc)
{
    switch (rc)
    {
    case RUNTIME_COMBAT:    return "enemy(runtime)";
    case RUNTIME_NO_DAMAGE: return "no damage";
    case RUNTIME_PLAYER:    return "player";
    default:                return "unklar";
    }
}

static const char* SpawnClassName(uint8_t klass)
{
    switch (klass)
    {
    case SPAWN_CLASS_COMBAT:         return "enemy";
    case SPAWN_CLASS_NON_COMBAT:     return "Requisite/Trigger";
    case SPAWN_CLASS_SCRIPTED_STAGE: return "Boss-Buehne";
    default:                         return "unbekannt";
    }
}

static const char* SpawnArchetypeName(uint8_t index)
{
    return index < kSpawnArchetypeCount ? kSpawnArchetypes[index] : "?";
}

// Percentage for this type, as set in the INI. 100 follows the global factor,
// 0 never scales this type, 150 is half again as strong.
static unsigned ArchetypePercent(uint8_t index)
{
    // An unknown type (0xFF) does not mean "not an enemy". Permission was
    // already granted earlier, by the corpus table, the enemy family or a hit
    // of your own. All that is left here is whether the player set this type
    // separately. With no type there is no slider, so the global value stands.
    //
    // This used to return 0, and because scaling requires `typePercent > 0`
    // that dropped everything without a recognised type: 55 of the confirmed
    // combat targets, plus every spawner that is only covered by an enemy
    // family, which is exactly what the family list exists for.
    if (index >= kSpawnArchetypeCount)
        return 100;
    return cfg::fourPlayerSpawnTypePercent[index];
}

// A retry is asynchronous. Remembering it prevents the generated event from
// recursively scheduling a second multiplier pass when it returns through the
// same FuncSpawn hook.
struct ScriptedRetryLedger
{
    uintptr_t spawner = 0;
    unsigned pending = 0;
};
static ScriptedRetryLedger gScriptedRetryLedger[96]{};
static unsigned gScriptedScaleRemainder = 0;

static bool ConsumeScriptedRetry(uintptr_t spawner)
{
    for (ScriptedRetryLedger& entry : gScriptedRetryLedger)
    {
        if (entry.spawner != spawner || !entry.pending)
            continue;
        --entry.pending;
        if (!entry.pending)
            entry.spawner = 0;
        return true;
    }
    return false;
}

static bool RememberScriptedRetry(uintptr_t spawner)
{
    for (ScriptedRetryLedger& entry : gScriptedRetryLedger)
    {
        if (entry.spawner == spawner)
        {
            if (entry.pending == 0xFFFFFFFFu)
                return false;
            ++entry.pending;
            return true;
        }
    }
    for (ScriptedRetryLedger& entry : gScriptedRetryLedger)
    {
        if (!entry.spawner)
        {
            entry.spawner = spawner;
            entry.pending = 1;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Raise maxSpawn so limited spawners scale too.
//
// The enemies that actually turn up have `max=1`, while scaling required
// `maxSpawn == -1`. A retry does nothing at `max=1`, because after the first
// instance the spawner has reached `spawnedCount == maxSpawn` and stops. So
// without raising it the normal case does not scale at all. `CMD_SetMaxSpawn`
// appears 66 times across the corpus and is always 1.
//
// Two traps this bookkeeping avoids.
//
// First, the attribute packet can be shared between spawners. Multiplying the
// value on every pass makes it grow without bound: 1 becomes 3, then 9, then
// 27. So each packet is raised exactly once and its original recorded; every
// later call recognises the packet and leaves it alone.
//
// Second, the target value is always computed from the original, never from
// the current one, which makes it independent of how often the hook runs.
struct MaxSpawnLedger
{
    uintptr_t attributes = 0;
    int original = 0;
    int applied = 0;
    int popOriginal = 0;
    int popApplied = 0;
};
static MaxSpawnLedger gMaxSpawnLedger[128]{};

// Cap for the raised value. A spawner suddenly producing 50 enemies would be
// a risk in a 32-bit process, and a typo in the INI should not make the game
// unplayable.
constexpr int kMaxSpawnCeiling = 12;
// Enemies alive at once per group. Deliberately lower than the spawn cap:
// this is the number that hits rendering and the 32-bit address space.
constexpr int kPopThresholdCeiling = 16;

// Returns the already-raised value, or raises it once. `percent` is the full
// scaling factor (200 = double).
static int RaiseMaxSpawn(uintptr_t attributes, int currentMax, unsigned percent,
                         bool& outFirstTime, int& outOriginal)
{
    outFirstTime = false;
    outOriginal = currentMax;

    if (!attributes || currentMax <= 0 || percent <= 100)
        return currentMax;

    for (MaxSpawnLedger& entry : gMaxSpawnLedger)
    {
        if (entry.attributes == attributes)
        {
            outOriginal = entry.original;
            return entry.applied;          // already raised, leave it alone
        }
    }

    // Target from the original, rounded up so 150% turns 1 into 2.
    long long wanted = (static_cast<long long>(currentMax) * percent + 99) / 100;
    if (wanted <= currentMax)
        wanted = currentMax + 1;
    if (wanted > kMaxSpawnCeiling)
        wanted = kMaxSpawnCeiling;
    if (wanted == currentMax)
        return currentMax;

    if (!Readable(attributes + kFuncSpawnMaxSpawn, sizeof(int)) ||
        !mem::Write<int>(attributes + kFuncSpawnMaxSpawn, static_cast<int>(wanted)))
        return currentMax;

    // The population limit has to grow with it, otherwise the scaling eats its
    // own budget. Visible in the retail code at 0x006AACC0:
    //
    //     cmp dword [esi+0x54], 0    ; popThreshold
    //     jle normal_spawn           ; 0 = no limit
    //     call 0x694700              ; population reached?
    //     je  normal_spawn
    //     call 0x67F7A0              ; otherwise bail
    //     ret 4                      ; no spawn
    //
    // Every spawner in a `group` shares that limit. Without raising it the
    // first spawners produce their extras, the budget is full, and the
    // spawners due later in the same group never get their turn, exactly the
    // "some enemies do not spawn at all" symptom.
    int popOriginal = 0, popApplied = 0;
    if (cfg::fourPlayerRaisePopThreshold &&
        Readable(attributes + kFuncSpawnPopThreshold, sizeof(int)))
    {
        popOriginal = mem::Read<int>(attributes + kFuncSpawnPopThreshold);
        if (popOriginal > 0)
        {
            long long popWanted =
                (static_cast<long long>(popOriginal) * percent + 99) / 100;
            if (popWanted <= popOriginal)
                popWanted = popOriginal + 1;
            if (popWanted > kPopThresholdCeiling)
                popWanted = kPopThresholdCeiling;
            if (popWanted != popOriginal &&
                mem::Write<int>(attributes + kFuncSpawnPopThreshold,
                                static_cast<int>(popWanted)))
                popApplied = static_cast<int>(popWanted);
        }
    }

    for (MaxSpawnLedger& entry : gMaxSpawnLedger)
    {
        if (!entry.attributes)
        {
            entry.attributes = attributes;
            entry.original = currentMax;
            entry.applied = static_cast<int>(wanted);
            entry.popOriginal = popOriginal;
            entry.popApplied = popApplied;
            break;
        }
    }
    outFirstTime = true;
    return static_cast<int>(wanted);
}

constexpr uintptr_t kPlayerManagerGlobalRva = 0x00DD3B78;
constexpr uintptr_t kPlayerManagerVtableRva = 0x00A26BA8;
constexpr uintptr_t kOnlineServicesGlobalRva = 0x00DD1A1C;
constexpr uintptr_t kOnlineSessionOffset = 0x28;
constexpr uintptr_t kIsHostVtableOffset = 0x54;

// Director quotas are instances that survive for the loaded level. Retaining
// their original targets lets a player join/leave change the factor without
// repeatedly scaling an already scaled value.
struct DynamicQuotaBaseline
{
    uintptr_t quota = 0;
    uintptr_t attributes = 0;
    unsigned targets[kQuotaEntries]{};
    unsigned appliedPercent = 0;
};
static DynamicQuotaBaseline gDynamicQuotaBaselines[24]{};

static unsigned MoreEnemiesPercentForPlayersImpl(unsigned playerCount)
{
    // Single player is the public Horde setting. In co-op DS3 is already
    // balanced for the configured base roster, so only additional players
    // increase the factor. This is deliberately shared with the Director
    // path; otherwise a quarantine room would not match its scripted lead-in.
    if (playerCount <= 1)
    {
        const unsigned solo = cfg::fourPlayerScriptedSpawnSoloTestPercent;
        return solo >= 100 ? solo : 100u;
    }

    // Freely configurable instead of hard-coding four players. The live
    // PlayerManager roster is bounded to 16 by its own validation.
    const unsigned basis = cfg::spawnBasePlayers;
    if (playerCount <= basis)
        return 100u;
    return 100u + (playerCount - basis) * cfg::spawnPercentPerPlayer;
}

static unsigned RoundQuotaTarget(unsigned baseline, unsigned percent)
{
    unsigned long long scaled =
        (static_cast<unsigned long long>(baseline) * percent + 50ull) / 100ull;
    if (baseline && !scaled)
        scaled = 1;
    if (scaled > kQuotaTargetCeiling)
        scaled = kQuotaTargetCeiling;
    return static_cast<unsigned>(scaled);
}

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
    const uintptr_t function = mem::Read<uintptr_t>(vtable + kIsHostVtableOffset);
    if (!function || !Readable(function, 1))
        return -1;
    using IsHostFn = bool(SAFETYHOOK_THISCALL*)(void*);
    return reinterpret_cast<IsHostFn>(function)(reinterpret_cast<void*>(session)) ? 1 : 0;
}

// May this machine create enemies?
//
// `LocalIsHost` has three answers, not two: 1 host, 0 client, -1 the question
// cannot be answered because there is no online session at all. That last case
// is single player.
//
// This used to be `LocalIsHost() == 1` everywhere, which made single player
// count as a client and excluded it from scaling, a guard only ever meant for
// other players. Visible in the log as solo play with `[CLIENT]` and
// `scheduled=0` on every spawn line.
//
// So only a machine that is demonstrably a client gets excluded.
static unsigned LivePlayerCount();

static bool MayScaleSpawns()
{
    // Ask the question that matters first: are there other players at all?
    // Being alone is the normal case, and then there is nobody to break
    // anything for.
    //
    // Why not just `LocalIsHost`: that answer is not reliable. In a perfectly
    // ordinary single player campaign it reported "client" for five minutes
    // straight: eight spawn lines, all with scheduled=0, so no scaling at
    // all. In a later run with the same INI, the same area and the same solo
    // mode, the same function reported "host". What made the difference could
    // not be established, and a single player feature must not depend on what
    // an online interface happens to be willing to say.
    //
    // The guard exists for one genuinely harmful case: a client creating
    // enemies that do not exist on the host. That needs a second player.
    // Without one, nothing is blocked.
    const unsigned spieler = LivePlayerCount();
    if (spieler <= 1)
        return true;

    // More than one player, so the role matters now. Still only a machine that
    // is demonstrably a client gets excluded; an unanswerable question (-1)
    // counts as allowed.
    return LocalIsHost() != 0;
}


static unsigned LivePlayerCount()
{
    if (!g.module)
        return 0;
    const uintptr_t global = reinterpret_cast<uintptr_t>(g.module) + kPlayerManagerGlobalRva;
    if (!Readable(global, sizeof(uintptr_t)))
        return 0;
    const uintptr_t manager = mem::Read<uintptr_t>(global);
    if (!manager || !Readable(manager, 0x10) ||
        mem::Read<uintptr_t>(manager) !=
            reinterpret_cast<uintptr_t>(g.module) + kPlayerManagerVtableRva)
        return 0;
    const unsigned capacity = mem::Read<unsigned>(manager + 0x08);
    const unsigned count = mem::Read<unsigned>(manager + 0x0C);
    return capacity >= 1 && capacity <= 16 && count >= 1 && count <= capacity ? count : 0;
}

static unsigned ExtraScriptedSpawnsForPlayers(unsigned playerCount,
                                              unsigned typePercent)
{
    unsigned percent = MoreEnemiesPercentForPlayersImpl(playerCount);

    // The per-type factor applies to the surcharge, not the base amount: at a
    // global 200% the surcharge is 100, and a type set to 50% takes 50 of that
    //, so it lands at 150% while the rest sits at 200%. A type at 0 is never
    // scaled.
    if (typePercent != 100 && percent > 100)
        percent = 100 + (percent - 100) * typePercent / 100;

    if (percent <= 100)
        return 0;

    // Keep fractional factors exact across the encounter: at 150 percent
    // every second authored combat spawn receives one native queued retry;
    // at 250 percent each gets two and every second one gets a third.
    gScriptedScaleRemainder += percent - 100;
    const unsigned extras = gScriptedScaleRemainder / 100;
    gScriptedScaleRemainder %= 100;
    return extras;
}

static bool QueueNativeScriptedRetry(uintptr_t spawner, uintptr_t attributes)
{
    if (!gFuncSpawnQueueRequest || !spawner || !attributes ||
        !Readable(attributes + kFuncSpawnOptions, sizeof(uint32_t)) ||
        !Readable(spawner + kFuncSpawnRetryMessage, sizeof(uint32_t)) ||
        !Readable(spawner + kFuncSpawnQueuedCount, sizeof(uint32_t)))
        return false;

    // QueueRequest only schedules its engine event when this authored option
    // bit is set. The source data remains untouched outside this native call.
    const unsigned originalOptions = mem::Read<unsigned>(attributes + kFuncSpawnOptions);
    const unsigned retryMessage = mem::Read<unsigned>(spawner + kFuncSpawnRetryMessage);
    const unsigned queuedBefore = mem::Read<unsigned>(spawner + kFuncSpawnQueuedCount);
    if (!retryMessage || !mem::Write<unsigned>(attributes + kFuncSpawnOptions,
                                                originalOptions | kFuncSpawnRetryOption))
        return false;

    gFuncSpawnQueueRequest(reinterpret_cast<void*>(spawner));
    const bool restored = mem::Write<unsigned>(attributes + kFuncSpawnOptions,
                                               originalOptions);
    const unsigned queuedAfter = mem::Read<unsigned>(spawner + kFuncSpawnQueuedCount);
    return restored && queuedAfter > queuedBefore && RememberScriptedRetry(spawner);
}

static DynamicQuotaBaseline* RememberDynamicQuota(uintptr_t quota, uintptr_t attributes)
{
    // Key on the attribute block only, not on the pair with the quota object.
    // A checkpoint restart gives the quota a new address while the attribute
    // block survives, including the values we wrote into it the first time.
    // Keying on the pair then finds nothing, creates a fresh entry and records
    // our own output as the baseline. Visible in the log as 18 -> 36 on the
    // first visit and 36 -> 72 after the restart.
    for (DynamicQuotaBaseline& state : gDynamicQuotaBaselines)
        if (state.attributes == attributes)
        {
            state.quota = quota;      // follow the pointer, keep the baseline
            return &state;
        }

    DynamicQuotaBaseline* slot = nullptr;
    for (DynamicQuotaBaseline& state : gDynamicQuotaBaselines)
    {
        if (!state.quota)
        {
            slot = &state;
            break;
        }
    }
    if (!slot)
        return nullptr;

    slot->quota = quota;
    slot->attributes = attributes;
    for (unsigned i = 0; i < kQuotaEntries; ++i)
        slot->targets[i] = mem::Read<unsigned>(
            attributes + i * kQuotaAttributeStride + kQuotaTargetCount);
    return slot;
}

static void ApplyDynamicQuota(uintptr_t quota, unsigned playerCount)
{
    if (!cfg::fourPlayerDirectorQuotaScaling)
        return;
    if (!Readable(quota + kQuotaSpawnCount, kQuotaEntries * sizeof(uint32_t)))
        return;
    const uintptr_t attributes = mem::Read<uintptr_t>(quota + kQuotaAttributes);
    constexpr size_t kAttributesSize = kQuotaEntries * kQuotaAttributeStride;
    if (!attributes || !Readable(attributes, kAttributesSize))
        return;

    DynamicQuotaBaseline* state = RememberDynamicQuota(quota, attributes);
    if (!state)
    {
        Log("EnemySpawns: dynamic quota baseline cache is full; quota %08X skipped",
            static_cast<unsigned>(quota));
        return;
    }

    const unsigned percent = MoreEnemiesPercentForPlayersImpl(playerCount);
    if (state->appliedPercent == percent)
        return;

    // Only the first m_numValidEntries slots belong to the roster. The array
    // is always 16 long, but a quarantine typically fills four or five of them
    // and leaves whatever the level data had in the rest. Scaling all sixteen
    // wrote into slots the encounter never reads, and inflated the logged
    // total on top of it: a roster of five real enemies was reported as
    // eighteen because eleven dead slots were counted with it.
    unsigned validEntries = mem::Read<unsigned>(quota + kQuotaValidEntries);
    if (!validEntries || validEntries > kQuotaEntries)
        return;                 // 0 or out of range: not a roster we know

    // Authored spawn counts are small. A single entry in the hundreds means
    // we are not looking at a quota at all, and writing there would corrupt
    // whatever it really is.
    for (unsigned i = 0; i < validEntries; ++i)
        if (state->targets[i] > kQuotaTargetCeiling)
            return;

    unsigned originalTotal = 0;
    unsigned targetTotal = 0;
    unsigned changed = 0;
    bool writeFailed = false;
    for (unsigned i = 0; i < validEntries; ++i)
    {
        const unsigned original = state->targets[i];
        const unsigned target = RoundQuotaTarget(original, percent);
        const uintptr_t targetAddress =
            attributes + i * kQuotaAttributeStride + kQuotaTargetCount;
        originalTotal += original;
        targetTotal += target;
        if (mem::Read<unsigned>(targetAddress) == target)
            continue;
        if (!mem::Write<unsigned>(targetAddress, target))
        {
            writeFailed = true;
            break;
        }
        ++changed;
    }
    if (writeFailed)
    {
        Log("FourPlayerSpawns: dynamic quota write failed at quota %08X",
            static_cast<unsigned>(quota));
        return;
    }

    state->appliedPercent = percent;
    Log("EnemySpawns: dynamic Director quota players=%u factor=%u%% "
        "quota=%08X entries=%u total=%u -> %u changed=%u", playerCount, percent,
        static_cast<unsigned>(quota), validEntries, originalTotal, targetTotal,
        changed);
}

static void ScaleEncounterTotal(uintptr_t encounter, unsigned playerCount)
{
    const uintptr_t module = reinterpret_cast<uintptr_t>(g.module);
    if (!Readable(encounter + kEncounterAttributes, sizeof(uintptr_t)))
        return;
    const uintptr_t attributes = mem::Read<uintptr_t>(encounter + kEncounterAttributes);
    if (!attributes || (attributes >= module && attributes < module + 0x02000000) ||
        !Readable(attributes, 0x20))
        return;

    EncounterTotalBaseline* slot = nullptr;
    for (EncounterTotalBaseline& state : gEncounterTotals)
    {
        if (state.attributes == attributes) { slot = &state; break; }
    }
    if (!slot)
        for (EncounterTotalBaseline& state : gEncounterTotals)
            if (!state.attributes) { slot = &state; break; }
    if (!slot)
        return;
    if (slot->attributes != attributes)
    {
        const unsigned authored = mem::Read<unsigned>(attributes + kEncounterTotalToSpawn);
        if (!authored || authored > kEncounterTotalCeiling)
            return;                 // not a wave count we recognise
        slot->attributes = attributes;
        slot->total = authored;
    }

    const unsigned percent = MoreEnemiesPercentForPlayersImpl(playerCount);
    unsigned want = static_cast<unsigned>(
        (static_cast<unsigned long long>(slot->total) * percent + 50ull) / 100ull);
    if (!want) want = slot->total;
    if (want > kEncounterTotalCeiling) want = kEncounterTotalCeiling;
    // Confirmed on PC, not carried over: read live on 27.08.2026 the three
    // encounters of one quarantine reported 3, 2 and 5 here, and 5 is exactly
    // the wave the player had been counting all evening. This is the number
    // that decides how big a wave gets. The roster quota we spent the night
    // raising only caps how many of each type may appear.
    if (mem::Read<unsigned>(attributes + kEncounterTotalToSpawn) == want)
        return;
    if (!mem::Write<unsigned>(attributes + kEncounterTotalToSpawn, want))
        return;
    if (slot->appliedPercent != percent)
    {
        slot->appliedPercent = percent;
        Log("EnemySpawns: encounter wave size %u -> %u (%u%%) at %08X",
            slot->total, want, percent, static_cast<unsigned>(attributes));
    }
}

static void ScaleKnownEncounterQuotasImpl(unsigned playerCount)
{
    if (!cfg::fourPlayerDirectorQuotaScaling || !playerCount)
        return;
    for (unsigned i = 0; i < gEncounterCount; ++i)
    {
        const uintptr_t encounter = gEncounters[i];
        if (!encounter)
            continue;
        if (!Readable(encounter, sizeof(uintptr_t)) ||
            mem::Read<uintptr_t>(encounter) !=
                reinterpret_cast<uintptr_t>(g.module) + kEncounterVtableRva)
        {
            gEncounters[i] = 0;      // gone with the last level
            continue;
        }
        ScaleEncounterTotal(encounter, playerCount);
        if (!Readable(encounter + kEncounterRoster, sizeof(uintptr_t)))
            continue;
        // SafePtr again: it points 0x10 past the object it refers to.
        const uintptr_t handle = mem::Read<uintptr_t>(encounter + kEncounterRoster);
        if (handle <= 0x10)
            continue;                    // roster not assigned yet
        // A roster lives on the heap. Seen on 27.08.2026: an encounter whose
        // +0x70 still held level data sent this walk into the module image,
        // where it happily read 16 entries totalling 122 million and wrote
        // 588 back into static data. Anything inside our own image is not a
        // roster, full stop.
        const uintptr_t module = reinterpret_cast<uintptr_t>(g.module);
        const uintptr_t quota = handle - 0x10;
        if (quota >= module && quota < module + 0x02000000)
            continue;
        ApplyDynamicQuota(quota, playerCount);
    }
}

static bool IsQuotaAlreadyReported(uintptr_t quota)
{
    // A quota is queried every Director update. Do not let a read-only log
    // turn into per-frame work or hide the first useful line in noise.
    static uintptr_t seen[24]{};
    for (uintptr_t entry : seen)
        if (entry == quota)
            return true;

    for (uintptr_t& entry : seen)
    {
        if (!entry)
        {
            entry = quota;
            return false;
        }
    }
    return true;
}

static void ReportQuota(uintptr_t quota)
{
    if (!quota || !Readable(quota + kQuotaSpawnCount,
                            kQuotaEntries * sizeof(uint32_t)))
        return;

    const uintptr_t attributes = mem::Read<uintptr_t>(quota + kQuotaAttributes);
    constexpr size_t kAttributesSize = kQuotaEntries * kQuotaAttributeStride;
    if (!attributes || !Readable(attributes, kAttributesSize))
    {
        Log("FourPlayerSpawns: quota=%08X has unreadable attributes=%08X",
            static_cast<unsigned>(quota), static_cast<unsigned>(attributes));
        return;
    }

    const long line = InterlockedIncrement(&gQuotaLines);
    if (line > 24)
        return;

    const unsigned percent = cfg::fourPlayerSpawnCountPercent;
    const bool scale = percent > 100;
    unsigned totalOriginal = 0;
    unsigned totalTarget = 0;
    unsigned totalSpawned = 0;
    unsigned changed = 0;
    char entries[384]{};
    size_t used = 0;
    for (unsigned i = 0; i < kQuotaEntries; ++i)
    {
        const uintptr_t targetAddress =
            attributes + i * kQuotaAttributeStride + kQuotaTargetCount;
        const unsigned original = mem::Read<unsigned>(targetAddress);
        unsigned target = original;
        if (scale && original)
        {
            unsigned long long scaled =
                (static_cast<unsigned long long>(original) * percent + 50ull) / 100ull;
            if (scaled < original + 1ull)
                scaled = original + 1ull;
            if (scaled > kQuotaTargetCeiling)
                scaled = kQuotaTargetCeiling;
            target = static_cast<unsigned>(scaled);
            if (target != original && mem::Write<unsigned>(targetAddress, target))
                ++changed;
            else
                target = original;
        }
        const unsigned spawned = mem::Read<unsigned>(quota + kQuotaSpawnCount + i * 4);
        totalOriginal += original;
        totalTarget += target;
        totalSpawned += spawned;
        if (!target && !spawned)
            continue;

        const int written = _snprintf_s(entries + used, sizeof(entries) - used,
            _TRUNCATE, "%s%u:%u/%u%s", used ? " " : "", i, spawned, target,
            target != original ? "*" : "");
        if (written > 0)
            used += static_cast<size_t>(written);
    }

    Log("FourPlayerSpawns: quota=%08X attrs=%08X valid=%u total=%u -> %u spawned=%u "
        "changed=%u [%s]",
        static_cast<unsigned>(quota), static_cast<unsigned>(attributes),
        mem::Read<unsigned>(quota + kQuotaValidEntries), totalOriginal, totalTarget,
        totalSpawned, changed,
        used ? entries : "no active entries");
}

static void RosterSeen(const char* which)
{
    if (!cfg::fourPlayerSpawnDiagnostics)
        return;
    const long line = InterlockedIncrement(&gRosterLines);
    if (line > 24)
        return;
    Log("FourPlayerSpawns: AI Director roster being built: %s (#%ld). This "
        "level runs its waves through the Director.", which, line);
}

static void RosterFactory_Hook(safetyhook::Context&)      { RosterSeen("AIDirectorRoster"); }
static void RosterQuotaFactory_Hook(safetyhook::Context&) { RosterSeen("AIDirectorRosterQuota"); }



// Same trick the AIDirectorSettings probe uses: sit on the instruction right
// after the constructor call inside the factory, where EAX still holds the
// finished object. Waiting for the first quota read instead meant rewriting
// the numbers while an encounter was already running off them, and a
// quarantine that had begun spawning under the authored counts never reached
// its new target, and the door stayed shut.
static void RosterQuotaReady_Hook(safetyhook::Context& ctx)
{
    if (!cfg::fourPlayerDynamicDirectorScaling || !MayScaleSpawns())
        return;
    const unsigned playerCount = LivePlayerCount();
    if (playerCount)
        ApplyDynamicQuota(ctx.eax, playerCount);
}

// This is `AIDirectorRosterQuota::GetValidRosterEntries`. At the first
// instruction ECX is the quota object, and the method's own retail code uses
// exactly the fields reported above. No game function is called from here.
// With a percentage above 100, only the already-proven target-count fields are
// changed before the stock method reads them; every other quota field remains
// untouched.
static void RosterQuotaUse_Hook(safetyhook::Context& ctx)
{
    const uintptr_t quota = ctx.ecx;
    if (cfg::fourPlayerDynamicDirectorScaling)
    {
        // Enemy creation is host-authoritative. Leaving clients entirely
        // read-only avoids a local prediction copy being scaled a second time.
        if (!MayScaleSpawns())
            return;
        const unsigned playerCount = LivePlayerCount();
        if (!playerCount)
            return;
        ApplyDynamicDirectorSettings(playerCount);
        ApplyDynamicQuota(quota, playerCount);
        return;
    }

    if (cfg::fourPlayerSpawnDiagnostics && !IsQuotaAlreadyReported(quota))
        ReportQuota(quota);
}

// FuncSpawn already built this exact 0x24-byte snapshot before it called
// SimManager::SpawnEntity. Reading the local stack copy at the post-call hook
// is safer than calling GetSpawnInfo again while the original event is still
// on the stack. Layout comes from the retail debug menu's independently
// verified SimGetSpawnInfo ABI.
struct StockSpawnInfo
{
    uintptr_t instanceData;
    uintptr_t transform;
    uint32_t lifetime;
    uintptr_t uid;
    uintptr_t attributePacket;
    uintptr_t componentList;
    uintptr_t resourceList;
    uint32_t subId;
    uint32_t spawnFlags;
};
static_assert(sizeof(StockSpawnInfo) == 0x24);

static void LearnNativeRandomizerProfile(const StockSpawnInfo& info)
{
    if (!info.uid || !info.componentList ||
        !Readable(info.uid, 16) || !Readable(info.componentList, 8))
        return;
    const RandomizerTarget* target = FindRandomizerTarget(
        mem::Read<uint32_t>(info.uid + 0), mem::Read<uint32_t>(info.uid + 4),
        mem::Read<uint32_t>(info.uid + 8), mem::Read<uint32_t>(info.uid + 12));
    if (target)
        RememberNativeRandomizerProfile(*target,
            mem::Read<uint32_t>(info.componentList));
}

static void ReportScriptedSpawnEntity(const safetyhook::Context& ctx,
                                      uintptr_t spawned, int maxSpawn,
                                      uint32_t target0, uint32_t target1,
                                      uint32_t target2, uint32_t target3)
{
    if (!cfg::fourPlayerScriptedSpawnEntityDiagnostics || ctx.ebp < 0x84)
        return;

    const uintptr_t infoAddress = ctx.ebp - 0x84;
    if (!Readable(infoAddress, sizeof(StockSpawnInfo)))
    {
        Log("EnemySpawns: target=%08X-%08X-%08X-%08X SpawnInfo unreadable at %08X",
            target0, target1, target2, target3, static_cast<unsigned>(infoAddress));
        return;
    }

    const StockSpawnInfo info = mem::Read<StockSpawnInfo>(infoAddress);
    uint32_t source0 = 0, source1 = 0, source2 = 0, source3 = 0;
    uint32_t instanceUid32 = 0;
    uint32_t componentId = 0, componentCount = 0;
    const uintptr_t spawnedVtable = Readable(spawned, sizeof(uintptr_t))
        ? mem::Read<uintptr_t>(spawned) : 0;

    if (info.uid && Readable(info.uid, 16))
    {
        source0 = mem::Read<uint32_t>(info.uid + 0);
        source1 = mem::Read<uint32_t>(info.uid + 4);
        source2 = mem::Read<uint32_t>(info.uid + 8);
        source3 = mem::Read<uint32_t>(info.uid + 12);
    }
    if (info.instanceData && Readable(info.instanceData + 0x1C, sizeof(uint32_t)))
        instanceUid32 = mem::Read<uint32_t>(info.instanceData + 0x1C);
    if (info.componentList && Readable(info.componentList, 8))
    {
        componentId = mem::Read<uint32_t>(info.componentList + 0);
        componentCount = mem::Read<uint32_t>(info.componentList + 4);
    }

    Log("EnemySpawns: target=%08X-%08X-%08X-%08X max=%d spawned=%08X vtable=%08X "
        "source=%08X-%08X-%08X-%08X instance=%08X uid32=%08X component=%08X/%u "
        "attrs=%08X stream=%08X resources=%08X sub=%u flags=%08X",
        target0, target1, target2, target3, maxSpawn, static_cast<unsigned>(spawned),
        static_cast<unsigned>(spawnedVtable), source0, source1, source2, source3,
        static_cast<unsigned>(info.instanceData), instanceUid32, componentId,
        componentCount, static_cast<unsigned>(info.attributePacket), info.lifetime,
        static_cast<unsigned>(info.resourceList), info.subId, info.spawnFlags);
}

// This hook sits immediately after the stock call to
// SimManager::SpawnEntity inside FuncSpawn::HandleEvents. At this point EAX
// is the actual spawned Base*, EDI is the FuncSpawn object and the original
// attributes pointer is still in [EBP-0x14]. It consequently cannot mistake
// a rejected, queued or merely signalled FuncSpawn event for a real spawn.
// Dynamic mode uses only FuncSpawn's own QueueRequest retry event; it never
// constructs an entity or calls SimManager::SpawnEntity itself.
// The authored spawn has just succeeded and the game's re-entry flag at
// [esi+0x34] is still zero, which is exactly the state SpawnEntity expects.
// Calling it again here puts the extras in the world immediately instead of
// handing them to the retry queue, which only ever carries one request per
// spawner and therefore drips them in one death at a time.
//
// subId is bumped per copy. It is the packet's own discriminator, and reusing
// it would ask the engine to create the same entity twice.
static void BurstExtraSpawns(const safetyhook::Context& ctx, unsigned extras)
{
    if (!gSimSpawnEntity || !extras || !ctx.ebx ||
        ctx.ebp < 0x84 || !Readable(ctx.ebp - 0x84, sizeof(StockSpawnInfo)))
        return;

    StockSpawnInfo info = mem::Read<StockSpawnInfo>(ctx.ebp - 0x84);
    if (!info.instanceData || !info.transform ||
        !Readable(info.transform, sizeof(float) * 16))
        return;

    // Every copy needs its own spot. FuncSpawn::CheckBlocked tests a Havok
    // phantom at the spawn point and refuses while a body is standing there,
    // so a second spawn on the exact same transform is rejected outright -
    // which is why the first version of this produced nothing at all. The
    // offsets are a small ring around the authored point, in metres.
    static const float kRing[][2] = {
        { 1.8f, 0.0f }, { -1.8f, 0.0f }, { 0.0f, 1.8f }, { 0.0f, -1.8f },
        { 1.3f, 1.3f }, { -1.3f, 1.3f }, { 1.3f, -1.3f }, { -1.3f, -1.3f },
    };
    constexpr unsigned kRingCount = sizeof(kRing) / sizeof(kRing[0]);

    // Row 3 of the 4x4 carries the translation; SetTransformParams takes the
    // matrix in that shape. The buffer is static because the engine only
    // reads it inside the call below, and game logic runs on one thread.
    static float matrix[16];
    const uintptr_t original = info.transform;

    unsigned made = 0;
    for (unsigned i = 0; i < extras; ++i)
    {
        for (unsigned w = 0; w < 16; ++w)
            matrix[w] = mem::Read<float>(original + w * sizeof(float));
        matrix[12] += kRing[i % kRingCount][0];
        matrix[14] += kRing[i % kRingCount][1];

        info.transform = reinterpret_cast<uintptr_t>(matrix);
        info.subId += 1;
        if (!gSimSpawnEntity(reinterpret_cast<void*>(ctx.ebx), &info, 0x2000))
            break;
        ++made;
    }
    if (!made)
        return;

    InterlockedExchangeAdd(&gBurstSpawned, static_cast<long>(made));
    const long line = InterlockedIncrement(&gBurstLines);
    if (line <= 20)
        Log("EnemySpawns: burst %u extra enemies at once (total %ld)",
            made, gBurstSpawned);
}

static void FuncSpawnResult_Hook(safetyhook::Context& ctx)
{
    // The Director only runs its own update while an encounter is active, so
    // relying on it alone meant the quotas were still being scaled at the
    // moment the first encounter started, too late for that one. Level
    // spawns happen all the way through a chapter, which gets the numbers in
    // place long before the player reaches a quarantine.
    if (cfg::fourPlayerDirectorQuotaScaling && MayScaleSpawns())
    {
        const unsigned players = LivePlayerCount();
        if (players)
            ScaleKnownEncounterQuotasImpl(players);
    }

    // Restore before any diagnostic or scaling reads the authored target. The
    // spawned object was made from the replacement; everything after the
    // stock call must continue to see the level's original attributes.
    uintptr_t randomizerAttributes = 0;
    if (ctx.ebp >= 0x14 && Readable(ctx.ebp - 0x14, sizeof(uintptr_t)))
        randomizerAttributes = mem::Read<uintptr_t>(ctx.ebp - 0x14);

    const bool randomizerAttempt = randomizerAttributes &&
        gRandomizerAttempt.attributes == randomizerAttributes;
    if (randomizerAttributes)
        RestoreRandomizerTarget(randomizerAttributes);

    // This is deliberately recorded at the return point of the game's own
    // SpawnEntity call, not at the GUID replacement point.  A non-zero EAX
    // proves that the replacement survived SimManager resolution and created
    // a native entity; zero means that the engine rejected it before a model
    // could ever appear.
    if (randomizerAttempt)
    {
        if (ctx.eax)
            InterlockedIncrement(&gRandomizerSpawned);
        else
            InterlockedIncrement(&gRandomizerRejected);

        const long line = InterlockedIncrement(&gRandomizerResultLines);
        if (line <= 12 || RandomizerMayLogDiagnostic())
            Log("EnemyRandomizer result: %08X-%08X-%08X-%08X -> "
                "%08X-%08X-%08X-%08X (%s), SpawnEntity=%08X [%s]",
                   gRandomizerAttempt.original[0], gRandomizerAttempt.original[1],
                   gRandomizerAttempt.original[2], gRandomizerAttempt.original[3],
                   gRandomizerAttempt.replacement[0], gRandomizerAttempt.replacement[1],
                   gRandomizerAttempt.replacement[2], gRandomizerAttempt.replacement[3],
                   RandomizerContextName(gRandomizerAttempt.contextMask),
                   static_cast<unsigned>(ctx.eax), ctx.eax ? "native entity" : "REJECTED");
        gRandomizerAttempt = {};
    }

    // This tiny read happens once after a successful native spawn. It is
    // independent of diagnostics and turns future compatible replacements
    // into a zero-streaming GUID redirect.
    if (cfg::enemyRandomizer && ctx.eax && ctx.ebp >= 0x84 &&
        Readable(ctx.ebp - 0x84, sizeof(StockSpawnInfo)))
        LearnNativeRandomizerProfile(mem::Read<StockSpawnInfo>(ctx.ebp - 0x84));

    const bool needsSpawnProcessing =
        cfg::fourPlayerSpawnDiagnostics || cfg::fourPlayerDynamicScriptedSpawnScaling ||
        cfg::fourPlayerScriptedSpawnDiagnostics || cfg::fourPlayerScriptedSpawnEntityDiagnostics;
    if (!needsSpawnProcessing)
        return;

    // There used to be a hard `if (LocalIsHost() != 1) return;` here. That
    // silenced the client logs completely, so we could not even tell whether
    // the level spawn logic runs on a client at all.
    //
    // Clients still scale nothing: enemies belong to the host and are
    // replicated from there through the ghost system. An enemy created locally
    // would have no ghost entry and exist only on that one machine. It is
    // logged anyway so the question stays answered.
    const int hostAnswer = LocalIsHost();
    const bool istHost = MayScaleSpawns();

    const uintptr_t spawner = ctx.edi;
    const uintptr_t spawned = ctx.eax;
    if (!spawner || !spawned || ctx.ebp < 0x14 ||
        !Readable(spawner + kFuncSpawnSpawnedCount,
                  kFuncSpawnRetryMessage + 12 - kFuncSpawnSpawnedCount) ||
        !Readable(ctx.ebp - 0x14, sizeof(uintptr_t)))
        return;

    const uintptr_t attributes = mem::Read<uintptr_t>(ctx.ebp - 0x14);
    if (!attributes || !Readable(attributes + kFuncSpawnTargetGuid, 16) ||
        !Readable(attributes + kFuncSpawnPopThreshold, sizeof(uint32_t)))
        return;

    const uint32_t guid0 = mem::Read<uint32_t>(attributes + kFuncSpawnTargetGuid + 0);
    const uint32_t guid1 = mem::Read<uint32_t>(attributes + kFuncSpawnTargetGuid + 4);
    const uint32_t guid2 = mem::Read<uint32_t>(attributes + kFuncSpawnTargetGuid + 8);
    const uint32_t guid3 = mem::Read<uint32_t>(attributes + kFuncSpawnTargetGuid + 12);
    const int maxSpawn = mem::Read<int>(attributes + kFuncSpawnMaxSpawn);
    const unsigned spawnedBefore = mem::Read<unsigned>(spawner + kFuncSpawnSpawnedCount);
    const unsigned group = mem::Read<unsigned>(attributes + kFuncSpawnGroup);
    const unsigned options = mem::Read<unsigned>(attributes + kFuncSpawnOptions);
    const int popThreshold = mem::Read<int>(attributes + kFuncSpawnPopThreshold);
    const unsigned triggerMessage = mem::Read<unsigned>(attributes);
    const unsigned retryMessage = mem::Read<unsigned>(spawner + kFuncSpawnRetryMessage);
    const unsigned retryData0 = mem::Read<unsigned>(spawner + kFuncSpawnRetryMessage + 4);
    const unsigned retryData1 = mem::Read<unsigned>(spawner + kFuncSpawnRetryMessage + 8);
    const unsigned queued = mem::Read<unsigned>(spawner + kFuncSpawnQueuedCount);
    const unsigned retryPending = mem::Read<unsigned char>(spawner + kFuncSpawnRetryPending);
    const unsigned retryEventReady = retryMessage ? 1u : 0u;
    const AuthoredCombatTarget* combatTarget =
        FindAuthoredCombatTarget(guid0, guid1, guid2, guid3);
    const bool modQueuedRetry = ConsumeScriptedRetry(spawner);
    unsigned scheduled = 0;

    // Two independent pieces of evidence have to line up before anything is
    // scaled: either the older, live-confirmed binding catalogue or the corpus
    // table has to call the target an enemy, and the corpus table must not
    // explicitly contradict it. A target the table lists as a prop, a boss
    // stage or unknown is never scaled, even if the old catalogue has it.
    const SpawnTargetEntry* corpusTarget =
        FindSpawnTarget(guid0, guid1, guid2, guid3);
    const bool corpusSaysCombat =
        corpusTarget && corpusTarget->klass == SPAWN_CLASS_COMBAT;
    // A veto from the table covers props and boss stages, which must never be
    // multiplied. `UNRESOLVED` only means "we do not know", which is no reason
    // to block an enemy the player shot personally. Out of 427 collected
    // enemies the table lists two as unresolved, both demonstrably real
    // enemies, both permanently blocked. A kill is better evidence than a
    // table with half the coverage.
    const bool corpusHartesVeto =
        corpusTarget && (corpusTarget->klass == SPAWN_CLASS_NON_COMBAT ||
                         corpusTarget->klass == SPAWN_CLASS_SCRIPTED_STAGE);
    const bool corpusUnklar =
        corpusTarget && corpusTarget->klass == SPAWN_CLASS_UNRESOLVED;
    const unsigned typePercent =
        corpusSaysCombat ? ArchetypePercent(corpusTarget->archetype) : 100u;

    // Runtime check on the finished object. It opens up the cases that have no
    // edge in the level data without softening the static block: whatever the
    // table forbids explicitly stays forbidden.
    uintptr_t runtimeSlot = 0;
    uintptr_t runtimeVtable = 0;
    const RuntimeClass runtimeClass =
        ClassifySpawnedObject(spawned, runtimeSlot, runtimeVtable);
    const bool runtimeSaysCombat =
        cfg::fourPlayerRuntimeSpawnClass && runtimeClass == RUNTIME_COMBAT;

    // The criterion that holds up: has this vtable ever appeared as a damage
    // victim? Then it is demonstrably an enemy. Fourteen families ship with
    // the patch, the rest is learned in game.
    const bool provenDamageTaker =
        cfg::fourPlayerProvenSpawnClass && !cfg::fourPlayerOnlySelfDamaged &&
        g.module &&
        damagetakers::IsKnown(runtimeVtable, reinterpret_cast<uintptr_t>(g.module));

    // The precise permission: this exact spawner has been shot at before.
    // Unlike the vtable that covers one target rather than a whole class,
    // which is why it comes before the coarse check.
    //
    // Remember every target we see, so the damage path can tell whether a GUID
    // that got hit came from a spawner at all.
    damagetakers::NoteSpawnTarget(guid0, guid1, guid2, guid3);

    const bool learnedEnemy =
        cfg::fourPlayerProvenSpawnClass && cfg::fourPlayerExactSpawnerMatch &&
        damagetakers::GuidIsKnown(guid0, guid1, guid2, guid3);

    // Requiring `maxSpawn == -1` was dropped: it meant the normal case never
    // scaled, because the authored spawners are all `max=1`. Limited spawners
    // now get their cap raised; unlimited ones (`-1`) only need the retry as
    // before.
    int raisedMax = maxSpawn;
    bool maxRaisedNow = false;
    int maxOriginal = maxSpawn;

    if (istHost && cfg::fourPlayerDynamicScriptedSpawnScaling && !modQueuedRetry &&
        (combatTarget || corpusSaysCombat || runtimeSaysCombat ||
         provenDamageTaker || learnedEnemy) &&
        !corpusHartesVeto &&
        !(corpusUnklar && !learnedEnemy) &&
        typePercent > 0 && retryEventReady)
    {
        const unsigned playerCount = LivePlayerCount();

        if (maxSpawn > 0 && cfg::fourPlayerRaiseMaxSpawn)
        {
            // The full factor here, not just the surcharge: at 200% a max of
            // 1 should become 2, not stay at 1.
            unsigned percent = MoreEnemiesPercentForPlayersImpl(playerCount);
            if (typePercent != 100 && percent > 100)
                percent = 100 + (percent - 100) * typePercent / 100;
            raisedMax = RaiseMaxSpawn(attributes, maxSpawn, percent,
                                      maxRaisedNow, maxOriginal);
        }

        // Without raising the cap this keeps the proven behaviour: only
        // unlimited spawners get a retry. At `max=1` it would do nothing
        // anyway, because the spawner has already hit its limit.
        const unsigned requested = (maxSpawn == -1 || cfg::fourPlayerRaiseMaxSpawn)
            ? ExtraScriptedSpawnsForPlayers(playerCount, typePercent) : 0u;

        // Straight into the world rather than into the queue. QueueRequest
        // refuses a second request while one is still pending on the spawner
        // (it checks its own byte at +0xCC), so the queued path can only ever
        // add one enemy at a time and the rest of the loop below spins for
        // nothing. Bursting is what makes a fight feel bigger instead of
        // longer.
        if (cfg::fourPlayerScriptedSpawnBurst && requested)
        {
            unsigned room = requested;
            if (raisedMax > 0)
            {
                const int free = raisedMax - static_cast<int>(spawnedBefore + 1);
                room = free > 0 ? static_cast<unsigned>(free) : 0u;
                if (room > requested)
                    room = requested;
            }
            BurstExtraSpawns(ctx, room);
            scheduled += room;
        }

        const unsigned queued = cfg::fourPlayerScriptedSpawnBurst ? 0u : requested;
        for (unsigned i = 0; i < queued; ++i)
        {
            // A limited spawner gets only as many retries as its raised cap
            // can take; beyond that they would be discarded anyway.
            if (raisedMax > 0 &&
                static_cast<int>(spawnedBefore + 1 + scheduled) >= raisedMax)
                break;
            if (!QueueNativeScriptedRetry(spawner, attributes))
                break;
            ++scheduled;
        }
    }

    if (maxRaisedNow)
    {
        int popFrom = 0, popTo = 0;
        for (const MaxSpawnLedger& entry : gMaxSpawnLedger)
            if (entry.attributes == attributes)
            {
                popFrom = entry.popOriginal;
                popTo = entry.popApplied;
                break;
            }
        Log("EnemySpawns: maxSpawn angehoben %d -> %d, "
            "popThreshold %d -> %d (attributes=%08X)",
            maxOriginal, raisedMax, popFrom, popTo ? popTo : popFrom,
            static_cast<unsigned>(attributes));
    }

    // A large authored encounter can fire many individual FuncSpawn objects.
    // Keep diagnostic output bounded, but never let that cap suppress scaling.
    const long line = InterlockedIncrement(&gScriptedSpawnLines);
    if (line > static_cast<long>(cfg::fourPlayerScriptedSpawnLogLimit))
        return;

    ReportScriptedSpawnEntity(ctx, spawned, maxSpawn, guid0, guid1, guid2, guid3);

    // Flush the first lines straight away. Spawn lines arrive in bursts and
    // then not at all for a while, so the last ones would sit in the stdio
    // buffer and be missing exactly when someone looks.
    (line <= 8 ? LogNow : Log)(
        "EnemySpawns: [%s/%uSp] #%ld FuncSpawn=%08X spawned=%08X target=%08X-%08X-%08X-%08X "
        "combat=%s korpus=%s art=%s artPct=%u laufzeit=%s vtable=%08X slot=%08X %s countBefore=%u max=%d group=%08X "
        "options=%08X popThreshold=%d attrs=%08X maxNow=%d event=%08X retry=%08X/%08X/%08X retryEventReady=%u "
        "retryEnabled=%u queued=%u pending=%u modRetry=%u scheduled=%u",
        hostAnswer == 1 ? "HOST" : hostAnswer == 0 ? "CLIENT" : "OHNE-SITZUNG",
        LivePlayerCount(),
        line, static_cast<unsigned>(spawner), static_cast<unsigned>(spawned),
        guid0, guid1, guid2, guid3,
        combatTarget ? combatTarget->displayName : "UNKNOWN",
        corpusTarget ? SpawnClassName(corpusTarget->klass) : "not in table",
        corpusSaysCombat ? SpawnArchetypeName(corpusTarget->archetype) : "-",
        typePercent,
        RuntimeClassName(runtimeClass), static_cast<unsigned>(runtimeVtable),
        static_cast<unsigned>(runtimeSlot),
        corpusHartesVeto ? "GESPERRT(Korpus)"
            : (corpusUnklar && !learnedEnemy) ? "GESPERRT(Korpus unklar)"
            : (corpusSaysCombat && typePercent == 0) ? "GESPERRT(INI-Art=0)"
            : (!combatTarget && !corpusSaysCombat && !runtimeSaysCombat &&
               !provenDamageTaker && !learnedEnemy) ? "GESPERRT(unbelegt)"
            : learnedEnemy ? "allowed(self-damaged)"
            : provenDamageTaker ? "erlaubt(Schadensbeleg)" : "erlaubt",
        spawnedBefore, maxSpawn, group, options, popThreshold,
        static_cast<unsigned>(attributes), raisedMax, triggerMessage,
        retryMessage, retryData0, retryData1, retryEventReady,
        (options & kFuncSpawnRetryOption) ? 1u : 0u, queued, retryPending,
        modQueuedRetry ? 1u : 0u, scheduled);
}

} // namespace

void ScaleEncounterNow(uintptr_t encounter, unsigned playerCount)
{
    if (!cfg::fourPlayerDirectorQuotaScaling || !encounter || !playerCount)
        return;
    if (!Readable(encounter, sizeof(uintptr_t)) ||
        mem::Read<uintptr_t>(encounter) !=
            reinterpret_cast<uintptr_t>(g.module) + kEncounterVtableRva)
        return;
    ScaleEncounterTotal(encounter, playerCount);
    if (!Readable(encounter + kEncounterRoster, sizeof(uintptr_t)))
        return;
    const uintptr_t handle = mem::Read<uintptr_t>(encounter + kEncounterRoster);
    if (handle <= 0x10)
        return;
    const uintptr_t module = reinterpret_cast<uintptr_t>(g.module);
    const uintptr_t quota = handle - 0x10;
    if (quota >= module && quota < module + 0x02000000)
        return;
    ApplyDynamicQuota(quota, playerCount);
}

void NoteDirectorEncounter(uintptr_t encounter)
{
    if (!encounter)
        return;
    for (unsigned i = 0; i < gEncounterCount; ++i)
        if (gEncounters[i] == encounter)
            return;
    // Stale entries are zeroed as levels come and go, so reuse a free slot
    // before growing. Without this the table filled up after a few loads and
    // silently stopped taking new encounters.
    for (unsigned i = 0; i < gEncounterCount; ++i)
        if (!gEncounters[i]) { gEncounters[i] = encounter; return; }
    if (gEncounterCount < kMaxEncounters)
        gEncounters[gEncounterCount++] = encounter;
}

void ScaleKnownEncounterQuotas(unsigned playerCount)
{
    ScaleKnownEncounterQuotasImpl(playerCount);

    // Says how many of the remembered encounters actually have their roster
    // yet. If this stays at zero the pointer at +0x70 is filled in later than
    // the constructor, and the roster has to be caught somewhere else.
    static unsigned lastReady = 0xFFFFFFFF;
    unsigned ready = 0;
    for (unsigned i = 0; i < gEncounterCount; ++i)
        if (gEncounters[i] && Readable(gEncounters[i] + kEncounterRoster, 4) &&
            mem::Read<uintptr_t>(gEncounters[i] + kEncounterRoster) > 0x10)
            ++ready;
    if (ready != lastReady)
    {
        lastReady = ready;
        Log("EnemySpawns: %u of %u encounters have a roster",
            ready, gEncounterCount);
    }
}

unsigned MoreEnemiesPercentForPlayers(unsigned playerCount)
{
    return MoreEnemiesPercentForPlayersImpl(playerCount);
}

void ApplyFourPlayerSpawnDiagnostics()
{
    if (!cfg::fourPlayerSpawnDiagnostics && !cfg::fourPlayerDynamicDirectorScaling &&
        !cfg::fourPlayerScriptedSpawnDiagnostics &&
        !cfg::fourPlayerScriptedSpawnEntityDiagnostics &&
        !cfg::fourPlayerDynamicScriptedSpawnScaling && !cfg::enemyRandomizer)
        return;

    if (cfg::fourPlayerDynamicScriptedSpawnScaling)
    {
        // FuncSpawn::QueueRequest is identified by its exact retry-option
        // gate, its +0xC8 counter increment and the engine queue dispatch of
        // the CMsg at +0xD0. It is called only from the successful FuncSpawn
        // result hook above, on the host.
        const DWORD queueRequest = Find(
            "56 8B F1 8B 46 0C F6 40 30 08 74 ?? 80 BE CC 00 00 00 00 75 ?? "
            "D9 05 ?? ?? ?? ?? 33 C0 50 50 51 D9 1C 24 56 8D 8E D0 00 00 00 "
            "51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 86 CC 00 00 00 01 FF 86 "
            "C8 00 00 00 5E C3",
            "FourPlayerScriptedSpawns FuncSpawn QueueRequest");
        if (queueRequest)
            gFuncSpawnQueueRequest =
                reinterpret_cast<FuncSpawnQueueRequestFn>(queueRequest);
    }

    struct Probe { const char* sig; const char* name; safetyhook::MidHookFn hook;
                   safetyhook::MidHook* slot; };
    const Probe probes[4] = {
        { "55 8B EC 6A FF 68 C1 DE CC 00 64 A1 00 00 00 00 50 64 89 25 "
          "00 00 00 00 51 8B 45 0C 50 6A 60 E8 FC 5E 68 00",
          "FourPlayerSpawns AIDirectorRoster factory",
          &RosterFactory_Hook, &gRosterFactory },
        { "55 8B EC 6A FF 68 C1 DE CC 00 64 A1 00 00 00 00 50 64 89 25 "
          "00 00 00 00 51 8B 45 0C 50 68 1C 01 00 00 E8 99 5E 68 00",
          "FourPlayerSpawns AIDirectorRosterQuota factory",
          &RosterQuotaFactory_Hook, &gRosterQuotaFactory },
        { "55 8B EC 6A FF 68 C0 F6 CD 00 64 A1 ?? ?? ?? ?? 50 64 89 25 "
          "?? ?? ?? ?? 83 EC 4C 53 56 8B F1 8B 46 0C 57 33 FF",
          "FourPlayerSpawns quota target reader",
          &RosterQuotaUse_Hook, &gRosterQuotaUse },
        // PC retail FuncSpawn::HandleEvents: use the first instruction after
        // SpawnEntity. The fixed 0x2000 call flag and the stream-pool flag
        // bracket make this site unique without relying on a PC code address.
        { "8B 35 ?? ?? ?? ?? 68 00 20 00 00 8D 85 7C FF FF FF 50 8B CB "
          "C6 46 34 00 E8 ?? ?? ?? ?? 8B D8 89 5D F0 C6 46 34 01 85 DB",
          "FourPlayerScriptedSpawns FuncSpawn successful SpawnEntity",
          &FuncSpawnResult_Hook, &gFuncSpawnResult },
    };

    unsigned installed = 0;
    DWORD funcSpawnSignature = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        DWORD site = Find(probes[i].sig, probes[i].name);
        if (!site)
            continue;
        // The FuncSpawn signature begins at the StreamManager load. Its
        // post-SpawnEntity result instruction is 0x1D bytes later.
        if (probes[i].slot == &gFuncSpawnResult)
        {
            funcSpawnSignature = site;
            // The call sits 0x18 bytes in; its rel32 operand follows the E8.
            const uintptr_t call = site + 0x18;
            if (mem::Read<uint8_t>(call) == 0xE8)
                gSimSpawnEntity = reinterpret_cast<SimSpawnEntityFn>(
                    call + 5 + mem::Read<int32_t>(call + 1));
            site += 0x1D;
        }
        // The factory signature starts at the function entry; the instruction
        // after the constructor call is 0x43 bytes in. Derived from the retail
        // code, which mirrors the AIDirectorSettings factory byte for byte.
        if (probes[i].slot == &gRosterQuotaFactory)
        {
            gRosterQuotaReady = safetyhook::create_mid(
                reinterpret_cast<void*>(site + 0x43), &RosterQuotaReady_Hook);
            Log("EnemySpawns: quota scaling at construction %s",
                gRosterQuotaReady ? "active" : "FAILED");
        }
        *probes[i].slot = safetyhook::create_mid(reinterpret_cast<void*>(site), probes[i].hook);
        if (*probes[i].slot)
            ++installed;
    }

    if (!installed)
    {
        Log("EnemySpawns: NOT installed");
        return;
    }
    Log("EnemySpawns: %u of 4 spawn probes active", installed);

    if (cfg::enemyRandomizer)
    {
        if (!funcSpawnSignature)
        {
            Log("EnemyRandomizer: disabled (FuncSpawn signature missing)");
        }
        else
        {
            // The signature begins immediately after GetSpawnInfo. The
            // matching FuncSpawn object is still in ESI at -0x17, just before
            // its authored GUID is handed to GetSpawnInfo. The resolver copies
            // that GUID into a local SpawnInfo packet, so restore at +0x00 is
            // deliberately before the later SpawnEntity call.
            gFuncSpawnPrepare = safetyhook::create_mid(
                reinterpret_cast<void*>(funcSpawnSignature - 0x17),
                &FuncSpawnPrepare_Hook);
            gFuncSpawnGuidRestore = safetyhook::create_mid(
                reinterpret_cast<void*>(funcSpawnSignature),
                &FuncSpawnGuidRestore_Hook);
            Log("EnemyRandomizer: %u authored targets, live-session resolver hook %s",
                static_cast<unsigned>(sizeof(kRandomizerTargets) / sizeof(kRandomizerTargets[0])),
                (gFuncSpawnPrepare && gFuncSpawnGuidRestore) ? "active" : "FAILED");
            if (cfg::enemyRandomizerDiagnostics)
                Log("EnemyRandomizer diagnostics: active, max %u decision lines; "
                    "buffered logging, no gameplay writes beyond the temporary GUID swap.",
                    cfg::enemyRandomizerLogLimit);
            if (cfg::enemyRandomizerGlobalTemplates)
                InstallStreamedTemplateLoader();
        }
    }

    // Report the block table once at startup so the log shows it loaded and
    // how much it covers.
    {
        unsigned combat = 0, blocked = 0;
        for (const SpawnTargetEntry& e : kSpawnTargets)
            (e.klass == SPAWN_CLASS_COMBAT ? combat : blocked)++;
        Log("EnemySpawns: spawn target table loaded: %u targets, %u enemies, "
            "%u blocked (props/boss stage/unknown), %u enemy types",
            static_cast<unsigned>(sizeof(kSpawnTargets) / sizeof(kSpawnTargets[0])),
            combat, blocked, kSpawnArchetypeCount);

        // Only name the types that differ from 100, otherwise it is noise.
        for (unsigned i = 0; i < kSpawnArchetypeCount; ++i)
        {
            const unsigned pct = cfg::fourPlayerSpawnTypePercent[i];
            if (pct != 100)
                Log("FourPlayerSpawns:   Art '%s' auf %u%%%s",
                    kSpawnArchetypes[i], pct, pct == 0 ? " (nie skalieren)" : "");
        }
    }
    if (cfg::fourPlayerDynamicDirectorScaling)
        // Report the curve that is actually used, not just the co-op half.
        // The old text claimed it started at three players and left the solo
        // case out entirely, which reads like the switch does nothing solo.
        Log("EnemySpawns: Director scaling active (HOST only), same curve "
            "as the spawn points: 1 player=%u%%, %u=100%%, +%u%% per player "
            "above that; quota cap=%u",
            cfg::fourPlayerScriptedSpawnSoloTestPercent >= 100
                ? cfg::fourPlayerScriptedSpawnSoloTestPercent : 100u,
            cfg::spawnBasePlayers, cfg::spawnPercentPerPlayer,
            kQuotaTargetCeiling);
    else if (cfg::fourPlayerSpawnCountPercent > 100)
        Log("FourPlayerSpawns: Director-Quota-Scaling AKTIV: %u%%, pro Eintrag max. %u",
            cfg::fourPlayerSpawnCountPercent, kQuotaTargetCeiling);
    else
        Log("EnemySpawns: read-only. SpawnCountPercent=%u, nothing is changed.",
            cfg::fourPlayerSpawnCountPercent);
    if (cfg::fourPlayerScriptedSpawnDiagnostics)
        Log("EnemySpawns: host-only read-only tracer active; "
            "logs genuine FuncSpawn successes, max %u per process.",
            cfg::fourPlayerScriptedSpawnLogLimit);
    if (cfg::fourPlayerScriptedSpawnEntityDiagnostics)
        Log("EnemySpawns: read-only FuncSpawn SpawnInfo tracer active; "
            "all successful scripted spawns, max %u per process; no engine calls and no game-memory writes.",
            cfg::fourPlayerScriptedSpawnLogLimit);
    if (cfg::fourPlayerDynamicScriptedSpawnScaling)
    {
        if (gFuncSpawnQueueRequest)
            Log("EnemySpawns: DYNAMIC host-only native retry scaling active; "
                "3=150%%, 4=200%%, +50%% per additional player; recognised combat targets only.");
        else
            Log("EnemySpawns: dynamic scaling DISABLED; QueueRequest signature missing.");
    }
}
