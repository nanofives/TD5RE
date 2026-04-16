# Test Drive 5: Tick-Rate-Dependent Physics Constants

**Date:** 2026-03-20
**Binary:** TD5_d3d.exe (Ghidra port 8193)
**Purpose:** Complete map of every constant that must change for a 30fps -> 60fps physics tick conversion.

---

## 0. Simulation Timing Framework (RunRaceFrame 0x42B580)

The physics loop is governed by a fixed-timestep accumulator:

```
while (g_simTimeAccumulator > 0xFFFF) {
    // ... all physics functions run once ...
    g_simTickBudget -= 1.0;               // _DAT_0045d5f4 (float 1.0)
    g_simTimeAccumulator -= 0x10000;       // hardcoded in RunRaceFrame
    g_simulationTickCounter += 1;
}
```

The accumulator is fed by `g_normalizedFrameDt` which is computed as:
```
elapsed_ms = (timeGetTime() - prev_timestamp)
g_normalizedFrameDt = elapsed_ms * double(0.001) * double(30.0)
                    // 0x45d658 = 0.001 (ms->s), 0x45d660 = 30.0 (target fps)
```

**For 60fps physics:**
- The target `30.0` at `0x45d660` (double) must become `60.0`
- The accumulator gate `0xFFFF` and subtraction `0x10000` effectively define a "tick" as 1/30s of wall time when the target is 30.0. At 60fps target, the accumulator drains twice as fast, so physics runs 2x per frame, which is correct.
- `g_simTickBudget` cap at `_DAT_0045d650` (float 4.0) = max 4 ticks catchup. At 60fps this should become 8.0.
- Benchmark override sets budget to 3.0; would need 6.0 for 60fps.

| Address | File Offset | Type | Current | 60fps Value | Role |
|---------|-------------|------|---------|-------------|------|
| `0x45d660` | 0x5d660 | double | 30.0 | **60.0** | Target ticks/sec in dt normalization |
| `0x45d650` | 0x5d650 | float | 4.0 | **8.0** | Max tick catchup budget |

---

## 1. Gravity (gGravityConstant @ 0x467380)

**Applied per tick** in two locations:

### 1a. ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
Applied to **slope-projected** velocity components (actor+0x1CC and actor+0x1D4):
```
slope_force_x = (gGravityConstant * cross_product_x) / 2   >> 12
actor->vel_x += slope_force_x      // per tick
slope_force_z = (gGravityConstant * cross_product_z) / 2   >> 12
actor->vel_z += slope_force_z      // per tick
```

### 1b. IntegrateVehiclePoseAndContacts (0x405E80)
Applied to **vertical velocity** (actor+0xE8):
```
actor->vel_y -= gGravityConstant   // per tick, direct subtraction
```

Also in UpdateVehicleSuspensionResponse (0x4057F0):
```
vertical_accumulator += gGravityConstant   // per tick, for grounded bounce
```

### 1c. Written by InitializeRaceVehicleRuntime (0x42F140)

| Condition | Value | Address Written |
|-----------|-------|-----------------|
| Hard (DAT_004aaf80 != 0) | **0x800** (2048) | 0x467380 |
| Easy (DAT_004aaf84 != 0) | **0x5DC** (1500) | 0x467380 |
| Normal (default) | **0x76C** (1900) | 0x467380 |

**For 60fps:** All three gravity values must be **halved**.

| Address | File Offset | Current | 60fps | Notes |
|---------|-------------|---------|-------|-------|
| 0x42F29F (imm32) | 0x2F29F | 0x800 | **0x400** | Hard gravity (MOV dword) |
| 0x42F354 (imm32) | 0x2F354 | 0x5DC | **0x2EE** | Easy gravity |
| 0x42F360 (imm32) | 0x2F360 | 0x76C | **0x3B6** | Normal gravity |

---

## 2. Velocity Damping (Per-Tick Multiplicative Decay)

### 2a. Player/AI Vehicle Linear Damping (UpdatePlayerVehicleDynamics 0x404030 / UpdateAIVehicleDynamics 0x404EC0)

Both paths apply the same damping formula per tick:
```
damping_coeff = surface_friction_table[surface_type] * 0x100 + tuning_table->linear_drag
vel_x -= (vel_x >> 8) * damping_coeff >> 12
vel_z -= (vel_z >> 8) * damping_coeff >> 12
```

