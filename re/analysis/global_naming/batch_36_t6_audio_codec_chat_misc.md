---
batch: 36
area: audio_codec_chat_misc
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x00455900, 0x00456400, 0x0041c000, 0x00410f00, 0x00425500, 0x0042c500, 0x0040de00, 0x0042f300, 0x00432d00
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — ADPCM tables, chat tokenizer, AI rubber-band, etc. (T6)

## Summary

- Functions analyzed: DecodeADPCMBlock, DecodeEAADPCM, DecodeEAIMA_Mono, DecodeEAIMA_Stereo, NormalizeFrontendChatTokens, InitializeRaceSeriesSchedule, ConfigureCupChampionshipSchedule/EraSchedule/PitbullSchedule/UltimateSchedule, ScreenQuickRaceMenu, TrackSelectionScreenStateMachine, ComputeAIRubberBandThrottle, InitializeRaceActorRuntime, FindActorTrackOffsetPeer, RemapCheckpointOrderForTrackDirection, ApplyMissingWheelVelocityCorrection, UpdateActorTrackSegmentContactsReverse, LoadTrackRuntimeData, PollRaceSessionInput, InitializeVehicleTaillightQuadTemplates, RenderVehicleTaillightQuads, SelectTracksideCameraProfile, LoadCameraPresetForView, BuildFrontendDitherOffsetTable, QueueFrontendOverlayRect, FlushFrontendSpriteBlits
- Unnamed DAT_* targeted: 25+
- Proposals — high: 17
- Proposals — medium: 7
- Proposals — comment-only: 3

## Methodology

