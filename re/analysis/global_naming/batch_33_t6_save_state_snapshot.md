---
batch: 33
area: save_state_snapshot
tier: T6
target_todos: [todo-view-replay-restarts-race-2026-05-19]
ghidra_session: TD5_pool3
analyzed_addresses: 0x0040f8e0, 0x0040fa00, 0x0040fc80, 0x00411200, 0x00411400, 0x0040e1d0, 0x0040ea00
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — race-status snapshot + packed-config buffer (T6)

## Summary

- Functions analyzed: SerializeRaceStatusSnapshot, RestoreRaceStatusSnapshot, WritePackedConfigTd5, LoadPackedConfigTd5, CarSelectionScreenStateMachine, ScreenControllerBindingPage
- Unnamed DAT_* targeted: 20+
- Already-named neighbors noted: g_configTd5XorKey, g_configTd5Filename, g_savedMaxUnlockedCar, g_savedCarLockTable
- Proposals — high: 15
- Proposals — medium: 4
- Proposals — comment-only: 2

## Methodology

The cluster 0x0048f300..0x0048f800 contains a large packed-config + race-status snapshot buffer. WritePackedConfigTd5 / LoadPackedConfigTd5 use `MOV EDI,<absolute_addr>; MOVSD.REP` to copy fixed-size sub-blocks. Each `MOV EDI,<addr>` reveals a struct-field base. SerializeRaceStatusSnapshot/RestoreRaceStatusSnapshot is a smaller pair around 0x0048f31a..0x0048f332. The data sits AFTER existing-named globals 0x00463e0c..0x00464120 and represents the in-memory representation of Config.td5.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f30d | u8 (offset) | `g_savedCarState_offset_carIdx` | high | MOVSX `[reg + 0x48f30c]` in 3 CarSelection sites — byte field at +1 of struct base 0x48f30c (5 refs) | td5_save.c |
| 0x0048f31a | u8 | `g_raceStatusSnapshot_phase` | high | SerializeRaceStatusSnapshot MOV AL, [0x48f31a]; RestoreRaceStatusSnapshot writes; CarSelection reads (4 refs) | `(none)` |
| 0x0048f326 | u8 (offset) | `g_savedCarState_offset_owned` | medium | `[ECX + 0x48f324]=0x2` write in CarSelection — owned/unlocked byte (4 refs) | td5_save.c |
| 0x0048f332 | u8 | `g_raceStatusSnapshot_completion` | high | SerializeRaceStatusSnapshot/RestoreRaceStatusSnapshot pair (4 refs) | `(none)` |
| 0x0048f34c | u32 | `g_carSelectionConfirmStateMachine` | high | CarSelection writer/reader (4 refs); state-machine subphase | `(none)` |
| 0x0048f354 | u32 | `g_carSelectionPaintIndex` | medium | CarSelection writer/reader (4 refs); paint-job index | `(none)` |
| 0x0048f3b6 | u8[N] | `g_packedConfig_subBlock0` | high | `MOV EDI,0x48f3b6; MOVSD.REP` in WritePackedConfigTd5+LoadPackedConfigTd5 (4 refs) — config sub-block #0 | `(none)` |
| 0x0048f3fe | u8[N] | `g_packedConfig_subBlock1` | high | Same pattern, second block (4 refs) | `(none)` |
| 0x0048f41e | u8[N] | `g_packedConfig_subBlock2` | high | Same pattern, third block (4 refs) | `(none)` |
| 0x0048f451 | u8[N] | `g_packedConfig_subBlock3` | high | Same pattern (4 refs) | `(none)` |
| 0x0048f5d9 | u8[N] | `g_packedConfig_subBlock4` | high | Same pattern (4 refs) | `(none)` |
| 0x0048f765 | u8[N] | `g_packedConfig_subBlock5` | high | Same pattern (4 refs) | `(none)` |
| 0x0048f385 | u8 | `g_packedConfig_dirtyFlag` | high | WritePackedConfigTd5/LoadPackedConfigTd5 writes 0; reads as base+offset; dirty-bit (8 refs) | `(none)` |
| 0x0048f2d0 | u32 | `g_carSelectionConfirmDialogRect` | medium | CarSelection PUSH 0x48f2d0 twice (4 refs); LEA target for sprintf — UI rect/struct ptr | `(none)` |
| 0x0048f350 | u32 | `g_carSelectionPreviewActive` | high | (duplicate from batch 28; reaffirm) — CarSelection sole writer (5 refs) | `(none)` |
| 0x0048f354 | u32 | `g_carSelectionPaintIndex` | high | (as above) | `(none)` |
| 0x004643cc | u32 | `g_highScoreEntryHeadPtr_PROVISIONAL` | medium | ScreenPostRaceNameEntry MOV EDX,[ECX]; DrawPostRaceHighScoreEntry reader (9 refs) — high-score linked list head | `(none)` |
| 0x004643d4 | u32 | `g_highScoreEntry_offset_name` | high | `[ECX + 0x4643d4] = EAX` in ScreenPostRaceNameEntry; +8 of entry struct (4 refs) — high-score name ptr field | `(none)` |
| 0x004643d8 | u32 | `g_highScoreEntry_offset_time` | high | `[ECX + 0x4643d8] = EDX`; +0xc of entry struct (4 refs) — high-score time field | `(none)` |
| 0x00465498 | char* | `g_uiFormatStringScratchTemplate` | medium | PUSH 0x465498 in ScreenTwoPlayerOptions/DrawRaceDataSummaryPanel/DrawPostRaceHighScoreEntry (6 refs) — printf format str | `(none)` |
| 0x0046549c | u32* | `g_trackedFrontendSurfaceListHead` | high | InitializeFrontendResourcesAndState writer; CreateTrackedFrontendSurface read/write (5 refs) — linked-list head | `(none)` |
| 0x00464060 | u32 | `g_controllerBindingScrollOffset` | medium | WritePackedConfigTd5 reads; ScreenControllerBindingPage writer (5 refs) | `(none)` |
| 0x00463fc8 | u8[N] | `g_controllerBindingsCache_PROVISIONAL` | medium | WritePackedConfigTd5 MOVSD.REP; `[EAX*4 + 0x463fc8]` in ScreenControllerBindingPage (16 refs) — 4-byte stride binding entries | `(none)` |
| 0x00463fcc | u32 | `g_controllerBindings_current_PROVISIONAL` | medium | (6 refs) — adjacent +4 to above, accessed `[EAX + 0x463fcc]` in render | `(none)` |
| 0x00490b9c | u32 | `g_controllerBindingPage_inputCursor` | medium | ScreenControllerBindingPage writer; RenderControllerBindingMenuPage reader (6 refs) | `(none)` |
| 0x00490ba0 | u32 | `g_controllerBindingPage_state` | high | ScreenControllerBindingPage writer/reader (15 refs); FSM state | `(none)` |
| 0x00494fd0 | u32 | `g_frontendSurfaceRegistryHead` | high | FlushFrontendSpriteBlits/ReleaseTrackedFrontendSurface/ClearFrontendSurfaceRegistry head ptr (13 refs) | `(none)` |
| 0x00494fd4 | u32 | `g_frontendSurfaceRegistryTail` | high | Same callers (5 refs); paired tail | `(none)` |
| 0x00494fd8 | u32 | `g_frontendSurfaceRegistryCount` | high | Same callers (9 refs) | `(none)` |
| 0x00494fdc | u32 | `g_frontendSurfaceRegistryEntries` | medium | GetFrontendSurfaceRegistryId reads `[ECX*8 + 0x494fd4]` (5 refs) | `(none)` |

