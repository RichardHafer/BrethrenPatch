// Havok constraint solving is tuned for a 30 FPS timestep. Above that, contact
// and ragdoll constraints get over-solved and corpses or severed limbs launch
// across the room. The solver inputs are scaled back towards the 30 FPS
// equivalent for the duration of each call, then restored.
//
// Ported from the Dead Space 2 Marker Patch. DS3 ships the same Havok build, so
// seven of the eight byte patterns matched unchanged.

#include "Common.hpp"
#include <cmath>

static safetyhook::InlineHook gSolverInit;
static safetyhook::InlineHook gAngular1D;
static safetyhook::InlineHook gBallSocket;
static safetyhook::InlineHook gContactJacobian;
static safetyhook::InlineHook gDeadState;

static safetyhook::MidHook gImpulseDamper;
static safetyhook::MidHook gErrorScaler;
static safetyhook::MidHook gMassCapture;
static safetyhook::MidHook gForceDamper;
static safetyhook::MidHook gTimestepLimiter;
static safetyhook::MidHook gFrameTick;

// Never return less than 1. Below 30 FPS the ratio inverts and the divisions
// below would amplify impulses instead of damping them, which throws limbs
// around during a stutter. Also guards against zero and NaN.
static float TimeScale(float deltaTime)
{
    if (!(deltaTime > 0.0f))
        return 1.0f;

    const float scale = kTargetFrameTime / deltaTime;
    return scale < 1.0f ? 1.0f : scale;
}

static int __fastcall SolverInit_Hook(float* self, int, float* a2, float* a3)
{
    g.frameTimeScale = TimeScale(a3[2]);
    return gSolverInit.thiscall<int>(self, a2, a3);
}

static void __cdecl ContactJacobian_Hook(__m128* a1, float* c, bool a3, __m128** a4)
{
    const float deltaTime = c[3];
    const float friction = c[9];
    const float scale = TimeScale(deltaTime);

    c[3] = deltaTime / scale;
    c[9] = friction / scale;

    gContactJacobian.call<void>(a1, c, a3, a4);

    c[3] = deltaTime;
    c[9] = friction;
}

static void __cdecl Angular1D_Hook(__m128* a1, float* c, __m128** a3)
{
    const float rhs = c[7];
    c[7] /= g.frameTimeScale;
    gAngular1D.call<void>(a1, c, a3);
    c[7] = rhs;
}

// SolveBallSocketChainConstraints reads [ecx+4] and [ecx+3] in its first two
// instructions, so ecx carries an input even though the rest of the arguments
// are on the stack. A plain __cdecl detour is free to clobber ecx, which hands
// the original a garbage pointer and crashes as soon as ragdoll chains exist.
// Hence the naked stub: save ecx on entry, restore it immediately before
// entering the trampoline. c[7] stays scaled for the whole call because the
// function reads it in three places.
static void* gBallSocketTrampoline = nullptr;

static float __cdecl BallSocketScale(float* c)
{
    const float previous = c[7];
    if (g.frameTimeScale != 0.0f)
        c[7] = previous / g.frameTimeScale;
    return previous;
}

static void __cdecl BallSocketRestore(float* c, float previous)
{
    c[7] = previous;
}

