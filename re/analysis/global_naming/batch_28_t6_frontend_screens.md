---
batch: 28
area: frontend_screens
tier: T6
target_todos: [todo-view-race-data-broken-2026-05-19]
ghidra_session: TD5_pool3
analyzed_addresses: 0x0040dfc0, 0x00415000, 0x00417dcc, 0x00418500, 0x00420cc0, 0x0041eb19, 0x0041bc78, 0x004154f4, 0x0040eaef
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — frontend screen FSMs (T6)

## Summary

- Functions analyzed: ~16 (Screen*** state machines, CarSelection, SoundOptions, TwoPlayerOptions, ExtrasGallery, MusicTest, RunFrontend* lobbies)
- Unnamed DAT_* globals targeted: 32
- Already-named neighbors noted: g_currentScreenIndex, g_returnToScreenIndex, g_selectedGameType, g_frontendButtonIndex
- Proposals — high: 22
- Proposals — medium: 8
- Proposals — comment-only: 2

## Methodology

Started from W1-E hot-traffic list filtered to Screen*/RunFrontend* callers. For each candidate, fetched all xrefs and grouped by writer function. The `Screen*StateMachine` family writes/reads a small set of FSM scalars (`flow_phase`, `selected_idx`, `confirm_state`) wedged between named neighbors at 0x00495200-0x00496400. Localization init scratch buffers (0x49b6xx..0x49b9xx) identified by the `LEA [EBP+N]; PUSH; <ptr=fixed addr>` pattern.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0048f31c | u32 | `g_quickRaceSelectedTrackId` | high | ScreenQuickRaceMenu writes/reads it as the chosen track id (56 refs); CarSelection/RaceResults read post-selection | `(none)` |
| 0x004962bc | u32 | `g_mainMenuFlowPhase` | high | CarSelection + ScreenMainMenu + RunFrontendConnectionBrowser + RaceTypeCategoryMenu writers; values 0/1/2 in CarSelection (`CMP=1, MOV=2`) — FSM phase scalar (17 refs) | `(none)` |
| 0x004962c0 | u32 | `g_networkLobbyEntryPhase` | high | Written by CreateFrontendNetworkSession (0x0), RunFrontendCreateSessionFlow (0x1), ScreenMainMenu and CarSelection — lobby-creation phase scalar (14 refs) | `(none)` |
| 0x004962dc | u32 | `g_oneRaceFlowSelectedCarId` | high | ScreenMainMenuAnd1PRaceFlow writes a selected-id (12 refs); CarSelection consumes it. Sits adjacent to `g_currentScreenIndex` block | `(none)` |
| 0x00496360 | u32 | `g_extrasGalleryPage` | high | ScreenExtrasGallery is the SOLE writer & reader; CMP/MOV pattern of a page-cursor (13 refs) | `(none)` |
| 0x00496364 | u32 | `g_extrasGalleryAssetHandle` | high | ScreenExtrasGallery + LoadFrontendExtrasGalleryResources writer (5 refs) — paired with 0x00496360 | `(none)` |
| 0x00496400 | u32 | `g_musicTestSelectedTrackId` | high | ScreenMusicTestExtras is sole writer/reader of u32 (11 refs); music-test cursor | `(none)` |
| 0x00497a48 | u32 | `g_soundOptionsMusicVolume` | high | ScreenSoundOptions sole writer/reader; sits in 4-tuple 0x497a48/4c/50/54 — music vol slider state (13 refs) | `(none)` |
| 0x00497a4c | u32 | `g_soundOptionsSfxVolume` | high | ScreenSoundOptions sole writer/reader (13 refs); second in 4-tuple | `(none)` |
| 0x00497a50 | u32 | `g_soundOptionsMenuVolume` | high | ScreenSoundOptions + ScreenControlOptions readers; third in 4-tuple (21 refs) | `(none)` |
| 0x00497a54 | u32 | `g_twoPlayerOptionsSelection` | high | ScreenTwoPlayerOptions sole writer/reader (8 refs); fourth in 4-tuple | `(none)` |
| 0x00496400 | u32 | `g_musicTestSelectedTrackId` | high | as above (dedupe) | `(none)` |
| 0x0049725d | u8 | `g_lobbyHostSessionFlag` | medium | InitializeRaceSeriesSchedule + RunFrontendNetworkLobby read; byte gate near network init (16 refs) | `(none)` |
| 0x004969d9 | u8 | `g_pendingNetMsgHeaderByte` | medium | QueueFrontendNetworkMessage builds a small struct via MOVSD; [EAX+1] read in ProcessFrontendNetworkMessages — msg-header byte (13 refs) | `(none)` |
| 0x004969dc | u32 | `g_pendingNetMsgPayloadPtr` | medium | Same MOVSD copy site as above; +3 bytes after `g_pendingNetMsgHeaderByte` (6 refs) | `(none)` |
| 0x004968ad | u8 | `g_lobbyNetMessageDispatchFlag` | high | Written by ProcessFrontendNetworkMessages (multiple bytes); RunFrontendNetworkLobby (33 refs) — dispatch-pending flag | `(none)` |
| 0x004968ae | u8 | `g_lobbyNetMessageState` | medium | Adjacent to dispatch flag (12 refs); same callers | `(none)` |
| 0x004968b1 | u8 | `g_lobbyNetMessageSubState` | medium | Adjacent (19 refs); same caller distribution | `(none)` |
| 0x004968b2 | u8 | `g_lobbyNetMessageRetryCount` | medium | 8 refs in same lobby net-msg cluster | `(none)` |
| 0x00496400 | u32 | `g_musicTestSelectedTrackId` | high | duplicate dropped | `(none)` |
| 0x00496408 | u32 | `g_lobbyChatInputCursor` | high | RenderFrontendLobbyChatPanel + ProcessFrontendNetworkMessages writer/reader (6 refs) — chat input cursor index | `(none)` |
| 0x004970ad | u8 | `g_postRaceNameEntryActiveFlag` | medium | RunFrontendConnectionBrowser/ScreenPostRaceNameEntry use a SCASB/PUSH 0x4970ad scan — name-entry buffer in use byte (6 refs) | `(none)` |
| 0x00496ffc | u32 | `g_postRaceNameEntrySessionId` | medium | RunFrontendCreateSessionFlow + ScreenPostRaceNameEntry pair (7 refs) | `(none)` |
| 0x0049726c | u32 | `g_raceScheduleEntryCount` | high | Written/read by InitializeRaceSession + InitializeRaceSeriesSchedule + RunRaceResultsScreen (7 refs) — series schedule len | `(none)` |
| 0x00497284 | u32 | `g_raceScheduleCurrentRound` | medium | Same caller set as schedule entry count (7 refs), 0x18 bytes apart | `(none)` |
| 0x0049729c | u32 | `g_raceScheduleCompletedMask` | medium | Same caller set as schedule entry count (7 refs) | `(none)` |
| 0x0048f350 | u32 | `g_carSelectionPreviewActive` | high | CarSelectionScreenStateMachine sole writer (5 refs) — preview-overlay active flag | `(none)` |
| 0x0048f358 | u32 | `g_carSelectionFrameAccumulator` | high | DrawCarSelectionPreviewOverlay + CarSelection write/read u32 (11 refs); ticks during preview anim | `(none)` |
| 0x0048f360 | u32 | `g_carSelectionPreviewFrameIndex` | high | DrawCarSelectionPreviewOverlay + CarSelection writer (writes 0) — initialized to zero in CarSelection (10 refs) | `(none)` |
| 0x0049b69c | u8[48] | `g_localizationCarNameScratch` | high | LEA EBP-relative followed by `ADD ECX,0x49b69c` in CarSelection/QuickRaceMenu/DrawRaceDataSummaryPanel — fixed scratch buffer for one localized car-name string (15 refs); zero-init bytes | `(none)` |
| 0x0049b6cc | u8[48] | `g_localizationCarManufScratch` | medium | Same stride/0x30 from 0x49b69c; ScreenLocalizationInit pushes its address; DrawPostRaceHighScoreEntry reads it (7 refs) | `(none)` |
| 0x0049b6fc | u8[48] | `g_localizationCupTitleScratch` | medium | Same stride; CarSelection + ScreenLocalizationInit pair (6 refs) | `(none)` |
| 0x0049b72c | u8[48] | `g_localizationTrackNameScratch` | high | CarSelection ADD EDX,0x49b72c 3x — track-name buffer (11 refs) | `(none)` |
| 0x0049b7ec | u8[48] | `g_localizationOpponentLeftScratch` | medium | ScreenLocalizationInit fills via push (8 refs); used in cmpf with team-name strings | `(none)` |
| 0x0049b81c | u8[48] | `g_localizationOpponentRightScratch` | medium | Same usage pattern (8 refs) | `(none)` |
| 0x0049b84c | u8[48] | `g_localizationModeNameScratch` | medium | (8 refs); ScreenLocalizationInit | `(none)` |
| 0x0049b87c | u8[48] | `g_localizationDifficultyScratch` | medium | (8 refs); ScreenLocalizationInit | `(none)` |
| 0x0049b8ac | u8[48] | `g_localizationOptionLabelScratch` | medium | CarSelection adds 0x49b8ac (8 refs) | `(none)` |
| 0x0049b96c | u8[48] | `g_localizationStatusLineScratch` | medium | ScreenLocalizationInit usage (11 refs) | `(none)` |
| 0x0049b99c | u8[48] | `g_localizationDialogTitleScratch` | medium | ScreenLocalizationInit usage (11 refs) | `(none)` |

