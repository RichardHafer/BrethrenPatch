// Depth of field gets weaker the higher the resolution, because the blur is
// specified in pixels.
//
// This hooks ScreenDof::UpdateLensDof (PC 0x009CAF00, PDB D436B0) and scales
// minRadius/maxRadius against a 720-high reference for the duration of the
// call, restoring them afterwards so the factor can't compound.
//
// It does not currently fix anything, and it is off by default.
//
// Measured in-game on 26.08.2026 with visible blur on screen: the params at
// 0x0124CA10 read alpha 1.0, dofType 0x04000000, that is GaussianBlur1_2 -
// while minRadius and maxRadius stay at 0.0. DS3 has five DoF paths and the PC
// build takes the gaussian one; minRadius/maxRadius belong to the lens path
// and are structurally always zero. So this scales nothing.
//
// Two more things learned the hard way, worth keeping:
//   * the main menu blur is not ScreenDof at all (alpha 0, dofType 0 while the
//     screen is visibly blurred). Testing DoF in the menu proves nothing.
//   * UpdateLensDof takes THREE arguments on PC, not the one the PDB signature
//     suggests, and the two trailing ones are output pointers. Passing only one
//     made the game write through a garbage pointer. Crash dump 58236 at
//     0x009CAF80, write to 0x27A3D2BF. The ret tells you the convention, not
//     the argument count; count the pushes at a call site.
//
// The real target is Render_DOFGaussianBlur (PDB D7B9E0). The shader selection
// for it sits at 0x009ECEB8, picking from a table at 0x0124E3C8.

#include "Common.hpp"

static safetyhook::InlineHook gUpdateLensDof;
static float* gMinRadius = nullptr;
static float* gMaxRadius = nullptr;
static volatile long gApplied = 0;
static volatile long gSkipped = 0;

static constexpr float kReferenceHeight = 720.0f;

static bool ValidRadius(float value)
{
    return value >= 0.0f && value < 10000.0f;
}

// Three arguments, not one. See the note at the top of the file.
static void __cdecl UpdateLensDof_Hook(void* renderContext, void* out1, void* out2)
{
    float oldMin = 0.0f;
    float oldMax = 0.0f;
    bool scaled = false;

    __try
    {
        const int height = g.Height();
        if (height > (int)kReferenceHeight && gMinRadius && gMaxRadius &&
            ValidRadius(*gMinRadius) && ValidRadius(*gMaxRadius))
        {
            oldMin = *gMinRadius;
            oldMax = *gMaxRadius;

            // Zero is a legitimate "no lens blur" value. Calling through is
            // still necessary, but no write is needed in that case.
            if (oldMin > 0.0001f || oldMax > 0.0001f)
            {
                const float factor = ((float)height / kReferenceHeight) * cfg::dofScale;
                *gMinRadius = oldMin * factor;
                *gMaxRadius = oldMax * factor;
                scaled = true;

                const long count = InterlockedIncrement(&gApplied);
                if (count <= 4)
                    LogNow("depth of field: UpdateLensDof %.3fx  min %.5f -> %.5f  "
                           "max %.5f -> %.5f",
                           factor, oldMin, *gMinRadius, oldMax, *gMaxRadius);
            }
        }
        else
        {
            InterlockedIncrement(&gSkipped);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Do not make a visual fix capable of taking down the renderer.
        gMinRadius = nullptr;
        gMaxRadius = nullptr;
        LogNow("depth of field: UpdateLensDof parameter block became unreadable; disabled");
    }

    gUpdateLensDof.ccall<void>(renderContext, out1, out2);

    if (scaled)
    {
        __try
        {
            *gMinRadius = oldMin;
            *gMaxRadius = oldMax;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogNow("depth of field: could not restore Lens DoF parameters");
        }
    }
}

void ApplyDepthOfField()
{
    if (!cfg::depthOfField)
        return;

    // Function entry for ScreenDof::UpdateLensDof. The status-byte operand
    // at +0x1B is ScreenDof::Params + 0x4C; derive the base instead of baking
    // in the image address. This keeps the radius pointers tied to the
    // signature we actually matched.
    const DWORD updateLensDof = Find(
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 "
        "8B EC 83 EC 3C F6 05 ?? ?? ?? ?? 04 D9 05 ?? ?? ?? ?? D9 5D FC",
        "ScreenDof UpdateLensDof");
    if (!updateLensDof)
        return;

    const DWORD paramsStatus = *reinterpret_cast<const DWORD*>(updateLensDof + 0x1B);
    if (paramsStatus < 0x4C)
    {
        Log("depth of field: UpdateLensDof parameter base invalid");
        return;
    }

    const DWORD paramsBase = paramsStatus - 0x4C;
    gMinRadius = reinterpret_cast<float*>(paramsBase + 0x30);
    gMaxRadius = reinterpret_cast<float*>(paramsBase + 0x34);

    gUpdateLensDof = Hook(reinterpret_cast<void*>(updateLensDof),
                           reinterpret_cast<void*>(&UpdateLensDof_Hook));
    Log("depth of field: UpdateLensDof hook %08X %s, Params=%08X "
        "(min=%08X max=%08X, reference %.0fp, %.0f%%)",
        updateLensDof, gUpdateLensDof ? "active" : "FAILED", paramsBase,
        (DWORD)gMinRadius, (DWORD)gMaxRadius, kReferenceHeight,
        cfg::dofScale * 100.0f);
}
