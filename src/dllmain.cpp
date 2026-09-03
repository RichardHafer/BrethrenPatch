// BrethrenPatch: fixes for the PC port of Dead Space 3.
//
// Loaded as an ASI early in the process, before the D3D9 device exists.

#include "Common.hpp"

void ApplyHavokPhysics();
void ApplyHighCoreCpu();
void ApplyVSyncD3D9();
void ApplyDiagnostics();
void ApplyDepthOfField();
void ResolveScreenSize();
void ApplyFrameRateLock();
void ApplyUiScaling();
void ApplyClassicCoop();
void ApplyClassicModeUnlock();
void ApplySkipAttractVideo();
void ApplySkipStartupLogos();
void ApplyWindowsVolume();
void ApplyRawMouseInput();
void ApplyControllerInput();
void ApplyFourPlayerScalingDiagnostics();
void ApplyFourPlayerSpawnDiagnostics();
void ApplyFourPlayerCheckpointGuard();

// The anchor used to tell whether the packer has finished; see WaitForGameCode.
static const char* kUnpackAnchor = "55 8B EC 83 E4 F0 81 EC A4 00 00 00 8B 55 08";
// Claim startup before logging or waiting for the unpacker. Some ASI loaders
// invoke the process-attach path more than once.
static volatile LONG gInitStarted = 0;

static std::string IniPath()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    if (slash != std::string::npos)
        result.erase(slash + 1);

    return result + "BrethrenPatch.ini";
}

