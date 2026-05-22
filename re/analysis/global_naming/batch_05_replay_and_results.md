---
batch: 05
area: replay_and_results
tier: T1.5
target_todos: [todo_view_replay_restarts_race_2026-05-19, todo_view_race_data_broken_2026-05-19]
ghidra_session: e41a7aeaddd341d78694384ed9906fbf
analyzed_addresses: 0x00422480, 0x0042aa10, 0x004269d0
agent: Claude Haiku 4.5
date: 2026-05-20
---

# Globals enumeration — Replay save/playback and Race Results state machine

## Summary

- Functions analyzed: 3
  - RunRaceResultsScreen() @ 0x00422480 (state machine hub for post-race UI)
  - InitializeRaceSession() @ 0x0042aa10 (race bootstrap; writes g_inputPlaybackActive)
  - ScreenLocalizationInit() @ 0x004269d0 (frontend initialization; clears playback flag)
- Unnamed DAT_* globals encountered: 17 (after de-dup)
- Already-named globals encountered: 2 (g_attractModeControlEnabled @ 0x004a2c8c, g_inputPlaybackActive @ 0x00466e9c)
- Proposals — high confidence: 10
- Proposals — medium confidence: 5
- Proposals — comment-only (low confidence): 2

## Methodology

Entry points: RunRaceResultsScreen (the post-race results dispatcher at 0x00422480) and its callees; then backward-traced to replay buffer initialization sites.

Key gate: The state machine at case 0xF (button dispatch) where:
- Case 0 = "Race Again" -> re-run race
- Case 1 = "View Replay" -> set g_attractModeControlEnabled = 2, g_inputPlaybackActive = 1, transition to race init
- Case 2 = "View Race Data" -> self-jump screen (re-enter state 0)
- Case 3 = "Select Car"
- Case 4 = "Quit"

Replay system flow:
1. Race starts -> InitializeRaceSession() checks g_inputPlaybackActive (0 = record, 1 = playback)
2. If 0, calls DXInput::WriteOpen() (save mode); if 1, calls DXInput::ReadOpen() (playback mode)
3. View Replay button sets g_inputPlaybackActive = 1 and transitions back to race init
4. State-3 gate (early-exit mechanism) checks actor runtime state; if no live race data, exits back to menu

Relevance gate: All globals traced here are either:
- Directly written in the three target functions
- Read within those functions to gate state transitions
- Accessed by referenced callees

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00497a78 | u32 | g_raceReplayStateFlag | high | Written as 1, 2, 0 in state 0; controls backup lifecycle | td5_input.c:157 s_replay_mode_flag |
| 0x00497a7c | u32 | g_savedCarIndex | high | Stores g_selectedCarIndex on backup; restored on Race Again | td5_frontend.c:9475 |
| 0x00497a80 | u32 | g_savedSlotCarIdTable | high | Saves car ID table snapshot during backup | td5_frontend.c:9475 |
| 0x00497a84 | u32 | g_savedSlotPositionTable | high | Saves position table for determinism | td5_frontend.c:9475 |
| 0x00497a88 | u32 | g_savedTrackModeFlags | high | Saves race mode/type flags | td5_frontend.c:9475 |
| 0x00497a8c | u32 | g_savedDragRaceState | high | Saves drag race mode state (game type 7) | td5_frontend.c:9475 |
| 0x00497a90 | u32 | g_savedDragRaceParam1 | high | Drag race parameter 1 | td5_frontend.c:9475 |
| 0x00497a94 | u32 | g_savedDragRaceParam2 | high | Drag race parameter 2 | td5_frontend.c:9475 |
| 0x00497a98 | u32 | g_savedDragRaceParam3 | high | Drag race parameter 3 | td5_frontend.c:9475 |
| 0x00497a60 | u32 | g_raceResultsSeriesAdvanceFlag | high | Gates series progression; set after first Race Again in cup mode | (none) |
| 0x00497a68 | u32 | g_selectedRaceResultIndex | high | Tracks viewed result slot (0-5) | td5_frontend.c implicit |
| 0x00497a70 | u32 | g_cupModeNextRaceAvailable | med | Set by ConfigureGameTypeFlags(); gates Next Cup Race button | td5_frontend.c implicit |
| 0x00497a74 | u32 | g_raceResultsStateGateFlag | med | Checked to gate state 3 exit; reset on state entry | td5_frontend.c:9188 |
| 0x00497a64 | u32 | g_lastSelectedResultButton | high | Stores button index (0-4) from state 0xf dispatch | td5_frontend.c:9484-9497 |
| 0x0049636c | u32 | g_raceResultsLoadedFlag | med | Checked as early-entry gate for load/restore logic | (none) |
| 0x004962d0 | u32 | g_raceSinglePlayerCompanionBitfield | med | Gates display mode for companion slot races | td5_frontend.c implicit |
| 0x0048d988 | u32 | g_raceActorRuntimeValid | high | Checked at state-3 early-exit gate; if invalid, bounces user | td5_frontend.c:9188 |

## Key discoveries

1. **Replay save is triggered at race START, not race END:** The backup at DAT_00497a78 == 1 stores car lineup + game flags BEFORE race runs. If the replay file save happens during InitializeRaceSession() when g_inputPlaybackActive == 0, the port must have a corresponding writer that matches the read site's format.

2. **State-3 early-exit gate is data-driven, not flag-driven:** Check at 0x00422506 evaluates g_raceActorRuntimeValid. Gate fires if NO human players exist (all slots are AI/empty). On first entry, actor slots are populated; on "View Race Data" re-entry, if actor table was cleared, gate fires and bounces user back.

3. **Race Results state machine is strictly linear, state-gated:** States follow a fixed sequence with explicit guards. State 0 gates on load-flag and actor-data validity. Re-entry requires full state-0 re-init.

4. **Backup state variables form a coherent "save snapshot":** Block DAT_00497a7c..DAT_00497a98 (8 dwords) is a structured backup copied atomically on DAT_00497a78 = 1, restored on 0. Suggests a single BackupGameState function.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004962cc, 0x004962c4, 0x004962c8 | Animation frame counters for panel slide | UI/Animation layer |
| 0x00496408 | Network multiplayer flag | Network subsystem (T2) |
| 0x00496358, 0x0049628c | Frontend surface pointers | Rendering/HID layer |
| 0x0049b690 | Result slot navigation direction | Navigation state (UI layer) |

## TODO impact

**todo_view_replay_restarts_race_2026-05-19:**
- Investigation surfaces MISSING WRITER side. Original saves replay data at race-start (confirmed in InitializeRaceSession where g_inputPlaybackActive is checked). Port must identify original replay save location and implement matching writer in td5_input.c.
- Closing path: Full audit of td5_input.c save/load, implement missing writer, verify round-trip replay recording.

**todo_view_race_data_broken_2026-05-19:**
- Root cause is state-3 early-exit gate check at 0x00422506. Gate fires if race-actor runtime is invalid/unpopulated. On "View Race Data" re-entry, if InitializeFrontendDisplayModeState() clears actor state, gate will fire and bounce user.
- Closing path: Ensure actor-runtime snapshot persists across View Race Data self-jump, OR gate state-3 exit on a separate "results_displayed_once" flag.

