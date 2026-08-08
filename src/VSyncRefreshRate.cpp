// With VSync enabled the game locks to 30 FPS regardless of the display's
// refresh rate.
//
// This is not a D3D9 presentation interval: the swap chain always reports
// D3DPRESENT_INTERVAL_IMMEDIATE and no Reset happens when VSync is toggled.
// The whole main loop runs at 30 Hz, verified by counting frames against the
// wall clock (1800 frames per 60.0 s).
//
// The cause is the engine's frame rate lock multiplier. At 0x005C3BC0 the game
// pushes 2 when VSync is on and 0 when it is off, then calls
// SetFrameRateLockMultiplier. A multiplier of 2 against a 60 Hz vblank is
// exactly the 30 FPS observed. The DS2 patch compensates for the same
// mechanism on the target frame time instead ("divide by 2 since a1 = 2").
//
// The function itself is three stores, so it is hooked rather than patching the
// single literal push: there are four call sites and three of them compute the
// value at runtime, including the in-game toggle.

#include "Common.hpp"
#include <d3d9.h>

using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static safetyhook::InlineHook gDirect3DCreate9;
static safetyhook::InlineHook gSetMultiplier;

static CreateDevice_t oCreateDevice = nullptr;
static Reset_t oReset = nullptr;

static const char* IntervalName(UINT interval)
{
    switch (interval)
    {
    case D3DPRESENT_INTERVAL_DEFAULT:   return "default";
    case D3DPRESENT_INTERVAL_ONE:       return "one";
    case D3DPRESENT_INTERVAL_TWO:       return "two (half rate)";
    case D3DPRESENT_INTERVAL_THREE:     return "three";
    case D3DPRESENT_INTERVAL_FOUR:      return "four";
    case D3DPRESENT_INTERVAL_IMMEDIATE: return "immediate (off)";
    default:                            return "unknown";
    }
}

// Kept as a safety net in case a build does use a half-rate interval.
static void ClampInterval(const char* where, D3DPRESENT_PARAMETERS* pp)
{
    if (!pp)
        return;

    const UINT interval = pp->PresentationInterval;
    const bool halfRate = interval == D3DPRESENT_INTERVAL_TWO ||
                          interval == D3DPRESENT_INTERVAL_THREE ||
                          interval == D3DPRESENT_INTERVAL_FOUR;

    if (halfRate)
        pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    // Device resets happen while playing, so do not log every one of them.
    static long reported = 0;
    if (++reported > 4)
        return;

    Log("d3d9 %s: %ux%u @%u Hz, windowed %d, interval %s%s", where,
        pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz,
        (int)pp->Windowed, IntervalName(interval), halfRate ? " -> one" : "");
}

// Several devices share one vtable, so skip if our detour is already in place;
// otherwise the stored "original" would point back at us.
static void* PatchVTable(void* object, int index, void* detour, void* known)
{
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[index] == detour)
        return known;

    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old))
        return known;

    void* original = vtable[index];
    vtable[index] = detour;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return original;
}

static HRESULT STDMETHODCALLTYPE Reset_Hook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
{
    ClampInterval("reset", pp);
    return oReset(device, pp);
}

static HRESULT STDMETHODCALLTYPE CreateDevice_Hook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
                                                   HWND window, DWORD flags,
                                                   D3DPRESENT_PARAMETERS* pp,
                                                   IDirect3DDevice9** out)
{
    ClampInterval("create device", pp);
    const HRESULT hr = oCreateDevice(d3d, adapter, type, window, flags, pp, out);

    // The game creates a throwaway device before the real one, so hook both.
    if (SUCCEEDED(hr) && out && *out)
        oReset = (Reset_t)PatchVTable(*out, 16, &Reset_Hook, (void*)oReset);

    return hr;
}

static IDirect3D9* WINAPI Direct3DCreate9_Hook(UINT sdkVersion)
{
    IDirect3D9* d3d = gDirect3DCreate9.stdcall<IDirect3D9*>(sdkVersion);
    if (d3d)
        oCreateDevice = (CreateDevice_t)PatchVTable(d3d, 16, &CreateDevice_Hook, (void*)oCreateDevice);
    return d3d;
}

static void __cdecl SetMultiplier_Hook(unsigned multiplier, unsigned id)
{
    const unsigned requested = multiplier;
    if (multiplier > 1 && cfg::vsyncMultiplier >= 0)
        multiplier = (unsigned)cfg::vsyncMultiplier;

    // The game re-applies this around cutscenes, so log the first few only;
    // writing to a file mid-frame is exactly what causes a hitch.
    static long reported = 0;
    if (requested != multiplier && ++reported <= 4)
        Log("frame rate lock: multiplier %u -> %u (id %08X)", requested, multiplier, id);

    gSetMultiplier.call<void>(multiplier, id);
}

// Must run before the device is created, so this is installed early.
void ApplyVSyncD3D9()
{
    if (!cfg::vsyncRefreshRate)
    {
        Log("VSyncRefreshRate: disabled");
        return;
    }

    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9)
    {
        Log("VSyncRefreshRate: d3d9.dll unavailable");
        return;
    }

    void* create = (void*)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create)
    {
        Log("VSyncRefreshRate: Direct3DCreate9 not exported");
        return;
    }

    gDirect3DCreate9 = Hook(create, &Direct3DCreate9_Hook);
    Log("VSyncRefreshRate: watching d3d9");
}

// Needs unpacked game code, so this runs after the unpack wait.
void ApplyFrameRateLock()
{
    if (!cfg::vsyncRefreshRate)
        return;

    const DWORD address = Find("55 8B EC 8B 45 08 8B 4D 0C C7 05 ?? ?? ?? ?? 01 00 00 00 A3",
                               "SetFrameRateLockMultiplier");
    if (!address)
        return;

    // The globals are read out of the function body rather than hardcoded:
    //   +9  C7 05 <flag> 01 00 00 00
    //   +19 A3    <multiplier>
    const DWORD flagAddress = mem::Read<DWORD>(address + 11);
    const DWORD multiplierAddress = mem::Read<DWORD>(address + 20);

    gSetMultiplier = Hook((void*)address, &SetMultiplier_Hook);
    Log("frame rate lock: hooked, target multiplier %d", cfg::vsyncMultiplier);

    // Settings are parsed before the hook exists, so correct a value that is
    // already in place.
    const bool sane = multiplierAddress > 0x400000 && multiplierAddress < 0x16D6000;
    if (sane && mem::Read<DWORD>(multiplierAddress) > 1 && cfg::vsyncMultiplier >= 0)
    {
        mem::Write<DWORD>(multiplierAddress, (DWORD)cfg::vsyncMultiplier);
        if (flagAddress > 0x400000 && flagAddress < 0x16D6000)
            mem::Write<DWORD>(flagAddress, 1);
        Log("frame rate lock: corrected the value already set");
    }
}
