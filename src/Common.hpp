#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <string_view>

#include "../include/safetyhook/safetyhook.hpp"

// Physics is authored for 30 FPS; the Havok fix scales against this.
constexpr float kTargetFrameTime = 1.0f / 30.0f;

// ---------------------------------------------------------------------------
// Vtables of objects that have demonstrably taken damage.
//
// Whether a spawn target is an enemy cannot be told from the level data (only
// 59 of the runtime targets have a CMD_SetSpawnTarget edge) nor from vtable
// slot +0x260, which most NPC classes do not override at all.
//
// What does work: eight of ten spawned object pointers turned up again later
// as damage victims, the same pointer. If it takes damage, it is an enemy.
// The damage path records every victim vtable here, the spawn hook asks.
namespace damagetakers
{
    // Stored as RVAs so nothing depends on a fixed load address.
    //
    // Each enemy type has exactly one vtable, so the vtable is the family.
    // This list ships with the patch so scaling works from the first launch
    // and nobody has to shoot hundreds of enemies first.
    //
    // Every entry was checked against the corpus table; only families listed
    // there exclusively as enemies made the cut. Deliberately left out:
    //   0x00978C88  boss_snow_beast, a boss family. It was in the learned
    //               list because the boss had been shot at. Shipping it would
    //               have multiplied boss stages for everyone.
    //   0x00935BE8  mixed: 18 enemies, but 8 props and 4 unclear.
    //   0x00934E30, 0x00936470, 0x00972490  no evidence at all (24 collected
    //               enemies, none of them resolvable in the corpus).
    //
    // Slasher and fodder are borderline (56/5 and 35/4). They are in because
    // they are the core of the feature and their props are known to the
    // corpus, and the hard corpus veto runs before this check anyway.
    constexpr uintptr_t kKnownRvas[] = {
        0x0092F680,   // slasher, slasher_berserk, slasher_aggro, super_slasher
        0x009303F0,   // fodder
        0x00933668,   // feeder, super_feeder
        0x009AF370,   // lurker, super_lurker
        0x00931BA8,   // swarm_infector
        0x009B16D8,   // hunter
        0x009AECD8,   // leaper
        0x0092EED0,   // puker
        0x0092C588,   // exploder
        0x0092FD18,   // divider_head
        0x009323B0,   // alien_crawler
        0x009AFF30,   // alien_necromorph
        0x009B1C70,   // pregnant
        0x00930F38,   // guardian
    };

    constexpr unsigned kCapacity = 256;
    inline volatile LONG count = 0;
    inline uintptr_t vtables[kCapacity]{};

    // Without a file the list would start out nearly empty on every launch and
    // the first enemy of each class would never be scaled. RVAs are stored
    // rather than load addresses, so the file survives the module landing
    // somewhere else.
    //
    // It is written the moment something is learned, not on shutdown: DS3
    // crashes often and no cleanup code runs on a crash. That costs one short
    // file access, but only the very first time a class appears, so a few dozen
    // times per installation, not per hit.
    inline char storePath[MAX_PATH]{};
    inline volatile LONG loaded = 0;

    inline const char* StorePath()
    {
        if (!storePath[0])
        {
            GetModuleFileNameA(nullptr, storePath, MAX_PATH);
            char* slash = std::strrchr(storePath, '\\');
            if (!slash)
                slash = std::strrchr(storePath, '/');
            if (slash)
                slash[1] = '\0';
            // English name from now on. If only the old German file is
            // there, keep using it so nobody loses what they collected.
            const size_t dirLen = std::strlen(storePath);
            std::strncat(storePath, "BrethrenPatch_EnemyClasses.txt",
                         MAX_PATH - dirLen - 1);
            if (GetFileAttributesA(storePath) == INVALID_FILE_ATTRIBUTES)
            {
                char legacy[MAX_PATH];
                std::strncpy(legacy, storePath, dirLen);
                legacy[dirLen] = '\0';
                std::strncat(legacy, "BrethrenPatch_Gegnerklassen.txt",
                             MAX_PATH - dirLen - 1);
                if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
                    std::strncpy(storePath, legacy, MAX_PATH);
            }
        }
        return storePath;
    }

    inline bool Contains(uintptr_t vtable)
    {
        const LONG have = InterlockedCompareExchange(&count, 0, 0);
        for (LONG i = 0; i < have; ++i)
            if (vtables[i] == vtable)
                return true;
        return false;
    }

    inline bool Add(uintptr_t vtable)
    {
        if (!vtable || Contains(vtable))
            return false;
        const LONG slot = InterlockedIncrement(&count) - 1;
        if (slot >= static_cast<LONG>(kCapacity))
        {
            InterlockedDecrement(&count);
            return false;
        }
        vtables[slot] = vtable;
        return true;
    }

    inline void Load(uintptr_t moduleBase)
    {
        if (InterlockedExchange(&loaded, 1))
            return;
        FILE* f = nullptr;
        if (fopen_s(&f, StorePath(), "r") != 0 || !f)
            return;
        char line[64];
        while (std::fgets(line, sizeof(line), f))
        {
            unsigned rva = 0;
            if (std::sscanf(line, "%x", &rva) == 1 && rva)
                Add(moduleBase + rva);
        }
        std::fclose(f);
    }

    // -- Class names ----------------------------------------------------
    //
    // Nobody can check a list of bare RVAs, so each entry gets a class name to
    // make what was learned visible.
    //
    // RTTI is no help: all 457 type descriptors in the exe come from Havok,
    // the STL and the Origin SDK. The game classes themselves are compiled
    // without it.
    //
    // Nothing is guessed even so. `GetClassDesc` is a two-liner in this
    // engine: `mov eax, <ClassDesc>` followed by `ret`, i.e. the bytes
    // B8 xx xx xx xx C3. The pattern is unambiguous inside a vtable and the
    // immediate is the class description. The name is then found by scanning
    // the leading fields of that description for a pointer to a plausible
    // string. No offset is assumed; if nothing is found the entry stays
    // unnamed rather than wrongly named.
    constexpr uintptr_t kModuleSpan = 0x01700000;

