# Vehicle Dynamics: Suspension Cross-Coupling, Weight Transfer, Yaw Torque, and Tire Model

**Date:** 2026-03-19
**Binary:** TD5_d3d.exe (Ghidra port 8193)
**Scope:** Complete decompilation-derived formulas for all four dynamics subsystems
**Depends on:** carparam-physics-table.md, data-structure-gaps-filled.md

---

## Actor Struct Field Reference (dynamics-relevant offsets)

All offsets below are byte offsets into the 0x388-byte actor struct at `gActorRuntimeState` (0x4AB108).

### Orientation & Position (float 3x3 matrix + world coords)
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x090-0x0B7 | 36B | `rotation_matrix[9]` | 3x3 float rotation matrix (row-major) |
| 0x0F8 | 4 | `angle_x` | Euler X angle (int, 8.8 fixed -> short = angle/256) |
| 0x0FA | 4 | `angle_y` | Euler Y angle (heading) |
| 0x0FC | 4 | `angle_z` | Euler Z angle |
| 0x0FE | 4 | `pos_x` | World X position (int) |
| 0x100 | 4 | `pos_y` | World Y position (vertical) |
| 0x102 | 4 | `pos_z` | World Z position |

### Velocities (all int32, fixed-point)
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x1C0 | 4 | `angular_vel_x` | Roll angular velocity |
| 0x1C4 | 4 | `angular_vel_y` | Yaw angular velocity |
| 0x1C8 | 4 | `angular_vel_z` | Pitch angular velocity |
| 0x1CC | 4 | `linear_vel_x` | World-space X velocity |
| 0x1D0 | 4 | `linear_vel_y` | World-space Y velocity (vertical) |
| 0x1D4 | 4 | `linear_vel_z` | World-space Z velocity |
| 0x1E0-0x1E8 | 12 | `angular_integrators` | Low-freq angular accumulators (via ApplyDampedSuspensionForce) |

### Force Accumulators (per-tick deltas, written by dynamics functions)
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x1E0 | 4 | `roll_accumulator` | Damped roll accumulator (via friction/suspension) |
| 0x1E4 | 4 | `pitch_accumulator` | Damped pitch accumulator |
| 0x1EC | 4 | `roll_velocity` | Roll velocity in damped system |
| 0x1F0 | 4 | `pitch_velocity` | Pitch velocity in damped system |

### Chassis Dynamics State
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x1C4 | 4 | `yaw_rate` | Current yaw angular velocity |
| 0x1CC | 4 | `world_vel_fwd` | Forward velocity (world X) |
| 0x1D4 | 4 | `world_vel_lat` | Lateral velocity (world Z) |
| 0x1F4 | 4 | `steering_angle` | Current steering angle (int, 8.8 fixed) |

### Wheel Subsystem
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x208-0x20F | 8 | `euler_short[3]` | Short Euler angles (12-bit wrapped) |
| 0x298-0x2C7 | 48 | `wheel_positions[4]` | Per-wheel world positions (4 x {x,y,z} ints, 3*4=12B each) |
| 0x2CC | 4 | `chassis_center_displacement` | Central suspension displacement (spring-damper) |
| 0x2D0 | 4 | `chassis_center_velocity` | Central suspension velocity |
| 0x2DC-0x2EB | 16 | `wheel_displacement[4]` | Per-wheel suspension displacement (int each) |
| 0x2EC-0x2FB | 16 | `wheel_velocity[4]` | Per-wheel suspension velocity (int each) |

### Wheel Contact State
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x37C | 1 | `wheel_contact_mask` | Bitmask: bit N=1 means wheel N is airborne/missing |
| 0x37D | 1 | `suspension_response_mask` | Previous-frame contact mask for response clamping |

### Physics/Tuning Pointers
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x1B8 | 4 | `tuning_ptr` | Pointer to tuning table (carparam.dat 0x00-0x8B) |
| 0x1BC | 4 | `physics_ptr` | Pointer to physics table (carparam.dat 0x8C-0x10B) |

### Drivetrain State
| Offset | Size | Name | Description |
|--------|------|------|-------------|
| 0x310 | 4 | `engine_speed` | Engine speed accumulator (0-max_rpm) |
| 0x314 | 4 | `longitudinal_speed` | Body-frame forward speed (transformed from world vel) |
| 0x318 | 4 | `lateral_body_speed` | Body-frame lateral speed |
| 0x30C | 4 | `steering_angle_accum` | Steering angle accumulator |
| 0x33E | 2 | `throttle_command` | Signed throttle input (-256 to +256) |
| 0x36B | 1 | `current_gear` | Gear index: 0=reverse, 1=neutral, 2-7=gears 1-6 |
| 0x36D | 1 | `braking_flag` | Non-zero = braking active |
| 0x376 | 1 | `drivetrain_flags` | Bitfield for traction/spin state |

