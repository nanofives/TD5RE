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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "td5_types.h"
#include "td5_credits.h"                 /* K_CREDIT_MUGSHOT_COUNT */
#include "../../ddraw_wrapper/src/wrapper.h"   /* ID3D11* handles, g_backend */

typedef struct { int width; int height; } BgGalImg;
typedef struct { const char *label; int cols; int rows; } MpSplitLayout;

/* ---- multiplayer SESSION-PERSISTENT store (RAM, lives for the whole process
 * run) ----
 * Survives every menu/lobby transition so re-entering the MP menu in the same
 * session keeps each player's name / accent colour / car (+paint/color/trans).
 * Owned by td5_frontend.c (single static s_mp_session, zero-init); other
 * frontend TUs save/restore one player at a time via the helpers below. Gated by
 * env TD5RE_MP_SESSION (default ON; "0" => the old wipe-every-time behaviour, in
 * which case `valid` never goes true and behaviour is byte-identical to before).
 * NB the position-select screen is a planned follow-up: the cell[]/pos_* fields
 * are RESERVED for that agent and stay 0 until it wires them. */
typedef struct {
    int  valid;                              /* 1 once a race has snapshotted a roster */
    int  count;                              /* number of human players captured */
    char name[TD5_MAX_HUMAN_PLAYERS][16];
    int  accent[TD5_MAX_HUMAN_PLAYERS];      /* 0x00RRGGBB identity colour */
    int  car[TD5_MAX_HUMAN_PLAYERS];
    int  paint[TD5_MAX_HUMAN_PLAYERS];
    int  color[TD5_MAX_HUMAN_PLAYERS];       /* TD6 body colour (0xRRGGBB, -1 = none) */
    int  trans[TD5_MAX_HUMAN_PLAYERS];       /* 0 = Automatic, 1 = Manual */
    int  device[TD5_MAX_HUMAN_PLAYERS];      /* [per-device 2026-06-21] enumerated input
                                              * device (s_mp_join_device) that occupied this
                                              * slot, so a profile is restored to whatever
                                              * pad it was set with, regardless of join order */
    /* --- reserved for the position-select screen agent (leave at 0) --- */
    int  cell[TD5_MAX_HUMAN_PLAYERS];        /* chosen viewport/grid cell per player */
    int  pos_assigned;                       /* 1 once positions have been chosen */
    int  pos_count_committed;                /* player count at position-commit time */
} MpSession;

int  mp_session_enabled(void);               /* TD5RE_MP_SESSION knob (cached) */
int  mp_session_is_valid(void);              /* s_mp_session.valid && enabled */
int  mp_session_count(void);                 /* captured human count (0 if invalid) */
void mp_session_save_player(int p);          /* live arrays[p] -> store[p] (records device) */
void mp_session_save_live_roster(int humans);/* [persist-on-edit] mirror live roster -> store
                                              * + mark valid, so profiles survive backing out
                                              * to the main menu before a race ever commits */
void mp_session_restore_player(int p);       /* store[p] -> live arrays[p] (if valid && p<count) */
int  mp_session_by_device(void);             /* TD5RE_MP_SESSION_BY_DEVICE knob (cached) */
int  mp_session_restore_player_for_device(int p); /* store slot matching s_mp_join_device[p] ->
                                              * live arrays[p]; 1 if a profile was restored */

/* ---- MP split-screen POSITION SELECT (#6/#8) ----
 * s_mp_player_cell[p] = the grid cell human player p occupies (default identity).
 * Owned by td5_frontend.c; written by the picker (Screen_MpPosition, in
 * td5_fe_race.c), persisted into / restored from s_mp_session by the helpers.
 * Gated by env TD5RE_MP_POSITIONS (default ON; "0" => skip picker + identity). */
extern int  s_mp_player_cell[TD5_MAX_HUMAN_PLAYERS];
int  mp_positions_enabled(void);             /* TD5RE_MP_POSITIONS knob (cached) */
void mp_positions_reset_identity(void);      /* live cells -> identity (cell[p]=p) */
void mp_session_commit_positions(int humans);/* live cells -> store; mark assigned */
int  mp_session_restore_positions(int humans);/* store -> live cells; 1 if usable (skip picker) */
void Screen_MpPosition(void);                /* the picker screen handler */
void frontend_mp_position_render(float sx, float sy);  /* its render (in td5_frontend.c) */

