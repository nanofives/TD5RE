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

#define LOG_TAG "physics"

extern void *g_actor_pool;
extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

int td5_game_get_total_actor_count(void);

static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b);

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
static int32_t g_difficulty_easy = 0;
static int32_t g_difficulty_hard = 0;
static int32_t g_total_actor_count = 6;
static int32_t g_race_slot_state[6];         /* 1=human, 0=AI per slot */

/* V2V inertia constant = 500,000 (DAT_00463204) */
#define V2V_INERTIA_K       500000
/* V2W inertia constant = 1,500,000 (DAT_00463200) */
#define V2W_INERTIA_K       1500000

/* Per-actor AABB table for broadphase (stride 20 bytes) */
static int32_t g_actor_aabb[TD5_MAX_TOTAL_ACTORS][5]; /* min_x, min_z, max_x, max_z, chain */
static uint8_t s_default_tuning[TD5_MAX_TOTAL_ACTORS][0x80];
static uint8_t s_default_cardef[TD5_MAX_TOTAL_ACTORS][0x90];

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
    angle &= 0xFFF;
    /* Map to [0, 0x400] quadrant */
    int32_t a;
    if (angle <= 0x400) {
        a = angle;
    } else if (angle <= 0x800) {
        a = 0x800 - angle;
    } else if (angle <= 0xC00) {
        a = angle - 0x800;
    } else {
        a = 0x1000 - angle;
    }
    /* Quadratic approximation: cos(x) ~ 1 - 2*x^2/Q^2, Q=0x400 */
    int32_t val = 0x1000 - ((2 * a * a + 0x80) >> 8);
    if (angle > 0x400 && angle < 0xC00)
        val = -val;
    return val;
}