static void __declspec(naked) BallSocket_Hook()
{
    __asm {
        push ebp
        mov  ebp, esp
        sub  esp, 8
        mov  [ebp-4], ecx

        mov  eax, dword ptr [gBallSocketTrampoline]
        test eax, eax
        jz   done                       // hook installed but address not stored yet

        mov  eax, [ebp+8]
        push eax
        call BallSocketScale
        add  esp, 4
        fstp dword ptr [ebp-8]

        push dword ptr [ebp+0x14]
        push dword ptr [ebp+0x10]
        push dword ptr [ebp+0x0C]
        push dword ptr [ebp+0x08]
        mov  ecx, [ebp-4]
        mov  eax, dword ptr [gBallSocketTrampoline]
        call eax
        add  esp, 16

        push dword ptr [ebp-8]
        push dword ptr [ebp+8]
        call BallSocketRestore
        add  esp, 8

    done:
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// DeadSpaceNPCDeadST::HandleStateMessage. The DS2 patch calls this
// "ProcessEntityDeath"; the prototype symbols name it after the death state
// machine. Message 0x1F is the one-shot transition into the dead state, which
// is when a corpse starts moving under physics. 0x20 and 0x21 arrive every
// frame afterwards, so triggering on those keeps the damper permanently active.
static char __fastcall DeadState_Hook(void* self, int, unsigned a2, float a3,
                                      unsigned a4, unsigned messageId, void* a6)
{
    if (messageId == (unsigned)cfg::ragdollMessageId)
        g.deathFrames = 5;

    return gDeadState.thiscall<char>(self, a2, a3, a4, messageId, a6);
}

void ApplyHavokPhysics()
{
    if (!cfg::havokPhysics)
    {
        Log("HavokPhysics: disabled");
        return;
    }

    Log("HavokPhysics: scanning");

    const DWORD contactJacobian = Find("55 8B EC 83 E4 F0 81 EC A4 00 00 00 8B 55 08", "BuildContactConstraintJacobian");
    const DWORD ballSocket      = Find("55 8B EC 83 E4 F0 81 EC 84 00 00 00 F3 0F 10 41 04", "SolveBallSocketChainConstraints");
    const DWORD angular1D       = Find("55 8B EC 83 E4 F0 83 EC 14 8B 45 08 53 8B 5D 10", "Build1DAngularConstraintJacobian");
    const DWORD solverInit      = Find("8B 44 24 04 D9 80 0C 01 00 00 D9 19", "InitializePhysicsSolverParameters");
    const DWORD impulseDamper   = Find("0F C6 D1 AA 0F C6 DB FF F3 0F 58 D4 0F 28 C8 0F C6 C8 FF F3 0F 5C CA F3 0F 59 CB 0F 28 E1", "physicsImpulseDamper");
    const DWORD errorScaler     = Find("0F 28 16 0F 59 CA 0F 58 C3 0F 58 C1 8D 4A 10 0F 28 C8 0F C6 C8 55 F3 0F 58 C8 83 C2 20", "constraintErrorScaler");
    const DWORD massCapture     = Find("D9 44 24 10 DE FA D9 C9 D9 58 0C D9 44 24 68", "constraintMassCapture");
    const DWORD deadState       = Find("55 8B EC 51 56 8B F1 8B 4D 14 8B C1 83 E8 1F C6 45 FF 00", "DeadStateHandleMessage");
    const DWORD frameTick       = Find("55 8B EC 83 EC 0C 56 57 8B F9 E8 ?? ?? ?? ?? 84 C0 74 1F", "frameTick");

    if (!contactJacobian || !ballSocket || !angular1D || !solverInit ||
        !impulseDamper || !errorScaler || !massCapture)
    {
        Log("HavokPhysics: aborted, a core signature is missing");
        return;
    }

    if (cfg::contactJacobian)
        gContactJacobian = Hook((void*)contactJacobian, &ContactJacobian_Hook);

    if (cfg::ballSocket)
    {
        gBallSocket = Hook((void*)ballSocket, &BallSocket_Hook);
        gBallSocketTrampoline = (void*)gBallSocket.original<void*>();
    }

    if (cfg::angular1D)
        gAngular1D = Hook((void*)angular1D, &Angular1D_Hook);

    if (cfg::solverInit)
        gSolverInit = Hook((void*)solverInit, &SolverInit_Hook);

    if (cfg::impulseDamper)
    {
        gImpulseDamper = safetyhook::create_mid((void*)impulseDamper, [](safetyhook::Context& ctx)
        {
            // Right after a death the threshold drops, because that is when a
            // corpse receives its largest impulse.
            const float limit = g.deathFrames > 0 ? 0.8f : 20.0f;
            if (ctx.xmm3.f32[3] > limit)
                ctx.xmm3.f32[3] /= g.frameTimeScale;
        });
    }

    if (cfg::errorScaler)
    {
        gErrorScaler = safetyhook::create_mid((void*)errorScaler, [](safetyhook::Context& ctx)
        {
            ctx.xmm2.f32[3] /= g.frameTimeScale;
        });
    }

    if (cfg::massCapture)
    {
        gMassCapture = safetyhook::create_mid((void*)massCapture, [](safetyhook::Context& ctx)
        {
            g.constraintMass = *(float*)(ctx.esp + 0x10);
        });

        gForceDamper = safetyhook::create_mid((void*)(massCapture + 0xB), [](safetyhook::Context& ctx)
        {
            const float error = *(float*)(ctx.esp + 0x158);
            const float rhs = *(float*)(ctx.esp + 0x15C);

            if (std::fabs(error) > 0.8f && g.constraintMass != 0.0f)
                *(float*)(ctx.eax + 0x0C) = ((rhs / g.frameTimeScale) / g.constraintMass) * 0.8f;
        });

        gTimestepLimiter = safetyhook::create_mid((void*)(massCapture + 0x15), [](safetyhook::Context& ctx)
        {
            const float error = *(float*)(ctx.esp + 0x158);
            const bool heavy = std::fabs(error) > 0.5f && g.constraintMass >= 100.0f;

            if (std::fabs(error) > 2.0f || heavy)
                *(float*)(ctx.esp + 0x10) = -30.0f;
        });
    }

    if (deadState && cfg::deadState)
        gDeadState = Hook((void*)deadState, &DeadState_Hook);

    if (frameTick)
    {
        gFrameTick = safetyhook::create_mid((void*)frameTick, [](safetyhook::Context&)
        {
            if (g.deathFrames != 0)
                --g.deathFrames;
        });
    }

    Log("HavokPhysics: active (ragdoll message %02X)", cfg::ragdollMessageId);
}
