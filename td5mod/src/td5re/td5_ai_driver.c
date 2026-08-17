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
    }
    if (td5_ai_driver_mode() != TD5_AI_MODE_DRIVER) { driver_free_path_table(); return; }
    driver_build_path_table();
    TD5_LOG_I(LOG_TAG, "ai_driver_race_init: DRIVER mode, path_valid=%d", s_pt_valid);
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

    /* --- STEERING: pure pursuit to a speed-scaled look-ahead aim point --- */
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

    int32_t steer_cmd = s_prev_steer[slot];
    if (have_target) {
        double dx = (double)(tx - actor->world_pos.x);
        double dz = (double)(tz - actor->world_pos.z);
        int heading = ((int)actor->euler_accum.yaw >> 8) & 0xFFF;
        int des_h   = driver_angle_from_vec(dx, dz);
        int herr    = ang_signed12(des_h - heading);   /* [-2048, +2048] */

        /* steering_command = +Kp*herr - Kd*yaw_rate (derived-sign negative
         * feedback: target-to-the-right -> herr>0 -> +cmd -> physics turns right).
         * Yaw-rate damping trims overshoot so the car settles instead of weaving,
         * replacing the banded slam. */
        int32_t yaw_rate = actor->angular_velocity_yaw;
        long target_cmd = (long)herr * (long)s_steer_kp
                        - ((long)yaw_rate * (long)s_steer_kd >> 8);
        if (target_cmd >  DRV_STEER_CLAMP) target_cmd =  DRV_STEER_CLAMP;
        if (target_cmd < -DRV_STEER_CLAMP) target_cmd = -DRV_STEER_CLAMP;

        /* rate-limit per tick */
        long d = target_cmd - (long)steer_cmd;
        if (d >  s_steer_rate) d =  s_steer_rate;
        if (d < -s_steer_rate) d = -s_steer_rate;
        steer_cmd = (int32_t)((long)steer_cmd + d);
    } else {
        /* no look-ahead (off track / no track): bleed steering toward centre */
        steer_cmd -= steer_cmd / 8;
    }
    if (steer_cmd >  DRV_STEER_CLAMP) steer_cmd =  DRV_STEER_CLAMP;
    if (steer_cmd < -DRV_STEER_CLAMP) steer_cmd = -DRV_STEER_CLAMP;
    s_prev_steer[slot] = steer_cmd;
    actor->steering_command = steer_cmd;

    /* --- THROTTLE / BRAKE ---
     * The profile at the current span (frac) is already backward-braked, so it
     * dips on a corner APPROACH -> braking points fall out naturally. But the
     * fraction is only meaningful once vmax reflects the true top speed, so we
     * launch flat-out until the car has run a straight (frac>=0.97) for a while. */
    int throttle;
    uint8_t brake = 0;

    if (frac >= 0.97) {
        s_straight_ticks[slot]++;
        if (s_straight_ticks[slot] >= 20) s_vmax_ready[slot] = 1;
    }

    if (!s_vmax_ready[slot] || frac >= 0.97) {
        /* Launch / straight: floor it. This both drives the car and lets vmax
         * converge to the car's real flat-out speed for the profile below. */
        throttle = DRV_THROTTLE_FULL;
        s_thr_integ[slot] = 0.0;
    } else {
        /* Corner: hold a fraction of the (now real) top speed. PI on the
         * speed-fraction error; lift then brake as the car goes over. */
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
            if (err_frac < -0.08) brake = 1;   /* meaningfully too fast -> brake */
        } else {
            s_thr_integ[slot] = 0.0;
            throttle = (int)((double)s_thr_kp * 0.20);  /* gentle maintenance */
            if (throttle > DRV_THROTTLE_FULL) throttle = DRV_THROTTLE_FULL;
            if (throttle < 0) throttle = 0;
        }
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