/* ---- MP GAME MODES (2026-06-22) ----
 * New MP setup pipeline screens, handlers in td5_fe_net.c. Inserted after the
 * viewport picker (Screen_MpPosition) and before car selection. */
void Screen_MpModeVote(void);    /* host votes/locks the game mode; others vote   */
void Screen_MpModeConfig(void);  /* per-mode options (host edits, clients view)   */
void Screen_CupWinners(void);    /* final cup standings / podium                  */
void Screen_MpCopRoles(void);    /* cop chase: each player picks cop / suspect    */
void Screen_MpTeamSelect(void);  /* cup teams: each player picks their team       */

/* ---- CHANGELOG (2026-06-25) ----
 * Scrollable version + changelog screen reached from the main menu (button beside
 * EXIT). Handler + render fn both live in td5_frontend.c. */
void Screen_Changelog(void);

/* ---- PENDING TO TEST (2026-06-25) ----
 * Dev/QA checklist screen reached from a button at the top of the main menu.
 * Handler + render fn live in td5_frontend.c; the list/state/overlay live in
 * td5_pending.c. */
void Screen_PendingTest(void);

/* ---- shared texture-page allocations ---- */
#define SHARED_PAGE_WHITE     899
#define SHARED_PAGE_FONT_MSDF 970   /* free page (frontend uses 888-955) */
#define SHARED_PAGE_SMALLFONT_MSDF 971  /* free page (BodyText MSDF is 970) */

/* ---- VectorUI roundrect constant buffer (matches ps_roundrect.hlsl) ---- */
typedef struct {
    float size_px[2];   /* button w,h in screen px */
    float border[2];    /* border thickness px: x = left/right, y = top/bottom */
    float radii[4];     /* outer corner radii px: TL, TR, BL, BR */
    float mid[4];       /* border gradient: lightest (middle of band) */
    float inner[4];     /* border gradient: inner-edge colour */
    float outer[4];     /* border gradient: outer-edge colour (darkest) */
    float fill[4];      /* interior rgb, a = interior alpha (0 = transparent) */
} FE_RoundRectParams;   /* 96 bytes, matches cbuffer RoundRectParams */

/* ---- misc shared constants ---- */
#define MP_MISSING_CONTENT_COUNT 3   /* keep in sync with k_mp_missing_content */
extern const char *const k_mp_missing_content[MP_MISSING_CONTENT_COUNT];
#define GALLERY_ZIP         "Front End/Extras/Mugshots.zip"
#define FE_CREDITS_ROW_H    32.0f   /* 0x20 per text row */
#define FE_CREDITS_PHOTO_H  224.0f  /* mugshot height (= 7 rows) */
#define FE_CREDITS_SPEED    0.060f  /* px/ms = 60px/s = orig 1px/frame @60fps */
extern float s_bg_gal_x, s_bg_gal_y;

/* ---- shared texture-page allocations (see td5_frontend.c page map) ---- */
#define SHARED_PAGE_MIN       888  /* lowest shared page -- don't clear below this */
#define SHARED_PAGE_BG_GALLERY 888 /* 5 pages 888-892: background slideshow pic1-5.tga */
#define SMALLFONT_PAGE   893
/* page 894 — FREE (was SHARED_PAGE_ARROWBTNZ; ArrowButtonz.tga retired 2026-06-16,
 * selector arrows are procedural via ps_arrow) */
#define SHARED_PAGE_BTNLIGHTS 895
#define SHARED_PAGE_CURSOR    896
#define SHARED_PAGE_BUTTONBITS 897
#define SHARED_PAGE_FONT      898
#define SHARED_PAGE_CURSOR_MSDF 981  /* free page (titles use 972..980) */
#define SHARED_PAGE_HUDFONT_SDF 982  /* free page (cursor MSDF is 981) */
#define SHARED_PAGE_PAUSEFONT_SDF 983

/* ---- BodyText.tga font atlas (10 cols x 24px cells, 240x552) ---- */
#define FONT_COLS 10
#define FONT_CELL 24
#define FONT_TEX_W (FONT_COLS * FONT_CELL)  /* 240 */
#define FONT_TEX_H 552  /* actual BodyText.tga height: 23 rows of 24px */

