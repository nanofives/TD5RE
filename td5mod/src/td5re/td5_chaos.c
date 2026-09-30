/* ========================================================================
 * td5_chaos.c — CHAOS CO-OP: seat table, role map, rotation (PORT-ONLY)
 *
 * See td5_chaos.h for the index spaces and the role table, and
 * docs/plans/CHAOS_COOP_MODE_PLAN.md for the full design.
 *
 * [CONTRACT 2026-09-29] Rules, activation and role lookup are final. The
 * SECTION: ROTATION block is a stub (no rotation ever fires) owned by the
 * GAME zone; it must keep every signature in td5_chaos.h.
 * ======================================================================== */
#include "td5_chaos.h"

#include <stdio.h>
#include <string.h>

#include "td5re.h"          /* g_td5 (mp_mode_config.mode, network_active) */
#include "td5_platform.h"   /* TD5_LOG_*, td5_plat_ff_* (per-device rumble)  */
#include "td5_race_state.h" /* read-only race queries (NOT td5_game.h)       */
#include "td5_track.h"      /* ring length + branch->main span remap         */

/* [CHAOS CO-OP 2026-09-29] The log sink routes by module TAG string, and only
 * the fixed list in td5_platform_win32_log.c reaches race.log. The rotation
 * belongs in race.log next to the rest of the per-tick race state, so this
 * module logs under the "td5_game" tag; every line is prefixed "[CHAOS] ". */
#define LOG_TAG "td5_game"

/* ========================================================================
 * SECTION: RULES (pure)
 * ======================================================================== */

int td5_chaos_count_is_legal(int players) {
    return players == 4 || players == 6 || players == 8;
}

TD5_ChaosRole td5_chaos_role_for_row(int team_size, int row) {
    static const TD5_ChaosRole k2[2] = { TD5_CHAOS_ROLE_STEER, TD5_CHAOS_ROLE_PEDALS };
    static const TD5_ChaosRole k3[3] = { TD5_CHAOS_ROLE_LEFT, TD5_CHAOS_ROLE_RIGHT,
                                         TD5_CHAOS_ROLE_PEDALS };
    static const TD5_ChaosRole k4[4] = { TD5_CHAOS_ROLE_LEFT, TD5_CHAOS_ROLE_RIGHT,
                                         TD5_CHAOS_ROLE_THROTTLE, TD5_CHAOS_ROLE_BRAKE };
    if (row < 0 || row >= team_size) return TD5_CHAOS_ROLE_NONE;
    switch (team_size) {
    case 2: return k2[row];
    case 3: return k3[row];
    case 4: return k4[row];
    default: return TD5_CHAOS_ROLE_NONE;
    }
}

const char *td5_chaos_role_name(TD5_ChaosRole role) {
    switch (role) {
    case TD5_CHAOS_ROLE_STEER:    return "STEER";
    case TD5_CHAOS_ROLE_LEFT:     return "LEFT";
    case TD5_CHAOS_ROLE_RIGHT:    return "RIGHT";
    case TD5_CHAOS_ROLE_PEDALS:   return "PEDALS";
    case TD5_CHAOS_ROLE_THROTTLE: return "THROTTLE";
    case TD5_CHAOS_ROLE_BRAKE:    return "BRAKE";
    default:                      return "--";
    }
}

/* ========================================================================
 * SECTION: ACTIVATION / SEAT TABLE
 * The CHAOS TEAMS screen commits the seat table here (td5_chaos_commit_config).
 * Not in g_td5.mp_mode_config: that struct is part of the net wire format.
 * ======================================================================== */

static TD5_ChaosConfig s_cfg;

void td5_chaos_commit_config(const TD5_ChaosConfig *cfg) {
    if (cfg) s_cfg = *cfg;
    else     memset(&s_cfg, 0, sizeof(s_cfg));
}

const TD5_ChaosConfig *td5_chaos_get_config(void) {
    return &s_cfg;
}

int td5_chaos_active(void) {
    return g_td5.mp_mode_config.mode == TD5_MP_MODE_CHAOS_COOP &&
           !g_td5.network_active &&
           td5_chaos_count_is_legal(s_cfg.seat_count);
}

int td5_chaos_seat_count(void) {
    return td5_chaos_active() ? s_cfg.seat_count : 0;
}

int td5_chaos_team_size(void) {
    return td5_chaos_seat_count() / 2;
}

int td5_chaos_input_slot_count(void) {
    return td5_chaos_seat_count();
}

