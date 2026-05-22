---
tier: T2
date: 2026-05-20
batches: [06, 07, 08, 09, 10]
status: ready_for_consolidation
total_proposals: 126
---

# T2 Global Naming Sweep — Summary (ARCH-DIVERGENCE writers)

## Headline stats

| batch | area | functions | proposals | high | med | low |
|---|---|---|---|---|---|---|
| 06 | main_game_loop + race_init | 4 + 5 neighbors | 9 | 5 | 3 | 1 |
| 07 | frontend orchestrators | 4 | 28 | 18 | 7 | 3 |
| 08 | render HUD/wheels/minimap | 3 (3854+1410+3707 B) | 41 | 27 | 9 | 5 |
| 09 | track lighting family | 8 + 2 neighbors | 24 | 19 | 5 | 0 |
| 10 | AI offset peer + small divergences | 4 + 5 callers | 24 | 16 | 6 | 2 |
| **TOTAL** | | ~30 functions | **126** | **85** | **30** | **11** |

## Cross-cutting findings — the "lost writer" lens

### 1. T2.6 (RunMainGameLoop) — NO LOST WRITER
The 3 ARCH-DIVERGENCE memos correctly characterize their divergences:
- `RunMainGameLoop` = DLL-side substitution (DDraw → D3D11)
- `cardef per-actor indirection` = architectural addressing only
- `AccumulateVehicleSpeedBonusScore` = intentional stub gated on Ultimate variant

**Verdict**: this area is byte-faithful for every side-effect that matters. No bug-class signals.

### 2. T2.7 (Frontend) — LOST NPC PROPAGATION
The orig cheat-completion at `0x00414E26` does TWO writes:
1. XOR target dword via `g_cheatCodeTargetFlagPtrTable[i]` — **port mirrors this** ✓
2. Walk `g_npcRacerGroupTable` (stride 0xA4) and set `gNpcRacerCheatFlags[i] |= 2` / `&= 1` on every NPC with `*pcVar4 == '\0'` — **port DROPS this** ✗

Port-side: `frontend_match_cheat_code("OPENALL")` at `td5_frontend.c:2474` lacks the NPC propagation pass. Candidate root cause if cheat-mediated NPC behavior ever appears missing.

Secondary: `g_cheatCodeXorMaskTable {1, 8, 2, 1, 1, 1}` — cheat #1 toggles BIT 3, not LSB. If port models cheat #1 as a bool → `Config.td5` round-trip corruption risk.

### 3. T2.8 (HUD render) — MISNAMED + STRUCT NEEDED
Two existing named globals are misnamed:
- `g_hudFontGlyphAtlasPtr` — actually different role; needs renaming
- `g_hudFadeOverlayAlpha` — actually the **U-turn dwell counter** (port has `s_wrong_way_counter` correct, so port semantics are right; orig label is the misleading one)

Per-view HUD layout = `TD5_HudViewLayout[3]` array at `0x004b1138`, 14-float stride. Existing `g_hudScaleX/Y` are actually `[0].scale_x/scale_y` of the array. Ghidra should declare the struct.

Address overload at `0x004b1140/0x004b1178` — init writes (center_x) vs HUD-loop writes (U-turn dwell counter).

New TODOs surfaced (not yet written to memory; do at consolidation):
- `g_hudUseMetricUnits @ 0x004b11c4` — exposes missing INI setting (KPH/MPH)
- `g_hudPositionLabelTable` rename — minor cleanup

### 4. T2.9 (Track lighting) — STRUCTURE CLARIFIED
The `s_light_dir[]` referenced in the lighting arch-memo is **NOT per-actor-slot**. It's a 3-slot global pipeline at `0x00467338` (stride 0xC, 3 slots × 3 floats = 9 floats per vertex) — one global pipeline serving the currently-processed actor.

Each slot has:
- enable bit at `0x004aafd0 + slot*4`
- active-basis output at `0x004ab0d0 + slot*0xC`

`DAT_004c38a0` is a **lighting→audio mailbox**: lighting writes zone-row offset +0x10 into a per-actor dword; only readers are inside `UpdateVehicleAudioMix` as a switch selecting engine-SFX volume codes (cases 1-5 → 0x5A/0x50/0x3C/0x28/0x14). Strongly suggests TD5_LightZone field should be renamed `audio_state_code` in the port.

`ComputeMeshVertexLighting` consumes **9 normals per vertex** (3 per lighting slot), summed — explains the mesh format.

