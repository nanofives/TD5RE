# Pilot Audit — 0x004063A0 UpdateVehiclePoseFromPhysicsState

**Date:** 2026-05-14
**Pool slot:** TD5_pool3
**Port-side function:** `update_vehicle_pose_from_physics` @ `td5_physics.c:5152`
**Worktree:** `.claude/worktrees/precise-004063A0` on branch `precise-004063A0`
**Body:** 0x004063A0..0x00406617 (0x278 bytes / 178 instructions / 87 decompiled lines).

**Caller:** `ApplyVehicleCollisionImpulse` (0x004079C0). Called as a per-actor pose-refresh callback after a V2V or wall-contact impulse changes world_pos / euler_accum / wheel_suspension_pos.

**Callees (in execution order):**
1. `BuildRotationMatrixFromAngles (0x42E1E0)` — first rotation matrix build from `display_angles@+0x208`.
2. `UpdateActorTrackPosition (0x4440F0)` — refresh `actor->track_span_raw` (+0x80) from `actor->world_pos` (+0x1FC).
3. `ComputeActorHeadingFromTrackSegment (0x445B90)` — recompute heading-relative-to-segment short at `actor+0x290`.
4. `RefreshVehicleWheelContactFrames (0x403720)` — full per-wheel contact-frame refresh (pilot 00403720).
5. *(switch)* `TransformTrackVertexByMatrix (0x446030)` (A), `TransformTrackVertexByMatrixB (0x446140)` (B), or `TransformTrackVertexByMatrixC (0x4461C0)` (C) — depends on OLD wheel_contact_bitmask snapshot (`actor->damage_lockout` @ +0x37D, set by 0x403720 entry).
6. `BuildRotationMatrixFromAngles (0x42E1E0)` — **second** rotation matrix build, after the wheel-derived roll/pitch writeback.
7. `LoadRenderRotationMatrix (0x43DA80)` — copy actor's matrix into global `[0x4BF6B8]` so step 8 can use it.
8. `ConvertFloatVec3ToShortAngles (0x42E2E0)` — per-grounded-wheel: transform body-space vec3 by global matrix, FISTP to 3 shorts. Provides rotated body-Y for chassis-snap denominator.

## Switch table at 0x00406618 (13 entries, key = `actor->damage_lockout` & 0xFF)

| case | target | callee | writeback |
|------|--------|--------|-----------|
| 0    | 0x406470 | MatrixA (0x446030) | roll@+0x208 AND pitch@+0x20C; updates euler_accum_roll AND euler_accum_pitch |
| 1    | 0x406470 | MatrixA | same |
| 2    | 0x406470 | MatrixA | same |
| 3    | 0x40649F | MatrixC (0x4461C0) | pitch only; updates euler_accum_pitch |
| 4    | 0x406470 | MatrixA | same as 0 |
| 5    | 0x4064C2 | MatrixB (0x446140) | roll only; updates euler_accum_roll |
| 6    | 0x406470 | MatrixA | same as 0 |
| 7    | 0x4064DF | — | none (default, skip transform) |
| 8    | 0x406470 | MatrixA | same as 0 |
| 9    | 0x406470 | MatrixA | same as 0 |
| 10   | 0x4064C2 | MatrixB | same as 5 |
| 11   | 0x4064DF | — | none |
| 12   | 0x40649F | MatrixC | same as 3 |

`damage_lockout > 0xC` → JA to default (no-op).

The transform helpers (0x446030/140/1C0) read wheel_suspension_pos values from `param_3 = +0x2DC` and the previously computed body-space wheel positions / matrix to derive a roll- or pitch-only orientation that matches the wheel travel. The result is written as new int16 roll/pitch at `+0x208/+0x20C` (display_angles). The pose function then shifts those back into the corresponding `euler_accum_*` field (`display_angle * 256`), so that the next physics tick starts integration from this newly-derived attitude.

## Per-grounded-wheel chassis-snap loop (0x00406551–0x004065F3)

For each wheel i in [0,4):
- If `(wheel_contact_bitmask & (1<<i)) != 0` → skip (airborne, not on ground).
- Otherwise:
  - Read `cardef[+0x42 + 8*i]` (body-Y for wheel i, int16).
  - Compute `sp_div = sar8_rz(actor->wheel_suspension_pos[i])` — round-to-zero divide by 256.
  - Compute new `body_y = cardef_y - sp_div - preload` (`preload = sar8_rz(cardef[+0x82] * 0xB5)`).
  - Write `body_y` to `actor+0x212+8*i` (overwrites the value left by 0x403720).
  - Call `ConvertFloatVec3ToShortAngles(&actor+0x210+8*i, &actor+0x230+8*i)` — multiplies the int16 body vec3 at +0x210 by the global render matrix at `[0x4BF6B8]`, FISTPs 3 shorts to the destination.
  - Restore `actor+0x212+8*i += preload` → final value is `cardef_y - sp_div` (matches state after refresh).
  - Read `puVar6[1]` (rotated body-Y short).
  - Accumulate `sum += wheel_contact_pos[i].y - (puVar6[1] << 8)` and `count++`.

