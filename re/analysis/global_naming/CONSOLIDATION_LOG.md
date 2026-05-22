# Consolidation Log — 2026-05-20

Master Ghidra session: `c4b6043840394c59a15e8daa771b9921` (writable, since closed)
Backup: `TD5.rep.bak.20260520-163159-pre-consolidation/`

## Summary totals

| Confidence | Applied | Total proposed | Conflict (other batch won) |
|---|---|---|---|
| High   | 453 | 538 |  85 |
| Medium |  95 | 122 |  27 |
| Low    | (comment-only) | 29 | — |
| Already-named (unknown rows in summary) | (no-op) | 44 | — |

Comments applied (PLATE/EOL with `[Tx.NN 2026-05-20]` marker): **775** across all confidence tiers (counting initial pass + retry pass).

## Phase 1 — Batch processing

## batch_01: ok — 6 attempted (4 high + 2 medium), 5 renames applied at first pass + comments. Subsequent batch 11 reclaimed 4 of these addresses with different per-frame-state semantic names (`g_player0SteeringBiasShort` etc); comments preserved from batch 01.
## batch_02: skipped — no proposal table (writer-loss documentation only; see batch_02_wanted_mode.md)
## batch_03: ok — 9 renames (7 high + 2 medium), 9 comments; used symbol_create at 0x004afbc8
## batch_04: ok — 5/5 high renames + 5 comments at initial pass; 3 overlaps with batch 05 at 0x004966xx addresses (batch 04 names won)
## batch_05: ok — 13 high + 4 medium attempts; 8 high applied (5 conflicts with batch 04 / batch 21); 17 comments
## batch_06: ok — 5 high + 3 medium + 1 low; 7 renames applied, 9 comments
## batch_07: ok — 29 high + 11 medium + 2 low; 33 renames applied (7 conflicts mostly with batch 14/16); 42 comments
## batch_08: ok — 59 high + 10 medium + 2 low + 20 already-named; 63 renames applied (6 conflicts), 71 comments
## batch_09: ok — 17 high + 6 medium; 16 renames applied (5 medium conflicts due to array-bracket names like `g_trackLightContribDirX[3]`); 16 comments
## batch_10: ok — 27 high + 4 medium + 2 low; 28 renames applied; 16 comments
## batch_11: WARN — 10 high + 4 medium + 6 low + 2 already-named; 0 applied renames at retry (ALL 14 high+medium addresses were claimed first by earlier batches 12/13/14/16). Comments still applied via retry pass and preserve T3.11 semantic notes (including the misnaming-comment on g_cameraTransitionActive).
## batch_12: ok — 32 high + 5 medium + 1 low; 35 renames applied, 38 comments
## batch_13: ok — 22 high + 6 medium; 26 renames applied, 28 comments
## batch_14: ok — 26 high + 1 medium + 1 low + 3 already-named; 20 renames applied (7 high conflicts with batch 11/16), 28 comments
## batch_15: ok — 26 high + 4 medium + 3 low; 30 renames applied (no conflicts), 33 comments
## batch_16: ok — 43 high + 6 medium + 1 low + 8 already-named; 36 renames applied (13 high conflicts with batches 04/05/07), 50 comments
## batch_17: ok — 29 high + 2 medium; 22 renames applied (7 conflicts), 31 comments
## batch_18: ok — 14 high + 5 medium + 3 low; 18 renames applied (1 conflict on 0x004c3d94 — Phase 2 resolved this), 21 comments
## batch_19: ok — 18 high + 2 medium; 13 renames applied (5 conflicts), 20 comments
## batch_20: ok — 17 high + 6 medium + 6 already-named; 19 renames applied (4 conflicts), 23 comments
## batch_21: ok — 27 high + 6 medium + 5 low; 30 renames applied (3 conflicts), 33 comments
## batch_22: ok — 14 high + 5 medium + 1 low; 17 renames applied (1 conflict + Phase 2 renames at 0x0048f2fc and 0x004a2c8c), 17 comments
## batch_23: ok — 29 high + 18 medium; 44 renames applied (3 medium conflicts), 47 comments
## batch_24: ok — 40 high + 5 medium + 1 low + 5 already-named; 45 renames applied (no conflicts), 46 comments
## batch_25: ok — 25 high + 5 medium; 29 renames applied (1 conflict), 30 comments

