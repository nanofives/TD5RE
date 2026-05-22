---
batch: 04
area: checkpoint_config
tier: T1
target_todos: [todo-checkpoint-no-time-added-2026-05-19, todo-circuit-no-checkpoint-timer-2026-05-19]
ghidra_session: 2299832a3c664bd18d5026f6c338db1d (closed)
analyzed_addresses: 0x40A530, 0x409E80, 0x42F140, 0x42FD70, 0x4076C0
agent: aac60336462cb5503 (Explore type, summary transcribed)
date: 2026-05-20
status: SUMMARY TRANSCRIBED — agent could not Write directly. Full per-global table captured from summary.
---

# Globals enumeration — checkpoint config (timer + circuit gate)

## Summary

- Functions analyzed: 8
- Unnamed DAT_* globals proposed: 5 (all high confidence)
- Already-named globals encountered: 25+
- Proposals — high confidence: 5
- Proposals — medium confidence: 0
- Proposals — comment-only: 0

## Functions analyzed

| address | name | role |
|---|---|---|
| 0x40A530 | `AdjustCheckpointTimersByDifficulty` | Difficulty scaling at race-init |
| 0x409E80 | `CheckRaceCompletionState` | Circuit vs P2P dispatch + finish detection |
| 0x42F140 | `InitializeRaceVehicleRuntime` | Caller of AdjustCheckpointTimersByDifficulty |
| 0x42FD70 | `RemapCheckpointOrderForTrackDirection` | Forward/reverse track checkpoint remap |
| 0x4076C0 | `ProcessActorForwardCheckpointPass` | Per-tick checkpoint detection |

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00483954 | u32 | `g_currentCheckpointStripIndex` | high | Written @ 0x42FD51 in RemapCheckpointOrderForTrackDirection; read @ 0x406F77, 0x4070AE, 0x4076DD; indexes into `g_trackStripRecords` for checkpoint geometry | (consider adding) |
| 0x004aedb0 | u32[16] | `gCheckpointOrderRemapTable` | high | Array of checkpoint order indices used when track direction changes (forward vs reverse). Read in RemapCheckpointOrderForTrackDirection @ 0x42FD70. Paired with `gTrackDirectionSwitchFlag` to switch orderings | `td5_track.c` (none yet) |
| 0x004aee00 | u32 | `gTrackDirectionSwitchFlag` | high | Discriminates between two sets of checkpoint order swaps in `RemapCheckpointOrderForTrackDirection`. When == -1, uses one pair of indices; otherwise uses alternate pair. Determines track reversal layout | `td5_track.c` (none yet) |
| 0x00466004 | u32 | `gCheckpointTimersUserToggle` | high | Written in `ScreenGameOptions` @ 0x420035 and 0x40FC43; read in game logic to enable/disable checkpoint timer display and time-bonus mechanics. Stored to `Config.td5` in `LoadPackedConfigTd5` | `g_td5.ini.checkpoint_timers` (already exists) |
| 0x00466008 | u32 | `gUnknownCheckpointToggle_B` | high* | Adjacent to `gCheckpointTimersUserToggle`; written at same locations (0x420047, 0x40FC43); semantics unknown. Possibly lap-counter display, P2P timer display, or sound-effect toggle. Needs deeper analysis before final name | (unknown) — marked high but with `_PROVISIONAL` suffix until semantics confirmed |

## Already-named globals (encountered, for cross-reference)

| address | name |
|---|---|
| 0x463210 | `gRaceDifficultyTier` |
| 0x4aed88 | `g_raceCheckpointTablePtr` |
| 0x466e8c | `gCircuitLapCount` |
| 0x466e94 | `gTrackIsCircuit` |
| 0x4aaf6c | `g_selectedGameType` |
| 0x4aee20 | `g_trackEnvironmentConfig` (= LEVELINF) |
| 0x4aad68 | `g_trackStartSpanIndex` |
| 0x4afc3c | `gActorTrackSubProgress` |
| 0x466894 | `gScheduleToPoolIndex` |

## Key discoveries

1. **Circuit vs P2P gate location**: `gTrackIsCircuit` @ 0x466e94 controls dispatch in `CheckRaceCompletionState` @ 0x409E80. When `circuit==0 && selected_game_type != 0`, P2P checkpoint logic runs; otherwise circuit lap logic runs.

2. **Difficulty source**: `gRaceDifficultyTier` @ 0x463210 is read by `AdjustCheckpointTimersByDifficulty` to scale base checkpoint timers (0.9× for tier 0 easy, 0.85× for tier 1 normal, etc.) from the hardcoded table @ 0x46CBB0.

3. **LEVELINF +0x08 gate**: `g_trackEnvironmentConfig` @ 0x4aee20 +0x08 field determines P2P support. Read at 0x0040A04D — `MOV EAX, [ECX+0x8]; TEST EAX,EAX; JZ 0x0040A1F7` — when zero, P2P checkpoint code skips entirely.

4. **Per-actor checkpoint state**: `g_actorRuntimeState` has per-slot sub-progress arrays (read via `gActorTrackSubProgress` @ 0x4afc3c). Lap counter at actor offset −0x24 (char), checkpoint counter at offset −0x32 (short).

5. **Time-bonus propagation**: P2P branch @ 0x0040A047–0x0040A1F7 reads per-checkpoint time values from the remap table, accumulates into `actor.field_0x344` (time accrual). Final bonus added in `CheckRaceCompletionState` at `*(short *)(pbVar6 + -0x3a) = *(short *)(pbVar6 + -0x3a) + [bonus]`.

6. **Two adjacent user toggles**: `DAT_00466004` and `DAT_00466008` — written at same UI callsite; the second one's semantics are unknown.

## TODO impact

- **todo-checkpoint-no-time-added-2026-05-19**: ✅ **Closes** — the time-bonus propagation DOES exist in orig at `CheckRaceCompletionState` @ 0x409E80. The gate is at the P2P-branch entry (0x40A04D, LEVELINF +0x08 == 0 skips). If the port's HUD doesn't show added time, either the port's `s_active_checkpoint.checkpoint_count == 0` gate is silently firing (per the original TODO suspect) OR the LEVELINF +0x08 read on the port side returns 0 wrongly. Port-side action: verify `s_levelinf_checkpoint_config` is non-zero on the actual test track.

- **todo-circuit-no-checkpoint-timer-2026-05-19**: ✅ **Partially closes** — `AdjustCheckpointTimersByDifficulty` @ 0x40A530 is called unconditionally from `InitializeRaceVehicleRuntime` for all actors and all modes. Suggests adding the gate at port `td5_game.c:2081` proposed in the TODO: `if (g_td5.checkpoint_timers_enabled && g_td5.track_type != TD5_TRACK_CIRCUIT)`. Original behavior is "scale on circuit too, but the HUD render doesn't show the timer" — so the port may have an additional bug where the HUD renders the timer on circuit despite the field being scaled. Two-part fix recommended: gate the scaler AND gate the HUD draw.

## Out-of-scope finds

None beyond already-named globals listed above. Area is tight; no spillover into adjacent T1 areas.

## Reproducibility note

Agent ran ~20 min via Explore type. Closed Ghidra session cleanly. Summary contained the full table — no re-run needed.
