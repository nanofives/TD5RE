/* ========================================================================
 * td5_laneassist.c — optional steering aid (PORT-ONLY, default OFF)
 *
 * See td5_laneassist.h for the user-facing description and knobs.
 *
 * Design (none of this exists in the original binary — accessibility aid):
 *
 *  1. Each tick, for an assisted human player, build a CONTINUOUS look-ahead
 *     lane-centre target line via td5_track_laneassist_target(): it walks a
 *     few spans forward from the player's current (span, sub_lane), following
 *     the SAME fork rule the original walker uses (UpdateActorTrackPosition
 *     @ 0x004440F0) and carrying the lane across lane-count changes with the
 *     confirmed remap sub_lane += branch_lanes - cur_lanes, then blends the
 *     sampled lane centres (near-weighted) into a single aim point (24.8 FP).
 *
 *  2. Compute the heading error to that aim point (pure-pursuit): the signed
 *     12-bit angle between the car's heading and the direction to the target.
 *
 *  3. STEER toward the lane with a capped PD controller on the yaw rate (the
 *     same place the handling yaw torque lands, angular_velocity_yaw): a
 *     proportional term turns toward the aim point and a derivative term brakes
 *     the current yaw rate so a strong gain converges without over-rotating /
 *     spinning. It FADES OUT while the player steers hard so it never overrides
 *     input. There is deliberately NO raw lateral-velocity injection — pushing
 *     the body sideways pumped lateral speed and threw the car into a spin-out;
 *     a steering aid STEERS and lets the car's own motion carry it onto the line.
 *
 *  4. Continuity across forks/merges: the lane remap keeps the sampled lane
 *     physically continuous through lane-count changes; the multi-span blend
 *     averages out single-span steps; and the aim point is additionally eased
 *     per-tick (exponential smoothing) so a fork/merge transition glides over
 *     several ticks/spans rather than jumping. A large jump (car reset / new
 *     race) snaps instead of easing.
 *
 * Routing: lane assist is a DRIVING aid, so it is keyed per-DRIVER — the human
 * player driving actor slot s is player s (the identity mapping confirmed at
 * td5_input.c:631). (view_for_player() maps a player to the PANE that shows
 * their car, which is the right thing for VIEW actions — camera / rear-view —
 * but the wrong thing for a driving aid, which must act on the car the player
 * actually controls.)
 * ======================================================================== */

#include "td5_laneassist.h"

#include "td5_types.h"
#include "td5_track.h"
#include "td5re.h"          /* g_td5 (ini.lane_assist) */
#include "td5_platform.h"   /* TD5_LOG_* */
#include "../../../re/include/td5_actor_struct.h"

#include <stdlib.h>         /* getenv, atoi */
#include <string.h>         /* strchr (sweep-diag env parse) */
#include <math.h>           /* atan2 — heading toward the look-ahead target */

#define LOG_TAG "laneassist"

/* ---- Tunables (read once from INI + env) ------------------------------- */
static int s_inited        = 0;
static int s_master_enable = 0;    /* [Input] LaneAssist — per-session default  */
static int s_strength_pct  = 900;  /* TD5RE_LANEASSIST_STRENGTH (ramp slope %)    */
static int s_lookahead     = 6;    /* TD5RE_LANEASSIST_LOOKAHEAD_SPANS (1..8)    */
static int s_max_yaw       = 400;  /* TD5RE_LANEASSIST_MAX_YAW (near-centre cap;
                                    * rises to LA_FAR_MAX with distance)         */
static int s_fork_commit   = 1;    /* TD5RE_LANEASSIST_FORK_COMMIT (0/1)         */
static int s_lane_band     = 0;    /* TD5RE_LANEASSIST_LANE_BAND: 0 = centre of ALL
                                    * drivable lanes (whole paved road, default);
                                    * 1 = single lane; 2 = centre of two; etc.     */

/* Derivative damping (on the heading-ERROR rate, not the absolute yaw rate — so
 * it damps the weave WITHOUT fighting your steering) + steering fade band. Fixed
 * (not env); STRENGTH (proportional) and MAX_YAW (cap) cover tuning. d_herr is
 * CLAMPED so a one-tick error jump at a lane-geometry change (fork/span step)
 * can't "kick" the derivative and reverse the steer. */
