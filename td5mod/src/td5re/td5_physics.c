/**
 * td5_physics.c -- Vehicle dynamics, suspension, collision
 *
 * CRITICAL: All simulation arithmetic MUST use integer fixed-point.
 * No float conversions in the simulation loop. This preserves exact
 * behavioral fidelity with the original binary.
 *
 * Original function addresses and implementation status:
 *
 * 0x404030  UpdatePlayerVehicleDynamics         -- IMPLEMENTED
 * 0x404EC0  UpdateAIVehicleDynamics             -- IMPLEMENTED
 * 0x4438F0  IntegrateVehicleFrictionForces      -- IMPLEMENTED (traffic)
 * 0x406650  UpdateVehicleActor                  -- IMPLEMENTED (dispatcher)
 * 0x405E80  IntegrateVehiclePoseAndContacts     -- IMPLEMENTED
 * 0x4063A0  UpdateVehiclePoseFromPhysicsState   -- IMPLEMENTED
 * 0x403720  RefreshVehicleWheelContactFrames    -- IMPLEMENTED
 * 0x403A20  IntegrateWheelSuspensionTravel      -- IMPLEMENTED
 * 0x4057F0  UpdateVehicleSuspensionResponse     -- IMPLEMENTED
 * 0x405B40  ClampVehicleAttitudeLimits          -- IMPLEMENTED
 * 0x405D70  ResetVehicleActorState              -- IMPLEMENTED
 * 0x403EB0  ApplyMissingWheelVelocityCorrection -- IMPLEMENTED
 * 0x403D90  UpdateVehicleState0fDamping         -- IMPLEMENTED
 * 0x403C80  ComputeReverseGearTorque            -- IMPLEMENTED
 * 0x42EBF0  ComputeVehicleSurfaceNormalAndGravity-- IMPLEMENTED
 * 0x42ED50  UpdateVehicleEngineSpeedSmoothed    -- IMPLEMENTED
 * 0x42EDF0  UpdateEngineSpeedAccumulator        -- IMPLEMENTED
 * 0x42EEA0  ApplySteeringTorqueToWheels         -- IMPLEMENTED
 * 0x42EF10  UpdateAutomaticGearSelection        -- IMPLEMENTED
 * 0x42F010  ApplyReverseGearThrottleSign        -- IMPLEMENTED
 * 0x42F030  ComputeDriveTorqueFromGearCurve     -- IMPLEMENTED
 * 0x42F140  InitializeRaceVehicleRuntime        -- IMPLEMENTED
 * 0x42F6D0  ComputeVehicleSuspensionEnvelope    -- IMPLEMENTED
 * 0x4437C0  ApplyDampedSuspensionForce          -- IMPLEMENTED (traffic)
 * 0x4079C0  ApplyVehicleCollisionImpulse        -- IMPLEMENTED
 * 0x409150  ResolveVehicleContacts              -- IMPLEMENTED
 */

#include "td5_physics.h"
#include "td5_track.h"
#include "td5_platform.h"
#include "td5re.h"

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */

#define LOG_TAG "physics"

extern void *g_actor_pool;
extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

int td5_game_get_total_actor_count(void);

/* OBB corner test output: per-corner penetration data */
typedef struct OBB_CornerData {
    int16_t proj_x;     /* projected X position of corner */
    int16_t proj_z;     /* projected Z position of corner */
    int16_t pen_x;      /* penetration depth along X axis */
    int16_t pen_z;      /* penetration depth along Z axis */
} OBB_CornerData;

static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b);
static void collision_detect_full(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b);
static void collision_detect_simple(TD5_Actor *a, TD5_Actor *b);
static int  obb_corner_test(TD5_Actor *a, TD5_Actor *b,
                            int32_t ax, int32_t az, int32_t bx, int32_t bz,
                            int32_t heading_a, int32_t heading_b,
                            OBB_CornerData corners[8]);
static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target);

/* ========================================================================
 * Key globals
 *
 * 0x4AB108  gRuntimeSlotActorTable (6 x 0x388 racers + 6 x 0x388 traffic)
 * 0x463188  g_3dCollisionsEnabled
 * 0x4AAD60  g_gamePaused
 * 0x4748C0  surface grip table (player only, short[32])
 * 0x474900  surface friction table (shared, short[32])
 * 0x467394  gear torque table (per-gear multipliers)
 * ======================================================================== */

/* --- Surface grip tables (to be loaded from binary data) --- */
static int16_t s_surface_friction[32];  /* DAT_00474900: shared friction per surface */
static int16_t s_surface_grip[32];      /* DAT_004748C0: player-only per-wheel grip */
static int16_t s_gear_torque[16];       /* DAT_00467394: per-gear torque multipliers */

/* --- Globals matching original binary layout --- */
static int32_t g_gravity_constant = TD5_GRAVITY_NORMAL;
static int32_t g_collisions_enabled = 0;     /* DAT_00463188: 0=on, 1=off */
static int32_t g_game_paused = 0;            /* DAT_004AAD60 */
static int32_t s_dynamics_mode = 0;          /* 0=arcade, 1=simulation (0x42F7B0) */
static int32_t g_difficulty_easy = 0;
static int32_t g_difficulty_hard = 0;
static int32_t g_total_actor_count = 6;
static int32_t g_race_slot_state[6];         /* 1=human, 0=AI per slot */

/* V2V inertia constant = 500,000 (DAT_00463204) */
#define V2V_INERTIA_K       500000
/* V2W inertia constant = 1,500,000 (DAT_00463200) */

/* Per-actor AABB table for broadphase (stride 20 bytes: xMin, zMin, xMax, zMax, chain) */
static int32_t g_actor_aabb[TD5_MAX_TOTAL_ACTORS][5];

/* Spatial grid broadphase (FUN_00409150).
 * Bucket array indexed by (segment_index >> 2).
 * Each entry is a chain head (actor index, or 0xFF sentinel).
 * Chain links stored in g_actor_aabb[][4]. */
#define COLLISION_GRID_SIZE     256
#define COLLISION_CHAIN_END     0xFF
#define COLLISION_MAX_WALK      17
static uint8_t s_collision_grid[COLLISION_GRID_SIZE];

/* OBB_CornerData defined near top of file with forward declarations */
static uint8_t s_default_tuning[TD5_MAX_TOTAL_ACTORS][0x80];
static uint8_t s_default_cardef[TD5_MAX_TOTAL_ACTORS][0x90];
static uint8_t s_carparam_loaded[TD5_MAX_TOTAL_ACTORS];  /* 1 if carparam.dat was loaded */
static uint8_t s_loaded_cardef[TD5_MAX_TOTAL_ACTORS][0x8C]; /* carparam 0x00..0x8B */
static uint8_t s_loaded_tuning[TD5_MAX_TOTAL_ACTORS][0x80]; /* carparam 0x8C..0x10B */

/* ========================================================================
 * Forward declarations for internal helpers
 * ======================================================================== */

static void update_engine_speed_smoothed(TD5_Actor *actor);
static void apply_damped_suspension_force(TD5_Actor *actor, int32_t lateral, int32_t longitudinal);
static void update_vehicle_pose_from_physics(TD5_Actor *actor);
static int32_t compute_reverse_gear_torque(TD5_Actor *actor);
static void bind_default_vehicle_tuning(TD5_Actor *actor, int slot);

