---
tier: T4
date: 2026-05-20
batches: [16, 17, 18, 19, 20]
status: ready_for_consolidation
total_proposals: 119
---

# T4 Global Naming Sweep — Config + Asset Loaders

## Headline stats

| batch | area | functions | globals | high | med | low |
|---|---|---|---|---|---|---|
| 16 | config_save_state | 12 | 32 | 24 | 6 | 2 |
| 17 | asset_loaders (ZIP/TGA/mesh) | 25 | 23 | 19 | 4 | 0 |
| 18 | track_data_parsers | 16 | 20 | 13 | 5 | 2 |
| 19 | carparam_ai_tuning | 7 | 22 | 17 | 4 | 1 |
| 20 | audio_asset_loaders | 11 | 22 | 17 | 5 | 1* |
| **TOTAL** | | ~71 functions | **119** | **90** | **24** | **6** |

## TODO impacts

### Newcastle invisible wall — concrete hypotheses (T4.18)
**`g_trackStripBlobAlias @ 0x004c3da0` is the JUNCTION-TABLE HOLDER for forks/branches**:
- Header at +0x14 = junction count
- Entries at +0x1c, stride 6 bytes = 3 ushorts: `target_span, exit_dist_lo, exit_dist_hi`

Three hypotheses for span 216:
1. Junction entry covers span 216 with non-linear `target_span`; port's linear `span+1` render dispatches incorrectly
2. Span 216 may be type 8 (junction-start) or type 11 (junction-end) — port render path likely lacks type-8/11 dispatch
3. Span 216 may be inside a junction's exit-distance range, skipped by relocator

**Investigation step**: Frida-hook reads of `g_trackStripBlobAlias + 0x14/+0x1c` during Newcastle race; dump junction table; check if span 216 falls inside any entry's exit range.

### "AI all same color" — candidate root cause (T4.17)
**`g_slotCarTypeIndexAlt @ 0x00466ec8` is the alt-color skin index table** — port currently doesn't emit alt skins. Flag as new investigation if AI livery variety is reported missing.

### Existing TODOs validated (T4.19)
- `reference_drag_ai_template_binding` — fully confirmed by audit
- `reference_arch_cardef_per_actor_indirection` — fully confirmed; no code change needed
- `todo_drag_strip_ai_idle` (RESOLVED 2026-04-28) — naming confirms port's full-throttle synthetic driver is design-consistent
- **`todo_playerisai_carparam_binding` — STALE flag**: memo says "shipped on worktree, NOT yet merged" 5 days ago. Consolidation should verify commit 48d320a landed on master.

### No new INI gaps (T4.16)
All 7 persistent `[GameOptions]` fields correctly wired to `g_td5.ini.*`. Optional: `[GameOptions] CatchupAssist` for `g_twoPlayerCatchupAssist @ 0x00465FF8` if deterministic 2P-test scripting is needed.

## Key structural reveals

### XOR-key strings verified byte-for-byte (T4.16)
- `g_configTd5XorKey @ 0x00463F9C` = `"Outta Mah Face !! "`
- `g_cupDataTd5XorKey @ 0x00464084` = `"Steve Snake says : No Cheating! "`

### AI rubber-band formula reconstructed (T4.19)
From `ComputeAIRubberBandThrottle @ 0x00432D60`. The 4 tuning constants form a `lag_gain × lag_saturation × lead_cut × lead_saturation` quad. Output is `0x100 - throttle_adj` written to `gActorDefaultRouteSteerBias[slot]` per tick.

**Drag-mode bypass** confirmed by both paths: when `g_selectedGameType != 0`, (a) AI template at `0x00473db0` NOT copied to actor cardef, (b) rubber-band constants forced to neutral (gains=0, saturations=0x40, output always `0x100`).

### Per-difficulty/tier 3D-flattened branch table (T4.19)
10-branch if/else at `0x00432f06..0x004334d5`. Each branch writes the same 4 rubber-band constants + 4 cardef offsets (+0x68, +0x6e, +0x70, +0xb0) with different scalars.

### ZIP + DEFLATE state (T4.17)
20 file-scope globals across two clusters:
- ZIP archive streaming state: 12 globals at `0x004c3760, 0x004cf974-88, 0x0047b1d4-ec`
- DEFLATE inflate state machine: 8 globals at `0x0047b1c8-f4` (bit-buffer/bit-count/output-cursor)

