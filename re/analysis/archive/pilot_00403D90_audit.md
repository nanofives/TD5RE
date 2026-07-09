# Pilot Audit — 0x00403D90 UpdateVehicleState0fDamping

**Date:** 2026-05-14
**Pool slot:** TD5_pool6
**Port-side function:** `td5_physics_state0f_damping` @ `td5_physics.c:5843`
**Worktree:** `.claude/worktrees/precise-00403D90` on branch `precise-00403D90`
**Caller graph:** `UpdateVehicleActor (0x00406650)` — single caller, branch reached only when actor state byte forced to 0x0F.
**Callee graph:** `UpdateVehicleEngineSpeedSmoothed (0x0042ED50)`, `IntegrateWheelSuspensionTravel (0x00403A20, called with accel=0,0)`, `CosFixed12bit (0x0040A6E0)`, `SinFixed12bit (0x0040A700)`.
**Body:** 0x00403D90..0x00403EAD (0x11D bytes / 86 instructions / ~45 decompiled lines).

## Function structure (from listing 00403D90..00403EAD)

```
prologue + side-effect calls:
    EDI = actor->tuning_data_ptr                              ; +0x1BC
    UpdateVehicleEngineSpeedSmoothed(actor)
    IntegrateWheelSuspensionTravel(actor, cardef, 0, 0)       ; accel_x=0, accel_z=0
    EDI = (int16_t) actor->display_angle_yaw                  ; +0x20A (MOVSX)
    NEG EDI                                                   ; EDI = -yaw
    actor->surface_contact_flags = 0                          ; +0x376
    actor->front_axle_slip_excess = 0                         ; +0x31C
    actor->rear_axle_slip_excess  = 0                         ; +0x320
    EAX = CosFixed12bit(-yaw)
    EBX = EAX                                                 ; cache cos(-yaw)
    EAX = SinFixed12bit(-yaw)
    EDX = EAX                                                 ; sin(-yaw)

body-frame projection (yields sVar2):
    ECX = actor->linear_velocity_z   ; +0x1CC (NOTE: name says z but offset matches +0x1CC)
    EAX = actor->linear_velocity_x   ; +0x1D4 (NOTE: layout: +0x1CC..0x1D4 contains lin_vel as x,?,z)
    SAR ECX, 8                       ; vz >> 8 (plain SAR)
    SAR EAX, 8                       ; vx >> 8
    MOVSX ECX, CX                    ; int16-truncate vz>>8
    MOVSX EAX, AX                    ; int16-truncate vx>>8
    IMUL ECX, EDX                    ; (int16)(vz>>8) * sin(-yaw)
    IMUL EAX, EBX                    ; (int16)(vx>>8) * cos(-yaw)
    SUB EAX, ECX                     ; proj = (vx>>8)*cos(-yaw) - (vz>>8)*sin(-yaw)
                                     ;       = (vx>>8)*cos(yaw) + (vz>>8)*sin(yaw)
                                     ;       (because sin(-y) = -sin(y))
                                     ; This is the BODY-FRAME LONGITUDINAL component.

    CX = (uint16_t) actor->display_angle_roll      ; +0x208
    CDQ
    AND EDX, 0xfff                   ; sign_mask & 0xfff
    ADD EAX, EDX
    SAR EAX, 12                      ; sVar2 = ROUND-TO-ZERO( proj / 4096 ), int16-truncated later
    SUB ECX, 0x800
    AND ECX, 0xfff
    SUB ECX, 0x800                   ; roll12 = (((uint16)disp_roll - 0x800) & 0xfff) - 0x800
                                     ; folds to signed [-0x800, 0x7FF]

gate (APPLY when |sVar2| >= 0x21 AND |roll12| < 0x80 with sign-compatibility):
    CMP AX, 0x20                     ; sVar2 (int16 truncated) vs 0x20
    JLE  e33                         ; sVar2 <= 0x20 ?
    ; sVar2 > 0x20 (positive, large)
    CMP ECX, 0x80
    JGE  e58 (SKIP)                  ; skip if roll12 >= 0x80
    JMP  e3e (APPLY)
e33:
    CMP AX, 0xffe0                   ; sVar2 vs -0x20 (signed)
    JGE  e58 (SKIP)                  ; if sVar2 >= -0x20, sVar2 in [-0x20,0x20] → SKIP
    ; sVar2 < -0x20 (negative, large)
    CMP ECX, -0x80
    JLE  e58 (SKIP)                  ; skip if roll12 <= -0x80
    ; fall to APPLY

apply (e3e..e52):
    ECX = actor->angular_velocity_roll              ; +0x1C0
    EAX = (int16_t)sVar2 (sign-extend AX)
    CDQ
    AND EDX, 0x3
    ADD EAX, EDX
    SAR EAX, 2                        ; sVar2 >> 2 with RZ rounding
    ECX += EAX
    actor->angular_velocity_roll = ECX

decay (e58..e8a):
    av_roll  -= sar_rz(av_roll,  4)   ; av_roll  -= (av_roll  + (sign?0xF:0)) >> 4
    av_pitch -= sar_rz(av_pitch, 4)   ; same idiom, +0x1C8

tire-slip accumulators:
    EDX = actor->lateral_speed        ; +0x318
    EAX = actor->longitudinal_speed   ; +0x314
    SAR EDX, 8                        ; plain SAR (low 16 bits used)
    word[+0x340] += DX                ; accumulated_tire_slip_x += (int16)(lat>>8)
    SAR EAX, 8
    word[+0x342] += AX                ; accumulated_tire_slip_z += (int16)(long>>8)
    ; av_pitch writeback
return
```

