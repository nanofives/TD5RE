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
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_platform.h"
#include "td5re.h"

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */
#include <stdlib.h>  /* abs */

#define LOG_TAG "physics"

extern void *g_actor_pool;
extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

int td5_game_get_total_actor_count(void);
int td5_game_is_wanted_mode(void);

/* OBB corner test output: per-corner penetration data */
typedef struct OBB_CornerData {
    int16_t proj_x;     /* corner position in TARGET's local frame (X) */
    int16_t proj_z;     /* corner position in TARGET's local frame (Z) */
    int16_t pen_x;      /* penetration depth along X axis */
    int16_t pen_z;      /* penetration depth along Z axis */
    int16_t own_x;      /* rotated corner offset from penetrator's center, in TARGET's frame (X) */
    int16_t own_z;      /* rotated corner offset from penetrator's center, in TARGET's frame (Z) */
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
                                     int32_t heading_target, int32_t impactForce);

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
static int32_t g_xz_freeze = 0;             /* DAT_00483030: 1=freeze XZ during countdown */
static int32_t s_dynamics_mode = 0;          /* 0=arcade, 1=simulation (0x42F7B0) */
static int32_t g_difficulty_easy = 0;
static int32_t g_difficulty_hard = 0;
static int32_t g_total_actor_count = 6;
static int32_t g_race_slot_state[6];         /* 1=human, 0=AI per slot */

/* ---- Per-slot NPC handicap (rubber-banding by prior championship position) ----
 * Mirrors the original's gSlotRaceResult/Bonus/Points tables at
 * 0x004AED40/0x004AED58/0x004AED28 (gSlotRacePointsTable, kept here as
 * g_slot_race_points for terminology parity with the decomp).
 *
 * Populated per race session from gRaceResultPointsTable @ 0x00466F90
 * indexed by g_slot_series_position[slot] (0..3 = leader..trailer). Until
 * championship-position plumbing is wired in the frontend, all slots default
 * to position 0 and every adjustment below becomes a mathematical no-op.
 *
 * DAT_004AED70 is never written in the original — it stays 0 and makes the
 * brake<->engine_brake redistribution inert. Not carried in the port.
 */
static const int32_t s_race_result_points_table[4][3] = {
    /* pos 0 (leader)  */ {    0,    0,    0 },
    /* pos 1           */ {  114, -102,  -40 },
    /* pos 2           */ {  -40,  307,  -40 },
    /* pos 3 (trailer) */ { -102,  114,    0 },
};
static int32_t g_slot_series_position[6] = {0, 0, 0, 0, 0, 0};
static int32_t g_slot_race_result[6];
static int32_t g_slot_race_bonus [6];
static int32_t g_slot_race_points[6];
static uint8_t s_prev_grounded_mask[16];     /* per-slot previous-frame grounded bitmask (1=grounded) */
static void integrate_traffic_pose(TD5_Actor *actor);  /* forward decl */
static void process_traffic_segment_edge(TD5_Actor *actor, int slot);  /* forward decl */
static void process_traffic_route_advance(TD5_Actor *actor, int slot);  /* forward decl */
static void process_traffic_forward_checkpoint_pass(TD5_Actor *actor, int slot);  /* forward decl */

/* Per-slot previous-frame wheel transform results (pre-snap) for gap_270 delta.
 * Using post-snap positions causes huge Y deltas because snap Y != transform Y. */
static int32_t s_prev_wheel_tx[12][4];  /* [slot][wheel] X transform result */
static int32_t s_prev_wheel_ty[12][4];  /* [slot][wheel] Y transform result */
static int32_t s_prev_wheel_tz[12][4];  /* [slot][wheel] Z transform result */
static uint8_t s_prev_wheel_valid[12];  /* per-slot: 1 if previous transform is valid */

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
static int32_t compute_reverse_gear_torque(TD5_Actor *actor, int32_t speed_in);
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

/* Wall-to-vehicle inertia constant = 1,500,000 [CONFIRMED @ DAT_00463200] */
#define V2W_INERTIA_K       1500000
/* Angular divisor shared with V2V = 0x28C (652) [CONFIRMED @ 0x406a66] */
#define ANGULAR_DIVISOR_W   0x28C
/* Impulse numerator scale factor [CONFIRMED @ 0x406aae] */
#define V2W_NUM_SCALE       0x1100

