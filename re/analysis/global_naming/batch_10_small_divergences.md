---
batch: 10
area: small_divergences_ai_track
tier: T2
target_todos: [todo_cascade_unwind_2026-05-17, reference_steering_cascade_root_cause_find_offset_peer, reference_arch_find_offset_peer_return_minus_one, reference_arch_recycle_heading_collapse]
ghidra_session: 4bf3b142fb97451796b8eff4f7ede736
analyzed_addresses: 0x004337E0, 0x00433CE0, 0x00434350, 0x00434900, 0x00432E60, 0x0042FB90, 0x00436A70, 0x004366E0, 0x00444070
agent: Claude Opus 4.7 (1M)
date: 2026-05-20
---

# Globals enumeration — Small AI/track divergences (catch-all for ARCH-DIVERGENCE writers)

## Summary

- Functions analyzed: 9 (4 primary T2.10 targets + 5 callers/initializers reached via xref walk)
- Unnamed DAT_* globals encountered: 24 (after de-dup)
- Already-named globals encountered: 8 (`gActorRouteStateTable`, `gActorRouteTableSelector`, `gActorForwardTrackComponent`, `gActorTrackSpanProgress`, `g_actorRuntimeState`, `g_racerCount`, `g_trackStripRecords`, `g_trackVertexPool`)
- Proposals — high confidence: 16
- Proposals — medium confidence: 6
- Proposals — comment-only (low confidence): 2

## Methodology

Entry points: `FindActorTrackOffsetPeer @ 0x004337E0`, `RenderTrackSegmentNearActor @ 0x00433CE0`, `InitializeActorTrackPose @ 0x00434350`, `UpdateActorTrackOffsetBias @ 0x00434900`. From these I walked one level of callers (`InitializeRaceActorRuntime @ 0x00432E60`, `LoadTrackRuntimeData @ 0x0042FB90`, `UpdateRaceActors @ 0x00436A70`, `UpdateActorTrackBehavior @ 0x00434FE0`, `BindTrackStripRuntimePointers @ 0x00444070`, `UpdateActorTrackBounds @ 0x004366E0`) to recover writer sites for every `DAT_*` read inside the four primaries.

Three structural insights drove the relevance gate:
1. The per-actor **route state table** is a flat array of 12 records × 0x11C (284) bytes at base `gActorRouteStateTable @ 0x004afb60`. Almost every `(&DAT_004afbXX)[slot * 0x47]` expression is a slot-0 alias of an RS field at offset `(DAT_004afbXX - 0x004afb60)`. I enumerated each unique offset reached and proposed an RS field name (semantic role).
2. **Two active route-table pointers** (`DAT_004afb58`, `DAT_004b08b4`) are non-RS globals that get swapped per-actor between the LEFT.TRK and RIGHT.TRK route blobs loaded by `LoadTrackRuntimeData`. They are essentially the runtime "current route" identity pair.
3. **`DAT_004b08b0`** is the `g_lateral_avoidance_direction` flagged in `reference_steering_cascade_root_cause_find_offset_peer.md`. Confirmed: written ONLY by `FindActorTrackOffsetPeer` peer-success paths and the `UpdateActorTrackOffsetBias` reactive flip. Read solely by the bias-relax/peer-push split.

## Proposals

