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
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* td5_input_ff_collision (wall/prop impact FF) */
#include "td5_vfx.h"      /* td5_vfx_queue_prop_break (TD6 prop debris) */
#include "td5_game.h"     /* td5_game_get_total_actor_count, td5_game_is_wanted_mode */
#include "td5_platform.h"
#include "td5_trace.h"    /* inner-tick physics_trace stages */
/* V2V trace headers are included unconditionally: their obb_corner_test /
 * collision_detect_full call sites below are ungated, so the snapshot types
 * must always be declared. The emitters self-stub to no-ops under
 * TD5RE_RELEASE (release build does not link the v2v trace modules). */
#include "td5re.h"

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

/* ------------------------------------------------------------------------
 * SAR-RZ (signed arithmetic shift with round-toward-zero) helpers.
 *
 * x86 encodes signed division by a power-of-two via:
 *     CDQ                  ; EDX = sign(EAX) ? -1 : 0
 *     AND EDX, (2^N - 1)   ; EDX = (sign ? 2^N-1 : 0)
 *     ADD EAX, EDX         ; bias negative values up by (2^N-1)
 *     SAR EAX, N           ; signed shift right
 *
 * Net result: divide-by-2^N with round-toward-zero (matches C signed `/`).
 * Plain `x >> N` in C is implementation-defined; on GCC/clang it is
 * arithmetic-shift-right which rounds toward -infinity for negative `x`,
 * differing from x86's SAR-RZ by 1 LSB when `x < 0` and the low N bits
 * are nonzero.
 *
 * Use SAR_RZ_8 / SAR_RZ_12 / SAR_RZ_6 / SAR_RZ_15 instead of plain `>>`
 * at any site sourced from a `CDQ; AND EDX,mask; ADD; SAR` idiom in the
 * original listing.
 *
 * `x` is evaluated once; safe to use with arbitrary signed int32 args.
 * Audit citations: pilot_00404030_audit.md (D1/D2/D5/D10/D14).
 * ------------------------------------------------------------------------ */
#define SAR_RZ_6(x)   (((int32_t)(x) + (((int32_t)(x) >> 31) & 0x3F))   >> 6)
#define SAR_RZ_8(x)   (((int32_t)(x) + (((int32_t)(x) >> 31) & 0xFF))   >> 8)
#define SAR_RZ_12(x)  (((int32_t)(x) + (((int32_t)(x) >> 31) & 0xFFF))  >> 12)
#define SAR_RZ_15(x)  (((int32_t)(x) + (((int32_t)(x) >> 31) & 0x7FFF)) >> 15)

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

/* [tasks #9/#14] V2V traffic-taming knob accessors + ground clamp, defined
 * lower in the file but referenced from obb_corner_test / collision_detect_*. */
static int   traffic_hit_tame_enabled(void);
static int   traffic_mass_pct(void);
static int   wreck_immobile_enabled(void);
static int   wreck_mass_pct(void);
static float traffic_hitbox_scale(void);
static void  traffic_clamp_above_ground(TD5_Actor *t);

/* [item #4] Racer (slot 0..g_traffic_slot_base-1) OBB hitbox-fit scale, defined
 * lower in the file but referenced from obb_corner_test / apply_collision_response.
 * Brings the player/AI collision box in line with the visible chassis (the cardef
 * OBB is authored larger than the mesh) so V2V contacts no longer fire across a
 * visible gap. Returns 1.0 (no shrink) when TD5RE_HITBOX_FIT=0. */
static float racer_hitbox_scale(void);
static float actor_hitbox_scale(const TD5_Actor *act);

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
 * out-of-range slots since only racer slots carry summary metrics. */
static inline void td5_physics_mark_collision(int slot)
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
static inline int32_t phys_surface_grip(int surface)
{
    if (td5_track_td6_surface_grid_loaded()) return td5_track_td6_surface_grip_q8(surface & 0x1F);
    return (int32_t)s_surface_friction[surface & 0x1F];
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
        const char *e = getenv("TD5RE_TD6_GRASS_SLIDE");
        s_en = (e && e[0] == '0') ? 0 : 1;
    }
    if (!s_en || !td5_track_td6_surface_grid_loaded()) return grip;
    int q8 = td5_track_td6_surface_grip_q8(surface & 0x1F);   /* 0x100 = full grip */
    if (q8 >= 0x100) return grip;                             /* full-grip surface: unchanged */
    return (int32_t)(((int64_t)grip * (int64_t)q8) >> 8);    /* scale by the surface ratio */
}

/* --- Globals matching original binary layout --- */
static int32_t g_gravity_constant = TD5_GRAVITY_NORMAL;

/* Pilot-trace accessor — exposes g_gravity_constant to the pilot
 * trace module without leaking the static. Read-only debug use. */
int32_t td5_physics_dbg_get_gravity_constant(void) {
    return g_gravity_constant;
}
static int32_t g_collisions_enabled = 0;     /* DAT_00463188 (== orig's `g_cameraMode`):
                                                * 0 = normal play / collisions ON / no-clip OFF,
                                                * 1 = no-clip mode / collisions OFF. The two names
                                                * refer to the SAME dword; the user-facing INI
                                                * "Collisions" knob is XOR'd into it (frontend at
                                                * 0x004155BD / 0x0041DC8E). Setter `td5_physics_set_collisions`
                                                * preserves that inversion. */
static int32_t g_game_paused = 0;            /* DAT_004AAD60 */
static int32_t g_xz_freeze = 0;             /* DAT_00483030: 1=freeze XZ during countdown */
static int32_t s_dynamics_mode = 0;          /* 0=arcade, 1=simulation (0x42F7B0) */
static int32_t g_difficulty_easy = 0;
static int32_t g_difficulty_hard = 0;
static int32_t g_total_actor_count = 6;
static int32_t g_race_slot_state[TD5_MAX_RACER_SLOTS]; /* 1=human, 0=AI per slot */

/* Viewport -> actor-slot map (defined in td5_game.c). Used by the stuck-recovery
 * driver to map a local human player index back to its actor slot, and to read
 * that player's one-shot manual-recovery edge from the input layer. */
extern int g_actorSlotForView[TD5_MAX_VIEWPORTS];

/* ========================================================================
 * Player stuck-car recovery (PORT ENHANCEMENT 2026-06-15)
 *
 * MANUAL ONLY. A LOCAL HUMAN player recovers a pinned/stuck car by pressing the
 * recovery control (keyboard R / joystick SELECT — an edge from the td5_input
 * layer). There is NO automatic trigger: a car is repositioned only when the
 * driver explicitly asks for it.
 *
 * [AUTO-RECOVERY REMOVED 2026-06-24] An earlier port revision also ran an
 * AUTOMATIC "stuck for N seconds -> recover" dwell. It was removed at the user's
 * request: even after the wall-contact and throttle/brake gates it still yanked
 * idle/slow cars into a recovery teleport ("recover out of nowhere"). Only the
 * manual trigger remains, now with a cooldown (below).
 *
 * The reset repositions the actor a few spans back, centred on the track,
 * upright, velocities zeroed, heading aligned to the track forward direction.
 *
 * COOLDOWN: after a manual recovery fires, further manual requests for that
 * player are ignored for TD5_MANUAL_RECOVERY_COOLDOWN_TICKS sim ticks (5 s @
 * 30 Hz). The input edge is still drained every tick so a press during the
 * cooldown can't queue up and fire the instant it expires.
 *
 * All of this runs from the deterministic 30 Hz sim tick (td5_physics_tick),
 * so it is replay-deterministic. DETERMINISM/NETPLAY: the manual edge reads
 * LOCAL input only — it is NOT exchanged over the lockstep protocol, so for v1
 * the whole recovery driver is restricted to non-network play (gated on
 * !g_td5.network_active). See the report for the lockstep follow-up.
 *
 * Knobs (cached on first use):
 *   TD5RE_STUCK_RECOVERY          default ON  — master gate for manual recovery.
 *   TD5RE_RECOVERY_SPANS_BACK     default 3   — spans to move back on reset.
 *   TD5RE_RECOVERY_COOLDOWN_TICKS default 150 — manual cooldown window (ticks).
 * ======================================================================== */

#define TD5_RECOVERY_DEFAULT_BACK   3
/* Manual-recovery cooldown: number of sim ticks after a manual recovery during
 * which further manual requests for the same player are ignored. 150 = 5 s at
 * the fixed 30 Hz tick. Runtime-overridable via TD5RE_RECOVERY_COOLDOWN_TICKS. */
#define TD5_MANUAL_RECOVERY_COOLDOWN_TICKS  150  /* 5 s at fixed 30 Hz */

/* Per-slot manual-recovery cooldown counter (racer slots only; humans are
 * racers). Counts DOWN each live sim tick from the cooldown window to 0; while
 * > 0 a manual recovery request is ignored. Cleared at race init and re-armed to
 * the cooldown window each time a manual recovery fires. */
static int     s_manual_recovery_cooldown[TD5_MAX_RACER_SLOTS];

static int     s_recovery_init = 0;
static int     s_recovery_enabled = 1;     /* TD5RE_STUCK_RECOVERY */
static int     s_recovery_spans_back = TD5_RECOVERY_DEFAULT_BACK;
static int     s_recovery_cooldown_ticks = TD5_MANUAL_RECOVERY_COOLDOWN_TICKS;  /* TD5RE_RECOVERY_COOLDOWN_TICKS */

/* [GENTLE FLIP-RECOVERY 2026-06-21] Forward decls (definitions live just after
 * the stuck-recovery block). For a LOCAL HUMAN, the byte-faithful scripted-
 * recovery tumble (vehicle_mode==1) is replaced with a ~1s gentle coast that
 * keeps the car's existing motion, levels it back upright, then hands off to
 * the same in-place ground-snap "recovery teleport". See
 * td5_physics_gentle_recovery_coast. Knob TD5RE_RECOVERY_GENTLE (default ON). */
static int  recovery_gentle_enabled(void);
static int  recovery_gentle_for_actor(const TD5_Actor *actor);
static void td5_physics_gentle_recovery_coast(TD5_Actor *actor);

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
static uint8_t s_prev_grounded_mask[16];     /* per-slot previous-frame grounded bitmask (1=grounded) */

/* ========================================================================
 * Multiplayer catch-up assist (rubber-banding for HUMAN players) — PORT-ONLY,
 * NON-FAITHFUL, opt-in. [MP CATCHUP 2026-06-14]
 *
 * In a multiplayer race (2+ HUMAN players, split-screen OR net) a human who is
 * FALLING BEHIND the race leader gets a small, smooth boost to longitudinal
 * drive force so the pack stays together; the leader (and anyone ahead) is
 * optionally throttled a touch. This is the classic arcade "catch-up" — it is
 * NOT the netcode/lag resync (that lives in td5_net.c).
 *
 * DETERMINISM (critical — this game runs LOCKSTEP UDP netplay):
 *   - The per-slot multiplier is a PURE FUNCTION of replicated simulation state:
 *     each racer's track_span_high_water (+0x086, the same monotonic forward
 *     span counter UpdateRaceOrder @0x0042F5B0 sorts standings by) and the
 *     replicated per-slot human/AI table g_race_slot_state[]. Both are identical
 *     on every client every tick, so every client computes the same multiplier.
 *   - It is recomputed once per DETERMINISTIC fixed-30Hz sim tick (top of
 *     td5_physics_tick) and applied inside the integer drive-torque pipeline
 *     (td5_physics_compute_drive_torque). No rand()/wall-clock/per-viewport or
 *     other local-only input is read here.
 *
 * SCOPE GUARD: applies ONLY when >=2 human racer slots are present (split-screen
 * or net). Single-player (1 human) leaves every multiplier at 1.0, so the SP
 * experience is byte-unchanged. AI/traffic slots are never affected.
 *
 * KNOBS (env, cached once; CLI/INI not plumbed — env is the source here):
 *   TD5RE_MP_CATCHUP           master gate, DEFAULT 0 (OFF). "1" = enable.
 *   TD5RE_MP_CATCHUP_STRENGTH  strength 0..100, DEFAULT 35 (conservative).
 *                              Scales BOTH the max behind-boost and the leader
 *                              throttle. 0 behaves like OFF.
 *   TD5RE_MP_CATCHUP_LEADER    DEFAULT 1: also throttle cars AHEAD (down to a
 *                              small floor). "0" = boost trailers only, no
 *                              leader slow-down.
 * ======================================================================== */

/* Drive-force multiplier is Q8 fixed-point (0x100 = 1.0). */
#define MP_CATCHUP_Q8_ONE        0x100

/* Gap (in track spans, via track_span_high_water) at which a trailing human
 * reaches the FULL configured boost. Below this the boost ramps linearly with
 * the gap; at/above it the boost is clamped. ~30 spans ≈ a few car lengths of
 * separation on a typical TD5 strip, so the assist eases in smoothly and never
 * pops. Deterministic constant (same on every client). */
#define MP_CATCHUP_FULL_GAP_SPANS  30

/* Maximum behind-boost at 100% strength, Q8. 0x100 + 0x60 = 1.375x drive force
 * at full strength + full gap. At the conservative default strength (35%) the
 * realised cap is 0x100 + (0x60*35/100) ≈ 1.13x — a gentle nudge, not a warp. */
#define MP_CATCHUP_MAX_BOOST_Q8    0x60

/* Maximum leader throttle at 100% strength, Q8 (subtracted from 1.0 for cars
 * AT/AHEAD of the gap window). 0x100 - 0x30 = 0.8125x at full strength; floored
 * so the leader is never crippled. Scaled by strength like the boost. */
#define MP_CATCHUP_MAX_LEADER_CUT_Q8  0x30

/* Resolved config (lazy, cached). s_mp_catchup_cfg: -1 = unresolved, 0 = off,
 * 1 = on. s_mp_catchup_strength: 0..100. s_mp_catchup_leader: 0/1. */
static int      s_mp_catchup_cfg      = -1;
static int      s_mp_catchup_strength = 0;
static int      s_mp_catchup_leader   = 1;

/* Per-racer-slot drive-force multiplier, Q8 (0x100 = 1.0 = no change).
 * Written once per sim tick by td5_physics_update_mp_catchup(); read in
 * td5_physics_compute_drive_torque(). Default 1.0 so it is inert until the
 * feature is enabled AND >=2 humans are racing. */
static int32_t  s_mp_catchup_mult[TD5_MAX_RACER_SLOTS];

/* Resolve TD5RE_MP_CATCHUP* env knobs once and log the result. Safe to call
 * every tick (no-op after the first). getenv result is process config, not sim
 * input, and the SAME on every machine for a given launch, so reading it does
 * not break lockstep determinism. */
static void td5_physics_mp_catchup_config(void)
{
    const char *e;
    int i;

    if (s_mp_catchup_cfg >= 0)
        return;  /* already resolved */

    /* Default ON (user-confirmed 2026-06-15): the MP rubber-band catch-up assist.
     * Set TD5RE_MP_CATCHUP=0 (or n/f) to disable. The >=2-human scope guard in
     * td5_physics_update_mp_catchup() still keeps single-player byte-unchanged. */
    s_mp_catchup_cfg = 1;
    e = getenv("TD5RE_MP_CATCHUP");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        s_mp_catchup_cfg = 0;

    /* Strength 0..100, conservative default 35. */
    s_mp_catchup_strength = 35;
    e = getenv("TD5RE_MP_CATCHUP_STRENGTH");
    if (e && e[0]) {
        int v = atoi(e);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        s_mp_catchup_strength = v;
    }

    /* Leader throttle on by default. */
    s_mp_catchup_leader = 1;
    e = getenv("TD5RE_MP_CATCHUP_LEADER");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        s_mp_catchup_leader = 0;

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        s_mp_catchup_mult[i] = MP_CATCHUP_Q8_ONE;

    TD5_LOG_I(LOG_TAG,
              "MP catchup: %s strength=%d%% leader_throttle=%s "
              "(env TD5RE_MP_CATCHUP / _STRENGTH / _LEADER; full_gap=%d spans, "
              "max_boost=%d/256, max_leader_cut=%d/256)",
              s_mp_catchup_cfg ? "ON" : "OFF (default)",
              s_mp_catchup_strength,
              s_mp_catchup_leader ? "on" : "off",
              MP_CATCHUP_FULL_GAP_SPANS, MP_CATCHUP_MAX_BOOST_Q8,
              MP_CATCHUP_MAX_LEADER_CUT_Q8);
}

/* Recompute the per-slot catch-up drive-force multiplier for THIS sim tick.
 *
 * Pure function of replicated sim state (track_span_high_water + g_race_slot_state),
 * so it is lockstep-deterministic. Called once at the top of td5_physics_tick().
 *
 * Algorithm:
 *   - Count human racer slots (g_race_slot_state==1, slot < g_traffic_slot_base).
 *     <2 humans (single-player) => every multiplier reset to 1.0, return (the
 *     SP/MP scope guard — SP is never touched).
 *   - leader_progress = max(track_span_high_water) over ACTIVE racer slots
 *     (humans and AI both count toward "who is in front").
 *   - For each HUMAN slot: gap = leader_progress - my_progress (>=0).
 *       boost = (MAX_BOOST * strength/100) * min(gap,FULL_GAP)/FULL_GAP
 *       mult  = 1.0 + boost                       (trailing: speed up)
 *     For a human AT/ahead of the gap window (gap==0, i.e. the leader, or any
 *     car within ~0 gap) and TD5RE_MP_CATCHUP_LEADER on:
 *       cut   = (MAX_LEADER_CUT * strength/100)   (scaled toward the window)
 *       mult  = 1.0 - cut                         (leader: ease off)
 *     AI slots are left at 1.0 (never boosted/cut).
 *
 * Everything is integer 24.8/Q8; no float, no rounding surprises across clients. */
static void td5_physics_update_mp_catchup(void)
{
    int slot, total, racer_cap, human_count;
    int32_t leader_progress;

    td5_physics_mp_catchup_config();

    /* Reset to neutral up front so disabled/early-return paths are inert. */
    for (slot = 0; slot < TD5_MAX_RACER_SLOTS; slot++)
        s_mp_catchup_mult[slot] = MP_CATCHUP_Q8_ONE;

    if (!s_mp_catchup_cfg || s_mp_catchup_strength <= 0)
        return;
    if (!g_actor_table_base)
        return;

    total = td5_game_get_total_actor_count();
    if (total <= 0)
        return;
    racer_cap = (total < g_traffic_slot_base) ? total : g_traffic_slot_base;
    if (racer_cap > TD5_MAX_RACER_SLOTS)
        racer_cap = TD5_MAX_RACER_SLOTS;

    /* Pass 1: count humans + find the leader's progress among active racers. */
    human_count = 0;
    leader_progress = INT32_MIN;
    for (slot = 0; slot < racer_cap; slot++) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        int32_t prog = (int32_t)a->track_span_high_water;
        if (g_race_slot_state[slot] == 1)
            human_count++;
        if (prog > leader_progress)
            leader_progress = prog;
    }

    /* SCOPE GUARD: only engage with 2+ humans (split-screen or net MP). */
    if (human_count < 2)
        return;

    /* Pass 2: per-human multiplier from the gap to the leader. */
    for (slot = 0; slot < racer_cap; slot++) {
        TD5_Actor *a;
        int32_t my_prog, gap;

        if (g_race_slot_state[slot] != 1)
            continue;  /* humans only */

        a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        my_prog = (int32_t)a->track_span_high_water;
        gap = leader_progress - my_prog;          /* spans behind the leader */
        if (gap < 0) gap = 0;                     /* defensive (this IS leader) */

        if (gap > 0) {
            /* Behind: ramp a boost in with the gap, clamp at FULL_GAP. */
            int32_t g = (gap < MP_CATCHUP_FULL_GAP_SPANS) ? gap
                                                          : MP_CATCHUP_FULL_GAP_SPANS;
            /* boost_q8 = MAX_BOOST * strength/100 * g/FULL_GAP, all integer. */
            int32_t boost = (MP_CATCHUP_MAX_BOOST_Q8 * s_mp_catchup_strength) / 100;
            boost = (boost * g) / MP_CATCHUP_FULL_GAP_SPANS;
            s_mp_catchup_mult[slot] = MP_CATCHUP_Q8_ONE + boost;
        } else if (s_mp_catchup_leader) {
            /* Leader (gap 0): ease off by the strength-scaled leader cut. */
            int32_t cut = (MP_CATCHUP_MAX_LEADER_CUT_Q8 * s_mp_catchup_strength) / 100;
            s_mp_catchup_mult[slot] = MP_CATCHUP_Q8_ONE - cut;
        }
        /* else: leader throttle disabled -> stays 1.0 */
    }
}

/* Q8 catch-up multiplier for `slot` (0x100 = 1.0). Returns 1.0 for non-racer
 * slots / out of range so callers can apply it unconditionally. */
static inline int32_t td5_physics_mp_catchup_mult(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return MP_CATCHUP_Q8_ONE;
    return s_mp_catchup_mult[slot];
}

/* ========================================================================
 * Gearbox mode (manual vs automatic) — the per-actor flag. [#2 2026-06-15]
 *
 * The PER-ACTOR gearbox mode lives in the byte at actor +0x378. This is the
 * SAME byte the original input path writes every tick as
 *     actor[0x378] = ~(g_playerControlBits >> 28) & 1     (UpdatePlayerVehicleControlState @ 0x00402E60)
 * where control-bit 28 is the auto/manual TOGGLE. So:
 *     +0x378 == 0  ⟺  bit 28 SET  ⟺  MANUAL gearbox
 *     +0x378 != 0  ⟺  bit 28 clear ⟺ AUTOMATIC gearbox
 * The actor-struct header labels +0x378 "throttle_input_active" because in the
 * common (automatic) case the original folds throttle-active into the same byte;
 * but for the GEARBOX dispatch the original keys on this exact byte (see the
 * "field_0x378 == 0 → manual" checks in td5_physics_update_player at the on-
 * ground / airborne branches, and UpdateAutomaticGearSelection being CALLED only
 * on the !=0 (auto) side). The byte IS the authoritative gearbox-mode flag — the
 * earlier "+0x378 may be the wrong flag" worry was unfounded: there is no other
 * per-actor manual/auto field, and +0x378 carries the mode every tick.
 *
 * ⚠ KNOWN MENU-SIDE BUG (cross-file, NOT fixable here — see report): the
 * frontend car-select "Automatic / Manual" toggle writes only s_selected_
 * transmission (display + MP persistence) and NEVER reaches the input layer.
 * td5_input.c keys control-bit 28 off g_td5.ini.auto_gearbox (the INI key) only,
 * so picking "Manual" in the menu does nothing — the car stays in whatever
 * AutoGearbox= says (default 1 = automatic) and therefore auto-shifts. The
 * physics gate below is correct; the wiring fix belongs in td5_input.c.
 * ======================================================================== */

/* TRUE when the actor is in MANUAL gearbox mode (byte +0x378 == 0). Single
 * canonical predicate so every manual-vs-auto decision in this file reads the
 * same flag the same way. Null-safe (treats NULL as automatic). */
static inline int td5_physics_actor_is_manual_gearbox(const TD5_Actor *actor)
{
    return actor && *((const uint8_t *)actor + 0x378) == 0;
}

/* TD5RE_MANUAL_GEARBOX (env, cached once) DEFAULT 1 (ON). When OFF ("0"/"n"/"f")
 * the manual-gearbox behaviour layer is disabled: the manual performance boost
 * is forced to 1.0 AND a manual car is treated as automatic for the auto-shift
 * decision (defensive — restores pre-#2 behaviour for A/B). Launch config, not
 * sim input, so caching is lockstep-safe. */
static int s_manual_gearbox_on = -1;
static int td5_physics_manual_gearbox_enabled(void)
{
    if (s_manual_gearbox_on < 0) {
        const char *e = getenv("TD5RE_MANUAL_GEARBOX");
        s_manual_gearbox_on =
            (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                   e[0] == 'f' || e[0] == 'F')) ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG,
                  "manual_gearbox: TD5RE_MANUAL_GEARBOX=%d (1=manual cars never auto-shift)",
                  s_manual_gearbox_on);
    }
    return s_manual_gearbox_on;
}

/* TRUE when the auto-shift FSM should run for this actor THIS tick. An actor in
 * MANUAL mode (+0x378 == 0) must NOT auto-shift — the player shifts via the
 * gear-up/down inputs (td5_input.c writes actor[0x36B]). Automatic actors (all
 * AI/traffic, and the human player in auto mode) always auto-shift. With
 * TD5RE_MANUAL_GEARBOX=0 every actor auto-shifts (faithful pre-#2 fallback). */
static inline int td5_physics_actor_should_auto_shift(const TD5_Actor *actor)
{
    if (!td5_physics_manual_gearbox_enabled())
        return 1;                                   /* knob off → always auto */
    return !td5_physics_actor_is_manual_gearbox(actor);
}

/* ========================================================================
 * Manual-gearbox performance boost — PORT-ONLY. [MANUAL BOOST 2026-06-15, #2]
 *
 * A car driven in MANUAL gearbox mode gets +N% ACCELERATION and +N% TOP SPEED
 * over the same car in AUTOMATIC mode, for ALL cars (player, AI, traffic).
 * The acceleration half scales drive torque at the single drive-torque
 * chokepoint (td5_physics_compute_drive_torque, after the MP/Hard catch-up
 * blocks); the top-speed half raises the speed_limit at each drive-side gate by
 * the same Q8 factor. Automatic cars (and a manual car when the gearbox layer is
 * off) read a 1.0 factor → byte-unchanged.
 *
 * The factor is Q8 (0x100 = 1.0). For the default 20% it is 0x100 + (0x100 *
 * 20 / 100) = 0x100 + 0x33 = 0x133. Applied with the same biased-toward-zero
 * signed >>8 idiom as the catch-up multipliers so it composes cleanly.
 *
 * DETERMINISM: gated on the actor +0x378 gearbox byte (replicated/local input
 * state) and a launch-config Q8 factor — no rand()/wall-clock — so torque and
 * the speed gate stay bit-identical across lockstep clients.
 *
 * KNOBS: TD5RE_MANUAL_BOOST (env, cached once) DEFAULT 1 (ON); "0"/"n"/"f"
 * disables → factor 1.0 → byte-unchanged. TD5RE_MANUAL_BOOST_PCT (env, cached
 * once) DEFAULT 20, clamped 0..100. The boost additionally requires the gearbox
 * layer (TD5RE_MANUAL_GEARBOX) to be ON.
 * ======================================================================== */

/* Resolved manual-boost factor, Q8 (0x100 = 1.0 = inert). -1 = unresolved. */
static int32_t s_manual_boost_q8 = -1;

/* Resolve TD5RE_MANUAL_BOOST / _PCT once into a cached Q8 factor; logged once.
 * getenv is launch config (same on every client), not sim input, so caching it
 * does not break lockstep determinism. */
static int32_t td5_physics_manual_boost_q8(void)
{
    if (s_manual_boost_q8 < 0) {
        const char *e = getenv("TD5RE_MANUAL_BOOST");
        int on = 1;  /* default ON */
        int pct = 20;
        if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
            on = 0;
        e = getenv("TD5RE_MANUAL_BOOST_PCT");
        if (e && e[0]) {
            pct = atoi(e);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
        }
        s_manual_boost_q8 = on ? (MP_CATCHUP_Q8_ONE + (MP_CATCHUP_Q8_ONE * pct) / 100)
                               : MP_CATCHUP_Q8_ONE;
        TD5_LOG_I(LOG_TAG,
                  "manual_boost: TD5RE_MANUAL_BOOST=%d pct=%d -> factor=%d/256 "
                  "(+accel +top-speed for manual gearbox, all cars)",
                  on, pct, s_manual_boost_q8);
    }
    return s_manual_boost_q8;
}

/* Per-actor drive-side boost factor, Q8 (0x100 = 1.0). Returns the manual-boost
 * factor when the actor is in MANUAL gearbox mode (byte +0x378 == 0) AND the
 * gearbox layer is enabled, else 1.0. Used to scale both drive torque
 * (acceleration) and the speed_limit gate (top speed) so a manual car
 * accelerates harder AND tops out higher. */
static inline int32_t td5_physics_actor_manual_boost_q8(const TD5_Actor *actor)
{
    /* +0x378 == 0 → manual [authoritative gearbox-mode flag; see header above].
     * Gated by TD5RE_MANUAL_GEARBOX so the whole manual layer toggles together. */
    if (td5_physics_manual_gearbox_enabled() &&
        td5_physics_actor_is_manual_gearbox(actor))
        return td5_physics_manual_boost_q8();
    return MP_CATCHUP_Q8_ONE;
}

/* Raise a per-car speed limit by a Q8 factor (top-speed half of the manual
 * boost). speed_limit is non-negative (top_speed << 8), so the simple Q8
 * multiply + >>8 is exact and never needs the negative bias. Inert at 1.0. */
static inline int32_t td5_physics_apply_speed_limit_boost(int32_t speed_limit, int32_t q8)
{
    if (q8 == MP_CATCHUP_Q8_ONE)
        return speed_limit;
    return (int32_t)(((int64_t)speed_limit * (int64_t)q8) >> 8);
}

/* ========================================================================
 * Power-relative uphill-slope decel scaling — PORT-ONLY. [#8 2026-06-15]
 *
 * The gravity-along-slope term (section 14a-slope in td5_physics_update_player,
 * knob TD5RE_SLOPE_DECEL) subtracts a FIXED velocity delta per tick regardless
 * of how powerful the car is. That delta is the same absolute amount for a
 * 260 mph supercar and a slow truck, so on a weak car it eats a far larger
 * FRACTION of the modest forward speed it can muster — weak cars crawl uphill.
 *
 * Scale the UPHILL decel DOWN for lower-powered cars, proportional to the car's
 * TOP-SPEED rating (tuning +0x74, PHYS_TOP_SPEED — the most monotonic per-car
 * "how fast/strong is it" number; raw values observed ~0x3A1..0x433 = 929..1075
 * for the AI difficulty templates, player cars in the same band). The scale is
 *     s = clamp(top_speed / REF, FLOOR, 1.0)
 * so a car at/above REF keeps full decel and the weakest car keeps at least
 * FLOOR of it. Applied as a Q12 fixed-point factor on the (already negative)
 * uphill g_long only; downhill is untouched.
 *
 * KNOBS: TD5RE_SLOPE_DECEL_LIGHT (env, cached) DEFAULT 1 (ON); "0"/"n"/"f" →
 * scale forced to 1.0 (every car gets the full TD5RE_SLOPE_DECEL uphill term,
 * pre-#8 behaviour). TD5RE_SLOPE_DECEL_REF (env, cached) DEFAULT 1075 — the
 * top-speed rating that earns full uphill decel. TD5RE_SLOPE_DECEL_FLOOR_PCT
 * (env, cached) DEFAULT 45, clamped 5..100 — the minimum % of the uphill decel
 * any car keeps. All launch config (not sim input) so caching is lockstep-safe;
 * the only sim input is the per-car top_speed → deterministic.
 * ======================================================================== */
#define SLOPE_LIGHT_Q12_ONE  0x1000   /* Q12 1.0 */

static int s_slope_light_init = 0;
static int s_slope_light_on   = 1;     /* default ON */
static int s_slope_light_ref  = 1075;  /* top_speed earning full decel (0x433) */
static int s_slope_light_floor_q12 = (SLOPE_LIGHT_Q12_ONE * 45) / 100;  /* 0.45 */

