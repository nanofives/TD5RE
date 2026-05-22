---
batch: 54
area: wave2b_audit_comment_sync
tier: T6
target_todos: []
ghidra_session: (per-batch, ghidra-apply opens master)
analyzed_addresses: 0x0042e2e0,0x0042e3d0,0x0042e560,0x0042ed50,0x0042edf0,0x0042eea0,0x0042f010,0x0042f030,0x0042f140,0x0042f5b0,0x0042fad0,0x0042fb40,0x0042fe20,0x0042ffc0,0x00430150,0x00430cb0,0x00431190,0x00431260,0x004312e0,0x00431340,0x00431750,0x004317f0,0x004323d0,0x004326d0,0x004329e0,0x00432ab0,0x00433832,0x00433851,0x00433aa5,0x00433ac7,0x00433c92,0x00433fc0,0x00434040,0x004340c0,0x004342e0,0x00434327,0x00434390,0x00434408,0x00434410,0x00434450,0x00434501,0x004345b0,0x004345e0,0x00434631,0x0043465d,0x00434670,0x004346f5,0x00434740,0x0043483d,0x00434ba0,0x00434fe0,0x00434ffb,0x00435218,0x00435352,0x00435392,0x0043539f,0x004353b0,0x00435940,0x00435e80,0x00435f6a
agent: claude-opus-4-7
date: 2026-05-22
---

# Wave 2B audit-comment sync — batch 54 (60 addresses)

## Summary

- Addresses in this batch: 60
- All proposals are confidence=low (comment-only, no rename)
- Each address receives a consolidated PLATE comment derived from port-source audit headers

## Methodology

Pure derived from `re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv` —
port [CONFIRMED @ ...] / [ARCH-DIVERGENCE: ...] audit-header references extracted
and deduped per address. Comment text combines unique header summaries.

## Proposals (functions)

