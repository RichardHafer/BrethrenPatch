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
#include <cmath>

using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*,
                                                        IDirect3DPixelShader9**);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT,
                                                               const float*, UINT);

static safetyhook::InlineHook gDirect3DCreate9;
static safetyhook::InlineHook gSetMultiplier;

static CreateDevice_t oCreateDevice = nullptr;
static Reset_t oReset = nullptr;
static CreatePixelShader_t oCreatePixelShader = nullptr;
static SetPixelShader_t oSetPixelShader = nullptr;
static SetPixelShaderConstantF_t oSetPixelShaderConstantF = nullptr;

// This is intentionally a trace, not another speculative DoF fix.  The
// active DoF path did not enter either CPU-side parameter hook.  D3D9's
// SetPixelShaderConstantF is the point at which the final values reach the
// GPU, irrespective of which ScreenDof implementation prepared them.
static IDirect3DPixelShader9* gCurrentPixelShader = nullptr;
struct DofWidthBinding
{
    IDirect3DPixelShader9* shader = nullptr;
    const DWORD* bytecode = nullptr;
    UINT firstRegister = 0;
    UINT registerCount = 0;
    volatile LONG reported = 0;
    volatile LONG captured = 0;
};
static DofWidthBinding gDofWidthBindings[512]{};
static volatile LONG gDofWidthBindingCount = 0;
static volatile LONG gCreatedDofShaderCaptured = 0;
struct ShaderConstantSample
{
    IDirect3DPixelShader9* shader = nullptr;
    UINT startRegister = 0;
    UINT vectorCount = 0;
    float firstVector[4]{};
    LONG calls = 0;
};
static ShaderConstantSample gShaderSamples[64]{};
static volatile LONG gShaderSampleCount = 0;

