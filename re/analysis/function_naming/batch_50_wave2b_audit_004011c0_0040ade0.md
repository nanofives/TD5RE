---
batch: 50
area: wave2b_audit_comment_sync
tier: T6
target_todos: []
ghidra_session: (per-batch, ghidra-apply opens master)
analyzed_addresses: 0x004011c0,0x00401204,0x00401950,0x00402e40,0x00402e60,0x00403180,0x00403182,0x004032a0,0x004032e8,0x00403720,0x00403d90,0x00404030,0x004044f9,0x00404521,0x004046dc,0x00404b27,0x00404b93,0x00404ec0,0x0040506b,0x004050b8,0x004050ef,0x004051a0,0x00405285,0x004057f0,0x004057fa,0x00405d70,0x00405ff9,0x0040634b,0x00406650,0x00406950,0x004069cc,0x00406a10,0x00406a66,0x00406aae,0x00406cc0,0x004073b0,0x00407420,0x00407424,0x004076c0,0x00407840,0x00407879,0x004078b7,0x004079c0,0x00407adb,0x00408570,0x004089eb,0x00409bf0,0x00409dd0,0x00409e98,0x00409f76,0x00409fcc,0x0040a260,0x0040a2b0,0x0040a440,0x0040a490,0x0040a6a0,0x0040a6c0,0x0040a720,0x0040a880,0x0040ade0
agent: claude-opus-4-7
date: 2026-05-22
---

