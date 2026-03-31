# TD5RE -- Test Drive 5 Reimplementation Engine

Source-port reimplementation of **Test Drive 5** (Pitbull Syndicate / Accolade, 1999), derived
from full Ghidra decompilation of `TD5_d3d.exe` (822 functions, 864 named).

## Architecture

```
td5re/
  td5re.h            Master header: forward decls, global state, module table
  td5_types.h        Shared types: Vec3, Mat3x3, fixed-point, Actor, enums
  td5_platform.h     Platform abstraction: window, input, audio, file I/O, timing

  td5_game.c/h       Main loop, state machine, game flow (4-state FSM)
  td5_physics.c/h    Vehicle dynamics: player 4-wheel, AI 2-axle, traffic simplified
  td5_track.c/h      Track geometry: STRIP.DAT parsing, span contacts, segment walking
  td5_ai.c/h         AI routing: LEFT/RIGHT.TRK, rubber-banding, script interpreter
  td5_render.c/h     Scene setup: software transform, frustum cull, mesh dispatch
  td5_frontend.c/h   Menu screens: 30-entry screen table, navigation FSM
  td5_hud.c/h        Race HUD: speedometer, minimap, timers, text overlay
  td5_sound.c/h      Sound: DXSound wrapper, vehicle audio, ambient, CD
  td5_input.c/h      Input: polling, controller config, force feedback
  td5_asset.c/h      Asset loading: ZIP archive, TGA decode, mesh prepare
  td5_save.c/h       Save system: Config.td5, CupData.td5, XOR encryption
  td5_net.c/h        Network: DirectPlay lockstep, DXPTYPE protocol
  td5_camera.c/h     Camera: 7 chase presets, trackside, spline, orbit
  td5_vfx.c/h        VFX: particles, tire tracks, smoke, weather, billboards
  td5_fmv.c/h        FMV: stub for modern video playback (replaces EA TGQ)

  CMakeLists.txt     Build system
```

## Module Dependency Graph

```
                       td5re.h  (master)
                      /   |    \
              td5_types.h  td5_platform.h
                   |           |
    +---------+----+----+------+----+---------+
    |         |         |           |         |
td5_game  td5_render  td5_asset  td5_input  td5_sound
    |         |         |
    +----+----+----+----+
         |         |
    td5_physics  td5_track
         |         |
    td5_ai    td5_camera
         |
    td5_vfx

  td5_frontend --> td5_save, td5_sound, td5_input, td5_net
  td5_hud      --> td5_render (projected primitive pipeline)
  td5_net      --> td5_input (control bitmask sync)
  td5_fmv      --> td5_platform (video surface)
```

### Dependency Rules

1. **td5_types.h** and **td5_platform.h** are leaf headers -- they depend on nothing
   except the C standard library.
2. **Game logic modules** (physics, ai, track, camera, vfx, save) include only
   td5_types.h. They have ZERO platform dependencies.
3. **Platform bridge modules** (render, sound, input, net, fmv) include both
   td5_types.h and td5_platform.h.
4. **td5_game.c** is the orchestrator -- it includes all module headers and drives
   the state machine.
5. No module includes another module's .c file. All inter-module communication
   goes through the public header API.

## Build Instructions

### Prerequisites

- CMake >= 3.15
- MinGW-w64 (i686-w64-mingw32) or MSVC with Win32 target
- MinHook (fetched automatically by CMake)

### Building

```bash
cd td5mod
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ../src/td5re
cmake --build .
```

Or with MSVC:

```bash
cmake -G "Visual Studio 17 2022" -A Win32 ../src/td5re
cmake --build . --config Release
```

### Output

- `td5re.exe` -- Standalone source-port executable (future)
- `td5_mod.asi` -- ASI mod DLL with hook bridge (current)

## Porting Strategy

### Phase 1: Foundation (current)

Create module stubs with correct function signatures. Each stub contains:
- The module init/shutdown/tick entry points
- Comments listing every original function (address + name)
- TODO markers for implementation status
- Key data structures and globals

### Phase 2: Pure Logic Modules

Reimplement platform-independent modules first:
1. **td5_track** -- STRIP.DAT parser, segment contact resolution
2. **td5_physics** -- Direct translation of fixed-point dynamics (3 tiers)
3. **td5_ai** -- Route table, steering pipeline, rubber-band, script VM
4. **td5_save** -- XOR encrypt/decrypt, CRC-32, field serialization
5. **td5_camera** -- Chase cam math (pure trigonometry)

These can be unit-tested against the original binary's output via x32dbg traces.

### Phase 3: Platform Bridge

Wire platform-dependent modules to the existing D3D11 wrapper:
1. **td5_render** -- Software transform pipeline feeding D3D11 DrawPrimitive
2. **td5_sound** -- Abstract over DirectSound -> SDL_mixer / miniaudio
3. **td5_input** -- Abstract over DirectInput -> SDL / XInput
4. **td5_asset** -- ZIP reader (minizip/zlib), TGA decoder (stb_image)

### Phase 4: Frontend and Integration

1. **td5_frontend** -- 30-screen state machine with TGA-based UI
2. **td5_hud** -- Sprite-based overlay system
3. **td5_net** -- Replace DirectPlay with ENet or similar
4. **td5_fmv** -- Replace EA TGQ with FFmpeg/libvpx stub

### Fixed-Point Fidelity

Physics and AI modules MUST preserve exact fixed-point behavioral fidelity:
- All arithmetic uses integer types (int32_t) with explicit shift/mask operations
- No implicit float conversions in the simulation loop
- Grip clamps, torque curves, and rubber-band parameters use original constants
- The tire slip circle uses integer sqrt (isqrt), not sqrtf
- Gravity constants: Easy=0x5DC, Normal=0x76C, Hard=0x800

### Existing Code Integration

The td5mod ASI framework (td5_mod.c, td5_sdk.h) continues to work alongside td5re:
- td5_sdk.h provides 864 named function pointers for hooking the original binary
- td5re modules provide clean reimplementations that can replace hooks incrementally
- The D3D11 wrapper (ddraw.dll) serves as the rendering backend for both paths
- Transition: each function is replaced one at a time, validated against original

## Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| Actor stride | 0x388 (904 bytes) | Per-vehicle data structure |
| Max racers | 6 | Slots 0-5 |
| Max traffic | 6 | Slots 6-11 |
| Fixed-point shift | 8 | 24.8 format |
| Angle full circle | 0x1000 (4096) | 12-bit angles |
| Tick accumulator | 0x10000 | One simulation tick |
| Max translucent batches | 510 | Linked-list pool |
| Depth sort buckets | 4096 | Back-to-front rendering |
| Texture cache slots | ~600 | LRU streaming cache |
| Track span stride | 24 bytes | STRIP.DAT per-span record |
| Mesh vertex stride | 44 bytes (0x2C) | PRR vertex format |
| Game heap | 24 MB initial | Growable via HeapCreate |
