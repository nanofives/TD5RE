# td5mod Framework Analysis

Complete analysis of the td5mod modding framework for Test Drive 5 (1999).
All source files under `td5mod/` were reviewed.

---

## 1. Architecture

### Loading Mechanism

The mod uses **Ultimate ASI Loader** (an open-source DLL proxy loader). The loader is deployed as `dinput.dll` in the game directory. When TD5_d3d.exe loads, Windows loads the proxy `dinput.dll`, which scans `scripts/` for `.asi` files and loads them as DLLs.

```
TD5_d3d.exe (UNMODIFIED)
  +-- ddraw.dll (D3D11 wrapper OR dgVoodoo2)
  +-- dinput.dll (Ultimate ASI Loader, x86)
       +-- scripts/td5_mod.asi (this project)
            +-- MinHook trampolines
            +-- td5_sdk.h (864 named game functions + globals)
            +-- Feature modules (damage wobble, snow, widescreen, font, tuning, ...)
```

The ASI is a standard 32-bit DLL with `.asi` extension. `DllMain` with `DLL_PROCESS_ATTACH` calls `LoadConfig()` then `InstallHooks()`. Configuration is read from `scripts/td5_mod.ini` via Win32 `GetPrivateProfileIntA`/`GetPrivateProfileStringA`.

### Hooking Strategy

Two patching techniques are used:

1. **MinHook trampolines** -- for function-level hooks where the original must be callable. `MH_CreateHook` + `MH_EnableHook` on known VA addresses.
2. **Direct memory patching** -- `PatchMemory()` (VirtualProtect + memcpy) for byte-level patches (NOPs, JMPs, immediate operand changes, RET insertion). A helper `PatchCallSite()` writes `E8 rel32` CALL instructions with NOP padding.

### Hot-Reload

Pressing F11 at runtime re-reads select INI values (debug overlay toggle, menu frame cap, vsync, font settings). Checked per-frame from both the frontend `LogicGate_ScreenDispatch` and race `Hook_RenderRaceHudOverlays` paths.

---

## 2. Source File Inventory

| File | Purpose |
|------|---------|
| `td5_mod.c` | Main module (~2268 lines). DllMain, config, all hooks, widescreen, font fix, asset override, auto-dump |
| `td5_sdk.h` | Game SDK header. 864+ named functions, 100+ globals, all at known VAs (base 0x400000) |
| `td5_tuning.c` | AI/collision/physics tuning. INI-driven grip clamp, gravity, rubber-band, impulse scaling |
| `td5_tuning.h` | Public API for tuning module |
| `td5_asset_dump.c/h` | Saves every file loaded from ZIP archives to disk (organized by source) |
| `td5_png_replace.c/h` | Intercepts TGA loads, substitutes PNG files decoded to TGA in-memory via stb_image |
| `td5_gamemodes.c/h` | Custom game mode framework: time trial, survival, speed trap end conditions |
| `td5_log.h` | Structured logging system: per-category, leveled, file + OutputDebugString output |
| `ref/td5_gdi_text.c` | Reference/earlier standalone GDI text DLL (superseded by FontFix in td5_mod.c) |

---

## 3. All Hooks and Patches (Address Catalog)

### MinHook Function Hooks