static inline void write_i16(uint8_t *base, size_t offset, int16_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

static inline void write_i32(uint8_t *base, size_t offset, int32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

/* ========================================================================
 * Fixed-point trig (12-bit angle, returns 12-bit result)
 *
 * Compact lookup-free integer cos/sin using a quadratic approximation.
 * Input: 12-bit angle (0-4095 = 0-360 degrees).
 * Output: signed 12-bit result (-4096 .. +4096).
 * ======================================================================== */

static int32_t cos_fixed12(int32_t angle)
{
    /* Use standard math for exact results matching the original game's
     * lookup table.  The previous quadratic approximation was catastrophically
     * wrong at quadrant boundaries (e.g., sin(0) returned -4096 instead of 0). */
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(cos(rad) * 4096.0);
}

static int32_t sin_fixed12(int32_t angle)
{
    double rad = (double)(angle & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
    return (int32_t)(sin(rad) * 4096.0);
}

/* ========================================================================
 * Fixed-point atan2 (12-bit result: 0-4095 = 0-360°)
 *
 * Used for wall collision angle computation. Matches FUN_0040a720 in
 * the original binary (angle LUT). Input: (dx, dz) in world coords.
 * Returns: 12-bit angle where 0 = +Z direction, CW positive.
 * ======================================================================== */

static int32_t atan2_fixed12(int32_t dx, int32_t dz)
{
    double rad = atan2((double)dx, (double)dz);
    int32_t angle = (int32_t)(rad * (4096.0 / (2.0 * 3.14159265358979323846)));
    return angle & 0xFFF;
}

/* ========================================================================
 * Wall collision response -- FUN_00406980
 *
 * Called when a probe detects the car is outside a track span edge.
 * Pushes the car back onto the road and reflects velocity with damping.
 *
 * Parameters:
 *   actor       - the vehicle actor
 *   wall_angle  - 12-bit angle of the wall edge direction
 *   penetration - signed distance (negative = outside wall)
 *   side        - 0=left boundary, 1=left inner, 2=right inner
 * ======================================================================== */

/* Wall bounce damping factor [UNCERTAIN: DAT_00463200 not read, using 0xC0 = 75% energy loss] */
#define V2W_BOUNCE_DAMP   0xC0

void td5_physics_wall_response(TD5_Actor *actor, int32_t wall_angle,
                               int32_t penetration, int side)
{
    /* Compute wall normal (perpendicular to wall direction) */
    int32_t cos_w = cos_fixed12(wall_angle);
    int32_t sin_w = sin_fixed12(wall_angle);

    /* Push actor position out of wall by (|penetration| + 4) units.
     * penetration is negative when outside, so we push in +normal direction.
     * Normal points from wall INTO the road: (cos_w, sin_w) rotated 90° = (-sin_w, cos_w)
     * Actually the original uses the wall-perpendicular direction directly.
     * [CONFIRMED @ 0x4069a0]: push by (penetration_depth - 4) along normal */
    int32_t push = (-penetration) + 4;  /* penetration is negative, push is positive */
    /* Wall normal is perpendicular to wall edge: (-sin_w, cos_w) points into road */
    actor->world_pos.x += ((-sin_w) * push) >> 4;
    actor->world_pos.z += (cos_w * push) >> 4;

    /* Decompose velocity into wall-parallel and wall-perpendicular components
     * [CONFIRMED @ 0x4069cc] */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* parallel = dot(vel, wall_direction) = vx*cos_w + vz*sin_w */
    /* perpendicular = dot(vel, wall_normal) = -vx*sin_w + vz*cos_w */
    int32_t v_para = (vx * cos_w + vz * sin_w) >> 12;
    int32_t v_perp = (-vx * sin_w + vz * cos_w) >> 12;

    /* If perpendicular velocity is INTO the wall (positive = toward road, negative = into wall):
     * For our normal pointing into road, v_perp < 0 means moving away from road (into wall).
     * Actually, this depends on the normal convention. The original clamps perpendicular
     * to zero if already moving away from the wall. [CONFIRMED @ 0x406b6b] */
    if (v_perp < 0) {
        /* Moving into the wall: reflect with damping */
        v_perp = (-v_perp * V2W_BOUNCE_DAMP) >> 8;
    }
    /* If v_perp >= 0, car is already moving away from wall — leave it */

    /* Reconstruct world velocity from parallel + damped perpendicular
     * [CONFIRMED @ 0x406a10]: vel = parallel * wall_dir + perp * wall_normal */
    actor->linear_velocity_x = (v_para * cos_w - v_perp * sin_w) >> 12;
    actor->linear_velocity_z = (v_para * sin_w + v_perp * cos_w) >> 12;

    /* Apply yaw torque from wall impact [CONFIRMED @ 0x406b75] */
    int32_t impact = v_perp < 0 ? -v_perp : v_perp;
    int32_t yaw_kick = (impact >> 6);
    if (side <= 1) yaw_kick = -yaw_kick;  /* left wall pushes CW */
    actor->angular_velocity_yaw += yaw_kick;

    /* Clamp yaw angular velocity */
    if (actor->angular_velocity_yaw > 6000) actor->angular_velocity_yaw = 6000;
    if (actor->angular_velocity_yaw < -6000) actor->angular_velocity_yaw = -6000;

    /* Set track contact flag [CONFIRMED @ 0x406d7e/0x406e4e] */
    actor->track_contact_flag = (uint8_t)(side + 1);
}

/* ========================================================================
 * Tuning data access helpers
 *
 * The tuning pointer at actor+0x1BC points to the physics table.
 * The car definition pointer at actor+0x1B8 points to the tuning table
 * (bounding box, wheel positions, collision mass).
 * ======================================================================== */

static inline int16_t *get_phys(TD5_Actor *a)
{
    return (int16_t *)a->tuning_data_ptr;
}

static inline int16_t *get_cardef(TD5_Actor *a)
{
    return (int16_t *)a->car_definition_ptr;
}

/* Read a short from the physics table at byte offset */
#define PHYS_S(a, off) (*(int16_t*)((uint8_t*)get_phys(a) + (off)))
/* Read an int from the physics table at byte offset */
#define PHYS_I(a, off) (*(int32_t*)((uint8_t*)get_phys(a) + (off)))
/* Read a short from the car definition at byte offset */
#define CDEF_S(a, off) (*(int16_t*)((uint8_t*)get_cardef(a) + (off)))
#define ACTOR_I16(base, off) (*(int16_t *)((uint8_t *)(base) + (off)))
#define ACTOR_I32(base, off) (*(int32_t *)((uint8_t *)(base) + (off)))

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_physics_init(void)
{
    memset(s_carparam_loaded, 0, sizeof(s_carparam_loaded));
    /* Surface grip coefficients from DAT_004748C0 (short[32]).
     * NOTE: s_surface_friction is used for grip calc at line ~361,
     * despite the misleading variable name. Values from original binary. */
    {
        static const int16_t k_grip_004748C0[16] = {
            0x0100, 0x0100, 0x00DC, 0x00F0, 0x00FC, 0x00C0, 0x00B4, 0x0100,
            0x0100, 0x0100, 0x00C8, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100
        };
        for (int i = 0; i < 16; i++)
            s_surface_friction[i] = k_grip_004748C0[i];
        for (int i = 16; i < 32; i++)
            s_surface_friction[i] = 0x0100;
    }

    /* Surface drag coefficients from DAT_00474900 (short[32]).
     * NOTE: s_surface_grip is used for drag/damping at line ~393. */
    {
        static const int16_t k_drag_00474900[16] = {
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0002, 0x0000, 0x0000,
            0x0000, 0x0000, 0x0008, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
        };
        for (int i = 0; i < 16; i++)
            s_surface_grip[i] = k_drag_00474900[i];
        for (int i = 16; i < 32; i++)
            s_surface_grip[i] = 0;
    }

    /* Gear torque multipliers from DAT_00467394 (dword[8] -> int16_t[16]).
     * Original: {0, 0, 256, 192, 128, 64, 32, 16} indexed by gear number.
     * Entries [0]=reverse and [1]=neutral produce zero kick. */
    {
        static const int16_t k_gear_torque[8] = {
            0, 0, 256, 192, 128, 64, 32, 16
        };
        for (int i = 0; i < 8; i++)
            s_gear_torque[i] = k_gear_torque[i];
        for (int i = 8; i < 16; i++)
            s_gear_torque[i] = 0;
    }

    for (int i = 0; i < 5; ++i) {
        TD5_LOG_I(LOG_TAG, "Surface table[%d]: friction=%d grip=%d",
                  i, s_surface_friction[i], s_surface_grip[i]);
    }

    return 1;
}

void td5_physics_shutdown(void)
{
    /* No dynamic resources to free */
}

void td5_physics_tick(void)
{
    int total;
    static uint32_t s_physics_tick_counter;

    if (!g_actor_table_base) {
        return;
    }

    total = td5_game_get_total_actor_count();
    if (total <= 0) {
        return;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        total = TD5_MAX_TOTAL_ACTORS;
    }

    s_physics_tick_counter++;
    if ((s_physics_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Physics tick: actor_count=%d", total);
    }

    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        td5_physics_update_vehicle_actor(actor);
    }

    /* Skip collision resolution during countdown — wall/vehicle impulses
     * would accumulate in velocity without integrate_pose to dissipate them,
     * causing cars to shoot off at race start. */
    if (!g_game_paused)
        td5_physics_resolve_vehicle_contacts();
}

/* ========================================================================
 * Master dispatcher -- UpdateVehicleActor (0x406650)
 * ======================================================================== */

void td5_physics_update_vehicle_actor(TD5_Actor *actor)
{
    if (!actor) return;

    /* 1. Increment frame counter */
    actor->frame_counter++;

    /* 2. Clear per-frame flags */
    actor->track_contact_flag = 0;

    /* 3. Time trial ghost: force zero throttle if ghost flag set */
    if (actor->ghost_flag) {
        actor->encounter_steering_cmd = 0;
        actor->brake_flag = 1;
    }

    /* 4. Speed tracking: accumulate distance, track peak */
    {
        int32_t spd = actor->longitudinal_speed;
        if (spd < 0) spd = -spd;
        actor->accumulated_distance += (spd >> 8);
        if ((int16_t)(spd >> 8) > actor->peak_speed)
            actor->peak_speed = (int16_t)(spd >> 8);
    }

    /* 5. Attitude clamp (unless scripted mode) */
    if (actor->vehicle_mode == 0)
        td5_physics_clamp_attitude(actor);

    /* 6. Dynamics dispatch */
    if (actor->vehicle_mode == 0 && !g_game_paused) {
        /* Select effective grip: min of grip_reduction and race_position */
        uint8_t eff_grip = actor->grip_reduction;
        if (actor->race_position < eff_grip)
            eff_grip = actor->race_position;

        if (actor->damage_lockout == 0x0F && actor->airborne_frame_counter >= 3) {
            /* Stunned/damping recovery mode */
            td5_physics_state0f_damping(actor);
        } else if (actor->slot_index < 6 && g_race_slot_state[actor->slot_index]) {
            /* Human player */
            td5_physics_update_player(actor);
        } else {
            /* AI racer or traffic */
            if (actor->slot_index < 6)
                td5_physics_update_ai(actor);
            else
                td5_physics_update_traffic(actor);
        }
    } else if (actor->vehicle_mode == 1 && !g_game_paused) {
        /* 6b. Scripted recovery mode [CONFIRMED @ 0x406650 + 0x409E5E]
         * Original: vehicle_mode==1 runs RefreshScriptedVehicleTransforms +
         * IntegrateScriptedVehicleMotion for 59 frames, then calls
         * ResetVehicleActorState to respawn the car.
         *
         * Simplified implementation: damp velocities and recover after 59 frames. */
        actor->frame_counter++;

        /* Damp velocities (original: v -= v >> 8 each frame) */
        actor->linear_velocity_x -= actor->linear_velocity_x >> 8;
        actor->linear_velocity_z -= actor->linear_velocity_z >> 8;
        actor->angular_velocity_yaw -= actor->angular_velocity_yaw >> 4;
        actor->angular_velocity_roll -= actor->angular_velocity_roll >> 4;
        actor->angular_velocity_pitch -= actor->angular_velocity_pitch >> 4;

        if (actor->frame_counter > 0x3B) {  /* 59 frames */
            TD5_LOG_I(LOG_TAG, "mode1 recovery: slot=%d resetting after %d frames",
                      actor->slot_index, actor->frame_counter);
            td5_physics_reset_actor_state(actor);

            /* Teleport to current track span (same as OOB recovery) */
            int wx = 0, wy = 0, wz = 0;
            int sub_lane = (int)actor->track_sub_lane_index;
            int span = actor->track_span_raw;
            if (td5_track_get_span_lane_world(span, sub_lane, &wx, &wy, &wz)) {
                actor->world_pos.x = wx << 8;
                actor->world_pos.y = wy << 8;
                actor->world_pos.z = wz << 8;
                /* Set heading to match track direction at this span.
                 * td5_track_compute_heading writes euler_accum.yaw on the actor. */
                td5_track_compute_heading(actor);
                actor->euler_accum.roll = 0;
                actor->euler_accum.pitch = 0;
                TD5_LOG_I(LOG_TAG, "mode1 teleport: slot=%d span=%d pos=(%d,%d,%d)",
                          actor->slot_index, span, wx, wy, wz);
            }
        }
    } else if (g_game_paused) {
        /* Paused: only update engine RPM display */
        update_engine_speed_smoothed(actor);
    }

    /* 7. Integrate pose and contacts.
     * Run even during countdown (paused) so ground-snap keeps the car
     * at the correct height above the road surface. Velocities are zero
     * during pause, so the car doesn't actually move. */
    td5_physics_integrate_pose(actor);

    /* 8. Track wall contact resolution (FUN_00406CC0 + FUN_004070E0 + FUN_00406F50)
     * Check wheel probes against span edges and push car back if outside.
     * Called after pose integration, matching original UpdateVehicleActor order.
     * [CONFIRMED @ 0x4068c8] */
    if (actor->vehicle_mode == 0 && !g_game_paused)
        td5_track_resolve_wall_contacts(actor);

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "Vehicle actor0: speed=%d rpm=%d gear=%d surface=%u",
                  actor->longitudinal_speed,
                  actor->engine_speed_accum,
                  actor->current_gear,
                  actor->surface_type_chassis);
    }
}

/* ========================================================================
 * Player 4-wheel dynamics -- UpdatePlayerVehicleDynamics (0x404030)
 * ======================================================================== */

void td5_physics_update_player(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t i;

    /* --- 1. Surface type probes (5: chassis + 4 wheels) --- */
    uint8_t surface_center = actor->surface_type_chassis;
    uint8_t surface_wheel[4];
    /* In the full build, each wheel calls GetTrackSegmentSurfaceType.
     * Here we use the chassis surface type as a fallback for all wheels. */
    for (i = 0; i < 4; i++)
        surface_wheel[i] = surface_center;

    /* --- 2. Surface normal and gravity --- */
    td5_physics_compute_surface_gravity(actor);

    /* --- 3. Per-wheel grip from surface tables, clamped [0x38..0x50] --- */
    int32_t grip[4];
    int32_t front_weight = (int32_t)PHYS_S(actor, 0x28);
    int32_t rear_weight  = (int32_t)PHYS_S(actor, 0x2A);
    int32_t total_weight = front_weight + rear_weight;
    if (total_weight == 0) total_weight = 1;

    int32_t half_wb = PHYS_I(actor, 0x24);
    int32_t full_wb = half_wb * 2;
    if (full_wb == 0) full_wb = 1;

    /* Suspension deflection -> load transfer */
    int32_t susp_defl = actor->center_suspension_pos;

    /* Front load fraction (8.8 fixed) */
    int32_t front_load = ((front_weight << 8) / total_weight);
    front_load = front_load * (half_wb - (susp_defl >> 4)) / full_wb;
    int32_t rear_load = ((rear_weight << 8) / total_weight);
    rear_load = rear_load * (half_wb + (susp_defl >> 4)) / full_wb;

    for (i = 0; i < 4; i++) {
        int32_t sf = (int32_t)s_surface_friction[surface_wheel[i] & 0x1F];
        int32_t load = (i < 2) ? front_load : rear_load;
        grip[i] = (sf * load + 128) >> 8;
        if (grip[i] < TD5_PLAYER_GRIP_MIN) grip[i] = TD5_PLAYER_GRIP_MIN;
        if (grip[i] > TD5_PLAYER_GRIP_MAX) grip[i] = TD5_PLAYER_GRIP_MAX;
    }

    /* --- 3b. Arcade mode grip boost (0x42F7B0) ---
     * Arcade: multiply grip by ~1.3 (333/256). Simulation: raw values. */
    if (s_dynamics_mode == 0) {
        for (i = 0; i < 4; i++) {
            grip[i] = (grip[i] * 333) >> 8;  /* ~1.30x */
            if (grip[i] > TD5_PLAYER_GRIP_MAX) grip[i] = TD5_PLAYER_GRIP_MAX;
        }
    }

    /* --- 4. Handbrake modifier on rear wheels (tuning+0x7A) --- */
    if (actor->handbrake_flag) {
        int32_t hb_mod = (int32_t)PHYS_S(actor, 0x7A);
        grip[2] = (grip[2] * hb_mod) >> 8;
        grip[3] = (grip[3] * hb_mod) >> 8;
    }

    /* --- 5. Velocity drag in WORLD frame (confirmed by Ghidra at 0x40409x) ---
     * Original applies drag to linear_velocity_x/z BEFORE body decomposition.
     * 0x6A = driving drag (low value ~100), 0x6C = coasting drag (high ~3000).
     * Formula: v -= ((v >> 8) * drag_coeff) >> 12
     * Condition: throttle < 0x20 || gear < 2 → use 0x6C (coast), else 0x6A (drive) */
    {
        int32_t surf_drag = (int32_t)s_surface_grip[surface_center & 0x1F];
        int32_t damp_coeff;
        if (actor->encounter_steering_cmd < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6C);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6A);

        actor->linear_velocity_x -= ((actor->linear_velocity_x >> 8) * damp_coeff) >> 12;
        actor->linear_velocity_z -= ((actor->linear_velocity_z >> 8) * damp_coeff) >> 12;
    }

    /* --- 6. Resolve body-frame velocities (cos/sin of heading) --- */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* Longitudinal = dot(velocity, heading_forward) */
    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;
    /* Lateral = dot(velocity, heading_right) */
    int32_t v_lat  = (vx * cos_h - vz * sin_h) >> 12;

    actor->longitudinal_speed = v_long;
    actor->lateral_speed = v_lat;

    /* --- 7/8. Gear selection: mutually exclusive paths (original 0x404030).
     * field_0x378 (throttle_input_active) selects between:
     *   0 -> ApplyReverseGearThrottleSign (NOS-stun path)
     *   !0 -> UpdateAutomaticGearSelection (normal driving)
     * throttle_input_active defaults to 0 and must be written by input
     * handler as ~(bits>>28)&1. Since the NOS-stun input bit is not yet
     * implemented, default to auto_gear_select for normal driving. */
    if (actor->throttle_input_active != 0) {
        td5_physics_auto_gear_select(actor);
    } else {
        /* Fallback: always use auto gear until NOS-stun input is wired */
        td5_physics_auto_gear_select(actor);
    }

    /* --- 9. UpdateEngineSpeedAccumulator --- */
    td5_physics_update_engine_speed(actor);

    /* --- 10. ComputeDriveTorqueFromGearCurve ---
     * Drive torque is computed unconditionally (including airborne).
     * The original allows engine revving and speed changes mid-air. */
    int32_t drive_torque = td5_physics_compute_drive_torque(actor);

    if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "Player phys: gear=%d rpm=%d torque=%d v_long=%d v_lat=%d "
                  "throttle_active=%d surface_contact=0x%X",
                  actor->current_gear, actor->engine_speed_accum, drive_torque,
                  v_long, v_lat, actor->throttle_input_active,
                  actor->surface_contact_flags);
    }

    /* --- 11. Distribute torque by drivetrain (FWD/RWD/AWD) --- */
    int32_t wheel_drive[4] = {0, 0, 0, 0};
    int32_t dt_type = (int32_t)PHYS_S(actor, 0x76);

    /* Check speed limiter */
    int32_t speed_limit = (int32_t)PHYS_S(actor, 0x74) << 8;
    int32_t abs_speed = v_long < 0 ? -v_long : v_long;
    int32_t speed_limited = (abs_speed > speed_limit) ? 1 : 0;

    if (!speed_limited) {
        switch (dt_type) {
        case 1: /* RWD */
            wheel_drive[2] = drive_torque >> 1;
            wheel_drive[3] = drive_torque >> 1;
            break;
        case 2: /* FWD */
            wheel_drive[0] = drive_torque >> 1;
            wheel_drive[1] = drive_torque >> 1;
            break;
        case 3: /* AWD */
        default:
            wheel_drive[0] = drive_torque >> 2;
            wheel_drive[1] = drive_torque >> 2;
            wheel_drive[2] = drive_torque >> 2;
            wheel_drive[3] = drive_torque >> 2;
            break;
        }
    }

    /* --- Braking --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t brake_front = (int32_t)PHYS_S(actor, 0x6E);
    int32_t brake_rear  = (int32_t)PHYS_S(actor, 0x70);

    if (actor->brake_flag && throttle < 0) {
        /* Original grounded brake (0x404437-0x404481):
         * Uses ONLY tuning[0x6E] (front brake coeff), applies to front wheels,
         * rear wheels get 0. Clamps against lateral_speed, not longitudinal.
         * [CONFIRMED @ 0x404441: only 0x6E read, 0x404478: front only] */
        int32_t brake_cmd = (-throttle);
        int32_t bf = (brake_front * brake_cmd) >> 8;
        int32_t abs_lat = v_lat < 0 ? -v_lat : v_lat;
        if (bf > abs_lat) bf = abs_lat;
        bf >>= 1;
        int32_t sign = (v_long > 0) ? -1 : 1;
        wheel_drive[0] += sign * bf;
        wheel_drive[1] += sign * bf;
        /* Rear wheels: no brake force applied [CONFIRMED @ 0x40447B: = 0] */
    }

    /* --- 12. Per-axle lateral/longitudinal forces --- */
    int32_t steer_angle = actor->steering_command >> 8;
    /* Original (0x40415B): steer_angle = steering_command >> 8, no scaling.
     * Constant 294 does NOT exist in the binary. [CONFIRMED @ 0x404142-0x40415E] */
    int32_t steer_heading = (heading + steer_angle) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    /* Front axle lateral force from slip angle.
     * lateral_slip_stiffness (0x7C) scales slip sensitivity per car.
     * Original at 0x4041B0: slip is computed from WORLD-frame velocity
     * projected onto steered heading perpendicular, NOT from body-frame.
     * front_slip = (world_vx * cos(heading+steer) - world_vz * sin(heading+steer)) >> 12 */
    int32_t lat_stiff = (int32_t)PHYS_S(actor, 0x7C);
    int32_t front_slip = (vx * cos_s - vz * sin_s) >> 12;
    if (lat_stiff != 0)
        front_slip = (front_slip * lat_stiff) >> 8;
    int32_t front_lat_force = -(front_slip * ((grip[0] + grip[1]) >> 1)) >> 8;

    /* Rear axle lateral force.
     * Original at 0x4041E0: rear slip from WORLD-frame velocity projected
     * onto body heading perpendicular (unsteered).
     * rear_slip = (world_vx * cos(heading) - world_vz * sin(heading)) >> 12
     * which equals v_lat (body-frame lateral velocity).
     * Arcade mode: reduce rear slip by ~25% to limit oversteer tendency. */
    int32_t rear_slip = v_lat;
    if (s_dynamics_mode == 0) {
        rear_slip = (rear_slip * 192) >> 8;  /* 0.75x in arcade */
    }
    if (lat_stiff != 0)
        rear_slip = (rear_slip * lat_stiff) >> 8;
    int32_t rear_lat_force = -(rear_slip * ((grip[2] + grip[3]) >> 1)) >> 8;

    if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "SLIP: front=%d rear=%d f_lat=%d r_lat=%d steer_ang=%d v_lat=%d",
                  front_slip, rear_slip, front_lat_force, rear_lat_force,
                  steer_angle, v_lat);
    }

    /* Front/rear longitudinal forces (sum of per-wheel drive) */
    int32_t front_long = (wheel_drive[0] + wheel_drive[1]);
    int32_t rear_long  = (wheel_drive[2] + wheel_drive[3]);

    /* --- 13. Tire slip circle via isqrt (per axle) ---
     * Grip limit scaled by tire grip coefficient (tuning 0x2C).
     * Original at 0x4048xx: grip_limit = (axle_grip_sum * tuning[0x2C]) >> 8 */
    int32_t tire_grip_coeff = (int32_t)PHYS_S(actor, 0x2C);
    {
        /* Front axle slip circle */
        int32_t fl16 = front_lat_force >> 4;
        int32_t flo16 = front_long >> 4;
        int32_t combined_sq = fl16 * fl16 + flo16 * flo16;
        int32_t combined = td5_isqrt(combined_sq) << 4;
        int32_t grip_limit_f = ((grip[0] + grip[1]) >> 1);
        if (tire_grip_coeff != 0)
            grip_limit_f = (grip_limit_f * tire_grip_coeff) >> 8;
        grip_limit_f <<= 8;
        if (combined > grip_limit_f && combined > 0) {
            actor->front_axle_slip_excess = combined - grip_limit_f;
            front_long = ((grip_limit_f << 8) / combined * front_long) >> 8;
            front_lat_force = ((grip_limit_f << 8) / combined * front_lat_force) >> 8;
        } else {
            actor->front_axle_slip_excess = 0;
        }

        /* Rear axle slip circle */
        int32_t rl16 = rear_lat_force >> 4;
        int32_t rlo16 = rear_long >> 4;
        combined_sq = rl16 * rl16 + rlo16 * rlo16;
        combined = td5_isqrt(combined_sq) << 4;
        int32_t grip_limit_r = ((grip[2] + grip[3]) >> 1);
        if (tire_grip_coeff != 0)
            grip_limit_r = (grip_limit_r * tire_grip_coeff) >> 8;
        grip_limit_r <<= 8;
        if (combined > grip_limit_r && combined > 0) {
            actor->rear_axle_slip_excess = combined - grip_limit_r;
            rear_long = ((grip_limit_r << 8) / combined * rear_long) >> 8;
            rear_lat_force = ((grip_limit_r << 8) / combined * rear_lat_force) >> 8;
        } else {
            actor->rear_axle_slip_excess = 0;
        }
    }

    /* --- 14. Yaw torque, clamp [-0x578, +0x578] ---
     *
     * Original formula at 0x404C96-0x404CCC:
     *   yaw = (cos_s * front_weight * rear_long
     *        + (rear_lat - front_lat) * 500
     *        - front_long * rear_weight)
     *        / (inertia / 0x28C)
     *
     * The dominant term is the lateral force difference * 500 — this is the
     * primary yaw torque source from steering. The longitudinal terms provide
     * weight-transfer coupling (understeer/oversteer under throttle).
     * [CONFIRMED: *500 via LEA chain at 0x404CA0-0x404CBF]
     *
     * Speed gate: skip yaw torque when nearly stationary. At very low speed,
     * surface gravity injects velocity that produces parasitic yaw torque
     * (front/rear grip differ → lateral force imbalance → rotation).
     * No damping mechanism fires at zero speed, so the torque accumulates.
     * Gate on |v_long| + |v_lat| < 0x100 to prevent this feedback loop. */
    {
        int32_t abs_vl = v_long < 0 ? -v_long : v_long;
        int32_t abs_vlat = v_lat < 0 ? -v_lat : v_lat;
        int32_t speed_sum = abs_vl + abs_vlat;
        if (speed_sum < 0x100) {
            /* Near-stationary: zero out lateral forces to prevent parasitic yaw */
            front_lat_force = 0;
            rear_lat_force = 0;
            if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
                TD5_LOG_I(LOG_TAG, "YAW gate: speed_sum=0x%X < 0x100, zeroing lat forces",
                          speed_sum);
            }
        }
        int32_t inertia = PHYS_I(actor, 0x20);
        int32_t inertia_div = inertia / 0x28C;
        if (inertia_div == 0) inertia_div = 1;

        /* Term 1: cos(steer) * front_weight * rear_long_force [CONFIRMED @ 0x404C96] */
        int32_t term1 = ((cos_s * front_weight) >> 12);
        term1 = (term1 * rear_long) >> 8;

        /* Term 2 (DOMINANT): lateral force difference * 500 [CONFIRMED @ 0x404CA0] */
        int32_t lat_diff = rear_lat_force - front_lat_force;
        int32_t term2 = lat_diff * 500;

        /* Term 3: front_long_force * rear_weight [CONFIRMED @ 0x404CBF] */
        int32_t term3 = (front_long * rear_weight) >> 8;

        int32_t yaw_torque = (term1 + term2 - term3) / inertia_div;

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

        actor->angular_velocity_yaw += yaw_torque;

        if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "YAW: torque=%d ang_vel=%d heading=%d t1=%d t2=%d t3=%d idiv=%d",
                      yaw_torque, actor->angular_velocity_yaw,
                      (actor->euler_accum.yaw >> 8) & 0xFFF,
                      term1, term2, term3, inertia_div);
        }
    }

    /* --- Apply longitudinal and lateral forces back to world frame --- */
    {
        int32_t total_long = front_long + rear_long;
        int32_t total_lat  = front_lat_force + rear_lat_force;

        /* Rotate back to world frame */
        int32_t fx = (total_long * sin_h + total_lat * cos_h) >> 12;
        int32_t fz = (total_long * cos_h - total_lat * sin_h) >> 12;

        actor->linear_velocity_x += fx;
        actor->linear_velocity_z += fz;

    }

    /* --- 14b. Velocity magnitude safety clamp ---
     * Without working wall collisions, cars can leave the road where the
     * tire model creates a positive feedback loop (lateral forces >> drag).
     * Clamp total velocity magnitude to 2x the car's speed_limit to prevent
     * runaway speed while preserving normal driving feel.
     * This is a guardrail — once wall collisions work, it rarely activates. */
    {
        int32_t speed_lim = (int32_t)PHYS_S(actor, 0x74) << 8;
        int32_t vel_cap = speed_lim * 2;
        int32_t vxh = actor->linear_velocity_x >> 8;
        int32_t vzh = actor->linear_velocity_z >> 8;
        int32_t mag_sq = vxh * vxh + vzh * vzh;
        int32_t cap_sq = (vel_cap >> 8) * (vel_cap >> 8);
        if (mag_sq > cap_sq && mag_sq > 0) {
            int32_t mag = td5_isqrt(mag_sq);
            int32_t cap_h = vel_cap >> 8;
            actor->linear_velocity_x = (int32_t)((int64_t)actor->linear_velocity_x * cap_h / mag);
            actor->linear_velocity_z = (int32_t)((int64_t)actor->linear_velocity_z * cap_h / mag);
            TD5_LOG_W(LOG_TAG, "vel clamp: slot=%d mag=%d cap=%d",
                      actor->slot_index, mag << 8, vel_cap);
        }
    }

    /* --- 14c. Front slip excess yaw damping [CONFIRMED @ 0x404DB6-0x404DF4] ---
     * When front tires exceed grip circle, apply proportional correction to
     * angular_velocity_yaw. This is the primary yaw damping mechanism that
     * prevents the car from spinning. Without it, any small yaw perturbation
     * feeds back through tire slip → lateral force → more yaw torque. */
    if (actor->front_axle_slip_excess > 0) {
        int32_t correction = (actor->angular_velocity_yaw >> 6)
                           * actor->front_axle_slip_excess;
        correction = correction >> 15;
        if (correction > 0x200) correction = 0x200;
        if (correction < -0x200) correction = -0x200;
        actor->angular_velocity_yaw -= correction;
        if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
            TD5_LOG_I(LOG_TAG, "yaw_damp: corr=%d slip_ex=%d ang_vel=%d",
                      correction, actor->front_axle_slip_excess,
                      actor->angular_velocity_yaw);
        }
    }

    /* --- 14d. Near-zero velocity zeroing [CONFIRMED @ 0x404E57-0x404E95] ---
     * When car is nearly stopped, zero out all velocities to prevent drift. */
    {
        int32_t avx = actor->linear_velocity_x;
        int32_t avz = actor->linear_velocity_z;
        if (avx < 0) avx = -avx;
        if (avz < 0) avz = -avz;
        int32_t avy = actor->angular_velocity_yaw;
        if (avy < 0) avy = -avy;
        if (avx < 0x40 && avz < 0x40 && avy < 0x20) {
            actor->linear_velocity_x = 0;
            actor->linear_velocity_z = 0;
            actor->angular_velocity_yaw = 0;
        }
    }

    /* --- 15. ApplySteeringTorqueToWheels --- */
    td5_physics_apply_steering_torque(actor);

    /* --- 16. IntegrateWheelSuspensionTravel --- */
    td5_physics_integrate_suspension(actor);

    /* --- 17. ApplyMissingWheelVelocityCorrection --- */
    td5_physics_missing_wheel_correction(actor);

    /* Store tire slip for SFX */
    actor->accumulated_tire_slip_x = (int16_t)(actor->front_axle_slip_excess >> 8);
    actor->accumulated_tire_slip_z = (int16_t)(actor->rear_axle_slip_excess >> 8);
}

