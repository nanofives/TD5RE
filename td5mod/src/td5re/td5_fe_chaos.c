/* ========================================================================
 * td5_fe_chaos.c -- CHAOS CO-OP frontend: the CHAOS TEAMS seat/role board
 *
 * [CHAOS CO-OP 2026-09-29] Screen_ChaosTeams (TD5_SCREEN_CHAOS_TEAMS = 54),
 * reached from MP MODE CONFIG when the locked mode is TD5_MP_MODE_CHAOS_COOP.
 * 4 / 6 / 8 LOCAL humans claim one SEAT each in one of the two team panels;
 * the ROW a seat occupies carries the control role (td5_chaos_role_for_row).
 * On START the board is committed as a TD5_ChaosConfig (td5_chaos_commit_config)
 * and the flow continues to the MP car grid with exactly TWO pickers -- one per
 * team, driven by the device of that team's row-0 seat.
 *
 * This TU also owns the frontend-side DRAFT of the chaos config (rotation
 * trigger / period / AI opponent count), which the MP MODE CONFIG rows in
 * td5_fe_race.c edit through the int32_t accessors declared in
 * td5_frontend_internal.h, and the "chaos race pending" seam the MP setup
 * path (td5_fe_mp_setup.c) reads to collapse the race to 2 cars / 2 panes.
 *
 * Design: docs/plans/CHAOS_COOP_MODE_PLAN.md sections 6.2-6.4.
 * Cross-TU seam: td5_frontend_internal.h.
 * ======================================================================== */

#include "td5_frontend.h"
#include "td5_chaos.h"
#include "td5_asset.h"         /* TD5_ColorKeyMode (td5_frontend_internal.h uses it) */
#include "td5_platform.h"
#include "td5re.h"
#include "td5_vectorui.h"      /* public VectorUI surface (shared with the HUD) */
#include "td5_font.h"          /* runtime TTF glyph cache (native menu text)    */
#include "td5_i18n.h"          /* [I18N] TR() runtime string translation        */
#include "td5_config.h"        /* shared TD5RE_* env-knob accessors             */
#include "td5_frontend_internal.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "frontend"

/* ========================================================================
 * SECTION: layout constants (640x480 design space, plan section 6.3)
 * MP bands apply: left margin 112, right edge 628, nothing below y=460.
 * ======================================================================== */

#define CT_PANEL_AX      112.0f   /* TEAM RED panel x                          */
#define CT_PANEL_BX      380.0f   /* TEAM BLUE panel x                         */
#define CT_PANEL_Y        92.0f
#define CT_PANEL_W       248.0f
#define CT_PANEL_H       198.0f
#define CT_HEADER_H       24.0f   /* coloured band at the top of each panel    */
#define CT_ROW_X_OFF       4.0f   /* seat row inset inside the panel           */
#define CT_ROW_Y0        120.0f   /* seat row 0 top                            */
#define CT_ROW_PITCH      42.0f
#define CT_ROW_W         240.0f
#define CT_ROW_H          38.0f
#define CT_WAIT_X        112.0f
#define CT_WAIT_Y        300.0f
#define CT_WAIT_W        516.0f
#define CT_WAIT_H         42.0f
#define CT_ROT_X         112
#define CT_ROT_Y         352
#define CT_ROT_W         516
#define CT_ROT_H          28
#define CT_HINT_CX       370.0f
#define CT_HINT_Y        388.0f
#define CT_START_X       322
#define CT_START_Y       404
#define CT_START_W        96
#define CT_START_H        32

/* Panels are drawn at a FIXED height for every team size so the board does not
 * jump when the player count changes; rows >= team_size are greyed and inert. */
#define CT_MAX_ROWS       4

/* Button indices on this screen (created in that order). */
#define CT_BTN_ROTATE     0
#define CT_BTN_START      1

/* Host footer focus: the host's cursor is either ON the board (s_host_in_panel)
 * or on one of the two real buttons below it (s_selected_button). */
#define CT_TITLE_GOLD    0xFFE3D708u
#define CT_EMPTY_GREY    0xFF6A6A6Au

/* ========================================================================
 * SECTION: frontend-side DRAFT config (edited on MP MODE CONFIG)
 * Held here rather than in TD5_MpModeConfig: that struct is embedded in the
 * size-asserted net wire format and CHAOS CO-OP is local-only.
 * ======================================================================== */

static int32_t s_draft_trigger      = TD5_CHAOS_TRIGGER_CHECKPOINT;
static int32_t s_draft_period_secs  = 25;
static int32_t s_draft_ai_opponents = 0;

int32_t *frontend_chaos_draft_trigger(void)      { return &s_draft_trigger; }
int32_t *frontend_chaos_draft_period(void)       { return &s_draft_period_secs; }
int32_t *frontend_chaos_draft_ai_opponents(void) { return &s_draft_ai_opponents; }