    inline bool ReadableRange(uintptr_t address, size_t size)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info,
                          sizeof(info)) ||
            info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const uintptr_t end =
            reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return address <= end && size <= end - address;
    }

    // A class name is letters, digits, underscore and colon, starting with a
    // letter. Anything else is noise in memory and gets discarded.
    inline bool PlausibleName(const char* s)
    {
        size_t n = 0;
        while (n < 64 && s[n])
        {
            const char c = s[n];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == ':';
            if (!ok)
                return false;
            ++n;
        }
        if (n < 3 || n >= 64)
            return false;
        return (s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z');
    }

    inline uintptr_t FindClassDesc(uintptr_t vtable, uintptr_t moduleBase)
    {
        if (!vtable || !moduleBase)
            return 0;
        for (unsigned slot = 0; slot < 256; ++slot)
        {
            const uintptr_t at = vtable + slot * sizeof(uintptr_t);
            if (!ReadableRange(at, sizeof(uintptr_t)))
                break;
            const uintptr_t fn = *reinterpret_cast<uintptr_t*>(at);
            if (fn <= moduleBase || fn >= moduleBase + kModuleSpan)
                continue;
            if (!ReadableRange(fn, 6))
                continue;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(fn);
            if (p[0] != 0xB8 || p[5] != 0xC3)   // mov eax, imm32 ; ret
                continue;
            uintptr_t desc = 0;
            std::memcpy(&desc, p + 1, sizeof(uint32_t));
            if (desc > moduleBase && desc < moduleBase + kModuleSpan &&
                ReadableRange(desc, 0x80))
                return desc;
        }
        return 0;
    }

    // `outOffset` reports which field of the description held the name. If
    // that comes out the same across many classes, the field is confirmed and
    // the search can be pinned to it later.
    inline bool DescribeClass(uintptr_t vtable, uintptr_t moduleBase,
                              char* out, size_t outSize, unsigned& outOffset)
    {
        outOffset = 0xFFFFFFFFu;
        if (out && outSize)
            out[0] = '\0';

        const uintptr_t desc = FindClassDesc(vtable, moduleBase);
        if (!desc)
            return false;

        for (unsigned off = 0; off < 0x60; off += sizeof(uintptr_t))
        {
            if (!ReadableRange(desc + off, sizeof(uintptr_t)))
                break;
            const uintptr_t str = *reinterpret_cast<uintptr_t*>(desc + off);
            if (!str || !ReadableRange(str, 64))
                continue;
            const char* s = reinterpret_cast<const char*>(str);
            if (!PlausibleName(s))
                continue;
            outOffset = off;
            if (out && outSize)
            {
                std::strncpy(out, s, outSize - 1);
                out[outSize - 1] = '\0';
            }
            return true;
        }
        return false;
    }

    // Called from the damage path: this Sentient demonstrably took damage, so
    // its class is confirmed. Returns true only if the class really was new,
    // which is the only case worth a log line. `outName` and `outOffset`
    // describe the class as far as it could be named.
    inline bool Remember(uintptr_t vtable, uintptr_t moduleBase,
                         char* outName = nullptr, size_t outNameSize = 0,
                         unsigned* outOffset = nullptr)
    {
        if (!Add(vtable))
            return false;               // empty or already known

        char name[64] = {};
        unsigned off = 0xFFFFFFFFu;
        const bool benannt = DescribeClass(vtable, moduleBase, name,
                                           sizeof(name), off);
        if (outName && outNameSize)
        {
            std::strncpy(outName, benannt ? name : "?", outNameSize - 1);
            outName[outNameSize - 1] = 0;
        }
        if (outOffset)
            *outOffset = off;

        // Appended as an RVA. A vtable outside the module is used in memory
        // but not saved, because the RVA would be meaningless.
        if (!moduleBase || vtable <= moduleBase)
            return true;
        FILE* f = nullptr;
        if (fopen_s(&f, StorePath(), "a") != 0 || !f)
            return true;
        // The name goes after a semicolon. `Load` reads with %x and stops at
        // the space, so the file stays readable for the patch and for you.
        std::fprintf(f, "%08X ; %s\n", static_cast<unsigned>(vtable - moduleBase),
                     benannt ? name : "unbenannt");
        std::fclose(f);
        return true;
    }

    // -- Learned entity GUIDs --------------------------------------------
    //
    // More precise than the vtable: every Sentient that gets hit carries its
    // own entity GUID at +0x24, in exactly the format the spawn hook already
    // reads out of the spawner attributes. Confirmed on object 09A03E20:
    //
    //     obj+0x24 = 45CDAD96  825A905F  4A315357  48475548
    //
    // and the same GUID appeared as `target=` in the spawn log that run.
    //
    // The vtable is a poor identifier: only three distinct ones showed up
    // across the whole test area, even though several enemy types spawn
    // there. The GUID separates every individual spawner. That makes "I shoot
    // it, so scale it" mean literally what it says, and a prop hit by
    // accident only unlocks itself, not a whole class.
    //
    // Class names are not available this way: game classes have no RTTI and
    // the object header holds no string pointer at all. The GUID is the
    // substitute: clumsier to read, but exact.
    //
    // The cap is generous on purpose: a long session across many chapters
    // should not run into it. 4096 GUIDs cost 64 KB.
    constexpr unsigned kGuidCapacity = 4096;
    inline volatile LONG guidCount = 0;
    // Set when the list is full; the damage path reports it.
    inline volatile bool voll = false;
    inline uint32_t guids[kGuidCapacity][4]{};

    inline bool GuidContains(const uint32_t g[4])
    {
        const LONG have = InterlockedCompareExchange(&guidCount, 0, 0);
        for (LONG i = 0; i < have; ++i)
            if (guids[i][0] == g[0] && guids[i][1] == g[1] &&
                guids[i][2] == g[2] && guids[i][3] == g[3])
                return true;
        return false;
    }

    inline bool GuidAdd(const uint32_t g[4])
    {
        // A GUID of all zeroes is not a GUID.
        if (!(g[0] | g[1] | g[2] | g[3]))
            return false;
        if (GuidContains(g))
            return false;
        const LONG slot = InterlockedIncrement(&guidCount) - 1;
        if (slot >= static_cast<LONG>(kGuidCapacity))
        {
            InterlockedDecrement(&guidCount);
            // A silent overflow would be the worst case, because the session would
            // carry on looking like it was still learning. Say it once.
            static volatile LONG gemeldet = 0;
            if (!InterlockedExchange(&gemeldet, 1))
                voll = true;
            return false;
        }
        for (int i = 0; i < 4; ++i)
            guids[slot][i] = g[i];
        return true;
    }

    inline const char* GuidStorePath()
    {
        static char path[MAX_PATH]{};
        if (!path[0])
        {
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            char* slash = std::strrchr(path, '\\');
            if (!slash)
                slash = std::strrchr(path, '/');
            if (slash)
                slash[1] = '\0';
            const size_t dirLen = std::strlen(path);
            std::strncat(path, "BrethrenPatch_Enemies.txt",
                         MAX_PATH - dirLen - 1);
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            {
                char legacy[MAX_PATH];
                std::strncpy(legacy, path, dirLen);
                legacy[dirLen] = '\0';
                std::strncat(legacy, "BrethrenPatch_Gegner.txt",
                             MAX_PATH - dirLen - 1);
                if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
                    std::strncpy(path, legacy, MAX_PATH);
            }
        }
        return path;
    }

    inline void GuidLoad()
    {
        static volatile LONG once = 0;
        if (InterlockedExchange(&once, 1))
            return;
        FILE* f = nullptr;
        if (fopen_s(&f, GuidStorePath(), "r") != 0 || !f)
            return;
        char zeile[512];
        while (std::fgets(zeile, sizeof(zeile), f))
        {
            if (zeile[0] == ';' || zeile[0] == '#' || zeile[0] == '\n')
                continue;               // ausgeklammert - bewusst uebergangen
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (std::sscanf(zeile, "%x-%x-%x-%x", &a, &b, &c, &d) == 4)
            {
                const uint32_t g[4] = { a, b, c, d };
                GuidAdd(g);
            }
        }
        std::fclose(f);
    }

    // From the damage path. Returns true if the GUID had not been seen.
    inline bool GuidRemember(const uint32_t g[4], const char* notiz)
    {
        if (!GuidAdd(g))
            return false;
        FILE* f = nullptr;
        if (fopen_s(&f, GuidStorePath(), "a") != 0 || !f)
            return true;
        // A semicolon at the start of a line disables an entry, so a prop hit
        // by accident can be removed without losing the line.
        std::fprintf(f, "%08X-%08X-%08X-%08X ; %s\n", g[0], g[1], g[2], g[3],
                     notiz ? notiz : "");
        std::fclose(f);
        return true;
    }

    // -- Which GUIDs were ever a spawn target ----------------------------
    //
    // A crate in the learned list can do no harm: the list is only consulted
    // when a spawner fires and brings its target GUID along. Props are placed
    // in the level and are never a spawner's target, so their GUID is never
    // looked up.
    //
    // It should still be visible. The spawn hook records every target GUID it
    // sees here, and the damage path notes on learning whether that GUID has
    // ever turned up as a spawn target.
    constexpr unsigned kSeenCapacity = 4096;
    inline volatile LONG seenCount = 0;
    inline uint32_t seenTargets[kSeenCapacity][4]{};

    inline void NoteSpawnTarget(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        if (!(a | b | c | d))
            return;
        const LONG have = InterlockedCompareExchange(&seenCount, 0, 0);
        for (LONG i = 0; i < have; ++i)
            if (seenTargets[i][0] == a && seenTargets[i][1] == b &&
                seenTargets[i][2] == c && seenTargets[i][3] == d)
                return;
        const LONG slot = InterlockedIncrement(&seenCount) - 1;
        if (slot >= static_cast<LONG>(kSeenCapacity))
        {
            InterlockedDecrement(&seenCount);
            return;
        }
        seenTargets[slot][0] = a; seenTargets[slot][1] = b;
        seenTargets[slot][2] = c; seenTargets[slot][3] = d;
    }

    inline bool WasSpawnTarget(const uint32_t g[4])
    {
        const LONG have = InterlockedCompareExchange(&seenCount, 0, 0);
        for (LONG i = 0; i < have; ++i)
            if (seenTargets[i][0] == g[0] && seenTargets[i][1] == g[1] &&
                seenTargets[i][2] == g[2] && seenTargets[i][3] == g[3])
                return true;
        return false;
    }

    // -- Fingerprints ----------------------------------------------------
    //
    // The GUID names the individual spawner, not the enemy type: the same snow
    // slasher from another corner is a different GUID. What we want is a field
    // that is identical across every enemy of the same type.
    //
    // Note this records every enemy that gets hit, including long-known ones.
    // The first version was tied to learning and therefore produced nothing
    // once the list was full enough to stop learning, which is the normal
    // case after a few hours of collecting. Once per GUID per session, into
    // its own file.
    constexpr unsigned kFpCapacity = 2048;
    inline volatile LONG fpCount = 0;
    inline uint32_t fpSeen[kFpCapacity][4]{};

    inline const char* FpStorePath()
    {
        static char path[MAX_PATH]{};
        if (!path[0])
        {
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            char* slash = std::strrchr(path, '\\');
            if (!slash)
                slash = std::strrchr(path, '/');
            if (slash)
                slash[1] = '\0';
            std::strncat(path, "BrethrenPatch_Fingerabdruck.txt",
                         MAX_PATH - std::strlen(path) - 1);
        }
        return path;
    }

    inline bool FingerprintOnce(const uint32_t g[4], const char* text)
    {
        if (!(g[0] | g[1] | g[2] | g[3]))
            return false;
        const LONG have = InterlockedCompareExchange(&fpCount, 0, 0);
        for (LONG i = 0; i < have; ++i)
            if (fpSeen[i][0] == g[0] && fpSeen[i][1] == g[1] &&
                fpSeen[i][2] == g[2] && fpSeen[i][3] == g[3])
                return false;
        const LONG slot = InterlockedIncrement(&fpCount) - 1;
        if (slot >= static_cast<LONG>(kFpCapacity))
        {
            InterlockedDecrement(&fpCount);
            return false;
        }
        for (int i = 0; i < 4; ++i)
            fpSeen[slot][i] = g[i];

        FILE* f = nullptr;
        if (fopen_s(&f, FpStorePath(), "a") != 0 || !f)
            return true;
        std::fprintf(f, "%08X-%08X-%08X-%08X ; %s\n",
                     g[0], g[1], g[2], g[3], text ? text : "");
        std::fclose(f);
        return true;
    }

    inline bool GuidIsKnown(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        const uint32_t g[4] = { a, b, c, d };
        return GuidContains(g);
    }

    inline bool IsKnown(uintptr_t vtable, uintptr_t moduleBase)
    {
        if (!vtable)
            return false;
        for (uintptr_t rva : kKnownRvas)
            if (vtable == moduleBase + rva)
                return true;
        return Contains(vtable);
    }
}

