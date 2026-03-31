# Test Drive 5 Engine Dossier

Status: `core-mapped`

This file is the shortest engine-wide summary Claude should read before drilling into subsystem files.

## Binaries

| Binary | Role | Status | Notes |
|---|---|---|---|
| `TD5_d3d.exe` | Main game executable and process host | `confirmed` | x86 PE, image base `0x00400000`, `856` functions in the current Ghidra analysis |
| `M2DX.dll` | Primary Direct3D-era engine/runtime dependency | `confirmed` | `438` functions; owns substantial rendering, input, audio, and platform glue |
| `M2DXFX.dll` | Alternate 3dfx/FX path | `strongly-suspected` | Secondary branch, deprioritized until Direct3D path is mapped |
| `Language.dll` | UI strings and localized text | `confirmed` | Useful for menu/frontend recovery |
| `VOODOO2.exe` | Supporting executable for alternate renderer path | `hypothesis` | Keep isolated from the main Direct3D RE flow for now |

## Current Working Model

| Area | Ownership | Confidence | Source |
|---|---|---|---|
| Process startup and main host | `TD5_d3d.exe` | `confirmed` | Existing repo notes and PE metadata |
| DirectDraw / Direct3D runtime | `M2DX.dll` | `confirmed` | Export surface plus direct decompilation of device, mode, and present flow |
| DirectInput runtime | `M2DX.dll` | `confirmed` | `DXInput::Create` / `Destroy` decompilation |
| High-level race / frontend flow | `TD5_d3d.exe` | `confirmed` | EXE-side host loop, frontend loop, and race/session passes |
| Menu/UI strings | `Language.dll` | `confirmed` | Existing repo notes |
| Track, traffic, car, and media data | archive files under root | `confirmed` | Existing tooling and reports |
| Glide/3dfx-specific path | `M2DXFX.dll` and `VOODOO2.exe` | `strongly-suspected` | Existing runtime notes |

## Coverage Snapshot

| Scope | Coverage | Notes |
|---|---|---|
| All live-named functions | `~795 / 1294` | `~504 / 856` in `TD5_d3d.exe`, `291 / 438` in `M2DX.dll` -- ~120 functions named across 6 sessions on 2026-03-19 |
| Important catalog functions | `101 / 101` | all entries in `catalog/functions.md` now have live semantic names |

## Confirmed Startup Anchors

| Symbol | Address | Status | Notes |
|---|---|---|---|
| `entry` | `0x004493e0` | `confirmed` | CRT/bootstrap entrypoint, captures OS version, heap/TLS init, then calls `FUN_00430a90` |
| `InitProcessHeap` | `0x00449cce` | `confirmed` | process heap/bootstrap allocator setup via `HeapCreate` |
| `InitPrimaryTlsSlot` | `0x004499d0` | `confirmed` | TLS slot allocation and thread bootstrap |
| `FatalStartupExit` | `0x0044950d` | `confirmed` | fatal startup exit path via `ExitProcess(0xff)` |
| `FUN_00430a90` | `0x00430a90` | `confirmed` | host loop around `DXWin::Environment`, `DXWin::Initialize`, Win32 message pump, and `FUN_00442170` |
| `FUN_00442170` | `0x00442170` | `strongly-suspected` | early frontend/frame-state loop with `InitializeMemoryManagement`, `SetRenderState`, movie handling, flip, and keyboard exit check |

## Confirmed `M2DX.dll` Anchors

| Symbol | Address | Status | Notes |
|---|---|---|---|
| `DXDraw::Create` | `0x100062a0` | `confirmed` | Enumerates adapters, loads DirectDraw, configures cooperative level, and seeds display-mode state |
| `ProbeDirectDrawCapabilities` | `0x10007960` | `confirmed` | Queries DirectDraw caps and records capability flags used during device setup |
| `EnumerateDisplayModes` | `0x10007ec0` | `confirmed` | Enumerates and sorts display modes, then selects preferred defaults |
| `DXD3D::Create` | `0x100010b0` | `confirmed` | Creates `IDirect3D`, selects the active D3D device, and boots the texture manager |
| `InitializeD3DDriverAndMode` | `0x100034d0` | `confirmed` | Resolves driver/mode selection, creates device surfaces, builds the viewport, and applies baseline render state |
| `CreateAndBindViewport` | `0x10002920` | `confirmed` | Creates the active D3D viewport, binds it to the device, and applies `SetViewport2` / `SetCurrentViewport` |
| `DXD3D::FullScreen` | `0x10002170` | `confirmed` | Rebuilds display surfaces and the D3D device for the chosen mode |
| `ResizeWindowedD3DDevice` | `0x10003940` | `confirmed` | Rebuilds the renderer for a new windowed client size |
| `InitializeD3DDeviceSurfaces` | `0x10004f30` | `confirmed` | Creates render-target surfaces, chooses the D3D device GUID, and initializes Z/texture support |
| `DXD3D::BeginScene` | `0x100015a0` | `confirmed` | Restores lost surfaces, clears the viewport, and begins the active scene |
| `DXD3D::SetRenderState` | `0x10001770` | `confirmed` | Applies baseline Z, fog, filter, and blend render states |
| `DXDraw::Flip` | `0x10006fe0` | `confirmed` | Presents fullscreen or windowed frames |
| `DXInput::Create` | `0x10009530` | `confirmed` | Initializes DirectInput and acquires keyboard/joystick devices |

## Confirmed Subsystem Anchors (2026-03-19)

### Camera System