void frontend_chaos_apply_defaults(void) {
    s_draft_trigger      = TD5_CHAOS_TRIGGER_CHECKPOINT;
    s_draft_period_secs  = 25;
    s_draft_ai_opponents = 0;   /* the mode is a duel: no AI field by default */
}

/* ========================================================================
 * SECTION: "chaos race pending" seam
 * Set when the board is committed. While it holds, the MP setup path treats
 * the race as TWO cars / TWO panes and the car grid's two pickers are driven
 * by the row-0 seat device of each team.
 * ======================================================================== */

static int s_pending;                       /* 1 = board committed, race is chaos */
static int s_pane_device[TD5_CHAOS_TEAMS];  /* input source driving pane 0/1      */
static int s_saved_humans = 1;              /* s_num_human_players before commit  */

int frontend_chaos_pending(void) {
    /* Self-limiting: a pending flag can never leak out of the local MP flow or
     * survive a mode change (mp_mode_config_apply_defaults clears it too). */
    return s_pending && s_mp_flow && !s_network_active &&
           g_td5.mp_mode_config.mode == TD5_MP_MODE_CHAOS_COOP;
}

int frontend_chaos_pane_device(int pane) {
    if (pane < 0 || pane >= TD5_CHAOS_TEAMS) return 0;
    return s_pane_device[pane];
}

int frontend_chaos_ai_opponents(void) {
    int n = (int)s_draft_ai_opponents;
    if (n < 0) n = 0;
    if (n > 4) n = 4;
    return n;
}

void frontend_chaos_clear_pending(void) {
    if (!s_pending) return;
    s_pending = 0;
    /* Restore the local human count the board collapsed to 2 (the joined roster
     * itself was never touched, so the lobby/vote screens read it intact). */
    if (s_saved_humans >= 1 && s_saved_humans <= TD5_MAX_HUMAN_PLAYERS)
        s_num_human_players = s_saved_humans;
    TD5_LOG_I(LOG_TAG, "CHAOS: pending race cleared (humans restored to %d)",
              s_num_human_players);
}

/* ========================================================================
 * SECTION: board state
 * ======================================================================== */

/* Seat index == JOIN index (seat s is bound to input slot s, td5_chaos.h), so
 * the board only has to record WHICH CELL each joined player occupies. */
static int      s_cell_owner[TD5_CHAOS_TEAMS][CT_MAX_ROWS]; /* join index, -1 empty */
static int      s_cur_team[TD5_MAX_HUMAN_PLAYERS];          /* per-device cursor    */
static int      s_cur_row[TD5_MAX_HUMAN_PLAYERS];
static int      s_host_in_panel;        /* 1 = host cursor on the board, 0 = footer */
static int      s_back_confirm;         /* "BACK TO MODE OPTIONS?" modal up         */
static int      s_seat_count;           /* legal head count this entry uses (4/6/8) */
#ifndef TD5RE_RELEASE
static int      s_fake_roster;          /* 1 = dev direct-entry fake roster seeded   */
#endif

static const char *const k_chaos_trigger_names[TD5_CHAOS_TRIGGER_COUNT] = {
    "CHECKPOINT", "HALF LAP", "LAP", "TIME", "OFF"
};

/* Display name for a rotation trigger (also used by the MP MODE CONFIG rows). */
const char *frontend_chaos_trigger_name(int trigger) {
    return (trigger >= 0 && trigger < TD5_CHAOS_TRIGGER_COUNT)
           ? k_chaos_trigger_names[trigger] : "OFF";
}

const char *const *frontend_chaos_trigger_names(void) {
    return k_chaos_trigger_names;
}

/* ---- small helpers ---------------------------------------------------- */

static int chaos_team_size(void) {
    return (s_seat_count > 0) ? s_seat_count / TD5_CHAOS_TEAMS : 0;
}

