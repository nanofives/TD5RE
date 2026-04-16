# TD5_Actor Struct -- Remaining Gap Analysis

Analysis of all unknown gap fields in the TD5_Actor struct (0x388 bytes).
Derived from Ghidra decompilation of TD5_d3d.exe, session 50589c8b.

Date: 2026-03-28

## Critical Correction: collision_spin_matrix Location

The existing header places `collision_spin_matrix` at byte **0x0C0**. This is
**INCORRECT**. In every decompiled function (ApplyVehicleCollisionImpulse at
0x4079C0, IntegrateScriptedVehicleMotion at 0x409D20, ScriptedCollisionRecovery
at 0x4096B0), the collision spin matrix is accessed via `(float *)(short_ptr + 0xC0)`
which equals byte offset **0x180** (0xC0 * sizeof(short) = 0x180).

Corrected layout of the 0x0C0--0x1AF region:

| Offset | Size | Correct Identity | Confidence |
|--------|------|-----------------|------------|
| 0x0C0--0x0EF | 48 | **bbox_vertices_upper[4]** -- upper bounding box corners (Vec3_Fixed x4) | HIGH |
| 0x120--0x143 | 36 | rotation_matrix (Mat3x3) -- unchanged | CONFIRMED |
| 0x144--0x14F | 12 | render_pos (Vec3_Float) -- unchanged | CONFIRMED |
| 0x150--0x173 | 36 | **saved_orientation** (Mat3x3) -- scripted mode working copy of rotation | HIGH |
| 0x174--0x17F | 12 | **orientation_padding** -- unused/reserved | LOW |
| 0x180--0x1A3 | 36 | **collision_spin_matrix** (Mat3x3) -- per-frame incremental rotation | HIGH |
| 0x1A4--0x1AF | 12 | **matrix_block_padding** -- unused/reserved | LOW |

---

## Gap-by-Gap Analysis

### gap_088 -- 4 bytes at 0x088--0x08B

**Proposed:** `int32_t track_forward_distance` or `int16_t track_lane_offset[2]`
**Confidence:** LOW
**Evidence:** Not accessed by any identified vehicle dynamics function. Sits
between `track_span_high_water` (0x086) and `track_sub_lane_index` (0x08C) in
the track position block. Zeroed on init. Likely a track-relative distance or
lane offset metric used by AI routing (FUN_0040A530 init writes it but was not
decompiled).

### gap_08D -- 3 bytes at 0x08D--0x08F

**Proposed:** `uint8_t track_aux_state[3]` (padding/reserved)
**Confidence:** LOW
**Evidence:** No identified code accesses these bytes. Between
`track_sub_lane_index` (0x08C) and `probe_FL` (0x090). Likely padding to align
the probe positions to a 4-byte boundary.

### gap_0E4 -- 12 bytes at 0x0E4--0x0EF

**Proposed:** Continuation of `bbox_vertices_upper` -- last vertex data
**Confidence:** MEDIUM
**Evidence:** The second loop of `RefreshVehicleWheelContactFrames` (0x403720)
processes 4 vertices at `(int *)(param_1 + 0x60)` = byte 0xC0, with stride 12
bytes (3 ints). The 4th vertex at byte 0xE4 occupies 0x0E4--0x0EF. This is the
last 12 bytes of the upper bounding box region.

If the bounding box interpretation is wrong, then this region holds:
- 0x0E4--0x0E7: unknown (4 bytes)
- 0x0E8--0x0EB: unknown (4 bytes)
- 0x0EC--0x0EF: unknown (4 bytes)

### 0x0F0--0x11F -- Per-Wheel World Positions (48 bytes)

**Proposed:** `TD5_Vec3_Fixed wheel_contact_pos[4]`
**Confidence:** HIGH
**Evidence:** Accessed as `(int *)(short_ptr + 0x7A)` = byte 0xF4, with
`piVar8[-1]` = byte 0xF0, stride 12 bytes per wheel.

| Wheel | X offset | Y offset | Z offset |
|-------|----------|----------|----------|
| FL    | 0x0F0    | 0x0F4    | 0x0F8    |
| FR    | 0x0FC    | 0x100    | 0x104    |
| RL    | 0x108    | 0x10C    | 0x110    |
| RR    | 0x114    | 0x118    | 0x11C    |

Written by `RefreshVehicleWheelContactFrames` (0x403720). Read by
`IntegrateVehiclePoseAndContacts` (0x405E80), track contact functions, and
suspension response. The header's `wheel_track_contacts[4][6]` (int16 array)
is **incorrect** -- these are int32 world positions, not int16 track indices.

