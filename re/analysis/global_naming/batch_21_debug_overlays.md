---
batch: 21
area: debug_overlays_diagnostics
tier: T5
target_todos: []
ghidra_session: 4e6aff7bc16749d88e982b014a32d78e
analyzed_addresses: 0x00428d20, 0x00428d40, 0x00428d60, 0x00428d80, 0x0042b580, 0x00424e40, 0x0040ade0, 0x0040ae00, 0x00418450, 0x00415030, 0x00414f40, 0x0041f990, 0x0041d890, 0x00420400, 0x00415490, 0x0040f8d0, 0x00442170, 0x00422480, 0x004251a0
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration — Debug overlays + diagnostics

## Summary

- Functions analyzed: 19 (benchmark capture + report + frontend presentation + CPU detection + game-option toggles + main loop FSM + positioner debug tool)
- Unnamed DAT_* globals encountered: 35 (after de-dup)
- Already-named globals encountered (just noted): 28 (`g_benchmarkModeActive`, `g_benchmarkFirstFrameSkipped`, `g_benchmarkImageLoadPending`, `g_benchmarkImageDataPtr`, `g_benchmarkDecodedPixelPtr`, `g_benchmarkImageInfoStruct`, `g_frameEndTimestamp`, `g_framePrevTimestamp`, `g_normalizedFrameDt`, `g_smoothedFrameDt`, `g_reciprocalFrameDtMs`, `g_instantFPS`, `g_simTickBudget`, `g_simTickBudgetSnapshot`, `g_simTimeAccumulator`, `g_simulationTickCounter`, `g_raceSimSubTickCounter`, `g_subTickFraction`, `g_audioOptionsOverlayActive`, `g_inputPlaybackActive`, `g_gamePaused`, `g_cameraMode`, `gDifficultyEasy`, `gTrafficActorsEnabled`, `gCircuitLapCount`, `g_specialEncounterType`, `gSpecialEncounterConfigShadow`, `g_fogCapabilityEnabled`)
- Proposals — high confidence: 22
- Proposals — medium confidence: 8
- Proposals — comment-only (low confidence): 5

## Methodology

Started from the explicit debug/diagnostic anchors:
1. `WriteBenchmarkResultsTgaReport @ 0x00428D80` (referenced by "Min FPS: %d\tMax FPS: %d\tAvg FPS: %d" debug string at `0x00466bd4`)
2. `InitializeBenchmarkFrameRateCapture @ 0x00428D20` and `RecordBenchmarkFrameRateSample @ 0x00428D40` (single-caller `RunRaceFrame`)
3. `InitializeFrontendPresentationState @ 0x00424E40` — contains rdtsc/QPC CPU calibration + CPUID detection block + frontend hardware-flip capability probe
4. `RunRaceFrame @ 0x0042B580` — main frame timer (`timeGetTime()` reads, FPS calculation, benchmark sampling hook)
5. `ScreenPositionerDebugTool @ 0x00415030` — dev-only sprite positioning debug screen (table index 1, writes positioner.txt)
6. `ScreenGameOptions @ 0x0041F990` — 7-toggle Game Options screen including the "3D Collisions" debug toggle row
7. `ScreenDisplayOptions @ 0x00420400` — Display Options (Resolution, Fogging, Speed Readout, Camera Damping)
8. `RunMainGameLoop @ 0x00442170` — 4-state top-level FSM that switches into `GAMESTATE_BENCHMARK` (= 3) after a benchmark race to load+display the produced TGA
9. `NoOpHookStub @ 0x00418450` — empty `ret` hook called from frontend/race/actor/texture-page paths
10. The CPU-info broadcast site in `ProcessFrontendNetworkMessages @ 0x0041B610` (opcode 0x17 forwards `DAT_00497AA0..AC0..AC8` over DPLAY)

The relevance gate was: globals exclusively or primarily read/written by FPS sampling, CPU detection/calibration, frame-timer, benchmark TGA generation, debug-tool screens, or the 7-row `Game Options` block (which on close inspection is the user-facing override for ALL physics/render debug toggles).

## Proposals

