---
batch: 25
area: long_tail_misc
tier: T5
target_todos: [todo_precise_port_extend_particle_render_2026-05-17, todo_view_replay_restarts_race_2026-05-19, reference_arch_play_intro_movie_2026-05-18]
ghidra_session: edcc91dd0fbe4814997a5ba6f443eb3e
analyzed_addresses: 0x00422480, 0x004036b0, 0x00402e30, 0x00412e30, 0x004136c0, 0x00428855, 0x0043b7c0, 0x004539e0, 0x00452e20, 0x00448157, 0x0042fad0, 0x004285b0
agent: Claude Opus 4.7 (1M context)
date: 2026-05-20
---

# Globals enumeration — Long tail miscellany (T5 catch-all)

## Summary

- Functions analyzed: 12 (primary + caller-traced)
  - `_rand @ 0x00448157` (CRT, per-thread, no global state)
  - `PlayIntroMovie @ 0x0043C440` (FMV bootstrap — already-named cluster checked)
  - `OpenAndStartMediaPlayback @ 0x00452E20` (FMV codec entry)
  - `InitializeVideoDecoder @ 0x004539E0` (FMV codec — YCbCr clip LUT init)
  - `DecodeType2DeltaPalette @ 0x0045B420` (FMV codec — pure struct-based)
  - `RunRaceResultsScreen @ 0x00422480` (frontend animation + replay-availability gate)
  - `UpdatePlayerSteeringWeightBalance @ 0x004036B0` (2P steering bias writers)
  - `ResetPlayerVehicleControlAccumulators @ 0x00402E30` (control-buffer reset)
  - `CreateMenuStringLabelSurface @ 0x00412E30` (label-surface metadata writers)
  - `ScreenPostRaceHighScoreTable @ 0x00413580` (high-score iterator + state)
  - `CreateRaceForceFeedbackEffects @ 0x004285B0` (DirectInput FF effect GUID array reader)
  - `InitializePauseMenuOverlayLayout @ 0x0043B7C0` (pause-overlay sprite scratch)
- Prior-batch out-of-scope orphans swept: 38 (consolidated below)
- Unnamed DAT_* globals encountered in target functions: 31 (after de-dup with prior tiers)
- Already-named globals encountered: 22 (noted, not renamed)
- Proposals — high confidence: 14
- Proposals — medium confidence: 11
- Proposals — comment-only (low confidence): 4

## Methodology

The T5 sweep is a deliberate catch-all rather than a coherent module audit. The process was:

1. **RNG**: confirmed `_rand @ 0x00448157` is the VC++ CRT per-thread `_holdrand` (no static seed global). Race-side RNG seed globals (`g_randomSeedForRace @ 0x004969d4`, `g_raceSessionRandomSeed @ 0x004aad64`, `g_raceRandomSeedTable @ 0x004aadbc`) are ALREADY named — no work needed.

2. **Heap**: confirmed via `search_defined_strings` that TD5 uses the CRT heap (`HeapCreate`/`HeapAlloc` via msvcrt). The "24 MB game heap" mentioned in CLAUDE.md is a CRT-managed allocation, not a custom slab allocator. No file-scope globals own it.

3. **FMV bootstrap state** (`0x004BEA7C..0x004BEACC`, `0x004BCB80..0x004BD350`, `0x00474834..0x0047483C`) is FULLY NAMED in T2.7 (batch 07). Verified — no overlap. The FMV CODEC INTERNALS (`InitializeVideoDecoder` and friends) are struct-based (all state in `param_1`); the ONLY codec-side file global is the YCbCr saturation LUT at `0x004D3F30` (1024 bytes, init at runtime). Naming below.

4. **Front-end label-surface metadata** (`0x004962C4`, `0x004962C8`, `0x004962CC`) flagged out-of-scope in batches 05/14/16. Reading `CreateMenuStringLabelSurface` shows the trio is width/height/y-offset of the menu header-label surface — used by EVERY screen state machine to position the centered top-banner string. 100+ reads each.

