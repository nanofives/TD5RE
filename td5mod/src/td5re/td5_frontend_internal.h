/*
 * td5_frontend_internal.h -- shared state surface of the frontend module.
 *
 * The frontend is split across several TUs (td5_frontend.c core + per-screen
 * cluster files td5_fe_*.c). Everything here is INTERNAL to the frontend --
 * other modules must keep using the public td5_frontend.h API. Symbols listed
 * here used to be file-scope statics of the monolithic td5_frontend.c; they
 * are documented at their definitions in td5_frontend.c.
 */

#ifndef TD5_FRONTEND_INTERNAL_H
#define TD5_FRONTEND_INTERNAL_H

#include <stdint.h>
#include "td5_types.h"

/* ---- car roster bounds (carmodel.nfo order) ---- */
#define TD5_BASE_CAR_COUNT 37
#define TD5_CAR_COUNT      76

/* ---- frontend surface cache ---- */
#define FE_MAX_SURFACES    31
#define FE_SURFACE_PAGE_BASE 900  /* texture pages 900-931 reserved for frontend */

typedef struct {
    int in_use;
    int tex_page;
    int width, height;
    char source_name[128];
    char source_archive[128];
    char png_path[256];         /* resolved PNG path for recovery (empty = ZIP fallback) */
    uint32_t load_seq;          /* [PERF] monotonic use-order for the background LRU cache */
} FE_Surface;

/* ---- TD6 colour-panel layout (640x480 canvas coords) ---- */
#define TD6_PALETTE_N    16     /* keep in sync with s_td6_palette[] */
#define TD6_CP_LIST_X    46
#define TD6_CP_LIST_Y    226
#define TD6_CP_SW        19     /* predefined swatch size */
#define TD6_CP_GAP        2
#define TD6_CP_COLS       8
#define TD6_CP_MAP_X     46
#define TD6_CP_MAP_Y     272
#define TD6_CP_MAP_W    168
#define TD6_CP_MAP_H     46
#define TD6_CP_MAP_ROWS   6     /* keyboard grid rows over the color map */
#define TD6_CP_GRID_ROWS (2 + TD6_CP_MAP_ROWS)  /* 2 swatch rows + map rows */