The damping is `1 - coeff/4096` per tick. For 60fps, the damping coefficient must be halved (since `(1-d/4096)^2 approx 1-2d/4096` for small d).

The surface friction table is at **DAT_00474900** (16 short entries):
```
0x474900: [0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, ...]
```

The tuning-table drag values are at `carparam+0x6A` (easy) and `carparam+0x6C` (normal/hard). These are per-vehicle data from `carparam.dat`, not hardcoded. The surface table values are small (0-8), and the main drag comes from carparam. **No hardcoded EXE patch needed for these -- they are data-driven.**

However, the `* 0x100` multiplier at the surface table lookup IS hardcoded. For 60fps this should become `* 0x80`.

### 2b. Traffic Vehicle Damping (IntegrateVehicleFrictionForces 0x4438F0)

Traffic actors use explicit per-tick damping:
```
vel_x -= (vel_x * 0x10) >> 12    // = vel_x * 16/4096 = vel_x / 256
vel_z -= (vel_z * 0x10) >> 12    // same
```

**Hardcoded 0x10** (16): This is a per-tick velocity decay of 1/256 per tick.

For 60fps, this should become **0x08** (8).

| Address | File Offset | Current | 60fps | Notes |
|---------|-------------|---------|-------|-------|
| In IntegrateVehicleFrictionForces | (instruction-level) | 0x10 | **0x08** | Traffic linear damping fraction |

### 2c. State-0x0F Damping (UpdateVehicleState0fDamping 0x403D90)

Damped yaw/pitch decay when vehicle is in "horn boost" state:
```
yaw_vel -= yaw_vel >> 4      // 1/16 per tick
pitch_vel -= pitch_vel >> 4  // 1/16 per tick
```

For 60fps, the shift should effectively halve: `>> 5` (1/32 per tick).

---

## 3. Per-Tick Additive Forces

### 3a. Yaw Torque Clamp (UpdatePlayerVehicleDynamics / UpdateAIVehicleDynamics)

Both functions clamp yaw torque delta to +/- 0x578 (1400) per tick:
```
if (yaw_delta > 0x578) yaw_delta = 0x578;
if (yaw_delta < -0x578) yaw_delta = -0x578;
```

For 60fps, clamp to **0x2BC** (700).

### 3b. Angular Velocity Clamp from Suspension (UpdateVehicleSuspensionResponse 0x4057F0)

Roll/pitch angular velocity clamped to +/- 4000 per tick:
```
if (roll_vel > 4000) roll_vel = 4000;
if (roll_vel < -4000) roll_vel = -4000;
```

For 60fps: clamp to **2000**.

### 3c. Attitude Recovery Additive (ClampVehicleAttitudeLimits 0x405B40)

When vehicle is over-rotated, per-tick angular correction:
```
actor->roll_vel += 0x200     // or -= 0x200
actor->pitch_vel += 0x200    // or -= 0x200
```

For 60fps: **0x100** per tick.

### 3d. Angular Velocity Settlement (IntegrateVehiclePoseAndContacts 0x405E80)

Contact-constrained angular rate clamped to +/- 6000:
```
if (angular_delta > 6000) angular_delta = 6000;
if (angular_delta < -6000) angular_delta = -6000;
```

For 60fps: clamp to **3000**.

### 3e. Velocity Deadband (UpdatePlayerVehicleDynamics 0x404030)

When all three velocity components are below thresholds, zero them:
```
if (|vel_x| < 0x40 && |vel_z| < 0x40 && |yaw_vel| < 0x20) {
    vel_x = vel_z = yaw_vel = 0;
}
```

For 60fps: halve thresholds to **0x20, 0x20, 0x10** (since velocities are half as large per tick).

---

## 4. Steering Ramp Rates (UpdatePlayerVehicleControlState 0x402E60)

### 4a. Steering Ramp-Up Factor

The steering acceleration `ramp_factor` is computed from speed and applied per tick:
```
ramp_factor = 0xC0000 / (speed_factor + 0x40)
```

When turning LEFT/RIGHT:
```
// Ramp acceleration: steer_cmd -= ramp_factor * acceleration_scale >> 8
// Snap decay: steer_cmd += ramp_factor * -2  (when crossing zero)
```

