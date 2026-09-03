#include "Common.hpp"

static safetyhook::InlineHook gGetRawInputData;
static safetyhook::InlineHook gAimingUpdate;
static volatile LONG gRawMouseSamples = 0;
static volatile LONG gRawMouseX = 0;
static volatile LONG gRawMouseY = 0;
static volatile LONG gLastRawMouseTick = 0;

namespace
{
constexpr float kDefaultMouseSensitivity = 0.005f;
constexpr float kGroundAimDivisor = 750.0f;
constexpr float kRadiansToDegrees = 57.2957795f;
constexpr DWORD kMouseActiveWindowMs = 150;
}

static UINT WINAPI GetRawInputDataHook(HRAWINPUT input, UINT command,
                                       LPVOID data, PUINT size,
                                       UINT headerSize)
{
    const UINT result = gGetRawInputData.stdcall<UINT>(
        input, command, data, size, headerSize);

    if (result == static_cast<UINT>(-1) || command != RID_INPUT || !data)
        return result;

    const RAWINPUT* raw = static_cast<const RAWINPUT*>(data);
    if (raw->header.dwType != RIM_TYPEMOUSE)
        return result;

    const LONG x = raw->data.mouse.lLastX;
    const LONG y = raw->data.mouse.lLastY;
    if (!x && !y)
        return result;

    if (cfg::rawMouseInput)
    {
        InterlockedExchangeAdd(&gRawMouseX, x);
        InterlockedExchangeAdd(&gRawMouseY, y);
        InterlockedExchange(&gLastRawMouseTick,
                            static_cast<LONG>(GetTickCount()));
    }

    if (!cfg::rawMouseInputDiagnostics)
        return result;

    const LONG sample = InterlockedIncrement(&gRawMouseSamples);
    if (sample > 16)
        return result;

    void* stack[8]{};
    const USHORT frames = RtlCaptureStackBackTrace(1, 8, stack, nullptr);
    char callers[512]{};
    size_t used = 0;

    for (USHORT i = 0; i < frames && used < sizeof(callers) - 16; ++i)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(stack[i]);
        MEMORY_BASIC_INFORMATION memory{};
        const uintptr_t moduleBase = VirtualQuery(stack[i], &memory,
            sizeof(memory)) ? reinterpret_cast<uintptr_t>(memory.AllocationBase) : 0;
        char modulePath[MAX_PATH]{};
        const char* moduleName = "unknown";
        if (moduleBase && GetModuleFileNameA(
                reinterpret_cast<HMODULE>(moduleBase), modulePath, MAX_PATH))
        {
            const char* slash = std::strrchr(modulePath, '\\');
            moduleName = slash ? slash + 1 : modulePath;
        }
        const uintptr_t offset = moduleBase ? address - moduleBase : address;
        used += static_cast<size_t>(std::snprintf(
            callers + used, sizeof(callers) - used, "%s%s+%08X",
            i ? "," : "", moduleName, static_cast<unsigned>(offset)));
    }

    LogNow("Raw mouse sample %ld: delta=(%ld,%ld), flags=%04X, stack=%s",
           sample, x, y, raw->data.mouse.usFlags, callers);
    return result;
}

// GE2Aiming::Update is the first common gameplay aiming stage after DS3 has
// copied DIMOUSESTATE2 into its controller records. The retail function turns
// those values into a velocity, smooths it, then multiplies it by frame time.
// Calling it first keeps its internal controller state current. Mouse output
// is replaced afterwards, before PlayerFPSAimSM applies pitch, yaw and aim
// assist. Menus and mouse buttons never pass through this function.
static void __fastcall AimingUpdateHook(void* self, int, float frameTime,
                                        float* horizontal, float* vertical)
{
    gAimingUpdate.thiscall<void>(self, frameTime, horizontal, vertical);

    if (!horizontal || !vertical)
        return;

    const DWORD now = GetTickCount();
    const DWORD last = static_cast<DWORD>(
        InterlockedCompareExchange(&gLastRawMouseTick, 0, 0));
    if (last == 0 || now - last > kMouseActiveWindowMs)
        return;

    const LONG rawX = InterlockedExchange(&gRawMouseX, 0);
    const LONG rawY = InterlockedExchange(&gRawMouseY, 0);
    // The Marker Patch injects radians at ApplyCameraRotation. This hook is
    // one stage earlier: ProcessGroundAiming converts GE2Aiming output from
    // degrees to radians itself, so feed it the matching degree value.
    const float scale = kDefaultMouseSensitivity * cfg::rawMouseSensitivity *
                        kRadiansToDegrees / kGroundAimDivisor;

    *horizontal = -static_cast<float>(rawX) * scale;
    *vertical = static_cast<float>(rawY) * scale;
}

void ApplyRawMouseInput()
{
    if (!cfg::rawMouseInput && !cfg::rawMouseInputDiagnostics)
        return;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        user32 = LoadLibraryW(L"user32.dll");

    void* target = user32
        ? reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"))
        : nullptr;
    if (!target)
    {
        Log("Raw mouse diagnostics: GetRawInputData is unavailable");
        return;
    }

    gGetRawInputData = Hook(target, reinterpret_cast<void*>(&GetRawInputDataHook));
    Log("Raw mouse: GetRawInputData %p %s%s",
        target, gGetRawInputData ? "hooked" : "NOT hooked",
        cfg::rawMouseInputDiagnostics ? ", maximum 16 diagnostic samples" : "");

    if (!cfg::rawMouseInput || !gGetRawInputData)
        return;

    const DWORD update = Find(
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 "
        "89 6C 24 04 8B EC 83 EC 38 56 57 33 FF 8B F1",
        "GE2Aiming::Update");
    if (!update)
        return;

    gAimingUpdate = Hook(reinterpret_cast<void*>(update),
                         reinterpret_cast<void*>(&AimingUpdateHook));
    Log("Raw mouse: ground aiming %08X %s, sensitivity %.2fx",
        update, gAimingUpdate ? "hooked" : "NOT hooked",
        cfg::rawMouseSensitivity);
}