static void LoadSettings()
{
    const std::string ini = IniPath();

    auto number = [&](const char* key, int fallback) {
        return GetPrivateProfileIntA("BrethrenPatch", key, fallback, ini.c_str());
    };
    auto flag = [&](const char* key, bool fallback) {
        return number(key, fallback ? 1 : 0) != 0;
    };
    auto real = [&](const char* key, float fallback) {
        char text[64]{};
        char defaultText[32]{};
        std::snprintf(defaultText, sizeof(defaultText), "%.3f", fallback);
        GetPrivateProfileStringA("BrethrenPatch", key, defaultText,
                                 text, sizeof(text), ini.c_str());
        char* end = nullptr;
        const float value = std::strtof(text, &end);
        return end != text ? value : fallback;
    };

    // Keep the old FourPlayer keys as aliases for existing configurations.
    auto numberAlias = [&](const char* key, const char* legacyKey, int fallback) {
        return number(key, number(legacyKey, fallback));
    };
    auto flagAlias = [&](const char* key, const char* legacyKey, bool fallback) {
        return numberAlias(key, legacyKey, fallback ? 1 : 0) != 0;
    };

    // Off by default: it helps, but only about half the time. See the INI.
    cfg::havokPhysics     = flag("HavokPhysicsFix", false);
    cfg::highCoreCpu      = flag("HighCoreCPUFix", false);
    cfg::vsyncRefreshRate = flag("VSyncRefreshRateFix", true);
    cfg::uiScaling        = flag("UiScaling", true);
    cfg::classicCoop      = flag("ClassicCoop", true);
    cfg::classicModeUnlock = flag("ClassicModeUnlock", true);
    cfg::skipAttractVideo =
        flagAlias("SkipAttractVideo", "SkipStartupIntro", true);
    cfg::keepWindowsVolume = flag("KeepWindowsVolume", false);
    cfg::volumeDiagnostics = flag("VolumeDiagnostics", true);
    cfg::rawMouseInput = flag("RawMouseInput", false);
    cfg::rawMouseSensitivity = real("RawMouseSensitivity", 1.0f);
    if (cfg::rawMouseSensitivity < 0.05f)
        cfg::rawMouseSensitivity = 0.05f;
    if (cfg::rawMouseSensitivity > 20.0f)
        cfg::rawMouseSensitivity = 20.0f;
    cfg::rawMouseInputDiagnostics = flag("RawMouseInputDiagnostics", false);
    cfg::useSdlControllerInput = flag("UseSDLControllerInput", false);
    cfg::filterInputDevices =
        flagAlias("FilterInputDevices", "BlockDirectInputDevices", false);
    cfg::sdlControllerDiagnostics = flag("SDLControllerDiagnostics", false);
    cfg::gyroAiming = flagAlias("GyroAiming", "GyroEnabled", false);
    cfg::gyroAimOnly = flag("GyroAimOnly", true);
    cfg::gyroSensitivity = real("GyroSensitivity", 1.0f);
    cfg::gyroSmoothing = real("GyroSmoothing", 0.15f);
    cfg::skipStartupLogos = flag("SkipStartupLogos", true);
    const int logoPct = number("StartupLogoPercent", 0);
    cfg::startupLogoPercent = static_cast<unsigned>(
        logoPct < 0 ? 0 : logoPct > 100 ? 100 : logoPct);
    // ------------------------------------------------------------------
    // Multiplayer research. These modules live in src/coop/ and are not in the
    // build any more, see build.bat. The switches are still read so that
    // putting the modules back finds the old settings instead of quietly
    // resetting them.
    // ------------------------------------------------------------------
    cfg::fourPlayerDiagnostics = flag("FourPlayerDiagnostics", false);
    cfg::fourPlayerExperimental = flag("FourPlayerExperimental", false);
    cfg::fourPlayerVisualRepair = flag("FourPlayerVisualRepair", false);
    cfg::fourPlayerBoneMatrixConsumers = flag("FourPlayerBoneMatrixConsumers", false);
    cfg::fourPlayerUniformStreamLimit = number("FourPlayerUniformStreamLimit", 0);
    cfg::fourPlayerUniformIdentityBridge = flag("FourPlayerUniformIdentityBridge", false);
    cfg::fourPlayerDeathRecovery = flag("FourPlayerDeathRecovery", false);
    cfg::fourPlayerEndRoundDiagnostics = flag("FourPlayerEndRoundDiagnostics", false);
    cfg::fourPlayerEndRoundUnblock = flag("FourPlayerEndRoundUnblock", false);
    cfg::fourPlayerKeepGoingOnLeave = number("FourPlayerKeepGoingOnLeave", 0);
    cfg::fourPlayerKeepGoingIgnorePartner = flag("FourPlayerKeepGoingIgnorePartner", false);
    cfg::fourPlayerNoSharedPause = number("FourPlayerNoSharedPause", 0);
    cfg::fourPlayerNoLocalPause = number("FourPlayerNoLocalPause", 0);
    cfg::fourPlayerGiveNearest = number("FourPlayerGiveNearest", 0);
    cfg::fourPlayerScalingDiagnostics = flag("FourPlayerScalingDiagnostics", false);
    cfg::fourPlayerCheckpointGuard =
        flagAlias("CheckpointGuard", "FourPlayerCheckpointGuard", true);
    cfg::fourPlayerDifficultyScaling =
        flagAlias("DifficultyScaling", "FourPlayerDifficultyScaling", false);
    cfg::fourPlayerDifficultyHostOnly = flag("FourPlayerDifficultyHostOnly", true);
    cfg::fourPlayerEnemyDamageTakenPercent =
        numberAlias("EnemyDamageTakenPercent", "FourPlayerEnemyDamageTakenPercent", 100);
    cfg::fourPlayerPlayerDamageTakenPercent =
        numberAlias("PlayerDamageTakenPercent", "FourPlayerPlayerDamageTakenPercent", 100);
    cfg::fourPlayerDifficultyPerPlayer = number("FourPlayerDifficultyPerPlayer", 0);
    cfg::fourPlayerSpawnDiagnostics = flag("FourPlayerSpawnDiagnostics", false);
    cfg::fourPlayerScriptedSpawnDiagnostics = flag("FourPlayerScriptedSpawnDiagnostics", false);
    cfg::fourPlayerScriptedSpawnEntityDiagnostics =
        flag("FourPlayerScriptedSpawnEntityDiagnostics", false);
    const int scriptedSpawnLogLimit = number("FourPlayerScriptedSpawnLogLimit", 12000);
    cfg::fourPlayerScriptedSpawnLogLimit = static_cast<unsigned>(
        scriptedSpawnLogLimit < 1 ? 1 :
        scriptedSpawnLogLimit > 50000 ? 50000 : scriptedSpawnLogLimit);
    cfg::fourPlayerDynamicScriptedSpawnScaling =
        flagAlias("MoreEnemies", "FourPlayerDynamicScriptedSpawnScaling", false);
    cfg::fourPlayerScriptedSpawnSoloTestPercent =
        numberAlias("MoreEnemiesPercent", "FourPlayerScriptedSpawnSoloTestPercent", 200);
    cfg::fourPlayerSpawnCountPercent = number("FourPlayerSpawnCountPercent", 100);
    cfg::fourPlayerRuntimeSpawnClass =
        flagAlias("MoreEnemiesLearnFromKills", "FourPlayerRuntimeSpawnClass", false);
    cfg::fourPlayerDamageMemoryProbe =
        flagAlias("MoreEnemiesDamageMemoryProbe", "FourPlayerDamageMemoryProbe", false);
    cfg::fourPlayerProvenSpawnClass =
        flagAlias("MoreEnemiesKnownFamilies", "FourPlayerProvenSpawnClass", true);
    cfg::fourPlayerOnlySelfDamaged =
        flag("FourPlayerNurSelbstBeschossen", false);
    cfg::fourPlayerExactSpawnerMatch =
        flagAlias("MoreEnemiesExactSpawnerMatch", "FourPlayerSpawnerGenau", true);
    cfg::fourPlayerNullBaseGuard = flag("FourPlayerNullBaseGuard", true);
    cfg::fourPlayerD3dReturnGuard = flag("FourPlayerD3dReturnGuard", false);
    cfg::spawnBasePlayers = static_cast<unsigned>(
        numberAlias("MoreEnemiesBasePlayers", "FourPlayerSpawnBasisSpieler", 2));
    cfg::spawnPercentPerPlayer = static_cast<unsigned>(
        numberAlias("MoreEnemiesPerPlayerPercent", "FourPlayerSpawnProzentProSpieler", 50));
    cfg::fourPlayerRaiseMaxSpawn =
        flagAlias("MoreEnemiesRaiseSpawnerCap", "FourPlayerRaiseMaxSpawn", true);
    cfg::fourPlayerDirectorQuotaScaling =
        flagAlias("MoreEnemiesDirectorQuota", "FourPlayerDirectorQuotaScaling", false);
    cfg::fourPlayerScriptedSpawnBurst =
        flagAlias("MoreEnemiesAtOnce", "FourPlayerScriptedSpawnBurst", false);
    cfg::fourPlayerRaisePopThreshold =
        flagAlias("MoreEnemiesRaisePopulationLimit", "FourPlayerRaisePopThreshold", true);

    // Per enemy type. One key per type, `EnemyType_<name>`, value in percent
    // of the global surcharge: 100 follows the global factor, 0 leaves the
    // type alone, 200 doubles its share.
    //
    // The key names are built from the same table the spawn code uses. There
    // used to be a second hand-written list here in a different order, which
    // meant every slider hit the wrong type.
    for (unsigned i = 0; i < cfg::kSpawnTypeCount; ++i)
    {
        const std::string key = std::string("EnemyType_") + cfg::kSpawnTypeNames[i];
        const std::string legacyKey =
            std::string("FourPlayerSpawnArt_") + cfg::kSpawnTypeNames[i];
        cfg::fourPlayerSpawnTypePercent[i] = static_cast<unsigned>(
            numberAlias(key.c_str(), legacyKey.c_str(), 100));
    }
    // The Director places enemies on top of the authored spawn points, so
    // scaling it belongs to the same feature. It is off until it has had a
    // proper run, and it needs MoreEnemies anyway, hence the guard.
    cfg::fourPlayerDynamicDirectorScaling =
        cfg::fourPlayerDynamicScriptedSpawnScaling &&
        flagAlias("MoreEnemiesDirectorScaling", "FourPlayerDynamicDirectorScaling", true);
    cfg::fourPlayerLoadoutDiagnostics = flag("FourPlayerLoadoutDiagnostics", false);
    cfg::fourPlayerFieldDiagnostics = flag("FourPlayerFieldDiagnostics", false);
    cfg::fourPlayerFieldQuorum = flag("FourPlayerFieldQuorum", false);
    cfg::fourPlayerNameplateDiagnostics = flag("FourPlayerNameplateDiagnostics", false);
    cfg::fourPlayerExtraNameplates = flag("FourPlayerExtraNameplates", false);
    cfg::fourPlayerKioskDiagnostics = flag("FourPlayerKioskDiagnostics", false);
    cfg::fourPlayerRenderDiagnostics = flag("FourPlayerRenderDiagnostics", false);
    cfg::fourPlayerRenderGuard = flag("FourPlayerRenderGuard", false);
    cfg::fourPlayerGhostWeaponFix = flag("FourPlayerGhostWeaponFix", false);
    cfg::fourPlayerKioskRedirect = flag("FourPlayerKioskRedirect", false);
    cfg::fourPlayerRenderSkipBad = flag("FourPlayerRenderSkipBad", false);
    cfg::fourPlayerLobbyOverlay = flag("FourPlayerLobbyOverlay", false);
    cfg::fourPlayerNameDiagnostics = flag("FourPlayerNameDiagnostics", false);
    cfg::log              = flag("DebugLogging", false);
    cfg::tkDiagnostics    = flag("TKDiagnostics", false);
    cfg::solverDiagnostics = flag("SolverDiagnostics", false);

    cfg::cpuCoreCap       = number("CpuCoreCap", 8);
    cfg::vsyncMultiplier  = number("VSyncMultiplier", 1);
    cfg::ragdollMessageId = number("RagdollMessageId", 0x1F);
    cfg::ragdollSubsteps  = flag("RagdollSubsteps", false);
    cfg::uiScale          = number("UiScalePercent", 100) / 100.0f;
    cfg::depthOfField     = flag("DepthOfFieldFix", false);
    cfg::dofScale         = number("DepthOfFieldPercent", 100) / 100.0f;
    cfg::dofShaderFix     = flag("DepthOfFieldShaderFix", false);
    cfg::dofShaderDiagnostics = flag("DepthOfFieldShaderDiagnostics", false);
    cfg::enemyRandomizer = flag("EnemyRandomizer", false);
    cfg::enemyRandomizerPercent = number("EnemyRandomizerPercent", 100);
    if (cfg::enemyRandomizerPercent > 100)
        cfg::enemyRandomizerPercent = 100;
    cfg::enemyRandomizerAllowHumans = flag("EnemyRandomizerAllowHumans", false);
    cfg::enemyRandomizerDiagnostics = flag("EnemyRandomizerDiagnostics", false);
    cfg::enemyRandomizerGlobalTemplates =
        flag("EnemyRandomizerGlobalTemplates", false);
    cfg::enemyRandomizerCrossFamilyGround =
        flag("EnemyRandomizerCrossFamilyGround", false);
    const int randomizerLogLimit = number("EnemyRandomizerLogLimit", 160);
    cfg::enemyRandomizerLogLimit = static_cast<unsigned>(
        randomizerLogLimit < 1 ? 1 : randomizerLogLimit > 2000 ? 2000 : randomizerLogLimit);

    cfg::contactJacobian  = flag("HookContactJacobian", true);
    cfg::ballSocket       = flag("HookBallSocket", true);
    cfg::angular1D        = flag("HookAngular1D", true);
    cfg::solverInit       = flag("HookSolverInit", true);
    cfg::impulseDamper    = flag("HookImpulseDamper", true);
    cfg::errorScaler      = flag("HookErrorScaler", true);
    cfg::massCapture      = flag("HookMassCapture", true);
    cfg::deadState        = flag("HookDeadState", true);
}