static uint32_t chaos_player_color(int p) {
    /* Same rule as the other MP screens: the player's CHOSEN profile accent,
     * falling back to the built-in per-slot palette when none was picked. */
    uint32_t rgb = 0;
    if (p >= 0 && p < TD5_MAX_HUMAN_PLAYERS)
        rgb = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;
    if (rgb == 0)
        rgb = k_mp_player_colors[((p < 0) ? 0 : p) % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu;
    return rgb | 0xFF000000u;
}

static void chaos_small_centered(float cx_px, float y_px, const char *t,
                                 uint32_t col, float sx, float sy) {
    float gsx = (sx < sy) ? sx : sy;
    fe_draw_small_text(cx_px - fe_measure_small_text(t) * 0.5f * gsx, y_px, t, col, sx, sy);
}

/* Left-aligned small text condensed to fit `max_w_px` screen px (same shrink
 * floor as the MP panes' mp_simul_small_centered_fit). */
static void chaos_small_fit(float x_px, float y_px, const char *t, uint32_t col,
                            float sx, float sy, float max_w_px) {
    float w = fe_measure_small_text(t) * fe_glyph_sx(sx, sy);
    float s = 1.0f;
    if (max_w_px > 0.0f && w > max_w_px) {
        s = max_w_px / w;
        if (s < 0.55f) s = 0.55f;
    }
    fe_draw_small_text(x_px, y_px, t, col, sx * s, sy * s);
}

/* "KEYBOARD" / "PAD 3" for an input-source index (0 = keyboard, >=1 joystick). */
static void chaos_device_label(int dev, char *out, int outsz) {
    if (dev <= 0) snprintf(out, outsz, "%s", TR("KEYBOARD"));
    else          snprintf(out, outsz, TR("PAD %d"), dev);
}

static int chaos_cell_of_player(int p, int *out_team, int *out_row) {
    int t, r;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++)
        for (r = 0; r < CT_MAX_ROWS; r++)
            if (s_cell_owner[t][r] == p) {
                if (out_team) *out_team = t;
                if (out_row)  *out_row  = r;
                return 1;
            }
    return 0;
}

static int chaos_all_seated(void) {
    int t, r, ts = chaos_team_size();
    for (t = 0; t < TD5_CHAOS_TEAMS; t++)
        for (r = 0; r < ts; r++)
            if (s_cell_owner[t][r] < 0) return 0;
    return 1;
}

static int chaos_empty_seat_count(void) {
    int t, r, ts = chaos_team_size(), n = 0;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++)
        for (r = 0; r < ts; r++)
            if (s_cell_owner[t][r] < 0) n++;
    return n;
}

/* Keyboard seats currently on the board (plan section 5: at most one). */
static int chaos_keyboard_seat_count(void) {
    int t, r, ts = chaos_team_size(), n = 0;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++)
        for (r = 0; r < ts; r++) {
            int o = s_cell_owner[t][r];
            if (o >= 0 && s_mp_join_device[o] == 0) n++;
        }
    return n;
}

/* ========================================================================
 * SECTION: entry / auto-seating
 * ======================================================================== */

/* The live LOCAL joined head count this board seats. */
static int chaos_joined_count(void) {
    int n = s_mp_joined_count;
    if (n < 0) n = 0;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    return n;
}

/* 1 when CHAOS CO-OP may be picked with the CURRENT lobby roster. Shared with
 * the MP MODE VOTE gate (plan section 6.2) so the row and the board agree.
 * `why` (optional) receives a short reason string when the answer is 0. */
int frontend_chaos_mode_selectable(const char **why) {
    int n = chaos_joined_count();
    if (s_network_active || s_mp_net_config) {
        if (why) *why = TR("LOCAL PLAY ONLY - NOT AVAILABLE OVER THE NETWORK");
        return 0;
    }
    if (frontend_mp_ai_player_count() > 0) {
        if (why) *why = TR("EVERY SEAT NEEDS A REAL CONTROLLER - REMOVE THE AI PLAYERS");
        return 0;
    }
    if (!td5_chaos_count_is_legal(n)) {
        if (why) *why = TR("NEEDS EXACTLY 4, 6 OR 8 LOCAL PLAYERS");
        return 0;
    }
    if (why) *why = NULL;
    return 1;
}

/* Alternating RED / BLUE by join order, all locked (plan section 6.4): a group
 * that does not care can press START immediately. */
static void chaos_auto_seat(int n) {
    int p, t, r;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++)
        for (r = 0; r < CT_MAX_ROWS; r++) s_cell_owner[t][r] = -1;
    for (p = 0; p < n; p++) {
        t = p % TD5_CHAOS_TEAMS;
        r = p / TD5_CHAOS_TEAMS;
        if (r < CT_MAX_ROWS) s_cell_owner[t][r] = p;
    }
}

