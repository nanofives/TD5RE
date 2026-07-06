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
#include <stddef.h>   /* size_t (td5_gameopts_value) */

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
    int  laneassist[TD5_MAX_HUMAN_PLAYERS];  /* 0 = off, 1 = lane assist on */
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
/* [SECONDARY PAINT 2026-06-29] The panel now also picks a SECONDARY colour and a
 * PATTERN that blends the two. Two compact selector rows sit ABOVE the swatch
 * grid (PATTERN, then COLOR target = MAIN/2ND); the swatch grid was widened to
 * 32 quick-pick presets in 2 rows (smaller swatches) so the panel still fits the
 * left button column. The HSV map below is unchanged (any colour). */
#define TD6_PALETTE_N    32     /* keep in sync with s_td6_palette[] */
#define TD6_CP_LIST_X    46
#define TD6_CP_PATTERN_Y 220    /* row 0: PATTERN ◄ name ► */
#define TD6_CP_TARGET_Y  233    /* row 1: COLOR   ◄ MAIN/2ND ► */
#define TD6_CP_LIST_Y    248    /* first swatch row */
#define TD6_CP_SW         9      /* predefined swatch size */
#define TD6_CP_GAP        1
#define TD6_CP_COLS      16
#define TD6_CP_MAP_X     46
#define TD6_CP_MAP_Y     272
#define TD6_CP_MAP_W    168
#define TD6_CP_MAP_H     44
#define TD6_CP_MAP_ROWS   6     /* keyboard grid rows over the color map */
/* Unified cursor row model: 0 = PATTERN selector, 1 = COLOR-target selector,
 * 2.. = swatch grid rows, then the HSV-map rows. */
#define TD6_ROW_PATTERN   0
#define TD6_ROW_TARGET    1
#define TD6_SWATCH_ROW0   2
#define TD6_SWATCH_ROWS  ((TD6_PALETTE_N + TD6_CP_COLS - 1) / TD6_CP_COLS) /* =2 */
#define TD6_MAP_ROW0     (TD6_SWATCH_ROW0 + TD6_SWATCH_ROWS)               /* =4 */
#define TD6_CP_GRID_ROWS (TD6_MAP_ROW0 + TD6_CP_MAP_ROWS)

/* Paint patterns: how the primary + secondary colours combine on the body. */
enum {
    TD6_PAT_SOLID = 0,   /* whole body = primary */
    TD6_PAT_TWOTONE,     /* upper = primary / lower = secondary */
    TD6_PAT_STRIPES,     /* centre band = secondary */
    TD6_PAT_SPLIT,       /* front-ish = primary / rear-ish = secondary */
    TD6_PAT_COUNT
};
/* Pattern split thresholds (normalised body-bbox / overlay coords). Shared in
 * spirit by the in-race texel bake (td5_asset.c) and the menu preview region
 * draw (td5_frontend.c) so both read as the same livery. */
#define TD6_PAT_TWOTONE_V   0.50f
#define TD6_PAT_STRIPE_LO   0.42f
#define TD6_PAT_STRIPE_HI   0.58f
#define TD6_PAT_SPLIT_U     0.50f

extern int s_paint_target;          /* 0 = editing MAIN colour, 1 = SECONDARY */
const char *td6_pattern_name(int pat);

extern char s_lobby_password[32];
extern char s_mp_player_name[TD5_MAX_HUMAN_PLAYERS][16];
/* [PLAYER NAME 2026-07-02] Human display name for results/standings rows: MP
 * profile name if loaded, else (slot 0) the Game Options PLAYER NAME, else
 * NULL (caller falls back to "P<n>" / "PLAYER <n>"). Impl in td5_frontend.c. */
const char *frontend_human_display_name(int slot);
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
/* [NET GAME MODES 2026-07-04] 1 = the host is configuring the MP game mode from
 * the NETWORK lobby (not the local split-screen flow). Set when the net lobby's
 * GAME MODE button is pressed; routes the shared Mode-Vote / Mode-Config (and,
 * later, roles/team) screens' exits back to the net lobby instead of the local
 * car-select grid, and makes Mode-Vote a host-only picker (no per-pad voting).
 * Cleared on net-lobby (re)entry and the frontend reset. */
