/* ========================================================================
 * td5_physics_assists.c -- PORT-ONLY vehicle-dynamics assists & tuning
 *
 * Split out of td5_physics.c (P1-B 2026-07-02). Everything in this TU is a
 * PORT ADDITION / PORT ENHANCEMENT layered on top of the faithful sim:
 * inert (identity multiplier / disabled / no-op) in its default or gated
 * state, so the byte-faithful core in td5_physics.c is unchanged when the
 * features are off. Cross-TU seam: td5_physics_internal.h (PRIVATE).
 *
 * Sections (in original core order):
 *   - Player stuck-car recovery state + manual-recovery driver
 *   - Multiplayer catch-up assist (rubber-banding for HUMAN players)
 *   - Gearbox mode flag + manual-gearbox performance boost
 *   - Uphill-slope decel scaling + hill-climb drive-torque assist
 *   - Car-WEIGHT mechanics, slipstream/draft, downforce
 *   - Force-feedback signal getters
 *   - Hard-difficulty AI catch-up
 *   - Acute crash FX + battle/police knob getters
 *   - Gentle flip-recovery + in-place recovery teleport driver
 * ======================================================================== */

#include "td5_physics.h"
#include "td5_physics_internal.h"
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"   /* td5_render_get_vehicle_mesh */
#include "td5_sound.h"    /* td5_sound_play_at_position (Tier 2 recovery SFX) */
#include "td5_input.h"    /* manual-recovery edge, FF signal consumers */
#include "td5_game.h"     /* slot counts, cop-chase role queries */
#include "td5_arcade.h"
#include "td5_damage.h"
#include "td5_platform.h"
#include "td5_config.h"   /* shared TD5RE_* env-knob accessors */
#include "td5_carparam.h" /* shared carparam field map (weight mechanics) */
#include "td5re.h"

#include <string.h>  /* memset */
#include <math.h>
#include <stdlib.h>  /* getenv, atoi */

#define LOG_TAG "physics"

/* ======== [split] stuck-recovery state (moved verbatim from td5_physics.c) ======== */
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
 *   TD5RE_RECOVERY_SPANS_BACK     default 0   — 0 = reset IN PLACE (faithful to the
 *                                               original ResetVehicleActorState
 *                                               @0x00405D70, which never moves
 *                                               world XZ); >0 = legacy step-back
 *                                               (opt-in; can teleport to the start
 *                                               or drop the car into wall geometry
 *                                               on branch tracks — see fix note on
 *                                               td5_physics_recover_player).
 *   TD5RE_RECOVERY_COOLDOWN_TICKS default 150 — manual cooldown window (ticks).
 * ======================================================================== */

/* [RESET-CAR IN-PLACE FIX 2026-06-29] Default 0 = reset the car where it stands
 * (upright + ground-snap + zero velocity), matching the original. The earlier
 * default of 3 (step back 3 spans and re-derive world XZ from track geometry) was
 * the root cause of the reported split-screen bugs: it could resolve to span 0
 * ("teleported to the beginning of the race") and could land the car inside wall
 * geometry on branch/junction spans, where the new damage-bar feature then wrecked
 * it ("complete break of car"). The original game has no step-back respawn at all
 * (RE-confirmed: no manual reset / span-step-back / checkpoint respawn). */
#define TD5_RECOVERY_DEFAULT_BACK   0
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
/* recovery_gentle_for_actor / td5_physics_gentle_recovery_coast: extern,
 * declared in td5_physics_internal.h (the core recovery path calls them). */


/* ======== [split] tuning assists: MP catch-up .. acute crash FX (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * Multiplayer catch-up assist (rubber-banding for HUMAN players) — PORT-ONLY,
 * NON-FAITHFUL, opt-in. [MP CATCHUP 2026-06-14; reworked 2026-06-24]
 *
 * In a multiplayer race (2+ HUMAN players, split-screen OR net) each human paces
 * off the NEXT OPPONENT IMMEDIATELY AHEAD of it on track (the racer with the
 * smallest POSITIVE track-progress gap, human or AI). The effect is a smooth,
 * INCREMENTAL function of that gap:
 *   - FAR behind the car ahead  -> raise BOTH the top-speed cap AND drive torque
 *                                  (acceleration) so the trailing human rejoins
 *                                  the pack (classic catch-up). Boost ramps in
 *                                  with the gap and clamps at FULL_GAP.
 *   - CLOSING IN on the car ahead (gap below the NEUTRAL window) -> lower the
 *                                  TOP-SPEED CAP ONLY, ramping down to a floor at
 *                                  gap 0. Acceleration is NOT cut. Because the
 *                                  speed-limit gate in the drive paths zeroes
 *                                  drive force WITHOUT braking when over the cap
 *                                  (td5_physics_update_player / _ai), the car
 *                                  simply COASTS down to the lower cap and keeps
 *                                  its existing speed — it never decelerates
 *                                  under power. So a closing human settles in
 *                                  behind the car ahead instead of ramming it.
 *   - LEADER (nobody ahead)      -> neutral (1.0), runs at its normal pace.
 * This is the classic arcade "catch-up" — NOT the netcode/lag resync (td5_net.c).
 *
 * DETERMINISM (critical — this game runs LOCKSTEP UDP netplay):
 *   - Both per-slot multipliers are a PURE FUNCTION of replicated simulation
 *     state: every racer's track_span_high_water (+0x086, the same monotonic
 *     forward span counter UpdateRaceOrder @0x0042F5B0 sorts standings by) and
 *     the replicated per-slot human/AI table g_race_slot_state[]. Both are
 *     identical on every client every tick, so every client computes the same
 *     multipliers.
 *   - Recomputed once per DETERMINISTIC fixed-30Hz sim tick (top of
 *     td5_physics_tick). The acceleration half is applied in the integer
 *     drive-torque pipeline (td5_physics_compute_drive_torque); the top-speed
 *     half is applied in phys_top_speed_rating() (the single cap chokepoint feeding
 *     all player + AI speed-limit gates). No rand()/wall-clock/per-viewport input.
 *
 * SCOPE GUARD: applies ONLY when >=2 human racer slots are present (split-screen
 * or net). Single-player (1 human) leaves every multiplier at 1.0, so the SP
 * experience is byte-unchanged. AI/traffic slots are never given an effect (they
 * are only consulted as the "car ahead" a human paces off).
 *
 * KNOBS (env, cached once; CLI/INI not plumbed — env is the source here):
 *   TD5RE_MP_CATCHUP           master gate, DEFAULT 1 (ON). "0"/"n"/"f" = off.
 *   TD5RE_MP_CATCHUP_STRENGTH  strength 0..100, DEFAULT 50. Scales ALL of the
 *                              top-speed boost, accel boost, and top-speed ease.
 *                              0 behaves like OFF.
 *   TD5RE_MP_CATCHUP_EASE      DEFAULT 1: enable the approach-side top-speed
 *                              reduction (coast in behind the car ahead). "0" =
 *                              boost-when-behind only, never lower the cap.
 *                              (Legacy alias: TD5RE_MP_CATCHUP_LEADER.)
 *   TD5RE_MP_CATCHUP_FORCE     DEFAULT 0 (TEST ONLY): drop the >=2-human scope
 *                              guard to >=1 so a SINGLE player paces off the AI
 *                              field — lets the mechanic be observed in a
 *                              single-player trace. NOT for shipping play.
 * ======================================================================== */