static void chaos_screen_init(void) {
    int p, n;

    frontend_chaos_clear_pending();   /* re-entering the board un-does a prior commit */

#ifndef TD5RE_RELEASE
    /* Dev direct entry (--StartScreen=54 --StartScreenDirect=1) has no lobby
     * behind it. Seed a legal fake roster of 4 so the board renders a sane
     * layout for a framedump; every fake seat reads whatever device sits in
     * s_mp_join_device[] (all keyboard on a cold jump). */
    s_fake_roster = 0;
    if (!s_mp_flow && !td5_chaos_count_is_legal(chaos_joined_count())) {
        s_mp_joined_count = 4;
        for (p = 0; p < 4; p++) s_mp_slot_is_ai[p] = 0;
        s_fake_roster = 1;
        TD5_LOG_W(LOG_TAG,
                  "CHAOS TEAMS: direct entry with no lobby - seeded a FAKE roster of 4 "
                  "(dev build only; devices=%d/%d/%d/%d)",
                  s_mp_join_device[0], s_mp_join_device[1],
                  s_mp_join_device[2], s_mp_join_device[3]);
    }
#endif

    n = chaos_joined_count();
    if (!td5_chaos_count_is_legal(n)) {
        /* Should be unreachable: the vote row is greyed for an illegal count.
         * Clamp DOWN to the nearest legal count so the board can still render. */
        int legal = (n >= 8) ? 8 : (n >= 6) ? 6 : 4;
        TD5_LOG_W(LOG_TAG, "CHAOS TEAMS: illegal joined count %d -> seating %d", n, legal);
        n = legal;
    }
    s_seat_count = n;

    frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
    frontend_reset_buttons();
    {
        int b = frontend_create_button("", CT_ROT_X, CT_ROT_Y, CT_ROT_W, CT_ROT_H);
        if (b >= 0) s_buttons[b].is_selector = 1;     /* LEFT/RIGHT value widget */
    }
    frontend_create_button(TR("START"), CT_START_X, CT_START_Y, CT_START_W, CT_START_H);

    chaos_auto_seat(n);
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) {
        int t = 0, r = 0;
        if (!chaos_cell_of_player(p, &t, &r)) { t = 0; r = 0; }
        s_cur_team[p] = t;
        s_cur_row[p]  = r;
        s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
    }
    s_host_in_panel   = 1;
    s_back_confirm    = 0;
    s_selected_button = CT_BTN_START;
    s_anim_complete   = 1;
    s_inner_state     = 1;
    TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: enter (%d seats, %d per team, trigger=%s period=%ds)",
              s_seat_count, chaos_team_size(),
              frontend_chaos_trigger_name((int)s_draft_trigger), (int)s_draft_period_secs);
}

/* ========================================================================
 * SECTION: seat actions (A take / unlock / swap, B unlock)
 * ======================================================================== */

static void chaos_seat_action_a(int p, int team, int row) {
    int owner = s_cell_owner[team][row];
    int my_t = 0, my_r = 0;
    int seated = chaos_cell_of_player(p, &my_t, &my_r);

    if (owner == p) {                       /* own locked row -> unlock */
        s_cell_owner[team][row] = -1;
        frontend_play_sfx(5);
        TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: P%d left team %d row %d", p + 1, team, row);
        return;
    }
    if (owner < 0) {                        /* empty row -> take it, locked */
        /* Plan section 5: the keyboard can hold at most ONE seat. Reject a
         * second keyboard claim audibly instead of seating it silently. */
        if (s_mp_join_device[p] == 0 && !seated && chaos_keyboard_seat_count() >= 1) {
            frontend_play_sfx(10);
            TD5_LOG_W(LOG_TAG, "CHAOS TEAMS: rejected a SECOND keyboard seat (P%d)", p + 1);
            return;
        }
        if (seated) s_cell_owner[my_t][my_r] = -1;
        s_cell_owner[team][row] = p;
        frontend_play_sfx(3);
        TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: P%d took team %d row %d", p + 1, team, row);
        return;
    }
    /* Row owned by someone else -> SWAP both players, both stay locked. */
    if (seated) {
        s_cell_owner[my_t][my_r] = owner;
        s_cell_owner[team][row]  = p;
        s_cur_team[owner] = my_t;
        s_cur_row[owner]  = my_r;
    } else {
        s_cell_owner[team][row] = p;        /* unseated claimer displaces them */
    }
    frontend_play_sfx(3);
    TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: P%d swapped with P%d (team %d row %d)",
              p + 1, owner + 1, team, row);
}

static void chaos_seat_action_b(int p) {
    int t = 0, r = 0;
    if (!chaos_cell_of_player(p, &t, &r)) return;
    s_cell_owner[t][r] = -1;
    frontend_play_sfx(5);
    TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: P%d unlocked (team %d row %d)", p + 1, t, r);
}

/* ========================================================================
 * SECTION: commit + route to the car grid
 * ======================================================================== */