static void td5_physics_slope_light_resolve(void)
{
    if (s_slope_light_init) return;
    {
        const char *e = getenv("TD5RE_SLOPE_DECEL_LIGHT");
        if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                  e[0] == 'f' || e[0] == 'F'))
            s_slope_light_on = 0;
        e = getenv("TD5RE_SLOPE_DECEL_REF");
        if (e && e[0]) {
            int r = atoi(e);
            if (r > 0) s_slope_light_ref = r;
        }
        e = getenv("TD5RE_SLOPE_DECEL_FLOOR_PCT");
        if (e && e[0]) {
            int p = atoi(e);
            if (p < 5)   p = 5;
            if (p > 100) p = 100;
            s_slope_light_floor_q12 = (SLOPE_LIGHT_Q12_ONE * p) / 100;
        }
    }
    s_slope_light_init = 1;
    TD5_LOG_I(LOG_TAG,
              "slope_decel_light: TD5RE_SLOPE_DECEL_LIGHT=%d ref=%d floor=%d/4096 "
              "(weak cars get proportionally less uphill decel)",
              s_slope_light_on, s_slope_light_ref, s_slope_light_floor_q12);
}

/* Per-car Q12 scale (0x1000 = full uphill decel) for the given top-speed rating.
 * 1.0 when the feature is off or top_speed is unknown (<=0). */
static int32_t td5_physics_slope_light_scale_q12(int32_t top_speed)
{
    td5_physics_slope_light_resolve();
    if (!s_slope_light_on || top_speed <= 0 || s_slope_light_ref <= 0)
        return SLOPE_LIGHT_Q12_ONE;
    {
        int32_t s = (int32_t)(((int64_t)top_speed * SLOPE_LIGHT_Q12_ONE) /
                              s_slope_light_ref);
        if (s > SLOPE_LIGHT_Q12_ONE)        s = SLOPE_LIGHT_Q12_ONE;
        if (s < s_slope_light_floor_q12)    s = s_slope_light_floor_q12;
        return s;
    }
}

/* ------------------------------------------------------------------------
 * Hill-climb drive-torque assist — PORT-ONLY. [2026-06-16]
 *
 * On steep uphill grades the gravity-along-slope term (section 14a-slope)
 * subtracts more longitudinal speed per tick than a mid-power car's drive
 * torque can replace, so the car decelerates to a stall partway up the
 * incline (Scotland's ~45-degree cliff in a Camaro). Boost the drive torque
 * proportionally to how steep the climb is so the engine can overcome
 * gravity on a cliff. Flat ground and downhill are untouched (the boost is
 * gated on the uphill side and ramps from 1.0x at flat). It scales the drive
 * torque only — the abs_speed<=speed_limit gate still caps top speed, so this
 * does NOT raise flat-ground top speed (up==0 there -> 1.0x).
 *
 * Knobs:
 *   TD5RE_HILL_ASSIST       default ON ("0" disables -> old stall behaviour)
 *   TD5RE_HILL_ASSIST_MAX   peak drive-torque multiplier % at/above the
 *                           reference grade (default 260 = 2.6x), 100..600.
 * The steepness measure is the surface normal's forward projection along the
 * heading; at a 45-degree climb that magnitude is ~sin(45)*4096 = 2896. A
 * squared ramp keeps gentle rolling hills almost unaffected while strongly
 * boosting a true cliff. */
#define HILL_ASSIST_REF_UP 2896    /* fwd-projection magnitude at ~45 deg */
static int s_hill_assist_init     = 0;
static int s_hill_assist_on       = 1;
static int s_hill_assist_max_q12  = (4096 * 260) / 100;   /* 2.6x default */

static void td5_physics_hill_assist_resolve(void)
{
    const char *e, *m;
    if (s_hill_assist_init) return;
    e = getenv("TD5RE_HILL_ASSIST");
    if (e && e[0] == '0' && e[1] == '\0') s_hill_assist_on = 0;
    m = getenv("TD5RE_HILL_ASSIST_MAX");
    if (m && m[0]) {
        int p = atoi(m);
        if (p < 100) p = 100;
        else if (p > 600) p = 600;
        s_hill_assist_max_q12 = (4096 * p) / 100;
    }
    s_hill_assist_init = 1;
    TD5_LOG_I(LOG_TAG, "hill_assist: %s max=%d/4096 (drive-torque boost vs uphill grade)",
              s_hill_assist_on ? "on" : "off", s_hill_assist_max_q12);
}

/* Q12 drive-torque multiplier for the current uphill steepness. up_mag is the
 * (>=0) forward-projection magnitude of the surface normal (0 flat, ~2896 at
 * 45 deg). Returns 0x1000 (1.0x) when off or on flat/downhill ground. */
static int32_t td5_physics_hill_assist_q12(int32_t up_mag)
{
    int32_t extra, boost;
    int64_t r, r2;
    td5_physics_hill_assist_resolve();
    if (!s_hill_assist_on || up_mag <= 0) return 4096;
    extra = s_hill_assist_max_q12 - 4096;            /* >=0 */
    r = ((int64_t)up_mag << 12) / HILL_ASSIST_REF_UP;  /* grade ratio, Q12 */
    if (r > 4096) r = 4096;                          /* clamp at the ref grade */
    r2 = (r * r) >> 12;                              /* squared ramp, Q12 */
    boost = 4096 + (int32_t)(((int64_t)extra * r2) >> 12);
    if (boost > s_hill_assist_max_q12) boost = s_hill_assist_max_q12;
    return boost;
}

/* ========================================================================
 * Force-feedback signal getters — PORT-ONLY. [FF SIGNALS 2026-06-15, #1]
 *
 * Per-slot driving-state signals consumed by td5_input.c to drive force-feedback
 * effects. All three are refreshed exactly once per fixed-30Hz sim tick by
 * td5_physics_update_ff_signals() (wired into td5_physics_tick AFTER the per-actor
 * integration loop, so they read settled post-integration state), then read any
 * number of times per render frame by the input layer. Backing store is a set of
 * file-static per-slot arrays sized TD5_MAX_RACER_SLOTS.
 *
 * DETERMINISM: every input is replicated/local sim state (body-frame lateral vs
 * longitudinal speed, current_gear, engine RPM, redline tuning) and integer math —
 * no rand()/wall-clock/render-rate — so the signals are identical on every lockstep
 * client and on a replay. Getters return 0 for out-of-range slots.
 *
 * Fields used (verified offsets, re/include/td5_actor_struct.h):
 *   drift level   : lateral_speed (+0x318) vs longitudinal_speed (+0x314)
 *   gear-change   : current_gear (+0x36B), sim-tick edge-detected
 *   at-redline    : engine_speed_accum (+0x310) vs redline tuning (+0x72)
 * ======================================================================== */

/* Below this body-frame longitudinal speed the car is too slow for a meaningful
 * drift reading — report 0. [FF drift over-sensitivity fix 2026-06-16] Raised
 * from 0x100: at a crawl the |lat|/|long| ratio has a tiny denominator, so the
 * slightest sideways velocity at the race-start launch spiked drift_level to max
 * and the gamepad buzzed continuously and never stopped. The drift buzz now needs
 * a genuine driving speed (so a low-speed launch / creep produces 0). */
#define FF_DRIFT_MIN_LONG_SPEED   0x8000
/* AND a meaningful ABSOLUTE sideways speed — a real slide, not steering/contact
 * jitter — before drift registers (belt-and-suspenders against the small-
 * denominator spike above). */
#define FF_DRIFT_MIN_LAT_SPEED    0x2000
/* Lateral/longitudinal slip ratio (Q8) below which we treat the car as tracking
 * straight (no drift). ~0x40 = 0.25 → ~14 deg of slide before drift registers. */
#define FF_DRIFT_RATIO_DEADZONE   0x40
/* Redline proximity window, percent: at-redline when rpm >= redline*(100-N)/100.
 * [FF redline tighten 2026-06-16] 5% fired the manual rev-limiter buzz across the
 * whole top ~5% of the tacho (felt like "moderate RPM"); this feature is the
 * limiter BOUNCE, so narrow it to the top 2% — engine_speed_accum (+0x310) and
 * redline (+0x72) are the SAME fields the speedo needle uses, so 2% ≈ the needle
 * pinned at the very top. (Gated manual-only at the consumer in td5_input.c.) */
#define FF_REDLINE_PCT            2

static int      s_ff_drift_level[TD5_MAX_RACER_SLOTS];   /* 0 = not drifting, else ~1..255 */
static uint32_t s_ff_gear_seq[TD5_MAX_RACER_SLOTS];      /* increments on each gear change */
static int      s_ff_at_redline[TD5_MAX_RACER_SLOTS];    /* 1 = engine near redline */
static uint8_t  s_ff_prev_gear[TD5_MAX_RACER_SLOTS];     /* last tick's current_gear (edge detect) */
static int      s_ff_prev_seeded[TD5_MAX_RACER_SLOTS];   /* 1 once prev_gear holds a real sample */

/* [item #5(4)] Air-time landing detection state. A landing fires when the actor
 * was airborne long enough last tick (airborne_frame_counter >= floor, all four
 * wheels off) and has any ground contact this tick. We track last tick's
 * airborne_frame_counter so the edge is detected once, and latch the downward
 * vertical impact speed (|linear_velocity_y|) sampled the moment before ground
 * contact resettles it. s_ff_land_seq is a monotonic per-slot id (0 = none). */
#define FF_LANDING_MIN_AIRBORNE_TICKS  4    /* must have been airborne >= this (~0.13s @30Hz) to count */
#define FF_LANDING_MIN_IMPACT        0x600  /* below this downward speed the landing is too soft to feel */
/* Own per-slot consecutive-all-airborne tick counter, RESET on ground contact —
 * unlike actor.airborne_frame_counter, which the engine never resets (it only
 * stops growing), so it can't gate air-time per individual jump. */
static int16_t  s_ff_air_ticks[TD5_MAX_RACER_SLOTS];        /* consecutive all-4-airborne ticks (reset on ground) */
static uint32_t s_ff_land_seq[TD5_MAX_RACER_SLOTS];          /* increments on each felt landing */
static int32_t  s_ff_land_impact[TD5_MAX_RACER_SLOTS];       /* last landing's vertical impact speed (>=0) */

/* TD5RE_FF_LANDING (cached): gate the air-time landing event. Default ON; "0"
 * disables landing detection (no seq bump, getter stays 0 -> no landing jolt). */
static int td5_ff_landing_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("TD5RE_FF_LANDING");
        s_on = (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                      e[0] == 'f' || e[0] == 'F')) ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "FF air-time landing: TD5RE_FF_LANDING=%d (min_air=%d ticks min_impact=%d)",
                  s_on, FF_LANDING_MIN_AIRBORNE_TICKS, FF_LANDING_MIN_IMPACT);
    }
    return s_on;
}

/* Forward decl: the carparam accessor get_phys() (and the PHYS_* tuning macros)
 * are defined further down this file, but the FF redline check below needs it. */
static inline int16_t *get_phys(TD5_Actor *a);

/* Refresh all per-slot FF signals for THIS sim tick. Called once per tick from
 * td5_physics_tick after integration; pure function of settled actor state. */
void td5_physics_update_ff_signals(void)
{
    int total, racer_cap, slot;

    if (!g_actor_table_base)
        return;

    total = td5_game_get_total_actor_count();
    if (total <= 0)
        return;

    /* Racer slots only (humans + AI) — traffic carries no FF. */
    racer_cap = (total < g_traffic_slot_base) ? total : g_traffic_slot_base;
    if (racer_cap > TD5_MAX_RACER_SLOTS)
        racer_cap = TD5_MAX_RACER_SLOTS;

    for (slot = 0; slot < racer_cap; slot++) {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        int16_t  *phys;
        int32_t   v_long, v_lat, abs_long, abs_lat;
        uint8_t   gear;

        /* --- Drift level: lateral slip relative to longitudinal speed. --- */
        v_long = a->longitudinal_speed;
        v_lat  = a->lateral_speed;
        abs_long = v_long < 0 ? -v_long : v_long;
        abs_lat  = v_lat  < 0 ? -v_lat  : v_lat;
        if (abs_long < FF_DRIFT_MIN_LONG_SPEED || abs_lat < FF_DRIFT_MIN_LAT_SPEED) {
            /* Too slow, or not enough absolute sideways speed, to be a real slide
             * (kills the race-start launch buzz from the small-denominator ratio). */
            s_ff_drift_level[slot] = 0;
        } else {
            /* ratio = |lat| / |long| in Q8 (0x100 = sliding as fast sideways as
             * forward). Subtract the straight-tracking deadzone, then scale the
             * remainder into ~1..255 (full scale ≈ a 45deg slide). */
            int32_t ratio = (int32_t)(((int64_t)abs_lat << 8) / (int64_t)abs_long);
            ratio -= FF_DRIFT_RATIO_DEADZONE;
            if (ratio <= 0) {
                s_ff_drift_level[slot] = 0;
            } else {
                int32_t lvl = ratio;            /* Q8 over the deadzone */
                if (lvl > 255) lvl = 255;       /* clamp to ~1..255 */
                if (lvl < 1)   lvl = 1;
                s_ff_drift_level[slot] = lvl;
            }
        }

        /* --- Gear-change sequence: edge-detect current_gear once per tick. --- */
        gear = a->current_gear;
        if (!s_ff_prev_seeded[slot]) {
            s_ff_prev_gear[slot] = gear;        /* seed; no spurious first-tick edge */
            s_ff_prev_seeded[slot] = 1;
        } else if (gear != s_ff_prev_gear[slot]) {
            s_ff_gear_seq[slot]++;
            s_ff_prev_gear[slot] = gear;
        }

        /* --- At-redline: engine RPM within FF_REDLINE_PCT% of the redline. --- */
        s_ff_at_redline[slot] = 0;
        phys = get_phys(a);
        if (phys) {
            int32_t rpm     = a->engine_speed_accum;
            int32_t redline = (int32_t)*(const int16_t *)((const uint8_t *)phys + 0x72); /* PHYS_REDLINE_RPM, defined later */
            if (redline > 0 &&
                rpm >= redline - (redline * FF_REDLINE_PCT) / 100) {
                s_ff_at_redline[slot] = 1;
            }
        }

        /* --- Air-time landing: airborne->grounded transition this tick. ---
         * wheel_contact_bitmask is per-wheel AIRBORNE (1=airborne); == 0x0F means
         * all four wheels off the ground. We accumulate consecutive all-airborne
         * ticks in s_ff_air_ticks (our own counter, RESET on ground contact). A
         * landing fires when the car was airborne long enough (>= the floor) up to
         * last tick and now has at least one wheel back on the ground. Sample the
         * DOWNWARD vertical impact speed: negative linear_velocity_y is descent, so
         * we take max(-vy, 0); a car that grounds while still rising registers no
         * jolt. The reset-on-ground counter makes the air-time gate fire once per
         * jump (not just the first jump of the race). */
        {
            int all_airborne = (a->wheel_contact_bitmask == 0x0F);
            if (!all_airborne &&
                td5_ff_landing_enabled() &&
                s_ff_air_ticks[slot] >= FF_LANDING_MIN_AIRBORNE_TICKS)
            {
                int32_t vy = a->linear_velocity_y;
                int32_t impact = (vy < 0) ? -vy : 0;   /* downward speed only */
                if (impact >= FF_LANDING_MIN_IMPACT) {
                    s_ff_land_impact[slot] = impact;
                    s_ff_land_seq[slot]++;
                    if (s_ff_land_seq[slot] == 0) s_ff_land_seq[slot] = 1; /* keep 0 == none */
                    TD5_LOG_I(LOG_TAG, "ff_landing: slot=%d air_ticks=%d impact=%d seq=%u",
                              slot, (int)s_ff_air_ticks[slot], impact, s_ff_land_seq[slot]);
                }
            }
            /* Update the consecutive air-time counter: grow while all-airborne,
             * reset the moment any wheel grounds (so the next jump starts at 0 and
             * the air-time floor is per-jump). Clamp to avoid int16 overflow on a
             * very long fall. */
            if (all_airborne) {
                if (s_ff_air_ticks[slot] < 0x7FF0) s_ff_air_ticks[slot]++;
            } else {
                s_ff_air_ticks[slot] = 0;
            }
        }
    }
}

int td5_physics_get_drift_level(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return 0;
    return s_ff_drift_level[slot];
}

uint32_t td5_physics_gear_change_seq(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return 0;
    return s_ff_gear_seq[slot];
}

int td5_physics_at_redline(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return 0;
    return s_ff_at_redline[slot];
}

/* [item #5(4)] Public getter (prototype in td5_physics.h) — the FF layer polls
 * this each frame and fires a decaying jolt on a new landing. Returns the
 * per-slot landing sequence id (0 = none yet) and fills *out_impact with the
 * last landing's downward vertical impact speed (raw 24.8 units, >= 0). Mirrors
 * td5_physics_get_crash_fx's contract; out-of-range / non-racer slots return 0. */
uint32_t td5_physics_get_landing_fx(int slot, int32_t *out_impact)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) {
        if (out_impact) *out_impact = 0;
        return 0;
    }
    if (out_impact) *out_impact = s_ff_land_impact[slot];
    return s_ff_land_seq[slot];
}

/* ========================================================================
 * Hard-difficulty AI catch-up — PORT-ONLY. [HARD CATCHUP 2026-06-15, item #13]
 *
 * On HARD difficulty, AI OPPONENTS that are BEHIND the human player get a drive-
 * torque boost so they catch up more aggressively (makes Hard harder). This is
 * the AI-opponent analogue of the human MP catch-up above and shares its
 * drive-torque chokepoint (td5_physics_compute_drive_torque) — but it applies
 * ONLY to AI racer slots and ONLY when g_difficulty_hard is set, so non-hard /
 * easy / normal play is byte-unchanged.
 *
 * The existing init-time NPC handicap (gRaceResultPointsTable, ~td5_physics_
 * init_vehicle_runtime) only adjusts HUMAN slots (g_race_slot_state==1) and is a
 * no-op until championship position is plumbed, so it is not the right lever for
 * "AI catches the player". A live drive-force boost keyed on the gap to the
 * player is, and like MP catchup it scales ACCELERATION only — the per-car speed
 * limit in the callers is untouched, so a boosted AI cannot warp past its top
 * speed; it just builds speed harder when trailing.
 *
 * DETERMINISM: a pure function of replicated sim state (track_span_high_water +
 * g_race_slot_state + g_difficulty_hard), recomputed once per fixed-30Hz tick —
 * identical on every lockstep client.
 *
 * KNOB: TD5RE_HARD_CATCHUP (env, cached once), DEFAULT 1 (ON). "0"/"n"/"f"
 * disables → AI multiplier stays 1.0 → byte-unchanged even on Hard.
 * ======================================================================== */

/* Gap (track spans) at which a trailing AI reaches the full hard boost. Matches
 * the MP catch-up window so both assists ease in over a comparable separation. */
#define HARD_CATCHUP_FULL_GAP_SPANS  30
/* Max hard AI boost at full gap, Q8. 0x100 + 0x48 = 1.28x drive force — a ~50%
 * stronger pull than the MP trailer cap (0x60 only realised at 35% strength ≈
 * 0x21); here it is unconditional on Hard so the field presses the player. */
#define HARD_CATCHUP_MAX_BOOST_Q8    0x48

static int      s_hard_catchup_cfg = -1;                 /* -1 unresolved, 0 off, 1 on */
static int32_t  s_hard_catchup_mult[TD5_MAX_RACER_SLOTS]; /* Q8 per-slot, 1.0 = inert */

/* Resolve TD5RE_HARD_CATCHUP once. Cached file-static; logged once. */
static void td5_physics_hard_catchup_config(void)
{
    const char *e;
    int i;

    if (s_hard_catchup_cfg >= 0)
        return;

    s_hard_catchup_cfg = 1;   /* default ON */
    e = getenv("TD5RE_HARD_CATCHUP");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        s_hard_catchup_cfg = 0;

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        s_hard_catchup_mult[i] = MP_CATCHUP_Q8_ONE;

    TD5_LOG_I(LOG_TAG,
              "hard_catchup: TD5RE_HARD_CATCHUP=%d (full_gap=%d spans, max_boost=%d/256, "
              "AI opponents only, hard difficulty only)",
              s_hard_catchup_cfg, HARD_CATCHUP_FULL_GAP_SPANS, HARD_CATCHUP_MAX_BOOST_Q8);
}

/* Recompute the per-slot hard AI catch-up multiplier for THIS sim tick. Pure
 * function of replicated sim state, so lockstep-deterministic. Called once per
 * tick alongside td5_physics_update_mp_catchup().
 *
 * Algorithm: find the human player's progress (max track_span_high_water over
 * HUMAN racer slots — robust to PlayerIsAI / split-screen). For each AI racer
 * slot BEHIND that, ramp a boost in with the gap, clamped at FULL_GAP. Humans,
 * traffic, and cars at/ahead of the player are left at 1.0. Inert unless Hard
 * AND the knob is on AND at least one human is present. */
static void td5_physics_update_hard_catchup(void)
{
    int slot, total, racer_cap;
    int32_t player_progress;
    int have_human;

    td5_physics_hard_catchup_config();

    /* Reset to neutral up front so disabled/early-return paths are inert. */
    for (slot = 0; slot < TD5_MAX_RACER_SLOTS; slot++)
        s_hard_catchup_mult[slot] = MP_CATCHUP_Q8_ONE;

    if (!s_hard_catchup_cfg || !g_difficulty_hard)
        return;
    if (!g_actor_table_base)
        return;

    total = td5_game_get_total_actor_count();
    if (total <= 0)
        return;
    racer_cap = (total < g_traffic_slot_base) ? total : g_traffic_slot_base;
    if (racer_cap > TD5_MAX_RACER_SLOTS)
        racer_cap = TD5_MAX_RACER_SLOTS;

    /* Pass 1: the player's progress = furthest-along HUMAN racer. */
    player_progress = INT32_MIN;
    have_human = 0;
    for (slot = 0; slot < racer_cap; slot++) {
        if (g_race_slot_state[slot] != 1)
            continue;  /* humans only */
        {
            TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
            int32_t prog = (int32_t)a->track_span_high_water;
            if (prog > player_progress)
                player_progress = prog;
            have_human = 1;
        }
    }
    if (!have_human)
        return;  /* nobody to catch up TO */

    /* Pass 2: per-AI multiplier from the gap behind the player. */
    for (slot = 0; slot < racer_cap; slot++) {
        TD5_Actor *a;
        int32_t my_prog, gap, g, boost;

        if (g_race_slot_state[slot] == 1)
            continue;  /* AI opponents only — never touch humans */

        a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        my_prog = (int32_t)a->track_span_high_water;
        gap = player_progress - my_prog;          /* spans behind the player */
        if (gap <= 0)
            continue;                             /* level/ahead: no boost */

        g = (gap < HARD_CATCHUP_FULL_GAP_SPANS) ? gap : HARD_CATCHUP_FULL_GAP_SPANS;
        boost = (HARD_CATCHUP_MAX_BOOST_Q8 * g) / HARD_CATCHUP_FULL_GAP_SPANS;
        s_hard_catchup_mult[slot] = MP_CATCHUP_Q8_ONE + boost;
    }
}

/* Q8 hard AI catch-up multiplier for `slot` (0x100 = 1.0). 1.0 for out-of-range
 * so callers can apply it unconditionally. */
static inline int32_t td5_physics_hard_catchup_mult(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return MP_CATCHUP_Q8_ONE;
    return s_hard_catchup_mult[slot];
}

/* ========================================================================
 * Acute high-speed crash feedback — PORT-ONLY. [CRASH FX 2026-06-15, item #12]
 *
 * The heavy-impact branch in td5_physics_apply_collision_impulse already does a
 * byte-faithful angular scatter + vertical lift for hits over 90000 when 3D
 * Collisions are enabled. On a genuinely HIGH-SPEED crash that does not feel
 * punchy enough: the car keeps most of its forward speed. For PLAYER actors we
 * add (a) an extra forward-speed scrub so a fast traffic crash visibly bleeds
 * speed, and (b) a per-slot crash-fx event other modules (HUD/VFX/audio) can
 * poll to fire a one-shot reaction. Both are gated by TD5RE_CRASH_FX (default
 * ON); when off, nothing is scrubbed/recorded and the getter returns 0 —
 * byte-identical to before.
 *
 * NOTE: the heavy-impact gate is `g_collisions_enabled == 0`, which (inverted
 * semantics) means the in-game "3D Collisions" option is ON — so acute crashes
 * only fire when the player has collisions enabled, consistent with the
 * existing gate.
 *
 * "Acute" threshold: the heavy branch already runs over 90000; the high-impact
 * vertical lift saturates the port's 200000 clamp at impact_mag == 1,200,000.
 * Observed v2v_heavy_scatter logs put ordinary traffic taps in the ~90k–180k
 * range and full-speed head-ons well into the hundreds of thousands, so 250000
 * (~2.8x the heavy floor) selects a genuinely fast hit without firing on every
 * fender-bender.
 * ======================================================================== */
#define CRASH_FX_ACUTE_MAG   250000   /* impact_mag above which a player hit is "acute" */
/* [POLICE rewrite 2026-06-19] Approach-speed (iVar11) into a wall above which a
 * traffic car / cop / chased racer "breaks down" (halt + smoke; ends a pursuit).
 * Deliberately high so only a genuine head-on crash counts, not a scrape.
 * Tunable from drive-test. */
#define COP_WALL_BREAK_VPERP  20000
/* V2V impact magnitude above which a TRAFFIC car / cop / chased racer breaks
 * down. Lower than CRASH_FX_ACUTE_MAG (the player crash-fx) because traffic
 * crashes at lower speed, but still above the ordinary-tap range (~90k-180k,
 * per the v2v_heavy_scatter logs) so a normal bump doesn't wreck a car — only a
 * strong crash does. */
#define COP_BREAK_MAG  200000
/* [#5 FATAL NPC COLLISION 2026-06-20] When a TRAFFIC or COP car is involved in a
 * V2V hit, the dramatic "3D collision" heavy scatter+lift AND the wreck (break
 * down -> park, roof smoke, immobile) trigger at this LOWER magnitude than the
 * faithful racer-vs-racer 90000 floor, so crashing into traffic/cops feels fatal
 * and totals them much more readily (user request). Racer-vs-racer keeps 90000.
 * 60000 sits just under the ordinary-tap floor so a solid hit wrecks the NPC but
 * a gentle nudge does not. Tunable via TD5RE_NPC_FATAL_MAG. */
#define NPC_FATAL_MAG_DEFAULT  60000
static int32_t npc_fatal_mag(void) {
    static int32_t v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_NPC_FATAL_MAG");
        long m = (e && e[0]) ? strtol(e, NULL, 10) : NPC_FATAL_MAG_DEFAULT;
        if (m < 1000)    m = 1000;
        if (m > 1000000) m = 1000000;
        v = (int32_t)m;
    }
    return v;
}
/* Forward-speed scrub on an acute player hit, Q8 (0x100 = 1.0 = keep all speed).
 * 0x100 - 0x50 = 0xB0/256 ≈ 0.6875, i.e. shed ~31% of planar speed. Vertical
 * lift (linear_velocity_y) is intentionally left untouched. */
#define CRASH_FX_SCRUB_KEEP_Q8   0xB0

/* Per-slot crash-fx event state (racer slots only). Written by the acute branch
 * in td5_physics_apply_collision_impulse, read by td5_physics_get_crash_fx().
 * s_crash_fx_seq[slot] is a monotonic id bumped once per recorded acute crash
 * (0 = none yet); _mag holds that crash's impact magnitude; _tick the global sim
 * tick it happened on (for an age in ticks). */
static uint32_t s_crash_fx_seq [TD5_MAX_RACER_SLOTS];
static int32_t  s_crash_fx_mag [TD5_MAX_RACER_SLOTS];
static int32_t  s_crash_fx_tick[TD5_MAX_RACER_SLOTS];

/* [#1 WRECK STAND-STILL 2026-06-21] Free-slide window (ticks) for a broken-down
 * traffic car: armed by a real V2V ram in the impulse resolver, counted down in
 * td5_physics_update_traffic. While > 0 the wreck coasts (so the player can shove
 * it aside, issue #2); at 0 it is anchored and stands still. Sized for all (incl.
 * traffic) slots. */
#define WRECK_PUSH_TICKS       45      /* ~1.5s of free slide after a ram */
#define WRECK_PUSH_MIN_IMPACT  0x400   /* min V2V impact_mag that counts as a real shove */
static int16_t s_wreck_push_ticks[TD5_MAX_TOTAL_ACTORS];

/* Resolve TD5RE_CRASH_FX once. Cached file-static; logged once. Default ON;
 * "0"/"n"/"f" disables (no scrub, no event recording, getter returns 0). */
static int td5_physics_crash_fx_enabled(void)
{
    static int s_crash_fx = -1;
    if (s_crash_fx < 0) {
        const char *e = getenv("TD5RE_CRASH_FX");
        s_crash_fx = (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                            e[0] == 'f' || e[0] == 'F')) ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "crash_fx: TD5RE_CRASH_FX=%d (acute_mag=%d scrub_keep=%d/256)",
                  s_crash_fx, CRASH_FX_ACUTE_MAG, CRASH_FX_SCRUB_KEEP_Q8);
    }
    return s_crash_fx;
}

/* Public getter (prototype in td5_physics.h) — other modules poll this every
 * frame to drive a one-shot crash reaction (HUD shake, VFX, audio). Returns the
 * per-slot crash sequence id (0 = no acute crash yet), and fills *out_mag with
 * the last acute impact magnitude and *out_age with the sim ticks elapsed since
 * the crash (0 = same tick). Null out-params are tolerated; out-of-range or
 * non-racer slots return 0. Safe to call every frame. */
uint32_t td5_physics_get_crash_fx(int slot, int32_t *out_mag, int *out_age)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) {
        if (out_mag) *out_mag = 0;
        if (out_age) *out_age = 0;
        return 0;
    }
    if (out_mag) *out_mag = s_crash_fx_mag[slot];
    if (out_age) {
        int age = (int)g_td5.simulation_tick_counter - s_crash_fx_tick[slot];
        if (age < 0) age = 0;   /* defensive: counter reset between race sessions */
        *out_age = age;
    }
    return s_crash_fx_seq[slot];
}

/* Apply the acute-crash effect to one PLAYER actor: scrub planar (forward)
 * speed by CRASH_FX_SCRUB_KEEP_Q8 (vertical lift untouched) and record a
 * per-slot crash-fx event (impact_mag, sim tick, bumped sequence id). Caller
 * must already have confirmed the hit is acute and the actor is a player; this
 * is a no-op when TD5RE_CRASH_FX is off. */