/* ---- VectorUI gauge constant buffer (matches ps_gauge.hlsl) ---- */
typedef struct {
    float quad_px[2];   /* quad size px (uv -> local px) */
    float center[2];    /* dial center in local px */
    float radius;       /* outer disc radius px */
    float inner_r;      /* inner 3D circle radius px (0 => none) */
    float sweep_start;  /* first tick angle (radians, screen CW) */
    float sweep_end;    /* last tick angle (radians) */
    float tick_count;   /* ticks along the sweep (>=2) */
    float major_every;  /* every Nth tick is major */
    float major_len;    /* major tick length px */
    float minor_len;    /* minor tick length px */
    float tick_out;     /* tick outer radius px */
    float red_start;    /* red zone start (radians); ticks >= this are red */
    float red_end;      /* red zone end (radians); <= start => none */
    float pivot_px;     /* pivot dot radius px (0 => none) */
    float rim_red_px;   /* red rim arc thickness px */
    float pad0, pad1, pad2;
    float face[4];      /* outer disc rgba (semi-transparent) */
    float inner[4];     /* inner 3D disc rgba */
    float tick[4];      /* white tick rgba */
    float red[4];       /* red teeth + red rim rgba */
    float pivot[4];     /* pivot hub rgba (a<=0 => none) */
} FE_GaugeParams;       /* 160 bytes, matches cbuffer GaugeParams */

/* ---- frontend draw queue ---- */
#define FE_MAX_DRAW_CMDS 128

typedef enum { FE_CMD_RECT, FE_CMD_BLIT } FE_CmdType;

typedef struct {
    FE_CmdType type;
    int x, y, w, h;
    uint32_t color;
    int tex_page;  /* for blit */
    float u0, v0, u1, v1; /* UV for blit */
} FE_DrawCmd;

/* ---- car roster bounds (carmodel.nfo order) ---- */
#define TD5_BASE_CAR_COUNT 37
#define TD5_CAR_COUNT      76

/* Drop-in custom cars (re/assets/cars/custom_<name>/) occupy slots TD5_CAR_COUNT..
 * TD5_CAR_SLOT_MAX-1. TD5_CAR_SLOT_MAX is the ARRAY DIMENSION for the per-car
 * frontend caches; TD5_CAR_COUNT stays the built-in logical count, and
 * td5_car_total_count() (= 76 + discovered custom) is the live roster bound. */
#include "td5_customcar.h"               /* TD5_CUSTOM_CAR_MAX + registry API */
#define TD5_CAR_SLOT_MAX   (TD5_CAR_COUNT + TD5_CUSTOM_CAR_MAX)

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
extern int  s_mp_phase;
extern int  s_mp_player_accent[TD5_MAX_HUMAN_PLAYERS];
/* [MP AI TEST PLAYERS 2026-06-25] Dev "add AI player" lobby tool. */
extern int  s_mp_slot_is_ai[TD5_MAX_HUMAN_PLAYERS];   /* 1 = simulated AI player */
extern int  s_mp_ai_cop_count;                        /* cop chase: # AI players that are cops */
int  frontend_mp_slot_is_ai(int p);
int  frontend_mp_ai_player_count(void);
int  frontend_mp_human_count(void);
int  frontend_mp_ai_cop_count(void);
const char *frontend_mp_ai_pool_name(int idx);   /* [CUP TEAM SELECT] display name for a cup AI opponent */
int  frontend_mp_add_ai_player(void);
int  frontend_mp_remove_ai_player(void);
void frontend_mp_ai_cop_count_step(int d);
void frontend_mp_ai_players_reset(void);
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
/* [splitscreen back-confirm 2026-06-24] Universal "confirm before going back"
 * guard for LOCAL split-screen multiplayer. Any screen's back/cancel routes its
 * back action through frontend_back_confirm_request(action): in split-screen it
 * raises a dimmed "GO BACK?" yes/no prompt (the whole frontend is frozen while it
 * is up so nothing beneath can act on the pad) and returns 1 (caller defers); the
 * stored action runs on confirm. Outside split-screen / when disabled it returns
 * 0 and the caller performs the back immediately, exactly as before. Knob
 * TD5RE_MP_BACK_CONFIRM=0 restores the legacy instant back. Defined in
 * td5_frontend.c; the prompt reuses mp_confirm_modal_render (td5_fe_race.c). */