---

## 1. Suspension Cross-Coupling

### Function: `IntegrateWheelSuspensionTravel` (0x403A20)

**Signature:** `void __cdecl IntegrateWheelSuspensionTravel(int actor, int physics_ptr, int fwd_force, int lat_force)`

This is a **5-node spring-damper system**: 4 independent wheel nodes + 1 central chassis node. There is **NO anti-roll bar** -- the wheels are coupled only through the chassis body's angular response computed in `UpdateVehicleSuspensionResponse`, not through direct wheel-to-wheel linkage.

#### Physics Table Fields Used

| Phys Offset | Variable | Role |
|-------------|----------|------|
| +0x5E | `damping` (sVar2) | Displacement-proportional damping coefficient |
| +0x60 | `spring` (sVar3) | Velocity-proportional spring coefficient |
| +0x62 | `feedback` (sVar1) | Cross-axis force-to-travel coupling gain |
| +0x64 | `travel_limit` (iVar7) | Symmetric displacement clamp (+/-) |
| +0x66 | `response` (sVar4) | Previous-velocity feedback gain |

#### Per-Wheel Spring-Damper Formula (executed 4 times)

For each wheel `i` (0=FL, 1=FR, 2=RL, 3=RR):

```
Input:
  wheel_sample_fwd[i] = wheel_positions[i].x - (actor.pos_x >> 8)
  wheel_sample_lat[i] = wheel_positions[i].z - (actor.pos_z >> 8)
  cross_term = wheel_sample_fwd[i] * fwd_force + wheel_sample_lat[i] * lat_force

Step 1 -- Force coupling:
  force_input = (cross_term >> 8) * feedback >> 8

Step 2 -- Previous velocity feedback:
  vel_feedback = wheel_velocity[i] * response >> 8

Step 3 -- Accumulate:
  raw_velocity = force_input + vel_feedback + wheel_velocity[i]

Step 4 -- Damping (position-proportional):
  damp_force = wheel_displacement[i] * damping >> 8

Step 5 -- Spring (velocity-proportional):
  spring_force = raw_velocity * spring >> 8

Step 6 -- Net velocity:
  new_velocity = raw_velocity - damp_force - spring_force

Step 7 -- Deadband:
  if (abs(new_velocity) < 0x10) new_velocity = 0

Step 8 -- Integrate:
  wheel_velocity[i] = new_velocity
  wheel_displacement[i] += new_velocity

Step 9 -- Clamp (bottoming out):
  if (wheel_displacement[i] > +travel_limit) { displacement = +travel_limit; velocity = 0; }
  if (wheel_displacement[i] < -travel_limit) { displacement = -travel_limit; velocity = 0; }
```

#### Central Chassis Node (roll/pitch accumulator at +0x2CC/+0x2D0)

After the 4 wheels, a 5th integration pass runs for the chassis center using the **average** of front and rear axle heights:

```
avg_fwd = (wheel_pos[FL].x + wheel_pos[RL].x) / 2 - (actor.pos_x >> 8)
avg_lat = (wheel_pos[FL].z + wheel_pos[FR].z) / 2 - (actor.pos_z >> 8)
center_cross = avg_fwd * fwd_force + avg_lat * lat_force

(Same spring-damper integration as per-wheel, using chassis_center_displacement/velocity)
```

This central node represents the body's average suspension height. It does NOT feed back into individual wheels -- the coupling is one-directional (chassis forces -> wheel responses are independent).

#### Spring/Damper Model Classification

This is a **linear Hooke's law spring-damper** with:
- **Damping**: proportional to displacement (position damping, NOT velocity damping) -- unusual but effective for preventing oscillation in fixed-point arithmetic
- **Spring**: proportional to velocity (velocity spring, NOT displacement spring) -- acts as a velocity-decay integrator
- **No progressive springs**: all coefficients are constant (linear)
- **Hard stops**: symmetric travel limit with zero-velocity clamp (perfectly inelastic bottoming)
- **Deadband**: velocities below 0x10 (~0.06 in 8.8 fixed) are zeroed to prevent jitter

The naming convention in the physics table is somewhat inverted from classical mechanics terminology:
- `damping` (+0x5E) attenuates based on **position** (like a position-proportional controller)
- `spring` (+0x60) attenuates based on **velocity** (like a velocity-proportional controller / viscous damper)

---

## 2. Weight Transfer

### Function: `UpdatePlayerVehicleDynamics` (0x404030)

