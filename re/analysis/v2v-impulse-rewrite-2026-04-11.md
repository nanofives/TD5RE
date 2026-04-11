# V2V Impulse Rewrite — 2026-04-11

## Commit attribution

The V2V collision impulse rewrite landed in **commit `03e6811`** alongside an
unrelated alpha-bleed texture fix. The commit message on `03e6811` reads
`"fix: alpha bleed world textures (DAT + loose PNG paths)"` and only documents
the `td5_asset.c` changes — it does NOT mention the V2V rewrite, which is the
bulk of the diff (400 lines in `td5_physics.c`). This document records what
actually shipped so future archaeology does not miss it.

**File touched:** `td5mod/src/td5re/td5_physics.c`, function
`apply_collision_response` (~line 1497).

## Why it was rewritten

The prior `apply_collision_response` used a naive "2D minimum-penetration-axis"
contact frame: pick the axis with the smaller `abs(pen)` between `pen_x` and
`pen_z`, treat that as the contact normal, apply a symmetric restitution
impulse. This did not match the original binary in three fundamental ways:

1. **Contact frame derivation is wrong.** The original case-splits on the sign
   of `contactData[1]` (cz_A) and compares against cardef extents to choose
   between a *side impact* branch (tangent velocity channel, `z²` in the mass
   polynomial) and a *front/rear* branch (normal channel, `x²`). The port
   conflated the two into a single symmetric model.
2. **TOI sub-step missing.** The original rolls back position and yaw
   accumulator by `(0x100 - impactForce) * velocity >> 8` before committing
   the new velocities, then re-advances by the same fraction with the new
   velocities. The port skipped this entirely — visible as "jitter on contact"
   because each impulse shifted position by half a frame of velocity delta.
3. **Separating-contact rejection missing.** The original early-outs when
   `((cx_B - cx_A) ^ impulse) < 0` (side) or `((cz_B - cz_A) ^ impulse) < 0`
   (front) — the impulse direction implies the bodies are already moving
   apart. The port applied the impulse unconditionally.

The many prior "collision fix" commits in the git log (f25328b, 52f95bd,
0e00828, 3cab5f8, 714a655, 26efb5c, 9f52fbb, 98bd141, cd6560c, and others)
were chasing symptoms of these three structural gaps by flipping signs in
the lateral wall branch — which is architecturally correct and was never the
problem.

## RE basis

All findings tagged `[CONFIRMED @ 0xADDR]` come from two Ghidra MCP passes on
`TD5_d3d.exe` against `ApplyVehicleCollisionImpulse` (`0x4079C0`) and its
caller `ResolveVehicleCollisionPair` (`0x408A60`).

| Address | Original name | Role |
|---------|---------------|------|
| `0x4079C0` | `ApplyVehicleCollisionImpulse` | Per-corner impulse solver (body `0x4079C0..0x408566`) |
| `0x4079C8` | prologue | `iVar10 = actorA[0x1C4]` (save ω_A); `iVar11 = actorB[0x1C4]` (save ω_B) |
| `0x4079EB` | rotation setup | `cos = CosFixed12bit(angle)`, `sin = SinFixed12bit(angle)` |
| `0x407A0A..0x407AA9` | velocity rotation | Rotate both actors' linear velocities into A's local frame |
| `0x407AA5..0x407AC2` | contact unpack | Read `contactData[0..3]` as cx_A, cz_A, cx_B, cz_B |
| `0x407AF6..0x407AFF` | case split | Compare cz_A sign + extent calc to pick side vs. front/rear |
| `LAB_00407B7F` | side branch | Uses `z²` in mass poly, tangent channel (`local_54`/`local_4c`) |
| `LAB_00407B2D` | front branch | Uses `x²` in mass poly, normal channel (`local_50`/`local_44`) |
| `0x407D18` | TOI rollback | `iVar12 = 0x100 - impactForce`; roll pos/euler back |
| `0x408107..0x408115` | post-impulse | `UpdateVehiclePoseFromPhysicsState` on both |
| `0x408A60` | `ResolveVehicleCollisionPair` | Caller; broadphase + bisection + per-corner dispatch |

