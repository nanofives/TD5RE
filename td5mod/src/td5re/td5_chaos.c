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
 * SECTION: ROTATION  (stub — GAME zone owns this block)
 * ======================================================================== */

static int32_t s_rotation_step[TD5_CHAOS_TEAMS];

void td5_chaos_race_begin(void) {
    memset(s_rotation_step, 0, sizeof(s_rotation_step));
}

void td5_chaos_tick(void) {
}

int td5_chaos_rotation_step(int team) {
    return (team >= 0 && team < TD5_CHAOS_TEAMS) ? s_rotation_step[team] : 0;
}

int td5_chaos_swap_ticks_left(int team) {
    (void)team;
    return 0;
}

int td5_chaos_swap_flash_ticks(int team) {
    (void)team;
    return 0;
}

int td5_chaos_next_milestone_label(int team, char *out, int outsz) {
    (void)team;
    if (out && outsz > 0) out[0] = '\0';
    return 0;
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