#define LA_YAW_DKD      12         /* light derivative damping (settle w/o weave)  */
#define LA_YAW_DCLAMP   4          /* reject derivative kicks from herr jumps      */
/* Steering = a LINEAR cross-track term (the correction climbs CONTINUOUSLY the
 * further off-centre you are — no gentle/aggressive two-step) + a modest heading
 * anticipation term for corners + light damping. The yaw cap also rises with
 * distance so the far end of the ramp isn't clipped early.
 *   yaw_ct = lat_err * strength% / LA_CT_DIV     (main, distance-proportional)
 *   yaw_p  = herr    * strength% / LA_HEAD_DIV    (modest corner anticipation)   */
#define LA_CT_DIV       100000     /* lat_err(24.8) * strength% -> yaw, linear     */
#define LA_HEAD_DIV     500        /* herr * strength% -> yaw (small vs cross-track)*/
#define LA_FAR_DIV      220        /* cap += |lat_err| / LA_FAR_DIV                 */
#define LA_FAR_MAX      1100       /* absolute yaw-cap ceiling (< handling ±1400)   */
#define LA_EASE_SHIFT   3          /* target ease: target += (raw-target)>>3     */
#define LA_SNAP         0x40000    /* >1024 world-units jump -> snap, not ease   */
#define LA_MIN_VLONG    0x80       /* no nudge below this forward speed / reverse */
#define LA_AIRBORNE_MAX 5          /* pause after this many CONSECUTIVE fully-airborne
                                    * ticks (wheel_contact_bitmask==0x0F = all four
                                    * wheels OFF the ground), RESET the instant a
                                    * wheel touches down. NOTE: must NOT use
                                    * actor->airborne_frame_counter — the engine
                                    * never resets that, so it crosses any threshold
                                    * early and gates the aid off for the rest of the
                                    * race. Curbs/bumps (1-4 ticks) don't trip it; a
                                    * jump/launch does, and it clears on landing.    */
/* Steering-fade band. The aid HELPS you steer, so it must stay on through normal
 * cornering and only back off for a hard, deliberate yank (lane change / dodge).
 * steering_command is +/-0x18000 (full lock), so 0xE000 ~ 58% and 0x17000 ~ 96%:
 * full assist up to ~58% lock, fading to zero only past ~96%. (Was 0x4000/0xC000
 * = 17%/50%, which switched the aid OFF during ordinary steering — felt like it
 * "stopped working" on any twisty section.) */
#define LA_STEER_DEAD   0xE000     /* |steer| below this -> full assist          */
#define LA_STEER_FULL   0x17000    /* |steer| above this -> assist fully yields  */

/* ---- Per-player runtime state ----------------------------------------- */
static uint8_t s_enable[TD5_MAX_HUMAN_PLAYERS];
static int32_t s_tx[TD5_MAX_HUMAN_PLAYERS];   /* eased aim point (24.8 world)   */
static int32_t s_tz[TD5_MAX_HUMAN_PLAYERS];
static uint8_t s_have[TD5_MAX_HUMAN_PLAYERS];  /* eased target valid             */
static int8_t  s_dir[TD5_MAX_HUMAN_PLAYERS];   /* last assist dir: -1/0/+1 (HUD) */
static int8_t  s_active[TD5_MAX_HUMAN_PLAYERS]; /* 1 = steering this tick; 0 =
                                                 * paused (airborne/crash/slow/
                                                 * hard-steer) — drives the HUD
                                                 * "active vs paused" badge */
static int32_t s_prev_herr[TD5_MAX_HUMAN_PLAYERS]; /* last tick's heading error  */
static int16_t s_air[TD5_MAX_HUMAN_PLAYERS];   /* consecutive no-ground-contact ticks
                                                * (resets on any wheel contact)    */

/* atan2 in the game's 12-bit heading convention (0=+Z, 0x400=+X), matching
 * ai_angle_from_vector / AngleFromVector12: the returned angle a satisfies
 * forward=(sin a, cos a) ~ (dx,dz). */
