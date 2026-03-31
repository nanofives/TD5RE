# carparam.dat Physics Table Layout

**Date:** 2026-03-19
**Binary:** TD5_d3d.exe (Ghidra port 8193)
**Scope:** Complete field-by-field layout of carparam.dat (0x10C bytes), with emphasis on the 0x80-byte physics portion

---

## File Structure Overview

`carparam.dat` is 0x10C (268) bytes, loaded per-vehicle from `cars\*.zip` by `LoadRaceVehicleAssets` (0x443280). On load it is split into two tables:

| Region | File Offset | Size | Runtime Symbol | Stride per Slot |
|---|---|---|---|---|
| Tuning table | 0x00-0x8B | 0x8C (140 bytes, 0x23 dwords) | `gVehicleTuningTable` (0x4AE580) | 0x8C bytes |
| Physics table | 0x8C-0x10B | 0x80 (128 bytes, 0x20 dwords) | `gVehiclePhysicsTable` (0x4AE280) | 0x80 bytes |

The physics table pointer is stored at actor+0x1BC (`short*` offset 0xDE).
The tuning table pointer is stored at actor+0x1B8 (`short*` offset 0xDC).

**IMPORTANT CORRECTION:** The previous session document (2026-03-12-vehicle-dynamics.md) lists fields like torque curve, gear ratios, inertia, drivetrain type, etc. under "Tuning Table". This is incorrect -- those fields are accessed through the **physics** pointer (actor+0x1BC), meaning they reside in the physics table (carparam.dat 0x8C-0x10B). The decompiled code consistently shows `iVar4 = *(int*)(param_1 + 0xde)` (= actor+0x1BC) before accessing these offsets.

---

## Physics Table: Field-by-Field Layout (0x80 bytes)

All offsets below are relative to the physics table base (= carparam.dat offset 0x8C).
Confidence: `confirmed` = directly traced in decompiled code with clear semantics; `strongly-suspected` = accessed in code with plausible interpretation; `hypothesis` = inferred from position or partial evidence.

### Torque Curve (0x00-0x1F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x00-0x1F | 32B | short[16] | `torque_curve` | confirmed | `ComputeDriveTorqueFromGearCurve` (0x42F030): indexes `*(short*)(phys + engine_speed/512 * 2)`, interpolates adjacent pair. 16 entries cover engine speed range 0 to 8192 (512 per entry). |

### Chassis Geometry (0x20-0x2B)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x20 | 4 | int | `vehicle_inertia` | confirmed | `UpdatePlayerVehicleDynamics`: `iVar27 = *(int*)(phys+0x20)` -- yaw moment of inertia. Divisor in `yaw_torque / (inertia / 0x28C)`. Also used in weight transfer computation `(rear_weight^2 + inertia) >> 10`. Fixed-point, likely 10.22 or similar. |
| 0x24 | 4 | int | `half_wheelbase` | confirmed | `UpdatePlayerVehicleDynamics`: `*(int*)(phys+0x24)` -- half the wheelbase distance. Used to compute front/rear axle load distribution and as `psVar30 = half_wheelbase * 2` (full wheelbase) in grip split. |
| 0x28 | 2 | short | `front_weight_dist` | confirmed | Front axle weight distribution numerator. `iVar13 = *(short*)(phys+0x28)` then `front_load = (front_dist << 8) / (front + rear)`. |
| 0x2A | 2 | short | `rear_weight_dist` | confirmed | Rear axle weight distribution numerator. `iVar32 = *(short*)(phys+0x2A)`. Together with 0x28, these define the CG bias (front_dist + rear_dist = total weight). |

### Aerodynamic Drag (0x2C-0x2D)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x2C | 2 | short | `drag_coefficient` | confirmed | `InitializeRaceVehicleRuntime` difficulty scaling: normal mode `drag *= 300/256`, hard mode `drag *= 380/256`. Used in `ComputeDriveTorqueFromGearCurve` context and speed-dependent force. |

### Gear Ratios (0x2E-0x3D)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x2E-0x3D | 16B | short[8] | `gear_ratio_table` | confirmed | `UpdateEngineSpeedAccumulator`: `target = abs(speed/256) * gear_ratio[gear] * 45 / 4096 + 400`. `ComputeDriveTorqueFromGearCurve`: `result *= gear_ratio[gear] / 256`. Index 0=reverse, 1=neutral, 2-7=forward gears 1-6. `ComputeReverseGearTorque`: uses `gear_ratio[gear]` as denominator for speed calculation. |

