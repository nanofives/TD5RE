---
batch: 27
area: conflict_triage
tier: T7
target_todos: []
ghidra_session: <none — text-only analysis>
analyzed_addresses: 0x0045d650, 0x0045d658, 0x0045d660, 0x0045d668, 0x0045d68c, 0x00465e18, 0x00483014, 0x0048d988, 0x0048dc50, 0x0049636c, 0x004962d0, 0x004970ac, 0x00497a60, 0x00497a64, 0x00497a68, 0x00497a6c, 0x00497a70, 0x00497a74, 0x00497a78, 0x00497a7c, 0x004aaf93, 0x004c3d78, 0x004c3d7c, 0x004c3da0, 0x004c3ddc, 0x004c3de4
agent: claude-opus-4-7
date: 2026-05-20
---

# Globals enumeration — Conflict triage (T1-T5 cross-batch name reconciliation)

## Summary

- Functions analyzed: 0 (text-only batch-file diff)
- Conflict addresses inventoried: **106** (139 losing alternates across 79 two-way + 22 three-way + 5 four-way + 1 five-way)
- Identical-name conflicts (T2 re-discovered T1 name verbatim): 10 — no action
- Distinct-name conflicts requiring decision: 96
- **Re-rename recommended**: 26 (listed in Proposals table below)
- **Keep current** (most precise / equally precise / Phase 2 already corrected): 70
- **Merge into combined name** suggested: 1 (documented in Key discoveries)
- **Struct-promotion candidates** (single-name picking is wrong choice): 5 (listed in Out-of-scope)

## Methodology

1. Parsed all 25 batch files via `_build_conflicts.py` → `_conflicts.json` (106 conflict addresses).
2. For each conflict, ranked proposals by batch number (lowest = winner per consolidation rule).
3. Applied triage rubric: keep current unless an alternate is materially more precise OR catches a misnaming.
4. Cross-checked against `CONSOLIDATION_LOG.md` Phase 2 (already-corrected misnamings) to avoid double-fixing.
5. Defaulted to **keep current**; re-renames flagged only when the winner name is generic-while-alternate-is-semantic, or when winner has been disproved by later evidence (e.g. wrong type/size).

Confidence preserved from the higher of the two batches in each pair. All Ghidra EOL comments from `/ghidra-apply` already preserve alternate names — this batch only changes which name is the primary symbol.

## Proposals (globals)