Weight transfer is computed per-wheel as a **static load distribution** modified by longitudinal weight shift from the steering offset (effectively a simplified CG-shift model). There is **no dynamic weight transfer** from acceleration/braking forces -- the system uses only geometric weight distribution.

#### Physics Table Fields Used

| Phys Offset | Variable | Role |
|-------------|----------|------|
| +0x20 | `inertia` | Yaw moment of inertia |
| +0x24 | `half_wheelbase` | CG-to-axle distance (half the total wheelbase) |
| +0x28 | `front_weight` | Front axle weight distribution numerator |
| +0x2A | `rear_weight` | Rear axle weight distribution numerator |
| +0x2C | `drag_coeff` | Aerodynamic drag coefficient |

#### Weight Transfer Formula

```c
// Setup
total_weight = front_weight + rear_weight;
full_wheelbase = half_wheelbase * 2;
steering_offset = actor.chassis_center_displacement;  // actor+0x2CC (suspension CG shift)

// Front axle load fraction (both front wheels share this)
front_load = ((rear_weight << 8) / total_weight)
           * (half_wheelbase - steering_offset) / full_wheelbase;

// Rear axle load fraction (both rear wheels share this)
rear_load = ((front_weight << 8) / total_weight)
          * (steering_offset + half_wheelbase) / full_wheelbase;
```

**Key insight:** The `steering_offset` here is `actor+0x166` (= actor+0x2CC in int offset), which is the chassis center suspension displacement. When the car pitches forward (braking), the suspension compresses the front and this value shifts forward, increasing `rear_load` and decreasing `front_load`. This provides **indirect** longitudinal weight transfer through the suspension geometry, not through explicit F=ma force computation.

#### Per-Wheel Grip Calculation

```c
// Surface friction lookup: DAT_004748c0[surface_type] (short, 0x100 = 1.0)
surface_grip_FL = surface_friction_table[surface_FL];
surface_grip_FR = surface_friction_table[surface_FR];
surface_grip_RL = surface_friction_table[surface_RL];
surface_grip_RR = surface_friction_table[surface_RR];

// Per-wheel grip = surface_friction * load_fraction / 256
grip_FL = surface_grip_FL * front_load >> 8;   // clamped [0x38, 0x50]
grip_FR = surface_grip_FR * front_load >> 8;   // clamped [0x38, 0x50]
grip_RL = surface_grip_RL * rear_load >> 8;    // clamped [0x38, 0x50]
grip_RR = surface_grip_RR * rear_load >> 8;    // clamped [0x38, 0x50]

// Handbrake: reduces rear grip
if (handbrake_active) {
    grip_RL = grip_RL * handbrake_grip_modifier >> 8;  // phys+0x7A, typically <256
    grip_RR = grip_RR * handbrake_grip_modifier >> 8;
}
```

#### Surface Friction Table (0x4748C0)

16-entry `short[]` table indexed by surface type. Values in 8.8 fixed-point (0x100 = 1.0):

| Index | Hex | Decimal | Fraction | Surface |
|-------|-----|---------|----------|---------|
| 0 | 0x0100 | 256 | 1.000 | Asphalt (standard) |
| 1 | 0x0100 | 256 | 1.000 | Asphalt (variant) |
| 2 | 0x00DC | 220 | 0.859 | Concrete |
| 3 | 0x00F0 | 240 | 0.938 | Bridge surface |
| 4 | 0x00FC | 252 | 0.984 | Highway |
| 5 | 0x00C0 | 192 | 0.750 | Gravel |
| 6 | 0x00B4 | 180 | 0.703 | Grass |
| 7 | 0x0100 | 256 | 1.000 | Tunnel |
| 8 | 0x0100 | 256 | 1.000 | Reserved |
| 9 | 0x0100 | 256 | 1.000 | Reserved |
| 10 | 0x00C8 | 200 | 0.781 | Dirt |
| 11+ | 0x0100 | 256 | 1.000 | Default |

#### Surface Damping Table (0x474900)

16-entry `short[]` table indexed by surface type. Additive velocity damping (in 8.8 fixed):

| Index | Hex | Decimal | Surface | Effect |
|-------|-----|---------|---------|--------|
| 0 | 0x0000 | 0 | Asphalt | No extra damping |
| 1 | 0x0000 | 0 | Asphalt variant | No extra damping |
| 2 | 0x0000 | 0 | Concrete | No extra damping |
| 3 | 0x0000 | 0 | Bridge | No extra damping |
| 4 | 0x0000 | 0 | Highway | No extra damping |
| 5 | 0x0002 | 2 | Gravel | Slight deceleration |
| 6 | 0x0000 | 0 | Grass | No extra damping |
| 7 | 0x0000 | 0 | Tunnel | No extra damping |
| 8 | 0x0000 | 0 | Reserved | No extra damping |
| 9 | 0x0000 | 0 | Reserved | No extra damping |
| 10 | 0x0008 | 8 | Dirt | Significant deceleration |
| 11+ | 0x0000 | 0 | Default | No extra damping |

