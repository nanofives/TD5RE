# Pilot Audit — 0x00404EC0 UpdateAIVehicleDynamics

**Date:** 2026-05-14
**Pool slot:** TD5_pool12
**Port-side function:** `td5_physics_update_ai` @ `td5_physics.c:1793`
**Worktree:** `.claude/worktrees/precise-00404EC0` on branch `precise-00404EC0` from `fpu-cw-fix`
**Callers:** `UpdateRaceActors @ 0x00436A70` (per slot loop, called via 0x406879)
**Callees:** GetTrackSegmentSurfaceType (0x42F100), ComputeVehicleSurfaceNormalAndGravity (0x42EBF0),
CosFixed12bit (0x40A6E0), SinFixed12bit (0x40A700), UpdateAutomaticGearSelection (0x42EF10),
UpdateEngineSpeedAccumulator (0x42EDF0), ComputeDriveTorqueFromGearCurve (0x42F030),
__ftol + FILD + FSQRT, IntegrateWheelSuspensionTravel (0x00403A20).
**Body:** 0x004057E5 - 0x00404EC0 = 0x925 (~2.3 KB).

## Function structure (from listing + decomp + port)

```
prologue (0x00404EC0..0x00404EF1):
    EBX = actor->tuning_data_ptr (= cardef)
    ESI = actor
    call GetTrackSegmentSurfaceType(&actor->track_span_raw) → AL = surface_type
    actor->surface_type_chassis = AL
    call ComputeVehicleSurfaceNormalAndGravity(actor)

velocity drag (0x00404EF4..0x00404F91):
    if (encounter_steer < 0x20 || gear <= 1):
        EDI = (surface_grip[surface] << 8) + cardef[0x6c]   ; damp_low path
    else:
        EDI = (surface_grip[surface] << 8) + cardef[0x6a]   ; damp_high path
    # SAR_RZ_8(v) := (v + (v>>31 & 0xff)) >> 8
    # SAR_RZ_12(v) := (v + (v>>31 & 0xfff)) >> 12
    actor->vx -= SAR_RZ_12(SAR_RZ_8(actor->vx) * EDI)
    actor->vz -= SAR_RZ_12(SAR_RZ_8(actor->vz) * EDI)

body-frame trig + velocities (0x00404F93..0x0040506A):
    yaw   = actor->euler_accum_yaw >> 8 [SAR — uses MOVZX/etc indeterminately here; check]
    steer = SAR_RZ_8(actor->steering_command)
    cos_h, sin_h, cos_s, sin_s, cos_d, sin_d via LUT calls
    raw_lat = SAR_RZ_12(cos_h * vx - sin_h * vz)
    body_long = SAR_RZ_12(cos_h * vz + sin_h * vx)

load transfer (0x0040506B..0x004050EF):
    front_load = ((Wr<<8) / (Wf+Wr)) * (half_wb - center_pos) / half_wb     [0x70..0xA0 clamp]
    rear_load  = ((Wf<<8) / (Wf+Wr)) * (half_wb + center_pos) / half_wb     [0x70..0xA0 clamp]
    NOTE: Original uses raw IDIV (no SAR bias on these) — Ghidra decomp shows
    plain `/` operators with no `(x>>31 & mask)` round-bias. Port matches.

lat_speed writeback (0x004050EF..0x0040514C):
    steered_long = SAR_RZ_12(cos_s * vx + sin_s * vz)
    yaw_corr_premult = SAR_RZ_12((sin_d * Wf) * yaw_rate)        [IMUL EBP, [esp+0x10], yaw_rate; then *0x30 imul]
    yaw_corr = (yaw_corr_premult + sign>>31&_used) / 0x28C        [magic IMUL by 0xc907da5 + SAR 5 + SHR 31 + ADD]
    actor->lateral_speed = steered_long - yaw_corr               [+0x318]
    actor->longitudinal_speed = body_long                        [+0x314]

throttle/brake branch (0x0040514C..0x00405285):
    if (brake_flag != 0):
        if (encounter_steer == 0) goto LAB_004051c7
        else fall into brake path with encounter_steer
    else:
        if (encounter_steer != 0):
            UpdateAutomaticGearSelection
            UpdateEngineSpeedAccumulator
            torque = ComputeDriveTorqueFromGearCurve
            local_40 = local_3c = SAR_RZ_2(torque)
            if (body_long > speed_limit<<8): local_40=local_3c=0
            goto LAB_00405285
        else LAB_004051c7: encounter_steer = -0x20
    LAB_004051c7:
    brake path (0x004051CF..0x00405281):
        UpdateEngineSpeedAccumulator
        F_raw = SAR_RZ_8(cardef[0x6e] * encounter_steer)   [brake_front]
        R_raw = SAR_RZ_8(cardef[0x70] * encounter_steer)   [brake_rear]
        # FRONT clamp: bound by actor->lateral_speed (= +0x318)
        local_3c = clamp(F_raw, -lat/2, F_raw, -F_raw — see nested compare) / 2
        # REAR clamp: bound by body_long (= +0x314)
        local_40 = clamp(R_raw, ..., body_long) / 2

LAB_00405285 — bicycle solve (0x00405285..0x004054F3):
    local_3c *= 2; local_40 *= 2
    det = ((Wr² + I)>>10 * cos_d² ...) - identical to port lines 2090-2096
    D   = (Wr*Wf - I) >> 10
    raw_lat used directly (NOT lateral_speed) in `(Wr*omega/0x28C - raw_lat)`
    yaw_term = (I/0x28C) * omega
    local_40 (REAR_LAT) = (rear_cross_term * cos_d + (yaw_corr + drive_cos + D_drive_sin²) * sin_d) / det
    local_44 (FRONT_LAT) = (front_cross_term * sin_d - (raw_lat*Wr + yaw_term)>>10 * (Wr+Wf) >> 12 * cos_d) / det

slip circle FRONT (0x004054F7..0x00405584):
    actor->current_slip_metric = 0       [+0x33C cleared]
    front_grip_limit = SAR_RZ_8(front_load_clamped * cardef[0x2c])   ; tire_grip
    fl4 = SAR EAX,4   ; local_44 >> 4
    fd4 = SAR ECX,4   ; local_3c >> 4   (after both have been shifted)
    mag_sq = fl4*fl4 + fd4*fd4
    EBP = SAR mag_storage_post_sar8 ?   ← need to trace; the original FILD reads [ESP+0x50] = mag_sq raw
    FILD [ESP+0x50]; FSQRT; CALL __ftol   → EAX = sqrt rounded toward zero
    slip_16 = EAX << 4
    if slip_16 > front_grip_limit:
        local_44_excess = slip_16 - front_grip_limit  → actor->+0x31C
        local_44 = SAR_RZ_8(local_44 * (front_grip_limit<<8) / slip_16)

slip circle REAR (0x00405584..0x00405612):
    actor->front_axle_slip_excess (= +0x31C) = local_44_excess
    [SAME shape: rear_grip_limit, fd4, fl4 for rear half, FILD+FSQRT+__ftol, SAR_RZ_8 rescale of local_40]
    actor->rear_axle_slip_excess (= +0x320) = local_40_excess

NOTE: Original write order at slip circle is non-obvious:
  - `+0x31C` field is written AFTER the front circle (at 0x004055A6)
  - `+0x320` field is written AFTER the rear circle (at 0x00405616)
  Port writes them in the same order — confirmed.

yaw torque (0x00405612..0x004056C4):
    M_pre = (SAR_RZ_12(cos_d * Wf) * local_44 - local_40 * Wr) / (I / 0x28C)
    clamp ±0x578
    actor->angular_velocity_yaw += M_pre

world-frame force (0x004056C4..0x0040575C):
    ai_fx = SAR_RZ_12(rear_drive*sin_h) + SAR_RZ_12(rear_lat*cos_h)
          + SAR_RZ_12(front_drive*sin_s) + SAR_RZ_12(front_lat*cos_s)
    ai_fz = SAR_RZ_12(rear_drive*cos_h) - SAR_RZ_12(rear_lat*sin_h)
          + SAR_RZ_12(front_drive*cos_s) - SAR_RZ_12(front_lat*sin_s)
    actor->vx += ai_fx
    actor->vz += ai_fz
    actor->tire_slip_x +=  (uint16_t)(lateral_speed >> 8)
    actor->tire_slip_z +=  (uint16_t)(longitudinal_speed >> 8)

slip-yaw damping (0x0040578E..0x004057CE):
    if (front_slip_excess > 0):
        damp_pre = SAR_RZ_6(ang_yaw) * front_slip_excess
        damp = SAR_RZ_15(damp_pre)
        clamp ±0x200
        actor->ang_yaw -= damp

tail: call IntegrateWheelSuspensionTravel(actor, cardef, ai_fx, ai_fz) (0x004057CE..0x004057E5)
```