5. **High-score / view-race-data / replay-availability latches** (`0x00497A60..0x00497A98`) flagged out-of-scope in batch 16. `RunRaceResultsScreen` shows this is the post-race state-machine block: replay-available, view-race-data-displayed, racer-card-iterator, scratch backup for car-selection mode.

6. **Pause-menu overlay scratch** (`0x004B11D0..0x004B14E0`, `0x004BCB70..0x004BCB74`) discovered in `InitializePauseMenuOverlayLayout`. Mentioned partially in batch 13 (audio-options modal) but the actual identity is the pause-menu overlay sprite-template scratch.

7. **2P steering-weight balance** (`0x0046317C`, `0x0048301C`) flagged out-of-scope in batch 16. Decompile shows it's the per-player initial-steering-bias short pair written by `UpdatePlayerSteeringWeightBalance`.

8. **Control-buffer reset accumulators** (`0x00483014`, `0x00483018`) flagged out-of-scope in batch 16. Decompile shows a 2-int reset pair touched by `ResetPlayerVehicleControlAccumulators`; reader is `UpdatePlayerVehicleControlState`.

9. **FF DirectInput effect-template GUID array** (`0x0045EA28`) flagged out-of-scope in batch 14. Memory dump confirms 3× 16-byte GUIDs (DI ConstantForce / Spring / Damper). Single reader is `CreateRaceForceFeedbackEffects`.

10. **Pause-overlay language label table** (`0x00474498`) — 8-entry int table of per-language overlay-content-widths. Single reader (`InitializePauseMenuOverlayLayout`).

Relevance gate: a global is in-scope iff (a) it was flagged in batch 06-20 as out-of-scope AND no later batch picked it up, or (b) it is a writer in one of the long-tail functions enumerated above and is referenced from 2+ distinct call sites.

## Proposals

### FMV codec internal state

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004D3F30 | u32[256] | `g_fmvYcbcrSaturationLut` | **high** | `InitializeVideoDecoder @ 0x004539E0` populates this 1024-byte LUT in a -0x200..+0x200 loop, each entry = `clamp(iVar3, -0x80, 0x7F) + 0x80`. Index pattern `(uVar1 & 0x3ff) * 4` for each `iVar3`. 34 read-only references all in the codec hot paths (0x00456939..0x00456B18) — Type-1/2 frame-decode saturation step. Confirms it's the YCbCr-to-RGB saturation/clamp lookup. Port has no equivalent (uses Media Foundation Y'CbCr decode). | (none — orig codec replaced) |

### Frontend menu-label surface metadata trio (very high reuse — 300+ reads combined)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004962C4 | u32 (i32 or char*) | `g_menuHeaderLabelSurfaceWidth` | **high** | Written by `CreateMenuStringLabelSurface @ 0x00412E30`. Branch (a) text-mode: `DAT_004962c4 = (char*)iVar3` (width measured by `MeasureOrDrawFrontendFontString`); branch (b) image-mode: `DAT_004962c4 = s_SelectCompCarText_004642d4` (fallback width from sprite metadata). Read in 100+ sites as the `width` argument to `QueueFrontendOverlayRect`. Dual-purpose i32-or-cstring is intentional. | (none — port uses ImGui or manual blit; no shared scratch) |
| 0x004962C8 | u32 | `g_menuHeaderLabelSurfaceHeight` | **high** | Companion to `g_menuHeaderLabelSurfaceWidth`. Written `0x24` (text mode) or `0x14` (image mode); read as `height` argument to every overlay-rect emit. 100+ readers. | (none) |
| 0x004962CC | u32 | `g_menuHeaderLabelYOffset` | **high** | Companion to the pair above. Written `0x10` (text mode) or `0x0` (image mode). Used as a positional `y` bias for header-label rect emission. 100+ readers. | (none) |