# Wave 2B audit-comment sync — batch 50 (60 addresses)

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
| 0x004011c0 | _unchanged_ | _unchanged_ | low | 0x004011C0  RenderVehicleTaillightQuads           [ARCH-DIVERGENCE: D3D3 Templates] | td5_render.c:6102 |
| 0x00401204 | _unchanged_ | _unchanged_ | low | td5_vfx.c [CONFIRMED @ 0x401204] / @ 0x4011F5]. */ static void render_vehicle_brake_lights(const TD5_Actor *actor, int slot) | if (brightness < 0x80) {  /* cap at 128 [CONFIRMED @ 0x401204] */ brightness += 8; | td5_render.c:4228 |
| 0x00401950 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x401950]: original calls LoadRenderRotationMatrix with actor+0x120 then ConvertFloatVec3ToShortAngles. */ | td5_camera.c:1126 |
| 0x00402e40 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00402E40] — Ghidra-verified: orig walks &gPlayerControlBuffer with a 6-iteration loop writing 0 per int (6 × 4 bytes). Port loop matche | td5_input.c:1002 |
| 0x00402e60 | _unchanged_ | _unchanged_ | low | Sync current gear from live actor [CONFIRMED @ 0x402E60] */ if (a_gear) s_gear[slot] = ((uint8_t *)a_gear)[0x36B]; | td5_input.c:816 |
| 0x00403180 | _unchanged_ | _unchanged_ | low | Per-slot encounter latch accessor [CONFIRMED @ 0x00403180]. Original UpdatePlayerVehicleControlState gates the encounter-control branch | Per-slot encounter latch [CONFIRMED @ 0x00403180]. Original gate: gActorSpecialEncounterActive[slot * 0x11c] != 0. | No brake: reset throttle_state to forward [CONFIRMED @ 0x403180] */ s_brake[slot] = 0; | td5_ai.c:430 |
| 0x00403182 | _unchanged_ | _unchanged_ | low | s_throttle[slot] = 0x100;  /* original DAT_0046317C = 0x0100 [CONFIRMED @ 0x403182] */ | td5_input.c:769 |
| 0x004032a0 | _unchanged_ | _unchanged_ | low | actor->throttle_state = 1;  /* forward mode [CONFIRMED @ 0x4032A0] */ actor->vehicle_mode = 0; | td5_physics.c:9139 |
| 0x004032e8 | _unchanged_ | _unchanged_ | low | brake_flag mirrors throttle_state [CONFIRMED @ 0x4032E8] */ s_brake[slot] = throttle_st; | td5_input.c:740 |
| 0x00403720 | _unchanged_ | _unchanged_ | low | Per-probe track position update [CONFIRMED @ 0x403720]. Original calls FUN_004440F0 per probe with probe's own world pos. | [CONFIRMED @ 0x00403720] */ int32_t force = (wheel_y - ground_y) + g_gravity_constant; | td5_physics.c:7011 |
| 0x00403d90 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x403D90] Byte-faithful with orig UpdateVehicleState0fDamping. L5 audit 2026-05-18 (TD5_pool0 read-only): | td5_physics.c:7790 |
| 0x00404030 | _unchanged_ | _unchanged_ | low | 3) [ARCH-DIVERGENCE — port-only step 9 surface_contact_flags safety net] Port writes scf at the tail of this dispatcher when slot is the human | [CONFIRMED @ 0x00404030] Byte-faithful with orig UpdatePlayerVehicleDynamics. SAR-RZ rounding fixed 2026-05-18 (L5 promotion follow-up). | [CONFIRMED @ 0x00404030] | td5_physics.c:886 |
| 0x004044f9 | _unchanged_ | _unchanged_ | low | before calling auto_gear [CONFIRMED @ 0x4044F9]. During braking, throttle=-256 which would make auto_gear set gear=REVERSE, | td5_physics.c:1423 |
| 0x00404521 | _unchanged_ | _unchanged_ | low | Gearbox dispatch [CONFIRMED @ 0x404521]: field_0x378 == 0 → manual, != 0 → automatic */ | td5_physics.c:1585 |
| 0x004046dc | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004046DC]: original re-reads gSurfaceGripCoefficientTable (DAT_004748C0) directly — it does NOT use the load-weighted/clamped grip[]. | td5_physics.c:1679 |
| 0x00404b27 | _unchanged_ | _unchanged_ | low | rear_lat_force = local_14_num / denom;  /* no negation [CONFIRMED @ 0x00404B27] */ | td5_physics.c:1760 |
| 0x00404b93 | _unchanged_ | _unchanged_ | low | front_lat_force = local_c_num / denom;  /* no negation [CONFIRMED @ 0x00404B93] */ | td5_physics.c:1774 |
| 0x00404ec0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00404EC0] Byte-faithful with orig UpdateAIVehicleDynamics. SAR-RZ rounding fixed 2026-05-18 (L5 promotion follow-up). | --- 4. Velocity drag in WORLD frame [CONFIRMED @ 0x404EC0] --- */ { | td5_physics.c:2240 |
| 0x0040506b | _unchanged_ | _unchanged_ | low | Cross-weight load transfer [CONFIRMED @ 0x40506B]: front_load = (rear_weight << 8) / total_weight * (half_wb - susp_defl) / half_wb | td5_physics.c:2316 |
| 0x004050b8 | _unchanged_ | _unchanged_ | low | Grip from surface friction * load [CONFIRMED @ 0x4050B8] */ int32_t sf = (int32_t)s_surface_friction[surface & 0x1F]; | td5_physics.c:2322 |
| 0x004050ef | _unchanged_ | _unchanged_ | low | Steer delta trig (cos_d, sin_d) [CONFIRMED @ 0x4050EF] */ int32_t cos_d = cos_fixed12(steer_angle & 0xFFF); | td5_physics.c:2357 |
| 0x004051a0 | _unchanged_ | _unchanged_ | low | --- 6. Engine pipeline [CONFIRMED @ 0x4051A0] --- */ int32_t throttle = (int32_t)actor->encounter_steering_cmd; | td5_physics.c:2395 |
| 0x00405285 | _unchanged_ | _unchanged_ | low | before the bicycle solve [CONFIRMED @ 0x00405285]. The drive path already stored torque/4; brake path stored clamp/2. | td5_physics.c:2541 |
| 0x004057f0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004057F0] L5 promotion sweep audit (2026-05-18). | td5_physics.c:4625 |
| 0x004057fa | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004057FA] UpdateVehicleSuspensionResponse calls `TransposeMatrix3x3(&actor->rotation_m00, local_30)` then | td5_physics.c:4584 |
| 0x00405d70 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00405D70] Byte-faithful with orig ResetVehicleActorState. L5 promotion 2026-05-18 (small-tier sweep). 54-instr listing match: clears | those 4 per-wheel values into world_pos.y. [CONFIRMED @ 0x00405D70] */ actor->world_pos.y = (int32_t)0xC0000000;   /* +0x200 */ | td5_physics.c:7545 |
| 0x00405ff9 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00405FF9] gate angular_velocity on mask stability. */ if (mask_stable) { | td5_physics.c:6115 |
| 0x0040634b | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0040634B]: original does INC word ptr [ESI+0x360] inside the per-wheel averaging loop of IntegrateVehiclePoseAndContacts, | td5_physics.c:6014 |
| 0x00406650 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00406650] Steps 1-8 (frame counter, ghost reset, speed tracking, race timer, attitude clamp, dynamics dispatch + paused + scripted, | td5_physics.c:859 |
| 0x00406950 | _unchanged_ | _unchanged_ | low | 0x00406950  ClearTrackSegmentVisibilityTable  [ARCH-DIVERGENCE: broadphase grid array-width compaction + chain fold; L5 sweep 2026-05-21] | td5_track.c:6444 |
| 0x004069cc | _unchanged_ | _unchanged_ | low | wall-normal (v_perp, iVar10) components [CONFIRMED @ 0x4069cc]. Same round-toward-zero correction on the >>12 (D2/D3/D4/D6 family). */ | td5_physics.c:413 |
| 0x00406a10 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x406a10]. D6 audit: round-toward-zero on the >>12, matching original (val + (val>>31 & 0xFFF)) >> 12 pattern. */ | td5_physics.c:530 |
| 0x00406a66 | _unchanged_ | _unchanged_ | low | Angular divisor shared with V2V = 0x28C (652) [CONFIRMED @ 0x406a66] */ #define ANGULAR_DIVISOR_W   0x28C | td5_physics.c:349 |
| 0x00406aae | _unchanged_ | _unchanged_ | low | Impulse numerator scale factor [CONFIRMED @ 0x406aae] */ #define V2W_NUM_SCALE       0x1100 | td5_physics.c:351 |
| 0x00406cc0 | _unchanged_ | _unchanged_ | low | dispatch [CONFIRMED @ 0x00406CC0]. | td5_track.c:808 |
| 0x004073b0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004073B0] outer_sub = lane_count nibble (NOT -1). Fixed 2026-05-18 (was `lane_count - 1` per L4 audit). */ | td5_physics.c:5386 |
| 0x00407420 | _unchanged_ | _unchanged_ | low | (int16_t*)(car_def + 0x0C) = half-width equivalent   [CONFIRMED @ 0x407420] | td5_physics.c:5220 |
| 0x00407424 | _unchanged_ | _unchanged_ | low | (int16_t*)(car_def + 0x08) = half-length equivalent  [CONFIRMED @ 0x407424] (int16_t*)(car_def + 0x0C) = half-width equivalent   [CONFIRMED @ 0x407420 | td5_physics.c:5219 |
| 0x004076c0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x4076C0] if (*(short*)(actor+0x80) == DAT_00483954 + 1) [CONFIRMED @ 0x443ED0] called after ProcessActorRouteAdvance, before | td5_physics.c:5517 |
| 0x00407840 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00407840] Byte-faithful with orig ProcessActorRouteAdvance. - Wrap-span strip indexed at DAT_00483550 (total_spans), matching the | td5_physics.c:5426 |
| 0x00407879 | _unchanged_ | _unchanged_ | low | Only fires at the last span [CONFIRMED @ 0x407879] */ int span_idx = (int)actor->track_span_raw; | td5_physics.c:5444 |
| 0x004078b7 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004078B7] orig reads strip[wrap+3]&0xF = lane_nibble. */ int wrap_lanes = (int)(((const uint8_t *)sp_wrap)[3] & 0x0F); | td5_physics.c:5467 |
| 0x004079c0 | _unchanged_ | _unchanged_ | low | NUM_CONST = (500000 >> 8) * 0x1100 = 8,499,456 [CONFIRMED @ 0x4079C0]. The `>> 8` on INERTIA_K is NOT a compiler overflow workaround — it is | td5_physics.c:3399 |
| 0x00407adb | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x407ADB]: cardef+0x04 = front-Z extent (positive), cardef+0x08 = half-width, | td5_physics.c:3357 |
| 0x00408570 | _unchanged_ | _unchanged_ | low | Contact data from OBB corner test [CONFIRMED @ 0x00408570]: cx_A, cz_A = corner->proj_x, proj_z   (corner in TARGET's frame, WITH translation) | td5_physics.c:3248 |
| 0x004089eb | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x004089EB] — original applies the same subtraction with sVar1/sVar2 = A_center in B's frame. */ | td5_physics.c:3177 |
| 0x00409bf0 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: recovery-mode Euler decomp not ported; L5 sweep 2026-05-21] Orig extracts (yaw, pitch, roll) from rotation matrix via | td5_camera.c:2671 |
| 0x00409dd0 | _unchanged_ | _unchanged_ | low | if (actor->frame_counter > 0x3B) {  /* 59 frames [CONFIRMED @ 0x00409DD0] */ TD5_LOG_I(LOG_TAG, "mode1 recovery: slot=%d resetting after %d frames", | td5_physics.c:1061 |
| 0x00409e98 | _unchanged_ | _unchanged_ | low | Store split time — raw timing_frame_counter at crossing [CONFIRMED @ 0x00409E98] */ | td5_game.c:3928 |
| 0x00409f76 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00409F76] formula: actor+0x336 = (sub_prog * 0x5DC) / denom | td5_game.c:3782 |
| 0x00409fcc | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x00409FCC] Original: param_1[1]='\x01' (companion_2=1), NOT 2. P2P branch at 0x0040A180 also writes '\x01'. Circuit was | td5_game.c:3826 |
| 0x0040a260 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0040A260] */ static void UpdateCameraTransitionHudIndicator(int view, int actor_index) { | td5_camera.c:2589 |
| 0x0040a2b0 | _unchanged_ | _unchanged_ | low | 2) [CONFIRMED @ 0x0040A2B0] AdvancePendingFinishState consolidated into td5_game.c game-tick loop (tick_pending_finish_timer @ td5_game.c). | td5_physics.c:877 |
| 0x0040a440 | _unchanged_ | _unchanged_ | low | DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440]: encounter mode 4 erodes the actor's clean_driving_score by 1 | DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440] — same as inner-edge */ if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) { | td5_physics.c:5348 |
| 0x0040a490 | _unchanged_ | _unchanged_ | low | UpdateRaceCameraTransitionTimer @ 0x0040a490 [ARCH-DIVERGENCE] See detailed audit comment at tick_race_countdown() — same-shape | td5_game.c:234 |
| 0x0040a6a0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0040A6A0] Byte-faithful with orig CosFloat12bit. L5 promotion 2026-05-18 (small-tier sweep). 4-instr listing match: | td5_render.c:5014 |
| 0x0040a6c0 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0040A6C0] Byte-faithful with orig SinFloat12bit. L5 promotion 2026-05-18 (small-tier sweep). 5-instr listing match: | td5_render.c:5029 |
| 0x0040a720 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE @ 0x00433FC0] Orig calls AngleFromVector12 (0x0040A720) which indexes a 12-bit atan2 LUT at DAT_00463214. Port uses libm | td5_ai.c:359 |
| 0x0040a880 | _unchanged_ | _unchanged_ | low | ResetRaceResultsTable @ 0x0040a880 [ARCH-DIVERGENCE] Orig writes a 6-entry, 20-byte-stride table at 0x0048d988. Per entry: | td5_game.c:198 |
| 0x0040ade0 | _unchanged_ | _unchanged_ | low | [ARCH-DIVERGENCE: D3D3 BeginScene -> D3D11 platform-abstracted begin-frame; L5 sweep 2026-05-21] | td5_render.c:1145 |

## Key discoveries

- (Mechanical comment-sync batch; no new findings.)

## Out-of-scope finds

- (None — this batch only consolidates existing port audit headers.)

## TODO impact

- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.

