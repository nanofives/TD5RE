---
batch: 23
area: replay_record_telemetry
tier: T5
target_todos: [todo_view_replay_restarts_race_2026-05-19, todo_view_race_data_broken_2026-05-19]
ghidra_session: 4cf74668f13144c7a82d91487ad991bd
analyzed_addresses: 0x0042aa10, 0x0042b29c, 0x0048d988, 0x00416d64
agent: Claude Opus 4.7
date: 2026-05-20
---

# Globals enumeration — Replay record (write side) + race-results slot table + telemetry

## Summary

- Functions analyzed: 7
  - InitializeRaceSession @ 0x0042aa10 (race bootstrap, calls DXInput::Write/ReadOpen)
  - ScreenLocalizationInit @ 0x004269d0 (writes g_replayFileAvailable=1 in attract-mode case)
  - RaceTypeCategoryMenuStateMachine @ 0x004168b0 (writes g_replayFileAvailable=0 in Continue Cup case)
  - ResetRaceResultsTable @ 0x0040a880 + SortRaceResultsByPrimaryMetricAsc @ 0x0040aad0 (race-results slot table operators)
  - SerializeRaceStatusSnapshot @ 0x00411120 + RestoreRaceStatusSnapshot @ 0x004112c0 (cup-data snapshot pair)
  - WriteCupData @ 0x004114f0 + LoadContinueCupData @ 0x00411590 + ValidateCupDataChecksum @ 0x00411630 (CupData.td5 I/O)
  - ScreenPostRaceNameEntry @ 0x00413bc0 (high-score table editor / persistence)
- Unnamed DAT_* globals encountered: ~50 (after de-dup)
- Already-named globals encountered: 8 (g_inputPlaybackActive, g_raceReplayStateFlag, g_replayFileAvailable, g_postRaceCarSelectionBackup, g_selectedScheduleIndex, g_raceWithinSeriesIndex, g_attractModeTrackIndex, g_selectedGameType — and 6 frontend backup fields from T1.5)
- Proposals — high confidence: 30
- Proposals — medium confidence: 15
- Proposals — comment-only (low confidence): 0
- Two struct-promotion candidates flagged for follow-up consolidation session (see Out-of-scope)

## Methodology

Three narrowed clusters per the batch-23 prompt:

- **Cluster A** — Replay write path: enter `InitializeRaceSession @ 0x0042aa10`; walk into `DXInput::WriteOpen` (one level); enumerate file-handle / frame-counter / record-mode sentinels.
- **Cluster B** — Race-results slot table at 0x0048d988..0x0048da00 (per prior agent observation: stride 20, 6 slots). Propose ONE provisional name + struct-promotion note.
- **Cluster C** — Time-trial / best-time / ghost-car persistence via string search for ".tt", "BESTTIME", "TIMETRIAL", "ghost".

Already-named anchors from prior tiers (reused, NOT re-named):

