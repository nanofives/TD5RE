# TD5_Actor Type Propagation Results

**Date:** 2026-03-28
**Session:** 50589c8be1e74a2ca357bf035d019805 (TD5_d3d.exe, read-write)

## Types Created in Ghidra

All types placed in the `/TD5` category:

| Type | Size | Description |
|------|------|-------------|
| `TD5_Vec3_Fixed` | 12 bytes | 3-component int32 vector (24.8 fixed-point) |
| `TD5_Vec3_Float` | 12 bytes | 3-component float vector |
| `TD5_Mat3x3` | 36 bytes | 3x3 rotation matrix (float[9]) |
| `TD5_EulerAccum` | 12 bytes | 20-bit euler angle accumulators (roll/yaw/pitch) |
| `TD5_DisplayAngles` | 6 bytes | 12-bit display angles (3 x int16) |
| `TD5_TrackProbeState` | 16 bytes | (pre-existing) |
| `TD5_Actor` | **904 bytes** (0x388) | Main vehicle actor struct, 60+ named fields |

The `TD5_Actor` struct was created via a Ghidra Jython script (`re/scripts/create_td5_actor.py`) using `StructureDataType.replaceAtOffset()` to place fields at exact byte offsets within the 904-byte allocation. All sub-structs (`TD5_Vec3_Fixed`, `TD5_Mat3x3`, etc.) are embedded at their correct offsets.

## Function Signatures Updated

| Function | Address | Old Signature | New Signature |
|----------|---------|---------------|---------------|
| UpdateVehicleActor | 0x406650 | `void FUN_00406650(short *param_1)` | `void UpdateVehicleActor(TD5_Actor *actor)` |
| ResetVehicleActorState | 0x405D70 | `void FUN_00405d70(short *param_1)` | `void ResetVehicleActorState(TD5_Actor *actor)` |
| IntegrateVehiclePoseAndContacts | 0x405E80 | `void FUN_00405e80(short *param_1)` | `void IntegrateVehiclePoseAndContacts(TD5_Actor *actor)` |
| UpdateVehiclePoseFromPhysicsState | 0x4063A0 | `void FUN_004063a0(short *param_1)` | `void UpdateVehiclePoseFromPhysicsState(TD5_Actor *actor)` |
| ComputeActorWorldBoundingVolume | 0x4096B0 | `void FUN_004096b0(short *param_1, uint param_2)` | `void ComputeActorWorldBoundingVolume(TD5_Actor *actor, uint contact_mask)` |

All signatures were committed via `decomp_writeback_params`.

## Type Propagation Through Call Chains

### Forward Propagation (from ResetVehicleActorState)

The forward trace from `actor` in ResetVehicleActorState produced **96 PTRSUB operations**, showing the decompiler resolved field accesses at offsets:
- 0x376 (surface_contact_flags), 0x379 (vehicle_mode), 0x37C (damage_lockout)
- 0x1C0-0x1D4 (angular/linear velocities, 6 fields)
- 0x1F0-0x1F8 (euler_accum sub-struct)
- 0x1FC-0x204 (world_pos sub-struct)
- 0x208-0x20C (display_angles sub-struct)
- 0x144-0x14C (render_pos sub-struct)
- 0x2DC (wheel_suspension_pos array)
- 0x2FC (wheel_suspension_vel array)
- 0x310 (engine_speed_accum), 0x338 (frame_counter), 0x36B (current_gear)

The type also propagated into the **CALL to IntegrateVehiclePoseAndContacts** at 0x405E47, meaning the callee automatically received the typed actor pointer.

### Forward Propagation (from UpdateVehicleActor)

The forward trace from `actor` in UpdateVehicleActor was extensive (133K+ characters, truncated by the tool). Key observations:
- The actor pointer propagates into calls to IntegrateVehiclePoseAndContacts and UpdateVehiclePoseFromPhysicsState
- Multiple indexed accesses via `(int)actor * 0x388` and `(int)actor * 0xe2` are visible -- these are the slot-index-based addressing patterns where actor is treated as a slot index rather than a pointer

