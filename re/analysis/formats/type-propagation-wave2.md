# Type Propagation Wave 2 -- TD5_Actor* Parameter Typing

Session: `50589c8be1e74a2ca357bf035d019805`
Date: 2026-03-28

## Summary

Applied `TD5_Actor *` to param_1 across 19 functions (9 primary targets + 10 forward-propagated).
All changes persisted via `decomp_writeback_params` and program saved.

## Primary Targets -- Renamed and Typed

| Address  | Name                            | param_1 changed | Notes |
|----------|---------------------------------|-----------------|-------|
| 0x409D20 | IntegrateScriptedVehicleMotion  | short* -> TD5_Actor* | Scripted/cinematic vehicle motion integration |
| 0x404030 | UpdatePlayerVehicleDynamics     | short* -> TD5_Actor* | Main player physics tick, very large function |
| 0x404EC0 | UpdateAIVehicleDynamics         | int -> TD5_Actor*    | AI vehicle physics tick |
| 0x405B40 | ClampVehicleAttitudeLimits      | int -> TD5_Actor*    | Clamps pitch/roll/yaw limits |
| 0x42F140 | InitializeRaceVehicleRuntime    | (void -- no param)   | Renamed only; iterates global actor array, no param to type |

## Physics Functions -- Renamed and Typed

| Address  | Name                            | param_1 changed | Notes |
|----------|---------------------------------|-----------------|-------|
| 0x4057F0 | UpdateVehicleSuspensionResponse | int -> TD5_Actor*    | Suspension spring/damper response |
| 0x4079C0 | ApplyVehicleCollisionImpulse    | short* -> TD5_Actor* | 5-param collision impulse application |
| 0x408570 | CollectVehicleCollisionContacts | int -> TD5_Actor*    | 5-param collision contact gathering |
| 0x4440F0 | UpdateActorTrackPosition        | short* -> TD5_Actor* | Track position update with int* param_2 |
| 0x445450 | ComputeActorTrackContactNormal  | short* -> TD5_Actor* | Track contact normal with 2x int* outputs |

## Forward-Propagated Functions (discovered via trace_type_forward)

These functions were identified as callees receiving the actor pointer from the primary targets.

| Address  | Current Name   | param_1 changed | Caller(s) |
|----------|----------------|-----------------|-----------|
| 0x4092D0 | FUN_004092d0   | short* -> TD5_Actor* | IntegrateScriptedVehicleMotion |
| 0x403C80 | FUN_00403c80   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics |
| 0x42EBF0 | FUN_0042ebf0   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics, UpdateAIVehicleDynamics |
| 0x42EDF0 | FUN_0042edf0   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics, UpdateAIVehicleDynamics |
| 0x42EF10 | FUN_0042ef10   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics, UpdateAIVehicleDynamics |
| 0x42F010 | FUN_0042f010   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics |
| 0x42F030 | FUN_0042f030   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics, UpdateAIVehicleDynamics |
| 0x403A20 | FUN_00403a20   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics, UpdateAIVehicleDynamics |
| 0x403EB0 | FUN_00403eb0   | int -> TD5_Actor*    | UpdatePlayerVehicleDynamics |
| 0x40A530 | FUN_0040a530   | int -> TD5_Actor*    | InitializeRaceVehicleRuntime |

## Notable Decompilation Improvements

### IntegrateScriptedVehicleMotion (0x409D20)
- All field accesses now use struct names: `actor->linear_velocity_x`, `actor->world_pos`,
  `actor->rotation_matrix`, `actor->saved_orientation`, `actor->recovery_target_matrix`,
  `actor->render_pos`, `actor->frame_counter`
- Calls to `ComputeActorWorldBoundingVolume(actor, ...)` and `ResetVehicleActorState(actor)` visible

### UpdatePlayerVehicleDynamics (0x404030)
- Complex tire physics with named fields: `actor->lateral_speed`, `actor->longitudinal_speed`,
  `actor->angular_velocity_yaw`, `actor->steering_command`, `actor->euler_accum`,
  `actor->surface_contact_flags`, `actor->handbrake_flag`, `actor->brake_flag`,
  `actor->current_gear`, `actor->encounter_steering_cmd`, `actor->engine_speed_accum`,
  `actor->front_axle_slip_excess`, `actor->rear_axle_slip_excess`,
  `actor->accumulated_tire_slip_x`, `actor->accumulated_tire_slip_z`
- Tuning data pointer: `actor->tuning_data_ptr` with indexed gear/surface parameters

### FUN_0042ebf0 (0x42EBF0) -- Suspension Normal Force Calculator
- Probe points resolved: `actor->probe_FL`, `actor->probe_FR`, `actor->probe_RL`, `actor->probe_RR`
- Computes cross product of suspension arms, applies gravity-aligned correction to `actor->linear_velocity_x/z`

### FUN_00403eb0 (0x403EB0) -- Pitch Damping from Suspension
- Reads `actor->damage_lockout` bitmask to skip damaged suspension points
- Accesses `actor->display_angles.yaw` for orientation
- Updates `actor->angular_velocity_pitch`

### FUN_0040a530 (0x40A530) -- Actor Race Initialization
- Checks `actor->slot_index` for player slot
- Sets `actor->pending_finish_timer`, `actor->ghost_flag`, `actor->finish_time`, `actor->timing_frame_counter`

## Future Propagation Candidates (Wave 3)

These functions still use generic param types and would benefit from TD5_Actor* in a future pass:

| Address  | Name           | Reason |
|----------|----------------|--------|
| 0x42F100 | FUN_0042f100   | Called with actor sub-pointers (track_span_raw etc), may take TD5_Actor* or sub-struct |
| 0x4457E0 | FUN_004457e0   | Called from IntegrateScriptedVehicleMotion with actor fields |
| 0x42DA10 | FUN_0042da10   | Matrix multiply, takes actor orientation matrices |
| 0x42CCD0 | FUN_0042ccd0   | Vector normalize, called from FUN_0042ebf0 |
| 0x42EAC0 | FUN_0042eac0   | Cross product, called from FUN_0042ebf0 |
| 0x0040A6E0 | FUN_0040a6e0 | Sin lookup (called with actor euler angles) |
| 0x0040A700 | FUN_0040a700 | Cos lookup (called with actor euler angles) |
| 0x00443CF0 | FUN_00443cf0 | Called from InitializeRaceVehicleRuntime with actor |
| 0x00405D70 | ComputeActorWorldBoundingVolume | Already named, may need param verification |
| 0x004096B0 | ResetVehicleActorState | Already named, may need param verification |

Total functions with TD5_Actor* param_1: **19** (this wave) + existing from wave 1.