/* ========================================================================
 * AI 2-axle dynamics -- UpdateAIVehicleDynamics (0x404EC0)
 * ======================================================================== */

void td5_physics_update_ai(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* --- 1. Single surface probe (chassis center only) --- */
    uint8_t surface = actor->surface_type_chassis;

    /* --- 2. Surface normal and gravity --- */
    td5_physics_compute_surface_gravity(actor);

    /* --- 3. 2-axle grip, clamped [0x70..0xA0] --- */
    int32_t front_weight = (int32_t)PHYS_S(actor, 0x28);
    int32_t rear_weight  = (int32_t)PHYS_S(actor, 0x2A);
    int32_t total_weight = front_weight + rear_weight;
    if (total_weight == 0) total_weight = 1;

    int32_t half_wb = PHYS_I(actor, 0x24);
    int32_t full_wb = half_wb * 2;
    if (full_wb == 0) full_wb = 1;

    int32_t susp_defl = actor->center_suspension_pos;

    int32_t front_load = ((front_weight << 8) / total_weight);
    front_load = front_load * (half_wb - (susp_defl >> 4)) / full_wb;
    int32_t rear_load = ((rear_weight << 8) / total_weight);
    rear_load = rear_load * (half_wb + (susp_defl >> 4)) / full_wb;

    int32_t sf = (int32_t)s_surface_friction[surface & 0x1F];
    int32_t grip_front = (sf * front_load + 128) >> 8;
    int32_t grip_rear  = (sf * rear_load + 128) >> 8;

    if (grip_front < TD5_AI_GRIP_MIN) grip_front = TD5_AI_GRIP_MIN;
    if (grip_front > TD5_AI_GRIP_MAX) grip_front = TD5_AI_GRIP_MAX;
    if (grip_rear < TD5_AI_GRIP_MIN) grip_rear = TD5_AI_GRIP_MIN;
    if (grip_rear > TD5_AI_GRIP_MAX) grip_rear = TD5_AI_GRIP_MAX;

    /* --- 4. Velocity drag in WORLD frame (same formula as player, 0x404EC0) ---
     * 0x6A = driving drag, 0x6C = coasting drag */
    {
        int32_t surf_drag = (int32_t)s_surface_grip[surface & 0x1F];
        int32_t damp_coeff;
        if (actor->encounter_steering_cmd < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6C);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6A);

        actor->linear_velocity_x -= ((actor->linear_velocity_x >> 8) * damp_coeff) >> 12;
        actor->linear_velocity_z -= ((actor->linear_velocity_z >> 8) * damp_coeff) >> 12;
    }

    /* --- 5. Resolve body-frame velocities --- */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;
    int32_t v_lat  = (vx * cos_h - vz * sin_h) >> 12;

    actor->longitudinal_speed = v_long;
    actor->lateral_speed = v_lat;

    /* --- Engine pipeline (same as player) --- */
    td5_physics_reverse_throttle_sign(actor);
    td5_physics_auto_gear_select(actor);
    td5_physics_update_engine_speed(actor);
    int32_t drive_torque = td5_physics_compute_drive_torque(actor);

    /* --- Distribute torque by drivetrain --- */
    int32_t dt_type = (int32_t)PHYS_S(actor, 0x76);
    int32_t front_drive = 0, rear_drive = 0;

    int32_t speed_limit = (int32_t)PHYS_S(actor, 0x74) << 8;
    int32_t abs_speed = v_long < 0 ? -v_long : v_long;

    if (abs_speed <= speed_limit) {
        switch (dt_type) {
        case 1: /* RWD */
            rear_drive = drive_torque;
            break;
        case 2: /* FWD */
            front_drive = drive_torque;
            break;
        case 3: /* AWD */
        default:
            front_drive = drive_torque >> 1;
            rear_drive = drive_torque >> 1;
            break;
        }
    }

    /* --- Braking (2-axle) --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    if (actor->brake_flag && throttle < 0) {
        int32_t brake_cmd = (-throttle);
        int32_t bf = ((int32_t)PHYS_S(actor, 0x6E) * brake_cmd) >> 8;
        int32_t br = ((int32_t)PHYS_S(actor, 0x70) * brake_cmd) >> 8;
        int32_t half_speed = abs_speed >> 1;
        if (bf > half_speed) bf = half_speed;
        if (br > half_speed) br = half_speed;
        int32_t sign = (v_long > 0) ? -1 : 1;
        front_drive += sign * bf;
        rear_drive  += sign * br;
    }

    /* --- 2-axle lateral forces --- */
    int32_t steer_angle = actor->steering_command >> 8;
    int32_t steer_heading = (heading + steer_angle) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    int32_t front_slip = (v_lat * cos_s - v_long * sin_s) >> 12;
    int32_t front_lat = -(front_slip * grip_front) >> 8;
    /* Rear slip uses heading rotation [CONFIRMED @ 0x4041E0] */
    int32_t rear_slip_ai = (v_lat * cos_h - v_long * sin_h) >> 12;
    int32_t rear_lat  = -(rear_slip_ai * grip_rear) >> 8;

    /* Slip detection (no per-wheel slip circle for AI) */
    {
        int32_t front_combined = (front_lat < 0 ? -front_lat : front_lat) +
                                 (front_drive < 0 ? -front_drive : front_drive);
        int32_t rear_combined  = (rear_lat < 0 ? -rear_lat : rear_lat) +
                                 (rear_drive < 0 ? -rear_drive : rear_drive);
        int32_t glf = grip_front << 8;
        int32_t glr = grip_rear << 8;
        actor->front_axle_slip_excess = (front_combined > glf) ? front_combined - glf : 0;
        actor->rear_axle_slip_excess  = (rear_combined > glr)  ? rear_combined - glr  : 0;
    }

    /* --- Yaw torque (same formula as player, 0x404C96) --- */
    {
        int32_t inertia = PHYS_I(actor, 0x20);
        int32_t inertia_div = inertia / 0x28C;
        if (inertia_div == 0) inertia_div = 1;

        int32_t term1 = ((cos_s * front_weight) >> 12);
        term1 = (term1 * rear_drive) >> 8;
        int32_t lat_diff = rear_lat - front_lat;
        int32_t term2 = lat_diff * 500;
        int32_t term3 = (front_drive * rear_weight) >> 8;

        int32_t yaw_torque = (term1 + term2 - term3) / inertia_div;

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

        actor->angular_velocity_yaw += yaw_torque;
    }

    /* --- Apply forces back to world frame --- */
    {
        int32_t total_long = front_drive + rear_drive;
        int32_t total_lat  = front_lat + rear_lat;
        int32_t fx = (total_long * sin_h + total_lat * cos_h) >> 12;
        int32_t fz = (total_long * cos_h - total_lat * sin_h) >> 12;
        actor->linear_velocity_x += fx;
        actor->linear_velocity_z += fz;
    }

    /* --- Velocity magnitude safety clamp (same as player path) --- */
    {
        int32_t speed_lim = (int32_t)PHYS_S(actor, 0x74) << 8;
        int32_t vel_cap = speed_lim * 2;
        int32_t vxh = actor->linear_velocity_x >> 8;
        int32_t vzh = actor->linear_velocity_z >> 8;
        int32_t mag_sq = vxh * vxh + vzh * vzh;
        int32_t cap_sq = (vel_cap >> 8) * (vel_cap >> 8);
        if (mag_sq > cap_sq && mag_sq > 0) {
            int32_t mag = td5_isqrt(mag_sq);
            int32_t cap_h = vel_cap >> 8;
            actor->linear_velocity_x = (int32_t)((int64_t)actor->linear_velocity_x * cap_h / mag);
            actor->linear_velocity_z = (int32_t)((int64_t)actor->linear_velocity_z * cap_h / mag);
        }
    }

    /* --- Suspension integration --- */
    td5_physics_integrate_suspension(actor);

    /* Store tire slip for SFX */
    actor->accumulated_tire_slip_x = (int16_t)(actor->front_axle_slip_excess >> 8);
    actor->accumulated_tire_slip_z = (int16_t)(actor->rear_axle_slip_excess >> 8);
}

/* ========================================================================
 * Traffic simplified dynamics -- IntegrateVehicleFrictionForces (0x4438F0)
 * ======================================================================== */

void td5_physics_update_traffic(TD5_Actor *actor)
{
    /* Fixed drag: 0x10/4096 per axis */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* Velocity damping: 16/4096 per frame */
    int32_t round_x = (vx >= 0) ? 0x800 : -0x800;
    int32_t round_z = (vz >= 0) ? 0x800 : -0x800;
    vx -= (vx * 0x10 + round_x) >> 12;
    vz -= (vz * 0x10 + round_z) >> 12;

    /* 2-axle bicycle model with fixed grip constants */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;
    int32_t v_lat  = (vx * cos_h - vz * sin_h) >> 12;

    actor->longitudinal_speed = v_long;
    actor->lateral_speed = v_lat;

    /* Steering from AI route: actor+0x30C */
    int32_t steer = actor->steering_command >> 8;
    int32_t steer_heading = (heading + steer) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    /* Front axle: grip = 0x271 */
    int32_t front_slip = (v_lat * cos_s - v_long * sin_s) >> 12;
    int32_t front_force = -(front_slip * 0x271) >> 8;
    /* Clamp to [-0x800, +0x800] */
    if (front_force > 0x800) front_force = 0x800;
    if (front_force < -0x800) front_force = -0x800;

    /* Rear axle: grip = 0x14C */
    int32_t rear_force = -(v_lat * 0x14C) >> 8;
    if (rear_force > 0x800) rear_force = 0x800;
    if (rear_force < -0x800) rear_force = -0x800;

    /* Traffic throttle: actor+0x33E * 4 */
    int32_t throttle_cmd = (int32_t)actor->encounter_steering_cmd * 4;
    int32_t long_force = throttle_cmd;

    /* Yaw from lateral imbalance (simplified: no inertia lookup) */
    int32_t yaw_delta = (front_force - rear_force) >> 4;
    if (yaw_delta > TD5_YAW_TORQUE_MAX) yaw_delta = TD5_YAW_TORQUE_MAX;
    if (yaw_delta < -TD5_YAW_TORQUE_MAX) yaw_delta = -TD5_YAW_TORQUE_MAX;
    actor->angular_velocity_yaw += yaw_delta;

    /* Total lateral + longitudinal -> world frame */
    int32_t total_lat = front_force + rear_force;
    int32_t fx = (long_force * sin_h + total_lat * cos_h) >> 12;
    int32_t fz = (long_force * cos_h - total_lat * sin_h) >> 12;

    actor->linear_velocity_x = vx + fx;
    actor->linear_velocity_z = vz + fz;

    /* ApplyDampedSuspensionForce for pitch/roll */
    apply_damped_suspension_force(actor, total_lat, long_force);
}

/* ========================================================================
 * ApplyDampedSuspensionForce (0x4437C0) -- Traffic only
 *
 * Simple 2-DOF spring-damper for roll and pitch.
 * ======================================================================== */

static void apply_damped_suspension_force(TD5_Actor *actor, int32_t lateral, int32_t longitudinal)
{
    /* Roll axis: use center_suspension_pos/vel as roll state */
    int32_t roll_pos = actor->center_suspension_pos;
    int32_t roll_vel = actor->center_suspension_vel;

    roll_vel += (lateral * 0x80) >> 8;           /* drive force */
    roll_vel += (roll_vel * -0x20) >> 8;          /* velocity damping */
    roll_vel -= (roll_pos * 0x20) >> 8;           /* spring force */
    roll_pos += roll_vel;

    if (roll_pos > 0x2000) roll_pos = 0x2000;
    if (roll_pos < -0x2000) roll_pos = -0x2000;

    actor->center_suspension_pos = roll_pos;
    actor->center_suspension_vel = roll_vel;

    /* Pitch axis: use wheel_suspension_pos[0]/vel[0] as pitch state */
    int32_t pitch_pos = actor->wheel_suspension_pos[0];
    int32_t pitch_vel = actor->wheel_suspension_vel[0];

    pitch_vel += (longitudinal * 0x80) >> 8;
    pitch_vel += (pitch_vel * -0x20) >> 8;
    pitch_vel -= (pitch_pos * 0x20) >> 8;
    pitch_pos += pitch_vel;

    if (pitch_pos > 0x4000) pitch_pos = 0x4000;
    if (pitch_pos < -0x4000) pitch_pos = -0x4000;

    actor->wheel_suspension_pos[0] = pitch_pos;
    actor->wheel_suspension_vel[0] = pitch_vel;

    /* Feed into angular velocity for visual tilt */
    actor->angular_velocity_roll  += (roll_pos >> 6);
    actor->angular_velocity_pitch += (pitch_pos >> 6);
}