/* Drive-force / top-speed multiplier is Q8 fixed-point (0x100 = 1.0). */
/* (moved to td5_physics_internal.h) MP_CATCHUP_Q8_ONE */

/* Gap (in track spans, via track_span_high_water) to the next opponent AHEAD at
 * which a trailing human reaches the FULL catch-up boost. Between NEUTRAL_GAP and
 * this the boost ramps in linearly; at/above it the boost is clamped. Deterministic
 * constant (same on every client). */
#define MP_CATCHUP_FULL_GAP_SPANS  30

/* Crossover gap. At/below this the human is "closing in" on the car ahead and the
 * TOP-SPEED cap eases DOWN (coast in). Above it a catch-up boost ramps in. ~12
 * spans ≈ a comfortable following distance of a few car lengths on a TD5 strip. */
#define MP_CATCHUP_NEUTRAL_GAP_SPANS  12

/* Maximum ACCELERATION (drive-torque) boost at full gap + 100% strength, Q8.
 * 0x100 + 0x60 = 1.375x drive force at the extreme; at the default 50% strength
 * + full gap the realised cap is 0x100 + (0x60*50/100) ≈ 1.19x — a firm pull,
 * not a warp. Accel is boosted ONLY when behind (never cut). */
#define MP_CATCHUP_MAX_ACCEL_BOOST_Q8  0x60

/* Maximum TOP-SPEED cap boost at full gap + 100% strength, Q8. Gentler than the
 * accel boost (a small top-end lift to rejoin the pack, not a warp): 0x100 + 0x30
 * = 1.1875x at the extreme; ≈ 1.09x at the 50% default + full gap. */
#define MP_CATCHUP_MAX_TS_BOOST_Q8     0x30

/* Maximum TOP-SPEED cap CUT at gap 0 + 100% strength, Q8 (subtracted from 1.0 as
 * the human bears down on the car ahead so it settles in behind and coasts).
 * 0x100 - 0x30 = 0.8125x at the extreme; ≈ 0.91x at the 50% default. Acceleration
 * is never cut here — only the cap — so the car coasts, it does not decelerate. */
#define MP_CATCHUP_MAX_TS_CUT_Q8       0x30

/* Resolved config (lazy, cached). s_mp_catchup_cfg: -1 = unresolved, 0 = off,
 * 1 = on. s_mp_catchup_strength: 0..100. s_mp_catchup_ease: 0/1. s_mp_catchup_force:
 * 0/1 (TEST: drop the 2-human guard). */
static int      s_mp_catchup_cfg      = -1;
static int      s_mp_catchup_strength = 0;
static int      s_mp_catchup_ease     = 1;
static int      s_mp_catchup_force    = 0;

/* Per-racer-slot ACCELERATION (drive-torque) multiplier, Q8 (0x100 = 1.0 = no
 * change). Written once per sim tick by td5_physics_update_mp_catchup(); read in
 * td5_physics_compute_drive_torque(). Default 1.0 so it is inert until the
 * feature is enabled AND >=2 humans are racing. */
static int32_t  s_mp_catchup_mult[TD5_MAX_RACER_SLOTS];

/* Per-racer-slot TOP-SPEED cap multiplier, Q8 (0x100 = 1.0 = no change). Written
 * alongside s_mp_catchup_mult; read in phys_top_speed_rating(). >1.0 when a human
 * is far behind the car ahead (rejoin), <1.0 when closing in (coast). 1.0 for
 * AI/traffic, single-player, and disabled feature. */
static int32_t  s_mp_catchup_ts_mult[TD5_MAX_RACER_SLOTS];

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

    /* Strength 0..100, default 50 (scales the top-speed boost, accel boost and
     * top-speed ease together). */
    s_mp_catchup_strength = td5_env_int("TD5RE_MP_CATCHUP_STRENGTH", 50, 0, 100);

    /* Approach-side top-speed ease on by default. Accept the legacy LEADER alias
     * (its old "throttle cars ahead" meaning is the closest analog). */
    s_mp_catchup_ease = 1;
    e = getenv("TD5RE_MP_CATCHUP_EASE");
    if (!e || !e[0])
        e = getenv("TD5RE_MP_CATCHUP_LEADER");   /* legacy alias */
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        s_mp_catchup_ease = 0;

    /* TEST-ONLY: drop the >=2-human scope guard to >=1 so a single player paces
     * off the AI field (lets the mechanic be traced in single-player). OFF by
     * default — shipping play always requires 2+ humans. */
    s_mp_catchup_force = 0;
    e = getenv("TD5RE_MP_CATCHUP_FORCE");
    if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y' || e[0] == 't' || e[0] == 'T'))
        s_mp_catchup_force = 1;

    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_mp_catchup_mult[i]    = MP_CATCHUP_Q8_ONE;
        s_mp_catchup_ts_mult[i] = MP_CATCHUP_Q8_ONE;
    }

    TD5_LOG_I(LOG_TAG,
              "MP catchup: %s strength=%d%% ease=%s force=%s "
              "(env TD5RE_MP_CATCHUP / _STRENGTH / _EASE / _FORCE; ref=next-opp-ahead, "
              "neutral_gap=%d full_gap=%d spans, ts_boost=%d ts_cut=%d accel_boost=%d /256)",
              s_mp_catchup_cfg ? "ON" : "OFF (default)",
              s_mp_catchup_strength,
              s_mp_catchup_ease ? "on" : "off",
              s_mp_catchup_force ? "ON(test)" : "off",
              MP_CATCHUP_NEUTRAL_GAP_SPANS, MP_CATCHUP_FULL_GAP_SPANS,
              MP_CATCHUP_MAX_TS_BOOST_Q8, MP_CATCHUP_MAX_TS_CUT_Q8,
              MP_CATCHUP_MAX_ACCEL_BOOST_Q8);
}

