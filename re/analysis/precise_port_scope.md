# Precise-Port Scope — Must-Be-Byte-Exact Simulation Core

**Created:** 2026-05-13
**Goal:** Enumerate the deterministic-simulation functions that must produce byte-identical output to `TD5_d3d.exe` per sim_tick. Functions outside this set (render, audio, FMV, frontend, IO) are intentionally replaced and excluded.

**Total: 58 unique functions across 5 harness families (59 in-scope entries; 0x004079C0 appears in both physics_trace and collision_trace).**

## Why this set

The render/audio/IO layers diverge by design (D3D11 wrapper, DXSound, FMV stub). The simulation chain — physics integrator, suspension, AI dynamics, route VM, collision/V2V, track walker — must be 1:1 or compound divergences propagate as the symptoms we've been chasing (Edinburgh chassis launch, Honolulu rollover, V2V clipping, drag-strip AI gap, branch traversal stalls). Memory entries `reference_joint_collision_suspension_chain` and `plan_faithful_collision_port` already identify this chain; this file scopes it.

**Batch-port revisions (2026-05-14):** During parallel pool-agent verification the original scope list surfaced 3 incorrect addresses and 1 missing support function:
- `0x00435930 InitializeTrafficFromQueue` — bogus; that address is mid-data in a jump-table region (confirmed by pool2 agent). Removed.
- `0x00435310 RecycleTrafficFromQueue` — bogus; that address is `SUB EAX,EDX` mid-body inside `UpdateActorTrackBehavior` at 0x00434FE0 (confirmed by pool1 agent). Removed.
- `0x0040728B TOI_substep` — wrong address; mid-function in `AND EDX,0xFFF`. Real function starts at `0x00407270 ApplySimpleTrackSurfaceForce` and is a track-surface force helper, not a TOI bisector. Remapped.
- `0x0042EB10 TransformShortVec3ByRenderMatrixRounded` — added to physics_trace as a support function pulled in during batching (precise-0042EB10 commit af4255a).

---

## physics_trace family (27)

| Addr | Name | Purpose | Port file | Risk |
|------|------|---------|-----------|------|
| 0x00404030 | UpdatePlayerVehicleDynamics | 4-wheel player dynamics | td5_physics.c | LOW |
| 0x00404EC0 | UpdateAIVehicleDynamics | 2-axle AI dynamics (bicycle solve) | td5_physics.c | MEDIUM |
| 0x00406650 | UpdateVehicleActor | Per-tick dispatcher | td5_physics.c | LOW |
| 0x00405E80 | IntegrateVehiclePoseAndContacts | Pose + contact probe | td5_physics.c | LOW |
| 0x004063A0 | UpdateVehiclePoseFromPhysicsState | Physics → world transform | td5_physics.c | LOW |
| 0x00403720 | RefreshVehicleWheelContactFrames | Per-wheel probe (TWO transforms) | td5_physics.c | **HIGH** |
| 0x00403A20 | IntegrateWheelSuspensionTravel | Spring integrator | td5_physics.c | **HIGH** |
| 0x004057F0 | UpdateVehicleSuspensionResponse | Damping/spring response | td5_physics.c | **HIGH** |
| 0x00405B40 | ClampVehicleAttitudeLimits | Roll/pitch limits | td5_physics.c | LOW |
| 0x00405D70 | ResetVehicleActorState | Crash recovery state init | td5_physics.c | LOW |
| 0x00403EB0 | ApplyMissingWheelVelocityCorrection | Wheel-mask snap | td5_physics.c | MEDIUM |
| 0x00403D90 | UpdateVehicleState0fDamping | Damping flag transitions | td5_physics.c | LOW |
| 0x00403C80 | ComputeReverseGearTorque | Reverse torque formula | td5_physics.c | LOW |
| 0x0042EBF0 | ComputeVehicleSurfaceNormalAndGravity | Surface normal + gravity | td5_physics.c | MEDIUM |
| 0x0042ED50 | UpdateVehicleEngineSpeedSmoothed | Engine RPM smoothing | td5_physics.c | LOW |
| 0x0042EDF0 | UpdateEngineSpeedAccumulator | Engine accumulator | td5_physics.c | LOW |
| 0x0042EEA0 | ApplySteeringTorqueToWheels | Steering torque distribution | td5_physics.c | LOW |
| 0x0042EF10 | UpdateAutomaticGearSelection | Auto-gear FSM | td5_physics.c | LOW |
| 0x0042F010 | ApplyReverseGearThrottleSign | Reverse throttle sign | td5_physics.c | LOW |
| 0x0042F030 | ComputeDriveTorqueFromGearCurve | Drive-torque LUT | td5_physics.c | LOW |
| 0x0042F140 | InitializeRaceVehicleRuntime | Vehicle runtime init | td5_physics.c | LOW |
| 0x0042F6D0 | ComputeVehicleSuspensionEnvelope | Envelope calc | td5_physics.c | LOW |
| 0x004437C0 | ApplyDampedSuspensionForce | Traffic suspension | td5_physics.c | LOW |
| 0x004438F0 | IntegrateVehicleFrictionForces | Traffic friction | td5_physics.c | LOW |
| 0x004079C0 | ApplyVehicleCollisionImpulse | Per-corner impulse solver | td5_physics.c | **HIGH** |
| 0x00409150 | ResolveVehicleContacts | Spatial contact resolution | td5_physics.c | **HIGH** |
| 0x0042EB10 | TransformShortVec3ByRenderMatrixRounded | Render-matrix transform support helper | td5_physics.c | LOW |

## ai_trace family (15)