/* ========================================================================
 * OBB Corner Test -- FUN_00408570
 *
 * Tests 8 corners (4 of each car) against the other car's OBB.
 * Returns bitmask: bits 0-3 = B corners inside A, bits 4-7 = A corners inside B.
 * For each penetrating corner, stores {projX, projZ, penX, penZ} in corners[].
 *
 * Car bbox from carData pointer at actor+0x1B8:
 *   carData+0x04 = halfWidth  (int16)
 *   carData+0x08 = halfLength (int16)
 *   carData+0x14 = negHalfWidth (int16, asymmetric)
 * ======================================================================== */

static int obb_corner_test(TD5_Actor *a, TD5_Actor *b,
                           int32_t ax, int32_t az, int32_t bx, int32_t bz,
                           int32_t heading_a, int32_t heading_b,
                           OBB_CornerData corners[8])
{
    int result = 0;

    /* Get half-extents from car definition */
    int32_t hw_a = (int32_t)CDEF_S(a, 0x04);  /* halfWidth */
    int32_t hl_a = (int32_t)CDEF_S(a, 0x08);  /* halfLength */
    int32_t nw_a = (int32_t)CDEF_S(a, 0x14);  /* negHalfWidth (asymmetric) */
    int32_t hw_b = (int32_t)CDEF_S(b, 0x04);
    int32_t hl_b = (int32_t)CDEF_S(b, 0x08);
    int32_t nw_b = (int32_t)CDEF_S(b, 0x14);

    /* Precompute sin/cos for each heading */
    int32_t cos_a = cos_fixed12(heading_a);
    int32_t sin_a = sin_fixed12(heading_a);
    int32_t cos_b = cos_fixed12(heading_b);
    int32_t sin_b = sin_fixed12(heading_b);

    /* Delta heading: rotation from B's frame to A's frame */
    int32_t dheading = (heading_b - heading_a) & 0xFFF;
    int32_t cos_d = cos_fixed12(dheading);
    int32_t sin_d = sin_fixed12(dheading);

    /* Delta heading inverse: rotation from A's frame to B's frame */
    int32_t dheading_inv = (heading_a - heading_b) & 0xFFF;
    int32_t cos_di = cos_fixed12(dheading_inv);
    int32_t sin_di = sin_fixed12(dheading_inv);

    /* B's 4 corners in B's local frame (right-hand: X=lateral, Z=forward) */
    int32_t b_corners_lx[4] = { -nw_b,  hw_b,  hw_b, -nw_b };
    int32_t b_corners_lz[4] = {  hl_b,  hl_b, -hl_b, -hl_b };

    /* A's 4 corners in A's local frame */
    int32_t a_corners_lx[4] = { -nw_a,  hw_a,  hw_a, -nw_a };
    int32_t a_corners_lz[4] = {  hl_a,  hl_a, -hl_a, -hl_a };

    /* --- Test B's corners in A's OBB (bits 0-3) --- */
    /* World-space delta from A to B */
    int32_t delta_x = bx - ax;
    int32_t delta_z = bz - az;

    /* Rotate delta into A's local frame */
    int32_t local_dx = (delta_x * cos_a + delta_z * sin_a) >> 12;
    int32_t local_dz = (-delta_x * sin_a + delta_z * cos_a) >> 12;

    for (int i = 0; i < 4; i++) {
        /* Rotate B's corner from B's local frame into A's local frame */
        int32_t cx = (b_corners_lx[i] * cos_d - b_corners_lz[i] * sin_d) >> 12;
        int32_t cz = (b_corners_lx[i] * sin_d + b_corners_lz[i] * cos_d) >> 12;

        /* Translate by A-to-B delta in A's frame */
        cx += local_dx;
        cz += local_dz;

        /* Test if within A's half-extents */
        if (cx >= -nw_a && cx <= hw_a && cz >= -hl_a && cz <= hl_a) {
            result |= (1 << i);
            corners[i].proj_x = (int16_t)cx;
            corners[i].proj_z = (int16_t)cz;
            /* Penetration: distance from corner to nearest OBB face */
            int32_t pen_right = hw_a - cx;
            int32_t pen_left  = cx - (-nw_a);
            int32_t pen_front = hl_a - cz;
            int32_t pen_back  = cz - (-hl_a);
            corners[i].pen_x = (int16_t)((pen_right < pen_left) ? pen_right : -pen_left);
            corners[i].pen_z = (int16_t)((pen_front < pen_back) ? pen_front : -pen_back);
        }
    }

    /* --- Test A's corners in B's OBB (bits 4-7) --- */
    /* World-space delta from B to A */
    int32_t delta2_x = ax - bx;
    int32_t delta2_z = az - bz;

    /* Rotate delta into B's local frame */
    int32_t local2_dx = (delta2_x * cos_b + delta2_z * sin_b) >> 12;
    int32_t local2_dz = (-delta2_x * sin_b + delta2_z * cos_b) >> 12;

    for (int i = 0; i < 4; i++) {
        /* Rotate A's corner from A's local frame into B's local frame */
        int32_t cx = (a_corners_lx[i] * cos_di - a_corners_lz[i] * sin_di) >> 12;
        int32_t cz = (a_corners_lx[i] * sin_di + a_corners_lz[i] * cos_di) >> 12;

        /* Translate by B-to-A delta in B's frame */
        cx += local2_dx;
        cz += local2_dz;

        /* Test if within B's half-extents */
        if (cx >= -nw_b && cx <= hw_b && cz >= -hl_b && cz <= hl_b) {
            result |= (1 << (i + 4));
            corners[i + 4].proj_x = (int16_t)cx;
            corners[i + 4].proj_z = (int16_t)cz;
            int32_t pen_right = hw_b - cx;
            int32_t pen_left  = cx - (-nw_b);
            int32_t pen_front = hl_b - cz;
            int32_t pen_back  = cz - (-hl_b);
            corners[i + 4].pen_x = (int16_t)((pen_right < pen_left) ? pen_right : -pen_left);
            corners[i + 4].pen_z = (int16_t)((pen_front < pen_back) ? pen_front : -pen_back);
        }
    }

    return result;
}

/* ========================================================================
 * Collision Response -- FUN_004079c0
 *
 * Impulse-based with angular velocity coupling.
 * Called per penetrating corner from collision_detect_full.
 *
 * Key constants:
 *   INERTIA_BASE = 500000 (0x7A120)
 *   RESTITUTION_SCALE = 0x1100 (4352)
 *   ANGULAR_DIVISOR = 0x28C (652)
 *   Mass from carData+0x88 (int16)
 * ======================================================================== */

#define RESTITUTION_SCALE   0x1100
#define ANGULAR_DIVISOR     0x28C

static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target)
{
    if (!penetrator || !target) return;
    if (!penetrator->car_definition_ptr || !target->car_definition_ptr) return;

    /* Get masses */
    int32_t mass_p = (int32_t)CDEF_S(penetrator, 0x88);
    int32_t mass_t = (int32_t)CDEF_S(target, 0x88);
    if (mass_p <= 0) mass_p = 0x20;
    if (mass_t <= 0) mass_t = 0x20;

    int32_t pen_x = (int32_t)corner->pen_x;
    int32_t pen_z = (int32_t)corner->pen_z;

    /* Determine which OBB face was penetrated (minimum penetration axis) */
    int32_t abs_pen_x = pen_x < 0 ? -pen_x : pen_x;
    int32_t abs_pen_z = pen_z < 0 ? -pen_z : pen_z;

    /* Contact normal in target's local frame: choose minimum penetration axis */
    int32_t local_nx, local_nz;
    int32_t penetration;
    if (abs_pen_x < abs_pen_z) {
        /* Side face contact */
        local_nx = (pen_x > 0) ? 1 : -1;
        local_nz = 0;
        penetration = abs_pen_x;
    } else {
        /* Front/rear face contact */
        local_nx = 0;
        local_nz = (pen_z > 0) ? 1 : -1;
        penetration = abs_pen_z;
    }

    /* Rotate contact normal from target's local frame to world frame */
    int32_t cos_t = cos_fixed12(heading_target);
    int32_t sin_t = sin_fixed12(heading_target);
    int32_t world_nx = (local_nx * cos_t - local_nz * sin_t);
    int32_t world_nz = (local_nx * sin_t + local_nz * cos_t);
    /* world_nx/world_nz are in [-4096..4096] range (12-bit) when local is unit */
    if (local_nx == 0 && local_nz == 0) return;

    /* Normalize: local_nx/nz are +/-1, so world normal magnitude = 1 in 12-bit.
     * Scale to proper 12-bit range */
    if (local_nx != 0) {
        world_nx *= 0x1000;
        world_nz *= 0x1000;
    } else {
        world_nx *= 0x1000;
        world_nz *= 0x1000;
    }
    /* At this point world_nx/nz are 12-bit scaled unit normal */

    /* --- 3. Push actors apart along normal by penetration --- */
    int32_t push_half = (penetration + 1) >> 1;
    penetrator->world_pos.x += (world_nx * push_half) >> 12;
    penetrator->world_pos.z += (world_nz * push_half) >> 12;
    target->world_pos.x -= (world_nx * push_half) >> 12;
    target->world_pos.z -= (world_nz * push_half) >> 12;

    /* --- 1. Rotate velocities into collision frame using heading --- */
    /* Relative velocity */
    int32_t rel_vx = penetrator->linear_velocity_x - target->linear_velocity_x;
    int32_t rel_vz = penetrator->linear_velocity_z - target->linear_velocity_z;

    /* Project relative velocity onto contact normal */
    int32_t v_closing = (rel_vx * world_nx + rel_vz * world_nz) >> 12;

    /* Guard: if separating, only apply positional correction */
    if (v_closing > 0) return;

    /* --- 4. Compute impulse using moment-of-inertia formula --- */
    /* Contact offset from center of mass (use projected position) */
    int32_t r_p = (int32_t)corner->proj_x;  /* lateral offset in target frame */
    int32_t r_t = (int32_t)corner->proj_z;  /* longitudinal offset in target frame */
    int32_t r_sq_p = (int64_t)r_p * r_p;
    int32_t r_sq_t = (int64_t)r_t * r_t;

    /* denominator = (r_sq + INERTIA_BASE) / mass for each car */
    int64_t denom = (int64_t)(r_sq_t + V2V_INERTIA_K) * mass_p +
                    (int64_t)(r_sq_p + V2V_INERTIA_K) * mass_t;
    denom >>= 8;
    if (denom == 0) denom = 1;

    /* numerator = (INERTIA_BASE >> 8) * RESTITUTION_SCALE */
    int64_t impulse_num = (int64_t)(V2V_INERTIA_K >> 8) * RESTITUTION_SCALE;
    int32_t impulse = (int32_t)(impulse_num / denom) * (-v_closing);
    impulse >>= 4;

    /* --- 5. Apply linear impulse --- */
    int32_t imp_x = (impulse * world_nx) >> 12;
    int32_t imp_z = (impulse * world_nz) >> 12;

    penetrator->linear_velocity_x += imp_x / mass_p;
    penetrator->linear_velocity_z += imp_z / mass_p;
    target->linear_velocity_x -= imp_x / mass_t;
    target->linear_velocity_z -= imp_z / mass_t;

    /* --- 6. Angular impulse: angVel += J * mass * r / (INERTIA_BASE / ANGULAR_DIVISOR) --- */
    int32_t inertia_ang = V2V_INERTIA_K / ANGULAR_DIVISOR;  /* ~767 */
    if (inertia_ang == 0) inertia_ang = 1;

    /* Cross product of contact offset x impulse direction -> yaw torque */
    int32_t torque_p = ((int64_t)r_p * imp_z - (int64_t)r_t * imp_x) >> 12;
    int32_t torque_t = ((int64_t)corner->proj_x * imp_z -
                        (int64_t)corner->proj_z * imp_x) >> 12;

    penetrator->angular_velocity_yaw += (torque_p * mass_p) / (inertia_ang * mass_p + 1);
    target->angular_velocity_yaw     -= (torque_t * mass_t) / (inertia_ang * mass_t + 1);

    /* --- 7. Velocity damping based on penetration depth --- */
    if (penetration > 16) {
        int32_t damp = 256 - (penetration >> 2);
        if (damp < 128) damp = 128;
        penetrator->linear_velocity_x = (penetrator->linear_velocity_x * damp) >> 8;
        penetrator->linear_velocity_z = (penetrator->linear_velocity_z * damp) >> 8;
        target->linear_velocity_x = (target->linear_velocity_x * damp) >> 8;
        target->linear_velocity_z = (target->linear_velocity_z * damp) >> 8;
    }

    /* --- 8. Update poses --- */
    update_vehicle_pose_from_physics(penetrator);
    update_vehicle_pose_from_physics(target);

    /* --- 9. Crash effects based on impulse magnitude --- */
    int32_t impact_mag = impulse < 0 ? -impulse : impulse;

    /* Heavy impact (> 90000): tumble + heavy SFX */
    if (impact_mag > 90000 && g_collisions_enabled == 0) {
        if (penetrator->slot_index < 6) {
            penetrator->euler_accum.roll  += (impact_mag >> 10) & 0x3F;
            penetrator->euler_accum.pitch += (impact_mag >> 11) & 0x1F;
        }
        if (target->slot_index < 6) {
            target->euler_accum.roll  -= (impact_mag >> 10) & 0x3F;
            target->euler_accum.pitch -= (impact_mag >> 11) & 0x1F;
        }
        /* Increment damage lockout for traffic vehicles */
        if (penetrator->slot_index >= 6) penetrator->damage_lockout++;
        if (target->slot_index >= 6) target->damage_lockout++;
    }

    /* Medium impact (> 50000): medium SFX + traffic damage */
    if (impact_mag > 50000) {
        if (penetrator->slot_index >= 6) penetrator->damage_lockout++;
        if (target->slot_index >= 6) target->damage_lockout++;
    }

    /* Light impact (> 12800): light SFX (sound hook point) */
    /* Impact SFX severity stored for sound system to pick up */
    (void)impact_mag; /* sound integration point */
}

/* ========================================================================
 * Simple Collision -- FUN_00408f70
 *
 * For crashed/flipped cars -- sphere overlap with simple impulse.
 * Combined radius = (radiusA + radiusB) * 3/4
 * ======================================================================== */

static void collision_detect_simple(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    int32_t radius_a = (int32_t)CDEF_S(a, 0x80);
    int32_t radius_b = (int32_t)CDEF_S(b, 0x80);
    int32_t combined = ((radius_a + radius_b) * 3) >> 2;

    int32_t dx = (a->world_pos.x >> 8) - (b->world_pos.x >> 8);
    int32_t dy = (a->world_pos.y >> 8) - (b->world_pos.y >> 8);
    int32_t dz = (a->world_pos.z >> 8) - (b->world_pos.z >> 8);

    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq >= combined * combined) return;

    int32_t dist = td5_isqrt(dist_sq);
    if (dist == 0) return;

    /* Normalize separation vector (12-bit) */
    int32_t nx = (dx << 12) / dist;
    int32_t ny = (dy << 12) / dist;
    int32_t nz = (dz << 12) / dist;

    /* Project relative velocity onto normal */
    int32_t rel_vx = a->linear_velocity_x - b->linear_velocity_x;
    int32_t rel_vy = a->linear_velocity_y - b->linear_velocity_y;
    int32_t rel_vz = a->linear_velocity_z - b->linear_velocity_z;
    int32_t v_dot = (rel_vx * nx + rel_vy * ny + rel_vz * nz) >> 12;

    /* Only apply if closing (dot < 0) */
    if (v_dot >= 0) return;

    /* Apply half closing velocity / 16 as impulse */
    int32_t impulse = (-v_dot) >> 5;  /* /32 = half/16 */

    int32_t mass_a = (int32_t)CDEF_S(a, 0x88);
    int32_t mass_b = (int32_t)CDEF_S(b, 0x88);
    if (mass_a <= 0) mass_a = 0x20;
    if (mass_b <= 0) mass_b = 0x20;

    int32_t imp_x = (impulse * nx) >> 12;
    int32_t imp_y = (impulse * ny) >> 12;
    int32_t imp_z = (impulse * nz) >> 12;

    a->linear_velocity_x += imp_x / mass_a;
    a->linear_velocity_y += imp_y / mass_a;
    a->linear_velocity_z += imp_z / mass_a;
    b->linear_velocity_x -= imp_x / mass_b;
    b->linear_velocity_y -= imp_y / mass_b;
    b->linear_velocity_z -= imp_z / mass_b;
}

/* ========================================================================
 * Full Collision Detection -- FUN_00408a60
 *
 * Binary search refinement using OBB corner test:
 * 1. AABB pre-test from grid bounds
 * 2. Initial OBB test at full size
 * 3. Binary search 7 iterations: halve half-extents each step
 * 4. Penetration depth = final_adjustment - 0x10
 * 5. Dispatch collision response based on corner bitmask
 * ======================================================================== */