- `g_inputPlaybackActive @ 0x00466e9c` (T1.5)
- `g_raceReplayStateFlag @ 0x00497a78` (T1.5)
- `g_replayFileAvailable @ 0x00497A6C` (T5.25)
- `g_postRaceCarSelectionBackup` (T5.25)

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048d988 | struct[6]×20B (120B total) | `g_raceFinalResultsSlotTable_PROVISIONAL` | high | ResetRaceResultsTable @ 0x0040a880 iterates 6 entries stride 20 with index-stamp at +1; SortRaceResultsByPrimaryMetricAsc @ 0x0040aad0 reads field +0 metric and writes short rank at +2 | (none — needs struct-promotion follow-up; see "Out-of-scope") |
| 0x0048d990 | u32 | `g_raceFinalResultsSlot0_primaryMetric` | high | First metric field of slot 0 in g_raceFinalResultsSlotTable_PROVISIONAL; sorted by (metric*100)/30; aliased label for cross-ref clarity | (none) |
| 0x004ae279 | byte[6] | `g_raceResultsSortScratchBuffer` | high | Sibling 6-byte working buffer used by SortRaceResultsByPrimaryMetricAsc bubble-sort exchange; companion to g_raceOrderTable | (none) |
| 0x00465fec | u32 | `g_persistedMasterVolumePercent` | high | Loaded/saved in ScreenLocalizationInit attract-mode branch; DXSound::GetVolume scaled to 0..100 stepped by 10; written back via DXSound::SetVolume | td5_save.c (Config.td5 volume field) |
| 0x00465ff0 | u32 | `g_persistedCdVolumePercent` | high | Same pattern as master volume but for CD audio (DXSound::CDGetVolume / CDSetVolume) | td5_save.c (Config.td5 CD-volume field) |
| 0x00497a6c | u32 | `g_replayFileAvailable` | already-named (T5.25) | reset to 0 at 0x00416d64 in RaceTypeCategoryMenuStateMachine case 2 (Continue Cup); set to 1 at 0x00427192 in ScreenLocalizationInit attract-mode==2 branch (post-View-Replay); read by RunRaceResultsScreen | td5_input.c (replay-write-success flag) |
| 0x00490bac | u8 | `g_snapshotGameType` | high | Written at start of SerializeRaceStatusSnapshot @ 0x00411120 from g_selectedGameType; first byte of snapshot header (used as CRC base) | (none) |
| 0x00490bad | u8 | `g_snapshotRaceWithinSeries` | high | Snapshot field copied from g_raceWithinSeriesIndex | (none) |
| 0x00490bae | u8 | `g_snapshotScheduleIndex` | high | Copied from g_selectedScheduleIndex | (none) |
| 0x00490baf | u8 | `g_snapshotAttractModeTrackIndex` | high | Copied from g_attractModeTrackIndex | (none) |
| 0x00490bb0 | u8[4] | `g_snapshotRaceModeFlags` | high | Packed 4-byte flag block: +0 specialEncounterEnabled, +1 selectedGameType-mirror, +2 wantedModeEnabled, +3 raceDifficultyTier | (none) |
| 0x00490bb4 | u8[4] | `g_snapshotEnvironmentFlags` | high | Packed 4-byte: +0 specialEncounterType, +1 trafficActorsEnabled, +2 gSpecialEncounterEnabled, +3 circuitLapCount | (none) |
| 0x00490bb8 | u32 | `g_snapshotCrc32` | high | Final ~uVar1 (~CRC32) written at end of SerializeRaceStatusSnapshot; computed via DAT_00475160 lookup over 0x32a6 bytes starting at DAT_00490bac | td5_save.c CRC32 verification slot |
| 0x00490bbc | u32[30] | `g_snapshotFrontendStateBlock` | high | 30 dwords copied from DAT_00497250 (frontend selections region) | (none) |
| 0x00490c34 | u32[30] | `g_snapshotRaceResultsBlock` | high | 30 dwords copied from g_raceFinalResultsSlotTable_PROVISIONAL @ 0x0048d988 (full 6×20B = 120B) | (none) |
| 0x00490cac | u32[3164] | `g_snapshotActorRuntimeBlock` | high | 0xc5c dwords (12656B) copied from g_actorRuntimeState; the bulk of the snapshot payload | (none) |
| 0x00493e1c | struct[6] | `g_snapshotSlotStateTable` | high | 6-entry mirror of gRaceSlotStateTable copied byte-by-byte | (none) |
| 0x00493e34 | u32 | `g_snapshotMiscState0` | med | Copied from _DAT_0048f30c (overlap warning); semantic unknown — small dword | (none) |
| 0x00493e38 | u32 | `g_snapshotMiscState1` | med | Copied from DAT_0048f310 | (none) |
| 0x00493e3c | u32 | `g_snapshotMiscState2` | med | Copied from DAT_0048f314 | (none) |
| 0x00493e40 | u32 | `g_snapshotMiscState3` | med | Copied from DAT_0048f318 | (none) |
| 0x00493e42 | u16 | `g_snapshotMiscState4` | med | Copied from DAT_0048f31a (short overlap with prior dword) | (none) |
| 0x00493e43 | u32 | `g_snapshotMiscState5` | med | Copied from _DAT_0048f324 | (none) |
| 0x00493e47 | u8 | `g_snapshotMiscState6` | med | Copied from DAT_0048f328 (byte) | (none) |
| 0x00493e4b | u32 | `g_snapshotMiscState7` | med | Copied from DAT_0048f32c | (none) |
| 0x00493e4f | u16 | `g_snapshotMiscState8` | med | Copied from DAT_0048f330 | (none) |
| 0x00493e51 | u16 | `g_snapshotMiscState9` | med | Copied from DAT_0048f332 | (none) |
| 0x00494bbc | u32 | `g_snapshotPayloadSize` | high | Set to 0x32a6 (literal) at end of serialize; defines CRC region length | td5_save.c snapshot-length constant |
| 0x00475160 | u32[256] | `g_crc32LookupTable` | high | Standard CRC32 polynomial table; indexed by `(byte ^ crc&0xff)*4` in serialize loop; reused by restore for validation | td5_save.c crc32 table |
| 0x00464084 | char[31] | `g_cupDataXorKey` | high | "Steve Snake says: No Cheating!" — XOR key cycled mod-length with extra ^0x80; used by WriteCupData (0x004114f0), LoadContinueCupData (0x00411590), ValidateCupDataChecksum (0x00411630) | td5_save.c XOR key constant |
| 0x004643bc | struct[20×5×schedules]×32B | `g_perScheduleHighScoreTable_PROVISIONAL` | high | Per-schedule 5-entry table; stride 0x20; entry layout: +0 name[16] (3 dwords + 2 bytes), +0x10 score/time (u32), +0x14 carIndex (u8), +0x18 secondaryMetric (u32), +0x1c flags+ (u8). Insert/shift logic at ScreenPostRaceNameEntry case 4. Stride 0xa4 between schedules (32 × 5 entries + padding) | td5_save.c high-score region (needs struct-promotion follow-up) |
| 0x0046444c | u32 | `g_perScheduleHighScoreThreshold` | high | Per-schedule threshold; new score must beat (or be below for time-trials) this; sits at stride 0xa4 alongside the table; gate for is-this-a-high-score detection at 0x00413c30 | (none) |
| 0x004951d0 | u32 | `g_postRaceQualifyingScore` | high | Current race's primary metric stashed for high-score qualification check (case 0 selects either g_raceFinalResultsSlot0_primaryMetric or actor field +0x328 based on group flags) | (none) |
| 0x00496ff8 | char[16] | `g_postRacePlayerNameEntryBuffer` | high | Edit buffer for player name input on high-score screen; copied into table entry at insertion. Length-checked at strlen() and renamed to default if empty | (none) |
| 0x004970ac | char[16] | `g_postRacePlayerNameEntryDefault` | high | Fallback default name copied into entry buffer when user submits empty | (none) |
| 0x00497314 | int* | `g_postRaceNameButtonSurfacePtr` | med | Tracked frontend surface for "Enter Player Name" button; created/released within ScreenPostRaceNameEntry | (none) |
| 0x004969c0 | byte* | `g_postRaceNameEditTargetPtr` | high | Pointer set to `&g_postRacePlayerNameEntryBuffer`; consumed by RenderFrontendCreateSessionNameInput | (none) |
| 0x004969c8 | u32 | `g_postRaceNameEditMaxLength` | high | Set to 0x10 (16 chars) — limit for name edit | (none) |
| 0x004969d0 | u32 | `g_postRaceNameEditState` | high | 1 = editing in progress; 2 = submitted (case 2 detects user pressing OK) | (none) |
| 0x00497a68 | u32 | `g_postRaceHighScoreEntrySubState` | already-named-like-T1.5 | Reset at case 5/6 of ScreenPostRaceNameEntry; flagged in T1.5 as `g_selectedRaceResultIndex` — semantic clarification: this is the post-race results carousel sub-state, NOT just selection index. Document overlap, do not rename | (none) |
| 0x00466004 | u32 | `g_specialEncounterUnlockA` | med | Gate-block for high-score qualification in special-encounter mode (case 0) | (none) |
| 0x00466008 | u32 | `g_specialEncounterUnlockB` | med | Sibling gate to g_specialEncounterUnlockA | (none) |
| 0x00465e18 | u32 | `g_attractCdTrackCandidate` | high | rand()%7 candidate for next CD track; constrained to avoid current + restricted set (0/7/9/11). Final pick stored into g_selectedCdTrackIndex | (none) |
| 0x00490bd4 | u32 | `g_snapshotSelectedCarIndex` | high | Within snapshot frontend block (offset 0x18 into g_snapshotFrontendStateBlock); restored to g_selectedCarIndex by RestoreRaceStatusSnapshot | (none) |
| 0x00490bec | u32 | `g_snapshotMiscFrontend0` | med | Restored to DAT_0048f368 by RestoreRaceStatusSnapshot; semantic unknown | (none) |
| 0x00490c04 | u32 | `g_snapshotMiscFrontend1` | med | Restored to DAT_0048f370 | (none) |
| 0x00490c1c | u32 | `g_snapshotMiscFrontend2` | med | Restored to _DAT_0048f378 | (none) |

