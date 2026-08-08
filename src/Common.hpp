#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "../include/safetyhook/safetyhook.hpp"

// Physics is authored for 30 FPS; the Havok fix scales against this.
constexpr float kTargetFrameTime = 1.0f / 30.0f;

struct State
{
    bool    initialised = false;
    HMODULE module = nullptr;

    float   frameTimeScale = 0.0f;
    float   constraintMass = 0.0f;
    int     deathFrames = 0;
};
inline State g;

// Settings, loaded from BrethrenPatch.ini.
namespace cfg
{
    inline bool havokPhysics = true;
    inline bool highCoreCpu = false;
    inline bool vsyncRefreshRate = true;
    inline bool uiScaling = false;
    inline bool log = true;

    inline int   cpuCoreCap = 8;
    inline int   vsyncMultiplier = 1;
    inline int   ragdollMessageId = 0x1F;
    inline float uiScale = 1.0f;

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

inline void LogInit()
{
    GetModuleFileNameA(nullptr, gLogPath, MAX_PATH);
    if (char* slash = strrchr(gLogPath, '\\'))
        *(slash + 1) = 0;
    strcat_s(gLogPath, MAX_PATH, "BrethrenPatch.log");

    InitializeCriticalSection(&gLogLock);
    gLogReady = true;

    FILE* f = nullptr;                       // truncate on every launch
    if (fopen_s(&f, gLogPath, "w") == 0 && f)
        fclose(f);
}

inline void Log(const char* fmt, ...)
{
    if (!gLogReady || !cfg::log)
        return;

    EnterCriticalSection(&gLogLock);
    FILE* f = nullptr;
    if (fopen_s(&f, gLogPath, "a") == 0 && f)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);

        fputc('\n', f);
        fclose(f);
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

// Scan and report the result, so a broken signature shows up in the log
// instead of silently disabling a fix.
inline DWORD Find(std::string_view pattern, const char* name)
{
    const DWORD address = mem::Scan(g.module, pattern);
    if (address)
        Log("  found  %-34s %08X", name, address);
    else
        Log("  MISSING %-33s (signature did not match)", name);
    return address;
}

inline safetyhook::InlineHook Hook(void* target, void* detour)
{
    return safetyhook::create_inline(target, detour);
}
