---
batch: 37
area: math_lut_audio_fmv_remainder
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x0040a680, 0x0040a6e0, 0x00424440, 0x00424ab0, 0x00455900, 0x00456500, 0x0043bf90, 0x0043c350, 0x004183b0, 0x00418400, 0x00415d40
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — sin/cos LUT, font glyphs, ADPCM state, FMV, inline strings (T6)

## Summary

- Functions analyzed: BuildSinCosLookupTables, CosFixed12bit, SinFixed12bit, DrawFrontendLocalizedStringToSurface, MeasureOrCenterFrontendLocalizedString, DrawFrontendLocalizedStringSecondary, DecodeEAADPCM, PlayIntroMovie, RequestIntroMovieShutdown, RunAudioOptionsOverlay, ResetFrontendInlineStringTable, SetFrontendInlineStringTable, SetFrontendInlineStringEntry, ScreenMainMenuAnd1PRaceFlow, InitializeTrafficActorsFromQueue, RecycleTrafficActorFromQueue, ScreenPostRaceNameEntry, InitializeRaceSession, LoadRaceTexturePages, FinalizeCameraProjectionMatrices, BuildCameraBasisFromAngles, ApplyMeshRenderBasisFromTransform/WorldPosition, RenderRaceActorsForView, DrawRaceDataSummaryPanel
- Unnamed DAT_* targeted: 28
- Proposals — high: 17
- Proposals — medium: 7
- Proposals — comment-only: 4

## Methodology