static void chaos_commit_and_start(void) {
    TD5_ChaosConfig cfg;
    int t, r, ts = chaos_team_size();

    memset(&cfg, 0, sizeof cfg);
    cfg.trigger     = s_draft_trigger;
    cfg.period_secs = s_draft_period_secs;
    cfg.seat_count  = s_seat_count;
    for (t = 0; t < TD5_CHAOS_TEAMS; t++) {
        for (r = 0; r < ts; r++) {
            int seat = s_cell_owner[t][r];
            if (seat < 0 || seat >= TD5_CHAOS_MAX_SEATS) continue;
            cfg.team_of_seat[seat]   = t;
            cfg.row_of_seat[seat]    = r;
            /* s_mp_join_device[] is ALREADY in input-source encoding (0 =
             * keyboard, >=1 = 1-based joystick enum index) -- it is fed
             * verbatim to td5_input_set_input_source() by the lobby and the
             * car grid, so no conversion is needed here. */
            cfg.device_of_seat[seat] = s_mp_join_device[seat];
        }
    }
    td5_chaos_commit_config(&cfg);

    /* Two cars from here on: the car grid, the pane layout and the race
     * schedule all see 2 humans; the seat table lives only in TD5_ChaosConfig. */
    s_pane_device[0] = (s_cell_owner[0][0] >= 0) ? s_mp_join_device[s_cell_owner[0][0]] : 0;
    s_pane_device[1] = (s_cell_owner[1][0] >= 0) ? s_mp_join_device[s_cell_owner[1][0]] : 0;
    s_saved_humans   = s_num_human_players;
    s_pending        = 1;
    s_num_human_players = TD5_CHAOS_TEAMS;

    for (t = 0; t < TD5_MAX_HUMAN_PLAYERS; t++) {
        s_mp_player_ready[t]  = 0;
        s_mp_pane_nav_prev[t] = mp_simul_player_nav(t);
    }

    TD5_LOG_I(LOG_TAG,
              "CHAOS TEAMS: committed %d seats (RED rows %d/%d/%d/%d, BLUE rows %d/%d/%d/%d), "
              "trigger=%s period=%ds ai=%d; panes driven by dev %d / %d",
              cfg.seat_count,
              s_cell_owner[0][0], s_cell_owner[0][1], s_cell_owner[0][2], s_cell_owner[0][3],
              s_cell_owner[1][0], s_cell_owner[1][1], s_cell_owner[1][2], s_cell_owner[1][3],
              frontend_chaos_trigger_name((int)s_draft_trigger),
              (int)s_draft_period_secs, frontend_chaos_ai_opponents(),
              s_pane_device[0], s_pane_device[1]);

    frontend_play_sfx(3);
    td5_plat_input_flush_nav();
    s_mp_phase    = 1;               /* car GRID (phase 1), not the profile setup */
    s_inner_state = 0;
    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
}

/* ========================================================================
 * SECTION: screen handler
 * ======================================================================== */

