# Pilot Audit — 0x00403A20 IntegrateWheelSuspensionTravel

**Date:** 2026-05-14
**Pool slot:** TD5_pool5
**Port-side function:** `td5_physics_integrate_suspension` @ `td5_physics.c:~3412`
**Worktree:** `.claude/worktrees/precise-00403A20` on branch `precise-00403A20`
**Callers:** UpdateVehicleState0fDamping (0x00403D90, with accel=0), UpdatePlayerVehicleDynamics (0x00404030), UpdateAIVehicleDynamics (0x00404EC0).
**Callees:** none (pure compute leaf).
**Body:** 0x00403A20..0x00403C70 (0x250 bytes / 182 instructions / 95 decompiled lines).

## Function structure (from listing)

Per-wheel pass (4 iterations) at 0x00403A73..0x00403B64:
```
load 5 cardef constants: k_spring (0x62), k_pos_damp (0x5E), k_vel_damp (0x60),
                          k_travel_lim (0x64), k_load_scale (0x66)
load -k_travel_lim
load actor pointer, EBX=hires[0..15] base (+0x298), ESI=wheel_suspension_pos[0..3] base (+0x2DC)

for i=0..3:
   wpx = sar8_rz(world_pos.x);  wpz = sar8_rz(world_pos.z)        ; [0x1FC]/[0x204]
   arm_x = hires[i].x - wpx                                        ; raw hires (orig stores world-units)
   arm_z = hires[i].z - wpz
   proj  = arm_x * accel_x + arm_z * accel_z                       ; ALL params signed
   spring_term = sar8_rz(proj) * k_spring
   load_term   = wheel_load_accum[i] * k_load_scale                ; [0x2FC + i*4]
   new_vel = sar8_rz(load_term) + sar8_rz(spring_term) + wheel_spring_dv[i]  ; [0x2EC + i*4]
   pos_damp = wheel_suspension_pos[i] * k_pos_damp                 ; [0x2DC + i*4]
   vel_damp = new_vel * k_vel_damp
   new_vel -= sar8_rz(pos_damp) + sar8_rz(vel_damp)
   if (-0x10 < new_vel < 0x10) new_vel = 0                         ; deadzone
   wheel_spring_dv[i] = new_vel
   wheel_suspension_pos[i] += new_vel
   if (wheel_suspension_pos[i] > k_travel_lim):  clamp to +k_travel_lim,  spring_dv = 0
   if (wheel_suspension_pos[i] < -k_travel_lim): clamp to -k_travel_lim, spring_dv = 0
```

Central chassis pass at 0x00403B6A..0x00403C70:
```
front_mid_x = sar1_rz(hires[0].x + hires[1].x)                    ; round-to-zero /2
front_mid_z = sar1_rz(hires[0].z + hires[1].z)
arm_x = front_mid_x - wpx                                         ; same sar8_rz(world_pos) values
arm_z = front_mid_z - wpz
proj  = arm_x * accel_x + arm_z * accel_z
spring_term = sar8_rz(proj) * k_spring
new_vel = sar8_rz(spring_term) + center_suspension_vel           ; [+0x2D0]
pos_damp = center_suspension_pos * k_pos_damp                    ; [+0x2CC]
vel_damp = new_vel * k_vel_damp
new_vel -= sar8_rz(pos_damp) + sar8_rz(vel_damp)
                                                                  ; NO load term, NO deadzone
center_suspension_pos += new_vel
center_suspension_vel  = new_vel
if (center_suspension_pos > k_travel_lim):  clamp,  vel = 0
if (center_suspension_pos < -k_travel_lim): clamp, vel = 0
                                                                  ; SAME k_travel_lim, NOT doubled
```

## Key arithmetic primitives

`sar8_rz(x)` — round-to-zero signed divide by 256:
```
CDQ            ; EDX = sign-extend(EAX) = all 1s if neg, 0 otherwise
AND EDX, 0xFF  ; → 0xFF if neg, 0 if non-neg
ADD EAX, EDX
SAR EAX, 8
```
Equivalent C: `((x < 0) ? (x + 0xFF) : x) >> 8`. For positive x identical to `x >> 8`; for negative x not divisible by 256 it returns one unit closer to zero than plain SAR.