### gap_174 -- 4 bytes at 0x174--0x177

**Proposed:** `uint8_t orientation_reserved[4]`
**Confidence:** LOW
**Evidence:** Not accessed by any identified function. Between `saved_orientation`
end (0x173) and `gap_178` (0x178). Likely padding or reserved.

### gap_178 -- 8 bytes at 0x178--0x17F

**Proposed:** `uint8_t orientation_reserved2[8]`
**Confidence:** LOW
**Evidence:** Not accessed by any identified function. Between gap_174 and
`collision_spin_matrix` (0x180). Combined with gap_174, these 12 bytes
(0x174--0x17F) may be reserved for future orientation state or simply struct
padding.

### gap_1D8 -- 24 bytes at 0x1D8--0x1EF

**Proposed:** `uint8_t physics_reserved[24]`
**Confidence:** MEDIUM
**Evidence:** Not zeroed by `ResetVehicleActorState` (0x405D70). Not accessed by
`IntegrateVehiclePoseAndContacts`, `UpdateVehicleSuspensionResponse`, or any
identified dynamics function. Sits between `linear_velocity_z` (0x1D4) and
`euler_accum` (0x1F0). Likely unused padding between the velocity and angle
accumulator blocks, or reserved for an unused physics feature.

### gap_20E -- 2 bytes at 0x20E--0x20F

**Proposed:** `int16_t display_angles_padding`
**Confidence:** MEDIUM
**Evidence:** Between `display_angles.pitch` (0x20C, 2 bytes) and
`wheel_display_angles` (0x210). The display angles are 3 x int16 = 6 bytes
(0x208--0x20D), leaving 2 bytes before the per-wheel data at 0x210. Likely
alignment padding.

### gap_2A0 and gap_2B0 -- RESOLVED

**Proposed:** Part of `wheel_world_positions_hires[4]` (Vec3_Fixed array)
**Confidence:** HIGH
**Evidence:** `UpdateWheelSuspension` (0x403A20) accesses:
- `(int *)(param_1 + 0x298)` with stride 12 bytes (3 ints per wheel)
- `piVar10[0]` = wheel X, `piVar10[2]` = wheel Z

The header's `wheel_world_positions[4]` at 0x270 (12 bytes each) ends at 0x2A0.
But the actual high-resolution wheel positions span **0x298--0x2C7** (4 wheels x
12 bytes). The header's layout at 0x270--0x29F needs revision:

| Offset | Size | Field | Confidence |
|--------|------|-------|------------|
| 0x270--0x297 | 40 | `wheel_contact_velocity_hires[4]` (mixed) | MEDIUM |
| 0x298--0x2C7 | 48 | `wheel_world_positions_hires[4]` (Vec3_Fixed x4) | HIGH |
| 0x2C8--0x2CB | 4 | `clean_driving_score` (int32) | HIGH |

### clean_driving_score -- 0x2C8 (NEW FIELD)

**Proposed:** `int32_t clean_driving_score`
**Confidence:** HIGH
**Evidence:**
- Accumulated per-frame in `FUN_0040A3D0` (race mode only, DAT_004aaf70 == 4):
  `actor+0x2C8 += abs(longitudinal_speed) >> 8 - grip_reduction/2`
- Decremented on collision in `FUN_0040A440`:
  `actor+0x2C8 -= collision_severity`
- Only accumulates while `finish_time == 0` (still racing)
- Read by `FUN_00402E60` for brake force modulation based on speed and score

### gap_324 -- 4 bytes at 0x324--0x327

**Proposed:** `int32_t cached_car_suspension_travel`
**Confidence:** HIGH
**Evidence:** Written during actor initialization (`FUN_0042F140`):
```
*(int *)(actor + 0x324) = (int)*(short *)(car_definition + 0x1E)
```
Read by `FUN_00402E60` (player input handler) as a multiplier for brake force
calculation. `car_definition + 0x1E` is a suspension/geometry parameter.
Used in conjunction with `longitudinal_speed` for brake force scaling.

### gap_33A -- 4 bytes at 0x33A--0x33D

**Proposed:** Split into two int16 fields:
- **0x33A--0x33B:** `int16_t steering_ramp_accumulator`
- **0x33C--0x33D:** `int16_t current_slip_metric`

**Confidence:** MEDIUM-HIGH

**Evidence for 0x33A (steering_ramp_accumulator):**
- `FUN_004340C0` (steering input processor): increments by 0x40 per frame up to
  0x100, used as a ramp multiplier for steering force. Decays when no input.