void Screen_ChaosTeams(void) {
    int p, n, ts, d;
    int host_start = 0, host_back = 0;

    if (s_inner_state == 0) { chaos_screen_init(); return; }

    n  = s_seat_count;
    ts = chaos_team_size();
    if (ts <= 0) { td5_frontend_set_screen(TD5_SCREEN_MP_MODE_CONFIG); return; }

    if (frontend_mp_setup_disconnect_check(n)) return;

    /* "BACK TO MODE OPTIONS?" confirm modal -- host only. */
    if (s_back_confirm) {
        uint32_t hb   = mp_simul_player_nav(0);
        uint32_t he   = hb & ~s_mp_pane_nav_prev[0];
        s_mp_pane_nav_prev[0] = hb;
        if ((he & 0x10) || s_input_ready) {
            s_back_confirm = 0;
            frontend_play_sfx(3);
            td5_plat_input_flush_nav();
            td5_frontend_set_screen(TD5_SCREEN_MP_MODE_CONFIG);
            return;
        }
        if ((he & 0x20) || frontend_check_escape()) {
            s_back_confirm = 0;
            frontend_play_sfx(5);
            td5_plat_input_flush_nav();
        }
        return;
    }

    /* START is inert until every seat is filled; the disabled frame + the sfx
     * 10 rejection below are the only feedback the host gets. */
    s_buttons[CT_BTN_START].disabled = chaos_all_seated() ? 0 : 1;

    /* Per-DEVICE cursors: each joined controller moves its OWN cursor, exactly
     * like MP MODE VOTE / MP TEAM SELECT. The host (join index 0) additionally
     * owns the footer (ROTATE AT + START). */
    for (p = 0; p < n; p++) {
        uint32_t bits = mp_simul_player_nav(p);
        uint32_t edge = bits & ~s_mp_pane_nav_prev[p];
        int in_panel  = (p != 0) || s_host_in_panel;
        s_mp_pane_nav_prev[p] = bits;

        if (s_cur_team[p] < 0 || s_cur_team[p] >= TD5_CHAOS_TEAMS) s_cur_team[p] = 0;
        if (s_cur_row[p] < 0 || s_cur_row[p] >= ts)                s_cur_row[p]  = 0;

        if (in_panel) {
            if (edge & 4) {                       /* UP (wraps to the last row) */
                s_cur_row[p] = (s_cur_row[p] > 0) ? s_cur_row[p] - 1 : ts - 1;
                frontend_play_sfx(2);
            }
            if (edge & 8) {                       /* DOWN */
                if (s_cur_row[p] + 1 < ts) {
                    s_cur_row[p]++;
                    frontend_play_sfx(2);
                } else if (p == 0) {              /* host drops into the footer */
                    s_host_in_panel   = 0;
                    s_selected_button = CT_BTN_ROTATE;
                    frontend_play_sfx(2);
                } else {
                    s_cur_row[p] = 0;
                    frontend_play_sfx(2);
                }
            }
            if (edge & 1) { if (s_cur_team[p] != 0) { s_cur_team[p] = 0; frontend_play_sfx(2); } }
            if (edge & 2) { if (s_cur_team[p] != 1) { s_cur_team[p] = 1; frontend_play_sfx(2); } }
            if (edge & 0x10) chaos_seat_action_a(p, s_cur_team[p], s_cur_row[p]);
            if (edge & 0x20) chaos_seat_action_b(p);
        } else {                                   /* host on the footer rows */
            if (edge & 4) {
                if (s_selected_button == CT_BTN_START) {
                    s_selected_button = CT_BTN_ROTATE;
                } else {
                    s_host_in_panel = 1;
                    s_cur_row[0]    = ts - 1;
                }
                frontend_play_sfx(2);
            }
            if (edge & 8) {
                if (s_selected_button == CT_BTN_ROTATE) {
                    s_selected_button = CT_BTN_START;
                    frontend_play_sfx(2);
                }
            }
            if (edge & 0x20) host_back = 1;
            if (edge & 0x10) {
                if (s_selected_button == CT_BTN_START) host_start = 1;
                else {                              /* A on the selector row = step it */
                    s_draft_trigger = (s_draft_trigger + 1) % TD5_CHAOS_TRIGGER_COUNT;
                    frontend_play_sfx(2);
                }
            }
        }
    }

    /* Keyboard / mouse host. The seat cursor already consumed the host's arrow
     * keys above (mp_simul_player_nav reads them directly for device 0), so the
     * shared nav's own move is overridden here -- the same override MP MODE
     * VOTE uses to stop a non-host pad dragging the host highlight. */
    if (!s_host_in_panel) {
        d = frontend_option_delta();
        if (d) {
            int v = (int)s_draft_trigger + d;
            if (v < 0) v = TD5_CHAOS_TRIGGER_COUNT - 1;
            if (v >= TD5_CHAOS_TRIGGER_COUNT) v = 0;
            if (v != (int)s_draft_trigger) { s_draft_trigger = v; frontend_play_sfx(2); }
        }
    }
    if (s_input_ready && frontend_input_confirm_was_mouse() && s_button_index >= 0) {
        s_host_in_panel   = 0;
        s_selected_button = s_button_index;
        if (s_button_index == CT_BTN_START) host_start = 1;
    }
    if (s_selected_button < 0)              s_selected_button = CT_BTN_ROTATE;
    if (s_selected_button > CT_BTN_START)   s_selected_button = CT_BTN_START;
    if (frontend_check_escape()) host_back = 1;

    if (host_start) {
        if (!chaos_all_seated()) {
            frontend_play_sfx(10);              /* error/disabled cue */
            TD5_LOG_I(LOG_TAG, "CHAOS TEAMS: START rejected - %d seat(s) still empty",
                      chaos_empty_seat_count());
        } else {
            chaos_commit_and_start();
        }
        return;
    }
    if (host_back) {
        s_back_confirm = 1;
        frontend_play_sfx(2);
        td5_plat_input_flush_nav();
        for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
            s_mp_pane_nav_prev[p] = mp_simul_player_nav(p);
    }
}

/* ========================================================================
 * SECTION: render (POST-button pass -- composites on top of the frames)
 * ======================================================================== */

