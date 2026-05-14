# Pilot Audit — 0x00404030 UpdatePlayerVehicleDynamics

**Date:** 2026-05-14
**Pool slot:** TD5_pool0
**Port-side function:** `td5_physics_update_player` @ `td5_physics.c:829`
**Worktree:** `.claude/worktrees/precise-00404030` on branch `precise-00404030`
**Caller graph:** `UpdateVehicleActor (0x00406650)` — single caller, per-tick.
**Callees:** `GetTrackSegmentSurfaceType (0x42F100)`, `ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)`, `CosFixed12bit (0x40A6E0)`, `SinFixed12bit (0x40A700)`, `ComputeReverseGearTorque (0x403C80)`, `ComputeDriveTorqueFromGearCurve (0x42F030)`, `UpdateAutomaticGearSelection (0x42EF10)`, `ApplyReverseGearThrottleSign (0x42F010)`, `UpdateEngineSpeedAccumulator (0x42EDF0)`, `IntegrateWheelSuspensionTravel (0x403A20)`, `ApplyMissingWheelVelocityCorrection (0x403EB0)`.
**Body:** `0x00404030..0x00404eb7` — 3719 bytes / ~870 instructions / 470+ decompiled lines.

## Function structure (from listing + decomp)

The port already has extensive inline comments anchoring each block to original
instruction addresses (see `td5_physics.c:829-1778`). This audit focuses on
divergences observed against the listing.

Block layout in the original:

```
0x00404030  prologue + 5x GetTrackSegmentSurfaceType (chassis @+0x80, 4 wheels @+0x00..0x30)
0x0040408E  actor->surface_type_chassis = chassis_surface
0x00404094  ComputeVehicleSurfaceNormalAndGravity(actor)
0x0040409C  drag selection: if (steering_cmd < 0x20 || gear <= 1) damp_low(0x6c) else damp_high(0x6a)
0x004040DF  vx_drag = vx - sar8_rz(sar8_rz(vx) * damp_coeff) … plain SAR12_RZ at end too
              ACTUAL: vx -= sar12_rz(sar8_rz(vx) * damp_coeff)
0x0040410D  vz_drag = same
0x00404139  load actor.euler_accum @+0x1C4 -> local_4c (cache)
0x00404142  uVar12 = sar8_rz(actor->steering_command @+0x30C)
0x0040415B  steer = uVar12
0x00404189  yaw_heading = sar8_rz(actor->angular_velocity_yaw @+0x1F4)  *** WAIT: SAR not SAR_RZ
0x00404196  iVar14 = CosFixed12bit(yaw_heading)
0x0040419F  iVar15 = SinFixed12bit(yaw_heading)
0x004041AD  iVar16 = CosFixed12bit(yaw_heading + steer)
0x004041B6  iVar17 = SinFixed12bit(yaw_heading + steer)
0x004041C2  iVar18 = CosFixed12bit(steer)
0x004041CD  iVar19 = SinFixed12bit(steer)
0x004041E6..0x0040421F: body-frame rotation
    iVar20 (lat) = sar12_rz(cos_h*vx - sin_h*vz)     -> [EBP-0x24]  
    uVar12 (long) = sar12_rz(cos_h*vz + sin_h*vx)    -> [EBP-0x34]   *** reused as longitudinal
    note: 0x004040E0..0x00404104 already wrote drag-modified vx/vz to actor
0x00404222..0x0040433E  per-wheel grip from tables + load transfer:
    full_wb = 2*half_wb       [EBP-0x1c] = phys[0x24]*2
    front_load_factor_F = sar8_rz(phys[0x2A]<<8 / total_weight * (half_wb - center_susp_pos) / full_wb * surface_grip_table[wheel_surface])  // FL
    front_load_factor_R = same but with wheel_surface[1] (FR)
    rear_load_factor_L  = sar8_rz(phys[0x28]<<8 / total_weight * (half_wb + center_susp_pos) / full_wb * surface_grip_table[wheel_surface[2]])
    rear_load_factor_R  = same with surface[3]
    each clamped [0x38, 0x50]
0x0040433E..0x00404372  handbrake gate (actor+0x36E): rear grips *= phys[0x7a]/256
0x00404375..0x004043B6  uVar37 calc:
    chassis_long  = sar12_rz(sin_h*vx + cos_h*vz)
    yaw_term      = (sin(s) * phys[0x28] * angular_velocity_yaw / 0x1000) / 0x28C
    uVar37 (front-axle-frame Vlong w/ yaw correction) = chassis_long - yaw_term
                                                                              ^ stored at [EBP-0x20]
0x004043B8  surface_contact_flags = actor+0x376 check
0x004043C8  if scf!=0 -> drivetrain dispatch (sVar2 == 1/2/3): write actor+0x314 / +0x318
            ON-GROUND branch:
            sVar2=1: longitudinal_speed = CRGT(uVar12);  lateral_speed = uVar37
            sVar2=2: lateral_speed      = CRGT(uVar37); longitudinal_speed = uVar12
            sVar2=3: lateral_speed      = CRGT((uVar37+uVar12)/2); longitudinal_speed = (same CRGT)
0x00404437  brake_flag check (actor+0x36D)
            if (brake_flag != 0): brake-clamp path (uses phys[0x6E], builds nested signed clamp)
            else: drive path:
                sVar2=1: F_long = CDTFGC/2 to pRVar29; pRVar30=0; local_1c=0
                sVar2=2: front-front split: pRVar30 = CDTFGC/2 to both; pRVar29=0
                sVar2=3: CDTFGC/4 to all four (with rz rounding: (x+(x>>0x1f&3))>>2)
0x004044F9  AIRBORNE branch  (actor+0x376 == 0):
            longitudinal_speed = uVar12     (raw, no CRGT)
            lateral_speed      = uVar37     (raw)
            if (brake_flag): drop into brake path
            else if (pRVar29 -- throttle != 0):
                actor+0x378 manual/auto select
                update engine speed
                drivetrain dispatch (same RWD/FWD/AWD)
                speed limit clamp (actor+0x314 > phys[0x74]<<8 -> all zero)
            else: coast (-32 throttle constant)
0x004045E3  airborne+brake path: ComputeBrakeForces — engine update + symmetric front/rear brake
            with handbrake-specific clamps (actor+0x36E)
            handbrake on: rear = -(uVar12); -(rear/2) ; clamp via 2-way min
            handbrake off: rear = -(uVar37/2) / -(brake/2) ; clamp via 2-way min
0x004046DC  POST-FORCE/PRE-SOLVE: per-wheel surface grip table re-read:
            iVar36 = sar8_rz(grip_table[wheel_surf[1]] * pRVar30)   (FR rear value??)
            iVar11 = sar8_rz(grip_table[wheel_surf[2]] * actor)
            iVar35 = sar8_rz(grip_table[wheel_surf[0]] * local_1c)
            iVar21 (NOT rounded) = grip_table[wheel_surf[3]] * local_8
            local_8 = iVar36 + iVar35   (rear + front summed)
            local_2c = sar8_rz(iVar21) + iVar11
0x00404713..0x004047E5  coupled lateral force solve (local_c = front_lat, local_14 = rear_lat)
            denom = sar10_rz(L*L)*cos²(s)/4096 + sar10_rz(b²+I)*sin²(s)/4096
            rear_lat numerator = D*cos²(s) + [A + B + C]*sin(s)
            front_lat numerator = E*sin(s) - F*cos(s)
0x004047E5..0x0040488D  yaw torque writeback:
            iVar28 = (cos(s)*Wf>>12 * front_lat + (FR_w - RL_w - FL_w + RR_w)*500 - rear_lat*Wr) / (I/0x28C)
            CLAMP yaw_torque to [-0x578, +0x578]
0x004048AC..0x004048ED  apply forces back to world frame:
            actor->linear_velocity_x += sar12_rz(rear_long*sin_h) + sar12_rz(front_long*sin_s)
                                      + sar12_rz(rear_lat*cos_h) + sar12_rz(front_lat*cos_s)
            actor->linear_velocity_z += sar12_rz(rear_long*cos_h) + sar12_rz(front_long*cos_s)
                                      - sar12_rz(rear_lat*sin_h) - sar12_rz(front_lat*sin_s)
0x00404912  actor->accumulated_tire_slip_x += (lateral_speed >> 8)        ; +0x340 short
0x0040491A  actor->accumulated_tire_slip_z += (longitudinal_speed >> 8)   ; +0x342 short  (handbrake-off only)
0x00404933..0x0040493D  if (yaw_torque > 0):
              yaw_torque = sar6_rz(yaw_torque) * yaw_torque                ; ? actually sar15(prod)
              clamp to [-0x200, +0x200], subtract from angular_velocity_yaw
0x0040495F  if (current_gear==2 && wheelspin && encounter_steer > 0x7F): scf = phys[0x76]
0x00404995  zero-velocity-deadzone: if |vx|<0x40 && |vz|<0x40 && |yaw|<0x20 -> zero all 3
0x00404EA2  IntegrateWheelSuspensionTravel(actor, phys, fx, fz)
0x00404EAB  ApplyMissingWheelVelocityCorrection(actor)
```

