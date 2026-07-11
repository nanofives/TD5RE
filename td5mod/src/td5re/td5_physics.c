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
#include "td5_fp.h"       /* FP_TRUNC/FP_SCALE/FP_ANGLE 24.8 fixed-point macros */
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* td5_input_ff_collision (wall/prop impact FF) */
#include "td5_vfx.h"      /* td5_vfx_queue_prop_break (TD6 prop debris) */
#include "td5_game.h"     /* td5_game_get_total_actor_count, td5_game_is_wanted_mode */
#include "td5_arcade.h"   /* arcade collision mult / ghost / wrecking-ball / launch */
#include "td5_damage.h"   /* [CAR DAMAGE] health from impacts, knockout freeze, handling penalty */
#include "td5_laneassist.h" /* optional lane-assist steering aid (port-only, default OFF) */
#include "td5_platform.h"
#include "td5_trace.h"    /* inner-tick physics_trace stages */
#include "td5_carparam.h" /* shared carparam field map + heaviness math (weight mechanics) */
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors (td5_env_int/float/flag_*) */
/* V2V trace headers are included unconditionally: their obb_corner_test /
 * collision_detect_full call sites below are ungated, so the snapshot types
 * must always be declared. The emitters self-stub to no-ops under
 * TD5RE_RELEASE (release build does not link the v2v trace modules). */
#include "td5re.h"
#include "td5_physics_internal.h"  /* PRIVATE core<->assists seam (SAR_RZ, tuning accessors, assist decls) */

/* Include the full actor struct for field-level access.
 * The build system must add TD5RE/re/include to the include path (-I). */
#include "../../../re/include/td5_actor_struct.h"

#include <string.h>  /* memset, memcpy */
#include <math.h>    /* cos, sin */
#include <stdlib.h>  /* abs */
#include <stdio.h>   /* FILE/fopen/fprintf — used by the [Trace] TrafficEdgePen
                      * CSV mirror at traffic_edge_pen (no-op when flag is 0).
                      * [TRACE 2026-05-24 traffic-edge-pen-cluster] */

#define LOG_TAG "physics"


extern void *g_actor_base;
extern uint8_t *g_actor_table_base;

/* [P1-B SPLIT step 2, 2026-07-02] OBB_CornerData + the collision fwd decls,
 * the wall collision response and the whole V2V cluster (OBB test, collision
 * response/impulse, mesh hitbox + hull, simple/full detection, anti-tunnel,
 * ResolveVehicleContacts broadphase, traffic crash spin) moved to
 * td5_physics_collision.c. Shared seam: td5_physics_internal.h. */

/* ========================================================================
 * Per-racer race telemetry (#10 race-end summary)
 * ========================================================================
 * Accumulated INSIDE the deterministic sim tick from replicated actor state
 * only (planar velocity / lateral slip / wheel-contact mask), so it is
 * lockstep- and replay-deterministic. Declared in td5re.h. See
 * td5_physics_accumulate_metrics() (called once per LIVE race tick from
 * td5_game_run_race_frame after td5_physics_tick) and the collision flag
 * sites in td5_physics_wall_response / apply_collision_response. */
TD5_RaceMetrics g_race_metrics[TD5_MAX_RACER_SLOTS];

/* Drift detection thresholds (raw 24.8 body-frame speed units, matching the
 * actor's longitudinal_speed / lateral_speed scale):
 *   - DRIFT_MIN_FWD : must be moving forward this fast to count as a drift
 *     (~12 MPH) so a stationary/parked spin or low-speed wiggle is ignored.
 *   - lateral slip ratio: |lateral_speed| * 4 >= |longitudinal_speed| means the
 *     sideways component is at least ~25% of forward speed = a real slide.
 *   - DRIFT_HOLD_TICKS : a drift must be held this many sim ticks (15 @30Hz =
 *     0.5 s) before it is counted, and is counted exactly ONCE per slide. */
#define TD5_DRIFT_MIN_FWD     0x3000   /* ~12 MPH in raw 24.8 fwd units */
#define TD5_DRIFT_HOLD_TICKS  15       /* 0.5 s at the 30 Hz sim tick */

/* Mark a per-slot collision event for THIS sim tick (rising edge counted in
 * td5_physics_accumulate_metrics). Safe for any slot value; ignores traffic /
 * out-of-range slots since only racer slots carry summary metrics. Extern
 * (not static inline) since the P1-B step-2 split: the collision TU calls it
 * but g_race_metrics stays core-private. */
void td5_physics_mark_collision(int slot)
{
    if (slot >= 0 && slot < TD5_MAX_RACER_SLOTS)
        g_race_metrics[slot].hit_this_tick = 1;
}

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

/* [task#15 2026-06-13] TD6 surface routing. On TD6 tracks td5_track_get_surface_type
 * returns 0x80|grid_class (the per-lane SURFACE-GRID class), which must index the
 * ported TD6 grip/drag tables (td5_track @0x0049d7b8/0x0049d7f8) instead of the
 * TD5 surface tables. These helpers keep every physics call site one-liner-clean
 * and leave native TD5 (no 0x80 bit) byte-identical. */
/* [SNOW GRIP 2026-06-27] Snowy tracks (Bern etc.) feel slippery because their
 * track spans use the "ice" surface (index 6, grip 0xB4 = 180 = 0.70 q8 — the
 * lowest entry in gSurfaceGripCoefficientTable @ 0x004748C0). The original couples
 * NO weather term to grip; the slip is purely those low ice/mud/off-track
 * coefficients. To make snowy tracks easier to drive WITHOUT changing dry-track
 * handling, lift every sub-full-grip surface coefficient toward full grip (0x100)
 * by TD5RE_SNOW_GRIP percent of its deficit — but ONLY while the active track's
 * weather is SNOW. Full-grip surfaces (tarmac/default = 0x100) have zero deficit
 * so they are mathematically untouched. We lift ALL slippery entries (not just
 * idx 6) because whether a given snow track's racing line writes the primary ice
 * index (6) or the lane-alternate ice index (25 = 0xC0) lives in STRIP.DAT, not
 * the exe — boosting the whole sub-1.0 set is correct either way. Default 65;
 * =0 reverts to byte-faithful original grip. Raising grip increases both the
 * load-weighted grip limit (line ~3721) and the raw lateral/longitudinal force
 * (line ~4292), so the car slides less and is easier to control. */
static inline int32_t phys_snow_grip_boost(int32_t grip)
{
    static int s_pct = -1;
    if (s_pct < 0) {
        s_pct = td5_env_int("TD5RE_SNOW_GRIP", 65, 0, 100);   /* % of the grip deficit to recover */
        TD5_LOG_I(LOG_TAG, "phys_snow_grip_boost: TD5RE_SNOW_GRIP=%d%% "
                  "(lift slippery surfaces toward full grip on SNOW weather)", s_pct);
    }
    if (s_pct == 0 || g_td5.weather != TD5_WEATHER_SNOW) return grip;
    if (grip >= 0x100) return grip;            /* full-grip surface: unchanged */
    return grip + (int32_t)(((int64_t)(0x100 - grip) * s_pct) / 100);
}

static inline int32_t phys_surface_grip(int surface)
{
    int32_t g;
    if (td5_track_td6_surface_grid_loaded()) g = td5_track_td6_surface_grip_q8(surface & 0x1F);
    else g = (int32_t)s_surface_friction[surface & 0x1F];
    return phys_snow_grip_boost(g);   /* [SNOW GRIP] no-op off SNOW weather */
}
static inline int32_t phys_surface_drag(int surface)
{
    if (td5_track_td6_surface_grid_loaded()) return td5_track_td6_surface_drag(surface & 0x1F);
    return (int32_t)s_surface_grip[surface & 0x1F];
}

/* [#15 2026-06-19] TD6 grass/verge SLIDE. The faithful [MIN,MAX] grip clamp caps
 * a low-grip TD6 surface (class 16 = 0.74, 18 = 0.54) back UP to road level, so
 * the verge only saps acceleration (the raw longitudinal grip), not cornering —
 * grass never feels like grass. Re-apply the surface's grip ratio to the already
 * CLAMPED lateral grip so a wheel that drifts onto the verge actually loses
 * cornering traction and the car slides. TD6-only; native TD5 untouched.
 * A/B: TD5RE_TD6_GRASS_SLIDE (default on; =0 reverts to the faithful clamp). */
static inline int32_t phys_td6_grass_slide(int32_t grip, int surface)
{
    static int s_en = -1;
    if (s_en < 0) {
        s_en = td5_env_flag_on("TD5RE_TD6_GRASS_SLIDE");
    }
    if (!s_en || !td5_track_td6_surface_grid_loaded()) return grip;
    int q8 = td5_track_td6_surface_grip_q8(surface & 0x1F);   /* 0x100 = full grip */
    if (q8 >= 0x100) return grip;                             /* full-grip surface: unchanged */
    return (int32_t)(FP_TRUNC(((int64_t)grip * (int64_t)q8)));    /* scale by the surface ratio */
}

/* --- Globals matching original binary layout --- */
int32_t g_gravity_constant = TD5_GRAVITY_NORMAL;   /* shared w/ drivetrain TU (surface gravity) */

/* Pilot-trace accessor — exposes g_gravity_constant to the pilot
 * trace module without leaking the static. Read-only debug use. */
int32_t td5_physics_dbg_get_gravity_constant(void) {
    return g_gravity_constant;
}
int32_t g_collisions_enabled = 0;     /* DAT_00463188 (== orig's `g_cameraMode`):
                                                * 0 = normal play / collisions ON / no-clip OFF,
                                                * 1 = no-clip mode / collisions OFF. The two names
                                                * refer to the SAME dword; the user-facing INI
                                                * "Collisions" knob is XOR'd into it (frontend at
                                                * 0x004155BD / 0x0041DC8E). Setter `td5_physics_set_collisions`
                                                * preserves that inversion. */
int32_t g_game_paused = 0;                   /* DAT_004AAD60 (shared w/ assists TU via td5_physics_internal.h) */
static int32_t g_xz_freeze = 0;             /* g_freezeHorizontalIntegration: 1=freeze XZ during countdown */

/* [COP CHASE ARREST FREEZE 2026-06-25] An arrested suspect is fully immobilized
 * (its drive integrator is skipped and its velocity is zeroed every tick). Knob
 * TD5RE_COPCHASE_ARREST_FREEZE=0 reverts to the pre-arrest behaviour (AI brakes to
 * a stop, human suspects can still roll). Default ON. */
static int td5_copchase_arrest_freeze_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = td5_env_flag_on("TD5RE_COPCHASE_ARREST_FREEZE");
    }
    return cached;
}
static int32_t s_dynamics_mode = 0;          /* 0=arcade, 1=simulation (0x42F7B0) */
static int32_t g_difficulty_easy = 0;
int32_t g_difficulty_hard = 0;
int32_t g_race_slot_state[TD5_MAX_RACER_SLOTS]; /* 1=human, 0=AI per slot */

/* [CAR BROKE DOWN 2026-07-10] Public read of the per-slot human flag (owns
 * g_race_slot_state). Lets the HUD show the broke-down prompt only on human
 * panes without pulling in td5_physics_internal.h. */
int td5_physics_slot_is_human(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return g_race_slot_state[slot] == 1;
}

/* Viewport -> actor-slot map (defined in td5_game.c). Used by the stuck-recovery
 * driver to map a local human player index back to its actor slot, and to read
 * that player's one-shot manual-recovery edge from the input layer. */
extern int g_actorSlotForView[TD5_MAX_VIEWPORTS];

/* [P1-B SPLIT 2026-07-02] The PORT-ONLY assists that lived here (stuck-recovery
 * state, MP/hard catch-up, gearbox mode + manual boost, slope/hill/weight/draft/
 * downforce, FF signal getters, crash FX + battle/police knobs, gentle recovery)
 * moved to td5_physics_assists.c. Shared seam: td5_physics_internal.h. */
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
static int32_t g_slot_series_position[TD5_MAX_RACER_SLOTS] = {0};
static int32_t g_slot_race_result[TD5_MAX_RACER_SLOTS];
static int32_t g_slot_race_bonus [TD5_MAX_RACER_SLOTS];
static int32_t g_slot_race_points[TD5_MAX_RACER_SLOTS];

/* s_prev_grounded_mask moved to td5_physics_suspension.c (P1-B step 4) with
 * its writers/readers (suspension response snapshot + landing detection). */

/* [#1 WRECK STAND-STILL 2026-06-21] Free-slide window (ticks) for a broken-down
 * traffic car: armed by a real V2V ram in the impulse resolver (now in
 * td5_physics_collision.c, hence non-static), counted down in
 * td5_physics_update_traffic. While > 0 the wreck coasts (so the player can shove
 * it aside, issue #2); at 0 it is anchored and stands still. Sized for all (incl.
 * traffic) slots. */
int16_t g_wreck_push_ticks[TD5_MAX_TOTAL_ACTORS];


/* [P1-B SPLIT step 4, 2026-07-02] The suspension/wheel-contact/pose cluster
 * (damped suspension force, wheel travel, suspension response, traffic edge
 * containment, traffic + vehicle pose integration, wheel-contact refresh,
 * attitude clamp) and its per-slot state moved to td5_physics_suspension.c.
 * Seam: td5_physics_internal.h. */



/* V2V inertia constant, the per-actor AABB table + spatial-grid broadphase
 * state and the anti-tunnel tuning macros moved to td5_physics_collision.c
 * (P1-B step 2) along with their only consumers. OBB_CornerData + the
 * collision fwd decls moved there too. */
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

/* update_engine_speed_smoothed + compute_reverse_gear_torque: extern, in
 * td5_physics_drivetrain.c since the P1-B step-3 split (decls in
 * td5_physics_internal.h). */
void update_vehicle_pose_from_physics(TD5_Actor *actor);  /* extern: collision TU re-snaps poses */
static void bind_default_vehicle_tuning(TD5_Actor *actor, int slot);

static inline void write_i16(uint8_t *base, size_t offset, int16_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

static inline void write_i32(uint8_t *base, size_t offset, int32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

/* TD5_F32_SPILL (x87 float32 spill macro) moved to td5_physics_internal.h —
 * its remaining users are in td5_physics_suspension.c (pose integration). */

/* ========================================================================
 * Fixed-point trig (12-bit angle, returns 12-bit result)
 *
 * Thin wrappers over the byte-faithful integer LUT shared with the rest of
 * the port. The original physics path NEVER calls libm: it indexes the
 * fixed-point sin/cos LUT g_sinCosLut_fixed12 @ 0x00483984 via CosFixed12bit
 * @ 0x0040A6E0 / SinFixed12bit @ 0x0040A700, and the angle LUT DAT_00463214
 * via AngleFromVector12 @ 0x0040A720.
 *
 * [CONFIRMED @ 0x0040A6E0/0x0040A700] CosFixed12bit/SinFixed12bit are the
 *   INTEGER LUT cos/sin (not the float family 0x0040A6A0/0x0040A6C0). The
 *   player/AI/wall/collision physics functions (0x00404030, 0x00404EC0,
 *   0x00406980, 0x004079C0, 0x00409520, 0x00443CF0) all call this integer
 *   family; no physics path uses the float LUT.
 * [CONFIRMED @ 0x0040A720] AngleFromVector12(dx, dz): first arg = param_1 =
 *   dx (horizontal), second = param_2 = dz; angle 0 ⇄ +dz, increasing toward
 *   +dx. The three atan2_fixed12 call sites map 1:1 to the original's
 *   AngleFromVector12 args (0x00406CC0 wall-edge B.z-A.z/B.x-A.x; 0x00443CF0
 *   roll -rotated_z/-normal_y; 0x00443CF0 pitch rotated_x/mag_xz).
 *
 * Input: 12-bit angle (0-4095 = 0-360 degrees), masked internally by the LUT.
 * Output: signed 12-bit result (-4096 .. +4096) for cos/sin; 0-4095 for atan2.
 *
 * Routing these to the LUT (was libm cos/sin/atan2 *4096) removes the
 * per-angle truncation drift the host-trig path leaked into wall response,
 * steering, heading, and collision normals.
 * ======================================================================== */

int32_t cos_fixed12(int32_t angle)
{
    return (int32_t)CosFixed12bit((unsigned int)angle);
}

int32_t sin_fixed12(int32_t angle)
{
    return (int32_t)SinFixed12bit(angle);
}

/* ========================================================================
 * Fixed-point atan2 (12-bit result: 0-4095 = 0-360°)
 *
 * Byte-faithful port of AngleFromVector12 @ 0x0040A720 (atan LUT
 * DAT_00463214). Input: (dx, dz) in world coords (dx=param_1, dz=param_2).
 * Returns: 12-bit angle where 0 = +Z direction, CW positive. The & 0xFFF
 * folds the degenerate octant-7 (0x1000) result back into range, preserving
 * the prior call-site masking.
 * ======================================================================== */

int32_t atan2_fixed12(int32_t dx, int32_t dz)
{
    return AngleFromVector12(dx, dz) & 0xFFF;
}


/* Tuning data access helpers (get_phys/get_cardef, the PHYS_ and CDEF_ offset
 * tables, ACTOR_I16/I32) moved to td5_physics_internal.h (shared with
 * td5_physics_assists.c). */

/* [COP-CHASE 2026-06-21] AI suspect top-speed penalty in Cop Chase / wanted mode,
 * as a percent of the per-car rating (default 70 = 30% slower) so the suspect
 * stays catchable on higher difficulties. Tunable: TD5RE_COPCHASE_AI_SPEED_PCT. */
static int copchase_ai_speed_pct(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_COPCHASE_AI_SPEED_PCT");
        int pct = e ? atoi(e) : 70;
        if (pct < 10)  pct = 10;
        if (pct > 100) pct = 100;
        cached = pct;
    }
    return cached;
}

/* [POLICE rewrite 2026-06-19] Effective top-speed rating (tuning +0x74). A
 * chasing police cop has NO top-speed cap — it must be able to overtake any
 * car — so it gets a very high rating (its real speed is still bounded to ~1.5x
 * the chased car by the catch-up throttle in cop_drive). Everyone else uses the
 * per-car tuning value, so non-cops are byte-identical. The actor's own slot
 * index lives at +0x375. */
int32_t phys_top_speed_rating(TD5_Actor *actor) {
    int slot = (int)((const uint8_t *)actor)[0x375];   /* ACTOR_SLOT_INDEX */
    if (td5_ai_cop_is_chasing(slot)) return 0x7FFF;    /* effectively uncapped */
    int32_t rating = (int32_t)PHYS_S(actor, PHYS_TOP_SPEED);
    /* Slow the SUSPECT(s) so a Cop Chase is winnable. The cop keeps full speed;
     * every other racer (suspect) is debuffed. SP wanted (cop=slot 0) debuffs
     * the AI slots 1..5 exactly as before; MP cop chase debuffs every non-cop
     * racer (incl. human suspects, per "same debuff on the other players").
     * Non-cop-chase races are byte-faithful (is_suspect returns 0). */
    if (td5_game_cop_chase_is_suspect(slot))
        rating = (rating * copchase_ai_speed_pct()) / 100;
    /* [MP CATCHUP top-speed 2026-06-24] Human MP catch-up paces the cap off the
     * next opponent ahead: raised when far behind (rejoin), lowered when closing
     * in (so the human coasts in behind instead of ramming — the speed-limit gate
     * zeroes drive WITHOUT braking). Inert (1.0) for AI/traffic, single-player and
     * a disabled feature, so non-MP play is byte-identical. Applied here so all
     * three speed-limit gates (player on-ground / airborne, AI) inherit it. */
    {
        int32_t ts_q8 = td5_physics_mp_catchup_ts_mult(slot);
        if (ts_q8 != MP_CATCHUP_Q8_ONE)
            rating = (int32_t)(FP_TRUNC(((int64_t)rating * (int64_t)ts_q8)));
    }
    /* [ARCADE NITRO 2026-07-04] Raise the cap (default +50%) while NITRO is
     * active so the boosted drive torque can actually be exploited instead of
     * hitting the same faithful cap sooner. Inert (100%) outside arcade mode or
     * when NITRO isn't active on this slot. */
    {
        int pct = td5_arcade_slot_topspeed_pct(slot);
        if (pct != 100)
            rating = (int32_t)(((int64_t)rating * pct) / 100);
    }
    return rating;
}


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
        /* Full 32-entry table read from the binary at DAT_004748C0 (2026-05-26).
         * Entries 16-31 are the ALTERNATE (off-strip/grass) surfaces returned by
         * GetTrackSegmentSurfaceType's high-nibble path `(attr>>4)|0x10`. The
         * port previously filled 16-31 with 0x0100 (full grip), so alternate
         * surfaces wrongly had tarmac grip. Binary has them at 0xC0 (192). */
        static const int16_t k_grip_004748C0[32] = {
            0x0100, 0x0100, 0x00DC, 0x00F0, 0x00FC, 0x00C0, 0x00B4, 0x0100,
            0x0100, 0x0100, 0x00C8, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
            0x00C0, 0x00C0, 0x00C0, 0x00DC, 0x00C0, 0x0100, 0x00C0, 0x00C0,
            0x00C0, 0x00C0, 0x00C0, 0x00C0, 0x00C0, 0x00C0, 0x00C0, 0x00C0
        };
        for (int i = 0; i < 32; i++)
            s_surface_friction[i] = k_grip_004748C0[i];
    }

    /* Surface drag coefficients from DAT_00474900 (short[32]).
     * NOTE: s_surface_grip is used for drag/damping at line ~393. */
    {
        /* Full 32-entry table read from the binary at DAT_00474900 (2026-05-26).
         * Entries 16-31 (alternate/grass surfaces) carry drag 0x20 (32) in the
         * binary; the port previously zeroed them, so alternate surfaces added
         * no slowdown. */
        static const int16_t k_drag_00474900[32] = {
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0002, 0x0000, 0x0000,
            0x0000, 0x0000, 0x0008, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0010, 0x0020, 0x0020,
            0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020
        };
        for (int i = 0; i < 32; i++)
            s_surface_grip[i] = k_drag_00474900[i];
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
        float dz = (float)(cur->z - prev->z);
        actor->render_pos.x = ((float)prev->x + dx * subtick_fraction) * kInv256;
        /* Y deliberately NOT interpolated. The original's integrate_pose
         * (0x00405E80) writes render_pos.y exactly once at the pre-snap
         * (post-gravity) step; the chassis-snap at 0x00406300 updates
         * world_pos.y only. Lerping prev->cur world_pos.y here with
         * subtick_fraction ~= 1.0 re-introduces the post-snap value that
         * commit 43fa800 was eliminating -- producing the persistent
         * slot-0 +7.42 fp8-unit render_pos.y delta vs orig. Camera/HUD
         * readers of render_pos.y should see the pre-snap value frozen
         * between sim ticks (orig's body-mesh draw at 0x0040C164
         * extrapolates Y separately via velocity, doesn't touch render_pos).
         * Interpolating X/Z only preserves the high-framerate rubber-band
         * mitigation of commits 97c6756 / 7631b59 for slide motion.
         * See memory todo_render_pos_y_residual_2026-05-16.md (Agent J). */
        actor->render_pos.z = ((float)prev->z + dz * subtick_fraction) * kInv256;
    }
}

/* [task#14 2026-06-14] TD6 breakable-prop collision + break effect (port of
 * TD6.exe FUN_00441070 broadphase + the impulse/break core of FUN_0043e700).
 * Props are collision volumes anchored to the baked world geometry (the visible
 * lampposts/bins/traffic-furniture; London ships ~199, other TD6 tracks ~none).
 *
 * BREAK MODEL: street furniture is light, so a hit doesn't bounce the car off a
 * wall — it knocks the prop over. For each wheel within radius*16 world units of
 * an un-broken prop we (1) absorb ~25% of the inward velocity (a thud, not a
 * rebound), (2) if the approach was hard enough (>0x3200, the same threshold the
 * wall handler uses) play a crash SFX + FF rumble + queue a debris dust-burst,
 * and (3) mark the prop broken one-shot so the car then plows straight through
 * it. Any actor (player/AI/traffic) can break a prop. No-op on non-prop tracks. */
static int td6_props_enabled(void)
{
    static int s = -1;   /* A/B knob: TD5RE_TD6_PROPS=0 disables prop collision */
    if (s < 0) { s = td5_env_flag_on("TD5RE_TD6_PROPS"); }
    return s;
}

/* [#20 pushable] A/B knob: TD5RE_TD6_PUSH=0 reverts to break-everything-on-contact.
 * Default on: props (MOV byte-6 mass != 0) are SHOVED and the car's speed penalty
 * scales with mass (light props barely slow it); a prop only BREAKS on a hard,
 * heavy impulse. Matches the original (RE: mass=0 immovable, break when impulse>30000). */
static int td6_push_enabled(void)
{
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_TD6_PUSH"); }
    return s;
}