Key constants:

| Constant | Value | Role |
|----------|-------|------|
| `DAT_00463204` | `500000` | Inertia base (`INERTIA_K`) |
| `0x1100` | `4352` | Impulse numerator scale (`NUM_SCALE`) |
| `0x28C` | `652` | Angular divisor |
| `500000 / 652` | `766` | `INERTIA_PER_ANG` (integer division, not 767, not a power of 2) |
| `cardef+0x04` | short | Front-Z extent (positive) |
| `cardef+0x08` | short | Half-width |
| `cardef+0x14` | short | Rear-Z extent (stored negative; `NEG` after `MOVSX` at `0x407ADB`) |
| `cardef+0x88` | short | Mass parameter |

## What the rewrite does

The port's caller interface is preserved:

```c
static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target);
```

Internally the function now maps:

- **A** = `target` (frame owner, whose yaw = `heading_target` = `angle`)
- **B** = `penetrator` (the other actor)

Algorithm:

1. **Prologue.** Save `saved_omega_A`, `saved_omega_B` for delta-based commit.
2. **Rotate** both velocities into A's local frame via `cos_fixed12(angle)`
   and `sin_fixed12(angle)`. Produces `local_54`/`local_50` (A tangent/normal)
   and `local_4c`/`local_44` (B tangent/normal).