static size_t ShaderBytecodeSize(const DWORD* bytecode)
{
    if (!bytecode)
        return 0;
    __try
    {
        for (size_t words = 0; words < 16 * 1024; ++words)
            if (bytecode[words] == 0x0000FFFF)
                return (words + 1) * sizeof(DWORD);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

static bool FindDofWidthBinding(const DWORD* bytecode, UINT& firstRegister,
                                UINT& registerCount)
{
    const size_t bytecodeSize = ShaderBytecodeSize(bytecode);
    if (!bytecodeSize)
        return false;

    // D3D9 CTAB layout after the 'CTAB' marker.  ConstantInfo is 20 bytes:
    // name offset, register set (WORD), register index (WORD), register
    // count (WORD), reserved (WORD), type-info offset, default-value offset.
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(bytecode);
    __try
    {
        for (size_t marker = 0; marker + 24 < bytecodeSize; ++marker)
        {
            if (std::memcmp(bytes + marker, "CTAB", 4) != 0)
                continue;
            const unsigned char* header = bytes + marker + 4;
            DWORD constants = 0, infoOffset = 0;
            std::memcpy(&constants, header + 12, sizeof(constants));
            std::memcpy(&infoOffset, header + 16, sizeof(infoOffset));
            if (!constants || constants > 1024 || infoOffset >= bytecodeSize)
                continue;

            // The offsets in retail CTABs are relative to the header.  Keep
            // the marker variant as a compatibility fallback for a driver
            // that preserves the FourCC as part of the table base.
            const unsigned char* bases[2] = { header, bytes + marker };
            for (const unsigned char* base : bases)
            {
                if (base < bytes || base > bytes + bytecodeSize ||
                    infoOffset > (size_t)(bytes + bytecodeSize - base) ||
                    constants > (size_t)(bytes + bytecodeSize - base - infoOffset) / 20)
                    continue;
                const unsigned char* info = base + infoOffset;
                for (DWORD i = 0; i < constants; ++i)
                {
                    const unsigned char* entry = info + i * 20;
                    DWORD nameOffset = 0;
                    WORD index = 0, count = 0;
                    std::memcpy(&nameOffset, entry, sizeof(nameOffset));
                    std::memcpy(&index, entry + 6, sizeof(index));
                    std::memcpy(&count, entry + 8, sizeof(count));
                    if (nameOffset >= bytecodeSize || !count || count > 256 ||
                        nameOffset > (size_t)(bytes + bytecodeSize - base) - 11)
                        continue;
                    const char* name = reinterpret_cast<const char*>(base + nameOffset);
                    if (name < reinterpret_cast<const char*>(bytes) ||
                        name + 11 > reinterpret_cast<const char*>(bytes + bytecodeSize))
                        continue;
                    if (std::strcmp(name, "dof_widths") == 0)
                    {
                        firstRegister = index;
                        registerCount = count;
                        return true;
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // CTAB discovery is diagnostic-only.  A malformed/driver-owned blob
        // simply contributes no binding; it must never affect rendering.
    }
    return false;
}

static void RegisterDofWidthBinding(IDirect3DPixelShader9* shader, const DWORD* bytecode)
{
    UINT firstRegister = 0, registerCount = 0;
    if (!FindDofWidthBinding(bytecode, firstRegister, registerCount))
    {
        // This fallback is not a guess: BrethrenPatch_DofWidthsShader.bin was
        // captured from this exact `dof_widths` marker and disassembled with
        // fxc. It declares dof_color=c0, dof_params=c1 and dof_widths=c2;
        // the bytecode uses c2.xyxy as the offsets for eighteen bb_texture
        // samples.  The retail CTAB offset encoding is not uniform enough for
        // the generic reader above, but this verified shader layout is.
        firstRegister = 2;
        registerCount = 1;
    }
    const LONG slot = InterlockedIncrement(&gDofWidthBindingCount) - 1;
    if (slot < 0 || slot >= (LONG)_countof(gDofWidthBindings))
        return;
    auto& binding = gDofWidthBindings[slot];
    binding.shader = shader;
    binding.bytecode = bytecode;
    binding.firstRegister = firstRegister;
    binding.registerCount = registerCount;
}

static void CaptureDofShaderAtCreation(IDirect3DPixelShader9* shader, const DWORD* bytecode)
{
    if (!cfg::dofShaderDiagnostics || !bytecode ||
        InterlockedIncrement(&gCreatedDofShaderCaptured) != 1)
        return;
    const size_t bytecodeSize = ShaderBytecodeSize(bytecode);
    if (!bytecodeSize)
        return;

    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (!slash)
        slash = std::strrchr(path, '/');
    if (!slash)
        return;
    slash[1] = '\0';
    std::strncat(path, "BrethrenPatch_DofWidthsShader.bin",
                 MAX_PATH - std::strlen(path) - 1);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file)
        return;
    const size_t words = bytecodeSize / sizeof(DWORD);
    const size_t written = std::fwrite(bytecode, sizeof(DWORD), words, file);
    std::fclose(file);
    LogNow("DoF shader capture: %zu bytes from CTAB dof_widths shader=%p -> %s%s",
           written * sizeof(DWORD), shader, path,
           written == words ? "" : " (SHORT WRITE)");
}

static void CaptureActiveDofShader(DofWidthBinding& binding)
{
    if (!cfg::dofShaderDiagnostics || !binding.bytecode ||
        InterlockedIncrement(&binding.captured) != 1)
        return;

    // D3D9 shader bytecode ends in 0x0000FFFF.  Save exactly one active,
    // CTAB-confirmed shader rather than every shader that merely declares the
    // shared uniform.  The capture is an original engine asset and is used
    // only for offline disassembly; no device state is touched.
    size_t words = 0;
    __try
    {
        while (words < 16 * 1024)
        {
            if (binding.bytecode[words++] == 0x0000FFFF)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogNow("DoF shader capture: bytecode became unreadable");
        return;
    }
    if (!words || words >= 16 * 1024)
    {
        LogNow("DoF shader capture: no D3D9 end token found");
        return;
    }

    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (!slash)
        slash = std::strrchr(path, '/');
    if (!slash)
        return;
    slash[1] = '\0';
    std::strncat(path, "BrethrenPatch_ActiveDofShader.bin",
                 MAX_PATH - std::strlen(path) - 1);

    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file)
    {
        LogNow("DoF shader capture: cannot write %s", path);
        return;
    }
    const size_t written = std::fwrite(binding.bytecode, sizeof(DWORD), words, file);
    std::fclose(file);
    LogNow("DoF shader capture: %zu bytes from shader=%p c%u -> %s%s",
           written * sizeof(DWORD), binding.shader, binding.firstRegister,
           path, written == words ? "" : " (SHORT WRITE)");
}

static void RecordDofWidthUpload(UINT startRegister, const float* data, UINT vectorCount)
{
    if (!cfg::dofShaderDiagnostics || !data || !vectorCount)
        return;
    const LONG known = InterlockedCompareExchange(&gDofWidthBindingCount, 0, 0);
    const LONG bounded = known < (LONG)_countof(gDofWidthBindings)
        ? known : (LONG)_countof(gDofWidthBindings);
    for (LONG i = 0; i < bounded; ++i)
    {
        auto& binding = gDofWidthBindings[i];
        if (binding.shader != gCurrentPixelShader ||
            startRegister > binding.firstRegister ||
            startRegister + vectorCount <= binding.firstRegister)
            continue;
        CaptureActiveDofShader(binding);
        if (InterlockedIncrement(&binding.reported) > 3)
            return;
        const UINT at = binding.firstRegister - startRegister;
        const float* values = data + at * 4;
        LogNow("DoF width upload: shader=%p c%u count=%u values=(%.7f %.7f %.7f %.7f)",
               binding.shader, binding.firstRegister, binding.registerCount,
               values[0], values[1], values[2], values[3]);
        return;
    }
}

static const float* ScaleDofWidthUpload(UINT startRegister, const float* data,
                                        UINT vectorCount, float (&scaled)[4])
{
    if (!cfg::depthOfField || !cfg::dofShaderFix || !data || vectorCount != 1 ||
        g.Height() <= 720)
        return data;

    const LONG known = InterlockedCompareExchange(&gDofWidthBindingCount, 0, 0);
    const LONG bounded = known < (LONG)_countof(gDofWidthBindings)
        ? known : (LONG)_countof(gDofWidthBindings);
    for (LONG i = 0; i < bounded; ++i)
    {
        auto& binding = gDofWidthBindings[i];
        // The engine uploads this uniform as one float4.  Requiring the exact
        // register and vector count is intentional: a broad batched upload
        // could contain unrelated uniforms beside it and must be left alone.
        if (binding.shader != gCurrentPixelShader ||
            binding.firstRegister != startRegister || binding.registerCount != 1)
            continue;

        std::memcpy(scaled, data, sizeof(scaled));
        const float factor = ((float)g.Height() / 720.0f) * cfg::dofScale;
        scaled[0] *= factor;
        scaled[1] *= factor;
        if (InterlockedIncrement(&binding.reported) <= 3)
            LogNow("depth of field: CTAB dof_widths shader=%p c%u factor %.3f "
                   "xy %.5f %.5f -> %.5f %.5f", binding.shader,
                   binding.firstRegister, factor, data[0], data[1],
                   scaled[0], scaled[1]);
        return scaled;
    }
    return data;
}

static bool ShaderBytecodeContains(const DWORD* bytecode, const char* needle)
{
    if (!bytecode || !needle)
        return false;

    const size_t bytecodeSize = ShaderBytecodeSize(bytecode);
    if (!bytecodeSize)
        return false;
    const size_t length = std::strlen(needle);
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(bytecode);
    // Do not search a fixed heap range.  A previous version did that and
    // reported an unrelated neighbouring string as dof_widths; the captured
    // shader was then conclusively disassembled as SSAO.  The D3D9 END token
    // gives us the exact, safe extent of this shader blob.
    __try
    {
        for (size_t i = 0; i + length <= bytecodeSize; ++i)
            if (std::memcmp(bytes + i, needle, length) == 0)
                return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A driver-owned shader blob may end at a page boundary.  A missing
        // metadata string is harmless; a diagnostic must never destabilize
        // rendering merely to read past such a boundary.
    }
    return false;
}

static HRESULT STDMETHODCALLTYPE CreatePixelShader_Hook(IDirect3DDevice9* device,
                                                         const DWORD* bytecode,
                                                         IDirect3DPixelShader9** out)
{
    const HRESULT hr = oCreatePixelShader(device, bytecode, out);
    if ((!cfg::dofShaderDiagnostics && !(cfg::depthOfField && cfg::dofShaderFix)) ||
        FAILED(hr) || !out || !*out)
        return hr;

    // HLSL constant-table strings survive in many retail D3D9 shaders.  If
    // present, this identifies the exact object without a pointer/address
    // assumption and without modifying any rendering state.
    const char* tag = nullptr;
    if (ShaderBytecodeContains(bytecode, "dof_widths")) tag = "dof_widths";
    else if (ShaderBytecodeContains(bytecode, "LensDof")) tag = "LensDof";
    else if (ShaderBytecodeContains(bytecode, "ScreenDof")) tag = "ScreenDof";
    else if (ShaderBytecodeContains(bytecode, "Dof")) tag = "Dof";
    if (tag)
    {
        if (std::strcmp(tag, "dof_widths") == 0)
        {
            RegisterDofWidthBinding(*out, bytecode);
            CaptureDofShaderAtCreation(*out, bytecode);
        }
        if (cfg::dofShaderDiagnostics)
            LogNow("DoF shader trace: created pixel shader=%p metadata=%s", *out, tag);
    }
    return hr;
}

static void RecordPixelConstants(UINT startRegister, const float* data, UINT vectorCount)
{
    if (!cfg::dofShaderDiagnostics || !data || !vectorCount)
        return;

    // A shader normally submits many constants each frame.  One record for
    // each (shader, register, vector count) triple is enough to identify the
    // DoF material and guarantees that diagnostics cannot turn into per-frame
    // disk I/O or a stutter source.
    const LONG known = InterlockedCompareExchange(&gShaderSampleCount, 0, 0);
    const LONG boundedKnown = known < (LONG)_countof(gShaderSamples)
        ? known : (LONG)_countof(gShaderSamples);
    for (LONG i = 0; i < boundedKnown; ++i)
    {
        auto& sample = gShaderSamples[i];
        if (sample.shader == gCurrentPixelShader &&
            sample.startRegister == startRegister && sample.vectorCount == vectorCount)
        {
            InterlockedIncrement(&sample.calls);
            return;
        }
    }

    if (known >= (LONG)_countof(gShaderSamples))
        return;

    const LONG slot = InterlockedIncrement(&gShaderSampleCount) - 1;
    if (slot < 0 || slot >= (LONG)_countof(gShaderSamples))
        return;

    auto& sample = gShaderSamples[slot];
    sample.shader = gCurrentPixelShader;
    sample.startRegister = startRegister;
    sample.vectorCount = vectorCount;
    std::memcpy(sample.firstVector, data, sizeof(sample.firstVector));
    sample.calls = 1;
    LogNow("DoF shader trace: shader=%p c%u vectors=%u first=(%.6f %.6f %.6f %.6f)",
           sample.shader, startRegister, vectorCount, sample.firstVector[0],
           sample.firstVector[1], sample.firstVector[2], sample.firstVector[3]);
}

static HRESULT STDMETHODCALLTYPE SetPixelShader_Hook(IDirect3DDevice9* device,
                                                       IDirect3DPixelShader9* shader)
{
    gCurrentPixelShader = shader;
    return oSetPixelShader(device, shader);
}

static HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF_Hook(IDirect3DDevice9* device,
                                                                UINT startRegister,
                                                                const float* data,
                                                                UINT vectorCount)
{
    RecordPixelConstants(startRegister, data, vectorCount);
    RecordDofWidthUpload(startRegister, data, vectorCount);
    float scaled[4]{};
    data = ScaleDofWidthUpload(startRegister, data, vectorCount, scaled);
    return oSetPixelShaderConstantF(device, startRegister, data, vectorCount);
}

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

    // The only dependable source for the resolution. The game's own dimension
    // globals still read zero well into startup, which silently disabled the
    // fixes that scale with resolution.
    if (pp->BackBufferWidth > 0 && pp->BackBufferHeight > 0)
    {
        const bool changed = g.deviceWidth != (int)pp->BackBufferWidth ||
                             g.deviceHeight != (int)pp->BackBufferHeight;

        g.deviceWidth = (int)pp->BackBufferWidth;
        g.deviceHeight = (int)pp->BackBufferHeight;

        if (changed)
            Log("resolution: %dx%d", g.deviceWidth, g.deviceHeight);
    }

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
    {
        oReset = (Reset_t)PatchVTable(*out, 16, &Reset_Hook, (void*)oReset);
        if (cfg::dofShaderDiagnostics || (cfg::depthOfField && cfg::dofShaderFix))
        {
            // IDirect3DDevice9 vtable: CreatePixelShader=106,
            // SetPixelShader=107,
            // SetPixelShaderConstantF=109.
            oCreatePixelShader = (CreatePixelShader_t)PatchVTable(*out, 106,
                &CreatePixelShader_Hook, (void*)oCreatePixelShader);
            oSetPixelShader = (SetPixelShader_t)PatchVTable(*out, 107,
                &SetPixelShader_Hook, (void*)oSetPixelShader);
            oSetPixelShaderConstantF = (SetPixelShaderConstantF_t)PatchVTable(*out, 109,
                &SetPixelShaderConstantF_Hook, (void*)oSetPixelShaderConstantF);
            Log("DoF shader trace: watching SetPixelShaderConstantF (64 unique uploads max)");
        }
    }

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