#### Grip Clamp Ranges

| Vehicle Type | Min Grip | Max Grip | Notes |
|-------------|----------|----------|-------|
| Player | 0x38 (56) | 0x50 (80) | Tight range, keeps handling consistent |
| AI | 0x70 (112) | 0xA0 (160) | Much wider range, allows more extreme behavior |

This 2x difference in grip range between player and AI is intentional game balance -- AI vehicles have higher base grip to compensate for simpler steering logic.

#### Lateral Weight Transfer (Cornering)

There is **NO explicit lateral weight transfer computation**. Cornering load shift is modeled implicitly:
1. The suspension displacement (+0x2CC) shifts under cornering forces from `UpdateVehicleSuspensionResponse`
2. This shift feeds into the front/rear load split via `steering_offset`
3. The individual wheel grips are further modulated by the slip circle (see Section 4)

This means the game uses a **1D front/rear weight distribution** model, not a full 4-corner weight model.

---

## 3. Yaw Torque

### Source: `UpdatePlayerVehicleDynamics` (0x404030) lines computing `iVar28` (yaw delta)

#### Yaw Torque Generation

Yaw torque comes from **three sources**:
1. **Tire lateral force differential** between front and rear axles
2. **Longitudinal force coupling** through the CG offset
3. **Steering geometry** (front wheel angle creating moment about CG)

#### Complete Yaw Torque Formula

```c
// Coordinate frame transform: world velocity -> body frame
// Uses CosFixed12bit/SinFixed12bit with steering angle (steer_angle = actor+0x1F4 >> 8)

cos_steer = CosFixed12bit(steer_angle);
sin_steer = SinFixed12bit(steer_angle);
cos_combined = CosFixed12bit(steer_angle + front_steer_offset);
sin_combined = SinFixed12bit(steer_angle + front_steer_offset);
cos_offset = CosFixed12bit(front_steer_offset);
sin_offset = SinFixed12bit(front_steer_offset);

// Transform world velocity to body frame
fwd_speed = (cos_steer * world_vel_x - sin_steer * world_vel_z) >> 12;  // longitudinal
lat_speed = (cos_steer * world_vel_z + sin_steer * world_vel_x) >> 12;  // lateral

// Front axle lateral velocity (with steering angle)
front_lat = (cos_combined * world_vel_z + sin_combined * world_vel_x) >> 12
          - (sin_offset * front_weight * yaw_rate) / 0x28C >> 12;

// Per-axle force computation (after drivetrain torque applied):
// front_force_fwd, front_force_lat = sum of FL+FR drive/brake forces
// rear_force_fwd, rear_force_lat = sum of RL+RR drive/brake forces

// Yaw torque from bicycle model:
yaw_numerator = (cos_offset * front_weight >> 12) * rear_force_lat
              + ((rear_drive_diff - front_drive_diff - front_grip + rear_grip) * 500)
              - front_force_fwd * rear_weight;

yaw_torque = yaw_numerator / (inertia / 0x28C);

// Clamp: [-0x578, +0x578]  (= [-1400, +1400] in fixed-point)
if (yaw_torque > 0x578) yaw_torque = 0x578;
if (yaw_torque < -0x578) yaw_torque = -0x578;
```

#### Yaw Integration

```c
actor.yaw_rate += yaw_torque;

// Deadband: if all 3 angular velocities are within [-0x40, +0x40] and yaw < [-0x20, +0x20]
// then zero everything (settling to rest)
if (abs(angular_vel_x) < 0x40 && abs(angular_vel_z) < 0x40 && abs(yaw_rate) < 0x20) {
    angular_vel_x = angular_vel_z = yaw_rate = 0;
}
```

#### Yaw Damping

Yaw is damped through **two mechanisms**:

1. **Velocity damping** (same formula as forward/lateral):
   ```c
   damping_coeff = surface_damp_table[surface] * 256 + (low_speed ? damping_low : damping_high);
   yaw_rate -= (yaw_rate / 256) * damping_coeff / 4096;
   ```
   This is NOT explicit in the player dynamics path. Instead, yaw damping comes from:

2. **Oversteer correction via excess-force feedback**:
   ```c
   // When combined tire force exceeds grip budget:
   excess_force = sqrt(fwd_force_sq + lat_force_sq) * 16;
   if (grip_budget < excess_force) {
       overshoot = excess_force - grip_budget;
       // Scale yaw torque proportionally to grip fraction
       yaw_torque = (grip_budget << 8) / excess_force * yaw_torque >> 8;
   }
   ```
   This slip-circle check (see Section 4) naturally attenuates yaw torque when tires exceed their grip limit.

3. **State-0x0F emergency damping** (`UpdateVehicleState0fDamping` 0x403D90):
   When the vehicle enters the 0x0F damage/spin state:
   ```c
   angular_vel_x -= angular_vel_x >> 4;   // 6.25% decay per tick
   angular_vel_z -= angular_vel_z >> 4;   // 6.25% decay per tick
   // Plus lateral-velocity-dependent yaw correction clamped to [-0x200, +0x200]
   ```

4. **Attitude clamp** (`ClampVehicleAttitudeLimits` 0x405B40):
   Hard limits on pitch/roll prevent runaway angular velocity:
   - Roll (angle_x): recovery kicks in at |angle| > 0x27F (~58 deg), hard clamp at 0x355 (~77 deg)
   - Pitch (angle_z): recovery at |angle| > 0x2BC (~63 deg), hard clamp at 0x3A4 (~84 deg)
   - At the hard clamp, angular velocity is zeroed and angle is locked

#### Yaw Torque Components Breakdown

| Source | Magnitude | Description |
|--------|-----------|-------------|
| Front lateral force * rear moment arm | Dominant | `cos_steer * rear_weight * front_lat_force / inertia` |
| Drive force differential * 500 | Moderate | Asymmetric L/R drive force (e.g., one wheel on grass) |
| Forward force * CG offset | Moderate | Longitudinal force creating moment about CG |
| Inertia divisor: `inertia / 0x28C` | Scaling | 0x28C = 652 -- converts inertia to yaw-torque-compatible scale |
| Hard clamp: +/- 0x578 (1400) | Safety | Prevents numerical explosion |

---

## 4. Tire Model

### Classification: **Linear Slip Circle (Friction Circle)**

The tire model is a **linear grip model with a circular saturation limit**. There is no Pacejka "Magic Formula", no lookup tables for slip curves. The model is:

1. Linear force generation below grip limit
2. Proportional force scaling when the friction circle is exceeded
3. Surface-dependent grip multiplier

### Slip Angle Computation

Slip angle is NOT computed explicitly as `atan(v_lat / v_fwd)`. Instead, the game uses a **velocity-based lateral force** that implicitly encodes slip:

```c
// Body-frame velocity decomposition (per axle):
// Front axle: includes steering angle rotation
front_fwd = cos(steer+offset) * world_vel_z + sin(steer+offset) * world_vel_x >> 12;
front_lat = cos(steer+offset) * world_vel_x - sin(steer+offset) * world_vel_z >> 12;

// Rear axle: body frame only (no steering)
rear_fwd = cos(steer) * world_vel_x - sin(steer) * world_vel_z >> 12;
rear_lat = cos(steer) * world_vel_z + sin(steer) * world_vel_x >> 12;

// The lateral velocity IS the implicit slip -- higher lat velocity = higher slip angle
// No atan() needed because forces are directly proportional to velocities
```

### Friction Circle (Slip Circle) Implementation

For each axle (front and rear separately):

```c
// Compute maximum available grip from weight and surface
grip_budget = (grip_FL + grip_FR) * drag_coeff >> 8;   // front axle
// OR
grip_budget = (grip_RL + grip_RR) * drag_coeff >> 8;   // rear axle

// Check if tire is sliding (drivetrain_flags bit test)
if (is_sliding) {
    // Compute actual speed beyond road speed
    speed_excess = abs(wheel_speed - road_speed);
    slip_magnitude = (speed_excess >> 8);

    if (slip_magnitude < 0x41) {
        // Below slip threshold: disable sliding flag
        clear_sliding_flag();
    }

    // Lateral slip component from lateral_slip_stiffness (phys+0x7C)
    lat_slip = slip_magnitude * lateral_slip_stiffness >> 8;

    // Combined slip (friction ellipse approximation):
    fwd_component = abs(fwd_force) >> 8;
    combined_force = sqrt(fwd_component * fwd_force / 256 + lat_slip * lat_slip);

    // Scale grip budget by normalized forward force direction
    grip_budget = abs(fwd_force / 256) * grip_budget / combined_force;

    // Generate yaw-inducing lateral force from slip
    lat_force_contribution = (axle_lat_sum / 2) * lat_slip / combined_force;
}

// Friction circle saturation check
actual_force = sqrt((lat_force/16)^2 + (fwd_force/16)^2) * 16;

if (actual_force > grip_budget) {
    // Force exceeds grip: proportionally scale down
    excess = actual_force - grip_budget;
    scale = (grip_budget << 8) / actual_force;
    fwd_force = scale * fwd_force >> 8;
    // Record excess for sound/visual effects
    actor.excess_force = excess;
}
```