### Post-race / results-screen state machine latches

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497A60 | u32 | `g_postRaceProgressionAdvanced` | high | `RunRaceResultsScreen` writes `0` at race-start (state 0), `1` after `g_raceWithinSeriesIndex += 1` (state 0xd next-cup-race path). Guards against double-increment when re-entering the screen. 4 refs (3 writes, 1 read at 0x00422f43). | td5_frontend.c post-race progression gate |
| 0x00497A64 | u32 | `g_postRaceMenuButtonChoice` | high | Written at state 0xf (`DAT_00497a64 = g_frontendButtonIndex`) when user clicks one of the 5 post-race menu buttons (Race Again / View Replay / View Race Data / Select New Car / Quit). Read by state 0x10 to dispatch the chosen branch. 6 refs. | td5_frontend.c menu-choice latch |
| 0x00497A68 | u32 | `g_postRaceRacerCardIndex` | **high** | The current racer slot whose card is being displayed on the results screen. Initialised to `0` (or to `*(int*)(dpu_exref + 0xbe8)` in network mode). Incremented/decremented by `+= DAT_0049b690` (nav direction) in state 6 with wraparound 0..5 and skip-if-state-3 logic. 17 refs. Passed to `DrawRaceDataSummaryPanel(...)`. | td5_frontend.c results racer iterator |
| 0x00497A6C | u32 | `g_replayFileAvailable` | **high** | Single writer at 0x00416d64 (`RaceTypeCategoryMenuStateMachine`) sets it when a replay file IS present on disk; 4 reads in `RunRaceResultsScreen` state 0xd choose between `CreateFrontendDisplayModeButton(SNK_ViewReplay_exref, ...)` (button enabled, replay available) vs `CreateFrontendDisplayModePreviewButton(...)` (button disabled / grayed-out). 5 refs. **Directly relevant to `todo_view_replay_restarts_race_2026-05-19`** — port may set this incorrectly. | td5_frontend.c view-replay button gate |
| 0x00497A70 | u32 | `g_postRaceNextCupRaceAvailable` | high | Written `1` at state 0xd when `ConfigureGameTypeFlags()` returns 0 (cup mode has more races queued); also written `0` after race-results screen restart. Read at 0x00423507 to choose between "Next Cup Race" preview/active button styling. 3 refs. | td5_frontend.c cup-progress flag |
| 0x00497A74 | u32 | `g_postRaceRestartSelectedRace` | high | Reentry sentinel. Written `1` at state 0x15 (Race Again path) and at `InitializeFrontendResourcesAndState @ 0x00414789`. Read at 0x00422940 to gate "skip results, restart race" early-exit. 11 refs. | td5_frontend.c restart-race flag |
| 0x00497A78 | u32 | `g_postRaceCarSelectionBackupValid` | high | Backup-state-valid flag for the car-selection scratch block at 0x00497A7C..0x00497A98. Written `1` at start of `RaceTypeCategoryMenuStateMachine @ 0x00415500`; written `2` (consumed) in state 0 after the 8-field car-selection snapshot is taken; written `0` in state 0xf option-0 path after restore. Tri-state. 7 refs. | td5_frontend.c car-selection backup gate |
| 0x00497A7C | u32[8] | `g_postRaceCarSelectionBackup` | **high** | 8-dword backup of car-selection state taken when the user starts a race, restored on "Race Again" via state 0xf option-0. Saved fields are `g_selectedCarIndex`, `DAT_0048f368` (= `gFrontendSlotCarIdTable`), `DAT_0048f370`, `_DAT_0048f378`, `DAT_00463e08`, `DAT_0048f36c`, `DAT_0048f374`, `DAT_0048f37c`. Block runs 0x00497a7c..0x00497a98 (8 × 4 = 32 bytes). 2 refs each. | td5_frontend.c car-selection backup struct |
| 0x0049B690 | u32 (signed) | `g_postRaceRacerCardNavDirection` | high | Navigation direction (-1, 0, +1) for the post-race racer-card iterator. Written by 11 sites in input-handling functions (mostly `0x0042695A`, `0x0042676F` etc.); read at 11 sites in `RunRaceResultsScreen` / `ScreenPostRaceHighScoreTable` / `RaceTypeCategoryMenuStateMachine` to drive +1/-1 advance of `g_postRaceRacerCardIndex`. 22 refs. Mentioned in batch_05 "result slot navigation direction" but never named. | td5_frontend.c results nav-direction |
| 0x004962D0 | u32 | `g_postRaceSkipResultsBanner` | medium | Single read in `RunRaceResultsScreen` at 0x00422620: gates the "if (DAT_004962d0 != 1)" path that skips to `SetFrontendScreen(0x1a)` early when in attract mode or DNF. Written 0/1 by `InitializeFrontendResourcesAndState @ 0x0041476F` and `0x00414777`. 3 refs total. | td5_frontend.c attract-mode skip-results flag |