/* One seat row inside a team panel. */
static void chaos_draw_seat_row(int team, int row, int ts, float sx, float sy) {
    float px = (team == 0) ? CT_PANEL_AX : CT_PANEL_BX;
    float rx = px + CT_ROW_X_OFF;
    float ry = CT_ROW_Y0 + (float)row * CT_ROW_PITCH;
    int   live  = (row < ts);
    int   owner = live ? s_cell_owner[team][row] : -1;
    uint32_t col = (owner >= 0) ? chaos_player_color(owner) : CT_EMPTY_GREY;
    char buf[48];

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(rx * sx, ry * sy, CT_ROW_W * sx, CT_ROW_H * sy,
                 live ? ((owner >= 0) ? 0xA0141C28u : 0x80101018u) : 0x60101014u,
                 -1, 0, 0, 1, 1);
    /* 4px colour bar in the player colour (grey while the row is empty). */
    fe_draw_quad(rx * sx, ry * sy, 4.0f * sx, CT_ROW_H * sy, col, -1, 0, 0, 1, 1);

    /* row number */
    snprintf(buf, sizeof buf, "%d", row + 1);
    fe_draw_small_text((rx + 10.0f) * sx, (ry + 14.0f) * sy, buf,
                       live ? 0xFFD0D8E4u : CT_EMPTY_GREY, sx, sy);

    if (!live) {
        fe_draw_small_text((rx + 26.0f) * sx, (ry + 14.0f) * sy, "--", CT_EMPTY_GREY, sx, sy);
        return;
    }

    /* role badge (small caps) */
    fe_draw_small_text((rx + 26.0f) * sx, (ry + 5.0f) * sy,
                       td5_tr(td5_chaos_role_name(td5_chaos_role_for_row(ts, row))),
                       0xFFE6ECF4u, sx, sy);

    if (owner < 0) {
        fe_draw_small_text((rx + 26.0f) * sx, (ry + 21.0f) * sy, "--", CT_EMPTY_GREY, sx, sy);
        return;
    }

    /* player token + device label, condensed so a long label cannot overrun */
    snprintf(buf, sizeof buf, "P%d", owner + 1);
    fe_draw_small_text((rx + 26.0f) * sx, (ry + 21.0f) * sy, buf, col, sx, sy);
    chaos_device_label(s_mp_join_device[owner], buf, sizeof buf);
    chaos_small_fit((rx + 62.0f) * sx, (ry + 21.0f) * sy, buf, 0xFFB8C0CCu,
                    sx, sy, (CT_ROW_W - 82.0f) * sx);
    /* ready tick on the right (a seated row is a locked row). */
    fe_draw_small_text((rx + CT_ROW_W - 16.0f) * sx, (ry + 14.0f) * sy, "*",
                       0xFF40FF40u, sx, sy);
}

/* Per-device cursor arrows to the LEFT of the row each controller is hovering. */
static void chaos_draw_cursors(int n, int ts, float sx, float sy) {
    int team, row, p;
    for (team = 0; team < TD5_CHAOS_TEAMS; team++) {
        for (row = 0; row < ts; row++) {
            float px = (team == 0) ? CT_PANEL_AX : CT_PANEL_BX;
            float ry = CT_ROW_Y0 + (float)row * CT_ROW_PITCH;
            int stack = 0;
            for (p = 0; p < n; p++) {
                if (p == 0 && !s_host_in_panel) continue;   /* host is on the footer */
                if (s_cur_team[p] != team || s_cur_row[p] != row) continue;
                td5_vui_arrow((px - 14.0f - (float)stack * 13.0f) * sx,
                              (ry + CT_ROW_H * 0.5f - 8.0f) * sy,
                              12.0f * sx, 14.0f * sy, 1, chaos_player_color(p));
                stack++;
            }
        }
    }
}

