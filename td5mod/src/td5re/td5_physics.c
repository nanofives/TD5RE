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
#include "td5_game.h"     /* td5_game_get_total_actor_count, td5_game_is_wanted_mode */
#include "td5_platform.h"
#include "td5_trace.h"    /* inner-tick physics_trace stages */
#include "td5_pilot_trace.h" /* precise-port pilot CSV emit for 0x00403720 */
#include "td5_pilot_trace_00405B40.h" /* precise-port pilot CSV emit for 0x00405B40 */
#include "td5_pilot_trace_00405D70.h" /* precise-port pilot CSV emit for 0x00405D70 */
#include "td5_pilot_trace_00405E80.h" /* precise-port pilot CSV emit for 0x00405E80 */
#include "td5_pilot_trace_004063A0.h" /* precise-port pilot CSV emit for 0x004063A0 */
#include "td5_pilot_trace_00406650.h" /* precise-port pilot CSV emit for 0x00406650 */
#include "td5_pilot_trace_00406980.h"  /* precise-port pilot trace */
#ifdef TD5_PILOT_TRACE_00409150
#include "td5_pilot_trace_00409150.h"
#endif
#include "td5_pilot_trace_0042EB10.h"  /* precise-port pilot trace */
#include "td5_pilot_trace_0042EBF0.h" /* precise-port pilot CSV emit for 0x0042EBF0 */
#ifdef TD5_PILOT_TRACE_TRAFFIC
#include "td5_pilot_trace_traffic.h" /* precise-port pilot CSV emit for 0x004437C0 + 0x004438F0 */
#include "td5_pilot_trace_v2v_contact.h" /* pool15 V2V pilot trace */
#include "td5_pilot_trace_v2v.h"  /* pool14_v2v precise-port pilot */
#endif
#include "td5re.h"

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */
#include <stdlib.h>  /* abs */

#define LOG_TAG "physics"

extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

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
                            int32_t pos_a_x_fp, int32_t pos_a_z_fp,
                            int32_t pos_b_x_fp, int32_t pos_b_z_fp,
                            int32_t yaw_a_acc, int32_t yaw_b_acc,
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

/* Pilot-trace accessor — exposes g_gravity_constant to the pilot
 * trace module without leaking the static. Read-only debug use. */
int32_t td5_physics_dbg_get_gravity_constant(void) {
    return g_gravity_constant;
}
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

/* Per-slot rotated wheel-Y world offset, captured INSIDE
 * td5_physics_refresh_wheel_contacts at the same moment wheel_contact_pos[i].y
 * is computed (pre-snap). Consumed by the chassis ground-snap tail in
 * update_vehicle_pose_from_physics to mirror the original's
 *   local_10 = sum(wheel_probe_y - rotated_wheel_y_offset) / count
 * form @ 0x004063A0 — replacing a port-invented re-probe loop that called
 * td5_track_probe_height(x, z, actor->track_span_raw, ...) for every
 * grounded wheel. The re-probe used the chassis span for all wheels and
 * a heuristic lane picker; with the per-wheel walker fixed (single-step,
 * 2026-05-01) the wheel_contact_pos values are already correct, so the
 * faithful sum form gives a stable chassis Y on slope onsets. Replacing
 * the re-probe alone (without the walker fix) made the launch worse
 * because the walker was over-advancing wheels and feeding bad ground_y. */
static int32_t s_wheel_offset_y_world[16][4];

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
/* ARCHITECTURAL DIVERGENCE — see memory/reference_arch_cardef_per_actor_indirection.md
 * Original binary stores cardef bytes in a single global table:
 *   gVehicleTuningTable @ DAT_004AE580, indexed by slot*0x8C.
 * Original cardef readers compute &gVehicleTuningTable[slot*0x8C] directly
 * (e.g. ComputeVehicleSuspensionEnvelope @ 0x0042F6D0 uses ESI=that address).
 *
 * Port instead stores the bytes here in `s_loaded_cardef[slot]` (file scope),
 * memcpy's them into a per-actor buffer pointed to by `actor->car_definition_ptr`
 * at race init (bind_default_vehicle_tuning below), and every cardef reader
 * dereferences `actor->car_definition_ptr` instead of computing slot offsets.
 *
 * Bytes are byte-faithful; the divergence is purely the addressing scheme.
 * Cardef writers/readers in this file (and the one render-side reader in
 * td5_render.c) must stay consistent with the per-actor-pointer convention.
 * Fixing requires editing every cardef reader across the codebase.
 *
 * Commits 9da6c15 (precise-0042F140 InitializeRaceVehicleRuntime) and
 * cec14b6 (precise-0042F6D0 ComputeVehicleSuspensionEnvelope) both call out
 * this mapping in their headers. */
static uint8_t s_loaded_cardef[TD5_MAX_TOTAL_ACTORS][0x8C]; /* carparam 0x00..0x8B; row maps to gVehicleTuningTable[slot] */
static uint8_t s_loaded_tuning[TD5_MAX_TOTAL_ACTORS][0x80]; /* carparam 0x8C..0x10B; row maps to gVehiclePhysicsTable[slot] */

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
    /* Pilot trace: capture args + pre-state. Frida probe at
     * tools/frida_pool7_00406980.js mirrors this schema. Output:
     * log/port/pool7_00406980.csv. The `side` parameter is the port's
     * encoding of the original's `flags` arg (side=-1 maps to flags=0;
     * side=1/2 maps to flags=1/2). probe_x_fp8/probe_z_fp8 stand in for
     * the original's `forceVec[0]/forceVec[2]`. */
    {
        uint32_t flags_for_trace = (side < 0) ? 0u : (uint32_t)(side);
        td5_pilot_emit_00406980_enter(actor,
                                       probe_x_fp8, 0, probe_z_fp8,
                                       (uint32_t)wall_angle,
                                       penetration,
                                       flags_for_trace);
    }

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
     * because actor->world_pos was already updated above.
     *
     * Original uses Borland-style round-toward-zero on the >>12: adds 0xFFF
     * to negative values before SAR (asm: `CDQ; AND EDX,0xFFF; ADD EAX,EDX;
     * SAR EAX,0xC`). GCC's plain `>> 12` rounds toward -inf for negatives,
     * producing 1-unit-too-negative results. D4 audit. */
    int32_t arm_x_int = (actor->world_pos.x - probe_x_fp8) >> 8;
    int32_t arm_z_int = (actor->world_pos.z - probe_z_fp8) >> 8;
    int64_t iVar9_raw = (int64_t)arm_z_int * sin_w + (int64_t)arm_x_int * cos_w;
    int32_t iVar9 = (int32_t)((iVar9_raw + ((iVar9_raw >> 63) & 0xFFF)) >> 12);

    /* Decompose velocity into wall-tangent (v_para, iVar4) and
     * wall-normal (v_perp, iVar10) components [CONFIRMED @ 0x4069cc].
     * Same round-toward-zero correction on the >>12 (D2/D3/D4/D6 family). */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;
    int64_t v_para_raw = (int64_t)vx * cos_w + (int64_t)vz * sin_w;
    int64_t v_perp_raw = (int64_t)vz * cos_w - (int64_t)vx * sin_w;
    int32_t v_para = (int32_t)((v_para_raw + ((v_para_raw >> 63) & 0xFFF)) >> 12);
    int32_t v_perp = (int32_t)((v_perp_raw + ((v_perp_raw >> 63) & 0xFFF)) >> 12);

    /* iVar11 = contact-point normal velocity (center normal vel + rotation
     * contribution at the lever arm). [CONFIRMED @ 0x00406A60-0x00406A66] */
    int32_t iVar11 = (actor->angular_velocity_yaw / ANGULAR_DIVISOR_W) * iVar9 + v_perp;

    int32_t impulse = 0;
    int32_t new_v_para = v_para;
    int32_t new_v_perp = v_perp;
    int32_t pre_clamp_delta = 0;
    int     clamp_fired = 0;

    /* Early-out when separating [CONFIRMED @ 0x00406A72-0x00406A78]:
     * JS 0x00406CBA — when iVar11 < 0 the original jumps to the function
     * epilogue and returns WITHOUT updating linear_velocity_x/z or
     * angular_velocity_yaw. D1 audit: port previously fell through and
     * rotated (new_v_para, new_v_perp) back to world basis, introducing
     * round-off in lv_x/lv_z on separating contacts. Faithful behavior is
     * to skip the entire writeback block when iVar11 < 0. */
    if (iVar11 >= 0) {
        /* Impulse numerator = ((K>>8) * -0x1100) >> 12 with Borland round-
         * toward-zero on the negative >>12. D2 audit. The compiler-time
         * constant evaluation here yields a different result from the
         * runtime add-and-shift form, but since iVar6 = -25497168 is
         * negative, we apply (val + 0xFFF) >> 12 explicitly.
         * [CONFIRMED @ 0x00406A86-0x00406ACB] */
        int32_t iVar6_num = (V2W_INERTIA_K >> 8) * -V2W_NUM_SCALE;  /* -25497168 */
        int32_t num = (iVar6_num + ((iVar6_num >> 31) & 0xFFF)) >> 12;

        /* Denominator = (iVar9^2 + K) >> 8. [CONFIRMED @ 0x00406AAE-0x00406AD2]
         * iVar9^2 + K is always non-negative (K>0, square>=0) so no sign-
         * round needed; matches port's prior code. */
        int64_t denom64 = ((int64_t)iVar9 * iVar9 + (int64_t)V2W_INERTIA_K) >> 8;
        if (denom64 == 0) denom64 = 1;
        impulse = (int32_t)((int64_t)num * iVar11 / denom64);

        new_v_perp = v_perp + impulse;

        /* Tangential damping — corrected asm reading [CONFIRMED via asm
         * 0x00406B0B-0x00406B69, re-verified 2026-05-02]:
         *   v_para >  0 branch: new_v_para = v_para - delta. If sign flips
         *                       negative (SUB result < 0), JNS at 0x00406B33
         *                       falls through to JMP 0x00406B69 → XOR ECX,ECX
         *                       → zero. So result is clamped to 0 on neg-flip.
         *   v_para <= 0 branch: new_v_para = v_para + delta. TEST/JLE at
         *                       0x00406B65 takes the jump when result <= 0
         *                       (preserve), else falls through to
         *                       XOR ECX,ECX at 0x00406B69 → zero. So result
         *                       is clamped to 0 on pos-flip.
         *
         * Both branches hard-zero on sign-flip. The asymmetry is purely in
         * *which* sign-flip each side cares about (v_para>0 →
         * v_para-delta, clamp to 0 on negative-flip; v_para<=0 →
         * v_para+delta, clamp to 0 on positive-flip).
         *
         * 2026-05-10 — reverted to faithful behaviour on the v_para<=0
         * branch. A prior port version used a magnitude-only soft clamp
         * here (allowing sign-flip up to |incoming v_para|) under the
         * theory that hard-zero killed wall-friction reorient. A Frida
         * long-pass on Moscow (StartSpanOffset=175, PlayerIsAI=1, branch
         * fix-1778394064-46915 long_span280_pass) showed the opposite
         * effect: with the soft clamp, AI cars hit walls, then carry a
         * sign-flipped tangential velocity that prevents them from
         * settling parallel to the wall — they roll back, get hit again,
         * and stall permanently at span ~430. Original AI cars in the
         * same geometry cruise through unobstructed (slot 3 reached
         * span 1373 vs port's 430 in 2882 ticks).
         *
         * Ghidra agent re-read of 0x00406B65-69 (verbatim disasm: TEST/
         * JLE + XOR ECX,ECX) confirmed both branches hard-zero on
         * sign-flip in the original. The earlier "no clamp on v_para<=0"
         * memory note was a misread.
         *
         * Branch order matches the asm (positive first). */
        /* `v_para_round` mirrors the original `(v_para + (v_para>>31 &
         * 0x3F)) >> 6` round-toward-zero pattern. D3 audit: GCC `>> 6` on
         * negative values rounds toward -inf, off by 1 unit; original adds
         * 0x3F before SAR. */
        int32_t v_para_round = (v_para + ((v_para >> 31) & 0x3F)) >> 6;
        int32_t tmp;
        if (v_para > 0) {
            tmp = v_para_round + 0x800 + iVar11 * 2;
            /* D3 round-toward-zero on the >>11 of (tmp * 0x180). */
            int32_t mul_raw = tmp * 0x180;
            int32_t delta = (mul_raw + ((mul_raw >> 31) & 0x7FF)) >> 11;
            pre_clamp_delta = delta;
            new_v_para = v_para - delta;
            if (new_v_para < 0) { new_v_para = 0; clamp_fired = 1; }
        } else {
            tmp = (iVar11 * 2 + 0x800) - v_para_round;
            int32_t mul_raw = tmp * 0x180;
            int32_t delta = (mul_raw + ((mul_raw >> 31) & 0x7FF)) >> 11;
            pre_clamp_delta = delta;
            new_v_para = v_para + delta;
            /* Faithful port of 0x00406B65-69: TEST EAX,EAX / JLE
             * (jump if signed less-or-equal preserves) / fall through to
             * XOR ECX,ECX. → clamp to 0 on positive-flip. */
            if (new_v_para > 0) { new_v_para = 0; clamp_fired = 1; }
        }

        /* Angular velocity update: ω += (impulse * iVar9) / (K / 0x28C).
         * K / 0x28C = 1500000 / 652 = 2300 (integer). This is the yaw
         * alignment kick: with iVar9 non-zero (probe not at CoM along wall),
         * the sign of impulse drives the car toward tangential alignment.
         * [CONFIRMED @ 0x00406B6B-0x00406B7A] */
        int32_t ang_div = V2W_INERTIA_K / ANGULAR_DIVISOR_W;  /* 2300 */
        if (ang_div == 0) ang_div = 1;
        actor->angular_velocity_yaw += (impulse * iVar9) / ang_div;

        /* Rotate (new_v_para, new_v_perp) back to world basis.
         * [CONFIRMED @ 0x406a10]. D6 audit: round-toward-zero on the >>12,
         * matching original (val + (val>>31 & 0xFFF)) >> 12 pattern. */
        int64_t lvx_raw = (int64_t)new_v_para * cos_w - (int64_t)new_v_perp * sin_w;
        int64_t lvz_raw = (int64_t)new_v_para * sin_w + (int64_t)new_v_perp * cos_w;
        actor->linear_velocity_x = (int32_t)((lvx_raw + ((lvx_raw >> 63) & 0xFFF)) >> 12);
        actor->linear_velocity_z = (int32_t)((lvz_raw + ((lvz_raw >> 63) & 0xFFF)) >> 12);

        /* No ±6000 yaw clamp here — original has none inside 0x406980.
         * [CONFIRMED — no write to actor+0x1C4 after LAB_00406B6B] */

        /* Track contact flag: Forward/Reverse handlers pass side=-1 (no write).
         * The original wrote nothing of this kind directly inside the iVar11>=0
         * block either — track_contact_flag is a port-only field used by the
         * port's lateral-handler dispatch (which we have disabled). It's
         * still useful as a per-tick "did wall fire" marker. Gated inside
         * the iVar11>=0 branch per D1: separating contacts shouldn't flag.
         * [CONFIRMED @ 0x406d7e/0x406e4e on the side-encoding mapping] */
        if (side >= 0)
            actor->track_contact_flag = (uint8_t)(side + 1);
    }

    TD5_LOG_I(LOG_TAG,
              "wall_response: side=%d pen=%d angle=%d arm=%d iVar11=%d imp=%d vpara=%d vperp=%d yaw=%d delta=%d",
              side, penetration, wall_angle, iVar9, iVar11, impulse,
              new_v_para, new_v_perp, actor->angular_velocity_yaw,
              pre_clamp_delta);

    if (clamp_fired && actor->slot_index == 0) {
        TD5_LOG_I(LOG_TAG,
                  "wall_response: soft-clamp fired slot0 vpara_in=%d delta=%d vpara_out=%d",
                  v_para, pre_clamp_delta, new_v_para);
    }

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

    /* Pilot trace: capture post-state and emit row. */
    td5_pilot_emit_00406980_leave(actor);
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

/* Per-actor previous-tick world position. Captured at the top of each
 * td5_physics_tick (BEFORE world_pos changes), consumed by
 * td5_physics_apply_render_interpolation after the sim loop drains. */
static TD5_Vec3_Fixed s_prev_world_pos[TD5_MAX_TOTAL_ACTORS];

void td5_physics_snapshot_prev_world_pos(void)
{
    if (!g_actor_table_base) return;
    int total = td5_game_get_total_actor_count();
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;
    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        s_prev_world_pos[slot] = actor->world_pos;
    }
}

void td5_physics_seed_prev_world_pos(void)
{
    /* Race-init seed: zero the table, then snapshot current world_pos so the
     * first interpolation pass before any tick fires lerps current->current. */
    memset(s_prev_world_pos, 0, sizeof(s_prev_world_pos));
    td5_physics_snapshot_prev_world_pos();
}

void td5_physics_apply_render_interpolation(float subtick_fraction)
{
    if (!g_actor_table_base) return;
    if (subtick_fraction < 0.0f) subtick_fraction = 0.0f;
    if (subtick_fraction > 1.0f) subtick_fraction = 1.0f;

    int total = td5_game_get_total_actor_count();
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;

    const float kInv256 = 1.0f / 256.0f;
    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        const TD5_Vec3_Fixed *prev = &s_prev_world_pos[slot];
        const TD5_Vec3_Fixed *cur  = &actor->world_pos;
        float dx = (float)(cur->x - prev->x);
        float dy = (float)(cur->y - prev->y);
        float dz = (float)(cur->z - prev->z);
        actor->render_pos.x = ((float)prev->x + dx * subtick_fraction) * kInv256;
        actor->render_pos.y = ((float)prev->y + dy * subtick_fraction) * kInv256;
        actor->render_pos.z = ((float)prev->z + dz * subtick_fraction) * kInv256;
    }
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

    /* Snapshot world_pos -> prev_world_pos BEFORE any per-actor integration.
     * Sub-tick render interpolation lerps prev -> cur using g_subTickFraction
     * once the sim loop drains for this render frame. */
    td5_physics_snapshot_prev_world_pos();

    s_physics_tick_counter++;
    if ((s_physics_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Physics tick: actor_count=%d", total);
    }

    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        td5_physics_update_vehicle_actor(actor);
    }

    /* Run V2V resolution UNCONDITIONALLY each sub-tick — matches
     * RunRaceFrame @ 0x0042B580 which calls ResolveVehicleContacts every
     * iteration of the sub-tick loop without any paused gate. The original
     * relies on countdown V2V to gradually separate the initial OBB overlap
     * (paired grid cars spawn ~56 units inside each other's box on circuit
     * tracks); without these pushes, the first race-tick V2V delivers one
     * large kick that visibly slides slot 0 sideways by +3400 units.
     *
     * Safe for stationary spawn: V2V impulse is (NUM/denom) * rel_vel and
     * rel_vel is built from linear_velocity and angular_velocity (both 0
     * during countdown), so the impulse solver produces 0; only the
     * positional push at lines 2777-2780 fires, which converges the
     * overlap to 0 over the 160 sub-ticks. [CONFIRMED @ 0x42B5C0 Ghidra
     * pass 2026-05-13: no paused gate around ResolveVehicleContacts.] */
    td5_physics_resolve_vehicle_contacts();
}

/* ========================================================================
 * Master dispatcher -- UpdateVehicleActor (0x406650)
 * ======================================================================== */