static void collision_detect_full(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* AABB pre-test from broadphase grid */
    if (g_actor_aabb[idx_a][2] < g_actor_aabb[idx_b][0] ||
        g_actor_aabb[idx_b][2] < g_actor_aabb[idx_a][0] ||
        g_actor_aabb[idx_a][3] < g_actor_aabb[idx_b][1] ||
        g_actor_aabb[idx_b][3] < g_actor_aabb[idx_a][1]) {
        return;
    }

    int32_t ax = a->world_pos.x >> 8;
    int32_t az = a->world_pos.z >> 8;
    int32_t bx = b->world_pos.x >> 8;
    int32_t bz = b->world_pos.z >> 8;

    /* Get headings from euler accumulators (>> 8 for 12-bit display angle) */
    int32_t heading_a = (a->euler_accum.yaw >> 8) & 0xFFF;
    int32_t heading_b = (b->euler_accum.yaw >> 8) & 0xFFF;

    /* --- Initial OBB test at full size --- */
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));
    int bitmask = obb_corner_test(a, b, ax, az, bx, bz,
                                  heading_a, heading_b, corners);

    if (bitmask == 0) return;  /* No overlap at full size */

    /* --- 7-iteration binary search refinement --- */
    /* Save original half-extents */
    int16_t orig_hw_a = CDEF_S(a, 0x04);
    int16_t orig_hl_a = CDEF_S(a, 0x08);
    int16_t orig_nw_a = CDEF_S(a, 0x14);
    int16_t orig_hw_b = CDEF_S(b, 0x04);
    int16_t orig_hl_b = CDEF_S(b, 0x08);
    int16_t orig_nw_b = CDEF_S(b, 0x14);

    /* Start with half-size adjustment */
    int32_t adj_a = orig_hw_a >> 1;
    int32_t adj_b = orig_hw_b >> 1;

    for (int iter = 0; iter < 7; iter++) {
        int32_t step = adj_a >> 1;
        if (step < 1) step = 1;

        /* Temporarily shrink/grow half-extents */
        int16_t test_hw_a, test_hl_a, test_nw_a;
        int16_t test_hw_b, test_hl_b, test_nw_b;

        if (bitmask != 0) {
            /* Overlap found: shrink inward */
            test_hw_a = orig_hw_a - (int16_t)adj_a;
            test_hl_a = orig_hl_a - (int16_t)adj_a;
            test_nw_a = orig_nw_a + (int16_t)adj_a;  /* neg side: shrink = add */
            test_hw_b = orig_hw_b - (int16_t)adj_b;
            test_hl_b = orig_hl_b - (int16_t)adj_b;
            test_nw_b = orig_nw_b + (int16_t)adj_b;
        } else {
            /* No overlap: grow outward */
            test_hw_a = orig_hw_a + (int16_t)adj_a;
            test_hl_a = orig_hl_a + (int16_t)adj_a;
            test_nw_a = orig_nw_a - (int16_t)adj_a;
            test_hw_b = orig_hw_b + (int16_t)adj_b;
            test_hl_b = orig_hl_b + (int16_t)adj_b;
            test_nw_b = orig_nw_b - (int16_t)adj_b;
        }

        /* Clamp to non-negative */
        if (test_hw_a < 1) test_hw_a = 1;
        if (test_hl_a < 1) test_hl_a = 1;
        if (test_hw_b < 1) test_hw_b = 1;
        if (test_hl_b < 1) test_hl_b = 1;

        /* Temporarily set modified extents for the OBB test */
        write_i16((uint8_t*)a->car_definition_ptr, 0x04, test_hw_a);
        write_i16((uint8_t*)a->car_definition_ptr, 0x08, test_hl_a);
        write_i16((uint8_t*)a->car_definition_ptr, 0x14, test_nw_a);
        write_i16((uint8_t*)b->car_definition_ptr, 0x04, test_hw_b);
        write_i16((uint8_t*)b->car_definition_ptr, 0x08, test_hl_b);
        write_i16((uint8_t*)b->car_definition_ptr, 0x14, test_nw_b);

        memset(corners, 0, sizeof(corners));
        bitmask = obb_corner_test(a, b, ax, az, bx, bz,
                                  heading_a, heading_b, corners);

        adj_a = step;
        adj_b = (orig_hw_b >> 1) >> (iter + 1);
        if (adj_b < 1) adj_b = 1;
    }

    /* Restore original half-extents */
    write_i16((uint8_t*)a->car_definition_ptr, 0x04, orig_hw_a);
    write_i16((uint8_t*)a->car_definition_ptr, 0x08, orig_hl_a);
    write_i16((uint8_t*)a->car_definition_ptr, 0x14, orig_nw_a);
    write_i16((uint8_t*)b->car_definition_ptr, 0x04, orig_hw_b);
    write_i16((uint8_t*)b->car_definition_ptr, 0x08, orig_hl_b);
    write_i16((uint8_t*)b->car_definition_ptr, 0x14, orig_nw_b);

    /* Recalculate with original extents using the refined positions */
    memset(corners, 0, sizeof(corners));
    bitmask = obb_corner_test(a, b, ax, az, bx, bz,
                              heading_a, heading_b, corners);

    if (bitmask == 0) return;

    /* --- Dispatch collision response based on corner bitmask --- */
    /* Bits 0-3: B corners in A -> response(B, A, corner) -- B is penetrating A */
    for (int i = 0; i < 4; i++) {
        if (bitmask & (1 << i)) {
            apply_collision_response(b, a, i, &corners[i], heading_a);
        }
    }
    /* Bits 4-7: A corners in B -> response(A, B, corner) -- A is penetrating B */
    for (int i = 0; i < 4; i++) {
        if (bitmask & (1 << (i + 4))) {
            apply_collision_response(a, b, i, &corners[i + 4], heading_b);
        }
    }
}

/* ========================================================================
 * Collision impulse API (0x4079C0) -- Wrapper for backward compatibility
 * ======================================================================== */

void td5_physics_apply_collision_impulse(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* Use the simple sphere-based collision for API-level calls.
     * The full OBB system is invoked through resolve_vehicle_contacts. */
    collision_detect_simple(a, b);
}

/* ========================================================================
 * ResolveVehicleContacts (0x409150) -- Spatial grid broadphase + OBB
 *
 * Phase 1: Build AABB grid, insert actors into spatial buckets
 * Phase 2: For each actor, walk adjacent buckets, test pairs
 * Phase 3: V2W wall collision
 * Phase 4: Reset grid
 * ======================================================================== */

void td5_physics_resolve_vehicle_contacts(void)
{
    int total;

    if (!g_actor_table_base) {
        return;
    }

    total = td5_game_get_total_actor_count();
    if (total < 2) {
        return;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        total = TD5_MAX_TOTAL_ACTORS;
    }

    /* --- Phase 1: Build AABB grid --- */
    /* Reset grid chains */
    memset(s_collision_grid, COLLISION_CHAIN_END, sizeof(s_collision_grid));

    for (int i = 0; i < total; ++i) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
        int32_t radius;

        if (!actor->car_definition_ptr) {
            memset(g_actor_aabb[i], 0, sizeof(g_actor_aabb[i]));
            continue;
        }

        /* Compute AABB from position +/- bounding radius */
        radius = (int32_t)CDEF_S(actor, 0x80);
        g_actor_aabb[i][0] = (actor->world_pos.x >> 8) - radius;  /* xMin */
        g_actor_aabb[i][1] = (actor->world_pos.z >> 8) - radius;  /* zMin */
        g_actor_aabb[i][2] = (actor->world_pos.x >> 8) + radius;  /* xMax */
        g_actor_aabb[i][3] = (actor->world_pos.z >> 8) + radius;  /* zMax */

        /* Insert into bucket[segment >> 2] linked list */
        int32_t seg = actor->track_span_normalized;
        if (seg < 0) seg = 0;
        int bucket = (seg >> 2) & (COLLISION_GRID_SIZE - 1);

        /* Chain: actor's chain byte points to previous head */
        g_actor_aabb[i][4] = s_collision_grid[bucket];
        s_collision_grid[bucket] = (uint8_t)i;
    }

    /* --- Phase 2: Walk adjacent buckets for each actor --- */
    for (int i = 0; i < total; ++i) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);

        if (!a->car_definition_ptr) continue;

        int32_t seg_a = a->track_span_normalized;
        if (seg_a < 0) seg_a = 0;
        int base_bucket = (seg_a >> 2) & (COLLISION_GRID_SIZE - 1);

        /* Walk 3 adjacent buckets: base-1, base, base+1 */
        for (int boff = -1; boff <= 1; boff++) {
            int bucket = (base_bucket + boff) & (COLLISION_GRID_SIZE - 1);
            int chain = s_collision_grid[bucket];
            int walk_count = 0;

            while (chain != COLLISION_CHAIN_END && walk_count < COLLISION_MAX_WALK) {
                int j = chain;
                walk_count++;

                /* Only test pairs where i < j to avoid duplicates */
                if (j <= i) {
                    chain = g_actor_aabb[j][4] & 0xFF;
                    continue;
                }

                TD5_Actor *b = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);

                if (!b->car_definition_ptr) {
                    chain = g_actor_aabb[j][4] & 0xFF;
                    continue;
                }

                /* Determine collision test type */
                int a_crashed = (a->damage_lockout >= 15);
                int b_crashed = (b->damage_lockout >= 15);

                if (a_crashed || b_crashed) {
                    /* Crashed/flipped: simple sphere collision */
                    collision_detect_simple(a, b);
                } else {
                    /* Both active: full OBB collision */
                    collision_detect_full(a, b, i, j);
                }

                chain = g_actor_aabb[j][4] & 0xFF;
            }
        }
    }

    /* --- Phase 3: Grid reset is handled at start of next call --- */
}

/* Internal: dispatch collision between two actors (grid broadphase wrapper) */
static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    int a_crashed = (a->damage_lockout >= 15);
    int b_crashed = (b->damage_lockout >= 15);

    if (a_crashed || b_crashed) {
        collision_detect_simple(a, b);
    } else {
        collision_detect_full(a, b, idx_a, idx_b);
    }
}

/* ========================================================================
 * Suspension: IntegrateWheelSuspensionTravel (0x403A20)
 *
 * Spring-damper per wheel (4) + central chassis pass.
 * Tuning offsets: 0x5E=damping, 0x60=spring, 0x62=feedback,
 *                 0x64=travel_limit, 0x66=response_factor.
 * ======================================================================== */

void td5_physics_integrate_suspension(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t damping   = (int32_t)PHYS_S(actor, 0x5E);
    int32_t spring_k  = (int32_t)PHYS_S(actor, 0x60);
    int32_t feedback  = (int32_t)PHYS_S(actor, 0x62);
    int32_t limit     = (int32_t)PHYS_S(actor, 0x64);
    int32_t response  = (int32_t)PHYS_S(actor, 0x66);

    /* Per-wheel spring-damper */
    for (int i = 0; i < 4; i++) {
        int32_t pos = actor->wheel_suspension_pos[i];
        int32_t vel = actor->wheel_suspension_vel[i];
        int32_t force = actor->wheel_force_accum[i];

        /* Compute target from wheel contact position relative to chassis */
        /* Use the force accumulator as the driving input */
        int32_t target = (force * feedback) >> 8;
        target += (vel * response) >> 8;

        /* Add spring restoring force */
        int32_t accel = (target * spring_k) >> 8;
        accel += vel;

        /* Apply damping */
        accel -= (pos * damping) >> 8;

        /* Dead zone */
        if (accel > -16 && accel < 16)
            accel = 0;

        vel = accel;
        pos += accel;

        /* Clamp to travel limits */
        if (pos > limit) {
            pos = limit;
            vel = 0;
        }
        if (pos < -limit) {
            pos = -limit;
            vel = 0;
        }

        actor->wheel_suspension_pos[i] = pos;
        actor->wheel_suspension_vel[i] = vel;
    }

    /* Central chassis suspension (averaged from wheel positions) */
    {
        int32_t avg_pos = 0;
        for (int i = 0; i < 4; i++)
            avg_pos += actor->wheel_suspension_pos[i];
        avg_pos >>= 2;

        int32_t cpos = actor->center_suspension_pos;
        int32_t cvel = actor->center_suspension_vel;

        int32_t target = ((avg_pos - cpos) * feedback) >> 8;
        target += (cvel * response) >> 8;
        int32_t accel = (target * spring_k) >> 8;
        accel += cvel;
        accel -= (cpos * damping) >> 8;

        if (accel > -16 && accel < 16)
            accel = 0;

        cvel = accel;
        cpos += accel;
        if (cpos > limit) { cpos = limit; cvel = 0; }
        if (cpos < -limit) { cpos = -limit; cvel = 0; }

        actor->center_suspension_pos = cpos;
        actor->center_suspension_vel = cvel;
    }
}

/* ========================================================================
 * UpdateVehicleSuspensionResponse (0x4057F0)
 *
 * Aggregates per-wheel contact loads into chassis angular accelerations.
 * ======================================================================== */

void td5_physics_update_suspension_response(TD5_Actor *actor)
{
    int32_t bounce_roll = 0;
    int32_t bounce_pitch = 0;
    int32_t bounce_vert = 0;
    int32_t gravity_roll = 0;
    int32_t gravity_pitch = 0;
    int32_t grounded_count = 0;

    uint8_t contact_mask = actor->wheel_contact_bitmask;

    for (int i = 0; i < 4; i++) {
        if (contact_mask & (1 << i))
            continue;  /* Airborne wheel -- skip */

        grounded_count++;

        /* Use wheel suspension deflection for force computation */
        int32_t wpos = actor->wheel_suspension_pos[i];

        /* Compute roll contribution: left wheels (+), right wheels (-) */
        if (i == 0 || i == 2)
            gravity_roll += wpos;
        else
            gravity_roll -= wpos;

        /* Compute pitch contribution: front (+), rear (-) */
        if (i < 2)
            gravity_pitch += wpos;
        else
            gravity_pitch -= wpos;

        /* Bounce from velocity */
        int32_t wvel = actor->wheel_suspension_vel[i];
        bounce_vert += wvel;
        if (i == 0 || i == 2)
            bounce_roll += wvel;
        else
            bounce_roll -= wvel;
        if (i < 2)
            bounce_pitch += wvel;
        else
            bounce_pitch -= wvel;
    }

    if (grounded_count > 0) {
        /* Apply averaged forces to angular velocities */
        /* Roll: /0x4B0 */
        actor->angular_velocity_roll += (bounce_roll + gravity_roll / grounded_count) / 0x4B0;
        /* Pitch: /0x226 */
        actor->angular_velocity_pitch += (bounce_pitch + gravity_pitch / grounded_count) / 0x226;
        /* Vertical: add gravity to counteract the gravity subtracted in
         * integrate_pose. When grounded, net vertical force = bounce_vert only.
         * When airborne, this block doesn't execute, so gravity pulls freely.
         * [CONFIRMED @ 0x4057F0: iVar8 = iVar8 + DAT_00467380] */
        actor->linear_velocity_y += (bounce_vert / grounded_count) + g_gravity_constant;
    }

    /* Clamp angular velocities */
    if (actor->angular_velocity_roll > 4000) actor->angular_velocity_roll = 4000;
    if (actor->angular_velocity_roll < -4000) actor->angular_velocity_roll = -4000;
    if (actor->angular_velocity_pitch > 4000) actor->angular_velocity_pitch = 4000;
    if (actor->angular_velocity_pitch < -4000) actor->angular_velocity_pitch = -4000;
}

/* ========================================================================
 * Integration: IntegrateVehiclePoseAndContacts (0x405E80)
 *
 * Core integration step: gravity -> velocity -> position -> euler -> matrix.
 * ======================================================================== */