## Key discoveries

1. **Replay record/playback is fully delegated to DXInput (M2DX middleware).** `WriteOpen(g_trackPoolIndex)` and `ReadOpen(g_trackPoolIndex)` are EXTERNAL imports at EXTERNAL:00000024 and EXTERNAL:00000023 respectively. The binary itself owns no replay-file handle, no replay-frame-counter, and no replay-buffer pointer/size globals. The track-pool index IS the seed/discriminator; DXInput hashes it into its own per-track temp file. This is a strategy-shifting finding for the port: `td5_input.c` must mimic M2DX's WriteOpen/ReadOpen contract (per-track temp file written each race; persisted across only ONE frontend-results screen lifetime) — there is NO record-mode sentinel in the main exe to mirror.

2. **The "save" surface is CupData.td5 (cup-mode), not a replay save.** `SerializeRaceStatusSnapshot @ 0x00411120` packs frontend selections + raceFinalResultsSlotTable + g_actorRuntimeState + slot-state table into a single 0x32a6-byte buffer at 0x00490bac, computes CRC32 (DAT_00490bb8), then `WriteCupData @ 0x004114f0` XOR-cycle-encrypts with the "Steve Snake says: No Cheating!" key and writes to CupData.td5. Crucially: there is **no separate ReplayData.td5 file** in the orig — replay data is purely DXInput's per-session temp.

