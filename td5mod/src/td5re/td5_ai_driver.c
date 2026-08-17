/* ========================================================================
 * td5_ai_driver.c -- closed-loop "Driver Model" AI (PORT-ONLY)
 *
 * See td5_ai_driver.h for the design overview and the mode ladder.
 *
 * P1 SCOPE: a working closed-loop driver for a single AI racer.
 *   - PATH TABLE (per race): per-span rail midpoint, arc length and curvature,
 *     derived from the public track geometry helpers (the track is a span-
 *     quantised lane ribbon, not a spline -- curvature/length are computed).
 *   - SPEED PROFILE (per race): a per-span target expressed as a FRACTION of
 *     the car's OWN demonstrated top speed (running max of longitudinal_speed).
 *     This sidesteps the absolute fixed-point unit-calibration risk entirely:
 *     the profile is "how hard to push here vs this car's flat-out", which is
 *     exactly what matters for driving well and is unit-agnostic. A curvature
 *     slow-in cap feeds a BACKWARD braking pass so the car lifts/brakes BEFORE
 *     a corner (correct braking points) and a forward pass limits corner-exit
 *     snap. (The plan's "physically-grounded from PHYS_*" is realised here as
 *     fraction-of-own-capability; per-car cornering-grip differentiation is a
 *     later-phase refinement.)
 *   - CONTROLLER (per tick): pure-pursuit steering to a speed-scaled look-ahead
 *     aim point (reusing td5_track_laneassist_target's fork-following walk),
 *     proportional heading error + yaw-rate damping -> steering_command; a PI
 *     speed controller -> encounter_steering_cmd / brake_flag. This REPLACES
 *     the banded steering cascade (the documented zig-zag / wall-scrape cause)
 *     for driver-owned cars.
 *
 * P1 does NOT include racecraft, personalities, recovery states or the hidden
 * leash (P2..P4). All gains are env-tunable (TD5RE_AI_DRIVER_*) for calibration.
 *
 * Determinism: the tick reads only sim state and uses double math consistently
 * (as SmartAI does). No CRT rand(); per-slot seeded randomness arrives with
 * personalities in P4.
 * ======================================================================== */

#include "td5_ai_driver.h"

#include "td5_types.h"
#include "td5re.h"          /* g_td5 (ini.ai_model, drag/wanted flags) */
#include "td5_platform.h"   /* TD5_LOG_* */
#include "td5_config.h"     /* td5_env_int/float/int_opt */
#include "td5_race_state.h" /* td5_game_get_slot_state / _get_actor */
#include "td5_track.h"      /* span geometry + laneassist_target look-ahead */
#include "../../../re/include/td5_actor_struct.h"

#include <stdlib.h>
#include <math.h>

#define LOG_TAG "ai"

/* Throttle command levels (encounter_steering_cmd, +0x33E; see td5_ai_driver.h
 * control-seam table). 0x100 = full forward, 0 = coast. */
#define DRV_THROTTLE_FULL   0x100
#define DRV_STEER_CLAMP     0x18000   /* physics saturation clamp on steering_command */

/* [P4] Per-driver personality (deterministic). Seeded from (slot, difficulty
 * tier) only -- NOT the CRT rand() and NOT a race seed, so it is identical on
 * every lockstep peer and every replay by construction (slot + tier are both
 * replicated). skill scales pace + corner speed; aggression tunes overtaking /
 * following; consistency gates a small deterministic per-corner mistake;
 * line_bias [-1,1] is the driver's preferred lane across the road (route
 * variety, replacing P3's static third-bias). */
typedef struct {
    float skill;        /* 0.12 .. 0.99  */
    float aggression;   /* 0.25 .. 0.95  */
    float consistency;  /* 0.40 .. 0.97 (higher = fewer mistakes) */
    float line_bias;    /* -1 .. +1 preferred lateral fraction     */
} DriverPersona;
static DriverPersona s_persona[TD5_MAX_RACER_SLOTS];