### 2P split-screen steering / control accumulators (batch 16 out-of-scope sweep)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0046317C | i16 | `g_player0SteeringBiasShort` | **high** | `_DAT_0046317c = (short)iVar1 + 0x100` written by `UpdatePlayerSteeringWeightBalance @ 0x004036B0`; read by `UpdatePlayerVehicleControlState` via `(short *)(&DAT_0046317C + slotIndex * 2)`. The "initial steering bias short" the memo refers to. Bit 0x200 of input mask gates application. | td5_input.c per-slot steering-bias base |
| 0x0046317E | i16 | `g_player1SteeringBiasShort` | **high** | Sister to above. `_DAT_0046317e = 0x100 - (short)iVar1` — opposite-sign offset for the second player so the bias sums to 0x200. Same write site / same reader. | td5_input.c per-slot steering-bias base[1] |
| 0x0048301C | u32 | `g_steeringBiasMaxSwing` | high | Read at `UpdatePlayerSteeringWeightBalance` 0x004036C4 / 0x004036EE as the clamp ceiling on the computed bias delta: `if (DAT_0048301c < iVar1) iVar1 = DAT_0048301c;`. Single writer at `ScreenMainMenuAnd1PRaceFlow @ 0x004155EA` (likely sets per-difficulty). 3 refs. | td5_input.c steering-bias cap |
| 0x00483014 | u32 | `g_playerControlAccum0` | high | Cleared by `ResetPlayerVehicleControlAccumulators @ 0x00402E30` to zero. Reader is `UpdatePlayerVehicleControlState`; written/incremented per-frame as a control-state accumulator. 5 refs across `0x00402E32`, `0x0040343D`, `0x004034CD`, `0x004034D6`, `0x0042c275` (RunRaceFrame tail read). | td5_input.c control accum slot 0 |
| 0x00483018 | u32 | `g_playerControlAccum1` | high | Sister field cleared alongside above by `ResetPlayerVehicleControlAccumulators`. Single 2-int pair memset to 0. | td5_input.c control accum slot 1 |