extern int  s_mp_net_config;
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
/* [HOST CAR OPTIONS 2026-06-28] Host-only modal on the simultaneous MP car-select
 * grid: the host (slot 0) presses X (pad) / TAB (keyboard) to raise a menu that
 * only the host drives (every other pane freezes) to force every player's car. */
enum {
    MP_HOST_OPT_SAME = 0,   /* everyone copies the host's chosen car            */
    MP_HOST_OPT_SLOW,       /* everyone -> a random SLOW car  (accel+top speed) */
    MP_HOST_OPT_AVG,        /* everyone -> a random AVERAGE car                 */
    MP_HOST_OPT_FAST,       /* everyone -> a random FAST car                    */
    MP_HOST_OPT_COUNT
};
extern int s_mp_host_menu_open;   /* 1 = host car-options modal up (panes frozen) */
extern int s_mp_host_menu_sel;    /* highlighted MP_HOST_OPT_*                    */
/* Speed class of a car by acceleration + top-speed ONLY. Returns 0/1/2 if the car
 * is in the LOW / MID / HIGH class, else -1 (invalid/cop, a dropped extreme, or a
 * car between classes). Cars are ranked by score; the single slowest and fastest
 * are dropped, then each class is K distinct cars (next-slowest / median / next-
 * fastest) with no overlap. K = TD5RE_HOST_TIER_CARS, default max(9, ~15% of the
 * roster). Defined in td5_frontend.c (reuses the cached frontend_carphys_frac stats). */
int   frontend_carphys_speed_tier(int car);
/* Normalised position [0,1] of a car in the roster's combined accel+top-speed range
 * (0 = slowest, 1 = fastest); -1 if invalid/cop. For the nearest-centre fallback. */
float frontend_car_speed_norm(int car);
/* Band centre (0.20/0.50/0.80) for tier 0/1/2. */
float frontend_speed_tier_center(int tier);
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
    int nav_selector;       /* [2026-06-26] 1 = row draws ◄► value arrows (set by
                             * fe_draw_option_arrows). Tells the shared LEFT/RIGHT nav
                             * to CYCLE this row's value instead of moving focus, so a
                             * plain L/R on a gamepad still adjusts the selector while
                             * non-selectors move focus to a same-row neighbour. */
    char label[64];
} FE_Button;

extern FE_Button s_buttons[FE_MAX_BUTTONS];
extern char s_text_input_prompt[40];

const char *frontend_get_track_name(int track_index);
extern FE_Surface s_surfaces[FE_MAX_SURFACES];
extern const char *const k_mp_kbd_rows[];

/* [R4 2026-06-19] Shared MULTIPLAYER simultaneous-pane layout bands (design space,
 * 640x480). Every MP setup screen reserves FE_MP_TOP_BAND at the top so the
 * panes/boxes (including their coloured header bars) start clearly BELOW the
 * screen title AND the MainMenu background art's upper decorative line (~y85);
 * the prior 40/50px band was too small and the boxes overlapped both. A matching
 * FE_MP_BOTTOM_BAND keeps the boxes off the art's lower text lines.
 * CROSS-FILE COUPLING: td5_fe_race.c's frontend_mp_setup_profile_render (the
 * PROFILE chip overlay) and frontend_mp_position_render2 (CHOOSE YOUR SCREEN grid)
 * define the SAME literals — keep all three in sync. */
#define FE_MP_TOP_BAND     85.0f
/* [layout 2026-06-19] Shrunk the bottom band (was 44) so the cards extend toward
 * the bottom screen edge — the art's lower text lines sit above y~466, so ~14px is
 * plenty of clearance. Cards now occupy y[85..466] instead of stopping at y436. */
#define FE_MP_BOTTOM_BAND  14.0f
/* [layout 2026-06-19] Shared horizontal grid band for the MP simultaneous-pane
 * screens. The MainMenu background art has a black bar down the LEFT (~110px), so
 * the cards must start RIGHT of it; they extend out to near the right screen edge.
 * The grid spans x[FE_MP_LEFT_MARGIN .. FE_MP_RIGHT_EDGE] (usable width ~516) so
 * with more players the cards stay as large as the available area allows, instead
 * of being capped to a narrow centred 640/3 column with a wide right gap.
 * CROSS-FILE COUPLING: td5_fe_race.c defines the SAME literals — keep in sync. */