/* Recompute the per-slot catch-up multipliers (ACCEL + TOP-SPEED) for THIS sim
 * tick.
 *
 * Pure function of replicated sim state (track_span_high_water + g_race_slot_state),
 * so it is lockstep-deterministic. Called once at the top of td5_physics_tick().
 *
 * Algorithm (reference = the NEXT OPPONENT IMMEDIATELY AHEAD, not the leader):
 *   - Need >=2 human racer slots (or TD5RE_MP_CATCHUP_FORCE for a 1-human trace);
 *     otherwise every multiplier stays 1.0 and we return (SP byte-unchanged).
 *   - For each HUMAN slot find gap_ahead = min over racers strictly ahead
 *     (their high_water > mine) of (their_progress - my_progress). Humans AND AI
 *     are candidates for "the car ahead"; the EFFECT is applied to humans only.
 *   - No car ahead (leader) => neutral (1.0 / 1.0).
 *   - gap_ahead >= NEUTRAL_GAP (behind, room to close):
 *       t      = min(gap_ahead, FULL_GAP) - NEUTRAL_GAP   in [0, FULL_GAP-NEUTRAL]
 *       accel  = 1.0 + MAX_ACCEL_BOOST * strength/100 * t/(FULL_GAP-NEUTRAL)
 *       ts     = 1.0 + MAX_TS_BOOST    * strength/100 * t/(FULL_GAP-NEUTRAL)
 *   - gap_ahead < NEUTRAL_GAP (closing in):
 *       u      = NEUTRAL_GAP - gap_ahead                  in (0, NEUTRAL_GAP)
 *       accel  = 1.0                                      (NEVER cut — no decel)
 *       ts     = 1.0 - MAX_TS_CUT * strength/100 * u/NEUTRAL_GAP   (ease/coast)
 *     The ts cut needs TD5RE_MP_CATCHUP_EASE on; off => ts stays 1.0.
 *
 * Everything is integer Q8; no float, no rounding surprises across clients. */
void td5_physics_update_mp_catchup(void)
{
    static int s_mp_catchup_log_tick = 0;   /* ~1 Hz log gate (rate-limit) */
    int slot, total, racer_cap, human_count, min_humans, do_log;

    td5_physics_mp_catchup_config();

    /* Reset to neutral up front so disabled/early-return paths are inert. */
    for (slot = 0; slot < TD5_MAX_RACER_SLOTS; slot++) {
        s_mp_catchup_mult[slot]    = MP_CATCHUP_Q8_ONE;
        s_mp_catchup_ts_mult[slot] = MP_CATCHUP_Q8_ONE;
    }

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

    /* Pass 1: count humans. */
    human_count = 0;
    for (slot = 0; slot < racer_cap; slot++) {
        if (g_race_slot_state[slot] == 1)
            human_count++;
    }

    /* SCOPE GUARD: only engage with 2+ humans (split-screen or net MP). The TEST
     * force knob lowers the bar to 1 so the mechanic can be traced single-player. */
    min_humans = s_mp_catchup_force ? 1 : 2;
    if (human_count < min_humans)
        return;

    do_log = ((s_mp_catchup_log_tick % 30) == 0);
    s_mp_catchup_log_tick++;

    /* Pass 2: per-human multipliers from the gap to the NEXT opponent ahead. */
    for (slot = 0; slot < racer_cap; slot++) {
        TD5_Actor *a;
        int32_t my_prog, gap_ahead;
        int j;

        if (g_race_slot_state[slot] != 1)
            continue;  /* effect applied to humans only */

        a = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        my_prog = (int32_t)a->track_span_high_water;

        /* Car immediately ahead = smallest POSITIVE progress gap over ALL racer
         * slots (humans + AI), excluding self. */
        gap_ahead = INT32_MAX;
        for (j = 0; j < racer_cap; j++) {
            TD5_Actor *o;
            int32_t g;
            if (j == slot)
                continue;
            o = (TD5_Actor *)(g_actor_table_base + (size_t)j * TD5_ACTOR_STRIDE);
            g = (int32_t)o->track_span_high_water - my_prog;
            if (g > 0 && g < gap_ahead)
                gap_ahead = g;
        }

        if (gap_ahead == INT32_MAX)
            continue;  /* nobody ahead (leader) -> neutral */

        if (gap_ahead >= MP_CATCHUP_NEUTRAL_GAP_SPANS) {
            /* Behind with room to close: ramp BOTH boosts in with the gap,
             * clamped at FULL_GAP. */
            int32_t span = MP_CATCHUP_FULL_GAP_SPANS - MP_CATCHUP_NEUTRAL_GAP_SPANS;
            int32_t cap  = (gap_ahead < MP_CATCHUP_FULL_GAP_SPANS) ? gap_ahead
                                                                   : MP_CATCHUP_FULL_GAP_SPANS;
            int32_t t    = cap - MP_CATCHUP_NEUTRAL_GAP_SPANS;             /* [0, span] */
            int32_t accel = (MP_CATCHUP_MAX_ACCEL_BOOST_Q8 * s_mp_catchup_strength * t)
                            / (100 * span);
            int32_t ts    = (MP_CATCHUP_MAX_TS_BOOST_Q8 * s_mp_catchup_strength * t)
                            / (100 * span);
            s_mp_catchup_mult[slot]    = MP_CATCHUP_Q8_ONE + accel;
            s_mp_catchup_ts_mult[slot] = MP_CATCHUP_Q8_ONE + ts;
        } else if (s_mp_catchup_ease) {
            /* Closing in: lower the TOP-SPEED cap only (coast in behind). Accel
             * is NEVER cut, so the car keeps its speed and coasts — no decel. */
            int32_t u   = MP_CATCHUP_NEUTRAL_GAP_SPANS - gap_ahead;        /* (0, NEUTRAL) */
            int32_t cut = (MP_CATCHUP_MAX_TS_CUT_Q8 * s_mp_catchup_strength * u)
                          / (100 * MP_CATCHUP_NEUTRAL_GAP_SPANS);
            s_mp_catchup_ts_mult[slot] = MP_CATCHUP_Q8_ONE - cut;
            /* s_mp_catchup_mult[slot] stays 1.0 (no accel cut). */
        }

        if (do_log) {
            TD5_LOG_I(LOG_TAG,
                      "mp_catchup: slot=%d gap_ahead=%d accel=%d/256 topspeed=%d/256",
                      slot, gap_ahead,
                      (int)s_mp_catchup_mult[slot], (int)s_mp_catchup_ts_mult[slot]);
        }
    }
}

/* Q8 catch-up ACCELERATION multiplier for `slot` (0x100 = 1.0). Returns 1.0 for
 * non-racer slots / out of range so callers can apply it unconditionally. */
int32_t td5_physics_mp_catchup_mult(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return MP_CATCHUP_Q8_ONE;
    return s_mp_catchup_mult[slot];
}

/* Q8 catch-up TOP-SPEED cap multiplier for `slot` (0x100 = 1.0). >1.0 = cap
 * raised (far behind the car ahead, rejoining); <1.0 = cap lowered (closing in,
 * coast). 1.0 for non-racer / out-of-range / inert feature so phys_top_speed_rating
 * can apply it unconditionally. */