Port follow-up flagged (not a TODO yet): port's `tl_commit_to_render_globals` zeroes disabled slots, while orig rebrands them with `g_trackLightFallbackDir{X,Y,Z}` (`0x004ab0f8/fc/100`). Benign if fallback stays zero (steady-state observed), but could diverge if any track-init writes non-zero. **Fallback writer not found inside the lighting family** — out-of-scope for future batch.

### 5. T2.10 (AI offset peer) — CASCADE FOCAL POINT NAMED
**Biggest single-global win across both tiers**:

- `RS_TRACK_OFFSET_BIAS = DAT_004afb84` (RS idx 9) — **22 xrefs**, the cascade focal point from `reference_steering_cascade_root_cause_find_offset_peer.md`
- `RS_ACTIVE_UPPER_BOUND = DAT_004afbb0` (RS idx 0x14) — confirms memo
- `RS_ACTIVE_LOWER_BOUND = DAT_004afbb4` (RS idx 0x15) — confirms memo
- `g_lateralAvoidanceDirection = DAT_004b08b0`

Route-state table base: `gActorRouteStateTable @ 0x004afb60`, 12 × 0x11C struct array. All `(&DAT_004afbXX)[slot*0x47]` expressions are slot-0 aliases of per-actor RS fields. **17 RS fields named** in this batch.

Active bounds are COPIED (not computed) from per-route bound pairs at RS idx 0x10..0x13, gated by route identity at RS[0]. Identity tokens:
- `g_activeRouteTablePtrA_left @ 0x004afb58`
- `g_activeRouteTablePtrB_right @ 0x004b08b4`

Port must preserve **pointer identity** (`==` comparison), not just buffer content.

**Renaming reveal**: `RenderTrackSegmentNearActor @ 0x00433CE0` is misnamed — it's a SIM-side barycentric helper, not a render function. Suggest T3 rename to `ComputeActorTrackBarycentric`.

**Snapshot-replay harness gap surfaced**: `g_lateralAvoidanceDirection @ 0x004b08b0` is a shared flag and likely **not captured** in `fillGlobalsBlob` (first 0x60 bytes of globals snapshot). Recommend extending capture to include this address.

## Orphan ARCH-DIVERGENCE writers for future batches (T3+)

Surfaced during T2.10 analysis, not yet enumerated:
- `0x00473d9c..0x00473da8` — AI rubber-band tuning constants, rewritten per difficulty tier in `InitializeRaceActorRuntime`
- `0x00473db0` — AI carparam template base (referenced in `reference_drag_ai_template_binding.md`)
- `0x004ad288` — drag-race lane strip pointer (15-write block in `InitializeRaceActorRuntime`)
- `0x004c3da0` — strip blob alias used by junction-table walkers

## TODO impact (T2)

No NEW TODOs root-caused via T2 (T1 already closed the easy ones). T2's payoff is structural:

1. **Naming the cascade focal points** (`RS_TRACK_OFFSET_BIAS`, `RS_ACTIVE_*`) makes the long-pending cascade investigation in `todo_cascade_unwind_2026-05-17` newly tractable — instead of grepping `DAT_004afb84` you can grep the actual name.

2. **HUD units TODO surfaced** — `g_hudUseMetricUnits @ 0x004b11c4` should be wired to a new INI setting `[Display] SpeedUnits` (currently the INI has `SpeedUnits = 1` but it's not actually wired through).

3. **Cheat-code NPC propagation gap** — flag as candidate root cause if cheat-mediated NPC behavior is reported missing in future.

4. **Track-lighting fallback dir writer** — out-of-scope here; flag for a T3 batch focused on lighting init paths.

## Surfaced future cleanups (beyond global-naming scope)

- Promote `gVehiclePhysicsTable` and `gVehicleTuningTable` to proper Ghidra struct array types → eliminates 38 fake-DAT_ labels (14 + 24) that are really just operand-encoding artifacts.

## Files

- `re/analysis/global_naming/batch_06_main_game_loop.md` (114 lines, 9 globals)
- `re/analysis/global_naming/batch_07_frontend_orchestrators.md` (164 lines, 28 globals)
- `re/analysis/global_naming/batch_08_render_hud_wheels.md` (207 lines, 41 globals)
- `re/analysis/global_naming/batch_09_track_lighting.md` (105 lines, 24 globals)
- `re/analysis/global_naming/batch_10_small_divergences.md` (142 lines, 24 globals)

## Inventory CSV

Deferred — will be generated by a script at consolidation time across all tiers (T1+T2 = 163 globals, plus T3+T4+T5 incoming).