3. **Snapshot is taken at FRONTEND RE-ENTRY (after race ends), not at race start.** `InitializeFrontendResourcesAndState @ 0x00414740` calls SerializeRaceStatusSnapshot only when `g_inputPlaybackActive == 0`. So the snapshot represents the END state of the just-completed race, allowing "Continue Cup" / "View Race Data" to restore the actor table + results + selections atomically.

4. **High-score table is in-binary writable initialized data, not a separate file.** The `g_perScheduleHighScoreTable_PROVISIONAL @ 0x004643bc` region (default first entry literally "Frank", "Jeffrey", etc. baked in) is read/written in-place. It gets persisted via the CupData.td5 snapshot since the snapshot's actor-runtime block at +0x00490cac is 0xc5c dwords = 12656 bytes, large enough to overlap the region (verify by address arithmetic). Actually — re-checking: g_actorRuntimeState's source is at a separate address; the high-score table is NOT inside it. The high-score table must be stable in-memory until exe quit, and likely re-loaded only if CupData.td5 includes it (or it never persists across exe runs and resets to defaults on launch).

5. **`g_replayFileAvailable @ 0x00497A6C` (T5.25) semantics confirmed:** writer at 0x00416d64 (RaceTypeCategoryMenuStateMachine case 2 "Continue Cup") clears to 0, ScreenLocalizationInit attract-mode==2 writer (the "View Replay" code path) sets to 1. Readers in RunRaceResultsScreen at 0x00422f8a, 0x0042304e, 0x0042310c gate the View Replay button visibility. This is purely a UI-gate flag — the actual file existence is owned by DXInput.