Walked the remaining mid-traffic candidates (5-10 refs). Grouped by:
1. ADPCM codec LUTs (0x0047fb20..0x00481180)
2. Chat tokenizer scratch + state (0x004972cc..0x004972d3)
3. Series-schedule fields (0x004972a0..0x004972b8)
4. AI rubber-band + race actor runtime (0x004afc7c..0x004afcdc)
5. Camera profile tables (0x00482ea2..0x00482edc, 0x00482dcc..0x00482dd0)
6. ConfigureCupXxxSchedule (0x00494bb0..0x00494bc1)
7. PollRaceSessionInput per-slot state (0x004aaf9c)
8. Misc

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0047fb20 | i32[N] | `g_eaImaStepTableLow` | high | `[idx*4 + 0x47fb20]` reads in DecodeEAIMA_Mono/Stereo (3 refs) — EA-IMA ADPCM step table | `(none)` |
| 0x0047fb30 | i32[N] | `g_eaImaStepTableHigh` | high | Same callers (3 refs) — second half of step table | `(none)` |
| 0x0047fb40 | i32[N] | `g_eaAdpcmCoefTable` | high | `[EAX + EBX*4 + 0x47fb40]` in DecodeADPCMBlock/DecodeEAADPCM (3 refs) — coefficient LUT | `(none)` |
| 0x00481180 | i32[N] | `g_eaAdpcmShiftTable` | high | `[EBX*4 + 0x481180]` in DecodeADPCMBlock/DecodeEAADPCM (3 refs) — shift LUT | `(none)` |
| 0x004972cc | u8 | `g_chatTokenizerCharClass` | high | NormalizeFrontendChatTokens writer; RunFrontendNetworkLobby clears (16 refs) — char-class byte | `(none)` |
| 0x004972cd | u8 (offset) | `g_chatTokenScratchBase` | high | LEA `[ECX + EAX + 0x4972cd]` in NormalizeFrontendChatTokens; PUSH 0x4972cd — base of scratch tokenization buffer (18 refs) | `(none)` |
| 0x004972ce | u8 | `g_chatTokenScratchByte0` | medium | Adjacent token scratch byte (9 refs) | `(none)` |
| 0x004972cf | u8 | `g_chatTokenScratchByte1` | medium | LEA `[ECX + EAX + 0x4972cd]` operates +2 (12 refs) | `(none)` |
| 0x004972d0 | u8 | `g_chatTokenScratchByte2` | medium | (4 refs) | `(none)` |
| 0x004972d1 | u8 | `g_chatTokenScratchByte3` | medium | (8 refs) | `(none)` |
| 0x004972d2 | u8 | `g_chatTokenScratchByte4` | medium | (4 refs) | `(none)` |
| 0x004972d3 | u8 | `g_chatTokenScratchByte5` | low | (3 refs) | `(none)` |
| 0x004972a0 | u32 | `g_raceScheduleEntryPtr_PROVISIONAL` | medium | InitializeRaceSeriesSchedule write (3 refs) — schedule walker base | `(none)` |
| 0x004972b0 | u32 | `g_raceScheduleEntryHeadPtr` | high | InitializeRaceSeriesSchedule + RunRaceResultsScreen + ProcessFrontendNetworkMessages + RunFrontendNetworkLobby writers (6 refs) | `(none)` |
| 0x004972b4 | u32 | `g_raceScheduleEntryTailPtr` | high | InitializeRaceSeriesSchedule + RunRaceResultsScreen (6 refs) | `(none)` |
| 0x004972b8 | u32 | `g_raceScheduleEntryCount_alt` | medium | InitializeRaceSeriesSchedule (3 refs) | `(none)` |
| 0x004afc7c | u32 | `g_raceActorRuntimeListHead` | medium | InitializeRaceActorRuntime writer (3 refs) | `(none)` |
| 0x004afc88 | u32 | `g_aiRubberBandSpeedCap_PROVISIONAL` | medium | InitializeRaceActorRuntime + FindActorTrackOffsetPeer + ComputeAIRubberBandThrottle (3 refs) | `(none)` |
| 0x004afc8c | u32 | `g_aiRubberBandThrottleAccumulator` | high | ComputeAIRubberBandThrottle writes 0x100 init; modified by FindActorTrackOffsetPeer (3 refs) | `(none)` |
| 0x004afcdc | u32 | `g_raceActorListSentinel` | medium | UpdateRaceActors CMP [EBP],0x0; FindActorTrackOffsetPeer (3 refs) — actor list walk sentinel | `(none)` |
| 0x00483020 | u32 | `g_missingWheelVelocityScratch` | high | ApplyMissingWheelVelocityCorrection sole writer 3x; per-wheel velocity correction scratch (4 refs) | `(none)` |
| 0x00483050 | u32* | `g_vehicleCollisionPairScratchA` | medium | ResolveVehicleContacts/ResolveVehicleCollisionPair (5 refs) | `(none)` |
| 0x00483054 | u32 | `g_vehicleCollisionPairScratchB` | medium | Same callers (3 refs) | `(none)` |
| 0x00483550 | u32 | `g_trackRuntimeDataPtr` | high | LoadTrackRuntimeData writer; UpdateActorTrackSegmentContactsReverse readers (4 refs) — track-data ptr | `(none)` |
| 0x00494bb0 | u32 | `g_cupSchedule_currentCup` | high | ConfigureCupPitbullSchedule writes 0x5; ScreenCupWonDialog + ScreenMainMenu1PRaceFlow readers (7 refs) | `(none)` |
| 0x00494bb4 | u32 | `g_cupSchedule_currentRound` | high | ConfigureCupChampionshipSchedule writes 0x2; same readers (7 refs) | `(none)` |
| 0x00494bc1 | u8 (offset) | `g_frontendDitherOffsetTable` | high | BuildFrontendDitherOffsetTable writes `[ESI + 0x494bc0]` (3 refs) | `(none)` |
| 0x004668b4 | u8[N] | `g_packedConfig_quickRaceBlock` | medium | WritePackedConfigTd5/LoadPackedConfigTd5 MOVSD.REP target (4 refs) | `(none)` |
| 0x004668ba | u8 | `g_cupScheduleConfiguredFlag` | high | ConfigureCupChampionshipSchedule + ConfigureCupEraSchedule shared flag (3 refs) | `(none)` |
| 0x004668bf | u8 | `g_quickRaceMenuSelectionByte` | high | ScreenQuickRaceMenu + TrackSelectionScreenStateMachine + ConfigureCupUltimateSchedule (5 refs) | `(none)` |
| 0x00482dcc | u32 | `g_vehicleTaillightQuadTemplatePtr` | high | InitializeVehicleTaillightQuadTemplates + RenderVehicleTaillightQuads (4 refs) | `(none)` |
| 0x00482dd0 | u8 (offset) | `g_vehicleTaillightQuadTemplate_offset_alpha` | medium | STOSD.REP base; RenderVehicleTaillightQuads reads `[ESI + 0x482dd0]` (4 refs) | `(none)` |
| 0x00482ea2 | u16 | `g_tracksideCameraProfile_offset_pitch` | high | SelectTracksideCameraProfile writes 0xff38/0xe2 at `[EAX*8 + 0x482ea2]` (4 refs) — 8B/profile entry | `(none)` |
| 0x00482ea4 | u16 | `g_tracksideCameraProfile_offset_yaw` | high | Same writer; +2 of 8B record (4 refs) | `(none)` |
| 0x00482edc | u32 | `g_tracksideOrbitCameraState_distance` | medium | UpdateTracksideOrbitCamera + UpdateChaseCamera (3 refs) | `(none)` |
| 0x004ad150 | u16 | `g_specialTrafficEncounter_lastSlot` | high | UpdateSpecialTrafficEncounter sole reader (4 refs) | `(none)` |
| 0x004ad152 | u16 | `g_specialTrafficEncounter_pendingFlag` | high | UpdateSpecialEncounterControl + UpdateSpecialTrafficEncounter (5 refs) | `(none)` |
| 0x004aedc0 | u32 | `g_checkpointReorderingSchedule_PROVISIONAL` | medium | RemapCheckpointOrderForTrackDirection (3 refs); compared as ptr | `(none)` |
| 0x004ae584 | u32 | `g_vehicleSuspensionEnvelopeCache_PROVISIONAL` | medium | ComputeVehicleSuspensionEnvelope + LoadRaceVehicleAssets (3 refs) | `(none)` |
| 0x004ae954 | u32 | `g_vehicleAssetPackBase_PROVISIONAL` | low | LoadRaceVehicleAssets (3 refs) | `(none)` |
| 0x004ae2f0 | u16 | `g_vehicleRuntime_offset_yawTrim` | medium | InitializeRaceVehicleRuntime `[ECX+0x70]` (3 refs) | `(none)` |
| 0x004967e4 | u32* | `g_lobbyChatMessageRingBufferEnd` | high | ProcessFrontendNetworkMessages CMP EDI,0x4967e4 (sentinel) + MOVSD.REP (3 refs) | `(none)` |
| 0x00496984 | u32 | `g_lobbyCreateSessionState` | medium | CreateFrontendNetworkSession STOSD; RunFrontendNetworkLobby CMP `[ECX],0x2`; RunFrontendCreateSessionFlow (4 refs) | `(none)` |
| 0x00498f50 | u32 | `g_frontendOverlayRectColor_clear` | medium | RenderFrontendUiRects writes 0xffffffff; QueueFrontendOverlayRect uses `[EAX*4 + 0x498f50]` (4 refs) — overlay color array base | `(none)` |
| 0x00498f64 | u32 | `g_frontendOverlayRectArrayStride` | low | (3 refs) | `(none)` |
| 0x00498724 | u32 | `g_frontendSpriteBlitEntry_offset_dst` | medium | FlushFrontendSpriteBlits `[EDI+0x4]` (3 refs) — sprite-blit entry +4 = dst-rect ptr | `(none)` |
| 0x0049870c | u32 | `g_frontendDisplayModePreviewPriorLayoutPtr` | high | RenderFrontendUiRects CMP; RestoreFrontendDisplayModePreviewLayout reader; BeginFrontendDisplayModePreviewLayout writer (3 refs) | `(none)` |
| 0x004aaf9c | u32[6] | `g_perSlotInputBindingState` | high | PollRaceSessionInput `[ESI*4 + 0x4aaf98]` — per-slot input binding state (3 refs); base 0x4aaf98 has named neighbor (g_subTickFraction) at 0x4aaf60 | `(none)` |
| 0x00497234 | u32 | `g_frontendNetMsgOpcodeMatched` | medium | ProcessFrontendNetworkMessages CMP/MOV (3 refs) | `(none)` |
| 0x00497270 | u32 | `g_raceScheduleEntryWalkOffset_PROVISIONAL` | low | InitializeRaceSeriesSchedule ADD EAX,0x4 (3 refs) | `(none)` |
| 0x00497263 | u8 | `g_lobbyChatBuffer_flag` | medium | RunFrontendNetworkLobby R/W (3 refs) | `(none)` |
| 0x00496ffd | u8 (offset) | `g_postRaceNameEntryBuffer_offset_1` | low | MOVSB.REP target +1 (3 refs) | `(none)` |