/* Deterministic integer hash (no runtime RNG). */
static uint32_t drv_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static inline float drv_clampf(float v, float lo, float hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------------
 * Mode resolution
 * ---------------------------------------------------------------------- */

int td5_ai_driver_mode(void)
{
    int mode = g_td5.ini.ai_model;

    /* TD5RE_AI_MODEL env override (0/1/2) for quick A/B without touching the
     * INI. Sentinel -1 = "not set" so an absent var keeps the INI value. */
    int ov = td5_env_int_opt("TD5RE_AI_MODEL", 0, TD5_AI_MODE_COUNT - 1, -1);
    if (ov >= 0) mode = ov;

    if (mode < 0) mode = 0;
    if (mode >= TD5_AI_MODE_COUNT) mode = TD5_AI_MODE_COUNT - 1;
    return mode;
}

const char *td5_ai_driver_mode_name(int mode)
{
    switch (mode) {
    case TD5_AI_MODE_CLASSIC: return "CLASSIC";
    case TD5_AI_MODE_SMART:   return "SMART";
    case TD5_AI_MODE_DRIVER:  return "DRIVER";
    default:                  return "?";
    }
}

int td5_ai_driver_active(void)
{
    /* DRIVER mode owns racer AI, except in the faithful-only modes (drag,
     * wanted/cop-chase) which keep their dedicated drivers. Same shape as
     * td5_ai_smart_active(). */
    return td5_ai_driver_mode() == TD5_AI_MODE_DRIVER
        && !g_td5.drag_race_enabled
        && !g_td5.wanted_mode_enabled;
}

int td5_ai_driver_owns_slot(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    if (!td5_ai_driver_active()) return 0;
    return td5_game_get_slot_state(slot) == 0;   /* AI-driven racer only */
}

/* ------------------------------------------------------------------------
 * Env-tunable gains (read once). All safe to leave at defaults; exposed so a
 * calibration pass can adjust from the `driver` trace without a rebuild.
 * ---------------------------------------------------------------------- */

static int    s_knobs_read = 0;
static int    s_look_base;       /* base look-ahead in spans                 */
static int    s_look_speed;      /* extra look-ahead spans at full speed      */
static int    s_steer_kp;        /* steering P gain (steering_command per herr)*/
static int    s_steer_kd;        /* yaw-rate damping gain (Q8 of yaw rate)     */
static int    s_steer_rate;      /* max steering_command change per tick       */
static double s_curv_sharp;      /* route-heading delta (per look win) == full corner */
static double s_corner_floor;    /* min corner speed fraction                  */
static double s_corner_loss;     /* max fraction lost in the sharpest corner   */
static double s_brake_k;         /* backward-pass braking coupling (frac^2 per track-unit) */
static double s_accel_k;         /* forward-pass accel coupling                */
static int    s_thr_kp;          /* throttle P gain (cmd per speed-frac error) */
static double s_thr_deadband;    /* speed-frac error deadband                  */
static int    s_slip_thresh;     /* rear-slip level where the traction cap starts */
static int    s_slip_range;      /* slip span over which the cap ramps to floor    */
static int    s_ahead_spans;     /* [P3] how many spans ahead a peer counts as blocking */
static double s_pass_frac;       /* [P3] overtake aim offset as a fraction of half-width */
static double s_offset_clamp;    /* [P3] max lateral offset as a fraction of half-width  */
static int    s_overtake_ticks;  /* [P3] dwell ticks a pass side stays committed         */
static int    s_leash_max;       /* [P4] hidden catch-up: max % speed boost for laggards  */

static void driver_read_knobs(void)
{
    if (s_knobs_read) return;
    s_knobs_read   = 1;
    /* Look-ahead 3 + up-to-4 spans (calibrated 2026-08-17 on Moscow+Newcastle):
     * a shorter aim point follows the line faithfully and stops the pure-pursuit
     * corner-cutting that rode the inner edge / stalled on Newcastle's tight
     * bends, without costing straight-line pace. */
    s_look_base    = td5_env_int  ("TD5RE_AI_DRIVER_LOOK_BASE",    3,   1,  24);
    s_look_speed   = td5_env_int  ("TD5RE_AI_DRIVER_LOOK_SPEED",   4,   0,  32);
    s_steer_kp     = td5_env_int  ("TD5RE_AI_DRIVER_STEER_KP",    110,  8, 800);
    s_steer_kd     = td5_env_int  ("TD5RE_AI_DRIVER_STEER_KD",     24,  0, 400);
    s_steer_rate   = td5_env_int  ("TD5RE_AI_DRIVER_STEER_RATE", 0x9000, 0x800, DRV_STEER_CLAMP);
    s_curv_sharp   = (double)td5_env_int("TD5RE_AI_DRIVER_CURV_SHARP", 0x2A0, 0x40, 0x800);
    s_corner_floor = (double)td5_env_int("TD5RE_AI_DRIVER_CORNER_FLOOR", 42, 10, 95) / 100.0;
    s_corner_loss  = (double)td5_env_int("TD5RE_AI_DRIVER_CORNER_LOSS",  55, 10, 90) / 100.0;
    s_brake_k      = (double)td5_env_int("TD5RE_AI_DRIVER_BRAKE_K",  12, 1, 400) / 100000.0;
    s_accel_k      = (double)td5_env_int("TD5RE_AI_DRIVER_ACCEL_K",   6, 1, 400) / 100000.0;
    s_thr_kp       = td5_env_int  ("TD5RE_AI_DRIVER_THR_KP",       900, 50, 4000);
    s_thr_deadband = (double)td5_env_int("TD5RE_AI_DRIVER_THR_DEADBAND", 3, 0, 40) / 100.0;
    s_slip_thresh  = td5_env_int  ("TD5RE_AI_DRIVER_SLIP_THRESH", 0x600, 0x40, 0x8000);
    s_slip_range   = td5_env_int  ("TD5RE_AI_DRIVER_SLIP_RANGE",  0x1200, 0x100, 0x8000);
    s_ahead_spans  = td5_env_int  ("TD5RE_AI_DRIVER_AHEAD_SPANS", 10, 2, 40);
    s_pass_frac    = (double)td5_env_int("TD5RE_AI_DRIVER_PASS_FRAC",   55, 10, 90) / 100.0;
    s_offset_clamp = (double)td5_env_int("TD5RE_AI_DRIVER_OFFSET_CLAMP",70, 10, 95) / 100.0;
    s_overtake_ticks = td5_env_int("TD5RE_AI_DRIVER_OVERTAKE_TICKS", 40, 5, 200);
    s_leash_max      = td5_env_int("TD5RE_AI_DRIVER_LEASH", 3, 0, 15); /* hidden catch-up, % ; 0 = off */
}

/* ------------------------------------------------------------------------
 * Path table (per race)
 * ---------------------------------------------------------------------- */

static int      s_pt_count;          /* spans in the table (ring length)      */
static int      s_pt_cap;            /* allocated capacity                    */
static double  *s_pt_seg_len;        /* arc length span[s] -> span[s+1]        */
static double  *s_pt_frac;           /* speed-profile target fraction 0..1     */
static int      s_pt_circuit;        /* 1 = ring wraps, 0 = point-to-point     */

/* running per-slot max longitudinal_speed (the car's demonstrated top speed on
 * this track), used as the speed-fraction reference. */
static double   s_vmax[TD5_MAX_RACER_SLOTS];
static double   s_thr_integ[TD5_MAX_RACER_SLOTS];  /* throttle PI integrator   */
static int      s_prev_steer[TD5_MAX_RACER_SLOTS]; /* last steering_command    */
static double   s_target_frac_dbg[TD5_MAX_RACER_SLOTS]; /* for the trace row   */
/* vmax bootstrap: the fraction profile is meaningless until the car has been
 * flat-out on a straight at least once, because vmax (running max speed) equals
 * the current speed while still accelerating. Until "ready", command full
 * throttle so vmax converges to the car's true top speed; only then start
 * limiting/braking to a fraction of it. */
static int      s_vmax_ready[TD5_MAX_RACER_SLOTS];
static int      s_straight_ticks[TD5_MAX_RACER_SLOTS];

/* [P2] Recovery state machine. NORMAL is the P1 controller; the others take
 * over when the car is airborne, spun (travelling backwards relative to its
 * heading), or stuck (not moving while trying to). Replaces racer_collision_
 * escape for driver-owned cars. */
enum { DRV_NORMAL = 0, DRV_AIRBORNE, DRV_SPUN, DRV_STUCK };
static int      s_rec_state[TD5_MAX_RACER_SLOTS];
static int      s_rec_ticks[TD5_MAX_RACER_SLOTS];   /* ticks in current non-normal state */
static int      s_spin_cnt[TD5_MAX_RACER_SLOTS];    /* consecutive spun-detection ticks   */
static int      s_stuck_cnt[TD5_MAX_RACER_SLOTS];   /* consecutive not-moving ticks        */
static int      s_launched[TD5_MAX_RACER_SLOTS];    /* car has moved off the line at least once */

/* [P3] Racecraft: a smoothed lateral aim-point offset (24.8 world units; + =
 * right of the racing line) that overtaking / defending / hazard-avoidance
 * steer through, plus a dwell so an overtaking commitment doesn't flip lane
 * every tick. */
static double   s_lane_offset[TD5_MAX_RACER_SLOTS];
static int      s_overtake_dwell[TD5_MAX_RACER_SLOTS]; /* ticks left committed to a pass side */

static int      s_pt_valid;          /* table built for this race             */

static inline int ang_signed12(int a) { a &= 0xFFF; if (a > 0x800) a -= 0x1000; return a; }

/* Rail-midpoint of a span in track units (world pos >> 8 space, as SmartAI). */
static int pt_span_mid(int span, double *mx, double *mz)
{
    int lx, lz, rx, rz;
    if (!td5_track_get_span_route_frame(span, &lx, &lz, &rx, &rz)) return 0;
    *mx = 0.5 * ((double)lx + (double)rx);
    *mz = 0.5 * ((double)lz + (double)rz);
    return 1;
}

static void driver_free_path_table(void)
{
    free(s_pt_seg_len); s_pt_seg_len = NULL;
    free(s_pt_frac);    s_pt_frac    = NULL;
    s_pt_cap   = 0;   /* MUST reset with the buffers: otherwise a later race on a
                       * SHORTER ring sees count <= cap, skips the realloc, and
                       * writes through the freed NULL pointer (0xC0000005). */
    s_pt_count = 0;
    s_pt_valid = 0;
}

/* Build arc length + curvature + a fraction-of-max speed profile for the ring.
 * Robust to missing geometry (leaves s_pt_valid=0 and the tick drives on a
 * constant target instead). */
static void driver_build_path_table(void)
{
    driver_free_path_table();

    int count = td5_track_get_ring_length();
    if (count <= 2) count = td5_track_get_span_count();
    if (count <= 2) return;

    s_pt_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);

    if (count > s_pt_cap) {
        double *sl = (double *)realloc(s_pt_seg_len, (size_t)count * sizeof(double));
        double *fr = (double *)realloc(s_pt_frac,    (size_t)count * sizeof(double));
        if (!sl || !fr) { free(sl); free(fr); s_pt_seg_len = NULL; s_pt_frac = NULL; s_pt_cap = 0; return; }
        s_pt_seg_len = sl; s_pt_frac = fr; s_pt_cap = count;
    }

    /* Pass 0: arc length (mid[s] -> mid[s+1]) + curvature-based slow-in cap. */
    double *curv_cap = (double *)malloc((size_t)count * sizeof(double));
    if (!curv_cap) return;

    for (int s = 0; s < count; s++) {
        double ax, az, bx, bz;
        int s1 = (s + 1) % count;
        double len = 1.0;
        if (pt_span_mid(s, &ax, &az) && pt_span_mid(s1, &bx, &bz)) {
            double dx = bx - ax, dz = bz - az;
            len = sqrt(dx * dx + dz * dz);
            if (len < 1.0) len = 1.0;
        }
        s_pt_seg_len[s] = len;

        /* Curvature over a short window ahead (heading delta), like SmartAI's
         * smart_corner_eval. Sharper bend -> lower cap; floor keeps it moving. */
        int s0 = s;
        int s2 = (s + 3) % count;
        int turn = ang_signed12(td5_track_get_primary_route_heading(s2)
                                - td5_track_get_primary_route_heading(s0));
        double sharp = (double)(turn < 0 ? -turn : turn) / s_curv_sharp;
        if (sharp > 1.0) sharp = 1.0;
        double cap = 1.0 - s_corner_loss * sharp;
        if (cap < s_corner_floor) cap = s_corner_floor;
        curv_cap[s] = cap;
    }

    for (int s = 0; s < count; s++) s_pt_frac[s] = curv_cap[s];

    /* Backward braking pass: target[s] can be no faster than what still lets the
     * car slow to target[s+1] over seg_len[s]. v^2 model in fraction space:
     *   frac[s] = min(cap[s], sqrt(frac[s+1]^2 + BRAKE_K * seg_len[s]))
     * Two loops around the ring so a circuit converges across the wrap. */
    int passes = s_pt_circuit ? 2 : 1;
    for (int p = 0; p < passes; p++) {
        for (int i = 0; i < count; i++) {
            int s  = count - 1 - i;
            int s1 = (s + 1) % count;
            if (!s_pt_circuit && s == count - 1) continue;
            double lim = sqrt(s_pt_frac[s1] * s_pt_frac[s1] + s_brake_k * s_pt_seg_len[s]);
            if (lim < s_pt_frac[s]) s_pt_frac[s] = lim;
            if (s_pt_frac[s] < s_corner_floor) s_pt_frac[s] = s_corner_floor;
        }
    }
    /* Forward accel pass: corner exit can't jump instantly to full. */
    for (int p = 0; p < passes; p++) {
        for (int s = 0; s < count; s++) {
            int sp = (s - 1 + count) % count;
            if (!s_pt_circuit && s == 0) continue;
            double lim = sqrt(s_pt_frac[sp] * s_pt_frac[sp] + s_accel_k * s_pt_seg_len[sp]);
            if (lim < s_pt_frac[s]) s_pt_frac[s] = lim;
        }
    }

    free(curv_cap);
    s_pt_count = count;
    s_pt_valid = 1;
    TD5_LOG_I(LOG_TAG, "ai_driver: path table built spans=%d circuit=%d",
              count, s_pt_circuit);
}