## Confirmed divergences (port vs original)

### D1 — Gate POLARITY INVERTED **(HIGH IMPACT, CRITICAL)**
Port (`td5_physics.c:5877`):
```c
if (abs_v < 0x21 && abs_r < 0x7F) {
    actor->angular_velocity_roll += v_long >> 2;
}
```
**Apply when |v_long| < 0x21 AND |roll| < 0x7F (LOW speed + small roll).**

Original (listing 0x00403E23..0x00403E50):
**Apply when |sVar2| >= 0x21 (HIGH speed) AND sign-compatible roll12 in [-0x7F, 0x7F].**

The port has the speed threshold inverted. The original is a "barrel-roll recovery" — at high body-frame longitudinal speed with shallow roll, nudge roll angular velocity by speed/4. The port instead nudges only when the car is nearly stopped, which is exactly when this recovery is least useful.

**Symptom:** state-0f damping fails to recover heavy roll under any actual motion. The car keeps spinning. This matches the open TODO `todo_state0f_overfire_skips_player.md` "AI dynamics force overshoot".

### D2 — Gate uses WRONG SIGN-COMPATIBILITY **(HIGH IMPACT)**
Even when speed gate is fixed, the port uses `abs_r < 0x7F` (symmetric). Original gates roll per sign of sVar2:
- if sVar2 > 0x20: skip if `roll12 >= 0x80`  (only nudge when roll is on the "low" side)
- if sVar2 < -0x20: skip if `roll12 <= -0x80` (only nudge when roll is on the "high" side)

The asymmetric gate prevents the correction from making things worse when the car is already rolled far past upright in the same direction as the lateral drift.

### D3 — Wrong source for v_long projection **(MEDIUM IMPACT)**
Port computes:
```c
int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;     // +0x1F4
int32_t cos_h = cos_fixed12(heading);
int32_t sin_h = sin_fixed12(heading);
int32_t v_long = (actor->linear_velocity_x * sin_h +
                  actor->linear_velocity_z * cos_h) >> 12;
```

Original uses `display_angle_yaw` (+0x20A int16) directly:
- `MOVSX EDI, word[+0x20a]`
- `NEG EDI`
- `cos_h = CosFixed12bit(-yaw)`, `sin_h = SinFixed12bit(-yaw)`

The two should match because `display_angle_yaw = (int16_t)(euler_accum.yaw >> 8)` is written each tick by the integration step. But there is a one-tick lag risk: if `display_angle_yaw` is written AFTER state0f damping runs in the caller chain, the port reads a fresher value than the original.

Verify in trace: capture both `actor->euler_accum.yaw` and `actor->display_angle_yaw` at function entry; compare derived heading12 against original.

### D4 — Plain SAR instead of round-to-zero **(LOW IMPACT but pervasive)**
Original uses the `(x + (x>>31 & MASK)) >> SHIFT` round-to-zero idiom in three places:
- `proj >> 12` (mask = 0xFFF)
- `sVar2 >> 2` (mask = 0x3) in apply
- `av_roll  >> 4` (mask = 0xF) in decay
- `av_pitch >> 4` (mask = 0xF) in decay

Port uses plain arithmetic shift `>> N` for all four. For negative non-divisible values this yields a result 1 unit further from zero than the original.

Same idiom as pool5 D1 (`pilot_00403A20_audit.md`) — the C analogue is:
```c
static inline int32_t sar_rz(int32_t x, int n) {
    int32_t mask = (1 << n) - 1;
    return (x + (((x) >> 31) & mask)) >> n;
}
```

### D5 — int16 truncation before multiply **(LOW IMPACT)**
Original applies `MOVSX CX, vz>>8` and `MOVSX AX, vx>>8` BEFORE the IMUL with cos/sin. Port multiplies full int32 `vx * sin_h` then shifts at end.

For Edinburgh typical velocities (`vx <= 6000 fp8`, `vx>>8 <= 23`), int16 truncation is a no-op. Becomes a wash only at extreme speeds (vx > ~32768*256 ~ 8.4M fp8, never reached).

### D6 — Tire-slip accumulator source mismatch **(MEDIUM IMPACT)**
Port:
```c
actor->accumulated_tire_slip_x += (int16_t)(v_lat >> 8);
actor->accumulated_tire_slip_z += (int16_t)(v_long >> 8);
```

Original:
```c
actor->accumulated_tire_slip_x += (int16_t)(actor->lateral_speed       >> 8);  // +0x318
actor->accumulated_tire_slip_z += (int16_t)(actor->longitudinal_speed  >> 8);  // +0x314
```