After loop, if `count > 0`: `actor->world_pos.y = sum / count` (signed CDQ;IDIV). Total airborne (count==0): world_pos.y unchanged.

## Confirmed divergence points (port vs original)

### D1 — Missing `UpdateActorTrackPosition` + `ComputeActorHeadingFromTrackSegment` calls **(HIGH IMPACT)**
After the first rotation matrix build, the original calls:
```
UpdateActorTrackPosition(&actor->track_span_raw, &actor->world_pos);
ComputeActorHeadingFromTrackSegment(&actor->track_span_raw, &actor->world_pos, &actor->field_0x290);
```
The port does **not**. Symptom: after a collision impulse, the chassis `track_span_raw` (+0x80) and heading-relative-to-segment short at `+0x290` go stale. Downstream collision/AI/track lookups then operate on pre-impulse track state. The wheel probes inside `td5_physics_refresh_wheel_contacts` copy `actor->track_span_raw` to each per-wheel probe **before** updating it from the new world position — so the per-wheel walker also starts from a stale span.

### D2 — Display-angles masking to 12 bits **(MEDIUM IMPACT)**
Port:
```c
actor->display_angles.roll  = (int16_t)((euler_accum.roll  >> 8) & 0xFFF);
actor->display_angles.yaw   = (int16_t)((euler_accum.yaw   >> 8) & 0xFFF);
actor->display_angles.pitch = (int16_t)((euler_accum.pitch >> 8) & 0xFFF);
```
Original (listing 0x004063D5/3DE/3E1/3F8/401/408):
```
SAR EAX, 8       (signed)
MOV [+0x208], AX (int16 truncate, no AND mask)
```
For negative euler_accum the port's `& 0xFFF` mask flips the sign bit to a 12-bit positive number; the original keeps full int16. Trig helpers (CosFloat12bit/SinFloat12bit) mask to 12 bits internally so the resulting matrix is the same, but any other consumer of `display_angles.roll/yaw/pitch` (camera, HUD, network) reads the wrong value for negative angles.

### D3 — Missing damage_lockout switch + wheel-derived attitude writeback **(HIGH IMPACT)**
The port omits the entire switch+writeback block. After collision impulse, the original re-derives roll/pitch from the wheels (via Transform helpers) and feeds it back into euler_accum so the next physics tick starts at the wheel-implied attitude. Without this, the impulse-applied attitude diverges immediately on the next physics tick because integration starts from an attitude that doesn't match the wheels.

### D4 — Missing second `BuildRotationMatrixFromAngles` call **(HIGH IMPACT)**
After the switch overwrites display_angle_roll and/or display_angle_pitch (and writes them back to euler_accum), the original calls `BuildRotationMatrixFromAngles` a second time. This rebuilds the actor's rotation matrix to reflect the new wheel-derived orientation. The port still has the matrix from the first (pre-switch) build, which encodes the pre-impulse orientation.

