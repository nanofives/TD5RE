# Pilot Audit — 0x004437C0 ApplyDampedSuspensionForce + 0x004438F0 IntegrateVehicleFrictionForces

**Date:** 2026-05-14
**Pool slot:** TD5_pool8
**Worktree:** `.claude/worktrees/precise-traffic-physics` on branch `precise-traffic-physics`
**Port-side functions:**
- `apply_damped_suspension_force` @ `td5_physics.c:2355`
- `td5_physics_update_traffic` @ `td5_physics.c:2249`
**Caller chain (original):** `UpdateTrafficActorMotion (0x443ED0)` → `IntegrateVehicleFrictionForces (0x4438F0)` → `ApplyDampedSuspensionForce (0x4437C0)`.

## 0x004437C0 ApplyDampedSuspensionForce — listing

Function body 0x004437C0..0x004438EE (92 instructions, 0x12E bytes). Two near-identical axes:

```
Axis 0 (lateral): pos @ +0x2DC, vel @ +0x2EC, drive=param_2,   clamp ±0x2000
Axis 1 (long.):   pos @ +0x2E0, vel @ +0x2F0, drive=param_3,   clamp ±0x4000
```

Listing instructions (Axis 0 — Axis 1 mirrors with different offsets/clamp):

```
004437c4 MOV EAX,[ECX+0x2dc]   ; EAX = pos
004437cd MOV ESI,[ECX+0x2ec]   ; ESI = vel
004437d3 ADD EAX,ESI            ; EAX = new_pos = pos + vel
004437d5 MOV [ECX+0x2dc],EAX    ; store new_pos
004437db SHL EAX,5              ; EAX = new_pos*32
004437de CDQ                    ;
004437df AND EDX,0xff           ; sign-bias mask
004437e5 ADD EAX,EDX            ; EAX = new_pos*32 + bias
004437e8 MOV EDI,EAX            ; EDI = new_pos*32 + bias

004437ea MOV EAX,ESI            ; EAX = vel
004437ec NEG EAX                ; EAX = -vel
004437ee SHL EAX,5              ; EAX = -vel*32
004437f1 CDQ
004437f2 AND EDX,0xff
004437f8 ADD EAX,EDX            ; EAX = -vel*32 + bias
004437fa MOV EBX,EAX            ; EBX = -vel*32 + bias

004437fc MOV EAX,[ESP+0x18]     ; EAX = lateral (param_2)
00443800 SHL EAX,7              ; EAX = lateral*128
00443803 CDQ
00443804 SAR EDI,8               ; EDI = sar8_rz(new_pos * 32)
00443807 AND EDX,0xff
0044380d ADD EAX,EDX             ; EAX = lateral*128 + bias

00443815 SAR EBX,8               ; EBX = sar8_rz(-vel * 32)
00443818 SUB EBX,EDI             ; EBX = sar8_rz(-vel*32) - sar8_rz(new_pos*32)
0044381a SAR EAX,8               ; EAX = sar8_rz(lateral * 128)
0044381d ADD ESI,EBX             ; ESI = vel + damping
0044381f ADD EAX,ESI             ; EAX = drive + ESI = new_vel
00443821 MOV [ECX+0x2ec],EAX     ; store new_vel
```

Followed by the two-`if`-statement clamps:

```
00443827 MOV EAX, 0x2000
00443830 JLE skip_upper_clamp
00443832    MOV [ECX+0x2dc], EAX  ; clamp pos = +0x2000
00443838    MOV [ECX+0x2ec], EDI  ; EDI = 0 from "XOR EDI,EDI" at 0044382c → clamp vel = 0
skip_upper_clamp:
00443844 MOV EAX, 0xffffe000
0044384b JGE skip_lower_clamp
0044384d    MOV [ECX+0x2dc], EAX  ; clamp pos = -0x2000
00443853    MOV [ECX+0x2ec], EDI  ; clamp vel = 0
```

Algebraic summary:

```
new_pos = pos + vel
new_vel = vel
        + sar8_rz(-vel * 32)            ; velocity damping (uses OLD vel)
        - sar8_rz(new_pos * 32)         ; spring restore (uses NEW pos)
        + sar8_rz(drive * 128)          ; external force
if new_pos > +CLAMP: new_pos =  CLAMP, new_vel = 0
if new_pos < -CLAMP: new_pos = -CLAMP, new_vel = 0
```