/* ------------------------------------------------------------------------
 * Per-race init
 * ---------------------------------------------------------------------- */

void td5_ai_driver_race_init(void)
{
    driver_read_knobs();
    for (int s = 0; s < TD5_MAX_RACER_SLOTS; s++) {
        s_vmax[s]           = 1.0;   /* tiny positive; grows to real top speed */
        s_thr_integ[s]      = 0.0;
        s_prev_steer[s]     = 0;
        s_target_frac_dbg[s] = 0.0;
        s_vmax_ready[s]     = 0;
        s_straight_ticks[s] = 0;
        s_rec_state[s]      = DRV_NORMAL;
        s_rec_ticks[s]      = 0;
        s_spin_cnt[s]       = 0;
        s_stuck_cnt[s]      = 0;
        s_launched[s]       = 0;
        s_lane_offset[s]    = 0.0;
        s_overtake_dwell[s] = 0;
    }
    if (td5_ai_driver_mode() != TD5_AI_MODE_DRIVER) { driver_free_path_table(); return; }
    driver_build_path_table();

    /* [P4] Derive per-driver personalities from (slot, difficulty tier). */
    int tier = g_td5.difficulty_tier;
    float base = (tier <= 0) ? 0.42f : (tier == 1 ? 0.63f : 0.86f);
    for (int s = 0; s < TD5_MAX_RACER_SLOTS; s++) {
        uint32_t h = drv_hash((uint32_t)s * 2654435761u + (uint32_t)(tier + 1) * 40503u);
        float sk = drv_clampf(base + ((float)(h & 0xFFFF) / 65535.0f - 0.5f) * 0.24f, 0.12f, 0.99f);
        s_persona[s].skill      = sk;
        s_persona[s].aggression = 0.25f + (float)((h >> 16) & 0xFF) / 255.0f * 0.70f;
        /* consistency correlates with skill (aces make fewer mistakes) */
        s_persona[s].consistency = drv_clampf(
            (0.55f + (float)((h >> 8) & 0xFF) / 255.0f * 0.40f) * 0.6f + sk * 0.4f, 0.40f, 0.97f);
        s_persona[s].line_bias  = ((float)((h >> 1) & 0x3FF) / 1023.0f - 0.5f) * 2.0f;
    }
    TD5_LOG_I(LOG_TAG, "ai_driver_race_init: DRIVER mode, path_valid=%d tier=%d "
              "(s0 skill=%.2f aggr=%.2f cons=%.2f line=%.2f)",
              s_pt_valid, tier, (double)s_persona[0].skill, (double)s_persona[0].aggression,
              (double)s_persona[0].consistency, (double)s_persona[0].line_bias);
}