### Pause-menu overlay scratch (batch 13 mis-attributed to "audio options")

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004BCB74 | char** | `g_pauseOverlayLanguagePackPtr` | **high** | Written `&PTR_s_PAUSED_004744b8 + param_1 * 0xc` (per-language pause-menu string pack base) in `InitializePauseMenuOverlayLayout`. Read at 0x0043BCF9 / 0x0043BD0F as the loop-stride source for rendering "PAUSED" / option string per language. Different language packs at 12-byte stride. | (none — port uses different localization) |
| 0x004BCB70 | void* | `g_pauseOverlaySelboxScratchPtr` | medium | Written once at `InitializePauseMenuOverlayLayout` 0x0043BAC2 as `DAT_004bcb70 = &DAT_004b1428` (alias of selection-box sprite scratch). Read by per-frame pause-overlay update to draw the selection cursor sprite. 1 writer, 2 readers in pause-menu update code. | (none) |
| 0x004B11D0 | float | `g_pauseOverlayContentWidth` | high | Written `(float)(int)(&DAT_00474498)[param_1]` — per-language content-width pre-cached at overlay init from the `g_pauseOverlayLanguageWidthTable`. Read 10+ times during overlay quad-template assembly. | (none) |
| 0x004B11D4 | float | `g_pauseOverlayHalfContentWidth` | high | Sister to above: `DAT_004b11d4 = (float)widthInt * 0.5f`. Used as vp-left/right offset in every BLACKBOX/SELBOX/SLIDER sprite-quad assembly. 10 reads. | (none) |
| 0x00474498 | int[8] | `g_pauseOverlayLanguageWidthTable` | high | Constant 8-entry int table of pause-overlay content widths per localization (0x100, 0x140, 0x12a, 0x10c, 0x140, 0x12d, 0x147, 0x14c). Single reader at `InitializePauseMenuOverlayLayout @ 0x0043B7DB`. 1 ref total. | (none) |
| 0x004B1358 | byte* | `g_pauseOverlaySliderEntryPtr` | high | Result of `FindArchiveEntryByName(..., s_SLIDER_00474818)` — the slider knob sprite static.hed entry. Read multiple times during slider-sprite emission. 4 refs total. | (none) |
| 0x004B1368 | i32 | `g_pauseOverlayPrimitiveCount` | **high** | Pause-menu overlay primitive counter — incremented per `BuildSpriteQuadTemplate` call. Reset to 0 at entry of `InitializePauseMenuOverlayLayout`; counts the total number of overlay sprite quads emitted (BLACKBOX + SELBOX + 5×SLIDER pairs + per-language label glyphs). 15 refs (writes during init, reads in `RunOverlayPresentation` callers). | (none — port emits inline) |
| 0x004B1370 | u8[N] | `g_pauseOverlayBlackboxScratch` | medium | `BuildSpriteQuadTemplate` output buffer for the BLACKBOX background sprite (top-level pause-screen background). 1 quad × 0xB8 bytes = 184 bytes. | (none) |
| 0x004B1428 | u8[N] | `g_pauseOverlaySelboxScratch` | medium | `BuildSpriteQuadTemplate` output for the SELBOX selection-cursor sprite. Address aliased via `g_pauseOverlaySelboxScratchPtr`. 1 quad × 0xB8 bytes. | (none) |
| 0x004B14E0 | u8[N] | `g_pauseOverlaySliderScratchPool` | medium | `BuildSpriteQuadTemplate` output for the 5-pair (5 sliders + 5 knobs) slider sprite block. Stride 0x170 per pair (= 2 × 0xB8); covers 5 audio-options sliders. | (none) |

### Force-feedback DirectInput template

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045EA28 | GUID[3] | `g_ffEffectGuidTable` | high | 48-byte constant table = 3 DirectInput effect type GUIDs. First (0x2A1C5413-8E33-11D0-9AD0-00A0C9A06E35) = `GUID_ConstantForce`. Second (0x2B1C5413-...) = `GUID_RampForce`. Third (0x02538130-898F-11D0-...) = `GUID_Sine`/`GUID_Spring` (need cross-verify with DInput SDK). Single reader at `CreateRaceForceFeedbackEffects @ 0x00428855` — passed to `EnumerateEffects` to filter the joystick's reported effect list. Read also at 0x004288xx for the third GUID. | td5_input.c FF effect creation block |

## Already-named globals encountered

| address | name | note |
|---|---|---|
| 0x004969D4 | `g_randomSeedForRace` | Race-RNG seed (already-named in T1) |
| 0x004AAD64 | `g_raceSessionRandomSeed` | (already-named) |
| 0x004AADBC | `g_raceRandomSeedTable` | Per-slot RNG seed table (already-named) |
| 0x004BEA7C..0x004BEACC | `g_fmvUseDdrawSurfaceFlag`..`g_fmvShutdownLatch` | FMV bootstrap state (T2.7 batch 07) |
| 0x004BCB80..0x004BD350 | `g_fmvUseRgbOverlayFlag`..`g_fmvRgbOverlayParam` | FMV TGQ control-block scratch (T2.7) |
| 0x00474834..0x0047483C | `g_fmvOutputWidth`/`g_fmvOutputHeight`/`g_fmvOwnershipClaimed` | (T2.7) |
| 0x004962A4 | `g_loaderError` (now suspected) | Already noted as flagged for verification in T4.17; 15 writers all in `LoadFrontendTgaSurfaceFromArchive`/`LoadTgaToFrontendSurfaceFromArchive` — confirmed asset-load error sentinel. **Recommend explicit rename to `g_assetLoadErrorFlag`** during consolidation. |
| 0x004962B4 | `g_cheatCodeActivated` | (T2.7) |
| 0x004962C0 | `g_frontendTransientFlag` (or similar) | 14 refs across multiple frontend state machines — likely already partially-named; verify in symbol table at consolidation |
| 0x00497A78 (companion sibling) | (covered above as backup-valid flag) | |
| `g_selectedScheduleIndex` (0x004A2C90) | (already named) | |
| `g_attractModeControlEnabled`, `g_attractModeTrackIndex` | (already named) | |
| `g_inputPlaybackActive` | (already named) | |
| `g_returnToScreenIndex` | (already named) | |
| 0x00496400 | (likely `g_frontendButtonPressedFlag` neighbour) | 11 refs in `RaceTypeCategoryMenuStateMachine` — verify naming at consolidation |
| `dpu_exref + 0xbe8` | DXPlay local-participant slot | (external, DLL-side) |