static void td5_physics_apply_acute_crash_fx(TD5_Actor *actor, int32_t impact_mag)
{
    int slot;
    if (!td5_physics_crash_fx_enabled()) return;
    if (!actor) return;
    slot = actor->slot_index;
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;

    /* Extra forward-speed scrub: bleed the planar velocity by ~31% so a high-
     * speed crash visibly sheds speed. Vertical lift (linear_velocity_y) is left
     * alone so the launch from the heavy branch above is preserved. Same biased-
     * toward-zero signed >>8 idiom the drive pipeline uses. */
    {
        int64_t sx = (int64_t)actor->linear_velocity_x * CRASH_FX_SCRUB_KEEP_Q8;
        int64_t sz = (int64_t)actor->linear_velocity_z * CRASH_FX_SCRUB_KEEP_Q8;
        actor->linear_velocity_x = (int32_t)((sx + ((sx >> 63) & 0xFF)) >> 8);
        actor->linear_velocity_z = (int32_t)((sz + ((sz >> 63) & 0xFF)) >> 8);
    }

    /* Record the crash-fx event for HUD/VFX/audio to poll. */
    s_crash_fx_mag[slot]  = impact_mag;
    s_crash_fx_tick[slot] = (int32_t)g_td5.simulation_tick_counter;
    s_crash_fx_seq[slot]++;
    if (s_crash_fx_seq[slot] == 0) s_crash_fx_seq[slot] = 1;  /* keep 0 == "none" */

    TD5_LOG_I(LOG_TAG, "crash_fx acute: slot=%d mag=%d seq=%u tick=%d scrub_keep=%d/256",
              slot, impact_mag, s_crash_fx_seq[slot], s_crash_fx_tick[slot],
              CRASH_FX_SCRUB_KEEP_Q8);
}

/* [S18] Per-slot consecutive-ticks-over-attitude-limit counter, used ONLY on
 * migrated TD6 tracks to debounce the MODE-0 recovery latch (see
 * td5_physics_clamp_attitude). Faithful TD5 tracks never touch this. */
#define S18_TD6_RECOVERY_DEBOUNCE_TICKS 8   /* ~0.27s at 30Hz before recovery latches on TD6 */
static uint8_t s_td6_recovery_debounce[TD5_MAX_TOTAL_ACTORS];
static void integrate_traffic_pose(TD5_Actor *actor);  /* forward decl */
static inline void td5_transform_short_vec3_by_render_matrix_rounded(
    const int16_t param_1[3], int32_t param_2[3], const float matrix[12]);  /* fwd decl (def @ 7240) */
static void process_traffic_segment_edge(TD5_Actor *actor, int slot);  /* forward decl */
static void process_traffic_route_advance(TD5_Actor *actor, int slot);  /* forward decl */
static void process_traffic_forward_checkpoint_pass(TD5_Actor *actor, int slot);  /* forward decl */

/* Per-slot previous-frame wheel transform results (pre-snap) for gap_270 delta.
 * Using post-snap positions causes huge Y deltas because snap Y != transform Y. */
static int32_t s_prev_wheel_tx[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] X transform result */
static int32_t s_prev_wheel_ty[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] Y transform result */
static int32_t s_prev_wheel_tz[TD5_MAX_TOTAL_ACTORS][4];  /* [slot][wheel] Z transform result */
static uint8_t s_prev_wheel_valid[TD5_MAX_TOTAL_ACTORS];  /* per-slot: 1 if previous transform is valid */

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
/* Per-bucket chain-walk cap (Phase 2 of td5_physics_resolve_vehicle_contacts).
 * Must be >= the maximum number of actors that can land in one span bucket,
 * which is the full active-actor count. The original binary capped the field
 * at 12 (6 racers + 6 traffic), so a fixed 17 always cleared the real chain.
 * The N-way expansion raised TD5_MAX_TOTAL_ACTORS to 22, so a tight pack of
 * >17 cars in a single ~12-span window silently dropped the trailing chain
 * nodes -> some pairs were never tested -> cars passed through each other.
 * Scaling the cap with the actor cap guarantees no real chain is truncated.
 * [S08 car-vs-car coverage 2026-06-04] */
#define COLLISION_MAX_WALK      TD5_MAX_TOTAL_ACTORS
static uint8_t s_collision_grid[COLLISION_GRID_SIZE];

/* --- Anti-tunnel car-vs-car depenetration (S17 2026-06-05) ---
 * Number of outer relaxation rounds the position-only separation pass runs
 * per frame. Each round pushes every still-overlapping pair fully apart along
 * the line of centres; extra rounds let multi-car packs (A shoved into C while
 * separating from B) settle. This is Gauss-Seidel relaxation of separation
 * constraints, so a dense N-car pile-up needs several sweeps to converge --
 * 24 fully clears even a 12-racer heap in one frame. The loop breaks as soon
 * as no pair moves, so the common 1-2 contact case still costs only 1-2 rounds. */
#define ANTITUNNEL_RELAX_ROUNDS  24
/* Clamp on the per-call separation magnitude (display units). Bounds the move
 * for a pathological deep overlap so the reposition can't teleport a car;
 * normal car overlaps are well under this, so they separate in one round. */
#define ANTITUNNEL_MAX_DEPTH     256

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

/* Force a float expression to round to float32 precision via an x87 FSTP-to-
 * memory + reload. Mirrors orig 0x0042E1E0's intermediate-product spill
 * pattern: each `FMUL ...; FSTP [esp+N]; FLD [esp+N]; FMUL ...` truncates
 * the 80-bit FPU register to a 32-bit memory slot before the next operand
 * arrives. Plain `volatile float` is NOT enough on i686 + MinGW x87 (GCC
 * PR323) — GCC keeps the intermediate at 80 bit on the FPU stack. The
 * explicit inline asm forces the spill exactly where orig does. */
#define TD5_F32_SPILL(expr) ({                          \
    float _td5_spill_v = (expr);                         \
    float _td5_spill_t;                                  \
    __asm__ volatile (                                   \
        "fstps %0"                                       \
        : "=m" (_td5_spill_t)                            \
        : "t"  (_td5_spill_v)                            \
        : "st");                                         \
    _td5_spill_t;                                        \
})

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

static int32_t cos_fixed12(int32_t angle)
{
    return (int32_t)CosFixed12bit((unsigned int)angle);
}

static int32_t sin_fixed12(int32_t angle)
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