| Address | Game Function | Hook | Feature |
|---------|--------------|------|---------|
| `0x446560` | `RenderAmbientParticleStreaks` | `Hook_RenderParticles` | Snow Rendering |
| `0x4388A0` | `RenderRaceHudOverlays` | `Hook_RenderRaceHudOverlays` | Debug Overlay |
| `0x425360` | `PresentFrontendBufferSoftware` | `Hook_PresentSoftware` | Widescreen (scale cave) |
| `0x4242B0` | `DrawFrontendLocalizedStringPrimary` | `Hook_DrawLocPrimary` | FontFix |
| `0x424390` | `DrawFrontendLocalizedStringSecondary` | `Hook_DrawLocSecondary` | FontFix |
| `0x424560` | `DrawFrontendLocalizedStringToSurface` | `Hook_DrawLocToSurface` | FontFix |
| `0x412D50` | `MeasureOrDrawFrontendFontString` | `Hook_MeasureOrDrawFont` | FontFix |
| `0x424A50` | `MeasureOrCenterFrontendLocalizedStr` | `Hook_MeasureOrCenter` | FontFix |
| `0x424660` | `DrawFrontendSmallFontStringToSurface` | `Hook_DrawSmallFontToSurface` | FontFix |
| `0x424740` | `DrawFrontendClippedStringToSurface` | `Hook_DrawClippedToSurface` | FontFix |
| `0x428240` | `InitRaceHudFontGlyphAtlas` | `Hook_InitRaceHudFontAtlas` | FontFix (HUD scaling) |
| `0x428320` | `QueueRaceHudFormattedText` | `Hook_QueueRaceHudText` | FontFix (HUD scaling) |
| `0x440790` | `ReadArchiveEntry` | `Hook_ReadArchiveEntry` | AssetOverride / AssetDump / PngReplace |
| `0x4409B0` | `GetArchiveEntrySize` | `Hook_GetArchiveEntrySize` | AssetOverride / PngReplace |
| `0x418450` | `NoOpHookStub` (9 lifecycle call sites) | `Hook_NoOpHookStub` | AutoDump state machine |
| `0x443280` | `LoadRaceVehicleAssets` | `Hook_LoadRaceVehicleAssets` | AutoDump |
| `0x410CA0` | `ConfigureGameTypeFlags` | `Hook_ConfigureGameTypeFlags` | GameModes |
| `0x409E80` | `CheckRaceCompletionState` | `Hook_CheckRaceCompletionState` | GameModes |
| M2DX+`0x12a10` | `DXWin::Initialize` | `Hook_DXWin_Initialize` | Windowed mode |
| `0x4079C0` | `ApplyVehicleCollisionImpulse` | `Hook_ApplyCollisionImpulse` | Tuning (V2V impulse) |
| `0x406980` | `ApplyTrackSurfaceForceToActor` | `Hook_ApplyTrackForce` | Tuning (V2W impulse) |
| `0x42F110` | `InitializeRaceVehicleRuntime` | `Hook_InitRaceVehicleRuntime` | Tuning (gravity) |
| `0x432E60` | `InitializeRaceActorRuntime` | `Hook_InitRaceActorRuntime` | Tuning (rubber-band) |
| `0x410CA0` | `ConfigureGameTypeFlags` | `Hook_ConfigGameType` | Tuning (traffic toggle) |
| `0x434DA0` | `UpdateSpecialTrafficEncounter` | `Hook_UpdateEncounter` | Tuning (encounter) |

### Direct Memory Patches (td5_mod.c)

| Address | Bytes | Description |
|---------|-------|-------------|
| `0x42C8D0` | 5 | DamageWobble: JMP to `_rand` (0x448157) replacing XOR EAX,EAX;RET |
| `0x43C440` | 1 | SkipIntro: RET (0xC3) at PlayIntroMovie entry |
| `0x42C8E0` | 1 | SkipLegals: RET (0xC3) at ShowLegalScreens entry |
| `0x466840` | 4 | Unlock all 19 circuit tracks (was 16) |
| `0x430AB2` | 4 | Widescreen: WinMain width immediate |
| `0x430AC2` | 4 | Widescreen: WinMain height immediate |
| `0x42A999` | 4 | Widescreen: menu init PUSH height |
| `0x42A99E` | 4 | Widescreen: menu init PUSH width |
| `0x42A9AD` | 4 | Widescreen: menu init float width |
| `0x42A9B7` | 4 | Widescreen: menu init float height |
| `0x43E828` | 1 | Widescreen: FOV fix struct offset 0x0C->0x10 |
| `0x43E82C` | 1 | Widescreen: FOV fix constant 0x8C->0x80 |
| `0x414BA7` | 1 | Widescreen: DEC EAX -> NOP (force blit every frame) |
| `0x414EB0` | 1 | Widescreen: INC EDX -> NOP (frame counter; LogicGate handles it) |
| `0x423E2A` | 1 | Widescreen: DDBLT_WAIT flag on ClearBackbufferWithColor |
| `0x423F77` | 1 | Widescreen: DDBLT_WAIT flag on FillPrimaryFrontendRect |
| `0x424E2A` | 1 | Widescreen: DDBLT_WAIT flag on FillPrimaryFrontendScanline |
| `0x414D09` | 10 | Widescreen: Frontend Flip -> ScaleCave_Present (CALL+NOPs) |
| `0x42CBBE` | 10 | Widescreen: Loading Flip -> ScaleCave_Present (CALL+NOPs) |
| `0x414C9E` | 6 | Widescreen: Screen dispatch -> LogicGate_ScreenDispatch (CALL+NOP) |
| M2DX+`0x61c1c` | 4 | Windowed: clear fullscreen flag |
| M2DX+`0x6637` | 1 | Windowed: JNZ->JMP bypass adapter check |
| M2DX+`0x78ca` | 1 | Windowed: JZ->JMP always windowed coop |
| M2DX+`0x80B3` | 1 | Widescreen: remove 4:3 filter (JE->JMP) |
| M2DX+`0x7F94` | 4 | Widescreen: EnumDisplayModes default width |
| M2DX+`0x7FA4` | 4 | Widescreen: EnumDisplayModes default height |
| M2DX+`0x2BC0` | 4 | Widescreen: SelectPreferredDisplayMode width |
| M2DX+`0x2BC9` | 4 | Widescreen: SelectPreferredDisplayMode height |