int td5_chaos_team_of_seat(int seat) {
    if (seat < 0 || seat >= td5_chaos_seat_count()) return -1;
    return s_cfg.team_of_seat[seat] ? 1 : 0;
}

int td5_chaos_device_of_seat(int seat) {
    if (seat < 0 || seat >= td5_chaos_seat_count()) return -1;
    return s_cfg.device_of_seat[seat];
}

/* ========================================================================
 * SECTION: ROTATION
 *
 * [CHAOS CO-OP 2026-09-29] Milestones are POLLED on the 30 Hz sim clock, not
 * hooked at the checkpoint-crossing site: that site's caller
 * (advance_pending_finish_state) runs once per RENDERED frame, which would make
 * the rotation frame-rate dependent. See docs/plans/CHAOS_COOP_MODE_PLAN.md 7.1.
 *
 * Milestone quantity per trigger, per track class:
 *   CHECKPOINT  P2P: td5_game_get_player_lap() (see the overload note below)
 *               circuit: lap*4 + quarter-lap arc of the ring
 *   HALF_LAP    P2P: every other checkpoint; circuit: lap*2 + half-lap arc
 *   LAP         td5_game_get_player_lap() (laps on a circuit)
 *   TIME        racing ticks / (period_secs * 30)
 *   OFF         constant 0 (never fires)
 * Drag strips have exactly one checkpoint (the finish), so everything except
 * OFF falls back to TIME there.
 *
 * td5_game_get_player_lap(slot) returns s_metrics[].checkpoint_index, which is
 * LAPS on a circuit and CHECKPOINTS PASSED on a point-to-point track. That
 * overload is normally a trap; here it is exactly the quantity wanted, and it
 * makes one accessor cover both track classes. Do not "fix" it.
 *
 * Deliberately NOT used (plan 7.1): track_span_accumulated (int16_t, overflows
 * past ~10 laps and goes negative in reverse) and td5_game_get_slot_progress
 * (multiplies the lap accessor by the ring length, so it jumps a whole ring per
 * checkpoint on a point-to-point track). track_span_normalized wraps every lap
 * and near ring-1 when a car reverses over span 0 — the strictly-forward
 * milestone guard plus the branch->main remap contains both.
 * ======================================================================== */

/* Fixed simulation rate (td5_game.c's fixed-step loop). secs*30 is exact. */
#define CHAOS_SIM_HZ            30
/* Rumble pulse length at a swap, in sim ticks (~0.3 s). */
#define CHAOS_SWAP_RUMBLE_TICKS 9
/* Rumble magnitude, in td5_plat_ff_constant units (-10000..+10000). */
#define CHAOS_SWAP_RUMBLE_MAG   8000
/* FF effect slot 1 = the event-driven "crash / gear jolt" slot; on an XInput
 * pad it is routed to the high-frequency motor, on a DI wheel it is a real
 * effect. The continuous slots (0 steering, 3 terrain) must not be borrowed. */
#define CHAOS_SWAP_FF_SLOT      1

static int32_t s_rotation_step[TD5_CHAOS_TEAMS];
static int32_t s_last_milestone[TD5_CHAOS_TEAMS];
static int32_t s_swap_ticks_left[TD5_CHAOS_TEAMS];
static int32_t s_swap_flash_ticks[TD5_CHAOS_TEAMS];
static int32_t s_rumble_ticks_left[TD5_CHAOS_TEAMS];
static int32_t s_race_ticks;        /* sim ticks since the start countdown cleared */
static int     s_milestone_seeded;  /* 0 until the baseline has been taken */

/* Committed trigger with the per-track-class fallbacks applied. */
static int chaos_effective_trigger(void) {
    int t = s_cfg.trigger;
    if (t < 0 || t >= TD5_CHAOS_TRIGGER_COUNT) t = TD5_CHAOS_TRIGGER_CHECKPOINT;
    /* A drag strip has one checkpoint (the finish) and no lap: TIME is the only
     * trigger that can ever fire there. */
    if (g_td5.drag_race_enabled &&
        t != TD5_CHAOS_TRIGGER_OFF && t != TD5_CHAOS_TRIGGER_TIME)
        t = TD5_CHAOS_TRIGGER_TIME;
    return t;
}

/* TIME trigger period in sim ticks (committed 10..60 s, default 25 s). */
static int chaos_period_ticks(void) {
    int p = s_cfg.period_secs;
    if (p < 10 || p > 60) p = 25;
    return p * CHAOS_SIM_HZ;
}

/* Milestones per lap for the arc triggers on a CIRCUIT. */
static int chaos_arcs_per_lap(int trigger) {
    return (trigger == TD5_CHAOS_TRIGGER_HALF_LAP) ? 2 : 4;
}