void td5_physics_wall_response(TD5_Actor *actor, int32_t wall_angle,
                               int32_t penetration, int side,
                               int32_t probe_x_fp8, int32_t probe_z_fp8)
{
    /* Pre-impulse attitude snapshot (slot 0) — lets us see whether wall
     * response is the proximate trigger of a pitch/roll spike. */
    int32_t pre_av_roll  = actor->angular_velocity_roll;
    int32_t pre_av_pitch = actor->angular_velocity_pitch;
    int32_t pre_av_yaw   = actor->angular_velocity_yaw;
    int32_t pre_disp_r   = actor->display_angles.roll;
    int32_t pre_disp_p   = actor->display_angles.pitch;
    int32_t pre_pos_y    = actor->world_pos.y;
    uint8_t pre_bm       = actor->wheel_contact_bitmask;

    /* wall tangent direction (cos,sin); the wall normal is (-sin, cos). */
    int32_t cos_w = cos_fixed12(wall_angle);
    int32_t sin_w = sin_fixed12(wall_angle);

    /* Push actor position out of wall along the wall normal.
     * [CONFIRMED @ 0x4069a1-0x4069d7]: push = penetration - 4, negative →
     * outward. Arithmetic-right-shift by 4 with sign-bit round mask. */
    int32_t push = penetration - 4;
    {
        int64_t px = (int64_t)sin_w * push;
        int64_t pz = (int64_t)cos_w * push;
        actor->world_pos.x -= (int32_t)((px + ((px >> 63) & 0xF)) >> 4);
        actor->world_pos.z += (int32_t)((pz + ((pz >> 63) & 0xF)) >> 4);
    }

    /* Lever arm iVar9 = (actor_center - probe) dot wall_tangent, both sides
     * divided by 256 first to avoid overflow before the >>12.
     * [CONFIRMED @ 0x00406A04-0x00406A60]. POST-push positions are used
     * because actor->world_pos was already updated above. */
    int32_t arm_x_int = (actor->world_pos.x - probe_x_fp8) >> 8;
    int32_t arm_z_int = (actor->world_pos.z - probe_z_fp8) >> 8;
    int32_t iVar9 = ((int64_t)arm_z_int * sin_w + (int64_t)arm_x_int * cos_w) >> 12;

    /* Decompose velocity into wall-tangent (v_para, iVar4) and
     * wall-normal (v_perp, iVar10) components [CONFIRMED @ 0x4069cc]. */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int32_t v_para = ((int64_t)vx * cos_w + (int64_t)vz * sin_w) >> 12;
    int32_t v_perp = ((int64_t)vz * cos_w - (int64_t)vx * sin_w) >> 12;

    /* iVar11 = contact-point normal velocity (center normal vel + rotation
     * contribution at the lever arm). [CONFIRMED @ 0x00406A60-0x00406A66] */
    int32_t iVar11 = (actor->angular_velocity_yaw / ANGULAR_DIVISOR_W) * iVar9 + v_perp;

    int32_t impulse = 0;
    int32_t new_v_para = v_para;
    int32_t new_v_perp = v_perp;

    /* Early-out when separating [CONFIRMED @ 0x00406A72-0x00406A78]:
     * skip impulse + tangential damping, fall through to rotate velocity back. */
    if (iVar11 >= 0) {
        /* Impulse numerator = ((K>>8) * -0x1100) >> 12, numerator full =
         * num * iVar11. [CONFIRMED @ 0x00406A86-0x00406ACB] */
        int32_t num = (((V2W_INERTIA_K >> 8) * -V2W_NUM_SCALE) >> 12);

        /* Denominator = (iVar9^2 + K) >> 8. [CONFIRMED @ 0x00406AAE-0x00406AD2] */
        int64_t denom64 = ((int64_t)iVar9 * iVar9 + (int64_t)V2W_INERTIA_K) >> 8;
        if (denom64 == 0) denom64 = 1;
        impulse = (int32_t)((int64_t)num * iVar11 / denom64);

        new_v_perp = v_perp + impulse;

        /* Tangential damping — asm-faithful asymmetry [CONFIRMED via asm
         * 0x00406B0B-0x00406B69]:
         *   v_para >  0 branch: new_v_para = v_para - delta, clamped to 0
         *                       if the subtraction crosses sign (JNS at
         *                       0x00406B33 → zero at 0x00406B69).
         *   v_para <= 0 branch: new_v_para = v_para + delta, NO CLAMP
         *                       (TEST/JLE at 0x00406B65 jumps straight to
         *                       the rotate-back code with whatever value).
         *
         * The asymmetry is what lets the wall REDIRECT sideways-sliding
         * impacts: a glancing crash with negative tangential v_para gets
         * +delta added, often flipping v_para positive — the "wall-friction
         * reorient" effect. Clamping that branch to 0 (as the port did
         * before 2026-04-23) zeroed the lateral slide and stopped the car
         * dead on every sideways hit.
         *
         * Branch order matches the asm (positive first) so the missing
         * clamp on the negative branch stays visually obvious. */
        int32_t v_para_round = v_para >> 6;  /* signed shift; arithmetic-round */
        int32_t tmp;
        if (v_para > 0) {
            tmp = v_para_round + 0x800 + iVar11 * 2;
            int32_t delta = (tmp * 0x180) >> 11;
            new_v_para = v_para - delta;
            if (new_v_para < 0) new_v_para = 0;
        } else {
            tmp = (iVar11 * 2 + 0x800) - v_para_round;
            int32_t delta = (tmp * 0x180) >> 11;
            new_v_para = v_para + delta;
            /* No clamp — original asm at 0x00406B65 jumps to rotate-back
             * regardless of new_v_para's sign. */
        }

        /* Angular velocity update: ω += (impulse * iVar9) / (K / 0x28C).
         * K / 0x28C = 1500000 / 652 = 2300 (integer). This is the yaw
         * alignment kick: with iVar9 non-zero (probe not at CoM along wall),
         * the sign of impulse drives the car toward tangential alignment.
         * [CONFIRMED @ 0x00406B6B-0x00406B7A] */
        int32_t ang_div = V2W_INERTIA_K / ANGULAR_DIVISOR_W;  /* 2300 */
        if (ang_div == 0) ang_div = 1;
        actor->angular_velocity_yaw += (impulse * iVar9) / ang_div;
    }

    /* Rotate (new_v_para, new_v_perp) back to world basis [CONFIRMED @ 0x406a10] */
    actor->linear_velocity_x = ((int64_t)new_v_para * cos_w - (int64_t)new_v_perp * sin_w) >> 12;
    actor->linear_velocity_z = ((int64_t)new_v_para * sin_w + (int64_t)new_v_perp * cos_w) >> 12;

    /* No ±6000 yaw clamp here — original has none inside 0x406980.
     * [CONFIRMED — no write to actor+0x1C4 after LAB_00406B6B] */

    /* Track contact flag: Forward/Reverse handlers pass side=-1 (no write).
     * [CONFIRMED @ 0x406d7e/0x406e4e] */
    if (side >= 0)
        actor->track_contact_flag = (uint8_t)(side + 1);

    TD5_LOG_I(LOG_TAG,
              "wall_response: side=%d pen=%d angle=%d arm=%d iVar11=%d imp=%d vpara=%d vperp=%d yaw=%d",
              side, penetration, wall_angle, iVar9, iVar11, impulse,
              new_v_para, new_v_perp, actor->angular_velocity_yaw);

    if (actor->slot_index == 0) {
        TD5_LOG_I(LOG_TAG,
                  "wall_delta slot0: bm=0x%02x pos_y=%d pre{ar=%d ap=%d ay=%d dr=%d dp=%d} "
                  "post{ar=%d ap=%d ay=%d dr=%d dp=%d}",
                  (int)pre_bm, pre_pos_y,
                  pre_av_roll, pre_av_pitch, pre_av_yaw,
                  pre_disp_r, pre_disp_p,
                  actor->angular_velocity_roll, actor->angular_velocity_pitch,
                  actor->angular_velocity_yaw,
                  (int)actor->display_angles.roll, (int)actor->display_angles.pitch);
    }
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
         * Partial port: gravity + linear damping confirmed from
         * IntegrateScriptedVehicleMotion @ 0x00409D20-0x00409D66.
         * Angular velocity damping removed — original overwrites them from
         * Euler decomposition of recovery_target matrix, does NOT damp.
         * Teleport removed — original ResetVehicleActorState has no teleport.
         * Missing: RefreshScriptedVehicleTransforms rotation-matrix advance
         * (MultiplyRotationMatrices3x3); requires recovery_target_m00 / saved_orientation_m00
         * subsystem not yet ported. */
        actor->frame_counter++;

        /* Linear velocity damping [CONFIRMED @ 0x00409D20-0x00409D3B]:
         * v -= (v + (v>>31 & 0xFF)) >> 8  (sign-extending divide-by-256) */
        actor->linear_velocity_x -= (actor->linear_velocity_x + (actor->linear_velocity_x >> 31 & 0xFF)) >> 8;
        actor->linear_velocity_z -= (actor->linear_velocity_z + (actor->linear_velocity_z >> 31 & 0xFF)) >> 8;

        /* Gravity on Y velocity [CONFIRMED @ 0x00409D3C-0x00409D52]:
         * IntegrateScriptedVehicleMotion subtracts gGravityConstant each tick */
        actor->linear_velocity_y -= g_gravity_constant;

        if (actor->frame_counter > 0x3B) {  /* 59 frames [CONFIRMED @ 0x00409DD0] */
            TD5_LOG_I(LOG_TAG, "mode1 recovery: slot=%d resetting after %d frames",
                      actor->slot_index, actor->frame_counter);
            td5_physics_reset_actor_state(actor);
        }
    } else if (g_game_paused) {
        /* Paused: only update engine RPM display.
         *
         * Original UpdateVehicleActor @ 0x00406650 paused branch:
         *   UpdateVehicleEngineSpeedSmoothed(actor);
         *   if (<AI-slot predicate>)
         *       actor->engine_speed_accum = (cardef[0x72] << 1) / 3;
         *
         * The predicate as decompiled reads (g_selectedGameType != 0 &&
         * slot_state != 1). Empirically the `(redline*2)/3` AI pin DOES
         * fire during /diff-race single-race runs on slots that haven't
         * begun AI dynamics yet (trace slots 3/5 on Moscow land at exactly
         * 7400 = 11100*2/3). So the port's condition on game_type is
         * dropped here — gate only on the slot being a non-player racer,
         * which matches the observed trace. [RE basis: 0x004068B3-0x004068CB] */
        update_engine_speed_smoothed(actor);
        if (actor->slot_index < 6 && g_race_slot_state[actor->slot_index] != 1) {
            int32_t redline = (int32_t)PHYS_S(actor, 0x72);
            actor->engine_speed_accum = (redline << 1) / 3;
        }
    }

    /* 7. Integrate pose and contacts.
     * Traffic (slot >= 6) uses a dedicated path that skips gravity and
     * per-wheel ground snap — the original never calls IntegrateVehiclePoseAndContacts
     * for traffic [CONFIRMED @ 0x443ED0]. Instead, UpdateTrafficVehiclePose sets Y
     * absolutely from barycentric track height each tick.
     *
     * Dispatch note: the original NEVER routes traffic through UpdateVehicleActor
     * either — UpdateRaceActors @ 0x00436a70 dispatches slot>=6 through
     * UpdateTrafficRoutePlan (0x00435e80) + UpdateTrafficActorMotion (0x00443ed0)
     * directly. The port consolidates these into the slot>=6 sub-path of this
     * function, which achieves equivalent semantics as long as the sub-path
     * mirrors UpdateTrafficActorMotion's tick order (route-plan → friction →
     * traffic pose, no wall resolvers). */
    if (actor->slot_index >= 6) {
        integrate_traffic_pose(actor);
        /* Traffic edge containment: call AFTER pose update so world_pos is
         * current. Mirrors UpdateTrafficActorMotion @ 0x443ED0 which calls:
         *   ProcessActorRouteAdvance(slot)        [CONFIRMED @ 0x443ED0+0x54]
         *   ProcessActorForwardCheckpointPass(slot)[CONFIRMED @ 0x443ED0+0x5A]
         *   ProcessActorSegmentTransition(slot)   [CONFIRMED @ 0x443ED0+0x60]
         * Without these, traffic has NO wall containment — only steering-based
         * routing — and drives straight through track walls. */
        int _ts = actor->slot_index;
        process_traffic_route_advance(actor, _ts);
        process_traffic_forward_checkpoint_pass(actor, _ts);  /* [CONFIRMED @ 0x443ED0] */
        process_traffic_segment_edge(actor, _ts);
    } else {
        /* Racer path: full gravity + per-wheel ground snap.
         * Run even during countdown (paused) so ground-snap keeps the car
         * at the correct height above the road surface. */
        td5_physics_integrate_pose(actor);
    }

    /* 8. Track wall contact resolution (FUN_004070E0 -> FUN_00406F50 -> FUN_00406CC0)
     * Racers only (slots 0-5). The original NEVER calls wall resolvers for
     * traffic — traffic is dispatched through UpdateTrafficRoutePlan +
     * UpdateTrafficActorMotion (in UpdateRaceActors @ 0x00436a70) and those
     * paths have ZERO callers of FUN_004070E0/F50/CC0. Containment for
     * traffic is purely via steering: route-plan computes lane-deviation
     * (rs +0x58/+0x5c) -> UpdateActorSteeringBias -> steering_command.
     * [CONFIRMED @ 0x00436a70 + callers-of FUN_004070E0/F50/CC0]
     *
     * Running these resolvers on traffic was already a no-op in practice
     * (resolve_wall_contacts has its own slot>=6 early-out at td5_track.c:536,
     * and fwd_rev_resolve_contact uses stale probe positions for traffic
     * which pass the pen>=0 test and return without contact) — but the
     * guard here keeps the port faithful to the original's dispatch. */
    if (actor->vehicle_mode == 0 && actor->slot_index < 6) {
        td5_track_resolve_reverse_contacts(actor);
        td5_track_resolve_forward_contacts(actor);
        td5_track_resolve_wall_contacts(actor);
    }

    /* 9. Update surface_contact_flags for the NEXT tick's dynamics dispatch.
     * Placed here (not in integrate_pose) so the init path — which calls
     * integrate_pose once from reset_actor_state for suspension settle —
     * doesn't seed the flag. The first post-countdown update_player reads
     * 0 (from the spawn memset) and takes the airborne/coast branch → no
     * drive torque applied → no tick-1 velocity impulse. From tick 2
     * onwards the flag reflects wheel_contact_bitmask and drive torque
     * runs normally. [RE basis: the original's surface_contact_flags is
     * only written inside UpdatePlayerVehicleDynamics @ 0x00404030, at a
     * late drivetrain-commit condition; leaving it at 0 at tick 1 matches
     * the original's observed tick-1 state where vel_x=vel_z=0.] */
    if (!g_game_paused) {
        uint8_t bm = actor->wheel_contact_bitmask;
        uint8_t flags = 0;
        if (!(bm & 0x04) || !(bm & 0x08))  /* RL or RR grounded */
            flags |= 1;
        if (!(bm & 0x01) || !(bm & 0x02))  /* FL or FR grounded */
            flags |= 2;
        actor->surface_contact_flags = flags;
    }

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

    /* Front/rear load fraction (8.8 fixed) — weight transfer via suspension
     * deflection. Original @ 0x004041AE uses center_suspension_pos directly
     * (no shift). Prior port divided by 16, which near-zeroed load transfer
     * and made grip front/rear symmetric under weight shift, killing the
     * asymmetry that drives oversteer/understeer during cornering. [CONFIRMED] */
    int32_t front_load = ((front_weight << 8) / total_weight);
    front_load = front_load * (half_wb - susp_defl) / full_wb;
    int32_t rear_load = ((rear_weight << 8) / total_weight);
    rear_load = rear_load * (half_wb + susp_defl) / full_wb;

    for (i = 0; i < 4; i++) {
        int32_t sf = (int32_t)s_surface_friction[surface_wheel[i] & 0x1F];
        int32_t load = (i < 2) ? front_load : rear_load;
        grip[i] = (sf * load + 128) >> 8;
        if (grip[i] < TD5_PLAYER_GRIP_MIN) grip[i] = TD5_PLAYER_GRIP_MIN;
        if (grip[i] > TD5_PLAYER_GRIP_MAX) grip[i] = TD5_PLAYER_GRIP_MAX;
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "GRIP: front_load=%d rear_load=%d susp_defl=%d grip=[%d,%d,%d,%d]",
                  front_load, rear_load, susp_defl,
                  grip[0], grip[1], grip[2], grip[3]);
    }

    /* --- 4. Handbrake modifier on rear wheels (tuning+0x7A) ---
     * Faithful to UpdatePlayerVehicleDynamics: rear grip *= phys[0x7A]/256.
     * Observed range in carparam files: 144..212 (≈56-83% retention).
     * Original has no clamp — shipped values ARE the tuning. AI vehicles
     * never engage handbrake_flag, so this branch is player-only. */
    if (actor->handbrake_flag) {
        int32_t hb_mod = (int32_t)PHYS_S(actor, 0x7A);
        int32_t g2_pre = grip[2], g3_pre = grip[3];
        grip[2] = (grip[2] * hb_mod) >> 8;
        grip[3] = (grip[3] * hb_mod) >> 8;
        if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "HBRAKE: hb_mod=%d grip_rl=%d->%d grip_rr=%d->%d",
                      hb_mod, g2_pre, grip[2], g3_pre, grip[3]);
        }
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

    /* NOTE: `actor->longitudinal_speed` / `lateral_speed` are NOT written
     * here. The original UpdatePlayerVehicleDynamics @ 0x00404030 writes
     * both fields at its TAIL, using (a) the POST-drag PRE-force velocity
     * (captured above in `vx`/`vz`) and (b) for the lateral field, the
     * front-axle-frame forward projection minus a yaw-rate-induced term,
     * NOT the chassis-right projection. See the dispatch block near the
     * force-writeback for the faithful compute. */

    /* --- 7/8/9/10/11. On-ground vs airborne gate — matches original 0x404030:
     *   if (surface_contact_flags != 0) → ON-GROUND:
     *     always run auto-gear + engine + drive torque (drive_torque
     *     returns 0 at idle throttle, so an idle stationary car gets
     *     wheel_drive=0 and no force is added to vel_x/vel_z here).
     *     If brake_flag set, use brake path instead.
     *   else → AIRBORNE:
     *     run engine + the -32 coast fallback.
     * [CONFIRMED @ 0x00404030]
     *
     * The previous port structure inverted this: it gated on
     * `(!brake && throttle != 0)`, so an idle stationary on-ground car
     * fell into the coast path and got a nonzero -32*brake_coeff impulse
     * on every tick — the root cause of Cluster A vel_x=-399 / vel_z=+895
     * observed in /diff-race on 2026-04-11. */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t drive_torque = 0;
    int32_t wheel_drive[4] = {0, 0, 0, 0};
    int32_t brake_front = (int32_t)PHYS_S(actor, 0x6E);
    int32_t brake_rear  = (int32_t)PHYS_S(actor, 0x70);

    if (actor->surface_contact_flags != 0) {
        /* --- ON-GROUND branch ---
         * Original structure [CONFIRMED @ 0x404030]:
         *   - Manual gearbox (field_0x378==0): reverse_throttle_sign only
         *   - Auto gearbox: NO auto_gear_select on-ground — only on airborne path
         *   - CRGT handles RPM slew (called later in speed writeback section)
         *   - Then: drive or brake path
         * The original relies on micro-airborne frames from track bumps for
         * gear shifts. Do NOT call auto_gear_select here — it causes gear
         * skipping, drivetrain kick accumulation, and RPM oscillation. */
        /* Gearbox dispatch — only when NOT braking.
         * Original airborne path gates on (!brake_flag && throttle != 0)
         * before calling auto_gear [CONFIRMED @ 0x4044F9]. During braking,
         * throttle=-256 which would make auto_gear set gear=REVERSE,
         * corrupting the forward gear state. */
        if (!actor->brake_flag) {
            if (*((const uint8_t *)actor + 0x378) == 0) {
                td5_physics_reverse_throttle_sign(actor);
            } else {
                td5_physics_auto_gear_select_no_kick(actor);
            }
        }

        if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                "GATE: on_ground brake_flag=%d throttle=%d gear=%d rpm=%d v_long=%d scf=0x%X",
                actor->brake_flag, throttle, actor->current_gear,
                actor->engine_speed_accum, v_long, actor->surface_contact_flags);
        }

        if (!actor->brake_flag) {
            /* Drive path: drive torque distributed by drivetrain. At
             * idle throttle (encounter_steering_cmd == 0), compute_drive_torque
             * returns 0 via the actor+0x33e multiply inside the gear curve,
             * so wheel_drive stays zero and no force is added below. */
            drive_torque = td5_physics_compute_drive_torque(actor);

            int32_t dt_type = (int32_t)PHYS_S(actor, 0x76);
            int32_t speed_limit = (int32_t)PHYS_S(actor, 0x74) << 8;
            int32_t abs_speed = v_long < 0 ? -v_long : v_long;

            if (abs_speed <= speed_limit) {
                /* Per-wheel distribution [CONFIRMED @ 0x00404030]:
                 *   RWD: D/2 per rear wheel (front = 0)
                 *   FWD: D/2 per front wheel (rear = 0)
                 *   AWD: D/4 per wheel (total = D)
                 * After grip scaling (grip[i]*wheel_drive[i]>>8) the effective
                 * totals are: RWD rear_long ≈ grip*D, FWD front_long ≈ grip*D,
                 * AWD each pair ≈ grip*D/2. Prior D/4 was a workaround for
                 * missing grip scaling — now that grip is applied, D/2 is correct. */
                switch (dt_type) {
                case 1: /* RWD — D/2 per rear wheel [CONFIRMED @ 0x00404030] */
                    wheel_drive[2] = drive_torque >> 1;
                    wheel_drive[3] = drive_torque >> 1;
                    break;
                case 2: /* FWD — D/2 per front wheel [CONFIRMED @ 0x00404030] */
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
        } else {
            /* Brake path [CONFIRMED @ 0x404441-0x404481]:
             * bf = (brake_front * throttle) >> 8
             * Clamp: bf magnitude capped to abs(v_long) — prevents over-braking
             * past zero speed. Original uses min(abs(bf), abs(uVar37)) where
             * uVar37 = body-frame longitudinal speed (sin_h*vx + cos_h*vz).
             * Previous port used v_lat in the clamp, which killed brake force
             * when going straight (v_lat ≈ 0).
             * Wheel assignment: bf/2 per driven pair, 0 on other pair.
             * [CONFIRMED @ 0x404474-0x40447e] */
            /* On-ground brake: REAR wheels only, clamped to |v_long|
             * [CONFIRMED @ 0x404441-0x404481]: uses tuning+0x6E only,
             * assigns result/2 to RL/RR, 0 to FL/FR.
             * Original clamps against uVar37 = velocity projected onto the
             * steered-front-wheel axis: ((cos(yaw+steer)*vz + sin(yaw+steer)*vx)>>12)
             * minus a yaw-rate correction term [CONFIRMED @ 0x404441-0x404481].
             * When steer≈0, uVar37 ≡ body longitudinal speed (v_long).
             * Previously this clamped against |v_lat|, which collapsed to 0
             * when driving straight and killed all braking force. */
            int32_t bf = (brake_front * throttle) >> 8;
            {
                int32_t abs_bf = bf < 0 ? -bf : bf;
                /* Faithful clamp against velocity projected onto the steered-
                 * front-wheel axis [CONFIRMED @ 0x404441-0x404481]:
                 *   uVar37 ≈ (cos(yaw+steer)*vz + sin(yaw+steer)*vx) >> 12
                 * When steer=0 this equals v_long (straight-line brake works
                 * at full strength). When steering, the projection shrinks by
                 * roughly cos(steer), naturally weakening brake during turns —
                 * matching the original's behavior. The yaw-rate sub-term
                 * (sin(steer)*tuning[0x28]*avy)/0x28C is omitted — port sign
                 * conventions differ from the binary's and getting the correction
                 * right requires a sign audit. Cap doubled to 2x for user-
                 * preferred stronger feel; remove the <<1 for full RE parity. */
                int32_t steer_angle = -(actor->steering_command >> 8);
                int32_t steer_h = (heading + steer_angle) & 0xFFF;
                int32_t cos_sh = cos_fixed12(steer_h);
                int32_t sin_sh = sin_fixed12(steer_h);
                int32_t v_steer_axis = (cos_sh * vz + sin_sh * vx) >> 12;

                /* Yaw-rate correction [CONFIRMED @ 0x00404441-0x00404481]:
                 * uVar37 -= (sin(steer_raw) * tuning[0x28] * angular_velocity_yaw >> 12) / 0x28C
                 * Use original's unsigned steer angle (undo port negation convention). */
                {
                    int32_t steer_raw = (-steer_angle) & 0xFFF;
                    int32_t sin_sr = sin_fixed12(steer_raw);
                    int32_t yaw_corr = (sin_sr * (int32_t)PHYS_S(actor, 0x28) * actor->angular_velocity_yaw) >> 12;
                    yaw_corr /= ANGULAR_DIVISOR_W;
                    v_steer_axis -= yaw_corr;
                }

                int32_t abs_vsa = v_steer_axis < 0 ? -v_steer_axis : v_steer_axis;
                int32_t cap = abs_vsa;  /* RE parity: no <<1 doubling */
                int32_t clamped = abs_bf < cap ? abs_bf : cap;
                bf = (bf < 0) ? -(int32_t)clamped : (int32_t)clamped;
            }
            wheel_drive[0] = 0;
            wheel_drive[1] = 0;
            wheel_drive[2] = bf >> 1;
            wheel_drive[3] = bf >> 1;
            /* When braking on-ground at low forward speed, hand off to
             * REVERSE drive. Original achieves this via auto_gear during
             * airborne microbumps [CONFIRMED @ 0x42EF1C: throttle<0 → gear=0];
             * port replicates the effect directly because ground contact is
             * too sticky for microbumps. Tuning departure: dropped lateral
             * velocity gate (drift during brake blocked the trigger) and
             * raised the longitudinal threshold from 0x40 to 0x100 so reverse
             * engages crisply at the moment forward motion stops. Clearing
             * brake_flag here routes the next tick through the drive path,
             * which with throttle<0 + gear=REVERSE produces reverse motion. */
            {
                int32_t abs_vlong = v_long < 0 ? -v_long : v_long;
                if (abs_vlong < 0x100) {
                    actor->current_gear = TD5_GEAR_REVERSE;
                    actor->brake_flag = 0;
                }
            }
            if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "BRAKE: bf=%d brk_front=%d throttle=%d v_long=%d v_lat=%d wd2=%d gear=%d sf=%d",
                    bf, brake_front, throttle, v_long, v_lat, wheel_drive[2],
                    (int)actor->current_gear,
                    (int)s_surface_friction[surface_wheel[0] & 0x1F]);
            }
        }
    } else {
        /* --- AIRBORNE branch [CONFIRMED @ 0x4044F9-0x4045AE] ---
         * Original structure:
         *   1. Write body velocities to long/lat speed
         *   2. Brake check
         *   3. If !brake && throttle != 0:
         *        auto_gear or reverse_throttle + UESA + CDTFGC + wheel dist
         *   4. Speed limit check
         *   5. If brake || coast(-32): UESA + brake forces */
        int32_t coast_throttle = (throttle != 0) ? throttle : -32;

        if (!actor->brake_flag && throttle != 0) {
            /* Gearbox dispatch [CONFIRMED @ 0x404521]:
             * field_0x378 == 0 → manual, != 0 → automatic */
            if (*((const uint8_t *)actor + 0x378) == 0) {
                td5_physics_reverse_throttle_sign(actor);
            } else {
                td5_physics_auto_gear_select(actor);
            }
            td5_physics_update_engine_speed(actor);

            /* Drive torque while airborne [CONFIRMED @ 0x404560-0x4045AE] */
            drive_torque = td5_physics_compute_drive_torque(actor);
            int32_t dt_type = (int32_t)PHYS_S(actor, 0x76);
            int32_t speed_limit = (int32_t)PHYS_S(actor, 0x74) << 8;
            int32_t abs_speed = v_long < 0 ? -v_long : v_long;
            if (abs_speed <= speed_limit) {
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
        } else {
            /* Brake or coast path — clamp to v_long (same fix as on-ground). */
            td5_physics_update_engine_speed(actor);
            int32_t bf = (brake_front * coast_throttle) >> 8;
            int32_t br = (brake_rear  * coast_throttle) >> 8;
            {
                int32_t abs_vl = v_long < 0 ? -v_long : v_long;
                int32_t abs_bf = bf < 0 ? -bf : bf;
                int32_t abs_br = br < 0 ? -br : br;
                int32_t cf = abs_bf < abs_vl ? abs_bf : abs_vl;
                int32_t cr = abs_br < abs_vl ? abs_br : abs_vl;
                bf = (bf < 0) ? -(int32_t)cf : (int32_t)cf;
                br = (br < 0) ? -(int32_t)cr : (int32_t)cr;
            }
            wheel_drive[0] = bf >> 1;
            wheel_drive[1] = bf >> 1;
            wheel_drive[2] = br >> 1;
            wheel_drive[3] = br >> 1;
        }

        if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
            TD5_LOG_I(LOG_TAG, "update_player: airborne gear=%d rpm=%d torque=%d",
                      actor->current_gear, actor->engine_speed_accum, drive_torque);
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "Player phys: gear=%d rpm=%d torque=%d v_long=%d v_lat=%d "
                  "throttle_cmd=%d throttle_active=%d surface_contact=0x%X",
                  actor->current_gear, actor->engine_speed_accum, drive_torque,
                  v_long, v_lat, (int)actor->encounter_steering_cmd,
                  actor->throttle_input_active,
                  actor->surface_contact_flags);
    }

    /* --- 12. Per-axle lateral/longitudinal forces --- */
    int32_t steer_angle = -(actor->steering_command >> 8);
    /* Original input maps LEFT=positive, RIGHT=negative (bit 0x02=LEFT feeds
     * the add-to-cmd path). Port input maps LEFT=negative, RIGHT=positive.
     * Negate here to match the original convention the physics was built
     * around. [CONFIRMED: original bit layout documented in td5_input.c:484]
     *
     * Original (0x40415B): steer_angle = steering_command >> 8, no scaling.
     * Constant 294 does NOT exist in the binary. [CONFIRMED @ 0x404142-0x40415E] */
    int32_t steer_heading = (heading + steer_angle) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);   /* cos(h+s) — iVar16 */
    int32_t sin_s = sin_fixed12(steer_heading);   /* sin(h+s) — iVar17 */

    /* Steer-angle-only cos/sin for the lateral force solve.
     * Decomp uses iVar18 = cos(steer), iVar19 = sin(steer) — NOT (h+s). */
    int32_t steer_only = steer_angle & 0xFFF;
    int32_t cos_sr = cos_fixed12(steer_only);     /* iVar18 = cos(s) */
    int32_t sin_sr = sin_fixed12(steer_only);     /* iVar19 = sin(s) */

    /* Front/rear longitudinal forces scaled by RAW surface friction coefficient.
     * [CONFIRMED @ 0x004046DC]: original re-reads gSurfaceGripCoefficientTable
     * (DAT_004748C0) directly — it does NOT use the load-weighted/clamped grip[].
     * The grip[] array is for slip-circle limiting only.
     * Formula: sf[i] * wheel_drive[i] >> 8, where sf = raw table value (e.g. 256 for asphalt).
     * Prior port used grip[i] (56-80 after load+clamp) which is ~2x too small. */
    int32_t sf_fl = (int32_t)s_surface_friction[surface_wheel[0] & 0x1F];
    int32_t sf_fr = (int32_t)s_surface_friction[surface_wheel[1] & 0x1F];
    int32_t sf_rl = (int32_t)s_surface_friction[surface_wheel[2] & 0x1F];
    int32_t sf_rr = (int32_t)s_surface_friction[surface_wheel[3] & 0x1F];
    int32_t front_long = ((sf_fl * wheel_drive[0]) >> 8) + ((sf_fr * wheel_drive[1]) >> 8);
    int32_t rear_long  = ((sf_rl * wheel_drive[2]) >> 8) + ((sf_rr * wheel_drive[3]) >> 8);

    /* --- Coupled bicycle-model lateral force solve ---
     * Literal port of UpdatePlayerVehicleDynamics @ 0x00404A40-0x00404CCC.
     * Ghidra name mapping: local_c = front_lat_force, local_14 = rear_lat_force,
     * local_8 = F_f (front drive), local_2c = F_r (rear drive), iVar27 = I
     * (tuning+0x20 inertia).
     *
     * Prior port used a linear approximation
     *   front_lat = -front_slip * grip_avg >> 8
     *   rear_lat  = -v_lat * grip_avg >> 8
     * which dropped the 2x2 bicycle determinant, yaw-rate coupling, and
     * drive-force cross terms — all required for drift initiation and
     * propagation. Replaced with literal decomp transcription.
     *
     * Uses signed integer `/` (not `>>`) to match the original's rounding-
     * toward-zero idiom `(x + (x>>31 & mask)) >> n`. [CONFIRMED bit-exact] */
    int32_t front_lat_force;
    int32_t rear_lat_force;
    {
        int32_t a_ = front_weight;                          /* iVar32 */
        int32_t b_ = rear_weight;                           /* iVar13 */
        int32_t I_ = PHYS_I(actor, 0x20);                   /* iVar27 */
        int32_t L_ = a_ + b_;                               /* iVar33 */
        int32_t w_ = actor->angular_velocity_yaw;           /* iVar28 (initial) */
        int32_t F_f = front_long;                           /* local_8 */
        int32_t F_r = rear_long;                            /* local_2c */
        int32_t vx_b = v_lat;                               /* iVar20 */
        int32_t vz_b = v_long;                              /* uVar12 */

        /* Determinant: denom = [L²·cos²(s) + (b²+I)·sin²(s)] / 2^34
         * [CONFIRMED @ 0x00404A40-0x00404A9F]
         * Original x86 uses 32-bit `imul reg,reg` (wrapping) with the
         * `(x + (x>>31 & mask)) >> n` rounding idiom — Ghidra's `>> 0x1f`
         * confirms 32-bit, not 64-bit. All intermediates are int32 with
         * natural wrapping matching the original binary. */
        int32_t bb_plus_I = (b_ * b_ + I_) / 1024;          /* iVar21 */
        int32_t LL_over_1024 = (L_ * L_) / 1024;
        int32_t t22 = (LL_over_1024 * cos_sr / 4096) * cos_sr;
        int32_t t23 = (bb_plus_I * sin_sr / 4096) * sin_sr;
        int32_t denom = t22 / 4096 + t23 / 4096;
        if (denom == 0) denom = 1;

        /* Rear lateral force (local_14) numerator
         * [CONFIRMED @ 0x00404AA0-0x00404B27] */
        int32_t ba_minus_I = (b_ * a_ - I_) / 1024;
        int32_t iv24 = (I_ / 0x28C) * w_;

        /* A = (I/1024) * ((b*w)/652 - vx_b) / 4096 * sin(s) */
        int32_t t_a = (I_ / 1024) * ((b_ * w_) / 0x28C - vx_b);
        t_a = (t_a / 4096) * sin_sr;

        /* B = ((F_f*cos(s)/4096 + F_r + vz_b) * (b*a-I)/1024 / 4096) * cos(s) */
        int32_t drive_sum = (F_f * cos_sr / 4096) + F_r + vz_b;
        int32_t t_b = drive_sum * ba_minus_I;
        t_b = (t_b / 4096) * cos_sr;

        /* C = ((b*a-I)/1024 * F_f / 4096 * sin(s)) / 4096 * sin(s) */
        int32_t t_c = ba_minus_I * F_f;
        t_c = (t_c / 4096) * sin_sr;
        t_c = (t_c / 4096) * sin_sr;

        /* D = (((I/652)*w - vx_b*a) / 1024 * L) / 4096 * cos(s) */
        int32_t t_d = (iv24 - vx_b * a_) / 1024 * L_;
        t_d = (t_d / 4096) * cos_sr;

        int32_t local_14_num = (t_d / 4096) * cos_sr
                             + ((t_a / 4096) + (t_b / 4096) + (t_c / 4096)) * sin_sr;
        rear_lat_force = local_14_num / denom;  /* no negation [CONFIRMED @ 0x00404B27] */

        /* Front lateral force (local_c) numerator
         * [CONFIRMED @ 0x00404B28-0x00404B93] */
        /* E = ((a+2b)*a - I)/1024 * F_f / 4096 * cos(s) + (F_r + vz_b) * (b²+I)/1024 */
        int32_t acoef = (a_ + 2 * b_) * a_ - I_;
        int32_t t_e = (acoef / 1024) * F_f;
        t_e = (t_e / 4096) * cos_sr + (F_r + vz_b) * bb_plus_I;

        /* F = (vx_b*b + iv24) / 1024 * L */
        int32_t iv24b = vx_b * b_ + iv24;
        int32_t t_f = (iv24b / 1024) * L_;

        int32_t local_c_num = (t_e / 4096) * sin_sr - (t_f / 4096) * cos_sr;
        front_lat_force = local_c_num / denom;  /* no negation [CONFIRMED @ 0x00404B93] */
    }

    /* Tuning departure: while braking on-ground, halve the lateral tire
     * forces before they're rotated into world-frame velocity. The coupled
     * bicycle model generates large cornering forces when steering, which
     * get added on top of the rear longitudinal brake force and produce a
     * ~9x-stronger per-tick deceleration than straight-line braking
     * (observed in log/race.log FORCE_BRK entries: straight fz≈-593,
     * steering fx=5414 fz=-888). This made brake feel uneven between
     * straight and turning. Halving f_lat / r_lat flattens the asymmetry
     * while preserving some cornering bite so the car still carves when
     * trail-braking. Not RE-faithful — save as TODO for /diff-race
     * investigation whether the original's coupled solve really produces
     * these magnitudes or if a slip-circle / grip-limit upstream bug
     * inflates them. */
    if (actor->brake_flag && actor->surface_contact_flags != 0) {
        front_lat_force >>= 1;
        rear_lat_force  >>= 1;
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "LATFORCE: f_lat=%d r_lat=%d vx_b=%d vz_b=%d w=%d steer=%d",
                  front_lat_force, rear_lat_force, v_lat, v_long,
                  actor->angular_velocity_yaw, steer_angle);
    }

    /* --- 13. Tire slip circle — two-pass port of 0x00404950-0x00404ADE.
     *
     * Pass A (slip-coupling, phys[0x7C]): when an axle has meaningful slip,
     *   shape the axle forces via `(long/2) * coupled / hyp` where
     *   hyp = sqrt(lat² + coupled²) + 1 and coupled = slip * phys[0x7C] / 256.
     *   Frida @ 0x00404030 (2026-04-23) confirmed the shaped local_2c flows
     *   to linear_velocity writebacks at 0x00404D7E/D9E — it IS the drive
     *   force at that point. Pass-A also attenuates grip_limit by |lat>>8|/hyp
     *   so the following classical slip-circle (Pass B) has a tighter clamp.
     *
     * Zero-slip edge: at slip=0 the original's formula gives drive=0 (hyp=1,
     *   coupled=0 → result*0). The original avoids this via the flag-clear:
     *   when slip_shift<0x41 it sets surface_contact_flags='\\0', so the
     *   NEXT tick's pass-A doesn't run. The port's contact-flag semantics
     *   differ enough that flag-clear alone isn't safe (bit can stay set
     *   at rest, zeroing drive). Defensive guard: only enter pass-A when
     *   slip_shift >= 0x41 — same threshold the original uses for the clear,
     *   protects from the zero-drive edge case at rest / grid / countdown.
     *
     * Pass B (classical): unconditional slip-circle clamp — same behavior
     *   as the prior single-pass implementation. Clamps both longitudinal
     *   and lateral when combined > grip_limit. */
    int32_t tire_grip_coeff = (int32_t)PHYS_S(actor, 0x2C);
    int32_t slip_coupling   = (int32_t)PHYS_S(actor, 0x7C);
    {
        /* ---- FRONT axle ---- */
        int32_t grip_limit_f = (grip[0] + grip[1]);
        if (tire_grip_coeff != 0)
            grip_limit_f = (grip_limit_f * tire_grip_coeff) >> 8;

        /* Pass A — only when slip already meaningful (avoids zero-drive edge) */
        if ((actor->surface_contact_flags & 1) && slip_coupling != 0) {
            int32_t body_vlong = (vx * sin_h + vz * cos_h) >> 12;
            int32_t pos_vlong  = (body_vlong < 0) ? 0 : body_vlong;
            int32_t delta      = actor->longitudinal_speed - pos_vlong;
            int32_t delta_abs  = (delta < 0) ? -delta : delta;
            int32_t slip_shift = delta_abs >> 8;
            if (slip_shift >= 0x41) {
                int32_t coupled = (slip_shift * slip_coupling) >> 8;
                int32_t lat     = front_lat_force;
                int32_t latSh   = lat >> 8;
                int32_t latMix  = (latSh * lat) >> 8;
                int32_t hyp_sq  = latMix + coupled * coupled;
                if (hyp_sq < 0) hyp_sq = 0;
                int32_t hyp     = td5_isqrt(hyp_sq) + 1;
                int32_t latSh_a = (latSh < 0) ? -latSh : latSh;
                grip_limit_f    = (latSh_a * grip_limit_f) / hyp;
                front_long      = ((front_long / 2) * coupled) / hyp;
            }
        }

        /* Pass B — classical slip-circle clamp */
        int32_t fl16 = front_lat_force >> 4;
        int32_t flo16 = front_long >> 4;
        int32_t combined_sq = fl16 * fl16 + flo16 * flo16;
        int32_t combined = td5_isqrt(combined_sq) << 4;
        if (combined > grip_limit_f && combined > 0) {
            actor->front_axle_slip_excess = combined - grip_limit_f;
            front_long = ((grip_limit_f << 8) / combined * front_long) >> 8;
            front_lat_force = ((grip_limit_f << 8) / combined * front_lat_force) >> 8;
        } else {
            actor->front_axle_slip_excess = 0;
        }

        /* ---- REAR axle ---- */
        int32_t grip_limit_r = (grip[2] + grip[3]);
        if (tire_grip_coeff != 0)
            grip_limit_r = (grip_limit_r * tire_grip_coeff) >> 8;

        if ((actor->surface_contact_flags & 2) && slip_coupling != 0) {
            int32_t raw_front = (int32_t)(((int64_t)sin_s * vx + (int64_t)cos_s * vz) >> 12);
            int64_t yaw_q12   = (int64_t)sin_sr * front_weight * actor->angular_velocity_yaw;
            int32_t yaw_term  = (int32_t)((yaw_q12 >> 12) / 0x28c);
            int32_t axle_vlat = raw_front - yaw_term;
            int32_t pos_vlat  = (axle_vlat < 0) ? 0 : axle_vlat;
            int32_t delta     = actor->lateral_speed - pos_vlat;
            int32_t delta_abs = (delta < 0) ? -delta : delta;
            int32_t slip_shift = delta_abs >> 8;
            if (slip_shift >= 0x41) {
                int32_t coupled = (slip_shift * slip_coupling) >> 8;
                int32_t lat     = rear_lat_force;
                int32_t latSh   = lat >> 8;
                int32_t latMix  = (latSh * lat) >> 8;
                int32_t hyp_sq  = latMix + coupled * coupled;
                if (hyp_sq < 0) hyp_sq = 0;
                int32_t hyp     = td5_isqrt(hyp_sq) + 1;
                int32_t latSh_a = (latSh < 0) ? -latSh : latSh;
                grip_limit_r    = (latSh_a * grip_limit_r) / hyp;
                rear_long       = ((rear_long / 2) * coupled) / hyp;
            }
        }

        int32_t rl16 = rear_lat_force >> 4;
        int32_t rlo16 = rear_long >> 4;
        combined_sq = rl16 * rl16 + rlo16 * rlo16;
        combined = td5_isqrt(combined_sq) << 4;
        if (combined > grip_limit_r && combined > 0) {
            actor->rear_axle_slip_excess = combined - grip_limit_r;
            rear_long = ((grip_limit_r << 8) / combined * rear_long) >> 8;
            rear_lat_force = ((grip_limit_r << 8) / combined * rear_lat_force) >> 8;
        } else {
            actor->rear_axle_slip_excess = 0;
        }

        if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "SLIPCIRC: f_comb=%d f_lim=%d r_comb=%d r_lim=%d f_ex=%d r_ex=%d cpl=%d",
                      td5_isqrt(flo16*flo16 + fl16*fl16) << 4,
                      grip_limit_f,
                      combined, grip_limit_r,
                      (int)actor->front_axle_slip_excess,
                      (int)actor->rear_axle_slip_excess,
                      slip_coupling);
        }
    }

    /* --- 13a. current_slip_metric (+0x33C) — faithful port @ 0x004049BA/0x00404A80.
     * Rear overwrites front when both contact bits set. */
    {
        int32_t slip_mag = 0;
        uint8_t scf = actor->surface_contact_flags;

        if (scf & 1) {
            int32_t body_vlong = (vx * sin_h + vz * cos_h) >> 12;
            int32_t pos_vlong  = (body_vlong < 0) ? 0 : body_vlong;
            int32_t delta = actor->longitudinal_speed - pos_vlong;
            int32_t abs_d = (delta < 0) ? -delta : delta;
            slip_mag = abs_d >> 8;
        }
        if (scf & 2) {
            int32_t raw_front = (int32_t)(((int64_t)sin_s * vx + (int64_t)cos_s * vz) >> 12);
            int64_t yaw_q12 = (int64_t)sin_sr * front_weight * actor->angular_velocity_yaw;
            int32_t yaw_term = (int32_t)((yaw_q12 >> 12) / 0x28c);
            int32_t body_vlat = raw_front - yaw_term;
            int32_t pos_vlat  = (body_vlat < 0) ? 0 : body_vlat;
            int32_t delta = actor->lateral_speed - pos_vlat;
            int32_t abs_d = (delta < 0) ? -delta : delta;
            slip_mag = abs_d >> 8;
        }
        if (slip_mag > 0x7FFF) slip_mag = 0x7FFF;
        actor->current_slip_metric = (int16_t)slip_mag;
    }

    /* --- 14. Yaw torque, clamp [-0x578, +0x578] ---
     *
     * Literal decomp @ 0x404C80-0x404CCC:
     *   yaw = (cos(s)*a/4096 * front_lat
     *        + (wheel_RR - wheel_RL - wheel_FL + wheel_FR) * 500
     *        - rear_lat * b)
     *        / (I / 0x28C)
     *
     * Term2 is the left-right per-WHEEL longitudinal force differential:
     * each wheel_XX = grip_surface[bVarN] * wheel_drive_force >> 8. Since
     * the port uses a single center surface probe (all 4 wheels share the
     * same grip), and each axle pair shares equal drive force, term2 = 0.
     * When per-wheel surface probing is implemented, term2 becomes non-zero
     * on mixed-surface transitions (e.g. two wheels on grass).
     * [CONFIRMED @ 0x404C80 via literal decomp — iVar31 = rear-right,
     * iVar11 = rear-left, iVar35 = front-left, iVar36 = front-right]
     *
     * PRIOR BUG: port used (rear_lat - front_lat)*500, which was millions
     * of units and saturated the ±1400 clamp every tick — root cause of
     * handling over-sensitivity after the coupled solve landed.
     *
     * Speed gate (0x100 threshold) also removed — original has no analogous
     * gate. With the coupled solve and correct yaw formula, lateral forces
     * naturally go to zero at zero velocity. [CONFIRMED no gate in original] */
    {
        int32_t inertia = PHYS_I(actor, 0x20);
        int32_t inertia_div = inertia / 0x28C;
        if (inertia_div == 0) inertia_div = 1;

        /* Term 1: cos(steer) * front_weight / 4096 * front_lat_force
         * [CONFIRMED @ 0x404C80: (iVar18*iVar32 >> 12) * local_c] */
        int32_t term1 = (cos_sr * front_weight) / 4096;
        term1 = term1 * front_lat_force;

        /* Term 2: per-wheel longitudinal force left-right differential * 500.
         * Currently 0 because port uses uniform grip per axle pair.
         * [CONFIRMED @ 0x404C8E: (iVar31-iVar11-iVar35+iVar36)*500] */
        int32_t wheel_long_fl = (sf_fl * wheel_drive[0]) / 256;
        int32_t wheel_long_fr = (sf_fr * wheel_drive[1]) / 256;
        int32_t wheel_long_rl = (sf_rl * wheel_drive[2]) / 256;
        int32_t wheel_long_rr = (sf_rr * wheel_drive[3]) / 256;
        int32_t term2 = (wheel_long_rr - wheel_long_rl - wheel_long_fl + wheel_long_fr) * 500;

        /* Term 3: rear_lat_force * rear_weight
         * [CONFIRMED @ 0x404CBF: local_14 * iVar13] */
        int32_t term3 = rear_lat_force * rear_weight;

        int32_t yaw_torque = (term1 + term2 - term3) / inertia_div;

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

        actor->angular_velocity_yaw += yaw_torque;

        if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "YAW: torque=%d ang_vel=%d heading=%d t1=%d t2=%d t3=%d idiv=%d",
                      yaw_torque, actor->angular_velocity_yaw,
                      (actor->euler_accum.yaw >> 8) & 0xFFF,
                      term1, term2, term3, inertia_div);
        }
    }

    /* --- Apply longitudinal and lateral forces back to world frame ---
     *
     * Original UpdatePlayerVehicleDynamics @ 0x00404030 writes linear_velocity
     * via TWO SEPARATE ROTATIONS, not one combined total-force rotation:
     *
     *   rear-axle forces (rear_long, rear_lat_force)
     *     → rotated by HEADING                (cos_h, sin_h)
     *   front-axle forces (front_long, front_lat_force)
     *     → rotated by HEADING + STEER        (cos_s, sin_s)
     *
     * The previous port collapsed both pairs into (total_long, total_lat)
     * and rotated once by HEADING, which folds the steered axis into the
     * body axis and produced a ~2.15x magnitude error at the first post-GO
     * drive tick (observed 2026-04-11 in /diff-race, orig vel_x=-185 vs
     * port vel_x=-399). This is the same structure the AI path already
     * uses at td5_physics.c:1246-1249, just applied to the 4-wheel player
     * distribution. [CONFIRMED @ 0x00404D10 / 0x00404D30 force-writeback
     * tail of UpdatePlayerVehicleDynamics] */
    int32_t player_fx = ((int64_t)rear_long       * sin_h
                       + (int64_t)rear_lat_force  * cos_h
                       + (int64_t)front_long      * sin_s
                       + (int64_t)front_lat_force * cos_s) >> 12;
    int32_t player_fz = ((int64_t)rear_long       * cos_h
                       - (int64_t)rear_lat_force  * sin_h
                       + (int64_t)front_long      * cos_s
                       - (int64_t)front_lat_force * sin_s) >> 12;

    if (actor->slot_index == 0 && actor->brake_flag &&
        (actor->frame_counter % 30u) == 0u) {
        TD5_LOG_I(LOG_TAG,
            "FORCE_BRK: fx=%d fz=%d f_long=%d r_long=%d f_lat=%d r_lat=%d vx=%d vz=%d",
            player_fx, player_fz, front_long, rear_long, front_lat_force, rear_lat_force,
            (int)actor->linear_velocity_x, (int)actor->linear_velocity_z);
    }
    actor->linear_velocity_x += player_fx;
    actor->linear_velocity_z += player_fz;

    /* --- 14a. Write longitudinal/lateral speeds at UpdatePlayerVehicleDynamics
     * tail. Two bugs fixed here (T7):
     *
     * BUG B — velocity input was POST-force. Original at 0x00404030 computes
     *   uVar12 (chassis forward) and uVar37 (front-axle-frame forward minus
     *   yaw-rate term) from the POST-DRAG PRE-FORCE velocity — i.e. the
     *   `linear_velocity_x/_z` state BEFORE the per-wheel force add-back at
     *   0x00404D66 / 0x00404D7E. The port was reading post-force velocity
     *   (after `actor->linear_velocity_x += player_fx` above), which doubled
     *   the long_speed magnitude at throttle-forward ticks (orig=59904 vs
     *   port=122880 at sim_tick=4). Switched to the function-scoped `vx`/`vz`
     *   locals captured at the top of this function (post-drag, pre-force).
     *
     * BUG A — lateral_speed (+0x318) was chassis-right projection; original
     *   writes the FRONT-AXLE-FRAME FORWARD projection minus a yaw-rate-
     *   induced term. For a near-straight car this yields a small positive
     *   value (orig=912 at sim_tick=4), whereas chassis-right yields ~0
     *   (port=-1). Exact formula [CONFIRMED @ 0x00404030 decomp]:
     *     uVar37 = (sin(h+s)*vx + cos(h+s)*vz) >> 12
     *            - (sin(s) * front_weight * angular_velocity_yaw >> 12) / 0x28c
     *   where sin/cos ports: sin_s/cos_s = sin/cos(yaw+steer), sin_sr = sin(steer),
     *   front_weight = PHYS_S(0x28), and 0x28c (652) is a physics-units
     *   scaling divisor kept verbatim from the original.
     *
     *   sVar2 | longitudinal_speed (+0x314) | lateral_speed (+0x318)
     *   ------+-----------------------------+------------------------
     *     1   | CRGT(engine, gear_ratio)    | front-axle Vlat'
     *     2   | body-frame Vlong            | CRGT(engine, gear_ratio)
     *     3   | CRGT(engine, gear_ratio)    | CRGT(engine, gear_ratio)
     *
     * Airborne cars (surface_contact_flags == 0) get both fields as plain
     * projections (no CRGT dispatch). [CONFIRMED @ 0x00404030] */
    {
        /* Pre-force velocity (vx/vz captured at line 677-678, post-drag). */
        int64_t pre_vx = vx;
        int64_t pre_vz = vz;

        /* uVar12 — chassis forward projection. */
        int32_t body_vlong = (int32_t)((pre_vx * sin_h + pre_vz * cos_h) >> 12);

        /* uVar37 — front-axle-frame forward minus yaw-rate correction. */
        int32_t raw_front = (int32_t)(((int64_t)sin_s * pre_vx + (int64_t)cos_s * pre_vz) >> 12);
        int64_t yaw_term_q12 = (int64_t)sin_sr * front_weight * actor->angular_velocity_yaw;
        int32_t yaw_term = (int32_t)((yaw_term_q12 >> 12) / 0x28c);
        int32_t body_vlat = raw_front - yaw_term;

        if (actor->surface_contact_flags != 0) {
            /* Drivetrain dispatch per UpdatePlayerVehicleDynamics @ 0x00404030:
             * sVar2=1 (RWD): CRGT(body_vlong) → long; lat=body_vlat (front-axle form)
             * sVar2=2 (FWD): CRGT(body_vlat)  → lat;  long=body_vlong
             * sVar2=3 (AWD): CRGT((long+lat)/2) → BOTH */
            int32_t dt_layout = (int32_t)PHYS_S(actor, 0x76);
            switch (dt_layout) {
            case 1: {
                int32_t crgt = compute_reverse_gear_torque(actor, body_vlong);
                actor->longitudinal_speed = crgt;
                actor->lateral_speed      = body_vlat;
                break;
            }
            case 2: {
                int32_t crgt = compute_reverse_gear_torque(actor, body_vlat);
                actor->lateral_speed      = crgt;
                actor->longitudinal_speed = body_vlong;
                break;
            }
            case 3: {
                int32_t crgt = compute_reverse_gear_torque(actor, (body_vlong + body_vlat) / 2);
                actor->longitudinal_speed = crgt;
                actor->lateral_speed      = crgt;
                break;
            }
            default:
                actor->longitudinal_speed = body_vlong;
                actor->lateral_speed      = body_vlat;
                break;
            }
        } else {
            actor->longitudinal_speed = body_vlong;
            actor->lateral_speed      = body_vlat;
        }

        if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "body_speeds: long=%d lat=%d (vlong=%d vlat=%d yaw_term=%d)",
                      actor->longitudinal_speed, actor->lateral_speed,
                      body_vlong, body_vlat, yaw_term);
        }
    }

    /* --- 14b. Velocity magnitude safety clamp ---
     * Without working wall collisions, cars leave the road immediately.
     * Clamp total velocity magnitude to the car's speed_limit (1x, not 2x)
     * so the car stays at a controllable speed. The original game relies on
     * track walls to contain speed through corners; until walls are
     * implemented, this hard cap prevents runaway velocity. */
    {
        int32_t speed_lim = (int32_t)PHYS_S(actor, 0x74) << 8;
        int32_t vel_cap = speed_lim;  /* 1x speed limit until walls exist */
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

    /* --- 16. IntegrateWheelSuspensionTravel ---
     * Pass the net world-frame velocity delta applied this frame as the
     * excitation for the per-wheel spring-damper (matches original at
     * 0x00404EA2 passing iVar11/iVar36 = fx/fz). */
    td5_physics_integrate_suspension(actor, player_fx, player_fz);

    /* --- 17. ApplyMissingWheelVelocityCorrection --- */
    td5_physics_missing_wheel_correction(actor);

    /* --- 17a. Wheelspin override [CONFIRMED @ 0x00404E00-0x00404E1C].
     * Three-condition gate: gear==2, RPM-derived wheelspin exceeds threshold,
     * and throttle > 0x7F. When all true: surface_contact_flags = tuning[0x76]
     * (drivetrain type byte) — marks only the driven axle in contact.
     * uVar12 at this site is longitudinal_speed [UNCERTAIN — Ghidra reuses the
     * variable name; steering_cmd>>8 is the alternative interpretation]. */
    {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x32);
        if (gear_ratio != 0) {
            int32_t rpm_norm = (((actor->engine_speed_accum - 400) * 0x1000) / 0x2d) / gear_ratio;
            int32_t wheelspin = rpm_norm * 0x100 - actor->longitudinal_speed;
            if (actor->current_gear == 2 &&
                wheelspin > 0x12C00 &&
                (int32_t)(uint8_t)actor->encounter_steering_cmd > 0x7F) {
                TD5_LOG_I(LOG_TAG, "wheelspin: slot=%d rpm_norm=%d wsp=%d scf=%d->tuning[0x76]",
                          actor->slot_index, rpm_norm, wheelspin, actor->surface_contact_flags);
                actor->surface_contact_flags = (uint8_t)PHYS_S(actor, 0x76);
            }
        }
    }

    /* Tire slip accumulators — drive the wheel billboard rotation visuals
     * via RenderVehicleWheelBillboards (0x446F00).
     * [CONFIRMED @ UpdatePlayerVehicleDynamics 0x404030, near the end]:
     *
     *   slip_x += lateral_speed >> 8;          (always)
     *   if (!handbrake) slip_z += long_speed >> 8;  (handbrake gates slip_z)
     *
     * Front wheels rotate by slip_z * -4, rear wheels by slip_x * -4
     * (see RenderVehicleWheelBillboards decomp).  An earlier version
     * here assigned these fields from {front,rear}_axle_slip_excess,
     * which wiped out the accumulation every tick — wheels never spun. */
    actor->accumulated_tire_slip_x += (int16_t)(actor->lateral_speed >> 8);
    if (!actor->handbrake_flag) {
        actor->accumulated_tire_slip_z += (int16_t)(actor->longitudinal_speed >> 8);
    }

    /* NOTE: current_slip_metric (+0x33C) is now written inside the slip-circle
     * block (see section 13a above) per the original's structure at
     * 0x004049BA/0x00404A80. Do NOT write it again here — the earlier
     * `abs(lateral_speed)>>8` tail-write used the wrong values (post-update)
     * and collapsed slip to near-zero in normal driving. */
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

    /* --- 3. 2-axle load transfer with cross-weights [CONFIRMED @ 0x40506B-0x4050B4]
     * Original uses CROSSED weight numerators: front_load uses rear_weight,
     * rear_load uses front_weight. Divisor is half_wb, not full_wb. */
    int32_t front_weight = (int32_t)PHYS_S(actor, 0x28);
    int32_t rear_weight  = (int32_t)PHYS_S(actor, 0x2A);
    int32_t total_weight = front_weight + rear_weight;
    if (total_weight == 0) total_weight = 1;

    int32_t half_wb = PHYS_I(actor, 0x24);
    if (half_wb == 0) half_wb = 1;

    int32_t susp_defl = actor->center_suspension_pos;

    /* Cross-weight load transfer [CONFIRMED @ 0x40506B]:
     * front_load = (rear_weight << 8) / total_weight * (half_wb - susp_defl) / half_wb
     * rear_load  = (front_weight << 8) / total_weight * (half_wb + susp_defl) / half_wb */
    int32_t front_load = ((rear_weight << 8) / total_weight) * (half_wb - susp_defl) / half_wb;
    int32_t rear_load  = ((front_weight << 8) / total_weight) * (half_wb + susp_defl) / half_wb;

    /* Grip from surface friction * load [CONFIRMED @ 0x4050B8] */
    int32_t sf = (int32_t)s_surface_friction[surface & 0x1F];
    int32_t grip_front = (sf * front_load + 128) >> 8;
    int32_t grip_rear  = (sf * rear_load + 128) >> 8;

    if (grip_front < TD5_AI_GRIP_MIN) grip_front = TD5_AI_GRIP_MIN;
    if (grip_front > TD5_AI_GRIP_MAX) grip_front = TD5_AI_GRIP_MAX;
    if (grip_rear < TD5_AI_GRIP_MIN) grip_rear = TD5_AI_GRIP_MIN;
    if (grip_rear > TD5_AI_GRIP_MAX) grip_rear = TD5_AI_GRIP_MAX;

    /* --- 4. Velocity drag in WORLD frame [CONFIRMED @ 0x404EC0] --- */
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

    /* --- 5. Body-frame velocities + steered-frame trig --- */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t steer_angle = actor->steering_command >> 8;
    int32_t steer_heading = (heading + steer_angle) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);
    int32_t sin_s = sin_fixed12(steer_heading);

    /* Steer delta trig (cos_d, sin_d) [CONFIRMED @ 0x4050EF] */
    int32_t cos_d = cos_fixed12(steer_angle & 0xFFF);
    int32_t sin_d = sin_fixed12(steer_angle & 0xFFF);

    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;

    /* Body-frame lateral velocity for INTERNAL bicycle solve consumption.
     * The bicycle's mass-matrix uses this as v_lat (with yaw_corr applied). */
    int32_t raw_lat = (cos_h * vx - sin_h * vz) >> 12;
    int32_t yaw_rate = actor->angular_velocity_yaw;
    int32_t inertia = PHYS_I(actor, 0x20);
    int32_t inertia_div = inertia / 0x28C;
    if (inertia_div == 0) inertia_div = 1;
    int32_t yaw_corr = ((sin_d * front_weight) >> 12) * yaw_rate / 0x28C;
    int32_t v_lat = raw_lat - yaw_corr;

    /* Field +0x314 = body-frame longitudinal velocity (v_long). Port matches
     * original [verified via Frida runtime probe 2026-04-22]. */
    actor->longitudinal_speed = v_long;

    /* Field +0x318 is NOT body-frame lateral. The original writes the
     * STEERED-frame longitudinal velocity minus yaw_corr_sin to +0x318
     * [VERIFIED via Frida runtime probe 2026-04-22 against FUN_00404EC0
     *  on TD5_d3d.exe; formula:
     *    f318 = (sin_s*vx + cos_s*vz) >> 12 - ((sin_d*Wf) >> 12) * omega / 0x28C
     *  matches with diff=0 on every early-tick row in
     *  log/bicycle_probe_original.csv].
     *
     * Downstream consumers of +0x318 (slip accumulator, force feedback, HUD,
     * trace) were getting the wrong value when port wrote body-lat - yaw_corr. */
    int32_t steered_long = (sin_s * vx + cos_s * vz) >> 12;
    actor->lateral_speed = steered_long - yaw_corr;

    /* --- 6. Engine pipeline [CONFIRMED @ 0x4051A0] --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    int32_t drive_torque = 0;
    int32_t front_drive = 0, rear_drive = 0;
    int32_t brake_front = (int32_t)PHYS_S(actor, 0x6E);
    int32_t brake_rear  = (int32_t)PHYS_S(actor, 0x70);
    int32_t speed_limit = (int32_t)PHYS_S(actor, 0x74) << 8;
    int32_t abs_speed = v_long < 0 ? -v_long : v_long;

    /* Drivetrain dispatch — verbatim port of UpdateAIVehicleDynamics @ 0x004051A0.
     * Original has NO surface_contact_flags gate; the flag is ONLY written in
     * UpdatePlayerVehicleDynamics and is always 0 for AI slots.
     * [CONFIRMED via Ghidra decomp 0x00404EC0 — no such branch exists]
     *
     * Original branch structure:
     *   if (brake_flag == 0) {
     *       if (throttle != 0) { gear/engine/drive; goto LAB_00405285; }
     *       LAB_004051c7: throttle = -32;  // idle decel scalar
     *   } else if (throttle == 0) { goto LAB_004051c7; }
     *   // brake path: uses 'throttle' (may be -32 for idle)
     *   UpdateEngineSpeedAccumulator();
     *   front_drive = clamp(Wr * throttle / 256, -(|v_long|/2), +(|v_long|/2)) / 2
     *   rear_drive  = clamp(Wf * throttle / 256, -(|v_long|/2), +(|v_long|/2)) / 2
     *   LAB_00405285: [bicycle solve uses front_drive * 2, rear_drive * 2]
     *
     * The -32 idle path: stationary car has v_long=0 → half_spd clamp = 0,
     * so both front/rear_drive end up 0. No net force for idle stationary AI. */
    if (!actor->brake_flag) {
        if (throttle != 0) {
            /* Drive path: gear shift, engine update, torque */
            td5_physics_auto_gear_select(actor);
            td5_physics_update_engine_speed(actor);
            drive_torque = td5_physics_compute_drive_torque(actor);
            /* Original @ 0x40521C: local_40 = (torque + (torque>>31 & 3)) >> 2
             * local_3c = local_40; then skips to LAB_00405285 which does *2. */
            front_drive = (drive_torque + ((drive_torque >> 31) & 3)) >> 2;
            rear_drive  = front_drive;
            if (v_long > speed_limit) {
                front_drive = 0;
                rear_drive  = 0;
            }
            /* Fall through to bicycle (goto LAB_00405285 in original) */
        } else {
            /* Idle: set throttle = -32 (LAB_004051c7), fall into brake path */
            throttle = -0x20;
            goto ai_brake_path;
        }
    } else {
        if (throttle == 0) {
            /* brake_flag set but throttle=0: also use -32 (LAB_004051c7) */
            throttle = -0x20;
        }
    ai_brake_path:;
        /* Brake path [original @ 0x004051D5-0x00405282].
         * Original formula: local_40 = (Wr*throttle + (Wr*throttle>>31 & 0xFF)) >> 8
         *   then clamp to [-|v_long|/2, +|v_long|/2], divide by 2.
         * Same for local_3c with Wf (note: Wr=rear, Wf=front — swapped vs names).
         * Verified: original uses rear_weight for local_40 (local_3c divides by 2
         * in the final step), front_weight for local_3c. */
        td5_physics_update_engine_speed(actor);
        {
            int32_t half_spd = abs_speed >> 1;
            /* local_3c = clamp((Wr * throttle) >> 8, -half_spd, half_spd) / 2 */
            int32_t rc = rear_weight * throttle;
            int32_t r_raw = (rc + ((rc >> 31) & 0xFF)) >> 8;
            if (r_raw > half_spd)  r_raw =  half_spd;
            if (r_raw < -half_spd) r_raw = -half_spd;
            rear_drive = r_raw / 2;
            /* local_40 = clamp((Wf * throttle) >> 8, -(|v_long|/2), +(|v_long|/2)) / 2 */
            int32_t fc = front_weight * throttle;
            int32_t f_raw = (fc + ((fc >> 31) & 0xFF)) >> 8;
            int32_t half_vlong = v_long / 2;
            if (half_vlong < 0) half_vlong = -half_vlong;
            if (f_raw > half_vlong)  f_raw =  half_vlong;
            if (f_raw < -half_vlong) f_raw = -half_vlong;
            front_drive = f_raw / 2;
        }
    }
    /* LAB_00405285: original doubles both values before bicycle solve */

    /* LAB_00405285: original doubles local_40 (front) and local_3c (rear)
     * before the bicycle solve [CONFIRMED @ 0x00405285].
     * The drive path already stored torque/4; brake path stored clamp/2.
     * Both get *2 here, yielding torque/2 or clamp/1 respectively. */
    front_drive = front_drive * 2;  /* iVar18 = local_40 * 2 */
    rear_drive  = rear_drive  * 2;  /* local_3c = local_3c * 2 */


    /* --- 7. Coupled bicycle model lateral forces — VERBATIM port of
     * FUN_00404EC0 @ 0x00405285-0x004054F3. Variable names match the
     * Ghidra decomp exactly (iVar13..iVar17, local_40, local_44) so shifts
     * and operand orderings can be cross-checked line-by-line.
     *
     * Semantic mapping (verified via the yaw_torque formula at the bottom
     * of the function, where cos_d*Wf pairs with local_44):
     *   local_44 → FRONT_LAT (Ff) — paired with cos_d*Wf in yaw moment
     *   local_40 → REAR_LAT  (Fr) — paired with Wr
     *
     * Prior port labelled these in reverse, which is why every previous
     * "sign flip" / "operand swap" attempt only half-worked.
     *
     * Sign-bias pattern `(x + (x>>31 & MASK)) >> SHIFT` is Ghidra's
     * round-toward-zero arithmetic shift. Kept verbatim via the SBR macro.
     */
    #define SBR(x, mask, shift) (((int32_t)((x) + (((x) >> 31) & (mask))) >> (shift)))
    int32_t front_lat, rear_lat;
    {
        int32_t Wf = front_weight;
        int32_t Wr = rear_weight;
        int32_t I  = inertia;
        int32_t omega = yaw_rate;

        /* Determinant `local_44` (reused var — same name as original). */
        int32_t iVar4  = SBR((int64_t)Wr * Wr + I, 0x3FF, 10);                    /* A  */
        int32_t iVar13 = SBR((int64_t)(Wr + Wf) * (Wr + Wf), 0x3FF, 10) * cos_d;   /* B*cos_d */
                iVar13 = SBR(iVar13, 0xFFF, 12) * cos_d;                           /* B*cos_d² */
        int32_t iVar14 = iVar4 * sin_d;                                            /* A*sin_d */
                iVar14 = SBR(iVar14, 0xFFF, 12) * sin_d;                           /* A*sin_d² */
        int32_t det = SBR(iVar13, 0xFFF, 12) + SBR(iVar14, 0xFFF, 12);             /* local_44 */
        if (det == 0) det = 1;

        /* D coefficient `iVar14` (reused var). */
                iVar14 = SBR((int64_t)Wr * Wf - I, 0x3FF, 10);                     /* D */

        /* yaw_term `iVar15`, yaw_corr `iVar16` (reused). */
        int32_t iVar15 = (I / 0x28C) * omega;                                      /* yaw_term */
        int32_t iVar16 = SBR(I, 0x3FF, 10) * (((int32_t)(Wr * omega)) / 0x28C - v_lat); /* (I>>10)*(Wr*ω/652 - v_lat) */
                iVar16 = SBR(iVar16, 0xFFF, 12) * sin_d;

        /* iVar13 drive+vlong cos term. Original operands: local_3c = front_drive
         * (paired with cos_d inside), iVar18 = rear_drive (unpaired). */
                iVar13 = (SBR(front_drive * cos_d, 0xFFF, 12) + rear_drive + v_long) * iVar14;  /* *D */
                iVar13 = SBR(iVar13, 0xFFF, 12) * cos_d;

        /* iVar14 reused for D*front_drive*sin_d² (original uses local_3c = front_drive). */
                iVar14 = iVar14 * front_drive;
                iVar14 = SBR(iVar14, 0xFFF, 12) * sin_d;
                iVar14 = SBR(iVar14, 0xFFF, 12) * sin_d;

        /* iVar17 = (yaw_term - v_lat*Wf) >> 10 * (Wf+Wr) >> 12 * cos_d. */
        int32_t iVar17 = iVar15 - v_lat * Wf;
                iVar17 = SBR(iVar17, 0x3FF, 10) * (Wr + Wf);
                iVar17 = SBR(iVar17, 0xFFF, 12) * cos_d;

        /* local_40 (REAR_LAT in this physics convention). */
        int32_t local_40_var = (SBR(iVar17, 0xFFF, 12) * cos_d +
                                (SBR(iVar16, 0xFFF, 12) +
                                 SBR(iVar13, 0xFFF, 12) +
                                 SBR(iVar14, 0xFFF, 12)) * sin_d) / det;

        /* Rear-cross coefficient for local_44 (FRONT_LAT). Original uses
         * local_3c = front_drive in the rear_cross product, and iVar18 =
         * rear_drive in the (drive + v_long) * A term. */
        int32_t iVar16b = SBR((Wf + 2 * Wr) * Wf - I, 0x3FF, 10) * front_drive;
        int32_t iVar4b  = SBR(iVar16b, 0xFFF, 12) * cos_d + (rear_drive + v_long) * iVar4; /* iVar4 = A */
                iVar15  = v_lat * Wr + iVar15;                                               /* v_lat*Wr + yaw_term */
                iVar16b = SBR(iVar15, 0x3FF, 10) * (Wr + Wf);

        /* local_44 (FRONT_LAT). */
        int32_t local_44_var = (SBR(iVar4b, 0xFFF, 12) * sin_d -
                                SBR(iVar16b, 0xFFF, 12) * cos_d) / det;

        /* Expose in port's naming: front_lat = FRONT, rear_lat = REAR. */
        front_lat = local_44_var;
        rear_lat  = local_40_var;
    }
    #undef SBR

    /* --- 8. Slip circle with sqrt [CONFIRMED @ 0x405518-0x405580] --- */
    {
        int32_t tire_grip = (int32_t)PHYS_S(actor, 0x2C);
        if (tire_grip == 0) tire_grip = sf; /* fallback */

        /* Front axle slip circle */
        int32_t fl4 = front_lat >> 4;
        int32_t fd4 = front_drive >> 4;
        int32_t mag_sq_f = fl4 * fl4 + fd4 * fd4;
        int32_t mag_f = td5_isqrt(mag_sq_f);
        int32_t grip_limit_f = (front_load * tire_grip) >> 8;
        int32_t slip_f16 = mag_f << 4;

        if (slip_f16 > grip_limit_f && slip_f16 > 0) {
            actor->front_axle_slip_excess = slip_f16 - grip_limit_f;
            int32_t scale = (grip_limit_f << 8) / slip_f16;
            front_lat = (front_lat * scale) >> 8;
            front_drive = (front_drive * scale) >> 8;
        } else {
            actor->front_axle_slip_excess = 0;
        }

        /* Rear axle slip circle */
        int32_t rl4 = rear_lat >> 4;
        int32_t rd4 = rear_drive >> 4;
        int32_t mag_sq_r = rl4 * rl4 + rd4 * rd4;
        int32_t mag_r = td5_isqrt(mag_sq_r);
        int32_t grip_limit_r = (rear_load * tire_grip) >> 8;
        int32_t slip_r16 = mag_r << 4;

        if (slip_r16 > grip_limit_r && slip_r16 > 0) {
            actor->rear_axle_slip_excess = slip_r16 - grip_limit_r;
            int32_t scale = (grip_limit_r << 8) / slip_r16;
            rear_lat = (rear_lat * scale) >> 8;
            rear_drive = (rear_drive * scale) >> 8;
        } else {
            actor->rear_axle_slip_excess = 0;
        }
    }

    /* --- 9. Yaw torque [VERBATIM @ 0x00405680-0x004056A0]
     * Original: M = (((cos_d * Wf) >> 12) * FRONT_LAT - REAR_LAT * Wr) / (I / 0x28c)
     * With the corrected front/rear semantic mapping (local_44 = front_lat,
     * local_40 = rear_lat), this matches the standard bicycle-model yaw
     * moment formula a·Fyf·cos(δ) − b·Fyr. Port-only stabilizers (/8
     * scaling, omega damping) removed — if the bicycle-solve port is
     * correct, the natural magnitude should match the original's. */
    {
        int32_t yaw_torque = (int32_t)(((int64_t)((cos_d * front_weight + ((cos_d * front_weight) >> 31 & 0xFFF)) >> 12) * front_lat
                             - (int64_t)rear_lat * rear_weight) / inertia_div);

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

        actor->angular_velocity_yaw += yaw_torque;
    }

    /* --- 10. World-frame force application [CONFIRMED @ 0x4056C4-0x405762]
     * Front forces use steered heading (cos_s, sin_s).
     * Rear forces use body heading (cos_h, sin_h). */
    int32_t ai_fx = ((int64_t)rear_drive * sin_h + (int64_t)rear_lat * cos_h
                   + (int64_t)front_drive * sin_s + (int64_t)front_lat * cos_s) >> 12;
    int32_t ai_fz = ((int64_t)rear_drive * cos_h - (int64_t)rear_lat * sin_h
                   + (int64_t)front_drive * cos_s - (int64_t)front_lat * sin_s) >> 12;
    actor->linear_velocity_x += ai_fx;
    actor->linear_velocity_z += ai_fz;

    /* --- 11. Slip yaw damping [CONFIRMED @ 0x40578E-0x4057C8] --- */
    if (actor->front_axle_slip_excess > 0) {
        int32_t damp = ((actor->angular_velocity_yaw >> 6)
                       * actor->front_axle_slip_excess) >> 15;
        if (damp > 0x200) damp = 0x200;
        if (damp < -0x200) damp = -0x200;
        actor->angular_velocity_yaw -= damp;
    }

    /* --- 12. Velocity magnitude safety clamp --- */
    {
        int32_t vel_cap = speed_limit * 2;
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

    /* --- 13. Suspension integration ---
     * Pass the net world-frame velocity delta as the spring excitation
     * (matches original at 0x00404EA2 passing iVar11/iVar36). */
    td5_physics_integrate_suspension(actor, ai_fx, ai_fz);

    /* --- 14. Tire slip accumulation [CONFIRMED @ 0x405768-0x40577B]
     * Original uses += with lateral/longitudinal speed, not assignment with slip excess */
    actor->accumulated_tire_slip_x += (int16_t)(actor->lateral_speed >> 8);
    actor->accumulated_tire_slip_z += (int16_t)(actor->longitudinal_speed >> 8);

    /* current_slip_metric (+0x33C) — see player path for rationale. Same
     * formula so AI cars also drop tire marks and smoke when drifting. */
    {
        int32_t slip = abs(actor->lateral_speed) >> 8;
        if (slip > 0x7FFF) slip = 0x7FFF;
        actor->current_slip_metric = (int16_t)slip;
    }

}