int32_t td5_physics_mp_catchup_ts_mult(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS)
        return MP_CATCHUP_Q8_ONE;
    return s_mp_catchup_ts_mult[slot];
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
int td5_physics_actor_is_manual_gearbox(const TD5_Actor *actor)
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
int td5_physics_actor_should_auto_shift(const TD5_Actor *actor)
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
        pct = td5_env_int("TD5RE_MANUAL_BOOST_PCT", 20, 0, 100);
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
int32_t td5_physics_actor_manual_boost_q8(const TD5_Actor *actor)
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
int32_t td5_physics_apply_speed_limit_boost(int32_t speed_limit, int32_t q8)
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
/* (moved to td5_physics_internal.h) SLOPE_LIGHT_Q12_ONE */

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
        {
            int p = td5_env_int("TD5RE_SLOPE_DECEL_FLOOR_PCT", 45, 5, 100);
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
int32_t td5_physics_slope_light_scale_q12(int32_t top_speed)
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
    const char *e;
    if (s_hill_assist_init) return;
    e = getenv("TD5RE_HILL_ASSIST");
    if (e && e[0] == '0' && e[1] == '\0') s_hill_assist_on = 0;
    {
        int p = td5_env_int("TD5RE_HILL_ASSIST_MAX", 260, 100, 600);
        s_hill_assist_max_q12 = (4096 * p) / 100;
    }
    s_hill_assist_init = 1;
    TD5_LOG_I(LOG_TAG, "hill_assist: %s max=%d/4096 (drive-torque boost vs uphill grade)",
              s_hill_assist_on ? "on" : "off", s_hill_assist_max_q12);
}

/* Q12 drive-torque multiplier for the current uphill steepness. up_mag is the
 * (>=0) forward-projection magnitude of the surface normal (0 flat, ~2896 at
 * 45 deg). Returns 0x1000 (1.0x) when off or on flat/downhill ground. */
int32_t td5_physics_hill_assist_q12(int32_t up_mag)
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
 * Car-WEIGHT physics mechanics — PORT ADDITION [2026-06-25, car-param stats].
 *
 * The original game's per-car MASS (carparam.dat collision_mass @0x88, an
 * INVERSE-mass term: HIGHER value = LIGHTER car) is consumed ONLY by the V2V
 * collision impulse, so weight has zero effect on acceleration or hill-climbing
 * — every car climbs and accelerates identically. These additions wire mass
 * into three more places so heavier cars actually feel heavy:
 *
 *   (1) UPHILL    — heavier cars lose more speed on a climb (extra slope decel);
 *                   lighter cars climb easily (less slope decel).
 *   (2) ACCEL     — power-to-weight: lighter cars accelerate harder everywhere,
 *                   heavier cars build speed more slowly (drive-torque scale).
 *   (3) FLY-AWAY  — lighter cars get a bigger vertical pop in a crash, heavier
 *                   cars stay planted (collision-lift scale). The HORIZONTAL
 *                   knockback is already mass-driven by the faithful impulse.
 *
 * All three derive from the SHARED td5cp_heaviness_q8() (td5_carparam.h) so the
 * WEIGHT bar on the car-select screen and these behaviours agree by
 * construction. Each is knob-gated (default ON, "grounded" strength) and is a
 * pure function of replicated sim state + once-cached launch knobs, so it stays
 * lockstep- and replay-deterministic (same discipline as the slope/hill/
 * catch-up helpers above).
 *
 * Knobs (env, cached once):
 *   TD5RE_WEIGHT_MECH        master gate, default ON ("0"/"n"/"f" -> all off)
 *   TD5RE_WEIGHT_SLOPE_PCT   uphill strength %, default 55, 0..200
 *   TD5RE_WEIGHT_ACCEL_PCT   power-to-weight strength %, default 40, 0..200
 *   TD5RE_WEIGHT_LIFT_PCT    crash-lift strength %, default 60, 0..200
 * ======================================================================== */
/* This block sits ABOVE the get_phys/PHYS_S/CDEF_S macros and the trig table so
 * the whole mechanic lives in one place; it therefore reads carparam fields via
 * the two tiny local accessors below and the shared fixed-point trig (extern,
 * defined in the td5_physics.c core; declared in td5_physics_internal.h).
 * cp_cdef16 reads the cardef table (actor+0x1B8), cp_phys16 the
 * tuning table (actor+0x1BC) — identical to CDEF_S()/PHYS_S(). */
static inline int16_t cp_cdef16(const TD5_Actor *a, int off) {
    return a->car_definition_ptr ? *(const int16_t *)((const uint8_t *)a->car_definition_ptr + off) : 0;
}
static inline int16_t cp_phys16(const TD5_Actor *a, int off) {
    return a->tuning_data_ptr ? *(const int16_t *)((const uint8_t *)a->tuning_data_ptr + off) : 0;
}

static int s_weight_init       = 0;
static int s_weight_on         = 1;
static int s_weight_slope_pct  = 55;
static int s_weight_accel_pct  = 40;
static int s_weight_lift_pct   = 60;

static void td5_physics_weight_resolve(void)
{
    if (s_weight_init) return;
    s_weight_init = 1;
    {
        const char *e = getenv("TD5RE_WEIGHT_MECH");
        if (e && (e[0]=='0'||e[0]=='n'||e[0]=='N'||e[0]=='f'||e[0]=='F')) s_weight_on = 0;
        s_weight_slope_pct = td5_env_int("TD5RE_WEIGHT_SLOPE_PCT", 55, 0, 200);
        s_weight_accel_pct = td5_env_int("TD5RE_WEIGHT_ACCEL_PCT", 40, 0, 200);
        s_weight_lift_pct  = td5_env_int("TD5RE_WEIGHT_LIFT_PCT",  60, 0, 200);
    }
    TD5_LOG_I(LOG_TAG, "weight_mech: on=%d slope=%d%% accel=%d%% lift=%d%% (mass->hills/accel/fly-away)",
              s_weight_on, s_weight_slope_pct, s_weight_accel_pct, s_weight_lift_pct);
}

/* This actor's inverse-mass (cardef file 0x88). 0 if no cardef loaded. */
static inline int32_t td5_physics_actor_inv_mass(const TD5_Actor *a)
{
    if (!a || !a->car_definition_ptr) return 0;
    return (int32_t)cp_cdef16(a, 0x88);   /* collision_mass (file 0x88) */
}

/* 1/heaviness as a Q8 factor (lighter -> >0x100). Shared by accel + lift. */
static inline int32_t td5_physics_lightness_q8(int32_t inv_mass)
{
    int32_t h = td5cp_heaviness_q8(inv_mass);
    if (h <= 0) return 0x100;
    return (int32_t)(((int64_t)0x100 * 0x100) / h);   /* 1 / heaviness, Q8 */
}

/* Q12 uphill-decel multiplier — heavy (>1.0) loses more, light (<1.0) less.
 * 1.0 (Q12 0x1000) when off / no mass. Grounded clamp [0.70, 1.60]. */
int32_t td5_physics_weight_slope_q12(const TD5_Actor *a)
{
    int32_t inv;
    td5_physics_weight_resolve();
    if (!s_weight_on || s_weight_slope_pct == 0) return 0x1000;
    inv = td5_physics_actor_inv_mass(a);
    if (inv <= 0) return 0x1000;
    return td5cp_blend_clamp_q8(td5cp_heaviness_q8(inv), s_weight_slope_pct,
                                0xB3 /*0.70*/, 0x19A /*1.60*/) << 4;   /* Q8 -> Q12 */
}