static int32_t atan2_fixed12(int32_t dx, int32_t dz)
{
    return AngleFromVector12(dx, dz) & 0xFFF;
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

/* [#10 NEAR-WALL CAMERA ZOOM 2026-06-19] Per-racer-slot countdown (in sim ticks)
 * set when a wheel probe clips a wall deeply. The chase camera zooms in while
 * this is > 0 so the wall geometry cannot occlude the car. Cosmetic (camera
 * only) — decremented in the camera per-tick update. Defined here because the
 * wall-contact response is the producer. */
int16_t g_actor_near_wall[TD5_MAX_RACER_SLOTS];

void td5_physics_wall_response(TD5_Actor *actor, int32_t wall_angle,
                               int32_t penetration, int side,
                               int32_t probe_x_fp8, int32_t probe_z_fp8)
{
    /* [#10] A deep wall clip arms the chase-cam zoom for this racer. penetration
     * is negative when the probe is outside the rail; the more negative, the
     * deeper the clip. Racer slots only (traffic has no camera). Knob
     * TD5RE_WALL_CAM_ZOOM (default on); "0" disables the zoom entirely. */
    if (actor->slot_index >= 0 && actor->slot_index < TD5_MAX_RACER_SLOTS) {
        static int s_wall_cam = -1;
        static int s_wall_cam_pen = 0;
        if (s_wall_cam < 0) {
            const char *e = getenv("TD5RE_WALL_CAM_ZOOM");
            s_wall_cam = (!e || e[0] != '0') ? 1 : 0;
            const char *p = getenv("TD5RE_WALL_CAM_PEN");
            /* [#R3-4 2026-06-19] Lowered 100 -> 40: the zoom only fired sometimes
             * (esp. reversing into a wall, where contact is gentler -> shallower
             * penetration). 40 arms on almost any real wall contact. */
            s_wall_cam_pen = (p && p[0]) ? atoi(p) : 40;   /* min penetration to arm */
            if (s_wall_cam_pen < 10) s_wall_cam_pen = 10;
        }
        /* [#R11 2026-06-19] Lowered the arm threshold (was -250, rarely hit) and
         * lengthened the hold (was 8 ticks — too short for the radius spring to
         * visibly settle) so the zoom actually engages on a normal wall scrape. */
        if (s_wall_cam && penetration < -s_wall_cam_pen)
            g_actor_near_wall[actor->slot_index] = 22;  /* ~0.7s hold at 30Hz */
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
        /* [POLICE rewrite 2026-06-19] A high-speed head-on wall hit breaks down
         * a CHASED racer (ending the pursuit) or a CHASING cop. iVar11 is the
         * approach speed into the wall; the threshold is high so only a genuine
         * crash counts. Ordinary traffic is left alone. */
        if (iVar11 > COP_WALL_BREAK_VPERP) {
            int wslot = actor->slot_index;
            /* Any TRAFFIC car / cop that slams a wall hard breaks down (halt +
             * smoke); so does a chased racer (ends the pursuit). Un-chased racers
             * keep control. */
            if (wslot >= g_traffic_slot_base || td5_ai_actor_is_pursued(wslot)) {
                td5_ai_mark_actor_broken_down(wslot);
                TD5_LOG_I(LOG_TAG, "wall_break: slot=%d vperp=%d (broke down)",
                          wslot, iVar11);
            }
        }
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

        /* [FIX 2026-05-24 OVERSIGHT: wall_impact_sfx_ff_missing; orig 0x00406B70..0x00406CB7]
         * After computing the wall impulse, the orig plays a sound + FF rumble
         * if v_perp (= iVar10) > 0x3200. Two SFX tiers based on impulse
         * magnitude; FF rumble channel selected by route heading delta.
         * Visual / audio only — no sim feedback. */
        if (v_perp > 0x3200) {
            /* [#10 telemetry] A meaningful wall impact (same threshold the
             * original uses to play the crunch SFX). Flag a collision for this
             * racer's per-tick edge-detector; the accumulate pass turns a
             * contiguous run of flagged ticks into a single counted collision. */
            td5_physics_mark_collision((int)actor->slot_index);

            int32_t pitch_arg = v_perp - 0x2000;
            if (pitch_arg < 0x400) pitch_arg = 0x400;
            else if (pitch_arg > 0x800) pitch_arg = 0x800;
            int variant = (v_perp < 0x19001) ? 0x16 : 0x1b;
            int mag     = (v_perp < 0x19001) ? 0x5622 : 0x2198;
            int volume  = (v_perp < 0x19001) ? 1 : 4;
            int32_t wpos[3] = { actor->world_pos.x, actor->world_pos.y, actor->world_pos.z };
            /* Port td5_sound_play_at_position param order matches orig:
             * (variant, pitch, mag, &pos, volume_or_variant_count). */
            td5_sound_play_at_position(variant, pitch_arg, mag, wpos, volume);

            /* DecayUltimateVariantTimer(actor, 1) [orig 0x0040A440]: inlined to
             * match existing call sites at td5_physics.c:5410/5468. */
            if (g_td5.special_encounter_enabled == 4 && actor->finish_time == 0) {
                if (actor->clean_driving_score > 0) actor->clean_driving_score -= 1;
                if (actor->clean_driving_score < 0) actor->clean_driving_score  = 0;
            }

            /* Force-feedback rumble: magnitude = (v_perp + sign-bit&3) >> 2,
             * capped at 100000. Channel chosen by route heading delta XOR'd
             * with the contact side when |heading_delta - 0x800| < 0x400. */
            int32_t ff_mag = (v_perp + ((v_perp >> 31) & 3)) >> 2;
            if (ff_mag > 99999) ff_mag = 100000;

            /* [FIX 2026-05-25 crash-compute-heading-delta-stride]: the route-state
             * stride is RS_STRIDE_DWORDS = 0x47 DWORDS (= 0x11C bytes), not 0x47
             * BYTES.  Previous "(uint8_t *)g_route_data + slot*0x47" landed in
             * garbage for any slot > 0; td5_compute_heading_delta then read
             * rs[0x35] (RS_SLOT_INDEX) from that garbage, multiplied by 0x388
             * (actor stride) and crashed at "actor + 0x1F4".  Reproduced as
             * "CRASH at EIP=0071CB97" on Scotland span 191/203 and Moscow
             * span 224 (high-velocity wall impacts triggering this tail block).
             * Use td5_ai_get_route_state() which already applies the correct
             * stride. */
            extern int32_t *td5_ai_get_route_state(int slot);
            uint32_t hd = 0;
            {
                int32_t *rs = td5_ai_get_route_state((int)actor->slot_index);
                if (rs) hd = td5_compute_heading_delta(rs);
            }
            uint32_t local_flags = (side < 0) ? 0u : (uint32_t)(side + 1);
            if ((int32_t)hd > 0x3FF && (int32_t)hd < 0xC00) local_flags ^= 3;
            /* [item #5(1)(2)] Route the wall impact through td5_input_ff_collision
             * (decaying directional pulse) instead of the old persistent
             * td5_input_ff_play_effect: local_flags 1/2 encode the contact side, so
             * map them to the LEFT (0x10) / RIGHT (0x40) contact-side bits the FF
             * layer uses to pick the impacted-side motor. actor_b_slot = -1 (single
             * actor, ignored). The raw ff_mag is in the same 0..100000 domain
             * td5_input_ff_collision expects (it divides by COLLISION_DIV). */
            if (local_flags == 1) {
                td5_input_ff_collision(0x10, (int)actor->slot_index, -1, ff_mag);
            } else if (local_flags == 2) {
                td5_input_ff_collision(0x40, (int)actor->slot_index, -1, ff_mag);
            }
        }
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
/* Named field offsets for the two per-car tuning tables the accessors above
 * read. PHYS_* index the vehicle-physics table entry (get_phys, mostly int16
 * fields); CDEF_* index the cardef entry (get_cardef). Values mirror the
 * original binary's table layouts — the listing-anchored comments at the use
 * sites are the provenance. */
#define PHYS_INERTIA_YAW        0x20   /* int32 */
#define PHYS_HALF_WHEELBASE     0x24   /* int32 */
#define PHYS_FRONT_WEIGHT       0x28
#define PHYS_REAR_WEIGHT        0x2A
#define PHYS_TIRE_GRIP_COEFF    0x2C
#define PHYS_GEAR_RATIO_BASE    0x2E   /* int16[gear], stride 2 */
#define PHYS_GEAR_UPSHIFT_BASE  0x3E   /* int16[gear] RPM threshold, stride 2 */
#define PHYS_GEAR_DOWNSHIFT_BASE 0x4E  /* int16[gear] RPM threshold, stride 2 */
#define PHYS_SUSP_POS_DAMP      0x5E
#define PHYS_SUSP_VEL_DAMP      0x60
#define PHYS_SUSP_SPRING        0x62
#define PHYS_SUSP_TRAVEL_LIM    0x64
#define PHYS_SUSP_LOAD_SCALE    0x66
#define PHYS_DRIVE_TORQUE_MULT  0x68
#define PHYS_DAMP_COEFF_TURN    0x6A   /* picked when steering hard in gear >= 2 */
#define PHYS_DAMP_COEFF_BASE    0x6C   /* picked when near-straight or gear < 2 */
#define PHYS_BRAKE_FRONT        0x6E
#define PHYS_BRAKE_REAR         0x70
#define PHYS_REDLINE_RPM        0x72
#define PHYS_TOP_SPEED          0x74
#define PHYS_DRIVETRAIN_TYPE    0x76
#define PHYS_SPEED_SCALE        0x78
#define PHYS_HANDBRAKE_MOD      0x7A
#define PHYS_SLIP_COUPLING      0x7C

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
static inline int32_t phys_top_speed_rating(TD5_Actor *actor) {
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
    return rating;
}

#define CDEF_FRONT_Z_EXTENT     0x04   /* positive */
#define CDEF_HALF_WIDTH         0x08   /* positive */
#define CDEF_REAR_Z_EXTENT      0x14   /* negative */
#define CDEF_COLLISION_RADIUS   0x80
#define CDEF_SUSP_REF_HEIGHT    0x82
#define CDEF_HEIGHT_OFFSET      0x86
#define CDEF_WHEEL_Y_BASE       0x42   /* per-wheel, stride 8 */

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
        float dy = (float)(cur->y - prev->y);
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
    if (s < 0) { const char *e = getenv("TD5RE_TD6_PROPS"); s = (e && e[0] == '0') ? 0 : 1; }
    return s;
}

/* [#20 pushable] A/B knob: TD5RE_TD6_PUSH=0 reverts to break-everything-on-contact.
 * Default on: props (MOV byte-6 mass != 0) are SHOVED and the car's speed penalty
 * scales with mass (light props barely slow it); a prop only BREAKS on a hard,
 * heavy impulse. Matches the original (RE: mass=0 immovable, break when impulse>30000). */
static int td6_push_enabled(void)
{
    static int s = -1;
    if (s < 0) { const char *e = getenv("TD5RE_TD6_PUSH"); s = (e && e[0] == '0') ? 0 : 1; }
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
        seg_ax = (cx >> 8) - (int32_t)(fwx * (float)half_len);
        seg_az = (cz >> 8) - (int32_t)(fwz * (float)half_len);
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
        pxw = px >> 8; pzw = pz >> 8;
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
                int32_t dx = (actor->wheel_contact_pos[w].x - px) >> 8;
                int32_t dz = (actor->wheel_contact_pos[w].z - pz) >> 8;
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

    /* [STUCK RECOVERY 2026-06-15] After this tick's integration + contact
     * resolution have settled, run the per-local-human stuck-recovery driver:
     * manual (R / SELECT) reposition. Deterministic (sim-tick cadence,
     * replicated/local state); restricted to non-network play for v1. Inert
     * when TD5RE_STUCK_RECOVERY=0. */
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
        int32_t speed_raw = (int32_t)(mag >> 8);                          /* raw display unit */
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
        if (v < 0) { const char *e = getenv("TD5RE_COUNTDOWN_REV_ALL"); v = (e && e[0] == '0') ? 0 : 1; }
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
    if (actor->vehicle_mode == 0 && actor->slot_index >= 6) {
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
        } else if (actor->slot_index < g_traffic_slot_base && g_race_slot_state[actor->slot_index] == 1) {
            /* Human player — listing 0x0040685C tests `state == 1` strictly,
             * not `state != 0`. [Audit D13 — tightened 2026-05-14.] */
            td5_physics_update_player(actor);
        } else {
            /* AI racer (slot < 6). Traffic handled by the slot>=6 branch above. */
            td5_physics_update_ai(actor);
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
        actor->euler_accum.roll  = (int32_t)actor->display_angles.roll  << 8;
        actor->euler_accum.yaw   = (int32_t)actor->display_angles.yaw   << 8;
        actor->euler_accum.pitch = (int32_t)actor->display_angles.pitch << 8;
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
    if (actor->slot_index >= 6 && actor->vehicle_mode != 1) {
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
    } else if (actor->slot_index < 6) {
        /* Racer path: full gravity + per-wheel ground snap.
         * Run even during countdown (paused) so ground-snap keeps the car
         * at the correct height above the road surface.
         */
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
    if (actor->slot_index < 6) {
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

void td5_physics_update_player(TD5_Actor *actor)
{
    int16_t *phys = get_phys(actor);
    if (!phys) return;


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
    int32_t grip[4];
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
    int32_t front_load = ((rear_weight << 8) / total_weight);
    front_load = front_load * (half_wb - susp_defl) / full_wb;
    int32_t rear_load = ((front_weight << 8) / total_weight);
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
    int32_t heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
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
            int32_t speed_limit = phys_top_speed_rating(actor) << 8;
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
            int32_t speed_limit = phys_top_speed_rating(actor) << 8;
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
            int32_t bf = (brake_front * coast_throttle) >> 8;
            int32_t br = (brake_rear  * coast_throttle) >> 8;
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
            grip_limit_f = (grip_limit_f * tire_grip_coeff) >> 8;

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
            int32_t slip_shift = delta_abs >> 8;
            /* Wheelspin CLEAR [CONFIRMED @ 0x00404030] — engine and ground
             * speed have re-synced; drop scf so next tick uses UESA. */
            if (slip_shift < 0x41) {
                actor->surface_contact_flags = 0;
            }
            int32_t coupled = (slip_shift * slip_coupling) >> 8;
            int32_t lat     = rear_lat_force;       /* REAR lateral (orig +0x31C) */
            int32_t latSh   = lat >> 8;
            int32_t latMix  = (latSh * lat) >> 8;
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
            rear_lat_force = ((grip_limit_f << 8) / combined * rear_lat_force) >> 8;
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
            grip_limit_r = (grip_limit_r * tire_grip_coeff) >> 8;

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
            int32_t slip_shift = delta_abs >> 8;
            if (slip_shift < 0x41) {
                actor->surface_contact_flags = 0;
            }
            int32_t coupled = (slip_shift * slip_coupling) >> 8;
            int32_t lat     = front_lat_force;      /* FRONT lateral (orig +0x320) */
            int32_t latSh   = lat >> 8;
            int32_t latMix  = (latSh * lat) >> 8;
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
            front_lat_force = ((grip_limit_r << 8) / combined * front_lat_force) >> 8;
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

        if (yaw_torque > TD5_YAW_TORQUE_MAX) yaw_torque = TD5_YAW_TORQUE_MAX;
        if (yaw_torque < -TD5_YAW_TORQUE_MAX) yaw_torque = -TD5_YAW_TORQUE_MAX;

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
                const char *e = getenv("TD5RE_GRIP_DRIFT_FIX");
                s_gdf = (e && e[0] == '0') ? 0 : 1;
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
                    int32_t damp_q8 = (excess * 24) >> 8;
                    if (damp_q8 > 38) damp_q8 = 38;       /* cap ~0.15 (38/256) */
                    int32_t yd = (surplus * damp_q8) >> 8;
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
                    const char *eg = getenv("TD5RE_SLOPE_GEAR1_FREE");
                    s_gear1_free = (!eg || eg[0] != '0') ? 1 : 0;
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
        int32_t speed_lim = (int32_t)PHYS_S(actor, PHYS_TOP_SPEED) << 8;
        int32_t vel_cap = speed_lim * 2;  /* 2x backstop (was 1x); walls do real containment */
        int32_t vxh = actor->linear_velocity_x >> 8;
        int32_t vzh = actor->linear_velocity_z >> 8;
        int32_t mag_sq = vxh * vxh + vzh * vzh;
        int32_t cap_sq = (vel_cap >> 8) * (vel_cap >> 8);
        if (mag_sq > cap_sq && mag_sq > 0) {
            int32_t mag = td5_isqrt(mag_sq);
            int32_t cap_h = vel_cap >> 8;
            actor->linear_velocity_x = (int32_t)((int64_t)actor->linear_velocity_x * cap_h / mag);
            actor->linear_velocity_z = (int32_t)((int64_t)actor->linear_velocity_z * cap_h / mag);
            /* Throttled diagnostic (slot 0, every 120 frames) so a drive-test
             * can tell whether the 2x backstop is engaging — same pattern as 14c. */
            if (actor->slot_index == 0 && (actor->frame_counter % 120u) == 0u) {
                TD5_LOG_I(LOG_TAG, "vel_backstop_2x: fired mag=%d cap_h=%d speed_lim_h=%d",
                          mag, cap_h, speed_lim >> 8);
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
    int32_t front_load = ((rear_weight << 8) / total_weight) * (half_wb - susp_defl) / half_wb;
    int32_t rear_load  = ((front_weight << 8) / total_weight) * (half_wb + susp_defl) / half_wb;

    /* Grip from surface friction * load [CONFIRMED @ 0x4050B8] */
    int32_t sf = phys_surface_grip(surface);   /* [task#15] TD6-aware */
    int32_t grip_front = (sf * front_load + 128) >> 8;
    int32_t grip_rear  = (sf * rear_load + 128) >> 8;

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
    int32_t speed_limit = phys_top_speed_rating(actor) << 8;
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
        int32_t tire_grip = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
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
     * ram left it inside its free-slide window (s_wreck_push_ticks), during which
     * it coasts so the player can shove it aside (issue #2). Knob TD5RE_WRECK_IMMOBILE. */
    int wreck_no_throttle = 0;
    if (wreck_immobile_enabled() &&
        td5_ai_actor_is_broken_down(actor->slot_index)) {
        int si = (int)actor->slot_index;
        if (si >= 0 && si < TD5_MAX_TOTAL_ACTORS && s_wreck_push_ticks[si] > 0) {
            s_wreck_push_ticks[si]--;       /* sliding from a fresh ram -> coast */
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
    if (wreck_no_throttle) throttle = 0;   /* [#1] a wreck never drives itself */

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
    int32_t half_w_a  = (int32_t)CDEF_S(a, CDEF_HALF_WIDTH);  /* half-width (positive) */
    int32_t front_z_a = (int32_t)CDEF_S(a, CDEF_FRONT_Z_EXTENT);  /* front-Z extent (positive) */
    int32_t rear_z_a  = (int32_t)CDEF_S(a, CDEF_REAR_Z_EXTENT);  /* rear-Z extent (negative) */
    int32_t half_w_b  = (int32_t)CDEF_S(b, CDEF_HALF_WIDTH);
    int32_t front_z_b = (int32_t)CDEF_S(b, CDEF_FRONT_Z_EXTENT);
    int32_t rear_z_b  = (int32_t)CDEF_S(b, CDEF_REAR_Z_EXTENT);

    /* [task #14 / item #4] Shrink each car's collision box to match its visible
     * mesh. The cardef OBB extents are authored noticeably larger than the model,
     * so a car "crashes" onto another before visibly touching it ("invisible
     * crash"). Each actor is scaled by its own factor: TRAFFIC keeps the
     * TD5RE_TRAFFIC_HITBOX_SCALE shrink (0.8); RACERS (player + AI opponents) now
     * use TD5RE_HITBOX_SCALE (0.85, item #4) instead of the byte-faithful full box.
     * Self-consistent: every corner / penetration value below derives from these
     * scaled extents. Each scale returns 1.0 when its knob is off. */
    {
        float hbs_a = actor_hitbox_scale(a);
        float hbs_b = actor_hitbox_scale(b);
        if (hbs_a < 1.0f) {
            half_w_a  = (int32_t)((float)half_w_a  * hbs_a);
            front_z_a = (int32_t)((float)front_z_a * hbs_a);
            rear_z_a  = (int32_t)((float)rear_z_a  * hbs_a);
        }
        if (hbs_b < 1.0f) {
            half_w_b  = (int32_t)((float)half_w_b  * hbs_b);
            front_z_b = (int32_t)((float)front_z_b * hbs_b);
            rear_z_b  = (int32_t)((float)rear_z_b  * hbs_b);
        }
    }

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
    /* Pilot snapshot stores raw 24.8 fp coords for direct comparison with the
     * Frida capture of the original callee. */

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
        for (int _i = 0; _i < 8; _i++) {
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

/* [task #9] TD5RE_TRAFFIC_HIT_TAME (default ON) — when a TRAFFIC car (slot >=
 * g_traffic_slot_base) is involved in a V2V collision, constrain the response so
 * it does NOT get launched into the air and cannot sink below the ground:
 *   - zero/clamp the vertical (Y) component injected by the collision impulse,
 *   - clamp the overall impulse magnitude that drives the crash-spin lift,
 *   - clamp post-collision world_pos.y so a traffic car never ends up under the
 *     track surface (the "clips through the ground" half of the report).
 * "0" reverts to the byte-faithful (launch + lift) response. */
static int traffic_hit_tame_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_HIT_TAME");
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "traffic_hit_tame: TD5RE_TRAFFIC_HIT_TAME=%d", s);
    }
    return s;
}

/* [#1 WRECK IMMOBILE 2026-06-21] A broken-down (wrecked) car must sit still, not
 * reverse. The brake path's port-only low-speed REVERSE hand-off (gear=REVERSE +
 * clear brake) turns the wreck's negative stop-command into sustained backward
 * drive — the reported "broken cars reverse and keep accelerating backwards".
 * Suppress that hand-off for a broken car so it just brakes to rest and stays
 * (it is still pushable by a fresh collision). Default ON; TD5RE_WRECK_IMMOBILE=0
 * reverts. */
static int wreck_immobile_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_WRECK_IMMOBILE");
        s = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "wreck_immobile: TD5RE_WRECK_IMMOBILE=%d", s);
    }
    return s;
}

/* [#2 LIGHT WRECK 2026-06-21] Make a broken-down car easy to shove out of the
 * way: scale its V2V mass term (+0x88, which behaves as an INVERSE mass here —
 * a bigger value = lighter / more mobile, so the wreck gains MORE velocity from
 * a hit while the car that hits it loses LESS and plows through). Verified:
 * dv = impulse*mass is written straight to linear_velocity (td5_physics.c ~6060),
 * and |dv_wreck| rises monotonically with the wreck's mass term. Percentage of
 * the stock mass; default 400 (= 4x lighter). TD5RE_WRECK_MASS_PCT=100 disables.
 * Clamped to [100,2000] so a wreck is never made HEAVIER than stock. */
static int wreck_mass_pct(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_WRECK_MASS_PCT");
        long p = (e && e[0]) ? strtol(e, NULL, 10) : 400;
        if (p < 100)  p = 100;
        if (p > 2000) p = 2000;
        v = (int)p;
        TD5_LOG_I(LOG_TAG, "wreck_mass_pct: TD5RE_WRECK_MASS_PCT=%d", v);
    }
    return v;
}

/* [traffic-lighter 2026-06-24] Make ORDINARY (non-wrecked) traffic easier to shove
 * out of the way when the player crashes into it. Same mechanism as wreck_mass_pct
 * (above), applied to live traffic instead of broken-down cars: cardef+0x88 behaves
 * as an INVERSE mass in the V2V impulse math [CONFIRMED @ 0x004079C0], so scaling a
 * traffic car's mass term UP gives it MORE velocity from a hit and bleeds LESS speed
 * off the car that hits it — the player plows through and the traffic flies aside.
 * Stock traffic mass is 32 (0x20) [CONFIRMED @ 0x0042F235]; the default 250 (= 2.5x
 * lighter) is noticeable but well short of the 4x wreck boost. Percentage of stock;
 * TD5RE_TRAFFIC_MASS_PCT=100 disables. Clamped [100,2000] so traffic is never made
 * HEAVIER than stock. Only applies to a traffic slot that is NOT broken down (a wreck
 * already gets wreck_mass_pct, which takes precedence so the two never compound). */
static int traffic_mass_pct(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_MASS_PCT");
        long p = (e && e[0]) ? strtol(e, NULL, 10) : 250;
        if (p < 100)  p = 100;
        if (p > 2000) p = 2000;
        v = (int)p;
        TD5_LOG_I(LOG_TAG, "traffic_mass_pct: TD5RE_TRAFFIC_MASS_PCT=%d", v);
    }
    return v;
}

/* [task #14 / #16] TD5RE_TRAFFIC_HITBOX_SCALE (default 0.70) — multiplier applied
 * to a TRAFFIC car's OBB half-extents (half-width / front-Z / rear-Z) in the V2V
 * corner test so the collision box matches the visible model instead of being
 * noticeably larger (the player "crashes" onto traffic without visually
 * touching). Lowered from 0.8 -> 0.70 (#16: traffic hitbox still felt too big).
 * Scales only when at least one of the pair is traffic, so racer-on-racer
 * contacts stay byte-faithful. "1"/"1.0" disables (no shrink). */
static float traffic_hitbox_scale(void)
{
    static int   s_init = 0;
    static float s_scale = 0.70f;
    if (!s_init) {
        const char *e = getenv("TD5RE_TRAFFIC_HITBOX_SCALE");
        if (e && e[0]) {
            s_scale = (float)atof(e);
            if (s_scale < 0.1f) s_scale = 0.1f;   /* sane floor */
            if (s_scale > 1.0f) s_scale = 1.0f;   /* never inflate */
        }
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "traffic_hitbox_scale: TD5RE_TRAFFIC_HITBOX_SCALE=%.3f", s_scale);
    }
    return s_scale;
}

/* [item #4] TD5RE_HITBOX_FIT (default ON) + TD5RE_HITBOX_SCALE (default 0.85) —
 * multiplier applied to a RACER's OBB half-extents (half-width / front-Z / rear-Z)
 * in the V2V corner test + impulse branch-split, the analog of
 * TD5RE_TRAFFIC_HITBOX_SCALE but for the player and AI opponents (slots
 * 0..g_traffic_slot_base-1). The cardef OBB is authored noticeably larger than the
 * visible chassis mesh (see the "playability slop" in v2v_depenetrate_pair and the
 * traffic-shrink note in obb_corner_test), so without this the player takes a
 * collision response — impulse / speed loss / the acute-crash shake — while there is
 * still a visible gap to the other car ("invisible crash"). Conservative default
 * (0.85): cars still collide on genuine contact, just not across the gap.
 *
 * TD5RE_HITBOX_FIT=0 returns 1.0 (no shrink) -> byte-faithful racer boxes.
 * TD5RE_HITBOX_SCALE overrides the magnitude (clamped to [0.5, 1.0] — never inflate,
 * sane floor so cars cannot pass through each other). This affects vehicle-vs-vehicle
 * only; the player-vs-wall path is probe-based (wheel corners cross the rail, no
 * half-width added) so it is not inflated and is intentionally left faithful. */
static float racer_hitbox_scale(void)
{
    static int   s_init = 0;
    static float s_scale = 0.85f;
    if (!s_init) {
        const char *fit = getenv("TD5RE_HITBOX_FIT");
        if (fit && fit[0] == '0') {
            s_scale = 1.0f;   /* knob OFF -> faithful (no shrink) */
        } else {
            const char *e = getenv("TD5RE_HITBOX_SCALE");
            if (e && e[0]) {
                s_scale = (float)atof(e);
                if (s_scale < 0.5f) s_scale = 0.5f;   /* sane floor — still collides */
                if (s_scale > 1.0f) s_scale = 1.0f;   /* never inflate */
            }
        }
        s_init = 1;
        TD5_LOG_I(LOG_TAG, "racer_hitbox_scale: TD5RE_HITBOX_FIT/SCALE -> %.3f", s_scale);
    }
    return s_scale;
}

/* [item #4] Effective OBB half-extent scale for one actor in a V2V pair: traffic
 * actors keep their existing TD5RE_TRAFFIC_HITBOX_SCALE shrink, racers (player + AI)
 * use the new racer_hitbox_scale. A single helper so obb_corner_test and
 * apply_collision_response derive identical scaled extents (the impulse branch-split
 * re-reads the cardef, so it must match the corner test or the side/front decision
 * would use a different box than the overlap that produced the contact). */
static float actor_hitbox_scale(const TD5_Actor *act)
{
    if (act && act->slot_index >= g_traffic_slot_base)
        return traffic_hitbox_scale();
    return racer_hitbox_scale();
}

/* Clamp a (presumed traffic) actor's world_pos.y so it never sits below the
 * track surface at its current span — the "traffic clips through the ground"
 * recovery for task #9. Uses the same barycentric ground query the traffic pose
 * integrator uses (td5_track_compute_contact_height_with_normal). No-op if the
 * car is already at/above ground or the query is unavailable. */
static void traffic_clamp_above_ground(TD5_Actor *t)
{
    int span, lane;
    int16_t snrm[3] = {0, 1, 0};
    int32_t ground_y;
    if (!t) return;
    span = (int)t->track_span_raw;
    lane = (int)t->track_sub_lane_index;
    ground_y = td5_track_compute_contact_height_with_normal(
        span, lane, t->world_pos.x, t->world_pos.z, snrm);
    /* world_pos is Y-up 24.8 FP; "below ground" = world_pos.y < ground_y. */
    if (t->world_pos.y < ground_y) {
        t->world_pos.y = ground_y;
        if (t->linear_velocity_y < 0) t->linear_velocity_y = 0;
    }
}

/* [FIX 2026-05-28 — traffic crash-spin animation] Port of the heavy-impact
 * TRAFFIC branch of ApplyVehicleCollisionImpulse (orig 0x00408289+). When a
 * traffic vehicle (slot >= 6) takes a heavy hit it enters scripted recovery
 * (vehicle_mode=1): a per-tick spin matrix is latched into collision_spin_matrix,
 * the current orientation is snapshotted to saved_orientation, the car is popped
 * up (linear_velocity_y = impact/6), and the recovery animation plays for ~0x3B
 * frames before ResetVehicleActorState rights it (handled by the existing
 * vehicle_mode==1 dispatch + td5_physics_integrate_scripted_motion).
 *
 * The spin-angle MAGNITUDE is byte-faithful to the original (GetDamageRulesStub
 * @0x0042C8D0 returns 0, so the original's spin is deterministic, not random) and
 * is NOT cosmetic: it is precisely what keeps the recovery's per-frame angular
 * velocity above the early-exit threshold so the tumble stays visible for the full
 * ~0x3B-frame window. The per-tick angle formula is documented inline below. */
static void td5_physics_apply_traffic_crash_spin(TD5_Actor *t, int32_t impact_mag)
{
    if (!t) return;

    /* [FIX 2026-06-02 traffic-tumble-visible] Per-tick spin angles derived from the
     * impact magnitude exactly like the original traffic branch (orig 0x408289+),
     * NOT a state LCG. orig: scatter = max(impact_mag/4, 0x7FFF) [floor, same as the
     * racer scatter]; iVar = scatter >> 8; each of the 3 spin-matrix angles =
     * GetDamageRulesStub() % iVar - iVar/2. GetDamageRulesStub @0x0042C8D0 returns 0,
     * so the byte-faithful deterministic value is -(iVar/2).
     *
     * Why this fixes the INVISIBLE tumble: RefreshScriptedVehicleTransforms re-derives
     * angular_velocity_{roll,yaw,pitch} from this spin matrix every tick, and the
     * recovery exit (td5_physics_check_and_update_actor_collision_alignment, orig
     * CheckAndUpdateActorCollisionAlignment @0x00409520) calls ResetVehicleActorState
     * at frame_counter==3 if the car is still upright (bin 0) AND |ang_vel_roll| < 0x20
     * AND |ang_vel_pitch| < 0x20. The OLD state-LCG produced angles in [-128,127]
     * INDEPENDENT of impact and frequently < 0x20, so a fresh hit reset out of scripted
     * mode in ~3 frames -> no visible tumble. The original's -(iVar/2) is always >= 63
     * (scatter >= 0x7FFF -> iVar >= 127), so |ang_vel| > 0x20 on every axis, the
     * early-exit can never fire, and the car tumbles the full ~0x3B frames (until the
     * unconditional frame_counter > 0x3B reset at the tail of integrate_scripted_motion).
     * Magnitude scales with impact, so harder hits spin faster. */
    int32_t scatter = impact_mag / 4;
    if (scatter < 0x7FFF) scatter = 0x7FFF;            /* floor (orig 0x4082A2-C9) */
    int16_t spin_ang = (int16_t)(-((scatter >> 8) / 2));  /* -(iVar/2), <= -63 */
    int16_t ang[3] = { spin_ang, spin_ang, spin_ang };

    float spin[9];
    BuildRotationMatrixFromAngles(spin, ang);

    /* Latch per-tick spin + snapshot current orientation. Mirror the orig's
     * 12-float (48-byte) copies to +0x180 / +0x150 (trailing 3 floats are
     * residue but the orig writes them, so we match). */
    memcpy(&t->collision_spin_matrix, spin, 9 * sizeof(float));
    memcpy(((uint8_t *)&t->collision_spin_matrix) + 9 * sizeof(float),
           spin, 3 * sizeof(float));
    memcpy(&t->saved_orientation, &t->rotation_matrix, 9 * sizeof(float));
    memcpy(((uint8_t *)&t->saved_orientation) + 9 * sizeof(float),
           &t->render_pos, 3 * sizeof(float));

    int32_t lift = impact_mag / 6;
    if (lift > 200000) lift = 200000;
    /* [task #9] Tame the vertical launch: keep just enough pop to read as a
     * crash but stop the car flying off the road. The spin animation stays
     * (driven by the angular spin matrix above, not by Y velocity), so the
     * tumble is unaffected — only the height is curbed. */
    if (traffic_hit_tame_enabled()) {
        const int32_t TRAFFIC_LIFT_CAP = 24000;   /* ~93 world-u/tick (was up to 200000) */
        if (lift > TRAFFIC_LIFT_CAP) lift = TRAFFIC_LIFT_CAP;
    }
    t->linear_velocity_y = lift;

    t->vehicle_mode = 1;     /* route to scripted-recovery dispatch next tick */
    t->frame_counter = 0;    /* start the ~0x3B-frame recovery animation */

    TD5_LOG_I(LOG_TAG, "traffic_crash_spin: slot=%d mag=%d lift=%d ang=[%d,%d,%d]",
              t->slot_index, impact_mag, lift, ang[0], ang[1], ang[2]);
}

/* True when `slot` is a HUMAN-driven racer (race_slot_state == 1). AI racers,
 * traffic, and out-of-range slots return 0. Used by the S08 rear-impact
 * softening so only the player's response is tuned, never AI/traffic.
 * g_race_slot_state is sized [TD5_MAX_RACER_SLOTS]; a human is always a racer
 * slot (< g_traffic_slot_base <= TD5_MAX_RACER_SLOTS), so the bound is safe. */
static inline int v2v_slot_is_human(int slot)
{
    return slot >= 0 && slot < g_traffic_slot_base &&
           slot < TD5_MAX_RACER_SLOTS && g_race_slot_state[slot] == 1;
}

/* Scale x to `pct`% (round-to-zero), used to soften a rear-ended player's
 * angular response. pct==100 is identity (byte-faithful pass-through). */
static inline int32_t v2v_scale_pct(int32_t x, int32_t pct)
{
    if (pct >= 100) return x;
    if (pct <= 0)   return 0;
    return (int32_t)(((int64_t)x * pct) / 100);
}

/* [MP GAME MODES: TIME TRIAL 2026-06-22] In time trial every player races the
 * clock and PASSES THROUGH the others (ghosts). True only for human-vs-human
 * pairs, so car-vs-traffic and car-vs-wall still resolve normally and each
 * viewport interacts with traffic on its own. Applied to BOTH V2V paths (the
 * impulse resolver and the anti-tunnel depenetration). Knob
 * TD5RE_TT_NO_COLLISION=0 restores solid player-vs-player contact. */
static int tt_pair_passthrough(int slot_a, int slot_b)
{
    static int knob = -1;
    if (knob < 0) {
        const char *e = getenv("TD5RE_TT_NO_COLLISION");
        knob = (e && e[0] == '0') ? 0 : 1;   /* default ON */
    }
    if (!knob) return 0;
    if (g_td5.mp_mode_config.mode != TD5_MP_MODE_TIME_TRIAL) return 0;
    /* Both are HUMAN players (slots 0..num_human_players-1) — the same definition
     * the ghost RENDER uses, so what you see (a ghost) and what you collide with
     * agree. Don't key off g_race_slot_state here (not reliably set for local
     * split-screen humans). AI opponents are NOT humans -> they still collide. */
    return slot_a >= 0 && slot_a < g_td5.num_human_players &&
           slot_b >= 0 && slot_b < g_td5.num_human_players;
}

static void apply_collision_response(TD5_Actor *penetrator, TD5_Actor *target,
                                     int corner_idx, OBB_CornerData *corner,
                                     int32_t heading_target, int32_t impactForce)
{
    if (!penetrator || !target) return;
    if (!penetrator->car_definition_ptr || !target->car_definition_ptr) return;
    (void)corner_idx;

    /* [#10 telemetry] A dispatched OBB corner contact = a real car-to-car
     * collision; flag both actors for this tick's edge-detector (idempotent —
     * multiple corners in one tick still count as a single collision). */
    td5_physics_mark_collision((int)penetrator->slot_index);
    td5_physics_mark_collision((int)target->slot_index);

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

    /* [#2 LIGHT WRECK 2026-06-21] Boost a broken-down car's inverse-mass term so
     * it shoves aside easily and barely slows the car that hits it. See
     * wreck_mass_pct(). slot_index>=0 guards the (rare) sentinel; the broken-down
     * flag is replicated, so this stays netplay-deterministic. */
    {
        int wp = wreck_mass_pct();
        int tp = traffic_mass_pct();
        /* Wreck boost takes precedence over the ordinary-traffic boost so a
         * broken-down traffic car keeps its (heavier) 4x shove and the two
         * multipliers never compound. A live traffic slot (>= traffic base, not
         * broken) gets the lighter-traffic boost instead. Racers (slot <
         * g_traffic_slot_base) are never scaled — byte-faithful. */
        if (A->slot_index >= 0) {
            if (wp != 100 && td5_ai_actor_is_broken_down((int)A->slot_index))
                mass_A = (mass_A * wp) / 100;
            else if (tp != 100 && (int)A->slot_index >= g_traffic_slot_base)
                mass_A = (mass_A * tp) / 100;
        }
        if (B->slot_index >= 0) {
            if (wp != 100 && td5_ai_actor_is_broken_down((int)B->slot_index))
                mass_B = (mass_B * wp) / 100;
            else if (tp != 100 && (int)B->slot_index >= g_traffic_slot_base)
                mass_B = (mass_B * tp) / 100;
        }
    }

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
    int32_t half_w_A   = (int32_t)CDEF_S(A, CDEF_HALF_WIDTH);
    int32_t front_z_A  = (int32_t)CDEF_S(A, CDEF_FRONT_Z_EXTENT);
    int32_t rear_z_A   = (int32_t)CDEF_S(A, CDEF_REAR_Z_EXTENT);

    /* [item #4] Use the SAME scaled box obb_corner_test used to generate the
     * corner data below. The side-vs-front/rear split compares |side_extent| =
     * half_w_A - |cx_A| against the front/rear depth; cx_A/cz_A were already
     * computed from the shrunk extents, so half_w_A/front_z_A/rear_z_A must match
     * or the branch decision (and its push direction) would reference a different
     * box than the overlap. No-op when the knob is off (scale == 1.0). */
    {
        float hbs_A = actor_hitbox_scale(A);
        if (hbs_A < 1.0f) {
            half_w_A  = (int32_t)((float)half_w_A  * hbs_A);
            front_z_A = (int32_t)((float)front_z_A * hbs_A);
            rear_z_A  = (int32_t)((float)rear_z_A  * hbs_A);
        }
    }

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

    /* [S08 rear-impact softening 2026-06-04] When a HUMAN player (always the
     * target A inside this function) is struck on its REAR face, the angular
     * response below — and the heavy-impact scatter/lift in step 10 — is scaled
     * down so getting rear-ended destabilises but stays recoverable instead of
     * spinning the car out. `cz_A < 1` is A's rear half (same predicate as the
     * side-vs-front/rear split above); a rear face contact is never the side
     * branch, so require the front/rear branch too. Only the victim's spin is
     * softened: the attacker B, front/side contacts on A, and every AI/traffic
     * response stay byte-faithful. rear_retain==100 disables the softening. */
    int rear_hit_on_player = (!is_side_branch) && (cz_A < 1) &&
                             v2v_slot_is_human(A->slot_index);
    int32_t rear_retain = rear_hit_on_player ? g_td5.ini.rear_impact_response : 100;
    if (rear_retain < 0)   rear_retain = 0;
    if (rear_retain > 100) rear_retain = 100;

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
            /* [S08] Tame the yaw spin imparted to a rear-ended player; the
             * linear impulse (local_50/local_44) is left faithful so the hit
             * still shoves the car forward like a real bump. */
            omega_A_delta = v2v_scale_pct(omega_A_delta, rear_retain);
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

    /* [#1 WRECK PUSH WINDOW 2026-06-21] A real ram on a broken-down (anchored)
     * wreck opens its free-slide window so it can be shoved aside — otherwise the
     * stand-still anchor in td5_physics_update_traffic re-freezes it next tick.
     * Gated on a meaningful impact so merely resting against a wreck does not keep
     * nudging it. */
    if (impact_mag > WRECK_PUSH_MIN_IMPACT) {
        if (A->slot_index >= 0 && A->slot_index < TD5_MAX_TOTAL_ACTORS &&
            td5_ai_actor_is_broken_down((int)A->slot_index))
            s_wreck_push_ticks[A->slot_index] = WRECK_PUSH_TICKS;
        if (B->slot_index >= 0 && B->slot_index < TD5_MAX_TOTAL_ACTORS &&
            td5_ai_actor_is_broken_down((int)B->slot_index))
            s_wreck_push_ticks[B->slot_index] = WRECK_PUSH_TICKS;
    }

    TD5_LOG_I(LOG_TAG, "v2v_impulse: side=%d slot_A=%d slot_B=%d mA=%d mB=%d "
              "cxA=%d czA=%d cxB=%d czB=%d imp=%d mag=%d toi=%d",
              is_side_branch, A->slot_index, B->slot_index, mass_A, mass_B,
              cx_A, cz_A, cx_B, cz_B, impulse, impact_mag, impactForce);

    /* [orig ApplyVehicleCollisionImpulse 0x004079c0, pre-LAB_00408289]: car/
     * traffic collision SFX. A racer-involved impact (at least one racer slot)
     * above 0x3201 plays a positional one-shot: LHit1-5 (light) below 0xc801,
     * HHit1-4 (hard) at/above it. Volume 0x1000; pitch per tier (0xd02 / 0x2198).
     * Reached only on an APPROACHING impact (the separating early-out above
     * returns before this), so it fires once per real hit, not while resting.
     * [N-way 2026-06-04] racer boundary g_traffic_slot_base, not hardcoded 6. */
    if ((A->slot_index < g_traffic_slot_base || B->slot_index < g_traffic_slot_base) &&
        impact_mag >= 0x3201) {
        int hit_slot, hit_pitch, hit_variants;
        if (impact_mag < 0xc801) {
            hit_slot = 0x1F; hit_pitch = 0xd02;  hit_variants = 5; /* LHit1-5 */
        } else {
            hit_slot = 0x1B; hit_pitch = 0x2198; hit_variants = 4; /* HHit1-4 */
        }
        int32_t hit_pos[3] = { A->world_pos.x, A->world_pos.y, A->world_pos.z };
        td5_sound_play_at_position(hit_slot, 0x1000, hit_pitch, hit_pos, hit_variants);
    }

    /* Wanted mode (cop chase): player<->cop collision awards damage score.
     * Mirrors ApplyVehicleCollisionImpulse @ 0x40817A/0x4081BE → AwardWantedDamageScore.
     * [CONFIRMED]: both A-is-player and B-is-player paths call AwardWantedDamageScore
     * on the cop slot with impact magnitude. */
    if (td5_game_is_wanted_mode()) {
        /* [MP COP CHASE] Generalized from the SP "slot 0 is the cop" gate: a ram
         * between ANY cop and any SUSPECT (non-cop racer) decrements that suspect's
         * damage bar. SP (cop=0) reproduces the old slot-0-vs-1..5 behaviour exactly.
         * [multi-cop 2026-06-24] Uses is_cop (mask-aware) so a SECOND human cop's rams
         * also count — previously only the single primary cop_slot dealt damage. */
        int sa = (int)A->slot_index, sb = (int)B->slot_index;
        if (td5_game_cop_chase_is_cop(sa) && td5_game_cop_chase_is_suspect(sb))
            td5_ai_wanted_cop_hit(sb, impact_mag);
        else if (td5_game_cop_chase_is_cop(sb) && td5_game_cop_chase_is_suspect(sa))
            td5_ai_wanted_cop_hit(sa, impact_mag);
    }

    /* Traffic recovery escalation (> 50000 and traffic slot). [N-way 2026-06-04]
     * traffic boundary g_traffic_slot_base, not hardcoded 6 — a human racer in
     * an expanded slot must not pick up the traffic-only lockout escalation. */
    if (A->slot_index >= g_traffic_slot_base && impact_mag > 50000 &&
        A->damage_lockout > 0 && A->damage_lockout < 7) {
        A->damage_lockout++;
    }
    if (B->slot_index >= g_traffic_slot_base && impact_mag > 50000 &&
        B->damage_lockout > 0 && B->damage_lockout < 7) {
        B->damage_lockout++;
    }

    /* Heavy impact (> 90000) with collisions enabled: per-actor angular scatter
     * + vertical lift, byte-faithful to the original high-impact branch.
     *
     * [CONFIRMED @ 0x408289-0x40855D, ApplyVehicleCollisionImpulse]: the branch
     * runs an INDEPENDENT pass over each actor of the pair -- actor A (base EBX,
     * 0x4082CB-0x408362) then actor B (base EBP, 0x40842B-0x408497). For a racer
     * slot (<6) each pass writes, via a fresh GetDamageRulesStub() call:
     *     angular_velocity_roll  (+0x1C0) += stub %  scatter    - scatter/2
     *     angular_velocity_yaw   (+0x1C4) += stub %  scatter    - scatter/2
     *     angular_velocity_pitch (+0x1C8) += stub % (2*scatter) - scatter
     *     linear_velocity_y      (+0x1D0)  = impact_mag / 6
     * scatter = (impact_mag >> 2) FLOORED to a MINIMUM of 0x7FFF
     * [CONFIRMED @ 0x4082A2-0x4082C9]. GetDamageRulesStub @0x0042C8D0 literally
     * `return 0` [CONFIRMED], so every `stub % N` term is 0 and the writes fold
     * to a DETERMINISTIC result with the SAME sign on BOTH actors:
     *     roll -= scatter/2;  yaw -= scatter/2;  pitch -= scatter;
     *     linear_velocity_y = impact_mag / 6;
     *
     * [FIX 2026-06-02 v2v-heavy-scatter-faithfulness] The prior port diverged on
     * three counts, all corrected here: (a) it clamped scatter with a CEILING
     * (`if > 0x7FFF`) -- inverted vs the original's FLOOR; (b) it used ad-hoc
     * kick magnitudes (scatter/8, -scatter/2, scatter/4 on roll/pitch/yaw) that
     * matched neither the original's axis mapping nor its magnitude; and (c) it
     * applied OPPOSITE signs to A (+kick) and B (-kick), whereas the original
     * writes the SAME sign to both. The vertical lift keeps the port-only 200000
     * clamp (the original has none) -- that clamp is a #bounce concern deferred
     * to a manual A/B; it only touches linear_velocity_y, not the spin.
     *
     * Gate semantics [VERIFIED 2026-05-17]: original at 0x00408289 reads
     * `if (90000 < iVar10 && g_cameraMode == 0)`. The port's `g_collisions_enabled`
     * lives at the SAME address (DAT_00463188) as the original's `g_cameraMode`;
     * `td5_physics_set_collisions(1)` writes 0, so `== 0` means collisions ON --
     * byte-faithful to the original's gate. */
    /* [#5 2026-06-20] Heavy-crash gate. Racer-vs-racer keeps the faithful 90000
     * floor; a pair that includes a TRAFFIC or COP car trips the dramatic 3D
     * collision (scatter+lift) AND the wreck at the lower npc_fatal_mag() so
     * those hits feel fatal and total the NPC much more readily. */
    int a_is_npc = (A->slot_index >= g_traffic_slot_base);
    int b_is_npc = (B->slot_index >= g_traffic_slot_base);
    int32_t heavy_gate = (a_is_npc || b_is_npc) ? npc_fatal_mag() : 90000;
    if (impact_mag > heavy_gate && g_collisions_enabled == 0) {
        int32_t scatter = impact_mag / 4;
        if (scatter < 0x7FFF) scatter = 0x7FFF;   /* FLOOR (orig 0x4082A2-C9) */
        int32_t kick_ry = scatter / 2;            /* roll & yaw delta magnitude */
        int32_t kick_p  = scatter;                /* pitch delta magnitude      */
        /* [N-way coverage 2026-06-04] The racer/traffic split was hardcoded
         * `< 6`, the ORIGINAL racer count. With the N-way expansion a human
         * racer can sit in slots 6..15, so `< 6` wrongly routed it into the
         * traffic scripted crash-spin (vehicle_mode=1 takeover). Use the live
         * racer/traffic boundary g_traffic_slot_base — byte-identical to `< 6`
         * in a legacy 6-racer race, correct for the expanded field. */
        if (A->slot_index < g_traffic_slot_base) {
            /* [S08] Soften the rear-ended player's launch/spin (rear_retain is
             * 100 for everyone else, so this is a no-op for AI/front/side). */
            int32_t a_kick_ry = v2v_scale_pct(kick_ry, rear_retain);
            int32_t a_kick_p  = v2v_scale_pct(kick_p,  rear_retain);
            int32_t lift_a = impact_mag / 6;
            if (lift_a > 200000) lift_a = 200000; /* port-only clamp (orig: none) */
            lift_a = v2v_scale_pct(lift_a, rear_retain);
            A->angular_velocity_roll  -= a_kick_ry;
            A->angular_velocity_yaw   -= a_kick_ry;
            A->angular_velocity_pitch -= a_kick_p;
            A->linear_velocity_y  = lift_a;
        } else {
            /* Traffic: scripted crash-spin recovery, orig 0x00408289+. */
            td5_physics_apply_traffic_crash_spin(A, impact_mag);
        }
        if (B->slot_index < g_traffic_slot_base) {
            /* B is the penetrator (never the rear-contacted victim here), so it
             * always takes the byte-faithful kick — the attacker is unchanged. */
            B->angular_velocity_roll  -= kick_ry;
            B->angular_velocity_yaw   -= kick_ry;
            B->angular_velocity_pitch -= kick_p;
            int32_t lift_b = impact_mag / 6;
            if (lift_b > 200000) lift_b = 200000; /* port-only clamp (orig: none) */
            B->linear_velocity_y  = lift_b;
        } else {
            /* Traffic: scripted crash-spin recovery, orig 0x00408289+. */
            td5_physics_apply_traffic_crash_spin(B, impact_mag);
        }
        /* [CRASH FX 2026-06-15, item #12] On a genuinely high-speed hit, give a
         * PLAYER actor an extra forward-speed scrub and record a crash-fx event
         * other modules can poll (HUD shake / VFX / audio). Players only (slot <
         * g_traffic_slot_base); both A and B are scrubbed if both are players.
         * Gated by TD5RE_CRASH_FX (default ON) inside the helper; off => no-op.
         * The faithful angular scatter + vertical lift above are unchanged. */
        if (impact_mag > CRASH_FX_ACUTE_MAG) {
            if (A->slot_index < g_traffic_slot_base)
                td5_physics_apply_acute_crash_fx(A, impact_mag);
            if (B->slot_index < g_traffic_slot_base)
                td5_physics_apply_acute_crash_fx(B, impact_mag);
        }
        /* [#5 2026-06-20, was POLICE rewrite 2026-06-19] This heavy crash WRECKS
         * every involved TRAFFIC/COP car: it breaks down -> parks, smokes from
         * the roof, and can never drive itself again (permanent). A chased racer
         * is also flagged so its "broke down" ENDS the pursuit, but racers/the
         * player are never immobilised or smoked (handled downstream). Because we
         * are already inside the (NPC-lowered) heavy gate, no separate magnitude
         * test is needed — a fatal hit on traffic/cops always totals them. */
        /* [MP TRAFFIC FAIRNESS 2026-06-22] In fair split-screen MP, don't
         * PERMANENTLY total plain background traffic (keep the shared stream
         * deterministic so every viewport sees the same cars — it free-slides
         * then recovers). Cops and pursued racers still wreck normally. */
        {
            int fair = td5_game_mp_traffic_fair();
            int sa = (int)A->slot_index, sb = (int)B->slot_index;
            int plain_a = (sa >= g_traffic_slot_base) &&
                          !td5_ai_actor_is_pursued(sa) && !td5_ai_cop_is_chasing(sa);
            int plain_b = (sb >= g_traffic_slot_base) &&
                          !td5_ai_actor_is_pursued(sb) && !td5_ai_cop_is_chasing(sb);
            if ((a_is_npc || td5_ai_actor_is_pursued(sa)) && !(fair && plain_a))
                td5_ai_mark_actor_broken_down(sa);
            if ((b_is_npc || td5_ai_actor_is_pursued(sb)) && !(fair && plain_b))
                td5_ai_mark_actor_broken_down(sb);
        }
        TD5_LOG_I(LOG_TAG, "v2v_heavy_scatter: A=%d B=%d mag=%d scatter=%d kick_ry=%d kick_p=%d rear_retain=%d",
                  A->slot_index, B->slot_index, impact_mag, scatter, kick_ry, kick_p, rear_retain);
    }

    /* [task #9] After the full impulse + (optional) heavy scatter, keep any
     * traffic actor in the pair at/above the track surface so it can never sink
     * through the ground following a hard hit. Racers are left untouched (their
     * own ground-snap runs in the pose integrator). No-op when tame is off. */
    if (traffic_hit_tame_enabled()) {
        if (A->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(A);
        if (B->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(B);
    }

    /* pool14_v2v pilot trace: capture post-state at function exit.
     * Original returns int impact_mag at 0x004084A2 RET. */
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

    int32_t radius_a = (int32_t)CDEF_S(a, CDEF_COLLISION_RADIUS);
    int32_t radius_b = (int32_t)CDEF_S(b, CDEF_COLLISION_RADIUS);
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

    /* [task #9] Constrain the response to the horizontal plane for traffic.
     * This sphere path runs for player-vs-traffic (the explicit S20 collide)
     * and scripted pairs; its separation vector includes a vertical (Y)
     * component, so a player ramming a traffic car at speed can punt it
     * upward. When tame is on and EITHER actor is traffic, drop the Y impulse
     * for the pair so the bump stays in-plane (the player still gets shoved
     * sideways/forward exactly as before). */
    if (traffic_hit_tame_enabled() &&
        (a->slot_index >= g_traffic_slot_base || b->slot_index >= g_traffic_slot_base)) {
        delta_y = 0;
    }

    a->linear_velocity_x += delta_x;
    a->linear_velocity_y += delta_y;
    a->linear_velocity_z += delta_z;
    b->linear_velocity_x -= delta_x;
    b->linear_velocity_y -= delta_y;
    b->linear_velocity_z -= delta_z;

    /* [#10 telemetry] A closing sphere overlap that produced a separation
     * impulse is a real car-to-car contact — flag both actors for this tick. */
    td5_physics_mark_collision((int)a->slot_index);
    td5_physics_mark_collision((int)b->slot_index);

    /* [task #9] Belt-and-suspenders: keep any traffic actor in the pair from
     * ending up below the track surface this tick. */
    if (traffic_hit_tame_enabled()) {
        if (a->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(a);
        if (b->slot_index >= g_traffic_slot_base) traffic_clamp_above_ground(b);
    }
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
 * Anti-tunnel car-vs-car depenetration (S17 2026-06-05) -- PORT-ONLY
 *
 * The original v2v resolution (ApplyVehicleCollisionImpulse @ 0x004079C0)
 * applies a single FIXED half-cos/sin positional push -- NOT proportional to
 * how deeply the cars overlap -- and runs once per frame with no relaxation
 * loop. A high-speed / deep collision therefore stays visibly interpenetrated
 * for one or more frames before the impulse plus repeated small fixed pushes
 * finally separate the cars. That is the user-visible "at the moment of
 * crashing I can briefly go THROUGH cars".
 *
 * This pass restores hard non-overlap by pushing any genuinely OBB-overlapping
 * pair fully apart along their line of centres. It is purely POSITIONAL: it
 * writes world_pos only and never touches linear or angular velocity, so the
 * S08 rear/side/front impact response tuning (RearImpactResponse etc.) is
 * provably unaffected -- the cars bounce and spin exactly as before, they just
 * no longer clip through one another first.
 *
 * Overlap is detected with the SAME obb_corner_test the collision system uses,
 * so two cars racing side-by-side without actually touching are never pushed
 * apart (no ghost repulsion) and we separate exactly the pairs the game already
 * treats as in contact. The push direction is the world line of centres, which
 * for convex bodies monotonically reduces penetration (so it cannot oscillate)
 * and whose sign is trivially "away from each other".
 *
 * Returns the pre-push penetration depth in display units (> 0 means it moved
 * the pair apart), or 0 if the cars were not actually overlapping.
 * ======================================================================== */
static int32_t v2v_depenetrate_pair(TD5_Actor *a, TD5_Actor *b)
{
    OBB_CornerData corners[8];
    memset(corners, 0, sizeof(corners));

    /* Detect overlap at the cars' CURRENT positions (the faithful impulse and
     * earlier relaxation rounds may already have moved them). Raw 24.8 fp in;
     * the callee shifts internally. The pilot-trace emits inside obb_corner_test
     * self-gate to a no-op unless V2V contact tracing is explicitly enabled, so
     * these extra detection calls are free in normal and release builds. */
    int bm = obb_corner_test(a, b,
                             a->world_pos.x, a->world_pos.z,
                             b->world_pos.x, b->world_pos.z,
                             a->euler_accum.yaw, b->euler_accum.yaw,
                             corners);
    if (bm == 0) return 0;            /* not actually overlapping */

    /* Deepest minimal-axis penetration across all set corners (display units).
     * pen_x/pen_z are the signed depth to the nearest face on each local axis;
     * min(|pen_x|,|pen_z|) is that corner's minimum-translation distance, and
     * the max over corners is how far the pair must move to fully clear. */
    int32_t depth = 0;
    for (int c = 0; c < 8; c++) {
        if (!(bm & (1 << c))) continue;
        int32_t px = corners[c].pen_x; if (px < 0) px = -px;
        int32_t pz = corners[c].pen_z; if (pz < 0) pz = -pz;
        int32_t m  = (px < pz) ? px : pz;
        if (m > depth) depth = m;
    }
    if (depth <= 0) return 0;         /* exactly touching, nothing to clear */

    /* Playability slop: the collision OBB is slightly larger than the visible
     * car mesh, so depenetrating to ZERO OBB overlap parks the cars with a
     * visible gap (~the combined box-vs-mesh margin). Allow anti_tunnel_slop
     * units of OBB overlap to remain so the cars settle at ~mesh contact, and
     * push only the excess. The faithful impulse/bounce path is untouched --
     * only the resting separation changes. */
    int32_t push_depth = depth - g_td5.ini.anti_tunnel_slop;
    if (push_depth <= 0) return 0;    /* within the allowed slop -> leave it */
    if (push_depth > ANTITUNNEL_MAX_DEPTH) push_depth = ANTITUNNEL_MAX_DEPTH;

    /* World line of centres (display units), pointing A away from B. */
    int32_t dx = (a->world_pos.x - b->world_pos.x) >> 8;
    int32_t dz = (a->world_pos.z - b->world_pos.z) >> 8;
    int32_t nx, nz;                   /* 12-bit unit direction */
    int32_t len = td5_isqrt(dx * dx + dz * dz);
    if (len < 1) {
        /* Centres coincide -- eject along A's lateral axis as a fallback so we
         * still make progress instead of dividing by zero. */
        int32_t ha = a->euler_accum.yaw >> 8;
        nx =  cos_fixed12(ha);
        nz = -sin_fixed12(ha);
    } else {
        nx = (dx << 12) / len;
        nz = (dz << 12) / len;
    }

    /* Split the separation 50/50: each car moves half of (push_depth + 2)
     * display units along the unit normal, converted to 24.8 world position
     * units ((unit/4096) * half * 256 == (unit * half) >> 4). The +2 guard
     * clears the slop boundary by a couple of units so the next test settles. */
    int32_t half = (push_depth + 2) >> 1;
    int32_t mv_x = (nx * half) >> 4;
    int32_t mv_z = (nz * half) >> 4;

    a->world_pos.x += mv_x;  a->world_pos.z += mv_z;
    b->world_pos.x -= mv_x;  b->world_pos.z -= mv_z;
    return depth;
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

    /* [CORRECTED 2026-05-26 v2v-include-traffic; orig 0x00409150]
     * Earlier port comment claimed DAT_004aaf00 was g_racerCount and
     * excluded traffic. Ghidra re-audit confirms DAT_004aaf00 is the
     * FULL active-actor count written by 0x00432e60 as ((traffic_on)?12:6)
     * — so orig DOES include traffic slots 6..11 in V2V broadphase.
     * The dispatch at line ~4350 already gates per-slot on vehicle_mode
     * and wheel_contact_bitmask, so re-including traffic here surfaces
     * the user-reported missing traffic-vs-player collisions without
     * affecting racer-on-racer behavior. */
    total = (int)td5_game_get_total_actor_count();
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;
    if (total < 2) {
        return;
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

        if (!actor->car_definition_ptr ||
            /* [dynamic-traffic] parked cars are intangible — keep their ghost
             * pose out of the broadphase so nobody collides with a hidden car. */
            td5_ai_traffic_dynamic_parked(i)) {
            memset(g_actor_aabb[i], 0, sizeof(g_actor_aabb[i]));
            continue;
        }

        /* Compute AABB from position +/- bounding radius */
        radius = (int32_t)CDEF_S(actor, CDEF_COLLISION_RADIUS);
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
        if (td5_ai_traffic_dynamic_parked(i)) continue;  /* [dynamic-traffic] intangible */

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
                                  "pos_i=(%d,%d) pos_j=(%d,%d) span_i=%d span_j=%d "
                                  "dispatch=%s a[vm=%d wm=%d scr=%d] b[vm=%d wm=%d scr=%d]",
                                  i, j, a->slot_index, b->slot_index,
                                  a->world_pos.x >> 8, a->world_pos.z >> 8,
                                  b->world_pos.x >> 8, b->world_pos.z >> 8,
                                  a->track_span_normalized, b->track_span_normalized,
                                  (a_scripted || b_scripted) ? "SIMPLE" : "FULL",
                                  a->vehicle_mode, a->wheel_contact_bitmask, a_scripted,
                                  b->vehicle_mode, b->wheel_contact_bitmask, b_scripted);
                    }
                }

                /* [TIME TRIAL] human-vs-human pairs pass THROUGH each other
                 * (ghosts) — skip the V2V impulse entirely. This is the real V2V
                 * dispatch (the broadphase), not resolve_collision_pair.
                 * [PER-VIEWPORT TRAFFIC] also skip pairs that belong to different
                 * viewports' traffic partitions (a player only touches its own
                 * traffic; cross-partition traffic twins never collide). */
                if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index) ||
                    td5_ai_traffic_pair_blocked((int)a->slot_index, (int)b->slot_index)) {
                    /* no contact */
                } else if (a_scripted || b_scripted) {
                    collision_detect_simple(a, b);
                } else {
                    collision_detect_full(a, b, i, j);
                }

                chain = g_actor_aabb[j][4] & 0xFF;
            }
        }
    }


    /* --- Phase 2.5: anti-tunnel position-only depenetration (S17) ---
     * Port-only robustness pass (gated by [GameOptions] AntiTunnel, default
     * on). The faithful resolution above can leave a fast/deep pair visibly
     * interpenetrated for a frame; this clears any residual OBB overlap by
     * pushing the cars apart, moving ONLY world position so the S08 impact
     * tuning is untouched. It walks every pair O(n^2) (n <= 22) over the LIVE
     * post-resolution positions, so unlike the span-bucket broadphase above it
     * has no bucket-aliasing blind spot for a fast closing pair. */
    if (g_td5.ini.anti_tunnel) {
        static uint32_t s_sep_frames = 0;   /* throttle for the summary log */
        int     moved_any_actor   = 0;
        int     separated_cleanly = 0;
        int32_t max_pre_depth     = 0;       /* deepest overlap seen this frame */
        uint8_t moved[TD5_MAX_TOTAL_ACTORS];
        memset(moved, 0, sizeof(moved));

        for (int round = 0; round < ANTITUNNEL_RELAX_ROUNDS; round++) {
            int moved_this_round = 0;

            for (int i = 0; i < total; i++) {
                TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
                if (!a->car_definition_ptr) continue;

                /* Script-controlled vehicles (crash takeovers, AI script VM)
                 * own their own position -- leave them alone, the same set the
                 * faithful dispatch routes to the sphere (non-OBB) handler. */
                int a_scripted = (a->vehicle_mode != 0) ||
                                 (a->wheel_contact_bitmask >= 0x0F);
                if (a_scripted) continue;

                int32_t rad_a = (int32_t)CDEF_S(a, CDEF_COLLISION_RADIUS);
                int32_t ax = a->world_pos.x >> 8;
                int32_t az = a->world_pos.z >> 8;

                for (int j = i + 1; j < total; j++) {
                    TD5_Actor *b = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);
                    if (!b->car_definition_ptr) continue;

                    int b_scripted = (b->vehicle_mode != 0) ||
                                     (b->wheel_contact_bitmask >= 0x0F);
                    if (b_scripted) continue;

                    /* Cheap live AABB reject before the OBB test. */
                    int32_t rad_b = (int32_t)CDEF_S(b, CDEF_COLLISION_RADIUS);
                    int32_t bx = b->world_pos.x >> 8;
                    int32_t bz = b->world_pos.z >> 8;
                    int32_t r  = rad_a + rad_b;
                    int32_t ddx = ax - bx; if (ddx < 0) ddx = -ddx;
                    int32_t ddz = az - bz; if (ddz < 0) ddz = -ddz;
                    if (ddx > r || ddz > r) continue;

                    /* [TIME TRIAL] don't separate two players — they ghost.
                     * [PER-VIEWPORT TRAFFIC] don't separate cross-partition traffic
                     * twins or a non-owner player from another viewport's traffic. */
                    if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index) ||
                        td5_ai_traffic_pair_blocked((int)a->slot_index, (int)b->slot_index)) continue;

                    int32_t pen = v2v_depenetrate_pair(a, b);
                    if (pen > 0) {
                        moved[i] = moved[j] = 1;
                        moved_this_round = 1;
                        moved_any_actor  = 1;
                        if (round == 0 && pen > max_pre_depth) max_pre_depth = pen;
                        /* a moved -- refresh its cached centre for the rest of
                         * this row so later j see the updated position. */
                        ax = a->world_pos.x >> 8;
                        az = a->world_pos.z >> 8;
                    }
                }
            }

            if (!moved_this_round) { separated_cleanly = 1; break; }
        }

        /* Resync render pose (render_pos + matrices) for any car we moved, the
         * same way apply_collision_response does after its push. Only XZ
         * position changed (angles untouched), but the shared helper is the
         * established idiom and keeps wheel/camera/HUD consumers consistent. */
        if (moved_any_actor) {
            for (int i = 0; i < total; i++) {
                if (!moved[i]) continue;
                TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)i * TD5_ACTOR_STRIDE);
                if (a->car_definition_ptr) update_vehicle_pose_from_physics(a);
            }
            s_sep_frames++;
            if (!separated_cleanly) {
                TD5_LOG_W(LOG_TAG, "v2v_antitunnel: pair(s) still overlapping after %d relax rounds (max_pre_depth=%d units, deep pile-up); continuing next frame",
                          ANTITUNNEL_RELAX_ROUNDS, max_pre_depth);
            } else if ((s_sep_frames % 30u) == 1u) {
                TD5_LOG_I(LOG_TAG, "v2v_antitunnel: cleared car overlap (max_pre_depth=%d units) in <=%d rounds; frames_with_sep=%u",
                          max_pre_depth, ANTITUNNEL_RELAX_ROUNDS, s_sep_frames);
            }
        }
    }

    /* [S20 PlayerCollide] Explicit player-vs-traffic collision by world proximity.
     * The span-bucket broadphase above only pairs actors within +/-1 bucket
     * (~4 track-spans). On curves/junctions a traffic car can be physically on
     * top of the player yet several spans apart in track-progress, so the pair
     * is never tested and the player drives THROUGH it (user-reported "traffic
     * doesn't collide"). Supplement here: test slot 0 against every traffic
     * actor whose bucket the broadphase did NOT cover, gated on a real world
     * overlap. resolve_collision_pair runs the same OBB test + impulse as the
     * faithful path, so the response is identical — just no longer skipped. */
    if (g_td5.ini.traffic_player_collide && total > g_traffic_slot_base) {
        int tmax = (total < TD5_MAX_TOTAL_ACTORS) ? total : TD5_MAX_TOTAL_ACTORS;
        /* [PER-VIEWPORT TRAFFIC] When active, run this supplement for EVERY human
         * viewport's player against ITS OWN traffic partition (so each player can
         * hit only its own traffic). Otherwise it stays the original slot-0 path. */
        int per_vp = td5_ai_traffic_per_viewport_active();
        int vc = per_vp ? g_td5.viewport_count : 1;
        int vp;
        if (vc > TD5_MAX_VIEWPORTS) vc = TD5_MAX_VIEWPORTS;
        for (vp = 0; vp < vc; vp++) {
            int pslot = per_vp ? td5_game_get_player_slot(vp) : 0;
            TD5_Actor *player;
            int32_t prad;
            if (pslot < 0 || pslot >= g_traffic_slot_base) continue;
            player = (TD5_Actor *)(g_actor_table_base + (size_t)pslot * TD5_ACTOR_STRIDE);
            if (!player->car_definition_ptr) continue;
            prad = (int32_t)CDEF_S(player, CDEF_COLLISION_RADIUS);
            for (int t = g_traffic_slot_base; t < tmax; ++t) {
                TD5_Actor *tr =
                    (TD5_Actor *)(g_actor_table_base + (size_t)t * TD5_ACTOR_STRIDE);
                int32_t dxp, dzp, rr;
                int64_t d2;
                if (per_vp && td5_ai_traffic_slot_owner_vp(t) != vp) continue; /* own partition only */
                if (!tr->car_definition_ptr) continue;
                /* [2026-06-12] Don't collide with INVISIBLE traffic. Dynamic
                 * traffic despawns by fading draw alpha to 0 (the renderer then
                 * skips the car), but the actor stays parked at its last span
                 * with a live car_definition_ptr -> the player rams an unrendered
                 * car = the "invisible wall on span 110" report (LOW volume cap=2
                 * left 4 despawned cars parked at span ~110). Gate collision on the
                 * SAME visibility the renderer uses, so you only ever hit traffic
                 * you can see. get_draw_alpha() returns 255 when dynamic traffic is
                 * off, so classic-mode collision is unchanged. [2026-06-12b]
                 * Also skip PARKED (inactive) slots outright — belt-and-suspenders
                 * in case a slot is mid-state with a transient non-zero alpha. */
                if (td5_ai_traffic_dynamic_parked(t) ||
                    td5_ai_traffic_get_draw_alpha(t) == 0) continue;
                dxp = (player->world_pos.x >> 8) - (tr->world_pos.x >> 8);
                dzp = (player->world_pos.z >> 8) - (tr->world_pos.z >> 8);
                rr  = prad + (int32_t)CDEF_S(tr, CDEF_COLLISION_RADIUS);
                d2  = (int64_t)dxp * dxp + (int64_t)dzp * dzp;
                if (d2 > (int64_t)rr * rr) continue;   /* not physically overlapping */
                /* Physically overlapping the player. The bucket broadphase
                 * misses player<->traffic pairs (verified 0 hits), so force the
                 * sphere collision response here regardless of track-span bucket
                 * or OBB orientation. Sphere only impulses when CLOSING, so this
                 * is a no-op once separated (won't fight the broadphase). */
                TD5_LOG_I(LOG_TAG,
                          "player_traffic_collide: player_slot=%d traffic_slot=%d dist=%d rr=%d span_p=%d span_t=%d "
                          "(world-overlap -> sphere collision)",
                          pslot, t, (int)td5_isqrt((int32_t)(d2 > 0x7FFFFFFF ? 0x7FFFFFFF : d2)), rr,
                          player->track_span_normalized, tr->track_span_normalized);
                collision_detect_simple(player, tr);
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

    /* [TIME TRIAL] human-vs-human pairs pass through each other (ghost). */
    if (tt_pair_passthrough((int)a->slot_index, (int)b->slot_index)) return;

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


    /* cardef constants -- see function header.
     * Names chosen to match the original's semantic use. */
    const int32_t k_pos_damp   = (int32_t)PHYS_S(actor, PHYS_SUSP_POS_DAMP);  /* position-proportional damping (restoring) */
    const int32_t k_vel_damp   = (int32_t)PHYS_S(actor, PHYS_SUSP_VEL_DAMP);  /* velocity-proportional damping */
    const int32_t k_spring     = (int32_t)PHYS_S(actor, PHYS_SUSP_SPRING);  /* spring coefficient (multiplies lever proj) */
    const int32_t k_travel_lim = (int32_t)PHYS_S(actor, PHYS_SUSP_TRAVEL_LIM);  /* per-wheel +/- travel clamp */
    const int32_t k_load_scale = (int32_t)PHYS_S(actor, PHYS_SUSP_LOAD_SCALE);  /* multiplier on wheel_load_accum */

    /* world_pos >> 8 with the original's round-to-zero bias (0x00403A7D-A85
     * and the symmetric one at 0x00403A9C-AA0). Plain SAR rounds toward -∞
     * for negatives. */
    const int32_t wpx_scaled = sar8_rz(actor->world_pos.x);
    const int32_t wpz_scaled = sar8_rz(actor->world_pos.z);

    /* [SUSPCONST 2026-05-27] Force-cascade root-cause: capture the spring
     * constants, lever arm, and accel so the suspension equilibrium can be
     * compared to the orig (0x00403A20). The port suspension saturates while
     * the orig stays ~500; fx is only 2x, so the extra factor is in the
     * spring response (constants or arm). Slot 0, every 60 ticks. */
    if (actor->slot_index == 0 && (actor->frame_counter % 60u) == 0u) {
        int32_t arm0x = actor->wheel_world_positions_hires[0].x - wpx_scaled;
        int32_t arm0z = actor->wheel_world_positions_hires[0].z - wpz_scaled;
        TD5_LOG_I(LOG_TAG,
            "SUSPCONST k_spring=%d k_pos_damp=%d k_vel_damp=%d k_travel=%d "
            "k_load=%d | arm0=(%d,%d) accel=(%d,%d) hires0=(%d,%d) wp8=(%d,%d)",
            k_spring, k_pos_damp, k_vel_damp, k_travel_lim, k_load_scale,
            arm0x, arm0z, accel_x, accel_z,
            actor->wheel_world_positions_hires[0].x,
            actor->wheel_world_positions_hires[0].z,
            wpx_scaled, wpz_scaled);
        TD5_LOG_I(LOG_TAG,
            "TUNING LUT[8..11]=%d,%d,%d,%d torque_mult=%d gear_ratio[2]=%d "
            "redline=%d | throttle(+0x33E)=%d rpm=%d gear=%d",
            (int)PHYS_S(actor, 16), (int)PHYS_S(actor, 18),
            (int)PHYS_S(actor, 20), (int)PHYS_S(actor, 22),
            (int)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT), (int)PHYS_S(actor, 0x32),
            (int)PHYS_S(actor, PHYS_REDLINE_RPM),
            (int)actor->encounter_steering_cmd,
            actor->engine_speed_accum, (int)actor->current_gear);
    }

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

        /* [SPRINGTRACE 2026-05-27] Per-tick spring dynamics for slot 0, wheels
         * 0(FL)/2(RL), every 30 ticks. Shows WHY pos runs to ±12288 when the
         * measured-accel equilibrium is only ~1044: logs proj, spring/load/damp
         * terms, old pos+dv, and new_vel. pos_in/dv_in are still pre-write. */
        if (actor->slot_index == 0 && i == 0 && actor->frame_counter < 80u) {
            TD5_LOG_I(LOG_TAG,
                "SPRINGTRACE w%d: proj=%d spT=%d(s%d) loadAcc=%d loadT=%d(s%d) "
                "dvIn=%d posIn=%d posdampT=%d(s%d) veldampT=%d(s%d) new_vel=%d",
                i, proj, spring_term_x256, sar8_rz(spring_term_x256),
                actor->wheel_load_accum[i], load_term_x256, sar8_rz(load_term_x256),
                actor->wheel_spring_dv[i], actor->wheel_suspension_pos[i],
                pos_damp_x256, sar8_rz(pos_damp_x256),
                vel_damp_x256, sar8_rz(vel_damp_x256), new_vel);
            TD5_LOG_I(LOG_TAG,
                "SPRINGARM w%d: arm=(%d,%d) accel=(%d,%d) hires=(%d,%d) wp8=(%d,%d)",
                i, arm_x, arm_z, accel_x, accel_z,
                actor->wheel_world_positions_hires[i].x,
                actor->wheel_world_positions_hires[i].z,
                wpx_scaled, wpz_scaled);
        }

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


/* [CONFIRMED @ 0x004057F0] L5 promotion sweep audit (2026-05-18).
 *
 * UpdateVehicleSuspensionResponse — 783 bytes / 241 instructions / 120
 * decompiled lines. Body audited line-for-line vs disassembly:
 *
 *   - bVar1=wheel_contact_bitmask / bVar2=damage_lockout polarity:
 *     CONFIRMED at 0x004058E4 (spring-dot gate) + 0x00405A6A (pattern
 *     clamp) — port matches AIRBORNE-mask polarity end-to-end.
 *   - loop 4-wheel structure (psVar11 stride +0x254 / 4 shorts/wheel):
 *     CONFIRMED at 0x00405884/0x00405888.
 *   - g_view rotation_world_to_body_y reads row 1 of body^T:
 *     CONFIRMED at 0x004057FA-0x004058D8 (TransposeMatrix3x3 +
 *     LoadRenderRotationMatrix + ConvertFloatVec3ToShortAngles).
 *   - signed div-by-2 with C-style truncation (dot/2 vs dot>>1):
 *     SHIPPED fix in commit on precise-004057F0 branch.
 *   - axis assignment to ang_vel_roll/pitch:
 *     CONFIRMED at 0x00405A3D / 0x00405A43.
 *   - divisors /0x4B0 (roll) and /0x226 (pitch):
 *     CONFIRMED via IMUL constants 0x1B4E81B5 SAR 7 + 0x77280773 SAR 8.
 *   - pattern-clamp switch table on prev_air (bVar2): MATCHES.
 *
 * STATUS: Static audit COMPLETE — function is byte-faithful with one
 * SHIPPED fix (D1 dot>>1 → dot/2). Per the chassis-launch chain
 * investigation (agent #41) the residual runtime divergence is UPSTREAM
 * in ComputeActorTrackContactNormalExtended @ 0x00403720 (pool1) — wcv
 * sign / hires shift / gap_270 chain — and the side-channel cardef-
 * loading issue for PlayerIsAI=1 slot 0.
 *
 * KNOWN TODO CHAIN OWNERS (chassis-launch / suspension cascade):
 *   - todo_edinburgh_chassis_launch_proximate_cause.md (wcb=0 transient)
 *   - todo_edinburgh_span_433_residual_launch.md (second crest launch)
 *   - todo_chassis_snap_fix_2026-05-16.md (floorf vs FISTP-RNE rounding)
 *
 * Audit reference: re/analysis/pilot_004057F0_audit.md (pool6, 2026-05-14).
 * Effective level: L4 (byte-faithful per static audit; upstream-blocked).
 */

/* [FIX 2026-06-21] QoL damp for the documented "wcb=0 chassis launch" on steep
 * DESCENTS (user repro: Scotland ~span 2078, a real ~33° descent; cf. the TODO
 * notes above re: edinburgh_chassis_launch / wcb=0 transient). Driving DOWN a
 * steep grade, the contact/suspension path accrues spurious UPWARD chassis
 * velocity and the car takes off. descent_damp_apply() is called once per actor
 * from the SHARED td5_physics_integrate_pose (covering the 4-wheel PLAYER and the
 * 2-axle AI OPPONENTS alike — both populate wheel contacts there): when the
 * actor has ground contact AND the ground DROPPED this tick (descent) yet
 * linear_velocity_y is positive (rising), it replaces that velocity with the
 * ground's own drop rate so the chassis FOLLOWS the hill down instead of
 * launching. Scoped tight: it never fires while fully airborne (real jumps) or
 * on uphills (ground rising, dg > 0). Default ON; TD5RE_DESCENT_DAMP=0 reverts.
 * Per-slot state for netplay determinism (getenv is identical launch config on
 * every peer; the damp is a deterministic function of replicated sim state). */
static int descent_damp_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) {
        const char *e = getenv("TD5RE_DESCENT_DAMP");
        s_en = (e && e[0] == '0') ? 0 : 1;
    }
    return s_en;
}
static int32_t s_descent_prev_ground[16];
static int     s_descent_prev_valid[16];
/* Min per-tick ground DROP (24.8 fp) counted as "descending". The steep Scotland
 * section drops ~76800 fp/tick at speed; 8000 ignores flat-ground noise while
 * catching any genuine descent. */