### `lateral_slip_stiffness` (phys+0x7C) -- Role Confirmed

This field controls how quickly lateral forces build during a slide:

```c
lat_slip = slip_speed * lateral_slip_stiffness >> 8;
```

- **Higher values** = faster lateral force buildup = snappier oversteer response
- **Lower values** = more gradual slide onset = more forgiving handling
- Typical range: appears to be 0x40-0x100 (0.25-1.0 in 8.8 fixed)
- Only active when the `is_sliding` flag is set (drivetrain_flags bit 1 or 2)

### Surface Type Effect on Grip

Surface type affects grip through **two independent tables**:

1. **Friction multiplier** (0x4748C0): Scales per-wheel grip budget
   - Applied multiplicatively: `grip = base_load * surface_friction >> 8`
   - Grass (0xB4/256 = 70.3%) and gravel (0xC0/256 = 75%) have the largest reductions

2. **Velocity damping** (0x474900): Additional per-frame speed decay
   - Applied as: `speed -= (speed/256) * (surface_damp * 256 + base_damp) / 4096`
   - Only dirt (0x08) and gravel (0x02) have non-zero values
   - This slows the car independently of grip -- simulates rolling resistance

### Traction Control (Automatic Grip Recovery)

```c
// In UpdateVehicleActor: when stopped (DAT_004aad60 != 0)
if (player_controlled && engine_speed > max_rpm * 3/4) {
    actor.drivetrain_flags = drivetrain_type;  // Enable traction flag
}

// In UpdatePlayerVehicleDynamics: top speed limiter
if (top_speed_limit * 256 < longitudinal_speed) {
    all_four_drive_forces = 0;  // Hard cutoff, no gradual reduction
}
```

---

## 5. `UpdateVehicleSuspensionResponse` (0x4057F0) -- Chassis Angular Forces

This function converts per-wheel ground contact loads into chassis roll and pitch angular accelerations.

### Contact Pattern Switching

The function uses the `wheel_contact_mask` to determine which wheels are grounded, then applies per-wheel forces only for grounded wheels:

```c
if (wheel_contact_mask == 0x0F) return;  // All 4 wheels airborne, skip entirely

// For each grounded wheel:
if (!(mask & (1 << wheel_idx))) {
    // Compute contact normal slope
    ConvertFloatVec3ToShortAngles(wheel_contact_vec, &angles);
    slope_y = angles.y;

    // Gravity-weighted contact force
    contact_force = (slope_y * gGravityConstant) >> 12;

    // Accumulate into chassis torques
    pitch_sum += contact_force * wheel_x_offset;  // front/rear weight
    roll_sum -= contact_force * wheel_z_offset;   // left/right weight
    grounded_count++;

    // If suspension response mask confirms good contact:
    if (response_mask & (1 << wheel_idx)) {
        // Full contact force with friction
        friction_vec = dot(contact_normal, surface_normal) * suspension_height;
        surface_force = friction_vec >> 12;
        pitch_force_sum += surface_force * wheel_x_offset * -256;
        roll_force_sum += surface_force * wheel_z_offset * 256;
        surface_count++;
        half_normal += surface_force / 2;
    }
}

// Average forces over contacted wheels
if (surface_count > 0) {
    pitch_force = pitch_force_sum / surface_count;
    half_normal = half_normal / surface_count;
    roll_force = roll_force_sum / surface_count;
}
```

### Angular Velocity Update

```c
// Pitch: divisor 0x226 (550)
pitch_rate += (roll_force_sum + pitch_sum / grounded_count) / 0x226;

// Roll: divisor 0x4B0 (1200)
roll_rate += (pitch_force_sum + roll_sum / grounded_count) / 0x4B0;

// Vertical: gravity + surface normal force
vertical_rate += half_normal + gGravityConstant;
```

### Angular Velocity Clamping by Contact Pattern

The function applies different clamp patterns based on `suspension_response_mask`:

| Mask Value | Grounded Pattern | Pitch Clamp | Roll Clamp |
|-----------|-----------------|-------------|------------|
| 0,1,2,4,6,8,9 | General/symmetric | [-4000, +4000] | [-4000, +4000] |
| 3, 0xC | Left or right pair only | [-4000, +4000] | None |
| 5, 0xA | Front or rear pair only | None | [-4000, +4000] |