## Key discoveries

- **EA-ADPCM/IMA codec tables** at 0x0047fb20..0x00481180 are present in TD5_d3d.exe — confirms the M2DX FMV codec is linked statically. These should be tagged ARCH-COLLAPSED for any future codec replacement audit (modern FFmpeg replacement would replace the entire codec subsystem).
- Chat tokenizer at 0x004972cc..0x004972d3 is **8 bytes of scratch state** consumed by NormalizeFrontendChatTokens. This is a TODO from L3 triage — "chat tokenizer 2KB" workload now has a named scratch surface to anchor the port.
- 0x00482ea2..0x00482ea4 is a **TracksideCameraProfile struct base** — 8 bytes per entry: (pitch_u16, yaw_u16, distance_u16, fov_u16?). SelectTracksideCameraProfile writes constants like `0xff38`, `0xe2` for specific track-camera presets.
- 0x004ad150/152 confirm `g_specialTrafficEncounter` — single state for cop chase / special-events traffic dispatcher (limited refs because the feature is rarely active).
- The `g_aiRubberBandThrottleAccumulator` at 0x004afc8c writes `0x100` initial value — the "0x100 = +1.0 fp8" rubber-band base offset.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00465e54 | u32 — Music test track table | T7 frontend (low refs) |
| 0x004ae2f0/2f8 | u16 — vehicle runtime fields | T7 physics |
| 0x004cf078/080/088 | locale | ARCH-COLLAPSED |
| 0x0048f765 | already covered in batch 33 | save |

## TODO impact

- No direct closures. Chat tokenizer and AI rubber-band names anchor surfaces for future targeted fixes.