static int32_t la_angle_from_vector(int32_t dx, int32_t dz)
{
    double a;
    if (dx == 0 && dz == 0) return 0;
    a = atan2((double)dx, (double)dz);
    return ((int32_t)(a * (4096.0 / (2.0 * 3.14159265358979323846)))) & 0xFFF;
}

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !e[0]) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static void laneassist_init_once(void)
{
    int i;
    if (s_inited) return;
    s_inited = 1;

    s_master_enable = (g_td5.ini.lane_assist != 0);
    s_strength_pct  = env_int("TD5RE_LANEASSIST_STRENGTH",       900, 0, 1000);
    s_lookahead     = env_int("TD5RE_LANEASSIST_LOOKAHEAD_SPANS",  6, 1, 8);
    s_max_yaw       = env_int("TD5RE_LANEASSIST_MAX_YAW",        400, 0, 800);
    s_fork_commit   = env_int("TD5RE_LANEASSIST_FORK_COMMIT",      1, 0, 1);
    s_lane_band     = env_int("TD5RE_LANEASSIST_LANE_BAND",        0, 0, 4);

    for (i = 0; i < TD5_MAX_HUMAN_PLAYERS; i++) {
        s_enable[i]    = (uint8_t)s_master_enable;   /* per-session default */
        s_have[i]      = 0;
        s_dir[i]       = 0;
        s_active[i]    = 0;
        s_prev_herr[i] = 0;
        s_air[i]       = 0;
    }
    TD5_LOG_I(LOG_TAG,
        "init: master=%d strength=%d%% lookahead=%d max_yaw=%d fork_commit=%d lane_band=%d",
        s_master_enable, s_strength_pct, s_lookahead, s_max_yaw, s_fork_commit, s_lane_band);
}

void td5_laneassist_reset(void)
{
    int i;
    for (i = 0; i < TD5_MAX_HUMAN_PLAYERS; i++)
        s_have[i] = 0;
}

void td5_laneassist_toggle(int player)
{
    laneassist_init_once();
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    s_enable[player] = (uint8_t)!s_enable[player];
    /* Re-enable from a clean slate so a prior crash/pause can never leave the aid
     * wedged: drop the eased target, the derivative history and the HUD state. */
    s_have[player]      = 0;
    s_dir[player]       = 0;
    s_active[player]    = 0;
    s_prev_herr[player] = 0;
    s_air[player]       = 0;
    TD5_LOG_I(LOG_TAG, "toggle: player=%d -> %s",
              player, s_enable[player] ? "ON" : "OFF");
}

int td5_laneassist_enabled(int player)
{
    laneassist_init_once();
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    return s_enable[player];
}

void td5_laneassist_set_player(int player, int on)
{
    laneassist_init_once();
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    s_enable[player] = (uint8_t)(on ? 1 : 0);
    /* Start from a clean slate (matches the toggle path) so a re-enable mid-
     * session can never inherit a wedged eased target / derivative history. */
    s_have[player]      = 0;
    s_dir[player]       = 0;
    s_active[player]    = 0;
    s_prev_herr[player] = 0;
    s_air[player]       = 0;
    TD5_LOG_I(LOG_TAG, "set_player: player=%d -> %s (menu)",
              player, s_enable[player] ? "ON" : "OFF");
}

int td5_laneassist_hud_dir(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    return (int)s_dir[player];
}

int td5_laneassist_hud_active(int player)
{
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    return (int)s_active[player];
}

void td5_laneassist_note_inactive(int player)
{
    /* Called by the physics dispatch when a car is routed into the stunned
     * state0f path (heavy crash / launch), where td5_laneassist_apply is NOT
     * called at all — so the HUD badge would otherwise stay stuck "active". */
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    s_active[player] = 0;
    s_dir[player]    = 0;
}