This prevents the chassis from spinning uncontrollably when only some wheels touch ground.

---

## 6. `ApplyDampedSuspensionForce` (0x4437C0) -- Low-Frequency Roll/Pitch Damping

Only called from `IntegrateVehicleFrictionForces` (traffic vehicles). Applies a second-order damped oscillator to roll and pitch accumulators at actor+0x2DC/+0x2E0.

```c
// Roll channel (actor+0x2DC = displacement, actor+0x2EC = velocity)
vel = actor.roll_velocity;
disp = actor.roll_displacement + vel;
actor.roll_displacement = disp;

new_vel = (param_roll_force * 0x80 >> 8)  // input force scaled by 0.5
        + vel                               // inertia
        + (vel * -0x20 >> 8)               // velocity damping (12.5%)
        - (disp * 0x20 >> 8);              // position spring (12.5%)
actor.roll_velocity = new_vel;

// Clamp: displacement to [-0x2000, +0x2000] (roll)
if (disp > 0x2000 || disp < -0x2000) { clamp; zero velocity; }

// Pitch channel (actor+0x2E0 = displacement, actor+0x2F0 = velocity)
// Same formula, but clamp to [-0x4000, +0x4000] (pitch has wider range)
```

**Spring constant:** 0x20/256 = 12.5% per tick
**Damping constant:** 0x20/256 = 12.5% per tick (critically damped)
**Force input gain:** 0x80/256 = 50%

---

## 7. `IntegrateVehicleFrictionForces` (0x4438F0) -- Traffic Vehicle Physics

This is the simplified friction model for NPC traffic vehicles. It computes yaw and lateral forces using a reduced 2-axle model.

```c
// Velocity decay: 1/256 of speed per frame (friction)
vel_x -= (vel_x * 16) >> 12;  // = vel_x / 256
vel_z -= (vel_z * 16) >> 12;

// Body-frame decomposition (same cos/sin approach as player)
fwd_speed = (cos * vel_x - sin * vel_z) >> 12;
lat_speed = (cos * vel_z + sin * vel_x) >> 12;

// Yaw moment of inertia (hardcoded for traffic)
I_yaw_cos = (cos * 0x271 >> 12) * cos >> 12;  // 0x271 = 625
I_yaw_sin = (sin * 0x14C >> 12) * sin >> 12;  // 0x14C = 332
I_total = I_yaw_cos + I_yaw_sin;

// Centripetal + lateral force -> yaw torque
yaw_torque_front = ((yaw_rate * 0x114 + fwd_speed * 400) >> 10) * 800;
lat_force_term = (steering_delta + lat_speed) * 0x14C;
front_torque = (lat_force_term >> 12 * sin - yaw_torque_front >> 12 * cos) / I_total;

rear_torque = (similar with opposite signs);

// Clamp both to [-0x800, +0x800]

// Update angular velocity (yaw)
yaw_rate += ((cos * 400 >> 12) * front_torque + rear_torque * -400) / 0x114;

// Update world velocities
vel_x += front_torque * cos_combined + rear_torque * cos_steer + steering * sin_steer;
vel_z += -front_torque * sin_combined - rear_torque * sin_steer + steering * cos_steer;
```

---

## 8. Fixed-Point Scaling Summary

| Scale | Bit Shift | Range | Used For |
|-------|-----------|-------|----------|
| 8.8 | >>8, <<8 | +/- 128.0 | Velocities, forces, grip, angles (low precision) |
| 4.12 | >>12 | +/- 8.0 | Trig functions (CosFixed12bit returns [-4096, +4096] = [-1.0, +1.0]) |
| 10.22 | N/A | Large | Inertia, wheelbase (high precision geometry) |
| Angle unit | 4096 = 360 deg | 0-4095 | 12-bit wrapped angles (0x800 = 180 deg, 0x400 = 90 deg) |
| Suspension | raw int | +/- travel_limit | Displacement in abstract units, typically 0-512 range |

### Key Constants

| Constant | Hex | Decimal | Meaning |
|----------|-----|---------|---------|
| `gGravityConstant` | 0x800 / 0x76C / 0x5DC | 2048/1900/1500 | Gravity (hard/normal/easy) |
| Inertia divisor | 0x28C | 652 | Converts inertia to yaw-compatible scale |
| Yaw clamp | 0x578 | 1400 | Maximum yaw torque per tick |
| Roll/pitch angular clamp | 4000 | 4000 | Max angular velocity from suspension |
| Attitude recovery threshold | 0x27F-0x355 | 639-853 | ~58-77 deg roll recovery |
| Grip clamp (player) | 0x38-0x50 | 56-80 | Per-wheel grip range |
| Velocity deadband | 0x10 | 16 | Suspension jitter threshold |
| Angular deadband | 0x40 / 0x20 | 64 / 32 | Chassis settling threshold |