The `0xC0000` (786432) numerator controls total steering rate. For 60fps, this should become **0x60000** (393216).

### 4b. Steering Limit Rate

The steering limit `iVar6` computed from:
```
limit_factor = 0xC00000 / (speed_component + 0x80)
```

The `0xC00000` (12582912) numerator should become **0x600000** for 60fps.

### 4c. Steering Centering Rate

When no direction is pressed, steering returns to zero:
```
steer_cmd += ramp_factor * 4    // or -= ramp_factor * 4
```

The `* 4` multiplier is the centering speed ratio. For 60fps: `* 2`.

### 4d. Steering Ramp Accumulator

```
accel_scale += 0x40 per tick (up to 0x100 max)
// When neither direction pressed:
accel_scale -= 0x40 per tick (down to 0)
```

For 60fps: increment/decrement **0x20** per tick.

### 4e. Horn/Siren Velocity Boost (bit 0x200000)

Applied as a one-shot velocity impulse per tick while button held:
```
actor->height += 0x6400     // vertical boost
actor->vel_x *= 2           // double velocity
actor->vel_y *= 2
// clamp to +/- 400000
```

The `0x6400` (25600) is a per-activation impulse, NOT a per-tick rate, so it does NOT need halving. But the velocity doubling is instantaneous, also not per-tick.

### 4f. Gear Debounce Counter

```
gPlayerControlBuffer[slot] = 5    // 5-tick debounce after gear change
decremented by 1 per tick
```

For 60fps: set debounce to **10** (keeps same 1/6 second timing).

---

## 5. Collision Impulse (ApplyVehicleCollisionImpulse 0x4079C0)

The collision impulse formula uses:
- `DAT_00463204` = **500,000** (inertia constant K) -- address `0x463204`, file offset `0x63204`
- `DAT_00463200` = **1,500,000** (unused in impulse, used in UpdateVehicleActor distance accumulation)
- `0x28C` (652) = angular-to-linear conversion divisor
- `0x1100` (4352) = impulse numerator multiplier

The impulse formula is:
```
impulse = ((K>>8) * 0x1100 / (denominator>>8)) * v_rel >> 12
```

**Collision impulses are NOT tick-rate dependent.** They are applied instantaneously when contact is detected. The relative velocity `v_rel` will naturally be halved at 60fps (since velocities accumulate at half rate), so impulse magnitudes self-correct.

However, the **collision separation push** at the end of `ApplyVehicleCollisionImpulse` does scale existing velocities by elasticity `(0x100 - penetration_depth)`. Since both vehicles move half as far per tick at 60fps, the separation pushes remain correct.

**No changes needed for collision constants.**

---

## 6. AI System

### 6a. ComputeAIRubberBandThrottle (0x432D60)

The rubber-band system computes a throttle multiplier based on distance to leader:
```
rubber_band_output = 0x100 - (distance_scale * distance / max_distance)
```

Constants at `0x473D9C`:
- `DAT_00473D9C` = 0x80 (behind catch-up distance max)
- `DAT_00473DA0` = 0x80 (behind catch-up scale)
- `DAT_00473DA4` = 0x80 (ahead slow-down distance max)
- `DAT_00473DA8` = 0x80 (ahead slow-down scale)

**These are NOT tick-rate-dependent.** They scale the throttle multiplier, which is then applied to the drive torque. The torque itself is already tick-dependent through the dynamics path.

### 6b. UpdateActorRouteThresholdState (0x434AA0)

Three-state threshold check:
```
threshold_byte = route_table[span*3 + 2]
scaled_threshold = (threshold_byte * 0x400) / 0xFF
```

Compares against `gActorForwardTrackComponent` (forward speed projection). The thresholds are speed comparisons, and since speed = accumulated velocity which is half per tick at 60fps, the comparison naturally self-corrects.

**No changes needed for AI routing constants.**

---

## 7. Particle System

### 7a. Particle Spawn (SpawnVehicleSmokeVariant 0x429A30)

Per-spawn constants (NOT per-tick -- set once at creation):
```
velocity_y = 0x1800                          // upward velocity
velocity_xz = rand(-0x3000..0x3000)          // lateral scatter
lifetime = rand() % 0x1F                     // ticks until death
scale_initial = 0x7000
scale_velocity = 0x2080
scale_decay = -0x3000 / lifetime
alpha_grow = 0x1900 / lifetime
```