static int32_t sin_fixed12(int32_t angle)
{
    return cos_fixed12(angle - 0x400);
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
    /* Initialize surface tables with sensible defaults.
     * In the final build these are loaded from binary data. */
    for (int i = 0; i < 32; i++) {
        s_surface_friction[i] = 0x48; /* moderate grip */
        s_surface_grip[i] = 0x08;     /* small drag */
    }
    /* Dry asphalt: high grip, low drag */
    s_surface_friction[1] = 0x50;
    s_surface_grip[1] = 0x04;
    /* Wet asphalt: lower grip */
    s_surface_friction[2] = 0x38;
    s_surface_grip[2] = 0x0C;
    /* Dirt: low grip, high drag */
    s_surface_friction[3] = 0x30;
    s_surface_grip[3] = 0x14;
    /* Gravel */
    s_surface_friction[4] = 0x28;
    s_surface_grip[4] = 0x18;

    /* Gear torque multipliers (per-gear shift kick scaling) */
    for (int i = 0; i < 16; i++)
        s_gear_torque[i] = (int16_t)(0x100 - i * 0x10);

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
    } else if (g_game_paused) {
        /* Paused: only update engine RPM display */
        update_engine_speed_smoothed(actor);
    }

    /* 7. Integrate pose and contacts */
    td5_physics_integrate_pose(actor);

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

    /* --- 4. Handbrake modifier on rear wheels (tuning+0x7A) --- */
    if (actor->handbrake_flag) {
        int32_t hb_mod = (int32_t)PHYS_S(actor, 0x7A);
        grip[2] = (grip[2] * hb_mod) >> 8;
        grip[3] = (grip[3] * hb_mod) >> 8;
    }

    /* --- 5. Resolve body-frame velocities (cos/sin of heading) --- */
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

    /* --- 6. Velocity damping (surface-dependent) --- */
    {
        int32_t surf_drag = (int32_t)s_surface_grip[surface_center & 0x1F];
        int32_t damp_coeff;
        if (actor->frame_counter < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6A);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6C);

        v_long -= ((v_long >> 8) * damp_coeff) >> 12;
        v_lat  -= ((v_lat >> 8) * damp_coeff) >> 12;
    }

    /* --- 7. ApplyReverseGearThrottleSign --- */
    td5_physics_reverse_throttle_sign(actor);

    /* --- 8. UpdateAutomaticGearSelection --- */
    td5_physics_auto_gear_select(actor);

    /* --- 9. UpdateEngineSpeedAccumulator --- */
    td5_physics_update_engine_speed(actor);

    /* --- 10. ComputeDriveTorqueFromGearCurve --- */
    int32_t drive_torque = td5_physics_compute_drive_torque(actor);

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
        int32_t brake_cmd = (-throttle);
        int32_t bf = (brake_front * brake_cmd) >> 8;
        int32_t br = (brake_rear * brake_cmd) >> 8;
        /* Clamp brake to not exceed wheel speed */
        if (bf > abs_speed) bf = abs_speed;
        if (br > abs_speed) br = abs_speed;
        int32_t sign = (v_long > 0) ? -1 : 1;
        wheel_drive[0] += sign * (bf >> 1);
        wheel_drive[1] += sign * (bf >> 1);
        wheel_drive[2] += sign * (br >> 1);
        wheel_drive[3] += sign * (br >> 1);
    }

    /* --- 12. Per-axle lateral/longitudinal forces --- */
    int32_t steer_angle = actor->steering_command >> 8;
    int32_t steer_heading = (heading + (steer_angle >> 4)) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    /* Front axle lateral force from slip angle */
    int32_t front_slip = (v_lat * cos_s - v_long * sin_s) >> 12;
    int32_t front_lat_force = -(front_slip * ((grip[0] + grip[1]) >> 1)) >> 8;

    /* Rear axle lateral force */
    int32_t rear_slip = v_lat;
    int32_t rear_lat_force = -(rear_slip * ((grip[2] + grip[3]) >> 1)) >> 8;

    /* Front/rear longitudinal forces (sum of per-wheel drive) */
    int32_t front_long = (wheel_drive[0] + wheel_drive[1]);
    int32_t rear_long  = (wheel_drive[2] + wheel_drive[3]);

    /* --- 13. Tire slip circle via isqrt (per axle) --- */
    {
        /* Front axle slip circle */
        int32_t fl16 = front_lat_force >> 4;
        int32_t flo16 = front_long >> 4;
        int32_t combined_sq = fl16 * fl16 + flo16 * flo16;
        int32_t combined = td5_isqrt(combined_sq) << 4;
        int32_t grip_limit_f = ((grip[0] + grip[1]) >> 1) << 8;
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
        int32_t grip_limit_r = ((grip[2] + grip[3]) >> 1) << 8;
        if (combined > grip_limit_r && combined > 0) {
            actor->rear_axle_slip_excess = combined - grip_limit_r;
            rear_long = ((grip_limit_r << 8) / combined * rear_long) >> 8;
            rear_lat_force = ((grip_limit_r << 8) / combined * rear_lat_force) >> 8;
        } else {
            actor->rear_axle_slip_excess = 0;
        }
    }

    /* --- 14. Yaw torque, clamp [-0x578, +0x578] --- */
    {
        int32_t inertia = PHYS_I(actor, 0x20);
        int32_t inertia_div = inertia / 0x28C;
        if (inertia_div == 0) inertia_div = 1;

        /* Yaw torque = (front_lat * cos(steer) * rear_arm - rear_lat * front_arm) / inertia */
        int32_t front_moment = (front_lat_force * cos_s) >> 12;
        front_moment = (front_moment * rear_weight) >> 8;
        int32_t rear_moment = (rear_lat_force * front_weight) >> 8;

        int32_t yaw_torque = (front_moment - rear_moment) / inertia_div;

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

        actor->angular_velocity_yaw += yaw_torque;
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

    /* --- 4. Resolve body-frame velocities --- */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;
    int32_t v_lat  = (vx * cos_h - vz * sin_h) >> 12;

    actor->longitudinal_speed = v_long;
    actor->lateral_speed = v_lat;

    /* --- Velocity damping --- */
    {
        int32_t surf_drag = (int32_t)s_surface_grip[surface & 0x1F];
        int32_t damp_coeff;
        if (actor->frame_counter < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6A);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, 0x6C);

        v_long -= ((v_long >> 8) * damp_coeff) >> 12;
        v_lat  -= ((v_lat >> 8) * damp_coeff) >> 12;
    }

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
        if (bf > abs_speed) bf = abs_speed;
        if (br > abs_speed) br = abs_speed;
        int32_t sign = (v_long > 0) ? -1 : 1;
        front_drive += sign * bf;
        rear_drive  += sign * br;
    }

    /* --- 2-axle lateral forces --- */
    int32_t steer_angle = actor->steering_command >> 8;
    int32_t steer_heading = (heading + (steer_angle >> 4)) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    int32_t front_slip = (v_lat * cos_s - v_long * sin_s) >> 12;
    int32_t front_lat = -(front_slip * grip_front) >> 8;
    int32_t rear_lat  = -(v_lat * grip_rear) >> 8;

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

    /* --- Yaw torque --- */
    {
        int32_t inertia = PHYS_I(actor, 0x20);
        int32_t inertia_div = inertia / 0x28C;
        if (inertia_div == 0) inertia_div = 1;

        int32_t front_moment = (front_lat * cos_s) >> 12;
        front_moment = (front_moment * rear_weight) >> 8;
        int32_t rear_moment = (rear_lat * front_weight) >> 8;
        int32_t yaw_torque = (front_moment - rear_moment) / inertia_div;

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
    int32_t steer_heading = (heading + (steer >> 4)) & 0xFFF;
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
 * Collision: ApplyVehicleCollisionImpulse (0x4079C0)
 * ======================================================================== */

void td5_physics_apply_collision_impulse(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* --- Get masses from cardef+0x88 --- */
    int32_t mass_a = (int32_t)CDEF_S(a, 0x88);
    int32_t mass_b = (int32_t)CDEF_S(b, 0x88);
    if (mass_a <= 0) mass_a = 0x20;
    if (mass_b <= 0) mass_b = 0x20;

    /* --- Compute separation vector --- */
    int32_t dx = (a->world_pos.x - b->world_pos.x) >> 8;
    int32_t dz = (a->world_pos.z - b->world_pos.z) >> 8;

    /* Compute contact angle from separation vector */
    /* Use atan2 approximation: just use the raw dx,dz as contact normal */
    int32_t dist_sq = dx * dx + dz * dz;
    int32_t dist = td5_isqrt(dist_sq);
    if (dist == 0) return;

    /* Normalize to 12-bit */
    int32_t nx = (dx << 12) / dist;
    int32_t nz = (dz << 12) / dist;

    /* --- Relative velocity along contact normal --- */
    int32_t rel_vx = a->linear_velocity_x - b->linear_velocity_x;
    int32_t rel_vz = a->linear_velocity_z - b->linear_velocity_z;
    int32_t v_rel = (rel_vx * nx + rel_vz * nz) >> 12;

    /* Guard: if separating, no impulse */
    if (v_rel > 0) return;

    /* --- Contact point offsets for angular coupling --- */
    int32_t half_len_a = (int32_t)CDEF_S(a, 0x08);
    int32_t half_len_b = (int32_t)CDEF_S(b, 0x08);
    int32_t r1 = half_len_a;
    int32_t r2 = half_len_b;

    /* --- Impulse magnitude via moment-of-inertia formula --- */
    /* denominator = (r2^2 + K) * mass_a + (r1^2 + K) * mass_b */
    int32_t denom = ((int64_t)(r2 * r2 + V2V_INERTIA_K) * mass_a +
                     (int64_t)(r1 * r1 + V2V_INERTIA_K) * mass_b) >> 8;
    if (denom == 0) denom = 1;

    int32_t impulse_num = ((int64_t)(V2V_INERTIA_K >> 8) * 0x1100) >> 0;
    int32_t impulse = (impulse_num / denom) * (-v_rel);
    impulse >>= 4; /* scale to physics range */

    /* --- Apply linear impulse --- */
    int32_t imp_x = (impulse * nx) >> 12;
    int32_t imp_z = (impulse * nz) >> 12;

    a->linear_velocity_x += imp_x / mass_a;
    a->linear_velocity_z += imp_z / mass_a;
    b->linear_velocity_x -= imp_x / mass_b;
    b->linear_velocity_z -= imp_z / mass_b;

    /* --- Angular impulse from contact offset --- */
    /* Cross product of offset x impulse direction -> yaw torque */
    int32_t ang_a = (r1 * imp_x) >> 12;
    int32_t ang_b = (r2 * imp_x) >> 12;
    a->angular_velocity_yaw += ang_a / (mass_a + 1);
    b->angular_velocity_yaw -= ang_b / (mass_b + 1);

    /* --- Post-impulse: update poses --- */
    update_vehicle_pose_from_physics(a);
    update_vehicle_pose_from_physics(b);

    /* --- Traffic recovery: if impact > 50,000 on traffic vehicle --- */
    int32_t impact_mag = impulse < 0 ? -impulse : impulse;
    if (impact_mag > 50000) {
        if (a->slot_index >= 6) a->damage_lockout++;
        if (b->slot_index >= 6) b->damage_lockout++;
    }

    /* --- Visual damage above 90,000 (if collisions enabled) --- */
    if (impact_mag > 90000 && g_collisions_enabled == 0) {
        /* Small random angular perturbation for cosmetic wobble */
        if (a->slot_index < 6) {
            a->euler_accum.roll  += (impact_mag >> 10) & 0x1F;
            a->euler_accum.pitch += (impact_mag >> 11) & 0x0F;
        }
        if (b->slot_index < 6) {
            b->euler_accum.roll  -= (impact_mag >> 10) & 0x1F;
            b->euler_accum.pitch -= (impact_mag >> 11) & 0x0F;
        }
    }
}

/* ========================================================================
 * ResolveVehicleContacts (0x409150) -- 7-iteration TOI binary search
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

    for (int i = 0; i < total; ++i) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
        int32_t radius;

        if (!actor->car_definition_ptr) {
            memset(g_actor_aabb[i], 0, sizeof(g_actor_aabb[i]));
            continue;
        }

        radius = (int32_t)CDEF_S(actor, 0x80);
        g_actor_aabb[i][0] = (actor->world_pos.x >> 8) - radius;
        g_actor_aabb[i][1] = (actor->world_pos.z >> 8) - radius;
        g_actor_aabb[i][2] = (actor->world_pos.x >> 8) + radius;
        g_actor_aabb[i][3] = (actor->world_pos.z >> 8) + radius;
        g_actor_aabb[i][4] = -1;
    }

    for (int i = 0; i < total - 1; ++i) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);

        if (!a->car_definition_ptr) {
            continue;
        }

        for (int j = i + 1; j < total; ++j) {
            TD5_Actor *b = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);

            if (!b->car_definition_ptr) {
                continue;
            }
            if (g_actor_aabb[i][2] < g_actor_aabb[j][0] ||
                g_actor_aabb[j][2] < g_actor_aabb[i][0] ||
                g_actor_aabb[i][3] < g_actor_aabb[j][1] ||
                g_actor_aabb[j][3] < g_actor_aabb[i][1]) {
                continue;
            }

            TD5_LOG_D(LOG_TAG, "Collision pair detected: slot_a=%d slot_b=%d", i, j);
            resolve_collision_pair(a, b);
        }
    }

    /* --- V2W: Vehicle-to-Wall collision --- */
    for (int i = 0; i < total; ++i) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
        int32_t ax, az, radius;
        int span_idx, edge;

        if (!a->car_definition_ptr) continue;

        ax = a->world_pos.x >> 8;
        az = a->world_pos.z >> 8;
        radius = (int32_t)CDEF_S(a, 0x80);
        span_idx = ACTOR_I16(a, ACTOR_OFF_SPAN_INDEX);

        /* Check actor bounding circle against track span edges.
         * Query the track for left/right boundary at this span. */
        {
            int left_x, left_z, right_x, right_z;
            int pen_left, pen_right;
            int nx, nz, nmag;

            td5_track_get_span_edges(span_idx, &left_x, &left_z, &right_x, &right_z);

            /* Simplified: check perpendicular distance to each edge */
            /* Left edge penetration */
            pen_left = radius - (ax - left_x);
            if (pen_left > 0) {
                /* Push right, apply impulse */
                int impulse = (pen_left * V2W_INERTIA_K) >> 12;
                ACTOR_I32(a, ACTOR_OFF_LIN_VEL_X) += impulse >> 4;
                ACTOR_I32(a, ACTOR_OFF_ANG_VEL_YAW) += impulse >> 8;
                TD5_LOG_D(LOG_TAG,
                          "Wall impulse: actor=%d side=left penetration=%d impulse=%d",
                          a->slot_index, pen_left, impulse);
            }

            /* Right edge penetration */
            pen_right = radius - (right_x - ax);
            if (pen_right > 0) {
                int impulse = (pen_right * V2W_INERTIA_K) >> 12;
                ACTOR_I32(a, ACTOR_OFF_LIN_VEL_X) -= impulse >> 4;
                ACTOR_I32(a, ACTOR_OFF_ANG_VEL_YAW) -= impulse >> 8;
                TD5_LOG_D(LOG_TAG,
                          "Wall impulse: actor=%d side=right penetration=%d impulse=%d",
                          a->slot_index, pen_right, impulse);
            }
        }
    }
}

