# carparam.dat Physics Table Layout

**Date:** 2026-03-19 (field map); updated 2026-04-23 (100% coverage sweep)
**Binary:** TD5_d3d.exe (Ghidra port 8193)
**Scope:** Complete field-by-field layout of carparam.dat (0x10C bytes). As of the 2026-04-23 coverage sweep, every byte in the file is classified: **live data** (read at runtime), **runtime-written** (file bytes overwritten at load by `ComputeVehicleSuspensionEnvelope`), **traffic-only** (read only when slot >= 6), **write-only/dead** (mutated but never consumed), or **padding** (uniformly zero with no readers).

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
| 0x78 | 2 | short | `speed_scale_factor` (write-only / dead) | confirmed dead | `InitializeRaceVehicleRuntime` @ 0x42F1AC / 0x42F2C7 / 0x42F389: `*(short*)(phys+0x78) <<= 1` (normal) or `<<= 2` (hard). **Write-only field** — 2026-04-23 sweep (full-binary xrefs on all 8 per-slot addresses, search_pcode/instructions/bytes across every plausible consumer) found zero readers. The init shift has no behavioural effect. File values: 9 distinct (32..144, all multiples of 4); no correlation with torque/redline/top-speed. Likely a designer-facing rating field whose consumer was cut from the shipping binary. |
| 0x7A | 2 | short | `handbrake_grip_modifier` | confirmed | `UpdatePlayerVehicleDynamics`: when handbrake active (`actor+0x36E != 0`): `rear_grip_L = rear_grip_L * handbrake_grip / 256` and `rear_grip_R = rear_grip_R * handbrake_grip / 256`. Values < 256 reduce rear grip. |
| 0x7C | 2 | short | `slip_circle_speed_coupling` (alias: `lateral_slip_stiffness`) | confirmed | Single reader at `UpdatePlayerVehicleDynamics` @ 0x404185 (`MOVSX ECX, word ptr [EBX+0x7C]`). Used symmetrically in both front-axle (flag bit 1) and rear-axle (flag bit 2) slip-circle blocks as the coupling gain weighting the along-axle speed component into the slip-circle hypotenuse denominator: `iVar = |longitudinal_slip|/256 * sVar3; hyp = sqrt(lateral_term + iVar^2) + 1; grip_scale = ... / hyp`. Higher 0x7C → slip grip saturates faster at speed. Zero reads in `UpdateAIVehicleDynamics` (AI path uses a different code branch). Empirical range across 37 cars: 32..360. |

### Padding (0x7E-0x7F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x7E | 2 | short | `_pad_7E` | confirmed padding | 2026-04-23 sweep: zero readers binary-wide (full-binary `search_pcode 0x7E` + search on every suspension/dynamics/HUD consumer). All 37 `carparam.dat` files have `0x0000` at this offset. Pure 2-byte alignment pad rounding the physics table to 0x80. |

---

## Tuning Table: Field-by-Field Layout (0x8C bytes)

All offsets relative to tuning table base (= carparam.dat offset 0x00).
The tuning table is accessed via actor+0x1B8 (`short*` offset 0xDC).

### Chassis Bounding Corners (0x00-0x3F)

**Load-order note:** `InitializeRaceVehicleRuntime` does NOT read any byte in 0x00-0x3F before `ComputeVehicleSuspensionEnvelope` (called later in the init chain via `InitializeVehicleShadowAndWheelSpriteTemplates` @ 0x40BB70) overwrites the entire region from the loaded mesh's AABB. **For player/AI slots (0..5), the on-disk values in 0x00-0x3F are effectively dead** — they are always replaced before any runtime reader sees them. For traffic slots (>= 6), `LoadRaceVehicleAssets` copies slot-0's tuning table into the traffic slot, so traffic inherits slot-0's envelope-written bbox, and the traffic carparam.dat's own 0x00-0x3F bytes are also unused. Empirical 37-car dump confirms 0x20-0x3F bytes are rotated duplicates of 0x00-0x1F (author-precomputed mesh corners left in the file), never read.

