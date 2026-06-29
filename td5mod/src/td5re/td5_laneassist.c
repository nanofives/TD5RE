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
#include "td5re.h"          /* g_td5 (ini.lane_assist, drag_race_enabled) */
#include "td5_platform.h"   /* TD5_LOG_* */
#include "td5_input.h"      /* td5_input_drag_target_lane (drag chosen lane) */
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
static int s_fork_diverge  = 1;    /* TD5RE_LANEASSIST_FORK_DIVERGE (0/1): aim at
                                    * the committed branch's lanes on the approach
                                    * to a fork (diverge early, not at the divider) */
static int s_lane_band     = 0;    /* TD5RE_LANEASSIST_LANE_BAND: 0 = centre of ALL
                                    * drivable lanes (whole paved road, default);
                                    * 1 = single lane; 2 = centre of two; etc.     */

/* [DRAG] Aggressive auto-lane-change variant. In a drag race the aid is FORCE-ON
 * for every human and steers toward the player's CHOSEN lane (L/R taps, via
 * td5_input_drag_target_lane) instead of the current one — band 1 (that single
 * lane), stronger gain and a higher yaw cap so each lane change is decisive,
 * shorter look-ahead so it commits rather than drifting. Same robust PD/no-spin
 * machinery as the accessibility aid. */
static int s_drag_strength  = 1200; /* TD5RE_DRAG_LA_STRENGTH (cross-track pull gain) */
static int s_drag_maxyaw    = 1500; /* TD5RE_DRAG_LA_MAXYAW (drag-only yaw cap/tick;
                                     * higher = faster lane change. Not bound by the
                                     * normal aid's LA_FAR_MAX ceiling.)            */
static int s_drag_lookahead = 6;    /* TD5RE_DRAG_LA_LOOKAHEAD (farther = gentler)  */
static int s_drag_band      = 1;    /* aim at the single chosen lane              */
static int s_drag_rate_damp = 70;   /* TD5RE_DRAG_RATE_DAMP (q8: yaw-loop damping —
                                     * lighter now that perp-velocity does the arrest) */
static int s_drag_vlat_damp = 28;   /* TD5RE_DRAG_VLAT_DAMP: LANE-RELATIVE lateral-
                                     * velocity damping (×/LA_VLAT_DIV). The car has ~no
                                     * body slip, so its approach speed onto the lane is
                                     * the velocity component PERPENDICULAR to the lane
                                     * (= v_long·sin(heading−lane_dir)); damping it lets a
                                     * strong pull arrive with ~0 lane-lateral speed
                                     * instead of overshooting. Higher = brakes earlier. */

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
/* [DRAG settle] At drag's extreme speed the aggressive cross-track pull forms a
 * 1-tick limit cycle once the car is on the lane (physics over-responds to each
 * nudge -> shake). When within LA_DRAG_DEAD_CT of the chosen lane AND nearly
 * aligned, switch to a critically-damped hold: cancel the current yaw RATE and
 * add only a gentle heading-straighten term, so the heading eases straight and
 * STOPS instead of oscillating. (~13% of a ~371540-unit lane.) */
