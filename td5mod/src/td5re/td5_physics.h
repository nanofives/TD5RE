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

/* --- Sub-tick render interpolation ---
 * The 30 Hz sim tick is decoupled from the render loop (typically 60+ Hz under
 * VSync), so between two ticks the renderer needs a smooth pose for everything
 * anchored to an actor (camera target, HUD speedometer, shadow blob, exhaust /
 * tire smoke, audio panning). The body-mesh draw at td5_render.c:1786 uses
 * world_pos + linear_velocity * g_subTickFraction (faithful to the original
 * @ 0x40C164-0x40C1D4), but actor->render_pos itself was snap-set to
 * world_pos / 256 each tick, leaving every other consumer reading a stale
 * snapped value. That mismatch produces visible rubber-band between the car
 * body and its attached effects.
 *
 * The pattern below is the standard fixed-timestep with interpolation:
 *   1. snapshot world_pos -> prev_world_pos at the top of every sim tick
 *      (BEFORE physics integration moves world_pos)
 *   2. after the sim loop drains and g_subTickFraction is computed, lerp
 *      actor->render_pos = prev + (cur - prev) * frac, then convert /256 to
 *      float. The result is "the position 1 tick ago, advanced forward by
 *      g_subTickFraction of a tick" -- accurate during acceleration and
 *      contact response in a way velocity-extrapolation cannot be.
 *   3. seed prev_world_pos = world_pos at race init so the first interpolated
 *      frame doesn't lerp from a zeroed prev.
 */
void td5_physics_snapshot_prev_world_pos(void);
void td5_physics_seed_prev_world_pos(void);
void td5_physics_apply_render_interpolation(float subtick_fraction);

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
void td5_physics_integrate_suspension(TD5_Actor *actor, int32_t accel_x, int32_t accel_z);
void td5_physics_update_suspension_response(TD5_Actor *actor);

/* --- Attitude / Recovery --- */
void td5_physics_clamp_attitude(TD5_Actor *actor);
void td5_physics_reset_actor_state(TD5_Actor *actor);
void td5_physics_missing_wheel_correction(TD5_Actor *actor);
void td5_physics_state0f_damping(TD5_Actor *actor);

/* --- Engine / Transmission --- */
void td5_physics_update_engine_speed(TD5_Actor *actor);
void td5_physics_auto_gear_select(TD5_Actor *actor);
void td5_physics_auto_gear_select_no_kick(TD5_Actor *actor);
int32_t td5_physics_compute_drive_torque(TD5_Actor *actor);
void td5_physics_apply_steering_torque(TD5_Actor *actor);
void td5_physics_reverse_throttle_sign(TD5_Actor *actor);

/* --- Surface / Gravity --- */
void td5_physics_compute_surface_gravity(TD5_Actor *actor);

/* --- Initialization --- */
void td5_physics_init_vehicle_runtime(void);
void td5_physics_compute_suspension_envelope(TD5_Actor *actor, int slot);

/* --- Collision --- */
void td5_physics_set_collisions(int enabled);
void td5_physics_apply_collision_impulse(TD5_Actor *a, TD5_Actor *b);
void td5_physics_resolve_vehicle_contacts(void);

/* --- Wall collision response (FUN_00406980) ---
 * probe_x_fp8 / probe_z_fp8: probe world position in 24.8 fixed point.
 * Required to compute the lever-arm tangential offset that drives the
 * yaw alignment impulse (iVar9 in decomp). */
void td5_physics_wall_response(TD5_Actor *actor, int32_t wall_angle,
                               int32_t penetration, int side,
                               int32_t probe_x_fp8, int32_t probe_z_fp8);

/* --- Pose rebuild callback (UpdateVehiclePoseFromPhysicsState) ---
 * Rebuilds rotation matrix + render_pos + wheel contacts from current
 * euler angles and world_pos. Called after wall push or V2V impulse. */
void td5_physics_rebuild_pose(TD5_Actor *actor);

/* --- Dynamics mode --- */
void td5_physics_set_dynamics(int mode);  /* 0=arcade, 1=simulation */
int  td5_physics_get_dynamics(void);      /* returns current mode */
void td5_physics_set_paused(int paused); /* 1=countdown (RPM only), 0=full sim */
void td5_physics_set_xz_freeze(int freeze); /* 1=freeze XZ pos (countdown), 0=normal */
void td5_physics_set_race_slot_state(int slot, int is_human); /* 1=player, 0=AI */

/* Per-slot championship position (0=leader, 3=trailer). Drives the
 * rubber-banding tuning adjustments in td5_physics_init_vehicle_runtime.
 * Default 0 for all slots (no handicap). Call from the frontend/championship
 * progression code before td5_physics_init_vehicle_runtime(). */
void td5_physics_set_slot_series_position(int slot, int position);

/* Load per-car physics tuning from carparam.dat (268 bytes).
 * Must be called before td5_physics_init_vehicle_runtime(). */
void td5_physics_load_carparam(int slot, const uint8_t *data_268);

/* --- 12-bit angle trig utilities --- */
/* 0x40A6A0: Cos from 12-bit angle (4096 = 360 degrees), returns float */
float td5_cos_12bit(uint32_t angle);
/* 0x40A6C0: Sin from 12-bit angle (4096 = 360 degrees), returns float */
float td5_sin_12bit(uint32_t angle);

#endif /* TD5_PHYSICS_H */