void td5_laneassist_apply(TD5_Actor *actor, int32_t sin_h, int32_t cos_h)
{
    int     player;
    int     fresh = 0;                            /* target just (re)seeded?      */
    int32_t vx, vz, v_long;
    int32_t raw_x, raw_z, tx, tz;
    int32_t cx, cz, dx, dz, lat_err;

    if (!actor) return;
    laneassist_init_once();

    player = (int)actor->slot_index;             /* driving identity: player==slot */
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;
    if (!s_enable[player]) return;
    s_dir[player]    = 0;                         /* cleared unless we nudge below */
    s_active[player] = 0;                         /* "paused" until we apply below */

    /* Diagnostic (env-gated, slot 0, once): sweep the look-ahead target across a
     * span range so a trace run can confirm continuity across fork/merge spans.
     * TD5RE_LANEASSIST_SWEEP="start[,count]". Inert unless set. */
    if (player == 0) {
        static int s_sweep_done = 0;
        if (!s_sweep_done) {
            const char *e = getenv("TD5RE_LANEASSIST_SWEEP");
            s_sweep_done = 1;
            if (e && e[0]) {
                const char *comma = strchr(e, ',');
                int start = atoi(e);
                int count = comma ? atoi(comma + 1) : 64;
                td5_track_laneassist_sweep_diag(start, count,
                                                s_lookahead, s_fork_commit, s_lane_band);
            }
        }
    }

    vx = actor->linear_velocity_x;
    vz = actor->linear_velocity_z;
    v_long = (vx * sin_h + vz * cos_h) >> 12;     /* forward speed (24.8/tick) */

    /* CURRENT airborne state: wheel_contact_bitmask == 0x0F means all four wheels
     * are OFF the ground (the engine's "fully airborne" flag — see the FF landing
     * detector, td5_physics.c:1464); 0 means grounded. Count consecutive
     * fully-airborne ticks and reset the instant a wheel touches down. (Do NOT use
     * actor->airborne_frame_counter — the engine never resets that, so it would
     * gate the aid off permanently mid-race.) */
    if (actor->wheel_contact_bitmask == 0x0F) {
        if (s_air[player] < 0x7FFF) s_air[player]++;
    } else {
        s_air[player] = 0;
    }

    /* Only assist while the car is in normal forward driving on the ground.
     * Steering a genuinely airborne / crash-launched car (no traction) is
     * pointless and would spin its heading mid-air, so pause until it settles —
     * the aid then resumes on its own the moment a wheel touches down again
     * (re-seeding the target fresh, so there is no yank on resume). The gate
     * reason is logged (slot 0, throttled) so a "stops working" report pins the
     * exact gate. */
    {
        const char *gate = NULL;
        if (actor->finish_time != 0)            gate = "finished";
        else if (s_air[player] >= LA_AIRBORNE_MAX) gate = "airborne";
        else if (v_long < LA_MIN_VLONG)         gate = "slow/reverse";
        if (gate) {
            s_have[player] = 0;
            s_active[player] = 0;
            return;
        }
    }

    /* Build the continuous look-ahead lane-centre target (the real work). */
    if (!td5_track_laneassist_target((int)actor->track_span_raw,
                                     (int)actor->track_sub_lane_index,
                                     s_lookahead, s_fork_commit, s_lane_band,
                                     &raw_x, &raw_z)) {
        s_have[player] = 0;
        s_active[player] = 0;
        return;
    }

    /* Ease the aim point over N ticks/spans (merge smoothing + anti-yank).
     * Snap on a large jump (car reset / new race) instead of sweeping across
     * the whole map. `fresh` (no prior target — e.g. resuming after a pause)
     * suppresses the derivative term below so resume never spikes the steer. */
    fresh = !s_have[player];
    if (s_have[player]) {
        int32_t jx = raw_x - s_tx[player];
        int32_t jz = raw_z - s_tz[player];
        int32_t ax = jx < 0 ? -jx : jx;
        int32_t az = jz < 0 ? -jz : jz;
        if (ax > LA_SNAP || az > LA_SNAP) {
            s_tx[player] = raw_x;
            s_tz[player] = raw_z;
        } else {
            s_tx[player] += jx >> LA_EASE_SHIFT;
            s_tz[player] += jz >> LA_EASE_SHIFT;
        }
    } else {
        s_tx[player] = raw_x;
        s_tz[player] = raw_z;
        s_have[player] = 1;
    }
    tx = s_tx[player];
    tz = s_tz[player];

    /* Cross-track error (kept for the trace/HUD only): the steering itself is
     * driven by the heading error below (pure-pursuit), which ALSO corrects a
     * parallel-but-offset car because the look-ahead aim point then sits to the
     * side. dx/dz are world-POSITION deltas (up to millions in 24.8), so the
     * dx*cos_h products must be 64-bit to avoid overflow before the subtraction. */
    cx = actor->world_pos.x;
    cz = actor->world_pos.z;
    dx = tx - cx;
    dz = tz - cz;
    lat_err = (int32_t)(((int64_t)dx * cos_h - (int64_t)dz * sin_h) >> 12);

    {
        int32_t steer = actor->steering_command;
        int32_t s_abs = steer < 0 ? -steer : steer;
        int32_t fade_q8;
        int32_t heading, des_h, herr, d_herr, yaw_p, yaw_d, yaw_ct, yaw, ae, cap;

        /* Never overrides input: deliberate steering fully disables the aid
         * (linear fade between LA_STEER_DEAD and LA_STEER_FULL). */
        if (s_abs >= LA_STEER_FULL) {
            s_active[player] = 0;
            return;
        }
        if (s_abs > LA_STEER_DEAD) {
            int32_t band = LA_STEER_FULL - LA_STEER_DEAD;
            fade_q8 = (256 * (band - (s_abs - LA_STEER_DEAD))) / band;
        } else {
            fade_q8 = 256;
        }

        /* Heading error to the (far) look-ahead aim point (pure-pursuit): the
         * signed 12-bit angle between the car's heading and the direction to the
         * target. A FAR aim point (s_lookahead) keeps this small and makes the
         * correction anticipatory, which is the main thing that stops the aid
         * from over-shooting the lane and weaving. */
        heading = (actor->euler_accum.yaw >> 8) & 0xFFF;
        des_h   = la_angle_from_vector(dx, dz);
        herr    = (des_h - heading) & 0xFFF;
        if (herr >= 0x800) herr -= 0x1000;        /* wrap to [-2048, +2048] */

        /* PD STEERING on the yaw rate (same place the handling yaw torque lands).
         *   yaw_p : proportional — turn toward the lane.
         *   yaw_d : derivative on the ERROR rate — as the heading error closes,
         *           d_herr is negative and trims the push, so a firm gain settles
         *           on the line instead of over-rotating into a weave. Damping the
         *           error rate (not the absolute yaw rate) means it does NOT fight
         *           your own steering through a corner. Capped well below the
         *           handling yaw-torque clamp (±1400) as a hard ceiling. There is
         *           deliberately NO raw lateral-velocity push: injecting sideways
         *           speed pumped v_lat and threw the car into a spin-out; a
         *           steering aid STEERS and lets the car's motion carry it on. */
        d_herr = fresh ? 0 : (herr - s_prev_herr[player]);
        s_prev_herr[player] = herr;
        if (d_herr >  LA_YAW_DCLAMP) d_herr =  LA_YAW_DCLAMP;  /* reject kicks */
        if (d_herr < -LA_YAW_DCLAMP) d_herr = -LA_YAW_DCLAMP;

        ae = lat_err < 0 ? -lat_err : lat_err;
        /* LINEAR cross-track pull — the correction climbs CONTINUOUSLY the further
         * off the road centre you are (this is the main steering force). */
        yaw_ct = (int32_t)(((int64_t)lat_err * s_strength_pct) / LA_CT_DIV);
        /* Modest heading anticipation so corners are taken a touch early (kept
         * small so it doesn't dominate / saturate the smooth distance ramp). */
        yaw_p  = (herr * s_strength_pct) / LA_HEAD_DIV;
        /* Light derivative damping so it settles instead of weaving. */
        yaw_d  = d_herr * LA_YAW_DKD;
        /* Yaw cap RISES with distance so the far end of the ramp isn't clipped. */
        cap = s_max_yaw + (int32_t)((int64_t)ae / LA_FAR_DIV);
        if (cap > LA_FAR_MAX) cap = LA_FAR_MAX;

        yaw   = yaw_ct + yaw_p + yaw_d;
        yaw   = (yaw * fade_q8) >> 8;
        if (yaw >  cap) yaw =  cap;
        if (yaw < -cap) yaw = -cap;
        actor->angular_velocity_yaw += yaw;

        /* HUD: actively assisting this tick, and which way it is steering. */
        s_active[player] = 1;
        s_dir[player] = (yaw > 4) ? 1 : (yaw < -4 ? -1 : 0);
    }
}