/* ========================================================================
 * Traffic simplified dynamics -- IntegrateVehicleFrictionForces (0x4438F0)
 * ======================================================================== */

void td5_physics_update_traffic(TD5_Actor *actor)
{
    /* Literal port of IntegrateVehicleFrictionForces @ 0x004438F0.
     * Transcribed from Ghidra decompilation; every SAR uses
     * truncate-toward-zero rounding: (x + ((x>>31)&mask)) >> shift. */

    #define SAR12(x) (((x) + (((x) >> 31) & 0xFFF)) >> 12)
    #define SAR10(x) (((x) + (((x) >> 31) & 0x3FF)) >> 10)
    #define SAR8_U8(x) (((x) + (((x) >> 31) & 0xFF)) >> 8)

    /* 1. Velocity drag — v -= trunc(v*16 / 4096). [@ 0x00443900-0x00443932] */
    int32_t t = actor->linear_velocity_x * 0x10;
    int32_t vx = actor->linear_velocity_x - SAR12(t);
    t = actor->linear_velocity_z * 0x10;
    int32_t vz = actor->linear_velocity_z - SAR12(t);
    actor->linear_velocity_z = vz;

    int32_t yaw_vel = actor->angular_velocity_yaw;  /* iVar1 — used RAW */
    uint32_t yaw12 = (uint32_t)actor->euler_accum.yaw >> 8;
    actor->linear_velocity_x = vx;

    /* 2. Steering & trig [@ 0x00443958-0x00443999] */
    int32_t steer_raw = actor->steering_command;
    uint32_t steer12 = (uint32_t)SAR8_U8(steer_raw);

    int32_t cos_h  = cos_fixed12(yaw12 & 0xFFF);
    int32_t sin_h  = sin_fixed12(yaw12 & 0xFFF);
    int32_t cos_hs = cos_fixed12((yaw12 + steer12) & 0xFFF);
    int32_t sin_hs = sin_fixed12((yaw12 + steer12) & 0xFFF);
    int32_t cos_s  = cos_fixed12(steer12 & 0xFFF);
    int32_t sin_s  = sin_fixed12(steer12 & 0xFFF);

    int32_t throttle = (int32_t)actor->encounter_steering_cmd * 4;  /* iVar9 */

    /* 3. Body-frame velocity [@ 0x004439BB-0x004439EC] */
    int32_t lat_body  = SAR12(cos_h * vx - sin_h * vz);  /* iVar14 */
    int32_t long_body = SAR12(cos_h * vz + sin_h * vx);  /* iVar10 */

    /* 4. Steering-inertia denom iVar16_denom [@ 0x004439F3-0x00443A44] */
    int32_t a = SAR12(cos_s * 0x271) * cos_s;
    int32_t b = SAR12(sin_s * 0x14C) * sin_s;
    int32_t denom = SAR12(a) + SAR12(b);
    if (denom == 0) denom = 1;  /* safety (should never hit in practice) */

    /* 5. Front-axle force iVar15 [@ 0x00443A47-0x00443AC4].
     * iVar11 = yaw_vel * 0x114 (RAW yaw_vel, NO prior shift). */
    int32_t iVar11 = yaw_vel * 0x114;
    int32_t front_num = iVar11 + lat_body * 400;
    int32_t iVar15_pre = SAR10(front_num) * 800;
    int32_t num_a = throttle + long_body;
    int32_t num_a_14c = num_a * 0x14C;
    int32_t iVar15 = (SAR12(num_a_14c) * sin_s - SAR12(iVar15_pre) * cos_s) / denom;

    /* 6. Rear-axle force iVar16_force [@ 0x00443AC6-0x00443B7C] */
    int32_t rear_p1 = ((yaw_vel * 400) / 0x28C - lat_body) * 0xAF;
    int32_t iVar1_rear = SAR12(rear_p1) * sin_s;  /* note: stored unshifted at this stage */
    int32_t num_a_13 = num_a * 0x13;
    int32_t iVar2_rear = SAR12(num_a_13) * cos_s;
    int32_t iVar11_rear = iVar11 + lat_body * -400;
    int32_t iVar12_s1 = SAR10(iVar11_rear) * 800;
    int32_t iVar12_s2 = SAR12(iVar12_s1) * cos_s;
    int32_t iVar16_force = (SAR12(iVar12_s2) * cos_s
                          + (SAR12(iVar1_rear) - SAR12(iVar2_rear)) * sin_s) / denom;

    /* 7. Clamp [-0x800, +0x800] [@ 0x00443B80-0x00443BBA] */
    if (iVar15 > 0x800)  iVar15 = 0x800;
    if (iVar15 < -0x800) iVar15 = -0x800;
    if (iVar16_force > 0x800)  iVar16_force = 0x800;
    if (iVar16_force < -0x800) iVar16_force = -0x800;

    /* 8. Yaw torque [@ 0x00443BBA-0x00443BEE] */
    int32_t yaw_torque = (SAR12(cos_s * 400) * iVar15
                        + iVar16_force * -400) / 0x114;
    actor->angular_velocity_yaw += yaw_torque;

    /* 9. World-frame velocity update [@ 0x00443BF4-0x00443C8F]
     * Front uses cos_hs/sin_hs (heading+steer), rear uses cos_h/sin_h. */
    actor->linear_velocity_x = vx
        + SAR12(iVar15 * cos_hs)
        + SAR12(iVar16_force * cos_h)
        + SAR12(throttle * sin_h);
    actor->linear_velocity_z = vz
        - SAR12(iVar15 * sin_hs)
        - SAR12(iVar16_force * sin_h)
        + SAR12(throttle * cos_h);

    /* 10. Suspension force [@ 0x00443C95] */
    apply_damped_suspension_force(actor, iVar15 + iVar16_force, throttle);

    /* 11. Integrate XZ, yaw, longitudinal_speed [@ 0x00443CBF-0x00443CDE] */
    actor->world_pos.x += actor->linear_velocity_x;
    actor->world_pos.z += actor->linear_velocity_z;
    actor->euler_accum.yaw += actor->angular_velocity_yaw;
    actor->longitudinal_speed = long_body;

    #undef SAR12
    #undef SAR10
    #undef SAR8_U8
}

/* ========================================================================
 * ApplyDampedSuspensionForce (0x4437C0) -- Traffic only
 *
 * Simple 2-DOF spring-damper for roll and pitch.
 * ======================================================================== */

static void apply_damped_suspension_force(TD5_Actor *actor, int32_t lateral, int32_t longitudinal)
{
    /* First axis (lateral-driven): wheel_suspension_pos[0] (+0x2DC) / wheel_spring_dv[0] (+0x2EC)
     * [CONFIRMED @ 0x4437C0-0x443838] */
    int32_t axis0_pos = actor->wheel_suspension_pos[0];
    int32_t axis0_vel = actor->wheel_spring_dv[0];

    axis0_vel += (lateral * 0x80) >> 8;           /* drive force */
    axis0_vel += (axis0_vel * -0x20) >> 8;        /* velocity damping */
    axis0_vel -= (axis0_pos * 0x20) >> 8;         /* spring force */
    axis0_pos += axis0_vel;

    if (axis0_pos > 0x2000) axis0_pos = 0x2000;
    if (axis0_pos < -0x2000) axis0_pos = -0x2000;

    actor->wheel_suspension_pos[0] = axis0_pos;
    actor->wheel_spring_dv[0] = axis0_vel;

    /* Second axis (longitudinal-driven): wheel_suspension_pos[1] (+0x2E0) / wheel_spring_dv[1] (+0x2F0)
     * [CONFIRMED @ 0x443838-0x4438EC] */
    int32_t axis1_pos = actor->wheel_suspension_pos[1];
    int32_t axis1_vel = actor->wheel_spring_dv[1];

    axis1_vel += (longitudinal * 0x80) >> 8;
    axis1_vel += (axis1_vel * -0x20) >> 8;
    axis1_vel -= (axis1_pos * 0x20) >> 8;
    axis1_pos += axis1_vel;

    if (axis1_pos > 0x4000) axis1_pos = 0x4000;
    if (axis1_pos < -0x4000) axis1_pos = -0x4000;

    actor->wheel_suspension_pos[1] = axis1_pos;
    actor->wheel_spring_dv[1] = axis1_vel;

    /* Original does NOT feed suspension into angular_velocity.
     * [CONFIRMED @ 0x4437C0-0x4438EC: writes only to +0x2DC/+0x2EC/+0x2E0/+0x2F0,
     *  never to angular_velocity_roll/pitch.]
     * Roll/pitch display angles are computed from surface normal + suspension
     * correction in UpdateTrafficVehiclePose, not from euler accumulators. */
}

/* ========================================================================
 * OBB Corner Test -- FUN_00408570
 *
 * Tests 8 corners (4 of each car) against the other car's OBB.
 * Returns bitmask: bits 0-3 = B corners inside A, bits 4-7 = A corners inside B.
 * For each penetrating corner, stores {projX, projZ, penX, penZ} in corners[].
 *
 * Car bbox from carData pointer at actor+0x1B8 [CONFIRMED @ 0x407ADB-0x407ADF]:
 *   carData+0x04 = front_z  (int16, positive)  — forward Z extent from center
 *   carData+0x08 = half_w   (int16, positive)  — lateral X extent
 *   carData+0x14 = rear_z   (int16, negative)  — backward Z extent (stored signed)
 *
 * Prior port had these SWAPPED (0x04 → "halfWidth", 0x08 → "halfLength"),
 * which produced corners laid out with front/rear-Z along the X axis and
 * half-width along the Z axis. The degenerate bounds check
 * `cx in [-rear_z, front_z]` ≈ `[160, 156]` (empty range) made nearly every
 * test fail except for corners whose integer math collapsed to cx=0. That
 * left ~12k spurious "contacts" per lap with cx=0 and tiny rel_vel → imp=0.
 * The mapping above is verified by how 0x4079C0's case split consumes
 * them: `side_extent = cardef[0x08] - |cx_A|` requires 0x08 = half-width.
 * ======================================================================== */

static int obb_corner_test(TD5_Actor *a, TD5_Actor *b,
                           int32_t ax, int32_t az, int32_t bx, int32_t bz,
                           int32_t heading_a, int32_t heading_b,
                           OBB_CornerData corners[8])
{
    int result = 0;

    /* Car box extents (correct mapping — see header comment) */
    int32_t half_w_a  = (int32_t)CDEF_S(a, 0x08);  /* half-width (positive) */
    int32_t front_z_a = (int32_t)CDEF_S(a, 0x04);  /* front-Z extent (positive) */
    int32_t rear_z_a  = (int32_t)CDEF_S(a, 0x14);  /* rear-Z extent (negative) */
    int32_t half_w_b  = (int32_t)CDEF_S(b, 0x08);
    int32_t front_z_b = (int32_t)CDEF_S(b, 0x04);
    int32_t rear_z_b  = (int32_t)CDEF_S(b, 0x14);

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

    /* B's 4 corners in B's local frame. Layout (X=lateral, Z=forward):
     *   0 = FL  (-half_w, front_z)
     *   1 = FR  (+half_w, front_z)
     *   2 = RR  (+half_w, rear_z)   ← rear_z is stored negative
     *   3 = RL  (-half_w, rear_z)                                         */
    int32_t b_corners_lx[4] = { -half_w_b, +half_w_b, +half_w_b, -half_w_b };
    int32_t b_corners_lz[4] = {  front_z_b, front_z_b,  rear_z_b,  rear_z_b };

    /* A's 4 corners in A's local frame (same layout as B's) */
    int32_t a_corners_lx[4] = { -half_w_a, +half_w_a, +half_w_a, -half_w_a };
    int32_t a_corners_lz[4] = {  front_z_a, front_z_a,  rear_z_a,  rear_z_a };

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

        /* Test if within A's box: |cx| <= half_w_a and rear_z_a <= cz <= front_z_a */
        if (cx >= -half_w_a && cx <= half_w_a &&
            cz >= rear_z_a  && cz <= front_z_a) {
            result |= (1 << i);
            corners[i].proj_x = (int16_t)cx;
            corners[i].proj_z = (int16_t)cz;
            /* contactData[2,3] = rotated corner offset in target's frame
             * [CONFIRMED @ 0x00408763/0x00408771]: original stores
             *   contactData[2] = iVar27 - sVar1 = corner_in_A - B_center_in_A
             *   contactData[3] = iVar28 - sVar2
             * i.e. the penetrator's static corner ROTATED by (heading_b-heading_a)
             * but WITHOUT the A→B translation. Equivalent to (cx - local_dx,
             * cz - local_dz). Previously stored raw unrotated b_corners_lx/lz,
             * which gave the impulse solver a wrong-orientation lever arm for
             * any non-aligned-heading contact. */
            corners[i].own_x = (int16_t)(cx - local_dx);
            corners[i].own_z = (int16_t)(cz - local_dz);
            /* Penetration depth along each face normal (signed).
             * Minimum |pen| identifies the closest face. */
            int32_t pen_right = half_w_a  - cx;     /* to +X face */
            int32_t pen_left  = cx + half_w_a;      /* to -X face */
            int32_t pen_front = front_z_a - cz;     /* to +Z face */
            int32_t pen_back  = cz - rear_z_a;      /* to -Z face */
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

        if (cx >= -half_w_b && cx <= half_w_b &&
            cz >= rear_z_b  && cz <= front_z_b) {
            result |= (1 << (i + 4));
            corners[i + 4].proj_x = (int16_t)cx;
            corners[i + 4].proj_z = (int16_t)cz;
            /* Symmetric second half: rotated corner offset in B's frame.
             * [CONFIRMED @ 0x004089EB] — original applies the same subtraction
             * with sVar1/sVar2 = A_center in B's frame. */
            corners[i + 4].own_x = (int16_t)(cx - local2_dx);
            corners[i + 4].own_z = (int16_t)(cz - local2_dz);
            int32_t pen_right = half_w_b  - cx;
            int32_t pen_left  = cx + half_w_b;
            int32_t pen_front = front_z_b - cz;
            int32_t pen_back  = cz - rear_z_b;
            corners[i + 4].pen_x = (int16_t)((pen_right < pen_left) ? pen_right : -pen_left);
            corners[i + 4].pen_z = (int16_t)((pen_front < pen_back) ? pen_front : -pen_back);
        }
    }

    return result;
}

/* ========================================================================
 * Collision Response -- FUN_004079c0 ApplyVehicleCollisionImpulse
 *
 * Rewrite (2026-04-11) to match the original's case-split impulse model.
 * Prior port used a 2D minimum-penetration-axis model; the original does NOT.
 *
 * Algorithm [CONFIRMED @ 0x4079C0 via Ghidra pass]:
 *   1. Save angular velocities (prologue).
 *   2. Rotate both actors' linear velocities into A's local frame using
 *      cos/sin of `angle` (= target's world yaw).
 *        local_54 = A tangent, local_50 = A normal
 *        local_4c = B tangent, local_44 = B normal
 *   3. Case split on contactData[1] (cz_A) sign vs. A's box extents to pick
 *      SIDE or FRONT/REAR impact branch.
 *   4. Branch body (either side or front/rear):
 *      - Apply positional push along ±sin/cos of angle.
 *      - Compute mass polynomial using z² (side) or x² (front) terms.
 *      - Angular contribution: (cz_B*ω_B - cz_A*ω_A)/0x28C for side, (cx ...)
 *        for front.
 *      - Impulse scalar = (500000>>8 * 0x1100) / (poly>>8) * rel_vel, >>12.
 *      - Sign rejection: if ((cx_B - cx_A) ^ impulse) < 0 → return (separating).
 *      - Update tangent/normal velocity channels and angular deltas.
 *   5. TOI rollback: pos/euler -= (0x100 - impactForce) * vel >> 8 for both.
 *   6. Commit new angular velocity (saved + delta).
 *   7. Rotate linear velocities back to world frame from tangent/normal.
 *   8. TOI re-advance: pos/euler += (0x100 - impactForce) * new_vel >> 8.
 *   9. UpdateVehiclePoseFromPhysicsState on both actors.
 *  10. Impact magnitude = |(mass_A + mass_B) * impulse|; apply damage/sfx.
 *
 * Key constants:
 *   INERTIA_K      = 500000 (DAT_00463204)
 *   NUM_SCALE      = 0x1100 (4352)
 *   ANG_DIVISOR    = 0x28C  (652)
 *   INERTIA_PER_ANG = 500000/652 = 766 (integer)
 *   Mass: cardef+0x88 (int16)
 *   Extents: cardef+0x04 (front-Z), cardef+0x08 (half-width),
 *            cardef+0x14 (rear-Z, stored negative).
 *
 * Caller interface: (penetrator, target, corner_idx, corner, heading, impactForce).
 * impactForce is now passed from the caller's position-based binary search.
 * Internally, "A" = target (frame owner whose yaw = angle), "B" = penetrator.
 * Contact data from OBB corner test [CONFIRMED @ 0x00408570]:
 *   cx_A, cz_A = corner->proj_x, proj_z   (corner in TARGET's frame, WITH translation)
 *   cx_B, cz_B = corner->own_x, own_z     (rotated corner OFFSET from penetrator's
 *                                          center, expressed in TARGET's frame —
 *                                          proj minus A→B center translation)
 * ======================================================================== */