### Route-state-table per-slot field aliases (RS index = (addr - 0x004afb60) / 4)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004afb74 | int[12] | `g_rs_probe_span_progress_min_slot0` (RS idx 5) | high | `UpdateActorTrackBounds @ 0x004366E0` writes `min(probe[0..3].span_progress)` at `+slot*0x11C`; read by `ComputeSignedTrackOffset` callers to anchor RIGHT/LEFT lateral offsets | td5_ai.c / td5_track.c probe-span-progress min |
| 0x004afb78 | int[12] | `g_rs_probe_span_progress_max_slot0` (RS idx 6) | high | Twin of 0x004afb74; stores max(probe[0..3].span_progress) | td5_ai.c probe-span-progress max |
| 0x004afb7c | int[12] | `g_rs_probe_span_progress_rear_slot0` (RS idx 7) | high | `UpdateActorTrackBounds` copies probe[1] (rear-axle) progress here for bound math | td5_ai.c probe-rear-axle-progress |
| 0x004afb80 | int[12] | `g_rs_probe_span_progress_front_slot0` (RS idx 8) | high | Twin of 0x004afb7c; copies probe[0] (front-axle) progress | td5_ai.c probe-front-axle-progress |
| 0x004afb84 | int[12] | `g_rs_track_offset_bias_slot0` (RS idx 9) | **high** | This is `RS_TRACK_OFFSET_BIAS` from cascade root-cause memo. Written by peer-relax/push in `UpdateActorTrackOffsetBias`; written by classify clamps in `UpdateRaceActors @ 0x00436A70`; read everywhere downstream as the lateral steering bias seed. **22 xrefs.** | td5_ai.c:1869+ port mirror |
| 0x004afb88 | int[12] | `g_rs_probe_progress_slot0_p0` (RS idx 10) | high | First of 4-int probe-progress array; `piVar1 = (int *)(&DAT_004afb88 + iVar8)` then 4-iteration loop in `UpdateActorTrackBounds` | td5_ai.c probe[0] progress |
| 0x004afb8c | int[12] | `g_rs_probe_progress_slot0_p1` (RS idx 11) | high | Second probe (rear-left axle); same array | td5_ai.c probe[1] progress |
| 0x004afb90 | int[12] | `g_rs_probe_progress_slot0_p2` (RS idx 12) | high | Third probe; same array | td5_ai.c probe[2] progress |
| 0x004afb94 | int[12] | `g_rs_probe_progress_slot0_p3` (RS idx 13) | high | Fourth probe; same array | td5_ai.c probe[3] progress |
| 0x004afb98 | int[12] | `g_rs_track_offset_for_left_route_slot0` (RS idx 14) | high | Written in `UpdateRaceActors` LEFT branch (route == DAT_004afb58 path); cached signed offset wrt LEFT.TRK | td5_ai.c LEFT_TRACK_OFFSET |
| 0x004afb9c | int[12] | `g_rs_track_offset_for_right_route_slot0` (RS idx 15) | high | Twin of 0x004afb98; cached signed offset wrt RIGHT.TRK | td5_ai.c RIGHT_TRACK_OFFSET |
| 0x004afba0 | int[12] | `g_rs_left_bound_upper_slot0` (RS idx 16) | high | `UpdateActorTrackBounds` writes `ComputeSignedTrackOffset(min_progress, LEFT_TBL)`; copied into RS_ACTIVE_UPPER on LEFT route | td5_ai.c LEFT_BOUND_HI |
| 0x004afba4 | int[12] | `g_rs_left_bound_lower_slot0` (RS idx 17) | high | Twin of 0x004afba0; written from `max_progress + LEFT` | td5_ai.c LEFT_BOUND_LO |
| 0x004afba8 | int[12] | `g_rs_right_bound_upper_slot0` (RS idx 18) | high | Mirror for RIGHT route; `min_progress + RIGHT_TBL` | td5_ai.c RIGHT_BOUND_HI |
| 0x004afbac | int[12] | `g_rs_right_bound_lower_slot0` (RS idx 19) | high | Mirror for RIGHT route; `max_progress + RIGHT_TBL` | td5_ai.c RIGHT_BOUND_LO |
| 0x004afbb0 | int[12] | `g_rs_active_upper_bound_slot0` (RS idx 20) | **high** | This is **`RS_ACTIVE_UPPER_BOUND`** named in cascade memo. `find_offset_peer` reads it as `piVar11[0x14]` gate. Written by RS_LEFT_BOUND_LOWER copy on LEFT, RS_RIGHT_BOUND_LOWER copy on RIGHT in `UpdateRaceActors` pre-loop. Index 0x14 = byte offset 0x50. | td5_ai.c RS_ACTIVE_UPPER_BOUND (per memo) |
| 0x004afbb4 | int[12] | `g_rs_active_lower_bound_slot0` (RS idx 21) | **high** | This is **`RS_ACTIVE_LOWER_BOUND`** named in cascade memo. `find_offset_peer` reads it as `piVar11[0x15]` gate. Written by RS_LEFT_BOUND_UPPER copy on LEFT, RS_RIGHT_BOUND_UPPER copy on RIGHT. Index 0x15 = byte offset 0x54. | td5_ai.c RS_ACTIVE_LOWER_BOUND (per memo) |