| Symbol | Address | Status |
|---|---|---|
| `UpdateChaseCamera` | `0x00401590` | `confirmed` -- main chase camera, terrain-banking orbit |
| `UpdateVehicleRelativeCamera` | `0x00401c20` | `confirmed` -- cockpit/bumper fixed-offset camera |
| `UpdateTracksideCamera` | `0x00402480` | `confirmed` -- 11-behavior trackside dispatcher |
| `LoadCameraPresetForView` | `0x00401450` | `confirmed` -- loads from 7-preset table at 0x463098 |
| `InitializeTracksideCameraProfiles` | `0x004020b0` | `confirmed` -- per-track profile init |
| `InitializeRaceCameraTransitionDuration` | `0x0040a480` | `confirmed` -- sets 0xA000 fly-in timer |
| `ConfigureProjectionForViewport` | `0x0043e7e0` | `confirmed` -- FOV = width * 0.5625 |

### AI Command Chain

| Symbol | Address | Status |
|---|---|---|
| `UpdateActorRouteThresholdState` | `0x00434aa0` | `confirmed` -- primary AI throttle/brake writer |
| `ComputeAIRubberBandThrottle` | `0x00432d60` | `confirmed` -- distance-based catch-up scaling |
| `UpdateActorTrackBehavior` | `0x00434fe0` | `confirmed` -- pre-vehicle track behavior update |
| `AdvanceActorTrackScript` | `0x004370a0` | `confirmed` -- per-actor route command stream |
| `UpdateSpecialEncounterControl` | `0x00434ba0` | `confirmed` -- exclusive slot-9 encounter control |

### Particle and Effects System

| Symbol | Address | Status |
|---|---|---|
| `InitializeRaceParticleSystem` | `0x00429510` | `confirmed` -- pool init, SMOKE + RAINSPL sprites |
| `UpdateRaceParticleEffects` | `0x00429790` | `confirmed` -- per-view callback-driven update |
| `DrawRaceParticleEffects` | `0x00429720` | `confirmed` -- projection + callback-driven render |
| `FlushQueuedTranslucentPrimitives` | `0x00431340` | `confirmed` -- linked-list bucket sort flush |
| `FlushProjectedPrimitiveBuckets` | `0x0043e2f0` | `confirmed` -- 4096-bucket depth-sorted primitive flush |
| `InitializeWeatherOverlayParticles` | `0x00446240` | `confirmed` -- rain/snow init (snow cut) |
| `AdvanceWorldBillboardAnimations` | `0x0043cdc0` | `confirmed` -- police marker cosine pulse |
| `InitializeVehicleTaillightQuadTemplates` | `0x00401000` | `confirmed` -- BRAKED sprite quad alloc |

### Save / Profile System

| Symbol | Address | Status |
|---|---|---|
| `WritePackedConfigTd5` | `0x0040f8d0` | `confirmed` -- Config.td5 writer (XOR + CRC32) |
| `LoadPackedConfigTd5` | `0x0040fb60` | `confirmed` -- Config.td5 reader |
| `WriteCupData` | `0x004114f0` | `confirmed` -- CupData.td5 writer |
| `LoadContinueCupData` | `0x00411590` | `confirmed` -- CupData.td5 reader + restore |
| `ValidateCupDataChecksum` | `0x00411630` | `confirmed` -- CupData.td5 CRC32 check |
| `SerializeRaceStatusSnapshot` | `0x00411120` | `confirmed` -- live state to snapshot buffer |
| `RestoreRaceStatusSnapshot` | `0x004112c0` | `confirmed` -- snapshot to live state |
| `BuildRaceResultsTable` | `0x0040a8c0` | `confirmed` -- standings from actor state |
| `AwardCupCompletionUnlocks` | `0x00421da0` | `confirmed` -- car/group unlock on cup win |

### Frontend Options

| Symbol | Address | Status |
|---|---|---|
| `ScreenGameOptions` | `0x0041f990` | `confirmed` -- 7-row game settings |
| `ScreenControlOptions` | `0x0041df20` | `confirmed` -- device select + configure |
| `ScreenSoundOptions` | `0x0041ea90` | `confirmed` -- SFX mode, volumes, music test |
| `ScreenExtrasGallery` | `0x00417d50` | `confirmed` -- credits scroller with mugshots |

### Track Geometry

| Symbol | Address | Status |
|---|---|---|
| `BindTrackStripRuntimePointers` | `0x00444070` | `confirmed` -- STRIP.DAT header parse |
| `ComputeTrackSpanProgress` | `0x004345b0` | `confirmed` -- parametric span projection |
| `SampleTrackTargetPoint` | `0x00434800` | `confirmed` -- world-space interpolation + lateral offset |
| `ClassifyTrackOffsetClamp` | `0x004368a0` | `confirmed` -- 3-way boundary classifier |
| `CheckRaceCompletionState` | `0x00409e80` | `confirmed` -- circuit 4-bit bitmask + point-to-point |
| `NormalizeActorTrackWrapState` | `0x00443fb0` | `confirmed` -- modular span wrap |

## Reverse-Engineering Priorities

1. Establish binary ownership boundaries and startup control flow.
2. Build a stable symbol catalog for functions, globals, and structures.
3. Separate renderer, frontend, gameplay, asset, audio, and input subsystems.
4. Tie runtime observations to static addresses.
5. Keep unknowns explicit so Claude does not overfit on guesses.

## Evidence Discipline

- `confirmed`: direct disassembly, decompilation, import table, string xref, runtime trace, or byte-verified patch site.
- `strongly-suspected`: multiple converging clues, but missing direct proof.
- `hypothesis`: useful working theory that still needs proof.

## Expected Outputs

- `re/catalog/functions.md`: durable symbol table.
- `re/catalog/globals.md`: durable global/state table.
- `re/catalog/types.md`: recovered structures and enums.
- `re/systems/*.md`: subsystem narratives backed by evidence.
- `re/maps/*.md`: engine-level ownership and runtime maps.
- `re/sessions/*.md`: dated work logs with promotion targets.