int  frontend_back_confirm_request(void (*action)(void));
int  frontend_back_confirm_active(void);
void frontend_back_confirm_tick(void);
void mp_confirm_modal_render(float sx, float sy, const char *prompt);
int frontend_create_button(const char *label, int x, int y, int w, int h);
int frontend_load_tga(const char *name, const char *archive);
int frontend_option_delta(void);
int frontend_input_confirm_was_mouse(void);
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
#ifndef TD5RE_RELEASE
/* [2026-06-15 TASK A1] Dev-only: toggle the Quick Race span-offset field's
 * "input active" state (click-to-type). Owned by td5_frontend.c (which holds the
 * editable buffer + render); called from the QR_BTN_SPAN activation branch in the
 * Screen_QuickRaceMenu FSM (td5_fe_race.c). Commits the buffer to
 * g_td5.ini.start_span_offset on each toggle and plays the confirm cue. */
void frontend_qr_span_toggle_active(void);
#endif
void frontend_present_buffer(void);
void frontend_render_session_locked_overlay(float sx, float sy);
void frontend_reset_buttons(void);
void td5_frontend_set_screen(TD5_ScreenIndex index);
void td5_frontend_show_net_disconnect(const char *reason);
extern int g_net_disconnect_mode;          /* [2026-06-19] 1 = SessionLocked dialog shows the net-disconnect notice */
extern char g_net_disconnect_reason[64];
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
extern const char *s_car_zip_paths[TD5_CAR_SLOT_MAX];
extern const int s_cup_schedules[][13];
/* [CUP TRACK SELECT 2026-06-25] player-chosen per-race cup tracks (port enhancement) */
extern int s_cup_user_tracks[TD5_CUP_MAX_RACES + 1];
extern int s_cup_user_dirs[TD5_CUP_MAX_RACES + 1];
extern int s_cup_user_traffic[TD5_CUP_MAX_RACES + 1];
extern int s_cup_user_cops[TD5_CUP_MAX_RACES + 1];
extern int s_cup_user_laps[TD5_CUP_MAX_RACES + 1];
extern int s_cup_user_count;
extern int s_cup_pick_index;
extern int s_cup_user_active;
extern int s_cup_pick_is_mp;
int frontend_cup_race_count(int game_type);
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
extern uint8_t s_car_lock_table[TD5_CAR_SLOT_MAX];
extern uint8_t s_track_lock_table[];  /* sized in td5_frontend.c: 37 native/cup/TD6 + custom headroom */
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
void Screen_MpPostRace(void);    /* [2026-06-25] MP split-screen post-race menu   */
void Screen_TrackSelection(void);
void frontend_load_car_spec_fields(int car_index);
void frontend_release_surface(int handle);
void frontend_set_cursor_visible(int visible);
void mp_simul_load_pane_spec(int p, int car);
extern int  s_mouse_x, s_mouse_y;
extern int  s_cursor_h;
extern int  s_buttonbits_h;
extern int  s_buttonlights_h;
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
/* [2026-06-12] dev-only toggles placed to the RIGHT of the Opponents row:
 * PlayerIsAI (slot 0 driven by AI) and AutoThrottle (trace auto-throttle). */
#define QR_BTN_PLAYERAI   9
#define QR_BTN_AUTOTHR    10
/* [2026-06-15] dev-only click-to-type span-offset field, its own row below the
 * "AI Screens" row (QR_BTN_SPLITSCREENS). Created LAST so the OK/Back/PlayerAI/
 * AutoThr indices above are unchanged; hidden+disabled in release. */
#define QR_BTN_SPAN       11

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
/* [2026-06-15 TASK A2] Row pitch tightened 56 -> 44 so the added dev "Span
 * Offset" row (8 rows total, 0..7) fits the 480px canvas with OK/Back at row 7
 * (y = 96 + 7*44 = 404, 32px tall -> bottom 436). 32px buttons leave a 12px gap
 * between rows; the title rests at ~17 so the first row at 96 stays clear. */