### Active route identity (LEFT/RIGHT runtime pointers and lateral avoidance)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004afb58 | void* | `g_activeRouteTablePtrA_left` | high | `InitializeRaceActorRuntime` writes `DAT_004afb58 = DAT_004aed94` (LEFT.TRK buffer). Per-actor RS[0] is initialized to this for even slots. Acts as identity-key for "which route are we on now" in `find_offset_peer` and `UpdateActorTrackOffsetBias`. 13 xrefs. | td5_ai.c left-route-pointer global |
| 0x004b08b4 | void* | `g_activeRouteTablePtrB_right` | high | Symmetric to `g_activeRouteTablePtrA_left`. Writes `DAT_004b08b4 = DAT_004aee1c` (RIGHT.TRK buffer); used for odd slots and as the "other route" pointer that RS[0] gets swapped to on route changes. 11 xrefs. | td5_ai.c right-route-pointer global |
| 0x004b08b0 | u32 (bool) | `g_lateralAvoidanceDirection` | **high** | Per the cascade root-cause memo. Set in `find_offset_peer` peer-success final block (`(int)(abs(uVar2)) <= (int)(abs(uVar1))`); flipped reactively in `UpdateActorTrackOffsetBias` when the chosen peer's track-contact-flag transitions. 5 xrefs. Confirmed as the `DAT_004b08b0` flagged in `reference_steering_cascade_root_cause_find_offset_peer.md` candidate (d). | td5_ai.c `g_lateral_avoidance_direction` |

### Track-data archive blob pointers (initialized by `LoadTrackRuntimeData`)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004aed90 | void* | `g_trackStripDataBlob` | high | `LoadTrackRuntimeData` allocates and reads STRIP.DAT here; bound to runtime pointers by `BindTrackStripRuntimePointers @ 0x00444070` (which writes `g_trackStripRecords`, `g_trackVertexPool`, `g_trackStripAttributeBasePtr`) | td5_track.c STRIP.DAT loader |
| 0x004aed94 | void* | `g_leftRouteTableBlob` | high | `LoadTrackRuntimeData` reads LEFT.TRK into this allocation (`&PTR_s_LEFT_TRK_004673b8`); seeds `g_activeRouteTablePtrA_left` (0x004afb58) | td5_ai.c LEFT.TRK buffer |
| 0x004aee1c | void* | `g_rightRouteTableBlob` | high | `LoadTrackRuntimeData` reads RIGHT.TRK (`&PTR_s_RIGHT_TRK_004673bc`); seeds `g_activeRouteTablePtrB_right` (0x004b08b4) | td5_ai.c RIGHT.TRK buffer |
| 0x004aed8c | void* | `g_trafficBusTableBlob` | high | `LoadTrackRuntimeData` reads TRAFFIC.BUS (`&PTR_s_TRAFFIC_BUS_004673c0`); seeds `g_activeTrafficBusCursor` (0x004b08b8) | td5_ai.c TRAFFIC.BUS table |
| 0x004aedb0 | byte[0x60] | `g_checkpointNumTable` | med | `LoadTrackRuntimeData` reads CHECKPT.NUM (0x60 bytes) directly into this address. Referenced by checkpoint logic (out of scope). Memo `todo_drag_checkpt_num_sentinel` already mentions this archive — confirm name candidate. | td5_track.c CHECKPT.NUM |
| 0x004aee14 | u32 | `g_trackCircuitFlagOrLapCount` | med | `LoadTrackRuntimeData` writes `*(DAT_00469c78 + param_1 * 4)` here (per-track lookup). Read by 0x0040cdaa and 0x00430150. Likely lap/checkpoint config. Comment-flag for verification. | (none) |
| 0x004b08b8 | void* | `g_activeTrafficBusCursor` | high | `InitializeRaceActorRuntime` initializes `DAT_004b08b8 = DAT_004aed8c` (TRAFFIC.BUS blob ptr). Then advanced/rewound by `RecycleTrafficActorFromQueue @ 0x004353B0` reads + writes. Functions as the active "next traffic entry" cursor. 11 xrefs. | td5_ai.c traffic-recycle cursor |