## Audit progress

Source of truth: `0x00404EC0` listing (500 instructions) + decomp.
Port read: `td5_physics.c:1793-2248` (= `td5_physics_update_ai`).

The port is already line-by-line annotated as a verbatim port (see comments
referencing 0x40506B, 0x4050EF, 0x405285, 0x4056FA etc.). The 7-section
structure of the port matches the listing's blocks exactly.

Visual algorithmic divergences found before runtime trace:

### D1 — Velocity drag uses plain SAR, not round-to-zero (LOW-MED IMPACT)
`td5_physics.c:1860-1861`:
```c
actor->linear_velocity_x -= ((actor->linear_velocity_x >> 8) * damp_coeff) >> 12;
actor->linear_velocity_z -= ((actor->linear_velocity_z >> 8) * damp_coeff) >> 12;
```
Original (0x00404F3B-0x00404F58 + 0x00404F6A-0x00404F91):
```
CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8     ; SAR_RZ_8(vx)
IMUL EAX, EDI                                  ; * damp_coeff
CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc  ; SAR_RZ_12(prod)
SUB ECX, EAX                                   ; vx -= result
```
Same as pilot_00403A20 D1 pattern. Plain `>>` differs from round-to-zero for
negative non-aligned values. Effect: 1 LSB drift per negative-velocity tick,
compounds.