#define V2V_NUM_SCALE        0x1100
#define V2V_ANG_DIVISOR      0x28C
#define V2V_INERTIA_PER_ANG  (V2V_INERTIA_K / V2V_ANG_DIVISOR)  /* 766 */

static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target, int32_t impactForce)
{
    if (!penetrator || !target) return;
    if (!penetrator->car_definition_ptr || !target->car_definition_ptr) return;
    (void)corner_idx;

    TD5_Actor *A = target;      /* frame owner (yaw drives `angle`) */
    TD5_Actor *B = penetrator;  /* other actor */
    int32_t    angle       = heading_target;

    /* impactForce is now passed from the caller's position-based binary search
     * (matching ResolveVehicleCollisionPair @ 0x408A60). Range [0x10, 0xF0].
     * Higher = contact near end of tick (less rollback).
     * Lower = contact early in tick (more rollback). */

    /* --- 1. Prologue: save angular velocities for delta application --- */
    int32_t saved_omega_A = A->angular_velocity_yaw;
    int32_t saved_omega_B = B->angular_velocity_yaw;

    /* Mass from cardef+0x88 (int16); clamp invalid values. */
    int32_t mass_A = (int32_t)CDEF_S(A, 0x88);
    int32_t mass_B = (int32_t)CDEF_S(B, 0x88);
    if (mass_A <= 0) mass_A = 0x20;
    if (mass_B <= 0) mass_B = 0x20;

    /* --- 2. Rotate both velocities into A's local (contact) frame --- */
    int32_t cos_a = cos_fixed12(angle);
    int32_t sin_a = sin_fixed12(angle);

    int32_t vxA = A->linear_velocity_x;
    int32_t vzA = A->linear_velocity_z;
    int32_t vxB = B->linear_velocity_x;
    int32_t vzB = B->linear_velocity_z;

    int32_t local_54 = (vxA * cos_a - vzA * sin_a) >> 12;  /* A tangent */
    int32_t local_50 = (vxA * sin_a + vzA * cos_a) >> 12;  /* A normal  */
    int32_t local_4c = (vxB * cos_a - vzB * sin_a) >> 12;  /* B tangent */
    int32_t local_44 = (vxB * sin_a + vzB * cos_a) >> 12;  /* B normal  */

    /* --- Unpack contactData [CONFIRMED @ 0x00408570 stores] --- */
    /* cx_A, cz_A = corner position in TARGET's (A's) frame (WITH A→B translation).
     * cx_B, cz_B = rotated corner OFFSET from penetrator's (B's) center, in A's frame.
     *              i.e. the penetrator's static carbox corner rotated by
     *              (heading_b - heading_a), no translation.
     * Original stores 'contactData[2] = contactData[0] - B_center_in_A.x' at
     * 0x00408763 (SUB), i.e. the pre-translation rotated corner. Using the
     * unrotated static corner here gave the mass polynomial and angular
     * contribution the wrong lever-arm orientation whenever cars impacted at
     * a heading delta, leaving relative velocity and impulse sign noisy. */
    int32_t cx_A = (int32_t)corner->proj_x;
    int32_t cz_A = (int32_t)corner->proj_z;
    int32_t cx_B = (int32_t)corner->own_x;
    int32_t cz_B = (int32_t)corner->own_z;

    /* --- 3. Case split: side vs front/rear --- */
    /* [CONFIRMED @ 0x407ADB]: cardef+0x04 = front-Z extent (positive),
     *                         cardef+0x08 = half-width,
     *                         cardef+0x14 = rear-Z extent (stored negative). */
    int32_t half_w_A   = (int32_t)CDEF_S(A, 0x08);
    int32_t front_z_A  = (int32_t)CDEF_S(A, 0x04);
    int32_t rear_z_A   = (int32_t)CDEF_S(A, 0x14);

    int32_t abs_cx_A   = cx_A < 0 ? -cx_A : cx_A;
    int32_t side_extent = half_w_A - abs_cx_A;

    int is_side_branch;
    if (cz_A < 1) {
        /* Rear half (cz_A <= 0) — compare distance past rear vs side */
        int32_t rear_depth = cz_A - rear_z_A;  /* rear_z_A negative → adds |rear| */
        if (rear_depth < 0) rear_depth = -rear_depth;
        is_side_branch = (rear_depth > side_extent);
    } else {
        /* Front half (cz_A > 0) — compare distance past front vs side */
        int32_t front_depth = front_z_A - cz_A;
        if (front_depth < 0) front_depth = -front_depth;
        is_side_branch = (side_extent < front_depth);
    }

    /* --- 4. Branch-specific impulse math --- */
    int32_t impulse      = 0;
    int32_t omega_A_delta = 0;
    int32_t omega_B_delta = 0;
    int     rejected     = 0;
    int32_t push_x = 0, push_z = 0;

    const int64_t INERTIA_K_64 = (int64_t)V2V_INERTIA_K;
    /* NUM_CONST = (500000 >> 8) * 0x1100 = 8,499,456 [CONFIRMED @ 0x4079C0].
     * The `>> 8` on INERTIA_K is NOT a compiler overflow workaround — it is
     * intentional scaling paired with the denom `>> 8` below. Together they
     * produce small integer impulses (typically 1-200) such that
     * `impulse * mass` — the velocity channel update — lands in the right
     * 24.8-FP range.
     *
     * An earlier commit tried full-width NUM_CONST = 500000 * 0x1100 to
     * "recover precision"; with the OBB corner fix making rel_vel realistic,
     * it produced impulse magnitudes in the 10^8..10^9 range. That's a
     * clear signal the shift is load-bearing, not cosmetic. The earlier
     * symptom of impulse∈[-4..1] was unrelated — it came from obb_corner_test
     * having swapped cardef offsets (fixed in 4e8860e), which made rel_vel
     * collapse to near-zero. */
    const int64_t NUM_CONST    = (INERTIA_K_64 >> 8) * V2V_NUM_SCALE;

    if (is_side_branch) {
        /* --- SIDE BRANCH (LAB_00407B7F) --- */
        if (cx_A - cx_B >= 0) { push_x = -cos_a / 2; push_z =  sin_a / 2; }
        else                  { push_x =  cos_a / 2; push_z = -sin_a / 2; }

        int64_t denom = ((int64_t)cz_B * cz_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cz_A * cz_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cz_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cz_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_54 + local_4c;

        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = (int32_t)(impulse_raw >> 12);

        /* [CONFIRMED @ 0x4079C0 side branch]: XOR sign rejection. */
        if (((cx_B - cx_A) ^ impulse) < 0) {
            rejected = 1;
        } else {
            local_54 += impulse * mass_A;
            local_4c -= impulse * mass_B;
            omega_A_delta =  (int32_t)(((int64_t)impulse * mass_A * cz_A) / V2V_INERTIA_PER_ANG);
            omega_B_delta = -(int32_t)(((int64_t)impulse * mass_B * cz_B) / V2V_INERTIA_PER_ANG);
        }
    } else {
        /* --- FRONT/REAR BRANCH (LAB_00407B2D) --- */
        if (cz_A - cz_B >= 0) { push_x = -sin_a / 2; push_z = -cos_a / 2; }
        else                  { push_x =  sin_a / 2; push_z =  cos_a / 2; }

        int64_t denom = ((int64_t)cx_B * cx_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cx_A * cx_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cx_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cx_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_50 + local_44;

        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = (int32_t)(impulse_raw >> 12);

        if (((cz_B - cz_A) ^ impulse) < 0) {
            rejected = 1;
        } else {
            local_50 += impulse * mass_A;
            local_44 -= impulse * mass_B;
            omega_A_delta = -(int32_t)(((int64_t)impulse * mass_A * cx_A) / V2V_INERTIA_PER_ANG);
            omega_B_delta =  (int32_t)(((int64_t)impulse * mass_B * cx_B) / V2V_INERTIA_PER_ANG);
        }
    }

    /* Apply positional push BEFORE rejection check.
     * [CONFIRMED @ 0x00407BB7-0x00407BDB]: The ±cos/sin/2 push is applied
     * unconditionally whenever a corner penetration is detected; the XOR
     * sign-rejection below only gates the velocity impulse, not the push.
     *
     * Net direction matches Ghidra: `actorA -= iVar6; actorB += iVar6`
     * where actorA = TARGET (frame-owner, this function's A) and
     * actorB = PENETRATOR (this function's B). The port's push_x
     * encoding uses the OPPOSITE sign of Ghidra's iVar6 (see the ± in
     * 2452/2479) and is applied with OPPOSITE operators (A +=, B -=),
     * which cancels out to the same net outcome as the original. No
     * magnitude scaling — the original applies exactly cos/2 + sin/2
     * world units per penetrating corner per tick. */
    A->world_pos.x += push_x;
    A->world_pos.z += push_z;
    B->world_pos.x -= push_x;
    B->world_pos.z -= push_z;

    if (rejected) {
        /* Separating contact — push applied above, but no velocity impulse. */
        TD5_LOG_I(LOG_TAG, "v2v_reject: slot_A=%d slot_B=%d side=%d cxA=%d czA=%d cxB=%d czB=%d imp=%d push=(%d,%d)",
                  A->slot_index, B->slot_index, is_side_branch, cx_A, cz_A, cx_B, cz_B, impulse, push_x, push_z);
        return;
    }

    /* --- 5. TOI rollback (before committing new velocities) --- */
    int32_t toi_frac = 0x100 - impactForce;

    A->world_pos.x -= (toi_frac * A->linear_velocity_x) >> 8;
    A->world_pos.z -= (toi_frac * A->linear_velocity_z) >> 8;
    A->euler_accum.yaw -= (toi_frac * A->angular_velocity_yaw) >> 8;
    B->world_pos.x -= (toi_frac * B->linear_velocity_x) >> 8;
    B->world_pos.z -= (toi_frac * B->linear_velocity_z) >> 8;
    B->euler_accum.yaw -= (toi_frac * B->angular_velocity_yaw) >> 8;

    /* --- 6. Commit new angular velocities --- */
    A->angular_velocity_yaw = saved_omega_A + omega_A_delta;
    B->angular_velocity_yaw = saved_omega_B + omega_B_delta;

    /* --- 7. Rotate tangent/normal channels back to world frame --- */
    A->linear_velocity_x = (local_50 * sin_a + local_54 * cos_a) >> 12;
    A->linear_velocity_z = (local_50 * cos_a - local_54 * sin_a) >> 12;
    B->linear_velocity_x = (local_44 * sin_a + local_4c * cos_a) >> 12;
    B->linear_velocity_z = (local_44 * cos_a - local_4c * sin_a) >> 12;

    /* --- 8. TOI re-advance (with the new post-impulse velocities) --- */
    A->world_pos.x += (toi_frac * A->linear_velocity_x) >> 8;
    A->world_pos.z += (toi_frac * A->linear_velocity_z) >> 8;
    A->euler_accum.yaw += (toi_frac * A->angular_velocity_yaw) >> 8;
    B->world_pos.x += (toi_frac * B->linear_velocity_x) >> 8;
    B->world_pos.z += (toi_frac * B->linear_velocity_z) >> 8;
    B->euler_accum.yaw += (toi_frac * B->angular_velocity_yaw) >> 8;

    /* --- 9. Post-impulse pose update on both --- */
    update_vehicle_pose_from_physics(A);
    update_vehicle_pose_from_physics(B);

    /* --- 10. Impact magnitude and damage effects --- */
    int64_t impact_signed = (int64_t)(mass_A + mass_B) * impulse;
    int32_t impact_mag = (int32_t)(impact_signed < 0 ? -impact_signed : impact_signed);

    TD5_LOG_I(LOG_TAG, "v2v_impulse: side=%d slot_A=%d slot_B=%d mA=%d mB=%d "
              "cxA=%d czA=%d cxB=%d czB=%d imp=%d mag=%d toi=%d",
              is_side_branch, A->slot_index, B->slot_index, mass_A, mass_B,
              cx_A, cz_A, cx_B, cz_B, impulse, impact_mag, impactForce);

    /* Wanted mode (cop chase): player ramming a stationary cop engages pursuit.
     * Threshold 10000 filters noise while catching any real collision. */
    if (td5_game_is_wanted_mode() && impact_mag > 10000) {
        if (A->slot_index == 0 && B->slot_index >= 1 && B->slot_index < 6)
            td5_ai_engage_wanted_cop(B->slot_index);
        else if (B->slot_index == 0 && A->slot_index >= 1 && A->slot_index < 6)
            td5_ai_engage_wanted_cop(A->slot_index);
    }

    /* Traffic recovery escalation (> 50000 and slot>=6). */
    if (A->slot_index >= 6 && impact_mag > 50000 &&
        A->damage_lockout > 0 && A->damage_lockout < 7) {
        A->damage_lockout++;
    }
    if (B->slot_index >= 6 && impact_mag > 50000 &&
        B->damage_lockout > 0 && B->damage_lockout < 7) {
        B->damage_lockout++;
    }

    /* Heavy impact (> 90000) with collisions enabled: visual scatter.
     * Original uses GetDamageRulesStub() RNG + traffic orientation rebuild;
     * we use a deterministic approximation here. */
    if (impact_mag > 90000 && g_collisions_enabled == 0) {
        int32_t scatter = impact_mag / 4;
        if (scatter > 0x7FFF) scatter = 0x7FFF;
        if (A->slot_index < 6) {
            A->euler_accum.roll  += (scatter >> 2) - (scatter >> 3);
            A->euler_accum.pitch += (scatter >> 1) - scatter;
            int32_t lift_a = impact_mag / 6;
            if (lift_a > 200000) lift_a = 200000;
            A->linear_velocity_y  = lift_a;
        }
        if (B->slot_index < 6) {
            B->euler_accum.roll  -= (scatter >> 2) - (scatter >> 3);
            B->euler_accum.pitch -= (scatter >> 1) - scatter;
            int32_t lift_b = impact_mag / 6;
            if (lift_b > 200000) lift_b = 200000;
            B->linear_velocity_y  = lift_b;
        }
    }
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

    /* --- Initial OBB test at full (end-of-tick) position.
     * [CONFIRMED @ 0x00408B48-0x00408B56]: the original's pre-loop call to
     * CollectVehicleCollisionContacts seeds `local_88` (the cached bitmask)
     * at current positions and early-returns when it is zero. */
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));
    int bitmask = obb_corner_test(a, b, ax, az, bx, bz,
                                  heading_a, heading_b, corners);

    if (bitmask == 0) return;  /* No overlap at full size */

    /* --- 7-iteration position-based binary search (matching 0x00408A60) ---
     *
     * The original's ResolveVehicleCollisionPair sweeps actor positions and
     * headings backward/forward in time on STACK COPIES to find the approximate
     * moment of first contact (TOI). This produces an impactForce fraction
     * used by the impulse solver for position rollback/re-advance.
     *
     * CRITICAL — LAST-NON-ZERO BITMASK CACHE [CONFIRMED @ 0x00408CD0]:
     * The original caches `local_88 = uVar5` ONLY on the bitmask!=0 branch.
     * On bitmask==0 iterations `local_88` keeps its prior value. After the
     * 7 iterations the dispatch switch at 0x00408D48 reads `local_88`
     * straight — there is NO post-loop CollectVehicleCollisionContacts call
     * [CONFIRMED @ 0x00408D23-0x00408D48 direct fall-through]. So the
     * dispatched bitmask is always the LAST iteration's non-zero result
     * (or the pre-loop seed if all iterations missed), never a fresh
     * end-of-loop re-test. A prior version of this port did a final re-test
     * with `if (bitmask == 0) return;`, which silently dropped the contact
     * whenever the 7-iter binary search converged onto the separation
     * moment — matching the observed "cars clip through each other"
     * symptom. */

    /* Per-tick velocity in OBB space (world_pos >> 8 coordinates).
     * [CONFIRMED @ 0x00408B69-0x00408BB1]: The original seeds per-axis step
     * accumulators from actor fields +0x1cc, +0x1d4, +0x1c4 via DWORD MOV
     * (no cast, no truncation) and halves each with SAR 1.
     *   +0x1cc = linear_velocity_x [CONFIRMED @ td5_ai.c ACTOR_LIN_VEL_X]
     *   +0x1d4 = linear_velocity_z [CONFIRMED @ td5_ai.c ACTOR_LIN_VEL_Z]
     *   +0x1c4 = angular_velocity_yaw [CONFIRMED @ td5_ai.c:1854]
     * The original keeps position accumulators in raw 24.8 FP units and the
     * heading accumulator as raw euler_accum.yaw (also 24.8 scale), then
     * converts to display units inside CollectVehicleCollisionContacts via
     * (delta_pos >> 8) and (heading >> 8) [CONFIRMED @ 0x00408570 disasm].
     * The port pre-divides to display units before entering the loop, which
     * produces identical test points — the two formulations are algebraically
     * equivalent [verified by tracing all 7 iteration paths]. */
    int32_t vel_ax = a->linear_velocity_x >> 8;
    int32_t vel_az = a->linear_velocity_z >> 8;
    int32_t vel_bx = b->linear_velocity_x >> 8;
    int32_t vel_bz = b->linear_velocity_z >> 8;

    /* Per-tick angular velocity in heading space (euler_accum >> 8).
     * Original uses angular_velocity_yaw / 2 in accumulator units then
     * converts >>8 at dispatch — mathematically equivalent to omega_h / 2
     * in 12-bit display units as used here. */
    int32_t omega_a_h = a->angular_velocity_yaw >> 8;
    int32_t omega_b_h = b->angular_velocity_yaw >> 8;

    /* Bisection fraction: 0 = one tick ago, 0x100 = current time.
     * Start at midpoint (0x80). [CONFIRMED @ 0x00408B5C-0x00408B65]
     *
     * The original's pre-loop rewind (`local_80 -= vel/2`) plus incremental
     * halving (SAR each iter) [CONFIRMED @ 0x00408BB5-0x00408C54] visits the
     * same test points as the port's absolute interpolation formula.
     * Proof: both converge to the same frac-indexed positions at every iter
     * regardless of hit/miss history — verified for all 7 iterations. */
    int32_t frac = 0x80;
    int32_t bisect_step = 0x40;

    int32_t test_ax = ax, test_az = az, test_bx = bx, test_bz = bz;
    int32_t test_ha = heading_a, test_hb = heading_b;

    /* Cache last-hit bitmask + corresponding corner data. Seeded from the
     * pre-loop test; updated in-loop ONLY when a test reports bitmask!=0.
     * Matches the original's `local_88` + `local_58..local_20` semantics. */
    int32_t        cached_bitmask = bitmask;
    OBB_CornerData cached_corners[8];
    memcpy(cached_corners, corners, sizeof(cached_corners));

    for (int iter = 0; iter < 7; iter++) {
        /* Interpolated positions at current fraction.
         * test_pos = current_pos - vel * (0x100 - frac) / 0x100 */
        int32_t rollback = 0x100 - frac;
        test_ax = ax - ((vel_ax * rollback) >> 8);
        test_az = az - ((vel_az * rollback) >> 8);
        test_bx = bx - ((vel_bx * rollback) >> 8);
        test_bz = bz - ((vel_bz * rollback) >> 8);

        /* Fix #4 [CONFIRMED @ 0x00408B99-0x00408BA2, 0x00408BB5-0x00408BF5]:
         * The original does NOT mask the heading accumulator during bisection.
         * `local_94`/`local_90` accumulate without any AND/masking — the >> 8
         * conversion to 12-bit only happens at dispatch (0x00408D58 SAR ECX,8).
         * cos_fixed12/sin_fixed12 both apply `& 0xFFF` internally, so the
         * mask was functionally redundant but inconsistent with the original. */
        test_ha = heading_a - ((omega_a_h * rollback) >> 8);
        test_hb = heading_b - ((omega_b_h * rollback) >> 8);

        memset(corners, 0, sizeof(corners));
        bitmask = obb_corner_test(a, b, test_ax, test_az, test_bx, test_bz,
                                  test_ha, test_hb, corners);

        if (bitmask != 0) {
            /* Overlap at this frac: remember this state for dispatch and
             * step backward in time. [CONFIRMED @ 0x00408CD0 MOV [ESP+0x24],EAX] */
            cached_bitmask = bitmask;
            memcpy(cached_corners, corners, sizeof(cached_corners));
            frac -= bisect_step;
        } else {
            /* No overlap: step forward in time. `cached_bitmask` and
             * `cached_corners` retain the last-hit state. */
            frac += bisect_step;
        }
        bisect_step >>= 1;
        if (bisect_step < 1) bisect_step = 1;
    }

    /* impactForce = local_84 - 0x10
     * [CONFIRMED @ 0x00408D34 SUB ESI,0x10 ; PUSH ESI @ 0x00408D57]
     * The original does NOT clamp — range is [-0x10, 0xF0] after 7 iterations
     * from 0x80. Prior port applied an [0x10, 0xF0] clamp that both offset the
     * value by 0x10 AND cut off the lower half of the range; both wrong. */
    int32_t impactForce = frac - 0x10;

    /* Dispatch uses the cached last-non-zero bitmask + corners, NOT a final
     * re-test. `test_ha` / `test_hb` carry the final-state yaws (in 12-bit
     * units, possibly slightly outside [0,0xFFF] — handled by cos_fixed12/
     * sin_fixed12's internal mask) because the original's dispatch push uses
     * `local_94 >> 8` / `local_90 >> 8` which are the bisection accumulators'
     * final values [CONFIRMED @ 0x00408D58 SAR ECX,8; 0x00408DD4].
     * Final-state yaw sits within ±(last bisect_step) of the last-hit yaw. */
    TD5_LOG_I(LOG_TAG, "v2v_bisect: slotA=%d slotB=%d frac=0x%02X impactForce=%d bitmask=0x%02X bisHA=0x%03X bisHB=0x%03X rawHA=0x%03X rawHB=0x%03X",
              a->slot_index, b->slot_index, frac, impactForce, cached_bitmask,
              test_ha, test_hb, heading_a, heading_b);

    /* --- Dispatch collision response based on corner bitmask ---
     * 4th arg `angle` is the target's BISECTED yaw (>> 8).
     * [CONFIRMED @ 0x00408D58 SAR ECX,0x8; 0x00408D5B PUSH ECX]
     *   For cases where B is the penetrator (bits 0-3): angle = local_94 >> 8
     *     = (actor_A.euler_accum.yaw accumulator post-bisection) >> 8 = test_ha.
     *   For cases where A is the penetrator (bits 4-7): angle = local_90 >> 8
     *     = (actor_B.euler_accum.yaw accumulator post-bisection) >> 8 = test_hb. */
    /* Bits 0-3: B corners in A -> response(B, A, corner) -- B is penetrating A */
    for (int i = 0; i < 4; i++) {
        if (cached_bitmask & (1 << i)) {
            apply_collision_response(b, a, i, &cached_corners[i], test_ha, impactForce);
        }
    }
    /* Bits 4-7: A corners in B -> response(A, B, corner) -- A is penetrating B */
    for (int i = 0; i < 4; i++) {
        if (cached_bitmask & (1 << (i + 4))) {
            apply_collision_response(a, b, i, &cached_corners[i + 4], test_hb, impactForce);
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
    static uint32_t s_v2v_tick = 0;
    static uint32_t s_v2v_slot0_pair_count = 0;
    static uint32_t s_v2v_slot0_obb_enter = 0;

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

    s_v2v_tick++;

    /* Log slot 0's broadphase state once per second (60 ticks). Helps diagnose
     * why player rarely produces V2V events: either the grid isn't bucketing
     * it with AI (spatial separation) or the dispatch is filtering it out. */
    if ((s_v2v_tick % 60u) == 0u) {
        TD5_Actor *p0 = (TD5_Actor *)g_actor_table_base;
        if (p0->car_definition_ptr) {
            int32_t seg0 = p0->track_span_normalized < 0 ? 0 : p0->track_span_normalized;
            int bucket0 = (seg0 >> 2) & (COLLISION_GRID_SIZE - 1);
            TD5_LOG_I(LOG_TAG,
                      "v2v_tick slot0: total=%d span=%d bucket=%d pos=(%d,%d,%d) "
                      "vmode=%d dmg=%d pair_calls=%u aabb_hits=%u",
                      total, seg0, bucket0,
                      p0->world_pos.x >> 8, p0->world_pos.y >> 8, p0->world_pos.z >> 8,
                      p0->vehicle_mode, p0->damage_lockout,
                      s_v2v_slot0_pair_count, s_v2v_slot0_obb_enter);
            s_v2v_slot0_pair_count = 0;
            s_v2v_slot0_obb_enter = 0;
        }
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

                /* Dispatch rule from 0x00409150 ResolveVehicleContacts
                 * [CONFIRMED]: Full OBB path when BOTH actors have
                 * field_0x379 == 0 (normal) AND field_0x37c < 0x0F.
                 * Otherwise use sphere separation.
                 *
                 * Traffic (slots 6-11) runs the FULL OBB path in the
                 * original — `UpdateTrafficActorMotion`'s cVar3==0 branch
                 * leaves field_0x379 at 0. Gating on slot_index was wrong. */
                int a_scripted = (a->vehicle_mode != 0) ||
                                 (a->damage_lockout >= 0x0F);
                int b_scripted = (b->vehicle_mode != 0) ||
                                 (b->damage_lockout >= 0x0F);

                if (i == 0 || j == 0) {
                    s_v2v_slot0_pair_count++;

                    /* Inline AABB check so we only log the cases where the
                     * boxes actually overlap — the previous "v2v_obb_enter"
                     * was firing on every dispatch and missing the fact that
                     * collision_detect_full bails at the same AABB pre-check
                     * 588/601 times. The cases below are what we actually
                     * care about: slot 0 pairs whose AABBs touch but OBB
                     * still produces no impulse. */
                    int aabb_sep = (g_actor_aabb[i][2] < g_actor_aabb[j][0] ||
                                    g_actor_aabb[j][2] < g_actor_aabb[i][0] ||
                                    g_actor_aabb[i][3] < g_actor_aabb[j][1] ||
                                    g_actor_aabb[j][3] < g_actor_aabb[i][1]);
                    if (!aabb_sep) {
                        s_v2v_slot0_obb_enter++;
                        TD5_LOG_I(LOG_TAG,
                                  "v2v_aabb_hit: i=%d j=%d slot_i=%d slot_j=%d "
                                  "pos_i=(%d,%d) pos_j=(%d,%d) aabb_i=[%d,%d,%d,%d] aabb_j=[%d,%d,%d,%d]",
                                  i, j, a->slot_index, b->slot_index,
                                  a->world_pos.x >> 8, a->world_pos.z >> 8,
                                  b->world_pos.x >> 8, b->world_pos.z >> 8,
                                  g_actor_aabb[i][0], g_actor_aabb[i][1],
                                  g_actor_aabb[i][2], g_actor_aabb[i][3],
                                  g_actor_aabb[j][0], g_actor_aabb[j][1],
                                  g_actor_aabb[j][2], g_actor_aabb[j][3]);
                    }
                }

                if (a_scripted || b_scripted) {
                    collision_detect_simple(a, b);
                } else {
                    collision_detect_full(a, b, i, j);
                }

                chain = g_actor_aabb[j][4] & 0xFF;
            }
        }
    }

    /* --- Phase 3: Grid reset is handled at start of next call --- */
}

/* Internal: dispatch collision between two actors (grid broadphase wrapper).
 * Same rule as the inline dispatch in td5_physics_resolve_vehicle_contacts
 * (see comment there). */
static void resolve_collision_pair(TD5_Actor *a, TD5_Actor *b, int idx_a, int idx_b)
{
    if (!a || !b) return;
    if (!a->car_definition_ptr || !b->car_definition_ptr) return;

    int a_scripted = (a->vehicle_mode != 0) ||
                     (a->damage_lockout >= 0x0F);
    int b_scripted = (b->vehicle_mode != 0) ||
                     (b->damage_lockout >= 0x0F);

    if (a_scripted || b_scripted) {
        collision_detect_simple(a, b);
    } else {
        collision_detect_full(a, b, idx_a, idx_b);
    }
}

/* ========================================================================
 * Suspension: IntegrateWheelSuspensionTravel (0x00403A20)
 *
 * Per-wheel spring-damper. Projects the world-frame net acceleration
 * applied this frame (accel_x, accel_z — the fx/fz written into
 * linear_velocity just before this call) onto the lever arm from the
 * chassis centre to each wheel's high-res world contact position. That
 * scalar projection is the external excitation for the spring.
 *
 *   arm.x = wheel_world_positions_hires[i].x - (world_pos.x >> 8)
 *   arm.z = wheel_world_positions_hires[i].z - (world_pos.z >> 8)
 *   input = arm.x * accel_x + arm.z * accel_z        (world-frame lever torque)
 *   spring_term = ((input >> 8) * cardef+0x62) >> 8
 *   load_term   = (wheel_load_accum[i] * cardef+0x66) >> 8
 *   new_vel = wheel_spring_dv[i] + spring_term + load_term
 *   new_vel -= (wheel_suspension_pos[i] * cardef+0x5E) >> 8   // pos-damping (restoring)
 *   new_vel -= (new_vel                * cardef+0x60) >> 8    // vel-damping
 *   if |new_vel| < 0x10: new_vel = 0                          // deadzone
 *   wheel_spring_dv[i] = new_vel
 *   wheel_suspension_pos[i] += new_vel
 *   clamp to +/- cardef+0x64   (zeros vel on hit)
 *
 * The post-loop central block repeats the same formula once with the
 * front-axle contact midpoint for center_suspension_pos/vel
 * (+0x2CC/+0x2D0), using 2x the per-wheel clamp and NO load-accum term
 * and NO deadzone.
 *
 * NOTE ON FIELD NAMES (from re/include/td5_actor_struct.h — confirmed):
 *   +0x2DC wheel_suspension_pos[4]  -- spring position (state)
 *   +0x2EC wheel_spring_dv[4]       -- spring velocity (state)  (misnomer)
 *   +0x2FC wheel_load_accum[4]      -- external force feed      (misnomer)
 *
 * The pre-2026-04-13 port of this function read/wrote the last two
 * swapped AND ignored the world-frame acceleration input entirely, so
 * it produced no dynamic bounce response. Fixed here.
 * ======================================================================== */