/* ------------------------------------------------------------------------
 * Per-tick controller
 * ---------------------------------------------------------------------- */

/* atan2-based 12-bit angle, matching ai_angle_from_vector's convention:
 * 0 = +Z, 0x400 = +X, clockwise positive. */
static int driver_angle_from_vec(double dx, double dz)
{
    if (dx == 0.0 && dz == 0.0) return 0;
    double a = atan2(dx, dz) * (4096.0 / (2.0 * 3.14159265358979323846));
    int ai = (int)a;
    return ai & 0xFFF;
}

/* [P3] Racecraft: scan the other cars and decide (a) a speed cap as a fraction
 * of this car's top speed (to follow a slower car ahead instead of rear-ending
 * it, or to slow for a hazard) and (b) a lateral aim-point offset in 24.8 world
 * units (to commit to an overtake down a clear side). Hybrid proximity: span
 * ordering gives reliable ahead/behind; a metric lateral projection gives lane
 * overlap the span-only classic search can't see (a car one lane over).
 *
 * right is the car's unit rightward vector in world (x,z); fore/aft comes from
 * span ordering, so only the lateral (right) projection is needed here. */
static void driver_racecraft(int slot, TD5_Actor *self,
                             double rightx, double rightz,
                             double v_abs, double vmax, double straightness,
                             double *out_cap, double *out_offset)
{
    *out_cap = 1.0;
    *out_offset = 0.0;

    int ring = (s_pt_valid && s_pt_count > 0) ? s_pt_count : td5_track_get_ring_length();
    if (ring <= 2) return;

    int self_span = (int)self->track_span_normalized;

    /* half road width at the car (24.8), from the rail frame. */
    double hw = 0.25e6;   /* fallback if geometry unavailable */
    { int lx, lz, rx, rz;
      if (td5_track_get_span_route_frame(((self_span % ring) + ring) % ring, &lx, &lz, &rx, &rz)) {
          double ddx = (double)(rx - lx), ddz = (double)(rz - lz);
          double w_tu = 0.5 * sqrt(ddx * ddx + ddz * ddz);
          if (w_tu > 1.0) hw = w_tu * 256.0;   /* track-units -> 24.8 */
      } }
    double block_w = 0.60 * hw;

    /* [P4] Per-driver preferred line across the road (route variety) -- replaces
     * P3's static third-bias. Each driver favours its own lateral fraction, so
     * the field spreads out and cars don't all converge on one line.
     * [P5] Scaled by straightness: cars run their preferred line on straights /
     * gentle bends but converge toward the racing line through tight corners,
     * where an off-centre line would clip the walls (Newcastle/BlueRidge). */
    double baseline = (double)s_persona[slot].line_bias * 0.32 * hw * straightness;

    double self_x = (double)self->world_pos.x;
    double self_z = (double)self->world_pos.z;

    int    have_ahead = 0;
    int    ahead_gap  = 999;
    double ahead_v    = 0.0;
    double ahead_lat  = 0.0;
    int    left_block = 0, right_block = 0;   /* a peer sitting in the pass corridor */

    int total = td5_game_get_total_actor_count();
    for (int j = 0; j < total && j < TD5_MAX_TOTAL_ACTORS; j++) {
        if (j == slot) continue;
        if (td5_game_get_slot_state(j) == 3) continue;   /* inactive grid slot */
        TD5_Actor *p = td5_game_get_actor(j);
        if (!p) continue;

        double rx = (double)p->world_pos.x - self_x;
        double rz = (double)p->world_pos.z - self_z;
        double lat = rx * rightx + rz * rightz;   /* + = to my right */
        double latabs = lat < 0 ? -lat : lat;

        int pspan = (int)p->track_span_normalized;
        int d = pspan - self_span;
        if (d >  ring / 2) d -= ring;
        if (d < -ring / 2) d += ring;

        double pv = (double)p->longitudinal_speed;
        if (pv < 0) pv = -pv;

        /* nearest car ahead, roughly in my path */
        if (d > 0 && d <= s_ahead_spans && latabs < block_w) {
            if (d < ahead_gap) { ahead_gap = d; ahead_v = pv; ahead_lat = lat; have_ahead = 1; }
        }
        /* pass-corridor occupancy (from just behind to a bit ahead, out to a lane) */
        if (d > -2 && d <= s_ahead_spans && latabs >= 0.20 * hw && latabs <= 1.05 * hw) {
            if (lat > 0) right_block = 1; else left_block = 1;
        }
    }

    if (!have_ahead) { s_overtake_dwell[slot] = 0; *out_offset = baseline; return; }

    /* [P4] Aggression tunes how small a speed deficit is worth a pass: an
     * aggressive driver pulls out for a car only slightly slower; a cautious
     * one waits for a clear speed advantage. */
    double aggr = (double)s_persona[slot].aggression;
    /* pass line also converges toward the racing line in tight corners (but not
     * to zero -- overtakes into corners still happen). */
    double pass_mag  = s_pass_frac * hw * (0.35 + 0.65 * straightness);
    double slower_thr = 0.99 - 0.07 * aggr;    /* ~0.99 (cautious) .. ~0.92 (aggressive) */
    int    slower    = ahead_v < v_abs * slower_thr;
    int    very_slow = ahead_v < vmax * 0.15;   /* stopped / crawling hazard */

    if (s_overtake_dwell[slot] > 0) {
        /* stay committed to the side we already chose (sign of the live offset) */
        double side = (s_lane_offset[slot] >= 0.0) ? 1.0 : -1.0;
        *out_offset = side * pass_mag;
        *out_cap = 1.0;
        s_overtake_dwell[slot]--;
        return;
    }

    /* choose the side away from the car ahead; require it clear */
    double want_side = (ahead_lat <= 0.0) ? 1.0 : -1.0;   /* peer left -> pass right */
    int side_clear = (want_side > 0.0) ? !right_block : !left_block;

    if ((slower || very_slow) && side_clear) {
        *out_offset = want_side * pass_mag;
        *out_cap = 1.0;                        /* commit to the pass, keep the speed */
        s_overtake_dwell[slot] = s_overtake_ticks;
        return;
    }

    /* can't pass -> follow the car ahead: cap speed to it, scaled by the gap so
     * we tuck in a car-length back instead of punting it. */
    double gapf = 0.55 + 0.055 * (double)ahead_gap;   /* gap 1 -> ~0.6, gap 8 -> ~1.0 */
    if (gapf > 1.0) gapf = 1.0;
    double cap = (vmax > 1.0) ? (ahead_v / vmax) * gapf : 1.0;
    if (ahead_gap <= 1) { double c2 = (ahead_v / vmax) * 0.85; if (c2 < cap) cap = c2; }
    if (very_slow && cap > 0.25) cap = 0.25;   /* hazard with no room: slow hard */
    if (cap < 0.05) cap = 0.05;
    if (cap > 1.0)  cap = 1.0;
    *out_cap = cap;
    *out_offset = baseline;   /* hold your own lane while tucked in behind */
}