#define QR_ROW_DY       44    /* uniform vertical gap between rows         */
#define QR_ROW_Y(n)     (QR_ROW_Y0 + (n) * QR_ROW_DY)
#define FE_QR_VALUE_X  336    /* value column left edge (clear of button @328)  */
#define FE_QR_VALUE_SCALE 0.9f /* value glyph scale — matches the button-caption size */
#define FE_QR_SCREEN_W   640  /* canvas width                                    */
#define FE_QR_RIGHT_MARGIN 12 /* keep value text this far from the right edge     */
#define FE_QR_VALUE_LINE_H 22 /* canvas px per value line (centers + wrap spacing) */

const MpSplitLayout *mp_split_layouts(int n, int *count);
extern BgGalImg s_bg_gallery[5];
extern FE_DrawCmd s_draw_queue[FE_MAX_DRAW_CMDS];
extern ID3D11Buffer      *s_gauge_cb;
extern ID3D11Buffer      *s_rr_cb;
extern ID3D11PixelShader *s_ps_arrow;
extern ID3D11PixelShader *s_ps_cursor;
extern ID3D11PixelShader *s_ps_gauge;
extern ID3D11PixelShader *s_ps_msdf;
extern ID3D11PixelShader *s_ps_roundrect;
extern char s_music_test_now_band[64];
extern char s_music_test_now_title[64];
extern char s_music_test_track_label[64];
extern const char * const k_ctrl_action_labels[TD5_JSBIND_ACTIONS];
extern int             s_display_camera_damping;
extern int             s_display_fog_enabled;
extern int             s_display_mode_count;
extern int             s_display_mode_index;
extern int             s_display_show_fps;
extern int             s_display_speed_units;
extern int             s_display_vsync;
extern int             s_display_window_mode;
extern int             s_game_option_checkpoint_timers;
extern int             s_game_option_collisions;
extern int             s_game_option_difficulty;
extern int             s_game_option_dynamics;
extern int             s_race_difficulty;   /* per-race AI difficulty row on Track Selection (0..2) */
extern int             s_sound_option_music_volume;
extern int             s_sound_option_sfx_mode;
extern int             s_sound_option_sfx_volume;
extern int      s_ctrl_capture_armed;
extern int      s_ctrl_capturing;
extern int      s_ctrl_input_source;
extern int      s_ctrl_opts_player;
extern int      s_ctrl_player;
extern int      s_ctrl_sel_action;
extern int      s_fade_active;
extern int      s_fade_direction;
extern int      s_fade_progress;
extern int   s_bg_gal_blend;
extern int   s_bg_gal_current;
extern int   s_bg_gal_loaded;
extern int  s_attract_demo_active;
extern int  s_attract_mode_ctrl;
extern int  s_attract_track;
extern int  s_band_cover_surface[5];
extern int  s_control_options_surface;
extern int  s_credit_mugshot_surf[K_CREDIT_MUGSHOT_COUNT];
extern int  s_cup_unlock_tier;
extern int  s_language_bg_surface;
extern int  s_language_flag_surface;
extern int  s_mp_btn_catchup;
extern int  s_mp_btn_layout;
extern int  s_mp_btn_missing[2];
extern int  s_mp_btn_nickname;
extern int  s_mp_btn_players;
extern int  s_mp_layout_optcount;
extern int  s_mp_layout_sel;
extern int  s_mp_missing_content[2];
extern int  s_mp_missing_count;
extern int  s_music_attract_track;
extern int  s_music_test_playing_set;
extern int  s_sound_volumebox_surface;
extern int  s_sound_volumefill_surface;
extern int s_buttonbits_tex_page;
extern int s_buttonbits_w;
extern int s_buttonlights_tex_page;
extern int s_buttonlights_w;
extern int s_cursor_msdf_page;
extern int s_cursor_tex_page;
extern int s_cursor_w;
extern int s_draw_queue_count;
extern int s_font_page;
extern int s_hudfont_sdf_page;
extern int s_msdf_font_page;
extern int s_pausefont_sdf_page;
extern int s_smallfont_msdf_page;
extern int s_smallfont_page;
extern int s_white_tex_page;
extern uint32_t s_anim_start_ms;
extern uint32_t s_attract_idle_timestamp;
extern uint32_t s_credits_start_ms;
extern uint32_t s_ctrl_action_bind[TD5_MAX_HUMAN_PLAYERS][TD5_JSBIND_ACTIONS];
extern uint8_t  s_ctrl_kb_scancodes[16];
int frontend_load_tga_ck(const char *name, const char *archive, TD5_ColorKeyMode colorkey);
int frontend_load_tga_colorkey(const char *name, const char *archive,
                                       int dest_page, int *out_w, int *out_h,
                                       TD5_ColorKeyMode colorkey);