void td5_physics_update_vehicle_actor(TD5_Actor *actor)
{
    if (!actor) return;

    /* precise-port pilot 0x00406650: capture enter snapshot. */
    td5_pilot_emit_00406650_enter(actor);

    /* 1. Increment frame counter — listing 0x00406664 INC WORD ptr [+0x338]. */
    actor->frame_counter++;

    /* 2. Clear per-frame flags — listing 0x00406673 MOV BYTE [+0x37B], 0. */
    actor->track_contact_flag = 0;

    /* 3. Ghost reset — listing 0x0040667A-66A6:
     *   if (g_selectedGameType != 0 && (uint8)[+0x37E] != 0) {
     *       [+0x30C] = 0           // steering_command
     *       [+0x33E] = 0xFF00      // encounter_steering_cmd
     *       [+0x36D] = 1           // brake_flag
     *   }
     * Field 0x37E is overloaded: time-trial-ghost flag (single-race) or
     * checkpoint_count (P2P). Original gates BOTH on game_type != 0; port
     * previously omitted the game_type gate AND wrote wrong values.
     * [Audit D2 — fixed 2026-05-14.] */
    if (g_game_type != 0 && actor->ghost_flag) {
        actor->steering_command = 0;
        actor->encounter_steering_cmd = (int16_t)0xFF00;
        actor->brake_flag = 1;
    }

    /* 4. Speed tracking — listing 0x004066A6-66E9, gated on finish_time == 0:
     *     abs_spd = abs(longitudinal_speed)
     *     accumulated_distance += abs_spd >> 8
     *     peak_speed = max(peak_speed, (int16)(abs_spd >> 8))
     * [Audit D3 — finish_time gate added 2026-05-14.] */
    if (actor->finish_time == 0) {
        int32_t spd = actor->longitudinal_speed;
        if (spd < 0) spd = -spd;
        int32_t spd_sar8 = spd >> 8;   /* sar8_rz collapses to >>8 for spd >= 0 */
        actor->accumulated_distance += spd_sar8;
        if ((int16_t)spd_sar8 > actor->peak_speed)
            actor->peak_speed = (int16_t)spd_sar8;
    }

    /* 4b. Race timer + average-speed block — listing 0x004066FB-6769:
     *   if (gRaceCameraTransitionGate == 0 && finish_time == 0) {
     *       if ((uint16)timing_frame_counter < 0xFFFF) timing_frame_counter++
     *       average_speed_metric = (int16)(accumulated_distance / timing_frame_counter)
     *       (re-update peak_speed from same abs_spd — algebraically a no-op
     *        because nothing in between mutates longitudinal_speed)
     *   }
     * The port's g_game_paused mirrors both gRaceCameraTransitionGate and
     * g_gamePaused (both are 1 during countdown, 0 during the race), so this
     * gate uses !g_game_paused. [Audit D4 — added 2026-05-14.] */
    if (!g_game_paused && actor->finish_time == 0) {
        uint16_t *tc = (uint16_t *)((uint8_t *)actor + 0x34C);
        if (*tc < 0xFFFF) (*tc)++;
        if (*tc != 0) {
            actor->average_speed_metric =
                (int16_t)(actor->accumulated_distance / (int32_t)(uint32_t)*tc);
        }
        /* Second peak_speed update at 0x00406745-6762 — identical to the
         * first because longitudinal_speed hasn't been mutated; skipped to
         * avoid a redundant write that would also be visible in the trace. */
    }

    /* 5. Attitude clamp (unless scripted mode) — listing 0x0040677B-678E:
     *     if ([+0x379] == 0) ClampVehicleAttitudeLimits(actor) */
    if (actor->vehicle_mode == 0)
        td5_physics_clamp_attitude(actor);

    /* 6. Dynamics dispatch */
    if (actor->vehicle_mode == 0 && !g_game_paused) {
        /* Select effective grip: min of grip_reduction and race_position.
         * Listing 0x00406823-683D: AL=[+0x380]; CL=[+0x383]; if (CL < AL) AL=CL;
         * MOV [+0x380], AL.  Original WRITES the clamped result back to
         * actor->grip_reduction. Port previously kept it in a local only.
         * [Audit D7 — fixed 2026-05-14.] */
        uint8_t eff_grip = actor->grip_reduction;
        if (actor->race_position < eff_grip)
            eff_grip = actor->race_position;
        actor->grip_reduction = eff_grip;

        if (actor->wheel_contact_bitmask == 0x0F && actor->airborne_frame_counter >= 3) {
            /* Stunned/damping recovery mode */
            TD5_LOG_I(LOG_TAG, "state0f_enter: slot=%d afc=%d av_roll=%d av_pitch=%d",
                      actor->slot_index, actor->airborne_frame_counter,
                      actor->angular_velocity_roll, actor->angular_velocity_pitch);
            td5_physics_state0f_damping(actor);
        } else if (actor->slot_index < 6 && g_race_slot_state[actor->slot_index] == 1) {
            /* Human player — listing 0x0040685C tests `state == 1` strictly,
             * not `state != 0`. [Audit D13 — tightened 2026-05-14.] */
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
        /* [Audit D1 — removed double frame_counter increment 2026-05-14.]
         * Original 0x004067A2 does NOT re-increment frame_counter inside the
         * vehicle_mode==1 branch — the single increment at function entry
         * (line 617 above, matching listing 0x00406664) is the only one. */

        /* D11 — Scripted-mode world_pos write, listing 0x004067A8-67D9:
         *   world_pos.x = (int16)*(+0x208) << 8
         *   world_pos.y = (int16)*(+0x20A) << 8
         *   world_pos.z = (int16)*(+0x20C) << 8
         * The three shorts at +0x208/A/C alias the "display_angles" struct in
         * the port, but the original treats them as recovery-target world
         * coordinates during vehicle_mode==1 — the recovery animation
         * overwrites world_pos every tick from this triple.
         * [Audit D11 — added 2026-05-14.] */
        {
            uint8_t *abase = (uint8_t *)actor;
            int16_t rx = *(int16_t *)(abase + 0x208);
            int16_t ry = *(int16_t *)(abase + 0x20A);
            int16_t rz = *(int16_t *)(abase + 0x20C);
            actor->world_pos.x = (int32_t)rx << 8;
            actor->world_pos.y = (int32_t)ry << 8;
            actor->world_pos.z = (int32_t)rz << 8;
        }

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
        /* Paused branch — listing 0x00406881-690B, line-by-line.
         *
         * Order from the listing:
         *   DL = [+0x383]                     ; race_position
         *   [+0x381] = DL                     ; prev_race_position = race_position
         *   UpdateVehicleEngineSpeedSmoothed(actor)
         *   if (g_selectedGameType != 0) {
         *       if (slot[+0x375].state != 1)
         *           engine_speed_accum = (cardef[0x72] << 1) / 3
         *   }
         *   [+0x376] = 0                       ; surface_contact_flags = 0
         *   if (slot[slotIndex].state == 1) {
         *       cardef = [+0x1BC]
         *       three_quart_redline = ((int16)cardef[0x72] * 3 + sgn_adj) >> 2  (round-toward-zero)
         *       if (engine_speed_accum > three_quart_redline)
         *           surface_contact_flags = (uint8)cardef[0x76]
         *   }
         *
         * Port previously matched only the UpdateVehicleEngineSpeedSmoothed
         * call + the AI engine pin. [Audit D5 — added prev_race_position,
         * scf=0, scf-from-cardef-on-high-rpm 2026-05-14.] */

        /* D5a — prev_race_position = race_position */
        actor->prev_race_position = actor->race_position;

        update_engine_speed_smoothed(actor);

        /* D5b — AI engine pin: gate on g_selectedGameType != 0 (championship/
         * cup modes only). [RE basis: 0x004068B3-0x004068CB] */
        if (g_game_type != 0 &&
            actor->slot_index < 6 &&
            g_race_slot_state[actor->slot_index] != 1) {
            int32_t redline = (int32_t)PHYS_S(actor, 0x72);
            actor->engine_speed_accum = (redline << 1) / 3;
        }

        /* D5c — clear surface_contact_flags */
        actor->surface_contact_flags = 0;

        /* D5d — player path: set scf from cardef[0x76] when engine > 3/4 redline.
         * Listing 0x004068D8-690B:
         *   if (gRaceSlotStateTable.slot[slotIndex].state == 1) {
         *       phys = [+0x1BC]                     ; tuning_data_ptr
         *       eax = (int16)phys[0x72]
         *       lea eax, [eax + eax*2]              ; eax = redline * 3
         *       cdq; and edx, 3; add eax, edx; sar eax, 2  ; round-toward-zero /4
         *       if (engine_speed_accum > eax) [+0x376] = phys[0x76]
         *   } */
        if (actor->slot_index < 6 && g_race_slot_state[actor->slot_index] == 1) {
            int16_t *phys = get_phys(actor);
            if (phys) {
                int32_t redline = (int32_t)PHYS_S(actor, 0x72);
                int32_t triple = redline * 3;
                int32_t thresh = (triple + ((triple >> 31) & 3)) >> 2;  /* /4 round-rz */
                if (actor->engine_speed_accum > thresh) {
                    actor->surface_contact_flags =
                        ((uint8_t *)phys)[0x76];
                }
            }
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

    /* 9. Update surface_contact_flags for the NEXT tick's dynamics dispatch
     * — HUMAN-PLAYER PATH ONLY.
     *
     * [RE basis: the original's surface_contact_flags is only written inside
     * UpdatePlayerVehicleDynamics @ 0x00404030, at a late drivetrain-commit
     * condition. AI cars in the original never have scf written — it stays
     * at 0 from the spawn memset for the entire race.]
     *
     * Frida rotation_probe.csv 2026-05-03 confirmed: orig slot 0 (AI)
     * scf=0 at every sim_tick from 1..50. Port had scf=3 from sim_tick=2
     * onwards because this update fired for AI cars too — a port-specific
     * deviation that flipped the AI dynamics path from airborne (scf=0)
     * to grounded (scf=3), changing drive-torque application on tick 2+.
     *
     * Gated on slot_state==1 (human player) so AI slots match the
     * original's scf=0 behavior. The player path still gets the eager
     * update because the original's late conditional inside
     * UpdatePlayerVehicleDynamics isn't fully ported. */
    if (!g_game_paused &&
        actor->slot_index < 6 &&
        g_race_slot_state[actor->slot_index] == 1) {
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

    /* precise-port pilot 0x00406650: capture leave snapshot. */
    td5_pilot_emit_00406650_leave(actor);
}

/* ========================================================================
 * Player 4-wheel dynamics -- UpdatePlayerVehicleDynamics (0x404030)
 * ======================================================================== */

/* Pilot trace hooks (pool0 / 0x00404030) */
extern void td5_pilot_emit_00404030_enter(const TD5_Actor *actor, uintptr_t caller_ra);
extern void td5_pilot_emit_00404030_leave(const TD5_Actor *actor);

void td5_physics_update_player(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    td5_pilot_emit_00404030_enter(actor, (uintptr_t)__builtin_return_address(0));

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
                /* Faithful brake-magnitude/direction clamp matching original
                 * UpdatePlayerVehicleDynamics @ 0x004044*. Decompiled form:
                 *   iVar11 = -bf;
                 *   if (-vsa <= -bf) iVar11 = -vsa;
                 *   if (bf <= iVar11) {
                 *     bf = -bf;
                 *     if (-vsa <= -bf) bf = -vsa;
                 *   }
                 * Net effect: output magnitude = min(|bf|,|vsa|), output sign
                 * OPPOSES v_steer_axis when bf opposes vsa. The port previously
                 * preserved bf's throttle sign, which made AI brake on a
                 * backward-moving car ACCELERATE backward (negative wheel torque
                 * on negative velocity) — the actual "reverses nonstop" trap
                 * after the brake→REVERSE workaround engaged. The original's
                 * formula correctly produces forward wheel torque to decelerate
                 * backward motion. */
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

                int32_t neg_bf  = -bf;
                int32_t neg_vsa = -v_steer_axis;
                int32_t lim = neg_bf;
                if (neg_vsa <= neg_bf) lim = neg_vsa;
                if (bf <= lim) {
                    bf = neg_bf;
                    if (neg_vsa <= neg_bf) {
                        bf = neg_vsa;
                    }
                }
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
                    /* Original airborne RWD (0x404576-0x404592): SAR ECX,1 then MOV [EBP-0x18] only.
                     * Only ONE rear wheel slot receives torque/2; wheel_drive[3] stays 0
                     * (zero-initialised at line 829). Writing both gives 2× vz vs original.
                     * [CONFIRMED by Ghidra agent: single MOV in RWD airborne path] */
                    wheel_drive[2] = drive_torque >> 1;
                    break;
                case 2: /* FWD */
                    /* Original airborne FWD (0x404594-0x4045AE): writes [EBP-0x4] and [EBP+0x8]
                     * — both front slots. [CONFIRMED] */
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

        /* A = (I/1024) * ((b*w)/652 - raw_lat) / 4096 * sin(s) */
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
     * uVar12 = (steering_command + sign_round) >> 8 [CONFIRMED @ FUN_00404030
     * decompilation: *(int*)(short_ptr + 0x186) = byte offset 0x30C =
     * steering_command; the arithmetic right-shift rounding idiom matches
     * Ghidra's (x + (x>>31 & 0xFF)) >> 8 pattern]. */
    {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x32);
        if (gear_ratio != 0) {
            int32_t rpm_norm = (((actor->engine_speed_accum - 400) * 0x1000) / 0x2d) / gear_ratio;
            int32_t steer    = actor->steering_command;
            int32_t uVar12   = (steer + (steer >> 31 & 0xff)) >> 8;
            int32_t wheelspin = rpm_norm * 0x100 - uVar12;
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

    td5_pilot_emit_00404030_leave(actor);
}

/* ========================================================================
 * AI 2-axle dynamics -- UpdateAIVehicleDynamics (0x404EC0)
 * ======================================================================== */

/* Pilot trace emitters (pool12 / precise-port workflow) */
extern void td5_pilot_emit_00404EC0_enter(const TD5_Actor *actor, uintptr_t caller_ra);
extern void td5_pilot_emit_00404EC0_leave(const TD5_Actor *actor);

void td5_physics_update_ai(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    td5_pilot_emit_00404EC0_enter(actor, (uintptr_t)__builtin_return_address(0));

    /* Calls-trace probe: capture per-slot AI dynamics entry state.
     * Hooks YAML: re/trace-hooks/tick0_ai_chain.yaml
     * Original RVA: 0x00404EC0 UpdateAIVehicleDynamics
     * Args layout: slot, steer_cmd, yaw_accum, vlong, encounter_steer,
     *              throttle_byte, brake_flag, sub_lane */
    {
        char *_a = (char *)actor;
        TD5_TRACE_CALL_ENTER("ai_dynamics",
            (int32_t)actor->slot_index,
            *(int32_t *)(_a + 0x30C),                       /* steering_cmd */
            *(int32_t *)(_a + 0x1F4),                       /* yaw_accum */
            *(int32_t *)(_a + 0x314),                       /* longitudinal_speed */
            (int32_t)*(int16_t *)(_a + 0x33E),              /* encounter_steer */
            (int32_t)*(uint8_t *)(_a + 0x36F),              /* throttle_state */
            (int32_t)*(uint8_t *)(_a + 0x36D),              /* brake_flag */
            (int32_t)*(uint8_t *)(_a + 0x08C));             /* sub_lane_index */
    }

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
    /* iVar11 in original — pure body-lateral, used verbatim in bicycle solve.
     * yaw_corr is ONLY subtracted for actor->lateral_speed (+0x318 field).
     * [CONFIRMED @ 0x00405285: original never subtracts yaw_corr from iVar11
     *  before passing it into the matrix] */
    int32_t raw_lat = (cos_h * vx - sin_h * vz) >> 12;
    int32_t yaw_rate = actor->angular_velocity_yaw;
    int32_t inertia = PHYS_I(actor, 0x20);
    int32_t inertia_div = inertia / 0x28C;
    if (inertia_div == 0) inertia_div = 1;
    int32_t yaw_corr = ((sin_d * front_weight) >> 12) * yaw_rate / 0x28C;

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
        /* Brake path — verbatim port of UpdateAIVehicleDynamics @ 0x004051D5
         * -0x00405282. Three corrections vs prior implementation:
         *
         * 1. Uses BRAKE coefficients (phys+0x6E front, phys+0x70 rear) for the
         *    F_raw/R_raw products, not the axle weights (phys+0x28/0x2A). The
         *    original explicitly reads `*(short *)(puVar2 + 0x6e)` and
         *    `*(short *)(puVar2 + 0x70)` here.
         *
         * 2. Signed nested-clamp pattern (same shape as commit 600fb62 for
         *    the player path). Preserves directional sign: brake force opposes
         *    motion. Prior abs-clamp gave backward force on backward motion
         *    (the "AI accelerates reverse" half of the recover-stall trap).
         *
         * 3. Different velocity bounds per axle:
         *    - FRONT_DRIVE (local_3c) clamped against lateral_speed (+0x318,
         *      steered-frame v_long minus yaw_corr). At full recovery steer,
         *      lateral_speed < v_long → tighter clamp keeps steering authority.
         *    - REAR_DRIVE  (local_40) clamped against body v_long (+0x314).
         *
         * Variable names mirror Ghidra's so the LAB_00405285 entry block is
         * line-for-line cross-checkable with the decomp.
         * [CONFIRMED @ 0x004051D5-0x00405282 via Ghidra read-only session.] */
        td5_physics_update_engine_speed(actor);
        {
            int32_t lateral_speed = actor->lateral_speed;  /* iVar18 at brake-clamp time */

            /* R_raw, F_raw with truncate-toward-zero round bias */
            int32_t R_raw = ((int32_t)brake_rear  * throttle
                          + ((((int32_t)brake_rear  * throttle) >> 31) & 0xFF)) >> 8;
            int32_t F_raw = ((int32_t)brake_front * throttle
                          + ((((int32_t)brake_front * throttle) >> 31) & 0xFF)) >> 8;

            /* --- FRONT clamp: bound by lateral_speed (+0x318, steered frame).
             * Ghidra decomp:
             *   iVar13 = -F_raw;
             *   local_3c = -(lateral_speed/2);
             *   iVar18 = iVar13;
             *   if (local_3c <= iVar13) iVar18 = local_3c;
             *   if ((iVar18 < F_raw) || (iVar4 = iVar13, iVar13 < local_3c)) {
             *       local_3c = iVar4;  // F_raw on first leg, -F_raw on second leg
             *   }
             *   local_3c = local_3c / 2;
             */
            int32_t neg_F = -F_raw;
            int32_t local_3c = -(lateral_speed / 2);
            int32_t t1 = neg_F;
            if (local_3c <= neg_F) t1 = local_3c;
            if (t1 < F_raw) {
                local_3c = F_raw;
            } else if (neg_F < local_3c) {
                local_3c = neg_F;
            }
            local_3c = local_3c / 2;

            /* --- REAR clamp: bound by body v_long (+0x314).
             * Ghidra decomp:
             *   iVar4 = -(v_long/2);
             *   iVar13 = -R_raw;
             *   iVar18 = iVar13;
             *   if (iVar4 <= iVar13) iVar18 = iVar4;
             *   if (local_40 <= iVar18) {
             *       local_40 = iVar4;
             *       if (iVar13 < iVar4) local_40 = iVar13;
             *   }
             *   local_40 = local_40 / 2;
             */
            int32_t neg_R = -R_raw;
            int32_t neg_half_vlong = -(v_long / 2);
            int32_t local_40 = R_raw;
            int32_t t2 = neg_R;
            if (neg_half_vlong <= neg_R) t2 = neg_half_vlong;
            if (local_40 <= t2) {
                local_40 = neg_half_vlong;
                if (neg_R < neg_half_vlong) local_40 = neg_R;
            }
            local_40 = local_40 / 2;

            front_drive = local_3c;
            rear_drive  = local_40;

            if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "AI_BRAKE: throttle=%d brk_f=%d brk_r=%d F_raw=%d R_raw=%d "
                    "lat_spd=%d v_long=%d front_drv=%d rear_drv=%d brake_flag=%d",
                    throttle, brake_front, brake_rear, F_raw, R_raw,
                    lateral_speed, v_long, front_drive, rear_drive,
                    (int)actor->brake_flag);
            }
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

        /* yaw_term `iVar15`, yaw_corr `iVar16` (reused).
         * raw_lat = iVar11 in original — body-lateral WITHOUT yaw_corr.
         * [CONFIRMED @ 0x00405285: original uses iVar11 here, not actor->lateral_speed] */
        int32_t iVar15 = (I / 0x28C) * omega;                                      /* yaw_term */
        int32_t iVar16 = SBR(I, 0x3FF, 10) * (((int32_t)(Wr * omega)) / 0x28C - raw_lat); /* (I>>10)*(Wr*ω/652 - raw_lat) */
                iVar16 = SBR(iVar16, 0xFFF, 12) * sin_d;

        /* iVar13 drive+vlong cos term. Original operands: local_3c = front_drive
         * (paired with cos_d inside), iVar18 = rear_drive (unpaired). */
                iVar13 = (SBR(front_drive * cos_d, 0xFFF, 12) + rear_drive + v_long) * iVar14;  /* *D */
                iVar13 = SBR(iVar13, 0xFFF, 12) * cos_d;

        /* iVar14 reused for D*front_drive*sin_d² (original uses local_3c = front_drive). */
                iVar14 = iVar14 * front_drive;
                iVar14 = SBR(iVar14, 0xFFF, 12) * sin_d;
                iVar14 = SBR(iVar14, 0xFFF, 12) * sin_d;

        /* iVar17 = (yaw_term - raw_lat*Wf) >> 10 * (Wf+Wr) >> 12 * cos_d. */
        int32_t iVar17 = iVar15 - raw_lat * Wf;
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
                iVar15  = raw_lat * Wr + iVar15;                                              /* raw_lat*Wr + yaw_term */
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
        int32_t mag_f = (int32_t)sqrtf((float)mag_sq_f); /* [CONFIRMED @ 0x0040554B: FILD/FSQRT/__ftol] */
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
        int32_t mag_r = (int32_t)sqrtf((float)mag_sq_r); /* [CONFIRMED @ 0x004055D9: FILD/FSQRT/__ftol] */
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

    /* --- 10. World-frame force application [CONFIRMED @ 0x4056FA-0x405762]
     * Per Ghidra raw decomp + raw asm 2026-05-03 (Opus 4.7 audit round 5):
     *   iVar18 = rear_drive  (post-LAB doubled, paired with sin_h/cos_h)
     *   local_3c = front_drive (post-LAB doubled, paired with sin_s/cos_s)
     *   local_44 = FRONT_LAT (post-overwrite, paired with cos_s/sin_s — steered)
     *   local_40 = REAR_LAT  (post-overwrite, paired with cos_h/sin_h — heading)
     * (An earlier audit round inverted iVar18 and local_3c — disproven by
     * raw asm at LAB_00405285 + IMUL ordering at 0x4056F1/0x405712.)
     *
     *   ai_fx = (rear_drive*sin_h + rear_lat*cos_h + front_drive*sin_s + front_lat*cos_s) >> 12
     *   ai_fz = (rear_drive*cos_h - rear_lat*sin_h + front_drive*cos_s - front_lat*sin_s) >> 12
     *
     * Front forces in steered frame (cos_s/sin_s).
     * Rear forces in body frame (cos_h/sin_h). */
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

    /* --- 12. Suspension integration ---
     * Pass the net world-frame velocity delta as the spring excitation
     * (matches original at 0x00404EA2 passing iVar11/iVar36). */
    td5_physics_integrate_suspension(actor, ai_fx, ai_fz);

    /* --- 13. Tire slip accumulation [CONFIRMED @ 0x405768-0x40577B]
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

    td5_pilot_emit_00404EC0_leave(actor);
}

/* ========================================================================
 * Traffic simplified dynamics -- IntegrateVehicleFrictionForces (0x4438F0)
 * ======================================================================== */

void td5_physics_update_traffic(TD5_Actor *actor)
{
    /* Literal port of IntegrateVehicleFrictionForces @ 0x004438F0.
     * Transcribed from Ghidra decompilation; every SAR uses
     * truncate-toward-zero rounding: (x + ((x>>31)&mask)) >> shift. */

#ifdef TD5_PILOT_TRACE_TRAFFIC
    td5_pilot_emit_traffic_friction_enter(actor);
#endif

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

#ifdef TD5_PILOT_TRACE_TRAFFIC
    td5_pilot_emit_traffic_friction_leave(actor);
#endif
}

/* ========================================================================
 * ApplyDampedSuspensionForce (0x4437C0) -- Traffic only
 *
 * Simple 2-DOF spring-damper for roll and pitch.
 *
 * Byte-faithful port (pool8 precise-port pilot 2026-05-14).
 *
 * Original (per axis):
 *   new_pos = old_pos + old_vel                          [stored before damping]
 *   new_vel = old_vel
 *           + sar8_rz(-old_vel * 32)                     ; velocity damping (OLD vel)
 *           - sar8_rz( new_pos * 32)                     ; spring restore   (NEW pos)
 *           + sar8_rz( drive    * 128)                   ; external force
 *   if new_pos > +CLAMP: new_pos = +CLAMP, new_vel = 0
 *   if new_pos < -CLAMP: new_pos = -CLAMP, new_vel = 0
 *
 * `sar8_rz(x) = ((x < 0) ? (x + 0xFF) : x) >> 8`  — matches CDQ;AND EDX,0xFF;ADD;SAR 8.
 * ======================================================================== */

static inline int32_t sar8_rz(int32_t x) {
    /* Encodes original's CDQ + AND EDX,0xFF + ADD EAX,EDX + SAR EAX,8
     * (round-to-zero signed divide by 256). */
    return (x + (((int32_t)((uint32_t)x >> 31)) * 0xFF)) >> 8;
}

static void apply_damped_suspension_force(TD5_Actor *actor, int32_t lateral, int32_t longitudinal)
{
#ifdef TD5_PILOT_TRACE_TRAFFIC
    td5_pilot_emit_traffic_susp_enter(actor, lateral, longitudinal);
#endif

    /* === Axis 0 (lateral-driven): pos @ +0x2DC, vel @ +0x2EC, clamp ±0x2000 ===
     * [CONFIRMED @ 0x004437C4-0x00443859] */
    {
        int32_t old_pos = actor->wheel_suspension_pos[0];
        int32_t old_vel = actor->wheel_spring_dv[0];

        /* new_pos = old_pos + old_vel  [0x004437D3-0x004437D5] */
        int32_t new_pos = old_pos + old_vel;
        actor->wheel_suspension_pos[0] = new_pos;

        /* new_vel = old_vel + sar8_rz(-old_vel*32) - sar8_rz(new_pos*32) + sar8_rz(drive*128)
         *           [0x004437DB-0x00443821] */
        int32_t new_vel = old_vel
                        + sar8_rz(-old_vel * 32)
                        - sar8_rz( new_pos * 32)
                        + sar8_rz( lateral * 128);
        actor->wheel_spring_dv[0] = new_vel;

        /* Two separate if-clamps (matches listing 0x00443827-0x00443859 exactly).
         * Algebraically equivalent to if/else-if. */
        if (new_pos > 0x2000) {
            actor->wheel_suspension_pos[0] = 0x2000;
            actor->wheel_spring_dv[0] = 0;
        }
        if (actor->wheel_suspension_pos[0] < -0x2000) {
            actor->wheel_suspension_pos[0] = -0x2000;
            actor->wheel_spring_dv[0] = 0;
        }
    }

    /* === Axis 1 (longitudinal-driven): pos @ +0x2E0, vel @ +0x2F0, clamp ±0x4000 ===
     * [CONFIRMED @ 0x00443859-0x004438EA] */
    {
        int32_t old_pos = actor->wheel_suspension_pos[1];
        int32_t old_vel = actor->wheel_spring_dv[1];

        int32_t new_pos = old_pos + old_vel;
        actor->wheel_suspension_pos[1] = new_pos;

        int32_t new_vel = old_vel
                        + sar8_rz(-old_vel * 32)
                        - sar8_rz( new_pos * 32)
                        + sar8_rz( longitudinal * 128);
        actor->wheel_spring_dv[1] = new_vel;

        if (new_pos > 0x4000) {
            actor->wheel_suspension_pos[1] = 0x4000;
            actor->wheel_spring_dv[1] = 0;
        }
        if (actor->wheel_suspension_pos[1] < -0x4000) {
            actor->wheel_suspension_pos[1] = -0x4000;
            actor->wheel_spring_dv[1] = 0;
        }
    }

    /* Original does NOT feed suspension into angular_velocity.
     * [CONFIRMED @ 0x4437C0-0x4438EC: writes only to +0x2DC/+0x2EC/+0x2E0/+0x2F0,
     *  never to angular_velocity_roll/pitch.]
     * Roll/pitch display angles are computed from surface normal + suspension
     * correction in UpdateTrafficVehiclePose, not from euler accumulators. */

#ifdef TD5_PILOT_TRACE_TRAFFIC
    td5_pilot_emit_traffic_susp_leave(actor);
#endif
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
                           int32_t pos_a_x_fp, int32_t pos_a_z_fp,
                           int32_t pos_b_x_fp, int32_t pos_b_z_fp,
                           int32_t yaw_a_acc, int32_t yaw_b_acc,
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

    /* [LISTING ALIGNMENT 0x00408570]: the original's
     *   CollectVehicleCollisionContacts(int param_1, int param_2,
     *                                   int *param_3, int *param_4, short *param_5)
     * receives raw 24.8-fp position triplets (param_3 = &local_80 = pos triplet of A,
     * etc.) and raw 24.8-fp euler accumulators (param_4 = &local_94 = yaw triplet),
     * then computes:
     *   iVar3  = (param_3[5] - param_3[2]) >> 8;          // (pos_b.z - pos_a.z) >> 8
     *   iVar24 = (param_3[3] - *param_3) >> 8;            // (pos_b.x - pos_a.x) >> 8
     *   uVar25 = (uint)(short)((*param_4 - param_4[1]) >> 8); // (yaw_a - yaw_b) >> 8
     *   CosFixed12bit(*param_4 >> 8);                     // yaw_a >> 8
     *   CosFixed12bit(param_4[1] >> 8);                   // yaw_b >> 8
     * — i.e. the >>8 happens on the DELTA (positions) or on the raw accumulator
     * (headings), INSIDE the callee. The prior port wrapper required the caller
     * to pre-shift, so two callers in resolve_vehicle_collision_pair both did
     * (pos_X >> 8) before calling. That introduced an off-by-1-LSB divergence
     * for any pair whose fractional bytes underflowed on subtract
     * (e.g. pos_a=255, pos_b=0: orig (0-255)>>8 = -1; pre-shifted (0>>8)-(255>>8)=0).
     *
     * Fix (this commit): match the listing — wrapper now takes raw 24.8 fp pos +
     * raw 24.8 fp yaw accumulators, computes the same deltas-then-shift and
     * raw-then-shift internally. */
    int32_t delta_world_x = (pos_b_x_fp - pos_a_x_fp) >> 8;
    int32_t delta_world_z = (pos_b_z_fp - pos_a_z_fp) >> 8;
    int32_t heading_a = yaw_a_acc >> 8;            /* matches `*param_4 >> 8` */
    int32_t heading_b = yaw_b_acc >> 8;            /* matches `param_4[1] >> 8` */
    /* dheading derived from raw accumulator subtract then shift, matching
     * `uVar25 = (uint)(short)((*param_4 - param_4[1]) >> 8)`. The (short) cast
     * means the LOW 16 bits are then treated as int — equivalent in our path
     * because cos_fixed12/sin_fixed12 mask with & 0xFFF internally. */
    int32_t dheading_raw_shift = (yaw_a_acc - yaw_b_acc) >> 8;

    /* Pool15 V2V pilot trace — capture inputs at entry. */
    TD5_PilotV2VContactSnap _v2v_snap;
    int _v2v_slot_a = a ? a->slot_index : -1;
    int _v2v_slot_b = b ? b->slot_index : -1;
    int _v2v_call_idx = td5_pilot_v2v_next_call_idx();
    _v2v_snap.actor_a_addr = (uint32_t)(uintptr_t)a;
    _v2v_snap.actor_b_addr = (uint32_t)(uintptr_t)b;
    /* Pilot snapshot stores raw 24.8 fp coords for direct comparison with the
     * Frida capture of the original callee. */
    _v2v_snap.ax = pos_a_x_fp;
    _v2v_snap.ay = a ? a->world_pos.y : 0;
    _v2v_snap.az = pos_a_z_fp;
    _v2v_snap.bx = pos_b_x_fp;
    _v2v_snap.by = b ? b->world_pos.y : 0;
    _v2v_snap.bz = pos_b_z_fp;
    _v2v_snap.yaw_a_raw = yaw_a_acc;
    _v2v_snap.yaw_b_raw = yaw_b_acc;
    _v2v_snap.cardef_a_off04 = (int16_t)front_z_a;
    _v2v_snap.cardef_a_off08 = (int16_t)half_w_a;
    _v2v_snap.cardef_a_off14 = (int16_t)rear_z_a;
    _v2v_snap.cardef_b_off04 = (int16_t)front_z_b;
    _v2v_snap.cardef_b_off08 = (int16_t)half_w_b;
    _v2v_snap.cardef_b_off14 = (int16_t)rear_z_b;
    memset(_v2v_snap.corner_proj_x, 0, sizeof(_v2v_snap.corner_proj_x));
    memset(_v2v_snap.corner_proj_z, 0, sizeof(_v2v_snap.corner_proj_z));
    memset(_v2v_snap.corner_own_x,  0, sizeof(_v2v_snap.corner_own_x));
    memset(_v2v_snap.corner_own_z,  0, sizeof(_v2v_snap.corner_own_z));
    _v2v_snap.bitmask = 0;
    td5_pilot_v2v_contact_emit_enter(&_v2v_snap, _v2v_slot_a, _v2v_slot_b, _v2v_call_idx);

    /* Precompute sin/cos for each heading. cos_fixed12/sin_fixed12 mask with
     * & 0xFFF internally, so passing the raw (yaw_acc >> 8) is safe and matches
     * the listing's CosFixed12bit(*param_4 >> 8) / CosFixed12bit(param_4[1] >> 8). */
    int32_t cos_a = cos_fixed12(heading_a);
    int32_t sin_a = sin_fixed12(heading_a);
    int32_t cos_b = cos_fixed12(heading_b);
    int32_t sin_b = sin_fixed12(heading_b);

    /* Delta heading. Listing uses `(*param_4 - param_4[1]) >> 8` =
     * (yaw_a - yaw_b) >> 8 = +dheading_inv (rotation from A to B).
     * The original then feeds that uVar25 into CosFixed12bit/SinFixed12bit and
     * uses the resulting cos/sin to rotate B's CORNERS into A's frame
     * (= "+dheading_inv" rotates B-frame vectors into A-frame; the inverse of
     * the orientation difference). Port's dheading_inv variable is the same. */
    int32_t dheading_inv = dheading_raw_shift & 0xFFF;
    int32_t cos_di = cos_fixed12(dheading_inv);
    int32_t sin_di = sin_fixed12(dheading_inv);

    /* For the symmetric second half (A's corners into B's frame), the rotation
     * is the opposite sign: dheading = -dheading_inv. */
    int32_t dheading = (-dheading_raw_shift) & 0xFFF;
    int32_t cos_d = cos_fixed12(dheading);
    int32_t sin_d = sin_fixed12(dheading);

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
    /* World-space delta from A to B, in display units. Listing computes this
     * as (param_3[3] - *param_3) >> 8 and (param_3[5] - param_3[2]) >> 8,
     * i.e. subtraction on the raw 24.8 fp THEN >>8. The previously pre-shifted
     * (pos_b>>8) - (pos_a>>8) form drifts by 1 LSB on fractional-underflow. */
    int32_t delta_x = delta_world_x;
    int32_t delta_z = delta_world_z;

    /* Rotate delta into A's local frame.
     * [CONFIRMED @ 0x004086D0-0x004086F6]: game uses "CW from +Z" convention
     *   iVar20 = iVar6 * iVar24 - iVar7 * iVar3  (cos*delta_x - sin*delta_z)
     *   iVar6  = iVar6 * iVar3  + iVar7 * iVar24 (sin*delta_x + cos*delta_z)
     * Earlier port had BOTH sin terms inverted (CCW from +X / world rotated
     * by -yaw_a). That produced false bitmask=0x28 contacts on the Jarash
     * stationary spawn — exactly the visible-slide symptom in memory
     * `reference_obb_corner_test_rotation_sign`. Algorithmic re-validation
     * vs 501 captured original inputs (tools/validate_pool15_v2v_contact_math.py)
     * dropped bitmask divergence from 66.7% to 0% with the sign flip. */
    /* Rotate WORLD delta into A's local frame.
     * [CONFIRMED @ 0x00408570 CollectVehicleCollisionContacts]:
     *   local_dx = cos(A.yaw)*delta_x - sin(A.yaw)*delta_z
     *   local_dz = sin(A.yaw)*delta_x + cos(A.yaw)*delta_z
     * The game stores yaw in "CW from +Z" convention (matches AngleFromVector12
     * argument order (dz, dx)). The earlier port formula (cos*x+sin*z, -sin*x+cos*z)
     * was the standard math "CCW from +X" rotation and inverted slot 1's apparent
     * position in slot 0's frame. */
    int32_t local_dx = (delta_x * cos_a - delta_z * sin_a) >> 12;
    int32_t local_dz = (delta_x * sin_a + delta_z * cos_a) >> 12;

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
    /* World-space delta from B to A = negation of the A-to-B delta. The
     * listing's second half re-uses the same iVar3/iVar24 with sign-flipped
     * sin/cos terms (see iVar15 = sin_b*dz - cos_b*dx at 0x004088E0). */
    int32_t delta2_x = -delta_world_x;
    int32_t delta2_z = -delta_world_z;

    /* Rotate delta into B's local frame.
     * [CONFIRMED @ second half of 0x00408570]: symmetric sign convention
     * — same "CW from +Z" world→local form as the B-in-A half above. */
    /* Rotate WORLD delta into B's local frame — same "CW from +Z" convention. */
    int32_t local2_dx = (delta2_x * cos_b - delta2_z * sin_b) >> 12;
    int32_t local2_dz = (delta2_x * sin_b + delta2_z * cos_b) >> 12;

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

    /* Pool15 V2V pilot trace — capture outputs at exit. */
    {
        TD5_PilotV2VContactSnap _v2v_out;
        memset(&_v2v_out, 0, sizeof(_v2v_out));
        _v2v_out.bitmask = (uint32_t)result;
        for (int _i = 0; _i < 8; _i++) {
            _v2v_out.corner_proj_x[_i] = corners[_i].proj_x;
            _v2v_out.corner_proj_z[_i] = corners[_i].proj_z;
            _v2v_out.corner_own_x[_i]  = corners[_i].own_x;
            _v2v_out.corner_own_z[_i]  = corners[_i].own_z;
        }
        td5_pilot_v2v_contact_emit_leave(&_v2v_out);
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

/* Signed round-to-zero divide by 256 — matches the original's
 * [CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8] idiom used 12 times in the
 * TOI rollback/advance halves at 0x00407F31-0x004080A6 and 0x004080C8-0x0040816B.
 * For positive x: equivalent to x >> 8.
 * For negative x not divisible by 256: x >> 8 rounds toward -inf, this rounds toward 0. */
static inline int32_t v2v_sar8_rz(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* Signed round-to-zero divide by 4096 — matches the original's
 * [CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc] idiom used at the four
 * velocity-rotation sites in the prologue and the four velocity writeback
 * sites in the tail, plus twice at the impulse-scale step. */
static inline int32_t v2v_sar12_rz(int32_t x) {
    return ((x < 0) ? (x + 0xFFF) : x) >> 12;
}

/* 64-bit signed round-to-zero divide by 4096 — used at the impulse final
 * scale where (NUM_CONST / denom) * rel_vel can exceed 31 bits before the
 * >>12 truncation. */
static inline int32_t v2v_sar12_rz_64(int64_t x) {
    return (int32_t)(((x < 0) ? (x + 0xFFF) : x) >> 12);
}

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

    /* pool14_v2v pilot trace: capture pre-state. The Frida probe captures
     * args[0]=actorA (=slot_a=frame owner) and args[1]=actorB (=slot_b).
     * Port's A=target=caller's `a`=slot_a → maps to Frida's actorA. */
    td5_pilot_v2v_enter(A, B, corner, angle, impactForce);

    /* --- 1. Prologue: save angular velocities for delta application --- */
    int32_t saved_omega_A = A->angular_velocity_yaw;
    int32_t saved_omega_B = B->angular_velocity_yaw;

    /* Mass from cardef+0x88 (int16). [CONFIRMED @ 0x00407BE7, 0x00407BFE,
     * 0x00407DA0, 0x00407DB4]: original ApplyVehicleCollisionImpulse loads
     * mass via MOVSX with NO clamp. Upstream writers guarantee mass > 0
     * before V2V fires: (a) racing slots 0..5 load carparam.dat into
     * car_definition (positive int16); (b) traffic slots 6+ get an explicit
     * mass=0x20 write at InitializeRaceVehicleRuntime+0xF5 (`MOV word ptr
     * [EAX + 0x88], 0x20` @ 0x0042F235), mirrored in the port's
     * traffic-init path (td5_physics.c:8116-8118). The earlier port-only
     * `if (mass <= 0) mass = 0x20;` defensive clamp was never reachable
     * and diverged from the byte-faithful listing. Removed per
     * audit-v2v-mass-clamp 2026-05-14. */
    int32_t mass_A = (int32_t)CDEF_S(A, 0x88);
    int32_t mass_B = (int32_t)CDEF_S(B, 0x88);

    /* --- 2. Rotate both velocities into A's local (contact) frame --- */
    int32_t cos_a = cos_fixed12(angle);
    int32_t sin_a = sin_fixed12(angle);

    int32_t vxA = A->linear_velocity_x;
    int32_t vzA = A->linear_velocity_z;
    int32_t vxB = B->linear_velocity_x;
    int32_t vzB = B->linear_velocity_z;

    /* [CONFIRMED @ 0x00407A19-29, 0x00407A3E-4D, 0x00407A69-72, 0x00407A95-AA]:
     * each rotation step uses CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,0xc —
     * signed round-to-zero divide by 0x1000. Plain `>>12` on a negative product
     * diverges by 1 LSB. */
    int32_t local_54 = v2v_sar12_rz(vxA * cos_a - vzA * sin_a);  /* A tangent */
    int32_t local_50 = v2v_sar12_rz(vxA * sin_a + vzA * cos_a);  /* A normal  */
    int32_t local_4c = v2v_sar12_rz(vxB * cos_a - vzB * sin_a);  /* B tangent */
    int32_t local_44 = v2v_sar12_rz(vxB * sin_a + vzB * cos_a);  /* B normal  */

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
    /* [CONFIRMED @ 0x00407B0D-10 (front) + 0x00407B65-6E (rear)]: the listing
     * applies a CDQ; XOR; SUB abs() idiom to side_extent before comparing it
     * against |rear_diff| or |front_diff|. Without this abs, a corner outside
     * the box laterally (abs(cx_A) > half_w_A → side_extent < 0) would
     * unconditionally take the SIDE branch in the port while the original
     * would still pick FRONT/REAR based on |negative| vs depth magnitudes. */
    int32_t abs_side_extent = side_extent < 0 ? -side_extent : side_extent;

    int is_side_branch;
    if (cz_A < 1) {
        /* Rear half (cz_A <= 0). [CONFIRMED @ 0x00407B7B-7D]:
         *   CMP ECX, EAX ; ECX=|side_extent|, EAX=|cz_A - rear_z_raw|
         *   JGE LAB_00407B2D (FRONT/REAR) — SIDE if |side_extent| < rear_depth. */
        int32_t rear_depth = cz_A - rear_z_A;
        if (rear_depth < 0) rear_depth = -rear_depth;
        is_side_branch = (rear_depth > abs_side_extent);
    } else {
        /* Front half (cz_A > 0). [CONFIRMED @ 0x00407B29-2B]:
         *   CMP ECX, EAX ; ECX=|side_extent|, EAX=|front_z_A - cz_A|
         *   JL  LAB_00407B7F (SIDE) — SIDE if |side_extent| < front_depth. */
        int32_t front_depth = front_z_A - cz_A;
        if (front_depth < 0) front_depth = -front_depth;
        is_side_branch = (abs_side_extent < front_depth);
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
        /* [CONFIRMED @ 0x00407B7F-0x00407BB5]: predicate is
         *     cx_A == cx_B || cx_A - cx_B < 0   (i.e. cx_A <= cx_B)
         * iVar6 holds the cos channel, iVar14 holds the sin channel.
         * Push writes at 0x00407BB7-0x00407BDB are A.x -= iVar6, A.z -= iVar14,
         * B.x += iVar6, B.z += iVar14 (see apply block below).
         * push_x mirrors iVar6, push_z mirrors iVar14. */
        if (cx_A <= cx_B) { push_x = -cos_a / 2; push_z =  sin_a / 2; }
        else              { push_x =  cos_a / 2; push_z = -sin_a / 2; }

        int64_t denom = ((int64_t)cz_B * cz_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cz_A * cz_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cz_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cz_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_54 + local_4c;

        /* [CONFIRMED @ 0x00407CB2-C1]: CDQ; AND EDX,0xfff; ADD; SAR 0xc —
         * signed round-to-zero divide by 0x1000. */
        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = v2v_sar12_rz_64(impulse_raw);

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
        /* [CONFIRMED @ 0x00407B41 (predicate JLE), 0x00407B47-0x00407B54
         *  (else: cz_A > cz_B → +sin/2, +cos/2),
         *  0x00407D5F-0x00407D6E (taken: cz_A <= cz_B → NEG; NEG → -sin/2, -cos/2),
         *  0x00407D70-0x00407D94 (apply A -= ECX/EAX, B += ECX/EAX)]:
         *
         * Predicate is `cz_A == cz_B || cz_A - cz_B < 0` (i.e. cz_A <= cz_B).
         * In ASM: ECX holds the iVar14 sin channel (loaded from [ESP+0x7c]=sin_a),
         *         EAX holds the iVar6 cos channel (loaded from EDI=cos_a, where
         *         EDI was set by CosFixed12bit @ 0x004079EB and never reloaded
         *         through the FRONT path).
         * Push writes at 0x00407D70-0x00407D94 are A.x -= iVar14 (sin channel),
         * A.z -= iVar6 (cos channel), B.x += iVar14, B.z += iVar6.
         * The /2 idiom (SUB EAX,EDX after CDQ; SAR 1) is signed
         * round-toward-zero division by 2, which equals plain C `/2` for int32
         * (C99/C11 truncation toward zero).
         * Precise-port audit 2026-05-14 re-verified against decomp
         * ApplyVehicleCollisionImpulse and listing — already byte-faithful. */
        if (cz_A <= cz_B) { push_x = -sin_a / 2; push_z = -cos_a / 2; }
        else              { push_x =  sin_a / 2; push_z =  cos_a / 2; }

        /* [CONFIRMED FRONT impulse/omega vs decomp ApplyVehicleCollisionImpulse]:
         *   denom = (cx_B^2 + K) * mass_A + (cx_A^2 + K) * mass_B
         *   NUM_CONST / (denom >> 8) * rel_vel  →  sar12_rz → impulse
         *   reject if  (cz_B - cz_A) ^ impulse < 0   (XOR sign mismatch)
         *   local_50 += impulse * mass_A
         *   local_44 -= impulse * mass_B
         *   omega_A_delta = -(imp * mass_A * cx_A) / (K / 0x28C)   [iVar6 in decomp]
         *   omega_B_delta =  (imp * mass_B * cx_B) / (K / 0x28C)   [iVar8 in decomp]
         * Note the omega signs are FLIPPED vs SIDE: SIDE has +A/-B, FRONT has -A/+B.
         * V2V_INERTIA_PER_ANG = 500000/0x28C = 766 (compile-time fold of the
         * runtime magic-divide DAT_00463204 / 0x28C at 0x00407EC4-D7). */
        int64_t denom = ((int64_t)cx_B * cx_B + INERTIA_K_64) * mass_A
                      + ((int64_t)cx_A * cx_A + INERTIA_K_64) * mass_B;
        denom >>= 8;
        if (denom == 0) denom = 1;

        int32_t ang_contrib =
            (int32_t)(((int64_t)cx_B * saved_omega_B) / V2V_ANG_DIVISOR) -
            (int32_t)(((int64_t)cx_A * saved_omega_A) / V2V_ANG_DIVISOR);
        int32_t rel_vel = ang_contrib - local_50 + local_44;

        /* [CONFIRMED @ 0x00407E68-7A]: same round-to-zero idiom as SIDE branch. */
        int64_t impulse_raw = (NUM_CONST / denom) * rel_vel;
        impulse = v2v_sar12_rz_64(impulse_raw);

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
     * [CONFIRMED @ 0x00407BB7-0x00407BDB (SIDE) + 0x00407D70-0x00407D94 (FRONT)]:
     *     A.x -= iVar6/iVar14;  A.z -= iVar14/iVar6;
     *     B.x += iVar6/iVar14;  B.z += iVar14/iVar6;
     * push_x/push_z carry the iVar sign per the per-branch assignments above
     * (cosmetic line-for-line match to the decomp; supersedes the earlier
     * algebraically-equivalent A+=, B-= encoding). The XOR rejection below
     * only gates the velocity impulse, not the push. */
    A->world_pos.x -= push_x;
    A->world_pos.z -= push_z;
    B->world_pos.x += push_x;
    B->world_pos.z += push_z;

    if (rejected) {
        /* Separating contact — push applied above, but no velocity impulse.
         * Original returns 0 (XOR EAX,EAX at 0x00407CCD / 0x00407E86). */
        TD5_LOG_I(LOG_TAG, "v2v_reject: slot_A=%d slot_B=%d side=%d cxA=%d czA=%d cxB=%d czB=%d imp=%d push=(%d,%d)",
                  A->slot_index, B->slot_index, is_side_branch, cx_A, cz_A, cx_B, cz_B, impulse, push_x, push_z);
        td5_pilot_v2v_leave(A, B, 0);
        return;
    }

    /* --- 5. TOI rollback (before committing new velocities) ---
     * [CONFIRMED @ 0x00407F31-F4C (A.x), F50-6E (A.z), F6F-8E (B.x),
     *  F8F-AE (B.z), FAF-D6 (A.yaw_eul), FDE-FFD (B.yaw_eul)]:
     * each rollback uses IMUL; CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8; NEG;
     * ADD [field], EAX — i.e. `field += -sar8_rz(toi_frac * vel)` which equals
     * `field -= sar8_rz(toi_frac * vel)`. Plain `>>8` on a negative product
     * diverges by 1 LSB. */
    int32_t toi_frac = 0x100 - impactForce;

    A->world_pos.x -= v2v_sar8_rz(toi_frac * A->linear_velocity_x);
    A->world_pos.z -= v2v_sar8_rz(toi_frac * A->linear_velocity_z);
    A->euler_accum.yaw -= v2v_sar8_rz(toi_frac * A->angular_velocity_yaw);
    B->world_pos.x -= v2v_sar8_rz(toi_frac * B->linear_velocity_x);
    B->world_pos.z -= v2v_sar8_rz(toi_frac * B->linear_velocity_z);
    B->euler_accum.yaw -= v2v_sar8_rz(toi_frac * B->angular_velocity_yaw);

    /* --- 6. Commit new angular velocities --- */
    A->angular_velocity_yaw = saved_omega_A + omega_A_delta;
    B->angular_velocity_yaw = saved_omega_B + omega_B_delta;

    /* --- 7. Rotate tangent/normal channels back to world frame ---
     * [CONFIRMED @ 0x00408027-37, 0x00408048-58, 0x00408068-78, 0x00408088-98]:
     * each writeback uses the same CDQ; AND EDX,0xfff; SAR 0xc idiom — signed
     * round-to-zero divide by 0x1000. */
    A->linear_velocity_x = v2v_sar12_rz(local_50 * sin_a + local_54 * cos_a);
    A->linear_velocity_z = v2v_sar12_rz(local_50 * cos_a - local_54 * sin_a);
    B->linear_velocity_x = v2v_sar12_rz(local_44 * sin_a + local_4c * cos_a);
    B->linear_velocity_z = v2v_sar12_rz(local_44 * cos_a - local_4c * sin_a);

    /* --- 8. TOI re-advance (with the new post-impulse velocities) ---
     * [CONFIRMED @ 0x004080C8 onwards]: same round-to-zero idiom but the NEG
     * disappears so `field += sar8_rz(toi_frac * vel)`. */
    A->world_pos.x += v2v_sar8_rz(toi_frac * A->linear_velocity_x);
    A->world_pos.z += v2v_sar8_rz(toi_frac * A->linear_velocity_z);
    A->euler_accum.yaw += v2v_sar8_rz(toi_frac * A->angular_velocity_yaw);
    B->world_pos.x += v2v_sar8_rz(toi_frac * B->linear_velocity_x);
    B->world_pos.z += v2v_sar8_rz(toi_frac * B->linear_velocity_z);
    B->euler_accum.yaw += v2v_sar8_rz(toi_frac * B->angular_velocity_yaw);

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

    /* Wanted mode (cop chase): player<->cop collision awards damage score.
     * Mirrors ApplyVehicleCollisionImpulse @ 0x40817A/0x4081BE → AwardWantedDamageScore.
     * [CONFIRMED]: both A-is-player and B-is-player paths call AwardWantedDamageScore
     * on the cop slot with impact magnitude. */
    if (td5_game_is_wanted_mode()) {
        if (A->slot_index == 0 && B->slot_index >= 1 && B->slot_index < 6)
            td5_ai_wanted_cop_hit(B->slot_index, impact_mag);
        else if (B->slot_index == 0 && A->slot_index >= 1 && A->slot_index < 6)
            td5_ai_wanted_cop_hit(A->slot_index, impact_mag);
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

    /* Heavy impact (> 90000) with collisions enabled: scatter kick.
     * [CONFIRMED @ 0x4082F2, 0x4082FC, 0x40844A, 0x408455]: original
     * ApplyVehicleCollisionImpulse high-impact branch writes
     *   actor->angular_velocity_{roll,pitch,yaw} += RNG % angle - angle/2
     * (RNG via GetDamageRulesStub modulo angle). Port previously wrote
     * (scatter>>2 - scatter>>3) into euler_accum.{roll,pitch}, which
     * jumps the angle accumulator directly — bypassing the per-pattern
     * ±4000 clamp at the next suspension_response tail and integrating
     * uncontrollably across consecutive impacts. Switch to writing
     * angular_velocity instead, so the integrator's clamps apply.
     *
     * Gate variable mismatch noted but not fixed: original gates on
     * g_cameraMode==0 (normal play); port has no g_cameraMode equivalent
     * yet so the gate stays as the inverted g_collisions_enabled==0
     * condition until a faithful camera-mode flag is introduced. */
    if (impact_mag > 90000 && g_collisions_enabled == 0) {
        int32_t scatter = impact_mag / 4;
        if (scatter > 0x7FFF) scatter = 0x7FFF;
        int32_t kick_r = (scatter >> 2) - (scatter >> 3);
        int32_t kick_p = (scatter >> 1) - scatter;
        int32_t kick_y = (scatter >> 1) - (scatter >> 2);
        if (A->slot_index < 6) {
            A->angular_velocity_roll  += kick_r;
            A->angular_velocity_pitch += kick_p;
            A->angular_velocity_yaw   += kick_y;
            int32_t lift_a = impact_mag / 6;
            if (lift_a > 200000) lift_a = 200000;
            A->linear_velocity_y  = lift_a;
        }
        if (B->slot_index < 6) {
            B->angular_velocity_roll  -= kick_r;
            B->angular_velocity_pitch -= kick_p;
            B->angular_velocity_yaw   -= kick_y;
            int32_t lift_b = impact_mag / 6;
            if (lift_b > 200000) lift_b = 200000;
            B->linear_velocity_y  = lift_b;
        }
        TD5_LOG_I(LOG_TAG, "v2v_heavy_scatter: A=%d B=%d mag=%d kick_r=%d kick_p=%d kick_y=%d",
                  A->slot_index, B->slot_index, impact_mag, kick_r, kick_p, kick_y);
    }

    /* pool14_v2v pilot trace: capture post-state at function exit.
     * Original returns int impact_mag at 0x004084A2 RET. */
    td5_pilot_v2v_leave(A, B, impact_mag);
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

    /* Faithful port of ResolveSimpleActorSeparation @ 0x00408F70
     * [CONFIRMED 2026-05-12 via Ghidra decomp]:
     *   iVar6 = (nx_12bit * v_proj_16) / 32
     *   A->vel += iVar6;  B->vel -= iVar6;
     * No mass divide in orig. Operand order is multiply-then-divide so the
     * small projection magnitude doesn't truncate to zero.
     *
     * Previous port did `impulse = -v_dot >> 5` FIRST (truncating tiny
     * closing-velocity projections to 0 or -1), then `imp_x = (impulse * nx)
     * >> 12`, then divided by mass. That made the impulse ~260x weaker than
     * orig at typical race speeds (orig: ~6 wu/tick separation; port: 0.023
     * wu/tick). Cars in continuous grounded contact (slot 0+slot 4 logged
     * 31k V2V events on Moscow) never decelerated enough to separate.
     *
     * Scale match: orig stores rel_v as int16 (vel>>8). Port keeps 24.8 FP.
     * v_dot_port = v_dot_orig * 256, so divide by 8192 (instead of orig's 32)
     * to land on the same 24.8 FP delta magnitude. */
    int32_t impulse_scalar = -v_dot;  /* positive when closing, in 24.8 FP */
    int32_t delta_x = (nx * impulse_scalar) >> 13;
    int32_t delta_y = (ny * impulse_scalar) >> 13;
    int32_t delta_z = (nz * impulse_scalar) >> 13;

    a->linear_velocity_x += delta_x;
    a->linear_velocity_y += delta_y;
    a->linear_velocity_z += delta_z;
    b->linear_velocity_x -= delta_x;
    b->linear_velocity_y -= delta_y;
    b->linear_velocity_z -= delta_z;
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

    /* Pool15 V2V pilot trace — reset corner-test call counter so the next
     * 1+7 obb_corner_test calls inside this function get event_idx 1..8. */
    td5_pilot_v2v_reset_call_idx(idx_a, idx_b);
    TD5_PilotV2VToiSnap _v2v_toi;
    memset(&_v2v_toi, 0, sizeof(_v2v_toi));
    _v2v_toi.actor_a_addr = (uint32_t)(uintptr_t)a;
    _v2v_toi.actor_b_addr = (uint32_t)(uintptr_t)b;
    _v2v_toi.ax = a->world_pos.x;
    _v2v_toi.az = a->world_pos.z;
    _v2v_toi.bx = b->world_pos.x;
    _v2v_toi.bz = b->world_pos.z;
    _v2v_toi.yaw_a_raw = a->euler_accum.yaw;
    _v2v_toi.yaw_b_raw = b->euler_accum.yaw;
    _v2v_toi.lin_vel_a_x = a->linear_velocity_x;
    _v2v_toi.lin_vel_a_z = a->linear_velocity_z;
    _v2v_toi.lin_vel_b_x = b->linear_velocity_x;
    _v2v_toi.lin_vel_b_z = b->linear_velocity_z;
    _v2v_toi.ang_vel_a_yaw = a->angular_velocity_yaw;
    _v2v_toi.ang_vel_b_yaw = b->angular_velocity_yaw;

    /* AABB pre-test from broadphase grid.
     * [CONFIRMED @ 0x00408A92-0x00408AB7]: original uses JGE/JLE — i.e.
     * `<=` / `>=` predicates. At exact equality the original rejects;
     * port previously used `<` and admitted the pair. One-LSB over-
     * approximation closed by switching `<` to `<=`. */
    if (g_actor_aabb[idx_a][2] <= g_actor_aabb[idx_b][0] ||
        g_actor_aabb[idx_b][2] <= g_actor_aabb[idx_a][0] ||
        g_actor_aabb[idx_a][3] <= g_actor_aabb[idx_b][1] ||
        g_actor_aabb[idx_b][3] <= g_actor_aabb[idx_a][1]) {
        return;
    }

    /* [precise-00408A60 byte-faithful port 2026-05-14]
     *
     * The original ResolveVehicleCollisionPair (0x00408A60) keeps the
     * bisection accumulators in RAW 24.8 fixed-point throughout — the
     * `>> 8` conversion to display units only happens INSIDE
     * CollectVehicleCollisionContacts at 0x00408570 and at the final
     * dispatch push (0x00408D58 SAR ECX,8 / 0x00408DD4 SAR EDX,8).
     *
     * Listing 0x00408AED-0x00408B23 reads the SIX accumulator seeds via
     * DWORD MOV directly out of the actor structs at full 24.8 scale:
     *   local_80 / [ESP+0x2C] = actor_A.world_pos.x  (+0x1FC)
     *   local_7c / [ESP+0x30] = actor_A.world_pos.y  (+0x200) (read but unused)
     *   local_78 / [ESP+0x34] = actor_A.world_pos.z  (+0x204)
     *   local_94 / [ESP+0x1C] = actor_A.euler_accum.yaw (+0x1F4)
     *   local_74..local_6c  = actor_B mirror (B.x/y/z @ +0x1FC..+0x204)
     *   local_90 / [ESP+0xa8 or saved] = actor_B.euler_accum.yaw
     *
     * Per-iter halving uses the CDQ/SUB/SAR pattern (0x00408B6F-0x00408BB1
     * pre-loop, 0x00408C10-0x00408C56 in-loop) which is C-style signed
     * divide-by-2 (truncate-toward-zero) — different from `>>= 1` on
     * negative odd values by 1 LSB. We use `/= 2` to match exactly.
     *
     * Bisection accumulator `local_84` (sum of ±local_8c) is the analog of
     * the port's old `frac`; impactForce = local_84 - 0x10. */

    /* Pre-load raw 24.8 accumulators from actor (listing 0x00408AED-0x00408B23). */
    int32_t pos_a_x = a->world_pos.x;
    int32_t pos_a_z = a->world_pos.z;
    int32_t pos_b_x = b->world_pos.x;
    int32_t pos_b_z = b->world_pos.z;
    int32_t yaw_a_acc = a->euler_accum.yaw;
    int32_t yaw_b_acc = b->euler_accum.yaw;

    /* Initial-state display-unit headings (used only for the diagnostic log
     * below; mirrors the prior heading_a/heading_b labels). */
    int32_t initial_heading_a = (yaw_a_acc >> 8) & 0xFFF;
    int32_t initial_heading_b = (yaw_b_acc >> 8) & 0xFFF;

    /* Initial OBB test at full (end-of-tick) position.
     * [CONFIRMED @ 0x00408B27-B48]: CollectVehicleCollisionContacts is
     * invoked with raw 24.8-fp position triplets and raw 24.8-fp euler
     * accumulators. The callee does (pos_b - pos_a) >> 8 and yaw_acc >> 8
     * internally — see the wrapper header comment. Pass raw, matching the
     * listing exactly. */
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));
    int bitmask = obb_corner_test(a, b,
                                  pos_a_x, pos_a_z, pos_b_x, pos_b_z,
                                  yaw_a_acc, yaw_b_acc, corners);

    if (bitmask == 0) return;  /* No overlap at full size — early-return. */

    /* --- 7-iteration TOI binary search [listing 0x00408B5C-0x00408D23] ---
     *
     * Per-axis half-step accumulators (initial = velocity/2 via CDQ/SUB/SAR
     * at 0x00408B6F-0x00408BB1). These are raw 24.8 fp values; integer
     * signed division by 2 (truncate-toward-zero) matches the asm pattern.
     *
     * Pre-loop subtraction (listing 0x00408BB5-0x00408BF5) walks the
     * positions/headings BACK to the t=0.5 midpoint before the loop:
     *   local_80 -= iVar12; local_78 -= iVar13; local_94 -= iVar9
     *   local_74 -= iVar11; local_6c -= local_4; local_90 -= local_5c
     *
     * local_84 = local_8c = 0x80 (initial centre + initial step).
     * local_68 = 7 (loop counter). */
    int32_t step_ax = a->linear_velocity_x   / 2;  /* iVar12 */
    int32_t step_az = a->linear_velocity_z   / 2;  /* iVar13 */
    int32_t step_ah = a->angular_velocity_yaw / 2; /* iVar9 — yaw step */
    int32_t step_bx = b->linear_velocity_x   / 2;  /* iVar11 */
    int32_t step_bz = b->linear_velocity_z   / 2;  /* local_4 */
    int32_t step_bh = b->angular_velocity_yaw / 2; /* local_5c — B yaw step */

    pos_a_x -= step_ax;
    pos_a_z -= step_az;
    pos_b_x -= step_bx;
    pos_b_z -= step_bz;
    yaw_a_acc -= step_ah;
    yaw_b_acc -= step_bh;

    int32_t local_84  = 0x80;     /* bisection accumulator (-> impactForce) */
    int32_t local_8c  = 0x80;     /* current half-step magnitude (sign chosen per-iter) */

    /* Cache last-hit bitmask + corner data. Seeded from the pre-loop test
     * (listing 0x00408B52 MOV [ESP+0x24],EAX). Loop updates it ONLY on
     * the bitmask!=0 branch (listing 0x00408CD0). */
    int32_t        cached_bitmask = bitmask;
    OBB_CornerData cached_corners[8];
    memcpy(cached_corners, corners, sizeof(cached_corners));

    /* Display-unit heading snapshot for the dispatch path. Updated each
     * iter from yaw_a_acc/yaw_b_acc after the wrapper call. */
    int32_t test_ha = yaw_a_acc >> 8;
    int32_t test_hb = yaw_b_acc >> 8;

    for (int iter = 0; iter < 7; iter++) {
        /* Per-iter halving [listing 0x00408C0E-0x00408C56]:
         * each step accumulator is signed-divided by 2 (CDQ/SUB/SAR pattern).
         * local_8c is also signed-/2 — `(int)local_8c / 2` in the decompiler. */
        step_ax  /= 2;
        step_az  /= 2;
        step_ah  /= 2;
        step_bx  /= 2;
        step_bz  /= 2;
        step_bh  /= 2;
        local_8c /= 2;

        /* Pass current raw 24.8 accumulators to the wrapper, matching the
         * listing exactly — callee owns the >>8 shift on delta/yaw. */
        test_ha = yaw_a_acc >> 8;  /* snapshot for dispatch logging */
        test_hb = yaw_b_acc >> 8;

        memset(corners, 0, sizeof(corners));
        int32_t result = obb_corner_test(a, b,
                                         pos_a_x, pos_a_z, pos_b_x, pos_b_z,
                                         yaw_a_acc, yaw_b_acc, corners);

        /* Default direction (uVar5 == 0 / miss path, listing 0x00408CD6-0x00408D1F):
         *   move FORWARD in time — add the current half-step to positions
         *   and local_84 (uVar10 = local_8c, iVar1=iVar12, etc.).
         * Hit path (uVar5 != 0, listing 0x00408C83-0x00408CD0):
         *   negate all step deltas (uVar10 = -local_8c, iVar1 = -iVar12...),
         *   cache the bitmask, and apply — which steps BACK in time. */
        int32_t dir_ax = step_ax;
        int32_t dir_az = step_az;
        int32_t dir_ah = step_ah;
        int32_t dir_bx = step_bx;
        int32_t dir_bz = step_bz;
        int32_t dir_bh = step_bh;
        int32_t dir_8c = local_8c;
        if (result != 0) {
            cached_bitmask = result;
            memcpy(cached_corners, corners, sizeof(cached_corners));
            dir_ax = -step_ax;
            dir_az = -step_az;
            dir_ah = -step_ah;
            dir_bx = -step_bx;
            dir_bz = -step_bz;
            dir_bh = -step_bh;
            dir_8c = -local_8c;
        }

        pos_a_x  += dir_ax;
        pos_a_z  += dir_az;
        pos_b_x  += dir_bx;
        pos_b_z  += dir_bz;
        yaw_a_acc += dir_ah;
        yaw_b_acc += dir_bh;
        local_84  += dir_8c;
    }

    /* Post-loop: refresh display-unit yaws from the FINAL raw accumulators
     * for the dispatch impulse calls (listing 0x00408D58 SAR ECX,8 reads
     * local_94 — the raw accumulator — and shifts; ditto 0x00408DD4 for
     * local_90 with B's yaw). Without this the dispatch yaw would be stale
     * from the LAST iteration's read instead of from the FINAL post-update
     * accumulator. The original matches because it re-reads local_94/90
     * at the dispatch site after the loop has updated them. */
    test_ha = yaw_a_acc >> 8;
    test_hb = yaw_b_acc >> 8;

    /* impactForce = local_84 - 0x10 [listing 0x00408D34 SUB ESI,0x10].
     * Range after 7 iters from 0x80: [-0x10, 0xEF] (no clamp). */
    int32_t impactForce = local_84 - 0x10;

    /* Pool15 V2V pilot trace — emit toi-row at dispatch boundary. */
    _v2v_toi.impactForce = impactForce;
    _v2v_toi.dispatched_bitmask = (uint32_t)cached_bitmask;
    _v2v_toi.final_yaw_a_disp = test_ha;
    _v2v_toi.final_yaw_b_disp = test_hb;
    _v2v_toi.final_ax = pos_a_x >> 8;
    _v2v_toi.final_az = pos_a_z >> 8;
    _v2v_toi.final_bx = pos_b_x >> 8;
    _v2v_toi.final_bz = pos_b_z >> 8;
    td5_pilot_v2v_toi_emit(&_v2v_toi, idx_a, idx_b);

    /* Dispatch uses the cached last-non-zero bitmask + corners, NOT a final
     * re-test. `test_ha` / `test_hb` carry the final-state yaws (in 12-bit
     * units, possibly slightly outside [0,0xFFF] — handled by cos_fixed12/
     * sin_fixed12's internal mask) because the original's dispatch push uses
     * `local_94 >> 8` / `local_90 >> 8` which are the bisection accumulators'
     * final values [CONFIRMED @ 0x00408D58 SAR ECX,8; 0x00408DD4].
     * Final-state yaw sits within ±(last bisect_step) of the last-hit yaw. */
    TD5_LOG_I(LOG_TAG, "v2v_bisect: slotA=%d slotB=%d local_84=0x%02X impactForce=%d bitmask=0x%02X bisHA=0x%03X bisHB=0x%03X rawHA=0x%03X rawHB=0x%03X",
              a->slot_index, b->slot_index, local_84, impactForce, cached_bitmask,
              test_ha, test_hb, initial_heading_a, initial_heading_b);

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

    /* [precise-00409150 D10 2026-05-14]
     * Listing 0x00409150-0x00409168 + 0x004091C9-CB + 0x004091F0:
     *   MOV EDX, [0x004aaf00]   ; g_racerCount (NOT total_actor_count)
     *   TEST EDX, EDX / JLE end
     *
     * Original iterates `g_racerCount` (count of racing vehicles only,
     * excludes traffic). Port previously iterated total actor count
     * (racers + traffic), causing traffic actors to be broadphase-included
     * in this function. Traffic V2V is handled separately in
     * UpdateTrafficActorMotion in the original — NOT here.
     *
     * Faithful behavior: iterate only g_racer_count slots. */
    total = g_racer_count;
    if (total < 2) {
        return;
    }
    if (total > TD5_MAX_TOTAL_ACTORS) {
        total = TD5_MAX_TOTAL_ACTORS;
    }

#ifdef TD5_PILOT_TRACE_00409150
    td5_pilot_trace_00409150_enter(total);
    int pilot_pair_idx = 0;
#endif

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

#ifdef TD5_PILOT_TRACE_00409150
        td5_pilot_trace_00409150_phase1(i,
                                        actor->world_pos.x >> 8,
                                        actor->world_pos.z >> 8,
                                        radius,
                                        actor->track_span_normalized,
                                        bucket,
                                        g_actor_aabb[i][4],
                                        g_actor_aabb[i][0],
                                        g_actor_aabb[i][1],
                                        g_actor_aabb[i][2],
                                        g_actor_aabb[i][3]);
#endif
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
                                 (a->wheel_contact_bitmask >= 0x0F);
                int b_scripted = (b->vehicle_mode != 0) ||
                                 (b->wheel_contact_bitmask >= 0x0F);

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

#ifdef TD5_PILOT_TRACE_00409150
                td5_pilot_trace_00409150_pair(i, j,
                                              boff, walk_count - 1,
                                              (a_scripted || b_scripted) ? 1 : 0,
                                              a->vehicle_mode, a->wheel_contact_bitmask,
                                              b->vehicle_mode, b->wheel_contact_bitmask,
                                              pilot_pair_idx++);
#endif
                if (a_scripted || b_scripted) {
                    collision_detect_simple(a, b);
                } else {
                    collision_detect_full(a, b, i, j);
                }

                chain = g_actor_aabb[j][4] & 0xFF;
            }
        }
    }

#ifdef TD5_PILOT_TRACE_00409150
    td5_pilot_trace_00409150_leave(pilot_pair_idx);
#endif

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
                     (a->wheel_contact_bitmask >= 0x0F);
    int b_scripted = (b->vehicle_mode != 0) ||
                     (b->wheel_contact_bitmask >= 0x0F);

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

/* pool5 / 0x00403A20 pilot trace hooks — header included separately to keep
 * the function signature unchanged. */
extern void td5_pilot_emit_00403A20_enter(const TD5_Actor *actor, int32_t accel_x, int32_t accel_z, uintptr_t caller_ra);
extern void td5_pilot_emit_00403A20_leave(const TD5_Actor *actor);

/* Round-to-zero divide by 2 for signed int32. Mirrors the central-pass
 * `CDQ; SUB EAX,EDX; SAR EAX,1` idiom at 0x00403B78..B95.
 *   CDQ            ; EDX = sign(EAX) = -1 if neg, 0 if non-neg
 *   SUB EAX, EDX   ; subtract -1 (=add 1) if negative, no-op if non-neg
 *   SAR EAX, 1
 */
static inline int32_t sar1_rz(int32_t x)
{
    int32_t bias = (x < 0) ? -1 : 0;
    return (x - bias) >> 1;     /* (x - (-1)) = x + 1 when negative */
}

void td5_physics_integrate_suspension(TD5_Actor *actor, int32_t accel_x, int32_t accel_z)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    td5_pilot_emit_00403A20_enter(actor, accel_x, accel_z, (uintptr_t)__builtin_return_address(0));

    /* cardef constants -- see function header.
     * Names chosen to match the original's semantic use. */
    const int32_t k_pos_damp   = (int32_t)PHYS_S(actor, 0x5E);  /* position-proportional damping (restoring) */
    const int32_t k_vel_damp   = (int32_t)PHYS_S(actor, 0x60);  /* velocity-proportional damping */
    const int32_t k_spring     = (int32_t)PHYS_S(actor, 0x62);  /* spring coefficient (multiplies lever proj) */
    const int32_t k_travel_lim = (int32_t)PHYS_S(actor, 0x64);  /* per-wheel +/- travel clamp */
    const int32_t k_load_scale = (int32_t)PHYS_S(actor, 0x66);  /* multiplier on wheel_load_accum */

    /* world_pos >> 8 with the original's round-to-zero bias (0x00403A7D-A85
     * and the symmetric one at 0x00403A9C-AA0). Plain SAR rounds toward -∞
     * for negatives. */
    const int32_t wpx_scaled = sar8_rz(actor->world_pos.x);
    const int32_t wpz_scaled = sar8_rz(actor->world_pos.z);

    /* ---- Per-wheel pass (4 wheels) ---- */
    for (int i = 0; i < 4; i++) {
        /* Lever arm from chassis centre to this wheel's contact position.
         * Original (0x00403A7D-A88):
         *   arm = hires - sar8_rz(world_pos)
         * The original's hires is stored in raw world units (the 0x00403720
         * write path does NOT apply the `<<8` that wheel_contact_pos gets —
         * see decomp of 0x00403720: the shifts only touch piVar1=+0xF0).
         *
         * Port storage divergence (UPSTREAM, owned by pool1/0x00403720):
         * the port stores wheel_world_positions_hires in 24.8 FP (the write
         * at td5_physics.c:5580 applies `hub_x << 8`). vfx consumes that
         * 24.8 FP scale at td5_vfx.c:1367.
         *
         * To make the integrator byte-exact under the port's CURRENT hires
         * convention, we shift the port's 24.8 FP hires down to world units
         * with `sar8_rz`. If pool1 later changes hires storage to raw (matching
         * the original's 0x00403720 path), this `sar8_rz` becomes a no-op and
         * must be removed.
         *
         * Why sar8_rz and not plain `>>8`: the port writes hires as
         * `int32_t hub_x = lrintf(...); hires.x = hub_x << 8`. For positive
         * hub_x, `hires.x >> 8 == hub_x` exactly. For negative hub_x not
         * divisible by 256... actually `hub_x << 8` produces a value with the
         * low 8 bits zero, so `hires.x >> 8 == hub_x` for both signs. So plain
         * `>>8` would also recover hub_x exactly. BUT to match the original's
         * arithmetic semantics on whatever values the port writes (in case
         * the port's hires write later changes), use the round-to-zero shift
         * which matches the original's `(x + sign_bias) >> 8` convention. */
        /* Hires now stored in raw world units (the <<8 was removed in
         * 6595-6597, matching original 0x00403720 semantics). sar8_rz()
         * is no longer needed here -- consume the value directly. */
        const int32_t arm_x = actor->wheel_world_positions_hires[i].x - wpx_scaled;
        const int32_t arm_z = actor->wheel_world_positions_hires[i].z - wpz_scaled;

        /* proj = arm_x * accel_x + arm_z * accel_z (computed as
         * arm_z*accel_z then ADD arm_x*accel_x in the listing — order
         * doesn't matter for signed 32-bit add). */
        int32_t proj = arm_x * accel_x + arm_z * accel_z;

        /* spring_term = sar8_rz(proj) * k_spring
         * [0x00403AAC-BA: MOV EAX,ECX; CDQ; AND EDX,0xFF; ADD EAX,EDX;
         *                 SAR EAX,8; IMUL EAX, k_spring]
         * Note: the result is held as the multiplied (not yet shifted)
         * value; the next SAR happens later. */
        const int32_t spring_term_x256 = sar8_rz(proj) * k_spring;

        /* load_term = wheel_load_accum[i] * k_load_scale
         * [0x00403ACA-CD: MOV EAX,[ESI+0x20]; IMUL EAX, k_load_scale]
         * No pre-shift; the SAR comes after. */
        const int32_t load_term_x256 = actor->wheel_load_accum[i] * k_load_scale;

        /* new_vel = sar8_rz(load_term) + sar8_rz(spring_term) + wheel_spring_dv[i]
         * [0x00403AD2-E3: CDQ; AND EDX,0xFF; ADD; SAR EAX,8 (for load_term)
         *                 SAR ECX,8 (for spring_term — bias was added at 0x00403ABF)
         *                 ADD ECX,EAX; ADD ECX,[ESI+0x10]]
         *
         * Critical: the order of the SAR-8s in the listing applies the bias
         * to spring_term BEFORE the load_term computation overwrites the bias
         * register. To be byte-faithful, we sar8_rz each operand separately.
         * Algebraically equivalent because round-to-zero is per-operand. */
        int32_t new_vel = sar8_rz(load_term_x256)
                        + sar8_rz(spring_term_x256)
                        + actor->wheel_spring_dv[i];

        /* pos_damp = sar8_rz(wheel_suspension_pos[i] * k_pos_damp)
         * [0x00403AE6-F6: MOV EAX,[ESI]; IMUL EAX, k_pos_damp;
         *                 CDQ; AND EDX,0xFF; ADD EAX,EDX; (held in EBP)
         *                 ...; SAR EBP,8]
         *
         * vel_damp = sar8_rz(new_vel * k_vel_damp)
         * [0x00403AF8-08: MOV EAX,ECX; IMUL EAX, k_vel_damp;
         *                 CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8]
         *
         * new_vel -= vel_damp + pos_damp     [0x00403B0E-12: NEG EAX; SUB
         *                                     EAX,EBP; ADD ECX,EAX]
         */
        const int32_t pos_damp_x256 = actor->wheel_suspension_pos[i] * k_pos_damp;
        const int32_t vel_damp_x256 = new_vel * k_vel_damp;
        new_vel = new_vel - sar8_rz(pos_damp_x256) - sar8_rz(vel_damp_x256);

        /* Deadzone: |new_vel| < 0x10 → 0
         * [0x00403B14-1E: CMP -0x10; JLE; CMP 0x10; JGE; XOR ECX,ECX]
         * Strict inequality: -0x10 < new_vel && new_vel < 0x10. Both
         * jumps are signed-LE/GE; CMP -0x10 + JLE jumps when new_vel <= -0x10
         * (i.e. stays non-zero), CMP 0x10 + JGE jumps when new_vel >= 0x10. */
        if (new_vel > -0x10 && new_vel < 0x10)
            new_vel = 0;

        /* Write velocity then position; clamp at +k_travel_lim then -k_travel_lim.
         * [0x00403B24-54] */
        actor->wheel_spring_dv[i] = new_vel;
        int32_t new_pos = actor->wheel_suspension_pos[i] + new_vel;
        actor->wheel_suspension_pos[i] = new_pos;

        if (new_pos > k_travel_lim) {
            actor->wheel_suspension_pos[i] = k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        }
        /* Note: the original uses `if new_pos < -k_travel_lim` (not else-if),
         * but in any single tick the new_pos can't be both > k_travel_lim
         * and < -k_travel_lim, so else-if and a separate-if are equivalent.
         * Listing uses a separate IF (0x00403B3C-B53 after the upper-clamp
         * block at 0x00403B33-3B). */
        if (actor->wheel_suspension_pos[i] < -k_travel_lim) {
            actor->wheel_suspension_pos[i] = -k_travel_lim;
            actor->wheel_spring_dv[i] = 0;
        }
    }

    /* ---- Central chassis pass ----
     * The original averages the front-axle contact (wheels 0+1) x and z
     * to derive the lever arm for the body-level suspension. No load
     * term, no deadzone; same spring/damping constants; travel clamp is
     * the SAME cardef+0x64 as per-wheel (NOT doubled).
     *
     * Listing: 0x00403B6A-C70.
     * - (FL.z + FR.z) summed, then `CDQ; SUB EAX,EDX; SAR EAX,1` (round-to-zero /2)
     * - Subtract sar8_rz(world_pos.z)
     * - Multiply by accel_z
     * - Same for x axis
     * - Sum into proj, sar8_rz(proj) * k_spring → spring_term
     * - new_vel = sar8_rz(spring_term) + center_suspension_vel
     * - pos_damp = sar8_rz(center_suspension_pos * k_pos_damp)
     * - vel_damp = sar8_rz(new_vel * k_vel_damp)
     * - new_vel -= vel_damp + pos_damp
     * - new_pos = center_suspension_pos + new_vel
     * - Clamp +k_travel_lim then -k_travel_lim
     * - NO deadzone, NO load term.
     */
    {
        /* Original (0x00403B6A-99): front-midpoint = sar1_rz(FL + FR), then
         * arm = front_mid - sar8_rz(world_pos). Hires is treated as already-
         * in-world-units in the original. The port's hires is 24.8 FP, so we
         * apply sar8_rz to bring it to world units; the sum-then-/2 commutes
         * with the /256 scaling (both are linear), but to preserve byte-exact
         * matching to the original's instruction order we compute /2 on the
         * 24.8 FP sum first, then /256 of that.
         *
         * For port hires written as `(int32_t lrintf_result) << 8`, the low 8
         * bits are zero, so `(FL.x + FR.x) % 256 == 0` and `sar1_rz((FL.x +
         * FR.x))` has low 8 bits still zero (since sum/2 of two LSB-8-zero
         * values has low 7 bits zero). Then `>>8` is exact regardless of
         * sign. We still use the round-to-zero idiom to mirror the original. */
        /* Hires now stored in raw world units (see 6595-6597). The original's
         * mid = (FL + FR)/2 then arm = mid - (world_pos >> 8) -- both operands
         * already in world units; no sar8_rz pass needed. */
        const int32_t fl_x = actor->wheel_world_positions_hires[0].x;
        const int32_t fr_x = actor->wheel_world_positions_hires[1].x;
        const int32_t fl_z = actor->wheel_world_positions_hires[0].z;
        const int32_t fr_z = actor->wheel_world_positions_hires[1].z;

        const int32_t front_mid_x = sar1_rz(fl_x + fr_x);
        const int32_t front_mid_z = sar1_rz(fl_z + fr_z);

        const int32_t arm_x = front_mid_x - wpx_scaled;
        const int32_t arm_z = front_mid_z - wpz_scaled;

        const int32_t proj = arm_x * accel_x + arm_z * accel_z;
        const int32_t spring_term_x256 = sar8_rz(proj) * k_spring;

        int32_t new_vel = sar8_rz(spring_term_x256) + actor->center_suspension_vel;

        const int32_t pos_damp_x256 = actor->center_suspension_pos * k_pos_damp;
        const int32_t vel_damp_x256 = new_vel * k_vel_damp;
        new_vel = new_vel - sar8_rz(pos_damp_x256) - sar8_rz(vel_damp_x256);

        int32_t new_pos = actor->center_suspension_pos + new_vel;
        actor->center_suspension_pos = new_pos;
        actor->center_suspension_vel = new_vel;

        if (new_pos > k_travel_lim) {
            actor->center_suspension_pos = k_travel_lim;
            actor->center_suspension_vel = 0;
        }
        if (actor->center_suspension_pos < -k_travel_lim) {
            actor->center_suspension_pos = -k_travel_lim;
            actor->center_suspension_vel = 0;
        }
    }

    td5_pilot_emit_00403A20_leave(actor);
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
/* Full body→world short rotation matching ConvertFloatVec3ToShortAngles @
 * 0x0042E2E0 (row-major float[9] times short[3], FPU TRUNCATE rounding via
 * __ftol). Writes 3 shorts; saturates each component to int16.
 *
 * Used by the chassis-snap loop in IntegrateVehiclePoseAndContacts to
 * populate wheel_contact_normals at +0x230+i*8 (per-wheel rotated body
 * offset for the suspension-response consumer). The Y component matches
 * `rotate_body_to_world_y` above. */
static void rotate_body_to_world_vec3(const TD5_Actor *actor,
                                      const int16_t v[3],
                                      int16_t out[3])
{
    const float *m = actor->rotation_matrix.m;
    float rx = (float)v[0] * m[0] + (float)v[1] * m[1] + (float)v[2] * m[2];
    float ry = (float)v[0] * m[3] + (float)v[1] * m[4] + (float)v[2] * m[5];
    float rz = (float)v[0] * m[6] + (float)v[1] * m[7] + (float)v[2] * m[8];
    if (rx >  32767.0f) rx =  32767.0f;
    if (rx < -32768.0f) rx = -32768.0f;
    if (ry >  32767.0f) ry =  32767.0f;
    if (ry < -32768.0f) ry = -32768.0f;
    if (rz >  32767.0f) rz =  32767.0f;
    if (rz < -32768.0f) rz = -32768.0f;
    out[0] = (int16_t)(int32_t)rx;
    out[1] = (int16_t)(int32_t)ry;
    out[2] = (int16_t)(int32_t)rz;
}

static int16_t rotate_body_to_world_y(const TD5_Actor *actor, const int16_t v[3])
{
    float m3 = actor->rotation_matrix.m[3];
    float m4 = actor->rotation_matrix.m[4];
    float m5 = actor->rotation_matrix.m[5];
    float result = (float)v[0] * m3 + (float)v[1] * m4 + (float)v[2] * m5;
    if (result >  32767.0f) return  32767;
    if (result < -32768.0f) return -32768;
    /* Original ConvertFloatVec3ToShortAngles @ 0x0042E2E0 calls __ftol @
     * 0x0044817C which EXPLICITLY sets the FPU rounding mode to TRUNCATE
     * (`OR AH, 0xC` = RC=11 = chop) before FISTP. So the orig truncates
     * toward zero, not round-to-nearest-even. Earlier port comment claimed
     * "FISTP rounds-to-nearest-even by default" — that's only true when
     * RC=00 in the control word, but __ftol forces RC=11. C cast
     * `(int32_t)float` already truncates toward zero, matching orig.
     *
     * Sum/4 of per-wheel rotated body-Y, scaled by -0x100, accumulates
     * the ~±1 LSB rounding drift into the chassis world_y. The previous
     * lrintf path produced a +128 FP world_y offset at spawn (port=58112
     * vs orig=57984), which propagated to wheel_y at sim_tick=2 refresh
     * → rear wheel airborne detection → chassis launch upward → Honolulu
     * rollover root cause. [round 28: 2026-05-03] */
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

#include "td5_pilot_trace_004057F0.h"

void td5_physics_update_suspension_response(TD5_Actor *actor)
{
    td5_pilot_emit_004057F0_enter(actor,
                                  (uintptr_t)__builtin_return_address(0),
                                  g_gravity_constant);
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
        td5_pilot_emit_004057F0_leave(actor);
        return;
    }

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "susp_resp_enter slot0: lock=0x%02x prev_air=0x%02x",
                  (int)lock, (int)prev_air);
    }

    /* Naming reflects WHAT the accumulator holds, NOT which actor field it
     * eventually feeds — orig and port disagree on that mapping (see write-
     * back block below).
     *
     * loni_grav / loni_spr accumulate (g_scaled * arm2)-derived terms, where
     * arm2 = wheel_display_angles[i][2] = body-Z (longitudinal) arm.
     * lat_grav  / lat_spr  accumulate (g_scaled * arm0)-derived terms, where
     * arm0 = wheel_display_angles[i][0] = body-X (lateral) arm.
     *
     * Original Ghidra locals: local_50=loni_grav, local_5c=lat_grav,
     * local_64=loni_spr, local_60=lat_spr, local_78=bounce, local_4c=cnt_active,
     * local_58=cnt_grounded. */
    int32_t loni_grav = 0;      /* local_50 — Σ g_scaled * arm2 */
    int32_t lat_grav  = 0;      /* local_5c — Σ -(g_scaled * arm0) */
    int32_t loni_spr  = 0;      /* local_64 — Σ (dot * arm2) * -0x100 */
    int32_t lat_spr   = 0;      /* local_60 — Σ (dot * arm0) * +0x100 */
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

        /* [CONFIRMED @ 0x004058B6-0x004058D5 disasm]:
         *   IMUL EDX, EBP   ; EDX = g_scaled * arm2 (loni)
         *   ADD  ECX, EDX   ; local_50 (loni_grav) += g_scaled * arm2
         *   IMUL EAX, EBX   ; EAX = g_scaled * arm0 (lat)
         *   SUB  ECX, EAX   ; local_5c (lat_grav)  -= g_scaled * arm0
         */
        loni_grav += g_scaled * (int32_t)loni;    /* += longitudinal arm */
        lat_grav  -= g_scaled * (int32_t)lat;     /* -= lateral arm (NOTE sign) */
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

            /* Per orig 0x004058E4+ disasm (and decompile):
             *   local_64 (loni_spr) += (dot * arm2) * -0x100
             *   local_60 (lat_spr)  += (dot * arm0) * +0x100 */
            loni_spr += (dot * (int32_t)loni) * -0x100;    /* arm2 with -0x100 */
            lat_spr  += (dot * (int32_t)lat ) *  0x100;    /* arm0 with +0x100 */
            /* [CONFIRMED @ 0x00405938-0x0040593F] CDQ + SUB EAX,EDX + SAR EAX,1
             * implements C-style signed div-by-2 with truncation toward zero
             * (e.g. -5/2 = -2). Previous port used `dot >> 1` which is SAR
             * = truncation toward -inf (-5 >> 1 = -3). For negative odd dot
             * this would diverge by 1. */
            bounce    += dot / 2;                          /* signed div toward zero */
            ++cnt_grounded;

            if (actor->slot_index == 0) {
                TD5_LOG_I(LOG_TAG,
                    "susp_resp landing_impulse slot0: wheel=%d dot=%d "
                    "lat=%d loni=%d loni_spr+=%d lat_spr+=%d",
                    i, dot, (int)lat, (int)loni,
                    (dot * (int32_t)loni) * -0x100,
                    (dot * (int32_t)lat)  *  0x100);
            }
        }
    }

    if (cnt_grounded > 0) {
        loni_spr /= cnt_grounded;
        lat_spr  /= cnt_grounded;
        bounce   /= cnt_grounded;
    }

    /* PlayVehicleSoundAtPosition(0x17, bounce*50, ...) call from the
     * original is omitted — sound side is stubbed elsewhere. */

    /* Apply roll/pitch corrective torque + Y velocity restore.
     *
     * [CONFIRMED @ 0x004059B5-0x00405A49] Original writes ang_vel_roll,
     * ang_vel_pitch and lin_vel_y UNCONDITIONALLY once cnt_active > 0.
     * The cnt_grounded > 0 condition (`JLE 0x00405AFA` at 0x00405A50)
     * only gates the pattern-clamp switch that follows — the WRITES
     * have already happened.
     *
     * AXIS-ASSIGNMENT FIX [CONFIRMED via byte-level disasm 2026-05-04
     * round 23, addresses 0x00405A3D + 0x00405A43]:
     *
     *   ORIG:  av_roll(+0x1C0)  += (loni_spr + loni_grav/cnt) / 0x4B0
     *          av_pitch(+0x1C8) += (lat_spr  + lat_grav /cnt) / 0x226
     *
     * The longitudinal-arm (loni / arm2 / wheel_display_angles[i][2])
     * derived torque feeds AV_ROLL, and the lateral-arm (lat / arm0)
     * derived torque feeds AV_PITCH — opposite of the standard physics
     * convention. Ghidra disasm shows:
     *
     *   0x004058BB  IMUL EDX, EBP   ; EDX = g_scaled * arm2 (loni)
     *   0x004058BE  ADD  ECX, EDX   ; local_50 += loni-derived
     *   0x004058B8  IMUL EAX, EBX   ; EAX = g_scaled * arm0 (lat)
     *   0x004058CC  SUB  ECX, EAX   ; local_5c -= lat-derived
     *   0x00405A3D  MOV [EDI+0x1c0], ESI   ; av_roll  = ... + local_50/0x4B0
     *   0x00405A43  MOV [EDI+0x1c8], EAX   ; av_pitch = ... + local_5c/0x226
     *
     * HISTORY: prior port (since restoration commit eb36524 2026-04-21)
     * routed loni-derived torque into av_pitch and lat-derived torque
     * into av_roll, mirroring standard physics — but contradicting orig.
     * For Viper (lat values ±255 symmetric, sum=0; loni 435/-394, sum=82)
     * the consequence under PlayerIsAI=0 manual drive on a slope was:
     *   port:  av_pitch = -1900*82/4/0x226 ≈ -71/tick    (loni-derived)
     *          av_roll  = 0                              (lat sum = 0)
     *   orig:  av_pitch = 0                              (lat sum = 0)
     *          av_roll  = -1900*82/4/0x4B0 ≈ -32/tick    (loni-derived)
     * Frida probe confirmed orig av_pitch=0 throughout sim_tick=1..50,
     * av_roll oscillating around -288. Math + disasm both agree.
     *
     * Round 22 noted the writes are unconditional once cnt_active > 0,
     * which round 9 commit a4b91ac had over-gated on cnt_grounded > 0.
     * That gate is now removed (correct). The axis-swap fix below sits
     * on top: same divisors per orig, just the right inputs feeding the
     * right outputs. */
    if (cnt_active > 0) {
        int32_t roll_term  = (loni_spr + loni_grav / cnt_active) / 0x4B0;
        int32_t pitch_term = (lat_spr  + lat_grav  / cnt_active) / 0x226;
        actor->angular_velocity_roll  += roll_term;
        actor->angular_velocity_pitch += pitch_term;

        /* Y-velocity update: bounce + gravity restored. Original adds
         * gravity back here, cancelling the subtract at top of integrate_pose
         * for grounded cars.
         *
         * [CONFIRMED via Edinburgh wheel_contact_probe 2026-05-11]: this
         * write MUST be inside the `cnt_active > 0` gate. Comment at line
         * 3778 explicitly says "the original writes both ang_vel_pitch and
         * lin_vel_y UNCONDITIONALLY once cnt_active > 0" — meaning BOTH
         * are gated on cnt_active > 0, not "unconditional in absolute
         * terms". When all 4 wheels are airborne (cnt_active=0), the
         * original SKIPS this write, letting gravity from integrate_pose
         * accumulate. The port previously placed this write OUTSIDE the
         * gate, which cancelled gravity during airborne and produced the
         * Edinburgh "car drifts forward at constant altitude" symptom
         * after launching off a road bump. */
        actor->linear_velocity_y += bounce + g_gravity_constant;
    }

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
                  "loni_grav=%d lat_grav=%d loni_spr=%d lat_spr=%d bounce=%d "
                  "av_r=%d av_p=%d vy=%d",
                  (int)lock, (int)prev_air, cnt_active, cnt_grounded,
                  loni_grav, lat_grav, loni_spr, lat_spr, bounce,
                  actor->angular_velocity_roll, actor->angular_velocity_pitch,
                  actor->linear_velocity_y);
    }

    td5_pilot_emit_004057F0_leave(actor);
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

/* ApplySimpleTrackSurfaceForce — byte-faithful port of 0x00407270.
 *
 * Pushes actor out of wall penetration and reflects the outward velocity.
 *
 * Address note: scope file lists this as "TOI_substep at 0x0040728B", but
 * 0x0040728B is mid-function (the AND EDX,0xFFF sign-correction inside the
 * pen → depth computation). The actual function entry is 0x00407270.
 *
 * Register-level register/var mapping (from listing 0x00407270-0x0040738E):
 *   EDI = cos = CosFixed12bit(edge_angle)      [0x00407279 CALL 0x40A6E0]
 *   EBX = sin = SinFixed12bit(edge_angle)      [0x00407281 CALL 0x40A700]
 *   ESI (depth) = ((pen + sign_corr(0xFFF)) >> 12) - 4   [0x0040728C-72A4]
 *   EBP_pos (a/k/a iVar5_pos) = depth*sin sign-corrected then shifted by 4
 *           and *negated*; added to *(actor+0x1fc) (= world_pos.x)
 *                                                       [0x004072A9-72B7]
 *   EAX_pos = depth*cos sign-corrected then shifted by 4;
 *           added to *(actor+0x204) (= world_pos.z)     [0x004072BB-72D5]
 *
 *   ESI (vx) = *(actor+0x1cc) = linear_velocity_x       [0x004072BE]
 *   ...
 *   EBP_vel = (vx*cos + vz*sin + sign_corr(0xFFF))      [0x004072E1-72FD]
 *           — TANGENTIAL component (along edge)
 *   EAX_vel = (vz*cos - vx*sin + sign_corr(0xFFF))      [0x004072FF-407311]
 *           — NORMAL/OUTWARD component (perp to edge)
 *   After SAR by 12: EBP=tang_sh, EAX=normal_sh.
 *
 *   JS 0x40738A at 0x40731C → exits if NORMAL component is negative,
 *           i.e. when vel projects INTO the wall (no work to do).
 *   Otherwise (normal_sh >= 0):
 *     vel_perp_new = -(normal_sh >> 1)            [0x40731E-407325]
 *           — CDQ/SUB-EDX is a no-op here because the branch is gated
 *             on normal_sh >= 0, so sign-bit broadcast = 0.
 *     Dead-zone clamp on tang_sh (EBP):           [0x407327-407347]
 *       if tang_sh >  0x180: tang_clamped = tang_sh - 0x180
 *       elif tang_sh < -0x180: tang_clamped = tang_sh + 0x180
 *       else: tang_clamped = 0
 *     vx = (tang_clamped*cos - vel_perp_new*sin + sign_corr) >> 12
 *     vz = (tang_clamped*sin + vel_perp_new*cos + sign_corr) >> 12
 *     *(actor+0x37b) = 0  (track_contact_flag)    [0x407383]
 *
 * Naming reconciliation: Ghidra decompiler labels the components "perp"
 * and "para" but the geometry is reversed from intuition — what Ghidra
 * calls iVar4 (perp) is actually the EDGE-TANGENTIAL projection, and
 * what it calls iVar3 (para) is the OUTWARD-NORMAL projection. We use
 * tang_/normal_ here to match the listing semantics.
 * ======================================================================== */
static void apply_simple_track_surface_force(TD5_Actor *actor,
                                              uint32_t edge_angle,
                                              int32_t pen)
{
    /* 0x00407278-0x00407286 */
    int32_t cos_a = cos_fixed12((int32_t)(edge_angle & 0xFFF));
    int32_t sin_a = sin_fixed12((int32_t)(edge_angle & 0xFFF));

    /* 0x0040728C-0x004072A4: depth = ((pen + sign_corr) >> 12) - 4
     *   CDQ; AND EDX,0xFFF; ADD EAX,EDX; SAR ESI,0xC; SUB ESI,0x4   */
    int32_t depth = ((pen + ((int32_t)((uint32_t)(pen >> 31) & 0xFFFu))) >> 12) - 4;

    /* 0x004072A9-0x004072B7: actor->world_pos.x += -((depth*sin + sign_corr(0xF)) >> 4)
     *   IMUL EAX,EBX(sin); CDQ; AND EDX,0xF; ADD EAX,EDX; SAR EAX,0x4; NEG EAX;
     *   ADD EBP,EAX  (EBP was preloaded from [ECX+0x1fc])                 */
    {
        int32_t prod_sin = depth * sin_a;
        int32_t step_x   = (prod_sin + ((int32_t)((uint32_t)(prod_sin >> 31) & 0xFu))) >> 4;
        actor->world_pos.x += -step_x;
    }

    /* 0x004072B9-0x004072D5: actor->world_pos.z += ((depth*cos + sign_corr(0xF)) >> 4)
     *   IMUL EAX,EDI(cos); CDQ; AND EDX,0xF; ADD EAX,EDX; SAR EAX,0x4;
     *   ADD EDX(=[ECX+0x204]),EAX                                          */
    {
        int32_t prod_cos = depth * cos_a;
        int32_t step_z   = (prod_cos + ((int32_t)((uint32_t)(prod_cos >> 31) & 0xFu))) >> 4;
        actor->world_pos.z += step_z;
    }

    /* 0x004072BE / 0x004072DB-0x00407319: velocity decomposition.
     * Note ESI=vx is loaded at 0x4072BE BEFORE the x-position write, but
     * since world_pos.x is offset 0x1FC and linear_velocity_x is offset
     * 0x1CC they don't alias, and the order of reads/writes is observable
     * only through the actor struct (same outcome). */
    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* EBP = (vx*cos + vz*sin) + sign_corr(0xFFF) — TANGENTIAL raw       */
    int32_t tang_raw = vx * cos_a + vz * sin_a;
    int32_t tang_adj = tang_raw + ((int32_t)((uint32_t)(tang_raw >> 31) & 0xFFFu));

    /* EAX = (vz*cos - vx*sin) + sign_corr(0xFFF) — OUTWARD-NORMAL raw  */
    int32_t normal_raw = vz * cos_a - vx * sin_a;
    int32_t normal_adj = normal_raw + ((int32_t)((uint32_t)(normal_raw >> 31) & 0xFFFu));

    /* SAR by 12 [0x00407316, 0x00407319] */
    int32_t tang_sh   = tang_adj   >> 12;
    int32_t normal_sh = normal_adj >> 12;

    /* JS 0x0040738A at 0x40731C — exit when normal_sh < 0 [outward vel
     * projects toward wall: nothing to push back]                       */
    if (normal_sh < 0) {
        return;
    }

    /* 0x0040731E-0x00407325: vel_perp_new = -((normal_sh - 0) >> 1)
     *   CDQ; SUB EAX,EDX; SAR EAX,1; MOV ESI,EAX; NEG ESI
     * Since we just gated on normal_sh >= 0, EDX = sign-broadcast = 0,
     * so SUB EAX,EDX is a no-op. The arithmetic SAR happens BEFORE NEG,
     * which matters when normal_sh is odd: e.g. normal_sh=5 →
     *   asm: -(5 >> 1) = -2          (vs. (-5) >> 1 = -3)               */
    int32_t vel_perp_new = -(normal_sh >> 1);

    /* 0x00407327-0x00407347: dead-zone clamp on tang_sh (EBP)
     *   CMP EBP,0x180; JLE -> next test; ADD EBP,-0x180; JMP done
     *     [reached when EBP > 0x180, i.e. EBP >= 0x181]
     *   CMP EBP,-0x180; JGE 0x40347 (XOR EBP,EBP); ADD EBP,0x180
     *     [JGE means EBP >= -0x180 → zero;
     *      else (EBP < -0x180): EBP += 0x180]                            */
    int32_t tang_clamped;
    if (tang_sh > 0x180) {
        tang_clamped = tang_sh - 0x180;
    } else if (tang_sh < -0x180) {
        tang_clamped = tang_sh + 0x180;
    } else {
        tang_clamped = 0;
    }

    /* 0x00407349-0x0040737D: recompose velocity in world frame.
     *   nvx = tang_clamped*cos - vel_perp_new*sin
     *   nvz = tang_clamped*sin + vel_perp_new*cos
     * Each summand gets sign_corr(0xFFF) before SAR by 12.               */
    {
        int32_t nvx_raw = tang_clamped * cos_a - vel_perp_new * sin_a;
        int32_t nvx_adj = nvx_raw + ((int32_t)((uint32_t)(nvx_raw >> 31) & 0xFFFu));
        actor->linear_velocity_x = nvx_adj >> 12;
    }
    {
        int32_t nvz_raw = tang_clamped * sin_a + vel_perp_new * cos_a;
        int32_t nvz_adj = nvz_raw + ((int32_t)((uint32_t)(nvz_raw >> 31) & 0xFFFu));
        actor->linear_velocity_z = nvz_adj >> 12;
    }

    /* 0x00407383: *(actor+0x37b) = 0 — track_contact_flag */
    actor->track_contact_flag = 0;
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
            apply_simple_track_surface_force(actor, edge_angle, pen);
            /* DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440]:
             * encounter mode 4 erodes the actor's clean_driving_score by 1
             * per wall-contact tick, but only while the actor is still racing. */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }
            /* Original calls UpdateTrafficVehiclePose again after push;
             * the port's integrate_traffic_pose rebuilds the pose at the end
             * of the tick anyway, so skip the redundant rebuild here. */
            return;
        }
    }