struct State
{
    HMODULE module = nullptr;

    float   frameTimeScale = 0.0f;
    float   constraintMass = 0.0f;
    int     deathFrames = 0;

    // Two sources for the resolution. The D3D9 back buffer is authoritative and
    // comes from CreateDevice and Reset; the game's own globals are the fallback,
    // and they read zero until fairly late in startup.
    int deviceWidth = 0;
    int deviceHeight = 0;
    const unsigned short* screenWidth = nullptr;
    const unsigned short* screenHeight = nullptr;

    int Width() const
    {
        if (deviceWidth > 0)
            return deviceWidth;
        return screenWidth ? *screenWidth : 0;
    }

    int Height() const
    {
        if (deviceHeight > 0)
            return deviceHeight;
        return screenHeight ? *screenHeight : 0;
    }
};
inline State g;

// Settings, loaded from BrethrenPatch.ini.
namespace cfg
{
    inline bool havokPhysics = true;
    inline bool highCoreCpu = false;
    inline bool vsyncRefreshRate = true;
    inline bool uiScaling = false;
    inline bool classicCoop = false;
    inline bool classicModeUnlock = false;
    // Skips only the two frontend attract/logo movies.  Story videos and the
    // "previously" recap continue to use DS3's normal movie path.
    inline bool skipAttractVideo = false;
    // Stops the game from moving its own slider in the Windows volume mixer.
    inline bool keepWindowsVolume = false;
    inline bool volumeDiagnostics = false;
    inline bool rawMouseInput = false;
    inline float rawMouseSensitivity = 1.0f;
    inline bool rawMouseInputDiagnostics = false;
    inline bool useSdlControllerInput = false;
    inline bool filterInputDevices = false;
    inline bool sdlControllerDiagnostics = false;
    inline bool gyroAiming = false;
    inline bool gyroAimOnly = true;
    inline float gyroSensitivity = 1.0f;
    inline float gyroSmoothing = 0.15f;
    // Publisher/company logo sequence at launch. The retail duration is 9.0
    // seconds; this is a percentage of it, so 0 removes it entirely and 100
    // leaves it alone.
    inline bool     skipStartupLogos = false;
    inline unsigned startupLogoPercent = 0;
    inline bool fourPlayerDiagnostics = false;
    inline bool fourPlayerExperimental = false;
    inline bool fourPlayerVisualRepair = false;
    // The broad bone-matrix consumers match an instruction shape only.  Keep
    // them opt-in until each caller is proven to operate on a player pose.
    inline bool fourPlayerBoneMatrixConsumers = false;
    // How many uniform streams may be loaded at once. Retail hardcodes 2 for
    // multiplayer and 1 for singleplayer, which is why a third player cannot
    // get a suit of his own. 0 leaves retail alone; the structural ceiling is
    // 8, because UniformEntry::m_playerMask is a single byte.
    inline unsigned fourPlayerUniformStreamLimit = 0;
    // Keeps a third/fourth local actor distinct only while the native uniform
    // lookup executes. It never changes the actor's replicated online ID.
    inline bool fourPlayerUniformIdentityBridge = false;
    inline bool fourPlayerDeathRecovery = false;
    // Read-only instrumentation of the end-of-round response quorum, which is
    // where a third player's death currently stalls. See FourPlayerEndRound.cpp.
    inline bool fourPlayerEndRoundDiagnostics = false;
    // Empties the end-of-round wait list on a machine that is not the host.
    // A client can only send its own response after reaching state 8, and it
    // only reaches state 8 once its list is empty, so a non-empty client list
    // is always a circular wait. See FourPlayerEndRound.cpp.
    inline bool fourPlayerEndRoundUnblock = false;
    // EXPERIMENTAL: when a player quits, retail sends the whole session back to
    // the checkpoint. This is the minimum number of players that must remain
    // for that restart to be suppressed. 0 disables the feature entirely; 2 is
    // the intended production value; 1 also covers "everyone else left and the
    // host is now alone", which is the only variant testable with two players.
    inline unsigned fourPlayerKeepGoingOnLeave = 0;
    // TEST ONLY. Skips the "is the story partner still here" condition, so the
    // suppression can be observed with two players, where the leaver IS the
    // partner and the rule would otherwise correctly decline. Never ship this
    // enabled: in a real lobby it would keep a session running that has lost
    // Carver.
    inline bool fourPlayerKeepGoingIgnorePartner = false;
    // EXPERIMENTAL: stops one player's pause menu from opening everybody
    // else's. This is the player count from which the suppression applies, so
    // the vanilla behaviour is kept where it is still reasonable. 0 disables
    // it; 3 leaves a two-player session exactly as retail and only steps in
    // once freezing everyone actually hurts. Trade-off in FourPlayerEndRound.cpp.
    inline unsigned fourPlayerNoSharedPause = 0;
    // EXPERIMENTAL: makes the inventory "give" go to the player standing
    // closest to the giver instead of to every client at once. Value is the
    // player count from which it applies, 0 disables it. See
    // FourPlayerKiosk.cpp for why this has to be decided on the receiving side.
    inline unsigned fourPlayerGiveNearest = 0;
    // READ-ONLY measurement for the planned player-count difficulty scaling.
    // Traces the shared damage path and Player::ApplyDamageModifiers to settle
    // which machine runs which, before any factor is applied. Writes nothing.
    inline bool fourPlayerScalingDiagnostics = false;