## Confirmed divergences from port (pre-fix at session start)

### D1 — Plain SAR vs round-to-zero idiom **(SCALE: 40+ sites)**

The original uses the `((x + (x>>31 & mask)) >> N)` rounding-toward-zero idiom
(SAR_RZ) throughout, encoded in x86 as `CDQ; AND EDX,mask; ADD EAX,EDX; SAR EAX,N`.
For positive values, identical to `x >> N`. For negative values, the result is
one LSB closer to zero than plain SAR.

**Audit of port (`td5_physics_update_player`)**:

Sites already using exact idiom (`(x + (x>>31 & 0xFF)) >> 8`):
- Line 1745 (wheelspin steering shift) — manual idiom present.

Sites using PLAIN `>> N` where original uses SAR_RZ:
- **Line 916-917 (drag)**: `((actor->linear_velocity_x >> 8) * damp_coeff) >> 12`
  Original is `sar12_rz(sar8_rz(vx) * damp_coeff)`. Two divergence points.
- **Line 929-931 (body velocity)**: `(vx * sin_h + vz * cos_h) >> 12` plain SAR.
- **Line 921 (heading)**: `(actor->euler_accum.yaw >> 8) & 0xFFF` — original uses SAR_RZ at 0x00404189: actually `SAR EDI,0x8` — PLAIN SAR (no CDQ idiom). Port is correct here.
- **Line 1067 (steer_angle)**: `-(actor->steering_command >> 8)` — original at 0x0040414B-415B uses SAR_RZ. Port plain. **DIVERGENT.**
- **Line 1078-1081 (yaw_corr)**: shifts on signed intermediates lack RZ.
- **Line 1276-1322 (coupled bicycle solve)**: many `/ 1024`, `/ 4096` — **using signed integer division `/`** which is correct (rounds toward zero in C99+), so this is equivalent to SAR_RZ. NOT divergent — port comments explicitly note this.
- **Line 1454-1464 (current_slip_metric calc)**: `delta >> 8` plain — original 0x004049BA uses SAR_RZ.
- **Line 1556-1563 (player_fx/fz writeback)**: `>> 12` on int64_t arithmetic — needs verification against the 32-bit IMUL+SAR_RZ flow.
- **Line 1613-1619 (body_speeds writeback)**: same chassis_long + yaw_term pattern as in 0x00404375. The yaw_term divider `/ 0x28c` uses C signed division (RZ). The `>> 12` on sin*vx products is plain.

### D2 — Drag IMUL ordering (HIGH IMPACT)

Original 0x004040E5-00404102:
```
EAX = vx
sar8_rz(EAX)  ; EAX = vx_h8
IMUL EAX, ECX ; EAX = vx_h8 * damp_coeff (32-bit signed wrap, may overflow!)
sar12_rz(EAX) ; round-to-zero by 4096
SUB EDI, EAX  ; vx -= drag
```

Port line 916: `((actor->linear_velocity_x >> 8) * damp_coeff) >> 12`

Port uses plain SAR for both shifts. Original uses SAR_RZ for both.

For typical Edinburgh-grip damp_coeff (~7-15K) and vx (~1M-10M), the
intermediate exceeds 2^31 — port's 32-bit `int32_t` multiply WRAPS the same way
as the original IMUL (no `int64_t` lift), so the wrap behavior is faithful.
Just the rounding is wrong.

### D3 — uVar37 yaw correction divisor (LIKELY MATCH)

Original 0x004043A7-004043B7: `idiv` by 0x28C followed by post-adjust with `SHR ... ADD` —
this is the classic "magic number" reciprocal-multiply for `/ 0x28C` (because
0xC907DA5 ≈ 2^40 / 0x28C). The result IS signed integer division rounding
toward zero.

Port lines 1404 / 1464 / 1618: `yaw_term = (sin_sr * phys[0x28] * actor->ang_vel_yaw) >> 12 / 0x28C`.
This is C signed `/` which already rounds toward zero. Match.

