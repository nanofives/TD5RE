---
batch: 09
area: track_lighting
tier: T2
target_todos: []
ghidra_session: f28b61bed3344d3a8c15abd74a3a73f4
analyzed_addresses: 0x00430150, 0x0042E130, 0x0042CE90, 0x0043DDF0, 0x0042FE20, 0x0042FFC0, 0x0043E7B0, 0x0042CDB0
agent: T2.9 (Opus 4.7 1M, ghidra_pool0)
date: 2026-05-20
---

# Globals enumeration — Track lighting family

## Summary

- Functions analyzed: 8 (per RE memo's lighting family, no callers walked outside)
- Unnamed DAT_* globals encountered: 24 (after de-dup)
- Already-named globals encountered: 4 (`g_trackStripRecords`, `g_trackVertexPool`, `g_trackEnvironmentConfig`, `s_1st_light->spacing=zero_00473b50`)
- Proposals — high confidence: 19
- Proposals — medium confidence: 5
- Proposals — comment-only (low confidence): 0

Note: the `ComputeMeshVertexLighting` address in the prompt (0x0042CFC0) was stale — current Ghidra puts it at **0x0043DDF0**. 0x0042CFC0 is inside `UpdateActiveTrackLightDirections` (0x0042CE90–0x0042D0AE). All other addresses verified.

## Methodology

Entry points: the 8 lighting-family functions named in the prompt. For each, decompiled and cross-referenced every globals read/write. Then walked the immediate callers/callees for one hop (e.g., `LoadTrackRuntimeData` at 0x0042FB90 to find the seed of `DAT_004aee14`; `UpdateActorTrackLightState` at 0x0040CD10 to confirm the per-zone field at offset +0x1c).

Relevance gate: a global is in-scope if it is written or read by any of the 8 lighting-family functions, AND its semantic role (lighting basis, ambient, zone table, enable flag, contribution scale) is established by the call graph from those 8 functions.

The 8 functions split into 4 storage clusters:
1. **Lighting basis state** (3 contribution-slot enable flags + 3 × 3-float direction vectors). Written by `SetTrackLightDirectionContribution`, read by `UpdateActiveTrackLightDirections`.
2. **Active render-globals output** (3 × 3-float final lighting basis after M^T rotation + 1 × 3-float fallback default vector). Written by `UpdateActiveTrackLightDirections`, read by `ComputeMeshVertexLighting`.
3. **Per-vertex ambient depth scalar.** Written by `ComputeAverageDepth`, read by `ComputeMeshVertexLighting`.
4. **Per-track lighting-zone table base pointer + audio-state mailbox.** Written by `LoadTrackRuntimeData` (the seed), per-actor zone reads in `ApplyTrackLightingForVehicleSegment`, and a write to `DAT_004c38a0` (audio mailbox).

## Proposals

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aafd0 | u32 | `g_trackLightSlot0Enabled` | high | Slot-0 enable flag; written 0/1 by `SetTrackLightDirectionContribution` (param_1=0) @ 0x0042E16E; read by `UpdateActiveTrackLightDirections` @ 0x0042CE90 to choose fallback vs computed basis | `td5_render.c` light-zone state (`tl_commit_to_render_globals` zero branch) |
| 0x004aafd4 | u32 | `g_trackLightSlot1Enabled` | high | Slot-1 enable flag; written 0/1 by `SetTrackLightDirectionContribution` (param_1=1); read @ 0x0042CF49 | `td5_render.c` slot-1 zero branch |
| 0x004aafd8 | u32 | `g_trackLightSlot2Enabled` | high | Slot-2 enable flag; written 0/1 by `SetTrackLightDirectionContribution` (param_1=2); read @ 0x0042CFF7 | `td5_render.c` slot-2 zero branch |
| 0x00467338 | float | `g_trackLightContribDirX[3]` (3×4 bytes from 0x467338, stride 0xC) | high | FSTP @ 0x0042E19A writes `dir_short[0] * intensity_avg * (1/1024)` to `[EAX + 0x467338]` where EAX=param_1*0xC; this is the contribution-X for slot N. Three slot entries at 0x467338, 0x467344, 0x467350 | `td5_render.c` static `s_light_contrib[3].dir.x` (port-internal name) |
| 0x0046733c | float | `g_trackLightContribDirY[3]` (component, stride 0xC) | high | FSTP @ 0x0042E1B4 writes Y-component analogously; per-slot at 0x46733c, 0x467348, 0x467354 | `td5_render.c` `s_light_contrib[*].dir.y` |
| 0x00467340 | float | `g_trackLightContribDirZ[3]` (component, stride 0xC) | high | FSTP @ 0x0042E1CE writes Z-component analogously; per-slot at 0x467340, 0x46734c, 0x467358 | `td5_render.c` `s_light_contrib[*].dir.z` |
| 0x004ab0d0 | float | `g_activeLightBasisDirX[3]` (stride 0xC, slot 0/1/2 at d0/dc/e8) | high | Written by `UpdateActiveTrackLightDirections` @ 0x0042CF1C as `dot(contrib, M^T row 0)`; read by `ComputeMeshVertexLighting` @ 0x0043DE0B as the X-coefficient of the per-vertex lighting reduction | `td5_render.c` `s_active_light[*].x` |
| 0x004ab0d4 | float | `g_activeLightBasisDirY[3]` (stride 0xC) | high | Written analogously @ 0x0042CF06 / 0x0042CFB8 / 0x0042D06A; read by `ComputeMeshVertexLighting` @ 0x0043DE14 / 0x0043DE36 / 0x0043DE5C | `td5_render.c` `s_active_light[*].y` |
| 0x004ab0d8 | float | `g_activeLightBasisDirZ[3]` (stride 0xC) | high | Written analogously @ 0x0042CF16 / 0x0042CFC8 / 0x0042D07F; read by `ComputeMeshVertexLighting` @ 0x0043DE1D / 0x0043DE43 / 0x0043DE69 | `td5_render.c` `s_active_light[*].z` |
| 0x004ab0f8 | float | `g_trackLightFallbackDirX` | high | Read when `g_trackLightSlot0Enabled == 0` @ 0x0042CF24; same fallback used for slots 1 & 2 @ 0x0042CFD6 / 0x0042D089; assigned directly into the slot-0/1/2 X-component of the active basis | none yet (port currently zeroes disabled slots in `tl_commit_to_render_globals` — see Key Discovery #4) |
| 0x004ab0fc | float | `g_trackLightFallbackDirY` | high | Read @ 0x0042CF2A / 0x0042CFDC / 0x0042D08F (same pattern, Y component) | none yet |
| 0x004ab100 | float | `g_trackLightFallbackDirZ` | high | Read @ 0x0042CF36 / 0x0042CFE8 / 0x0042D094 (same pattern, Z component) | none yet |
| 0x0045d6a0 | float (rodata) | `k_lightContribDirScale` | high | Constant `1/1024 = 0x3a800000`; multiplied into every contribution-direction FSTP at 0x0042E194 / 0x0042E1AE / 0x0042E1C8. Converts short-angle directional units (max 4096) into normalized float range (~±4.0). Const-pool — read-only | `td5_render.c` literal constant in `tl_set_contrib()` |
| 0x004bf6a8 | s32 | `g_trackLightAmbientBaseline` | high | Single scalar; written by `ComputeAverageDepth` as `(r+g+b)/3` @ 0x0043E7CE; read once by `ComputeMeshVertexLighting` @ 0x0043DE7D as the constant added to every per-vertex dot product before clamping to [0x40, 0xff] | `td5_render.c` static `s_light_ambient_baseline` (per memo equivalence "scalar ambient") |
| 0x004aee14 | u32 ptr | `g_trackLightingZoneTablePtr` | high | Seeded by `LoadTrackRuntimeData` @ 0x0042FD19 from `(&DAT_00469c78)[track_index]`; read by `ApplyTrackLightingForVehicleSegment` @ 0x00430150 (indexed by `actor->field_0x377` × 0x24) and `UpdateActorTrackLightState` @ 0x0040CDAA (reads field +0x1c of same row). Per memo: stride 0x24, ~285 zones × 42 tracks total across the per-track tables | `td5mod/src/td5re/td5_light_zones_table.inc` (zone table is statically extracted port-side; runtime pointer is per-track, mirrored in port helpers) |
| 0x00469c78 | array of 24 ptrs | `g_perTrackLightZoneTables` | high | 24-entry array at .rdata; loaded as `(&DAT_00469c78)[param_1 * 4]` in `LoadTrackRuntimeData` (param_1 = track number 1..24). Each entry points to that track's zone-row block (stride 0x24 per zone) | `td5_light_zones_table.inc` (per-track tables; port flattens) |
| 0x004c38a0 | u32[6] | `g_actorTrackZoneAudioStateCode` | high | 6 dwords (per-slot); written by `ApplyTrackLightingForVehicleSegment` @ 0x004301E2 as `*(undefined4 *)(&DAT_004c38a0 + slot*4) = *(undefined4 *)(psVar9 + 0x10)` — i.e. copies the zone row's u32 at offset +0x10 (zone-row field `slot_color` per the RE memo). Read **only by audio** (`UpdateVehicleAudioMix` @ 0x00441035 / 0x00441195) as a switch value (cases 1-5) selecting per-engine SFX volume. **Lighting writes it, audio reads it — a cross-subsystem mailbox.** | (none — port appears to not propagate this; flag for audio TODO) |

### Medium-confidence

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004ab0dc | float | `g_activeLightBasisDirX[1]` (slot 1 alias of 0x4ab0d0+12) | med | Part of the array `g_activeLightBasisDirX[3]`. Listed separately because Ghidra has independent labels for each component but they form a 3-element stride-12 array. Same pattern at 0x4ab0e8 (slot 2) | covered above |
| 0x004ab0e0 | float | `g_activeLightBasisDirY[1]` | med | Same — slot 1 of stride-12 array; written at 0x42CFB8 / 0x42CFEE; read at 0x43DE36 | covered above |
| 0x004ab0e4 | float | `g_activeLightBasisDirZ[1]` | med | Same — slot 1 of stride-12 array; written at 0x42CFC8 / 0x42CFF4 | covered above |
| 0x004ab0e8 | float | `g_activeLightBasisDirX[2]` | med | Slot 2; written @ 0x42D07F / 0x42D09A | covered above |
| 0x004ab0ec | float | `g_activeLightBasisDirY[2]` | med | Slot 2; written @ 0x42D06A / 0x42D0A0 | covered above |
| 0x004ab0f0 | float | `g_activeLightBasisDirZ[2]` | med | Slot 2; written @ 0x42D079 / 0x42D0A5 | covered above |

The above six rows are technically redundant with the array entries `g_activeLightBasisDir{X,Y,Z}[3]` already listed in high-confidence. Listed here for completeness because Ghidra currently has them as separate `DAT_*` labels and the consolidation session may choose to either (a) keep individual labels or (b) define a `float g_activeLightBasis[3][3]` struct at 0x004ab0d0. Either way, semantics are unambiguous.

## Key discoveries

1. **Lighting state is a 3-slot mini-pipeline** with explicit enable bits. Each slot has:
   - An *input* contribution (dir_xyz at 0x467338 + slot*0xC, scaled by 1/1024).
   - An *enable bit* (0x004aafd0 + slot*4) — `SetTrackLightDirectionContribution(slot, dir, 0, 0, 0)` writes 0; any non-zero RGB writes 1.
   - An *output* basis vector (0x004ab0d0 + slot*0xC), which is the contribution rotated into the actor's body frame by M^T.
   - When the enable bit is 0, the output slot inherits the fallback default direction (0x004ab0f8/fc/100). This is the *equivalent* of zeroing the slot but it actually rebrands the slot with the default direction. The port's `tl_commit_to_render_globals` zeroes disabled slots instead — this is functionally identical only when `g_trackLightFallback*` is zero, which is the steady state but **not necessarily the case** if a per-track init writes a non-zero fallback. Worth a quick port-side audit.

2. **The lighting-zone table is per-track, not global.** `g_perTrackLightZoneTables[24]` at 0x469c78 holds one pointer per track; `g_trackLightingZoneTablePtr` at 0x4aee14 is just the currently-loaded one (refreshed by `LoadTrackRuntimeData`). The port's static `td5_light_zones_table.inc` correctly flattens this — confirms the prior memo's claim that the port extraction is faithful.

3. **`DAT_004c38a0` is a lighting→audio mailbox.** This is the *most interesting* find of the batch. The lighting code writes a u32 per actor slot from zone-row offset +0x10 (described in the RE memo as `slot_color`). The only readers are in `UpdateVehicleAudioMix`: a `switch` statement with cases 1-5 mapping to engine-SFX volume codes (cases give volumes 0x5A, 0x50, 0x3C, 0x28, 0x14). So **the zone table simultaneously encodes a per-zone audio gain code** that is sampled when the actor enters the zone. Not a "color" — closer to "ambient-audio key" or "engine-volume-curve selector". Name `g_actorTrackZoneAudioStateCode` reflects the actual semantic. The memo's `slot_color` is a misnomer; consider revising the per-zone struct field name to `audio_state_code` in the port `TD5_LightZone`.

4. **The fallback default vector at 0x4ab0f8/fc/100 has no writer inside the lighting family.** I searched the entire instruction stream — no MOV/FSTP targets these three addresses outside the read sites. They are either (a) zeroed at startup and never written (memory_read shows 00), or (b) written by a non-lighting subsystem (level-config load, fog/sky setup). Worth one more agent pass to find the writer; provisionally `g_trackLightFallbackDir{X,Y,Z}` is correct but the source is TBD. Flagged as out-of-scope below.

5. **Ambient depth is a scalar, not a vector.** `g_trackLightAmbientBaseline` is `int32` not RGB. `ComputeAverageDepth` is literally `(r+g+b)/3` — the lighting pipeline reduces three byte channels to one int that is then added to every per-vertex dot product. There is no separate per-channel ambient in this codepath. Anything labelled "ambient color" elsewhere is unrelated to track lighting.

6. **`ComputeMeshVertexLighting` reads all 9 active-basis floats per vertex.** No fewer. The inner loop performs `n[0]*basis[0] + ... + n[8]*basis[8] + ambient`, where `n[0..8]` are 9 floats per vertex (3 per slot — three independent normal channels). Confirms the slot model: every mesh vertex has *three* normals, one per lighting slot, and the slots are independently lit then summed. This is non-obvious — port-side this likely fed into `td5_render_compute_vertex_lighting()` and explains the 9-component vertex-light input.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004ab0f8 / 0x004ab0fc / 0x004ab100 writer | Fallback default direction is read only in the lighting family — written by some non-lighting init (sky/fog config? level header?). Find the writer to ground the `g_trackLightFallbackDir*` rename. | scene init / track config |
| 0x0045d6a4 | Adjacent rodata constant `0x3e4ccccd = 0.2`; only read at 0x42f933 (outside lighting family). Possibly fog distance or another lighting normalization. | fog / camera distance constants |
| 0x004aee1c | Right.trk archive pointer; written by `LoadTrackRuntimeData`. Same loader, but route-table family, not lighting. | track route data |
| 0x004aed8c, 0x004aed90, 0x004aed94, 0x004aed98 | Other archive pointers (strip/left/right/traffic-bus and checkpoint-remap base) written by `LoadTrackRuntimeData`. Adjacent in memory and worth grouping in a future "track_runtime_data" batch. | track loader |
| 0x00483954 | Already named `g_currentCheckpointStripIndex` in batch_04 — saw it written by `LoadTrackRuntimeData` too. Not in lighting family but noted for consolidation cross-check. | checkpoint config (already covered) |
| 0x004a2cdc, 0x004c3770-0x004c38c8 block | Per-slot audio state codes / horn states / vehicle audio mixer dwords. Surfaced indirectly via the `0x4c38a0` mailbox. ~10 unnamed DATs in this region; ripe for a dedicated audio batch. | vehicle audio (T2/T3) |

## TODO impact

- **(no related TODO):** Lighting is not in the current TODO list. This batch is pure naming/documentation. Pay-off: future investigations of visual issues (e.g. "vehicles too dark at zone X", "shadow contribution wrong on slot 2") gain a complete state inventory and can avoid re-discovering the 3-slot pipeline.
- **Cross-link to audio:** the `g_actorTrackZoneAudioStateCode` mailbox (key discovery #3) creates a previously-undocumented coupling between lighting and audio. If a future TODO surfaces around "engine sound mode wrong in tunnels / certain zones", this batch's naming will localize the root cause to zone-row offset +0x10 quickly.
- **Port follow-up suggestion (no TODO yet):** Key discovery #1 — the port zeroes disabled lighting slots while the original rebrands them with `g_trackLightFallbackDir`. This is benign while the fallback vector is zero (steady state on the levels checked) but could diverge if a track init writes a non-zero fallback. Out-of-scope find #1 is the missing piece.