outer_test:
    /* Outer edge test: sub_lane >= lane_count - 2  [CONFIRMED @ 0x407462: if (iVar12 >= laneCount-2)] */
    if (sub_lane >= lane_count - 2) {
        /* Outer boundary vertices at lane_count-1.
         * Original outer test [CONFIRMED @ 0x4074B4-0x4074F4]:
         *   psVar1 = vertex_pool[DAT_004631a0[span_type] + strip[+0x04] + outer_sub]
         *   psVar2 = vertex_pool[DAT_004631a4[span_type] + strip[+0x06] + outer_sub]
         * In port naming: strip[+0x04] = right_vertex_index, strip[+0x06] = left_vertex_index.
         * DAT_004631a4 is 0 for all 12 span types [CONFIRMED @ 0x004631A0].
         * DAT_004631a0 values [CONFIRMED @ 0x004631A0]:
         *   span_type: 0  1  2  3  4  5  6  7  8  9 10 11 */
        static const int8_t k_outer_left_offsets[12] = {
            0, 0, -1, -1, -2, 0, -1, 0, -1, 0, -1, -2
        };
        int outer_sub = lane_count - 1;
        /* li (psVar1) uses right_vertex_index + offset; ri (psVar2) uses left_vertex_index + 0 */
        int li_idx = k_outer_left_offsets[span_type] + (int)sp->right_vertex_index + outer_sub;
        int ri_idx = (int)sp->left_vertex_index + outer_sub;
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
            TD5_LOG_I("physics", "seg_edge outer push slot=%d span=%d type=%d pen=%d", slot, (int)actor->track_span_raw, span_type, (int)pen);
            apply_simple_track_surface_force(actor, edge_angle, pen);
            /* DecayUltimateVariantTimer [CONFIRMED @ 0x0040A440] — same as inner-edge */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }
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

    /* Refresh heading_normal at +0x290 after the chassis-walker resolves
     * the new span. [CONFIRMED @ 0x00443d3d: UpdateTrafficVehiclePose
     * calls ComputeActorHeadingFromTrackSegment with LEA [esi+0x290].]
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

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

    /* Yaw from euler accumulator (only axis using accumulators for traffic).
     * No 0xFFF mask -- original (0x4063AA-style) truncates via int16 store
     * only; mask flipped sign bit for negative accumulators. Matches the
     * unmasked fix at 5978 for the player/AI integrator path. */
    actor->display_angles.yaw = (int16_t)(actor->euler_accum.yaw >> 8);

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
    /* Pilot precise-port trace — snapshot inputs at entry (slot 0 only). */
    td5_pilot_emit_00405E80_enter(actor, (uintptr_t)__builtin_return_address(0));

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

    /* Refresh heading_normal at +0x290 from the live span geometry.
     * [CONFIRMED @ 0x00405fb6: IntegrateVehiclePoseAndContacts calls
     * ComputeActorHeadingFromTrackSegment with LEA [esi+0x290].]
     * Mirrors the trailing call inside UpdateVehiclePoseFromPhysicsState;
     * was previously missing here, which kept heading_normal at its
     * stale spawn-pose value (with y=0 from the old td5_track_compute_heading
     * mapping) for the entire IntegrateVehiclePoseAndContacts code path.
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

    /* 4. Convert accumulators to display angles. NO 0xFFF mask -- original
     * truncates via int16 store; explicit mask flipped sign bit. Matches
     * 5978 unmasked fix from the precise-port pilot 004063A0. */
    actor->display_angles.roll  = (int16_t)(actor->euler_accum.roll  >> 8);
    actor->display_angles.yaw   = (int16_t)(actor->euler_accum.yaw   >> 8);
    actor->display_angles.pitch = (int16_t)(actor->euler_accum.pitch >> 8);

    /* 5. Build rotation matrix from euler angles in FLOAT precision.
     *
     * Original (0x42E1E0): Ry(yaw) * Rx(roll) * Rz(pitch) — YXZ order, all
     * trig multiplications in 80-bit FPU. Earlier port used integer Q12
     * with `>> 12` truncations after each multiply — those LSB losses
     * propagated through `body_wy * m[4]` in chassis-snap and produced a
     * +128 FP averaged delta in world_pos.y at sim_tick=1.
     * [E1 / 2026-05-02 — physics_trace.csv ground-truth diff] */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        const float k = 1.0f / 4096.0f;
        float cr = (float)cos_fixed12(roll_a)  * k;
        float sr = (float)sin_fixed12(roll_a)  * k;
        float cy = (float)cos_fixed12(yaw_a)   * k;
        float sy = (float)sin_fixed12(yaw_a)   * k;
        float cp = (float)cos_fixed12(pitch_a) * k;
        float sp = (float)sin_fixed12(pitch_a) * k;

        actor->rotation_matrix.m[0] = sp * sy * sr + cp * cy;
        actor->rotation_matrix.m[1] = cp * sy * sr - sp * cy;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp * cy * sr - cp * sy;
        actor->rotation_matrix.m[7] = cp * cy * sr + sp * sy;
        actor->rotation_matrix.m[8] = cy * cr;
    }

    /* 6. Compute render position (world_pos / 256 as float) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* 7. Save previous-frame grounded mask, then refresh wheel contacts.
     * suspension_response needs "was grounded last frame" for the spring
     * damping path. Refresh writes the live airborne mask into +0x37C
     * (wheel_contact_bitmask, 1=airborne) and snapshots the prior tick's
     * value into +0x37D (damage_lockout) at entry. This side-channel
     * remains for compatibility with consumers that already use it; +0x37D
     * also now carries the OLD mask in airborne polarity. */
    s_prev_grounded_mask[actor->slot_index & 0x0F] = (~actor->wheel_contact_bitmask) & 0x0F;
    td5_physics_refresh_wheel_contacts(actor);

    /* Inner-tick trace post_refresh: deprecated — td5_trace_write_physics
     * was dropped by merge 1acd3fb in favor of the modular MOD_PHYSICS API.
     * Stubbed out until rewritten against the new emission path. */

    /* Increment airborne_frame_counter (+0x360) when all 4 wheels airborne.
     * [CONFIRMED @ 0x0040634B]: original does INC word ptr [ESI+0x360]
     * inside the per-wheel averaging loop of IntegrateVehiclePoseAndContacts,
     * reached only when the grounded-wheel count is 0 — equivalently
     * damage_lockout == 0x0F. NO reset instruction exists in the binary
     * (exhaustive search): the counter monotonically grows during sustained
     * airborne, and simply stops growing when any wheel grounds (the INC
     * isn't reached). State0f_damping at td5_physics.c:547 fires when
     * afc >= 3 AND dlk == 0x0F (CMP at 0x00406835 + JL/JNZ at 0x0040683D). */
    if (actor->wheel_contact_bitmask == 0x0F && !g_game_paused) {
        /* afc++ gated behind !g_game_paused so countdown ticks don't
         * accumulate the all-airborne counter. Refresh runs every render
         * frame including the ~190-frame pause countdown; without this
         * gate, airborne_frame_counter would already be ≥5 by the first
         * un-paused dispatch on Honolulu, instantly triggering the
         * state0f damping gate at line 641 (wcb==0x0F && afc>=3) — that
         * gate replaces update_player, so longitudinal_speed (+0x314)
         * stays at zero through countdown into the active race. The
         * original almost certainly does NOT increment afc during the
         * countdown (race not active → tick loop suppressed). Verifying
         * exact gate location in original is a follow-up; this guard
         * preserves the desired post-countdown behavior either way. */
        actor->airborne_frame_counter++;
        TD5_LOG_I(LOG_TAG, "tumble_gate: slot=%d wcb=0x0F dlk=0x0F afc=%d",
                  actor->slot_index, actor->airborne_frame_counter);
    }

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
         * display_angles in float precision (E1 fix — see line ~4361). */
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        const float k = 1.0f / 4096.0f;
        float cr = (float)cos_fixed12(roll_a)  * k;
        float sr = (float)sin_fixed12(roll_a)  * k;
        float cy = (float)cos_fixed12(yaw_a)   * k;
        float sy = (float)sin_fixed12(yaw_a)   * k;
        float cp = (float)cos_fixed12(pitch_a) * k;
        float sp = (float)sin_fixed12(pitch_a) * k;

        actor->rotation_matrix.m[0] = sp * sy * sr + cp * cy;
        actor->rotation_matrix.m[1] = cp * sy * sr - sp * cy;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp * cy * sr - cp * sy;
        actor->rotation_matrix.m[7] = cp * cy * sr + sp * sy;
        actor->rotation_matrix.m[8] = cy * cr;

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

    /* Inner-tick trace post_t2: deprecated — see post_refresh stub above. */

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
             * 0x004061EA before ConvertFloatVec3ToShortAngles.
             *
             * Original 0x00405E80 (Ghidra decomp pool10 2026-05-15):
             *
             *   ConvertFloatVec3ToShortAngles(psVar10 + -1, puVar8);
             *
             * where psVar10-1 = wheel_disp_angles[i] = (cwx, body_wy, cwz)
             * and puVar8 = &actor->field_0x230 + i*8 = wheel_contact_normals[i].
             *
             * So the original writes all 3 rotated components to +0x230+i*8.
             * The chassis-snap accumulator uses only puVar8[1] (the Y),
             * but downstream suspension-response consumers read the full
             * vector. Without this write the wheel_contact_normals stay
             * zero (visible in whole_state_port.bin diff as the 16-byte
             * +0x230 blob mismatching). */
            int16_t src[3] = { cwx, body_wy, cwz };
            int16_t rot_v3[3];
            rotate_body_to_world_vec3(actor, src, rot_v3);
            int16_t rot_y = rot_v3[1];

            /* Write to wheel_contact_normals[i] at +0x230 + i*8 (4-short
             * stride, 4th short is padding / unused in the original). */
            {
                int16_t *wcn = (int16_t *)((uint8_t *)actor + 0x230 + i * 8);
                wcn[0] = rot_v3[0];
                wcn[1] = rot_v3[1];
                wcn[2] = rot_v3[2];
            }

            int32_t wheel_y = actor->wheel_contact_pos[i].y;
            contact_y_sum += (int64_t)(wheel_y + (int32_t)rot_y * -0x100);
            contact_count++;
        }

        if (contact_count > 0) {
            /* [FIX #2] NO +0x80 bias — the original writes the plain
             * signed-IDIV result (MOV [ESI+0x200], EAX at 0x00406307). */
            int32_t new_y = (int32_t)(contact_y_sum / contact_count);

            /* No snap-delta clamp. Original at 0x00406300 directly writes
             * world_pos.y from the contact-Y average without bounding the
             * delta vs prev_frame_y_position; the +0x37D OLD-mask gate
             * below is the original's only filter on this write. The prior
             * port's ±2M clamp was a workaround for the +0x37C/+0x37D
             * semantic reversal that has since been corrected. */

            /* Chassis Y-snap. No chassis-level airborne override here —
             * the per-wheel airborne bits written by refresh_wheel_contacts
             * (force >= 0x801 @ 0x00403720) now drive downstream airborne
             * behavior. Match original 0x00406300 which snaps
             * unconditionally once contact_count > 0.
             *
             * [CONFIRMED via Ghidra decomp of IntegrateVehiclePoseAndContacts
             * @ 0x00405E80, 2026-05-15]: the original writes render_pos_y
             * exactly ONCE per tick — at the post-gravity, pre-snap step
             * (matches our line 5468). The chassis-snap (this line) updates
             * world_pos.y ONLY, leaving render_pos.y at the gravity-dropped
             * value. The next frame's sub-tick interpolation pass propagates
             * the new world_pos.y into render_pos.y via
             * td5_physics_apply_render_interpolation. Removing the port's
             * post-snap render_pos.y write closes the 7.4u (g/256) gap
             * between port=226.5 and orig=219.078 on Honolulu sim_tick=1. */
            actor->world_pos.y = new_y;

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
             *
             * 2026-05-01 follow-up: bugs (2) + (3) returned in disguise.
             * The port's refresh_wheel_contacts mutates +0x37D in place to
             * hold the NEW airborne mask (per-wheel writes at 5204/5207),
             * and line 4409 copies that NEW value into +0x37C. So BOTH
             * actor+0x37C and actor+0x37D end the refresh holding NEW —
             * the gate `(prev_mask & (new_mask ^ 0x0F)) == 0` reduces to
             * `(NEW & ~NEW) == 0` → always TRUE. Velocity-snap fires every
             * tick, overriding integrated vy with a ±30000-clamped delta.
             * On slope onsets this manifests as the chassis NOT following
             * terrain (vertical momentum is killed, then clamped into a
             * step that doesn't match the ground gradient).
             *
             * The OLD mask in airborne polarity is reconstructed from
             * s_prev_grounded_mask[] (snapshotted pre-refresh at line 4398
             * in grounded polarity, so we re-invert here).
             *
             * [CONFIRMED @ 0x00403720, 0x004039E5: original keeps +0x37D
             * = OLD across the refresh]. */
            {
                static const uint8_t k_mode_gate[16] = {
                    1,1,1,0, 1,0,0,0, 1,0,0,0, 0,0,0,0
                };
                uint8_t new_mask  = actor->wheel_contact_bitmask;
                uint8_t prev_mask = (~s_prev_grounded_mask[actor->slot_index & 0x0F]) & 0x0F;
                if (k_mode_gate[new_mask & 0xF] &&
                    (prev_mask & (new_mask ^ 0x0F)) == 0 &&
                    actor->prev_frame_y_position != (int32_t)0xC0000000) {
                    int32_t snap_vy = new_y - actor->prev_frame_y_position
                                     - g_gravity_constant;
                    /* No clamp. Original at 0x0040630D-0x00406335 writes the
                     * raw delta-minus-gravity unconditionally once the gate
                     * passes; ±30000 was a port-only workaround for the
                     * +0x37C/+0x37D semantic reversal that gated the snap
                     * every tick. With the OLD-mask now correctly held in
                     * +0x37D, the gate fires only on stable wheel sets and
                     * the unclamped delta is faithful. */
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

    /* 9. Clamp angular velocity deltas to +/- 6000 per frame.
     * [CONFIRMED @ 0x405F0A-0x405F2C, 0x406058-0x40607B]: original T2 block
     * clamps roll and pitch only — yaw is NOT clamped here. The yaw clamp
     * was a port addition that throttled spin recovery after V2V hits and
     * hard cornering; removing it restores faithful behavior. */
    if (actor->angular_velocity_roll > 6000) actor->angular_velocity_roll = 6000;
    if (actor->angular_velocity_roll < -6000) actor->angular_velocity_roll = -6000;
    if (actor->angular_velocity_pitch > 6000) actor->angular_velocity_pitch = 6000;
    if (actor->angular_velocity_pitch < -6000) actor->angular_velocity_pitch = -6000;

    /* Inner-tick trace post_snap: deprecated — see post_refresh stub above. */

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

    /* Pilot precise-port trace — snapshot outputs at exit (slot 0 only). */
    td5_pilot_emit_00405E80_leave(actor);
}

/* ========================================================================
 * UpdateVehiclePoseFromPhysicsState (0x4063A0)
 *
 * Lightweight pose refresh (no force integration). Used as a callback
 * during track segment contact resolution.
 * ======================================================================== */

static void update_vehicle_pose_from_physics(TD5_Actor *actor)
{
    /* Pilot precise-port trace — snapshot inputs at entry. */
    td5_pilot_emit_004063A0_enter(actor, (uintptr_t)__builtin_return_address(0));

    /* Convert current angles to display.
     *
     * Listing 0x004063D5/3DE/3E1/3F8/401/408:
     *   SAR EAX, 8           (arithmetic signed shift)
     *   MOV [actor+0x208], AX (int16 truncate — NO 12-bit mask)
     *
     * Removed the `& 0xFFF` mask the port previously applied to all three
     * components: for negative euler_accum values that mask flipped the
     * sign bit into a 12-bit positive number, diverging from the original's
     * int16 truncation. The trig helpers (cos_fixed12 / sin_fixed12) mask
     * to 12 bits internally, so the rotation matrix is unaffected, but
     * any external consumer of display_angles (camera, HUD, network) was
     * reading wrong values for negative angles.
     * [D2 — precise-port pilot 004063A0, 2026-05-14] */
    actor->display_angles.roll  = (int16_t)(actor->euler_accum.roll  >> 8);
    actor->display_angles.yaw   = (int16_t)(actor->euler_accum.yaw   >> 8);
    actor->display_angles.pitch = (int16_t)(actor->euler_accum.pitch >> 8);

    /* Render position — matches FILD/FMUL/FSTP sequence at 0x004063AA-0x0040641B.
     * (Float scale = 1/256 from [0x0045D5E8].) */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Build rotation matrix #1 (first call to BuildRotationMatrixFromAngles
     * @ 0x0042E1E0 at 0x00406421). */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        /* Float-precision matrix build (E1 fix — see line ~4361). */
        const float k = 1.0f / 4096.0f;
        float cr = (float)cos_fixed12(roll_a)  * k;
        float sr = (float)sin_fixed12(roll_a)  * k;
        float cy = (float)cos_fixed12(yaw_a)   * k;
        float sy = (float)sin_fixed12(yaw_a)   * k;
        float cp = (float)cos_fixed12(pitch_a) * k;
        float sp = (float)sin_fixed12(pitch_a) * k;

        actor->rotation_matrix.m[0] = sp * sy * sr + cp * cy;
        actor->rotation_matrix.m[1] = cp * sy * sr - sp * cy;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp * cy * sr - cp * sy;
        actor->rotation_matrix.m[7] = cp * cy * sr + sp * sy;
        actor->rotation_matrix.m[8] = cy * cr;
    }

    /* Refresh chassis track position from updated world coords.
     * Original 0x00406432: CALL UpdateActorTrackPosition (0x004440F0)
     *   args: (&actor->track_span_raw @+0x80, &actor->world_pos @+0x1FC).
     *
     * Without this, after a V2V/wall impulse the chassis span (+0x80) stays at
     * the pre-impulse value and downstream per-wheel probes in
     * td5_physics_refresh_wheel_contacts copy the stale span into the wheel
     * probes (the per-wheel walker then re-syncs but starts from the wrong
     * span — measurable as a 1-tick lag in lateral-wall pen tests).
     * [D1 — precise-port pilot 004063A0, 2026-05-14] */
    td5_track_update_actor_position(actor);

    /* Recompute heading-relative-to-segment short at +0x290.
     * Original 0x00406447: CALL ComputeActorHeadingFromTrackSegment (0x00445B90)
     *   args: (&actor->track_span_raw @+0x80, &actor->world_pos @+0x1FC,
     *          &actor->heading_normal @+0x290).
     *
     * Re-routed 2026-05-14 (precise-00445B90) to the byte-faithful
     * runtime variant td5_track_compute_runtime_heading_normal — it
     * writes the normalized surface normal of the picked triangle
     * (heading_normal.y ≈ 4096 on flat track, dropping on slopes),
     * unblocking ApplyMissingWheelVelocityCorrection which multiplies
     * by heading_normal.y. The previous mapping to compute_heading
     * (InitializeActorTrackPose, 0x00434350) was a SPAWN-only writer
     * that hard-coded heading_normal[1]=0, neutering the correction.
     * [precise-port pilot 00445B90, 2026-05-14] */
    td5_track_compute_runtime_heading_normal(actor);

    /* Refresh wheel contacts (CALL RefreshVehicleWheelContactFrames @ 0x00403720
     * at 0x00406453). The wheel walker, contact-frame transforms, gap_270,
     * and the OLD-bitmask snapshot at +0x37D all happen inside this call. */
    td5_physics_refresh_wheel_contacts(actor);

    /* TODO (D3+D4 — damage_lockout switch + 2nd matrix rebuild):
     * Original 0x00406459-0x004064E1:
     *   switch (actor->damage_lockout @ +0x37D) {
     *     case 0,1,2,4,6,8,9: TransformTrackVertexByMatrix  (0x00446030); writes back roll + pitch
     *     case 5,10:          TransformTrackVertexByMatrixB (0x00446140); writes back roll only
     *     case 3,12:          TransformTrackVertexByMatrixC (0x004461C0); writes back pitch only
     *     case 7,11,default:  no-op
     *   }
     *   euler_accum_roll  = display_angle_roll  << 8   (if A or B branch)
     *   euler_accum_pitch = display_angle_pitch << 8   (if A or C branch)
     *   BuildRotationMatrixFromAngles(rotation_matrix, display_angles)   // 2nd build
     *
     * The Transform* helpers derive wheel-implied roll/pitch from the post-
     * impulse wheel_contact_pos + wheel_suspension_pos, allowing the next
     * physics tick to start integration at the wheel-attitude. Without this,
     * after a collision the chassis pose remains at the impulse-applied
     * attitude even when the wheels disagree with it.
     *
     * Faithful port of the three Transform* helpers requires ports of
     * AngleFromVector12 (0x0040A720, already done in precise-trig worktree)
     * + FUN_0044817C + an inline FSQRT-rounding path. Deferred to a
     * follow-up precise-port worktree (precise-00446030). */

    /* Second BuildRotationMatrixFromAngles call (0x004064E1).
     * Even without the switch writeback, the original UNCONDITIONALLY rebuilds
     * the rotation matrix here after the switch path completes. For cases
     * where the switch is a no-op (case 7, 11, or out-of-range), this rebuild
     * is functionally a redundant identical recomputation; for the active
     * cases (0..6, 8..10, 12) it picks up the new wheel-derived roll/pitch.
     *
     * Since the port currently SKIPS the switch (D3), this 2nd rebuild
     * regenerates the SAME matrix already built above. It is included anyway
     * so the byte-faithful sequence is preserved for the day D3 lands.
     * [D4 — precise-port pilot 004063A0, 2026-05-14] */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        const float k = 1.0f / 4096.0f;
        float cr = (float)cos_fixed12(roll_a)  * k;
        float sr = (float)sin_fixed12(roll_a)  * k;
        float cy = (float)cos_fixed12(yaw_a)   * k;
        float sy = (float)sin_fixed12(yaw_a)   * k;
        float cp = (float)cos_fixed12(pitch_a) * k;
        float sp = (float)sin_fixed12(pitch_a) * k;

        actor->rotation_matrix.m[0] = sp * sy * sr + cp * cy;
        actor->rotation_matrix.m[1] = cp * sy * sr - sp * cy;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp * cy * sr - cp * sy;
        actor->rotation_matrix.m[7] = cp * cy * sr + sp * sy;
        actor->rotation_matrix.m[8] = cy * cr;
    }

    /* Load the rebuilt matrix into the global render matrix
     * (LoadRenderRotationMatrix @ 0x0043DA80 at 0x0040650B). Chassis-snap's
     * ConvertFloatVec3ToShortAngles (0x0042E2E0) reads from this global. The
     * port's chassis-snap below uses a cached s_wheel_offset_y_world from
     * refresh — but loading the matrix maintains parity for any other
     * consumer that pumps body-space vectors through the global transform
     * before the next render tick.
     * [D5 — precise-port pilot 004063A0, 2026-05-14] */
    LoadRenderRotationMatrix(actor->rotation_matrix.m);

    /* Chassis ground snap — faithful port of the tail of
     *   UpdateVehiclePoseFromPhysicsState @ 0x004063A0
     *
     *     local_10 = sum_over_grounded(wheel_probe_y - rotated_wheel_y_offset)
     *              / count;
     *     world_pos.y = local_10;          // absolute assignment, NOT delta
     *
     * Inputs (post-refresh):
     *   wheel_contact_pos[i].y == ground_y           (refresh snapped it for
     *                                                 grounded wheels @ td5_physics.c:5210)
     *   s_wheel_offset_y_world[slot][i]               (rotated body→world Y offset,
     *                                                 pre-snap, captured in refresh)
     *
     * Replaces a port-invented `td5_track_probe_height(x, z, chassis_span)`
     * re-probe that used the chassis span for every wheel and a heuristic
     * lane picker disjoint from the per-wheel walker. With the per-wheel
     * walker fixed to single-step (2026-05-01), the wheel_contact_pos
     * values are themselves the correct ground heights at each wheel's own
     * span/lane, and the original's straight `sum/count` form lands a
     * stable chassis on slope onsets without the spurious +896 FP per-tick
     * launch the re-probe produced at Moscow span 196 (Frida-localized).
     *
     * "Grounded" = wheel_contact_bitmask bit clear (1=airborne). On total
     * airborne (count==0) world_pos.y is left alone. */
    {
        int slot = actor->slot_index;
        if (slot < 0 || slot >= 16) slot = 0;
        uint8_t lock = actor->wheel_contact_bitmask;
        int64_t target_sum = 0;
        int target_count = 0;
        for (int i = 0; i < 4; i++) {
            if (lock & (1 << i)) continue;        /* airborne — skip */
            target_sum += (int64_t)actor->wheel_contact_pos[i].y
                        - (int64_t)s_wheel_offset_y_world[slot][i];
            target_count++;
        }
        if (target_count > 0) {
            actor->world_pos.y = (int32_t)(target_sum / target_count);
            actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
        }
    }

    /* Pilot precise-port trace — snapshot outputs at exit. */
    td5_pilot_emit_004063A0_leave(actor);
}