### 7b. Particle Update (callback at 0x4297D0)

The particle update callback is invoked once per physics tick via `UpdateRaceParticleEffects`. Each call:
- Advances position by velocity (per-tick)
- Decrements lifetime counter by 1
- Adjusts scale/alpha by per-tick rates

**Lifetime** is in ticks. For 60fps, lifetimes would be consumed at 2x rate (particles die in half the wall time). The spawn function sets `lifetime = rand() % 0x1F` (0-30 ticks = 0-1s at 30fps). For 60fps, this should be `rand() % 0x3E` (0-61 ticks).

**Velocity constants** (0x1800, 0x3000, 0x2080, 0x7000, etc.) are added per tick and would need halving, but they are runtime-computed at spawn time. The key constants are:

| Address | Constant | Current | 60fps | Notes |
|---------|----------|---------|-------|-------|
| In SpawnVehicleSmokeVariant | 0x1800 | 6144 | **0xC00** (3072) | Vertical velocity |
| In SpawnVehicleSmokeVariant | 0x3000 | 12288 | **0x1800** (6144) | Lateral velocity range |
| In SpawnVehicleSmokeVariant | 0x6000 | 24576 | **0x3000** (12288) | Lateral velocity range (doubled) |
| In SpawnVehicleSmokeVariant | 0x1F | 31 | **0x3E** (62) | Max lifetime in ticks |
| In SpawnVehicleSmokeVariant | 0x7000 | 28672 | unchanged | Initial scale (not per-tick) |
| In SpawnVehicleSmokeVariant | 0x2080 | 8320 | unchanged | Initial alpha (not per-tick) |

NOTE: The `scale_decay` and `alpha_grow` are computed as `constant / lifetime`, so they auto-adjust if lifetime doubles. Only the velocity and lifetime values need explicit patching.

---

## 8. Camera Transition Timer (UpdateRaceCameraTransitionTimer 0x40A490)

```
g_cameraTransitionActive -= 0x100 per tick    // decay rate
threshold = g_cameraTransitionActive / 0x2800  // phase gate
```

For 60fps, the decay should be **0x80** per tick. The `0x2800` divisor would also need halving to **0x1400** to maintain the same wall-time phase boundaries.

---

## 9. Tracked Actor Marker Decay (DecayTrackedActorMarkerIntensity 0x43D7E0)

```
DAT_004bf500 -= 0x200 per tick    // marker intensity decay (max 0x1000)
```

For 60fps: decay **0x100** per tick.

---

## 10. Tire Track Pool (UpdateTireTrackPool 0x43EB50)

The tire track system works by comparing wheel position delta per tick. No hardcoded velocity constants -- it computes displacement from actual wheel positions. The segment-split threshold compares angle deltas (`0x80` bit threshold on angle XOR).

At 60fps, wheels move half as far per tick, so tracks will spawn **more segments** (finer resolution). This is cosmetically different but not incorrect. The segment budget limit is 0x50 (80) slots, which may fill faster.

**No critical constants to change**, but the visual density will increase. If this is unwanted, the angle change threshold could be tightened.

---

## 11. Suspension Spring-Damper (IntegrateWheelSuspensionTravel 0x403A20)

Uses per-vehicle tuning parameters from `carparam.dat`:
- `+0x5E` (sVar2): damping rate
- `+0x60` (sVar3): spring rate
- `+0x62` (sVar1): force coupling
- `+0x64` (iVar7): travel limit
- `+0x66` (sVar4): cross-coupling

The update is:
```
force = coupling * (wheel_contact_projection) + previous_bias * cross_coupling + velocity
velocity = velocity + force - velocity * damping_rate - force * spring_rate
position += velocity
```

This is a spring-damper integrator running once per tick. The spring/damper coefficients are per-vehicle data, not hardcoded. At 60fps:
- Spring stiffness (sVar3) should be halved
- Damping (sVar2) should be halved
- Force coupling (sVar1) should be halved

**These are in carparam.dat, not hardcoded in the EXE.** However, the same formula is used for ALL vehicles, so a global scale factor could be applied by patching the integration function itself.

The **deadband threshold** of 0x10 in the integration:
```
if (-0x10 < velocity && velocity < 0x10) velocity = 0;
```