### Track strip runtime aliases / per-track misc

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x004c3da0 | void* | `g_trackStripBlobAlias` | med | `BindTrackStripRuntimePointers` writes `DAT_004c3da0 = DAT_004aed90` (alias of strip-blob ptr). Read by junction-table walkers (`+0x14` = count, `+0x1c` = first entry stride 6 — junction/branch table). Wrap of STRIP header. 16 xrefs. | td5_track.c strip-blob alias |
| 0x004ad288 | void* | `g_dragRaceLaneStripPtr` | med | `InitializeRaceActorRuntime` writes 15 short writes at `*(DAT_004ad288 + 0x40..0x84)` — drag race lane geometry init (gates only run in `g_selectedGameType != 0` paths visible elsewhere). 15 xrefs. | td5_physics.c drag-strip block |
| 0x00473d2c | int[6] | `g_defaultCarIdTable` | high | `InitializeRaceActorRuntime` writes `local_4[4] = (&DAT_00473d2c)[local_c]` per-slot — seeds the per-slot car id from a constant table at slot init time. Value at 0x00473d2c = 0x00000100 (typical default carparam id) | td5_save.c default car table |
| 0x00483550 | u32 | `g_trackCameraConfigSecondary` | low | `LoadTrackRuntimeData` writes from `&DAT_00473824 + (param_1-1) * 8` per-track lookup. Used outside scope; comment-flag only. | (none) |
| 0x00483954 | u32 | `g_trackCameraConfigPrimary` | low | Twin of 0x00483550, writes from `&DAT_00473820 + (param_1-1) * 8`; same per-track lookup pair. Comment-flag only. | (none) |
| 0x004aed98 | int[6] | `g_raceCheckpointTableLive` | high | `LoadTrackRuntimeData` copies 6 dwords from `&DAT_0046cf6c + param_1*4`-pointed array into `&DAT_004aed98`. Address is then assigned to the already-named `g_raceCheckpointTablePtr`. This is the back-store for the checkpoint table. | (none) |

## Key discoveries

1. **Route-state-table is a 12 × 0x11C-byte struct array** anchored at `gActorRouteStateTable @ 0x004afb60`. The first 12 slots map 1:1 to actor slots 0..11 (racers 0..5, traffic 6..11). The stride 0x47 dwords = 284 bytes = 0x11C. Every `(&DAT_004afbXX)[slot * 0x47]` expression in `td5_ai.c` is an alias of the field at offset `(DAT_004afbXX - 0x004afb60)` in slot N. Naming each RS field at slot 0 directly recovers the field semantic without requiring a struct rewrite in Ghidra.

2. **`RS_TRACK_OFFSET_BIAS` (RS idx 9, addr 0x004afb84) is the cascade focal point.** 22 xrefs span peer-relax (`UpdateActorTrackOffsetBias`), classify clamps (`UpdateRaceActors @ 0x00436A70` pre-loop), and the spawn-time init (`InitializeActorTrackPose @ 0x00434350` final write). The cascade root-cause memo's "+27 delta at sub-tick 0" is literally a write to this single field by `UpdateActorTrackOffsetBias`. **Naming this single global is the single biggest readability win in the cascade investigation.**

3. **`RS_ACTIVE_UPPER_BOUND` (RS idx 0x14, addr 0x004afbb0) and `RS_ACTIVE_LOWER_BOUND` (RS idx 0x15, addr 0x004afbb4) names from the cascade memo are confirmed.** They are NOT computed in-place — they're populated by COPYING from one of two precomputed bound-pairs (RS[0x10..0x11] for LEFT route, RS[0x12..0x13] for RIGHT route) inside `UpdateRaceActors` based on the current `RS[0]` route identity. The pre-loop sequence is:
   - `UpdateActorTrackBounds` writes RS[0x10..0x13] (LEFT_BOUND_HI/LO + RIGHT_BOUND_HI/LO from probe min/max progress)
   - Then `UpdateRaceActors` branch on `RS[0] == g_activeRouteTablePtrA_left` selects which pair gets copied into RS[0x14] (ACTIVE_UPPER) and RS[0x15] (ACTIVE_LOWER)
   - So if `UpdateActorTrackBounds` is skipped OR the pre-loop's classify-clamp path doesn't run, RS[0x14] and RS[0x15] stay zero — exactly the "uninitialized in port" symptom described in the cascade memo.