/* ========================================================================
 * TransformShortVec3ByRenderMatrixRounded (0x0042EB10) -- byte-faithful port
 *
 * Original layout (from listing 0x0042EB10..0x0042EBE3):
 *   out[0] = round(p1*M[1] + p2*M[2] + p0*M[0] + M[9])
 *   out[1] = round(p1*M[4] + p0*M[3] + p2*M[5] + M[10])
 *   out[2] = round(p1*M[7] + p0*M[6] + p2*M[8] + M[11])
 *
 * The x87 sequence per output is:
 *   FILD p1; FMUL M[col_y]
 *   FILD p2; FMUL M[col_z]; FADDP
 *   FILD p0; FMUL M[col_x]; FADDP
 *   FADD M[trans]
 *   FSTP [tmp]   ; collapse 80→32 bit
 *   FLD  [tmp]   ; reload as 32-bit
 *   FISTP [out]  ; round to int32 USING THE CURRENT FPU CONTROL WORD
 *
 * [PILOT FINDING 2026-05-14] — empirical FPU control-word audit via the
 * trace at log/orig/pool4_0042EB10.csv shows 99.82% of rounded outputs
 * match `floorf` (round toward -infinity, RC=01). The 0.18% residue is
 * 80-bit vs 64-bit accumulator divergence. The original's runtime startup
 * therefore sets the x87 RC bits to 01 — not the Windows default 00
 * (round-to-nearest-even).
 *
 * Comparison vs alternative rounding modes (6228 outputs):
 *   round-to-nearest:        49.05%
 *   round-down (toward -inf): 99.82%   ← matches original
 *   round-toward-zero:       66.20%
 *   round-up (toward +inf):   0.11%
 *
 * Smoking-gun example (input p=(-255,-227,435), matrix at slot-0 wcp wheel 0):
 *   FP accumulator = 204.5853, original out2 = 204
 *   round-to-nearest → 205 (off by 1), floor → 204 (match).
 *
 * NOTE on row order: outputs 1 and 2 use a DIFFERENT operand permutation
 * than output 0 (the assembler emits "p1, p0, p2"). We match that exactly.
 *
 * NOTE on order-of-operations: the x87 chain accumulates at 80-bit until
 * the final FSTP. The single `volatile float tmp` below would force a
 * 32-bit collapse at every intermediate step. Original keeps 80-bit until
 * the last FSTP only. Since GCC -m32 -O2 typically uses x87 too, removing
 * the volatile lets the compiler emit the same FADDP chain.
 *
 * Pilot trace pool4_0042EB10.csv hooks at every call site.
 *
 * [CONFIRMED @ 0x0042EB10 by precise-port pilot 2026-05-14]
 * ======================================================================== */