### Shift Threshold Tables (0x3E-0x5D)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x3E-0x4D | 16B | short[8] | `upshift_rpm_table` | confirmed | `UpdateAutomaticGearSelection` (0x42EF10): upshift when `engine_speed > upshift_rpm[gear]` AND `gear < 8` AND `longitudinal_speed > 0`. |
| 0x4E-0x5D | 16B | short[8] | `downshift_rpm_table` | confirmed | Same function: downshift when `engine_speed < downshift_rpm[gear]` AND `gear > 2`. |

### Suspension Parameters (0x5E-0x67)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x5E | 2 | short | `suspension_damping` | confirmed | `IntegrateWheelSuspensionTravel` (0x403A20): `sVar2 = *(short*)(phys + 0x5E)` -- multiplied with wheel displacement to compute damping force: `displacement * damping / 256`. Reduces oscillation. |
| 0x60 | 2 | short | `suspension_spring_rate` | confirmed | Same function: `sVar3 = *(short*)(phys + 0x60)` -- multiplied with suspension velocity: `velocity * spring / 256`. Spring-like restoring force. |
| 0x62 | 2 | short | `suspension_feedback` | confirmed | Same function: `sVar1 = *(short*)(phys + 0x62)` -- multiplied with cross-axis velocity coupling term: `cross_term * feedback / 256`. Couples lateral and vertical forces into wheel travel. |
| 0x64 | 2 | short | `suspension_travel_limit` | confirmed | Same function: `iVar7 = (int)*(short*)(phys + 0x64)` -- max wheel displacement (symmetrical: +-travel_limit). When exceeded, displacement clamped and velocity zeroed (bottoming out). |
| 0x66 | 2 | short | `suspension_response_factor` | confirmed | Same function: `sVar4 = *(short*)(phys + 0x66)` -- multiplied with previous wheel velocity: `prev_vel * response / 256`. Additional damping/feedback term. |

### Drive Force Parameters (0x68-0x75)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x68 | 2 | short | `drive_torque_multiplier` | confirmed | `ComputeDriveTorqueFromGearCurve`: `torque_curve[i] * drive_torque_mult / 256`. Also `ApplySteeringTorqueToWheels`: gear-shift kick = `throttle * torque_mult * 0x1A / 256`. Difficulty-scaled: normal `*= 360/256`, hard `*= 650/256`. |
| 0x6A | 2 | short | `damping_low_speed` | confirmed | `UpdatePlayerVehicleDynamics` / `UpdateAIVehicleDynamics`: used as velocity damping when `actor.frame_counter < 0x20` OR `gear < 2` -- reduces speed linearly. Formula: `speed -= (speed/256) * (surface_damp*256 + damping_low) / 4096`. |
| 0x6C | 2 | short | `damping_high_speed` | confirmed | Same functions: used as velocity damping when frame_counter >= 0x20 AND gear >= 2. Same formula structure but with this higher value. Acts as aerodynamic drag at speed. |
| 0x6E | 2 | short | `brake_force` | confirmed | `UpdatePlayerVehicleDynamics` brake path: `brake_decel = brake_force * throttle_term / 256`. Clamped to not exceed current wheel speed. Difficulty-scaled in hard mode: `*= 450/256`. Also adjusted per-player by NPC speed modifier. |
| 0x70 | 2 | short | `engine_brake_force` | confirmed | Same brake path: `engine_decel = engine_brake * throttle_term / 256`. Works with brake_force for rear/front wheel braking. Hard mode: `*= 400/256`. |
| 0x72 | 2 | short | `max_rpm` | confirmed | `ComputeDriveTorqueFromGearCurve`: returns 0 when `engine_speed > max_rpm - 50` (redline cutoff). `UpdateEngineSpeedAccumulator`: clamps engine_speed to max_rpm. `UpdateVehicleEngineSpeedSmoothed`: `target = (max_rpm - 400) * throttle/256 + 400`. HUD RPM gauge: `needle_angle = (engine_speed * 0xA5A) / max_rpm + 0x400`. |
| 0x74 | 2 | short | `top_speed_limit` | confirmed | `UpdatePlayerVehicleDynamics`: `if (top_speed * 0x100 < longitudinal_speed)` then all 4 wheel drive forces zeroed. Hard speed cap. Also in difficulty-adjusted per-player code: `top_speed[gear] = (top_speed[gear] << 11) / (npc_adjust + 0x800)`. |

