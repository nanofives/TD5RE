/* ========================================================================
 * td5_chaos.h — CHAOS CO-OP mode: one car per team, many drivers (PORT-ONLY)
 *
 * 4 / 6 / 8 LOCAL humans split into 2 teams (RED drives racer slot 0, BLUE
 * drives racer slot 1). Every human ("seat") holds ONE control axis of their
 * team's car; roles rotate inside the team when the car reaches a track
 * milestone, after a 3 s countdown. No original-binary counterpart.
 * Full design: docs/plans/CHAOS_COOP_MODE_PLAN.md.
 *
 * INDEX SPACES (the whole point of the mode — do not conflate them):
 *   seat         0..seat_count-1   one human + one controller. Seat s is bound
 *                                  to INPUT SLOT s (s_control_bits[s]).
 *   team         0..1              team t drives RACER SLOT t and VIEWPORT t.
 *   row          0..team_size-1    position inside the team panel; the ROW is
 *                                  what carries the role (see role table).
 * g_td5.num_human_players == 2 while the mode runs (two cars, two panes);
 * the input poll covers seat_count slots (td5_chaos_input_slot_count()).
 *
 * ROLE TABLE (td5_chaos_role_for_row):
 *   team of 2: STEER, PEDALS
 *   team of 3: LEFT, RIGHT, PEDALS
 *   team of 4: LEFT, RIGHT, THROTTLE, BRAKE
 *
 * MODULE SPLIT:
 *   td5_chaos.c       seat table, role map, rotation timer, HUD queries,
 *                     TD5RE_CHAOS_FAKE_SEATS dev knob          (zone: GAME)
 *   td5_chaos_fold.c  pure N-seat -> 2-car input fold          (zone: INPUT)
 * ======================================================================== */
#ifndef TD5_CHAOS_H
#define TD5_CHAOS_H

#include <stdint.h>
#include "td5_types.h"   /* TD5_ChaosRole, TD5_ChaosTrigger, TD5_CHAOS_MAX_SEATS */

/* Swap countdown length once a milestone arms a rotation (sim ticks @30 Hz). */
#define TD5_CHAOS_SWAP_COUNTDOWN_TICKS 90   /* 3 s */
/* Post-swap HUD flash length (sim ticks @30 Hz). */
#define TD5_CHAOS_SWAP_FLASH_TICKS     45   /* 1.5 s */

/* ---- Committed configuration -------------------------------------------- */

/* Written by the CHAOS TEAMS screen (td5_chaos_commit_config), read at race
 * start. Module-owned on purpose: TD5_MpModeConfig is embedded in the net wire
 * struct (TD5_NetRaceConfig, size-asserted in td5_net.h) and this mode is
 * local-only, so its state must not grow the protocol. */
typedef struct TD5_ChaosConfig {
    int32_t trigger;                              /* TD5_ChaosTrigger          */
    int32_t period_secs;                          /* TIME trigger, 10..60      */
    int32_t seat_count;                           /* 4/6/8, else mode inert    */
    int32_t team_of_seat[TD5_CHAOS_MAX_SEATS];    /* 0 = RED (slot 0), 1 = BLUE */
    int32_t row_of_seat[TD5_CHAOS_MAX_SEATS];     /* starting row in the team  */
    int32_t device_of_seat[TD5_CHAOS_MAX_SEATS];  /* input source: 0 = keyboard,
                                                   * >=1 = 1-based joystick enum
                                                   * index; bound to input slot
                                                   * == seat index             */
} TD5_ChaosConfig;

/* Copy `cfg` in as the committed config (NULL = clear to inert). */
void td5_chaos_commit_config(const TD5_ChaosConfig *cfg);
/* The committed config (never NULL; seat_count 0 when nothing committed). */
const TD5_ChaosConfig *td5_chaos_get_config(void);

/* ---- Rules (pure, usable from the frontend before any race) ------------- */

/* 1 if `players` is a legal CHAOS CO-OP head count (4, 6 or 8). */
int td5_chaos_count_is_legal(int players);

/* Role carried by `row` in a team of `team_size` (2..4). ROLE_NONE if out of range. */
TD5_ChaosRole td5_chaos_role_for_row(int team_size, int row);