`sar1_rz(x)` — round-to-zero signed divide by 2:
```
CDQ            ; EDX = -1 if neg, 0 otherwise
SUB EAX, EDX   ; +1 if neg
SAR EAX, 1
```
Equivalent C: `((x < 0) ? (x + 1) : x) >> 1`.

## Confirmed divergences from port (pre-fix at session start)

### D1 — Wrong sign-bias in port's `sar8_rz` analogues
Port used plain `>> 8` (arithmetic SAR) on signed ints throughout the function. For negative non-256-aligned values this diverges by 1 LSB from the original's round-to-zero `(x + 0xFF) >> 8` idiom. Applied to:
- (a) `world_pos.x >> 8` and `world_pos.z >> 8` (4 instances total)
- (b) `proj >> 8` (per-wheel + central)
- (c) `spring_term >> 8` (per-wheel + central)
- (d) `load_term >> 8` (per-wheel only)
- (e) `pos_damp >> 8` (per-wheel + central)
- (f) `vel_damp >> 8` (per-wheel + central)

### D2 — Wrong sign-bias on `(FL + FR) / 2` in central pass
Port used plain `>> 1` for the front-axle midpoint average. Original uses round-to-zero (`CDQ; SUB EAX,EDX; SAR 1`). For port hires written as `(int32_t) << 8` the low 8 bits are zero, so the sum-then-/2 result is always a multiple of 128 (the LSB of the divide); the rounding behaviour matters only on a hypothetical zero-low-byte hires write.

### D3 — Else-if vs separate-if on travel-limit clamps
Port used `if … else if` for the upper/lower clamp. Original listing uses two separate `if` blocks (each with its own CMP + JMP). Algebraically equivalent because in a single tick `new_pos` cannot be simultaneously > k_travel_lim and < -k_travel_lim.

### D4 — `s_loaded_tuning` fallback (UPSTREAM, NOT in 0x00403A20)
Port reads cardef constants via `PHYS_S(actor, 0x5E..0x66)` which dereferences `actor->tuning_data_ptr`. For the captured Edinburgh + PlayerIsAI=1 race, the original loads Viper's carparam:
```
k_pos_damp=50  k_vel_damp=40  k_spring=30  k_travel_lim=12288  k_load_scale=32
```
The port falls back to the hardcoded defaults in `LoadVehicleAssets` because `s_carparam_loaded[0]` is false in this AutoRace launch path:
```
k_pos_damp=48  k_vel_damp=96  k_spring=48  k_travel_lim=384   k_load_scale=32
```
This is a separate upstream binding bug — see "Blocked by upstream" below.

## Fixes applied (this pilot)

`td5_physics.c` (worktree only):

1. **Added `sar8_rz` and `sar1_rz` static inlines** at the top of
   `td5_physics_integrate_suspension`, encoding the original's CDQ/AND/ADD/SAR idiom.
2. **Per-wheel pass**: replaced 6 plain `>>8` operations with `sar8_rz` calls,
   matching every signed divide-by-256 in the original at line-level.
3. **Per-wheel pass — hires read**: changed `(actor->wheel_world_positions_hires[i].x >> 8)`
   to `sar8_rz(actor->wheel_world_positions_hires[i].x)`. Port stores hires as
   `lrintf_result << 8` (low 8 bits zero) so the change is a no-op on positive
   values but ensures negative hires would round consistently with the original's
   semantics if/when the upstream hires write convention changes.