int td5_ai_driver_tick(int slot)
{
    driver_read_knobs();
    if (!td5_ai_driver_active()) return 0;      /* mode changed mid-race -> classic */
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;

    TD5_Actor *actor = td5_game_get_actor(slot);
    if (!actor) return 0;

    /* --- speed reference: this car's demonstrated top speed on this track --- */
    int32_t v_long = actor->longitudinal_speed;
    double  v_abs  = (double)(v_long < 0 ? -v_long : v_long);
    if (v_abs > s_vmax[slot]) s_vmax[slot] = v_abs;
    double vmax = s_vmax[slot];

    /* --- dynamics: world velocity, travel direction vs heading (spin), slip --- */
    double vx = (double)actor->linear_velocity_x;
    double vz = (double)actor->linear_velocity_z;
    double speed2d = sqrt(vx * vx + vz * vz);
    int heading = ((int)actor->euler_accum.yaw >> 8) & 0xFFF;
    int vel_head = (speed2d > 1.0) ? driver_angle_from_vec(vx, vz) : heading;
    int misalign = ang_signed12(vel_head - heading);
    int misalign_abs = misalign < 0 ? -misalign : misalign;   /* 0..0x800 */
    int32_t yaw_rate = actor->angular_velocity_yaw;
    int32_t rslip = actor->rear_axle_slip_excess;
    if (rslip < 0) rslip = -rslip;

    /* --- target speed fraction from the profile at the current ring span --- */
    double frac = 0.80;   /* fallback if the path table is unavailable */
    if (s_pt_valid && s_pt_count > 0) {
        int span = (int)actor->track_span_normalized;
        if (span < 0) span = 0;
        if (span >= s_pt_count) span %= s_pt_count;
        frac = s_pt_frac[span];
    }
    s_target_frac_dbg[slot] = frac;
    double target_speed = frac * vmax;

    /* --- STEERING aim point (pure pursuit); reused by recovery + normal --- */
    double speed_frac_now = (vmax > 1.0) ? (v_abs / vmax) : 0.0;
    if (speed_frac_now > 1.0) speed_frac_now = 1.0;
    int lookahead = s_look_base + (int)(speed_frac_now * (double)s_look_speed);
    if (lookahead < 1) lookahead = 1;

    int tx = 0, tz = 0;
    int have_target = td5_track_laneassist_target((int)actor->track_span_raw,
                                                  (int)actor->track_sub_lane_index,
                                                  lookahead, /*fork_commit=*/1,
                                                  /*fork_diverge=*/1, /*lane_band=*/1,
                                                  &tx, &tz);
    int herr = 0;
    if (have_target) {
        double dx = (double)(tx - actor->world_pos.x);
        double dz = (double)(tz - actor->world_pos.z);
        herr = ang_signed12(driver_angle_from_vec(dx, dz) - heading);
    }

    /* --- RECOVERY STATE MACHINE ---
     * Runs before the normal controller. Spin = travelling backwards relative
     * to the way the car points (misalign near 180); stuck = not moving while
     * meant to. Speed thresholds are relative to this car's own top speed so
     * they scale with the car/track. (No airborne state: a brief crest is
     * normal racing, not a recovery situation -- an earlier airborne branch
     * over-triggered on Newcastle's undulations and cut the launch throttle,
     * deadlocking the car.) */
    double spin_speed_min = 0.05 * vmax;   /* only judge a spin above a crawl */
    double stuck_speed    = 0.015 * vmax;  /* effectively stationary          */
    double stuck_exit     = 0.06 * vmax;
    int st = s_rec_state[slot];

    /* spin detection (hysteresis) */
    if (speed2d > spin_speed_min && misalign_abs > 0x600) {
        if (s_spin_cnt[slot] < 99) s_spin_cnt[slot]++;
    } else if (s_spin_cnt[slot] > 0) {
        s_spin_cnt[slot]--;
    }
    /* stuck detection -- but only AFTER the car has launched off the line. Until
     * then (grid + countdown) every car sits at v=0, which is not "stuck": the
     * game holds them for the green light. Rocking free during the countdown
     * would command reverse against a field of stationary cars. */
    if (v_abs > stuck_exit) s_launched[slot] = 1;
    if (v_abs < stuck_speed) s_stuck_cnt[slot]++;
    else                     s_stuck_cnt[slot] = 0;

    if (st == DRV_NORMAL) {
        if (s_spin_cnt[slot] >= 3)         { st = DRV_SPUN;  s_rec_ticks[slot] = 0;
            TD5_LOG_I(LOG_TAG, "ai_driver: slot=%d SPUN (misalign=0x%X yaw=%d spd2d=%d)",
                      slot, misalign_abs, (int)yaw_rate, (int)speed2d); }
        else if (s_launched[slot] && s_stuck_cnt[slot] >= 45)  { st = DRV_STUCK; s_rec_ticks[slot] = 0;
            TD5_LOG_I(LOG_TAG, "ai_driver: slot=%d STUCK (v=%d)", slot, (int)v_abs); }
    } else if (st == DRV_SPUN) {
        /* recovered when re-aligned and no longer spinning fast */
        int yr = yaw_rate < 0 ? -yaw_rate : yaw_rate;
        if ((misalign_abs < 0x280 && yr < 0x1800) || v_abs < stuck_speed) {
            int held = s_rec_ticks[slot];
            st = (s_launched[slot] && v_abs < stuck_speed && s_stuck_cnt[slot] >= 45) ? DRV_STUCK : DRV_NORMAL;
            s_spin_cnt[slot] = 0;
            s_rec_ticks[slot] = 0;
            TD5_LOG_I(LOG_TAG, "ai_driver: slot=%d SPUN->%s after %d ticks",
                      slot, st == DRV_STUCK ? "STUCK" : "NORMAL", held);
        }
    } else if (st == DRV_STUCK) {
        if (v_abs > stuck_exit)            { st = DRV_NORMAL; s_stuck_cnt[slot] = 0; s_rec_ticks[slot] = 0;
            TD5_LOG_I(LOG_TAG, "ai_driver: slot=%d STUCK->NORMAL (v=%d)", slot, (int)v_abs); }
    }
    s_rec_state[slot] = st;

    int32_t steer_cmd = s_prev_steer[slot];
    int     throttle  = 0;
    uint8_t brake     = 0;

    if (st != DRV_NORMAL) {
        s_rec_ticks[slot]++;
        if (st == DRV_SPUN) {
            /* Coast + steer to re-align with the track-forward aim, with strong
             * yaw damping to kill the rotation; scrub speed if still fast. */
            long tc = (long)herr * (long)s_steer_kp
                    - ((long)yaw_rate * (long)(s_steer_kd * 3) >> 8);
            if (tc >  DRV_STEER_CLAMP) tc =  DRV_STEER_CLAMP;
            if (tc < -DRV_STEER_CLAMP) tc = -DRV_STEER_CLAMP;
            long d = tc - (long)steer_cmd;
            if (d >  s_steer_rate) d =  s_steer_rate;
            if (d < -s_steer_rate) d = -s_steer_rate;
            steer_cmd = (int32_t)((long)steer_cmd + d);
            throttle = 0;
            if (speed2d > 0.25 * vmax) brake = 1;   /* bleed off a fast spin */
        } else { /* DRV_STUCK: rock free. Try steer-to-track + forward FIRST
                  * (frees most shove-offs); only reverse if forward didn't
                  * work within the first phase (truly nosed into a wall). */
            int phase = s_rec_ticks[slot] % 60;
            long tc = (long)herr * (long)s_steer_kp;
            if (tc >  DRV_STEER_CLAMP) tc =  DRV_STEER_CLAMP;
            if (tc < -DRV_STEER_CLAMP) tc = -DRV_STEER_CLAMP;
            steer_cmd = (int32_t)tc;
            if (phase < 35) {
                throttle = DRV_THROTTLE_FULL;   /* pull forward toward the track */
            } else {
                /* reverse away (encounter_steering_cmd < 0 drives the reverse path) */
                actor->encounter_steering_cmd = (int16_t)(-0xC0);
                actor->brake_flag = 0;
                actor->throttle_state = 0;
                actor->steering_command = steer_cmd;
                s_prev_steer[slot] = steer_cmd;
                return 1;
            }
        }
        if (steer_cmd >  DRV_STEER_CLAMP) steer_cmd =  DRV_STEER_CLAMP;
        if (steer_cmd < -DRV_STEER_CLAMP) steer_cmd = -DRV_STEER_CLAMP;
        s_prev_steer[slot] = steer_cmd;
        actor->steering_command       = steer_cmd;
        actor->encounter_steering_cmd = (int16_t)throttle;
        actor->brake_flag             = brake;
        actor->throttle_state         = (uint8_t)(brake ? 1 : 0);
        return 1;
    }

    /* ================= NORMAL CONTROL ================= */

    /* --- RACECRAFT: follow / overtake / hazard relative to the other cars.
     * Produces a speed cap (fraction of top speed) and a lateral aim offset. */
    double cap_frac = 1.0;
    if (have_target && s_launched[slot]) {
        double theta  = (double)heading * (2.0 * 3.14159265358979323846 / 4096.0);
        double rightx = cos(theta), rightz = -sin(theta); /* +right of heading */
        /* straightness 0 (tight corner) .. 1 (straight), from the profile frac */
        double straightness = (frac - 0.55) / 0.45;
        if (straightness < 0.0) straightness = 0.0;
        if (straightness > 1.0) straightness = 1.0;
        double off_target = 0.0;
        driver_racecraft(slot, actor, rightx, rightz,
                         v_abs, vmax, straightness, &cap_frac, &off_target);
        /* ease the lateral offset (anti-yank) */
        s_lane_offset[slot] += (off_target - s_lane_offset[slot]) * 0.15;
        /* shift the aim point sideways and recompute the heading error */
        double atx = (double)tx + s_lane_offset[slot] * rightx;
        double atz = (double)tz + s_lane_offset[slot] * rightz;
        double dx = atx - (double)actor->world_pos.x;
        double dz = atz - (double)actor->world_pos.z;
        herr = ang_signed12(driver_angle_from_vec(dx, dz) - heading);
    } else {
        s_lane_offset[slot] = 0.0;
        s_overtake_dwell[slot] = 0;
    }

    /* --- STEERING: pure pursuit + yaw-rate damping (replaces the banded slam) --- */
    if (have_target) {
        long target_cmd = (long)herr * (long)s_steer_kp
                        - ((long)yaw_rate * (long)s_steer_kd >> 8);
        if (target_cmd >  DRV_STEER_CLAMP) target_cmd =  DRV_STEER_CLAMP;
        if (target_cmd < -DRV_STEER_CLAMP) target_cmd = -DRV_STEER_CLAMP;
        long d = target_cmd - (long)steer_cmd;
        if (d >  s_steer_rate) d =  s_steer_rate;
        if (d < -s_steer_rate) d = -s_steer_rate;
        steer_cmd = (int32_t)((long)steer_cmd + d);
    } else {
        steer_cmd -= steer_cmd / 8;   /* no aim point: bleed toward centre */
    }
    if (steer_cmd >  DRV_STEER_CLAMP) steer_cmd =  DRV_STEER_CLAMP;
    if (steer_cmd < -DRV_STEER_CLAMP) steer_cmd = -DRV_STEER_CLAMP;
    s_prev_steer[slot] = steer_cmd;
    actor->steering_command = steer_cmd;

    /* --- THROTTLE / BRAKE (backward-braked profile => braking points free) ---
     * base_frac = min(profile fraction, racecraft follow cap). The floor
     * decision keys on THIS (not the persona-shaped value): a clear straight is
     * always flat-out, so vmax keeps converging to the car's true top speed on
     * every straight (fixes the bootstrap trap where persona pace pushed the
     * value below 0.97 and froze vmax early). The follow cap still gates the
     * floor, so a car settles behind a slower car ahead instead of flooring
     * into it. Personality shapes only the LIMITED (corner / following) target,
     * which is where driver skill actually shows -- top speed stays uniform. */
    double base_frac = frac;
    if (cap_frac < base_frac) base_frac = cap_frac;

    if (base_frac >= 0.97) {
        s_straight_ticks[slot]++;
        if (s_straight_ticks[slot] >= 20) s_vmax_ready[slot] = 1;
    }

    if (!s_vmax_ready[slot] || base_frac >= 0.97) {
        throttle = DRV_THROTTLE_FULL;   /* launch / clear straight: floor (builds vmax) */
        s_thr_integ[slot] = 0.0;
        target_speed = base_frac * vmax;
    } else {
        /* Corner / following: apply personality + hidden leash to the target. */
        double skill = (double)s_persona[slot].skill;
        double pace  = 0.90 + 0.10 * skill;   /* weaker drivers carry less corner speed */

        double leash = 1.0;   /* subtle catch-up for cars running behind (target only) */
        if (s_leash_max > 0) {
            int rpos = (int)actor->race_position;
            if (rpos > 3) rpos = 3;
            if (rpos > 0) leash = 1.0 + (double)rpos * ((double)s_leash_max / 100.0) / 3.0;
        }

        /* consistency: a small deterministic per-corner error (span-bucketed,
         * hashed -- no RNG, so it stays lockstep/replay-deterministic). */
        int cb = ((int)actor->track_span_normalized) / 3;
        uint32_t hj = drv_hash((uint32_t)slot * 2654435761u ^ (uint32_t)cb * 40503u);
        double j = (double)(hj & 0xFFFF) / 65535.0 - 0.5;   /* -0.5..0.5 */
        double jit = 1.0 + j * (1.0 - (double)s_persona[slot].consistency) * 0.20;

        double eff = base_frac * pace * leash * jit;
        if (eff > base_frac + 0.06) eff = base_frac + 0.06;   /* bounded mistake */
        if (eff < 0.05) eff = 0.05;
        target_speed = eff * vmax;

        double err_frac = (vmax > 1.0) ? (target_speed - v_abs) / vmax : 0.0;
        if (err_frac > s_thr_deadband) {
            s_thr_integ[slot] += err_frac;
            if (s_thr_integ[slot] > 1.0) s_thr_integ[slot] = 1.0;
            double cmd = (double)s_thr_kp * (err_frac + 0.15 * s_thr_integ[slot]);
            throttle = (int)cmd;
            if (throttle > DRV_THROTTLE_FULL) throttle = DRV_THROTTLE_FULL;
            if (throttle < 0) throttle = 0;
        } else if (err_frac < -s_thr_deadband) {
            s_thr_integ[slot] = 0.0;
            throttle = 0;
            if (err_frac < -0.08) brake = 1;
        } else {
            s_thr_integ[slot] = 0.0;
            throttle = (int)((double)s_thr_kp * 0.20);
            if (throttle > DRV_THROTTLE_FULL) throttle = DRV_THROTTLE_FULL;
            if (throttle < 0) throttle = 0;
        }
    }

    /* --- TRACTION CAP: back off power when the rear is sliding beyond grip, so
     * a power-on corner exit doesn't snap into a spin. Never touches braking. */
    if (!brake && throttle > 0 && rslip > s_slip_thresh) {
        double over = (double)(rslip - s_slip_thresh) / (double)s_slip_range;
        double k = 1.0 - over;
        if (k < 0.25) k = 0.25;
        throttle = (int)((double)throttle * k);
        if (throttle < 0) throttle = 0;
    }

    actor->encounter_steering_cmd = (int16_t)throttle;
    actor->brake_flag             = brake;
    actor->throttle_state         = (uint8_t)(brake ? 1 : 0);

    return 1;   /* driver model owns this car this tick */
}

int td5_ai_driver_target_speed(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    /* Report the target in longitudinal_speed units for the `driver` trace:
     * fraction * this car's running-max top speed. */
    return (int)(s_target_frac_dbg[slot] * s_vmax[slot]);
}

int td5_ai_driver_rec_state(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_rec_state[slot];
}