    // ---- Difficulty scaling by player count ------------------------------
    // Master switch. Without it no damage value is touched at all.
    // Supplies the null check missing at 0x005F9BC0. Pure crash protection
    // against a retail bug on checkpoint reload, hence on by default. Details
    // in CheckpointGuard.cpp.
    inline bool fourPlayerCheckpointGuard = true;

    inline bool fourPlayerDifficultyScaling = false;
    // Safety catch: only the host scales. A factor cannot double itself that
    // way while it is still unmeasured which machine runs the damage path.
    // Only turn this off once that has been established.
    inline bool fourPlayerDifficultyHostOnly = true;
    // Base values at two players, in percent of the original damage.
    // Enemies take less (<100), players take more (>100).
    inline unsigned fourPlayerEnemyDamageTakenPercent = 100;
    inline unsigned fourPlayerPlayerDamageTakenPercent = 100;
    // Surcharge per extra player from the third onwards, in percentage points.
    inline unsigned fourPlayerDifficultyPerPlayer = 0;
    // Reads the finished AI Director rosters in the running process. The
    // earlier SimGroup parser hook is never reached on the retail load path.
    // Diagnostics only, read-only.
    inline bool     fourPlayerSpawnDiagnostics = false;
    // Host-only, read-only trace of real FuncSpawn successes. This covers
    // authored/scripted encounter spawns rather than AI Director waves.
    inline bool     fourPlayerScriptedSpawnDiagnostics = false;
    // Extra read-only identity evidence for every FuncSpawn target. Reads the
    // stock SpawnInfo already present on HandleEvents' stack after a successful
    // SpawnEntity call; it never calls an engine method itself.
    inline bool     fourPlayerScriptedSpawnEntityDiagnostics = false;
    // Upper bound for the per-process scripted-spawn catalogue. Kept finite
    // so a long play session cannot grow BrethrenPatch.log without bound.
    inline unsigned fourPlayerScriptedSpawnLogLimit = 12000;
    // Uses FuncSpawn's own delayed retry queue to add recognised combat
    // spawns on the host.  The factor is 3=150%, 4=200%, then +50% each.
    inline bool     fourPlayerDynamicScriptedSpawnScaling = false;
    // Optional local stress-test override. 500 means 500 percent total
    // (four additional native retries) while the host is alone.
    inline unsigned fourPlayerScriptedSpawnSoloTestPercent = 0;
    // Opt-in Director-Quota-Scaler. 100 ist unveraendert; groesser skaliert
    // only the m_npcTargetSpawnCount fields proven at runtime.
    inline unsigned fourPlayerSpawnCountPercent = 100;