void frontend_chaos_teams_render(float sx, float sy) {
    int team, row, p, n = s_seat_count, ts = chaos_team_size();
    char buf[80];

    if (ts <= 0) return;

    fe_race_draw_screen_title(TR("CHAOS CO-OP - TEAMS AND ROLES"),
                              FE_TITLE_LEFT_X * sx, 17.0f * sy, CT_TITLE_GOLD, sx, sy);

    /* player-count badge, right aligned against the MP right edge */
    snprintf(buf, sizeof buf, TR("%d PLAYERS"), n);
    {
        float gsx = (sx < sy) ? sx : sy;
        fe_draw_small_text((FE_MP_RIGHT_EDGE * sx) - fe_measure_small_text(buf) * gsx,
                           22.0f * sy, buf, 0xFFB8C0CCu, sx, sy);
    }

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (team = 0; team < TD5_CHAOS_TEAMS; team++) {
        float px = (team == 0) ? CT_PANEL_AX : CT_PANEL_BX;
        uint32_t band = (team == 0) ? 0xC0801820u : 0xC0182C80u;
        fe_draw_quad(px * sx, CT_PANEL_Y * sy, CT_PANEL_W * sx, CT_PANEL_H * sy,
                     0xA00C1018u, -1, 0, 0, 1, 1);
        fe_draw_quad(px * sx, CT_PANEL_Y * sy, CT_PANEL_W * sx, CT_HEADER_H * sy,
                     band, -1, 0, 0, 1, 1);
        chaos_small_centered((px + CT_PANEL_W * 0.5f) * sx,
                             (CT_PANEL_Y + (CT_HEADER_H - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                             (team == 0) ? TR("TEAM RED") : TR("TEAM BLUE"),
                             0xFFFFFFFFu, sx, sy);
        for (row = 0; row < CT_MAX_ROWS; row++)
            chaos_draw_seat_row(team, row, ts, sx, sy);
    }
    chaos_draw_cursors(n, ts, sx, sy);

    /* WAITING strip: every joined player without a seat. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(CT_WAIT_X * sx, CT_WAIT_Y * sy, CT_WAIT_W * sx, CT_WAIT_H * sy,
                 0x900C1018u, -1, 0, 0, 1, 1);
    fe_draw_small_text((CT_WAIT_X + 8.0f) * sx, (CT_WAIT_Y + 6.0f) * sy,
                       TR("WAITING"), 0xFF98A0B0u, sx, sy);
    {
        float wx = CT_WAIT_X + 8.0f;
        int   shown = 0;
        for (p = 0; p < n; p++) {
            char dev[24];
            if (chaos_cell_of_player(p, NULL, NULL)) continue;
            shown++;
            chaos_device_label(s_mp_join_device[p], dev, sizeof dev);
            snprintf(buf, sizeof buf, "P%d  %s", p + 1, dev);
            fe_draw_quad(wx * sx, (CT_WAIT_Y + 20.0f) * sy, 3.0f * sx, 14.0f * sy,
                         chaos_player_color(p), -1, 0, 0, 1, 1);
            fe_draw_small_text((wx + 7.0f) * sx, (CT_WAIT_Y + 23.0f) * sy, buf,
                               0xFFE6ECF4u, sx, sy);
            wx += 8.0f + fe_measure_small_text(buf) + 14.0f;
            if (wx > CT_WAIT_X + CT_WAIT_W - 40.0f) break;
        }
        if (shown == 0)
            fe_draw_small_text((wx + 64.0f) * sx, (CT_WAIT_Y + 23.0f) * sy,
                               TR("NOBODY - EVERY SEAT IS TAKEN"), 0xFF80FF80u, sx, sy);
    }

    /* ROTATE AT selector row (label left, value + arrows right). */
    {
        float ty = (float)CT_ROT_Y + 1.0f;
        float ay = (float)CT_ROT_Y + (float)CT_ROT_H * 0.5f - 7.0f;
        float val_l = (float)CT_ROT_X + 300.0f;
        float val_r = (float)CT_ROT_X + (float)CT_ROT_W - 24.0f;
        float valc  = (val_l + 12.0f + val_r) * 0.5f;
        fe_draw_text(((float)CT_ROT_X + 16.0f) * sx, ty * sy, TR("ROTATE AT"),
                     0xFFE6ECF4u, sx, sy);
        fe_draw_text_centered(valc * sx, ty * sy,
                              td5_tr(frontend_chaos_trigger_name((int)s_draft_trigger)),
                              0xFFFFFFFFu, sx, sy);
        td5_vui_arrow(val_l * sx, ay * sy, 12.0f * sx, 14.0f * sy, 0, 0xFF7995FFu);
        td5_vui_arrow(val_r * sx, ay * sy, 12.0f * sx, 14.0f * sy, 1, 0xFF7995FFu);
    }

    /* hint line + START status */
    chaos_small_centered(CT_HINT_CX * sx, CT_HINT_Y * sy,
                         TR("Roles shift down one row. Row 1 wraps to the bottom. Team only."),
                         0xFF98A0B0u, sx, sy);
    {
        int empty = chaos_empty_seat_count();
        if (empty == 0) snprintf(buf, sizeof buf, "%s", TR("ALL SEATS FILLED"));
        else if (empty == 1) snprintf(buf, sizeof buf, "%s", TR("1 SEAT STILL EMPTY"));
        else snprintf(buf, sizeof buf, TR("%d SEATS STILL EMPTY"), empty);
        fe_draw_small_text((float)(CT_START_X + CT_START_W + 10) * sx,
                           (float)(CT_START_Y + 12) * sy, buf,
                           empty ? 0xFFFFC060u : 0xFF80FF80u, sx, sy);
        chaos_small_centered(200.0f * sx, (float)(CT_START_Y + 12) * sy,
                             TR("D-PAD MOVE   A TAKE/SWAP   B LEAVE"), 0xFFB0B0B0u, sx, sy);
    }

#ifndef TD5RE_RELEASE
    if (s_fake_roster)
        chaos_small_centered(320.0f * sx, 446.0f * sy,
                             "DEV: FAKE ROSTER (no lobby behind this screen)",
                             0xFFFFC060u, sx, sy);
#endif

    if (s_back_confirm)
        mp_confirm_modal_render(sx, sy, TR("BACK TO MODE OPTIONS?"));
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}