| Addr | Name | Purpose | Port file | Risk |
|------|------|---------|-----------|------|
| 0x00434FE0 | UpdateActorTrackBehavior | Route FSM | td5_ai.c | MEDIUM |
| 0x004340C0 | UpdateActorSteeringBias | Steering bias select | td5_ai.c | MEDIUM |
| 0x004370A0 | AdvanceActorTrackScript | 12-opcode VM | td5_ai.c | MEDIUM |
| 0x00434900 | UpdateActorTrackOffsetBias | Lateral offset bias | td5_ai.c | MEDIUM |
| 0x004337E0 | FindActorTrackOffsetPeer | Lateral proximity scan | td5_ai.c | LOW |
| 0x00434AA0 | UpdateActorRouteThresholdState | Threshold gate | td5_ai.c | MEDIUM |
| 0x00432D60 | ComputeAIRubberBandThrottle | Rubber-banding | td5_ai.c | MEDIUM |
| 0x00432E60 | InitializeRaceActorRuntime | AI init | td5_ai.c | LOW |
| 0x00434DA0 | UpdateSpecialTrafficEncounter | Traffic encounter | td5_ai.c | MEDIUM |
| 0x00434BA0 | UpdateSpecialEncounterControl | Encounter control branch | td5_ai.c | MEDIUM |
| 0x004353B0 | RecycleTrafficActorFromQueue | Traffic despawn | td5_ai.c | LOW |
| 0x00435940 | InitializeTrafficActorsFromQueue | Traffic spawn | td5_ai.c | LOW |
| 0x00435E80 | UpdateTrafficRoutePlan | Traffic routing | td5_ai.c | LOW |
| 0x00433680 | FindNearestRoutePeer | Traffic proximity | td5_ai.c | LOW |
| 0x00436A70 | UpdateRaceActors | Master per-tick dispatcher | td5_ai.c | LOW |

## collision_trace family (7) — HIGH priority

| Addr | Name | Purpose | Port file | Risk |
|------|------|---------|-----------|------|
| 0x004079C0 | ApplyVehicleCollisionImpulse | Per-corner impulse | td5_physics.c | **HIGH** |
| 0x00408A60 | ResolveVehicleCollisionPair | Broadphase + TOI bisect | td5_physics.c | **HIGH** |
| 0x00408570 | CollectVehicleCollisionContacts | contactData[4] layout | td5_physics.c | **HIGH** |
| 0x00406980 | WallResponse (V2W) | Track wall impulse | td5_physics.c | **HIGH** |
| 0x00407BB7 | V2V_SIDE_branch | Lateral V2V | td5_physics.c | **HIGH** |
| 0x00407D70 | V2V_FRONT_branch | Axial V2V | td5_physics.c | **HIGH** |
| 0x00407270 | ApplySimpleTrackSurfaceForce | Track surface force helper (formerly mis-listed as 0x0040728B TOI_substep) | td5_physics.c | **HIGH** |

## route_trace family (7)

| Addr | Name | Purpose | Port file | Risk |
|------|------|---------|-----------|------|
| 0x00444070 | BindTrackStripRuntimePointers | STRIP.DAT binding | td5_track.c | LOW |
| 0x004440F0 | UpdateActorTrackPosition | 4-edge walker | td5_track.c | MEDIUM |
| 0x00443FB0 | NormalizeActorTrackWrapState | Ring-modulo normalize | td5_track.c | LOW |
| 0x00443FF0 | ResolveActorSegmentBoundary | Segment boundary remap | td5_track.c | MEDIUM |
| 0x00434670 | ComputeSplinePosition | Signed spline distance | td5_track.c | MEDIUM |
| 0x00434800 | SampleTrackTargetPoint | Span/lane target | td5_track.c | MEDIUM |
| 0x00434350 | InitializeActorTrackPose | Checkpoint sentinel + heading | td5_track.c | LOW |

## shared_math family (3) — small, high-leverage

| Addr | Name | Purpose | Port file | Risk |
|------|------|---------|-----------|------|
| 0x0040A720 | AngleFromVector12 | atan2 → 12-bit angle from (z,x) | td5_ai.c, td5_track.c | LOW |
| 0x0040A6A0 | CosFixed12bit | Cos LUT | td5_physics.c | LOW |
| 0x0040A6C0 | SinFixed12bit | Sin LUT (cos LUT offset by -1024) | td5_physics.c | LOW |

---

## Capture batching strategy

Group functions for shared Frida sessions — each batch attaches all listed probes to one TD5_d3d.exe race, splits trace by function name in the CSV:

- **batch A — physics core**: 0x00403720, 0x00403A20, 0x004057F0, 0x00405E80, 0x004063A0, 0x00406650
- **batch B — collision**: 0x004079C0, 0x00408A60, 0x00408570, 0x00406980, 0x00407BB7, 0x00407D70, 0x00407270
- **batch C — AI core**: 0x00404EC0, 0x00434FE0, 0x004340C0, 0x004370A0, 0x00434900, 0x00432D60
- **batch D — route**: 0x004440F0, 0x00434670, 0x00434800, 0x00434350, 0x00443FB0, 0x00443FF0
- **batch E — engine/gear**: 0x0042EBF0, 0x0042ED50, 0x0042EDF0, 0x0042EEA0, 0x0042EF10, 0x0042F010, 0x0042F030
- **batch F — math + low-risk**: 0x0040A720, 0x0040A6A0, 0x0040A6C0, plus 6 LOW-risk physics_trace remainders
- **batches G–J**: traffic, AI threshold/encounter, remaining route, attitude/state remainders

Six batches cover the MEDIUM/HIGH-risk functions; the LOW set fills out the remainder (59 in-scope entries; 58 unique after 0x004079C0 dedup). Each batch = one game race + one trace split + N parallel port/diff sessions.