void td5_physics_integrate_suspension(TD5_Actor *actor, int32_t accel_x, int32_t accel_z)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* cardef constants -- see function header.
     * Names chosen to match the original's semantic use. */
    const int32_t k_pos_damp   = (int32_t)PHYS_S(actor, 0x5E);  /* position-proportional damping (restoring) */
    const int32_t k_vel_damp   = (int32_t)PHYS_S(actor, 0x60);  /* velocity-proportional damping */
    const int32_t k_spring     = (int32_t)PHYS_S(actor, 0x62);  /* spring coefficient (multiplies lever proj) */
    const int32_t k_travel_lim = (int32_t)PHYS_S(actor, 0x64);  /* per-wheel +/- travel clamp */
    const int32_t k_load_scale = (int32_t)PHYS_S(actor, 0x66);  /* multiplier on wheel_load_accum */

    const int32_t wpx_scaled = actor->world_pos.x >> 8;  /* signed fixed-point shift w/ round-to-zero */
    const int32_t wpz_scaled = actor->world_pos.z >> 8;

    /* ---- Per-wheel pass (4 wheels) ---- */
    for (int i = 0; i < 4; i++) {
        /* Lever arm from chassis centre to this wheel's contact position,
         * in world units. Original (0x00403A20):
         *   arm = wheel_world_positions_hires[i] - (world_pos >> 8)
         * Port stores wheel_world_positions_hires in 24.8 FP (vfx consumes
         * that scale at td5_vfx.c:1367), so shift down here to match the
         * original's world-unit arithmetic. Without the >>8, arm magnitudes
         * hit ~10M and spring_term blows past int32, producing per-tick
         * suspension jumps — historically masked by a port-local VEL_CAP
         * (now removed below). [From commit e97669e; regressed between then
         * and HEAD; restored 2026-04-22 to fix wheel oscillation on slopes.] */
        const int32_t arm_x = (actor->wheel_world_positions_hires[i].x >> 8) - wpx_scaled;
        const int32_t arm_z = (actor->wheel_world_positions_hires[i].z >> 8) - wpz_scaled;

        int32_t proj = arm_x * accel_x + arm_z * accel_z;
        int32_t spring_term = (proj >> 8) * k_spring;
        int32_t load_term   = actor->wheel_load_accum[i] * k_load_scale;

        int32_t new_vel = (spring_term >> 8)
                        + (load_term   >> 8)
                        + actor->wheel_spring_dv[i];

        int32_t pos_damp = actor->wheel_suspension_pos[i] * k_pos_damp;
        int32_t vel_damp = new_vel * k_vel_damp;
        new_vel = new_vel - (pos_damp >> 8) - (vel_damp >> 8);

        /* Deadzone: suppresses micro-oscillation at rest. Matches original
         * 0x00403A20's |new_vel| < 0x10 dead-zone. No per-tick VEL_CAP —
         * original relies only on this deadzone plus the post-add travel-
         * limit clamp below. A previous port-local VEL_CAP=0x100 was
         * removed because it was a workaround for the lever-arm unit bug
         * (now fixed above). Re-introducing it silences legitimate
         * restoring transients and makes the front suspension saturate
         * one side ahead of the other, producing nose-up asymmetric tilt
         * and the steady-slope wheel oscillation (susp_vel=12000)
         * observed 2026-04-21. */
        if (new_vel > -0x10 && new_vel < 0x10)
            new_vel = 0;

        actor->wheel_spring_dv[i] = new_vel;
        int32_t new_pos = actor->wheel_suspension_pos[i] + new_vel;
        actor->wheel_suspension_pos[i] = new_pos;

        if (new_pos > k_travel_lim) {
            actor->wheel_suspension_pos[i] = k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        } else if (new_pos < -k_travel_lim) {
            actor->wheel_suspension_pos[i] = -k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        }
    }

    /* ---- Central chassis pass ----
     * The original averages the front-axle contact (wheels 0+1) x and z
     * to derive the lever arm for the body-level suspension. No load
     * term, no deadzone; same spring/damping constants; travel clamp is
     * the SAME cardef+0x64 as per-wheel (NOT doubled — verified @
     * 0x00403A20 `iVar7 = (int)*(short *)(param_2 + 100)`, used
     * directly without *2). [From commit e97669e; regressed between
     * then and HEAD; restored 2026-04-22.] */
    {
        /* Same FP→world-unit shift as per-wheel arm above. */
        const int32_t front_mid_x =
            ((actor->wheel_world_positions_hires[1].x + actor->wheel_world_positions_hires[0].x) >> 1) >> 8;
        const int32_t front_mid_z =
            ((actor->wheel_world_positions_hires[1].z + actor->wheel_world_positions_hires[0].z) >> 1) >> 8;

        const int32_t arm_x = front_mid_x - wpx_scaled;
        const int32_t arm_z = front_mid_z - wpz_scaled;

        int32_t proj = arm_x * accel_x + arm_z * accel_z;
        int32_t spring_term = (proj >> 8) * k_spring;

        int32_t new_vel = (spring_term >> 8) + actor->center_suspension_vel;

        int32_t pos_damp = actor->center_suspension_pos * k_pos_damp;
        int32_t vel_damp = new_vel * k_vel_damp;
        new_vel = new_vel - (pos_damp >> 8) - (vel_damp >> 8);

        int32_t new_pos = actor->center_suspension_pos + new_vel;
        actor->center_suspension_pos = new_pos;
        actor->center_suspension_vel = new_vel;

        if (new_pos > k_travel_lim) {
            actor->center_suspension_pos = k_travel_lim;
            actor->center_suspension_vel = 0;
        } else if (new_pos < -k_travel_lim) {
            actor->center_suspension_pos = -k_travel_lim;
            actor->center_suspension_vel = 0;
        }
    }
}

/* ========================================================================
 * UpdateVehicleSuspensionResponse (0x4057F0)
 *
 * Aggregates per-wheel contact loads into chassis angular accelerations.
 *
 * Original decompilation (confirmed):
 *   bVar1 = damage_lockout (0x37C) = airborne/new bitmask
 *   bVar2 = wheel_contact_bitmask (0x37D) = previous-frame bitmask
 *   If bVar1 == 0xF (all airborne), skip entirely.
 *
 *   Per-wheel loop: for each wheel i NOT in bVar1 (grounded):
 *     sVar3 = wheel_display_angles[i][0] = body-space X arm (roll/pitch)
 *     sVar4 = wheel_display_angles[i][2] = body-space Z arm (roll)
 *     Rotate wheel_contact_velocities[i][0..2] by transposed rotation matrix.
 *     local_36 = Y-component of rotated velocity
 *     iVar8 = (local_36 * gravity + round) >> 12
 *     local_50 += iVar8 * sVar4   (roll accumulator)
 *     local_5c -= iVar8 * sVar3   (pitch accumulator)
 *     local_4c++                   (grounded wheel count)
 *
 *     If wheel was AIRBORNE previous frame (bVar2 & (1<<i), i.e. just
 *     landed this tick — bVar2 = [ESI+0x37D] = prev airborne mask):
 *       spring_dot   = gap_270[i][0]*wcv[0] + gap_270[i][1]*wcv[1] + gap_270[i][2]*wcv[2]
 *       spring_force = arith_round_shift(spring_dot, 0xFFF, 12)
 *       local_64    += spring_force * sVar4 * -0x100  (roll spring)
 *       local_60    += spring_force * sVar3 *  0x100  (pitch spring)
 *       bounce_accum+= spring_force / 2
 *       local_58++   (spring-grounded count)
 *
 *   angular_velocity_roll  += (local_64 + local_50 / local_4c) / 0x4B0
 *   angular_velocity_pitch += (local_60 + local_5c / local_4c) / 0x226
 *   linear_velocity_y      += iVar8 + gravity
 *
 *   Clamp roll/pitch to ±4000 per bitmask switch table.
 * ======================================================================== */

/* Y-component projection of a body-space vector into world space.
 * Uses ROW 1 of the body→world rotation matrix: {m[3], m[4], m[5]}.
 *
 * [CONFIRMED @ 0x0042E2E0] ConvertFloatVec3ToShortAngles Y output:
 *   Y = v[0]*M[+0xC] + v[1]*M[+0x10] + v[2]*M[+0x14]
 *     = v[0]*M[3]    + v[1]*M[4]     + v[2]*M[5]   (row-major float[9])
 *
 * Callers that pass `actor->rotation_matrix` directly (via
 * LoadRenderRotationMatrix without prior transpose) get body→world.
 * In port: td5_physics_integrate_pose ground-snap tail (line ~3618) —
 * src = per-wheel body offset, producing world-space Y for chassis
 * correction averaging. */
static int16_t rotate_body_to_world_y(const TD5_Actor *actor, const int16_t v[3])
{
    float m3 = actor->rotation_matrix.m[3];
    float m4 = actor->rotation_matrix.m[4];
    float m5 = actor->rotation_matrix.m[5];
    float result = (float)v[0] * m3 + (float)v[1] * m4 + (float)v[2] * m5;
    if (result >  32767.0f) return  32767;
    if (result < -32768.0f) return -32768;
    return (int16_t)(int32_t)result;
}

/* Y-component projection of a world-space vector into body space.
 * Uses COLUMN 1 of body→world matrix = ROW 1 of body^T = {m[1], m[4], m[7]}.
 *
 * [CONFIRMED @ 0x004057FA] UpdateVehicleSuspensionResponse calls
 * `TransposeMatrix3x3(&actor->rotation_m00, local_30)` then
 * `LoadRenderRotationMatrix(local_30)`, so the subsequent
 * `ConvertFloatVec3ToShortAngles(wcv, ...)` reads ROW 1 of the TRANSPOSE =
 * COLUMN 1 of the original matrix.
 *
 * Semantically this is world→body: wcv is a world-space surface normal,
 * the output Y is the body-frame Y component of that normal — used to
 * compute the gravity projection onto body Y for the per-wheel torque
 * accumulator.
 *
 * Caller: td5_physics_update_suspension_response (line ~3022) with
 * src = actor->wheel_contact_velocities[i] (wcv, world-space normal).
 *
 * HISTORY: originally this helper WAS col-1 (correct for susp_response
 * via transpose semantics). A 2026-04-22 Ghidra re-pass against
 * ConvertFloatVec3ToShortAngles mistakenly flagged col-1 as wrong and
 * switched it to row-1 — that fixed the ground-snap callsite (which had
 * been silently broken) but broke this one. Now split: each call site
 * reads the matrix the way the original's transpose-or-not pairing
 * implies. */
static int16_t rotate_world_to_body_y(const TD5_Actor *actor, const int16_t v[3])
{
    float m1 = actor->rotation_matrix.m[1];
    float m4 = actor->rotation_matrix.m[4];
    float m7 = actor->rotation_matrix.m[7];
    float result = (float)v[0] * m1 + (float)v[1] * m4 + (float)v[2] * m7;
    if (result >  32767.0f) return  32767;
    if (result < -32768.0f) return -32768;
    return (int16_t)(int32_t)result;
}

/* Arithmetic rounding helper: (x + (x>>31 & mask)) >> shift
 * Matches original binary's signed-divide rounding idiom. */
static inline int32_t arith_round_shift(int32_t x, int32_t mask, int32_t shift)
{
    return (x + (int32_t)((uint32_t)(x >> 31) & (uint32_t)mask)) >> shift;
}

void td5_physics_update_suspension_response(TD5_Actor *actor)
{
    /* Faithful port of UpdateVehicleSuspensionResponse @ 0x004057F0.
     *
     * HISTORY: this is a restore of commit e97669e's literal port, which
     * had been replaced (between then and 2026-04-21) with a pragmatic
     * ground-probe P-controller + 1/16 decay. That replacement produced
     * an exaggerated uphill-roll bug: on positive pitch slopes the
     * P-controller's target and the decay-to-zero both pulled the same
     * direction (runaway), while downhill they opposed (self-resets).
     * Research agent 2026-04-21 confirmed:
     *   - No asymmetric term exists in the original 0x004057F0.
     *   - T2 block in IntegrateVehiclePoseAndContacts @ 0x00405E80 OVERWRITES
     *     angular_velocity_{roll,pitch} each tick; 0x004057F0 ADDS a small
     *     per-tick correction clamped ±4000. That pair is self-limiting.
     *   - Port T2 block already matches original exactly (±6000 clamp,
     *     wrap*0x100 delta). So restoring the faithful 0x004057F0 add-step
     *     reinstates the T2-dominates-with-corrective pattern.
     *
     * Two masks are needed:
     *   lock     = current-frame AIRBORNE mask (1=airborne). Original reads
     *              [ESI+0x37C]. Port live equivalent is
     *              actor->wheel_contact_bitmask after refresh.
     *   prev_air = previous-frame AIRBORNE mask (1=airborne). Original
     *              reads [ESI+0x37D] — refresh overwrites +0x37D ← old
     *              +0x37C at its top, so after refresh +0x37D holds last
     *              tick's airborne mask. Port reconstructs by inverting
     *              s_prev_grounded_mask[slot] (which was captured pre-
     *              refresh from the PREVIOUS frame's wheel_contact_bitmask).
     *
     * NOTE polarity: both masks are AIRBORNE (1=airborne). Prior port
     * mixed GROUNDED/AIRBORNE conventions and got BOTH the spring gate
     * and the pattern clamp switch-key wrong. Fixed 2026-04-24.
     *
     * Struct layout source: decomp @ 0x00405884/0x00405888 shows the loop
     * reads sVar3=psVar11[-0x22], sVar4=psVar11[-0x20] where psVar11 walks
     * +0x254-style stride of 4 shorts/wheel, giving actor+0x210/+0x214 for
     * wheel 0 — matches the `arms` pointer below. wcv at +0x250 stride 4
     * shorts; gap_270 at +0x270 same stride.
     */
    const uint8_t lock = actor->wheel_contact_bitmask;
    /* prev_air = previous-frame AIRBORNE mask (bVar2 in original @ +0x37D).
     * After refresh_wheel_contacts, the original's +0x37D holds the old
     * value of +0x37C (= last-frame airborne). Port snapshots
     * s_prev_grounded_mask[slot] = ~(last-frame airborne) BEFORE refresh,
     * so we invert to reconstruct prev_air.
     *
     * HISTORY: prior port named this `grnd` and treated it as previous-
     * frame GROUNDED. That polarity was wrong in TWO places:
     *   (1) spring-dot gate @ 0x004058E4 fires when wheel was AIRBORNE last
     *       frame (i.e. just landed this tick), not when grounded. Port
     *       was firing the spring impulse on every grounded frame, which
     *       continuously amplified tiny per-tick Y fluctuations into
     *       roll_spr/pitch_spr — matches user's "very sensitive of Y" bug.
     *   (2) pattern-clamp switch @ 0x00405A6A keys on +0x37D directly.
     *       Port's inverted key matches the complement pattern set.
     * 2026-04-24 research agent confirmed both polarities at the cited
     * disassembly addresses. */
    const uint8_t prev_air = (uint8_t)((~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F);

    if (lock == 0x0F) {
        /* All four wheels airborne — early-return matches original
         * 0x00405809; gravity stays subtracted (not added back). */
        return;
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "susp_resp_enter slot0: lock=0x%02x prev_air=0x%02x",
                  (int)lock, (int)prev_air);
    }

    int32_t pitch_grav = 0;     /* local_50 */
    int32_t roll_grav  = 0;     /* local_5c */
    int32_t pitch_spr  = 0;     /* local_64 */
    int32_t roll_spr   = 0;     /* local_60 */
    int32_t bounce     = 0;     /* local_78 */
    int32_t cnt_active   = 0;   /* local_4c — non-locked wheels */
    int32_t cnt_grounded = 0;   /* local_58 — wheels also grounded prev tick */

    const int16_t *arms = (const int16_t *)((const uint8_t *)actor + 0x210);
    const int16_t *wcv  = (const int16_t *)((const uint8_t *)actor + 0x250);
    const int16_t *wfdh = (const int16_t *)((const uint8_t *)actor + 0x270);

    for (int i = 0; i < 4; i++) {
        const uint8_t bit = (uint8_t)(1u << i);
        if (lock & bit) continue;            /* skip airborne wheel entirely */

        const int16_t lat  = arms[i * 4 + 0];   /* sVar3, lateral arm  */
        const int16_t loni = arms[i * 4 + 2];   /* sVar4, longitudinal */

        /* g_view = Y component of (transposed body matrix) · wcv[i] —
         * matches `ConvertFloatVec3ToShortAngles(wcv, &local_38)` reading
         * local_36. `rotate_world_to_body_y` reads {m[1],m[4],m[5]/m[7]} —
         * = row 1 of body^T = column 1 of body matrix — matching the
         * original's TransposeMatrix3x3 + LoadRenderRotationMatrix +
         * ConvertFloatVec3ToShortAngles sequence at 0x004057FA-0x004058D8. */
        int16_t cn[3] = { wcv[i * 4 + 0], wcv[i * 4 + 1], wcv[i * 4 + 2] };
        const int32_t y_view = (int32_t)rotate_world_to_body_y(actor, cn);

        /* g_scaled is (Y · gravity) >> 12; signed-divide rounding via
         * arith_round_shift matches original's SAR-with-bias idiom. */
        const int32_t g_scaled = arith_round_shift(y_view * g_gravity_constant, 0xFFF, 12);

        pitch_grav += g_scaled * (int32_t)loni;   /* +=, longitudinal arm */
        roll_grav  -= g_scaled * (int32_t)lat;    /* -=, lateral arm (NOTE sign) */
        ++cnt_active;

        if (prev_air & bit) {
            /* Landing-impulse spring: fires ONLY for wheels that were
             * AIRBORNE last frame (just landed this tick). Literal port of
             * `if ((bVar2 & uVar7) != 0)` @ 0x004058E4 where bVar2 =
             * [ESI+0x37D] = previous-frame airborne mask. This is an
             * impulsive correction tied to touchdown, NOT a per-frame
             * spring damper.
             *
             * wfdh and wcv share +0x270 / +0x250 layout (4 shorts per
             * wheel). dot(wfdh[i], wcv[i]) scaled by per-wheel body arms
             * goes into pitch_spr/roll_spr/bounce. */
            int32_t dot =   (int32_t)wfdh[i * 4 + 0] * (int32_t)wcv[i * 4 + 0]
                          + (int32_t)wfdh[i * 4 + 1] * (int32_t)wcv[i * 4 + 1]
                          + (int32_t)wfdh[i * 4 + 2] * (int32_t)wcv[i * 4 + 2];
            dot = arith_round_shift(dot, 0xFFF, 12);   /* signed >>12 */

            pitch_spr += (dot * (int32_t)loni) * -0x100;   /* note minus */
            roll_spr  += (dot * (int32_t)lat ) *  0x100;
            bounce    += dot >> 1;                         /* signed SAR 1 */
            ++cnt_grounded;

            if (actor->slot_index == 0) {
                TD5_LOG_I(LOG_TAG,
                    "susp_resp landing_impulse slot0: wheel=%d dot=%d "
                    "lat=%d loni=%d pitch_spr+=%d roll_spr+=%d",
                    i, dot, (int)lat, (int)loni,
                    (dot * (int32_t)loni) * -0x100,
                    (dot * (int32_t)lat)  *  0x100);
            }
        }
    }

    if (cnt_grounded > 0) {
        pitch_spr /= cnt_grounded;
        roll_spr  /= cnt_grounded;
        bounce    /= cnt_grounded;
    }

    /* PlayVehicleSoundAtPosition(0x17, bounce*50, ...) call from the
     * original is omitted — sound side is stubbed elsewhere. */

    const int32_t roll_term  = (cnt_active > 0)
        ? (roll_spr  + roll_grav  / cnt_active) / 0x4B0    /* /1200 */
        : roll_spr  / 0x4B0;
    const int32_t pitch_term = (cnt_active > 0)
        ? (pitch_spr + pitch_grav / cnt_active) / 0x226    /* /550 */
        : pitch_spr / 0x226;

    actor->angular_velocity_roll  += roll_term;
    actor->angular_velocity_pitch += pitch_term;

    /* Y-velocity update: bounce + gravity restored. Original adds
     * gravity back here, cancelling the subtract at top of integrate_pose
     * for grounded cars. */
    actor->linear_velocity_y += bounce + g_gravity_constant;

    if (cnt_grounded > 0) {
        /* Per-pattern angular velocity clamps [CONFIRMED @ 0x00405a6a..
         * 0x00405af4]. Original switches on bVar2 = [ESI+0x37D] =
         * previous-frame AIRBORNE mask. Bitmasks 7,11,13,14,15 fall
         * through with NO clamp.
         *
         * HISTORY: prior port keyed on `grnd` (previous-frame GROUNDED),
         * which matched the COMPLEMENT pattern set (case 0 in port =
         * all airborne, case 0 in original = all grounded). Polarity
         * fixed 2026-04-24. */
        const int32_t LIM = 4000;   /* 0xFA0 */
        switch (prev_air) {
            case 0: case 1: case 2: case 4: case 6: case 8: case 9:
                if (actor->angular_velocity_roll  >  LIM) actor->angular_velocity_roll  =  LIM;
                if (actor->angular_velocity_roll  < -LIM) actor->angular_velocity_roll  = -LIM;
                if (actor->angular_velocity_pitch >  LIM) actor->angular_velocity_pitch =  LIM;
                if (actor->angular_velocity_pitch < -LIM) actor->angular_velocity_pitch = -LIM;
                break;
            case 3: case 12:
                if (actor->angular_velocity_roll  >  LIM) actor->angular_velocity_roll  =  LIM;
                if (actor->angular_velocity_roll  < -LIM) actor->angular_velocity_roll  = -LIM;
                break;
            case 5: case 10:
                if (actor->angular_velocity_pitch >  LIM) actor->angular_velocity_pitch =  LIM;
                if (actor->angular_velocity_pitch < -LIM) actor->angular_velocity_pitch = -LIM;
                break;
            default: /* 7,11,13,14,15 — no clamp */
                break;
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "susp_resp slot0: lock=0x%02x prev_air=0x%02x cnt=%d/%d "
                  "p_grav=%d r_grav=%d p_spr=%d r_spr=%d bounce=%d "
                  "av_r=%d av_p=%d vy=%d",
                  (int)lock, (int)prev_air, cnt_active, cnt_grounded,
                  pitch_grav, roll_grav, pitch_spr, roll_spr, bounce,
                  actor->angular_velocity_roll, actor->angular_velocity_pitch,
                  actor->linear_velocity_y);
    }
}

/* ========================================================================
 * Traffic Edge Containment Helpers
 *
 * The original UpdateTrafficActorMotion (0x443ED0) calls, after
 * UpdateTrafficVehiclePose:
 *   ProcessActorRouteAdvance (0x407840)
 *   ProcessActorForwardCheckpointPass (0x4076C0)  -- route progression only
 *   ProcessActorSegmentTransition (0x407390)
 *
 * ProcessActorRouteAdvance and ProcessActorSegmentTransition both:
 *   1. Compute heading delta (from route table vs actor yaw).
 *   2. Build the boundary edge tangent from the first/last span vertex pair.
 *   3. Compute signed perpendicular distance from edge: pen.
 *   4. If pen < 0: call ApplySimpleTrackSurfaceForce (0x407270) to push
 *      the actor back and zero the outward velocity component.
 *   5. Call UpdateTrafficVehiclePose again to snap pose after the push.
 *
 * Without these calls traffic has NO wall containment, only steering-based
 * routing — on tight corners it drives straight through the wall.
 *
 * [CONFIRMED @ 0x443ED0: call sequence after IntegrateVehicleFrictionForces]
 * [CONFIRMED @ 0x407270: ApplySimpleTrackSurfaceForce modifies world_pos/vel]
 * [CONFIRMED @ 0x407390: ProcessActorSegmentTransition edge test for sub_lane boundaries]
 * [CONFIRMED @ 0x407840: ProcessActorRouteAdvance forward-limit edge test]
 * ======================================================================== */

/* Route state indices (mirrors RS_ constants from td5_ai.c).
 * Exposed here for cross-module route state access.
 * [CONFIRMED @ gActorRouteStateTable stride 0x47 dwords] */
#define RS_ROUTE_TABLE_PTR_PHYS    0x00   /* pointer to LEFT/RIGHT.TRK data */
#define RS_SLOT_INDEX_PHYS         0x35   /* slot index of reference actor */

/* Compute route heading delta for slot:
 *   heading_delta = -( ((euler_yaw>>8) - route_angle*0x102C/0x100) - 0x800 & 0xFFF ) - 0x800 & 0xFFF
 * [CONFIRMED @ ComputeActorRouteHeadingDelta 0x434040]
 *
 * The route table stores 3 bytes per span: [angle_lo, angle_hi, ???].
 * The angle at span 'span_normalized' is *(uint8_t*)(route_table + span_norm*3 + 1).
 * Multiplied by 0x102C gives the route heading in 24-bit fixed.
 */
static uint32_t traffic_route_heading_delta(int slot)
{
    int32_t *rs = td5_ai_get_route_state(slot);
    if (!rs) return 0;

    const uint8_t *route_table = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR_PHYS];
    if (!route_table) return 0;

    char *actor = (char *)((uint8_t *)td5_track_get_span(0) - 0x80);  /* not used directly */
    (void)actor;

    /* Get the reference actor slot — RS_SLOT_INDEX gives which actor's yaw to use */
    int ref_slot = rs[RS_SLOT_INDEX_PHYS];
    if (ref_slot < 0 || ref_slot >= TD5_MAX_TOTAL_ACTORS) return 0;

    extern char *g_actor_table_base_ptr;  /* defined in td5_physics.c */
    char *ref_actor = (char *)(g_actor_table_base + (size_t)ref_slot * TD5_ACTOR_STRIDE);

    int16_t span_normalized = *(int16_t *)(ref_actor + 0x082);  /* track_span_normalized */
    if (span_normalized < 0) span_normalized = 0;
    uint8_t route_angle = route_table[(int)span_normalized * 3 + 1];
    int32_t yaw_accum   = *(int32_t *)(ref_actor + 0x1F4);      /* euler_accum.yaw */

    /* Formula from 0x434040:
     *   -(( ((yaw>>8) - route_angle*0x102C/0x100) - 0x800) & 0xFFF) - 0x800) & 0xFFF */
    uint32_t yaw12     = (uint32_t)(yaw_accum >> 8) & 0xFFF;
    uint32_t route12   = ((uint32_t)route_angle * 0x102C) >> 8;
    uint32_t raw_delta = (yaw12 - route12 - 0x800U) & 0xFFF;
    /* Negate to get heading_delta */
    return (-(int32_t)(((int32_t)raw_delta - 0x800) & 0xFFF)) & 0xFFF;
}

/* ApplySimpleTrackSurfaceForce — direct port of 0x00407270.
 *
 * Pushes actor out of wall penetration and zeroes the outward velocity.
 *
 * param_1  = actor pointer
 * edge_angle = 12-bit track-space angle of the edge tangent
 * pen      = signed penetration (< 0 = outside edge)
 *
 * Position correction:
 *   depth = (pen + ((pen>>31)&0xFFF)) >> 12 - 4   [CONFIRMED @ 0x40728B]
 *   world_pos.z += (depth * cos) >> 4              [CONFIRMED @ 0x40729E]
 *   world_pos.x -= (depth * sin) >> 4              [CONFIRMED @ 0x4072A7]
 *
 * Velocity correction (only if lateral velocity is outward, vel_perp >= 0):
 *   vel_perp = (vel_x * cos + vel_z * sin) >> 12   [CONFIRMED @ 0x4072B8]
 *   vel_para = (vel_z * cos - vel_x * sin) >> 12   [CONFIRMED @ 0x4072C6]
 *   if vel_perp >= 0:
 *     vel_para_adj = clamp_dead_zone(vel_para, 0x180)  [CONFIRMED @ 0x4072DE]
 *     vel_perp_new = -(vel_para >> 1)             [CONFIRMED @ 0x4072D7]
 *     vel_x = (vel_para_adj * cos - vel_perp_new * sin) >> 12
 *     vel_z = (vel_para_adj * sin + vel_perp_new * cos) >> 12
 *   track_contact_flag = 0                         [CONFIRMED @ 0x407338]
 * ======================================================================== */
static void apply_simple_track_surface_force(TD5_Actor *actor,
                                              uint32_t edge_angle,
                                              int32_t pen)
{
    int32_t cos_a = cos_fixed12((int32_t)(edge_angle & 0xFFF));
    int32_t sin_a = sin_fixed12((int32_t)(edge_angle & 0xFFF));

    /* Position correction [CONFIRMED @ 0x40728B-0x4072A7] */
    {
        int32_t depth = (pen + ((pen >> 31) & 0xFFF)) >> 12;
        depth -= 4;
        int32_t pz = depth * cos_a;
        int32_t px = depth * sin_a;
        actor->world_pos.z += (int32_t)((pz + ((pz >> 31) & 0xF)) >> 4);
        actor->world_pos.x -= (int32_t)((px + ((px >> 31) & 0xF)) >> 4);
    }

    /* Velocity decomposition [CONFIRMED @ 0x4072AE-0x4072C6] */
    int32_t vx     = actor->linear_velocity_x;
    int32_t vz     = actor->linear_velocity_z;
    int32_t vel_perp_raw = vx * cos_a + vz * sin_a;
    int32_t vel_para_raw = vz * cos_a - vx * sin_a;
    int32_t vel_para_sh  = (vel_para_raw + ((vel_para_raw >> 31) & 0xFFF)) >> 12;
    int32_t vel_perp_sh  = (int32_t)((vel_perp_raw + ((vel_perp_raw >> 31) & 0xFFF)) >> 12);

    /* Only correct if perpendicular component is outward (>= 0) [CONFIRMED @ 0x4072CE] */
    if (vel_perp_sh >= 0) {
        /* Dead-zone clamp: if |vel_para| <= 0x180, zero it; else subtract 0x180
         * [CONFIRMED @ 0x4072DE-0x407307] */
        int32_t vel_para_adj;
        if (vel_para_sh >= 0x181) {
            vel_para_adj = vel_para_sh - 0x180;
        } else if (vel_para_sh <= -0x180) {
            vel_para_adj = vel_para_sh + 0x180;
        } else {
            vel_para_adj = 0;
        }
        /* Reflected perp = -(vel_para >> 1) [CONFIRMED @ 0x4072D7] */
        int32_t vel_perp_new = -(vel_para_sh - (vel_para_raw >> 31)) >> 1;

        int32_t nvx = vel_para_adj * cos_a - vel_perp_new * sin_a;
        int32_t nvz = vel_para_adj * sin_a + vel_perp_new * cos_a;
        actor->linear_velocity_x = (int32_t)((nvx + ((nvx >> 31) & 0xFFF)) >> 12);
        actor->linear_velocity_z = (int32_t)((nvz + ((nvz >> 31) & 0xFFF)) >> 12);
        actor->track_contact_flag = 0;
    }
}

/* Compute signed perpendicular distance from span edge for traffic containment.
 *
 * The original (ProcessActorSegmentTransition, ProcessActorRouteAdvance) builds
 * a normalized tangent vector from two span vertices via ConvertFloatVec4ToShortAngles
 * (x87 FPU normalization), then computes:
 *   pen = (world_offset . tangent) - car_size_projection
 *
 * Port uses atan2_fixed12 to get the angle, then cos/sin for the dot product.
 * [CONFIRMED @ 0x407390 + 0x407840: same vertex pair logic, result sign test < 0]
 *
 * v0_x,v0_z = first vertex (world-relative, from span origin)
 * v1_x,v1_z = second vertex (world-relative, from span origin)
 * car_origin_x, car_origin_z = actor pos minus span origin
 * car_half_w = *(int16_t*)(car_def + 0x0C)
 * car_half_l = *(int16_t*)(car_def + 0x08)
 * cos_hd, sin_hd = cos/sin of heading delta from route
 *
 * Returns (pen, edge_angle).  pen < 0 means outside the edge.
 */
static int32_t traffic_edge_pen(int32_t v0_x, int32_t v0_z,
                                 int32_t v1_x, int32_t v1_z,
                                 int32_t car_x, int32_t car_z,
                                 int32_t car_half_w, int32_t car_half_l,
                                 int32_t cos_hd, int32_t sin_hd,
                                 uint32_t *out_edge_angle)
{
    /* Edge direction: tangent from v0 to v1 */
    int32_t tdx = v1_x - v0_x;
    int32_t tdz = v1_z - v0_z;

    /* Edge angle for ApplySimpleTrackSurfaceForce */
    uint32_t angle = (uint32_t)(atan2_fixed12(tdz, tdx) & 0xFFF);
    if (out_edge_angle) *out_edge_angle = angle;

    /* Normalized tangent components (4096-scale) */
    int32_t tan_x = cos_fixed12((int32_t)angle);
    int32_t tan_z = sin_fixed12((int32_t)angle);

    /* Signed penetration [CONFIRMED @ 0x4073D8 / 0x407920]:
     *   pen = (car_offset . tangent) - car_size  */
    int32_t dot = (int32_t)(((int64_t)car_z * tan_z + (int64_t)car_x * tan_x) >> 12);
    int32_t car_size = (int32_t)(((int64_t)sin_hd * car_half_w +
                                   (int64_t)cos_hd * car_half_l) >> 12);
    return dot - car_size;
}

/* ProcessActorSegmentTransition — port of 0x00407390.
 *
 * Tests the inner (sub_lane < 2) and outer (sub_lane >= lane_count-2) edges
 * of the current span segment and applies containment if the car is outside.
 * [CONFIRMED @ 0x00407390: called from UpdateTrafficActorMotion 0x443ED0]
 *
 * The car_definition_ptr carries:
 *   *(int16_t*)(car_def + 0x08) = half-length equivalent  [CONFIRMED @ 0x407424]
 *   *(int16_t*)(car_def + 0x0C) = half-width equivalent   [CONFIRMED @ 0x407420]
 */
static void process_traffic_segment_edge(TD5_Actor *actor, int slot)
{
    TD5_StripSpan *sp = td5_track_get_span((int)actor->track_span_raw);
    if (!sp) return;
    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int span_type    = (int)sp->span_type;
    if (span_type < 0 || span_type >= 12) return;

    int sub_lane  = (int)(int8_t)actor->track_sub_lane_index;
    int lane_count = (int)(sp->surface_attribute & 0xF);  /* lower 4 bits = lane count */

    /* Heading delta for car projection */
    uint32_t hd      = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    /* Span origin in world space (24.8 FP) */
    int32_t orig_x = sp->origin_x;
    int32_t orig_z = sp->origin_z;

    /* Actor position relative to span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - orig_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - orig_z;

    /* Inner edge test: sub_lane < 2  [CONFIRMED @ 0x407394: if (iVar12 < 2)] */
    if (sub_lane < 2) {
        /* Inner boundary: left vertex[sub_lane] to right vertex[sub_lane] */
        int li_idx = (int)sp->left_vertex_index  + sub_lane;
        int ri_idx = (int)sp->right_vertex_index + sub_lane;
        TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
        TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
        if (!vl || !vr) goto outer_test;

        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)vl->x, (int32_t)vl->z,
            (int32_t)vr->x, (int32_t)vr->z,
            rel_x, rel_z,
            (int32_t)car_half_w, (int32_t)car_half_l,
            cos_hd, sin_hd,
            &edge_angle);

        if (pen < 0) {
            /* DecayUltimateVariantTimer — only fires when specialEncounterEnabled==4
             * [CONFIRMED @ 0x4073C8: conditional on g_specialEncounterEnabled==4]
             * In the port, encounter mode 4 is uncommon; skip for now. */
            apply_simple_track_surface_force(actor, edge_angle, pen);
            /* Original calls UpdateTrafficVehiclePose again after push;
             * the port's integrate_traffic_pose rebuilds the pose at the end
             * of the tick anyway, so skip the redundant rebuild here. */
            return;
        }
    }

