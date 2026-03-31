# Test Drive 5 Modding Guide

> **Binary**: TD5_d3d.exe (32-bit x86 PE, MSVC, base 0x00400000)
> **Engine DLL**: M2DX.dll (DirectX 6 engine, base 0x10000000)
> **864 named functions** in EXE, **438 in M2DX.dll** -- fully reverse-engineered
> **Source references**: `td5mod/src/td5_sdk.h`, `td5mod/src/td5_mod.c`, `re/` analysis docs

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Asset Modding](#2-asset-modding)
3. [Code Modding](#3-code-modding)
4. [Code Caves](#4-code-caves)
5. [Game Data Reference](#5-game-data-reference)
6. [Known Modding Recipes](#6-known-modding-recipes)
7. [Appendix](#7-appendix)

---

## 1. Quick Start

### Prerequisites

- **MinGW i686** (32-bit GCC) -- the mod framework builds as a 32-bit DLL
- **MinHook 1.3.4** -- function hooking library (included in `td5mod/deps/`)
- **Ultimate ASI Loader** -- injects the mod DLL at startup (place `winmm.dll` or `dinput.dll` proxy in game root)
- **DDrawCompat v0.6.0** -- recommended for widescreen/high-refresh fixes (replaces dgVoodoo2 which has sync bugs)
- **Unpatched originals** of `TD5_d3d.exe` and `M2DX.dll` when using the widescreen fix

### Building

From the `td5mod/` directory, run:

```
build.bat
```

This compiles MinHook, then `td5_mod.c`, and links them into `build/scripts/td5_mod.asi`. The build system:

1. Compiles MinHook sources (hook.c, buffer.c, trampoline.c, hde32.c)
2. Compiles td5_mod.c with `-O2 -Wall -DWIN32`
3. Links as a shared DLL renamed to `.asi`, linking against `-lkernel32 -luser32 -lgdi32`

### Installing

1. Place `winmm.dll` (Ultimate ASI Loader) in the game root directory (next to `TD5_d3d.exe`)
2. Copy `td5_mod.asi` to `scripts/` subfolder in the game directory
3. Copy `td5_mod.ini` to `scripts/` subfolder
4. (Optional) Delete `Config.td5` before first run if changing resolution settings

### INI Configuration

The mod reads `scripts/td5_mod.ini` at DLL load time via `GetPrivateProfileIntA`/`GetPrivateProfileStringA`.

```ini
[Features]
DamageWobble=1          ; Restore cut random collision wobble + wheel shimmy
SnowRendering=1         ; Restore cut snow particle rendering
WidescreenFix=1         ; Widescreen + high-refresh fix (needs unpatched EXE)
SkipIntro=1             ; Skip intro movie
SkipLegals=1            ; Skip legal splash screens
DebugOverlay=0          ; FPS/speed/gear overlay during races
FontFix=1               ; Replace bitmap font with GDI TrueType text

[Widescreen]
Width=0                 ; 0 = auto-detect native resolution
Height=0                ; 0 = auto-detect native resolution
Pillarbox=1             ; 1 = 4:3 with black bars, 0 = stretch
VSync=1                 ; WaitForVerticalBlank in present
MenuFrameCap=0          ; ms between menu logic ticks (16 = ~60Hz, 0 = uncapped)

[FontFix]
FontName=Scream         ; TrueType font family name
FontFile=scream__.ttf   ; TTF path relative to EXE (empty = system font)
FontWeight=400          ; 400=Normal, 700=Bold
SmallFontSize=20        ; Body text height at 640x480 virtual res
LargeFontSize=36        ; Title text height at 640x480 virtual res
```

---

## 2. Asset Modding

### 2.1 Loose File Overrides

The game's ZIP reader (`ReadArchiveEntry` at 0x440790) implements **transparent fallback**: it tries `fopen(filename)` on disk first, falling back to the ZIP only if the file does not exist. This means any file inside a ZIP can be overridden by placing a loose file with the same name in the working directory (game root).

```
ReadArchiveEntry(entryName, zipPath, destBuf, maxSize):
    1. Try fopen(entryName, "rb")        // loose file on disk?
    2. If found: fread into destBuf       // use loose file
    3. If not found: open ZIP, search central directory, decompress
```

**Key points:**
- Filename matching is **case-insensitive** (uses `stricmp`)
- Directory prefixes are stripped from ZIP entry names before comparison
- Works for ALL game data: textures, models, physics, sounds, track data
- The same applies to `GetArchiveEntrySize` (0x4409B0)

### 2.2 Car Physics Editing (carparam.dat)

Each car's physics are defined in `carparam.dat` (268 bytes, 0x10C), loaded from `cars/*.zip` by `LoadRaceVehicleAssets` (0x443280). The file splits into two tables:

| Region | File Offset | Size | Runtime Location |
|--------|-------------|------|-----------------|
| Tuning table | 0x00-0x8B | 140 bytes | `gVehicleTuningTable` (0x4AE580), stride 0x8C/slot |
| Physics table | 0x8C-0x10B | 128 bytes | `gVehiclePhysicsTable` (0x4AE280), stride 0x80/slot |

#### Physics Table Fields (offsets relative to file offset 0x8C)

| File Offset | Phys Offset | Size | Type | Name | Notes |
|-------------|-------------|------|------|------|-------|
| 0x8C | +0x00 | 32B | short[16] | `torque_curve` | Engine torque at 16 RPM points (0-8192, 512/step) |
| 0xAC | +0x20 | 4B | int | `vehicle_inertia` | Yaw moment of inertia |
| 0xB0 | +0x24 | 4B | int | `half_wheelbase` | CG-to-axle distance |
| 0xB4 | +0x28 | 2B | short | `front_weight_dist` | Front axle weight numerator |
| 0xB6 | +0x2A | 2B | short | `rear_weight_dist` | Rear axle weight numerator |
| 0xB8 | +0x2C | 2B | short | `drag_coefficient` | Aero drag (difficulty-scaled) |
| 0xBA | +0x2E | 16B | short[8] | `gear_ratio_table` | R/N/1st-6th gear ratios |
| 0xCA | +0x3E | 16B | short[8] | `upshift_rpm_table` | Upshift threshold per gear |
| 0xDA | +0x4E | 16B | short[8] | `downshift_rpm_table` | Downshift threshold per gear |
| 0xEA | +0x5E | 2B | short | `suspension_damping` | Damping coefficient |
| 0xEC | +0x60 | 2B | short | `suspension_spring_rate` | Spring restoring force |
| 0xEE | +0x62 | 2B | short | `suspension_feedback` | Cross-axis coupling |
| 0xF0 | +0x64 | 2B | short | `suspension_travel_limit` | Max displacement (+/-) |
| 0xF2 | +0x66 | 2B | short | `suspension_response` | Velocity feedback |
| 0xF4 | +0x68 | 2B | short | `drive_torque_multiplier` | Base torque scalar (difficulty-scaled) |
| 0xF6 | +0x6A | 2B | short | `damping_low_speed` | Velocity decay at low speed |
| 0xF8 | +0x6C | 2B | short | `damping_high_speed` | Velocity decay at high speed |
| 0xFA | +0x6E | 2B | short | `brake_force` | Brake deceleration (difficulty-scaled) |
| 0xFC | +0x70 | 2B | short | `engine_brake_force` | Engine braking (difficulty-scaled) |
| 0xFE | +0x72 | 2B | short | `max_rpm` | Redline / torque cutoff |
| 0x100 | +0x74 | 2B | short | `top_speed_limit` | Hard speed cap (val * 256 > speed) |
| 0x102 | +0x76 | 2B | short | `drivetrain_type` | 1=RWD, 2=FWD, 3=AWD |
| 0x104 | +0x78 | 2B | short | `speed_scale_factor` | Speed-to-RPM conversion |
| 0x106 | +0x7A | 2B | short | `handbrake_grip_mod` | Rear grip reduction (<256 = less grip) |
| 0x108 | +0x7C | 2B | short | `lateral_slip_stiffness` | Cornering slip sensitivity |

#### Tuning Table Key Fields (offsets relative to file offset 0x00)

| Offset | Size | Type | Name |
|--------|------|------|------|
| 0x40 | 32B | short[4] x4 | `wheel_positions` (FL/FR/RL/RR, 8B each: x,y,z,flags) |
| 0x82 | 2B | short | `suspension_height_ref` (vertical reference for wheel contact) |
| 0x88 | 2B | short | `collision_mass` (higher = heavier = less collision knockback) |

#### Difficulty Scaling (applied at runtime by InitializeRaceVehicleRuntime)

| Difficulty | drive_torque_mult | drag_coeff | speed_scale | brake | engine_brake |
|-----------|-------------------|------------|------------|-------|-------------|
| Easy | unchanged | unchanged | unchanged | unchanged | unchanged |
| Normal | x1.41 (360/256) | x1.17 (300/256) | x2 (<<1) | unchanged | unchanged |
| Hard | x2.54 (650/256) | x1.48 (380/256) | x4 (<<2) | x1.76 (450/256) | x1.56 (400/256) |

#### Key Formulas

**Drive Torque:**
```c
curve_idx = engine_speed / 512;
base = torque_curve[curve_idx] * drive_torque_mult / 256;
next = torque_curve[curve_idx+1] * drive_torque_mult / 256;
frac = engine_speed & 0x1FF;
interpolated = base + (next - base) * frac / 512;
result = interpolated * throttle / 256 * gear_ratio[gear] / 256;
if (engine_speed > max_rpm - 50) result = 0;  // redline cutoff
```

**Velocity Damping (per-frame):**
```c
damp = surface_damping[surface_type] * 256 + (speed_low ? damping_low : damping_high);
velocity -= (velocity / 256) * damp / 4096;
```

### 2.3 LEVELINF.DAT Editing (Track Configuration)

Each track's environment config is stored in `levelinf.dat` (100 bytes) inside `level%03d.zip`. Loaded by `LoadTrackRuntimeData` (0x42FB90), stored at `gTrackEnvironmentConfig` (0x4AEE20).

```c
struct LevelInfoDat {  // 100 bytes (0x64)
    /* +0x00 */ uint32_t track_type;          // 0 = circuit, 1 = point-to-point
    /* +0x04 */ uint32_t smoke_enable;         // 1 = tire smoke sprites
    /* +0x08 */ uint32_t checkpoint_count;     // 1-7 checkpoint stages
    /* +0x0C */ uint32_t checkpoint_spans[7];  // Span index per checkpoint
    /* +0x28 */ uint32_t weather_type;         // 0=rain, 1=snow(CUT), 2=clear
    /* +0x2C */ uint32_t density_pair_count;   // 0-6 weather density zones
    /* +0x30 */ uint32_t is_circuit;           // 1=circuit (lapped), 0=P2P
    /* +0x34 */ struct { int16_t segment_id; int16_t density; } density_pairs[6];
    /* +0x4C */ uint8_t  padding[8];
    /* +0x54 */ uint32_t sky_animation_index;  // vestigial (unused on PC)
    /* +0x58 */ uint32_t total_span_count;     // spans in track (9999 = P2P)
    /* +0x5C */ uint32_t fog_enabled;          // 0=off, 1=on
    /* +0x60 */ uint8_t  fog_color_r;
    /* +0x61 */ uint8_t  fog_color_g;
    /* +0x62 */ uint8_t  fog_color_b;
    /* +0x63 */ uint8_t  padding2;
};
```

**Track Weather Map (vanilla):**

| Level | Weather | Smoke | Fog | Fog Color |
|-------|---------|-------|-----|-----------|
| 001 | Rain | Yes | No | -- |
| 002 | Clear | No | No | -- |
| 003 | **Snow** | No | No | -- |
| 013 | Clear | No | Yes | (32,32,0) |
| 014 | Clear | No | Yes | (16,16,16) |
| 023 | Rain | Yes | Yes | (0,0,0) |
| 025 | Clear | No | Yes | (16,16,32) |
| 026 | Clear | No | Yes | (64,16,0) |
| 030 | Rain | Yes | No | -- |

To add rain to a clear track: set `weather_type` (+0x28) to 0, add `density_pairs` to control intensity zones.

### 2.4 Texture Replacement

Car skins are loaded as TGA files from per-car ZIPs:
- `CARSKIN%d.TGA` -- main body texture (256x256, composited in pairs for UV tiling)
- `CARHUB%d.TGA` -- wheel hub texture (64x64, composited into 256x256 grid)

Track textures are loaded from `static.zip`:
- `tpage%d.dat` -- track texture pages
- `FORWSKY.TGA` / `BACKSKY.TGA` -- sky dome textures from `level%03d.zip`

Traffic skins: `skin%d.tga` from `traffic.zip`

All can be overridden by placing loose files with matching names in the game directory (see section 2.1).

### 2.5 Sound Replacement

Vehicle sounds are loaded per-car from `cars/*.zip` via `LoadVehicleSoundBank`. Ambient sounds are loaded per-track. Override by placing loose files with matching names.

---

## 3. Code Modding

### 3.1 ASI Architecture

The mod framework is a 32-bit DLL renamed to `.asi`, injected by Ultimate ASI Loader at process startup. The entry point is `DllMain`:

1. On `DLL_PROCESS_ATTACH`: call `LoadConfig()` to read INI settings
2. Initialize MinHook (`MH_Initialize`)
3. Apply `PatchMemory` byte patches (for simple NOP/JMP fixes)
4. Create MinHook hooks (for function replacement with trampoline)
5. Apply `PatchCallSite` redirections (for CALL-site hijacks)
6. Enable all hooks (`MH_EnableHook(MH_ALL_HOOKS)`)

### 3.2 Three Patching Patterns

#### Pattern 1: PatchMemory (Direct Byte Write)

For simple NOP-outs, JMP redirections, or small data changes. Handles VirtualProtect internally.

```c
static int PatchMemory(void *addr, const void *data, size_t len) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 1;
}

// Example: NOP out the snow render skip (6-byte JNZ -> 6x NOP)
uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
PatchMemory((void*)0x4465F4, nops, 6);
```

#### Pattern 2: MinHook (Function Hook with Trampoline)

For replacing entire functions while preserving the ability to call the original. Best for complex behavioral changes.

```c
typedef void (__cdecl *fn_TargetFunc)(void);
static fn_TargetFunc Original_TargetFunc = NULL;

static void __cdecl Hook_TargetFunc(void) {
    // Custom pre-processing
    Original_TargetFunc();   // call original via trampoline
    // Custom post-processing
}

// In DllMain:
MH_CreateHook((void*)0x00XXXXXX, Hook_TargetFunc, (void**)&Original_TargetFunc);
MH_EnableHook(MH_ALL_HOOKS);
```

#### Pattern 3: PatchCallSite (CALL Instruction Redirect)

For hijacking a specific CALL site to redirect to your function. Writes `E8 <rel32>` and pads with NOPs.

```c
static int PatchCallSite(uintptr_t site, void *target, int total_bytes) {
    uint8_t buf[16];
    int32_t rel = (int32_t)((uintptr_t)target - (site + 5));
    buf[0] = 0xE8;  // CALL rel32
    memcpy(&buf[1], &rel, 4);
    for (int i = 5; i < total_bytes; i++)
        buf[i] = 0x90;  // NOP
    return PatchMemory((void*)site, buf, total_bytes);
}

// Example: redirect a 6-byte CALL site to your function
PatchCallSite(0x00414C9E, MyHookFunction, 6);
```

### 3.3 Key Hook Points

| Address | Function | Hook Use Case |
|---------|----------|--------------|
| 0x0042B580 | `RunRaceFrame` | Per-frame race logic (physics, rendering, HUD) |
| 0x00414B50 | `RunFrontendDisplayLoop` | Per-frame menu logic |
| 0x00442170 | `GameStateDispatcher` | Game state transitions |
| 0x00418450 | `NoOpHookStub` | **9 lifecycle call sites** -- free hook point (see section 4) |
| 0x004388A0 | `RenderRaceHudOverlays` | HUD rendering (speedometer, timers, minimap) |
| 0x00428320 | `QueueRaceHudFormattedText` | Printf-style text to D3D glyph quads |
| 0x0042C470 | `PollRaceSessionInput` | Input polling bridge (local -> controlBits) |
| 0x00404030 | `UpdatePlayerVehicleDynamics` | Per-player physics update |
| 0x00409150 | `ResolveVehicleContacts` | Vehicle collision resolution |
| 0x0042FB90 | `LoadTrackRuntimeData` | Track data loading (6 files from ZIP) |
| 0x00443280 | `LoadRaceVehicleAssets` | Vehicle model/physics/sound loading |
| 0x00414610 | `SetFrontendScreen` | Menu screen transitions |
| 0x00437BA0 | `InitializeRaceHudLayout` | HUD element positioning (dynamic scaling) |
| 0x0043E7E0 | `ConfigureProjectionForViewport` | Viewport/projection setup |
| 0x00425360 | `PresentFrontendBufferSoftware` | Frontend present (Blt to front surface) |
| 0x004269D0 | `ScreenLocalizationInit` | First startup screen (hardware init, config load) |

### 3.4 COM Vtable Calls

DirectDraw/Direct3D interfaces use COM calling convention (__stdcall, first arg = `this`):

```c
// COM ABI: (*vtable[slot])(this, args...)  -- NOT __thiscall
typedef HRESULT (__stdcall *pfn_DDS_Blt)(
    void *self, RECT *lpDestRect, void *lpDDSSrc,
    RECT *lpSrcRect, DWORD dwFlags, void *lpDDBltFx);

// IDirectDrawSurface vtable slot indices:
//   +0x14 = Blt (slot 5)
//   +0x1C = BltFast (slot 7)
//   +0x44 = GetDC (slot 17)
//   +0x64 = Lock (slot 25)
//   +0x68 = ReleaseDC (slot 26)
//   +0x80 = Unlock (slot 32)

// Example: Blt-stretch back buffer to front surface
void **exref = g_ddExrefPtr;      // [IDirectDraw*, front*, back*]
void *front = exref[1];
void *back  = exref[2];
void **front_vt = *(void ***)front;
pfn_DDS_Blt blt = (pfn_DDS_Blt)front_vt[5];  // slot 5 = Blt
blt(front, &dest_rect, back, &src_rect, DDBLT_WAIT, NULL);
```

### 3.5 INI Config Pattern

Use `GetPrivateProfileIntA` / `GetPrivateProfileStringA` for feature toggles:

```c
static struct {
    int my_feature;
    int my_value;
} g_config;

static void LoadConfig(void) {
    char ini[MAX_PATH];
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *s = strrchr(ini, '\\');
    if (s) strcpy(s + 1, "scripts\\td5_mod.ini");

    g_config.my_feature = GetPrivateProfileIntA("MySection", "MyFeature", 0, ini);
    g_config.my_value   = GetPrivateProfileIntA("MySection", "MyValue", 100, ini);
}
```

---

## 4. Code Caves

### 4.1 Widescreen Code Cave Region (0x45C1A0-0x45C580)

A ~1000-byte region of zeroed space in the .text section, confirmed free and usable for code caves. The widescreen mod uses portions of this region:

| Start VA | End VA | Size | Used By |
|----------|--------|------|---------|
| 0x45C1A0 | 0x45C330 | ~400B | Available |
| 0x45C330 | 0x45C3FF | ~208B | Widescreen scale/logic caves (when using binary patches) |
| 0x45C400 | 0x45C580 | ~384B | Available |

When using the ASI mod framework (which applies patches at runtime via C functions), the entire binary cave region remains unused -- the C code replaces the ASM caves.

### 4.2 NoOpHookStub (0x418450)

A single-byte `RET` (0xC3) followed by 7 NOP bytes, called from **9 distinct lifecycle functions**:

| Caller | VA | Context |
|--------|-----|---------|
| InitializeFrontendResourcesAndState | 0x414740 | Frontend startup |
| RunFrontendDisplayLoop | 0x414B50 | Per-frame frontend update |
| RunRaceFrame | 0x42B580 | Per-frame race update |
| RenderRaceActorForView | 0x40C120 | Per-actor render |
| LoadEnvironmentTexturePages | 0x42F990 | Texture loading |
| RunFrontendConnectionBrowser | 0x418D50 | Network lobby (5 sites) |
| RenderFrontendSessionBrowser | 0x419B30 | Network session list |
| RenderFrontendCreateSessionNameInput | 0x41A530 | Network name input |
| RenderFrontendLobbyChatInput | 0x41A670 | Network chat |

This is the ideal free hook point for custom instrumentation. Replace the `RET` with a `JMP` to your code, or use MinHook:

```c
MH_CreateHook((void*)0x00418450, MyInstrumentationHook, (void**)&Original_NoOp);
```

Your hook will be called at every major lifecycle point: frontend init, per-frame menu, per-frame race, per-actor render, and during network screens.

---

## 5. Game Data Reference

### 5.1 Game States

Stored at `g_gameState` (0x4C3CE8):

| Value | State | Description |
|-------|-------|-------------|
| 0 | Intro/Movie | Intro movie playback, legal screens |
| 1 | Menu | Frontend display loop (`RunFrontendDisplayLoop` at 0x414B50) |
| 2 | Race | Race simulation loop (`RunRaceFrame` at 0x42B580) |
| 3 | Benchmark Viewer | FPS report TGA display (after TimeDemo race) |

### 5.2 Game Types

Stored at `g_selectedGameType` (0x4AAF6C):

| ID | Name | Races | Difficulty |
|----|------|-------|-----------|
| 1 | Championship | 4 | Easy |
| 2 | Era | 6 | Easy |
| 3 | Challenge | 6 | Medium |
| 4 | Pitbull | 8 | Hard |
| 5 | Masters | 10 | Medium |
| 6 | Ultimate | 12 | Hard |
| 7 | Time Trial | 1 | Hard |
| 8 | Cop Chase | 1 | -- |
| 9 | Drag Race | 1 | -- |

### 5.3 Actor System

6 actor slots at `g_actorRuntimeState` (0x4AB108), each 0x388 bytes:

```c
#define TD5_ACTOR(slot) ((uint8_t*)0x4AB108 + (slot) * 0x388)
```

**Key offsets within each actor struct:**

| Offset | Size | Type | Name |
|--------|------|------|------|
| 0x082 | 2B | int16 | `span_index` -- current STRIP.DAT span |
| 0x084 | 4B | int32 | `span_counter` -- monotonic forward span count |
| 0x1B8 | 4B | ptr | `tuning_table_ptr` -- pointer to tuning table |
| 0x1BC | 4B | ptr | `physics_table_ptr` -- pointer to physics table |
| 0x1FC | 4B | int32 | `world_pos_x` -- 24.8 fixed-point X |
| 0x200 | 4B | int32 | `world_pos_y` -- 24.8 fixed-point Y |
| 0x204 | 4B | int32 | `world_pos_z` -- 24.8 fixed-point Z |
| 0x314 | 4B | int32 | `engine_rpm` |
| 0x318 | 4B | int32 | `forward_speed` |
| 0x31C | 4B | int32 | `lateral_speed` |
| 0x33C | 2B | int16 | `steering_cmd` -- range +/-0x18000 |
| 0x36A | 1B | int8 | `current_gear` |
| 0x380 | 1B | int8 | `race_finished` -- 1 when done |

### 5.4 Input Bitmask

Player control bits at `g_playerControlBits` (0x482FFC), DWORD[2] for P1/P2:

| Bit | Hex Mask | Action |
|-----|----------|--------|
| 0 | 0x00000001 | Steer Left |
| 1 | 0x00000002 | Steer Right |
| 9 | 0x00000200 | Throttle |
| 10 | 0x00000400 | Brake |
| 19 | 0x00080000 | Gear Down |
| 20 | 0x00100000 | Gear Up |
| 21 | 0x00200000 | Horn |
| 27 | 0x08000000 | Analog Y Flag |
| 31 | 0x80000000 | Analog X Flag |

Input sources: `g_p1InputSource` (0x497A58), `g_p2InputSource` (0x465FF4). 0=keyboard, 1+=joystick.

### 5.5 Frontend Screen Table

30 function pointers at `g_frontendScreenFnTable` (0x4655C4). Key entries:

| Index | Function | Purpose |
|-------|----------|---------|
| 0 | `ScreenLocalizationInit` (0x4269D0) | First startup screen |
| 1 | Positioner debug tool (vestigial) | Developer layout tool |
| 5 | `ScreenMainMenuAnd1PRaceFlow` | Main menu |
| 8 | `RunFrontendConnectionBrowser` (0x418D50) | Network session browser |

Current screen pointer at `g_currentScreenFnPtr` (0x495238). Set screen via `SetFrontendScreen` (0x414610).

### 5.6 Startup Tokens (M2DX.dll TD5.ini)

26 tokens parsed from `TD5.ini` by M2DX.dll:

| Index | Token | Effect |
|-------|-------|--------|
| 0-4 | `//`, `rem`, `;`, `\`, `#` | Comment (ignored) |
| 5 | `Log` | Enable M2DX debug logging |
| 6 | `TimeDemo` | Activate benchmark mode |
| 7 | `FullScreen` | Force fullscreen |
| 8 | `Window` | Force windowed |
| 9 | `DoubleBuffer` | Disable triple buffering |
| 10 | `NoTripleBuffer` | Disable triple buffering |
| 11 | `NoWBuffer` | Disable W-buffer |
| 12 | `Primary` | Force primary adapter |
| 13 | `DeviceSelect` | Adapter selection override |
| 14 | `NoMovie` | Disable movie playback |
| 15 | `FixTransparent` | Transparency fix |
| 16 | `FixMovie` | Movie playback fix |
| 17 | `NoAGP` | Disable AGP textures |
| 18 | `NoPrimaryTest` | Skip primary driver test |
| 19 | `NoMultiMon` | Disable multi-monitor enumeration |
| 20 | `NoMIP` | Disable mipmapping |
| 21 | `NoLODBias` | Disable LOD bias |
| 22 | `NoZBias` | Disable Z-bias |
| 23 | `MatchBitDepth` | Match desktop bit depth |
| 24 | `WeakForceFeedback` | FF gain = 0.5 |
| 25 | `StrongForceFeedback` | FF gain = 1.0 |

### 5.7 XOR Encryption Keys

Both save files use XOR encryption with `^ 0x80`:

```c
void xor_crypt(byte *buf, int size, char *key) {
    int key_len = strlen(key);
    for (int i = 0; i < size; i++)
        buf[i] = buf[i] ^ key[i % key_len] ^ 0x80;
}
```

| File | Key | Length | Key VA |
|------|-----|--------|--------|
| Config.td5 | `"Outta Mah Face!! "` | 18 | 0x463F9C |
| CupData.td5 | `"Steve Snake says : No Cheating! "` | 31 | 0x464084 |

Both files use CRC-32 (standard ISO 3309, table at 0x475160) for integrity validation.

**Config.td5**: 5351 bytes. CRC at offset 0x00. Contains all settings, controller bindings, high scores, car/track lock tables, cheat flags.

**CupData.td5**: 12966 bytes. CRC at offset 0x0C. Contains full mid-cup state snapshot (schedule, results, actor state, slot state).

### 5.8 Cheat Codes

Typed on the **Options Hub** screen (frontend). All cheats are toggles (XOR operation) -- entering the same code again deactivates it. Sound feedback: SFX 4 on activate, SFX 5 on deactivate.

| Code | Text | Target | Effect |
|------|------|--------|--------|
| 0 | `I HAVE THE KEY` | 0x496298 XOR 1 | Unlock flag A (combine with code 2 for all cars) |
| 1 | `CUP OF CHOICE` | 0x4962A8 XOR 8 | Unlock all cup tiers |
| 2 | `I CARRY A BADGE` | 0x4962AC XOR 2 | Unlock flag B (combine with code 0 for all cars) |
| 3 | `LONE CRUSADER IN A DANGEROUS WORLD` | 0x4AAF7C XOR 1 | Nitro boost (horn = 2x speed + vertical launch) |
| 4 | `REMOTE BRAKING` | 0x49629C XOR 1 | Horn freezes all opponents |
| 5 | `THAT TAKES ME BACK` | 0x4962B4 XOR 1 | Unlock retro/classic car variants |

### 5.9 Easter Egg Cars

Hidden vehicles in the car roster (referenced in LANGUAGE.DLL string tables):

- **FEAR FACTORY WAGON** -- band-themed special car
- **THE MIGHTY MAUL** -- oversized novelty vehicle
- **CHRIS'S BEAST** -- developer personal car
- **HOT DOG** -- food-themed novelty vehicle

### 5.10 Cup Unlock Table

Winning 1st place in the final cup race triggers `AwardCupCompletionUnlocks` (0x421DA0):

| NPC Group | Cup Context | Unlocked Car Index | Car |
|-----------|-------------|-------------------|-----|
| 4 | Era Race 1 | 21 (0x15) | Special 2 |
| 5 | Era Race 5 | 17 (0x11) | Chevelle SS |
| 6 | Era Race 3 | 24 (0x18) | A-Type Concept |
| 7 | Era Race 4 | 25 (0x19) | Nissan 240SX |
| 16 | Era Race 2 | 23 (0x17) | Special 4 |
| 17 | Era Race 6 | 26 (0x1A) | Ford Mustang |
| 18 | Bonus | 32 (0x20) | Cadillac Eldorado |

Track groups (0-3, 8-15) set track completion flags instead of unlocking cars.

**Cup tier progression** (`DAT_004962A8`):

| Value | Available Cups |
|-------|----------------|
| 0 | Championship, Era only |
| 1 | + Challenge, Pitbull |
| >= 2 | All cups |

Car unlock mechanics:
```c
DAT_00463E4C[car_index] = 0;            // clear lock flag
if (DAT_00463E0C < car_index + 1)
    DAT_00463E0C = car_index + 1;       // advance max car count
```

### 5.11 Key Globals Quick Reference

| Address | Type | Name | Description |
|---------|------|------|-------------|
| 0x4C3CE8 | int32 | `g_gameState` | 0=intro, 1=menu, 2=race, 3=benchmark |
| 0x4AAF08 | float | `g_renderWidthF` | Race render width |
| 0x4AAF0C | float | `g_renderHeightF` | Race render height |
| 0x4AAF00 | int32 | `g_racerCount` | Total actors (6 or 12 w/ traffic) |
| 0x4AAF44 | uint32 | `g_framePrevTimestamp` | timeGetTime() of last frame |
| 0x4AAD70 | float | `g_normalizedFrameDt` | 1.0 at 30fps |
| 0x4AAF60 | float | `g_subTickFraction` | 0.0-1.0 interpolation (47+ consumers) |
| 0x495220 | ptr | `g_backBufferSurface` | IDirectDrawSurface* back buffer |
| 0x45D53C | ptr | `g_appExref` | App/window state object |
| 0x45D564 | ptr** | `g_ddExrefPtr` | [IDirectDraw*, frontSurf*, backSurf*] |
| 0x4C3DE8 | int32 | `g_weatherType` | 0=rain, 1=snow(cut), 2=clear |
| 0x4AEE20 | ptr | `g_trackEnvironmentConfig` | LEVELINF.DAT loaded data |
| 0x4AAF7C | int32 | `g_cheatNitroEnabled` | Nitro cheat flag |
| 0x49629C | int32 | `g_cheatRemoteBraking` | Remote braking cheat flag |
| 0x4655C4 | ptr[30] | `g_frontendScreenFnTable` | 30 screen function pointers |
| 0x49522C | int32 | `g_frontendAnimTick` | Menu animation frame counter |

---

## 6. Known Modding Recipes

### 6.1 Custom HUD Element

Hook `RenderRaceHudOverlays` (0x4388A0) and call `QueueRaceHudFormattedText` to add text:

```c
static fn_RenderRaceHudOverlays Original_RenderRaceHudOverlays;

static void __cdecl Hook_RenderRaceHudOverlays(void) {
    Original_RenderRaceHudOverlays();

    // Add custom text overlay (x, y, format string, ...)
    uint8_t *player = TD5_ACTOR(0);
    int speed = *(int*)(player + ACTOR_OFF_FORWARD_SPEED);
    TD5_QueueRaceHudFormattedText(10, 10, "SPD: %d", speed / 256);
}
```

### 6.2 Runtime Physics Changes

Modify the physics table in memory after loading. Access via actor struct pointers:

```c
// Double the brake force for player 1
uint8_t *actor = TD5_ACTOR(0);
short *phys = (short*)*(int*)(actor + ACTOR_OFF_PHYSICS_PTR);
phys[0x6E / 2] *= 2;  // brake_force at physics offset 0x6E
```

Or modify during `InitializeRaceVehicleRuntime` by hooking it:

```c
// Change drivetrain to AWD (physics offset 0x76 = drivetrain_type)
short *phys = (short*)(0x4AE280 + slot * 0x80);
phys[0x76 / 2] = 3;  // 3 = AWD
```

### 6.3 Skip Intros

NOP the intro movie call and legal screen display:

```c
// Skip intro movie: NOP the call to PlayIntroMovie
if (g_config.skip_intro) {
    uint8_t nops5[] = {0x90, 0x90, 0x90, 0x90, 0x90};
    PatchMemory((void*)INTRO_CALL_SITE, nops5, 5);
}
```

Or use the `NoMovie` token in `TD5.ini` (M2DX.dll startup token 14).

### 6.4 Re-enable Snow Rendering

Snow particles are fully allocated but rendering is skipped by a JNZ at 0x4465F4:

```c
// NOP the 6-byte JNZ that skips render when weather != 0
uint8_t nops6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
PatchMemory((void*)0x4465F4, nops6, 6);
```

Snow will render as streak-style particles (same renderer as rain). The visual effect looks like heavy sleet rather than gentle snowfall, since the original devs cut it before implementing billboard-style snowflake rendering. Level003 is the only vanilla track with `weather_type=1` (snow).

### 6.5 Restore Damage Wobble

The `GetDamageRulesStub` (0x42C8D0) is a `XOR EAX,EAX; RET` stub that kills all randomness in collision wobble and wheel shimmy. Redirect it to `_rand` (0x448157):

```c
// JMP _rand (5 bytes): E9 82 B8 01 00
uint8_t jmp_rand[] = {0xE9, 0x82, 0xB8, 0x01, 0x00};
PatchMemory((void*)0x42C8D0, jmp_rand, 5);
```

This restores random angular perturbation on high-energy collisions and visible wheel vibration on damaged vehicles. 22 call sites across 4 functions are fixed by this single 5-byte patch.

### 6.6 Unlock All Cars (Programmatic)

Write directly to the car lock table:

```c
// Clear all car lock flags
memset((void*)0x463E4C, 0, 37);       // 37 car lock bytes -> 0 = unlocked
*(int*)0x463E0C = 37;                  // max unlocked car index
*(int*)0x4962B0 = 1;                   // all-cars-unlocked flag
```

Or use the cheat code combination: type `I HAVE THE KEY` then `I CARRY A BADGE` on the Options Hub screen.

### 6.7 Weather Effects on Any Track

Force weather at runtime by writing to the weather global after track load:

```c
// Hook LoadTrackRuntimeData and force rain on any track
static fn_LoadTrackRuntimeData Original_LoadTrackRuntimeData;

static void __cdecl Hook_LoadTrackRuntimeData(void) {
    Original_LoadTrackRuntimeData();
    *(int*)0x4C3DE8 = 0;  // Force rain (0=rain, 1=snow, 2=clear)
}
```

Or edit `levelinf.dat` inside the track ZIP (see section 2.3): set offset +0x28 to 0 (rain) or 1 (snow, requires snow render patch), and add density_pairs for intensity control.

### 6.8 Texture Replacement via Loose Files

Place replacement TGA files in the game root directory with the exact same filename as the ZIP entry:

```
Game Root/
  TD5_d3d.exe
  CARSKIN0.TGA        <-- overrides CARSKIN0.TGA from any car ZIP
  FORWSKY.TGA         <-- overrides sky texture from level ZIP
  levelinf.dat        <-- overrides track config from level ZIP
```

The ZIP reader checks the filesystem first, so no ZIP modification is needed. Filenames are case-insensitive.

### 6.9 Activate Benchmark Mode

Use the `-TimeDemo` command-line flag (simplest, no binary patch):

```
TD5_d3d.exe -TimeDemo
```

This activates the built-in benchmark: auto-drives a race on track 23, records per-frame FPS, and generates a 640x480 TGA report with system info and FPS graph.

---

## 7. Appendix

### 7.1 Key Source Files

| File | Path | Purpose |
|------|------|---------|
| SDK Header | `td5mod/src/td5_sdk.h` | All game globals, function typedefs, struct offsets |
| Mod Source | `td5mod/src/td5_mod.c` | Complete ASI mod framework (widescreen, cut features, HUD) |
| Build Script | `td5mod/build.bat` | MinGW i686 build (compiles MinHook + mod, links as .asi) |
| INI Config | `scripts/td5_mod.ini` | Runtime feature toggles and widescreen settings |
| Actor Struct | `re/include/td5_actor_struct.h` | Complete 0x388-byte actor struct (100% mapped) |
| Globals Header | `re/include/td5_globals.h` | Full global variable catalog |
| Functions Header | `re/include/td5_functions.h` | Full function address catalog |
| M2DX Header | `re/include/m2dx.h` | M2DX.dll function catalog |
| Physics Table | `re/analysis/carparam-physics-table.md` | Complete carparam.dat field-by-field layout |
| Track Config | `re/analysis/levelinf-dat-format.md` | LEVELINF.DAT structure and track weather map |
| Cut Features | `re/patches/cut-feature-restoration-guide.md` | Snow, damage wobble, benchmark restoration |
| Asset Loading | `re/analysis/archive-and-asset-loading.md` | ZIP/VFS system, texture pipeline, vehicle loading |
| Save System | `re/analysis/cup-progression-save-system.md` | XOR encryption, CRC-32, cup state, unlock logic |
| Save Formats | `re/analysis/save-file-formats.md` | Byte-level Config.td5 and CupData.td5 layouts |
| Data Tables | `re/analysis/data-tables-decoded.md` | Cheat codes, surface friction, NPC groups |
| DXD3D Internals | `re/analysis/dxd3d-class-internals.md` | M2DX render state, texture formats, device loss |
| Debug System | `re/analysis/debug-diagnostics-system.md` | Startup tokens, benchmark, FPS overlay |

### 7.2 File Offset Calculation

For the unmodified retail EXE, sections are page-aligned with no padding gap:

```
file_offset = virtual_address - 0x400000
```

Example: function at VA 0x42B580 is at file offset 0x2B580.

### 7.3 PE Sections

| Section | VA Start | Purpose |
|---------|----------|---------|
| `.text` | 0x401000 | Code (864 functions) |
| `.rdata` | 0x45D000 | Read-only data (IAT, vtables, string literals) |
| `.data` | 0x463000 | Read-write data (globals, tables, BSS) |
| `IDCT_DAT` | -- | IDCT tables (video decoder) |
| `UVA_DATA` | -- | UV animation data |
| `tdb` | -- | Track database |

### 7.4 M2DX.dll Key Addresses

| Address | Purpose |
|---------|---------|
| 0x10002170 | `DXD3D::FullScreen(index)` |
| 0x10007EC0 | Display mode enumeration + res table builder |
| 0x10008020 | `RecordDisplayModeIfUsable` -- 4:3 aspect filter |
| 0x10008ED0 | `ApplyDirectDrawDisplayMode` wrapper |
| 0x100062A0 | `DXDraw::Create` |
| 0x10001770 | `SetRenderState` -- full baseline inside BeginScene/EndScene |
| 0x10001700 | `TextureClamp` -- 0=WRAP, nonzero=CLAMP |

Resolution table: base 0x10031024, stride 20 bytes, count at 0x10032678, max 50 entries.

### 7.5 Conventions

- All game functions use `__cdecl` calling convention (caller cleans stack)
- All COM/DirectX interfaces use `__stdcall` (callee cleans stack, first arg = this)
- Fixed-point arithmetic: most position values are 24.8 (divide by 256 for world units)
- Physics values: most use 8.8 fixed-point (divide by 256 for real multiplier)
- M2DX classes (DXD3D, DXDraw, DXInput, DXSound, DXD3DTexture) use static `__cdecl` methods with no vtable -- they are NOT C++ virtual classes