- `FUN_00402E60`: same pattern -- incremented/decremented based on steering input.
- Acts as a "steering sensitivity ramp" that increases force the longer the
  player holds a direction.

**Evidence for 0x33C (current_slip_metric):**
- `FUN_00404030` (human dynamics): `psVar5[0x19E] = 0` then set to
  `abs(tire_slip) >> 8`. Short* offset 0x19E = byte 0x33C.
- `FUN_00404EC0` (AI dynamics): `*(undefined2 *)(param_1 + 0x33C) = 0` -- zeroed.
- Measures instantaneous tire slip magnitude for SFX/feedback.

### gap_346 -- 2 bytes at 0x346--0x347

**Proposed:** `int16_t checkpoint_gate_state` or padding
**Confidence:** LOW
**Evidence:** Not directly accessed in any identified function. Between
`pending_finish_timer` (0x344) and `gap_348`. May be unused.

### gap_348 -- 4 bytes at 0x348--0x34B

**Proposed:** `int32_t total_checkpoint_time` or `int16_t checkpoint_aux[2]`
**Confidence:** LOW
**Evidence:** In `FUN_0040A8C0` (race results), `psVar7[0x151]` = byte 0x328
(finish_time), and nearby fields are used for timing. The 4 bytes at 0x348 sit
between `gap_346` and `timing_frame_counter` (0x34C). May hold total elapsed
checkpoint time or unused state.

### gap_34E -- 18 bytes at 0x34E--0x35F

**Proposed:** `int16_t checkpoint_split_times[9]`
**Confidence:** HIGH
**Evidence:** In `FUN_00409E80` (race checkpoint handler):
- `pbVar4[-0x30]` = actor + 0x37E - 0x30 = actor + 0x34E
- Written as: `*(short *)(actor + 0x34E + checkpoint_index * 2) = timing_value`
- Indexed by checkpoint count (stored at 0x37E, overloading ghost_flag)
- Each entry stores the timing_frame_counter value when that checkpoint was
  crossed, allowing lap split time computation
- Maximum 9 checkpoints (18 bytes / 2 bytes each)

### gap_360 -- 2 bytes at 0x360--0x361

**Proposed:** `int16_t airborne_frame_counter`
**Confidence:** HIGH
**Evidence:** In `UpdateVehicleActor` (0x406650):
```c
if (*(short *)(actor + 0x360) < 3 || damage_lockout != 0x0F) {
    // normal dynamics
} else {
    FUN_00403d90(actor); // enter damping/recovery mode
}
```
And in `IntegrateVehiclePoseAndContacts` (0x405E80):
```c
psVar8[0x1B0] = psVar8[0x1B0] + 1;  // short* 0x1B0 = byte 0x360
```
This field is incremented each frame when NO wheels have ground contact (all
airborne). When it reaches >= 3 frames and all wheels are off ground
(damage_lockout == 0x0F), the vehicle enters a damping recovery mode that
slowly reduces angular velocities.

### gap_362 -- 9 bytes at 0x362--0x36A

**Proposed:** `uint8_t input_state_block[9]`
**Confidence:** LOW
**Evidence:** Not directly accessed in identified functions. Between
`airborne_frame_counter` (0x360) and `current_gear` (0x36B). May contain
additional input processing state or be unused padding.

### gap_36C -- 1 byte at 0x36C

**Proposed:** `uint8_t max_gear_index`
**Confidence:** HIGH
**Evidence:** In `FUN_0042F140` (race init):
```c
local_4 = &DAT_004ab474;  // actor + 0x36C
// ...
if (2 < *local_4) {  // if max_gear > 2 (has forward gears beyond 1st)
    // modify gear ratios based on tuning
}
```
In `FUN_00402E60` (player input): gear shift up is clamped to
`(&DAT_004ab474)[iVar1] - 1` = `max_gear_index - 1`, confirming this is the
maximum gear the car can reach. Initialized from car definition during race setup:
```c
puVar11[-0xc] = (char)iVar7 + 1;  // max gear = first gear above 10000 RPM threshold + 1
```

### gap_36F -- 1 byte at 0x36F (previously labeled gap_36F)

**Proposed:** `uint8_t throttle_state`
**Confidence:** MEDIUM-HIGH
**Evidence:** In `FUN_00402E60`:
- Set to 1 when throttle is released and car is in neutral (no brake/gas input)
- Set to 0 when brake input detected at low speed (for auto-reverse logic)
- Controls whether the car is in a "coasting" neutral state
- Used alongside `brake_flag` (0x36D) and `handbrake_flag` (0x36E)