/* Short upper-case display name ("LEFT", "PEDALS", ...). Never NULL. */
const char *td5_chaos_role_name(TD5_ChaosRole role);

/* THE seat colour. Opaque 0xAARRGGBB, one DISTINCT hue per seat 0..7, so the
 * CHAOS TEAMS board and the in-race role strip label the same player with the
 * same colour. Pure: no activation gate, no race state — safe from the frontend
 * before a race and from the HUD preview harness. `seat` is wrapped modulo
 * TD5_CHAOS_MAX_SEATS, so it never returns a black/garbage colour.
 *
 * Use this instead of a per-slot wheel: hud_filler_slot_color() spreads only 6
 * hues, so at 8 seats it gave seats 0 and 6 the same colour — and with the
 * board's alternating RED/BLUE seating those two are on the SAME team, i.e. two
 * identically-coloured rows in one pane (found in 8-player framedumps). */
uint32_t td5_chaos_seat_color(int seat);

/* ---- Activation / seat table -------------------------------------------- */

/* 1 while CHAOS CO-OP is selected and usable: g_td5.mp_mode_config.mode ==
 * CHAOS_COOP, committed seat count legal, not network. Cheap; safe per tick. */
int td5_chaos_active(void);

int td5_chaos_seat_count(void);              /* 0 when inactive */
int td5_chaos_team_size(void);               /* seat_count / 2, 0 when inactive */
int td5_chaos_team_of_seat(int seat);        /* 0/1, -1 if out of range */
int td5_chaos_device_of_seat(int seat);      /* input source, -1 if out of range */

/* Number of INPUT slots the race poll must cover: seat_count while active,
 * 0 otherwise (caller keeps its normal count). Used at td5_game.c's
 * td5_input_set_active_players() call. */
int td5_chaos_input_slot_count(void);

/* ---- Roles (apply the team's current rotation step) --------------------- */

int           td5_chaos_row_of_seat(int seat);     /* current row, -1 if n/a */
TD5_ChaosRole td5_chaos_role_of_seat(int seat);    /* current role */
int           td5_chaos_owner_of_role(int team, TD5_ChaosRole role); /* seat or -1 */

/* ---- Race lifecycle (called from td5_game.c) ---------------------------- */

/* Per-race reset from the committed config. Call at race init, after
 * the mode config is final. Inert (and cheap) when the mode is not selected. */
void td5_chaos_race_begin(void);

/* Once per FIXED sim tick, inside the !paused fixed-step block. Detects
 * milestones per team, runs the swap countdown, applies rotations. */
void td5_chaos_tick(void);

/* ---- HUD queries (read-only) -------------------------------------------- */

int td5_chaos_rotation_step(int team);     /* rotations applied so far */
int td5_chaos_swap_ticks_left(int team);   /* >0 while a swap countdown runs */
int td5_chaos_swap_flash_ticks(int team);  /* >0 during the post-swap flash */
/* Writes the "next swap" readout for the HUD strip ("CP 3/5", "LAP 2",
 * "0:12", "" when OFF). Returns chars written (0 = nothing to draw). */
int td5_chaos_next_milestone_label(int team, char *out, int outsz);

/* ---- Input fold (td5_chaos_fold.c) — PURE, no globals besides roles ----- */

/* Fold the polled seat words into the two car words.
 *   seat_bits/seat_ax/seat_ay: the raw per-INPUT-SLOT words as the poll wrote
 *     them (index = seat), count = seat_count. Analog is packed in the bits
 *     (TD5_INPUT_ANALOG_X/Y_FLAG); seat_ax/ay are the signed re-centred axes.
 *   out_*[TD5_CHAOS_TEAMS]: the synthesized word + axes for racer slot 0/1.
 * Returns 1 when it wrote the outputs, 0 (outputs untouched) when inactive.
 * Rules: docs/plans/CHAOS_COOP_MODE_PLAN.md section 4. */
int td5_chaos_fold_inputs(const uint32_t *seat_bits, const int16_t *seat_ax,
                          const int16_t *seat_ay, int seat_count,
                          uint32_t *out_bits, int16_t *out_ax, int16_t *out_ay);

#endif /* TD5_CHAOS_H */