### Drivetrain & Traction (0x76-0x7D)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x76 | 2 | short | `drivetrain_type` | confirmed | `UpdatePlayerVehicleDynamics`: branching on `sVar2 = *(short*)(phys+0x76)`. Value 1=RWD (rear torque only), 2=FWD (front torque only), 3=AWD (torque/4 per wheel). Also checked in brake path, reverse gear, and traction control code. Used in `UpdateVehicleActor` for checking drive wheels. |
| 0x78 | 2 | short | `speed_scale_factor` | confirmed | `InitializeRaceVehicleRuntime`: `*(short*)(phys+0x78) <<= 1` (normal) or `<<= 2` (hard). Read as `*(short*)(phys+0x78)` in init and stored. Possible purpose: speed-to-RPM conversion or display multiplier. |
| 0x7A | 2 | short | `handbrake_grip_modifier` | confirmed | `UpdatePlayerVehicleDynamics`: when handbrake active (`actor+0x36E != 0`): `rear_grip_L = rear_grip_L * handbrake_grip / 256` and `rear_grip_R = rear_grip_R * handbrake_grip / 256`. Values < 256 reduce rear grip. |
| 0x7C | 2 | short | `lateral_slip_stiffness` | strongly-suspected | `UpdatePlayerVehicleDynamics`: `sVar3 = *(short*)(phys+0x7C)` -- used in slip circle calculation near traction limit: `slip_speed = speed_display * lateral_stiffness / 256`. Determines how quickly lateral grip saturates during slides. |

### Remaining Fields (0x7E-0x7F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x7E | 2 | short | `(unknown_7E)` | hypothesis | No direct access found in decompiled functions. May be padding or reserved. |

---

## Tuning Table: Field-by-Field Layout (0x8C bytes)

All offsets relative to tuning table base (= carparam.dat offset 0x00).
The tuning table is accessed via actor+0x1B8 (`short*` offset 0xDC).

### Bounding Box / Collision Geometry (0x00-0x1F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x00-0x0B | 12B | short[6] | `bbox_extents_front` | strongly-suspected | `ComputeVehicleSuspensionEnvelope`: reads vertex positions from model mesh and stores bounding extents into tuning+0x00 onward. 6 shorts = 3 pairs of (x, y, z) or (min, max) per axis. |
| 0x0C-0x17 | 12B | short[6] | `bbox_extents_rear` | strongly-suspected | Continuation of bounding volume data from `ComputeVehicleSuspensionEnvelope`. |
| 0x18-0x1F | 8B | short[4] | `bbox_vertical` | hypothesis | Continuation of suspension envelope data. Stores min/max Y values for height. |

### Wheel Position Templates (0x40-0x5F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x40-0x47 | 8B | short[4] | `wheel_pos_FL` | confirmed | `InitializeRaceVehicleRuntime` copies tuning+0x40 through +0x5F (4 iterations x 8 bytes) into actor+0x210. These are the untransformed local-space wheel positions. `RefreshVehicleWheelContactFrames` accesses these for each of 4 wheels. |
| 0x48-0x4F | 8B | short[4] | `wheel_pos_FR` | confirmed | Same copy loop, second iteration. |
| 0x50-0x57 | 8B | short[4] | `wheel_pos_RL` | confirmed | Third iteration. |
| 0x58-0x5F | 8B | short[4] | `wheel_pos_RR` | confirmed | Fourth iteration. |

### Extended Wheel/Axle Data (0x60-0x87)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x60-0x81 | 34B | mixed | `wheel_contact_geometry` | strongly-suspected | `RefreshVehicleWheelContactFrames` accesses `local_10[0x41]` = tuning+0x82 and `tuning+0x43` = tuning+0x86 as per-wheel vertical offset and suspension height parameters. Additional contact positions and reference heights for the 4 wheel + 4 extended probes. |
| 0x82 | 2 | short | `suspension_height_ref` | confirmed | `RefreshVehicleWheelContactFrames`: `sVar2 = local_10[0x41]` (tuning+0x82). Scaled by `0xB5/256` and used as vertical offset subtracted from wheel contact height to determine suspension preload. Also used in `IntegrateVehiclePoseAndContacts` for vertical reference. |
| 0x84-0x87 | 4B | mixed | `(extended_wheel_data)` | hypothesis | Likely additional contact probe offsets. |