// deadspace3.exe is packed. An ASI loaded through winmm.dll runs before the
// unpacker in the entry point, when .text still holds compressed data and no
// pattern can match. Symptom if you skip this: the ASI loads but not a single
// hook is installed.
static bool WaitForGameCode()
{
    for (int attempt = 0; attempt < 480; ++attempt)      // up to two minutes
    {
        if (mem::Scan(g.module, kUnpackAnchor))
        {
            Log("game code unpacked after roughly %d ms", attempt * 250);
            return true;
        }
        Sleep(250);
    }

    Log("game code still packed, skipping the hooks that need it");
    return false;
}

static DWORD WINAPI Init(LPVOID)
{
    if (InterlockedCompareExchange(&gInitStarted, 1, 0) != 0)
        return 0;

    g.module = GetModuleHandleA(nullptr);
    if (!g.module)
        return 0;

    LoadSettings();
    LogInit();

    Log("BrethrenPatch starting, module %08X", (unsigned)(uintptr_t)g.module);
    Log("havok %d, cpu %d, vsync %d (multiplier %d), ui %d",
        (int)cfg::havokPhysics, (int)cfg::highCoreCpu, (int)cfg::vsyncRefreshRate,
        cfg::vsyncMultiplier, (int)cfg::uiScaling);
    Log("classic coop %d, Classic/NG+ menu unlock %d",
        (int)cfg::classicCoop, (int)cfg::classicModeUnlock);
    Log("attract video skip %d", (int)cfg::skipAttractVideo);
    Log("input: raw mouse %d (%.2fx), SDL controller %d, device filter %d, gyro %d",
        (int)cfg::rawMouseInput, cfg::rawMouseSensitivity,
        (int)cfg::useSdlControllerInput, (int)cfg::filterInputDevices,
        (int)cfg::gyroAiming);
    Log("four-player diagnostics %d (observation only)", (int)cfg::fourPlayerDiagnostics);
    Log("four-player experimental capacity %d (normal campaign only)",
        (int)cfg::fourPlayerExperimental);
    Log("four-player visual repair %d (extra-player uniform bootstrap)",
        (int)cfg::fourPlayerVisualRepair);
    Log("four-player broad bone-matrix consumers %d (unsafe experimental opt-in)",
        (int)cfg::fourPlayerBoneMatrixConsumers);
    Log("four-player uniform identity bridge %d (local PlayerEntry lookup only)",
        (int)cfg::fourPlayerUniformIdentityBridge);
    Log("four-player death recovery %d (host checkpoint fallback)",
        (int)cfg::fourPlayerDeathRecovery);
    Log("four-player end-round diagnostics %d (read-only quorum trace), unblock %d",
        (int)cfg::fourPlayerEndRoundDiagnostics,
        (int)cfg::fourPlayerEndRoundUnblock);
    Log("four-player loadout diagnostics %d (read-only two-slot check)",
        (int)cfg::fourPlayerLoadoutDiagnostics);
    Log("four-player field diagnostics %d (elevator/door quorum trace)",
        (int)cfg::fourPlayerFieldDiagnostics);
    Log("four-player lobby overlay %d (local eight-slot presentation)",
        (int)cfg::fourPlayerLobbyOverlay);
    Log("four-player name probe %d (read-only diagnostics)",
        (int)cfg::fourPlayerNameDiagnostics);

    // Neither of these touches game code, and both have to happen early.
    ApplyHighCoreCpu();
    ApplyVSyncD3D9();
    ApplyControllerInput();

    if (WaitForGameCode())
    {
        ApplyFrameRateLock();
        ResolveScreenSize();      // both of the next two need the screen size
        ApplyUiScaling();
        ApplyDepthOfField();
        ApplyHavokPhysics();
        ApplyDiagnostics();
        ApplySkipAttractVideo();
        ApplySkipStartupLogos();
        ApplyWindowsVolume();
        ApplyRawMouseInput();
        ApplyClassicModeUnlock();
        // Four-player installs its observation hooks from the untouched retail
        // capacity sequence. ClassicCoop changes one branch in that sequence,
        // so it must run afterwards when both options are enabled.
        // Load the learned enemy classes before the spawn hooks so the very
        // first spawn of a known class is already recognised.
        if (cfg::fourPlayerProvenSpawnClass)
        {
            damagetakers::Load(reinterpret_cast<uintptr_t>(g.module));
            damagetakers::GuidLoad();
            // Spell the curve out once, so the log shows what the setting
            // means at the actual player count instead of leaving it to be
            // worked out from two numbers.
            {
                char curve[192] = {};
                size_t len = 0;
                for (unsigned n = 2; n <= 16 && len < sizeof(curve) - 16; n += 2)
                {
                    const unsigned pct = n <= cfg::spawnBasePlayers
                        ? 100u
                        : 100u + (n - cfg::spawnBasePlayers) *
                                  cfg::spawnPercentPerPlayer;
                    len += static_cast<size_t>(std::snprintf(
                        curve + len, sizeof(curve) - len, "%s%u players=%u%%",
                        len ? "  " : "", n, pct));
                }
                Log("EnemySpawns: scaling curve (base %u players, "
                    "+%u%% each above that): %s",
                    cfg::spawnBasePlayers, cfg::spawnPercentPerPlayer, curve);
            }
            Log("EnemySpawns: %ld self-damaged enemies loaded from %s",
                InterlockedCompareExchange(&damagetakers::guidCount, 0, 0),
                damagetakers::GuidStorePath());
            Log("EnemySpawns: %ld learned enemy classes loaded from %s "
                "(plus %u shipped with the patch)",
                InterlockedCompareExchange(&damagetakers::count, 0, 0),
                damagetakers::StorePath(),
                static_cast<unsigned>(sizeof(damagetakers::kKnownRvas) /
                                      sizeof(damagetakers::kKnownRvas[0])));
        }

        ApplyFourPlayerScalingDiagnostics();
        ApplyFourPlayerSpawnDiagnostics();
        ApplyFourPlayerCheckpointGuard();
        ApplyClassicCoop();
    }

    // One line that answers "is this build fully applied?", the question that
    // has to be settled before a co-op session, not after it.
    Log("--------------------------------------------------------------");
    if (gSignaturesMissing == 0)
        Log("SELF-CHECK OK: %ld of %ld signatures resolved, nothing missing",
            gSignaturesFound, gSignaturesFound);
    else
        Log("SELF-CHECK: %ld resolved, %ld MISSING -> %s",
            gSignaturesFound, gSignaturesMissing, gMissingNames);

    // The four-player switches that must match on every machine. A mismatched
    // INI on one client already cost a whole test session once.
    Log("SELF-CHECK four-player switches: experimental=%d endRoundUnblock=%d "
        "uniformStreamLimit=%u fieldQuorum=%d ghostWeaponFix=%d kioskRedirect=%d "
        "renderSkipBad=%d boneConsumers=%d keepGoingOnLeave=%u ignorePartner=%d extraNameplates=%d "
        "noSharedPause=%u noLocalPause=%u giveNearest=%u spawnDiag=%d scriptedSpawnDiag=%d dynamicScripted=%d soloScriptedPercent=%u spawnPercent=%u dynamicDirector=%d "
        "difficulty=%d diffHostOnly=%d enemyDmg=%u playerDmg=%u diffPerPlayer=%u",
        (int)cfg::fourPlayerExperimental, (int)cfg::fourPlayerEndRoundUnblock,
        cfg::fourPlayerUniformStreamLimit, (int)cfg::fourPlayerFieldQuorum,
        (int)cfg::fourPlayerGhostWeaponFix, (int)cfg::fourPlayerKioskRedirect,
        (int)cfg::fourPlayerRenderSkipBad, (int)cfg::fourPlayerBoneMatrixConsumers,
        cfg::fourPlayerKeepGoingOnLeave,
        (int)cfg::fourPlayerKeepGoingIgnorePartner,
        (int)cfg::fourPlayerExtraNameplates, cfg::fourPlayerNoSharedPause, cfg::fourPlayerNoLocalPause,
        cfg::fourPlayerGiveNearest, (int)cfg::fourPlayerSpawnDiagnostics,
        (int)cfg::fourPlayerScriptedSpawnDiagnostics,
        (int)cfg::fourPlayerDynamicScriptedSpawnScaling,
        cfg::fourPlayerScriptedSpawnSoloTestPercent,
        cfg::fourPlayerSpawnCountPercent, (int)cfg::fourPlayerDynamicDirectorScaling,
        (int)cfg::fourPlayerDifficultyScaling, (int)cfg::fourPlayerDifficultyHostOnly,
        cfg::fourPlayerEnemyDamageTakenPercent, cfg::fourPlayerPlayerDamageTakenPercent,
        cfg::fourPlayerDifficultyPerPlayer);
    Log("--------------------------------------------------------------");

    // One line an ordinary player can read. The line above is for us; this
    // one should answer "is it on?" without anyone having to ask.
    if (cfg::fourPlayerDynamicScriptedSpawnScaling)
        Log("More enemies: ON, %u percent in single player, %u base players "
            "+%u percent each. Spawner cap %s, population limit %s, known "
            "families %s, learn from kills %s.",
            cfg::fourPlayerScriptedSpawnSoloTestPercent,
            cfg::spawnBasePlayers, cfg::spawnPercentPerPlayer,
            cfg::fourPlayerRaiseMaxSpawn ? "raised" : "left alone",
            cfg::fourPlayerRaisePopThreshold ? "raised" : "left alone",
            cfg::fourPlayerProvenSpawnClass ? "on" : "off",
            cfg::fourPlayerRuntimeSpawnClass ? "on" : "off");
    else
        Log("More enemies: OFF. Set MoreEnemies=1 in BrethrenPatch.ini to "
            "turn it on.");

    Log("BrethrenPatch ready");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);

        // Nothing heavy under the loader lock.
        if (HANDLE thread = CreateThread(nullptr, 0, Init, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
