# OVERSIGHT triage — 10 new bugs (2026-05-24)

Source: re/analysis/orig_vs_port_verdict_2026-05-24.csv (verdict=OVERSIGHT, tag not in REGR#1/REGR#2/REGR#4/REGR#6)

Note: The prompt anticipated ~13 rows; the actual count of OVERSIGHT entries minus the four REGR# tags shipped in `permanent_l4_residual.md` (REGR#1/REGR#2/REGR#4/REGR#6, lines 10/11/228/433/434/435/436 of the CSV) is **10**.

## Summary
| Category | Count | Action |
|---|---:|---|
| TRIVIAL fixes | 2 | Apply in next bug-fix session (snow Z modulus, dead-field semantics) |
| SMALL fixes | 3 | Schedule for individual investigation (wall-impact SFX, case 1/2 helper swap, ai_wanted decrement gates) |
| MEDIUM fixes | 2 | Plan as standalone session (damage_lockout switch, scripted Y damping + matrix advance) |
| NEEDS_RUNTIME_TRACE | 2 | Needs Frida/A-B before fixing (chase camera terrain pitch/roll, DA-T2 cascade) |
| REJECTED (re-classified) | 1 | Update CSV (D4_audit_residual — actually `surface_wheel[]` IS each wheel's own surface, not center fallback as evidence claimed) |

## By severity (HIGH → LOW)

### HIGH impact

#### wall_impact_sfx_ff_missing — 0x00406980 ApplyTrackSurfaceForceToActor
- **Port location:** td5mod/src/td5re/td5_physics.c:354 (td5_physics_wall_response), tail block missing
- **Orig behavior:** After computing impulse, if `iVar10 > 0x3200` (≈12800, big lateral velocity), calls `PlayVehicleSoundAtPosition` (variant 0x16/0x1b with mag/pitch from velocity) + `DecayUltimateVariantTimer` + `PlayAssignedControllerEffect` (force-feedback rumble channel 1 or 2, magnitude derived from `iVar10/4`). Also reads `ComputeActorRouteHeadingDelta` to XOR-swap rumble channel under bend conditions.
- **Port behavior:** Function ends after rotating new_v_para/new_v_perp back to world basis at line 536. The wall-impact SFX + FF tail is completely absent (grep confirms no PlayVehicleSoundAtPosition / PlayAssignedControllerEffect calls anywhere in td5_physics.c).
- **Real divergence?** YES — verified via decomp 0x00406B70..0x00406CB7.
- **Fix class:** SMALL
- **Recommended fix:** After the `if (iVar11 >= 0)` block (post-line 549), add:
  ```c
  if (v_perp > 0x3200) {
      int32_t pitch_arg = v_perp - 0x2000;
      if (pitch_arg < 0x400) pitch_arg = 0x400;
      else if (pitch_arg > 0x800) pitch_arg = 0x800;
      int variant = (v_perp < 0x19001) ? 0x16 : 0x1b;
      int mag     = (v_perp < 0x19001) ? 0x5622 : 0x2198;
      int volume  = (v_perp < 0x19001) ? 1 : 4;
      td5_sound_play_vehicle_sound(variant, pitch_arg, mag, &actor->world_pos.x, volume);
      td5_vfx_decay_ultimate_variant_timer(actor, 1);
      int32_t ff_mag = (v_perp + ((v_perp >> 31) & 3)) >> 2;
      if (ff_mag > 99999) ff_mag = 100000;
      uint32_t hd = td5_ai_compute_route_heading_delta(actor->slot_index);
      uint32_t local_flags = (side < 0) ? 0u : (uint32_t)(side + 1);
      if ((int32_t)hd > 0x3FF && (int32_t)hd < 0xC00) local_flags ^= 3;
      if (local_flags == 1) td5_input_play_ff_effect(actor->slot_index, 2, ff_mag, ff_mag * 10);
      else if (local_flags == 2) td5_input_play_ff_effect(actor->slot_index, 1, ff_mag, ff_mag * 10);
  }
  ```
  Helper functions for sound/FF may already exist; verify symbol names. May need `td5_ai_compute_route_heading_delta` stub if not present.
- **Risk:** Adds 4 helper-call dependencies. If FF or sound helpers don't yet bind to playable backends, must stub safely. Visual/audio only — no sim effect.

#### damage_lockout_switch_skipped — 0x004063a0 UpdateVehiclePoseFromPhysicsState
- **Port location:** td5_physics.c:6713 (large TODO comment block, no switch)
- **Orig behavior:** After 1st BuildRotationMatrixFromAngles + RefreshVehicleWheelContactFrames, runs an 11-case switch on `actor->damage_lockout`. Cases 0/1/2/4/6/8/9 call `TransformTrackVertexByMatrix` (3-axis); cases 3/12 call `TransformTrackVertexByMatrixC` (pitch only); cases 5/10 call `TransformTrackVertexByMatrixB` (roll only). Each writes back `euler_accum_roll` and/or `euler_accum_pitch` from the new wheel-implied attitude. Then a 2nd BuildRotationMatrixFromAngles picks up the new values for downstream chassis snap.
- **Port behavior:** Port acknowledges the omission in the TODO comment at 6709-6713. Cases skipped entirely; the 2nd rebuild produces the SAME matrix as the 1st (port comment 6720-6724 says as much).
- **Real divergence?** YES — well-documented; affects per-wheel attitude feedback after collisions.
- **Fix class:** MEDIUM
- **Recommended fix:** Port `TransformTrackVertexByMatrix` (and B/C variants) at 0x42E*** — each is a 3-axis Euler extraction from the rotation matrix using `AngleFromVector12` + `FUN_0044817C`. Then add the switch. Estimated ~80-150 lines including helper ports.
- **Risk:** HIGH. Changes per-tick attitude integration. Must validate against pool13 dynamic-diff. Comment explicitly defers to a "follow-up precise-port worktree (precise-00446030)".

#### partial_port_missing_y_damping_and_matrix_advance — 0x00409d20 IntegrateScriptedVehicleMotion
- **Port location:** td5_physics.c:10385 (td5_physics_integrate_scripted_motion)
- **Orig behavior:** lin_vel_x damping; lin_vel_y damping THEN subtract gravity (`vy = vy - SAR_RZ_8(vy) - gravity`); lin_vel_z damping; world_pos += each velocity; `MultiplyRotationMatrices3x3(recovery_target, saved_orientation, scratch) → saved`, then copy to actor->rotation_matrix; `UpdateActorTrackPosition` + `ComputeActorTrackContactNormalExtended`; render_pos = world_pos * 1/256; `RenderVehicleActorModel` → returns view_mask; if non-zero call `ComputeActorWorldBoundingVolume`; if frame_counter > 0x3B → `ResetVehicleActorState`.
- **Port behavior:** Port at td5_physics.c:10385-10440 ALREADY implements all six steps (X/Y/Z damping + gravity + matrix multiply + track update + render_pos + view_mask + reset). Evidence column is OUTDATED — it was written before the 2026-05-24 Tier 2 port (see header comment line 1015-1021 "Tier 2 NOT_PORTED port (2026-05-24)").
- **Real divergence?** NO — caller (td5_physics.c:1037-1040) overwrites world_pos from +0x208/A/C BEFORE `integrate_scripted_motion` runs, which makes the `world_pos += velocity` inside the helper a no-op. This IS a legitimate residual; the orig's IntegrateScriptedVehicleMotion does the `+=` after RefreshScriptedVehicleTransforms (which doesn't pre-write world_pos from those fields). Port's "D11" seed at :1024-1040 is the inverted-order bug.
- **Fix class:** MEDIUM
- **Recommended fix:** Remove the D11 world_pos seed block at td5_physics.c:1032-1040 — IntegrateScriptedVehicleMotion already advances world_pos via `+= velocity`. The shorts at +0x208/A/C alias display_angles, and during recovery mode they DO hold pose data but ANNEX'd by RefreshScriptedVehicleTransforms — they should not be re-read as world_pos. Verify by Frida-dumping `world_pos.{x,y,z}` of slot 0 in recovery mode against orig.
- **Risk:** MEDIUM. The D11 seed was added intentionally to "fix" recovery; removing without validation could regress recovery animation. Needs A-B test in recovery scene.

### MEDIUM impact

#### case_1_2_basis_transform — 0x00402480 UpdateTracksideCamera
- **Port location:** td5_camera.c:1992 (case 1/2 uses TransformVector3ByBasis)
- **Orig behavior:** Orig case 1/2 calls `ConvertFloatVec3ToIntVec3 @ 0x0042DB40` which does `__ftol` of the result then `(int)(short)` clamp to [-32768, 32767].
- **Port behavior:** Port calls `TransformVector3ByBasis @ 0x0042DBD0` which casts via `(int)` (truncate toward zero) with NO short clamp. For offset values |x| > 32767 the diverge is 1+ unit; for normal values (|offset| ≤ 32767) the math is equivalent.
- **Real divergence?** YES — verified at td5_render.c:5412 documentation; orig camera sites called the short-clamped helper.
- **Fix class:** SMALL
- **Recommended fix:** Add `ConvertFloatVec3ToIntVec3` helper that does `out[i] = (short)(int)(matrix[i*3+0]*v[0] + ...)`. Replace `TransformVector3ByBasis` call at td5_camera.c:1992 with this. The other call site (UpdateVehicleRelativeCamera at 0x00401C20) per evidence is FAITHFUL so should also flip; check whether it has the same issue.
- **Risk:** LOW. Trackside cameras only; offsets are bounded by camera preset table which has short-range values, so practical observable change is near-zero. Cosmetic safety net for edge cases.

#### missing-gates-and-arrest-counter — 0x0043d690 AwardWantedDamageScore
- **Port location:** td5_ai.c:447 (td5_ai_wanted_cop_hit)
- **Orig behavior:** Multiple gates: `DAT_004bf518 == 0` early-out; `cop_slot != g_wantedTargetSlotIndex` AND `g_wantedTargetTrackerActive != 0` AND `impact_mag > 9999`. First-hit vs re-hit: if `g_wantedDamageHudOverlayCount != cop_slot`, sets rand-based message index + zeroes timers + returns WITHOUT decrement. Re-hit path: decrements `gWantedDamageStateTable[cop_slot]` by 0x200 (or 0x400 if impact > 20000), bumps `g_wantedArrestCounter`, adds +10/+20/+50 to `+0x8260` timer based on impact tier and sign, clamps state to [0, 0x1000].
- **Port behavior:** Decrements unconditionally on every call: no impact gate, no global gates, no first-hit-vs-rehit branching, no arrest counter, no +0x8260 timer increments, no upper 0x1000 clamp.
- **Real divergence?** YES — port is a partial stub (4 lines + arrest freeze, vs orig ~80 lines).
- **Fix class:** SMALL
- **Recommended fix:** Add the three early-out gates (impact > 9999, global flag, target tracker), the rehit-vs-first-hit branch, arrest counter increment, and the +0x8260 timer bumps. ~25-line expansion. The +0x8260 offset is into the per-slot `g_raceParticlePoolBase[iVar1*0x388 + 0x8260]` region.
- **Risk:** MEDIUM. Cop chase mode is rarely exercised in current testing; changes timer/HUD message timing. Acceptable risk because port currently over-decrements on every collision (way too easy to "arrest" cops).

### LOW impact / REJECTED

#### snow_z_range_off_by_48 — 0x00446240 InitializeWeatherOverlayParticles
- **Port location:** td5_vfx.c:1040
- **Orig behavior:** `rand() % 0x1f37` (= 7991) + 200
- **Port behavior:** `rand() % 7943` + 200
- **Real divergence?** YES — orig uses 0x1f37 = 7991; port uses 7943. Off-by-48.
- **Fix class:** TRIVIAL
- **Recommended fix:**
  ```diff
  - buf[i].pos_z = (float)(rand() % 7943 + 200);
  + buf[i].pos_z = (float)(rand() % 7991 + 200);
  ```
- **Risk:** NONE. Snow particle pool is initialized but never rendered (port comment at td5_vfx.c:1041 confirms "snow never renders"). Purely a constant fidelity fix. Use `0x1f37` for byte-faithfulness.

#### dead-fields — 0x0040a3d0 AccumulateVehicleSpeedBonusScore
- **Port location:** td5_game.c:4025 (accumulate_speed_bonus)
- **Orig behavior:** Gates on `actor->finish_time==0 && actor->surface_contact_flags==0 && actor->lateral_speed > 0 && (g_raceParticlePoolBase[0x1eb].view_x.b0 & 3) == 0`. Bonus = `(lateral_speed >> 15) - (race_position >> 1)`. Clamp to 0 if `surface_type_chassis > 15` OR `bonus < 0`. Slot-0 only: bonus = 0 if `track_span_normalized < track_span_high_water`. Accumulates into `actor->field_0x2c8`.
- **Port behavior:** Reads `m->forward_speed`, `m->skid_factor`, `m->contact_count` from `ActorRaceMetric` — these three fields are NEVER WRITTEN anywhere in the codebase (Grep confirms 0 writers). `forward_speed > 0` test always fails (always 0), so the function is an effective no-op.
- **Real divergence?** YES — port also misuses fields (skid_factor/contact_count vs orig's race_position/surface_type_chassis), but doubly broken because input fields are uninitialized.
- **Fix class:** TRIVIAL
- **Recommended fix:** Two options:
  1. Rewrite to read from actor directly: `actor->lateral_speed`, `actor->race_position`, `actor->surface_type_chassis`, `actor->surface_contact_flags`, `actor->finish_time`. Accumulate into `actor->field_0x2c8` (the bonus score).
  2. Or remove the function entirely if speed-bonus scoring is unused in port.
- **Risk:** LOW. Speed bonus is an arcade scoring extra not visible in TT mode. May only affect championship final score totals. Recommend Option 1 — replace `m->...` reads with actor field reads to fix dead-field bug AND wrong-field-semantics bug in one shot.

#### terrain_pitch_roll_zeroed — 0x00401590 UpdateChaseCamera
- **Port location:** td5_camera.c:752-754
- **Orig behavior:** `AngleFromVector12` derives pitch from `(local_38 - (local_2c+local_20)/2)>>8` and roll from `(local_2c - local_20)>>8` based on 3 terrain probe normals at the wheels.
- **Port behavior:** Hard-codes `cam_angles[0] = 0` (pitch) and `cam_angles[2] = 0` (roll). Keeps `cam_angles[1] = combined_angle` (yaw).
- **Real divergence?** YES — intentional pragmatic fix per `todo_camera_no_slope_adjustment_2026-05-16.md`. Port comment 747-750 says terrain probes return "garbage" because of coord-system mismatch.
- **Fix class:** NEEDS_RUNTIME_TRACE
- **Recommended fix:** Cannot safely fix without resolving the documented coordinate-system mismatch between terrain probes (24.8 FP expected) and actor positions (raw int). Needs Frida trace of `local_38/local_2c/local_20` orig vs port across a slope. Closes when chassis-snap cascade is also fixed.
- **Risk:** HIGH. Cosmetic camera-bob on slopes only. Restoring orig math will likely add visible jitter unless probe-vs-pos unit mismatch is resolved first.

#### DA-T2-cascade — 0x004440f0 UpdateActorTrackPosition
- **Port location:** td5_track.c:2520 (update_position_recursive)
- **Orig behavior:** Cases 1 (FWD), 4 (BACK), 2 (RIGHT), 8 (LEFT) all use 3-outcome secondary-cross tests on the NEW span: cross1 ≤ 0 → advance; cross2 ≤ 0 (or mask bit clear) → advance with sub±1; else REJECT and rollback. Compound cases 3/6/9/12 already implement this in port; single-bit cases 1/2/4/8 advance unconditionally.
- **Port behavior:** Cases 1/2/4/8 always advance — never reject. Comments at lines 2545-2568 explicitly document the gap with audit reference to `re/analysis/wave4_deep_audits/da_t2_update_actor_track_pos.md`.
- **Real divergence?** YES — confirmed by side-by-side decomp + port comment.
- **Fix class:** NEEDS_RUNTIME_TRACE
- **Recommended fix:** Per comment, apply Fix 1 (case 1 FWD secondary tests) then measure pool13 lift, then Fix 2/3/4. Estimated 30-60 lines per case. Sequence and per-fix lift estimates already in DA-T2 audit doc.
- **Risk:** HIGH cascade impact. Port comment says: "HIGH-risk cascade changes that need runtime pool13 dynamic-diff measurement to validate". Edinburgh ~210u slot 0 residual bias hangs on this. Do NOT apply blind.

#### D4_audit_residual — 0x00404030 UpdatePlayerVehicleDynamics (REJECTED)
- **Port location:** td5_physics.c:1264 area
- **Orig behavior:** Calls `GetTrackSegmentSurfaceType` 5 times for chassis + 4 wheels (`iVar7..iVar10` for FL/FR/RL/RR), each wheel gets its own surface from its own probe.
- **Port behavior:** Per the evidence note, `surface_wheel[i] = surface_center` is the "fallback". Reading the actual port code at td5_physics.c:1287: `int32_t sf = (int32_t)s_surface_friction[surface_wheel[i] & 0x1F];` — this reads `surface_wheel[i]`, NOT `surface_center`. Per-wheel surface IS being indexed; the per-wheel fill must happen earlier (likely in the contact-refresh function).
- **Real divergence?** NO (likely) — evidence appears to be agent over-flagging. Need to verify where `surface_wheel[]` is filled. If it IS pinned to `surface_center` somewhere then the bug holds; if each wheel's own track probe writes its own surface, REJECT.
- **Fix class:** REJECTED (pending verification of `surface_wheel[]` writer)
- **Recommended fix:** Verify by grepping `surface_wheel[` writers. If found writing `surface_center` to all 4, restore SMALL fix (per-wheel GetTrackSegmentSurfaceType calls). Otherwise update CSV to FAITHFUL.
- **Risk:** N/A — depends on verification.

## Cross-cutting observations

1. **3 of 10 are in `td5_physics.c`** (wall SFX, damage_lockout, scripted Y damping) — suggests collision/recovery pipeline is the under-audited area. The `damage_lockout` and `scripted-motion` items both involve the post-collision attitude rebuild path (`TransformTrackVertex* + Multiply3x3`), which is itself documented as a deferred precise-port worktree (`precise-00446030`).

2. **2 of 10 are camera-related** (case 1/2 helper, terrain pitch/roll). Camera divergences are cosmetic; can be batched into a "camera fidelity pass" session.

3. **Evidence quality is variable**: at least 2 entries (`D4_audit_residual`, `partial_port_missing_y_damping_and_matrix_advance`) appear to be agent over-flagging from outdated evidence — the port already addresses what the CSV says is missing. Recommend a CSV re-run after each major port session to refresh evidence.

4. **`snow_z_range_off_by_48` is the only TRIVIAL constant fix** — apply it now (1-char edit `7943` → `7991`, ideally `0x1f37`).