    // Runtime classification via vtable slot +0x260.
    //
    // Disproven, hence off by default. The idea was: "slot 0x260 points at the
    // empty Sentient stub 0x00A52A60, so it is not an enemy". Measured live,
    // all three spawned objects showed exactly that stub even though group and
    // population threshold made them clearly enemies. Most NPC classes simply
    // do not override ApplyDamageModifiers and inherit the empty stub, so the
    // slot does not separate enemies from props.
    //
    // The hook stays, but only to measure: it logs the vtable of the created
    // object. The damage path logs the vtable of everything that gets hit, and
    // anything appearing there as a victim demonstrably takes damage.
    // Comparing the two lists gives a criterion that holds up.
    inline bool fourPlayerRuntimeSpawnClass = false;

    // Expensive research-only path: inspect an entity's live memory after it
    // takes damage and write a compact fingerprint for offline analysis.  It
    // is deliberately separate from the shipped known-family allow-list so a
    // normal MoreEnemies game never pays this cost per hit.
    inline bool fourPlayerDamageMemoryProbe = false;

    // The criterion that holds up: a spawn vtable counts as an enemy once an
    // object of that class has demonstrably taken damage. Eight of ten spawned
    // pointers turned up again as damage victims, the same pointer. Fourteen
    // families ship with the patch, more are learned in game. See the
    // damagetakers namespace.
    inline bool fourPlayerProvenSpawnClass = true;
    // Only scale what the player shot at personally, matched by GUID. This
    // disables the coarse vtable list as a permission source, because that
    // opens whole classes at once and takes the decision out of your hands.
    // Both are still learned.
    inline bool fourPlayerOnlySelfDamaged = false;