#define DESCENT_DAMP_MIN_DROP 8000
/* Max UPWARD chassis velocity (24.8 fp) tolerated for a GROUNDED car. ~16000 (≈63
 * world units/tick = a brisk vertical climb) clears real bumps/kerbs but clamps
 * the launch/snap spike (which reaches tens of thousands). Jumps go airborne first
 * (early return), so ramps/crests are unaffected. */
#define DESCENT_DAMP_MAX_UP 16000

/* [FIX 2026-06-21] Slope-roll damp. The wheel-derived attitude (attitude_from_wheels)
 * tips the car side-to-side on steep grades because the right-side wheel probes
 * over-walk a span and read asymmetric heights (the documented "rolls out of
 * control on steep slopes" residual — exposed once the descent-launch was fixed).
 * On a STEEP grade driven roughly STRAIGHT, the true roll should be ~0, so scale
 * the computed roll toward level. Cornering / banked turns keep their real roll
 * (high yaw rate gates the damp off). Default ON; TD5RE_ROLL_DAMP=0 reverts. */
static int roll_damp_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) {
        const char *e = getenv("TD5RE_ROLL_DAMP");
        s_en = (e && e[0] == '0') ? 0 : 1;
    }
    return s_en;
}
/* |pitch| (12-bit angle, abs in [0,2048]) above which the grade counts as steep.
 * ~205 ≈ 18°. */
#define ROLL_DAMP_PITCH_MIN 205
/* |angular_velocity_yaw| below which the car counts as going straight (not
 * cornering). Conservative so only genuinely-straight descents are damped. */
#define ROLL_DAMP_YAW_MAX   1500
/* Fraction of the wheel-derived roll KEPT on a steep straight grade (Q8).
 * 0x50 ≈ 0.31 → most of the spurious slope-tilt removed, a little kept so the
 * transition isn't a hard snap. */
#define ROLL_DAMP_KEEP_Q8   0x50