### D2 — Yaw-corr formula uses div, original uses signed magic IMUL (NO IMPACT)
Original at 0x00405126-0x00405135:
```
MOV EAX, 0xc907da5    ; magic constant
IMUL EDX               ; signed 64-bit multiply
SAR EDX, 5
MOV EAX, EDX
SHR EAX, 0x1f          ; sign-bit
ADD EDX, EAX
```
This is GCC/MSVC's signed-divide-by-0x28C inlined. Algebraically identical
to `x / 0x28C` for signed 32-bit operands. Port uses `/ 0x28C` directly —
results match byte-for-byte under correct C semantics. NO actual divergence.

### D3 — Tire-grip fallback when cardef[0x2C]==0 (UNLIKELY-HIT)
Port at `td5_physics.c:2149-2150`:
```c
int32_t tire_grip = (int32_t)PHYS_S(actor, 0x2C);
if (tire_grip == 0) tire_grip = sf; /* fallback */
```
Original has NO fallback — uses `sVar1` unconditionally. Effect: only hits
if Viper carparam loads with tire_grip=0, which shouldn't happen. Risk only
manifests if cardef-loading sets it to 0; under normal carparam it's 2900.

### D4 — slip_metric clear is unconditional (NO IMPACT)
Port at `td5_physics.c:2243-2246` writes `current_slip_metric` from
`|lateral_speed|>>8` at the end. Original at 0x00405511 writes 0 BEFORE the
slip-circle (`MOV [ESI+0x33C], AX` with AX=0), then nothing else touches it
in this function. The port's tail-write is an ADDITION that overwrites the
zero clear.

Per the existing port comment (lines 2244-2245), this is intentional to
drive the slip-metric-driven tire-mark/smoke pipeline for AI cars. Not a
faithful divergence but is a documented enhancement.

### D5 — `s_loaded_tuning` fallback for PlayerIsAI=1 slot 0 (UPSTREAM)
Identical to pool5's D4 — port doesn't load Viper carparam for slot 0 when
`PlayerIsAI=1` on Edinburgh AutoRace. All cardef fields fall back to the
hardcoded defaults from `LoadVehicleAssets`. Affects EVERY input column to
the bicycle solve simultaneously.

Captured row 1 (sim_tick=1) comparison:

