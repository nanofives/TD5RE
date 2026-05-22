---
tier: T5
date: 2026-05-20
batches: [21, 22, 23, 24, 25]
status: ready_for_consolidation
total_proposals: 169
---

# T5 Global Naming Sweep — Long Tail (debug, attract, replay, network, misc)

## Headline stats

| batch | area | functions | globals | high | med | low |
|---|---|---|---|---|---|---|
| 21 | debug_overlays_diagnostics | 19 | 35 | 22 | 8 | 5 |
| 22 | attract_mode_intro | 20 | 18 | 12 | 5 | 1 |
| 23 | replay_record_telemetry | 7 | 45 | 30 | 15 | 0 |
| 24 | network_multiplayer (DXPlay) | 9 | 42 | 27 | 12 | 3 |
| 25 | long_tail_misc + orphan sweep | 12 | 29 | 14 | 11 | 4 |
| **TOTAL** | | ~67 functions | **169** | **105** | **51** | **13** |

## Major TODO impacts

### TODOs ADVANCED via T5 findings

1. **todo_view_replay_restarts_race_2026-05-19** — **STRATEGY PIVOT** (T5.23 + T5.25 + T5.22)
   - T5.25 found `g_replayFileAvailable @ 0x00497A6C` — single writer at `0x00416d64` (post-save), 4 readers in `RunRaceResultsScreen` state 0xd choose enabled-vs-disabled "View Replay" button. **Already wired correctly in port via the discriminator.**
   - T5.23 found there is NO orig-side write-path state to port. Orig delegates ALL replay record/playback to the closed-source **DXInput middleware** (M2DX) — `WriteOpen`/`ReadOpen` are external imports at `EXTERNAL:00000024/23`. The main EXE owns no file handle, no frame counter, no buffer pointer, no record-mode sentinel. Only input is the track-pool index.
   - **Fix recipe**: port must build its own replay primitives in `td5_input.c` (open/write/read/close + ring buffer) to mimic the M2DX contract. There are no missing globals to wire; this is a port-side construction task.
   - T5.22 side-finding: **attract-mode race playback uses `g_replayModeFlag=1`** (same path as "View Replay" menu). Fixing the port-side replay primitives is a prerequisite for attract mode too.

2. **todo_ini_refactor_2026-05-19** — **INDIRECTLY ADVANCED** by T5.21
   - Naming the `gCircuitLapCount` / `gCheckpointTimers` / `gTrafficActorsEnabled` / `gDynamicsActorsEnabled` / `g3dCollisionsActorsEnabled` / `gFoggingActorsEnabled` / `gSpeedReadoutUnits` ConfigShadow block (7 entries at `0x004cXXXX`) gives an explicit `[GameOptions]` ↔ globals map.

### Existing-name corrections discovered

3. **`g_attractModeIdleCounter @ 0x0048f2fc` is MISNAMED** (T5.22)
   - **True semantic**: extras-gallery cross-fade phase counter. Should be `g_extrasGalleryCrossFadePhase`.
   - "Attract idle" usage in `RunFrontendDisplayLoop`'s `< -0xF` gate is a secondary consumer.
   - **Explains** why orig's attract trigger requires visiting extras gallery — the counter only naturally drifts below -15 from gallery's per-frame decrement.

4. **`g_attractModeControlEnabled @ 0x004a2c8c` is MISNAMED** (T5.22)
   - **True semantic**: 3-state `g_frontendBootDispatchMode` discriminator inside `ScreenLocalizationInit` (0=first-boot, 1=main-menu, 2=language-select-then-main-menu). 6 writers across 5 functions, none specifically attract-mode.

5. **"3D Collisions" toggle is MISLABELED** (T5.21)
   - Actually flips `g_cameraMode` between two camera-collision modes (ON→0, OFF→1), NOT a collision-detection enable.

6. **"Dynamics" toggle is MISLABELED** (T5.21)
   - Actually drives `gDifficultyEasy` flag, not a physics-quality switch.

## Major structural reveals