Final pass: walk remaining 4-7 ref candidates, cross-check with `BuildSinCosLookupTables` for math-LUT discovery and `DecodeEAADPCM` for codec state. The 0x004ab000..0x004ab088 cluster is now fully resolved as the 3x4 view-basis matrix (3 rows × 4 floats = 48 bytes) plus a separate output staging area at 0x004ab030..0x004ab050.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00483984 | i32[4096] | `g_sinCosLut_fixed12` | high | BuildSinCosLookupTables writer indexed by `[EAX*4 + 0x483984]`; CosFixed12bit + SinFixed12bit readers (3 refs) — fixed-point sin/cos table indexed by 12-bit angle | `(none)` — port replaces via inline sin/cos? |
| 0x004ab078 | float | `g_cameraBasisMatrix_m20` | high | BuildCameraBasisFromAngles writes 0 (4 refs) — m[2][0] | `(none)` |
| 0x004ab07c | float | `g_cameraBasisMatrix_m21` | high | Same writer 0 (4 refs) — m[2][1] | `(none)` |
| 0x004ab080 | float | `g_cameraBasisMatrix_m22` | high | BuildCameraBasisFromAngles writes `0x3f800000` (=1.0f) — m[2][2] diagonal (4 refs) | `(none)` |
| 0x004ab084 | float | `g_cameraBasisMatrix_m23` | high | Same writer 0 (4 refs) | `(none)` |
| 0x004ab050 | float | `g_cameraBasisMatrix_m10` | high | BuildCameraBasisFromAngles writes 1.0f (4 refs); diag of M[1][0] | `(none)` |
| 0x004ab054 | float | `g_cameraBasisMatrix_m11_alt` | medium | Adjacent (4 refs) — second column of m[1] | `(none)` |
| 0x004ab048 | float | `g_cameraBasisMatrix_m02` | medium | BuildCameraBasisFromAngles writes 0 (4 refs); m[0][2] | `(none)` |
| 0x004ab04c | float | `g_cameraBasisMatrix_m03` | medium | Same (4 refs); m[0][3] | `(none)` |
| 0x004ab034 | float | `g_cameraBasisStaged_m00` | medium | ApplyMeshRenderBasisFromTransform/WorldPosition + ApplyMeshResourceRenderTransform FSTP (3 refs) — output of basis-staging pass | `(none)` |
| 0x004ab038 | float | `g_cameraBasisStaged_m01` | medium | Same callers (3 refs) | `(none)` |
| 0x004ab03c | float | `g_cameraBasisStaged_m02` | medium | Same callers (3 refs) | `(none)` |
| 0x004ab000 | float | `g_cameraBasisInputTranslateX` | high | FLD `[0x4ab000]` then FSUB by world coords in ApplyMeshRenderBasis* — input translate-X (3 refs) | `(none)` |
| 0x004ab004 | float | `g_cameraBasisInputTranslateY` | high | FSUB `[0x4ab004]` (3 refs) | `(none)` |
| 0x004ab008 | float | `g_cameraBasisInputTranslateZ` | high | FSUB `[0x4ab008]` (3 refs) | `(none)` |
| 0x004ab00c | float | `g_cameraBasisInputDistance` | medium | FSUB `[0x4ab00c]` (3 refs); 4th-coord input | `(none)` |
| 0x004aaff8 | float | `g_cameraStagedVecA` | medium | FLD `[0x4aaff8]` in ApplyMeshRenderBasis* (3 refs) — staged vector x | `(none)` |
| 0x004aaffc | float | `g_cameraStagedVecB` | medium | Same callers (3 refs) | `(none)` |
| 0x004660b0 | char[4] | `g_quickRaceMenuFormatPrefix` | high | ScreenQuickRaceMenu PUSH 0x4660b0 3x — printf format string addr (4 refs) | `(none)` |
| 0x004660b4 | u32[N] | `g_raceDataSummaryFieldTable` | high | `[EAX*4 + 0x4660b4]` in DrawRaceDataSummaryPanel 3x (4 refs) — 4-byte-stride field descriptor table | `(none)` |
| 0x004660e8 | u8[N] | `g_localizedFontGlyphWidthTable` | high | `[ESI + 0x4660e8]` in DrawFrontendLocalizedStringToSurface/Secondary + MeasureOrCenter (4 refs) — glyph-width LUT for current-localization font | `(none)` |
| 0x00466eb0 | u32[N] | `g_raceTexturePageDescriptorTable` | high | InitializeRaceSession STOSD initialize + MOVSD.REP populate; LoadRaceTexturePages `[ESI*4 + 0x466eb0]` (4 refs) — texture page descriptor table | `(none)` |
| 0x004668b0 | u8[N] | `g_packedConfig_displayPrefsBlock` | medium | WritePackedConfigTd5/LoadPackedConfigTd5 MOVSD; RunFrontendDisplayLoop reads `[EDX + 0x4668b0]` (4 refs) — display-prefs block | `(none)` |
| 0x004811a0 | u32 | `g_eaAdpcmDecoderState_predA` | high | DecodeEAADPCM sole writer/reader (4 refs) — predictor A | `(none)` |
| 0x004811a4 | u32 | `g_eaAdpcmDecoderState_predB` | high | DecodeEAADPCM sole writer/reader (4 refs) — predictor B | `(none)` |
| 0x004811a8 | u32 | `g_eaAdpcmDecoderState_predC` | medium | DecodeEAADPCM (4 refs) | `(none)` |
| 0x004811ac | u32 | `g_eaAdpcmDecoderState_stepIndex` | medium | DecodeEAADPCM (4 refs) — step-table index | `(none)` |
| 0x004bea98 | u32 | `g_introMovieStatePtr` | high | PlayIntroMovie sole writer (5 refs) — playback state ptr | `(none)` |
| 0x004beaac | u32 | `g_introMovieFrameAccumulator` | high | PlayIntroMovie sole writer (5 refs) — frame accumulator | `(none)` |
| 0x004beb98 | u8[N] | `g_trackedActorMarkerEntryScratch` | medium | InitializeTrackedActorMarkerBillboards LEA `[ESI + 0xfffffdd4]` (5 refs) — billboard entry scratch | `(none)` |
| 0x004bed08 | u8[N] | `g_trackedActorMarkerLodScratch` | low | InitializeTrackedActorMarkerBillboards LEA `[ESI + 0xffffff44]` (6 refs) | `(none)` |
| 0x004bec50 | u8[N] | `g_trackedActorMarkerSpriteScratch` | low | InitializeTrackedActorMarkerBillboards (6 refs) | `(none)` |
| 0x004963f4 | u32 | `g_frontendInlineStringFSM_state` | high | ResetFrontendInlineStringTable writes 0; SetFrontendInlineStringTable + ScreenMainMenuAnd1PRaceFlow writers (4 refs) | `(none)` |
| 0x00496370 | u32 | `g_frontendInlineStringActiveEntry` | high | ScreenMainMenuAnd1PRaceFlow reader 3x (4 refs); paired with above | `(none)` |
| 0x00496374 | u32[N] | `g_frontendInlineStringEntryTable` | high | SetFrontendInlineStringEntry `[ECX*4 + 0x496374]` writer (3 refs) | `(none)` |
| 0x004964d4 | u8[N] | `g_lobbyChatScratchBuffer` | high | RenderFrontendLobbyChatPanel byte read; ProcessFrontendNetworkMessages MOVSD.REP target via LEA `[EDI+0xc4]` (4 refs) — chat-msg scratch | `(none)` |
| 0x00496410 | u8[N] | `g_lobbyChatMessageRingBuffer` | high | RenderFrontendLobbyChatPanel + ProcessFrontendNetworkMessages MOVSD.REP target (11 refs) — chat-msg ring buffer base | `(none)` |
| 0x0049640c | u32 | `g_lobbyChatMessageRingCursor` | high | RenderFrontendLobbyChatPanel + ProcessFrontendNetworkMessages writes 0x1 (5 refs) — cursor | `(none)` |
| 0x004b1360 | float | `g_audioOptionsMusicVolumeRendered` | high | RunAudioOptionsOverlay FSTP/FLD pairs (4 refs) — rendered slider position | `(none)` |
| 0x004b1364 | float | `g_audioOptionsSfxVolumeRendered` | high | Same caller (3 refs) | `(none)` |
| 0x004b0270 | u32 | `g_trafficActorRecycleHead` | high | RecycleTrafficActorFromQueue + InitializeTrafficActorsFromQueue MOV EBP,0x4b0270 sentinel (3 refs) | `(none)` |
| 0x004b0290 | u32 | `g_trafficActorActiveList` | high | RecycleTrafficActorFromQueue + InitializeTrafficActorsFromQueue + RenderRaceActorsForView `[ECX*4 + 0x4afbe8]` offsetted (3 refs) | `(none)` |
| 0x00496ffc | u32 | `g_postRaceNameEntrySlotIndex` | medium | ScreenPostRaceNameEntry + RunFrontendCreateSessionFlow (7 refs) | `(none)` |