    // Per-spawner permission via the entity GUID. At 0 only the enemy family
    // (the vtable) counts. Each enemy type has exactly one vtable, so the
    // family never splits a type, so one kill covers that whole family for
    // the entire game.
    inline bool fourPlayerExactSpawnerMatch = true;

    // General net against accesses from a null base (the third player's
    // missing bone matrix buffer). Only fires where the game would otherwise
    // crash. See coop/FourPlayerNullBaseGuard.cpp.
    inline bool fourPlayerNullBaseGuard = true;

    // Special case: the one known d3d9 site that dereferences a garbage
    // return value. Separately switchable because it patches foreign code.
    // See coop/FourPlayerNullBaseGuard.cpp.
    inline bool fourPlayerD3dReturnGuard = false;

    // The player-count to spawn-factor curve. Up to and including
    // `spawnBasePlayers` it stays at 100% (no scaling); above that each player
    // adds `spawnPercentPerPlayer`.
    //
    //   Default 2 / 50:  3 players = 150%   4 = 200%   8 = 400%   16 = 800%
    //
    // None of this is pinned to four players; the count comes live from the
    // PlayerManager and goes up to 16.
    inline unsigned spawnBasePlayers = 2;
    inline unsigned spawnPercentPerPlayer = 50;

    // Raise maxSpawn / popThreshold in the attribute packet.
    //
    // This is the only change on the spawn path that overwrites level data
    // rather than just adding an event, and it was once suspected of
    // suppressing spawns. The proven route (retry only when maxSpawn == -1)
    // does not need it.
    inline bool fourPlayerRaiseMaxSpawn = false;

    // Density instead of duration. The queued-retry path can only ever have
    // one request outstanding per spawner, so extra enemies arrive one at a
    // time as the previous one dies. With this on, the extras are spawned
    // straight away from the result hook, at the same spot the authored one
    // just appeared.
    inline bool     fourPlayerScriptedSpawnBurst = false;

    // Raising the Director's roster quota also raises what an encounter must
    // spawn before it counts as finished. Measured 26.08.2026: the Director
    // runs out of usable spawn points at five to ten enemies in a quarantine,
    // so a quota of 28 can never be met and the door never unlocks. Off by
    // default until that is understood.
    inline bool     fourPlayerDirectorQuotaScaling = false;

    // Raise the population limit separately. `maxSpawn` is how many a spawner
    // may produce over time, `popThreshold` how many may be alive at once.
    // Two switches so each can be tested on its own. Combined there was no
    // telling which of them was doing the work.
    inline bool fourPlayerRaisePopThreshold = false;

    // Per enemy type scaling. The index is the order in
    // src/generated/pc_spawn_archetypes.inc, generated by
    // trainer/gen_spawn_tables.py with the most common type first. The value
    // is a percentage of the global spawn surcharge: 100 normal, 0 never scale
    // this type, 150 half again.
    //
    // Names and count come straight from the generated file. There used to be
    // a hand-written count (26) here and a second name list in a different
    // order in dllmain.cpp, which meant every slider hit the wrong type and
    // types 26 to 31 read past the end of the array.
    inline constexpr const char* const kSpawnTypeNames[] = {
#include "generated/pc_spawn_archetypes.inc"
    };
    constexpr unsigned kSpawnTypeCount =
        sizeof(kSpawnTypeNames) / sizeof(kSpawnTypeNames[0]);

    // Start every type at 100 so nothing sits at 0 when there is no INI.
    struct SpawnTypeTable
    {
        unsigned value[kSpawnTypeCount];
        SpawnTypeTable() { for (unsigned& w : value) w = 100; }
    };
    inline SpawnTypeTable spawnTypeTable;
    inline unsigned* const fourPlayerSpawnTypePercent = spawnTypeTable.value;
    // Host-authoritative Director scaling. Starting with three players, every
    // additional player adds 50 percent to quota targets and population while
    // shortening the authored spawn intervals by the reciprocal factor.
    inline bool     fourPlayerDynamicDirectorScaling = false;
    // EXPERIMENTAL: keeps the world running on the machine whose pause menu is
    // open, instead of freezing time locally. Same shape as the option above -
    // the value is the player count from which it applies, 0 disables it.
    inline unsigned fourPlayerNoLocalPause = 0;
    // Read-only check of the two-slot campaign loadout arrays, which would
    // silently give players three and up no weapons. See FourPlayerLoadout.cpp.
    inline bool fourPlayerLoadoutDiagnostics = false;
    // Read-only trace of the elevator/door/gating field test, to find out
    // which players actually count. See FourPlayerField.cpp.
    inline bool fourPlayerFieldDiagnostics = false;
    // EXPERIMENTAL: makes an elevator wait for EVERY remote player instead of
    // acting on whichever response arrived last. See FourPlayerField.cpp.
    inline bool fourPlayerFieldQuorum = false;
    // Read-only trace of the in-world player name label. See FourPlayerNameplate.cpp.
    inline bool fourPlayerNameplateDiagnostics = false;
    // EXPERIMENTAL, off by default. Spawns one extra BuddyPositionIndicator so
    // a second remote player also gets a name above his head. The engine only
    // manages the single indicator in Player+0x4C40, so ours is unmanaged; see
    // the risk note in FourPlayerNameplate.cpp before enabling.
    inline bool fourPlayerExtraNameplates = false;
    // Read-only trace of the suit kiosk's single-remote-player resolution.
    inline bool fourPlayerKioskDiagnostics = false;
    // Read-only render correlation. Records the renderer instance, player
    // model and animation-table pointer at the exact null-table guard. This
    // is the missing evidence needed to replace the old global "last Carver
    // table" fallback with a stable per-actor profile.
    inline bool fourPlayerRenderDiagnostics = false;
    // Counts null D3D resources in the mesh binder that crashed a three-player
    // suit change. Read-only. See FourPlayerRenderGuard.cpp.
    inline bool fourPlayerRenderGuard = false;
    // EXPERIMENTAL: keeps a per-player copy of the single ghost weapon set and
    // swaps the right one in before it is read. See FourPlayerLoadout.cpp.
    inline bool fourPlayerGhostWeaponFix = false;
    // EXPERIMENTAL: re-applies a kiosk suit change to the actor actually at the
    // terminal instead of the one the single resolver picked.
    inline bool fourPlayerKioskRedirect = false;
    // EXPERIMENTAL: suppresses the D3D device call for an implausible mesh
    // resource instead of letting it crash. Needs FourPlayerRenderGuard.
    inline bool fourPlayerRenderSkipBad = false;
    inline bool fourPlayerLobbyOverlay = false;
    // Read-only, temporary probe for the OnlinePlayer -> Blaze user-name path.
    inline bool fourPlayerNameDiagnostics = false;
    inline bool log = false;