/* Round-to-minus-infinity replacement for lrintf. Always uses floorf which
 * is rounding mode-independent (no reliance on FPU control word). */
static inline int32_t td5_round_toward_neg_inf_to_int32(float x) {
    return (int32_t)floorf(x);
}

static inline void td5_transform_short_vec3_by_render_matrix_rounded(
    const int16_t param_1[3], int32_t param_2[3], const float matrix[12])
{
    /* Promote int16 → int32 → float exactly (FILD-equivalent). */
    int p0 = (int)param_1[0];
    int p1 = (int)param_1[1];
    int p2 = (int)param_1[2];
    float fp0 = (float)p0;
    float fp1 = (float)p1;
    float fp2 = (float)p2;

    /* Output 0 — operand order from listing 0x0042EB28..0x0042EB4A
     *   ST = p1*M1 + p2*M2 + p0*M0 + M9 */
    {
        /* Keep accumulator at compiler-natural precision through the chain.
         * Only collapse once via the float assignment below before FISTP. */
        float tmp = fp1 * matrix[1] + fp2 * matrix[2] + fp0 * matrix[0] + matrix[9];
        /* `tmp` is `float` — the assignment forces 32-bit collapse the same way
         * the original's FSTP/FLD pair does. */
        param_2[0] = td5_round_toward_neg_inf_to_int32(tmp);
    }
    /* Output 1 — operand order from listing 0x0042EB6B..0x0042EB94
     *   ST = p1*M4 + p0*M3 + p2*M5 + M10 */
    {
        float tmp = fp1 * matrix[4] + fp0 * matrix[3] + fp2 * matrix[5] + matrix[10];
        param_2[1] = td5_round_toward_neg_inf_to_int32(tmp);
    }
    /* Output 2 — operand order from listing 0x0042EBB1..0x0042EBD6
     *   ST = p1*M7 + p0*M6 + p2*M8 + M11 */
    {
        float tmp = fp1 * matrix[7] + fp0 * matrix[6] + fp2 * matrix[8] + matrix[11];
        param_2[2] = td5_round_toward_neg_inf_to_int32(tmp);
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
    /* Pilot precise-port trace — snapshot inputs at entry. */
    td5_pilot_emit_00403720_enter(actor, (uintptr_t)__builtin_return_address(0));

    float *rot = actor->rotation_matrix.m;
    int resolved_surface = actor->surface_type_chassis;
    int resolved_surface_valid = 0;

    /* Entry snapshot: copy prior-tick airborne mask (+0x37C) into the OLD
     * slot (+0x37D). Matches original 0x00403793/0x004037D5:
     *   MOV CL, byte ptr [ESI+0x37C]
     *   MOV byte ptr [ESI+0x37D], CL
     * The per-wheel loop below will overwrite +0x37C with the freshly-
     * computed mask; +0x37D retains the OLD value for downstream gates
     * (velocity-snap @ 0x004060CE/D4, tumble-recovery transition logic). */
    actor->damage_lockout = actor->wheel_contact_bitmask;

    /* Step 1 (original 0x403720): seed each probe with the actor's
     * CURRENT span_index and sub_lane_index only. The walker (called
     * after the position transform below) updates span_index further
     * if the probe's world position crosses span boundaries, and also
     * increments span_accumulated / span_high_water on forward steps.
     *
     * span_normalized, span_accumulated, span_high_water, contact_vertex_A,
     * contact_vertex_B are NOT seeded here -- they carry over from the
     * previous tick and are updated by the walker + contactNormal
     * computation. At race start they are zero-initialized by the actor
     * memset, matching the original's tick-1 state. Whole-state diff
     * 2026-05-15 showed the prior init-time copies of actor->track_span_*
     * polluted span_norm to 102 on tick 1 while the original had 0. */
    for (int i = 0; i < 4; i++) {
        actor->wheel_probes[i].span_index     = actor->track_span_raw;
        actor->wheel_probes[i].sub_lane_index = (int8_t)actor->track_sub_lane_index;
    }

    /* Step 1b: same for body_probes (corners). The original runs TWO
     * symmetric loops -- the second writes to actor+0x00 (body_probes).
     * Without this, UpdateRaceOrder and scripted-recovery mode read
     * empty body-corner probe state. */
    for (int i = 0; i < 4; i++) {
        actor->body_probes[i].span_index     = actor->track_span_raw;
        actor->body_probes[i].sub_lane_index = (int8_t)actor->track_sub_lane_index;
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

        /* Faithful port of TransformShortVec3ByRenderMatrixRounded @ 0x0042EB10
         * followed by `<<8` in 0x00403720. The original transform consumes
         * float render_pos (= world_pos/256.0f) and FISTPs the integer result;
         * the `<<8` then forces wheel_contact_pos to a multiple of 256 in
         * 24.8 FP. Earlier port path truncated `(rot*body)` then added
         * world_pos directly (preserving its low 8 bits), producing wheel
         * positions that aren't multiples of 256 — every chassis-snap then
         * inherited those leaked low bits and produced the +128 FP world_y
         * delta + ~263..1017 FP wheel XZ deltas observed in physics_trace.csv.
         * [E2 / 2026-05-02 ground-truth diff finding]
         *
         * [PRECISE-PORT PILOT 2026-05-14] — replaced the previous in-line
         * `rot*body + render_pos` expression with the byte-faithful helper
         * `td5_transform_short_vec3_by_render_matrix_rounded`, which mirrors
         * the original's operand order and 80→32-bit precision collapse.
         */
        {
            const int16_t body_off[3] = {
                (int16_t)wx, (int16_t)wy, (int16_t)wz
            };
            float matrix[12];
            for (int j = 0; j < 9; j++) matrix[j] = rot[j];
            matrix[9]  = actor->render_pos.x;
            matrix[10] = actor->render_pos.y;
            matrix[11] = actor->render_pos.z;
            int32_t world[3];
            td5_transform_short_vec3_by_render_matrix_rounded(body_off, world, matrix);
            actor->wheel_contact_pos[i].x = world[0] << 8;
            actor->wheel_contact_pos[i].y = world[1] << 8;
            actor->wheel_contact_pos[i].z = world[2] << 8;

            td5_pilot_emit_0042EB10(
                (uint32_t)td5_trace_current_sim_tick(),
                actor->slot_index, PILOT_0042EB10_KIND_WCP, i,
                0x00403873u,  /* faux caller_ra matching Frida CSV column */
                body_off, &actor->wheel_contact_pos[i],
                body_off[0], body_off[1], body_off[2],
                matrix, world[0], world[1], world[2]);
        }

        /* Step C: stash int16-truncated body-rotated Y for chassis-snap.
         * Original `ConvertFloatVec3ToShortAngles @ 0x0042E2E0` writes
         * `(short)__ftol(by)` to actor+0x232+8*i; chassis-snap then reads
         * MOVSX EAX, [EBP+2]; SHL EAX, 8 → effectively `(int16)round(by) * 256`.
         * Port stashes the equivalent in s_wheel_offset_y_world for the
         * integrator's chassis-snap loop in update_vehicle_pose_from_physics.
         *
         * Recompute body-only Y (no render_pos add) in the same FADD order as
         * 0x0042EB10 output 1: p1*M4 + p0*M3 + p2*M5. No translation add.
         * Rounds toward -inf to match the original FPU control word. */
        {
            float by_tmp = (float)(int16_t)wy * rot[4]
                         + (float)(int16_t)wx * rot[3]
                         + (float)(int16_t)wz * rot[5];
            s_wheel_offset_y_world[slot & 0x0F][i] =
                (int32_t)(int16_t)floorf(by_tmp) * 256;
        }

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

        /* Mirror the original's ComputeActorTrackContactNormalExtended
         * prefix: write contact_vertex_A/B to the probe based on its
         * (post-walker) span_index + sub_lane_index. */
        td5_track_compute_probe_contact_vertices(&actor->wheel_probes[i]);

        /* Compute wheel vertical force from the probed span surface. */
        int32_t wheel_y = actor->wheel_contact_pos[i].y;
        int32_t ground_y = 0;
        int surface_type = actor->surface_type_chassis;
        int probe_span = actor->wheel_probes[i].span_index;

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
            /* Resolve surface_type from the probe's span+lane attributes
             * instead of echoing the stale actor->surface_type_chassis
             * (which is zero on tick 1 from the actor memset). Use the
             * first probe-index 4..7 (= wheel_probes[i]) on the first
             * valid wheel, matching the original's pattern of picking the
             * surface from the lead grounded wheel. */
            if (!resolved_surface_valid) {
                resolved_surface = td5_track_get_surface_type(actor, 4 + i);
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
                /* No clamp on the delta. Original 0x00403720 writes
                 * `(new - old) >> 8` with sign-extending round, NO bounds
                 * test (decomp: `*local_28 = (short)((*piVar1 - local_c) +
                 * (*piVar1 - local_c >> 0x1f & 0xffU) >> 8);`). Pilot Frida
                 * capture 2026-05-13 confirms orig writes large deltas
                 * verbatim. Prior `CLAMP_DELTA(±20000)` was an approximation
                 * for "teleport hides launch"; removing it per the
                 * byte-exact precise-port workflow. */
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

        /* Store high-res wheel world position.
         *
         * [FIXED 2026-05-12 via Ghidra audit of 0x00403720]: the original
         * runs TransformShortVec3ByRenderMatrixRounded TWICE — once with
         * body_y = (cwy - sp/256 - preload) writing wheel_contact_pos
         * (the ground-contact point), then ADDS BACK preload to body_y
         * (giving cwy - sp/256) and transforms AGAIN, writing that to
         * field_0x298 = wheel_world_positions_hires (the wheel hub).
         *
         * The two transforms produce DIFFERENT world X and Z because
         * rot[1] * preload contributes to the rotated X and Z when the
         * chassis is pitched/rolled. The port previously aliased
         * wheel_world_positions_hires = wheel_contact_pos, missing this
         * rot[1]*preload offset (~5-15 fp units at typical pitch angles
         * for Viper's preload, accumulating into wrong suspension lever
         * arms in td5_physics_integrate_suspension).
         *
         * Second transform: input body_y_no_preload = wy + href_preload
         * (where wy already had the preload subtracted at line 5295).
         *
         * NO `<<8` on the hires output. Original asm 0x40388A-0x40388E only
         * shifts wheel_contact_pos (piVar8.x/y/z), NOT the hires field at
         * actor+0x298. Confirmed via pilot Frida capture 2026-05-13:
         * orig hires_x at tick 0 = -461 (raw world unit), port was writing
         * -65280 = -255<<8 — i.e. shifted body offset, not unshifted world. */
        {
            const int16_t body_off[3] = {
                (int16_t)wx, (int16_t)(wy + href_preload), (int16_t)wz
            };
            float matrix[12];
            for (int j = 0; j < 9; j++) matrix[j] = rot[j];
            matrix[9]  = actor->render_pos.x;
            matrix[10] = actor->render_pos.y;
            matrix[11] = actor->render_pos.z;
            int32_t hub[3];
            td5_transform_short_vec3_by_render_matrix_rounded(body_off, hub, matrix);
            /* NO <<8 -- the original (0x40388A-0x40388E) writes the rounded
             * world-unit value straight into actor+0x298. Comment above
             * documents this; previous version applied the shift in error,
             * producing values 256x too large. Verified via whole-state diff
             * 2026-05-15: port hub<<8 = -17829120 vs original = -69644 = hub. */
            actor->wheel_world_positions_hires[i].x = hub[0];
            actor->wheel_world_positions_hires[i].y = hub[1];
            actor->wheel_world_positions_hires[i].z = hub[2];

            td5_pilot_emit_0042EB10(
                (uint32_t)td5_trace_current_sim_tick(),
                actor->slot_index, PILOT_0042EB10_KIND_HIRES, i,
                0x0040388Au,  /* faux caller_ra matching Frida CSV column */
                body_off, &actor->wheel_world_positions_hires[i],
                body_off[0], body_off[1], body_off[2],
                matrix, hub[0], hub[1], hub[2]);
        }

    }

    /* Mark this slot's pre-snap transform as valid for next frame */
    s_prev_wheel_valid[slot] = 1;

    /* Original (0x00403720) runs a SECOND loop that walks each body
     * corner probe (body_probes[0..3]) through the per-probe walker and
     * ComputeActorTrackContactNormal, mirroring the wheel-probe loop.
     *
     * Body corner offsets are stored at the head of car_definition_ptr as
     * 4 x short[4] (8-byte stride, last short ignored). Each is the body
     * corner position in chassis-local space; transforming via the
     * (rot, render_pos) matrix and shifting <<8 yields the corner's world
     * position in 24.8 FP, written to actor->probe_FL/FR/RL/RR.
     *
     * Whole-state diff 2026-05-15: replaced the prior wheel_contact alias
     * (which used wheel offsets instead of body corners) with a faithful
     * port of the original's TransformShortVec3ByRenderMatrixRounded
     * pass. The walker + ComputeActorTrackContactNormal helper write
     * span_acc/hw and contact_vertex_A/B per body probe. */
    if (actor->car_definition_ptr) {
        TD5_Vec3_Fixed *body_pos[4] = {
            &actor->probe_FL,
            &actor->probe_FR,
            &actor->probe_RL,
            &actor->probe_RR,
        };
        const int16_t *cardef_corners = (const int16_t *)actor->car_definition_ptr;

        float matrix[12];
        for (int j = 0; j < 9; j++) matrix[j] = rot[j];
        matrix[9]  = actor->render_pos.x;
        matrix[10] = actor->render_pos.y;
        matrix[11] = actor->render_pos.z;

        for (int i = 0; i < 4; i++) {
            const int16_t corner_off[3] = {
                cardef_corners[i * 4 + 0],
                cardef_corners[i * 4 + 1],
                cardef_corners[i * 4 + 2],
            };
            int32_t world[3];
            td5_transform_short_vec3_by_render_matrix_rounded(corner_off, world, matrix);
            body_pos[i]->x = world[0] << 8;
            body_pos[i]->y = world[1] << 8;
            body_pos[i]->z = world[2] << 8;

            td5_track_update_probe_position(&actor->body_probes[i],
                                            body_pos[i]->x,
                                            body_pos[i]->z);
            int max_sp = td5_track_get_span_count();
            if (max_sp > 0 && actor->body_probes[i].span_index >= (int16_t)max_sp)
                actor->body_probes[i].span_index = (int16_t)(max_sp - 1);
            td5_track_compute_probe_contact_vertices(&actor->body_probes[i]);

            /* Original ComputeActorTrackContactNormal (0x00445450) overwrites
             * piVar8[1] (= probe_FL/FR/RL/RR .y) with the barycentric ground
             * height at the body corner's XZ. Use the same compute_contact_
             * height path the wheel loop uses; pass NULL for the normal
             * out-pointer since the body probe doesn't store one. */
            int probe_span = actor->body_probes[i].span_index;
            int probe_lane = actor->body_probes[i].sub_lane_index;
            if (probe_span >= 0 && probe_span < max_sp) {
                body_pos[i]->y = td5_track_compute_contact_height_with_normal(
                    probe_span, probe_lane,
                    body_pos[i]->x, body_pos[i]->z, NULL);
            }
        }
    }

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

    /* Pilot precise-port trace — emit row(s) at exit. */
    td5_pilot_emit_00403720_leave(actor);
}

/* ========================================================================
 * ClampVehicleAttitudeLimits (0x405B40) — precise-port (pool4 / session 20)
 *
 * Listing: 0x00405B40..0x00405D65 (Ghidra TD5_pool4, 2026-05-14)
 * Audit:   re/analysis/pilot_00405B40_audit.md
 *
 * Reads two 12-bit display angles (roll, pitch) and dispatches on the
 * global at 0x00463188 (Ghidra "g_cameraMode" -- actually the user's
 * 3D-collisions option):
 *   *0x00463188 == 0 -> MODE-0 (matrix recovery latch)
 *   *0x00463188 != 0 -> MODE-1 (soft nudge + hard clamp)
 *
 * The port mirrors g_collisions_enabled to this dword. NOTE: the SETTER
 * `td5_physics_set_collisions(int enabled)` writes the value INVERTED vs
 * the original ("0=on, 1=off" comment in line ~6983); that is a separate
 * upstream binding bug. This function ports the LISTING faithfully -- it
 * reads g_collisions_enabled with the same semantics as the original reads
 * *0x00463188 (so the branch routing matches the LISTING, given that the
 * port's value at this address means the same as the original's value).
 * ======================================================================== */

void td5_physics_clamp_attitude(TD5_Actor *actor)
{
    /* Pilot precise-port trace — emit input snapshot at entry. */
    td5_pilot_emit_00405B40_enter(actor, (uintptr_t)__builtin_return_address(0));

    /* === Read 12-bit display angles + center-shift unwrap [0x00405B40-B85] ===
     *
     * Original: EAX = (uint16) actor->display_angle_roll  [+0x208]
     *           ECX = (uint16) actor->display_angle_pitch [+0x20C]
     *           iVar1 = ((EAX - 0x800) & 0xFFF) - 0x800   ; signed[-0x800..+0x7FF]
     *           iVar2 = ((ECX - 0x800) & 0xFFF) - 0x800
     *
     * The center-shift unwrap maps the raw uint16 to signed [-0x800,+0x7FF].
     * Critically, val == 0x800 maps to -0x800 (signed wrap), not +0x800.
     * The earlier port used (val >> 8) & 0xFFF with "if (v > 0x800) v -= 0x1000"
     * which off-by-ones at v == 0x800. */
    int32_t raw_roll  = (uint16_t)actor->display_angles.roll;
    int32_t raw_pitch = (uint16_t)actor->display_angles.pitch;
    int32_t iVar1 = ((raw_roll  - 0x800) & 0xFFF) - 0x800;  /* signed roll  */
    int32_t iVar2 = ((raw_pitch - 0x800) & 0xFFF) - 0x800;  /* signed pitch */

    /* === Dispatch on collisions flag [0x00405B86-B88] === */
    int branch_taken = 0;  /* 0=skip, 1=mode1, 2=mode0-latch */
    if (g_collisions_enabled != 0) {
        branch_taken = 1;
        /* MODE-1: soft nudge then hard clamp [0x00405B8E-C3D].
         *
         * Original layout is 8 straight-line independent `if`s with no
         * combined early-out. The signed comparisons match the listing
         * directly. Each hard-clamp branch writes BOTH the display_angles
         * field AND the euler_accum field (the earlier port omitted the
         * display_angles write). */

        /* Soft nudge: pull angular velocity back toward 0 with a fixed
         * +/-0x200 step when |angle| > nudge threshold [0x00405B8E-CE].
         * Original constants: ESI = +0x200, EDX = -0x200.
         * Roll nudge bound: 0x27F (listing CMP EAX, 0xfffffd81 / 0x27f).
         * Pitch nudge bound: 0x2BB (listing CMP ECX, 0xfffffd45 / 0x2bb). */
        if (iVar1 < -0x27F) actor->angular_velocity_roll  += 0x200;
        if (iVar1 >  0x27F) actor->angular_velocity_roll  += -0x200;
        if (iVar2 < -0x2BB) actor->angular_velocity_pitch += 0x200;
        if (iVar2 >  0x2BB) actor->angular_velocity_pitch += -0x200;

        /* Hard clamp: zero omega and pin angle at +/-limit [0x00405BCE-C3D].
         * Roll  limit: 0x355  (listing CMP EAX, 0xfffffcab / 0x355).
         * Pitch limit: 0x3A4  (listing CMP ECX, 0xfffffc5c / 0x3a4).
         *
         * Both display_angle (+0x208 / +0x20C) AND euler_accum (+0x1F0 / +0x1F8)
         * are written. euler_accum value is the display value shifted left by 8. */
        if (iVar1 < -0x355) {
            actor->angular_velocity_roll = 0;
            actor->display_angles.roll   = (int16_t)0xFCAB;  /* (uint16)-0x355 */
            actor->euler_accum.roll      = (int32_t)0xFFFCAB00; /* -0x35500 */
        }
        if (iVar1 >  0x355) {
            actor->angular_velocity_roll = 0;
            actor->display_angles.roll   = (int16_t)0x355;
            actor->euler_accum.roll      = 0x35500;
        }
        if (iVar2 < -0x3A4) {
            actor->angular_velocity_pitch = 0;
            actor->display_angles.pitch   = (int16_t)0xFC5C;  /* (uint16)-0x3A4 */
            actor->euler_accum.pitch      = (int32_t)0xFFFC5C00; /* -0x3A400 */
        }
        if (iVar2 >  0x3A4) {
            actor->angular_velocity_pitch = 0;
            actor->display_angles.pitch   = (int16_t)0x3A4;
            actor->euler_accum.pitch      = 0x3A400;
            /* original early-returns here as a compiler artifact (POP chain
             * split); functionally equivalent to falling through to RET. */
            td5_pilot_emit_00405B40_leave(actor, branch_taken);
            return;
        }
        td5_pilot_emit_00405B40_leave(actor, branch_taken);
        return;
    }

    /* === MODE-0: matrix recovery latch [0x00405C5C-D65] ===
     *
     * If |roll| <= 0x355 AND |pitch| <= 0x3A4, no-op early return.
     * Otherwise: build a delta-rotation matrix from (eul - omega) >> 8,
     * post-multiply by actor->rotation_matrix, store the product in
     * collision_spin_matrix (+0x180), snapshot rotation_matrix into
     * saved_orientation (+0x150), set vehicle_mode=1, zero frame_counter.
     *
     * NOTE: the earlier port had this branch disabled with a comment
     * about suspension equilibrium drift triggering spurious latches.
     * Per the precise-port mandate, this function is now ported
     * faithfully; the upstream suspension drift fix is out of scope. */

    /* Inside-limits early-out [0x00405C5C-78] */
    if (iVar1 >= -0x355 && iVar1 <= 0x355 &&
        iVar2 >= -0x3A4 && iVar2 <= 0x3A4) {
        td5_pilot_emit_00405B40_leave(actor, branch_taken);  /* branch_taken==0 here */
        return;
    }
    branch_taken = 2;

    /* Build delta-rotation angles: (eul - omega) >> 8 [0x00405C7E-CB].
     * Original SAR is signed arithmetic shift right; signed int32 >> 8 in C
     * is arithmetic for the platform we target (i686). The result is
     * truncated to int16 via the `MOV word ptr [ESP+offset], reg` stores. */
    int16_t delta_roll  = (int16_t)(((int32_t)(actor->euler_accum.roll  - actor->angular_velocity_roll))  >> 8);
    int16_t delta_yaw   = (int16_t)(((int32_t)(actor->euler_accum.yaw   - actor->angular_velocity_yaw))   >> 8);
    int16_t delta_pitch = (int16_t)(((int32_t)(actor->euler_accum.pitch - actor->angular_velocity_pitch)) >> 8);

    /* BuildRotationMatrixFromAngles(&local_30, {delta_roll, delta_yaw, delta_pitch})
     * [CALL 0x0042E1E0 at 0x00405CC8].  Original takes a short* with three
     * angles laid out as roll/yaw/pitch. */
    int16_t delta_angles[3] = { delta_roll, delta_yaw, delta_pitch };
    float build_mat[9];
    BuildRotationMatrixFromAngles(build_mat, delta_angles);

    /* The original then shuffles the build output through `local_30..local_10`
     * back into `local_60[0..8]`. The shuffle is identity for the port-side
     * helper (which writes its 9-float row-major output directly into the
     * destination buffer). Skip the shuffle.
     *
     * MultiplyRotationMatrices3x3(&actor->rotation_matrix, local_60, local_60)
     * [CALL 0x0042DA10 at 0x00405D26]. */
    float product[9];
    MultiplyRotationMatrices3x3((float *)&actor->rotation_matrix,
                                build_mat, product);

    /* MOVSD.REP 12-dword copies [0x00405D2B-4C].
     *
     * Original copies 48 bytes (12 dwords) to BOTH destinations:
     *   - +0x180 collision_spin_matrix <- product (9 floats) + 3 trailing
     *     dwords from stack (originally the first row of the BuildRotation
     *     output before the shuffle). The trailing 3 dwords land in
     *     gap_1A4[12] which the actor struct marks "unused/reserved".
     *   - +0x150 saved_orientation <- rotation_matrix (9 floats) + 3
     *     trailing floats which are actor->render_pos at +0x144..+0x14F.
     *     The trailing 3 dwords land in gap_174[4] + gap_178[8] which
     *     the actor struct also marks "unused/reserved".
     *
     * Per static-port mandate we replicate the 48-byte copy semantics
     * exactly via memcpy. The trailing 12 bytes are stack/render_pos
     * residue but the original writes them, so we do too. */
    memcpy(&actor->collision_spin_matrix, product, 9 * sizeof(float));
    /* gap_1A4 trailing 12 bytes: original copies stack residue (the first
     * row of the pre-shuffle BuildRotation output, which the shuffle then
     * read into local_60). The exact values are stack garbage from the
     * compiler's perspective, but bit-exact matching means writing the
     * post-shuffle values that remain at the source addresses. Given the
     * port doesn't perform the shuffle (it writes Build output directly to
     * build_mat[0..8]), the equivalent residue is build_mat[0..2]. */
    memcpy(((uint8_t *)&actor->collision_spin_matrix) + 9 * sizeof(float),
           build_mat, 3 * sizeof(float));

    memcpy(&actor->saved_orientation, &actor->rotation_matrix, 9 * sizeof(float));
    /* gap_174/178 trailing 12 bytes: original copies the 12 bytes following
     * rotation_matrix, which is render_pos (+0x144..+0x14F). */
    memcpy(((uint8_t *)&actor->saved_orientation) + 9 * sizeof(float),
           &actor->render_pos, 3 * sizeof(float));

    /* Set recovery state flags [0x00405D4E-5D].
     *   MOV byte ptr [EBX+0x379], 0x1   -> vehicle_mode = 1
     *   MOV word ptr [EBX+0x338], 0x0   -> frame_counter = 0 (int16 write) */
    actor->vehicle_mode = 1;
    actor->frame_counter = 0;

    /* Pilot precise-port trace — emit row at exit. */
    td5_pilot_emit_00405B40_leave(actor, branch_taken);
}

/* Accessor for the pilot trace emitter to read the collisions flag without
 * exposing the file-static g_collisions_enabled. */
int td5_physics_get_collisions_flag(void)
{
    return g_collisions_enabled;
}

/* ========================================================================
 * ResetVehicleActorState (0x405D70)
 *
 * Resets vehicle to initial conditions (respawn/reset).
 * ======================================================================== */

void td5_physics_reset_actor_state(TD5_Actor *actor)
{
    /* Byte-faithful port of ResetVehicleActorState @ 0x00405D70 (54 instr).
     * Every write below corresponds to a specific store in the listing.
     * Field-by-field map: re/analysis/pilot_00405D70_audit.md.
     *
     * The original touches EXACTLY 25 unique offsets. Earlier port versions
     * added ~16 extra writes (slip, steering, tire-track-emitter IDs,
     * center_suspension, damage_lockout, etc.). Those are removed here —
     * the dependent paths reinitialise those fields elsewhere, or rely on
     * residual values surviving the reset (notably the traffic recyclers
     * 0x004353B0 / 0x00435940 / 0x00434DA0 which expect slip/steering
     * history to persist across the call). */

    /* Capture PRE-state for the pilot probe BEFORE any mutation. */
    td5_pilot_trace_00405D70_enter(actor);

    /* 0x405D78 / 0x405D7E — clear flag bytes */
    actor->surface_contact_flags = 0;       /* +0x376 */
    actor->vehicle_mode = 0;                /* +0x379 */

    /* 0x405D84-DA6 — zero 6 dwords: ang_vel + lin_vel block */
    actor->angular_velocity_roll = 0;       /* +0x1C0 */
    actor->angular_velocity_yaw = 0;        /* +0x1C4 */
    actor->angular_velocity_pitch = 0;      /* +0x1C8 */
    actor->linear_velocity_x = 0;           /* +0x1CC */
    actor->linear_velocity_y = 0;           /* +0x1D0 */
    actor->linear_velocity_z = 0;           /* +0x1D4 */

    /* 0x405DA8 — zero frame_counter (int16) */
    actor->frame_counter = 0;               /* +0x338 */

    /* 0x405DAF — zero wheel_contact_bitmask (NOT damage_lockout at 0x37D) */
    actor->wheel_contact_bitmask = 0;       /* +0x37C */

    /* 0x405DB5 — sentinel world_pos.y so the upcoming integrate ground-snaps
     * via the per-wheel refresh_wheel_contacts -> wheel_contact_pos averaging
     * path. force = (sentinel - ground_y) + gravity is hugely negative
     * (< 0x801), so the grounded branch in refresh_wheel_contacts snaps
     * wheel_contact_pos[i].y = ground_y, and integrate_pose then averages
     * those 4 per-wheel values into world_pos.y. [CONFIRMED @ 0x00405D70] */
    actor->world_pos.y = (int32_t)0xC0000000;   /* +0x200 */

    /* 0x405DBF — gear = 2 (first forward) */
    actor->current_gear = TD5_GEAR_FIRST;       /* +0x36B */

    /* 0x405DC6 — engine_speed_accum = 0x190 (idle RPM) */
    actor->engine_speed_accum = TD5_ENGINE_IDLE_RPM;  /* +0x310 */

    /* 0x405DD0-E4 — loop: wheel_suspension_pos[0..3] and wheel_spring_dv[0..3]
     * are zeroed. The integrator (0x4057F0) settles to the correct equilibrium
     * on the first tick from zero, producing the asymmetric front/rear
     * positions purely from gravity-driven load_accum signs.
     * Earlier "preload = (href*5+4)/9" heuristic produced +54/+43 FP delta vs
     * original (frida physics_trace.csv 2026-05-02). */
    for (int i = 0; i < 4; i++) {
        actor->wheel_suspension_pos[i] = 0;     /* +0x2DC + i*4 */
        actor->wheel_spring_dv[i] = 0;          /* +0x2EC + i*4 */
    }

    /* 0x405DE6-E41 — render_pos = (float)world_pos * 1/256, interleaved with
     * display_angles/euler_accum stores. Source-ordering interleave is for
     * x87/integer pipeline scheduling and has no visible effect since all
     * fields are disjoint.
     *
     * DAT_0045D5E8 = 0x3B800000f = 1.0f/256.0f (verified via memory_read). */
    actor->display_angles.roll  = 0;        /* +0x208 */
    actor->euler_accum.roll     = 0;        /* +0x1F0 */
    actor->display_angles.pitch = 0;        /* +0x20C */
    actor->euler_accum.pitch    = 0;        /* +0x1F8 */
    /* SAR EAX, 8 on signed dword → low16 captured by MOV [+0x20A], AX.
     * Original has no &0xFFF mask; keep the int16 truncate faithful. */
    actor->display_angles.yaw = (int16_t)(actor->euler_accum.yaw >> 8);  /* +0x20A */

    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);  /* +0x144 */
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);  /* +0x148 */
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);  /* +0x14C */

    /* 0x405E47 — CALL IntegrateVehiclePoseAndContacts (settles suspension
     * + ground-snaps world_pos.y from the sentinel). */
    td5_physics_integrate_pose(actor);

    /* 0x405E4C-66 — zero wheel_load_accum[0..3] after the integrate */
    for (int i = 0; i < 4; i++)
        actor->wheel_load_accum[i] = 0;         /* +0x2FC + i*4 */

    /* 0x405E69-75 — re-zero a subset of dynamics overwritten by integrate */
    actor->angular_velocity_roll  = 0;          /* +0x1C0 */
    actor->angular_velocity_pitch = 0;          /* +0x1C8 */
    actor->linear_velocity_y      = 0;          /* +0x1D0 */

    /* === pilot trace hook ===
     * Fires once per reset (rare event — spawn/respawn/recycle); negligible
     * cost. Schema in tools/diff_func_trace.py reads addr=0x00405D70. */
    td5_pilot_trace_00405D70_leave(actor);
}