The following addresses have a current name that is **less precise, wrongly typed, or semantically wrong** vs an alternate from a later batch. Re-rename recommended.

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045d650 | f32 (4.0f) | `g_simTickBudgetCap` | high | currently `g_renderHudFixedPointShift_4` (low conf b08, speculation "probably a glyph-cell-mod constant"); b21 high-conf evidence proves it's the hardcoded 4.0 cap on `g_simTickBudget` in `RunRaceFrame`. b20's `g_audioDopplerVelocityScale` (200000.0) is byte-pattern wrong (0x48435000 vs actual 0x40800000). Current name is misleading. [conflict-triage] | `td5_game.c` `TD5_SIM_TICK_BUDGET_CAP = 4.0f` |
| 0x0045d658 | f64 (1e-3) | `g_msPerSec_double` | high | currently `g_doubleInvThousand` (low conf b11, generic-literal name); b21 high-conf evidence shows it's the ms→s conversion divisor in `RunRaceFrame` (`g_normalizedFrameDt = dt_ms * 30/1000`). Both batches agree on byte pattern (0.001). b21 captures the role; b11 only captures the value. [conflict-triage] | `td5_game.c` time scaling const |
| 0x0045d660 | f64 (30.0) | `g_targetFps_double` | high | currently `g_doubleThirty` (low conf b11); b21 high-conf evidence confirms it's the 30Hz reference rate constant in the FMUL chain that produces `g_normalizedFrameDt`. b11 evidence even notes the value is "30 Hz = the original target framerate". Promote the semantic name. [conflict-triage] | `td5_game.c` `TD5_SIM_TARGET_FPS = 30.0f` |
| 0x0045d668 | f64 (1000.0) | `g_msPerSec_reciprocal` | high | currently `g_doubleThousand` (low conf b11); b21 high-conf evidence: multiplied with `1/frame_dt_ms` to yield FPS (`g_instantFPS = (1/dt) * 1000`). b11 name is pure-literal; b21 captures the role. [conflict-triage] | `td5_game.c` ms→fps converter |
| 0x0045d68c | float (1/65536) | `g_subTickQuantum` | high | currently `g_inv65536F` (medium conf b11, value-only); b21 high-conf evidence: converts 24.8 fixed-point sim accumulator low-word to normalized sub-tick fraction. The b11 name describes the value; the b21 name describes the role. [conflict-triage] | `td5_game.c` `TD5_SUB_TICK_QUANTUM = 1.0f/65536.0f` |
| 0x0048d988 | u32[30] (120B) | `g_raceResults` | high | currently `g_raceActorRuntimeValid` (b05 high — but evidence is only "checked at state-3 early-exit gate; if invalid, bounces user" with single-u32 size). b16 high-conf evidence proves it's a 120-byte (u32[30]) race-results array, twin of `g_raceSchedule @ 0x00497250`, copied to cup buffer at `&DAT_00490C34` by `SerializeRaceStatusSnapshot`. b23's `g_raceFinalResultsSlotTable_PROVISIONAL` is the same block viewed as struct[6]×20B. The b05 name is wrong: the "state-3 gate" reader was likely checking results[0] != 0 = "results have been recorded". Critical misnaming — directly affects todo_view_race_data_broken. [conflict-triage] | `td5_save.c` TD5_CupDataBuffer.race_results (offset 0x88) |
| 0x0048dc50 | byte[6][0x170] | `g_vehicleShadowAndWheelSpriteTemplates` | high | currently `g_vehicleShadowSpriteTemplates` (b06 medium); b15 high-conf evidence is more precise: holds the shadow quad AND wheel-billboard template pair per slot, with explicit stride 0x170 × 6 records ending at 0x0048F128. The b06 name omits the wheel-template half. [conflict-triage] | `td5_render.c` shadow+wheel templates |
| 0x00483014 | u8[6] | `g_remoteBrakeCheatPerSlotLatch` | high | currently `g_cheatRemoteBrakingApplied` (b01 high — but the b01 evidence is sparse: "Reset to 0 / Set to 1"); b11 high-conf evidence is materially more precise: it's a per-slot 4-bit latch (set to 0xF, not 1) gated on `_g_networkControlBufferReset != 0` inside `UpdatePlayerVehicleControlState`. b25's `g_playerControlAccum0` is wrong (that's the next address). The b11 name documents the 0xF magnitude + the network-controlled gate. [conflict-triage] | port: cheat-remote-brake handling (likely missing from port) |
| 0x0049636c | u32 (0/1) | `g_benchmarkRequestPending` | high | currently `g_raceResultsLoadedFlag` (b05 medium, weak evidence "checked as early-entry gate"); b21 high-conf evidence definitive: set to 1 by `ScreenMainMenuAnd1PRaceFlow case 4 button 2` (TimeDemo button) and read by `RunRaceResultsScreen case 0` to fast-forward to benchmark display loop. Cleared by `ScreenStartupInit`. The b05 name describes a guess; b21 names the actual TimeDemo benchmark mechanism. [conflict-triage] | `td5_game.c` benchmark request gate |
| 0x004970ac | char[60] | `g_localComputerName` | high | currently `g_postRacePlayerNameEntryDefault` (b23 high — but the b23 evidence is only "fallback default name"); b24 high-conf evidence proves it's filled by `GetComputerNameA(&DAT_004970ac, &local_8)` at network browser entry with 60-byte capacity (next named global at 0x004970bc). Falls back to literal "Clint Eastwood" if WinAPI fails. The b23 name is incomplete; b24 names the actual Win32 source. [conflict-triage] | port `s_session.name` default |
| 0x00497a60 | u32 | `g_postRaceProgressionAdvanced` | high | currently `g_raceResultsSeriesAdvanceFlag` (b05 high, vague); b25 high-conf evidence is more precise: write `0` at race-start (state 0), `1` after `g_raceWithinSeriesIndex += 1` in cup-next-race path. Guards against double-increment when re-entering the screen. Captures the explicit double-increment-guard mechanism. [conflict-triage] | `td5_frontend.c` post-race progression gate |
| 0x00497a64 | u32 | `g_postRaceMenuButtonChoice` | high | currently `g_lastSelectedResultButton` (b05 high); b25 high-conf evidence is more precise: stores 0-4 from one of the 5 specific post-race menu buttons (Race Again / View Replay / View Race Data / Select New Car / Quit), read by state 0x10 to dispatch the chosen branch. The b25 name documents the enum nature and dispatch target. [conflict-triage] | `td5_frontend.c` menu-choice latch |
| 0x00497a68 | u32 (0..5) | `g_postRaceRacerCardIndex` | high | currently `g_selectedRaceResultIndex` (b05 high); b25 high-conf evidence: NOT "selection index" — it's the racer slot whose card is being displayed on the results screen, incremented/decremented by nav direction with wraparound 0..5 and skip-if-state-3 logic. Passed to `DrawRaceDataSummaryPanel()`. The b05 name elides the per-slot carousel iteration. [conflict-triage] | `td5_frontend.c` results racer iterator |
| 0x00497a6c | u32 | `g_replayFileAvailable` | high | currently `(g_languageSelectPostBootFlag — low confidence, comment-only)` (b22 low, marked "out of attract scope"); b23 (medium) AND b25 (high) both identify it as the replay-file-on-disk flag — gates between active vs grayed-out View Replay button in `RunRaceResultsScreen`. The b22 winner is explicitly low-confidence/comment-only so the address has no rename applied at all. The b25 name is high-confidence + directly relevant to todo_view_replay_restarts_race. [conflict-triage] | `td5_frontend.c` view-replay button gate |
| 0x00497a74 | u32 | `g_postRaceRestartSelectedRace` | high | currently `g_raceResultsStateGateFlag` (b05 medium, weak "checked to gate state 3 exit"); b25 high-conf evidence: written `1` at state 0x15 (Race Again path) and at `InitializeFrontendResourcesAndState @ 0x00414789`. Read at 0x00422940 to gate "skip results, restart race" early-exit. The b25 name documents the actual Race Again semantic. [conflict-triage] | `td5_frontend.c` restart-race flag |
| 0x00497a78 | u32 (0..2) | `g_postRaceCarSelectionBackupValid` | high | currently `g_raceReplayStateFlag` (b05 high — "Written as 1, 2, 0 in state 0; controls backup lifecycle"); b25 high-conf evidence shows the "backup" is the car-selection scratch block at 0x00497A7C..0x00497A98 — NOT replay state. Tri-state valid-flag for the 8-dword backup taken when user starts a race, restored on Race Again. The b05 name mistakenly conflates "replay" with the car-selection backup mechanism. [conflict-triage] | `td5_frontend.c` car-selection backup gate |
| 0x004aaf93 | u8 | `g_f3PauseInputHeldFlag` | high | currently `g_globalDebugPanelKeySticky` (b11 medium, "Likely a debug/diagnostics toggle key"); b14 high-conf evidence is definitive: F3-key edge-detect latch where releasing F3 sets `g_inputPollDeferFlag = 1`. b24's `g_autoExitKeyHeldState` is wrong (claims F2 = VK_0x3d but VK_F3 = 0x72, and 0x3d is actually VK_OEM_PLUS — needs verification but b14 evidence cites "F3" explicitly via DXInput::CheckKey). Port mirror already labels this as `s_f3_held`. [conflict-triage] | `td5_input.c:186` `s_f3_held` |
| 0x004c3d78 | void* | `g_chassisMeshArchiveEntry` | high | currently `g_chassisStaticHedEntryPtr` (b17 high — focuses on "static.hed entry"); b19 + b20 (both high, identical name) converge on `g_chassisMeshArchiveEntry` which is more general and accurate. The b17 name is too narrow (the entry is the chassis MESH header, not just a static.hed reference). Two later batches independently arrived at the same alternate — strong signal. [conflict-triage] | `td5_physics.c` / `td5_render.c` chassis sub-mesh resource |
| 0x004c3d7c | float | `g_chassisBoundsXMin` | high | currently `g_chassisFixedPointBounds` (b17 medium — group-named all 4 floats as one block with caveat "individual semantic needs follow-up"); b19 high-conf evidence: this address is specifically the `min_x` component (chassis-mesh header +0x2c). The b17 group-name is fine for the cluster but loses the per-component semantic. b20 also proposes `g_chassisBoundsMinX` (close synonym). Re-rename to b19's specific name. [conflict-triage] | `td5_physics.c` chassis bounds floats |
| 0x004c3da0 | void* (junction-table holder) | `g_trackStripBlobAliasJunction` | high | currently `g_track_route_metadata` (b03 high — but framed as "struct ~96 bytes" which is wrong; this is a pointer, not a struct); b18 + b19 high-conf converge on `g_trackStripBlobAlias` / `g_trackStripBlobAliasJunction`. The b19 name is most precise: it's the alias of the STRIP.DAT blob pointer specifically used as the junction-table holder (+0x14 = count, +0x1c = entries × 6B). 16 reads across track/AI/junction walkers. **MEMORY-NOTED**: this is RS_TRACK_OFFSET_BIAS focal point candidate per [t2-global-naming-sweep] memo — naming it precisely supports cascade-unwind investigation. Per memo `reference_t4_global_naming_sweep_2026-05-20` line "g_trackStripBlobAlias @ 0x004c3da0 IS junction-table holder". [conflict-triage] | `td5_track.c` strip-blob alias (junction-table holder) |
| 0x004c3ddc | i32 | `g_weatherSegmentTargetDensityView1` | medium | currently `g_weatherSegmentTargetView0_PROVISIONAL` (b15 low, comment-flag only — explicitly provisional); b26 medium-conf evidence is more definite about the view-1 role (read/written as `*(int *)(&g_weatherSegmentTargetDensity + view*4)`). Note: current name says "View0" but the address (+0x4 from base 0x004c3dd8) makes it View1. Drop _PROVISIONAL and correct View0→View1. [conflict-triage] | (none) |
| 0x004c3de4 | dword | `g_weatherActiveCountView1` | medium | currently `g_weatherActiveCountView1_PROVISIONAL` (b15 low); b26 medium-conf evidence confirms the view-1 role via consistent stride-4 indexing. Drop _PROVISIONAL suffix; both batches agree on the view-1 semantic. [conflict-triage] | (none) |
| 0x00465e18 | u32 | `g_attractCdTrackCandidate` | high | currently `g_lastPlayedCdTrackIndex` (b13 high — focuses on `ScreenMusicTestExtras` writer); b22 + b23 evidence both show it's the random-track-pick mechanism used by `InitializeFrontendResourcesAndState` (random number 0..6 with restricted set 0/7/9/11 avoided). The b13 reader is a downstream consumer of the b22/b23 producer. The producer-side semantic is more fundamental. **Note**: b23's name is most precise; consider that the b13 reader is a USE not the SOURCE. [conflict-triage] | (none — port stub) |
| 0x004962d0 | u32 | `g_postRaceSkipResultsBanner` | medium | currently `g_raceSinglePlayerCompanionBitfield` (b05 medium — vague "gates display mode for companion slot races"); b25 medium-conf evidence is more concrete: gates `RunRaceResultsScreen` `if (DAT_004962d0 != 1)` early-skip to `SetFrontendScreen(0x1a)` in attract-mode/DNF. Writes 0/1 by `InitializeFrontendResourcesAndState`. The b05 name appears to be a guess; b25 ties it to actual `SetFrontendScreen` dispatch. [conflict-triage] | `td5_frontend.c` attract-mode skip-results flag |
| 0x00497a70 | u32 | `g_postRaceNextCupRaceAvailable` | high | currently `g_cupModeNextRaceAvailable` (b05 medium — short name, weak evidence "Set by ConfigureGameTypeFlags(); gates Next Cup Race button"); b25 high-conf evidence adds the explicit state-0xd write site + race-restart clear + 0x00423507 reader for button-styling. Both names mean the same thing; b25 is higher confidence + slightly more precise (the "postRace" prefix anchors it to the screen). Marginal re-rename. [conflict-triage] | `td5_frontend.c` cup-progress flag |
| 0x00497a7c | u32[8] | `g_postRaceCarSelectionBackup` | high | currently `g_savedCarIndex` (b05 high — but described as single u32); b25 high-conf evidence: it's an 8-DWORD backup block at 0x00497a7c..0x00497a98 covering `{g_selectedCarIndex, DAT_0048f368, DAT_0048f370, _DAT_0048f378, DAT_00463e08, DAT_0048f36c, DAT_0048f374, DAT_0048f37c}`. The b05 name describes only the first field of the array. Critical size correction. [conflict-triage] | `td5_frontend.c` car-selection backup struct |

## Key discoveries

- **Total conflicts found**: 106 addresses × ~139 alternates. Breakdown:
  - **Keep current**: 67 conflicts where current name is at least as precise (and often the alternate is just a sibling-discovery of the same field from a later batch's lens).
  - **Re-rename to alternate**: 28 conflicts where current name is misleading, wrongly sized, or vague while a later batch's name is materially more specific.
  - **Merge into combined**: 1 (the b08-internal conflict at 0x004b1140 where init writes one float and runtime writes a wrong-way counter to the same address — kept as `g_hudWrongWayCounter_v0` per runtime-dominance, but a struct/union promotion would be cleaner).
  - **Identical-name confirmations** (no action, both batches independently arrived at same name): 10. These include `g_perTrackLightZoneTables` (b09+b18), `g_trafficBusTableBlob` (b10+b18), `g_trackStripDataBlob` (b10+b18), `g_leftRouteTableBlob`/`g_rightRouteTableBlob`/`g_raceCheckpointTableLive` (b10+b18), `g_dragRaceLaneStripPtr` (b10+b19), `g_chassisMeshArchiveEntry` (b19+b20 — informs Phase 2 corrective above), `g_cheatCodeXorMaskTable` (b07+b16), and `g_hudCurrentViewIndex` (b08+b21). These T2/T3 doubles are validation, not problems.

- **Meta-pattern A — Batch 21 (debug overlays) systematically improved batch 11 (per-frame state) numeric-constant naming.** Six addresses in the 0x0045d6xx FPU-constants band (`g_floatFour`, `g_doubleThirty`, `g_doubleThousand`, `g_inv65536F` etc.) were named by b11 as *value-only* (correct value, but no functional context). b21 re-discovered them with their role in `RunRaceFrame` (sim-tick budget, target FPS, sub-tick quantum). The b21 names are uniformly more useful for source-port maintenance. This explains why "batch 11 lost everything" — its early-phase constant-naming was deliberately literal; the role-based naming came later.

- **Meta-pattern B — Batch 25 (long-tail post-race) systematically reframed batch 05 (replay/results).** Eight addresses in the 0x00497axx post-race state band were named by b05 with generic flag suffixes (`_Flag`, `_Index`, `_StateGateFlag`); b25 re-discovered them with specific button-dispatch / state-machine semantics (`Choice`, `BackupValid`, `RacerCardIndex`, `RestartSelectedRace`). b25 has stronger evidence because it traced the actual state-0xd / state-0x15 dispatch graph. Re-renames recommended for 7 of these.

- **Meta-pattern C — A handful of T1 (b01-b06) names captured fields that later turned out to be wrong-sized or wrong-typed.** Notable: `g_raceActorRuntimeValid` (b05 u32) → actually `g_raceResults` (b16 u32[30] — 120 bytes); `g_savedCarIndex` (b05 u32) → actually `g_postRaceCarSelectionBackup` (b25 u32[8]). These were under-typed at T1 because the early sweeps only looked at single readers. Re-renames recommended.

- **Phase 2 already handled the highest-impact misnamings**: `g_attractModeIdleCounter @ 0x0048f2fc`, `g_attractModeControlEnabled @ 0x004a2c8c`, `g_audioReplayDistanceMultiplier @ 0x0045d5d0` → `g_halfFloatConstant`, `g_trackStripAttributeBasePtr @ 0x004c3d94`, `g_uiInputFreezeOverlayActive_PROVISIONAL`, and the `g_cameraTransitionActive` misnaming comment at 0x004aaef0. These are NOT re-listed here — already resolved.

- **The 0x0046317c steering bias conflict is subtler than it looks.** Three batches gave different names:
  - b01 (winner): `g_playerSteeringWeightTarget` — semantic, but evidence ambiguous about indexing
  - b11: `g_perSlotViewIndicatorInitialAngle` — wrong (the address is u16[6] for steering, not view indicator)
  - b25: `g_player0SteeringBiasShort` — accurate but per-slot only-element-0
  - Verdict: Keep b01. b01's name is the most semantically correct (it IS the player steering weight target); the b25 evidence actually validates this by showing the write formula `(short)iVar1 + 0x100`. The b11 evidence is the misreading (the indexed read into actor field +0x33e is a side-effect, not the primary role).

- **Cross-batch convergence is a positive signal.** Where b19 and b20 (both T4 mesh-archive sweeps) independently arrived at the same name (`g_chassisMeshArchiveEntry` for 0x004c3d78), that name is more credible than the lone b17 alternative. Two such convergent renames recommended above.

- **Notable keep-current cases worth documenting** (current name is correct; alternate is the misreading):
  - `0x004aee14 g_trackLightingZoneTablePtr` (b09 winner) — b10's `g_trackCircuitFlagOrLapCount` was a wrong guess; the address is the actual lighting zone table pointer per per-track lookup.
  - `0x004c38a0 g_actorTrackZoneAudioStateCode` (b09 winner) — b18 misread `psVar9[0x10]` as "texture page id" but audio reads it as a switch case 1-5; the b09 cross-subsystem-mailbox framing is correct.
  - `0x004c3d04 g_textureUploadSkipResample` (b17 winner) — b18 noted "never written via instruction" but that's a quirk-observation, not a different semantic; keep b17.
  - `0x0046317c g_playerSteeringWeightTarget` (b01 winner, kept despite 3-way conflict) — b11's `g_perViewIndicatorInitialAngle` is wrong (misread the +0x33e write as the primary role); b25's `g_player0SteeringBiasShort` is over-narrow (only-element-0 view of a u16[6]).
  - `0x004b1140` / `0x004b1178` (b08 internal conflict, kept as wrong-way counters) — see Out-of-scope for the union/struct-promotion candidate that would resolve cleanly.

## Out-of-scope finds

Addresses where the conflict turned out to be a legitimate dual-semantic / dual-phase use of the global. A struct/union promotion would be cleaner than picking a single name. Defer to struct-promotion follow-up.

| address | brief note | probable area |
|---|---|---|
| 0x004b1140 | b08 internal conflict: init writes `view0_center_x` (float); runtime writes `wrong_way_counter[0]` (i32) to same byte range | Promote to `union { f32 init_centerX; i32 runtime_wrongWayCount; }` in HUD per-view struct |
| 0x004b1178 | Same dual-phase pattern as 0x004b1140 for view 1 | Same union pattern |
| 0x0048d988 | 120-byte block read by b05 as gate flag (offset 0), by b16 as u32[30] schedule, by b23 as struct[6]×20B | Struct promotion `RaceResultsEntry result[6]` with field +0 = primary_metric + field +1 = slot_index + field +2 = rank (b23 evidence) |
| 0x00497a7c | 8-dword block (per b25) covering 8 distinct car-selection fields; current name `g_savedCarIndex` only names the first | Struct promotion `CarSelectionBackup { u32 car_id; u32 paint; u32 ...; }` |
| 0x004c3d7c..0x004c3d88 | 4 floats (chassis bounds X-min/X-max/Z/Y per b19, or Min/Max XZ per b20 — semantic disagreement on whether it's a {xmin,xmax,zmin,zmax} or {xmin,xmax,zmin,ymax} layout) | Struct promotion `ChassisBounds { f32 xmin, xmax, zmin, ymax; }` resolves the semantic ambiguity rather than picking one batch's labelling |

## TODO impact

No frontmatter TODOs targeted (this is a meta batch). Indirect impacts:

- **todo_view_race_data_broken**: Re-renaming 0x0048d988 from `g_raceActorRuntimeValid` (single u32) to `g_raceResults` (u32[30]) clarifies that the "state-3 early-exit gate" reader is checking results[0] (not a "valid" flag). Suggests fix at td5_frontend.c:9188 to validate the results array properly.
- **todo_view_replay_restarts_race**: Re-renaming 0x00497a6c to `g_replayFileAvailable` (high confidence) clarifies the button-gate variable. The port may set this incorrectly — direct named target for investigation.
- **todo_cascade_unwind_2026-05-17** (RS_TRACK_OFFSET_BIAS focus): Re-renaming 0x004c3da0 to `g_trackStripBlobAliasJunction` precisely identifies the junction-table holder that 16 track/AI/junction walkers dereference. Memory-noted in MEMORY.md as the cascade focal point.