    // Measurement only, see Diagnostics.cpp.
    inline bool tkDiagnostics = false;

    // Keeps solver substeps per second constant, see SolverInit_Hook.
    inline bool ragdollSubsteps = false;
    inline bool solverDiagnostics = false;

    inline int   cpuCoreCap = 8;
    inline int   vsyncMultiplier = 1;
    inline int   ragdollMessageId = 0x1F;
    inline float uiScale = 1.0f;

    inline bool  depthOfField = false;
    inline float dofScale = 1.0f;
    // Scales only CTAB-identified `dof_widths` uploads.  Unlike the retired
    // screen-space candidate, this never touches reciprocal texture sizes.
    inline bool  dofShaderFix = false;
    // Write-only renderer trace; never changes a shader constant.  It is
    // deliberately opt-in because recording the initial shader set is useful
    // for reverse engineering but not needed by players.
    inline bool  dofShaderDiagnostics = false;

    // Replaces an authored combat target only with another combat target from
    // the same offline-confirmed PC archive. It is intentionally independent
    // from MoreEnemies: a player can randomize retail encounter sizes.
    inline bool     enemyRandomizer = false;
    inline unsigned enemyRandomizerPercent = 100;
    inline bool     enemyRandomizerAllowHumans = false;
    // Read-only per-FuncSpawn decision trace for randomizer investigation.
    // It is capped in the INI and uses buffered logging, so it must never
    // change gameplay timing like the old synchronous spawn logging did.
    inline bool     enemyRandomizerDiagnostics = false;
    inline unsigned enemyRandomizerLogLimit = 160;
    // Optional, reviewed extension of the current-level randomizer cache.
    // The package is streamed normally and admitted only after its controller
    // UID appears in the live SimManager hash.
    inline bool     enemyRandomizerGlobalTemplates = false;
    // Experimental opt-in: permits only globally reviewed templates to
    // replace an already-classified *ground* FuncSpawn from another family.
    // Ordinary live targets remain same-family only.
    inline bool     enemyRandomizerCrossFamilyGround = false;

