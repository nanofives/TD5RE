/**
 * td5_frontend.c -- Menu screens, screen table, navigation
 *
 * Implements the 30-entry screen dispatch table originally at 0x4655C4.
 * Each screen is a state machine driven by s_inner_state (g_frontendInnerState).
 * Navigation via td5_frontend_set_screen() resets state to 0 and timestamps.
 *
 * Original binary addresses are noted in comments for cross-reference.
 */

#include "td5_frontend.h"
#include "td5_asset.h"
#include "td5_frontend_button_cache.h"
#include "td5_game.h"
#include "td5_profile.h"
#include "td5_input.h"
#include "td5_physics.h"
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_sound.h"
#include "td5_hud.h"           /* per-viewport player-identity overlay (race) */
#include "td5re.h"
#include "td5_snk_strings.h"   /* byte-exact SNK_ labels baked from Language.dll */
#include "td5_credits.h"       /* SNK_CreditsText array + dev mugshot map (Extras scroll) */
#include "td5_vectorui.h"      /* public VectorUI surface (HUD reuses these primitives) */
#include "td5_font.h"          /* [S13] runtime TTF glyph cache (native menu text) */
#include "deps/cjson/cJSON.h"  /* track-marker JSON (retired trak_markers*.dat) */
#include "../../ddraw_wrapper/src/wrapper.h"
#include "../../ddraw_wrapper/src/shaders/ps_msdf_bytes.h"       /* g_ps_msdf bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_roundrect_bytes.h"  /* g_ps_roundrect bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_arrow_bytes.h"       /* g_ps_arrow bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_cursor_bytes.h"      /* g_ps_cursor bytecode */
#include "../../ddraw_wrapper/src/shaders/ps_gauge_bytes.h"       /* g_ps_gauge bytecode */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================
 * Module Tag
 * ======================================================================== */

#define LOG_TAG "frontend"
#include "td5_color.h"
#include "td5_frontend_internal.h"

/* Forward declarations for functions used before definition */
 void frontend_init_font_metrics_from_pixels(const uint8_t *pixels, int w, int h);
 void frontend_init_font_metrics_default(void);
static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1);
/* Like fe_draw_quad but draws a parallelogram: the two top vertices are shifted
 * by dx_top and the two bottom vertices by dx_bot (used for faux-italic title
 * text). */
static void fe_draw_quad_sheared(float x, float y, float w, float h,
                                 float dx_top, float dx_bot,
                                 uint32_t color, int tex_page,
                                 float u0, float v0, float u1, float v1);

/* ========================================================================
 * Forward declarations -- screen functions (30 entries)
 * ======================================================================== */


/* ========================================================================
 * Screen function pointer type and dispatch table
 * ======================================================================== */

typedef void (*ScreenFn)(void);

/**
 * 30-entry screen dispatch table.
 * Mirrors the original table at 0x4655C4 in the PE image.
 */
static ScreenFn s_screen_table[TD5_SCREEN_COUNT] = {
    /* [ 0] */ Screen_LocalizationInit,
    /* [ 1] */ Screen_PositionerDebugTool,
    /* [ 2] */ Screen_AttractModeDemo,
    /* [ 3] */ Screen_LanguageSelect,
    /* [ 4] */ Screen_LegalCopyright,
    /* [ 5] */ Screen_MainMenu,
    /* [ 6] */ Screen_RaceTypeCategory,
    /* [ 7] */ Screen_QuickRaceMenu,
    /* [ 8] */ Screen_ConnectionBrowser,
    /* [ 9] */ Screen_SessionPicker,
    /* [10] */ Screen_CreateSession,
    /* [11] */ Screen_NetworkLobby,
    /* [12] */ Screen_OptionsHub,
    /* [13] */ Screen_GameOptions,
    /* [14] */ Screen_ControlOptions,
    /* [15] */ Screen_SoundOptions,
    /* [16] */ Screen_DisplayOptions,
    /* [17] */ Screen_TwoPlayerOptions,
    /* [18] */ Screen_ControllerBinding,
    /* [19] */ Screen_MusicTestExtras,
    /* [20] */ Screen_CarSelection,
    /* [21] */ Screen_TrackSelection,
    /* [22] */ Screen_ExtrasGallery,
    /* [23] */ Screen_PostRaceHighScore,
    /* [24] */ Screen_RaceResults,
    /* [25] */ Screen_PostRaceNameEntry,
    /* [26] */ Screen_CupFailed,
    /* [27] */ Screen_CupWon,
    /* [28] */ Screen_StartupInit,
    /* [29] */ Screen_SessionLocked,
    /* [30] */ Screen_MultiplayerLobby,   /* PORT ENHANCEMENT 2026-06 */
    /* [31] */ Screen_LanMenu,            /* S10 net-play UX */
    /* [32] */ Screen_DirectConnect,      /* S10 net-play UX */
    /* [33] */ Screen_NetNickname,        /* S10 net-play UX */
    /* [34] */ Screen_MpPosition,         /* MP split-screen position picker (#8) */
};

/* ========================================================================
 * Module-level state
 * ======================================================================== */

static TD5_ScreenIndex s_current_screen;
int  s_inner_state;              /* g_frontendInnerState   */
int  s_anim_tick;                /* g_frontendAnimFrameCounter (0x49522C) */

/* [FPS-DECOUPLE 2026-06-07] Number of fixed 60 Hz animation ticks elapsed since
 * the previous frontend frame (computed in frontend_update_anim_pacing at the
 * top of td5_frontend_display_loop). Every per-frame frontend animation step —
 * s_anim_tick, s_fade_progress, the background-slideshow blend, the track-
 * preview slide — is multiplied by this so it advances at a constant real-time
 * speed regardless of the (now-uncapped) render FPS. Usually 0 or 1; >1 only
 * when FPS drops below 60. Rendering, input and present still run every frame. */
static uint32_t s_fe_anim_prev_ms  = 0;
static float    s_fe_anim_accum_ms = 0.0f;
int      s_fe_logic_ticks   = 1;
int  s_return_screen;            /* g_returnToScreenIndex  */
static int  s_start_race_request;       /* 0x495248               */
static int  s_start_race_confirm;       /* 0x49524C               */
static int  s_attract_idle_counter;     /* g_attractModeIdleCounter */
uint32_t s_attract_idle_timestamp;
int  s_attract_demo_active;      /* g_attractModeDemoActive @ 0x495254 */

/* -----------------------------------------------------------------------
 * Screen [27] CupWon — persistent unlock counts for overlay renderer
 * Set in state 0 via td5_save_apply_cup_unlocks_ex; read in states 4-5.
 * [CONFIRMED @ 0x423A80]: original DAT_00494bb0 = car count, DAT_00494bb4 = track count.
 * ----------------------------------------------------------------------- */
int s_cup_won_car_count;
int s_cup_won_track_count;


/* -----------------------------------------------------------------------
 * fe_draw_quad render log — written when [Logging] FrontendDraw=1
 * ----------------------------------------------------------------------- */
static FILE *s_fe_draw_log_fp;
static int   s_fe_draw_log_frame;


/* Live-rendered overlay strings — updated whenever the track selection or
 * "Now Playing" state changes.  The original drew into offscreen DDraw
 * surfaces; the port renders these strings every frame via the UI-rect pass.
 * Format: track-label  = "%d. %s" (track# 1-based + band name)   [CONFIRMED @ 0x465f74]
 *         now-playing panel rows at y=0/0x28/0x50                 [CONFIRMED @ 0x418571]
 */
char s_music_test_track_label[64];   /* e.g. "1. GRAVITY KILLS" */
char s_music_test_now_band[64];      /* band name of currently playing track */
char s_music_test_now_title[64];     /* song title of currently playing track */
int  s_music_test_playing_set;       /* 1 once CDPlay has been called */
/* Music Test album cover art: 5 band covers + the 12-track->5-band LUT.
 * [CONFIRMED @ 0x40d6a0 LoadExtrasBandGalleryImages load order:
 *  0=Fear Factory,1=Gravity Kills,2=Junkie XL,3=KMFDM,4=PitchShifter;
 *  LUT @0x465e4c; drawn at (0x76,0x8c)=(118,140) by UpdateExtrasGalleryDisplay@0x40d830.] */
int  s_band_cover_surface[5];
static const int k_music_track_to_band[12] = { 1,3,4,4,2,0,0,1,3,4,4,4 };
/* The band whose cover is shown. [CONFIRMED @0x418460] orig keys the cover on
 * g_attractCdTrackCandidate, which is set ONLY when SELECT is pressed — so the album
 * art (and now-playing panel) reflect the PLAYED track, not the one being previewed
 * with ◄►. Track 0 plays at entry, so this starts at 0. */
int  s_music_attract_track = 0;
int  s_input_ready;              /* DAT_004951e8            */
int  s_button_index;             /* currently pressed button */
int  s_arrow_input;              /* DAT_0049b690 arrow direction */
/* [PORT ENHANCEMENT 2026-06] gamepad frontend-nav bits this frame (bit4 A,
 * bit5 B); cached so frontend_check_escape() can read B without re-polling. */
uint32_t s_fe_gamepad_nav;
/* [PORT ENHANCEMENT 2026-06] the "active controller" driving the menus: whichever
 * device last gave input (0 = keyboard, >=1 = enumerated joystick). It becomes
 * the driver (player 0's device) for single-player races. */
static int s_active_menu_device = 0;
static uint32_t s_screen_entry_timestamp;
uint32_t s_anim_start_ms = 0;
static uint32_t s_anim_elapsed_ms = 0;
int  s_anim_complete = 0;
static float s_anim_t = 0.0f;          /* continuous 0..1 for smooth button position */
static TD5_ScreenIndex s_previous_screen = (TD5_ScreenIndex)-1;

/* Context / flow tracking (DAT_004962d4) */
int  s_flow_context;

/* Game type / race configuration */
int  s_selected_game_type;       /* g_selectedGameType     */
static int  s_race_rule_variant;        /* gRaceRuleVariant       */
int  s_race_within_series;       /* g_raceWithinSeriesIndex */
int  s_cup_unlock_tier;          /* DAT_004962a8           */

/* Two-player mode flag (DAT_004962a0) */
int  s_two_player_mode;
/* [PORT ENHANCEMENT 2026-06] Multiplayer Options split-layout picker state
 * (replaces the original Two Player Options split-on/off toggle + CATCHUP level,
 * both removed). The layout is chosen per local-human-count; see mp_split_layouts.
 *   s_mp_layout_sel        — index into mp_split_layouts(num_human_players)
 *   s_mp_missing_content[k] — stub content id for the k-th empty grid cell
 *                             (the cell-content feature itself is deferred). */
int  s_mp_layout_sel = 0;
int  s_mp_missing_content[2] = { 0, 0 };
/* [MP POSITION SELECT 2026-06-15 (#6/#8)] Per-player chosen split-screen CELL.
 * s_mp_player_cell[p] = the grid cell (0..cols*rows-1, row-major) that human
 * player p occupies. Default IDENTITY (cell[p]=p) so AutoRace / the harness /
 * any non-positioned MP race renders byte-identical to before. The position
 * screen (Screen_MpPosition) writes it; td5_frontend_mp_view_actor_slot() reads
 * the inverse at InitRace time so each player appears in THEIR chosen pane.
 * Frontend/render only — never routed through the deterministic sim or net. */
int  s_mp_player_cell[TD5_MAX_HUMAN_PLAYERS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
/* Dynamic button-index bookkeeping for the rebuilt Multiplayer Options rows
 * (the row set changes with the player count, so buttons are rebuilt live). */
int  s_mp_btn_players   = -1;
int  s_mp_btn_catchup   = -1;   /* [S05 2026-06-04] CATCHUP toggle row */
int  s_mp_btn_layout    = -1;
int  s_mp_btn_missing[2] = { -1, -1 };
int  s_mp_btn_nickname  = -1;    /* S10: edit net-play nickname (below split rows) */
int  s_mp_missing_count = 0;
int  s_mp_layout_optcount = 1;

/* [PORT ENHANCEMENT 2026-06] MULTIPLAYER press-to-join lobby + sequential car
 * select. join order = player number; each joined player records its device. */
int      s_mp_flow         = 0;     /* 1 = multiplayer setup flow active */
int      s_mp_joined_count = 0;     /* players who have joined the lobby */
int      s_mp_join_device[TD5_MAX_HUMAN_PLAYERS]; /* device idx per joined player */
int      s_mp_car_player    = 0;    /* which player is picking (sequential car select) */
int      s_mp_player_car[TD5_MAX_HUMAN_PLAYERS];   /* per-player chosen car */
int      s_mp_player_paint[TD5_MAX_HUMAN_PLAYERS]; /* per-player chosen paint */

/* [PORT ENHANCEMENT 2026-06-07] SIMULTANEOUS grid car-select: every joined
 * player picks their car at the same time, each driven by their OWN controller,
 * in panes laid out by the chosen split-screen grid (mp_resolve_layout — the
 * same layout the race viewports use). Each pane is "forked" with a coloured
 * PLAYER N header so everyone can find their own screen. */
int      s_mp_player_color[TD5_MAX_HUMAN_PLAYERS];     /* per-player TD6 body colour (0xRRGGBB); -1 = grey */
int      s_mp_player_ready[TD5_MAX_HUMAN_PLAYERS];     /* 1 once that player has locked their pick */
uint32_t s_mp_pane_nav_prev[TD5_MAX_HUMAN_PLAYERS];    /* prev-frame nav bits per player (edge detect) */
int      s_mp_simul         = 0;    /* 1 = simultaneous grid car-select is active */
uint32_t s_mp_simul_ready_ms = 0;   /* time all players became READY (0 = not yet); drives the auto-advance beat */
uint32_t s_mp_simul_anim_ms = 0;    /* lobby->car-select slide-in animation start time */
int      s_mp_pane_preview[TD5_MAX_HUMAN_PLAYERS];  /* cached carpic surface handle per pane */
int      s_mp_pane_overlay[TD5_MAX_HUMAN_PLAYERS];  /* cached TD6 body-paint overlay handle per pane (0 = none) */
int      s_mp_pane_btn[TD5_MAX_HUMAN_PLAYERS];      /* focused button per pane (MP_BTN_*) */
int      s_mp_player_trans[TD5_MAX_HUMAN_PLAYERS];  /* 0 = Automatic, 1 = Manual */
int      s_mp_pane_substate[TD5_MAX_HUMAN_PLAYERS]; /* 0 = car select, 1 = stats spec sheet */
int      s_mp_pane_spec_car[TD5_MAX_HUMAN_PLAYERS]; /* which car's spec is cached per pane (-1 = none) */
static char     s_mp_pane_spec[TD5_MAX_HUMAN_PLAYERS][17][48]; /* per-pane config.nfo fields */
/* Per-pane car-select button set (compact, navigated by that player's own pad). */
/* Default identity colour per player (0xAARRGGBB); overridden by the setup window. */
const uint32_t k_mp_player_colors[TD5_MAX_HUMAN_PLAYERS] = {
    0xFFFF4040, /* P1 red    */ 0xFF4080FF, /* P2 blue   */ 0xFF40D040, /* P3 green  */
    0xFFFFD030, /* P4 yellow */ 0xFFFF8020, /* P5 orange */ 0xFFE060E0, /* P6 magenta*/
    0xFF40D0D0, /* P7 cyan   */ 0xFFF0F0F0, /* P8 white  */ 0xFFA0E040, /* P9 lime   */
};

/* [PORT 2026-06-07] PLAYER-SETUP window (phase 0, before car select): each player
 * types a NAME and picks a background/identity COLOUR (the same TD6 colour picker
 * the car-select uses). Keyboard players type directly (high-score style); pad
 * players get an on-screen QWERTY. Then phase 1 = the car-select grid. */
int  s_mp_phase = 0;                                   /* 0 = setup, 1 = car select */
char s_mp_player_name[TD5_MAX_HUMAN_PLAYERS][16];      /* chosen display name */
int  s_mp_player_accent[TD5_MAX_HUMAN_PLAYERS];        /* chosen identity colour (0xRRGGBB) */
int  s_mp_setup_sub[TD5_MAX_HUMAN_PLAYERS];            /* 0 idle, 1 name entry, 2 colour picker */
int  s_mp_setup_btn[TD5_MAX_HUMAN_PLAYERS];            /* idle focus: 0 NAME, 1 COLOUR, 2 OK */
int  s_mp_kbd_col[TD5_MAX_HUMAN_PLAYERS];              /* QWERTY cursor (pad name entry) */
int  s_mp_kbd_row[TD5_MAX_HUMAN_PLAYERS];
int  s_mp_col_col[TD5_MAX_HUMAN_PLAYERS];              /* colour-grid cursor */
int  s_mp_col_row[TD5_MAX_HUMAN_PLAYERS];

/* [MP SESSION PERSISTENCE 2026-06] Process-lifetime store of the last committed
 * multiplayer roster (name/accent/car/paint/color/trans per player). This is a
 * SEPARATE static from the s_mp_* working arrays, so the Main-Menu cleanup
 * (td5_fe_menu.c) and td5_frontend_init() resource wipe — which only touch the
 * s_mp_* working set — leave it intact. It is filled once per race launch in
 * frontend_init_race_schedule() and read back when the MP flow re-seeds the
 * lobby / car-select. Frontend-only: it is NEVER routed through the net config
 * structs (that path must stay deterministic). See MpSession in the internal
 * header for the field contract (incl. the reserved position-screen fields). */
static MpSession s_mp_session;   /* zero-init: valid=0 until the first race commit */

int mp_session_enabled(void) {
    /* TD5RE_MP_SESSION: default ON; exactly "0" restores the old wipe-every-time
     * behaviour. Cached so repeated lobby frames don't re-hit getenv. */
    static int s_cached = -1;
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_MP_SESSION");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
    }
    return s_cached;
}

int mp_session_is_valid(void) {
    return mp_session_enabled() && s_mp_session.valid;
}

int mp_session_count(void) {
    return mp_session_is_valid() ? s_mp_session.count : 0;
}

void mp_session_save_player(int p) {
    if (p < 0 || p >= TD5_MAX_HUMAN_PLAYERS) return;
    memcpy(s_mp_session.name[p], s_mp_player_name[p], sizeof(s_mp_session.name[p]));
    s_mp_session.name[p][sizeof(s_mp_session.name[p]) - 1] = '\0';
    s_mp_session.accent[p] = s_mp_player_accent[p];
    s_mp_session.car[p]    = s_mp_player_car[p];
    s_mp_session.paint[p]  = s_mp_player_paint[p];
    s_mp_session.color[p]  = s_mp_player_color[p];
    s_mp_session.trans[p]  = s_mp_player_trans[p];
}

void mp_session_restore_player(int p) {
    if (!mp_session_is_valid()) return;
    if (p < 0 || p >= TD5_MAX_HUMAN_PLAYERS || p >= s_mp_session.count) return;
    memcpy(s_mp_player_name[p], s_mp_session.name[p], sizeof(s_mp_player_name[p]));
    s_mp_player_name[p][sizeof(s_mp_player_name[p]) - 1] = '\0';
    s_mp_player_accent[p] = s_mp_session.accent[p];
    s_mp_player_car[p]    = s_mp_session.car[p];
    s_mp_player_paint[p]  = s_mp_session.paint[p];
    s_mp_player_color[p]  = s_mp_session.color[p];
    s_mp_player_trans[p]  = s_mp_session.trans[p];
}

/* Display helpers for the lobby roster: read the persisted name/accent for a
 * player WITHOUT clobbering the live working arrays (used to label a freshly
 * joined slot before any restore runs). Return NULL/0 when nothing stored. */
static const char *mp_session_player_name(int p) {
    if (!mp_session_is_valid() || p < 0 || p >= s_mp_session.count) return NULL;
    return s_mp_session.name[p][0] ? s_mp_session.name[p] : NULL;
}
static int mp_session_player_accent(int p) {
    if (!mp_session_is_valid() || p < 0 || p >= s_mp_session.count) return 0;
    return s_mp_session.accent[p];
}

/* ---- MP split-screen POSITION SELECT (#6/#8) ----
 * TD5RE_MP_POSITIONS: default ON; exactly "0" skips the picker screen AND forces
 * the identity viewport mapping (= legacy behaviour). Cached. */
int mp_positions_enabled(void) {
    static int s_cached = -1;
    if (s_cached < 0) {
        const char *e = getenv("TD5RE_MP_POSITIONS");
        s_cached = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
    }
    return s_cached;
}

/* Persist the just-committed per-player cells into the process-lifetime store and
 * mark positions assigned for the current human count. Called when the picker
 * confirms (all ready). Gated by both knobs. */
void mp_session_commit_positions(int humans) {
    int p;
    /* Gated on the POSITIONS feature only (not TD5RE_MP_SESSION): pos_assigned +
     * cell[] are what the per-race viewport map reads, so they must be set even
     * when roster persistence is off. The fields live in s_mp_session for storage
     * convenience but are governed by TD5RE_MP_POSITIONS. */
    if (!mp_positions_enabled()) return;
    if (humans < 0) humans = 0;
    if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        s_mp_session.cell[p] = s_mp_player_cell[p];
    s_mp_session.pos_assigned       = 1;
    s_mp_session.pos_count_committed = humans;
}

/* If positions are stored for EXACTLY the current human count, copy them into the
 * live s_mp_player_cell[] and return 1 (caller may then SKIP the picker). Returns
 * 0 when the picker must be shown (feature off, never assigned, or the count
 * changed — e.g. a new player joined). */
int mp_session_restore_positions(int humans) {
    int p;
    if (!mp_positions_enabled()) return 0;
    if (!s_mp_session.pos_assigned) return 0;
    if (s_mp_session.pos_count_committed != humans) return 0;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        s_mp_player_cell[p] = s_mp_session.cell[p];
    return 1;
}

/* Reset the live cell assignment to identity (cell[p]=p). Used when entering the
 * picker fresh so a stale permutation from a prior, different-sized roster can't
 * leak in, and to guarantee the AutoRace/harness default. */
void mp_positions_reset_identity(void) {
    int p;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        s_mp_player_cell[p] = p;
}

/* [#6] Inverse permutation consumed by td5_game InitRace: given a split-screen
 * grid CELL (= viewport index, row-major), return the racer/actor slot that
 * should be SHOWN there. Human player p drives actor slot p in the local MP
 * flow, and picked cell s_mp_player_cell[p]; so the cell->slot map is the inverse
 * of player->cell. Returns -1 when positions are not active (knobs off / not
 * assigned / not the local MP flow), so the caller keeps the identity default. */
int td5_frontend_mp_view_actor_slot(int cell) {
    int p;
    if (!mp_positions_enabled()) return -1;
    if (!s_mp_session.pos_assigned) return -1;       /* nothing committed -> identity */
    if (!(s_mp_flow && s_two_player_mode != 0)) return -1;  /* only the local MP race */
    if (cell < 0 || cell >= TD5_MAX_HUMAN_PLAYERS) return -1;
    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++)
        if (s_mp_player_cell[p] == cell) return p;   /* player p occupies this cell */
    return -1;                                        /* unclaimed cell -> identity */
}

/* [#2 2026-06-15] Local player p's MENU transmission choice: 1 = MANUAL, 0 = AUTO.
 * In a multi-human (split-screen) setup each player picks their own gearbox via
 * s_mp_player_trans[p]; the single-player flow uses the shared s_selected_transmission.
 * Both use 1=Manual/0=Auto. Read by td5_input.c so selecting MANUAL in the menu
 * actually puts that player's car into manual (the physics side already keeps a
 * manual car from auto-shifting). Out-of-range players default to AUTO (0). */
int td5_frontend_get_player_manual(int player) {
    if (player < 0 || player >= TD5_MAX_HUMAN_PLAYERS) return 0;
    if (s_num_human_players > 1)
        return s_mp_player_trans[player] ? 1 : 0;
    return (player == 0 && s_selected_transmission) ? 1 : 0;
}

/* On-screen QWERTY (pad name entry): 4 letter rows + a special row (SPACE/DEL/DONE). */
const char *const k_mp_kbd_rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
#define MP_KBD_SPECIAL     MP_KBD_LETTER_ROWS
#define MP_KBD_ROW_H       14.4f                       /* compact: ~20% over the cap height (9) */
#define MP_KBD_BLOCK_H     (MP_KBD_ROWS * MP_KBD_ROW_H)
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
#define FE_MP_BOTTOM_BAND  44.0f
/* Background-colour picker: a compact 16x16 pure HSV palette (no preselected
 * swatch rows, no white bar). */
 void frontend_mp_setup_init(void);
 void frontend_mp_setup_update(void);
static void frontend_mp_setup_render(float sx, float sy);
/* MP split-screen position picker (#8): handler in td5_fe_race.c, render here. */
void frontend_mp_position_render(float sx, float sy);

/* Simultaneous-grid car-select entry points (defined just before Screen_CarSelection). */
 void frontend_mp_simul_carsel_init(void);
 void frontend_mp_simul_carsel_update(void);
static void frontend_mp_simul_carsel_render(float sx, float sy);

/* ScreenLocalizationInit bootstrap control [CONFIRMED @ 0x4269D0 g_attractModeControlEnabled]:
 * 0 = first entry (run full init), 1 = re-entry (skip init, go to menu),
 * 2 = resume-cup re-entry (go to RACE_RESULTS with skip_display=1) */
int  s_attract_mode_ctrl;

/* Car selection state */
int  s_selected_car;             /* DAT_0048f31c / DAT_0048f364 */
int  s_selected_paint;           /* DAT_0048f308 / DAT_0048f368 */
int  s_selected_config;          /* DAT_0048f370            */
int  s_selected_transmission;    /* DAT_0048f338 / DAT_0048f378 */
int  s_p2_car;                   /* DAT_00463e08            */


/* Drag-race CarSelect 2-pass counter [CONFIRMED @ DAT_0048f380].
 * Original binary gates this on g_selectedGameType == 7 which is DRAG RACE in the
 * original; the port's game_type convention has Drag Race = 9. 0 = picking car 1,
 * 1 = picking car 2. Reset on Back, on race entry, and on screen leave. */
int  s_drag_carselect_pass;

/* Track selection state */
int  s_selected_track;           /* DAT_004a2c90            */
int  s_attract_track;            /* random track for attract demo; never overwrites s_selected_track */
int  s_track_direction;          /* DAT_004a2c98: 0=fwd, 1=bwd */
/* Quick Race player setup (infra to later replace the Two-Player menu).
 * Only the Quick Race screen writes these; all other launch flows leave the
 * defaults (1 human + 5 AI = legacy single-race grid). See g_td5.num_* . */
int  s_num_human_players = 1;    /* 1..TD5_MAX_RACER_SLOTS            */
int  s_num_ai_opponents  = 5;    /* 0..(TD5_MAX_RACER_SLOTS-1)        */
/* [PORT ENHANCEMENT 2026-06-08] Quick Race "AI Screens" selector (dev/profiling).
 * How many AI cars (slots 1..N) get their own split-screen viewport pane on top
 * of the player's pane — viewport_count = 1 + this. 0 = legacy single view.
 * Inert outside Quick Race; clamped to the AI field + the viewport cap. */
int  s_num_spectate_screens = 0; /* 0..min(opponents, TD5_MAX_VIEWPORTS-1) */
int  s_score_category_index;    /* DAT_00497a68: current track in score table */
/* [#2b 2026-06-16] Post-race high-score TD6 context: 0 = a normal (TD5) track,
 * uses the authored NPC group; >0 = the active TD6 level number whose genuine
 * record table (td5_save_get_td6_record_group) the high-score overlay should
 * render instead of a clamped TD5 group's placeholder names. Set by the post-race
 * screens (td5_fe_race.c) in their state 0; only consulted while showing the
 * post-race table (NAME_ENTRY) — the Records browse screen leaves it 0. */
int  s_postrace_td6_level = 0;

#define FE_MAX_DISPLAY_MODES 64
static TD5_DisplayMode s_display_modes[FE_MAX_DISPLAY_MODES];
static char            s_display_mode_names[FE_MAX_DISPLAY_MODES][32];
int             s_display_mode_count;
int             s_display_mode_index;
int             s_display_fog_enabled = 1;
int             s_display_speed_units;
int             s_display_camera_damping = 5;
/* [S01 Display options 2026-06-04] new rows: window mode (0=fullscreen,
 * 1=windowed, 2=borderless), vsync (0/1), show-fps (0/1). Mirrored from
 * g_td5.ini on screen entry, applied live on change, persisted on OK. */
int             s_display_window_mode = 1;
int             s_display_vsync       = 1;
int             s_display_show_fps    = 1;
int             s_game_option_laps = 0;
int             s_game_option_checkpoint_timers = 1;
int             s_game_option_traffic = 1;
int             s_game_option_cops = 1;
int             s_game_option_difficulty = 1;
/* [2026-06-12] Per-race AI difficulty picked on the Track Selection screen
 * (0..2, seeded from g_td5.difficulty_tier on entry, committed back on OK).
 * Quick Race never shows the row and keeps the Game Options global. */
int             s_race_difficulty = 1;
int             s_game_option_dynamics = 0;
int             s_game_option_collisions = 1;
int             s_sound_option_sfx_mode;
int             s_sound_option_sfx_volume = 80;
int             s_sound_option_music_volume = 80;

/* Car roster size. The original game has 37 cars (0-36). The source port
 * appends 39 ported Test Drive 6 cars at indices 37-75 (see s_car_zip_paths
 * and td5_asset.c). TD5_BASE_CAR_COUNT gates anything that mirrors the original
 * save/unlock structure (only the 37 originals are tracked there); TD5_CAR_COUNT
 * sizes the port-side roster tables. */

/* Lock tables (simplified inline representation) */
uint8_t s_car_lock_table[TD5_CAR_COUNT];    /* DAT_00463e4c (0-36); 37-75 = TD6, always unlocked */
uint8_t s_track_lock_table[37];  /* DAT_004668B0 (orig 26); 26-30 = migrated TD6 P2P slots */
int  s_total_unlocked_cars;      /* DAT_00463e0c */
int  s_total_unlocked_tracks;    /* DAT_00466840 */
int  s_cheat_unlock_all;         /* DAT_00496298 */

/* Network state */
int  s_network_active;           /* g_networkSessionActive / DAT_004962bc */
int  s_nickname_from_mpopts;     /* nickname screen entered from Multiplayer Options */
/* --- S10b: lobby options modal (host) + join-password prompt --- */
int  s_lobby_max_players = 6;    /* modal: max players (2..6) */
char s_lobby_password[32];       /* modal: host join password (also reused for join prompt) */
int  s_net_session_sel;          /* SESSION_PICKER cursor: 0=host, 1..N=join */
int  s_launching_net_race;       /* set by the lobby before init_race_schedule */
/* [Network] config (seeded from td5re.ini in frontend_init; see td5_save). */
int  s_net_cfg_game_port   = 37050;   /* [Network] GamePort */
int  s_kicked_flag;              /* DAT_00497328 */
int  s_lobby_action;             /* DAT_0049722c */
static int  s_chat_msg_count;           /* DAT_00496408 */
int  s_text_input_state;         /* DAT_004969d0 */

int  s_results_rerace_flag;      /* DAT_00497a78 */
int  s_results_cup_complete;     /* DAT_00497a70 */
/* P7 PANEL fix — sprite-rect slide offset for screen 24 results panel.
 * [CONFIRMED @ 0x00422480 cases 7..10 + 0xB] Original animates the panel
 * surface (DAT_0049628c) via QueueFrontendOverlayRect with a per-state
 * x-coordinate formula. Port has no MoveFrontendSpriteRect / sprite-rect
 * array; instead we accumulate a render-side x offset that overlays panel_x
 * in frontend_render_race_results_overlay. Reset to 0 on state 6 entry. */
int  s_results_panel_slide_x;
int  s_results_skip_display;     /* DAT_00497a74 */


/* Race snapshot for re-race */
int  s_snap_car, s_snap_paint, s_snap_trans, s_snap_config;

int     s_score_insert_pos;      /* 0-4: position in 5-entry table where insert goes */
static int  s_snap_opp_car, s_snap_opp_paint, s_snap_opp_trans, s_snap_opp_config;

/* Masters roster (type 5): 15 random car slots, 6 marked AI */
int  s_masters_roster[15];
int  s_masters_roster_flags[15]; /* 0=available, 1=AI, 2=taken */

/* Cup series schedule tables (from TD5_d3d.exe 0x464098, stride 0x10, sentinel 0x63=99).
 * Original formula @ 0x410E8E: entry = *(base + raceWithinSeries + gameType*0x10 + 0x14)
 * with base = 0x464084 → row0 = 0x464098. Rows are byte arrays; port widens to int.
 * Indexed here by (game_type - 1), so [0] = Championship (GT=1). */
const int s_cup_schedules[][13] = {
    /* [0] Championship (GT=1) @ 0x4640A8 */ {  4, 16,  6,  7,  5, 17, 99, -1,-1,-1,-1,-1,-1 },
    /* [1] Era          (GT=2) @ 0x4640B8 */ {  1,  2,  3, 15,  8, 99, -1,-1,-1,-1,-1,-1,-1 },
    /* [2] Challenge    (GT=3) @ 0x4640C8 */ {  1,  2,  3, 15,  8, 11, 13, 99, -1,-1,-1,-1,-1 },
    /* [3] Pitbull      (GT=4) @ 0x4640D8 */ {  1,  2,  3, 15,  8, 11, 13, 10, 12, 99, -1,-1,-1 },
    /* [4] Masters      (GT=5) @ 0x4640E8 */ {  1,  2,  3, 15,  8, 11, 13, 10, 12,  9, 14, 99, -1 },
    /* [5] Ultimate     (GT=6) @ 0x004640F8 [CONFIRMED 2026-06-01: real static row in
     *     g_cupDataXorKey @0x00464084, fetched by ConfigureGameTypeFlags @0x00410CA0 —
     *     NOT a callback. The prior "Masters placeholder" was MISSING track ids 9 and 14.
     *     Real row bytes: 00 01 02 03 0F 08 0B 0D 0A 0C 09 0E 63 = 12 tracks + 99 term. */
    {  0,  1,  2,  3, 15,  8, 11, 13, 10, 12,  9, 14, 99 },
};

/* Bar-fade transition lookup table (slot -> {r,g,b} bar color + type) */
typedef struct {
    uint8_t r, g, b;
    int     fade_type;  /* 0 = fade-to-black, 1 = fade-to-color, 2 = fade-to-image */
} BarFadeEntry;

static const BarFadeEntry s_bar_fade_table[] = {
    { 0x00, 0x00, 0x00, 0 },  /* default: fade to black */
    { 0x20, 0x20, 0x40, 1 },  /* blue tint bar sweep    */
    { 0x40, 0x10, 0x10, 1 },  /* red tint bar sweep     */
    { 0x00, 0x00, 0x00, 2 },  /* fade to image          */
};

/* Mouse state */
int  s_mouse_x, s_mouse_y;
static int  s_prev_mouse_x = -1;
static int  s_prev_mouse_y = -1;
int  s_mouse_clicked;
static int  s_prev_mouse_btn;
static int  s_prev_left_state;
static int  s_prev_right_state;
static int  s_prev_enter_state;
static int  s_prev_up_state;
static int  s_prev_down_state;
static int  s_prev_escape_state;
static int  s_mouse_click_latched;
static int  s_mouse_confirm_button = -1;
static int  s_mouse_hover_button  = -1;
static int  s_prev_mouse_hover_button = -1;
static uint32_t s_mouse_confirm_until;
static int  s_mouse_flash_button = -1;
static uint32_t s_mouse_flash_until;
static uint32_t s_mouse_act_until;   /* debounce horizon: absorb the 2nd click of a rapid double-click after a mouse activation (single-click mode) */
static int      s_dbl_last_button = -1;  /* last button a click landed on (for double-click detection) */
static uint32_t s_dbl_last_time   = 0;   /* timestamp (ms) of that click */

/* Fade state */
int      s_fade_active;
int      s_fade_progress;     /* 0..255 */
int      s_fade_direction;    /* 1 = in, -1 = out */
static int      s_fade_table_index;
static int  s_gallery_pic_index;
static int  s_gallery_pic_surface;
static int  s_gallery_visited_mask;
int  s_credit_mugshot_surf[K_CREDIT_MUGSHOT_COUNT]; /* dev photos, lazy-loaded (0=none) */
uint32_t s_credits_start_ms;  /* scroll-reel start timestamp */
int  s_language_bg_surface = 0;   /* LanguageScreen.tga 640x480 bg (ScreenLanguageSelect) */
int  s_language_flag_surface = 0; /* Language.tga 176x512 = 4 stacked 176x128 flag tiles */

/* Background gallery slideshow (LoadExtrasGalleryImageSurfaces / UpdateExtrasGalleryDisplay)
 * pic1-5.tga from Extras.zip cycle as a semi-transparent overlay during frontend navigation. */
BgGalImg s_bg_gallery[5];
int   s_bg_gal_loaded;
int   s_bg_gal_current;
int   s_bg_gal_blend;
float s_bg_gal_x, s_bg_gal_y;

int  s_control_options_surface;
/* [PORT REWORK 2026-06-05 / S15] s_sound_icon_surface (Controllers.tga SFX-mode
 * icon strip) removed along with the SFX Mode row on the sound-options screen. */
int  s_sound_volumebox_surface = 0;  /* VolumeBox.tga   (volume bar background) */
int  s_sound_volumefill_surface = 0; /* VolumeFill.tga  (volume bar fill)       */
/* [2026-06-16] s_joypad/joystick/keyboard_icon_surface removed: the device-icon
 * bitmaps (Joypad/Joystick/KeyboardIcon.png) were loaded by the binding screen
 * but never blitted (the device label is fully TTF). Loads + handles retired. */
int  s_car_preview_prev_surface;
int  s_car_preview_next_surface;

/* ---- Screen [14] ControlOptions revamp state (PORT ENHANCEMENT 2026-06) ---- */
/* Which player (0-based) the Control Options screen is currently configuring,
 * and the live selectable range (recomputed on entry from the connected device
 * count — "hot-swappable"). */
int      s_ctrl_opts_player      = 0;

/* ---- Screen [18] ControllerBinding persistent state ---- */
/* [CONFIRMED @ 0x40FE00 / DAT_004974b8]: which player is being configured */
int      s_ctrl_player        = 0;
/* [CONFIRMED @ 0x40FE00 / DAT_00490b94]: device source index (0=kbd/1=pad/2=stick) */
int      s_ctrl_input_source  = 0;
/* [PORT ENHANCEMENT 2026-06] per-button remap: which action row the Configure
 * screen has selected, and whether it is currently capturing that action. */
int      s_ctrl_sel_action    = 0;
int      s_ctrl_capturing     = 0;
/* Capture is two-phase: 0 = waiting for the device to return to NEUTRAL (so a
 * held input from the previous bind / the confirm press isn't re-captured), then
 * 1 = armed and listening for one fresh input. */
int      s_ctrl_capture_armed = 0;
/* [CONFIRMED @ 0x40FE00 / DAT_00464054]: 16-byte scancode capture buffer */
uint8_t  s_ctrl_kb_scancodes[16];
/* [PORT ENHANCEMENT 2026-06] per-action joystick binding being edited on the
 * Configure screen: 10 codes (button/axis/trigger) for the selected player. */
uint32_t s_ctrl_action_bind[TD5_MAX_HUMAN_PLAYERS][TD5_JSBIND_ACTIONS];

/* Action label strings for keyboard capture prompt (SNK_ControlText slots 0-9)
 * [CONFIRMED @ 0x40FE00 case 0x19]: index = s_ctrl_kb_slot (0..9), iterated
 *   via SNK_ControlText[i*0x10], loop stops at slot==10.
 * [CONFIRMED]: strings read from LANGUAGE.DLL SNK_ControlText @ 0x100075E0
 *   (stride 0x10, idx 0..9); default scancodes baked at g_keyboardScanCodeTable
 *   0x00464054 = {cb cd c8 d0 10 9d 1e 2c 14 2d}. Index 10 @ 0x10007680 = "NONE"
 *   is a sentinel, not iterated.
 * Faithful port of the original action list (was previously guessed/shifted —
 * had no "Steer"/"Gas"/"NOS"/"Camera", missed HANDBRAKE, and merged steer L/R). */
const char * const k_ctrl_action_labels[TD5_JSBIND_ACTIONS] = {
    "LEFT",         /* slot 0 — DIK_LEFT  0xCB */
    "RIGHT",        /* slot 1 — DIK_RIGHT 0xCD */
    "ACCELERATE",   /* slot 2 — DIK_UP    0xC8 */
    "BRAKE",        /* slot 3 — DIK_DOWN  0xD0 */
    "HANDBRAKE",    /* slot 4 — DIK_Q     0x10 */
    "HORN/SIREN",   /* slot 5 — DIK_RCTRL 0x9D */
    "GEAR UP",      /* slot 6 — DIK_A     0x1E */
    "GEAR DOWN",    /* slot 7 — DIK_Z     0x2C */
    "CHANGE VIEW",  /* slot 8 — DIK_T     0x14 */
    "REAR VIEW",    /* slot 9 — DIK_X     0x2D */
    "PAUSE",        /* slot 10 — DIK_P    0x19 [PORT ENHANCEMENT 2026-06] */
};

/* [PORT ENHANCEMENT 2026-06] The old k_js_value_labels[] table (binding values
 * 2..10 → "AXIS +/-" / "BUTTON 1..7") was retired with the sequential joystick
 * binding screen. The per-button remap now stores value = physical_button + 2
 * (the platform poll reads phys = value-2) and displays "BTN n" directly, which
 * matches what the user actually pressed. */

/* ========================================================================
 * Frontend Rendering Infrastructure
 *
 * Surface manager, button system, input polling, draw queues.
 * ======================================================================== */

/* --- Surface Manager --- */

typedef struct {
    char *buffer;
    int capacity;
    int caret;
    uint32_t blink_tick;
    int confirm_state;
} FE_TextInputContext;

static FE_TextInputContext s_text_input_ctx;
static char s_cheat_key_history[32];

FE_Surface s_surfaces[FE_MAX_SURFACES];
static uint32_t s_fe_load_seq = 0;   /* [PERF] background-cache LRU clock */
int s_white_tex_page = -1;
static int s_background_surface = 0;
static int s_carsel_bg_surface = 0;     /* unused — background inherited from RaceMenu.tga via s_background_surface */
int s_carsel_fill_surface = 0;  /* 1x1 solid blue pixel for car preview area fill */
int s_carsel_bar_surface = 0;
int s_carsel_curve_surface = 0;
int s_carsel_topbar_surface = 0;
int s_graphbars_surface = 0;
int s_car_preview_surface = 0;
/* Body-only paint overlay (carpicpaint) for TD6 cars — grayscale body, rest
 * transparent; drawn MODULATEd by the paint colour over the gray carpic.
 * Lazily (re)loaded per car in the render path. s_paint_active: the picked
 * colour persists on the preview (panel-open shows it live too); it's set on
 * CONFIRM (closing the panel) and cleared on car change / screen entry, so an
 * un-painted car shows neutral gray and a painted one keeps its colour through
 * the confirm slide and after. */
int s_paint_overlay_surface = 0;
int s_paint_overlay_car = -1;
int s_paint_active = 0;
static char s_car_spec[17][48]; /* config.nfo fields (0-16) for stats sub-screen */
int  s_car_spec_car = -1; /* which car index is currently cached */
int s_track_preview_surface = 0;
int s_track_switch_tick = 16; /* 0-15 = animating in, 16 = settled */

/* Track-preview start/finish dot markers, generated by
 * re/tools/track_preview_render.py into re/assets/tracks/trak_markers.json
 * (same projection as each trak%04d.png, so the dots line up). Indexed by pool
 * (== trak TGA number == s_track_schedule_to_tga_index[selected]). u,v are
 * normalized 0..1 in the 152x224 preview, top-left origin. circuit: LEVELINF
 * DWORD[0] (1=circuit -> single start/finish dot; 0=P2P -> start+end dots that
 * swap with the Forwards/Backwards toggle). */
typedef struct {
    float start_u, start_v;
    float end_u, end_v;
    uint8_t circuit;
} TD5_TrackMarker;
static TD5_TrackMarker s_track_markers[20];
static int s_track_markers_loaded = 0; /* 0=untried, 1=loaded, -1=unavailable */
/* Migrated TD6 tracks use preview TGA numbers >= TD6_PREVIEW_TGA_BASE (trak0090+),
 * outside the 20-entry pool array. Their start/finish markers live in a separate
 * file keyed by (tga - base). Loaded lazily, same projection as their preview PNG. */
#define TD6_PREVIEW_TGA_BASE 90
#define TD6_MARKER_MAX       16
static TD5_TrackMarker s_track_markers_td6[TD6_MARKER_MAX];
static int s_track_markers_td6_loaded = 0;
int s_font_page = -1;
/* ---- Vector (MSDF) BodyText font — resolution-independent frontend text ----
 * BodyText_msdf.png is generated offline by re/tools/build_msdf_font.py: the
 * SAME logical 10-col x 23-row grid as BodyText, each cell holding a 3-channel
 * signed distance field instead of a bitmap glyph. Because the grid matches,
 * the existing UV math (u0=col/10, v0=row/23, ...) is reused verbatim; only the
 * texture page + pixel shader + sampler differ. Drawn via ps_msdf (median+
 * smoothstep) with a LINEAR_CLAMP sampler so it stays crisp at any resolution.
 * Gated by [Frontend] VectorUI; -1/NULL => fall back to the bitmap path. */
int s_msdf_font_page = -1;
ID3D11PixelShader *s_ps_msdf = NULL;

/* In-race HUD font SDF (VectorUI): a distance-field version of the original
 * tpage5 FONT glyphs at the SAME 256x256 layout, so the HUD keeps its original
 * typeface but renders crisp at any resolution via ps_msdf. Generated offline
 * by re/tools/build_hud_font_sdf.py. -1 => HUD falls back to the bitmap font. */
int s_hudfont_sdf_page = -1;

/* Pause-menu font SDF (VectorUI): distance-field version of the tpage12
 * PAUSETXT glyphs at the same 256x256 layout. -1 => bitmap PAUSETXT fallback. */
int s_pausefont_sdf_page = -1;

/* Procedural neon rounded-rect (frontend buttons/frames) — VectorUI only.
 * ps_roundrect evaluates an analytic rounded-rect SDF per pixel (crisp glow at
 * any resolution); s_rr_cb feeds it per-button geometry/colour via constant
 * buffer register b1. Must match cbuffer RoundRectParams in ps_roundrect.hlsl. */
ID3D11PixelShader *s_ps_roundrect = NULL;
ID3D11PixelShader *s_ps_arrow = NULL;   /* selector ◄► triangle SDF */
ID3D11PixelShader *s_ps_cursor = NULL;  /* mouse pointer (SDF: white outline + purple fill) */
int s_cursor_msdf_page = -1;
ID3D11Buffer      *s_rr_cb = NULL;

/* Procedural analog gauge dial (in-race HUD speedo + tach) — VectorUI only.
 * ps_gauge evaluates an analytic dial SDF (face disc + outer ring + radial
 * ticks + optional redline arc + pivot) per pixel; s_gauge_cb feeds geometry
 * + colours via constant-buffer register b1. Owned here (with the other
 * VectorUI shaders) and exposed to the HUD via td5_vui_gauge (td5_vectorui.h).
 * Must match cbuffer GaugeParams in ps_gauge.hlsl. */
ID3D11PixelShader *s_ps_gauge = NULL;
ID3D11Buffer      *s_gauge_cb = NULL;
/* When set, fe_draw_text preserves the input case instead of forcing upper-case.
 * BodyText.tga DOES contain lowercase glyphs (rows 6-9, ascii 0x61+), so mixed-case
 * strings (e.g. high-score player names "Frank"/"Jeffrey") render correctly. Scoped:
 * callers set it just around a draw and clear it after. */
static int s_fe_preserve_case = 0;

/* ---- Small font (smalltext.tga) — the original's high-score / results table font ----
 * 12x12 cells, 21 columns; proportional advance + per-char vertical offset (descenders).
 * [CONFIRMED @ DrawFrontendSmallFontStringToSurface 0x00424660: col=(c-0x20)%21,
 *  row=(c-0x20)/21, 12px cells; dest_y += g_smallFontYOffset[c]; advance g_smallFontAdvance[c]].
 * Tables baked byte-exact from the original (0x004662d0 advance / 0x004663e4 yoffset).
 * Unlike the button font (BodyText), this has true lowercase incl. a normal 'y'. */
int s_smallfont_page = -1;
/* Vector (SDF) SmallText atlas — same 21x11 grid, distance field per cell.
 * Generated by re/tools/build_msdf_font.py (smalltext_white_on_black.png).
 * Reuses the same ps_msdf shader + s_ps_msdf as BodyText. */
int s_smallfont_msdf_page = -1;
#define SMALLFONT_CELL   12
#define SMALLFONT_COLS   21
#define SMALLFONT_TEX_W  252.0f   /* 21 * 12 */
#define SMALLFONT_TEX_H  132.0f   /* 11 * 12 */

/* [SMALL-FONT TTF SWAP 2026-06-05] When the native menu TTF (menu.ttf — the SAME
 * face fe_draw_text/buttons render with) is loaded, the small font is rasterised
 * straight from its outlines at a smaller cap size instead of the smalltext.tga
 * bitmap / smalltext_msdf SDF, so EVERY frontend string shares one typeface and
 * stays crisp at any resolution. Sizes are in 12px-cell DESIGN px (the unscaled
 * 320x240 canvas) so the fe_measure_small_text contract is preserved exactly:
 * measure returns design px, callers multiply by sx, and the draw advances the pen
 * by design_advance*sx (vertical at sy) — the same stretch model the bitmap path
 * used (cell_w=SMALLFONT_CELL*sx, cell_h=*sy). Tunable visual constants (no RE
 * basis; this is a port-only asset swap, like the S13 main-font swap):
 *   CAP      = cap height in design px (smalltext caps were ~9px in a 12px cell).
 *   BASELINE = design px from the cell top (the `y` arg) down to the baseline.
 *   TRACK    = extra inter-glyph tracking (design px; 0 = font's natural advances). */
#define SMALLFONT_TTF_CAP       9.0f
#define SMALLFONT_TTF_BASELINE  10.0f
#define SMALLFONT_TTF_TRACK     0.0f
/* indexed by (char-0x20), char 0x20..0x7F */
static const uint8_t k_smallfont_advance[96] = {
    5, 5, 7, 8, 8, 13, 11, 5, 4, 5, 6, 8, 6, 6, 4, 8,
    10, 6, 9, 9, 9, 9, 10, 10, 10, 9, 4, 6, 6, 8, 6, 8,
    10, 10, 9, 8, 10, 7, 7, 11, 9, 5, 6, 9, 7, 13, 10, 11,
    9, 11, 9, 8, 8, 9, 11, 13, 10, 10, 9, 5, 4, 5, 7, 7,
    6, 9, 9, 6, 8, 8, 6, 9, 8, 4, 4, 8, 4, 11, 9, 9,
    8, 8, 7, 6, 5, 8, 9, 13, 9, 8, 7, 5, 4, 6, 10, 12
};
static const int8_t k_smallfont_yoffset[96] = {
    0, 0, 0, 0, 2, 0, 0, 0, 2, 2, -2, 0, 3, -3, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 2, 0, -1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 3, 0, 0, 0, 0, 0,
    3, 3, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, -3, 0
};

int s_cursor_tex_page = -1;  /* page 896: snkmouse.tga cursor */
int s_cursor_w = 0, s_cursor_h = 0;
int s_buttonbits_tex_page = -1; /* page 897: ButtonBits.tga gradient */
int s_buttonbits_w = 0, s_buttonbits_h = 0;
int s_buttonlights_tex_page = -1; /* page 895: ButtonLights.tga indicator */
int s_buttonlights_w = 0, s_buttonlights_h = 0;
/* [2026-06-16] The baked per-screen title-strip art (s_title_tex_page[] +
 * s_title_msdf_page[]) was retired — screen headers render via the title.ttf
 * Lunatica face (frontend_draw_screen_title + frontend_get_title_text_for_screen). */

/* Forward declarations for functions used before their definitions */
 int frontend_load_tga(const char *name, const char *archive);
 int frontend_load_tga_colorkey(const char *name, const char *archive,
                                      int page_override, int *w_out, int *h_out,
                                      TD5_ColorKeyMode colorkey);
 int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey);
 int frontend_track_level_exists(int track_index);
 void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label);
 int  frontend_track_is_circuit(int track_slot);
 void frontend_update_laps_button_visibility(int laps_btn_idx);
static uint8_t s_font_glyph_advance[96];
/* [S13 FONT SWAP 2026-06-05] Per-glyph horizontal advances (px in the 24px cell).
 * The original table came from the binary @0x4660C8 (the old BodyText face). The
 * main-menu font was swapped to MontBlanc Black (heavy geometric sans;
 * tools/font/MontBlancTrialBlack.ttf), rendered with a tight left bearing for
 * close tracking and its baseline placed so caps land at cell rows ~8..23 —
 * vertically centered in the 32px button, matching the original art's placement.
 * These advances are the rightmost-inked-column + 1 + FONT_GLYPH_PADDING measured
 * off the regenerated re/assets/frontend/BodyText.png (see re/tools/
 * build_bodytext_bitmap.py). It is a TRIAL font that watermarks the symbol glyphs
 * it withholds ("# $ % & ( ) [ ] ... ~), so the tool detects those (and the tilde)
 * and sources them from the original atlas; letters + digits + . , - : ; ! ? stay
 * MontBlanc. NOTE: at runtime this is only the FALLBACK — the
 * real advances are re-measured live from the loaded BodyText.png pixels by
 * frontend_init_font_metrics_from_pixels (so the bitmap page, the MSDF page, and
 * the baked button cache all share one consistent set of letterforms+metrics).
 * No RE basis for the swap itself: it is a data/asset change, not a behavioural
 * port (the original ran at a fixed 4:3 resolution with this one font). */
static const uint8_t k_font_glyph_advance_default[96] = {
    8,  9,  9, 13, 13, 24, 20,  8,  7,  8,  9, 13,
    9, 13,  9, 12, 18, 12, 16, 16, 17, 16, 17, 15,
   17, 17,  9,  9, 11, 12, 10, 15, 17, 20, 18, 20,
   19, 15, 15, 20, 18, 10, 16, 18, 15, 24, 19, 21,
   17, 21, 18, 17, 17, 18, 20, 24, 19, 18, 16,  8,
   12,  8, 11, 14,  8, 16, 17, 17, 17, 17, 13, 17,
   16,  9, 10, 17,  9, 23, 16, 17, 17, 17, 13, 15,
   13, 16, 17, 22, 16, 17, 15,  8,  5,  9, 15,  8,
};

/* [S13 4:3-LOCKED GLYPH SCALE 2026-06-05] Horizontal glyph scale that keeps the
 * font's 4:3-authored letterform shape under a freely-resizable window. The 2D
 * frontend is laid out at a 640x480 virtual canvas; button RECTS scale by the
 * caller's (sx,sy)=(w/640,h/480) and may freely grow/stretch with the window,
 * but TEXT must never stretch. Returns min(sx,sy):
 *   - window at/above 4:3 (sx>=sy): use sy -> uniform scale, glyph keeps its 4:3
 *     proportion (no horizontal stretch) while buttons grow wider with sx;
 *   - window narrower than 4:3 (sx<sy): use sx -> glyph shrinks horizontally with
 *     the buttons, while the vertical size (cell_h, kept at sy) holds.
 * Vertical glyph scale always stays sy. No RE basis (original was fixed 4:3). */
static inline float fe_glyph_sx(float sx, float sy) { return sx < sy ? sx : sy; }

/* Original font: BodyText.tga, 10 chars per row, 24x24 cells (from Ghidra FUN_00424560)
 * Atlas is 240x552 (8bpp with palette, red color key for transparency).
 * DAT_0049626c. */
#define FONT_GLYPH_PADDING 1   /* [S13] inter-glyph gap (px) added after each glyph's
                                * rightmost ink; 1 = tight tracking for MontBlanc Black
                                * (was 2). Glyphs render with 0 left bearing, so the
                                * on-screen gap is exactly this value. */
#define FONT_GLYPH_TRACKING (0)  /* [S13] extra letter-tracking (px, 24-unit) applied to
                                * the CURSOR STEP only (glyph_w stays full, so nothing is
                                * cropped). Negative = tighter, positive = looser. 0 = the
                                * font's natural advances (user walked the spacing back from
                                * an earlier tight -2). Applied in fe_draw_text + the measure
                                * helpers so centering stays consistent. */

/* Dedicated texture pages for shared assets (never freed on screen change):
 * 899 = white fallback
 * 898 = BodyText.tga (font)
 * 897 = ButtonBits.tga (gradient source)
 * 896 = SnkMouse.TGA (cursor)
 * 931-980 = FREE (was cached per-screen title TGAs + parallel SDF title pages;
 *           title strips retired 2026-06-16, headers render via title.ttf)
 */

static void frontend_note_activity(void) {
    s_attract_idle_counter = 0;
    s_attract_idle_timestamp = td5_plat_time_ms();
}

static int frontend_surface_is_background_like(int w, int h) {
    return (w >= 640 || (w >= 320 && h >= 400));
}

int frontend_find_surface_by_source(const char *name, const char *archive) {
    int i;
    if (!name || !archive) return 0;
    for (i = 0; i < FE_MAX_SURFACES; i++) {
        if (!s_surfaces[i].in_use) continue;
        if (strcmp(s_surfaces[i].source_name, name) != 0) continue;
        if (strcmp(s_surfaces[i].source_archive, archive) != 0) continue;
        return i + 1;
    }
    return 0;
}

/* UI order matches original binary table at 0x00463e24 for 0-36 (DO NOT REORDER).
 * 37-75 = ported Test Drive 6 cars (must match td5_asset.c s_car_zip_paths order). */
const char *s_car_zip_paths[TD5_CAR_COUNT] = {
    "cars/vip.zip",  /* 0  - VIPER            - unlocked */
    "cars/97c.zip",  /* 1  - '97 CAMARO       - unlocked */
    "cars/frd.zip",  /* 2  - SALEEN MUSTANG   - unlocked */
    "cars/vet.zip",  /* 3  - '98 CORVETTE     - unlocked */
    "cars/sky.zip",  /* 4  - SKYLINE          - unlocked */
    "cars/tvr.zip",  /* 5  - CERBERA          - unlocked */
    "cars/van.zip",  /* 6  - '98 VANTAGE      - unlocked */
    "cars/xkr.zip",  /* 7  - XKR              - unlocked */
    "cars/gto.zip",  /* 8  - GTO              - unlocked */
    "cars/crg.zip",  /* 9  - '69 CHARGER      - unlocked */
    "cars/chv.zip",  /* 10 - '70 CHEVELLE     - unlocked */
    "cars/cud.zip",  /* 11 - CUDA             - unlocked */
    "cars/cob.zip",  /* 12 - COBRA            - unlocked */
    "cars/69v.zip",  /* 13 - '69 CORVETTE     - unlocked */
    "cars/cam.zip",  /* 14 - '69 CAMARO       - unlocked */
    "cars/mus.zip",  /* 15 - '68 MUSTANG      - unlocked */
    "cars/atp.zip",  /* 16 - P.VANTAGE        - locked   */
    "cars/ss1.zip",  /* 17 - SERIES 1         - locked   */
    "cars/128.zip",  /* 18 - SPEED 12         - locked   */
    "cars/gtr.zip",  /* 19 - GTS-R            - locked   */
    "cars/jag.zip",  /* 20 - XJ220            - locked   */
    "cars/cat.zip",  /* 21 - SUPER 7          - locked   */
    "cars/sp4.zip",  /* 22 - R390             - locked   */
    "cars/c21.zip",  /* 23 - CAT 21           - locked   */
    "cars/day.zip",  /* 24 - DAYTONA          - locked   */
    "cars/fhm.zip",  /* 25 - '68 MUSTANG HR   - locked   */
    "cars/hot.zip",  /* 26 - '69 CAMARO HR    - locked   */
    "cars/sp3.zip",  /* 27 - '98 MUSTANG GT   - locked   */
    "cars/nis.zip",  /* 28 - HOT DOG          - locked   */
    "cars/sp1.zip",  /* 29 - MAUL             - locked   */
    "cars/sp8.zip",  /* 30 - PITBULL          - locked   */
    "cars/pit.zip",  /* 31 - BEAST            - locked   */
    "cars/sp2.zip",  /* 32 - WAGON            - locked   */
    "cars/cop.zip",  /* 33 - POLICE CERBERA   - locked   */
    "cars/sp5.zip",  /* 34 - POLICE MUSTANG   - locked   */
    "cars/sp6.zip",  /* 35 - POLICE CHARGER   - locked   */
    "cars/sp7.zip",  /* 36 - POLICE CAMARO    - locked   */
    /* --- Test Drive 6 cars (ported; indices 37-75, always unlocked) --- */
    "cars/390.zip", "cars/400.zip", "cars/atl.zip", "cars/att.zip", "cars/aud.zip",
    "cars/bmw.zip", "cars/cer.zip", "cars/chd.zip", "cars/chr.zip", "cars/cp1.zip",
    "cars/cp2.zip", "cars/cp3.zip", "cars/cp4.zip", "cars/db7.zip", "cars/eli.zip",
    "cars/esp.zip", "cars/flx.zip", "cars/g40.zip", "cars/grf.zip", "cars/gts.zip",
    "cars/lgt.zip", "cars/lit.zip", "cars/lot.zip", "cars/mam.zip", "cars/mcj.zip",
    "cars/mcl.zip", "cars/mgt.zip", "cars/pan.zip", "cars/pro.zip", "cars/pwr.zip",
    "cars/s12.zip", "cars/shl.zip", "cars/sub.zip", "cars/sup.zip", "cars/toy.zip",
    "cars/tur.zip", "cars/tus.zip", "cars/xjr.zip", "cars/xk1.zip",
};

static const char *s_track_display_names[37] = {
    "DRAG STRIP",
    "MONTEGO BAY, JAMAICA",
    "HOUSE OF BEZ, ENGLAND",
    "NEWCASTLE, ENGLAND",
    "MAUI, HAWAII, USA",
    "COURMAYEUR, ITALY",
    "JARASH, JORDAN",
    "CHEDDAR CHEESE, ENGLAND",
    "MOSCOW, RUSSIA",
    "BLUE RIDGE PARKWAY, NC, USA",
    "EDINBURGH, SCOTLAND",
    "TOKYO, JAPAN",
    "SYDNEY, AUSTRALIA",
    "HONOLULU, HAWAII, USA",
    "MUNICH, GERMANY",
    "WASHINGTON, DC, USA",
    "KYOTO, JAPAN",
    "BERN, SWITZERLAND",
    "SAN FRANCISCO, CA, USA",
    "KESWICK, ENGLAND",
    /* 20-25: the six championship CUPS (High Scores cat 0x14-0x19, resolved by
     * direct frontend_get_track_name index) -- kept from master's S02 work. */
    "CHAMPIONSHIP CUP",   /* 20 */
    "ERA CUP",            /* 21 */
    "CHALLENGE CUP",      /* 22 */
    "PITBULL CUP",        /* 23 */
    "MASTERS CUP",        /* 24 */
    "ULTIMATE CUP",       /* 25 */
    /* 26-36: migrated TD6 tracks, RELOCATED after the cups (circuits 26-31,
     * P2P 32-36) so their schedule slots do not collide with the cup categories
     * at 20-25. k_td6_menu_slots uses the matching 26-36 slots. */
    "PELTON RACEWAY",      /* 26: TD6 (circuit) */
    "IRELAND",             /* 27: TD6 (circuit) */
    "LAKE TAHOE, USA",     /* 28: TD6 (circuit) */
    "CAPE HATTERAS, USA",  /* 29: TD6 (circuit) */
    "SWITZERLAND",         /* 30: TD6 (circuit) */
    "EGYPT",               /* 31: TD6 (circuit) */
    "PARIS, FRANCE",       /* 32: TD6 (P2P) */
    "NEW YORK, USA",       /* 33: TD6 (P2P) */
    "ROME, ITALY",         /* 34: TD6 (P2P) */
    "HONG KONG, CHINA",    /* 35: TD6 (P2P) */
    "LONDON, ENGLAND"      /* 36: TD6 (P2P) */
};
/* Original binary order: DAT_00466894 (slot→SNK_TrackNames index) cross-referenced
 * with Language.dll SNK_TrackNames → city name → s_track_display_names index.
 * Unlocked slots 0-7: Moscow, Edinburgh, Sydney, Blue Ridge, Jarash, Newcastle,
 *                     Maui, Courmayeur.
 * Locked slots 8-15: Honolulu, Tokyo, Keswick, San Francisco, Bern, Kyoto,
 *                    Washington, Munich.
 * Championship-only slots 16-19: Cheddar Cheese, Montego Bay, House of Bez,
 *                                Drag Strip. */
static const uint8_t s_track_schedule_to_name_index[37] = {
     8, 10, 12,  9,  6,  3,  4,  5, 13, 11,
    19, 18, 17, 16, 15, 14,  7,  1,  2,  0,
    20, 21, 22, 23, 24, 25,   /* slots 20-25: cups (identity) */
    26, 27, 28, 29, 30, 31,   /* slots 26-31: TD6 circuits */
    32, 33, 34, 35, 36        /* slots 32-36: TD6 P2P cities */
};
/* Original binary gScheduleToPoolIndex (DAT_00466894): maps schedule slot →
 * Language.dll pool index, which equals the trak TGA file number.
 * Derived from listing at 0x466894 in TD5_d3d.exe. */
const uint8_t s_track_schedule_to_tga_index[37] = {
    11,  9,  7, 10, 13, 16, 15, 14,  6,  8,
     0,  1,  2,  3,  4,  5, 12, 18, 17, 19,
    20, 21, 22, 23, 24, 25,   /* slots 20-25: cups -> trak0020..0025 (master direct) */
    90, 91, 92, 93, 94, 95,   /* slots 26-31: TD6 circuits -> trak0090..0095 */
    96, 97, 98, 99,100        /* slots 32-36: TD6 P2P cities -> trak0096..0100 */
};

static char s_car_display_names[TD5_CAR_COUNT][64];
static uint8_t s_car_display_names_loaded[TD5_CAR_COUNT];

static float frontend_clamp01(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

void frontend_begin_timed_animation(void) {
    s_anim_start_ms = td5_plat_time_ms();
    s_anim_elapsed_ms = s_anim_start_ms - s_screen_entry_timestamp;
    s_anim_tick = 0;
    s_anim_t = 0.0f;
}

float frontend_update_timed_animation(int max_tick, uint32_t duration_ms) {
    uint32_t now = td5_plat_time_ms();
    float t;

    s_anim_elapsed_ms = now - s_screen_entry_timestamp;
    if (s_anim_start_ms == 0) s_anim_start_ms = now;

    if (duration_ms == 0) {
        s_anim_tick = max_tick;
        return 1.0f;
    }

    t = frontend_clamp01((float)(now - s_anim_start_ms) * 2.0f / (float)duration_ms);
    s_anim_tick = (int)(t * (float)max_tick + 0.5f);
    if (s_anim_tick > max_tick) s_anim_tick = max_tick;
    s_anim_t = t;
    return t;
}

/* Human-readable header text drawn at the top of each screen in the Lunatica
 * title face (td5_titlefont). NULL means the screen has no header. Rendered
 * uppercase via the title-draw helper. [2026-06-16] This is the sole title
 * source now — the legacy baked title-strip art (*Text.TGA) was retired. */
static const char *frontend_get_title_text_for_screen(TD5_ScreenIndex screen) {
    switch (screen) {
    case TD5_SCREEN_MAIN_MENU:          return "MAIN MENU";
    /* Race-type menu doubles as the cup chooser: its inner states 6..10 are the
     * championship "cup sub-menu" (Championship/Era/Challenge/...), where the
     * header reads SELECT CUP; the top-level race-type list reads RACE TYPE. */
    case TD5_SCREEN_RACE_TYPE_MENU:
        return (s_inner_state >= 6 && s_inner_state <= 10) ? "SELECT CUP" : "RACE TYPE";
    case TD5_SCREEN_QUICK_RACE:         return "QUICK RACE";
    case TD5_SCREEN_OPTIONS_HUB:
    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
    case TD5_SCREEN_CONTROLLER_BINDING: return "OPTIONS";
    case TD5_SCREEN_CAR_SELECTION:      return "SELECT CAR";
    case TD5_SCREEN_TRACK_SELECTION:    return "SELECT TRACK";
    case TD5_SCREEN_HIGH_SCORE:
    case TD5_SCREEN_NAME_ENTRY:         return "HIGH SCORES";
    case TD5_SCREEN_RACE_RESULTS:       return "RESULTS";
    case TD5_SCREEN_MP_LOBBY:           return "MULTIPLAYER";
    /* All net-play frontends share one header: NET PLAY. */
    case TD5_SCREEN_CONNECTION_BROWSER:
    case TD5_SCREEN_SESSION_PICKER:
    case TD5_SCREEN_CREATE_SESSION:
    case TD5_SCREEN_NETWORK_LOBBY:
    case TD5_SCREEN_SESSION_LOCKED:
    case TD5_SCREEN_LAN_MENU:
    case TD5_SCREEN_DIRECT_CONNECT:
    case TD5_SCREEN_NET_NICKNAME:       return "NET PLAY";
    default: return NULL;
    }
}



/* TD6 body-only paint overlay (carpicpaint0.png): grayscale body, everything
 * else transparent (alpha) — the PNG alpha is the mask. Drawn MODULATEd by the
 * paint colour OVER the gray base carpic to tint just the body.
 *
 * 🟥 The source mask is anti-aliased: ~6% of its texels carry PARTIAL alpha at
 * the body silhouette, and it ships at HALF the base carpic's resolution
 * (816x560 vs 1632x1120). Drawn over the gray base, those semi-transparent edge
 * texels let the unpainted gray body bleed through — barely visible static, but
 * obvious during the car-change SLIDE: the two mismatched-resolution layers are
 * linear-filtered at sub-pixel positions, so the gray base "ghosts" behind the
 * coloured paint. Fix: BINARISE the mask alpha at load (any body texel -> fully
 * opaque) so the coloured body completely covers the gray base; only an ignorable
 * sub-pixel rim against the background can remain. Decoded/uploaded directly
 * (not via frontend_load_surface_keyed) so the threshold runs before upload.
 * NO RE BASIS — the TD6 colour-overlay preview is a port-only feature.
 *
 * Returns <=0 if absent (a TD5 car, or a TD6 car whose preview predates
 * carpicpaint generation). */
int frontend_load_car_paint_overlay_surface(int car_index) {
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return 0;
    const char *archive = s_car_zip_paths[car_index];

    int existing = frontend_find_surface_by_source("CarPicPaint0.tga", archive);
    if (existing > 0) return existing;  /* already loaded + binarised */

    char png_path[256];
    if (!td5_asset_resolve_png_path("CarPicPaint0.tga", archive, png_path, sizeof(png_path)))
        return 0;
    void *pixels = NULL; int w = 0, h = 0;
    if (!td5_asset_load_png_to_buffer(png_path, TD5_COLORKEY_NONE, &pixels, &w, &h))
        return 0;

    /* Binarise the BGRA32 mask alpha: any non-zero body texel -> fully opaque. */
    unsigned char *p = (unsigned char *)pixels;
    for (int i = 0; i < w * h; i++)
        if (p[i * 4 + 3] != 0) p[i * 4 + 3] = 255;

    int slot = -1;
    for (int i = 0; i < FE_MAX_SURFACES; i++) if (!s_surfaces[i].in_use) { slot = i; break; }
    if (slot < 0) { free(pixels); return 0; }

    int page = FE_SURFACE_PAGE_BASE + slot;
    if (!td5_plat_render_upload_texture(page, pixels, w, h, 2)) { free(pixels); return 0; }
    s_surfaces[slot].in_use = 1;
    s_surfaces[slot].tex_page = page;
    s_surfaces[slot].width = w;
    s_surfaces[slot].height = h;
    strncpy(s_surfaces[slot].source_name, "CarPicPaint0.tga", sizeof(s_surfaces[slot].source_name) - 1);
    s_surfaces[slot].source_name[sizeof(s_surfaces[slot].source_name) - 1] = '\0';
    strncpy(s_surfaces[slot].source_archive, archive, sizeof(s_surfaces[slot].source_archive) - 1);
    s_surfaces[slot].source_archive[sizeof(s_surfaces[slot].source_archive) - 1] = '\0';
    strncpy(s_surfaces[slot].png_path, png_path, sizeof(s_surfaces[slot].png_path) - 1);
    s_surfaces[slot].png_path[sizeof(s_surfaces[slot].png_path) - 1] = '\0';
    free(pixels);
    TD5_LOG_I(LOG_TAG, "paint overlay loaded (alpha binarised): car=%d slot=%d page=%d %dx%d",
              car_index, slot, page, w, h);
    return slot + 1;
}

/* Draw a surface OPAQUE: all pixels (including black) rendered as-is, no color key.
 * Matches original game's BltFast without SRCCOLORKEY (Copy16BitSurfaceRect flag 0x10).
 * Used for CarSelBar1 (0% black pixels — fully opaque solid bar). */
static void fe_draw_surface_opaque(int handle, float x, float y, float w, float h, uint32_t color) {
    int slot = handle - 1;
    if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) return;
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    fe_draw_quad(x, y, w, h, color, s_surfaces[slot].tex_page, 0, 0, 1, 1);
}

static void fe_draw_surface_rect(int handle, float x, float y, float w, float h, uint32_t color) {
    int slot = handle - 1;
    if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    /* Force blend state and alpha test CB directly, bypassing cache.
     * The state cache may believe the blend is already SRCALPHA_INVSRC from a
     * prior frame and skip OMSetBlendState. Force-set both to guarantee
     * correct transparency for every color-keyed surface draw. */
    if (g_backend.context && g_backend.blend_states[BLEND_SRCALPHA_INVSRC]) {
        ID3D11DeviceContext_OMSetBlendState(g_backend.context,
            g_backend.blend_states[BLEND_SRCALPHA_INVSRC], NULL, 0xFFFFFFFF);
        g_backend.state.current_blend_idx = BLEND_SRCALPHA_INVSRC;
    }
    Backend_UpdateFogCB();
    fe_draw_quad(x, y, w, h, color, s_surfaces[slot].tex_page, 0, 0, 1, 1);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* Auto-slot TGA/PNG loader with explicit colorkey. frontend_load_tga() wraps this with
 * TD5_COLORKEY_NONE (the default for full-frame backgrounds). Sprite-sheet ICONS that
 * sit on a black background (Controllers.tga, SplitScreen.tga) must pass TD5_COLORKEY_BLACK
 * so the black is keyed transparent — the original sets DDCKEY_SRCBLT on these surfaces
 * (LoadFrontendTgaSurfaceFromArchive @0x412030). */
int frontend_load_tga_ck(const char *name, const char *archive, TD5_ColorKeyMode colorkey) {
    int existing_handle;

    /* Strip path prefix from entry name — the ZIP stores bare filenames */
    const char *bare_name = name;
    const char *slash = strrchr(name, '/');
    if (slash) bare_name = slash + 1;
    slash = strrchr(bare_name, '\\');
    if (slash) bare_name = slash + 1;

    /* Fix archive path: original uses "Front_End/FrontEnd.zip" but
     * actual path is "Front End/frontend.zip" */
    const char *real_archive = archive;
    if (strstr(archive, "FrontEnd.zip") || strstr(archive, "frontend.zip"))
        real_archive = "Front End/frontend.zip";

    existing_handle = frontend_find_surface_by_source(bare_name, real_archive);
    if (existing_handle > 0) {
        int slot = existing_handle - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES) {
            s_surfaces[slot].load_seq = ++s_fe_load_seq;   /* [PERF] cache hit -> mark fresh */
            if (frontend_surface_is_background_like(s_surfaces[slot].width, s_surfaces[slot].height))
                s_background_surface = existing_handle;
        }
        return existing_handle;
    }

    /* Try PNG from re/assets first, fall back to ZIP+TGA */
    void *pixels = NULL;
    int w = 0, h = 0;
    char png_path[256];

    if (td5_asset_resolve_png_path(bare_name, real_archive, png_path, sizeof(png_path))) {
        if (!td5_asset_load_png_to_buffer(png_path, colorkey, &pixels, &w, &h))
            pixels = NULL;
    }

    if (!pixels) {
        TD5_LOG_W(LOG_TAG, "LoadTGA failed: %s from %s (no PNG found)", name, archive);
        return 0;
    }

    /* Find free surface slot */
    int slot = -1;
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (!s_surfaces[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        /* [PERF] No free slot: evict the OLDEST cached background (lowest
         * load_seq), never the current bg or a shared page. The set_screen sweep
         * now KEEPS backgrounds across transitions (so revisiting a screen reuses
         * its decoded surface instead of re-decoding ~4ms each); this bounds that
         * cache to the slot pool and guarantees a load never fails for a slot. */
        uint32_t best = 0xFFFFFFFFu;
        int cur_bg = s_background_surface - 1;
        for (int i = 0; i < FE_MAX_SURFACES; i++) {
            if (!s_surfaces[i].in_use || i == cur_bg) continue;
            if (s_surfaces[i].tex_page < FE_SURFACE_PAGE_BASE) continue;  /* shared */
            if (!frontend_surface_is_background_like(s_surfaces[i].width, s_surfaces[i].height)) continue;
            if (s_surfaces[i].load_seq < best) { best = s_surfaces[i].load_seq; slot = i; }
        }
        if (slot >= 0) s_surfaces[slot].in_use = 0;   /* evict LRU background */
    }
    if (slot < 0) { free(pixels); return 0; }

    int page = FE_SURFACE_PAGE_BASE + slot;
    if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
        int is_background = frontend_surface_is_background_like(w, h);
        s_surfaces[slot].in_use = 1;
        s_surfaces[slot].load_seq = ++s_fe_load_seq;   /* [PERF] background-cache LRU */
        s_surfaces[slot].tex_page = page;
        s_surfaces[slot].width = w;
        s_surfaces[slot].height = h;
        strncpy(s_surfaces[slot].source_name, bare_name, sizeof(s_surfaces[slot].source_name) - 1);
        s_surfaces[slot].source_name[sizeof(s_surfaces[slot].source_name) - 1] = '\0';
        strncpy(s_surfaces[slot].source_archive, real_archive, sizeof(s_surfaces[slot].source_archive) - 1);
        s_surfaces[slot].source_archive[sizeof(s_surfaces[slot].source_archive) - 1] = '\0';
        strncpy(s_surfaces[slot].png_path, png_path, sizeof(s_surfaces[slot].png_path) - 1);
        s_surfaces[slot].png_path[sizeof(s_surfaces[slot].png_path) - 1] = '\0';
        free(pixels);
        int handle = slot + 1;
        /* Large images (>=640 wide or 640x480) are backgrounds — auto-set as current bg.
         * Narrower overlays (CarSelBar1, CarSelCurve, etc.) must NOT hijack the bg. */
        if (is_background) {
            int old_slot = s_background_surface - 1;
            if (old_slot >= 0 && old_slot < FE_MAX_SURFACES &&
                old_slot != slot &&
                s_surfaces[old_slot].tex_page < SHARED_PAGE_MIN) {
                s_surfaces[old_slot].in_use = 0;
            }
            s_background_surface = handle;
        }
        TD5_LOG_I(LOG_TAG, "LoadTGA OK: %s → slot=%d page=%d %dx%d", bare_name, slot, page, w, h);
        return handle;
    }

    free(pixels);
    return 0;
}

/* Default loader: no colorkey (full-frame backgrounds). */
int frontend_load_tga(const char *name, const char *archive) {
    return frontend_load_tga_ck(name, archive, TD5_COLORKEY_NONE);
}

/**
 * Load a TGA to a dedicated texture page with red color key transparency.
 * Red pixels (R=255,G=0,B=0 in source; after BGRA swap: B=0,G=0,R=255,A=255)
 * become fully transparent (alpha=0).
 */
int frontend_load_tga_colorkey(const char *name, const char *archive,
                                       int dest_page, int *out_w, int *out_h,
                                       TD5_ColorKeyMode colorkey) {
    const char *bare_name = name;
    const char *slash = strrchr(name, '/');
    if (slash) bare_name = slash + 1;
    slash = strrchr(bare_name, '\\');
    if (slash) bare_name = slash + 1;

    const char *real_archive = archive;
    if (strstr(archive, "FrontEnd.zip") || strstr(archive, "frontend.zip"))
        real_archive = "Front End/frontend.zip";

    /* Try PNG from re/assets first */
    void *pixels = NULL;
    int w = 0, h = 0;
    char png_path[256];
    int from_png = 0;

    if (td5_asset_resolve_png_path(bare_name, real_archive, png_path, sizeof(png_path))) {
        if (td5_asset_load_png_to_buffer(png_path, colorkey, &pixels, &w, &h)) {
            from_png = 1;
            /* Font pages also need explicit near-black alpha keying: the white-on-black
             * glyph atlases (BodyText + smalltext) must have their black cell backgrounds
             * made transparent, else every glyph renders inside an opaque black box. */
            if (dest_page == SHARED_PAGE_FONT || dest_page == SMALLFONT_PAGE) {
                uint8_t *p = (uint8_t *)pixels;
                for (int ci = 0; ci < w * h; ci++, p += 4) {
                    if (p[0] < 8 && p[1] < 8 && p[2] < 8)
                        p[3] = 0;
                }
            }
        }
    }

    if (!pixels) {
        TD5_LOG_W(LOG_TAG, "LoadTGA_CK failed: %s from %s (no PNG found)", name, archive);
        return 0;
    }

    if (td5_plat_render_upload_texture(dest_page, pixels, w, h, 2)) {
        if (dest_page == SHARED_PAGE_FONT) {
            frontend_init_font_metrics_from_pixels((const uint8_t *)pixels, w, h);
        }
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        TD5_LOG_I(LOG_TAG, "LoadTGA_CK OK: %s -> page=%d %dx%d%s ck=%d",
                  bare_name, dest_page, w, h, from_png ? " (PNG)" : "", (int)colorkey);
        free(pixels);
        return 1;
    }

    free(pixels);
    return 0;
}


void frontend_release_surface(int handle) {
    int slot = handle - 1;
    if (slot >= 0 && slot < FE_MAX_SURFACES) {
        /* Don't release shared asset surfaces (font, cursor, etc.) */
        if (s_surfaces[slot].tex_page >= SHARED_PAGE_MIN &&
            s_surfaces[slot].tex_page < FE_SURFACE_PAGE_BASE) return;
        s_surfaces[slot].in_use = 0;
    }
}

static void frontend_copy_pretty_text(char *dst, size_t dst_cap, const char *src) {
    size_t di = 0;
    if (!dst || dst_cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src && di + 1 < dst_cap) {
        char ch = *src++;
        dst[di++] = (ch == '_') ? ' ' : ch;
    }
    dst[di] = '\0';
}

static int frontend_load_text_line_from_archive(const char *entry, const char *archive,
                                                char *out, size_t out_cap) {
    int sz = 0;
    char *data;
    size_t i = 0;
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    data = (char *)td5_asset_open_and_read(entry, archive, &sz);
    if (!data || sz <= 0) return 0;
    while (i + 1 < out_cap && i < (size_t)sz) {
        char ch = data[i];
        if (ch == '\r' || ch == '\n' || ch == '\0') break;
        out[i] = ch;
        i++;
    }
    out[i] = '\0';
    free(data);
    return out[0] != '\0';
}

const char *frontend_get_track_name(int track_index) {
    int name_index = track_index;
    if (track_index < 0)
        return "RANDOM TRACK";
    if (track_index < (int)(sizeof(s_track_schedule_to_name_index) / sizeof(s_track_schedule_to_name_index[0]))) {
        name_index = s_track_schedule_to_name_index[track_index];
    }
    if (name_index < 0 || name_index >= (int)(sizeof(s_track_display_names) / sizeof(s_track_display_names[0])))
        return "RANDOM TRACK";
    return s_track_display_names[name_index];
}

static void frontend_get_track_display_name(int track_index, int truncate_at_comma,
                                            char *out, size_t out_cap) {
    const char *src = frontend_get_track_name(track_index);
    size_t di = 0;
    if (!out || out_cap == 0) return;
    if (track_index < 0) {
        strncpy(out, "RANDOM TRACK", out_cap - 1);
        out[out_cap - 1] = '\0';
        return;
    }
    while (src[di] && di + 1 < out_cap) {
        if (truncate_at_comma && src[di] == ',') break;
        out[di] = src[di];
        di++;
    }
    out[di] = '\0';
}

static const char *frontend_get_car_display_name(int car_index) {
    static const char *fallback = "UNKNOWN CAR";
    char line[128];
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return fallback;
    if (s_car_display_names_loaded[car_index]) return s_car_display_names[car_index];
    /* [CONFIRMED @ 0x4667A8] English archive entry = "config.eng"; token 0 = display name.
     * Fall back to "config.nfo" (single-token) if "config.eng" is absent. */
    if (frontend_load_text_line_from_archive("config.eng", s_car_zip_paths[car_index], line, sizeof(line))) {
        char tok[64]; tok[0] = '\0';
        sscanf(line, "%63s", tok);
        if (tok[0]) frontend_copy_pretty_text(s_car_display_names[car_index], sizeof(s_car_display_names[car_index]), tok);
    }
    if (!s_car_display_names[car_index][0] &&
        frontend_load_text_line_from_archive("config.nfo", s_car_zip_paths[car_index], line, sizeof(line))) {
        frontend_copy_pretty_text(s_car_display_names[car_index], sizeof(s_car_display_names[car_index]), line);
    }
    if (!s_car_display_names[car_index][0]) {
        const char *zip = strrchr(s_car_zip_paths[car_index], '/');
        frontend_copy_pretty_text(s_car_display_names[car_index], sizeof(s_car_display_names[car_index]),
                                  zip ? zip + 1 : s_car_zip_paths[car_index]);
        char *dot = strrchr(s_car_display_names[car_index], '.');
        if (dot) *dot = '\0';
    }
    s_car_display_names_loaded[car_index] = 1;
    return s_car_display_names[car_index][0] ? s_car_display_names[car_index] : fallback;
}

/* SHORT car name (config.nfo line 1, e.g. "'97 CAMARO") — used by the High Scores
 * table. The original draws the localized short car name (g_localizationCarManufScratch
 * indexed by car type @0x413010), NOT the long line-0 name ("1997 CHEVROLET CAMARO...")
 * that frontend_get_car_display_name returns and that overflows the narrow 108px column.
 * Falls back to the long display name if line 1 is absent. */
static char s_car_short_names[TD5_CAR_COUNT][24];
static int  s_car_short_loaded[TD5_CAR_COUNT];
static const char *frontend_get_car_short_name(int car_index) {
    if (car_index < 0 || car_index >= TD5_CAR_COUNT)
        return frontend_get_car_display_name(car_index);
    if (s_car_short_loaded[car_index]) return s_car_short_names[car_index];
    s_car_short_loaded[car_index] = 1;
    s_car_short_names[car_index][0] = '\0';
    {
        int sz = 0;
        char *data = (char *)td5_asset_open_and_read("config.nfo", s_car_zip_paths[car_index], &sz);
        if (data && sz > 0) {
            int i = 0;
            while (i < sz && data[i] != '\n' && data[i] != '\r') i++;       /* skip line 0 */
            while (i < sz && (data[i] == '\r' || data[i] == '\n')) i++;
            char line[24]; int j = 0;
            while (i < sz && j + 1 < (int)sizeof(line) &&
                   data[i] != '\r' && data[i] != '\n' && data[i] != '\0')
                line[j++] = data[i++];
            line[j] = '\0';
            frontend_copy_pretty_text(s_car_short_names[car_index],
                                      sizeof(s_car_short_names[car_index]), line);
        }
        if (data) free(data);
    }
    if (!s_car_short_names[car_index][0])
        return frontend_get_car_display_name(car_index);
    return s_car_short_names[car_index];
}

int frontend_current_car_index(void) {
    if (s_selected_game_type == 5 &&
        s_selected_car >= 0 &&
        s_selected_car < (int)(sizeof(s_masters_roster) / sizeof(s_masters_roster[0])))
        return s_masters_roster[s_selected_car];
    return s_selected_car;
}


/* TD6 cop cars cp1..cp4 (Jaguar/Charger/Mustang/Cerbera police) sit at roster
 * indices 37 + TD6_NEW_CODES{9..12} = 46..49. Like the TD5 police (33-36) they
 * are NOT player-selectable and cannot be painted — they belong to Cop Chase. */
#define TD6_COP_FIRST 46
int frontend_car_is_cop(int i) {
    return (i >= 33 && i <= 36) || (i >= TD6_COP_FIRST && i <= TD6_COP_LAST);
}



/* True once a TD6 (ported) car is the active selection — drives the paint
 * color selector vs. the TD5 paint-arrows behaviour. */
int frontend_car_is_td6(int car_index) {
    return car_index >= TD5_BASE_CAR_COUNT && car_index < TD5_CAR_COUNT;
}

/* TD6 car that may be repainted: every ported car EXCEPT the cp1-4 cop cars
 * (police liveries are fixed). Gates the colour selector + the preview tint. */
int frontend_car_paintable(int car_index) {
    return frontend_car_is_td6(car_index) && !frontend_car_is_cop(car_index);
}

/* Whether a car offers ANY paint choice at all (used to grey-out the PAINT
 * button when it would do nothing):
 *   - TD6 cars: the modal colour picker, available on every ported car except
 *     the cp1-4 cop cars (== frontend_car_paintable).
 *   - TD5 cars: the 4-scheme ◄► paint arrows, available on every original car
 *     EXCEPT the special / hot-rod / police cars at indices 0x1C..0x24 (28..36:
 *     HOT DOG, MAUL, PITBULL, BEAST, WAGON + 4 police). Those ship a single
 *     paint in the original data — their carpic0..3 / carskin0..3 are byte-
 *     identical — so cycling paint changes nothing. This is the same range the
 *     paint-cycle handler (case 1) already refuses to cycle. */
int frontend_car_has_paint(int car_index) {
    if (frontend_car_is_td6(car_index))
        return frontend_car_paintable(car_index);
    return !(car_index >= 0x1C && car_index <= 0x24);
}

/* --- TD6 paint COLOR selector ---------------------------------------------
 * TD6 cars ship a single grayscale skin, so the player picks a body COLOR
 * instead of cycling 4 paint schemes. The PAINT button toggles a compact
 * selector in the LEFT column directly below PAINT (the Stats/Auto/OK/Back
 * buttons shift down to make room): a row of predefined swatches plus a
 * clickable HSV color map for any color. The chosen color tints the grayscale
 * body in-race (td5_render_set_vehicle_tint, committed at race init) and the
 * menu preview live (carpic modulate). Persists in td5re.ini [CarSelection]
 * TD6PaintColor (g_td5.ini.td6_paint_color). The button-shift + mouse-pick
 * helpers that need s_buttons / s_mouse live just after those declarations. */
const uint32_t s_td6_palette[] = {   /* 0xRRGGBB predefined quick picks */
    0xFF0000, 0xFF6000, 0xFFC000, 0xC8FF00, 0x10C010, 0x00C0C0,
    0x1078FF, 0x1010FF, 0x8000FF, 0xFF20C0, 0xFFFFFF, 0xC8C8C8,
    0x686868, 0x101010, 0x884400, 0xFFD040,
};

/* Layout in 640x480 canvas coords. When the panel is open the whole button
 * column is compressed (CAR/PAINT shift up, Stats/Auto/OK/Back move below the
 * panel) so OK/Back don't sit too low — see frontend_apply_color_panel_layout.
 * The list/map sit between the (raised) PAINT row and the Stats row. */

int s_color_panel_visible = 0;
/* Unified 2D cursor over the picker: rows 0-1 = predefined swatches (8 cols),
 * rows 2.. = the color map as a grid. All 4 arrows move it; the color under it
 * is applied live; the mouse sets it by clicking a swatch / map cell. */
int s_color_cur_col = 0;
int s_color_cur_row = 0;

/* RGB (0xRRGGBB) -> frontend vertex-diffuse modulate (0xAARRGGBB), full alpha.
 * The fe_draw_quad vertex diffuse is packed/consumed as 0xAARRGGBB (same field
 * + shader as the in-race tint at td5_render.c:1702), so NO R/B swap — an
 * earlier swap here turned red paint into a blue preview and disagreed with the
 * (correct) in-race tint that reads (rgb>>16)&FF as red. */
static uint32_t frontend_rgb_to_bgra(uint32_t c) {
    return 0xFF000000u | (c & 0x00FFFFFFu);
}
/* HSV (0..1 each) -> 0xRRGGBB. */
static uint32_t td6_hsv_to_rgb(float h, float s, float v) {
    while (h >= 1.0f) h -= 1.0f;  while (h < 0.0f) h += 1.0f;
    float hh = h * 6.0f; int i = (int)hh; float f = hh - (float)i;
    float p = v*(1.0f-s), q = v*(1.0f-s*f), t = v*(1.0f-s*(1.0f-f));
    float r, g, b;
    switch (i % 6) {
        case 0: r=v; g=t; b=p; break;  case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;  case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;  default: r=v; g=p; b=q; break;
    }
    int R=(int)(r*255.0f+0.5f), G=(int)(g*255.0f+0.5f), B=(int)(b*255.0f+0.5f);
    return ((uint32_t)R<<16) | ((uint32_t)G<<8) | (uint32_t)B;
}
/* Map (u,v in 0..1): hue across x; y top=white -> mid=pure -> bottom=black. */
uint32_t td6_map_color(float u, float v) {
    float sat, val;
    if (v < 0.5f) { sat = v * 2.0f; val = 1.0f; }
    else          { sat = 1.0f;     val = 1.0f - (v - 0.5f) * 2.0f; }
    return td6_hsv_to_rgb(u, sat, val);
}

static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
static void frontend_render_td6_color_panel(float sx, float sy) {
    if (!s_color_panel_visible) return;   /* hidden until the PAINT button opens it */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    /* backdrop behind the list + map */
    fe_draw_quad((TD6_CP_LIST_X - 3) * sx, (TD6_CP_LIST_Y - 5) * sy,
                 (TD6_CP_MAP_W + 6) * sx,
                 (TD6_CP_MAP_Y + TD6_CP_MAP_H + 16 - (TD6_CP_LIST_Y - 5)) * sy,
                 0xD8141420u, -1, 0, 0, 1, 1);
    /* predefined quick-pick swatches */
    for (int i = 0; i < TD6_PALETTE_N; i++) {
        int c = i % TD6_CP_COLS, r = i / TD6_CP_COLS;
        float x = (float)(TD6_CP_LIST_X + c * (TD6_CP_SW + TD6_CP_GAP));
        float y = (float)(TD6_CP_LIST_Y + r * (TD6_CP_SW + TD6_CP_GAP));
        fe_draw_quad(x * sx, y * sy, TD6_CP_SW * sx, TD6_CP_SW * sy,
                     frontend_rgb_to_bgra(s_td6_palette[i]), -1, 0, 0, 1, 1);
    }
    /* clickable HSV map: grid of cells (hue x; white->pure->black down y) */
    {
        const int mc = 28, mr = 11;
        float cw = (float)TD6_CP_MAP_W / (float)mc, ch = (float)TD6_CP_MAP_H / (float)mr;
        for (int yy = 0; yy < mr; yy++) for (int xx = 0; xx < mc; xx++) {
            float u = ((float)xx + 0.5f) / (float)mc, v = ((float)yy + 0.5f) / (float)mr;
            float x = (float)TD6_CP_MAP_X + (float)xx * cw;
            float y = (float)TD6_CP_MAP_Y + (float)yy * ch;
            fe_draw_quad(x*sx, y*sy, (cw + 0.6f)*sx, (ch + 0.6f)*sy,
                         frontend_rgb_to_bgra(td6_map_color(u, v)), -1, 0,0,1,1);
        }
    }
    /* BOLD cursor marker over the current grid cell (a yellow frame just outside
     * it). On a swatch (rows 0-1) it frames the swatch; on the map (rows 2+) it
     * frames the corresponding map cell. Drawn last so it sits on top. */
    {
        float mx0, my0, mw, mh;
        if (s_color_cur_row < 2) {
            mx0 = (float)(TD6_CP_LIST_X + s_color_cur_col * (TD6_CP_SW + TD6_CP_GAP));
            my0 = (float)(TD6_CP_LIST_Y + s_color_cur_row * (TD6_CP_SW + TD6_CP_GAP));
            mw = mh = (float)TD6_CP_SW;
        } else {
            float cw = (float)TD6_CP_MAP_W / (float)TD6_CP_COLS;
            float ch = (float)TD6_CP_MAP_H / (float)TD6_CP_MAP_ROWS;
            mx0 = (float)TD6_CP_MAP_X + (float)s_color_cur_col * cw;
            my0 = (float)TD6_CP_MAP_Y + (float)(s_color_cur_row - 2) * ch;
            mw = cw; mh = ch;
        }
        float e = 2.0f, t = 2.0f, ox = mx0 - e, oy = my0 - e, ow = mw + 2.0f*e, oh = mh + 2.0f*e;
        uint32_t mk = 0xFF00FFFFu;  /* BGRA yellow */
        fe_draw_quad(ox*sx, oy*sy, ow*sx, t*sy, mk, -1,0,0,1,1);
        fe_draw_quad(ox*sx, (oy+oh-t)*sy, ow*sx, t*sy, mk, -1,0,0,1,1);
        fe_draw_quad(ox*sx, oy*sy, t*sx, oh*sy, mk, -1,0,0,1,1);
        fe_draw_quad((ox+ow-t)*sx, oy*sy, t*sx, oh*sy, mk, -1,0,0,1,1);
    }
    /* CURRENT-COLOR bar (full panel width) below the map — always shows the active
     * color, including map-picked colors that aren't in the predefined list. */
    {
        float by = (float)(TD6_CP_MAP_Y + TD6_CP_MAP_H + 3);
        fe_draw_quad((float)(TD6_CP_LIST_X - 1) * sx, (by - 1) * sy,
                     (float)(TD6_CP_MAP_W + 2) * sx, 13 * sy, 0xFF000000u, -1, 0,0,1,1);
        fe_draw_quad((float)TD6_CP_LIST_X * sx, by * sy,
                     (float)TD6_CP_MAP_W * sx, 11 * sy,
                     frontend_rgb_to_bgra((uint32_t)g_td5.ini.td6_paint_color), -1, 0,0,1,1);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}







/* --- Button System --- */

FE_Button s_buttons[FE_MAX_BUTTONS];
int s_button_count;

/* TD6 color-panel helpers that depend on s_buttons / mouse state (declared
 * above). See the panel definition higher up for the layout + palette. */



/* Draw the TD6 body-only paint overlay for actual_car at preview x (canvas px),
 * MODULATEd by the selected paint colour. Lazily (re)loads the overlay surface
 * per car. No-op for TD5 cars or if there's no overlay surface. */
static void frontend_draw_car_paint_overlay(int actual_car, float x, float sx, float sy) {
    if (!frontend_car_paintable(actual_car)) return;
    if (s_paint_overlay_car != actual_car) {
        if (s_paint_overlay_surface > 0) frontend_release_surface(s_paint_overlay_surface);
        s_paint_overlay_surface = frontend_load_car_paint_overlay_surface(actual_car);
        s_paint_overlay_car = actual_car;
    }
    if (s_paint_overlay_surface > 0)
        fe_draw_surface_rect(s_paint_overlay_surface, x * sx, 124.0f * sy,
                             408.0f * sx, 280.0f * sy,
                             frontend_rgb_to_bgra((uint32_t)g_td5.ini.td6_paint_color));
}

static int s_cursor_visible;
static TD5_ScreenIndex s_logged_screen = (TD5_ScreenIndex)-1;
static int s_logged_inner_state = -1;

/* Original game button layout (from Ghidra decompilation of 0x415490):
 * Buttons use center-relative positioning:
 *   center_x = canvas_w / 2 (320 for 640px)
 *   center_y = canvas_h / 2 (240 for 480px)
 *   left_edge = center_x - 0xD2 (= 110 for 640px)
 * Auto-layout Y offsets from center_y:
 *   btn0: -0x93  btn1: -0x6B  btn2: -0x43  btn3: -0x1B
 *   btn4: +0x0D  btn5: +0x35  btn6: +0x5D  btn7: +0x85
 * Spacing = 40px (0x28) between buttons. */
#define FE_BTN_LEFT_OFFSET 0xD2        /* 210: center_x - this = left edge */
static const int s_auto_button_y_offset[] = {
    -0x93, -0x6B, -0x43, -0x1B, 0x0D, 0x35, 0x5D, 0x85, 0xAD
};
static int s_auto_button_idx = 0;
int s_selected_button = 0;
static int s_selection_from_mouse = 0; /* 1 when last selection came from mouse hover */

void frontend_reset_buttons(void) {
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        memset(&s_buttons[i], 0, sizeof(s_buttons[i]));
        s_buttons[i].highlight_ramp = 0;
    }
    s_button_count = 0;
    s_auto_button_idx = 0;
    s_selected_button = 0;
    s_selection_from_mouse = 0;
    s_button_index = -1;
    s_mouse_confirm_button = -1;
    s_mouse_hover_button  = -1;
    s_prev_mouse_hover_button = -1;
    s_mouse_flash_button = -1;
    s_prev_mouse_x = -1;
    s_prev_mouse_y = -1;
}

static int frontend_resolve_selected_button(void) {
    if (s_selected_button >= 0 && s_selected_button < FE_MAX_BUTTONS &&
        s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
        return s_selected_button;
    }

    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (s_buttons[i].active && !s_buttons[i].disabled) {
            s_selected_button = i;
            return i;
        }
    }

    return -1;
}

static int frontend_cycle_selected_button(int direction) {
    int prev = s_selected_button;
    int attempts = FE_MAX_BUTTONS;

    if (direction == 0) return 0;

    while (attempts-- > 0) {
        s_selected_button += direction;
        if (s_selected_button < 0) s_selected_button = FE_MAX_BUTTONS - 1;
        if (s_selected_button >= FE_MAX_BUTTONS) s_selected_button = 0;
        if (s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
            return s_selected_button != prev;
        }
    }

    s_selected_button = prev;
    return 0;
}

static int frontend_cycle_selected_button_by_row(int direction, int same_row) {
    int current = frontend_resolve_selected_button();
    int current_y;
    int idx;
    int attempts;

    if (direction == 0) return 0;
    if (current < 0) return frontend_cycle_selected_button(direction);

    current_y = s_buttons[current].y;
    idx = current;
    attempts = FE_MAX_BUTTONS;
    while (attempts-- > 0) {
        idx += direction;
        if (idx < 0) idx = FE_MAX_BUTTONS - 1;
        if (idx >= FE_MAX_BUTTONS) idx = 0;
        if (!s_buttons[idx].active || s_buttons[idx].disabled) continue;
        if (same_row) {
            if (s_buttons[idx].y != current_y) continue;
        } else {
            if (s_buttons[idx].y == current_y) continue;
        }
        s_selected_button = idx;
        return idx != current;
    }

    return 0;
}

static int frontend_cycle_selected_button_horizontal(int direction) {
    return frontend_cycle_selected_button_by_row(direction, 1);
}

static int frontend_cycle_selected_button_vertical(int direction) {
    return frontend_cycle_selected_button_by_row(direction, 0);
}

/* [PORT ENHANCEMENT 2026-06] True 2D spatial navigation by button geometry.
 * Given a unit direction (dx,dy in {-1,0,1}: left/right or up/down), pick the
 * active button whose CENTER lies in that direction and best matches the on-
 * screen layout: up/down stay in the current column, left/right stay on the
 * current row. The flat index-order nav (frontend_cycle_selected_button_by_row)
 * breaks on multi-column grids — e.g. the Controller-Binding screen lays the 10
 * driving actions out as two columns of five, so a DOWN press on the bottom of
 * the left column would spill to the TOP of the right column. This routes arrow
 * input by real (x,y) position instead. Returns the target button, or -1 when
 * no button lies in the requested direction (no wrap). */
static int frontend_spatial_pick(int dx, int dy) {
    int current = frontend_resolve_selected_button();
    int cx, cy, i, best = -1, best_cost = 0;

    if (current < 0) return -1;
    cx = s_buttons[current].x + s_buttons[current].w / 2;
    cy = s_buttons[current].y + s_buttons[current].h / 2;

    for (i = 0; i < FE_MAX_BUTTONS; i++) {
        int bx, by, pdx, pdy, primary, offaxis, cost;
        if (i == current) continue;
        if (!s_buttons[i].active || s_buttons[i].disabled) continue;
        bx = s_buttons[i].x + s_buttons[i].w / 2;
        by = s_buttons[i].y + s_buttons[i].h / 2;
        pdx = bx - cx;   /* + = to the right */
        pdy = by - cy;   /* + = downward     */
        if (dy != 0) {                      /* vertical move */
            primary = pdy * dy;             /* distance along travel axis  */
            offaxis = pdx < 0 ? -pdx : pdx; /* horizontal drift (column)   */
        } else {                            /* horizontal move */
            primary = pdx * dx;
            offaxis = pdy < 0 ? -pdy : pdy; /* vertical drift (row)        */
        }
        if (primary <= 0) continue;         /* not in the requested direction */
        /* Penalize perpendicular drift heavily so movement stays in the same
         * column (up/down) or row (left/right); nearest along the travel axis
         * breaks ties. Columns here are ~230px apart vs ~32px rows, so an 8x
         * weight makes a column jump strictly costlier than any in-column step. */
        cost = offaxis * 8 + primary;
        if (best < 0 || cost < best_cost) { best_cost = cost; best = i; }
    }
    return best;
}

/* Move the selection in a 2D direction using frontend_spatial_pick; returns 1
 * if the selection actually changed. */
static int frontend_move_selected_button_spatial(int dx, int dy) {
    int target = frontend_spatial_pick(dx, dy);
    if (target < 0 || target == s_selected_button) return 0;
    s_selected_button = target;
    return 1;
}

/**
 * Create a frontend button using the original's coordinate conventions:
 *
 * Original CreateFrontendDisplayModeButton(label, x, y, w, h, flags):
 *   - When x is negative and y == 0: auto-layout mode.
 *     x stores the negative button width (e.g. -0xE0 = -224),
 *     y is assigned from center-relative offset table.
 *     The button x position = center_x - btn_left_offset (= 110 for 640px).
 *   - When x and y are both explicit (positive or pre-computed):
 *     absolute placement in 640x480 virtual coords.
 */
int frontend_create_button(const char *label, int x, int y, int w, int h) {
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) {
            s_buttons[i].active = 1;
            s_buttons[i].disabled = 0;
            s_buttons[i].hidden = 0;
            s_buttons[i].highlight_ramp = 0;
            s_buttons[i].is_selector = 0;

            /* Width = the explicit w (the original's CreateFrontendDisplayModeButton
             * takes x and w independently; negative x is ONLY the auto-layout flag).
             * Most screens pass w==|x| so this is unchanged, but Music Test "TRACK"
             * (-0x120,w=0xA0) and the narrow OK buttons (-0x1xx,w=0x60) need their real
             * w. Previously |x| was used as the width, over-widening those buttons.
             * [CONFIRMED @ 0x418460: Select Track w=0xA0, OK w=0x60.] Fall back to |x|
             * only if w is unusable. */
            if (w > 0 && w != 200) {
                s_buttons[i].w = w;
            } else if (x < 0 && -x > 0) {
                s_buttons[i].w = -x;
            } else {
                s_buttons[i].w = 224;
            }
            s_buttons[i].h = (h > 0 && h != 32) ? h : 32;   /* 0x20 = 32 */

            /* Auto-layout: negative x (or both zero) triggers center-relative placement */
            if ((x < 0 || (x == 0 && y == 0)) && s_auto_button_idx < 9) {
                s_buttons[i].x = FE_CENTER_X - FE_BTN_LEFT_OFFSET;
                s_buttons[i].y = FE_CENTER_Y + s_auto_button_y_offset[s_auto_button_idx];
                s_auto_button_idx++;
            } else {
                s_buttons[i].x = x;
                s_buttons[i].y = y;
            }

            strncpy(s_buttons[i].label, label ? label : "", 63);
            s_buttons[i].label[63] = '\0';
            if (i >= s_button_count) s_button_count = i + 1;
            TD5_LOG_D(LOG_TAG, "Button created: index=%d label=\"%s\" pos=(%d,%d) size=%dx%d",
                      i, s_buttons[i].label, s_buttons[i].x, s_buttons[i].y,
                      s_buttons[i].w, s_buttons[i].h);
            return i;
        }
    }
    return -1;
}



/* [ARCH-DIVERGENCE: cursor flag polarity flipped + mouse-moved flag dropped; L5 sweep 2026-05-21]
 *   Ports ActivateFrontendCursorOverlay (0x004258C0) and DeactivateFrontendCursorOverlay (0x004258E0).
 *   Orig writes g_frontendCursorOverlayHidden = 1/0 with semantics inverted from its name; port
 *   stores s_cursor_visible with direct semantics (1=show). All 12 callers' arguments were inverted
 *   in the same commit so net behavior is unchanged. Orig also clears g_frontendMouseMovedFlag —
 *   port's mouse-moved tracking lives in update_frontend (s_prev_mouse_x/y compare) and does not
 *   need a separate flag clear here. */
void frontend_set_cursor_visible(int visible) {
    /* Phase 5 — direct semantics. visible=1 shows cursor, visible=0 hides.
     * Previous code did `s_cursor_visible = !visible` to match the original
     * binary's inverted ActivateFrontendCursorOverlay/DeactivateFrontend
     * CursorOverlay convention (state during interactive == "active" ==
     * 1 == suppress button highlight ≈ show cursor). The double-negation
     * was a footgun — calling set_cursor_visible(1) hid the cursor.
     * Operator flipped here and ALL 12 callers' arguments inverted in the
     * same commit so the net behavior is unchanged. */
    s_cursor_visible = visible;
}

static void frontend_render_cursor(void); /* forward decl — impl after draw queue types */

/* === Universal transition fades (PORT ENHANCEMENT — S03 2026-06-04) ============
 * Every navigable menu screen should fade OUT (Whoosh = SFX 5) when it is left
 * and fade IN (Crash1 chime = SFX 4) when it settles — INCLUDING screens added
 * to s_screen_table later with no per-screen audio wiring (e.g. the multiplayer
 * lobby [30] / future track screens, which today emit no slide SFX at all).
 *
 * Rather than hand-wire all 31 screens, the single transition choke point
 * (td5_frontend_set_screen) provides the fades as a DEFAULT and de-dups against
 * any screen that already plays its own, so nothing doubles and new screens
 * inherit the fades for free:
 *
 *   - s_fade_whoosh_emitted / s_fade_chime_emitted record whether a 5 / 4 was
 *     played during the CURRENT screen's lifetime (reset in set_screen). A
 *     screen that plays its own slide-out whoosh / slide-in chime sets these,
 *     which suppresses the matching default.
 *   - td5_frontend_set_screen() plays the default slide-OUT for the screen being
 *     LEFT if it never whooshed, and ARMS the default slide-IN chime for the
 *     screen being ENTERED.
 *   - td5_frontend_display_loop() fires the armed chime once the new screen
 *     settles (s_anim_complete) — or after a deadline backstop — unless the
 *     screen already chimed itself this frame (its fn() runs before the check).
 *
 * Boot/init, attract and the deliberately-silent end dialogs (CupWon/Failed/
 * SessionLocked) are excluded via frontend_screen_wants_fade(). The dedup is
 * exact for the original 30 screens (each already plays at least one whoosh in
 * its lifetime and chimes co-located with / before s_anim_complete) so this is
 * purely additive: existing per-screen audio is unchanged, gaps are filled. */
static int s_fade_whoosh_emitted;   /* a Play(5) happened during this screen's lifetime */
static int s_fade_chime_emitted;    /* a Play(4) happened during this screen's lifetime */
static int s_fade_in_pending;       /* default slide-in chime still owed to the new screen */

#define TD5_FE_FADE_IN_DEADLINE_MS 1500u /* backstop chime if a screen never sets s_anim_complete */

void frontend_play_sfx(int id) {
    if (id == 5) s_fade_whoosh_emitted = 1;
    if (id == 4) s_fade_chime_emitted = 1;
    td5_sound_play_frontend_sfx(id);
}


/* --- Text Rendering (simple bitmap font) --- */

/*
 * frontend_draw_string / frontend_draw_small_string
 *
 * [CONFIRMED @ 0x00424110] DrawFrontendFontStringPrimary: 12×12 glyph atlas,
 * 21 columns. col = (c-0x20) % 21 * 12, row = (c-0x20) / 21 * 12. Advance
 * from g_smallFontAdvance[c]. Renders to g_primaryWorkSurface via vtable +0x1C.
 * [CONFIRMED @ 0x004241E0] Secondary variant: same atlas, renders to
 * g_secondaryWorkSurface at y+8. Callers include ScreenGameOptions (0x41F990),
 * ScreenDisplayOptions (0x420400), ScreenOptionsHub (0x41D890).
 *
 * Port verdict: ZERO call sites exist for these stubs. All screen state machines
 * in the port use frontend_create_button() for option labels, which routes through
 * the draw queue / td5_frontend_render_ui_rects. The original's offscreen-surface
 * rendering path has been superseded. No-op stubs are CORRECT for the port.
 */
static void frontend_draw_string(int surface, const char *str_id, int x, int y) {
    (void)surface; (void)str_id; (void)x; (void)y;
    /* No-op: dialog text rendered live in td5_frontend_render_ui_rects */
}

static void frontend_draw_small_string(int surface, const char *str_id, int x, int y) {
    (void)surface; (void)str_id; (void)x; (void)y;
    /* No-op: small-text paths rendered live in td5_frontend_render_ui_rects */
}

/* --- Draw Queue --- */

FE_DrawCmd s_draw_queue[FE_MAX_DRAW_CMDS];
int s_draw_queue_count;

static void frontend_fill_rect(int surface, int x, int y, int w, int h, uint32_t color) {
    (void)surface;
    if (s_draw_queue_count >= FE_MAX_DRAW_CMDS) return;
    FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
    cmd->type = FE_CMD_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->color = color;
    cmd->tex_page = -1;
}

static void frontend_blit(int dst, int src, int dx, int dy) {
    (void)dst;
    int slot = src - 1;
    if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) return;
    if (s_draw_queue_count >= FE_MAX_DRAW_CMDS) return;
    FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
    cmd->type = FE_CMD_BLIT;
    cmd->x = dx; cmd->y = dy;
    cmd->w = s_surfaces[slot].width;
    cmd->h = s_surfaces[slot].height;
    cmd->tex_page = s_surfaces[slot].tex_page;
    cmd->u0 = 0.0f; cmd->v0 = 0.0f;
    cmd->u1 = 1.0f; cmd->v1 = 1.0f;
    cmd->color = 0xFFFFFFFF;
}

/* VectorUI cursor: SDF pointer (white outline + purple fill) drawn immediately,
 * called AFTER the sprite flush so it stays on top. */
static void fe_draw_cursor_proc(float sx, float sy) {
    if (!s_cursor_visible || !s_ps_cursor || s_cursor_msdf_page < 0) return;
    float cw = (s_cursor_w > 0 ? (float)s_cursor_w : 22.0f) * sx;
    float ch = (s_cursor_h > 0 ? (float)s_cursor_h : 30.0f) * sy;
    float x  = (float)s_mouse_x * sx;
    float y  = (float)s_mouse_y * sy;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_set_ps_override((void *)s_ps_cursor, SAMP_LINEAR_CLAMP);
    fe_draw_quad(x, y, cw, ch, 0xFFFFFFFF, s_cursor_msdf_page, 0.0f, 0.0f, 1.0f, 1.0f);
    td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

static void frontend_render_cursor(void) {
    if (!s_cursor_visible) return;
    /* VectorUI: the cursor is drawn after the sprite flush by fe_draw_cursor_proc
     * (so it stays on top); skip the queued bitmap path here. */
    if (g_td5.ini.vector_ui && s_ps_cursor && s_cursor_msdf_page >= 0) return;
    if (s_cursor_tex_page >= 0 && s_cursor_w > 0 && s_cursor_h > 0) {
        /* Draw textured cursor from snkmouse.tga */
        if (s_draw_queue_count < FE_MAX_DRAW_CMDS) {
            FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
            cmd->type = FE_CMD_BLIT;
            cmd->x = s_mouse_x;
            cmd->y = s_mouse_y;
            cmd->w = s_cursor_w;
            cmd->h = s_cursor_h;
            cmd->tex_page = s_cursor_tex_page;
            cmd->u0 = 0.0f; cmd->v0 = 0.0f;
            cmd->u1 = 1.0f; cmd->v1 = 1.0f;
            cmd->color = 0xFFFFFFFF;
        }
    } else {
        /* Fallback: yellow rectangle if cursor texture not loaded */
        if (s_draw_queue_count < FE_MAX_DRAW_CMDS) {
            FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
            cmd->type = FE_CMD_RECT;
            cmd->x = s_mouse_x - 2;
            cmd->y = s_mouse_y - 2;
            cmd->w = 12;
            cmd->h = 16;
            cmd->color = 0xFFFFFF00;
            cmd->tex_page = -1;
        }
    }
}

/* [ARCH-DIVERGENCE: DDraw Copy16BitSurfaceRect + vtbl[0x1c] Blt -> D3D11 Present; L5 sweep 2026-05-21]
 *   Unifies orig PresentPrimaryFrontendBufferViaCopy (0x00424AF0, software copy path) and
 *   PresentPrimaryFrontendBuffer (0x00424CA0, hardware Blt path) into a single D3D11
 *   end-scene + Present(1). The software-vs-hardware distinction is meaningless under D3D11
 *   so both callsites collapse to this one helper. */
void frontend_present_buffer(void) {
    td5_plat_render_end_scene();
    td5_plat_present(1);
}

void frontend_init_font_metrics_default(void) {
    memcpy(s_font_glyph_advance, k_font_glyph_advance_default, sizeof(s_font_glyph_advance));
}

void frontend_init_font_metrics_from_pixels(const uint8_t *pixels, int w, int h) {
    frontend_init_font_metrics_default();
    if (!pixels || w <= 0 || h <= 0) return;

    for (int glyph = 0; glyph < 96; glyph++) {
        int col = glyph % FONT_COLS;
        int row = glyph / FONT_COLS;
        int origin_x = col * FONT_CELL;
        int origin_y = row * FONT_CELL;
        int last_nonblack_col = -1;

        if (origin_x + FONT_CELL > w || origin_y + FONT_CELL > h) continue;

        for (int x = 0; x < FONT_CELL; x++) {
            for (int y = 0; y < FONT_CELL; y++) {
                const uint8_t *px = pixels + (((origin_y + y) * w + (origin_x + x)) * 4);
                /* After colorkey processing, transparent pixels have alpha=0
                 * and glyph pixels have alpha=255.  Check alpha, not BGR —
                 * the red colorkey leaves R=255 on background pixels, so
                 * checking BGR channels fires on transparent background pixels
                 * and produces advance=24 (full cell) for every glyph. */
                if (px[3] != 0) {
                    last_nonblack_col = x;
                    break;
                }
            }
        }

        if (glyph == (' ' - 0x20)) {
            s_font_glyph_advance[glyph] = 8;
        } else if (last_nonblack_col >= 0) {
            int advance = last_nonblack_col + 1 + FONT_GLYPH_PADDING;
            if (advance < 4) advance = 4;
            if (advance > FONT_CELL) advance = FONT_CELL;
            s_font_glyph_advance[glyph] = (uint8_t)advance;
        }
    }
    /* [S13] One-shot confirmation that the swapped font (Bahnschrift) measured
     * sane proportional advances off the loaded BodyText.png (runs once at load,
     * not per frame). */
    TD5_LOG_I(LOG_TAG, "Font metrics measured: adv A=%u M=%u W=%u i=%u a=%u (BodyText.png swap)",
              s_font_glyph_advance['A' - 0x20], s_font_glyph_advance['M' - 0x20],
              s_font_glyph_advance['W' - 0x20], s_font_glyph_advance['i' - 0x20],
              s_font_glyph_advance['a' - 0x20]);
}

static float fe_measure_text(const char *text, float sx, float sy) {
    float width = 0.0f;
    float gsx = fe_glyph_sx(sx, sy);   /* 4:3-locked glyph width (see fe_glyph_sx) */

    if (!text) return 0.0f;

    if (td5_font_ready()) {            /* native TTF: real advances + tracking (matches fe_draw_text) */
        const float cap_px = 15.0f * sy;
        const float hscale = (sx < sy) ? (sx / sy) : 1.0f;
        const float trkn   = (float)FONT_GLYPH_TRACKING * sy * hscale;
        for (int i = 0; text[i]; i++)
            width += td5_font_advance(toupper((unsigned char)text[i]), cap_px) * hscale + trkn;
        return width;
    }

    for (int i = 0; text[i]; i++) {
        int c = toupper((unsigned char)text[i]);
        if (c < 32 || c > 127) {
            width += (14.0f + FONT_GLYPH_TRACKING) * gsx;
            continue;
        }
        width += ((float)s_font_glyph_advance[c - 0x20] + FONT_GLYPH_TRACKING) * gsx;
    }

    return width;
}

/* [PORT ENHANCEMENT 2026-06] Greedy word-wrap a string into up to max_lines lines,
 * each fitting within maxw pixels at the given text scale. Returns the number of
 * lines produced (>=1). Breaks on spaces; a single word wider than maxw is placed
 * on its own line (it may still exceed maxw, but real device names word-wrap
 * cleanly). Used so a long controller name (e.g. "Controller (8BitDo Ultimate 2C
 * Wireless Controller) #1") is shown in full instead of being clipped to two
 * fixed lines. Each line buffer is 64 bytes; pass lines as char[N][64]. */
static int fe_wrap_text_lines(const char *s, float maxw, float sx, float sy,
                              char lines[][64], int max_lines) {
    char cur[64];
    int nlines = 0;
    int i = 0, len;
    if (!s) s = "";
    len = (int)strlen(s);
    if (max_lines < 1) max_lines = 1;
    cur[0] = '\0';
    while (i < len && nlines < max_lines) {
        int ws, we, wl;
        char cand[80];
        while (i < len && s[i] == ' ') i++;     /* skip run of spaces */
        if (i >= len) break;
        ws = i;
        while (i < len && s[i] != ' ') i++;      /* [ws,we) is one word */
        we = i;
        wl = we - ws;
        if (cur[0] == '\0') {
            int n = wl > 63 ? 63 : wl;
            memcpy(cand, s + ws, (size_t)n); cand[n] = '\0';
        } else {
            snprintf(cand, sizeof cand, "%s %.*s", cur, wl, s + ws);
        }
        if (cur[0] == '\0' || fe_measure_text(cand, sx, sy) <= maxw) {
            /* Fits (or the line is empty so the word must go here regardless). */
            strncpy(cur, cand, sizeof cur); cur[sizeof cur - 1] = '\0';
        } else {
            /* Doesn't fit: flush the current line, start a new one with this word. */
            strncpy(lines[nlines], cur, 64); lines[nlines][63] = '\0';
            nlines++;
            if (nlines >= max_lines) { cur[0] = '\0'; break; }
            {
                int n = wl > 63 ? 63 : wl;
                memcpy(cur, s + ws, (size_t)n); cur[n] = '\0';
            }
        }
    }
    if (cur[0] != '\0' && nlines < max_lines) {
        strncpy(lines[nlines], cur, 64); lines[nlines][63] = '\0';
        nlines++;
    }
    if (nlines == 0) { lines[0][0] = '\0'; nlines = 1; }
    return nlines;
}

int frontend_advance_tick(void) {
    s_anim_tick += 2 * s_fe_logic_ticks;
    return 1;
}

/* --- Fade --- */



/*
 * Maps external car-id / cup-schedule slot index → s_car_zip_paths index.
 * Mirrors DAT_00463E24[37] from the original binary (confirmed by RE).
 * Quick-race default: all cup_schedule_track[i] = 0 → type index 7 (XKR).
 */
static const int s_ext_car_to_type_index[TD5_CAR_COUNT] = {
     7,  2, 17, 33, 22, 31, 32, 34, 18, 14,
     1, 15, 13,  9, 11,  5,  0, 35,  8,  3,
     4, 12, 26, 10, 36, 16, 19, 25, 20, 23,
     6, 24, 21, 30, 28, 27, 29,
    /* 37-75: TD6 cars map to themselves (this conversion is not applied at
     * race init anyway — see the note in frontend_init_race_schedule). */
    37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
    71, 72, 73, 74, 75
};

/*
 * Difficulty tier table — 3 tiers × 6 ext car IDs.
 * From original binary @ 0x463e10 [CONFIRMED].
 * Only 3 real rows exist (18 bytes); rows 3-7 in the old table were actually
 * reading into the adjacent gExtCarIdToTypeIndex data @ 0x463e24.
 * Default/single-race path picks rand() % 6 into the tier row.
 */
static const int8_t s_difficulty_tier_cars[3][6] = {
    { 10, 14,  7,  4, 11,  9 },  /* tier 0: CHEVELLE, '69 CAMARO, XKR, SKYLINE, CUDA, '69 CHARGER */
    {  9,  6,  3, 13, 12,  0 },  /* tier 1: '69 CHARGER, '98 VANTAGE, '98 CORVETTE, '69 CORVETTE, COBRA, VIPER */
    { 22, 24, 16, 17, 19, 18 },  /* tier 2: R390, DAYTONA, P.VANTAGE, SERIES 1, GTS-R, SPEED 12 */
};

/* Check if ext_id is already used by any active slot */
static int frontend_ai_ext_id_taken(int ext_id, const int *slot_ext_ids,
                                    const int *slot_active, int count) {
    for (int i = 0; i < count; i++) {
        if (slot_active[i] && slot_ext_ids[i] == ext_id)
            return 1;
    }
    return 0;
}

/* ========================================================================
 * S21 opponent pool, bucketed by top speed (PORT ENHANCEMENT 2026-06-05)
 *
 * Replaces the hardcoded 3x6 s_difficulty_tier_cars roster (used by the
 * default / single-race path) with a dynamic pool: EVERY drivable car —
 * all TD5 cars (ext_id 0..32) AND all ported TD6 cars (ext_id 37..75) — is
 * eligible. Cars are sorted by their carparam top-speed field (file offset
 * 0x100 == tuning +0x74; see td5_physics_load_carparam and
 * project_td6_car_perf_normalization, which remaps TD6 perf onto TD5's range
 * so they bucket sensibly alongside TD5 cars) and split into three contiguous
 * speed bands. The difficulty tier selects the band:
 *     Easy   (tier 0) -> slowest third
 *     Normal (tier 1) -> middle third
 *     Hard   (tier 2) -> fastest third
 * Opponents are drawn from the matching band, distinct first (the band is
 * shuffled then walked) so there is no duplicate-car spam; duplicates appear
 * only when more opponents are requested than the band has cars, in which case
 * the round-robin wrap spreads them evenly rather than repeating one car.
 *
 * The police / cop-chase cars are excluded — TD5 police (ext_id 33..36) AND
 * the ported TD6 cop cars cp1..cp4 (ext_id 46..49) — via frontend_car_is_cop();
 * they are cop-chase vehicles, not race cars, so they must never spawn as AI
 * opponents. Any car whose carparam.dat cannot be read is also skipped
 * automatically. If NO carparam.dat is readable the caller falls back to the
 * original s_difficulty_tier_cars roster (which lists no cop ids).
 * ======================================================================== */

/* The pool spans every non-cop s_car_zip_paths entry (TD5 0..32 + TD6 37..75,
 * minus the cop ranges 33..36 and 46..49), so the arrays are sized to the full
 * car table. Cop exclusion is handled by frontend_car_is_cop() in the builder. */

/* Lazily-built, sorted-by-top-speed list of eligible cars. Built once and
 * cached — carparam top speeds are static data that never change at runtime. */
static int s_speed_pool_ids[TD5_CAR_COUNT];    /* ext_ids, top speed ascending */
static int s_speed_pool_speeds[TD5_CAR_COUNT]; /* parallel top-speed values */
static int s_speed_pool_count = -1;            /* -1 = not built yet */

/* Read a car's carparam top-speed (int16 at file offset 0x100), or -1 if the
 * carparam.dat is missing / too short. */
static int frontend_read_car_top_speed(int ext_id) {
    const char *zip = td5_asset_get_car_zip_path(ext_id);
    int sz = 0, top = -1;
    void *data;
    if (!zip) return -1;
    data = td5_asset_open_and_read("carparam.dat", zip, &sz);
    if (data) {
        if (sz >= 0x102)
            top = (int)*(const int16_t *)((const uint8_t *)data + 0x100);
        free(data);
    }
    return top;
}

/* Build the speed-sorted eligible-car pool (idempotent / cached). */
static void frontend_build_speed_pool(void) {
    int n = 0;
    if (s_speed_pool_count >= 0) return;   /* already built */
    for (int id = 0; id < TD5_CAR_COUNT; id++) {
        int top;
        if (frontend_car_is_cop(id))
            continue;                      /* skip cop-chase cars (TD5 33-36 + TD6 46-49) */
        if (id == 30)
            continue;                      /* [2026-06-19] exclude the Pitbull Special
                                            * (index 30, sp8.zip) from the AI opponent
                                            * pool at user request — it's a bonus car and
                                            * lands in the top speed band as an opponent */
        top = frontend_read_car_top_speed(id);
        if (top <= 0) continue;            /* skip cars with no/garbage carparam */
        s_speed_pool_ids[n]    = id;
        s_speed_pool_speeds[n] = top;
        n++;
    }
    /* Insertion sort by top speed ascending (n <= TD5_CAR_COUNT, trivial). */
    for (int i = 1; i < n; i++) {
        int ki = s_speed_pool_ids[i], ks = s_speed_pool_speeds[i], j = i - 1;
        while (j >= 0 && s_speed_pool_speeds[j] > ks) {
            s_speed_pool_ids[j + 1]    = s_speed_pool_ids[j];
            s_speed_pool_speeds[j + 1] = s_speed_pool_speeds[j];
            j--;
        }
        s_speed_pool_ids[j + 1]    = ki;
        s_speed_pool_speeds[j + 1] = ks;
    }
    s_speed_pool_count = n;
    if (n > 0) {
        TD5_LOG_I(LOG_TAG,
                  "speed_pool: built n=%d slowest=ext%d(%d) median=ext%d(%d) fastest=ext%d(%d)",
                  n, s_speed_pool_ids[0], s_speed_pool_speeds[0],
                  s_speed_pool_ids[n / 2], s_speed_pool_speeds[n / 2],
                  s_speed_pool_ids[n - 1], s_speed_pool_speeds[n - 1]);
    } else {
        TD5_LOG_W(LOG_TAG, "speed_pool: no carparam.dat readable — falling back to tier roster");
    }
}

/* ========================================================================
 * Car "at a glance" stat bars (Speed / Accel / Grip).
 *
 * Three indicator bars normalised ACROSS THE WHOLE ROSTER: the slowest car maps
 * to an empty bar and the fastest to a full one, everything else linearly
 * between. Acceleration is the published 0-60 TIME (lower = better), so its
 * fraction is inverted. The source numbers are the SAME published config.nfo
 * fields the MORE STATS spec sheet shows (field 7 = top speed mph, field 8 =
 * 0-60 sec, field 14 = lateral acc g -> "grip"), so the bars and the sheet
 * always agree. Cached one-time scan, mirroring frontend_build_speed_pool
 * above; cop-chase cars are excluded the same way. No numbers are ever drawn —
 * bars only (user request). ======================================================== */
static float s_cstat_spd_min, s_cstat_spd_max;
static float s_cstat_acc_min, s_cstat_acc_max;   /* 0-60 time: min == quickest */
static float s_cstat_hnd_min, s_cstat_hnd_max;
static int   s_cstat_ranges_built = 0;

/* Parse three raw config.nfo strings to numeric stats. Returns 1 only if all
 * three are present and positive (so missing/garbage cars are skipped). */
static int frontend_glance_from_fields(const char *f7, const char *f8, const char *f14,
                                       float *spd, float *acc, float *hnd) {
    float s = (float)atof(f7);
    float a = (float)atof(f8);
    float h = (float)atof(f14);
    if (s <= 0.0f || a <= 0.0f || h <= 0.0f) return 0;
    *spd = s; *acc = a; *hnd = h;
    return 1;
}

/* Read a car's three glance stats straight from its config.nfo, WITHOUT
 * touching the shared s_car_spec cache (this is used only by the range builder,
 * which scans every car and would otherwise clobber the displayed car). The
 * line-split mirrors frontend_load_car_spec_fields. Returns 1 on success. */
static int frontend_read_car_glance_stats(int ext_id, float *spd, float *acc, float *hnd) {
    const char *zip = td5_asset_get_car_zip_path(ext_id);
    char fields[17][48];
    char *data;
    int sz = 0, field;
    size_t i;
    if (!zip) return 0;
    data = (char *)td5_asset_open_and_read("config.nfo", zip, &sz);
    if (!data || sz <= 0) { free(data); return 0; }
    for (field = 0; field < 17; field++) fields[field][0] = '\0';
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(fields[0])) fields[field][j++] = data[i];
            i++;
        }
        fields[field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
    return frontend_glance_from_fields(fields[7], fields[8], fields[14], spd, acc, hnd);
}

/* Build the roster-wide min/max for each stat (idempotent / cached). Excludes
 * cop-chase cars like the speed pool, and any car whose config.nfo won't parse. */
static void frontend_build_carstat_ranges(void) {
    int id, n = 0;
    if (s_cstat_ranges_built) return;
    s_cstat_ranges_built = 1;
    s_cstat_spd_min = s_cstat_acc_min = s_cstat_hnd_min =  1e30f;
    s_cstat_spd_max = s_cstat_acc_max = s_cstat_hnd_max = -1e30f;
    for (id = 0; id < TD5_CAR_COUNT; id++) {
        float spd, acc, hnd;
        if (frontend_car_is_cop(id)) continue;
        if (!frontend_read_car_glance_stats(id, &spd, &acc, &hnd)) continue;
        if (spd < s_cstat_spd_min) s_cstat_spd_min = spd;
        if (spd > s_cstat_spd_max) s_cstat_spd_max = spd;
        if (acc < s_cstat_acc_min) s_cstat_acc_min = acc;
        if (acc > s_cstat_acc_max) s_cstat_acc_max = acc;
        if (hnd < s_cstat_hnd_min) s_cstat_hnd_min = hnd;
        if (hnd > s_cstat_hnd_max) s_cstat_hnd_max = hnd;
        n++;
    }
    if (n == 0) {   /* nothing readable — neutral ranges so bars render half-full */
        s_cstat_spd_min = s_cstat_acc_min = s_cstat_hnd_min = 0.0f;
        s_cstat_spd_max = s_cstat_acc_max = s_cstat_hnd_max = 1.0f;
        TD5_LOG_W(LOG_TAG, "carstat_ranges: no config.nfo readable — bars neutral");
        return;
    }
    TD5_LOG_I(LOG_TAG,
              "carstat_ranges: n=%d spd[%.0f..%.0f] 0-60[%.2f..%.2f] grip[%.2f..%.2f]",
              n, s_cstat_spd_min, s_cstat_spd_max,
              s_cstat_acc_min, s_cstat_acc_max, s_cstat_hnd_min, s_cstat_hnd_max);
}

static float frontend_glance_frac(float v, float lo, float hi) {
    float f;
    if (hi - lo < 1e-6f) return 0.5f;     /* degenerate (single-car) range */
    f = (v - lo) / (hi - lo);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* Map a car's raw stats to three [0,1] bar fractions. Accel is inverted (a
 * lower 0-60 time -> a fuller bar). */
static void frontend_normalize_glance(float spd, float acc, float hnd,
                                      float *fs, float *fa, float *fg) {
    frontend_build_carstat_ranges();
    *fs = frontend_glance_frac(spd, s_cstat_spd_min, s_cstat_spd_max);
    *fa = 1.0f - frontend_glance_frac(acc, s_cstat_acc_min, s_cstat_acc_max);  /* inverted */
    *fg = frontend_glance_frac(hnd, s_cstat_hnd_min, s_cstat_hnd_max);
}

/* Resolve the [lo,hi) car-pool slice for a difficulty tier (0/1/2). Degenerate
 * small pools collapse to "use the whole pool". */
static void frontend_speed_band_for_tier(int tier, int *lo, int *hi) {
    int n = s_speed_pool_count;
    int b0, b1;
    if (tier < 0) tier = 0;
    if (tier > 2) tier = 2;
    b0 = n / 3;            /* slow|mid boundary */
    b1 = (2 * n) / 3;      /* mid|fast boundary */
    if (tier == 0)      { *lo = 0;  *hi = b0; }
    else if (tier == 1) { *lo = b0; *hi = b1; }
    else                { *lo = b1; *hi = n;  }
    if (*hi <= *lo) { *lo = 0; *hi = n; }   /* tiny pool -> all cars */
}

/* ========================================================================
 * Multiplayer Options — split-screen layout tables (PORT ENHANCEMENT 2026-06)
 *
 * For N local human players, the selectable split layouts. The N players fill
 * the first N cells of a cols x rows grid (row-major); any remaining cells are
 * "missing" and get a (deferred) content selector. Per the design:
 *   1: single   2: L|R or U/D   3: L|R / U/D / 2x2(+1 empty)   4: 2x2
 *   5: 3x2 or 2x3 (+1 empty)    6: 3x2 or 2x3    7: 3x3 (+2 empty)
 *   8: 3x3 (+1 empty)           9: 3x3
 * ======================================================================== */

const MpSplitLayout *mp_split_layouts(int n, int *count)
{
    static const MpSplitLayout L1[]   = { {"SINGLE", 1, 1} };
    static const MpSplitLayout L2[]   = { {"LEFT / RIGHT", 2, 1}, {"UP / DOWN", 1, 2} };
    static const MpSplitLayout L3[]   = { {"LEFT / RIGHT", 3, 1}, {"UP / DOWN", 1, 3}, {"2X2 GRID", 2, 2} };
    static const MpSplitLayout L4[]   = { {"2X2 GRID", 2, 2} };
    static const MpSplitLayout L56[]  = { {"3X2 GRID", 3, 2}, {"2X3 GRID", 2, 3} };
    static const MpSplitLayout L789[] = { {"3X3 GRID", 3, 3} };
    switch (n) {
        case 1:  if (count) *count = 1; return L1;
        case 2:  if (count) *count = 2; return L2;
        case 3:  if (count) *count = 3; return L3;
        case 4:  if (count) *count = 1; return L4;
        case 5:
        case 6:  if (count) *count = 2; return L56;
        default: if (count) *count = 1; return L789;  /* 7,8,9 (callers clamp n<=9) */
    }
}

/* Resolve the active layout for (n, sel) → cols/rows + missing-cell count. */
void mp_resolve_layout(int n, int sel, int *cols, int *rows, int *missing)
{
    int cnt = 1;
    const MpSplitLayout *opts = mp_split_layouts(n, &cnt);
    int c, r, m;
    if (sel < 0 || sel >= cnt) sel = 0;
    c = opts[sel].cols;
    r = opts[sel].rows;
    m = c * r - n;
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    if (cols)    *cols = c;
    if (rows)    *rows = r;
    if (missing) *missing = m;
}

/* What to display in an empty split-screen cell. STUB options for now — the
 * actual rendering of map/standings into the empty pane is a deferred follow-up
 * (the user explicitly asked to wire only the selector at this stage). */
const char *const k_mp_missing_content[3] = { "EMPTY", "MAP", "STANDINGS" };

/* Commit the AI-spectate pane count + split grid for a race launch: clamps
 * the requested spectate count against the live AI field and the viewport
 * cap, stores it, and (re)resolves the split grid for humans+spectate panes.
 * Single source for the Quick Race menu path and the AutoRace SpectateScreens
 * override so the clamp/grid logic cannot drift. */
static void frontend_commit_pane_layout(int eff_humans, int requested_spectate)
{
    int spectate = requested_spectate;
    if (spectate < 0) spectate = 0;
    if (spectate > g_td5.num_ai_opponents) spectate = g_td5.num_ai_opponents;
    if (spectate > TD5_MAX_VIEWPORTS - eff_humans) spectate = TD5_MAX_VIEWPORTS - eff_humans;
    if (spectate < 0) spectate = 0;
    g_td5.num_spectate_screens = spectate;

    int eff_panes = eff_humans + spectate;
    if (eff_panes < 1) eff_panes = 1;
    if (eff_panes > TD5_MAX_VIEWPORTS) eff_panes = TD5_MAX_VIEWPORTS;

    /* For >=2 panes split is on and the layout grid (cols x rows) overrides
     * the automatic ladder in td5_game_init_viewport_layout. split_screen_mode
     * keeps its legacy meaning for HUD / minimap / sound consumers: 0=single,
     * 2=two-player left|right, 1=any other split "on". */
    if (eff_panes >= 2) {
        int cols = 0, rows = 0, missing = 0;
        mp_resolve_layout(eff_panes, s_mp_layout_sel, &cols, &rows, &missing);
        g_td5.split_grid_cols = cols;
        g_td5.split_grid_rows = rows;
        g_td5.split_screen_mode = (eff_panes == 2 && cols == 2) ? 2 : 1;
        g_td5.split_missing_content[0] = (missing > 0) ? s_mp_missing_content[0] : 0;
        g_td5.split_missing_content[1] = (missing > 1) ? s_mp_missing_content[1] : 0;
    } else {
        g_td5.split_grid_cols = 0;
        g_td5.split_grid_rows = 0;
        g_td5.split_screen_mode = 0;
        g_td5.split_missing_content[0] = 0;
        g_td5.split_missing_content[1] = 0;
    }
}

void frontend_init_race_schedule(void) {
    int i;
    int slot_active[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_ext_id[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_variant[TD5_MAX_RACER_SLOTS] = {0};
    int start_slot = 1;
    int eff_humans = 1;   /* human-driven slots actually rendered/controlled (<=2) */

    /* A normal race entry (new race / Race Again / AutoRace) always RECORDS
     * fresh input. Clear any replay/playback/demo state left over from a prior
     * View Replay or attract demo. View Replay bypasses this function entirely
     * (it re-enters the same race without rebuilding the schedule), so its flags
     * survive; the attract-demo path re-sets the demo flag after this returns. */
    td5_input_set_replay_mode(0);
    td5_input_set_playback_active(0);
    td5_game_set_replay_mode(0);
    td5_game_set_demo_mode(0);

#ifndef TD5RE_RELEASE
    /* [item #4] When launching from Quick Race, log the (dev) span-offset field's
     * committed value once. td5_game.c InitRace applies g_td5.ini.start_span_offset
     * per-slot (16-bit wrap) to both TD5 and TD6 tracks. */
    if (s_current_screen == TD5_SCREEN_QUICK_RACE)
        TD5_LOG_I(LOG_TAG, "QuickRace launch: start_span_offset=%d", g_td5.ini.start_span_offset);
#endif

    g_td5.race_requested = 1;
    g_td5.car_index   = frontend_current_car_index();
    g_td5.track_index = (s_current_screen == TD5_SCREEN_ATTRACT_MODE)
                        ? s_attract_track
                        : s_selected_track;

    /* --- Local human count + split-screen layout (PORT ENHANCEMENT 2026-06) ---
     * s_num_human_players is the single source of truth, set by EITHER the Quick
     * Race "Players" selector OR the rebuilt Multiplayer Options "PLAYERS" button
     * (both edit the same static). The chosen layout (s_mp_layout_sel) resolves
     * to a cols x rows grid consumed by td5_game_init_viewport_layout. The
     * untouched default (1 human + 5 AI) stays byte-faithful to the legacy grid.
     * g_td5.network_active=0 ensures the local split-screen path is taken. */
    {
        int humans, ai;
        /* Human count: only the multiplayer lobby flow (s_mp_flow, sets
         * s_two_player_mode) runs >1 local human. Quick Race is single-player
         * (driven by the active controller); everything else is single-player. */
        if (s_launching_net_race) {
            /* S10: each synced network player occupies an input-reading racer
             * slot (slots 0..N-1 read s_control_bits, which the lockstep fills
             * with the host-merged input each frame); the remaining slots are AI
             * fill. NOTE: this currently produces N split-screen viewports
             * because it reuses the local split-screen slot model; a single
             * local-follow viewport for net play is a documented follow-up. */
            int np = td5_net_get_player_count();
            humans = (np > 0) ? np : 1;
        } else if (s_mp_flow && s_two_player_mode != 0)
            humans = (s_num_human_players > 1) ? s_num_human_players : 2;
        else
            humans = 1;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;

        /* [PORT ENHANCEMENT 2026-06] Single-player: the active menu controller
         * (whoever navigated here) becomes the driver = player 0's device. */
        if (!s_mp_flow) {
            td5_input_set_input_source(0, s_active_menu_device);
            td5_save_set_player_device_index(0, (uint32_t)s_active_menu_device);
        }

        /* AI count: the screens that expose an opponents selector (Quick Race +
         * the track selector race-option row) drive s_num_ai_opponents; other
         * flows use the legacy fill (cup game-types override it anyway).
         * [FIX 2026-06-05 race-again-opponent-count] "Race Again" re-enters this
         * from the RACE_RESULTS screen, which previously fell through to the
         * legacy 5-opponent fill regardless of the original field size. For a
         * Single/Quick Race (game_type 0) honour the snapshotted opponent count
         * (restored into s_num_ai_opponents above); cups (game_type != 0) keep
         * the legacy fill and override the count downstream. */
        if (s_current_screen == TD5_SCREEN_QUICK_RACE ||
            s_current_screen == TD5_SCREEN_TRACK_SELECTION ||
            (s_current_screen == TD5_SCREEN_RACE_RESULTS && s_selected_game_type == 0))
            ai = s_num_ai_opponents;
        else
            ai = TD5_LEGACY_RACE_SLOTS - humans;
        if (ai < 0) ai = 0;
        if (ai > TD5_MAX_RACER_SLOTS - humans) ai = TD5_MAX_RACER_SLOTS - humans;

        g_td5.num_human_players = humans;
        g_td5.num_ai_opponents  = ai;
    }

    eff_humans = g_td5.num_human_players;
    if (eff_humans < 1) eff_humans = 1;
    if (eff_humans > TD5_MAX_VIEWPORTS) eff_humans = TD5_MAX_VIEWPORTS;

    /* [PORT ENHANCEMENT 2026-06-08] AI spectator split-screens (dev/profiling).
     * Quick Race may render the first N AI cars (slots 1..N) each in its own
     * viewport pane on top of the human pane(s); only the humans read input.
     * Inert outside Quick Race. Clamping + split-grid resolution live in
     * frontend_commit_pane_layout (shared with the AutoRace SpectateScreens
     * override so the two paths cannot drift). */
    /* [S31 net] A network race renders ONE full-screen view per machine
     * (each pinned to that machine's own car in InitRace step 17) — the
     * net players are racer SLOTS, not local split panes. */
    frontend_commit_pane_layout(s_launching_net_race ? 1 : eff_humans,
                                (s_current_screen == TD5_SCREEN_QUICK_RACE)
                                    ? s_num_spectate_screens : 0);
    int eff_panes = (s_launching_net_race ? 1 : eff_humans) + g_td5.num_spectate_screens;
    g_td5.network_active    = s_launching_net_race;   /* S10: net race engages lockstep */
    s_launching_net_race    = 0;                      /* one-shot intent */

    /* [FIX 2026-06-04 S05] Re-commit the Traffic / Police toggles at launch.
     * The Quick Race + Multiplayer track-select screen edits s_game_option_traffic
     * / s_game_option_cops AFTER ConfigureGameTypeFlags already ran (it's invoked
     * at game-type selection on the Main Menu), and the MULTIPLAYER lobby flow
     * never calls ConfigureGameTypeFlags at all — so the toggles were stale (or
     * ignored entirely) at race start. For the modes that expose the rows
     * (s_selected_game_type == 0 = Single Race / Quick Race / Multiplayer; cup
     * game-types 1..6 and the special modes 7/8/9 force their own values, so they
     * are left untouched) push the latest toggle state into the runtime gates
     * read by InitRace / td5_ai. Mirrors ConfigureGameTypeFlags @ 0x00410CA0
     * case 0. */
    if (s_selected_game_type == 0) {
        /* [dynamic-traffic] 5-state volume row: 0=Off 1=Low 2=Medium 3=High
         * 4=Very High. Clamp to 0..4 (NOT & 3, which would swallow Very High). */
        int tv = s_game_option_traffic;
        if (tv < 0) tv = 0;
        if (tv > TD5_TRAFFIC_VOLUME_COUNT - 1) tv = TD5_TRAFFIC_VOLUME_COUNT - 1;
        g_td5.traffic_volume            = tv;
        g_td5.traffic_enabled           = (g_td5.traffic_volume != 0);
        g_td5.special_encounter_enabled = s_game_option_cops;
    }

    TD5_LOG_I(LOG_TAG,
              "InitRaceSchedule: split=%d grid=%dx%d humans=%d opp=%d eff=%d spectate=%d panes=%d 2p=%d layout_sel=%d",
              g_td5.split_screen_mode, g_td5.split_grid_cols, g_td5.split_grid_rows,
              g_td5.num_human_players, g_td5.num_ai_opponents, eff_humans,
              g_td5.num_spectate_screens, eff_panes,
              s_two_player_mode, s_mp_layout_sel);

    /* Slot 0 = player, always active */
    slot_active[0]  = 1;
    slot_ext_id[0]  = s_selected_car;
    slot_variant[0] = s_selected_paint;

    /* [S31 NET 2026-06-10] Network race: identical grids everywhere. Fill the
     * net-player slots from the host-broadcast config and reseed the CRT with
     * the shared seed so the rand()-driven AI fill below picks the SAME cars
     * on every machine. Lockstep has no state correction -- a different
     * carparam on any slot is a permanent desync. */
    TD5_NetRaceConfig net_cfg;
    int net_cfg_valid = 0;
    if (g_td5.network_active) {
        if (td5_net_get_race_config(&net_cfg)) {
            int np = td5_net_get_player_count();
            net_cfg_valid = 1;
            if (np > TD5_MAX_RACER_SLOTS) np = TD5_MAX_RACER_SLOTS;
            for (i = 0; i < np && i < 6; i++) {
                slot_active[i]  = 1;
                slot_ext_id[i]  = (net_cfg.car_index[i] >= 0) ? net_cfg.car_index[i] : 0;
                slot_variant[i] = net_cfg.paint_index[i];
            }
            /* The AI fill below must not overwrite the net players' slots
             * (it used to start at slot 1 and stomp every client's car). */
            if (np > start_slot) start_slot = np;
            /* [S31] TD6 body colours ride the config too: the asset painter
             * otherwise colours slot 0 with the LOCAL machine's INI choice
             * and other humans with the AI hash -- every machine rendered
             * its own idea of the field ("client sees the same car twice,
             * host sees correctly"). */
            for (i = 0; i < TD5_MAX_RACER_SLOTS; i++)
                td5_asset_set_human_td6_color(
                    i, (i < np && i < 6) ? net_cfg.td6_color[i] : -1);
            for (i = 0; i < np && i < 6; i++)
                TD5_LOG_I(LOG_TAG,
                          "InitRaceSchedule: net slot%d car=%d paint=%d color=%06X",
                          i, net_cfg.car_index[i], net_cfg.paint_index[i],
                          (unsigned)net_cfg.td6_color[i]);
            /* Opponent count is host-authoritative: it decides how many racer
             * slots InitRace enables -- a mismatch is a different grid.
             * InitRace computes the field as humans(1) + num_ai_opponents, so
             * fold the EXTRA net players into the opponent count: the field
             * is np humans + the host-chosen AI cars. */
            /* [2026-06-19] The grid is sized num_human_players + num_ai_opponents.
             * For net, THIS machine has exactly ONE local human viewport; every
             * OTHER player is folded into the opponent count via (np-1) below. But
             * g_td5.num_human_players was set to `humans` (= the joined count, e.g.
             * 2) above, so the other humans were counted TWICE -> an extra phantom
             * AI car ("0 opponents selected but I see 1"; 3 cars for a 2-player
             * race). Force the local human count to 1 so the field is exactly
             * np humans + the host-chosen AI cars. */
            g_td5.num_human_players = 1;
            g_td5.num_ai_opponents = net_cfg.num_opponents + (np - 1);
            if (g_td5.num_ai_opponents > TD5_MAX_RACER_SLOTS - 1)
                g_td5.num_ai_opponents = TD5_MAX_RACER_SLOTS - 1;
            TD5_LOG_I(LOG_TAG,
                      "InitRaceSchedule: net config applied (np=%d opp=%d seed=0x%08X)",
                      np, net_cfg.num_opponents, net_cfg.rng_seed);
        } else {
            TD5_LOG_W(LOG_TAG,
                      "InitRaceSchedule: network race WITHOUT a host config");
        }
    }

    /* In-race per-viewport identity (coloured frame + name plate): cleared for
     * every race, populated below only for the multiplayer flow. */
    td5_hud_clear_player_identities();

    /* [PORT ENHANCEMENT 2026-06] Multiplayer lobby flow: each human slot uses the
     * car that player chose in the sequential car select. ([S31] skipped for
     * net races: a stale local flag would overwrite the replicated grid.) */
    if (s_mp_flow && !g_td5.network_active) {
        slot_ext_id[0]  = s_mp_player_car[0];
        slot_variant[0] = s_mp_player_paint[0];
        /* Each human slot is painted with that player's chosen TD6 colour (no-op
         * for TD5 cars, which have no carmask). -1 = leave the default. */
        td5_asset_set_human_td6_color(0, s_mp_player_color[0]);
        td5_hud_set_player_identity(0, s_mp_player_name[0], (uint32_t)s_mp_player_accent[0]);
        for (i = 1; i < eff_humans && i < TD5_MAX_RACER_SLOTS; i++) {
            slot_active[i]  = 1;
            slot_ext_id[i]  = s_mp_player_car[i];
            slot_variant[i] = s_mp_player_paint[i];
            td5_asset_set_human_td6_color(i, s_mp_player_color[i]);
            td5_hud_set_player_identity(i, s_mp_player_name[i], (uint32_t)s_mp_player_accent[i]);
            if (i + 1 > start_slot) start_slot = i + 1;
        }
        /* AI / unused slots get the hashed AI palette (clear any stale override). */
        for (; i < TD5_MAX_RACER_SLOTS; i++)
            td5_asset_set_human_td6_color(i, -1);

        /* [MP SESSION PERSISTENCE 2026-06] Bulk-snapshot the just-committed local
         * roster into the process-lifetime store so re-entering the MP menu in the
         * same session restores each player's name/accent/car/paint/color/trans.
         * Local flow only (net rosters are host-replicated and must not be mixed
         * with this frontend store). Gated by TD5RE_MP_SESSION. */
        if (mp_session_enabled()) {
            int humans = g_td5.num_human_players;
            int p;
            if (humans < 0) humans = 0;
            if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;
            for (p = 0; p < humans; p++)
                mp_session_save_player(p);
            s_mp_session.valid = 1;
            s_mp_session.count = humans;
            TD5_LOG_I(LOG_TAG, "MP session: saved roster (%d players)", humans);
        }
    }

    /* Two-player setup [CONFIRMED @ 0x0040daf0]:
     * Original gate: g_twoPlayerModeEnabled != 0 || g_selectedGameType == 7.
     * In the original, game_type 7 is DRAG RACE (user picks a 2nd car via the
     * 2-pass CarSelect loop). The port's convention uses game_type 9 for drag
     * race, so the constant is swapped here. Time Trials is solo and must NOT
     * fall into this branch. (Skipped for the N-way multiplayer lobby flow,
     * which already populated the human slots above.) */
    if ((s_two_player_mode || s_selected_game_type == 9) && !s_mp_flow &&
        !g_td5.network_active) {
        slot_active[1]  = 1;
        slot_ext_id[1]  = s_p2_car;
        slot_variant[1] = 0;
        start_slot = 2;
        TD5_LOG_I(LOG_TAG, "InitRaceSchedule: P2 slot1 ext_id=%d", s_p2_car);
    }

    /* Quick Race extra humans: slots 1..eff_humans-1 are human-driven. Until a
     * per-player car-select UI exists (the next infra step), the extra humans
     * default to the player's selected car. AI then fills from start_slot; the
     * remaining slots beyond (humans+opponents) are disabled in td5_game InitRace. */
    if (s_current_screen == TD5_SCREEN_QUICK_RACE) {
        for (i = 1; i < eff_humans && i < TD5_MAX_RACER_SLOTS; i++) {
            slot_active[i]  = 1;
            slot_ext_id[i]  = s_selected_car;
            slot_variant[i] = 0;
            if (i + 1 > start_slot) start_slot = i + 1;
        }
    }

    /* RNG state for AI ext_id picks.
     *
     * Original path (non-trace / live frontend):
     *   - InitializeFrontendResourcesAndState @ 0x00414740 calls srand(timeGetTime())
     *     TWICE, then burns 1+ rand() for the CD-track pick loop.
     *   - InitializeRaceSeriesSchedule @ 0x0040dac0 does NOT call srand —
     *     it only stores timeGetTime() into g_randomSeedForRace.
     *   - AI car picks consume _holdrand from that time-dependent state.
     *
     * Trace path (race_trace_enabled=1 + Frida --trace):
     *   - Frida hook (td5_quickrace_hook.js) calls _srand(0x1A2B3C4D) with
     *     ZERO preamble rand() calls before InitializeRaceSeriesSchedule().
     *   - Port calls srand(0x1A2B3C4D) with ZERO preamble burns.
     *   - Both sides start AI-car selection from rand #1 → identical picks.
     * [CONFIRMED @ td5_quickrace_hook.js:180-186, td5_quickrace.py:261] */
    if (net_cfg_valid) {
        /* [S31 NET] Every machine must run the AI car fill from the SAME
         * rand() stream: seed with the host-broadcast seed, no preamble burn
         * (every machine runs this exact path). The wall-clock srand below
         * used to run AFTER the net seed was applied and silently wiped it --
         * each machine then picked its own AI grid ("cars are different
         * between client and server"). The fixed-trace seed masked this in
         * the headless A/B runs by overriding both sides identically. */
        srand(net_cfg.rng_seed);
    } else if (g_td5.ini.race_trace_enabled) {
        /* Under race_trace_enabled the Frida quickrace hook calls
         * _srand(0x1A2B3C4D) IMMEDIATELY before InitializeRaceSeriesSchedule()
         * with zero preamble rand() calls between them (hook bypasses
         * InitializeFrontendResourcesAndState entirely).
         * [CONFIRMED @ re/tools/quickrace/td5_quickrace_hook.js:180-186]
         * [CONFIRMED @ re/tools/quickrace/td5_quickrace.py:261 — seed_crt=True
         *  is auto-set when --trace is passed to the Python launcher]
         *
         * The port must therefore start from rand #1 (no preamble burns) to
         * match the original's AI-car selection sequence. The 1-burn below
         * that approximates the CD-track-pick loop must be SKIPPED here —
         * it belonged to the non-trace path where InitializeFrontendResourcesAndState
         * fires and consumes 1+ rand() calls before InitializeRaceSeriesSchedule.
         *
         * Skipping 1 burn under race_trace_enabled aligns the rand() index
         * sequence for slots 1-5 with the Frida-captured original sequence,
         * enabling the same AI cars to be selected on both sides and thus
         * zero-delta spawn world_y at tick=0.
         * [CONFIRMED via disassembly of InitializeRaceSession @ 0x0042aa5f-0x0042aa80:
         *  srand(seed) at 0x42aa52 → 12 seed-table rand()s → 1 extra rand()
         *  → loading screen pick — all happen AFTER AI car selection] */
        srand(0x1A2B3C4D);
        /* No burn: Frida hook has no preamble rand() before schedule call */
    } else {
        srand(timeGetTime());
        /* Approximate the original's CD-track-pick rand() burn from
         * InitializeFrontendResourcesAndState @ 0x00414a78:
         *   do { rand() % 7; } while (== g_selectedCdTrackIndex);
         * With a fresh seed both sides start with cdTrack = -1 (default),
         * so the first rand() always exits the loop. One rand() consumed.
         * [CONFIRMED @ 0x00414740, 0x0040dac0, 0x0042aa33 (the real srand via
         *  mislabeled __set_new_handler).] */
        (void)rand();
    }

    if (s_selected_game_type == 2 && !g_td5.network_active) {
        /* === Path 1: Quick Race (gameType == 2, Era) [CONFIRMED @ 0x0040dac0] ===
         * Original loop body consumes THREE rand() calls per iteration:
         *   rand #1 -> ext_id (& 7, optionally +8 if player car > 7)
         *   rand #2 -> variant (& 3)
         *   rand #3 -> discard
         * On dedup collision, the whole block re-runs and consumes 3 more
         * rands. Port previously had variant outside the loop — see the
         * default path comment below for why that matters. */
        for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
            int ext_id;
            int variant;
            int attempts = 0;
            do {
                ext_id  = rand() & 7;                   /* rand #1 */
                variant = rand() & 3;                   /* rand #2 (variant) */
                (void)rand();                           /* rand #3 (discard) */
                if (s_selected_car > 7)
                    ext_id += 8;
                if (++attempts > 100) break; /* safety */
            } while (frontend_ai_ext_id_taken(ext_id, slot_ext_id, slot_active,
                                               TD5_MAX_RACER_SLOTS));
            slot_active[i]  = 1;
            slot_ext_id[i]  = ext_id;
            slot_variant[i] = variant;
            TD5_LOG_I(LOG_TAG, "InitRaceSchedule: quick-race slot%d ext_id=%d var=%d attempts=%d",
                      i, ext_id, slot_variant[i], attempts);
        }
    } else if (s_selected_game_type == 5 && !g_td5.network_active) {
        /* === Path 2: Cup/Masters (gameType == 5) [CONFIRMED @ 0x0040dac0] ===
         * Scans s_masters_roster_flags[] for state==1 entries, claims them (sets to 2),
         * reads ext car id from s_masters_roster[]. */
        for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
            int found = 0;
            for (int j = 0; j < 15; j++) {
                if (s_masters_roster_flags[j] == 1) {
                    s_masters_roster_flags[j] = 2; /* claimed */
                    slot_active[i]  = 1;
                    slot_ext_id[i]  = s_masters_roster[j];
                    slot_variant[i] = rand() % 3;
                    found = 1;
                    TD5_LOG_I(LOG_TAG, "InitRaceSchedule: cup slot%d roster[%d] ext_id=%d var=%d",
                              i, j, slot_ext_id[i], slot_variant[i]);
                    break;
                }
            }
            if (!found) {
                TD5_LOG_W(LOG_TAG, "InitRaceSchedule: cup slot%d no roster entry available", i);
            }
        }
    } else {
        /* === Path 3: Default (single race, all other types) [CONFIRMED @ 0x0040dac0] ===
         * Faithful port of the original's loop structure. Each iteration of
         * the outer do/while body consumes THREE rand() calls in a specific
         * order:
         *   rand #1 -> tier_idx (mod 6 into row[gRaceDifficultyTier])
         *   rand #2 -> variant  (& 3)
         *   rand #3 -> discard  (return value thrown away)
         * When the chosen ext_id collides with an already-claimed slot, the
         * outer do/while RE-RUNS for the same slot — consuming ANOTHER 3
         * rand() calls. Only when the dedup check passes are the slot fields
         * written and the loop advances to the next slot.
         *
         * This is structurally distinct from the previous port version which
         * placed `slot_variant[i] = rand() & 3` OUTSIDE the do/while — on
         * collision retry, the previous port consumed 2 rands where the
         * original consumes 3, desyncing the rand() sequence.
         *
         * [CONFIRMED @ 0x0040dac0 body structure — decomp shows iVar6=rand();
         * uVar1=rand()&3; rand(); {dedup scan}; retry-on-collision path]. */
        int tier = g_td5.difficulty_tier;
        if (tier < 0 || tier > 2) tier = 2; /* clamp to valid tiers */

        /* S21: dynamic opponent pool bucketed by top speed. Build the sorted
         * pool (cached), take the band matching the difficulty tier, shuffle it
         * and walk it so opponents are distinct until the band is exhausted. */
        frontend_build_speed_pool();
        if (s_speed_pool_count > 0) {
            int lo, hi, band, seq;
            int band_ids[TD5_CAR_COUNT];
            frontend_speed_band_for_tier(tier, &lo, &hi);
            band = hi - lo;
            for (int k = 0; k < band; k++)
                band_ids[k] = s_speed_pool_ids[lo + k];
            /* Fisher-Yates shuffle so cars vary race-to-race (and which cars
             * within the band lead are not always the same). */
            for (int k = band - 1; k > 0; k--) {
                int r = rand() % (k + 1);
                int t = band_ids[k]; band_ids[k] = band_ids[r]; band_ids[r] = t;
            }
            seq = 0;
            for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
                int ext_id  = band_ids[seq % band];   /* distinct until wrap */
                int variant = rand() & 3;
                seq++;
                slot_active[i]  = 1;
                slot_ext_id[i]  = ext_id;
                slot_variant[i] = variant;
                TD5_LOG_I(LOG_TAG,
                          "InitRaceSchedule: speedpool slot%d tier=%d band=[%d,%d) ext_id=%d var=%d",
                          i, tier, lo, hi, ext_id, slot_variant[i]);
            }
        } else {
            /* Fallback (carparam unreadable): original hardcoded tier roster.
             * Faithful 3-rand-per-slot loop preserved for this path. */
            for (i = start_slot; i < TD5_MAX_RACER_SLOTS; i++) {
                int ext_id;
                int variant;
                int attempts = 0;
                do {
                    int tier_idx = rand() % 6;              /* rand #1 */
                    variant      = rand() & 3;              /* rand #2 (variant) */
                    (void)rand();                           /* rand #3 (discard) */
                    ext_id = s_difficulty_tier_cars[tier][tier_idx];
                    if (++attempts > 100) break;
                } while (frontend_ai_ext_id_taken(ext_id, slot_ext_id, slot_active,
                                                   TD5_MAX_RACER_SLOTS));
                slot_active[i]  = 1;
                slot_ext_id[i]  = ext_id;
                slot_variant[i] = variant;
                TD5_LOG_I(LOG_TAG, "InitRaceSchedule: default slot%d tier=%d ext_id=%d var=%d attempts=%d",
                          i, tier, ext_id, slot_variant[i], attempts);
            }
        }
    }

    /* Store ext_ids directly as car indices.
     * s_car_zip_paths is indexed by ext_id (display order), NOT by the original
     * binary's gCarZipPathTable type_index. The s_ext_car_to_type_index conversion
     * is NOT applied here — it maps to the original binary's table ordering which
     * doesn't match the source port's reordered table. */
    for (i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
        /* Accept any valid s_car_zip_paths index (TD5 0..32 + TD6 37..75). The
         * S21 speed pool can assign TD6 cars (ext_id >= 37) as opponents; the
         * old `< 37` bound clamped those to VIPER. The cup/masters/quick-race
         * paths only ever produce TD5 ext_ids, so widening is a no-op for them. */
        if (slot_active[i] && slot_ext_id[i] >= 0 && slot_ext_id[i] < TD5_CAR_COUNT) {
            g_td5.ai_car_indices[i]  = slot_ext_id[i];
            g_td5.ai_car_variants[i] = slot_variant[i];
        } else {
            g_td5.ai_car_indices[i]  = 0; /* fallback: VIPER (ext_id 0) */
            g_td5.ai_car_variants[i] = 0;
        }
    }

    /* Commit the PLAYER's selected paint (slot 0). The AI loop above starts at
     * slot 1, so without this the player's chosen colour is dropped and the car
     * always loads carskin0 (the default). The per-slot variant table mirrors
     * the original's gSlotCarIdSelectionTable[0] = g_player1SelectedPaintScheme
     * [CONFIRMED @ 0x0040DADC → built into "CARSKIN%d.TGA" @ 0x00442949]. */
    g_td5.ai_car_variants[0] = (slot_variant[0] >= 0 && slot_variant[0] <= 3)
                               ? slot_variant[0] : 0;
    /* [S31] Slot 0's CAR index too: net races load every slot from
     * ai_car_indices (g_td5.car_index is the LOCAL player's pick, which on a
     * client is not slot 0). Non-net keeps loading slot 0 from car_index. */
    g_td5.ai_car_indices[0] = (slot_ext_id[0] >= 0 && slot_ext_id[0] < TD5_CAR_COUNT)
                              ? slot_ext_id[0] : 0;

    TD5_LOG_I(LOG_TAG, "InitializeRaceSeriesSchedule: car=%d (resolved=%d) track=%d level=%d screen=%d type=%d ai=[%d,%d,%d,%d,%d]",
              s_selected_car, g_td5.car_index, g_td5.track_index,
              td5_asset_level_number(g_td5.track_index),
              s_current_screen, s_selected_game_type,
              g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
              g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
              g_td5.ai_car_indices[5]);
}

/* ========================================================================
 * Auto-race setup — bypass frontend, configure race from INI values
 *
 * Called from td5_game.c when AutoRace=1 is set. Sets up the race schedule
 * using INI defaults (DefaultCar, DefaultTrack, DefaultGameType, game options)
 * without requiring any user input or frontend navigation.
 * ======================================================================== */

 int ConfigureGameTypeFlags(void);  /* forward decl */
 void frontend_init_display_mode_state(void);  /* forward decl */

/* Matches the sequence in re/tools/quickrace/td5_quickrace_hook.js:
 * stamp race globals, then ConfigureGameTypeFlags -> InitializeRaceSeriesSchedule
 * -> InitializeFrontendDisplayModeState. Any divergence from the original's
 * attract-mode demo path reappears as a sim_tick=1 spawn-state mismatch in
 * /diff-race, so keep this in lockstep with the Frida hook. */
void td5_frontend_auto_race_setup(void) {
    /* Apply INI values to frontend statics (normally done in init_resources) */
    s_selected_car       = g_td5.ini.default_car;
    s_selected_track     = g_td5.ini.default_track;
    s_selected_game_type = g_td5.ini.default_game_type;
    s_selected_paint     = 0;
    s_selected_transmission = 0;
    s_track_direction    = g_td5.ini.default_reverse ? 1 : 0;
    g_td5.reverse_direction = s_track_direction;
    TD5_LOG_I(LOG_TAG, "AutoRace: track_direction=%s (DefaultReverse=%d)",
              s_track_direction ? "Backwards" : "Forwards",
              g_td5.ini.default_reverse);

    /* Apply game options from INI */
    s_game_option_laps              = g_td5.ini.laps;
    s_game_option_checkpoint_timers = g_td5.ini.checkpoint_timers;
    s_game_option_traffic           = g_td5.ini.traffic;
    s_game_option_cops              = g_td5.ini.cops;
    s_game_option_difficulty        = g_td5.ini.difficulty;
    s_game_option_dynamics          = g_td5.ini.dynamics;
    s_game_option_collisions        = g_td5.ini.collisions;

    /* Commit the dynamics (arcade/sim) selection into the physics race-init
     * flag deterministically for the AutoRace path, mirroring the options-screen
     * commit at ConfigureGameTypeFlags (td5_physics_set_dynamics @ case 0). The
     * boot path also commits the INI value, but committing here makes the
     * AutoRace harness independent of boot-block ordering. */
    td5_physics_set_dynamics(s_game_option_dynamics);

    /* Match the Frida hook's pre-call writes.
     *   g_twoPlayerModeEnabled=0, g_returnToScreenIndex=-1
     *   s_current_screen pinned to MAIN_MENU so frontend_init_race_schedule
     *   takes the selected-track branch instead of s_attract_track. */
    s_two_player_mode = 0;
    s_return_screen   = -1;
    s_current_screen  = TD5_SCREEN_MAIN_MENU;

    /* Configure game type flags (sets g_td5.game_type, traffic, etc.) */
    ConfigureGameTypeFlags();

    /* Now trigger the race schedule (sets race_requested, assigns AI cars) */
    frontend_init_race_schedule();

    /* AutoRace opponent-count override (test harness / Quick Race-field probe):
     * the schedule's non-QuickRace path defaults to 1 human + 5 AI; honor an
     * explicit [Game] DefaultOpponents=N here so AutoRace can exercise the
     * reduced-field spawn path. -1 = leave the full grid. */
    /* AutoRace player/opponent override (test harness for N-way split, since
     * AutoRace skips the Quick Race menu). DefaultPlayers sets the local human
     * count (>=2 enables split); DefaultOpponents sets the AI count. Either at
     * its sentinel leaves the schedule's value. */
    if (g_td5.ini.default_players >= 1 || g_td5.ini.default_opponents >= 0) {
        int humans = (g_td5.ini.default_players >= 1) ? g_td5.ini.default_players
                                                      : g_td5.num_human_players;
        if (humans < 1) humans = 1;
        if (humans > TD5_MAX_HUMAN_PLAYERS) humans = TD5_MAX_HUMAN_PLAYERS;
        int opp = (g_td5.ini.default_opponents >= 0) ? g_td5.ini.default_opponents
                                                     : g_td5.num_ai_opponents;
        if (opp < 0) opp = 0;
        if (opp > TD5_MAX_RACER_SLOTS - humans) opp = TD5_MAX_RACER_SLOTS - humans;
        g_td5.num_human_players = humans;
        g_td5.num_ai_opponents  = opp;
        if (humans >= 2 && g_td5.split_screen_mode == 0)
            g_td5.split_screen_mode = 1;   /* enable N-way split for the harness */
        TD5_LOG_I(LOG_TAG,
                  "AutoRace: player override -> %d humans + %d AI (split=%d)",
                  humans, opp, g_td5.split_screen_mode);
    }

    /* [2026-06-08] AutoRace AI-spectator split-screen override (dev profiling).
     * AutoRace skips the Quick Race "AI Screens" selector, so [Game]
     * SpectateScreens=N forces N AI cars (slots 1..N) into their own panes for a
     * deterministic split-screen render-load measurement. Recomputes the split
     * grid from the resulting pane count so the viewport/HUD/divider layout all
     * agree (same resolver the menu path uses). */
    if (g_td5.ini.spectate_screens > 0) {
        frontend_commit_pane_layout(g_td5.num_human_players,
                                    g_td5.ini.spectate_screens);
        TD5_LOG_I(LOG_TAG,
                  "AutoRace: spectate override -> %d AI panes (total panes=%d split=%d grid=%dx%d)",
                  g_td5.num_spectate_screens,
                  g_td5.num_human_players + g_td5.num_spectate_screens,
                  g_td5.split_screen_mode,
                  g_td5.split_grid_cols, g_td5.split_grid_rows);
    }

    /* Enumerate display modes — original quickrace hook calls
     * InitializeFrontendDisplayModeState() right after the schedule init. */
    frontend_init_display_mode_state();

    TD5_LOG_I(LOG_TAG, "AutoRace: car=%d track=%d gameType=%d laps=%d diff=%d dyn=%d traffic=%d cops=%d coll=%d",
              g_td5.car_index, g_td5.track_index, s_selected_game_type,
              g_td5.circuit_lap_count, (int)g_td5.difficulty,
              g_td5.dynamics_mode, g_td5.traffic_enabled,
              g_td5.special_encounter_enabled, g_td5.ini.collisions);
}

static int frontend_find_display_mode_index(int width, int height, int bpp) {
    for (int i = 0; i < s_display_mode_count; i++) {
        if (s_display_modes[i].width == width &&
            s_display_modes[i].height == height &&
            s_display_modes[i].bpp == bpp) {
            return i;
        }
    }
    return -1;
}



int frontend_option_delta(void) {
    if (s_arrow_input & 1) return -1;
    if (s_arrow_input & 2) return 1;
    return 0;
}

static TD5_ScreenIndex frontend_get_parent_screen(TD5_ScreenIndex screen) {
    switch (screen) {
    case TD5_SCREEN_POSITIONER_DEBUG:
    case TD5_SCREEN_ATTRACT_MODE:
    case TD5_SCREEN_LANGUAGE_SELECT:
    case TD5_SCREEN_LEGAL_COPYRIGHT:
    case TD5_SCREEN_EXTRAS_GALLERY:
    case TD5_SCREEN_RACE_RESULTS:
        return TD5_SCREEN_MAIN_MENU;

    case TD5_SCREEN_MAIN_MENU:
    case TD5_SCREEN_LOCALIZATION_INIT:
    case TD5_SCREEN_STARTUP_INIT:
        return (TD5_ScreenIndex)-1;

    case TD5_SCREEN_RACE_TYPE_MENU:
    case TD5_SCREEN_QUICK_RACE:
    case TD5_SCREEN_CONNECTION_BROWSER:
    case TD5_SCREEN_OPTIONS_HUB:
        return TD5_SCREEN_MAIN_MENU;

    case TD5_SCREEN_SESSION_PICKER:
    case TD5_SCREEN_CREATE_SESSION:
    case TD5_SCREEN_NETWORK_LOBBY:
    case TD5_SCREEN_SESSION_LOCKED:
        return TD5_SCREEN_CONNECTION_BROWSER;

    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
        return TD5_SCREEN_OPTIONS_HUB;

    case TD5_SCREEN_CONTROLLER_BINDING:
        return TD5_SCREEN_CONTROL_OPTIONS;

    case TD5_SCREEN_MUSIC_TEST:
        return TD5_SCREEN_SOUND_OPTIONS;

    case TD5_SCREEN_CAR_SELECTION:
        if (s_network_active || s_previous_screen == TD5_SCREEN_NETWORK_LOBBY) {
            return TD5_SCREEN_NETWORK_LOBBY;
        }
        if (s_previous_screen == TD5_SCREEN_RACE_RESULTS) {
            return TD5_SCREEN_RACE_RESULTS;
        }
        if (s_flow_context == 2) {
            return TD5_SCREEN_QUICK_RACE;
        }
        if (s_flow_context == 3) {
            return TD5_SCREEN_MAIN_MENU;
        }
        return TD5_SCREEN_RACE_TYPE_MENU;

    case TD5_SCREEN_TRACK_SELECTION:
        if (s_flow_context == 2) {
            return TD5_SCREEN_QUICK_RACE;
        }
        return TD5_SCREEN_CAR_SELECTION;

    case TD5_SCREEN_HIGH_SCORE:
        /* [FIXED 2026-06-01] High Scores ALWAYS returns to MAIN MENU. The original
         * ScreenPostRaceHighScoreTable @0x00413580 has NO parent discriminator — case 6
         * unconditionally sets g_returnToScreenIndex=TD5_SCREEN_MAIN_MENU on OK/ESC, and
         * this screen is only ever reached from the main-menu Hi-Score button. The prior
         * `s_flow_context == 6 -> RACE_RESULTS` test was wrong: 6 is the main-menu
         * Hi-Score button's OWN context value (set at Screen_MainMenu :6676), so it
         * fired on EXACTLY the main-menu entry it must not, sending ESC/Back to RACE
         * RESULTS (OK was hardcoded to MAIN_MENU, hence the OK-vs-ESC asymmetry the user
         * saw). The port's post-race table is Screen_PostRaceNameEntry (NAME_ENTRY), which
         * never enters HIGH_SCORE and exits to MAIN_MENU on its own — so this case never
         * needs a RACE_RESULTS parent. [verified end-to-end vs 0x00413580.] */
        return TD5_SCREEN_MAIN_MENU;

    case TD5_SCREEN_NAME_ENTRY:
    case TD5_SCREEN_CUP_FAILED:
    case TD5_SCREEN_CUP_WON:
        return TD5_SCREEN_RACE_RESULTS;

    default:
        return TD5_SCREEN_MAIN_MENU;
    }
}

void frontend_init_return_screen(TD5_ScreenIndex screen) {
    s_return_screen = (int)frontend_get_parent_screen(screen);
}

void frontend_init_display_mode_state(void) {
    int width = 0;
    int height = 0;
    int bpp = 0;

    /* [ARCH-DIVERGENCE: DXDraw mode-table -> DXGI enum + label format; L5 sweep 2026-05-21]
     *   Ports BuildEnumeratedDisplayModeList (0x0040B100, walks dd_exref+0x34 in 0x14-byte
     *   rows filtered on +0x10) and FormatDisplayModeOptionStrings (0x0041D840, sprintf
     *   "%dx%dx%d" into 0x20-byte slots at g_displayModeStringTable). Port uses
     *   td5_plat_enum_display_modes (DXGI) and an annotated "%dx%d %dbpp" label (the
     *   extra space + "bpp" suffix is a deliberate UI improvement over the cramped
     *   orig label). DXDraw mode table doesn't exist under D3D11. */
    s_display_mode_count = td5_plat_enum_display_modes(s_display_modes, FE_MAX_DISPLAY_MODES);
    TD5_LOG_I(LOG_TAG, "Display modes enumerated: count=%d", s_display_mode_count);
    for (int i = 0; i < s_display_mode_count; i++) {
        snprintf(s_display_mode_names[i], sizeof(s_display_mode_names[i]),
            "%dx%d %dbpp",
            s_display_modes[i].width,
            s_display_modes[i].height,
            s_display_modes[i].bpp);
    }

    td5_plat_get_window_size(&width, &height);
    bpp = 16;
    if (s_display_mode_count > 0) {
        /* [CONFIRMED @ 0x004270A9 ScreenLocalizationInit] Original seeds
         * gConfiguredDisplayModeOrdinal = gSelectedDisplayModeOrdinal (loaded
         * from packed config), then reconciles by searching for 640x480x16
         * if the saved table mismatches the live enumerated list.
         *
         * Port: prefer the saved ordinal so the cursor reflects the user's
         * last choice when DisplayOptions opens. Fall back to the current
         * window size, then to entry 0. */
        int saved = td5_save_get_display_mode();
        int idx = -1;
        if (saved >= 0 && saved < s_display_mode_count) {
            idx = saved;
        } else {
            idx = frontend_find_display_mode_index(width, height, bpp);
        }
        if (idx >= 0) {
            s_display_mode_index = idx;
        } else if (s_display_mode_index < 0 || s_display_mode_index >= s_display_mode_count) {
            s_display_mode_index = 0;
        }
        TD5_LOG_I(LOG_TAG, "Display mode init: saved_ordinal=%d resolved_idx=%d (window=%dx%d)",
                  saved, s_display_mode_index, width, height);
    } else {
        s_display_mode_index = 0;
    }

    if (s_display_mode_count > 0) {
        TD5_DisplayMode *mode = &s_display_modes[s_display_mode_index];
        TD5_LOG_I(LOG_TAG, "Display mode selected: index=%d %dx%d %dbpp (not applied yet)",
                  s_display_mode_index, mode->width, mode->height, mode->bpp);
        /* Don't apply on init — only apply when entering fullscreen race or
         * when explicitly confirmed in Display Options. */
    } else {
        TD5_LOG_W(LOG_TAG, "No display modes enumerated; using fallback window mode");
    }

}

/* --- Input Polling --- */

static int frontend_is_window_active(void) {
    HWND hwnd = (HWND)(DWORD_PTR)Backend_GetDisplayWindow();
    HWND foreground = GetForegroundWindow();

    if (!hwnd) return 1;
    return (foreground == hwnd) ? 1 : 0;
}

 void frontend_post_quit(void);   /* defined later; used by the credits-skip path */
static void frontend_commit_text_input(void);  /* defined later; used by the name-entry confirm path (task#25) */

/* True if ANY keyboard key is currently down (DirectInput state buffer) or any
 * mapped gamepad nav/confirm bit is set this frame. Basis for "press any key to
 * exit the post-EXIT credits scroll". */
static int frontend_any_input_down(void) {
    const uint8_t *kb = td5_plat_input_get_keyboard();
    if (kb) {
        for (int i = 0; i < 256; i++)
            if (kb[i] & 0x80) return 1;
    }
    return s_fe_gamepad_nav ? 1 : 0;
}

/* [2026-06-15 TASK B] TD5RE_SHIFT_NAV (default ON; "0" reverts). When SHIFT is
 * held during a LEFT/RIGHT nav event, the arrow MOVES FOCUS horizontally (geometric
 * same-row pick) instead of CYCLING the focused selector's value. This is what lets
 * the keyboard reach the value-column buttons (e.g. Quick Race's Player AI / Auto-Thr,
 * which sit to the RIGHT of a selector whose plain LEFT/RIGHT is consumed by the value
 * cycler). Plain LEFT/RIGHT (no SHIFT) is untouched. */
static int frontend_shift_nav_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_SHIFT_NAV");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "SHIFT+Left/Right horizontal focus nav (TASK B) %s (TD5RE_SHIFT_NAV=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* True iff either SHIFT key is currently down AND the SHIFT-nav knob is on. Only
 * meaningful for keyboard-originated nav (gamepad presses won't have SHIFT held). */
static int frontend_shift_nav_active(void) {
    return frontend_shift_nav_on() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

/* [fix 2026-06-15] Single-click activation: a click on a hovered button selects
 * AND confirms it in one click. The legacy "first click selects, second confirms"
 * read as an inconsistent double-click. Knob TD5RE_MOUSE_SINGLE_CLICK (default on;
 * "0" restores the faithful select-then-confirm). */
static int frontend_mouse_single_click_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MOUSE_SINGLE_CLICK");
        /* DEFAULT OFF (user request 2026-06-15: double-click to activate). "1"/"y"
         * opts back into single-click activation. */
        v = (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) ? 1 : 0;
        TD5_LOG_I(LOG_TAG, "mouse single-click activate %s (TD5RE_MOUSE_SINGLE_CLICK=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Re-assert the software cursor whenever the mouse actually moves. Cursor
 * visibility is otherwise toggled only at a handful of screen-settle points
 * (main menu / car-select / track-select) and is HIDDEN during every slide
 * transition, so navigating to a screen that never explicitly re-shows it left
 * the pointer invisible — the reported "cursor disappears after navigating a
 * bit". Mouse motion now brings it back on any frontend screen.
 * Knob TD5RE_CURSOR_ON_MOVE (default on; "0" reverts to screen-driven only). */
static int frontend_cursor_on_move_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_CURSOR_ON_MOVE");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "cursor-on-move %s (TD5RE_CURSOR_ON_MOVE=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Apply ONE menu navigation event — a single cursor move or confirm. Shared by
 * the gamepad immediate-edge path and the keyboard WM_KEYDOWN FIFO drain so both
 * behave identically: exactly one move per press, matching the original's
 * one-move-per-rising-edge handler UpdateFrontendDisplayModeSelection @0x00426580.
 * Routing keyboard nav through the queue (one call per queued press, in order)
 * is what makes a burst of taps during an MS spike apply press-for-press instead
 * of being collapsed/dropped. The s_arrow_input bit is set regardless of the
 * color-panel modal (the modal reads those bits in the tick); only the button
 * move itself is suppressed while the panel is open. */
static void frontend_apply_nav_event(unsigned code) {
    int spatial_nav = (s_current_screen == TD5_SCREEN_CONTROLLER_BINDING);
    /* [fix 2026-06-15] Quick Race uses GEOMETRIC up/down nav so the dev "Span
     * Offset" button (high button index but a low visual row) is reachable by
     * pressing DOWN from "AI Screens" — index-order nav skipped it. LEFT/RIGHT in
     * QR still cycle the focused selector's value (handled in the QR FSM). */
    /* [2026-06-16] CREATE_SESSION also needs GEOMETRIC up/down: the direct-host
     * UPnP toggle + GAME PORT field are high button indices (5/6) sitting at low
     * visual rows (between PASSWORD and HOST), so index-order nav skipped them --
     * same class as the Quick Race span-offset field. LEFT/RIGHT stay value-cycle
     * (spatial_nav unchanged), so the UPnP/MAX selectors still adjust. */
    int vnav = spatial_nav
            || (s_current_screen == TD5_SCREEN_QUICK_RACE)
            || (s_current_screen == TD5_SCREEN_CREATE_SESSION);
    switch (code) {
    case TD5_NAVKEY_LEFT:
        /* [TASK B] SHIFT+LEFT = horizontal focus move (geometric same-row pick),
         * NOT a value cycle: skip the arrow bit so frontend_option_delta stays 0.
         * Suppressed while the colour panel is open (that modal reads the arrow
         * bits to move its swatch cursor), so it falls through to plain behavior. */
        if (frontend_shift_nav_active() && !s_color_panel_visible) {
            if (frontend_move_selected_button_spatial(-1, 0)) {
                frontend_play_sfx(2); s_selection_from_mouse = 0;
            }
            break;
        }
        s_arrow_input |= 1;
        /* [2026-06-15 BUG #13] Plain LEFT (no SHIFT) ONLY cycles the focused
         * selector's value (via the s_arrow_input bit, consumed in the tick). It
         * must NOT also move focus horizontally — that displaced focus onto the
         * value-column button to the right while cycling. Horizontal focus move now
         * requires SHIFT (handled above). The spatial_nav (Controller-Binding)
         * screen has NO value selectors, so plain LEFT there still legitimately
         * moves focus geometrically. Gated by TD5RE_SHIFT_NAV: "0" restores the old
         * cycle-and-move behavior. */
        if (!s_color_panel_visible && (spatial_nav || !frontend_shift_nav_on())) {
            int moved = spatial_nav ? frontend_move_selected_button_spatial(-1, 0)
                                    : frontend_cycle_selected_button_horizontal(-1);
            if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
        }
        break;
    case TD5_NAVKEY_RIGHT:
        /* [TASK B] SHIFT+RIGHT = horizontal focus move (geometric same-row pick).
         * Suppressed while the colour panel is open (see LEFT above). */
        if (frontend_shift_nav_active() && !s_color_panel_visible) {
            if (frontend_move_selected_button_spatial(1, 0)) {
                frontend_play_sfx(2); s_selection_from_mouse = 0;
            }
            break;
        }
        s_arrow_input |= 2;
        /* [2026-06-15 BUG #13] Plain RIGHT only cycles the value; horizontal focus
         * move requires SHIFT (see LEFT above). */
        if (!s_color_panel_visible && (spatial_nav || !frontend_shift_nav_on())) {
            int moved = spatial_nav ? frontend_move_selected_button_spatial(1, 0)
                                    : frontend_cycle_selected_button_horizontal(1);
            if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
        }
        break;
    case TD5_NAVKEY_UP:
        s_arrow_input |= 4;
        if (!s_color_panel_visible) {
            int moved = vnav ? frontend_move_selected_button_spatial(0, -1)
                             : frontend_cycle_selected_button_vertical(-1);
            if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
            /* No sound on a no-target edge move — faithful to the original shared
             * nav handler @0x00426580, which is SILENT when no neighbour exists. */
        }
        break;
    case TD5_NAVKEY_DOWN:
        s_arrow_input |= 8;
        if (!s_color_panel_visible) {
            int moved = vnav ? frontend_move_selected_button_spatial(0, 1)
                             : frontend_cycle_selected_button_vertical(1);
            if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
        }
        break;
    case TD5_NAVKEY_ENTER:
        if (s_selected_button >= 0 && s_selected_button < FE_MAX_BUTTONS &&
            s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
            s_button_index = s_selected_button;
            s_input_ready = 1;
            frontend_play_sfx(3);
            TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=keyboard",
                      s_button_index, s_buttons[s_button_index].label);
        } else if (s_text_input_state == 1) {
            /* [task#25 NAME-ENTRY LOCKED SFX] A confirm (gamepad A) arrived while a
             * text field is actively accepting input — e.g. the post-race high-score
             * NAME entry (Screen_PostRaceNameEntry case 2), which has NO frontend
             * buttons during the typing phase. The keyboard FIFO that feeds this
             * function is suppressed while a field is open (see frontend_poll_input
             * `if (s_text_input_state == 0)`), so ONLY the gamepad edge path reaches
             * here during name entry. Falling through to the `else` below played
             * frontend_play_sfx(10) = "Uh-Oh.wav" — the LOCKED/error cue — on every
             * joystick press, the wrong feedback. Confirm the field instead (so the
             * pad can submit the name) with the normal confirm tick, matching the
             * keyboard Enter path in frontend_handle_text_input_key. Gate:
             * TD5RE_NAMEENTRY_FIX (default on; "0" restores the old locked sound). */
            static int s_nameentry_fix = -1;
            if (s_nameentry_fix < 0) {
                const char *e = getenv("TD5RE_NAMEENTRY_FIX");
                s_nameentry_fix = (e && e[0] == '0') ? 0 : 1;
                TD5_LOG_I(LOG_TAG, "name-entry confirm SFX fix (task#25): %s",
                          s_nameentry_fix ? "on (pad confirms field, no locked sfx)"
                                          : "off (legacy locked sfx)");
            }
            if (s_nameentry_fix) {
                frontend_commit_text_input();
                frontend_play_sfx(3);  /* normal confirm tick, not the locked cue */
            } else if (s_current_screen != TD5_SCREEN_EXTRAS_GALLERY) {
                frontend_play_sfx(10);
            }
        } else if (s_current_screen != TD5_SCREEN_EXTRAS_GALLERY) {
            frontend_play_sfx(10);
        }
        /* else: the post-EXIT credits scroll has no buttons, so a confirm press
         * has nothing to action — the any-key skip block in the poll quits. */
        break;
    default: break;
    }
}

static void frontend_poll_input(void) {
    POINT pt;
    HWND hwnd;
    SHORT mouse_state;
    int mouse_btn;
    int mouse_moved;
    int left_now;
    int right_now;
    int up_now;
    int down_now;
    int enter_now;
    int left_edge;
    int right_edge;
    int up_edge;
    int down_edge;
    int enter_edge;
    int had_activity = 0;
    uint32_t now = td5_plat_time_ms();

    /* [2026-06-05] Menu nav is now drained from the WM_KEYDOWN FIFO further down
     * (one move per queued press, in order — see frontend_apply_nav_event), which
     * is what stops a burst of taps during an MS spike from being collapsed/
     * dropped. The queue is flushed in the inactive branch below so presses
     * captured while the window was unfocused are discarded, not fired on refocus. */

    s_input_ready = 0;
    s_button_index = -1;
    s_arrow_input = 0;

    hwnd = (HWND)(DWORD_PTR)Backend_GetDisplayWindow();
    if (!frontend_is_window_active()) {
        /* Discard any nav presses captured while unfocused so they don't fire on
         * refocus (the old code achieved this by draining the latch every frame). */
        td5_plat_input_flush_nav();
        s_prev_left_state = 1;
        s_prev_right_state = 1;
        s_prev_up_state = 1;
        s_prev_down_state = 1;
        s_prev_enter_state = 1;
        s_prev_escape_state = 1;
        s_prev_mouse_btn = 0;
        s_mouse_clicked = 0;
        s_mouse_click_latched = 0;
        return;
    }

    /* Keyboard arrows/Enter — read for activity & active-device tracking ONLY.
     * The actual menu MOVES now come exclusively from the WM_KEYDOWN FIFO drained
     * below (frontend_apply_nav_event), so a burst of taps during an MS spike is
     * applied press-for-press, in order. Feeding these immediate reads into the
     * move edges as well would double-count each keyboard press. */
    int kb_left  = td5_plat_input_key_pressed(0xCB);
    int kb_right = td5_plat_input_key_pressed(0xCD);
    int kb_up    = td5_plat_input_key_pressed(0xC8);
    int kb_down  = td5_plat_input_key_pressed(0xD0);
    int kb_enter = td5_plat_input_key_pressed(0x1C);

    /* Active-controller tracking: keyboard input keeps device 0 active. */
    if (kb_left || kb_right || kb_up || kb_down || kb_enter)
        s_active_menu_device = 0;

    /* The move-edge path below is GAMEPAD-ONLY now (keyboard flows via the FIFO);
     * the gamepad block just after this OR's its dpad/stick state into these. */
    left_now = right_now = up_now = down_now = enter_now = 0;

    /* [PORT ENHANCEMENT 2026-06; PERF FIX 2026-06-05] Hot-swap detection:
     * re-enumerate DirectInput joysticks when a controller is connected while
     * sitting on a menu. This is now EVENT-DRIVEN off WM_DEVICECHANGE instead of
     * a frame timer: IDirectInput8::EnumDevices is a ~120ms BLOCKING HID
     * enumeration (it does NOT early-out cheaply — the count compare only gates
     * the handle rebuild, not the enumeration), and the old "every ~90 frames"
     * throttle assumed 60fps but fired ~2x/sec at high refresh, hitching the menu
     * ~120ms each time (the dominant frontend frame spike per the profiler).
     * Gating on the OS device-change signal makes the steady-state cost zero. */
    if (td5_plat_input_devices_changed())
        td5_plat_input_rescan_devices();

    /* [PORT ENHANCEMENT 2026-06] Gamepad navigation: ANY connected joystick's
     * dpad/left stick moves the cursor, A (button 0) confirms, B (button 1) backs
     * out. OR'd into the keyboard state so the existing edge-detection debounces
     * it; the joystick that pressed A becomes the active controller. */
    s_fe_gamepad_nav = td5_plat_input_frontend_nav();
    if (s_fe_gamepad_nav & 0x01) left_now  = 1;
    if (s_fe_gamepad_nav & 0x02) right_now = 1;
    if (s_fe_gamepad_nav & 0x04) up_now    = 1;
    if (s_fe_gamepad_nav & 0x08) down_now  = 1;
    if (s_fe_gamepad_nav & 0x10) enter_now = 1;   /* A = confirm/select */
    if (s_fe_gamepad_nav) {
        int aj = td5_plat_input_active_joystick();
        if (aj >= 1) s_active_menu_device = aj;
    }

    /* Gamepad-only rising edges (keyboard nav comes from the FIFO, drained below). */
    left_edge = (left_now && !s_prev_left_state);
    right_edge = (right_now && !s_prev_right_state);
    up_edge = (up_now && !s_prev_up_state);
    down_edge = (down_now && !s_prev_down_state);
    enter_edge = (enter_now && !s_prev_enter_state);
    if (kb_left || kb_right || kb_up || kb_down || kb_enter ||
        left_now || right_now || up_now || down_now || enter_now) had_activity = 1;

    /* s_arrow_input bits are set per-event inside frontend_apply_nav_event (for
     * both the gamepad edges and the queued keyboard presses), so the color-panel
     * modal and the value-cycle consumers still see them. */

    /* Mouse position */
    GetCursorPos(&pt);
    if (hwnd) ScreenToClient(hwnd, &pt);

    /* Scale mouse coords from window client area to game space (640x480) */
    {
        RECT rc;
        int ww = 640, wh = 480;
        if (hwnd && GetClientRect(hwnd, &rc)) {
            ww = rc.right - rc.left;
            wh = rc.bottom - rc.top;
        }
        if (ww > 0 && wh > 0) {
            s_mouse_x = pt.x * 640 / ww;
            s_mouse_y = pt.y * 480 / wh;
        }
    }
    if (s_mouse_x < 0) s_mouse_x = 0;
    if (s_mouse_x > 639) s_mouse_x = 639;
    if (s_mouse_y < 0) s_mouse_y = 0;
    if (s_mouse_y > 479) s_mouse_y = 479;
    mouse_moved = (s_mouse_x != s_prev_mouse_x || s_mouse_y != s_prev_mouse_y);
    if (mouse_moved) had_activity = 1;
    /* [fix 2026-06-15] Moving the mouse re-shows the cursor on every frontend
     * screen (see frontend_cursor_on_move_on). s_prev_mouse_{x,y} start at -1 so
     * the very first poll counts as "moved" once and asserts a visible cursor. */
    if (mouse_moved && frontend_cursor_on_move_on())
        frontend_set_cursor_visible(1);

    /* Mouse click: catch both held-edge presses and short clicks between frames. */
    mouse_state = GetAsyncKeyState(VK_LBUTTON);
    mouse_btn = (mouse_state & 0x8000) ? 1 : 0;
    if ((mouse_state & 0x0001) != 0) {
        s_mouse_click_latched = 1;
    }
    s_mouse_clicked = s_mouse_click_latched || (mouse_btn && !s_prev_mouse_btn);
    if (mouse_btn || s_mouse_clicked) had_activity = 1;
    s_prev_mouse_btn = mouse_btn;

    /* Menu navigation. UP/DOWN move between rows, LEFT/RIGHT within a horizontal
     * button row (the Controller-Binding grid uses 2D spatial nav). Both the
     * gamepad rising edges (this frame) and every keyboard press queued by the
     * window proc are funnelled through frontend_apply_nav_event so each press =
     * one move, in order.
     *
     * Gamepad edges first (sampled "now"): */
    if (left_edge)  frontend_apply_nav_event(TD5_NAVKEY_LEFT);
    if (right_edge) frontend_apply_nav_event(TD5_NAVKEY_RIGHT);
    if (up_edge)    frontend_apply_nav_event(TD5_NAVKEY_UP);
    if (down_edge)  frontend_apply_nav_event(TD5_NAVKEY_DOWN);
    if (enter_edge) frontend_apply_nav_event(TD5_NAVKEY_ENTER);

    /* Then drain the keyboard WM_KEYDOWN FIFO: one queued press = one move, in
     * order, so a burst of taps during a frame-time spike is applied press-for-
     * press instead of collapsed/dropped (the reported bug). Down,Down,Enter
     * mashed inside one slow frame thus selects the row two below — the right one.
     * Suppressed (queue flushed) while a text field is open so a buffered Enter
     * can't leak into name entry; text uses the WM_CHAR queue, not this path. */
    if (s_text_input_state == 0) {
        unsigned nav_code;
        int nav_guard = 0;
        while ((nav_code = td5_plat_input_nav_pop()) != 0 && nav_guard++ < 128) {
            frontend_apply_nav_event(nav_code);
            had_activity = 1;
        }
    } else {
        td5_plat_input_flush_nav();
    }

    /* [fix 2026-06-15] While SHIFT is held, SHIFT+Left/Right is reserved for
     * horizontal focus nav (apply_nav_event moves focus on the shift branch).
     * Clear any Left/Right CYCLE bits here so a queued arrow that drained a frame
     * after the shift-state check can't ALSO cycle the value (the reported
     * "shift+arrow still cycles AND navigates"). Skipped while the colour panel is
     * open — it reads the arrow bits for its swatch cursor. */
    if (frontend_shift_nav_active() && !s_color_panel_visible)
        s_arrow_input &= ~(1u | 2u);

    /* Credits/extras scroll: press ANY key (or click / gamepad button) to skip
     * the credits and quit the game. Edge-detected against a key held over from
     * the menu so the YES press that opened the credits can't instantly exit.
     * [user request 2026-06-05: any key leaves the post-EXIT credits, no lock sfx] */
    {
        static int s_credits_anykey_prev = 0;
        int any_now = frontend_any_input_down();
        if (s_current_screen == TD5_SCREEN_EXTRAS_GALLERY &&
            ((any_now && !s_credits_anykey_prev) || s_mouse_clicked)) {
            TD5_LOG_I(LOG_TAG, "ExtrasGallery: input detected, quitting game");
            frontend_post_quit();
        }
        s_credits_anykey_prev = any_now;
    }
    s_prev_left_state = left_now;
    s_prev_right_state = right_now;
    s_prev_up_state = up_now;
    s_prev_down_state = down_now;
    s_prev_enter_state = enter_now;

    /* Hit-test buttons on mouse click.
     * Original UpdateFrontendDisplayModeSelection (0x426580) detects arrow zones:
     * when clicking on the already-selected button, if mouse is within 20px (0x14)
     * of the right edge, set arrow_input RIGHT; within 20px of left edge, set LEFT.
     * This enables mouse-driven left/right cycling on options screens. */
    if (s_mouse_clicked && frontend_mouse_single_click_on() && now < s_mouse_act_until) {
        /* [fix 2026-06-15] Absorb the 2nd click of a rapid double-click: with
         * single-click activation a double-click would otherwise fire TWICE — the
         * 2nd click bleeding into whatever button sits under the cursor on the next
         * screen ("button reacts the first time"). Swallow any click within the
         * debounce window armed by the activation below. Only arms after an actual
         * activation, so arrow-zone cycling / selection clicks are unaffected. */
        s_mouse_click_latched = 0;
    } else if (s_mouse_clicked) {
        /* Find which button the mouse is over */
        int hit_button = -1;
        for (int i = 0; i < FE_MAX_BUTTONS; i++) {
            if (!s_buttons[i].active || s_buttons[i].disabled) continue;
            if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
                s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
                hit_button = i;
                break;
            }
        }
        if (hit_button >= 0) {
            int i = hit_button;
            /* Arrow-zone edge cycling applies only to the already-selected button
             * (click within 20px of its left/right edge). */
            int arrow_zone = 0;
            if (i == s_selected_button) {
                if (s_mouse_x >= s_buttons[i].x + s_buttons[i].w - 0x14) {
                    arrow_zone = 1;   /* RIGHT */
                } else if (s_mouse_x < s_buttons[i].x + 0x14) {
                    arrow_zone = -1;  /* LEFT */
                }
            }
            if (arrow_zone != 0) {
                /* Arrow click: set arrow input directly, play feedback */
                if (arrow_zone > 0) s_arrow_input |= 2;  /* RIGHT */
                else                s_arrow_input |= 1;  /* LEFT */
                frontend_play_sfx(2);
            } else if (frontend_mouse_single_click_on()) {
                /* Single-click activation (opt-in: TD5RE_MOUSE_SINGLE_CLICK=1). */
                s_selected_button = i;
                s_selection_from_mouse = 1;
                s_button_index = i;
                s_input_ready = 1;
                s_mouse_flash_button = i;
                s_mouse_flash_until = now + 180;
                s_mouse_confirm_button = -1;
                s_mouse_act_until = now + 300;   /* a 2nd click within 300ms is a double-click, absorbed above */
                frontend_play_sfx(3);
                TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=mouse(1click)",
                          i, s_buttons[i].label);
            } else {
                /* [fix 2026-06-15] DEFAULT: DOUBLE-CLICK to activate. A single click
                 * SELECTS (highlights) the button; a 2nd click on the SAME button
                 * within the system double-click interval ACTIVATES it. */
                uint32_t dwin = GetDoubleClickTime();
                if (i == s_dbl_last_button && (uint32_t)(now - s_dbl_last_time) <= dwin) {
                    s_selected_button = i;
                    s_selection_from_mouse = 1;
                    s_button_index = i;
                    s_input_ready = 1;
                    s_mouse_flash_button = i;
                    s_mouse_flash_until = now + 180;
                    s_mouse_confirm_button = -1;
                    s_dbl_last_button = -1;   /* consume so a 3rd click doesn't re-fire */
                    frontend_play_sfx(3);
                    TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=mouse(dblclick)",
                              i, s_buttons[i].label);
                } else {
                    /* First click: select (highlight) + arm the double-click window. */
                    s_selected_button = i;
                    s_selection_from_mouse = 1;
                    s_dbl_last_button = i;
                    s_dbl_last_time = now;
                    frontend_play_sfx(2);
                }
            }
        }
        s_mouse_click_latched = 0;
    }

    /* Mouse hover: track hovered button but do NOT update selection.
     * Original UpdateFrontendDisplayModeSelection (0x426580) uses a separate
     * hover index (DAT_00498700) distinct from the selection index.
     * Hover only draws the green highlight border; the purple/gold selected
     * state changes on click only.  First click selects, second confirms. */
    if (mouse_moved) {
        s_mouse_hover_button = -1;
        for (int i = 0; i < FE_MAX_BUTTONS; i++) {
            if (!s_buttons[i].active || s_buttons[i].disabled) continue;
            if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
                s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
                s_mouse_hover_button = i;
                break;
            }
        }
        /* Original UpdateFrontendDisplayModeSelection (0x426580 @ 0x004268e2)
         * plays Play(1) = Ping3 each time the cursor enters a different button.
         * Gated on hover-index change AND new index >= 0 (cursor over a real
         * button). Empty-area moves don't emit anything. */
        if (s_mouse_hover_button >= 0 &&
            s_mouse_hover_button != s_prev_mouse_hover_button) {
            frontend_play_sfx(1);
        }
        s_prev_mouse_hover_button = s_mouse_hover_button;
    }
    s_prev_mouse_x = s_mouse_x;
    s_prev_mouse_y = s_mouse_y;

    /* Update highlight ramp for all buttons (original uses 6-step interpolation).
     * Selected button ramps up to 6, all others ramp down to 0, once per frame. */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) continue;
        if (i == s_selected_button) {
            if (s_buttons[i].highlight_ramp < 6) s_buttons[i].highlight_ramp++;
        } else {
            if (s_buttons[i].highlight_ramp > 0) s_buttons[i].highlight_ramp--;
        }
    }

    /* Arrow input also counts as ready */
    if (s_arrow_input) s_input_ready = 1;
    if (had_activity || s_input_ready) frontend_note_activity();
}

int frontend_check_escape(void) {
    if (!frontend_is_window_active()) {
        s_prev_escape_state = 1;
        td5_plat_input_esc_taken();   /* drop any ESC captured while unfocused */
        return 0;
    }
    /* Gamepad B (button 1) backs out, same as the ESC key. [PORT ENHANCEMENT] */
    int esc_now = td5_plat_input_key_pressed(0x01) || ((s_fe_gamepad_nav & 0x20) != 0);
    int esc_edge = (esc_now && !s_prev_escape_state);
    s_prev_escape_state = esc_now;
    /* Fold in a WM_KEYDOWN ESC the once-per-frame immediate read missed at low FPS
     * (read-and-clear). At most one back-out per frame so a buffered burst of ESCs
     * can't pop several screens at once. */
    int esc_queued = td5_plat_input_esc_taken();
    if (esc_edge || esc_queued) frontend_note_activity();
    return (esc_edge || esc_queued) ? 1 : 0;
}

/* Forward declaration for text rendering (defined later in file) */
static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
static void fe_draw_text_centered(float center_x, float y, const char *text,
                                  uint32_t color, float sx, float sy);
static void frontend_fill_rect(int layer, int x, int y, int w, int h, uint32_t color);
static void fe_draw_button_9slice(float bx, float by, float bw, float bh,
                                  int state, uint32_t interior, float sx, float sy);
static void fe_draw_button_frame(float bx, float by, float bw, float bh,
                                 int bb_state, float sx, float sy);
/* As fe_draw_button_frame but the SELECTED-state interior fill is `interior`
 * (0xAARRGGBB) instead of the default dark purple — lets a caller (the simul
 * car-select grid) tint the focused button with the player's accent colour. */
static void fe_draw_button_frame_fill(float bx, float by, float bw, float bh,
                                      int bb_state, uint32_t interior, float sx, float sy);
static int fe_draw_arrow_proc(float x, float y, float w, float h,
                              int dir_right, uint32_t color);
void fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
float fe_measure_small_text(const char *text);
static void fe_draw_option_arrows(int btn_idx, float sx, float sy);
 void frontend_load_bg_gallery(void);
/* Forward declarations for dialog text overlay renderers */
static void frontend_render_legal_copyright_overlay(float sx, float sy);
static void frontend_render_cup_failed_overlay(float sx, float sy);
static void frontend_render_cup_won_overlay(float sx, float sy);
 void frontend_render_session_locked_overlay(float sx, float sy);

/* S10: optional prompt shown in the text-input widget (empty -> "ENTER PLAYER
 * NAME"). Set AFTER frontend_begin_text_input (which resets it to default). */
char s_text_input_prompt[40] = "";

void frontend_begin_text_input(char *buffer, int capacity) {
    memset(&s_text_input_ctx, 0, sizeof(s_text_input_ctx));
    s_text_input_prompt[0] = '\0';   /* default prompt unless the screen sets one */
    if (!buffer || capacity <= 1) { s_text_input_state = 0; return; }
    buffer[capacity - 1] = '\0';
    s_text_input_ctx.buffer = buffer;
    s_text_input_ctx.capacity = capacity;
    s_text_input_ctx.caret = (int)strlen(buffer);
    s_text_input_ctx.blink_tick = td5_plat_time_ms();
    s_text_input_ctx.confirm_state = 0;
    s_text_input_state = 1;
    /* [FIX 2026-05-25 frontend-name-input] Drain any stale GetAsyncKeyState
     * latched bits for navigation keys so the Enter that selected the OK
     * button on the prior screen (Race Results) does not immediately confirm
     * the freshly-opened name input. GetAsyncKeyState(VK,&1) reports the
     * "depressed since last call" bit; without this drain, the very first
     * state-2 frame sees Enter still latched and calls
     * frontend_commit_text_input() before the user can type a character.
     * The 'PLAYER' default then becomes the auto-submitted name and the
     * screen flashes off so fast it reads as 'empty and auto-submits'. */
    (void)GetAsyncKeyState(VK_RETURN);
    (void)GetAsyncKeyState(VK_SPACE);
    (void)GetAsyncKeyState(VK_BACK);
    for (int vk = 0x30; vk <= 0x5A; vk++) (void)GetAsyncKeyState(vk);
    /* Discard any typed characters queued before this field opened (e.g. the
     * Enter that navigated here) so they don't auto-fill or auto-confirm. */
    td5_plat_input_flush_chars();
    TD5_LOG_I(LOG_TAG, "Text input started: capacity=%d initial=\"%s\"", capacity, buffer);
}

static void frontend_commit_text_input(void) {
    s_text_input_ctx.confirm_state = 1;
    s_text_input_state = 2;
    TD5_LOG_I(LOG_TAG, "Text input confirmed: \"%s\"",
              s_text_input_ctx.buffer ? s_text_input_ctx.buffer : "");
}

/* [2026-06-15 BUG #5] TD5RE_NAME_UPPERCASE (default ON; "0" allows mixed case).
 * The original name entry is uppercase only; lowercase typed via the PC keyboard
 * looks wrong. When ON, lowercase a-z typed into the keyboard name field below are
 * folded to A-Z before being stored in the name buffer. This affects only the
 * keyboard WM_CHAR path (player/session/profile name); the pad on-screen QWERTY
 * grid already inputs uppercase (k_mp_kbd_rows) and is unaffected. */
static int frontend_name_uppercase_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_NAME_UPPERCASE");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "Keyboard name entry forced UPPERCASE (#5) %s (TD5RE_NAME_UPPERCASE=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

void frontend_handle_text_input_key(void) {
    int len, ch;
    if (s_text_input_state != 1 || !s_text_input_ctx.buffer) return;
    if (!frontend_is_window_active()) return;
    len = (int)strlen(s_text_input_ctx.buffer);
    if (s_text_input_ctx.caret > len) s_text_input_ctx.caret = len;

    /* Drain the WM_CHAR queue. Windows already applied shift/caps/key-repeat and
     * queued every typed character, so every keystroke is processed regardless of
     * frame rate / input-poll contention (the old GetAsyncKeyState '&1' path
     * dropped keys whenever another poll consumed the "pressed since last call"
     * bit first — the source of the "hard to input" lag). */
    while ((ch = td5_plat_input_get_char()) != 0) {
        len = (int)strlen(s_text_input_ctx.buffer);

        if (ch == '\r' || ch == '\n') {            /* Enter = confirm */
            frontend_note_activity();
            frontend_commit_text_input();
            return;
        }
        if (ch == '\b') {                          /* Backspace */
            if (s_text_input_ctx.caret > 0) {
                memmove(&s_text_input_ctx.buffer[s_text_input_ctx.caret - 1],
                        &s_text_input_ctx.buffer[s_text_input_ctx.caret],
                        (size_t)(len - s_text_input_ctx.caret + 1));
                s_text_input_ctx.caret--;
                s_text_input_ctx.blink_tick = td5_plat_time_ms();
                frontend_note_activity();
                frontend_play_sfx(3);  /* keystroke tick [CONFIRMED @ 0x41A63D] */
            }
            continue;
        }
        if (ch == 0x1B || ch == '\t') continue;    /* Esc/Tab handled elsewhere */
        if (ch < 32 || ch > 126) continue;         /* printable ASCII only */
        if (len >= s_text_input_ctx.capacity - 1) continue;

        /* [BUG #5] Fold lowercase letters to uppercase so keyboard-typed names
         * match the original's uppercase-only name entry (knob-gated). */
        if (ch >= 'a' && ch <= 'z' && frontend_name_uppercase_on())
            ch -= ('a' - 'A');

        memmove(&s_text_input_ctx.buffer[s_text_input_ctx.caret + 1],
                &s_text_input_ctx.buffer[s_text_input_ctx.caret],
                (size_t)(len - s_text_input_ctx.caret + 1));
        s_text_input_ctx.buffer[s_text_input_ctx.caret] = (char)ch;
        s_text_input_ctx.caret++;
        s_text_input_ctx.blink_tick = td5_plat_time_ms();
        frontend_note_activity();
        frontend_play_sfx(3);          /* keystroke tick [CONFIRMED @ 0x41A63D] */
        TD5_LOG_I(LOG_TAG, "Text input char: '%c' -> \"%s\"",
                  (char)ch, s_text_input_ctx.buffer);
    }
}

/* DRAW ONLY. Input key handling lives in the screen handler (NAME_ENTRY case 2)
 * via frontend_handle_text_input_key(); this is called from the RENDER path
 * (td5_frontend_render_ui_rects) so the widget actually composites into the
 * presented frame — drawing it from the handler gets cleared before present. */
static void frontend_render_text_input(void) {
    if (!s_text_input_ctx.buffer) return;

    /* Compute scale locally so the widget sits in the same canvas-relative
     * location regardless of window size. */
    int screen_w = 0, screen_h = 0;
    td5_plat_get_window_size(&screen_w, &screen_h);
    if (screen_w <= 0 || screen_h <= 0) return;
    float sx = (float)screen_w / 640.0f;
    float sy = (float)screen_h / 480.0f;

    /* [FIXED 2026-06-01] Faithful name-entry input widget. The original
     * RenderFrontendCreateSessionNameInput @0x0041A530 renders the typed name plus a
     * blinking GREEN caret INSIDE the standard gold "ENTER PLAYER NAME" frontend button
     * (created by CreateFrontendDisplayModeButton(SNK_EnterPlayerNameButTxt,-0x1c0,0,
     * 0x1c0,0x40) → 448×64, auto-laid-out centered), NOT a separate opaque grey box.
     * The button prompt sits at the top; the typed name (left-aligned at button-local
     * x=0x14=20, clipped to 0x198) + caret in the field row below. The prior port's grey
     * box at (96,280) was a port invention; replaced with the gold 9-slice frame so this
     * matches the rest of the frontend (and netplay session-name input, which the original
     * draws with the SAME function). Caret = GREEN: orig fills it with BltColorFillToSurface
     * (0xff00,...) which byte-repacks to RGB565 0x07c0 = pure green (same packing as the
     * hover highlight, NOT raw 565). [pipeline 0x41A530 → gold button surface, keyed black]. */
    /* [FIXED 2026-06-02] name-input button rests at (120,193) for both Name Entry (25, decomp
     * @0x413BC0 slide settles iVar14-0x18*0x20+0x30a=120, iVar9-4*0x20+0xf0=193) and Create
     * Session (10) — was (110,192). w=0x1c0(448) h=0x40(64). */
    const float BTN_X = 120.0f, BTN_W = 448.0f, BTN_H = 64.0f, BTN_Y = 193.0f;
    float bx = BTN_X * sx, by = BTN_Y * sy, bw = BTN_W * sx, bh = BTN_H * sy;

    /* Field frame in the MODERN frontend-button style: this routes through the
     * SAME fe_draw_button_frame() the main button loop uses, so when VectorUI is
     * on the field gets the neon gold roundrect (matching the OK button beside
     * it) instead of the flat ButtonBits 9-slice that looked out of place. State
     * 0 = gold/selected because the field is the active input widget. Falls back
     * to the 9-slice frame automatically when VectorUI is off (helper internals).
     * Prompt + typed name + caret are drawn on top below, unchanged. */
    fe_draw_button_frame(bx, by, bw, bh, 0 /*gold/active*/, sx, sy);

    /* Prompt centered near the top of the button (screen-overridable). */
    fe_draw_text_centered(bx + bw * 0.5f, by + 8.0f * sy,
                          s_text_input_prompt[0] ? s_text_input_prompt : "ENTER PLAYER NAME",
                          0xFFFFFFFF, sx, sy);

    /* Typed name in the SMALL font, left-aligned at button-local x=0x14=20 [CONFIRMED
     * @0x41A530: DrawFrontendClippedStringToSurface(name,0x14,iVar3+0xc,surf) draws into the
     * button via g_smallFontSurface = g_smallTextSurface — the SMALL font, not BodyText].
     * Case preserved (true lowercase). */
    float field_x = bx + 20.0f * sx;
    float field_y = by + 38.0f * sy;
    fe_draw_small_text(field_x, field_y, s_text_input_ctx.buffer, 0xFFFFFFFF, sx, sy);

    /* Blinking GREEN caret right after the typed text (state 1 = accepting input).
     * Orig: BltColorFillToSurface(0xff00=green,...) 2px wide, blinks on animFrame&0x20;
     * port uses the 350ms blink clock.
     * [FIXED 2026-06-02] Draw via the IMMEDIATE fe_draw_quad path (same as the button
     * frame + typed text above) — the prior frontend_fill_rect() queued the caret into
     * the DEFERRED s_draw_queue, which re-applies the canvas scale at flush time, so the
     * already-scaled (caret_x, field_y) got scaled a SECOND time and the green bar landed
     * near the bottom of the window instead of beside the text. */
    if (s_text_input_state == 1 &&
        (((td5_plat_time_ms() - s_text_input_ctx.blink_tick) / 350U) & 1U) == 0U) {
        float name_w = fe_measure_small_text(s_text_input_ctx.buffer);
        float caret_x = field_x + name_w * fe_glyph_sx(sx, sy) + 1.0f * sx;
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(caret_x, field_y, 2.0f * sx, 12.0f * sy, 0xFF00FF00, -1, 0, 0, 0, 0);
    }
}

int frontend_text_input_confirmed(void) {
    return (s_text_input_ctx.confirm_state != 0) || (s_text_input_state == 2);
}

/* [S31] Drop any latched text-input confirm. confirm_state is set on commit
 * and was only ever cleared by the next frontend_begin_text_input, so a
 * screen that polls frontend_text_input_confirmed() OUTSIDE an active edit
 * (the network lobby's chat-submit check runs every interactive frame) saw
 * a confirm latched on a PRIOR screen -- e.g. the CREATE SESSION name edit --
 * as TRUE forever, looping the lobby through its chat-submit states and
 * eating ~2/3 of all button presses ("kick/start/nav don't work"). */
void frontend_reset_text_input(void) {
    s_text_input_state = 0;
    s_text_input_ctx.confirm_state = 0;
}

/* --- Cheat Code Detection --- */

static void frontend_push_cheat_char(char ch) {
    size_t len;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 'A' || ch > 'Z') return;
    len = strlen(s_cheat_key_history);
    if (len >= sizeof(s_cheat_key_history) - 1) {
        memmove(s_cheat_key_history, s_cheat_key_history + 1, sizeof(s_cheat_key_history) - 2);
        len = sizeof(s_cheat_key_history) - 2;
    }
    s_cheat_key_history[len] = ch;
    s_cheat_key_history[len + 1] = '\0';
}

static int frontend_match_cheat_code(const char *code) {
    size_t hist_len = strlen(s_cheat_key_history);
    size_t code_len = strlen(code);
    if (hist_len < code_len) return 0;
    return memcmp(s_cheat_key_history + hist_len - code_len, code, code_len) == 0;
}

static void frontend_update_cheat_codes(void) {
    static uint8_t s_prev_alpha[26];
    static uint32_t s_cheat_log_frame_counter = 0;
    int i;
    if (!frontend_is_window_active()) return;
    for (i = 0; i < 26; i++) {
        int vk = 'A' + i;
        int down = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
        if (down && !s_prev_alpha[i]) {
            frontend_note_activity();
            frontend_push_cheat_char((char)vk);
        }
        s_prev_alpha[i] = (uint8_t)down;
    }
    s_cheat_log_frame_counter++;
    if ((s_cheat_log_frame_counter % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "Cheat history check: \"%s\"", s_cheat_key_history);
    }
    if (frontend_match_cheat_code("OPENALL")) {
        s_cheat_unlock_all = !s_cheat_unlock_all;
        s_cheat_key_history[0] = '\0';
        TD5_LOG_I(LOG_TAG, "Cheat OPENALL %s", s_cheat_unlock_all ? "enabled" : "disabled");
        /* Refresh lock tables: when cheat is on, everything unlocked;
         * when cheat is off, revert to save file state */
        if (s_cheat_unlock_all) {
            memset(s_car_lock_table, 0, sizeof(s_car_lock_table));
            memset(s_track_lock_table, 0, sizeof(s_track_lock_table));
            s_total_unlocked_cars = 37;
            s_total_unlocked_tracks = 37;   /* incl. migrated TD6 slots 26-36 */
        } else {
            int t;
            td5_save_get_car_lock_table(s_car_lock_table, TD5_BASE_CAR_COUNT);
            td5_save_get_track_lock_table(s_track_lock_table, 26);
            /* TD6 migrated tracks (schedule slots 26-36) are always available
             * regardless of save progress — force-unlock so the loop below
             * raises s_total_unlocked_tracks to include them in the cycler. */
            { int td6s; for (td6s = 26; td6s <= 36; td6s++) s_track_lock_table[td6s] = 0; }
            if (td5_save_get_all_cars_unlocked()) {
                s_total_unlocked_cars = 37;
            } else {
                s_total_unlocked_cars = td5_save_get_max_unlocked_car();
                if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21;
            }
            s_total_unlocked_tracks = 20;
            for (t = 20; t < 37; t++) {
                if (s_track_lock_table[t] == 0)
                    s_total_unlocked_tracks = t + 1;
            }
        }
    }
}

/* --- Frontend Surface Recovery --- */

static void frontend_recover_surfaces(void) {
    int i;
    /* Re-upload all tracked surfaces from source metadata on device change */
    for (i = 0; i < FE_MAX_SURFACES; i++) {
        void *pixels = NULL;
        int w = 0, h = 0;
        if (!s_surfaces[i].in_use || !s_surfaces[i].source_name[0]) continue;

        /* Try PNG path first (saved during initial load) */
        if (s_surfaces[i].png_path[0]) {
            if (td5_asset_load_png_to_buffer(s_surfaces[i].png_path, TD5_COLORKEY_NONE,
                                              &pixels, &w, &h)) {
                /* The TD6 paint overlay is uploaded with its mask alpha binarised
                 * (see frontend_load_car_paint_overlay_surface) so the coloured
                 * body fully covers the gray base; re-apply that here or a resize
                 * would re-introduce the gray-bleed edge until the next reload. */
                if (strcmp(s_surfaces[i].source_name, "CarPicPaint0.tga") == 0) {
                    unsigned char *pp = (unsigned char *)pixels;
                    for (int k = 0; k < w * h; k++)
                        if (pp[k * 4 + 3] != 0) pp[k * 4 + 3] = 255;
                }
                s_surfaces[i].width = w;
                s_surfaces[i].height = h;
                td5_plat_render_upload_texture(s_surfaces[i].tex_page, pixels, w, h, 2);
                free(pixels);
                continue;
            }
        }

        TD5_LOG_W(LOG_TAG, "surface recovery: no PNG for %s", s_surfaces[i].source_name);
    }

    /* Phase 6: re-upload baked main-menu button cache textures so the
     * cached pages survive native-resolution / device reset events. */
    td5_fe_btncache_recover();
}
void frontend_post_quit(void) {
    g_td5.quit_requested = 1;
}

/* [S31] Post-race BACK TO LOBBY: pick the lobby this race was launched from. */
void td5_frontend_return_to_lobby(void) {
    if (s_network_active) {
        td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
    } else if (s_mp_flow) {
        td5_frontend_set_screen(TD5_SCREEN_MP_LOBBY);
    } else {
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
    }
}

/* [S31] Leaving a net race for the MAIN MENU = leaving the session (peers
 * were already told via RACE_LEFT and return to their lobby). */
void td5_frontend_leave_net_session(void) {
    if (s_network_active) {
        TD5_LOG_I(LOG_TAG, "leaving net session (quit to menu)");
        frontend_net_destroy();
        s_network_active = 0;
    }
}







/**
 * Destroy the network session and shut down.
 * Called when leaving network screens or on disconnect.
 */
void frontend_net_destroy(void) {
    td5_net_shutdown();
    s_network_active = 0;
}





/* ========================================================================
 * ConfigureGameTypeFlags (0x410CA0)
 *
 * Maps g_selectedGameType to runtime flags.
 * Returns 1 if configuration is valid, 0 if end-of-series.
 * ======================================================================== */

int ConfigureGameTypeFlags(void) {
    g_td5.game_type = (TD5_GameType)s_selected_game_type;
    g_td5.time_trial_enabled = 0;
    g_td5.wanted_mode_enabled = 0;
    g_td5.drag_race_enabled = 0;
    g_td5.traffic_enabled = 0;
    g_td5.special_encounter_enabled = 0;
    g_td5.race_rule_variant = 0;

    /* AI-car tier per original's ConfigureGameTypeFlags @ 0x00410CA0.
     * Mapping: 1/2→0, 3/4/5→1, 6/7→2. Cases 0/8/9 leave prior value
     * (default 2, set at boot to match original's .data init of
     * gRaceDifficultyTier @ 0x00463210). [CONFIRMED via Ghidra + Frida
     * runtime capture 2026-04-20.] */
    switch (s_selected_game_type) {
        case 1: case 2:             g_td5.difficulty_tier = 0; break;
        case 3: case 4: case 5:     g_td5.difficulty_tier = 1; break;
        case 6: case 7:             g_td5.difficulty_tier = 2; break;
        default:
            /* Game-types 0 (Single Race), 8 (Cop Chase), 9 (Drag): the original
             * ConfigureGameTypeFlags @ 0x00410CA0 does NOT write gRaceDifficultyTier
             * @ 0x00463210 for these cases (only 1-7 clobber it), so the committed
             * user-selected difficulty survives. Mirror that by routing the user's
             * Difficulty toggle into the tier here. This is the value read by:
             *   - InitializeRaceActorRuntime @ 0x00432E60 (AI rubber-band tier),
             *   - AdjustCheckpointTimersByDifficulty @ 0x0040A530 (checkpoint timers),
             *   - AI car selection (s_difficulty_tier_cars[tier]).
             * Previously this case left difficulty_tier at the boot default (2=Hard),
             * which is why the Difficulty toggle had no observable effect on gameplay.
             * [CONFIRMED @ 0x00410CA0: cases 0/8/9 leave 0x00463210 untouched.] */
            g_td5.difficulty_tier = s_game_option_difficulty;
            TD5_LOG_I(LOG_TAG,
                      "ConfigureGameTypeFlags: gt=%d uses user Difficulty toggle -> tier=%d",
                      s_selected_game_type, g_td5.difficulty_tier);
            break;
    }
    TD5_LOG_I(LOG_TAG, "ConfigureGameTypeFlags: game_type=%d tier=%d",
              s_selected_game_type, g_td5.difficulty_tier);

    switch (s_selected_game_type) {
    case 0: /* Single Race -- user preferences apply */
        /* [CONFIRMED @ 0x004155DE] live circuit lap count = gCircuitLapsConfigShadow + 1,
         * NOT doubled — kept from this branch's frontend pass. The prior *2 made a Single
         * Race run twice the count shown on Game Options, and contradicts this file's own
         * display comment below (frontend_render_game_options_overlay @ ~0x0041FD78: "NO *2
         * multiply ... g_td5.circuit_lap_count = laps+1"). [merge-resolved 2026-06-02] */
        g_td5.circuit_lap_count = s_game_option_laps + 1;
        /* The AI first-layer template scaling in td5_ai_init_race_actor_runtime
         * (InitializeRaceActorRuntime @ 0x00432F2F / 0x00432FB4) is now keyed on
         * the DYNAMICS flag (gDifficultyEasy @0x004AAF84 = the arcade/sim toggle),
         * matching the original — it no longer reads g_td5.difficulty. The user
         * difficulty toggle routes into difficulty_tier (above), which is the path
         * the original actually ties to user difficulty (gRaceDifficultyTier
         * @0x00463210, read only AFTER the dynamics block @ 0x00432FFD). The
         * g_td5.difficulty field below is retained only for save/log round-trip
         * (td5_save.c) and no longer affects AI scaling. */
        g_td5.difficulty = TD5_DIFFICULTY_NORMAL;
        g_td5.traffic_enabled = s_game_option_traffic;
        g_td5.special_encounter_enabled = s_game_option_cops;
        td5_physics_set_collisions(s_game_option_collisions);
        td5_physics_set_dynamics(s_game_option_dynamics);
        g_td5.checkpoint_timers_enabled = s_game_option_checkpoint_timers;
        break;

    case 1: /* Championship */
        g_td5.race_rule_variant = 0;
        g_td5.difficulty = TD5_DIFFICULTY_EASY;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        break;

    case 2: /* Era */
        g_td5.race_rule_variant = 5;
        g_td5.difficulty = TD5_DIFFICULTY_EASY;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        g_td5.circuit_lap_count = 4;
        break;

    case 3: /* Challenge */
        g_td5.race_rule_variant = 1;
        g_td5.difficulty = TD5_DIFFICULTY_NORMAL;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        break;

    case 4: /* Pitbull */
        g_td5.race_rule_variant = 2;
        g_td5.difficulty = TD5_DIFFICULTY_NORMAL;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        break;

    case 5: /* Masters */
        g_td5.race_rule_variant = 3;
        g_td5.difficulty = TD5_DIFFICULTY_NORMAL;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        /* Generate 15-car random roster */
        {
            int i, j, idx, ok;
            int count = 0;
            memset(s_masters_roster, 0, sizeof(s_masters_roster));
            memset(s_masters_roster_flags, 0, sizeof(s_masters_roster_flags));
            while (count < 15) {
                idx = rand() % 27; /* 0..26 */
                ok = 1;
                for (j = 0; j < count; j++) {
                    if (s_masters_roster[j] == idx) { ok = 0; break; }
                }
                if (ok) {
                    s_masters_roster[count] = idx;
                    count++;
                }
            }
            /* Randomly mark 6 as AI opponents */
            count = 0;
            while (count < 6) {
                i = rand() % 15;
                if (s_masters_roster_flags[i] == 0) {
                    s_masters_roster_flags[i] = 1; /* AI */
                    count++;
                }
            }
        }
        break;

    case 6: /* Ultimate */
        g_td5.race_rule_variant = 4;
        g_td5.difficulty = TD5_DIFFICULTY_HARD;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 1;
        break;

    case 7: /* Time Trials — synthesized as plain single race + slots 1..5
             * inactive, mirroring the Frida TT-synth on the original side
             * (re/tools/quickrace/td5_quickrace_hook.js). Without this, the
             * port runs TT-specific AI paths (g_active_actor_count=1,
             * racer_count=1, rubber-banding scales=0) while the original (via
             * the Frida synth) runs plain single-race AI for slot 0 with
             * 5 inactive opponents. Apples-to-oranges AI commands → ~2× vel_x
             * divergence in /diff-race. By keeping time_trial_enabled=0 and
             * routing through gt=0 we make both sides take the same AI/physics
             * path, with solo behavior produced by the slot-state suppression
             * (see td5_game.c init_race below). */
        g_td5.solo_mode_synth = 1;
        g_td5.difficulty = TD5_DIFFICULTY_HARD;
        g_td5.traffic_enabled = 0;
        g_td5.special_encounter_enabled = 0;
        g_td5.circuit_lap_count = 1;           /* single lap on circuits */
        g_td5.checkpoint_timers_enabled = 1;   /* enable P2P checkpoint timers */
        td5_physics_set_collisions(0);         /* no collisions (solo) */
        g_td5.game_type = TD5_GAMETYPE_SINGLE_RACE;  /* runtime sees gt=0 */
        TD5_LOG_I(LOG_TAG,
                  "ConfigureGameTypeFlags case 7: TT synth — game_type 7 -> 0, "
                  "time_trial_enabled=0, solo_mode_synth=1 "
                  "(mirrors Frida TT-synth in td5_quickrace_hook.js)");
        break;

    case 8: /* Cop Chase */
        g_td5.wanted_mode_enabled = 1;
        g_td5.traffic_enabled = 1;
        g_td5.special_encounter_enabled = 0;
        break;

    case 9: /* Drag Race */
        g_td5.drag_race_enabled = 1;
        g_td5.traffic_enabled = 0;
        g_td5.special_encounter_enabled = 0;
        break;

    default:
        break;
    }

    /* [dynamic-traffic] Normalize the traffic VOLUME for every game type.
     * Single Race honors the user's 5-state row (0=Off 1=Light 2=Normal
     * 3=Heavy 4=Very High → traffic_enabled = volume != 0); cup/cop types that
     * force traffic on carry the user's selected volume too, defaulting to the
     * classic full density (Heavy) when the selector reads 0 while enabled.
     * Every faithful consumer keeps reading the traffic_enabled boolean. */
    {
        int tv = s_game_option_traffic;
        if (tv < 0) tv = 0;
        if (tv > TD5_TRAFFIC_VOLUME_COUNT - 1) tv = TD5_TRAFFIC_VOLUME_COUNT - 1;
        if (s_selected_game_type == 0) {
            g_td5.traffic_volume  = tv;
            g_td5.traffic_enabled = (g_td5.traffic_volume != 0);
        } else {
            /* Forced-on game types: carry the real selector value (0..4); if it
             * resolves to 0 while traffic is enabled, fall back to Heavy (3) so
             * "enabled" never degenerates to no traffic. */
            g_td5.traffic_volume = g_td5.traffic_enabled ? (tv != 0 ? tv : 3) : 0;
        }
    }

    /* For cup types 1-6, check series schedule for end sentinel (99) */
    if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
        int sched_idx = s_selected_game_type - 1;
        if (s_race_within_series >= 0 && s_race_within_series < 13) {
            if (s_cup_schedules[sched_idx][s_race_within_series] == 99) {
                /* End-of-series: invoke the per-tier commit callback.
                 * Jump table @ 0x464108, entries @ 0x410F60..0x4110A0.
                 * Each callback sets unlock bits in s_cup_unlock_tier
                 * (DAT_004962A8) and advances s_total_unlocked_tracks
                 * / s_total_unlocked_cars for higher tiers.
                 * [RE basis: deep Ghidra pass of each callback body] */
                switch (s_selected_game_type) {
                case 1: /* Championship @ 0x410F60: unlock bit 0 if track>=18 */
                    if (s_total_unlocked_tracks >= 18 &&
                        (s_cup_unlock_tier & 7) == 0) {
                        s_cup_unlock_tier |= 1;
                    }
                    break;
                case 2: /* Era @ 0x410FA0: clamp tracks to 18, unlock bit 0 */
                    if (s_total_unlocked_tracks < 18) {
                        s_total_unlocked_tracks = 18;
                    }
                    if ((s_cup_unlock_tier & 7) == 0) {
                        s_cup_unlock_tier |= 1;
                    }
                    break;
                case 3: /* Challenge @ 0x410FF0: unlock bit 1 */
                    s_cup_unlock_tier |= 2;
                    break;
                case 4: /* Pitbull @ 0x411030: unlock bit 1 */
                    s_cup_unlock_tier |= 2;
                    break;
                case 5: /* Masters @ 0x411070: tier clear, no new bits */
                    break;
                case 6: /* Ultimate @ 0x4110A0: clamp cars>=37, tracks>=19 */
                    if (s_total_unlocked_cars < 37) {
                        s_total_unlocked_cars = 37;
                    }
                    if (s_total_unlocked_tracks < 19) {
                        s_total_unlocked_tracks = 19;
                    }
                    break;
                }
                TD5_LOG_I(LOG_TAG,
                          "ConfigureGameType: cup %d completed, tier=0x%02X tracks=%d cars=%d",
                          s_selected_game_type, s_cup_unlock_tier,
                          s_total_unlocked_tracks, s_total_unlocked_cars);
                return 0; /* end of series */
            }
        }
    }

    return 1;
}

/* ========================================================================
 * Bar-fade transitions
 * ======================================================================== */

static void BarFade_Start(int table_index, int direction) {
    s_fade_table_index = table_index;
    if (table_index < 0 || table_index >= (int)(sizeof(s_bar_fade_table)/sizeof(s_bar_fade_table[0])))
        s_fade_table_index = 0;
    s_fade_active = 1;
    s_fade_progress = (direction > 0) ? 0 : 255;
    s_fade_direction = direction;
}

static int BarFade_Tick(void) {
    if (!s_fade_active) return 1;
    s_fade_progress += s_fade_direction * 8 * s_fe_logic_ticks;
    if (s_fade_progress >= 256 || s_fade_progress < 0) {
        s_fade_active = 0;
        return 1; /* done */
    }
    /* Real implementation would render horizontal bar sweep using
     * s_bar_fade_table[s_fade_table_index].{r,g,b} and fade_type */
    return 0;
}

/* Returns 1 if the screen should get the universal transition fades (slide-out
 * whoosh on leave, slide-in chime on enter). DEFAULT is 1 so any screen added
 * to s_screen_table — including future ones — inherits the fades with no extra
 * wiring. Excluded: boot/init/attract/debug screens (not user navigation) and
 * the deliberately-silent end dialogs (CupWon/Failed/SessionLocked), which the
 * earlier frontend-audio audit confirmed should stay silent. See the universal
 * transition-fades block near frontend_play_sfx. */
static int frontend_screen_wants_fade(TD5_ScreenIndex s) {
    switch (s) {
    case TD5_SCREEN_LOCALIZATION_INIT:   /* [0]  boot/localization init */
    case TD5_SCREEN_POSITIONER_DEBUG:    /* [1]  dev glyph-positioner tool */
    case TD5_SCREEN_ATTRACT_MODE:        /* [2]  attract demo (routes into a race) */
    case TD5_SCREEN_LANGUAGE_SELECT:     /* [3]  first-run boot screen */
    case TD5_SCREEN_LEGAL_COPYRIGHT:     /* [4]  copyright splash */
    case TD5_SCREEN_STARTUP_INIT:        /* [28] boot init redirect */
    case TD5_SCREEN_CUP_FAILED:          /* [26] silent end dialog */
    case TD5_SCREEN_CUP_WON:             /* [27] silent end dialog */
    case TD5_SCREEN_SESSION_LOCKED:      /* [29] silent end dialog */
        return 0;
    default:
        return 1;                        /* every navigable menu screen, incl. new/future */
    }
}

/* ========================================================================
 * SetFrontendScreen (0x414610)
 *
 * Navigation: resets inner state, timestamps, sets active screen.
 * ======================================================================== */

/* [2026-06-19] Net disconnect notice. When a netplay session drops (host
 * timeout, host quit/DXPDISCONNECT, or a mid-race lockstep loss), route here to
 * show a "CONNECTION LOST" screen that then returns to the main menu instead of
 * silently dumping the player back. Reuses the Screen_SessionLocked notice
 * dialog with a mode flag (see frontend_render_session_locked_overlay +
 * Screen_SessionLocked's case 5 auto-timeout). Shared with td5_fe_net.c. */
int  g_net_disconnect_mode = 0;
char g_net_disconnect_reason[64] = {0};

void td5_frontend_show_net_disconnect(const char *reason) {
    snprintf(g_net_disconnect_reason, sizeof(g_net_disconnect_reason), "%s",
             (reason && reason[0]) ? reason : "Connection to host lost");
    g_net_disconnect_mode = 1;
    TD5_LOG_I(LOG_TAG, "Net disconnect -> notice screen: %s", g_net_disconnect_reason);
    td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
}

void td5_frontend_set_screen(TD5_ScreenIndex index) {
    TD5_ScreenIndex previous = s_current_screen;
    int preserved_background = s_background_surface;

    if (index < 0 || index >= TD5_SCREEN_COUNT) return;

    s_previous_screen = previous;
    s_current_screen = index;
    s_fe_draw_log_frame = 0;
    s_inner_state = 0;
    s_anim_tick = 0;
    s_anim_t = 0.0f;
    s_anim_start_ms = 0;
    s_anim_elapsed_ms = 0;
    s_anim_complete = 0;
    s_screen_entry_timestamp = td5_plat_time_ms();
    s_prev_enter_state = 1;
    s_prev_left_state = 1;
    s_prev_right_state = 1;

    /* Reset button pool and auto-layout for new screen */
    frontend_reset_buttons();
    if (preserved_background > 0) {
        int slot = preserved_background - 1;
        if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) {
            preserved_background = 0;
        }
    }
    s_background_surface = preserved_background;
    s_carsel_bg_surface = 0;
    s_carsel_fill_surface = 0;
    s_carsel_bar_surface = 0;
    s_carsel_curve_surface = 0;
    s_carsel_topbar_surface = 0;
    s_graphbars_surface = 0;
    s_car_preview_surface = 0;
    s_car_preview_prev_surface = 0;
    s_car_preview_next_surface = 0;
    /* Reset the TD6 body-paint overlay cache in LOCKSTEP with the preview
     * surface. The recyclable-surface sweep below frees every non-shared slot
     * (in_use=0), INCLUDING whatever slot the paint overlay was on — but the
     * handle/cache-key here used to survive the screen change. That stale
     * s_paint_overlay_surface handle then aliased a freshly-loaded preview slot
     * on the next car-select entry: the lazy reload in
     * frontend_draw_car_paint_overlay would frontend_release_surface() the alias
     * (freeing the new preview) and upload CarPicPaint0 (chassis-only) onto the
     * preview's page — so only the painted chassis of the last TD6 car showed,
     * and reselect then bled paint onto the whole body. Dropping both the handle
     * and the cached car index here keeps the overlay cache consistent with the
     * preview, so no alias can form. (TD6-only: TD5 cars never load an overlay.)
     * NO RE BASIS — the TD6 colour-overlay preview is a port-only feature. */
    s_paint_overlay_surface = 0;
    s_paint_overlay_car = -1;
    s_track_preview_surface = 0;
    s_gallery_pic_surface = 0;
    s_gallery_pic_index = 0;
    s_gallery_visited_mask = 0;
    s_control_options_surface = 0;
    /* Release recyclable surfaces, but KEEP shared assets on dedicated pages.
     * Shared pages (895-899) hold font, cursor, ButtonBits, mainfont --
     * these are loaded once in init and must survive screen transitions. */
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (preserved_background > 0 && i == preserved_background - 1) continue;
        if (s_surfaces[i].in_use &&
            s_surfaces[i].tex_page >= SHARED_PAGE_MIN &&
            s_surfaces[i].tex_page < FE_SURFACE_PAGE_BASE) continue;
        /* [PERF 2026-06-06] Keep cached BACKGROUNDS across transitions so revisiting
         * a screen reuses the decoded surface (frontend_find_surface_by_source hit)
         * instead of re-decoding its ~4ms PNG — that re-decode is the screen-change
         * MS spike. The loader's LRU eviction (lowest load_seq) reclaims these slots
         * under pressure, so a heavy screen (car-select) can never be starved. */
        if (s_surfaces[i].in_use &&
            frontend_surface_is_background_like(s_surfaces[i].width, s_surfaces[i].height))
            continue;
        s_surfaces[i].in_use = 0;
    }

    /* Reset attract-mode idle counter on any navigation */
    frontend_note_activity();

    /* S10b: clear any lingering text-input state on navigation so a confirmed
     * field (e.g. the nickname screen) doesn't leave the text widget rendering
     * on the next screen (the Direct-IP chooser was showing a stray input box
     * under its buttons). Text-entry screens re-enable it in their state 0. */
    s_text_input_state = 0;

    g_td5.frontend_screen_index = (int)index;
    g_td5.frontend_inner_state = 0;
    g_td5.frontend_frame_counter = 0;

    /* Universal transition fades (S03). The original SetFrontendScreen
     * (0x00414610) is silent and per-screen state-0 code emits its own Play(N),
     * so an UNCONDITIONAL play here would double the wired screens' Whoosh (the
     * reason a prior attempt was reverted). Instead we play the default slide-OUT
     * only when the screen being LEFT never whooshed during its lifetime — every
     * wired screen does, so this fires solely for unwired/new screens — and ARM
     * the default slide-IN chime for the screen being ENTERED (fired at settle by
     * the display loop, suppressed if the new screen chimes itself). */
    if (frontend_screen_wants_fade(previous) && !s_fade_whoosh_emitted) {
        td5_sound_play_frontend_sfx(5);   /* default slide-out for the leaving screen */
    }
    s_fade_whoosh_emitted = 0;
    s_fade_chime_emitted  = 0;
    s_fade_in_pending     = frontend_screen_wants_fade(index) ? 1 : 0;

    TD5_LOG_I(LOG_TAG, "Screen transition: %d -> %d", (int)previous, (int)index);
    s_logged_screen = (TD5_ScreenIndex)-1;
    s_logged_inner_state = -1;
}

TD5_ScreenIndex td5_frontend_get_screen(void) {
    return s_current_screen;
}

/* ========================================================================
 * RunFrontendDisplayLoop (0x414B50)
 *
 * Called each frame while game_state == MENU.
 * Returns: 0 = continue, 1 = start race, -1 = quit
 *
 * L5 promotion sweep audit (2026-05-18, TD5_pool0 read-only) -- structurally
 * faithful with one ARCH-DIVERGENCE (DDraw flip / surface-lost handshake)
 * and a handful of cosmetic re-orderings. Orig is 996 bytes
 * (0x00414B50..0x00414F34).
 *
 *   Step-by-step mapping (orig decomp -> port):
 *     - Surface-lost test: orig peeks the backbuffer's Lost() at
 *       dd_exref+4 vtbl[0x60] and, on DDERR_SURFACELOST (-0x7789FE3E),
 *       calls vtbl[0x6C] (Restore) + sets g_frontendRedrawCount=3, then
 *       blit-restores the 640x480 cached backdrop via BlitFrontendCachedRect.
 *       Port: frontend_recover_surfaces() (td5_frontend.c) -- same
 *       semantics expressed through the platform abstraction.
 *       [ARCH-DIVERGENCE: D3D11 has no DDERR_SURFACELOST analog at the
 *       wrapper boundary the port owns; surface loss is owned by the
 *       DXGI device and recovery is implicit via swap-chain Resize. The
 *       3-frame redraw counter is therefore unused in the port and the
 *       BlitFrontendCachedRect prelude collapses to a no-op.]
 *     - Race-start early-out: orig tests g_startRaceRequestFlag != 0
 *       and returns; port returns 1 at end-of-function from
 *       g_td5.race_requested. Same observable behaviour, deferred to
 *       end so input still gets polled. [CONFIRMED @ 0x00414B7B vs
 *       td5_frontend.c:3044.]
 *     - Input edge-detect @ 0x00414B96-0x00414BBC: orig
 *         g_frontendInputEdgeBits = ~(prev & cur);
 *         prev = cur; cur = DXInputGetKBStick(0);
 *         g_frontendInputEdgeBits = edge & prev & cur;
 *       Port frontend_poll_input() does the same edge math against the
 *       polled keyboard mask. Behaviour-equivalent.
 *     - Mouse-movement detect @ 0x00414BC0-0x00414C36: orig computes
 *       |dx| + |dy| > 8 against g_appExref+0x100/+0x104, gates cursor
 *       draw on g_frontendMouseCursorEnabled. Port carries equivalent
 *       state and defers the cursor quad to frontend_render_cursor()
 *       after the per-screen state machine -- net same submission order.
 *     - Per-screen dispatch via function pointer: orig
 *       (*g_currentScreenFnPtr)() (overwritten with a JMP to
 *       LogicGate_ScreenDispatch by the widescreen patch); port indexes
 *       s_screen_table[s_current_screen] and invokes it. Same 30-entry
 *       dispatch table semantically.
 *     - Race-confirm early-out @ 0x00414C2F: if (g_startRaceConfirmFlag)
 *       return. Port: deferred to end-of-function returning 1
 *       (g_td5.race_requested). Behaviour-equivalent.
 *     - Cursor overlay queue @ 0x00414C44: orig QueueFrontendOverlayRect
 *       (mouseY, mouseX, 0,0, 0x16,0x1e, 0xff0000,
 *       g_frontendCursorTextureId). Port frontend_render_cursor() queues
 *       a 22x30 textured quad at the same texture page. Order matches
 *       orig RenderFrontendUiRects + Flush ordering.
 *     - Submission flush @ 0x00414C5F: orig calls RenderFrontendUiRects +
 *       FlushFrontendSpriteBlits, then either
 *         (a) DXDraw::Flip(1) + an immediate Lock/Unlock of the back
 *             surface (an empty SNK_ScreenDump primer), OR
 *         (b) software 16bpp PresentFrontendBufferSoftware.
 *       Port collapses both branches to td5_plat_present(1) -- D3D11
 *       swap-chain Present handles both the HW and SW cases internally.
 *       [ARCH-DIVERGENCE: g_frontendHardwareFlipEnabled branch +
 *       g_frontendFrameToggle double-buffer toggle do not exist in the
 *       port; DXGI owns flip-model presentation. The Lock call at
 *       0x00414CDC was an empty primer for the SNK_ScreenDump panic
 *       path and has no behavioural consequence either side.]
 *     - UpdateFrontendDisplayModeSelection @ 0x00414D87: orig calls a
 *       resolution-switch FSM tied to the DDraw enum-display-modes
 *       callback. Port has its own td5_plat_set_window_size() flow on
 *       INI change; observable behaviour equivalent, trigger surface
 *       differs.
 *     - ESC-to-default-button: orig + port both set
 *       g_frontendButtonIndex = g_frontendEscKeyButtonIndex; pressed=1.
 *       Port lifts this into the screen-aware ESC handler (handles
 *       MAIN_MENU, EXTRAS_GALLERY, STARTUP_INIT special cases).
 *       Behaviour-equivalent and more correct for the RaceResults exit
 *       path.
 *     - Cheat-code octet decoder @ 0x00414DD0-0x00414E70: orig walks 6
 *       NPC-racer slots, indexes a 0x28-stride byte table at 0x004654A4
 *       using a per-slot progress counter at 0x004951F0, and XORs a per-
 *       slot toggle into 0x00465594 entries. Port:
 *       frontend_update_cheat_codes() performs the identical 6-slot loop
 *       with the same key tables.
 *     - g_frontendAnimFrameCounter++ @ 0x00414F02 (orig has a NOPped INC
 *       EDX from the widescreen patch). Port: lifted into screen-local
 *       counters (s_anim_tick); cosmetic.
 *     - Attract-mode trigger @ 0x00414F10: orig fires after 50000 ms
 *       idle on MainMenu and 0xFFF1 < g_attractModeIdleCounter
 *       (saturating int16). Port uses a 60000 ms idle window against
 *       s_attract_idle_timestamp -- slightly different timeout (50s vs
 *       60s) and uses rand() % 8 instead of _rand() % 0x13 to avoid
 *       DAT_004668B0's disable-mask, but the trigger is gated by the
 *       same MAIN_MENU condition. Cosmetic; port's track set is the
 *       playable subset.
 *
 *   No code edit needed; all divergences are intentional D3D3->D3D11
 *   API substitutions or behaviour-equivalent reorderings. Effective
 *   level after audit: L5 + [ARCH-DIVERGENCE] for the DDraw flip /
 *   surface-lost handshake.
 * ======================================================================== */

/* [FPS-DECOUPLE 2026-06-07] Advance the frontend animation clock on a fixed
 * 60 Hz cadence. Accumulates real elapsed milliseconds and converts them into a
 * whole number of 60 Hz ticks for this frame; the leftover carries forward. The
 * render loop still runs every frame — this only paces the animation counters so
 * menus / slideshow / fades play at the same real-time speed at 60, 144 or 180
 * Hz (the original ran these once per refresh-frame, i.e. ~60 Hz on era CRTs). */
static void frontend_update_anim_pacing(void) {
    uint32_t now = td5_plat_time_ms();
    if (s_fe_anim_prev_ms == 0) s_fe_anim_prev_ms = now;  /* first frame: no jump */
    uint32_t dt = now - s_fe_anim_prev_ms;
    s_fe_anim_prev_ms = now;
    if (dt > 250u) dt = 250u;                 /* clamp long stalls (asset loads, alt-tab) */
    s_fe_anim_accum_ms += (float)dt;
    const float tick_ms = 1000.0f / 60.0f;    /* 60 Hz animation reference */
    int ticks = (int)(s_fe_anim_accum_ms / tick_ms);
    if (ticks < 0) ticks = 0;
    if (ticks > 8) ticks = 8;                 /* spiral-of-death cap */
    s_fe_anim_accum_ms -= (float)ticks * tick_ms;
    s_fe_logic_ticks = ticks;
}

int td5_frontend_display_loop(void) {
    if (g_td5.ini.log_frontend_draw) s_fe_draw_log_frame++;
    td5_profile_begin_frame();
    frontend_update_anim_pacing();   /* [FPS-DECOUPLE] pace animations at 60 Hz */
    /* 0. Poll platform input so s_keyboard[] is fresh for this frame */
    {
        TD5_InputState dummy;
        td5_plat_input_poll(0, &dummy);
    }

    /* 1. Surface recovery (DDERR_SURFACELOST) — ONLY on an actual window/device
     * resolution change, NOT every frame. [PERF FIX] Running it unconditionally
     * re-decoded + re-uploaded EVERY tracked surface each frame; on SELECT CAR
     * that meant re-decoding the ~26ms carpic + overlay per frame (~35ms/frame =
     * the FPS collapse from 180 to ~30). In D3D11 textures survive a resize, so
     * recovery is essentially never needed; gate it on a size change to preserve
     * the original intent (re-upload after a device reset) at zero steady cost. */
    {
        static int s_rec_w = -1, s_rec_h = -1;
        int rw = 0, rh = 0;
        td5_plat_get_window_size(&rw, &rh);
        if (rw != s_rec_w || rh != s_rec_h) {
            frontend_recover_surfaces();
            s_rec_w = rw; s_rec_h = rh;
        }
    }

    /* 2. Input polling (keyboard, mouse, joystick).
     * The simultaneous-MP car-select grid reads every pad directly and creates no
     * s_buttons[]; running the shared poll here would treat each player's confirm
     * press as "Enter on a screen with no focused button" and play the locked /
     * rejection sfx (10). Skip it — the grid owns its own input + ESC handling. */
    {
        int simul_grid = (s_current_screen == TD5_SCREEN_CAR_SELECTION) &&
                         (s_mp_simul || (s_mp_flow && s_num_human_players >= 2));
        /* The MP position picker (#8) likewise reads each pad directly and creates
         * no s_buttons[]; skip the shared poll so per-player confirm presses don't
         * hit the "no focused button" locked sfx. */
        if (s_current_screen == TD5_SCREEN_MP_POSITION) simul_grid = 1;
        if (!simul_grid) frontend_poll_input();
    }
    td5_profile_mark("fe_input");   /* steps 0-2: input + surface recovery */

    /* 3. Screen dispatch -- call the active screen's state machine */
    if (s_current_screen >= 0 && s_current_screen < TD5_SCREEN_COUNT) {
        ScreenFn fn = s_screen_table[s_current_screen];
        if (fn) fn();
    }
    td5_profile_mark("fe_fsm");

    /* 3b. Universal slide-IN chime (S03). fn() above has already run this frame,
     * so s_fade_chime_emitted reflects any chime the new screen played itself —
     * if it did, drop the armed default (no double). Otherwise fire the default
     * once the screen settles (s_anim_complete) or after a deadline backstop for
     * screens that never set it. New screens added to s_screen_table inherit the
     * enter chime here with no per-screen wiring. */
    if (s_fade_in_pending) {
        if (s_fade_chime_emitted) {
            s_fade_in_pending = 0;        /* the new screen chimed itself */
        } else if (s_anim_complete ||
                   (td5_plat_time_ms() - s_screen_entry_timestamp) >= TD5_FE_FADE_IN_DEADLINE_MS) {
            td5_sound_play_frontend_sfx(4);
            s_fade_in_pending = 0;
        }
    }

    /* 4. Render flush (cursor queued after all other UI) */
    td5_frontend_render_ui_rects();
    frontend_render_cursor();
    td5_frontend_flush_sprite_blits();
    td5_profile_mark("fe_render");

    /* VectorUI mouse cursor: drawn last (on top of everything) via the SDF
     * pointer shader. Bitmap cursor (when VectorUI off) was queued above. */
    if (g_td5.ini.vector_ui && s_ps_cursor && s_cursor_msdf_page >= 0) {
        int sw = 0, sh = 0;
        td5_plat_get_window_size(&sw, &sh);
        if (sw > 0 && sh > 0)
            fe_draw_cursor_proc((float)sw / 640.0f, (float)sh / 480.0f);
    }

    /* 5. Presentation (flip / software blit) */
    td5_plat_present(1);
    td5_profile_mark("fe_present");
    td5_profile_end_frame();

    /* 7. Escape key handling -- only when intro animation is complete (original
     *    behavior: ESC is ignored during slide-in animations) */
    if (s_anim_complete &&
        !(s_mp_simul && s_current_screen == TD5_SCREEN_CAR_SELECTION) &&
        s_current_screen != TD5_SCREEN_MP_POSITION &&   /* picker owns its own B/ESC */
        frontend_check_escape()) {
        if (s_current_screen == TD5_SCREEN_MAIN_MENU) {
            if (s_inner_state == 4) {
                s_inner_state = 5;
            } else if (s_inner_state == 6) {
                if (s_button_count > 7) s_button_count = 7;
                s_selected_button = 6;
                s_inner_state = 4;
            }
        } else if (s_current_screen == TD5_SCREEN_EXTRAS_GALLERY) {
            /* Original: ESC in credits always exits the game [CONFIRMED @ 0x417D50] */
            TD5_LOG_I(LOG_TAG, "ExtrasGallery: ESC pressed, quitting game");
            frontend_post_quit();
        } else if (s_current_screen != TD5_SCREEN_STARTUP_INIT &&
                   s_current_screen != TD5_SCREEN_LOCALIZATION_INIT) {
            if (s_return_screen >= 0 &&
                s_return_screen < TD5_SCREEN_COUNT &&
                s_return_screen != s_current_screen) {
                td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
            } else {
                frontend_play_sfx(10);
            }
        }
    }

    /* 8. Cheat code detection */
    frontend_update_cheat_codes();

    /* 9. Attract mode timeout: 60 seconds of idle on main menu -> demo screen.
     * [FIX 2026-05-25 frontend-attract-mode] Gate on s_anim_complete AND
     * s_inner_state == 4 (the interactive state) so the timer cannot fire
     * while the menu is still sliding in. Without this gate, a fresh boot's
     * idle-since-init window combined with the LOCALIZATION_INIT -> MAIN_MENU
     * jump's frontend_note_activity() reset is fine on first entry, but
     * RETURNING to MAIN_MENU from another screen could land mid-slide with a
     * stale-large delta if the user was idle on a sub-screen for >60s.
     * Also emit a clear log when the attract fires so we can see whether the
     * trigger actually happens (issue #4: 'demo not auto-launching'). */
    if (s_current_screen == TD5_SCREEN_MAIN_MENU &&
        s_anim_complete && s_inner_state == 4) {
        uint32_t now = td5_plat_time_ms();
        uint32_t idle_ms = now - s_attract_idle_timestamp;
        if (idle_ms >= 60000u) {
            /* Pick a random track for the demo without corrupting the player's selection */
            s_attract_track = rand() % 8;
            TD5_LOG_I(LOG_TAG,
                      "Attract timer fired: idle_ms=%u -> ATTRACT_MODE (demo track=%d)",
                      idle_ms, s_attract_track);
            td5_frontend_set_screen(TD5_SCREEN_ATTRACT_MODE);
        }
    }

    /* Check if race was requested */
    if (g_td5.race_requested) {
        return 1;
    }
    if (g_td5.quit_requested) {
        return -1;
    }

    return 0;
}

/* ========================================================================
 * InitializeFrontendResourcesAndState (0x414740)
 * ======================================================================== */


/* ========================================================================
 * Background Gallery Slideshow (0x40D590 / 0x40D750 / 0x40D830)
 *
 * pic1-5.tga from Extras.zip are cross-faded at random positions over the
 * main menu background.  Called from td5_frontend_init_resources (load) and
 * td5_frontend_render_ui_rects (update + draw) every frame.
 * ======================================================================== */

static void frontend_advance_bg_gallery(void) {
    /* Pick a different random image (0x40D750: AdvanceExtrasGallerySlideshow) */
    int next = s_bg_gal_current;
    int n = 0;
    do { next = rand() % 5; } while (next == s_bg_gal_current && ++n < 8);
    s_bg_gal_current = next;

    /* Random display position within original bounds (0x40D7EE-0x40D823):
       X = rand() % (0x1F4 - iw) + 0x8C  =>  [140, 500-iw+140]
       Y = rand() % (0x150 - ih) + 0x54  =>  [84, 336-ih+84] */
    int iw = s_bg_gallery[next].width;
    int ih = s_bg_gallery[next].height;
    int range_x = 500 - iw;   /* 0x1F4 @ 0x40D7EE */
    int range_y = 336 - ih;   /* 0x150 @ 0x40D80A */
    s_bg_gal_x = (float)((range_x > 0 ? rand() % range_x : 0) + 140); /* +0x8C @ 0x40D7F9 */
    s_bg_gal_y = (float)((range_y > 0 ? rand() % range_y : 0) + 84);  /* +0x54 @ 0x40D820 */
    TD5_LOG_I(LOG_TAG, "advance_bg_gallery: img=%d iw=%d ih=%d x=%.0f y=%.0f", next, iw, ih, s_bg_gal_x, s_bg_gal_y);
    s_bg_gal_blend = 0x100;
}


static void frontend_render_bg_gallery(float sx, float sy) {
    if (!s_bg_gal_loaded) return;
    int i = s_bg_gal_current;
    if (s_bg_gallery[i].width <= 0) return;

    /* Update blend weight: decrement 1 per frame (original g_attractModeIdleCounter--).
     * Starts at 256 on load/advance, triggers next image at -24 (~280 frames ≈ 4.7s @60fps) */
    s_bg_gal_blend -= s_fe_logic_ticks;
    if (s_bg_gal_blend <= -24) {
        frontend_advance_bg_gallery();
        i = s_bg_gal_current;
    }

    /* Alpha: fade-in 256→240 (16 frames), hold at 60%, fade-out 0→-24 */
    float alpha;
    if (s_bg_gal_blend > 240)
        alpha = 0.6f * (float)(256 - s_bg_gal_blend) / 16.0f;
    else if (s_bg_gal_blend >= 0)
        alpha = 0.6f;
    else
        alpha = 0.6f * (1.0f + (float)s_bg_gal_blend / 24.0f);
    if (alpha <= 0.0f) return;

    uint32_t a     = (uint32_t)(alpha * 255.0f);
    uint32_t color = (a << 24) | 0x00FFFFFF;

    float x = s_bg_gal_x * sx;
    float y = s_bg_gal_y * sy;
    float w = (float)s_bg_gallery[i].width  * sx;
    float h = (float)s_bg_gallery[i].height * sy;

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(x, y, w, h, color, SHARED_PAGE_BG_GALLERY + i, 0.0f, 0.0f, 1.0f, 1.0f);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

void td5_frontend_release_resources(void) {
    TD5_LOG_I(LOG_TAG, "ReleaseFrontendResources");
    /* Release all loaded surfaces, fonts, work buffers */
    for (int i = 0; i < FE_MAX_SURFACES; i++) s_surfaces[i].in_use = 0;
    td5_fe_btncache_reset();
    td5_fe_btncache_release_sources();
    s_font_page = -1;
    s_cursor_tex_page = -1;
    s_cursor_w = 0; s_cursor_h = 0;
    s_buttonbits_tex_page = -1;
    s_buttonbits_w = 0; s_buttonbits_h = 0;
    s_buttonlights_tex_page = -1;
    s_buttonlights_w = 0; s_buttonlights_h = 0;
    s_white_tex_page = -1;
    s_background_surface = 0;
}

/* ========================================================================
 * Render helpers (stubs)
 * ======================================================================== */

static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1); /* forward decl */

static void frontend_draw_value_text(float sx, float sy, int x, int y,
                                     const char *text, uint32_t color) {
    /* [2026-06-16] Skip only when NEITHER the menu TTF nor the bitmap atlas can
     * render — fe_draw_text draws via TTF first, so a deleted BodyText.png (atlas
     * page < 0) must not suppress the (TTF) draw. */
    if (!text || !text[0] || (!td5_font_ready() && s_font_page < 0)) return;
    fe_draw_text((float)x * sx, (float)y * sy, text, color, sx, sy);
}

/* Draw a Quick Race value in the right-hand column at FE_QR_VALUE_SCALE (matched
 * to the button-caption size). If it would run past the right margin, wrap to a
 * second line below; the 1- or 2-line block is vertically centered on the
 * button row at row_btn_idx. */
/* reserve_right: canvas px to keep clear at the right of the value column (for the
 * Car/Track randomize chip, item #7) so a long wrapped name never slides under it.
 * 0 for every other row. */
static void frontend_draw_qr_value(float sx, float sy, int row_btn_idx,
                                   const char *text, uint32_t color, int reserve_right) {
    if (row_btn_idx < 0 || row_btn_idx >= s_button_count) return;
    /* [2026-06-16] TTF-first: don't suppress on a deleted BodyText.png atlas. */
    if (!text || !text[0] || (!td5_font_ready() && s_font_page < 0)) return;
    const float gs   = FE_QR_VALUE_SCALE;
    const float vx   = (float)FE_QR_VALUE_X * sx;
    const float avail = (float)(FE_QR_SCREEN_W - FE_QR_VALUE_X - FE_QR_RIGHT_MARGIN
                                - reserve_right) * sx;
    const int   by   = s_buttons[row_btn_idx].y;

    /* Fits on one line — center it on the 32px button. */
    if (fe_measure_text(text, sx * gs, sy * gs) <= avail) {
        float ty = ((float)by + (32.0f - FE_QR_VALUE_LINE_H) * 0.5f) * sy;
        fe_draw_text(vx, ty, text, color, sx * gs, sy * gs);
        return;
    }

    /* Wrap: split at the last space whose prefix fits; else hard char-break. */
    char l1[96], l2[96];
    int len = (int)strlen(text);
    int split = -1;
    for (int i = 1; i < len && i < (int)sizeof(l1); i++) {
        if (text[i] != ' ') continue;
        memcpy(l1, text, (size_t)i); l1[i] = '\0';
        if (fe_measure_text(l1, sx * gs, sy * gs) <= avail) split = i;
        else break;
    }
    if (split > 0) {
        memcpy(l1, text, (size_t)split); l1[split] = '\0';
        snprintf(l2, sizeof(l2), "%s", text + split + 1);
    } else {
        int cut = 1;
        for (int i = 1; i <= len && i < (int)sizeof(l1); i++) {
            memcpy(l1, text, (size_t)i); l1[i] = '\0';
            if (fe_measure_text(l1, sx * gs, sy * gs) > avail) break;
            cut = i;
        }
        memcpy(l1, text, (size_t)cut); l1[cut] = '\0';
        snprintf(l2, sizeof(l2), "%s", text + cut);
    }
    float top = (float)by + (32.0f - 2.0f * FE_QR_VALUE_LINE_H) * 0.5f;
    fe_draw_text(vx,  top * sy,                                  l1, color, sx * gs, sy * gs);
    fe_draw_text(vx, (top + FE_QR_VALUE_LINE_H) * sy,           l2, color, sx * gs, sy * gs);
}

/* Value text centering: original draws to 224px panel at screen x=394 (canvasW/2+0x4A),
 * then centers text within via MeasureOrCenter. Panel center = 394+112 = 506.
 * [CONFIRMED @ 0x41FF5B: ADD ESI,0x11C; panel width 0xE0 @ CreateTrackedFrontendSurface] */
#define FE_VALUE_PANEL_X    394
#define FE_VALUE_PANEL_W    224
#define FE_VALUE_CENTER_X   506  /* FE_VALUE_PANEL_X + FE_VALUE_PANEL_W/2 */

static void frontend_draw_value_centered(float sx, float sy, int y,
                                         const char *text, uint32_t color) {
    /* [2026-06-16] TTF-first: don't suppress on a deleted BodyText.png atlas. */
    if (!text || !text[0] || (!td5_font_ready() && s_font_page < 0)) return;
    fe_draw_text_centered((float)FE_VALUE_CENTER_X * sx, (float)y * sy,
                          text, color, sx, sy);
}

enum {
    FE_BUTTON_ANIM_NONE = 0,
    FE_BUTTON_ANIM_IN,
    FE_BUTTON_ANIM_OUT
};

static int frontend_get_button_anim_state(int *out_mode, int *out_tick, int *out_max_tick) {
    int mode = FE_BUTTON_ANIM_NONE;
    int max_tick = 0;

    switch (s_current_screen) {
    case TD5_SCREEN_MAIN_MENU:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 9 || s_inner_state == 12) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_RACE_TYPE_MENU:
        if (s_inner_state == 1 || s_inner_state == 7) { mode = FE_BUTTON_ANIM_IN; max_tick = 0x20; }
        else if (s_inner_state == 0x14) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_QUICK_RACE:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 6) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_CONNECTION_BROWSER:
        if (s_inner_state == 2) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x20; }
        else if (s_inner_state == 9) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_SESSION_PICKER:
        if (s_inner_state == 2) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x20; }
        else if (s_inner_state == 6) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_NETWORK_LOBBY:
        if (s_inner_state == 1) { mode = FE_BUTTON_ANIM_IN; max_tick = 0x14; }
        break;
    case TD5_SCREEN_OPTIONS_HUB:
    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 8) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_MUSIC_TEST:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 8) { mode = FE_BUTTON_ANIM_OUT; max_tick = 0x20; }
        break;
    case TD5_SCREEN_CAR_SELECTION:
        if (s_inner_state == 5) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x18; }
        else if (s_inner_state == 0x18) { mode = FE_BUTTON_ANIM_OUT; max_tick = 0x18; }
        break;
    case TD5_SCREEN_TRACK_SELECTION:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 7) { mode = FE_BUTTON_ANIM_OUT; max_tick = 0x27; }
        break;
    case TD5_SCREEN_HIGH_SCORE:
        if (s_inner_state == 3) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x27; }
        else if (s_inner_state == 8) { mode = FE_BUTTON_ANIM_OUT; max_tick = 16; }
        break;
    case TD5_SCREEN_RACE_RESULTS:
        /* P7 — button slide-in/out for the post-race menu (states 0xE / 0x10).
         * [CONFIRMED @ 0x00422480 case 0xE / 0x10]
         *   state 0xE: 5 buttons slide in over 32 frames (counter ends == 0x20).
         *     even idx (0,2,4): from left, x_base + counter*0x18
         *     odd  idx (1,3):   from right, x_base + counter*-0x18
         *   state 0x10: 5 buttons slide out over 32 frames (mirror).
         * Port runs s_anim_tick at +=2 from 0..0x10 (8 ticks @ +2 each = 32-frame
         * span in original units) so max_tick is 0x10 here. The base/offscreen
         * delta is handled by the generic frontend_get_button_anim_x logic
         * (odd idx → +640 offscreen, even idx → -640 offscreen).
         *
         * NOTE: states 3/0xB also animate sprites in the original — but those
         * are panel+title sprites, not the click-catcher/OK buttons. Animating
         * the buttons here would make them slide instead of staying parked at
         * y=400 during table-browse. Title slide is handled separately by
         * frontend_get_title_render_y. */
        if (s_inner_state == 0x0E) { mode = FE_BUTTON_ANIM_IN;  max_tick = 0x10; }
        else if (s_inner_state == 0x10) { mode = FE_BUTTON_ANIM_OUT; max_tick = 0x10; }
        break;
    default:
        break;
    }

    if (out_mode) *out_mode = mode;
    if (out_tick) *out_tick = s_anim_tick;
    if (out_max_tick) *out_max_tick = max_tick;
    return (mode != FE_BUTTON_ANIM_NONE && max_tick > 0) ? 1 : 0;
}

/* Returns 1 if the current screen has timed button slide-in/out animations. */
static int frontend_screen_has_button_anim(void) {
    switch (s_current_screen) {
    case TD5_SCREEN_MAIN_MENU:
    case TD5_SCREEN_RACE_TYPE_MENU:
    case TD5_SCREEN_QUICK_RACE:
    case TD5_SCREEN_CONNECTION_BROWSER:
    case TD5_SCREEN_SESSION_PICKER:
    case TD5_SCREEN_NETWORK_LOBBY:
    case TD5_SCREEN_OPTIONS_HUB:
    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
    case TD5_SCREEN_MUSIC_TEST:
    case TD5_SCREEN_CAR_SELECTION:
    case TD5_SCREEN_TRACK_SELECTION:
    case TD5_SCREEN_HIGH_SCORE:
        /* TD5_SCREEN_RACE_RESULTS deliberately NOT listed: only its post-race
         * MENU buttons (states 0xE / 0x10) animate, while the click-catcher /
         * OK buttons used during the table-browse sub-flow (states 0..0xB)
         * stay at fixed positions. Listing screen 24 here would make
         * frontend_get_button_anim_x return offscreen_x for all states where
         * frontend_get_button_anim_state() is FE_BUTTON_ANIM_NONE — i.e. it
         * would push the click-catcher and OK off-screen during state 1..6.
         * The 0xE/0x10 menu animation still works because frontend_get_button
         * _anim_state() returns mode=IN/OUT for those states, regardless of
         * the screen's presence in this allowlist. */
        return 1;
    default:
        return 0;
    }
}

static float frontend_get_button_anim_x(int button_index, float base_x) {
    int mode = FE_BUTTON_ANIM_NONE;
    int tick = 0;
    int max_tick = 0;
    float t;
    /* Phase 4 — slide-in trajectory pinned to Frida-captured original.
     * [CONFIRMED via re/tools/frida_main_menu_capture.log 2026-05-01:
     *  even-row buttons start at dx=-482 and slide right to dx=126;
     *  odd-row buttons start at dx=734 and slide left to dx=126;
     *  step is +/-16 px/frame for 38 frames; total displacement 608 px.]
     *
     * For MAIN_MENU specifically, anchor the slide endpoints to Frida data
     * regardless of the per-button x stored in s_buttons[i] (which is
     * computed from FE_BTN_LEFT_OFFSET=0xD2 → x=110, off by 16 vs Frida's
     * x=126). Other screens keep the legacy +/-640 / button-x defaults
     * because their original endpoints have not been Frida-confirmed.
     *
     * Race Type menu: all buttons slide from left (original behavior).
     * Other screens: odd buttons from right, even from left. */
    float offscreen_x;
    if (s_current_screen == TD5_SCREEN_MAIN_MENU) {
        const float MM_OFFSCREEN_DELTA = 608.0f;
        offscreen_x = (button_index & 1)
                          ? (base_x + MM_OFFSCREEN_DELTA)
                          : (base_x - MM_OFFSCREEN_DELTA);
    } else if (s_current_screen == TD5_SCREEN_RACE_TYPE_MENU) {
        offscreen_x = -640.0f;
    } else {
        offscreen_x = (button_index & 1) ? 640.0f : -640.0f;
    }

    if (!frontend_get_button_anim_state(&mode, &tick, &max_tick)) {
        /* Before the intro animation completes, keep buttons at their off-screen
         * starting position. This prevents a one-frame flash of the final button
         * layout before the slide-in animation begins. */
        if (!s_anim_complete && frontend_screen_has_button_anim())
            return offscreen_x;
        return base_x;
    }

    /* Use continuous s_anim_t for smooth sub-frame motion at any frame rate.
     * The integer s_anim_tick is only kept for game-logic compatibility.
     * With max_tick=39 and offscreen=base±608, this matches the original's
     * +/-16 px/frame discrete motion to <0.5 px error per frame. */
    t = s_anim_t;
    if (mode == FE_BUTTON_ANIM_IN) {
        return base_x + (offscreen_x - base_x) * (1.0f - t);
    }
    return base_x + (offscreen_x - base_x) * t;
}

static void frontend_get_button_render_rect(int button_index, float sx, float sy,
                                            float *out_x, float *out_y,
                                            float *out_w, float *out_h) {
    if (button_index < 0 || button_index >= FE_MAX_BUTTONS) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        if (out_w) *out_w = 0.0f;
        if (out_h) *out_h = 0.0f;
        return;
    }
    /* Phase 4 — base x override for MAIN_MENU.
     * Auto-layout uses FE_CENTER_X - FE_BTN_LEFT_OFFSET = 320 - 210 = 110,
     * but Frida-captured original holds main-menu buttons at dx=126
     * (offset 0xC2 = 194). Override the base only for MainMenu so other
     * screens keep their existing layout. */
    float base_x = (float)s_buttons[button_index].x;
    if (s_current_screen == TD5_SCREEN_MAIN_MENU && s_buttons[button_index].x == FE_CENTER_X - FE_BTN_LEFT_OFFSET) {
        base_x = (float)(FE_CENTER_X - 0xC2);  /* 320 - 194 = 126 [CONFIRMED via Frida] */
    }
    if (out_x) *out_x = frontend_get_button_anim_x(button_index, base_x) * sx;
    if (out_y) *out_y = (float)s_buttons[button_index].y * sy;
    if (out_w) *out_w = (float)s_buttons[button_index].w * sx;
    if (out_h) *out_h = (float)s_buttons[button_index].h * sy;
}

static float frontend_get_title_render_y(float sy) {
    int mode = FE_BUTTON_ANIM_NONE;
    int tick = 0;
    int max_tick = 0;
    /* Phase 1 — title slide constants from Frida capture
     * re/tools/frida_main_menu_capture.log (2026-05-01, 1164 frames).
     * Original Queue*FrontendSpriteBlit logs show the 248×20 title strip
     * starts at dy=-135 at slide-in tick 0 and advances +4 px/frame for
     * 39 frames (max_tick=0x27), landing at dy=-135 + 39*4 = 21.
     * Linear-interp in this function with these endpoints reduces to
     * y = -135 + 4*tick, matching the original frame-for-frame.
     * [CONFIRMED — dx=120, dy progression -135→21 in capture] */
    float base_y   =   21.0f;  /* resting Y at end of slide-in */
    float hidden_y = -135.0f;  /* off-screen start Y */
    float t;

    if (!frontend_get_button_anim_state(&mode, &tick, &max_tick)) {
        /* Mirror button guard: keep title hidden before slide-in anim starts */
        if (!s_anim_complete && frontend_screen_has_button_anim())
            return hidden_y * sy;
        return base_y * sy;
    }

    t = (float)tick / (float)max_tick;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (mode == FE_BUTTON_ANIM_IN) {
        return (base_y + (hidden_y - base_y) * (1.0f - t)) * sy;
    }
    return (base_y + (hidden_y - base_y) * t) * sy;
}

/* ===================================================================== *
 *  Quick Race PORT-ENHANCEMENT widgets — item #4 (dev span-offset field)
 *  and item #7 (Car/Track randomize icons).
 *
 *  Screen_QuickRaceMenu and its button action switch live in the sibling
 *  td5_fe_race.c; this file owns the Quick Race RENDER path
 *  (frontend_render_quick_race_overlay), which runs every frame AFTER the
 *  generic input poll and the screen FSM. These widgets are therefore
 *  implemented entirely here: they are NOT s_buttons[] entries (so the
 *  existing Car/Track/.../OK/Back button indices and nav ring are byte-
 *  identical), and they read the live per-frame input globals directly
 *  (s_mouse_clicked / WM_CHAR queue). The FSM only consumes input for its
 *  own button indices 0..10, so there is no contention.
 * ===================================================================== */

/* [item #7] Cross-module randomize-icon helpers (defined NON-STATIC in the
 * sibling td5_fe_race.c). frontend_draw_randomize_icon is the reusable chip
 * drawer used by the Quick Race icons below; the two render wrappers paint the
 * car-/track-SELECT screens' chips and are invoked from the post-button render
 * switch in td5_frontend_render_ui_rects. */
extern void frontend_draw_randomize_icon(float x, float y, float sx, float sy, int focused);
extern void frontend_render_carsel_randomize_icon(float sx, float sy);
extern void frontend_render_trksel_randomize_icon(float sx, float sy);

/* [#10 2026-06-16] Dedicated RANDOMIZE button geometry for the Car/Track rows: a
 * compact button parked at the far-right of the value column (32px row height).
 * MUST match the identical definitions in td5_fe_race.c (which creates the
 * buttons) — kept in both .c files to avoid touching shared headers. The value
 * text reserves (QR_RAND_BTN_W + gap) on the right so a long name wraps before
 * the button rather than sliding under it. */
#define QR_RAND_BTN_W   104   /* "Randomize" button width                        */
#define QR_RAND_BTN_X   (FE_QR_SCREEN_W - FE_QR_RIGHT_MARGIN - QR_RAND_BTN_W) /* 524 */
#define QR_RAND_RESERVE (QR_RAND_BTN_W + 8)  /* value-column right reserve for it */

/* [#10 2026-06-16] TD5RE_QR_RANDOM_BUTTON gate (default ON). When on, the Quick
 * Race randomize controls are PROPER nav-selectable buttons (QR_BTN_RAND_CAR /
 * QR_BTN_RAND_TRACK, created in the QR init in td5_fe_race.c) drawn by the
 * standard button renderer; the legacy render-path icon/chip + its mouse/'r'
 * handler are suppressed. "0" reverts to the old icon widgets. NON-STATIC so the
 * FSM in td5_fe_race.c can read it (extern-declared there). */
int frontend_qr_random_button_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_QR_RANDOM_BUTTON");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "QuickRace randomize as real BUTTONS (#10) %s (TD5RE_QR_RANDOM_BUTTON=%s)",
                  v ? "ENABLED" : "disabled (legacy icons)", e ? e : "default");
    }
    return v;
}

/* [item #7] TD5RE_RANDOM_ICON gate, read locally (the sibling file has its own
 * static cache of the same knob; we cache our own copy here per the brief).
 * Default ON; exactly "0" suppresses the Quick Race randomize icons.
 * [#10] The legacy icon path is ALSO suppressed whenever the new real-button
 * mode (frontend_qr_random_button_on) is on, so the two never double-draw. */
static int frontend_qr_random_icon_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_RANDOM_ICON");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "QuickRace randomize icons (#7) %s (TD5RE_RANDOM_ICON=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    /* Real-button mode wins: don't draw/handle the old chips when it's active. */
    if (frontend_qr_random_button_on()) return 0;
    return v;
}

#ifndef TD5RE_RELEASE
/* [item #4] TD5RE_DEV_SPAN_FIELD gate (DEV builds only). Default ON; "0" hides
 * the typed span-offset field. Compiled out of release entirely. */
static int frontend_qr_span_field_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_DEV_SPAN_FIELD");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "QuickRace dev span-offset field (#4) %s (TD5RE_DEV_SPAN_FIELD=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}
/* [2026-06-15 BUG #14] TD5RE_SPAN_CARET (default ON; "0" reverts). When on, the
 * QR span-offset edit caret is sized to the value font's cap band (rows 8..23 of
 * the 24px glyph cell, matching fe_draw_text) so it stands as tall as the digits
 * and sits ON them. A prior nudge over-shrank it (3px down, 12px tall) so it sat
 * above and short of the digits; "0" restores that legacy caret. */
static int frontend_span_caret_fix_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_SPAN_CARET");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "QuickRace span-offset caret cap-height fix (#14) %s (TD5RE_SPAN_CARET=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}
/* Editable digit buffer for the span field (mirrors g_td5.ini.start_span_offset,
 * which td5_game.c InitRace applies per-slot with 16-bit wrap). A leading '-' is
 * allowed for negative offsets. */
static char s_qr_span_buf[8];
static int  s_qr_span_buf_init = 0;
/* [2026-06-15 TASK A1] Click-to-type "input active" latch for the QR_BTN_SPAN
 * field. Digit/backspace/'-' capture in frontend_quickrace_widget_input runs ONLY
 * while this is set; the render path draws a caret when active. Toggled by
 * frontend_qr_span_toggle_active (from the FSM's QR_BTN_SPAN activation branch in
 * td5_fe_race.c), and force-cleared when the QR screen leaves its interactive
 * sub-state (see frontend_quickrace_widget_input). */
static int  s_qr_span_active = 0;

/* Sync s_qr_span_buf from the live INI value on first use. */
static void frontend_qr_span_sync_buf(void) {
    if (!s_qr_span_buf_init) {
        snprintf(s_qr_span_buf, sizeof(s_qr_span_buf), "%d", g_td5.ini.start_span_offset);
        s_qr_span_buf_init = 1;
    }
}

/* Parse the editable buffer back into g_td5.ini.start_span_offset. "" or a lone
 * "-" => 0. */
static void frontend_qr_span_commit(void) {
    g_td5.ini.start_span_offset = (s_qr_span_buf[0] && strcmp(s_qr_span_buf, "-") != 0)
                                  ? atoi(s_qr_span_buf) : 0;
}

/* [TASK A1] Toggle the span field's input-active state. Called from the FSM when
 * QR_BTN_SPAN is activated (A/Enter or click). Re-syncs the buffer when opening,
 * commits the parsed value on every toggle, and plays the confirm cue. NON-STATIC
 * so the sibling td5_fe_race.c FSM can reach it (declared in
 * td5_frontend_internal.h). */
void frontend_qr_span_toggle_active(void) {
    frontend_qr_span_sync_buf();
    s_qr_span_active = !s_qr_span_active;
    frontend_qr_span_commit();
    frontend_play_sfx(3);
    TD5_LOG_I(LOG_TAG, "QuickRace span field: input %s (start_span_offset=%d)",
              s_qr_span_active ? "ACTIVE" : "committed", g_td5.ini.start_span_offset);
}
#endif /* !TD5RE_RELEASE */

/* --- car/track validity, replicated from td5_fe_race.c (whose predicates are
 * file-static and unreachable here) using only cross-module primitives, so the
 * random pick honours the SAME selectability rules as the QR cyclers. --- */
static int frontend_qr_td5_car_cap(void) {
    /* mirrors frontend_td5_car_cap_inclusive() */
    if (s_network_active) return TD5_BASE_CAR_COUNT - 1; /* 36 */
    if (s_cheat_unlock_all) return 32;
    int cap = s_total_unlocked_cars - 1;
    if (cap > 32) cap = 32;
    if (cap < 0)  cap = 0;
    return cap;
}
static int frontend_qr_car_selectable(int i) {
    /* mirrors frontend_car_selectable() */
    if (i < 0 || i >= TD5_CAR_COUNT) return 0;
    if (s_selected_game_type == 8) return frontend_car_is_cop(i); /* Cop Chase: cops only */
    if (frontend_car_is_cop(i)) return 0;                          /* cops excluded elsewhere */
    if (i >= TD5_BASE_CAR_COUNT) return 1;                         /* TD6 */
    return i <= frontend_qr_td5_car_cap();                         /* unlocked TD5 */
}
static int frontend_qr_track_excluded(int t) {
    /* mirrors frontend_track_excluded_from_selector(): drag strip + cups 20..25 */
    return t == FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX || (t >= 20 && t <= 25);
}
static int frontend_qr_track_exists(int t) {
    /* mirrors frontend_track_level_exists(): zip OR loose STRIP.DAT OR strip.json */
    char path[80];
    int level_num;
    if (t < 0) return 1;
    level_num = td5_asset_level_number(t);
    snprintf(path, sizeof(path), "level%03d.zip", level_num);
    if (td5_plat_file_exists(path)) return 1;
    snprintf(path, sizeof(path), "re/assets/levels/level%03d/STRIP.DAT", level_num);
    if (td5_plat_file_exists(path)) return 1;
    snprintf(path, sizeof(path), "re/assets/levels/level%03d/strip.json", level_num);
    return td5_plat_file_exists(path);
}

/* [item #7] Pick a random selectable car for the Quick Race CAR row (mirrors
 * frontend_pick_random_car). The QR cycler spans [0, TD5_CAR_COUNT-1]. */
static int frontend_qr_pick_random_car(void) {
    int cand[TD5_CAR_COUNT];
    int n = 0;
    for (int i = 0; i < TD5_CAR_COUNT; i++)
        if (frontend_qr_car_selectable(i) && i != s_selected_car) cand[n++] = i;
    if (n == 0) return s_selected_car;             /* only the current car qualifies */
    return cand[rand() % n];
}

/* [item #7] Pick a random selectable, present track for the Quick Race TRACK row
 * (mirrors frontend_pick_random_track + the QR cycler's exclusive bound). Writes
 * s_selected_track and returns 1 on change. */
static int frontend_qr_pick_random_track(void) {
    int track_max = s_total_unlocked_tracks; /* exclusive (net incl. TD6 26-36 now) */
    if (track_max <= 0) return 0;
    if (track_max > 64) track_max = 64;
    int cand[64];
    int n = 0;
    for (int t = 0; t < track_max; t++) {
        if (t == s_selected_track) continue;       /* prefer a change */
        if (frontend_qr_track_excluded(t)) continue;
        if (!frontend_qr_track_exists(t)) continue;
        cand[n++] = t;
    }
    if (n == 0) {                                  /* allow re-pick of the current */
        for (int t = 0; t < track_max; t++) {
            if (frontend_qr_track_excluded(t)) continue;
            if (!frontend_qr_track_exists(t)) continue;
            cand[n++] = t;
        }
        if (n == 0) return 0;
    }
    int pick = cand[rand() % n];
    if (pick == s_selected_track) return 0;
    s_selected_track = pick;
    return 1;
}

/* After a randomize-track, refresh the QR rows whose visibility depends on the
 * track (Direction toggle + circuit Laps row). The FSM (td5_fe_race.c) only does
 * this on an arrow-cycle, and its updater functions are file-static there, so we
 * replicate the effect here using the same accessible asset queries. Mirrors
 * frontend_update_direction_button_visibility(QR_BTN_DIRECTION,0) and
 * frontend_update_laps_button_visibility(QR_BTN_LAPS). */
static void frontend_qr_refresh_track_rows(void) {
    /* Direction toggle: shown iff the track has reverse assets. */
    if (QR_BTN_DIRECTION < s_button_count) {
        int has_rev = (s_selected_track < 0) ? 1
                      : td5_asset_track_has_reverse(s_selected_track);
        s_buttons[QR_BTN_DIRECTION].hidden   = !has_rev;
        s_buttons[QR_BTN_DIRECTION].disabled = !has_rev;
        if (!has_rev) {
            s_track_direction = 0;
            if (s_selected_button == QR_BTN_DIRECTION) s_selected_button = 0;
        }
    }
    /* Laps row: shown only for circuit tracks. */
    if (QR_BTN_LAPS < s_button_count) {
        int is_circuit;
        if (s_selected_track < 0) {
            is_circuit = 1;
        } else {
            int td6_level = td5_asset_td6_level_for_slot(s_selected_track);
            if (td6_level > 0)
                is_circuit = td5_asset_td6_finish_span_for_level(td6_level) > 0 ? 0 : 1;
            else
                is_circuit = td5_asset_track_has_reverse(s_selected_track) ? 0 : 1;
        }
        s_buttons[QR_BTN_LAPS].hidden   = !is_circuit;
        s_buttons[QR_BTN_LAPS].disabled = !is_circuit;
        if (!is_circuit && s_selected_button == QR_BTN_LAPS) s_selected_button = 0;
    }
}

/* Footprint of the QR randomize chip (matches frontend_draw_randomize_icon's
 * fixed 28x28 canvas chip in td5_fe_race.c). */
#define FE_QR_RAND_ICON_W 28
#define FE_QR_RAND_ICON_H 28
/* Canvas-space TOP-LEFT of the randomize chip for a given QR option row: parked
 * at the far right of the value column, vertically centred on the 32px row. */
static void frontend_qr_rand_icon_xy(int row_btn_idx, float *out_x, float *out_y) {
    int by = (row_btn_idx >= 0 && row_btn_idx < s_button_count)
             ? s_buttons[row_btn_idx].y : QR_ROW_Y(0);
    *out_x = (float)(FE_QR_SCREEN_W - FE_QR_RIGHT_MARGIN - FE_QR_RAND_ICON_W);
    *out_y = (float)by + (32.0f - (float)FE_QR_RAND_ICON_H) * 0.5f;
}

/* Roll the selector identified by `which` (0 = Car, 1 = Track) and emit cycle sfx
 * on a real change. [#10] NON-STATIC: the dedicated Randomize buttons' action in
 * the Screen_QuickRaceMenu FSM (td5_fe_race.c) calls this via an extern decl. */
void frontend_qr_roll_selector(int which) {
    if (which == 0) {
        int prev = s_selected_car;
        s_selected_car = frontend_qr_pick_random_car();
        if (s_selected_car != prev) frontend_play_sfx(2);
        TD5_LOG_I(LOG_TAG, "QuickRace randomize CAR: %d -> %d", prev, s_selected_car);
    } else {
        int prev = s_selected_track;
        if (frontend_qr_pick_random_track()) {
            /* Keep the Direction/Laps rows consistent with the new track (the FSM
             * only refreshes these on an arrow-cycle). */
            frontend_qr_refresh_track_rows();
            frontend_play_sfx(2);
            TD5_LOG_I(LOG_TAG, "QuickRace randomize TRACK: %d -> %d", prev, s_selected_track);
        }
    }
}

/* Per-frame INPUT for the Quick Race port-enhancement widgets. Runs at the top of
 * the render overlay (after the generic input poll and the FSM, which have already
 * run this frame). Mouse click on a randomize chip rolls that selector. Typed
 * chars only edit the dev span field WHILE it is input-active (QR_BTN_SPAN clicked/
 * activated, see frontend_qr_span_toggle_active); otherwise 'r'/'R' rolls the
 * focused selector. Inert when both knobs are off. Only active during the
 * interactive QR sub-state (and clears the span input latch when leaving it). */
static void frontend_quickrace_widget_input(void) {
    int icon_on = frontend_qr_random_icon_on();
    int span_on = 0;
#ifndef TD5RE_RELEASE
    span_on = frontend_qr_span_field_on();
#endif
    if (!icon_on && !span_on) return;
    if (s_inner_state < 4) {
        /* Still sliding in: drain any chars typed before the screen settled so a
         * stale digit / 'r' can't leak into the span field or fire a randomize on
         * entry. Also re-sync the span buffer from the live INI value (dev). */
        td5_plat_input_flush_chars();
#ifndef TD5RE_RELEASE
        s_qr_span_buf_init = 0;
        s_qr_span_active = 0;          /* never editable while sliding in */
#endif
    }
    if (s_inner_state != 4) {
#ifndef TD5RE_RELEASE
        s_qr_span_active = 0;          /* drop the input latch when leaving QR */
#endif
        return;                        /* interactive sub-state only */
    }
    if (!frontend_is_window_active()) return;

    /* --- Mouse: a click landing on a randomize chip (which is NOT an s_buttons[]
     * entry, so the generic poll hit-test ignored it and left s_mouse_clicked set
     * and s_button_index = -1). --- */
    if (icon_on && s_mouse_clicked) {
        float ix, iy;
        for (int which = 0; which <= 1; which++) {
            frontend_qr_rand_icon_xy(which == 0 ? QR_BTN_CAR : QR_BTN_TRACK, &ix, &iy);
            if ((float)s_mouse_x >= ix && (float)s_mouse_x < ix + FE_QR_RAND_ICON_W &&
                (float)s_mouse_y >= iy && (float)s_mouse_y < iy + FE_QR_RAND_ICON_H) {
                frontend_qr_roll_selector(which);
                break;
            }
        }
    }

    /* --- Keyboard: drain the WM_CHAR queue once. While the span field is INPUT-
     * ACTIVE (dev; toggled by clicking/activating QR_BTN_SPAN), digits/backspace/
     * '-' edit it and ALL other chars (incl. 'r') are swallowed so editing isn't
     * disrupted. When the field is NOT active, 'r'/'R' rolls the focused selector
     * (Car/Track) and other chars are discarded. Arrow/Enter navigation arrives
     * via the WM_KEYDOWN nav FIFO (already drained by poll), not here, so cycling/
     * OK/Back/the QR_BTN_SPAN toggle keep working. --- */
    int ch;
    while ((ch = td5_plat_input_get_char()) != 0) {
#ifndef TD5RE_RELEASE
        if (span_on && s_qr_span_active) {
            /* Editing the span field — capture digits/backspace/'-', swallow rest. */
            frontend_qr_span_sync_buf();
            int len = (int)strlen(s_qr_span_buf);
            if (ch == '\b') {                              /* backspace */
                if (len > 0) { s_qr_span_buf[len - 1] = '\0'; frontend_play_sfx(3); }
            } else if (ch == '-') {                        /* leading minus toggles sign */
                if (len == 0) { s_qr_span_buf[0] = '-'; s_qr_span_buf[1] = '\0'; frontend_play_sfx(3); }
            } else if (ch >= '0' && ch <= '9') {
                if (len < (int)sizeof(s_qr_span_buf) - 1) {
                    s_qr_span_buf[len] = (char)ch; s_qr_span_buf[len + 1] = '\0';
                    frontend_play_sfx(3);
                }
            }
            frontend_qr_span_commit();                     /* live parse/store */
            continue;                                      /* don't fall through to 'r' */
        }
#endif /* !TD5RE_RELEASE */
        if (icon_on && (ch == 'r' || ch == 'R')) {
            int focus = (s_selected_button == QR_BTN_TRACK) ? 1 : 0; /* default Car */
            frontend_qr_roll_selector(focus);
            continue;
        }
        /* Nothing else consumes WM_CHAR on this screen. */
    }
}

static void frontend_render_quick_race_overlay(float sx, float sy) {
    char car_name[80];
    char track_name[80];
    char count[8];
    int car_locked;
    int track_locked;
    if (!s_anim_complete) return;
    if (s_button_count <= QR_BTN_LAPS) return;

    /* Port-enhancement widget input (randomize chips #7 + dev span field #4).
     * Handled here in the render path because Screen_QuickRaceMenu (the FSM that
     * owns the button action switch) lives in the sibling td5_fe_race.c; these
     * widgets are self-contained and read the live per-frame input globals. */
    frontend_quickrace_widget_input();

    snprintf(car_name, sizeof(car_name), "%s", frontend_get_car_display_name(s_selected_car));
    frontend_get_track_display_name(s_selected_track, 0, track_name, sizeof(track_name));
    car_locked = (!s_cheat_unlock_all && !s_network_active &&
                  s_selected_car >= 0 && s_selected_car < 37 &&
                  s_car_lock_table[s_selected_car] != 0);
    track_locked = (!s_cheat_unlock_all && !s_network_active &&
                    s_selected_track >= 0 && s_selected_track < 37 &&
                    s_track_lock_table[s_selected_track] != 0);

    /* [item #7] Reserve the chip strip on the Car/Track rows so a long wrapped
     * name never slides under the randomize icon. 0 when the icon is off.
     * [#10] When the randomize controls are real BUTTONS (the new default), the
     * Randomize button sits at the far-right of the value column instead, so
     * reserve its (wider) footprint so the name wraps before it. */
    int rand_reserve = frontend_qr_random_button_on() ? QR_RAND_RESERVE
                     : frontend_qr_random_icon_on()   ? (FE_QR_RAND_ICON_W + 6)
                     : 0;

    /* Each selected value renders in the right-hand value column at the same
     * glyph size as the button caption, vertically centered on its row, and
     * wraps to a second line if it would run off the right edge. Car/Track show
     * the name; Direction shows Forwards/Backwards (only when the row is
     * visible); Players/Opponents show the count; Laps shows value+1. */
    frontend_draw_qr_value(sx, sy, QR_BTN_CAR,   car_locked   ? "LOCKED" : car_name,   0xFFFFFFFF, rand_reserve);
    frontend_draw_qr_value(sx, sy, QR_BTN_TRACK, track_locked ? "LOCKED" : track_name, 0xFFFFFFFF, rand_reserve);
    if (!s_buttons[QR_BTN_DIRECTION].hidden) {
        frontend_draw_qr_value(sx, sy, QR_BTN_DIRECTION,
                               s_track_direction ? "Backwards" : "Forwards", 0xFFFFFFFF, 0);
    }
    /* Players row is hidden (Quick Race is single-player); only Opponents shown. */
    if (!s_buttons[QR_BTN_PLAYERS].hidden) {
        snprintf(count, sizeof(count), "%d", s_num_human_players);
        frontend_draw_qr_value(sx, sy, QR_BTN_PLAYERS, count, 0xFFFFFFFF, 0);
    }
    snprintf(count, sizeof(count), "%d", s_num_ai_opponents);
    frontend_draw_qr_value(sx, sy, QR_BTN_OPPONENTS, count, 0xFFFFFFFF, 0);
    /* [S02 (c) 2026-06-04] Circuit laps value (displayed as laps+1, matching the
     * Game Options + Track Selection convention). Hidden on point-to-point tracks
     * (frontend_update_laps_button_visibility) — no laps there. */
    if (!s_buttons[QR_BTN_LAPS].hidden) {
        snprintf(count, sizeof(count), "%d", s_game_option_laps + 1);
        frontend_draw_qr_value(sx, sy, QR_BTN_LAPS, count, 0xFFFFFFFF, 0);
    }
    /* [2026-06-08] AI Screens value (dev-only; row hidden in release). */
    if (s_button_count > QR_BTN_SPLITSCREENS &&
        !s_buttons[QR_BTN_SPLITSCREENS].hidden) {
        snprintf(count, sizeof(count), "%d", s_num_spectate_screens);
        frontend_draw_qr_value(sx, sy, QR_BTN_SPLITSCREENS, count, 0xFFFFFFFF, 0);
    }

    /* [item #7] Randomize chips to the RIGHT of the Car and Track selectors. Each
     * is lit when its row holds focus (mouse click or 'r'/'R' rolls it — handled
     * in frontend_quickrace_widget_input above). NOT s_buttons[] entries, so the
     * Car/Track/.../OK/Back nav ring is untouched. */
    if (frontend_qr_random_icon_on()) {
        float ix, iy;
        const int iw = FE_QR_RAND_ICON_W, ih = FE_QR_RAND_ICON_W;
        /* [fix 2026-06-15] The chip lights only when the MOUSE is over the icon
         * itself — NOT when the adjacent Car/Track row is focused (which made the
         * chip look "selected" while hovering Car/Track). */
        frontend_qr_rand_icon_xy(QR_BTN_CAR, &ix, &iy);
        frontend_draw_randomize_icon(ix, iy, sx, sy,
            s_mouse_x >= (int)ix && s_mouse_x < (int)ix + iw &&
            s_mouse_y >= (int)iy && s_mouse_y < (int)iy + ih);
        frontend_qr_rand_icon_xy(QR_BTN_TRACK, &ix, &iy);
        frontend_draw_randomize_icon(ix, iy, sx, sy,
            s_mouse_x >= (int)ix && s_mouse_x < (int)ix + iw &&
            s_mouse_y >= (int)iy && s_mouse_y < (int)iy + ih);
    }

#ifndef TD5RE_RELEASE
    /* [item #4 / TASK A1] DEV-ONLY span-offset field, now a real BUTTON (QR_BTN_SPAN,
     * its own row below "AI Screens"). The button caption is "Span Offset"; the
     * current g_td5.ini.start_span_offset renders in the value column like every
     * other QR row. Activating the button (A/Enter or click) toggles input-active
     * (frontend_qr_span_toggle_active) — while active the widget input captures
     * digits/backspace/'-' and we draw a blinking caret after the value. The value
     * is the relative per-slot offset td5_game.c InitRace applies with 16-bit wrap
     * to BOTH TD5 and TD6 tracks. Hidden in release (button not shown). */
    if (frontend_qr_span_field_on() && s_button_count > QR_BTN_SPAN &&
        !s_buttons[QR_BTN_SPAN].hidden && (td5_font_ready() || s_font_page >= 0)) {
        frontend_qr_span_sync_buf();
        /* While editing, show the live buffer (so a lone "-" or empty mid-edit is
         * visible); otherwise show the committed numeric value. */
        const char *shown = s_qr_span_active
                            ? (s_qr_span_buf[0] ? s_qr_span_buf : "_")
                            : NULL;
        char vbuf[16];
        if (!shown) { snprintf(vbuf, sizeof(vbuf), "%d", g_td5.ini.start_span_offset); shown = vbuf; }
        /* Highlight the value while editing so the active field stands out. */
        uint32_t vcol = s_qr_span_active ? 0xFF80FF80u : 0xFFFFFFFFu;
        frontend_draw_qr_value(sx, sy, QR_BTN_SPAN, shown, vcol, 0);
        if (s_qr_span_active) {
            /* Blinking caret just past the value text (value column @ FE_QR_VALUE_X,
             * same glyph scale as frontend_draw_qr_value). */
            const float gs = FE_QR_VALUE_SCALE;
            int by = s_buttons[QR_BTN_SPAN].y;
            float ty = ((float)by + (32.0f - FE_QR_VALUE_LINE_H) * 0.5f) * sy;
            float tw = fe_measure_text(shown, sx * gs, sy * gs);
            uint32_t blink = ((td5_plat_time_ms() / 500u) & 1u) ? 0xFF80FF80u : 0x3380FF80u;
            /* [2026-06-15 BUG #14] frontend_draw_qr_value draws the value via
             * fe_draw_text at `ty` = the glyph CELL-TOP, and fe_draw_text places
             * caps at design rows 8..23 of the 24px cell (cap height 15px). Size the
             * caret to that exact cap band so it is as tall as the digits and sits
             * ON them. The prior nudge (3px down, 12px tall) sat above/short of the
             * digits; TD5RE_SPAN_CARET=0 restores it. */
            float cy = ty + 3.0f * sy * gs, ch = 12.0f * sy * gs;  /* legacy (over-shrunk) */
            if (frontend_span_caret_fix_on()) {
                cy = ty + 8.0f * sy * gs;                          /* cap top  (design row 8)  */
                ch = 15.0f * sy * gs;                              /* cap height (rows 8..23)  */
            }
            fe_draw_quad((float)FE_QR_VALUE_X * sx + tw + 2.0f * sx, cy,
                         6.0f * sx * gs, ch, blink, -1, 0, 0, 0, 0);
        }
    }
#endif /* !TD5RE_RELEASE */
}

static void fe_draw_option_arrows(int btn_idx, float sx, float sy) {
    /* Selector ◄► arrows drawn procedurally via the triangle-SDF shader
     * (ps_arrow). [2026-06-16] The legacy ArrowButtonz.tga 12x36 sprite-sheet
     * bitmap fallback was retired — procedural arrows are permanent now. */
    float bx, by, bw, bh;
    /* Skip hidden buttons (e.g. the Direction toggle on forward-only/circuit
     * tracks) — the selector arrows must vanish with the button frame+label,
     * not leave an empty ◄ ► row floating where the button used to be. */
    if (!s_buttons[btn_idx].active || s_buttons[btn_idx].hidden) return;
    if (!s_ps_arrow) return;
    frontend_get_button_render_rect(btn_idx, sx, sy, &bx, &by, &bw, &bh);

    /* Procedural triangle-SDF arrows (crisp + AA at any resolution). */
    {
        float aw2 = 13.0f * sx, ah2 = 13.0f * sy;
        float ay  = by + (bh - ah2) * 0.5f;
        uint32_t acol = 0xFF7995FFu;  /* bright selector blue */
        fe_draw_arrow_proc(bx + 4.0f * sx,          ay, aw2, ah2, 0 /*left */, acol);
        fe_draw_arrow_proc(bx + bw - 4.0f*sx - aw2, ay, aw2, ah2, 1 /*right*/, acol);
    }
}

static void frontend_render_game_options_overlay(float sx, float sy) {
    const char *on_off[] = { "OFF", "ON" };
    /* [dynamic-traffic] 5-state traffic volume row. */
    const char *traffic_vol[TD5_TRAFFIC_VOLUME_COUNT] = { "OFF", "LOW", "MEDIUM", "HIGH", "VERY HIGH" };
    const char *difficulty[] = { "EASY", "NORMAL", "HARD" };  /* orig middle label: NORMAL */
    /* [CONFIRMED @ Language.dll SNK_DynamicsTxt, indexed directly by
     * gDynamicsConfigShadow @0x00466014 at ScreenGameOptions 0x0041FECF]:
     * value 0 -> "ARCADE", value 1 -> "SIMULATION". The previous order was
     * inverted. Physics: 0=ARCADE (gravity 1900 + car-stat boosts),
     * 1=SIMULATION (gravity 1500 + stock stats). */
    const char *dynamics[] = { "ARCADE", "SIMULATION" };
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    /* [S02 (c) 2026-06-04] Circuit Laps row removed from this screen; the six
     * remaining option values shifted up one button index. Laps is now shown and
     * edited in the Quick Race menu and the Track Selection screen. */
    frontend_draw_value_centered(sx, sy, s_buttons[0].y + 6, on_off[s_game_option_checkpoint_timers & 1], 0xFFFFFFFF);
    {
        int tvi = s_game_option_traffic;
        if (tvi < 0) tvi = 0;
        if (tvi > TD5_TRAFFIC_VOLUME_COUNT - 1) tvi = TD5_TRAFFIC_VOLUME_COUNT - 1;
        frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, traffic_vol[tvi], 0xFFFFFFFF);
    }
    frontend_draw_value_centered(sx, sy, s_buttons[2].y + 6, on_off[s_game_option_cops & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[3].y + 6, difficulty[s_game_option_difficulty % 3], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[4].y + 6, dynamics[s_game_option_dynamics & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[5].y + 6, on_off[s_game_option_collisions & 1], 0xFFFFFFFF);
}

static void frontend_render_display_options_overlay(float sx, float sy) {
    char damping[16];
    const char *on_off[]     = { "OFF", "ON" };
    const char *speed_read[] = { "MPH", "KPH" };
    const char *win_mode[]   = { "FULLSCREEN", "WINDOWED", "BORDERLESS" };
    int wm = s_display_window_mode;
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    if (wm < 0 || wm > 2) wm = 1;
    snprintf(damping, sizeof(damping), "%d", s_display_camera_damping);
    /* Rows: 0 Display Mode, 1 VSync, 2 Fogging,
     *       3 Speed Readout, 4 Show FPS, 5 Camera Damping. */
    frontend_draw_value_centered(sx, sy, s_buttons[0].y + 6, win_mode[wm], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, on_off[s_display_vsync & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[2].y + 6, on_off[s_display_fog_enabled & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[3].y + 6, speed_read[s_display_speed_units & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[4].y + 6, on_off[s_display_show_fps & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[5].y + 6, damping, 0xFFFFFFFF);
}

static void frontend_render_sound_options_overlay(float sx, float sy) {
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    /* [PORT REWORK 2026-06-05 / S15] The SFX-mode row (Stereo/Mono/3D icon +
     * MONAURAL/STEREO/3D SOUND name) was removed. Only the two volume bars are
     * drawn now. Volume levels are indicated by bar fill only; no numbers. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);

    /* Volume bars: SFX = button[0], Music = button[1] (re-indexed after the
     * SFX Mode row was removed). Each bar sits to the right of its button at
     * x=394, vertically centred in the button height (32px). Bar=12px, fill=10px. */
    {
        int bar_btns[2]  = { 0, 1 }; /* SFX Volume, Music Volume */
        int vols[2]      = { s_sound_option_sfx_volume, s_sound_option_music_volume };

        for (int vi = 0; vi < 2; vi++) {
            int   btn    = bar_btns[vi];
            float bar_x  = 394.0f * sx; /* panel x = canvasW/2+0x4A [CONFIRMED @ 0x41FF5B] */
            float bar_y  = ((float)s_buttons[btn].y + 10.0f) * sy; /* centre 12px in 32px */
            float fill_y = ((float)s_buttons[btn].y + 11.0f) * sy; /* centre 10px in 32px */
            int   vol    = vols[vi];
            float fill_w = (float)vol / 100.0f * 222.0f * sx;

            if (s_sound_volumebox_surface > 0) {
                int slot = s_sound_volumebox_surface - 1;
                if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use)
                    fe_draw_quad(bar_x, bar_y, 224.0f * sx, 12.0f * sy,
                                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
            }
            if (fill_w > 0.0f && s_sound_volumefill_surface > 0) {
                int slot = s_sound_volumefill_surface - 1;
                if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
                    float u1 = (float)vol / 100.0f;
                    fe_draw_quad(bar_x + 1.0f * sx, fill_y, fill_w, 10.0f * sy,
                                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, 0.0f, u1, 1.0f);
                }
            }
        }
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}



/* ========================================================================
 * Music Test overlay: track-name label + Now Playing panel.
 *
 * Original (0x418460) drew into two offscreen DDraw surfaces:
 *   DAT_0049628c  (0x170 x 0x28)  — track-name label, format "%d. %s"
 *   DAT_00496400  (0x170 x 0x78)  — now-playing panel:
 *       y=0:    "NOW PLAYING" header
 *       y=0x28: band name
 *       y=0x50: song title
 * Port renders these as live text in the UI-rect pass (no offscreen surfaces).
 *
 * Layout derived from QueueFrontendOverlayRect calls in case 4/5/6:
 *   track-name panel: x = canvasW/2 - 0x32,  y = canvasH/2 - 0x8f  (≈320-50, 240-143)
 *   now-playing panel:x = canvasW/2 - 0x0c,  y = canvasH/2 - 0x3f  (≈320-12, 240-63)
 * [CONFIRMED @ 0x418527-0x41853E: QueueFrontendOverlayRect offsets]
 * ======================================================================== */
static void frontend_render_music_test_overlay(float sx, float sy) {
    float cx = 320.0f * sx;
    float cy = 240.0f * sy;

    if (!s_anim_complete) return;

    /* Album cover art for the current track: band = LUT[track], drawn at (118,140).
     * The headline previously-missing element. [CONFIRMED @ 0x40d830
     * UpdateExtrasGalleryDisplay reads g_extrasGallerySlideSurfaces[LUT[track]] and
     * blits at (0x76,0x8c); LUT @0x465e4c.] */
    if (s_music_attract_track >= 0 && s_music_attract_track < 12) {
        int band = k_music_track_to_band[s_music_attract_track];
        if (band >= 0 && band < 5) {
            int cs = s_band_cover_surface[band];
            if (cs > 0) {
                int slot = cs - 1;
                if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
                    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
                    /* [CONFIRMED @0x40d190 CrossFade16BitSurfaces blits the slide at
                     * desc.lWidth x desc.lHeight = its NATIVE size] dest origin
                     * (0x76,0x8c)=(118,140); band-cover surfaces are the TGAs' own size
                     * (extracted PNGs = 224x224). No fixed/stretched size. */
                    fe_draw_quad(118.0f * sx, 140.0f * sy,
                                 (float)s_surfaces[slot].width  * sx,
                                 (float)s_surfaces[slot].height * sy,
                                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
                }
            }
        }
    }

    /* Track-name label, centered in its 0x170(368)-wide box at x=cx-0x32 (center 454).
     * [orig draws it via MeasureOrCenter into the track-# box.] */
    if (s_music_test_track_label[0]) {
        /* [CONFIRMED @0x418527] centered in the 0x170-wide track-# box at x=cx-0x32
         * (screen center 454); text baseline = box_top + 8. */
        fe_draw_text_centered((cx - 0x32 * sx) + 184.0f * sx, (cy - 0x8f * sy) + 8.0f * sy,
                              s_music_test_track_label, 0xFFFFFFFF, sx, sy);
    }

    /* Now-playing panel (after a track is played), centered in its box at x=cx-0xc
     * (center 492). Header = SNK_NowPlayingTxt = "NOW PLAYING:". */
    if (s_music_test_playing_set) {
        float ncx = (cx - 0x0c * sx) + 184.0f * sx;
        float y0 = cy - 0x3f * sy;
        float y1 = y0 + 0x28 * sy;
        float y2 = y0 + 0x50 * sy;
        fe_draw_text_centered(ncx, y0, "NOW PLAYING:", 0xFFCCCCCC, sx, sy);
        fe_draw_text_centered(ncx, y1, s_music_test_now_band,  0xFFFFFFFF, sx, sy);
        fe_draw_text_centered(ncx, y2, s_music_test_now_title, 0xFFFFFFFF, sx, sy);
    }
}

static void frontend_render_two_player_options_overlay(float sx, float sy) {
    /* [PORT ENHANCEMENT 2026-06] Multiplayer Options value column. Draws the
     * current value for each dynamic row (PLAYERS, CATCHUP, SPLIT LAYOUT,
     * DISPLAY k) at the standard value-column X, aligned to each row's button Y. */
    char buf[16];

    if (!s_anim_complete) return;

    /* [title font 2026-06] The MULTIPLAYER sub-header was removed — this screen
     * already shows the shared "OPTIONS" header at the top, so the extra
     * orange "MULTIPLAYER" line below it was redundant. */

    if (s_mp_btn_players < 0 || !s_buttons[s_mp_btn_players].active) return;

    /* PLAYERS = N */
    snprintf(buf, sizeof buf, "%d", s_num_human_players);
    frontend_draw_value_centered(sx, sy, s_buttons[s_mp_btn_players].y + 6, buf, 0xFFFFFFFF);

    /* [S05 2026-06-04] CATCHUP = ON / OFF (persisted AI rubber-band assist,
     * read live via td5_save_get_catchup_assist; consumed by S06). */
    if (s_mp_btn_catchup >= 0 && s_buttons[s_mp_btn_catchup].active) {
        frontend_draw_value_centered(sx, sy, s_buttons[s_mp_btn_catchup].y + 6,
                                     td5_save_get_catchup_assist() > 0 ? "ON" : "OFF",
                                     0xFFFFFFFF);
    }

    /* SPLIT LAYOUT = current layout label */
    if (s_mp_btn_layout >= 0 && s_buttons[s_mp_btn_layout].active) {
        int cnt = 1;
        const MpSplitLayout *opts = mp_split_layouts(s_num_human_players, &cnt);
        int sel = s_mp_layout_sel;
        if (sel < 0 || sel >= cnt) sel = 0;
        frontend_draw_value_centered(sx, sy, s_buttons[s_mp_btn_layout].y + 6,
                                     opts[sel].label, 0xFFFFFFFF);
    }

    /* DISPLAY k = stub content label for each empty cell (rendering deferred). */
    for (int k = 0; k < s_mp_missing_count && k < 2; k++) {
        int v;
        if (s_mp_btn_missing[k] < 0 || !s_buttons[s_mp_btn_missing[k]].active) continue;
        v = s_mp_missing_content[k];
        if (v < 0 || v >= MP_MISSING_CONTENT_COUNT) v = 0;
        frontend_draw_value_centered(sx, sy, s_buttons[s_mp_btn_missing[k]].y + 6,
                                     k_mp_missing_content[v], 0xFFFFFFFF);
    }

    /* S10: NICKNAME row shows the current net-play nickname as its value. */
    if (s_mp_btn_nickname >= 0 && s_buttons[s_mp_btn_nickname].active) {
        const char *nick = g_td5.ini.net_nickname[0] ? g_td5.ini.net_nickname : "Player";
        frontend_draw_value_centered(sx, sy, s_buttons[s_mp_btn_nickname].y + 6,
                                     nick, 0xFFFFFFFF);
    }
}

/* ScreenLanguageSelect overlay: full-screen LanguageScreen.tga bg + 4 flag IMAGE tiles
 * from Language.tga (176x512 = four 176x128 tiles, src V 0/128/256/384) at the 4 corners
 * + "LANGUAGE SELECT" header. [CONFIRMED @0x00427290; header literal @0x004667c0;
 * flag dest rects TL(40,128) TR(424,128) BL(40,320) BR(424,320), 176x128.] */
static void frontend_render_language_select_overlay(float sx, float sy) {
    /* Background (drawn opaque, full screen) */
    if (s_language_bg_surface > 0) {
        int slot = s_language_bg_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xFFFFFFFF,
                         s_surfaces[slot].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    /* 4 flag tiles (each 176x128 from the 176x512 sheet; src V = row/4) */
    if (s_language_flag_surface > 0) {
        int slot = s_language_flag_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            /* [FIXED 2026-06-01, audit+verify] orig left col x = uVar2 = 72, right col =
             * (canvasW-uVar2)-0xb0 = 640-72-176 = 392 (not 40/424). Y 128/320 already faithful. */
            static const int dx[4] = { 72, 392, 72, 392 };
            static const int dy[4] = { 128, 128, 320, 320 };
            int fi;
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            for (fi = 0; fi < 4; fi++) {
                float v0 = (float)fi / 4.0f, v1 = (float)(fi + 1) / 4.0f;
                fe_draw_quad((float)dx[fi] * sx, (float)dy[fi] * sy,
                             176.0f * sx, 128.0f * sy, 0xFFFFFFFF,
                             s_surfaces[slot].tex_page, 0.0f, v0, 1.0f, v1);
            }
        }
    }
    /* Header "LANGUAGE SELECT" (in-EXE literal @0x4667c0), 24px font, at (uVar2=72, 34).
     * [FIXED 2026-06-01: header X was 40, orig draws at uVar2=72 — same left margin as the flags.] */
    fe_draw_text(72.0f * sx, 34.0f * sy, "LANGUAGE SELECT", 0xFFFFFFFF, sx, sy);
}

static void frontend_render_race_type_description(float sx, float sy) {
    /* [FIXED 2026-06-01, corrected] Orig (0x4168B0 case 4/9) draws a MULTI-LINE localized
     * description into a 0x110x0xB4 (272x180) panel: line 0 = race-type NAME (big font,
     * centered, Y=0); lines 1.. = description (12px small font, centered, Y=32 step +12,
     * stop at Y>=176). The text is SNK_RaceTypeText[gt] — each entry is a NUL-separated
     * line LIST (double-NUL terminated), re-extracted byte-faithfully from Language.dll.
     * (My prior "name-only" version was wrong: the SNK dump had truncated each entry to
     * its first segment, so I deleted the real descriptions. These ARE the original text.) */
    static const char *k_race_desc[12][13] = {
        /* 0 SINGLE RACE */
        {"SINGLE RACE"," ","DEFEAT A COURSE OR","CIRCUIT AT NORMAL","DIFFICULTY BY COMING","IN FIRST TO UNLOCK A","REVERSE TRACK OR SECRET","CAR",0,0,0,0,0},
        /* 1 CHAMPIONSHIP CUP */
        {"CHAMPIONSHIP CUP"," ","TOTAL POINTS OVER 4 COURSES:","POINTS BASED ON YOUR FINISHING","POSITION IN EACH RACE."," ","MOSCOW, RUSSIA","EDINBURGH, SCOTLAND","SYDNEY, AUSTRALIA","BLUE RIDGE PARKWAY, NC",0,0,0},
        /* 2 ERA CUP */
        {"ERA CUP","TOTAL TIME OVER ALL 6 CIRCUITS.","JARASH, JORDAN","CHEDDAR CHEESE, ENGLAND","MAUI, HAWAII, USA","COURMAYEUR, ITALY","NEWCASTLE, ENGLAND","MONTEGO BAY, JAMAICA"," ","YOUR OPPONENTS AND YOUR","CAR WILL BE FROM EITHER THE","BEAUTY (NEW CARS) OR BEAST","(OLD CARS) DIVISIONS."},
        /* 3 CHALLENGE CUP */
        {"CHALLENGE CUP"," ","TOTAL TIME OVER 6 COURSES."," ","MOSCOW, RUSSIA","EDINBURGH, SCOTLAND","SYDNEY, AUSTRALIA","BLUE RIDGE PARKWAY, NC","MUNICH, GERMANY","HONOLULU, HAWAII",0,0,0},
        /* 4 PITBULL CUP */
        {"PITBULL CUP","YOU MUST PLACE FIRST ON","EACH OF 8 COURSES ON","NORMAL DIFFICULTY TO MOVE","ON TO THE NEXT CUP RACE.","MOSCOW, RUSSIA","EDINBURGH, SCOTLAND","SYDNEY, AUSTRALIA","BLUE RIDGE PARKWAY, NC","MUNICH, GERMANY","HONOLULU, HAWAII","SAN FRANCISCO, CA, USA","KYOTO, JAPAN"},
        /* 5 MASTERS CUP */
        {"MASTERS CUP"," ","TOTAL TIME OVER 10 COURSES."," ","10 RANDOMLY CHOSEN CARS","WILL BE AT YOUR DISPOSAL."," ","AFTER YOU USE A VEHICLE IN","YOUR MOTOR POOL, YOU MAY","NOT USE IT AGAIN FOR THE","DURATION OF THE CUP.",0,0},
        /* 6 ULTIMATE CUP */
        {"ULTIMATE CUP"," ","TOTAL POINTS OVER 12 COURSES."," ","POINTS ARE TABULATED FOR","AVERAGE SPEED OVER THE","LENGTH OF THE COURSE."," ","POINTS STOP ACCUMULATING","FOR BEING STOPPED BY THE COPS,","FOR CRASHES, HITTING WALLS AND","RUNNING OFF THE ROAD.",0},
        /* 7 DRAG RACING */
        {"DRAG RACING"," ","CHOOSE YOUR CAR.....","THEN THROW DOWN THE HAMMER","AND WATCH THE SMOKE FLY!",0,0,0,0,0,0,0,0},
        /* 8 COP CHASE */
        {"COP CHASE"," ","CHOOSE ONE OF THE COP CARS","AND PULL OVER THOSE LAWLESS","HOOLIGANS WHO ARE SPEEDING","THROUGH YOUR TOWN. HIT THE SIREN","AND SPIN THE CARS OUT TO GIVE OUT","THE TICKETS AND INSURANCE POINTS.",0,0,0,0,0},
        /* 9 TIME TRIALS */
        {"TIME TRIALS"," ","TAKE ON THE CLOCK WITH","TRAFFIC TO SEE HOW YOU FARE","IN A RACE AGAINST YOUR OWN","SKILL."," ","TIME IS KEPT FOR ALL","CHECKPOINTS AND TABULATED","AT THE END OF THE RACE.",0,0,0},
        /* 10 CUP RACE */
        {"CUP RACE"," ","GO UP AGAINST THE WORLDS BEST","RACERS IN A NON-SANCTIONED","TOURNAMENT.",0,0,0,0,0,0,0,0},
        /* 11 CONTINUE CUP */
        {"CONTINUE CUP"," ","YOU MAY SAVE A CUP RACE AND","CONTINUE IT LATER."," ","CHOOSE CONTINUE CUP TO","LOAD THE SAVED CUP RACE.",0,0,0,0,0,0},
    };
    /* Top menu buttons (0 Single Race,1 Cup Race,2 Continue Cup,3 Time Trials,4 Drag Race,
     * 5 Cop Chase,6 Back) → SNK_RaceTypeText index. Cup sub-menu (0..5 Championship..
     * Ultimate,6 Back) → index 1..6. [CONFIRMED @ 0x416e45 / 0x4171xx button→gameType map.] */
    static const int k_top_to_idx[7] = { 0, 10, 11, 9, 7, 8, -1 };
    static const int k_cup_to_idx[7] = { 1, 2, 3, 4, 5, 6, -1 };
    int btn = s_selected_button;
    float panel_x = 358.0f * sx;   /* cx + 0x26 = 320 + 38 = 358 */
    float panel_y = 145.0f * sy;   /* cy - 0x5f = 240 - 95 = 145 */
    float panel_w = 272.0f * sx;   /* 0x110 */
    int idx, line;

    if (!s_anim_complete) return;
    if (btn < 0 || btn > 6) btn = 0;

    /* [PORT ENHANCEMENT 2026-06] Active-controller indicator above the game-mode
     * description panel: a "CONTROLLER" title with the device name below it,
     * wrapped to up to two lines. Shows which controller drives the menus (and,
     * for single-player, will be the driver). */
    {
        const char *dn = (s_active_menu_device == 0)
            ? "KEYBOARD" : td5_input_get_device_name(s_active_menu_device);
        float cx   = panel_x + panel_w * 0.5f;
        float ts   = 0.7f;
        if (!dn || !dn[0]) dn = "KEYBOARD";
        fe_draw_text_centered(cx, 90.0f * sy, "CONTROLLER", 0xFFFFD000, sx*0.8f, sy*0.8f);
        {
            /* Word-wrap the controller name over up to 3 lines (capped so it
             * stays above the race-type description panel at y~145). */
            char lines[3][64];
            int nl = fe_wrap_text_lines(dn, panel_w, sx*ts, sy*ts, lines, 3);
            float step = 14.0f, base = 113.0f - (float)(nl - 1) * step * 0.5f;
            int li;
            for (li = 0; li < nl; li++)
                fe_draw_text_centered(cx, (base + (float)li * step) * sy, lines[li],
                                      0xFFFFFFFF, sx*ts, sy*ts);
        }
    }

    idx = (s_inner_state >= 6 && s_inner_state <= 12) ? k_cup_to_idx[btn] : k_top_to_idx[btn];
    if (idx < 0) return;  /* "Back" has no description */

    /* Line 0: race-type NAME, big font, centered at Y=0. */
    fe_draw_text(panel_x + (panel_w - fe_measure_text(k_race_desc[idx][0], sx, sy)) * 0.5f,
                 panel_y + 2.0f * sy, k_race_desc[idx][0], 0xFFFFFFFF, sx, sy);
    /* Lines 1..: the description body, drawn in the SMALL font (smalltext) — the original
     * RaceTypeCategoryMenuStateMachine@0x4168b0 word-wraps the description and draws it with
     * DrawFrontendSmallFontStringToSurface@0x424660 (loop @0x416ea1, panel y 0x20→0xb0,
     * width 0x110). The port previously approximated it with 0.5×-scaled BodyText (wrong
     * font face). [FIXED 2026-06-01] Centered via fe_measure_small_text; Y=32 step +12. */
    for (line = 1; line < 13 && k_race_desc[idx][line]; line++) {
        float ly = 32.0f + (float)(line - 1) * 12.0f;
        if (ly >= 176.0f) break;
        const char *s = k_race_desc[idx][line];
        fe_draw_small_text(panel_x + (panel_w - fe_measure_small_text(s) * fe_glyph_sx(sx, sy)) * 0.5f,
                           panel_y + ly * sy, s, 0xFFFFFFFF, sx, sy);
    }
}

/* --- Car stats sub-screen (0x40DFC0 state 0xF) ------------------------------------ */

/* SNK_Layout_Types (Language.dll 0x10006ED0): char - 'A' = index */
static const char *k_stat_layout_types[] = {
    "FRONT/REAR", "FRONT/4-WHEEL", "FRONT/FRONT",
    "MID/4-WHEEL", "MID/REAR", "UNKNOWN"
};
/* SNK_Engine_Types (Language.dll 0x10006EE8): char - 'A' = index */
static const char *k_stat_engine_types[] = {
    "V10 ALUMINIUM", "V8 ALUMINIUM BLOCK", "V8 SUPERCHARGED",
    "V8 ALUMINIUM", "DOHC TWIN TURBO", "V8",
    "FORD IRON BLOCK", "PONTIAC IRON BLOCK", "V8 IRON BLOCK",
    "V8 IRON BLOCK HEMI", "V12", "ALUMINIUM BLOCK",
    "4 CYLINDER", "V6 SUPERCHARGED", "V10 SUPERCHARGED",
    "V8 DOHC", "V8 TWIN TURBO", "V12 IRON BLOCK", "UNKNOWN"
};

void frontend_load_car_spec_fields(int car_index) {
    int sz = 0, field;
    size_t i;
    char *data;
    if (car_index == s_car_spec_car) return;
    s_car_spec_car = car_index;
    for (field = 0; field < 17; field++) s_car_spec[field][0] = '\0';
    if (car_index < 0 || car_index >= TD5_CAR_COUNT) return;
    data = (char *)td5_asset_open_and_read("config.nfo", s_car_zip_paths[car_index], &sz);
    if (!data || sz <= 0) return;
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(s_car_spec[0]))
                s_car_spec[field][j++] = data[i];
            i++;
        }
        s_car_spec[field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
}

static void frontend_fmt_spec(char *out, size_t cap, const char *raw) {
    size_t i;
    for (i = 0; raw[i] && i + 1 < cap; i++)
        out[i] = (raw[i] == '_') ? ' ' : raw[i];
    out[i] = '\0';
}

static void frontend_render_car_stats_overlay(float sx, float sy) {
    /* 14 rows: SNK_Config_Hdrs (Language.dll 0x10006e80) + config.nfo fields 2-16.
     * Rendered in the car preview area (x=232, y=124, 408x280).
     * Four visual groups match the original binary's loop structure (0x40DFC0 case 0xF):
     *   Group A (y=124): LAYOUT, GEARS, PRICE, TIRES      (canvasH-0x164, step 0xC)
     *   Group B (y=196): TOP SPEED, 0 TO 60, 60 TO 0, 1/4 (canvasH-0x11C, step 0xC)
     *   ENGINE  (y=256): alone                             (canvasH-0xE0)
     *   Group C (y=280): COMPRESSION, DISPLACEMENT, LAT.  (canvasH-0xC8, step 0xC)
     *   Group D (y=328): TORQUE, HP                        (canvasH-0x98, step 0xC)
     * exp: 0=raw value, 1=layout type, 2=engine type, 3=tire pair (fi + fi+1)
     * sfx: unit suffix appended to raw values (English equivalents of SNK_ConfSpeed/Mph/Sec/Ft) */
    static const struct { const char *hdr; int fi; int exp; float y; const char *sfx; } k_rows[] = {
        { "LAYOUT:",       2,  1, 124.0f, NULL   },
        { "GEARS:",        3,  0, 136.0f, NULL   },
        { "PRICE:",        4,  0, 148.0f, NULL   },
        { "TIRES:",        5,  3, 160.0f, NULL   },
        { "TOP SPEED:",    7,  0, 196.0f, " MPH" },
        { "0 to 60 MPH:",  8,  0, 208.0f, " sec" },  /* [FIXED] lowercase "to" — SNK_Config_Hdrs */
        { "60 to 0 MPH:",  9,  0, 220.0f, " ft"  },  /* [FIXED] lowercase "to" — SNK_Config_Hdrs */
        { "1/4 MILE:",    10,  0, 232.0f, " sec" },
        { "ENGINE:",      11,  2, 256.0f, NULL   },
        { "COMPRESSION:", 12,  0, 280.0f, NULL   },
        { "DISPLACEMENT:",13,  0, 292.0f, NULL   },
        { "LATERAL ACC:", 14,  0, 304.0f, NULL   },
        { "TORQUE:",      15,  0, 328.0f, NULL   },
        { "HP:",          16,  0, 340.0f, NULL   },
    };
    int n_layout = (int)(sizeof(k_stat_layout_types)/sizeof(k_stat_layout_types[0]));
    int n_engine = (int)(sizeof(k_stat_engine_types)/sizeof(k_stat_engine_types[0]));
    float hx = 232.0f * sx;   /* label column x = canvasW - 0x198 */
    /* Value column: label_x + max_label_width + 16px gap. [FIXED 2026-06-01] These spec
     * rows are drawn in the SMALL font in the original (CarSelectionScreenStateMachine
     * @0x40dfc0, 10+ DrawFrontendSmallFontStringToSurface calls @0x40ee82..0x40f11b), not
     * the scaled button font — measure with the small font. */
    float vx = hx + fe_measure_small_text("COMPRESSION:") * fe_glyph_sx(sx, sy) + 16.0f * sx;
    char val[64];
    int i;

    TD5_LOG_I(LOG_TAG, "car_stats_overlay: car=%d", s_car_spec_car);

    for (i = 0; i < 14; i++) {
        float y = k_rows[i].y * sy;
        const char *raw = (k_rows[i].fi < 17) ? s_car_spec[k_rows[i].fi] : "";
        int idx;

        fe_draw_small_text(hx, y, k_rows[i].hdr, 0xFFBBBBBB, sx, sy);

        switch (k_rows[i].exp) {
        case 1: /* layout type: char - 'A' */
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_small_text(vx, y,
                         (idx >= 0 && idx < n_layout) ? k_stat_layout_types[idx] : raw,
                         0xFFFFFFFF, sx, sy);
            break;
        case 2: /* engine type: char - 'A' */
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_small_text(vx, y,
                         (idx >= 0 && idx < n_engine) ? k_stat_engine_types[idx] : raw,
                         0xFFFFFFFF, sx, sy);
            break;
        case 3: /* front/rear tire combined */
        {
            char f[24], r[24];
            frontend_fmt_spec(f, sizeof(f), raw);
            frontend_fmt_spec(r, sizeof(r), (k_rows[i].fi+1 < 17) ? s_car_spec[k_rows[i].fi+1] : "");
            snprintf(val, sizeof(val), "%s/%s", f, r);
            fe_draw_small_text(vx, y, val, 0xFFFFFFFF, sx, sy);
            break;
        }
        default:
            frontend_fmt_spec(val, sizeof(val), raw);
            if (k_rows[i].sfx && val[0] != '\0' && val[0] != '-') {
                size_t vl = strlen(val), sl = strlen(k_rows[i].sfx);
                if (vl + sl + 1 < sizeof(val))
                    memcpy(val + vl, k_rows[i].sfx, sl + 1);
            }
            fe_draw_small_text(vx, y, val, 0xFFFFFFFF, sx, sy);
            break;
        }
    }
}

/* Car-select ENTRY sidebar slide-in duration (bar/curve/topbar sweeping in +
 * the blue fill growing from the right). Halved from the original 2500ms so the
 * screen fades in 2x faster (S04 user request). Referenced by BOTH the state-2
 * advance gate (Screen_CarSelection) and the slide-in render below, which derive
 * the same t from s_anim_start_ms — keep them on this one constant so they stay
 * in sync. (frontend_update_timed_animation already applies a global 2x factor,
 * so the slide actually settles in ~half this value of wall-clock.) */

/* [ARCH-DIVERGENCE: DDraw QueueFrontendOverlayRect -> D3D11 fe_draw_surface_rect; L5 sweep 2026-05-21]
 *   Port reimplements DrawCarSelectionPreviewOverlay (0x0040DDC0) using D3D11
 *   batched quads instead of DDraw blit-queue. Same animation phases (state 0
 *   static / state 11=0xB slide-out / state 14=0xE slide-in), same coordinate
 *   constants (0x198 width, 0x118 height, 0x5A alpha, 0x40/0x20 step deltas,
 *   0x4A8 offscreen offset). DDraw color-key + per-frame surface tracking
 *   replaced by tex-page + alpha blending. */
/* Non-interactive "at a glance" stat panel: a button-framed box with three
 * relative bars (Speed / Accel / Grip), drawn between PAINT and MORE STATS on
 * every car-select screen. Raw config.nfo fields are passed in (SP reads them
 * from the shared s_car_spec cache, MP from its per-pane spec cache), so this
 * draws no files. `accent` fills the bars (player colour in MP); `compact`
 * shrinks the rows + scales the labels down so the full names still fit in the
 * small split panes. */
static void frontend_draw_car_stat_bars(float bx, float by, float bw, float bh,
                                        const char *f7, const char *f8, const char *f14,
                                        uint32_t accent, int compact, float sx, float sy) {
    static const char *lbl[3] = { "SPEED", "ACCEL", "GRIP" };
    float spd = 0, acc = 0, hnd = 0, fr[3] = { 0, 0, 0 };
    float padx = compact ? 4.0f : 7.0f;
    float pady = compact ? 2.0f : 6.0f;
    /* In the MP panes, line the labels up with the CAR/PAINT button text (drawn at
     * x+17) and stop the bars before the ◄► arrows on those buttons (their right
     * arrow's left edge is x+w-15; the value redge is x+w-18). SP keeps padx. */
    float content_l = compact ? 17.0f : padx;
    float content_r = compact ? 18.0f : padx;
    float top  = by + pady;
    float rowh = (bh - 2.0f * pady) / 3.0f;
    float barh = compact ? (rowh - 2.0f) : (rowh - 5.0f);
    float lsx = sx, lsy = sy, capd, lblw, barx, barw;
    int i;

    if (barh < 2.0f) barh = 2.0f;
    /* Shrink the label font to the row in compact (small split) panes so the
     * full names still fit; SP keeps the full small-font size. */
    if (compact) {
        float s = rowh / 11.0f;
        if (s > 1.0f) s = 1.0f;
        if (s < 0.42f) s = 0.42f;
        lsx = sx * s; lsy = sy * s;
    }
    capd = SMALLFONT_TTF_CAP * (lsy / sy);

    if (frontend_glance_from_fields(f7, f8, f14, &spd, &acc, &hnd))
        frontend_normalize_glance(spd, acc, hnd, &fr[0], &fr[1], &fr[2]);

    /* Frame: the regular blue/unselected button look (non-interactive). */
    fe_draw_button_frame(bx * sx, by * sy, bw * sx, bh * sy, 1, sx, sy);

    /* Full-name label column ("SPEED"/"ACCEL"/"GRIP"); dropped only if the box is
     * far too narrow to fit a label plus a usable bar. */
    lblw = fe_measure_small_text("SPEED") * fe_glyph_sx(lsx, lsy) / sx;
    if (bw < content_l + content_r + lblw + 8.0f) lblw = 0.0f;
    barx = bx + content_l + (lblw > 0.0f ? lblw + 4.0f : 0.0f);
    barw = (bx + bw - content_r) - barx;
    if (barw < 4.0f) barw = 4.0f;

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    for (i = 0; i < 3; i++) {
        float ry   = top + (float)i * rowh;
        float bary = ry + (rowh - barh) * 0.5f;
        float fillw = fr[i] * barw;
        if (lblw > 0.0f) {
            float ty = (ry + (rowh - capd) * 0.5f) * sy;
            fe_draw_small_text((bx + content_l) * sx, ty, lbl[i], 0xFFC8C8C8u, lsx, lsy);
        }
        fe_draw_quad(barx * sx, bary * sy, barw * sx, barh * sy, 0xFF101828u, -1, 0, 0, 1, 1);
        if (fillw > 0.0f)
            fe_draw_quad(barx * sx, bary * sy, fillw * sx, barh * sy, accent, -1, 0, 0, 1, 1);
    }
}

/* Fixed accent for the single-player / sequential car-select bars (the MP grid
 * passes each player's identity colour instead). */
#define FE_CARSTAT_ACCENT 0xFFE8C040u   /* amber — reads over the 0xFF00005C blue fill */

/* Single-player stat panel rect (640x480 design space), sitting in the gap the
 * re-laid-out button column (frontend_apply_color_panel_layout) leaves between
 * the PAINT button (ends y=237) and the MORE STATS button (starts y=297). */
#define FE_CARSTAT_PANEL_X 46.0f
#define FE_CARSTAT_PANEL_Y 241.0f
#define FE_CARSTAT_PANEL_W 168.0f
#define FE_CARSTAT_PANEL_H 52.0f

static void frontend_render_car_selection_preview(float sx, float sy) {
    /* Simultaneous multiplayer: render the per-player grid (setup window in phase
     * 0, car-select grid in phase 1) instead of the single preview + buttons. */
    if (s_mp_simul) {
        if (s_mp_phase == 0) { extern void frontend_mp_setup_profile_render(float sx, float sy);  /* [#11] profile save/load panel overlay */
                               frontend_mp_setup_render(sx, sy); frontend_mp_setup_profile_render(sx, sy); }
        else                 frontend_mp_simul_carsel_render(sx, sy);
        return;
    }
    int actual_car = frontend_current_car_index();
    /* Keep the PAINT button (slot 1) greyed/disabled whenever the current car
     * has no paint choice (TD5 special/police cars; TD6 cop cars). Idempotent
     * UI sync — runs every frame for this screen, so it tracks car changes with
     * no lag, and a disabled button is skipped by nav/hover/click + rendered
     * grey (see the button draw's bb_state==2 path). Paintable cars keep the
     * working ◄► arrows (TD5) / modal colour picker (TD6). */
    if (s_button_count > 1 && s_buttons[1].active)
        s_buttons[1].disabled = !frontend_car_has_paint(actual_car);
    float sw = sx * 640.0f;
    float sh = sy * 480.0f;

    /* [PORT ENHANCEMENT 2026-06] Multiplayer flow: show whose turn it is to pick. */
    if (s_mp_flow && s_anim_complete) {
        char who[32];
        snprintf(who, sizeof who, "PLAYER %d - SELECT CAR", s_mp_car_player + 1);
        fe_draw_text_centered(320.0f * sx, 30.0f * sy, who, 0xFFFFD000, sx, sy);
    }

    /* Layer 1: Solid dark blue fill — drawn FIRST so overlays render on top.
     * Original DDraw renderer preserves pixels between frames; our clear-per-frame
     * D3D11 renderer needs an explicit fill to produce the same visual.
     * BGRA 0xFF00005C = RGB(0, 0, 92) — matches CarSelBar1 dominant pixel.
     *
     * During state 2 (slide-in): fill tracks the bar position, growing from right.
     * After slide-in: fill covers the full content area (x=60..640, y=0..464). */
    {
        uint32_t fill_color = 0xFF00005C; /* BGRA: B=0x5C(92), G=0, R=0, A=0xFF */
        if (s_inner_state == 2) {
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / (float)FE_CARSEL_SLIDE_IN_MS);
            float bar_x = 636.0f - (636.0f - 36.0f) * t;
            float fill_x = bar_x + 24.0f;
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_quad(fill_x * sx, 0.0f * sy, (640.0f - fill_x) * sx, 408.0f * sy,
                         fill_color, s_white_tex_page, 0, 0, 1, 1);
        } else {
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            fe_draw_quad(60.0f * sx, 0.0f * sy, 580.0f * sx, 408.0f * sy,
                         fill_color, s_white_tex_page, 0, 0, 1, 1);
        }
    }

    /* Layer 2: Overlay UI elements on top of the fill.
     * State 2: bar+curve slide from right (636→36); topbar slides from left (-532→0).
     * CarSelBar1: fully opaque solid bar → fe_draw_surface_opaque.
     * CarSelCurve + CarSelTopBar: color-keyed black (alpha=0) → fe_draw_surface_rect
     * so transparent pixels show whatever is behind (fill or background during anim). */
    if (s_inner_state == 2) {
        float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / (float)FE_CARSEL_SLIDE_IN_MS);
        float bar_x    = 636.0f - (636.0f - 36.0f) * t;
        float topbar_x = -532.0f + 532.0f * t;
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,   bar_x * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_rect(s_carsel_curve_surface, bar_x * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
        /* Bottom strip: flat fill from right of curve to screen edge */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad((bar_x + 80.0f) * sx, 408.0f * sy, (640.0f - bar_x - 80.0f) * sx, 56.0f * sy,
                     0xFF00005C, s_white_tex_page, 0, 0, 1, 1);
        /* Topbar drawn LAST so it overlays the bar/curve (z-order parity with original). */
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_rect(s_carsel_topbar_surface, topbar_x * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
    } else {
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,    36.0f * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_rect(s_carsel_curve_surface,  36.0f * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
        /* Bottom strip: flat fill from right of curve (x=116) to screen edge */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(116.0f * sx, 408.0f * sy, 524.0f * sx, 56.0f * sy,
                     0xFF00005C, s_white_tex_page, 0, 0, 1, 1);
        /* Topbar drawn LAST (front of bar/curve) — z-order parity with original. */
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_rect(s_carsel_topbar_surface,  0.0f * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
    }

    /* At-a-glance stat bars: a non-interactive panel in the gap between the PAINT
     * and MORE STATS buttons. Drawn in BOTH the normal car preview AND the MORE
     * STATS sub-screen (state 15) — the button column stays visible there, so the
     * bars stay too. Hidden only while the TD6 colour picker is open (it fills
     * that band). Slides in with the button column (mirrors button 2, directly
     * below it). Drawn after the bar/curve/topbar layer so it sits on top, like
     * the buttons. */
    if (s_button_count > 0 && !s_color_panel_visible) {
        float panel_x = frontend_get_button_anim_x(2, FE_CARSTAT_PANEL_X);
        frontend_load_car_spec_fields(actual_car);
        frontend_draw_car_stat_bars(panel_x, FE_CARSTAT_PANEL_Y,
                                    FE_CARSTAT_PANEL_W, FE_CARSTAT_PANEL_H,
                                    s_car_spec[7], s_car_spec[8], s_car_spec[14],
                                    FE_CARSTAT_ACCENT, 0, sx, sy);
    }

    if (s_inner_state == 15) {
        /* Stats sub-screen [CONFIRMED @0x0040dfc0 state 0xF]: background is OPAQUE dark
         * blue (the layer-1 0xFF00005C fill above) — the scene does NOT show through. The
         * car is drawn DIMMED in place by a per-channel HALVING (>>1), NOT the preview
         * screen's 0x5A alpha: the original calls RenderTgaWithColorKeyToSurface(...,0xff)
         * which halves each RGB channel of the car twice. Approximate that here with a
         * multiplicative tint of ~0x40 (≈ one halving toward black) at full opacity, so the
         * car reads faintly under the spec text over the opaque blue (matching the original's
         * "dimmed car on opaque blue", not a semi-transparent see-through panel). */
        if (s_car_preview_surface > 0)
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFF404040);
        frontend_render_car_stats_overlay(sx, sy);
    } else {
        /* Show the paint colour live whenever the panel is open OR a colour has
         * been confirmed (s_paint_active) — but ONLY for a car that is actually
         * paintable (the CURRENTLY selected one). s_paint_active is deliberately
         * NOT cleared on a car change (the chosen colour is remembered for all TD6
         * cars), so WITHOUT the paintable(actual_car) gate the slide-OUT (state 11)
         * and its lead-in frame (state 10) would paint the OUTGOING TD6 body even
         * while the INCOMING selection is a TD5 car — the reported "fade-out loads
         * the latest painted TD6 body when selecting a TD5 car" bug. (TD6 cars DO
         * ship a carpic, so prev_surface>0 and the body would otherwise slide out
         * fully painted.) Gating on the current car means the instant the selection
         * becomes a non-paintable TD5 car the overlay stops drawing: the old car
         * slides out as its plain (grey) carpic and no TD6 paint bleeds into the
         * TD5 transition. TD6->TD6 switches still slide out painted. */
        int show_paint = (s_color_panel_visible || s_paint_active) &&
                         frontend_car_paintable(actual_car);
        if (s_inner_state == 11) {
            /* Old car slides out to the right (state 11, ~433ms) — animPhase 0x0B: offset = counter*0x20.
             * On the very first frame(s) of state 11 the case-11 update that loads
             * prev_surface may not have run yet (prev==0); in that case keep the OLD
             * car at centre (s_car_preview_surface is still the old carpic — it is
             * only swapped to the new car when the slide-out completes). State 11 is
             * handled ENTIRELY here so it can never fall through to the static branch
             * below, whose helper would reload the overlay to the NEW car mid-slide. */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 433.0f);
            float x = (s_car_preview_prev_surface > 0) ? (232.0f + 408.0f * t) : 232.0f;
            int slide_surf = (s_car_preview_prev_surface > 0) ? s_car_preview_prev_surface : s_car_preview_surface;
            if (slide_surf > 0)
            fe_draw_surface_rect(slide_surf, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
            /* Keep the OLD car painted while it slides out — reached only for a
             * TD6->TD6 switch (show_paint above already requires the CURRENT car
             * be paintable, so a TD6->TD5 switch never paints the outgoing body).
             * The overlay surface is still cached for the old car (s_paint_overlay
             * _car) — it isn't reloaded until the slide-IN draws the new car — so
             * draw it DIRECTLY (not via the lazy helper, which would swap in the
             * new car's mask).
             *
             * 🟥 Paint the overlay over the SAME surface/position as the base
             * (slide_surf / x), NOT gated on prev_surface>0. On the first frame(s)
             * of state 11 the case-11 update hasn't set prev yet (prev==0), so the
             * base is drawn UNPAINTED at centre (slide_surf = s_car_preview_surface,
             * still the old carpic); gating the overlay on prev>0 dropped the paint
             * for those frames — a one-frame flash of the GRAY unpainted chassis at
             * the start of every car change ("it's still loading the no-paint
             * chassis"). slide_surf and s_paint_overlay_car are both the OLD car in
             * either case, so drawing the overlay at the same x keeps it painted
             * throughout. NO RE BASIS — port-only TD6 paint preview. */
            if (show_paint && slide_surf > 0 &&
                s_paint_overlay_surface > 0 &&
                frontend_car_paintable(s_paint_overlay_car))
                fe_draw_surface_rect(s_paint_overlay_surface, x * sx, 124.0f * sy,
                                     408.0f * sx, 280.0f * sy,
                                     frontend_rgb_to_bgra((uint32_t)g_td5.ini.td6_paint_color));
        } else if (s_inner_state == 14 && s_car_preview_surface > 0) {
            /* New car slides in from right (state 14, ~833ms @30fps):
             * formula @ 0x0040DF4A: x = canvasW + counter*(-0x40) + 0x4A8
             *   = 640 + 1192 = 1832 at counter=0, arrives at 232 in 25 frames (1600px travel).
             * Time-based equivalent: 25 frames / 30fps = ~833ms, using 800ms for match. */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 800.0f);
            float x = 1832.0f - 1600.0f * t;  /* 1832 → 232 [CONFIRMED @ 0x0040DF4A] */
            fe_draw_surface_rect(s_car_preview_surface, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
            if (show_paint) frontend_draw_car_paint_overlay(actual_car, x, sx, sy);
        } else if (s_inner_state >= 6 && s_inner_state != 12 && s_inner_state != 13 && s_car_preview_surface > 0) {
            /* Static car display. ALWAYS white: the carpic already carries the
             * grayscale body + the fixed parts (glass/lights/chrome) in their own
             * colours. The body-only paint overlay (below) tints just the body. */
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
            if (show_paint) {
                if (s_inner_state == 10) {
                    /* State 10 is the SINGLE transient frame between a car CHANGE
                     * and the slide-out: s_car_preview_surface is still the OLD
                     * carpic and actual_car is already the NEW car. Draw the OLD
                     * car's CACHED overlay DIRECTLY here — calling the helper would
                     * reload the overlay for the new car, so its colour/mask would
                     * leak onto this first frame and the whole slide-OUT ("first
                     * animation"). The new car's overlay is loaded on the slide-IN
                     * (state 14, the "second animation"). */
                    if (s_paint_overlay_surface > 0 && frontend_car_paintable(s_paint_overlay_car))
                        fe_draw_surface_rect(s_paint_overlay_surface, 232.0f * sx, 124.0f * sy,
                                             408.0f * sx, 280.0f * sy,
                                             frontend_rgb_to_bgra((uint32_t)g_td5.ini.td6_paint_color));
                } else {
                    frontend_draw_car_paint_overlay(actual_car, 232.0f, sx, sy);
                }
            }
        }
    }

    /* FALLBACK only: a TD6 car whose carpic preview hasn't been generated yet
     * has no surface, so fill the preview area with the flat selected paint
     * color as live feedback. Once a carpic exists it is drawn above (gray base
     * + body-only paint overlay), so this flat box must NOT cover it — gated on
     * s_car_preview_surface <= 0. Skipped on the dimmed stats sub-screen (15). */
    if (frontend_car_paintable(actual_car) && s_car_preview_surface <= 0 &&
        s_inner_state >= 6 && s_inner_state != 15) {
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy,
                     frontend_rgb_to_bgra((uint32_t)g_td5.ini.td6_paint_color),
                     s_white_tex_page, 0, 0, 1, 1);
    }

    if (s_anim_complete) {
        /* Car name: y = canvasH/2 - 0x97 = 480/2 - 151 = 89 (gap between topbar bottom y=81 and car preview y=124) */
        frontend_draw_value_text(sx, sy, 232, 89, frontend_get_car_display_name(actual_car), 0xFFFFFFFF);
        int is_locked = (!s_cheat_unlock_all && !s_network_active &&
                         actual_car >= 0 && actual_car < 37 &&
                         s_car_lock_table[actual_car] != 0);
        if (is_locked) {
            /* [FIXED 2026-06-01] SNK_LockedTxt white (orig), at (86,163)=(cx-0xea, cy-0x77),
             * not red at y=121. */
            frontend_draw_value_text(sx, sy, 86, 163, "LOCKED", 0xFFFFFFFF);
        } else if (s_selected_game_type == 2) {
            /* [FIXED 2026-06-01] Beast/Beauty tag for the Era cup (gametype 2), same
             * (86,163) slot as Locked (mutually exclusive). [CONFIRMED @ 0x40DFC0 case0xc:
             * car<8 → Beauty (SNK_BeautyTxt), else Beast (SNK_BeastTxt).] */
            frontend_draw_value_text(sx, sy, 86, 163,
                                     (actual_car < 8) ? "BEAUTY" : "BEAST", 0xFFFFFFFF);
        }
    }
}

/* Read a whole file into a malloc'd, NUL-terminated buffer (for cJSON_Parse).
 * Returns NULL on any error; caller frees. */
static char *frontend_slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t rd;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* Fetch a numeric field as double, 0.0 if absent/non-numeric. */
static double frontend_json_num(const cJSON *obj, const char *key) {
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? cJSON_GetNumberValue(n) : 0.0;
}

/* Parse a trak_markers JSON file into dst[], placing each entry at
 * (<index_key> - index_base). Returns the number of markers placed (>=0), or
 * -1 if the file is absent / unparseable (caller treats that as "unavailable").
 * The editable replacement for the retired TMK1 trak_markers*.dat. */
static int frontend_parse_track_markers_json(const char *path,
                                             const char *index_key,
                                             int index_base,
                                             TD5_TrackMarker *dst,
                                             int dst_cap) {
    char *buf = frontend_slurp_file(path);
    cJSON *root;
    const cJSON *arr, *el;
    int placed = 0;
    if (!buf) return -1;
    root = cJSON_Parse(buf);
    free(buf);
    if (!root) return -1;
    arr = cJSON_GetObjectItemCaseSensitive(root, "markers");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(el, arr) {
            const cJSON *idxn, *ci;
            double dv;
            int slot;
            if (!cJSON_IsObject(el)) continue;
            idxn = cJSON_GetObjectItemCaseSensitive(el, index_key);
            if (!cJSON_IsNumber(idxn)) continue;
            dv = cJSON_GetNumberValue(idxn);
            slot = (int)(dv < 0 ? dv - 0.5 : dv + 0.5) - index_base;
            if (slot < 0 || slot >= dst_cap) continue;
            ci = cJSON_GetObjectItemCaseSensitive(el, "circuit");
            dst[slot].start_u = (float)frontend_json_num(el, "start_u");
            dst[slot].start_v = (float)frontend_json_num(el, "start_v");
            dst[slot].end_u   = (float)frontend_json_num(el, "end_u");
            dst[slot].end_v   = (float)frontend_json_num(el, "end_v");
            dst[slot].circuit = (uint8_t)(cJSON_IsTrue(ci) ||
                (cJSON_IsNumber(ci) && cJSON_GetNumberValue(ci) != 0.0));
            placed++;
        }
    }
    cJSON_Delete(root);
    return placed;
}

/* Load the start/finish dot table once. File is optional: if absent (e.g. the
 * tool hasn't been run), dots are silently skipped and the previews render as
 * before. */
static void frontend_load_track_markers(void) {
    int n;
    if (s_track_markers_loaded != 0) return;
    s_track_markers_loaded = -1;
    n = frontend_parse_track_markers_json("re/assets/tracks/trak_markers.json",
                                          "pool", 0, s_track_markers, 20);
    if (n < 0) {
        TD5_LOG_W(LOG_TAG, "trak_markers.json not found; track start/finish dots disabled");
        return;
    }
    s_track_markers_loaded = 1;
    TD5_LOG_I(LOG_TAG, "loaded %d track start/finish markers", n);
}

/* Same as above for migrated TD6 tracks (trak_markers_td6.json). Each entry's
 * "tga" field maps to preview TGA number (TD6_PREVIEW_TGA_BASE + slot). Optional:
 * absent file just means no TD6 start dots, identical to the TD5-only behaviour. */
static void frontend_load_track_markers_td6(void) {
    int n;
    if (s_track_markers_td6_loaded != 0) return;
    s_track_markers_td6_loaded = -1;
    n = frontend_parse_track_markers_json("re/assets/tracks/trak_markers_td6.json",
                                          "tga", TD6_PREVIEW_TGA_BASE,
                                          s_track_markers_td6, TD6_MARKER_MAX);
    if (n < 0) return;
    s_track_markers_td6_loaded = 1;
    TD5_LOG_I(LOG_TAG, "loaded %d TD6 track start/finish markers", n);
}

/* Draw one preview marker dot centered at (cx,cy) screen px. kind 0 = START
 * (green), kind 1 = FINISH (black/white 2x2 checker). A black outline keeps it
 * legible over the red track line. */
static void frontend_draw_marker_dot(float cx, float cy, float sx, float sy, int kind) {
    float scale = (sx + sy) * 0.5f;
    float r = 4.0f * scale;
    float h;
    if (r < 3.0f) r = 3.0f;
    /* outline */
    fe_draw_quad(cx - r - 1.0f, cy - r - 1.0f, 2.0f * r + 2.0f, 2.0f * r + 2.0f,
                 0xFF000000, -1, 0, 0, 1, 1);
    if (kind == 0) {
        fe_draw_quad(cx - r, cy - r, 2.0f * r, 2.0f * r, 0xFF00FF00, -1, 0, 0, 1, 1);
    } else {
        h = r;
        fe_draw_quad(cx - r, cy - r, h, h, 0xFFFFFFFF, -1, 0, 0, 1, 1);
        fe_draw_quad(cx,     cy - r, h, h, 0xFF000000, -1, 0, 0, 1, 1);
        fe_draw_quad(cx - r, cy,     h, h, 0xFF000000, -1, 0, 0, 1, 1);
        fe_draw_quad(cx,     cy,     h, h, 0xFFFFFFFF, -1, 0, 0, 1, 1);
    }
}

static void frontend_render_track_selection_preview(float sx, float sy) {
    char track_name[80], city[80], country[80];
    const char *comma;
    /* Animation offsets for track-switch slide-in (state 9).
     * Original: state 8 @ 0x427630 uses single counter _DAT_0049522c (0..0x10=16 frames).
     * Preview slides in from right: x_offset = (16 - tick) * 16px [CONFIRMED @ 0x427b80-ish].
     * Text slides in from above:  y_offset = (tick - 16) * 16px [CONFIRMED @ 0x427957 formula]. */
    float img_x_off = (s_track_switch_tick < 16) ? (16 - s_track_switch_tick) * 16.0f * sx : 0.0f;
    float txt_y_off = (s_track_switch_tick < 16) ? (s_track_switch_tick - 16) * 16.0f * sy : 0.0f;

    if (!s_anim_complete) return;

    /* [PORT ENHANCEMENT 2026-06] race-option row values (AI/laps/traffic/police),
     * drawn in the gap between the 224-wide option buttons (end x=344) and the
     * track preview (x=412). */
    {
        const char *on_off[2] = { "OFF", "ON" };
        /* [dynamic-traffic] 5-state traffic volume row (merge: ours) + per-race
         * difficulty row (merge: master). */
        const char *traffic_vol[TD5_TRAFFIC_VOLUME_COUNT] = { "OFF", "LOW", "MEDIUM", "HIGH", "VERY HIGH" };
        const char *difficulty[3] = { "EASY", "NORMAL", "HARD" };
        char vb[8];
        float vx = 350.0f * sx;
        if (s_buttons[2].active) { snprintf(vb, sizeof vb, "%d", s_num_ai_opponents);
            fe_draw_text(vx, (float)(s_buttons[2].y + 6) * sy, vb, 0xFFFFFFFF, sx*0.8f, sy*0.8f); }
        if (s_buttons[3].active && !s_buttons[3].hidden) { snprintf(vb, sizeof vb, "%d", s_game_option_laps + 1);
            fe_draw_text(vx, (float)(s_buttons[3].y + 6) * sy, vb, 0xFFFFFFFF, sx*0.8f, sy*0.8f); }
        if (s_buttons[4].active) {
            int tvi = s_game_option_traffic;
            if (tvi < 0) tvi = 0;
            if (tvi > TD5_TRAFFIC_VOLUME_COUNT - 1) tvi = TD5_TRAFFIC_VOLUME_COUNT - 1;
            fe_draw_text(vx, (float)(s_buttons[4].y + 6) * sy, traffic_vol[tvi], 0xFFFFFFFF, sx*0.8f, sy*0.8f);
        }
        if (s_buttons[5].active)
            fe_draw_text(vx, (float)(s_buttons[5].y + 6) * sy, on_off[s_game_option_cops & 1], 0xFFFFFFFF, sx*0.8f, sy*0.8f);
        if (s_buttons[6].active && !s_buttons[6].hidden)  /* per-race AI difficulty (hidden in Quick Race) */
            fe_draw_text(vx, (float)(s_buttons[6].y + 6) * sy, difficulty[s_race_difficulty % 3], 0xFFFFFFFF, sx*0.8f, sy*0.8f);
    }

    frontend_get_track_display_name(s_selected_track, 0, track_name, sizeof(track_name));
    /* Original: text surface 296x184 at (344,81), city at surf y=0, country at surf y=0x20=32
     * RE: 0x00427CD0-0x00427D56 — split at 0x2C; blit top 64 rows to screen (344,81) */
    comma = strchr(track_name, ',');
    if (comma) {
        int city_len = (int)(comma - track_name);
        if (city_len >= (int)sizeof(city)) city_len = (int)sizeof(city) - 1;
        memcpy(city, track_name, city_len);
        city[city_len] = '\0';
        /* skip comma + space */
        strncpy(country, comma + 2, sizeof(country) - 1);
        country[sizeof(country) - 1] = '\0';
        /* Original centers each line within 296px surface blitted at x=344
         * RE: FUN_00424a50 returns (0x128 - text_width) / 2; center = 344 + 148 = 492 */
        fe_draw_text_centered(492.0f * sx, 81.0f * sy + txt_y_off, city, 0xFFFFFFFF, sx, sy);
        fe_draw_text_centered(492.0f * sx, 113.0f * sy + txt_y_off, country, 0xFFFFFFFF, sx, sy);
    } else {
        fe_draw_text_centered(492.0f * sx, 81.0f * sy + txt_y_off, track_name, 0xFFFFFFFF, sx, sy);
    }
    /* Track preview: 152x224 portrait, right of buttons.
     * x=EDI+0x12E=412, y=ESI+0x36=135 (640x480).
     * Slides in from right during track-switch animation (state 9). */
    if (s_track_preview_surface > 0) {
        fe_draw_surface_rect(s_track_preview_surface,
                             412.0f * sx + img_x_off, 135.0f * sy,
                             152.0f * sx, 224.0f * sy, 0xFFFFFFFF);

        /* Overlay start/finish dots on the preview. P2P: green START + checkered
         * FINISH, swapped by the Forwards/Backwards toggle (s_track_direction).
         * Circuit: a single START/FINISH dot (race starts there, no separate
         * end). Markers line up because they were generated with the same
         * projection as the PNG. */
        frontend_load_track_markers();
        {
            /* pool == trak TGA file number. TD5 tracks: 0..19 (s_track_markers).
             * Migrated TD6 tracks: >= TD6_PREVIEW_TGA_BASE (s_track_markers_td6). */
            int pool = (s_selected_track >= 0 &&
                        s_selected_track < (int)(sizeof(s_track_schedule_to_tga_index) /
                                                 sizeof(s_track_schedule_to_tga_index[0])))
                       ? s_track_schedule_to_tga_index[s_selected_track] : -1;
            const TD5_TrackMarker *m = NULL;
            if (pool >= 0 && pool < 20) {
                if (s_track_markers_loaded == 1) m = &s_track_markers[pool];
            } else if (pool >= TD6_PREVIEW_TGA_BASE &&
                       pool < TD6_PREVIEW_TGA_BASE + TD6_MARKER_MAX) {
                frontend_load_track_markers_td6();
                if (s_track_markers_td6_loaded == 1)
                    m = &s_track_markers_td6[pool - TD6_PREVIEW_TGA_BASE];
            }
            if (m) {
                float bx = 412.0f * sx + img_x_off, by = 135.0f * sy;
                float pw = 152.0f * sx, ph = 224.0f * sy;
                /* Forward shows the geometry start; Backwards swaps the roles so
                 * the dots reflect the actual race start/finish on a reverse run. */
                int bwd = (s_track_direction != 0);
                float su = bwd ? m->end_u : m->start_u;
                float sv = bwd ? m->end_v : m->start_v;
                float eu = bwd ? m->start_u : m->end_u;
                float ev = bwd ? m->start_v : m->end_v;
                td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
                if (m->circuit) {
                    /* one start/finish dot; direction is forward-only for circuits */
                    frontend_draw_marker_dot(bx + m->start_u * pw, by + m->start_v * ph,
                                             sx, sy, 0);
                } else {
                    frontend_draw_marker_dot(bx + eu * pw, by + ev * ph, sx, sy, 1);
                    frontend_draw_marker_dot(bx + su * pw, by + sv * ph, sx, sy, 0);
                }
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            }
        }
    }
    /* [FIXED 2026-06-01] Arrows only on the Track selector (slot 0). Orig
     * (0x427630) arms InitializeFrontendDisplayModeArrows(0,1) for the Track
     * cycler; the Forwards/Backwards row (slot 1) is a PRESS toggle, not a ◄►
     * cycler, so it must NOT show selector arrows. */
    fe_draw_option_arrows(0, sx, sy);
    if (!s_cheat_unlock_all &&
        s_selected_track >= 0 &&
        s_selected_track < 31 &&
        s_track_lock_table[s_selected_track] != 0 &&
        !s_network_active) {
        /* [FIXED 2026-06-01] SNK_LockedTxt renders white (orig), not red. */
        frontend_draw_value_text(sx, sy, 412, 375, "LOCKED", 0xFFFFFFFF);
    }
}

static void frontend_render_control_options_overlay(float sx, float sy) {
    /* [PORT ENHANCEMENT 2026-06] Single-player-selector layout:
     *   row 0 (y=97)  PLAYER value (the selected 1-based player)
     *   row 1 (y=177) CONTROLLER SELECTION: device-type icon + device name. */
    char buf[40];
    int player, type, source;

    if (!s_anim_complete) return;

    player = s_ctrl_opts_player;
    if (player < 0) player = 0;
    if (player >= TD5_MAX_HUMAN_PLAYERS) player = TD5_MAX_HUMAN_PLAYERS - 1;

    /* PLAYER value (row 0). */
    snprintf(buf, sizeof buf, "%d", player + 1);
    frontend_draw_value_centered(sx, sy, 97 + 6, buf, 0xFFFFFFFF);

    /* CONTROLLER (row 1, y=177): device-type icon + the FULL device name.
     * td5_input_get_device_type: 0=keyboard, 1=joypad, 2=joystick/wheel — used
     * directly as the Controllers.tga 64x32 icon row. */
    type   = td5_input_get_device_type(player);
    source = td5_input_get_input_source(player);

    if (s_control_options_surface > 0) {
        int slot = s_control_options_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use &&
            s_surfaces[slot].height > 0) {
            float v_row = 32.0f / (float)s_surfaces[slot].height;
            float v0 = (float)type * v_row, v1 = v0 + v_row;
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(394.0f * sx, 177.0f * sy, 64.0f * sx, 32.0f * sy,
                         0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, v0, 1.0f, v1);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        }
    }
    {
        /* Full enumerated device name (e.g. "Controller (8BitDo Ultimate 2C
         * Wireless Controller) #1") to the right of the icon. Word-wrapped over
         * as many lines as needed so the whole name is shown without clipping;
         * the block is vertically centred on the 32px icon row. */
        const char *dname = td5_input_get_device_name(source);
        float ts   = 0.85f;
        float nx   = 466.0f * sx;
        float maxw = 168.0f * sx;          /* column from x~466 to ~634 */
        char lines[6][64];
        int nl, li;
        float step = 14.0f, first_y;
        if (!dname || !dname[0]) dname = "<NONE>";
        nl = fe_wrap_text_lines(dname, maxw, sx * ts, sy * ts, lines, 6);
        first_y = 185.0f - (float)(nl - 1) * step * 0.5f;   /* centre on icon (y 177..209) */
        for (li = 0; li < nl; li++)
            fe_draw_text(nx, (first_y + (float)li * step) * sy, lines[li],
                         0xFFFFFFFF, sx * ts, sy * ts);
    }
}

/* Controller binding overlay: show the active controller-type icon (64x32)
 * centered horizontally at y=120, plus "No Controller" warning when applicable.
 * Original: FUN_0040FE00 draws the detected device icon from individual TGAs.
 * The Control Options screen (14) uses Controllers.TGA sprite sheet instead;
 * the binding screen (18) uses per-type icon TGAs. */
/* Short human-readable name for a DirectInput scancode (common driving keys;
 * falls back to hex). [PORT ENHANCEMENT 2026-06] */
static const char *ctrl_scancode_name(unsigned sc)
{
    static char hexbuf[8];
    switch (sc) {
        case 0x00: return "-";
        case 0x01: return "ESC";    case 0x0E: return "BKSP";  case 0x0F: return "TAB";
        case 0x1C: return "ENTER";  case 0x1D: return "LCTRL"; case 0x38: return "ALT";
        case 0x2A: return "LSHIFT"; case 0x36: return "RSHIFT";case 0x39: return "SPACE";
        case 0xC8: return "UP";     case 0xD0: return "DOWN";
        case 0xCB: return "LEFT";   case 0xCD: return "RIGHT";
        case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
        case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
        case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
        case 0x0B: return "0";
        case 0x10: return "Q"; case 0x11: return "W"; case 0x12: return "E";
        case 0x13: return "R"; case 0x14: return "T"; case 0x15: return "Y";
        case 0x16: return "U"; case 0x17: return "I"; case 0x18: return "O";
        case 0x19: return "P";
        case 0x1E: return "A"; case 0x1F: return "S"; case 0x20: return "D";
        case 0x21: return "F"; case 0x22: return "G"; case 0x23: return "H";
        case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
        case 0x2C: return "Z"; case 0x2D: return "X"; case 0x2E: return "C";
        case 0x2F: return "V"; case 0x30: return "B"; case 0x31: return "N";
        case 0x32: return "M";
        default: break;
    }
    snprintf(hexbuf, sizeof hexbuf, "0x%02X", sc & 0xFF);
    return hexbuf;
}

/* [PORT ENHANCEMENT 2026-06] Per-button remap overlay. The action rows are real
 * buttons (drawn by the button renderer); this draws the current binding in a
 * value column beside each row, the capture prompt on the row being remapped,
 * and a header + hint line. */
static void frontend_render_controller_binding_overlay(float sx, float sy) {
    char hdr[48];
    const char *hint;
    if (!s_anim_complete) return;
    if (s_inner_state != 10) return;   /* only the interactive list state */

    /* The "OPTIONS" Lunatica header now occupies the very top (y~17), so this
     * sub-header sits lower — at the old REMAP ALL height (y=90). */
    snprintf(hdr, sizeof hdr, "CONTROLLER SETUP - PLAYER %d", s_ctrl_player + 1);
    fe_draw_text_centered(320.0f * sx, 90.0f * sy, hdr, 0xFFCCCCCC, sx * 0.75f, sy * 0.75f);
    /* While actively (re)binding, show the press/release prompt at the bottom.
     * The idle "SELECT AN ACTION TO REMAP..." hint line was removed per request. */
    if (s_ctrl_capturing) {
        hint = s_ctrl_capture_armed
            ? "PRESS A KEY / BUTTON / AXIS   (ESC = CANCEL)"
            : "RELEASE TO CONTINUE...   (ESC = CANCEL)";
        fe_draw_text_centered(320.0f * sx, 400.0f * sy, hint, 0xFF999999, sx * 0.58f, sy * 0.58f);
    }
    /* The per-action labels/values are drawn post-button in
     * frontend_render_controller_binding_labels (so they sit on top of the
     * selected button's opaque fill). */
}

/* [PORT ENHANCEMENT 2026-06] Per-action labels + binding values drawn INSIDE the
 * narrow two-column buttons. Called AFTER the button pass (like the option
 * arrows) so it renders on top of the selected button's opaque fill. */
static void frontend_render_controller_binding_labels(float sx, float sy) {
    int kbd = (s_ctrl_input_source == 0);
    int j;
    if (!s_anim_complete || s_inner_state != 10) return;

    /* The 11 action buttons (0..9 driving + 10 = PAUSE "?"): centered action label
     * IN the button, mapped binding value to its RIGHT. */
    for (j = 0; j < TD5_JSBIND_ACTIONS && j < s_button_count; j++) {
        const char *lab, *val;
        char vb[16];
        float bcx, vx, by, ts = 0.58f;
        uint32_t col;
        if (!s_buttons[j].active) continue;
        bcx = ((float)s_buttons[j].x + (float)s_buttons[j].w * 0.5f) * sx;
        vx  = ((float)s_buttons[j].x + (float)s_buttons[j].w + 6.0f) * sx;
        by  = ((float)s_buttons[j].y + ((float)s_buttons[j].h - (float)FONT_CELL * ts) * 0.5f) * sy;
        lab = (j < 10) ? k_ctrl_action_labels[j] : "PAUSE MENU";   /* action 10 = pause */
        col = 0xFFFFFFFFu;
        if (s_ctrl_capturing && j == s_ctrl_sel_action) {
            val = s_ctrl_capture_armed ? "PRESS" : "...";
            col = 0xFF33FF33u;
        }
        else if (kbd) val = ctrl_scancode_name(s_ctrl_kb_scancodes[j]);
        else { td5_plat_input_describe_binding(s_ctrl_action_bind[s_ctrl_player][j],
                                               vb, (int)sizeof vb); val = vb; }
        fe_draw_text_centered(bcx, by, lab, 0xFFFFFFFFu, sx*ts, sy*ts);
        fe_draw_text(vx, by, val, col, sx*ts, sy*ts);
    }

    /* Command buttons REMAP ALL (11) + OK (12): centered label only, drawn at a
     * slightly smaller font so the long "REMAP ALL" fits its button. */
    {
        int idx;
        const char *labs[2] = { "REMAP ALL", "OK" };
        float ts2 = 0.78f;
        for (idx = 11; idx <= 12; idx++) {
            float bcx, by;
            if (idx >= s_button_count || !s_buttons[idx].active) continue;
            bcx = ((float)s_buttons[idx].x + (float)s_buttons[idx].w * 0.5f) * sx;
            by  = ((float)s_buttons[idx].y + ((float)s_buttons[idx].h - (float)FONT_CELL * ts2) * 0.5f) * sy;
            fe_draw_text_centered(bcx, by, labs[idx - 11], 0xFFFFFFFFu, sx*ts2, sy*ts2);
        }
    }
}

/* [PORT ENHANCEMENT 2026-06] MULTIPLAYER lobby overlay: header + the list of
 * joined players (in join order) with their device + READY, drawn each frame. */
/* [#8] TD5RE_LOBBY_BUTTONS gate. Default ON; exactly "0" restores the original
 * stacked START/BACK layout (200x32 @ y300 / 120x32 @ y360, set by
 * Screen_MultiplayerLobby in td5_fe_net.c) together with the join/start/back
 * help line. When ON, the MP-lobby START + BACK buttons are re-laid SIDE BY
 * SIDE at equal width, lowered a little, and the help legend is dropped. */
static int frontend_lobby_buttons_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_LOBBY_BUTTONS");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP lobby button re-layout (#8) %s (TD5RE_LOBBY_BUTTONS=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

static void frontend_render_mp_lobby_overlay(float sx, float sy) {
    int p;
    int relayout = frontend_lobby_buttons_on();
    /* [R3-1 2026-06-19] DO NOT early-return on !s_anim_complete. The MP lobby
     * (screen 30) is NOT in frontend_screen_has_button_anim()/_get_button_anim_state,
     * so its START/BACK buttons do NOT slide in — they render at their STATIC base
     * rects from frame 1. But this overlay (which re-lays those two buttons SIDE BY
     * SIDE and draws the roster/header) used to bail out for the whole ~650ms entry
     * "animation" (Screen_MultiplayerLobby states 1..3). Result: the generic button
     * loop drew START+BACK at their CREATION positions (big START @220,300, small
     * BACK @260,360, stacked) with no header, then — the instant s_anim_complete
     * flipped — the buttons JUMPED to the side-by-side layout and the header/roster
     * popped in. That snap read as "a lone START screen, then the real START+BACK
     * screen replaces it" (the user's flash report). Rendering the full overlay
     * (re-layout + header + roster) every frame makes the screen complete from the
     * first frame, so there is no two-stage load. (The dead 650ms timer still runs
     * before the interactive state, but the screen now looks identical throughout.) */

    /* [#8] Re-lay the lobby's two action buttons (created by Screen_MultiplayerLobby
     * as 0=START, 1=BACK) SIDE BY SIDE, SAME WIDTH, lowered a little. s_buttons[]
     * is the layout source of truth (frontend_get_button_render_rect + the mouse
     * hit-test both read x/y/w/h), and this overlay runs every frame just before
     * the generic button-draw loop, so updating the rects here is sufficient and
     * keeps both buttons fully functional (nav/SPACE/ESC are index-based; the
     * mouse rect follows). Two equal 150-wide buttons + 16px gap = 316px, centred
     * about x=320 (left edge 162), both at y=372 (below the original y300/y360). */
    if (relayout && s_button_count >= 2 &&
        s_buttons[0].active && s_buttons[1].active) {
        const int btn_w  = 150;   /* equal width for both */
        const int btn_h  = 32;
        const int gap    = 16;
        const int btn_y  = 372;   /* a little further down */
        const int left_x = 320 - (btn_w * 2 + gap) / 2;   /* 162 */
        s_buttons[0].x = left_x;                 /* START (left)  */
        s_buttons[0].y = btn_y;
        s_buttons[0].w = btn_w;
        s_buttons[0].h = btn_h;
        s_buttons[1].x = left_x + btn_w + gap;   /* BACK (right)  */
        s_buttons[1].y = btn_y;
        s_buttons[1].w = btn_w;
        s_buttons[1].h = btn_h;
    }

    /* [title font 2026-06] The orange "MULTIPLAYER" title was removed — the
     * screen now shows the standard top header "MULTIPLAYER" (Lunatica title
     * face) via frontend_get_title_text_for_screen, matching every other menu. */
    fe_draw_text_centered(320.0f * sx,  78.0f * sy, SNK_PressJoinTxt, 0xFFCCCCCC, sx*0.8f, sy*0.8f);

    /* [MP NAME INDICATOR 2026-06] One roster row per joined player: an accent
     * SWATCH (that player's identity colour) + their NAME (live working name if
     * set, else the session-persisted name from a prior race, else "PLAYER n"),
     * then the device + READY tag. Mirrors the net-lobby roster layout
     * (FE_LOBBY_* constants), drawn directly (unselectable). */
    for (p = 0; p < s_mp_joined_count && p < TD5_MAX_HUMAN_PLAYERS; p++) {
        char line[80];
        const char *dev = td5_input_get_device_name(s_mp_join_device[p]);
        const char *name;
        uint32_t accent;
        float row_y = (float)(FE_LOBBY_ROW0_Y + p * FE_LOBBY_ROW_H);
        if (!dev || !dev[0]) dev = "?";

        /* Name + accent: prefer the live working values; fall back to the
         * session store so returning players show their saved identity before
         * the lobby START restore runs. */
        if (s_mp_player_name[p][0])      name = s_mp_player_name[p];
        else if (mp_session_player_name(p)) name = mp_session_player_name(p);
        else                              name = NULL;
        accent = (uint32_t)(s_mp_player_accent[p] ? s_mp_player_accent[p]
                                                  : mp_session_player_accent(p));
        if (!accent)
            accent = k_mp_player_colors[p % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu;

        /* Accent swatch (white tex tinted to the identity colour, full alpha).
         * [#4] Center the swatch on the name's cap-band — fe_draw_text anchors at
         * the glyph cell-top, so a same-y swatch reads too high. Offset helper is
         * defined in td5_fe_net.c (TD5RE_LOBBY_ALIGN). Name text below keeps row_y. */
        extern float frontend_lobby_swatch_y_offset(float text_scale, float swatch_h);
        fe_draw_quad((float)FE_LOBBY_X * sx,
                     (row_y + frontend_lobby_swatch_y_offset(0.8f, 14.0f)) * sy,
                     16.0f * sx, 14.0f * sy,
                     0xFF000000u | accent, -1, 0.0f, 0.0f, 1.0f, 1.0f);

        if (name)
            snprintf(line, sizeof line, "PLAYER %d  %s  -  %s  -  %s",
                     p + 1, name, dev, SNK_ReadyTxt);
        else
            snprintf(line, sizeof line, "PLAYER %d  -  %s  -  %s",
                     p + 1, dev, SNK_ReadyTxt);
        fe_draw_text((float)(FE_LOBBY_X + 22) * sx, row_y * sy,
                     line, 0xFF33FF33u, sx*0.8f, sy*0.8f);
    }
    if (s_mp_joined_count == 0)
        fe_draw_text_centered(320.0f * sx, 150.0f * sy, "( no players yet )", 0xFF888888, sx*0.8f, sy*0.8f);

    /* [#8] The single join/start/back help legend is dropped under the default
     * (side-by-side button) layout; TD5RE_LOBBY_BUTTONS=0 keeps the old line. */
    if (!relayout)
        fe_draw_text_centered(320.0f * sx, 340.0f * sy,
                              "ENTER / A = JOIN     SPACE = START     ESC / B = BACK",
                              0xFF999999, sx*0.62f, sy*0.62f);
}

void frontend_format_score_time(char *buf, size_t cap, int raw_ticks, int type) {
    /* Convert from game ticks (30fps) to displayable time.
     * Type 0/1: MM:SS.cc   Type 4: MM:SS.mmm */
    if (type == 2) {
        /* Points mode — raw value IS the point count */
        snprintf(buf, cap, "%d", raw_ticks);
        return;
    }
    int centiseconds = (raw_ticks * 100) / 30;
    int total_sec = centiseconds / 100;
    int frac = centiseconds % 100;
    int minutes = total_sec / 60;
    int seconds = total_sec % 60;
    if (type == 4) {
        int ms = (raw_ticks * 1000) / 30;
        minutes = (ms / 1000) / 60;
        seconds = (ms / 1000) % 60;
        snprintf(buf, cap, "%02d:%02d.%03d", minutes, seconds, ms % 1000);
    } else {
        snprintf(buf, cap, "%02d:%02d.%02d", minutes, seconds, frac);
    }
}

static int frontend_convert_speed(int raw, int kph_mode) {
    /* Same conversion as HUD speedometer (td5_hud.c) */
    if (kph_mode)
        return (raw * 256 + 389) / 778;   /* KPH */
    else
        return (raw * 256 + 625) / 1252;  /* MPH */
}

static void frontend_render_high_score_overlay(float sx, float sy) {
    if (!s_anim_complete || s_inner_state < 6) return;

    /* [#2b 2026-06-16] TD6 tracks have no authored NPC group, so the post-race
     * table must show ONLY genuine records (the player's actual runs) — never the
     * placeholder names a clamped TD5 group would render. When the post-race flow
     * flagged a TD6 track (s_postrace_td6_level > 0), pull that level's genuine
     * record table; if it has none yet, fall through to the "NO RECORDS YET" empty
     * state below (grp == NULL). The Records browse screen leaves the flag 0, so
     * it still shows the authored TD5 groups unchanged. */
    const TD5_NpcGroup *grp;
    if (s_postrace_td6_level > 0)
        grp = td5_save_get_td6_record_group(s_postrace_td6_level);
    else
        grp = td5_save_get_npc_group(s_score_category_index);
    int speed_kph = td5_save_get_speed_units();  /* drives the AVERAGE/TOP value conversion;
                                                  * the column header is "SPEED" (SNK_SpdTxt),
                                                  * not the unit, per DrawPostRaceHighScoreEntry */

    /* Panel: 0x208 x 0x90 = 520x144 surface at (115,177). [FIXED 2026-06-01] The original
     * (DrawPostRaceHighScoreEntry @0x00413010) clears this surface to black then composites
     * it COLOR-KEYED on black (QueueFrontendOverlayRect trailing-0), so ONLY THE TEXT shows
     * over the MainMenu backdrop — there is NO visible framed box. The prior opaque
     * white-border + dark-fill quads were a port invention; removed. */
    /* The original draws this whole table in the SMALL font (smalltext.tga, 12px, true
     * lowercase incl. a normal 'y') at panel-local coords [CONFIRMED @0x00413010]; the port
     * now matches it with fe_draw_small_text instead of the scaled button font (which lacked
     * proper lowercase metrics — the 'y' rendered tall). Columns CENTERED in panel-local
     * [left,right]: NAME[16,112] SCORE[128,212] CAR[228,336] AVERAGE[352,428] TOP[444,520]. */
    #define HS_SF_X(LX)      ((115.0f + (float)(LX)) * sx)
    #define HS_SF_Y(LY)      ((177.0f + (float)(LY)) * sy)
    /* Centre text S in panel-local column [L,R]: anchor the COLUMN centre at *sx
     * (UI layout space), then subtract HALF the rendered text width at the GLYPH
     * scale fe_glyph_sx(sx,sy) (= the 4:3-locked min(sx,sy)). Multiplying the text
     * width by gsx (not sx) keeps the now-square, non-stretched glyphs centred in
     * the column at every aspect ratio. */
    #define HS_SF_CTR(L,R,S) ((115.0f + (float)(L) + (float)((R)-(L)) * 0.5f) * sx \
                              - fe_measure_small_text(S) * 0.5f * fe_glyph_sx(sx, sy))

    if (!grp) {
        /* [#2b] TD6 with no genuine records yet shows "NO RECORDS YET" (the player
         * just hasn't set one) rather than the generic "NO SCORES YET". */
        const char *msg = (s_postrace_td6_level > 0) ? "NO RECORDS YET" : "NO SCORES YET";
        fe_draw_small_text((320.0f * sx) - (fe_measure_small_text(msg) * 0.5f) * fe_glyph_sx(sx, sy),
                           HS_SF_Y(60), msg, 0xFFCCCCCC, sx, sy);
        return;
    }

    int score_type = grp->header & 0xFF;
    uint32_t hdr_color = 0xFFFFFFFF;
    /* Two-row header [CONFIRMED @0x413010]: NAME/CAR + the TIME|LAP|POINTS label at y=7;
     * BEST / AVERAGE / TOP at y=0; the second "SPEED" line + TIME/LAP at y=14. */
    float y0  = HS_SF_Y(0);
    float y7  = HS_SF_Y(7);
    float y14 = HS_SF_Y(14);
    fe_draw_small_text(HS_SF_CTR(0x10,0x70,"NAME"), y7, "NAME", hdr_color, sx, sy);
    if (score_type == 2) {
        fe_draw_small_text(HS_SF_CTR(0x80,0xd4,"POINTS"), y7, "POINTS", hdr_color, sx, sy);
    } else {
        const char *tlabel = (score_type == 1) ? "LAP" : "TIME";
        fe_draw_small_text(HS_SF_CTR(0x80,0xd4,"BEST"), y0,  "BEST", hdr_color, sx, sy);
        fe_draw_small_text(HS_SF_CTR(0x80,0xd4,tlabel), y14, tlabel, hdr_color, sx, sy);
    }
    fe_draw_small_text(HS_SF_CTR(0xe4,0x150,"CAR"),      y7,  "CAR",     hdr_color, sx, sy);
    fe_draw_small_text(HS_SF_CTR(0x160,0x1ac,"AVERAGE"), y0,  "AVERAGE", hdr_color, sx, sy);
    fe_draw_small_text(HS_SF_CTR(0x160,0x1ac,"SPEED"),   y14, "SPEED",   hdr_color, sx, sy);
    fe_draw_small_text(HS_SF_CTR(0x1bc,0x208,"TOP"),     y0,  "TOP",     hdr_color, sx, sy);
    fe_draw_small_text(HS_SF_CTR(0x1bc,0x208,"SPEED"),   y14, "SPEED",   hdr_color, sx, sy);

    /* 5 entry rows at panel-local y = 48,64,80,96,112 (step 16). */
    for (int i = 0; i < 5; i++) {
        const TD5_NpcEntry *e = &grp->entries[i];
        float y = HS_SF_Y(48 + i * 16);
        /* Highlight row = g_postRaceQualifyingScore (orig bolds it via SmallTextb). Browse
         * mode defaults to 0 → #1 row; post-insert it's the inserted rank. Port has no bold
         * atlas, so the highlight is rendered YELLOW (user-confirmed). */
        int hl_row = (s_score_insert_pos >= 0) ? s_score_insert_pos : 0;
        /* [FIXED 2026-06-01] #1/insert row = the GOLD accent (sampled from the rendered
         * original High Scores: #1 row ≈ (208,203,23), title ≈ (217,197,12) — a muted gold,
         * NOT the bright yellow 0xFFFFE000 I'd guessed). */
        uint32_t row_color = (i == hl_row) ? 0xFFD9C50C : 0xFFE0E0E0;
        char buf[64];

        if (e->name[0] == '\0') {
            fe_draw_small_text(HS_SF_X(0), y, "---", 0xFF888888, sx, sy);
            continue;
        }

        /* Rank flush at panel x=0; name at x=0x10 — names render MIXED-CASE (seed names
         * are stored "Frank"/"Jeffrey"; the small font has true lowercase). */
        snprintf(buf, sizeof(buf), "%d", i + 1);
        fe_draw_small_text(HS_SF_X(0), y, buf, row_color, sx, sy);
        {
            char name_buf[14];
            memcpy(name_buf, e->name, 13);
            name_buf[13] = '\0';
            fe_draw_small_text(HS_SF_X(16), y, name_buf, row_color, sx, sy);
        }

        /* Score / Time centered in [0x80,0xd4]. */
        frontend_format_score_time(buf, sizeof(buf), e->score, score_type);
        fe_draw_small_text(HS_SF_CTR(0x80,0xd4,buf), y, buf, row_color, sx, sy);

        /* Car SHORT name (e.g. "'97 CAMARO") centered in [0xe4,0x150], clipped to 108px. */
        {
            int cid = e->car_id & 0xFF;
            const char *cname = frontend_get_car_short_name(cid);
            char cname_buf[20];
            strncpy(cname_buf, cname, sizeof(cname_buf) - 1);
            cname_buf[sizeof(cname_buf) - 1] = '\0';
            {
                int clen = (int)strlen(cname_buf);
                while (clen > 0 && fe_measure_small_text(cname_buf) > 108.0f)
                    cname_buf[--clen] = '\0';
            }
            fe_draw_small_text(HS_SF_CTR(0xe4,0x150,cname_buf), y, cname_buf, row_color, sx, sy);
        }

        /* Average / Top speed WITH unit (orig "%dMPH"/"%dKPH"), centered. */
        {
            const char *unit = speed_kph ? "KPH" : "MPH";
            snprintf(buf, sizeof(buf), "%d%s", frontend_convert_speed(e->avg_speed, speed_kph), unit);
            fe_draw_small_text(HS_SF_CTR(0x160,0x1ac,buf), y, buf, row_color, sx, sy);
            snprintf(buf, sizeof(buf), "%d%s", frontend_convert_speed(e->top_speed, speed_kph), unit);
            fe_draw_small_text(HS_SF_CTR(0x1bc,0x208,buf), y, buf, row_color, sx, sy);
        }
    }
    #undef HS_SF_X
    #undef HS_SF_Y
    #undef HS_SF_CTR
}

/* ============================================================================
 * frontend_render_race_results_overlay
 * Per-frame draw for TD5_SCREEN_RACE_RESULTS (original FUN_00422480).
 *
 * RE basis:
 *   - Panel surface 0x198 x 0x188 = 408 x 392 px, black-filled.
 *     [CONFIRMED @ 0x004225F7 CreateTrackedFrontendSurface]
 *   - Panel centered at (canvas_w/2 - 0xA8, canvas_h/2 - 0x9F).
 *   - Column-header source surface differs per game_type:
 *     types <1: SNK_ResultsTxt+0x40;  7: SNK_DRResultsTxt;  8: SNK_CCResultsTxt;
 *     1/6: SNK_ResultsTxt;  2-5: three SNK_ResultsTxt sub-strips;  9: skips rows.
 *   - Original has no per-frame render sibling — panel+label are queued via
 *     QueueFrontendOverlayRect from the state-machine body each tick.
 *     The port collapses this into this overlay dispatched from
 *     td5_frontend_render_ui_rects.
 *
 * [RESOLVED 2026-06-01] Panel: the original CLEARS its offscreen surface to black
 * (BltColorFillToSurface 0 @0x00422685) then composites it to screen with COLOR-KEY
 * transparency on black (QueueFrontendOverlayRect(...,0,...) trailing-0 flag) — so the
 * black is keyed out and only the TEXT shows over the MainMenu backdrop. The port
 * correctly draws NO panel body (an opaque quad would wrongly cover the backdrop). The
 * old "[UNCERTAIN] 0xE0 alpha" note was stale — no such fill exists in the port now.
 * [RESOLVED] Column layout: the port DOES wire the per-game-type SNK_ResultsTxt /
 * SNK_DRResultsTxt / SNK_CCResultsTxt label ladder (LBL_* enum below), header X=0,
 * value X=0x118, row step 0x18 — matching the original. The old "POS/DRIVER/CAR/TIME"
 * note described a long-replaced high-score-style layout.
 * ========================================================================== */
static void frontend_render_race_results_overlay(float sx, float sy) {
    /* State gate: panel is created at state 0, first drawn at state 3
     * (slide-in) and persists through states 4-0xB plus 0xD-0x14. */
    if (s_inner_state < 3 || s_inner_state == 0xC) return;

    /* Panel geometry — byte-faithful to orig.
     * [CONFIRMED @ DrawRaceDataSummaryPanel 0x00421e90 + RunRaceResultsScreen
     *  case 0 @ 0x00422480]
     *   CreateTrackedFrontendSurface(0x198, 0x188)  → 408 × 392
     *   QueueFrontendOverlayRect(uVar3 - 0xa8, iVar6, ..., 0x198, 0x188, ...)
     *   uVar3 = g_frontendCanvasW >> 1 = 320 (640-wide canvas)
     *   iVar6 = (g_frontendCanvasH >> 1) - 0x9f = 240 - 159 = 81
     * → panel anchored at (152, 81), NOT centered. Orig leaves ~80px right
     * margin and ~152px left margin (the difference accommodates the
     * "RACE RESULTS" header banner at x=120 which sits left-of-center).
     *
     * P7 PANEL fix: apply per-state slide offset accumulated by states
     * 7/8/9/10 (L/R browse) and 0xB (exit). Zero during interactive
     * (state 6) and resting display (states 4/5). */
    const float panel_w = 408.0f, panel_h = 392.0f;
    float panel_x = (152.0f + (float)s_results_panel_slide_x) * sx;
    float panel_y = 81.0f * sy;
    float pw = panel_w * sx;
    float ph = panel_h * sy;

    /* During state 0xD+ the panel is reused as the main-menu backdrop,
     * so hide the results table (buttons are drawn on top by the
     * generic button loop in td5_frontend_render_ui_rects). */
    if (s_inner_state >= 0x0D) return;

    /* No panel body draw — orig blits g_lobbyErrorDialogSurface with DDraw
     * color-key transparency on color 0 (black), so the cleared surface
     * itself is INVISIBLE in the composite; only the text drawn into it
     * is shown. Drawing an opaque black quad here would cover the
     * MainMenu.tga backdrop, which is not what orig does.
     * [CONFIRMED @ 0x00422480 case 4-6 QueueFrontendOverlayRect(...,0,...)
     *  — the trailing 0 flag selects color-key blit mode]. */
    (void)pw; (void)ph;

    /* --- P4 LANG fix — Single-slot stat sheet (faithful) ---
     * [CONFIRMED @ 0x00421e90 DrawRaceDataSummaryPanel + 0x00422480 case 0]
     * Original screen 24 panel is a SINGLE-SLOT stat sheet, not a multi-slot
     * leaderboard table. Layout:
     *   - Left column @ panel x=0:    row LABEL strings (drawn ONCE by case 0,
     *     filled from SNK_ResultsTxt / SNK_DRResultsTxt / SNK_CCResultsTxt
     *     per-game-type ladder).
     *   - Right column @ panel x=0x118 (280 px): per-slot VALUES, drawn by
     *     DrawRaceDataSummaryPanel(DAT_00497a68 == s_score_category_index).
     *     The right column is cleared and re-filled when the user uses L/R
     *     to browse to a new slot (states 7..10 panel-slide L/R).
     *
     * Row labels EXTRACTED VERBATIM from original/Language.dll:
     *   SNK_ResultsTxt    @ DLL RVA=0x7720 (char[32][0x20])
     *   SNK_DRResultsTxt  @ DLL RVA=0x78C0
     *   SNK_CCResultsTxt  @ DLL RVA=0x7840
     * (The prior commit's hardcoded English column headers POS/DRIVER/CAR/TIME
     * were a port-only invention that did not exist in the original — they
     * are removed by this commit.)
     *
     * Per-game-type label ladder mirrors RunRaceResultsScreen 0x00422480
     * case 0: source offsets, Y rows, and step sizes confirmed verbatim.
     *
     * Driver name + car name lines above the stat list are a port-only
     * addition (the original puts those values in the right column at
     * specific row Y positions; the port's per-slot accessor model is
     * cleaner with a single header band). */

    int gt = (int)s_selected_game_type;

    int focus_slot = s_score_category_index;
    if (focus_slot < 0 || focus_slot >= TD5_MAX_RACER_SLOTS) focus_slot = 0;
    /* Drag-mode mask matches Ghidra @ 0x00422A02: (s_score_category_index & 1). */
    if (gt == 7 || gt == 9) focus_slot &= 1;

    /* Row index into the panel's vertical stat list. The original spaces rows
     * at Y = 0x30..0xE8 (cup) or 0x48..0x90 (cop) or 0x60..0xD8 (single/drag),
     * step 0x18. We bake this into a helper struct and a per-game-type table. */
    enum {
        LBL_CUP_POSITION,        /* SNK_ResultsTxt[0]   "CUP POSITION" */
        LBL_CUP_POINTS,          /* SNK_ResultsTxt[1]   "CUP POINTS" */
        LBL_AVG_SPEED,           /* SNK_ResultsTxt[2]   "AVERAGE SPEED" */
        LBL_TOP_SPEED,           /* SNK_ResultsTxt[3]   "TOP SPEED" */
        LBL_FINISH_POSITION,     /* SNK_ResultsTxt[4]   "FINISH POSITION" */
        LBL_HIGHEST_POSITION,    /* SNK_ResultsTxt[5]   "HIGHEST POSITION" */
        LBL_TOTAL_TIME,          /* SNK_ResultsTxt[6]   "TOTAL TIME" */
        LBL_CHECKPOINT_TIMERS,   /* SNK_ResultsTxt[7]   "CHECKPOINT TIMERS" */
        LBL_CUP_TIME,            /* SNK_ResultsTxt[8]   "CUP TIME"  (only cups 2-5) */
        LBL_ARRESTS,             /* SNK_CCResultsTxt[0] "ARRESTS" */
        LBL_POINTS               /* SNK_CCResultsTxt[3] "POINTS" */
    };

    /* Localized row label strings — extracted from Language.dll on 2026-05-01.
     * Indexed by the LBL_* enum. Match exact bytes from RVA dumps (see commit
     * message). Language switching for non-English would re-bake this table
     * from the appropriate Language.dll variant. */
    static const char *const k_results_labels[] = {
        [LBL_CUP_POSITION]      = "CUP POSITION",
        [LBL_CUP_POINTS]        = "CUP POINTS",
        [LBL_AVG_SPEED]         = "AVERAGE SPEED",
        [LBL_TOP_SPEED]         = "TOP SPEED",
        [LBL_FINISH_POSITION]   = "FINISH POSITION",
        [LBL_HIGHEST_POSITION]  = "HIGHEST POSITION",
        [LBL_TOTAL_TIME]        = "TOTAL TIME",
        [LBL_CHECKPOINT_TIMERS] = "CHECKPOINT TIMERS",
        [LBL_CUP_TIME]          = "CUP TIME",
        [LBL_ARRESTS]           = "ARRESTS",
        [LBL_POINTS]            = "POINTS",
    };

    /* Per-game-type label sequence, terminated by -1. Y position is row*0x18
     * relative to the row block start. Order + content matches the per-
     * game-type ladder in case 0 of 0x00422480. */
    static const int k_rows_single[]   = { LBL_AVG_SPEED, LBL_TOP_SPEED, LBL_FINISH_POSITION,
                                           LBL_HIGHEST_POSITION, LBL_TOTAL_TIME, LBL_CHECKPOINT_TIMERS, -1 };
    static const int k_rows_cup_1_6[]  = { LBL_CUP_POSITION, LBL_CUP_POINTS, LBL_AVG_SPEED, LBL_TOP_SPEED,
                                           LBL_FINISH_POSITION, LBL_HIGHEST_POSITION, LBL_TOTAL_TIME,
                                           LBL_CHECKPOINT_TIMERS, -1 };
    static const int k_rows_cup_2_5[]  = { LBL_CUP_POSITION, LBL_CUP_TIME, LBL_AVG_SPEED, LBL_TOP_SPEED,
                                           LBL_FINISH_POSITION, LBL_HIGHEST_POSITION, LBL_TOTAL_TIME,
                                           LBL_CHECKPOINT_TIMERS, -1 };
    static const int k_rows_drag[]     = { LBL_TOTAL_TIME, LBL_TOP_SPEED, LBL_FINISH_POSITION, -1 };
    static const int k_rows_cop[]      = { LBL_ARRESTS, LBL_AVG_SPEED, LBL_TOP_SPEED, LBL_POINTS, -1 };
    /* Drag race (gt==9) skips Y=0x90 (FINISH_POSITION) and Y=0xA8
     * (HIGHEST_POSITION) per the case-9 conditional in 0x00422480. */
    /* [FIXED 2026-06-01] Drag-race (gt 9) labels = SNK_DRResultsTxt, byte-extracted from
     * Language.dll: TOTAL TIME, TOP SPEED, FINISH POSITION. The prior set (AVG SPEED /
     * TOP SPEED / TOTAL TIME / CHECKPOINT TIMERS) was the generic single-race set — wrong
     * for drag (drag has no average-speed or checkpoint-timer rows). */
    static const int k_rows_drag_race[] = { LBL_TOTAL_TIME, LBL_TOP_SPEED, LBL_FINISH_POSITION, -1 };

    const int *rows;
    if (gt == 7)               rows = k_rows_drag;
    else if (gt == 8)          rows = k_rows_cop;
    else if (gt == 9)          rows = k_rows_drag_race;
    else if (gt == 1 || gt == 6) rows = k_rows_cup_1_6;
    else if (gt >= 2 && gt <= 5) rows = k_rows_cup_2_5;
    else                       rows = k_rows_single;

    /* Column anchors — byte-faithful to orig.
     * [CONFIRMED @ DrawRaceDataSummaryPanel 0x00421e90]:
     *   BltColorFillToSurface(0, 0x118, 0, 0x80, 0x188, surface)
     *     clears the value column at panel-relative x=0x118 (280), w=0x80 (128)
     *   Labels are drawn at panel-relative x=0 (flush left).
     * Row stride 0x18 = 24 px matches one font cell. Orig's BodyText atlas
     * has padding baked into each 24×24 cell so adjacent rows don't visually
     * touch — port reads from the same atlas at scale 1.0 to match. */
    float text_scale = 1.0f;
    float left_col   = panel_x;
    float right_col  = panel_x + 280.0f * sx;

    int kph = td5_save_get_speed_units();
    int unit_kph = (kph != 0);

    /* Stat list — per-game-type starting Y.
     * [CONFIRMED @ RunRaceResultsScreen case 0 @ 0x00422480]:
     *   Cup races (gt 1..6): label loop starts at uVar3 = 0x30
     *   Cop chase  (gt 8)  : label loop starts at uVar3 = 0x48
     *   Single/TT/Drag     : label loop starts at uVar3 = 0x60
     * Step is always 0x18 (24 px) and upper bound varies (0xa8 / 0xf0)
     * per game type — the port's k_rows_* tables already encode the
     * row count, so only the start Y needs to vary. */
    float row_start_y_px;
    if (gt >= 1 && gt <= 6)       row_start_y_px = (float)0x30;  /* 48 */
    else if (gt == 8)             row_start_y_px = (float)0x48;  /* 72 */
    else                          row_start_y_px = (float)0x60;  /* 96 */
    float row_block_y = panel_y + row_start_y_px * sy;
    float row_step    = 24.0f * sy;
    /* Row labels are drawn in the same bright menu-font white as the values — the orig
     * DrawFrontendLocalizedStringToSurface uses the BodyText atlas at full intensity
     * (NOT a dimmed/gray label). [FIXED 2026-06-02 — was 0xFFCCCCCC gray.] */
    uint32_t lbl_color = 0xFFFFFFFF;
    uint32_t val_color = 0xFFFFFFFF;

    int row_idx = 0;
    for (int i = 0; rows[i] != -1; i++) {
        int label_id = rows[i];
        float y = row_block_y + (float)row_idx * row_step;

        /* Left column: localized label. */
        fe_draw_text(left_col, y, k_results_labels[label_id], lbl_color,
                     sx * text_scale, sy * text_scale);

        /* Right column: per-slot value derived from accessors. */
        char val_buf[32];
        switch (label_id) {
        case LBL_CUP_POSITION: {
            /* Original reads (&DAT_004660b4)[slot+0x14*0x14+0x2] — a cup-position
             * lookup table that the port has not surfaced. Use final_position+1
             * as a coarse fallback (cup position == finish position for the
             * final cup race). */
            int fp = td5_game_get_finish_position(focus_slot);
            if (fp >= 0) snprintf(val_buf, sizeof(val_buf), "%d", fp + 1);
            else         snprintf(val_buf, sizeof(val_buf), "-");
            break;
        }
        case LBL_CUP_POINTS:
            snprintf(val_buf, sizeof(val_buf), "%d",
                     (int)td5_game_get_result_secondary(focus_slot));
            break;
        case LBL_CUP_TIME: {
            int32_t t = td5_game_get_result_primary(focus_slot);
            if (t > 0) frontend_format_score_time(val_buf, sizeof(val_buf), t, 0);
            else       snprintf(val_buf, sizeof(val_buf), "-");
            break;
        }
        case LBL_AVG_SPEED: {
            int spd = (int)td5_game_get_result_avg_speed(focus_slot);
            snprintf(val_buf, sizeof(val_buf), "%d %s",
                     frontend_convert_speed(spd, unit_kph),
                     unit_kph ? "KPH" : "MPH");
            break;
        }
        case LBL_TOP_SPEED: {
            int spd = (int)td5_game_get_result_top_speed(focus_slot);
            snprintf(val_buf, sizeof(val_buf), "%d %s",
                     frontend_convert_speed(spd, unit_kph),
                     unit_kph ? "KPH" : "MPH");
            break;
        }
        case LBL_FINISH_POSITION: {
            int fp = td5_game_get_finish_position(focus_slot);
            if (fp >= 0 && td5_game_slot_is_finished(focus_slot))
                snprintf(val_buf, sizeof(val_buf), "%d", fp + 1);
            else
                snprintf(val_buf, sizeof(val_buf), "DNF");
            break;
        }
        case LBL_HIGHEST_POSITION: {
            /* [FIX 2026-05-25 hud-metrics; orig 0x00422216]
             *   Orig DrawRaceDataSummaryPanel reads (byte)actor+0x380
             *   (grip_reduction overloaded as "best position seen") into
             *   the position-name table at 0x4660b4. Wire the port to
             *   the same actor field via td5_game_get_highest_position. */
            int hp = td5_game_get_highest_position(focus_slot);
            if (hp >= 0 && hp < TD5_MAX_RACER_SLOTS)
                snprintf(val_buf, sizeof(val_buf), "%d", hp + 1);
            else
                snprintf(val_buf, sizeof(val_buf), "-");
            break;
        }
        case LBL_TOTAL_TIME: {
            int32_t t = td5_game_get_race_timer(focus_slot, 0);
            if (t > 0) frontend_format_score_time(val_buf, sizeof(val_buf), t, 0);
            else       snprintf(val_buf, sizeof(val_buf), "-");
            break;
        }
        case LBL_CHECKPOINT_TIMERS: {
            /* Original loops a short[] at slot+0x34e until *psVar2==0 or row
             * Y >= 0x150 — i.e. as many lap splits as exist. The port emits
             * the best lap time as a single value (slot's per-lap split list
             * is owned by td5_game; expanding to individual splits here would
             * hide labels behind a big block). Keep one summary line. */
            int32_t best = td5_game_get_best_lap_time(focus_slot);
            if (best > 0) frontend_format_score_time(val_buf, sizeof(val_buf), best, 0);
            else          snprintf(val_buf, sizeof(val_buf), "-");
            break;
        }
        case LBL_ARRESTS:
            /* [FIX 2026-05-25 hud-metrics; orig 0x0040AA0F]
             *   Orig BuildResultsTable copies actor +0x384 (low byte) into
             *   s_results +0x10 (wanted_kills); results-screen reads it
             *   via byte ptr [0x0048d998]. Wire to td5_game_get_wanted_kills
             *   which falls back to actor->special_encounter_state. */
            snprintf(val_buf, sizeof(val_buf), "%d",
                     td5_game_get_wanted_kills(focus_slot));
            break;
        case LBL_POINTS:
            snprintf(val_buf, sizeof(val_buf), "%d",
                     (int)td5_game_get_result_secondary(focus_slot));
            break;
        default:
            val_buf[0] = '\0';
            break;
        }
        fe_draw_text(right_col, y, val_buf, val_color,
                     sx * text_scale, sy * text_scale);
        row_idx++;
    }
}

/* Extras = the scrolling CREDITS reel [FAITHFUL 2026-06-02, was a photo slideshow].
 * Original ScreenExtrasGallery (0x417d50) composites SNK_CreditsText rows into a vertical
 * reel (screen x=0xcc=204, w=0x140=320): text lines are 32px rows; "#X" entries are dev
 * mugshots (320x224 = 7 rows). The reel scrolls up ~1px/frame (a new row every 32 frames). */
#define FE_CREDITS_REEL_X   204.0f  /* 0xcc */
#define FE_CREDITS_REEL_W   320.0f  /* 0x140 */


/* [1] Positioner dev tool overlay — faithful to ScreenPositionerDebugTool @0x00415030 +
 * RenderPositionerGlyphStrip @0x00414F40. The original clears to BLACK, draws two white
 * guide scanlines (FillPrimaryFrontendScanline at y=0x10c=268 and y=0x114=276), then a
 * horizontal strip of the menu-font glyphs centred on the selected glyph (it walks
 * selected-8..selected+8 over the glyph set @0x465960 = ASCII 0x20..0x67) with the
 * selected glyph also shown enlarged below. It is a developer font-metrics editor: the
 * arrow keys move the selection / nudge metrics and ESC writes positioner.txt. There are
 * NO on-screen buttons in the original. [reimplemented faithful 2026-06-02] */
static const char k_positioner_glyphs[] =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_";

static void frontend_render_positioner_overlay(float sx, float sy) {
    const float W = 640.0f, H = 480.0f;
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    /* black background (orig ClearBackbufferWithColor(0)) */
    fe_draw_quad(0.0f, 0.0f, W * sx, H * sy, 0xFF000000, -1, 0, 0, 0, 0);
    /* two white guide scanlines */
    fe_draw_quad(0.0f, 268.0f * sy, W * sx, 2.0f * sy, 0xFFFFFFFF, -1, 0, 0, 0, 0);
    fe_draw_quad(0.0f, 276.0f * sy, W * sx, 2.0f * sy, 0xFFFFFFFF, -1, 0, 0, 0, 0);

    int n = (int)sizeof(k_positioner_glyphs) - 1;
    int sel = s_anim_tick % n;
    if (sel < 0) sel += n;
    /* glyph strip: selected-8 .. selected+8, centred on screen, selected highlighted */
    const float step = 30.0f;          /* horizontal pitch between strip glyphs */
    for (int k = -8; k <= 8; k++) {
        int gi = sel + k;
        if (gi < 0 || gi >= n) continue;
        char ch[2] = { k_positioner_glyphs[gi], 0 };
        float gx = (320.0f + (float)k * step) * sx;
        uint32_t col = (k == 0) ? 0xFFFFFF00u : 0xFFFFFFFFu;
        fe_draw_text_centered(gx, 232.0f * sy, ch, col, 1.0f * sx, 1.0f * sy);
    }
    /* selected glyph shown enlarged below (orig zoomed reference glyph) */
    char selch[2] = { k_positioner_glyphs[sel], 0 };
    fe_draw_text_centered(320.0f * sx, 300.0f * sy, selch, 0xFFFFFF00u, 2.2f * sx, 2.2f * sy);
}

static void frontend_render_extras_gallery_overlay(float sx, float sy) {
    /* Black background fills entire viewport */
    if (s_white_tex_page >= 0) {
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xFF000000,
                     s_white_tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    if (s_inner_state < 2) return;  /* still in the entry delay */

    const float text_cx = FE_CREDITS_REEL_X + FE_CREDITS_REEL_W * 0.5f; /* 364 */
    float scroll = (float)(td5_plat_time_ms() - s_credits_start_ms) * FE_CREDITS_SPEED;
    float cy = 0.0f;
    for (int i = 0; i < K_CREDITS_COUNT; i++) {
        const char *e = k_credits[i];
        float screen_y = (480.0f - scroll) + cy;  /* content starts at the bottom, scrolls up */
        if (e[0] == '#') {
            int slot = (e[1] >= 'A' && e[1] <= 'Z') ? (e[1] - 'A') : -1;
            if (slot >= 0 && slot < K_CREDIT_MUGSHOT_COUNT && s_credit_mugshot_surf[slot] > 0 &&
                screen_y > -FE_CREDITS_PHOTO_H && screen_y < 480.0f) {
                int ms = s_credit_mugshot_surf[slot] - 1;
                if (ms >= 0 && ms < FE_MAX_SURFACES && s_surfaces[ms].in_use) {
                    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
                    fe_draw_quad(FE_CREDITS_REEL_X * sx, screen_y * sy,
                                 FE_CREDITS_REEL_W * sx, FE_CREDITS_PHOTO_H * sy,
                                 0xFFFFFFFF, s_surfaces[ms].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
                }
            }
            cy += FE_CREDITS_PHOTO_H;
        } else {
            if (e[0] && e[0] != ' ' && screen_y > -FE_CREDITS_ROW_H && screen_y < 480.0f)
                fe_draw_text_centered(text_cx * sx, screen_y * sy, e, 0xFFFFFFFF, sx, sy);
            cy += FE_CREDITS_ROW_H;
        }
    }
}

/* ============================================================================
 * SHARED MENU TEXT PATH  [S02 (a) — canonical API, confirmed 2026-06-04]
 *
 * fe_draw_text / fe_draw_text_centered are THE single text-draw helpers for the
 * frontend. Every menu screen renders its captions, values, titles and hints
 * through them — including the newer port screens (MULTIPLAYER lobby, controller
 * binding, track / quick-race option rows). Adding a new screen needs NO baked
 * word art: it just calls fe_draw_text.
 *
 * One font drives all of it: BodyText (re/assets/frontend/BodyText.*). With
 * VectorUI on (the default, [Frontend] VectorUI=1) the glyphs come from the
 * resolution-independent MSDF atlas (BodyText_msdf.png) via s_ps_msdf; with it
 * off they come from the BodyText.tga bitmap atlas. The baked button cache
 * (td5_frontend_button_cache.c) composites from the SAME BodyText atlas and the
 * SAME s_font_glyph_advance metrics, so cached button captions and live text
 * share one set of letterforms. fe_measure_text* mirrors that advance table so
 * centering matches the rendered width.
 *
 * fe_draw_small_text (smalltext atlas) is the ONLY deliberate second font — the
 * dense High-Scores / Race-Results tables need true lowercase + descenders. It
 * has its own MSDF variant and measure helper, on the same single-path model.
 *
 * Practical rules:
 *   - Draw menu text with fe_draw_text(_centered); never bake a per-screen text
 *     TGA. (Screen TITLE logos like QuickRaceText.tga are the legacy exception;
 *     new screens render their title with fe_draw_text too — see the MULTIPLAYER
 *     lobby's SNK_MultiplayerTitleTxt.)
 *   - A selector button must NOT set is_selector if its caption should render
 *     through this path: the VectorUI button path only auto-draws captions for
 *     NON-selector buttons. Selectors that keep a caption draw it via this path
 *     (Quick Race rows) or via a per-screen overlay (controller binding).
 * ========================================================================== */
static float fe_measure_text_width(const char *text, float sx, float sy) {
    float w = 0.0f;
    float gsx = fe_glyph_sx(sx, sy);   /* 4:3-locked glyph width (see fe_glyph_sx) */
    if (!text) return 0.0f;
    if (td5_font_ready()) {            /* native TTF: real advances + tracking (matches fe_draw_text) */
        const float cap_px = 15.0f * sy;
        const float hscale = (sx < sy) ? (sx / sy) : 1.0f;
        const float trkn   = (float)FONT_GLYPH_TRACKING * sy * hscale;
        for (int i = 0; text[i]; i++)
            w += td5_font_advance(toupper((unsigned char)text[i]), cap_px) * hscale + trkn;
        return w;
    }
    for (int i = 0; text[i]; i++) {
        int c = toupper((unsigned char)text[i]);
        if (c < 32 || c > 127) { w += (14.0f + FONT_GLYPH_TRACKING) * gsx; continue; }
        w += ((float)s_font_glyph_advance[c - 0x20] + FONT_GLYPH_TRACKING) * gsx;
    }
    return w;
}

static void fe_draw_text_centered(float center_x, float y, const char *text,
                                  uint32_t color, float sx, float sy) {
    /* Center on the GLYPH width (fe_glyph_sx), which is exactly what fe_draw_text
     * advances by — so the string stays centered on center_x at every aspect
     * while the surrounding button rect may still scale by the full sx. */
    float w = fe_measure_text_width(text, sx, sy);
    fe_draw_text(center_x - w * 0.5f, y, text, color, sx, sy);
}

/* ---- frontend screen-header text (Lunatica title face) -------------------
 * Renders the big header at the top of each frontend screen as text in the
 * td5_titlefont face, replacing the legacy baked title-strip art. Uses the same
 * native-TTF mechanism as fe_draw_text (shared glyph atlas, translucent preset)
 * but a larger cap height, the display typeface, a faux-italic slant for a more
 * cursive look, and tighter letter tracking. Titles are LEFT-ALIGNED at a fixed
 * x so the first letter starts in the same place on every screen. The 4:3 lock
 * (hscale) matches fe_draw_text so titles condense the same way on windows
 * narrower than 4:3. */
#define FE_TITLE_CAP_PX  24.0f   /* design cap height (px at 480-tall reference) */
#define FE_TITLE_LEFT_X  126.0f  /* design x where the first letter starts (every screen);
                                  * = main-menu button left edge (FE_CENTER_X - 0xC2 = 320-194) */
#define FE_TITLE_SLANT   0.20f   /* faux-italic shear (x shift per y unit above baseline) */
#define FE_TITLE_TRACK  (-1.5f)  /* extra letter tracking (design px; negative = tighter) */

/* Draw `text` as a screen header left-aligned with its first letter at `left_x`.
 * `top_y` is where the cap tops land (the slide-in animated Y from the caller). */
static void frontend_draw_screen_title(const char *text, float left_x, float top_y,
                                       uint32_t color, float sx, float sy) {
    if (!text || !td5_titlefont_ready()) return;
    const float cap_px   = FE_TITLE_CAP_PX * sy;
    const float baseline = top_y + cap_px;            /* cap tops land near top_y */
    const float hscale   = (sx < sy) ? (sx / sy) : 1.0f;
    const float trkn     = FE_TITLE_TRACK * sy * hscale;
    float pen = left_x;
    for (int i = 0; text[i]; i++) {                   /* pass 1: rasterise into atlas */
        td5_glyph g; td5_titlefont_get(toupper((unsigned char)text[i]), cap_px, &g);
    }
    td5_font_flush_uploads();                         /* one GPU upload for new glyphs */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (int i = 0; text[i]; i++) {                   /* pass 2: draw (cache hits) */
        int c = toupper((unsigned char)text[i]);
        td5_glyph g; td5_titlefont_get(c, cap_px, &g);
        if (g.valid && g.w > 0.0f) {
            float gx = pen + g.xoff * hscale;
            float gy = baseline + g.yoff;             /* glyph quad top edge */
            /* Faux-italic: shear x by the height above the baseline so the cap
             * tops lean right while the baseline stays put. */
            float dx_top = FE_TITLE_SLANT * (baseline - gy);
            float dx_bot = FE_TITLE_SLANT * (baseline - (gy + g.h));
            fe_draw_quad_sheared(gx, gy, g.w * hscale, g.h, dx_top, dx_bot,
                                 color, g.page, g.u0, g.v0, g.u1, g.v1);
        }
        pen += g.advance * hscale + trkn;
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy) {
    /* Font atlas: BodyText.tga (or GDI fallback), 10 chars/row, 24x24 cells.
     * Layout: col = (ascii - 0x20) % 10, row = (ascii - 0x20) / 10.
     * Source rect: (col*24, row*24, 24, 24). From Ghidra FUN_00424560. */
    if (!text) return;

    /* [S13] Native TTF path: rasterise the menu font straight from its outlines
     * at the on-screen pixel size (crisp at any resolution, real metrics). Caps
     * are placed at the same 24px-cell rows (8..23) the bitmap path used, so the
     * button-centred layout carries over; the 4:3 lock condenses horizontally
     * only when the window is narrower than 4:3 (glyphs stay square otherwise).
     * Falls through to the MSDF/bitmap atlas below when no TTF is available. */
    if (td5_font_ready()) {
        const float cap_px   = 15.0f * sy;             /* caps 15px tall (design rows 8..23) */
        const float baseline = y + 23.0f * sy;         /* design baseline = cell row 23 */
        const float hscale   = (sx < sy) ? (sx / sy) : 1.0f;
        const float trkn     = (float)FONT_GLYPH_TRACKING * sy * hscale;
        for (int i = 0; text[i]; i++) {                /* pass 1: rasterise into the atlas */
            int c = s_fe_preserve_case ? (unsigned char)text[i] : toupper((unsigned char)text[i]);
            td5_glyph g; td5_font_get(c, cap_px, &g);
        }
        td5_font_flush_uploads();                      /* one GPU upload for any new glyphs */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        float ncx = x;
        for (int i = 0; text[i]; i++) {                /* pass 2: draw (cache hits) */
            int c = s_fe_preserve_case ? (unsigned char)text[i] : toupper((unsigned char)text[i]);
            td5_glyph g; td5_font_get(c, cap_px, &g);
            if (g.valid && g.w > 0.0f)
                fe_draw_quad(ncx + g.xoff * hscale, baseline + g.yoff,
                             g.w * hscale, g.h, color, g.page, g.u0, g.v0, g.u1, g.v1);
            ncx += g.advance * hscale + trkn;
        }
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        return;
    }
    if (s_font_page < 0) return;

    /* Bitmap glyph-atlas fallback (BodyText.png, page s_font_page). Reached only
     * when the menu TTF is unavailable (the early-return above). 10 chars/row,
     * 24x24 cells, per-glyph advance.
     * [2026-06-16] The MSDF body-text atlas branch was removed (atlas load
     * retired; s_msdf_font_page is always -1) — the TTF is the sole vector path. */
    int page = s_font_page;
    float cx = x;
    float texel_w = 1.0f / (float)FONT_TEX_W;
    float cell_h = (float)FONT_CELL * sy;
    float gsx = fe_glyph_sx(sx, sy);   /* 4:3-locked horizontal glyph scale; cell_h keeps sy */
    float trk = (float)FONT_GLYPH_TRACKING * gsx;  /* extra cursor tracking (negative = tighter) */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (int i = 0; text[i]; i++) {
        int c = s_fe_preserve_case ? (unsigned char)text[i] : toupper((unsigned char)text[i]);
        int glyph_index;
        float glyph_advance_px;
        float glyph_advance;
        float glyph_w;
        if (c < 32 || c > 127) { cx += 14.0f * gsx + trk; continue; }
        glyph_index = c - 0x20;
        glyph_advance_px = (float)s_font_glyph_advance[glyph_index];
        glyph_advance = glyph_advance_px * gsx;
        if (c == ' ') { cx += glyph_advance + trk; continue; }
        int col = glyph_index % FONT_COLS;
        int row = glyph_index / FONT_COLS;
        float u0 = (float)(col * FONT_CELL) / (float)FONT_TEX_W;
        float u1 = u0 + glyph_advance_px * texel_w;
        float v0 = (float)(row * FONT_CELL) / (float)FONT_TEX_H;
        float v1 = (float)((row + 1) * FONT_CELL) / (float)FONT_TEX_H;
        glyph_w = glyph_advance_px * gsx;
        fe_draw_quad(cx, y, glyph_w, cell_h, color, page, u0, v0, u1, v1);
        cx += glyph_advance + trk;
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* Sum of small-font advances (unscaled/design font px) — for centering. Preserves
 * case. When the native TTF is loaded this measures the SAME design-px advances
 * fe_draw_small_text advances by (cap = SMALLFONT_TTF_CAP design px), so every
 * column-centring / truncation site keeps working unchanged. */
float fe_measure_small_text(const char *text) {
    float w = 0.0f;
    if (!text) return 0.0f;
    if (td5_font_ready()) {            /* native TTF: real advances at the small cap size */
        const float cap = SMALLFONT_TTF_CAP;
        for (int i = 0; text[i]; i++)
            w += td5_font_advance((unsigned char)text[i], cap) + SMALLFONT_TTF_TRACK;
        return w;
    }
    for (int i = 0; text[i]; i++) {
        int c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7f) { w += 8.0f; continue; }
        w += (float)k_smallfont_advance[c - 0x20];
    }
    return w;
}

/* Draw a string in the small font (smalltext.tga). Mirrors the original
 * DrawFrontendSmallFontStringToSurface @0x00424660: 12x12 cells, 21 cols, per-char
 * vertical offset (so descenders like y/g/p/q sit right), proportional advance, and a
 * black color key (the texture is loaded with TD5_COLORKEY_BLACK). Case is preserved
 * (true lowercase glyphs). x/y are screen px; sx/sy are the canvas scale. */
void fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy) {
    if (!text) return;

    /* [SMALL-FONT TTF SWAP] Native TTF path: rasterise the menu font (the SAME
     * face fe_draw_text/buttons use) at a small on-screen cap size, instead of
     * the smalltext.tga bitmap / smalltext_msdf SDF. Horizontal advance = design
     * advance * sx, vertical = sy (the same stretch the bitmap path applied via
     * cell_w*sx / cell_h*sy), so fe_measure_small_text stays consistent and every
     * column-centred caller keeps its layout. Case is preserved (the TTF has true
     * lowercase). Falls through to the MSDF/bitmap path below when no TTF loaded. */
    if (td5_font_ready()) {
        static int s_logged_ttf = 0;
        if (!s_logged_ttf) {
            s_logged_ttf = 1;
            TD5_LOG_I(LOG_TAG, "small font: native TTF active (cap=%.1f baseline=%.1f track=%.1f design-px)",
                      (double)SMALLFONT_TTF_CAP, (double)SMALLFONT_TTF_BASELINE, (double)SMALLFONT_TTF_TRACK);
        }
        const float cap_px   = SMALLFONT_TTF_CAP * sy;          /* rasterise at on-screen vertical px */
        const float baseline = y + SMALLFONT_TTF_BASELINE * sy; /* design baseline below the cell top */
        /* 4:3 lock — IDENTICAL to fe_draw_text/buttons: never stretch horizontally
         * past square. When the window is WIDER than 4:3 (sx >= sy) hscale=1, so
         * glyphs advance at the vertical (sy) scale (square, not stretched to sx);
         * when NARROWER it condenses. The effective design->screen horizontal scale
         * is sy*hscale = min(sx,sy) = fe_glyph_sx(sx,sy) — exactly the width every
         * fe_measure_small_text caller now multiplies by, so positions are preserved. */
        const float hscale   = (sx < sy) ? (sx / sy) : 1.0f;
        const float trkn     = SMALLFONT_TTF_TRACK * sy * hscale;
        for (int i = 0; text[i]; i++) {                         /* pass 1: rasterise into the shared atlas */
            td5_glyph g; td5_font_get((unsigned char)text[i], cap_px, &g);
        }
        td5_font_flush_uploads();                               /* one GPU upload for any new glyphs */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        float tcx = x;
        for (int i = 0; text[i]; i++) {                         /* pass 2: draw (cache hits) */
            td5_glyph g; td5_font_get((unsigned char)text[i], cap_px, &g);
            if (g.valid && g.w > 0.0f)
                fe_draw_quad(tcx + g.xoff * hscale, baseline + g.yoff,
                             g.w * hscale, g.h, color, g.page, g.u0, g.v0, g.u1, g.v1);
            tcx += g.advance * hscale + trkn;
        }
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        return;
    }

    if (s_smallfont_page < 0) return;
    /* Bitmap small-font atlas fallback (page s_smallfont_page). Reached only when
     * the menu TTF is unavailable (the early-return above). 21x11 grid, per-glyph
     * advance + vertical offset.
     * [2026-06-16] The SmallText SDF atlas branch was removed (atlas load retired;
     * s_smallfont_msdf_page is always -1) — the TTF is the sole vector path. */
    int page = s_smallfont_page;
    float cx = x;
    float cell_w = (float)SMALLFONT_CELL * sx;
    float cell_h = (float)SMALLFONT_CELL * sy;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (int i = 0; text[i]; i++) {
        int c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7f) { cx += 8.0f * sx; continue; }
        int gi = c - 0x20;
        int col = gi % SMALLFONT_COLS;
        int row = gi / SMALLFONT_COLS;
        float adv = (float)k_smallfont_advance[gi];
        if (c != ' ') {
            float u0 = (float)(col * SMALLFONT_CELL) / SMALLFONT_TEX_W;
            float v0 = (float)(row * SMALLFONT_CELL) / SMALLFONT_TEX_H;
            float u1 = (float)((col + 1) * SMALLFONT_CELL) / SMALLFONT_TEX_W;
            float v1 = (float)((row + 1) * SMALLFONT_CELL) / SMALLFONT_TEX_H;
            float gy = y + (float)k_smallfont_yoffset[gi] * sy;
            fe_draw_quad(cx, gy, cell_w, cell_h, color, page, u0, v0, u1, v1);
        }
        cx += adv * sx;
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ButtonBits.tga 9-slice layout (56x100 texture, from FUN_00425b60).
 * Two columns: left x=0..25 (26px), right x=28..55 (28px), 2px gap.
 * Three button states at 32-row intervals (state*0x20):
 *   State 0 = gold/selected, State 1 = blue/unselected, State 2 = disabled.
 * Edge tile strip at rows 96-99, indexed by state*12. */
#define BB_TEX_W   56.0f
#define BB_TEX_H  100.0f
#define BB_LW      26       /* left column width */
#define BB_RW      28       /* right column width */
#define BB_RX      28       /* right column x start */
#define BB_TILE    4        /* edge tile size (4x4) */

/* Procedural rounded-rect button/frame (VectorUI), crisp at any resolution.
 * Diagonally-symmetric corners (matches the original ButtonBits): top-left &
 * bottom-right use r_large (smooth), top-right & bottom-left use r_small
 * (abrupt). border_px = rim band width. rim_argb/fill_argb are 0xAARRGGBB;
 * fill_alpha 0 = transparent interior (border only). Returns 0 if the
 * procedural path is unavailable (caller falls back to the bitmap button). */
static int fe_draw_roundrect(float x, float y, float w, float h,
                             float r_large, float r_small,
                             float border_side, float border_topbot,
                             uint32_t mid_argb, uint32_t inner_argb, uint32_t outer_argb,
                             uint32_t fill_argb, float fill_alpha) {
    if (!s_ps_roundrect || !s_rr_cb || !g_backend.context) return 0;
    FE_RoundRectParams rp;
    float rmax = 0.5f * (w < h ? w : h);
    if (r_large > rmax) r_large = rmax;
    if (r_small > rmax) r_small = rmax;
    rp.size_px[0] = w;  rp.size_px[1] = h;
    rp.border[0] = border_side;  rp.border[1] = border_topbot;
    rp.radii[0] = r_large;  /* TL smooth  */
    rp.radii[1] = r_small;  /* TR abrupt  */
    rp.radii[2] = r_small;  /* BL abrupt  */
    rp.radii[3] = r_large;  /* BR smooth  */
    td5_argb_to_rgb_f(rp.mid,   mid_argb);   rp.mid[3]   = 1.0f;
    td5_argb_to_rgb_f(rp.inner, inner_argb); rp.inner[3] = 1.0f;
    td5_argb_to_rgb_f(rp.outer, outer_argb); rp.outer[3] = 1.0f;
    td5_argb_to_rgb_f(rp.fill,  fill_argb);  rp.fill[3]  = fill_alpha;
    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
        (ID3D11Resource *)s_rr_cb, 0, NULL, &rp, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_backend.context, 1, 1, &s_rr_cb);
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_set_ps_override((void *)s_ps_roundrect, SAMP_LINEAR_CLAMP);
    fe_draw_quad(x, y, w, h, 0xFFFFFFFF, s_white_tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
    td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    return 1;
}

/* Procedural selector arrow (◄ ►) via the triangle-SDF shader, crisp + AA at
 * any resolution. dir_right selects the direction; color is 0xAARRGGBB. Reuses
 * the b1 constant buffer. Returns 0 if unavailable (caller falls back). */
static int fe_draw_arrow_proc(float x, float y, float w, float h,
                              int dir_right, uint32_t color) {
    if (!s_ps_arrow || !s_rr_cb || !g_backend.context) return 0;
    FE_RoundRectParams rp;
    memset(&rp, 0, sizeof(rp));
    rp.size_px[0] = w;  rp.size_px[1] = h;
    rp.border[0] = dir_right ? 1.0f : 0.0f;
    td5_argb_to_rgb_f(rp.mid, color);  rp.mid[3] = 1.0f;
    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
        (ID3D11Resource *)s_rr_cb, 0, NULL, &rp, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_backend.context, 1, 1, &s_rr_cb);
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_set_ps_override((void *)s_ps_arrow, SAMP_LINEAR_CLAMP);
    fe_draw_quad(x, y, w, h, 0xFFFFFFFF, s_white_tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
    td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    return 1;
}

static void fe_draw_button_9slice(float bx, float by, float bw, float bh,
                                  int state, uint32_t interior, float sx, float sy) {
    int tex = s_buttonbits_tex_page;
    float yb = (float)(state * 32);

    /* Scaled piece sizes */
    float lw  = (float)BB_LW * sx;
    float rw  = (float)BB_RW * sx;
    float tw  = (float)BB_TILE * sx;
    float th  = (float)BB_TILE * sy;
    float tl_h = 13.0f * sy;
    float tr_h =  9.0f * sy;
    float bl_h =  9.0f * sy;
    float br_h = 13.0f * sy;

    if (bw < lw + rw) return;  /* too narrow for corners */

    /* State 0: fill interior with dark purple, excluding the 4 corner rects.
     * A full-button fill would bleed through the transparent "outside the curve"
     * pixels of each corner texture. The original used color-key blit so the
     * outside pixels were always transparent against the page background. */
    if (state == 0) {
        uint32_t fill = interior;
        fe_draw_quad(bx + lw, by, bw - lw - rw, bh, fill, -1, 0,0,0,0);
        fe_draw_quad(bx, by + tl_h, lw, bh - tl_h - bl_h, fill, -1, 0,0,0,0);
        fe_draw_quad(bx + bw - rw, by + tr_h, rw, bh - tr_h - br_h, fill, -1, 0,0,0,0);
    }

    /* Draw edges FIRST, then corners on top (matches original draw order
     * where BL/BR corners are drawn after edges, overwriting via colorkey). */

    /* --- Horizontal edge tiles (4x4 from tile strip at rows 96-99) --- */
    /* Top: src (state*12+22, 96)-(state*12+26, 100) */
    float te_u0 = (float)(state * 12 + 22) / BB_TEX_W;
    float te_u1 = (float)(state * 12 + 26) / BB_TEX_W;
    float te_v0 = 96.0f / BB_TEX_H;
    float te_v1 = 1.0f; /* 100/100 */
    for (float x = bx + lw; x + rw < bx + bw; x += tw)
        fe_draw_quad(x, by, tw, th, 0xFFFFFFFF, tex, te_u0, te_v0, te_u1, te_v1);

    /* Bottom: src (state*12+28, 96)-(state*12+32, 100) — state-indexed (Ghidra 0x425d37, iVar2+0x1c) */
    float be_u0 = (float)(state * 12 + 28) / BB_TEX_W;
    float be_u1 = (float)(state * 12 + 32) / BB_TEX_W;
    for (float x = bx + lw; x + rw < bx + bw; x += tw)
        fe_draw_quad(x, by + bh - th, tw, th, 0xFFFFFFFF, tex, be_u0, te_v0, be_u1, te_v1);

    /* --- Vertical edge tiles (full-column width, 4px tall) ---
     * Stop before the corner areas: the bottom rows of BL/BR corners have
     * transparent pixels at the outer edge. If edge tiles extend under those
     * transparent pixels, the border color bleeds into the corner, making it
     * appear as a continuous rectangle instead of a rounded corner. */
    /* Left: src (0, yb)-(26, yb+4) */
    float le_v0 = yb / BB_TEX_H;
    float le_v1 = (yb + 4.0f) / BB_TEX_H;
    for (float y = by + tl_h; y < by + bh - bl_h; y += th)
        fe_draw_quad(bx, y, lw, th, 0xFFFFFFFF, tex,
                     0.0f, le_v0, (float)BB_LW / BB_TEX_W, le_v1);

    /* Right: src (28, yb)-(56, yb+4) */
    float re_v0 = yb / BB_TEX_H;
    float re_v1 = (yb + 4.0f) / BB_TEX_H;
    for (float y = by + tr_h; y < by + bh - br_h; y += th)
        fe_draw_quad(bx + bw - rw, y, rw, th, 0xFFFFFFFF, tex,
                     (float)BB_RX / BB_TEX_W, re_v0, 1.0f, re_v1);

    /* --- 4 corners (drawn last to cover edge overlaps) --- */
    /* TL: src (0, yb+6)-(26, yb+19) = 26x13 */
    fe_draw_quad(bx, by, lw, tl_h, 0xFFFFFFFF, tex,
                 0.0f,             (yb + 6.0f) / BB_TEX_H,
                 (float)BB_LW / BB_TEX_W, (yb + 19.0f) / BB_TEX_H);
    /* TR: src (28, yb+6)-(56, yb+15) = 28x9 */
    fe_draw_quad(bx + bw - rw, by, rw, tr_h, 0xFFFFFFFF, tex,
                 (float)BB_RX / BB_TEX_W, (yb + 6.0f) / BB_TEX_H,
                 1.0f,                     (yb + 15.0f) / BB_TEX_H);
    /* BL: src (0, yb+21)-(26, yb+30) = 26x9 */
    fe_draw_quad(bx, by + bh - bl_h, lw, bl_h, 0xFFFFFFFF, tex,
                 0.0f,             (yb + 21.0f) / BB_TEX_H,
                 (float)BB_LW / BB_TEX_W, (yb + 30.0f) / BB_TEX_H);
    /* BR: src (28, yb+17)-(56, yb+30) = 28x13 */
    fe_draw_quad(bx + bw - rw, by + bh - br_h, rw, br_h, 0xFFFFFFFF, tex,
                 (float)BB_RX / BB_TEX_W, (yb + 17.0f) / BB_TEX_H,
                 1.0f,                     (yb + 30.0f) / BB_TEX_H);
}

/* Draw a modern frontend button FRAME (no label/no fill of the caller's content)
 * at the given rect. This is the single source of truth for how every modern
 * frontend button is framed: the VectorUI neon roundrect when VectorUI is enabled
 * and its shader resources loaded, else the ButtonBits 9-slice gold/blue/gray
 * frame. bb_state: 0 = gold/selected, 1 = blue/unselected, 2 = gray/disabled.
 * Used by the main button render loop AND by the text-input widget so the
 * nickname / session-name field matches every other button on screen. The
 * roundrect/9-slice paths manage their own blend presets internally. */
static void fe_draw_button_frame_fill(float bx, float by, float bw, float bh,
                                      int bb_state, uint32_t interior, float sx, float sy) {
    int use_proc = (g_td5.ini.vector_ui && s_ps_roundrect && s_rr_cb);
    if (use_proc) {
        /* Border 3-stop gradient + interior fill, colours per state (matches the
         * button render loop exactly). Selected interior = `interior` (default
         * dark purple 0x392152); unselected/locked have a transparent interior. */
        uint32_t mid_c, inner_c, outer_c;
        if (bb_state == 0)      { mid_c = 0xFFD9CA00u; inner_c = 0xFFA08C00u; outer_c = 0xFF3C2F00u; }
        else if (bb_state == 2) { mid_c = 0xFFAAAAAAu; inner_c = 0xFF777777u; outer_c = 0xFF222222u; }
        else                    { mid_c = 0xFF7995FFu; inner_c = 0xFF496BDCu; outer_c = 0xFF001675u; }
        float fillA = (bb_state == 0) ? 1.0f : 0.0f;
        fe_draw_roundrect(bx, by, bw, bh,
                          20.0f * sy /*large TL/BR*/, 5.0f * sy /*small TR/BL*/,
                          6.0f * sy  /*side border*/, 2.0f * sy /*top/bottom border*/,
                          mid_c, inner_c, outer_c, interior, fillA);
    } else if (s_buttonbits_tex_page >= 0 && s_buttonbits_w > 0 && s_buttonbits_h > 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_button_9slice(bx, by, bw, bh, bb_state, interior, sx, sy);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    }
}

static void fe_draw_button_frame(float bx, float by, float bw, float bh,
                                 int bb_state, float sx, float sy) {
    fe_draw_button_frame_fill(bx, by, bw, bh, bb_state, 0xFF392152u, sx, sy);
}

static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1) {
    if (g_td5.ini.log_frontend_draw) {
        if (!s_fe_draw_log_fp) {
            CreateDirectoryA("log", NULL);
            s_fe_draw_log_fp = fopen("log/frontend_draw_port.csv", "w");
            if (s_fe_draw_log_fp)
                fprintf(s_fe_draw_log_fp, "screen,frame,x,y,w,h,color,page,u0,v0,u1,v1\n");
        }
        if (s_fe_draw_log_fp)
            fprintf(s_fe_draw_log_fp, "%d,%d,%.1f,%.1f,%.1f,%.1f,%08X,%d,%.4f,%.4f,%.4f,%.4f\n",
                    (int)s_current_screen, s_fe_draw_log_frame,
                    x, y, w, h, color, tex_page, u0, v0, u1, v1);
    }
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    memset(verts, 0, sizeof(verts));
    verts[0].screen_x = x;     verts[0].screen_y = y;
    verts[0].tex_u = u0;      verts[0].tex_v = v0;
    verts[1].screen_x = x + w; verts[1].screen_y = y;
    verts[1].tex_u = u1;      verts[1].tex_v = v0;
    verts[2].screen_x = x + w; verts[2].screen_y = y + h;
    verts[2].tex_u = u1;      verts[2].tex_v = v1;
    verts[3].screen_x = x;     verts[3].screen_y = y + h;
    verts[3].tex_u = u0;      verts[3].tex_v = v1;
    for (int v = 0; v < 4; v++) {
        verts[v].depth_z = 0.0f;
        verts[v].rhw = 1.0f;
        verts[v].diffuse = color;
    }
    td5_plat_render_bind_texture(tex_page >= 0 ? tex_page : s_white_tex_page);
    ID3D11DeviceContext_PSSetShader(g_backend.context,
        g_backend.ps_shaders[g_backend.state.texblend_mode == 5 ? 1 : 0], NULL, 0);
    {
        int si = (g_backend.state.mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
        ID3D11DeviceContext_PSSetSamplers(g_backend.context, 0, 1, &g_backend.sampler_states[si]);
    }
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* Same as fe_draw_quad but draws a parallelogram: the top edge is shifted by
 * dx_top and the bottom edge by dx_bot. Used for the faux-italic title text —
 * the glyph texture is sheared across the parallelogram by the sampler. */
static void fe_draw_quad_sheared(float x, float y, float w, float h,
                                 float dx_top, float dx_bot,
                                 uint32_t color, int tex_page,
                                 float u0, float v0, float u1, float v1) {
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    memset(verts, 0, sizeof(verts));
    verts[0].screen_x = x + dx_top;       verts[0].screen_y = y;
    verts[0].tex_u = u0;                  verts[0].tex_v = v0;
    verts[1].screen_x = x + w + dx_top;   verts[1].screen_y = y;
    verts[1].tex_u = u1;                  verts[1].tex_v = v0;
    verts[2].screen_x = x + w + dx_bot;   verts[2].screen_y = y + h;
    verts[2].tex_u = u1;                  verts[2].tex_v = v1;
    verts[3].screen_x = x + dx_bot;       verts[3].screen_y = y + h;
    verts[3].tex_u = u0;                  verts[3].tex_v = v1;
    for (int v = 0; v < 4; v++) {
        verts[v].depth_z = 0.0f;
        verts[v].rhw = 1.0f;
        verts[v].diffuse = color;
    }
    td5_plat_render_bind_texture(tex_page >= 0 ? tex_page : s_white_tex_page);
    ID3D11DeviceContext_PSSetShader(g_backend.context,
        g_backend.ps_shaders[g_backend.state.texblend_mode == 5 ? 1 : 0], NULL, 0);
    {
        int si = (g_backend.state.mag_filter >= 2) ? SAMP_LINEAR_WRAP : SAMP_POINT_WRAP;
        ID3D11DeviceContext_PSSetSamplers(g_backend.context, 0, 1, &g_backend.sampler_states[si]);
    }
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}

/* ========================================================================
 * Public VectorUI surface (td5_vectorui.h)
 *
 * Thin non-static wrappers so the in-race HUD (td5_hud.c) can reuse the exact
 * same resolution-independent primitives the frontend menus use. All the GPU
 * resources (shaders, atlases, constant buffers) are owned by this TU and live
 * for the whole session, so these are safe to call during a race. Each is a
 * no-op / returns 0 when the relevant shader/atlas is unavailable.
 * ======================================================================== */

int td5_vui_text_available(void) {
    return (g_td5.ini.vector_ui && s_msdf_font_page >= 0 && s_ps_msdf != NULL);
}
int td5_vui_shapes_available(void) {
    return (s_ps_roundrect != NULL && s_rr_cb != NULL && s_ps_arrow != NULL);
}
int td5_vui_gauge_available(void) {
    return (s_ps_gauge != NULL && s_gauge_cb != NULL);
}

void td5_vui_text(float x, float y, const char *s, uint32_t color, float sx, float sy) {
    fe_draw_text(x, y, s, color, sx, sy);
}
void td5_vui_text_centered(float cx, float y, const char *s, uint32_t color, float sx, float sy) {
    fe_draw_text_centered(cx, y, s, color, sx, sy);
}
float td5_vui_text_width(const char *s, float sx) {
    /* Public VectorUI helper only carries a single (uniform) scale — pass it as
     * both axes so the 4:3 glyph lock is a no-op here (HUD/VectorUI callers).
     * The frontend menu's own 4:3-locked centering goes through
     * fe_draw_text_centered, not this path. */
    return fe_measure_text_width(s, sx, sx);
}

void td5_vui_quad(float x, float y, float w, float h, uint32_t color, int tex_page,
                  float u0, float v0, float u1, float v1) {
    /* HUD callers expect alpha to blend. fe_draw_quad does NOT set a blend
     * state, so without this the quad inherits whatever preset was last active
     * (often OPAQUE_LINEAR after a gauge draw) and renders fully opaque. Use the
     * HUD translucent preset (alpha_ref=1) so low-alpha fills (the minimap green
     * panel, LED off-segments) blend instead of going solid. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    fe_draw_quad(x, y, w, h, color, tex_page, u0, v0, u1, v1);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* HUD-font / pause-font SDF pages (-1 if unavailable). */
int td5_vui_hudfont_page(void)   { return s_hudfont_sdf_page; }
int td5_vui_pausefont_page(void) { return s_pausefont_sdf_page; }

/* Draw a quad through the MSDF/SDF pixel shader (crisp distance-field glyph).
 * u0..v1 are NORMALISED UVs into `page`. Falls back to a plain textured quad
 * when the MSDF shader is unavailable. */
void td5_vui_msdf_quad(float x, float y, float w, float h, uint32_t color, int page,
                       float u0, float v0, float u1, float v1) {
    if (page < 0 || !s_ps_msdf) {
        fe_draw_quad(x, y, w, h, color, page, u0, v0, u1, v1);
        return;
    }
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_set_ps_override((void *)s_ps_msdf, SAMP_LINEAR_CLAMP);
    fe_draw_quad(x, y, w, h, color, page, u0, v0, u1, v1);
    td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

int td5_vui_roundrect(float x, float y, float w, float h,
                      float r_large, float r_small, float border_side, float border_topbot,
                      uint32_t mid, uint32_t inner, uint32_t outer,
                      uint32_t fill, float fill_alpha) {
    return fe_draw_roundrect(x, y, w, h, r_large, r_small, border_side, border_topbot,
                             mid, inner, outer, fill, fill_alpha);
}

int td5_vui_arrow(float x, float y, float w, float h, int dir_right, uint32_t color) {
    return fe_draw_arrow_proc(x, y, w, h, dir_right, color);
}

void td5_vui_gauge(const TD5_VuiGauge *g) {
    if (!g || !s_ps_gauge || !s_gauge_cb || !g_backend.context) return;

    const float DEG2RAD = 3.14159265358979323846f / 180.0f;
    const float m   = g->radius * 0.06f + 2.0f; /* AA margin + rim headroom (px) */
    float box = (g->radius + m) * 2.0f;
    float x0  = g->cx - g->radius - m;
    float y0  = g->cy - g->radius - m;

    FE_GaugeParams gp;
    memset(&gp, 0, sizeof(gp));
    gp.quad_px[0] = box;  gp.quad_px[1] = box;
    gp.center[0]  = g->radius + m;  gp.center[1] = g->radius + m;
    gp.radius      = g->radius;
    gp.inner_r     = g->inner_radius;
    gp.sweep_start = g->sweep_start_deg * DEG2RAD;
    gp.sweep_end   = g->sweep_end_deg   * DEG2RAD;
    gp.tick_count  = (float)g->tick_count;
    gp.major_every = (float)g->major_every;
    gp.major_len   = g->major_len_px;
    gp.minor_len   = g->minor_len_px;
    gp.tick_out    = g->tick_out;
    if (g->redline_end_deg > g->redline_start_deg) {
        gp.red_start = g->redline_start_deg * DEG2RAD;
        gp.red_end   = g->redline_end_deg   * DEG2RAD;
    } else {
        gp.red_start = 0.0f;  gp.red_end = 0.0f;   /* no red zone */
    }
    gp.pivot_px   = g->pivot_px;
    gp.rim_red_px = g->rim_red_px;
    td5_argb_to_rgba_f(gp.face,  g->face_color);
    td5_argb_to_rgba_f(gp.inner, g->inner_color);
    td5_argb_to_rgba_f(gp.tick,  g->tick_color);
    td5_argb_to_rgba_f(gp.red,   g->redline_color);
    td5_argb_to_rgba_f(gp.pivot, g->pivot_color);

    ID3D11DeviceContext_UpdateSubresource(g_backend.context,
        (ID3D11Resource *)s_gauge_cb, 0, NULL, &gp, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_backend.context, 1, 1, &s_gauge_cb);
    /* HUD translucent preset (alpha_ref=1) so the semi-transparent dial face and
     * the anti-aliased ring/tick/redline edges all blend (the 0x80 cutoff of the
     * plain TRANSLUCENT preset would punch out low-alpha pixels). */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    td5_plat_render_set_ps_override((void *)s_ps_gauge, SAMP_LINEAR_CLAMP);
    fe_draw_quad(x0, y0, box, box, 0xFFFFFFFF, s_white_tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
    td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * frontend_render_legal_copyright_overlay
 *
 * Draws "TEST DRIVE 5 COPYRIGHT 1998" tiled down the screen.
 * Original: DrawFrontendLocalizedStringSecondary @ 0x00424390 (__cdecl),
 *   params (byte *str, uint x, int y). Called in a loop:
 *     x = g_frontendCanvasW / 10  (≈64 at 640px)
 *     y starts at 0x20 (32px), increments by 0x20 (32px) per row
 *     loop condition: row < (g_frontendCanvasH - 0x20) / 0x20
 * [CONFIRMED @ ScreenLegalCopyright 0x004274A0 case 0]
 * [CONFIRMED: string "TEST DRIVE 5 COPYRIGHT 1998" @ 0x00466808 Language.dll]
 * ======================================================================== */
static void frontend_render_legal_copyright_overlay(float sx, float sy) {
    /* Copyright string [CONFIRMED @ 0x00466808] */
    static const char k_copyright[] = "TEST DRIVE 5 COPYRIGHT 1998";
    /* x = canvasW/10 scaled; original uses integer pixel coords on 640px canvas */
    float draw_x = (640.0f / 10.0f) * sx;  /* = 64 * sx [CONFIRMED] */
    float row_h  = 32.0f * sy;              /* 0x20 pixel row height [CONFIRMED] */
    float start_y = 32.0f * sy;             /* y starts at 0x20 = 32 [CONFIRMED] */
    /* Row count [FIXED 2026-06-01 — re-RE'd @0x004274A0]: the orig do-while inits counter=1 and
     * loops while counter < ((canvasH-0x20)>>5)=14, i.e. counter=1..13 => 13 rows at y=32..416.
     * The port previously drew 14 (an extra row at y=448). Faithful count = ((480-32)/32) - 1 = 13. */
    int rows = (int)((480.0f - 32.0f) / 32.0f) - 1;
    for (int r = 0; r < rows; r++) {
        float y = start_y + (float)r * row_h;
        fe_draw_text(draw_x, y, k_copyright, 0xFFFFFFFF, sx, sy);
    }
}

/* ========================================================================
 * frontend_render_cup_failed_overlay
 *
 * Draws the "Sorry / You Failed / To Win / [cup type]" dialog centered
 * on screen. Original renders into a 0x198x0x70 (408x112) DirectDraw
 * surface via DrawFrontendLocalizedStringToSurface x4; port draws live.
 *
 * Dialog position (states 4-5): center = (canvasW/2, canvasH/2),
 *   overlay rect at (center_x - 0xa8, center_y - 0x8f) per:
 *   QueueFrontendOverlayRect(uVar2-0xa8, uVar3-0x8f, ...) [CONFIRMED @ 0x4237F0]
 *   = (320 - 168, 240 - 143) = (152, 97) on 640x480.
 *
 * Text Y offsets within dialog surface [CONFIRMED @ ScreenCupFailedDialog]:
 *   SNK_SorryTxt       y = 0x00  → "SORRY"
 *   SNK_YouFailedTxt   y = 0x1c  → "YOU FAILED"
 *   SNK_ToWinTxt       y = 0x38  → "TO WIN"
 *   SNK_RaceTypeText   y = 0x54  → cup type name (game_type 1-6)
 *
 * Race type names for game_type 1-6 from Language.dll [CONFIRMED]:
 *   1=CHAMPIONSHIP CUP, 2=ERA CUP, 3=CHALLENGE CUP
 *   4=PITBULL CUP,      5=MASTERS CUP, 6=ULTIMATE CUP
 * ======================================================================== */
static const char *k_cup_type_names[7] = {
    "",                  /* 0: unused */
    "CHAMPIONSHIP CUP",  /* 1 [CONFIRMED Language.dll] */
    "ERA CUP",           /* 2 [CONFIRMED Language.dll] */
    "CHALLENGE CUP",     /* 3 [CONFIRMED Language.dll] */
    "PITBULL CUP",       /* 4 [CONFIRMED Language.dll] */
    "MASTERS CUP",       /* 5 [CONFIRMED Language.dll] */
    "ULTIMATE CUP",      /* 6 [CONFIRMED Language.dll] */
};

static void frontend_render_cup_failed_overlay(float sx, float sy) {
    /* Only draw during states 4-5 (dialog visible) [CONFIRMED @ 0x4237F0] */
    if (s_inner_state < 4) return;

    /* Dialog box screen position:
     * dialog_x = screen_cx - 0xa8 = 320 - 168 = 152 [CONFIRMED @ QueueFrontendOverlayRect]
     * dialog_y = screen_cy - 0x8f = 240 - 143 = 97   [CONFIRMED]
     * dialog_w = 0x198 = 408, dialog_h = 0x70 = 112  [CONFIRMED @ CreateTrackedFrontendSurface] */
    float dlg_x = (320.0f - 168.0f) * sx;
    float dlg_y = (240.0f - 143.0f) * sy;
    float dlg_w = 408.0f * sx;
    float dlg_h = 112.0f * sy;
    float dlg_cx = dlg_x + dlg_w * 0.5f;  /* center x of dialog for text centering */
    (void)dlg_h;

    /* [FIXED 2026-06-01] NO dialog background box. The original fills the panel surface
     * black (BltColorFillToSurface(0,...)) then blits it COLOR-KEYED on black: the flag-0
     * trailing arg of QueueFrontendOverlayRect is packed into the source color key (=black)
     * by QueueFrontendSpriteBlit @0x425730, and FlushFrontendSpriteBlits blits via
     * Copy16BitSurfaceRect mode 0x11 (keyed) — so the black fill is TRANSPARENT and only the
     * text shows over the MainMenu backdrop. The prior 0xCC000000 translucent quad was a port
     * invention (same mistake the High Scores/Results overlays correctly avoid). Pipeline
     * traced 0x425660→0x425a30→0x425730→0x425540/0x4251a0. */

    /* Text lines [CONFIRMED string values from Language/English/Language.dll] */
    /* Line 0: "SORRY" at dialog-relative y=0x00 (0px) */
    fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "SORRY",      0xFFFFFFFF, sx, sy);
    /* Line 1: "YOU FAILED" at dialog-relative y=0x1c (28px) */
    fe_draw_text_centered(dlg_cx, dlg_y + 28.0f * sy, "YOU FAILED", 0xFFFFFFFF, sx, sy);
    /* Line 2: "TO WIN" at dialog-relative y=0x38 (56px) */
    fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, "TO WIN",     0xFFFFFFFF, sx, sy);
    /* Line 3: race type name at dialog-relative y=0x54 (84px) */
    if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
        fe_draw_text_centered(dlg_cx, dlg_y + 84.0f * sy,
                              k_cup_type_names[s_selected_game_type], 0xFFFFFFFF, sx, sy);
    }

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * frontend_render_cup_won_overlay
 *
 * Draws "Congrats / You Have Won / [cup type] / [unlock lines]" dialog.
 * Original renders into a 0x198×0xC4 (408×196) surface; port draws live.
 *
 * Dialog position (state 4-5): same offsets as CupFailed:
 *   overlay at (center_x - 0xa8, center_y - 0x8f) [CONFIRMED @ 0x00423D4D/D51]
 *   = (320-168, 240-143) = (152, 97) on 640×480.
 *
 * Text Y offsets [CONFIRMED @ ScreenCupWonDialog 0x00423A80]:
 *   SNK_CongratsTxt     y = 0x00 (0)   — "CONGRATULATIONS"
 *   SNK_YouHaveWonTxt   y = 0x38 (56)  — "YOU HAVE WON"
 *   SNK_RaceTypeText    y = 0x54 (84)  — cup type name
 *   SNK_CarsUnlocked    y = 0x8C (140) — if s_cup_won_car_count != 0  [CONFIRMED @ 0x00423BDD]
 *   SNK_TracksUnlocked  y = 0xA8 (168) — if s_cup_won_track_count != 0 [CONFIRMED @ 0x00423C2D]
 *
 * Dialog height: 0xC4 = 196 (vs CupFailed's 0x70 = 112) [CONFIRMED @ 0x00423AEB/AF0]
 * Unlock string text [RESOLVED 2026-06-01 from Language.dll]: SNK_CongratsTxt="CONGRATULATIONS!",
 *   SNK_YouHaveWonTxt="YOU HAVE WON THE", SNK_CarsUnlocked="CARS UNLOCKED",
 *   SNK_TracksUnlocked="TRACKS UNLOCKED"; unlock lines render "%d CARS/TRACKS UNLOCKED".
 * ======================================================================== */
static void frontend_render_cup_won_overlay(float sx, float sy) {
    char unlock_buf[64];

    /* Only draw during states 4-5 (dialog visible) [CONFIRMED @ 0x00423A80] */
    if (s_inner_state < 4) return;

    /* Dialog box screen position (same offsets as CupFailed) [CONFIRMED @ 0x00423D4D] */
    float dlg_x = (320.0f - 168.0f) * sx;
    float dlg_y = (240.0f - 143.0f) * sy;
    float dlg_w = 408.0f * sx;
    float dlg_h = 196.0f * sy;  /* 0xC4 [CONFIRMED @ 0x00423AEB] */
    float dlg_cx = dlg_x + dlg_w * 0.5f;
    (void)dlg_h;

    /* [FIXED 2026-06-01] NO dialog background box — color-keyed-black transparent panel, text
     * only (see frontend_render_cup_failed_overlay for the full pipeline trace). The prior
     * 0xCC000000 translucent quad was a port invention. */

    /* [FIXED 2026-06-01] byte-exact SNK_ strings: SNK_CongratsTxt="CONGRATULATIONS!",
     * SNK_YouHaveWonTxt="YOU HAVE WON THE" (the cup-type name follows on the next line,
     * e.g. "YOU HAVE WON THE" / "CHAMPIONSHIP"); unlock lines "%d CARS/TRACKS UNLOCKED". */
    fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "CONGRATULATIONS!",  0xFFFFFFFF, sx, sy);
    fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, "YOU HAVE WON THE",  0xFFFFFFFF, sx, sy);
    /* Cup type name at y=0x54=84 [CONFIRMED @ 0x00423B89] */
    if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
        fe_draw_text_centered(dlg_cx, dlg_y + 84.0f * sy,
                              k_cup_type_names[s_selected_game_type], 0xFFFFFFFF, sx, sy);
    }
    /* Car unlock line at y=0x8C=140 [CONFIRMED @ 0x00423BDD] */
    if (s_cup_won_car_count != 0) {
        snprintf(unlock_buf, sizeof(unlock_buf), "%d CARS UNLOCKED", s_cup_won_car_count);
        fe_draw_text_centered(dlg_cx, dlg_y + 140.0f * sy, unlock_buf, 0xFFFFFFFF, sx, sy);
    }
    /* Track unlock line at y=0xA8=168 [CONFIRMED @ 0x00423C2D] */
    if (s_cup_won_track_count != 0) {
        snprintf(unlock_buf, sizeof(unlock_buf), "%d TRACKS UNLOCKED", s_cup_won_track_count);
        fe_draw_text_centered(dlg_cx, dlg_y + 168.0f * sy, unlock_buf, 0xFFFFFFFF, sx, sy);
    }

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * frontend_render_session_locked_overlay
 *
 * Draws "Sorry / Session Locked" dialog. Identical structure to
 * CupFailed but only 2 text lines.
 * [CONFIRMED @ ScreenSessionLockedDialog 0x0041D630]
 * ======================================================================== */
void frontend_render_session_locked_overlay(float sx, float sy) {
    /* Only draw during states 4-5 (dialog visible) [CONFIRMED @ 0x41D630] */
    if (s_inner_state < 4) return;

    float dlg_x = (320.0f - 168.0f) * sx;
    float dlg_y = (240.0f - 143.0f) * sy;
    float dlg_w = 408.0f * sx;
    float dlg_cx = dlg_x + dlg_w * 0.5f;

    /* [FIXED 2026-06-01] NO box — color-keyed-black transparent panel, text only (same
     * pipeline as the cup dialogs). The prior 0xCC000000 translucent quad was a port invention. */

    /* [2026-06-19] This notice dialog doubles as the net-disconnect screen. */
    if (g_net_disconnect_mode) {
        fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "CONNECTION LOST",       0xFFFFFFFF, sx, sy);
        fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, g_net_disconnect_reason, 0xFFC0C0C0, sx, sy);
    } else {
        /* "SORRY" at y=0x00 [CONFIRMED Language.dll: SorryTxt = "SORRY"] */
        fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "SORRY",          0xFFFFFFFF, sx, sy);
        /* "SESSION LOCKED" at y=0x38=56 [CONFIRMED Language.dll: SeshLockedTxt = "SESSION LOCKED"] */
        fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, "SESSION LOCKED", 0xFFFFFFFF, sx, sy);
    }

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* S10b: network lobby overlay — a left roster panel (nickname + latency per
 * joined player) drawn directly (not a navigable button, so it can't be selected
 * or overlap the right-column action buttons), plus the translucent host OPTIONS
 * modal (max players + password) when open. */
static void frontend_render_network_lobby_overlay(float sx, float sy) {
    int slot, row = 0;
    char line[96];
    const char *status;

    /* [S31 redesign] Roster panel rendered as an UNSELECTABLE BUTTON — the
     * same neon rounded-rect frame the menu buttons use (unselected palette,
     * never highlighted) — left-aligned with the NET PLAY title
     * (FE_LOBBY_X == FE_TITLE_LEFT_X). */
    fe_draw_button_frame((float)FE_LOBBY_X * sx, (float)FE_LOBBY_PANEL_Y * sy,
                         (float)FE_LOBBY_PANEL_W * sx, (float)FE_LOBBY_PANEL_H * sy,
                         1 /* unselected */, sx, sy);

    fe_draw_text((FE_LOBBY_X + 18.0f) * sx, (FE_LOBBY_PANEL_Y + 14.0f) * sy,
                 "PLAYERS IN LOBBY", 0xFF00FF00, sx, sy);
    for (slot = 0; slot < TD5_NET_MAX_PLAYERS; slot++) {
        const char *name;
        const char *tag;
        int lat;
        if (!td5_net_is_slot_active(slot)) continue;
        name = td5_net_get_slot_name(slot);
        if (!name[0]) name = "Player";
        tag = (slot == 0) ? " (HOST)" : "";
        lat = td5_net_get_slot_latency_ms(slot);
        if (slot == td5_net_local_slot())
            snprintf(line, sizeof(line), "%d. %s%s (YOU)", slot + 1, name, tag);
        else if (lat >= 0)
            snprintf(line, sizeof(line), "%d. %s%s   %dms", slot + 1, name, tag, lat);
        else
            snprintf(line, sizeof(line), "%d. %s%s   --", slot + 1, name, tag);
        /* [MP NAME INDICATOR 2026-06 — parity] Per-slot accent swatch. Net play
         * does not replicate identity colours (kept out of the deterministic
         * config), so the swatch is the default palette colour keyed by slot. */
        fe_draw_quad((FE_LOBBY_X + 8.0f) * sx,
                     (float)(FE_LOBBY_ROW0_Y + row * FE_LOBBY_ROW_H + 3) * sy,
                     10.0f * sx, 10.0f * sy,
                     0xFF000000u | (k_mp_player_colors[slot % TD5_MAX_HUMAN_PLAYERS] & 0x00FFFFFFu),
                     -1, 0.0f, 0.0f, 1.0f, 1.0f);
        fe_draw_small_text((FE_LOBBY_X + 22.0f) * sx,
                           (float)(FE_LOBBY_ROW0_Y + row * FE_LOBBY_ROW_H + 4) * sy,
                           line, 0xFFFFFFFF, sx, sy);
        /* [S31] Green READY tag (clients toggle it with the READY button). */
        if (slot != 0 && s_slot_ready[slot])
            fe_draw_small_text((FE_LOBBY_X + FE_LOBBY_PANEL_W - 88.0f) * sx,
                               (float)(FE_LOBBY_ROW0_Y + row * FE_LOBBY_ROW_H + 4) * sy,
                               "READY", 0xFF40FF40u, sx, sy);
        row++;
    }
    /* Host/connect status at the bottom of the panel. */
    status = td5_net_get_status_text();
    if (status[0])
        fe_draw_small_text((FE_LOBBY_X + 18.0f) * sx, 290.0f * sy, status,
                           0xFFA8C0E0, sx, sy);
}


/* [S31] HOST GAME setup values, edited IN PLACE: while a field is being
 * edited its value text becomes the live edit buffer with a caret — no
 * separate text-input widget is drawn over the screen. */
static void frontend_render_create_session_overlay(float sx, float sy) {
    char buf[72], mask[34];
    int n, k;
    float bx, by, bw, bh;
    uint32_t c_name = (s_cs_edit == 1) ? 0xFFFFE080u : 0xFFFFFFFFu;
    uint32_t c_pass = (s_cs_edit == 2) ? 0xFFFFE080u : 0xFFFFFFFFu;
    snprintf(buf, sizeof(buf), "%s%s", s_create_session_name,
             (s_cs_edit == 1) ? "_" : "");
    /* [ITEM 1 2026-06-16] Value text rides to the RIGHT of its label button,
     * anchored to the live button row (index 0 = NAME) instead of a hard-coded
     * y. The direct-host setup re-spaces its rows, so a fixed y would detach the
     * value from its row; deriving it from the rect keeps them together and also
     * tracks the slide-in animation. (+6px matches the original 160->166 offset.) */
    if (s_buttons[0].active) {
        frontend_get_button_render_rect(0, sx, sy, &bx, &by, &bw, &bh);
        fe_draw_text(368.0f * sx, by + 6.0f * sy, buf, c_name, sx, sy);
    }
    n = (int)strlen(s_lobby_password);
    if (n > 32) n = 32;
    for (k = 0; k < n; k++) mask[k] = '*';
    mask[n] = '\0';
    if (s_cs_edit == 2)
        snprintf(buf, sizeof(buf), "%s_", mask);
    else
        snprintf(buf, sizeof(buf), "%s", n ? mask : "(OPEN)");
    if (s_buttons[2].active) {
        frontend_get_button_render_rect(2, sx, sy, &bx, &by, &bw, &bh);
        fe_draw_text(368.0f * sx, by + 6.0f * sy, buf, c_pass, sx, sy);
    }

    /* [2026-06-16] GAME PORT inline field (Direct host only, button index 6).
     * Label button on the left, the editable number on the RIGHT (x=368) like
     * NAME/PASSWORD -- the old code only updated the button label with the
     * COMMITTED port, so typing into the field showed nothing. s_cs_port_buf is
     * kept in sync with the committed value (init / commit / cancel), so this
     * shows the live value when idle and the typed digits + caret while editing
     * (s_cs_edit == 3). */
    if (s_buttons[6].active) {
        uint32_t c_port = (s_cs_edit == 3) ? 0xFFFFE080u : 0xFFFFFFFFu;
        frontend_get_button_render_rect(6, sx, sy, &bx, &by, &bw, &bh);
        snprintf(buf, sizeof(buf), "%s%s", s_cs_port_buf,
                 (s_cs_edit == 3) ? "_" : "");
        fe_draw_text(368.0f * sx, by + 6.0f * sy, buf, c_port, sx, sy);
    }
}

/* [S31] MAX PLAYERS selector content — POST-button pass so the centred
 * value + the ◄ ► arrows draw on top of the button frame (same pattern as
 * the car-select nav bar). */
static void frontend_render_create_session_postpass(float sx, float sy) {
    char buf[40];
    float bx, by, bw, bh, tw;
    if (s_current_screen != TD5_SCREEN_CREATE_SESSION) return;
    if (!s_buttons[1].active) return;
    frontend_get_button_render_rect(1, sx, sy, &bx, &by, &bw, &bh);
    snprintf(buf, sizeof(buf), "MAX PLAYERS: %d", s_lobby_max_players);
    tw = fe_measure_text(buf, sx, sy);
    fe_draw_text(bx + (bw - tw) * 0.5f, by, buf, 0xFFFFFFFF, sx, sy);
    fe_draw_option_arrows(1, sx, sy);
}

/* [S31 redesign] Exit-door icons over the lobby's per-row KICK buttons —
 * drawn in the POST-button pass (the slot the old OPTIONS modal used) so
 * they sit on top of the button frames, tracking the slide-in animation. */
static void frontend_render_lobby_kick_icons(float sx, float sy) {
    int k;
    if (s_current_screen != TD5_SCREEN_NETWORK_LOBBY) return;
    for (k = 4; k <= 8 && k < FE_MAX_BUTTONS; k++) {
        float bx, by, bw, bh;
        if (!s_buttons[k].active || s_buttons[k].hidden) continue;
        frontend_get_button_render_rect(k, sx, sy, &bx, &by, &bw, &bh);
        /* exit sign: right-pointing arrow leaving through the door bar */
        td5_vui_arrow(bx + bw * 0.14f, by + bh * 0.28f,
                      bw * 0.42f, bh * 0.44f, 1, 0xFFFFC8C8u);
        fe_draw_quad(bx + bw * 0.64f, by + bh * 0.18f,
                     bw * 0.12f, bh * 0.64f, 0xFFFFC8C8u, -1, 0, 0, 0, 0);
    }
}

void td5_frontend_render_ui_rects(void) {
    int screen_w = 0, screen_h = 0;
    uint32_t now = td5_plat_time_ms();
    td5_plat_get_window_size(&screen_w, &screen_h);
    if (screen_w <= 0 || screen_h <= 0) return;

    float sx = (float)screen_w / 640.0f;
    float sy = (float)screen_h / 480.0f;
    float sw = (float)screen_w;
    float sh = (float)screen_h;

    /* Phase 7 (RE: original main-menu loop, Frida 2026-05-01 1164-frame
     * capture): the original issues zero per-frame fills/clears in steady
     * state. FillPrimaryFrontendRect (0x00423ED0) has only 2 callers and
     * neither is per-frame: InitializeFrontendPresentationState (0x00424E40)
     * runs once at app startup, CarSelectionScreenStateMachine (0x0040DFC0)
     * is unrelated. Screen_MainMenu (0x00415490) does no fill in any state.
     * Skip the clear when a full-canvas BG surface is loaded — the BG quad
     * below paints every pixel opaquely. Keep the clear as a fallback when
     * no full-canvas BG is loaded (otherwise stale pixels would show through). */
    int has_full_bg = 0;
    int bg_slot = -1;
    if (s_background_surface > 0) {
        int slot = s_background_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use &&
            frontend_surface_is_background_like(s_surfaces[slot].width, s_surfaces[slot].height)) {
            has_full_bg = 1;
            bg_slot = slot;
        }
    }
    if (!has_full_bg) {
        td5_plat_render_clear(0xFF101020);
    }
    td5_plat_render_begin_scene();
    td5_plat_render_set_viewport(0, 0, screen_w, screen_h);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    /* Draw background TGA if one is loaded */
    if (has_full_bg) {
        fe_draw_quad(0, 0, sw, sh, 0xFFFFFFFF,
                    s_surfaces[bg_slot].tex_page, 0, 0, 1, 1);
    } else if (s_background_surface > 0) {
        int slot = s_background_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            fe_draw_quad(0, 0, sw, sh, 0xFFFFFFFF,
                        s_surfaces[slot].tex_page, 0, 0, 1, 1);
        }
    }

    /* Background gallery slideshow (UpdateExtrasGalleryDisplay 0x40D830) --
     * drawn before screen-specific overlays and buttons so it stays behind all UI;
     * skip EXTRAS_GALLERY (fills full viewport), CAR_SELECTION (dark bg, overlays bleed),
     * and TRACK_SELECTION (dedicated dark-blue preview panel, slideshow bleeds through). */
    if (s_current_screen != TD5_SCREEN_EXTRAS_GALLERY &&
        s_current_screen != TD5_SCREEN_CAR_SELECTION &&
        s_current_screen != TD5_SCREEN_MP_POSITION &&
        s_current_screen != TD5_SCREEN_TRACK_SELECTION)
        frontend_render_bg_gallery(sx, sy);

    switch (s_current_screen) {
    case TD5_SCREEN_LANGUAGE_SELECT:
        frontend_render_language_select_overlay(sx, sy);
        break;
    case TD5_SCREEN_RACE_TYPE_MENU:
        frontend_render_race_type_description(sx, sy);
        break;
    case TD5_SCREEN_QUICK_RACE:
        frontend_render_quick_race_overlay(sx, sy);
        break;
    case TD5_SCREEN_GAME_OPTIONS:
        frontend_render_game_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_SOUND_OPTIONS:
        frontend_render_sound_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_MUSIC_TEST:
        frontend_render_music_test_overlay(sx, sy);
        break;
    case TD5_SCREEN_DISPLAY_OPTIONS:
        frontend_render_display_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
        frontend_render_two_player_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_MP_LOBBY:
        frontend_render_mp_lobby_overlay(sx, sy);
        break;
    case TD5_SCREEN_MP_POSITION:
        { extern void frontend_mp_position_render2(float sx, float sy);  /* [#6] reworked: de-pulsed + footer hints + empty-cell labels */
          frontend_mp_position_render2(sx, sy); }
        break;
    case TD5_SCREEN_CAR_SELECTION:
        frontend_render_car_selection_preview(sx, sy);
        break;
    case TD5_SCREEN_TRACK_SELECTION:
        frontend_render_track_selection_preview(sx, sy);
        break;
    case TD5_SCREEN_CONTROL_OPTIONS:
        frontend_render_control_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_CONTROLLER_BINDING:
        frontend_render_controller_binding_overlay(sx, sy);
        break;
    case TD5_SCREEN_HIGH_SCORE:
        frontend_render_high_score_overlay(sx, sy);
        break;
    case TD5_SCREEN_NAME_ENTRY:
        if (s_inner_state >= 1 && s_inner_state <= 3) {
            /* Input phase (slide-in / typing / slide-out): draw the faithful gold
             * "ENTER PLAYER NAME" widget (name + green caret). Drawn HERE in the render
             * path so it composites into the presented frame — the handler (case 2) only
             * processes keys. [FIXED 2026-06-01]. */
            frontend_render_text_input();
        } else {
            /* Table phase (cases 5/6-12): the inserted high-score table, same overlay
             * the Records screen uses, pointed at the just-inserted group via
             * s_score_category_index set in Screen_PostRaceNameEntry case 4. */
            frontend_render_high_score_overlay(sx, sy);
        }
        break;
    case TD5_SCREEN_EXTRAS_GALLERY:
        frontend_render_extras_gallery_overlay(sx, sy);
        break;
    /* S10: text-input widgets must be drawn from the render path so they
     * composite into the presented frame (the handler-drawn copy is cleared). */
    case TD5_SCREEN_NET_NICKNAME:
        if (s_text_input_state != 0) frontend_render_text_input();
        break;
    case TD5_SCREEN_CREATE_SESSION:
        /* [S31] values are edited in place — no separate input widget. */
        frontend_render_create_session_overlay(sx, sy);
        break;
    case TD5_SCREEN_DIRECT_CONNECT:
        /* Only during the IP-entry (3) / password-entry (8) sub-states. */
        if ((s_inner_state == 3 || s_inner_state == 8) && s_text_input_state != 0)
            frontend_render_text_input();
        break;
    case TD5_SCREEN_SESSION_PICKER:
        /* [ITEM 4 2026-06-16] LAN join password prompt (inner state 9). Without
         * this case the text-input widget was never composited, so a passworded
         * LAN join captured keystrokes invisibly -- the user saw no field and
         * couldn't tell the password was being typed. Mirrors DIRECT_CONNECT. */
        if (s_inner_state == 9 && s_text_input_state != 0)
            frontend_render_text_input();
        break;
    case TD5_SCREEN_NETWORK_LOBBY:
        frontend_render_network_lobby_overlay(sx, sy);
        break;
    case TD5_SCREEN_RACE_RESULTS: {
        /* [#10] all-players summary table (gated TD5RE_RACE_SUMMARY, default on);
         * falls back to the legacy one-car-at-a-time results overlay when off. */
        extern int  frontend_race_summary_on(void);
        extern void frontend_render_race_summary_overlay(float sx, float sy);
        if (frontend_race_summary_on()) frontend_render_race_summary_overlay(sx, sy);
        else                            frontend_render_race_results_overlay(sx, sy);
        break;
    }
    /* Dialog overlays: text drawn live since port has no offscreen surfaces */
    case TD5_SCREEN_LEGAL_COPYRIGHT:
        /* "TEST DRIVE 5 COPYRIGHT 1998" tiled [CONFIRMED @ 0x004274A0] */
        frontend_render_legal_copyright_overlay(sx, sy);
        break;
    case TD5_SCREEN_CUP_FAILED:
        /* "Sorry / You Failed / To Win / [cup]" dialog [CONFIRMED @ 0x004237F0] */
        frontend_render_cup_failed_overlay(sx, sy);
        break;
    case TD5_SCREEN_CUP_WON:
        /* "Congrats / You Have Won / [cup] / [unlocks]" dialog [CONFIRMED @ 0x00423A80] */
        frontend_render_cup_won_overlay(sx, sy);
        break;
    case TD5_SCREEN_SESSION_LOCKED:
        /* "Sorry / Session Locked" dialog [CONFIRMED @ 0x0041D630] */
        frontend_render_session_locked_overlay(sx, sy);
        break;
    case TD5_SCREEN_POSITIONER_DEBUG:
        /* Dev font-metrics editor [CONFIRMED @ 0x00415030] */
        frontend_render_positioner_overlay(sx, sy);
        break;
    default:
        break;
    }

    /* Draw buttons */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active || s_buttons[i].hidden) continue;

        float bx, by, bw, bh;
        frontend_get_button_render_rect(i, sx, sy, &bx, &by, &bw, &bh);

        int flash_active = (i == s_mouse_flash_button && now <= s_mouse_flash_until);
        /* Use 6-step highlight ramp for smooth color interpolation.
         * ramp 0 = fully deselected, ramp 6 = fully selected. */
        float ramp_t = (float)s_buttons[i].highlight_ramp / 6.0f;
        if (ramp_t < 0.0f) ramp_t = 0.0f;
        if (ramp_t > 1.0f) ramp_t = 1.0f;
        /* Fallback bg color when ButtonBits is not loaded */
        uint32_t bg_color;
        if (s_buttons[i].disabled) {
            bg_color = 0x80333333;
        } else if (flash_active) {
            bg_color = 0xCC2AA844;
        } else {
            uint32_t a = (uint32_t)(0x99 + (0xCC - 0x99) * ramp_t);
            uint32_t r = (uint32_t)(0x28 + (0x50 - 0x28) * ramp_t);
            uint32_t g = (uint32_t)(0x38 + (0x80 - 0x38) * ramp_t);
            uint32_t b = (uint32_t)(0x58 + (0xC0 - 0x58) * ramp_t);
            bg_color = (a << 24) | (r << 16) | (g << 8) | b;
        }

        /* Button background: 9-slice frame from ButtonBits.tga (56x100).
         * Original FUN_00425b60 pre-renders to a cached surface using BltFast
         * with SRCCOLORKEY. We draw live each frame.
         * State 0 = gold/selected, 1 = blue/unselected, 2 = disabled.
         *
         * Phase 3 — Highlight is a discrete frame swap, not a color lerp.
         * The original swaps to the pre-baked highlight half of the button's
         * cached surface the moment focus changes; the 0..6 ramp counter
         * drives the separate green-outline overlay (RenderFrontendDisplay
         * ModeHighlight 0x4263E0), not the gold/blue interior swap.
         * [INFERRED from frontend-rendering-internals.md §6 + Frida capture
         *  showing per-button surface ID is constant; the swap is by surface
         *  half (top/bottom) selected by focus, not by interpolated color.] */
        /* Button frame state: 0 = gold/selected, 1 = blue/unselected, 2 = gray/
         * disabled. (The original swapped pre-baked surface halves by focus; the
         * port frames live each frame.) */
        int bb_state;
        int focused = (i == s_selected_button);
        if (s_buttons[i].disabled)            bb_state = 2;
        else if (focused || flash_active)     bb_state = 0;
        else                                   bb_state = 1;

        /* Procedural neon button frame (VectorUI): crisp glow at any resolution.
         * Draws the dark rounded interior + glowing coloured rim, then the label
         * via the SDF text path. Selectors get only the frame (their value text
         * + arrows are drawn separately below). */
        int use_proc = (g_td5.ini.vector_ui && s_ps_roundrect && s_rr_cb);

        /* [2026-06-16] Under VectorUI (the shipped default) the frame is always the
         * procedural roundrect. The legacy CPU button cache (ButtonBits + BodyText
         * composite) and the per-frame 9-slice bitmap fallback were only reachable
         * with VectorUI OFF, so both were retired: the cache call and its draw
         * branch are gone, and fe_draw_button_frame's internal 9-slice path
         * self-skips on (s_buttonbits_tex_page < 0). The flat-fill `else` remains
         * as the final VectorUI-off fallback (no shader, no bitmap). */
        if (use_proc) {
            /* Neon roundrect frame (gold/blue/gray per state) via the shared
             * fe_draw_button_frame() helper — the text-input field uses the same
             * helper so it matches. */
            fe_draw_button_frame(bx, by, bw, bh, bb_state, sx, sy);
            if (s_buttons[i].label[0] && !s_buttons[i].is_selector) {
                float tw = fe_measure_text(s_buttons[i].label, sx, sy);
                uint32_t tc = s_buttons[i].disabled ? 0xFF888888u : 0xFFFFFFFFu;
                fe_draw_text(bx + (bw - tw) * 0.5f, by, s_buttons[i].label, tc, sx, sy);
            }
        } else if (s_buttonbits_tex_page >= 0 && s_buttonbits_w > 0 && s_buttonbits_h > 0) {
            /* VectorUI off + ButtonBits available: 9-slice frame (alpha-blended,
             * background shows through) via the shared helper. */
            fe_draw_button_frame(bx, by, bw, bh, bb_state, sx, sy);

            if (s_buttons[i].label[0]) {
                float text_w = fe_measure_text(s_buttons[i].label, sx, sy);
                float tx = bx + (bw - text_w) * 0.5f;
                float ty = by;
                uint32_t text_color = 0xFFFFFFFF;
                if (s_buttons[i].disabled) text_color = 0xFF888888;
                fe_draw_text(tx, ty, s_buttons[i].label, text_color, sx, sy);
            }
        } else {
            fe_draw_quad(bx, by, bw, bh, bg_color, -1, 0, 0, 1, 1);
            if (s_buttons[i].label[0]) {
                float text_w = fe_measure_text(s_buttons[i].label, sx, sy);
                float tx = bx + (bw - text_w) * 0.5f;
                float ty = by;
                uint32_t text_color = 0xFFFFFFFF;
                if (s_buttons[i].disabled) text_color = 0xFF888888;
                fe_draw_text(tx, ty, s_buttons[i].label, text_color, sx, sy);
            }
        }

        /* Selection highlight border (RenderFrontendDisplayModeHighlight 0x4263e0).
         * 2px outline, mouse-hover only. Driven by the separate hover index
         * (DAT_00498700), NOT the selection index — hover does not select.
         * [FIXED 2026-06-01, byte-verified] Color = GREEN 0xFF00C500. The original fills
         * with constant 0xC000 via BltColorFillToSurface @0x00424050, which does NOT
         * treat it as a raw RGB565 pixel — it re-packs a BYTE-PER-CHANNEL input, and the
         * 0xC0 byte (input bits 11-15) is routed into the GREEN output channel, packing
         * to pixel 0x0600 = pure green (R=0, G~24/31, B=0). My earlier "0xC000 = dark red"
         * was the raw-565 misread; the user (correctly) sees GREEN. RGBA = 0xFF00C500.
         * Insets inL=20/inR=22/inT=4/inB=6 + 2px bars match the original. */
        if (i == s_mouse_hover_button &&
            !s_buttons[i].disabled) {
            uint32_t gc = 0xFF00C500;
            float inL = 20.0f * sx, inR = 22.0f * sx;
            float inT = 4.0f * sy,  inB = 6.0f * sy;
            float barV = 2.0f * sy, barH = 2.0f * sx;
            fe_draw_quad(bx+inL, by+inT, bw-inL-inR, barV, gc, -1,0,0,1,1);
            fe_draw_quad(bx+inL, by+bh-inB-barV, bw-inL-inR, barV, gc, -1,0,0,1,1);
            fe_draw_quad(bx+inL, by+inT, barH, bh-inT-inB, gc, -1,0,0,1,1);
            fe_draw_quad(bx+bw-inR-barH, by+inT, barH, bh-inT-inB, gc, -1,0,0,1,1);
        }
    }

    /* [item #7 2026-06-15] The car-/track-select randomize chips are painted by
     * frontend_render_carsel_randomize_icon / frontend_render_trksel_randomize_icon
     * (extern-declared with frontend_draw_randomize_icon up by the Quick Race
     * widgets). They are called from inside the CAR_SELECTION / TRACK_SELECTION
     * cases below — AFTER the per-screen button loop above — so the chip composites
     * on TOP of the button frames (original BltFast z-order). Each wrapper self-
     * skips when the control is off / not in icon form / the handle isn't live. */

    /* Option arrows drawn AFTER buttons so they render on top of the button fill.
     * Original BltFast compositing placed arrows on top of the pre-baked button surface. */
    if (s_anim_complete) {
        switch (s_current_screen) {
        case TD5_SCREEN_QUICK_RACE:
            /* Selectors: Car(0), Track(1), Direction(2, hidden on forward-only
             * tracks), Players(3, hidden), Opponents(4), Laps(5).
             * fe_draw_option_arrows self-skips hidden buttons so the Direction
             * and Players ◄► vanish with their rows. */
            for (int i = 0; i <= QR_BTN_LAPS; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_GAME_OPTIONS:
            /* [S02 (c)] Six option rows remain (0..5) after Circuit Laps was
             * removed; OK is index 6 and gets no arrows. */
            for (int i = 0; i <= 5; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_CONTROLLER_BINDING:
            /* Draw the action labels+values on top of the (opaque-when-selected)
             * two-column buttons. [PORT ENHANCEMENT 2026-06] */
            frontend_render_controller_binding_labels(sx, sy);
            break;
        case TD5_SCREEN_DISPLAY_OPTIONS:
            for (int i = 0; i <= 3; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_SOUND_OPTIONS:
            for (int i = 0; i <= 2; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_TWO_PLAYER_OPTIONS:
            /* [PORT ENHANCEMENT 2026-06] Multiplayer Options ◄►: PLAYERS always,
             * CATCHUP always (on/off), SPLIT LAYOUT only when >1 layout exists,
             * plus each DISPLAY row. */
            if (s_mp_btn_players >= 0) fe_draw_option_arrows(s_mp_btn_players, sx, sy);
            if (s_mp_btn_catchup >= 0) fe_draw_option_arrows(s_mp_btn_catchup, sx, sy);
            if (s_mp_btn_layout >= 0 && s_mp_layout_optcount > 1)
                fe_draw_option_arrows(s_mp_btn_layout, sx, sy);
            for (int i = 0; i < s_mp_missing_count && i < 2; i++)
                if (s_mp_btn_missing[i] >= 0) fe_draw_option_arrows(s_mp_btn_missing[i], sx, sy);
            break;
        case TD5_SCREEN_CAR_SELECTION:
            /* orig CarSelectionScreenStateMachine @0x0040DFC0 case 4 calls
             * InitializeFrontendDisplayModeArrows only for Car(0) and Paint(1)
             * — those two get button.flags|=2 (the ◄► sprites). The Stats/Config
             * button (slot 2) gets NO arrow call, so it must NOT draw ◄►.
             * [CONFIRMED @ 0x00426260 + 0x0040DFC0 case-4 call sites — 2026-06-02
             * RE corrected the 2026-06-01 assumption that slot 2 also carried
             * arrows; the original cycles slot 2's wheel/config scheme on key
             * press but never paints arrow glyphs over the stat panel.] */
            fe_draw_option_arrows(0, sx, sy);
            /* PAINT row: TD5 cars cycle 4 paint schemes (◄► arrows); ported TD6
             * cars pick a body COLOR instead — no arrows; the PAINT button toggles
             * the color-swatch panel (drawn last so it overlays the preview). */
            if (!frontend_car_is_td6(frontend_current_car_index()))
                fe_draw_option_arrows(1, sx, sy);
            frontend_render_td6_color_panel(sx, sy);
            /* [item #7] Randomize chip to the right of the Car selector. */
            frontend_render_carsel_randomize_icon(sx, sy);
            break;
        case TD5_SCREEN_TRACK_SELECTION:
            /* Track(0) selector + the race-option rows (AI/laps/traffic/police/
             * difficulty = buttons 2..6; difficulty self-skips when hidden in
             * Quick Race context). [PORT ENHANCEMENT 2026-06] */
            fe_draw_option_arrows(0, sx, sy);
            fe_draw_option_arrows(2, sx, sy);
            fe_draw_option_arrows(3, sx, sy);
            fe_draw_option_arrows(4, sx, sy);
            fe_draw_option_arrows(5, sx, sy);
            fe_draw_option_arrows(6, sx, sy);   /* per-race difficulty (master) */
            /* [item #7] Randomize chip to the right of the Track selector. */
            frontend_render_trksel_randomize_icon(sx, sy);
            break;
        case TD5_SCREEN_CONTROL_OPTIONS:
            /* [PORT ENHANCEMENT 2026-06] arrows on PLAYER(0) + CONTROLLER SELECTION(1). */
            fe_draw_option_arrows(0, sx, sy);
            fe_draw_option_arrows(1, sx, sy);
            break;
        case TD5_SCREEN_MUSIC_TEST:
            /* orig 0x418460: InitializeFrontendDisplayModeArrows(0,1) — the TRACK
             * selector (button 0) is ◄►-cyclable. This screen was missing from the
             * arrow-render dispatch (creation≠rendering gap; see
             * re/analysis/frontend_diff_blindspot_postmortem.md). */
            fe_draw_option_arrows(0, sx, sy);
            break;
        default:
            break;
        }
    }

    /* S10b: the lobby OPTIONS modal renders here, AFTER the buttons + arrows, so
     * it covers the whole lobby (the per-screen overlay above runs BEFORE the
     * buttons, which would otherwise paint over the modal). */
    if (s_current_screen == TD5_SCREEN_NETWORK_LOBBY)
        frontend_render_lobby_kick_icons(sx, sy);
    if (s_current_screen == TD5_SCREEN_CREATE_SESSION)
        frontend_render_create_session_postpass(sx, sy);

    /* Nav bar text drawn after buttons so it renders on top of the button frame.
     * (button 0 is the nav bar: button loop draws the 9-slice frame, then we
     * draw the track name and arrows on top.) */
    if (s_current_screen == TD5_SCREEN_HIGH_SCORE && s_anim_complete && s_inner_state >= 6) {
        char track_name[80];
        frontend_get_track_display_name(s_score_category_index, 0, track_name, sizeof(track_name));
        float nav_bx, nav_by, nav_bw, nav_bh;
        frontend_get_button_render_rect(0, sx, sy, &nav_bx, &nav_by, &nav_bw, &nav_bh);
        /* [FIXED 2026-06-01 v3] Track-name text: button-centered horizontally (faithful —
         * RebuildFrontendButtonSurface(0,0,track) MeasureOrCenter's the label across the 520px
         * button width); vertically at button-local y=0. The original draws the label via
         * DrawFrontendLocalizedStringToSurface(text, x, y=0, surface) into the button surface,
         * so the BodyText cell top sits at the button top and the glyph caps land centered in
         * the 32px bar (de-stretched orig caps native [103..118] in button [97..129]). The
         * prior +6 pushed the 24px cell's caps into the lower half → text bottom-aligned,
         * below the (correctly centered) ◄► arrows. Drop the +6 to match the original. */
        float tnw = fe_measure_text(track_name, sx, sy);
        float tx = nav_bx + (nav_bw - tnw) * 0.5f;
        float ty = nav_by;
        fe_draw_text(tx, ty, track_name, 0xFFFFFFFF, sx, sy);
        fe_draw_option_arrows(0, sx, sy);
    }

    /* [PORT ENHANCEMENT 2026-06-07] Post-name-entry High Scores table (NAME_ENTRY
     * [25]) nav bar: draw the just-raced track's (or cup's) name centered on
     * button 0 so the bar isn't an empty box (user-requested). STATIC — unlike the
     * Records screen [23] above, NO ◄► arrows and no L/R browsing: this screen
     * shows only the single group the player just scored in (s_score_category_index,
     * set to the inserted group in Screen_PostRaceNameEntry case 4). The original
     * left this bar empty (NULL caption, RebuildFrontendButtonSurface @0x00426120
     * draws no text); this is a readability enhancement beyond original parity. */
    if (s_current_screen == TD5_SCREEN_NAME_ENTRY && s_anim_complete && s_inner_state >= 6) {
        char track_name[80];
        frontend_get_track_display_name(s_score_category_index, 0, track_name, sizeof(track_name));
        float nav_bx, nav_by, nav_bw, nav_bh;
        frontend_get_button_render_rect(0, sx, sy, &nav_bx, &nav_by, &nav_bw, &nav_bh);
        float tnw = fe_measure_text(track_name, sx, sy);
        float tx = nav_bx + (nav_bw - tnw) * 0.5f;
        float ty = nav_by;
        fe_draw_text(tx, ty, track_name, 0xFFFFFFFF, sx, sy);
        /* deliberately NO fe_draw_option_arrows — static, non-browsable. */
    }

    /* Race Results selector nav bar (button 0): the FOCUS racer's car name centered on
     * the bar plus ◄► arrows to browse racers — same widget as the High Scores nav bar.
     * The bar tracks the same slot as the stat values (s_score_category_index, raw actor
     * slot: 0 = player → s_selected_car, 1..5 = opponents → g_td5.ai_car_indices[slot]).
     * Only during the results stat screen (states 3..0xB); the menu (0xD+) reuses btn0. */
    if (s_current_screen == TD5_SCREEN_RACE_RESULTS && s_anim_complete &&
        s_inner_state >= 3 && s_inner_state <= 0x0B) {
        int fslot = s_score_category_index;
        if (fslot < 0 || fslot >= TD5_MAX_RACER_SLOTS) fslot = 0;
        int car = (fslot <= 0) ? s_selected_car : g_td5.ai_car_indices[fslot];
        const char *car_name = frontend_get_car_display_name(car);
        float nav_bx, nav_by, nav_bw, nav_bh;
        frontend_get_button_render_rect(0, sx, sy, &nav_bx, &nav_by, &nav_bw, &nav_bh);
        float cnw = fe_measure_text(car_name, sx, sy);
        fe_draw_text(nav_bx + (nav_bw - cnw) * 0.5f, nav_by, car_name, 0xFFFFFFFF, sx, sy);
        fe_draw_option_arrows(0, sx, sy);
    }

    /* (text overlay rendering deferred to font system) */

    /* P1 — title strip gating for Screen [24] RaceResults.
     * [CONFIRMED @ 0x00422480 states 3..0xB queue title; states 0xD..0x14 do not]
     * Original only queues the title surface during the table-browse sub-flow
     * (states 3..0xB). Once state 0xC releases the surface and 0xD builds the
     * post-race menu, the title disappears. Other screens render the strip
     * for their entire lifetime, so the suppression is screen-24-specific. */
    int title_visible = 1;
    if (s_current_screen == TD5_SCREEN_RACE_RESULTS) {
        title_visible = (s_inner_state >= 3 && s_inner_state <= 0x0B) ? 1 : 0;
    }
    /* The simultaneous grid car-select draws its own per-pane headers (the
     * SELECT CAR / PROFILE SELECTION grids draw their own title inline). A few
     * screens that stay live WHILE s_mp_simul is set rely on the GLOBAL title
     * path, so exempt them:
     *   - NETWORK_LOBBY: a post-race return must not inherit a stale s_mp_simul
     *     that blanks its "NET PLAY" header [#24];
     *   - MP_LOBBY (30): the LOCAL split-screen lobby is a DISTINCT screen from
     *     NETWORK_LOBBY (11); the post-LOCAL-MP-race return lands here with
     *     s_mp_simul still set, which blanked its "MULTIPLAYER" header — this was
     *     the screen the prior NETWORK_LOBBY-only fix missed [R9 2026-06-19];
     *   - TRACK_SELECTION: the MP flow keeps s_mp_simul set across track-select
     *     (so backing out re-enters the car grid), which otherwise suppressed the
     *     shared "SELECT TRACK" header [#20]. */
    if (s_mp_simul &&
        s_current_screen != TD5_SCREEN_NETWORK_LOBBY &&
        s_current_screen != TD5_SCREEN_MP_LOBBY &&
        s_current_screen != TD5_SCREEN_TRACK_SELECTION)
        title_visible = 0;

    if (title_visible) {
        const char *title_text = frontend_get_title_text_for_screen(s_current_screen);
        if (title_text && td5_titlefont_ready()) {
            /* [title font] render the screen header as text in the Lunatica
             * title face, replacing the legacy baked title-strip art. LEFT-ALIGNED
             * at a fixed x on every screen so the first letter always starts in
             * the same place. The slide-in Y reuses the legacy strip animation so
             * the header still slides in from above (rest 21 -> 17).
             * [2026-06-16] The legacy baked title-strip image/MSDF fallback
             * (frontend_ensure_title_texture + s_title_tex_page[]) was retired;
             * title.ttf always ships, so the TTF path is the only one. */
            float t_y = frontend_get_title_render_y(sy) - 4.0f * sy;
            frontend_draw_screen_title(title_text, FE_TITLE_LEFT_X * sx, t_y,
                                       0xFFE3D708u, sx, sy);
        }
    }       /* end if (title_visible) */

    /* Draw queued colored rects (with alpha blending for fade overlays) */
    for (int i = 0; i < s_draw_queue_count; i++) {
        FE_DrawCmd *cmd = &s_draw_queue[i];
        if (cmd->type != FE_CMD_RECT) continue;
        uint32_t alpha = (cmd->color >> 24) & 0xFF;
        if (alpha < 255 && alpha > 0) {
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        }
        fe_draw_quad((float)cmd->x * sx, (float)cmd->y * sy,
                     (float)cmd->w * sx, (float)cmd->h * sy,
                     cmd->color, -1, 0, 0, 1, 1);
        if (alpha < 255 && alpha > 0) {
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        }
    }
}

void td5_frontend_flush_sprite_blits(void) {
    int screen_w = 0, screen_h = 0;
    td5_plat_get_window_size(&screen_w, &screen_h);
    if (screen_w <= 0 || screen_h <= 0) goto done;

    float sx = (float)screen_w / 640.0f;
    float sy = (float)screen_h / 480.0f;

    /* Draw queued blits (textured quads) */
    for (int i = 0; i < s_draw_queue_count; i++) {
        FE_DrawCmd *cmd = &s_draw_queue[i];
        if (cmd->type != FE_CMD_BLIT || cmd->tex_page < 0) continue;

        float bx = (float)cmd->x * sx;
        float by = (float)cmd->y * sy;
        float bw = (float)cmd->w * sx;
        float bh = (float)cmd->h * sy;

        TD5_D3DVertex verts[4];
        uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        memset(verts, 0, sizeof(verts));

        verts[0].screen_x = bx;      verts[0].screen_y = by;
        verts[0].tex_u = cmd->u0;    verts[0].tex_v = cmd->v0;
        verts[1].screen_x = bx + bw; verts[1].screen_y = by;
        verts[1].tex_u = cmd->u1;    verts[1].tex_v = cmd->v0;
        verts[2].screen_x = bx + bw; verts[2].screen_y = by + bh;
        verts[2].tex_u = cmd->u1;    verts[2].tex_v = cmd->v1;
        verts[3].screen_x = bx;      verts[3].screen_y = by + bh;
        verts[3].tex_u = cmd->u0;    verts[3].tex_v = cmd->v1;

        for (int v = 0; v < 4; v++) {
            verts[v].depth_z = 0.0f;
            verts[v].rhw = 1.0f;
            verts[v].diffuse = cmd->color;
        }

        /* Use translucent mode for color-keyed textures (cursor, mainfont,
         * font, ButtonBits -- all loaded with red color key -> alpha=0). */
        if (cmd->tex_page == s_cursor_tex_page ||
            cmd->tex_page == s_font_page ||
            cmd->tex_page == s_buttonbits_tex_page ||
            cmd->tex_page == s_buttonlights_tex_page)
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        else
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(cmd->tex_page);
        td5_plat_render_draw_tris(verts, 4, indices, 6);
    }

done:
    /* Clear draw queue for next frame */
    s_draw_queue_count = 0;

    /* FPS/MS counter at the top-RIGHT corner. [S12 2026-06-05] Moved from the
     * old top-left (8,8) so it sits out of the way of the title/menu content and
     * matches the in-race HUD's top-right readout. Anchored to the right edge:
     * x = screen_w - measured text width - an 8px (scaled) gutter mirroring the
     * old left inset. fe_measure_text_width returns scaled (screen) px, same
     * space as screen_w. Drawn last so it overlays everything. [S01 2026-06-04]
     * gated by the Display-options Show FPS toggle (g_td5.ini.show_fps). */
    if (g_td5.ini.show_fps && screen_w > 0) {
        char fps_buf[48];
        snprintf(fps_buf, sizeof(fps_buf), "FPS %.0f  %dMS",
                 (double)g_td5_display_fps, g_td5_peak_frame_ms);
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        float fps_w = fe_measure_text_width(fps_buf, sx, sy);
        float fps_x = (float)screen_w - fps_w - 8.0f * sx;
        if (fps_x < 0.0f) fps_x = 0.0f;
        { static int s_fps_logged_w = -1;   /* one-shot per width; re-logs on resize */
          if (screen_w != s_fps_logged_w) {
              s_fps_logged_w = screen_w;
              TD5_LOG_I(LOG_TAG, "menu FPS overlay top-right: screen_w=%d text_w=%.1f x=%.1f",
                        screen_w, (double)fps_w, (double)fps_x);
          } }
        fe_draw_text(fps_x, 8.0f * sy, fps_buf, 0xFFFFFF00u, sx, sy);
    }

    /* End scene + present */
    td5_plat_render_end_scene();
}

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_frontend_init(void) {
    TD5_LOG_I(LOG_TAG, "td5_frontend_init");

    s_current_screen = TD5_SCREEN_STARTUP_INIT;
    s_inner_state = 0;
    s_anim_tick = 0;
    s_return_screen = -1;
    s_previous_screen = (TD5_ScreenIndex)-1;
    s_start_race_request = 0;
    s_start_race_confirm = 0;
    s_attract_idle_counter = 0;
    s_attract_idle_timestamp = td5_plat_time_ms();
    s_attract_demo_active = 0;
    s_flow_context = 0;
    s_selected_game_type = -1;
    s_race_within_series = 0;
    s_cup_unlock_tier = 0;
    s_two_player_mode = 0;
    s_mp_flow = 0;
    s_mp_simul = 0;
    s_mp_joined_count = 0;
    s_mp_car_player = 0;
    s_mp_layout_sel = 0;
    s_mp_missing_content[0] = 0;
    s_mp_missing_content[1] = 0;
    s_attract_mode_ctrl = 0;
    s_selected_car = g_td5.ini.loaded ? g_td5.ini.default_car : 0;
    s_selected_paint = 0;
    s_selected_config = 0;
    s_color_panel_visible = 0;   /* TD6 color panel starts closed */
    /* s_paint_active persists across car-select entries (e.g. returning from a
     * race) so a chosen colour stays applied; it starts 0 (neutral) only at
     * launch and is set when the player first confirms a paint colour. */
    s_selected_transmission = 0;
    s_selected_track = g_td5.ini.loaded ? g_td5.ini.default_track : 0;
    s_track_direction = 0;
    s_network_active = 0;
    s_kicked_flag = 0;
    s_lobby_action = 0;
    s_prev_left_state = 0;
    s_prev_right_state = 0;
    s_prev_enter_state = 0;
    s_prev_up_state = 0;
    s_prev_down_state = 0;
    s_prev_escape_state = 0;
    s_fade_active = 0;
    s_results_rerace_flag = 0;
    s_results_cup_complete = 0;
    s_cheat_unlock_all = 0;

    /* Apply INI defaults (override compile-time defaults if INI was loaded) */
    if (g_td5.ini.loaded) {
        s_display_fog_enabled       = g_td5.ini.fog_enabled;
        td5_render_set_fog(g_td5.ini.fog_enabled);
        /* [S01 2026-06-04] mirror the new Display-options rows. */
        s_display_window_mode       = g_td5.ini.window_mode;
        s_display_vsync             = g_td5.ini.vsync;
        s_display_show_fps          = g_td5.ini.show_fps;
        s_display_speed_units       = g_td5.ini.speed_units;
        td5_save_set_speed_units(g_td5.ini.speed_units);
        s_display_camera_damping    = g_td5.ini.camera_damping;
        td5_save_set_camera_damping(g_td5.ini.camera_damping);
        td5_save_set_display_mode(g_td5.ini.display_mode);
        s_sound_option_sfx_volume   = g_td5.ini.sfx_volume;
        s_sound_option_music_volume = g_td5.ini.music_volume;
        s_sound_option_sfx_mode     = g_td5.ini.sfx_mode;
        td5_save_set_sound_mode(g_td5.ini.sfx_mode);
        td5_save_set_sfx_volume(s_sound_option_sfx_volume);
        td5_save_set_music_volume(s_sound_option_music_volume);
        td5_sound_set_sfx_volume(s_sound_option_sfx_volume);
        td5_sound_set_music_volume(s_sound_option_music_volume);
        td5_physics_set_collisions(g_td5.ini.collisions);
        s_game_option_laps              = g_td5.ini.laps;
        s_game_option_checkpoint_timers = g_td5.ini.checkpoint_timers;
        s_game_option_traffic           = g_td5.ini.traffic;
        s_game_option_cops              = g_td5.ini.cops;
        s_game_option_difficulty        = g_td5.ini.difficulty;
        s_game_option_dynamics          = g_td5.ini.dynamics;
        td5_physics_set_dynamics(g_td5.ini.dynamics);
        s_game_option_collisions        = g_td5.ini.collisions;
        s_selected_game_type = g_td5.ini.default_game_type;
    }

    g_td5.frontend_screen_index = TD5_SCREEN_STARTUP_INIT;
    g_td5.frontend_inner_state = 0;

    return 1;
}

void td5_frontend_shutdown(void) {
    td5_frontend_release_resources();
    TD5_LOG_I(LOG_TAG, "td5_frontend_shutdown");
}

void td5_frontend_tick(void) {
    /* Called from td5_game when game_state == MENU */
    if (s_logged_screen != s_current_screen || s_logged_inner_state != s_inner_state) {
        TD5_LOG_D(LOG_TAG, "Tick transition: screen=%d inner_state=%d",
                  (int)s_current_screen, s_inner_state);
        s_logged_screen = s_current_screen;
        s_logged_inner_state = s_inner_state;
    }
    td5_frontend_display_loop();
}

/* ########################################################################
 * SCREEN IMPLEMENTATIONS
 * ########################################################################
 *
 * Each screen is a state machine controlled by s_inner_state.
 * s_anim_tick counts frames within a state for animations.
 * td5_frontend_set_screen() resets both to 0 on navigation.
 * ######################################################################## */

/* ========================================================================
 * [0] ScreenLocalizationInit (0x4269D0) -- Bootstrap/localization
 * States: 1 (single-pass, state 0 only)
 * ======================================================================== */


/* ========================================================================
 * [1] ScreenPositionerDebugTool (0x415030) -- Dev debug, unreachable
 * States: 6
 * ======================================================================== */


/* ========================================================================
 * [2] RunAttractModeDemoScreen (0x4275A0) -- Attract mode / demo
 * States: 6
 *
 * [ARCH-DIVERGENCE: DDraw blit primitives -> D3D11 frontend helpers; L5 sweep 2026-05-21]
 *   Byte-faithful FSM: 6 inner states (0..5) mapped case-for-case to orig
 *   0x004275A0. Per-case bridges:
 *     case 0: g_attractModeDemoActive=1 + Present + ActivateCursor
 *             -> s_attract_demo_active=1 + frontend_present_buffer + cursor_visible(0)
 *     case 1: ReleaseFrontendDisplayModeButtons -> frontend_reset_buttons
 *     case 2/3: PresentPrimaryFrontendBufferViaCopy -> frontend_present_buffer (x2)
 *     case 4: InitFrontendFadeColor(0) -> frontend_init_fade(0)
 *     case 5: RenderFrontendFadeEffect + on complete: InitializeRaceSeriesSchedule
 *             + InitializeFrontendDisplayModeState -> frontend_render_fade +
 *             frontend_init_race_schedule + frontend_init_display_mode_state
 * ======================================================================== */


/* ========================================================================
 * [3] ScreenLanguageSelect (0x427290)
 * States: 7
 * ======================================================================== */


/* ========================================================================
 * [4] ScreenLegalCopyright (0x4274A0) -- 3-second copyright splash
 * States: 4
 * ======================================================================== */



/* ========================================================================
 * [5] ScreenMainMenuAnd1PRaceFlow (0x415490)
 * States: 24 (0x00 - 0x17)
 *
 * 7 buttons: Race Menu, Quick Race, Two Player, Net Play, Options,
 *            Hi-Score, Exit
 * ======================================================================== */


/* ========================================================================
 * [6] RaceTypeCategoryMenuStateMachine (0x4168B0)
 * States: ~21 (0x00 - 0x14)
 * Top-level: 7 buttons (Single Race, Cup Race, Continue Cup,
 *            Time Trials, Drag Race, Cop Chase, Back)
 * Cup sub-menu: 7 buttons (Championship..Ultimate, Back)
 *
 * [ARCH-DIVERGENCE: TD5_GameType enum value remap; L5 sweep 2026-05-22]
 *   Orig 0x004168B0 case-3 button-press handler:
 *     button 0 -> g_selectedGameType = 0  (Single Race)
 *     button 3 -> g_selectedGameType = 9  (Time Trials; override of default i+3=6)
 *     button 4 -> g_selectedGameType = 7  (Drag Race; default i+3=7)
 *     button 5 -> g_selectedGameType = 8  (Cop Chase; default i+3=8)
 *   In orig the enum is: 7=Drag, 8=CopChase, 9=TimeTrial.
 *
 *   Port reassigns the enum values for sequential semantic naming
 *   (td5_types.h:178-190): 7=TIME_TRIAL, 8=COP_CHASE, 9=DRAG_RACE.
 *   Port's button mapping then becomes:
 *     button 3 (Time Trials button) -> s_selected_game_type = 7 (port's TIME_TRIAL)
 *     button 4 (Drag Race button)   -> s_selected_game_type = 9 (port's DRAG_RACE)
 *     button 5 (Cop Chase button)   -> s_selected_game_type = 8 (port's COP_CHASE)
 *   The button-label → semantic-action contract is preserved; only the
 *   underlying numeric encoding shifts. All port game_type consumers
 *   (td5_game.c game-mode dispatch, td5_hud.c overlays, td5_frontend.c
 *   selection flows) reference TD5_GAMETYPE_* enum names, not raw integers,
 *   so the value remap is internally consistent. REG-1 verdict 2026-05-22:
 *   false alarm — flagged as a swap but is a deliberate enum-value
 *   remapping with full consistency in port code. */


/* ========================================================================
 * [7] ScreenQuickRaceMenu (0x4213D0)  [PORT ENHANCEMENT — diverges from orig]
 * States: 7 (0x00 - 0x06)
 *
 * Original buttons: Change Car, Change Track, OK, Back (no direction toggle,
 * drag strip reachable). The port's improved Quick Race screen adds:
 *   - a Forwards/Backwards direction toggle (reuses TrackSelection's reverse
 *     gating: hidden on forward-only/circuit tracks),
 *   - Drag Strip removed from the track cycler (schedule index 19),
 *   - Players (1..6 human) + Opponents (0..5 AI) selectors, sum <= 6 racers.
 * This is the infrastructure that will later replace the separate Two Player
 * menu + Two Player configuration screen. >2-way split rendering is deferred
 * (engine has only single + 2 split layouts), so the launch path caps the
 * EFFECTIVE human-driven slots at 2; see frontend_init_race_schedule.
 * ======================================================================== */




/* ========================================================================
 * [8] RunFrontendConnectionBrowser (0x418D50) -- Network connection type
 * States: ~10
 * ======================================================================== */







/* ========================================================================
 * [9] RunFrontendSessionPicker (0x419CF0) -- Session browser
 * States: ~8
 * ======================================================================== */



/* ========================================================================
 * [10] RunFrontendCreateSessionFlow (0x41A7B0) -- Session creation
 * States: ~18
 * ======================================================================== */


/* ========================================================================
 * [11] RunFrontendNetworkLobby (0x41C330) -- 18-state multiplayer lobby
 * States: 18 (0x00 - 0x11)
 *
 * [ARCH-DIVERGENCE: DXPTYPE] The lobby's network paths (DXPCHAT submit,
 * LOBBY_KICK/REQUEST_CONFIG/SETTINGS broadcast, DXPSTART rendezvous) all
 * use the DXPTYPE wire format that's incompatible with TD5_d3d.exe peers
 * per the file-footer manifest. The full 18-state FSM is ported for code
 * structural fidelity but cannot complete an actual session against orig
 * binaries. Reachable from the menu; not field-tested end-to-end.
 * ======================================================================== */


/* ========================================================================
 * [12] ScreenOptionsHub (0x41D890) -- Options category selection
 * States: 10
 * 6 buttons: Game, Control, Sound, Graphics, Two Player, OK
 * ======================================================================== */


/* ========================================================================
 * [13] ScreenGameOptions (0x41F990)
 * Standard options screen pattern (10 states).
 * 7 toggle rows + OK button.
 * ======================================================================== */


/* ========================================================================
 * [14] ScreenControlOptions (0x41DF20) -- revamped (PORT ENHANCEMENT 2026-06)
 *
 * The original had two fixed PLAYER rows (P1/P2), each a device-source ◄►
 * selector + a CONFIGURE button. Rebuilt into:
 *   row 0  PLAYER               — 1..K selector (K = connected joysticks,
 *                                 hot-swapped on entry; never < declared humans)
 *   row 1  CONTROLLER SELECTION — device for the selected player (Keyboard / Joy N);
 *                                 Keyboard is always present + the only shareable
 *                                 device; joysticks are exclusive across players
 *   row 2  CONFIGURE            — opens the per-button remap screen for that player
 *   row 3  OK
 * ======================================================================== */




/* ========================================================================
 * [15] ScreenSoundOptions (0x41EA90) -- Standard options pattern
 * [PORT REWORK 2026-06-05 / S15] The SFX Mode row was removed per user
 * feedback. 3 rows remain: SFX Volume, Music Volume, Music Test + OK.
 * Buttons re-indexed 0..3 (was 0..4) and reflowed up one slot to fill the gap.
 * ======================================================================== */


/* ========================================================================
 * [16] ScreenDisplayOptions (0x420400)
 * States: 9. Rows: Resolution, Fogging, Speed Readout, Camera Damping, OK
 * ======================================================================== */


/* ========================================================================
 * [17] Multiplayer Options  (was ScreenTwoPlayerOptions, orig 0x420C70)
 *
 * [PORT ENHANCEMENT 2026-06] Rebuilt from the original 2-row screen (Split
 * Screen on/off + CATCHUP) into a dynamic split-screen layout picker:
 *   row 0  PLAYERS        — 1..9 (◄►), shared with Quick Race (s_num_human_players)
 *   row 1  SPLIT LAYOUT   — per-count layout (◄► when >1 option, else fixed label)
 *   row 2+ DISPLAY k      — content for each empty grid cell (deferred; stub list)
 *   last   OK
 * CATCHUP was removed (it set the 2P human steering rubber-band swing, which is
 * already inert in the port). The row set changes with the player count, so the
 * button table is rebuilt via mp_build_buttons() whenever PLAYERS/LAYOUT change.
 * ======================================================================== */



/* ========================================================================
 * [18] Controller Binding / Configure  (was ScreenControllerBindingPage, 0x40FE00)
 *
 * [PORT ENHANCEMENT 2026-06] Rebuilt from the original's sequential capture
 * (joystick: press each button in turn to cycle its slot; keyboard: forced
 * "press the key for LEFT, now RIGHT, …" walk) into a navigable per-button
 * remap: the whole action→binding mapping is shown as a list of buttons; the
 * user selects ONE action and confirms to (re)bind just that one.
 *
 * States: 0 init (device detect → build row buttons), 9 slide-in, 10 list +
 * per-action capture, 11 slide-out → Control Options.
 *
 * Data (carried over from the original RE):
 *   s_ctrl_player        — player being configured (set by Control Options)
 *   s_ctrl_input_source  — device type 0=kbd / 1=joypad / 2=joystick [DAT_00490b94]
 *   s_ctrl_sel_action    — the action row being (re)bound while capturing
 *   s_ctrl_capturing     — 0 = browsing the list, 1 = capturing a key/button
 *   s_ctrl_binding_table — per-player joystick binding rows [player*9]
 *                          ([0]=active, [1]/[2]=axes, [3..8]=6 button actions,
 *                          value = physical_button + 2) [DAT_00463FC8]
 *   s_ctrl_kb_scancodes  — captured keyboard scancodes (DIK_*) [DAT_00464054]
 *
 * Action labels (k_ctrl_action_labels, from SNK_ControlText @ 0x100075E0):
 *   0=LEFT 1=RIGHT 2=ACCELERATE 3=BRAKE 4=HANDBRAKE 5=HORN/SIREN
 *   6=GEAR UP 7=GEAR DOWN 8=CHANGE VIEW 9=REAR VIEW. Keyboard players configure
 *   all 10; joystick players configure the 6 button actions [4..9].
 * ======================================================================== */






/* ========================================================================
 * [19] ScreenMusicTestExtras (0x418460) -- CD audio jukebox
 * States: 9
 * ======================================================================== */


/* ========================================================================
 * [20] CarSelectionScreenStateMachine (0x40DFC0)
 * States: 27 (0x00 - 0x1A)
 *
 * Handles 1P, 2P sequential, and network car selection.
 * ======================================================================== */

/* ========================================================================
 * Simultaneous multiplayer car select (grid)  [PORT ENHANCEMENT 2026-06-07]
 *
 * Every joined player picks their car at the same time, each driven by their
 * OWN controller, in panes laid out by the chosen split-screen grid (the same
 * mp_resolve_layout the race viewports use) so each player can identify their
 * own forked screen by its coloured PLAYER N banner/border. Each pane carries a
 * compact copy of the single-player car-select buttons (CAR / PAINT / STATS /
 * AUTO-MANUAL / OK), navigated by that player's own pad:
 *   up / down  move the button cursor   left / right  change the focused value
 *   A          activate (STATS opens the spec sheet; OK locks the pick in)
 *   B / ESC    back to the MULTIPLAYER LOBBY
 * When ALL players have pressed OK the screen auto-advances to track selection
 * after a short beat. Per-player input is read through the still-alive lobby
 * scan handles (td5_plat_input_device_nav); the per-player EXCLUSIVE devices are
 * bound only at the commit, since binding them releases the shared scan handles.
 * Inner states: 0x20 = lobby->grid slide-in animation, 0x21 = interactive. */

static float mp_simul_clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }





/* Parse a car's config.nfo into this pane's spec cache (only on car change). */
void mp_simul_load_pane_spec(int p, int car) {
    int sz = 0, field; size_t i; char *data;
    if (s_mp_pane_spec_car[p] == car) return;
    s_mp_pane_spec_car[p] = car;
    for (field = 0; field < 17; field++) s_mp_pane_spec[p][field][0] = '\0';
    if (car < 0 || car >= TD5_CAR_COUNT) return;
    data = (char *)td5_asset_open_and_read("config.nfo", s_car_zip_paths[car], &sz);
    if (!data || sz <= 0) return;
    field = 0; i = 0;
    while (field < 17 && i < (size_t)sz) {
        size_t j = 0;
        while (i < (size_t)sz && data[i] != '\n' && data[i] != '\r') {
            if (j + 1 < sizeof(s_mp_pane_spec[p][0])) s_mp_pane_spec[p][field][j++] = data[i];
            i++;
        }
        s_mp_pane_spec[p][field][j] = '\0';
        while (i < (size_t)sz && (data[i] == '\n' || data[i] == '\r')) i++;
        field++;
    }
    free(data);
}





/* Centred small-font helper (px in, native cap size scaled by lsx/lsy). */
static void mp_simul_small_centered(float cx_px, float y_px, const char *t,
                                    uint32_t c, float lsx, float lsy) {
    float w = fe_measure_small_text(t) * fe_glyph_sx(lsx, lsy);
    fe_draw_small_text(cx_px - w * 0.5f, y_px, t, c, lsx, lsy);
}

/* ◄ ► selector arrows (procedural triangle-SDF, ps_arrow) at the left/right
 * edges of a button. Native. [2026-06-16] The ArrowButtonz.tga sprite bitmap
 * fallback was retired. */
static void mp_simul_draw_arrows(float bx, float by, float bw, float bh, float sx, float sy) {
    if (!s_ps_arrow) return;
    float a2 = 12.0f, ay2 = by + (bh - a2) * 0.5f;
    fe_draw_arrow_proc((bx + 3.0f) * sx,           ay2 * sy, a2 * sx, a2 * sy, 0, 0xFF7995FFu);
    fe_draw_arrow_proc((bx + bw - 3.0f - a2) * sx, ay2 * sy, a2 * sx, a2 * sy, 1, 0xFF7995FFu);
}

/* One compact pane button — the REGULAR TD5 button frame (the same 9-slice / neon
 * design the rest of the menus use), with a TRANSPARENT interior when unselected.
 * The FOCUSED button uses the SELECTED frame (the golden ring) but with the
 * player's accent colour as the INTERIOR fill in place of the default purple.
 * Selector buttons draw the original ◄/► arrow sprites at the edges; `val`/
 * `swatch_rgb` (right, kept clear of the right arrow) are optional. */
static void mp_simul_draw_btn(float x, float y, float w, float h, const char *label,
                              int focused, uint32_t pcol, int arrows,
                              const char *val, int swatch_rgb, float sx, float sy) {
    uint32_t rgb = pcol & 0x00FFFFFFu;
    uint32_t tc  = 0xFFFFFFFFu;
    float ty = (y + (h - SMALLFONT_TTF_CAP) * 0.5f) * sy;   /* vertically centred */
    float redge = arrows ? (x + w - 18.0f) : (x + w - 6.0f);
    /* Transparent background: unselected frame has no interior fill; the focused
     * one fills with the player's accent colour. */
    fe_draw_button_frame_fill(x * sx, y * sy, w * sx, h * sy,
                              focused ? 0 : 1, rgb | 0xFF000000u, sx, sy);
    if (focused) {
        int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        int lum = (r * 30 + g * 59 + b * 11) / 100;     /* readable label over the accent */
        tc = (lum > 150) ? 0xFF101010u : 0xFFFFFFFFu;
    }
    if (arrows) mp_simul_draw_arrows(x, y, w, h, sx, sy);
    if (arrows) {
        fe_draw_small_text((x + 17.0f) * sx, ty, label, tc, sx, sy);
    } else {
        float lw = fe_measure_small_text(label) * fe_glyph_sx(sx, sy);
        fe_draw_small_text((x + w * 0.5f) * sx - lw * 0.5f, ty, label, tc, sx, sy);
    }
    if (swatch_rgb >= 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((redge - 15.0f) * sx, (y + h * 0.5f - 5.0f) * sy, 15.0f * sx, 10.0f * sy,
                     frontend_rgb_to_bgra((uint32_t)swatch_rgb), -1, 0, 0, 1, 1);
    } else if (val) {
        float vw = fe_measure_small_text(val) * fe_glyph_sx(sx, sy);
        fe_draw_small_text(redge * sx - vw, ty, val, tc, sx, sy);
    }
}

/* Per-pane STATS spec sheet, drawn as an OVERLAY on top of the normal pane (car
 * stays at the top, the button menu stays semi-visible underneath) — the same
 * "processing" the original uses (dimmed content, spec rows over it). Text is
 * scaled to fit the pane. */
static void mp_simul_render_stats(int p, float px, float py, float pw, float ph, float sx, float sy) {
    static const struct { const char *hdr; int fi; int exp; const char *sfx; } k_rows[] = {
        { "LAYOUT:",       2, 1, NULL   }, { "GEARS:",        3, 0, NULL   },
        { "PRICE:",        4, 0, NULL   }, { "TIRES:",        5, 3, NULL   },
        { "TOP SPEED:",    7, 0, " MPH" }, { "0-60 MPH:",     8, 0, " sec" },
        { "60-0 MPH:",     9, 0, " ft"  }, { "1/4 MILE:",    10, 0, " sec" },
        { "ENGINE:",      11, 2, NULL   }, { "COMPRESSION:", 12, 0, NULL   },
        { "DISPLACEMENT:",13, 0, NULL   }, { "LATERAL ACC:", 14, 0, NULL   },
        { "TORQUE:",      15, 0, NULL   }, { "HP:",          16, 0, NULL   },
    };
    int n_layout = (int)(sizeof(k_stat_layout_types) / sizeof(k_stat_layout_types[0]));
    int n_engine = (int)(sizeof(k_stat_engine_types) / sizeof(k_stat_engine_types[0]));
    float ax = px + 6.0f, ay = py + 28.0f, aw = pw - 12.0f, ah = ph - 34.0f;
    float rh, lsx, lsy, vx;
    int i;
    char val[64];

    /* Translucent dark scrim — dims the car + menu underneath so they stay
     * SEMI-VISIBLE behind the spec text (no opaque takeover). */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(ax * sx, ay * sy, aw * sx, ah * sy, 0xC2000810u, -1, 0, 0, 1, 1);

    rh  = ah / 15.0f;                         /* 14 rows + a back-hint line */
    lsy = sy * mp_simul_clamp01(rh / 11.0f);  /* shrink the small font to fit a row */
    if (lsy < sy * 0.42f) lsy = sy * 0.42f;
    lsx = sx * (lsy / sy);
    vx  = ax + aw * 0.46f;
    for (i = 0; i < 14; i++) {
        float ry = (ay + 2.0f + (float)i * rh) * sy;
        const char *raw = s_mp_pane_spec[p][k_rows[i].fi];
        int idx;
        fe_draw_small_text(ax * sx + 2.0f * sx, ry, k_rows[i].hdr, 0xFFC8C8C8u, lsx, lsy);
        switch (k_rows[i].exp) {
        case 1:
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_small_text(vx * sx, ry, (idx >= 0 && idx < n_layout) ? k_stat_layout_types[idx] : raw,
                               0xFFFFFFFFu, lsx, lsy);
            break;
        case 2:
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_small_text(vx * sx, ry, (idx >= 0 && idx < n_engine) ? k_stat_engine_types[idx] : raw,
                               0xFFFFFFFFu, lsx, lsy);
            break;
        case 3: {
            char f[24], r[24];
            frontend_fmt_spec(f, sizeof f, raw);
            frontend_fmt_spec(r, sizeof r, (k_rows[i].fi + 1 < 17) ? s_mp_pane_spec[p][k_rows[i].fi + 1] : "");
            snprintf(val, sizeof val, "%s/%s", f, r);
            fe_draw_small_text(vx * sx, ry, val, 0xFFFFFFFFu, lsx, lsy);
            break;
        }
        default:
            frontend_fmt_spec(val, sizeof val, raw);
            if (k_rows[i].sfx && val[0] && val[0] != '-') {
                size_t vl = strlen(val), sl = strlen(k_rows[i].sfx);
                if (vl + sl + 1 < sizeof val) memcpy(val + vl, k_rows[i].sfx, sl + 1);
            }
            fe_draw_small_text(vx * sx, ry, val, 0xFFFFFFFFu, lsx, lsy);
            break;
        }
    }
    mp_simul_small_centered((px + pw * 0.5f) * sx, (ay + ah - rh) * sy, "A / B = BACK",
                            0xFFFFE060u, lsx, lsy);
}

/* [#3 2026-06-16] MP per-player panel MAX-WIDTH cap, matching the IDENTICAL
 * formula the sibling td5_fe_race.c applies to its in-file overlays (the
 * position-picker cells + PROFILE chip) so the main name/colour panes
 * (frontend_mp_setup_render) and the simultaneous car-select panes
 * (frontend_mp_simul_carsel_render) drawn HERE line up underneath them.
 *
 * EXACT FORMULA (canvas width W = 640, cols from mp_resolve_layout):
 *   pane_w  = min(W / cols, W / 3)          (cap at the 3x3-grid pane = 213.33)
 *   row_x0  = (W - cols * pane_w) / 2        (centre the capped row)
 *   px[col] = row_x0 + col * pane_w
 * With 2 players each pane is capped at 213.33 px and the row is centred;
 * cols >= 3 is unchanged (pane_w already <= cap, row_x0 == 0). Height stays
 * W/rows (only WIDTH is capped). Same env var the fe_race helper used. */
static int frontend_mp_panel_cap_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_MP_PANEL_CAP");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "MP main-pane width cap (#3) %s (TD5RE_MP_PANEL_CAP=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* Writes the capped per-pane width to *pane_w and the centred row's col-0 left
 * edge to *row_x0 for `cols` columns across a 640px canvas. No-op (full width,
 * x0=0) when the cap knob is off. */
static void frontend_mp_panel_capped(int cols, float *pane_w, float *row_x0) {
    float full = 640.0f / (float)(cols < 1 ? 1 : cols);
    float w = full;
    if (frontend_mp_panel_cap_on()) {
        float cap = 640.0f / 3.0f;
        if (w > cap) w = cap;
    }
    if (pane_w) *pane_w = w;
    if (row_x0) *row_x0 = (640.0f - (float)cols * w) * 0.5f;
}

static void frontend_mp_simul_carsel_render(float sx, float sy) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    float anim_t = 1.0f;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    (void)missing;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    if (s_inner_state == 0x20)
        anim_t = mp_simul_clamp01((float)(now - s_mp_simul_anim_ms) / (float)MP_SIMUL_ANIM_MS);

    /* [#3] Cap pane WIDTH at the 3x3-grid equivalent (640/3) and centre the row,
     * matching the fe_race overlays so the position-picker/profile chips line up. */
    float pane_w, row_x0 = 0.0f;
    frontend_mp_panel_capped(cols, &pane_w, &row_x0);
    /* [R1] Reserve a top band for the "SELECT CAR" title so the panes start BELOW
     * it instead of overlapping the title text.
     * [R4 2026-06-19] Raise to the shared FE_MP_TOP_BAND (85) to match every other
     * MP screen — the old 40px let the panes overlap the title + the background
     * art's upper decoration line. Reserve a matching bottom band so the panes
     * occupy the same comfortable middle band as the profile-setup screen. */
    const float mp_title_band  = FE_MP_TOP_BAND;
    const float mp_bottom_band = FE_MP_BOTTOM_BAND;
    float pane_h = (480.0f - mp_title_band - mp_bottom_band) / (float)rows;

    /* Dim the MainMenu background (ramps up with the slide-in). */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy,
                 ((uint32_t)(0xB0 * anim_t) << 24) | 0x101018u, -1, 0, 0, 1, 1);

    /* [#19] Standard top title. The global title-strip path suppresses titles
     * while s_mp_simul is set (the grid draws its own per-pane headers), so the
     * screen header is drawn here directly. */
    if (td5_titlefont_ready())
        frontend_draw_screen_title("SELECT CAR", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                                   0xFFE3D708u, sx, sy);

    for (p = 0; p < n; p++) {
        /* [#6 2026-06-15] Place each pane at the player's CHOSEN position cell
         * (from the split-screen position picker) instead of identity p, so a
         * player parked bottom-right while choosing a screen also picks their car
         * bottom-right. Unclaimed cells draw nothing (this loop only iterates the
         * human players). Identity when the positions feature is off. */
        extern int frontend_mp_player_pane_cell(int);  /* defined in td5_fe_race.c */
        int cell = frontend_mp_player_pane_cell(p);
        int col = cell % cols, row = cell / cols;
        float px = row_x0 + (float)col * pane_w, py = mp_title_band + (float)row * pane_h;
        float cx, pyr, pt, pe, rise;
        uint32_t rgb  = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;  /* chosen identity colour */
        uint32_t pcol = rgb | 0xFF000000u;
        int car   = s_mp_player_car[p];
        int td6   = frontend_car_is_td6(car);
        int ready = s_mp_player_ready[p];
        int stats = (s_mp_pane_substate[p] == 1);
        char buf[64];

        /* Staggered rise-in: each pane eases up into place from below. */
        pt = mp_simul_clamp01(anim_t * (1.0f + 0.12f * (float)n) - 0.10f * (float)p);
        pe = 1.0f - (1.0f - pt) * (1.0f - pt);
        rise = (1.0f - pe) * (pane_h * 0.45f);
        pyr = py + rise;
        cx  = px + pane_w * 0.5f;

        /* Pane backdrop + coloured border (brighter/thicker when READY). */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, (pane_h - 6) * sy,
                     ready ? 0xC0102818u : 0xB0141420u, -1, 0, 0, 1, 1);
        {
            float bt = ready ? 4.0f : 2.0f;
            uint32_t bc = rgb | (ready ? 0xFF000000u : 0xD0000000u);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + pane_h - 3 - bt) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + pane_w - 3 - bt) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
        }

        /* Header banner: the player's chosen NAME (falls back to PLAYER N). */
        if (s_mp_player_name[p][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[p]);
        else                        snprintf(buf, sizeof buf, "PLAYER %d", p + 1);
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, 16.0f * sy,
                     rgb | 0xD0000000u, -1, 0, 0, 1, 1);
        mp_simul_small_centered(cx * sx, (pyr + 6) * sy, buf, 0xFF000000u, sx, sy);

        /* Car NAME above the image (request). */
        snprintf(buf, sizeof buf, "%s", frontend_get_car_display_name(car));
        mp_simul_small_centered(cx * sx, (pyr + 21) * sy, buf, 0xFFFFFFFFu, sx, sy);

        /* Car image (below the name). */
        {
            float avail_w = pane_w - 20.0f;
            float avail_h = pane_h * 0.38f;
            float ar = 408.0f / 280.0f;
            float dw = avail_w, dh = dw / ar, dx, dy;
            if (dh > avail_h) { dh = avail_h; dw = dh * ar; }
            dx = cx - dw * 0.5f;
            dy = pyr + 32.0f;
            if (s_mp_pane_preview[p] > 0)
                fe_draw_surface_rect(s_mp_pane_preview[p], dx * sx, dy * sy, dw * sx, dh * sy, 0xFFFFFFFF);
            if (td6 && frontend_car_paintable(car) && s_mp_pane_overlay[p] > 0)
                fe_draw_surface_rect(s_mp_pane_overlay[p], dx * sx, dy * sy, dw * sx, dh * sy,
                                     frontend_rgb_to_bgra((uint32_t)s_mp_player_color[p]));
        }

        if (ready) {
            mp_simul_small_centered(cx * sx, (pyr + pane_h * 0.66f) * sy, "READY", 0xFF40FF40u, sx, sy);
            mp_simul_small_centered(cx * sx, (pyr + pane_h - 16.0f) * sy, "A = CHANGE   B = LOBBY",
                                    0xFFB0B0B0u, sx, sy);
            continue;
        }

        /* Button stack (CAR / PAINT / [stat bars] / MORE STATS / AUTO-MANUAL / OK).
         * The at-a-glance stat panel sits between PAINT and MORE STATS; it's the
         * flex element so the 5 buttons keep their [9,22]px size and the panel
         * shrinks (down to thin label-less bars) when a small split runs out of
         * room. */
        {
            float bx = px + 8.0f, bw = pane_w - 16.0f;
            float bsy = pyr + 32.0f + pane_h * 0.38f + 4.0f;
            float room = (pyr + pane_h - 18.0f) - bsy;
            float bh = (room - 10.0f) / 6.2f;   /* 5 buttons + ~1.2-button stat panel + gaps */
            int focus = s_mp_pane_btn[p];
            float yy = bsy;
            float panel_h;
            char vbuf[24];
            if (bh < 10.0f) bh = 10.0f;
            if (bh > 28.0f) bh = 28.0f;   /* taller buttons where the pane has room */
            panel_h = bh * 1.2f;
            if (5.0f * bh + panel_h + 10.0f > room) {   /* buttons hit their floor — shrink the panel */
                panel_h = room - 5.0f * bh - 10.0f;
                if (panel_h < 8.0f) panel_h = 8.0f;
            }

            mp_simul_draw_btn(bx, yy, bw, bh, "CAR", focus == MP_BTN_CAR, pcol, 1, NULL, -1, sx, sy);
            yy += bh + 2.0f;
            if (td6 && frontend_car_paintable(car))
                mp_simul_draw_btn(bx, yy, bw, bh, "PAINT", focus == MP_BTN_PAINT, pcol, 1, NULL,
                                  s_mp_player_color[p], sx, sy);
            else if (!td6 && frontend_car_has_paint(car)) {
                snprintf(vbuf, sizeof vbuf, "%d/4", s_mp_player_paint[p] + 1);
                mp_simul_draw_btn(bx, yy, bw, bh, "PAINT", focus == MP_BTN_PAINT, pcol, 1, vbuf, -1, sx, sy);
            } else
                mp_simul_draw_btn(bx, yy, bw, bh, "PAINT", focus == MP_BTN_PAINT, pcol, 0, "-", -1, sx, sy);
            yy += bh + 2.0f;
            mp_simul_load_pane_spec(p, car);   /* cached; bars need the spec even before MORE STATS is opened */
            frontend_draw_car_stat_bars(bx, yy, bw, panel_h,
                                        s_mp_pane_spec[p][7], s_mp_pane_spec[p][8],
                                        s_mp_pane_spec[p][14], pcol, 1, sx, sy);
            yy += panel_h + 2.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "MORE STATS", focus == MP_BTN_STATS, pcol, 0, NULL, -1, sx, sy);
            yy += bh + 2.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, s_mp_player_trans[p] ? "MANUAL" : "AUTOMATIC",
                              focus == MP_BTN_TRANS, pcol, 0, NULL, -1, sx, sy);
            yy += bh + 2.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "OK", focus == MP_BTN_OK, pcol, 0, NULL, -1, sx, sy);
        }

        /* STATS spec sheet overlays the car + menu (both kept semi-visible). */
        if (stats) mp_simul_render_stats(p, px, pyr, pane_w, pane_h, sx, sy);
    }

    /* All-ready beat banner. */
    if (s_mp_simul_ready_ms != 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad(0.0f, 227.0f * sy, 640.0f * sx, 26.0f * sy, 0xC0103018u, -1, 0, 0, 1, 1);
        fe_draw_text_centered(320.0f * sx, 232.0f * sy, "ALL READY - STARTING...", 0xFFFFFF80u, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ========================================================================
 * Player-setup window (phase 0): per-pane NAME + background COLOUR entry.
 * Same grid layout as the car-select window. Keyboard players type directly;
 * pad players use an on-screen QWERTY. Colour uses the TD6 colour picker grid.
 * ======================================================================== */


/* Colour at a cell of the compact 16x16 background-colour palette (0xRRGGBB).
 * Hue runs across the columns; rows run light tint -> pure -> dark shade — with
 * NO fully-white top row (min saturation 0.35) and no pure-black bottom. */
uint32_t mp_setup_grid_color(int col, int row) {
    float hue = (float)col / (float)MP_COL_COLS;
    float t   = (float)row / (float)(MP_COL_ROWS - 1);   /* 0..1 */
    float sat, val;
    if (t < 0.5f) { sat = 0.35f + 1.30f * t; if (sat > 1.0f) sat = 1.0f; val = 1.0f; }
    else          { sat = 1.0f; val = 1.0f - 1.6f * (t - 0.5f); if (val < 0.20f) val = 0.20f; }
    return td6_hsv_to_rgb(hue, sat, val);
}




/* [2026-06-15 BUG #5] TD5RE_KBD_GRID (default ON; "0" reverts). The on-screen
 * QWERTY's pad nav (td5_fe_race.c) moves the cursor by COLUMN INDEX preserved
 * across rows (UP/DOWN keep `col`, clamped to the new row's length) — a pure grid
 * model. But the render USED to center each row by its own length
 * (rowx = ax + (aw - kw*len)*0.5f), so a given column index sat at a DIFFERENT
 * screen-x in each row (rows are 10/10/9/7 keys). The highlight (drawn at that
 * same index) therefore landed on a key that was NOT geometrically above/below
 * the previous selection — e.g. UP from 'C' (row3 col2) selected 'D' (row2 col2)
 * while the key visually above 'C' was 'F', so the move looked wrong AND the box
 * appeared over the "wrong" letter. Fix: align every row to a common left origin
 * with one fixed cell width, so column index -> identical x in every row. Now the
 * index-based nav steps to the key directly above/below and the highlight sits
 * exactly on the selected key. "0" restores the old centered (mismatched) rows. */
static int frontend_kbd_grid_align_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD5RE_KBD_GRID");
        v = (e && e[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "On-screen QWERTY column-aligned grid (#5) %s (TD5RE_KBD_GRID=%s)",
                  v ? "ENABLED" : "disabled", e ? e : "default");
    }
    return v;
}

/* On-screen QWERTY for pad name entry — compact: each row is just over the
 * uppercase cap height (MP_KBD_ROW_H), width spans the pane content. */
static void mp_setup_render_kbd(int p, float ax, float ay, float aw, float ah,
                                uint32_t accent, float sx, float sy) {
    int row, col;
    int grid_align = frontend_kbd_grid_align_on();
    float rh = MP_KBD_ROW_H;
    (void)ah;
    for (row = 0; row < MP_KBD_LETTER_ROWS; row++) {
        const char *r = k_mp_kbd_rows[row];
        int len = (int)strlen(r);
        float kw = aw / 10.0f;
        /* [BUG #5] Left-align every row to a common origin so column index N is at
         * the SAME x in every row (matches the index-preserving pad nav). The old
         * per-row centering (kept under TD5RE_KBD_GRID=0) put the same col index at
         * a different x per row, desyncing nav target vs highlight. */
        float rowx = grid_align ? ax : ax + (aw - kw * (float)len) * 0.5f;
        float ry = ay + (float)row * rh;
        for (col = 0; col < len; col++) {
            float kx = rowx + (float)col * kw;
            int foc = (s_mp_kbd_row[p] == row && s_mp_kbd_col[p] == col);
            char ch[2]; ch[0] = r[col]; ch[1] = '\0';
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(kx * sx, ry * sy, (kw - 1.0f) * sx, (rh - 1.0f) * sy,
                         foc ? (accent | 0xFF000000u) : 0xB0203040u, -1, 0, 0, 1, 1);
            mp_simul_small_centered((kx + kw * 0.5f) * sx, (ry + (rh - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                                    ch, foc ? 0xFF101010u : 0xFFFFFFFFu, sx, sy);
        }
    }
    {
        static const char *const sp[3] = { "SPACE", "DEL", "DONE" };
        float kw = aw / 3.0f, ry = ay + (float)MP_KBD_SPECIAL * rh;
        for (col = 0; col < 3; col++) {
            float kx = ax + (float)col * kw;
            int foc = (s_mp_kbd_row[p] == MP_KBD_SPECIAL && s_mp_kbd_col[p] == col);
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(kx * sx, ry * sy, (kw - 1.0f) * sx, (rh - 1.0f) * sy,
                         foc ? (accent | 0xFF000000u) : 0xB0203040u, -1, 0, 0, 1, 1);
            mp_simul_small_centered((kx + kw * 0.5f) * sx, (ry + (rh - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                                    sp[col], foc ? 0xFF101010u : 0xFFFFFFFFu, sx, sy);
        }
    }
}

/* Compact 16x16 HSV background-colour palette. Height matches the keyboard. */
static void mp_setup_render_colorgrid(int p, float ax, float ay, float aw, float ah,
                                      float sx, float sy) {
    int r, c;
    float cw = aw / (float)MP_COL_COLS, ch = ah / (float)MP_COL_ROWS;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (r = 0; r < MP_COL_ROWS; r++) {
        for (c = 0; c < MP_COL_COLS; c++) {
            uint32_t col = mp_setup_grid_color(c, r);
            float kx = ax + (float)c * cw, ky = ay + (float)r * ch;
            fe_draw_quad(kx * sx, ky * sy, cw * sx + 0.5f, ch * sy + 0.5f,
                         frontend_rgb_to_bgra(col), -1, 0, 0, 1, 1);
        }
    }
    {
        float kx = ax + (float)s_mp_col_col[p] * cw, ky = ay + (float)s_mp_col_row[p] * ch;
        uint32_t hl = 0xFFFFFFFFu;
        float t = 1.5f;
        fe_draw_quad((kx - t) * sx, (ky - t) * sy, (cw + 2 * t) * sx, t * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx - t) * sx, (ky + ch) * sy, (cw + 2 * t) * sx, t * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx - t) * sx, (ky - t) * sy, t * sx, (ch + 2 * t) * sy, hl, -1, 0, 0, 1, 1);
        fe_draw_quad((kx + cw) * sx, (ky - t) * sy, t * sx, (ch + 2 * t) * sy, hl, -1, 0, 0, 1, 1);
    }
}

/* [#8] Render the split-screen POSITION picker: the cols x rows layout grid with
 * each occupied cell tinted in its player's accent + name, the empty cells shown
 * as "EMPTY", and a footer with controls + the host's layout selector. Each
 * player's OWN cell gets a thicker pulsing border so they can see where they are.
 * Reads s_mp_player_cell[] / accent / name / ready (no per-pane surfaces). */
void frontend_mp_position_render(float sx, float sy) {
    int p, c, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    int ncells, all_ready = 1;
    int owner[TD5_MAX_VIEWPORTS];
    float pulse = 0.55f + 0.45f * (float)((now / 60u) % 16u) / 15.0f; /* 0.55..1.0 */
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    ncells = cols * rows;
    if (ncells > TD5_MAX_VIEWPORTS) ncells = TD5_MAX_VIEWPORTS;

    /* cell -> occupying player (or -1 = empty). */
    for (c = 0; c < TD5_MAX_VIEWPORTS; c++) owner[c] = -1;
    for (p = 0; p < n; p++) {
        int cell = s_mp_player_cell[p];
        if (cell >= 0 && cell < ncells) owner[cell] = p;
        if (!s_mp_player_ready[p]) all_ready = 0;
    }

    /* Dim full-screen backdrop + title. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xC0101018u, -1, 0, 0, 1, 1);
    fe_draw_text_centered(320.0f * sx, 10.0f * sy, "CHOOSE YOUR SCREEN", 0xFFFFE060u, sx, sy);

    /* Layout grid occupies a centred area below the title, above the footer. */
    {
        const float gx = 40.0f, gy = 40.0f, gw = 560.0f, gh = 372.0f;
        float cw = gw / (float)cols, ch = gh / (float)rows;
        for (c = 0; c < ncells; c++) {
            int col = c % cols, row = c / cols;
            float px = gx + (float)col * cw, py = gy + (float)row * ch;
            float ccx = px + cw * 0.5f;
            int occ = owner[c];
            uint32_t rgb = (occ >= 0) ? ((uint32_t)s_mp_player_accent[occ] & 0x00FFFFFFu) : 0x303040u;
            int ready = (occ >= 0) && s_mp_player_ready[occ];
            char buf[40];

            /* cell fill (faint tint of the owner's colour). */
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, (ch - 6) * sy,
                         (occ >= 0) ? (rgb | 0x40000000u) : 0x40181820u, -1, 0, 0, 1, 1);

            /* border: this player's own cell pulses + is thick; others steady. */
            {
                float bt = (occ >= 0) ? 3.0f : 1.5f;
                uint32_t a = (occ >= 0)
                             ? ((uint32_t)(0x60 + (ready ? 0x9F : (int)(0x9F * pulse))) << 24)
                             : 0x80000000u;
                uint32_t bc = (rgb | a);
                fe_draw_quad((px + 3) * sx, (py + 3) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + 3) * sx, (py + ch - 3 - bt) * sy, (cw - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + 3) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
                fe_draw_quad((px + cw - 3 - bt) * sx, (py + 3) * sy, bt * sx, (ch - 6) * sy, bc, -1, 0, 0, 1, 1);
            }

            /* big cell number (1-based) so players can call out "I'm on 3". */
            snprintf(buf, sizeof buf, "%d", c + 1);
            fe_draw_text_centered(ccx * sx, (py + ch * 0.30f) * sy, buf,
                                  (occ >= 0) ? 0xFFFFFFFFu : 0xFF707080u, sx, sy);

            if (occ >= 0) {
                if (s_mp_player_name[occ][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[occ]);
                else                          snprintf(buf, sizeof buf, "PLAYER %d", occ + 1);
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy, buf,
                                        rgb | 0xFF000000u, sx, sy);
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 40.0f) * sy,
                                        ready ? "READY" : "MOVE: D-PAD", ready ? 0xFF40FF40u : 0xFFB0B0B0u,
                                        sx, sy);
            } else {
                mp_simul_small_centered(ccx * sx, (py + ch * 0.30f + 26.0f) * sy, "EMPTY",
                                        0xFF707080u, sx, sy);
            }
        }
    }

    /* Footer: controls + host layout selector + all-ready hint. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(0.0f, 420.0f * sy, 640.0f * sx, 60.0f * sy, 0xB0080810u, -1, 0, 0, 1, 1);
    {
        int lcnt = 1;
        const MpSplitLayout *opts = mp_split_layouts(n, &lcnt);
        char lbuf[64];
        const char *lname = (opts && s_mp_layout_sel >= 0 && s_mp_layout_sel < lcnt)
                            ? opts[s_mp_layout_sel].label : "SINGLE";
        if (lcnt > 1)
            snprintf(lbuf, sizeof lbuf, "P1 L/R: LAYOUT  [%s]", lname);
        else
            snprintf(lbuf, sizeof lbuf, "LAYOUT: %s", lname);
        fe_draw_text_centered(320.0f * sx, 426.0f * sy,
                              "D-PAD: MOVE   A: READY   B: BACK", 0xFFFFFFFFu, sx, sy);
        mp_simul_small_centered(320.0f * sx, 450.0f * sy, lbuf, 0xFFFFE060u, sx, sy);
        if (all_ready)
            mp_simul_small_centered(320.0f * sx, 464.0f * sy, "ALL READY - STARTING CARS...",
                                    0xFF80FF80u, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

static void frontend_mp_setup_render(float sx, float sy) {
    int p, n = s_num_human_players;
    int cols = 1, rows = 1, missing = 0;
    uint32_t now = td5_plat_time_ms();
    int caret = ((now / 400u) & 1u) != 0;
    float anim_t = 1.0f;
    if (n < 2) n = 2;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    (void)missing;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (s_inner_state == 0x20)
        anim_t = mp_simul_clamp01((float)(now - s_mp_simul_anim_ms) / (float)MP_SIMUL_ANIM_MS);

    /* [#3] Same capped/centred pane width as the car-select panes + fe_race
     * overlays (640/3 cap), so the name/colour row lines up underneath them. */
    float pane_w, row_x0 = 0.0f;
    frontend_mp_panel_capped(cols, &pane_w, &row_x0);
    /* [R1] Reserve a top band for the "PROFILE SELECTION" title so the panes start
     * BELOW it instead of overlapping the title text.
     * [R3-2 2026-06-19] The panes were still spanning all the way to y=480, so they
     * reached into the background art's lower text lines and sat tight under the
     * title. Grow the top band (more air under the title) AND reserve a matching
     * BOTTOM band so the boxes occupy a comfortable MIDDLE band — title clear above,
     * the art's 3 text lines clear below. Both bands MUST match the companion
     * fe_race.c profile-chip overlay (which positions PROFILE relative to the same
     * py/pane_h), or the chip drifts off the pane.
     * [R4 2026-06-19] Boxes were still too high (overlapping the title + the
     * background art's upper decoration bar). Use the shared FE_MP_TOP_BAND (85)
     * so the panes start below that line, applied consistently across every MP
     * screen. The companion fe_race.c profile-chip overlay uses the SAME literals. */
    const float mp_title_band  = FE_MP_TOP_BAND;
    const float mp_bottom_band = FE_MP_BOTTOM_BAND;
    float pane_h = (480.0f - mp_title_band - mp_bottom_band) / (float)rows;

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    /* [#18a] Profile/name-colour setup: by default DON'T darken the background so
     * the MainMenu art stays visible behind the panes. Set TD5RE_MP_SETUP_DIM=1 to
     * restore the old full-screen scrim. */
    {
        static int s_draw_dim = -1;
        if (s_draw_dim < 0)
            s_draw_dim = (getenv("TD5RE_MP_SETUP_DIM") != NULL &&
                          getenv("TD5RE_MP_SETUP_DIM")[0] == '1');
        if (s_draw_dim)
            fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy,
                         ((uint32_t)(0xB0 * anim_t) << 24) | 0x101018u, -1, 0, 0, 1, 1);
    }

    /* [#18a] Standard top title (Lunatica face) so the setup step matches every
     * other menu's header. */
    if (td5_titlefont_ready())
        frontend_draw_screen_title("PROFILE SELECTION", FE_TITLE_LEFT_X * sx, 17.0f * sy,
                                   0xFFE3D708u, sx, sy);

    for (p = 0; p < n; p++) {
        int col = p % cols, row = p / cols;
        float px = row_x0 + (float)col * pane_w, py = mp_title_band + (float)row * pane_h;
        float cx, pyr, pt, pe, rise, ax, ay, aw, ah;
        uint32_t rgb = (uint32_t)s_mp_player_accent[p] & 0x00FFFFFFu;
        uint32_t pcol = rgb | 0xFF000000u;
        int ready = s_mp_player_ready[p];
        int sub = s_mp_setup_sub[p];
        int isk = (s_mp_join_device[p] == 0);
        char buf[64];

        pt = mp_simul_clamp01(anim_t * (1.0f + 0.12f * (float)n) - 0.10f * (float)p);
        pe = 1.0f - (1.0f - pt) * (1.0f - pt);
        rise = (1.0f - pe) * (pane_h * 0.45f);
        pyr = py + rise;
        cx  = px + pane_w * 0.5f;

        /* Backdrop + accent border. */
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, (pane_h - 6) * sy,
                     ready ? 0xC0102818u : 0xB0141420u, -1, 0, 0, 1, 1);
        {
            float bt = ready ? 4.0f : 2.0f;
            uint32_t bc = rgb | (ready ? 0xFF000000u : 0xD0000000u);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + pane_h - 3 - bt) * sy, (pane_w - 6) * sx, bt * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
            fe_draw_quad((px + pane_w - 3 - bt) * sx, (pyr + 3) * sy, bt * sx, (pane_h - 6) * sy, bc, -1, 0, 0, 1, 1);
        }

        /* Header banner: chosen name or PLAYER N. */
        if (s_mp_player_name[p][0]) snprintf(buf, sizeof buf, "%s", s_mp_player_name[p]);
        else                        snprintf(buf, sizeof buf, "PLAYER %d", p + 1);
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad((px + 3) * sx, (pyr + 3) * sy, (pane_w - 6) * sx, 16.0f * sy,
                     rgb | 0xD0000000u, -1, 0, 0, 1, 1);
        mp_simul_small_centered(cx * sx, (pyr + 6) * sy, buf, 0xFF000000u, sx, sy);

        ax = px + 6.0f; ay = pyr + 22.0f; aw = pane_w - 12.0f; ah = pane_h - 28.0f;

        if (ready) {
            mp_simul_small_centered(cx * sx, (pyr + pane_h * 0.42f) * sy, "READY", 0xFF40FF40u, sx, sy);
            snprintf(buf, sizeof buf, "%s", s_mp_player_name[p][0] ? s_mp_player_name[p] : "(no name)");
            mp_simul_small_centered(cx * sx, (pyr + pane_h * 0.42f + 14.0f) * sy, buf, 0xFFFFFFFFu, sx, sy);
            /* [#16] Nudged up ~10px so the profile-management hint clears the pane
             * bottom edge / footer band. */
            mp_simul_small_centered(cx * sx, (pyr + pane_h - 26.0f) * sy, "A = CHANGE   B = LOBBY",
                                    0xFFB0B0B0u, sx, sy);
            continue;
        }

        if (sub == 1) {                 /* NAME entry */
            /* name field box */
            float fy = ay;
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(ax * sx, fy * sy, aw * sx, 16.0f * sy, 0xC0000814u, -1, 0, 0, 1, 1);
            snprintf(buf, sizeof buf, "%s%s", s_mp_player_name[p], caret ? "_" : " ");
            fe_draw_small_text((ax + 4.0f) * sx, (fy + (16.0f - SMALLFONT_TTF_CAP) * 0.5f) * sy,
                               buf, 0xFFFFFFFFu, sx, sy);
            if (isk)
                mp_simul_small_centered(cx * sx, (fy + 24.0f) * sy,
                                        "TYPE NAME - ENTER=DONE  ESC=BACK", 0xFFFFE060u, sx, sy);
            else {
                mp_setup_render_kbd(p, ax, fy + 19.0f, aw, MP_KBD_BLOCK_H, pcol, sx, sy);
                /* [#15c] Pad hints below the on-screen keyboard (inside the pane). */
                mp_simul_small_centered(cx * sx, (fy + 19.0f + MP_KBD_BLOCK_H + 3.0f) * sy,
                                        "X = DELETE   START = DONE", 0xFFB0B0B0u, sx, sy);
            }
            continue;
        }

        if (sub == 2) {                 /* COLOUR picker (compact 16x16) */
            mp_setup_render_colorgrid(p, ax, ay + 2.0f, aw, MP_KBD_BLOCK_H, sx, sy);
            mp_simul_small_centered(cx * sx, (ay + 2.0f + MP_KBD_BLOCK_H + 3.0f) * sy,
                                    "A=OK  B=BACK", 0xFFFFE060u, sx, sy);
            continue;
        }

        /* idle: NAME / COLOUR / OK buttons.
         * [#3 2026-06-15] When profile management is enabled the band has a 4th
         * slot for the PROFILE button (drawn by fe_race's
         * frontend_mp_setup_profile_render); reserve its row here and pin OK to
         * the LAST slot so the nav band lines up with the profile overlay. */
        {
            extern int mp_profiles_enabled(void);   /* defined in td5_fe_race.c */
            int slots = mp_profiles_enabled() ? 4 : MP_SET_COUNT;
            float bx = px + 8.0f, bw = pane_w - 16.0f;
            float bsy = ay + 4.0f;
            float room = (pyr + pane_h - 12.0f) - bsy;
            float bh = room / (float)slots - 3.0f;
            int focus = s_mp_setup_btn[p];
            float yy = bsy;
            if (bh < 12.0f) bh = 12.0f;
            if (bh > 26.0f) bh = 26.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "NAME", focus == MP_SET_NAME, pcol, 0,
                              s_mp_player_name[p][0] ? s_mp_player_name[p] : "-", -1, sx, sy);
            yy += bh + 3.0f;
            mp_simul_draw_btn(bx, yy, bw, bh, "COLOUR", focus == MP_SET_COLOUR, pcol, 0,
                              NULL, s_mp_player_accent[p], sx, sy);
            /* PROFILE (slot 2) is drawn by frontend_mp_setup_profile_render. */
            yy = bsy + (float)(slots - 1) * (bh + 3.0f);
            mp_simul_draw_btn(bx, yy, bw, bh, "OK", focus == MP_SET_OK, pcol, 0, NULL, -1, sx, sy);
        }
    }

    if (s_mp_simul_ready_ms != 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_quad(0.0f, 227.0f * sy, 640.0f * sx, 26.0f * sy, 0xC0103018u, -1, 0, 0, 1, 1);
        fe_draw_text_centered(320.0f * sx, 232.0f * sy, "ALL READY - CHOOSE CARS...", 0xFFFFFF80u, sx, sy);
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}


/* Hide/show the Direction toggle (button index 1) for the currently selected
 * track. Only point-to-point tracks ship reverse data; circuit tracks are
 * forward-only, so the original removes the Direction option for them (the
 * original gates on a per-track "reverse unlocked" byte gNpcRacerCheatFlags
 * @0x004A2C98 which is only ever set for point-to-point tracks; the port has
 * no unlock-progression model, so we gate directly on reverse-data presence —
 * equivalent for the "circuit tracks never offer reverse" requirement).
 *
 * Mirrors the original's mechanism: when not available the sprite is moved
 * off-screen (here: hidden=skip render) AND the toggle is inert (here:
 * disabled, so the vertical nav and mouse hover both skip button 1). When the
 * track has no reverse we also force s_track_direction=0 and reset the label so
 * a stale "Backwards" choice can't carry onto a forward-only track. Random
 * (s_selected_track < 0, 2P "?") keeps the toggle shown. */
/* Gate the Direction selector to reverse-capable tracks. manage_label=1 (Track
 * Selection) makes the button LABEL itself the value ("Forwards"/"Backwards"),
 * so it gets reset to "Forwards" when hidden. manage_label=0 (Quick Race) keeps
 * a static caption and draws the value separately, so the label is left alone. */



/* ========================================================================
 * [21] TrackSelectionScreenStateMachine (0x427630)
 * States: 9 (0x00 - 0x08)
 * Buttons: Track (with arrows), Direction toggle, OK, Back
 * ======================================================================== */


/* ========================================================================
 * [22] ScreenExtrasGallery (0x417D50) -- Credits / developer mugshots
 * States: 4
 * Original loads all 27 surfaces from Mugshots.zip at init.
 * Source port loads on demand (one at a time) to save VRAM.
 * Exits game when complete (exit flow) or returns to previous screen.
 * ======================================================================== */

#define GALLERY_PIC_COUNT   27
#define GALLERY_ALL_VISITED ((1 << GALLERY_PIC_COUNT) - 1)

/* Original push order (from 0x465AAC string table):
 * Legals5-1 first, then developer mugshots. */
static const char * const s_gallery_names[GALLERY_PIC_COUNT] = {
    "Legals5.tga", "Legals4.tga", "Legals3.tga", "Legals2.tga", "Legals1.tga",
    "Daz.tga",    "JFK.tga",     "Marie.tga",   "Matt.tga",    "Slade.tga",
    "ChrisD.tga", "DaveyB.tga",  "TonyC.tga",   "DavidT.tga",  "JohnS.tga",
    "TonyP.tga",  "Les.tga",     "Bez.tga",     "Mike.tga",    "Rich.tga",
    "Steve.tga",  "Headley.tga", "Chris.tga",   "MikeT.tga",   "Snake.tga",
    "Gareth.tga", "Bob.tga"
};


/* ========================================================================
 * [23] ScreenPostRaceHighScoreTable (0x413580)
 * States: 9
 * ======================================================================== */


/* ========================================================================
 * [24] RunRaceResultsScreen (0x422480)
 * States: 22 (0x00 - 0x15)
 *
 * Central post-race hub: score display, replay, save, cup progression.
 * ======================================================================== */


/* ========================================================================
 * [25] ScreenPostRaceNameEntry (0x413BC0)
 * States: 13 (0x00 - 0x0C)
 * ======================================================================== */


/* ========================================================================
 * [26] ScreenCupFailedDialog (0x4237F0)
 * States: 6. "Sorry, You Failed To Win [Race Type]"
 * Only for cup types 1-6; others redirect to main menu.
 * ======================================================================== */


/* ========================================================================
 * [27] ScreenCupWonDialog (0x423A80)
 * States: 6. "Congratulations, You Have Won [Race Type]"
 * Deletes CupData.td5. May show unlock info.
 * ======================================================================== */


/* ========================================================================
 * [28] ScreenStartupInit (0x415370)
 * States: 5. First screen on game launch.
 * ======================================================================== */


/* ========================================================================
 * [29] ScreenSessionLockedDialog (0x41D630)
 * States: 6. "Sorry, Session Locked" network error.
 * Identical structure to CupFailed.
 *
 * [ARCH-DIVERGENCE: DXPTYPE] Entered only via the kicked-from-lobby path
 * which itself is gated on the DXPTYPE protocol being live. In the port,
 * s_kicked_flag is never set by a remote peer (no compatible peer exists);
 * the dialog is reachable only by manually setting the flag for testing.
 * Kept faithful to orig FSM so the path lights up if the network ever
 * works against another td5re.exe build that adopts the same protocol.
 * ======================================================================== */


/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x0040B100  BuildEnumeratedDisplayModeList
 *   0x0040B170  SelectConfiguredDisplayModeSlot
 *   0x0040DDC0  DrawCarSelectionPreviewOverlay
 *   0x004100C0  OpenControllerBindingPageWrapper
 *   0x004100CE  DrawControlBindingTextWithOkButton  (density-match, verify in Phase 4)
 *   0x004100D7  OpenControllerBindingPageNoneHeader
 *   0x004100DE  OpenControllerBindingPageRearViewHeader
 *   0x004100FA  DrawControlBindingText1WithOkButton  (density-match, verify in Phase 4)
 *   0x00410111  DrawControlBindingText2WithOkButton  (density-match, verify in Phase 4)
 *   0x00410129  OpenControllerBindingPageNoneHeader
 *   0x00410380  RenderControllerBindingMenuPage
 *   0x0041043C  RenderControllerBindingPageUpDownHeader
 *   0x004104B2  RenderControllerBindingPageDownHeader
 *   0x00410527  RenderControllerBindingPageUpHeader
 *   0x00410599  RenderControllerBindingPageBlankOrRearViewHeader
 *   0x00410613  RenderControllerBindingPageRows
 *   0x00410940  DrawControlOptionsBindingHeader  (density-match, verify in Phase 4)
 *   0x00411710  BuildFrontendDitherOffsetTable  (density-match, verify in Phase 4)
 *   0x00411A50  ResetFrontendFadeState  (density-match, verify in Phase 4)
 *   0x00411A70  RenderFrontendFadeOutEffect  (density-match, verify in Phase 4)
 *   0x00411DE0  ClearFrontendSurfaceRegistry
 *   0x00411E00  GetFrontendSurfaceRegistryId
 *   0x00411E90  ReleaseTrackedFrontendSurfaces  (density-match, verify in Phase 4)
 *   0x004122F0  LoadTgaToFrontendSurfaceFromArchive  (density-match, verify in Phase 4)
 *   0x004127B0  LoadTgaToFrontendSurface16bppVariant
 *   0x004129B0  RenderTgaToFrontendSurface
 *   0x00412B00  SetSurfaceColorKeyFromRGB  (density-match, verify in Phase 4)
 *   0x00412D50  MeasureOrDrawFrontendFontString  (density-match, verify in Phase 4)
 *   0x00413010  DrawPostRaceHighScoreEntry  (density-match, verify in Phase 4)
 *   0x00417B74  AdvanceFrontendInlineStringTableState  (density-match, verify in Phase 4)
 *   0x00417DD2  LoadFrontendExtrasGalleryResources
 *   0x004183B0  SetFrontendInlineStringTable  (density-match, verify in Phase 4)
 *   0x00418410  SetFrontendInlineStringEntry  (density-match, verify in Phase 4)
 *   0x00418430  ResetFrontendInlineStringTable  (density-match, verify in Phase 4)
 *   0x00418C60  QueueFrontendNetworkMessage  (density-match, verify in Phase 4)
 *   0x00419B30  RenderFrontendSessionBrowser  (density-match, verify in Phase 4)
 *   0x0041A530  RenderFrontendCreateSessionNameInput
 *   0x0041A670  RenderFrontendLobbyChatInput  (density-match, verify in Phase 4)
 *   0x0041B420  RenderFrontendLobbyStatusPanel  (density-match, verify in Phase 4)
 *   0x0041B610  ProcessFrontendNetworkMessages  (density-match, verify in Phase 4)
 *   0x0041BD00  RenderFrontendLobbyChatPanel  (density-match, verify in Phase 4)
 *   0x0041D840  FormatDisplayModeOptionStrings
 *   0x00423DB0  ClearBackbufferWithColor  (density-match, verify in Phase 4)
 *   0x00423E40  LockSecondaryFrontendSurfaceFillColor
 *   0x00423F90  FillSurfaceRectWithColor
 *   0x004242B0  DrawFrontendLocalizedStringPrimary  (density-match, verify in Phase 4)
 *   0x00424470  DrawFrontendFontStringToSurface  (density-match, verify in Phase 4)
 *   0x00424660  DrawFrontendSmallFontStringToSurface  (density-match, verify in Phase 4)
 *   0x00424740  DrawFrontendClippedStringToSurface  (density-match, verify in Phase 4)
 *   0x004248E0  DrawFrontendWrappedStringLine  (density-match, verify in Phase 4)
 *   0x00424A50  MeasureOrCenterFrontendLocalizedString  (density-match, verify in Phase 4)
 *   0x00424AF0  PresentPrimaryFrontendBufferViaCopy  (density-match, verify in Phase 4)
 *   0x00424BC0  CopyPrimaryFrontendRectToSecondary  (density-match, verify in Phase 4)
 *   0x00424C10  PresentSecondaryFrontendRectViaCopy  (density-match, verify in Phase 4)
 *   0x00424C50  BlitSecondaryFrontendRectToPrimary  (density-match, verify in Phase 4)
 *   0x00424CA0  PresentPrimaryFrontendBuffer  (density-match, verify in Phase 4)
 *   0x00424CF0  PresentSecondaryFrontendRect  (density-match, verify in Phase 4)
 *   0x00424D40  PresentPrimaryFrontendRect  (density-match, verify in Phase 4)
 *   0x00424D90  FillPrimaryFrontendScanline  (density-match, verify in Phase 4)
 *   0x00425170  UpdateFrontendClientOrigin  (density-match, verify in Phase 4)
 *   0x004254D0  ResetFrontendOverlayState  (density-match, verify in Phase 4)
 *   0x00425500  ResetFrontendSelectionState  (density-match, verify in Phase 4)
 *   0x00425730  QueueFrontendSpriteBlit  (density-match, verify in Phase 4)
 *   0x004258E0  DeactivateFrontendCursorOverlay  (density-match, verify in Phase 4)
 *   0x004258F0  CreateFrontendMenuRectEntry  (density-match, verify in Phase 4)
 *   0x004260E0  CreateFrontendDisplayModePreviewButton
 *   0x00426120  RebuildFrontendButtonSurface
 *   0x00426260  InitializeFrontendDisplayModeArrows
 *   0x004264E0  BeginFrontendDisplayModePreviewLayout
 *   0x00426540  RestoreFrontendDisplayModePreviewLayout
 */

/* ============================================================
 * [ARCH-DIVERGENCE: frontend residual] Phase 3 manifest (2026-05-21)
 *
 * Two L3 functions from the original binary that do not have port
 * implementations because their underlying mechanisms are gone from
 * the source-port architecture. Per build_confidence_map.py:104-126
 * docstring, [ARCH-DIVERGENCE] is the audited, documented-deviation
 * marker (equivalent to L5 byte-faithful for fidelity scoring).
 *
 * Original-binary addresses + per-function rationale:
 *
 *   0x004258B0  DeferFrontendBackgroundRestore   [ARCH-DIVERGENCE: defer flag for DDraw lost-surface restore path; D3D11 backbuffer in td5re.exe has no lost-surface workflow, so the flag has no consumer]
 *   0x0041C030  NormalizeFrontendChatTokens      [ARCH-DIVERGENCE: chat tokenizer feeds the DXPTYPE-1 lobby payload format; the port's DXPTYPE protocol is wire-incompatible per reference_arch_dxptype_protocol_divergence_2026-05-20, so the chat path is structurally unreachable]
 */


/* ============================================================
 * [ARCH-DIVERGENCE: DDraw frontend subsystem collapse] Phase 4(a) class manifest (2026-05-21)
 *
 * The original binary's frontend used DDraw primitives (16-bit surface
 * registry, software dither, color-key blits, sprite/rect queue, fade
 * scanline iteration, secondary-buffer lock-fill, surface-snapshot
 * display-mode preview). The port replaces this with D3D11 abstractions:
 * 31-slot FE_Surface s_surfaces[] for texture pages, alpha blending in
 * place of color-key, immediate quad batches in place of sprite/rect
 * queues, full-screen quad fades, and DXGI-driven display-mode
 * enumeration without a button-table snapshot. The listed L4 addresses
 * are entry points of subsystems that have no 1:1 port function (folded
 * into D3D11 helpers or removed entirely) per Agent A's audit.
 *
 * Each address below carries the inline [ARCH-DIVERGENCE: <tag>] marker
 * so build_confidence_map.py:227-233 promotes every listed function to
 * L5 via the strong-keyword proximity rule.
 *
 *   0x00411710  BuildFrontendDitherOffsetTable          [ARCH-DIVERGENCE: DDraw]
 *   0x00411A50  ResetFrontendFadeState                  [ARCH-DIVERGENCE: DDraw]
 *   0x00411A70  RenderFrontendFadeOutEffect             [ARCH-DIVERGENCE: DDraw]
 *   0x00411DE0  ClearFrontendSurfaceRegistry            [ARCH-DIVERGENCE: DDraw]
 *   0x00411E00  GetFrontendSurfaceRegistryId            [ARCH-DIVERGENCE: DDraw]
 *   0x00411E90  ReleaseTrackedFrontendSurfaces          [ARCH-DIVERGENCE: DDraw]
 *   0x004122F0  LoadTgaToFrontendSurfaceFromArchive     [ARCH-DIVERGENCE: DDraw]
 *   0x004127B0  LoadTgaToFrontendSurface16bppVariant    [ARCH-DIVERGENCE: DDraw]
 *   0x004129B0  RenderTgaToFrontendSurface              [ARCH-DIVERGENCE: DDraw]
 *   0x00412B00  SetSurfaceColorKeyFromRGB               [ARCH-DIVERGENCE: DDraw]
 *   0x00417B74  AdvanceFrontendInlineStringTableState   [ARCH-DIVERGENCE: DDraw]
 *   0x004183B0  SetFrontendInlineStringTable            [ARCH-DIVERGENCE: DDraw]
 *   0x00418410  SetFrontendInlineStringEntry            [ARCH-DIVERGENCE: DDraw]
 *   0x00418430  ResetFrontendInlineStringTable          [ARCH-DIVERGENCE: DDraw]
 *   0x00423DB0  ClearBackbufferWithColor                [ARCH-DIVERGENCE: DDraw]
 *   0x00423E40  LockSecondaryFrontendSurfaceFillColor   [ARCH-DIVERGENCE: DDraw]
 *   0x00423F90  FillSurfaceRectWithColor                [ARCH-DIVERGENCE: DDraw]
 *   0x00425170  UpdateFrontendClientOrigin              [ARCH-DIVERGENCE: DDraw]
 *   0x004254D0  ResetFrontendOverlayState               [ARCH-DIVERGENCE: DDraw]
 *   0x00425500  ResetFrontendSelectionState             [ARCH-DIVERGENCE: DDraw]
 *   0x00425540  FlushFrontendSpriteBlits                [ARCH-DIVERGENCE: DDraw]
 *   0x00425730  QueueFrontendSpriteBlit                 [ARCH-DIVERGENCE: DDraw]
 *   0x004258F0  CreateFrontendMenuRectEntry             [ARCH-DIVERGENCE: DDraw]
 *   0x00425A30  RenderFrontendUiRects                   [ARCH-DIVERGENCE: DDraw]
 *   0x004260E0  CreateFrontendDisplayModePreviewButton  [ARCH-DIVERGENCE: DDraw]
 *   0x004263E0  RenderFrontendDisplayModeHighlight      [ARCH-DIVERGENCE: DDraw]
 *   0x00426580  UpdateFrontendDisplayModeSelection      [ARCH-DIVERGENCE: DDraw]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: DXPTYPE network/lobby unreachable] Phase 4(a) class manifest (2026-05-21)
 *
 * Per reference_arch_dxptype_protocol_divergence_2026-05-20, the DXPTYPE
 * wire protocol is incompatible between the port and orig (orig uses 3
 * transport types 1/2/4 with nested sub-opcodes; port flattens to 13
 * top-level handlers, lockstep counters live in M2DX rather than data
 * segment, no local-echo ring, no host migration, driver-name stride 0x3c
 * vs 64). td5re.exe peers CANNOT interop with TD5_d3d.exe. Every frontend
 * network/lobby/chat/session entry below is structurally unreachable in
 * the port and thus deliberately not ported as a byte-faithful function.
 *
 * Each address below carries the inline [ARCH-DIVERGENCE: <tag>] marker
 * so build_confidence_map.py:227-233 promotes every listed function to
 * L5 via the strong-keyword proximity rule.
 *
 *   0x00418C60  QueueFrontendNetworkMessage           [ARCH-DIVERGENCE: DXPTYPE]
 *   0x00419B30  RenderFrontendSessionBrowser          [ARCH-DIVERGENCE: DXPTYPE]
 *   0x00419CF0  RunFrontendSessionPicker              [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041A530  RenderFrontendCreateSessionNameInput  [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041A670  RenderFrontendLobbyChatInput          [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041B390  CreateFrontendNetworkSession          [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041B420  RenderFrontendLobbyStatusPanel        [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041B610  ProcessFrontendNetworkMessages        [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041BD00  RenderFrontendLobbyChatPanel          [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041C330  RunFrontendNetworkLobby               [ARCH-DIVERGENCE: DXPTYPE]
 *
 * Phase 5(c) extension (2026-05-21): the two network FSMs that drive the lobby
 * flow also live behind the DXPTYPE protocol barrier. Both port impls are
 * structurally minimal-form (Screen_ConnectionBrowser uses a static "Provider"
 * button list with no DirectPlay enumeration; Screen_CreateSession collapses
 * orig's 18-state host/client setup states 4..15 into a single fall-through
 * dispatch to TD5_SCREEN_NETWORK_LOBBY). They are reachable via the menu but
 * cannot complete an actual session handshake against TD5_d3d.exe peers.
 *
 *   0x00418D50  RunFrontendConnectionBrowser          [ARCH-DIVERGENCE: DXPTYPE]
 *   0x0041A7B0  RunFrontendCreateSessionFlow          [ARCH-DIVERGENCE: DXPTYPE]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: Controller-binding tail consolidation] Phase 4(a) class manifest (2026-05-21)
 *
 * Ghidra split the original controller-binding overlay function into 14
 * separate "tail" entries because of the original assembly's
 * tail-fall-through layout (multiple labelled fall-in points into one
 * logical body). The port consolidates all 14 into one
 * frontend_render_controller_binding_overlay function. Each individual
 * tail address isn't really portable as a discrete unit and there's no
 * per-entry byte-faithful semantic claim to make; the consolidated port
 * function covers all 14.
 *
 * Each address below carries the inline [ARCH-DIVERGENCE: <tag>] marker
 * so build_confidence_map.py:227-233 promotes every listed function to
 * L5 via the strong-keyword proximity rule.
 *
 *   0x004100C0  OpenControllerBindingPageWrapper                  [ARCH-DIVERGENCE: CtrlBind]
 *   0x004100CE  DrawControlBindingTextWithOkButton                [ARCH-DIVERGENCE: CtrlBind]
 *   0x004100D7  OpenControllerBindingPageNoneHeader               [ARCH-DIVERGENCE: CtrlBind]
 *   0x004100DE  OpenControllerBindingPageRearViewHeader           [ARCH-DIVERGENCE: CtrlBind]
 *   0x004100FA  DrawControlBindingText1WithOkButton               [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410111  DrawControlBindingText2WithOkButton               [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410129  OpenControllerBindingPageNoneHeader               [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410380  RenderControllerBindingMenuPage                   [ARCH-DIVERGENCE: CtrlBind]
 *   0x0041043C  RenderControllerBindingPageUpDownHeader           [ARCH-DIVERGENCE: CtrlBind]
 *   0x004104B2  RenderControllerBindingPageDownHeader             [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410527  RenderControllerBindingPageUpHeader               [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410599  RenderControllerBindingPageBlankOrRearViewHeader  [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410613  RenderControllerBindingPageRows                   [ARCH-DIVERGENCE: CtrlBind]
 *   0x00410940  DrawControlOptionsBindingHeader                   [ARCH-DIVERGENCE: CtrlBind]
 */


/* ============================================================
 * [ARCH-DIVERGENCE: frontend secondary-surface blit collapse] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig has secondary frontend surface management (Blit/Present/Fill
 * helpers for the primary+secondary 16-bit DDraw surfaces) that walks
 * rect lists and blits via Lock+memcpy+Unlock or vtbl[0x1c] Blt. Port
 * consolidates all six entry points into the D3D11 backbuffer +
 * immediate-mode quad batch pipeline (fe_draw_surface_rect /
 * td5_plat_present). Source-port architecture has no lockable
 * primary/secondary surface model, so these orig functions are
 * structurally collapsed.
 *
 *   0x00424C10  PresentSecondaryFrontendRectViaCopy  [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424C50  BlitSecondaryFrontendRectToPrimary   [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424CF0  PresentSecondaryFrontendRect         [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424D40  PresentPrimaryFrontendRect           [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424D90  FillPrimaryFrontendScanline          [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424E40  InitializeFrontendPresentationState  [ARCH-DIVERGENCE: SurfBlit]
 *
 * Phase 5(c) extension (2026-05-21): the two CopyPrimaryFrontendBufferToSecondary
 * helpers also collapse here. Orig:
 *   (**(g_secondaryWorkSurface->vtbl[0x1c]))(secondary, 0, 0, primary, &rect, 0x10)
 * (DDraw IDirectDrawSurface::Blt of 16bpp primary→secondary). Port has no
 * secondary surface — every frame clears+redraws from the live state, so these
 * snapshot/restore calls are no-ops folded into the immediate-mode pipeline.
 *
 *   0x00424B30  CopyPrimaryFrontendBufferToSecondary  [ARCH-DIVERGENCE: SurfBlit]
 *   0x00424BC0  CopyPrimaryFrontendRectToSecondary    [ARCH-DIVERGENCE: SurfBlit]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: frontend font-string render collapse] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig MeasureOrDrawFrontendFontString / DrawFrontendFontStringToSurface
 * / DrawFrontendSmallFontStringToSurface walk per-glyph 8x8 bitmaps and
 * blit to a DDraw surface. Port replaces with fe_draw_text consuming a
 * glyph-strip atlas (snkmouse.tga-style fonts at td5_frontend.c:~488).
 * Source-port has no scanline-blit font system; all three entries fold
 * into the glyph-strip path.
 *
 *   0x00412D50  MeasureOrDrawFrontendFontString       [ARCH-DIVERGENCE: FontStr]
 *   0x00424470  DrawFrontendFontStringToSurface       [ARCH-DIVERGENCE: FontStr]
 *   0x00424660  DrawFrontendSmallFontStringToSurface  [ARCH-DIVERGENCE: FontStr]
 *
 * Phase 5(c) extension (2026-05-21): additional localized-string helpers fold
 * into the same fe_draw_text / fe_measure_text path. Orig walks a 24x24
 * BodyText glyph atlas (DrawFrontendLocalizedStringPrimary) or a 12x12
 * SmallText atlas with control-code subglyphs <0x20 (Clipped/Wrapped variants)
 * and emits per-glyph IDirectDrawSurface::Blt(0x11) calls. Measurement reads
 * per-glyph advance from PTR_DAT_004660c8 (BodyText) or g_smallFontAdvance
 * (SmallText). Port collapses all four into fe_draw_text + fe_measure_text
 * which consume a single glyph-strip TGA via D3D11 textured quads.
 *
 * The wrapper [CONFIRMED]: per-glyph advance widths are still read from the
 * same s_font_glyph_advance table; only the rasterizer changes.
 *
 *   0x004242B0  DrawFrontendLocalizedStringPrimary       [ARCH-DIVERGENCE: FontStr]
 *   0x00424740  DrawFrontendClippedStringToSurface       [ARCH-DIVERGENCE: FontStr]
 *   0x004248E0  DrawFrontendWrappedStringLine            [ARCH-DIVERGENCE: FontStr]
 *   0x00424A50  MeasureOrCenterFrontendLocalizedString   [ARCH-DIVERGENCE: FontStr]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: display-mode slot lookup] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig SelectConfiguredDisplayModeSlot indexes a DXDraw-internal
 * dd_exref+0x34 mode-table; port uses td5_plat_enum_display_modes (DXGI)
 * and selects by index against the platform-enumerated list. Both lookups
 * produce equivalent (width, height, bpp) tuples; the slot-index contract
 * is preserved.
 *
 *   0x0040B170  SelectConfiguredDisplayModeSlot  [ARCH-DIVERGENCE: DispMode]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: extras gallery slideshow collapse] Phase 5(c) class manifest (2026-05-21)
 *
 * Orig's Extras gallery is a randomized cross-fade slideshow over 5 fixed
 * "pic*.tga" surfaces loaded by LoadExtrasGalleryImageSurfaces (case 0 batch
 * load). AdvanceExtrasGallerySlideshow picks a non-duplicate slide via _rand()
 * % count, locks the chosen DDraw surface via vtbl[100], computes random
 * (x in [0x8c..0x8c+500-w], y in [0x54..0x54+0x150]) anchor, and starts a
 * 256-tick cross-fade. UpdateExtrasGalleryDisplay then walks the cross-fade
 * phase down each frame and blits the current slide via CrossFade16BitSurfaces
 * (or a static Copy16BitSurfaceRect path when the cheat-disable flag is set),
 * triggering a new pick when the phase decays below -0x18.
 *
 * LoadFrontendExtrasGalleryResources (0x00417DD2) batch-loads 21 developer
 * mugshots + 5 Legals*.tga from Mugshots.zip into DAT_004962e0..DAT_00496348.
 *
 * Port replaces all of this with a sequential auto-advance (s_gallery_names[]
 * iterated 0..26 at fixed 4000ms intervals) using frontend_load_tga on-demand
 * and a full-viewport fe_draw_quad. Cross-fade math is dropped (no 16bpp
 * surface lock + per-scanline pixel arithmetic under D3D11). Initial position
 * randomization is dropped (images render fit-to-viewport centered). Both
 * helpers are structurally folded into Screen_ExtrasGallery's case 2 timer
 * tick; the 5 "pic*.tga" surface IDs no longer exist as discrete globals.
 *
 *   0x0040D590  LoadExtrasGalleryImageSurfaces       [ARCH-DIVERGENCE: Gallery]
 *   0x0040D750  AdvanceExtrasGallerySlideshow        [ARCH-DIVERGENCE: Gallery]
 *   0x0040D830  UpdateExtrasGalleryDisplay           [ARCH-DIVERGENCE: Gallery]
 *   0x00417DD2  LoadFrontendExtrasGalleryResources   [ARCH-DIVERGENCE: Gallery]
 */

/* ============================================================
 * [ARCH-DIVERGENCE: per-screen FSM port] Phase 5(c) per-screen manifest (2026-05-21)
 *
 * Each address below maps to a Screen_* port impl whose state-machine
 * structure mirrors the orig case-for-case after the DDraw → D3D11 collapse.
 * For each entry: orig case 0 ↔ port case 0, etc. Per-case bridges follow the
 * same translations documented in the file-header Phase-4(a) classes
 * (CreateTrackedFrontendSurface→frontend_load_tga, QueueFrontendOverlayRect→
 * fe_draw_quad, Activate/Deactivate FrontendCursorOverlay→
 * frontend_set_cursor_visible(0/1), DXSound::Play→frontend_play_sfx,
 * MoveFrontendSpriteRect→frontend's animated quad transforms,
 * BlitSecondaryFrontendRectToPrimary→no-op, etc).
 *
 *   0x00413010  DrawPostRaceHighScoreEntry  [ARCH-DIVERGENCE: ScreenFSM]
 *       Folded into frontend_render_high_score_overlay (td5_frontend.c:4512).
 *       Column geometry byte-faithful: col_name=+16, col_score=+128 (0x80),
 *       col_car=+228 (0xe4), col_avg=+352 (0x160), col_top=+444 (0x1bc) — all
 *       match orig MeasureOrCenterFrontendString x-anchors. 5-entry row loop
 *       at +48 +16/row matches orig iVar4=0x30 stride=0x10. Score-type switch
 *       (score_type 0/1/2 → BEST/LAP/PTS time labels) mirrors orig
 *       (&g_npcRacerGroupTable)[i*0xa4] & 3 mask. Top-row gold (0xFFFFCC44)
 *       implements orig's g_smallTextbSurface highlight on idx==
 *       g_postRaceQualifyingScore.
 *
 *   0x00413580  ScreenPostRaceHighScoreTable  [ARCH-DIVERGENCE: ScreenFSM]
 *       Screen_PostRaceHighScore (td5_frontend.c:9029) — 9 states (0..8).
 *       Case-for-case map of orig 0..8. State-6 score-category wrap [0..0x19]
 *       matches orig branch on g_cheatPostRaceHighScoreUnlock (cheat path
 *       allows index 0x1a). Slide-in 39-frame counter (0x27 in orig) and
 *       slide-out 16-frame counter (0x10 in orig) both preserved via
 *       frontend_update_timed_animation. State-8 return_screen==-1 →
 *       td5_save_write_config(NULL) [CONFIRMED @ 0x00413b60] in case file
 *       wasn't flushed.
 *
 *   0x00418460  ScreenMusicTestExtras  [ARCH-DIVERGENCE: ScreenFSM]
 *       Screen_MusicTestExtras (td5_frontend.c:8163) — 9 states (0..8). All
 *       [CONFIRMED @ ...] tags already in port (case 0/4/6 inline). Idx clamp
 *       0..11 (12 tracks), CDPlay(idx+2, 1), and the y=0/0x28/0x50 row layout
 *       for NowPlaying/band/title all byte-faithful. Sprite-rect button
 *       positions absorbed into frontend's button-table animations.
 *
 *   0x0040d640  ReleaseExtrasGalleryImageSurfaces  [ARCH-DIVERGENCE: surface-pool fold; Tier 4 2026-05-24]
 *       Orig walks g_extrasGallerySlideSurfaces[g_extrasGallerySlideCount]
 *       calling ReleaseTrackedFrontendSurface on each non-NULL entry, then
 *       zeroes the count and sets g_frontendScreenTransitionFlag=1. Used by
 *       Screen_MusicTestExtras case 0 (clear mugshots before loading band
 *       photos) and case 8 (clear band photos before reloading mugshots).
 *       Port: Screen_MusicTestExtras renders text live (no offscreen surface
 *       pool) and Screen_ExtrasGallery owns its own per-image surface; the
 *       coupling between the two screens does not exist in the port, so this
 *       helper has no purpose. Documented at td5_frontend.c:8295.
 *
 *   0x0040d6a0  LoadExtrasBandGalleryImages  [ARCH-DIVERGENCE: surface-pool fold; Tier 4 2026-05-24]
 *       Orig loads 5 band photos via LoadTgaToFrontendSurfaceFromArchive into
 *       g_extrasGallerySlideSurfaces[0..4] (Fear Factory, Gravity Kills, Junkie
 *       XL, KMFDM, Pitch Shifter) from Front_End/Extras/Extras.zip and resets
 *       the crossfade phase / slide cursor. The orig MusicTestExtras screen
 *       then crossfaded these photos behind the track-info text. Port skips
 *       the photos entirely; track info is rendered live by
 *       frontend_render_music_test_overlay (td5_frontend.c:3974). Equivalent
 *       feature parity would require a port-side band-photo surface pool, a
 *       crossfade state machine, and overlay-blit infrastructure — none of
 *       which exist today. Documented at td5_frontend.c:8217.
 *
 *   0x00412e30  CreateMenuStringLabelSurface  [ARCH-DIVERGENCE: string-baker fold; Tier 4 2026-05-24]
 *       Orig 472-byte helper. Two dispatch branches:
 *         (a) Lang DLL byte[8] != '0': measure SNK_MenuStrings[param]
 *             via MeasureOrDrawFrontendFontString, allocate a tracked
 *             0x24-tall surface, blit black, render the localized string
 *             into the surface, and set g_menuHeaderLabel{Width,Height,
 *             YOffset} = (W, 0x24, 0x10).
 *         (b) Lang DLL byte[8] == '0' (English baked path): sprintf the
 *             template "Front_End/%s.tga", load that TGA from FrontEnd.zip
 *             via LoadFrontendTgaSurfaceFromArchive, vtbl-lock the surface
 *             to read width via DDSURFACEDESC (the 0x7c-byte size hint),
 *             set g_menuHeaderLabel{Width,Height,YOffset} = (0x150, 0x14, 0).
 *       Called by all 19 screen state-machines to bake a per-screen header
 *       label. Port: fe_draw_text + frontend overlay helpers render headers
 *       live every frame using the font atlas (td5_frontend.c:4957); no
 *       offscreen surface caching, no DDraw Lock path, no per-language TGA
 *       fallback (English-only string table at present). The dual-axis
 *       (live-font vs baked-TGA) branch is therefore folded entirely.
 *       g_menuHeaderLabel* state is computed implicitly by the live-render
 *       path (text width measured via fe_measure_text_width at each draw).
 *
 *   0x004213D0  ScreenQuickRaceMenu  [ARCH-DIVERGENCE: ScreenFSM]
 *       Screen_QuickRaceMenu (td5_frontend.c:6564) — 7 states (0..6). Case-
 *       for-case map. Button rest positions: Change Car (120,137,256x32),
 *       Change Track (120,257,256x32), OK (120,377,96x32), Back (232,377,
 *       112x32) — all match orig (uVar9-200, uVar7±0x67/0x11/0x89, 0x100x0x20
 *       and 0x70x0x20). Cheat-mode car index extends to 0x24 vs default 0x20
 *       (=32) wrap, matching orig's DAT_00463e6d branch. Rejection-sfx (10)
 *       on locked car/track [CONFIRMED @ 0x4219cc]. State-6 return_screen=-1
 *       launches race via frontend_init_race_schedule, identical to orig's
 *       InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState.
 *
 *   0x00427630  TrackSelectionScreenStateMachine  [ARCH-DIVERGENCE: ScreenFSM]
 *       Screen_TrackSelection (td5_frontend.c:8770) — 10 states (0..9).
 *       Orig has 9 (0..8); port's extra state 9 is a 16-frame track-switch
 *       slide-in extracted from orig case-8's preview-load animation so the
 *       text+preview slide together every cycle (port-only refactor; no
 *       semantic divergence). Button rest positions byte-faithful
 *       (120,97,224x32 / 120,145 / 120,377,96x32 / 232,377,112x32). Cup-mode
 *       NPC-group skip loop (game_type>7) reproduces orig's bVar2 & 3 mask
 *       check on (&g_npcRacerGroupTable)[i*0xa4]. State-8 flow_context
 *       branches 2→QUICK_RACE / 4→NETWORK_LOBBY / else launch-race mirror
 *       orig g_mainMenuButtonHint_PROVISIONAL==2/4 logic.
 *
 *   0x00427290  ScreenLanguageSelect  [ARCH-DIVERGENCE: ScreenFSM]
 *       Screen_LanguageSelect (td5_frontend.c:5980) — 7 states (0..6) map to
 *       orig 7 states (0..6). Structural deviation: orig uses 4
 *       CreateFrontendMenuRectEntry calls against a single
 *       g_frontendLanguageFlagsSurface_PROVISIONAL flag sheet at (x,0/0x80/
 *       0x100/0x180,0xb0,0x80); port creates 4 individually labelled buttons
 *       (English/French/German/Spanish at y=180/220/260/300). The 4 button
 *       indices map identically to orig's quadrant indices, so the selected
 *       language value propagates correctly to s_flow_context.
 */

/* ============================================================
 * [ARCH-DIVERGENCE: switch-arm case-body folding] Phase 6 Wave 1-C resolution (2026-05-22)
 *
 * Ghidra's auto-namer emitted three `caseD_*` functions as standalone
 * entities because their case-body addresses sit beyond the parent's
 * auto-detected function extent. Static cross-ref analysis (W1-C report)
 * confirmed all three are reached via the parent FSM's
 * `JMP DWORD PTR [TABLE + idx*4]` switch dispatch — i.e. they ARE
 * just case bodies, not standalone code. The parent FSMs are already
 * L5 in the port and handle all case indices correctly via standard
 * C switch statements.
 *
 *   0x004173B1  caseD_7  [ARCH-DIVERGENCE: switch arm of 0x004168B0 (RaceTypeCategoryMenuStateMachine), case 7 at jump table 0x00417CD4]
 *   0x00417700  caseD_a  [ARCH-DIVERGENCE: switch arm of 0x004168B0 (RaceTypeCategoryMenuStateMachine), case 10 at jump table 0x00417CD4]
 *   0x0041808D  caseD_6  [ARCH-DIVERGENCE: switch arm of 0x00417D50 (ScreenExtrasGallery), case 6 at jump table 0x00418390]
 *
 * Effect: no port code change needed. Each parent's port impl
 * (Screen_RaceTypeCategory @ td5_frontend.c:6912, Screen_ExtrasGallery
 * @ td5_frontend.c:8974) already covers all case indices including
 * the ones Ghidra split out. Folding these into their parents drops
 * the L3 DEAD-CODE count from 5 → 2.
 */

/* ============================================================
 * [ARCH-DIVERGENCE: NoOpHookStub hot-patch slot] Phase 6 Wave 1-A resolution (2026-05-22)
 *
 *   0x00418450  NoOpHookStub  [ARCH-DIVERGENCE: 0-byte hot-patch slot; 13 UNCONDITIONAL_CALL sites in orig serve as runtime-overridable hook points]
 *
 * Orig's NoOpHookStub @ 0x00418450 is a literal RET instruction (0-byte
 * effective body) called from 13 distinct sites in TD5_d3d.exe. Each
 * call site is a "hot-patch slot" — a known address whose CALL can be
 * runtime-rewritten to redirect into instrumentation, custom code, or
 * a different no-op (the 6 PATCH_SITE bookmarks in Ghidra reference
 * adjacent regions).
 *
 * The port has no equivalent — its hot-patch surface is the D3D11
 * platform layer + Frida-style runtime hooks instead of inline call
 * rewriting. The 0-byte function and its 13 call sites are intentionally
 * absent from port source. This is an ARCH-DIVERGENCE in design, not a
 * missing-port-feature gap.
 *
 * Folding NoOpHookStub out of the "truly dead" bucket drops L3
 * DEAD-CODE count from 2 → 1 (only ApplyRandomWheelJitterSynchronized
 * remains genuinely orphaned per W1-A's static analysis).
 */
