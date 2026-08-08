# Brethren Patch

Fixes for the PC port of Dead Space 3, in the spirit of the
[Marker Patch](https://github.com/Wemino/MarkerPatch) for Dead Space 2 and
[DeadSpace2008Fixes](https://github.com/seamusduncmcgrath/DeadSpace2008Fixes)
for the first game. Neither of those covers Dead Space 3, and most of their
patches do not transfer, so the addresses here were found from scratch. Named
after the Brethren Moons, since the Marker Patch is named after the Marker.

Tested against the Steam build (`deadspace3.exe`, 18,193,776 bytes,
ImageBase 0x400000, ASLR off).

## Fixes

### VSync no longer locks the game to 30 FPS

With VSync enabled the game runs at exactly 30 FPS whatever the display does.
It is not a D3D9 presentation interval - the swap chain always reports
`IMMEDIATE`, and no device reset happens when VSync is toggled. The entire main
loop runs at 30 Hz.

The cause is the engine's frame rate lock multiplier: the game passes 2 when
VSync is on, and against a 60 Hz vblank that is the 30 FPS you get. The patch
hooks `SetFrameRateLockMultiplier` and clamps the multiplier to 1. Set
`VSyncMultiplier=2` to restore the original behaviour, or `0` for uncapped.

### Physics no longer breaks above 30 FPS

Havok's constraint solving is tuned for a 30 FPS timestep. Above it, contact and
ragdoll constraints get over-solved and corpses or severed limbs launch across
the room. Solver inputs are scaled back to the 30 FPS equivalent for the
duration of each call.

Dead Space 3 ships the same Havok build as Dead Space 2, so seven of the eight
byte patterns from the Marker Patch matched unchanged. The eighth is the death
state machine's `HandleStateMessage`, which had to be located separately.

Two things worth knowing if you port this further:

- `SolveBallSocketChainConstraints` reads `[ecx+4]` in its first instruction, so
  `ecx` carries an input. A plain `__cdecl` detour clobbers it and the game
  crashes the moment ragdoll chains exist. There is a naked stub for that one.
- The scale must never drop below 1. It is `targetFrameTime / deltaTime`, which
  inverts below 30 FPS and then amplifies impulses instead of damping them -
  limbs fly off during a stutter. The Marker Patch has the same latent issue.

### Subtitles scale with the resolution

Subtitles keep a fixed pixel size, so they shrink as resolution goes up. The
game's drawing object can normalise either against a virtual 1280x720 canvas or
against the real resolution; text takes the second path.

The reference width follows the actual aspect ratio, so ultrawide displays do
not stretch the text horizontally: height stays anchored at 720 and width is
derived, giving 1720x720 at 3440x1440 and exactly 1280x720 on any 16:9 mode.
`UiScalePercent` tunes the size.

### High core count guard (off by default)

Mirrors what the Dead Space (2008) fixes do, but disabled, because the array
overflow those patches work around does not appear to exist here - the CPUID
enumeration walks a 32-bit affinity mask into 256-byte arrays, and the worker
pool it feeds is allocated dynamically. Capping also costs throughput, since the
pool is sized from the processor count. Enable it only if a many-core machine
actually crashes.

## Installing

Take `BrethrenPatch.asi` from a release, or build it yourself as described below.

1. Put an ASI loader in the game folder, for example
   [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   renamed to `winmm.dll`.
2. Copy `BrethrenPatch.asi` into `plugins\`.
3. Copy `BrethrenPatch.ini` next to `deadspace3.exe`.

`BrethrenPatch.log` is written next to the executable and lists every signature
that matched, so a failed scan is visible rather than silent.

### Compatibility with the DS3 Debug Menu

They cannot run together. The Debug Menu verifies the FNV1a64 hash of the game's
`.text` section to make sure its own hardcoded addresses are trustworthy, and the
hooks here change that section, so it locks its bindings and drops into a safe
overlay. Set `HavokPhysicsFix=0`, `VSyncRefreshRateFix=0` and `UiScaling=0` to
leave the code section untouched when you need the Debug Menu.

## Building

Visual Studio 2022 with the x86 toolchain, then `build.bat`. Adjust the
`vcvars32.bat` path in the script if your install differs. safetyhook needs
`std::expected`, hence `/std:c++latest`.

`include/safetyhook` is vendored from
[safetyhook](https://github.com/cursey/safetyhook) (Boost Software License 1.0),
which bundles Zydis (MIT). See `THIRD-PARTY-NOTICES.md`.

## Notes

Addresses are absolute VAs for the build listed above. If a future patch moves
them, the byte patterns in `src/` should still find them; the log will say which
one did not match.

## Licence

MIT, see `LICENSE`. Not affiliated with EA or Visceral Games.