### Benchmark FPS sample capture + report

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004a2cf4 | u32 (void*) | `g_benchmarkSampleBuffer` | **high** | Set ONLY by `InitializeBenchmarkFrameRateCapture` to `HeapAllocTracked(1000000)` (1 MB). Read by `RecordBenchmarkFrameRateSample` and `WriteBenchmarkResultsTgaReport` as `int[N]` of per-frame instant-FPS samples. 1 writer, 2 readers. | `td5_game.c` benchmark FPS sample buffer (currently stubbed) |
| 0x004a2cf8 | u32 | `g_benchmarkSampleCount` | **high** | Bump counter of frames captured into `g_benchmarkSampleBuffer`. Initialized to 0 by `InitializeBenchmarkFrameRateCapture`; incremented by `RecordBenchmarkFrameRateSample`; read by `WriteBenchmarkResultsTgaReport` for averaging. | `td5_game.c` benchmark FPS count |
| 0x004a2cfc | char[N] | `g_benchmarkReportTextScratch` | **high** | Static scratch buffer written by `FormatBenchmarkReportText @ 0x00428D60` via `wvsprintfA(&DAT_004A2CFC, format, args)`. Used by the report-text formatter for the TGA labels ("DirectX %d APIs", "Driver Version %d.%02d", etc.). Size is implicit (must hold one localized line). | (port doesn't generate benchmark TGA) |

### CPU detection + calibration block (broadcast to network peers as 0x20-byte payload)

These 5 globals at `0x00497AA0..0x00497AC8` are written ONCE by `InitializeFrontendPresentationState` and read by `WriteBenchmarkResultsTgaReport` (locally) AND by `ProcessFrontendNetworkMessages` opcode 0x17 (broadcast for lobby display).

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497aa0 | char[12] | `g_cpuVendorString` | **high** | `cpuid(0)` writes EBX/EDX/ECX vendor string ("GenuineIntel", "AuthenticAMD", etc.) — stored as 3 dwords at `0x00497aa0` (EBX), `0x00497aa4` (EDX), `0x00497aa8` (ECX). Broadcast to network peers and printed on benchmark TGA. | `td5_game.c` (could query `__cpuid` on Win32) |
| 0x00497aac | u32 | `g_cpuVersionInfo` | **high** | `cpuid(1)` returns EAX (version info: stepping/model/family/type) at `_DAT_00497AAC`. 2 writers (in CPUID-supported branch + fallback zero), 1 read in net broadcast. | (none) |
| 0x00497ab0 | u32 | `g_cpuFeatureFlags` | **high** | `cpuid(1)` returns EDX feature bits at `DAT_00497AB0`. Used to derive `DAT_00495234` (MMX flag = bit 23) and `_DAT_004951D4` (TSC flag = bit 4). | (port could mirror but not used) |
| 0x00497ac0 | char[N] | `g_osVersionString` | **high** | Written by `sprintf_game(&DAT_00497AC0, fmt)` at end of `InitializeFrontendPresentationState`. Format string at `0x0046661C`. Broadcast to peers and embedded in benchmark report. | (none) |
| 0x00497ac8 | u32 | `g_cpuMhzEstimate` | **high** | Computed by the rdtsc/QueryPerformanceCounter calibration loop at the end of `InitializeFrontendPresentationState`: `(tsc_delta * qpc_freq) / qpc_delta / 1000000`. Single write, 1 read in net broadcast. | (port could use `QueryPerformanceFrequency` directly) |

### CPU/system capability flags

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00495234 | u32 | `g_cpuHasMmx` | **high** | Derived from `g_cpuFeatureFlags >> 23 & 1` (MMX feature bit). Read by `LoadExtrasGalleryImageSurfaces`, `UpdateExtrasGalleryDisplay`, and `InitializeFrontendDisplayModeState` to GATE the use of MMX-accelerated extras gallery image surfaces. 6 readers. | `td5_render.c` always-MMX assumption (port targets x86 baseline) |
| 0x004951d4 | u32 | `g_cpuHasTsc` | **high** | Derived from `g_cpuFeatureFlags >> 4 & 1` (TSC feature bit). Set to 1 only after successful `rdtsc`/`QueryPerformanceCounter` calibration; reset to 0 on calibration failure. Read by `Copy16BitSurfaceRect`-adjacent paths via `0x4951D4` boundary. 4 xrefs. | (port uses `QueryPerformanceFrequency` unconditionally) |
| 0x004951ec | u32 | `g_frontendHardwareFlipEnabled_Alt_PROVISIONAL` | medium | Existing `g_frontendHardwareFlipEnabled` is at a different address — this `_DAT_004951EC` is a SECOND flag written alongside it 5 times in `InitializeFrontendPresentationState`. Reads in `Copy16BitSurfaceRect` (line: `if (DAT_004951EC != 0) call IDirectDraw::BltFast(); else memcpy`). Likely the "use hardware blit vs software memcpy fallback" gate. | (port doesn't have DDraw fallback) |

### Frame-timing magic constants (read-only data segment)

These ARE referenced via `_DAT_*` autonames in `RunRaceFrame` — let's name them for the consolidation. All 8-byte doubles or 4-byte floats in the `.rdata` segment at `0x0045D650..0x0045D68F`.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d650 | float (4.0f) | `g_simTickBudgetCap` | **high** | Read by `RunRaceFrame` end: `if (_DAT_0045D650 < g_simTickBudget) g_simTickBudget = 4.0;`. Sole purpose: cap the per-frame sim tick accumulator to prevent run-away spiral on slow hosts. Byte pattern `40 80 00 00` = float 4.0. | `td5_game.c` `TD5_SIM_TICK_BUDGET_CAP = 4.0f` |
| 0x0045d658 | double (1/1000) | `g_msPerSec_double` | **high** | Read in `RunRaceFrame` via FLD/FMUL for the ms→s conversion: `g_normalizedFrameDt = local_8 * _DAT_0045d660 * _DAT_0045d658`. Byte pattern `fc a9 f1 d2 4d 62 50 3f` = double 0.001. | `td5_game.c` time scaling const |
| 0x0045d660 | double (30.0) | `g_targetFps_double` | **high** | Read in same FMUL chain as `g_msPerSec_double`. Byte pattern `00 00 00 00 00 00 3e 40` = double 30.0. The product (`dt_ms × 30 / 1000`) yields normalized "sim ticks @ 30 FPS" = port's standard frame quantization. | `td5_game.c` `TD5_SIM_TARGET_FPS = 30.0f` |
| 0x0045d668 | double (1000.0) | `g_msPerSec_reciprocal` | **high** | Read by `RunRaceFrame`: `g_instantFPS = fVar13 * (float)_DAT_0045d668;`. Byte pattern `00 00 00 00 00 40 8f 40` = double 1000.0. Multiplies `(1/frame_dt_ms)` to get FPS. | `td5_game.c` ms→fps converter |
| 0x0045d670 | double (1.0) | `g_oneDouble_PROVISIONAL` | medium | Used as `((double)_DAT_0045d670 / (float)dt) → reciprocalFrameDtMs`. Byte pattern `00 00 00 00 00 00 f0 3f` = double 1.0. Generic 1.0 constant; might be shared with other render-frame code. | `td5_game.c` reciprocal numerator |
| 0x0045d68c | float (1/65536) | `g_subTickQuantum` | **high** | Read by `g_subTickFraction = (dword)((float)(uint)local_8 * _DAT_0045d68c)`. Byte pattern `00 00 80 37` = float `1.52587890625e-05` = `1/65536`. Converts 24.8 fixed-point sim accumulator to normalized sub-tick fraction. | `td5_game.c` `TD5_SUB_TICK_QUANTUM = 1.0f/65536.0f` |

### Sprite positioner debug tool state (dev-only)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0049521c | int | `g_positionerSelectedGlyphIndex` | **high** | Selected glyph in `ScreenPositionerDebugTool` (the dev sprite-positioning tool registered at frontend screen index 1). Modified by arrow keys (`g_frontendInputEdgeBits & 0x1/0x2/0x200/0x400`). | (port doesn't ship this tool) |
| 0x00495260 | int[37] (stride 8) | `g_positionerGlyphRectsPrimary` | **high** | 37-entry array of (x, y) pairs (interleaved with `g_positionerGlyphRectsSecondary` — actually it's a flat `(x,y)[37]` packed as `int X, int Y, int X, int Y, ...` × 37 = 296 bytes). Written by the cursor manipulation in `ScreenPositionerDebugTool` case 3, dumped to `positioner.txt` on save. | (port doesn't ship this tool) |
| 0x00495264 | int[37] (stride 8) | `g_positionerGlyphRectsSecondary` | **high** | Second 37-entry set at offset +4 from the primary (`(&DAT_00495260)[i*2]` = X, `(&DAT_00495264)[i*2]` = Y per record). Modified by up/down arrows in case 4. Persisted alongside the primary set to `positioner.txt`. **Together these 2 form `g_positionerEditedRects[37][2]` — a struct-typing candidate.** | (port doesn't ship this tool) |

### Game Options 7-toggle shadow block (the "3D Collisions" toggle is here)

This is the persisted-to-Config.td5 block of frontend toggles. The block is 7 dwords (`0x00466000..0x0046601C`); `WritePackedConfigTd5` round-trips it as a unit. Three siblings already named (`gSpecialEncounterConfigShadow` at +0x0C, `g_raceDifficultyTier` at +0x10). The remaining 4 entries are the unnamed ones:

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00466000 | u32 (range 0..3) | `gCircuitLapsConfigShadow` | **high** | Index 0 of Game Options screen (`SNK_CircuitLapsTxt`). Range-clamped 0..3 by `case 0:` arm at `0x0041FFFE`. Live mirror is `gCircuitLapCount = gCircuitLapsConfigShadow + 1` (in `ScreenMainMenuAnd1PRaceFlow`). 10 xrefs across Config save, both Options screens, and OptionsHub. | `td5_save.c` lap count shadow |
| 0x00466004 | u32 (0/1) | `gCheckpointTimersConfigShadow` | **high** | Index 1 of Game Options (`SNK_CheckpointTimersButTxt`). On/Off toggle. Live mirror is `g_specialEncounterType = gCheckpointTimersConfigShadow` (in main-menu commit). 9 xrefs. | `td5_save.c` checkpoint timers shadow |
| 0x00466008 | u32 (0/1) | `gTrafficConfigShadow` | **high** | Index 2 of Game Options (`SNK_TrafficButTxt`). Live mirror is `gTrafficActorsEnabled` (already named). 7 xrefs. | `td5_save.c` traffic config shadow |
| 0x00466014 | u32 (0/1) | `gDynamicsConfigShadow` | **high** | Index 5 of Game Options (`SNK_DynamicsButTxt`). Live mirror is `gDifficultyEasy = gDynamicsConfigShadow` (in OptionsHub commit). 6 xrefs. **The "Dynamics" label is a misleading public-facing name for an off-on physics-quality switch that maps to the `gDifficultyEasy` flag.** | `td5_save.c` dynamics config shadow |
| 0x00466018 | u32 (0/1) | `g3dCollisionsConfigShadow` | **high** | Index 6 of Game Options (`SNK_3dCollisionsButTxt`). On/Off toggle. Live mirror is `g_cameraMode = g3dCollisionsConfigShadow ^ 1` (in OptionsHub commit) — i.e. when "3D Collisions" is ON the camera mode is 0 ("locked"), when OFF camera mode is 1 ("free"). **The toggle is mislabeled as "3D Collisions" but actually flips between two camera-collision modes.** 6 xrefs. | `td5_save.c` camera-mode config shadow |

### Display Options 4-toggle shadow block (graphics)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00466024 | u32 (0/1) | `gFoggingConfigShadow` | **high** | Index 1 of Display Options (`SNK_FoggingButTxt`); gated by `DXD3D::CanFog()` capability. Live mirror is `g_fogCapabilityEnabled = gFoggingConfigShadow` (in main-menu commit). 7 xrefs. | `td5_save.c` fog shadow |
| 0x00466028 | u32 (0/1) | `gSpeedReadoutUnitsConfigShadow` | **high** | Index 2 of Display Options (`SNK_SpeedReadoutButTxt`). 0 = mph, 1 = kph (or vice versa — used in HUD digit formatter via `BuildRaceHudMetricDigits`). Live mirror is `DAT_004B11C4 = gSpeedReadoutUnitsConfigShadow`. 9 xrefs. | `td5_save.c` units shadow |

### Frontend FSM main-menu route hint

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004962d4 | int | `g_mainMenuButtonHint` | medium | Set to 1..6 in `ScreenMainMenuAnd1PRaceFlow case 4` based on which top-level button was pressed (Race / Quick / TimeDemo / NetPlay / Options / HiScore). Read by post-race results FSM to decide return path. 15 xrefs. Not strictly debug, but ENSURES the timedemo button properly cascades the benchmark path. | `td5_frontend.c` button hint cache |
| 0x004962d8 | char* | `g_frontendMustSelectMessage_PROVISIONAL` | medium | Holds pointer to either `SNK_MustPlayer1Txt` or `SNK_MustPlayer2Txt` depending on which slot lacks an input device. Read by `case 0x15` to draw the warning overlay. 5 xrefs. | (port player-input gate) |
| 0x0049636c | u32 (0/1) | `g_benchmarkRequestPending` | **high** | Set to 1 by `ScreenMainMenuAnd1PRaceFlow case 4 button 2` (when TimeDemo button pressed in retail-debug build). Read by `RunRaceResultsScreen case 0` to fast-forward back to the menu/benchmark-image-display loop. Also set to 0 by `ScreenStartupInit`. 3 xrefs — clear semantic. | `td5_game.c` benchmark request gate |

### Per-view HUD indicator cache (jump-warn / arrow flash counters)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004b1120 | int[2] | `g_hudPrevSpeedCache` | medium | `&DAT_004b1120 + view` — set to `iVar10 = (int16)actor.field_0x80` (= player speed) by `RenderRaceHudOverlays` after the route-heading-divergence check; read next frame to detect deceleration spike (which counts up the wrong-way-warning fade). 2 xrefs. | (port currently always-on) |
| 0x004b11a8 | int[2] | `g_hudIndicatorState` | medium | Per-view HUD indicator state value. Written ONLY by `SetRaceHudIndicatorState @ 0x00439B60` (a single-line setter); read by `RenderRaceHudOverlays` to gate the "checkpoint-passed flash" / "milestone reached" sprite emission. **Named because of the dedicated setter — clear single-purpose value.** | `td5_hud.c` indicator state |
| 0x004b11b0 | int | `g_hudCurrentViewIndex` | **high** | The view-iteration index used by `RenderRaceHudOverlays` to walk through the active viewports. Re-read by every HUD branch; set/incremented by the outer loop. 10 xrefs all in HUD render. **This is the HUD-specific twin of `g_activeRenderViewIndex` (T3 batch 15).** | `td5_hud.c` current view |

### Comment-only / low-confidence

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497ab8 | int | (comment-only: frontend client origin X) | low | Set to 0 then conditionally to ClientToScreen result in `InitializeFrontendPresentationState`. Paired with `0x00497ABC` (Y). Used only when windowed. Not exercised in benchmark/diagnostics path proper. | (none) |
| 0x00497abc | int | (comment-only: frontend client origin Y) | low | Sibling of `0x00497AB8`. | (none) |
| 0x00497a78 | int | (comment-only: race-results saved-state stage) | low | 3-state stage marker (0/1/2) for whether the snapshot of `g_selectedCarIndex` / race options has been saved into `0x00497A7C..0x00497A98` for restoring after race-results. Adjacent to results-screen state, not debug. | (none) |
| 0x004b11c4 | u32 (0/1) | (comment-only: speed-readout-units live cache) | low | Already-known via cross-reference (`DAT_004B11C4 = DAT_00466028` in main-menu commit). Adds nothing new beyond the shadow→live mapping documented under `gSpeedReadoutUnitsConfigShadow`. | (port reads from save struct) |
| 0x004962d0 | u32 | (comment-only: career restart gate) | low | Written 2× during frontend bootstrap; read once by `RunRaceResultsScreen` to gate the "go directly to results" fast path. Unclear single-purpose — likely a career-stage restart cookie. 3 xrefs. | (none) |

## Key discoveries

1. **`NoOpHookStub @ 0x00418450` is the dev-build profiler-instrumentation slot.** It's a single-instruction function (`ret`) called from `RunRaceFrame`, frontend update, actor render, and texture-page paths. The retail build patched it to a no-op; the dev build had timing/logging stubs here. **For the port, this is the natural hook point if we ever want to add per-frame profiling**. No state to recover, but a clear architectural marker.

2. **The "3D Collisions" Game Options toggle is mislabeled — it's actually a camera-collision-mode switch.** `g_cameraMode = g3dCollisionsConfigShadow ^ 1` means ON→0, OFF→1 (not a collision-detection enable/disable). The port likely interprets this string literally; the orig actually flips `g_cameraMode` between two camera behavior modes. **Potential bug**: any port code that gates COLLISION DETECTION on this flag would be wrong; it should gate CAMERA BEHAVIOR.

3. **The "Dynamics" Game Options toggle drives `gDifficultyEasy`, not a physics-quality switch.** Despite the public-facing "Dynamics" string, the actual variable controlled is the easy-mode AI tuning flag (already named `gDifficultyEasy`). This is the SECOND mislabel in the same screen — port may want to relabel these in the UI.

4. **The CPU-detection block at `0x00497AA0..0x00497AC8` is BOTH a benchmark report field AND a network lobby broadcast payload.** A single 0x20-byte block (vendor string + version info + feature flags + OS-version string + MHz estimate) is computed once at startup and BOTH (a) printed onto the FPS benchmark TGA AND (b) broadcast via DXPlay opcode 0x17 to peers in network lobby. **This explains why the lobby always shows opponent's CPU model and clock speed.** The port currently has no equivalent block.

5. **The benchmark "image mode" (GAMESTATE = 3) is a distinct top-level state.** After completing a benchmark race, `RunRaceFrame` returns to `RunMainGameLoop` which then enters `GAMESTATE_BENCHMARK` (= `GAMESTATE_MENU + 2`). In this state the generated `fps.tga` file is reloaded from disk, decoded, and displayed via direct surface lock+blit. Pressing any key returns to the menu. **The port currently lacks this fourth state** — port's 4-state FSM in CLAUDE.md says "intro/menu/race/benchmark" but the benchmark state is the **image-display** state, not the race-with-FPS-capture state (that's just `GAMESTATE_RACE` with `g_benchmarkModeActive = 1`).

6. **The frame-timing constants form a precise 30 FPS quantization chain.** `_DAT_0045D650` (= 4.0) caps `g_simTickBudget`; the chain `dt_ms × 1000.0 × 0.001 × 30.0` produces normalized sub-ticks @ 30 Hz; `_DAT_0045D68C` (= 1/65536) converts 24.8 fixed-point sim accumulator to `g_subTickFraction`. **The port's `td5_game.c` should mirror these exact constants — any divergence will produce visible frame-rate sensitivity in the physics.**

7. **`ScreenPositionerDebugTool` is registered at frontend table index 1 but has no normal entry path.** The screen table at `0x004655C4` has `0x00415030` (the positioner) as its 2nd entry, but no code path calls `SetFrontendScreen(1)` in retail. It would need to be triggered by a debug hotkey (which the retail build doesn't ship) or via a build-config flag. **Dead-in-retail tool, NOT dead code in the binary.** Suitable for the port to re-enable as a debugging aid if ever needed for HUD layout work.

8. **`g_cpuHasMmx @ 0x00495234` actually gates the MMX-accelerated extras gallery surfaces.** Three TGA loads (`extras/pic1..pic5.tga`) are gated by this flag — non-MMX CPUs skip the extras-gallery menu entirely. **Discovery**: the menu entry "Extras" is conditionally available based on detected MMX capability. Port should always treat this as 1 (every x86 since 1997 has MMX).

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00497250 | DXPlay session-config 30-dword scratch (lobby) | T5 batch_24 network |
| 0x00497a60 | race-results sort/walk state cluster (`DAT_004969a0..DAT_004972c8`) | T5 race-results batch |
| 0x00497a64..0x00497a78 | post-race button-press cache + snapshot stage | T5 race-results batch |
| 0x00497a7c..0x00497a98 | race-options snapshot block (`g_selectedCarIndex`, `DAT_0048F368/0/8/B0/C/4/C/8` mirrors) | T5 race-results batch |
| 0x00496978..0x004969d8 | DPLAY message ring buffer (`ReceiveMessage` queue + slot table) | T5 batch_24 network |
| 0x004967e4..0x00496828 | chat scrollback buffer (5-row × 49-int ring) | T5 chat batch |
| 0x004968ac..0x004968c8 | outgoing packet scratch buffer (`DXPlay::SendMessageA` send-staging) | T5 batch_24 network |
| 0x00496408..0x0049640c | chat-message ring state (count + dirty-flag) | T5 chat batch |
| 0x004969d8..0x00497060 | 8-slot replay ring for `ReceiveMessage` | T5 batch_24 network |
| 0x0046661C | format string `"DX: %s"` or similar OS-info concat — embedded in net broadcast | T5 strings batch (no-op) |
| 0x00466bcc..0x00466bd0 | `tga\0fps.tga\0` two strings — the default filename for benchmark TGA (used by `_makepath` when `FPSName_exref` provides an empty config path) | T5 strings batch |
| 0x00466bd4..0x00466c1c | benchmark TGA labels ("Min/Max/Avg FPS", "Minimum/Maximum/Average FPS:", "Min FPS: %d\tMax FPS: %d\tAvg FPS: %d\n") | T5 strings batch |
| 0x00466c2c..0x00466d2c | benchmark TGA system-info labels ("Triple Buffer", "Mip Mapping", "WBuffer", "Processor", "Physical Memory", "Operating System", "Screen Mode", "Texture Memory", "DirectX %d APIs", "Build", "Test Drive 5") | T5 strings batch |
| 0x0048DBA0, 0x0048DBA8 | `InitializeRaceRenderGlobals` one-time-init flag + counter (flagged in batch_15) | T5 render-init batch |
| 0x0048DB90, 0x0048DB94 | FPU control word save/restore pair (flagged in batch_15) | T5 FPU-state batch |
| 0x00466528..0x0046652C | font-glyph width table | T5 font-glyph batch |
| 0x004962C0, 0x004962BC | secondary frontend cursor X/Y caches (state preserved across screens) | T5 frontend-cursor batch |
| 0x0048F300..0x0048F31C | extras-gallery image-pointer set (5 surfaces for pic1..pic5) | T5 extras-gallery batch |

## TODO impact

No TODOs in the `re/analysis/` tree directly target the debug/diagnostic area (collision overlay is a port-only F12 feature with no orig analog; FPS counter is benchmark-only and never displayed in-race in orig). 3 indirect impacts:

- **`todo_smoke_render_broken_2026-05-19`** (closed via batch_15): unrelated.
- **`todo_camera_lags_post_race_start_2026-05-17`**: investigation noted that `g_cameraMode` is set from `g3dCollisionsConfigShadow ^ 1`. If the camera-lag bug correlates with the "3D Collisions" toggle being OFF in the default config, that's a new bisection vector — but probably not the root cause (the lag bug was attributed to commit 630d797).
- **`todo_ini_refactor_2026-05-19`**: the port's `td5re.ini` may need to ship overrides for the `gCircuitLapsConfigShadow`/`gCheckpointTimersConfigShadow`/`gTrafficConfigShadow`/`gDynamicsConfigShadow`/`g3dCollisionsConfigShadow`/`gFoggingConfigShadow`/`gSpeedReadoutUnitsConfigShadow` block since they're persisted to Config.td5 in orig. Port currently has no Config.td5 reader for these.

## Ghidra session notes

- Session `4e6aff7bc16749d88e982b014a32d78e` opened `TD5_pool1` read-only as required (TD5_pool0 was locked at the start).
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; cleanup attempted via `bash scripts/ghidra_pool.sh cleanup`.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
- The 7-row Game Options shadow block is a strong struct-promotion candidate: `struct TD5_GameOptionsShadow { u32 circuit_laps, checkpoint_timers, traffic, cops, difficulty_tier, dynamics, threed_collisions; }` at `0x00466000`, followed by 3 more dwords (fog/speed-units/camera-damping) that form the Display Options sub-block.
- The CPU-info block at `0x00497AA0..0x00497AC8` is a struct-promotion candidate: `struct TD5_CpuInfo { char vendor[12]; u32 version_info; u32 feature_flags; u32 _pad; char os_string[16]; u32 mhz; }` — broadcast over network and printed on benchmark TGA.