    // Individual Havok hooks, so a misbehaving one can be isolated.
    inline bool contactJacobian = true;
    inline bool ballSocket = true;
    inline bool angular1D = true;
    inline bool solverInit = true;
    inline bool impulseDamper = true;
    inline bool errorScaler = true;
    inline bool massCapture = true;
    inline bool deadState = true;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
inline CRITICAL_SECTION gLogLock;
inline bool gLogReady = false;
inline char gLogPath[MAX_PATH]{};
inline FILE* gLogFile = nullptr;

// Keep the handle open to avoid stalls during diagnostic bursts. _SH_DENYWR
// still allows the log to be read while the game is running.
inline void LogInit()
{
    if (!cfg::log)
        return;

    GetModuleFileNameA(nullptr, gLogPath, MAX_PATH);
    if (char* slash = strrchr(gLogPath, '\\'))
        *(slash + 1) = 0;
    strcat_s(gLogPath, MAX_PATH, "BrethrenPatch.log");

    InitializeCriticalSection(&gLogLock);

    gLogFile = _fsopen(gLogPath, "w", _SH_DENYWR);   // truncate on every launch
    gLogReady = gLogFile != nullptr;
}

inline DWORD gLastFlush = 0;

inline void Log(const char* fmt, ...)
{
    if (!gLogReady || !cfg::log)
        return;

    EnterCriticalSection(&gLogLock);
    if (gLogFile)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(gLogFile, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(gLogFile, fmt, args);
        va_end(args);

        fputc('\n', gLogFile);
        // Throttle disk writes so diagnostics cannot stall the game thread.
        // Crash-sensitive messages use LogNow().
        const DWORD now = GetTickCount();
        if (now - gLastFlush >= 100)
        {
            fflush(gLogFile);
            gLastFlush = now;
        }
    }
    LeaveCriticalSection(&gLogLock);
}

// Like Log, but forces the write to disk immediately. Only for lines a crash
// right afterwards must not swallow.
inline void LogNow(const char* fmt, ...)
{
    if (!gLogReady || !cfg::log)
        return;
    EnterCriticalSection(&gLogLock);
    if (gLogFile)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(gLogFile, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute,
                t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfprintf(gLogFile, fmt, args);
        va_end(args);
        fputc('\n', gLogFile);
        fflush(gLogFile);
        gLastFlush = GetTickCount();
    }
    LeaveCriticalSection(&gLogLock);
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
namespace mem
{
    template <typename T>
    bool Write(uintptr_t address, T value)
    {
        DWORD old = 0;
        if (!VirtualProtect((LPVOID)address, sizeof(T), PAGE_EXECUTE_READWRITE, &old))
            return false;

        *reinterpret_cast<T*>(address) = value;
        VirtualProtect((LPVOID)address, sizeof(T), old, &old);
        return true;
    }

    template <typename T>
    T Read(uintptr_t address)
    {
        return *reinterpret_cast<T*>(address);
    }

    // Pattern in "55 8B EC ?? 90" form; ?? matches any byte.
    inline DWORD Scan(HMODULE module, std::string_view pattern)
    {
        auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;

        auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>((BYTE*)module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        uint8_t bytes[256]{}, mask[256]{};
        size_t len = 0;

        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };

        for (size_t i = 0; i < pattern.size() && len < sizeof(bytes); ++i)
        {
            if (pattern[i] == ' ')
                continue;

            if (pattern[i] == '?')
            {
                ++len;
                if (i + 1 < pattern.size() && pattern[i + 1] == '?')
                    ++i;
                continue;
            }

            bytes[len] = (nibble(pattern[i]) << 4) | nibble(pattern[i + 1]);
            mask[len] = 0xFF;
            ++len;
            ++i;
        }

        const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
        if (len == 0 || imageSize < len)
            return 0;

        BYTE* base = (BYTE*)module;
        for (BYTE* p = base; p <= base + imageSize - len; ++p)
        {
            bool hit = true;
            for (size_t i = 0; i < len; ++i)
            {
                if (mask[i] && p[i] != bytes[i]) { hit = false; break; }
            }
            if (hit)
                return reinterpret_cast<DWORD>(p);
        }
        return 0;
    }
}

// Called by the established game-thread frame tick.  It remains a no-op unless
// the experimental four-player visual repair is enabled.
void TickFourPlayerVisualRepair();

// Samples the game-flow state and pending-response count from the same tick.
// It is a no-op unless the end-round diagnostics are enabled, and never writes.
void TickFourPlayerEndRound();

// Publishes a bounded PlayerManager roster snapshot for the separate local GDI
// overlay. It never changes player, session, or network state.
void PublishFourPlayerLobbyRoster();

// EXPERIMENTAL and opt-in: gives a second remote player a name plate. A no-op
// unless FourPlayerExtraNameplates is set AND two or more remote players are
// present, so it cannot fire in a two-player session.
void TickFourPlayerExtraNameplate();

// The menu has no PlayerManager yet. These callbacks cache only confirmed
// lobby membership events; they do not dereference or modify OnlinePlayer.
void ObserveFourPlayerLobbyHostCreated();
void ObserveFourPlayerLobbyRemoteJoined();
// `displayName` is a bounded copy made by the game-thread diagnostics hook.
// The overlay never retains an OnlinePlayer, Player or User pointer.
void ObserveFourPlayerLobbyPlayerJoined(void* onlinePlayerRef, const char* displayName = nullptr);
void ObserveFourPlayerLobbyPlayerLeft(void* onlinePlayerRef);

// Called from the host's Director quota path after PlayerManager has a live
// roster. The settings factory caches the authored baseline; this function
// only reapplies a derived value when the player count changes.
void ApplyDynamicDirectorSettings(unsigned playerCount);
// Scales the roster quota of every AIDirectorEncounter seen so far. Called
// from the Director's own update so the numbers are in place long before an
// encounter starts reading them.
void ScaleKnownEncounterQuotas(unsigned playerCount);
// Remembers one AIDirectorEncounter so its quota can be scaled at load time.
void NoteDirectorEncounter(uintptr_t encounter);
// Scales one encounter's roster quota right now. The roster is resolved
// lazily, so the moment the Director takes an encounter on is the earliest
// point at which there is anything to scale.
void ScaleEncounterNow(uintptr_t encounter, unsigned playerCount);

// Shared MoreEnemies curve. Single-player uses MoreEnemiesPercent; once the
// roster reaches MoreEnemiesBasePlayers, each additional player contributes
// MoreEnemiesPerPlayerPercent. Both the authored FuncSpawn path and the AI
// Director must use this exact function, otherwise a mixed encounter would
// silently scale by two different factors.
unsigned MoreEnemiesPercentForPlayers(unsigned playerCount);

// Scan and report the result, so a broken signature shows up in the log
// instead of silently disabling a fix.
// Tallied so startup can end with a one-line verdict. Reading a hundred
// "found"/"MISSING" lines to answer "did everything install?" is how a
// half-applied build gets mistaken for a failed fix, which already happened
// once, when a machine ran an outdated INI and the missing switch was only
// noticed after the session.
// Resolved once by FourPlayerDiagnostics and shared, because a second scan for
// the same function fails: the first module installs an inline hook, which
// overwrites the prologue, and the signature no longer matches its own target.
// That cost the kiosk's sender identification a whole session on 2026-08-16 -
// "MISSING FourPlayerKiosk GetPlayerByOnlineId" while the very same address had
// been found seconds earlier.
inline uintptr_t gGetPlayerByOnlineIdAddr = 0;

inline long gSignaturesFound = 0;
inline long gSignaturesMissing = 0;
inline char gMissingNames[512] = "";

inline DWORD Find(std::string_view pattern, const char* name)
{
    const DWORD address = mem::Scan(g.module, pattern);
    if (address)
    {
        ++gSignaturesFound;
        Log("  found  %-34s %08X", name, address);
    }
    else
    {
        ++gSignaturesMissing;
        const size_t used = strlen(gMissingNames);
        if (used + strlen(name) + 3 < sizeof(gMissingNames))
        {
            if (used)
                strcat_s(gMissingNames, sizeof(gMissingNames), ", ");
            strcat_s(gMissingNames, sizeof(gMissingNames), name);
        }
        Log("  MISSING %-33s (signature did not match)", name);
    }
    return address;
}

inline safetyhook::InlineHook Hook(void* target, void* detour)
{
    return safetyhook::create_inline(target, detour);
}