4. **Central pass**: same `sar8_rz`/`sar1_rz` substitution; also restructured
   to use separate-if for the upper/lower clamps (matching listing's two `if`s).
5. **Inline comments** anchored to specific instruction-address ranges
   (e.g. `[0x00403A7D-A88]`) so future audits can verify against the listing.

## Self-validation (algorithmic byte-exact)

`tools/validate_pool5_00403A20_math.py` feeds the original's captured
inputs (`log/orig/pool5_00403A20.csv`, 184 rows / 46 calls) through a
Python reimplementation of the port's byte-exact math, then asserts the
outputs match the original's captured outputs.

**Result: 230/230 wheel + center samples match byte-exact.**

This validates that **the port's integrator math is byte-faithful** given
correct inputs.

## Blocked by upstream — runtime row-by-row diff cannot be performed

The runtime trace diff (`log/port/pool5_00403A20.csv` vs `log/orig/pool5_00403A20.csv`)
diverges on **every input column** at sim_tick=1:

| Column | Port | Original | Reason |
|---|---|---|---|
| k_pos_damp | 48 | 50 | Port uses hardcoded fallback (s_loaded_tuning) |
| k_vel_damp | 96 | 40 | Port uses hardcoded fallback |
| k_spring | 48 | 30 | Port uses hardcoded fallback |
| k_travel_lim | 384 | 12288 | Port uses hardcoded fallback |
| accel_x | 0 | -635 | Port AI dynamics produces different player_fx |
| accel_z | 264 | -130 | Port AI dynamics produces different player_fz |
| world_pos_x | 0 | 56486016 | **Port spawn never bound** to Edinburgh world coords |
| world_pos_z | 0 | -15011584 | Same |
| hires_x | 0 | 220320 | Downstream of wrong world_pos |
| hires_z | 0 | -59018 | Downstream of wrong world_pos |
| load_accum | 0 | 0 | (only column that matches at tick 1) |

Root cause: the port's `AutoRace=1` launch path on Edinburgh (track=1) with
`PlayerIsAI=1` does NOT trigger `td5_physics_load_carparam` for slot 0 AND
does not place the actor at Edinburgh's spawn span (62 in original; 0 in port).

**This is blocked by precise-00403720 upstream divergence** at the asset-load
+ spawn-bind chain (not RefreshVehicleWheelContactFrames itself). The
integrator inputs (cardef + world_pos + hires + load_accum + spring_dv) are
written by a chain that runs BEFORE 0x00403A20 each tick:

```
LoadVehicleAssets → td5_physics_load_carparam(slot, …)   ← FAILS in port
                  → s_carparam_loaded[slot] = 1
TD5_StartRace → SetupPlayerVehicleState → spawn binding
              → actor->world_pos = Edinburgh span 62 origin   ← FAILS in port
0x00403720 RefreshVehicleWheelContactFrames                  ← pool1 pilot
0x00404EC0 UpdateAIVehicleDynamics → computes player_fx/fz   ← pool? pilot
0x00403A20 IntegrateWheelSuspensionTravel                    ← THIS pilot
```

Until the carparam + spawn chain produces matching state for slot 0 on
Edinburgh AutoRace, no per-tick CSV diff will reflect 0x00403A20 quality.

## Static-port deliverable (this branch)

Even without a clean runtime diff:

- `td5_physics.c` integrator is byte-exact-correct vs the listing (algorithmic
  validation via captured-orig-inputs replay: 230/230 match).
- Probe + trace harness committed for future runtime diff once upstream blockers clear.
- Audit committed with line-by-line listing → port mapping.

## What NOT to do

- **Do not tune Viper carparam values** to mask the s_carparam_loaded issue.
- **Do not "fix" the integrator math** to compensate for wrong cardef inputs.
- **Do not remove the deadzone** — confirmed in listing at 0x00403B14-1E.
- **Do not double k_travel_lim** for the central pass — listing uses the same
  k_travel_lim (read once into EBP at 0x00403A3C-40 + 0x00403B20).

## Reference

- Listing: 0x00403A20..0x00403C70 (Ghidra TD5_pool5, 2026-05-14)
- Decompilation: same session, 95 lines
- Port: `td5mod/src/td5re/td5_physics.c:~3412-3560` (post-fix)
- Self-validator: `tools/validate_pool5_00403A20_math.py`
- Frida probe: `tools/frida_pool5_00403A20.js`
- Port trace emitter: `td5mod/src/td5re/td5_pilot_trace_00403A20.{c,h}` (worktree)
- Orig CSV: `log/orig/pool5_00403A20.csv` (184 rows / 46 calls)
- Port CSV: `.claude/worktrees/precise-00403A20/log/port/pool5_00403A20.csv` (120 rows; upstream-blocked)