static void descent_damp_apply(TD5_Actor *actor)
{
    if (!descent_damp_enabled()) return;
    int dslot = actor->slot_index & 0x0F;
    /* Average ONLY the grounded wheels (an airborne wheel_contact_pos.y is the
     * un-snapped transform Y and would pollute the ground reference). Any grounded
     * wheel gives a valid descent reference — on a bouncy descent the car often
     * has only partial contact while it pumps upward velocity. */
    int32_t gnd_sum = 0; int gnd_cnt = 0;
    for (int wi = 0; wi < 4; wi++) {
        if (((actor->wheel_contact_bitmask >> wi) & 1) == 0) {   /* bit clear = grounded */
            gnd_sum += actor->wheel_contact_pos[wi].y; gnd_cnt++;
        }
    }
    if (gnd_cnt == 0) { s_descent_prev_valid[dslot] = 0; return; }  /* airborne: no ref */
    int32_t gnd = gnd_sum / gnd_cnt;
#ifndef TD5RE_RELEASE
    int32_t before_vy = actor->linear_velocity_y;
#endif
    if (s_descent_prev_valid[dslot] && actor->linear_velocity_y > 0) {
        int32_t dg = gnd - s_descent_prev_ground[dslot];
        if (dg < -DESCENT_DAMP_MIN_DROP)     /* ground dropped -> descent */
            actor->linear_velocity_y = dg;   /* follow the hill down, don't launch */
    }
    /* Backstop: a GROUNDED car cannot plausibly have a large UPWARD velocity —
     * only a launch / ground-probe spike produces it. Clamp it even when the
     * descent test above didn't fire (catches the AI snap-spike and any partial-
     * contact moment the descent gate misses). Real jumps go through the airborne
     * early-return above, so ramps/crests are untouched. */
    if (actor->linear_velocity_y > DESCENT_DAMP_MAX_UP)
        actor->linear_velocity_y = DESCENT_DAMP_MAX_UP;
#ifndef TD5RE_RELEASE
    if (actor->slot_index <= 1 && actor->linear_velocity_y != before_vy) {
        static int s_dd_budget = 1500;
        if (s_dd_budget > 0) {
            s_dd_budget--;
            TD5_LOG_I(LOG_TAG, "descent-damp slot=%d vy %d->%d gnd=%d gnd_cnt=%d",
                      (int)actor->slot_index, before_vy,
                      actor->linear_velocity_y, gnd, gnd_cnt);
        }
    }
#endif
    s_descent_prev_ground[dslot] = gnd;
    s_descent_prev_valid[dslot]  = 1;
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

    /* [FIX 2026-05-31 falling-car-ground-impact-sound] Faithful port of the
     * normal-mode wheel-LANDING thud from UpdateVehicleSuspensionResponse
     * @0x004057F0. Orig: after averaging the per-just-landed-wheel impact into
     * `bounce` (iVar8), if bounce > 0x14 it plays sound 0x17 (Bottom*.wav,
     * 4 variants) at volume bounce*0x32, freq 0x5622, positioned at the actor
     * world_pos (+0x1FC). This is the "car falls off a slope and hits the
     * ground" thud the user reported missing — a SEPARATE emitter from the
     * collision/scripted-recovery thud at 0x004096B0 (which only runs in
     * vehicle_mode==1). RE basis: Ghidra agent (ab81bbcf) confirmed
     * iVar8>0x14 → PlayVehicleSoundAtPosition(0x17, iVar8*0x32, 0x5622,
     * actor+0x1FC, 4); the *0x32 multiplier is also corroborated by this
     * function's own line-by-line audit (the prior `bounce*50` stub comment).
     * Previously omitted ("sound side stubbed") → no landing thud on falls. */
    if (bounce > 0x14) {
        int32_t world_pos[3] = { actor->world_pos.x,
                                 actor->world_pos.y,
                                 actor->world_pos.z };
        td5_sound_play_at_position(0x17, bounce * 0x32, 0x5622, world_pos, 4);
    }

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

    /* [FIX 2026-06-21] Descent-launch damp for the PLAYER (this-tick wheel
     * contacts, applied after the faithful bounce write, before integrate_pose
     * integrates Y). AI opponents are damped from integrate_pose instead (they
     * never run this function). See descent_damp_apply(). */
    descent_damp_apply(actor);

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

/* [TRACE 2026-05-24 traffic-edge-pen-cluster] Per-call context so the
 * caller can tell traffic_edge_pen which call-site it is + which slot is
 * being tested. Gated by g_td5.ini.trace_traffic_edge_pen (INI key
 * [Trace] TrafficEdgePen=1). When disabled the trace path is a single
 * compare-and-bail at the top of traffic_edge_pen — no measurable cost.
 * Mirrors tools/_probes/traffic_edge_pen_probe.js capture for orig. */
static struct {
    const char *call_id;   /* "1a" inner / "1b" outer / "2" fwd-cp / "3" route-adv */
    int slot;
    int span_idx;
    int sub_lane;
    int span_type;
    int lane_count;
    int32_t rel_x;
    int32_t rel_z;
    int armed;             /* 1 = pen log is active for the next call */
} s_tep_trace;

static FILE *s_tep_trace_fp = NULL;

static void tep_trace_ensure_open(void)
{
    if (s_tep_trace_fp) return;
    s_tep_trace_fp = fopen("tools/frida_csv/traffic_edge_pen_port.csv", "w");
    if (s_tep_trace_fp) {
        fprintf(s_tep_trace_fp,
            "sim_tick,call_id,slot,span_idx,sub_lane,span_type,lane_count,"
            "A_x,A_z,B_x,B_z,rel_x,rel_z,pen_orig\n");
        fflush(s_tep_trace_fp);
    }
}

static void tep_trace_arm(const char *call_id, int slot,
                          int span_idx, int sub_lane,
                          int span_type, int lane_count,
                          int32_t rel_x, int32_t rel_z)
{
    if (!g_td5.ini.trace_traffic_edge_pen) { s_tep_trace.armed = 0; return; }
    s_tep_trace.call_id    = call_id;
    s_tep_trace.slot       = slot;
    s_tep_trace.span_idx   = span_idx;
    s_tep_trace.sub_lane   = sub_lane;
    s_tep_trace.span_type  = span_type;
    s_tep_trace.lane_count = lane_count;
    s_tep_trace.rel_x      = rel_x;
    s_tep_trace.rel_z      = rel_z;
    s_tep_trace.armed      = 1;
}

static void tep_trace_emit(int32_t v0_x, int32_t v0_z,
                           int32_t v1_x, int32_t v1_z, int32_t pen)
{
    if (!s_tep_trace.armed) return;
    s_tep_trace.armed = 0;
    tep_trace_ensure_open();
    if (!s_tep_trace_fp) return;
    /* "sim_tick" here is the port's simulation_tick_counter (mirrors
     * g_simTick @ 0x004AADA0 captured by the Frida orig probe). */
    fprintf(s_tep_trace_fp,
        "%u,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        (unsigned)g_td5.simulation_tick_counter,
        s_tep_trace.call_id,
        s_tep_trace.slot,
        s_tep_trace.span_idx,
        s_tep_trace.sub_lane,
        s_tep_trace.span_type,
        s_tep_trace.lane_count,
        (int)v0_x, (int)v0_z, (int)v1_x, (int)v1_z,
        (int)s_tep_trace.rel_x, (int)s_tep_trace.rel_z,
        (int)pen);
    fflush(s_tep_trace_fp);
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
/* Signed edge penetration for traffic containment — REWRITTEN 2026-05-26 to
 * match orig ProcessActorSegmentTransition @ 0x00407390 (was a divergent
 * parallel-projection re-impl that fired spuriously every tick → traffic
 * locked, see reference_traffic_steer_saturation_2026-05-26).
 *
 * Edge runs A→B. Orig builds the rotated OUTWARD NORMAL
 *   normal = (B.z - A.z, A.x - B.x)   [edge tangent rotated -90°]
 * and normalizes it to length 4096 (ConvertFloatVec4ToShortAngles @ 0x0042CDB0
 * = FPU x²+z²→FSQRT→4096/len→FMUL→ftol). Penetration is the signed
 * perpendicular distance of the actor from the edge, minus the heading-folded
 * car half-extent:
 *   rel  = (actor_world>>8 - origin - A)
 *   pen  = rel·normal - (sin_hd*half_w + cos_hd*half_l)
 * pen < 0 ⇒ the car has crossed the edge → containment push.
 * out_edge_angle = AngleFromVector12(B.z-A.z, B.x-A.x) for the push direction.
 *
 * Scale: rel is world units, normal is ×4096, so rel·normal is the perp
 * distance ×4096; car_extent is (sin/cos ×4096)·half so also ×4096. No shift —
 * matches orig (which leaves both at ×4096 and tests sign only). */
static int32_t traffic_edge_pen(int32_t a_x, int32_t a_z,
                                 int32_t b_x, int32_t b_z,
                                 int32_t rel_x, int32_t rel_z,
                                 int32_t car_half_w, int32_t car_half_l,
                                 int32_t cos_hd, int32_t sin_hd,
                                 uint32_t *out_edge_angle)
{
    if (out_edge_angle)
        *out_edge_angle = (uint32_t)(atan2_fixed12(b_z - a_z, b_x - a_x) & 0xFFF);

    /* Outward edge normal, normalized to length 4096. */
    double dnx = (double)(b_z - a_z);
    double dnz = (double)(a_x - b_x);
    double len = sqrt(dnx * dnx + dnz * dnz);
    int32_t nx = 0, nz = 0;
    if (len >= 1.0) {
        nx = (int32_t)(dnx * 4096.0 / len);
        nz = (int32_t)(dnz * 4096.0 / len);
    }

    int32_t proj       = rel_z * nz + rel_x * nx;
    int32_t car_extent = sin_hd * car_half_w + cos_hd * car_half_l;
    int32_t pen        = proj - car_extent;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] mirror Frida orig probe */
    tep_trace_emit(a_x, a_z, b_x, b_z, pen);

    return pen;
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
 *
 * [L4 — Frida-pending] Multiple algorithmic divergences vs orig
 * 0x00407390. Documented here per L5 promotion audit 2026-05-18.
 * Resolution unblocks on Frida-traced traffic-wall contact comparison
 * (see todo_traffic_route_advance_lane_count_byte_2026-05-18.md for the
 * unblock criterion). NOT ARCH-DIVERGENCE — these are genuine math gaps:
 *
 *   1) DOT-PRODUCT SEMANTIC MISMATCH (the dominant divergence)
 *      Orig builds the *rotated outward normal* of edge A→B via
 *      `local_8 = (B.z - A.z, 0, A.x - B.x)` and normalizes it to length
 *      4096 via ConvertFloatVec4ToShortAngles @ 0x0042CDB0 (FPU
 *      x²+y²+z²→FSQRT→4096/len→FMUL→ftol). It then computes
 *      `pen = ((world - origin - A) . local_8) - car_size_proj` — a
 *      *signed perpendicular distance* (geometric cross-product component
 *      of (rel) and the unit edge direction).
 *
 *      Port computes `tan = (cos(atan2(tdz,tdx)), sin(atan2(tdz,tdx)))`
 *      and dots `(rel . tan)` — a *parallel projection along the edge*,
 *      NOT the perpendicular distance. These give different signs in
 *      general; only the angle passed to ApplySimpleTrackSurfaceForce
 *      remains the same.
 *
 *   2) REFERENCE-POINT MISMATCH
 *      Orig dots from psVar1 (the edge endpoint), port dots from the span
 *      origin. The car-size projection subtraction is identical but the
 *      pre-subtraction term is offset by `-psVar1`.
 *
 *   3) VERTEX-PAIR SWAP (OUTER TEST)
 *      Orig outer-test uses `psVar1 = vertex(strip[+0x04] + DAT_004631a0
 *      + nibble) = left_vertex_index+offset+nibble`, port uses
 *      `right_vertex_index+offset+outer_sub`. The port comment at line 5254
 *      claims "strip[+0x04] = right_vertex_index" but td5_types.h:383-384
 *      assigns strip[+0x04] = left_vertex_index. The swap is internally
 *      inconsistent and likely contributes a sign flip on outer-edge tests.
 *
 *   4) (PARTIAL CLOSE) lane_count byte source (+0x03 not +0x01) — fixed
 *      2026-05-18 commit ef0e862. Outer_sub mapping (= lane_count not
 *      lane_count-1) — fixed same commit. These two are no longer
 *      divergent.
 *
 * Why this still ships:
 *
 *   The traffic-bias-clamp slot gate shipped 2026-05-17 (commit 79023df F1)
 *   bounds the pre-loop to slots 0..5 (TD5_MAX_RACER_SLOTS), preventing
 *   traffic from cascading into the racer steering pipeline. The
 *   remaining edge-test errors fire only at traffic-wall contact ticks
 *   which are rare enough that no current Wave3 scenario isolates this
 *   path. San Francisco scenario (slot 6+ traffic, see
 *   reference_san_francisco_ai_completion_2026-05-16.md) is the closest
 *   isolated test bed but no traffic-wall contacts have been Frida-traced
 *   yet.
 *
 * Unblock criterion (per todo): Frida hook at orig 0x004073D8 + port
 * traffic_edge_pen entrypoint capturing (slot, span, sub_lane, type, A.x,
 * A.z, B.x, B.z, rel.x, rel.z, pen_orig, pen_port) at the first
 * traffic-wall contact tick. If `sign(pen_orig) == sign(pen_port)`
 * across all sampled ticks the divergence is benign and these notes
 * upgrade to ARCH-DIVERGENCE; otherwise file as a fix-required regression.
 *
 * NOTE: confirmed byte-faithful for: lane_count byte source (+0x03,
 * fixed 2026-05-18), outer_sub mapping (= lane_count, fixed 2026-05-18),
 * inner/outer trigger conditions (sub_lane < 2 / sub_lane >= lane_count-2),
 * DAT_004631a0 outer-vertex LUT (k_outer_left_offsets matches orig dump),
 * ApplySimpleTrackSurfaceForce wiring + DecayUltimateVariantTimer wiring.
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
    /* [CONFIRMED @ 0x004073B0: `uVar10 = *(byte*)(strip+3) & 0xf`] — orig
     * reads byte +0x03 (geometry_metadata low nibble = lane_count), NOT
     * byte +0x01 (surface_attribute). Use the helper that mirrors orig. */
    int lane_count = (int)(((const uint8_t *)sp)[3] & 0x0F);

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

    /* Inner edge test: sub_lane < 2  [CONFIRMED @ 0x407394: if (iVar12 < 2)]
     * [FIX 2026-05-26] Orig 0x004073A4-0x004073B0: A=psVar1=right_vertex(+6),
     * B=psVar2=left_vertex(+4), NO sub_lane offset (sub_lane is only the gate).
     * Prior port added sub_lane to both indices (wrong). */
    if (sub_lane < 2) {
        TD5_StripVertex *A = td5_track_get_vertex((int)sp->right_vertex_index);
        TD5_StripVertex *B = td5_track_get_vertex((int)sp->left_vertex_index);
        if (!A || !B) goto outer_test;

        /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=1a (inner) */
        tep_trace_arm("1a", slot, (int)actor->track_span_raw, sub_lane,
                       span_type, lane_count, rel_x, rel_z);

        /* rel measured from edge endpoint A (orig dots from psVar1). */
        int32_t arel_x = rel_x - (int32_t)A->x;
        int32_t arel_z = rel_z - (int32_t)A->z;
        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)A->x, (int32_t)A->z,
            (int32_t)B->x, (int32_t)B->z,
            arel_x, arel_z,
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
            return;
        }
    }