### D4 — D[5C/58/10/3C] grip[] are FOUR independent values

Original 0x00404265-0x00404279 reads grip table FOUR times with separate
wheel-surface bytes (bVar7, bVar8, bVar9, bVar10). These become:
- local_5c (FL grip) at [EBP-0x58]
- local_58 (FR grip) at [EBP-0x54]
- local_10 (RL grip) at [EBP-0xc]
- local_3c (RR grip) at [EBP-0x38]

Each one is per-wheel because `bVar7/8/9/10` come from 4 INDIVIDUAL
`GetTrackSegmentSurfaceType` calls on actor+0x00..0x30 (4 body probes, not 4
wheel probes per se).

**Port lines 841-842**: hard-codes `surface_wheel[i] = surface_center` (all 4 same).

This means: on a mixed-surface segment (e.g., two wheels on grass), original
computes 4 distinct grip values that diverge by ~3x between asphalt/grass.
Port flattens them to one chassis value. Yaw term2 of the yaw torque (line
1516, `(wheel_long_rr - wheel_long_rl - wheel_long_fl + wheel_long_fr) * 500`)
becomes zero because all four `sf_*` are the same.

**Likely symptom:** in straight-line driving on uniform surface, no divergence
(asphalt-only spans). On mixed surfaces or surface transitions, port loses the
yaw torque cross term.

**This is the same kind of architectural divergence as 00403720's D1 missing
second loop.** It is a port-time scope cut; needs per-wheel `GetTrackSegmentSurfaceType`
to fix faithfully. NOT fixable in this pilot's scope alone — depends on
having all 4 body probes (actor+0x00..0x3F) populated with current span/lane.

### D5 — Load transfer in-iteration vs out-of-iteration

Port (line 866-869) computes a SINGLE `front_load` and `rear_load` value
(scalar), then per-wheel loops `(sf * load + 128) >> 8`. The `+128` rounding is
**plain rounding to nearest**, not SAR_RZ.

Original 0x00404228-0x0040427F computes the load transfer ratio per-axle then
multiplies by the per-wheel surface grip-table, all in one path:
```
ratio_front = (phys[0x2A] << 8) / total_weight             ; iVar22
ratio_front *= (half_wb - center_suspension_pos) / full_wb ; iVar22
local_5c = sar8_rz(grip_table[bVar7] * ratio_front)        ; FL
local_58 = sar8_rz(grip_table[bVar8] * ratio_front)        ; FR
ratio_rear = (phys[0x28] << 8) / total_weight              ; iVar22
ratio_rear *= (half_wb + center_susp_pos) / full_wb
local_10 = sar8_rz(grip_table[bVar9] * ratio_rear)         ; RL
local_3c = sar8_rz(grip_table[bVar10] * ratio_rear)        ; RR
```