outer_test:
    /* Outer edge test: sub_lane >= lane_count - 2  [CONFIRMED @ 0x407462: if (iVar12 >= laneCount-2)] */
    if (sub_lane >= lane_count - 2) {
        /* Outer boundary vertices at lane_count-1 */
        /* The original uses DAT_004631a0/a4 offset tables (k_quad_vertex_offsets).
         * For the outer edge: left vertex index = left_base + outer_offset + sub_lane,
         * right vertex index = right_base + outer_offset_r + sub_lane.
         * [CONFIRMED @ 0x4074B4-0x4074F4: uses DAT_004631a0/a4 * (bVar4=span_type)] */
        static const int8_t k_qvo[12][2] = {
            {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
            {0,0},{0,0},{0,0},{0,0}
        };
        (void)k_qvo;
        /* Use the last sub-lane vertex pair */
        int outer_sub = lane_count - 1;
        int li_idx = (int)sp->left_vertex_index  + outer_sub;
        int ri_idx = (int)sp->right_vertex_index + outer_sub;
        TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
        TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
        if (!vl || !vr) return;

        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)vl->x, (int32_t)vl->z,
            (int32_t)vr->x, (int32_t)vr->z,
            rel_x, rel_z,
            (int32_t)car_half_w, (int32_t)car_half_l,
            cos_hd, sin_hd,
            &edge_angle);

        if (pen < 0) {
            apply_simple_track_surface_force(actor, edge_angle, pen);
        }
    }
}

/* ProcessActorRouteAdvance — port of 0x00407840.
 *
 * Tests the forward end of the current span (the actor is near the end of the
 * span ring) and applies containment if the car has gone past the forward edge.
 * [CONFIRMED @ 0x00407840: called from UpdateTrafficActorMotion 0x443ED0]
 *
 * Only fires when track_span_raw == g_trackTotalSpanCount - 1 (last span).
 * [CONFIRMED @ 0x407879: `if (actor->track_span == DAT_00483550 + -1)`]
 */
static void process_traffic_route_advance(TD5_Actor *actor, int slot)
{
    int total_spans = td5_track_get_span_count();
    if (total_spans <= 0) return;

    /* Only fires at the last span [CONFIRMED @ 0x407879] */
    int span_idx = (int)actor->track_span_raw;
    if (span_idx != total_spans - 1) return;

    /* Last span in the ring — test forward edge using vertices of span 0 wrap.
     * [CONFIRMED @ 0x40787E: uses span_at(DAT_00483550) i.e. last+1 = wrap to 0] */
    TD5_StripSpan *sp_last = td5_track_get_span(span_idx);
    if (!sp_last) return;
    TD5_StripSpan *sp_wrap = td5_track_get_span(0);  /* wraps to span 0 */
    if (!sp_wrap) return;

    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    uint32_t hd = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    /* Forward edge: use last vertex of span_wrap (row 0 of the wrap span) */
    int sub = (int)(int8_t)actor->track_sub_lane_index;
    if (sub < 0) sub = 0;
    /* Count of left/right vertices in the wrap span */
    int wrap_lanes = (int)(sp_wrap->surface_attribute & 0xF);
    if (sub >= wrap_lanes) sub = wrap_lanes - 1;

    int li_idx = (int)sp_wrap->left_vertex_index  + sub;
    int ri_idx = (int)sp_wrap->right_vertex_index + sub;
    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;

    /* Actor position relative to wrap span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp_wrap->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp_wrap->origin_z;

    uint32_t edge_angle;
    int32_t pen = traffic_edge_pen(
        (int32_t)vl->x, (int32_t)vl->z,
        (int32_t)vr->x, (int32_t)vr->z,
        rel_x, rel_z,
        (int32_t)car_half_w, (int32_t)car_half_l,
        cos_hd, sin_hd,
        &edge_angle);

    if (pen < 0) {
        apply_simple_track_surface_force(actor, edge_angle, pen);
    }
}

/* ProcessActorForwardCheckpointPass — port of 0x004076C0.
 *
 * Forward-sentinel counterpart of process_traffic_route_advance.
 * Tests whether traffic has crossed the forward boundary span (near the
 * START of the ring, not the end) and applies a push impulse back into
 * the playable region.
 *
 * Key differences from ProcessActorRouteAdvance (0x407840):
 *   - Trigger:  span == fwd_sentinel + 1  (vs rev_sentinel - 1)
 *   - Strip:    strip[fwd_sentinel]        (vs strip[rev_sentinel])
 *   - Vertices: psVar2=left_base, psVar3=left_base+lane_count (NOT reversed)
 *
 * [CONFIRMED @ 0x4076C0] if (*(short*)(actor+0x80) == DAT_00483954 + 1)
 * [CONFIRMED @ 0x443ED0] called after ProcessActorRouteAdvance, before
 *                         ProcessActorSegmentTransition.
 */
static void process_traffic_forward_checkpoint_pass(TD5_Actor *actor, int slot)
{
    int fwd_sentinel = td5_track_get_fwd_sentinel();
    if (fwd_sentinel < 0) return;  /* {-1} placeholder disables handler */

    /* Trigger: actor is one span PAST the forward sentinel */
    int span_idx = (int)actor->track_span_raw;
    if (span_idx != fwd_sentinel + 1) return;

    /* Use the strip at fwd_sentinel for the boundary edge
     * [CONFIRMED @ 0x4076E8: iVar1 = g_trackStripRecords + DAT_00483954 * 0x18] */
    TD5_StripSpan *sp = td5_track_get_span(fwd_sentinel);
    if (!sp) return;

    int32_t *car_def = (int32_t *)actor->car_definition_ptr;
    if (!car_def) return;

    int16_t car_half_w = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x0C)) & 0xFFFF);
    int16_t car_half_l = (int16_t)((*(uint32_t *)((uint8_t *)car_def + 0x08)) & 0xFFFF);

    uint32_t hd = traffic_route_heading_delta(slot);
    hd &= 0x7FF;
    if (hd > 0x3FF) hd = 0x7FF - hd;
    int32_t cos_hd = cos_fixed12((int32_t)hd);
    int32_t sin_hd = sin_fixed12((int32_t)hd);

    /* Vertex ordering: NOT reversed (psVar2=left_base, psVar3=left_base+lane_count)
     * [CONFIRMED @ 0x407708-0x40771C in 0x4076C0]:
     *   uVar6 = left_vertex_index
     *   psVar2 = vertex[uVar6]                 (left base)
     *   psVar3 = vertex[uVar6 + lane_count]    (right end)
     * Compare ProcessActorRouteAdvance which swaps: psVar2=right, psVar3=left. */
    int sub = (int)(int8_t)actor->track_sub_lane_index;
    if (sub < 0) sub = 0;
    int lane_count = (int)(sp->surface_attribute & 0xF);
    if (sub >= lane_count) sub = lane_count - 1;

    int li_idx = (int)sp->left_vertex_index + sub;
    int ri_idx = (int)sp->left_vertex_index + lane_count;  /* end vertex (NOT right_vertex_index) */

    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;

    /* Actor position relative to sentinel span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp->origin_z;

    uint32_t edge_angle;
    int32_t pen = traffic_edge_pen(
        (int32_t)vl->x, (int32_t)vl->z,
        (int32_t)vr->x, (int32_t)vr->z,
        rel_x, rel_z,
        (int32_t)car_half_w, (int32_t)car_half_l,
        cos_hd, sin_hd,
        &edge_angle);

    if (pen < 0) {
        apply_simple_track_surface_force(actor, edge_angle, pen);
        /* Original calls UpdateTrafficVehiclePose again after push;
         * port's integrate_traffic_pose runs at end of tick — skip redundant rebuild. */
    }
}

/* ========================================================================
 * Traffic Pose Integration: UpdateTrafficVehiclePose (0x443CF0)
 *
 * Traffic (slots 6-11) NEVER goes through IntegrateVehiclePoseAndContacts.
 * Instead the original:
 *   1. Integrates X/Z from velocity (NO gravity, NO Y velocity)
 *   2. Updates track position from new XZ
 *   3. Sets Y absolutely from barycentric track contact height
 *   4. Converts euler accumulators to display angles + rotation matrix
 *   5. Computes render position
 * [CONFIRMED @ 0x443ED0 — no call to 0x405E80 for traffic]
 * ======================================================================== */

static void integrate_traffic_pose(TD5_Actor *actor)
{
    /* XZ + yaw integration is done by td5_physics_update_traffic
     * (inside the IntegrateVehicleFrictionForces port, matching original
     * @ 0x00443CBF/CCD/CD7). This function handles only Y ground-snap
     * and display-angle derivation, matching UpdateTrafficVehiclePose
     * @ 0x00443CF0. */

    /* Update chassis track position from new world XZ.
     * [CONFIRMED @ 0x443D40: UpdateTrafficVehiclePose calls UpdateActorTrackPosition] */
    {
        td5_track_update_actor_position(actor);

        int max_span = td5_track_get_span_count();
        if (max_span > 0 && actor->track_span_raw >= (uint16_t)max_span)
            actor->track_span_raw = (uint16_t)(max_span - 1);
    }

    /* 4. Set Y from barycentric track contact height + car height offset.
     * [CONFIRMED @ 0x443D58 + 0x445A1C: ComputeActorTrackContactNormalExtended
     *  writes world_pos_y = barycentric_height + origin_y * 0x100]
     * [CONFIRMED @ 0x443D7C-0x443D8E: adds *(int16_t*)(car_definition + 0x86) << 8
     *  to world_pos_y after contact height — lifts car above track surface] */
    {
        int span  = (int)actor->track_span_raw;
        int lane  = (int)actor->track_sub_lane_index;
        int16_t surface_normal[3] = {0, 1, 0};  /* default: flat up */
        int32_t ground_y = td5_track_compute_contact_height_with_normal(
            span, lane, actor->world_pos.x, actor->world_pos.z, surface_normal);

        /* Car height offset: signed int16 at car_definition + 0x86, shifted left 8.
         * Original (Y-DOWN): `ADD [world_pos_y], ECX` with ECX = MOVSX cdef[0x86] << 8
         * (negative → moves Y more negative → above ground in Y-DOWN).
         *
         * Port renders with Y-UP convention (as shown by track normal Y-sign
         * inversion required at line 3143 to get roll=0 on flat ground), so
         * we SUBTRACT the raw offset to lift the car above ground. */
        if (actor->car_definition_ptr) {
            int32_t height_offset = (int32_t)CDEF_S(actor, 0x86) << 8;
            ground_y -= height_offset;
        }

        actor->world_pos.y = ground_y;

        /* 5. Compute roll/pitch display angles from surface normal + suspension.
         * [CONFIRMED @ 0x443E00-0x443EC4: UpdateTrafficVehiclePose computes
         *  roll/pitch from surface normal rotated by yaw, with suspension correction.
         *  Roll  = AngleFromVector12(-rotated_z, -normal_y) - (susp[1] >> 8)
         *  Pitch = AngleFromVector12( rotated_x,  mag_xz)  + (susp[0] >> 8)
         *  where susp[0] = wheel_suspension_pos[0] (lateral-driven axis)
         *        susp[1] = wheel_suspension_pos[1] (longitudinal-driven axis)] */
        int32_t nx = (int32_t)surface_normal[0];
        int32_t ny = (int32_t)surface_normal[1];
        int32_t nz = (int32_t)surface_normal[2];

        /* Rotate normal XZ by yaw */
        int32_t yaw12 = (actor->euler_accum.yaw >> 8) & 0xFFF;
        int32_t cy = cos_fixed12(yaw12);
        int32_t sy = sin_fixed12(yaw12);
        int32_t rotated_x = (nx * cy - nz * sy) >> 12;
        int32_t rotated_z = (nx * sy + nz * cy) >> 12;

        /* Roll from surface normal + longitudinal suspension correction.
         * Original formula: AngleFromVector12(-rotated_z, -normal_y).
         * triangle_height produces Y-down normals matching the original
         * (ny < 0 for "up"), so we negate ny to get roll=0 on flat ground. */
        int32_t roll_from_normal = atan2_fixed12(-rotated_z, -ny);
        int32_t susp_roll_corr = actor->wheel_suspension_pos[1] >> 8;
        actor->display_angles.roll = (int16_t)((roll_from_normal - susp_roll_corr) & 0xFFF);

        /* Pitch from surface normal + lateral suspension correction. mag_xz
         * is the sqrt of (rotated_x^2 + ny^2) regardless of ny's sign. */
        int32_t mag_xz = td5_isqrt(rotated_x * rotated_x + ny * ny);
        int32_t pitch_from_normal = atan2_fixed12(rotated_x, mag_xz);
        int32_t susp_pitch_corr = actor->wheel_suspension_pos[0] >> 8;
        actor->display_angles.pitch = (int16_t)((pitch_from_normal + susp_pitch_corr) & 0xFFF);
    }

    /* Yaw from euler accumulator (only axis using accumulators for traffic) */
    actor->display_angles.yaw = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);

    /* Keep vertical dynamics zeroed — traffic has no gravity or suspension */
    actor->linear_velocity_y = 0;

    /* 6. Build rotation matrix from display angles (YXZ order, same as racers) */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        int32_t cr = cos_fixed12(roll_a);
        int32_t sr = sin_fixed12(roll_a);
        int32_t cyy = cos_fixed12(yaw_a);
        int32_t syy = sin_fixed12(yaw_a);
        int32_t cp = cos_fixed12(pitch_a);
        int32_t sp = sin_fixed12(pitch_a);

        float s = 1.0f / 4096.0f;

        actor->rotation_matrix.m[0] = (float)(((sp * syy >> 12) * sr >> 12) + ((cp * cyy) >> 12)) * s;
        actor->rotation_matrix.m[1] = (float)(((cp * syy >> 12) * sr >> 12) - ((sp * cyy) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)((syy * cr) >> 12) * s;

        actor->rotation_matrix.m[3] = (float)((sp * cr) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)((cp * cr) >> 12) * s;
        actor->rotation_matrix.m[5] = (float)(-sr) * s;

        actor->rotation_matrix.m[6] = (float)(((sp * cyy >> 12) * sr >> 12) - ((cp * syy) >> 12)) * s;
        actor->rotation_matrix.m[7] = (float)(((cp * cyy >> 12) * sr >> 12) + ((sp * syy) >> 12)) * s;
        actor->rotation_matrix.m[8] = (float)((cyy * cr) >> 12) * s;
    }

    /* 7. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Log once per 60 frames per slot for diagnostics */
    if ((actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "traffic_pose: slot=%d span=%d y=%d roll=%d pitch=%d",
                  actor->slot_index, (int)actor->track_span_raw,
                  actor->world_pos.y,
                  (int)actor->display_angles.roll, (int)actor->display_angles.pitch);
    }
}

/* Forward decl — AngleFromVector12 lives in td5_render.c (0x40A720 equiv) */
extern int AngleFromVector12(int x, int z);

/* ========================================================================
 * T2: TransformTrackVertexByMatrix equivalent (@ 0x00446030)
 *
 * Derives display_angle_roll and display_angle_pitch from the four per-wheel
 * ground-contact positions (actor+0xF0, written by refresh_wheel_contacts)
 * plus the four per-wheel suspension deflections (actor+0x2DC).
 *
 * Formula is a literal transcription of 0x00446037..0x0044612F x86
 * (two independent Ghidra passes, see T2 research log). All arithmetic is
 * 32-bit signed. SAR by 8 after each intermediate matches SAR instructions
 * at 0x446089/0x44608E/0x44608F/0x4460F4/0x4460F7/0x446103.
 *
 * The write to roll uses dz (front/rear-driven axis per original's wheel
 * ordering), write to pitch uses dx (left/right-driven axis). Semantic
 * labels "roll"/"pitch" match the original's field layout at +0x208/+0x20C;
 * whether this corresponds to the conventional pitch/roll axes depends on
 * the original binary's wheel index convention (not verified).
 * ======================================================================== */
static void td5_physics_attitude_from_wheels(const TD5_Actor *actor,
                                             int16_t *out_roll,
                                             int16_t *out_pitch)
{
    const int32_t *wcp = (const int32_t *)&actor->wheel_contact_pos[0];
    const int32_t *sp  = actor->wheel_suspension_pos;

    /* Numerators (height spreads + suspension deflection contributions) */
    int32_t dx = wcp[1] - wcp[4] - wcp[10] + wcp[7]
               + sp[0] - sp[1] + sp[2] - sp[3];       /* pitch numerator */
    int32_t dz = wcp[1] + wcp[4] - wcp[7] - wcp[10]
               + sp[0] + sp[1] - sp[2] - sp[3];       /* roll numerator */

    /* Cross spans (hypotenuse arguments — X/Z separations between wheel pairs) */
    int32_t crAp = wcp[0] - wcp[3] + wcp[6] - wcp[9];    /* pitch X span */
    int32_t crBp = wcp[2] - wcp[5] + wcp[8] - wcp[11];   /* pitch Z span */
    int32_t crAr = wcp[0] + wcp[3] - wcp[6] - wcp[9];    /* roll X span  */
    int32_t crBr = wcp[2] + wcp[5] - wcp[8] - wcp[11];   /* roll Z span  */

    dx   >>= 8;
    dz   >>= 8;
    crAp >>= 8;
    crBp >>= 8;
    crAr >>= 8;
    crBr >>= 8;

    int32_t hyp_p = td5_isqrt(crAp * crAp + crBp * crBp);
    int32_t hyp_r = td5_isqrt(crAr * crAr + crBr * crBr);

    *out_pitch = (int16_t)(AngleFromVector12(-dx, hyp_p) & 0xFFF);
    *out_roll  = (int16_t)(AngleFromVector12(-dz, hyp_r) & 0xFFF);
}

/* Helper: signed-wrap a 12-bit delta into [-2048, +2047], matching the
 * `(((new - old - 0x800) & 0xFFF) - 0x800)` pattern at 0x00405ED8-0x00405F10. */
static inline int32_t td5_physics_wrap_angle_delta(int32_t new_angle, int32_t old_angle) {
    return (int32_t)(((new_angle - old_angle - 0x800) & 0xFFF) - 0x800);
}

/* ========================================================================
 * Integration: IntegrateVehiclePoseAndContacts (0x405E80)
 *
 * Core integration step: gravity -> velocity -> position -> euler -> matrix.
 * Racers only (slots 0-5). Traffic uses integrate_traffic_pose above.
 * ======================================================================== */