### D5 — Missing `LoadRenderRotationMatrix` call **(HIGH IMPACT)**
The original calls `LoadRenderRotationMatrix(actor->rotation_matrix)` (0x0043DA80) before the chassis-snap loop. This copies the actor's matrix into the global render matrix at `[0x4BF6B8]` that `ConvertFloatVec3ToShortAngles` reads. The port's chassis-snap reads from `s_wheel_offset_y_world` (precomputed in refresh using actor's per-wheel rot-arm), bypassing this dependency — but only IF `s_wheel_offset_y_world` was computed with the SAME matrix the original uses post-switch. Since the port also skips the switch (D3), the port matrix never gets updated, so the cached `s_wheel_offset_y_world` is based on the pre-switch matrix.

### D6 — Different chassis-snap denominator source **(MEDIUM IMPACT)**
Port chassis-snap reads `s_wheel_offset_y_world[slot][i]` (set inside `td5_physics_refresh_wheel_contacts` at line 5346 as `(int16_t)lrintf(by) * 256`, where `by` is the body-Y rotated by the **first** matrix using `wheel_display_angles[i][1]` as the body-Y input).

Original chassis-snap reads `puVar6[1] << 8`, where puVar6 comes from `ConvertFloatVec3ToShortAngles(actor+0x210+8*i, puVar6)`. The input at `actor+0x210+8*i` is freshly overwritten to `(cardef[+0x42+8*i] - sp_div) - preload` — using cardef_y, NOT `wheel_display_angles[i][1]`.

After refresh (post-D2 fix in `precise-00403720`), `wheel_display_angles[i][1] == cardef[+0x42+8*i] - sp_div` (cardef_y minus suspension div). The original then subtracts `preload` AGAIN inside this function before transforming.

So:
- Original input to convert: `cardef_y - sp_div - preload`
- Port's cached `by` (via refresh): rotated `wy = cardef_y - sp_div - preload` (per refresh line 5309).

**These are the same.** Functionally equivalent IF the matrix is the same — but the port's matrix is the pre-switch one, original's is the post-switch one. So in cases 3/5/10/12 where wheel-derived attitude writeback happens, the matrices differ.

### D7 — Missing `& 0xFFF` masking before re-building the matrix is fine
Port's second matrix-build path (currently used as the only path) does pass the masked angles to `cos_fixed12/sin_fixed12`, which apply `& 0xFFF` internally too. The mask is harmless redundancy.

### D8 — `int64_t` chassis-snap accumulator vs original signed-int32
Port uses `int64_t target_sum` then `(int32_t)(target_sum / target_count)`. Original is `int local_10 = 0; ADD local_10, EAX (+ EDX*-0x100 etc)` — signed 32-bit accumulator with potential overflow. With 4 wheels and per-tick deltas typically far below 0x40000000, overflow is unlikely. Equivalent in practice.

### D9 — Bit-test op: `1 << ((byte)local_c & 0x1f)`
Original explicit `& 0x1F` mask on shift count (only 0..3 in loop). Port uses `(1 << i)` without mask. Same behaviour for i in [0,4). Equivalent.

## Capture schema for the pilot trace

Per call (1 row at entry + 4 rows for chassis-snap iterations + 1 row at exit):

**Keys:** `sim_tick`, `slot`, `phase` ("enter" / "wheel<i>" / "exit").
**Inputs at enter:** 
- `actor_addr`
- `euler_in_r,p,y` (3x int32)
- `world_in_x,y,z` (3x int32, FP8)
- `wheel_susp_pos[0..3]` (4x int32, FP8)
- `wcb_in` (uint8)
- `dam_lockout_in` (uint8)
- `cardef_addr`, `cardef_0x82` (preload base int16)

**Outputs at exit:**
- `disp_angles_r,p,y` (3x int16 — full int16 with sign, not masked)
- `euler_out_r,p,y` (3x int32 — captures matrix-A/B/C writeback)
- `track_span_out` (int16, +0x80, captures D1 effect)
- `field_0x290_out` (int16, captures heading post-update)
- `world_out_y` (int32, captures chassis-snap)
- `rot_m[0..8]` (9x float, captures matrix delta)

**Per-wheel rows (i=0..3) in chassis-snap:**
- `grounded_i` (0=skip airborne, 1=summed)
- `body_y_pre` (cardef_y - sp_div - preload)
- `rot_by_short` (puVar6[1] after ConvertFloatVec3ToShortAngles)
- `wheel_contact_y` (actor->wheel_contact_pos[i].y)
- `chassis_delta_i` (wheel_contact_y - rot_by_short<<8)

## Next actions for the pilot

1. **Generate** `tools/frida_pool3_004063A0.js` to capture entry+exit + per-wheel chassis-snap rows.
2. **Add** port-side trace emit (`td5_pilot_trace_004063A0`) at the same boundaries.
3. **Fix highest-impact divergences first**:
   - D1: insert UpdateActorTrackPosition + ComputeActorHeadingFromTrackSegment between matrix build and refresh.
   - D3+D4+D5: implement damage_lockout switch + writeback + 2nd matrix build + LoadRenderRotationMatrix.
   - D2: replace `& 0xFFF` mask with plain int16 truncation in display_angles writeback.
4. **Build, capture, diff, iterate.**

## What NOT to do

- Do not "fix" the chassis-snap math without first fixing the upstream matrix (D3+D4): the divergence will look like an integration error but actually traces back to a stale rotation matrix.
- Do not remove the `wheel_contact_bitmask & (1<<i)` airborne gate — it is in the original (0x00406568 TEST CL,DL).
- Do not assume `damage_lockout` is the same as the prior tick's bitmask: it is the OLD snapshot copied by 0x00403720 at entry (RefreshVehicleWheelContactFrames@+0x37D = +0x37C-pre-refresh).

## Runtime byte-diff status — upstream-blocked (cannot run on track=1 Edinburgh PlayerIsAI=1)

`UpdateVehiclePoseFromPhysicsState` is a **collision-only** callback. Its single
caller (`ApplyVehicleCollisionImpulse @ 0x004079C0`) only fires on V2V or
wall-impulse events. The pilot capture run (Edinburgh, track=1, slot 0,
PlayerIsAI=1, AutoThrottle=1, 60 sim_ticks) produced:

| Side | Rows captured |
|---|---|
| Port (`.claude/worktrees/precise-004063A0/log/port/pool3_004063A0.csv`) | 132 (68 from `0x14c1b9` + 64 from `0x14c1c0` — both inside td5_physics_v2v handler @ td5_physics.c:2819-2820) |
| Original (`log/orig/pool3_004063A0.csv`) | 0 |

The port's V2V handler triggers `update_vehicle_pose_from_physics` aggressively
during AI-driven races on Edinburgh; the original's V2V handler does not fire
in the same window. Therefore byte-equality diffing requires a scenario where
both sides take the V2V path — e.g. a multi-car race + intentional crash, or
a track with mandatory wall contacts. **The pilot's audit + static port + trace
harness are committed; the per-tick CSV diff is deferred to a future capture.**

The mismatch in V2V call frequency is itself a precise-port finding (filed
as a separate follow-up: original-side V2V dispatch is more conservative than
port's, possibly due to TOI bisection threshold or contact-data layout).

## Static-port deliverable (this branch)

Even without a clean runtime diff:

- `td5_physics.c::update_vehicle_pose_from_physics` is byte-faithful for D1/D2/D4/D5 vs the listing:
  - D2: `display_angles` writeback uses int16 truncate (no 12-bit mask).
  - D1: `td5_track_update_actor_position(actor)` + `td5_track_compute_heading(actor)`
    inserted between rotation matrix #1 and refresh_wheel_contacts.
    `td5_track_compute_heading` is a partial substitute for
    `ComputeActorHeadingFromTrackSegment @ 0x00445B90` — same source-of-truth
    (left/right edge vector cross), different sub-walker. Faithful port of
    0x00445B90 + `InterpolateTrackSegmentNormal` deferred to a follow-up.
  - D4: Second `BuildRotationMatrixFromAngles` call (rebuilds matrix from
    display_angles); functionally redundant in the absence of D3 but kept
    so the byte-faithful sequence is preserved.
  - D5: `LoadRenderRotationMatrix(actor->rotation_matrix.m)` call after the
    second build — loads the global render matrix consumed by any subsequent
    `ConvertFloatVec3ToShortAngles` call.
- D3 (damage_lockout switch + Transform helpers + euler_accum writeback)
  documented in audit + TODO comment in the port. Faithful port of
  TransformTrackVertexByMatrix/B/C @ 0x00446030/140/1C0 deferred — they
  require AngleFromVector12 (precise-trig worktree, already done) +
  FUN_0044817C + an inline FSQRT-rounding path.
- Probe + trace harness committed (`tools/frida_pool3_004063A0.js` +
  `td5mod/src/td5re/td5_pilot_trace_004063A0.{c,h}`). Ready for runtime diff
  once a V2V-triggering scenario lands.

## What NOT to do (in addition to the audit's WARNINGS)

- Do not invent a V2V-forcing tweak just to populate
  `log/orig/pool3_004063A0.csv` — the divergence in V2V dispatch is itself a
  separate precise-port finding; forcing it would mask the upstream cause.
- Do not merge this branch without a tick-aligned diff confirming the port's
  fixes match the original's outputs. The static port is conservative (avoids
  D3 specifically because Transform helpers aren't ported); a follow-up
  worktree must close D3 with byte-exact wheel-derived attitude.

## Reference

- Listing: 0x004063A0..0x00406617 (Ghidra TD5_pool3, 2026-05-14)
- Decompilation: same session, 87 lines
- Port (worktree): `td5mod/src/td5re/td5_physics.c:5152-5359`
- Switch table: data @ 0x00406618 (52 bytes, 13x dword)
- Frida probe: `tools/frida_pool3_004063A0.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_004063A0.{c,h}`
- Captured port CSV: `.claude/worktrees/precise-004063A0/log/port/pool3_004063A0.csv` (132 rows)
- Captured orig CSV: `log/orig/pool3_004063A0.csv` (0 rows; upstream-blocked)