extern char s_lobby_password[32];
extern char s_mp_player_name[TD5_MAX_HUMAN_PLAYERS][16];
extern const uint32_t k_mp_player_colors[TD5_MAX_HUMAN_PLAYERS];
extern int      s_fe_logic_ticks;
extern int      s_mp_car_player;
extern int      s_mp_flow;
extern int      s_mp_join_device[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_joined_count;
extern int      s_mp_player_car[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_color[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_paint[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_ready[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_simul;
extern int  s_anim_complete;
extern int  s_anim_tick;
extern int  s_arrow_input;
extern int  s_button_index;
extern int  s_flow_context;
extern int  s_inner_state;
extern int  s_input_ready;
extern int  s_kicked_flag;
extern int  s_launching_net_race;
extern int  s_lobby_action;
extern int  s_lobby_max_players;
extern int  s_lobby_modal;
extern int  s_mp_phase;
extern int  s_mp_player_accent[TD5_MAX_HUMAN_PLAYERS];
extern int  s_net_cfg_game_port;
extern int  s_net_session_sel;
extern int  s_network_active;
extern int  s_nickname_from_mpopts;
extern int  s_num_human_players;
extern int  s_return_screen;
extern int  s_selected_car;
extern int  s_text_input_state;
extern int  s_two_player_mode;
extern int s_selected_button;
extern uint32_t s_mp_pane_nav_prev[TD5_MAX_HUMAN_PLAYERS];
extern uint32_t s_mp_simul_ready_ms;
float frontend_update_timed_animation(int max_tick, uint32_t duration_ms);
int frontend_check_escape(void);
int frontend_create_button(const char *label, int x, int y, int w, int h);
int frontend_load_tga(const char *name, const char *archive);
int frontend_option_delta(void);
int frontend_text_input_confirmed(void);
void Screen_ConnectionBrowser(void);
void Screen_CreateSession(void);
void Screen_DirectConnect(void);
void Screen_LanMenu(void);
void Screen_MultiplayerLobby(void);
void Screen_NetNickname(void);
void Screen_NetworkLobby(void);
void Screen_SessionLocked(void);
void Screen_SessionPicker(void);
void frontend_begin_text_input(char *buffer, int capacity);
void frontend_begin_timed_animation(void);
void frontend_handle_text_input_key(void);
void frontend_init_display_mode_state(void);
void frontend_init_race_schedule(void);
void frontend_init_return_screen(TD5_ScreenIndex screen);
void frontend_net_destroy(void);
void frontend_play_sfx(int id);
void frontend_present_buffer(void);
void frontend_render_session_locked_overlay(float sx, float sy);
void frontend_reset_buttons(void);
void td5_frontend_set_screen(TD5_ScreenIndex index);
#define FE_MAX_BUTTONS 16

typedef struct {
    int active;
    int x, y, w, h;
    int disabled;
    int hidden;             /* 1 = not drawn at all (mirrors original moving the
                             * sprite off-screen, e.g. the Direction toggle on
                             * forward-only/circuit tracks). Pair with disabled
                             * so nav/mouse also skip it. */
    int highlight_ramp;     /* 0-6: smooth highlight fade (original uses 6-step ramp) */
    int is_selector;        /* 1 = left/right selector widget; always uses blue 9-slice */
    char label[64];
} FE_Button;

extern FE_Button s_buttons[FE_MAX_BUTTONS];
extern char s_text_input_prompt[40];

const char *frontend_get_track_name(int track_index);
extern FE_Surface s_surfaces[FE_MAX_SURFACES];
extern const char *const k_mp_kbd_rows[];
extern const char *s_car_zip_paths[TD5_CAR_COUNT];
extern const int s_cup_schedules[][13];
extern const uint32_t s_td6_palette[TD6_PALETTE_N];
extern const uint8_t s_track_schedule_to_tga_index[37];
extern int             s_game_option_cops;
extern int             s_game_option_laps;
extern int             s_game_option_traffic;
extern int      s_mp_pane_btn[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_pane_overlay[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_pane_preview[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_pane_spec_car[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_pane_substate[TD5_MAX_HUMAN_PLAYERS];
extern int      s_mp_player_trans[TD5_MAX_HUMAN_PLAYERS];
extern int     s_score_insert_pos;
extern int  s_car_preview_next_surface;
extern int  s_car_preview_prev_surface;
extern int  s_car_spec_car;
extern int  s_cheat_unlock_all;
extern int  s_drag_carselect_pass;
extern int  s_masters_roster[15];
extern int  s_masters_roster_flags[15];
extern int  s_mouse_clicked;
extern int  s_mp_col_col[TD5_MAX_HUMAN_PLAYERS];
extern int  s_mp_col_row[TD5_MAX_HUMAN_PLAYERS];
extern int  s_mp_kbd_col[TD5_MAX_HUMAN_PLAYERS];
extern int  s_mp_kbd_row[TD5_MAX_HUMAN_PLAYERS];
extern int  s_mp_setup_btn[TD5_MAX_HUMAN_PLAYERS];
extern int  s_mp_setup_sub[TD5_MAX_HUMAN_PLAYERS];
extern int  s_num_ai_opponents;
extern int  s_num_spectate_screens;
extern int  s_p2_car;
extern int  s_race_within_series;
extern int  s_results_cup_complete;
extern int  s_results_panel_slide_x;
extern int  s_results_rerace_flag;
extern int  s_results_skip_display;
extern int  s_score_category_index;
extern int  s_selected_config;
extern int  s_selected_game_type;
extern int  s_selected_paint;
extern int  s_selected_track;
extern int  s_selected_transmission;
extern int  s_total_unlocked_cars;
extern int  s_total_unlocked_tracks;
extern int  s_track_direction;
extern int s_button_count;
extern int s_car_preview_surface;
extern int s_carsel_bar_surface;
extern int s_carsel_curve_surface;
extern int s_carsel_fill_surface;
extern int s_carsel_topbar_surface;
extern int s_color_cur_col;
extern int s_color_cur_row;
extern int s_color_panel_visible;
extern int s_cup_won_car_count;
extern int s_cup_won_track_count;
extern int s_graphbars_surface;
extern int s_paint_active;
extern int s_paint_overlay_car;
extern int s_paint_overlay_surface;
extern int s_track_preview_surface;
extern int s_track_switch_tick;
extern uint32_t s_fe_gamepad_nav;
extern uint32_t s_mp_simul_anim_ms;
extern uint8_t s_car_lock_table[TD5_CAR_COUNT];
extern uint8_t s_track_lock_table[37];
int ConfigureGameTypeFlags(void);
int frontend_advance_tick(void);
int frontend_car_has_paint(int car_index);
int frontend_car_is_cop(int i);
int frontend_car_is_td6(int car_index);
int frontend_car_paintable(int car_index);
int frontend_current_car_index(void);
int frontend_find_surface_by_source(const char *name, const char *archive);
int frontend_load_car_paint_overlay_surface(int car_index);
uint32_t mp_setup_grid_color(int col, int row);
uint32_t td6_map_color(float u, float v);
void Screen_CarSelection(void);
void Screen_CupFailed(void);
void Screen_CupWon(void);
void Screen_PostRaceHighScore(void);
void Screen_PostRaceNameEntry(void);
void Screen_QuickRaceMenu(void);
void Screen_RaceResults(void);
void Screen_TrackSelection(void);
void frontend_load_car_spec_fields(int car_index);
void frontend_release_surface(int handle);
void frontend_set_cursor_visible(int visible);
void mp_simul_load_pane_spec(int p, int car);
extern int  s_mouse_x, s_mouse_y;
/* ---- frontend canvas (640x480 design space) ---- */
#define FE_CANVAS_W 640
#define FE_CANVAS_H 480
#define FE_CENTER_X (FE_CANVAS_W / 2)  /* 320 */
#define FE_CENTER_Y (FE_CANVAS_H / 2)  /* 240 */

/* ---- car selection / simultaneous-MP grid ---- */
#define FE_CARSEL_SLIDE_IN_MS 1250
#define TD6_COP_LAST  49
enum { MP_BTN_CAR = 0, MP_BTN_PAINT, MP_BTN_STATS, MP_BTN_TRANS, MP_BTN_OK, MP_BTN_COUNT };
enum { MP_SET_NAME = 0, MP_SET_COLOUR, MP_SET_OK, MP_SET_COUNT };
#define MP_SIMUL_ANIM_MS 480u   /* lobby -> car-select pane slide-in duration */
#define MP_KBD_LETTER_ROWS 4
#define MP_KBD_ROWS        (MP_KBD_LETTER_ROWS + 1)   /* + special row */
#define MP_COL_COLS 16
#define MP_COL_ROWS 16

extern int  s_snap_car, s_snap_paint, s_snap_trans, s_snap_config;
/* ---- Quick Race screen layout ---- */
/* Drag Strip = schedule index 19 (s_track_schedule_to_name_index[19]==0 ->
 * "DRAG STRIP"). Excluded from the Quick Race track cycler. */
#define FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX 19

/* Quick Race screen button indices (improved layout). */
#define QR_BTN_CAR        0
#define QR_BTN_TRACK      1
#define QR_BTN_DIRECTION  2
#define QR_BTN_PLAYERS    3
#define QR_BTN_OPPONENTS  4
#define QR_BTN_LAPS       5   /* [S02 (c)] circuit laps, re-homed from Game Options */
#define QR_BTN_SPLITSCREENS 6 /* [2026-06-08] AI spectator split-screens (dev-only) */
#define QR_BTN_OK         7
#define QR_BTN_BACK       8

/* Quick Race layout: caption buttons in a left column, the selected value in a
 * right column, all rows uniformly spaced.
 * NB frontend_create_button treats w==200 as an "unset" sentinel (substitutes
 * 224), so QR_BTN_W must NOT be 200. */
/* [S02 follow-up 2026-06-04] Whole QR row group shifted right +56 (64->120) so the
 * caption buttons clear the MainMenu background's black left bar (~0..110px); this
 * matches the other option screens' x=120 left edge. FE_QR_VALUE_X shifts by the
 * same +56 to keep the button->value gap (button right edge = 120+208 = 328). */
#define QR_COL_X       120    /* button left edge (clears the black left bar)     */
#define QR_BTN_W       208    /* button width (right edge = 328)          */
#define QR_ROW_Y0       96    /* first row y                              */
#define QR_ROW_DY       56    /* uniform vertical gap between rows         */
#define QR_ROW_Y(n)     (QR_ROW_Y0 + (n) * QR_ROW_DY)
#define FE_QR_VALUE_X  336    /* value column left edge (clear of button @328)  */
#define FE_QR_VALUE_SCALE 0.9f /* value glyph scale — matches the button-caption size */
#define FE_QR_SCREEN_W   640  /* canvas width                                    */
#define FE_QR_RIGHT_MARGIN 12 /* keep value text this far from the right edge     */
#define FE_QR_VALUE_LINE_H 22 /* canvas px per value line (centers + wrap spacing) */

/* @GENERATED-SYMBOLS@ */

#endif /* TD5_FRONTEND_INTERNAL_H */