4. **`g_lateralAvoidanceDirection` (DAT_004b08b0) is a 2-state shared flag, not per-actor.** Set inside `find_offset_peer` based on a per-peer geometric comparison (`abs(peer_upper - cardef_mid + offset)` vs `abs(peer_lower - cardef_mid + offset)`), then read by `UpdateActorTrackOffsetBias` to decide whether the next peer-interaction is a push (state==0) or a duck-under (state==1). This shared-flag design means peer iteration order in `find_offset_peer`'s Pass-1 vs Pass-2 affects the eventual bias trajectory of EVERY slot. Candidate (d) in the cascade memo's 2026-05-18 conclusion is correct: a snapshot-replay harness that doesn't capture this flag will produce divergent peer-interaction outcomes even with identical actor state. **Recommend extending snapshot capture to include DAT_004b08b0 in the globals blob.**

5. **`g_activeRouteTablePtrA_left` (0x004afb58) and `g_activeRouteTablePtrB_right` (0x004b08b4) are identity tokens, not just pointers.** They are compared with `RS[0]` ==-checks throughout the route-state machine to decide LEFT vs RIGHT branching. Two consequences:
   - The port MUST preserve pointer identity (not just byte-equality of the buffer contents) for these comparisons to behave correctly.
   - The "route swap" inside `find_offset_peer` (lines `if (*piVar3 == DAT_004afb58)`) is a simple toggle: `RS[0] = (RS[0] == LEFT) ? RIGHT : LEFT`. The companion writes `RS[9] = RS[0xE]` or `RS[0xF]` to swap in the per-route saved bias.

6. **`InitializeActorTrackPose @ 0x00434350` (heading-from-vertices) is byte-equivalent to `td5_track_compute_heading`.** The case dispatch (1/2/5, 3/4, 6/7, default) uses the IDENTICAL vertex offsets and signed div-by-4 rounding present in the port helper at `td5_track.c:3139`. This confirms the `reference_arch_recycle_heading_collapse.md` audit — the port's helper-collapse is byte-faithful.