/* How many milestones make one full cycle, for the "n/N" HUD readout. */
static int chaos_cycle_len(int trigger) {
    int n;
    if (g_td5.track_type == TD5_TRACK_CIRCUIT) return chaos_arcs_per_lap(trigger);
    n = td5_game_get_minimap_checkpoint_count();
    if (trigger == TD5_CHAOS_TRIGGER_HALF_LAP) n /= 2;
    return (n > 0) ? n : 1;
}

/* Monotonic milestone counter for one team's car (racer slot == team). */
static int32_t chaos_milestone_of(int team) {
    int trigger = chaos_effective_trigger();
    int lap;
    switch (trigger) {
    case TD5_CHAOS_TRIGGER_TIME:
        return s_race_ticks / chaos_period_ticks();
    case TD5_CHAOS_TRIGGER_LAP:
        return (int32_t)td5_game_get_player_lap(team);
    case TD5_CHAOS_TRIGGER_CHECKPOINT:
    case TD5_CHAOS_TRIGGER_HALF_LAP:
        lap = td5_game_get_player_lap(team);
        if (g_td5.track_type != TD5_TRACK_CIRCUIT) {
            /* Point to point: the accessor already counts checkpoints. */
            return (int32_t)((trigger == TD5_CHAOS_TRIGGER_HALF_LAP) ? lap / 2 : lap);
        }
        {
            /* Circuit: quantize the ring into K equal arcs. The branch->main
             * remap is mandatory on AUTO tracks — a raw fork/corridor span
             * aliases onto an unrelated low main-ring span and would fire a
             * spurious rotation every time the car took a branch. */
            int k    = chaos_arcs_per_lap(trigger);
            int ring = td5_track_get_ring_length();
            int span;
            if (ring <= 0) return (int32_t)lap * k;
            span = td5_track_branch_to_main_span(td5_game_get_slot_span(team));
            if (span < 0)     span = 0;
            if (span >= ring) span = ring - 1;
            return (int32_t)lap * k + (int32_t)((span * k) / ring);
        }
    default:
        return 0;   /* OFF */
    }
}

static void chaos_seed_milestones(void) {
    int t;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++) s_last_milestone[t] = chaos_milestone_of(t);
    s_milestone_seeded = 1;
}

/* Short rumble on every seat of `team`. The seat's device lives in input slot
 * == seat index, which is also the FF device slot. Keyboard seats (device 0)
 * have no motor, so they are skipped. */
static void chaos_rumble_team(int team, int magnitude) {
    int n = td5_chaos_seat_count(), s;
    for (s = 0; s < n; s++) {
        if (td5_chaos_team_of_seat(s) != team) continue;
        if (td5_chaos_device_of_seat(s) <= 0) continue;   /* keyboard: no motor */
        if (magnitude > 0) td5_plat_ff_constant(s, CHAOS_SWAP_FF_SLOT, magnitude);
        else               td5_plat_ff_stop(s, CHAOS_SWAP_FF_SLOT);
    }
}

void td5_chaos_race_begin(void) {
    memset(s_rotation_step,    0, sizeof(s_rotation_step));
    memset(s_last_milestone,   0, sizeof(s_last_milestone));
    memset(s_swap_ticks_left,  0, sizeof(s_swap_ticks_left));
    memset(s_swap_flash_ticks, 0, sizeof(s_swap_flash_ticks));
    memset(s_rumble_ticks_left, 0, sizeof(s_rumble_ticks_left));
    s_race_ticks      = 0;
    s_milestone_seeded = 0;
    if (td5_chaos_active()) {
        TD5_LOG_I(LOG_TAG, "[CHAOS] race begin: seats=%d trigger=%d period=%ds track_type=%d drag=%d",
                  td5_chaos_seat_count(), chaos_effective_trigger(),
                  chaos_period_ticks() / CHAOS_SIM_HZ, g_td5.track_type,
                  (int)g_td5.drag_race_enabled);
    }
}

