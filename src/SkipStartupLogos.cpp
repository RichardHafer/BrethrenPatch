// Shortens or removes the publisher logos at launch.
//
// The launch sequence isn't a movie. It is a frontend state machine driven by
// RWS messages. The company-logo message is sent from one place, and the tail
// of that function is all we need:
//
//     0051F8FE  call 0x00A7DE60          ; send iMsgUIStartCompanyLogos
//     0051F90C  fld  dword [0x00D44208]  ; 9.0 seconds
//     0051F915  fstp dword [edi+0x10]    ; arm the state timer
//     0051F918  mov  dword [edi+0x28],1  ; next state
//
// 0x00D44208 holds 9.0f and only that one instruction reads it, so it is the
// logo duration and nothing else. Rather than write to read-only data we point
// the fld at a float of our own: same six bytes, and the duration becomes a
// setting instead of a hardcoded zero.
//
// iMsgUIStartGameLogo comes from a function that arms no timer, so the game
// logo is driven from frontend data and isn't affected here. Chapter
// cinematics, the "previously" recap and the attract loop are all separate.

#include "Common.hpp"

// The patched instruction points here, so it has to outlive the call.
static float gLogoSeconds = 0.0f;

void ApplySkipStartupLogos()
{
    if (!cfg::skipStartupLogos)
        return;

    // Includes the constant's address, which is what separates this from any
    // other float load.
    const DWORD site = Find(
        "D9 05 08 42 D4 00 83 C4 14 D9 5F 10 C7 47 28 01 00 00 00",
        "startup logo timer");
    if (!site)
        return;

    gLogoSeconds = static_cast<float>(cfg::startupLogoPercent) / 100.0f * 9.0f;

    // Only the address bytes change, the opcode stays put.
    const DWORD target = reinterpret_cast<DWORD>(&gLogoSeconds);
    bool ok = true;
    for (unsigned i = 0; i < 4; ++i)
        ok = mem::Write<uint8_t>(site + 2 + i,
                                 static_cast<uint8_t>((target >> (i * 8)) & 0xFF)) && ok;

    if (!ok)
    {
        Log("Startup logos: could not repoint the timer load at %08X", site);
        return;
    }

    Log("Startup logos: timer load %08X repointed to %08X, %.2f s instead of "
        "9.00 s (%u%%)",
        site, target, gLogoSeconds, cfg::startupLogoPercent);

    if (cfg::startupLogoPercent != 0)
        return;

    // At zero duration the logos still flash for a frame, because the start
    // message is sent and we merely stop waiting for it. Kill the send too.
    //
    // NOPing the call is safe here: all three calls in this function are cdecl
    // and the caller clears all twenty bytes of arguments afterwards with a
    // single `add esp,0x14`, whether the call happened or not. Don't copy this
    // to the movie dispatcher, that one cleans its own arguments (ret 0xC),
    // and the same trick wrecks the stack there.
    //
    // Fourteen bytes ahead of the fld, so no second signature needed.
    const DWORD send = site - 14;
    if (mem::Read<uint8_t>(send) != 0xE8)
    {
        Log("Startup logos: expected a call at %08X, found %02X - logo start "
            "left alone", send, mem::Read<uint8_t>(send));
        return;
    }
    bool silenced = true;
    for (unsigned i = 0; i < 5; ++i)
        silenced = mem::Write<uint8_t>(send + i, 0x90) && silenced;
    Log("Startup logos: start message at %08X %s", send,
        silenced ? "suppressed" : "COULD NOT be suppressed");
}