#define FE_MP_LEFT_MARGIN  112.0f
#define FE_MP_RIGHT_EDGE   628.0f
#define FE_MP_USABLE_W     (FE_MP_RIGHT_EDGE - FE_MP_LEFT_MARGIN)

/* MP-setup TU API (td5_fe_mp_setup.c). init/update counterparts are
 * td5_fe_race.c statics; only the renders + layout helpers cross TUs. */
void frontend_commit_pane_layout(int eff_humans, int requested_spectate);
void frontend_mp_panel_capped(int cols, float *pane_w, float *row_x0);
void frontend_mp_setup_render(float sx, float sy);
void frontend_mp_simul_carsel_render(float sx, float sy);

/* --- carstats seam (td5_fe_carstats.c) --- */
extern char s_car_spec[17][48];       /* config.nfo field cache (stats sub-screen) */
extern int  s_car_spec_car;           /* which car index is cached (-1 = none) */
void frontend_render_car_stats_overlay(float sx, float sy);

/* --- devscreens seam (td5_fe_devscreens.c) --- */
extern int s_fe_preserve_case;        /* mixed-case text flag for fe_draw_text */
void  fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_measure_text(const char *text, float sx, float sy);
void  frontend_get_button_render_rect(int button_index, float sx, float sy,
                                      float *out_x, float *out_y, float *out_w, float *out_h);
void  frontend_changelog_render(float sx, float sy);
void  frontend_pending_render(float sx, float sy);
int   fe_wrap_text_lines(const char *s, float maxw, float sx, float sy,
                         char lines[][64], int max_lines);

/* --- mp_setup seam: shared frontend state + helpers (defined in td5_frontend.c) --- */
#define FE_TITLE_LEFT_X  126.0f  /* design x where the first letter starts (every screen);
                                  * = main-menu button left edge (FE_CENTER_X - 0xC2 = 320-194) */
#define SMALLFONT_TTF_CAP       9.0f


extern int s_active_menu_device;
extern TD5_ScreenIndex s_current_screen;
extern char     s_mp_pane_spec[TD5_MAX_HUMAN_PLAYERS][17][48];
extern MpSession s_mp_session;
extern const int8_t s_difficulty_tier_cars[3][6];
extern int s_speed_pool_ids[TD5_CAR_COUNT];
extern int s_speed_pool_count;
extern int  s_attract_car;
extern int  s_attract_opponents;
extern int  s_attract_traffic;