/* ========================================================================
 * ApplyMissingWheelVelocityCorrection (0x403EB0) -- BYTE-FAITHFUL PORT
 *
 * Listing-driven port of the original. When the wheel-contact bitmask at
 * actor+0x37C is in the "asymmetric" set, subtract a velocity bias from
 * actor+0x1C8 (angular_velocity_pitch in port naming, but the original
 * targets it as a raw int32 dword) computed from:
 *
 *   1. Average of the second int16 (offset +2) of each MISSING wheel's
 *      contact-normal slot at actor+0x230 (8-byte stride, 4 wheels).
 *   2. Multiplied by heading_normal.y (int16 @ actor+0x292), then
 *      round-to-zero >> 12.
 *   3. Body-frame projection of (linear_velocity_x, linear_velocity_z)
 *      by display_angles.yaw (int16 @ actor+0x20A): each component
 *      round-to-zero >>8, multiplied by cos / sin, subtracted, then
 *      round-to-zero >> 12 and clamped to [-0x200, +0x200].
 *   4. correction = (proj_clamped * avg_norm * 4), round-to-zero >> 8.
 *   5. actor+0x1C8 -= correction.
 *
 * The "break" set (jump-table 0x404014 + index table 0x40401C, verified
 * via memory_read 2026-05-14) is {0, 1, 2, 4, 6, 8, 9, 0xF}; everything
 * else (3, 5, 7, A, B, C, D, E, plus any mask >0xF via the JA fall-through
 * at 0x403EC1) takes the default body. The original has no divide-by-zero
 * guard: for the "default" masks in 0..0xF, count is always >= 1.
 * Bitmasks > 0xF (high bits set) could theoretically zero the count and
 * crash; the original accepts that hazard and so does this port.
 * ======================================================================== */

/* Round-to-zero arithmetic shift right.
 * Mirrors original CDQ/AND mask/ADD/SAR idiom: for negative non-divisible
 * x, yields one unit closer to zero than plain SAR. */
static inline int32_t mwvc_sar_rz(int32_t x, int n) {
    int32_t mask = (1 << n) - 1;
    return (x + ((x >> 31) & mask)) >> n;
}

void td5_physics_missing_wheel_correction(TD5_Actor *actor)
{
    uint8_t *ap = (uint8_t *)actor;
    uint32_t mask = (uint32_t)*(uint8_t *)(ap + 0x37C);   /* zero-extended BL */

    /* [0x403EBE..EC1] CMP EBX,0xF / JA 0x00403ED2 -- masks > 0xF take the
     * default body. For mask in 0..0xF, dispatch via the 16-byte index
     * table at 0x40401C (0=break, 1=default-body). */
    if (mask <= 0xFu) {
        /* Index table contents verified at 0x40401C:
         *   00 00 00 01 00 01 00 01 00 00 01 01 01 01 01 00
         * i.e. break for mask in {0,1,2,4,6,8,9,0xF}. */
        switch (mask) {
        case 0x0: case 0x1: case 0x2:
        case 0x4: case 0x6:
        case 0x8: case 0x9:
        case 0xF:
            return;
        default:
            break;  /* fall through to default body */
        }
    }

    /* --- Default body (0x403ED2..0x404012) --- */

    /* [0x403ED2..EFE] Initialize the four globals used as scratch by the
     * loop. The port stores them as locals; the original publishes them
     * to DAT_004830XX but those are not read by any other function in
     * the static call graph (verified: only this function writes them). */
    int16_t *wcn_p = (int16_t *)(ap + 0x230);   /* DAT_00483024: wheel_contact_normals base */
    uint32_t i_idx = 0;                          /* DAT_00483028: wheel index counter */
    int32_t  sum   = 0;                          /* DAT_00483020: accumulator (EAX) */
    int32_t  cnt   = 0;                          /* DAT_00483038: missing-wheel count (EDI) */
    int16_t *hn_p  = (int16_t *)(ap + 0x290);   /* DAT_00483044: heading_normal base */

    /* [0x403F03..F33] Loop: for each of 4 wheels, if its bit is clear in
     * the mask, add wcn[i+1] (the second int16 at offset +2) to sum and
     * increment cnt. wcn_p advances by 4 int16s (8 bytes) per iter. */
    do {
        uint32_t bit = 1u << (i_idx & 0x1Fu);
        if ((mask & bit) == 0) {
            sum += (int32_t)wcn_p[1];      /* MOVSX EBP, word ptr [EDX+0x2] */
            cnt += 1;
        }
        wcn_p += 4;                         /* ADD EDX, 0x8 */
        i_idx += 1;
    } while (i_idx < 4);

    /* [0x403F35..F4E] sum = (sum / cnt) * heading_normal.y, round-to-zero >> 12.
     * IDIV is signed; cnt is always >= 1 here (asserted by the dispatch
     * filter above for masks 0..0xF). */
    sum = (sum / cnt) * (int32_t)hn_p[1];           /* IMUL EAX, [ESI+0x292] */
    int32_t avg_norm = mwvc_sar_rz(sum, 12);        /* CDQ/AND 0xFFF/ADD/SAR 12 */
    /* avg_norm == DAT_00483020 at this point */

    /* [0x403F51..F8D] Cos/Sin of display_angles.yaw (int16 @ +0x20A).
     * Both calls take the same MOVSX-promoted argument. Original then
     * stores results to DAT_00483034 (cos) and DAT_0048303C/ECX (sin). */
    int32_t yaw = (int32_t)*(int16_t *)(ap + 0x20A);
    int32_t cos_v = cos_fixed12(yaw);               /* CALL 0x0040A6E0 */
    int32_t sin_v = sin_fixed12(yaw);               /* CALL 0x0040A700 */

    /* [0x403F8F..FC6] Body-frame projection of (lin_vel_x @+0x1CC,
     * lin_vel_z @+0x1D4) by yaw:
     *
     *     proj = ((vx + rz_mask_8) >> 8) * cos - ((vz + rz_mask_8) >> 8) * sin
     *     proj = round_to_zero_12(proj)
     *
     * Each load uses round-to-zero >>8 BEFORE the IMUL, matching the
     * CDQ/AND 0xFF/ADD/SAR 8 idiom in the listing. Note ordering: cos
     * pairs with linear_velocity_x (the first int32 at [EDI]), sin with
     * linear_velocity_z (the third int32 at [EDI+8]). */
    int32_t *vel = (int32_t *)(ap + 0x1CC);         /* DAT_00483040 */
    int32_t vx_h = mwvc_sar_rz(vel[0], 8);          /* (lin_vel_x rounded) >> 8 */
    int32_t vz_h = mwvc_sar_rz(vel[2], 8);          /* (lin_vel_z rounded) >> 8 */
    int32_t proj = vx_h * cos_v - vz_h * sin_v;
    int32_t proj_clamped = mwvc_sar_rz(proj, 12);   /* CDQ/AND 0xFFF/ADD/SAR 12 */

    /* [0x403FC9..FEA] Clamp proj_clamped to [-0x200, +0x200]. The original
     * stores back to DAT_00483048 after the clamp. */
    if (proj_clamped < -0x200) {
        proj_clamped = -0x200;
    } else if (proj_clamped > 0x200) {
        proj_clamped = 0x200;
    }

    /* [0x403FEF..04005] correction = (proj_clamped * avg_norm * 4) rz>>8. */
    int32_t prod = proj_clamped * avg_norm;
    prod <<= 2;                                      /* SHL EAX, 0x2 */
    int32_t correction = mwvc_sar_rz(prod, 8);       /* CDQ/AND 0xFF/ADD/SAR 8 */

    /* [0x0040400A] *(int32_t *)(actor + 0x1C8) -= correction. */
    *(int32_t *)(ap + 0x1C8) -= correction;
}

/* ========================================================================
 * UpdateVehicleState0fDamping (0x403D90)
 *
 * "Stunned" state: zero forces, 1/16 velocity decay per frame.
 * ======================================================================== */

/* Round-to-zero arithmetic shift right.
 * Mirrors original CDQ/AND mask/ADD/SAR idiom: for negative non-divisible x,
 * yields one unit closer to zero than plain SAR. Listing pattern @ 0x403E00..0E,
 * 0x403E44..4D, 0x403E60..66, 0x403E77..7F. */
static inline int32_t state0f_sar_rz(int32_t x, int n) {
    int32_t mask = (1 << n) - 1;
    return (x + (((x) >> 31) & mask)) >> n;
}

#ifdef TD5_PILOT_TRACE_00403D90
#include "td5_pilot_trace_00403D90.h"
#endif

void td5_physics_state0f_damping(TD5_Actor *actor)
{
#ifdef TD5_PILOT_TRACE_00403D90
    td5_pilot_emit_00403D90_enter(actor, (uintptr_t)__builtin_return_address(0));
#endif

    /* Keep engine alive [@ 0x403D9E CALL 0x0042ED50] */
    update_engine_speed_smoothed(actor);

    /* Integrate wheel suspension with zero chassis-motion excitation.
     * [@ 0x403DA5..A9: PUSH 0; PUSH 0; PUSH cardef; PUSH actor; CALL 0x403A20] */
    td5_physics_integrate_suspension(actor, 0, 0);

    /* Zero surface contact flags and slip [@ 0x403DB8/DBE/DC4 — written
     * BEFORE the cos/sin calls in the listing, but the sequence is order-
     * independent because cos/sin use only register state]. */
    actor->surface_contact_flags = 0;
    actor->front_axle_slip_excess = 0;
    actor->rear_axle_slip_excess = 0;

    /* Body-frame longitudinal projection.
     *
     * Original sources display_angle_yaw (+0x20A int16) directly, NEG it,
     * and passes -yaw to both Cos and Sin. Then truncates vx>>8, vz>>8 to
     * int16 BEFORE multiplying.
     * [@ 0x403DAE MOVSX EDI, [ESI+0x20A]; 0x403DB5 NEG EDI;
     *  0x403DCA CALL CosFixed12bit; 0x403DD2 CALL SinFixed12bit;
     *  0x403DDD..F7 SAR+MOVSX+IMUL+SUB sequence] */
    int32_t neg_yaw = -(int32_t)actor->display_angles.yaw;     /* +0x20A int16 */
    int32_t cos_neg_yaw = cos_fixed12((uint32_t)neg_yaw);
    int32_t sin_neg_yaw = sin_fixed12(neg_yaw);

    int32_t vz_hi = (int32_t)(int16_t)((uint32_t)actor->linear_velocity_z >> 8);
    int32_t vx_hi = (int32_t)(int16_t)((uint32_t)actor->linear_velocity_x >> 8);

    /* proj = (int16)(vx>>8) * cos(-yaw) - (int16)(vz>>8) * sin(-yaw)
     * Equivalently: (vx>>8)*cos(yaw) + (vz>>8)*sin(yaw) (body-frame longitudinal)
     * [@ 0x403DF1..F7] */
    int32_t proj = vx_hi * cos_neg_yaw - vz_hi * sin_neg_yaw;

    /* sVar2 = ROUND-TO-ZERO( proj / 4096 ), then truncated to int16
     * [@ 0x403E02 CDQ; 0x403E03 AND EDX,0xfff; 0x403E09 ADD EAX,EDX;
     *  0x403E0B SAR EAX,0xc] */
    int32_t proj_rz = state0f_sar_rz(proj, 12);
    int16_t sVar2 = (int16_t)proj_rz;

    /* roll12 = (((uint16)display_angle_roll - 0x800) & 0xfff) - 0x800
     * Folds [0,0xfff] to signed [-0x800, 0x7FF].
     * [@ 0x403DFB MOV CX,[ESI+0x208]; 0x403E11..1D SUB/AND/SUB sequence] */
    int32_t roll12 = (int32_t)(((uint32_t)(uint16_t)actor->display_angles.roll - 0x800u) & 0xfffu);
    roll12 -= 0x800;

    /* Gate (APPLY when |sVar2| > 0x20 AND sign-compatible |roll12| < 0x80):
     * Listing 0x403E23..3C decision tree:
     *   CMP AX,0x20; JLE e33        — sVar2 <= 0x20 ?
     *     [e33] CMP AX,-0x20; JGE SKIP   — if -0x20<=sVar2<=0x20 → skip
     *           CMP ECX,-0x80; JLE SKIP  — sVar2 < -0x20: skip if roll12 <= -0x80
     *           else APPLY
     *   else (sVar2 > 0x20):
     *           CMP ECX,0x80;  JGE SKIP  — skip if roll12 >= 0x80
     *           else APPLY
     * @ 0x403E23..3E.
     * NOTE: prior port had the gate INVERTED (apply on low speed). The correct
     * semantic is "recover from sustained body-frame longitudinal drift when
     * roll is small in the corresponding direction". */
    int apply = 0;
    if (sVar2 > 0x20) {
        if (roll12 < 0x80) apply = 1;
    } else if (sVar2 < -0x20) {
        if (roll12 > -0x80) apply = 1;
    }

    if (apply) {
        /* angular_velocity_roll += sar_rz(sVar2, 2)
         * [@ 0x403E3E..52: MOV ECX,[ESI+0x1C0]; MOVSX EAX,AX; CDQ; AND EDX,3;
         *  ADD EAX,EDX; SAR EAX,2; ADD ECX,EAX; MOV [ESI+0x1C0],ECX] */
        actor->angular_velocity_roll += state0f_sar_rz((int32_t)sVar2, 2);
    }

    /* Decay roll and pitch angular velocities by 1/16 per frame with RZ rounding.
     * [@ 0x403E58..6B: roll  — av -= sar_rz(av,4)]
     * [@ 0x403E71..A5: pitch — same pattern, +0x1C8] */
    actor->angular_velocity_roll  -= state0f_sar_rz(actor->angular_velocity_roll,  4);
    actor->angular_velocity_pitch -= state0f_sar_rz(actor->angular_velocity_pitch, 4);

    /* Accumulate slip from already-stored body-frame speeds.
     * IMPORTANT: original reads lateral_speed (+0x318) / longitudinal_speed (+0x314)
     * directly — NOT re-projecting from world velocity. Both are int32 (24.8 fp);
     * plain SAR 8 + 16-bit truncate (DX/AX register) is used (no RZ rounding here).
     * [@ 0x403E7F MOV EDX,[ESI+0x318]; 0x403E8A MOV EAX,[ESI+0x314];
     *  0x403E90 SAR EDX,8; 0x403E93 ADD word[ESI+0x340],DX;
     *  0x403E9A SAR EAX,8; 0x403E9D ADD word[ESI+0x342],AX] */
    actor->accumulated_tire_slip_x += (int16_t)(actor->lateral_speed >> 8);
    actor->accumulated_tire_slip_z += (int16_t)(actor->longitudinal_speed >> 8);

#ifdef TD5_PILOT_TRACE_00403D90
    td5_pilot_emit_00403D90_leave(actor);
#endif
}

/* ========================================================================
 * Engine & Transmission
 * ======================================================================== */

/* --- UpdateVehicleEngineSpeedSmoothed (0x0042ED50) ---
 *
 * Byte-faithful port of FUN_0042ED50 (RE: TD5_pool11 read-only listing,
 * audited 2026-05-14). Mirrors the original x86 control flow line-for-line:
 *
 *   0x42ED50  PUSH ESI                          ; entry
 *   0x42ED51  MOV  ESI, [ESP+8]                 ; actor
 *   0x42ED55  MOV  EAX, [ESI+0x1bc]             ; tuning_data_ptr
 *   0x42ED5B  MOVSX ECX, WORD [EAX+0x72]        ; ECX = redline (int)
 *   0x42ED5F  MOV  AL,  BYTE [ESI+0x36d]        ; AL  = brake_flag
 *   0x42ED65  TEST AL, AL
 *   0x42ED67  JNZ  0x42ED9A                     ; brake → idle (400)
 *   0x42ED69  MOV  DX,  WORD [ESI+0x33e]        ; DX  = encounter_steering_cmd
 *   0x42ED70  TEST DX, DX
 *   0x42ED73  JL   0x42ED9A                     ; throttle<0 → idle (400)
 *   0x42ED75  MOVSX EDX, DX                     ; EDX = throttle (int)
 *   0x42ED78  LEA  EAX, [ECX-0x190]             ; EAX = redline - 400
 *   0x42ED7E  IMUL EAX, EDX                     ; EAX *= throttle
 *   0x42ED81  CDQ
 *   0x42ED82  AND  EDX, 0xff                    ; round-toward-zero bias
 *   0x42ED88  ADD  EAX, EDX
 *   0x42ED8A  SAR  EAX, 8                       ; >> 8 (signed, round->0)
 *   0x42ED8D  CMP  ECX, EAX
 *   0x42ED8F  JGE  0x42ED93
 *   0x42ED91  MOV  EAX, ECX                     ; EAX = min(EAX, redline)
 *   0x42ED93  ADD  EAX, 0x190                   ; target = step + 400
 *   0x42ED98  JMP  0x42ED9F
 *   0x42ED9A  MOV  EAX, 0x190                   ; idle target = 400
 *   0x42ED9F  MOV  ECX, [ESI+0x310]             ; ECX = engine_speed_accum
 *   0x42EDA5  CMP  EAX, ECX
 *   0x42EDA7  JLE  0x42EDCA                     ; target <= rpm → down path
 * UP path (target > rpm):
 *   0x42EDA9  SUB  EAX, ECX                     ; EAX = target - rpm (>0)
 *   0x42EDAB  CDQ
 *   0x42EDAC  AND  EDX, 0xf                     ; round bias (no-op for >0)
 *   0x42EDAF  ADD  EAX, EDX
 *   0x42EDB1  SAR  EAX, 4                       ; step = delta >> 4
 *   0x42EDB4  CMP  EAX, 0x190
 *   0x42EDB9  JLE  0x42EDE1                     ; step <= 400 → store path
 *   0x42EDBB  MOV  EAX, 0x190                   ; clamp step to 400
 *   0x42EDC0  ADD  ECX, EAX                     ; rpm += step
 *   0x42EDC2  MOV  [ESI+0x310], ECX             ; store
 *   0x42EDC8  POP ESI; RET
 * DOWN path (target <= rpm):
 *   0x42EDCA  SUB  EAX, ECX                     ; EAX = target - rpm (<=0)
 *   0x42EDCC  CDQ
 *   0x42EDCD  AND  EDX, 0xf                     ; round bias = 0xf when <0
 *   0x42EDD0  ADD  EAX, EDX
 *   0x42EDD2  SAR  EAX, 4                       ; step = delta >> 4 (<=0)
 *   0x42EDD5  CMP  EAX, 0xc8
 *   0x42EDDA  JLE  0x42EDE1                     ; always-taken in practice
 *   0x42EDDC  MOV  EAX, 0xc8                    ; (dead) clamp to 200
 *   0x42EDE1  ADD  ECX, EAX                     ; rpm += step (negative)
 *   0x42EDE3  MOV  [ESI+0x310], ECX             ; store
 *   0x42EDE9  POP ESI; RET
 *
 * Notes preserved from prior port audit:
 *   - No post-store clamp (rpm is not bounded against redline or 400 after
 *     the slew — original 0x42EDC2/0x42EDE3 stores rpm unconditionally).
 *   - Down-path clamp to 200 at 0x42EDDC is unreachable: in the down branch
 *     EAX is the signed (target-rpm)>>4 which is <= 0, so the JLE EAX,0xc8
 *     at 0x42EDDA is always taken. We preserve the clamp anyway so the C
 *     mirrors the listing exactly.
 *
 * Field offsets verified vs td5_types.h:
 *   +0x1bc  tuning_data_ptr        +0x310  engine_speed_accum
 *   +0x33e  encounter_steering_cmd +0x36d  brake_flag
 *   tuning[+0x72] = redline (int16)
 */
static void update_engine_speed_smoothed(TD5_Actor *actor)
{
    /* MOVSX ECX, WORD [tuning+0x72]  [@ 0x42ED5B] */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);

    int32_t target;
    /* TEST AL,AL / JNZ + TEST DX,DX / JL  [@ 0x42ED65, 0x42ED70] */
    if (actor->brake_flag != 0 || (int16_t)actor->encounter_steering_cmd < 0) {
        /* MOV EAX, 0x190  [@ 0x42ED9A] */
        target = 0x190;
    } else {
        /* MOVSX EDX, DX  [@ 0x42ED75] */
        int32_t throttle = (int32_t)(int16_t)actor->encounter_steering_cmd;
        /* LEA EAX,[ECX-0x190]; IMUL EAX,EDX  [@ 0x42ED78,0x42ED7E] */
        int32_t v = (redline - 0x190) * throttle;
        /* CDQ; AND EDX,0xff; ADD EAX,EDX; SAR EAX,8  [@ 0x42ED81-0x42ED8A]
         * Signed round-toward-zero divide by 256. */
        v = (v + (((int32_t)((uint32_t)v >> 31) ? 0xff : 0))) >> 8;
        /* CMP ECX,EAX / JGE / MOV EAX,ECX  [@ 0x42ED8D-0x42ED91]
         * EAX = min(EAX, redline). */
        if (redline < v) v = redline;
        /* ADD EAX, 0x190  [@ 0x42ED93] */
        target = v + 0x190;
    }

    /* MOV ECX,[ESI+0x310]  [@ 0x42ED9F] */
    int32_t rpm = actor->engine_speed_accum;

    /* CMP EAX,ECX / JLE 0x42EDCA  [@ 0x42EDA5-0x42EDA7] */
    int32_t step;
    if (target > rpm) {
        /* UP path: SUB EAX,ECX  [@ 0x42EDA9] */
        int32_t delta = target - rpm;
        /* CDQ; AND EDX,0xf; ADD EAX,EDX; SAR EAX,4  [@ 0x42EDAB-0x42EDB1]
         * delta is strictly > 0 here, so the round bias is 0; plain >>4. */
        step = (delta + (((int32_t)((uint32_t)delta >> 31) ? 0xf : 0))) >> 4;
        /* CMP EAX,0x190 / JLE / MOV EAX,0x190  [@ 0x42EDB4-0x42EDBB] */
        if (step > 0x190) step = 0x190;
    } else {
        /* DOWN path: SUB EAX,ECX  [@ 0x42EDCA] — delta <= 0 */
        int32_t delta = target - rpm;
        /* Round-toward-zero divide by 16 for negative delta. */
        step = (delta + (((int32_t)((uint32_t)delta >> 31) ? 0xf : 0))) >> 4;
        /* CMP EAX,0xc8 / JLE / MOV EAX,0xc8  [@ 0x42EDD5-0x42EDDC]
         * Unreachable in practice — step <= 0 here — but kept for fidelity. */
        if (step > 0xc8) step = 0xc8;
    }

    /* ADD ECX,EAX; MOV [+0x310],ECX  [@ 0x42EDC0/EDC2 or 0x42EDE1/EDE3] */
    actor->engine_speed_accum = rpm + step;
}