### Direct Memory Patches (td5_tuning.c)

| Address(es) | Description |
|-------------|-------------|
| `0x4042DE`, `0x4042FE`, `0x404316`, `0x40432B` | Player grip low clamp CMP imm8 (default 0x38) |
| `0x4042EC`, `0x404304`, `0x40431C`, `0x404330` | Player grip low clamp MOV imm32 fallback |
| `0x4042E3` | Player grip high clamp MOV EDX imm32 (default 0x50) |
| `0x4050BA`, `0x4050DA` | AI grip low clamp CMP imm8 (default 0x70) |
| `0x4050CA`, `0x4050E1` | AI grip low clamp MOV imm32 fallback |
| `0x4050BC` | AI grip high clamp MOV EDX imm32 (default 0xA0) |
| `0x42F29F`, `0x42F354`, `0x42F360` | Gravity immediates (hard/easy/normal) |
| `0x467380` | Gravity runtime variable |
| `0x463204` | V2V collision inertia K (500,000) |
| `0x463200` | V2W collision inertia K (1,500,000) |
| `0x463188` | Collision damage toggle |
| `0x473D9C`-`0x473DA8` | Rubber-band parameters (behind/ahead scale+range) |
| `0x473D64` | AI default throttle array (6 dwords) |
| `0x4B064C` | Encounter cooldown runtime variable |

---

## 4. Feature Modules

### 4.1 DamageWobble

Restores a cut feature. `GetDamageRulesStub` at `0x42C8D0` was originally `XOR EAX,EAX; RET` (always return 0). The patch overwrites it with `JMP 0x448157` (the CRT `_rand` function), so collision damage generates random wobble values instead of zero.

### 4.2 SnowRendering

Restores cut snow particles on level003 (snow weather type). `RenderAmbientParticleStreaks` (`0x446560`) skips rendering when `g_weatherType != 0`. The hook temporarily sets `g_weatherType = 0` before calling the original, then restores it. Snow renders as rain-style particle streaks.

### 4.3 WidescreenFix

Replaces 31 binary patches with runtime C code. Three subsystems:

- **ScaleCave_Present**: Blt-stretches the 640x480 back buffer onto the front surface at native resolution with optional 4:3 pillarbox (black bars). VSync via `IDirectDraw::WaitForVerticalBlank`.
- **LogicGate_ScreenDispatch**: Gates menu logic to configurable Hz (default 60) while rendering runs at monitor refresh. Replaced the original `CALL [0x495238]` with a C function.
- **Resolution patches**: 14 EXE byte patches + 5 M2DX.dll byte patches for resolution immediates, FOV correction (hor+ widescreen), DDBLT_WAIT flags, and frame counter control.

FOV fix changes the focal length calculation from `width * 0.5625` (4:3 assumption) to `height * 0.75` (correct for any aspect ratio).

### 4.4 SkipIntro / SkipLegals

Single-byte patches: writes `0xC3` (RET) at the entry of `PlayIntroMovie` (`0x43C440`) and `ShowLegalScreens` (`0x42C8E0`).

### 4.5 DebugOverlay

Hooks `RenderRaceHudOverlays` (`0x4388A0`). When enabled, renders FPS (derived from `g_normalizedFrameDt`), forward speed, and gear index using the game's own `QueueRaceHudFormattedText` at screen position (10,10).

### 4.6 FontFix

The most complex feature (~600 lines). Replaces the bitmap glyph atlas text rendering with Windows GDI `TextOutA` for crisp, scalable text.