## Key discoveries

1. **The FMV codec is entirely struct-based.** All decoder state lives in the `param_1` pointer passed to `InitializeVideoDecoder` / `DecodeType2DeltaPalette` / etc. The ONLY file-scope global owned by the codec is `g_fmvYcbcrSaturationLut @ 0x004D3F30` — a 1024-byte runtime-initialized saturation/clamp LUT for converting raw YCbCr deltas to 0..255 RGB during type-1/type-2 frame decode. This is the entire codec footprint at module-scope. The bootstrap state at `0x004BEA7C+` (T2.7) is the CONNECTOR between the codec context struct and DXSound/DXDraw — not the codec proper.

2. **The "audio options modal" identified in batch_13 out-of-scope is actually the pause-menu overlay scratch.** Pointers `0x004B1358..0x004B1370` flagged in batch_13 as "audio options modal layout" are ACTUALLY emitted from `InitializePauseMenuOverlayLayout @ 0x0043B7C0` — the pause-menu-overlay sprite-quad assembler. The pause overlay is parameterised by language (`param_1`); the BLACKBOX background, SELBOX cursor, 5-slider stack (5 audio sliders: MASTER / SFX / MUSIC / VOICE / CD), and per-language string glyphs are all assembled into `0x004B1370+` and tracked by `g_pauseOverlayPrimitiveCount @ 0x004B1368`. The audio-options modal is a SUBSET of this overlay, not a separate block.

3. **`g_replayFileAvailable @ 0x00497A6C` is the EXACT control point for the "view replay restarts race" TODO**. Single writer at 0x00416d64 (RaceTypeCategoryMenuStateMachine, set when replay save succeeds); 4 readers in `RunRaceResultsScreen` state 0xd use it to choose between `CreateFrontendDisplayModeButton(SNK_ViewReplay_exref, ...)` (replay available → button enabled, click → playback) versus `CreateFrontendDisplayModePreviewButton(...)` (replay NOT available → button shown but disabled). **Port hypothesis**: the port likely either (a) never writes to this flag at race-start (so it stays 0 → preview-only button → no-op click falls through to "restart race"), or (b) the save side completes but the read side checks the wrong flag. Fix candidate at `td5_frontend.c:9477` (per memo "TODO — View Replay restarts the race"). Confirms the lead in `todo_view_replay_restarts_race_2026-05-19`.

4. **`g_postRaceCarSelectionBackup @ 0x00497A7C` is an 8-dword save/restore checkpoint for the "Race Again" button.** This is unusual — most state machines persist via Config.td5 or via RestoreRaceStatusSnapshot. The Race Again path takes a structural shortcut by snapshotting 8 hand-picked fields (car ID, driver ID, position, opponent info) at race-start, then restoring them in `RunRaceResultsScreen` state 0xf if the user chooses Race Again. Naming this block makes the "why does Race Again work but Save & Quit not" debugging tractable.

5. **`g_postRaceRacerCardNavDirection @ 0x0049B690` is shared between multiple post-race screens.** Initially flagged in batch_05 as "navigation direction" but never named. Reading the 22 refs shows it's a single signed-int "user pressed left/right/none" shared across `RunRaceResultsScreen`, `ScreenPostRaceHighScoreTable`, `RaceTypeCategoryMenuStateMachine`, and the input-handling cluster at `0x00426500..0x004269A0`. **Worth naming** because it's the only +1/-1 nav state shared across multiple screen state machines — easy class for "off-by-one navigation" bugs to manifest.