After envelope overwrite, the region holds 8 corner vertices (x, y, z, w-pad = 8 bytes each). Runtime readers of envelope-written values:

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x00-0x1F | 32B | short[8][2] | `chassis_corners_front` (runtime-written) | confirmed | 4 corner vertices written by `ComputeVehicleSuspensionEnvelope`. Read by `CollectVehicleCollisionContacts` @ 0x408570 (offsets +0x04, +0x08, +0x14), `ApplyVehicleCollisionImpulse` @ 0x4079C0 (+0x04, +0x08, +0x14), `ProcessActorSegmentTransition` @ 0x407390 (+0x08, +0x0C at 0x407593 / 0x4075a8), `ProcessActorForwardCheckpointPass` @ 0x4076C0 (+0x08, +0x0C at 0x40742b / 0x407440 / 0x407735 / 0x40774a / 0x4078a0 / 0x4078b5). Layout: short[4] per vertex (x, y, z, w-pad); w-pad is always 0. |
| 0x20-0x3F | 32B | short[8][2] | `chassis_corners_rear` (runtime-written, dead-on-disk) | confirmed | Continuation of corner vertex set — envelope writes, but **zero runtime readers** touch this range. File bytes are rotated duplicates of 0x00-0x1F in a different vertex ordering and are ignored at runtime. Classification: envelope-written, runtime-dead, file-dead. |

### Wheel Position Templates (0x40-0x5F)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x40-0x47 | 8B | short[4] | `wheel_pos_FL` | confirmed | `InitializeRaceVehicleRuntime` copies tuning+0x40 through +0x5F (4 iterations x 8 bytes) into actor+0x210. These are the untransformed local-space wheel positions. `RefreshVehicleWheelContactFrames` accesses these for each of 4 wheels. |
| 0x48-0x4F | 8B | short[4] | `wheel_pos_FR` | confirmed | Same copy loop, second iteration. |
| 0x50-0x57 | 8B | short[4] | `wheel_pos_RL` | confirmed | Third iteration. |
| 0x58-0x5F | 8B | short[4] | `wheel_pos_RR` | confirmed | Fourth iteration. |

### Traffic Alt-Wheel Templates (0x60-0x7F)

`ComputeVehicleSuspensionEnvelope` writes this 32-byte region **only when the envelope runs for a traffic slot** (`param_2 > 5`). Layout mirrors the player wheel_pos block: two alternate wheel templates at short[4] per vertex.

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x60-0x6F | 16B | short[2][4] | `traffic_alt_wheel_A` | confirmed | Envelope writes at 0x42F96A..0x42F97E (first alt wheel). Pair of (x, y, z, w-pad) short quads. Consumed by traffic wheel-contact refresh — player path never touches. |
| 0x70-0x7F | 16B | short[2][4] | `traffic_alt_wheel_B` | confirmed | Envelope writes at 0x42F949..0x42F966 (second alt wheel). Same layout. |

### Chassis Top + Suspension Reference (0x80-0x87)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x80 | 2 | short | `chassis_top_y` | confirmed | Written unconditionally by `ComputeVehicleSuspensionEnvelope` (`DAT_004ae600`). Read by `ResolveSimpleActorSeparation` @ 0x409018 / 0x409025 as separation radius: `(A.y_top + B.y_top) * 3 / 4`. Also read by `ProcessActorForwardCheckpointPass` @ 0x4078bf. |
| 0x82 | 2 | short | `suspension_height_ref` | confirmed | `RefreshVehicleWheelContactFrames`: `sVar2 = local_10[0x41]` (tuning+0x82). Scaled by `0xB5/256` and used as vertical offset subtracted from wheel contact height to determine suspension preload. Also used in `IntegrateVehiclePoseAndContacts` for vertical reference. |
| 0x84 | 2 | short | `envelope_reference_y` | confirmed | Read by `ComputeVehicleSuspensionEnvelope` @ 0x42F7CC in the running-max FP idiom: `MOVSX ECX, [ESI+0x84]; MOVSX EDX, [ESI+0x40]; SUB ECX, EDX; float(delta) > local_max ? store`. Used to compute a per-vehicle Y extent delta against wheel_pos_FL.y (tun+0x40). Paired 0x82/0x42 handled identically at 0x42F893. The 2026-04-23 xref sweep missed this site because the base pointer is register-indexed through `LEA ESI, [slot*4 + 0x4AE580]` rather than `[actor+0x1B8]`. Port mirrors this at `td5_physics.c:5487`. |
| 0x86 | 2 | short | `traffic_y_offset` | confirmed | Envelope writes unconditionally (`DAT_004ae606`). Read at `UpdateTrafficVehiclePose` @ 0x443D82: `*pose_y += *(tuning+0x86) << 8` — per-traffic-car vertical pose offset scaled into 24.8 FP. Player/AI path never reads. |

