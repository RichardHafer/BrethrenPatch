// Measurement only. Nothing here changes what the game does; it reads values out
// of two spots in the kinesis follow code and writes them to the log.
//
// The question being measured: a held corpse follows the crosshair with weight
// and lag up to about 60 FPS, but snaps to it instantly at high frame rates.
// That is the signature of a step taken once per frame instead of once per unit
// of time. If one of the inputs below changes with the frame rate when it should
// not, that input is the bug.
//
// Both fields are read exactly once in the whole executable, in one function:
//
//   0x0046E765   fld [eax+0x638]   CMD_SetTKMovePower       (default 50)
//   0x0046E833   fld [eax+0x63C]   CMD_SetTKEaseOutDistance (default 5)
//
// and the surrounding code computes
//
//   speed = MovePower * scale * [ebp-0x2C] / [ebp-0x1C]
//
// so the three locals are logged alongside the attribute. Frame time comes from
// the interval between consecutive calls: the first site runs once per frame
// while an object is held, which makes it its own clock.

#include "Common.hpp"

static safetyhook::MidHook gMovePowerSite;
static safetyhook::MidHook gEaseOutSite;

static LARGE_INTEGER gFrequency{};
static LARGE_INTEGER gLastCall{};
static LARGE_INTEGER gLastLog{};

static long gMoveLines = 0;
static long gEaseLines = 0;

// Writing to a file inside a frame is what caused hitches before, so this keeps
// the rate low enough to be free and the total bounded.
static const long kMaxLines = 400;
static const double kMinInterval = 0.25;

static bool ShouldLog(long& counter)
{
    if (counter >= kMaxLines)
        return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (gLastLog.QuadPart != 0)
    {
        const double since = double(now.QuadPart - gLastLog.QuadPart) / double(gFrequency.QuadPart);
        if (since < kMinInterval)
            return false;
    }

    gLastLog = now;
    ++counter;
    return true;
}

static float FrameTime()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double seconds = 0.0;
    if (gLastCall.QuadPart != 0)
        seconds = double(now.QuadPart - gLastCall.QuadPart) / double(gFrequency.QuadPart);

    gLastCall = now;
    return (float)seconds;
}

static void MovePowerSite(safetyhook::Context& ctx)
{
    const float frameTime = FrameTime();
    if (!ShouldLog(gMoveLines))
        return;

    __try
    {
        const float movePower = *(const float*)(ctx.eax + 0x638);
        const float scale     = *(const float*)(ctx.ebp - 0x18);
        const float factor    = *(const float*)(ctx.ebp - 0x2C);
        const float divisor   = *(const float*)(ctx.ebp - 0x1C);

        const float speed = divisor != 0.0f ? movePower * scale * factor / divisor : 0.0f;
        const float fps = frameTime > 0.0f ? 1.0f / frameTime : 0.0f;

        Log("TK move   fps %6.1f  power %8.3f  scale %8.4f  factor %10.4f  div %10.4f  speed %12.4f",
            fps, movePower, scale, factor, divisor, speed);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TK move   read failed");
    }
}

static void EaseOutSite(safetyhook::Context& ctx)
{
    if (!ShouldLog(gEaseLines))
        return;

    __try
    {
        const float easeOut  = *(const float*)(ctx.eax + 0x63C);
        const float divisor  = *(const float*)(ctx.ebp - 0x1C);
        const float value    = *(const float*)(ctx.ebp - 0x0C);
        const float distance = *(const float*)(ctx.ebp - 0x10);

        Log("TK ease   easeOut %8.3f  div %10.4f  value %10.4f  distance %10.4f",
            easeOut, divisor, value, distance);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("TK ease   read failed");
    }
}

// Corpses stiffen up as the frame rate rises. A ragdoll is not pure physics: the
// game blends between the animated pose and the simulated one, and
// CMD_RagdollBlendFraction (offset 0xBC, default 0.25) is that blend. A fraction
// of 0.25 taken once per frame converges more than four times as fast at 144 FPS
// as at 30, which would hold a corpse against its animation pose, exactly what
// "stiff" looks like.
//
// The site at 0x007BA89D picks a random value between the fraction at +0xBC and a
// second one at +0xC0. Whether that happens per frame or once per corpse decides
// whether this is the cause, and the global random counter at 0x01242C80 cannot
// answer it: measured at a steady 860 per second, it is shared by every consumer
// of that table. Hence counting this one site.
static safetyhook::MidHook gBlendSite;

static long gBlendCalls = 0;
static LARGE_INTEGER gBlendWindow{};

static void BlendSite(safetyhook::Context& ctx)
{
    ++gBlendCalls;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (gBlendWindow.QuadPart == 0)
    {
        gBlendWindow = now;
        return;
    }

    const double elapsed = double(now.QuadPart - gBlendWindow.QuadPart) / double(gFrequency.QuadPart);
    if (elapsed < 2.0)
        return;

    static long windows = 0;
    if (++windows <= 10)
    {
        // frameTimeScale is deltaTime measured against 30 FPS, so multiplying by
        // 30 gives the frame rate to compare the call rate against.
        const float fps = g.frameTimeScale * 30.0f;

        float low = 0.0f, high = 0.0f;
        __try
        {
            low = *(const float*)(ctx.eax + 0xBC);
            high = *(const float*)(ctx.eax + 0xC0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        Log("ragdoll blend: %.1f calls per second at about %.0f FPS (range %.3f..%.3f)",
            gBlendCalls / elapsed, fps, low, high);
    }

    gBlendCalls = 0;
    gBlendWindow = now;
}

void ApplyDiagnostics()
{
    if (!cfg::tkDiagnostics)
        return;

    QueryPerformanceFrequency(&gFrequency);
    if (gFrequency.QuadPart == 0)
    {
        Log("TK diagnostics: no performance counter");
        return;
    }

    const DWORD movePower = Find("D9 80 38 06 00 00 D8 4D E8 D8 4D D4", "TKMovePower read");
    const DWORD easeOut   = Find("D9 80 3C 06 00 00 D9 5D E8 85 C9", "TKEaseOutDistance read");

    if (movePower)
        gMovePowerSite = safetyhook::create_mid((void*)movePower, &MovePowerSite);
    if (easeOut)
        gEaseOutSite = safetyhook::create_mid((void*)easeOut, &EaseOutSite);

    Log("TK diagnostics: active, up to %ld lines per site, one line per %.0f ms",
        kMaxLines, kMinInterval * 1000.0);
    Log("TK diagnostics: carry a corpse with kinesis and swing it around; nothing is logged otherwise");

    const DWORD blend = Find("D9 80 BC 00 00 00 8B 0D 80 2C 24 01 D9 5D EC", "ragdoll blend fraction");
    if (blend)
        gBlendSite = safetyhook::create_mid((void*)blend, &BlendSite);
    Log("ragdoll blend: %s", gBlendSite ? "counting" : "not hooked");
}