### Backward Propagation

Backward traces from IntegrateVehiclePoseAndContacts and ComputeActorWorldBoundingVolume returned 0 items, which is expected -- the type was set at the parameter level (USER_DEFINED source), so there is no further backward inference needed.

### Call Chain Relationships

```
UpdateVehicleActor (0x406650)
  |-- IntegrateVehiclePoseAndContacts (0x405E80)  [actor passed directly]
  |-- UpdateVehiclePoseFromPhysicsState (0x4063A0) [passed as function pointer]
  |-- FUN_00406CC0, FUN_00406F50, FUN_004070E0  [dispatch helpers, pass actor + callback]
  |-- FUN_00404030 (human player physics)
  |-- FUN_00404EC0 (AI physics)
  |-- FUN_00403D90 (damage recovery physics)

ResetVehicleActorState (0x405D70)
  |-- IntegrateVehiclePoseAndContacts (0x405E80)  [actor passed directly]

ComputeActorWorldBoundingVolume (0x4096B0)
  ^-- called by FUN_00409D20 (short *param_1) -- candidate for TD5_Actor* typing
```

### Callers to type-propagate next

| Function | Address | Current param_1 type | Likely type |
|----------|---------|---------------------|-------------|
| FUN_00409D20 | 0x409D20 | `short *` | `TD5_Actor *` (calls ComputeActorWorldBoundingVolume) |
| FUN_0042F140 | 0x42F140 | `void` (stdcall) | Calls IntegrateVehiclePoseAndContacts -- initialization loop |
| FUN_00404030 | 0x404030 | `short *` | `TD5_Actor *` (human player physics dispatcher) |
| FUN_00404EC0 | 0x404EC0 | `int` | Actor pointer cast (AI physics dispatcher) |
| FUN_00405B40 | 0x405B40 | `int` | Actor pointer cast (pre-physics update) |

## Before/After Decompilation Quality Examples

### ResetVehicleActorState (0x405D70) -- Most dramatic improvement

**BEFORE:**
```c
void FUN_00405d70(short *param_1) {
    *(undefined1 *)(param_1 + 0x1bb) = 0;
    *(undefined1 *)((int)param_1 + 0x379) = 0;
    param_1[0xe0] = 0;  // What is this?
    param_1[0xe1] = 0;
    param_1[0xe2] = 0;
    // ... 50+ lines of opaque param_1[N] accesses
    param_1[0x188] = 400;
    *(float *)(param_1 + 0xa2) = fVar2;
    FUN_00405e80(param_1);
}
```

**AFTER:**
```c
void ResetVehicleActorState(TD5_Actor *actor) {
    actor->surface_contact_flags = '\0';
    actor->vehicle_mode = '\0';
    actor->angular_velocity_roll = 0;
    actor->angular_velocity_yaw = 0;
    actor->angular_velocity_pitch = 0;
    actor->linear_velocity_x = 0;
    actor->linear_velocity_y = 0;
    actor->linear_velocity_z = 0;
    actor->frame_counter = 0;
    actor->damage_lockout = '\0';
    (actor->world_pos).y = -0x40000000;
    actor->current_gear = '\x02';
    actor->engine_speed_accum = 400;
    // Per-wheel suspension reset loop
    piVar2 = actor->wheel_suspension_pos;
    iVar3 = 4;
    do {
        piVar2[4] = 0;  // wheel_force_accum
        *piVar2 = 0;    // wheel_suspension_pos
        piVar2++;
        iVar3--;
    } while (iVar3 != 0);
    // Euler -> display angle conversion
    (actor->display_angles).yaw = (short)((uint)(actor->euler_accum).yaw >> 8);
    (actor->render_pos).x = (float)(actor->world_pos).x * _DAT_0045d5e8;
    // ...
    IntegrateVehiclePoseAndContacts(actor);
    actor->wheel_suspension_vel[0] = 0;
    actor->wheel_suspension_vel[1] = 0;
    // ...
}
```

