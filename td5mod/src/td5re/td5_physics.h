/**
 * td5_physics.h -- Vehicle dynamics, suspension, collision
 *
 * Three physics tiers:
 *   Player: 4-wheel model, 5 surface probes, grip [0x38,0x50]
 *   AI:     2-axle model, 1 surface probe, grip [0x70,0xA0]
 *   Traffic: simplified 2-axle, fixed constants
 *
 * All physics uses integer fixed-point (8.8, 12.12). No float in sim loop.
 *
 * Original functions:
 *   0x404030  UpdatePlayerVehicleDynamics     (~480 lines)
 *   0x404EC0  UpdateAIVehicleDynamics         (~260 lines)
 *   0x4438F0  IntegrateVehicleFrictionForces  (~100 lines, traffic)
 *   0x406650  UpdateVehicleActor              (~170 lines, master dispatcher)
 *   0x405E80  IntegrateVehiclePoseAndContacts (~145 lines)
 *   0x4063A0  UpdateVehiclePoseFromPhysicsState (~80 lines)
 *   0x403720  RefreshVehicleWheelContactFrames (~95 lines)
 *   0x403A20  IntegrateWheelSuspensionTravel  (~75 lines)
 *   0x4057F0  UpdateVehicleSuspensionResponse (~130 lines)
 *   0x405B40  ClampVehicleAttitudeLimits      (~75 lines)
 *   0x405D70  ResetVehicleActorState          (~60 lines)
 *   0x403EB0  ApplyMissingWheelVelocityCorrection (~55 lines)
 *   0x403D90  UpdateVehicleState0fDamping     (~45 lines)
 *   0x403C80  ComputeReverseGearTorque        (~50 lines)
 *   0x42EBF0  ComputeVehicleSurfaceNormalAndGravity (~30 lines)
 *   0x42ED50  UpdateVehicleEngineSpeedSmoothed (~30 lines)
 *   0x42EDF0  UpdateEngineSpeedAccumulator    (~35 lines)
 *   0x42EEA0  ApplySteeringTorqueToWheels     (~12 lines)
 *   0x42EF10  UpdateAutomaticGearSelection    (~40 lines)
 *   0x42F010  ApplyReverseGearThrottleSign    (~5 lines)
 *   0x42F030  ComputeDriveTorqueFromGearCurve (~30 lines)
 *   0x42F140  InitializeRaceVehicleRuntime    (~170 lines)
 *   0x42F6D0  ComputeVehicleSuspensionEnvelope(~90 lines)
 *   0x4437C0  ApplyDampedSuspensionForce      (~35 lines, traffic)
 */

#ifndef TD5_PHYSICS_H
#define TD5_PHYSICS_H

#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_physics_init(void);
void td5_physics_shutdown(void);
void td5_physics_tick(void);

/* --- Per-actor update (master dispatcher) --- */
void td5_physics_update_vehicle_actor(TD5_Actor *actor);

/* --- Player 4-wheel dynamics --- */
void td5_physics_update_player(TD5_Actor *actor);

/* --- AI 2-axle dynamics --- */
void td5_physics_update_ai(TD5_Actor *actor);

/* --- Traffic simplified dynamics --- */
void td5_physics_update_traffic(TD5_Actor *actor);

/* --- Integration --- */
void td5_physics_integrate_pose(TD5_Actor *actor);
void td5_physics_refresh_wheel_contacts(TD5_Actor *actor);
void td5_physics_integrate_suspension(TD5_Actor *actor);
void td5_physics_update_suspension_response(TD5_Actor *actor);

/* --- Attitude / Recovery --- */
void td5_physics_clamp_attitude(TD5_Actor *actor);
void td5_physics_reset_actor_state(TD5_Actor *actor);
void td5_physics_missing_wheel_correction(TD5_Actor *actor);
void td5_physics_state0f_damping(TD5_Actor *actor);

/* --- Engine / Transmission --- */
void td5_physics_update_engine_speed(TD5_Actor *actor);
void td5_physics_auto_gear_select(TD5_Actor *actor);
int32_t td5_physics_compute_drive_torque(TD5_Actor *actor);
void td5_physics_apply_steering_torque(TD5_Actor *actor);
void td5_physics_reverse_throttle_sign(TD5_Actor *actor);

/* --- Surface / Gravity --- */
void td5_physics_compute_surface_gravity(TD5_Actor *actor);

/* --- Initialization --- */
void td5_physics_init_vehicle_runtime(void);
void td5_physics_compute_suspension_envelope(TD5_Actor *actor);

/* --- Collision --- */
void td5_physics_set_collisions(int enabled);
void td5_physics_apply_collision_impulse(TD5_Actor *a, TD5_Actor *b);
void td5_physics_resolve_vehicle_contacts(void);

/* --- Dynamics mode --- */
void td5_physics_set_dynamics(int mode);  /* 0=arcade, 1=simulation */
void td5_physics_set_paused(int paused); /* 1=countdown (RPM only), 0=full sim */

/* Load per-car physics tuning from carparam.dat (268 bytes).
 * Must be called before td5_physics_init_vehicle_runtime(). */
void td5_physics_load_carparam(int slot, const uint8_t *data_268);

#endif /* TD5_PHYSICS_H */