### gap_377 -- 1 byte at 0x377

**Proposed:** `uint8_t unknown_377`
**Confidence:** LOW
**Evidence:** No identified code accesses this byte directly. Between
`surface_contact_flags` (0x376) and `gap_378` (0x378). May be unused padding.

### gap_378 -- 1 byte at 0x378

**Proposed:** `uint8_t throttle_input_active`
**Confidence:** HIGH
**Evidence:** In `FUN_00402E60` (player input handler):
```c
(&DAT_004ab480)[iVar1] = ~(byte)(input_flags >> 0x1c) & 1;
```
Input bit 28 (0x1c) is the "no throttle" flag; inverting gives 1 = throttle
pressed. This field is read by multiple functions to determine if the player is
currently pressing the accelerator.

### gap_37A -- 1 byte at 0x37A

**Proposed:** `uint8_t unknown_37A`
**Confidence:** LOW
**Evidence:** No identified code accesses this byte directly. Between
`vehicle_mode` (0x379) and `track_contact_flag` (0x37B). May be unused padding
or a rarely-used debug flag.

### gap_37F -- 1 byte at 0x37F

**Proposed:** `uint8_t unknown_37F`
**Confidence:** LOW
**Evidence:** Not accessed in identified functions. Between `ghost_flag` (0x37E)
and `grip_reduction` (0x380). The ghost_flag at 0x37E is overloaded as
`checkpoint_count` during point-to-point races. This byte may be related to
checkpoint processing or simply unused.

### gap_381 -- 1 byte at 0x381

**Proposed:** `uint8_t prev_race_position`
**Confidence:** HIGH
**Evidence:** In `UpdateVehicleActor` (0x406650), traffic/encounter mode branch:
```c
(&DAT_004ab489)[iVar1] = (&DAT_004ab48b)[iVar1];
// actor + 0x381 = actor + 0x383 (race_position)
```
Each frame in traffic mode, the current `race_position` is copied here. This
preserves the previous frame's race position for delta/change detection. Also
used by `UpdateVehicleActor` for grip_reduction clamping:
```c
grip_reduction = min(grip_reduction, prev_race_position);
```

### gap_382 -- 1 byte at 0x382

**Proposed:** `uint8_t unknown_382`
**Confidence:** LOW
**Evidence:** Not accessed in identified functions. Between `prev_race_position`
(0x381) and `race_position` (0x383). May be unused padding.

---

## Additional Discoveries

### ghost_flag (0x37E) is Overloaded

In point-to-point race mode, `ghost_flag` at byte 0x37E is repurposed as
`checkpoint_count` -- it counts how many checkpoints the vehicle has passed.
The checkpoint handler (`FUN_00409E80`) reads and increments it, and it indexes
into `checkpoint_split_times[9]` at 0x34E.

### finish_time_subtick (0x336) is Overloaded

In circuit race mode, the `finish_time_subtick` field at byte 0x336 is
repurposed as `checkpoint_gate_mask` -- a bitmask (0/1/3/7/0xF) that tracks
which of the 4 circuit gates have been crossed. When all 4 gates are set
(mask == 0xF), a lap is completed.

### gap_1B4 at 0x1B4 -- car_config_ptr2

**Proposed:** `void *car_visual_config_ptr`
**Confidence:** MEDIUM
**Evidence:** In `FUN_0042F140` init:
```c
*(ptr *)(actor + 0x1B8) = local_10;  // car_definition_ptr
*(ptr *)(actor + 0x1BC) = local_c;   // tuning_data_ptr
```
The 4 bytes at 0x1B4 sit between `car_config_ptr` (0x1B0) and
`car_definition_ptr` (0x1B8). The init function writes 0x1B0 and 0x1B8/0x1BC
but skips 0x1B4, which is zeroed by the initial memset.

### gap_2D4 at 0x2D4 -- center suspension reserved

**Proposed:** `int32_t center_suspension_reserved`
**Confidence:** LOW
**Evidence:** Between `center_suspension_vel` (0x2D0) and `prev_frame_y_position`
(0x2D8). Not accessed by identified functions.

---

## Summary Table