For 60fps: threshold should become **0x08**.

---

## 12. Traffic Actor Physics (IntegrateVehicleFrictionForces 0x4438F0)

Several hardcoded constants that scale with tick rate:

| Constant | Value | Dec | Role | 60fps |
|----------|-------|-----|------|-------|
| Linear drag multiplier | 0x10 | 16 | vel *= (1 - 16/4096) per tick | **0x08** |
| Longitudinal force scale | 400 | 400 | tire force coupling | **200** |
| Lateral force multiplier | 800 | 800 | lateral grip force | **400** |
| Front grip coefficient | 0x271 | 625 | front axle lateral grip | **0x139** (~313) |
| Rear grip coefficient | 0x14C | 332 | rear axle lateral grip | **0xA6** (~166) |
| Yaw torque per-tick scale | 0x114 | 276 | yaw inertia divisor | **0x228** (556) -- DOUBLE |
| Lateral clamp | 0x800 | 2048 | max lateral force per tick | **0x400** |
| Result: position integration | `pos += vel` per tick | - | Euler integration step | natural (half vel = half pos delta) |

---

## 13. Vehicle Speed Accumulator (UpdateVehicleActor 0x406650)

Per-tick distance odometer:
```
speed = |velocity| >> 8
distance_accum += speed    // per tick
```

This is used for race timing/scoring. At 60fps, speed is halved but ticks are doubled, so the accumulation rate stays the same. **No changes needed.**

---

## 14. Race End Timer

```
if (timeGetTime() - g_raceEndTimerStart > 45000) ...  // 45 second timeout
```

Wall-clock based, **no changes needed**.

---

## COMPLETE PATCH TABLE (Priority Order)

### CRITICAL (Game-breaking if not changed)

| # | Address | File Offset | Current | 60fps | Description |
|---|---------|-------------|---------|-------|-------------|
| 1 | `0x45d660` | 0x5d660 | double 30.0 | **double 60.0** | Sim tick target rate |
| 2 | `0x42F29F` (imm32) | 0x2F29F | 0x800 | **0x400** | Gravity (hard difficulty) |
| 3 | `0x42F354` (imm32) | 0x2F354 | 0x5DC | **0x2EE** | Gravity (easy difficulty) |
| 4 | `0x42F360` (imm32) | 0x2F360 | 0x76C | **0x3B6** | Gravity (normal difficulty) |
| 5 | `0x45d650` | 0x5d650 | float 4.0 | **float 8.0** | Max tick catchup budget |

### HIGH (Physics feel significantly wrong)

| # | Address | Location | Current | 60fps | Description |
|---|---------|----------|---------|-------|-------------|
| 6 | Inline @ UpdatePlayerVehicleDynamics | yaw clamp | 0x578 | **0x2BC** | Max yaw torque per tick |
| 7 | Inline @ UpdateAIVehicleDynamics | yaw clamp | 0x578 | **0x2BC** | Max yaw torque (AI) |
| 8 | Inline @ UpdateVehicleSuspensionResponse | roll/pitch clamp | 4000 | **2000** | Suspension angular vel clamp |
| 9 | Inline @ ClampVehicleAttitudeLimits | recovery rate | 0x200 | **0x100** | Attitude correction per tick |
| 10 | Inline @ IntegrateVehiclePoseAndContacts | angular rate clamp | 6000 | **3000** | Contact angular delta clamp |
| 11 | Inline @ IntegrateWheelSuspensionTravel | deadband | 0x10 | **0x08** | Suspension velocity deadband |
| 12 | Inline @ UpdatePlayerVehicleDynamics | vel deadband | 0x40/0x20 | **0x20/0x10** | Chassis settling threshold |

### MEDIUM (Steering/control feel different)

| # | Address | Location | Current | 60fps | Description |
|---|---------|----------|---------|-------|-------------|
| 13 | Inline @ UpdatePlayerVehicleControlState | steer rate num | 0xC0000 | **0x60000** | Steering ramp rate |
| 14 | Inline @ UpdatePlayerVehicleControlState | steer limit num | 0xC00000 | **0x600000** | Steering limit rate |
| 15 | Inline @ UpdatePlayerVehicleControlState | centering mult | *4 | ***2** | Steering return-to-center |
| 16 | Inline @ UpdatePlayerVehicleControlState | accel_scale step | 0x40 | **0x20** | Steering accel ramp |
| 17 | Inline @ UpdatePlayerVehicleControlState | gear debounce | 5 | **10** | Gear change lockout ticks |