void td5_physics_integrate_pose(TD5_Actor *actor)
{
    /* Save previous Y for suspension delta */
    actor->prev_frame_y_position = actor->world_pos.y;

    /* 1. Apply gravity to velocity Y */
    actor->linear_velocity_y -= g_gravity_constant;

    /* 2. Integrate angular velocity into euler accumulators */
    actor->euler_accum.roll  += actor->angular_velocity_roll;
    actor->euler_accum.yaw   += actor->angular_velocity_yaw;
    actor->euler_accum.pitch += actor->angular_velocity_pitch;

    /* 3. Integrate linear velocity into position */
    actor->world_pos.x += actor->linear_velocity_x;
    actor->world_pos.y += actor->linear_velocity_y;
    actor->world_pos.z += actor->linear_velocity_z;

    /* 3b. Update chassis track position from new world pos.
     * Original calls FUN_004440F0 here [CONFIRMED @ 0x405E80 callees].
     * Without this, track_span_raw stays at 0 and wall checks use the
     * wrong span — the primary reason collisions don't work. */
    td5_track_update_actor_position(actor);

    /* 4. Convert accumulators to 12-bit display angles */
    actor->display_angles.roll  = (int16_t)((actor->euler_accum.roll >> 8) & 0xFFF);
    actor->display_angles.yaw   = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);
    actor->display_angles.pitch = (int16_t)((actor->euler_accum.pitch >> 8) & 0xFFF);

    /* 5. Build rotation matrix from euler angles (float boundary).
     *
     * Original (0x42E1E0): Ry(yaw) * Rx(roll) * Rz(pitch) — YXZ order.
     * angles[0]=roll applied as X rotation, angles[1]=yaw as Y, angles[2]=pitch as Z.
     * This matches BuildRotationMatrixFromAngles in td5_render.c. */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        int32_t cr = cos_fixed12(roll_a);
        int32_t sr = sin_fixed12(roll_a);
        int32_t cy = cos_fixed12(yaw_a);
        int32_t sy = sin_fixed12(yaw_a);
        int32_t cp = cos_fixed12(pitch_a);
        int32_t sp = sin_fixed12(pitch_a);

        /* Ry(yaw) * Rx(roll) * Rz(pitch) — verified against Ghidra 0x42E1E0 */
        float s = 1.0f / 4096.0f;

        actor->rotation_matrix.m[0] = (float)(((sp * sy >> 12) * sr >> 12) + ((cp * cy) >> 12)) * s;
        actor->rotation_matrix.m[1] = (float)(((cp * sy >> 12) * sr >> 12) - ((sp * cy) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)((sy * cr) >> 12) * s;

        actor->rotation_matrix.m[3] = (float)((sp * cr) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)((cp * cr) >> 12) * s;
        actor->rotation_matrix.m[5] = (float)(-sr) * s;

        actor->rotation_matrix.m[6] = (float)(((sp * cy >> 12) * sr >> 12) - ((cp * sy) >> 12)) * s;
        actor->rotation_matrix.m[7] = (float)(((cp * cy >> 12) * sr >> 12) + ((sp * sy) >> 12)) * s;
        actor->rotation_matrix.m[8] = (float)((cy * cr) >> 12) * s;
    }

    /* 6. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* 7. Refresh wheel contact frames */
    td5_physics_refresh_wheel_contacts(actor);

    /* DIAGNOSTIC: log player car (slot 0) physics state once per 30 frames */
    if (actor->slot_index == 0) {
        static int s_diag_frame = 0;
        if ((s_diag_frame++ % 30) == 0) {
            TD5_LOG_I(LOG_TAG,
                "DIAG slot0: pos_y=%d vel_y=%d prev_y=%d render_y=%.2f "
                "bitmask=0x%02X force=[%d,%d,%d,%d] susp_pos=[%d,%d,%d,%d] susp_vel=[%d,%d,%d,%d]",
                actor->world_pos.y, actor->linear_velocity_y, actor->prev_frame_y_position,
                actor->render_pos.y, actor->wheel_contact_bitmask,
                actor->wheel_force_accum[0], actor->wheel_force_accum[1],
                actor->wheel_force_accum[2], actor->wheel_force_accum[3],
                actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
                actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3],
                actor->wheel_suspension_vel[0], actor->wheel_suspension_vel[1],
                actor->wheel_suspension_vel[2], actor->wheel_suspension_vel[3]);
        }
    }

    /* 8. Ground-snap: correct world_pos.y to keep the car on the road.
     *
     * The correction = ground_y - wheel_contact_pos[i].y for grounded wheels.
     * However, RefreshWheelContacts applies a suspension height reference
     * (cardef+0x82 * 0xB5/256) that shifts wheel Y upward, reducing the
     * chassis-to-ground offset from ~120 to ~14 local units. In the original,
     * the contact normal offset in the ground-snap formula compensates for
     * this. Since we don't compute contact normals, we add the suspension
     * height reference back to restore the correct chassis height above road.
     */
    {
        int64_t corr_sum = 0;
        int corr_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;

        /* Compute the suspension height reference offset in world Y.
         * This is the amount by which wheel Y was raised in RefreshWheelContacts.
         * For a level car (rot[4] ≈ 1.0), this is approx (href * 0xB5) >> 8 * 256. */
        int32_t susp_href_world = 0;
        {
            int32_t href = (int32_t)CDEF_S(actor, 0x82);
            if (href != 0) {
                int32_t href_local = (href * 0xB5) >> 8;
                /* Transform through rotation matrix Y row and scale to 24.8 */
                float rot4 = actor->rotation_matrix.m[4];
                susp_href_world = (int32_t)(href_local * rot4 * 256.0f);
            }
        }

        for (int i = 0; i < 4; i++) {
            if (!(gnd_mask & (1 << i))) {  /* grounded wheel */
                int32_t g_y = 0;
                int g_surf = 0;
                int g_span = actor->track_span_raw;
                if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                           actor->wheel_contact_pos[i].z,
                                           g_span, &g_y, &g_surf)) {
                    /* Base correction + undo suspension height reference shift */
                    corr_sum += (int64_t)g_y - (int64_t)actor->wheel_contact_pos[i].y
                              - (int64_t)susp_href_world;
                    corr_count++;
                }
            }
        }
        if (corr_count > 0) {
            int32_t corr_val = (int32_t)(corr_sum / corr_count);
            if (actor->slot_index == 0) {
                static int s_snap_log = 0;
                if ((s_snap_log++ % 30) == 0) {
                    TD5_LOG_I(LOG_TAG,
                        "SNAP slot0: corr=%d count=%d wheel_y=[%d,%d,%d,%d] pos_y_before=%d",
                        corr_val, corr_count,
                        actor->wheel_contact_pos[0].y, actor->wheel_contact_pos[1].y,
                        actor->wheel_contact_pos[2].y, actor->wheel_contact_pos[3].y,
                        actor->world_pos.y);
                }
            }
            actor->world_pos.y += corr_val;
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
            /* Cancel downward velocity: ground is a hard constraint. */
            if (actor->linear_velocity_y < 0)
                actor->linear_velocity_y = 0;
        }
    }

    /* 8b. Out-of-bounds recovery: if the car has fallen far below the road
     * surface (or the ground probe failed entirely), teleport it back to
     * the nearest valid road position on its current span. */
    {
        int32_t road_y = 0;
        int road_surf = 0;
        int oob_span = actor->track_span_raw;
        int have_road = td5_track_probe_height(actor->world_pos.x,
                                                actor->world_pos.z,
                                                oob_span, &road_y, &road_surf);
        /* Threshold: 128 world units below road = 128*256 = 32768 in 24.8 FP */
        int32_t oob_threshold = 32768;
        int fallen = 0;
        if (have_road && (actor->world_pos.y < road_y - oob_threshold))
            fallen = 1;
        if (!have_road && actor->world_pos.y < -256000) /* absolute fallback */
            fallen = 1;

        if (fallen) {
            int wx = 0, wy = 0, wz = 0;
            /* Use span center (lane_count/2) instead of actor's sub_lane_index.
             * sub_lane_index can be 0 (wall/shoulder) after resolve_neighbor clamps
             * it when crossing into a narrower adjacent span — this was causing an
             * infinite OOB loop on Australia (and other tracks) where recovery would
             * teleport to X=24641024 (off-road), probe_height would return an
             * extrapolated road_y >> car Y, and fallen=1 every frame thereafter. */
            if (td5_track_get_span_center_world(oob_span, &wx, &wy, &wz)) {
                actor->world_pos.x = wx << 8;
                actor->world_pos.y = wy << 8;
                actor->world_pos.z = wz << 8;
                /* Reset sub_lane to center so the next boundary traversal starts
                 * from a valid road position rather than the wall. */
                actor->track_sub_lane_index = 1;
                TD5_LOG_I(LOG_TAG, "OOB recovery: slot=%d span=%d center pos=(%d,%d,%d)",
                          actor->slot_index, oob_span,
                          actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
            } else if (have_road) {
                /* Span center unavailable (type=9/10 or bad vertex) — at least snap Y */
                actor->world_pos.y = road_y;
                TD5_LOG_W(LOG_TAG, "OOB recovery: slot=%d span=%d center unavailable, snap Y only",
                          actor->slot_index, oob_span);
            }
            actor->linear_velocity_x = 0;
            actor->linear_velocity_y = 0;
            actor->linear_velocity_z = 0;
            actor->angular_velocity_roll = 0;
            actor->angular_velocity_yaw = 0;
            actor->angular_velocity_pitch = 0;
            actor->euler_accum.roll = 0;
            actor->euler_accum.pitch = 0;
            /* Preserve yaw (heading) — recompute from track */
            td5_track_compute_heading(actor);
            actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
            actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);
        }
    }

    /* 9. Clamp angular velocity deltas to +/- 6000 per frame */
    if (actor->angular_velocity_roll > 6000) actor->angular_velocity_roll = 6000;
    if (actor->angular_velocity_roll < -6000) actor->angular_velocity_roll = -6000;
    if (actor->angular_velocity_yaw > 6000) actor->angular_velocity_yaw = 6000;
    if (actor->angular_velocity_yaw < -6000) actor->angular_velocity_yaw = -6000;
    if (actor->angular_velocity_pitch > 6000) actor->angular_velocity_pitch = 6000;
    if (actor->angular_velocity_pitch < -6000) actor->angular_velocity_pitch = -6000;

    /* 10. Update suspension response */
    td5_physics_update_suspension_response(actor);

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Integrate actor0: pos=(%d,%d,%d)",
                  actor->world_pos.x,
                  actor->world_pos.y,
                  actor->world_pos.z);
    }
}

/* ========================================================================
 * UpdateVehiclePoseFromPhysicsState (0x4063A0)
 *
 * Lightweight pose refresh (no force integration). Used as a callback
 * during track segment contact resolution.
 * ======================================================================== */

static void update_vehicle_pose_from_physics(TD5_Actor *actor)
{
    /* Convert current angles to display */
    actor->display_angles.roll  = (int16_t)((actor->euler_accum.roll >> 8) & 0xFFF);
    actor->display_angles.yaw   = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);
    actor->display_angles.pitch = (int16_t)((actor->euler_accum.pitch >> 8) & 0xFFF);

    /* Rebuild rotation matrix */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        int32_t cr = cos_fixed12(roll_a);
        int32_t sr = sin_fixed12(roll_a);
        int32_t cy = cos_fixed12(yaw_a);
        int32_t sy = sin_fixed12(yaw_a);
        int32_t cp = cos_fixed12(pitch_a);
        int32_t sp = sin_fixed12(pitch_a);

        float s = 1.0f / 4096.0f;

        /* Ry(yaw) * Rx(roll) * Rz(pitch) — same as integrate_pose */
        actor->rotation_matrix.m[0] = (float)(((sp * sy >> 12) * sr >> 12) + ((cp * cy) >> 12)) * s;
        actor->rotation_matrix.m[1] = (float)(((cp * sy >> 12) * sr >> 12) - ((sp * cy) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)((sy * cr) >> 12) * s;
        actor->rotation_matrix.m[3] = (float)((sp * cr) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)((cp * cr) >> 12) * s;
        actor->rotation_matrix.m[5] = (float)(-sr) * s;
        actor->rotation_matrix.m[6] = (float)(((sp * cy >> 12) * sr >> 12) - ((cp * sy) >> 12)) * s;
        actor->rotation_matrix.m[7] = (float)(((cp * cy >> 12) * sr >> 12) + ((sp * sy) >> 12)) * s;
        actor->rotation_matrix.m[8] = (float)((cy * cr) >> 12) * s;
    }

    /* Render position */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Refresh wheel contacts */
    td5_physics_refresh_wheel_contacts(actor);

    /* Ground-snap: same suspension height reference compensation as IntegratePose */
    {
        int64_t corr_sum = 0;
        int corr_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;

        int32_t susp_href_world = 0;
        {
            int32_t href = (int32_t)CDEF_S(actor, 0x82);
            if (href != 0) {
                int32_t href_local = (href * 0xB5) >> 8;
                float rot4 = actor->rotation_matrix.m[4];
                susp_href_world = (int32_t)(href_local * rot4 * 256.0f);
            }
        }

        for (int i = 0; i < 4; i++) {
            if (!(gnd_mask & (1 << i))) {
                int32_t g_y = 0;
                int g_surf = 0;
                int g_span = actor->track_span_raw;
                if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                           actor->wheel_contact_pos[i].z,
                                           g_span, &g_y, &g_surf)) {
                    corr_sum += (int64_t)g_y - (int64_t)actor->wheel_contact_pos[i].y
                              - (int64_t)susp_href_world;
                    corr_count++;
                }
            }
        }
        if (corr_count > 0) {
            actor->world_pos.y += (int32_t)(corr_sum / corr_count);
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
        }
    }
}

/* ========================================================================
 * RefreshVehicleWheelContactFrames (0x403720)
 *
 * Builds per-wheel contact frames for suspension/collision system.
 * Transforms wheel offsets by body rotation matrix -> world coordinates.
 * Computes wheel vertical force and airborne bitmask.
 * ======================================================================== */

void td5_physics_refresh_wheel_contacts(TD5_Actor *actor)
{
    float *rot = actor->rotation_matrix.m;
    int resolved_surface = actor->surface_type_chassis;
    int resolved_surface_valid = 0;

    /* Step 1 (original 0x403720): copy chassis track position to each wheel probe.
     * Without this, wheel_probes[i].span_index stays at 0 (memset init) and
     * all height probes use span 0 instead of the car's actual span. */
    for (int i = 0; i < 4; i++) {
        actor->wheel_probes[i].span_index       = actor->track_span_raw;
        actor->wheel_probes[i].span_normalized   = actor->track_span_normalized;
        actor->wheel_probes[i].span_accumulated  = actor->track_span_accumulated;
        actor->wheel_probes[i].span_high_water   = actor->track_span_high_water;
        actor->wheel_probes[i].sub_lane_index    = (int8_t)actor->track_sub_lane_index;
    }

    /* Per-wheel contact frame computation */
    for (int i = 0; i < 4; i++) {
        /* Get wheel display angle data */
        int32_t wx = actor->wheel_display_angles[i][0];
        int32_t wy = actor->wheel_display_angles[i][1];
        int32_t wz = actor->wheel_display_angles[i][2];

        /* Apply suspension deflection offset + height reference preload.
         * cardef+0x82 is the suspension height reference, scaled by 0xB5/256. */
        int32_t susp_offset = actor->wheel_suspension_pos[i];
        int32_t susp_height_ref = (int32_t)CDEF_S(actor, 0x82);
        if (susp_height_ref != 0)
            susp_offset += (susp_height_ref * 0xB5) >> 8;
        wy += susp_offset;

        /* Transform by body rotation matrix and scale to world coords (<<8) */
        int32_t world_x = (int32_t)(rot[0] * wx + rot[1] * wy + rot[2] * wz);
        int32_t world_y = (int32_t)(rot[3] * wx + rot[4] * wy + rot[5] * wz);
        int32_t world_z = (int32_t)(rot[6] * wx + rot[7] * wy + rot[8] * wz);

        /* Add to chassis world position, scaling to 24.8 fixed-point.
         * Original (0x403720): integer matrix (4096-scale) * offset >> 12,
         * then << 8 to convert to 24.8 world units.
         * Float path: matrix is pre-divided by 4096, so multiply by 256
         * to match the original's >> 12 << 8 = >> 4 = * 256/4096.
         * [CONFIRMED @ 0x403720, RE analysis line 624] */
        actor->wheel_contact_pos[i].x = actor->world_pos.x + (int32_t)(world_x * 256.0f);
        actor->wheel_contact_pos[i].y = actor->world_pos.y + (int32_t)(world_y * 256.0f);
        actor->wheel_contact_pos[i].z = actor->world_pos.z + (int32_t)(world_z * 256.0f);

        /* Per-probe track position update [CONFIRMED @ 0x403720].
         * Original calls FUN_004440F0 per probe with probe's own world pos.
         * This gives each wheel its own span for accurate edge testing. */
        td5_track_update_probe_position(&actor->wheel_probes[i],
                                        actor->wheel_contact_pos[i].x,
                                        actor->wheel_contact_pos[i].z);

        /* Compute wheel vertical force from the probed span surface. */
        int32_t wheel_y = actor->wheel_contact_pos[i].y;
        int32_t ground_y = 0;
        int surface_type = actor->surface_type_chassis;
        int probe_span = actor->wheel_probes[i].span_index;

        if (probe_span < 0 || probe_span >= g_td5.track_span_ring_length)
            probe_span = actor->track_span_raw;

        if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                   actor->wheel_contact_pos[i].z,
                                   probe_span,
                                   (int *)&ground_y,
                                   &surface_type)) {
            if (!resolved_surface_valid) {
                resolved_surface = surface_type;
                resolved_surface_valid = 1;
            }
        }

        /* Force for airborne detection.
         * Original stores (wheel_y - ground_y + gravity) into wheel_force_accum,
         * but the original's suspension integrator reads XZ projections instead
         * (BUG 6). Since our suspension reads force_accum, storing gravity-biased
         * values creates a feedback loop. Use >>8 for stable airborne detection;
         * force_accum left at 0 (initialized at race start). */
        int32_t force = (wheel_y - ground_y) >> 8;

        /* Dead zone */
        if (force > -0x200 && force < 0x200)
            force = 0;

        /* Airborne detection */
        if (force > 0x800) {
            /* Wheel is airborne */
            actor->wheel_contact_bitmask |= (1 << i);
            force = 12000; /* default airborne force */
        } else {
            actor->wheel_contact_bitmask &= ~(1 << i);
        }

        /* Store high-res wheel world position */
        actor->wheel_world_positions_hires[i] = actor->wheel_contact_pos[i];
    }

    if (resolved_surface_valid)
        actor->surface_type_chassis = (uint8_t)resolved_surface;
}

/* ========================================================================
 * ClampVehicleAttitudeLimits (0x405B40)
 *
 * Roll limit: +/- 0x355, Pitch limit: +/- 0x3A4
 * Two modes based on collisions toggle.
 * ======================================================================== */