| Offset | Size | Proposed Name | Type | Confidence |
|--------|------|--------------|------|------------|
| 0x088 | 4 | track_forward_distance | int32 | LOW |
| 0x08D | 3 | track_aux_padding | uint8[3] | LOW |
| 0x0C0 | 48 | bbox_vertices_upper[4] | Vec3_Fixed[4] | HIGH |
| 0x0F0 | 48 | wheel_contact_pos[4] | Vec3_Fixed[4] | HIGH |
| 0x174 | 4 | orientation_reserved | uint8[4] | LOW |
| 0x178 | 8 | orientation_reserved2 | uint8[8] | LOW |
| 0x1A4 | 12 | matrix_block_padding | uint8[12] | LOW |
| 0x1D8 | 24 | physics_reserved | uint8[24] | MEDIUM |
| 0x20E | 2 | display_angles_pad | int16 | MEDIUM |
| 0x298 | 48 | wheel_world_pos_hires[4] | Vec3_Fixed[4] | HIGH |
| 0x2C8 | 4 | clean_driving_score | int32 | HIGH |
| 0x324 | 4 | cached_car_suspension_travel | int32 | HIGH |
| 0x33A | 2 | steering_ramp_accumulator | int16 | MEDIUM-HIGH |
| 0x33C | 2 | current_slip_metric | int16 | MEDIUM-HIGH |
| 0x346 | 2 | checkpoint_gate_state | int16 | LOW |
| 0x348 | 4 | total_checkpoint_time | int32 | LOW |
| 0x34E | 18 | checkpoint_split_times[9] | int16[9] | HIGH |
| 0x360 | 2 | airborne_frame_counter | int16 | HIGH |
| 0x362 | 9 | input_state_block | uint8[9] | LOW |
| 0x36C | 1 | max_gear_index | uint8 | HIGH |
| 0x36F | 1 | throttle_state | uint8 | MEDIUM-HIGH |
| 0x377 | 1 | unknown_377 | uint8 | LOW |
| 0x378 | 1 | throttle_input_active | uint8 | HIGH |
| 0x37A | 1 | unknown_37A | uint8 | LOW |
| 0x37F | 1 | unknown_37F | uint8 | LOW |
| 0x381 | 1 | prev_race_position | uint8 | HIGH |
| 0x382 | 1 | unknown_382 | uint8 | LOW |

**HIGH confidence:** 11 fields (resolved with direct code evidence)
**MEDIUM/MEDIUM-HIGH:** 6 fields (strong indirect evidence)
**LOW:** 11 fields (no code access found, likely padding/reserved)

---

## Key Functions Referenced

| Address | Name | Role |
|---------|------|------|
| 0x402E60 | PlayerInputHandler | Reads gamepad/keyboard, writes steering, brake, gear |
| 0x403720 | RefreshVehicleWheelContactFrames | Computes per-wheel world positions and contact data |
| 0x403A20 | UpdateWheelSuspension | Spring-damper simulation per wheel |
| 0x403EB0 | ApplyPitchDampingFromHeading | Pitch correction based on heading normal |
| 0x404030 | HumanVehicleDynamics | Full tire/friction model for human-controlled cars |
| 0x404EC0 | AIVehicleDynamics | Simplified tire model for AI cars |
| 0x4057F0 | UpdateVehicleSuspensionResponse | Gravity + track contact angle response |
| 0x405B40 | ClampVehicleAttitudeLimits | Roll/pitch limit enforcement, triggers recovery |
| 0x405D70 | ResetVehicleActorState | Zeros velocities/accumulators on recovery complete |
| 0x405E80 | IntegrateVehiclePoseAndContacts | Main integration: velocity -> position, rebuild matrix |
| 0x4063A0 | RefreshActorContactFrame | Post-collision contact frame rebuild |
| 0x406650 | UpdateVehicleActor | Per-frame top-level vehicle update dispatcher |
| 0x4079C0 | ApplyVehicleCollisionImpulse | Vehicle-vehicle collision impulse solver |
| 0x408A60 | ResolveVehicleCollisionPair | Broadphase + narrowphase V2V collision |
| 0x409BF0 | RefreshScriptedVehicleTransforms | Rebuild matrices in scripted recovery mode |
| 0x409D20 | IntegrateScriptedVehicleMotion | Scripted mode: apply spin matrix, decay velocity |
| 0x409E80 | RaceCheckpointHandler | Checkpoint/gate crossing detection and lap timing |
| 0x40A2B0 | UpdateCheckpointCountdown | Countdown timer decrement, triggers DNF |
| 0x40A3D0 | AccumulateCleanDrivingScore | Speed-based score accumulation |
| 0x40A440 | PenalizeCleanDrivingScore | Collision penalty to driving score |
| 0x40A8C0 | UpdateRaceResults | End-of-race timing and results tabulation |
| 0x42F140 | InitializeRaceActors | Actor allocation, zeroing, car binding |
| 0x4340C0 | ProcessSteeringInput | Steering force computation with ramp |