void Screen_AttractModeDemo(void);
void Screen_ControlOptions(void);
void Screen_ControllerBinding(void);
void Screen_DisplayOptions(void);
void Screen_ExtrasGallery(void);
void Screen_GameOptions(void);
void Screen_LanguageSelect(void);
void Screen_LegalCopyright(void);
void Screen_LocalizationInit(void);
void Screen_MainMenu(void);
void Screen_MusicTestExtras(void);
void Screen_OptionsHub(void);
void Screen_PositionerDebugTool(void);
void Screen_RaceTypeCategory(void);
void Screen_SoundOptions(void);
void Screen_StartupInit(void);
void Screen_TwoPlayerOptions(void);
void frontend_init_font_metrics_default(void);
void frontend_init_font_metrics_from_pixels(const uint8_t *pixels, int w, int h);
void frontend_post_quit(void);
void mp_resolve_layout(int n, int sel, int *cols, int *rows, int *missing);
/* ---- network lobby roster layout (design px) ----
 * Single source for the overlay renderer (td5_frontend.c) and the per-row
 * kick-button placement (td5_fe_net.c). FE_LOBBY_X matches FE_TITLE_LEFT_X
 * so the panel left-aligns with the NET PLAY screen title. */
#define FE_LOBBY_X        116
#define FE_LOBBY_PANEL_Y   96
#define FE_LOBBY_PANEL_W  290
#define FE_LOBBY_PANEL_H  220
#define FE_LOBBY_ROW0_Y   140
#define FE_LOBBY_ROW_H     24

/* ---- Left-column menu button geometry (design px) — THE alignment routine ----
 * Canonical "buttons sit under the screen title" layout, used by every standard
 * left-column menu. Screen_RaceTypeCategory (the race menu, td5_fe_menu.c) is the
 * reference implementation.
 *   RE basis: RaceTypeCategoryMenuStateMachine @0x004168B0 creates each option
 *   button via CreateFrontendDisplayModeButton with width 0xE0 (224) [CONFIRMED];
 *   the resting on-screen X is g_frontendCanvasW/2 - 200 = 120 at the 640px canvas
 *   [CONFIRMED]. The shared title-label creator (CreateMenuStringLabelSurface
 *   @0x00412E30) blits the screen title at the SAME g_frontendCanvasW/2 - 200 = 120,
 *   so in the original the title and the menu column align exactly at 120.
 * The button FRAME's left edge sits at FE_MENU_BTN_X; the LABEL is CENTRED inside
 * the frame (see the button loop in td5_frontend.c,
 * `fe_draw_text(bx + (bw - tw)*0.5f, ...)`), NOT left-aligned.
 * The port draws its title at FE_TITLE_LEFT_X = 126 (a port layout value eyeballed
 * from MainMenu.png — no binary literal; a 126 constant scan over the frontend
 * region returned zero hits), so menu buttons at FE_MENU_BTN_X (120) match the
 * original's button geometry and sit ~6 px left of the port title — that IS the
 * "like every other screen" position. Width 0xE0 (224) keeps the column clear of a
 * right-side description panel (panels start at x=348). New left-column menus MUST
 * use these constants instead of re-deriving an ad-hoc X.
 * See FRONTEND_SCREEN_GUIDE.md §Button column alignment. */
#define FE_MENU_BTN_X   120     /* button frame left edge (= orig canvasW/2-200; aligns under the title) */
#define FE_MENU_BTN_W   0xE0    /* 224: standard left-column button width (orig 0x004168B0 [CONFIRMED]) */
#define FE_MENU_BTN_H   0x20    /* 32:  standard button height                                          */

extern char s_create_session_name[64];
extern char s_cs_port_buf[8];     /* GAME PORT inline field buffer (direct host) */
extern int  s_cs_edit;
extern uint8_t s_slot_ready[6];   /* [S31] per-slot lobby READY latch */
void frontend_reset_text_input(void);

/* @GENERATED-SYMBOLS@ */

#endif /* TD5_FRONTEND_INTERNAL_H */