static void td5_physics_check_td6_props(TD5_Actor *actor)
{
    int n, i, w;
    int32_t cx, cz, vx, vz, vx0, vz0, speed;
    int32_t seg_ax, seg_az, seg_dx, seg_dz;
    int64_t seg_len2;
    if (!actor) return;
    if (!td6_props_enabled()) return;
    n = td5_track_td6_prop_count();
    if (n <= 0) return;
    vx = vx0 = actor->linear_velocity_x;
    vz = vz0 = actor->linear_velocity_z;
    /* Speed gate: only interact with props while genuinely driving (>~5 MPH).
     * This stops the start grid (cars spawned at spans 2-17, ON the props at
     * spans 2/5/9) and the stationary countdown from kicking cars into the
     * rails — the "invisible wall at the start" — and also means a prop only
     * breaks when you actually drive into it at speed. */
    speed = (int32_t)td5_isqrt((uint32_t)((int64_t)vx * vx + (int64_t)vz * vz));
    if (speed < 0x1000) return;   /* ~3 MPH: ignore the stationary start grid */
    cx = actor->world_pos.x; cz = actor->world_pos.z;
    /* Model the car body as a CAPSULE along its HEADING: segment [center +/- fwd*
     * half_len], radius half_width (=550, folded into the rw+550 test below). The
     * earlier swept-segment + velocity-forward-reach mis-fired BOTH ways — the
     * velocity direction lies when you turn (kick before you touch / miss entirely),
     * and a prop sitting directly ahead BETWEEN the front wheels fell in the centre
     * test's blind spot (>550 from centre, inside no wheel). A heading-aligned
     * capsule covers the whole footprint, fires exactly at contact, and is steer-
     * correct. half_len is derived from the frontmost wheel so the front cap
     * (half_len + half_width) lands on the bumper for any car. */
    {
        int32_t yaw = (int32_t)actor->display_angles.yaw & 0xFFF;
        float fwx = td5_sin_12bit((uint32_t)yaw);
        float fwz = td5_cos_12bit((uint32_t)yaw);
        float maxp = 0.0f;
        int   wi;
        int32_t half_len;
        for (wi = 0; wi < 4; wi++) {
            float rx = (float)(actor->wheel_contact_pos[wi].x - cx) * (1.0f / 256.0f);
            float rz = (float)(actor->wheel_contact_pos[wi].z - cz) * (1.0f / 256.0f);
            float p  = rx * fwx + rz * fwz; if (p < 0.0f) p = -p;   /* longitudinal */
            if (p > maxp) maxp = p;
        }
        half_len = (int32_t)maxp - 300;   /* + bumper overhang (~250) - cap radius (550) */
        if (half_len < 50) half_len = 50;
        seg_ax = (FP_TRUNC(cx)) - (int32_t)(fwx * (float)half_len);
        seg_az = (FP_TRUNC(cz)) - (int32_t)(fwz * (float)half_len);
        seg_dx = (int32_t)(fwx * (float)(half_len * 2));
        seg_dz = (int32_t)(fwz * (float)(half_len * 2));
    }
    seg_len2 = (int64_t)seg_dx * seg_dx + (int64_t)seg_dz * seg_dz;
    for (i = 0; i < n; i++) {
        int32_t px, pz, rw, hy, pxw, pzw, qx, qz;
        int64_t closest_d2, t_num;
        int hit;
        if (td5_track_td6_prop_is_broken(i)) continue;   /* already knocked over */
        if (!td5_track_td6_prop_get(i, &px, &pz, &rw, NULL)) continue;
        pxw = FP_TRUNC(px); pzw = FP_TRUNC(pz);
        /* closest point on the swept segment (seg_a -> current) to the prop */
        t_num = (int64_t)(pxw - seg_ax) * seg_dx + (int64_t)(pzw - seg_az) * seg_dz;
        if (seg_len2 <= 0 || t_num <= 0) { qx = seg_ax;          qz = seg_az; }
        else if (t_num >= seg_len2)      { qx = seg_ax + seg_dx; qz = seg_az + seg_dz; }
        else { qx = seg_ax + (int32_t)(t_num * seg_dx / seg_len2);
               qz = seg_az + (int32_t)(t_num * seg_dz / seg_len2); }
        { int32_t ddx = pxw - qx, ddz = pzw - qz;
          closest_d2 = (int64_t)ddx * ddx + (int64_t)ddz * ddz; }
        if (closest_d2 > (int64_t)(rw + 6000) * (rw + 6000))
            continue;                                    /* broadphase */
        /* Solid hit when the swept car body (half-width ~550) reaches the prop,
         * OR a wheel is inside. The swept segment kills high-speed tunnelling. */
        hit = (closest_d2 < (int64_t)(rw + 550) * (rw + 550));
        hy  = actor->wheel_contact_pos[0].y;
        if (!hit) {
            for (w = 0; w < 4; w++) {
                int32_t dx = FP_TRUNC((actor->wheel_contact_pos[w].x - px));
                int32_t dz = FP_TRUNC((actor->wheel_contact_pos[w].z - pz));
                if ((int64_t)dx * dx + (int64_t)dz * dz < (int64_t)rw * rw) {
                    hit = 1; hy = actor->wheel_contact_pos[w].y; break;
                }
            }
        }
        if (!hit) continue;
        {
            /* Impact strength = the car's PLANAR SPEED (raw velocity units,
             * ~1252 per MPH so 0x3200 ~= 10 MPH); robust to contact angle. */
            int32_t strength = (int32_t)td5_isqrt(
                    (uint32_t)((int64_t)vx * vx + (int64_t)vz * vz));
            int mass = td5_track_td6_prop_mass(i);   /* MOV byte 6; 0 = immovable */
            int m4 = mass * 4; if (m4 < 1) m4 = 1;   /* TD6 internal mass = byte6<<2 */
            int do_break;
            if (!td6_push_enabled() || mass == 0) {
                /* [#20] Legacy / immovable: solid thud (shed ~1/4 speed). Immovable
                 * props never break; with the knob off everything breaks as before. */
                vx -= vx >> 2; vz -= vz >> 2;
                do_break = (td6_push_enabled() ? 0 : 1);
            } else {
                /* [#20 PUSHABLE] Shove the prop in the car's travel direction, scaled
                 * inversely by mass (light bench m4=100 ~0.89x car speed, heavy m4=664
                 * ~0.55x) — the kick the user liked. The car slows proportionally to
                 * mass (light barely, heavy noticeably). Prop slides/decays + slide-cap
                 * via td5_track_td6_props_tick. */
                int32_t dvx = (int32_t)(((int64_t)vx0 * 800) / (800 + m4));
                int32_t dvz = (int32_t)(((int64_t)vz0 * 800) / (800 + m4));
                td5_track_td6_prop_push(i, dvx, dvz);
                vx -= (int32_t)(((int64_t)vx * m4) / (4000 + m4));
                vz -= (int32_t)(((int64_t)vz * m4) / (4000 + m4));
                /* Break only on a hard, heavy impulse (mass-weighted speed). Light
                 * furniture (low mass) is effectively unbreakable — it just shoves. */
                do_break = ((int64_t)strength * mass > 6000000LL);
                if (do_break) { vx -= vx >> 2; vz -= vz >> 2; }
            }
            if (do_break)
                td5_track_td6_prop_set_broken(i);   /* one-shot: now drive through */
            TD5_LOG_D(LOG_TAG, "TD6 prop hit: idx=%d slot=%d strength=%d mass=%d %s",
                      i, (int)actor->slot_index, strength, mass, do_break ? "BREAK" : "push");
            if (strength > 0x2400) {
                int32_t wpos[3] = { px, hy, pz };
                int variant = (strength < 0x19000) ? 0x16 : 0x1b;
                int mag     = (strength < 0x19000) ? 0x5622 : 0x2198;
                int volume  = (strength < 0x19000) ? 1 : 4;
                int32_t pitch = strength - 0x2000;
                int32_t ff_mag;
                if (pitch < 0x400) pitch = 0x400;
                else if (pitch > 0x800) pitch = 0x800;
                /* crash/thud SFX (mirrors the wall-impact param order @0x406B70) */
                td5_sound_play_at_position(variant, pitch, mag, wpos, volume);
                ff_mag = strength >> 2;
                if (ff_mag > 99999) ff_mag = 100000;
                td5_input_ff_collision(0x01, (int)actor->slot_index, -1, ff_mag);
                /* debris burst only when the prop actually BREAKS (not on a push) */
                if (do_break)
                    td5_vfx_queue_prop_break((float)px * (1.0f / 256.0f),
                                             (float)hy * (1.0f / 256.0f),
                                             (float)pz * (1.0f / 256.0f),
                                             strength);
            }
        }
    }
    if (vx != vx0 || vz != vz0) {
        actor->linear_velocity_x = vx;
        actor->linear_velocity_z = vz;
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

    /* [MP CATCHUP] Recompute the per-human catch-up drive multiplier for this
     * deterministic sim tick BEFORE the per-actor integration loop, so every
     * actor's drive-torque pass this tick reads a consistent value. Pure
     * function of replicated state (track_span_high_water + slot human/AI),
     * so it is identical on every lockstep client. Inert (all 1.0) unless
     * TD5RE_MP_CATCHUP=1 AND >=2 humans are racing. */
    td5_physics_update_mp_catchup();

    /* [HARD CATCHUP item #13] Same deal for the Hard-difficulty AI catch-up:
     * recompute the per-AI-slot drive boost (gap behind the player) once per
     * tick before integration. Pure replicated-state function; inert (all 1.0)
     * unless TD5RE_HARD_CATCHUP=1 AND g_difficulty_hard. */
    td5_physics_update_hard_catchup();

    /* [SLIPSTREAM 2026-06-25] Recompute every racer's draft boost (car closely
     * ahead in its wake) once per tick BEFORE integration, so the drive-torque
     * pass reads a consistent value. Pure function of replicated actor
     * positions/headings -> lockstep-deterministic. Inert (all 1.0) when off. */
    td5_physics_update_draft();

    s_physics_tick_counter++;
    if ((s_physics_tick_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Physics tick: actor_count=%d", total);
    }

    for (int slot = 0; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        td5_physics_update_vehicle_actor(actor);
    }

    /* [FF SIGNALS #1] Refresh per-slot force-feedback signals (drift level /
     * gear-change sequence / at-redline) once per sim tick, right after the
     * per-actor integration loop so they read this tick's settled state. Pure
     * replicated-state integer math, consumed by td5_input.c per render frame. */
    td5_physics_update_ff_signals();

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

    /* [CAR BROKE DOWN 2026-07-10] Age the post-recovery ghost window (per slot)
     * BEFORE the recovery driver, so a ghost armed this tick keeps its full
     * duration. Tick-based -> net-deterministic. Inert when CarDamage is off. */
    td5_damage_tick_ghost();

    /* [STUCK RECOVERY 2026-06-15; net-safe rewrite 2026-07-10] After this tick's
     * integration + contact resolution have settled, run the car-recovery driver:
     * manual (R / SELECT) reposition. Deterministic (sim-tick cadence; the request
     * rides the merged control_bits word, so it fires on the same lockstep round
     * on every peer). Inert when TD5RE_STUCK_RECOVERY=0. */
    td5_physics_update_stuck_recovery();
}

/* ========================================================================
 * Race telemetry (#10 race-end summary)
 * ======================================================================== */

/* 64-bit integer square root (deterministic, no float). The shared td5_isqrt
 * takes int32_t, but a planar velocity magnitude sum (vx^2+vz^2) at race speed
 * overflows int32 — keep the whole thing in int64 so top/avg speed are exact. */
static int64_t td5_isqrt64(int64_t x)
{
    if (x <= 0) return 0;
    int64_t r = 0, b = (int64_t)1 << 62;
    while (b > x) b >>= 2;
    while (b > 0) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return r;
}

void td5_physics_reset_metrics(void)
{
    memset(g_race_metrics, 0, sizeof(g_race_metrics));
}

const TD5_RaceMetrics *td5_physics_get_metrics(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return NULL;
    return &g_race_metrics[slot];
}

/* td5_physics_accumulate_metrics -- one LIVE race sim tick of per-racer
 * telemetry. Called from td5_game_run_race_frame() once per live tick, AFTER
 * td5_physics_tick() (so collision flags set during this tick's contact
 * resolution are visible). NOT called during the start countdown or while
 * paused — cars are stationary then and nothing should accumulate.
 *
 * Determinism: every input here is replicated sim state (planar velocity,
 * body-frame lateral/longitudinal speed, wheel-contact mask) and integer math
 * (td5_isqrt), so the result is identical on every lockstep client and on a
 * replay. No wall-clock, RNG, or render-rate inputs. */
void td5_physics_accumulate_metrics(void)
{
    if (!g_actor_table_base) return;

    int total = td5_game_get_total_actor_count();
    if (total <= 0) return;

    /* Racer slots only (humans + AI); traffic carries no summary metrics. */
    int racers = total;
    if (racers > g_traffic_slot_base) racers = g_traffic_slot_base;
    if (racers > TD5_MAX_RACER_SLOTS) racers = TD5_MAX_RACER_SLOTS;

    for (int slot = 0; slot < racers; ++slot) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        if (!a) continue;
        TD5_RaceMetrics *m = &g_race_metrics[slot];

        /* --- collisions: rising edge of "hit this tick" vs "hit last tick" ---
         * A sustained scrape (flagged for several consecutive ticks) counts as
         * ONE collision; a fresh impact after separating counts as another.
         * Always processed (even after finish) so the edge state stays sane. */
        if (m->hit_this_tick && !m->hit_prev_tick)
            m->collisions++;
        m->hit_prev_tick = m->hit_this_tick;
        m->hit_this_tick = 0;

        /* Skip the rest once this car has finished — its top/avg/air/drift are
         * frozen at the finish line (mirrors the original's finish_time==0 gate
         * on peak/avg speed). A decoration/parked slot stays at ~0 naturally. */
        if (a->finish_time != 0)
            continue;

        /* --- speed: planar velocity magnitude in raw display units ---
         * Same quantity the speedometer shows: sqrt(vx^2+vz^2) is a 24.8 value,
         * >>8 gives the raw unit that frontend_convert_speed() turns into
         * MPH/KPH. Uses world planar velocity (not the RPM-encoded
         * longitudinal_speed, which drops to 0 when the throttle is released). */
        int32_t vx = a->linear_velocity_x;
        int32_t vz = a->linear_velocity_z;
        int64_t mag = td5_isqrt64((int64_t)vx * vx + (int64_t)vz * vz);  /* 24.8 magnitude */
        int32_t speed_raw = (int32_t)(FP_TRUNC(mag));                          /* raw display unit */
        if (speed_raw < 0) speed_raw = 0;
        if (speed_raw > m->top_speed) m->top_speed = speed_raw;
        m->speed_sum += speed_raw;
        m->sample_ticks++;

        /* --- time on air: all four wheels off the ground this tick ---
         * wheel_contact_bitmask (+0x37C): bit set = that wheel is AIRBORNE; the
         * physics engine uses ==0x0F (all four airborne) as the "fully airborne"
         * condition (see refresh exit + airborne_frame_counter gate). */
        if (a->wheel_contact_bitmask == 0x0F)
            m->air_ticks++;

        /* --- drifts: sustained lateral slip held > 0.5 s, counted once ---
         * Body-frame speeds: longitudinal_speed (forward) / lateral_speed
         * (sideways), both raw 24.8. A drift = moving forward above a floor AND
         * the sideways component is a sizeable fraction of forward (slip angle),
         * sustained for TD5_DRIFT_HOLD_TICKS. A single long slide => one drift. */
        int32_t fwd = a->longitudinal_speed; if (fwd < 0) fwd = -fwd;
        int32_t lat = a->lateral_speed;       if (lat < 0) lat = -lat;
        int drifting = (fwd >= TD5_DRIFT_MIN_FWD) && ((int64_t)lat * 4 >= fwd);
        if (drifting) {
            m->drift_run_ticks++;
            if (m->drift_run_ticks >= TD5_DRIFT_HOLD_TICKS && !m->drift_counted) {
                m->drifts++;
                m->drift_counted = 1;   /* counted this slide; don't recount */
            }
        } else {
            m->drift_run_ticks = 0;
            m->drift_counted   = 0;     /* armed for the next slide */
        }
    }
}

/* ========================================================================
 * td5_physics_run_paused_engine_step -- engine-only paused sub-tick
 * ========================================================================
 *
 * Mirrors the engine-RPM portion of UpdateVehicleActor's paused branch
 * @ 0x00406881-0x00406908 (listing-verified via Ghidra TD5_pool0 2026-05-17):
 *
 *   DL = [+0x383]                         ; race_position
 *   [+0x381] = DL                         ; prev_race_position = race_position
 *   UpdateVehicleEngineSpeedSmoothed(actor)
 *   if (g_selectedGameType != 0 && slot[+0x375].state != 1)
 *       [+0x310] = (cardef[0x72] << 1) / 3
 *   [+0x376] = 0                          ; surface_contact_flags = 0
 *   if (slot[slotIndex].state == 1) {
 *       three_quart_redline = ((int16)cardef[0x72] * 3 + sgn) >> 2
 *       if (engine_speed_accum > three_quart_redline)
 *           surface_contact_flags = (uint8)cardef[0x76]
 *   }
 *
 * NOTE: the orig paused branch ALSO calls IntegrateVehiclePoseAndContacts
 * (UNCONDITIONAL fallthrough). That call is INTENTIONALLY skipped here —
 * the port's IntegrateVehiclePoseAndContacts doesn't converge to orig's
 * zero state during stationary countdown (see td5_game.c gate comment),
 * so we run only the engine-smoothing portion. Combined with the
 * td5_game.c gate that calls td5_physics_tick() on the first paused tick
 * + last 3 paused ticks, the integrator still runs at countdown boundaries
 * but engine smoothing now runs every paused sub-tick — matching orig's
 * smooth convergence from 400 → ~1200 by sub_tick=1.
 *
 * Closes engine_speed_accum cluster: 42 fields across 7 scenarios at
 * sub_tick=1 (one per slot per scenario). */
void td5_physics_run_paused_engine_step(void)
{
    if (!g_actor_table_base) return;

    int total = td5_game_get_total_actor_count();
    if (total <= 0) return;
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;

    /* Racer slots participate in the paused engine branch (pre-rev during the
     * 3-2-1). Traffic goes through its own integration path in orig
     * (UpdateTrafficVehiclePose) and never runs UpdateVehicleEngineSpeedSmoothed.
     * [#11c 2026-06-16] Use the runtime racer/traffic boundary g_traffic_slot_base
     * (the sibling paused-metrics loop above already does), NOT a hardcoded 6 —
     * with a >6-racer field g_traffic_slot_base becomes 16, so the old 6 left
     * slots 6..N idling through the countdown (no engine build-up, "doesn't show
     * accelerating beforehand" on AI viewports 8/9). Knob TD5RE_COUNTDOWN_REV_ALL
     * (default on; "0" restores the legacy 6). */
    int rev_all = 1;
    {
        static int v = -1;
        if (v < 0) { v = td5_env_flag_on("TD5RE_COUNTDOWN_REV_ALL"); }
        rev_all = v;
    }
    int racers = total;
    int rev_cap = rev_all ? g_traffic_slot_base : 6;
    if (racers > rev_cap) racers = rev_cap;
    if (racers > TD5_MAX_RACER_SLOTS) racers = TD5_MAX_RACER_SLOTS;

    for (int slot = 0; slot < racers; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        if (!actor) continue;

        /* D5a — prev_race_position = race_position */
        actor->prev_race_position = actor->race_position;

        /* D5/0x406897 — UpdateVehicleEngineSpeedSmoothed */
        update_engine_speed_smoothed(actor);

        /* D5b — AI engine pin: gate on g_selectedGameType != 0 (championship/
         * cup modes only). [RE basis: 0x004068B3-0x004068CB] */
        if (g_game_type != 0 &&
            actor->slot_index < g_traffic_slot_base &&
            g_race_slot_state[actor->slot_index] != 1) {
            int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
            actor->engine_speed_accum = (redline << 1) / 3;
        }

        /* D5c — clear surface_contact_flags */
        actor->surface_contact_flags = 0;

        /* D5d — player path: set scf from cardef[0x76] when engine > 3/4 redline.
         * Listing 0x004068D8-690B (round-toward-zero divide by 4). */
        if (actor->slot_index < g_traffic_slot_base && g_race_slot_state[actor->slot_index] == 1) {
            int16_t *phys = get_phys(actor);
            if (phys) {
                int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
                int32_t triple = redline * 3;
                int32_t thresh = (triple + ((triple >> 31) & 3)) >> 2;
                if (actor->engine_speed_accum > thresh) {
                    actor->surface_contact_flags = ((uint8_t *)phys)[0x76];
                }
            }
        }
    }
}

/* ========================================================================
 * td5_physics_run_paused_traffic_step -- traffic-only physics on skipped
 * middle countdown sub-ticks.
 * ========================================================================
 *
 * [FIX 2026-05-26 traffic-countdown-stall] Orig runs traffic motion
 * (UpdateTrafficActorMotion) EVERY sub-tick incl. the countdown, so orig
 * traffic accelerates to speed before the race starts. The port's td5_game.c
 * countdown gate calls the full td5_physics_tick() only on the first + last 3
 * paused sub-ticks (to avoid over-integrating RACER pose on the middle ~117);
 * on those skipped middle sub-ticks only the racer engine-RPM step ran, so
 * traffic never integrated. This runs the traffic (slots 6..11) per-actor
 * physics on those skipped sub-ticks so traffic integrates every countdown
 * tick like orig. Racers are untouched here (engine step handles them). */
void td5_physics_run_paused_traffic_step(void)
{
    if (!g_actor_table_base) return;

    int total = td5_game_get_total_actor_count();
    if (total <= g_traffic_slot_base) return;   /* no traffic active */
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;

    for (int slot = g_traffic_slot_base; slot < total; ++slot) {
        TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        if (!actor) continue;
        /* td5_physics_update_vehicle_actor runs traffic friction + pose
         * regardless of g_game_paused (slot>=6 branch at step 6). */
        td5_physics_update_vehicle_actor(actor);
    }
}

/* ========================================================================
 * Master dispatcher -- UpdateVehicleActor (0x406650)
 *
 * [CONFIRMED @ 0x00406650] Steps 1-8 (frame counter, ghost reset, speed
 * tracking, race timer, attitude clamp, dynamics dispatch + paused + scripted,
 * pose integration, wall resolvers) are byte-faithful with orig per the
 * inline [Audit D1..D13] markers. L5 promotion audit 2026-05-18 confirmed
 * the dispatch ordering, mode==0/mode==1 split, and effective-grip clamp
 * match orig 0x00406650-0x00406946 line-for-line.
 *
 * Audited divergences (L5 promotion audit 2026-05-18):
 *
 *   1) [ARCH-DIVERGENCE — AccumulateVehicleSpeedBonusScore @ 0x004066EA
 *      effectively no-op] Port relocates this call into the td5_game.c
 *      sub-tick loop (accumulate_speed_bonus @ td5_game.c:3567). Function
 *      itself IS ported, but the ActorRaceMetric source fields
 *      (forward_speed/skid_factor/contact_count) are never populated from
 *      actor state, so the bonus stays at 0 in practice. Not a sim
 *      divergence; scoring-side. Documented in
 *      reference_arch_no_speed_bonus_score_2026-05-18.md.
 *
 *   2) [CONFIRMED @ 0x0040A2B0] AdvancePendingFinishState consolidated into
 *      td5_game.c game-tick loop (tick_pending_finish_timer @ td5_game.c).
 *      Core hi/lo CONCAT11 decrement + state-2 promotion logic byte-faithful
 *      with orig disasm. All 5 orig gates now mirrored: state==1, transition
 *      gate (g_cameraTransitionActive==0), replay-mode==0, finish-time==0,
 *      special_encounter!=0. The 3 minor residuals previously filed in
 *      todo_advance_pending_finish_state_residuals_2026-05-18.md were closed
 *      Phase 2 follow-up — see tick_pending_finish_timer header.
 *
 *   3) [ARCH-DIVERGENCE — port-only step 9 surface_contact_flags safety net]
 *      Port writes scf at the tail of this dispatcher when slot is the human
 *      player (g_race_slot_state == 1) and !g_game_paused. Original writes
 *      scf only inside UpdatePlayerVehicleDynamics @ 0x00404030 at a late
 *      drivetrain-commit conditional that the port doesn't fully mirror.
 *      The AI gate (`state==1 only`) is the critical guard: Frida
 *      rotation_probe.csv 2026-05-03 confirmed orig AI slot 0 scf=0 at every
 *      sim_tick 1..50, and the port matched that AFTER restricting this
 *      write to the human path. AI cars now inherit scf=0 from spawn memset
 *      (matching orig). Pure port-only redundancy on the player path; faithful
 *      on AI/traffic.
 *
 *   4) [ARCH-DIVERGENCE — traffic sub-path inlined here vs split in orig]
 *      Orig's traffic (slot >= 6) NEVER routes through UpdateVehicleActor —
 *      UpdateRaceActors @ 0x00436A70 dispatches slot>=6 separately through
 *      UpdateTrafficRoutePlan (0x00435E80) + UpdateTrafficActorMotion
 *      (0x00443ED0). The port consolidates both paths into this dispatcher,
 *      using a `slot >= 6` branch at step 7 (integrate_traffic_pose + route
 *      helpers) and a `slot < 6` guard on the wall resolvers. Mirrors orig
 *      semantics by gating each step appropriately — no observable behavioral
 *      divergence given that orig's wall-resolver path is a no-op for traffic
 *      anyway (resolve_wall_contacts has its own slot>=6 early-out at
 *      td5_track.c:536). Documented at the dispatch site (lines 1067-1099).
 *      Consolidation reduces dispatcher fan-out and keeps the per-actor
 *      tick budget in one function for profiler clarity.
 *
 * See todo_update_vehicle_actor_dispatcher_2026-05-18.md for the L5 audit log.
 * ======================================================================== */

void td5_physics_update_vehicle_actor(TD5_Actor *actor)
{
    if (!actor) return;

    /* precise-port pilot 0x00406650: capture enter snapshot. */

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
        int32_t spd_sar8 = FP_TRUNC(spd);   /* sar8_rz collapses to >>8 for spd >= 0 */
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
    /* [BIG-FIELD FIX 2026-06-25] Use the runtime racer/traffic boundary
     * g_traffic_slot_base, NOT a hardcoded 6. For a legacy <=6-racer field this
     * is byte-identical (g_traffic_slot_base == 6). For a >6-racer split-screen
     * field g_traffic_slot_base becomes 16, so slots 6..15 are RACERS — routing
     * them through the simplified traffic dynamics + surface-normal traffic pose
     * banked/rolled "player 7"-style cars onto their side (they never got the
     * per-wheel racer suspension/ground-snap). */
    if (actor->vehicle_mode == 0 && actor->slot_index >= g_traffic_slot_base) {
        /* Traffic friction (XZ + yaw + speed integration) runs EVERY sub-tick,
         * INCLUDING the countdown (g_game_paused==1). [FIX 2026-05-26 traffic-
         * countdown-stall] Orig dispatches traffic via UpdateRaceActors ->
         * UpdateTrafficActorMotion (0x00443ED0 -> IntegrateVehicleFrictionForces
         * 0x004438F0) with NO paused gate — Frida (pool8 + Moscow) shows orig
         * traffic accelerating during the countdown (lspd 0->239->478... while
         * paused=1). The port consolidated traffic into this dispatcher and
         * gated it on !g_game_paused, so port traffic stayed at REST through the
         * whole countdown, then started the race from 0 speed. With the close
         * (1-span) route target, a stationary actor's target angle is unstable,
         * so the steering controller wound up to the +/-0x18000 clamp before the
         * actor could build speed -> stuck/spinning. Running friction during the
         * countdown (like orig) lets speed + steering grow together, so traffic
         * enters the race already moving and tracks the road. NOTE the td5_game.c
         * countdown gate also runs td5_physics_run_paused_traffic_step() on the
         * skipped middle sub-ticks so traffic integrates every countdown tick. */
        td5_physics_update_traffic(actor);
    } else if (actor->vehicle_mode == 0 && !g_game_paused) {
        /* [CAR DAMAGE 2026-06-28] A knocked-out (health<=0) racer is immobilized
         * with the SAME freeze an arrested suspect gets — it can't move or be
         * driven, so the wreck stays put. Inert unless [Game] CarDamage=1. */
        int dmg_knocked_out = td5_damage_actor_knocked_out(actor);
        if (dmg_knocked_out ||
            (g_td5.wanted_mode_enabled &&
            td5_copchase_arrest_freeze_enabled() &&
            td5_game_cop_chase_is_suspect(actor->slot_index) &&
            g_wanted_damage_state[actor->slot_index] <= 0)) {
            /* [COP CHASE ARREST FREEZE 2026-06-25] Busted suspect (or [CAR DAMAGE]
             * wreck): zero ALL motion and skip the drive integrator so it can't
             * move OR be driven (human suspects included). Re-zeroed every tick so
             * a later ram can't nudge it back into motion. The td6-prop / pushable
             * passes below still run (harmless when parked). Knob
             * TD5RE_COPCHASE_ARREST_FREEZE=0 disables the arrest case. */
            actor->linear_velocity_x = 0;
            actor->linear_velocity_y = 0;
            actor->linear_velocity_z = 0;
            actor->angular_velocity_roll  = 0;
            actor->angular_velocity_pitch = 0;
            actor->angular_velocity_yaw   = 0;
        } else {
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
            /* [LANE ASSIST] apply() never runs on this path — flag the aid paused
             * so its HUD badge doesn't stay stuck "active" during a crash/launch. */
            td5_laneassist_note_inactive((int)actor->slot_index);
            td5_physics_state0f_damping(actor);
        } else if (actor->slot_index < g_traffic_slot_base && g_race_slot_state[actor->slot_index] == 1) {
            /* Human player — listing 0x0040685C tests `state == 1` strictly,
             * not `state != 0`. [Audit D13 — tightened 2026-05-14.] */
            td5_physics_update_player(actor);
        } else {
            /* AI racer (slot < 6). Traffic handled by the slot>=6 branch above. */
            td5_physics_update_ai(actor);
        }
        }
        /* [task#14] TD6 breakable-prop collision (player + AI racers), after the
         * per-tick position/velocity update so wheel contacts are current. */
        td5_physics_check_td6_props(actor);
        /* [#20 pushable] Slide/decay pushed props ONCE per sim tick (this loop runs
         * per actor; guard on the tick counter so the integrator advances once). */
        {
            static uint32_t s_last_prop_tick = 0xFFFFFFFFu;
            uint32_t t = (uint32_t)g_td5.simulation_tick_counter;
            if (t != s_last_prop_tick) { s_last_prop_tick = t; td5_track_td6_props_tick(); }
        }
    } else if (actor->vehicle_mode == 1 && !g_game_paused) {
        if (recovery_gentle_for_actor(actor)) {
            /* [GENTLE FLIP-RECOVERY 2026-06-21] Local human player: replace the
             * byte-faithful 59-frame tumble (collision_spin × saved_orientation
             * twist + flatspin/drag impulses) AND the port-only crash camera
             * shake with a ~1s gentle coast that keeps the car's existing motion
             * and levels it upright, then hands off to the SAME in-place ground-
             * snap reset (the "recovery teleport"). The original NEVER shakes the
             * screen during recovery (Ghidra-confirmed: no camera/render reader
             * of vehicle_mode/frame_counter shakes), so suppressing the shake
             * also restores faithfulness. Knob TD5RE_RECOVERY_GENTLE=0 reverts to
             * the faithful path in the else branch below. */
            td5_physics_gentle_recovery_coast(actor);
        } else {
        /* 6b. Scripted recovery mode [CONFIRMED @ 0x00406881 / 0x00409BF0 + 0x00409D20]
         * Tier 2 NOT_PORTED port (2026-05-24): replaces the prior partial
         * gravity+damping placeholder with the full byte-faithful chain:
         *   RefreshScriptedVehicleTransforms → world_pos seed from display_angles
         *   → UpdateVehicleEngineSpeedSmoothed → IntegrateScriptedVehicleMotion.
         * The 59-frame reset gate lives inside integrate_scripted_motion.
         *
         * [DEFERRED 2026-05-25 traffic-scripted-recovery-latch; orig 0x00443ED0
         *  + 0x00408082-840A heavy-impact branch in ApplyVehicleCollisionImpulse]
         *  [SUPERSEDED 2026-06-02 v2v-heavy-scatter-faithfulness] The traffic
         *  heavy-impact writer side IS now ported: td5_physics_apply_traffic_
         *  crash_spin() (called from the ApplyVehicleCollisionImpulse heavy
         *  branch for slot>=6) sets vehicle_mode=1 + frame_counter=0 and latches
         *  the spin/orientation matrices, so traffic DOES enter the
         *  vehicle_mode==1 dispatch below and visibly tumbles. The historical
         *  "traffic slots are skipped entirely" / "Future work" text that
         *  follows is kept for context but no longer describes the live code.
         *  This dispatch is slot-agnostic and would handle traffic (slot>=6)
         *  too IF the writer side were ported. It currently is NOT:
         *    - Orig 0x4079C0 heavy-impact branch (impact_mag > 90000) writes a
         *      random rotation matrix to +0xC0 (collision_spin_matrix), sets
         *      actor->vehicle_mode = 1, and zeros the +0x338 frame_counter for
         *      traffic slots (slot>=6). Port-side at td5_physics.c:3753-3777
         *      ONLY processes slot_index<6 (racers receive angular scatter +
         *      vertical bounce); traffic slots are skipped entirely.
         *    - Orig 0x00409150 ResolveVehicleContacts gates the simple-vs-full
         *      collision response on (gap_0376[3]==0 && gap_0376[6]<0xF) — i.e.
         *      scripted-mode traffic uses ResolveSimpleActorSeparation. Port
         *      lacks the scripted-mode branch entirely so this gate is moot.
         *
         *  Net effect: traffic actors in the port NEVER enter vehicle_mode==1.
         *  They continue physics integration through td5_physics_update_traffic
         *  even after a heavy hit. Behavioral cost: traffic doesn't visibly
         *  spin/tumble on heavy impact, doesn't snap-respawn after 59 frames,
         *  and doesn't escalate g_actor_traffic_recovery_stage from collisions
         *  (only from heading-misalignment in route-plan). Recovery still
         *  happens implicitly via queue recycle.
         *
         *  Future work (separate session — too risky to land alongside Tier 2):
         *  port the 0x4079C0 traffic heavy-impact branch (random spin matrix
         *  init via GetDamageRulesStub + vehicle_mode=1 + frame_counter=0),
         *  then audit integrate_scripted_motion for traffic-safety (no wheel
         *  ground-snap, no per-axle suspension calls — uses
         *  ComputeActorWorldBoundingVolume for body-probe contacts only).
         *  Reference re/analysis/subsystems/traffic-ai-system.md L213-240 and
         *  re/analysis/global_naming/batch_03_traffic_init.md for the 12-slot
         *  g_actor_traffic_recovery_stage state machine that gates route-plan
         *  during recovery. */
        td5_physics_refresh_scripted_vehicle_transforms(actor);

        /* D11 — Scripted-mode euler-accumulator RESYNC [CONFIRMED @ 0x004067A8-0x004067D8]:
         *   euler_accum.roll  (+0x1F0) = (int16)display_angles.roll  (+0x208) << 8
         *   euler_accum.yaw   (+0x1F4) = (int16)display_angles.yaw   (+0x20A) << 8
         *   euler_accum.pitch (+0x1F8) = (int16)display_angles.pitch (+0x20C) << 8
         * Raw listing: MOVSX from +0x208/0x20A/0x20C, SHL 8, MOV to +0x1F0/0x1F4/0x1F8.
         *
         * The destination is the 24.8 euler ACCUMULATOR block (+0x1F0), NOT
         * world_pos (+0x1FC). RefreshScriptedVehicleTransforms (above) just ran
         * ExtractEulerAnglesFromMatrix which recomputes the 12-bit display
         * angles (+0x208/A/C) from the rotation matrix; this block re-seeds the
         * matching 24.8 accumulators so the two angle representations stay in
         * sync for the next integration step. It does NOT touch world_pos.
         *
         * [OOB-TELEPORT FIX 2026-05-29] The prior port wrote these (display
         * angles, range ±0x7FF) into world_pos.x/y/z << 8 — teleporting the
         * recovering car to ≈world-origin (±0x7FF00 ≈ ±2096 fp) every tick =
         * "sideways car flies out of bounds". The original NEVER reads
         * +0x208/A/C as world coordinates; world_pos is integrated purely from
         * velocity in td5_physics_integrate_scripted_motion, and the 59-frame
         * gate hands off to ResetVehicleActorState (0x00405D70) which re-drops
         * the car IN PLACE (preserves world_pos.x/z, only resets .y sentinel).
         * Verified against re/include/td5_actor_struct.h _Static_asserts:
         * euler_accum=+0x1F0, world_pos=+0x1FC, display_angles=+0x208 — distinct
         * fields, no union. */
        actor->euler_accum.roll  = (int32_t)FP_SCALE(actor->display_angles.roll);
        actor->euler_accum.yaw   = (int32_t)FP_SCALE(actor->display_angles.yaw);
        actor->euler_accum.pitch = (int32_t)FP_SCALE(actor->display_angles.pitch);
        if (actor->slot_index == 0 && actor->frame_counter == 0) {
            TD5_LOG_I(LOG_TAG,
                "scripted_recovery_enter: slot=0 disp{r=%d y=%d p=%d} "
                "world_pos=(%d,%d,%d) — euler resync (no world teleport)",
                (int)actor->display_angles.roll, (int)actor->display_angles.yaw,
                (int)actor->display_angles.pitch,
                actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
        }

        update_engine_speed_smoothed(actor);
        td5_physics_integrate_scripted_motion(actor);
        }  /* end byte-faithful recovery (else branch of gentle coast) */
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
            actor->slot_index < g_traffic_slot_base &&
            g_race_slot_state[actor->slot_index] != 1) {
            int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
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
        if (actor->slot_index < g_traffic_slot_base && g_race_slot_state[actor->slot_index] == 1) {
            int16_t *phys = get_phys(actor);
            if (phys) {
                int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
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
    /* [BIG-FIELD FIX 2026-06-25] g_traffic_slot_base, not literal 6 — see the
     * dispatch note at step 6. Slots 6..15 in a >6-racer field are racers and
     * must get the faithful pose integrator below, not integrate_traffic_pose. */
    if (actor->slot_index >= g_traffic_slot_base && actor->vehicle_mode != 1) {
        /* [FIX 2026-05-28] vehicle_mode != 1 guard: a traffic vehicle in scripted
         * crash-spin recovery (vehicle_mode==1) has its motion owned by
         * td5_physics_integrate_scripted_motion in the dispatch above. Skipping
         * the normal traffic pose/route here avoids double-integrating (which
         * would overwrite the spin animation's world_pos). No-op for normal
         * traffic, which is always vehicle_mode==0. */
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
    } else if (actor->slot_index < g_traffic_slot_base) {
        /* Racer path: full gravity + per-wheel ground snap. [BIG-FIELD FIX
         * 2026-06-25] g_traffic_slot_base (not literal 6) so slots 6..15 in a
         * >6-racer field get the faithful settle-to-flat pose integrator —
         * this is what stops "player 7" sitting rolled on its side.
         * Run even during countdown (paused) so ground-snap keeps the car
         * at the correct height above the road surface.
         */
        if (actor->slot_index >= TD5_LEGACY_RACE_SLOTS) {
            /* One-shot confirmation that a >6-racer-field slot (e.g. player 7 =
             * slot 6) now runs the faithful racer pose integrator instead of
             * the traffic surface-normal pose. Fires once per session. */
            static int s_bigfield_pose_logged = 0;
            if (!s_bigfield_pose_logged) {
                s_bigfield_pose_logged = 1;
                TD5_LOG_I(LOG_TAG,
                    "bigfield racer pose: slot=%d base=%d racer-path (settle-to-flat)",
                    (int)actor->slot_index, g_traffic_slot_base);
            }
        }
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
     * guard here keeps the port faithful to the original's dispatch.
     *
     * [OOB-SLIDE FIX 2026-05-30] The gate was previously
     * `vehicle_mode == 0 && slot < 6`, which STRIPPED wall containment during
     * scripted recovery (vehicle_mode==1) — a car that rolled past sideways
     * then slid through the rail and fell out of bounds (world_pos.y -> -1e9).
     * The original UpdateVehicleActor (0x00406650) calls these three resolvers
     * in BOTH dispatch branches: the normal branch (cVar3==0) with probe table
     * 0x467384 = {0,1,2,3} (4 wheels) + callback 0x4063A0, and the recovery
     * branch (cVar3==1, ~0x004068F0-0x00406946) with probe table 0x46738c =
     * {0..7} (4 wheels + 4 body corners) + callback 0x00409CB0, then RETURNs.
     * The resolvers feed ApplyTrackSurfaceForceToActor (0x00406980) which pushes
     * world_pos.x/z back inside the rail and damps lateral velocity. So the gate
     * must be slot<6 only — recovery is contained, not exempt. [CONFIRMED @ 0x00406650]
     *
     * Fidelity note: the original recovery branch additionally tests the 4 body
     * corners (probe table 0x46738c); the port's td5_track_resolve_* iterate the
     * wheel probes, so recovery here gets wheel-probe lateral containment (stops
     * the slide) but not full body-corner containment — a deeper follow-up if
     * body-first clip-through is observed. */
    /* [BIG-FIELD FIX 2026-06-25] g_traffic_slot_base, not literal 6 — slots
     * 6..15 in a >6-racer field are racers and need wall containment too
     * (without it "player 7" would also clip through the rails). Byte-identical
     * for legacy <=6-racer fields where g_traffic_slot_base == 6. */
    if (actor->slot_index < g_traffic_slot_base) {
        td5_track_resolve_reverse_contacts(actor);
        td5_track_resolve_forward_contacts(actor);
        td5_track_resolve_wall_contacts(actor);
        if (actor->slot_index == 0 && actor->vehicle_mode == 1 &&
            actor->frame_counter == 0) {
            TD5_LOG_I(LOG_TAG,
                "recovery_wall_contain: slot=0 vmode=1 resolvers now run "
                "world_pos=(%d,%d,%d)",
                actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
        }
    }

    /* 9. surface_contact_flags update — REMOVED 2026-05-23.
     *
     * The original NEVER derives scf from wheel-ground contact. scf is a
     * WHEELSPIN LATCH set/cleared inside UpdatePlayerVehicleDynamics
     * (0x00404030) by two late conditionals:
     *
     *   CLEAR (every tick): if abs(long_spd - body_v_long) >> 8 < 0x41 → 0
     *                       (engine-derived speed matches body speed → grip)
     *   SET   (FIRST gear only): when engine spins much faster than body
     *                            AND throttle > 0x7f → scf = cardef[0x76]
     *                            (1=RWD, 2=FWD, 3=AWD wheelspin)
     *
     * scf != 0 routes the next tick to CRGT (engine governs ground speed —
     * the launch/burnout model). scf == 0 routes to UESA (gear-ratio RPM
     * tracking) which is what 99% of normal driving uses.
     *
     * The prior wheel-contact-bitmask write here clobbered scf to 3 every
     * tick on flat ground, forcing every tick into the CRGT branch where
     * (a) UpdateAutomaticGearSelection never runs (orig doesn't call it on
     * that branch) and (b) CRGT slews engine_speed_accum to 0 in any gear
     * other than FIRST. Net effect: gear shifts stalled, RPM collapsed to
     * minimum after upshift, front grip math desynced, drift sound fired
     * on tiny inputs. Per the deep-dive Ghidra audit 2026-05-23.
     *
     * Phase 1 of the fix: leave scf at whatever set/clear sites it was
     * legitimately written by (initially 0 from ResetVehicleActorState
     * at 0x00405DC6). Phase 2 will port the orig's SET/CLEAR conditionals
     * into td5_physics_update_player. */

    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "Vehicle actor0: speed=%d rpm=%d gear=%d surface=%u",
                  actor->longitudinal_speed,
                  actor->engine_speed_accum,
                  actor->current_gear,
                  actor->surface_type_chassis);
    }

    /* precise-port pilot 0x00406650: capture leave snapshot. */
}

/* ========================================================================
 * Player 4-wheel dynamics -- UpdatePlayerVehicleDynamics (0x404030)
 * ========================================================================
 *
 * [CONFIRMED @ 0x00404030] Byte-faithful with orig UpdatePlayerVehicleDynamics.
 * SAR-RZ rounding fixed 2026-05-18 (L5 promotion follow-up).
 *
 * Static port audited byte-for-byte against listing 0x00404030..0x00404eb7
 * (3719 bytes / 870 instructions). The body has 15+ inline [CONFIRMED]
 * citations anchoring per-block addresses (see comments below). Overall
 * structure matches the original block layout: surface probes → drag damp
 * → body-frame trig → load transfer → drivetrain dispatch → slip-circle.
 *
 * SHIPPED 2026-05-18 — D1/D2/D5/D10/D14 SAR-RZ-class LSB sites:
 *   D1 — handbrake rear-grip mul @ 0x0040434C..0x0040436F (SAR-RZ-8).
 *   D2 — velocity drag @ 0x004040E5..0x00404137 (SAR-RZ-8 then SAR-RZ-12).
 *   D5 — per-wheel grip-from-load @ 0x00404253..0x004042B7 (SAR-RZ-8;
 *        replaced the `+128 >> 8` half-to-even bias with SAR-RZ).
 *   D10 — front_long / rear_long axle sum @ 0x004046DC..0x00404734 (SAR-RZ-8
 *         per wheel-grip product).
 *   D14 — front-slip yaw damping correction @ 0x00404DB6..0x00404DD5
 *         (SAR-RZ-6 then SAR-RZ-15).
 *
 * RESIDUAL KNOWN DIVERGENCES (documented in re/analysis/pilot_00404030_audit.md):
 *   D4   CLOSED 2026-05-24 — per-wheel GetTrackSegmentSurfaceType restored
 *        via td5_track_get_surface_type(actor, 0..3) reading body_probes
 *        (orig 0x00404030 byte-faithful for the 4 wheel calls). Chassis
 *        cache write remains via refresh_wheel_contact_frames (port-side
 *        deviation from orig's +0x80 chassis probe; out of scope here).
 *   D3,D11,D15  audited MATCH — no divergence.
 *
 * Most known wheel_load_accum / lateral_bias divergences are UPSTREAM
 * (carparam binding for PlayerIsAI=1 — see memory/
 * todo_playerisai_carparam_binding.md, SHIPPED 48d320a).
 *
 * Audit reference: re/analysis/pilot_00404030_audit.md (2026-05-14, pool0).
 */

/* Pilot trace hooks (pool0 / 0x00404030) */

/* --- Phases 1-4 of td5_physics_update_player: surface probes, surface/gravity,
 * per-wheel grip, handbrake modifier. Extracted verbatim (S3 code-motion split
 * -- see REFACTOR_PLAN.md) so the caller's variable scope shrinks; behavior is
 * byte-identical, only the call boundary changed. */
static void td5_physics_player_phase_grip(TD5_Actor *actor,
                                           int32_t grip[4],
                                           uint8_t *out_surface_center,
                                           uint8_t out_surface_wheel[4],
                                           int32_t *out_front_weight,
                                           int32_t *out_rear_weight)
{
    int32_t i;

    /* --- 1. Surface type probes (5: chassis + 4 wheels) ---
     *
     * [FIX 2026-05-24 D4-audit-residual: per-wheel surface probe restored;
     *  orig 0x00404030] Original calls GetTrackSegmentSurfaceType @ 0x0042F100
     *  five times: chassis (from +0x80 track_span block) + body_probes[0..3]
     *  at +0x00/+0x10/+0x20/+0x30 (FL/FR/RL/RR corners). Mixed-surface
     *  segments (tarmac/grass transitions, half-on/half-off) now give each
     *  wheel its own surface friction / grip coefficient.
     *
     *  Port helper `td5_track_get_surface_type(actor, probe_index)` maps
     *  probe_index 0..3 -> body_probes (orig's per-wheel corner probes),
     *  4..7 -> wheel_probes. Orig uses body probes, so we pass 0..3.
     *
     *  Chassis surface (`surface_center`) remains the cached value written
     *  upstream by refresh_wheel_contact_frames (orig 0x00403720). That
     *  cache currently picks the first valid wheel_probes[i] (port-side
     *  divergence from orig's chassis-block probe at +0x80) — left as-is
     *  since the body_probes-based per-wheel pass is the in-scope D4 fix. */
    uint8_t surface_center = actor->surface_type_chassis;
    uint8_t surface_wheel[4];
    for (i = 0; i < 4; i++)
        surface_wheel[i] = (uint8_t)td5_track_get_surface_type(actor, i);

    /* --- 2. Surface normal and gravity --- */
    td5_physics_compute_surface_gravity(actor);

    /* --- 3. Per-wheel grip from surface tables, clamped [0x38..0x50] --- */
    int32_t front_weight = (int32_t)PHYS_S(actor, PHYS_FRONT_WEIGHT);
    int32_t rear_weight  = (int32_t)PHYS_S(actor, PHYS_REAR_WEIGHT);
    int32_t total_weight = front_weight + rear_weight;
    if (total_weight == 0) total_weight = 1;

    int32_t half_wb = PHYS_I(actor, PHYS_HALF_WHEELBASE);
    int32_t full_wb = half_wb * 2;
    if (full_wb == 0) full_wb = 1;

    /* Suspension deflection -> load transfer */
    int32_t susp_defl = actor->center_suspension_pos;

    /* Front/rear load fraction (8.8 fixed) — weight transfer via suspension
     * deflection. Original @ 0x004041AE uses center_suspension_pos directly
     * (no shift). Prior port divided by 16, which near-zeroed load transfer
     * and made grip front/rear symmetric under weight shift, killing the
     * asymmetry that drives oversteer/understeer during cornering. [CONFIRMED]
     *
     * [FIX 2026-05-26] Weight terms were SWAPPED vs orig 0x00404030. The
     * original feeds tuning[0x2a] (rear_weight) into the FRONT-axle grip load
     * and tuning[0x28] (front_weight) into the REAR-axle grip load — bicycle-
     * model static axle load is proportional to the OPPOSITE-axle CG term.
     * The port previously fed front_weight into front_load, so on front-heavy
     * cars (Wf>Wr, true for nearly every player car — e.g. 414/397, gto 490/410)
     * the REAR axle got too little grip and slid at low provocation → premature
     * oversteer/drift. AI cars were immune because their template has
     * Wf==Wr==400, which is why AI regression sweeps never caught it. */
    int32_t front_load = ((FP_SCALE(rear_weight)) / total_weight);
    front_load = front_load * (half_wb - susp_defl) / full_wb;
    int32_t rear_load = ((FP_SCALE(front_weight)) / total_weight);
    rear_load = rear_load * (half_wb + susp_defl) / full_wb;

    for (i = 0; i < 4; i++) {
        int32_t sf = phys_surface_grip(surface_wheel[i]);   /* [task#15] TD6-aware */
        int32_t load = (i < 2) ? front_load : rear_load;
        /* D5 — SAR-RZ-8 per axle grip [CONFIRMED @ 0x00404253-0x004042B7].
         * Original idiom: IMUL grip_table,load; CDQ; AND EDX,0xFF; ADD; SAR 8.
         * Prior `(sf*load + 128) >> 8` was round-half-to-even via +128 bias;
         * port now matches x86 round-toward-zero semantics. */
        grip[i] = SAR_RZ_8(sf * load);
        if (grip[i] < TD5_PLAYER_GRIP_MIN) grip[i] = TD5_PLAYER_GRIP_MIN;
        if (grip[i] > TD5_PLAYER_GRIP_MAX) grip[i] = TD5_PLAYER_GRIP_MAX;
        grip[i] = phys_td6_grass_slide(grip[i], surface_wheel[i]);  /* [#15] verge slide */
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
        int32_t hb_mod = (int32_t)PHYS_S(actor, PHYS_HANDBRAKE_MOD);
        /* [FIX 2026-06-02 power-donut] On a RWD car the faithful ~25% rear-grip
         * cut leaves too much LATERAL grip to oversteer UNDER power, so
         * throttle+handbrake just drove forward (handbrake "negated"). When the
         * player is ALSO on throttle (encounter_steering_cmd > 0 thanks to the
         * power-slide input deviation), cut the rear grip ~2x harder so the rear
         * breaks loose and the car donuts. Handbrake-only (throttle=-256, i.e.
         * <=0 here) keeps the faithful cut. Tunable: >>1 = strong, >>2 = looser. */
        if (actor->encounter_steering_cmd > 0) {
            hb_mod >>= 1;
        }
        int32_t g2_pre = grip[2], g3_pre = grip[3];
        /* D1 — SAR-RZ-8 [CONFIRMED @ 0x0040434C-0x0040436F].
         * Original idiom: IMUL grip,hb_mod; CDQ; AND EDX,0xFF; ADD; SAR 8. */
        grip[2] = SAR_RZ_8(grip[2] * hb_mod);
        grip[3] = SAR_RZ_8(grip[3] * hb_mod);
        if (actor->slot_index == 0 && (actor->frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "HBRAKE: hb_mod=%d grip_rl=%d->%d grip_rr=%d->%d",
                      hb_mod, g2_pre, grip[2], g3_pre, grip[3]);
        }
    }

    *out_surface_center = surface_center;
    out_surface_wheel[0] = surface_wheel[0];
    out_surface_wheel[1] = surface_wheel[1];
    out_surface_wheel[2] = surface_wheel[2];
    out_surface_wheel[3] = surface_wheel[3];
    *out_front_weight = front_weight;
    *out_rear_weight = rear_weight;
}

void td5_physics_update_player(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;

    /* --- Phases 1-4 (surface probes, surface/gravity, grip, handbrake) --- */
    int32_t grip[4];
    uint8_t surface_center, surface_wheel[4];
    int32_t front_weight, rear_weight;
    td5_physics_player_phase_grip(actor, grip, &surface_center, surface_wheel,
                                   &front_weight, &rear_weight);

    /* --- 5. Velocity drag in WORLD frame (confirmed by Ghidra at 0x40409x) ---
     * Original applies drag to linear_velocity_x/z BEFORE body decomposition.
     * 0x6A = driving drag (low value ~100), 0x6C = coasting drag (high ~3000).
     * Formula: v -= ((v >> 8) * drag_coeff) >> 12
     * Condition: throttle < 0x20 || gear < 2 → use 0x6C (coast), else 0x6A (drive) */
    {
        int32_t surf_drag = phys_surface_drag(surface_center);   /* [task#15] TD6-aware */
        int32_t damp_coeff;
        if (actor->encounter_steering_cmd < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, PHYS_DAMP_COEFF_BASE);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, PHYS_DAMP_COEFF_TURN);

        /* D2 — SAR-RZ-8 then SAR-RZ-12 [CONFIRMED @ 0x004040E5-0x00404104,
         * 0x0040410D-0x00404137].
         * Original idiom: CDQ; AND EDX,0xFF; ADD EAX,EDX; SAR EAX,8 (vx_h8)
         *               ; IMUL EAX,damp; CDQ; AND EDX,0xFFF; ADD; SAR EAX,0xC.
         * 32-bit IMUL wrap behaviour preserved (same as C int32 multiply). */
        actor->linear_velocity_x -= SAR_RZ_12(SAR_RZ_8(actor->linear_velocity_x) * damp_coeff);
        actor->linear_velocity_z -= SAR_RZ_12(SAR_RZ_8(actor->linear_velocity_z) * damp_coeff);
    }

    /* --- 6. Resolve body-frame velocities (cos/sin of heading) --- */
    int32_t heading = FP_ANGLE(actor->euler_accum.yaw);
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t vx = actor->linear_velocity_x;
    int32_t vz = actor->linear_velocity_z;

    /* Longitudinal = dot(velocity, heading_forward) */
    int32_t v_long = (vx * sin_h + vz * cos_h) >> 12;
    /* Lateral = dot(velocity, heading_right) */
    int32_t v_lat  = (vx * cos_h - vz * sin_h) >> 12;

    /* --- 6a. Steering-angle cos/sin (HOISTED 2026-05-25 for scf-wheelspin
     * ordering fix). These were previously computed at the top of section 12
     * (per-axle forces) but are needed earlier by section 14a (long/lat speed
     * writeback) which is itself being moved UP to before the section 7-11
     * dispatch so that auto_gear / slip-circle / current_slip_metric all see
     * this-tick's freshly-written long_speed/lat_speed instead of stale
     * previous-tick values. See moved-block comment below for rationale. */
    int32_t steer_angle = -(FP_TRUNC(actor->steering_command));
    /* Original input maps LEFT=positive, RIGHT=negative (bit 0x02=LEFT feeds
     * the add-to-cmd path). Port input maps LEFT=negative, RIGHT=positive.
     * Negate here to match the original convention the physics was built
     * around. [CONFIRMED: original bit layout documented in td5_input.c:484]
     *
     * Original (0x40415B): steer_angle = steering_command >> 8, no scaling.
     * Constant 294 does NOT exist in the binary. [CONFIRMED @ 0x404142-0x40415E] */
    /* [CAR DAMAGE 2026-06-28] A damaged car steers sluggishly: scale steering
     * authority down with health (racer slots only; never traffic). 1.0 = no
     * change (off / pristine), floor at 1 - TD5RE_DAMAGE_PENALTY%. Reducing
     * authority is sim-stable (it can only understeer, never induce a spin). */
    if (td5_damage_enabled() && actor->slot_index >= 0 &&
        actor->slot_index < g_traffic_slot_base) {
        float hs = td5_damage_handling_scale((int)actor->slot_index);
        if (hs < 0.999f)
            steer_angle = (int32_t)((float)steer_angle * hs);
    }
    int32_t steer_heading = (heading + steer_angle) & 0xFFF;
    int32_t cos_s = cos_fixed12(steer_heading);   /* cos(h+s) — iVar16 */
    int32_t sin_s = sin_fixed12(steer_heading);   /* sin(h+s) — iVar17 */

    /* Steer-angle-only cos/sin for the lateral force solve.
     * Decomp uses iVar18 = cos(steer), iVar19 = sin(steer) — NOT (h+s). */
    int32_t steer_only = steer_angle & 0xFFF;
    int32_t cos_sr = cos_fixed12(steer_only);     /* iVar18 = cos(s) */
    int32_t sin_sr = sin_fixed12(steer_only);     /* iVar19 = sin(s) */

    /* --- 14a. [HOISTED 2026-05-25 from former tail location ~line 2122-2187 —
     * scf-wheelspin-ordering fix per todo_scf_wheelspin_ordering.md.]
     *
     * Orig UpdatePlayerVehicleDynamics @ 0x00404030 writes
     * actor->longitudinal_speed and actor->lateral_speed in the dispatch
     * block BEFORE the auto-gear / drive-torque / slip-circle stages, so the
     * downstream consumers (UpdateAutomaticGearSelection @ 0x0042EF10 uses
     * `0 < actor->longitudinal_speed` as its upshift gate; slip-circle pass-A
     * reads long_speed/lat_speed to compute slip delta; current_slip_metric
     * @ 0x004049BA/0x00404A80 reads the same) all see THIS tick's values.
     *
     * Prior port placed this block at the function tail (after force writeback
     * and yaw-torque). Result: at full-throttle launch, auto_gear read the
     * PREVIOUS tick's long_speed (could be >0 from countdown / prior gear),
     * so the upshift gate fired at rpm 5400 (Viper gear-2 threshold from
     * tuning+0x42), gear shifted to 3 before engine reached rpm ≈ 7295 that
     * the wheelspin SET gate at 0x00404E51 needs → actor->surface_contact_flags
     * (+0x376) stayed 0 → CRGT branch + UpdateTireTrackEmitters + wheel-sound
     * smoke all early-returned. User-visible symptoms (2026-05-24 Newcastle):
     * no initial-drift wheelspin at launch, no tire marks at spawn, smoke
     * particle count diverged from orig.
     *
     * Orig verification (Ghidra UpdatePlayerVehicleDynamics @ 0x00404030):
     *   if (scf != 0) {
     *     if (sVar2==1) { lateral_speed=uVar31; longitudinal_speed=CRGT(uVar12); }
     *     elif (sVar2==2) { lateral_speed=CRGT(uVar31); longitudinal_speed=uVar12; }
     *     elif (sVar2==3) { both=CRGT(...); }
     *     // then drive/brake dispatch — NO auto_gear on this branch
     *   } else {
     *     lateral_speed=uVar31; longitudinal_speed=uVar12;
     *     // then if (!brake && throttle) { auto_gear(); UESA(); drive... }
     *   }
     *
     * The slip-circle pass A reads (lines below in section 13) use these
     * freshly-written values. The current_slip_metric write (section 13a)
     * likewise. All three are the orig-faithful pattern; the prior port's
     * tail placement was the divergence.
     *
     * NOTE: CRGT here uses last-tick's engine_speed_accum (UESA runs AFTER
     * the dispatch on airborne). This matches orig: orig's on-ground branch
     * also runs CRGT with last-tick engine_speed_accum (no UESA on on-ground).
     *
     * See sections 13 (slip-circle pass A) and 13a (current_slip_metric)
     * below for the readers of these writes. */
    {
        /* Pre-force velocity (vx/vz captured at line ~1397, post-drag). */
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
            int32_t dt_layout = (int32_t)PHYS_S(actor, PHYS_DRIVETRAIN_TYPE);
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
    int32_t brake_front = (int32_t)PHYS_S(actor, PHYS_BRAKE_FRONT);
    int32_t brake_rear  = (int32_t)PHYS_S(actor, PHYS_BRAKE_REAR);

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
            /* Manual gearbox (+0x378==0): reverse-throttle-sign only — faithful
             * original on-ground behaviour (no auto-shift on ground regardless). */
            if (td5_physics_actor_is_manual_gearbox(actor)) {
                td5_physics_reverse_throttle_sign(actor);
            }
            /* No auto_gear_select call here — orig's on-ground branch
             * (scf != 0) does NOT call UpdateAutomaticGearSelection at all
             * [CONFIRMED @ 0x00404030 Ghidra decomp 2026-05-23]. Gears and
             * RPM only update on the airborne branch (scf == 0 → UESA path).
             * Previous on-ground compensatory call + its 30-tick debounce
             * wrapper were a port-only patch over the scf miscomputation
             * that is being removed in the same change. */
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

            /* [HILL ASSIST 2026-06-16] Boost drive torque on uphill grades so a
             * steep climb (Scotland ~45deg, mid-power car) doesn't stall mid-
             * cliff. Uphill => the surface normal's forward projection along the
             * heading is negative; use its magnitude as the steepness. Downhill
             * and flat (fwd_dot>=0) get no boost. drive_torque is 0 at idle
             * throttle so coasting uphill is unaffected (assist helps only when
             * the player is actually trying to climb). */
            {
                int32_t nx_h = (int32_t)actor->heading_normal.x;
                int32_t nz_h = (int32_t)actor->heading_normal.z;
                int32_t fwd_dot_h = (nx_h * sin_h + nz_h * cos_h) >> 12;  /* <0 uphill */
                if (fwd_dot_h < 0) {
                    int32_t ha_q12 = td5_physics_hill_assist_q12(-fwd_dot_h);
                    if (ha_q12 != 4096)
                        drive_torque = (int32_t)(((int64_t)drive_torque * ha_q12) >> 12);
                }
            }

            int32_t dt_type = (int32_t)PHYS_S(actor, PHYS_DRIVETRAIN_TYPE);
            int32_t speed_limit = FP_SCALE(phys_top_speed_rating(actor));
            /* [MANUAL BOOST #2] Top-speed half: a manual-gearbox car (byte
             * +0x378 == 0) tops out +N% higher. 1.0 (no change) for automatic
             * or knob-off → byte-faithful limit. */
            speed_limit = td5_physics_apply_speed_limit_boost(
                              speed_limit, td5_physics_actor_manual_boost_q8(actor));
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
            int32_t bf = FP_TRUNC((brake_front * throttle));
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
                int32_t steer_angle = -(FP_TRUNC(actor->steering_command));
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
                    int32_t yaw_corr = (sin_sr * (int32_t)PHYS_S(actor, PHYS_FRONT_WEIGHT) * actor->angular_velocity_yaw) >> 12;
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
                /* Suppress this port-only reverse hand-off while the handbrake
                 * is held. The original on-ground brake path NEVER writes
                 * current_gear=REVERSE [CONFIRMED @ 0x004043f0-0x00404452]; on
                 * flat ground a held handbrake decelerates to ~0 and the
                 * near-zero-velocity clamp holds the car at rest — it does not
                 * start reversing. This hand-off exists only to emulate the
                 * brake BUTTON's reverse transition (orig achieves it via
                 * airborne-microbump auto-gear, too sticky to reproduce on
                 * ground), so it must not fire for the handbrake. */
                if (abs_vlong < 0x100 && !actor->handbrake_flag &&
                    !(wreck_immobile_enabled() && actor->slot_index >= 0 &&
                      td5_ai_actor_is_broken_down((int)actor->slot_index))) {
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
             * field_0x378 == 0 → manual, != 0 → automatic.
             * [#2 2026-06-15] Route through the canonical should-auto-shift
             * predicate so a MANUAL car (+0x378==0) never auto-shifts here — the
             * player drives the gears via the gear-up/down inputs, which write
             * actor[0x36B] in td5_input.c. Auto cars (AI/traffic + auto-mode
             * player) take the auto_gear FSM. TD5RE_MANUAL_GEARBOX=0 forces the
             * old always-auto fallback. (When manual, mirror the original's
             * reverse-throttle-sign step so REVERSE gear still drives backward.) */
            if (td5_physics_actor_should_auto_shift(actor)) {
                td5_physics_auto_gear_select(actor);
            } else {
                td5_physics_reverse_throttle_sign(actor);
            }
            td5_physics_update_engine_speed(actor);

            /* Drive torque while airborne [CONFIRMED @ 0x404560-0x4045AE] */
            drive_torque = td5_physics_compute_drive_torque(actor);
            int32_t dt_type = (int32_t)PHYS_S(actor, PHYS_DRIVETRAIN_TYPE);
            int32_t speed_limit = FP_SCALE(phys_top_speed_rating(actor));
            /* [MANUAL BOOST #2] Top-speed half: +N% limit in manual gearbox. */
            speed_limit = td5_physics_apply_speed_limit_boost(
                              speed_limit, td5_physics_actor_manual_boost_q8(actor));
            int32_t abs_speed = v_long < 0 ? -v_long : v_long;
            if (abs_speed <= speed_limit) {
                switch (dt_type) {
                case 1: /* RWD */
                    /* Original airborne RWD writes BOTH rear wheel slots to
                     * drive_torque/2 — same as the on-ground RWD branch above.
                     *
                     * RE'd direct from orig UpdatePlayerVehicleDynamics
                     * @ 0x00404030 decomp 2026-05-23: in the !brake, throttle!=0,
                     * sVar2==1 block, the orig writes
                     *   local_8  = drive_torque / 2;  // → wheel_drive[2] (RL)
                     *   pRVar24  = local_8;           // → wheel_drive[3] (RR)
                     * The prior port (and the inline comment that claimed a
                     * "single MOV in RWD airborne path") missed the second
                     * assignment, so wheel_drive[3] stayed 0. Symptoms after
                     * Phase 1 of the scf rewrite (which made the airborne
                     * branch the dominant path):
                     *   - rear_long was HALVED (one rear contributed zero)
                     *     → accel from 0 felt sluggish.
                     *   - yaw term2 = (RR - RL - FL + FR)*500 = (0 - X - 0 + 0)*500
                     *     ran at -120k to -240k every tick → constant
                     *     self-steer to one side.
                     *   - asymmetric drive desynced the slip-circle math
                     *     → drift sound on tiny steering input.
                     * Writing both rear slots brings the airborne RWD path
                     * into line with the on-ground RWD branch and the orig. */
                    wheel_drive[2] = drive_torque >> 1;
                    wheel_drive[3] = drive_torque >> 1;
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
            int32_t bf = FP_TRUNC((brake_front * coast_throttle));
            int32_t br = FP_TRUNC((brake_rear  * coast_throttle));
            {
                int32_t abs_vl = v_long < 0 ? -v_long : v_long;
                int32_t abs_bf = bf < 0 ? -bf : bf;
                int32_t abs_br = br < 0 ? -br : br;
                int32_t cf = abs_bf < abs_vl ? abs_bf : abs_vl;
                int32_t cr = abs_br < abs_vl ? abs_br : abs_vl;
                /* [FIX 2026-06-02 handbrake-reverses-on-cobbles] The brake/coast
                 * force must OPPOSE travel (decelerate), not keep bf's throttle-
                 * derived sign. With the handbrake held throttle=-256 makes bf
                 * negative; on Moscow's cobblestone the car is intermittently
                 * AIRBORNE so THIS path runs, and the old sign-preserving clamp kept
                 * pushing BACKWARD even once the car was already moving backward ->
                 * runaway reverse. The on-ground brake path already opposes velocity
                 * (~0x404441); the comment above claimed "same fix as on-ground" but
                 * the code only clamped magnitude, not sign. Oppose v_long. Forward
                 * driving is unaffected (the sign only flips when v_long < 0). */
                bf = (v_long > 0) ? -(int32_t)cf : (int32_t)cf;
                br = (v_long > 0) ? -(int32_t)cr : (int32_t)cr;
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

    /* --- 12. Per-axle lateral/longitudinal forces ---
     * NOTE 2026-05-25: `steer_angle`, `steer_heading`, `cos_s`, `sin_s`,
     * `steer_only`, `cos_sr`, `sin_sr` were HOISTED to section 6a (above the
     * section 7-11 dispatch) so that the section 14a speed writeback — also
     * moved up — can compute body_vlat using sin_s/cos_s/sin_sr. See section
     * 6a header for the scf-wheelspin-ordering rationale. */

    /* Front/rear longitudinal forces scaled by RAW surface friction coefficient.
     * [CONFIRMED @ 0x004046DC]: original re-reads gSurfaceGripCoefficientTable
     * (DAT_004748C0) directly — it does NOT use the load-weighted/clamped grip[].
     * The grip[] array is for slip-circle limiting only.
     * Formula: sf[i] * wheel_drive[i] >> 8, where sf = raw table value (e.g. 256 for asphalt).
     * Prior port used grip[i] (56-80 after load+clamp) which is ~2x too small. */
    int32_t sf_fl = phys_surface_grip(surface_wheel[0]);   /* [task#15] TD6-aware */
    int32_t sf_fr = phys_surface_grip(surface_wheel[1]);
    int32_t sf_rl = phys_surface_grip(surface_wheel[2]);
    int32_t sf_rr = phys_surface_grip(surface_wheel[3]);
    /* D10 — SAR-RZ-8 per wheel-grip product [CONFIRMED @ 0x004046DC-0x00404734].
     * Original idiom: IMUL grip,drive; CDQ; AND EDX,0xFF; ADD; SAR EAX,8.
     * Plain `>> 8` rounds toward -inf for negative products (rolling reverse). */
    int32_t front_long = SAR_RZ_8(sf_fl * wheel_drive[0]) + SAR_RZ_8(sf_fr * wheel_drive[1]);
    int32_t rear_long  = SAR_RZ_8(sf_rl * wheel_drive[2]) + SAR_RZ_8(sf_rr * wheel_drive[3]);

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
        int32_t I_ = PHYS_I(actor, PHYS_INERTIA_YAW);                   /* iVar27 */
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
    int32_t tire_grip_coeff = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
    int32_t slip_coupling   = (int32_t)PHYS_S(actor, PHYS_SLIP_COUPLING);
    {
        /* ---- Longitudinal slip-circle -> REAR axle ----
         * [FIX 2026-05-28 tire-marks] Orig +0x31C store @0x404aed: scf&1 +
         * LONGITUDINAL slip clamps the REAR (driven) axle force pair against
         * the REAR wheel-grip pair; the slip-excess remainder is stored at
         * +0x31C (consumed by the rear wheel-sound skid-mark path). The port
         * previously fed FRONT operands here, so a RWD launch (rear wheelspin)
         * never saturated -> +0x31C stayed 0 -> no skid marks. Confirmed vs
         * Frida orig: +0x31C ~480 at launch. Local name kept _f (now rear). */
        int32_t grip_limit_f = (grip[2] + grip[3]);
        if (tire_grip_coeff != 0)
            grip_limit_f = FP_TRUNC((grip_limit_f * tire_grip_coeff));

        /* Pass A — runs whenever scf & 1.
         *
         * Mirrors orig 0x00404030 pass A which:
         *   (a) computes slip = |actor->long_speed - max(0, body_vlong)|
         *   (b) clears scf when slip>>8 < 0x41 (wheelspin recovered)
         *   (c) UNCONDITIONALLY applies the slip-coupling math: reduces
         *       grip_limit_f and modulates front_long by `coupled =
         *       slip*coupling/256` divided by hyp = sqrt(lat²/256² + coupled²).
         *
         * Earlier port wrapped (c) in `slip_shift >= 0x41` as a "defensive
         * guard against zero-drive at rest / grid / countdown" — but that
         * guard is no longer needed: after the scf source rewrite (Phase 1)
         * and the wheelspin SET fix (Phase 2), scf is only ever non-zero
         * during legitimate wheelspin where slip is large by definition.
         * The `if (slip_shift < 0x41) scf=0` CLEAR below fires before any
         * subsequent tick can enter pass-A with vanishing slip, so the math
         * always runs on meaningful values. Restoring orig's unconditional
         * form makes the slip-circle behavior match in the brief wheelspin
         * transition ticks. 2026-05-23 follow-up to scf rewrite. */
        if ((actor->surface_contact_flags & 1) && slip_coupling != 0) {
            int32_t body_vlong = (vx * sin_h + vz * cos_h) >> 12;
            int32_t pos_vlong  = (body_vlong < 0) ? 0 : body_vlong;
            int32_t delta      = actor->longitudinal_speed - pos_vlong;
            int32_t delta_abs  = (delta < 0) ? -delta : delta;
            int32_t slip_shift = FP_TRUNC(delta_abs);
            /* Wheelspin CLEAR [CONFIRMED @ 0x00404030] — engine and ground
             * speed have re-synced; drop scf so next tick uses UESA. */
            if (slip_shift < 0x41) {
                actor->surface_contact_flags = 0;
            }
            int32_t coupled = FP_TRUNC((slip_shift * slip_coupling));
            int32_t lat     = rear_lat_force;       /* REAR lateral (orig +0x31C) */
            int32_t latSh   = FP_TRUNC(lat);
            int32_t latMix  = FP_TRUNC((latSh * lat));
            int32_t hyp_sq  = latMix + coupled * coupled;
            if (hyp_sq < 0) hyp_sq = 0;
            int32_t hyp     = td5_isqrt(hyp_sq) + 1;
            int32_t latSh_a = (latSh < 0) ? -latSh : latSh;
            grip_limit_f    = (latSh_a * grip_limit_f) / hyp;
            rear_long       = ((rear_long / 2) * coupled) / hyp;   /* REAR long */
        }

        /* Pass B — classical slip-circle clamp (REAR axle force pair) */
        int32_t fl16 = rear_lat_force >> 4;
        int32_t flo16 = rear_long >> 4;
        int32_t combined_sq = fl16 * fl16 + flo16 * flo16;
        int32_t combined = td5_isqrt(combined_sq) << 4;
        if (combined > grip_limit_f && combined > 0) {
            actor->front_axle_slip_excess = combined - grip_limit_f;  /* +0x31C */
            /* [FIX 2026-05-28] Orig Pass-B (@0x00404ad1-0x00404ae4) rescales
             * ONLY the lateral force; the longitudinal/drive force is left
             * untouched. The port previously also clamped *_long, which —
             * once this block clamps the DRIVEN (rear) axle — collapsed the
             * drive force to ~0 during straight launch wheelspin (limit→0
             * when lateral≈0) and froze the car. Drop the _long rescale. */
            rear_lat_force = FP_TRUNC(((FP_SCALE(grip_limit_f)) / combined * rear_lat_force));
        } else {
            actor->front_axle_slip_excess = 0;
        }

        /* ---- Lateral slip-circle -> FRONT axle ----
         * [FIX 2026-05-28 tire-marks] Orig +0x320 store @0x404c3d: scf&2 +
         * LATERAL slip clamps the FRONT axle force pair against the FRONT
         * wheel-grip pair; slip-excess stored at +0x320 (front skid path).
         * Operands corrected REAR->FRONT to match orig. Local kept _r. */
        int32_t grip_limit_r = (grip[0] + grip[1]);
        if (tire_grip_coeff != 0)
            grip_limit_r = FP_TRUNC((grip_limit_r * tire_grip_coeff));

        /* Pass A (rear) — mirrors orig 0x00404030 rear pass A.
         * Same shape as front pass A above; see that block for the rationale
         * on running unconditionally once `(scf & 2)` is true. */
        if ((actor->surface_contact_flags & 2) && slip_coupling != 0) {
            int32_t raw_front = (int32_t)(((int64_t)sin_s * vx + (int64_t)cos_s * vz) >> 12);
            int64_t yaw_q12   = (int64_t)sin_sr * front_weight * actor->angular_velocity_yaw;
            int32_t yaw_term  = (int32_t)((yaw_q12 >> 12) / 0x28c);
            int32_t axle_vlat = raw_front - yaw_term;
            int32_t pos_vlat  = (axle_vlat < 0) ? 0 : axle_vlat;
            int32_t delta     = actor->lateral_speed - pos_vlat;
            int32_t delta_abs = (delta < 0) ? -delta : delta;
            int32_t slip_shift = FP_TRUNC(delta_abs);
            if (slip_shift < 0x41) {
                actor->surface_contact_flags = 0;
            }
            int32_t coupled = FP_TRUNC((slip_shift * slip_coupling));
            int32_t lat     = front_lat_force;      /* FRONT lateral (orig +0x320) */
            int32_t latSh   = FP_TRUNC(lat);
            int32_t latMix  = FP_TRUNC((latSh * lat));
            int32_t hyp_sq  = latMix + coupled * coupled;
            if (hyp_sq < 0) hyp_sq = 0;
            int32_t hyp     = td5_isqrt(hyp_sq) + 1;
            int32_t latSh_a = (latSh < 0) ? -latSh : latSh;
            grip_limit_r    = (latSh_a * grip_limit_r) / hyp;
            front_long      = ((front_long / 2) * coupled) / hyp;  /* FRONT long */
        }

        int32_t rl16 = front_lat_force >> 4;
        int32_t rlo16 = front_long >> 4;
        combined_sq = rl16 * rl16 + rlo16 * rlo16;
        combined = td5_isqrt(combined_sq) << 4;
        if (combined > grip_limit_r && combined > 0) {
            actor->rear_axle_slip_excess = combined - grip_limit_r;   /* +0x320 */
            /* [FIX 2026-05-28] Orig clamps ONLY the lateral force in Pass-B
             * (symmetric to the +0x31C block above) — leave the drive force
             * (front_long) untouched. */
            front_lat_force = FP_TRUNC(((FP_SCALE(grip_limit_r)) / combined * front_lat_force));
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
            slip_mag = FP_TRUNC(abs_d);
        }
        if (scf & 2) {
            int32_t raw_front = (int32_t)(((int64_t)sin_s * vx + (int64_t)cos_s * vz) >> 12);
            int64_t yaw_q12 = (int64_t)sin_sr * front_weight * actor->angular_velocity_yaw;
            int32_t yaw_term = (int32_t)((yaw_q12 >> 12) / 0x28c);
            int32_t body_vlat = raw_front - yaw_term;
            int32_t pos_vlat  = (body_vlat < 0) ? 0 : body_vlat;
            int32_t delta = actor->lateral_speed - pos_vlat;
            int32_t abs_d = (delta < 0) ? -delta : delta;
            slip_mag = FP_TRUNC(abs_d);
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
        int32_t inertia = PHYS_I(actor, PHYS_INERTIA_YAW);
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
        int32_t yaw_max = TD5_YAW_TORQUE_MAX;

        if (yaw_torque > yaw_max)  yaw_torque = yaw_max;
        if (yaw_torque < -yaw_max) yaw_torque = -yaw_max;

        actor->angular_velocity_yaw += yaw_torque;

        /* [task #16] High-grip oversteer / drift fix. In this model a higher
         * tire_grip_coeff (PHYS 0x2C) raises the slip-circle limits, so the REAR
         * axle sustains a larger lateral force (term3 = rear_lat * rear_weight)
         * before clamping. term3 is the spin-inducing (oversteer) term, and the
         * faithful yaw damping (section 14c) only fires on FRONT slip excess —
         * which a high-grip car rarely produces on normal ground — so the rear-
         * driven yaw goes unchecked and the car oversteers/drifts when it should
         * hold the line. Add a stability-control yaw-RATE damping that bleeds off
         * only the EXCESS yaw rate (beyond what the steering input is currently
         * commanding) so the line holds, applied ONLY on normal/high-friction
         * ground (so deliberate low-grip slides on grass etc. are preserved).
         *
         * [REGRESSION FIX #1 2026-06-15] The first cut of this damper multiplied
         * the WHOLE accumulated yaw rate (including the steer-commanded turn-in
         * just added via yaw_torque on this same tick) by up to 0.60/tick, and
         * the grip gate fires for almost every car because difficulty scaling
         * pushes tire_grip_coeff to ~1.17x (Normal) / ~1.48x (Hard) of baseline
         * (td5_physics.c ~10963 / ~10946). Net result: ordinary keyboard turn-in
         * lost a big slice of its yaw every tick → "stiff / unresponsive" steering,
         * worse on grippier cars and on Hard. Two changes restore responsiveness
         * while still curbing true drift:
         *   1. EXCESS-ONLY: keep a steering-commanded allowance band undamped
         *      (a multiple of |yaw_torque|, this tick's commanded yaw increment,
         *      dominated by the steer term1) plus a small floor; damp only the
         *      surplus |yaw rate| above it. Turn-in (yaw tracks the command)
         *      passes through unchanged; only yaw that persists after the command
         *      has been clamped/eased — i.e. real oversteer — gets trimmed.
         *   2. Gentler gain + cap: damp only a small slice of the SURPLUS (not of
         *      the whole rate).
         *
         * [REGRESSION FIX #2 2026-06-15, item #11 "grip fix may have worked too
         * good"] After fix #1 cars felt TOO planted — deliberate drifts no longer
         * held because even the surplus band was trimmed too eagerly. Loosen it so
         * drifting is possible again and ONLY extreme, persistent oversteer is
         * still curbed:
         *   - WIDER undamped allowance band: cmd*8 + 0x60 (was cmd*6 + 0x40), so
         *     more of the rotation reads as commanded turn-in / a held slide and
         *     passes through untouched.
         *   - LOWER gain: 24/256 = ~0.094 per unit-excess (was 32/256 = 0.125).
         *   - LOWER cap: 38/256 = ~0.15 of the SURPLUS (was 64/256 = 0.25).
         * Net: turn-in and ordinary drifts survive; only the surplus yaw that
         * persists well past a wide command band gets a light trim.
         *
         * Knob TD5RE_GRIP_DRIFT_FIX (default ON). "0" reverts to no damping. */
        {
            static int s_gdf = -1;
            if (s_gdf < 0) {
                s_gdf = td5_env_flag_on("TD5RE_GRIP_DRIFT_FIX");
                TD5_LOG_I(LOG_TAG, "grip_drift_fix: TD5RE_GRIP_DRIFT_FIX=%d", s_gdf);
            }
            /* Only damp when grip is above 1.0x baseline AND the rear is on
             * high-friction ground (sf ~256 = asphalt; low values = grass/dirt). */
            const int32_t GRIP_BASELINE = 256;     /* 1.0x in Q8 */
            if (s_gdf && tire_grip_coeff > GRIP_BASELINE && sf_rl >= 192 && sf_rr >= 192) {
                int32_t excess = tire_grip_coeff - GRIP_BASELINE;   /* Q8 grip over 1.0 */

                /* Steering-commanded allowance: the yaw the driver is asking for
                 * this tick is ~|yaw_torque| (term1 = steer term dominates). The
                 * accumulated rate up to a few ticks' worth of that command is
                 * intended turn-in and must NOT be damped. Anything past it is the
                 * rear-driven surplus (drift). Floor keeps tiny commands from
                 * leaving a near-zero band that nibbles steady-state cornering. */
                int32_t cmd = yaw_torque < 0 ? -yaw_torque : yaw_torque;
                int32_t allowance = cmd * 8 + 0x60;     /* ~8 ticks of command + wider floor */

                int32_t avy = actor->angular_velocity_yaw;
                int32_t avy_mag = avy < 0 ? -avy : avy;
                int32_t surplus = avy_mag - allowance;  /* yaw beyond the command */
                if (surplus > 0) {
                    /* damp_q8 = excess * gain, capped at ~0.15 of the SURPLUS.
                     * gain 24/256 = ~0.094 per unit-excess: a 1.17x-grip car
                     * (excess ~44) damps ~4% of the surplus, 1.48x (~123) ~4.5%
                     * pre-cap, very grippy cars reach the 0.15 cap — and all of
                     * it bites only the over-rotation, never the commanded turn.
                     * Tuned DOWN from 32/0.25 so deliberate drifts stay possible
                     * (item #11); only extreme persistent oversteer is trimmed. */
                    int32_t damp_q8 = FP_TRUNC((excess * 24));
                    if (damp_q8 > 38) damp_q8 = 38;       /* cap ~0.15 (38/256) */
                    int32_t yd = FP_TRUNC((surplus * damp_q8));
                    if (avy < 0) yd = -yd;               /* damp toward zero */
                    actor->angular_velocity_yaw = avy - yd;
                    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
                        TD5_LOG_I(LOG_TAG,
                            "grip_drift_fix slot0: grip=%d excess=%d cmd=%d allow=%d "
                            "surplus=%d damp_q8=%d yd=%d ang_vel=%d",
                            tire_grip_coeff, excess, cmd, allowance, surplus,
                            damp_q8, yd, actor->angular_velocity_yaw);
                    }
                }
            }
        }

        if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "YAW: torque=%d ang_vel=%d heading=%d t1=%d t2=%d t3=%d idiv=%d",
                      yaw_torque, actor->angular_velocity_yaw,
                      FP_ANGLE(actor->euler_accum.yaw),
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
    /* [FORCEDIAG 2026-05-27] Force-cascade root-cause: the suspension squat
     * over-excites because player_fx/fz (the per-tick world accel) feeds the
     * spring. Capture the whole longitudinal chain — engine torque → axle
     * long forces → player_fx/fz → suspension pos — so it can be diffed
     * against the orig under a deterministic AutoThrottle launch. Slot 0,
     * every 12 ticks. */
    if (actor->slot_index == 0 && (actor->frame_counter % 12u) == 0u) {
        TD5_LOG_I(LOG_TAG,
            "FORCEDIAG vlong=%d rpm=%d gear=%d torque=%d fLong=%d rLong=%d "
            "fLat=%d rLat=%d fx=%d fz=%d susp=[%d,%d,%d,%d]",
            v_long, actor->engine_speed_accum, (int)actor->current_gear, drive_torque,
            front_long, rear_long, front_lat_force, rear_lat_force,
            player_fx, player_fz,
            actor->wheel_suspension_pos[0], actor->wheel_suspension_pos[1],
            actor->wheel_suspension_pos[2], actor->wheel_suspension_pos[3]);
    }
    actor->linear_velocity_x += player_fx;
    actor->linear_velocity_z += player_fz;

    /* [LANE ASSIST 2026-06-28] Optional steering aid (port-only, default OFF):
     * a gentle, capped lateral nudge toward the nearest lane-centre line,
     * applied at this same lateral choke point so it composes with — never
     * overrides — the handling forces above. No-op unless enabled for this
     * player's slot. See td5_laneassist.c. */
    td5_laneassist_apply(actor, sin_h, cos_h);

    /* --- 14a-slope. Gravity-along-slope longitudinal force [task #6] ---------
     * PORT ADDITION (gated). The faithful model applies gravity only to
     * linear_velocity_y (vertical); the longitudinal slope drag emerges purely
     * from the ground-snap re-projecting velocity onto the incline, which is
     * weak — the car barely loses speed climbing. Add an explicit gravity-
     * along-slope force projected onto the heading-forward axis so driving
     * UPHILL decelerates noticeably more (and downhill stays sane).
     *
     * actor->heading_normal (+0x290) is the live surface normal during racer
     * driving (written by td5_track_compute_runtime_heading_normal via the
     * cross-product InterpolateTrackSegmentNormal; N_y ~= 4096 flat, dropping
     * on slopes — see the heading_normal.y comment at the integrate-pose
     * refresh site). The down-slope acceleration projected onto the (horizontal)
     * heading-forward direction (sin_h, cos_h) is:
     *     g_long = g * N_y * (N_x*sin_h + N_z*cos_h)        (two >>12 reductions)
     * On a climb the normal's horizontal part points backward, so
     * (N_x*sin_h + N_z*cos_h) < 0 -> g_long < 0 -> deceleration; on a descent
     * it is > 0 -> a (smaller) acceleration. We rotate g_long back to world XZ
     * along the heading and add it to linear_velocity.
     *
     * Knob TD5RE_SLOPE_DECEL = scalar multiplier on the UPHILL (decelerating)
     * component (default 2.0 = "stronger than current", which is effectively
     * zero explicit term). The DOWNHILL (accelerating) component always uses
     * 1.0x so descents stay physically sane and the car never rockets downhill.
     * "0" disables the whole term (revert to the old emergent-only behaviour). */
    {
        static int   s_slope_init = 0;
        static float s_slope_mult = 2.0f;   /* uphill decel multiplier (default) */
        if (!s_slope_init) {
            const char *e = getenv("TD5RE_SLOPE_DECEL");
            if (e && e[0]) {
                s_slope_mult = (float)atof(e);
                if (s_slope_mult < 0.0f) s_slope_mult = 0.0f;
            }
            s_slope_init = 1;
            TD5_LOG_I(LOG_TAG, "slope_decel: TD5RE_SLOPE_DECEL=%.3f (uphill decel x; downhill always x1)",
                      s_slope_mult);
        }

        if (s_slope_mult > 0.0f) {
            int32_t nx = (int32_t)actor->heading_normal.x;
            int32_t ny = (int32_t)actor->heading_normal.y;
            int32_t nz = (int32_t)actor->heading_normal.z;
            /* Forward (horizontal) projection of the surface normal — sign tells
             * uphill (<0) vs downhill (>0) along the travel heading. */
            int32_t fwd_dot = (nx * sin_h + nz * cos_h) >> 12;   /* 12-bit-scaled */
            /* g_long = g * N_y * fwd_dot, reduced by two >>12. ny ~= 4096 so the
             * first reduction recovers ~g; the second scales by fwd_dot/4096. */
            int32_t g_ny    = (g_gravity_constant * ny) >> 12;
            int32_t g_long  = (g_ny * fwd_dot) >> 12;   /* >0 downhill, <0 uphill */
            /* Apply the multiplier only on the decelerating (uphill) side. */
            int32_t light_q12 = SLOPE_LIGHT_Q12_ONE;
            if (g_long < 0) {
                /* [#6 2026-06-19] Once the car has braked all the way down to
                 * GEAR 1 (TD5_GEAR_FIRST), stop applying the uphill slope
                 * resistance entirely so it can always crawl up the hill in
                 * first instead of stalling to 0 and being unable to accelerate.
                 * Higher gears still feel the slope, so on a climb the car
                 * naturally brakes down through the gears until it reaches first,
                 * then holds a steady low-gear climb. Knob TD5RE_SLOPE_GEAR1_FREE
                 * (default on). */
                static int s_gear1_free = -1;
                if (s_gear1_free < 0) {
                    s_gear1_free = td5_env_flag_on("TD5RE_SLOPE_GEAR1_FREE");
                }
                if (s_gear1_free && actor->current_gear <= TD5_GEAR_FIRST) {
                    g_long = 0;   /* gear 1 (or lower): no uphill slope drag */
                } else {
                    g_long = (int32_t)((float)g_long * s_slope_mult);
                    /* [#8 2026-06-15] Weaken the uphill decel for lower-powered
                     * cars so a slow car does not crawl uphill. Scale by the
                     * car's top-speed rating (Q12, clamped to a floor); fast cars
                     * unchanged. Downhill (g_long > 0) is left at full strength. */
                    light_q12 = td5_physics_slope_light_scale_q12(
                                    (int32_t)PHYS_S(actor, PHYS_TOP_SPEED));
                    if (light_q12 != SLOPE_LIGHT_Q12_ONE)
                        g_long = (int32_t)(((int64_t)g_long * light_q12) >> 12);
                    /* [WEIGHT 2026-06-25] Mass-relative uphill decel: a HEAVIER
                     * car loses MORE speed on the climb (factor >1.0), a LIGHTER
                     * car climbs easily (<1.0). Derived from collision_mass
                     * (0x88) via the shared heaviness math, composes with the
                     * top-speed light scale above. Knob TD5RE_WEIGHT_SLOPE_PCT. */
                    {
                        int32_t w_q12 = td5_physics_weight_slope_q12(actor);
                        if (w_q12 != 0x1000)
                            g_long = (int32_t)(((int64_t)g_long * w_q12) >> 12);
                    }
                }
            }
            /* Rotate the longitudinal slope force back to world XZ along heading. */
            actor->linear_velocity_x += (g_long * sin_h) >> 12;
            actor->linear_velocity_z += (g_long * cos_h) >> 12;

            if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "slope_decel slot0: ny=%d fwd_dot=%d g_long=%d mult=%.2f "
                    "top_spd=%d light_q12=%d vlong=%d",
                    ny, fwd_dot, g_long, s_slope_mult,
                    (int)PHYS_S(actor, PHYS_TOP_SPEED), light_q12, v_long);
            }
        }
    }

    /* [DOWNFORCE 2026-06-25] Aerodynamic high-speed cornering grip: damp a slice
     * of lateral (slide) velocity scaled by the car's downforce rating and the
     * square of forward speed. High-downforce cars stay planted in fast corners;
     * low-downforce cars keep sliding. Gentle + speed-gated; knob TD5RE_DOWNFORCE.
     * Heading basis (sin_h/cos_h) computed at section 6 is still in scope here. */
    td5_physics_apply_downforce(actor, sin_h, cos_h);

    /* [moved 2026-05-25 — scf-wheelspin-ordering fix]: the section 14a
     * `actor->longitudinal_speed` / `lateral_speed` writeback block formerly
     * lived here. It is now executed UP at section 6a (just below v_long/v_lat
     * computation, before the section 7-11 dispatch) so that auto_gear,
     * slip-circle pass A, and current_slip_metric all see this tick's
     * freshly-written values instead of stale previous-tick values. See the
     * hoisted block's header comment for full rationale and orig-Ghidra
     * verification (UpdatePlayerVehicleDynamics @ 0x00404030 dispatch block).
     *
     * This placeholder is preserved for code-archaeology traceability.
     * todo: todo_scf_wheelspin_ordering.md */

    /* --- 14b. Velocity magnitude safety backstop (2x speed_limit) ---
     * NON-ORIGINAL. The original UpdatePlayerVehicleDynamics @ 0x00404030 has
     * NO total-velocity-magnitude clamp here: it limits speed via the drive-
     * torque cutoff at the `abs_speed <= speed_limit` guards (preserved above,
     * CONFIRMED @ 0x00404030) plus track-wall containment through corners. This
     * block sits between the confirmed-orig force writeback (14a) and the yaw
     * damping 14c (CONFIRMED @ 0x404DB6) — there is no original instruction
     * here.
     *
     * It was added as a 1x hard cap "until track walls are implemented". Lateral
     * rail containment is now ported (0x00406CC0, the Tokyo wall work), so the
     * 1x cap is obsolete: it clipped corner-exit speed and could mask upstream
     * force divergence by clamping the symptom. Raised to 2x speed_limit — the
     * walls now do the real containment; this remains only as a runaway-velocity
     * backstop and lets the car carry momentum (gravity / corner exit) above
     * speed_limit the way the original does.
     *
     * Drive-test gate: car must stay contained on Tokyo + Moscow AND reach
     * higher corner-exit speed than the 1x cap allowed. If a tested track lets
     * the car leave the road, KEEP the 2x cap (do not remove it). */
    {
        int32_t speed_lim = (int32_t)FP_SCALE(PHYS_S(actor, PHYS_TOP_SPEED));
        int32_t vel_cap = speed_lim * 2;  /* 2x backstop (was 1x); walls do real containment */
        int32_t vxh = FP_TRUNC(actor->linear_velocity_x);
        int32_t vzh = FP_TRUNC(actor->linear_velocity_z);
        int32_t mag_sq = vxh * vxh + vzh * vzh;
        int32_t cap_sq = (FP_TRUNC(vel_cap)) * (FP_TRUNC(vel_cap));
        if (mag_sq > cap_sq && mag_sq > 0) {
            int32_t mag = td5_isqrt(mag_sq);
            int32_t cap_h = FP_TRUNC(vel_cap);
            actor->linear_velocity_x = (int32_t)((int64_t)actor->linear_velocity_x * cap_h / mag);
            actor->linear_velocity_z = (int32_t)((int64_t)actor->linear_velocity_z * cap_h / mag);
            /* Throttled diagnostic (slot 0, every 120 frames) so a drive-test
             * can tell whether the 2x backstop is engaging — same pattern as 14c. */
            if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
                TD5_LOG_I(LOG_TAG, "vel_backstop_2x: fired mag=%d cap_h=%d speed_lim_h=%d",
                          mag, cap_h, FP_TRUNC(speed_lim));
            }
        }
    }

    /* --- 14c. Front slip excess yaw damping [CONFIRMED @ 0x404DB6-0x404DF4] ---
     * When front tires exceed grip circle, apply proportional correction to
     * angular_velocity_yaw. This is the primary yaw damping mechanism that
     * prevents the car from spinning. Without it, any small yaw perturbation
     * feeds back through tire slip → lateral force → more yaw torque. */
    if (actor->front_axle_slip_excess > 0) {
        /* D14 — SAR-RZ-6 then SAR-RZ-15 [CONFIRMED @ 0x00404DB6-0x00404DD5].
         * Original: CDQ; AND EDX,0x3F; ADD; SAR 6 → IMUL; CDQ; AND EDX,0x7FFF; ADD; SAR 15. */
        int32_t correction = SAR_RZ_6(actor->angular_velocity_yaw)
                           * actor->front_axle_slip_excess;
        correction = SAR_RZ_15(correction);
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

    /* --- 15. ApplySteeringTorqueToWheels — REMOVED from the per-tick physics
     * path (2026-05-27 root-cause fix for the slope-attitude / over-squat bug).
     *
     * The original calls ApplySteeringTorqueToWheels (0x0042EEA0) from EXACTLY
     * ONE place — UpdatePlayerVehicleControlState @ 0x00402E60 — and only on a
     * manual GEAR-UP shift event (`if (controlBits & 0x400000) { gear++;
     * ApplySteeringTorqueToWheels(); }`). It is a ONE-SHOT ±k pitch kick added
     * to wheel_spring_dv[0..3] (FL/FR +, RL/RR -) per upshift. The original's
     * UpdatePlayerVehicleDynamics @ 0x00404030 does NOT call it at all.
     *
     * The port already applies that upshift kick faithfully + gated in the
     * input handler (td5_input.c gear-up block, ~line 847). This per-tick call
     * was a spurious DUPLICATE: it re-injected ~+780/tick (throttle*mult*26 *
     * gearTorque) into wheel_spring_dv EVERY frame, so the suspension spring
     * never settled — it rang and sat pinned ~14x its equilibrium (the original
     * settles to a fixed point). That sustained over-squat dominated the
     * wheel-derived attitude (TransformTrackVertexByMatrix reads
     * wheel_suspension_pos), which is why the car would not follow slopes.
     * Verified via matched part-throttle A/B: orig suspension dv 39→0 (settles
     * pos~106); port dv 780→ring (pos~3500) at identical force. Removing this
     * call makes the port suspension converge like the original. */

    /* --- 16. IntegrateWheelSuspensionTravel ---
     * Pass the net world-frame velocity delta applied this frame as the
     * excitation for the per-wheel spring-damper (matches original at
     * 0x00404EA2 passing iVar11/iVar36 = fx/fz). */
    td5_physics_integrate_suspension(actor, player_fx, player_fz);

    /* --- 17. ApplyMissingWheelVelocityCorrection --- */
    td5_physics_missing_wheel_correction(actor);

    /* --- 17a. Wheelspin override [CONFIRMED @ 0x00404E00-0x00404E1C].
     *
     * Three-condition gate: gear==FIRST(2), engine-implied wheel speed
     * exceeds body forward speed by > 0x12C00, throttle > 0x7F. When all
     * true: surface_contact_flags = tuning[0x76] (drivetrain layout byte,
     * 1=RWD/2=FWD/3=AWD). The SET routes the next tick to CRGT (the
     * "engine governs ground speed" branch) — the orig's launch/burnout
     * model. Without it, 1st-gear accel from a stop is limited to what the
     * bicycle/slip-circle force model can deliver, which is sluggish.
     *
     * Two bugs in the previous port — both audited from a fresh Ghidra
     * decomp of 0x00404030 on 2026-05-23:
     *
     *   1. The throttle gate read `(int32_t)(uint8_t)encounter_steering_cmd
     *      > 0x7F`. encounter_steering_cmd is int16_t and normal forward
     *      throttle is 0x0100 (= 256); the uint8_t cast truncates that to
     *      the low byte 0x00, which is never > 0x7F. Gate never fired in
     *      practice — wheelspin SET was unreachable. Orig directly compares
     *      the signed int16 (`0x7f < (short)cmd`), so 256 > 127 ⇒ true.
     *
     *   2. `uVar12` was sourced as `steering_command >> 8`. Ghidra's
     *      decomp reuses the local name `uVar12` twice in this function:
     *      first as `steering_command >> 8` near the top, then reassigned
     *      to `(vx*sin + vz*cos) >> 12` (body-frame forward velocity) just
     *      before the dispatch around `if (scf != 0)`. The wheelspin SET
     *      conditional at the bottom uses the LATE one — body_v_long, not
     *      steering. Confusing the two made wheelspin = engine_implied -
     *      steering ≈ huge always (steering ~0 at center), so the SET
     *      fired indiscriminately whenever the throttle gate happened to
     *      pass. The orig wheelspin = engine_implied - body_v_long, which
     *      is huge only at launch (engine spinning, body not moving yet)
     *      and shrinks as the car catches up.
     *
     * After the scf-source removal in step 9 of update_player, scf is
     * permanently 0 going INTO this block (until this SET fires), so
     * actor->longitudinal_speed equals body_v_long from the scf==0 dispatch
     * branch — we use it directly as the orig's uVar12 stand-in. */
    {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, 0x32);
        if (gear_ratio != 0) {
            int32_t rpm_norm = (((actor->engine_speed_accum - 400) * 0x1000) / 0x2d) / gear_ratio;
            int32_t body_vlong_q8 = actor->longitudinal_speed;
            int32_t wheelspin = rpm_norm * 0x100 - body_vlong_q8;
            if (actor->current_gear == 2 &&
                wheelspin > 0x12C00 &&
                (int32_t)actor->encounter_steering_cmd > 0x7F) {
                TD5_LOG_I(LOG_TAG,
                    "wheelspin SET: slot=%d rpm_norm=%d wsp=%d vlong=%d thr=%d scf=%d->%d",
                    actor->slot_index, rpm_norm, wheelspin, body_vlong_q8,
                    (int)actor->encounter_steering_cmd,
                    actor->surface_contact_flags,
                    (int)(uint8_t)PHYS_S(actor, PHYS_DRIVETRAIN_TYPE));
                actor->surface_contact_flags = (uint8_t)PHYS_S(actor, PHYS_DRIVETRAIN_TYPE);
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
    actor->accumulated_tire_slip_x += (int16_t)(FP_TRUNC(actor->lateral_speed));
    if (!actor->handbrake_flag) {
        actor->accumulated_tire_slip_z += (int16_t)(FP_TRUNC(actor->longitudinal_speed));
    }

    /* NOTE: current_slip_metric (+0x33C) is now written inside the slip-circle
     * block (see section 13a above) per the original's structure at
     * 0x004049BA/0x00404A80. Do NOT write it again here — the earlier
     * `abs(lateral_speed)>>8` tail-write used the wrong values (post-update)
     * and collapsed slip to near-zero in normal driving. */
}

/* ========================================================================
 * AI 2-axle dynamics -- UpdateAIVehicleDynamics (0x404EC0)
 * ========================================================================
 *
 * [CONFIRMED @ 0x00404EC0] Byte-faithful with orig UpdateAIVehicleDynamics.
 * SAR-RZ rounding fixed 2026-05-18 (L5 promotion follow-up).
 *
 * Static port audited byte-for-byte against listing 0x00404EC0..0x004057E5
 * (2341 bytes / ~570 instructions / 95 decompiled lines). Body structure
 * matches original: surface probe → drag → body-frame trig → load
 * transfer → throttle/brake → bicycle solve → slip-circle.
 *
 * SHIPPED FIXES (already in master, audited match):
 *   - AI brake formula faithful port (commit 63e9624, three-bug fix)
 *   - AI slot 0 PlayerIsAI=1 carparam exemption (commit 48d320a)
 *
 * SHIPPED 2026-05-18 — D1 SAR-RZ LSB site:
 *   D1 — velocity drag @ 0x00404F33..0x00404F91 (SAR-RZ-8 then SAR-RZ-12).
 *
 * RESIDUAL KNOWN DIVERGENCES (re/analysis/pilot_00404EC0_audit.md):
 *   D3     Tire-grip fallback when 0 — safety net only fires under
 *          carparam-loading regression; benign with proper Viper carparam.
 *   D4     `current_slip_metric` tail-write — DELIBERATE enhancement to
 *          drive AI tire/smoke pipeline; not a faithful divergence.
 *   D5     PlayerIsAI=1 cardef fallback — UPSTREAM (shipped, see above).
 *
 * KNOWN TODO chain owners (cascade-investigation):
 *   - todo_state0f_overfire_skips_player.md (AI dynamics tick overshoot)
 *
 * Audit reference: re/analysis/pilot_00404EC0_audit.md (2026-05-14, pool12).
 */

/* Pilot trace emitters (pool12 / precise-port workflow) */

void td5_physics_update_ai(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;


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
    int32_t front_weight = (int32_t)PHYS_S(actor, PHYS_FRONT_WEIGHT);
    int32_t rear_weight  = (int32_t)PHYS_S(actor, PHYS_REAR_WEIGHT);
    int32_t total_weight = front_weight + rear_weight;
    if (total_weight == 0) total_weight = 1;

    int32_t half_wb = PHYS_I(actor, PHYS_HALF_WHEELBASE);
    if (half_wb == 0) half_wb = 1;

    int32_t susp_defl = actor->center_suspension_pos;

    /* Cross-weight load transfer [CONFIRMED @ 0x40506B]:
     * front_load = (rear_weight << 8) / total_weight * (half_wb - susp_defl) / half_wb
     * rear_load  = (front_weight << 8) / total_weight * (half_wb + susp_defl) / half_wb */
    int32_t front_load = ((FP_SCALE(rear_weight)) / total_weight) * (half_wb - susp_defl) / half_wb;
    int32_t rear_load  = ((FP_SCALE(front_weight)) / total_weight) * (half_wb + susp_defl) / half_wb;

    /* Grip from surface friction * load [CONFIRMED @ 0x4050B8] */
    int32_t sf = phys_surface_grip(surface);   /* [task#15] TD6-aware */
    int32_t grip_front = FP_TRUNC((sf * front_load + 128));
    int32_t grip_rear  = FP_TRUNC((sf * rear_load + 128));

    if (grip_front < TD5_AI_GRIP_MIN) grip_front = TD5_AI_GRIP_MIN;
    if (grip_front > TD5_AI_GRIP_MAX) grip_front = TD5_AI_GRIP_MAX;
    if (grip_rear < TD5_AI_GRIP_MIN) grip_rear = TD5_AI_GRIP_MIN;
    if (grip_rear > TD5_AI_GRIP_MAX) grip_rear = TD5_AI_GRIP_MAX;
    grip_front = phys_td6_grass_slide(grip_front, surface);  /* [#15] verge slide */
    grip_rear  = phys_td6_grass_slide(grip_rear,  surface);

    /* --- 4. Velocity drag in WORLD frame [CONFIRMED @ 0x404EC0] --- */
    {
        int32_t surf_drag = phys_surface_drag(surface);   /* [task#15] TD6-aware */
        int32_t damp_coeff;
        if (actor->encounter_steering_cmd < 0x20 || actor->current_gear < 2)
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, PHYS_DAMP_COEFF_BASE);
        else
            damp_coeff = surf_drag * 256 + (int32_t)PHYS_S(actor, PHYS_DAMP_COEFF_TURN);

        /* D1 — SAR-RZ-8 then SAR-RZ-12 [CONFIRMED @ 0x00404F33-0x00404F91].
         * Same SAR-RZ idiom as the player drag at 0x004040E5. */
        actor->linear_velocity_x -= SAR_RZ_12(SAR_RZ_8(actor->linear_velocity_x) * damp_coeff);
        actor->linear_velocity_z -= SAR_RZ_12(SAR_RZ_8(actor->linear_velocity_z) * damp_coeff);
    }

    /* --- 5. Body-frame velocities + steered-frame trig --- */
    int32_t heading = FP_ANGLE(actor->euler_accum.yaw);
    int32_t cos_h = cos_fixed12(heading);
    int32_t sin_h = sin_fixed12(heading);

    int32_t steer_angle = FP_TRUNC(actor->steering_command);
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
    int32_t inertia = PHYS_I(actor, PHYS_INERTIA_YAW);
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
    int32_t brake_front = (int32_t)PHYS_S(actor, PHYS_BRAKE_FRONT);
    int32_t brake_rear  = (int32_t)PHYS_S(actor, PHYS_BRAKE_REAR);
    int32_t speed_limit = FP_SCALE(phys_top_speed_rating(actor));

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
            /* [MANUAL BOOST #2] Top-speed half (AI drive gate): a manual-gearbox
             * AI car (byte +0x378 == 0) tops out +N% higher. 1.0 for automatic
             * or knob-off → byte-faithful gate. Drive-side only (brake clamps
             * below use lateral_speed/v_long, not speed_limit). */
            if (v_long > td5_physics_apply_speed_limit_boost(
                             speed_limit, td5_physics_actor_manual_boost_q8(actor))) {
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
            int32_t R_raw = FP_TRUNC(((int32_t)brake_rear  * throttle
                          + ((((int32_t)brake_rear  * throttle) >> 31) & 0xFF)));
            int32_t F_raw = FP_TRUNC(((int32_t)brake_front * throttle
                          + ((((int32_t)brake_front * throttle) >> 31) & 0xFF)));

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
        int32_t tire_grip = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
        if (tire_grip == 0) tire_grip = sf; /* fallback */

        /* Front axle slip circle */
        int32_t fl4 = front_lat >> 4;
        int32_t fd4 = front_drive >> 4;
        int32_t mag_sq_f = fl4 * fl4 + fd4 * fd4;
        int32_t mag_f = (int32_t)sqrtf((float)mag_sq_f); /* [CONFIRMED @ 0x0040554B: FILD/FSQRT/__ftol] */
        int32_t grip_limit_f = FP_TRUNC((front_load * tire_grip));
        int32_t slip_f16 = mag_f << 4;

        if (slip_f16 > grip_limit_f && slip_f16 > 0) {
            actor->front_axle_slip_excess = slip_f16 - grip_limit_f;
            int32_t scale = (FP_SCALE(grip_limit_f)) / slip_f16;
            front_lat = FP_TRUNC((front_lat * scale));
            front_drive = FP_TRUNC((front_drive * scale));
        } else {
            actor->front_axle_slip_excess = 0;
        }

        /* Rear axle slip circle */
        int32_t rl4 = rear_lat >> 4;
        int32_t rd4 = rear_drive >> 4;
        int32_t mag_sq_r = rl4 * rl4 + rd4 * rd4;
        int32_t mag_r = (int32_t)sqrtf((float)mag_sq_r); /* [CONFIRMED @ 0x004055D9: FILD/FSQRT/__ftol] */
        int32_t grip_limit_r = FP_TRUNC((rear_load * tire_grip));
        int32_t slip_r16 = mag_r << 4;

        if (slip_r16 > grip_limit_r && slip_r16 > 0) {
            actor->rear_axle_slip_excess = slip_r16 - grip_limit_r;
            int32_t scale = (FP_SCALE(grip_limit_r)) / slip_r16;
            rear_lat = FP_TRUNC((rear_lat * scale));
            rear_drive = FP_TRUNC((rear_drive * scale));
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
    actor->accumulated_tire_slip_x += (int16_t)(FP_TRUNC(actor->lateral_speed));
    actor->accumulated_tire_slip_z += (int16_t)(FP_TRUNC(actor->longitudinal_speed));

    /* current_slip_metric (+0x33C) — see player path for rationale. Same
     * formula so AI cars also drop tire marks and smoke when drifting. */
    {
        int32_t slip = FP_TRUNC(abs(actor->lateral_speed));
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

    /* [dynamic-traffic] A parked (despawned) traffic car is frozen in place —
     * no friction integration, no route advance. Covers both the main
     * dispatch and the countdown traffic step. No-op when Dynamic=0. */
    if (td5_ai_traffic_dynamic_parked(actor->slot_index))
        return;

    /* [#1 WRECK STAND-STILL 2026-06-21] A broken-down (wrecked) traffic car must
     * stand still — NOT reverse. The AI stop-command leaves encounter_steering_cmd
     * = -0x100, which this integrator turns into throttle = -1024 and applies as a
     * BACKWARD force (step 9), so a wreck accelerated backwards forever. Fix: a
     * wreck never self-propels (throttle forced 0 below) and is ANCHORED (zero
     * velocity, skip integration) so it sits where it died — UNLESS a recent V2V
     * ram left it inside its free-slide window (g_wreck_push_ticks), during which
     * it coasts so the player can shove it aside (issue #2). Knob TD5RE_WRECK_IMMOBILE. */
    int wreck_no_throttle = 0;
    if (wreck_immobile_enabled() &&
        td5_ai_actor_is_broken_down(actor->slot_index)) {
        int si = (int)actor->slot_index;
        if (si >= 0 && si < TD5_MAX_TOTAL_ACTORS && g_wreck_push_ticks[si] > 0) {
            g_wreck_push_ticks[si]--;       /* sliding from a fresh ram -> coast */
            wreck_no_throttle = 1;          /* but never self-propel */
        } else {
            actor->linear_velocity_x = 0;   /* anchored: stand still */
            actor->linear_velocity_z = 0;
            actor->longitudinal_speed = 0;
            return;
        }
    }

    #define SAR12(x) (((x) + (((x) >> 31) & 0xFFF)) >> 12)
    #define SAR10(x) (((x) + (((x) >> 31) & 0x3FF)) >> 10)
    #define SAR8_U8(x) (FP_TRUNC(((x) + (((x) >> 31) & 0xFF))))

    /* 1. Velocity drag — v -= trunc(v*16 / 4096). [@ 0x00443900-0x00443932] */
    int32_t t = actor->linear_velocity_x * 0x10;
    int32_t vx = actor->linear_velocity_x - SAR12(t);
    t = actor->linear_velocity_z * 0x10;
    int32_t vz = actor->linear_velocity_z - SAR12(t);
    actor->linear_velocity_z = vz;

    int32_t yaw_vel = actor->angular_velocity_yaw;  /* iVar1 — used RAW */
    uint32_t yaw12 = (uint32_t)FP_TRUNC(actor->euler_accum.yaw);
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
    if (wreck_no_throttle) throttle = 0;   /* [#1] a wreck never drives itself */

    /* [COP OVERHAUL 2026-06-29] Chasing-cop acceleration rubber-band: scale the
     * FORWARD propulsion by the catch-up boost (Q8) so a traffic-slot cop can
     * out-drag a faster suspect on a straight. The simplified traffic model has
     * no engine-power / top-speed term, so a fixed throttle force otherwise caps
     * the cop below a fast player car (the "police can't catch up" symptom).
     * Forward only (never amplifies a brake/reverse); the boost is 1.0 for
     * non-cops, idle cops, at the catch-up cap, or an imminent corner, so this is
     * inert for everything but an actively-sprinting cop. Same biased-toward-zero
     * signed >>8 idiom as the racer torque chokepoint. Deterministic: replicated
     * cop+target speeds -> lockstep-safe. */
    if (throttle > 0 && actor->slot_index >= 0 &&
        td5_ai_cop_is_chasing((int)actor->slot_index)) {
        int32_t cb = td5_ai_cop_chase_throttle_boost_q8((int)actor->slot_index);
        if (cb != 0x100) {
            int64_t scaled = (int64_t)throttle * (int64_t)cb;
            throttle = (int32_t)(FP_TRUNC((scaled + ((scaled >> 63) & 0xFF))));
        }
    }

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


/* Accessor for the pilot trace emitter to read the collisions flag without
 * exposing the file-static g_collisions_enabled. */
int td5_physics_get_collisions_flag(void)
{
    return g_collisions_enabled;
}

/* ========================================================================
 * [CONFIRMED @ 0x00405D70] Byte-faithful with orig ResetVehicleActorState.
 * L5 promotion 2026-05-18 (small-tier sweep). 54-instr listing match: clears
 * 25 unique offsets (surface_contact_flags, vehicle_mode, ang/lin vel block,
 * frame_counter, wheel_contact_bitmask, suspension_pos/spring_dv loops,
 * euler_accum, render_pos = world_pos / 256), seeds gear=2 + rpm=400 +
 * world_pos.y=0xC0000000 sentinel, integrates pose, then re-zeros vel subset.
 *
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
#ifndef TD5RE_RELEASE
#endif

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
    actor->display_angles.yaw = (int16_t)(FP_TRUNC(actor->euler_accum.yaw));  /* +0x20A */

    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);  /* +0x144 */
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);  /* +0x148 */
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);  /* +0x14C */

    /* [NATIVE REVERSE CIRCUIT 2026-06-29] Seed the wheel + body contact probes
     * with the chassis span/lane (+0x80/+0x8C, set by every spawn/recycle caller
     * before this reset) so the integrate's ground-snap below reads the surface
     * at the actor's ACTUAL span. Without it the probes start at the memset-0
     * default and the per-probe walker steps them to the ring-end sentinel
     * (span ~ring-1); on a reverse grid (which sits far from span 0) that reads
     * bogus ground -> the +vy launch that makes cars clip/blink on the FIRST race
     * (warm, hence fine, on a restart). The original (forward only) copies the
     * chassis span into the probes before each contact frame; the port only
     * re-walks them, and a forward grid next to span 0 hid the gap. Covers racers
     * AND traffic (both set +0x80 before reset). Reverse-scoped to keep forward
     * spawning byte-identical. */
    if (g_td5.reverse_direction) {
        uint8_t *abase = (uint8_t *)actor;
        int16_t cspan = *(int16_t *)(abase + 0x80);
        int8_t  clane = *(int8_t  *)(abase + 0x8C);
        int pi;
        for (pi = 0; pi < 4; pi++) {
            int16_t *bp = (int16_t *)(abase + 0x00 + pi * 0x10);  /* body_probes[pi]  */
            int16_t *wp = (int16_t *)(abase + 0x40 + pi * 0x10);  /* wheel_probes[pi] */
            bp[0] = cspan;  ((int8_t *)bp)[12] = clane;
            wp[0] = cspan;  ((int8_t *)wp)[12] = clane;
        }
    }

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
#ifndef TD5RE_RELEASE
#endif
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
    int32_t  sum   = 0;                          /* g_missingWheelVelocityScratch: accumulator (EAX) */
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
    /* avg_norm == g_missingWheelVelocityScratch at this point */

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
 *
 * [CONFIRMED @ 0x403D90] Byte-faithful with orig UpdateVehicleState0fDamping.
 * L5 audit 2026-05-18 (TD5_pool0 read-only):
 *   - Same listing-derived control flow: engine smooth + suspension(0,0) +
 *     zero contact/slip + body-frame longitudinal projection + gate +
 *     roll/pitch 1/16 decay + body-frame slip accumulation.
 *   - RZ shift idiom matches CDQ/AND/ADD/SAR @ 0x403E00..0E, E44..4D, E60..66,
 *     E77..7F; mask = (1<<n)-1.
 *   - Cos/Sin called with NEG yaw, vx/vz pre-shifted to int16 before IMUL
 *     (matches 0x403DAE/DB5/DCA/DD2/DDD..F7).
 *   - Roll fold + sVar2 gate match 0x403E23..3E decision tree (sign-compatible
 *     |sVar2|>0x20 with |roll12|<0x80 in matching sign).
 *   - Slip accumulator reads lateral_speed/longitudinal_speed (+0x318/+0x314)
 *     and uses plain SAR 8 (no RZ) before 16-bit truncation (0x403E7F..9D).
 * ======================================================================== */

/* Round-to-zero arithmetic shift right.
 * Mirrors original CDQ/AND mask/ADD/SAR idiom: for negative non-divisible x,
 * yields one unit closer to zero than plain SAR. Listing pattern @ 0x403E00..0E,
 * 0x403E44..4D, 0x403E60..66, 0x403E77..7F. */
static inline int32_t state0f_sar_rz(int32_t x, int n) {
    int32_t mask = (1 << n) - 1;
    return (x + (((x) >> 31) & mask)) >> n;
}


void td5_physics_state0f_damping(TD5_Actor *actor)
{

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

    int32_t vz_hi = (int32_t)(int16_t)((uint32_t)FP_TRUNC(actor->linear_velocity_z));
    int32_t vx_hi = (int32_t)(int16_t)((uint32_t)FP_TRUNC(actor->linear_velocity_x));

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
    actor->accumulated_tire_slip_x += (int16_t)(FP_TRUNC(actor->lateral_speed));
    actor->accumulated_tire_slip_z += (int16_t)(FP_TRUNC(actor->longitudinal_speed));

}

/* [P1-B SPLIT step 3, 2026-07-02] Engine & Transmission + Surface Normal &
 * Gravity moved to td5_physics_drivetrain.c. Seam: td5_physics_internal.h. */

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
     * differ from in-race transforms and cause huge gap_270 transients.
     * (The snapshots live in td5_physics_suspension.c since the P1-B split.) */
    td5_physics_suspension_race_reset();

    /* [STUCK RECOVERY 2026-06-15] Clear per-race assist state (manual-recovery
     * cooldowns) so state left over from a previous race in the same session
     * can't carry over. Lives in td5_physics_assists.c since the P1-B split. */
    td5_physics_assists_race_reset();

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
                    int32_t tm = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);
                    write_i16((uint8_t *)phys, PHYS_DRIVE_TORQUE_MULT, (int16_t)(FP_TRUNC((tm * 0x28A))));
                    /* 0x2C: tire_grip_coeff *= 0x17C/256 (1.48x) */
                    int32_t dc = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
                    write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)(FP_TRUNC((dc * 0x17C))));
                    /* 0x6E: brake_force *= 0x1C2/256 (1.76x) */
                    int32_t bf = (int32_t)PHYS_S(actor, PHYS_BRAKE_FRONT);
                    write_i16((uint8_t *)phys, PHYS_BRAKE_FRONT, (int16_t)(FP_TRUNC((bf * 0x1C2))));
                    /* 0x70: engine_brake *= 400/256 (1.56x) */
                    int32_t eb = (int32_t)PHYS_S(actor, PHYS_BRAKE_REAR);
                    write_i16((uint8_t *)phys, PHYS_BRAKE_REAR, (int16_t)(FP_TRUNC((eb * 400))));
                    /* 0x78: speed_scale <<= 2 */
                    int32_t ss = (int32_t)PHYS_S(actor, PHYS_SPEED_SCALE);
                    write_i16((uint8_t *)phys, PHYS_SPEED_SCALE, (int16_t)(ss << 2));
                } else {
                    /* [ARCADE/SIM CONSOLIDATION 2026-06-26] BOTH dynamics modes
                     * now apply this arcade stat scaling (torque/grip/speed). The
                     * only base difference between the modes is GRAVITY (selected
                     * above: SIMULATION=easy=1500, ARCADE=normal=1900). This makes
                     * SIMULATION the requested "sim gravity + arcade grip/torque/
                     * speed-scale" mix; ARCADE is the same base plus the extra
                     * exaggeration below + 3x collisions + power-ups. Previously
                     * this branch was gated `!g_difficulty_easy` (arcade only) so
                     * SIMULATION got raw carparam values — that path is retired. */
                    /* 0x68: drive_torque_mult *= 0x168/256. NOTE: 0x168 = 360, so
                     * this is ×1.40625 (a BOOST). The prior inline comment said
                     * "(0.5625x)" — that was wrong; the value/behaviour is a boost. */
                    int32_t tm = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);
                    write_i16((uint8_t *)phys, PHYS_DRIVE_TORQUE_MULT, (int16_t)(FP_TRUNC((tm * 0x168))));
                    /* 0x2C: tire_grip_coeff *= 300/256 (1.17x) */
                    int32_t dc = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
                    write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)(FP_TRUNC((dc * 300))));
                    /* 0x78: speed_scale <<= 1 */
                    int32_t ss = (int32_t)PHYS_S(actor, PHYS_SPEED_SCALE);
                    write_i16((uint8_t *)phys, PHYS_SPEED_SCALE, (int16_t)(ss << 1));

                    /* [ARCADE wild] Extra exaggeration on RACER slots only (skip
                     * traffic) when in ARCADE mode: a further grip/torque bump so
                     * arcade handling feels punchier. Modest defaults keep the car
                     * drivable — the headline arcade chaos is the 3x collisions +
                     * power-ups, not raw stat inflation. Knob-tunable; reads the
                     * already-scaled value so the percentages compound on top. */
                    if (!g_difficulty_easy && slot < g_traffic_slot_base) {
                        const char *te = getenv("TD5RE_ARCADE_TORQUE_PCT");
                        const char *ge = getenv("TD5RE_ARCADE_GRIP_PCT");
                        int tpct = te ? atoi(te) : 125;
                        int gpct = ge ? atoi(ge) : 112;
                        if (tpct < 100) tpct = 100;
                        if (tpct > 250) tpct = 250;
                        if (gpct < 100) gpct = 100;
                        if (gpct > 200) gpct = 200;
                        int32_t tm2 = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);
                        write_i16((uint8_t *)phys, PHYS_DRIVE_TORQUE_MULT, (int16_t)((tm2 * tpct) / 100));
                        int32_t dc2 = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
                        write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)((dc2 * gpct) / 100));
                    }
                }
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
                        write_i16((uint8_t *)phys, PHYS_GEAR_RATIO_BASE + (i + 2) * 2,
                                  (int16_t)(entry + prod));
                    }
                }

                /* (3a) top_speed_limit *= (race_bonus + 0x800) / 2048 */
                {
                    int32_t top = (int32_t)PHYS_S(actor, PHYS_TOP_SPEED);
                    int32_t n1  = (race_bonus + 0x800) * top;
                    write_i16((uint8_t *)phys, PHYS_TOP_SPEED,
                              (int16_t)((n1 + ((n1 >> 31) & 0x7FF)) >> 11));
                }
                /* (3b) gear_ratio_table[gears-1] and [gears-2] top-gear rescale */
                {
                    int32_t denom1 = race_bonus + 0x800;
                    if (denom1 != 0) {
                        int32_t top = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + (gears - 1) * 2);
                        write_i16((uint8_t *)phys, 0x2C + gears * 2,
                                  (int16_t)((top << 11) / denom1));
                    }
                    int32_t denom2 = race_bonus + 0x1000;
                    if (denom2 != 0) {
                        int32_t top2 = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + (gears - 2) * 2);
                        write_i16((uint8_t *)phys, 0x2A + gears * 2,
                                  (int16_t)((top2 << 12) / denom2));
                    }
                }
                /* (3c) damping_low_speed *= (0x200 - race_bonus) / 512 */
                {
                    int32_t dlo = (int32_t)PHYS_S(actor, PHYS_DAMP_COEFF_TURN);
                    int32_t n2  = (0x200 - race_bonus) * dlo;
                    write_i16((uint8_t *)phys, 0x6A,
                              (int16_t)((n2 + ((n2 >> 31) & 0x1FF)) >> 9));
                }

                /* (4) drag_coefficient adjustment via race_points */
                {
                    int32_t drag = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
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
                    write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)(drag + adj));
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
                /* [task #5] Pull a TRAFFIC car's wheels inward on the width axis.
                 * Some TD6-imported traffic models carry cardef wheel X offsets
                 * wider than the chassis, so the wheels render OUTSIDE the body.
                 * For traffic slots the lateral arm wheel_display_angles[w][0]
                 * feeds ONLY the render (the traffic pose integrator derives roll/
                 * pitch from the surface normal + wheel_suspension_pos, never this
                 * field), so scaling it here moves the visual wheels with zero
                 * physics side-effect. Racer slots (which DO use this arm for the
                 * contact frame) are left untouched. Knob TD5RE_TRAFFIC_WHEEL_TRACK
                 * (default 1.0 = no change; e.g. 0.85 narrows). NOTE: traffic
                 * models with NO cardef wheel data are wheeled by a separate
                 * SYNTHESIZED layout in td5_render.c (rb*0.38 half-track) which is
                 * render-only and out of scope for this file. */
                if (slot >= g_traffic_slot_base) {
                    static int   s_twt_init = 0;
                    static float s_twt = 1.0f;
                    if (!s_twt_init) {
                        s_twt = td5_env_float("TD5RE_TRAFFIC_WHEEL_TRACK", 1.0f, 0.1f, 2.0f);
                        s_twt_init = 1;
                        TD5_LOG_I(LOG_TAG, "traffic_wheel_track: TD5RE_TRAFFIC_WHEEL_TRACK=%.3f", s_twt);
                    }
                    if (s_twt != 1.0f) {
                        for (int w = 0; w < 4; w++) {
                            actor->wheel_display_angles[w][0] =
                                (int16_t)((float)actor->wheel_display_angles[w][0] * s_twt);
                        }
                    }
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

/* Per-difficulty-tier scale (numerator over 256) applied to the per-car carparam
 * values sourced into the AI template for racer slots (S06 2026-06-04,
 * [GameOptions] AIAccelFromCar). Tier 0 = Easy is deliberately softened so the
 * "not so fast" opponents are noticeably more forgiving; tier 2 = Hard lets a
 * quick car edge ahead. Difficulty SCALES; the per-vehicle magnitude comes from
 * the car's own carparam.dat.
 *   top    : 0.85 / 0.94 / 1.02  (the player tops out at ~carparam, no diff scale)
 *   torque : 0.60 / 0.75 / 0.875 (eased on Easy; near the template tier scale on Hard)
 */
static const int k_ai_tier_top_pct[3]    = { 0xD9, 0xF0, 0x105 };
static const int k_ai_tier_torque_pct[3] = { 0x99, 0xC0, 0xE0  };

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

    if (slot < g_traffic_slot_base && g_race_slot_state[slot] == 0 &&
        !is_player_is_ai_slot0 &&
        !(g_td5.drag_race_enabled && slot == 1 && s_carparam_loaded[slot])) {
        uint8_t *ai_tmpl = td5_ai_get_physics_template();
        if (ai_tmpl) {
            memcpy(tuning, ai_tmpl, 0x80);
            if (s_carparam_loaded[slot]) {
                memcpy(cardef, s_loaded_cardef[slot], 0x8C);
                /* S06 2026-06-04: source per-car ACCELERATION (drive-torque
                 * +0x68) and TOP SPEED (+0x74) from THIS car's carparam, scaled
                 * by a per-difficulty-tier factor, so different AI cars top out
                 * and accelerate per their own parameters rather than a single
                 * difficulty-only template constant. The bicycle-critical fields
                 * (Wf/Wr/I/half_wb +0x20..0x2B and grip +0x2C) stay on the AI
                 * template — sourcing those from carparam flips the bicycle
                 * determinant sign and spins the car (the documented hazard
                 * above). Gated by [GameOptions] AIAccelFromCar; off = faithful. */
                if (g_td5.ini.ai_accel_from_car) {
                    const int16_t *cp = (const int16_t *)s_loaded_tuning[slot];
                    int tier = g_td5.difficulty_tier;
                    if (tier < 0) tier = 0; else if (tier > 2) tier = 2;
                    int ai_top = FP_TRUNC(((int)cp[0x74 / 2] * k_ai_tier_top_pct[tier]));
                    int ai_dt  = FP_TRUNC(((int)cp[0x68 / 2] * k_ai_tier_torque_pct[tier]));
                    if (ai_top < 1) ai_top = 1;
                    if (ai_dt  < 1) ai_dt  = 1;
                    write_i16(tuning, 0x74, (int16_t)ai_top);  /* top speed */
                    write_i16(tuning, 0x68, (int16_t)ai_dt);   /* drive-torque (accel) */
                }
            }
            actor->tuning_data_ptr = tuning;
            actor->car_definition_ptr = cardef;
            TD5_LOG_I(LOG_TAG,
                      "bind_tuning slot=%d: AI template (Wf=%d Wr=%d I=%d) "
                      "accel_from_car=%d dt=%d top=%d",
                      slot, *(int16_t *)(tuning + 0x28),
                      *(int16_t *)(tuning + 0x2A),
                      *(int32_t *)(tuning + 0x20),
                      g_td5.ini.ai_accel_from_car,
                      (int)*(int16_t *)(tuning + 0x68),
                      (int)*(int16_t *)(tuning + 0x74));
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
    /* [MESH HITBOX 2026-06-25] Invalidate any stale model box + hull for this
     * slot from a previous race up front, so a slot that fails to get a mesh
     * this race cleanly falls back to its (freshly seeded) cardef box instead
     * of reusing a different car's bounds. Re-validated below once the vertex
     * scan succeeds (store fns live in td5_physics_collision.c). */
    td5_physics_hitbox_invalidate(slot);

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

    /* [MESH HITBOX 2026-06-25] Cache the PURE mesh AABB extents (max|x|, max|z|)
     * for the V2V collision box BEFORE the original's +20/-20 padding is applied
     * below. max_x already includes the racer axle-width clamp; max_z is the raw
     * |z| extent. This precise model silhouette is what the collision functions
     * use (via actor_collision_box) in place of the padded+scaled cardef box. The
     * padded 8-corner box keeps being written below for the suspension / AI lane
     * / separation consumers, so they stay byte-faithful. */
    mesh_box_store(slot, (int32_t)(int)max_x, (int32_t)(int)max_z);

    /* [SILHOUETTE HITBOX] Build the precise convex-hull silhouette for V2V.
     * hull_build_store lives in td5_physics_collision.c; declared here (not in
     * td5_physics_internal.h) because TD5_MeshVertex is a render-layer type. */
    {
        extern void hull_build_store(int slot, const TD5_MeshVertex *verts, int n);
        hull_build_store(slot, verts, vert_count);
    }
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
        TD5_LOG_I(LOG_TAG, "silhouette: slot=%d hull_pts=%d (valid=%d) box[hw=%d fz=%d]",
                  slot, td5_physics_hull_points(slot),
                  td5_physics_hull_points(slot) > 0, (int)(int)max_x, (int)(int)max_z);

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
    /* Commit to the race-init flag consumed by td5_physics_init_vehicle_runtime
     * (0x42F140) for gravity + per-car stat scaling. This is the faithful analog
     * of the original's `gDifficultyEasy = gDynamicsConfigShadow` at the frontend
     * transitions (0x004155F2 / 0x0041DC82). The dynamics shadow @0x00466014 is
     * copied verbatim into gDifficultyEasy @0x004AAF84. Mapping (CONFIRMED):
     *   mode 0 = ARCADE     -> Easy=0 -> gravity 1900 (0x76C) + car-stat boosts
     *   mode 1 = SIMULATION -> Easy=1 -> gravity 1500 (0x5DC) + stock stats
     * gDifficultyHard @0x004AAF80 has NO writers in the original, so the port's
     * g_difficulty_hard stays 0 (its HARD branch is dead, matching the orig). */
    g_difficulty_easy = s_dynamics_mode;
    TD5_LOG_I(LOG_TAG, "Dynamics mode set to %s (%d) -> g_difficulty_easy=%d",
              s_dynamics_mode ? "simulation" : "arcade", s_dynamics_mode,
              g_difficulty_easy);
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
        TD5_LOG_I(LOG_TAG, "XZ freeze=%d (g_freezeHorizontalIntegration)", freeze);
        g_xz_freeze = freeze;
    }
}

void td5_physics_set_race_slot_state(int slot, int is_human)
{
    if (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) {
        g_race_slot_state[slot] = is_human;
        TD5_LOG_I(LOG_TAG, "Slot %d physics mode: %s", slot,
                  is_human ? "player" : "AI");
    }
}

void td5_physics_set_slot_series_position(int slot, int position)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (position < 0) position = 0;
    if (position > 3) position = 3;
    g_slot_series_position[slot] = position;
}