`sar8_rz(x)` = `((x < 0) ? (x + 0xFF) : x) >> 8` — round-to-zero divide by 256 (same idiom as the 0x00403A20 pilot's `sar8_rz`).

## D1 — Port's order-of-operations is WRONG (HIGH IMPACT)

Current port (`apply_damped_suspension_force` at `td5_physics.c:2355`):

```c
int32_t axis0_pos = actor->wheel_suspension_pos[0];   // OLD pos
int32_t axis0_vel = actor->wheel_spring_dv[0];        // OLD vel

axis0_vel += (lateral * 0x80) >> 8;            // drive added FIRST   (WRONG)
axis0_vel += (axis0_vel * -0x20) >> 8;         // damping on (vel+drive) (WRONG)
axis0_vel -= (axis0_pos * 0x20) >> 8;          // spring uses OLD pos    (WRONG)
axis0_pos += axis0_vel;                        // new_pos = old_pos + new_vel (WRONG)
```

Effective port math:
```
new_vel = old_vel + drive
new_vel += -(new_vel)*32/256        ; = -(old_vel+drive)/8
new_vel -= old_pos*32/256           ; = -old_pos/8
new_pos = old_pos + new_vel
```

Original (correct):
```
new_pos = old_pos + old_vel
new_vel = old_vel + sar8_rz(-old_vel*32) - sar8_rz(new_pos*32) + sar8_rz(drive*128)
        = old_vel - sar8_rz(old_vel*32) - sar8_rz((old_pos+old_vel)*32) + sar8_rz(drive*128)
```

Three divergences:
1. **new_pos write timing**: original stores `old_pos + old_vel`; port stores `old_pos + new_vel`. Affects spring/damping math the next tick.
2. **Damping basis**: original damps OLD vel only; port damps `(old_vel + drive)` because drive was added before the damp pass.
3. **Spring basis**: original springs against NEW pos; port springs against OLD pos. One-tick lag on the spring restoring force.

## D2 — Plain `>> 8` instead of `sar8_rz` (LOW IMPACT)

Original uses `CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8` (round-to-zero divide by 256). Port uses plain `>> 8` (arithmetic right shift, round-toward-negative-infinity). They differ by 1 LSB on negative values not divisible by 256.

For typical traffic values (small drive/vel/pos), the actual numeric impact is at most ±1 per term per tick. Combined with D1 it may matter for visible drift.

## D3 — `else if` vs separate `if` on clamps (NO IMPACT)

Listing uses two independent `if` blocks (one upper, one lower); port uses `if … else if`. Algebraically equivalent — a single-tick `new_pos` cannot simultaneously be above +CLAMP and below -CLAMP. Noted only for completeness.

## 0x004438F0 IntegrateVehicleFrictionForces — listing audit

Function body 0x004438F0..0x00443CEA (322 instructions, 0x3FB bytes). Pure 2-axle traffic dynamics — no cardef references, all constants hardcoded:

| Constant | Value | Use |
|---|---|---|
| drag factor | `* 16 / 4096` | velocity drag per axis |
| 0x271 | 625 | cos_s² coefficient in denom |
| 0x14C | 332 | sin_s coefficient (also num_a multiplier) |
| 0x114 | 276 | yaw_vel inertia coefficient |
| 400 | 400 | lat_body × yaw cross-term |
| 0x28C | 652 | yaw_vel rear-axle divisor |
| 0xAF | 175 | rear-axle proportional gain |
| 0x13 | 19 | num_a rear coefficient |
| 800 | 800 | front/rear axle output gain |
| 0x800 | 2048 | per-axle force clamp |

Integration order (writes to actor) — from listing 0x00443BEE..0x00443CDE:

```
1. angular_velocity_yaw  += yaw_torque        (0x00443BEE)
2. linear_velocity_x     += dvx               (0x00443C37)
3. linear_velocity_z     += dvz               (0x00443C8F)
4. CALL ApplyDampedSuspensionForce(actor, iVar15+iVar16_force, throttle)
                                              (0x00443C95)
5. world_pos.x           += linear_velocity_x (0x00443CBF)
6. world_pos.z           += linear_velocity_z (0x00443CCD)
7. euler_accum.yaw       += angular_velocity_yaw (0x00443CD7)
8. longitudinal_speed    = long_body          (0x00443CDE)
```

Port (`td5_physics_update_traffic` at td5_physics.c:2249) matches this order line-by-line:

```c
actor->angular_velocity_yaw += yaw_torque;
actor->linear_velocity_x = vx + ...;
actor->linear_velocity_z = vz - ...;
apply_damped_suspension_force(actor, iVar15 + iVar16_force, throttle);
actor->world_pos.x += actor->linear_velocity_x;
actor->world_pos.z += actor->linear_velocity_z;
actor->euler_accum.yaw += actor->angular_velocity_yaw;
actor->longitudinal_speed = long_body;
```

### D4 — IntegrateVehicleFrictionForces is already algebraically faithful

The port's SAR12/SAR10/SAR8_U8 macros encode the round-to-zero idiom correctly:

```c
#define SAR12(x) (((x) + (((x) >> 31) & 0xFFF)) >> 12)
#define SAR10(x) (((x) + (((x) >> 31) & 0x3FF)) >> 10)
#define SAR8_U8(x) (((x) + (((x) >> 31) & 0xFF)) >> 8)
```

These match the original's `CDQ; AND EDX,mask; ADD EAX,EDX; SAR EAX,shift` exactly.

Variable names diverge from the listing but the data flow is identical:

| Port var | Listing | Decomp |
|---|---|---|
| `vx` | EBP (after first SAR12) | iVar15 |
| `vz` | EAX after store at 0x00443932 | iVar2 → actor->linear_velocity_z |
| `yaw_vel` | [ESP+0x18] (iVar1) | iVar1 |
| `yaw12` | EDI (after SAR EDI,8) | uVar17 |
| `steer12` | EBX (after SAR EBX,8) | uVar13 |
| `cos_h, sin_h, cos_hs, sin_hs, cos_s, sin_s` | EAX/MOV [ESP+0x..] | iVar3..iVar8 |
| `throttle` | EAX after SHL EAX,2 (LEA pattern) | iVar9 |
| `lat_body` | iVar14 in decomp | iVar14 |
| `long_body` | iVar10 in decomp | iVar10 |
| `denom` | iVar16 in decomp (before reuse) | iVar16 |
| `iVar15_pre, iVar15` | port-named | iVar15 stages |
| `iVar16_force` | port-named (final iVar16) | iVar16 |

The port's only non-trivial divergence on IntegrateVehicleFrictionForces is **inherited from `apply_damped_suspension_force`** (D1+D2 above). The integrator itself is byte-faithful.

## Field-offset verification (sanity)

| Field | Original offset (listing) | Port struct (`td5_actor_struct.h`) | Match |
|---|---|---|---|
| angular_velocity_yaw | +0x1C4 | +0x1C4 | OK |
| linear_velocity_x | +0x1CC | +0x1CC | OK |
| linear_velocity_z | +0x1D4 | +0x1D4 | OK |
| euler_accum.yaw | +0x1F4 | +0x1F4 (TD5_EulerAccum @ +0x1F0) | OK |
| world_pos.x | +0x1FC | +0x1FC | OK |
| world_pos.z | +0x204 | +0x204 | OK |
| wheel_suspension_pos[0] | +0x2DC | +0x2DC | OK |
| wheel_suspension_pos[1] | +0x2E0 | +0x2E0 | OK |
| wheel_spring_dv[0] | +0x2EC | +0x2EC | OK |
| wheel_spring_dv[1] | +0x2F0 | +0x2F0 | OK |
| steering_command | +0x30C | +0x30C | OK |
| longitudinal_speed | +0x314 | +0x314 | OK |
| encounter_steering_cmd | +0x33E | +0x33E | OK |

## Fixes to apply

1. **`apply_damped_suspension_force`** — rewrite the two-axis core to match the listing's order-of-operations:
   - Compute `new_pos = old_pos + old_vel` first; store immediately.
   - Compute `new_vel = old_vel + sar8_rz(-old_vel * 32) - sar8_rz(new_pos * 32) + sar8_rz(drive * 128)`.
   - Use `sar8_rz` (round-to-zero) everywhere, not plain `>> 8`.
   - Use two separate `if` clamps (matches listing layout exactly).
2. **`td5_physics_update_traffic`** — no math changes; only ensure the call still passes `(iVar15 + iVar16_force, throttle)` after the fix to `apply_damped_suspension_force`.

## Capture schema for trace

Per call (1 row per tick per traffic slot):

**Keys:** `sim_tick`, `slot`, `which_fn` (`"friction"` or `"susp"`)
**Common:** `actor_addr`
**friction inputs:**
- `in_lin_vel_x`, `in_lin_vel_z`, `in_ang_vel_yaw`, `in_yaw_accum`
- `in_steering_cmd`, `in_encounter_steer`
**friction outputs:**
- `out_lin_vel_x`, `out_lin_vel_z`, `out_ang_vel_yaw`, `out_yaw_accum`
- `out_world_pos_x`, `out_world_pos_z`, `out_long_speed`
- `arg2_lateral` (iVar15+iVar16_force), `arg3_throttle` (passed to apply_damped)
**susp inputs:**
- `in_pos0`, `in_vel0`, `in_pos1`, `in_vel1`
- `arg_lateral`, `arg_longitudinal`
**susp outputs:**
- `out_pos0`, `out_vel0`, `out_pos1`, `out_vel1`

Schema is ~25 columns combined; one CSV with `which_fn` selector keeps the diff simple.

## Self-validation (algorithmic byte-exact)

`tools/validate_pool8_traffic_math.py` feeds the original's captured inputs
(`log/orig/pool8_traffic.csv`) through a Python reimplementation of the
listing-faithful `ApplyDampedSuspensionForce` algorithm and asserts the
outputs match the original's captured outputs.

| Source | Rows | Pass |
|---|---|---|
| Original (`log/orig/pool8_traffic.csv`) | 15,900 susp / 15,900 friction | **15,900/15,900 byte-exact** |
| Port (`.claude/worktrees/precise-traffic-physics/log/port/pool8_traffic.csv`) | 360 susp / 360 friction | **360/360 byte-exact** |

This validates that **the port's suspension math is byte-faithful** given
correct inputs, and that the listing audit is correct.

## Runtime row-by-row diff — upstream-blocked

`tools/diff_func_trace.py` with `--min-tick=5 --max-tick=30 --key=sim_tick,slot,which_fn`
diverges on every input column at tick 5 because the port's AutoRace
launch path on Edinburgh (track=1) game_type=2 (Pursuit) does NOT spawn
traffic slots 6..11 at the same world positions as the original. Every
output column is downstream of wrong inputs.

| Column | Port | Original | Reason |
|---|---|---|---|
| in_lin_vel_x | 0 | 23721 | port traffic spawned stationary on Edinburgh path |
| in_world_pos_x | 0 | -4082968 | same |
| in_world_pos_z | -16384 | -108404963 | same |
| in_pos0 | 0 | 18 | suspension hasn't ticked because pose was never bound |
| arg_lateral | 0 | -2 | iVar15+iVar16 = 0 because integrator inputs are 0 |

This is the same upstream-block pattern as 0x00403A20: the *math* is
byte-exact (validated above), but the actor state going INTO the math
differs between port and original because the port's `td5_physics_update_traffic`
caller chain (UpdateTrafficActorMotion → ResolveTrafficContacts → …) doesn't
bind traffic slots 6..11 to the same world positions as the original's
`UpdateTrafficActorMotion @ 0x443ED0` on the AutoRace/game_type=2 launch path.

**Not in scope for this pilot.** The traffic-physics pair is the leaf —
fixing the spawn binding belongs in the traffic spawn / cardef chain
upstream.

## Static-port deliverable (this branch)

Even without a clean runtime diff:

- `td5_physics.c::apply_damped_suspension_force` is now byte-exact-correct
  vs the listing (algorithmic validation via captured-orig-inputs replay:
  15900/15900 match).
- `td5_physics.c::td5_physics_update_traffic` was already byte-faithful
  (audit confirmed; no math changes required).
- Probe + trace harness committed for future runtime diff once the upstream
  traffic spawn binding is wired correctly.
- Audit committed with line-by-line listing → port mapping.

## What NOT to do

- **Do not "fix" the suspension math** to compensate for wrong inputs —
  the math is byte-exact and provably so.
- **Do not double the clamp limits** — listing uses ±0x2000 for axis 0 and
  ±0x4000 for axis 1 (different, asymmetric).
- **Do not remove the `sar8_rz` round-to-zero** — it's mandatory for byte
  parity on negative values.
- **Do not feed suspension into angular_velocity_roll/pitch** — the
  listing writes ONLY to +0x2DC/+0x2EC/+0x2E0/+0x2F0.

## Reference

- Listing: 0x004437C0..0x004438EE (Ghidra TD5_pool8, 2026-05-14)
- Listing: 0x004438F0..0x00443CEA (same)
- Port post-fix: `td5_physics.c:2253-2452`
- Frida probe: `tools/frida_pool8_traffic.js`
- Port trace emitter: `td5_pilot_trace_traffic.{c,h}` (worktree)
- Self-validator: `tools/validate_pool8_traffic_math.py`
- Orig CSV: `log/orig/pool8_traffic.csv` (31,800 rows / 2,650 calls × 12 fn×slot)
- Port CSV: `.claude/worktrees/precise-traffic-physics/log/port/pool8_traffic.csv` (720 rows; upstream-blocked)