### Collision Mass (0x88-0x89)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x88 | 2 | short | `collision_mass` | confirmed | `ApplyVehicleCollisionImpulse` (0x4079C0): `local_40 = *(short*)(*(int*)(actor+0xDC) + 0x88)` -- inverse mass term in impulse formula: `impulse = (DAT/256 * 0x1100) / ((r1^2+DAT)*mass1 + (r2^2+DAT)*mass2) / 256`. Higher values = heavier = less affected by collisions. Also set to 0x20 for traffic vehicles in init. |
| 0x8A | 2 | short | `(unknown_8A)` | hypothesis | Last 2 bytes of tuning table. Possibly unused or padding to align 0x8C boundary. |

---

## Difficulty-Dependent Scaling (InitializeRaceVehicleRuntime)

After all vehicles are loaded, the physics table values are mutated in-place:

### Hard Mode (`gDifficultyHard != 0`, gravity = 0x800)
| Field | Operation | Scale Factor |
|---|---|---|
| `drive_torque_multiplier` (+0x68) | `val = val * 0x28A / 256` | x2.54 (650/256) |
| `speed_scale_factor` (+0x78) | `val <<= 2` | x4 |
| `drag_coefficient` (+0x2C) | `val = val * 0x17C / 256` | x1.48 (380/256) |
| `brake_force` (+0x6E) | `val = val * 0x1C2 / 256` | x1.76 (450/256) |
| `engine_brake_force` (+0x70) | `val = val * 400 / 256` | x1.56 (400/256) |

### Normal Mode (`gDifficultyHard == 0 && gDifficultyEasy == 0`, gravity = 0x76C)
| Field | Operation | Scale Factor |
|---|---|---|
| `drive_torque_multiplier` (+0x68) | `val = val * 0x168 / 256` | x1.41 (360/256) |
| `speed_scale_factor` (+0x78) | `val <<= 1` | x2 |
| `drag_coefficient` (+0x2C) | `val = val * 300 / 256` | x1.17 (300/256) |

### Easy Mode (`gDifficultyEasy != 0`, gravity = 0x5DC)
No per-vehicle scaling applied.

### Per-Player NPC Adjustments (all difficulty modes)
Applied per-slot when `gRaceSlotState[slot] == 1` (human player):
- `brake_force` (+0x6E) and `engine_brake_force` (+0x70): delta redistributed based on `DAT_004AED70[player]`
- Torque curve entries (+0x32 onward, gears 2+): flattened/steepened by `DAT_004AED40[player]`
- `top_speed_limit` (+0x74): scaled by `(DAT_004AED58[player] + 0x800) / 2048`
- `damping_low_speed` (+0x6A): inversely scaled by same factor

---

## Summary: Complete carparam.dat Byte Map

```
Offset  File     Table   Size  Name
------  ------   ------  ----  ----
0x000   0x00     Tun+00  32B   bbox/suspension_envelope (6 extents, computed from mesh)
0x020   0x20     Tun+20  (32B) (additional bounding/wheel reference data, partially unknown)
0x040   0x40     Tun+40  32B   wheel_positions[4] (FL/FR/RL/RR, 8B each: x,y,z,flags)
0x060   0x60     Tun+60  34B   extended_contact_geometry (wheel probes + vertical refs)
0x082   0x82     Tun+82  2B    suspension_height_reference
0x084   0x84     Tun+84  4B    (extended wheel data, unknown)
0x088   0x88     Tun+88  2B    collision_mass (inverse mass for impulse calc)
0x08A   0x8A     Tun+8A  2B    (unknown/padding)
------- physics table begins -------
0x08C   0x8C     Phys+00 32B   torque_curve[16] (short[16], engine speed 0-8192)
0x0AC   0xAC     Phys+20 4B    vehicle_inertia (int, yaw moment)
0x0B0   0xB0     Phys+24 4B    half_wheelbase (int, CG to axle distance)
0x0B4   0xB4     Phys+28 2B    front_weight_distribution (short)
0x0B6   0xB6     Phys+2A 2B    rear_weight_distribution (short)
0x0B8   0xB8     Phys+2C 2B    drag_coefficient (short, difficulty-scaled)
0x0BA   0xBA     Phys+2E 16B   gear_ratio_table[8] (short[8], R/N/1-6)
0x0CA   0xCA     Phys+3E 16B   upshift_rpm_table[8] (short[8])
0x0DA   0xDA     Phys+4E 16B   downshift_rpm_table[8] (short[8])
0x0EA   0xEA     Phys+5E 2B    suspension_damping (short)
0x0EC   0xEC     Phys+60 2B    suspension_spring_rate (short)
0x0EE   0xEE     Phys+62 2B    suspension_feedback_coupling (short)
0x0F0   0xF0     Phys+64 2B    suspension_travel_limit (short, max displacement)
0x0F2   0xF2     Phys+66 2B    suspension_velocity_response (short)
0x0F4   0xF4     Phys+68 2B    drive_torque_multiplier (short, difficulty-scaled)
0x0F6   0xF6     Phys+6A 2B    damping_low_speed (short, velocity decay at low speed)
0x0F8   0xF8     Phys+6C 2B    damping_high_speed (short, velocity decay at high speed)
0x0FA   0xFA     Phys+6E 2B    brake_force (short, difficulty-scaled)
0x0FC   0xFC     Phys+70 2B    engine_brake_force (short, difficulty-scaled)
0x0FE   0xFE     Phys+72 2B    max_rpm (short, redline / torque cutoff)
0x100   0x100    Phys+74 2B    top_speed_limit (short, hard speed cap: val*256 > speed)
0x102   0x102    Phys+76 2B    drivetrain_type (short: 1=RWD, 2=FWD, 3=AWD)
0x104   0x104    Phys+78 2B    speed_scale_factor (short, difficulty-scaled: <<1 or <<2)
0x106   0x106    Phys+7A 2B    handbrake_grip_modifier (short, <256 = reduce rear grip)
0x108   0x108    Phys+7C 2B    lateral_slip_stiffness (short, cornering slip sensitivity)
0x10A   0x10A    Phys+7E 2B    (unknown_7E / padding)
```