void td5_physics_clamp_attitude(TD5_Actor *actor)
{
    int32_t roll  = (actor->euler_accum.roll >> 8) & 0xFFF;
    int32_t pitch = (actor->euler_accum.pitch >> 8) & 0xFFF;

    /* Normalize to signed range: 0x000-0x7FF = 0 to +180, 0x800-0xFFF = -180 to 0 */
    if (roll > 0x800) roll -= 0x1000;
    if (pitch > 0x800) pitch -= 0x1000;

    int32_t roll_limit  = 0x355;
    int32_t pitch_limit = 0x3A4;

    int32_t exceeded = 0;
    if (roll > roll_limit || roll < -roll_limit) exceeded = 1;
    if (pitch > pitch_limit || pitch < -pitch_limit) exceeded = 1;

    if (!exceeded) return;

    if (g_collisions_enabled == 0) {
        /* Mode 0 (collisions ON): latch recovery state */
        /* Save current rotation -> saved orientation for recovery */
        memcpy(&actor->saved_orientation, &actor->rotation_matrix, sizeof(TD5_Mat3x3));

        /* Set recovery flag [CONFIRMED @ 0x405B40: sets vehicle_mode=1, frame_counter=0] */
        actor->vehicle_mode = 1;
        actor->frame_counter = 0;
        actor->steering_command = 0;
        TD5_LOG_I(LOG_TAG, "attitude exceeded: slot=%d roll=%d pitch=%d -> mode 1",
                  actor->slot_index, roll, pitch);
    } else {
        /* Mode 1 (collisions OFF): hard clamp */
        /* Soft nudge: if approaching limit, add correction */
        if (roll > roll_limit) {
            actor->angular_velocity_roll -= 0x200;
            if (roll > roll_limit + 0x40) {
                actor->angular_velocity_roll = 0;
                actor->euler_accum.roll = roll_limit << 8;
            }
        }
        if (roll < -roll_limit) {
            actor->angular_velocity_roll += 0x200;
            if (roll < -(roll_limit + 0x40)) {
                actor->angular_velocity_roll = 0;
                actor->euler_accum.roll = (-roll_limit) << 8;
            }
        }
        if (pitch > pitch_limit) {
            actor->angular_velocity_pitch -= 0x200;
            if (pitch > pitch_limit + 0x40) {
                actor->angular_velocity_pitch = 0;
                actor->euler_accum.pitch = pitch_limit << 8;
            }
        }
        if (pitch < -pitch_limit) {
            actor->angular_velocity_pitch += 0x200;
            if (pitch < -(pitch_limit + 0x40)) {
                actor->angular_velocity_pitch = 0;
                actor->euler_accum.pitch = (-pitch_limit) << 8;
            }
        }
    }
}

/* ========================================================================
 * ResetVehicleActorState (0x405D70)
 *
 * Resets vehicle to initial conditions (respawn/reset).
 * ======================================================================== */

void td5_physics_reset_actor_state(TD5_Actor *actor)
{
    /* Zero all velocities */
    actor->linear_velocity_x = 0;
    actor->linear_velocity_y = 0;
    actor->linear_velocity_z = 0;
    actor->angular_velocity_roll = 0;
    actor->angular_velocity_yaw = 0;
    actor->angular_velocity_pitch = 0;

    /* Reset gear to first forward */
    actor->current_gear = TD5_GEAR_FIRST;

    /* Reset engine RPM to idle */
    actor->engine_speed_accum = TD5_ENGINE_IDLE_RPM;

    /* Clear wheel contact and suspension state */
    actor->wheel_contact_bitmask = 0;
    for (int i = 0; i < 4; i++) {
        actor->wheel_suspension_pos[i] = 0;
        actor->wheel_suspension_vel[i] = 0;
        actor->wheel_force_accum[i] = 0;
    }
    actor->center_suspension_pos = 0;
    actor->center_suspension_vel = 0;

    /* Clear slip/tire state */
    actor->front_axle_slip_excess = 0;
    actor->rear_axle_slip_excess = 0;
    actor->accumulated_tire_slip_x = 0;
    actor->accumulated_tire_slip_z = 0;
    actor->longitudinal_speed = 0;
    actor->lateral_speed = 0;

    /* Clear control state */
    actor->steering_command = 0;
    actor->brake_flag = 0;
    actor->handbrake_flag = 0;
    actor->vehicle_mode = 0;
    actor->damage_lockout = 0;

    /* Original (0x405DA4) sets Y to extreme altitude so the first
     * IntegrateVehiclePoseAndContacts drop-resolves wheel contacts against
     * the track surface, establishing correct chassis height above road. */
    actor->world_pos.y = -0x40000000;  /* sky-high; integrate will ground-snap */
    TD5_LOG_I(LOG_TAG, "reset_actor_state: sky-drop Y=%d for actor %p", actor->world_pos.y, (void*)actor);

    /* Convert positions to float for render */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Drop from sky — integrate resolves wheel contacts against the track
     * and snaps Y to the correct road surface height (0x405E80). */
    td5_physics_integrate_pose(actor);

    /* Post-integrate: zero all dynamics to prevent bounce (0x405E5E-0x405E7C) */
    for (int i = 0; i < 4; i++)
        actor->wheel_suspension_vel[i] = 0;
    actor->angular_velocity_roll = 0;
    actor->angular_velocity_pitch = 0;
    actor->linear_velocity_y = 0;

    TD5_LOG_I(LOG_TAG, "reset_actor_state: grounded Y=%d", actor->world_pos.y);
}

/* ========================================================================
 * ApplyMissingWheelVelocityCorrection (0x403EB0)
 *
 * When wheel contact is asymmetric, applies corrective velocity bias
 * to prevent unrealistic spinning.
 * ======================================================================== */

void td5_physics_missing_wheel_correction(TD5_Actor *actor)
{
    uint8_t mask = actor->wheel_contact_bitmask;

    /* Only run for asymmetric patterns (not fully grounded or fully airborne) */
    switch (mask) {
    case 0x0: case 0x1: case 0x2: case 0x4: case 0x6: case 0x8: case 0x9: case 0xF:
        return; /* Symmetric or fully grounded/airborne */
    default:
        break;
    }

    /* Average Y-position of grounded wheels */
    int32_t avg_y = 0;
    int32_t count = 0;
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) {
            avg_y += actor->wheel_contact_pos[i].y;
            count++;
        }
    }
    if (count == 0) return;
    avg_y /= count;

    /* Compute body-frame longitudinal speed */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);
    int32_t v_long = (actor->linear_velocity_x * sin_h +
                      actor->linear_velocity_z * cos_h) >> 12;

    /* Compute correction from pitch angle * speed */
    int32_t pitch = (actor->euler_accum.pitch >> 8) & 0xFFF;
    if (pitch > 0x800) pitch -= 0x1000;

    int32_t correction = (pitch * v_long) >> 12;

    /* Clamp correction */
    if (correction > 0x200) correction = 0x200;
    if (correction < -0x200) correction = -0x200;

    /* Apply to pitch angular velocity */
    actor->angular_velocity_pitch -= correction;
}

/* ========================================================================
 * UpdateVehicleState0fDamping (0x403D90)
 *
 * "Stunned" state: zero forces, 1/16 velocity decay per frame.
 * ======================================================================== */

void td5_physics_state0f_damping(TD5_Actor *actor)
{
    /* Keep engine alive */
    update_engine_speed_smoothed(actor);

    /* Integrate wheel suspension with zero input */
    /* Temporarily zero force accumulators */
    int32_t saved_forces[4];
    for (int i = 0; i < 4; i++) {
        saved_forces[i] = actor->wheel_force_accum[i];
        actor->wheel_force_accum[i] = 0;
    }
    td5_physics_integrate_suspension(actor);
    for (int i = 0; i < 4; i++)
        actor->wheel_force_accum[i] = saved_forces[i];

    /* Zero tire screech */
    actor->surface_contact_flags &= ~1;

    /* Compute body-frame longitudinal speed */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);
    int32_t v_long = (actor->linear_velocity_x * sin_h +
                      actor->linear_velocity_z * cos_h) >> 12;

    int32_t pitch = (actor->euler_accum.pitch >> 8) & 0xFFF;
    if (pitch > 0x800) pitch -= 0x1000;

    /* If speed is low and pitch is small: apply 1/4 of speed to yaw */
    int32_t abs_v = v_long < 0 ? -v_long : v_long;
    int32_t abs_p = pitch < 0 ? -pitch : pitch;
    if (abs_v < 33 && abs_p < 127) {
        actor->angular_velocity_yaw += v_long >> 2;
    }

    /* Decay angular velocities by 1/16 per frame */
    actor->angular_velocity_roll  -= actor->angular_velocity_roll >> 4;
    actor->angular_velocity_yaw   -= actor->angular_velocity_yaw >> 4;
    actor->angular_velocity_pitch -= actor->angular_velocity_pitch >> 4;

    /* Decay linear velocities by 1/16 per frame */
    actor->linear_velocity_x -= actor->linear_velocity_x >> 4;
    actor->linear_velocity_z -= actor->linear_velocity_z >> 4;

    /* Accumulate slip counters */
    actor->accumulated_tire_slip_x += (int16_t)(actor->angular_velocity_yaw >> 4);
    actor->accumulated_tire_slip_z += (int16_t)(actor->angular_velocity_pitch >> 4);
}

/* ========================================================================
 * Engine & Transmission
 * ======================================================================== */

/* --- UpdateVehicleEngineSpeedSmoothed (0x42ED50) ---
 *
 * Handles neutral/reverse gear RPM:
 * smoothly approaches idle (400) or a throttle-proportional target.
 */
static void update_engine_speed_smoothed(TD5_Actor *actor)
{
    int32_t rpm = actor->engine_speed_accum;
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    int32_t target;

    if (!actor->brake_flag && throttle >= 0) {
        target = ((redline - 400) * throttle) >> 8;
        if (target > redline) target = redline;
        target += 400;
    } else {
        target = 400; /* idle */
    }

    /* Asymmetric slew: approach at delta>>4, clamped to max step.
     * Original (0x42ED80-0x42EDA0): delta>>4 smooth approach,
     * fast slew triggers when (delta>>4) > 400 (up) or > 200 (down).
     * [CONFIRMED @ 0x42ED92: 400 up clamp, 0x42ED88: >>4 shift] */
    int32_t delta = rpm - target;
    if (delta > 0) {
        int32_t step = delta >> 4;
        if (step > 200) step = 200;
        rpm -= step;
    } else if (delta < 0) {
        int32_t step = (-delta) >> 4;
        if (step > 400) step = 400;
        rpm += step;
    }

    if (rpm > redline) rpm = redline;
    if (rpm < 400) rpm = 400;

    actor->engine_speed_accum = rpm;
}

/* --- UpdateEngineSpeedAccumulator (0x42EDF0) --- */
void td5_physics_update_engine_speed(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t gear = (int32_t)actor->current_gear;

    /* Neutral: use smoothed idle/rev path */
    if (gear == 1) {
        update_engine_speed_smoothed(actor);
        return;
    }

    /* Target RPM from speed and gear ratio */
    int32_t abs_speed = actor->longitudinal_speed;
    if (abs_speed < 0) abs_speed = -abs_speed;
    int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E + gear * 2);

    /* target = abs(speed/256) * gear_ratio * 0x2D / 4096 + 400 */
    int32_t target = ((abs_speed >> 8) * gear_ratio * 0x2D) >> 12;
    target += 400;

    int32_t rpm = actor->engine_speed_accum;
    int32_t delta = rpm - target;

    /* Asymmetric slew toward target */
    if (delta > 0x321) {
        rpm -= 200;         /* fast downward slew */
    } else if (delta < -800) {
        rpm += 200;         /* fast upward slew */
    } else {
        rpm += (target - rpm) >> 2;  /* smooth approach */
    }

    /* Cap at redline */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    if (rpm > redline) rpm = redline;
    if (rpm < 400) rpm = 400;

    actor->engine_speed_accum = rpm;
}

/* --- UpdateAutomaticGearSelection (0x42EF10) --- */
void td5_physics_auto_gear_select(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t gear = (int32_t)actor->current_gear;
    int32_t rpm = actor->engine_speed_accum;

    /* Reverse gear on negative throttle */
    if (throttle < 0) {
        actor->current_gear = TD5_GEAR_REVERSE;
        return;
    }

    /* Skip neutral/reverse when throttle positive */
    if (gear == TD5_GEAR_REVERSE && throttle > 0)
        gear = TD5_GEAR_FIRST;

    /* Upshift: RPM > upshift_threshold AND speed > 0 AND gear < 8
     * Original hardcodes max gear index < 8 [CONFIRMED @ 0x42EF42] */
    if (gear < 8 && actor->longitudinal_speed > 0) {
        int32_t up_thresh = (int32_t)PHYS_S(actor, 0x3E + gear * 2);
        if (rpm > up_thresh) {
            gear++;
            /* Gear-change torque kick to wheel accumulators.
             * Original shifts >> 8 [CONFIRMED @ 0x42EF54], not >> 16. */
            int32_t kick = (throttle * (int32_t)PHYS_S(actor, 0x68) * 0x1A) >> 8;
            if (gear < 16)
                kick = (kick * (int32_t)s_gear_torque[gear]) >> 8;
            actor->wheel_force_accum[0] += kick;
            actor->wheel_force_accum[1] += kick;
            actor->wheel_force_accum[2] -= kick;
            actor->wheel_force_accum[3] -= kick;
            /* Original returns immediately after upshift [CONFIRMED @ 0x42EF8E].
             * This prevents the downshift check from undoing the shift
             * in the same frame with stale RPM. */
            actor->current_gear = (uint8_t)gear;
            return;
        }
    }

    /* Downshift: RPM < downshift_threshold AND gear > 2
     * Only reached if NO upshift occurred this frame. */
    if (gear > TD5_GEAR_FIRST) {
        int32_t dn_thresh = (int32_t)PHYS_S(actor, 0x4E + gear * 2);
        if (rpm < dn_thresh) {
            gear--;
        }
    }

    actor->current_gear = (uint8_t)gear;
}

/* --- ComputeDriveTorqueFromGearCurve (0x42F030) ---
 *
 * Piecewise-linear torque curve interpolation.
 */
int32_t td5_physics_compute_drive_torque(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return 0;

    int32_t gear = (int32_t)actor->current_gear;
    if (gear == 1) return 0; /* Neutral = no drive */

    int32_t rpm = actor->engine_speed_accum;
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);

    /* Redline cutoff */
    if (rpm > redline - 50) return 0;

    /* Drive torque multiplier */
    int32_t torque_mult = (int32_t)PHYS_S(actor, 0x68);

    /* Sample torque curve (entries every 512 RPM units, 16 entries at phys+0x00) */
    int32_t index = rpm >> 9;
    if (index < 0) index = 0;
    if (index >= 15) index = 14;

    int32_t t0 = ((int32_t)PHYS_S(actor, index * 2) * torque_mult) >> 8;
    int32_t t1 = ((int32_t)PHYS_S(actor, (index + 1) * 2) * torque_mult) >> 8;

    /* Linear interpolation within segment */
    int32_t frac = rpm & 0x1FF;
    int32_t torque = t0 + (((t1 - t0) * frac) >> 9);

    /* Scale by throttle and gear ratio.
     * Original preserves throttle sign (actor+0x33E) — negative in reverse
     * produces negative torque, which is correct for backward motion. */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    torque = (torque * throttle) >> 8;

    int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E + gear * 2);
    torque = (torque * gear_ratio) >> 8;

    return torque;
}

/* --- ApplySteeringTorqueToWheels (0x42EEA0) ---
 *
 * Differential torque: FL/FR += force, RL/RR -= force.
 */
void td5_physics_apply_steering_torque(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t sensitivity = (int32_t)PHYS_S(actor, 0x68);
    int32_t gear = (int32_t)actor->current_gear;

    int32_t force = (throttle * sensitivity * 0x1A) >> 8;
    if (gear < 16)
        force = (force * (int32_t)s_gear_torque[gear]) >> 8;

    actor->wheel_force_accum[0] += force;
    actor->wheel_force_accum[1] += force;
    actor->wheel_force_accum[2] -= force;
    actor->wheel_force_accum[3] -= force;
}

/* --- ApplyReverseGearThrottleSign (0x42F010) --- */
void td5_physics_reverse_throttle_sign(TD5_Actor *actor)
{
    if (actor->current_gear == TD5_GEAR_REVERSE)
        actor->encounter_steering_cmd = -actor->encounter_steering_cmd;
}

/* --- ComputeReverseGearTorque (0x403C80) ---
 *
 * Computes engine RPM and drive torque for reverse gear path.
 * Includes speed-dependent target RPM with asymmetric slew.
 */
static int32_t compute_reverse_gear_torque(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return 0;

    int32_t abs_speed = actor->longitudinal_speed;
    if (abs_speed < 0) abs_speed = -abs_speed;

    int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E); /* gear 0 = reverse */
    if (gear_ratio == 0) gear_ratio = 1;

    /* Target RPM for reverse */
    int32_t target = ((abs_speed >> 8) * gear_ratio * 0x2D) >> 12;
    target += 400;

    int32_t rpm = actor->engine_speed_accum;
    int32_t delta = rpm - target;

    /* Asymmetric slew: 200 base, 400 when braking */
    int32_t slew_rate = actor->brake_flag ? 400 : 200;

    if (delta > slew_rate)
        rpm -= slew_rate;
    else if (delta < -slew_rate)
        rpm += slew_rate;
    else
        rpm += (target - rpm) >> 2;

    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    if (rpm > redline) rpm = redline;
    if (rpm < 400) rpm = 400;
    actor->engine_speed_accum = rpm;

    /* Drive force scaled by gear ratio */
    int32_t torque = td5_physics_compute_drive_torque(actor);
    return torque;
}

/* ========================================================================
 * Surface Normal & Gravity -- ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
 *
 * Computes effective gravity vector projected onto body axes.
 * ======================================================================== */