## Phase 2 — Existing-name corrections

- `0x0048f2fc` g_attractModeIdleCounter → **g_extrasGalleryCrossFadePhase** (T5.22): RENAMED + comment
- `0x004a2c8c` g_attractModeControlEnabled → **g_frontendBootDispatchMode** (T5.22): RENAMED + comment
- `0x0045d5d0` (had been renamed to g_audioReplayDistanceMultiplier by batch 11) → **g_halfFloatConstant** (T4.19): RENAMED to general half-float constant; conflicting comments from batches 08/11/19/T4.20 preserved
- `0x004c3d94` g_trackStripAttributeBasePtr → **g_trackStripValidSpanCount** (T4.18): RENAMED + comment
- `g_audioOptionsOverlayActive` → **g_uiInputFreezeOverlayActive_PROVISIONAL** (T3.15/T4.20): RENAMED + comment
- `0x004aaef0` g_cameraTransitionActive: misnaming comment added (NOT renamed; many xrefs). T3.11 note attached.

## Phase 3 — Header file

Generated `td5mod/src/td5re/td5_orig_globals.h`:
- 452 `#define DAT_<addr> g_<name>` pairs (all high-confidence, successfully-applied)
- 471 total lines (with tier section comments and headers)
- Grouped by T1-T5, sorted by address within each tier

## Phase 4 — Pool sync

11 of 16 pool slots refreshed from master (slots 2-9, 13-15).

Slots 0, 1, 10, 11, 12 could NOT be refreshed: stale file handles on `.gbf`/`.ps` scratch files in their `idata/00/~00000000.db/` and `user/00/~00000000.db/` subdirectories. These files are held by either the MCP Ghidra server's prior session or stale Java processes; cannot be removed via rm/PowerShell `Remove-Item -Force`. Will require an MCP server restart or reboot to clear. Pool master `TD5.gpr` and `TD5_headless.gpr` show `available` (no lock files).

## Phase /ghidra-apply (incremental, post T1-T5)

## batch_26_t5_stragglers applied 2026-05-20: 13 renames (8 high + 5 medium) + 15 EOL comments + 2 skipped (operand-encoding artifacts at 0x004c3dc4/0x004c3dd4 — no standalone symbols inside arrays). Header appended with 8 new high-confidence #defines (T6.26 section). Pool slots 2-15 refreshed; slot 1 newly locked-out (stale Java handle from research agent); slot 0 still locked-out from original consolidation.

## batch_27_conflict_triage applied 2026-05-20: 26 re-renames (23 high + 3 medium) — supersedes earlier first-batch-wins names where evidence proved alternates were more precise. Headline catches: 0x0048d988 was single u32 (`g_raceActorRuntimeValid`) → actually u32[30] race-results array (`g_raceResults`); 0x00497a78 was `g_raceReplayStateFlag` → actually `g_postRaceCarSelectionBackupValid`. Skill deviation: triage proposals applied LITERALLY (medium did NOT auto-suffix `_PROVISIONAL`) — confidence is informational only for re-renames. Skipped EOL comments to preserve existing evidence chains. Header appended with 23 #defines under T7.27 section (old #defines from earlier batches remain as duplicates — V1 known-gap). Pool slots 2-15 refreshed; slots 0/1 still locked-out.

## Post-T7 cleanup 2026-05-20:
- **Header dedup**: 491 #defines → 453 unique (last-write-wins), tier section headers vestigial but harmless.
- **Struct promotions** (3 of 5 applied via background agent): TD5_RaceFinalResultsSlot[6] @ 0x0048d988 (120B), TD5_PostRaceCarSelectionBackup @ 0x00497a7c (32B), TD5_CpuInfo @ 0x00497aa0 (44B, base symbol renamed g_cpuVendorString→g_cpuInfo). All under category `/TD5/RE`. SKIPPED: TD5_ChassisBounds (b19/b20 layout disagreement) + TD5_LobbyChatRingEntry (revealed to be 3 parallel arrays, not contiguous stride-0xc4 struct). Pool slots 2-15 re-refreshed. Header updated for g_cpuInfo rename.
- **Function-naming sweep verified COMPLETE**: 876/876 functions named (0 FUN_* auto-name orphans remain). No campaign needed.