### Port-side architectural divergence — DXPTYPE protocol (T5.24)
**`td5_net.c` is wire-incompatible with orig `TD5_d3d.exe`**:
- Orig: 3 transport types (1/2/4); everything else is a sub-opcode (0x10..0x18, 0x7F) nested in type-1 payload.
- Port: flattens these into 13 distinct handlers.
- **Consequence**: `td5re.exe` peers cannot interop with `TD5_d3d.exe` peers.
- Port also lacks local-echo ring (orig has 8-entry stride-0xc4 ring fed by `QueueFrontendNetworkMessage @ 0x00418c60`) → port's local chat does not echo.
- Port lacks host-migration path (orig's `g_lobbyHostBootstrapDone` auto-resets on host promotion).
- Driver-name stride: orig 0x3c vs port 64 (truncation needed for wire-compat).
- **Recommend new memory entry**: `reference_arch_dxptype_protocol_divergence_2026-05-20`.

### CupData.td5 snapshot format fully reconstructed (T5.23)
0x32a6-byte buffer at `0x00490bac`:
- XOR-encrypted with literal key `"Steve Snake says: No Cheating!"` + 0x80
- CRC32-validated
- Partitioned into: header bytes + frontend-selections + race-results + actor-runtime + slot-state blocks
- Plus per-schedule **high-score table** at `0x004643bc` (5 entries × 32B per schedule, stride 0xa4) — in-binary writable initialized data.

### CPU detection / benchmark broadcast (T5.21)
- `g_cpuVendorString/VersionInfo/FeatureFlags/MhzEstimate` (cluster at `0x00497AA0..0x00497AC8`) is **dual-purpose**: printed on benchmark TGA AND broadcast over DXPlay to network peers (lobby shows opponent CPU model + clock speed).
- `NoOpHookStub @ 0x00418450` is the dev-build profiler instrumentation slot — single `ret` called from frontend/race/actor/texture-page paths. Natural port-side hook point for profiling.

### Race-results slot table (T5.23)
- `0x0048d988..0x0048da00`: 6 slots × 20 bytes (stride 5*4)
- Per-slot: `+0x0` (byte) valid + slot-index, `+0x4` (dword) lap_time_total, `+0x8` (dword) points/bonus, `+0xC` (short) top_speed, `+0xE` (short) result4
- Flagged for **struct promotion**: `TD5_RaceFinalResultsSlot[6]`

### Frontend cross-fade (T5.22)
- 64-line moving gradient band advancing 2 lines/frame (~4.5s at 60Hz)
- `g_frontendDitherRampLut @ 0x00494bc0` (32×32 bytes, 30 xrefs) is the per-channel 5-bit ramp LUT
- 5-slide extras-gallery surface array, fade-effect 5-channel state cluster, language-select screen surface, startup dialog panel — all named

### FMV codec is entirely struct-based (T5.25)
- ONLY file-scope codec global is `g_fmvYcbcrSaturationLut @ 0x004D3F30` (1024-byte LUT init by `InitializeVideoDecoder`)
- Everything else is allocated on the FMV bootstrap struct (T2.7 already named the cluster at 0x004BEXX)

### Lockstep is opaque to the binary (T5.24)
- Orig has **NO** frame-counter / barrier / generation-counter in the data segment — all lives inside M2DX!DXPlay::HandlePadHost/HandlePadClient.
- Port's `s_sync_generation` / `s_evt_sync` / `s_pending_ack_count` machinery has no orig data-segment counterpart.
- Max players = 6, hard-coded (3 distinct iteration patterns confirm).

## Out-of-scope finds (T5-completeness)

T5.25 explicitly identified 4 small straggler clusters for potential T6+ work:
- TGA-config field cluster at `0x00495xxx` (25+ xrefs)
- Weather particle pool at `0x004C3DAC` area
- Per-slot lighting state at `0x004C38B8/C4`
- FF per-player state block at `0x004A2CC4`

These were NOT named due to insufficient writer+reader evidence within the time-box.

## Consolidation flags

### Existing-name corrections to apply at consolidation session
- `g_attractModeIdleCounter @ 0x0048f2fc` → `g_extrasGalleryCrossFadePhase` (T5.22)
- `g_attractModeControlEnabled @ 0x004a2c8c` → `g_frontendBootDispatchMode` (T5.22)
- "3D Collisions" / "Dynamics" toggle wiring comments (T5.21)

### Struct-promotion candidates
- `TD5_RaceFinalResultsSlot[6]` at `0x0048d988` (T5.23)
- `TD5_GameOptionsShadow` (10 entries, T5.21)
- `TD5_CpuInfo` broadcast struct (T5.21)
- `TD5_LobbyChatRingEntry[8]` stride 0xc4 (T5.24)

### Structural divergence memo to write at consolidation
- `reference_arch_dxptype_protocol_divergence_2026-05-20` (T5.24)

## Cumulative status (T1 + T2 + T3 + T4 + T5)

| tier | globals | high | med | low | new TODO closes | major reveals |
|---|---|---|---|---|---|---|
| T1 | 37 | 29 | 7 | 1 | 6 TODOs root-caused | bit-28 input pattern |
| T2 | 126 | 85 | 30 | 11 | cascade focal point named | RS_TRACK_OFFSET_BIAS |
| T3 | 136 | 89 | 33 | 14 | smoke-render CLOSED + shadow partial | g_raceCountdownTimer rename |
| T4 | 119 | 90 | 24 | 6 | Newcastle wall hypotheses | AI rubber-band formula; XOR keys |
| T5 | 169 | 105 | 51 | 13 | View-Replay strategy pivot; ConfigShadow named | DXPTYPE divergence; CupData format; lockstep opaque |
| **TOTAL** | **587** | **398** | **145** | **44** | | |

## Files

`re/analysis/global_naming/`:
- BATCH_TEMPLATE.md
- T1_SUMMARY.md, T2_SUMMARY.md, T3_SUMMARY.md, T4_SUMMARY.md, T5_SUMMARY.md
- inventory_t1.csv
- batch_01..25_*.md (25 batches total — sweep complete)

## Status

**T5 COMPLETE — entire global-naming sweep done.** All 5 tiers ready for the writable consolidation session.

Consolidation scope:
1. Acquire master Ghidra project writable (NOT pool)
2. Apply `decomp_global_rename` for ~398 high-confidence proposals
3. Apply with `_PROVISIONAL` suffix for ~145 medium proposals
4. Add Ghidra comments with evidence for all ~44 low-confidence proposals
5. Apply existing-name corrections flagged in T2/T3/T4/T5
6. Promote 4 struct-array families (`gVehicleTuningTable`, `gVehiclePhysicsTable`, `g_actorRuntimeState`, `TD5_RaceFinalResultsSlot`) → eliminates 38+ fake DAT_ labels
7. Promote additional struct candidates from T2/T4/T5 (`TD5_ZipStreamState`, `TD5_HudViewLayout[3]`, `TD5_CpuInfo`, `TD5_GameOptionsShadow`, `TD5_LobbyChatRingEntry[8]`)
8. Generate `td5mod/src/td5re/td5_orig_globals.h` with `#define DAT_004XXXXX g_name` pairs
9. Write structural divergence memo `reference_arch_dxptype_protocol_divergence_2026-05-20`
10. `/ghidra-sync` to refresh pool clones
11. Update tracking memory with "renames applied" status

Estimated 40-60 min wall-clock for full T1-T5 consolidation.