void td5_chaos_tick(void) {
    int t;
    if (!td5_chaos_active()) return;

    if (td5_game_is_countdown_active()) {
        /* The 3-2-1 holds the grid. Keep re-taking the baseline so the start
         * line itself never reads as a milestone, and keep the race clock at 0. */
        chaos_seed_milestones();
        return;
    }
    if (!s_milestone_seeded) chaos_seed_milestones();   /* first racing tick */

    s_race_ticks++;

    for (t = 0; t < TD5_CHAOS_TEAMS; t++) {
        int32_t m = chaos_milestone_of(t);
        if (m > s_last_milestone[t]) {          /* strictly forward only */
            s_last_milestone[t] = m;
            TD5_LOG_I(LOG_TAG, "[CHAOS] milestone team=%d m=%d", t, (int)m);
            /* A milestone reached while a swap is already counting down does
             * NOT restart the countdown (that would postpone the swap forever
             * on a short arc). */
            if (s_swap_ticks_left[t] <= 0)
                s_swap_ticks_left[t] = TD5_CHAOS_SWAP_COUNTDOWN_TICKS;
        }
        if (s_swap_ticks_left[t] > 0 && --s_swap_ticks_left[t] == 0) {
            s_rotation_step[t]++;
            s_swap_flash_ticks[t]  = TD5_CHAOS_SWAP_FLASH_TICKS;
            s_rumble_ticks_left[t] = CHAOS_SWAP_RUMBLE_TICKS;
            chaos_rumble_team(t, CHAOS_SWAP_RUMBLE_MAG);
            TD5_LOG_I(LOG_TAG, "[CHAOS] swap team=%d step=%d", t, (int)s_rotation_step[t]);
        }
        if (s_swap_flash_ticks[t] > 0) s_swap_flash_ticks[t]--;
        if (s_rumble_ticks_left[t] > 0 && --s_rumble_ticks_left[t] == 0)
            chaos_rumble_team(t, 0);
    }
}

int td5_chaos_rotation_step(int team) {
    return (team >= 0 && team < TD5_CHAOS_TEAMS) ? s_rotation_step[team] : 0;
}

int td5_chaos_swap_ticks_left(int team) {
    return (team >= 0 && team < TD5_CHAOS_TEAMS) ? s_swap_ticks_left[team] : 0;
}

int td5_chaos_swap_flash_ticks(int team) {
    return (team >= 0 && team < TD5_CHAOS_TEAMS) ? s_swap_flash_ticks[team] : 0;
}

int td5_chaos_next_milestone_label(int team, char *out, int outsz) {
    int trigger, n;
    if (!out || outsz <= 0) return 0;
    out[0] = '\0';
    if (!td5_chaos_active() || team < 0 || team >= TD5_CHAOS_TEAMS) return 0;

    trigger = chaos_effective_trigger();
    switch (trigger) {
    case TD5_CHAOS_TRIGGER_TIME: {
        int period = chaos_period_ticks();
        int left   = period - (int)(s_race_ticks % period);
        int secs   = (left + CHAOS_SIM_HZ - 1) / CHAOS_SIM_HZ;
        n = snprintf(out, (size_t)outsz, "%d:%02d", secs / 60, secs % 60);
        break;
    }
    case TD5_CHAOS_TRIGGER_LAP:
        n = snprintf(out, (size_t)outsz, "LAP %d", (int)s_last_milestone[team] + 1);
        break;
    case TD5_CHAOS_TRIGGER_CHECKPOINT:
    case TD5_CHAOS_TRIGGER_HALF_LAP: {
        int cycle = chaos_cycle_len(trigger);
        int next  = (int)s_last_milestone[team] + 1;   /* 1-based milestone index */
        n = snprintf(out, (size_t)outsz, "CP %d/%d", ((next - 1) % cycle) + 1, cycle);
        break;
    }
    default:
        return 0;   /* OFF: nothing to draw */
    }
    if (n < 0) { out[0] = '\0'; return 0; }
    if (n >= outsz) n = outsz - 1;   /* snprintf truncated */
    return n;
}

/* ========================================================================
 * SECTION: ROLES (apply the team's rotation step)
 * ======================================================================== */

int td5_chaos_row_of_seat(int seat) {
    int ts = td5_chaos_team_size();
    int team = td5_chaos_team_of_seat(seat);
    int base;
    if (ts <= 0 || team < 0) return -1;
    base = s_cfg.row_of_seat[seat];
    if (base < 0 || base >= ts) return -1;
    return (base + s_rotation_step[team] % ts) % ts;
}

TD5_ChaosRole td5_chaos_role_of_seat(int seat) {
    return td5_chaos_role_for_row(td5_chaos_team_size(), td5_chaos_row_of_seat(seat));
}

int td5_chaos_owner_of_role(int team, TD5_ChaosRole role) {
    int n = td5_chaos_seat_count();
    for (int s = 0; s < n; s++) {
        if (td5_chaos_team_of_seat(s) == team && td5_chaos_role_of_seat(s) == role)
            return s;
    }
    return -1;
}