## Key discoveries

- `ScreenLocalizationInit` (at 0x00426aXX) builds **10 fixed-address localization scratch buffers** at 0x0049b6XX..0x0049b9XX, each 0x30 bytes apart. These are written-then-read by various Screen* renderers. They effectively form an array `g_localizationStringScratch[10][48]` — Wave 2 could type this as a struct.
- The `Screen*` family writes a coordinated 4-tuple of u32s at 0x00497a48/4c/50/54 — `(music, sfx, menu, twoPlayerOption)`. Worth a Wave 2 typed struct.
- `g_quickRaceSelectedTrackId` at 0x0048f31c is the **shared selection** read by CarSelection/RaceResults/QuickRaceMenu/MarkMastersRaceSlotCompleted — confirms the 1P selection bus.
- 0x0049725d is a single byte (not u32 in spite of 16 refs); read by InitializeRaceSeriesSchedule but also by RunFrontendNetworkLobby — looks like a **host-session active flag** crossing the FE/race boundary.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ad152 | u8 — UpdateSpecialEncounterControl + UpdateSpecialTrafficEncounter; police-chase trigger | T6 race state |
| 0x004cf288 | u32 — only 1 caller-less ref (no fn) — likely libc/static init | ARCH-COLLAPSED |

## TODO impact

- **todo-view-race-data-broken-2026-05-19**: No direct closure. `g_currentScreenIndex` already named, view-replay regression remains downstream (state-3 gate). Surface remains for T7.