- **Frontend text**: 7 MinHook hooks on localized draw/measure wrapper functions. Replaces BltFast glyph-by-glyph rendering with GDI `TextOutA` on DDraw surface DCs.
- **Deferred rendering**: When widescreen is active, frontend text is queued into `g_fontQueue[128]` at 640x480 virtual coordinates, then flushed to the front surface at native resolution by `FontFix_FlushToFrontSurface()` during `ScaleCave_Core()`.
- **Width tables**: GDI-measured per-character widths written into the game's `signed char[128]` width tables at `0x4662D0`-`0x466518` so the game's layout code uses correct metrics.
- **Race HUD scaling**: Hooks `InitRaceHudFontGlyphAtlas` (`0x428240`) and reimplements `QueueRaceHudFormattedText` (`0x428320`). The reimplementation separates UV coordinates (original atlas sizes) from screen quad sizes (scaled by `renderWidth/640` and `renderHeight/480`).
- **Custom font loading**: Loads a TTF file via `AddFontResourceExA(FR_PRIVATE)` and creates GDI fonts via `CreateFontA` with INI-configurable family, weight, and sizes.
- **Button backgrounds**: `Hook_DrawButtonBg` (currently disabled) replaces bitmap 9-slice button rendering with GDI `RoundRect` vector drawing with gradient borders.

### 4.7 AssetDump

Intercepts `ReadArchiveEntry` to save every file loaded from ZIP archives to a structured `td5_dump/` directory. Files are saved in original format (TGA, DAT, etc.) organized by archive source (levels/level001/, cars/cam/, static/, frontend/, etc.). Writes a manifest.txt with `entryName|zipPath|size|dumpPath` per file.

### 4.8 PngReplace

When a `.TGA` file is requested from a ZIP, checks for a matching `.png` in the configured PNG directory. If found, decodes the PNG with stb_image (compiled with `STB_IMAGE_IMPLEMENTATION`, PNG-only) and writes an uncompressed type-2 24-bit BGR TGA into the game's buffer. The TGA header is constructed manually (18 bytes + WxHx3 pixel data). Works independently of the rendering backend.

### 4.9 AssetOverride

Hooks `ReadArchiveEntry` (`0x440790`) and `GetArchiveEntrySize` (`0x4409B0`). Maps ZIP paths to loose file paths under `assets/` directory using the same subfolder logic as AssetDump:
- `level001.zip` -> `assets/levels/level001/`
- `cars\cam.zip` -> `assets/cars/cam/`
- `static.zip` -> `assets/static/`
- `Front End\FrontEnd.zip` -> `assets/frontend/`
- etc.

Priority order in `Hook_ReadArchiveEntry`: PngReplace (highest) -> loose file override -> original ZIP read. The hook also sets `TD5_LAST_ARCHIVE` environment variable as a side-channel for the ddraw wrapper's texture dump categorization.

### 4.10 AutoDump

State machine that programmatically cycles through all 19 circuit tracks with rotating car selection to capture all game textures. Hooks `NoOpHookStub` (`0x418450`, called at 9 lifecycle points per frame) for per-frame state updates. States: WAIT_MENU -> SETUP (set car/track globals, trigger race) -> WAIT_RACE -> WAIT_LOAD (configurable frames) -> EXIT_RACE (set `g_raceEndControl = 0x400000`) -> advance track -> repeat. Auto-disables in INI when complete.

### 4.11 Tuning (AI/Collision/Physics)

INI-driven parameter overrides in `td5_tuning.c`:

- **Grip clamps**: Patches CMP/MOV immediate operands in `UpdatePlayerVehicleDynamics` (0x404030, 4 wheels x 2 clamps = 8 sites) and `UpdateAIVehicleDynamics` (0x404EC0, 2 axles = 4 sites).
- **Gravity**: Scales the three difficulty-based gravity constants (hard/easy/normal) at their MOV immediates, plus a runtime hook on `InitializeRaceVehicleRuntime` to scale the written value.
- **Rubber-band**: Post-hook on `InitializeRaceActorRuntime` scales parameters at `0x473D9C`-`0x473DA8`.
- **V2V/V2W impulse**: Wrapper hooks on `ApplyVehicleCollisionImpulse` and `ApplyTrackSurfaceForceToActor` temporarily modify the inertia constants K (`0x463204`, `0x463200`) before calling the original.
- **Traffic/encounters**: Post-hook on `ConfigureGameTypeFlags` disables traffic. Wrapper on `UpdateSpecialTrafficEncounter` overrides cooldown reset value.

### 4.12 GameModes

Custom game mode framework in `td5_gamemodes.c`. Hooks `ConfigureGameTypeFlags` (`0x410CA0`) and `CheckRaceCompletionState` (`0x409E80`).