---

## Key Formulas Using Physics Table Fields

### Drive Torque
```c
curve_idx = engine_speed / 512;
base = torque_curve[curve_idx] * drive_torque_mult / 256;
next = torque_curve[curve_idx+1] * drive_torque_mult / 256;
frac = engine_speed & 0x1FF;
interpolated = base + (next - base) * frac / 512;
result = interpolated * throttle / 256 * gear_ratio[gear] / 256;
if (engine_speed > max_rpm - 50) result = 0;  // redline
```

### Velocity Damping (per-frame)
```c
damp_coeff = surface_damping_table[surface_type] * 256 + (speed_low ? damping_low : damping_high);
velocity -= (velocity / 256) * damp_coeff / 4096;
```

### Weight Transfer (per-wheel grip)
```c
total_weight = front_weight + rear_weight;
front_load = (front_weight << 8) / total_weight * (half_wheelbase - steering_offset) / (half_wheelbase * 2);
rear_load = (rear_weight << 8) / total_weight * (steering_offset + half_wheelbase) / (half_wheelbase * 2);
grip = surface_friction[surface] * load / 256;  // clamped [0x38, 0x50] or [0x70, 0xA0] for AI
```

### Yaw Torque
```c
yaw_delta = (front_lateral * cos(steer) * rear_axle_moment - forward_force * rear_weight_dist)
           / (inertia / 0x28C);
// Clamped to [-0x578, +0x578]
```

### Suspension (per-wheel spring-damper)
```c
cross_term = (wheel_sample - height/256) * fwd_force + (wheel_z_sample - height_z/256) * lat_force;
spring_input = cross_term / 256 * feedback + prev_velocity * response + displacement_force;
damping = displacement * damping_coeff / 256;
restoring = spring_input * spring_rate / 256;
new_velocity = spring_input - damping - restoring;
new_displacement = displacement + new_velocity;
if (abs(new_displacement) > travel_limit) { clamp; zero velocity; }
```

### Collision Impulse
```c
impulse = ((DAT_00463204/256 * 0x1100)
         / ((r1^2 + DAT_00463204) * mass_A + (r2^2 + DAT_00463204) * mass_B) / 256)
         * relative_approach_speed / 4096;
// mass_A, mass_B = tuning+0x88 (collision_mass)
```

---

## Open Questions

1. **Tuning table 0x00-0x3F:** The first 64 bytes are partially overwritten by `ComputeVehicleSuspensionEnvelope` with model-derived bounding box data. Need to determine if any original carparam.dat data in this range is meaningful or always replaced.
2. **Tuning table 0x20-0x3F:** This 32-byte region between the bbox and wheel positions is accessed by some functions but not yet traced in detail.
3. **Physics offset 0x7E:** No access found. May be padding or reserved for future use.
4. **Tuning offset 0x8A:** Last 2 bytes of tuning table, no access found.
5. **Fixed-point scales:** Some fields use 8.8, others may use different scales. Exact fractional formats need per-field verification against actual carparam.dat values.
6. **AI grip range:** AI vehicles use wider grip clamp range [0x70, 0xA0] vs player [0x38, 0x50]. Need to verify whether this is intentional balance or a different code path.