The original reads `lateral_speed`/`longitudinal_speed` already stored at +0x318/+0x314 (presumably from the prior `UpdateAIVehicleDynamics`/`UpdatePlayerVehicleDynamics` body-frame projection). The port computes a fresh v_lat/v_long from heading + world velocity, which:
- requires the heading to be in sync (D3)
- uses a different rotation order than whatever wrote +0x314/+0x318

If +0x314/+0x318 were written in a prior physics step using a slightly different formula or rounding, this is a guaranteed divergence per tick.

### D7 — Roll-folding source **(LOW IMPACT)**
Port:
```c
int32_t roll = (actor->euler_accum.roll >> 8) & 0xFFF;
if (roll > 0x800) roll -= 0x1000;
```

Original:
```c
roll12 = (((uint16_t)display_angle_roll - 0x800) & 0xfff) - 0x800;  // range [-0x800, 0x7FF]
```

Boundary: port keeps 0x800 → 0x800; original folds 0x800 → -0x800. For the gate check (`< 0x80` and `< -0x80`), the boundary case 0x800 vs -0x800 lies far outside either threshold, so the practical comparison outcome is identical.

Source mismatch (euler_accum vs display_angles) same as D3.

## Sign-extension correctness check

The original truncates `sVar2` to 16 bits before the gate compares:
```
CMP AX, 0x20          ; compares low 16 bits (signed via JLE)
CMP AX, 0xffe0        ; compares low 16 bits vs -0x20 (signed)
```

For `proj/4096` values that exceed int16 range, truncation wraps. With reasonable velocities and an int32 proj on the order of ±60M (vx*cos ~ 6000*4000 = 24M; combined ~50M), sVar2 = proj/4096 ~ ±12000, comfortably inside int16. So 16-bit truncation behaves as identity here.

When applied: the apply uses `MOVSX EAX, AX` (sign-extend back to 32). So sVar2 effectively goes through int16 round-trip; for in-range values this is identity. Port's `v_long` stays int32 throughout. For Edinburgh data the comparison results match modulo D4 RZ rounding.

## Fixes to apply (priority order)

1. **D1 (CRITICAL) — invert gate polarity**: change `abs_v < 0x21` to `abs_v >= 0x21` (or rather, rewrite to match the original's `> 0x20` / `< -0x20` branched form).
2. **D2 — asymmetric roll gate**: use sign-compatible roll bounds.
3. **D6 — read +0x314/+0x318 directly** for tire-slip accum, do NOT recompute from heading.
4. **D3 — switch yaw source** to `actor->display_angles.yaw` (NEG, then call cos/sin).
5. **D4 — round-to-zero** at all 4 SAR sites.
6. **D7 — switch roll source** to `display_angles.roll`, use `((uint16 - 0x800) & 0xfff) - 0x800` form.
7. **D5 — int16 truncate vx>>8, vz>>8** before IMUL with sin/cos.

## Capture schema for runtime probe

Per call (1 row per call — function is called at most once per actor per tick):

**Keys:** `sim_tick`, `slot`, `addr=0x403D90`, `phase` (enter/leave)
**Inputs (entry):**
- `display_angle_yaw` (+0x20A)
- `display_angle_roll` (+0x208)
- `euler_accum_yaw` (+0x1F4)   — for D3 diff
- `euler_accum_roll` (+0x1F0)  — for D7 diff
- `linear_velocity_x` (+0x1CC)
- `linear_velocity_z` (+0x1D4)
- `lateral_speed` (+0x318)
- `longitudinal_speed` (+0x314)
- `angular_velocity_roll` (+0x1C0)
- `angular_velocity_pitch` (+0x1C8)
- `accumulated_tire_slip_x` (+0x340)
- `accumulated_tire_slip_z` (+0x342)
- `surface_contact_flags` (+0x376)
- `front_axle_slip_excess` (+0x31C)
- `rear_axle_slip_excess` (+0x320)

**Outputs (leave):** same set — to capture all field mutations.

**Derived (leave):** computed `sVar2_proj` (after >>12), `roll12_folded`, `gate_decision` (skip/apply).

## Capture difficulty

State 0x0F only fires when the actor state byte (+0x0FB or similar) is forced to 0x0F by `UpdatePlayerVehicleControlState (0x00402E60)` on input bit `0x200000` — which per memory `todo_state0f_overfire_skips_player.md` is essentially the chassis-launch / airborne event. Edinburgh (track=1, span 0) historically does NOT trigger this often without a launch. Honolulu (track=11) has known rollover scenarios per `todo_chassis_launch_track_walker.md`.

If Edinburgh capture yields zero state-0f frames, switch to Honolulu. The pilot's existing harness must allow either track via INI.

## Reference

- Listing: 0x00403D90..0x00403EAD (Ghidra TD5_pool6, 2026-05-14)
- Decompilation: same session, 45 lines
- Port: `td5mod/src/td5re/td5_physics.c:5843-5892`
- Caller: `UpdateVehicleActor (0x00406650)` — single call site
- Related TODO: `todo_state0f_overfire_skips_player.md`
- Sister pilot (SAR_RZ idiom): `re/analysis/pilot_00403A20_audit.md`