#define LA_DRAG_DEAD_CT    50000   /* |lat_err|(24.8) below this -> settle hold     */
#define LA_DRAG_DEAD_HERR  60      /* AND |heading err| below this (12-bit) -> hold */
#define LA_DRAG_DEAD_VEL   2500    /* AND lane-perp approach speed below this -> hold
                                    * (else stay in the braking PD; don't coast past) */
#define LA_DRAG_SETTLE_CAP 2000    /* settle-yaw clamp (must cover the limit cycle)  */
#define LA_VLAT_DIV        1000     /* v_lat * VLAT_DAMP / this -> deceleration yaw   */
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
    s_fork_diverge  = env_int("TD5RE_LANEASSIST_FORK_DIVERGE",     1, 0, 1);
    s_lane_band     = env_int("TD5RE_LANEASSIST_LANE_BAND",        0, 0, 4);
    s_drag_strength  = env_int("TD5RE_DRAG_LA_STRENGTH",  1200, 0, 8000);
    s_drag_maxyaw    = env_int("TD5RE_DRAG_LA_MAXYAW",    1500, 0, 4000);
    s_drag_lookahead = env_int("TD5RE_DRAG_LA_LOOKAHEAD",    6, 1, 8);
    s_drag_rate_damp = env_int("TD5RE_DRAG_RATE_DAMP",      70, 0, 256);
    s_drag_vlat_damp = env_int("TD5RE_DRAG_VLAT_DAMP",      80, -2000, 2000);

    for (i = 0; i < TD5_MAX_HUMAN_PLAYERS; i++) {
        s_enable[i]    = (uint8_t)s_master_enable;   /* per-session default */
        s_have[i]      = 0;
        s_dir[i]       = 0;
        s_active[i]    = 0;
        s_prev_herr[i] = 0;
        s_air[i]       = 0;
    }
    TD5_LOG_I(LOG_TAG,
        "init: master=%d strength=%d%% lookahead=%d max_yaw=%d fork_commit=%d fork_diverge=%d lane_band=%d",
        s_master_enable, s_strength_pct, s_lookahead, s_max_yaw, s_fork_commit,
        s_fork_diverge, s_lane_band);
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
    int32_t vx, vz, v_long, v_lat;
    int32_t raw_x, raw_z, tx, tz;
    int32_t cx, cz, dx, dz, lat_err;
    int32_t perp_vel = 0;                        /* lane-perpendicular approach speed */
    int     la_lane, la_band, la_lookahead, la_strength, la_maxyaw; /* drag overrides */
    int     la_is_drag;                          /* aggressive drag lane-change mode */

    if (!actor) return;
    laneassist_init_once();

    player = (int)actor->slot_index;             /* driving identity: player==slot */
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return;

    /* [DRAG] Aggressive auto-lane-change: in a drag race the aid is FORCE-ON for
     * every human and aims at the player's CHOSEN lane (L/R taps) with stronger
     * gain / yaw cap / single-lane band and a shorter look-ahead, reusing all the
     * robust PD / no-spin machinery below. Outside drag these stay the normal
     * accessibility-aid values. */
    {
        la_is_drag = (g_td5.drag_race_enabled && player < g_td5.num_human_players);
        la_lane      = (int)actor->track_sub_lane_index;
        la_band      = s_lane_band;
        la_lookahead = s_lookahead;
        la_strength  = s_strength_pct;
        la_maxyaw    = s_max_yaw;
        if (la_is_drag) {
            int chosen = td5_input_drag_target_lane(player);
            if (chosen >= 0) la_lane = chosen;
            la_band      = s_drag_band;
            la_lookahead = s_drag_lookahead;
            la_strength  = s_drag_strength;
            la_maxyaw    = s_drag_maxyaw;
        }
        if (!s_enable[player] && !la_is_drag) return;
    }
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
                td5_track_laneassist_sweep_diag(start, count, s_lookahead,
                                                s_fork_commit, s_fork_diverge, s_lane_band);
            }
        }
    }

    vx = actor->linear_velocity_x;
    vz = actor->linear_velocity_z;
    v_long = (vx * sin_h + vz * cos_h) >> 12;     /* forward speed (24.8/tick) */
    v_lat  = (vx * cos_h - vz * sin_h) >> 12;     /* lateral speed (body frame, same
                                                   * frame as lat_err) — drag uses it
                                                   * to decelerate onto the lane     */

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
            if (player == 0 && (actor->frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG, "GATED: %s (air=%d wcm=0x%X v_long=%d finish=%d span=%d)",
                          gate, (int)s_air[player], (unsigned)actor->wheel_contact_bitmask,
                          v_long, (int)actor->finish_time, (int)actor->track_span_raw);
            return;
        }
    }

    /* Build the continuous look-ahead lane-centre target (the real work). */
    if (!td5_track_laneassist_target((int)actor->track_span_raw,
                                     la_lane,
                                     la_lookahead, s_fork_commit, s_fork_diverge,
                                     la_band, &raw_x, &raw_z)) {
        s_have[player] = 0;
        s_active[player] = 0;
        if (player == 0 && (actor->frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG, "GATED: no_target (span=%d sub=%d v_long=%d)",
                      (int)actor->track_span_raw,
                      (int)actor->track_sub_lane_index, v_long);
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

    /* [DRAG] Lane-PERPENDICULAR approach speed. The car barely slips in its own body
     * frame (v_lat≈0); its motion toward/away from the lane is the velocity component
     * perpendicular to the LANE direction. Lane direction = from the car's chosen-lane
     * centre at this span to the look-ahead aim point; perp velocity = the 2D cross of
     * the world velocity with that (unit) lane vector. Same rightward sign as lat_err,
     * so a simple PD (pull − perp_vel damp) decelerates onto the lane. */
    if (la_is_drag) {
        int lx0 = 0, ly0 = 0, lz0 = 0;
        if (td5_track_get_span_lane_world((int)actor->track_span_raw, la_lane,
                                          &lx0, &ly0, &lz0)) {
            double lvx = (double)tx - lx0, lvz = (double)tz - lz0;
            double llen = sqrt(lvx * lvx + lvz * lvz);
            if (llen > 1.0)
                perp_vel = (int32_t)(((double)vx * lvz - (double)vz * lvx) / llen);
        }
    }

    {
        int32_t steer = actor->steering_command;
        int32_t s_abs = steer < 0 ? -steer : steer;
        int32_t fade_q8;
        int32_t heading, des_h, herr, d_herr, yaw_p, yaw_d, yaw_ct, yaw, ae, cap;

        /* Never overrides input: deliberate steering fully disables the aid
         * (linear fade between LA_STEER_DEAD and LA_STEER_FULL). */
        if (s_abs >= LA_STEER_FULL) {
            s_active[player] = 0;
            if (player == 0 && (actor->frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG, "GATED: steering (steer=%d)", (int)steer);
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
        yaw_ct = (int32_t)(((int64_t)lat_err * la_strength) / LA_CT_DIV);
        /* Modest heading anticipation so corners are taken a touch early (kept
         * small so it doesn't dominate / saturate the smooth distance ramp). */
        yaw_p  = (herr * la_strength) / LA_HEAD_DIV;
        /* Light derivative damping so it settles instead of weaving. */
        yaw_d  = d_herr * LA_YAW_DKD;
        /* Yaw cap RISES with distance so the far end of the ramp isn't clipped. */
        cap = la_maxyaw + (int32_t)((int64_t)ae / LA_FAR_DIV);
        if (cap > LA_FAR_MAX) cap = LA_FAR_MAX;

        if (la_is_drag && ae < LA_DRAG_DEAD_CT &&
            herr < LA_DRAG_DEAD_HERR && herr > -LA_DRAG_DEAD_HERR &&
            perp_vel < LA_DRAG_DEAD_VEL && perp_vel > -LA_DRAG_DEAD_VEL) {
            /* SETTLED on the chosen lane -> critically-damped centring hold. Pull to
             * the lane CENTRE with the cross-track term (yaw_ct), NOT the heading
             * error: herr -> 0 as soon as the car points straight even if it is
             * parked off-centre, so a herr-only hold settled ~11% off the lane and
             * crept. The FULL yaw-rate cancel (-angular_velocity_yaw) leaves no
             * rotational momentum, so it centres without the limit-cycle shake.
             * NOTE the perp_vel gate above: don't enter the hold while still moving
             * across the lane, or it would coast past (the hold doesn't brake drift).*/
            yaw = yaw_ct - actor->angular_velocity_yaw;
            if (yaw >  LA_DRAG_SETTLE_CAP) yaw =  LA_DRAG_SETTLE_CAP;
            if (yaw < -LA_DRAG_SETTLE_CAP) yaw = -LA_DRAG_SETTLE_CAP;
        } else if (la_is_drag) {
            /* Drag lane change — PD on the LANE-RELATIVE lateral state:
             *   + yaw_ct  : strong cross-track pull toward the lane (the restoring
             *               force — without it the car ran away across all lanes).
             *   - perp_vel: decelerate as the car closes on the lane (perpendicular
             *               approach speed), so a strong pull ARRIVES with ~0 lane-
             *               lateral speed instead of over-rotating past it. This is
             *               the real "how fast am I approaching" signal (body v_lat
             *               is ~0). It lets the pull be fast AND not overshoot.
             *   - ang_vel : light yaw-loop damping for a clean (non-weaving) turn. */
            yaw  = yaw_ct + yaw_p;
            yaw  = (yaw * fade_q8) >> 8;
            yaw -= (perp_vel * s_drag_vlat_damp) / LA_VLAT_DIV;
            yaw -= (int32_t)(((int64_t)actor->angular_velocity_yaw * s_drag_rate_damp) >> 8);
            /* Drag uses its OWN higher cap (not LA_FAR_MAX): the car must point hard
             * enough to cross a lane briskly. The perp-velocity brake above still
             * arrests the (now faster) approach, so it stays overshoot-free. */
            if (yaw >  s_drag_maxyaw) yaw =  s_drag_maxyaw;
            if (yaw < -s_drag_maxyaw) yaw = -s_drag_maxyaw;
        } else {
            yaw   = yaw_ct + yaw_p + yaw_d;
            yaw   = (yaw * fade_q8) >> 8;
            if (yaw >  cap) yaw =  cap;
            if (yaw < -cap) yaw = -cap;
        }
        actor->angular_velocity_yaw += yaw;

        /* HUD: actively assisting this tick, and which way it is steering. */
        s_active[player] = 1;
        s_dir[player] = (yaw > 4) ? 1 : (yaw < -4 ? -1 : 0);

        if (player == 0 && (actor->frame_counter % 15u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                "p0 span=%d sub=%d lat_err=%d perp_vel=%d herr=%d yaw_ct=%d "
                "cap=%d yaw=%d ang_vel=%d",
                (int)actor->track_span_raw, (int)actor->track_sub_lane_index,
                lat_err, perp_vel, herr, yaw_ct, cap, yaw,
                (int)actor->angular_velocity_yaw);
        }
    }
}