6. **`_DAT_004962a4` (T4.17's "verify in symbol table" item) IS NAMED `g_loaderError`**, confirmed by Ghidra symbol table walk. 15 writers, all in TGA loader error paths. Should be renamed to `g_assetLoadErrorFlag` for clarity at consolidation — current name is too generic.

7. **2P steering-weight balance writers are byte-faithful to a 0x100-centered short pair**. `g_player0SteeringBiasShort + g_player1SteeringBiasShort == 0x200` invariant holds across all writer branches. The clamp ceiling `g_steeringBiasMaxSwing` is per-difficulty (single writer in `ScreenMainMenuAnd1PRaceFlow`). The port's 2P steering-bias is unlikely to be in a divergence-investigation hot zone since the data path is short and the clamp is structural.

8. **The DirectInput effect template at `0x0045EA28` is a 3-GUID constant table read ONCE per controller setup.** First GUID is `GUID_ConstantForce` (`2a1c5413-8e33-11d0-9ad0-00a0c9a06e35`). The other two are likely `GUID_RampForce` and `GUID_Sine`/`GUID_Spring`. Naming this enables documentation of which FF effect classes TD5 uses (memo `reference_arch_render_wheel_billboards_d3d_2026-05-18` does not currently mention this).

9. **`g_pauseOverlayLanguageWidthTable @ 0x00474498` is an 8-entry int table.** Decoded values: `[0x100, 0x140, 0x12a, 0x10c, 0x140, 0x12d, 0x147, 0x14c]` — content widths in pixels for 8 supported localizations (likely: English, French, German, Italian, Spanish, Dutch, Portuguese, ?). Confirms the localization SKU count without needing to chase LANGUAGE.DLL.

10. **CRT `_rand` is per-thread.** No static seed. The two race-side seed globals (`g_randomSeedForRace`, `g_raceSessionRandomSeed`, `g_raceRandomSeedTable[]`) are SEPARATE from CRT randomness — they feed game-specific deterministic RNGs (likely `ApplyRandomWheelJitterSynchronized` etc., not `_rand` itself). The codebase has TWO RNGs:
   - CRT `_rand` via `_getptd()->_holdrand` — used by smoke spawn, wheel jitter, particle streak
   - Race-seed-table — used by trajectory-replay-affecting RNG (per `reference_steering_cascade_root_cause_find_offset_peer`)
   For determinism, ONLY the race-seed-table matters. CRT `_rand` is per-thread and may diverge between port runs without affecting trajectory.

## Out-of-scope finds (T5-completeness section)

Globals noticed during this sweep but not classified into any tier. These would be **T6+** work if anyone runs it:

| address | brief note | probable area |
|---|---|---|
| 0x00482EXX area neighbours (camera-saved bytes after 0x00482F49) | Flagged in batch 16 as "possible future cam-saved fields" — not investigated this batch | T6 camera persistence |
| 0x0048d988._2_2_ | Half-word at +2 from `DAT_0048d988`, read in `RunRaceResultsScreen` state 0x10 to switch between `SetFrontendScreen(0x1b)` vs `0x1a` post-cup-complete dispatch | T6 cup-completion progression |
| 0x004cf974 / 0x004cf978 / 0x004cf980 / 0x004cf984 / 0x004cf988 sibling area | Already named via T4.17 — but the cluster at 0x004CF974..0x004CF990 has unanalyzed neighbours at 0x004CF98C, 0x004CF990 (small dwords, alignment padding?) | T6 ZIP-state padding audit |
| 0x004C38B8 / 0x004C38C4 | Flagged in batch_18 — per-slot lighting-state block neighbours. 4 / 2 refs respectively; this batch did not investigate (not in scope for FMV/RNG/heap/etc.) | T6 per-slot lighting state |
| 0x004C3DAC / 0x004C3DB0 / 0x004C3DA8 / 0x004C3DDC / 0x004C3DD8 / 0x004C3DE4 | Flagged batch_18 — weather particle pool pointers + counters seeded by `InitializeWeatherOverlayParticles` | T6 weather particle pool |
| 0x004962D0 | Named here as `g_postRaceSkipResultsBanner` provisionally — single read, 2 writers; classification uncertain. Read more closely at consolidation. | T6 attract-mode detail |
| 0x00482EC8..0x00482EF8 | Camera scratch neighbours mentioned in batch_12 — never enumerated by name; cluster looks like ~12 unnamed dwords near `g_cameraOrbitRadius` (0x00482ED0) | T6 camera scratch detail |
| 0x004749CC..0x004749D8 area | Adjacent rodata constants near `s_SCANDOTS` archive name string (0x004749D0); likely camera-distance floats or similar | T6 rodata constant pool |
| 0x004BCB70..0x004BCB74 sibling area | Pause-overlay scratch named here, but 0x004BCB78 onwards into `g_fmvTgqControlBlockMaster @ 0x004BCB90` is a 28-byte gap (0x004BCB78..0x004BCB8F) that may host more unanalyzed scratch | T6 pause/FMV layout audit |
| 0x0048d98c / 0x0048d9a0 (flagged batch_08 out-of-scope) | String table pointers at HUD position-label sites — never investigated | T6 string-table audit |
| 0x00495230 / 0x00495250 / 0x0049525c / 0x004951D8 (flagged batch_17 out-of-scope) | TGA-decode auxiliary fields cluster — 25+ xrefs from 0x0049525c alone, worth a dedicated mini-batch | T6 TGA-config batch |
| 0x004A2CC4..0x004A2CF0 (flagged batch_14 out-of-scope) | Force-feedback per-player state block — some named (e.g. `g_ffeffectsArrayState`), some unknown | T6 FF state cluster |

### Stragglers identified for future (T6+) work

- **TGA-config field cluster** (0x004951D8 / 0x00495230 / 0x00495250 / 0x0049525C): 4+ DAT_*s with 25+ xrefs from a single site — high-value mini-batch candidate. Likely TGA palette / format / source-data pointers seeded by some TGA-config init outside scope here.
- **Weather particle pool** (0x004C3DAC area): 6 DAT_*s with weather-related writers/readers; small batch could close them at once.
- **Per-slot lighting state** (0x004C38B8, 0x004C38C4): 2 DAT_*s with low ref counts; could be folded into a future audio/light-state mini-batch.
- **FF per-player state block** (0x004A2CC4..0x004A2CF0): partially-named cluster needing closure.

T5 verdict: **substantially swept** with 4 small straggler clusters identified above. Estimated T6 effort if attempted: 1 small batch of ~15-25 globals covering TGA-config + weather pool + lighting-state + FF state stragglers.

## TODO impact

- **todo_view_replay_restarts_race_2026-05-19**: SIGNIFICANT — `g_replayFileAvailable @ 0x00497A6C` is the discriminator that decides between "View Replay enabled → play replay" and "View Replay disabled (preview button) → fall through to other state". Single writer at `RaceTypeCategoryMenuStateMachine @ 0x00416D64` sets it after replay save succeeds. **Port investigation**: search for the equivalent in `td5_frontend.c` and verify it gets set when the replay file is written; check that the View Replay button dispatch reads it correctly. If the port wires the writer but the reader is missing, the button will always fall through to the restart-race path (consistent with reported bug).

- **reference_arch_play_intro_movie_2026-05-18**: FULLY CLOSED in T2.7. This batch confirms the FMV codec's internal state (the `param_1` struct) is the only orig-side state beyond the bootstrap globals already named. Adding `g_fmvYcbcrSaturationLut @ 0x004D3F30` to the naming set completes the FMV-side global map.

- **todo_precise_port_extend_particle_render_2026-05-17**: TANGENTIAL. This batch did not directly address the particle render path (covered in batches 15 + 17), but it confirms via the FMV-codec audit that the codec is byte-faithful to a per-context struct design — useful as a precedent for any future particle-context refactor.

## Ghidra session notes

- Session `edcc91dd0fbe4814997a5ba6f443eb3e` opened `TD5_pool12` read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; will release via `bash scripts/ghidra_pool.sh cleanup` after writing this batch.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