Future cleanup: natural fit for `TD5_ZipStreamState` struct.

### Track parsers (T4.18)
- STRIP.DAT header = 5-dword block; word[3] is write-only (`DAT_004c3d8c` vestigial)
- MODELS.DAT relocator rebases ONLY first dword in each 8-byte top-level entry pair; second dword never read
- `DAT_004aee10` (per-track `.STR` env metadata) is DISTINCT from `g_trackEnvironmentConfig` (LEVELINF.DAT)
- `g_textureUsesTallPageFormat` is dead-code feature flag (always 0)
- **Rename proposal**: `g_trackStripAttributeBasePtr` → `g_trackStripValidSpanCount` (usage is unambiguously a count, not a pointer)

### Audio loaders (T4.20)
- DXSound buffer pool: 88 slots (44 base + 44 dup), hardcoded immediate offsets — port's `TD5_SOUND_TOTAL_SLOTS=88` / `TD5_SOUND_DUP_OFFSET=44` are correct
- `g_audio3dDistanceScale @ 0x00474a58` = 1/550f (Ghidra had wrong 1-byte classification)
- 9 Doppler/attenuation floats named at `0x0045d5d0..0x0045d798`
- **`_DAT_0045d5d0` has DUAL semantics** — primary: replay-mode distance multiplier (0.5f); secondary: UV-atlas offset in LoadRaceVehicleAssets
- **Two distinct audio-options state sets**: pre-race `ScreenSoundOptions @ 0x0041ea90` writes percent forms (persisted to Config.td5); in-race `RunAudioOptionsOverlay @ 0x0043bf70` writes float-fraction siblings — port should treat percent as canonical

### Correction (T4.19 vs T4.20 cross-reference)
`_g_fixedPointToFloatScale` autoname at `0x0045d5d0` is **wrong**. Address is `0.5f` (`0x3f000000`), not `1/256` (`0x3b800000`). T4.19 renames it `g_halfFloatConstant`; T4.20 confirms dual semantics. **Consolidation must apply this correction.**

## Consolidation flags

### Operand-encoding artifact families (NOT separate globals)
3 families that need struct-array promotion at consolidation:
- `gVehicleTuningTable + N` (each row = 0x8C bytes)
- `gVehiclePhysicsTable + N`
- `g_actorRuntimeState + N` (each row = 0x388 bytes)

Promoting these eliminates **38+ fake-DAT_ labels** that are operand-encoding artifacts rather than real globals.

### Existing-name corrections
- `g_audioOptionsOverlayActive` is MISNAMED (T3.15 finding, reconfirmed) — actually a general "freeze state mutations" gate. Rename candidate.
- `g_hudFontGlyphAtlasPtr` and `g_hudFadeOverlayAlpha` misnamed per T2.8.
- `_g_fixedPointToFloatScale` autoname wrong (T4.19) — rename to `g_halfFloatConstant`.
- `g_trackStripAttributeBasePtr` should be `g_trackStripValidSpanCount` (T4.18).

## Cumulative status (T1 + T2 + T3 + T4)

| tier | globals | high | med | low | new TODO closes | major reveals |
|---|---|---|---|---|---|---|
| T1 | 37 | 29 | 7 | 1 | 6 TODOs root-caused | bit-28 input pattern |
| T2 | 126 | 85 | 30 | 11 | (cascade focal point named) | RS_TRACK_OFFSET_BIAS |
| T3 | 136 | 89 | 33 | 14 | smoke-render CLOSED + shadow partial | g_raceCountdownTimer rename unifies camera+countdown |
| T4 | 119 | 90 | 24 | 6 | Newcastle wall hypotheses, alt-skin table | AI rubber-band formula; XOR keys; junction-table holder |
| **TOTAL** | **418** | **293** | **94** | **31** | | |

## Files

`re/analysis/global_naming/`:
- BATCH_TEMPLATE.md
- T1_SUMMARY.md, T2_SUMMARY.md, T3_SUMMARY.md, T4_SUMMARY.md
- inventory_t1.csv (T1 flat table)
- batch_01..20_*.md (20 batches total)
- 4 reference memory entries

## Status

T5 deferred per user instruction. T1-T4 ready for the writable consolidation session whenever you choose to run it.