---

## 9. Call Graph Summary

```
UpdateVehicleActor (0x406650)
  |
  +-- [if player] UpdatePlayerVehicleDynamics (0x404030)
  |     |-- GetTrackSegmentSurfaceType (x5 -- center + 4 wheels)
  |     |-- ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
  |     |-- CosFixed12bit / SinFixed12bit (body-frame transform)
  |     |-- UpdateAutomaticGearSelection / ApplyReverseGearThrottleSign
  |     |-- UpdateEngineSpeedAccumulator (0x42EDF0)
  |     |-- ComputeDriveTorqueFromGearCurve (0x42F030)
  |     |-- ComputeReverseGearTorque (0x403C80)
  |     |-- [friction circle check + yaw torque computation -- inline]
  |     |-- IntegrateWheelSuspensionTravel (0x403A20)
  |     +-- ApplyMissingWheelVelocityCorrection (0x403EB0)
  |
  +-- [if AI] UpdateAIVehicleDynamics (0x404EC0)
  |     |-- (Same structure but simplified: no per-wheel surface, wider grip)
  |     +-- IntegrateWheelSuspensionTravel (0x403A20)
  |
  +-- [if state 0x0F] UpdateVehicleState0fDamping (0x403D90)
  |     +-- IntegrateWheelSuspensionTravel (0x403A20)
  |
  +-- IntegrateVehiclePoseAndContacts (0x405E80)
  |     |-- BuildRotationMatrixFromAngles
  |     |-- UpdateActorTrackPosition
  |     |-- RefreshVehicleWheelContactFrames (0x403720)
  |     |-- TransformTrackVertexByMatrix (contact switch)
  |     |-- [suspension response flag check at 0x46318C]
  |     +-- UpdateVehicleSuspensionResponse (0x4057F0)
  |
  +-- ClampVehicleAttitudeLimits (0x405B40)
  +-- UpdateActorTrackSegmentContacts (forward/reverse/check)
```

---

## 10. Comparison: Player vs AI vs Traffic Dynamics

| Feature | Player | AI Racer | Traffic |
|---------|--------|----------|---------|
| Wheel count | 4 independent | 4 (simplified) | 2-axle abstract |
| Surface sampling | 5 probes (center+4) | 1 probe (center) | 1 probe |
| Grip clamp | [0x38, 0x50] | [0x70, 0xA0] | N/A (friction const) |
| Weight transfer | CG shift via suspension | CG shift via suspension | None |
| Slip circle | Full with lateral_stiffness | Simplified | None |
| Yaw clamp | +/- 0x578 | +/- 0x578 | +/- 0x800 |
| Handbrake | Yes (rear grip reduction) | No | No |
| Gear selection | Manual or auto | Always auto | N/A |
| Traction control | Sliding flag + slip circle | Sliding flag (unused?) | None |
| Suspension | 5-node spring-damper | 5-node spring-damper | 2-axis damped |
| Dynamics function | UpdatePlayerVehicleDynamics | UpdateAIVehicleDynamics | IntegrateVehicleFrictionForces |

---

## Open Questions

1. **Anti-roll bar absence confirmed:** No cross-wheel coupling exists in `IntegrateWheelSuspensionTravel`. The 4 wheels are fully independent spring-dampers. The only coupling is through the chassis angular response in `UpdateVehicleSuspensionResponse`.

2. **Inverted spring/damper naming:** The physics table names `suspension_damping` (+0x5E) and `suspension_spring_rate` (+0x60) are functionally swapped from classical terminology. The "damping" field is position-proportional and the "spring" field is velocity-proportional.

3. **No dynamic weight transfer from forces:** The weight distribution only changes through suspension geometry (CG shift), not from F=ma computation. This simplification means weight transfer during hard braking/acceleration is qualitative (through suspension pitch) rather than quantitative.

4. **lateral_slip_stiffness role confirmed:** It scales the lateral force generated during slides, acting as the tire's cornering stiffness analog. Higher values = more aggressive oversteer behavior.

5. **Surface type 6 (grass) anomaly:** Grass has low friction (0xB4 = 70%) but zero damping (0x00), while dirt has moderate friction (0xC8 = 78%) but significant damping (0x08). This means grass is slippery but doesn't slow you, while dirt provides more grip but decelerates significantly.
