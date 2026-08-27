// Stops the game from moving its own slider in the Windows volume mixer.
//
// DS3 imports waveOutSetVolume from winmm and uses the old wave output path
// alongside its own mixer. Up to XP that call set the device volume; since
// Vista it is redirected to the per-application session volume, so the game
// thinks it is setting its own level and actually moves the user's. The wave
// path opens for the menu video audio, which is why it happens in the lobby
// already.
//
// The call is swallowed and answered with MMSYSERR_NOERROR. DS3 keeps its own
// volume internally (EA::Audio), so nothing is lost. Measured: it asks for
// 0xE665E665, which is 90% on both channels, three times during startup from
// the same caller.
//
// Careful with the module handle: our own ASI loader is also called winmm.dll
// and sits in the game folder, so GetModuleHandleW would likely return that
// one. Load the system copy by full path instead.

#include "Common.hpp"

#include <mmsystem.h>

typedef UINT (WINAPI *WaveOutSetVolumeFn)(HWAVEOUT, DWORD);

static safetyhook::InlineHook gWaveOutSetVolume;
static volatile long gBlocked = 0;

static UINT WINAPI WaveOutSetVolume_Hook(HWAVEOUT hwo, DWORD volume)
{
    const long n = InterlockedIncrement(&gBlocked);
    if (n <= 8)
    {
        // Left channel in the low 16 bits, right in the high; 0xFFFF is full.
        const unsigned links  = volume & 0xFFFF;
        const unsigned rechts = (volume >> 16) & 0xFFFF;
        LogNow("Windows volume: swallowed waveOutSetVolume(%p, %08X) "
               "- left %.1f%%, right %.1f%%, caller %08X (#%ld)",
               hwo, volume, links * 100.0 / 65535.0, rechts * 100.0 / 65535.0,
               static_cast<unsigned>(
                   reinterpret_cast<uintptr_t>(_ReturnAddress())), n);
    }

    if (!cfg::keepWindowsVolume)
        return gWaveOutSetVolume.stdcall<UINT>(hwo, volume);

    return MMSYSERR_NOERROR;      // pretend it worked
}

void ApplyWindowsVolume()
{
    if (!cfg::keepWindowsVolume && !cfg::volumeDiagnostics)
        return;

    wchar_t path[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (!len || len > MAX_PATH - 16)
    {
        Log("Windows volume: cannot determine the system directory");
        return;
    }
    wcscat_s(path, L"\\winmm.dll");

    // Full path on purpose; see the note at the top about our own winmm.
    const HMODULE winmm = LoadLibraryW(path);
    if (!winmm)
    {
        Log("Windows volume: cannot load %ls", path);
        return;
    }

    void* ziel = reinterpret_cast<void*>(
        GetProcAddress(winmm, "waveOutSetVolume"));
    if (!ziel)
    {
        Log("Windows volume: waveOutSetVolume not found");
        return;
    }

    gWaveOutSetVolume = Hook(ziel,
                             reinterpret_cast<void*>(&WaveOutSetVolume_Hook));
    Log("Windows volume: waveOutSetVolume %p %s, calls are %s",
        ziel, gWaveOutSetVolume ? "hooked" : "NOT hooked",
        cfg::keepWindowsVolume ? "discarded" : "logged only");
}