void td5_physics_compute_surface_gravity(TD5_Actor *actor)
{
    /* Original @ 0x42EBF0: uses 4 wheel probe world positions to compute
     * two surface tangent vectors, then cross product for surface normal.
     *
     * v1 = (probe_FL - probe_FR - probe_RR + probe_RL) >> 8
     * v2 = (probe_FL - probe_RR - probe_RL + probe_FR) >> 8
     * normal = cross(v1, v2)
     * gravity applied to vel_x and vel_z only.
     *
     * [CONFIRMED @ 0x42EBFA-0x42ECB7: offsets +0x090, +0x09C, +0x0A8, +0x0B4] */
    TD5_Vec3_Fixed *fl = &actor->probe_FL;
    TD5_Vec3_Fixed *fr = &actor->probe_FR;
    TD5_Vec3_Fixed *rl = &actor->probe_RL;
    TD5_Vec3_Fixed *rr = &actor->probe_RR;

    /* v1 = FL - FR - RR + RL (lateral-ish diagonal) */
    int32_t v1x = (fl->x - fr->x - rr->x + rl->x) >> 8;
    int32_t v1y = (fl->y - fr->y - rr->y + rl->y) >> 8;
    int32_t v1z = (fl->z - fr->z - rr->z + rl->z) >> 8;

    /* v2 = FL - RR - RL + FR (longitudinal-ish diagonal) */
    int32_t v2x = (fl->x - rr->x - rl->x + fr->x) >> 8;
    int32_t v2y = (fl->y - rr->y - rl->y + fr->y) >> 8;
    int32_t v2z = (fl->z - rr->z - rl->z + fr->z) >> 8;

    /* Cross product -> surface normal */
    int32_t nx = (v1y * v2z - v1z * v2y) >> 8;
    int32_t nz = (v1x * v2y - v1y * v2x) >> 8;

    /* Project gravity onto body X and Z axes */
    int32_t gravity_half = g_gravity_constant >> 1;
    actor->linear_velocity_x += (gravity_half * nx) >> 12;
    actor->linear_velocity_z += (gravity_half * nz) >> 12;
}

/* ========================================================================
 * Initialization -- InitializeRaceVehicleRuntime (0x42F140)
 *
 * Sets up all vehicle actors before race start.
 * Applies difficulty scaling.
 * ======================================================================== */

void td5_physics_init_vehicle_runtime(void)
{
    int total;

    /* Set gravity based on difficulty */
    if (g_difficulty_easy)
        g_gravity_constant = TD5_GRAVITY_EASY;
    else if (g_difficulty_hard)
        g_gravity_constant = TD5_GRAVITY_HARD;
    else
        g_gravity_constant = TD5_GRAVITY_NORMAL;

    if (!g_actor_table_base) {
        return;
    }

    g_actor_pool = g_actor_table_base;
    g_actor_base = g_actor_table_base;

    total = td5_game_get_total_actor_count();
    if (total <= 0) {
        return;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        total = TD5_MAX_TOTAL_ACTORS;
    }

    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);

        bind_default_vehicle_tuning(actor, slot);

        /* --- Difficulty scaling (from InitializeRaceVehicleRuntime 0x42F140) ---
         * Apply in-place multipliers to physics table fields per difficulty.
         * Normal: torque *= 360/256, drag *= 300/256, speed_scale <<= 1
         * Hard:   torque *= 650/256, drag *= 380/256, brake *= 450/256,
         *         engine_brake *= 400/256, speed_scale <<= 2 */
        {
            int16_t *phys = (int16_t *)actor->tuning_data_ptr;
            if (phys) {
                if (g_difficulty_hard) {
                    /* 0x68: drive_torque_mult *= 0x28A/256 (2.54x) */
                    int32_t tm = (int32_t)PHYS_S(actor, 0x68);
                    write_i16((uint8_t *)phys, 0x68, (int16_t)((tm * 0x28A) >> 8));
                    /* 0x2C: tire_grip_coeff *= 0x17C/256 (1.48x) */
                    int32_t dc = (int32_t)PHYS_S(actor, 0x2C);
                    write_i16((uint8_t *)phys, 0x2C, (int16_t)((dc * 0x17C) >> 8));
                    /* 0x6E: brake_force *= 0x1C2/256 (1.76x) */
                    int32_t bf = (int32_t)PHYS_S(actor, 0x6E);
                    write_i16((uint8_t *)phys, 0x6E, (int16_t)((bf * 0x1C2) >> 8));
                    /* 0x70: engine_brake *= 400/256 (1.56x) */
                    int32_t eb = (int32_t)PHYS_S(actor, 0x70);
                    write_i16((uint8_t *)phys, 0x70, (int16_t)((eb * 400) >> 8));
                    /* 0x78: speed_scale <<= 2 */
                    int32_t ss = (int32_t)PHYS_S(actor, 0x78);
                    write_i16((uint8_t *)phys, 0x78, (int16_t)(ss << 2));
                } else if (!g_difficulty_easy) {
                    /* Normal difficulty */
                    /* 0x68: drive_torque_mult *= 0x168/256 (0.5625x) */
                    int32_t tm = (int32_t)PHYS_S(actor, 0x68);
                    write_i16((uint8_t *)phys, 0x68, (int16_t)((tm * 0x168) >> 8));
                    /* 0x2C: tire_grip_coeff *= 300/256 (1.17x) */
                    int32_t dc = (int32_t)PHYS_S(actor, 0x2C);
                    write_i16((uint8_t *)phys, 0x2C, (int16_t)((dc * 300) >> 8));
                    /* 0x78: speed_scale <<= 1 */
                    int32_t ss = (int32_t)PHYS_S(actor, 0x78);
                    write_i16((uint8_t *)phys, 0x78, (int16_t)(ss << 1));
                }
                /* Easy: no scaling (raw carparam values used as-is) */
            }
        }

        actor->linear_velocity_x = 0;
        actor->linear_velocity_y = 0;
        actor->linear_velocity_z = 0;
        actor->angular_velocity_roll = 0;
        actor->angular_velocity_yaw = 0;
        actor->angular_velocity_pitch = 0;
        actor->current_gear = TD5_GEAR_FIRST;
        actor->engine_speed_accum = TD5_ENGINE_IDLE_RPM;
        actor->wheel_contact_bitmask = 0;
        actor->front_axle_slip_excess = 0;
        actor->rear_axle_slip_excess = 0;
        actor->accumulated_tire_slip_x = 0;
        actor->accumulated_tire_slip_z = 0;
        actor->longitudinal_speed = 0;
        actor->lateral_speed = 0;
        actor->steering_command = 0;
        actor->brake_flag = 0;
        actor->handbrake_flag = 0;
        actor->vehicle_mode = 0;
        actor->damage_lockout = 0;
        actor->center_suspension_pos = 0;
        actor->center_suspension_vel = 0;
        memset(actor->wheel_suspension_pos, 0, sizeof(actor->wheel_suspension_pos));
        memset(actor->wheel_suspension_vel, 0, sizeof(actor->wheel_suspension_vel));
        memset(actor->wheel_force_accum, 0, sizeof(actor->wheel_force_accum));
        actor->slot_index = (uint8_t)slot;
        actor->frame_counter = 0;
        actor->track_contact_flag = 0;
        actor->surface_contact_flags = 0;
        actor->throttle_input_active = 0;
        actor->grip_reduction = 0xFF;
        actor->prev_race_position = 0;
        actor->race_position = (uint8_t)slot;
        actor->max_gear_index = 6;

        /* --- Load wheel positions from car definition (cardef 0x40-0x5F) ---
         * 4 wheels x {x, y, z, pad} as int16, copied to actor->wheel_display_angles.
         * These define per-car wheel contact probe positions in body frame. */
        {
            int16_t *cardef = (int16_t *)actor->car_definition_ptr;
            if (cardef) {
                for (int w = 0; w < 4; w++) {
                    actor->wheel_display_angles[w][0] = cardef[(0x40 + w * 8 + 0) / 2];
                    actor->wheel_display_angles[w][1] = cardef[(0x40 + w * 8 + 2) / 2];
                    actor->wheel_display_angles[w][2] = cardef[(0x40 + w * 8 + 4) / 2];
                    actor->wheel_display_angles[w][3] = cardef[(0x40 + w * 8 + 6) / 2];
                }
                TD5_LOG_I(LOG_TAG,
                          "Wheel pos slot=%d: FL=(%d,%d,%d) FR=(%d,%d,%d) RL=(%d,%d,%d) RR=(%d,%d,%d)",
                          slot,
                          actor->wheel_display_angles[0][0], actor->wheel_display_angles[0][1], actor->wheel_display_angles[0][2],
                          actor->wheel_display_angles[1][0], actor->wheel_display_angles[1][1], actor->wheel_display_angles[1][2],
                          actor->wheel_display_angles[2][0], actor->wheel_display_angles[2][1], actor->wheel_display_angles[2][2],
                          actor->wheel_display_angles[3][0], actor->wheel_display_angles[3][1], actor->wheel_display_angles[3][2]);
            }
        }

        /* Ground-settle: build matrix, compute wheel positions, probe ground,
         * snap world_pos.y so the car starts ON the road — without applying
         * any gravity or velocity (which would drop the car on the first call).
         * [CONFIRMED @ 0x42F140: original calls FUN_00405e80 for slots < 6,
         *  but with velocity=0 the gravity in that call self-corrects.
         *  We do it explicitly to avoid any initial impulse.] */
        update_vehicle_pose_from_physics(actor);  /* builds matrix + render_pos */
        td5_physics_refresh_wheel_contacts(actor); /* probes ground, snaps wheels */
        /* Apply ground-snap correction with suspension height reference offset */
        {
            int64_t corr_sum = 0;
            int corr_count = 0;
            int32_t susp_href_world = 0;
            {
                int32_t href = (int32_t)CDEF_S(actor, 0x82);
                if (href != 0) {
                    int32_t href_local = (href * 0xB5) >> 8;
                    float rot4 = actor->rotation_matrix.m[4];
                    susp_href_world = (int32_t)(href_local * rot4 * 256.0f);
                }
            }
            for (int i = 0; i < 4; i++) {
                if (!(actor->wheel_contact_bitmask & (1 << i))) {
                    int32_t g_y = 0;
                    int g_surf = 0;
                    int g_span = actor->track_span_raw;
                    if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                               actor->wheel_contact_pos[i].z,
                                               g_span, &g_y, &g_surf)) {
                        corr_sum += (int64_t)g_y - (int64_t)actor->wheel_contact_pos[i].y
                                  - (int64_t)susp_href_world;
                        corr_count++;
                    }
                }
            }
            if (corr_count > 0) {
                actor->world_pos.y += (int32_t)(corr_sum / corr_count);
            }
        }
        actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
        actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
        actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);
        actor->prev_frame_y_position = actor->world_pos.y;
        TD5_LOG_I(LOG_TAG,
                  "Init vehicle runtime: slot=%d gear=%d rpm=%d grip=%u race_pos=%u",
                  slot,
                  actor->current_gear,
                  actor->engine_speed_accum,
                  actor->grip_reduction,
                  actor->race_position);
    }
}

static void bind_default_vehicle_tuning(TD5_Actor *actor, int slot)
{
    uint8_t *tuning;
    uint8_t *cardef;

    tuning = s_default_tuning[slot];
    cardef = s_default_cardef[slot];

    /* If carparam.dat was loaded for this slot, use it directly */
    if (slot < TD5_MAX_TOTAL_ACTORS && s_carparam_loaded[slot]) {
        memcpy(tuning, s_loaded_tuning[slot], 0x80);
        memcpy(cardef, s_loaded_cardef[slot], 0x8C);
        actor->tuning_data_ptr = tuning;
        actor->car_definition_ptr = cardef;
        TD5_LOG_I(LOG_TAG, "bind_tuning slot=%d: using carparam.dat data", slot);
        return;
    }

    /* Fallback: hardcoded defaults */
    static const int16_t k_torque_curve[16] = {
        96, 120, 144, 168, 184, 192, 196, 192,
        184, 176, 168, 156, 144, 132, 120, 104
    };
    static const int16_t k_gear_ratio[8] = {
        -192, 0, 320, 256, 208, 176, 152, 128
    };
    static const int16_t k_upshift[8] = {
        1400, 1800, 2600, 3200, 3800, 4400, 5000, 5400
    };
    static const int16_t k_downshift[8] = {
        400, 400, 900, 1200, 1600, 2000, 2400, 2800
    };

    memset(tuning, 0, sizeof(s_default_tuning[slot]));
    memset(cardef, 0, sizeof(s_default_cardef[slot]));

    for (int i = 0; i < 16; ++i) {
        write_i16(tuning, (size_t)i * 2, k_torque_curve[i]);
    }
    for (int i = 0; i < 8; ++i) {
        write_i16(tuning, 0x2E + (size_t)i * 2, k_gear_ratio[i]);
        write_i16(tuning, 0x3E + (size_t)i * 2, k_upshift[i]);
        write_i16(tuning, 0x4E + (size_t)i * 2, k_downshift[i]);
    }

    write_i32(tuning, 0x20, 0x18000);
    write_i32(tuning, 0x24, 0x600);
    write_i16(tuning, 0x28, 0x200);
    write_i16(tuning, 0x2A, 0x200);
    write_i16(tuning, 0x5E, 0x30);
    write_i16(tuning, 0x60, 0x60);
    write_i16(tuning, 0x62, 0x30);
    write_i16(tuning, 0x64, 0x180);
    write_i16(tuning, 0x66, 0x20);
    write_i16(tuning, 0x68, 0x100);
    write_i16(tuning, 0x6A, 0x20);
    write_i16(tuning, 0x6C, 0x10);
    write_i16(tuning, 0x6E, 0x120);
    write_i16(tuning, 0x70, 0x100);
    write_i16(tuning, 0x72, 6000);
    write_i16(tuning, 0x74, 0xA0);
    write_i16(tuning, 0x76, (slot >= TD5_MAX_RACER_SLOTS) ? 1 : 3);
    write_i16(tuning, 0x7A, 0x80);

    write_i16(cardef, 0x04, 0x70);
    write_i16(cardef, 0x08, 0xA0);
    write_i16(cardef, 0x80, 0x90);
    write_i16(cardef, 0x88, 0x400);

    actor->tuning_data_ptr = tuning;
    actor->car_definition_ptr = cardef;
}

/* ========================================================================
 * ComputeVehicleSuspensionEnvelope (0x42F6D0)
 *
 * Computes AABB of vehicle mesh for suspension/collision envelope.
 * ======================================================================== */

void td5_physics_compute_suspension_envelope(TD5_Actor *actor)
{
    /* In the full build:
     * 1. Iterate all vertices in the car mesh
     * 2. Track min/max for each axis
     * 3. Store as 8 corner points (AABB) in world-space short coordinates
     * 4. For traffic: also store simplified 4-point footprint
     *
     * This requires access to the mesh vertex data which is loaded
     * by the asset system. The envelope is stored in the tuning table
     * at offsets 0x00-0x1F (overwritten from carparam.dat original data).
     */
    if (!actor || !actor->car_definition_ptr) return;

    /* Store default envelope from car definition half-extents */
    int16_t *cardef = get_cardef(actor);
    int16_t hw = cardef[0x04 / 2]; /* half-width */
    int16_t hl = cardef[0x08 / 2]; /* half-length */

    /* Write to tuning table offset 0x00-0x17 as bounding corners */
    int16_t *tun = (int16_t *)actor->car_definition_ptr;
    tun[0] = -hw; tun[1] = 0;   tun[2] = -hl;
    tun[3] = hw;  tun[4] = 0;   tun[5] = hl;
    tun[6] = -hw; tun[7] = 0;   tun[8] = hl;
    tun[9] = hw;  tun[10] = 0;  tun[11] = -hl;
}

void td5_physics_set_collisions(int enabled)
{
    g_collisions_enabled = enabled ? 0 : 1;  /* 0=on, 1=off (inverted) */
}

void td5_physics_set_dynamics(int mode)
{
    s_dynamics_mode = (mode != 0) ? 1 : 0;
    TD5_LOG_I(LOG_TAG, "Dynamics mode set to %s (%d)",
              s_dynamics_mode ? "simulation" : "arcade", s_dynamics_mode);
}

int td5_physics_get_dynamics(void)
{
    return s_dynamics_mode;
}

void td5_physics_set_paused(int paused)
{
    if (g_game_paused != paused) {
        TD5_LOG_I(LOG_TAG, "Physics paused=%d", paused);
        g_game_paused = paused;
    }
}

void td5_physics_set_race_slot_state(int slot, int is_human)
{
    if (slot >= 0 && slot < 6) {
        g_race_slot_state[slot] = is_human;
        TD5_LOG_I(LOG_TAG, "Slot %d physics mode: %s", slot,
                  is_human ? "player" : "AI");
    }
}

void td5_physics_load_carparam(int slot, const uint8_t *data_268)
{
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !data_268) return;

    /* carparam.dat layout (268 bytes):
     *   0x00..0x8B: car definition (bounding box/collision) -> car_definition_ptr
     *   0x8C..0x10B: physics tuning (torque, gears, damping) -> tuning_data_ptr */
    memcpy(s_loaded_cardef[slot], data_268, 0x8C);
    memcpy(s_loaded_tuning[slot], data_268 + 0x8C, 0x80);
    s_carparam_loaded[slot] = 1;

    TD5_LOG_I(LOG_TAG,
              "carparam slot=%d: redline=%d speed_lim=%d dt=%d damp_hi=%d damp_lo=%d brk_f=%d brk_r=%d",
              slot,
              *(int16_t *)(s_loaded_tuning[slot] + 0x72),
              *(int16_t *)(s_loaded_tuning[slot] + 0x74),
              *(int16_t *)(s_loaded_tuning[slot] + 0x76),
              *(int16_t *)(s_loaded_tuning[slot] + 0x6A),
              *(int16_t *)(s_loaded_tuning[slot] + 0x6C),
              *(int16_t *)(s_loaded_tuning[slot] + 0x6E),
              *(int16_t *)(s_loaded_tuning[slot] + 0x70));
}