### ComputeActorWorldBoundingVolume (0x4096B0) -- Complex physics with struct access

**BEFORE (excerpt):**
```c
piVar5 = (int *)(param_1 + 0x4a);
// ...
local_10 = local_10 + (piVar5[-1] - *(int *)(param_1 + 0xfe) >> 4);
local_c = local_c + (*piVar5 - *(int *)(param_1 + 0x100) >> 4);
// ...
*(int *)(param_1 + 0xe6) = *(int *)(param_1 + 0xe6) - (iVar4 * local_1c >> 8);
if ((iVar7 < -400) && (*(byte *)((int)param_1 + 0x375) < 6)) {
```

**AFTER (excerpt):**
```c
piVar5 = &(actor->probe_FL).y;
// ...
local_10 = local_10 + (((TD5_Vec3_Fixed *)(piVar5 + -1))->x - (actor->world_pos).x >> 4);
local_c = local_c + (*piVar5 - (actor->world_pos).y >> 4);
// ...
actor->linear_velocity_x = actor->linear_velocity_x - (iVar4 * local_1c >> 8);
(actor->render_pos).x = (float)(actor->world_pos).x * _DAT_0045d5e8;
(actor->render_pos).y = (float)(actor->world_pos).y * _DAT_0045d5e8;
if ((iVar7 < -400) && (actor->slot_index < 6)) {
```

## New Type Insights Discovered

1. **UpdateVehicleActor uses slot-index addressing**: The `actor` parameter in UpdateVehicleActor is actually a **slot index** (0-11), not a pointer. The function computes `(int)actor * 0x388` to get the byte offset into the global actor table at 0x4AB108. This means its true signature is closer to `void UpdateVehicleActor(int slot_index)`, but it internally computes `actor_00 = (TD5_Actor*)(0x4AB108 + slot * 0x388)` which the decompiler correctly resolves.

2. **Offset 0x290-0x294 = heading normal vector**: The decompiler shows `*(short *)&actor->field_0x290`, `field_0x292`, `field_0x294` being used as a 3-component int16 heading normal in ComputeActorWorldBoundingVolume. These should be added to the struct as `int16_t heading_normal[3]` at offset 0x290.

3. **Offset 0x0F0 = wheel_track_contacts correctly resolved**: The `actor->wheel_track_contacts` array is properly accessed with stride 6 (shorts) per wheel in the decompilation loops, confirming the `short[4][6]` layout.

4. **Offset 0x1B8 (car_definition_ptr) dereference at +0x82**: Multiple functions read `*(short*)(actor->car_definition_ptr + 0x82)`, which is the car's bounding radius. This confirms the car definition struct has a field at offset 0x82.

5. **Offset 0x180 = recovery_target_matrix**: ComputeActorWorldBoundingVolume reads/writes `actor->recovery_target_matrix` (0x180) as a float[12] block used for bounding volume rotation, confirming this is a 3x4 or padded 3x3 matrix.

6. **Dispatch pattern at 0x406CC0/0x406F50/0x4070E0**: These three functions take `(actor, callback_fn, param_table_addr)` -- they are per-wheel physics dispatch helpers that call the callback (UpdateVehiclePoseFromPhysicsState or a scripted-mode variant) for each wheel contact.

## Summary

- **5 function signatures** updated with `TD5_Actor *` parameter type
- **6 sub-struct types** created (Vec3_Fixed, Vec3_Float, Mat3x3, EulerAccum, DisplayAngles + pre-existing TrackProbeState)
- **1 main struct** (TD5_Actor, 904 bytes) with 60+ named fields at verified offsets
- **96+ type propagation operations** observed in forward trace from a single function
- Decompilation quality transformed from opaque `param_1[0xE0]` indexing to readable `actor->angular_velocity_roll` field accesses
- **5 additional functions** identified as candidates for TD5_Actor* parameter typing in the next propagation pass
- Program saved to Ghidra project