void td5_physics_seed_traffic_cardef_from_player(int traffic_slot)
{
    if (traffic_slot < g_traffic_slot_base || traffic_slot >= TD5_MAX_TOTAL_ACTORS)
        return;
    if (!s_carparam_loaded[0]) {
        TD5_LOG_W(LOG_TAG,
                  "seed_traffic_cardef slot=%d: player slot 0 carparam not loaded — skipping",
                  traffic_slot);
        return;
    }

    /* Mirrors orig LoadRaceVehicleAssets @ 0x00443280 traffic copy loop
     * (`for iVar17 = 0x23; *puVar16 = *puVar6` at 0x004436C6-0x004436D7).
     * Orig copies 0x23 dwords = 0x8C bytes = full cardef from slot 0 into each
     * traffic slot's cardef row. Tuning is NOT copied — orig's traffic motion
     * path (UpdateTrafficActorMotion @ 0x00443ED0) uses hardcoded constants
     * for friction/throttle and does not read per-slot tuning. */
    memcpy(s_loaded_cardef[traffic_slot], s_loaded_cardef[0], 0x8C);
    s_carparam_loaded[traffic_slot] = 1;
    TD5_LOG_I(LOG_TAG,
              "seed_traffic_cardef slot=%d: copied slot-0 cardef "
              "(height_offset=%d half_w=%d half_l=%d sphere_r=%d)",
              traffic_slot,
              (int)*(int16_t *)(s_loaded_cardef[traffic_slot] + 0x86),
              (int)*(int16_t *)(s_loaded_cardef[traffic_slot] + 0x0C),
              (int)*(int16_t *)(s_loaded_cardef[traffic_slot] + 0x08),
              (int)*(int16_t *)(s_loaded_cardef[traffic_slot] + 0x80));
}