outer_test:
    /* Outer edge test: sub_lane >= lane_count - 2  [CONFIRMED @ 0x407462]
     * [FIX 2026-05-26] Orig outer 0x4074B4-0x4074F4:
     *   A = psVar1 = vtx[DAT_004631a0[type] + left_vertex_index(+4) + lane_count]
     *   B = psVar2 = vtx[DAT_004631a4[type](=0) + right_vertex_index(+6) + lane_count]
     * Prior port had LEFT/RIGHT swapped. DAT_004631a0 = k_outer_left_offsets. */
    if (sub_lane >= lane_count - 2) {
        static const int8_t k_outer_left_offsets[12] = {
            0, 0, -1, -1, -2, 0, -1, 0, -1, 0, -1, -2
        };
        int a_idx = k_outer_left_offsets[span_type] + (int)sp->left_vertex_index  + lane_count;
        int b_idx = (int)sp->right_vertex_index + lane_count;   /* DAT_004631a4 = 0 */
        TD5_StripVertex *A = td5_track_get_vertex(a_idx);
        TD5_StripVertex *B = td5_track_get_vertex(b_idx);
        if (!A || !B) return;

        /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=1b (outer) */
        tep_trace_arm("1b", slot, (int)actor->track_span_raw, sub_lane,
                       span_type, lane_count, rel_x, rel_z);

        int32_t arel_x = rel_x - (int32_t)A->x;
        int32_t arel_z = rel_z - (int32_t)A->z;
        uint32_t edge_angle;
        int32_t pen = traffic_edge_pen(
            (int32_t)A->x, (int32_t)A->z,
            (int32_t)B->x, (int32_t)B->z,
            arel_x, arel_z,
            (int32_t)car_half_w, (int32_t)car_half_l,
            cos_hd, sin_hd,
            &edge_angle);

        if (pen < 0) {
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
 *
 * [CONFIRMED @ 0x00407840] Byte-faithful with orig ProcessActorRouteAdvance.
 *   - Wrap-span strip indexed at DAT_00483550 (total_spans), matching the
 *     port's `td5_track_get_span(0)` ring-wrap.
 *   - lane_count byte source = byte +0x03 (fixed 2026-05-18, was +0x01).
 *   - Vertex pair = single rail from base to base+lane_nibble (fixed
 *     2026-05-18, was both-rails+sub_lane).
 *
 * Algorithm:
 *   uVar6 = strip[wrap].left_vertex_index
 *   psVar2 = vertex_pool[uVar6 + (strip[wrap+3] & 0xF)]   ; left + lane_nibble
 *   psVar3 = vertex_pool[uVar6]                            ; left base
 *   pen    = ((world - psVar2) . (rotated_edge_normal)) - car_size
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

    /* [CONFIRMED @ 0x004078B7] orig reads strip[wrap+3]&0xF = lane_nibble. */
    int wrap_lanes = (int)(((const uint8_t *)sp_wrap)[3] & 0x0F);

    /* [CONFIRMED @ 0x004078A4-0x004078BA] orig uses ONE rail of the wrap span:
     *   uVar6 = sp_wrap->left_vertex_index
     *   psVar2 = vertex(uVar6 + lane_nibble)    ; far endpoint
     *   psVar3 = vertex(uVar6)                  ; near endpoint
     * Port maps: vl = psVar2 (vertex at left+lane_nibble),
     *            vr = psVar3 (vertex at left base). traffic_edge_pen will
     * compute the tangent angle = atan2(vr.z - vl.z, vr.x - vl.x) =
     * angle of edge `(left+nibble) -> left`, equivalent to orig's
     * AngleFromVector12(psVar3.z - psVar2.z, psVar3.x - psVar2.x). */
    int li_idx = (int)sp_wrap->left_vertex_index + wrap_lanes;  /* psVar2 */
    int ri_idx = (int)sp_wrap->left_vertex_index;                /* psVar3 */
    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;
    /* Note: actor->track_sub_lane_index is intentionally unread here —
     * orig uses single-rail-across-full-width, NOT sub_lane indexed. */

    /* Actor position relative to wrap span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp_wrap->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp_wrap->origin_z;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=3 (route-adv) */
    tep_trace_arm("3", slot, (int)actor->track_span_raw,
                   (int)(int8_t)actor->track_sub_lane_index,
                   (int)sp_wrap->span_type, wrap_lanes, rel_x, rel_z);

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
     *   psVar2 = vertex[uVar6]                 (left base, NO sub_lane offset)
     *   psVar3 = vertex[uVar6 + lane_count]    (right end)
     * Compare ProcessActorRouteAdvance which swaps: psVar2=right, psVar3=left.
     *
     * [FIX 2026-06-01] li_idx previously added `+ sub` (the actor's sub_lane) to
     * the NEAR vertex — a divergence the original lacks. Independently re-confirmed
     * via Ghidra (0x407776-0x40777B): the near vertex is vertex[left_vertex_index],
     * and the actor sub_lane is NEVER read in 0x4076C0 (the only addend anywhere is
     * the lane-count nibble on the FAR vertex). The inner/outer
     * ProcessActorSegmentTransition path was already corrected 2026-05-26 (sub_lane
     * is only the edge-gate there); this forward-checkpoint path was the last site
     * still adding sub. Dropped — `sub` is retained only for the trace arg below. */
    int sub = (int)(int8_t)actor->track_sub_lane_index;
    if (sub < 0) sub = 0;
    int lane_count = (int)(sp->surface_attribute & 0xF);
    if (sub >= lane_count) sub = lane_count - 1;

    int li_idx = (int)sp->left_vertex_index;               /* psVar2 = left base (orig: NO sub) */
    int ri_idx = (int)sp->left_vertex_index + lane_count;  /* psVar3 = end vertex (NOT right_vertex_index) */

    TD5_StripVertex *vl = td5_track_get_vertex(li_idx);
    TD5_StripVertex *vr = td5_track_get_vertex(ri_idx);
    if (!vl || !vr) return;

    /* Actor position relative to sentinel span origin */
    int32_t rel_x = (actor->world_pos.x >> 8) - sp->origin_x;
    int32_t rel_z = (actor->world_pos.z >> 8) - sp->origin_z;

    /* [TRACE 2026-05-24 traffic-edge-pen-cluster] arm call_id=2 (fwd-cp) */
    tep_trace_arm("2", slot, (int)actor->track_span_raw, sub,
                   (int)sp->span_type, lane_count, rel_x, rel_z);

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
         * Original (Y-DOWN): `ADD [world_pos_y], ECX` with ECX = MOVSX cdef[0x86] << 8.
         * Port runs Y-UP so the equivalent lift is a subtract.
         *
         * compute_suspension_envelope @ 0x0042F8F4 writes cdef[0x86]:
         *   - Racer  (slot<6): negative ((cd[0x42]-cd[0x82]) * -0.7070...)
         *   - Traffic(slot>=6): positive (mesh max_y from vertex extents)
         *
         * This function is traffic-only, so cdef[0x86] is normally positive.
         * Subtracting a positive value would SINK the traffic vehicle into
         * the ground (user-reported "traffic clipping through ground"
         * 2026-05-26). Use the magnitude so the lift direction matches
         * racers regardless of the sign that compute_envelope chose. */
        if (actor->car_definition_ptr) {
            int32_t height_offset = (int32_t)CDEF_S(actor, CDEF_HEIGHT_OFFSET) << 8;
            if (height_offset > 0) height_offset = -height_offset;
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

    /* 8. Refresh the 4 wheel-contact probes (+0x90..0xbc) for the shadow.
     * [FIX 2026-05-26 r2 traffic-shadow-misaligned]
     * render_vehicle_shadow_quad (td5_render.c) reads probe_FL/FR/RL/RR as the
     * 4 ground corners of the shadow decal. The racer path computes these in
     * td5_physics_refresh_wheel_contacts (0x00403720) by transforming the
     * car-def BODY-CORNER offsets (4x int16[4] at the head of car_definition_
     * ptr) through the full render matrix, then OVERWRITING each corner Y with
     * the barycentric ground-contact height at that XZ. Traffic never runs
     * 0x00403720, so the r1 fix here hand-rolled a (±half_w,±half_l) box at
     * world_pos.y — which (a) used the wrong axis fields, rendering the decal
     * PERPENDICULAR to the car, and (b) sat at the body-CENTRE height, so the
     * shadow floated above the road and clipped through the body. Mirror the
     * proven racer body-corner pass exactly: real corners via render matrix +
     * ground-contact Y. */
    if (actor->car_definition_ptr) {
        TD5_Vec3_Fixed *body_pos[4] = {
            &actor->probe_FL, &actor->probe_FR, &actor->probe_RL, &actor->probe_RR
        };
        const int16_t *cardef_corners = (const int16_t *)actor->car_definition_ptr;

        float matrix[12];
        for (int j = 0; j < 9; j++) matrix[j] = actor->rotation_matrix.m[j];
        matrix[9]  = actor->render_pos.x;
        matrix[10] = actor->render_pos.y;
        matrix[11] = actor->render_pos.z;

        int max_sp = td5_track_get_span_count();
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
                                            body_pos[i]->x, body_pos[i]->z);
            if (max_sp > 0 && actor->body_probes[i].span_index >= (int16_t)max_sp)
                actor->body_probes[i].span_index = (int16_t)(max_sp - 1);
            td5_track_compute_probe_contact_vertices(&actor->body_probes[i]);

            int probe_span = actor->body_probes[i].span_index;
            int probe_lane = actor->body_probes[i].sub_lane_index;
            if (probe_span >= 0 && probe_span < max_sp) {
                body_pos[i]->y = td5_track_compute_contact_height_with_normal(
                    probe_span, probe_lane, body_pos[i]->x, body_pos[i]->z, NULL);
            }
        }
    }

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
 *
 * [2026-05-17 OPERAND-SOURCE AUDIT — closes body-roll TODO Item 1]
 * The IntegrateVehiclePoseAndContacts caller at 0x00405FEC pushes:
 *     PUSH &actor->wheel_suspension_pos_FL   ; param_3 (sp[])
 *     PUSH &actor->field_0xf0                ; param_2 (wheel_contact_pos)
 *     PUSH &actor->display_angle_roll        ; param_1 (out)
 * Disassembly of 0x00446030 reads `[ESI+4/+0x10/+0x1C/+0x28]` from param_2
 * (stride 12, offset +4) which is the Y component of each of the 4
 * wheel_contact_pos entries. The port's `wcp[1]/wcp[4]/wcp[7]/wcp[10]`
 * indices (int32_t pointer) map 1-to-1 to those byte offsets. The X/Z
 * cross-spans (wcp[0]/3/6/9 and wcp[2]/5/8/11) match the listing too. */
/* ======================================================================== */
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

    /* Diagnostic for [[todo-car-tilted-right-flat-surface-2026-05-19]]:
     * on flat ground all 4 wheel Y values are equal and sp[0..3]==0 →
     * dx=dz=0 → roll=pitch=0. If user reports persistent tilt, this log
     * surfaces which input is biased. Slot-0 only, rate-limited. */
    if (actor->slot_index == 0) {
        static int s_attitude_log_div = 0;
        if ((++s_attitude_log_div % 120) == 0) {
            TD5_LOG_I(LOG_TAG,
                "attitude_from_wheels slot0: roll=%d pitch=%d "
                "dz_pre=%d dx_pre=%d sp=[%d,%d,%d,%d] "
                "wcp_y=[%d,%d,%d,%d]",
                (int)*out_roll, (int)*out_pitch,
                (int)dz, (int)dx,
                (int)sp[0], (int)sp[1], (int)sp[2], (int)sp[3],
                (int)wcp[1], (int)wcp[4], (int)wcp[7], (int)wcp[10]);
        }
    }
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
     * [E1 / 2026-05-02 — physics_trace.csv ground-truth diff]
     *
     * 2026-05-15 PRECISION FIX: replaced `(float)cos_fixed12(a) * (1/4096)`
     * with direct `CosFloat12bit(a)`. The earlier path went int32-truncate →
     * float → divide, which clamps tiny `sin(small_angle)` outputs to 0
     * (cast to int32 truncates toward zero). Original 0x0042E1E0 reads
     * `g_sinCosFloatTable` directly as float32; for example at slot-0 tick-1
     * with near-zero roll/pitch, the original keeps sin(small) at its
     * float32 precision (~1e-3 .. 1e-6) while the port collapsed it to 0,
     * producing rotation_matrix.m[3] = port=0 vs orig=−7e−11 and cascading
     * into wheel_world_positions_hires.y off-by-1 LSB and probe_FR_x ±1
     * world unit. [whole_state_diff slot-0 tick-1 labelled cluster, 2026-05-15]
     *
     * 2026-05-22 FPU SPILL FIX (session_02): orig FSTPs four intermediate
     * products to float32 memory slots BEFORE the final FMUL/FADD chain:
     *   slot F = (float)(sp*sy)   slot G = (float)(cp*cy)
     *   slot H = (float)(cp*sy)   slot I = (float)(sp*cy)
     * Then m[0]/m[1]/m[6]/m[7] consume these float32 spills, so each entry
     * is `round_f32( round_f32(a*b) * c ± round_f32(d*e) )`, NOT the all-
     * 80-bit chain GCC produces by default. Use `volatile float` to force
     * MinGW (-m32, x87, no SSE) to emit FSTP-to-memory then FLD-from-memory
     * at exactly the orig spill points. This is the literal port of
     * 0x0042e244, 0x0042e250, 0x0042e26a, 0x0042e274. m[2]/m[3]/m[4]/m[5]/m[8]
     * have only one trig product each (no intermediate spill needed) and
     * already match orig at ULP level. */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy); /* slot F @ orig 0x42E244 */
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy); /* slot G @ orig 0x42E250 */
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy); /* slot H @ orig 0x42E26A */
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy); /* slot I @ orig 0x42E274 */

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
        actor->rotation_matrix.m[8] = cy * cr;

        /* Per-frame rotation_matrix dump for cross-binary diff against orig
         * Frida probe of 0x0042E1E0 (tools/_probes/rotation_matrix_probe.js).
         * Gated on RaceTrace + slot 0 so it does not flood normal runs.
         * [session_02 follow-up, 2026-05-22] */
        if (actor->slot_index == 0 && g_td5.ini.race_trace_enabled) {
            const float *m = actor->rotation_matrix.m;
            TD5_LOG_I(LOG_TAG,
                "ROTMAT slot0 fc=%u roll=%d yaw=%d pitch=%d "
                "m=[%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f] "
                "wp=(%d,%d,%d)",
                actor->frame_counter,
                (int)actor->display_angles.roll,
                (int)actor->display_angles.yaw,
                (int)actor->display_angles.pitch,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
                actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
        }
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

            /* [FIX 2026-06-21] Slope-roll damp: on a STEEP grade driven roughly
             * STRAIGHT, scale the wheel-derived roll toward level (the spurious
             * over-walk tilt). Cornering keeps its roll (yaw-rate gate). See
             * roll_damp_enabled(). */
            if (roll_damp_enabled()) {
                int32_t pitch_abs = (int32_t)new_pitch & 0xFFF;       /* 12-bit */
                if (pitch_abs > 0x800) pitch_abs = 0x1000 - pitch_abs; /* fold to [0,2048] */
                int32_t yaw_rate = actor->angular_velocity_yaw;
                if (yaw_rate < 0) yaw_rate = -yaw_rate;
                if (pitch_abs > ROLL_DAMP_PITCH_MIN && yaw_rate < ROLL_DAMP_YAW_MAX) {
                    int32_t roll_s = (int32_t)new_roll & 0xFFF;        /* 12-bit */
                    if (roll_s > 0x800) roll_s -= 0x1000;              /* -> signed */
                    roll_s = (roll_s * ROLL_DAMP_KEEP_Q8) >> 8;        /* scale toward 0 */
                    new_roll = (int16_t)(roll_s & 0xFFF);
                }
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
         * display_angles in float precision (E1 fix — see line ~4361).
         * 2026-05-15: switched from int Q12 trig to LUT-style float trig
         * (CosFloat12bit / SinFloat12bit) to preserve sub-Q12 magnitudes
         * for near-zero pitch/roll. Mirrors the per-tick builder above. */
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
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

        int32_t href = (int32_t)CDEF_S(actor, CDEF_SUSP_REF_HEIGHT);
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

            /* [FIX 2026-05-27 — wheels-lift-on-slopes root] Per-tick update of
             * the RENDER wheel-offset Y to the suspension-adjusted value
             * (cwy - sp_div), matching the orig integrate_pose loop @ 0x00405E80
             * (`*psVar10 = body_wy; ConvertFloatVec3ToShortAngles(...); *psVar10
             * += susp_offset` → wheel_display_angles[i][1] = cwy - sp_div).
             *
             * The port set wheel_display_angles ONCE at init (raw cardef Y, no
             * suspension — td5_physics.c ~9881), so the rendered wheel billboards
             * were RIGID and did NOT follow per-tick suspension travel. On slopes
             * the springs load differentially (and orientation-dependently), so
             * the static wheels floated/sank off the road — the "rear wheels lift
             * a lot uphill / side wheels lift sideways" symptom — even though the
             * 4-wheel attitude SOLVER (0x00446030) is correct. X,Z stay static
             * (cardef); only Y tracks the spring, grounded wheels only (the loop's
             * airborne `continue` mirrors the orig's damage_lockout gate). */
            /* [WHEEL OVERHAUL 2026-06-12] Render-only suspension clamp so the
             * wheel can't ride up INTO the chassis (or droop absurdly far) on
             * hard bumps. disp_y = cwy - sp_div is the wheel-centre Y arm; cwy
             * is its rest position, so (disp_y - cwy) = -sp_div is the visual
             * travel. Bound |travel| to WHEEL_VIS_TRAVEL body-units. This
             * touches ONLY the rendered wheel position — body_wy (which feeds
             * the faithful chassis ground-snap below) is left intact, so
             * gameplay/diff-race parity is unchanged. A/B: TD5RE_WHEEL_SUSP_CLAMP=0
             * disables; TD5RE_WHEEL_VIS_TRAVEL=N overrides the limit. */
            int32_t disp_y = (int32_t)body_wy + susp_offset;   /* = cwy - sp_div */
            {
                static int s_clamp_on = -1, s_travel = -1;
                if (s_clamp_on < 0) {
                    const char *e = getenv("TD5RE_WHEEL_SUSP_CLAMP");
                    s_clamp_on = (e && e[0] == '0') ? 0 : 1;
                    const char *t = getenv("TD5RE_WHEEL_VIS_TRAVEL");
                    s_travel = (t && t[0]) ? atoi(t) : 40;   /* body-units */
                    if (s_travel < 1) s_travel = 1;
                }
                if (s_clamp_on) {
                    int32_t lo = (int32_t)cwy - s_travel;
                    int32_t hi = (int32_t)cwy + s_travel;
                    if (disp_y < lo) disp_y = lo;
                    if (disp_y > hi) disp_y = hi;
                }
            }
            actor->wheel_display_angles[i][1] = (int16_t)disp_y;

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

            /* [WHEELGAP LOG 2026-05-27] Per-wheel RENDERED world-Y minus GROUND
             * contact-Y (world units, ÷256). Wheels render at world_pos +
             * body_matrix*wheel_display_angles[w] (+0x210); positive gap = that
             * wheel floats above the road. Self-contained capture: run with
             * AutoThrottle to drive up the hill, grep WHEELGAP at span ~160. */
            if (actor->slot_index == 0) {
                static int s_wg_div = 0;
                if ((s_wg_div++ % 15) == 0) {
                    uint8_t *ab = (uint8_t *)actor;
                    float wpy = (float)(*(int32_t *)(ab + 0x200)) / 256.0f;
                    /* body->world Y uses ROW 1 of the rotation matrix
                     * (m[3],m[4],m[5] @ +0x12C/+0x130/+0x134) — matching the
                     * actual wheel render (mat3x4 out[1]) and orig
                     * ConvertFloatVec3ToShortAngles (0x0042E2E0: Y = v·M[+0xC,+0x10,+0x14]).
                     * Previously read COL 1 (m[1],m[4],m[7]) → transposed,
                     * wrong gap magnitudes. [FIX 2026-05-27 PM-7] */
                    float m3f = *(float *)(ab + 0x12C);
                    float m4f = *(float *)(ab + 0x130);
                    float m5f = *(float *)(ab + 0x134);
                    int g[4];
                    for (int wq = 0; wq < 4; wq++) {
                        int16_t wxx = *(int16_t *)(ab + 0x210 + wq*8 + 0);
                        int16_t wyy = *(int16_t *)(ab + 0x210 + wq*8 + 2);
                        int16_t wzz = *(int16_t *)(ab + 0x210 + wq*8 + 4);
                        float roty = m3f*(float)wxx + m4f*(float)wyy + m5f*(float)wzz;
                        float ry   = wpy + roty;
                        float gy   = (float)(*(int32_t *)(ab + 0xF0 + wq*12 + 4)) / 256.0f;
                        g[wq] = (int)(ry - gy);
                    }
                    TD5_LOG_I(LOG_TAG,
                        "WHEELGAP span=%d wda=[%d,%d,%d,%d] gap(float+) FL=%d FR=%d RL=%d RR=%d",
                        (int)actor->track_span_raw,
                        (int)*(int16_t *)(ab+0x210+2), (int)*(int16_t *)(ab+0x210+8+2),
                        (int)*(int16_t *)(ab+0x210+16+2), (int)*(int16_t *)(ab+0x210+24+2),
                        g[0], g[1], g[2], g[3]);
                }
            }

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

            /* [FIX 2026-06-21] Descent-launch damp for AI OPPONENTS, applied AFTER
             * the snap that injects their vertical velocity (the player is damped
             * in td5_physics_update_suspension_response and is skipped here, so it
             * is never double-processed). Same human-player test as the dispatch
             * (td5_physics.c:2717); traffic uses a separate integrator. Runs inside
             * `contact_count > 0`, i.e. only while grounded. See descent_damp_apply(). */
            if (!(actor->slot_index < g_traffic_slot_base &&
                  g_race_slot_state[actor->slot_index] == 1)) {
                descent_damp_apply(actor);
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

        /* Float-precision matrix build (E1 fix — see line ~4361).
         * 2026-05-15: switched to LUT-style float trig to preserve
         * sub-Q12 magnitudes for near-zero angles (see line ~4625). */
        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        /* Float32 spills (slots F/G/H/I from orig 0x42E1E0). See
         * TD5_F32_SPILL at file top for rationale. */
        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
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

    /* TODO (D3+D4 — wheel-attitude switch + 2nd matrix rebuild):
     * Original 0x00406459-0x004064E1:
     *   switch (actor->wheel_contact_bitmask @ +0x37C) {   // NEW mask, NOT +0x37D
     *     case 0,1,2,4,6,8,9: TransformTrackVertexByMatrix  (0x00446030); writes back roll + pitch
     *     case 5,10:          TransformTrackVertexByMatrixB (0x00446140); writes back roll only
     *     case 3,12:          TransformTrackVertexByMatrixC (0x004461C0); writes back pitch only
     *     case 7,11,default:  no-op
     *   }
     *   euler_accum_roll  = display_angle_roll  << 8   (if A or B branch)
     *   euler_accum_pitch = display_angle_pitch << 8   (if A or C branch)
     *   BuildRotationMatrixFromAngles(rotation_matrix, display_angles)   // 2nd build
     *
     * NOTE 2026-05-25 audit: original switches on +0x37C (wheel_contact_bitmask,
     * the NEW airborne mask just written by refresh_wheel_contacts), NOT on
     * +0x37D (damage_lockout/OLD mask). Verified via Ghidra listing at
     * 0x0040645B: `MOV AL, byte ptr [ESI + 0x37c]`. The "damage_lockout"
     * name in earlier TODOs was a misread. Case values map to the airborne
     * bitmask: case 5 = FL+RL airborne (LEFT-side off) → roll-only solver;
     * case 3 = FL+FR airborne (FRONT axle off) → pitch-only solver; etc.
     *
     * The Transform* helpers derive wheel-implied roll/pitch from the post-
     * impulse wheel_contact_pos + wheel_suspension_pos, allowing the next
     * physics tick to start integration at the wheel-attitude. Without this,
     * after a collision the chassis pose remains at the impulse-applied
     * attitude even when the wheels disagree with it.
     *
     * IMPORTANT: orig 0x004063A0 (this function) is ONLY called from
     * FUN_004079c0 (V2V vehicle-to-vehicle collision response, callers
     * verified 2026-05-25 via ghidra function_callers). It is NOT per-tick.
     * The per-tick attitude rebuild lives in IntegrateVehiclePoseAndContacts
     * (orig 0x00405E80 → port td5_physics_integrate_pose at td5_physics.c
     * line ~6128) and that path's switch dispatch IS already implemented at
     * port lines 6402-6469 with the SAME case-set + helpers. So this missing
     * dispatch only affects POST-V2V-COLLISION attitude rebuild — without it,
     * after two cars touch, both chassis snap to the impulse-applied yaw
     * but retain their pre-impulse roll/pitch until the next 0x00405E80 tick
     * rebuilds them.
     *
     * Helper status (2026-05-25): A (full solver, 0x00446030) is shipped as
     * td5_physics_attitude_from_wheels (td5_physics.c near line 6054). B
     * (0x00446140) and C (0x004461C0) are ported as pure infrastructure at
     * the bottom of this file — see td5_physics_transform_track_vertex_by_matrix_b
     * and td5_physics_transform_track_vertex_by_matrix_c. AngleFromVector12
     * (0x0040A720) was already ported in the precise-trig worktree; FSQRT-
     * rounding mirrors orig FILD/FSQRT/__ftol via (int32_t)sqrtf((float)...).
     *
     * The switch dispatch + 2nd-rebuild wire-up remains DEFERRED pending
     * pool13 dynamic-diff validation. RULED OUT 2026-05-25 as candidate
     * for Jarash bouncing / not-touching-ground / yaw-rotation symptoms on
     * curved terrain — those occur during normal driving (no V2V impulse),
     * so this V2V-only dispatch is inert in that scenario. See companion
     * Frida probe tools/frida_jarash_pose_probe.js for paired-diff capture
     * to localize the actual culprit (candidate: DA-T2 case 1 always-advance
     * walker bug at td5_track.c:2619, or wheel-walker single_step on slope
     * onsets). HIGH-risk per-tick attitude change still applies — to be
     * applied in a follow-up precise-port worktree (precise-00446030). */
    /* The helpers themselves are defined at the bottom of this file with
     * __attribute__((unused)) to suppress the unused-function warning until
     * the switch dispatch lands. [PORT 2026-05-25 damage-lockout-helpers] */

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
     * [D4 — precise-port pilot 004063A0, 2026-05-14]
     *
     * 2026-05-22: switched from cos_fixed12*(1/4096) to CosFloat12bit + the
     * TD5_F32_SPILL float32 intermediate spill so both BuildRotationMatrix-
     * FromAngles call sites in 0x4063A0 produce identical byte-faithful
     * matrices. Previously this 2nd rebuild OVERWROTE the first with the
     * older Q12-truncating trig path, partially undoing the precision fix
     * in the first rebuild. [session_02 follow-up, 2026-05-22] */
    {
        int32_t roll_a  = actor->display_angles.roll  & 0xFFF;
        int32_t yaw_a   = actor->display_angles.yaw   & 0xFFF;
        int32_t pitch_a = actor->display_angles.pitch  & 0xFFF;

        float cr = CosFloat12bit((unsigned int)roll_a);
        float sr = SinFloat12bit(roll_a);
        float cy = CosFloat12bit((unsigned int)yaw_a);
        float sy = SinFloat12bit(yaw_a);
        float cp = CosFloat12bit((unsigned int)pitch_a);
        float sp = SinFloat12bit(pitch_a);

        float sp_sy_f32 = TD5_F32_SPILL(sp * sy);
        float cp_cy_f32 = TD5_F32_SPILL(cp * cy);
        float cp_sy_f32 = TD5_F32_SPILL(cp * sy);
        float sp_cy_f32 = TD5_F32_SPILL(sp * cy);

        actor->rotation_matrix.m[0] = sp_sy_f32 * sr + cp_cy_f32;
        actor->rotation_matrix.m[1] = cp_sy_f32 * sr - sp_cy_f32;
        actor->rotation_matrix.m[2] = sy * cr;
        actor->rotation_matrix.m[3] = sp * cr;
        actor->rotation_matrix.m[4] = cp * cr;
        actor->rotation_matrix.m[5] = -sr;
        actor->rotation_matrix.m[6] = sp_cy_f32 * sr - cp_sy_f32;
        actor->rotation_matrix.m[7] = cp_cy_f32 * sr + sp_sy_f32;
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

/* Match orig's FISTP rounding behavior in TransformShortVec3ByRenderMatrixRounded
 * @ 0x0042EB10. Orig uses raw FISTP with the default x87 control word
 * (RC=00 = round-to-nearest-even), NOT round-toward-negative-infinity.
 *
 * Pre-2026-05-16: this used `floorf` to be "rounding-mode independent" but
 * that's actively wrong -- floorf is RTNI while orig is RNE. For values
 * near half-integers (e.g. 0.5, -0.5, 1.5) the two differ by 1 LSB, which
 * after the post-transform `<< 8` pre-scale shows up as ±256 in the
 * resulting fp8 coords. Agent T3 (Round 3 Wave 2 audit) confirmed via
 * Ghidra static disasm of 0x0042EB10 that orig uses raw FISTP, not any
 * RC-clamping prologue.
 *
 * lrintf uses the current FPU rounding mode, which on x87 defaults to
 * round-to-nearest-even. TD5_d3d.exe never changes the FPU control word,
 * so RNE is always in effect for FISTP -- and the port's lrintf inherits
 * that same RNE by default. */
static inline int32_t td5_round_toward_neg_inf_to_int32(float x) {
    return (int32_t)lrintf(x);
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

    float *rot = actor->rotation_matrix.m;
    int resolved_surface = actor->surface_type_chassis;
    int resolved_surface_valid = 0;

    /* [DIAGNOSTIC 2026-05-27 — lateral-leak unit test, slot 0] On a purely
     * LONGITUDINAL slope, two ground samples displaced purely LATERALLY
     * (perpendicular to heading) must have EQUAL height. If the right sample
     * reads higher, the per-lane height interpolation is leaking the slope
     * into a sideways roll (the confirmed right-side-lift bug). We also sample
     * fore/aft so the lateral gradient (R-L) can be compared to the real
     * slope (A-B). Driven by the AI harness; read LATLEAK lines in race.log. */
    if (actor->slot_index == 0) {
        static int s_latleak_div = 0;
        if ((s_latleak_div++ % 30) == 0) {
            int yaw = (int)actor->display_angles.yaw & 0xFFF;
            float fwx = SinFloat12bit(yaw), fwz = CosFloat12bit(yaw); /* forward XZ */
            float ltx = fwz, ltz = -fwx;                              /* lateral XZ */
            int cx = actor->world_pos.x, cz = actor->world_pos.z;
            int sp = actor->track_span_raw;
            int d  = 50000;
            int yC = 0, yL = 0, yR = 0, yA = 0, yB = 0, st = 0;
            td5_track_probe_height(cx, cz, sp, &yC, &st);
            td5_track_probe_height(cx + (int)(d * ltx), cz + (int)(d * ltz), sp, &yR, &st);
            td5_track_probe_height(cx - (int)(d * ltx), cz - (int)(d * ltz), sp, &yL, &st);
            td5_track_probe_height(cx + (int)(d * fwx), cz + (int)(d * fwz), sp, &yA, &st);
            td5_track_probe_height(cx - (int)(d * fwx), cz - (int)(d * fwz), sp, &yB, &st);
            /* dRoll = display_angles.roll FIELD = front-rear-derived = the
             * car's PITCH (should track the surface slope A-B). dPitch =
             * display_angles.pitch FIELD = left-right-derived = the car's
             * ROLL (should track the lateral R-L). up.x/up.z = world up-axis
             * tilt. If slope is big but dRoll≈0 → car not following slope. */
            TD5_LOG_I(LOG_TAG,
                "LATLEAK span=%d yaw=%d slope=%d latRL=%d | car pitch(dRoll)=%d roll(dPitch)=%d | up.x=%.3f up.y=%.3f up.z=%.3f | FRwy=%d %d %d %d",
                sp, yaw, yA - yB, yR - yL,
                (int)actor->display_angles.roll, (int)actor->display_angles.pitch,
                actor->rotation_matrix.m[1], actor->rotation_matrix.m[4], actor->rotation_matrix.m[7],
                actor->wheel_contact_pos[0].y, actor->wheel_contact_pos[1].y,
                actor->wheel_contact_pos[2].y, actor->wheel_contact_pos[3].y);
        }
    }

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
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) slot = 0;

    /* Per-wheel contact frame computation */
    for (int i = 0; i < 4; i++) {
        /* Get wheel display angle data.
         * [2026-05-27] body-Y is read from the CARDEF directly, NOT from
         * wheel_display_angles[i][1]. That field is now rewritten every tick to
         * the suspension-adjusted RENDER value (cwy - sp_div) in
         * integrate_pose's chassis loop (matching orig 0x00405E80's psVar10
         * write), so reading it back here and subtracting sp_div again would
         * DOUBLE-count the spring and run away. The orig refresh (0x00403720)
         * reads the raw cardef wheel Y. X,Z stay static cardef (= the init
         * wheel_display_angles[0]/[2], unchanged). */
        int32_t wx = actor->wheel_display_angles[i][0];
        int32_t wy = (int32_t)CDEF_S(actor, CDEF_WHEEL_Y_BASE + i * 8);
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
        int32_t href = (int32_t)CDEF_S(actor, CDEF_SUSP_REF_HEIGHT);
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
         * Uses RNE (round-to-nearest-even) to match orig FISTP — see
         * td5_round_toward_neg_inf_to_int32 helper at line ~6740 and
         * memory/todo_chassis_snap_fix_2026-05-16.md. */
        {
            float by_tmp = (float)(int16_t)wy * rot[4]
                         + (float)(int16_t)wx * rot[3]
                         + (float)(int16_t)wz * rot[5];
            s_wheel_offset_y_world[slot & 0x0F][i] =
                (int32_t)(int16_t)lrintf(by_tmp) * 256;
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

        /* [FIX 2026-05-27 — slope-induced false ROLL] On slopes the RIGHT
         * wheels (FR=probe1, RR=probe3) consistently over-walk one span
         * FORWARD of their LEFT partners (FL=probe0, RL=probe2). Because the
         * right wheels then sample the ground a span further along the slope,
         * an UPHILL reads right-side-high (car lifts on the right) and a
         * DOWNHILL reads right-side-low (car lifts on the left). At a dead
         * stop on a slope this is a static ~9° roll (player overlay: SPD 0,
         * ROLL 9.1). The original keeps each axle's left/right wheels on the
         * SAME span, so the slope only ever produces PITCH, never a fake roll
         * (orig L-R wheel delta is balanced; port is right-biased by ~2400fp).
         *
         * Snap each axle's right wheel onto its left partner's span. The
         * wheel keeps its own lateral sub_lane, so genuine road camber is
         * still sampled; the front-vs-rear span gap (true pitch) is untouched.
         * Guard ±2 leaves real junction/branch transitions alone. Replaces an
         * earlier chassis-clamp that only halved the split and risked
         * flattening pitch. Processed in wheel order 0,1,2,3 so the left
         * partner (i-1) is already walked. */
        /* [FIX 2026-05-31 sub-lane-shift bounce] Axle right-wheel span-snap
         * DISABLED. It papered over the slope-roll by forcing FR/RR onto the
         * left partner's span, but at sub-lane-count transitions it toggles the
         * right wheel's reference span on/off, producing the user-reported
         * vertical bounce. The faithful 0x00403720 path walks each wheel
         * independently and reads height from the walker's carried
         * span+sub_lane (see the contact-height call below). */

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

        /* Ground height + surface normal at the wheel, using the BEST-FIT
         * (geometrically containing) lane within the wheel's span — NOT the
         * walker's stored sub_lane.
         *
         * [FIX 2026-05-27 — right-side-lift root cause] The per-wheel walker's
         * stored sub_lane lands on the WRONG lane on slopes (the right wheels
         * systematically), so the wheel reads a height a lane over — leaking
         * the longitudinal slope into a sideways roll (uphill→right lifts,
         * downhill→left lifts, ~9° even at a dead stop). Proven by a lateral-
         * leak unit test: sampling the surface at the containing lane is
         * balanced (R-L≈0 vs slope up to 21000), but the walked sub_lane is
         * right-biased ~6000fp (96% of rows). The original's walker resolves
         * the containing lane, so best-fit matches its result. The wheel keeps
         * its own world XZ, so genuine road camber is still sampled; only the
         * lane SELECTION is corrected.
         *
         * Also retrieves the span surface normal (orig FUN_00445A70 → actor+
         * 0x250+i*8, the "wcv" consumed by UpdateVehicleSuspensionResponse). */
        /* [FIX 2026-05-31] The BEST-FIT-lane rationale in the comment above is
         * SUPERSEDED. Per-tick lane re-selection (bestlane) made best_lane jump
         * discontinuously across sub-lane-count transitions, popping ground_y
         * (the user-reported bounce). We now use the walker's carried sub_lane,
         * faithful to 0x00403720 -> 0x00445450. If the slope-roll (2026-05-27)
         * returns, the real bug is the walker's sub_lane carry in
         * resolve_neighbor / update_position_recursive, not the height call. */
        int16_t span_normal[3] = {0, 4096, 0};  /* default: flat upward normal (magnitude 4096) */
        {
            ground_y = td5_track_compute_contact_height_bounded(
                probe_span,
                (int)actor->wheel_probes[i].sub_lane_index,
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

        /* [FIX 2026-05-28 — fast-tilt out-of-bounds launch] Did the height probe
         * just cap an upward out-of-quad extrapolation? If so this wheel is over
         * a fictional plane extrapolated above its (mis-assigned) span — it must
         * NOT be allowed to read "grounded", or the chassis Y-snap ratchets the
         * car upward off-track. Force it airborne in the contact test below so
         * the car falls under gravity instead, matching the original. */
        int wheel_capped = td5_track_last_contact_was_capped();

        /* [#20 2026-06-17] The old "S18" chassis-span ground reject lived here —
         * a TD6-only block that, on the false "displaced branch" premise, discarded
         * the chassis-span ground when a wheel probed a different span (and pulled
         * the car branch->main = the "teleport to the left track"). Branch geometry
         * is actually CONNECTED (verified; see re/tools/connect_branches.py),
         * so TD6 uses the native path with no special wheel-ground override — byte-
         * identical to faithful TD5. Removed once the faithful path was confirmed. */

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
         * Using post-snap (ground_y) as old causes huge Y deltas (~90k).
         *
         * 2026-05-22: the pragmatic `g_td5.paused -> zero gap_270` gate that
         * used to live here was removed once the rotation_matrix FPU spill
         * fix landed (orig 0x42E1E0 byte-faithful). If pause-frame gap_270
         * starts producing phantom impulses again, the gate's history is in
         * git (search for "countdown guard" in `td5_physics.c`). */
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
         * [CONFIRMED @ 0x403720 — piVar8 = local_30]
         *
         * `wheel_capped` (fast-tilt OOB fix) forces airborne regardless of force:
         * the probed ground was a capped upward extrapolation (fictional), so the
         * wheel is genuinely off-track even if its capped height happens to sit
         * near the chassis. */
        if (force > 0x800 || wheel_capped) {
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

                /* [S18 FIX — TD6 branch-fork probe teleport, shadow corners]
                 * Same root as the wheel-contact guard above, applied to the
                 * SHADOW body corners (probe_FL/FR/RL/RR). A body-corner probe
                 * can follow a branch sentinel's link_prev to a distant span and
                 * read a wild-high ground; that stretches the shadow decal right
                 * at the fork (user report: "the shadow distorts where the car
                 * used to jump"). Re-probe the ground from the CHASSIS span at
                 * the corner's XZ and, if the corner-span ground is a large
                 * outlier (> 256 world units) and the reference did not cap, use
                 * the chassis-span ground instead. TD6-only; faithful TD5 tracks
                 * are byte-identical. */
                if (g_active_td6_level > 0) {
                    int chassis_span = (int)actor->track_span_raw;
                    if (chassis_span >= 0 && chassis_span < max_sp &&
                        chassis_span != probe_span) {
                        int32_t ground_ref = td5_track_compute_contact_height_with_normal(
                            chassis_span, probe_lane,
                            body_pos[i]->x, body_pos[i]->z, NULL);
                        int ref_capped = td5_track_last_contact_was_capped();
                        int32_t diff = body_pos[i]->y - ground_ref;
                        if (!ref_capped && (diff > 0x10000 || diff < -0x10000))
                            body_pos[i]->y = ground_ref;
                    }
                }
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
    int32_t iVar1 = td5_angle12_signed(raw_roll);   /* signed roll  */
    int32_t iVar2 = td5_angle12_signed(raw_pitch);  /* signed pitch */

    /* === Dispatch on collisions flag [0x00405B86-B88] === */
    if (g_collisions_enabled != 0) {
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
            return;
        }
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
    int within_limits = (iVar1 >= -0x355 && iVar1 <= 0x355 &&
                         iVar2 >= -0x3A4 && iVar2 <= 0x3A4);

    /* [S18 FIX — TD6-scoped recovery-latch debounce]
     * Bug: on the migrated TD6 tracks the car "goes vertical and instantly
     * triggers recovery" on down slopes. Root analysis (S18):
     *   - The TD6 collision geometry is NOT anomalously steep: measured road
     *     slopes peak ~21deg (Rome/London), with banking/curvature comparable
     *     to stock Newcastle. A 21deg slope is pitch ~0xE3, far below the 0x3A4
     *     (82deg) recovery threshold — so the limit is only reached by a
     *     TRANSIENT overshoot, not by the terrain itself.
     *   - The MODE-0 recovery latch above is byte-faithful, but the port carries
     *     a known suspension equilibrium drift (see the note above: this branch
     *     was historically DISABLED in the port for exactly this reason, then
     *     re-enabled for faithfulness with the drift fix left "out of scope").
     *     On a slope that drift transiently spikes pitch/roll past the limit for
     *     a frame or two and the immediate latch yanks the car into recovery.
     * Mitigation: on TD6 tracks ONLY (g_active_td6_level > 0), require the
     * over-limit condition to PERSIST for a few consecutive ticks before
     * latching recovery. A genuinely flipped car stays over-limit and still
     * recovers a few ticks later (~0.27s, imperceptible); a transient drift
     * spike clears before the debounce elapses, so the car keeps driving.
     * Faithful TD5 tracks (g_active_td6_level == 0) are byte-IDENTICAL: the
     * debounce block is skipped entirely and the immediate latch is preserved.
     *
     * NOTE: this addresses the SYMPTOM (the spurious instant yank). The deeper
     * root is the port suspension drift; the over-limit log below records each
     * suppressed spike so the exact spiral can be captured on a real drive and
     * the drift fixed at source in a follow-up. */
    if (g_active_td6_level > 0) {
        int dbi = (int)actor->slot_index;
        if (dbi < 0 || dbi >= TD5_MAX_TOTAL_ACTORS) dbi = 0;
        if (!within_limits) {
            if (s_td6_recovery_debounce[dbi] < 255)
                s_td6_recovery_debounce[dbi]++;
            TD5_LOG_I(LOG_TAG,
                "S18 TD6 attitude over-limit slot=%d roll=%d pitch=%d ticks=%d "
                "span_raw=%d wcb=0x%02x (latch when ticks>=%d)",
                (int)actor->slot_index, (int)iVar1, (int)iVar2,
                (int)s_td6_recovery_debounce[dbi],
                (int)*(int16_t *)((uint8_t *)actor + 0x80),
                (int)actor->wheel_contact_bitmask,
                S18_TD6_RECOVERY_DEBOUNCE_TICKS);
            if (s_td6_recovery_debounce[dbi] < S18_TD6_RECOVERY_DEBOUNCE_TICKS)
                within_limits = 1;   /* not yet persistent — suppress recovery */
        } else {
            s_td6_recovery_debounce[dbi] = 0;  /* settled — reset */
        }
    }

    if (within_limits) {
        return;
    }

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
    /* [S18] Event log at the recovery latch: records the attitude (roll/pitch)
     * that tripped it and the span/collision context. Fires only when recovery
     * actually engages (a rare, session-level event, NOT a per-tick path), so
     * the cost is negligible. This is the capture the user needs when driving a
     * down slope: it pins down whether pitch/roll genuinely reach the 0x3A4
     * (~82deg) / 0x355 limit and on which span, so the residual port suspension
     * drift behind the TD6 down-slope blow-up can be fixed at source. */
    if (actor->vehicle_mode != 1) {
        TD5_LOG_I(LOG_TAG,
            "S18 RECOVERY LATCH: slot=%d roll=%d pitch=%d (lim roll=0x355 pitch=0x3A4) "
            "span_raw=%d span_norm=%d collflag=%d td6=%d",
            (int)actor->slot_index, (int)iVar1, (int)iVar2,
            (int)*(int16_t *)((uint8_t *)actor + 0x80),
            (int)*(int16_t *)((uint8_t *)actor + 0x82),
            (int)g_collisions_enabled, (int)g_active_td6_level);

        /* [task #12] Damp the RETAINED linear velocity on entering roll
         * recovery. The scripted recovery integrator (integrate_scripted_motion)
         * only sheds ~1/256 of linear velocity per tick, so a car that was
         * travelling fast when it rolled past ~90deg keeps almost all of that
         * momentum through the whole ~59-frame animation and then "accelerates
         * violently in the direction it was travelling at the moment it rolled".
         * Shed most of it here, ONCE, at the 0->1 latch transition (this branch
         * runs exactly when vehicle_mode flips into recovery).
         *
         * Knob TD5RE_ROLL_RECOVER_DAMP = retained fraction in [0,1] (default
         * 0.15 = strong damp, keep 15%). "1" = retain all (old byte-faithful
         * carry-through); "0" = kill all retained linear velocity. Y is left
         * to the integrator's gravity so the car still settles onto the road. */
        {
            static int   s_rrd_init = 0;
            static float s_rrd_keep = 0.15f;
            if (!s_rrd_init) {
                const char *e = getenv("TD5RE_ROLL_RECOVER_DAMP");
                if (e && e[0]) {
                    s_rrd_keep = (float)atof(e);
                    if (s_rrd_keep < 0.0f) s_rrd_keep = 0.0f;
                    if (s_rrd_keep > 1.0f) s_rrd_keep = 1.0f;
                }
                s_rrd_init = 1;
                TD5_LOG_I(LOG_TAG, "roll_recover_damp: TD5RE_ROLL_RECOVER_DAMP=%.3f (retained linvel fraction)",
                          s_rrd_keep);
            }
            if (s_rrd_keep < 1.0f && !recovery_gentle_for_actor(actor)) {
                /* [GENTLE FLIP-RECOVERY 2026-06-21] When the gentle coast owns
                 * this player's recovery it needs the car's REAL momentum to
                 * "continue the motion" (and eases it down itself), so skip the
                 * latch-time slash here. The faithful path still slashes. */
                actor->linear_velocity_x = (int32_t)((float)actor->linear_velocity_x * s_rrd_keep);
                actor->linear_velocity_z = (int32_t)((float)actor->linear_velocity_z * s_rrd_keep);
            }
        }
    }
    actor->vehicle_mode = 1;
    actor->frame_counter = 0;

    /* Pilot precise-port trace — emit row at exit. */
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

}

/* ========================================================================
 * Engine & Transmission
 * ======================================================================== */

/* [CONFIRMED @ 0x0042ED50] Byte-faithful with orig UpdateVehicleEngineSpeedSmoothed.
 * L5 promotion 2026-05-18 (small-tier sweep). Line-for-line listing port;
 * brake/neg-throttle → idle 400; else target = (redline-400)*throttle>>8 + 400,
 * SAR-4 slew (up clamp 400, dead down clamp 200), upper redline clamp omitted.
 *
 * --- UpdateVehicleEngineSpeedSmoothed (0x0042ED50) ---
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
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);

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

/* [CONFIRMED @ 0x0042EDF0] Byte-faithful with orig UpdateEngineSpeedAccumulator.
 * L5 promotion 2026-05-18 (small-tier sweep). Line-for-line listing port;
 * gear==1 → UpdateVehicleEngineSpeedSmoothed; else target = abs(speed>>8) *
 * gear_ratio[gear] * 45 SAR-12 + 400; delta > 800 fast-down -200, < -800
 * fast-up +200, smooth (target-rpm) SAR-2; upper redline clamp.
 *
 * --- UpdateEngineSpeedAccumulator (0x0042EDF0) ---
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
    int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + gear * 2);

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
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
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
    int32_t up_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_UPSHIFT_BASE + (int32_t)gear_cached * 2);

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
        int32_t k = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT) * throttle * 0x1A;
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
    int32_t dn_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_DOWNSHIFT_BASE + (int32_t)gear_cached * 2);

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

    int32_t up_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_UPSHIFT_BASE + gear_cached * 2);

    if (rpm > up_thresh
        && gear_cached < 8
        && actor->longitudinal_speed > 0) {
        uint8_t new_gear = (uint8_t)(actor->current_gear + 1);
        actor->current_gear = new_gear;
        /* No drivetrain kick — on-ground only */
        return;
    }

    int32_t dn_thresh = (int32_t)PHYS_S(actor, PHYS_GEAR_DOWNSHIFT_BASE + gear_cached * 2);
    if (rpm < dn_thresh && gear_cached > TD5_GEAR_FIRST) {
        actor->current_gear = (uint8_t)(actor->current_gear - 1);
    }
}

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

/* [CONFIRMED @ 0x0042F030] Byte-faithful with orig ComputeDriveTorqueFromGearCurve.
 * L5 promotion 2026-05-18 (small-tier sweep). Piecewise-linear LUT lerp at
 * tuning+rpm_idx*2 (SAR-9 index, mod-512 fraction), * throttle SAR-8 *
 * gear_ratio SAR-8; redline-50 upper cutoff returns 0; gear==1 early-out.
 *
 * --- ComputeDriveTorqueFromGearCurve (0x42F030) ---
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
/* [gear-1 accel 2026-06-18] Speed-dependent drive-torque shaping for the HUMAN
 * player (user: "make acceleration faster at low speed and weaker at high speed,
 * mimic gear 1" — which also strengthens hill starts). Returns a Q8 multiplier:
 * BOOST at/below LOW% of the car's top speed, tapering linearly to CUT at/above
 * HIGH%. 0x100 (unchanged) when off or top speed is unknown. Player-only so AI
 * pacing is untouched. Knobs:
 *   TD5RE_GEAR1_ACCEL=0      disable (default on)
 *   TD5RE_GEAR1_BOOST_PCT    low-speed torque %  (default 150 = 1.5x)
 *   TD5RE_GEAR1_CUT_PCT      high-speed torque % (default 70  = 0.7x)
 *   TD5RE_GEAR1_LOW_PCT      boost holds below this % of top speed (default 12)
 *   TD5RE_GEAR1_HIGH_PCT     cut reached at/above this % of top speed (default 75) */
static int32_t td5_physics_gear1_accel_q8(TD5_Actor *actor)
{
    static int inited = 0, on = 1, boost = 150, cut = 70, lowp = 12, highp = 75;
    int32_t top, spd, lo, hi, bq, cq;
    if (!inited) {
        const char *e;
        inited = 1;
        e = getenv("TD5RE_GEAR1_ACCEL");     if (e && e[0] == '0') on = 0;
        e = getenv("TD5RE_GEAR1_BOOST_PCT"); if (e && e[0]) boost = atoi(e);
        e = getenv("TD5RE_GEAR1_CUT_PCT");   if (e && e[0]) cut   = atoi(e);
        e = getenv("TD5RE_GEAR1_LOW_PCT");   if (e && e[0]) lowp  = atoi(e);
        e = getenv("TD5RE_GEAR1_HIGH_PCT");  if (e && e[0]) highp = atoi(e);
        if (boost < 100) boost = 100; else if (boost > 400) boost = 400;
        if (cut   < 20)  cut   = 20;  else if (cut   > 100) cut   = 100;
        if (lowp  < 0)   lowp  = 0;   else if (lowp  > 90)  lowp  = 90;
        if (highp <= lowp) highp = lowp + 1; else if (highp > 100) highp = 100;
        TD5_LOG_I(LOG_TAG, "gear1 accel: %s boost=%d%% cut=%d%% low=%d%% high=%d%% "
                  "(TD5RE_GEAR1_*)", on ? "ON" : "OFF", boost, cut, lowp, highp);
    }
    if (!on) return 0x100;
    top = (int32_t)PHYS_S(actor, PHYS_TOP_SPEED);
    if (top <= 0) return 0x100;
    spd = actor->longitudinal_speed;
    if (spd < 0) spd = -spd;                              /* 24.8 magnitude */
    lo = (int32_t)((((int64_t)top * lowp)  / 100) << 8);  /* top is in HUD units */
    hi = (int32_t)((((int64_t)top * highp) / 100) << 8);
    bq = (boost * 0x100) / 100;
    cq = (cut   * 0x100) / 100;
    if (spd <= lo) return bq;
    if (spd >= hi || hi <= lo) return cq;
    return bq + (int32_t)(((int64_t)(cq - bq) * (spd - lo)) / (hi - lo));
}

int32_t td5_physics_compute_drive_torque(TD5_Actor *actor)
{
    /* Entry trace hook (pure-leaf function; no state to snapshot at exit). */

    int16_t *phys = get_phys(actor);
    if (!phys) {
        return 0;
    }

    uint8_t  gear_u8 = actor->current_gear;
    int32_t  gear    = (int32_t)gear_u8;

    /* Neutral (gear == 1) — original CMP BL,0x1 / JZ RET_ZERO at 0x0042F03B-45. */
    if (gear_u8 == 0x01) {
        return 0;
    }

    int32_t rpm = actor->engine_speed_accum;

    /* index = sar9_rz(rpm) — listing 0x0042F04B-58 (CDQ/AND 0x1FF/ADD/SAR 9).
     * NO bounds clamp (original reads LUT[index] and LUT[index+1] freely). */
    int32_t index = sar9_rz_42F030(rpm);

    int32_t torque_mult = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);

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
    int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + (int32_t)(uint8_t)gear_u8 * 2);
    torque = sar8_rz_42F030(torque * gear_ratio);

    /* Redline cutoff: original computes the full pipeline then CMPs EBP (raw
     * engine_speed) against ECX (redline-50). JLE keep result; else XOR=0.
     * The compare is signed (JLE), so rpm > redline-50 → return 0. */
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);
    if (rpm > redline - 50) {
        return 0;
    }

    /* [MP CATCHUP 2026-06-14] Multiplayer rubber-band assist: in a 2+-human
     * race, scale a HUMAN player's longitudinal drive force by the per-slot
     * catch-up multiplier (Q8, 0x100 = 1.0). Behind the leader -> >1.0 (catch
     * up); leader (when enabled) -> <1.0 (ease off). The multiplier is 1.0 for
     * AI/traffic, for non-racer slots, and whenever the feature is off or there
     * is <2 humans, so this is a no-op in single-player and faithful play.
     *
     * Applied here (the single drive-torque chokepoint feeding all three player
     * drive paths: on-ground / airborne / reverse) so the assist is consistent
     * and smooth. It only scales ACCELERATION — top speed (the speed_limit gate
     * in the callers) is untouched, so it cannot warp a car past the field; it
     * just lets a trailing player build speed harder. The shift uses the same
     * biased-toward-zero signed >>8 idiom as the rest of this pipeline.
     *
     * DETERMINISM: gated on g_race_slot_state[] + s_mp_catchup_mult[], both of
     * which are pure replicated sim state recomputed once per tick — identical
     * on every lockstep client, so torque stays bit-identical across machines. */
    if (actor->slot_index < g_traffic_slot_base &&
        actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] == 1) {
        int32_t mult = td5_physics_mp_catchup_mult(actor->slot_index);
        if (mult != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mult;
            /* signed /256, biased toward zero (matches sar8_rz_42F030 etc.) */
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [HARD CATCHUP item #13] Hard-difficulty AI catch-up: the complement of the
     * MP block above — scale an AI OPPONENT's drive force up when it is behind
     * the human player on Hard, so the field presses harder. Applies ONLY to AI
     * racer slots (non-human, slot < g_traffic_slot_base); humans are handled
     * above and traffic/non-racer slots return 1.0. The multiplier is 1.0 (no-op)
     * unless TD5RE_HARD_CATCHUP=1 AND g_difficulty_hard, so non-hard / easy /
     * normal play is byte-unchanged. Acceleration only (top-speed gate in the
     * callers untouched). Same biased-toward-zero signed >>8 idiom.
     *
     * DETERMINISM: gated on g_race_slot_state[] + g_difficulty_hard +
     * s_hard_catchup_mult[], all replicated sim state recomputed once per tick. */
    if (actor->slot_index >= 0 &&
        actor->slot_index < g_traffic_slot_base &&
        actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] != 1) {
        int32_t mult = td5_physics_hard_catchup_mult(actor->slot_index);
        if (mult != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mult;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [MANUAL BOOST #2] Acceleration half: a car in MANUAL gearbox mode (byte
     * +0x378 == 0) gets +N% drive torque (default +20%). Applies to ALL cars
     * (player / AI / traffic) since it keys only on the per-actor gearbox byte.
     * The top-speed half lives at the speed_limit gates in the callers. 1.0 (no
     * boost) for automatic cars or when the knob is off, so they are unchanged.
     * Same biased-toward-zero signed >>8 idiom as the catch-up blocks above. */
    {
        int32_t mboost = td5_physics_actor_manual_boost_q8(actor);
        if (mboost != MP_CATCHUP_Q8_ONE) {
            int64_t scaled = (int64_t)torque * (int64_t)mboost;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    /* [gear-1 accel #2026-06-18] HUMAN-player-only speed shaping: strong low-end,
     * tapered top-end (mimics gear 1; also helps hill starts). Composes after the
     * catch-up/manual multipliers. g_race_slot_state==1 is the human-driven slot
     * (AI racers / traffic are untouched, preserving pacing). */
    if (actor->slot_index >= 0 && actor->slot_index < TD5_MAX_RACER_SLOTS &&
        g_race_slot_state[actor->slot_index] == 1) {
        int32_t g1 = td5_physics_gear1_accel_q8(actor);
        if (g1 != 0x100) {
            int64_t scaled = (int64_t)torque * (int64_t)g1;
            torque = (int32_t)((scaled + ((scaled >> 63) & 0xFF)) >> 8);
        }
    }

    return torque;
}

/* [CONFIRMED @ 0x0042EEA0] Byte-faithful with orig ApplySteeringTorqueToWheels.
 * L5 promotion 2026-05-18 (small-tier sweep). Verbatim port of 24-instr listing;
 * throttle * tuning[+0x68] * 26, biased SAR 8, * g_gearTorqueTable[gear],
 * biased SAR 8 → kick written FL/FR (+), RL/RR (-) on wheel_spring_dv[0..3].
 *
 * --- ApplySteeringTorqueToWheels (0x42EEA0) ---  [byte-faithful port]
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
    int32_t torque_mult = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);            /* tuning +0x68 s16 */

    int32_t k = throttle * torque_mult * 0x1A;
    k = (k + ((k >> 31) & 0xff)) >> 8;
    k = k * g_gear_torque_table[(uint8_t)actor->current_gear];     /* +0x36B u8 */
    k = (k + ((k >> 31) & 0xff)) >> 8;

    actor->wheel_spring_dv[0] += k;   /* +0x2EC FL */
    actor->wheel_spring_dv[1] += k;   /* +0x2F0 FR */
    actor->wheel_spring_dv[2] -= k;   /* +0x2F4 RL */
    actor->wheel_spring_dv[3] -= k;   /* +0x2F8 RR */
}

/* [CONFIRMED @ 0x0042F010] Byte-faithful with orig ApplyReverseGearThrottleSign.
 * L5 promotion 2026-05-18 (small-tier sweep). 8-instr listing match;
 * actor+0x36B (gear) zero-gate, 16-bit NEG on actor+0x33E (encounter_steering_cmd).
 *
 * --- ApplyReverseGearThrottleSign (0x42F010) ---
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
    int32_t redline = (int32_t)PHYS_S(actor, PHYS_REDLINE_RPM);          /* MOVSX from [+0x72] */
    int32_t step    = 200;                                    /* MOV EBP,0xC8 */

    /* --- Encode pseudo-speed (iVar5 / ESI) --- */
    int32_t iVar5;
    if (gear == 1) {
        /* 0x00403CAF JNZ skip → XOR ESI,ESI ; JMP 0x00403CE4.
         * Neutral SKIPS the encode but does NOT early-return; engine slew
         * still runs through the cold branch below. */
        iVar5 = 0;
    } else {
        int32_t gear_ratio = (int32_t)PHYS_S(actor, PHYS_GEAR_RATIO_BASE + gear * 2);

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

    /* [STUCK RECOVERY 2026-06-15] Clear the per-slot manual-recovery cooldown at
     * race init so a cooldown left over from a previous race in the same session
     * can't carry over and block the first manual recovery. */
    memset(s_manual_recovery_cooldown, 0, sizeof(s_manual_recovery_cooldown));

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
                    write_i16((uint8_t *)phys, PHYS_DRIVE_TORQUE_MULT, (int16_t)((tm * 0x28A) >> 8));
                    /* 0x2C: tire_grip_coeff *= 0x17C/256 (1.48x) */
                    int32_t dc = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
                    write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)((dc * 0x17C) >> 8));
                    /* 0x6E: brake_force *= 0x1C2/256 (1.76x) */
                    int32_t bf = (int32_t)PHYS_S(actor, PHYS_BRAKE_FRONT);
                    write_i16((uint8_t *)phys, PHYS_BRAKE_FRONT, (int16_t)((bf * 0x1C2) >> 8));
                    /* 0x70: engine_brake *= 400/256 (1.56x) */
                    int32_t eb = (int32_t)PHYS_S(actor, PHYS_BRAKE_REAR);
                    write_i16((uint8_t *)phys, PHYS_BRAKE_REAR, (int16_t)((eb * 400) >> 8));
                    /* 0x78: speed_scale <<= 2 */
                    int32_t ss = (int32_t)PHYS_S(actor, PHYS_SPEED_SCALE);
                    write_i16((uint8_t *)phys, PHYS_SPEED_SCALE, (int16_t)(ss << 2));
                } else if (!g_difficulty_easy) {
                    /* Normal difficulty */
                    /* 0x68: drive_torque_mult *= 0x168/256 (0.5625x) */
                    int32_t tm = (int32_t)PHYS_S(actor, PHYS_DRIVE_TORQUE_MULT);
                    write_i16((uint8_t *)phys, PHYS_DRIVE_TORQUE_MULT, (int16_t)((tm * 0x168) >> 8));
                    /* 0x2C: tire_grip_coeff *= 300/256 (1.17x) */
                    int32_t dc = (int32_t)PHYS_S(actor, PHYS_TIRE_GRIP_COEFF);
                    write_i16((uint8_t *)phys, PHYS_TIRE_GRIP_COEFF, (int16_t)((dc * 300) >> 8));
                    /* 0x78: speed_scale <<= 1 */
                    int32_t ss = (int32_t)PHYS_S(actor, PHYS_SPEED_SCALE);
                    write_i16((uint8_t *)phys, PHYS_SPEED_SCALE, (int16_t)(ss << 1));
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
                        const char *e = getenv("TD5RE_TRAFFIC_WHEEL_TRACK");
                        if (e && e[0]) {
                            s_twt = (float)atof(e);
                            if (s_twt < 0.1f) s_twt = 0.1f;
                            if (s_twt > 2.0f) s_twt = 2.0f;
                        }
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
                    int ai_top = ((int)cp[0x74 / 2] * k_ai_tier_top_pct[tier]) >> 8;
                    int ai_dt  = ((int)cp[0x68 / 2] * k_ai_tier_torque_pct[tier]) >> 8;
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

/* ========================================================================
 * Player stuck-car recovery (PORT ENHANCEMENT 2026-06-15)
 * ======================================================================== */

/* Cache the recovery knobs once. TD5RE_STUCK_RECOVERY (default ON) gates manual
 * recovery; TD5RE_RECOVERY_SPANS_BACK (default 3) is how far back to drop the
 * car; TD5RE_RECOVERY_COOLDOWN_TICKS (default 150 = 5 s) is the manual cooldown.
 * Logged once like the other [PORT ENHANCEMENT] knobs. */
static void recovery_init_knobs(void)
{
    if (s_recovery_init) return;
    {
        const char *e = getenv("TD5RE_STUCK_RECOVERY");
        s_recovery_enabled = (e && e[0] == '0') ? 0 : 1;   /* default ON */
    }
    {
        const char *e = getenv("TD5RE_RECOVERY_SPANS_BACK");
        if (e && e[0]) {
            int v = atoi(e);
            if (v < 0) v = 0;
            if (v > 64) v = 64;            /* sanity clamp */
            s_recovery_spans_back = v;
        }
    }
    {
        /* Tune the manual-recovery cooldown without a rebuild. 0 = no cooldown. */
        const char *e = getenv("TD5RE_RECOVERY_COOLDOWN_TICKS");
        if (e && e[0]) {
            int v = atoi(e);
            if (v < 0)    v = 0;           /* 0 = no cooldown */
            if (v > 1800) v = 1800;        /* <= 60 s sanity clamp */
            s_recovery_cooldown_ticks = v;
        }
    }
    s_recovery_init = 1;
    TD5_LOG_I(LOG_TAG, "Manual recovery: %s spans_back=%d cooldown=%d ticks (%.1fs)",
              s_recovery_enabled ? "enabled" : "disabled",
              s_recovery_spans_back, s_recovery_cooldown_ticks,
              s_recovery_cooldown_ticks / 30.0);
}

/* Reposition `slot`'s actor a few spans back, centred, upright, heading aligned
 * to track forward. Reuses the SPAWN-pose approach: write the span fields +
 * center-lane world XZ, set yaw via td5_track_compute_heading (geometry forward
 * heading at that span), then run td5_physics_reset_actor_state which zeroes the
 * linear+angular velocities and the roll/pitch euler accumulators, ground-snaps
 * world_pos.y, and rebuilds the rotation matrix from the (now-corrected) yaw via
 * integrate_pose — exactly the sequence td5_game.c InitRace uses per racer. */
int td5_physics_recover_player(int slot)
{
    recovery_init_knobs();
    if (!s_recovery_enabled) return 0;
    if (!g_actor_table_base) return 0;
    if (slot < 0 || slot >= g_traffic_slot_base || slot >= TD5_MAX_RACER_SLOTS)
        return 0;

    TD5_Actor *actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
    if (!actor) return 0;

    int cur_span = (int)actor->track_span_raw;
    int tgt_span, sub_lane, wx, wy, wz;
    if (!td5_track_get_recovery_pose(cur_span, s_recovery_spans_back,
                                     &tgt_span, &sub_lane, &wx, &wy, &wz)) {
        TD5_LOG_W(LOG_TAG, "recover_player slot=%d: no recovery pose (cur_span=%d)",
                  slot, cur_span);
        return 0;
    }

    /* Seed the track-position block to the target span (mirrors the spawn-time
     * InitActorTrackSegmentPlacement seed: +0x80/+0x84/+0x86 = span, and the
     * clamped center sub-lane at +0x8C). +0x82 (normalized) is left to the wrap-
     * normalizer / per-tick walker, same as spawn. */
    actor->track_span_raw         = (int16_t)tgt_span;   /* +0x80 */
    actor->track_span_accumulated = (int16_t)tgt_span;   /* +0x84 */
    actor->track_span_high_water  = (int16_t)tgt_span;   /* +0x86 */
    actor->track_sub_lane_index   = (uint8_t)sub_lane;   /* +0x8C */

    /* Center-lane world XZ in 24.8 FP; Y set to the reset sentinel so
     * reset_actor_state's integrate_pose ground-snaps the car onto the surface
     * (it overwrites world_pos.y with 0xC0000000 itself, but set it here too so
     * compute_heading / any pre-reset read sees a sane chassis position). */
    actor->world_pos.x = wx;                             /* +0x1FC */
    actor->world_pos.y = (int32_t)0xC0000000;            /* +0x200 (ground-snap sentinel) */
    actor->world_pos.z = wz;                             /* +0x204 */

    /* Heading aligned to track forward at the target span. compute_heading reads
     * track_span_raw (+0x80) + sub_lane (+0x8C) just written, and writes
     * euler_accum.yaw (+0x1F4) + heading_normal (+0x290). On TD6 tracks the
     * geometry yaw lands ~90 deg off, so re-seed from the route heading exactly
     * like the spawn path does (td5_game.c). */
    td5_track_compute_heading(actor);
    if (g_active_td6_level > 0)
        td5_ai_correct_spawn_heading(slot);

    /* Zero linear+angular velocity, roll/pitch euler, settle suspension, rebuild
     * the rotation matrix + render pose, and ground-snap Y. reset_actor_state
     * preserves euler_accum.yaw (only roll/pitch are zeroed), so the corrected
     * heading above survives. */
    td5_physics_reset_actor_state(actor);

    /* reset_actor_state -> integrate_pose -> update_actor_position may have
     * walked track_span_raw across a boundary; restore it to the recovery span
     * (same fix-up the spawn loop applies after reset). */
    actor->track_span_raw = (int16_t)tgt_span;

    /* Clear control-flag residue so the car doesn't immediately re-trip a stuck
     * heuristic or carry a stale wall/handbrake/reverse latch into the next tick. */
    actor->brake_flag = 0;          /* +0x36D */
    actor->handbrake_flag = 0;      /* +0x36E */
    actor->throttle_state = 1;      /* +0x36F: forward */
    actor->track_contact_flag = 0;  /* +0x37B: V2W contact */

    TD5_LOG_I(LOG_TAG,
              "recover_player: slot=%d cur_span=%d -> span=%d lane=%d pos=(%d,%d,%d)",
              slot, cur_span, tgt_span, sub_lane,
              actor->world_pos.x, actor->world_pos.y, actor->world_pos.z);
    return 1;
}

/* Per-tick driver: run MANUAL (R / SELECT) stuck-recovery for every LOCAL HUMAN
 * player. Called from td5_physics_tick (deterministic sim tick).
 *
 * Local human players occupy viewports 0..viewport_count-1, each mapped to an
 * actor slot via g_actorSlotForView[vp]; that vp index is also the input-layer
 * player index. We only act on slots flagged human (g_race_slot_state==1).
 *
 * [AUTO-RECOVERY REMOVED 2026-06-24] There is no automatic stuck detection any
 * more — a car is only ever repositioned when the driver presses recovery, and
 * each manual recovery arms a per-player cooldown (s_recovery_cooldown_ticks,
 * default 150 = 5 s) before another will be honoured.
 *
 * v1 NETPLAY SCOPE: the manual edge is local-only input (not exchanged over the
 * lockstep protocol), so to keep remote peers in sync the whole driver is
 * skipped under an active network session. */
void td5_physics_update_stuck_recovery(void)
{
    recovery_init_knobs();
    if (!s_recovery_enabled) return;
    if (!g_actor_table_base) return;
    if (g_td5.network_active) return;   /* v1: local/non-network only */

    /* During the start countdown / pause the sim is frozen and recovering makes
     * no sense, but we still DRAIN the manual edge so a press then doesn't leak
     * forward and fire the instant the race un-pauses. */
    int act = !g_game_paused;

    int views = g_td5.viewport_count;
    if (views < 1) views = 1;
    if (views > TD5_MAX_VIEWPORTS) views = TD5_MAX_VIEWPORTS;

    for (int vp = 0; vp < views; vp++) {
        int slot = g_actorSlotForView[vp];
        if (slot < 0 || slot >= g_traffic_slot_base || slot >= TD5_MAX_RACER_SLOTS)
            continue;
        if (g_race_slot_state[slot] != 1)   /* local human racers only */
            continue;

        TD5_Actor *actor =
            (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        if (!actor) continue;

        /* ---- MANUAL trigger (edge from input layer, keyed by player index) ----
         * Always consume the edge so it never leaks across a pause OR a cooldown;
         * only act on it while the sim is live and the car is still racing. */
        int manual = td5_input_recovery_requested(vp);

        /* Don't recover a car that has already finished the race. */
        if (actor->finish_time != 0) {
            s_manual_recovery_cooldown[slot] = 0;
            continue;
        }

        if (!act) {
            /* Paused/countdown: defer recovery (edge already drained above);
             * the cooldown does not tick down while the sim is frozen. */
            continue;
        }

        /* Count the per-player cooldown down toward 0 on each live sim tick. */
        if (s_manual_recovery_cooldown[slot] > 0)
            s_manual_recovery_cooldown[slot]--;

        if (manual) {
            if (s_manual_recovery_cooldown[slot] > 0) {
                /* On cooldown — ignore this press. */
                TD5_LOG_I(LOG_TAG,
                          "manual recovery ignored (cooldown): player=%d slot=%d "
                          "remaining=%d ticks (%.1fs)",
                          vp, slot, s_manual_recovery_cooldown[slot],
                          s_manual_recovery_cooldown[slot] / 30.0);
            } else {
                TD5_LOG_I(LOG_TAG, "manual recovery: player=%d slot=%d", vp, slot);
                if (td5_physics_recover_player(slot))
                    s_manual_recovery_cooldown[slot] = s_recovery_cooldown_ticks;
            }
        }
    }
}

/* ========================================================================
 * Gentle flip-recovery (PORT ENHANCEMENT 2026-06-21)
 *
 * The byte-faithful scripted recovery (vehicle_mode==1) tumbles the car for 59
 * frames via the compounding collision_spin × saved_orientation twist plus the
 * ComputeActorWorldBoundingVolume flatspin/drag impulses, then snap-respawns in
 * place (ResetVehicleActorState @ 0x00405D70 — preserves world_pos.x/z, only
 * ground-snaps Y). Together with the PORT-ONLY crash camera shake (td5_camera
 * ITEM #12 — the ORIGINAL never shakes during recovery, Ghidra-confirmed) that
 * reads as "weird + shaky".
 *
 * For a LOCAL HUMAN player this replaces the tumble with a gentle coast: keep
 * the car's existing translation (eased down), smoothly level roll+pitch back
 * toward upright (keep yaw/heading), hold height (no sink/fall), and after the
 * coast window hand off to the SAME in-place ground-snap reset — i.e. "gentle
 * continuation of the motion, then the recovery teleport". The crash shake is
 * held off for the duration (td5_physics_recovery_shake_suppressed).
 *
 * The byte-faithful path is untouched for AI / traffic / network-replicated AI
 * slots and when the knob is off.
 *
 * Knobs:
 *   TD5RE_RECOVERY_GENTLE       (default 1)     master gate
 *   TD5RE_RECOVERY_COAST_TICKS  (default 30)    ~1s @ 30Hz before the teleport
 *   TD5RE_RECOVERY_COAST_DECAY  (default 0.92)  per-tick horizontal vel retain
 *   TD5RE_RECOVERY_LEVEL_DECAY  (default 0.80)  per-tick roll/pitch retain (->0)
 * ======================================================================== */
static int   s_rgentle_init        = 0;
static int   s_rgentle_enabled     = 1;
static int   s_rgentle_coast_ticks = 30;
static float s_rgentle_coast_decay = 0.92f;
static float s_rgentle_level_decay = 0.80f;

static void recovery_gentle_init(void)
{
    if (s_rgentle_init) return;
    s_rgentle_init = 1;
    {
        const char *e;
        if ((e = getenv("TD5RE_RECOVERY_GENTLE")) && e[0] == '0')
            s_rgentle_enabled = 0;
        if ((e = getenv("TD5RE_RECOVERY_COAST_TICKS")) && e[0]) {
            int v = atoi(e);
            if (v < 1)   v = 1;
            if (v > 240) v = 240;          /* sanity clamp (~8s max) */
            s_rgentle_coast_ticks = v;
        }
        if ((e = getenv("TD5RE_RECOVERY_COAST_DECAY")) && e[0]) {
            float f = (float)atof(e);
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            s_rgentle_coast_decay = f;
        }
        if ((e = getenv("TD5RE_RECOVERY_LEVEL_DECAY")) && e[0]) {
            float f = (float)atof(e);
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            s_rgentle_level_decay = f;
        }
    }
    TD5_LOG_I(LOG_TAG,
              "Gentle flip-recovery: %s coast_ticks=%d coast_decay=%.3f level_decay=%.3f",
              s_rgentle_enabled ? "enabled" : "disabled",
              s_rgentle_coast_ticks, s_rgentle_coast_decay, s_rgentle_level_decay);
}

static int recovery_gentle_enabled(void)
{
    recovery_gentle_init();
    return s_rgentle_enabled;
}

/* True when `actor` is a local-human racer (the only case the gentle coast
 * owns). AI, traffic and the byte-faithful path are excluded. The human gate is
 * the REPLICATED g_race_slot_state so all net peers agree on which slots take
 * the gentle path (the trigger is the deterministic attitude latch, not local
 * input — unlike the stuck-recovery feature, so no netplay skip is needed). */
static int recovery_gentle_for_actor(const TD5_Actor *actor)
{
    if (!recovery_gentle_enabled()) return 0;
    if (!actor) return 0;
    if (actor->slot_index >= g_traffic_slot_base) return 0;
    if (actor->slot_index >= TD5_MAX_RACER_SLOTS)  return 0;
    if (g_race_slot_state[actor->slot_index] != 1) return 0;   /* human racers only */
    return 1;
}

/* Public (camera) query: 1 while `slot` is a human in gentle flip-recovery, so
 * the crash-FX screen shake (a port-only addition) is held off for the duration
 * — matching the original, which never shakes during recovery. */
int td5_physics_recovery_shake_suppressed(int slot)
{
    if (!recovery_gentle_enabled()) return 0;
    if (!g_actor_table_base) return 0;
    if (slot < 0 || slot >= g_traffic_slot_base || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (g_race_slot_state[slot] != 1) return 0;
    {
        TD5_Actor *a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        return (a && a->vehicle_mode == 1) ? 1 : 0;
    }
}

/* Per-tick gentle coast for a human player in flip-recovery (vehicle_mode==1).
 * frame_counter is already incremented at the top of update_vehicle_actor; once
 * it reaches the coast window we hand off to the byte-faithful in-place reset
 * (the "recovery teleport"). No spin matrix, no flatspin probe pass, no shake. */
static void td5_physics_gentle_recovery_coast(TD5_Actor *actor)
{
    /* 1. Smoothly level roll + pitch back toward upright; keep yaw (heading).
     *    display_angles.{roll,yaw,pitch} are signed int16 holding 12-bit angles. */
    actor->display_angles.roll  = (int16_t)((float)actor->display_angles.roll  * s_rgentle_level_decay);
    actor->display_angles.pitch = (int16_t)((float)actor->display_angles.pitch * s_rgentle_level_decay);

    /* Rebuild rotation_matrix from {roll,yaw,pitch}; keep the 24.8 euler
     * accumulators and saved_orientation in sync for any downstream reader. */
    BuildRotationMatrixFromAngles(actor->rotation_matrix.m, &actor->display_angles.roll);
    actor->euler_accum.roll  = (int32_t)actor->display_angles.roll  << 8;
    actor->euler_accum.yaw   = (int32_t)actor->display_angles.yaw   << 8;
    actor->euler_accum.pitch = (int32_t)actor->display_angles.pitch << 8;
    memcpy(&actor->saved_orientation, &actor->rotation_matrix, 9 * sizeof(float));

    /* No residual spin — suppress the violent tumble. */
    actor->angular_velocity_roll  = 0;
    actor->angular_velocity_yaw   = 0;
    actor->angular_velocity_pitch = 0;

    /* 2. Gentle horizontal coast — continue the car's motion, eased down. Hold
     *    height (no gravity, no ground probe here) so it neither sinks nor
     *    free-falls during the coast; the reset ground-snaps Y precisely. */
    {
        int32_t vx = (int32_t)((float)actor->linear_velocity_x * s_rgentle_coast_decay);
        int32_t vz = (int32_t)((float)actor->linear_velocity_z * s_rgentle_coast_decay);
        actor->linear_velocity_x = vx;
        actor->linear_velocity_z = vz;
        actor->linear_velocity_y = 0;          /* freeze height */
        actor->world_pos.x += vx;
        actor->world_pos.z += vz;
    }

    /* 3. Keep chassis track position + render pose current. */
    td5_track_update_actor_position(actor);
    actor->render_pos.x = (float)actor->world_pos.x * (1.0f / 256.0f);
    actor->render_pos.y = (float)actor->world_pos.y * (1.0f / 256.0f);
    actor->render_pos.z = (float)actor->world_pos.z * (1.0f / 256.0f);

    if (actor->slot_index == 0 && ((unsigned)actor->frame_counter % 10u) == 0u) {
        TD5_LOG_I(LOG_TAG,
            "gentle_recovery_coast: slot=%d t=%u/%d roll=%d pitch=%d vx=%d vz=%d",
            (int)actor->slot_index, (unsigned)actor->frame_counter, s_rgentle_coast_ticks,
            (int)actor->display_angles.roll, (int)actor->display_angles.pitch,
            actor->linear_velocity_x, actor->linear_velocity_z);
    }

    /* 4. End of the coast window -> the recovery teleport (in-place ground-snap). */
    if ((int)actor->frame_counter >= s_rgentle_coast_ticks) {
        TD5_LOG_I(LOG_TAG,
            "gentle_recovery_coast: slot=%d teleport (gentle coast %d ticks done)",
            (int)actor->slot_index, s_rgentle_coast_ticks);
        td5_physics_reset_actor_state(actor);
    }
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
        TD5_LOG_I(LOG_TAG, "XZ freeze=%d (DAT_00483030)", freeze);
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
    int32_t bin = (int32_t)((((uint32_t)(roll_target  + 0x200) >> 2) & 0x300u)
                          | ((uint32_t)(pitch_target + 0x200) & 0xC00u)) >> 8;

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
        int32_t wx_fp = body_out[0] << 8;
        int32_t wy_fp = body_out[1] << 8;
        int32_t wz_fp = body_out[2] << 8;
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
        int32_t wx_fp = out[0] << 8;
        int32_t wy_fp = out[1] << 8;
        int32_t wz_fp = out[2] << 8;

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
                actor->linear_velocity_x -= (k * l_z) >> 8;
                actor->linear_velocity_z -= (k * l_x) >> 8;
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
            actor->linear_velocity_x -= ((int32_t)hx * pf_i) >> 8;
            actor->linear_velocity_y -= ((int32_t)hy * pf_i) >> 8;
            actor->linear_velocity_z -= ((int32_t)hz * pf_i) >> 8;
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