3. **Case split** on `cz_A` sign: if `cz_A <= 0`, contact is on A's rear half
   — compare `rear_depth = |cz_A - rear_z_A|` against `side_extent = half_w_A
   - |cx_A|` and route to the side branch if `rear_depth > side_extent`. If
   `cz_A > 0`, contact is on A's front half — compute `front_depth = |front_z_A
   - cz_A|` and route to side branch if `side_extent < front_depth`.
4. **Side branch.** Positional push `±cos/2, ±sin/2` (sign from `cx_A - cx_B`).
   Mass polynomial `(cz_B² + K)*mA + (cz_A² + K)*mB`. Relative velocity on
   tangent channel with angular contribution from `(cz_B*ω_B - cz_A*ω_A)/0x28C`.
   XOR rejection `((cx_B - cx_A) ^ impulse) < 0` → skip. Otherwise update
   `local_54 += impulse*mA`, `local_4c -= impulse*mB`, angular deltas using
   `cz` lever arms.
5. **Front branch.** Positional push `±sin/2, ±cos/2` (sign from `cz_A - cz_B`).
   Mass polynomial `(cx_B² + K)*mA + (cx_A² + K)*mB`. Relative velocity on
   normal channel with angular contribution from cx. XOR rejection
   `((cz_B - cz_A) ^ impulse) < 0` → skip. Otherwise update `local_50 += ...`,
   `local_44 -= ...`, angular deltas sign-flipped relative to side branch.
6. **TOI rollback.** `toi_frac = 0x100 - impactForce`. Subtract
   `(toi_frac * linear_velocity) >> 8` from `world_pos.x/z` and
   `(toi_frac * angular_velocity_yaw) >> 8` from `euler_accum.yaw` on both
   actors.
7. **Commit angular.** `angular_velocity_yaw = saved_omega + delta` for both.
8. **Rotate velocities back** to world frame from the now-updated
   `local_50/54/4c/44` channels.
9. **TOI re-advance.** Add `(toi_frac * new_velocity) >> 8` to both actors'
   positions and euler accumulators — same formula as step 6 but additive and
   with the post-impulse velocities.
10. **Pose update.** `update_vehicle_pose_from_physics` on both.
11. **Impact magnitude & damage.** `impact_mag = |(mA + mB) * impulse|`.
    Traffic recovery escalation (`> 50000`). Heavy-impact visual scatter
    (`> 90000` and `g_collisions_enabled == 0`).

## Known approximations and follow-ups

These are tracked as ongoing work in
[`todo_collision_engine_gaps.md`](../../memory/todo_collision_engine_gaps.md):

### 1. `impactForce` is hardcoded to `0x70`

The original receives `impactForce` as a parameter from the caller's 7-iteration
binary-search TOI bisection (`local_84 - 0x10` at `0x408D26`, typical range
`[0x10..0xF0]`). The port's `collision_detect_full` does a different bisection
that shrinks/grows `car_def` extents in place (also architecturally wrong — see
next item) and does not track a TOI fraction. Fixing this requires rewriting
the caller to match `ResolveVehicleCollisionPair` at `0x408A60`, which does
position-based bisection on stack copies.

`0x70` was chosen because it is near the middle of the typical runtime range
and corresponds to `toi_frac = 0x100 - 0x70 = 0x90` — approximately 56% of
the pre-step fraction remains. Collisions will feel reasonable but the exact
energy transfer and position correction will not be frame-accurate.

### 2. Binary search mutates `car_def` memory

The port's `collision_detect_full` (line ~1787) writes to the car_def struct
during its 7 iterations (`write_i16` calls on `test_hw_a`, `test_hl_a`,
`test_nw_a`, etc.). It restores the original values at the end, so there is
no persistent corruption — but this is architecturally wrong: the original's
`ResolveVehicleCollisionPair` at `0x408A60` applies all deltas to stack copies
of the actor position/velocity and never touches car_def. Fixing this is
tangential to the impulse rewrite but should be addressed when the caller is
rewritten to produce a real TOI fraction (see item 1).

### 3. Contact data `[cx_B, cz_B]` synthesis is an approximation

The original's `contactData[4]` comes from `CollectVehicleCollisionContacts`
(`0x408570`), which was not fully decompiled in the passes for this rewrite.
The port synthesizes `cx_B, cz_B` as `(-corner->proj_x + corner->pen_x,
-corner->proj_z + corner->pen_z)` — an equal-and-opposite lever arm
approximation. This satisfies the algebraic structure of the impulse formula
and gives symmetric angular response but is not bit-exact. Fixing this
requires fully decoding `0x408570` and rewriting both the OBB corner test
and its output format to emit the original's 4-short-per-record layout.

### 4. Heavy-impact visual scatter uses deterministic approximation

The original's `GetDamageRulesStub()` path uses an RNG to scatter roll/yaw/
pitch accumulators and (for traffic slots) rebuilds the rotation matrix and
switches to `vehicle_mode = 1`. The port uses a deterministic formula
(`scatter = impact_mag / 4`) which is simpler but not faithful. Fixing this
requires `GetDamageRulesStub` decomp and a working RNG-compatible seed.

## Tracing

Two new log lines were added to aid runtime verification:

- `v2v_impulse: side={0|1} slot_A=%d slot_B=%d mA=%d mB=%d cxA=%d czA=%d cxB=%d czB=%d imp=%d mag=%d toi=%d`
- `v2v_reject: slot_A=%d slot_B=%d side=%d cxA=%d czA=%d cxB=%d czB=%d imp=%d`

Both write to `log/race.log` under the `physics` module tag. A race with
close-quarters overtaking (cop chase, traffic-heavy street circuits) will
exercise both branches and the rejection path.

## Related documents

- [`re/analysis/collision-system.md`](collision-system.md) — full V2V and V2W
  collision system overview (covers broadphase, narrowphase, caller dispatch,
  actor field offsets)
- [`memory/todo_collision_engine_gaps.md`](../../memory/todo_collision_engine_gaps.md)
  — the three structural gaps identified by the 2026-04-11 Ghidra passes
  (Forward/Reverse wall handlers, V2V impulse rewrite [this doc], TOI sub-step)