/* Per-slot carparam top-speed (tuning +0x74), or -1 if no carparam.dat was
 * loaded for the slot. Lets td5_ai.c derive a per-car traffic cruise throttle
 * from the vehicle's own top speed. [S06 2026-06-04 accel/top-from-car] */
int td5_physics_get_carparam_top_speed(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return -1;
    if (!s_carparam_loaded[slot]) return -1;
    return (int)*(const int16_t *)(s_loaded_tuning[slot] + 0x74);
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

/* ========================================================================
 * Vehicle Recovery Animation (vehicle_mode==1) port
 *
 * Tier 2 of the NOT_PORTED triage (2026-05-24) — restores the scripted
 * recovery flow that runs after a heavy collision. Five functions, all
 * confined to the vehicle_mode==1 dispatch:
 *
 *   0x0042E030 ExtractEulerAnglesFromMatrix       — Euler decomp w/ gimbal-lock
 *   0x00409BF0 RefreshScriptedVehicleTransforms   — extract + rebuild matrices
 *   0x00409520 CheckAndUpdateActorCollisionAlignment — heading-aligned reset gate
 *   0x004096B0 ComputeActorWorldBoundingVolume    — wall-impact response impulse
 *   0x004092D0 RenderVehicleActorModel (misnamed) — 4 wheel + 4 body probe pass
 *
 * Wire-up: ext call chain rooted at td5_physics_integrate_scripted_motion
 * which is invoked from the vehicle_mode==1 branch of UpdateVehicleActor
 * (td5_physics_update_vehicle_actor below). The branch previously had a
 * minimal-fidelity port (gravity + linear damping + frame-counter timeout);
 * this replaces it with the byte-faithful chain.
 *
 * Traffic-path note: orig 0x00443ED0 UpdateTrafficActorMotion has a SECOND
 * caller of RefreshScriptedVehicleTransforms in its cVar3==1 branch (traffic
 * scripted recovery). The port's dispatch at td5_physics_update_vehicle_actor
 * is slot-agnostic on vehicle_mode==1 and WOULD service traffic too, but the
 * upstream WRITER that sets vehicle_mode=1 + collision_spin_matrix on heavy
 * traffic impact (orig 0x4079C0 heavy-impact branch for slot>=6) is NOT
 * ported. As a result traffic slots never enter scripted recovery in the
 * port — see DEFERRED block at the vehicle_mode==1 dispatch site for the
 * full deferral rationale and future-work checklist. [2026-05-25]
 * ======================================================================== */

/* Probe-depth scratch storage corresponding to orig DAT_0x00483958 (32 bytes,
 * 4 wheel probe Y depths) and DAT_0x00483968 (16 bytes, 4 body probe Y depths).
 * In the orig binary these were `int32_t depth[8]` globals that the per-probe
 * walker pass at 0x004092D0 wrote and 0x004096B0 read. Port-side they live as
 * a file-static scratch since their lifetime is one call chain (refresh ->
 * compute_volume). [CONFIRMED @ 0x00483958 / 0x00483968 — 32+16 byte zeroed
 * .data block, only refs are from 0x00409356/0x004094CA writers and
 * 0x004096D7 readers per Ghidra reference_to.] */
static int32_t s_scripted_wheel_probe_depth[4];  /* mirror DAT_00483958[4] */
static int32_t s_scripted_body_probe_depth[4];   /* mirror DAT_00483968[4] */

/* [CONFIRMED @ 0x0042E030] ExtractEulerAnglesFromMatrix
 *
 * Decomposes a 3x4 (3x3 row-major + translation) rotation matrix into three
 * 12-bit display-angle Euler values (roll, yaw, pitch). The orig uses the
 * float trig LUT (4096 scale = full circle), with a gimbal-lock fall-through
 * when the extracted yaw lands on 0x400 (90 deg pitch) — in that case the
 * yaw axis is degenerate so the roll axis absorbs the rotation and pitch
 * is zeroed.
 *
 * Matrix layout (row-major, 4-float row stride from `param_1 + 0xN`):
 *   [+0x00 m00] [+0x04 m01] [+0x08 m02]
 *   [+0x0C m10] [+0x10 m11] [+0x14 m12]   ← yaw extracted from m10,m11,m12
 *   [+0x18 m20] [+0x1C m21] [+0x20 m22]
 *
 * NOTE: Orig multiplies floats by _DAT_0045d69c (= -4096.0f) and by
 * _DAT_0045d604 (g_audioDopplerSpeedOfSound = 4096.0f). Both constants are
 * 4096-magnitude trig scalars that match the 12-bit angle space; the sign of
 * _DAT_0045d69c puts the m12 component into the AngleFromVector12 quadrant
 * convention. Port keeps the orig 0x0045d69c sign by negating m12 once. */
void td5_physics_extract_euler_angles_from_matrix(const float *matrix,
                                                  int16_t *out_roll_yaw_pitch)
{
    /* iVar2 = (int)ROUND(m12 * -4096.0f)  [0x0042E030-43] */
    int32_t neg_m12_scaled = (int32_t)lrintf(matrix[5] * -4096.0f);
    /* dz = (int)ROUND(sqrt(m11^2 + m10^2) * 4096.0f)  [0x0042E044-78] */
    float fm10 = matrix[4];
    float fm11 = matrix[3];
    float xz_mag = sqrtf(fm11 * fm11 + fm10 * fm10);
    int32_t dz = (int32_t)lrintf(xz_mag * 4096.0f);

    /* yaw axis: AngleFromVector12(neg_m12_scaled, dz)  [0x0042E082-9C] */
    uint32_t yaw12 = (uint32_t)AngleFromVector12(neg_m12_scaled, dz);
    out_roll_yaw_pitch[0] = (int16_t)yaw12;

    /* Gimbal-lock detection — orig checks `(yaw & 0x7FF) == 0x400` which
     * fires when the pitch component places m12 exactly at sin(+/-90 deg). */
    if ((yaw12 & 0x7FF) != 0x400) {
        /* Roll = AngleFromVector12(m02 * 4096, m20 * 4096) [m02=matrix[2], m20=matrix[8]] */
        int32_t r0 = (int32_t)lrintf(matrix[2] * 4096.0f);
        int32_t r1 = (int32_t)lrintf(matrix[8] * 4096.0f);
        out_roll_yaw_pitch[1] = (int16_t)AngleFromVector12(r0, r1);

        /* Pitch = AngleFromVector12(m11 * 4096, m10 * 4096)  [m11=matrix[3], m10=matrix[4]] */
        int32_t p0 = (int32_t)lrintf(matrix[3] * 4096.0f);
        int32_t p1 = (int32_t)lrintf(matrix[4] * 4096.0f);
        out_roll_yaw_pitch[2] = (int16_t)AngleFromVector12(p0, p1);
        return;
    }

    /* Gimbal-lock branch: roll absorbs the remaining rotation, pitch=0.
     * The orig passes (iVar2, dz) again — same axis as the yaw extraction —
     * so roll = yaw in this case. */
    out_roll_yaw_pitch[1] = (int16_t)AngleFromVector12(neg_m12_scaled, dz);
    out_roll_yaw_pitch[2] = 0;
}

/* [CONFIRMED @ 0x00409BF0] RefreshScriptedVehicleTransforms
 *
 * Extracts Euler angles from both the primary rotation matrix and the
 * recovery_target (collision_spin) matrix, writes them to display_angles
 * and angular_velocity respectively, then (gated on the replay-skip flag)
 * rebuilds both matrices from the freshly-extracted angles.
 *
 * The gate at 0x00409C58 reads `g_raceParticlePoolBase[0x1eb].view_x._0_1_ & 3`
 * which corresponds to the replay-mode flag block. The port uses
 * td5_game_is_replay_active() as the equivalent gate — when replay is
 * playing back, the matrices are NOT rebuilt (replay supplies them). */
void td5_physics_refresh_scripted_vehicle_transforms(TD5_Actor *actor)
{
    if (!actor) return;

    /* Extract Euler from primary rotation matrix → display_angles  [0x00409C04] */
    int16_t *disp = &actor->display_angles.roll;
    td5_physics_extract_euler_angles_from_matrix(actor->rotation_matrix.m, disp);

    /* Copy collision_spin_matrix → stack buffer, extract Euler → ang_vel  [0x00409C16-49].
     * Orig copies 12 floats (the 3x3 + 3-float translation tail) to the stack
     * before calling Extract. The Extract helper only reads matrix[2..8] so the
     * copy is just for stack-local mutability later (rebuild also uses it). */
    float spin[12];
    memcpy(spin, &actor->collision_spin_matrix, 12 * sizeof(float));
    int16_t ang_vel_angles[3];
    td5_physics_extract_euler_angles_from_matrix(spin, ang_vel_angles);
    actor->angular_velocity_roll  = (int32_t)ang_vel_angles[0];
    actor->angular_velocity_yaw   = (int32_t)ang_vel_angles[1];
    actor->angular_velocity_pitch = (int32_t)ang_vel_angles[2];

    /* Replay-skip gate [CONFIRMED @ 0x00409C58]: orig tests
     *   (g_raceParticlePoolBase[0x1eb].view_x._0_1_ & 3) == 0
     * which corresponds to the playback-active flag block. Port mirrors
     * via td5_game_is_replay_active() — when playback is active, matrices
     * are sourced from the replay stream rather than being rebuilt. */
    if (td5_game_is_replay_active())
        return;

    /* Rebuild rotation_matrix from display_angles  [0x00409C66-A2].
     * BuildRotationMatrixFromAngles writes 9 floats; the orig then copies all
     * 12 floats (9 rotation + 3 translation tail) into saved_orientation so
     * the trailing 3 dwords sweep render_pos in. The port mirrors that exact
     * 12-dword copy semantics. */
    BuildRotationMatrixFromAngles(actor->rotation_matrix.m, disp);
    /* Orig pattern: memcpy 48B from rotation_matrix → saved_orientation
     * (9 rot floats + 3 render_pos floats). Mirrors the ClampVehicleAttitudeLimits
     * tail copy at 0x00405D2B. */
    memcpy(&actor->saved_orientation, &actor->rotation_matrix, 9 * sizeof(float));
    memcpy(((uint8_t *)&actor->saved_orientation) + 9 * sizeof(float),
           &actor->render_pos, 3 * sizeof(float));

    /* Rebuild collision_spin_matrix from ang_vel_angles  [0x00409CA4-CD0]. */
    BuildRotationMatrixFromAngles(spin, ang_vel_angles);
    memcpy(&actor->collision_spin_matrix, spin, 9 * sizeof(float));
    /* Trailing 3 dwords: orig copies stack residue (the local_30 buffer
     * follows the spin matrix on the stack, so its first 3 floats land in
     * the gap_1A4 region). Port writes spin[0..2] which is the equivalent
     * post-call source value. */
    memcpy(((uint8_t *)&actor->collision_spin_matrix) + 9 * sizeof(float),
           spin, 3 * sizeof(float));
}

/* [CONFIRMED @ 0x00409520] CheckAndUpdateActorCollisionAlignment
 *
 * Tail check inside ComputeActorWorldBoundingVolume — once enough frames
 * have passed (frame_counter > 2), test whether the vehicle's body axis is
 * now closely aligned with its heading_normal AND ang_vel is small. If the
 * combined (roll-bin | pitch-bin) sextant is "wheels down" (bin 0) or
 * "wheels up" (bin 10), trigger ResetVehicleActorState to exit scripted mode.
 *
 * Magic constants from listing:
 *   0x18  — angle delta threshold (~2.1 deg in 12-bit space)
 *   0x20  — ang_vel threshold (~1.7 deg/frame)
 *   `(angle1 + 0x200) >> 2 & 0x300 | (angle2 + 0x200) & 0xc00` >> 8 — quadrant
 *   bin: 0 (both up) or 10 (both inverted) accept; everything else reject. */
static void td5_physics_check_and_update_actor_collision_alignment(TD5_Actor *actor)
{
    if (!actor) return;
    if (actor->frame_counter <= 2) return;

    int16_t yaw_disp = actor->display_angles.yaw;
    int32_t cos_y = CosFixed12bit((int)yaw_disp);
    int32_t sin_y = SinFixed12bit((int)yaw_disp);

    int16_t hx = actor->heading_normal.x;
    int16_t hy = actor->heading_normal.y;
    int16_t hz = actor->heading_normal.z;

    /* Project heading into body-relative XZ via yaw rotation [0x004095AC-D2] */
    int32_t bx = (int32_t)hx * cos_y - (int32_t)hz * sin_y;
    int32_t bz = (int32_t)hx * sin_y + (int32_t)hz * cos_y;

    /* Round-toward-zero >> 12, then narrow to int16  [SAR_RZ_12 idiom] */
    bz = (int32_t)(int16_t)SAR_RZ_12(bz);
    int32_t hy_i = (int32_t)hy;

    /* iVar3 = AngleFromVector12(-bz, -hy)  [0x004095E0-EE] */
    int32_t roll_target = AngleFromVector12(-bz, -hy_i);

    /* iVar2 = AngleFromVector12(bx_rz12, sqrt(bz^2 + hy^2))  [0x004095EF-617] */
    bx = (int32_t)(int16_t)SAR_RZ_12(bx);
    float mag_sq = (float)(bz * bz + hy_i * hy_i);
    int32_t mag = (int32_t)lrintf(sqrtf(mag_sq));
    int32_t pitch_target = AngleFromVector12(bx, mag);

    /* Delta = signed wrap of (target - current)  [0x00409621-37] */
    int32_t droll  = roll_target  - actor->display_angles.roll;
    int32_t dpitch = pitch_target - actor->display_angles.pitch;

    /* Wrap to [-0x200, 0x200) via (x - 0x200 & 0x3FF) - 0x200  */
    int32_t wroll  = (int32_t)((droll  - 0x200u) & 0x3FF) - 0x200;
    int32_t wpitch = (int32_t)((dpitch - 0x200u) & 0x3FF) - 0x200;
    int32_t aroll  = (wroll  < 0) ? -wroll  : wroll;
    int32_t apitch = (wpitch < 0) ? -wpitch : wpitch;

    if (aroll >= 0x18 || apitch >= 0x18) return;

    /* Ang_vel must be near zero: wrap to [-0x800, 0x800) and check |.| < 0x20.
     * Orig only checks roll + pitch components. */
    int32_t avr = (actor->angular_velocity_roll  - 0x800) & 0xFFF;
    int32_t avp = (actor->angular_velocity_pitch - 0x800) & 0xFFF;
    int32_t wavr = avr - 0x800;
    int32_t wavp = avp - 0x800;
    if (wavr >= 0x20 || wavr <= -0x20) return;
    if (wavp >= 0x20 || wavp <= -0x20) return;

    /* Quadrant bin test [0x00409686-9A]:
     *   bin = ((roll_target + 0x200) >> 2 & 0x300) | ((pitch_target + 0x200) & 0xC00), then >> 8.
     * Orig uses `iVar3 + 0x200 >> 2 & 0x300` for the roll axis (bit 0x300)
     * and `iVar2 + 0x200 & 0xc00` for the pitch axis (bit 0xc00). */
    int32_t bin = (int32_t)FP_TRUNC(((((uint32_t)(roll_target  + 0x200) >> 2) & 0x300u)
                          | ((uint32_t)(pitch_target + 0x200) & 0xC00u)));

    if (bin == 0 || bin == 10) {
        td5_physics_reset_actor_state(actor);
    }
}

/* [CONFIRMED @ 0x004092D0] RenderVehicleActorModel (orig misnamed —
 * actually the scripted-mode probe pass that walks 4 wheel hubs + 4 body
 * corners through track contact, writes wheel_world_positions_hires,
 * bbox_vertices_upper, and the 8 probe depth values, then returns a
 * bitmask of probes whose Y is below the local ground plane.
 *
 * Mirrors td5_physics_refresh_wheel_contacts but without suspension —
 * scripted mode integrates a precomputed transform instead of computing
 * spring deflection. */
static uint32_t td5_physics_render_vehicle_actor_model(TD5_Actor *actor)
{
    if (!actor || !actor->car_definition_ptr)
        return 0;

    uint32_t below_ground_mask = 0;
    int16_t span = actor->track_span_raw;
    int8_t  sub_lane = (int8_t)actor->track_sub_lane_index;
    const uint8_t *cardef = (const uint8_t *)actor->car_definition_ptr;

    /* Set up render transform from rotation_matrix + render_pos  [0x0040930E-19] */
    LoadRenderRotationMatrix((float *)&actor->rotation_matrix);
    /* The orig CALL 0x43DC20 (LoadRenderTranslation) is passed `&actor->rotation_matrix`,
     * which is the same pointer used for LoadRenderRotationMatrix — Ghidra-typed as a
     * Vec3f. That's a pointer-cast bug in the orig decomp display; the actual
     * effect is to load translation from render_pos via the render module's
     * stashed matrix. Mirror by loading render_pos. */
    td5_render_load_translation((const TD5_Vec3f *)&actor->render_pos);

    /* Build a local 12-float matrix for transform helper. */
    float matrix[12];
    memcpy(matrix, actor->rotation_matrix.m, 9 * sizeof(float));
    matrix[9]  = actor->render_pos.x;
    matrix[10] = actor->render_pos.y;
    matrix[11] = actor->render_pos.z;

    int max_sp = td5_track_get_span_count();

    /* Loop 1 — 4 wheel hubs  [0x00409370-46B].
     * Reads cardef[+0x20+i*8 .. +0x26] as a short[3] wheel-hub offset; the
     * wheel_display_angle short at offset +0x42+i*8 in cardef is the per-wheel
     * ride-height ceiling. Subtracts `wheel_suspension_pos[i] >> 8` (SAR-RZ)
     * before second transform. */
    for (int i = 0; i < 4; i++) {
        int16_t wheel_off[3] = {
            *(int16_t *)(cardef + 0x20 + i * 8 + 0),
            *(int16_t *)(cardef + 0x20 + i * 8 + 2),
            *(int16_t *)(cardef + 0x20 + i * 8 + 4),
        };
        int32_t out[3];

        /* Seed wheel probe with chassis span/sub_lane */
        actor->wheel_probes[i].span_index = span;
        actor->wheel_probes[i].sub_lane_index = sub_lane;

        /* First transform: chassis-frame wheel offset → world (FISTP-rounded). */
        td5_transform_short_vec3_by_render_matrix_rounded(wheel_off, out, matrix);

        /* Compute body-corner short[1] = cardef[+0x42 + i*8] - (wheel_susp_pos[i] >> 8)
         * SAR_RZ_8 matches orig's CDQ;AND 0xFF;ADD;SAR 8 idiom. */
        int16_t body_y = (int16_t)((int16_t)*(int16_t *)(cardef + 0x42 + i * 8) -
                                   (int16_t)SAR_RZ_8(actor->wheel_suspension_pos[i]));
        int16_t body_corner_off[3] = {
            *(int16_t *)(cardef + 0x20 + i * 8 + 0),
            body_y,
            *(int16_t *)(cardef + 0x20 + i * 8 + 4),
        };

        int32_t body_out[3];
        td5_transform_short_vec3_by_render_matrix_rounded(body_corner_off, body_out, matrix);

        /* Pre-shift << 8 to convert int32 → 24.8 fp before track probe lookup.
         * Orig stores wheel_world_positions_hires[i] (= probe ring at +0x298) with
         * shifted values and the body-corner triple separately. */
        int32_t wx_fp = FP_SCALE(body_out[0]);
        int32_t wy_fp = FP_SCALE(body_out[1]);
        int32_t wz_fp = FP_SCALE(body_out[2]);
        actor->wheel_world_positions_hires[i].x = wx_fp;
        actor->wheel_world_positions_hires[i].y = wy_fp;
        actor->wheel_world_positions_hires[i].z = wz_fp;

        out[0] <<= 8;
        out[1] <<= 8;
        out[2] <<= 8;

        /* UpdateActorTrackPosition(probe, &wx_fp, &out, &depth_slot)
         * Orig signature is `(short *probe, int pos_xyz_ptr, int pos2_ptr, int depth_ptr, int unused)`
         * — the walker actually only uses the first pos arg and the depth out
         * pointer. Port simplification: walk probe based on body-corner X/Z. */
        td5_track_update_probe_position(&actor->wheel_probes[i], wx_fp, wz_fp);
        if (max_sp > 0 && actor->wheel_probes[i].span_index >= (int16_t)max_sp)
            actor->wheel_probes[i].span_index = (int16_t)(max_sp - 1);
        td5_track_compute_probe_contact_vertices(&actor->wheel_probes[i]);

        /* ComputeActorTrackContactNormal writes the depth (ground Y) at probe XZ.
         * Use the existing port's signature: (probe, pos_xyz_ptr, out_y_ptr).
         * The orig 0x00445450 writes its ground-Y into the dword pointed to by
         * its 3rd arg — port's variant has the same effect. */
        int32_t probe_pos[3] = { wx_fp, wy_fp, wz_fp };
        int32_t ground_y = 0;
        ComputeActorTrackContactNormal((short *)&actor->wheel_probes[i],
                                       probe_pos, &ground_y);
        s_scripted_wheel_probe_depth[i] = ground_y;

        /* If body-corner Y is below ground, set the probe-i bit. */
        if (wy_fp - ground_y < 0)
            below_ground_mask |= (1u << i);
    }

    /* Loop 2 — 4 body corner sample positions at cardef[+0x40 + i*8] (4-short pairs).
     * Uses body_probes[0..3] (the +0x40 region) and writes
     * collision_spin_matrix-derived corner positions into bbox_vertices_upper.
     *
     * NOTE: the orig reads cardef from `puVar6 + 0x20` which after the first
     * loop is incremented by 8 each iter, so the BASE for loop 2 is
     * `cardef + 0x40` (puVar6 was `cardef`, then incremented 4 times in loop 1
     * yielding `cardef + 0x20`, then `+ 0x20` to give `cardef + 0x40`). */
    for (int i = 0; i < 4; i++) {
        int16_t corner_off[3] = {
            *(int16_t *)(cardef + 0x40 + i * 8 + 0),
            *(int16_t *)(cardef + 0x40 + i * 8 + 2),
            *(int16_t *)(cardef + 0x40 + i * 8 + 4),
        };

        actor->body_probes[i].span_index = span;
        actor->body_probes[i].sub_lane_index = sub_lane;

        int32_t out[3];
        td5_transform_short_vec3_by_render_matrix_rounded(corner_off, out, matrix);
        int32_t wx_fp = FP_SCALE(out[0]);
        int32_t wy_fp = FP_SCALE(out[1]);
        int32_t wz_fp = FP_SCALE(out[2]);

        /* Mirror to bbox_vertices_upper[i] (collision_spin output target). */
        actor->bbox_vertices_upper[i].x = wx_fp;
        actor->bbox_vertices_upper[i].y = wy_fp;
        actor->bbox_vertices_upper[i].z = wz_fp;

        td5_track_update_probe_position(&actor->body_probes[i], wx_fp, wz_fp);
        if (max_sp > 0 && actor->body_probes[i].span_index >= (int16_t)max_sp)
            actor->body_probes[i].span_index = (int16_t)(max_sp - 1);
        td5_track_compute_probe_contact_vertices(&actor->body_probes[i]);

        int32_t probe_pos[3] = { wx_fp, wy_fp, wz_fp };
        int32_t ground_y = 0;
        ComputeActorTrackContactNormal((short *)&actor->body_probes[i],
                                       probe_pos, &ground_y);
        s_scripted_body_probe_depth[i] = ground_y;

        if (wy_fp - ground_y < 0)
            below_ground_mask |= (1u << (i + 4));
    }

    return below_ground_mask;
}

/* [CONFIRMED @ 0x0042E750] BuildWorldToViewMatrix — row-major rotation matrix of
 * `angle` about `axis` (int[3]; normalized INTERNALLY, do NOT pre-normalize). The
 * angle is a fixed-point value whose top 12-bit field (angle>>0x12) indexes the
 * float trig table (0x1000 = 2pi). Realized as B . Z . B^T (align axis, twist,
 * un-align). Every element + sign transcribed verbatim from the listing
 * (FCHS/FSTP sites @0x0042E7CF-0x0042E92E), x=axis[0] y=axis[1] z=axis[2],
 * r=sqrt(y^2+z^2), R=sqrt(x^2+y^2+z^2), c=cos, s=sin:
 *   B  = { r/R, 0, x/R,  -(y*x)/(R*r), z/r, y/R,  -(z*x)/(R*r), -(y/r), z/R }
 *   Z  = { c, s, 0,  -s, c, 0,  0, 0, 1 }
 *   B^T= transpose(B);  out = (B . Z) . B^T
 * r==0 (axis pure-X): yaw matrix { 1,0,0, 0,c,-s, 0,s,c }, s negated when x<0
 * [@0x0042E968/0x0042E977]. R<=0 (degenerate): identity [@0x0042E937]. */
static void td5_build_world_to_view_matrix(float *out, const int32_t axis[3], int32_t angle)
{
    int32_t a12 = angle >> 0x12;                  /* SAR angle,0x12 [@0x0042E75E] */
    float c = CosFloat12bit((unsigned int)a12);
    float s = SinFloat12bit(a12);

    float x = (float)axis[0];
    float y = (float)axis[1];
    float z = (float)axis[2];

    float r = sqrtf(y * y + z * z);
    if (r == 0.0f) {
        float ss = (axis[0] < 0) ? -s : s;        /* x<0 sign flip [@0x0042E977] */
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        out[3] = 0.0f; out[4] = c;    out[5] = -ss;
        out[6] = 0.0f; out[7] = ss;   out[8] = c;
        return;
    }

    float R = sqrtf(x * x + y * y + z * z);
    if (R <= 0.0f) {
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
        out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;
        return;
    }

    float r_R  = r / R;
    float x_R  = x / R;
    float y_R  = y / R;
    float z_R  = z / R;
    float z_r  = z / r;
    float ny_r = -(y / r);
    float nyx  = -(y * x) / (R * r);
    float nzx  = -(z * x) / (R * r);

    float B[9]  = { r_R,  0.0f, x_R,
                    nyx,  z_r,  y_R,
                    nzx,  ny_r, z_R };
    float Z[9]  = {  c,    s,   0.0f,
                    -s,    c,   0.0f,
                    0.0f, 0.0f, 1.0f };
    float BT[9] = { r_R,  nyx,  nzx,
                    0.0f, z_r,  ny_r,
                    x_R,  y_R,  z_R };
    float tmp[9];
    MultiplyRotationMatrices3x3(B, Z, tmp);       /* B . Z      [@0x0042E8C6] */
    MultiplyRotationMatrices3x3(tmp, BT, out);    /* (B.Z) . BT [@0x0042E928] */
}

/* [CONFIRMED @ 0x004096B0] ComputeActorWorldBoundingVolume
 *
 * Wall-impact response: averages the contact positions of all probes that hit
 * the ground (per `view_mask` bitmask from RenderVehicleActorModel), computes
 * a penetration-resolution impulse, applies it to linear_velocity, and lifts
 * world_pos.y by the deepest probe penetration. Also triggers contact SFX:
 *   - heavy thud (sound 0x17) when downward-into-ground velocity exceeds 4000
 *   - rolling scrape (sound 0x16) for racers (slot < 6) with vy < -400.
 *
 * Tail-calls CheckAndUpdateActorCollisionAlignment which may exit recovery
 * if the actor is now aligned + still. */
static void td5_physics_compute_actor_world_bounding_volume(TD5_Actor *actor,
                                                            uint32_t view_mask)
{
    if (!actor) return;

    /* Magic float constants (resolved from .data at 0x0045D5EC..0x0045D604):
     *   c_xz_eps      = 3.10254e-9f  (0x315555a9 @ 0x0045D5EC) — XZ vel epsilon
     *   c_pen_scale   = 1.0625f      (0x3F880000 @ 0x0045D5F0) — penetration scale
     *   c_min_eps     = 1.0f         (0x3F800000 @ 0x0045D5F4 = g_audioMinDistanceEpsilon)
     *   c_drag_quad_a = 200000.0f    (0x48435000 @ 0x0045D5F8)
     *   c_drag_quad_b = -212500.0f   (0xC84F8500 @ 0x0045D5FC)
     *   c_xz_thresh   = 128.0f       (0x43000000 @ 0x0045D600) — XZ magnitude gate
     *   c_4096        = 4096.0f      (0x45800000 @ 0x0045D604 = g_audioDopplerSpeedOfSound)
     *   c_yaw_factor  = 3.339476f    (0x4055A5B9 @ 0x00463208) — yaw twist scalar
     *   c_neg_4096    = -4096.0f     (0xC5800000 @ 0x0045D69C) */
    static const float c_pen_scale   = 1.0625f;
    static const float c_min_eps     = 1.0f;
    static const float c_drag_quad_a = 200000.0f;
    static const float c_drag_quad_b = -212500.0f;
    static const float c_xz_thresh   = 128.0f;
    static const float c_yaw_factor  = 3.339476f;
    /* g_fp8ToFloatScale (1/256) is defined elsewhere in render module; reuse
     * via the standard FP_TO_FLOAT macro. */

    /* Accumulate average probe position (in /16 fp units) and deepest dip
     * for whichever probes are flagged in view_mask. */
    int32_t avg_x = 0, avg_y = 0, avg_z = 0;
    int32_t lift_y = 0;
    int active = 0;

    /* Probes 0..3 = wheel probes (depth at s_scripted_wheel_probe_depth[i]);
     * probes 4..7 = body probes (depth at s_scripted_body_probe_depth[i-4]).
     * Orig walks pointers piVar5 = &actor->probe_FL_y (= wheel_world_positions_hires[0].y
     * in port terms) and piVar3 = &DAT_00483958 (= s_scripted_wheel_probe_depth here). */
    int down_proj = 0;
    for (int i = 0; i < 4; i++) {
        if (!(view_mask & (1u << i))) continue;
        TD5_Vec3_Fixed *wpos = &actor->wheel_world_positions_hires[i];
        avg_x += (wpos->x - actor->world_pos.x) >> 4;
        avg_y += (wpos->y - actor->world_pos.y) >> 4;
        avg_z += (wpos->z - actor->world_pos.z) >> 4;
        int32_t dip = s_scripted_wheel_probe_depth[i] - wpos->y;
        if (dip > lift_y) lift_y = dip;
        active++;
    }
    /* Body-corner probes (bits 4..7). Orig second loop walks
     * &actor->bbox_vertices_upper[i].y and DAT_00483968 with same structure. */
    for (int i = 0; i < 4; i++) {
        if (!(view_mask & (1u << (i + 4)))) continue;
        TD5_Vec3_Fixed *bpos = &actor->bbox_vertices_upper[i];
        avg_x += (bpos->x - actor->world_pos.x) >> 4;
        avg_y += (bpos->y - actor->world_pos.y) >> 4;
        avg_z += (bpos->z - actor->world_pos.z) >> 4;
        int32_t dip = s_scripted_body_probe_depth[i] - bpos->y;
        if (dip > lift_y) lift_y = dip;
        active++;
    }

    if (active > 0) {
        avg_x /= active;
        avg_y /= active;
        avg_z /= active;

        /* Transform avg into body-relative orientation via recovery_target
         * (collision_spin) matrix. ConvertFloatVec3ToIntVec3B is a 3x3 mat-vec
         * multiply that FILDs the int input and writes int16-truncated outputs.
         *
         * Port simplification: do the math inline since we have the matrix
         * already accessible. */
        const float *spin = actor->collision_spin_matrix.m;
        int16_t local_v[3];
        /* ConvertFloatVec3ToIntVec3B (0x0042DC30): row-major mat·vec, __ftol
         * TRUNCATES toward zero — prior port used lrintf (round). [CONFIRMED] */
        local_v[0] = (int16_t)(int32_t)(spin[0]*(float)avg_x + spin[1]*(float)avg_y + spin[2]*(float)avg_z);
        local_v[1] = (int16_t)(int32_t)(spin[3]*(float)avg_x + spin[4]*(float)avg_y + spin[5]*(float)avg_z);
        local_v[2] = (int16_t)(int32_t)(spin[6]*(float)avg_x + spin[7]*(float)avg_y + spin[8]*(float)avg_z);

        /* iVar1 (local_1c..local_14) = local_v + (lin_vel/16 - avg) */
        int32_t l_x = (int32_t)local_v[0] + ((actor->linear_velocity_x >> 4) - avg_x);
        int32_t l_y = (int32_t)local_v[1] + ((actor->linear_velocity_y >> 4) - avg_y);
        int32_t l_z = (int32_t)local_v[2] + ((actor->linear_velocity_z >> 4) - avg_z);

        /* XZ magnitude check  [0x00409808-21] */
        float fav_x = (float)avg_x;
        float fav_z = (float)avg_z;
        float xz_mag = (float)lrintf(sqrtf(fav_x * fav_x + fav_z * fav_z));

        /* Body-relative downward velocity into ground (using heading_normal as
         * approximate body-Y axis): -(hx*l_x + hy*l_y + hz*l_z) >> 12 */
        int16_t hx = actor->heading_normal.x;
        int16_t hy = actor->heading_normal.y;
        int16_t hz = actor->heading_normal.z;
        /* [FIX] orig 0x004096B0 pairs hn.x*l_x + hn.y*l_y + hn.z*l_z; prior port
         * had hy*l_z + hz*l_y swapped. */
        down_proj = -((int32_t)hx * l_x +
                      (int32_t)hy * l_y +
                      (int32_t)hz * l_z) >> 12;

        float pen_force = 0.0f;
        if (down_proj < 0) {
            if (down_proj < -4000) {
                int32_t world_pos[3] = { actor->world_pos.x, actor->world_pos.y, actor->world_pos.z };
                td5_sound_play_at_position(0x17, 0x400, 0x5622, world_pos, 4);
            }

            /* Staged spin matrix; committed to collision_spin (+0x180) below.
             * [ROLL-ANIM FIX 2026-05-30] Branch gating was SWAPPED in the prior
             * port. Orig 0x004096B0: xz_mag<=128 -> quadratic pen + cos/sin twist;
             * xz_mag>128 -> pen = -down*1.0625, no twist. */
            float d0[9];
            if (xz_mag <= c_xz_thresh) {
                /* === cos/sin twist branch — the continuous-roll driver.
                 * [CONFIRMED instruction-level @ 0x004098A5-0x004099B6]
                 *   spin_new = (A1 · B1 · A1^T) · spin_old
                 *   A1  = [[nz,0, nx],[0,1,0],[-nx,0,nz]]  (yaw from norm. avg dir)
                 *   B1  = [[1,0,0],[0,c,s],[0,-s,c]]        (X-axis impact twist)
                 *   A1T = A1 transpose
                 *   nz = avg_z/xz_mag, nx = avg_x/xz_mag, c=cos(yaw_q), s=sin(yaw_q)
                 *   pen = (down * c_drag_quad_b) / (xz_mag^2/256 + 200000) */
                pen_force = ((float)down_proj * c_drag_quad_b) /
                            (xz_mag * xz_mag * (1.0f / 256.0f) + c_drag_quad_a);
                int32_t yaw_q = ((int32_t)lrintf(c_yaw_factor * pen_force * xz_mag)) >> 0x12;
                float c = CosFloat12bit((uint32_t)yaw_q);
                float s = SinFloat12bit(yaw_q);
                float n  = c_min_eps / xz_mag;   /* 1.0 / xz_mag */
                float nx = n * fav_x;
                float nz = n * fav_z;

                float A1[9]  = { nz, 0.0f,  nx,   0.0f, 1.0f, 0.0f,  -nx, 0.0f, nz };
                float B1[9]  = { 1.0f, 0.0f, 0.0f, 0.0f, c,   s,      0.0f, -s,  c  };
                float A1T[9] = { nz, 0.0f, -nx,   0.0f, 1.0f, 0.0f,   nx, 0.0f, nz };
                float m1[9], m2[9];
                MultiplyRotationMatrices3x3(A1, B1, m1);    /* m1 = A1·B1 */
                MultiplyRotationMatrices3x3(m1, A1T, m2);   /* m2 = A1·B1·A1^T */
                MultiplyRotationMatrices3x3(m2, (float *)&actor->collision_spin_matrix, d0); /* d0 = twist·spin */
            } else {
                /* xz_mag>128: pen = -down*1.0625 [0x004099D3]; no twist.
                 * Stage d0 = spin so the lateral<1 commit is identity. */
                pen_force = (float)(-down_proj) * c_pen_scale;
                memcpy(d0, actor->collision_spin_matrix.m, 9 * sizeof(float));
            }

            /* lateral_mag = round(sqrt(l_x^2 + l_z^2)) [CONFIRMED FISTP round]. */
            int32_t lateral_mag = (int32_t)lrintf(sqrtf((float)(l_x * l_x + l_z * l_z)));
            if (lateral_mag < 1) {
                /* Commit staged twist to collision_spin. [CONFIRMED copy loop] */
                memcpy(&actor->collision_spin_matrix, d0, 9 * sizeof(float));
            } else {
                /* lateral>=1: ground-friction drag. bias from down_proj (NOT
                 * lift_y — prior port bug). k = min(lateral_mag, bias). */
                int32_t bias = (down_proj + 0x400) * 0x20;
                bias = (int32_t)(bias + (((int32_t)bias >> 31) & 0x3FF)) >> 10;  /* SAR_RZ 10 */
                int32_t k = (lateral_mag < bias) ? lateral_mag : bias;
                /* Velocity drag CROSSED per orig raw bytes @0x00409AD2:
                 * vx(+0x1cc) -= (k*l_z)>>8, vz(+0x1d4) -= (k*l_x)>>8.
                 * [CONFIRMED-from-bytes; decompiler labels x/z — flagged.] */
                actor->linear_velocity_x -= FP_TRUNC((k * l_z));
                actor->linear_velocity_z -= FP_TRUNC((k * l_x));
                /* [FIX 2026-06-02 flat-spin] Faithful re-derivation of collision_spin
                 * via BuildWorldToViewMatrix (orig 0x0042E750), replacing the prior
                 * staged-twist approximation that produced a tumble instead of the
                 * original's flat yaw spinout. The rotation axis = (l_x,0,l_z) x
                 * (avg_x,avg_y,avg_z) -- l.y is FORCED TO 0 [orig mov [l.y],0
                 * @0x00409A58], so the axis is ~vertical and the per-tick rotation
                 * is a FLAT YAW spin. Cross (>>12, l.y dropped) [@0x0042EAC0];
                 * angle = round((k * avg_y) * 3.108978186e-09) [const 0x3155A5B9
                 * @0x0045D5EC]; result = old_spin(d0) . new [old.new, @0x00409AAA]. */
                int32_t bwv_axis[3];
                bwv_axis[0] = (int32_t)(-(l_z * avg_y)) >> 12;
                bwv_axis[1] = (int32_t)(l_z * avg_x - l_x * avg_z) >> 12;
                bwv_axis[2] = (int32_t)(l_x * avg_y) >> 12;
                int32_t bwv_angle = (int32_t)lrintf((float)(k * avg_y) * 3.108978186e-09f);
                float bwv_mat[9];
                td5_build_world_to_view_matrix(bwv_mat, bwv_axis, bwv_angle);
                MultiplyRotationMatrices3x3(d0, bwv_mat, (float *)&actor->collision_spin_matrix);
                TD5_LOG_I(LOG_TAG, "recov_flatspin: slot=%d k=%d avg_y=%d angle=%d a12=%d axis=[%d,%d,%d]",
                          actor->slot_index, k, avg_y, bwv_angle, bwv_angle >> 0x12,
                          bwv_axis[0], bwv_axis[1], bwv_axis[2]);
            }

            /* Body-axis impulse: subtract heading-normal-aligned penetration.
             * [CONFIRMED shared tail @ 0x00409B0D] */
            int32_t pf_i = (int32_t)lrintf(pen_force);
            actor->linear_velocity_x -= FP_TRUNC(((int32_t)hx * pf_i));
            actor->linear_velocity_y -= FP_TRUNC(((int32_t)hy * pf_i));
            actor->linear_velocity_z -= FP_TRUNC(((int32_t)hz * pf_i));
            (void)c_min_eps;

            if (actor->slot_index == 0 && (actor->frame_counter & 7u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                    "recov_anim: t=%u xz=%d lat=%d down=%d branch=%s spin=[%.3f %.3f %.3f]",
                    (unsigned)actor->frame_counter, (int)xz_mag, lateral_mag, down_proj,
                    (xz_mag <= c_xz_thresh) ? "twist" : "drag",
                    actor->collision_spin_matrix.m[0], actor->collision_spin_matrix.m[4],
                    actor->collision_spin_matrix.m[8]);
            }
        }

        /* Tail: lift world_pos.y by deepest probe and trigger rolling SFX. */
        actor->world_pos.y += lift_y;
        td5_physics_check_and_update_actor_collision_alignment(actor);
        actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
        actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
        actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

        if (down_proj < -400 && actor->slot_index < 6) {
            int32_t world_pos[3] = { actor->world_pos.x, actor->world_pos.y, actor->world_pos.z };
            td5_sound_play_at_position(0x16, 0x1000, 0x5622, world_pos, 1);
        }
        return;
    }

    /* No probe hits — just lift_y (which is zero) and run tail. */
    actor->world_pos.y += lift_y;
    td5_physics_check_and_update_actor_collision_alignment(actor);
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Suppress unused-constant warnings if a branch is bypassed. */
    (void)c_pen_scale; (void)c_min_eps; (void)c_drag_quad_a;
    (void)c_drag_quad_b; (void)c_xz_thresh; (void)c_yaw_factor;
}

/* [CONFIRMED @ 0x00409D20] IntegrateScriptedVehicleMotion
 *
 * vehicle_mode==1 integration step — replaces the prior partial port at
 * the dispatch site. Linear-velocity damping + gravity, rebuild rotation
 * matrix via MultiplyRotationMatrices3x3(spin, saved → rotation), update
 * track position, run the probe pass + wall response, then post-pass
 * reset gate at frame_counter > 0x3B. */
void td5_physics_integrate_scripted_motion(TD5_Actor *actor)
{
    if (!actor) return;

    /* Damping: SAR_RZ_8 mirrors orig's CDQ;AND 0xFF;ADD;SAR 8 idiom. */
    int32_t vx = actor->linear_velocity_x;
    vx -= SAR_RZ_8(vx);
    actor->linear_velocity_x = vx;
    actor->world_pos.x += vx;

    /* Y: damp then subtract gravity */
    int32_t vy = actor->linear_velocity_y;
    vy = vy - SAR_RZ_8(vy) - g_gravity_constant;
    actor->linear_velocity_y = vy;
    actor->world_pos.y += vy;

    int32_t vz = actor->linear_velocity_z;
    vz -= SAR_RZ_8(vz);
    actor->linear_velocity_z = vz;
    actor->world_pos.z += vz;

    /* Rotation update: rotation = collision_spin * saved_orientation
     * [CONFIRMED @ 0x00409D67-95]. Result is written back to BOTH the rotation
     * matrix AND the saved_orientation copy in the orig (saved gets the new
     * rotation; collision_spin remains for the next tick's twist).
     *
     * Order matters: orig is `MultiplyRotationMatrices3x3(spin, saved, tmp)`
     * with output to a scratch, then copies tmp → saved → rotation.
     * Port mirrors. */
    float scratch[9];
    MultiplyRotationMatrices3x3((float *)&actor->collision_spin_matrix,
                                (float *)&actor->saved_orientation,
                                scratch);
    memcpy(&actor->saved_orientation, scratch, 9 * sizeof(float));
    memcpy(&actor->rotation_matrix, scratch, 9 * sizeof(float));

    /* Update chassis track position from new world pos. */
    td5_track_update_actor_position(actor);

    /* Update render_pos from world_pos. */
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    /* Probe pass → wall response. */
    uint32_t view_mask = td5_physics_render_vehicle_actor_model(actor);
    if (view_mask != 0)
        td5_physics_compute_actor_world_bounding_volume(actor, view_mask);

    /* Recovery timeout — orig has TWO sites that test frame_counter > 0x3B:
     *   1. End of IntegrateScriptedVehicleMotion @ 0x00409E5C-67 (this one)
     *   2. Inside CheckAndUpdateActorCollisionAlignment via alignment bin test
     * Either path calls ResetVehicleActorState. */
    if (actor->frame_counter > 0x3B)
        td5_physics_reset_actor_state(actor);
}

/* ========================================================================
 * [PORT 2026-05-25 damage-lockout-helpers; orig 0x00446140 / 0x004461C0]
 *
 * Infrastructure for the future damage_lockout switch fix described in
 *   re/analysis/oversight_triage_2026-05-24.md  (row "damage_lockout_switch_skipped")
 * and the long TODO block in update_vehicle_pose_from_physics @ td5_physics.c
 * (search "TODO (D3+D4 — damage_lockout switch").
 *
 * The switch dispatch in orig 0x004063A0 routes per damage_lockout value:
 *   cases 0,1,2,4,6,8,9 -> TransformTrackVertexByMatrix   @ 0x00446030 (A — both axes)
 *   cases 3, 12         -> TransformTrackVertexByMatrixC  @ 0x004461C0 (C — pitch only)
 *   cases 5, 10         -> TransformTrackVertexByMatrixB  @ 0x00446140 (B — roll only)
 *
 * The A variant (full solver) is ALREADY ported as
 * td5_physics_attitude_from_wheels (see td5_physics.c near line 6054). The
 * B and C variants below are byte-faithful per-axis ports — IDENTICAL math
 * to the corresponding half of the A solver. Confirmed by Ghidra
 * disassembly diff at 0x00446140/0x004461C0 (B/C use the SAME numerator,
 * span and AngleFromVector12 sequence as A's roll/pitch passes; the only
 * difference is that they SKIP writing the inactive axis).
 *
 * These helpers are PURE (no actor mutation beyond *out) and UNUSED by the
 * port until a future precise-port worktree (precise-00446030 per the
 * OVERSIGHT triage row's recommended-fix) wires them into the switch at
 * update_vehicle_pose_from_physics. They are exposed with TD5_UNUSED_FN so
 * GCC does not warn on the dead reference. Do not call them from the
 * per-tick attitude code without a runtime A/B test (HIGH risk; the author
 * of the triage explicitly defers to a precise-port worktree).
 *
 * Algorithm shared with A (attitude_from_wheels):
 *   wcp[12] = (int32_t*)&actor->wheel_contact_pos[0]      (4 vec3, stride 3)
 *   sp[4]   = actor->wheel_suspension_pos
 *   dz   = wcp[1] + wcp[4] - wcp[7] - wcp[10]
 *        + sp[0] + sp[1] - sp[2] - sp[3]                  (roll numerator)
 *   dx   = wcp[1] - wcp[4] - wcp[10] + wcp[7]
 *        + sp[0] - sp[1] + sp[2] - sp[3]                  (pitch numerator)
 *   crAr = wcp[0] + wcp[3] - wcp[6] - wcp[9]              (roll X span)
 *   crBr = wcp[2] + wcp[5] - wcp[8] - wcp[11]             (roll Z span)
 *   crAp = wcp[0] - wcp[3] + wcp[6] - wcp[9]              (pitch X span)
 *   crBp = wcp[2] - wcp[5] + wcp[8] - wcp[11]             (pitch Z span)
 *
 * Then each numerator/span pair is SAR>>8, squared and summed, FILD+FSQRT
 * (replicated via (int32_t)sqrtf((float)int_val) — RC=11 __ftol semantics),
 * fed into AngleFromVector12 with the numerator NEGated. No gimbal-lock
 * branch (variants B and C have NONE — straight-line code; the A variant
 * also has none — gimbal handling is a property of the EULER decomp at
 * 0x0042E030, not these per-axis solvers).
 * ======================================================================== */

/* `used` keeps the symbol in the object file even when no caller exists
 * yet (the dispatch wire-up is deferred to a precise-port worktree, see
 * the TODO at update_vehicle_pose_from_physics). `unused` suppresses the
 * accompanying "defined but not used" warning. */
#if defined(__GNUC__) || defined(__clang__)
#  define TD5_UNUSED_FN __attribute__((used, unused))
#else
#  define TD5_UNUSED_FN
#endif

/* [CONFIRMED @ 0x00446140] TransformTrackVertexByMatrixB — roll-only solver.
 *
 * Signature mirrors orig: param_1 = out (write *out = roll int16),
 * param_2 = wheel_contact_pos[] (int32_t* aliasing 4 vec3, stride 12B),
 * param_3 = wheel_suspension_pos[0..3] (int32_t*).
 *
 * Disassembly diff vs A's roll pass (0x00446030 first half):
 *   - ECX  = p[3] - p[9] - p[6] + p[0]                   (crAr — X span)
 *   - EDX  = p[5] - p[11] - p[8] + p[2]                  (crBr — Z span)
 *   - ESI  = p[4] - p[10] - p[7] - sp[3] - sp[2]
 *          + p[1] + sp[1] + sp[0]                        (dz   — roll numerator)
 *   - SAR>>8, IMUL, FILD/FSQRT, __ftol, NEG ESI, AngleFromVector12, MOV [param_1], AX
 * Byte-identical to attitude_from_wheels's roll arm. */
TD5_UNUSED_FN
static void td5_physics_transform_track_vertex_by_matrix_b(
    int16_t *out_roll,
    const int32_t *wheel_contact_pos,   /* aliased int32_t[12] over 4 vec3 */
    const int32_t *wheel_suspension_pos /* [4] */
)
{
    const int32_t *wcp = wheel_contact_pos;
    const int32_t *sp  = wheel_suspension_pos;

    int32_t dz   = wcp[1] + wcp[4] - wcp[7] - wcp[10]
                 + sp[0]  + sp[1]  - sp[2]  - sp[3];     /* roll numerator (ESI) */
    int32_t crAr = wcp[0] + wcp[3] - wcp[6] - wcp[9];    /* X span      (ECX)   */
    int32_t crBr = wcp[2] + wcp[5] - wcp[8] - wcp[11];   /* Z span      (EDX)   */

    /* SAR>>8 each before squaring (matches 0x00446182/0x00446187/0x00446198). */
    dz   >>= 8;
    crAr >>= 8;
    crBr >>= 8;

    /* IMUL EAX,EDX + IMUL EDX,ECX + ADD → squared sum (0x0044618A-0x00446194). */
    int32_t mag_sq = crBr * crBr + crAr * crAr;
    /* FILD+FSQRT+__ftol at 0x0044619B-0x004461A6. (int32_t)sqrtf matches the
     * RC=11 truncate-toward-zero semantics of orig __ftol. */
    int32_t mag = (int32_t)sqrtf((float)mag_sq);

    /* NEG ESI; PUSH ESI/EAX; CALL AngleFromVector12 (0x004461A7-0x004461AA). */
    int32_t roll12 = AngleFromVector12(-dz, mag);
    *out_roll = (int16_t)roll12;
}

/* [CONFIRMED @ 0x004461C0] TransformTrackVertexByMatrixC — pitch-only solver.
 *
 * Signature mirrors orig: param_1 = pointer such that *(short*)(param_1 + 4)
 * is the pitch output. To keep the port helper PURE and unambiguous, the
 * port exposes the pitch out-pointer DIRECTLY (caller does &display_angles.pitch).
 *
 * Disassembly diff vs A's pitch pass (0x00446030 second half):
 *   - ECX  = p[6] - p[9] - p[3] + p[0]                   (crAp — X span)
 *   - EDX  = p[8] - p[11] - p[5] + p[2]                  (crBp — Z span)
 *   - ESI  = p[4] - p[10] - p[7] - sp[3] - sp[1]
 *          + p[1] + sp[2] + sp[0]                        (dx   — pitch numerator,
 *                                                         sign-equivalent to
 *                                                         attitude_from_wheels)
 *   - SAR>>8, IMUL, FILD/FSQRT, __ftol, NEG ESI, AngleFromVector12, MOV [param_1+4], AX
 * Byte-identical to attitude_from_wheels's pitch arm. */
TD5_UNUSED_FN
static void td5_physics_transform_track_vertex_by_matrix_c(
    int16_t *out_pitch,
    const int32_t *wheel_contact_pos,   /* aliased int32_t[12] over 4 vec3 */
    const int32_t *wheel_suspension_pos /* [4] */
)
{
    const int32_t *wcp = wheel_contact_pos;
    const int32_t *sp  = wheel_suspension_pos;

    int32_t dx   = wcp[1] - wcp[4] + wcp[7] - wcp[10]
                 + sp[0]  - sp[1]  + sp[2]  - sp[3];     /* pitch numerator (ESI) */
    int32_t crAp = wcp[0] - wcp[3] + wcp[6] - wcp[9];    /* X span      (ECX)    */
    int32_t crBp = wcp[2] - wcp[5] + wcp[8] - wcp[11];   /* Z span      (EDX)    */

    /* SAR>>8 each before squaring (matches 0x00446202/0x00446207/0x00446218). */
    dx   >>= 8;
    crAp >>= 8;
    crBp >>= 8;

    /* IMUL+ADD → squared sum (0x0044620A-0x00446212). */
    int32_t mag_sq = crBp * crBp + crAp * crAp;
    /* FILD+FSQRT+__ftol at 0x0044621B-0x00446226. */
    int32_t mag = (int32_t)sqrtf((float)mag_sq);

    /* NEG ESI; PUSH ESI/EAX; CALL AngleFromVector12 (0x00446227-0x0044622A). */
    int32_t pitch12 = AngleFromVector12(-dx, mag);
    *out_pitch = (int16_t)pitch12;
}

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x00409520  CheckAndUpdateActorCollisionAlignment  [PORTED 2026-05-24 Tier 2]
 *   0x004096B0  ComputeActorWorldBoundingVolume        [PORTED 2026-05-24 Tier 2]
 *   0x0043C9E0  InitializeTrackedActorMarkerBillboards
 *   0x0043CDE0  RenderTrackedActorMarker
 *   0x0043D830  ApplyRandomWheelJitterHighSpeed  [ARCH-DIVERGENCE: dead in orig (0 callers verified 2026-05-21 via mcp__ghidra__function_callers); no port impl needed]
 *   0x0043D910  ApplyRandomWheelJitterLowSpeed   [ARCH-DIVERGENCE: dead in orig (0 callers verified 2026-05-21 via mcp__ghidra__function_callers); no port impl needed]
 *   0x0043E4C0  InsertTriangleIntoDepthSortBuckets  (density-match, verify in Phase 4)
 *   0x004431C0  LoadTrafficVehicleSkinTexture
 */