INI-defined modes (up to 16) are matched by `g_selectedGameType`. Overrides applied on top of vanilla: lap count, checkpoint time scaling, traffic/encounters toggle, AI count (1-5), damage toggle, reverse track, weather. Three custom end conditions (combinable):
- **TimeTrial**: Player finishes after N laps regardless of AI state.
- **Survival**: Actors eliminated when damage accumulator (`actor+0x2CC`) exceeds threshold. Last standing wins.
- **SpeedTrap**: Measures forward speed at designated checkpoint. Highest speed wins; positions assigned by speed ranking.

### 4.13 Windowed Mode

Hooks `DXWin::Initialize` in M2DX.dll (at DLL+`0x12a10`). Three M2DX patches: clear fullscreen flag, bypass adapter windowed check (JNZ->JMP), force DDSCL_NORMAL cooperative level (JZ->JMP). After the original creates the window, restyles it with `SetWindowPos` for borderless or captioned mode at configured size, centered on screen.

### 4.14 Logging System

`td5_log.h` provides structured logging with 12 categories (Core, Widescreen, Font, Physics, AI, Render, Input, Audio, GameMode, Snow, Collision, Camera). Per-category log levels (0=off through 4=debug) configurable via INI `[Logging]` section. Output to both `OutputDebugStringA` and optional log file. Thread-safe via `CRITICAL_SECTION`. Supports hot-reload of log levels.

---

## 5. DDraw Wrapper (`td5mod/ddraw_wrapper/`)

Despite the directory name, this is actually a **DirectDraw-to-D3D11 wrapper**. It exports `DirectDrawCreate`, `DirectDrawCreateEx`, `DirectDrawEnumerateA`, and `DirectDrawEnumerateExA` via `ddraw.def`, replacing the system `ddraw.dll`.

### Architecture

```
Game (TD5_d3d.exe) -> M2DX.dll -> Our COM objects -> D3D11 -> GPU
```

The wrapper implements the full DirectDraw4 + Direct3D3 COM interface chain that M2DX.dll expects:
- `IDirectDraw4` (`ddraw4.c`) -- cooperative levels, display mode enumeration, surface creation
- `IDirectDrawSurface4` (`surface4.c`) -- Blt, BltFast, Lock/Unlock, GetDC, Flip
- `IDirect3D3` (`d3d3.c`) -- device enumeration, viewport creation
- `IDirect3DDevice3` (`device3.c`) -- DrawPrimitive, SetRenderState, SetTexture, BeginScene/EndScene
- `IDirect3DViewport3` (`viewport3.c`) -- viewport management
- `IDirect3DTexture2` (`texture2.c`) -- texture handle management

### Key Design Points

- Pre-transformed vertices (XYZRHW) converted to NDC in vertex shader (`vs_pretransformed.hlsl`)
- All rendering uses HLSL shaders (no fixed-function pipeline in D3D11): 4 pixel shaders (modulate, modulate_alpha, decal, luminance_alpha) + composite shader
- D3D6 render states cached and mapped to immutable D3D11 state objects (6 blend states, 6 depth-stencil states, 4 sampler states)
- Frontend native-resolution scaling: surfaces created at native res, coordinates scaled in BltFast/Blt, Lock/Unlock provides virtual 640x480 buffer
- Compositing: BltFast (2D) content merged with D3D (3D) at present time via `Backend_CompositeAndPresent`
- PNG texture override via `png_loader.c` with stb_image

### Build

Compiled with MinGW 32-bit. Links against d3d11, dxgi, kernel32, user32, gdi32, uuid, dxguid. HLSL shaders are pre-compiled to byte arrays (`*_bytes.h`). Output: `ddraw.dll` deployed to game directory.

---

## 6. Dependencies and Build System

### Dependencies

- **MinHook** (TsudaKageyu) -- fetched automatically via CMake FetchContent or from local `deps/minhook-1.3.4/`
- **stb_image** (single-header) -- embedded in `td5_png_replace.c` and `ddraw_wrapper/src/stb_image.h`
- **stb_image_write** -- in wrapper for texture dumping
- **Ultimate ASI Loader** (ThirteenAG) -- external, x86 `dinput.dll` version
- **MinGW 32-bit** (local copy in `deps/mingw/`) or Visual Studio 2022 Build Tools

### Build System

Two build paths:

1. **CMake** (`CMakeLists.txt`): `cmake -B build -A Win32` (MSVC) or `cmake -B build -G "MinGW Makefiles"`. Fetches MinHook via git. Outputs `build/scripts/td5_mod.asi`.

