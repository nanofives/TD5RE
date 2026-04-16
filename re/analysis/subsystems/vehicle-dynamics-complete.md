# Test Drive 5 -- Complete Vehicle Dynamics & Physics Pipeline

> Generated 2026-03-20 from full decompilation of all 24 vehicle dynamics functions in TD5_d3d.exe.
> All addresses are virtual addresses in the loaded PE image (base 0x00400000).

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Three Physics Tiers](#three-physics-tiers)
3. [Actor Struct Physics Fields](#actor-struct-physics-fields)
4. [Tuning Data Layout](#tuning-data-layout)
5. [Per-Frame Update Pipeline](#per-frame-update-pipeline)
6. [Tire / Grip Model](#tire--grip-model)
7. [Suspension System](#suspension-system)
8. [Engine & Transmission](#engine--transmission)
9. [Steering](#steering)
10. [Integration & Pose Update](#integration--pose-update)
11. [Surface Normal & Gravity](#surface-normal--gravity)
12. [Attitude Clamping & Recovery](#attitude-clamping--recovery)
13. [Missing Wheel Correction](#missing-wheel-correction)
14. [Vehicle Initialization](#vehicle-initialization)
15. [Suspension Envelope (Bounding Volume)](#suspension-envelope)
16. [Difficulty Scaling](#difficulty-scaling)
17. [Function Reference Table](#function-reference-table)

---

## Architecture Overview

TD5 uses a **fixed-point physics engine** with 8.8 and 12.12 fractional formats. There is no floating-point in the core simulation loop -- all positions, velocities, forces, and angles are integer fixed-point. Floating-point is used only at the boundary (render matrix construction, bounding volume computation).

The engine runs three completely separate vehicle dynamics models depending on actor type:

| Tier | Function | Wheels | Surface Probes | Grip Range | Used By |
|------|----------|--------|----------------|------------|---------|
| **Player** | `UpdatePlayerVehicleDynamics` (0x404030) | 4 independent | 5 (chassis + 4 wheels) | [0x38, 0x50] | Human-controlled racers |
| **AI** | `UpdateAIVehicleDynamics` (0x404ec0) | 2 axles | 1 (chassis center) | [0x70, 0xA0] | AI-controlled racers |
| **Traffic** | `IntegrateVehicleFrictionForces` (0x4438f0) | 2 axles (simplified) | 1 | Fixed constants | Traffic vehicles (slots 6-11) |

All three tiers share the same actor struct (0x388 bytes) but use different subsets of fields.

---

## Three Physics Tiers

### Player 4-Wheel Model (0x404030)

The player model is the most complex. Key characteristics:
- **5 surface probes**: One per wheel + one chassis center, each calling `GetTrackSegmentSurfaceType`
- **Per-wheel grip**: Each wheel gets independent grip from surface type lookup table at `DAT_004748c0` (friction coefficient) and `DAT_00474900` (drag coefficient)
- **Grip clamped [0x38, 0x50]**: Hardcoded min/max per wheel -- approximately 22%-31% in 8.8 fixed
- **Rear grip modifier**: When `actor+0x36E != 0` (handbrake/e-brake), rear wheels get scaled by `tuning+0x7A`
- **Drivetrain split**: Supports FWD (type 1), RWD (type 2), AWD (type 3) via `tuning+0x76` byte
- **Tire slip circle**: Uses `sqrt(lateral^2 + longitudinal^2)` to enforce combined slip limit
- **Per-wheel force accumulators** at actor+0x2EC..0x2F8 (4 ints, one per wheel)
- **Wheel contact mask** at actor+0x37C (4-bit bitmask, 1 = airborne)

### AI 2-Axle Model (0x404ec0)

Simplified but structurally similar:
- **1 surface probe**: Chassis center only (`GetTrackSegmentSurfaceType` on actor+0x80)
- **2-axle split**: Front/rear load distribution based on weight transfer from suspension deflection
- **Grip clamped [0x70, 0xA0]**: Nearly double the player range (44%-63%) -- this is how AI compensates for fewer probes
- **Same drivetrain logic**: gear selection, torque curves, brake/throttle
- **Same yaw integration**: steering torque -> yaw rate -> heading update
- **No per-wheel slip circle**: Uses a simpler combined force model
- **Calls `IntegrateWheelSuspensionTravel` at end** (same as player)

### Traffic Simplified Model (0x4438f0)

Minimal physics for non-interactive background vehicles:
- **Fixed drag coefficients**: 0x10/4096 velocity damping per axis
- **2-axle bicycle model**: Front steered, rear fixed
- **Hardcoded grip constants**: 0x271 (front longitudinal), 0x14C (rear longitudinal)
- **No surface-type lookup**: Uses constant friction regardless of terrain
- **Steering from AI route**: `actor+0x30C` = steering angle, `actor+0x33E` = throttle
- **Force clamped to [-0x800, 0x800]** per axle
- **Calls `ApplyDampedSuspensionForce`** for simple pitch/roll spring-damper
- **Integrates position directly** at end (no separate integration step)

---

## Actor Struct Physics Fields

Key offsets within the 0x388-byte actor struct (all relative to actor base):

### Position & Orientation
| Offset | Size | Description |
|--------|------|-------------|
| 0x0F8 | int | Euler angle X (roll) << 8 |
| 0x0FA | int | Euler angle Y (pitch) << 8 |
| 0x0FC | int | Euler angle Z (yaw) << 8 |
| 0x0FE | int | World position X (high-precision) |
| 0x100 | int | World position Y (vertical) |
| 0x102 | int | World position Z |
| 0x104 | short | Euler X display (>>8 of 0xF8) |
| 0x105 | short | Euler Y display |
| 0x106 | short | Euler Z display |
| 0x120 | 36B | 3x3 rotation matrix (int[9]) |
| 0x144 | float[3] | Render position (float, for 3D engine) |

### Velocities & Angular Rates
| Offset | Size | Description |
|--------|------|-------------|
| 0x1C0 | int | Angular velocity roll |
| 0x1C4 | int | Angular velocity yaw (heading rate) |
| 0x1C8 | int | Angular velocity pitch |
| 0x1CC | int | Linear velocity X (world) |
| 0x1D0 | int | Linear velocity Y (vertical) |
| 0x1D4 | int | Linear velocity Z (world) |

### Forces (Player model -- per-tick accumulators)
| Offset | Size | Description |
|--------|------|-------------|
| 0x0E0 | int | Force/torque accumulator roll |
| 0x0E2 | int | Force accumulator yaw |
| 0x0E4 | int | Force accumulator pitch |
| 0x0E6 | int | Force accumulator X |
| 0x0E8 | int | Force accumulator Y (vertical) |
| 0x0EA | int | Force accumulator Z |

### Wheel Contact / Suspension
| Offset | Size | Description |
|--------|------|-------------|
| 0x166 | int | Suspension deflection (center, used for load transfer) |
| 0x16E | int[4] | Per-wheel suspension deflection |
| 0x17E | uint[4] | Per-wheel vertical force (or 12000 if airborne) |
| 0x1BE | byte | Wheel contact bitmask (bit=1 means airborne) |
| 0x208 | short | Display roll angle (0x800-centered) |
| 0x20C | short | Display pitch angle (0x800-centered) |
| 0x230+ | short[4*4] | Per-wheel track contact data (4 wheels x 8 shorts) |
| 0x270+ | short[4*4] | Per-wheel contact velocity vectors |
| 0x298+ | int[4*3] | Per-wheel world-space positions |
| 0x2CC | int | Central suspension accumulator (roll/pitch combined) |
| 0x2D0 | int | Central suspension velocity |
| 0x2DC | int[4+4+4] | Per-wheel: [position, velocity, contact_force] x4 |
| 0x2EC | int[4] | Per-wheel force accumulators (steering/drive torque) |

### Engine & Drivetrain
| Offset | Size | Description |
|--------|------|-------------|
| 0x310 | int | Engine speed accumulator (RPM-like, 0-based from idle=400) |
| 0x314 | int | Longitudinal speed (body-frame, signed, 8.8 fixed) |
| 0x318 | int | Lateral speed (body-frame) |
| 0x31C | int | Front axle slip excess (for tire squeal/effects) |
| 0x320 | int | Rear axle slip excess |
| 0x33E | short | Throttle/brake command (signed: +ve=throttle, -ve=brake) |
| 0x340 | short | Accumulated tire slip X (for sound/effects) |
| 0x342 | short | Accumulated tire slip Z |
| 0x36B | byte | Current gear (0=reverse, 1=neutral, 2-8=forward gears) |
| 0x36D | byte | Brake flag (!=0 means braking) |

### State & Control
| Offset | Size | Description |
|--------|------|-------------|
| 0x370 | byte | Surface type under chassis (AI model) |
| 0x376 | byte[16] | State flags array (see UpdateVehicleActor dispatcher) |
| 0x376[0] | byte | Tire screech flag |
| 0x376[3] | byte | Vehicle mode: 0=normal, 1=scripted, etc. |
| 0x376[5] | byte | Cleared each frame |
| 0x376[6] | byte | Sub-state (0x0F = damping mode) |
| 0x376[10] | byte | Grip reduction (clamped to min of this and [13]) |
| 0x376[13] | byte | Base grip level |
| 0x379 | byte | Recovery mode flag |
| 0x37C | byte | Wheel contact bitmask (duplicate/alias of 0x1BE) |
| 0x37D | byte | Previous-frame wheel contact bitmask |
| 0x1B7 | byte | Handbrake/e-brake active |
| 0x1BB | byte | Drivetrain type runtime (bitmask: bit0=front, bit1=rear) |
| 0x1BC | int | Tuning data pointer |

---

## Tuning Data Layout

The tuning data block is pointed to by `actor+0x1BC`. Key offsets within the tuning block:

| Offset | Size | Description |
|--------|------|-------------|
| 0x20 | int | Inertia / mass factor |
| 0x24 | int | Suspension half-travel (used in load transfer) |
| 0x28 | short | Front axle weight distribution |
| 0x2A | short | Rear axle weight distribution |
| 0x2C | short | Total weight / grip base |
| 0x2E+ | short[8] | Per-gear ratio table (gear 0=reverse through gear 8) |
| 0x32+ | short[~16] | Drive torque curve (sampled every 512 RPM units) |
| 0x3E+ | short[8] | Upshift RPM thresholds (per gear) |
| 0x4E+ | short[8] | Downshift RPM thresholds (per gear) |
| 0x5E | short | Suspension spring rate |
| 0x60 | short | Suspension damping rate |
| 0x62 | short | Suspension resonance frequency |
| 0x64 | short | Suspension travel limit |
| 0x66 | short | Suspension contact force multiplier |
| 0x68 | short | Steering sensitivity (scaled by difficulty) |
| 0x6A | short | High-speed drag coefficient |
| 0x6C | short | Low-speed/idle drag coefficient |
| 0x6E | short | Brake force front |
| 0x70 | short | Brake force rear |
| 0x72 | short | Engine redline / max RPM |
| 0x74 | short | Speed limiter threshold |
| 0x76 | byte | Drivetrain type: 1=FWD, 2=RWD, 3=AWD |
| 0x78 | short | Brake balance (scaled by difficulty) |
| 0x7A | short | Rear grip modifier (for handbrake) |
| 0x7C | short | Tire slip threshold (for ABS-like effects) |
| 0x82 | short | Suspension rest height |
| 0x88 | short | Traffic suspension stiffness override |

---

## Per-Frame Update Pipeline

The master per-frame call chain for each vehicle actor:

```
UpdateVehicleActor (0x406650)               -- per-actor dispatcher
  |
  +-- ClampVehicleAttitudeLimits (0x405b40)  -- pitch/roll safety
  |
  +-- [if player] UpdatePlayerVehicleDynamics (0x404030)
  |   [if AI]     UpdateAIVehicleDynamics    (0x404ec0)
  |   [if 0x0f]   UpdateVehicleState0fDamping (0x403d90)
  |
  +-- IntegrateVehiclePoseAndContacts (0x405e80)
  |     |
  |     +-- BuildRotationMatrixFromAngles
  |     +-- UpdateActorTrackPosition
  |     +-- ComputeActorHeadingFromTrackSegment
  |     +-- RefreshVehicleWheelContactFrames (0x403720)
  |     +-- TransformTrackVertexByMatrix (A/B/C variants by contact pattern)
  |     +-- UpdateVehicleSuspensionResponse (0x4057f0)
  |
  +-- UpdateActorTrackSegmentContacts{Reverse,Forward,Both}
  |     +-- UpdateVehiclePoseFromPhysicsState (0x4063a0) [callback]
```

### Inside UpdatePlayerVehicleDynamics:
```
GetTrackSegmentSurfaceType x5           -- one per wheel + chassis
ComputeVehicleSurfaceNormalAndGravity   -- slope-based gravity decomposition
[compute per-wheel grip from surface tables]
[clamp grip to [0x38..0x50]]
[resolve body-frame velocities via cos/sin of heading]
[compute front/rear load transfer from suspension deflection]
ApplyReverseGearThrottleSign            -- flip throttle for reverse
UpdateAutomaticGearSelection            -- rpm-based up/downshift
UpdateEngineSpeedAccumulator            -- slew RPM toward target
ComputeDriveTorqueFromGearCurve         -- interpolated torque curve
[distribute drive torque by drivetrain type: FWD/RWD/AWD]
[compute per-axle lateral/longitudinal forces]
[enforce tire slip circle via sqrt]
[yaw torque from force imbalance]
ApplySteeringTorqueToWheels             -- optional direct steering force
IntegrateWheelSuspensionTravel          -- spring-damper per wheel
ApplyMissingWheelVelocityCorrection     -- asymmetric wheel compensation
```

### Inside UpdateAIVehicleDynamics:
```
GetTrackSegmentSurfaceType x1           -- chassis center only
ComputeVehicleSurfaceNormalAndGravity
[compute 2-axle grip, clamp [0x70..0xA0]]
[resolve body-frame velocities]
[front/rear load transfer]
UpdateAutomaticGearSelection
UpdateEngineSpeedAccumulator
ComputeDriveTorqueFromGearCurve
[2-axle force model -- no per-wheel split]
[slip detection for tire squeal]
[yaw torque computation]
IntegrateWheelSuspensionTravel
```

---

## Tire / Grip Model

### Surface Type Tables

Two lookup tables indexed by surface type byte (from track segment data):

- **`DAT_004748c0`** (short[]): Friction coefficient per surface type. Used to scale per-wheel grip.
- **`DAT_00474900`** (short[]): Drag/resistance per surface type. Applied as velocity-proportional damping.

### Player Grip Computation

For each of the 4 wheels:
```c
// Load transfer based on suspension deflection
front_load = (rear_weight << 8 / total) * (susp_travel - deflection) / susp_travel;
rear_load  = (front_weight << 8 / total) * (deflection + susp_travel) / susp_travel;

// Surface-scaled grip
grip[i] = surface_friction[surface_type[i]] * load[i];
grip[i] = (grip[i] + 128) >> 8;  // fixed-point multiply

// Clamp to hardcoded range
if (grip[i] < 0x38) grip[i] = 0x38;  // ~22%
if (grip[i] > 0x50) grip[i] = 0x50;  // ~31%

// Handbrake modifier on rear wheels
if (handbrake_active) {
    rear_grip *= tuning->rear_grip_modifier;
    rear_grip >>= 8;
}
```

### AI Grip Computation

Same structure but with 2 axles and wider clamp:
```c
// Same load transfer formula
front_load = ...;
rear_load  = ...;

// Clamp to AI range
if (grip < 0x70) grip = 0x70;  // ~44%
if (grip > 0xA0) grip = 0xA0;  // ~63%
```

The wider AI grip range is the primary mechanism that makes AI competitive despite having a simpler dynamics model.

### Tire Slip Circle (Player Only)

The player model enforces a combined slip limit per axle:
```c
// Compute combined force magnitude
lateral_16  = lateral_force >> 4;
longitudinal_16 = longitudinal_force >> 4;
combined = sqrt(lateral_16^2 + longitudinal_16^2) * 16;

// If combined exceeds grip limit, scale back
if (grip_limit < combined) {
    slip_excess = combined - grip_limit;  // stored for tire squeal SFX
    longitudinal_force = (grip_limit << 8) / combined * longitudinal_force >> 8;
}
```

This enforces the classic "friction circle" -- you can't brake and turn at full force simultaneously.

### Traffic Tire Model

Traffic uses hardcoded constants with no surface lookup:
```c
// Velocity damping: 16/4096 per frame
vel_x -= (vel_x * 0x10 + round) >> 12;
vel_z -= (vel_z * 0x10 + round) >> 12;

// Fixed grip constants
front_longitudinal = 0x271;  // 625/4096 ~= 0.153
rear_longitudinal  = 0x14C;  // 332/4096 ~= 0.081

// Force clamped to +/- 0x800 per axle
```

---

## Suspension System

### IntegrateWheelSuspensionTravel (0x403a20)

A spring-damper integrator that runs per wheel (4 iterations) plus one central roll/pitch pass. Parameters from tuning data:

| Tuning Offset | Name | Role |
|---------------|------|------|
| 0x62 | `spring_k` | Spring stiffness (resonance) |
| 0x5E | `damping` | Velocity damping factor |
| 0x60 | `damping2` | Additional position-proportional damping |
| 0x64 | `travel_limit` | Max deflection (clamped to +/- this value) |
| 0x66 | `contact_force_k` | Contact force multiplier |

Per-wheel integration (simplified pseudocode):
```c
for (i = 0; i < 4; i++) {
    // Compute target from wheel contact position relative to chassis
    target = (wheel_pos - chassis_pos) * param3 + (wheel_height - chassis_height) * param4;
    target = (target >> 8) * spring_k;

    // Add contact force and existing velocity
    accel = (target >> 8) + contact_force * contact_k >> 8 + velocity;

    // Apply damping
    accel -= (position * damping >> 8) + (accel * damping2 >> 8);

    // Dead zone
    if (abs(accel) < 16) accel = 0;

    velocity = accel;
    position += accel;

    // Clamp to travel limits
    if (position > travel_limit) { position = travel_limit; velocity = 0; }
    if (position < -travel_limit) { position = -travel_limit; velocity = 0; }
}

// Central chassis suspension (same formula, averaged from wheel positions)
// Stored at actor+0x2CC (position), actor+0x2D0 (velocity)
```

### UpdateVehicleSuspensionResponse (0x4057f0)

Aggregates per-wheel contact loads into chassis angular and vertical accelerations:

1. Transpose the body rotation matrix
2. For each grounded wheel (not in contact bitmask):
   - Convert wheel contact normal to body-frame angles
   - Accumulate gravity-projected forces for roll and pitch
3. For each wheel that was also grounded last frame:
   - Compute dot product of contact velocity with wheel position
   - Accumulate vertical bounce force
   - Play impact sound if bounce > 20 (SFX 0x17)
4. Apply averaged forces to angular velocities:
   - Roll: `actor+0x1C0 += (bounce_roll + gravity_roll / count) / 0x4B0`
   - Pitch: `actor+0x1C8 += (bounce_pitch + gravity_pitch / count) / 0x226`
   - Vertical: `actor+0x1D0 += bounce_average + gravity`
5. Clamp angular velocities to +/- 4000 based on contact pattern

### ApplyDampedSuspensionForce (0x4437c0) -- Traffic Only

Simple 2-DOF spring-damper for traffic vehicles:
```c
// Roll axis
position_roll += velocity_roll;
velocity_roll += (input_combined * 0x80 >> 8)     // drive force
               + velocity_roll * -0x20 >> 8        // velocity damping
               - position_roll * 0x20 >> 8;        // spring force
clamp(position_roll, -0x2000, 0x2000);

// Pitch axis (same, with higher limits +/- 0x4000)
```

---

## Engine & Transmission

### UpdateEngineSpeedAccumulator (0x42edf0)

Slews the engine RPM accumulator (`actor+0x310`) toward a target derived from longitudinal speed and current gear ratio:

```c
if (gear == 1) {  // neutral
    UpdateVehicleEngineSpeedSmoothed();  // idle/rev path
    return;
}

// Target RPM from speed and gear ratio
target = abs(longitudinal_speed >> 8) * gear_ratio[gear] * 0x2D;
target = (target >> 12) + 400;  // 400 = idle RPM

// Slew toward target with asymmetric rates
delta = current - target;
if (delta > 0x321) {
    current -= 200;           // fast downward slew
} else if (delta < -800) {
    current += 200;           // fast upward slew
} else {
    current += (target - current) / 4;  // smooth approach
}

// Cap at redline
if (current > tuning->redline) current = tuning->redline;
```

### UpdateVehicleEngineSpeedSmoothed (0x42ed50)

Handles neutral/reverse gear RPM -- smoothly approaches idle (400) or a throttle-proportional target:

```c
if (!braking && throttle >= 0) {
    target = (redline - 400) * throttle >> 8;
    target = min(target, redline) + 400;
} else {
    target = 400;  // idle
}
// Asymmetric slew: max +400/frame up, max +200/frame down, else /16 smooth
```

### UpdateAutomaticGearSelection (0x42ef10)

RPM-threshold based automatic transmission:

```c
if (throttle < 0) { gear = 0 (reverse); return; }
if (gear == 0 && throttle > 0) gear = 2;  // skip neutral

// Upshift: RPM > upshift_threshold[gear] AND speed > 0 AND gear < 8
if (engine_rpm > tuning->upshift[gear] && gear < 8 && longitudinal_speed > 0) {
    gear++;
    // Apply gear-change torque kick to all 4 wheel accumulators
    kick = throttle * tuning->steering_sensitivity * 0x1A >> 8;
    kick = kick * gear_torque_table[gear] >> 8;
    wheel_force[FL] += kick;
    wheel_force[FR] += kick;
    wheel_force[RL] -= kick;
    wheel_force[RR] -= kick;
}

// Downshift: RPM < downshift_threshold[gear] AND gear > 2
if (engine_rpm < tuning->downshift[gear] && gear > 2) {
    gear--;
}
```

The `gear_torque_table` at `DAT_00467394` provides per-gear torque multipliers for the shift kick.

### ComputeDriveTorqueFromGearCurve (0x42f030)

Interpolates a piecewise-linear torque curve stored in tuning data:

```c
if (gear == 1) return 0;  // neutral = no drive

// Sample torque curve (entries every 512 RPM units)
index = engine_rpm >> 9;
t0 = tuning->torque_curve[index] * tuning->steering_sensitivity >> 8;
t1 = tuning->torque_curve[index+1] * tuning->steering_sensitivity >> 8;

// Linear interpolation within segment
frac = engine_rpm & 0x1FF;
torque = t0 + (t1 - t0) * frac >> 9;

// Scale by throttle and gear ratio
torque = torque * throttle >> 8;
torque = torque * gear_ratio[gear] >> 8;

// Cut torque past redline
if (engine_rpm > redline - 50) return 0;

return torque;
```

### ApplyReverseGearThrottleSign (0x42f010)

Trivial helper: negates the throttle value when gear == 0 (reverse):
```c
if (gear == 0) throttle = -throttle;
```

### ComputeReverseGearTorque (0x403c80)

Computes engine RPM and drive torque specifically for the reverse gear path. Includes a speed-dependent target RPM with asymmetric slew rates (200 base, 400 when braking). Returns the reverse drive force scaled by gear ratio.

---

## Steering

### ApplySteeringTorqueToWheels (0x42eea0)

Adds a direct steering torque to the per-wheel force accumulators:

```c
force = throttle * tuning->steering_sensitivity * 0x1A >> 8;
force = force * gear_torque_table[current_gear] >> 8;

wheel_force[FL] += force;  // +0x2EC
wheel_force[FR] += force;  // +0x2F0
wheel_force[RL] -= force;  // +0x2F4
wheel_force[RR] -= force;  // +0x2F8
```

This creates a differential torque that induces yaw. Front wheels get positive, rear get negative, creating a turning moment.

### Steering in UpdatePlayerVehicleDynamics

The main steering computation within the player dynamics function:

1. Resolve velocity into body-frame (longitudinal/lateral) using `cos/sin` of heading angle
2. Compute steered-axle angle: `heading + steering_angle`
3. Compute front/rear axle lateral forces from slip angle (difference between velocity direction and wheel pointing direction)
4. Yaw torque = (front_lateral * front_arm - rear_lateral * rear_arm) / inertia
5. Yaw torque clamped to [-0x578, +0x578]
6. Yaw rate integrated: `actor+0x1C4 += yaw_torque`

---

## Integration & Pose Update

### IntegrateVehiclePoseAndContacts (0x405e80)

The core integration step that advances vehicle state by one tick:

1. **Apply gravity**: `force_Y -= gGravityConstant`
2. **Integrate velocities into angles/position**:
   ```c
   angle_X += force_roll;
   angle_Y += force_pitch;
   angle_Z += force_yaw;
   pos_X += force_X;
   pos_Y += force_Y;
   pos_Z += force_Z;
   ```
3. **Convert to display coordinates** (>>8 for angles, *0.00390625 for float position)
4. **Build rotation matrix** from Euler angles
5. **Update track position** (which segment the vehicle is on)
6. **Refresh wheel contact frames** via `RefreshVehicleWheelContactFrames`
7. **Resolve track-surface contact** based on wheel contact pattern:
   - Pattern 0,1,2,4,6,8,9: Full 3-axis alignment (`TransformTrackVertexByMatrix`)
   - Pattern 3,0xC: Pitch-only alignment (`TransformTrackVertexByMatrixC`)
   - Pattern 5,0xA: Roll-only alignment (`TransformTrackVertexByMatrixB`)
8. **Clamp angular velocity deltas** to +/- 6000 per frame
9. **Compute averaged ground height** from grounded wheels -> update vertical position
10. **Call `UpdateVehicleSuspensionResponse`** for suspension forces

### UpdateVehiclePoseFromPhysicsState (0x4063a0)

A lighter-weight pose refresh used as a callback during track segment contact resolution. Does NOT integrate forces -- just rebuilds the visual state from the current physics state:

1. Convert positions to float for render
2. Build rotation matrix from current angles
3. Update track position
4. Refresh wheel contacts
5. Apply track surface alignment (same switch as above)
6. Rebuild rotation matrix
7. Compute averaged ground height from grounded wheels

### RefreshVehicleWheelContactFrames (0x403720)

Builds per-wheel contact frames for the suspension/collision system:

For each of 4 wheels:
1. Copy chassis track position to wheel slot
2. Offset wheel by suspension deflection from tuning data
3. Transform wheel offset by body rotation matrix
4. Scale to world coordinates (<<8)
5. Call `UpdateActorTrackPosition` + `ComputeActorTrackContactNormalExtended`
6. Compute wheel vertical force: `force = (wheel_Y - ground_Y) + gravity`
7. Dead zone: if |force| < 0x200, set to 0
8. If force > 0x800: wheel is airborne, mark in bitmask, set force = 12000
9. Store wheel velocity (delta position from last frame)

Then for the 4 "ghost" contact probes (used for rendering):
- Same transform but uses `ComputeActorTrackContactNormal` (simpler variant)

---

## Surface Normal & Gravity

### ComputeVehicleSurfaceNormalAndGravity (0x42ebf0)

Computes the effective gravity vector projected onto the vehicle's body axes, accounting for the slope of the track surface under the vehicle:

```c
// Compute two perpendicular vectors from the body rotation matrix
// These represent the track surface tangent frame
vec1 = {rot[0]-rot[3]-rot[6]+rot[9], rot[1]-rot[4]-rot[7]+rot[10], rot[2]-rot[5]-rot[8]+rot[11]} >> 8;
vec2 = {rot[0]-rot[6]-rot[9]+rot[3], rot[1]-rot[7]-rot[10]+rot[4], rot[2]-rot[8]-rot[11]+rot[5]} >> 8;

// Normalize
StoreRoundedVector3Ints(&vec1);
StoreRoundedVector3Ints(&vec2);

// Cross product -> surface normal
normal = cross(vec1, vec2);  // 12-bit fixed point

// Project gravity onto body X and Z axes
vel_X += (gravity * normal.x / 2) >> 12;
vel_Z += (gravity * normal.z / 2) >> 12;
```

This causes the vehicle to accelerate downhill and decelerate uphill, with the effect proportional to the slope steepness.

---

## Attitude Clamping & Recovery

### ClampVehicleAttitudeLimits (0x405b40)

Prevents vehicles from flipping over by detecting excessive pitch/roll and either latching a recovery state or hard-clamping:

**Thresholds** (in 12-bit angle units, 0x800 = 180 degrees):
- Roll limit: +/- 0x355 (~150 degrees)
- Pitch limit: +/- 0x3A4 (~165 degrees)

**Mode 0 (default, `DAT_00463188 == 0`)**:
If any limit exceeded:
1. Save current velocity-minus-angular as a recovery basis
2. Build recovery rotation matrix
3. Copy current rotation -> saved rotation
4. Set recovery flag `actor+0x379 = 1`
5. Zero steering command

**Mode 1 (hard clamp)**:
- Soft nudge: if approaching limit, add +/- 0x200 correction to angular velocity
- Hard clamp: if past limit, zero angular velocity and snap angle to limit value

---

## Missing Wheel Correction

### ApplyMissingWheelVelocityCorrection (0x403eb0)

When the wheel contact bitmask indicates an asymmetric ground contact (e.g., 2 wheels on one side, 0 on the other), this function applies a corrective velocity bias to prevent unrealistic spinning:

```c
// Only runs for specific asymmetric patterns (not 0,1,2,4,6,8,9,15)
switch (contact_mask) {
    case 3,5,7,10,11,12,13,14: // asymmetric patterns
        // Average the Y-position of grounded wheels
        // Multiply by chassis pitch angle
        // Compute body-frame longitudinal speed
        // Clamp to [-0x200, 0x200]
        // Subtract corrective bias from pitch angular velocity
}
```

---

## Vehicle Initialization

### InitializeRaceVehicleRuntime (0x42f140)

Sets up all vehicle actors before race start:

1. **Zero the entire actor struct** (0x388 bytes, word-by-word)
2. **Bind tuning + physics data pointers** from `gVehicleTuningTable` and `gVehiclePhysicsTable`
3. **Copy per-gear suspension data** from tuning to actor runtime
4. **For racers (slots 0-5)**: Call `IntegrateVehiclePoseAndContacts` to establish initial pose, determine max gear from gear ratio table
5. **For traffic (slots 6+)**: Set suspension stiffness override, call `UpdateTrafficVehiclePose`
6. **Apply difficulty scaling** (see below)
7. **Per-player tuning adjustments**: Based on `DAT_004aed70` (per-slot difficulty modifiers), scales brake front/rear, gear ratios, speed limiter, drag, and grip

### ResetVehicleActorState (0x405d70)

Resets a vehicle to initial conditions (used on respawn/reset):
- Zero all velocities, forces, angular rates
- Set gear to 2 (first forward gear)
- Set engine RPM to 400 (idle)
- Clear wheel contact data and suspension state
- Convert positions to float for render
- Call `IntegrateVehiclePoseAndContacts` to re-establish ground contact

### ComputeVehicleSuspensionEnvelope (0x42f6d0)

Computes the axis-aligned bounding box of a vehicle's mesh for use as the suspension/collision envelope:

1. Iterate all vertices in the car mesh
2. Track min/max for each axis
3. Store as 8 corner points (AABB) in world-space short coordinates
4. For traffic vehicles (slot > 5): also stores simplified 4-point footprint

---

## Difficulty Scaling

Applied in `InitializeRaceVehicleRuntime` after all vehicles are initialized:

### Easy (`gDifficultyEasy != 0`)
- `gGravityConstant = 0x5DC` (1500) -- reduced gravity
- No tuning modifications

### Normal (default)
- `gGravityConstant = 0x76C` (1900)
- Steering sensitivity: `*= 0x168 >> 8` (~89%)
- Brake balance: `*= 2`
- Weight: `*= 300 >> 8` (~117%)

### Hard (`gDifficultyHard != 0`)
- `gGravityConstant = 0x800` (2048) -- maximum gravity
- Steering sensitivity: `*= 0x28A >> 8` (~255%, more than doubled)
- Brake balance: `*= 4`
- Weight: `*= 0x17C >> 8` (~148%)
- Brake front: `*= 0x1C2 >> 8` (~177%)
- Brake rear: `*= 400 >> 8` (~156%)

### Per-Player Adjustments

After global difficulty, per-player modifiers from `DAT_004aed70` (4 bytes per slot, up to 6 slots) are applied:
- Brake front/rear scaling
- Per-gear torque curve scaling (iterates gear-2 through max gear)
- Speed limiter scaling
- Top-gear ratio scaling
- Drag coefficient scaling
- Grip base scaling

---

## UpdateVehicleState0fDamping (0x403d90)

A special damping path activated when the vehicle sub-state byte (`actor+0x376[6]`) is set to 0x0F (triggered by input bit 0x200000 in `UpdatePlayerVehicleControlState`). This appears to be a "stunned" or "recovery" state:

1. Call `UpdateVehicleEngineSpeedSmoothed` (keep engine alive)
2. Call `IntegrateWheelSuspensionTravel` with zero lateral/longitudinal input
3. Zero all force outputs and tire screech flag
4. Compute body-frame longitudinal speed using heading cos/sin
5. If speed is low (<33) and pitch angle is small (<127 units): apply 1/4 of speed to yaw rate
6. Decay both angular velocities by 1/16 per frame
7. Accumulate slip counters for tire sound

---

## UpdateVehicleActor (0x406650) -- Master Dispatcher

The top-level per-vehicle-per-frame function. Flow:

1. Increment frame counter at `actor+0x338`
2. Clear per-frame flag
3. **Time trial ghost**: if time trial mode and ghost flag set, force zero throttle + max brake
4. **Speed tracking**: accumulate absolute speed into `actor+0x32C`, track peak speed
5. **Speed bonus**: if race rule variant 4 (speed trap?), accumulate speed bonus
6. **Finish state**: advance pending finish state
7. **Attitude clamp**: call `ClampVehicleAttitudeLimits` (unless in scripted mode)
8. **Dynamics dispatch** (mode == 0, normal):
   - If `DAT_004aad60 == 0` (not paused):
     - Select min of grip levels [10] and [13]
     - If sub-state == 0x0F and timer >= 3: `UpdateVehicleState0fDamping`
     - Else if player: `UpdatePlayerVehicleDynamics`
     - Else: `UpdateAIVehicleDynamics`
   - If paused: only update engine RPM, set tire screech from speed
9. **Pose integration**: `IntegrateVehiclePoseAndContacts`
10. **Track contact resolution**: forward, reverse, and combined segment contact callbacks
11. **Scripted mode** (mode == 1): use scripted transforms, skip dynamics

---

## Function Reference Table

| Address | Name | Lines | Tier |
|---------|------|-------|------|
| 0x403720 | `RefreshVehicleWheelContactFrames` | ~95 | All |
| 0x403a20 | `IntegrateWheelSuspensionTravel` | ~75 | Player/AI |
| 0x403c80 | `ComputeReverseGearTorque` | ~50 | Player/AI |
| 0x403d90 | `UpdateVehicleState0fDamping` | ~45 | Player |
| 0x403eb0 | `ApplyMissingWheelVelocityCorrection` | ~55 | Player |
| 0x404030 | `UpdatePlayerVehicleDynamics` | ~480 | Player |
| 0x404ec0 | `UpdateAIVehicleDynamics` | ~260 | AI |
| 0x4057f0 | `UpdateVehicleSuspensionResponse` | ~130 | All |
| 0x405b40 | `ClampVehicleAttitudeLimits` | ~75 | All |
| 0x405d70 | `ResetVehicleActorState` | ~60 | All |
| 0x405e80 | `IntegrateVehiclePoseAndContacts` | ~145 | All |
| 0x4063a0 | `UpdateVehiclePoseFromPhysicsState` | ~80 | All |
| 0x406650 | `UpdateVehicleActor` | ~170 | All |
| 0x42ebf0 | `ComputeVehicleSurfaceNormalAndGravity` | ~30 | Player/AI |
| 0x42ed50 | `UpdateVehicleEngineSpeedSmoothed` | ~30 | All |
| 0x42edf0 | `UpdateEngineSpeedAccumulator` | ~35 | Player/AI |
| 0x42eea0 | `ApplySteeringTorqueToWheels` | ~12 | Player/AI |
| 0x42ef10 | `UpdateAutomaticGearSelection` | ~40 | Player/AI |
| 0x42f010 | `ApplyReverseGearThrottleSign` | ~5 | Player/AI |
| 0x42f030 | `ComputeDriveTorqueFromGearCurve` | ~30 | Player/AI |
| 0x42f140 | `InitializeRaceVehicleRuntime` | ~170 | Init |
| 0x42f6d0 | `ComputeVehicleSuspensionEnvelope` | ~90 | Init |
| 0x4437c0 | `ApplyDampedSuspensionForce` | ~35 | Traffic |
| 0x4438f0 | `IntegrateVehicleFrictionForces` | ~100 | Traffic |

---

## Key Observations

1. **No floating-point in physics**: Everything is integer fixed-point (8.8, 12.12). Float only used for render matrix and bounding volume.

2. **Grip asymmetry is the core AI balance mechanism**: AI gets [0x70,0xA0] grip vs player's [0x38,0x50]. This 2x grip advantage compensates for the AI's simpler 2-axle model with only 1 surface probe.

3. **Traffic is truly minimal**: Fixed constants, no surface awareness, simple spring-damper. Exists only to be visually plausible background dressing.

4. **The tire slip circle is player-only**: AI doesn't have combined slip limits, making their cornering behavior less realistic but more predictable.

5. **Difficulty primarily scales steering/brakes, not speed**: Hard mode doubles steering sensitivity and quadruples brake balance, making vehicles more twitchy rather than faster.

6. **Gravity scales with difficulty**: Easy=1500, Normal=1900, Hard=2048. Lower gravity means more forgiving air time and less aggressive downhill acceleration.

7. **Drivetrain types are tuned per-car**: FWD/RWD/AWD from tuning data. AWD splits torque equally to all 4 wheels (each gets 1/4).

8. **State 0x0F is a "stunned" state**: Triggered by input bit 0x200000, zeros all drive forces and lets the vehicle coast to a stop with 1/16 per-frame velocity decay.

9. **Track segment contact resolution runs the pose callback multiple times**: `UpdateVehiclePoseFromPhysicsState` is called as a callback from forward/reverse/combined track segment walkers, ensuring the vehicle properly "slides" along track geometry at segment boundaries.