/* Q8 power-to-weight drive-torque multiplier — light (>1.0) accelerates harder.
 * 1.0 (0x100) when off / no mass. Grounded clamp [0.70, 1.25]. */
int32_t td5_physics_weight_accel_q8(const TD5_Actor *a)
{
    int32_t inv;
    td5_physics_weight_resolve();
    if (!s_weight_on || s_weight_accel_pct == 0) return 0x100;
    inv = td5_physics_actor_inv_mass(a);
    if (inv <= 0) return 0x100;
    return td5cp_blend_clamp_q8(td5_physics_lightness_q8(inv), s_weight_accel_pct,
                                0xB3 /*0.70*/, 0x140 /*1.25*/);
}

/* Q8 crash-lift multiplier — light (>1.0) flies up more, heavy (<1.0) stays
 * planted. 1.0 (0x100) when off / no mass. Grounded clamp [0.60, 1.70]. */
int32_t td5_physics_weight_lift_q8(const TD5_Actor *a)
{
    int32_t inv;
    td5_physics_weight_resolve();
    if (!s_weight_on || s_weight_lift_pct == 0) return 0x100;
    inv = td5_physics_actor_inv_mass(a);
    if (inv <= 0) return 0x100;
    return td5cp_blend_clamp_q8(td5_physics_lightness_q8(inv), s_weight_lift_pct,
                                0x9A /*0.60*/, 0x1B3 /*1.70*/);
}

/* Apply a Q8 multiplier to a non-negative lift with round-to-zero. */
int32_t td5_physics_scale_lift_q8(int32_t lift, int32_t q8)
{
    if (q8 == 0x100 || lift <= 0) return lift;
    return (int32_t)(((int64_t)lift * (int64_t)q8) >> 8);
}

/* ========================================================================
 * Slipstream / draft — PORT ADDITION [2026-06-25].
 *
 * A racer running closely in another car's wake gets a forward drive-torque
 * boost (modelling reduced aero drag), rewarding pack racing and setting up
 * slingshot passes. Per-slot Q8 boost recomputed once per sim tick
 * (td5_physics_update_draft, before the integration loop) and consumed in the
 * drive-torque chokepoint — same plumbing as MP/Hard catch-up. Pure function of
 * replicated actor positions/headings -> lockstep-deterministic.
 *
 * Detection: project the vector to every OTHER actor onto this car's forward
 * heading. A candidate is "ahead in the wake" when the forward distance is in
 * (DRAFT_MIN, DRAFT_RANGE) and the |lateral offset| <= DRAFT_HALF_WIDTH. The
 * closest qualifying candidate sets the boost, ramping linearly from +peak at
 * DRAFT_MIN to 0 at DRAFT_RANGE. Distances are raw units (world_pos >> 8), the
 * same convention as collision_detect_simple; a car is ~850 raw units long.
 *
 * Knobs: TD5RE_DRAFT (default ON), TD5RE_DRAFT_PCT (peak boost %, default 18).
 * ======================================================================== */
#define DRAFT_MIN_U         500     /* ignore point-blank (that is a crash, not a draft) */
#define DRAFT_RANGE_U       2600    /* ~3 car lengths of forward reach */
#define DRAFT_HALF_WIDTH_U  520     /* must be roughly behind, not alongside */

static int s_draft_init = 0;
static int s_draft_on   = 1;
static int s_draft_pct  = 18;
static int32_t s_draft_boost_q8[TD5_MAX_RACER_SLOTS];

static void td5_physics_draft_resolve(void)
{
    if (s_draft_init) return;
    s_draft_init = 1;
    {
        const char *e = getenv("TD5RE_DRAFT");
        if (e && (e[0]=='0'||e[0]=='n'||e[0]=='N'||e[0]=='f'||e[0]=='F')) s_draft_on = 0;
        s_draft_pct = td5_env_int("TD5RE_DRAFT_PCT", 18, 0, 100);
    }
    TD5_LOG_I(LOG_TAG, "draft: on=%d peak=%d%% (slipstream forward boost behind a car)", s_draft_on, s_draft_pct);
}

/* Recompute every racer's draft boost for this tick. Called from
 * td5_physics_tick BEFORE the per-actor integration loop. */
void td5_physics_update_draft(void)
{
    int total, s, o;
    td5_physics_draft_resolve();
    for (s = 0; s < TD5_MAX_RACER_SLOTS; ++s) s_draft_boost_q8[s] = 0x100;
    if (!s_draft_on || s_draft_pct == 0 || !g_actor_table_base) return;
    total = td5_game_get_total_actor_count();
    if (total <= 0) return;
    if (total > TD5_MAX_TOTAL_ACTORS) total = TD5_MAX_TOTAL_ACTORS;

    {
        int racers = g_traffic_slot_base;
        if (racers > TD5_MAX_RACER_SLOTS) racers = TD5_MAX_RACER_SLOTS;
        int32_t peak_extra = (s_draft_pct * 0x100) / 100;   /* Q8 extra at point-blank */
        for (s = 0; s < racers; ++s) {
            TD5_Actor *me = (TD5_Actor *)(g_actor_table_base + (size_t)s * TD5_ACTOR_STRIDE);
            int32_t heading = (me->euler_accum.yaw >> 8) & 0xFFF;
            int32_t fx = sin_fixed12(heading);   /* forward (sin_h, cos_h) */
            int32_t fz = cos_fixed12(heading);
            int32_t best_prox = 0;
            for (o = 0; o < total; ++o) {
                if (o == s) continue;
                TD5_Actor *ot = (TD5_Actor *)(g_actor_table_base + (size_t)o * TD5_ACTOR_STRIDE);
                int32_t dx = (ot->world_pos.x - me->world_pos.x) >> 8;   /* raw units */
                int32_t dz = (ot->world_pos.z - me->world_pos.z) >> 8;
                int32_t fwd = (dx * fx + dz * fz) >> 12;                 /* forward dist */
                int32_t lat;
                if (fwd <= DRAFT_MIN_U || fwd >= DRAFT_RANGE_U) continue;
                lat = (dx * fz - dz * fx) >> 12;                         /* lateral offset */
                if (lat < 0) lat = -lat;
                if (lat > DRAFT_HALF_WIDTH_U) continue;
                {
                    int32_t prox = DRAFT_RANGE_U - fwd;                  /* closer -> bigger */
                    if (prox > best_prox) best_prox = prox;
                }
            }
            if (best_prox > 0)
                s_draft_boost_q8[s] = 0x100 +
                    (int32_t)(((int64_t)peak_extra * best_prox) / (DRAFT_RANGE_U - DRAFT_MIN_U));
        }
    }
}

int32_t td5_physics_draft_mult(int slot)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0x100;
    return s_draft_boost_q8[slot];
}