The port's `(sf * load + 128) >> 8` is NOT the same as `sar8_rz(sf * load)`:
- `+128 >> 8` rounds to nearest (banker's), adds 0.5 LSB.
- `sar8_rz` rounds toward zero, adds 0 to positive / 0xFF/256 → 1 LSB for negative.

For positive values, port adds 0.5 LSB while original adds 0. **Off by 0 or 1 LSB.**
Visible in the diff if grip is near a clamp boundary.

### D6 — Brake-clamp signed nested branch (LIKELY MATCH after sign analysis)

Original 0x00404441-0x00404481:
```
EAX = sar8_rz(phys[0x6E] * throttle)     ; bf
EDX = -bf
ECX = -uVar12 (vsa = body forward speed)
if (EDX < ECX) EBX = ECX else EBX = EDX  ; min(-bf, -vsa) = -max(bf, vsa)
if (bf > EBX): goto skip
  EAX = -bf
  if (EDX < ECX) EAX = ECX               ; ECX = -vsa
EAX = -bf  ; *negate AGAIN -- weird flow
ECX = EAX >> 1
[EBP-0x18] = ECX, [EBP-0x4] = 0, [EBP+0x8] = 0
```

Wait — the listing shows at 0x0040446F-0x00404474:
```
CDQ            (sign extend)
SUB EAX, EDX   (round-to-zero adjust for signed div-by-2)
MOV ECX, EAX
SAR ECX, 1     (signed /2)
XOR EAX, EAX
MOV [EBP-0x18], ECX  ; rear-wheel-LEFT  drive
MOV [EBP-0x4],  EAX  ; FR drive = 0
MOV [EBP+0x8], EAX  ; FL drive = 0
```

This is **`(bf - sign(bf)) >> 1`** — i.e. signed `bf / 2` with round-toward-zero.
Port (line 1097-1098): `bf >> 1` plain SAR. Off by 1 LSB for negative bf.

Note: the original assigns ONLY `[EBP-0x18]` (`pRVar30` = rear axle), leaves `local_1c`
and `local_8` zero. Port writes BOTH `wheel_drive[2]` and `wheel_drive[3]`. Original
writes wheel_drive[2] only (RL); wheel_drive[3] is left from prior zero-init.

Wait — `[EBP-0x18]` is `local_18` per the decomp variable. Looking back at the
decomp at line 88-94 in the saved decomp:

```c
local_8 = (RuntimeSlotActor *)0x0;
actor = (RuntimeSlotActor *)0x0;
pRVar30 = (RuntimeSlotActor *)(iVar36 / 2);
pRVar29 = actor;
local_1c = (RuntimeSlotActor *)(iVar36 / 2);
```

So in decomp form pRVar30 = bf/2 and local_1c = bf/2. These are pRVar30 = rear-axle, local_1c = some other slot.

The actual writeback at 0x004046DC reads:
- `local_5c` (FL grip) * `local_1c` -> iVar35 (FL contribution to local_8 sum)
- `local_58` (FR grip) * `pRVar30`  -> iVar36 (FR contribution to local_8 sum)
- `local_10` (RL grip) * `actor`    -> iVar11 (RL contribution to local_2c sum)
- `local_3c` (RR grip) * `local_8`  -> iVar21 (RR contribution to local_2c sum)

So:
- pRVar30 = FR wheel drive
- local_1c = FL wheel drive
- actor = RL wheel drive
- local_8 = RR wheel drive

In brake mode the original sets pRVar30 = bf/2 (FR), local_1c = bf/2 (FL) — i.e. FRONT
brakes. local_8 = 0, actor = 0 (no rear brake).

Port (line 1097-1098): `wheel_drive[2] = bf>>1; wheel_drive[3] = bf>>1` — brake REAR.

**DIVERGENCE D6:** Port brakes REAR axle; original brakes FRONT axle.

Wait — re-reading: at 0x00404478 the brake writeback is:
```
MOV [EBP-0x18], ECX  ; bf/2 -> local_18 (RR slot per the convention)
MOV [EBP-0x4],  EAX  ; 0 -> local_4 (??)
MOV [EBP+0x8], EAX   ; 0 -> [EBP+8] = throttle param slot reused
```

`local_18` = [EBP-0x18], `local_4` = [EBP-0x4], `actor` is parameter at [EBP+0x8].

The variable mapping appears scrambled by the decomp. Need explicit listing
trace. **HIGH PRIORITY:** verify which slot gets bf/2 in the brake path.

### D7 — Speed limit clamp condition direction

Original 0x004045B3-0x004045D6 (airborne branch only):
```
EDX = movsx phys[0x74]   ; speed limit short
EAX = actor->long_speed (+0x314)
SHL EDX, 8
CMP EAX, EDX
JLE skip
; if long_speed > speed_limit -> zero all wheel drives
```

But port line 1003 `if (abs_speed <= speed_limit)` is an **abs**-comparison and
runs only on the on-ground path. Original on-ground path also lacks a speed cap;
the cap is airborne-only.

**This is correct in the port already for the airborne path (line 1149),
incorrect for the on-ground path (line 1003)** — port adds a speed cap on-ground that
the original lacks. Actually no — the port at line 1003 is the abs-clamp BEFORE
the dt_type dispatch, not the post-dispatch zero-clamp. The on-ground original
indeed has no equivalent. Verify with Frida.

### D8 — Surface tables: gSurfaceDragCoefficientTable vs gSurfaceGripCoefficientTable

Original uses TWO distinct tables:
- `gSurfaceDragCoefficientTable @ DAT_00474900` — used in drag calc only
- `gSurfaceGripCoefficientTable @ DAT_004748C0` — used everywhere else (grip, force scaling, yaw term2)

Port (line 909): `s_surface_grip[...]` is used in the drag formula (Step 5),
and `s_surface_friction[...]` is used as grip elsewhere. Need to verify both
arrays' contents match the two distinct DAT tables. If port has only ONE table
populated and aliased, drag and grip diverge.

**Search confirmation:** check whether `s_surface_grip` and `s_surface_friction`
are loaded from `DAT_00474900` vs `DAT_004748C0` respectively, or both from one.

### D9 — Yaw term2 `(wheel_RR - wheel_RL - wheel_FL + wheel_FR) * 500`

This is the left-right longitudinal force differential at the rear/front
contact points. With per-wheel grip (D4 unfixed), term2 is always 0.

When D4 IS fixed, term2 contributes during cornering on mixed-surface roads —
verifying this is a Frida-after-D4 task.

### D10 — Coupled bicycle solve `front_long` / `rear_long` arithmetic

The port at line 1239-1240 computes:
```c
front_long = ((sf_fl * wheel_drive[0]) >> 8) + ((sf_fr * wheel_drive[1]) >> 8);
rear_long  = ((sf_rl * wheel_drive[2]) >> 8) + ((sf_rr * wheel_drive[3]) >> 8);
```

Original 0x004046DC-0x00404710 actually does:
```
iVar36 = sar8_rz(grip_table[bVar8] * pRVar30)       ; FR contribution
iVar11 = sar8_rz(grip_table[bVar9] * actor)         ; RL contribution
iVar21 = grip_table[bVar10] * local_8               ; RR raw (no shift yet)
iVar35 = sar8_rz(grip_table[bVar7] * local_1c)      ; FL contribution
local_8 = iVar36 + iVar35                           ; FRONT pair sum
iVar31 = sar8_rz(iVar21)                            ; RR contribution rounded
local_2c = iVar31 + iVar11                          ; REAR pair sum
```

So FL+FR -> "local_8" (which then gets used as a single per-front input to the
solve) and RL+RR -> "local_2c" (single per-rear input). Port matches this
structure. **The plain `>> 8` vs `sar8_rz` is the only LSB-divergence here.**

### D11 — Coupled solve `/ 1024` round semantics — already C signed (no fix needed)

The port comments at line 1255-1256 already note that `/` in C signed integer
rounds toward zero exactly matching the original. **NO ACTION.**

### D12 — Tire slip accumulator type widths

Original 0x00404912 `actor->accumulated_tire_slip_x += sar8(lateral_speed)`
where the actor field is a 16-bit short at +0x340. Port (line 1768) matches.

### D13 — surface_contact_flags written here, never AI

Port enforces this gate at line 1752 already, matching original.

### D14 — yaw damping correction at end

Original 0x00404933:
```
if (front_axle_slip_excess > 0) {
    correction = sar6_rz(yaw_torque) * front_axle_slip_excess  ; sar6_rz with +0x3F mask
    correction = sar15_rz(correction)                          ; sar15_rz with +0x7FFF mask
    clamp [-0x200, +0x200]
    angular_velocity_yaw -= correction
}
```

Port (line 1691-1696):
```c
correction = (actor->angular_velocity_yaw >> 6) * actor->front_axle_slip_excess;
correction = correction >> 15;
clamp [-0x200, +0x200]
```

**Both shifts plain.** Off by 1 LSB on negative angular_velocity_yaw.

### D15 — `current_slip_metric` field (+0x33C) write semantics

Port at line 1471-1472 writes the slip_mag computed in either branch — last
write wins. Original 0x004049BA / 0x00404A80 does the same (rear overwrites
front). Match.

## Summary table of impact

| ID | Divergence | LSB / structural | Impact |
|----|------------|------------------|--------|
| D1 | Plain SAR vs SAR_RZ at 5+ sites | 0/1 LSB per site | LOW per-tick, accumulates over hundreds of ticks |
| D2 | Drag rounding | 0/1 LSB | LOW |
| D3 | yaw_term division | MATCH | none |
| D4 | All 4 wheels same surface | structural | MEDIUM (mixed surface only) |
| D5 | `+128 >> 8` vs `sar8_rz` for grip | 0/1 LSB | LOW |
| D6 | Brake REAR vs FRONT axle assignment | structural | **HIGH if confirmed** |
| D7 | Speed limit clamp scope | structural | LOW |
| D8 | Drag vs Grip surface tables | possibly aliased | depends on data |
| D9 | Yaw term2 nonzero only with per-wheel grip | downstream of D4 | downstream |
| D10 | front_long/rear_long plain SAR | 0/1 LSB | LOW |
| D11 | Coupled solve / | MATCH | none |
| D14 | Yaw damping plain SAR | 0/1 LSB | LOW |
| D15 | slip_metric overwrite | MATCH | none |

**HIGHEST PRIORITY for fix:** D6 (verify and possibly correct), D5 (single
LSB), D1 (LSB sites), D4 (architectural — needs upstream per-wheel probes).

## Capture schema for pilot

Per call (one row per tick on slot 0):

**Keys:** `sim_tick`, `slot`, `caller_ra`

**Inputs (entry):**
- `actor_addr`, `cardef_ptr`
- `track_span_raw` (+0x80), `sub_lane_index` (+0x8C)
- `surface_chassis_in` (+0x370) — entry value before overwrite
- `lin_vx_in` (+0x1CC), `lin_vz_in` (+0x1D4), `ang_yaw_in` (+0x1F4)
- `euler_yaw_in` (+0x1C4)
- `steer_cmd_in` (+0x30C, int32)
- `encounter_steer_in` (+0x33E, int16)
- `current_gear_in` (+0x36B, u8), `brake_flag_in` (+0x36D, u8), `handbrake_flag_in` (+0x36E, u8)
- `surface_contact_flags_in` (+0x376, u8)
- `center_suspension_pos_in` (+0x2CC, i32)
- `long_speed_in` (+0x314), `lat_speed_in` (+0x318)
- `engine_speed_accum_in` (+0x33A)
- Cardef inputs: phys[0x20] inertia, [0x24] half_wb, [0x28] Wf, [0x2A] Wr, [0x2C] tire_grip, [0x6A] damp_high, [0x6C] damp_low, [0x6E] brake_front, [0x70] brake_rear, [0x74] speed_limit, [0x76] dt_layout, [0x7A] handbrake_factor, [0x7C] slip_coupling.

**Outputs (leave):**
- `lin_vx_out`, `lin_vz_out`, `ang_yaw_out`
- `long_speed_out` (+0x314), `lat_speed_out` (+0x318)
- `front_slip_excess_out` (+0x31C), `rear_slip_excess_out` (+0x320)
- `current_slip_metric_out` (+0x33C, i16)
- `tire_slip_x_out` (+0x340, i16), `tire_slip_z_out` (+0x342, i16)
- `surface_chassis_out` (+0x370, u8)
- `surface_contact_flags_out` (+0x376, u8)
- Suspension state passed to IntegrateWheelSuspensionTravel (only at exit, so include
  `center_suspension_pos_out`, `center_suspension_vel_out`, `susp_pos[0..3]_out`).

Schema is ~45 columns; single CSV.

## Next actions for the pilot

1. Author `tools/frida_pool0_00404030.js` capturing entry+leave on slot 0.
2. Author worktree `td5_pilot_trace_00404030.{c,h}` mirroring column layout.
3. Add hook calls inside `td5_physics_update_player` (enter + leave).
4. Wire into `build_standalone.bat` TD5RE_SRCS.
5. Build worktree.
6. Capture original via `td5_quickrace.py` with `--extra-script`.
7. Capture port via direct `td5re.exe` launch.
8. Diff via `tools/diff_func_trace.py`.
9. Iterate fixes: D6 (high priority structural), D5/D1 (LSB rounding), D4 (upstream).
10. Stop on zero diff OR document `blocked-by-upstream` with specific column.

## Blocked-by-upstream candidates (preview)

This function's outputs depend on inputs written upstream by:
- `RefreshVehicleWheelContactFrames (0x00403720)` — per-wheel surface bytes (D4)
- `UpdatePlayerVehicleControlState (somewhere)` — `steering_command`, `encounter_steering_cmd`, `brake_flag`, `handbrake_flag`
- `LoadVehicleAssets` — cardef binding (the same issue that blocked 00403A20)

If carparam binding or per-wheel surface bytes are wrong upstream, the input
columns will diverge before our function runs, and our output diff is not
informative. Same gating as the 00403A20 pilot.

## Runtime diff result — BLOCKED BY UPSTREAM

**Original capture (PlayerIsAI=false on launch):** 43 rows captured (sim_tick 5..30).
**Port capture (PlayerIsAI=0, AutoThrottle=1, Edinburgh):** 4 rows in the same window.

The port hits `td5_physics_update_player` only 4 times in ticks 5–30 because the
upstream dispatcher at `td5_physics.c:644-659` routes the actor into
`td5_physics_state0f_damping` whenever `wheel_contact_bitmask == 0x0F && airborne_frame_counter >= 3`.
At Edinburgh the port's wheel-contact bitmask sits at 0x0F for the majority of
the early-tick window (the same flicker pattern recorded in
`reference_wheel_mask_flicker_port_amplified`). The original calls 0x00404030
unconditionally for slot 0 in PlayerIsAI=false races — its state-0F damping
condition is downstream, not upstream of, the call.

Even on the 4 ticks where the port does dispatch to `td5_physics_update_player`,
every input column diverges severely. Selected first-common-tick (15, slot 0):

| Column | Port | Original | Notes |
|---|---|---|---|
| `surface_chassis_in` | 0 | 3 | Port's surface table is unwritten — upstream of 00403720 |
| `gear_in` | 5 | 2 | Port's auto-gear FSM is 3 gears ahead of original |
| `engine_speed_acc_in` | 0 | 0 | tied |
| `long_speed_in` | 1392 | 45312 | Port physics nowhere near original speed |
| `center_pos_in` | -478 | 268 | suspension at completely different state |
| `susp_pos[0..3]_in` | 6259 (all 4) | 265, 348, -520, -291 | port pinned at travel-limit |
| `lin_vx_in` | -1490 | -6488 | port ~5x slower |
| `front_slip_excess_out` | 0 | 448 | port slip-circle never engages |
| `tire_slip_z_out` | 1272 | 3007 | downstream of all above |

The integrator math inside 0x00404030 cannot be validated row-by-row when the
inputs differ this much. Same pattern as `pilot_00403A20_audit.md`:
**upstream-blocked** — fixing 0x00404030 in isolation will not produce a clean
diff until the chain that writes these inputs (0x00403720 surface bytes,
suspension integrator, gear FSM, drag pre-step) matches the original.

## Pilot deliverable on this branch

1. **Static port audit** (this document) — line-by-line listing→port mapping
   with 15 candidate divergences (D1..D15) and a Frida-capture column schema.
2. **Probe + emitter harness** committed:
   - `tools/frida_pool0_00404030.js`
   - `.claude/worktrees/precise-00404030/td5mod/src/td5re/td5_pilot_trace_00404030.{c,h}`
   - hook calls in `td5_physics_update_player` enter/leave
   - wired into `build_standalone.bat` TD5RE_SRCS
3. **Runtime diff confirms blocked-by-upstream** with the precise input columns
   that diverge first.

## Recommended unblocking pilots BEFORE retrying 00404030

The diff above identifies these upstream pilots as gating:

1. **0x00403720** (RefreshVehicleWheelContactFrames) — already merged; surface
   chassis byte still divergent at the *upstream* call. The port's chassis
   surface stays at 0 while original is 3. Worth a quick re-audit of the
   `surface_type_chassis` writeback path.
2. **0x00403A20** (IntegrateWheelSuspensionTravel) — already byte-correct in
   the integrator; but the inputs (center_pos, susp_pos[]) still diverge at
   travel-limit clamp because of upstream excitation source (player_fx/fz from
   00404030, or 0x004057F0 response). Circular dependency.
3. **Gear FSM 0x0042EF10** (UpdateAutomaticGearSelection) and
   **0x0042EDF0** (UpdateEngineSpeedAccumulator) — port's gear=5 at tick 15
   means the FSM advances faster than original. Likely the
   "auto-gear-on-microbumps" effect noted at td5_physics.c:980-983.

## What NOT to do

- Do NOT tune the rounding sites (D1, D5, D14) until the upstream input
  divergences are closed. Single-LSB fixes are invisible when inputs differ
  by 6000+ units.
- Do NOT remove the `wheel_contact_bitmask==0x0F` dispatch fallback at
  td5_physics.c:644 just to force exercise of `update_player` — that branch
  exists for a real game-state reason and removing it produces a different
  bug class.
- Do NOT modify the AI dispatch branch at line 656 — pilot 00404EC0 owns that.

## Reference

- Listing: 0x00404030..0x00404EB7 (Ghidra TD5_pool0, 2026-05-14)
- Decompilation: same session, 470+ lines
- Port: `td5mod/src/td5re/td5_physics.c:833-1791` (post-trace hooks)
- Frida probe: `tools/frida_pool0_00404030.js`
- Port emitter: `td5_pilot_trace_00404030.{c,h}` (worktree)
- Orig CSV: `log/orig/pool0_00404030.csv` (43 rows / sim_tick 1..30)
- Port CSV: `log/port/pool0_00404030.csv` (4 rows; upstream-blocked)
