// BrethrenPatch - fixes for the PC port of Dead Space 3.
//
// Loaded as an ASI early in the process, before the D3D9 device exists.

#include "Common.hpp"

void ApplyHavokPhysics();
void ApplyHighCoreCpu();
void ApplyVSyncD3D9();
void ApplyFrameRateLock();
void ApplyUiScaling();

// The anchor used to tell whether the packer has finished; see WaitForGameCode.
static const char* kUnpackAnchor = "55 8B EC 83 E4 F0 81 EC A4 00 00 00 8B 55 08";

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

    cfg::havokPhysics     = flag("HavokPhysicsFix", true);
    cfg::highCoreCpu      = flag("HighCoreCPUFix", false);
    cfg::vsyncRefreshRate = flag("VSyncRefreshRateFix", true);
    cfg::uiScaling        = flag("UiScaling", true);
    cfg::log              = flag("DebugLogging", true);

    cfg::cpuCoreCap       = number("CpuCoreCap", 8);
    cfg::vsyncMultiplier  = number("VSyncMultiplier", 1);
    cfg::ragdollMessageId = number("RagdollMessageId", 0x1F);
    cfg::uiScale          = number("UiScalePercent", 100) / 100.0f;

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
    if (g.initialised)
        return 0;

    g.module = GetModuleHandleA(nullptr);
    if (!g.module)
        return 0;

    LogInit();
    LoadSettings();

    Log("BrethrenPatch starting, module %08X", (unsigned)(uintptr_t)g.module);
    Log("havok %d, cpu %d, vsync %d (multiplier %d), ui %d",
        (int)cfg::havokPhysics, (int)cfg::highCoreCpu, (int)cfg::vsyncRefreshRate,
        cfg::vsyncMultiplier, (int)cfg::uiScaling);

    // Neither of these touches game code, and both have to happen early.
    ApplyHighCoreCpu();
    ApplyVSyncD3D9();

    if (WaitForGameCode())
    {
        ApplyFrameRateLock();
        ApplyUiScaling();
        ApplyHavokPhysics();
    }

    g.initialised = true;
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