/* ========================================================================
 * Downforce — PORT ADDITION [2026-06-25].
 *
 * Aerodynamic high-speed cornering grip: at speed, a slice of the car's LATERAL
 * (sideways-slide) velocity is damped out, scaled by the car's downforce rating
 * and the SQUARE of its forward speed (downforce ~ v^2). High-downforce cars
 * stay planted through fast corners; low-downforce cars keep sliding. A gentle,
 * speed-gated effect (≤~6% of lateral velocity per tick at top speed for the
 * highest-downforce car) so it improves stability without fighting the slip
 * physics. Applied in the player drive update where the heading basis is in
 * scope.
 *
 * Rating source: lateral_slip_stiffness (file 0x108 = tuning+0x7C, roster span
 * 32..360). [UNCERTAIN] the field's exact high/low semantics conflict between
 * sources, so this is purely ADDITIVE and off-by-knob — a wrong reading only
 * changes which cars feel planted, never breaks faithful handling.
 *
 * Knobs: TD5RE_DOWNFORCE (default ON), TD5RE_DOWNFORCE_PCT (peak lateral-damp %
 * at top speed for max-rating car, default 6, 0..40).
 * ======================================================================== */
#define DOWNFORCE_SLIP_MIN  32
#define DOWNFORCE_SLIP_MAX  360

static int s_downforce_init = 0;
static int s_downforce_on   = 1;
static int s_downforce_pct  = 6;

static void td5_physics_downforce_resolve(void)
{
    if (s_downforce_init) return;
    s_downforce_init = 1;
    {
        const char *e = getenv("TD5RE_DOWNFORCE");
        if (e && (e[0]=='0'||e[0]=='n'||e[0]=='N'||e[0]=='f'||e[0]=='F')) s_downforce_on = 0;
        s_downforce_pct = td5_env_int("TD5RE_DOWNFORCE_PCT", 6, 0, 40);
    }
    TD5_LOG_I(LOG_TAG, "downforce: on=%d peak=%d%% (high-speed lateral grip from slip-stiffness)",
              s_downforce_on, s_downforce_pct);
}

/* Damp lateral velocity by the aero downforce term. sin_h/cos_h = heading. */
void td5_physics_apply_downforce(TD5_Actor *actor, int32_t sin_h, int32_t cos_h)
{
    int32_t slip, df_norm_q12, top, fwd_v, speed_q12, lat_v, damp_frac_q12, damp;
    td5_physics_downforce_resolve();
    if (!s_downforce_on || s_downforce_pct == 0) return;
    if (!actor->tuning_data_ptr) return;

    slip = (int32_t)cp_phys16(actor, 0x7C);              /* lateral_slip_stiffness (file 0x108) */
    if (slip <= DOWNFORCE_SLIP_MIN) return;              /* lowest-aero car: no effect */
    df_norm_q12 = ((slip - DOWNFORCE_SLIP_MIN) * 0x1000) / (DOWNFORCE_SLIP_MAX - DOWNFORCE_SLIP_MIN);
    if (df_norm_q12 > 0x1000) df_norm_q12 = 0x1000;

    /* forward / lateral split of world velocity. forward = (sin_h, cos_h);
     * lateral axis L = (cos_h, -sin_h). */
    fwd_v = (actor->linear_velocity_x * sin_h + actor->linear_velocity_z * cos_h) >> 12;
    lat_v = (actor->linear_velocity_x * cos_h - actor->linear_velocity_z * sin_h) >> 12;
    if (fwd_v < 0) fwd_v = 0;

    /* speed fraction of this car's top speed, squared (downforce ~ v^2). */
    top = (int32_t)cp_phys16(actor, 0x74);   /* top_speed_limit (file 0x100) */
    if (top <= 0) return;
    {
        int64_t s = ((int64_t)fwd_v << 12) / ((int64_t)top << 8);   /* fwd_v / (top<<8), Q12 */
        if (s > 0x1000) s = 0x1000;
        speed_q12 = (int32_t)((s * s) >> 12);                       /* squared, Q12 */
    }

    /* peak per-tick lateral-damp fraction (Q12) * df_norm * speed^2. */
    {
        int32_t peak_q12 = (s_downforce_pct * 0x1000) / 100;
        int64_t f = ((int64_t)peak_q12 * df_norm_q12) >> 12;
        damp_frac_q12 = (int32_t)((f * speed_q12) >> 12);
    }
    if (damp_frac_q12 <= 0) return;

    damp = (int32_t)(((int64_t)lat_v * damp_frac_q12) >> 12);       /* lateral velocity to remove */
    /* v_new = v - damp * L  (L = cos_h, -sin_h) */
    actor->linear_velocity_x -= (damp * cos_h) >> 12;
    actor->linear_velocity_z += (damp * sin_h) >> 12;
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
void td5_physics_update_hard_catchup(void)
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
int32_t td5_physics_hard_catchup_mult(int slot)
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
/* (moved to td5_physics_internal.h) CRASH_FX_ACUTE_MAG */
/* [POLICE rewrite 2026-06-19] COP_WALL_BREAK_VPERP (the approach-speed wall-
 * break gate) moved to td5_physics_internal.h — the core wall response reads it
 * directly, in addition to the cop-durability scaling below. */
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
int32_t npc_fatal_mag(void) {
    static int32_t v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_NPC_FATAL_MAG", NPC_FATAL_MAG_DEFAULT, 1000, 1000000);
    }
    return v;
}
/* [TRAFFIC BATTLE 2026-06-28] Public accessor so the battle scoring hook
 * (td5_arcade_note_ram) shares the single npc-fatal-impact threshold with the
 * V2V heavy-hit gate (no constant duplication / drift). */
int32_t td5_physics_npc_fatal_mag(void) { return npc_fatal_mag(); }
/* [TRAFFIC BATTLE 2026-06-28] Sensitivity of the SPEED-based wreck trigger
 * (percent applied to the closing-speed impact estimate). The default
 * impact_mag is the NORMAL-projected impulse, so a fast glancing / corner crash
 * barely registers and never totals the traffic car. The battle path recomputes
 * the impact from the full closing speed so a hard crash wrecks at ANY angle;
 * this percent dials how eager that is (>100 = wreck on more hits). Clamp
 * [10,1000]. TD5RE_BATTLE_RAM_PCT overrides. */
#define BATTLE_RAM_PCT_DEFAULT 150
int battle_ram_pct(void) {
    static int v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_BATTLE_RAM_PCT", BATTLE_RAM_PCT_DEFAULT, 10, 1000);
        TD5_LOG_I(LOG_TAG, "battle_ram_pct: TD5RE_BATTLE_RAM_PCT=%d", v);
    }
    return v;
}
/* [TRAFFIC BATTLE 2026-06-28] Ceiling for the speed-based wreck impact. The raw
 * closing-speed*mass product can be 10-20x a normal head-on (a fast near-tangent
 * pass has a huge relative velocity but a tiny normal impulse), which would
 * launch/spin both cars absurdly. Cap it at a strong-but-already-tuned head-on
 * level (~the upper end the logged v2v_heavy_scatter hits reach) so the crash
 * reliably TOTALS the traffic car (well past npc_fatal_mag) with a firm but
 * survivable reaction. TD5RE_BATTLE_RAM_CAP overrides. */