### Collision Mass + Trailing Padding (0x88-0x8B)

| Offset | Size | Type | Name | Conf. | Evidence |
|---|---|---|---|---|---|
| 0x88 | 2 | short | `collision_mass` | confirmed | `ApplyVehicleCollisionImpulse` (0x4079C0): `local_40 = *(short*)(*(int*)(actor+0xDC) + 0x88)` -- inverse mass term in impulse formula: `impulse = (DAT/256 * 0x1100) / ((r1^2+DAT)*mass1 + (r2^2+DAT)*mass2) / 256`. Higher values = heavier = less affected by collisions. Also set to 0x20 for traffic vehicles in init. |
| 0x8A | 2 | short | `_pad_8A` | confirmed padding | 2026-04-23 sweep: all 37 `carparam.dat` files have `0x0000`; no reader accesses this offset. 2-byte alignment pad rounding the tuning table to the 0x8C stride. |

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

Legend: **[L]** live data (read as-is at runtime) · **[W]** runtime-written (envelope overwrites file bytes) · **[T]** traffic-only (read only when slot >= 6) · **[D]** write-only/dead (mutated at init, never consumed) · **[P]** padding (uniformly zero, no readers).

```
Offset  File     Table   Size  Kind  Name
------  ------   ------  ----  ----  ----
0x000   0x00     Tun+00  32B   [W]   chassis_corners_front (4 vertices, envelope-written, collision/checkpoint reads)
0x020   0x20     Tun+20  32B   [W]D  chassis_corners_rear (envelope-written, zero readers, file bytes inert)
0x040   0x40     Tun+40  32B   [L]   wheel_positions[4] (FL/FR/RL/RR, 8B each: x,y,z,w-pad)
0x060   0x60     Tun+60  16B   [T]   traffic_alt_wheel_A (envelope writes only when slot>=6)
0x070   0x70     Tun+70  16B   [T]   traffic_alt_wheel_B (envelope writes only when slot>=6)
0x080   0x80     Tun+80  2B    [W]   chassis_top_y (envelope-written, separation/checkpoint reads)
0x082   0x82     Tun+82  2B    [L]   suspension_height_ref
0x084   0x84     Tun+84  2B    [L]   envelope_reference_y (read by ComputeVehicleSuspensionEnvelope)
0x086   0x86     Tun+86  2B    [W]T  traffic_y_offset (envelope-written, UpdateTrafficVehiclePose only)
0x088   0x88     Tun+88  2B    [L]   collision_mass (inverse mass for impulse calc; traffic override = 0x20)
0x08A   0x8A     Tun+8A  2B    [P]   _pad_8A (always 0x0000; alignment to 0x8C)
------- physics table begins -------
0x08C   0x8C     Phys+00 32B   [L]   torque_curve[16] (short[16], engine speed 0-8192)
0x0AC   0xAC     Phys+20 4B    [L]   vehicle_inertia (int, yaw moment)
0x0B0   0xB0     Phys+24 4B    [L]   half_wheelbase (int, CG to axle distance)
0x0B4   0xB4     Phys+28 2B    [L]   front_weight_distribution (short)
0x0B6   0xB6     Phys+2A 2B    [L]   rear_weight_distribution (short)
0x0B8   0xB8     Phys+2C 2B    [L]   drag_coefficient (short, difficulty-scaled)
0x0BA   0xBA     Phys+2E 16B   [L]   gear_ratio_table[8] (short[8], R/N/1-6)
0x0CA   0xCA     Phys+3E 16B   [L]   upshift_rpm_table[8] (short[8])
0x0DA   0xDA     Phys+4E 16B   [L]   downshift_rpm_table[8] (short[8])
0x0EA   0xEA     Phys+5E 2B    [L]   suspension_damping (short)
0x0EC   0xEC     Phys+60 2B    [L]   suspension_spring_rate (short)
0x0EE   0xEE     Phys+62 2B    [L]   suspension_feedback_coupling (short)
0x0F0   0xF0     Phys+64 2B    [L]   suspension_travel_limit (short, max displacement)
0x0F2   0xF2     Phys+66 2B    [L]   suspension_velocity_response (short)
0x0F4   0xF4     Phys+68 2B    [L]   drive_torque_multiplier (short, difficulty-scaled)
0x0F6   0xF6     Phys+6A 2B    [L]   damping_low_speed (short, velocity decay at low speed)
0x0F8   0xF8     Phys+6C 2B    [L]   damping_high_speed (short, velocity decay at high speed)
0x0FA   0xFA     Phys+6E 2B    [L]   brake_force (short, difficulty-scaled)
0x0FC   0xFC     Phys+70 2B    [L]   engine_brake_force (short, difficulty-scaled)
0x0FE   0xFE     Phys+72 2B    [L]   max_rpm (short, redline / torque cutoff)
0x100   0x100    Phys+74 2B    [L]   top_speed_limit (short, hard speed cap: val*256 > speed)
0x102   0x102    Phys+76 2B    [L]   drivetrain_type (short: 1=RWD, 2=FWD, 3=AWD)
0x104   0x104    Phys+78 2B    [D]   speed_scale_factor (mutated <<1/<<2 at init, never read)
0x106   0x106    Phys+7A 2B    [L]   handbrake_grip_modifier (short, <256 = reduce rear grip)
0x108   0x108    Phys+7C 2B    [L]   slip_circle_speed_coupling (short, cornering slip sensitivity)
0x10A   0x10A    Phys+7E 2B    [P]   _pad_7E (always 0x0000; alignment to 0x80)
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

## Fixed-Point Scales

Speed anchor (confirmed via `td5_frontend.c:3890` HUD conversions): internal speed is 24.8 FP world-units per tick. `mph = raw*256/1252`, `kph = raw*256/778`. One world unit ≈ 1 cm (wheelbase 12000 / 256 ≈ 46.9 wu ≈ 2.3 m half-wheelbase → 4.6 m wheelbase).

| Field | Offset | Type | Raw range (37 cars) | Scale | Example |
|---|---|---|---|---|---|
| bbox_extents (runtime-written) | tun+0x00..0x1F | short×16 | -820..+860 | raw world units (~1 cm) | 310 ≈ 3.1 m |
| wheel_pos (x,y,z) | tun+0x40..0x5F | short | -476..+520 | raw world units, `<<8` to 24.8 at runtime | FL_x = -266 ≈ -2.66 m |
| suspension_height_ref | tun+0x82 | short | 140..190 | raw world units, scaled `*0xB5/256` ≈ 0.707 | 152 → 107 |
| chassis_top_y | tun+0x80 | short | positive | raw world units | — |
| collision_mass | tun+0x88 | short | 3..32 (traffic=0x20) | dimensionless inverse-mass coefficient | 16 |
| torque_curve[16] | phys+0x00 | short×16 | 128..256 | **8.8** (0x100 = 1.0) | 256 = 1.0× |
| vehicle_inertia | phys+0x20 | **int** | 120000..230000 | **24.8** (divisor `/0x28C` in yaw formula) | 160000 |
| half_wheelbase | phys+0x24 | **int** | 6000..18000 | **24.8** raw world units | 12000 = 46.9 wu |
| front_weight_dist | phys+0x28 | short | 300..500 | dimensionless (ratio numerator) | 414 |
| rear_weight_dist | phys+0x2A | short | 300..480 | dimensionless (ratio numerator) | 397 |
| drag_coefficient | phys+0x2C | short | 2200..2750 | **8.8** (effective divisor 4096 when combined with `>>8`) | 2600 |
| gear_ratio[8] | phys+0x2E | short×8 | 0..3600 | **8.8** (raw/256 = ratio) | 2500/256 ≈ 9.77 |
| upshift_rpm[8] | phys+0x3E | short×8 | 5200..9999 | **raw rpm** (9999 = "never") | 7000 |
| downshift_rpm[8] | phys+0x4E | short×8 | 0..9999 | **raw rpm** | 3800 |
| suspension_damping | phys+0x5E | short | 20..70 | **8.8** | 70 → 0.273 |
| suspension_spring_rate | phys+0x60 | short | 20..55 | **8.8** | 50 → 0.195 |
| suspension_feedback | phys+0x62 | short | 25..60 | **8.8** | 30 → 0.117 |
| suspension_travel_limit | phys+0x64 | short | 6144..16384 | **24.8** world-unit delta | 8192 = 32 wu |
| suspension_response | phys+0x66 | short | 16..32 | **8.8** | 16 → 0.0625 |
| drive_torque_multiplier | phys+0x68 | short | 50..180 | **8.8** | 105 → 0.41 |
| damping_low_speed | phys+0x6A | short | -100..800 | **8.8** (combined with surface table) | 100 |
| damping_high_speed | phys+0x6C | short | 3000 (≈constant) | **8.8** | 3000 |
| brake_force | phys+0x6E | short | 400..750 | **8.8** speed-units/tick² | 740 |
| engine_brake_force | phys+0x70 | short | 400..750 | **8.8** | 740 |
| max_rpm | phys+0x72 | short | 6200..7600 | **raw rpm** | 7400 |
| top_speed_limit | phys+0x74 | short | 719..1159 | **raw 16.0** speed (compared against 24.8 via `<<8`) | 1149 ≈ 234 mph |
| drivetrain_type | phys+0x76 | short | 1..3 | enum (1=RWD, 2=FWD, 3=AWD) | — |
| handbrake_grip_modifier | phys+0x7A | short | 144..212 | **8.8** (always <256 → reduces grip) | 144 → 0.5625 |
| slip_circle_speed_coupling | phys+0x7C | short | 32..360 | **8.8** | 256 = 1.0 |

Drivetrain distribution across 37 cars: RWD (1) = 27 cars, FWD (2) = 1 car (`pit`), AWD (3) = 9 cars.

---

## Resolved Open Questions

| # | Previous question | 2026-04-23 resolution |
|---|---|---|
| 1 | Tuning 0x00-0x3F file bytes meaningful or always replaced? | **Always replaced** before any reader fires. File bytes inert for all slots (player/AI/traffic). |
| 2 | Tuning 0x20-0x3F accessed by some functions? | **No readers.** Envelope-written, runtime-dead. |
| 3 | Physics 0x7E access? | **Confirmed padding** (all files 0x0000, zero readers). |
| 4 | Tuning 0x8A access? | **Confirmed padding** (all files 0x0000, zero readers). |
| 5 | Fixed-point scales per field? | **Resolved** — see table above. Three flavours: 8.8 coefficients, 24.8 for inertia/half_wheelbase/travel_limit, raw for rpms/enums. |
| 6 | Tuning 0x60-0x87 interpretation? | **Resolved** — 0x60-0x7F are traffic-only alt-wheel templates, 0x80 is chassis_top_y, 0x84 is unused, 0x86 is traffic_y_offset. |
| 7 | Physics 0x78 (`speed_scale_factor`) purpose? | **Write-only / dead.** Exhaustive sweep (xrefs on all 8 per-slot addresses + full-binary instruction/byte-pattern search + decompile of every plausible consumer) found zero readers. Init shift has no behavioural effect. |
| 8 | Physics 0x7C (`slip_circle_speed_coupling`) port drive-force question? | **RESOLVED 2026-04-23.** Frida @ 0x00404030 confirmed the shaped `local_2c` at `[EBP-0x28]` (front-axle grip-weighted force) DOES flow to `linear_velocity_x/z` writebacks at 0x00404D7E/D9E — captured 918 → 457 mutation with coupling=256, slip_shift=256. Port landed in `td5_physics.c:1168-1264` with a defensive `slip_shift >= 0x41` gate that mirrors the original's flag-clear threshold; avoids the zero-drive edge at rest without needing exact contact-flag parity. |

## Still Open (not carparam.dat format per se)

- **AI grip clamp range:** AI vehicles use `[0x70, 0xA0]` vs player `[0x38, 0x50]`. Not a field-mapping question — it's a runtime code path difference in `UpdatePlayerVehicleDynamics` vs `UpdateAIVehicleDynamics`. Deferred to a separate AI-grip investigation.