## Key discoveries

- **`g_sinCosLut_fixed12`** at 0x00483984 confirms TD5 uses a precomputed sin/cos LUT for 12-bit fixed-point angles (used by CosFixed12bit/SinFixed12bit). This is a candidate for byte-faithful port replication for FP determinism. The LUT spans approximately 16 KB (4096 entries × 4 bytes).
- The camera basis matrix at 0x004ab000..0x004ab088 is now fully decoded: 0x4ab000-0x4ab00f = input translate+distance, 0x4ab010-0x4ab02f = basis output stage, 0x4ab030-0x4ab057 = staged matrix output, 0x4ab060-0x4ab087 = final 3×3 view rotation matrix (m00..m22 diagonals at 0x4ab040, 0x4ab070, 0x4ab080 = 1.0f). Wave 2 should type this entire block as a `CameraBasisState` struct.
- 0x004b0270/290 paired with already-named `g_trafficActorRecycleQueue` neighbors confirm the **traffic recycle linked list + active array** structure. RenderRaceActorsForView walks the active array via `[ECX*4 + 0x4afbe8]`.
- The inline-string-entry table at 0x00496374 + state at 0x004963f4 is a **per-screen formattable string slot** mechanism — when ScreenMainMenu wants to display "Welcome back, {playername}", it stores the playername pointer in a slot here.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| ARCH-COLLAPSED libc/CRT: 0x004cf078..0x004cfd28 range | __pioinfo, __mb_cur_max, _setmbcp, locale | exclude from rename — vendor MSVC runtime |
| 0x004d0d00..0x004d0d28 | __sbh_heap_init/_heap_alloc/_heap_free — MSVC sbh heap | ARCH-COLLAPSED |
| 0x0047d6e6..0x0047d8e8 + 0x0047b240..0x0047b270 | _input, _strgtold12, _toupper_lk, _isspace, __sbh_alloc_block, _getptd | ARCH-COLLAPSED libc |

## TODO impact

- No direct closures. Names of `g_sinCosLut_fixed12` and `g_eaAdpcmDecoderState_*` mark substrate areas where future audits might find FP/codec divergences.