| address | current_name | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0042e2e0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042E2E0] ConvertFloatVec3ToShortAngles Y output: Y = v[0]*M[+0xC] + v[1]*M[+0x10] + v[2]*M[+0x14] | td5_physics.c:4520 |
| 0x0042e3d0 | _unchanged_ | _unchanged_ | low | 0x0042E3D0  TransformShortVectorToView         [ARCH-DIVERGENCE: MeshXform] 0x0042E560  TransformTriangleByRenderMatrix    [ARCH-DIVERGENCE: MeshXform | td5_render.c:5999 |
| 0x0042e560 | _unchanged_ | _unchanged_ | low | 0x0042E560  TransformTriangleByRenderMatrix    [ARCH-DIVERGENCE: MeshXform] 0x0043DEC0  ApplyMeshProjectionEffect          [ARCH-DIVERGENCE: MeshXform | td5_render.c:6000 |
| 0x0042ed50 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042ED50] Byte-faithful with orig UpdateVehicleEngineSpeedSmoothed. | td5_physics.c:7922 |
| 0x0042edf0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042EDF0] Byte-faithful with orig UpdateEngineSpeedAccumulator. L5 promotion 2026-05-18 (small-tier sweep). Line-for-line listing port; | td5_physics.c:8048 |
| 0x0042eea0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042EEA0] Byte-faithful with orig ApplySteeringTorqueToWheels. L5 promotion 2026-05-18 (small-tier sweep). Verbatim port of 24-instr li | td5_physics.c:8524 |
| 0x0042f010 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042F010] Byte-faithful with orig ApplyReverseGearThrottleSign. L5 promotion 2026-05-18 (small-tier sweep). 8-instr listing match; | td5_physics.c:8612 |
| 0x0042f030 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042F030] Byte-faithful with orig ComputeDriveTorqueFromGearCurve. | td5_physics.c:8435 |
| 0x0042f140 | _unchanged_ | _unchanged_ | low | actor->throttle_input_active = 1;  /* auto gearbox [CONFIRMED @ 0x42F140] */ actor->frame_counter = 0; | td5_physics.c:9148 |
| 0x0042f5b0 | _unchanged_ | _unchanged_ | low | actor->race_position = 0;  /* +0x383 stays 0 until UpdateRaceOrder writes at sim_tick>=1 [CONFIRMED @ 0x0042F5B0] */ | td5_physics.c:9154 |
| 0x0042fad0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042FAD0] Byte-faithful with orig InitializeTrackStripMetadata. | td5_track.c:5637 |
| 0x0042fb40 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0042FB40] L5 promotion sweep audit (2026-05-18). Byte-faithful port of orig fill loop: iterates s_span_count, consumes (span_idx, val) | [ARCH-DIVERGENCE @ 0x0042FB40] Orig signature is ApplyTrackStripAttributeOverrides(track_idx, default_attr) and seeds | td5_track.c:2008 |
| 0x0042fe20 | _unchanged_ | _unchanged_ | low | 0x0042FE20  BlendTrackLightEntryFromStart  [ARCH-DIVERGENCE: TrackLight] 0x0042FFC0  BlendTrackLightEntryFromEnd    [ARCH-DIVERGENCE: TrackLight] | td5_render.c:6035 |
| 0x0042ffc0 | _unchanged_ | _unchanged_ | low | 0x0042FFC0  BlendTrackLightEntryFromEnd    [ARCH-DIVERGENCE: TrackLight] | td5_render.c:6036 |
| 0x00430150 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00430150] ApplyTrackLightingForVehicleSegment. L5 audit 2026-05-18 (TD5_pool0 read-only). | td5_render.c:3483 |
| 0x00430cb0 | _unchanged_ | _unchanged_ | low | ResetGameHeap @ 0x00430cb0 [ARCH-DIVERGENCE] Orig: HeapDestroy gGameHeapHandle (gated on first-call sentinel | td5_game.c:210 |
| 0x00431190 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE @ 0x00431190] L5 promotion sweep audit (2026-05-18). Orig assumes a single fixed layout: DWORD[0]=entry_count, | td5_track.c:5117 |
| 0x00431260 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE @ 0x00431260] Orig is a 13-byte direct LUT lookup: return *(uint*)(gModelsDatEntryTable + idx*8). Port absorbs caller | td5_track.c:5960 |
| 0x004312e0 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: struct-pool vs raw-byte linked-list; L5 sweep 2026-05-21] Mirrors InitializeTranslucentPrimitivePipeline @ 0x004312E0. Orig builds t | td5_render.c:2441 |
| 0x00431340 | _unchanged_ | _unchanged_ | low | 0x00431340  FlushQueuedTranslucentPrimitives [ARCH-DIVERGENCE: D3D Pipeline] 0x00431750  EmitTranslucentTriangleStrip     [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6071 |
| 0x00431750 | _unchanged_ | _unchanged_ | low | 0x00431750  EmitTranslucentTriangleStrip     [ARCH-DIVERGENCE: D3D Pipeline] 0x0043DCB0  TransformAndQueueTranslucentMesh [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6072 |
| 0x004317f0 | _unchanged_ | _unchanged_ | low | 0x004317F0  ClipAndSubmitProjectedPolygon    [ARCH-DIVERGENCE: D3D Pipeline] 0x004323D0  RenderTrackSegmentBatch          [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6066 |
| 0x004323d0 | _unchanged_ | _unchanged_ | low | 0x004323D0  RenderTrackSegmentBatch          [ARCH-DIVERGENCE: D3D Pipeline] 0x004326D0  RenderTrackSegmentBatchVariant   [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6067 |
| 0x004326d0 | _unchanged_ | _unchanged_ | low | 0x004326D0  RenderTrackSegmentBatchVariant   [ARCH-DIVERGENCE: D3D Pipeline] 0x00432AB0  AppendClippedPolygonTriangleFan  [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6068 |
| 0x004329e0 | _unchanged_ | _unchanged_ | low | 0x004329E0  FlushImmediateDrawPrimitiveBatch [ARCH-DIVERGENCE: D3D Pipeline] 0x00431340  FlushQueuedTranslucentPrimitives [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6070 |
| 0x00432ab0 | _unchanged_ | _unchanged_ | low | 0x00432AB0  AppendClippedPolygonTriangleFan  [ARCH-DIVERGENCE: D3D Pipeline] 0x004329E0  FlushImmediateDrawPrimitiveBatch [ARCH-DIVERGENCE: D3D Pipeli | td5_render.c:6069 |
| 0x00433832 | _unchanged_ | _unchanged_ | low | Route-id match: rs[3] [CONFIRMED @ 0x00433832] */ if (route_state_ptr[3] != peer_rs[3]) continue; | td5_ai.c:2363 |
| 0x00433851 | _unchanged_ | _unchanged_ | low | self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433851] */ if (self_field82 > peer_field82) continue; | td5_ai.c:2365 |
| 0x00433aa5 | _unchanged_ | _unchanged_ | low | rs[3] route-id match [CONFIRMED @ 0x00433AA5] */ if (route_state_ptr[3] != peer_rs[3]) continue; | td5_ai.c:2504 |
| 0x00433ac7 | _unchanged_ | _unchanged_ | low | self.field_0x82 <= peer.field_0x82 [CONFIRMED @ 0x00433AC7] */ if (self_field82 > peer_field82) continue; | td5_ai.c:2508 |
| 0x00433c92 | _unchanged_ | _unchanged_ | low | Bounds source [CONFIRMED @ 0x00433C92]: cardef pointer comes from g_actorRuntimeState.slot[self_slot].field_0x1b8 (SELF's cardef), NOT | td5_ai.c:2207 |
| 0x00433fc0 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE @ 0x00433FC0] Orig calls AngleFromVector12 (0x0040A720) which indexes a 12-bit atan2 LUT at DAT_00463214. Port uses libm | [ARCH-DIVERGENCE @ 0x00433FC0] Orig dispatches into AngleFromVector12 (0x0040A720) which indexes a precomputed 12-bit atan2 LUT at | td5_ai.c:359 |
| 0x00434040 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00434040] */ uint32_t td5_compute_heading_delta(void *route_entry) { | slot = rs[0x35];   /* RS_SLOT_INDEX [CONFIRMED @ 0x00434040] */ actor = (char *)(uintptr_t)(uint32_t)g_actorBaseAddr + | actor_yaw  = *(int32_t *)(actor + 0x1F4); /* yaw accumulator [CONFIRMED @ 0x00434040] */ | td5_camera.c:2606 |
| 0x004340c0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004340C0] Byte-faithful with orig UpdateActorSteeringBias. L5 audit 2026-05-18 (TD5_pool0 read-only): | td5_ai.c:1839 |
| 0x004342e0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004342E0] ======================================================================== */ | td5_ai.c:2821 |
| 0x00434327 | _unchanged_ | _unchanged_ | low | Route byte from route table at span_norm * 3 [CONFIRMED @ 0x00434327]. | td5_ai.c:2842 |
| 0x00434390 | _unchanged_ | _unchanged_ | low | 1. Sub-lane NOT added to vertex indices [CONFIRMED @ 0x434390] 2. Case 3/4: uses right+2 vertex [CONFIRMED @ 0x434410] | ushort[3] = right_vertex_index (+6). [CONFIRMED @ 0x00434390] 3. Vertex pool: entry i = short[3]{x,y,z} at +i*6 bytes. | Base vertices — sub_lane NOT added [CONFIRMED @ 0x00434390] */ vl0 = vertex_at(sp->left_vertex_index); | td5_track.c:3668 |
| 0x00434408 | _unchanged_ | _unchanged_ | low | 4. Signed divide rounds toward zero [CONFIRMED @ 0x434408] 5. 180° (0x800) offset added to heading [CONFIRMED @ 0x434501] | Signed divide-by-4 rounding toward zero [CONFIRMED @ 0x434408] */ dx = (dx + ((dx >> 31) & 3)) >> 2; | [CONFIRMED @ 0x00434408] standard quad */ dx = ((int32_t)vl1->x - (int32_t)vr1->x) - (int32_t)vr0->x + (int32_t)vl0->x; | td5_track.c:3671 |
| 0x00434410 | _unchanged_ | _unchanged_ | low | 2. Case 3/4: uses right+2 vertex [CONFIRMED @ 0x434410] 3. Case 6/7: uses left+2 vertex [CONFIRMED @ 0x434450] | Shifted diagonal: uses right+2 vertex [CONFIRMED @ 0x434410] */ { | [CONFIRMED @ 0x00434410] diagonal — right+2 vertex */ { | td5_track.c:3669 |
| 0x00434450 | _unchanged_ | _unchanged_ | low | 3. Case 6/7: uses left+2 vertex [CONFIRMED @ 0x434450] 4. Signed divide rounds toward zero [CONFIRMED @ 0x434408] | Reversed winding: uses left+2 vertex [CONFIRMED @ 0x434450] */ { | [CONFIRMED @ 0x00434450] reversed winding — left+2 vertex */ { | td5_track.c:3670 |
| 0x00434501 | _unchanged_ | _unchanged_ | low | 5. 180° (0x800) offset added to heading [CONFIRMED @ 0x434501] | stored = (full_angle + 0x800) * 0x100  [CONFIRMED @ 0x00434501] Port's atan2-based AngleFromVector12 also returns the full-circle angle | Store yaw to euler accumulator at +0x1F4. [CONFIRMED @ 0x00434501] */ (int32_t *)((uint8_t *)actor + 0x1F4) = (angle + 0x800) << 8; | td5_track.c:3672 |
| 0x004345b0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004345B0] ======================================================================== */ | td5_track.c:4336 |
| 0x004345e0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004345E0] base vertex = *(uint16*)(span + 0x06) = right_vertex_index */ | td5_track.c:4352 |
| 0x00434631 | _unchanged_ | _unchanged_ | low | ax = actor_pos[0] >> 8;  /* strip 24.8 FP to integer world [CONFIRMED @ 0x00434631] */ | td5_track.c:4383 |
| 0x0043465d | _unchanged_ | _unchanged_ | low | Two-stage normalisation: (dot/len)<<8 then /len [CONFIRMED @ 0x0043465d] */ scaled = (dot / span_len) << 8; | td5_track.c:4392 |
| 0x00434670 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00434670] ======================================================================== */ | td5_track.c:4403 |
| 0x004346f5 | _unchanged_ | _unchanged_ | low | Sign from comparison [CONFIRMED @ 0x004346f5] */ { | td5_track.c:4448 |
| 0x00434740 | _unchanged_ | _unchanged_ | low | 0x00434740  ComputeRouteForwardOvershootScalar  [ARCH-DIVERGENCE: dead code, sole caller discards return; L5 sweep 2026-05-21] | td5_track.c:6462 |
| 0x0043483d | _unchanged_ | _unchanged_ | low | World-space base coordinates [CONFIRMED @ 0x43483D] */ int32_t left_x  = (int32_t)v_left->x  + sp->origin_x; | td5_track.c:4516 |
| 0x00434ba0 | _unchanged_ | _unchanged_ | low | FUN_00434BA0 [CONFIRMED @ 0x434BA0] UpdateSpecialEncounterControl. In the original, this modifies the cop (encounter) actor's heading/ | td5_input.c:751 |
| 0x00434fe0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00434FE0] decomp: if ((800 < uVar3) && (uVar3 < 0xce0)) — 800 is DECIMAL (= 0x320), upper is strict <. | td5_ai.c:3422 |
| 0x00434ffb | _unchanged_ | _unchanged_ | low | Route-byte → 12-bit angle: (byte * 0x102C) >> 8 [CONFIRMED @ 0x434FFB] */ route_heading = (((int32_t)route_table[(size_t)span * 3u + 1u] * 0x102C) >> | td5_ai.c:960 |
| 0x00435218 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00435218] walker field layout (stride 6, per entry): u16 remap_dst           at +0 | Record layout per [CONFIRMED @ 0x00435218] walker disasm: +0 u16 remap_dst | td5_track.c:4608 |
| 0x00435352 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x435352] */ int32_t delta = actor_heading - target_angle; | td5_ai.c:3594 |
| 0x00435392 | _unchanged_ | _unchanged_ | low | threshold != 0 → weight = 0x10000  [CONFIRMED @ 0x00435392] threshold == 0 → weight = 0x20000  [CONFIRMED @ 0x0043539F] | td5_ai.c:3638 |
| 0x0043539f | _unchanged_ | _unchanged_ | low | threshold == 0 → weight = 0x20000  [CONFIRMED @ 0x0043539F] | td5_ai.c:3639 |
| 0x004353b0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004353B0] L5 promotion sweep audit (2026-05-18). | td5_ai.c:3850 |
| 0x00435940 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00435940] L5 audit 2026-05-18 (TD5_pool0 read-only) — byte- faithful with original for all in-loop logic: | [ARCH-DIVERGENCE D2] The original 0x00435940 does NOT zero steering_cmd / vehicle_mode / etc — those resets happen in | td5_ai.c:4319 |
| 0x00435e80 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00435E80] L5 promotion sweep audit (2026-05-18). - Stages 1-7 enumerated above match orig FSM block layout | td5_ai.c:4760 |
| 0x00435f6a | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00435F6A] iVar14=field_0x82; iVar8 = iVar14 + 1 */ lin_span = ((int)span_norm + 1) % span_count; | td5_ai.c:4974 |

## Key discoveries

- (Mechanical comment-sync batch; no new findings.)

## Out-of-scope finds

- (None — this batch only consolidates existing port audit headers.)

## TODO impact

- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.