#define BATTLE_RAM_CAP_DEFAULT 200000
int32_t battle_ram_cap(void) {
    static int32_t v = -1;
    if (v < 0) {
        /* lo 60000: must clear npc_fatal_mag to wreck */
        v = td5_env_int("TD5RE_BATTLE_RAM_CAP", BATTLE_RAM_CAP_DEFAULT, 60000, 1000000);
    }
    return v;
}
/* [TRAFFIC BATTLE 2026-06-28] Minimum racer planar speed (24.8 units) for the
 * swept wreck pass to total a traffic car on contact — keeps a stationary car
 * from "wrecking" something it's resting against. Low by default so any real
 * driving counts (the mode is about plowing through traffic). TD5RE_BATTLE_RAM_MIN_SPEED. */
#define BATTLE_RAM_MIN_SPEED_DEFAULT 3000
int32_t battle_ram_min_speed(void) {
    static int32_t v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_BATTLE_RAM_MIN_SPEED", BATTLE_RAM_MIN_SPEED_DEFAULT, 0, 200000);
    }
    return v;
}
/* [TRAFFIC BATTLE 2026-06-28] Vertical-speed threshold above which a traffic car
 * is "thrown in the air" and counts as wrecked (user: "trigger wreck when cars
 * get thrown out in the air"). A car resting/rolling on the road has a small vy;
 * a launched one has a large one. TD5RE_BATTLE_AIRBORNE_VY. */
#define BATTLE_AIRBORNE_VY_DEFAULT 6000
int32_t battle_airborne_vy(void) {
    static int32_t v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_BATTLE_AIRBORNE_VY", BATTLE_AIRBORNE_VY_DEFAULT, 500, 200000);
    }
    return v;
}
/* [POLICE 2026-06-24] A police cop is more DURABLE than plain traffic: it
 * survives the impacts that total an ordinary traffic car, and only wrecks on a
 * genuinely heavy hit (the player deliberately ramming it). Expressed as a
 * percentage of the traffic break threshold (npc_fatal_mag for V2V,
 * COP_WALL_BREAK_VPERP for walls); default 250 = 2.5x (user: "police should be
 * 2.5x more durable than traffic ... breakable by us but shouldn't break down so
 * easily"). Clamped [100,1000]. TD5RE_COP_DURABILITY_PCT overrides. Port-only —
 * the original has NO cop-vs-traffic durability concept (the whole cop-chase-as-
 * traffic system is port-only). */
/* [COP OVERHAUL 2026-06-29] Raised 250 -> 300: cops shrug off harder hits so they
 * "don't break down so easily" (user). Still breakable by a deliberate player ram
 * (V2V threshold is only 1.2x higher than before). TD5RE_COP_DURABILITY_PCT
 * overrides. */
#define COP_DURABILITY_PCT_DEFAULT  300
int cop_durability_pct(void) {
    static int v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_COP_DURABILITY_PCT", COP_DURABILITY_PCT_DEFAULT, 100, 1000);
        TD5_LOG_I(LOG_TAG, "cop_durability_pct: TD5RE_COP_DURABILITY_PCT=%d", v);
    }
    return v;
}
/* [COP OVERHAUL 2026-06-29] Extra wall-break leniency (%) for an ACTIVELY CHASING
 * cop, applied ON TOP of cop_durability_pct at the wall gate only. A fast chase
 * scrapes walls; those self-inflicted scrapes must not wreck the cop and end the
 * pursuit (a core "cops break down too easily / never catch up" cause). Player
 * V2V rams are NOT affected — only the wall gate is relaxed, so the cop is still
 * breakable by a deliberate ram. Default 200 (2x); a catastrophic head-on still
 * exceeds the raised gate and wrecks it. TD5RE_COP_CHASE_WALL_FACTOR overrides. */
int cop_chase_wall_factor_pct(void) {
    static int v = -1;
    if (v < 0) {
        v = td5_env_int("TD5RE_COP_CHASE_WALL_FACTOR", 200, 100, 600);
    }
    return v;
}
/* The V2V impact magnitude that wrecks a COP — 2.5x the traffic floor by
 * default. A cop hit BELOW this shrugs the collision off entirely (no scatter,
 * no air-launch, no break-down) so a chase isn't ended by a fender-bender. */