2. **Standalone batch** (`build.bat`): Direct gcc invocations using local MinGW. Compiles MinHook sources (hook.c, buffer.c, trampoline.c, hde32.c), then td5_mod.c, td5_tuning.c, td5_asset_dump.c, td5_png_replace.c. Links as shared DLL with `.asi` extension. Strips symbols in release.

Install: `install.bat` copies the ASI and INI to the game directory (`td5mod/..`).

---

## 7. Relationship to RE Analysis

The td5_sdk.h header is explicitly generated from the Ghidra reverse engineering work ("864 named functions"). Every address used in hooks and patches corresponds to named functions and globals documented in the RE analysis files:

| SDK Definition | RE Analysis Source |
|---|---|
| `g_gameState` (0x4C3CE8) | `global-variable-catalog.md` |
| `g_weatherType` (0x4C3DE8) | `weather-particle-system.md` |
| `RenderAmbientParticleStreaks` (0x446560) | `weather-particle-system.md` |
| `GetDamageRulesStub` (0x42C8D0) | `rng-restoration-patch.md` |
| `ReadArchiveEntry` (0x440790) | `archive-and-asset-loading.md` |
| `ConfigureGameTypeFlags` (0x410CA0) | `race-progression-system.md` |
| `CheckRaceCompletionState` (0x409E80) | `race-progression-system.md` |
| Widescreen patches | `widescreen-patch-guide.md`, `hardcoded-resolution-audit.md` |
| Actor struct offsets (0x318, 0x36A, etc.) | `actor-struct-first-128-bytes.md`, `data-structure-gaps-filled.md` |
| Grip clamp addresses in UpdatePlayer/AIVehicleDynamics | `vehicle-dynamics-complete.md` |
| Rubber-band parameters (0x473D9C) | `ai-rubber-banding-deep-dive.md` |
| Collision inertia K (0x463204) | `collision-system.md` |
| Frontend text functions (0x4242B0, etc.) | `text-rendering-pipeline.md`, `gdi-text-rendering-implementation.md` |
| HUD glyph atlas (0x4A2CBC) | `surface-viewport-hud-rendering.md` |
| NoOpHookStub (0x418450) | `debug-hook-stubs.md` |
| M2DX.dll offsets | `m2dx-dxdraw-class-deep-dive.md`, `m2dx-resolution-hardcodes.md` |

The tuning module header explicitly states: "All addresses verified against Ghidra decompilation (port 8195)."

The SDK header covers ~100 game globals and ~80+ function typedefs spanning: core loop, HUD, rendering, camera, input, physics (16 functions), AI/traffic (20 functions), track/collision (15 functions), rendering (14 functions), frontend, save, and M2DX.dll functions. This represents a comprehensive modding API derived directly from the complete Ghidra analysis of all 1,761 functions across 4 binaries.

---

## 8. INI Configuration Summary

The runtime config (`scripts/td5_mod.ini`) has 11 sections:

| Section | Keys | Purpose |
|---------|------|---------|
| `[Features]` | DamageWobble, SnowRendering, WidescreenFix, SkipIntro, SkipLegals, DebugOverlay, FontFix, Windowed | Feature toggles |
| `[Widescreen]` | Width, Height, Pillarbox, VSync, MenuFrameCap | Resolution and display options |
| `[Windowed]` | Width, Height, Borderless | Window mode settings |
| `[FontFix]` | FontName, FontFile, FontWeight, SmallFontSize, LargeFontSize | GDI font configuration |
| `[AssetDump]` | Enable, DumpDir, SkipExisting | Archive extraction to disk |
| `[PngReplace]` | Enable, PngDir | PNG texture substitution |
| `[AssetOverride]` | Enable, AssetsDir | Loose file loading from extracted ZIPs |
| `[AutoDump]` | Enable, LoadFrames | Automated track/car cycling for texture capture |
| `[AITuning]` | AIGripScale*, AIRubberBandStrength, AISteeringBiasMax, AIThrottle/BrakeScale, Traffic*, Encounter* | AI behavior tuning |
| `[CollisionTuning]` | V2V/V2WImpulseScale, DamageEnabled, Player/AIGripClamp* | Collision physics tuning |
| `[PhysicsTuning]` | GravityScale, FrictionScale, TopSpeedScale | Vehicle physics tuning |
| `[GameModes]` + `[GameModeN]` | Count, Type, LapCount, CheckpointTimeScale, TrafficEnabled, AICount, EndConditions, etc. | Custom game mode definitions |
| `[Logging]` | LogFile, LogLevel, LogCore, LogWidescreen, ... (12 categories) | Debug logging control |