int fe_draw_arrow_proc(float x, float y, float w, float h, int dir_right, uint32_t color);
void fe_draw_button_frame_fill_scaled(float bx, float by, float bw, float bh, int bb_state, uint32_t interior, float border_scale, float sx, float sy);
void fe_draw_quad(float x, float y, float w, float h, uint32_t color, int tex_page, float u0, float v0, float u1, float v1);
void fe_draw_surface_rect(int handle, float x, float y, float w, float h, uint32_t color);
void fe_draw_text_centered(float center_x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_glyph_sx(float sx, float sy);
int frontend_ai_ext_id_taken(int ext_id, const int *slot_ext_ids, const int *slot_active, int count);
void frontend_build_speed_pool(void);
void frontend_draw_car_stat_bars(float bx, float by, float bw, float bh, const char *f7, const char *f8, int car_ext_id, uint32_t accent, int compact, float frame_scale, float sx, float sy);
void frontend_draw_screen_title(const char *text, float left_x, float top_y, uint32_t color, float sx, float sy);
const char *frontend_get_car_display_name(int car_index);
void frontend_render_physics_stats(int ext_id, float px, float py, float pw, float ph, uint32_t accent, int compact, float sx, float sy);
uint32_t frontend_rgb_to_bgra(uint32_t c);
void frontend_speed_band_for_tier(int tier, int *lo, int *hi);
uint32_t td6_hsv_to_rgb(float h, float s, float v);
void fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_measure_small_text(const char *text);
/* [UI GUIDE 2026-07-03] Canonical shared widget renderers, exported so every
 * fe_*.c screen (and the dev UI GUIDE gallery) uses THE routine instead of
 * hand-rolling: the selector ◄► arrows every selector row needs, the standard
 * centred option value, and the MP mode-vote border ring. All were file-static
 * before (td5_frontend.c / td5_fe_race.c). */
void fe_draw_option_arrows(int btn_idx, float sx, float sy);
void frontend_draw_value_centered(float sx, float sy, int y, const char *text, uint32_t color);
void mp_mode_draw_border_ring(float bx, float by, float bw, float bh, float out, float th, uint32_t color, float sx, float sy);
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
extern int      s_mp_player_laneassist[TD5_MAX_HUMAN_PLAYERS];
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
/* [RACE OPTIONS MODAL 2026-07-04] Consolidated race-options modal shown over the
 * track-select screen. Defined in td5_fe_race.c; the render dispatch + central
 * ESC handler in td5_frontend.c query/draw it. */
/* [RACE OPTIONS 2026-07-04] Dedicated screen (TD5_SCREEN_RACE_OPTIONS) opened by
 * the track-select RACE OPTIONS button; consolidates every per-race option and
 * reuses the track-select backdrop. Screen body in td5_fe_race.c; the label/value/
 * cycle model + value render live in td5_frontend.c. */
void Screen_RaceOptions(void);
/* Option ids: screen button i (0..RO_OPT_COUNT-1) maps 1:1 to these; OK + BACK
 * follow. */
enum {
    RO_OPPONENTS = 0, RO_TRAFFIC, RO_POLICE, RO_DIFFICULTY, RO_DYNAMICS,
    RO_CHECKPOINTS, RO_POWERUPS, RO_TOUGHNESS, RO_DEFORM, RO_OPT_COUNT
};
const char *td5_raceopts_label(int idx);
void        td5_raceopts_value(int idx, char *out, size_t out_sz);
void        td5_raceopts_cycle(int idx, int delta);
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
/* [2026-06-29] The simultaneous-MP car-select grid pane now carries only CAR /
 * PAINT / OK: MORE STATS was dropped (the at-a-glance SPEED/ACCEL/HANDLING bars
 * are always shown in each pane, so the spec overlay was redundant) and the
 * AUTO/MANUAL transmission toggle moved to the MP Profile-Selection screen
 * (alongside the new LANE ASSIST toggle — see MP_SET_TRANS/MP_SET_LANEASSIST). */
enum { MP_BTN_CAR = 0, MP_BTN_PAINT, MP_BTN_OK, MP_BTN_COUNT };
enum { MP_SET_NAME = 0, MP_SET_COLOUR, MP_SET_OK, MP_SET_COUNT };
/* Extra MP profile-setup button ids past the header enum (PROFILE=3 is #defined in
 * td5_fe_race.c). AUTO/MANUAL + LANE ASSIST live on the profile-setup screen between
 * PROFILE and OK; defined here so both td5_fe_race.c and td5_frontend.c (render) see them. */
#define MP_SET_TRANS      4
#define MP_SET_LANEASSIST 5
/* Shared per-player pane-button drawer (defined in td5_frontend.c). Exposed so the
 * PROFILE button drawn from td5_fe_race.c uses the IDENTICAL frame/label rendering
 * as its NAME/COLOUR/AUTO-MANUAL/ASSIST/OK siblings (was a drifting replica). */
void mp_simul_draw_btn(float x, float y, float w, float h, const char *label,
                       int focused, uint32_t pcol, int arrows,
                       const char *val, int swatch_rgb, float sx, float sy);
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
/* [PHYSICS 2026-06-26] ARCADE/SIMULATION (dynamics) row on Quick Race, between
 * Laps (row 4) and the dev rows. Created LAST (after the dev toggles + the two
 * RANDOMIZE buttons at indices 12/13) so every hard-coded index above stays put.
 * Visible in BOTH dev and release; flips the shared s_game_option_dynamics. */
#define QR_BTN_PHYSICS    14

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
 * Offset" row fit the 480px canvas.
 * [PHYSICS 2026-06-26] Tightened again 44 -> 40 ("put the option buttons
 * together a little more") so the NEW Physics (ARCADE/SIMULATION) row makes 9
 * rows total (0..8) and still fits: OK/Back move down to row 8
 * (y = 96 + 8*40 = 416, 32px tall -> bottom 448, clear of the 480 canvas). 32px
 * buttons leave an 8px gap; the title rests at ~17 so the first row at 96 stays clear. */
#define QR_ROW_DY       40    /* uniform vertical gap between rows         */
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
extern int             s_game_option_powerups;   /* [ITEM CHAOS 2026-07-04] 0=OFF 1=CASUAL 2=CHAOS */
extern int             s_game_option_laneassist;
extern int             s_game_option_difficulty;
extern int             s_game_option_dynamics;
extern int             s_game_option_car_toughness;   /* [CAR DAMAGE] 0=Low 1=Normal 2=High */
extern int             s_game_option_car_deform;      /* [CAR DAMAGE] 0=Low 1=Normal 2=High */
extern int             s_game_option_car_damage;      /* [DAMAGE 2026-07-04] single toggle: master car-damage + HUD bar/wreck */
extern int             s_game_option_tutorial;        /* [TUTORIAL 2026-06-29] controller overlay every race on/off */
extern int             s_race_difficulty;   /* per-race AI difficulty row on Track Selection (0..2) */
extern int             s_trksel_dyn_btn;    /* [ARCADE] ARCADE/SIM row index on Track Selection (-1=none) */

/* --- Paginated Game Options model (impl in td5_frontend.c, driven by the
 * Screen_GameOptions FSM in td5_fe_menu.c). Same pagination scheme as the
 * PENDING TO TEST checklist: rows-per-page + < PREV / NEXT > + a page footer.
 * [TUTORIAL 2026-06-29] ------------------------------------------------------*/
int  td5_gameopts_count(void);                 /* total selectable option rows */
int  td5_gameopts_page(void);                  /* current 0-based page */
int  td5_gameopts_pages(void);                 /* page count */
int  td5_gameopts_row_count(void);             /* visible option rows on this page */
int  td5_gameopts_row_option(int row);         /* option index for a page row, or -1 */
int  td5_gameopts_ok_btn(void);                /* button index of OK on this page */
int  td5_gameopts_prev_btn(void);              /* < PREV button index, or -1 */
int  td5_gameopts_next_btn(void);              /* NEXT > button index, or -1 */
void td5_gameopts_value(int option, char *out, size_t out_sz); /* value string */
void td5_gameopts_cycle(int option, int delta);/* LEFT/RIGHT cycle an option */
void td5_gameopts_build_page(void);            /* (re)create buttons for current page */
void td5_gameopts_reset_page(void);            /* jump back to page 0 (on screen entry) */
int  td5_gameopts_page_prev(void);             /* page--; rebuild; 1 if it moved */
int  td5_gameopts_page_next(void);             /* page++; rebuild; 1 if it moved */
/* [PLAYER NAME 2026-07-02] The PLAYER NAME row (Enter-to-edit, no ◄►). */
int  td5_gameopts_name_option(void);           /* option index of the name row */
void td5_gameopts_name_edit_begin(void);       /* open the text-input editor */
int  td5_gameopts_name_edit_tick(void);        /* 1 when editor closed (ok/esc) */
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
/* Screen_LanguageSelect / Screen_LegalCopyright RETIRED 2026-07-03 —
 * table slots [3]/[4] are NULL; set_screen redirects them to MAIN_MENU. */
void Screen_LocalizationInit(void);
void Screen_MainMenu(void);
void Screen_MusicTestExtras(void);
void Screen_OptionsHub(void);
void Screen_UiGuide(void);              /* dev UI style guide (slot 1, td5_fe_devscreens.c) */
void frontend_uiguide_render(float sx, float sy);
void Screen_MpGuide(void);              /* dev MP-widgets gallery (slot 44, td5_fe_devscreens.c) */
void frontend_mpguide_render(float sx, float sy);
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