6. **`g_postRaceHighScoreEntrySubState @ 0x00497a68` overlaps with T1.5's `g_selectedRaceResultIndex`.** Both batches see this address. T1.5 labeled it as result-index navigator; this batch sees it cleared at case 5/6 transition of name-entry screen. Likely it's a generic frontend sub-state counter reused across screens — keep T1.5's name, document the dual use as a discovery.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x0048f30c..0x0048f380 | 9 small misc-fields restored by snapshot but never directly named here (mirror via g_snapshotMiscState0..9). Likely frontend selection sub-state / drag-race params | frontend selection state (T6/T2 follow-up — already partially named) |
| 0x00497250 | Base of 30-dword frontend selection block copied to snapshot; very likely overlaps the gSelected* family (T1) | frontend selection state |
| 0x0046563c..0x00465664 | 3×3 display-mode resolution preset tables (3 candidates × {w,h,bpp}) used in ScreenLocalizationInit case logic | display-mode subsystem (T6) |
| 0x00465664, 0x00465684 | Per-joystick button-count and type tables (7 slots); seeded from JoystickButtonC_exref/JoystickType_exref at frontend init | input subsystem (T3 — already named family) |
| 0x004ae279 | g_raceResultsSortScratchBuffer adjacent — could be paired-named in struct-promotion | (handled) |
| 0x0049b690..0x0049b910 | DAT_0049b90c base of large translation/localization arrays (stride 0x330 — likely 14 language slots × big struct) iterated in ScreenLocalizationInit | localization (T6) |
| 0x00497a78..0x00497a98 | 8-dword backup save block (already named in T1.5: g_savedCarIndex..g_savedDragRaceParam3) | covered in T1.5 |
| g_perScheduleHighScoreTable_PROVISIONAL | Should be promoted to a struct: `HighScoreEntry { char name[16]; u32 score; u8 carIdx; u8 _pad[3]; u32 secondaryMetric; u8 flags; u8 _pad[3]; }` (32B), then a `PerScheduleHighScores { HighScoreEntry entries[5]; u32 threshold; u8 npc_group_flag; ... }` 0xa4-stride outer struct | struct promotion follow-up |
| g_raceFinalResultsSlotTable_PROVISIONAL | Should be promoted to a struct: `RaceResultEntry { u8 valid; u8 slotIdx; u16 sortRank; u32 lapTimeTotal; u32 pointsOrBonus; u16 topSpeed; u16 result4; }` (20B), 6 slots | struct promotion follow-up |

## TODO impact

- **todo_view_replay_restarts_race_2026-05-19** — **DEFERRED to a runtime / port-side audit (not a global-naming gap)**. Findings: (a) The orig binary has NO record-mode sentinel, NO file-handle global, NO frame-counter — it ALL lives inside the closed-source DXInput middleware that ships as part of M2DX. (b) `g_replayFileAvailable @ 0x00497A6C` (T5.25) is the only UI-side discriminator and is correctly toggled by the existing logic. (c) `g_inputPlaybackActive @ 0x00466e9c` (T1.5) is the binary's only mode-flag, and the existing port sets/clears it at the right code paths.
  - **Fix recipe:** The "View Replay restarts race" bug is therefore **NOT** a missing global; it is a missing port-side equivalent of DXInput's WriteOpen/ReadOpen pair in `td5_input.c`. Concretely: the port must (1) own a `g_replay_record_handle` + `g_replay_playback_handle` + `g_replay_byte_buffer[N]` pair; (2) WriteOpen flushes accumulated input samples to a per-track temp file (track index parameter is filename seed); (3) ReadOpen opens that file and switches the input poller to read from it; (4) On race-end transition, the recorder is flushed and the playback handle is reset. The decision logic itself (write vs read at 0x0042b29c) is already correctly mirrored by the port — only the recorder primitives are missing.
  - **Files to edit:** `td5mod/src/td5re/td5_input.c` (add open/write/read/close primitives), `td5mod/src/td5re/td5_input.h` (handles); no `td5_save.c` changes needed; no new globals to name.

- **todo_view_race_data_broken_2026-05-19** — closed via T1.5; this batch confirms the underlying snapshot mechanism. `g_raceActorRuntimeValid @ 0x0048d988` (renamed here as the slot table's valid-bit at field +0) IS the gate. The fix is: ensure that the actor-runtime block at `g_snapshotActorRuntimeBlock @ 0x00490cac` (and `g_snapshotRaceResultsBlock @ 0x00490c34`) survive across the View-Race-Data re-entry, which they will as long as the CupData.td5 snapshot lifecycle is not aborted mid-screen. The state-3 early-exit gate that T1.5 flagged at 0x00422506 should be relaxed to also accept "valid snapshot present" (DAT_00494bbc != 0 and CRC matches).