## Follow-up items

1. **Stale pool slots 0/1**: Restart Ghidra MCP server (or reboot) to release file locks; re-run `bash scripts/ghidra_pool.sh sync` to propagate. Each pool acquire/release seems to leak a Java handle on the released slot — investigate the MCP cleanup path.
2. **Conflict triage**: 85 high + 27 medium proposals didn't apply because earlier batches named the same address with different (also-semantic) names. The Ghidra EOL comments preserve all interpretations — a future pass can review and pick the most-precise name.
3. **Struct promotions**: Per spec, struct definitions deferred to follow-up session (not attempted in this consolidation).
4. **Batch 02 (wanted_mode)**: No proposal table; rerun via `general-purpose` agent if specific DAT_* labels are wanted (writer-loss finding is already documented).
5. **Batch 11 (per-frame state) 0% apply rate**: All 14 high+medium proposals lost to earlier batches. Comments preserved.
## batch_50_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_51_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_52_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_53_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_54_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_55_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_56_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_57_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 60 comments + 0 skipped
## batch_58_wave2b_audit applied 2026-05-22: 0 renames + 0 function-renames + 28 comments + 0 skipped

## batch_28_t6_frontend_screens applied 2026-05-22: 32 renames + 0 function-renames + 32 comments + 0 skipped
## batch_29_t6_render_pipeline applied 2026-05-22: 32 renames + 0 function-renames + 33 comments + 0 skipped (1 low-conf comment-only)
## batch_30_t6_particle_pool applied 2026-05-22: 16 renames + 0 function-renames + 16 comments + 0 skipped
## batch_31_t6_track_lut_inflate applied 2026-05-22: 16 renames + 0 function-renames + 20 comments + 0 skipped (4 low-conf comment-only)
## batch_32_t6_traffic_camera_audio applied 2026-05-22: 16 renames + 0 function-renames + 18 comments + 0 skipped (1 low-conf, 1 dup vendor)
## batch_33_t6_save_state_snapshot applied 2026-05-22: 26 renames + 0 function-renames + 26 comments + 0 skipped (1 idempotent reaffirm of batch 28 g_carSelectionPreviewActive)
## batch_34_t6_frontend_display_modes applied 2026-05-22: 23 renames + 0 function-renames + 28 comments + 0 skipped (5 low-conf comment-only)
## batch_35_t6_texture_mesh_camera applied 2026-05-22: 18 renames + 0 function-renames + 28 comments + 0 skipped (6 duplicates from batch 29/32, 2 low-conf comment-only, 1 batch29 promotion provisional→high for 0x004b1158/0x004bf504)
## batch_36_t6_audio_codec_chat_misc applied 2026-05-22: 41 renames + 0 function-renames + 45 comments + 0 skipped (4 low-conf comment-only, 1 reaffirm 0x00483050)
## batch_37_t6_math_audio_fmv_final applied 2026-05-22: 41 renames + 0 function-renames + 44 comments + 0 skipped (2 low-conf comment-only); CONFLICT: 0x00496ffc renamed from g_postRaceNameEntrySessionId_PROVISIONAL (batch28) → g_postRaceNameEntrySlotIndex_PROVISIONAL (batch37; same provisional surface, alt-name)
## batch_38_t6_arch_collapsed_exclusions applied 2026-05-22: 0 renames + 0 function-renames + 62 comments + 0 skipped (comment-only ARCH-COLLAPSED CRT exclusions)

## Tier1-A T6 sweep summary 2026-05-22
- 11 batches applied, 261 renames + 352 comments
- 153 high-confidence renames (no _PROVISIONAL); 108 medium (_PROVISIONAL); 24 low-conf comment-only
- 1 conflict (0x00496ffc batch28↔batch37 — both _PROVISIONAL, alt-names retained in EOL comments)
- Header td5mod/src/td5re/td5_orig_globals.h updated: 154 new #define lines under "Added 2026-05-22 from Tier1-A T6 sweep"