/* --- UpdateEngineSpeedAccumulator (0x0042EDF0) ---
 *
 * Byte-faithful port of FUN_0042EDF0 (RE: TD5_pool7 read-only listing,
 * audited 2026-05-14). Mirrors the original x86 control flow line-for-line:
 *
 *   0x42EDF0  PUSH ESI                          ; entry
 *   0x42EDF1  MOV  ESI, [ESP+8]                 ; ESI = actor (param_1)
 *   0x42EDF5  XOR  EAX, EAX
 *   0x42EDF7  MOV  AL,  BYTE [ESI+0x36b]        ; AL  = current_gear
 *   0x42EDFD  PUSH EDI
 *   0x42EDFE  MOV  EDI, EAX                     ; EDI = gear (zero-extended)
 *   0x42EE00  CMP  EDI, 0x1
 *   0x42EE03  JNZ  0x42EE11                     ; gear != 1 → forward path
 *   0x42EE05  PUSH ESI
 *   0x42EE06  CALL 0x0042ed50                   ; UpdateVehicleEngineSpeedSmoothed
 *   0x42EE0B  ADD  ESP, 0x4
 *   0x42EE0E  POP  EDI; POP ESI; RET            ; gear==1: neutral helper + ret
 * Forward path (gear != 1):
 *   0x42EE11  MOV  EAX, [ESI+0x314]             ; EAX = longitudinal_speed (int32)
 *   0x42EE17  SAR  EAX, 0x8                     ; EAX = speed >> 8 (signed arith)
 *   0x42EE1A  CDQ                               ; EDX = sign(EAX)
 *   0x42EE1B  XOR  EAX, EDX
 *   0x42EE1D  SUB  EAX, EDX                     ; EAX = abs(speed >> 8)
 *   0x42EE1F  MOV  EDX, [ESP+0x10]              ; EDX = param_2 (tuning_data_ptr)
 *   0x42EE23  MOVSX EDX, WORD [EDX+EDI*2+0x2e]  ; EDX = gear_ratio (signed int16)
 *   0x42EE28  MOV  ECX, [ESI+0x310]             ; ECX = engine_speed_accum (rpm)
 *   0x42EE2E  IMUL EAX, EDX                     ; EAX *= gear_ratio
 *   0x42EE31  LEA  EAX, [EAX+EAX*4]             ; EAX *= 5
 *   0x42EE34  LEA  EAX, [EAX+EAX*8]             ; EAX *= 9  (cumulative *45 = 0x2D)
 *   0x42EE37  CDQ
 *   0x42EE38  AND  EDX, 0xfff                   ; round-toward-zero bias for SAR 12
 *   0x42EE3E  ADD  EAX, EDX
 *   0x42EE40  SAR  EAX, 0xc                     ; EAX = (abs*ratio*45) >> 12 (signed r->0)
 *   0x42EE43  ADD  EAX, 0x190                   ; target = step + 400
 *   0x42EE48  MOV  EDX, ECX                     ; EDX = rpm
 *   0x42EE4A  SUB  EDX, EAX                     ; EDX = delta = rpm - target
 *   0x42EE4C  CMP  EDX, 0x320
 *   0x42EE52  JLE  0x42EE5C                     ; delta <= 0x320 → not fast-down
 *   0x42EE54  SUB  ECX, 0xc8                    ; fast-down: rpm -= 200
 *   0x42EE5A  JMP  0x42EE79                     ; → clamp/store
 *   0x42EE5C  CMP  EDX, 0xfffffce0              ; (-800)
 *   0x42EE62  JGE  0x42EE6C                     ; delta >= -800 → smooth
 *   0x42EE64  ADD  ECX, 0xc8                    ; fast-up: rpm += 200
 *   0x42EE6A  JMP  0x42EE79                     ; → clamp/store
 * Smooth path (-800 <= delta <= 0x320):
 *   0x42EE6C  SUB  EAX, ECX                     ; EAX = target - rpm (= -delta)
 *   0x42EE6E  CDQ
 *   0x42EE6F  AND  EDX, 0x3                     ; round-toward-zero bias for SAR 2
 *   0x42EE72  ADD  EAX, EDX
 *   0x42EE74  SAR  EAX, 0x2                     ; step = (target-rpm) >> 2 (signed r->0)
 *   0x42EE77  ADD  ECX, EAX                     ; rpm += step
 * Clamp/store:
 *   0x42EE79  MOV  EAX, [ESI+0x1bc]             ; EAX = tuning_data_ptr (actor field)
 *   0x42EE7F  MOVSX EAX, WORD [EAX+0x72]        ; EAX = redline (int16)
 *   0x42EE83  CMP  ECX, EAX
 *   0x42EE85  JLE  0x42EE89
 *   0x42EE87  MOV  ECX, EAX                     ; rpm = min(rpm, redline)
 *   0x42EE89  POP  EDI
 *   0x42EE8A  MOV  [ESI+0x310], ECX             ; store rpm
 *   0x42EE90  POP  ESI; RET
 *
 * Notes:
 *   - Original takes a SECOND param (tuning_data_ptr) loaded from [ESP+0x10].
 *     Both callers (0x00404030 UpdatePlayerVehicleDynamics, 0x00404EC0
 *     UpdateAIVehicleDynamics) cache EBX = [actor+0x1bc] and push it as the
 *     second arg, so param_2 == actor->tuning_data_ptr in practice. The port
 *     uses get_phys(actor) (which reads +0x1bc) and reads the gear ratio at
 *     phys[+0x2e + gear*2] — equivalent in effect.
 *   - abs() is computed on the SHIFTED value (`abs(speed >> 8)`), NOT on the
 *     raw speed. For negative speed this rounds DOWN before abs, which
 *     differs from `abs(speed) >> 8` by 1 in the absolute-value bin for
 *     speeds that are not a multiple of 256. The prior port used
 *     `abs(speed) >> 8`; this version mirrors the listing.
 *   - Both SAR steps (>>12 and >>2 in the smooth path) include the explicit
 *     CDQ+AND+ADD round-toward-zero bias used by the original — required for
 *     bit-exact parity on negative intermediates (the >>12 step's intermediate
 *     EAX is unsigned in practice because abs() preceded it, so the bias is
 *     a no-op there, but kept for structural fidelity).
 *   - Fast-down threshold is `delta > 0x320` (i.e. >= 0x321), from
 *     `CMP EDX,0x320; JLE smooth`. Prior port used `> 0x321` (off-by-one) —
 *     fixed here to match the listing exactly.
 *   - Smooth-path bias `+ (sign? 3 : 0)` before `>> 2` was missing from the
 *     prior port; added so the divide-by-4 rounds toward zero (matters when
 *     target < rpm by an amount not divisible by 4).
 *   - Final clamp is UPPER-only against redline. No 400 floor (original does
 *     not clamp the lower bound here — see 0x42EE83 single CMP/JLE/MOV).
 *
 * Field offsets verified vs td5_types.h:
 *   +0x1bc  tuning_data_ptr            +0x314  longitudinal_speed
 *   +0x310  engine_speed_accum         +0x36b  current_gear (byte)
 *   tuning[+0x2e + gear*2] = gear_ratio (int16)
 *   tuning[+0x72]          = redline    (int16)
 */
void td5_physics_update_engine_speed(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* MOVZX EDI, BYTE [ESI+0x36b]  [@ 0x42EDF5-0x42EDFE] — gear is treated as
     * an unsigned byte index (XOR EAX,EAX / MOV AL,...). */
    int32_t gear = (int32_t)(uint32_t)(uint8_t)actor->current_gear;

    /* CMP EDI,1 / JNZ  [@ 0x42EE00-0x42EE03] — gear==1 → neutral helper. */
    if (gear == 1) {
        update_engine_speed_smoothed(actor);
        return;
    }

    /* MOV EAX,[ESI+0x314]  [@ 0x42EE11] */
    int32_t lspd = actor->longitudinal_speed;
    /* SAR EAX,8  [@ 0x42EE17] — arithmetic shift, rounds toward -inf for neg. */
    int32_t lspd_shr8 = (int32_t)(lspd >> 8); /* signed >> in two's complement */
    /* CDQ; XOR EAX,EDX; SUB EAX,EDX  [@ 0x42EE1A-0x42EE1D] — abs(EAX). */
    int32_t abs_shr8 = lspd_shr8 < 0 ? -lspd_shr8 : lspd_shr8;

    /* MOVSX EDX,WORD [param_2 + gear*2 + 0x2e]  [@ 0x42EE23] — gear ratio.
     * param_2 is the caller-cached tuning_data_ptr (== actor->tuning_data_ptr). */
    int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2e + gear * 2);

    /* MOV ECX,[ESI+0x310]  [@ 0x42EE28] */
    int32_t rpm = actor->engine_speed_accum;

    /* IMUL EAX,EDX; LEA*5; LEA*9  [@ 0x42EE2E-0x42EE34] — EAX = abs*ratio*45. */
    int32_t prod = abs_shr8 * gear_ratio * 0x2d;

    /* CDQ; AND EDX,0xfff; ADD EAX,EDX; SAR EAX,12  [@ 0x42EE37-0x42EE40]
     * — signed round-toward-zero divide by 4096. */
    int32_t step12 = (prod + (((int32_t)((uint32_t)prod >> 31)) ? 0xfff : 0)) >> 12;

    /* ADD EAX,0x190  [@ 0x42EE43] — target = step + 400. */
    int32_t target = step12 + 0x190;

    /* MOV EDX,ECX; SUB EDX,EAX  [@ 0x42EE48-0x42EE4A] — delta = rpm - target. */
    int32_t delta = rpm - target;

    /* CMP EDX,0x320 / JLE  [@ 0x42EE4C-0x42EE52] — fast-down on delta > 0x320. */
    if (delta > 0x320) {
        /* SUB ECX,0xc8  [@ 0x42EE54] */
        rpm = rpm - 0xc8;
        /* JMP clamp/store  [@ 0x42EE5A] */
    } else if (delta < -800) {
        /* CMP EDX,-800 / JGE smooth → fall-through fast-up
         * ADD ECX,0xc8  [@ 0x42EE64] */
        rpm = rpm + 0xc8;
        /* JMP clamp/store  [@ 0x42EE6A] */
    } else {
        /* Smooth path  [@ 0x42EE6C-0x42EE77] */
        /* SUB EAX,ECX  — EAX = target - rpm (= -delta). */
        int32_t toward = target - rpm;
        /* CDQ; AND EDX,3; ADD EAX,EDX; SAR EAX,2 — signed r->0 divide by 4. */
        int32_t s = (toward + (((int32_t)((uint32_t)toward >> 31)) ? 3 : 0)) >> 2;
        /* ADD ECX,EAX. */
        rpm = rpm + s;
    }

    /* Clamp/store  [@ 0x42EE79-0x42EE90] — upper clamp at redline, no floor. */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    if (rpm > redline) rpm = redline;

    actor->engine_speed_accum = rpm;
}

/* Round-to-zero signed divide by 256 — byte-exact port of the 0x0042EF10
 * idiom: CDQ ; AND EDX, 0xFF ; ADD EAX, EDX ; SAR EAX, 8.
 *
 *   For x >= 0: plain SAR by 8 (low byte truncated).
 *   For x <  0 with (x & 0xFF) != 0: (x + 0xFF) >> 8 — one unit closer to zero
 *     than plain SAR (truncated division by 256). */
static inline int32_t sar8_rz_42EF10(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* --- UpdateAutomaticGearSelection (0x0042EF10) ---
 *
 * Byte-exact port from listing 0x0042EF10..0x0042F008 (73 instructions).
 *
 * Automatic transmission FSM. Promotes reverse→first when throttle goes
 * positive, force-reverses on negative throttle, applies upshift/downshift
 * based on per-gear RPM thresholds, and on upshift fires a drivetrain
 * kick across the four wheel_spring_dv slots.
 *
 *   actor + 0x33E  encounter_steering_cmd   int16 (signed throttle)  → EDX
 *   actor + 0x36B  current_gear             uint8                     → AL/BL
 *   actor + 0x310  engine_speed_accum       int32                     → EDI
 *   actor + 0x314  longitudinal_speed       int32                     → EBX
 *   actor + 0x2EC..0x2F8  wheel_spring_dv[4] int32 (FL/FR/RL/RR)
 *   phys + 0x3E + gear*2  upshift threshold int16  (indexed by CACHED gear)
 *   phys + 0x4E + gear*2  downshift threshold int16 (indexed by CACHED gear)
 *   phys + 0x68           drive_torque_mult  int16
 *   DAT_00467394          g_gearTorqueTable  int32[9] = {0,0,0x100,0xC0,
 *                                            0x80,0x40,0x20,0x10,0}
 *
 * KEY semantics from the listing:
 *
 *   1. CACHED GEAR INDEXING.  At 0x0042EF1D the function `XOR EAX,EAX` then
 *      `MOV AL,[ECX+0x36B]` zero-extends current_gear into EAX. This cached
 *      EAX value is used for BOTH the upshift threshold read at 0x0042EF52
 *      (`MOVSX EBX,[ESI+EAX*2+0x3E]`) and the downshift threshold read at
 *      0x0042EFF1 (`MOVSX EDX,[ESI+EAX*2+0x4E]`), AND for the `CMP EAX,0x8`
 *      gate at 0x0042EF5F and `CMP EAX,0x2` gate at 0x0042EFFA. The cache
 *      is NEVER refreshed across the function body — even after the
 *      reverse→2 promotion writes memory at 0x0042EF47, EAX stays at 0.
 *
 *   2. DRIVETRAIN KICK.  On upshift the original spreads a per-gear torque
 *      pulse across wheel_spring_dv via the table at 0x00467394, indexed
 *      by the NEW post-upshift gear (BL after INC at 0x0042EF78).
 *      +0x2EC (FL) and +0x2F0 (FR) ADD k; +0x2F4 (RL) and +0x2F8 (RR)
 *      SUB k. The formula is:
 *          tmp1 = sar8_rz(phys[0x68] * throttle * 0x1A)
 *          tmp2 = tmp1 * g_gearTorqueTable[new_gear & 0xFF]
 *          k    = sar8_rz(tmp2)
 */
void td5_physics_auto_gear_select(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* Listing 0x0042EF14: MOVSX EDX, [ECX+0x33E]  (signed throttle). */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;

    /* Listing 0x0042EF1D-21: XOR EAX,EAX ; MOV AL,[ECX+0x36B].
     * EAX holds the zero-extended current_gear and is NEVER refreshed. */
    uint32_t gear_cached = (uint32_t)actor->current_gear;

    /* Listing 0x0042EF28: MOV EDI, [ECX+0x310]  (engine_speed_accum). */
    int32_t rpm = actor->engine_speed_accum;

    /* Listing 0x0042EF1F-3A: TEST EDX,EDX ; JGE 0x42EF3B.
     * Negative throttle → write gear=0 (reverse) and RET. */
    if (throttle < 0) {
        actor->current_gear = (uint8_t)0;
        return;
    }

    /* Listing 0x0042EF3B-4D: if (EAX == 0) check throttle, promote to 2.
     *   TEST EAX,EAX ; JNZ 0x42EF4E
     *   TEST EDX,EDX ; JLE 0x42F005   (cleanup-RET when throttle <= 0)
     *   MOV BYTE PTR [ECX+0x36B], 0x2  (gear=2 in memory only) */
    if (gear_cached == 0u) {
        if (throttle <= 0) return;
        actor->current_gear = (uint8_t)2;
        /* gear_cached intentionally NOT refreshed — original keeps EAX=0. */
    }

    /* Listing 0x0042EF4E: MOV ESI, [ESP+0x14]  (param_2 == phys table). */

    /* Listing 0x0042EF52: MOVSX EBX, [ESI + EAX*2 + 0x3E]  upshift threshold. */
    int32_t up_thresh = (int32_t)PHYS_S(actor, 0x3E + (int32_t)gear_cached * 2);

    /* Listing 0x0042EF57-70:
     *   CMP EDI,EBX     ; JLE 0x42EFF1  (rpm <= up_thresh → downshift branch)
     *   CMP EAX,0x8     ; JGE 0x42EFF1  (gear_cached >= 8 signed → downshift)
     *   MOV EBX,[ECX+0x314] ; TEST EBX,EBX ; JLE 0x42EFF1  (long_spd<=0 → ds) */
    if (rpm > up_thresh
        && (int32_t)gear_cached < 8
        && actor->longitudinal_speed > 0) {

        /* Listing 0x0042EF72-7A: MOV BL,[ECX+0x36B] ; INC BL ; MOV
         * [ECX+0x36B],BL.  BL is re-loaded fresh (will be 2 after promo). */
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;

        /* Listing 0x0042EF80-8D:
         *   MOVSX EAX,[ESI+0x68]         ; phys[0x68] (drive_torque_mult)
         *   IMUL EAX,EDX                 ; * throttle
         *   LEA EDX,[EAX+EAX*2]          ; EDX = EAX * 3
         *   LEA EAX,[EAX+EDX*4]          ; EAX = EAX + 12*EAX = 13*EAX
         *   SHL EAX,1                    ; EAX = 26*EAX = EAX*0x1A
         *
         * Listing 0x0042EF8F-A4: CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX,8
         *   → first sar8_rz (truncated divide by 256). */
        int32_t k = (int32_t)PHYS_S(actor, 0x68) * throttle * 0x1A;
        k = sar8_rz_42EF10(k);

        /* Listing 0x0042EFA7-AD: AND EBX,0xFF ; IMUL EAX,[EBX*4 + 0x467394]
         * EBX still has new_gear in BL from the store at 0x0042EF7A; the
         * AND masks the high bytes (leftover from the 0x314 long_spd load).
         * Table at DAT_00467394 = {0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20,
         * 0x10, 0} (9 dwords). new_gear is bounded by the < 8 gate to
         * [1..8], so indices 1..8 of the table are all that's reachable. */
        static const int32_t g_gear_torque_table[9] = {
            0, 0, 0x100, 0xC0, 0x80, 0x40, 0x20, 0x10, 0
        };
        k = k * g_gear_torque_table[new_gear & 0xFFu];

        /* Listing 0x0042EFB5-CA: MOV EBX,[ECX+0x2EC] ; CDQ ; AND EDX,0xFF ;
         * ADD EAX,EDX ; MOV EDX,[ECX+0x2F8] ; SAR EAX,8
         *   → second sar8_rz. */
        k = sar8_rz_42EF10(k);

        /* Listing 0x0042EFCD-E9: spread k across the four spring-dv slots.
         *   ADD EDI,EAX ; SUB ESI,EAX ; ADD EBX,EAX ; SUB EDX,EAX
         * with EDI=+0x2F0, ESI=+0x2F4, EBX=+0x2EC, EDX=+0x2F8 ─ so:
         *   +0x2EC (FL) += k ; +0x2F0 (FR) += k
         *   +0x2F4 (RL) -= k ; +0x2F8 (RR) -= k */
        actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
        actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
        actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
        actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
        return;
    }

    /* Listing 0x0042EFF1: MOVSX EDX,[ESI + EAX*2 + 0x4E]  dn_thresh
     *                        (still indexed by CACHED gear, not refreshed). */
    int32_t dn_thresh = (int32_t)PHYS_S(actor, 0x4E + (int32_t)gear_cached * 2);

    /* Listing 0x0042EFF6-FF:
     *   CMP EDI,EDX  ; JGE 0x42F005   (rpm >= dn_thresh → skip)
     *   CMP EAX,0x2  ; JLE 0x42F005   (gear_cached <= 2 → skip)
     *   DEC [ECX+0x36B] */
    if (rpm < dn_thresh && (int32_t)gear_cached > 2) {
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

/* pool13 / 0x0042F030 pilot trace hooks — extern declarations to keep the
 * function signature unchanged. Implementation in td5_pilot_trace_0042F030.c. */
extern void td5_pilot_emit_0042F030_enter(const TD5_Actor *actor, uintptr_t caller_ra);
extern void td5_pilot_emit_0042F030_leave(const TD5_Actor *actor, int32_t return_value,
                                          int32_t lut_index_used, int32_t lut_frac_used);

/* Round-to-zero signed divide by 256 — byte-exact port of the 0x0042F030
 * idiom: CDQ ; AND EDX, 0xFF ; ADD EAX, EDX ; SAR EAX, 8.
 *
 *   For x >= 0: plain SAR by 8 (low byte truncated).
 *   For x <  0 with (x & 0xFF) != 0: (x + 0xFF) >> 8 — one unit closer to zero
 *     than plain SAR (truncated division by 256).
 *
 * Equivalent to C99 truncated division: (int32_t)(x / 256). */
static inline int32_t sar8_rz_42F030(int32_t x) {
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}

/* Round-to-zero signed divide by 512 — original idiom:
 *   CDQ ; AND EDX, 0x1FF ; ADD EAX, EDX ; SAR EAX, 9.
 * Equivalent to C99 truncated division: (int32_t)(x / 512). */
static inline int32_t sar9_rz_42F030(int32_t x) {
    return ((x < 0) ? (x + 0x1FF) : x) >> 9;
}

/* --- ComputeDriveTorqueFromGearCurve (0x42F030) ---
 *
 * Byte-exact port from listing 0x0042F030..0x0042F0FC.
 *
 * Piecewise-linear torque curve interpolation.
 *
 *   actor + 0x310  engine_speed_accum  int32
 *   actor + 0x33E  encounter_steering  int16 (signed throttle)
 *   actor + 0x36B  current_gear        uint8 — 0x01 == neutral, early-out
 *   tuning + 0x00..0x1F  LUT[N] int16 (per-512-rpm torque samples)
 *   tuning + 0x2E + gear*2  gear_ratio[gear] int16
 *   tuning + 0x68  torque_mult int16
 *   tuning + 0x72  redline     int16; cutoff when engine_speed > redline-50
 *
 * Audit: re/analysis/pilot_0042F030_audit.md
 */
int32_t td5_physics_compute_drive_torque(TD5_Actor *actor)
{
    /* Entry trace hook (pure-leaf function; no state to snapshot at exit). */
    td5_pilot_emit_0042F030_enter(actor, (uintptr_t)__builtin_return_address(0));

    int16_t *phys = get_phys(actor);
    if (!phys) {
        td5_pilot_emit_0042F030_leave(actor, 0, 0, 0);
        return 0;
    }

    uint8_t  gear_u8 = actor->current_gear;
    int32_t  gear    = (int32_t)gear_u8;

    /* Neutral (gear == 1) — original CMP BL,0x1 / JZ RET_ZERO at 0x0042F03B-45. */
    if (gear_u8 == 0x01) {
        td5_pilot_emit_0042F030_leave(actor, 0, 0, 0);
        return 0;
    }

    int32_t rpm = actor->engine_speed_accum;

    /* index = sar9_rz(rpm) — listing 0x0042F04B-58 (CDQ/AND 0x1FF/ADD/SAR 9).
     * NO bounds clamp (original reads LUT[index] and LUT[index+1] freely). */
    int32_t index = sar9_rz_42F030(rpm);

    int32_t torque_mult = (int32_t)PHYS_S(actor, 0x68);

    /* t0 = sar8_rz(LUT[index] * mult) — listing 0x0042F060-72.
     * t1 = sar8_rz(LUT[index+1] * mult) — listing 0x0042F077-8F. */
    int32_t lut_i  = (int32_t)PHYS_S(actor, index * 2);
    int32_t lut_i1 = (int32_t)PHYS_S(actor, index * 2 + 2);
    int32_t t0 = sar8_rz_42F030(lut_i  * torque_mult);
    int32_t t1 = sar8_rz_42F030(lut_i1 * torque_mult);

    /* Signed low-9-bit fraction (truncated-divide remainder mod 512).
     * Listing 0x0042F096-A5: AND ECX,0x800001FF; if neg: DEC; OR 0xFFFFFE00; INC.
     * Algebraically equivalent to C99 truncated remainder rpm % 512.
     * For rpm >= 0: frac in [0, 511].
     * For rpm <  0: frac in [-511, 0]. */
    int32_t frac = rpm % 512;

    /* lerp = sar9_rz((t1 - t0) * frac) + t0 — listing 0x0042F0A6-C0. */
    int32_t torque = sar9_rz_42F030((t1 - t0) * frac) + t0;

    /* * throttle — original at 0x0042F0C2-D0 uses sar8_rz. Throttle is signed
     * (encounter_steering_cmd, sign-flipped by 0x42F010 for reverse gear). */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;
    torque = sar8_rz_42F030(torque * throttle);

    /* * gear_ratio[gear] — original at 0x0042F0D2-EF uses sar8_rz on the
     * EBX-byte-masked gear index. PHYS_S already does 16-bit signed load. */
    int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E + (int32_t)(uint8_t)gear_u8 * 2);
    torque = sar8_rz_42F030(torque * gear_ratio);

    /* Redline cutoff: original computes the full pipeline then CMPs EBP (raw
     * engine_speed) against ECX (redline-50). JLE keep result; else XOR=0.
     * The compare is signed (JLE), so rpm > redline-50 → return 0. */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);
    if (rpm > redline - 50) {
        td5_pilot_emit_0042F030_leave(actor, 0, index, frac);
        return 0;
    }

    td5_pilot_emit_0042F030_leave(actor, torque, index, frac);
    return torque;
}

/* --- ApplySteeringTorqueToWheels (0x42EEA0) ---  [byte-faithful port]
 *
 * Verbatim port of FUN_0042EEA0 (0x0042EEA0..0x0042EF06). Originally the
 * port stubbed this out to suppress a pitch-divergence symptom, but the
 * batch precise-port mandate is byte-faithful behaviour first; downstream
 * suspension fixes belong in their owning functions. Disassembly:
 *
 *   MOVSX EDX,[param2+0x68]            ; cardef/tuning short (drive-torque mult)
 *   MOVSX EAX,[param1+0x33E]           ; actor encounter_steering_cmd (s16)
 *   IMUL  EAX,EDX                      ; throttle * mult
 *   LEA   EDX,[EAX+EAX*2]              ; *3
 *   LEA   EAX,[EAX+EDX*4]              ; *(1+12) = *13
 *   SHL   EAX,1                        ; *26 (= 0x1A)
 *   CDQ / AND EDX,0xff / ADD EAX,EDX / SAR EAX,0x8   ; biased >>8
 *   XOR   EDX,EDX / MOV DL,[param1+0x36B]            ; zero-ext current_gear
 *   IMUL  EAX,[EDX*4 + 0x467394]       ; g_gearTorqueTable[gear]
 *   CDQ / AND EDX,0xff / ADD EAX,EDX / SAR EAX,0x8   ; biased >>8
 *   ADD   [param1+0x2EC],EAX           ; wheel_spring_dv[0] += k (FL)
 *   ADD   [param1+0x2F0],EAX           ; wheel_spring_dv[1] += k (FR)
 *   SUB   [param1+0x2F4],EAX           ; wheel_spring_dv[2] -= k (RL)
 *   SUB   [param1+0x2F8],EAX           ; wheel_spring_dv[3] -= k (RR)
 *
 * The (x + ((x>>31)&0xff)) >> 8 idiom is the biased-toward-zero arithmetic
 * right-shift Microsoft's compiler emits for signed /256. g_gearTorqueTable
 * at DAT_00467394 mirrors the same LUT used inline by auto_gear_select.
 */
void td5_physics_apply_steering_torque(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* DAT_00467394 — g_gearTorqueTable (int32[]). Indexed by current_gear
     * read as an unsigned byte; only entries 0..8 are meaningful (0,0,
     * 256,192,128,64,32,16,0).
     *
     * BOUNDS AUDIT (2026-05-14, fix-gear-bounds):
     *   Every writer to actor+0x36B in TD5_d3d.exe is bounded to [0..8]:
     *     0x40368e  player INC, gated by  gear < gear_count - 1
     *     0x4036xx  player DEC, gated by  gear != 0
     *     0x405dbf  ResetVehicleActorState   = 0x02 (constant)
     *     0x42ef32  UpdateAutomaticGearSelection = 0x00 (constant)
     *     0x42ef47  UpdateAutomaticGearSelection = 0x02 (constant)
     *     0x42ef7a  UpdateAutomaticGearSelection INC, gated by gear < 8
     *     0x42efff  UpdateAutomaticGearSelection DEC, gated by gear > 2
     *   Port-side writers in td5_physics.c match those bounds line-for-line
     *   (see td5_physics_auto_gear_select / _no_kick — INC under `< 8` gate,
     *   DEC under `> 2` gate, plus four constant-only writes of 0 / 2).
     *
     *   Reachable indices in normal play: 2..7 (active forward gears) + 0
     *   (reverse) + 8 (transient one-tick upshift result of the < 8 gate
     *   when current_gear == 7).  Indices 1 and 9..255 are unreachable.
     *
     * The 256-entry zero-filled expansion below is DEFENSIVE belt-and-
     * suspenders against any future writer that violates the [0..8] range.
     * It is NOT required for byte-faithful behavior — the original's nine-
     * entry table is reached at indices 0..8 only, and the port respects
     * that. The expansion costs 988 bytes of .rodata to make an OOB read
     * silent (returns 0 → no kick) rather than crashing on a malformed
     * save or modded actor stream. */
    static const int32_t g_gear_torque_table[256] = {
        [2] = 0x100,
        [3] = 0xC0,
        [4] = 0x80,
        [5] = 0x40,
        [6] = 0x20,
        [7] = 0x10,
        /* indices 0,1,8..255 = 0 (default) */
    };

    int32_t throttle    = (int32_t)actor->encounter_steering_cmd;  /* +0x33E s16 */
    int32_t torque_mult = (int32_t)PHYS_S(actor, 0x68);            /* tuning +0x68 s16 */

    int32_t k = throttle * torque_mult * 0x1A;
    k = (k + ((k >> 31) & 0xff)) >> 8;
    k = k * g_gear_torque_table[(uint8_t)actor->current_gear];     /* +0x36B u8 */
    k = (k + ((k >> 31) & 0xff)) >> 8;

    actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
    actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
    actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
    actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
}

/* --- ApplyReverseGearThrottleSign (0x42F010) ---
 *
 * Byte-exact port from listing 0x0042F010..0x0042F02F (8 instructions).
 *
 *   0042f010  MOV  EAX,dword ptr [ESP + 0x4]      ; actor
 *   0042f014  MOV  CL,byte ptr [EAX + 0x36b]      ; gear_u8 = actor->current_gear
 *   0042f01a  TEST CL,CL
 *   0042f01c  JNZ  0x0042f02f                     ; if gear != 0, skip
 *   0042f01e  MOV  CX,word ptr [EAX + 0x33e]      ; thr = actor->encounter_steering_cmd
 *   0042f025  NEG  CX                             ; thr = -thr (16-bit two's complement)
 *   0042f028  MOV  word ptr [EAX + 0x33e],CX
 *   0042f02f  RET
 *
 * Flips the signed throttle term in-place when current_gear == 0 (REVERSE) so
 * the same forward drive-torque pipeline (0x0042F030) can be reused for
 * backward motion. Field +0x36B is the 1-byte current_gear (REVERSE=0,
 * NEUTRAL=1, FIRST=2, ...). Field +0x33E is the int16_t encounter_steering_cmd
 * (also reachable as the signed-throttle source consumed by
 * ComputeDriveTorqueFromGearCurve). 16-bit NEG matches C int16_t negation. */
void td5_physics_reverse_throttle_sign(TD5_Actor *actor)
{
    /* TEST CL,CL / JNZ — early-out when gear != REVERSE (0). */
    if (actor->current_gear != 0)
        return;

    /* NEG CX on a 16-bit word in memory — int16_t two's-complement negation. */
    actor->encounter_steering_cmd = (int16_t)-(int32_t)actor->encounter_steering_cmd;
}

/* --- ComputeReverseGearTorque (0x00403C80) — byte-faithful port ---
 *
 * Despite the Ghidra name, this function does NOT compute torque. It
 * (a) produces the RPM-encoded pseudo-speed written back as the caller's
 * longitudinal_speed or lateral_speed, and (b) slews engine_speed_accum
 * toward a target that depends on throttle, gear, brake, and a caller-
 * supplied signed speed term. It is the GROUND-PATH authoritative engine
 * updater — UpdateEngineSpeedAccumulator (UESA) runs on the airborne path
 * instead; the two are mutually exclusive per tick.
 *
 * Byte-exact port from listing 0x00403C80..0x00403D82 (98 instructions).
 *
 * Original signature (Ghidra): __cdecl(phys *param_1, actor *param_2, int speed_in)
 *   param_1 (EAX,[ESP+0xC])  phys/tuning ptr   — reads [+0x2E + gear*2], [+0x72]
 *   param_2 (EDI,[ESP+0x18]) actor ptr         — reads [+0x310],[+0x33E],[+0x36B],[+0x36D]
 *                                                writes [+0x310]
 *   param_3      ([ESP+0x20]) signed speed_in
 *
 * Port keeps the existing (actor, speed_in) wrapper signature; `get_phys`
 * recovers param_1.  All math/control-flow inside is line-for-line with
 * the original.
 *
 * Original logic:
 *   gear == 1  (neutral) → iVar5 (ret_value) = 0, BUT continues to slew
 *                          engine through the cold branch (target=0).
 *   gear != 1            → iVar5 = ((((rpm-400)*0x1000) /45 trunc) /gear_ratio
 *                                  trunc) << 8
 *
 *   throttle  > 0 AND gear == 2 → hot branch:
 *      u = sar8_rz(speed_in * 4); if (u<0) u = 0  (SETS/DEC/AND clamp)
 *      target = u + redline - 0x708 ;  step = 200
 *   else                          → cold branch:
 *      target = 0; step = (brake ? 400 : 200) * 2  (i.e. 800 or 400)
 *
 *   Slew engine_speed_accum toward target:
 *     if (target < engine):                              # descending (JLE label)
 *         engine -= step
 *         if (engine < target):  write target (clamp)
 *         else:                  write engine (stepped)
 *     else:                                              # ascending
 *         if (engine < target - 4*step):  write engine + step  (big-gap ramp)
 *         else:  engine += sar2_rz(target - engine); write engine  (exp pull)
 *
 *   Return iVar5 (the encoded pseudo-speed).
 *
 * sar8_rz / sar2_rz : C99 truncated signed division by 256 / 4. */
static inline int32_t sar8_rz_403C80(int32_t x) {
    /* CDQ ; AND EDX,0xFF ; ADD EAX,EDX ; SAR EAX,8.
     * Equivalent to (int32_t)(x / 256) — truncate toward zero. */
    return ((x < 0) ? (x + 0xFF) : x) >> 8;
}
static inline int32_t sar2_rz_403C80(int32_t x) {
    /* CDQ ; AND EDX,3 ; ADD EAX,EDX ; SAR EAX,2.
     * Equivalent to (int32_t)(x / 4). */
    return ((x < 0) ? (x + 3) : x) >> 2;
}

static int32_t compute_reverse_gear_torque(TD5_Actor *actor, int32_t speed_in)
{
    if (!actor) return 0;
    int16_t *phys = get_phys(actor);
    if (!phys) return 0;
    (void)phys;  /* PHYS_S(actor, off) wraps the same dereference. */

    /* Stack-mirroring the listing for clarity:
     *   ECX = engine          (rpm in/out)
     *   EBX = gear            (BL, byte-extended)
     *   EDX = redline (saved at [ESP+0x18])
     *   EBP = step (200 default, kept at [ESP+0x10])
     *   ESI = iVar5  (return value)
     *   EAX = target  (after target-build joins)  */
    int32_t engine  = actor->engine_speed_accum;
    int32_t gear    = (int32_t)actor->current_gear;          /* zero-extended byte */
    int32_t redline = (int32_t)PHYS_S(actor, 0x72);          /* MOVSX from [+0x72] */
    int32_t step    = 200;                                    /* MOV EBP,0xC8 */

    /* --- Encode pseudo-speed (iVar5 / ESI) --- */
    int32_t iVar5;
    if (gear == 1) {
        /* 0x00403CAF JNZ skip → XOR ESI,ESI ; JMP 0x00403CE4.
         * Neutral SKIPS the encode but does NOT early-return; engine slew
         * still runs through the cold branch below. */
        iVar5 = 0;
    } else {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x2E + gear * 2);

        /* LEA ESI,[ECX-400] ; SHL ESI,12  →  ESI = (engine - 400) * 0x1000.
         * Then 0xB60B60B7 IMUL idiom = ESI / 45 with truncate-toward-zero
         * via SAR 5 + sign-bit fixup (SHR 31 / ADD).
         * Then CDQ / IDIV gear_ratio truncates toward zero. */
        int32_t num = (engine - 400) * 0x1000;
        int32_t div45 = num / 45;                            /* C99 trunc */
        int32_t div_gr = div45 / gear_ratio;                 /* C99 trunc */
        iVar5 = div_gr << 8;
    }

    /* --- Target + step selection (0x00403CE4..0x00403D2F) --- */
    int32_t throttle = (int32_t)actor->encounter_steering_cmd;  /* MOVSX [+0x33E] */
    int32_t target;

    if (throttle > 0 && gear == 2) {
        /* Hot branch 0x00403CF3..0x00403D1C:
         *   EAX = speed_in*4 ; sar8_rz ; clamp <0 → 0 ; + redline - 0x708. */
        int32_t u = sar8_rz_403C80(speed_in * 4);
        /* SETS DL ; DEC EDX ; AND EAX,EDX → clamp u<0 to 0. */
        if (u < 0) u = 0;
        target = u + redline - 0x708;
        /* step stays at 200 (EBP unmodified on this path). */
    } else {
        /* Cold branch 0x00403D1E..0x00403D2F:
         *   if (brake_flag != 0) EBP = 0x190 (400)
         *   ADD EBP,EBP            → EBP *= 2  → 400 or 800
         *   XOR EAX,EAX            → target = 0 */
        if (actor->brake_flag != 0) {
            step = 0x190;        /* 400 */
        }
        step = step + step;      /* 400 or 800 */
        target = 0;
    }

    /* --- Slew engine_speed_accum (0x00403D31..0x00403D75) --- */
    /* CMP ECX,EAX ; JLE ascending. Note: JLE in original is SIGNED-LE.
     * The original CMP is engine (ECX) vs target (EAX). JLE means
     * engine <= target → take ascending branch. We invert: descending
     * iff engine > target. */
    if (engine > target) {
        /* Descending 0x00403D35..0x00403D48:
         *   SUB ECX,EBP        (engine -= step)
         *   CMP ECX,EAX ; JGE write_dec
         *   else: [+0x310] = EAX (clamp to target)   ; return ESI
         *   write_dec: [+0x310] = ECX                ; return ESI */
        engine = engine - step;
        if (engine >= target) {
            actor->engine_speed_accum = engine;       /* write_dec / 0x00403D75 */
        } else {
            actor->engine_speed_accum = target;       /* clamp / 0x00403D3B */
        }
    } else {
        /* Ascending 0x00403D49..0x00403D75:
         *   EDX = step*4 ; EBX = target - step*4
         *   CMP ECX,EBX ; JGE exp_pull
         *   else: ECX += EBP ; [+0x310] = ECX        ; return ESI  (big-gap ramp)
         *   exp_pull: EAX = target - engine ; sar2_rz ; ECX += EAX
         *            [+0x310] = ECX                  ; return ESI */
        int32_t threshold = target - step * 4;
        if (engine < threshold) {
            actor->engine_speed_accum = engine + step;  /* big-gap / 0x00403D5A */
        } else {
            int32_t delta = target - engine;
            int32_t inc   = sar2_rz_403C80(delta);
            actor->engine_speed_accum = engine + inc;   /* exp pull / 0x00403D75 */
        }
    }

    return iVar5;
}