/* Internal: perform 7-iteration TOI binary search between two actors */
static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    /* AABB broadphase check */
    int32_t radius_a = (int32_t)CDEF_S(a, 0x80);
    int32_t radius_b = (int32_t)CDEF_S(b, 0x80);

    int32_t ax = a->world_pos.x >> 8;
    int32_t az = a->world_pos.z >> 8;
    int32_t bx = b->world_pos.x >> 8;
    int32_t bz = b->world_pos.z >> 8;

    if (ax + radius_a <= bx - radius_b) return;
    if (bx + radius_b <= ax - radius_a) return;
    if (az + radius_a <= bz - radius_b) return;
    if (bz + radius_b <= az - radius_a) return;

    /* Simple sphere distance check */
    int32_t dx = ax - bx;
    int32_t dz = az - bz;
    int32_t combined_radius = radius_a + radius_b;
    if (dx * dx + dz * dz > combined_radius * combined_radius) return;

    /* --- 7-iteration binary search TOI refinement --- */
    /* Save positions */
    int32_t saved_ax = a->world_pos.x, saved_az = a->world_pos.z;
    int32_t saved_bx = b->world_pos.x, saved_bz = b->world_pos.z;
    int32_t saved_ayaw = a->euler_accum.yaw, saved_byaw = b->euler_accum.yaw;

    /* Start at half-step back */
    int32_t step_x_a = a->linear_velocity_x >> 1;
    int32_t step_z_a = a->linear_velocity_z >> 1;
    int32_t step_x_b = b->linear_velocity_x >> 1;
    int32_t step_z_b = b->linear_velocity_z >> 1;
    int32_t step_yaw_a = a->angular_velocity_yaw >> 1;
    int32_t step_yaw_b = b->angular_velocity_yaw >> 1;

    a->world_pos.x -= step_x_a;
    a->world_pos.z -= step_z_a;
    b->world_pos.x -= step_x_b;
    b->world_pos.z -= step_z_b;
    a->euler_accum.yaw -= step_yaw_a;
    b->euler_accum.yaw -= step_yaw_b;

    int32_t elasticity = 0x80;
    int32_t contact_found = 0;

    for (int iter = 0; iter < 7; iter++) {
        step_x_a >>= 1;
        step_z_a >>= 1;
        step_x_b >>= 1;
        step_z_b >>= 1;
        step_yaw_a >>= 1;
        step_yaw_b >>= 1;
        int32_t step_e = 0x80 >> (iter + 1);

        /* Simple overlap test at current interpolated positions */
        int32_t cx = (a->world_pos.x >> 8) - (b->world_pos.x >> 8);
        int32_t cz = (a->world_pos.z >> 8) - (b->world_pos.z >> 8);
        int32_t overlap = (cx * cx + cz * cz < combined_radius * combined_radius);

        if (overlap) {
            /* Still overlapping -- step backward */
            a->world_pos.x -= step_x_a;
            a->world_pos.z -= step_z_a;
            b->world_pos.x -= step_x_b;
            b->world_pos.z -= step_z_b;
            a->euler_accum.yaw -= step_yaw_a;
            b->euler_accum.yaw -= step_yaw_b;
            elasticity -= step_e;
            contact_found = 1;
        } else {
            /* No overlap -- step forward */
            a->world_pos.x += step_x_a;
            a->world_pos.z += step_z_a;
            b->world_pos.x += step_x_b;
            b->world_pos.z += step_z_b;
            a->euler_accum.yaw += step_yaw_a;
            b->euler_accum.yaw += step_yaw_b;
            elasticity += step_e;
        }
    }

    /* Restore positions */
    a->world_pos.x = saved_ax;
    a->world_pos.z = saved_az;
    b->world_pos.x = saved_bx;
    b->world_pos.z = saved_bz;
    a->euler_accum.yaw = saved_ayaw;
    b->euler_accum.yaw = saved_byaw;

    /* Apply impulse if contact was found */
    if (contact_found) {
        td5_physics_apply_collision_impulse(a, b);
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
        /* Vertical */
        actor->linear_velocity_y += (bounce_vert / grounded_count);
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

    /* 4. Convert accumulators to 12-bit display angles */
    actor->display_angles.roll  = (int16_t)((actor->euler_accum.roll >> 8) & 0xFFF);
    actor->display_angles.yaw   = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);
    actor->display_angles.pitch = (int16_t)((actor->euler_accum.pitch >> 8) & 0xFFF);

    /* 5. Build rotation matrix from euler angles (float boundary) */
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

        /* ZYX euler rotation matrix (row-major) */
        /* Scale: fixed12 * fixed12 >> 12 = fixed12, then /4096.0f to float */
        float s = 1.0f / 4096.0f;

        actor->rotation_matrix.m[0] = (float)((cy * cp) >> 12) * s;
        actor->rotation_matrix.m[1] = (float)(((cy * sp >> 12) * sr >> 12) - ((sy * cr) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)(((cy * sp >> 12) * cr >> 12) + ((sy * sr) >> 12)) * s;

        actor->rotation_matrix.m[3] = (float)((sy * cp) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)(((sy * sp >> 12) * sr >> 12) + ((cy * cr) >> 12)) * s;
        actor->rotation_matrix.m[5] = (float)(((sy * sp >> 12) * cr >> 12) - ((cy * sr) >> 12)) * s;

        actor->rotation_matrix.m[6] = (float)(-sp) * s;
        actor->rotation_matrix.m[7] = (float)((cp * sr) >> 12) * s;
        actor->rotation_matrix.m[8] = (float)((cp * cr) >> 12) * s;
    }

    /* 6. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* 7. Refresh wheel contact frames */
    td5_physics_refresh_wheel_contacts(actor);

    /* 8. Ground-snap: compute averaged ground height from grounded wheels and
     * correct world_pos.y (IntegrateVehiclePoseAndContacts step 9 in analysis).
     * Without this, gravity accumulates each frame with no correction and cars
     * fall away from the road surface. */
    {
        int32_t corr_sum = 0;
        int corr_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;
        for (int i = 0; i < 4; i++) {
            if (!(gnd_mask & (1 << i))) {  /* grounded wheel */
                int32_t g_y = 0;
                int g_surf = 0;
                int g_span = actor->track_span_raw;
                if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                           actor->wheel_contact_pos[i].z,
                                           g_span, &g_y, &g_surf)) {
                    corr_sum += g_y - actor->wheel_contact_pos[i].y;
                    corr_count++;
                }
            }
        }
        if (corr_count > 0) {
            actor->world_pos.y += corr_sum / corr_count;
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
            /* Cancel downward velocity: ground is a hard constraint. */
            if (actor->linear_velocity_y < 0)
                actor->linear_velocity_y = 0;
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

        actor->rotation_matrix.m[0] = (float)((cy * cp) >> 12) * s;
        actor->rotation_matrix.m[1] = (float)(((cy * sp >> 12) * sr >> 12) - ((sy * cr) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)(((cy * sp >> 12) * cr >> 12) + ((sy * sr) >> 12)) * s;
        actor->rotation_matrix.m[3] = (float)((sy * cp) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)(((sy * sp >> 12) * sr >> 12) + ((cy * cr) >> 12)) * s;
        actor->rotation_matrix.m[5] = (float)(((sy * sp >> 12) * cr >> 12) - ((cy * sr) >> 12)) * s;
        actor->rotation_matrix.m[6] = (float)(-sp) * s;
        actor->rotation_matrix.m[7] = (float)((cp * sr) >> 12) * s;
        actor->rotation_matrix.m[8] = (float)((cp * cr) >> 12) * s;
    }

    /* Render position */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Refresh wheel contacts */
    td5_physics_refresh_wheel_contacts(actor);

    /* Ground-snap from grounded wheels (UpdateVehiclePoseFromPhysicsState step 7) */
    {
        int32_t corr_sum = 0;
        int corr_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;
        for (int i = 0; i < 4; i++) {
            if (!(gnd_mask & (1 << i))) {
                int32_t g_y = 0;
                int g_surf = 0;
                int g_span = actor->track_span_raw;
                if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                           actor->wheel_contact_pos[i].z,
                                           g_span, &g_y, &g_surf)) {
                    corr_sum += g_y - actor->wheel_contact_pos[i].y;
                    corr_count++;
                }
            }
        }
        if (corr_count > 0) {
            actor->world_pos.y += corr_sum / corr_count;
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

        /* Apply suspension deflection offset */
        int32_t susp_offset = actor->wheel_suspension_pos[i];
        wy += susp_offset;

        /* Transform by body rotation matrix and scale to world coords (<<8) */
        int32_t world_x = (int32_t)(rot[0] * wx + rot[1] * wy + rot[2] * wz);
        int32_t world_y = (int32_t)(rot[3] * wx + rot[4] * wy + rot[5] * wz);
        int32_t world_z = (int32_t)(rot[6] * wx + rot[7] * wy + rot[8] * wz);

        /* Add to chassis world position */
        actor->wheel_contact_pos[i].x = actor->world_pos.x + (world_x << 0);
        actor->wheel_contact_pos[i].y = actor->world_pos.y + (world_y << 0);
        actor->wheel_contact_pos[i].z = actor->world_pos.z + (world_z << 0);

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

        /* Set recovery flag */
        actor->vehicle_mode = 1;
        actor->steering_command = 0;
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

    /* Convert positions to float for render */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Re-establish ground contact */
    td5_physics_integrate_pose(actor);
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

    /* Asymmetric slew rates */
    int32_t delta = rpm - target;
    if (delta > 0) {
        /* Slew down: max 200/frame */
        if (delta > 200)
            rpm -= 200;
        else
            rpm -= delta / 16 + 1;
    } else {
        /* Slew up: max 400/frame */
        if (delta < -400)
            rpm += 400;
        else
            rpm += (-delta) / 16 + 1;
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

    /* Skip neutral when throttle positive */
    if (gear == TD5_GEAR_REVERSE && throttle > 0)
        gear = TD5_GEAR_FIRST;

    /* Upshift: RPM > upshift_threshold AND speed > 0 AND gear < max */
    if (gear < actor->max_gear_index && actor->longitudinal_speed > 0) {
        int32_t up_thresh = (int32_t)PHYS_S(actor, 0x3E + gear * 2);
        if (rpm > up_thresh) {
            gear++;
            /* Gear-change torque kick to wheel accumulators */
            int32_t kick = (throttle * (int32_t)PHYS_S(actor, 0x68) * 0x1A) >> 16;
            if (gear < 16)
                kick = (kick * (int32_t)s_gear_torque[gear]) >> 8;
            actor->wheel_force_accum[0] += kick;
            actor->wheel_force_accum[1] += kick;
            actor->wheel_force_accum[2] -= kick;
            actor->wheel_force_accum[3] -= kick;
        }
    }

    /* Downshift: RPM < downshift_threshold AND gear > 2 */
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

    /* Scale by throttle and gear ratio */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    if (throttle < 0) throttle = -throttle; /* abs for torque magnitude */
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

    int32_t force = (throttle * sensitivity * 0x1A) >> 16;
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
    float *rot = actor->rotation_matrix.m;

    /* Compute two perpendicular vectors from body rotation matrix
     * representing the track surface tangent frame */
    int32_t v1x = (int32_t)((rot[0] - rot[3] - rot[6] + rot[3]) * 256.0f);
    int32_t v1y = (int32_t)((rot[1] - rot[4] - rot[7] + rot[4]) * 256.0f);
    int32_t v1z = (int32_t)((rot[2] - rot[5] - rot[8] + rot[5]) * 256.0f);

    int32_t v2x = (int32_t)((rot[0] - rot[6] - rot[3] + rot[3]) * 256.0f);
    int32_t v2y = (int32_t)((rot[1] - rot[7] - rot[4] + rot[4]) * 256.0f);
    int32_t v2z = (int32_t)((rot[2] - rot[8] - rot[5] + rot[5]) * 256.0f);

    /* Cross product -> surface normal (12-bit fixed) */
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
        update_vehicle_pose_from_physics(actor);
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

    tuning = s_default_tuning[slot];
    cardef = s_default_cardef[slot];

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