## Key discoveries

- The packed-config buffer at 0x0048f3b6..0x0048f800 is a contiguous **block of nested sub-records**, each copied as a MOVSD.REP run. Wave 2 should reconstruct the Config.td5 in-memory layout by examining each sub-block's size between consecutive MOV EDI,<addr> instructions.
- 0x0048f30C is the base of a small **per-car saved-state struct** (offsets +0/+1/+0x1e/+0x26 used as fields). Likely 6 entries (one per garage slot). Wave 2 should type it.
- 0x004643cc is a **high-score table head pointer**, with entries containing +0x8 (name) and +0xc (time). Linked-list/array. Confirms post-race name entry writes to a separate persistent buffer.
- The frontend-surface registry at 0x00494fd0..0x00494fdc is a small (head, tail, count, entries) 4-word descriptor with `[ECX*8 + base]` stride — 8 bytes per entry.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x0048f765 | inside packed config — sub-block 5 | already covered |
| 0x004660b0/b4 | (4-3 refs) | save XOR / packed config |
| 0x004660e8 | (4 refs) | save XOR / packed config |
| 0x00465e54 | u32 — Music test extras table | T6 frontend |
| 0x004668bf | u8 — quick race menu byte | T6 frontend |
| 0x00465e10 | (3 refs) | font/asset |
| 0x00465664 | (3 refs) | frontend |
| 0x00465678 | not seen | |

## TODO impact

- **todo-view-replay-restarts-race-2026-05-19**: The save buffer structure here confirms there IS a serialize/restore path (`SerializeRaceStatusSnapshot/RestoreRaceStatusSnapshot`) — fields exist but **replay file** path is separate. No closure here; the missing replay-record callsite is in the race-loop, not the snapshot.