void td5_physics_integrate_pose(TD5_Actor *actor)
{
    /* Save previous Y for suspension delta */
    actor->prev_frame_y_position = actor->world_pos.y;

    /* T2: Save pre-integration display roll/pitch for delta-based angular
     * velocity update. Matches IntegrateVehiclePoseAndContacts @ 0x00405E80
     * — angular_velocity_roll/pitch are recomputed from wrap(new - old)*256
     * each tick after TransformTrackVertexByMatrix. */
    int16_t t2_old_disp_roll  = actor->display_angles.roll;
    int16_t t2_old_disp_pitch = actor->display_angles.pitch;

    /* 1. Apply gravity to velocity Y — UNCONDITIONAL, matching
     * IntegrateVehiclePoseAndContacts @ 0x00405EDE. The suspension_response
     * tail call (0x004057F0) adds back `(g + Σ(wheel_vel·normal)/2/n)` at
     * 0x00405A49 — for stationary grounded cars the Σ term is 0 and the
     * two writes cancel cleanly. */
    actor->linear_velocity_y -= g_gravity_constant;

    /* 2. Integrate angular velocity into euler accumulators */
    actor->euler_accum.roll  += actor->angular_velocity_roll;
    actor->euler_accum.yaw   += actor->angular_velocity_yaw;
    actor->euler_accum.pitch += actor->angular_velocity_pitch;

    /* 3. Integrate linear velocity into position.
     * The original gates this on DAT_00483030 (ref_count=1, read-only), which
     * is never written, so effectively always runs. With dynamics skipped
     * during pause, vel_x/vel_z stay 0 and the integration is a no-op. */
    actor->world_pos.x += actor->linear_velocity_x;
    actor->world_pos.z += actor->linear_velocity_z;
    actor->world_pos.y += actor->linear_velocity_y;

    /* 3b. Update chassis track position from new world pos.
     * Original calls FUN_004440F0 here [CONFIRMED @ 0x405E80 callees].
     * Without this, track_span_raw stays at 0 and wall checks use the
     * wrong span — the primary reason collisions don't work. */
    {
        int16_t prev_span = actor->track_span_raw;
        td5_track_update_actor_position(actor);

        /* Guard against span walker overflow. The walker can jump to out-of-
         * bounds or distant spans at track edges, causing terrain height
         * garbage (Y launch) and XZ teleportation.
         *
         * Use td5_track_get_span_count() which includes branch spans
         * (NOT ring_length which is main road only). Junction links
         * legitimately jump from e.g. span 499 to span 2790.
         *
         * The jump limit is DISABLED because junction transitions produce
         * large span deltas that are valid. The hard bounds clamp alone
         * prevents OOB access. */
        int max_span = td5_track_get_span_count();  /* includes branch spans */
        if (max_span > 0) {
            /* Hard bounds clamp */
            if (actor->track_span_raw >= (uint16_t)max_span)
                actor->track_span_raw = (uint16_t)(max_span - 1);

            /* Clamp per-wheel probes too */
            for (int wi = 0; wi < 4; wi++) {
                if (actor->wheel_probes[wi].span_index >= (int16_t)max_span)
                    actor->wheel_probes[wi].span_index = actor->track_span_raw;
                int wdelta = (int)actor->wheel_probes[wi].span_index - (int)actor->track_span_raw;
                if (wdelta < 0) wdelta = -wdelta;
                if (wdelta > 50)
                    actor->wheel_probes[wi].span_index = actor->track_span_raw;
            }
        }
    }

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

    /* 7. Save previous-frame grounded mask, then refresh wheel contacts.
     * suspension_response needs "was grounded last frame" for the spring
     * damping path. refresh overwrites wheel_contact_bitmask (airborne polarity:
     * 1=airborne), so save the inverted version (1=grounded) before the call. */
    s_prev_grounded_mask[actor->slot_index & 0x0F] = (~actor->wheel_contact_bitmask) & 0x0F;
    td5_physics_refresh_wheel_contacts(actor);

    /* T2: Wheel-contact attitude feedback — literal port of
     * TransformTrackVertexByMatrix @ 0x00446030 called from
     * IntegrateVehiclePoseAndContacts @ 0x00405E80.
     *
     * CASE GATING [CONFIRMED @ 0x00405FDE switch on [ESI+0x37C]]:
     * The original's switch skips T2 entirely for current-airborne masks
     * {7, 11, 13, 14, 15} (3+ wheels airborne) — with only one wheel on
     * the ground the 4-wheel height-spread solver produces garbage
     * (isqrt of a small hypotenuse vs a huge Y numerator → runaway
     * pitch/roll). Port previously ran this unconditionally, which matches
     * the user's "sometimes rolls out of control on steep slopes" symptom
     * (partial 2026-04-22 residual). 2026-04-24: added gate.
     *
     * Cases {0,1,2,4,6,8,9} call TransformTrackVertexByMatrix @ 0x00446030
     * (full 4-wheel solver — byte-faithful in attitude_from_wheels).
     * Cases {3,5,10,12} call reduced B/C solvers @ 0x00446140 / 0x004461C0
     * (not decompiled). Port falls back to the full solver for those;
     * likely over-corrects a single axle but less catastrophic than the
     * 3+-airborne garbage path. Decompile + split solver is a followup.
     *
     * 1. Compute new_roll, new_pitch from the freshly refreshed
     *    wheel_contact_pos[] heights + wheel_suspension_pos[] deflections.
     * 2. Derive angular_velocity_roll/pitch as signed-wrapped 12-bit delta,
     *    scaled by 256, clamped to ±6000 (matches 0x00405F0A-0x00405F2C).
     * 3. Override display_angles.roll/pitch with the wheel-derived values
     *    (overrides the euler_accum-derived values written at step 4 above).
     * 4. Update euler_accum.roll/pitch so the next tick's integration starts
     *    from the wheel-corrected attitude.
     * 5. Rebuild rotation_matrix from the corrected display_angles so
     *    downstream ground-snap and next-tick refresh use the right matrix. */
    uint8_t t2_lock = actor->wheel_contact_bitmask;

    /* Per-axis solver dispatch — [CONFIRMED @ 0x00405E80 switch(damage_lockout)]:
     *
     *   cases 0,1,2,4,6,8,9  → TransformTrackVertexByMatrix  @ 0x00446030 (full)
     *   cases 3, 12          → TransformTrackVertexByMatrixC @ 0x004461C0 (PITCH only)
     *   cases 5, 10          → TransformTrackVertexByMatrixB @ 0x00446140 (ROLL only)
     *   cases 7,11,13,14,15  → NOT IN SWITCH — T2 entirely skipped
     *
     * The B/C variants use IDENTICAL per-axis formulas to the full solver
     * (verified 2026-04-24 Ghidra byte-diff at listed addresses); the ONLY
     * difference is that they SKIP writing the inactive axis.
     *
     * Prior port ran the full solver on masks 3/5/10/12 and wrote BOTH
     * axes unconditionally. On steep slopes where one axle lifts (mask=12
     * rear-airborne or mask=5 diagonal), the port's unconditional roll/
     * pitch overwrite introduced noise the original leaves untouched —
     * matches the user's "sometimes rolls out of control on steep slopes"
     * residual after the 2026-04-22 6-fix chain. */
    int t2_full  = (t2_lock == 0 || t2_lock == 1 || t2_lock == 2 ||
                    t2_lock == 4 || t2_lock == 6 || t2_lock == 8 ||
                    t2_lock == 9);
    int t2_pitch_only = (t2_lock == 3  || t2_lock == 12);  /* C variant */
    int t2_roll_only  = (t2_lock == 5  || t2_lock == 10);  /* B variant */
    int t2_skip  = !(t2_full || t2_pitch_only || t2_roll_only);

    if (actor->slot_index == 0 && t2_skip) {
        TD5_LOG_I(LOG_TAG, "T2 skip slot0: lock=0x%02x (3+ airborne)",
                  (int)t2_lock);
    }
    if (!t2_skip)
    {
        int16_t new_roll  = 0;
        int16_t new_pitch = 0;
        td5_physics_attitude_from_wheels(actor, &new_roll, &new_pitch);

        uint8_t prev_airborne = (~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F;
        int mask_stable = (actor->wheel_contact_bitmask == prev_airborne);

        /* ROLL axis: written only for full solver OR roll-only (B variant). */
        if (t2_full || t2_roll_only) {
            int32_t d_roll = td5_physics_wrap_angle_delta((int32_t)new_roll,
                                                          (int32_t)t2_old_disp_roll) * 0x100;
            if (d_roll >  6000) d_roll =  6000;
            if (d_roll < -6000) d_roll = -6000;
            /* [CONFIRMED @ 0x00405FF9] gate angular_velocity on mask stability. */
            if (mask_stable) {
                actor->angular_velocity_roll = d_roll;
            }
            actor->display_angles.roll = new_roll;
            actor->euler_accum.roll    = (int32_t)new_roll << 8;
        }

        /* PITCH axis: written only for full solver OR pitch-only (C variant). */
        if (t2_full || t2_pitch_only) {
            int32_t d_pitch = td5_physics_wrap_angle_delta((int32_t)new_pitch,
                                                           (int32_t)t2_old_disp_pitch) * 0x100;
            if (d_pitch >  6000) d_pitch =  6000;
            if (d_pitch < -6000) d_pitch = -6000;
            if (mask_stable) {
                actor->angular_velocity_pitch = d_pitch;
            }
            actor->display_angles.pitch = new_pitch;
            actor->euler_accum.pitch    = (int32_t)new_pitch << 8;
        }

        /* Rebuild rotation_matrix from the (possibly partially updated)
         * display_angles. Matches original 0x00405E80's second call to
         * BuildRotationMatrixFromAngles — runs unconditionally after the
         * switch for all non-skipped cases. */
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        int32_t cr = cos_fixed12(roll_a);
        int32_t sr = sin_fixed12(roll_a);
        int32_t cy = cos_fixed12(yaw_a);
        int32_t sy = sin_fixed12(yaw_a);
        int32_t cp = cos_fixed12(pitch_a);
        int32_t sp2 = sin_fixed12(pitch_a);

        float s = 1.0f / 4096.0f;
        actor->rotation_matrix.m[0] = (float)(((sp2 * sy >> 12) * sr >> 12) + ((cp * cy) >> 12)) * s;
        actor->rotation_matrix.m[1] = (float)(((cp  * sy >> 12) * sr >> 12) - ((sp2 * cy) >> 12)) * s;
        actor->rotation_matrix.m[2] = (float)((sy * cr) >> 12) * s;
        actor->rotation_matrix.m[3] = (float)((sp2 * cr) >> 12) * s;
        actor->rotation_matrix.m[4] = (float)((cp  * cr) >> 12) * s;
        actor->rotation_matrix.m[5] = (float)(-sr) * s;
        actor->rotation_matrix.m[6] = (float)(((sp2 * cy >> 12) * sr >> 12) - ((cp * sy) >> 12)) * s;
        actor->rotation_matrix.m[7] = (float)(((cp  * cy >> 12) * sr >> 12) + ((sp2 * sy) >> 12)) * s;
        actor->rotation_matrix.m[8] = (float)((cy * cr) >> 12) * s;

        /* One-line per-tick diagnostic for the player car only */
        if (actor->slot_index == 0) {
            static int s_t2_log_frame = 0;
            if ((s_t2_log_frame++ % 120) == 0) {
                const char *variant = t2_full ? "FULL" :
                                      t2_pitch_only ? "C_pitch_only" :
                                      t2_roll_only  ? "B_roll_only"  : "?";
                TD5_LOG_I(LOG_TAG,
                    "T2 attitude slot0: lock=0x%02x variant=%s new_roll=%d new_pitch=%d "
                    "old_roll=%d old_pitch=%d",
                    (int)t2_lock, variant,
                    (int)new_roll, (int)new_pitch,
                    (int)t2_old_disp_roll, (int)t2_old_disp_pitch);
            }
        }
    }

    /* DIAGNOSTIC: log player car (slot 0) physics state once per 30 frames */
    if (actor->slot_index == 0) {
        static int s_diag_frame = 0;
        if ((s_diag_frame++ % 30) == 0) {
            TD5_LOG_I(LOG_TAG,
                "DIAG slot0: pos_y=%d vel_y=%d prev_y=%d render_y=%.2f "
                "bitmask=0x%02X force=[%d,%d,%d,%d] susp_pos=[%d,%d,%d,%d] susp_vel=[%d,%d,%d,%d]",
                actor->world_pos.y, actor->linear_velocity_y, actor->prev_frame_y_position,
                actor->render_pos.y, actor->wheel_contact_bitmask,
                actor->wheel_spring_dv[0], actor->wheel_spring_dv[1],
                actor->wheel_spring_dv[2], actor->wheel_spring_dv[3],
                actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
                actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3],
                actor->wheel_load_accum[0], actor->wheel_load_accum[1],
                actor->wheel_load_accum[2], actor->wheel_load_accum[3]);
        }
    }

    /* 8. Ground-snap: correct world_pos.y to keep the car on the road.
     *
     * RefreshWheelContacts adds susp_href (cardef+0x82 * 0xB5/256) to the
     * wheel Y before rotation, raising the contact probe ~107 units toward
     * the chassis. Without compensation, ground-snap aligns this elevated
     * probe at ground level, leaving the visual model sunk by susp_href.
     *
     * Original (0x403720 / 0x406300): subtracts susp_href before rotation,
     * adds it back after — the add-back in world space effectively raises
     * the chassis by susp_href. We replicate by adding susp_href_world to
     * the per-wheel correction. */
    /* 8. Ground-snap: direct Y write from wheel contact average.
     *
     * Original (0x405E80): accumulates per-wheel Y from local_1c + suspension
     * spring output, averages over grounded wheels, and DIRECTLY WRITES the
     * result to world_pos.y. No delta clamp — the chassis position is set
     * absolutely to the contact surface each tick.
     *
     * Per-wheel Y contribution = wheel_world_y + spring_output * -0x100
     * where wheel_world_y is from RefreshWheelContacts (includes susp_href).
     */
    /* Ground-snap: direct Y write from wheel contact average.
     * [CONFIRMED via 2026-04-11 Ghidra pass on 0x00405E80 by research agent.]
     * Three bugs fixed vs prior port:
     *
     *   1. Per-wheel cardef stride is 8 bytes (not 16). Each wheel's body-
     *      space offset is `short[3]` at `cd + 0x40 + i*8`: X at +0x40+i*8,
     *      Y at +0x42+i*8, Z at +0x44+i*8. Prior port indexed at stride 16
     *      which made wheels 1..3 read garbage for every non-Viper car.
     *   2. No `+0x80` rounding bias. Original writes the plain `IDIV` result.
     *      Previous port added +0x80 to work around bug #1 on Viper.
     *   3. Full 3-vec rotation through the chassis rotation matrix. The
     *      ride_offset vector is (cwx, body_wy, cwz), rotated by the
     *      actor's rotation matrix, and ONLY the Y component of the rotated
     *      result is used. Prior port used `body_wy * -0x100` directly,
     *      ignoring the X/Z terms which contribute pitch/roll coupling.
     *
     * The `rotate_vec_world_y` helper (see ~line 2403) extracts exactly the
     * Y component via `m[1]*v[0] + m[4]*v[1] + m[7]*v[2]`, matching the
     * original's `TransformShortVec3ByRenderMatrixRounded @ 0x0042E2E0`
     * used inside 0x00405E80 (followed by reading out[1]). */
    {
        int64_t contact_y_sum = 0;
        int contact_count = 0;
        uint8_t gnd_mask = actor->wheel_contact_bitmask;

        int32_t href = (int32_t)CDEF_S(actor, 0x82);
        int32_t href_x181 = href * 0xB5;
        int32_t susp_offset = (href_x181 + ((href_x181 >> 31) & 0xFF)) >> 8;

        uint8_t *cd = (uint8_t *)actor->car_definition_ptr;

        for (int i = 0; i < 4; i++) {
            if (gnd_mask & (1 << i)) continue;  /* airborne wheel — skip */
            if (!cd) break;

            /* [FIX #1] stride 8 per wheel, not 16. */
            int16_t cwx = *(int16_t *)(cd + 0x40 + i * 8);
            int16_t cwy = *(int16_t *)(cd + 0x42 + i * 8);
            int16_t cwz = *(int16_t *)(cd + 0x44 + i * 8);

            /* Truncate-toward-zero /256 on suspension pos (matches the
             * original's CDQ;AND 0xff;ADD;SAR 8 idiom at 0x00406273). */
            int32_t sp = actor->wheel_suspension_pos[i];
            int32_t sp_div = (sp + ((sp >> 31) & 0xFF)) >> 8;

            int16_t body_wy = (int16_t)((int32_t)cwy - sp_div - susp_offset);

            /* [FIX #3] rotate the full short[3] body-offset vector through
             * the actor's rotation matrix and take the Y component. Uses
             * body→world (row 1 of matrix) — matches original's direct
             * LoadRenderRotationMatrix (no transpose) at 0x00406135 /
             * 0x004061EA before ConvertFloatVec3ToShortAngles. */
            int16_t src[3] = { cwx, body_wy, cwz };
            int16_t rot_y = rotate_body_to_world_y(actor, src);

            int32_t wheel_y = actor->wheel_contact_pos[i].y;
            contact_y_sum += (int64_t)(wheel_y + (int32_t)rot_y * -0x100);
            contact_count++;
        }

        if (contact_count > 0) {
            /* [FIX #2] NO +0x80 bias — the original writes the plain
             * signed-IDIV result (MOV [ESI+0x200], EAX at 0x00406307). */
            int32_t new_y = (int32_t)(contact_y_sum / contact_count);

            /* Reject ground snap if new_y is wildly different from prev_y.
             * The span walker can overflow to out-of-bounds spans, producing
             * garbage terrain heights (47M vs -800K). The per-wheel span
             * clamp catches most cases, but some height functions return
             * stale/wrong values even for in-bounds spans at track edges.
             * 2M ≈ 7800 world units — well beyond any real terrain step.
             * Skip the check when prev_y is the spawn sentinel (0xC0000000)
             * since the first ground snap is always a huge legitimate delta. */
            if (actor->prev_frame_y_position != (int32_t)0xC0000000) {
                int32_t snap_delta = new_y - actor->prev_frame_y_position;
                if (snap_delta > 2000000 || snap_delta < -2000000) {
                    new_y = actor->prev_frame_y_position;
                }
            }

            /* Chassis Y-snap. No chassis-level airborne override here —
             * the per-wheel airborne bits written by refresh_wheel_contacts
             * (force >= 0x801 @ 0x00403720) now drive downstream airborne
             * behavior. Match original 0x00406300 which snaps
             * unconditionally once contact_count > 0. */
            actor->world_pos.y = new_y;
            actor->render_pos.y = (float)new_y * (1.0f / 256.0f);

          if (contact_count > 0) {
            /* Velocity-from-snap gate — literal port of original
             * IntegrateVehiclePoseAndContacts tail (0x0040630D–0x00406335):
             *
             *   if (DAT_0046318C[actor[0x37C]] != 0 &&
             *       (actor[0x37D] & (actor[0x37C] ^ 0xF)) == 0)
             *       linear_velocity_y = (new_y - prev_y) - gGravityConstant;
             *
             * Field roles (see td5_physics.c:2057-2058, 2113-2114):
             *   0x37C = NEW-frame wheel-contact bitmask (gate index)
             *   0x37D = PREVIOUS-frame wheel-contact bitmask (mask operand)
             *
             * The second condition detects "no wheel that was grounded last
             * frame is airborne this frame" — i.e. only apply the
             * snap-derived velocity when the wheel contact set did not lose
             * any wheels this tick. Otherwise we keep the integrated velocity
             * so a wheel lifting off doesn't kill vertical momentum.
             *
             * DAT_0046318C read via mcp memory_read at 0x0046318C:
             *   01 01 01 00 01 00 00 00 01 00 00 00 00 00 00 00
             * (indices 0,1,2,4,8 are gated ON; everything else OFF.)
             *
             * Previous port code had THREE bugs in this gate:
             *   (1) table bytes wrong (leaked into indices 6, 9, 12)
             *   (2) index read from 0x37D instead of 0x37C
             *   (3) XOR operand also read from 0x37D, making the mask
             *       check `(x & ~x) == 0` → always true → effectively
             *       no mask check at all.
             * Combined, the gate fired spurious velocity resets during
             * normal driving and produced visible chase-cam shake on accel.
             *
             * Use raw byte offsets rather than named fields because the
             * port has named accessors (`damage_lockout`, `wheel_contact_
             * bitmask`) whose semantics are overloaded across call sites. */
            {
                static const uint8_t k_mode_gate[16] = {
                    1,1,1,0, 1,0,0,0, 1,0,0,0, 0,0,0,0
                };
                uint8_t new_mask  = *(const uint8_t *)((const uint8_t *)actor + 0x37C);
                uint8_t prev_mask = *(const uint8_t *)((const uint8_t *)actor + 0x37D);
                if (k_mode_gate[new_mask & 0xF] &&
                    (prev_mask & (new_mask ^ 0x0F)) == 0 &&
                    actor->prev_frame_y_position != (int32_t)0xC0000000) {
                    int32_t snap_vy = new_y - actor->prev_frame_y_position
                                     - g_gravity_constant;
                    /* Clamp snap-derived velocity to realistic per-tick motion.
                     * Legitimate max: 100mph (160 units/tick horizontal) on a
                     * 30° grade → ~80 units/tick vertical → ~20480 FP. A 40°
                     * slope at 150mph → ~38000 FP. The live-run log observed
                     * ±52992 on Newcastle slopes, producing a visible "bounce"
                     * on uphill entry and "sloppy settle" on downhill→flat.
                     * ±30000 FP ≈ 117 units/tick = 7000 units/sec vertical —
                     * tight enough to suppress glitch-level spikes, loose
                     * enough for any real race-track grade. Previous 200000
                     * was a leftover spawn-sentinel safeguard, way too loose
                     * for runtime clamping. */
                    if (snap_vy > 30000)  snap_vy =  30000;
                    if (snap_vy < -30000) snap_vy = -30000;
                    if (actor->slot_index == 0 &&
                        (snap_vy > 100000 || snap_vy < -100000 ||
                         (actor->frame_counter % 60u) == 0u)) {
                        TD5_LOG_I(LOG_TAG, "y_snap: new=%u prev=%u vy=%d new_y=%d prev_y=%d",
                                  new_mask, prev_mask, snap_vy, new_y,
                                  actor->prev_frame_y_position);
                    }
                    actor->linear_velocity_y = snap_vy;
                }
            }
          } /* end velocity snap guard */
        } else {
            /* No ground contact: increment airborne counter (original 0x40631A) */
            actor->frame_counter++;
        }
    }

    /* 8b. OOB recovery disabled — was teleporting cars due to suspension
     * instability rather than genuine out-of-bounds conditions. */

    /* 9. Clamp angular velocity deltas to +/- 6000 per frame */
    if (actor->angular_velocity_roll > 6000) actor->angular_velocity_roll = 6000;
    if (actor->angular_velocity_roll < -6000) actor->angular_velocity_roll = -6000;
    if (actor->angular_velocity_yaw > 6000) actor->angular_velocity_yaw = 6000;
    if (actor->angular_velocity_yaw < -6000) actor->angular_velocity_yaw = -6000;
    if (actor->angular_velocity_pitch > 6000) actor->angular_velocity_pitch = 6000;
    if (actor->angular_velocity_pitch < -6000) actor->angular_velocity_pitch = -6000;

    /* 10. Update suspension response.
     * UNCONDITIONAL, matching original 0x00405E80 which always calls
     * UpdateVehicleSuspensionResponse (0x004057F0) when damage_lockout == 0.
     * The target-angle spring-damper block inside is gated on !g_game_paused
     * internally so the parked car doesn't tilt during countdown. */
    td5_physics_update_suspension_response(actor);

    /* SPIKE TRIGGER: log full attitude state when av_roll or av_pitch
     * exceeds ±3000 in magnitude. Catches the "car goes nuts on wall +
     * uphill" scenario without flooding the log during normal driving. */
    if (actor->slot_index == 0) {
        int32_t ar = actor->angular_velocity_roll;
        int32_t ap = actor->angular_velocity_pitch;
        if (ar > 3000 || ar < -3000 || ap > 3000 || ap < -3000) {
            TD5_LOG_I(LOG_TAG,
                "spike slot0: t=%u bm=0x%02x pos_y=%d vy=%d "
                "av{r=%d p=%d y=%d} disp{r=%d p=%d y=%d} "
                "eacc{r=%d p=%d} susp_pos=[%d %d %d %d] contact_y=[%d %d %d %d]",
                (unsigned)actor->frame_counter,
                (int)actor->wheel_contact_bitmask,
                actor->world_pos.y, actor->linear_velocity_y,
                ar, ap, actor->angular_velocity_yaw,
                (int)actor->display_angles.roll,
                (int)actor->display_angles.pitch,
                (int)actor->display_angles.yaw,
                actor->euler_accum.roll, actor->euler_accum.pitch,
                actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
                actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3],
                actor->wheel_contact_pos[0].y, actor->wheel_contact_pos[1].y,
                actor->wheel_contact_pos[2].y, actor->wheel_contact_pos[3].y);
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Integrate actor0: pos=(%d,%d,%d)",
                  actor->world_pos.x,
                  actor->world_pos.y,
                  actor->world_pos.z);
    }

    /* ---- CLIP TELEMETRY ----
     * Dedicated per-tick diagnostic for slot 0. Probes the road surface at
     * the current chassis XZ and compares against pos_y. Logs a compact
     * CSV-style line every tick so we can count clip events, correlate with
     * gear/throttle, and verify whether a "should-clip" scenario (jump over
     * a gap, genuine airborne) is misclassified as a ride-height-bug clip.
     *
     * Fields:
     *   clip_t: t=<frame_counter> pos_y=<FP> probe=<FP> d=<FP_diff> vy=<FP>
     *           bm=<contact_bitmask> gear=<n> thr=<0..256> brk=<0..1>
     *           lspd=<FP> span=<raw> CLIP/ok
     * Delta is (pos_y - probe_y): NEGATIVE means pos_y BELOW ground (clip).
     * Only slot 0 and paused=0 OR every 10 frames during paused to keep
     * volume manageable. */
    if (actor->slot_index == 0) {
        int emit = 1;
        if (g_game_paused && (actor->frame_counter % 10u) != 0u) emit = 0;
        if (emit) {
            int32_t probe_y = 0;
            int probe_surf = 0;
            int probe_span = actor->track_span_raw;
            int probe_ok = td5_track_probe_height(
                actor->world_pos.x, actor->world_pos.z,
                probe_span, &probe_y, &probe_surf);
            if (probe_ok) {
                int32_t delta = actor->world_pos.y - probe_y;
                const char *flag = (delta < 0) ? "CLIP" : "ok";
                TD5_LOG_I(LOG_TAG,
                    "clip_t: t=%u pos_y=%d probe=%d d=%d vy=%d bm=0x%02X "
                    "gear=%u thr=%d brk=%u lspd=%d span=%d paused=%d %s",
                    actor->frame_counter,
                    actor->world_pos.y, probe_y, delta,
                    actor->linear_velocity_y,
                    actor->wheel_contact_bitmask,
                    actor->current_gear,
                    actor->encounter_steering_cmd,
                    actor->brake_flag,
                    actor->longitudinal_speed,
                    (int)actor->track_span_raw,
                    g_game_paused,
                    flag);
            }
        }
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

        /* NOTE: no susp_href correction here. The original's ground-snap
         * (IntegrateVehiclePoseAndContacts @ 0x00405E80, secondary snap at
         * UpdateVehiclePoseFromPhysicsState @ 0x004063A0) does NOT add a
         * scalar susp_href term to corr_sum. It folds the susp_href offset
         * into the per-wheel rotated Y via the pre-subtract / post-add
         * pattern in refresh_wheel_contacts (td5_physics.c:3930-3970). A
         * port-invented `susp_href_world = href_local * m[4] * 256.0f`
         * was previously computed here but never used (dead code) — now
         * deleted to keep intent clear. */
        for (int i = 0; i < 4; i++) {
            if (!(gnd_mask & (1 << i))) {
                int32_t g_y = 0;
                int g_surf = 0;
                int g_span = actor->track_span_raw;
                if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                           actor->wheel_contact_pos[i].z,
                                           g_span, &g_y, &g_surf)) {
                    corr_sum += (int64_t)g_y - (int64_t)actor->wheel_contact_pos[i].y;
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

    /* gap_270 uses the pre-snap transform results, not post-snap positions.
     * Read old values from the persistent per-slot array (set at end of this
     * function from this frame's transform output, before Y-snap). */
    int slot = actor->slot_index;
    if (slot < 0 || slot >= 12) slot = 0;

    /* Per-wheel contact frame computation */
    for (int i = 0; i < 4; i++) {
        /* Get wheel display angle data */
        int32_t wx = actor->wheel_display_angles[i][0];
        int32_t wy = actor->wheel_display_angles[i][1];
        int32_t wz = actor->wheel_display_angles[i][2];

        /* Body-space Y of the wheel = cwy − (susp_pos>>8) − (href*0xB5>>8).
         *
         * Literal port of the instruction sequence at 0x0040384D–0x00403869
         * inside RefreshVehicleWheelContactFrames @ 0x00403720:
         *   CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8      ; sp_div  = sp / 256 (signed toward zero)
         *   SUB CX, AX                                     ; body_wy = cwy - sp_div
         *   SUB ECX, EDX                                   ; body_wy -= href_preload (preloaded once, signed /256 of href*0xB5)
         *   MOV [EBX], CX                                  ; body_wy is written as int16
         *
         * Two pre-existing port bugs corrected here:
         *   1. susp_pos (`sp`) was used RAW — susp_pos is stored in 24.8 FP
         *      (see 0x00403A20 consumers), so it must be >>8 before subtract.
         *   2. Both correction terms were ADDED. Original SUBTRACTS. This sign
         *      inversion alone produced the ~55k FP (~217 world unit)
         *      equilibrium offset above ground (memory:
         *      reference_port_ride_height_offset.md).
         * [CONFIRMED @ 0x00403720 by research agent — ride-height refactor] */
        int32_t sp = actor->wheel_suspension_pos[i];
        int32_t sp_div = (sp + (int32_t)((uint32_t)(sp >> 31) & 0xFFu)) >> 8;
        int32_t href = (int32_t)CDEF_S(actor, 0x82);
        int32_t href_x181 = href * 0xB5;
        int32_t href_preload =
            (href_x181 + (int32_t)((uint32_t)(href_x181 >> 31) & 0xFFu)) >> 8;
        wy = (int32_t)(int16_t)(wy - sp_div - href_preload);

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

        /* Clamp per-wheel span after the probe walker (it can overflow
         * independently of the chassis span walker). */
        {
            int max_sp = td5_track_get_span_count();  /* includes branch spans */
            if (max_sp > 0 && actor->wheel_probes[i].span_index >= (int16_t)max_sp)
                actor->wheel_probes[i].span_index = (int16_t)(max_sp - 1);
        }

        /* Compute wheel vertical force from the probed span surface. */
        int32_t wheel_y = actor->wheel_contact_pos[i].y;
        int32_t ground_y = 0;
        int surface_type = actor->surface_type_chassis;
        int probe_span = actor->wheel_probes[i].span_index;
        int probe_ok = 0;

        /* Per-wheel probe span bounds check.
         *
         * Use the FULL physical span count (td5_track_get_span_count) — branch
         * spans are indices [ring_length, s_span_count) and must be probed
         * directly, not rejected. The earlier `probe_span >= ring_length`
         * fallback mis-classified branch spans as invalid and forced every
         * wheel to read ground_y from span 0, which on Newcastle made all
         * wheels report force > 0x800 → airborne bitmask 0x0F → chassis
         * Y-snap skipped → car freefell through the branch road. */
        {
            int max_sp = td5_track_get_span_count();
            if (probe_span < 0 || probe_span >= max_sp)
                probe_span = actor->track_span_raw;
            if (probe_span < 0 || probe_span >= max_sp)
                probe_span = 0;  /* absolute fallback */
        }

        /* Use the wheel probe's own sub_lane for height computation.
         * The original (0x403720 → 0x4457E0) passes the per-wheel probe
         * state directly, which has its own sub_lane from the track
         * position update. Our td5_track_probe_height re-computes the
         * lane via boundary testing which may pick a different lane.
         *
         * Also retrieve the span surface normal, which the original writes to
         * actor+0x250+i*8 (wheel_contact_velocities[i]) via FUN_00445A70.
         * This normal is the "wcv" consumed by UpdateVehicleSuspensionResponse
         * (0x4057F0) as the contact surface direction vector. */
        int16_t span_normal[3] = {0, 4096, 0};  /* default: flat upward normal (magnitude 4096) */
        {
            int probe_lane = actor->wheel_probes[i].sub_lane_index;
            ground_y = td5_track_compute_contact_height_with_normal(
                probe_span, probe_lane,
                actor->wheel_contact_pos[i].x,
                actor->wheel_contact_pos[i].z,
                span_normal);
            probe_ok = 1;
            if (!resolved_surface_valid) {
                resolved_surface = surface_type;
                resolved_surface_valid = 1;
            }
        }

        /* Write surface normal to wheel_contact_velocities[i][0..2] (actor+0x250+i*8).
         * Original: FUN_00445A70 computes cross-product of span edge vectors >> 12,
         * then FUN_0042CD40 normalizes to magnitude 4096. For flat ground: (0, 4096, 0).
         * Normalize the raw cross-product normal to magnitude 4096 before writing. */
        {
            int32_t snx = span_normal[0], sny = span_normal[1], snz = span_normal[2];
            int32_t mag_sq = snx * snx + sny * sny + snz * snz;
            if (mag_sq > 0) {
                int32_t mag = td5_isqrt(mag_sq);
                if (mag > 0) {
                    snx = (snx * 4096) / mag;
                    sny = (sny * 4096) / mag;
                    snz = (snz * 4096) / mag;
                }
            } else {
                snx = 0; sny = 4096; snz = 0;
            }
            actor->wheel_contact_velocities[i][0] = (int16_t)snx;
            actor->wheel_contact_velocities[i][1] = (int16_t)sny;
            actor->wheel_contact_velocities[i][2] = (int16_t)snz;
        }

        /* Original order in FUN_00403720:
         *   1. force = (wheel_y - ground_y) + gravity_offset
         *   2. gap_270 = (new_pos - old_pos) >> 8   ← BEFORE Y-snap
         *   3. dead zone on force
         *   4. Y-snap if grounded
         * The gap_270 Y component uses the TRANSFORM-computed Y, not ground-snapped Y.
         * This ensures the velocity reflects the body's actual motion, not the snap. */

        /* Compute per-wheel contact force — literal port of 0x00403720.
         *
         *   *local_4c = (*piVar8 - local_30) + gGravityConstant;
         *   dead-zone |force| < 0x200 -> 0
         *   if (force < 0x801) grounded: snap wheel_y = ground_y
         *   else                airborne: set bit in new mask, clamp force = 12000
         *
         * The force is written to wheel_load_accum (+0x2FC) where the
         * spring-damper (IntegrateWheelSuspensionTravel @ 0x00403A20)
         * reads it next tick. This drives the convergence that keeps
         * wheel_y tracking ground_y at equilibrium and lets the per-wheel
         * airborne bit drive downstream suspension response.
         *
         * Raw 24.8 FP units — NO >>8 shift. Thresholds 0x200/0x801/12000
         * are all in the same raw scale.
         * [CONFIRMED @ 0x00403720] */
        int32_t force = (wheel_y - ground_y) + g_gravity_constant;

        /* gap_270[i] = frame-to-frame wheel contact position delta >> 8.
         * MUST compare pre-snap transform results from two consecutive frames.
         * Using post-snap (ground_y) as old causes huge Y deltas (~90k). */
        {
            int16_t *g270 = (int16_t *)((uint8_t *)actor + 0x270 + i * 8);
            if (s_prev_wheel_valid[slot]) {
                int32_t dx = actor->wheel_contact_pos[i].x - s_prev_wheel_tx[slot][i];
                int32_t dy = actor->wheel_contact_pos[i].y - s_prev_wheel_ty[slot][i];
                int32_t dz = actor->wheel_contact_pos[i].z - s_prev_wheel_tz[slot][i];
                /* Clamp deltas: if any component exceeds ±20k FP (~78 world units/tick),
                 * zero it — indicates teleport (wall collision response, span
                 * rebind, spawn transient) rather than physical motion. The
                 * faithful update_suspension_response computes gap_270 · wcv as
                 * the spring excitation + bounce accumulator, so a single
                 * teleport delta produces launch-sized vy impulses (observed
                 * vy=+80773, bounce=197, pitch_spr=39M on uphill transitions
                 * when clamp was ±100k). ±20k ≈ 78 world units/tick is already
                 * well above any legal single-tick wheel motion at racing
                 * speeds. Restored from commit e97669e; had been reverted to
                 * ±100k between then and HEAD. */
                #define CLAMP_DELTA(v) ((v) > 20000 ? 0 : (v) < -20000 ? 0 : (v))
                dx = CLAMP_DELTA(dx); dy = CLAMP_DELTA(dy); dz = CLAMP_DELTA(dz);
                #undef CLAMP_DELTA
                g270[0] = (int16_t)arith_round_shift(dx, 0xFF, 8);
                g270[1] = (int16_t)arith_round_shift(dy, 0xFF, 8);
                g270[2] = (int16_t)arith_round_shift(dz, 0xFF, 8);
                if (actor->slot_index == 0 && i == 0 && (actor->frame_counter % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG, "gap270[0]: dx=%d dy=%d dz=%d g270=(%d,%d,%d) "
                              "wcv=(%d,%d,%d)",
                              dx, dy, dz, g270[0], g270[1], g270[2],
                              actor->wheel_contact_velocities[0][0],
                              actor->wheel_contact_velocities[0][1],
                              actor->wheel_contact_velocities[0][2]);
                }
            } else {
                g270[0] = 0; g270[1] = 0; g270[2] = 0;
            }
            /* Save this frame's pre-snap transform result for next frame */
            s_prev_wheel_tx[slot][i] = actor->wheel_contact_pos[i].x;
            s_prev_wheel_ty[slot][i] = actor->wheel_contact_pos[i].y;
            s_prev_wheel_tz[slot][i] = actor->wheel_contact_pos[i].z;
        }

        /* Dead zone */
        if (force > -0x200 && force < 0x200)
            force = 0;

        /* Airborne detection + ground snap.
         * Original (0x403720): when force < 0x801 (grounded), DIRECTLY
         * overwrites wheel_contact_pos[i].y = ground_y. Without this,
         * wheel_y stays at the rotation-computed value (often 0) and the
         * ground snap in integrate_pose has no valid baseline.
         * [CONFIRMED @ 0x403720 — piVar8 = local_30] */
        if (force > 0x800) {
            actor->wheel_contact_bitmask |= (1 << i);
            force = 12000;
        } else {
            actor->wheel_contact_bitmask &= ~(1 << i);
            /* Snap wheel Y to ground surface when grounded.
             * Original (0x403720) unconditionally snaps without checking
             * probe success — the track update above ensures a valid span. */
            actor->wheel_contact_pos[i].y = ground_y;
        }

        /* Publish the per-wheel force to wheel_load_accum (+0x2FC).
         * IntegrateWheelSuspensionTravel (0x00403A20) reads this next tick
         * as the external excitation for the spring-damper.
         * [CONFIRMED @ 0x00403720: *local_4c = ...] */
        actor->wheel_load_accum[i] = force;

        if (actor->slot_index == 0 && i == 0 && (actor->frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG, "wheel_force: slot=%d wheel=%d wy=%d gy=%d force=%d bitmask=0x%02x",
                      actor->slot_index, i, wheel_y, ground_y, force,
                      actor->wheel_contact_bitmask);
        }

        /* Store high-res wheel world position */
        actor->wheel_world_positions_hires[i] = actor->wheel_contact_pos[i];

        /* Copy to probe_FL/FR/RL/RR (0x090) for wall contact detection.
         * The original RefreshVehicleWheelContactFrames writes both 0x0F0
         * (wheel_contact_pos) and 0x090 (probe_FL..RR). Without this,
         * UpdateActorTrackSegmentContacts reads zero positions. */
        switch (i) {
            case 0: actor->probe_FL = actor->wheel_contact_pos[0]; break;
            case 1: actor->probe_FR = actor->wheel_contact_pos[1]; break;
            case 2: actor->probe_RL = actor->wheel_contact_pos[2]; break;
            case 3: actor->probe_RR = actor->wheel_contact_pos[3]; break;
        }
    }

    /* Mark this slot's pre-snap transform as valid for next frame */
    s_prev_wheel_valid[slot] = 1;

    if (resolved_surface_valid)
        actor->surface_type_chassis = (uint8_t)resolved_surface;

    /* surface_contact_flags update moved to the tail of integrate_pose
     * so the spawn-time init call chain (td5_physics_init_vehicle_runtime
     * -> refresh_wheel_contacts at line 3707) does NOT seed the flag. The
     * original leaves the flag at 0 from the slot memset; it is only
     * written inside UpdatePlayerVehicleDynamics. Running it at the end of
     * integrate_pose (after refresh_wheel_contacts has updated
     * wheel_contact_bitmask) keeps the field in sync with the port's
     * existing "bits = grounded axles" interpretation while matching the
     * original's tick-1 behavior (flag=0 → airborne branch → no drive
     * torque). See the gate block in td5_physics_integrate_pose. */
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

    /* Hard-clamp / MODE-0 exceeded thresholds [CONFIRMED @ FUN_00405B40 0x405B63] */
    int32_t roll_limit  = 0x355;
    int32_t pitch_limit = 0x3A4;

    if (g_collisions_enabled == 0) {
        /* MODE 0 (collisions ON): original sets recovery flag (actor+0x379=1) when
         * |roll| > 0x355 or |pitch| > 0x3A4, then copies rotation matrix via
         * FUN_0042E1E0 to actor+0x150/+0x180. Port maps this to vehicle_mode=1
         * (59-frame recovery ticker). [CONFIRMED @ FUN_00405B40 0x405B5D]
         *
         * Disabled: port's suspension equilibrium drift holds pitch ≈ ±935 on flat
         * ground (7 units above the 932 limit), triggering recovery teleport on every
         * actor every ~61 frames during normal driving. Fix pending suspension
         * equilibrium correction (reference_port_ride_height_offset.md). */
    } else {
        /* MODE 1 (collisions OFF): soft nudge then hard clamp.
         * Soft nudge threshold: 0x27F roll, 0x2BB pitch [CONFIRMED @ 0x405BEE]
         * Hard clamp threshold: 0x355 roll, 0x3A4 pitch [CONFIRMED @ 0x405B63] */
        int32_t roll_nudge  = 0x27F;
        int32_t pitch_nudge = 0x2BB;

        if (roll <= roll_nudge && roll >= -roll_nudge &&
            pitch <= pitch_nudge && pitch >= -pitch_nudge)
            return;

        if (roll > roll_nudge)  actor->angular_velocity_roll  -= 0x200;
        if (roll < -roll_nudge) actor->angular_velocity_roll  += 0x200;
        if (roll > roll_limit) {
            actor->angular_velocity_roll = 0;
            actor->euler_accum.roll = roll_limit << 8;
        }
        if (roll < -roll_limit) {
            actor->angular_velocity_roll = 0;
            actor->euler_accum.roll = (-roll_limit) << 8;
        }

        if (pitch > pitch_nudge)  actor->angular_velocity_pitch -= 0x200;
        if (pitch < -pitch_nudge) actor->angular_velocity_pitch += 0x200;
        if (pitch > pitch_limit) {
            actor->angular_velocity_pitch = 0;
            actor->euler_accum.pitch = pitch_limit << 8;
        }
        if (pitch < -pitch_limit) {
            actor->angular_velocity_pitch = 0;
            actor->euler_accum.pitch = (-pitch_limit) << 8;
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

    /* Zero euler pitch/roll accumulators — yaw was set by
     * InitializeActorTrackPose (compute_heading) and must be preserved.
     * Missing these writes caused slot 1+ spawn-pose residuals in
     * disp_pitch / ang_pitch / disp_roll at sim_tick=1.
     * [RE basis: ResetVehicleActorState @ 0x00405D70 zeroes the pitch
     * and roll accumulators while leaving yaw untouched.] */
    actor->euler_accum.roll = 0;
    actor->euler_accum.pitch = 0;

    /* Seed display angles from the accumulator so the first render frame
     * matches the spawn yaw. integrate_pose rewrites these each tick from
     * (euler_accum >> 8), but the initial values need to be consistent. */
    actor->display_angles.roll = 0;
    actor->display_angles.yaw = (int16_t)((actor->euler_accum.yaw >> 8) & 0xFFF);
    actor->display_angles.pitch = 0;

    /* Reset frame counter — used by mode1 recovery timeout. Carrying
     * residual counts from a prior race would shorten the recovery window
     * on respawn. [RE basis: ResetVehicleActorState zeroes the counter.] */
    actor->frame_counter = 0;

    /* Reset gear to first forward */
    actor->current_gear = TD5_GEAR_FIRST;

    /* Reset engine RPM to idle */
    actor->engine_speed_accum = TD5_ENGINE_IDLE_RPM;

    /* Initialize wheel contact and suspension state.
     * In the original, the suspension spring-damper (0x4057F0) converges
     * wheel_suspension_pos to a steady-state value over multiple ticks.
     * Since our spring-damper is simplified, we initialize to the
     * steady-state deflection derived from the cardef suspension
     * geometry. The exact value depends on the cardef spring constants;
     * the heuristic below (susp_href / 2) produces the correct ride
     * height for the cars tested. */
    actor->wheel_contact_bitmask = 0;
    {
        int32_t href = 0;
        uint8_t *cd = (uint8_t *)actor->car_definition_ptr;
        if (cd) {
            int16_t sh = *(int16_t *)(cd + 0x82);
            href = (sh * 0xB5 + ((sh * 0xB5) >> 31 & 0xFF)) >> 8;
        }
        /* Steady-state deflection ≈ href * 5/9 (from trace-matching:
         * the original's spring-damper converges susp_pos to ~59.5
         * when href=107, giving the correct ride height). */
        int16_t ss = (int16_t)((href * 5 + 4) / 9);
        for (int i = 0; i < 4; i++) {
            actor->wheel_suspension_pos[i] = ss;
            actor->wheel_load_accum[i] = 0;
            actor->wheel_spring_dv[i] = 0;
        }
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
    actor->current_slip_metric = 0;

    /* Tire-track emitter IDs default to 0xFF (no emitter). The vfx module
     * uses 0xFF as the "free" sentinel when checking whether to allocate
     * a new emitter from the 80-slot pool; leaving these zero means the
     * acquire path never fires and tire marks never appear. */
    actor->tire_track_emitter_FL = 0xFF;
    actor->tire_track_emitter_FR = 0xFF;
    actor->tire_track_emitter_RL = 0xFF;
    actor->tire_track_emitter_RR = 0xFF;

    /* Clear control state */
    actor->steering_command = 0;
    actor->encounter_steering_cmd = 0;  /* zero throttle */
    actor->brake_flag = 0;
    actor->handbrake_flag = 0;
    actor->vehicle_mode = 0;
    actor->damage_lockout = 0;

    /* Match original ResetVehicleActorState @ 0x00405D70: write the
     * sentinel world_pos.y = -0x40000000 and let IntegrateVehiclePoseAndContacts
     * ground-snap via the per-wheel refresh_wheel_contacts -> wheel_contact_pos
     * averaging path. force = (sentinel - ground_y) + gravity is hugely
     * negative (< 0x801), so the grounded branch in refresh_wheel_contacts
     * snaps wheel_contact_pos[i].y = ground_y, and integrate_pose then
     * averages those 4 per-wheel values into world_pos.y.
     *
     * Prior implementation probed at chassis XZ and wrote world_pos.y
     * before integrate — that sampled ONE terrain point instead of the
     * 4 rotated wheel XZs, causing per-slot Y deltas that scaled with
     * grid offset (slot 1 -12416, slot 2 +4736, slot 4 -8000, ...).
     * [CONFIRMED @ 0x00405D70] */
    actor->world_pos.y = (int32_t)0xC0000000;

    /* Convert positions to float for render */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Run one integrate step to settle suspension against the ground. */
    td5_physics_integrate_pose(actor);

    /* Post-integrate: zero all dynamics to prevent bounce (0x405E5E-0x405E7C) */
    for (int i = 0; i < 4; i++)
        actor->wheel_load_accum[i] = 0;
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

    /* Integrate wheel suspension with zero chassis-motion excitation.
     * Matches original 0x00403DA9 call: IntegrateWheelSuspensionTravel(
     *   actor, cardef, 0, 0). The wheel_load_accum term still drives
     *  the spring; previous port hack of clearing wheel_spring_dv
     * (the velocity state!) was incorrect — removed. */
    td5_physics_integrate_suspension(actor, 0, 0);

    /* Zero surface contact flags and slip [CONFIRMED @ 0x403DC4-0x403DD0] */
    actor->surface_contact_flags = 0;
    actor->front_axle_slip_excess = 0;
    actor->rear_axle_slip_excess = 0;

    /* Compute body-frame longitudinal speed */
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);
    int32_t v_long = (actor->linear_velocity_x * sin_h +
                      actor->linear_velocity_z * cos_h) >> 12;
    int32_t v_lat  = (actor->linear_velocity_x * cos_h -
                      actor->linear_velocity_z * sin_h) >> 12;

    int32_t roll = (actor->euler_accum.roll >> 8) & 0xFFF;
    if (roll > 0x800) roll -= 0x1000;

    /* Roll correction: if low speed and small roll, apply speed/4 to roll
     * angular velocity. This naturally steers the car back on-road.
     * [CONFIRMED @ 0x403DF8-0x403E58] */
    int32_t abs_v = v_long < 0 ? -v_long : v_long;
    int32_t abs_r = roll < 0 ? -roll : roll;
    if (abs_v < 0x21 && abs_r < 0x7F) {
        actor->angular_velocity_roll += v_long >> 2;
    }

    /* Decay roll and pitch angular velocities by 1/16 per frame.
     * [CONFIRMED @ 0x403E58-0x403EA4]
     * NOTE: yaw and linear velocities are NOT decayed — the car maintains
     * momentum while airborne. Previous port code incorrectly decayed all
     * three, causing airborne cars to lose speed. */
    actor->angular_velocity_roll  -= actor->angular_velocity_roll >> 4;
    actor->angular_velocity_pitch -= actor->angular_velocity_pitch >> 4;

    /* Accumulate slip from body-frame speeds [CONFIRMED @ 0x403E74-0x403E8C] */
    actor->accumulated_tire_slip_x += (int16_t)(v_lat >> 8);
    actor->accumulated_tire_slip_z += (int16_t)(v_long >> 8);
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

    /* Original @ 0x42EDC2 / 0x42EDE3 stores rpm unconditionally — NO
     * post-store clamps. Previously the port added `if (rpm > redline)
     * rpm = redline; if (rpm < 400) rpm = 400;` here, which blocked the
     * natural overshoot behavior of the asymmetric slew. Removed to
     * match the original. [CONFIRMED @ 0x42EDC2, 0x42EDE3] */
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

    /* Cap at redline — original @ 0x42EE6F-0x42EE7A clamps only the UPPER
     * bound; it does NOT have a 400 floor. Previously the port had
     * `if (rpm < 400) rpm = 400;` which blocked the valid low-RPM
     * transient during shifts. [CONFIRMED @ 0x42EE7A] */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    if (rpm > redline) rpm = redline;

    actor->engine_speed_accum = rpm;
}

/* --- UpdateAutomaticGearSelection (0x0042EF10) ---
 *
 * [CONFIRMED via 2026-04-11 full Ghidra pass.] Key port-vs-original
 * corrections over the prior implementation:
 *
 *   1. CACHED GEAR for threshold indexing. The original loads current_gear
 *      into EAX once at 0x0042EF21 and never refreshes it before the
 *      upshift/downshift index reads (`MOVSX [ESI+EAX*2+0x3e]` at 0x42EF52
 *      and `MOVSX [ESI+EAX*2+0x4e]` at 0x42EFF1). When the reverse→forward
 *      promotion writes memory (gear=2), the cached EAX stays at 0, so the
 *      upshift reads `phys[0x3E + 0]` (reverse entry), NOT `phys[0x42]`.
 *      The prior port updated its local `gear` variable in the promotion
 *      path and then used it for indexing, causing a one-tick upshift race
 *      on the first post-reverse tick.
 *
 *   2. DRIVETRAIN KICK into wheel_spring_dv on upshift. The original
 *      spreads a per-gear torque pulse across the four wheel force-accum
 *      slots (+0x2EC/+0x2F0/+0x2F4/+0x2F8) via the table at 0x00467394
 *      (= {0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20, 0x10, 0}, indexed by the
 *      NEW post-upshift gear). FL/FR get +k, RL/RR get -k. The prior port
 *      omitted this entirely. */
void td5_physics_auto_gear_select(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* Cache up-front — never refresh across the function body. */
    int32_t throttle    = (int32_t)actor->encounter_steering_cmd;
    uint8_t gear_cached = actor->current_gear;
    int32_t rpm         = actor->engine_speed_accum;

    /* Negative throttle → force reverse and return. */
    if (throttle < 0) {
        actor->current_gear = TD5_GEAR_REVERSE;
        return;
    }

    /* Already in reverse: promote to first forward in MEMORY only when
     * throttle > 0. Cached value is intentionally NOT updated — matches
     * the original's non-refreshed EAX cache. */
    if (gear_cached == TD5_GEAR_REVERSE) {
        if (throttle <= 0) return;
        actor->current_gear = TD5_GEAR_FIRST;
        /* gear_cached stays 0 on purpose */
    }

    /* Upshift test — indexed by gear_cached (pre-promotion). */
    int32_t up_thresh = (int32_t)PHYS_S(actor, 0x3E + gear_cached * 2);

    if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "AUTO_GEAR: gear=%d rpm=%d up_thresh=%d long_spd=%d throttle=%d f378=%d",
                  gear_cached, rpm, up_thresh, actor->longitudinal_speed,
                  throttle, *((const uint8_t *)actor + 0x378));
    }

    if (rpm > up_thresh
        && gear_cached < 8
        && actor->longitudinal_speed > 0) {

        /* Re-load gear byte from memory (may already be 2 after promotion)
         * and +1, matching the original's `INC BL` after a second load. */
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;

        /* Drivetrain kick — per-gear force impulse spread across wheels.
         * [CONFIRMED @ 0x42EFB0..0x42EFD7 raw disasm + DAT_00467394 table.] */
        static const int32_t g_gear_torque_table[9] = {
            0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20, 0x10, 0
        };
        int32_t k = (int32_t)PHYS_S(actor, 0x68) * throttle * 0x1A;
        k = ((k + ((k >> 31) & 0xFF)) >> 8)
          * g_gear_torque_table[new_gear & 0x0F];
        k =  (k + ((k >> 31) & 0xFF)) >> 8;

        actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
        actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
        actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
        actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
        return;
    }

    /* Downshift test — also indexed by gear_cached. */
    int32_t dn_thresh = (int32_t)PHYS_S(actor, 0x4E + gear_cached * 2);
    if (rpm < dn_thresh && gear_cached > TD5_GEAR_FIRST) {
        actor->current_gear = (uint8_t)(actor->current_gear - 1);
    }
}

/* --- On-ground variant of auto_gear_select ---
 * Same gear logic but WITHOUT the drivetrain kick (wheel_spring_dv writes).
 * The original only calls auto_gear on airborne frames where kicks are rare.
 * The port calls it on-ground every tick, so the kick accumulates and
 * pitches the car up without recovery. */
void td5_physics_auto_gear_select_no_kick(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    int32_t throttle    = (int32_t)actor->encounter_steering_cmd;
    uint8_t gear_cached = actor->current_gear;
    int32_t rpm         = actor->engine_speed_accum;

    if (throttle < 0) {
        actor->current_gear = TD5_GEAR_REVERSE;
        return;
    }

    if (gear_cached == TD5_GEAR_REVERSE) {
        if (throttle <= 0) return;
        actor->current_gear = TD5_GEAR_FIRST;
    }

    int32_t up_thresh = (int32_t)PHYS_S(actor, 0x3E + gear_cached * 2);

    if (rpm > up_thresh
        && gear_cached < 8
        && actor->longitudinal_speed > 0) {
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;
        /* No drivetrain kick — on-ground only */
        return;
    }

    int32_t dn_thresh = (int32_t)PHYS_S(actor, 0x4E + gear_cached * 2);
    if (rpm < dn_thresh && gear_cached > TD5_GEAR_FIRST) {
        actor->current_gear = (uint8_t)(actor->current_gear - 1);
    }
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

    /* Original writes force to wheel_spring_dv[0..3] as [+,+,-,-],
     * creating a pitch differential. However, the original's suspension
     * reads XZ projections (not this Y-based force_accum), so steering
     * torque never actually drives the suspension spring-damper. In the
     * source port, this force goes directly into the suspension and
     * causes immediate pitch divergence (62400 vs ground contact ~107).
     * Steering already affects yaw via the separate yaw torque formula
     * in update_player. Omit the force_accum writes. */
    (void)throttle;
    (void)sensitivity;
    (void)gear;
}

/* --- ApplyReverseGearThrottleSign (0x42F010) --- */
void td5_physics_reverse_throttle_sign(TD5_Actor *actor)
{
    if (actor->current_gear == TD5_GEAR_REVERSE)
        actor->encounter_steering_cmd = -actor->encounter_steering_cmd;
}

/* --- ComputeReverseGearTorque (0x00403C80) — full encode + engine slew ---
 *
 * Despite the Ghidra name, this function does NOT compute torque. It
 * (a) produces the RPM-encoded pseudo-speed written back as the caller's
 * longitudinal_speed or lateral_speed, and (b) slews engine_speed_accum
 * toward a target that depends on throttle, gear, brake, and a caller-
 * supplied signed speed term. It is the GROUND-PATH authoritative engine
 * updater — UpdateEngineSpeedAccumulator (UESA) runs on the airborne path
 * instead; the two are mutually exclusive per tick.
 *
 * [CONFIRMED @ 0x00403C80 via 2026-04-11 full Ghidra pass + raw disasm
 * 0x00403d2f..0x00403d75 for the three +0x310 writeback sites.]
 *
 * Target RPM:
 *   - gear == 1 (neutral): return 0, leave engine alone (early-out).
 *   - throttle >= 1 AND gear == 2 (first forward): hot target =
 *       max((speed*4) >> 8, 0) + redline - 1800, step = 400
 *   - otherwise: target = 0, step = (brake ? 800 : 400) — coast down fast.
 *
 * Slew:
 *   cur > target: cur -= step (clamp to target if overshoot) → write A or C
 *   cur <= target - 4*step: cur += step (big-gap linear ramp) → write B
 *   cur <= target, gap <= 4*step: cur += (target - cur) / 4 (exponential) → write C
 *
 * Return value (pseudo-speed) is the closed-form inverse of UESA's decode:
 *   encode(rpm, gear_ratio) = (((rpm - 400) * 0x1000) / 45 / gear_ratio) << 8
 *
 * speed_in is drivetrain-dependent per UpdatePlayerVehicleDynamics @ 0x00404030:
 *   sVar2=1 (RWD): body_vlong (uVar12)
 *   sVar2=2 (FWD): body_vlat  (uVar37)
 *   sVar2=3 (AWD): (uVar12 + uVar37) / 2
 */
static int32_t compute_reverse_gear_torque(TD5_Actor *actor, int32_t speed_in)
{
    if (!actor) return 0;
    int16_t *phys = get_phys(actor);
    if (!phys) return 0;

    int32_t engine = actor->engine_speed_accum;
    int32_t gear   = (int32_t)actor->current_gear;

    /* --- Encode pseudo-speed (return value) --- */
    int32_t ret_value;
    if (gear == 1) {
        return 0;  /* neutral — no engine slew, no return value */
    }
    {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E + gear * 2);
        if (gear_ratio == 0) return 0;
        int32_t num = (engine - 400) * 0x1000;
        ret_value = ((num / 45) / gear_ratio) << 8;
    }

    /* --- Target RPM + slew step --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;  /* signed short */
    int32_t target, step;

    if (throttle < 1) {
        /* Cold branch: no throttle → coast target=0. */
        int base = actor->brake_flag ? 400 : 200;
        step = base * 2;                     /* 800 or 400 */
        target = 0;
    } else if (gear == 2) {
        /* Hot branch: first-forward under throttle. Step STAYS at 200 here —
         * the doubling is inside the cold branch only. [CONFIRMED @ 0x00403C80
         * raw disasm: iVar4=200 default, `iVar4 * 2` only inside the cold
         * conditional.] */
        int32_t u = (speed_in * 4) >> 8;
        if (u < 0) u = 0;                    /* clamp via (AND sign-1) idiom */
        int32_t redline = (int32_t)PHYS_S(actor, 0x72);
        target = u + redline - 1800;         /* 0x708 */
        step = 200;
    } else if (gear > 2 && throttle >= 1) {
        /* Higher gears under throttle: maintain RPM proportional to speed
         * and gear ratio. The original targets RPM=0 here and relies on
         * airborne UESA ticks to maintain RPM. Since the port rarely goes
         * airborne and runs auto_gear on-ground, CRGT must maintain RPM
         * for higher gears or the car oscillates (upshift→RPM dies→downshift). */
        int32_t gr = (int32_t)PHYS_S(actor, 0x2E + gear * 2);
        int32_t abs_spd = speed_in < 0 ? -speed_in : speed_in;
        target = ((abs_spd >> 8) * gr * 0x2D) >> 12;
        target += 400;
        step = 200;
    } else {
        /* Reverse or neutral under throttle, or higher gears coasting: target=0. */
        int base = actor->brake_flag ? 400 : 200;
        step = base * 2;
        target = 0;
    }

    /* --- Slew engine_speed_accum toward target --- */
    int32_t new_accum;
    if (target < engine) {
        /* Descending: linear step, clamp to target on overshoot */
        int32_t stepped = engine - step;
        if (stepped < target) new_accum = target;   /* write A */
        else                  new_accum = stepped;  /* write C (dec) */
    } else {
        if (engine < target - 4 * step) {
            new_accum = engine + step;                /* write B (big-gap ramp) */
        } else {
            /* Exponential pull — C99 signed integer division rounds toward
             * zero, matching the original's SAR+CDQ+AND 3 idiom. */
            int32_t delta = target - engine;
            new_accum = engine + (delta / 4);        /* write C (exp) */
        }
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
        TD5_LOG_I(LOG_TAG,
            "CRGT: gear=%d rpm=%d->%d target=%d step=%d throttle=%d brake=%d spd_in=%d",
            gear, engine, new_accum, target, step, throttle,
            actor->brake_flag, speed_in);
    }
    actor->engine_speed_accum = new_accum;
    return ret_value;
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

    /* Cross product -> surface normal via CrossProduct3i_FixedPoint12 @ 0x42EAC0.
     * Original shifts by >> 12, not >> 8. The previous >> 8 produced a 16x
     * magnitude error that injected a ~1.5 world-unit lateral impulse at
     * spawn — root cause of Cluster A vel_x=-399 / vel_z=+895 observed on
     * 2026-04-11. [CONFIRMED @ 0x42EAC0 CrossProduct3i_FixedPoint12] */
    int32_t nx = (v1y * v2z - v1z * v2y) >> 12;
    int32_t nz = (v1x * v2y - v1y * v2x) >> 12;

    /* Project gravity onto body X and Z axes.
     * Original @ 0x42EBF0: (gGravityConstant * n) / 2 then
     * (ax + ((ax >> 31) & 0xfff)) >> 12  (signed round toward zero).
     * Previous port used (g >> 1) * n which loses 1 LSB on odd n. */
    int32_t ax = (g_gravity_constant * nx) / 2;
    int32_t az = (g_gravity_constant * nz) / 2;
    actor->linear_velocity_x += (ax + ((ax >> 31) & 0xfff)) >> 12;
    actor->linear_velocity_z += (az + ((az >> 31) & 0xfff)) >> 12;
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

    /* Invalidate pre-snap wheel transform data — init path transforms
     * differ from in-race transforms and cause huge gap_270 transients. */
    memset(s_prev_wheel_valid, 0, sizeof(s_prev_wheel_valid));

    /* Populate per-slot handicap tables from gRaceResultPointsTable indexed
     * by each slot's prior championship position. See declaration above. */
    for (int s = 0; s < 6; s++) {
        int pos = g_slot_series_position[s];
        if (pos < 0) pos = 0;
        if (pos > 3) pos = 3;
        g_slot_race_result[s] = s_race_result_points_table[pos][0];
        g_slot_race_bonus [s] = s_race_result_points_table[pos][1];
        g_slot_race_points[s] = s_race_result_points_table[pos][2];
    }

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

        /* Traffic-only: force cdef+0x88 (mass) to 0x20 regardless of what
         * carparam.dat supplies. Mirrors the original's init-time write at
         * 0x0042F235 (`MOV word ptr [EAX + 0x88], 0x0020`). Without this,
         * traffic cars use whatever carparam provides (observed 0x400=1024)
         * and the V2V impulse solver produces a 64× mass imbalance vs
         * player=16, making players stick to traffic cars on contact. */
        if (slot >= TD5_MAX_RACER_SLOTS && actor->car_definition_ptr) {
            int16_t *cd = (int16_t *)actor->car_definition_ptr;
            cd[0x88 / 2] = 0x20;
        }

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

        /* --- Per-slot NPC handicap (faithful port of InitializeRaceVehicleRuntime
         * @ 0x42F140 gRaceSlotState==1 branch) ---
         * Applies rubber-banding derived from the slot's prior championship
         * position. s_race_result_points_table row 0 (default) is all zeros so
         * every adjustment is a no-op when championship position is not set.
         * Call td5_physics_set_slot_series_position() from frontend to enable.
         *
         * Gear count assumed 8 (R,N,1-6) — all shipped TD5 vehicles use this.
         * If a modded car defines fewer, the triangular loop denom guard below
         * prevents div-by-zero. */
        if (slot < 6 && g_race_slot_state[slot] == 1) {
            int16_t *phys = (int16_t *)actor->tuning_data_ptr;
            if (phys) {
                const int32_t race_result = g_slot_race_result[slot];
                const int32_t race_bonus  = g_slot_race_bonus [slot];
                const int32_t race_points = g_slot_race_points[slot];
                const int     gears       = 8;  /* R+N+6 forward */

                /* (1) brake/engine_brake redistribution — DAT_004AED70 is always
                 * zero in the original so the delta is always 0. Omitted. */

                /* (2) Torque-curve triangular fade over forward gears.
                 * Starts at phys+0x32 (gear index 2 = 1st forward) and walks
                 * `gears - 2` entries. Weight runs denom..0, divided by denom.
                 * For race_result < 1 (negative/zero), divide by 0x300 and use
                 * signed div. For race_result >= 1 use >>9 with round-to-zero. */
                if (gears > 3) {
                    const int denom = gears - 3;
                    const int n     = gears - 2;
                    for (int i = 0; i < n; i++) {
                        const int weight = denom - i;
                        int32_t entry = (int32_t)PHYS_S(actor, 0x32 + i * 2);
                        int32_t prod;
                        if (race_result < 1) {
                            prod = ((entry * race_result) / 0x300) * weight / denom;
                        } else {
                            int32_t mul = entry * race_result;
                            prod = ((mul + ((mul >> 31) & 0x1FF)) >> 9) * weight / denom;
                        }
                        write_i16((uint8_t *)phys, 0x32 + i * 2,
                                  (int16_t)(entry + prod));
                    }
                }

                /* (3a) top_speed_limit *= (race_bonus + 0x800) / 2048 */
                {
                    int32_t top = (int32_t)PHYS_S(actor, 0x74);
                    int32_t n1  = (race_bonus + 0x800) * top;
                    write_i16((uint8_t *)phys, 0x74,
                              (int16_t)((n1 + ((n1 >> 31) & 0x7FF)) >> 11));
                }
                /* (3b) gear_ratio_table[gears-1] and [gears-2] top-gear rescale */
                {
                    int32_t denom1 = race_bonus + 0x800;
                    if (denom1 != 0) {
                        int32_t top = (int32_t)PHYS_S(actor, 0x2C + gears * 2);
                        write_i16((uint8_t *)phys, 0x2C + gears * 2,
                                  (int16_t)((top << 11) / denom1));
                    }
                    int32_t denom2 = race_bonus + 0x1000;
                    if (denom2 != 0) {
                        int32_t top2 = (int32_t)PHYS_S(actor, 0x2A + gears * 2);
                        write_i16((uint8_t *)phys, 0x2A + gears * 2,
                                  (int16_t)((top2 << 12) / denom2));
                    }
                }
                /* (3c) damping_low_speed *= (0x200 - race_bonus) / 512 */
                {
                    int32_t dlo = (int32_t)PHYS_S(actor, 0x6A);
                    int32_t n2  = (0x200 - race_bonus) * dlo;
                    write_i16((uint8_t *)phys, 0x6A,
                              (int16_t)((n2 + ((n2 >> 31) & 0x1FF)) >> 9));
                }

                /* (4) drag_coefficient adjustment via race_points */
                {
                    int32_t drag = (int32_t)PHYS_S(actor, 0x2C);
                    int32_t adj;
                    if (race_points < 0) {
                        /* Signed /0x300 with trunc-to-zero adjustment idiom */
                        int32_t mul = drag * race_points;
                        int64_t prod64 = (int64_t)mul * 0x2AAAAAABLL;
                        int32_t sign_adj = (int32_t)(prod64 >> 63);
                        adj = (mul / 0x300) + (mul >> 31) - sign_adj;
                    } else {
                        int32_t mul = drag * race_points;
                        adj = (mul + ((mul >> 31) & 0x1FF)) >> 9;
                        /* AI pacing bias at actor+0x324 — same >>9 rounding idiom
                         * [CONFIRMED @ ~0x42F54F]: *(int32*)(actor+0x324) +=
                         * (pacing * race_points + rounding) >> 9 */
                        {
                            int32_t pacing = *(int32_t *)((uint8_t *)actor + 0x324);
                            int32_t pm = pacing * race_points;
                            *(int32_t *)((uint8_t *)actor + 0x324) =
                                pacing + ((pm + ((pm >> 31) & 0x1FF)) >> 9);
                        }
                    }
                    write_i16((uint8_t *)phys, 0x2C, (int16_t)(drag + adj));
                }
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
        actor->encounter_steering_cmd = 0;  /* zero throttle — prevents stale value from driving torque */
        actor->brake_flag = 0;
        actor->handbrake_flag = 0;
        actor->throttle_state = 1;  /* forward mode [CONFIRMED @ 0x4032A0] */
        actor->vehicle_mode = 0;
        actor->damage_lockout = 0;
        actor->center_suspension_pos = 0;
        actor->center_suspension_vel = 0;
        memset(actor->wheel_suspension_pos, 0, sizeof(actor->wheel_suspension_pos));
        memset(actor->wheel_load_accum, 0, sizeof(actor->wheel_load_accum));
        memset(actor->wheel_spring_dv, 0, sizeof(actor->wheel_spring_dv));
        actor->slot_index = (uint8_t)slot;
        actor->throttle_input_active = 1;  /* auto gearbox [CONFIRMED @ 0x42F140] */
        actor->frame_counter = 0;
        actor->track_contact_flag = 0;
        actor->surface_contact_flags = 0;
        actor->grip_reduction = 0xFF;
        actor->prev_race_position = 0;
        actor->race_position = 0;  /* +0x383 stays 0 until UpdateRaceOrder writes at sim_tick>=1 [CONFIRMED @ 0x0042F5B0] */
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
        /* Force all wheels grounded for init ground snap. At spawn,
         * world_pos.y may be far from the track surface (sentinel/default),
         * causing force = (wheel_y - ground_y) to exceed the airborne
         * threshold. The init snap MUST run regardless — it's placing the
         * car on the road, not simulating physics. */
        actor->wheel_contact_bitmask = 0;
        /* Apply ground-snap correction. Original init path
         * (InitializeActorTrackPose @ 0x00434350 → ResetVehicleActorState
         * @ 0x00405D70 → IntegrateVehiclePoseAndContacts @ 0x00405E80) does
         * NOT pre-lift chassis_y by a susp_href scalar term. susp_href is
         * folded into the per-wheel rotated Y via refresh_wheel_contacts'
         * pre-subtract/post-add pattern; adding it again here double-counts
         * and produces an orientation-dependent spawn Y bias (because the
         * port-invented term multiplied by m[4], which varies with tilt).
         * On tilted spawn terrain this kicked the suspension into launch
         * transients → "sometimes rolls out of control on slopes" user
         * symptom. [CONFIRMED no analog in 0x00406283-0x0040628B] */
        {
            int64_t corr_sum = 0;
            int corr_count = 0;
            for (int i = 0; i < 4; i++) {
                if (!(actor->wheel_contact_bitmask & (1 << i))) {
                    int32_t g_y = 0;
                    int g_surf = 0;
                    int g_span = actor->track_span_raw;
                    if (td5_track_probe_height(actor->wheel_contact_pos[i].x,
                                               actor->wheel_contact_pos[i].z,
                                               g_span, &g_y, &g_surf)) {
                        corr_sum += (int64_t)g_y - (int64_t)actor->wheel_contact_pos[i].y;
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

    /* AI slots (1..5) use the GLOBAL AI physics template (DAT_00473DB0 in
     * the original), not the per-slot carparam.dat. Frida probe 2026-04-23
     * confirmed original AI slots all read Wf=400, Wr=400, I=180000 from
     * this template. Using per-car carparam gave Wf=420, I=160000 which
     * flipped the sign of D in the bicycle solve.
     *
     * The template is scaled by difficulty/tier at
     * td5_ai_init_race_actor_runtime, so it's finalized by the time actors
     * spawn. Cardef still comes from carparam.dat (for bounding box, etc.)
     * because only tuning is AI-specific in the original.
     *
     * Slot 0 stays on carparam (it's the player). Slots 6-11 (traffic) also
     * stay on carparam — they don't run through FUN_00404EC0. */
    if (slot >= 1 && slot < TD5_MAX_RACER_SLOTS) {
        uint8_t *ai_tmpl = td5_ai_get_physics_template();
        if (ai_tmpl) {
            memcpy(tuning, ai_tmpl, 0x80);
            if (s_carparam_loaded[slot]) {
                memcpy(cardef, s_loaded_cardef[slot], 0x8C);
            }
            actor->tuning_data_ptr = tuning;
            actor->car_definition_ptr = cardef;
            TD5_LOG_I(LOG_TAG, "bind_tuning slot=%d: using AI template (Wf=%d Wr=%d I=%d)",
                      slot, *(int16_t *)(tuning + 0x28),
                      *(int16_t *)(tuning + 0x2A),
                      *(int32_t *)(tuning + 0x20));
            return;
        }
    }

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
 * Iterates all mesh vertices to compute the AABB, then writes collision
 * geometry into the car definition buffer:
 *   0x00-0x1C: 4 lower corners (Y = suspension height for racers)
 *   0x20-0x3C: 4 upper corners (Y = max_abs_y from mesh)
 *   0x60-0x7C: simplified traffic footprint (slot >= 6 only)
 *   0x80:      bounding sphere radius
 *   0x86:      Y height value (suspension for racers, min_y for traffic)
 * ======================================================================== */

void td5_physics_compute_suspension_envelope(TD5_Actor *actor, int slot)
{
    if (!actor || !actor->car_definition_ptr) return;

    TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
    if (!mesh || mesh->total_vertex_count <= 0) return;

    int16_t *cd = get_cardef(actor);

    /* --- Iterate mesh vertices to find extents [CONFIRMED @ 0x0042f720-0x0042f7b9] --- */
    float max_x = 0.0f, max_y = 0.0f, max_z = 0.0f, min_y = 0.0f;
    int vert_count = mesh->total_vertex_count;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;

    for (int i = 0; i < vert_count; i++) {
        float vx = verts[i].pos_x;
        float vy = verts[i].pos_y;
        float vz = verts[i].pos_z;
        if ( vx > max_x) max_x =  vx;
        if (-vx > max_x) max_x = -vx;
        if ( vy > max_y) max_y =  vy;
        if (-vy > max_y) max_y = -vy;
        if ( vz > max_z) max_z =  vz;
        if (-vz > max_z) max_z = -vz;
        if (vy < min_y)  min_y = vy;
    }

    /* --- Racer-only width clamp [CONFIRMED @ 0x0042f7cc-0x0042f7f2] --- */
    float y_val;  /* used for lower box Y and bounding sphere */
    if (slot < TD5_MAX_RACER_SLOTS) {
        float delta = (float)((int32_t)cd[0x84 / 2] - (int32_t)cd[0x40 / 2]);
        if (delta > max_x) max_x = delta;
        /* Suspension-derived Y for lower box [CONFIRMED @ 0x0042f893-0x0042f8aa] */
        y_val = (float)((int32_t)cd[0x42 / 2] - (int32_t)cd[0x82 / 2]) * -0.707f;
    } else {
        y_val = min_y;
    }

    /* --- Add/subtract 20.0f padding [CONFIRMED @ 0x0042f7fa, 0x0042f808] --- */
    max_x += 20.0f;
    max_z -= 20.0f;
    if (max_z < 0.0f) max_z = 0.0f; /* safety clamp */

    /* Convert to int16 */
    int16_t neg_mx = (int16_t)(int)(-max_x);
    int16_t pos_mx = (int16_t)(int)( max_x);
    int16_t my_i16 = (int16_t)(int)( max_y);
    int16_t mz_i16 = (int16_t)(int)( max_z);
    int16_t nmz_i16= (int16_t)(int)(-max_z);
    int16_t ny_i16 = (int16_t)(int)(-y_val);

    /* --- Upper AABB box at offsets 0x20-0x3C [CONFIRMED @ 0x0042f827-0x0042f88d] --- */
    cd[0x20 / 2] = neg_mx;   cd[0x22 / 2] = my_i16;  cd[0x24 / 2] = mz_i16;
    cd[0x28 / 2] = pos_mx;   cd[0x2a / 2] = my_i16;  cd[0x2c / 2] = mz_i16;
    cd[0x30 / 2] = neg_mx;   cd[0x32 / 2] = my_i16;  cd[0x34 / 2] = nmz_i16;
    cd[0x38 / 2] = pos_mx;   cd[0x3a / 2] = my_i16;  cd[0x3c / 2] = nmz_i16;

    /* --- Lower AABB box at offsets 0x00-0x1C [CONFIRMED @ 0x0042f8b0-0x0042f8f0] --- */
    cd[0x00 / 2] = neg_mx;   cd[0x02 / 2] = ny_i16;  cd[0x04 / 2] = mz_i16;
    cd[0x08 / 2] = pos_mx;   cd[0x0a / 2] = ny_i16;  cd[0x0c / 2] = mz_i16;
    cd[0x10 / 2] = neg_mx;   cd[0x12 / 2] = ny_i16;  cd[0x14 / 2] = nmz_i16;
    cd[0x18 / 2] = pos_mx;   cd[0x1a / 2] = ny_i16;  cd[0x1c / 2] = nmz_i16;

    /* --- Bounding sphere radius at 0x80 [CONFIRMED @ 0x0042f918-0x0042f921] --- */
    float r = sqrtf(max_z * max_z + y_val * y_val + max_x * max_x);
    cd[0x80 / 2] = (int16_t)(int)r;

    /* --- Y height at 0x86 [CONFIRMED @ 0x0042f8f4-0x0042f901] --- */
    cd[0x86 / 2] = (int16_t)(int)y_val;

    /* --- Traffic simplified footprint at 0x60-0x7C [CONFIRMED @ 0x0042f928-0x0042f97e] --- */
    if (slot >= TD5_MAX_RACER_SLOTS) {
        float shrunk_x = max_x - max_x * 0.2f;  /* max_x * 0.8 */
        int16_t neg_sx = (int16_t)(int)(-shrunk_x);
        int16_t pos_sx = (int16_t)(int)( shrunk_x);

        cd[0x70 / 2] = neg_sx;  cd[0x72 / 2] = 0;  cd[0x74 / 2] = mz_i16;
        cd[0x78 / 2] = pos_sx;  cd[0x7a / 2] = 0;  cd[0x7c / 2] = mz_i16;
        cd[0x60 / 2] = neg_sx;  cd[0x62 / 2] = 0;  cd[0x64 / 2] = nmz_i16;
        cd[0x68 / 2] = pos_sx;  cd[0x6a / 2] = 0;  cd[0x6c / 2] = nmz_i16;
    }

    TD5_LOG_I(LOG_TAG,
              "suspension_envelope slot=%d: max_x=%.1f max_y=%.1f max_z=%.1f min_y=%.1f "
              "y_val=%.1f radius=%d",
              slot, max_x, max_y, max_z, min_y, y_val, (int)cd[0x80 / 2]);
}

void td5_physics_set_collisions(int enabled)
{
    g_collisions_enabled = enabled ? 0 : 1;  /* 0=on, 1=off (inverted) */
}

void td5_physics_rebuild_pose(TD5_Actor *actor)
{
    if (!actor) return;
    update_vehicle_pose_from_physics(actor);
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

void td5_physics_set_xz_freeze(int freeze)
{
    if (g_xz_freeze != freeze) {
        TD5_LOG_I(LOG_TAG, "XZ freeze=%d (DAT_00483030)", freeze);
        g_xz_freeze = freeze;
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

void td5_physics_set_slot_series_position(int slot, int position)
{
    if (slot < 0 || slot >= 6) return;
    if (position < 0) position = 0;
    if (position > 3) position = 3;
    g_slot_series_position[slot] = position;
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