/* ========================================================================
 * Surface Normal & Gravity -- ComputeVehicleSurfaceNormalAndGravity (0x42EBF0)
 *
 * Computes effective gravity vector projected onto body axes.
 * ======================================================================== */

/* Byte-faithful port of FUN_0042CCD0 "StoreRoundedVector3Ints" — actually
 * NormalizeVec3iToConstantMagnitude4096. The original FPU sequence is:
 *   - FILD each int component → 80-bit (truncated to PC=53 by phase-1 FPU CW)
 *   - sum = x*x + y*y + z*z   (double, FMUL/FADDP on stack)
 *   - scale = 4096.0 / sqrt(sum)  (FSQRT + FDIVR on the float constant)
 *   - foreach component: __ftol(scale * component) writes int (RC=11
 *     truncate-toward-zero via FLDCW then FISTP qword)
 *
 * If sum == 0, the original divides by zero → returns the indefinite integer
 * sentinel. We guard against that explicitly; in practice the four body probes
 * are always distinct, but on tick 0 they can all be zero before init.
 * [CONFIRMED @ 0x0042CCD0 listing 2026-05-14] */
static void td5_normalize_vec3i_to_4096(int32_t v[3]) {
    double x = (double)v[0];
    double y = (double)v[1];
    double z = (double)v[2];
    double sum = x * x + y * y + z * z;
    if (sum <= 0.0) {
        v[0] = 0; v[1] = 0; v[2] = 0;
        return;
    }
    double scale = 4096.0 / sqrt(sum);
    /* C99 cast-to-int truncates toward zero — matches __ftol with RC=11. */
    v[0] = (int32_t)(scale * x);
    v[1] = (int32_t)(scale * y);
    v[2] = (int32_t)(scale * z);
}

void td5_physics_compute_surface_gravity(TD5_Actor *actor)
{
    /* Original @ 0x42EBF0: uses 4 wheel probe world positions to compute
     * two diagonal-difference vectors of the body probes, normalizes each
     * to length 4096, cross-products them into a tilt vector, and projects
     * gravity onto X and Z body axes.
     *
     * Listing reference: 0x0042EBF0..0x0042ED47 (audited 2026-05-14 TD5_pool7).
     *
     * [CONFIRMED @ 0x42EBFA-0x42ECB7: actor offsets +0x090 FL, +0x09C FR,
     *  +0x0A8 RL, +0x0B4 RR; gravity int @ 0x00467380; mag-4096 @ 0x0046736C] */
    TD5_Vec3_Fixed *fl = &actor->probe_FL;
    TD5_Vec3_Fixed *fr = &actor->probe_FR;
    TD5_Vec3_Fixed *rl = &actor->probe_RL;
    TD5_Vec3_Fixed *rr = &actor->probe_RR;

    /* Phase 1 — Diagonal-difference vectors of the 4 body probes.
     * v1 = FL - FR - RR + RL   (per axis, then >> 8)  [SAR @ 0x42ec27 et al]
     * v2 = FL - RR - RL + FR   (per axis, then >> 8)  [SAR @ 0x42ec86 et al]
     * Plain arithmetic SAR — no round-toward-zero idiom here. */
    int32_t v1[3];
    v1[0] = (fl->x - fr->x - rr->x + rl->x) >> 8;
    v1[1] = (fl->y - fr->y - rr->y + rl->y) >> 8;
    v1[2] = (fl->z - fr->z - rr->z + rl->z) >> 8;

    int32_t v2[3];
    v2[0] = (fl->x - rr->x - rl->x + fr->x) >> 8;
    v2[1] = (fl->y - rr->y - rl->y + fr->y) >> 8;
    v2[2] = (fl->z - rr->z - rl->z + fr->z) >> 8;

    /* Pilot trace — pre-normalize snapshot (so the diff can localize Phase 1
     * vs Phase 2 divergence if it ever recurs). Compile-out for release builds. */
    td5_pilot_emit_0042EBF0_inputs(actor, v1, v2);

    /* Phase 2 — Normalize both diagonals to length 4096.
     * [CONFIRMED @ 0x42ecbf + 0x42ecd0 (CALL 0x42ccd0)] */
    td5_normalize_vec3i_to_4096(v1);
    td5_normalize_vec3i_to_4096(v2);

    /* Phase 3 — CrossProduct3i_FixedPoint12(v1, v2):
     *   cross[0] = (v2.z * v1.y - v1.z * v2.y) >> 12   (= v1.y*v2.z - v1.z*v2.y)
     *   cross[1] = (v1.z * v2.x - v1.x * v2.z) >> 12   (UNUSED — no Y projection)
     *   cross[2] = (v1.x * v2.y - v2.x * v1.y) >> 12
     * [CONFIRMED @ 0x0042EAC0 listing 2026-05-14] */
    int32_t cross_x = (v1[1] * v2[2] - v1[2] * v2[1]) >> 12;
    int32_t cross_z = (v1[0] * v2[1] - v2[0] * v1[1]) >> 12;

    /* Phase 4 — Project gravity onto X and Z body axes.
     * IMUL g, cross_n  → /2 round-toward-zero (matches C99 signed `/2`).
     * Then /4096 with round-toward-zero via `(ax + ((ax>>31)&0xfff)) >> 12`,
     * matching the original CDQ+AND 0xfff+ADD+SAR sequence.
     * [CONFIRMED @ 0x42eceb-0x42ed3b listing 2026-05-14] */
    int32_t ax = (g_gravity_constant * cross_x) / 2;
    int32_t az = (g_gravity_constant * cross_z) / 2;
    actor->linear_velocity_x += (ax + ((ax >> 31) & 0xfff)) >> 12;
    actor->linear_velocity_z += (az + ((az >> 31) & 0xfff)) >> 12;

    /* Pilot trace — post-update snapshot. */
    td5_pilot_emit_0042EBF0_outputs(actor, v1, v2, cross_x, cross_z);
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

        /* AI racer slots default to AUTOMATIC gearbox (actor+0x378 = 1).
         * The original sets 0x378 from input bits in UpdatePlayerVehicleControlState
         * (single write site @ 0x00402e97), which never runs for AI cars; their
         * 0x378 stays at its post-allocation value. AI cars never hit microbump
         * gear-shift opportunities here in the port because the brake→REVERSE
         * workaround at td5_physics_compute_drive_forces fires on every stuck
         * stop, and the manual-gearbox dispatch only flips throttle sign without
         * upshifting back from REVERSE — locking AI cars in nonstop reverse
         * after any recovery brake. Setting 0x378=1 here routes them through
         * td5_physics_auto_gear_select_no_kick which handles REVERSE→FIRST when
         * positive throttle resumes. For slot 0 with PlayerIsAI=0, the human
         * input path overwrites 0x378 each tick, so this init is a no-op. */
        if (slot < TD5_MAX_RACER_SLOTS) {
            *((uint8_t *)actor + 0x378) = 1;
        }

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

        /* --- Initial heading: actor+0x1F4 (euler_accum.yaw) = 0xE6A00 ---
         * Faithful port of MOV dword ptr [ESI + 0xfffffe7c], 0xe6a00 @ 0x0042F198.
         * This is the spawn sentinel yaw that InitializeActorTrackPose
         * (called from IntegrateVehiclePoseAndContacts) overwrites with the
         * per-track route heading before the first sim_tick. */
        actor->euler_accum.yaw = 0xE6A00;

        /* --- Cached suspension travel: actor+0x324 = sext_i32(phys+0x78) ---
         * Faithful port of MOVSX EDX, word ptr [EBX + 0x78]; MOV [ESI - 0x54], EDX
         * @ 0x0042F1AC. EBX = local_c = gVehiclePhysicsTable (port's tuning_data_ptr).
         * Field 0x78 of the physics table is the suspension/brake multiplier.
         *
         * Read from s_loaded_tuning (the RAW carparam.dat content) rather
         * than actor->tuning_data_ptr (which has already had `<<1` Normal
         * difficulty scaling applied at line 8284). Original's cache at
         * 0x42F1AC sees the unscaled value (=96 on Viper). Verified via
         * whole-state diff 2026-05-15: port pre-fix cached 192 = 96<<1. */
        /* Slot 0 with carparam.dat loaded: read RAW unscaled value (96 on
         * Viper). Slots 1..5 take the AI-template path in bind_default which
         * doesn't apply the `<<1` Normal-difficulty scaling to their tuning
         * buffer, so reading the (already-AI-template) tuning_data_ptr is
         * correct for them. Slots 6-11 (traffic) fall back to tuning_data_ptr.
         *
         * Whole-state diff 2026-05-15: this restriction restores slots 3/5
         * back to their pre-fix values (which matched original's AI-template
         * derived cache). */
        int is_slot0_carparam = (slot == 0 && s_carparam_loaded[0]);
        if (is_slot0_carparam) {
            int16_t *raw_tuning = (int16_t *)s_loaded_tuning[0];
            actor->cached_car_suspension_travel = (int32_t)raw_tuning[0x78 / 2];
        } else if (actor->tuning_data_ptr) {
            int16_t *phys_local = (int16_t *)actor->tuning_data_ptr;
            actor->cached_car_suspension_travel = (int32_t)phys_local[0x78 / 2];
        }

        /* --- Tire-track emitter IDs: actor+0x371..0x374 = 0xFF ---
         * Faithful port of the inner copy-loop write at 0x0042F1C8
         * (MOV byte ptr [EDX], 0xff with EDX=ESI-0x7 then INC EDX each iter).
         * Initial 0xFF marks emitter slot as unused. */
        actor->tire_track_emitter_FL = 0xFF;
        actor->tire_track_emitter_FR = 0xFF;
        actor->tire_track_emitter_RL = 0xFF;
        actor->tire_track_emitter_RR = 0xFF;

        /* --- Dynamic gear-count scan: actor+0x36C = scan_count + 1 ---
         * Faithful port of 0x0042F20C-0x0042F226:
         *   MOV ECX, 0x2
         *   ADD EAX, 0x42                  ; EAX = phys+0x42
         * loop:
         *   CMP word ptr [EAX], 0x270F     ; signed >= 9999?
         *   JGE done
         *   INC ECX                        ; scan_count++
         *   ADD EAX, 0x2                   ; next entry
         *   CMP ECX, 0x8
         *   JL loop
         * done:
         *   INC CL                         ; CL = scan_count + 1
         *   MOV byte ptr [EBP + 0x36c], CL ; actor+0x36C = result
         *
         * Result is the highest forward gear index (3..9). Replaces the prior
         * hardcoded `max_gear_index = 6` which was wrong for 6-gear cars whose
         * physics table reports CL=8 → write 9. */
        if (slot < TD5_MAX_RACER_SLOTS && actor->tuning_data_ptr) {
            int16_t *phys_scan = (int16_t *)actor->tuning_data_ptr;
            int cl = 2;
            const int16_t *p = &phys_scan[0x42 / 2];
            while (cl < 8 && *p < 0x270F) {
                cl++;
                p++;
            }
            actor->max_gear_index = (uint8_t)(cl + 1);
        } else {
            /* Traffic slot: original falls through the JGE @ 0042F1F1 branch and
             * does NOT run the gear-count scan; field stays 0 from the STOSD
             * zero-fill. In the port, the actor block is not zeroed at runtime
             * (re-init across races would leave stale data), so write 0
             * explicitly to match. */
            actor->max_gear_index = 0;
        }

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

        /* Faithful spawn settle: zero linear+angular velocities and run the
         * full integrator once. This places the chassis on the road via the
         * canonical chassis-snap (0x00406283-0x00406307) which uses the per-
         * wheel rotated body-offset Y term — the same formula that runs every
         * tick in flight. Earlier port had a custom corr_sum block here using
         * td5_track_probe_height that omitted the rot_y*-0x100 contribution,
         * leaving the chassis +128 FP (0.5 world units) above the original.
         * [CONFIRMED @ 0x42F140 → 0x00405D70 → 0x00405E80] */
        actor->linear_velocity_x = 0;
        actor->linear_velocity_y = 0;
        actor->linear_velocity_z = 0;
        actor->angular_velocity_yaw   = 0;
        actor->angular_velocity_pitch = 0;
        actor->angular_velocity_roll  = 0;
        actor->wheel_contact_bitmask = 0;
        td5_physics_integrate_pose(actor);
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
     * stay on carparam — they don't run through FUN_00404EC0.
     *
     * DRAG-MODE EXEMPTION (port enhancement, NOT original parity):
     * In the original, drag mode is encoded by g_dragRaceModeEnabled +
     * g_raceOverlayPresetMode==1 (set by ConfigureGameTypeFlags @ 0x00410CA0
     * case 9). InitializeRaceSession (0x0042AA10) reseats slots 1..5 to
     * span=1, lane=i-1, then sets their slot.state to '\x03' (decoration).
     * InitializeRaceActorRuntime (0x00432E60) gates the DAT_00473DB0 copy on
     * `slot.state == 0 && g_selectedGameType == 0`, so original slots 1..5 in
     * drag never get the AI template copied — they keep whatever bootstrap
     * tuning pointer they had at allocation, and they never run
     * UpdateVehicleActor (UpdateRaceActors @ 0x00436A70 skips state==3).
     * In short: original drag has NO 2-car race at all.
     *
     * The port's slot-1 racing enhancement (td5_game.c decoration_start=2 +
     * synthetic full-throttle driver in td5_ai.c:2948) makes slot 1 race as
     * AI. Without this exemption, bind_default_vehicle_tuning would pick the
     * AI template for slot 1 (since slot.state==0 in the port), giving slot 1
     * +17% torque (140 vs Viper carparam 120) and 2x wheelbase (24000 vs
     * 12000) — making the player lose every drag run by ~13% on
     * cumulative_timer. Since the original never exercises the AI template
     * for drag slot 1 (it's decoration), the cleanest port-side behavior for
     * the 2-car drag enhancement is "Viper-vs-Viper" via carparam — neither
     * original-faithful (original has no race) nor surprising (matches what
     * the original would do for a 2P split-screen drag, where both player
     * slots use carparam via the player path). */
    /* Original InitializeRaceActorRuntime @ 0x00432E60 gates the
     * DAT_00473DB0 AI-template copy on `slot.state == 0` (NOT slot index).
     * For slot 0 in PlayerIsAI=1 mode, g_race_slot_state[0] = 0 (AI), so
     * the original copies the AI template — same as it does for slots 1..5.
     *
     * The port previously gated on `slot >= 1` which excluded slot 0
     * unconditionally. With PlayerIsAI=1 + SINGLE_RACE, slot 0 (Viper)
     * was using its native carparam (high torque/top speed) while the
     * original would have given it the balanced AI template (Wf=400,
     * Wr=400, I=180000). Result: port slot 0 reaches walls at vlong=90041
     * vs slot 3 (car 17 with AI template) at ~78000 — the higher speed
     * produces unrecoverable wall impacts.
     *
     * [CONFIRMED via Ghidra agent audit 2026-05-02 — see memory entry
     *  reference_drag_ai_template_binding.md for the original's
     *  `slot.state == 0 && g_selectedGameType == 0` gate.]
     *
     * Note: the original ALSO gates on game_type==0 (SINGLE_RACE), but
     * the port's slots 1..5 in other modes have always used the AI
     * template here; preserving that until a separate audit pass. */
    /* PlayerIsAI=1 (port-only test flag) sets slot 0's state to 0 BEFORE the
     * tuning binding fires. That makes the next gate think slot 0 is a normal
     * AI car and copies the AI template over Viper's carparam — giving slot 0
     * a 17% higher torque_mult (140 vs 120 post-NORMAL-scaling) and 32%
     * higher gear ratios than its actual car. The resulting +54% drive_torque
     * overshoots the original's frida_force_player_ai parity baseline
     * (slot_state hacked AFTER init, so tuning stays bound to Viper carparam)
     * by ~41% in vlong growth from tick 1, which is what triggers the
     * Edinburgh "launches off road bumps and floats" symptom.
     *
     * Gate exception: when slot 0 is AI because of PlayerIsAI=1, skip the AI
     * template copy and let the carparam fallback below run. That makes
     * PlayerIsAI=1 parity-comparable to running force_player_ai.js on
     * TD5_d3d.exe with the same car. Slots 1..5 still get the AI template
     * (matches original behaviour for genuine AI racers).
     *
     * [Edinburgh 2026-05-11: original force_player_ai run reports
     * rpm=5985 torque_mult=120 gear_ratio=1398 drive_torque=655 at tick 1;
     * port pre-fix reported 7185/140/1850/1011. With this gate the port
     * should produce Viper-carparam values matching the original.] */
    int is_player_is_ai_slot0 = (slot == 0 && g_td5.ini.player_is_ai);

    if (slot < TD5_MAX_RACER_SLOTS && g_race_slot_state[slot] == 0 &&
        !is_player_is_ai_slot0 &&
        !(g_td5.drag_race_enabled && slot == 1 && s_carparam_loaded[slot])) {
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
        TD5_LOG_I(LOG_TAG,
                  "bind_tuning slot=%d: using carparam.dat data "
                  "(k_pos_damp=%d k_vel_damp=%d k_spring=%d k_travel_lim=%d k_load_scale=%d)",
                  slot,
                  (int)*(int16_t *)(tuning + 0x5E),
                  (int)*(int16_t *)(tuning + 0x60),
                  (int)*(int16_t *)(tuning + 0x62),
                  (int)*(int16_t *)(tuning + 0x64),
                  (int)*(int16_t *)(tuning + 0x66));
        return;
    }

    /* Fallback: hardcoded defaults.
     *
     * Reaching this branch means:
     *   (a) td5_asset_load_vehicle() didn't run for this slot, OR
     *   (b) it ran but couldn't find carparam.dat (missing re/assets/cars/<car>/
     *       symlink in the worktree, or zip archive missing), OR
     *   (c) the slot is >= TD5_MAX_TOTAL_ACTORS (impossible — caller guards).
     *
     * Precise-port audit sessions (pool5/pool6 etc) hit case (b) because their
     * worktrees don't have re/assets/cars/ symlinks. The captured "fallback"
     * cardef constants (k_pos_damp=48, k_vel_damp=96, k_spring=48, k_travel_lim=384)
     * come from the literals below — they are NOT what the live source-port
     * uses in production. Live use loads Viper's actual carparam.dat values
     * (50/40/30/12288 for Viper) via the branch above.
     *
     * Audit sessions wishing to reproduce production cardef values MUST link
     * re/assets/cars from the main tree into their worktree, e.g.:
     *   cmd //c "mklink /D re\\assets C:\\path\\to\\TD5RE\\re\\assets" */
    TD5_LOG_W(LOG_TAG,
              "bind_tuning slot=%d: FALLBACK HARDCODED DEFAULTS (s_carparam_loaded[%d]=0)"
              " — re/assets/cars/<car>/carparam.dat was not found at runtime",
              slot, slot);
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
 * ComputeVehicleSuspensionEnvelope (0x0042F6D0)  -- byte-faithful port
 *
 * Iterates all mesh vertices to compute axis-aligned extents, then writes
 * collision geometry into the per-slot row of gVehicleTuningTable
 * (in the port, mapped to actor->car_definition_ptr; carparam.dat seeds
 *  the same 0x8C-byte cardef rows the original references via ESI =
 *  &gVehicleTuningTable[slot*0x8c]):
 *   0x00-0x1C: 4 lower corners (Y = -X where X = y_val for racer / max_y for traffic)
 *   0x20-0x3C: 4 upper corners (Y = max_abs_y from mesh)
 *   0x60-0x7C: simplified traffic footprint (slot >= 6 only)
 *   0x80:      bounding sphere radius
 *   0x86:      Y height value (X = y_val for racer / max_y for traffic)
 *
 * Original quirks preserved here:
 *  - min_y (`local_10`) is computed by the loop but never read after — its
 *    stack slot `[ESP+0x10]` is overwritten by `BX` (neg_mx) at 0x42F822
 *    before any consumer. Earlier port versions fed min_y to traffic y_val;
 *    that was a port-only divergence and is removed below.
 *  - For traffic slots (param_2 >= 6), the post-loop FSTP ST0 at 0x0042F89A
 *    is GATED by the param_2<6 branch; the original therefore reuses the
 *    raw max_y still on x87 ST(0) as the "X" feeding the lower-corner Y,
 *    the 0x86 store, and the sphere radius. (Racer path replaces ST(0)
 *    with y_val via FMUL at 0x0042F8AA.)
 *  - Multiplier constant at 0x0045D6A8 is the literal 32-bit float
 *    0xBF350000 (-0.7070312500), NOT the C literal -0.707f (0xBF350481).
 *    Byte-faithful spelling uses the union punning below.
 *  - No `max_z<0` safety clamp -- the original does FSUB by 20 and uses
 *    the raw signed value; FTOL truncates toward zero (CW set RC=11
 *    via OR AH,0xC at 0x0044818B in __ftol).
 * ======================================================================== */

void td5_physics_compute_suspension_envelope(TD5_Actor *actor, int slot)
{
    if (!actor || !actor->car_definition_ptr) return;

    TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
    if (!mesh || mesh->total_vertex_count <= 0) return;

    int16_t *cd = get_cardef(actor);

    /* --- Iterate mesh vertices to find extents [CONFIRMED @ 0x0042F720-0x0042F7B9].
     * The original keeps fVar2 (max_y) on x87 ST(0) across iterations (loaded
     * pre-loop from _DAT_0045D624 = 0.0f at 0x0042F6D7). The decomp shows
     * per-axis compare/swap pattern; reordering the |y| updates next to each
     * other here is byte-equivalent for IEEE-754 32-bit float comparisons. --- */
    float max_x = 0.0f, max_y = 0.0f, max_z = 0.0f, min_y = 0.0f;
    int vert_count = mesh->total_vertex_count;
    TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;

    for (int i = 0; i < vert_count; i++) {
        float vx = verts[i].pos_x;
        float vy = verts[i].pos_y;
        float vz = verts[i].pos_z;
        if ( vx > max_x) max_x =  vx;
        if ( vy > max_y) max_y =  vy;
        if ( vz > max_z) max_z =  vz;
        if (-vx > max_x) max_x = -vx;
        if (-vy > max_y) max_y = -vy;
        if (-vz > max_z) max_z = -vz;
        if (vy < min_y)  min_y = vy;  /* dead in original; kept for symmetry */
    }
    (void)min_y;

    /* --- Post-loop "X" = the value the original keeps on x87 ST(0) feeding
     * the lower-corner Y, the 0x86 store, and the sphere radius:
     *   racer  (param_2 < 6): y_val = (cd[0x42] - cd[0x82]) * (-1/sqrt(2))
     *   traffic(param_2 >=6): X stays as max_y (the FSTP ST0 at 0x0042F89A
     *                         is gated out, leaving max_y still on stack).
     * --- */
    float y_val_x;
    if (slot < TD5_MAX_RACER_SLOTS) {
        /* Racer width clamp [CONFIRMED @ 0x0042F7CC-0x0042F7F2] */
        float delta = (float)((int32_t)cd[0x84 / 2] - (int32_t)cd[0x40 / 2]);
        if (delta > max_x) max_x = delta;
        /* Suspension-derived Y [CONFIRMED @ 0x0042F893-0x0042F8AA].
         * Constant at 0x0045D6A8 is exactly 0xBF350000 (-0.7070312500),
         * NOT the C literal -0.707f (0xBF350481). */
        const union { uint32_t u; float f; } k_neg_inv_sqrt2 = { 0xBF350000u };
        y_val_x = (float)((int32_t)cd[0x42 / 2] - (int32_t)cd[0x82 / 2])
                  * k_neg_inv_sqrt2.f;
    } else {
        /* Traffic: original leaves max_y on FPU stack; we mirror that. */
        y_val_x = max_y;
    }

    /* --- Add/subtract 20.0f padding [CONFIRMED @ 0x0042F7FA, 0x0042F808].
     * No safety clamp in the original -- FTOL truncates toward zero. --- */
    max_x += 20.0f;
    max_z -= 20.0f;

    /* Convert to int16. C `(int)` cast truncates toward zero, matching
     * the original `__ftol` (CW RC=11 then FISTP m64 at 0x00448195). */
    int16_t neg_mx = (int16_t)(int)(-max_x);
    int16_t pos_mx = (int16_t)(int)( max_x);
    int16_t my_i16 = (int16_t)(int)( max_y);
    int16_t mz_i16 = (int16_t)(int)( max_z);
    int16_t nmz_i16= (int16_t)(int)(-max_z);
    /* ny_i16 = (long)(-y_val_x) -- FCHS at 0x0042F8B7 negates the dup'd
     * ST(0) before the CALL __ftol that fills uVar4_final. */
    int16_t ny_i16 = (int16_t)(int)(-y_val_x);

    /* --- Upper AABB box at offsets 0x20-0x3C [CONFIRMED @ 0x0042F827-0x0042F88D] --- */
    cd[0x20 / 2] = neg_mx;   cd[0x22 / 2] = my_i16;  cd[0x24 / 2] = mz_i16;
    cd[0x28 / 2] = pos_mx;   cd[0x2a / 2] = my_i16;  cd[0x2c / 2] = mz_i16;
    cd[0x30 / 2] = neg_mx;   cd[0x32 / 2] = my_i16;  cd[0x34 / 2] = nmz_i16;
    cd[0x38 / 2] = pos_mx;   cd[0x3a / 2] = my_i16;  cd[0x3c / 2] = nmz_i16;

    /* --- Lower AABB box at offsets 0x00-0x1C [CONFIRMED @ 0x0042F8B0-0x0042F8F0] --- */
    cd[0x00 / 2] = neg_mx;   cd[0x02 / 2] = ny_i16;  cd[0x04 / 2] = mz_i16;
    cd[0x08 / 2] = pos_mx;   cd[0x0a / 2] = ny_i16;  cd[0x0c / 2] = mz_i16;
    cd[0x10 / 2] = neg_mx;   cd[0x12 / 2] = ny_i16;  cd[0x14 / 2] = nmz_i16;
    cd[0x18 / 2] = pos_mx;   cd[0x1a / 2] = ny_i16;  cd[0x1c / 2] = nmz_i16;

    /* --- Bounding sphere radius at 0x80 [CONFIRMED @ 0x0042F8F9-0x0042F921].
     *   FLD [max_z-20]; FMUL [max_z-20]    ; (max_z-20)^2
     *   FLD ST1; FMUL ST2; FADDP            ; + y_val_x^2
     *   FLD [max_x+20]; FMUL [max_x+20]; FADDP
     *   FSQRT; CALL __ftol                  ; sphere radius
     * --- */
    float r = sqrtf(max_z * max_z + y_val_x * y_val_x + max_x * max_x);
    cd[0x80 / 2] = (int16_t)(int)r;

    /* --- Y height at 0x86 [CONFIRMED @ 0x0042F8F4-0x0042F901] --- */
    cd[0x86 / 2] = (int16_t)(int)y_val_x;

    /* --- Traffic simplified footprint at 0x60-0x7C [CONFIRMED @ 0x0042F928-0x0042F97E].
     *   shrunk_x = (max_x+20) - (max_x+20) * 0.2  (i.e. (max_x+20) * 0.8)
     *   FMUL constant at 0x0045D6A4 = 0x3E4CCCCD = 0.2f (exact). --- */
    if (slot >= TD5_MAX_RACER_SLOTS) {
        float shrunk_x = max_x - max_x * 0.2f;
        int16_t neg_sx = (int16_t)(int)(-shrunk_x);
        int16_t pos_sx = (int16_t)(int)( shrunk_x);

        cd[0x70 / 2] = neg_sx;  cd[0x72 / 2] = 0;  cd[0x74 / 2] = mz_i16;
        cd[0x78 / 2] = pos_sx;  cd[0x7a / 2] = 0;  cd[0x7c / 2] = mz_i16;
        cd[0x60 / 2] = neg_sx;  cd[0x62 / 2] = 0;  cd[0x64 / 2] = nmz_i16;
        cd[0x68 / 2] = pos_sx;  cd[0x6a / 2] = 0;  cd[0x6c / 2] = nmz_i16;
    }

    TD5_LOG_I(LOG_TAG,
              "suspension_envelope slot=%d: max_x=%.1f max_y=%.1f max_z=%.1f "
              "y_val_x=%.1f radius=%d",
              slot, max_x, max_y, max_z, y_val_x, (int)cd[0x80 / 2]);
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