int32_t cop_break_mag(void) {
    return (int32_t)((int64_t)npc_fatal_mag() * cop_durability_pct() / 100);
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


/* ======== [split] crash-FX gate + getters (moved verbatim from td5_physics.c) ======== */
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
void td5_physics_apply_acute_crash_fx(TD5_Actor *actor, int32_t impact_mag)
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

/* ======== [split] recovery drivers (moved verbatim from td5_physics.c) ======== */
/* ========================================================================
 * Player stuck-car recovery (PORT ENHANCEMENT 2026-06-15)
 * ======================================================================== */

/* Cache the recovery knobs once. TD5RE_STUCK_RECOVERY (default ON) gates manual
 * recovery; TD5RE_RECOVERY_SPANS_BACK (default 0 = reset IN PLACE; >0 = legacy
 * step-back, opt-in) controls placement; TD5RE_RECOVERY_COOLDOWN_TICKS
 * (default 150 = 5 s) is the manual cooldown. Logged once like the other
 * [PORT ENHANCEMENT] knobs. */
static void recovery_init_knobs(void)
{
    if (s_recovery_init) return;
    s_recovery_enabled = td5_env_flag_on("TD5RE_STUCK_RECOVERY");   /* default ON */
    /* sanity clamp [0,64] */
    s_recovery_spans_back = td5_env_int("TD5RE_RECOVERY_SPANS_BACK",
                                        TD5_RECOVERY_DEFAULT_BACK, 0, 64);
    /* Tune the manual-recovery cooldown without a rebuild. 0 = no cooldown;
     * <= 60 s (1800 ticks) sanity clamp. */
    s_recovery_cooldown_ticks = td5_env_int("TD5RE_RECOVERY_COOLDOWN_TICKS",
                                            TD5_MANUAL_RECOVERY_COOLDOWN_TICKS, 0, 1800);
    s_recovery_init = 1;
    TD5_LOG_I(LOG_TAG, "Manual recovery: %s spans_back=%d cooldown=%d ticks (%.1fs)",
              s_recovery_enabled ? "enabled" : "disabled",
              s_recovery_spans_back, s_recovery_cooldown_ticks,
              s_recovery_cooldown_ticks / 30.0);
}

/* Recover `slot`'s actor: upright it, kill its motion, re-face it track-forward,
 * ground-snap it, and (port damage feature) fully repair it.
 *
 * [RESET-CAR IN-PLACE FIX 2026-06-29] DEFAULT (s_recovery_spans_back == 0) is an
 * IN-PLACE reset, faithful to the original ResetVehicleActorState @0x00405D70:
 * world_pos.x/z are NOT moved, only Y is ground-snapped and roll/pitch + velocity
 * are zeroed. This is what the original's automatic flip-recovery does, and it
 * structurally CANNOT teleport the car to the start or drop it into wall geometry
 * — because the car's XZ never changes. The only port addition over the faithful
 * reset is re-facing the car track-forward (compute_heading), so a spun/flipped
 * car points the right way after the press.
 *
 * The LEGACY step-back path (s_recovery_spans_back > 0, opt-in) re-derives a
 * span-stepped-back center-lane world XZ. That path is the documented root cause
 * of the split-screen "reset teleported me to the beginning" and "reset spawned
 * the car somewhere it broke" bugs (the recomputed span could resolve to ~span 0,
 * and a branch/junction span's center lane can sit in wall geometry that the new
 * damage-bar then wrecks). Kept only behind the explicit knob.
 *
 * Either way, on success the car is fully repaired (health restored, dents +
 * knockout cleared) so a damaged or knocked-out car is actually recovered — the
 * byte-faithful reset can't touch the port-only damage fields, and a knocked-out
 * car (health 0) would otherwise stay physics-frozen forever. */
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
    int tgt_span = cur_span;

    if (s_recovery_spans_back > 0) {
        /* ---- LEGACY OPT-IN: step back N spans + re-derive world XZ from track
         * geometry. Off by default (see fix note above). ---- */
        int sub_lane, wx, wy, wz;
        if (!td5_track_get_recovery_pose(cur_span, s_recovery_spans_back,
                                         &tgt_span, &sub_lane, &wx, &wy, &wz)) {
            TD5_LOG_W(LOG_TAG, "recover_player slot=%d: no recovery pose (cur_span=%d)",
                      slot, cur_span);
            return 0;
        }
        actor->track_span_raw         = (int16_t)tgt_span;   /* +0x80 */
        actor->track_span_accumulated = (int16_t)tgt_span;   /* +0x84 */
        actor->track_span_high_water  = (int16_t)tgt_span;   /* +0x86 */
        actor->track_sub_lane_index   = (uint8_t)sub_lane;   /* +0x8C */
        actor->world_pos.x = wx;                             /* +0x1FC */
        actor->world_pos.y = (int32_t)0xC0000000;            /* +0x200 (ground-snap sentinel) */
        actor->world_pos.z = wz;                             /* +0x204 */
    } else {
        /* ---- DEFAULT: IN-PLACE reset (faithful to ResetVehicleActorState). Keep
         * world_pos.x/z exactly; only mark Y for the ground-snap. The current span
         * + sub-lane are already correct (the per-tick track walker maintains
         * them), so leave them untouched for compute_heading to read. ---- */
        actor->world_pos.y = (int32_t)0xC0000000;            /* +0x200 (ground-snap sentinel) */
    }

    /* Heading aligned to track forward at the (current or stepped-back) span.
     * compute_heading reads track_span_raw (+0x80) + sub_lane (+0x8C) and writes
     * euler_accum.yaw (+0x1F4) + heading_normal (+0x290); it does NOT move world
     * XZ, so the in-place path stays in place. On TD6 tracks the geometry yaw
     * lands ~90 deg off, so re-seed from the route heading like the spawn path. */
    td5_track_compute_heading(actor);
    if (g_active_td6_level > 0)
        td5_ai_correct_spawn_heading(slot);

    /* Zero linear+angular velocity, roll/pitch euler, settle suspension, rebuild
     * the rotation matrix + render pose, and ground-snap Y. reset_actor_state
     * preserves euler_accum.yaw (only roll/pitch are zeroed), so the corrected
     * heading above survives, and it never touches world_pos.x/z. */
    td5_physics_reset_actor_state(actor);

    /* reset_actor_state -> integrate_pose -> update_actor_position may have
     * walked track_span_raw across a boundary; restore it to the recovery span. */
    actor->track_span_raw = (int16_t)tgt_span;

    /* Clear control-flag residue so the car doesn't immediately re-trip a stuck
     * heuristic or carry a stale wall/handbrake/reverse latch into the next tick. */
    actor->brake_flag = 0;          /* +0x36D */
    actor->handbrake_flag = 0;      /* +0x36E */
    actor->throttle_state = 1;      /* +0x36F: forward */
    actor->track_contact_flag = 0;  /* +0x37B: V2W contact */

    /* [RESET-CAR REPAIR 2026-06-29] Fully recover the car: restore health, clear
     * dents + the knockout state (port-only damage feature — inert when CarDamage
     * is off), and clear the broken-down flag so a knocked-out racer is no longer
     * treated as wrecked (it would otherwise stay physics-frozen, since health
     * never regenerates). */
    td5_damage_repair_actor(slot);
    td5_ai_clear_actor_broken_down(slot);

    TD5_LOG_I(LOG_TAG,
              "recover_player: slot=%d cur_span=%d -> span=%d%s pos=(%d,%d,%d) [repaired]",
              slot, cur_span, tgt_span,
              (s_recovery_spans_back > 0) ? " (step-back)" : " (in-place)",
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

    int route_to_driven = td5_input_split_btn_route_on();
    for (int vp = 0; vp < views; vp++) {
        /* [SPLIT-SCREEN BUTTON ROUTING 2026-06-27] `vp` is the local-player index
         * (the same index the input layer armed s_recovery_request[] with). Reset the
         * actor that player DRIVES — actor slot == vp (identity, matching the per-slot
         * steering loop and the original's gPrimarySelectedSlot/{0,1} map) — NOT
         * g_actorSlotForView[vp] (the car SHOWN in pane vp). When the MP position
         * picker permutes panes these differ, and the old code reset the wrong
         * player's car (sending it to wherever that player was — often the start).
         * Byte-identical when the pane map is identity (g_actorSlotForView[vp]==vp).
         * Knob "0" reverts to the legacy pane-index routing. */
        int slot = route_to_driven ? vp : g_actorSlotForView[vp];
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
    s_rgentle_enabled     = td5_env_flag_on("TD5RE_RECOVERY_GENTLE");
    s_rgentle_coast_ticks = td5_env_int("TD5RE_RECOVERY_COAST_TICKS", 30, 1, 240);   /* sanity clamp (~8s max) */
    s_rgentle_coast_decay = td5_env_float("TD5RE_RECOVERY_COAST_DECAY", 0.92f, 0.0f, 1.0f);
    s_rgentle_level_decay = td5_env_float("TD5RE_RECOVERY_LEVEL_DECAY", 0.80f, 0.0f, 1.0f);
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
int recovery_gentle_for_actor(const TD5_Actor *actor)
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
void td5_physics_gentle_recovery_coast(TD5_Actor *actor)
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

/* Race-init hook (called from InitializeRaceVehicleRuntime in the core):
 * clears per-race assist state so nothing carries across races. */
void td5_physics_assists_race_reset(void)
{
    memset(s_manual_recovery_cooldown, 0, sizeof(s_manual_recovery_cooldown));
}