7. **`RenderTrackSegmentNearActor @ 0x00433CE0` is misnamed (it's NOT a render function).** It computes barycentric coordinates `(s, t)` from the actor's interpolated position relative to span-quad edges, writing the result to `param_1 + 0xdc` and `param_1 + 0xe0`. `param_1` here is the same RS pointer + offset 0xdc/0xe0 = RS indices 0x37/0x38. The function is SIM-side, called inside the racer hot-loop of `UpdateRaceActors` between the per-slot pre-bounds and `UpdateActorTrackBounds`. **Suggest rename to `ComputeActorTrackBarycentric` in a future T3 cleanup pass** (out of scope for naming-only batch).

8. **`UpdateActorTrackOffsetBias @ 0x00434900` uses `peer == self_slot` as the no-peer sentinel, NOT `peer < 0`.** Confirmed against `reference_arch_find_offset_peer_return_minus_one.md` — the original returns `self_slot` (piVar3[0x35]) on no-peer-found, and the caller does `if (piVar1 == (int *)slotIndex)` to detect it. The port's `-1` return is a deliberate ARCH-DIVERGENCE.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x004afb88..0x004afb94 | RS probe progress array (4 ints) — labeled here for completeness but accessed by `UpdateActorTrackBounds` not the 4 primary targets | T2 collision/probe batch (already-covered if any) |
| 0x004afb74..0x004afb80 | RS bound min/max/front/rear progress quartet — same caveat | T2 collision/probe batch |
| 0x00483550, 0x00483954 | Per-track camera/config pointers seeded by `LoadTrackRuntimeData` from constant tables `DAT_00473820`/`DAT_00473824` | T3 camera config batch |
| 0x004aedb0 | CHECKPT.NUM live buffer | T2 checkpoint batch (touched by batch_04) |
| 0x004aee14 | Per-track config dword (lap count or circuit flag) | T3 track-config batch |
| 0x004ad288 | Drag-race lane strip pointer block | T3 drag-mode batch |
| 0x00473d2c | Default car-id constant table | T3 frontend/car-select batch |
| 0x0046cf6c | Per-track checkpoint table array (read by `LoadTrackRuntimeData`) | T3 track-data batch |
| 0x00469c78 | Per-track circuit-flag constant table | T3 track-data batch |
| 0x00473820, 0x00473824 | Per-track camera config constant tables | T3 camera batch |
| 0x00473d9c, 0x00473da0, 0x00473da4, 0x00473da8 | AI rubber-band tuning constants (rewritten by `InitializeRaceActorRuntime` per difficulty tier) | T3 AI-tuning batch |
| 0x00473db0 | AI carparam template base (32 dwords copied in `InitializeRaceActorRuntime`) | T3 AI-tuning batch — referenced in `reference_drag_ai_template_binding.md` |
| 0x004b08b4 + 0x4 ... 0x004b08C8 area | Unanalyzed neighbors of the active-route-pointer pair; cluster suggests grouped state | T3 catch-all |

## TODO impact

**todo_cascade_unwind_2026-05-17:** Naming `g_rs_track_offset_bias_slot0` (0x004afb84), `g_rs_active_upper_bound_slot0` (0x004afbb0), `g_rs_active_lower_bound_slot0` (0x004afbb4), `g_lateralAvoidanceDirection` (0x004b08b0), `g_activeRouteTablePtrA_left` (0x004afb58), and `g_activeRouteTablePtrB_right` (0x004b08b4) makes the cascade-unwind investigation drastically more readable in Ghidra. The bias-clamp pre-loop in `UpdateRaceActors @ 0x00436A70` becomes traceable end-to-end. **Does NOT close the cascade investigation** (that's a multi-session unwind per the memo), but lowers the per-session cognitive overhead substantially.

**reference_steering_cascade_root_cause_find_offset_peer:** This investigation confirms the memo's identification of `RS_ACTIVE_UPPER_BOUND` / `RS_ACTIVE_LOWER_BOUND` at the named offsets (0x50, 0x54 from RS base), confirms `g_lateral_avoidance_direction` at 0x004b08b0 is correctly identified as the shared flag candidate (d), and adds the structural insight that `RS_ACTIVE_*` is COPIED (not computed) from per-route bound-pairs at indices 0x10..0x13 (LEFT) or 0x12..0x13 (RIGHT). **The 5-hypothesis next-session Frida script in the memo should be extended to capture the LEFT/RIGHT bound pair writers (in `UpdateActorTrackBounds`) alongside the ACTIVE writers** — divergence may already exist upstream of the swap.

**reference_arch_find_offset_peer_return_minus_one:** Read-only audit confirms the original returns `piVar3[0x35]` (self_slot) on no-peer, and `UpdateActorTrackOffsetBias` uses `piVar1 == (int *)slotIndex` to detect. Port's `-1` adapter is correctly deliberate. No naming change needed.

**reference_arch_recycle_heading_collapse:** Read-only audit confirms `InitializeActorTrackPose @ 0x00434350`'s case dispatch (1/2/5, 3/4, 6/7, default) uses identical vertex math to `td5_track_compute_heading`. The +0x290 side-effect noted in the memo is NOT done by `InitializeActorTrackPose` itself — it must be in `RecycleTrafficActorFromQueue @ 0x004353B0`'s inline copies. Recycle-inline-vs-helper +0x290 audit remains a valid TODO.

## Ghidra session notes

- Session 4bf3b142fb97451796b8eff4f7ede736 opened TD5_pool0 read-only as required.
- Pool slot acquired via `bash scripts/ghidra_pool.sh acquire`; released via `bash scripts/ghidra_pool.sh cleanup` after analysis.
- No writes to Ghidra performed. Names listed here are PROPOSED only — the consolidation session will apply them.