| Column | port | orig | port has |
|---|---|---|---|
| `inertia` | 98304 | 180000 | fallback |
| `half_wb` | 1536 | 12000 | fallback |
| `Wf, Wr` | 512, 512 | 414, 397 | fallback |
| `tire_grip` | 0 | 2900 | fallback (and triggers D3 path!) |
| `k_spring` | 48 | 30 | fallback |
| `damp_high, damp_low` | 32, 16 | 300, 3000 | fallback |
| `brake_front, brake_rear` | 288, 256 | 600, 600 | fallback |
| `speed_limit` | 160 | 910 | fallback |
| `track_span_raw` | 0 | 62 | port spawned at span 0, not Edinburgh start span |
| `surface_chassis_in` | 0 | 3 (paved) | port off-track |
| `ang_yaw_in` (sim_tick=1) | 0 | 732672 | downstream of all the above |
| `steer_cmd_in` (sim_tick=1) | 0 | 20000 | downstream of all the above |

Conclusion: every algorithmic input to UpdateAIVehicleDynamics differs on
sim_tick=1, so output validation against the original is impossible without
fixing the upstream cardef-load + spawn-bind chain.

## Runtime diff — BLOCKED BY UPSTREAM POOL8 + spawn-bind

Same blocker class as pool5 (0x00403A20) and pool6 (0x004057F0). Per
memory entry `todo_playerisai_carparam_binding.md`, the binding fix was
shipped to master (commit 48d320a) but the worktree branched from
`fpu-cw-fix` which appears to predate that fix, OR the AutoRace launch
path takes a different code path that bypasses the binding fix.

Confirmed input divergence on first sim_tick:
- `cardef_ptr`: 0x00825920 (port) vs 0x004AE280 (orig). The port HAS a
  cardef pointer (not null), but the loaded values are the fallback
  defaults — meaning the carparam loader was called but loaded the
  WRONG file or fell through to `LoadVehicleAssets`'s defaults.

## Static port deliverables (this branch)

- **Probe + emitter installed**: `tools/frida_pool12_00404EC0.js`,
  `td5_pilot_trace_00404EC0.{c,h}`, wired at entry/exit of
  `td5_physics_update_ai`.
- **CSVs captured**: 36 rows in `log/orig/pool12_00404EC0.csv` and 13 rows
  in `log/port/pool12_00404EC0.csv` (over 30 sim_ticks of Edinburgh
  AutoRace with `PlayerIsAI=1` slot 0). Schemas match column-for-column.
- **Audit committed** with line-by-line listing → port mapping above.
- **One real low-impact divergence noted but NOT fixed** (D1) — applying
  the SAR-RZ idiom on velocity drag is a 1-LOC change but masks the much
  larger upstream blockers, so deferred until the cardef binding lands.

## What NOT to do

- Do NOT "fix" the bicycle-solve internals to compensate for wrong cardef
  inputs — the solver math is already byte-faithful per static audit and
  per `memory/reference_lat_speed_steered_frame_projection.md`.
- Do NOT remove the tire-grip fallback (`tire_grip == 0 → sf`) here —
  it's a port safety net that activates ONLY because of D5. Once D5 is
  fixed and tire_grip is the real 2900, the fallback becomes inert.
- Do NOT chase the lat_speed/lspd projection — `memory/
  reference_lat_speed_steered_frame_projection.md` confirms it's
  by-design steered-frame projection (cos(33.75°) ratio at saturated steer).

## Definition of done

Zero diff on (lin_vx_out, lin_vz_out, ang_yaw_out, long_speed_out,
lat_speed_out, front_slip_excess_out, rear_slip_excess_out) at sim_tick ≥
5 in `pool12_00404EC0.csv` once upstream inputs are aligned.

Current state: STATIC PORT AUDITED (one optional SAR-RZ fix on velocity drag
documented as D1) + INPUT DIVERGENCE TRACED TO UPSTREAM POOL8 (cardef
binding for PlayerIsAI=1 AutoRace slot 0). PAUSED.

## Reference

- Listing: 0x00404EC0..0x004057E5 (Ghidra TD5_pool12, 2026-05-14)
- Decompilation: 95 lines, captured in this audit
- Port: `td5mod/src/td5re/td5_physics.c:1793-2248`
- Frida probe: `tools/frida_pool12_00404EC0.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_00404EC0.{c,h}` (worktree)
- Orig CSV: `log/orig/pool12_00404EC0.csv` (36 rows, 30 sim_ticks)
- Port CSV: `.claude/worktrees/precise-00404EC0/log/port/pool12_00404EC0.csv`
  (13 rows; upstream-blocked)