### LOW (Visual/cosmetic differences)

| # | Address | Location | Current | 60fps | Description |
|---|---------|----------|---------|-------|-------------|
| 18 | Inline @ SpawnVehicleSmokeVariant | velocity_y | 0x1800 | **0xC00** | Particle upward velocity |
| 19 | Inline @ SpawnVehicleSmokeVariant | lateral_vel | 0x3000/0x6000 | **0x1800/0x3000** | Particle scatter |
| 20 | Inline @ SpawnVehicleSmokeVariant | lifetime | 0x1F | **0x3E** | Particle lifetime ticks |
| 21 | Inline @ DecayTrackedActorMarkerIntensity | decay rate | 0x200 | **0x100** | Wanted-mode marker fade |
| 22 | Inline @ UpdateRaceCameraTransitionTimer | decay rate | 0x100 | **0x80** | Camera blend decay |
| 23 | Inline @ UpdateRaceCameraTransitionTimer | phase div | 0x2800 | **0x1400** | Camera phase threshold |
| 24 | Inline @ UpdateVehicleState0fDamping | damping shift | >>4 | **>>5** | Horn state vel decay |

### DATA-DRIVEN (In carparam.dat, not EXE -- per-vehicle)

These do NOT need EXE patches but DO need carparam.dat adjustments or a runtime multiplier:

| Field | Offset in carparam | Role | 60fps Action |
|-------|-------------------|------|-------------|
| `linear_drag` | +0x6A / +0x6C | Velocity damping rate | Halve |
| `spring_rate` | +0x60 | Suspension spring | Halve |
| `damping_rate` | +0x5E | Suspension damper | Halve |
| `force_coupling` | +0x62 | Wheel force transfer | Halve |
| `cross_coupling` | +0x66 | Suspension cross-talk | Halve |
| `brake_decel` | +0x6E | Brake force per tick | Halve |
| `reverse_brake` | +0x70 | Reverse brake rate | Halve |
| `grip_table[surface]` | 0x4748C0 (EXE) | Per-surface grip multiplier | Halve (but see note) |

### NATURALLY SELF-CORRECTING (No changes needed)

| System | Reason |
|--------|--------|
| Collision impulses | Relative velocity auto-halves; impulse magnitude correct |
| Race order (UpdateRaceOrder) | Comparison-only, no per-tick accumulation |
| AI rubber-band throttle | Scales throttle multiplier, not velocity directly |
| AI route thresholds | Speed comparison auto-adjusts |
| Distance odometer | Halved speed * 2x ticks = same accumulation |
| Race end timer | Wall-clock (timeGetTime), not tick-based |
| Ambient particle density | Segment-based spawning, not tick-rate |
| Tire track spawning | Position-delta based, cosmetically finer at 60fps |

---

## KEY GLOBALS REFERENCE

| Address | File Offset | Size | Value | Name |
|---------|-------------|------|-------|------|
| `0x467380` | 0x67380 | dword | 0x5DC/0x76C/0x800 | gGravityConstant (runtime, set at race init) |
| `0x463200` | 0x63200 | dword | 1,500,000 | Inertia constant A (actor distance scoring) |
| `0x463204` | 0x63204 | dword | 500,000 | Inertia constant K (collision impulse denominator) |
| `0x474900` | 0x74900 | 16 shorts | [0,0,0,0,0,2,0,0,8,0,...] | Surface friction additive table |
| `0x4748C0` | 0x748C0 | 16 shorts | [0x100,0x100,0xDC,...] | Surface grip multiplier table |
| `0x473D9C` | 0x73D9C | 4 dwords | [0x80,0x80,0x80,0x80] | AI rubber-band distance parameters |
| `0x45d5E8` | 0x5d5E8 | float | 1/256 (0.00390625) | Fixed->float angle conversion |
| `0x45d5F4` | 0x5d5F4 | float | 1.0 | Tick budget decrement per step |
| `0x45d660` | 0x5d660 | double | 30.0 | Target physics tick rate |
| `0x45d650` | 0x5d650 | float | 4.0 | Max tick catchup frames |
