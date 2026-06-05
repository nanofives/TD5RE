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
#include "td5re.h"
#include "td5_snk_strings.h"   /* byte-exact SNK_ labels baked from Language.dll */
#include "td5_credits.h"       /* SNK_CreditsText array + dev mugshot map (Extras scroll) */
#include "td5_vectorui.h"      /* public VectorUI surface (HUD reuses these primitives) */
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

/* Forward declarations for functions used before definition */
static void frontend_init_font_metrics_from_pixels(const uint8_t *pixels, int w, int h);
static void frontend_init_font_metrics_default(void);
static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1);

/* ========================================================================
 * Forward declarations -- screen functions (30 entries)
 * ======================================================================== */

static void Screen_LocalizationInit(void);        /* [0]  0x4269D0 */
static void Screen_PositionerDebugTool(void);      /* [1]  0x415030 */
static void Screen_AttractModeDemo(void);           /* [2]  0x4275A0 */
static void Screen_LanguageSelect(void);            /* [3]  0x427290 */
static void Screen_LegalCopyright(void);            /* [4]  0x4274A0 */
static void Screen_MainMenu(void);                  /* [5]  0x415490 */
static void Screen_RaceTypeCategory(void);           /* [6]  0x4168B0 */
static void Screen_QuickRaceMenu(void);              /* [7]  0x4213D0 */
static void Screen_ConnectionBrowser(void);          /* [8]  0x418D50 */
static void Screen_SessionPicker(void);              /* [9]  0x419CF0 */
static void Screen_CreateSession(void);              /* [10] 0x41A7B0 */
static void Screen_NetworkLobby(void);               /* [11] 0x41C330 */
static void Screen_OptionsHub(void);                 /* [12] 0x41D890 */
static void Screen_GameOptions(void);                /* [13] 0x41F990 */
static void Screen_ControlOptions(void);             /* [14] 0x41DF20 */
static void Screen_SoundOptions(void);               /* [15] 0x41EA90 */
static void Screen_DisplayOptions(void);             /* [16] 0x420400 */
static void Screen_TwoPlayerOptions(void);           /* [17] 0x420C70 */
static void Screen_ControllerBinding(void);          /* [18] 0x40FE00 */
static void Screen_MusicTestExtras(void);            /* [19] 0x418460 */
static void Screen_CarSelection(void);               /* [20] 0x40DFC0 */
static void Screen_TrackSelection(void);             /* [21] 0x427630 */
static void Screen_ExtrasGallery(void);              /* [22] 0x417D50 */
static void Screen_PostRaceHighScore(void);          /* [23] 0x413580 */
static void Screen_RaceResults(void);                /* [24] 0x422480 */
static void Screen_PostRaceNameEntry(void);          /* [25] 0x413BC0 */
static void Screen_CupFailed(void);                  /* [26] 0x4237F0 */
static void Screen_CupWon(void);                     /* [27] 0x423A80 */
static void Screen_StartupInit(void);                /* [28] 0x415370 */
static void Screen_SessionLocked(void);              /* [29] 0x41D630 */
static void Screen_MultiplayerLobby(void);           /* [30] PORT ENHANCEMENT 2026-06 */
static void Screen_LanMenu(void);                    /* [31] S10 net-play UX */
static void Screen_DirectConnect(void);              /* [32] S10 net-play UX */
static void Screen_NetNickname(void);                /* [33] S10 net-play UX */

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
};

/* ========================================================================
 * Module-level state
 * ======================================================================== */

static TD5_ScreenIndex s_current_screen;
static int  s_inner_state;              /* g_frontendInnerState   */
static int  s_anim_tick;                /* g_frontendAnimFrameCounter (0x49522C) */
static int  s_return_screen;            /* g_returnToScreenIndex  */
static int  s_start_race_request;       /* 0x495248               */
static int  s_start_race_confirm;       /* 0x49524C               */
static int  s_attract_idle_counter;     /* g_attractModeIdleCounter */
static uint32_t s_attract_idle_timestamp;
static int  s_attract_demo_active;      /* g_attractModeDemoActive @ 0x495254 */

/* -----------------------------------------------------------------------
 * Screen [27] CupWon — persistent unlock counts for overlay renderer
 * Set in state 0 via td5_save_apply_cup_unlocks_ex; read in states 4-5.
 * [CONFIRMED @ 0x423A80]: original DAT_00494bb0 = car count, DAT_00494bb4 = track count.
 * ----------------------------------------------------------------------- */
static int s_cup_won_car_count;
static int s_cup_won_track_count;

/* -----------------------------------------------------------------------
 * Screen [19] MusicTestExtras — state shared between state machine + render
 * ----------------------------------------------------------------------- */
static int s_music_test_track_idx;      /* 0..11; mirrors g_selectedCdTrackIndex @ 0x465e14 */

/* -----------------------------------------------------------------------
 * fe_draw_quad render log — written when [Logging] FrontendDraw=1
 * ----------------------------------------------------------------------- */
static FILE *s_fe_draw_log_fp;
static int   s_fe_draw_log_frame;

/* Band names and song titles for tracks 0..11.
 * [CONFIRMED @ Ghidra 0x418460]: PTR_s_GRAVITY_KILLS_00465e1c (band) and
 * PTR_s_FALLING_00465e58 (title) pointer tables, decoded from binary data. */
static const char * const k_music_test_band[12] = {
    "GRAVITY KILLS", "KMFDM",        "PITCHSHIFTER", "PITCHSHIFTER",
    "JUNKIE XL",     "FEAR FACTORY", "FEAR FACTORY", "GRAVITY KILLS",
    "KMFDM",         "PITCHSHIFTER", "PITCHSHIFTER", "PITCHSHIFTER"
};
static const char * const k_music_test_title[12] = {
    "FALLING",           "ANARCHY",     "GENIUS",            "WYSIWYG",
    "DEF BEAT",          "21ST CENTURY","GENETIC BLUEPRINT",  "FALLING (DUB)",
    "MEGALOMANIAC (DUB)","GENIUS (DUB)","MICROWAVED (DUB)",   "WYSIWYG (DUB)"
};

/* Live-rendered overlay strings — updated whenever the track selection or
 * "Now Playing" state changes.  The original drew into offscreen DDraw
 * surfaces; the port renders these strings every frame via the UI-rect pass.
 * Format: track-label  = "%d. %s" (track# 1-based + band name)   [CONFIRMED @ 0x465f74]
 *         now-playing panel rows at y=0/0x28/0x50                 [CONFIRMED @ 0x418571]
 */
static char s_music_test_track_label[64];   /* e.g. "1. GRAVITY KILLS" */
static char s_music_test_now_band[64];      /* band name of currently playing track */
static char s_music_test_now_title[64];     /* song title of currently playing track */
static int  s_music_test_playing_set;       /* 1 once CDPlay has been called */
/* Music Test album cover art: 5 band covers + the 12-track->5-band LUT.
 * [CONFIRMED @ 0x40d6a0 LoadExtrasBandGalleryImages load order:
 *  0=Fear Factory,1=Gravity Kills,2=Junkie XL,3=KMFDM,4=PitchShifter;
 *  LUT @0x465e4c; drawn at (0x76,0x8c)=(118,140) by UpdateExtrasGalleryDisplay@0x40d830.] */
static int  s_band_cover_surface[5];
static const int k_music_track_to_band[12] = { 1,3,4,4,2,0,0,1,3,4,4,4 };
/* The band whose cover is shown. [CONFIRMED @0x418460] orig keys the cover on
 * g_attractCdTrackCandidate, which is set ONLY when SELECT is pressed — so the album
 * art (and now-playing panel) reflect the PLAYED track, not the one being previewed
 * with ◄►. Track 0 plays at entry, so this starts at 0. */
static int  s_music_attract_track = 0;
static int  s_input_ready;              /* DAT_004951e8            */
static int  s_button_index;             /* currently pressed button */
static int  s_arrow_input;              /* DAT_0049b690 arrow direction */
/* [PORT ENHANCEMENT 2026-06] gamepad frontend-nav bits this frame (bit4 A,
 * bit5 B); cached so frontend_check_escape() can read B without re-polling. */
static uint32_t s_fe_gamepad_nav;
/* [PORT ENHANCEMENT 2026-06] the "active controller" driving the menus: whichever
 * device last gave input (0 = keyboard, >=1 = enumerated joystick). It becomes
 * the driver (player 0's device) for single-player races. */
static int s_active_menu_device = 0;
/* [PORT ENHANCEMENT 2026-06] hot-swap: re-enumerate joysticks periodically so a
 * controller plugged in at the main menu is picked up without leaving the screen. */
static unsigned s_fe_rescan_tick = 0;
static uint32_t s_screen_entry_timestamp;
static uint32_t s_anim_start_ms = 0;
static uint32_t s_anim_elapsed_ms = 0;
static int  s_anim_complete = 0;
static float s_anim_t = 0.0f;          /* continuous 0..1 for smooth button position */
static TD5_ScreenIndex s_previous_screen = (TD5_ScreenIndex)-1;

/* Context / flow tracking (DAT_004962d4) */
static int  s_flow_context;

/* Game type / race configuration */
static int  s_selected_game_type;       /* g_selectedGameType     */
static int  s_race_rule_variant;        /* gRaceRuleVariant       */
static int  s_race_within_series;       /* g_raceWithinSeriesIndex */
static int  s_cup_unlock_tier;          /* DAT_004962a8           */

/* Two-player mode flag (DAT_004962a0) */
static int  s_two_player_mode;
/* [PORT ENHANCEMENT 2026-06] Multiplayer Options split-layout picker state
 * (replaces the original Two Player Options split-on/off toggle + CATCHUP level,
 * both removed). The layout is chosen per local-human-count; see mp_split_layouts.
 *   s_mp_layout_sel        — index into mp_split_layouts(num_human_players)
 *   s_mp_missing_content[k] — stub content id for the k-th empty grid cell
 *                             (the cell-content feature itself is deferred). */
static int  s_mp_layout_sel = 0;
static int  s_mp_missing_content[2] = { 0, 0 };
/* Dynamic button-index bookkeeping for the rebuilt Multiplayer Options rows
 * (the row set changes with the player count, so buttons are rebuilt live). */
static int  s_mp_btn_players   = -1;
static int  s_mp_btn_catchup   = -1;   /* [S05 2026-06-04] CATCHUP toggle row */
static int  s_mp_btn_layout    = -1;
static int  s_mp_btn_missing[2] = { -1, -1 };
static int  s_mp_btn_nickname  = -1;    /* S10: edit net-play nickname (below split rows) */
static int  s_mp_btn_ok        = -1;
static int  s_mp_missing_count = 0;
static int  s_mp_layout_optcount = 1;

/* [PORT ENHANCEMENT 2026-06] MULTIPLAYER press-to-join lobby + sequential car
 * select. join order = player number; each joined player records its device. */
static int      s_mp_flow         = 0;     /* 1 = multiplayer setup flow active */
static int      s_mp_joined_count = 0;     /* players who have joined the lobby */
static int      s_mp_join_device[TD5_MAX_HUMAN_PLAYERS]; /* device idx per joined player */
static uint32_t s_mp_join_prev    = 0;     /* lobby join-scan mask last frame (edge) */
static int      s_mp_car_player    = 0;    /* which player is picking (sequential car select) */
static int      s_mp_player_car[TD5_MAX_HUMAN_PLAYERS];   /* per-player chosen car */
static int      s_mp_player_paint[TD5_MAX_HUMAN_PLAYERS]; /* per-player chosen paint */

/* ScreenLocalizationInit bootstrap control [CONFIRMED @ 0x4269D0 g_attractModeControlEnabled]:
 * 0 = first entry (run full init), 1 = re-entry (skip init, go to menu),
 * 2 = resume-cup re-entry (go to RACE_RESULTS with skip_display=1) */
static int  s_attract_mode_ctrl;

/* Car selection state */
static int  s_selected_car;             /* DAT_0048f31c / DAT_0048f364 */
static int  s_selected_paint;           /* DAT_0048f308 / DAT_0048f368 */
static int  s_selected_config;          /* DAT_0048f370            */
static int  s_selected_transmission;    /* DAT_0048f338 / DAT_0048f378 */
static int  s_car_preview_overlay;      /* DAT_0048f360            */
static int  s_car_roster_max;           /* max car index for current mode */
static int  s_car_roster_min;           /* min car index for current mode */
static int  s_p2_car;                   /* DAT_00463e08            */
static int  s_p2_paint;
static int  s_p2_config;
static int  s_p2_transmission;

/* Dedicated PLAYER (pass-1) car storage for the drag-race 2-pass CarSelect.
 * The original keeps the player car in g_selectedCarIndex @0x0048f364 and never
 * overwrites it during the opponent pass (the user cycles a SEPARATE scratch
 * register). The port collapsed scratch + player slot into s_selected_car, so
 * the pass-2 navigation clobbered the player car -> both racers ended up with
 * the later (opponent) car. s_p1_car preserves the player's pass-1 choice across
 * the opponent pass. [CONFIRMED root cause @ 0x40dfc0 case-0/case-0x18, 0x40dac0] */
static int  s_p1_car;
static int  s_p1_paint;

/* Drag-race CarSelect 2-pass counter [CONFIRMED @ DAT_0048f380].
 * Original binary gates this on g_selectedGameType == 7 which is DRAG RACE in the
 * original; the port's game_type convention has Drag Race = 9. 0 = picking car 1,
 * 1 = picking car 2. Reset on Back, on race entry, and on screen leave. */
static int  s_drag_carselect_pass;

/* Track selection state */
static int  s_selected_track;           /* DAT_004a2c90            */
static int  s_attract_track;            /* random track for attract demo; never overwrites s_selected_track */
static int  s_track_direction;          /* DAT_004a2c98: 0=fwd, 1=bwd */
static int  s_track_max;               /* max track index for current mode */
/* Quick Race player setup (infra to later replace the Two-Player menu).
 * Only the Quick Race screen writes these; all other launch flows leave the
 * defaults (1 human + 5 AI = legacy single-race grid). See g_td5.num_* . */
static int  s_num_human_players = 1;    /* 1..TD5_MAX_RACER_SLOTS            */
static int  s_num_ai_opponents  = 5;    /* 0..(TD5_MAX_RACER_SLOTS-1)        */
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
#define QR_BTN_OK         6
#define QR_BTN_BACK       7

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
static int  s_score_category_index;    /* DAT_00497a68: current track in score table */

#define FE_MAX_DISPLAY_MODES 64
static TD5_DisplayMode s_display_modes[FE_MAX_DISPLAY_MODES];
static char            s_display_mode_names[FE_MAX_DISPLAY_MODES][32];
static int             s_display_mode_count;
static int             s_display_mode_index;
static int             s_display_fog_enabled = 1;
static int             s_display_speed_units;
static int             s_display_camera_damping = 5;
/* [S01 Display options 2026-06-04] new rows: window mode (0=fullscreen,
 * 1=windowed, 2=borderless), vsync (0/1), show-fps (0/1). Mirrored from
 * g_td5.ini on screen entry, applied live on change, persisted on OK. */
static int             s_display_window_mode = 1;
static int             s_display_vsync       = 1;
static int             s_display_show_fps    = 1;
static int             s_game_option_laps = 0;
static int             s_game_option_checkpoint_timers = 1;
static int             s_game_option_traffic = 1;
static int             s_game_option_cops = 1;
static int             s_game_option_difficulty = 1;
static int             s_game_option_dynamics = 0;
static int             s_game_option_collisions = 1;
static int             s_sound_option_sfx_mode;
static int             s_sound_option_sfx_volume = 80;
static int             s_sound_option_music_volume = 80;

/* Car roster size. The original game has 37 cars (0-36). The source port
 * appends 39 ported Test Drive 6 cars at indices 37-75 (see s_car_zip_paths
 * and td5_asset.c). TD5_BASE_CAR_COUNT gates anything that mirrors the original
 * save/unlock structure (only the 37 originals are tracked there); TD5_CAR_COUNT
 * sizes the port-side roster tables. */
#define TD5_BASE_CAR_COUNT 37
#define TD5_CAR_COUNT      76

/* Lock tables (simplified inline representation) */
static uint8_t s_car_lock_table[TD5_CAR_COUNT];    /* DAT_00463e4c (0-36); 37-75 = TD6, always unlocked */
static uint8_t s_track_lock_table[37];  /* DAT_004668B0 (orig 26); 26-30 = migrated TD6 P2P slots */
static int  s_total_unlocked_cars;      /* DAT_00463e0c */
static int  s_total_unlocked_tracks;    /* DAT_00466840 */
static int  s_cheat_unlock_all;         /* DAT_00496298 */

/* Network state */
static int  s_network_active;           /* g_networkSessionActive / DAT_004962bc */
/* --- S10 net-play: explicit connection modes (LAN / Direct-IP) --- */
static char s_net_direct_ip[64];        /* "ip" or "ip:port" entry buffer (Direct join) */
static int  s_nickname_from_mpopts;     /* nickname screen entered from Multiplayer Options */
/* Main-menu EXIT confirm dialog: button-pool indices of the YES / NO! buttons,
 * recorded when the dialog is built (state 5) so the handler (state 6) dispatches
 * by index instead of by label text — the SNK labels are "YES"/"NO!", which never
 * matched the old strcmp(..,"Yes")/strcmp(..,"No") so EXIT did nothing. */
static int  s_exit_confirm_yes_idx = -1;
static int  s_exit_confirm_no_idx  = -1;
/* --- S10b: lobby options modal (host) + join-password prompt --- */
static int  s_lobby_modal;              /* 0=closed, 1=OPTIONS modal open */
static int  s_lobby_modal_armed;        /* 1 once the opening Enter is released */
static int  s_lobby_max_players = 6;    /* modal: max players (2..6) */
static char s_lobby_password[32];       /* modal: host join password (also reused for join prompt) */
static int  s_net_join_pending_ui;      /* awaiting JOIN_ACK before entering lobby */
static uint32_t s_net_join_wait_start;  /* tick when the join wait began (timeout) */
static int  s_net_session_sel;          /* SESSION_PICKER cursor: 0=host, 1..N=join */
static int  s_launching_net_race;       /* set by the lobby before init_race_schedule */
/* [Network] config (seeded from td5re.ini in frontend_init; see td5_save). */
static int  s_net_cfg_game_port   = 37050;   /* [Network] GamePort */
static int  s_net_cfg_enable_upnp = 1;       /* [Network] EnableUPnP (Direct host) */
static int  s_kicked_flag;              /* DAT_00497328 */
static int  s_lobby_action;             /* DAT_0049722c */
static int  s_dialog_mode;              /* DAT_00496350 */
static int  s_per_slot_status[6];       /* DAT_00496980[6] */
static int  s_config_received[6];       /* DAT_00497262[6] */
static int  s_participant_flags[6];     /* DAT_0049725c[6] */
static int  s_race_active_flag;         /* DAT_00497324 */
static int  s_chat_msg_count;           /* DAT_00496408 */
static int  s_chat_dirty;              /* DAT_0049640c */
static uint32_t s_last_poll_timestamp;  /* DAT_004968a8 */
static int  s_text_input_state;         /* DAT_004969d0 */
static char s_chat_input_buffer[64];    /* DAT_004972cc */

/* Race results state */
static int  s_results_button;           /* DAT_00497a64 */
static int  s_results_rerace_flag;      /* DAT_00497a78 */
static int  s_results_cup_complete;     /* DAT_00497a70 */
/* P7 PANEL fix — sprite-rect slide offset for screen 24 results panel.
 * [CONFIRMED @ 0x00422480 cases 7..10 + 0xB] Original animates the panel
 * surface (DAT_0049628c) via QueueFrontendOverlayRect with a per-state
 * x-coordinate formula. Port has no MoveFrontendSpriteRect / sprite-rect
 * array; instead we accumulate a render-side x offset that overlays panel_x
 * in frontend_render_race_results_overlay. Reset to 0 on state 6 entry. */
static int  s_results_panel_slide_x;
static int  s_results_panel_slide_dir;   /* +1 = right (next), -1 = left (prev) */
static int  s_results_skip_display;     /* DAT_00497a74 */

/* View Race Data sentinel: when the user clicks "View Race Data" in state
 * 0x0F, we re-enter screen 24 via td5_frontend_set_screen — but state 0's
 * cup-fail early-route and state 3's table-skip gate both check
 * td5_game_slot_is_finished(0) / companion_2, which can be false if the
 * player DNF'd or quit early. Orig relied on actor.slot._808_4_ to detect
 * "race data exists", a field the port doesn't materialize. This flag is
 * set in the View Race Data dispatch and cleared after the state-3 gate
 * passes, forcing the table to display even on a partial race. */
static int  s_results_view_data_request;

/* Race snapshot for re-race */
static int  s_snap_car, s_snap_paint, s_snap_trans, s_snap_config;
/* [FIX 2026-06-05 race-again-opponent-count] Snapshot the opponent count too,
 * so "Race Again" (and S15's pause-menu RESTART RACE, which reads this same
 * snapshot) reruns with the SAME field size instead of falling back to the
 * legacy 5-opponent default. Defaults to -1 = "no snapshot captured yet". */
static int  s_snap_num_ai_opponents = -1;

/* Post-race name entry state (Screen [25]) */
static int32_t s_post_race_score;       /* DAT_004951d0: player's score for qualification */
static int     s_score_insert_pos;      /* 0-4: position in 5-entry table where insert goes */
/* Snapshotted at NAME_ENTRY case 0 (race-end transition) so case 4 can
 * insert non-zero values even if the actor pool has been torn down by
 * the time we reach the insert step. Without these the inserted entry's
 * AVG/TOP kph columns show 0 (user-reported 2026-05-26). */
static int32_t s_post_race_top_speed;
static int32_t s_post_race_avg_speed;
static int  s_snap_opp_car, s_snap_opp_paint, s_snap_opp_trans, s_snap_opp_config;

/* Masters roster (type 5): 15 random car slots, 6 marked AI */
static int  s_masters_roster[15];
static int  s_masters_roster_flags[15]; /* 0=available, 1=AI, 2=taken */

/* Cup series schedule tables (from TD5_d3d.exe 0x464098, stride 0x10, sentinel 0x63=99).
 * Original formula @ 0x410E8E: entry = *(base + raceWithinSeries + gameType*0x10 + 0x14)
 * with base = 0x464084 → row0 = 0x464098. Rows are byte arrays; port widens to int.
 * Indexed here by (game_type - 1), so [0] = Championship (GT=1). */
static const int s_cup_schedules[][13] = {
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
static int  s_mouse_x, s_mouse_y;
static int  s_prev_mouse_x = -1;
static int  s_prev_mouse_y = -1;
static int  s_mouse_clicked;
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

/* Fade state */
static int      s_fade_active;
static int      s_fade_progress;     /* 0..255 */
static int      s_fade_direction;    /* 1 = in, -1 = out */
static int      s_fade_table_index;
/*
 * Fade overlay color (RGB only, alpha is driven by s_fade_progress).
 * [CONFIRMED @ 0x411750 InitFrontendFadeColor]: original stores
 *   DAT_00494fc4 = param_1 >> 3 & 0x1f1f1f  (packed B/G/R channel levels)
 * We store the raw ARGB word and extract RGB in the renderer.
 */
static uint32_t s_fade_color;        /* packed 0x00RRGGBB from caller */
static int  s_gallery_pic_index;
static int  s_gallery_pic_surface;
static int  s_gallery_visited_mask;
static int  s_credit_mugshot_surf[K_CREDIT_MUGSHOT_COUNT]; /* dev photos, lazy-loaded (0=none) */
static uint32_t s_credits_start_ms;  /* scroll-reel start timestamp */
static int  s_language_bg_surface = 0;   /* LanguageScreen.tga 640x480 bg (ScreenLanguageSelect) */
static int  s_language_flag_surface = 0; /* Language.tga 176x512 = 4 stacked 176x128 flag tiles */

/* Background gallery slideshow (LoadExtrasGalleryImageSurfaces / UpdateExtrasGalleryDisplay)
 * pic1-5.tga from Extras.zip cycle as a semi-transparent overlay during frontend navigation. */
typedef struct { int width; int height; } BgGalImg;
static BgGalImg s_bg_gallery[5];
static int   s_bg_gal_loaded;
static int   s_bg_gal_current;
static int   s_bg_gal_blend;
static float s_bg_gal_x, s_bg_gal_y;

static int  s_control_options_surface;
/* [PORT REWORK 2026-06-05 / S15] s_sound_icon_surface (Controllers.tga SFX-mode
 * icon strip) removed along with the SFX Mode row on the sound-options screen. */
static int  s_sound_volumebox_surface = 0;  /* VolumeBox.tga   (volume bar background) */
static int  s_sound_volumefill_surface = 0; /* VolumeFill.tga  (volume bar fill)       */
static int  s_split_screen_surface = 0;     /* SplitScreen.tga (Two Player layout preview)    */
static int  s_joypad_icon_surface = 0;      /* JoypadIcon.tga   (64x32 gamepad icon)   */
static int  s_joystick_icon_surface = 0;    /* JoystickIcon.tga (64x32 joystick icon)  */
static int  s_keyboard_icon_surface = 0;    /* KeyboardIcon.tga (64x32 keyboard icon)  */
static int  s_nocontroller_surface = 0;     /* NoControllerText.tga (376x20 warning)   */
static int  s_car_preview_prev_surface;
static int  s_car_preview_next_surface;
static int  s_car_preview_change_loaded;  /* state-11 load-once guard (a missing
                                           * preview, e.g. a TD6 car with no carpic,
                                           * returns 0 — without this the slide
                                           * transition would re-load forever). */

/* ---- Screen [14] ControlOptions revamp state (PORT ENHANCEMENT 2026-06) ---- */
/* Which player (0-based) the Control Options screen is currently configuring,
 * and the live selectable range (recomputed on entry from the connected device
 * count — "hot-swappable"). */
static int      s_ctrl_opts_player      = 0;
static int      s_ctrl_opts_max_players = 1;

/* ---- Screen [18] ControllerBinding persistent state ---- */
/* [CONFIRMED @ 0x40FE00 / DAT_004974b8]: which player is being configured */
static int      s_ctrl_player        = 0;
/* [CONFIRMED @ 0x40FE00 / DAT_00490b94]: device source index (0=kbd/1=pad/2=stick) */
static int      s_ctrl_input_source  = 0;
/* [PORT ENHANCEMENT 2026-06] per-button remap: which action row the Configure
 * screen has selected, and whether it is currently capturing that action. */
static int      s_ctrl_sel_action    = 0;
static int      s_ctrl_capturing     = 0;
/* [PORT ENHANCEMENT 2026-06] "REMAP ALL" sequential mode: capture every action
 * one by one (the original's sequential flow, re-added as an option). */
static int      s_ctrl_remap_all     = 0;
/* Capture is two-phase: 0 = waiting for the device to return to NEUTRAL (so a
 * held input from the previous bind / the confirm press isn't re-captured), then
 * 1 = armed and listening for one fresh input. */
static int      s_ctrl_capture_armed = 0;
/* [CONFIRMED @ 0x40FE00 / DAT_00464054]: 16-byte scancode capture buffer */
static uint8_t  s_ctrl_kb_scancodes[16];
/* [PORT ENHANCEMENT 2026-06] keyboard state snapshot at capture-begin, so a key
 * already held when remap starts (e.g. the Enter that confirmed the row) is
 * ignored — only a fresh rising-edge press is accepted. */
static uint8_t  s_ctrl_capture_kb_snapshot[256];
/* [PORT ENHANCEMENT 2026-06] per-action joystick binding being edited on the
 * Configure screen: 10 codes (button/axis/trigger) for the selected player. */
static uint32_t s_ctrl_action_bind[TD5_MAX_HUMAN_PLAYERS][TD5_JSBIND_ACTIONS];

/* Action label strings for keyboard capture prompt (SNK_ControlText slots 0-9)
 * [CONFIRMED @ 0x40FE00 case 0x19]: index = s_ctrl_kb_slot (0..9), iterated
 *   via SNK_ControlText[i*0x10], loop stops at slot==10.
 * [CONFIRMED]: strings read from LANGUAGE.DLL SNK_ControlText @ 0x100075E0
 *   (stride 0x10, idx 0..9); default scancodes baked at g_keyboardScanCodeTable
 *   0x00464054 = {cb cd c8 d0 10 9d 1e 2c 14 2d}. Index 10 @ 0x10007680 = "NONE"
 *   is a sentinel, not iterated.
 * Faithful port of the original action list (was previously guessed/shifted —
 * had no "Steer"/"Gas"/"NOS"/"Camera", missed HANDBRAKE, and merged steer L/R). */
static const char * const k_ctrl_action_labels[TD5_JSBIND_ACTIONS] = {
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

#define FE_MAX_SURFACES    31
#define FE_SURFACE_PAGE_BASE 900  /* texture pages 900-931 reserved for frontend */

typedef struct {
    int in_use;
    int tex_page;
    int width, height;
    char source_name[128];
    char source_archive[128];
    char png_path[256];         /* resolved PNG path for recovery (empty = ZIP fallback) */
} FE_Surface;

typedef struct {
    char *buffer;
    int capacity;
    int caret;
    uint32_t blink_tick;
    int confirm_state;
} FE_TextInputContext;

static FE_TextInputContext s_text_input_ctx;
static char s_create_session_name[64];
static char s_post_race_name[32];
static char s_cheat_key_history[32];

static FE_Surface s_surfaces[FE_MAX_SURFACES];
static int s_white_tex_page = -1;
static int s_background_surface = 0;
static int s_carsel_bg_surface = 0;     /* unused — background inherited from RaceMenu.tga via s_background_surface */
static int s_carsel_fill_surface = 0;  /* 1x1 solid blue pixel for car preview area fill */
static int s_carsel_bar_surface = 0;
static int s_carsel_curve_surface = 0;
static int s_carsel_topbar_surface = 0;
static int s_graphbars_surface = 0;
static int s_car_preview_surface = 0;
/* Body-only paint overlay (carpicpaint) for TD6 cars — grayscale body, rest
 * transparent; drawn MODULATEd by the paint colour over the gray carpic.
 * Lazily (re)loaded per car in the render path. s_paint_active: the picked
 * colour persists on the preview (panel-open shows it live too); it's set on
 * CONFIRM (closing the panel) and cleared on car change / screen entry, so an
 * un-painted car shows neutral gray and a painted one keeps its colour through
 * the confirm slide and after. */
static int s_paint_overlay_surface = 0;
static int s_paint_overlay_car = -1;
static int s_paint_active = 0;
static char s_car_spec[17][48]; /* config.nfo fields (0-16) for stats sub-screen */
static int  s_car_spec_car = -1; /* which car index is currently cached */
static int s_track_preview_surface = 0;
static int s_track_switch_tick = 16; /* 0-15 = animating in, 16 = settled */

/* Track-preview start/finish dot markers, generated by
 * re/tools/track_preview_render.py into re/assets/tracks/trak_markers.dat
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
static int s_font_page = -1;
/* ---- Vector (MSDF) BodyText font — resolution-independent frontend text ----
 * BodyText_msdf.png is generated offline by re/tools/build_msdf_font.py: the
 * SAME logical 10-col x 23-row grid as BodyText, each cell holding a 3-channel
 * signed distance field instead of a bitmap glyph. Because the grid matches,
 * the existing UV math (u0=col/10, v0=row/23, ...) is reused verbatim; only the
 * texture page + pixel shader + sampler differ. Drawn via ps_msdf (median+
 * smoothstep) with a LINEAR_CLAMP sampler so it stays crisp at any resolution.
 * Gated by [Frontend] VectorUI; -1/NULL => fall back to the bitmap path. */
static int s_msdf_font_page = -1;
static ID3D11PixelShader *s_ps_msdf = NULL;
#define SHARED_PAGE_FONT_MSDF 970   /* free page (frontend uses 888-955) */

/* In-race HUD font SDF (VectorUI): a distance-field version of the original
 * tpage5 FONT glyphs at the SAME 256x256 layout, so the HUD keeps its original
 * typeface but renders crisp at any resolution via ps_msdf. Generated offline
 * by re/tools/build_hud_font_sdf.py. -1 => HUD falls back to the bitmap font. */
static int s_hudfont_sdf_page = -1;
#define SHARED_PAGE_HUDFONT_SDF 982   /* free page (cursor MSDF is 981) */

/* Pause-menu font SDF (VectorUI): distance-field version of the tpage12
 * PAUSETXT glyphs at the same 256x256 layout. -1 => bitmap PAUSETXT fallback. */
static int s_pausefont_sdf_page = -1;
#define SHARED_PAGE_PAUSEFONT_SDF 983

/* Procedural neon rounded-rect (frontend buttons/frames) — VectorUI only.
 * ps_roundrect evaluates an analytic rounded-rect SDF per pixel (crisp glow at
 * any resolution); s_rr_cb feeds it per-button geometry/colour via constant
 * buffer register b1. Must match cbuffer RoundRectParams in ps_roundrect.hlsl. */
static ID3D11PixelShader *s_ps_roundrect = NULL;
static ID3D11PixelShader *s_ps_arrow = NULL;   /* selector ◄► triangle SDF */
static ID3D11PixelShader *s_ps_cursor = NULL;  /* mouse pointer (SDF: white outline + purple fill) */
static int s_cursor_msdf_page = -1;
#define SHARED_PAGE_CURSOR_MSDF 981  /* free page (titles use 972..980) */
static ID3D11Buffer      *s_rr_cb = NULL;
typedef struct {
    float size_px[2];   /* button w,h in screen px */
    float border[2];    /* border thickness px: x = left/right, y = top/bottom */
    float radii[4];     /* outer corner radii px: TL, TR, BL, BR */
    float mid[4];       /* border gradient: lightest (middle of band) */
    float inner[4];     /* border gradient: inner-edge colour */
    float outer[4];     /* border gradient: outer-edge colour (darkest) */
    float fill[4];      /* interior rgb, a = interior alpha (0 = transparent) */
} FE_RoundRectParams;   /* 96 bytes, matches cbuffer RoundRectParams */

/* Procedural analog gauge dial (in-race HUD speedo + tach) — VectorUI only.
 * ps_gauge evaluates an analytic dial SDF (face disc + outer ring + radial
 * ticks + optional redline arc + pivot) per pixel; s_gauge_cb feeds geometry
 * + colours via constant-buffer register b1. Owned here (with the other
 * VectorUI shaders) and exposed to the HUD via td5_vui_gauge (td5_vectorui.h).
 * Must match cbuffer GaugeParams in ps_gauge.hlsl. */
static ID3D11PixelShader *s_ps_gauge = NULL;
static ID3D11Buffer      *s_gauge_cb = NULL;
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
static int s_smallfont_page = -1;
/* Vector (SDF) SmallText atlas — same 21x11 grid, distance field per cell.
 * Generated by re/tools/build_msdf_font.py (smalltext_white_on_black.png).
 * Reuses the same ps_msdf shader + s_ps_msdf as BodyText. */
static int s_smallfont_msdf_page = -1;
#define SHARED_PAGE_SMALLFONT_MSDF 971  /* free page (BodyText MSDF is 970) */
#define SMALLFONT_PAGE   893
#define SMALLFONT_CELL   12
#define SMALLFONT_COLS   21
#define SMALLFONT_TEX_W  252.0f   /* 21 * 12 */
#define SMALLFONT_TEX_H  132.0f   /* 11 * 12 */
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

static int s_cursor_tex_page = -1;  /* page 896: snkmouse.tga cursor */
static int s_cursor_w = 0, s_cursor_h = 0;
static int s_buttonbits_tex_page = -1; /* page 897: ButtonBits.tga gradient */
static int s_buttonbits_w = 0, s_buttonbits_h = 0;
static int s_buttonlights_tex_page = -1; /* page 895: ButtonLights.tga indicator */
static int s_buttonlights_w = 0, s_buttonlights_h = 0;
static int s_arrowbuttonz_tex_page = -1; /* page 894: ArrowButtonz.tga 12x36 sprite sheet */
static int s_title_tex_page[TD5_SCREEN_COUNT];
static int s_title_tex_w[TD5_SCREEN_COUNT];
static int s_title_tex_h[TD5_SCREEN_COUNT];
/* Parallel SDF title pages (VectorUI): same word-strip art as a distance field
 * so the big yellow headers stay crisp at any resolution. 0 = not loaded. */
static int s_title_msdf_page[TD5_SCREEN_COUNT];

/* Forward declarations for functions used before their definitions */
static int frontend_load_tga(const char *name, const char *archive);
static int frontend_load_tga_colorkey(const char *name, const char *archive,
                                      int page_override, int *w_out, int *h_out,
                                      TD5_ColorKeyMode colorkey);
static int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey);
static int frontend_track_level_exists(int track_index);
static void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label);
static int  frontend_track_is_circuit(int track_slot);
static void frontend_update_laps_button_visibility(int laps_btn_idx);
static uint8_t s_font_glyph_advance[96];
static const uint8_t k_font_glyph_advance_default[96] = {
    8,  9, 10, 14, 14, 24, 21,  9,  8,  9, 10, 14,
   10, 10,  8, 13, 17, 10, 17, 16, 17, 16, 16, 17,
   17, 17,  8, 10, 12, 13, 11, 13, 18, 19, 15, 14,
   17, 13, 12, 19, 17,  8, 12, 18, 13, 23, 17, 20,
   16, 20, 17, 14, 14, 17, 18, 24, 19, 18, 17,  9,
   13,  9, 12, 15,  9, 16, 16, 12, 15, 15, 11, 15,
   15,  8,  8, 15,  8, 21, 15, 16, 15, 16, 12, 12,
   11, 15, 16, 22, 17, 16, 14,  9,  6, 10, 16, 14,
};

/* Original font: BodyText.tga, 10 chars per row, 24x24 cells (from Ghidra FUN_00424560)
 * Atlas is 240x552 (8bpp with palette, red color key for transparency).
 * DAT_0049626c. */
#define FONT_COLS 10
#define FONT_CELL 24
#define FONT_GLYPH_PADDING 2
#define FONT_TEX_W (FONT_COLS * FONT_CELL)  /* 240 */
#define FONT_TEX_H 552  /* actual BodyText.tga height: 23 rows of 24px */

/* Dedicated texture pages for shared assets (never freed on screen change):
 * 899 = white fallback
 * 898 = BodyText.tga (font)
 * 897 = ButtonBits.tga (gradient source)
 * 896 = SnkMouse.TGA (cursor)
 * 931-960 = cached per-screen frontend title TGAs
 */
#define SHARED_PAGE_WHITE     899
#define SHARED_PAGE_FONT      898
#define SHARED_PAGE_BUTTONBITS 897
#define SHARED_PAGE_CURSOR    896
#define SHARED_PAGE_BTNLIGHTS 895
#define SHARED_PAGE_ARROWBTNZ 894  /* ArrowButtonz.tga 12x36 sprite sheet */
#define SHARED_PAGE_BG_GALLERY 888 /* 5 pages 888-892: background slideshow pic1-5.tga */
#define SHARED_PAGE_MIN       888  /* lowest shared page -- don't clear below this */
#define FE_TITLE_PAGE_BASE    931
#define FE_TITLE_MSDF_PAGE_BASE 972  /* parallel SDF title pages 972..980 */

static void frontend_note_activity(void) {
    s_attract_idle_counter = 0;
    s_attract_idle_timestamp = td5_plat_time_ms();
}

static int frontend_surface_is_background_like(int w, int h) {
    return (w >= 640 || (w >= 320 && h >= 400));
}

static int frontend_find_surface_by_source(const char *name, const char *archive) {
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
static const char *s_car_zip_paths[TD5_CAR_COUNT] = {
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
static const uint8_t s_track_schedule_to_tga_index[37] = {
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

static void frontend_begin_timed_animation(void) {
    s_anim_start_ms = td5_plat_time_ms();
    s_anim_elapsed_ms = s_anim_start_ms - s_screen_entry_timestamp;
    s_anim_tick = 0;
    s_anim_t = 0.0f;
}

static float frontend_update_timed_animation(int max_tick, uint32_t duration_ms) {
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

static const char *frontend_get_title_tga_for_screen(TD5_ScreenIndex screen) {
    switch (screen) {
    case TD5_SCREEN_MAIN_MENU: return "MainMenuText.TGA";
    case TD5_SCREEN_RACE_TYPE_MENU: return "RaceMenuText.TGA";
    case TD5_SCREEN_QUICK_RACE: return "QuickRaceText.tga";
    /* All six options screens share the "OptionsText.tga" menu-header (CreateMenuStringLabelSurface(6)).
     * [FIXED 2026-06-01: Game/Control/Sound/Display/TwoPlayer Options were missing their title entirely
     * — the port returned NULL here, so no header drew, while the original draws one on all of them.] */
    case TD5_SCREEN_OPTIONS_HUB:
    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS: return "OptionsText.tga";
    case TD5_SCREEN_CAR_SELECTION: return "SelectCarText.tga";
    case TD5_SCREEN_TRACK_SELECTION: return "TrackSelectText.TGA";
    /* Name Entry shares the High Scores header (CreateMenuStringLabelSurface(7)) — it's the
     * qualifying-high-score entry screen. [FIXED 2026-06-02: was NULL → no title drawn.] */
    case TD5_SCREEN_HIGH_SCORE:
    case TD5_SCREEN_NAME_ENTRY: return "HighScoresText.TGA";
    case TD5_SCREEN_RACE_RESULTS: return "ResultsText.tga";
    case TD5_SCREEN_CONNECTION_BROWSER:
    case TD5_SCREEN_SESSION_PICKER:
    case TD5_SCREEN_CREATE_SESSION:
    case TD5_SCREEN_NETWORK_LOBBY:
    case TD5_SCREEN_SESSION_LOCKED: return "NetPlayText.TGA";
    default: return NULL;
    }
}

static int frontend_get_title_page_for_screen(TD5_ScreenIndex screen) {
    switch (screen) {
    case TD5_SCREEN_MAIN_MENU: return FE_TITLE_PAGE_BASE + 0;
    case TD5_SCREEN_RACE_TYPE_MENU: return FE_TITLE_PAGE_BASE + 1;
    case TD5_SCREEN_QUICK_RACE: return FE_TITLE_PAGE_BASE + 2;
    case TD5_SCREEN_OPTIONS_HUB:
    case TD5_SCREEN_GAME_OPTIONS:
    case TD5_SCREEN_CONTROL_OPTIONS:
    case TD5_SCREEN_SOUND_OPTIONS:
    case TD5_SCREEN_DISPLAY_OPTIONS:
    case TD5_SCREEN_TWO_PLAYER_OPTIONS: return FE_TITLE_PAGE_BASE + 3; /* shared OptionsText page */
    case TD5_SCREEN_CAR_SELECTION: return FE_TITLE_PAGE_BASE + 4;
    case TD5_SCREEN_TRACK_SELECTION: return FE_TITLE_PAGE_BASE + 5;
    case TD5_SCREEN_HIGH_SCORE:
    case TD5_SCREEN_NAME_ENTRY: return FE_TITLE_PAGE_BASE + 6; /* shared HighScoresText page */
    case TD5_SCREEN_RACE_RESULTS: return FE_TITLE_PAGE_BASE + 7;
    case TD5_SCREEN_CONNECTION_BROWSER:
    case TD5_SCREEN_SESSION_PICKER:
    case TD5_SCREEN_CREATE_SESSION:
    case TD5_SCREEN_NETWORK_LOBBY:
    case TD5_SCREEN_SESSION_LOCKED: return FE_TITLE_PAGE_BASE + 8;
    default: return -1;
    }
}

static int frontend_ensure_title_texture(TD5_ScreenIndex screen) {
    const char *entry = frontend_get_title_tga_for_screen(screen);
    int page = frontend_get_title_page_for_screen(screen);

    if (!entry || page < 0) return 0;
    if (screen >= 0 && screen < TD5_SCREEN_COUNT && s_title_tex_page[screen] == page) return 1;
    /* Title TGAs have black backgrounds in the PNG files → use black colorkey.
     * [CONFIRMED]: all *Text.png files in re/assets/frontend/ have corners (0,0,0). */
    if (!frontend_load_tga_colorkey(entry, "Front End/frontend.zip", page,
                                    &s_title_tex_w[screen], &s_title_tex_h[screen],
                                    TD5_COLORKEY_BLACK)) {
        return 0;
    }
    s_title_tex_page[screen] = page;

    /* Parallel SDF title (VectorUI): load re/assets/frontend/<base>_msdf.png to a
     * parallel page so the header can render crisp at any resolution. The strips
     * are flat menu-font yellow, so the SDF shader's diffuse colour carries the
     * tint (see the title draw). Failure leaves s_title_msdf_page[screen]=0 and
     * the draw falls back to the bitmap strip. */
    if (g_td5.ini.vector_ui && s_ps_msdf &&
        screen >= 0 && screen < TD5_SCREEN_COUNT && s_title_msdf_page[screen] == 0) {
        char base[128], sdf_path[256];
        size_t n = 0;
        while (entry[n] && entry[n] != '.' && n < sizeof(base) - 1) { base[n] = entry[n]; n++; }
        base[n] = 0;
        snprintf(sdf_path, sizeof(sdf_path), "re/assets/frontend/%s_msdf.png", base);
        void *pixels = NULL;
        int mw = 0, mh = 0;
        if (td5_asset_load_png_to_buffer(sdf_path, TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
            int mpage = FE_TITLE_MSDF_PAGE_BASE + (page - FE_TITLE_PAGE_BASE);
            if (td5_plat_render_upload_texture(mpage, pixels, mw, mh, 2)) {
                s_title_msdf_page[screen] = mpage;
                TD5_LOG_I(LOG_TAG, "Title SDF loaded: %s page=%d %dx%d", sdf_path, mpage, mw, mh);
            }
            free(pixels);
        } else {
            TD5_LOG_W(LOG_TAG, "Title SDF not found: %s (falls back to bitmap)", sdf_path);
        }
    }
    return 1;
}

static int frontend_load_car_preview_surface(int car_index, int paint_index) {
    char entry[32];
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return 0;
    snprintf(entry, sizeof(entry), "CarPic%d.tga", paint_index & 3);
    /* Car preview PNGs have a blue (0,0,90) background — key it out so the
     * car silhouette floats over the CarSel background.
     * [CONFIRMED]: all carpic*.png in re/assets/cars have corners (0,0,90,255). */
    return frontend_load_surface_keyed(entry, s_car_zip_paths[car_index], TD5_COLORKEY_BLUE88);
}

/* TD6 body-only paint overlay (carpicpaint0.png): grayscale body, everything
 * else already transparent (alpha), so load with NO colour key — the PNG alpha
 * is the mask. Paint-independent, so a single frame. Returns <=0 if absent (e.g.
 * a TD5 car, or a TD6 car whose preview predates carpicpaint generation). */
static int frontend_load_car_paint_overlay_surface(int car_index) {
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return 0;
    return frontend_load_surface_keyed("CarPicPaint0.tga", s_car_zip_paths[car_index],
                                       TD5_COLORKEY_NONE);
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
static int frontend_load_tga_ck(const char *name, const char *archive, TD5_ColorKeyMode colorkey) {
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
        if (slot >= 0 && slot < FE_MAX_SURFACES &&
            frontend_surface_is_background_like(s_surfaces[slot].width, s_surfaces[slot].height)) {
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
    if (slot < 0) { free(pixels); return 0; }

    int page = FE_SURFACE_PAGE_BASE + slot;
    if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
        int is_background = frontend_surface_is_background_like(w, h);
        s_surfaces[slot].in_use = 1;
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
static int frontend_load_tga(const char *name, const char *archive) {
    return frontend_load_tga_ck(name, archive, TD5_COLORKEY_NONE);
}

/**
 * Load a TGA to a dedicated texture page with red color key transparency.
 * Red pixels (R=255,G=0,B=0 in source; after BGRA swap: B=0,G=0,R=255,A=255)
 * become fully transparent (alpha=0).
 */
static int frontend_load_tga_colorkey(const char *name, const char *archive,
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

/* Load a TGA/PNG into a surface slot with a caller-specified color key.
 * colorkey = TD5_COLORKEY_BLACK: keys near-black pixels (track previews)
 * colorkey = TD5_COLORKEY_BLUE88: keys (0,0,~90) blue pixels (car previews)
 * colorkey = TD5_COLORKEY_NONE: no keying (opaque blit) */
static int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey) {
    int existing_handle;

    const char *bare_name = name;
    const char *slash = strrchr(name, '/');
    if (slash) bare_name = slash + 1;
    slash = strrchr(bare_name, '\\');
    if (slash) bare_name = slash + 1;

    const char *real_archive = archive;
    if (strstr(archive, "FrontEnd.zip") || strstr(archive, "frontend.zip"))
        real_archive = "Front End/frontend.zip";

    existing_handle = frontend_find_surface_by_source(bare_name, real_archive);
    if (existing_handle > 0) return existing_handle;

    /* Try PNG from re/assets first */
    void *pixels = NULL;
    int w = 0, h = 0;
    char png_path[256];

    if (td5_asset_resolve_png_path(bare_name, real_archive, png_path, sizeof(png_path)))
        td5_asset_load_png_to_buffer(png_path, colorkey, &pixels, &w, &h);

    if (!pixels) {
        TD5_LOG_W(LOG_TAG, "LoadSurfaceKeyed failed: %s from %s ck=%d (no PNG found)", name, archive, (int)colorkey);
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (!s_surfaces[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { free(pixels); return 0; }

    int page = FE_SURFACE_PAGE_BASE + slot;
    if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
        s_surfaces[slot].in_use = 1;
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
        TD5_LOG_I(LOG_TAG, "LoadSurfaceKeyed OK: %s → slot=%d page=%d %dx%d ck=%d", bare_name, slot, page, w, h, (int)colorkey);
        return slot + 1;
    }
    free(pixels);
    return 0;
}

static void frontend_release_surface(int handle) {
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

static const char *frontend_get_track_name(int track_index) {
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

static int frontend_current_car_index(void) {
    if (s_selected_game_type == 5 &&
        s_selected_car >= 0 &&
        s_selected_car < (int)(sizeof(s_masters_roster) / sizeof(s_masters_roster[0])))
        return s_masters_roster[s_selected_car];
    return s_selected_car;
}

/* Highest selectable ORIGINAL (TD5) car index, inclusive, for normal cycling.
 * Mirrors the original unlock cap. s_total_unlocked_cars is a COUNT, so the
 * inclusive index is count-1 (this is the off-by-one that produced an
 * "UNKNOWN CAR" entry at index == count). Police (33-36) are excluded from
 * normal cycling by capping at 32. */
static int frontend_td5_car_cap_inclusive(void) {
    if (s_network_active) return TD5_BASE_CAR_COUNT - 1; /* 36 */
    if (s_cheat_unlock_all) return 32;
    int cap = s_total_unlocked_cars - 1;
    if (cap > 32) cap = 32;
    if (cap < 0)  cap = 0;
    return cap;
}

/* TD6 cop cars cp1..cp4 (Jaguar/Charger/Mustang/Cerbera police) sit at roster
 * indices 37 + TD6_NEW_CODES{9..12} = 46..49. Like the TD5 police (33-36) they
 * are NOT player-selectable and cannot be painted — they belong to Cop Chase. */
#define TD6_COP_FIRST 46
#define TD6_COP_LAST  49
static int frontend_car_is_cop(int i) {
    return (i >= 33 && i <= 36) || (i >= TD6_COP_FIRST && i <= TD6_COP_LAST);
}

/* Is car index i reachable by normal car cycling for the current game type?
 *   Cop Chase (type 8): ONLY the cop cars (TD5 33-36 + TD6 cp1-4 = 46-49).
 *   Otherwise:          [0..td5_cap] unlocked non-police TD5 + [37..75] TD6,
 *                       with the cop cars excluded.
 * The gap (td5_cap+1 .. 36) — locked TD5 cars + police — is skipped. */
static int frontend_car_selectable(int i) {
    if (i < 0 || i >= TD5_CAR_COUNT) return 0;
    if (s_selected_game_type == 8)                  /* Cop Chase: cops only */
        return frontend_car_is_cop(i);
    if (frontend_car_is_cop(i)) return 0;           /* cops excluded everywhere else */
    if (i >= TD5_BASE_CAR_COUNT) return 1;          /* TD6 */
    return i <= frontend_td5_car_cap_inclusive();   /* unlocked TD5 (police excluded by cap) */
}

/* Step `cur` by `delta` within [lo,hi], skipping unreachable indices so TD6
 * cars (37-75) are cyclable but locked TD5 cars / police are not. For ranges
 * that don't reach the TD6 block (era cup, Masters, Cop Chase) it is a plain
 * wrap. Never returns an out-of-range index, so "UNKNOWN CAR" can't appear. */
static int frontend_car_cycle_step(int cur, int delta, int lo, int hi) {
    int span = hi - lo + 1;
    if (span <= 0) return cur;
    for (int n = 0; n < span; n++) {
        cur += delta;
        if (cur < lo) cur = hi;
        if (cur > hi) cur = lo;
        if (hi < TD5_BASE_CAR_COUNT) return cur;    /* no TD6/police gap in this range */
        if (frontend_car_selectable(cur)) return cur;
    }
    return cur;
}

/* True once a TD6 (ported) car is the active selection — drives the paint
 * color selector vs. the TD5 paint-arrows behaviour. */
static int frontend_car_is_td6(int car_index) {
    return car_index >= TD5_BASE_CAR_COUNT && car_index < TD5_CAR_COUNT;
}

/* TD6 car that may be repainted: every ported car EXCEPT the cp1-4 cop cars
 * (police liveries are fixed). Gates the colour selector + the preview tint. */
static int frontend_car_paintable(int car_index) {
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
static int frontend_car_has_paint(int car_index) {
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
static const uint32_t s_td6_palette[] = {   /* 0xRRGGBB predefined quick picks */
    0xFF0000, 0xFF6000, 0xFFC000, 0xC8FF00, 0x10C010, 0x00C0C0,
    0x1078FF, 0x1010FF, 0x8000FF, 0xFF20C0, 0xFFFFFF, 0xC8C8C8,
    0x686868, 0x101010, 0x884400, 0xFFD040,
};
#define TD6_PALETTE_N ((int)(sizeof(s_td6_palette) / sizeof(s_td6_palette[0])))

/* Layout in 640x480 canvas coords. When the panel is open the whole button
 * column is compressed (CAR/PAINT shift up, Stats/Auto/OK/Back move below the
 * panel) so OK/Back don't sit too low — see frontend_apply_color_panel_layout.
 * The list/map sit between the (raised) PAINT row and the Stats row. */
#define TD6_CP_LIST_X    46
#define TD6_CP_LIST_Y    226
#define TD6_CP_SW        19      /* predefined swatch size */
#define TD6_CP_GAP        2
#define TD6_CP_COLS       8
#define TD6_CP_MAP_X     46
#define TD6_CP_MAP_Y     272
#define TD6_CP_MAP_W    168
#define TD6_CP_MAP_H     46
#define TD6_CP_MAP_ROWS   6     /* keyboard grid rows over the color map */
#define TD6_CP_GRID_ROWS (2 + TD6_CP_MAP_ROWS)  /* 2 swatch rows + map rows */

static int s_color_panel_visible = 0;
/* Unified 2D cursor over the picker: rows 0-1 = predefined swatches (8 cols),
 * rows 2.. = the color map as a grid. All 4 arrows move it; the color under it
 * is applied live; the mouse sets it by clicking a swatch / map cell. */
static int s_color_cur_col = 0;
static int s_color_cur_row = 0;

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
static uint32_t td6_map_color(float u, float v) {
    float sat, val;
    if (v < 0.5f) { sat = v * 2.0f; val = 1.0f; }
    else          { sat = 1.0f;     val = 1.0f - (v - 0.5f) * 2.0f; }
    return td6_hsv_to_rgb(u, sat, val);
}
/* Color at grid cell (col,row): rows 0-1 = predefined swatches; rows 2.. = map. */
static uint32_t td6_cursor_color(int col, int row) {
    if (row < 2) {
        int idx = row * TD6_CP_COLS + col;
        if (idx < 0) idx = 0;
        if (idx >= TD6_PALETTE_N) idx = TD6_PALETTE_N - 1;
        return s_td6_palette[idx];
    }
    float u = (float)col / (float)(TD6_CP_COLS - 1);
    float v = (float)(row - 2) / (float)(TD6_CP_MAP_ROWS - 1);
    return td6_map_color(u, v);
}
/* Move the cursor to the predefined swatch matching the current color (if any),
 * so reopening the panel highlights the active color. */
static void frontend_color_panel_sync_index(void) {
    for (int k = 0; k < TD6_PALETTE_N; k++)
        if (s_td6_palette[k] == (uint32_t)g_td5.ini.td6_paint_color) {
            s_color_cur_row = k / TD6_CP_COLS;
            s_color_cur_col = k % TD6_CP_COLS;
            return;
        }
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

/* Check whether level data for a given track index is present on disk.
 * Checks both the zip archive and the loose extracted directory. */
static int frontend_track_level_exists(int track_index) {
    char path[64];
    int level_num;
    if (track_index < 0) return 1; /* -1 = random, always "valid" */
    level_num = td5_asset_level_number(track_index);
    snprintf(path, sizeof(path), "level%03d.zip", level_num);
    if (td5_plat_file_exists(path)) return 1;
    /* Also check for extracted loose-file directory (re/assets/levels/levelNNN/STRIP.DAT) */
    snprintf(path, sizeof(path), "re/assets/levels/level%03d/STRIP.DAT", level_num);
    return td5_plat_file_exists(path);
}

/* Advance track by delta (+1 or -1), skipping indices whose level zips are absent.
 * Stops after one full revolution to avoid an infinite loop when no tracks exist. */
static void frontend_cycle_track(int delta, int track_min, int track_max) {
    int start = s_selected_track;
    int attempts = track_max - track_min + 1;
    while (attempts-- > 0) {
        s_selected_track += delta;
        if (s_selected_track < track_min) s_selected_track = track_max - 1;
        if (s_selected_track >= track_max) s_selected_track = track_min;
        if (frontend_track_level_exists(s_selected_track)) return;
    }
    /* No available track found -- restore original selection */
    s_selected_track = start;
}

static void frontend_load_selected_car_preview(void) {
    int car_index = frontend_current_car_index();
    if (s_car_preview_surface > 0) {
        frontend_release_surface(s_car_preview_surface);
        s_car_preview_surface = 0;
    }
    s_car_preview_surface = frontend_load_car_preview_surface(car_index, s_selected_paint);
    /* Invalidate the paint-overlay cache so it RELOADS with the preview. On
     * car-select ENTRY (incl. returning from a race) the car index is unchanged,
     * so the lazy per-car load wouldn't refresh it — and a race may have reused
     * its texture page (it would otherwise paint the WHOLE body from stale
     * content). DROP the handle too (set to 0) — do NOT frontend_release_surface()
     * it: the old overlay slot was already freed by td5_frontend_set_screen's
     * recyclable sweep, and the preview we just loaded may now occupy that very
     * slot. Releasing the stale handle would free the fresh preview; uploading
     * CarPicPaint0 onto it then showed only the painted chassis. Zeroing the
     * handle leaks nothing (slot already free) and guarantees the lazy reload in
     * frontend_draw_car_paint_overlay takes the clean "no prior surface" path. */
    s_paint_overlay_surface = 0;
    s_paint_overlay_car = -1;
    TD5_LOG_I(LOG_TAG, "car preview (re)loaded: car=%d surf=%d; paint overlay cache invalidated",
              car_index, s_car_preview_surface);
}

static void frontend_load_selected_track_preview(void) {
    char entry[32];
    int tga_idx;
    if (s_track_preview_surface > 0) {
        frontend_release_surface(s_track_preview_surface);
        s_track_preview_surface = 0;
    }
    if (s_selected_track < 0) return;
    /* Use the original binary's gScheduleToPoolIndex table (DAT_00466894):
     * pool_index == trak TGA file number, NOT the schedule slot. */
    tga_idx = (s_selected_track < (int)(sizeof(s_track_schedule_to_tga_index)))
              ? s_track_schedule_to_tga_index[s_selected_track]
              : s_selected_track;
    snprintf(entry, sizeof(entry), "trak%04d.tga", tga_idx);
    /* Black background is color-keyed out so the track outline floats over the scene background. */
    s_track_preview_surface = frontend_load_surface_keyed(entry, "Front End/Tracks/Tracks.zip", TD5_COLORKEY_BLACK);
}

/* --- Button System --- */

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

static FE_Button s_buttons[FE_MAX_BUTTONS];
static int s_button_count;

/* TD6 color-panel helpers that depend on s_buttons / mouse state (declared
 * above). See the panel definition higher up for the layout + palette. */

/* Idempotent: position the 6 car-select buttons for the current panel state.
 * Closed = the normal rows. Open = CAR/PAINT shift UP and Stats/Auto/OK/Back drop
 * BELOW the color panel, but compressed so OK/Back don't sit too low. Recomputed
 * from these tables every frame, so it survives button recreation (no drift). */
static void frontend_apply_color_panel_layout(void) {
    static const int closed_y[6] = { 169, 209, 249, 289, 329, 329 };
    static const int open_y[6]   = { 150, 190, 336, 372, 408, 408 };
    const int *y = s_color_panel_visible ? open_y : closed_y;
    for (int i = 0; i < 6 && i < FE_MAX_BUTTONS; i++)
        if (s_buttons[i].active) s_buttons[i].y = y[i];
}
static void frontend_set_color_panel(int open) {
    s_color_panel_visible = open ? 1 : 0;
    if (s_color_panel_visible) frontend_color_panel_sync_index();
    frontend_apply_color_panel_layout();
}

/* Mouse pick inside the open panel: clicking a swatch or a map cell moves the
 * 2D cursor to it. Returns 1 if it set the cursor (caller applies the color).
 * Mouse coords are in 640x480 canvas space, same as the swatch/map rects. */
static int frontend_color_panel_mouse(void) {
    if (!s_mouse_clicked) return 0;
    int mx = s_mouse_x, my = s_mouse_y;
    for (int i = 0; i < TD6_PALETTE_N; i++) {
        int c = i % TD6_CP_COLS, r = i / TD6_CP_COLS;
        int x = TD6_CP_LIST_X + c * (TD6_CP_SW + TD6_CP_GAP);
        int y = TD6_CP_LIST_Y + r * (TD6_CP_SW + TD6_CP_GAP);
        if (mx >= x && mx < x + TD6_CP_SW && my >= y && my < y + TD6_CP_SW) {
            s_color_cur_col = c; s_color_cur_row = r;
            return 1;
        }
    }
    if (mx >= TD6_CP_MAP_X && mx < TD6_CP_MAP_X + TD6_CP_MAP_W &&
        my >= TD6_CP_MAP_Y && my < TD6_CP_MAP_Y + TD6_CP_MAP_H) {
        int col = (mx - TD6_CP_MAP_X) * TD6_CP_COLS / TD6_CP_MAP_W;
        int row = (my - TD6_CP_MAP_Y) * TD6_CP_MAP_ROWS / TD6_CP_MAP_H;
        if (col >= TD6_CP_COLS)     col = TD6_CP_COLS - 1;
        if (row >= TD6_CP_MAP_ROWS) row = TD6_CP_MAP_ROWS - 1;
        s_color_cur_col = col; s_color_cur_row = 2 + row;
        return 1;
    }
    return 0;
}

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
#define FE_CANVAS_W 640
#define FE_CANVAS_H 480
#define FE_CENTER_X (FE_CANVAS_W / 2)  /* 320 */
#define FE_CENTER_Y (FE_CANVAS_H / 2)  /* 240 */
#define FE_BTN_LEFT_OFFSET 0xD2        /* 210: center_x - this = left edge */
static const int s_auto_button_y_offset[] = {
    -0x93, -0x6B, -0x43, -0x1B, 0x0D, 0x35, 0x5D, 0x85, 0xAD
};
static int s_auto_button_idx = 0;
static int s_selected_button = 0;
static int s_selection_from_mouse = 0; /* 1 when last selection came from mouse hover */

static void frontend_reset_buttons(void) {
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
static int frontend_create_button(const char *label, int x, int y, int w, int h) {
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

static int frontend_create_preview_button(const char *label, int x, int y, int w, int h) {
    int idx = frontend_create_button(label, x, y, w, h);
    if (idx >= 0) s_buttons[idx].disabled = 1;
    return idx;
}

static void frontend_release_button(int handle) {
    if (handle >= 0 && handle < FE_MAX_BUTTONS)
        s_buttons[handle].active = 0;
}

/* [ARCH-DIVERGENCE: cursor flag polarity flipped + mouse-moved flag dropped; L5 sweep 2026-05-21]
 *   Ports ActivateFrontendCursorOverlay (0x004258C0) and DeactivateFrontendCursorOverlay (0x004258E0).
 *   Orig writes g_frontendCursorOverlayHidden = 1/0 with semantics inverted from its name; port
 *   stores s_cursor_visible with direct semantics (1=show). All 12 callers' arguments were inverted
 *   in the same commit so net behavior is unchanged. Orig also clears g_frontendMouseMovedFlag —
 *   port's mouse-moved tracking lives in update_frontend (s_prev_mouse_x/y compare) and does not
 *   need a separate flag clear here. */
static void frontend_set_cursor_visible(int visible) {
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

static void frontend_play_sfx(int id) {
    if (id == 5) s_fade_whoosh_emitted = 1;
    if (id == 4) s_fade_chime_emitted = 1;
    td5_sound_play_frontend_sfx(id);
}

static void frontend_cd_play(int track) {
    td5_plat_cd_play(track + 2);
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

#define FE_MAX_DRAW_CMDS 128

typedef enum { FE_CMD_RECT, FE_CMD_BLIT } FE_CmdType;

typedef struct {
    FE_CmdType type;
    int x, y, w, h;
    uint32_t color;
    int tex_page;  /* for blit */
    float u0, v0, u1, v1; /* UV for blit */
} FE_DrawCmd;

static FE_DrawCmd s_draw_queue[FE_MAX_DRAW_CMDS];
static int s_draw_queue_count;

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
static void frontend_present_buffer(void) {
    td5_plat_render_end_scene();
    td5_plat_present(1);
}

static void frontend_init_font_metrics_default(void) {
    memcpy(s_font_glyph_advance, k_font_glyph_advance_default, sizeof(s_font_glyph_advance));
}

static void frontend_init_font_metrics_from_pixels(const uint8_t *pixels, int w, int h) {
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
}

static float fe_measure_text(const char *text, float sx) {
    float width = 0.0f;

    if (!text) return 0.0f;

    for (int i = 0; text[i]; i++) {
        int c = toupper((unsigned char)text[i]);
        if (c < 32 || c > 127) {
            width += 14.0f * sx;
            continue;
        }
        width += (float)s_font_glyph_advance[c - 0x20] * sx;
    }

    return width;
}

static int frontend_advance_tick(void) {
    s_anim_tick += 2;
    return 1;
}

/* --- Fade --- */

static void frontend_init_fade(int color) {
    s_fade_active = 1;
    s_fade_progress = 0;
    s_fade_direction = 1;
    /*
     * [CONFIRMED @ 0x411750 InitFrontendFadeColor]: color is a packed ARGB/RGB
     * word whose B/G/R channels are stored as luma multipliers after >> 3 & 0x1f.
     * Port: store raw 0x00RRGGBB for use in frontend_render_fade.
     */
    s_fade_color = (uint32_t)color & 0x00FFFFFFu;
}

static int frontend_render_fade(void) {
    if (!s_fade_active) return 1;
    s_fade_progress += 16;
    if (s_fade_progress >= 256) {
        s_fade_active = 0;
        return 1;
    }
    /* Draw semi-transparent black overlay */
    if (s_draw_queue_count < FE_MAX_DRAW_CMDS) {
        int screen_w = 0, screen_h = 0;
        td5_plat_get_window_size(&screen_w, &screen_h);
        uint32_t alpha = (uint32_t)(s_fade_progress < 256 ? s_fade_progress : 255);
        FE_DrawCmd *cmd = &s_draw_queue[s_draw_queue_count++];
        cmd->type = FE_CMD_RECT;
        cmd->x = 0; cmd->y = 0; cmd->w = screen_w; cmd->h = screen_h;
        /* Use s_fade_color (set by frontend_init_fade) for the overlay RGB.
         * Alpha is driven by fade progress [CONFIRMED @ 0x411750 / 0x411780]. */
        cmd->color = (alpha << 24) | (s_fade_color & 0x00FFFFFFu);
        cmd->tex_page = -1;
    }
    return 0;
}

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
 * Only the police cars (ext_id 33..36) are excluded — they are cop-chase
 * vehicles, not race cars. Any car whose carparam.dat cannot be read is also
 * skipped automatically. If NO carparam.dat is readable the caller falls back
 * to the original s_difficulty_tier_cars roster.
 * ======================================================================== */

/* Police cars (cop-chase only) sit at ext_id 33..36 and are skipped. The pool
 * spans all other s_car_zip_paths entries (TD5 0..32 + TD6 37..75), so the
 * arrays are sized to the full car table. */
#define S21_POLICE_FIRST 33
#define S21_POLICE_LAST  36

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
        if (id >= S21_POLICE_FIRST && id <= S21_POLICE_LAST)
            continue;                      /* skip cop-chase cars */
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
typedef struct { const char *label; int cols; int rows; } MpSplitLayout;

static const MpSplitLayout *mp_split_layouts(int n, int *count)
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
static void mp_resolve_layout(int n, int sel, int *cols, int *rows, int *missing)
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
static const char *const k_mp_missing_content[] = { "EMPTY", "MAP", "STANDINGS" };
#define MP_MISSING_CONTENT_COUNT ((int)(sizeof(k_mp_missing_content) / sizeof(k_mp_missing_content[0])))

static void frontend_init_race_schedule(void) {
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

    /* Resolve the chosen split layout. For >=2 humans split is on and the layout
     * grid (cols x rows) overrides the automatic ladder in
     * td5_game_init_viewport_layout. split_screen_mode keeps its legacy meaning
     * for HUD / minimap / sound consumers: 0=single, 2=two-player left|right,
     * 1=any other split "on". */
    if (eff_humans >= 2) {
        int cols = 0, rows = 0, missing = 0;
        mp_resolve_layout(eff_humans, s_mp_layout_sel, &cols, &rows, &missing);
        g_td5.split_grid_cols = cols;
        g_td5.split_grid_rows = rows;
        g_td5.split_screen_mode = (eff_humans == 2 && cols == 2) ? 2 : 1;
        g_td5.split_missing_content[0] = (missing > 0) ? s_mp_missing_content[0] : 0;
        g_td5.split_missing_content[1] = (missing > 1) ? s_mp_missing_content[1] : 0;
    } else {
        g_td5.split_grid_cols = 0;
        g_td5.split_grid_rows = 0;
        g_td5.split_screen_mode = 0;
        g_td5.split_missing_content[0] = 0;
        g_td5.split_missing_content[1] = 0;
    }
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
        g_td5.traffic_enabled           = s_game_option_traffic;
        g_td5.special_encounter_enabled = s_game_option_cops;
    }

    TD5_LOG_I(LOG_TAG,
              "InitRaceSchedule: split=%d grid=%dx%d humans=%d opp=%d eff=%d 2p=%d layout_sel=%d",
              g_td5.split_screen_mode, g_td5.split_grid_cols, g_td5.split_grid_rows,
              g_td5.num_human_players, g_td5.num_ai_opponents, eff_humans,
              s_two_player_mode, s_mp_layout_sel);

    /* Slot 0 = player, always active */
    slot_active[0]  = 1;
    slot_ext_id[0]  = s_selected_car;
    slot_variant[0] = s_selected_paint;

    /* [PORT ENHANCEMENT 2026-06] Multiplayer lobby flow: each human slot uses the
     * car that player chose in the sequential car select. */
    if (s_mp_flow) {
        slot_ext_id[0]  = s_mp_player_car[0];
        slot_variant[0] = s_mp_player_paint[0];
        for (i = 1; i < eff_humans && i < TD5_MAX_RACER_SLOTS; i++) {
            slot_active[i]  = 1;
            slot_ext_id[i]  = s_mp_player_car[i];
            slot_variant[i] = s_mp_player_paint[i];
            if (i + 1 > start_slot) start_slot = i + 1;
        }
    }

    /* Two-player setup [CONFIRMED @ 0x0040daf0]:
     * Original gate: g_twoPlayerModeEnabled != 0 || g_selectedGameType == 7.
     * In the original, game_type 7 is DRAG RACE (user picks a 2nd car via the
     * 2-pass CarSelect loop). The port's convention uses game_type 9 for drag
     * race, so the constant is swapped here. Time Trials is solo and must NOT
     * fall into this branch. (Skipped for the N-way multiplayer lobby flow,
     * which already populated the human slots above.) */
    if ((s_two_player_mode || s_selected_game_type == 9) && !s_mp_flow) {
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
    if (g_td5.ini.race_trace_enabled) {
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

    if (s_selected_game_type == 2) {
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
    } else if (s_selected_game_type == 5) {
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

static int ConfigureGameTypeFlags(void);  /* forward decl */
static void frontend_init_display_mode_state(void);  /* forward decl */

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

static void frontend_set_button_label(int idx, const char *text) {
    if (idx < 0 || idx >= s_button_count) return;
    strncpy(s_buttons[idx].label, text, sizeof(s_buttons[idx].label) - 1);
    s_buttons[idx].label[sizeof(s_buttons[idx].label) - 1] = '\0';
}

/* [S01 Display options 2026-06-04] 6 option rows + OK (Resolution row removed —
 * the window is freely resizable). Row order:
 *   0 Display Mode  1 VSync  2 Fogging
 *   3 Speed Readout 4 Show FPS  5 Camera Damping  6 OK */
static void frontend_refresh_display_option_labels(void) {
    frontend_set_button_label(0, "Display Mode");
    frontend_set_button_label(1, "VSync");
    frontend_set_button_label(2, "Fogging");
    frontend_set_button_label(3, "Speed Readout");
    frontend_set_button_label(4, "Show FPS");
    frontend_set_button_label(5, "Camera Damping");
    frontend_set_button_label(6, "OK");
}

static int frontend_option_delta(void) {
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

static void frontend_init_return_screen(TD5_ScreenIndex screen) {
    s_return_screen = (int)frontend_get_parent_screen(screen);
}

static void frontend_init_display_mode_state(void) {
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

static void frontend_post_quit(void);   /* defined later; used by the credits-skip path */

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

    /* [S12] Drain the WM_KEYDOWN nav latch every frame. Read here (before the
     * inactive early-return) so a press that arrived while the window was
     * unfocused is discarded rather than firing on refocus. Applied to the edge
     * flags further down, only while no text field is open. */
    unsigned nav_latch = td5_plat_input_nav_latch();

    s_input_ready = 0;
    s_button_index = -1;
    s_arrow_input = 0;

    hwnd = (HWND)(DWORD_PTR)Backend_GetDisplayWindow();
    if (!frontend_is_window_active()) {
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

    /* Keyboard arrows */
    left_now = td5_plat_input_key_pressed(0xCB);
    right_now = td5_plat_input_key_pressed(0xCD);
    up_now = td5_plat_input_key_pressed(0xC8);
    down_now = td5_plat_input_key_pressed(0xD0);
    enter_now = td5_plat_input_key_pressed(0x1C);

    /* Active-controller tracking: keyboard input keeps device 0 active. */
    if (left_now || right_now || up_now || down_now || enter_now)
        s_active_menu_device = 0;

    /* [PORT ENHANCEMENT 2026-06] Hot-swap detection: re-enumerate DirectInput
     * joysticks every ~90 frames (~1.5s @ 60fps) so a controller connected while
     * sitting on a menu is detected without having to leave/re-enter the screen.
     * Cheap (an IDirectInput8::EnumDevices pass that early-outs when the count is
     * unchanged); only the scan handles are rebuilt when the device set changes. */
    if ((++s_fe_rescan_tick % 90u) == 0u)
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

    left_edge = (left_now && !s_prev_left_state);
    right_edge = (right_now && !s_prev_right_state);
    up_edge = (up_now && !s_prev_up_state);
    down_edge = (down_now && !s_prev_down_state);
    enter_edge = (enter_now && !s_prev_enter_state);
    if (left_now || right_now || up_now || down_now || enter_now) had_activity = 1;

    /* [S12] Fold in nav-key taps the window proc captured between frames so a
     * quick press at low FPS isn't dropped by the once-per-frame DI immediate
     * read. Press-only (auto-repeat filtered in the proc) so a held key still
     * moves once and never auto-repeats. Suppressed while a text field is open
     * so a latched Enter can't leak into name entry (text uses the WM_CHAR
     * queue, not these edges). */
    if (s_text_input_state == 0 && nav_latch) {
        if (nav_latch & TD5_NAVKEY_LEFT)  left_edge  = 1;
        if (nav_latch & TD5_NAVKEY_RIGHT) right_edge = 1;
        if (nav_latch & TD5_NAVKEY_UP)    up_edge    = 1;
        if (nav_latch & TD5_NAVKEY_DOWN)  down_edge  = 1;
        if (nav_latch & TD5_NAVKEY_ENTER) enter_edge = 1;
        had_activity = 1;
    }

    if (left_edge) s_arrow_input |= 1;  /* LEFT — rising edge only (original: DAT_004951f8) */
    if (right_edge) s_arrow_input |= 2; /* RIGHT — rising edge only */
    if (up_edge) s_arrow_input |= 4;   /* UP */
    if (down_edge) s_arrow_input |= 8; /* DOWN */

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

    /* Mouse click: catch both held-edge presses and short clicks between frames. */
    mouse_state = GetAsyncKeyState(VK_LBUTTON);
    mouse_btn = (mouse_state & 0x8000) ? 1 : 0;
    if ((mouse_state & 0x0001) != 0) {
        s_mouse_click_latched = 1;
    }
    s_mouse_clicked = s_mouse_click_latched || (mouse_btn && !s_prev_mouse_btn);
    if (mouse_btn || s_mouse_clicked) had_activity = 1;
    s_prev_mouse_btn = mouse_btn;

    /* Keyboard navigation: UP/DOWN move between rows, LEFT/RIGHT move within
     * a horizontal button row. Suppressed while the TD6 color panel is open —
     * there the 4 arrows navigate the color grid (modal), handled in the tick;
     * the s_arrow_input bits are still set above for that.
     *
     * [PORT ENHANCEMENT 2026-06] The Controller-Binding (Configure) screen lays
     * its action buttons out as a two-column grid, which the flat index-order
     * row nav mis-handles (DOWN at a column's bottom spills to the next column's
     * top). For that screen route all four arrows through true 2D spatial nav so
     * UP/DOWN stay in the current column and LEFT/RIGHT change column. */
    int spatial_nav = (s_current_screen == TD5_SCREEN_CONTROLLER_BINDING);
    if (left_edge && !s_color_panel_visible) {
        int moved = spatial_nav ? frontend_move_selected_button_spatial(-1, 0)
                                : frontend_cycle_selected_button_horizontal(-1);
        if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
    }
    if (right_edge && !s_color_panel_visible) {
        int moved = spatial_nav ? frontend_move_selected_button_spatial(1, 0)
                                : frontend_cycle_selected_button_horizontal(1);
        if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
    }
    if (up_edge && !s_color_panel_visible) {
        int moved = spatial_nav ? frontend_move_selected_button_spatial(0, -1)
                                : frontend_cycle_selected_button_vertical(-1);
        if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
        /* No sound on a no-target edge move. Faithful to the original shared nav
         * handler UpdateFrontendDisplayModeSelection @0x00426580, which is SILENT
         * when no neighbour button exists. The port previously played Uh-Oh (10)
         * here, producing an error blip every time you hit the top/bottom of a
         * list. (Horizontal L/R moves already had no failed-move sound.) */
    }
    if (down_edge && !s_color_panel_visible) {
        int moved = spatial_nav ? frontend_move_selected_button_spatial(0, 1)
                                : frontend_cycle_selected_button_vertical(1);
        if (moved) { frontend_play_sfx(2); s_selection_from_mouse = 0; }
    }
    if (enter_edge) {
        if (s_selected_button >= 0 && s_selected_button < FE_MAX_BUTTONS &&
            s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
            s_button_index = s_selected_button;
            s_input_ready = 1;
            frontend_play_sfx(3);
            TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=keyboard",
                      s_button_index, s_buttons[s_button_index].label);
        } else if (s_current_screen != TD5_SCREEN_EXTRAS_GALLERY) {
            frontend_play_sfx(10);
        }
        /* else: the post-EXIT credits scroll has no buttons, so a confirm press
         * has nothing to action — the any-key skip block below quits instead of
         * playing the rejection blip. */
    }

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
    if (s_mouse_clicked) {
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
            if (i == s_selected_button) {
                /* Already selected: check arrow zones (20px from edges) */
                int arrow_zone = 0;
                if (s_mouse_x >= s_buttons[i].x + s_buttons[i].w - 0x14) {
                    arrow_zone = 1;   /* RIGHT */
                } else if (s_mouse_x < s_buttons[i].x + 0x14) {
                    arrow_zone = -1;  /* LEFT */
                }

                if (arrow_zone != 0) {
                    /* Arrow click: set arrow input directly, play feedback */
                    if (arrow_zone > 0) s_arrow_input |= 2;  /* RIGHT */
                    else                s_arrow_input |= 1;  /* LEFT */
                    frontend_play_sfx(2);
                } else {
                    /* Center click on selected button: confirm */
                    s_button_index = i;
                    s_input_ready = 1;
                    s_mouse_flash_button = i;
                    s_mouse_flash_until = now + 180;
                    s_mouse_confirm_button = -1;
                    frontend_play_sfx(3);
                    TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=mouse",
                              s_button_index, s_buttons[s_button_index].label);
                }
            } else {
                /* Different button: select it (first click selects, second confirms) */
                s_selected_button = i;
                s_selection_from_mouse = 1;
                frontend_play_sfx(2);
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

static int frontend_check_escape(void) {
    if (!frontend_is_window_active()) {
        s_prev_escape_state = 1;
        return 0;
    }
    /* Gamepad B (button 1) backs out, same as the ESC key. [PORT ENHANCEMENT] */
    int esc_now = td5_plat_input_key_pressed(0x01) || ((s_fe_gamepad_nav & 0x20) != 0);
    int esc_edge = (esc_now && !s_prev_escape_state);
    s_prev_escape_state = esc_now;
    if (esc_edge) frontend_note_activity();
    return esc_edge;
}

/* Forward declaration for text rendering (defined later in file) */
static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
static void fe_draw_text_centered(float center_x, float y, const char *text,
                                  uint32_t color, float sx, float sy);
static void frontend_fill_rect(int layer, int x, int y, int w, int h, uint32_t color);
static void fe_draw_button_9slice(float bx, float by, float bw, float bh,
                                  int state, float sx, float sy);
static void fe_draw_button_frame(float bx, float by, float bw, float bh,
                                 int bb_state, float sx, float sy);
static int fe_draw_arrow_proc(float x, float y, float w, float h,
                              int dir_right, uint32_t color);
static void fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy);
static float fe_measure_small_text(const char *text);
static void fe_draw_option_arrows(int btn_idx, float sx, float sy);
static void frontend_load_bg_gallery(void);
/* Forward declarations for dialog text overlay renderers */
static void frontend_render_legal_copyright_overlay(float sx, float sy);
static void frontend_render_cup_failed_overlay(float sx, float sy);
static void frontend_render_cup_won_overlay(float sx, float sy);
static void frontend_render_session_locked_overlay(float sx, float sy);

/* S10: optional prompt shown in the text-input widget (empty -> "ENTER PLAYER
 * NAME"). Set AFTER frontend_begin_text_input (which resets it to default). */
static char s_text_input_prompt[40] = "";
static void frontend_set_text_input_prompt(const char *p) {
    if (p && p[0]) {
        strncpy(s_text_input_prompt, p, sizeof(s_text_input_prompt) - 1);
        s_text_input_prompt[sizeof(s_text_input_prompt) - 1] = '\0';
    } else {
        s_text_input_prompt[0] = '\0';
    }
}

static void frontend_begin_text_input(char *buffer, int capacity) {
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

static void frontend_handle_text_input_key(void) {
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
        float caret_x = field_x + name_w * sx + 1.0f * sx;
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(caret_x, field_y, 2.0f * sx, 12.0f * sy, 0xFF00FF00, -1, 0, 0, 0, 0);
    }
}

static int frontend_text_input_confirmed(void) {
    return (s_text_input_ctx.confirm_state != 0) || (s_text_input_state == 2);
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
static void frontend_post_quit(void) {
    g_td5.quit_requested = 1;
}

/* Write cup data: sync game state into save module, then write file. */
static int frontend_write_cup_data(void) {
    td5_save_sync_cup_from_game(s_race_within_series);
    int ok = td5_save_write_cup_data(NULL);
    TD5_LOG_I(LOG_TAG, "WriteCupData: result=%d race=%d type=%d",
              ok, s_race_within_series, (int)s_selected_game_type);
    return ok;
}

/* Load continue cup data: read + decrypt + restore game state. */
static int frontend_load_continue_cup_data(void) {
    int ok = td5_save_load_cup_data(NULL);
    if (ok) {
        int restored_race = 0;
        int game_type = td5_save_sync_cup_to_game(&restored_race);
        s_selected_game_type = game_type;
        s_race_within_series = restored_race;
        TD5_LOG_I(LOG_TAG, "LoadContinueCupData: type=%d race=%d", game_type, restored_race);
    } else {
        TD5_LOG_W(LOG_TAG, "LoadContinueCupData: failed");
    }
    return ok;
}

/* Validate CupData.td5 checksum without restoring state. */
static int frontend_validate_cup_checksum(void) {
    return td5_save_is_cup_valid(NULL);
}

/* Delete cup data file.
 * [CONFIRMED @ 0x423ACD]: original calls _unlink(CupData.td5) directly in
 * ScreenCupWonDialog case 0. Port delegates to td5_plat_file_delete which
 * wraps DeleteFileA — semantically identical. */
static void frontend_delete_cup_data(void) {
    TD5_LOG_I(LOG_TAG, "frontend_delete_cup_data: removing td5re_cup.ini");
    td5_save_delete_cup_data();
}

/**
 * Send a network message via td5_net.
 * Wraps td5_net_send with the DXPTYPE cast and optional payload header.
 * For DATA messages (type 1), prepends a 4-byte payload size header
 * matching the original DXPDATA wire format.
 */
static void frontend_net_send(int type, const void *data, int size) {
    td5_net_send((TD5_NetMsgType)type, data, size);
}

/**
 * Receive a network message from the ring buffer.
 * Returns the DXPTYPE message type (0-12) or -1 if no message available.
 * Copies payload into buf (up to max_size bytes).
 */
static int frontend_net_receive(void *buf, int max_size) {
    TD5_NetMsgType type;
    void *data = NULL;
    int size = 0;

    if (!td5_net_receive(&type, &data, &size))
        return -1;

    /* Copy payload into caller's buffer */
    if (data && size > 0 && buf && max_size > 0) {
        int copy_size = (size < max_size) ? size : max_size;
        memcpy(buf, data, (size_t)copy_size);
    }

    return (int)type;
}

/**
 * Destroy the network session and shut down.
 * Called when leaving network screens or on disconnect.
 */
static void frontend_net_destroy(void) {
    td5_net_shutdown();
    s_network_active = 0;
}

/**
 * Seal or unseal the session (prevent/allow new joins).
 */
static void frontend_net_seal(int sealed) {
    td5_net_seal_session(sealed);
}

/**
 * Enumerate network providers and sessions.
 * Initializes the network subsystem if needed, enumerates connections,
 * picks the first (UDP LAN), and triggers session discovery.
 * Returns the number of sessions found.
 */
static int frontend_net_enumerate(void) {
    int conn_count;

    /* Initialize network if not already done (td5_net_init is idempotent) */
    if (!td5_net_init()) {
        TD5_LOG_W(LOG_TAG, "frontend_net_enumerate: td5_net_init failed");
        return 0;
    }

    conn_count = td5_net_enumerate_connections();
    if (conn_count > 0) {
        td5_net_pick_connection(0);
    }

    return td5_net_enumerate_sessions();
}

/**
 * Check if the local player is the session host.
 */
static int frontend_net_is_host(void) {
    return td5_net_is_host();
}

/**
 * Get the local player's slot index (0-5).
 */
static int frontend_net_local_slot(void) {
    return td5_net_local_slot();
}

/* ========================================================================
 * ConfigureGameTypeFlags (0x410CA0)
 *
 * Maps g_selectedGameType to runtime flags.
 * Returns 1 if configuration is valid, 0 if end-of-series.
 * ======================================================================== */

static int ConfigureGameTypeFlags(void) {
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
    s_fade_progress += s_fade_direction * 8;
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
    s_joypad_icon_surface = 0;
    s_joystick_icon_surface = 0;
    s_keyboard_icon_surface = 0;
    s_nocontroller_surface = 0;
    /* Release recyclable surfaces, but KEEP shared assets on dedicated pages.
     * Shared pages (895-899) hold font, cursor, ButtonBits, mainfont --
     * these are loaded once in init and must survive screen transitions. */
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (preserved_background > 0 && i == preserved_background - 1) continue;
        if (s_surfaces[i].in_use &&
            s_surfaces[i].tex_page >= SHARED_PAGE_MIN &&
            s_surfaces[i].tex_page < FE_SURFACE_PAGE_BASE) continue;
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

int td5_frontend_display_loop(void) {
    if (g_td5.ini.log_frontend_draw) s_fe_draw_log_frame++;
    td5_profile_begin_frame();
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

    /* 2. Input polling (keyboard, mouse, joystick) */
    frontend_poll_input();
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
    if (s_anim_complete && frontend_check_escape()) {
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

int td5_frontend_init_resources(void) {
    TD5_LOG_I(LOG_TAG, "InitializeFrontendResourcesAndState");
    frontend_init_font_metrics_default();

    /* Create 1x1 white fallback texture for untextured draws */
    if (s_white_tex_page < 0) {
        s_white_tex_page = SHARED_PAGE_WHITE;
        uint32_t white = 0xFFFFFFFF;
        if (td5_plat_render_upload_texture(s_white_tex_page, &white, 1, 1, 2)) {
            TD5_LOG_I(LOG_TAG, "Fallback background texture loaded: white page=%d",
                      s_white_tex_page);
        } else {
            TD5_LOG_W(LOG_TAG, "Fallback background texture upload failed: page=%d",
                      s_white_tex_page);
        }
    }

    /* ---- Vector (MSDF) BodyText atlas + shader (resolution-independent) ----
     * Generated by re/tools/build_msdf_font.py. Loaded with NO colorkey (it is
     * a signed distance field, not a keyed bitmap). On ANY failure we leave
     * s_msdf_font_page = -1 / s_ps_msdf = NULL so fe_draw_text falls back to the
     * bitmap glyph atlas -- a missing asset/shader never breaks the menu. */
    if (g_td5.ini.vector_ui && s_msdf_font_page < 0) {
        if (!s_ps_msdf && g_backend.device) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_msdf, sizeof(g_ps_msdf), NULL, &s_ps_msdf);
            if (FAILED(hr)) {
                s_ps_msdf = NULL;
                TD5_LOG_W(LOG_TAG, "MSDF pixel shader create failed hr=0x%08lX",
                          (unsigned long)hr);
            }
        }
        if (s_ps_msdf) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/BodyText_msdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_FONT_MSDF, pixels, mw, mh, 2)) {
                    s_msdf_font_page = SHARED_PAGE_FONT_MSDF;
                    TD5_LOG_I(LOG_TAG, "MSDF font atlas loaded: page=%d %dx%d (VectorUI on)",
                              s_msdf_font_page, mw, mh);
                } else {
                    TD5_LOG_W(LOG_TAG, "MSDF font atlas upload failed");
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "BodyText_msdf.png not found -- VectorUI falls back to bitmap font");
            }
        }
    }

    /* ---- Procedural rounded-rect button shader + constant buffer (VectorUI) ----
     * On failure both stay NULL and the button loop falls back to the bitmap
     * 9-slice / button cache. */
    if (g_td5.ini.vector_ui && g_backend.device) {
        if (!s_ps_roundrect) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_roundrect, sizeof(g_ps_roundrect), NULL, &s_ps_roundrect);
            if (FAILED(hr)) {
                s_ps_roundrect = NULL;
                TD5_LOG_W(LOG_TAG, "roundrect shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_roundrect && !s_rr_cb) {
            D3D11_BUFFER_DESC bd;
            ZeroMemory(&bd, sizeof(bd));
            bd.ByteWidth = sizeof(FE_RoundRectParams);   /* 96, 16-aligned */
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            HRESULT hr = ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &s_rr_cb);
            if (FAILED(hr)) {
                s_rr_cb = NULL;
                TD5_LOG_W(LOG_TAG, "roundrect cbuffer create failed hr=0x%08lX", (unsigned long)hr);
            } else {
                TD5_LOG_I(LOG_TAG, "Procedural roundrect button shader ready (VectorUI)");
            }
        }
        if (!s_ps_arrow) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_arrow, sizeof(g_ps_arrow), NULL, &s_ps_arrow);
            if (FAILED(hr)) {
                s_ps_arrow = NULL;
                TD5_LOG_W(LOG_TAG, "arrow shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (!s_ps_cursor) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_cursor, sizeof(g_ps_cursor), NULL, &s_ps_cursor);
            if (FAILED(hr)) {
                s_ps_cursor = NULL;
                TD5_LOG_W(LOG_TAG, "cursor shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_cursor && s_cursor_msdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/snkmouse_msdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_CURSOR_MSDF, pixels, mw, mh, 2))
                    s_cursor_msdf_page = SHARED_PAGE_CURSOR_MSDF;
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "snkmouse_msdf.png not found -- cursor falls back to bitmap");
            }
        }

        /* ---- Procedural analog gauge dial shader + constant buffer (VectorUI) ----
         * Drives the in-race HUD speedometer dial + the added RPM tachometer via
         * td5_vui_gauge. On failure both stay NULL and the HUD falls back to the
         * baked GDI dial texture. */
        if (!s_ps_gauge) {
            HRESULT hr = ID3D11Device_CreatePixelShader(g_backend.device,
                g_ps_gauge, sizeof(g_ps_gauge), NULL, &s_ps_gauge);
            if (FAILED(hr)) {
                s_ps_gauge = NULL;
                TD5_LOG_W(LOG_TAG, "gauge shader create failed hr=0x%08lX", (unsigned long)hr);
            }
        }
        if (s_ps_gauge && !s_gauge_cb) {
            D3D11_BUFFER_DESC bd;
            ZeroMemory(&bd, sizeof(bd));
            bd.ByteWidth = sizeof(FE_GaugeParams);   /* 144, 16-aligned */
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            HRESULT hr = ID3D11Device_CreateBuffer(g_backend.device, &bd, NULL, &s_gauge_cb);
            if (FAILED(hr)) {
                s_gauge_cb = NULL;
                TD5_LOG_W(LOG_TAG, "gauge cbuffer create failed hr=0x%08lX", (unsigned long)hr);
            } else {
                TD5_LOG_I(LOG_TAG, "Procedural gauge dial shader ready (VectorUI)");
            }
        }

        /* ---- In-race HUD font SDF (VectorUI) ----
         * Distance-field version of the original tpage5 HUD font; rendered via
         * ps_msdf so the HUD text keeps its typeface but stays crisp. On any
         * failure s_hudfont_sdf_page stays -1 and the HUD uses the bitmap font. */
        if (s_ps_msdf && s_hudfont_sdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/static/hudfont_sdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_HUDFONT_SDF, pixels, mw, mh, 2)) {
                    s_hudfont_sdf_page = SHARED_PAGE_HUDFONT_SDF;
                    TD5_LOG_I(LOG_TAG, "HUD font SDF atlas loaded: page=%d %dx%d (VectorUI on)",
                              s_hudfont_sdf_page, mw, mh);
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "hudfont_sdf.png not found -- HUD text falls back to bitmap font");
            }
        }

        /* ---- Pause-menu font SDF (VectorUI) ---- */
        if (s_ps_msdf && s_pausefont_sdf_page < 0) {
            void *pixels = NULL;
            int mw = 0, mh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/static/pausefont_sdf.png",
                                              TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
                if (td5_plat_render_upload_texture(SHARED_PAGE_PAUSEFONT_SDF, pixels, mw, mh, 2)) {
                    s_pausefont_sdf_page = SHARED_PAGE_PAUSEFONT_SDF;
                    TD5_LOG_I(LOG_TAG, "Pause font SDF atlas loaded: page=%d %dx%d (VectorUI on)",
                              s_pausefont_sdf_page, mw, mh);
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "pausefont_sdf.png not found -- pause menu falls back to bitmap font");
            }
        }
    }

    /* ---- Font atlas (BodyText.tga) ----
     * The ACTUAL game font is BodyText.tga (240x552, 8bpp palette TGA with
     * red color key). 10 chars/row, 24px cells. DAT_0049626c in original.
     * Load it to dedicated page 898 with red color key transparency.
     * Fall back to GDI-generated font if the TGA is missing. */
    if (s_font_page < 0) {
        int font_w = 0, font_h = 0;
        if (frontend_load_tga_colorkey("BodyText.tga", "Front End/frontend.zip",
                                        SHARED_PAGE_FONT, &font_w, &font_h,
                                        TD5_COLORKEY_BLACK)) {
            s_font_page = SHARED_PAGE_FONT;
            TD5_LOG_I(LOG_TAG, "Font atlas loaded: BodyText.tga page=%d %dx%d",
                      s_font_page, font_w, font_h);
        } else {
            /* FALLBACK: generate font with GDI if BodyText.tga is unavailable.
             * Uses 240x240 (only 10 rows needed for ASCII 32-127). The UV math
             * in fe_draw_text still works because row/col calculations produce
             * coords within the 240px region; the atlas is just smaller. */
            TD5_LOG_W(LOG_TAG, "BodyText.tga not found, falling back to GDI font");
            int fw = FONT_TEX_W, fh = 240; /* 10 rows * 24px for GDI fallback */
            uint32_t *pixels = (uint32_t *)calloc((size_t)(fw * fh), 4);
            if (pixels) {
                HDC hdc = CreateCompatibleDC(NULL);
                BITMAPINFO bmi;
                memset(&bmi, 0, sizeof(bmi));
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = fw;
                bmi.bmiHeader.biHeight = -fh;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                void *bits = NULL;
                HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
                if (hbm && bits) {
                    HGDIOBJ old_bm = SelectObject(hdc, hbm);
                    AddFontResourceExA("../FunctionX Bold.ttf", FR_PRIVATE, NULL);
                    HFONT hfont = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "FunctionX");
                    HGDIOBJ old_font = SelectObject(hdc, hfont);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    for (int ch = 32; ch < 128; ch++) {
                        int col = (ch - 32) % FONT_COLS;
                        int row = (ch - 32) / FONT_COLS;
                        char c = (char)ch;
                        RECT rc = { col * FONT_CELL + 2, row * FONT_CELL + 2,
                                    (col + 1) * FONT_CELL, (row + 1) * FONT_CELL };
                        DrawTextA(hdc, &c, 1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP);
                    }
                    GdiFlush();
                    memcpy(pixels, bits, (size_t)(fw * fh * 4));
                    for (int i = 0; i < fw * fh; i++) {
                        uint32_t p = pixels[i];
                        uint8_t b = (uint8_t)(p & 0xFF);
                        uint8_t g = (uint8_t)((p >> 8) & 0xFF);
                        uint8_t r = (uint8_t)((p >> 16) & 0xFF);
                        uint8_t a = (uint8_t)((r + g + b) / 3);
                        pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                    ((uint32_t)g << 8) | b;
                    }
                    SelectObject(hdc, old_font);
                    DeleteObject(hfont);
                    SelectObject(hdc, old_bm);
                    DeleteObject(hbm);
                }
                DeleteDC(hdc);
                s_font_page = SHARED_PAGE_FONT;
                frontend_init_font_metrics_from_pixels((const uint8_t *)pixels, fw, fh);
                if (td5_plat_render_upload_texture(s_font_page, pixels, fw, fh, 2)) {
                    TD5_LOG_I(LOG_TAG, "GDI font atlas loaded: page=%d %dx%d",
                              s_font_page, fw, fh);
                } else {
                    TD5_LOG_W(LOG_TAG, "GDI font atlas upload failed: page=%d %dx%d",
                              s_font_page, fw, fh);
                    s_font_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "GDI font atlas allocation failed");
            }
        }
    }

    /* ---- Small font (smalltext) — high-score / results table font ----
     * 252x132, 12x12 cells. NOTE: the extracted "smalltext.png" is BLACK-on-WHITE
     * (inverted), which would render opaque boxes; the "_white_on_black" variant is the
     * correctly-oriented atlas (black bg → keyed transparent, white glyphs → tintable).
     * Used by frontend_render_high_score_overlay (true lowercase + descenders). */
    if (s_smallfont_page < 0) {
        int sfw = 0, sfh = 0;
        if (frontend_load_tga_colorkey("smalltext_white_on_black.tga", "Front End/frontend.zip",
                                        SMALLFONT_PAGE, &sfw, &sfh, TD5_COLORKEY_BLACK)) {
            s_smallfont_page = SMALLFONT_PAGE;
            TD5_LOG_I(LOG_TAG, "Small font loaded: smalltext.tga page=%d %dx%d",
                      s_smallfont_page, sfw, sfh);
        } else {
            TD5_LOG_W(LOG_TAG, "smalltext.tga not found — high-score table will fall back to BodyText");
        }
    }

    /* ---- Vector (SDF) SmallText atlas (resolution-independent table text) ---- */
    if (g_td5.ini.vector_ui && s_ps_msdf && s_smallfont_msdf_page < 0) {
        void *pixels = NULL;
        int mw = 0, mh = 0;
        if (td5_asset_load_png_to_buffer("re/assets/frontend/smalltext_msdf.png",
                                          TD5_COLORKEY_NONE, &pixels, &mw, &mh)) {
            if (td5_plat_render_upload_texture(SHARED_PAGE_SMALLFONT_MSDF, pixels, mw, mh, 2)) {
                s_smallfont_msdf_page = SHARED_PAGE_SMALLFONT_MSDF;
                TD5_LOG_I(LOG_TAG, "SmallText SDF atlas loaded: page=%d %dx%d",
                          s_smallfont_msdf_page, mw, mh);
            }
            free(pixels);
        } else {
            TD5_LOG_W(LOG_TAG, "smalltext_msdf.png not found — SmallText falls back to bitmap");
        }
    }

    /* ---- ButtonBits (gradient source for button backgrounds) ----
     * 56x100 paletted texture (DAT_00496268 in original).
     * Layout: 3 sections of 32px each (style * 0x20 offset).
     * Black colorkey. */
    if (s_buttonbits_tex_page < 0) {
        s_buttonbits_tex_page = SHARED_PAGE_BUTTONBITS;
        {
            void *pixels = NULL;
            int bw = 0, bh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/ButtonBits.png",
                                              TD5_COLORKEY_BLACK, &pixels, &bw, &bh)) {
                if (td5_plat_render_upload_texture(s_buttonbits_tex_page, pixels, bw, bh, 2)) {
                    s_buttonbits_w = bw;
                    s_buttonbits_h = bh;
                    TD5_LOG_I(LOG_TAG, "ButtonBits loaded (PNG): page=%d %dx%d",
                              s_buttonbits_tex_page, bw, bh);
                } else {
                    s_buttonbits_tex_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "Failed to load ButtonBits.png");
                s_buttonbits_tex_page = -1;
            }
        }
    }

    /* ---- ArrowButtonz (left/right scroll arrows on selector buttons) ----
     * 12x36 sprite sheet (DAT_00496284 in original, FUN_00426260).
     * Red colorkey. */
    if (s_arrowbuttonz_tex_page < 0) {
        s_arrowbuttonz_tex_page = SHARED_PAGE_ARROWBTNZ;
        {
            void *pixels = NULL;
            int aw = 0, ah = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/ArrowButtonz.png",
                                              TD5_COLORKEY_RED, &pixels, &aw, &ah)) {
                if (td5_plat_render_upload_texture(s_arrowbuttonz_tex_page, pixels, aw, ah, 2)) {
                    TD5_LOG_I(LOG_TAG, "ArrowButtonz loaded (PNG): page=%d %dx%d",
                              s_arrowbuttonz_tex_page, aw, ah);
                } else {
                    s_arrowbuttonz_tex_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "Failed to load ArrowButtonz.png");
                s_arrowbuttonz_tex_page = -1;
            }
        }
    }

    /* ---- ButtonLights (selection indicator dot) ----
     * 16x32 texture. Two 16x16 frames stacked vertically.
     * Black colorkey. */
    if (s_buttonlights_tex_page < 0) {
        s_buttonlights_tex_page = SHARED_PAGE_BTNLIGHTS;
        {
            void *pixels = NULL;
            int lw = 0, lh = 0;
            if (td5_asset_load_png_to_buffer("re/assets/frontend/ButtonLights.png",
                                              TD5_COLORKEY_BLACK, &pixels, &lw, &lh)) {
                if (td5_plat_render_upload_texture(s_buttonlights_tex_page, pixels, lw, lh, 2)) {
                    s_buttonlights_w = lw;
                    s_buttonlights_h = lh;
                    TD5_LOG_I(LOG_TAG, "ButtonLights loaded (PNG): page=%d %dx%d",
                              s_buttonlights_tex_page, lw, lh);
                } else {
                    s_buttonlights_tex_page = -1;
                }
                free(pixels);
            } else {
                TD5_LOG_W(LOG_TAG, "ButtonLights.png not found (optional)");
                s_buttonlights_tex_page = -1;
            }
        }
    }

    /* ---- SnkMouse.TGA (cursor) ---- */
    if (s_cursor_tex_page < 0) {
        s_cursor_tex_page = SHARED_PAGE_CURSOR;
        /* SnkMouse.png has a red background → use red colorkey. */
        if (!frontend_load_tga_colorkey("snkmouse.tga", "Front End/frontend.zip",
                                         s_cursor_tex_page, &s_cursor_w, &s_cursor_h,
                                         TD5_COLORKEY_RED)) {
            TD5_LOG_W(LOG_TAG, "Failed to load snkmouse.tga cursor texture");
            s_cursor_tex_page = -1;
        } else {
            TD5_LOG_I(LOG_TAG, "Cursor texture loaded: page=%d %dx%d",
                      s_cursor_tex_page, s_cursor_w, s_cursor_h);
        }
    }

    /* Per-screen backgrounds are loaded on demand by each screen function.
     * No need to preload them here -- they go into the recyclable surface pool. */

    /* Create work surfaces for UI rendering */
    /* (Real implementation allocates DirectDraw surfaces) */

    /* Load frontend fonts (from Language.dll string table) */
    /* (Real implementation loads SNK_* string exports) */

    /* Initialize CD audio volume from saved settings */
    td5_plat_cd_set_volume(80);
    td5_sound_load_frontend_sfx();

    /* Car lock table: DAT_00463e4c (original binary).
     * Selector shows 23 cars (positions 0-22) in regular mode; DAT_00463e0c = 23.
     * Positions 0-20: unlocked (21 cars visible + selectable).
     *   Note: atp(16), ss1(17), 128(18), gtr(19), jag(20) are UNLOCKED in original.
     * Positions 21-22: visible but locked (cat=SUPER7, sp4=R390).
     * Positions 23-36: invisible in regular mode (cop-chase / cup unlock only). */
    /* Populate lock tables from save system (Config.td5 loaded by td5_save_init). */
    td5_save_get_car_lock_table(s_car_lock_table, TD5_BASE_CAR_COUNT);
    td5_save_get_track_lock_table(s_track_lock_table, 26);
    { int td6s; for (td6s = 26; td6s <= 36; td6s++) s_track_lock_table[td6s] = 0; } /* TD6 tracks always available */

    /* Compute total unlocked car count (visible + selectable in roster).
     * s_total_unlocked_cars = max visible car index (exclusive).
     * The original uses DAT_00463e0c which counts contiguous visible slots. */
    if (td5_save_get_all_cars_unlocked()) {
        s_total_unlocked_cars = 37;
    } else {
        s_total_unlocked_cars = td5_save_get_max_unlocked_car();
        if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21; /* minimum visible roster */
    }

    /* Compute total navigable track count. Race tracks (0-19) are always
     * navigable in the selector (locked ones show "LOCKED" but are visible).
     * Cup tracks (20-25) become navigable when unlocked via cup wins.
     * s_total_unlocked_tracks = exclusive upper bound for track cycling. */
    {
        int t;
        s_total_unlocked_tracks = 20; /* race tracks 0-19 always navigable */
        for (t = 20; t < 37; t++) {
            if (s_track_lock_table[t] == 0) /* 0 = unlocked in frontend table */
                s_total_unlocked_tracks = t + 1; /* extend range to include this cup track */
        }
    }
    TD5_LOG_I(LOG_TAG, "Progression: unlocked_cars=%d unlocked_tracks=%d cup_tier=0x%02X cheat_unlock=%d",
              s_total_unlocked_cars, s_total_unlocked_tracks, td5_save_get_cup_tier(), s_cheat_unlock_all);

    /* Background gallery slideshow (LoadExtrasGalleryImageSurfaces 0x40D590) */
    frontend_load_bg_gallery();

    return 1;
}

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

static void frontend_load_bg_gallery(void) {
    static const char * const png_names[5] = {
        "re/assets/extras/pic1.png",
        "re/assets/extras/pic2.png",
        "re/assets/extras/pic3.png",
        "re/assets/extras/pic4.png",
        "re/assets/extras/pic5.png"
    };
    if (s_bg_gal_loaded) return;
    for (int i = 0; i < 5; i++) {
        int page = SHARED_PAGE_BG_GALLERY + i;
        void *pixels = NULL; int w = 0, h = 0;

        /* Try PNG first */
        if (!td5_asset_load_png_to_buffer(png_names[i], TD5_COLORKEY_BLACK, &pixels, &w, &h)) {
            TD5_LOG_W(LOG_TAG, "BgGallery: failed to load %s", png_names[i]);
            continue;
        }

        if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
            s_bg_gallery[i].width  = w;
            s_bg_gallery[i].height = h;
            TD5_LOG_I(LOG_TAG, "BgGallery[%d]: %dx%d page=%d", i, w, h, page);
        }
        free(pixels);
    }
    s_bg_gal_current = 0;
    s_bg_gal_blend   = 0x100;
    s_bg_gal_x       = 140.0f;
    s_bg_gal_y       = 84.0f;
    s_bg_gal_loaded  = 1;
}

static void frontend_render_bg_gallery(float sx, float sy) {
    if (!s_bg_gal_loaded) return;
    int i = s_bg_gal_current;
    if (s_bg_gallery[i].width <= 0) return;

    /* Update blend weight: decrement 1 per frame (original g_attractModeIdleCounter--).
     * Starts at 256 on load/advance, triggers next image at -24 (~280 frames ≈ 4.7s @60fps) */
    s_bg_gal_blend--;
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
    s_arrowbuttonz_tex_page = -1;
    s_white_tex_page = -1;
    s_background_surface = 0;
    memset(s_title_tex_page, 0, sizeof(s_title_tex_page));
    memset(s_title_msdf_page, 0, sizeof(s_title_msdf_page));
    memset(s_title_tex_w, 0, sizeof(s_title_tex_w));
    memset(s_title_tex_h, 0, sizeof(s_title_tex_h));
}

/* ========================================================================
 * Render helpers (stubs)
 * ======================================================================== */

static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1); /* forward decl */

static void frontend_draw_value_text(float sx, float sy, int x, int y,
                                     const char *text, uint32_t color) {
    if (!text || !text[0] || s_font_page < 0) return;
    fe_draw_text((float)x * sx, (float)y * sy, text, color, sx, sy);
}

/* Draw a Quick Race value in the right-hand column at FE_QR_VALUE_SCALE (matched
 * to the button-caption size). If it would run past the right margin, wrap to a
 * second line below; the 1- or 2-line block is vertically centered on the
 * button row at row_btn_idx. */
static void frontend_draw_qr_value(float sx, float sy, int row_btn_idx,
                                   const char *text, uint32_t color) {
    if (row_btn_idx < 0 || row_btn_idx >= s_button_count) return;
    if (!text || !text[0] || s_font_page < 0) return;
    const float gs   = FE_QR_VALUE_SCALE;
    const float vx   = (float)FE_QR_VALUE_X * sx;
    const float avail = (float)(FE_QR_SCREEN_W - FE_QR_VALUE_X - FE_QR_RIGHT_MARGIN) * sx;
    const int   by   = s_buttons[row_btn_idx].y;

    /* Fits on one line — center it on the 32px button. */
    if (fe_measure_text(text, sx * gs) <= avail) {
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
        if (fe_measure_text(l1, sx * gs) <= avail) split = i;
        else break;
    }
    if (split > 0) {
        memcpy(l1, text, (size_t)split); l1[split] = '\0';
        snprintf(l2, sizeof(l2), "%s", text + split + 1);
    } else {
        int cut = 1;
        for (int i = 1; i <= len && i < (int)sizeof(l1); i++) {
            memcpy(l1, text, (size_t)i); l1[i] = '\0';
            if (fe_measure_text(l1, sx * gs) > avail) break;
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
    if (!text || !text[0] || s_font_page < 0) return;
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

static void frontend_render_quick_race_overlay(float sx, float sy) {
    char car_name[80];
    char track_name[80];
    char count[8];
    int car_locked;
    int track_locked;
    if (!s_anim_complete) return;
    if (s_button_count <= QR_BTN_LAPS) return;
    snprintf(car_name, sizeof(car_name), "%s", frontend_get_car_display_name(s_selected_car));
    frontend_get_track_display_name(s_selected_track, 0, track_name, sizeof(track_name));
    car_locked = (!s_cheat_unlock_all && !s_network_active &&
                  s_selected_car >= 0 && s_selected_car < 37 &&
                  s_car_lock_table[s_selected_car] != 0);
    track_locked = (!s_cheat_unlock_all && !s_network_active &&
                    s_selected_track >= 0 && s_selected_track < 37 &&
                    s_track_lock_table[s_selected_track] != 0);

    /* Each selected value renders in the right-hand value column at the same
     * glyph size as the button caption, vertically centered on its row, and
     * wraps to a second line if it would run off the right edge. Car/Track show
     * the name; Direction shows Forwards/Backwards (only when the row is
     * visible); Players/Opponents show the count; Laps shows value+1. */
    frontend_draw_qr_value(sx, sy, QR_BTN_CAR,   car_locked   ? "LOCKED" : car_name,   0xFFFFFFFF);
    frontend_draw_qr_value(sx, sy, QR_BTN_TRACK, track_locked ? "LOCKED" : track_name, 0xFFFFFFFF);
    if (!s_buttons[QR_BTN_DIRECTION].hidden) {
        frontend_draw_qr_value(sx, sy, QR_BTN_DIRECTION,
                               s_track_direction ? "Backwards" : "Forwards", 0xFFFFFFFF);
    }
    /* Players row is hidden (Quick Race is single-player); only Opponents shown. */
    if (!s_buttons[QR_BTN_PLAYERS].hidden) {
        snprintf(count, sizeof(count), "%d", s_num_human_players);
        frontend_draw_qr_value(sx, sy, QR_BTN_PLAYERS, count, 0xFFFFFFFF);
    }
    snprintf(count, sizeof(count), "%d", s_num_ai_opponents);
    frontend_draw_qr_value(sx, sy, QR_BTN_OPPONENTS, count, 0xFFFFFFFF);
    /* [S02 (c) 2026-06-04] Circuit laps value (displayed as laps+1, matching the
     * Game Options + Track Selection convention). Hidden on point-to-point tracks
     * (frontend_update_laps_button_visibility) — no laps there. */
    if (!s_buttons[QR_BTN_LAPS].hidden) {
        snprintf(count, sizeof(count), "%d", s_game_option_laps + 1);
        frontend_draw_qr_value(sx, sy, QR_BTN_LAPS, count, 0xFFFFFFFF);
    }
}

static void fe_draw_option_arrows(int btn_idx, float sx, float sy) {
    /* ArrowButtonz.tga: 12x36 sprite sheet (FUN_00426260, original DAT_00496284).
     * Four 12x9 rows (u spans full width = 0.0..1.0):
     *   Row 0 v=0.00..0.25  Left  arrow (lighter blue — not used)
     *   Row 1 v=0.25..0.50  Right arrow (lighter blue — not used)
     *   Row 2 v=0.50..0.75  Left  arrow (brighter blue)
     *   Row 3 v=0.75..1.00  Right arrow (brighter blue)
     * Use bottom rows (2/3) — brighter blue, matches original appearance.
     * Original FUN_00426260 always passes param_2=1 (confirmed @ 0x00422430). */
    float bx, by, bw, bh;
    float aw = 12.0f * sx, ah = 9.0f * sy;
    /* Skip hidden buttons (e.g. the Direction toggle on forward-only/circuit
     * tracks) — the selector arrows must vanish with the button frame+label,
     * not leave an empty ◄ ► row floating where the button used to be. */
    if (!s_buttons[btn_idx].active || s_buttons[btn_idx].hidden) return;
    frontend_get_button_render_rect(btn_idx, sx, sy, &bx, &by, &bw, &bh);

    /* VectorUI: procedural triangle-SDF arrows (crisp + AA at any resolution). */
    if (g_td5.ini.vector_ui && s_ps_arrow) {
        float aw2 = 13.0f * sx, ah2 = 13.0f * sy;
        float ay  = by + (bh - ah2) * 0.5f;
        uint32_t acol = 0xFF7995FFu;  /* bright selector blue */
        fe_draw_arrow_proc(bx + 4.0f * sx,          ay, aw2, ah2, 0 /*left */, acol);
        fe_draw_arrow_proc(bx + bw - 4.0f*sx - aw2, ay, aw2, ah2, 1 /*right*/, acol);
        return;
    }

    if (s_arrowbuttonz_tex_page < 0) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
    fe_draw_quad(bx + 4.0f * sx,          by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, 0.50f, 1.0f, 0.75f);
    fe_draw_quad(bx + bw - 4.0f*sx - aw, by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, 0.75f, 1.0f, 1.00f);
}

static void frontend_render_game_options_overlay(float sx, float sy) {
    const char *on_off[] = { "OFF", "ON" };
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
    frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, on_off[s_game_option_traffic & 1], 0xFFFFFFFF);
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

static void frontend_music_test_update_track_label(void) {
    int idx = s_music_test_track_idx;
    if (idx < 0)  idx = 0;
    if (idx > 11) idx = 11;
    /* format: "%d. %s" [CONFIRMED @ 0x465f74: "%d. %s"] */
    snprintf(s_music_test_track_label, sizeof(s_music_test_track_label),
             "%d. %s", idx + 1, k_music_test_band[idx]);
}

static void frontend_music_test_update_now_playing(int idx) {
    if (idx < 0)  idx = 0;
    if (idx > 11) idx = 11;
    snprintf(s_music_test_now_band,  sizeof(s_music_test_now_band),
             "%s", k_music_test_band[idx]);
    snprintf(s_music_test_now_title, sizeof(s_music_test_now_title),
             "%s", k_music_test_title[idx]);
    s_music_test_playing_set = 1;
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

    /* [S05 2026-06-04] MULTIPLAYER sub-header, drawn in the shared menu font
     * (fe_draw_text_centered — the same path used by every other screen title
     * and the lobby's MULTIPLAYER label) so it matches the rest of the menus'
     * format. Sits BELOW the shared "OPTIONS" title strip (OptionsText.tga,
     * native y~17) and ABOVE the first row button (PLAYERS @ y=77) so the two
     * don't overlap — it reads as "OPTIONS / MULTIPLAYER". */
    fe_draw_text_centered(320.0f * sx, 60.0f * sy, SNK_MultiplayerTitleTxt,
                          0xFFFFD000, sx * 0.8f, sy * 0.8f);

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
        if (fe_measure_text(dn, sx*ts) <= panel_w) {
            fe_draw_text_centered(cx, 112.0f * sy, dn, 0xFFFFFFFF, sx*ts, sy*ts);
        } else {
            char l1[48], l2[48];
            int n = (int)strlen(dn), mid = n/2, best = -1, i, cut, skip, a, b;
            for (i = 1; i < n - 1; i++) {
                int di, db;
                if (dn[i] != ' ') continue;
                if (best < 0) { best = i; continue; }
                di = i - mid;    if (di < 0) di = -di;
                db = best - mid; if (db < 0) db = -db;
                if (di < db) best = i;
            }
            cut = (best >= 0) ? best : mid;
            skip = (dn[cut] == ' ') ? 1 : 0;
            a = cut;              if (a > 47) a = 47;
            memcpy(l1, dn, (size_t)a); l1[a] = 0;
            b = n - (cut + skip); if (b > 47) b = 47;
            memcpy(l2, dn + cut + skip, (size_t)b); l2[b] = 0;
            fe_draw_text_centered(cx, 108.0f * sy, l1, 0xFFFFFFFF, sx*ts, sy*ts);
            fe_draw_text_centered(cx, 124.0f * sy, l2, 0xFFFFFFFF, sx*ts, sy*ts);
        }
    }

    idx = (s_inner_state >= 6 && s_inner_state <= 12) ? k_cup_to_idx[btn] : k_top_to_idx[btn];
    if (idx < 0) return;  /* "Back" has no description */

    /* Line 0: race-type NAME, big font, centered at Y=0. */
    fe_draw_text(panel_x + (panel_w - fe_measure_text(k_race_desc[idx][0], sx)) * 0.5f,
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
        fe_draw_small_text(panel_x + (panel_w - fe_measure_small_text(s) * sx) * 0.5f,
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

static void frontend_load_car_spec_fields(int car_index) {
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
    float vx = hx + fe_measure_small_text("COMPRESSION:") * sx + 16.0f * sx;
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
#define FE_CARSEL_SLIDE_IN_MS 1250

/* [ARCH-DIVERGENCE: DDraw QueueFrontendOverlayRect -> D3D11 fe_draw_surface_rect; L5 sweep 2026-05-21]
 *   Port reimplements DrawCarSelectionPreviewOverlay (0x0040DDC0) using D3D11
 *   batched quads instead of DDraw blit-queue. Same animation phases (state 0
 *   static / state 11=0xB slide-out / state 14=0xE slide-in), same coordinate
 *   constants (0x198 width, 0x118 height, 0x5A alpha, 0x40/0x20 step deltas,
 *   0x4A8 offscreen offset). DDraw color-key + per-frame surface tracking
 *   replaced by tex-page + alpha blending. */
static void frontend_render_car_selection_preview(float sx, float sy) {
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
             * new car's mask). Extra gates: cached car paintable (defence in depth
             * against a stale mask) + prev_surface>0 so the body rides a sliding
             * carpic rather than sitting pinned at centre. */
            if (show_paint && s_car_preview_prev_surface > 0 &&
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

/* Load the start/finish dot table once. File is optional: if absent (e.g. the
 * tool hasn't been run), dots are silently skipped and the previews render as
 * before. */
static void frontend_load_track_markers(void) {
    FILE *f;
    char magic[4];
    uint32_t count;
    if (s_track_markers_loaded != 0) return;
    s_track_markers_loaded = -1;
    f = fopen("re/assets/tracks/trak_markers.dat", "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "trak_markers.dat not found; track start/finish dots disabled");
        return;
    }
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "TMK1", 4) == 0 &&
        fread(&count, sizeof(count), 1, f) == 1) {
        int n = (count > 20) ? 20 : (int)count;
        int i, ok = 1;
        for (i = 0; i < n; i++) {
            float v[4];
            uint8_t b[4];
            if (fread(v, sizeof(float), 4, f) != 4 || fread(b, 1, 4, f) != 4) { ok = 0; break; }
            s_track_markers[i].start_u = v[0];
            s_track_markers[i].start_v = v[1];
            s_track_markers[i].end_u   = v[2];
            s_track_markers[i].end_v   = v[3];
            s_track_markers[i].circuit = b[0];
        }
        if (ok) {
            s_track_markers_loaded = 1;
            TD5_LOG_I(LOG_TAG, "loaded %d track start/finish markers", n);
        }
    }
    fclose(f);
}

/* Same as above for migrated TD6 tracks (trak_markers_td6.dat). Entry i maps to
 * preview TGA number (TD6_PREVIEW_TGA_BASE + i). Optional: absent file just means
 * no TD6 start dots, identical to the TD5-only behaviour. */
static void frontend_load_track_markers_td6(void) {
    FILE *f;
    char magic[4];
    uint32_t count;
    if (s_track_markers_td6_loaded != 0) return;
    s_track_markers_td6_loaded = -1;
    f = fopen("re/assets/tracks/trak_markers_td6.dat", "rb");
    if (!f) return;
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "TMK1", 4) == 0 &&
        fread(&count, sizeof(count), 1, f) == 1) {
        int n = (count > TD6_MARKER_MAX) ? TD6_MARKER_MAX : (int)count;
        int i, ok = 1;
        for (i = 0; i < n; i++) {
            float v[4];
            uint8_t b[4];
            if (fread(v, sizeof(float), 4, f) != 4 || fread(b, 1, 4, f) != 4) { ok = 0; break; }
            s_track_markers_td6[i].start_u = v[0];
            s_track_markers_td6[i].start_v = v[1];
            s_track_markers_td6[i].end_u   = v[2];
            s_track_markers_td6[i].end_v   = v[3];
            s_track_markers_td6[i].circuit = b[0];
        }
        if (ok) {
            s_track_markers_td6_loaded = 1;
            TD5_LOG_I(LOG_TAG, "loaded %d TD6 track start/finish markers", n);
        }
    }
    fclose(f);
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
        char vb[8];
        float vx = 350.0f * sx;
        if (s_buttons[2].active) { snprintf(vb, sizeof vb, "%d", s_num_ai_opponents);
            fe_draw_text(vx, (float)(s_buttons[2].y + 6) * sy, vb, 0xFFFFFFFF, sx*0.8f, sy*0.8f); }
        if (s_buttons[3].active && !s_buttons[3].hidden) { snprintf(vb, sizeof vb, "%d", s_game_option_laps + 1);
            fe_draw_text(vx, (float)(s_buttons[3].y + 6) * sy, vb, 0xFFFFFFFF, sx*0.8f, sy*0.8f); }
        if (s_buttons[4].active)
            fe_draw_text(vx, (float)(s_buttons[4].y + 6) * sy, on_off[s_game_option_traffic & 1], 0xFFFFFFFF, sx*0.8f, sy*0.8f);
        if (s_buttons[5].active)
            fe_draw_text(vx, (float)(s_buttons[5].y + 6) * sy, on_off[s_game_option_cops & 1], 0xFFFFFFFF, sx*0.8f, sy*0.8f);
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
        /* Full enumerated device name (e.g. "Logitech G29 Racing Wheel") to the
         * right of the icon. If it's wider than the column, wrap to two lines at
         * the space nearest the middle. */
        const char *dname = td5_input_get_device_name(source);
        float ts   = 0.85f;
        float nx   = 466.0f * sx;
        float maxw = 168.0f * sx;          /* column from x~466 to ~634 */
        if (!dname || !dname[0]) dname = "<NONE>";
        if (fe_measure_text(dname, sx * ts) <= maxw) {
            fe_draw_text(nx, (177.0f + 8.0f) * sy, dname, 0xFFFFFFFF, sx * ts, sy * ts);
        } else {
            char l1[64], l2[64];
            int n = (int)strlen(dname), mid = n / 2, best = -1, i, cut, skip, a, b;
            for (i = 1; i < n - 1; i++) {
                int di, db;
                if (dname[i] != ' ') continue;
                if (best < 0) { best = i; continue; }
                di = i - mid;    if (di < 0) di = -di;
                db = best - mid; if (db < 0) db = -db;
                if (di < db) best = i;
            }
            cut  = (best >= 0) ? best : mid;
            skip = (dname[cut] == ' ') ? 1 : 0;
            a = cut;                if (a > 63) a = 63;
            memcpy(l1, dname, (size_t)a); l1[a] = 0;
            b = n - (cut + skip);   if (b > 63) b = 63;
            memcpy(l2, dname + cut + skip, (size_t)b); l2[b] = 0;
            fe_draw_text(nx, (177.0f +  1.0f) * sy, l1, 0xFFFFFFFF, sx * ts, sy * ts);
            fe_draw_text(nx, (177.0f + 17.0f) * sy, l2, 0xFFFFFFFF, sx * ts, sy * ts);
        }
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

    snprintf(hdr, sizeof hdr, "CONTROLLER SETUP - PLAYER %d", s_ctrl_player + 1);
    fe_draw_text_centered(320.0f * sx, 34.0f * sy, hdr, 0xFFCCCCCC, sx * 0.75f, sy * 0.75f);
    hint = s_ctrl_capturing
        ? (s_ctrl_capture_armed
            ? "PRESS A KEY / BUTTON / AXIS   (ESC = CANCEL)"
            : "RELEASE TO CONTINUE...   (ESC = CANCEL)")
        : "SELECT AN ACTION TO REMAP   -   REMAP ALL = ONE BY ONE";
    fe_draw_text_centered(320.0f * sx, 350.0f * sy, hint, 0xFF999999, sx * 0.58f, sy * 0.58f);
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
static void frontend_render_mp_lobby_overlay(float sx, float sy) {
    int p;
    if (!s_anim_complete) return;

    fe_draw_text_centered(320.0f * sx,  40.0f * sy, SNK_MultiplayerTitleTxt, 0xFFFFD000, sx, sy);
    fe_draw_text_centered(320.0f * sx,  78.0f * sy, SNK_PressJoinTxt, 0xFFCCCCCC, sx*0.8f, sy*0.8f);

    for (p = 0; p < s_mp_joined_count && p < TD5_MAX_HUMAN_PLAYERS; p++) {
        char line[80];
        const char *dev = td5_input_get_device_name(s_mp_join_device[p]);
        if (!dev || !dev[0]) dev = "?";
        snprintf(line, sizeof line, "PLAYER %d:  %s  -  %s", p + 1, dev, SNK_ReadyTxt);
        fe_draw_text(120.0f * sx, (float)(110 + p * 24) * sy, line, 0xFF33FF33u, sx*0.8f, sy*0.8f);
    }
    if (s_mp_joined_count == 0)
        fe_draw_text_centered(320.0f * sx, 150.0f * sy, "( no players yet )", 0xFF888888, sx*0.8f, sy*0.8f);

    fe_draw_text_centered(320.0f * sx, 340.0f * sy,
                          "ENTER / A = JOIN     SPACE = START     ESC / B = BACK",
                          0xFF999999, sx*0.62f, sy*0.62f);
}

static void frontend_format_score_time(char *buf, size_t cap, int raw_ticks, int type) {
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

    const TD5_NpcGroup *grp = td5_save_get_npc_group(s_score_category_index);
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
    #define HS_SF_CTR(L,R,S) ((115.0f + (float)(L) + ((float)((R)-(L)) - fe_measure_small_text(S)) * 0.5f) * sx)

    if (!grp) {
        const char *msg = "NO SCORES YET";
        fe_draw_small_text((320.0f * sx) - (fe_measure_small_text(msg) * 0.5f) * sx,
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
#define FE_CREDITS_ROW_H    32.0f   /* 0x20 per text row */
#define FE_CREDITS_PHOTO_H  224.0f  /* mugshot height (= 7 rows) */
#define FE_CREDITS_SPEED    0.060f  /* px/ms = 60px/s = orig 1px/frame @60fps. ScreenExtrasGallery
                                     * @0x417d50: g_frontendAnimFrameCounter +1/frame drives the reel
                                     * srcY; a 32px row composites every (counter&0x1f)==0 = 32 frames,
                                     * synced to the row scrolling past in 32 frames. */

/* total scroll-column height of all credit rows (used by the handler to know when done) */
static float frontend_credits_total_height(void) {
    float cy = 0.0f;
    for (int i = 0; i < K_CREDITS_COUNT; i++)
        cy += (k_credits[i][0] == '#') ? FE_CREDITS_PHOTO_H : FE_CREDITS_ROW_H;
    return cy;
}

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
static float fe_measure_text_width(const char *text, float sx) {
    float w = 0.0f;
    if (!text) return 0.0f;
    for (int i = 0; text[i]; i++) {
        int c = toupper((unsigned char)text[i]);
        if (c < 32 || c > 127) { w += 14.0f * sx; continue; }
        w += (float)s_font_glyph_advance[c - 0x20] * sx;
    }
    return w;
}

static void fe_draw_text_centered(float center_x, float y, const char *text,
                                  uint32_t color, float sx, float sy) {
    float w = fe_measure_text_width(text, sx);
    fe_draw_text(center_x - w * 0.5f, y, text, color, sx, sy);
}

static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy) {
    /* Font atlas: BodyText.tga (or GDI fallback), 10 chars/row, 24x24 cells.
     * Layout: col = (ascii - 0x20) % 10, row = (ascii - 0x20) / 10.
     * Source rect: (col*24, row*24, 24, 24). From Ghidra FUN_00424560. */
    if (s_font_page < 0 || !text) return;
    /* Resolution-independent MSDF path: same grid/UV/metrics, swapped page +
     * shader. Falls back to the bitmap atlas when VectorUI is off or the MSDF
     * assets failed to load. */
    int msdf = (g_td5.ini.vector_ui && s_msdf_font_page >= 0 && s_ps_msdf != NULL);
    int page = msdf ? s_msdf_font_page : s_font_page;
    float cx = x;
    float texel_w = 1.0f / (float)FONT_TEX_W;
    float cell_h = (float)FONT_CELL * sy;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    if (msdf) td5_plat_render_set_ps_override((void *)s_ps_msdf, SAMP_LINEAR_CLAMP);
    for (int i = 0; text[i]; i++) {
        int c = s_fe_preserve_case ? (unsigned char)text[i] : toupper((unsigned char)text[i]);
        int glyph_index;
        float glyph_advance_px;
        float glyph_advance;
        float glyph_w;
        if (c < 32 || c > 127) { cx += 14.0f * sx; continue; }
        glyph_index = c - 0x20;
        glyph_advance_px = (float)s_font_glyph_advance[glyph_index];
        glyph_advance = glyph_advance_px * sx;
        if (c == ' ') { cx += glyph_advance; continue; }
        int col = glyph_index % FONT_COLS;
        int row = glyph_index / FONT_COLS;
        float u0 = (float)(col * FONT_CELL) / (float)FONT_TEX_W;
        float u1 = u0 + glyph_advance_px * texel_w;
        float v0 = (float)(row * FONT_CELL) / (float)FONT_TEX_H;
        float v1 = (float)((row + 1) * FONT_CELL) / (float)FONT_TEX_H;
        glyph_w = glyph_advance_px * sx;
        fe_draw_quad(cx, y, glyph_w, cell_h, color, page, u0, v0, u1, v1);
        cx += glyph_advance;
    }
    if (msdf) td5_plat_render_clear_ps_override();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* Sum of small-font advances (unscaled font px) — for centering. Preserves case. */
static float fe_measure_small_text(const char *text) {
    float w = 0.0f;
    if (!text) return 0.0f;
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
static void fe_draw_small_text(float x, float y, const char *text, uint32_t color, float sx, float sy) {
    if (s_smallfont_page < 0 || !text) return;
    /* Resolution-independent SDF path: same 21x11 grid/UV/metrics, swapped page
     * + shader. Falls back to the bitmap atlas when VectorUI is off or the SDF
     * atlas failed to load. */
    int sdf = (g_td5.ini.vector_ui && s_smallfont_msdf_page >= 0 && s_ps_msdf != NULL);
    int page = sdf ? s_smallfont_msdf_page : s_smallfont_page;
    float cx = x;
    float cell_w = (float)SMALLFONT_CELL * sx;
    float cell_h = (float)SMALLFONT_CELL * sy;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    if (sdf) td5_plat_render_set_ps_override((void *)s_ps_msdf, SAMP_LINEAR_CLAMP);
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
    if (sdf) td5_plat_render_clear_ps_override();
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
#define FE_ARGB_TO_RGB(dst, argb) do {                       \
        (dst)[0] = (((argb) >> 16) & 0xFF) / 255.0f;         \
        (dst)[1] = (((argb) >>  8) & 0xFF) / 255.0f;         \
        (dst)[2] = ( (argb)        & 0xFF) / 255.0f;         \
    } while (0)

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
    FE_ARGB_TO_RGB(rp.mid,   mid_argb);   rp.mid[3]   = 1.0f;
    FE_ARGB_TO_RGB(rp.inner, inner_argb); rp.inner[3] = 1.0f;
    FE_ARGB_TO_RGB(rp.outer, outer_argb); rp.outer[3] = 1.0f;
    FE_ARGB_TO_RGB(rp.fill,  fill_argb);  rp.fill[3]  = fill_alpha;
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
    FE_ARGB_TO_RGB(rp.mid, color);  rp.mid[3] = 1.0f;
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
                                  int state, float sx, float sy) {
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
        uint32_t fill = 0xFF392152u;
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
static void fe_draw_button_frame(float bx, float by, float bw, float bh,
                                 int bb_state, float sx, float sy) {
    int use_proc = (g_td5.ini.vector_ui && s_ps_roundrect && s_rr_cb);
    if (use_proc) {
        /* Border 3-stop gradient + interior fill, colours per state (matches the
         * button render loop exactly). Selected interior = dark purple 0x392152;
         * unselected/locked have a transparent interior (border only). */
        uint32_t mid_c, inner_c, outer_c;
        if (bb_state == 0)      { mid_c = 0xFFD9CA00u; inner_c = 0xFFA08C00u; outer_c = 0xFF3C2F00u; }
        else if (bb_state == 2) { mid_c = 0xFFAAAAAAu; inner_c = 0xFF777777u; outer_c = 0xFF222222u; }
        else                    { mid_c = 0xFF7995FFu; inner_c = 0xFF496BDCu; outer_c = 0xFF001675u; }
        float fillA = (bb_state == 0) ? 1.0f : 0.0f;
        fe_draw_roundrect(bx, by, bw, bh,
                          20.0f * sy /*large TL/BR*/, 5.0f * sy /*small TR/BL*/,
                          6.0f * sy  /*side border*/, 2.0f * sy /*top/bottom border*/,
                          mid_c, inner_c, outer_c, 0xFF392152u, fillA);
    } else if (s_buttonbits_tex_page >= 0 && s_buttonbits_w > 0 && s_buttonbits_h > 0) {
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
        fe_draw_button_9slice(bx, by, bw, bh, bb_state, sx, sy);
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    }
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
    return fe_measure_text_width(s, sx);
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

#define VUI_ARGB4(dst, argb) do {                                   \
        (dst)[0] = (((argb) >> 16) & 0xFF) / 255.0f;                \
        (dst)[1] = (((argb) >>  8) & 0xFF) / 255.0f;                \
        (dst)[2] = ( (argb)        & 0xFF) / 255.0f;                \
        (dst)[3] = (((argb) >> 24) & 0xFF) / 255.0f;                \
    } while (0)

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
    VUI_ARGB4(gp.face,  g->face_color);
    VUI_ARGB4(gp.inner, g->inner_color);
    VUI_ARGB4(gp.tick,  g->tick_color);
    VUI_ARGB4(gp.red,   g->redline_color);
    VUI_ARGB4(gp.pivot, g->pivot_color);
#undef VUI_ARGB4

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
static void frontend_render_session_locked_overlay(float sx, float sy) {
    /* Only draw during states 4-5 (dialog visible) [CONFIRMED @ 0x41D630] */
    if (s_inner_state < 4) return;

    float dlg_x = (320.0f - 168.0f) * sx;
    float dlg_y = (240.0f - 143.0f) * sy;
    float dlg_w = 408.0f * sx;
    float dlg_cx = dlg_x + dlg_w * 0.5f;

    /* [FIXED 2026-06-01] NO box — color-keyed-black transparent panel, text only (same
     * pipeline as the cup dialogs). The prior 0xCC000000 translucent quad was a port invention. */

    /* "SORRY" at y=0x00 [CONFIRMED Language.dll: SorryTxt = "SORRY"] */
    fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "SORRY",          0xFFFFFFFF, sx, sy);
    /* "SESSION LOCKED" at y=0x38=56 [CONFIRMED Language.dll: SeshLockedTxt = "SESSION LOCKED"] */
    fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, "SESSION LOCKED", 0xFFFFFFFF, sx, sy);

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

    /* Roster panel backdrop (left column). */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(40.0f * sx, 96.0f * sy, 340.0f * sx, 220.0f * sy, 0xC0101C30, -1, 0, 0, 0, 0);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    fe_draw_text(56.0f * sx, 106.0f * sy, "PLAYERS IN LOBBY", 0xFF00FF00, sx, sy);
    for (slot = 0; slot < TD5_NET_MAX_PLAYERS; slot++) {
        const char *name;
        int lat;
        if (!td5_net_is_slot_active(slot)) continue;
        name = td5_net_get_slot_name(slot);
        if (!name[0]) name = "Player";
        lat = td5_net_get_slot_latency_ms(slot);
        if (slot == td5_net_local_slot())
            snprintf(line, sizeof(line), "%d. %s (you)", slot + 1, name);
        else if (lat >= 0)
            snprintf(line, sizeof(line), "%d. %s   %dms", slot + 1, name, lat);
        else
            snprintf(line, sizeof(line), "%d. %s   --", slot + 1, name);
        fe_draw_text(60.0f * sx, (136.0f + row * 22.0f) * sy, line, 0xFFFFFFFF, sx, sy);
        row++;
    }
    /* Host/connect status line at the bottom of the panel. */
    status = td5_net_get_status_text();
    if (status[0])
        fe_draw_small_text(56.0f * sx, 298.0f * sy, status, 0xFFA8C0E0, sx, sy);
}

/* S10b: the OPTIONS modal is drawn in a POST-button pass (the action buttons are
 * rendered after the per-screen overlay, so drawing the modal in the overlay let
 * the buttons paint over it). Called from td5_frontend_render_ui_rects after the
 * button loop so it covers everything. */
static void frontend_render_lobby_modal(float sx, float sy) {
    char buf[72], mask[33];
    int n, k;
    if (!s_lobby_modal) return;

    /* Heavy backdrop so the whole lobby (incl. buttons) is hidden behind it. */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xE6000000, -1, 0, 0, 0, 0);
    fe_draw_quad(150.0f * sx, 140.0f * sy, 340.0f * sx, 200.0f * sy, 0xF8102845, -1, 0, 0, 0, 0);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    fe_draw_text_centered(320.0f * sx, 154.0f * sy, "GAME OPTIONS", 0xFFFFD040, sx, sy);
    snprintf(buf, sizeof(buf), "MAX PLAYERS:  < %d >", s_lobby_max_players);
    fe_draw_text_centered(320.0f * sx, 196.0f * sy, buf, 0xFFFFFFFF, sx, sy);
    n = (int)strlen(s_lobby_password);
    if (n > 32) n = 32;
    for (k = 0; k < n; k++) mask[k] = '*';
    mask[n] = '\0';
    snprintf(buf, sizeof(buf), "PASSWORD: %s_", mask);
    fe_draw_text_centered(320.0f * sx, 226.0f * sy, buf, 0xFFFFFFFF, sx, sy);
    fe_draw_small_text(180.0f * sx, 286.0f * sy,
                       "<- -> set MAX   -   type PASSWORD", 0xFFB0B0B0, sx, sy);
    fe_draw_small_text(180.0f * sx, 306.0f * sy,
                       "ENTER = done    ESC = cancel", 0xFFB0B0B0, sx, sy);
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
        if (s_inner_state == 2 && s_text_input_state != 0) frontend_render_text_input();
        break;
    case TD5_SCREEN_DIRECT_CONNECT:
        /* Only during the IP-entry (3) / password-entry (8) sub-states. */
        if ((s_inner_state == 3 || s_inner_state == 8) && s_text_input_state != 0)
            frontend_render_text_input();
        break;
    case TD5_SCREEN_NETWORK_LOBBY:
        frontend_render_network_lobby_overlay(sx, sy);
        break;
    case TD5_SCREEN_RACE_RESULTS:
        frontend_render_race_results_overlay(sx, sy);
        break;
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
        /* Phase 6: prefer the baked button cache (single quad per button)
         * when available. The cache holds both halves stacked vertically
         * (top = state 1 unselected, bottom = state 0 selected); UV picks
         * by focus. Disabled state still falls through to the per-frame
         * 9-slice path because state 2 isn't baked. */
        int bb_state;
        int focused = (i == s_selected_button);
        if (s_buttons[i].disabled)            bb_state = 2;
        else if (focused || flash_active)     bb_state = 0;
        else                                   bb_state = 1;

        /* Eligibility for surface caching: any active, non-disabled,
         * non-selector button with a non-empty static label. Selectors
         * draw arrows + value text on top of the frame and would need a
         * different cache layout; disabled buttons render with state 2
         * (gray) which isn't baked. */
        /* Procedural neon button frame (VectorUI): crisp glow at any resolution.
         * Draws the dark rounded interior + glowing coloured rim, then the label
         * via the SDF text path. Selectors get only the frame (their value text
         * + arrows are drawn separately below). */
        int use_proc = (g_td5.ini.vector_ui && s_ps_roundrect && s_rr_cb);

        int cache_page = -1;
        if (!use_proc && !s_buttons[i].disabled && !s_buttons[i].is_selector
            && s_buttons[i].label[0] && s_font_page >= 0) {
            cache_page = td5_fe_btncache_ensure_page(i, s_buttons[i].label,
                                                    s_buttons[i].w, s_buttons[i].h,
                                                    s_font_glyph_advance);
        }

        if (use_proc) {
            /* Neon roundrect frame (gold/blue/gray per state) via the shared
             * fe_draw_button_frame() helper — the text-input field uses the same
             * helper so it matches. */
            fe_draw_button_frame(bx, by, bw, bh, bb_state, sx, sy);
            if (s_buttons[i].label[0] && !s_buttons[i].is_selector && s_font_page >= 0) {
                float tw = fe_measure_text(s_buttons[i].label, sx);
                uint32_t tc = s_buttons[i].disabled ? 0xFF888888u : 0xFFFFFFFFu;
                fe_draw_text(bx + (bw - tw) * 0.5f, by, s_buttons[i].label, tc, sx, sy);
            }
        } else if (cache_page >= 0) {
            /* Cache is 224x64 with halves stacked at row 32. Inset v at
             * the half boundary by half a texel so LINEAR filtering does
             * not blend row 31 (other half) with row 32. u stays at the
             * cache edges -- corners fully cover cols 0..25 and 196..223
             * with opaque content, so there is no need to inset u. */
            const float kV = 0.5f / 64.0f;
            float v0 = (bb_state == 0) ? (0.5f + kV) : 0.0f;
            float v1 = (bb_state == 0) ? 1.0f         : (0.5f - kV);
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(bx, by, bw, bh, 0xFFFFFFFF, cache_page,
                         0.0f, v0, 1.0f, v1);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        } else if (s_buttonbits_tex_page >= 0 && s_buttonbits_w > 0 && s_buttonbits_h > 0) {
            /* No opaque fill — original blits button surface to screen with
             * DDBLT_KEYSRC (black = transparent). We draw only the 9-slice
             * frame with alpha blending; background shows through naturally.
             * Routed through the shared helper (9-slice path) for parity. */
            fe_draw_button_frame(bx, by, bw, bh, bb_state, sx, sy);

            if (s_buttons[i].label[0] && s_font_page >= 0) {
                float text_w = fe_measure_text(s_buttons[i].label, sx);
                float tx = bx + (bw - text_w) * 0.5f;
                float ty = by;
                uint32_t text_color = 0xFFFFFFFF;
                if (s_buttons[i].disabled) text_color = 0xFF888888;
                fe_draw_text(tx, ty, s_buttons[i].label, text_color, sx, sy);
            }
        } else {
            fe_draw_quad(bx, by, bw, bh, bg_color, -1, 0, 0, 1, 1);
            if (s_buttons[i].label[0] && s_font_page >= 0) {
                float text_w = fe_measure_text(s_buttons[i].label, sx);
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
            break;
        case TD5_SCREEN_TRACK_SELECTION:
            /* Track(0) selector + the race-option rows (AI/laps/traffic/police =
             * buttons 2..5). [PORT ENHANCEMENT 2026-06] */
            fe_draw_option_arrows(0, sx, sy);
            fe_draw_option_arrows(2, sx, sy);
            fe_draw_option_arrows(3, sx, sy);
            fe_draw_option_arrows(4, sx, sy);
            fe_draw_option_arrows(5, sx, sy);
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
        frontend_render_lobby_modal(sx, sy);

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
        float tnw = fe_measure_text(track_name, sx);
        float tx = nav_bx + (nav_bw - tnw) * 0.5f;
        float ty = nav_by;
        fe_draw_text(tx, ty, track_name, 0xFFFFFFFF, sx, sy);
        fe_draw_option_arrows(0, sx, sy);
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
        float cnw = fe_measure_text(car_name, sx);
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

    if (title_visible && frontend_ensure_title_texture(s_current_screen)) {
        int page = s_title_tex_page[s_current_screen];
        int title_w = s_title_tex_w[s_current_screen];
        int title_h = s_title_tex_h[s_current_screen];
        if (page > 0 && title_w > 0 && title_h > 0) {
            float title_x = 120.0f * sx; /* original: uVar4-200 = screenW/2-200 = 120 [0x4213D0, 0x41D890, others] */
            float title_y = frontend_get_title_render_y(sy);
            float draw_w = (float)title_w * sx;
            float draw_h = (float)title_h * sy;
            /* [FIXED 2026-06-01 v2/v3] FAITHFUL menu-header title geometry.
             * The title is the menu-header label surface (CreateMenuStringLabelSurface) drawn
             * native-size (NO scaling): for English it's the TGA at its native W/H. The prior
             * High Scores hack (379x24 @154,29) was measured from the widescreen-PATCHED
             * TD5_d3d.exe, which stretches the whole 640x480 frontend ~1.23x H / ~1.275x V at
             * present — that stretch must NOT be baked into native coords. The faithful draw is
             * QueueFrontendOverlayRect(halfW-200=120, (halfH-0x9f)-YOffset-0x40, nativeW, nativeH).
             * RESTING Y = (240-0x9f-0)-0x40 = 17 for EVERY menu-header screen (audited across
             * screens 6,7,8,9,10,11,13,18,19,21,23 — all share this exact CreateMenuStringLabelSurface
             * formula; runtime-confirmed for 23: globals @0x4962c4 = 308/20/0). Only the MAIN MENU
             * uses a separate 248x20 strip resting at 21 (frontend_get_title_render_y base). So
             * shift every non-main-menu title up 4px; the slide-in shape is preserved (the orig
             * slide also ends at 17: anim*4 - 0xdc + (halfH-0x9f) lands on 17). */
            /* ALL menu-header titles — INCLUDING the main menu — rest at native 17 =
             * (halfH-0x9f)-YOffset-0x40. Confirmed by decomp of every screen fn (main menu
             * @0x415490 case 4 draws the title at (iVar5-YOffset)-0x40, iVar5=halfH-0x9f=81 => 17;
             * same formula on screens 6,7,8,9,10,11,12,13,14,21,23...). The shared resting base of
             * 21 came from a widescreen-PATCHED Frida capture (stretched ~1.275x V, untrusted per
             * the stretch trap). Shift every title up 4px; the slide shape is preserved (orig slide
             * anim*4-0xdc+(halfH-0x9f) lands on 17, == render_y(-135..21) - 4 = -139..17). */
            title_y -= 4.0f * sy; /* resting 21 -> 17 */
            /* Car Selection's menu-header title is left-aligned at native x = halfW-0x110 = 48
             * (ScreenCarSelection @0x40dfc0 uses uVar10-0x110, not the usual halfW-200=120). */
            if (s_current_screen == TD5_SCREEN_CAR_SELECTION) {
                title_x = 48.0f * sx;
            }
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            int tmsdf = (g_td5.ini.vector_ui && s_ps_msdf &&
                         s_title_msdf_page[s_current_screen] > 0);
            if (tmsdf) {
                /* SDF strip carries no colour, so tint with the menu-header
                 * yellow (227,215,8 = mainfont glyph colour). */
                td5_plat_render_set_ps_override((void *)s_ps_msdf, SAMP_LINEAR_CLAMP);
                fe_draw_quad(title_x, title_y, draw_w, draw_h,
                             0xFFE3D708u, s_title_msdf_page[s_current_screen],
                             0.0f, 0.0f, 1.0f, 1.0f);
                td5_plat_render_clear_ps_override();
            } else {
                fe_draw_quad(title_x, title_y, draw_w, draw_h,
                             0xFFFFFFFF, page, 0.0f, 0.0f, 1.0f, 1.0f);
            }
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        }
    }

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
            (s_current_screen >= 0 && s_current_screen < TD5_SCREEN_COUNT &&
             cmd->tex_page == s_title_tex_page[s_current_screen]) ||
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
        float fps_w = fe_measure_text_width(fps_buf, sx);
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

static void Screen_LocalizationInit(void) {
    /* [CONFIRMED @ 0x4269D0] g_attractModeControlEnabled three-state gate:
     *   0 = first entry: run full init, set ctrl=1, route to MAIN_MENU (screen 5)
     *   1 = normal re-entry: skip init, route to MAIN_MENU (screen 5)
     *   2 = resume-cup re-entry: set results_skip_display=1, route to RACE_RESULTS (screen 0x18=24) */
    if (s_attract_mode_ctrl == 2) {
        /* [CONFIRMED @ 0x42718A-0x4271A2]: DAT_00497a6c=1 then SetFrontendScreen(0x18) */
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: resume-cup path -> RACE_RESULTS (skip_display=1)");
        s_results_skip_display = 1;
        s_attract_mode_ctrl = 1;
        td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS);
        return;
    }

    if (s_attract_mode_ctrl == 1) {
        /* [CONFIRMED @ 0x427182-0x427188]: re-entry shortcut straight to MAIN_MENU */
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: re-entry shortcut -> MAIN_MENU");
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        return;
    }

    /* First entry (s_attract_mode_ctrl == 0) [CONFIRMED @ 0x4269D0 case 0]: */
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_LOCALIZATION_INIT);
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: first entry, loading resources");

        /* [CONFIRMED @ 0x4269D0 / 0x4267A8] LANGUAGE.DLL is a static PE import.
         * [CORRECTED 2026-06-01 — byte-verified from the English Language.dll export
         * SNK_LangDLL = "LANGDLL 0 : ENGLISH/US"]: byte[8] is the digit '0' = 0x30 for
         * English (the prior comment's "0x31" was WRONG). The font/text gate compares
         * byte[8] against 0x30 (CMP byte[reg+8],0x30 @0x00424568 / @0x004242b8 /
         * CreateMenuStringLabelSurface 0x00412e30); the ==0x30 (JZ-taken) branch is the
         * English/localized-blit path. (Other locales use different digits, but the
         * shipped English DLL is '0'.)
         * English entry name = "config.eng" [CONFIRMED @ 0x4667A8].
         * The original reads "config.eng" per car ZIP, sscanf's 17 tokens into
         * DAT_0049b90c (stride 0x330, 17 rows × 0x30 bytes each).
         * Port: re/assets/cars/<car>/config.nfo has the same 17-token layout (extracted
         * from the original ZIPs). frontend_load_car_spec_fields() reads all 17 tokens;
         * frontend_render_car_stats_overlay() displays them on the Stats sub-screen
         * (car-select state 15, button 2 "Stats"). */
        /* [CONFIRMED @ 0x4269D0] Car ZIP path table: handled in td5_asset.c */
        /* [CONFIRMED @ 0x426F80]: LoadPackedConfigTd5() reads config.td5 settings */
        /* [INFERRED] Enumerate display modes (handled in td5_render.c) */
        /* [CONFIRMED @ 0x427081]: Seed controller/input state from DXInput joystick exports
         *   g_player1InputSource=0, g_player2InputSource=7 if no saved controller match.
         *   Port omits: DXInput (M2DX) exports not available; td5_input.c handles this. */

        td5_frontend_init_resources();

        /* Mark init done so re-entry skips straight to menu [CONFIRMED @ 0x427060] */
        s_attract_mode_ctrl = 1;

        /* [CONFIRMED @ 0x427182]: SetFrontendScreen(5) = TD5_SCREEN_MAIN_MENU */
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;

    default:
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

/* ========================================================================
 * [1] ScreenPositionerDebugTool (0x415030) -- Dev debug, unreachable
 * States: 6
 * ======================================================================== */

static void Screen_PositionerDebugTool(void) {
    switch (s_inner_state) {
    case 0: /* Load the menu font + clear (orig loads Positioner.tga as the cursor-bar
             * colour source, clears the backbuffer black and draws two guide scanlines).
             * The original creates NO on-screen buttons — the whole tool is keyboard-
             * driven (arrows move/edit, ESC saves), so the port draws none either. */
        frontend_init_return_screen(TD5_SCREEN_POSITIONER_DEBUG);
        frontend_load_tga("Front_End/Positioner.tga", "Front_End/FrontEnd.zip");
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: /* present (orig case 1) */
        frontend_present_buffer();
        s_inner_state = 2;
        break;
    case 2: /* init glyph selection (orig g_positionerSelectedGlyphIndex = 0) */
        s_anim_tick = 0;
        s_anim_complete = 1;   /* enable the shared ESC handler (-> return screen = main menu) */
        s_inner_state = 3;
        break;
    case 3: /* navigate the glyph strip (orig case 3: arrow bits LEFT=1/RIGHT=2/UP=4/DOWN=8;
             * ←/→ = ±1, ↓/↑ = ±8). ESC is handled by the shared escape path -> main menu. */
        if (s_input_ready && s_arrow_input) {
            if (s_arrow_input & 1) s_anim_tick -= 1;   /* LEFT  */
            if (s_arrow_input & 2) s_anim_tick += 1;   /* RIGHT */
            if (s_arrow_input & 8) s_anim_tick += 8;   /* DOWN  */
            if (s_arrow_input & 4) s_anim_tick -= 8;   /* UP    */
            frontend_play_sfx(1);
        }
        break;
    default:
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

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

static void Screen_AttractModeDemo(void) {
    switch (s_inner_state) {
    case 0: /* Set attract mode flag */
        frontend_init_return_screen(TD5_SCREEN_ATTRACT_MODE);
        /* [CONFIRMED @ 0x4275B1] g_attractModeDemoActive = 1 */
        s_attract_demo_active = 1;
        frontend_present_buffer();
        frontend_set_cursor_visible(0);
        s_inner_state = 1;
        break;

    case 1: /* Release frontend buttons from main menu */
        /* [CONFIRMED @ 0x4275B7] ReleaseFrontendDisplayModeButtons() */
        frontend_reset_buttons();
        s_inner_state = 2;
        break;

    case 2: /* Present primary buffer */
        frontend_present_buffer();
        s_inner_state = 3;
        break;

    case 3: /* Present (2 frames of setup) */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Init fade-to-black */
        frontend_init_fade(0);
        s_inner_state = 5;
        break;

    case 5: /* Execute fade, then launch demo race */
        if (frontend_render_fade()) {
            /* Fade complete -- launch attract-demo race. The original demo is
             * AI-driven (no input playback) and shows the "DEMO MODE" status
             * text; set the demo flag AFTER frontend_init_race_schedule (which
             * clears it). Distinct from View Replay, which plays back input and
             * shows the REPLAY banner. [orig g_attractModeDemoActive path.] */
            frontend_init_race_schedule();
            td5_game_set_demo_mode(1);
            frontend_init_display_mode_state();
        }
        break;
    }
}

/* ========================================================================
 * [3] ScreenLanguageSelect (0x427290)
 * States: 7
 * ======================================================================== */

static void Screen_LanguageSelect(void) {
    switch (s_inner_state) {
    case 0: /* Load Language.tga and LanguageScreen.tga */
        frontend_init_return_screen(TD5_SCREEN_LANGUAGE_SELECT);
        /* [FIXED 2026-06-01] Faithful to ScreenLanguageSelect @0x00427290: the original
         * draws LanguageScreen.tga (bg) + 4 FLAG IMAGE tiles from Language.tga (176x512,
         * four stacked 176x128 tiles, src V 0/128/256/384) as clickable hit-rects at the
         * four corners + a "LANGUAGE SELECT" header (in-EXE literal @0x4667c0). It has NO
         * text buttons. Port previously showed 4 text buttons — replaced: the 4 buttons
         * are now HIDDEN hit-rects at the confirmed flag dest rects (input still works via
         * s_button_index<4), and frontend_render_language_select_overlay draws the flags +
         * header + bg. Clicking any flag advances to LEGAL (no language global written —
         * CONFIRMED). Dest rects @640x480: TL(40,128) TR(424,128) BL(40,320) BR(424,320),
         * each 176x128. */
        s_language_bg_surface   = frontend_load_tga("Front_End/LanguageScreen.tga", "Front_End/FrontEnd.zip");
        s_language_flag_surface = frontend_load_tga("Front_End/Language.tga", "Front_End/FrontEnd.zip");
        {
            int fi;
            for (fi = 0; fi < 4; fi++) {
                int fx = (fi & 1) ? 424 : 40;     /* TL/BL left=40, TR/BR right=424 */
                int fy = (fi < 2) ? 128 : 320;    /* top row 128, bottom row 320 */
                int b = frontend_create_button("", fx, fy, 176, 128);
                if (b >= 0) s_buttons[b].hidden = 1;  /* invisible hit-rect; flag drawn in overlay */
            }
        }
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Tick */
        frontend_present_buffer();
        s_inner_state = 2;
        break;

    case 2:
        s_anim_tick += 2;
        frontend_present_buffer();
        if (s_anim_tick >= 16) {
            s_inner_state = 3;
        }
        break;

    case 3: /* Interaction -- wait for language selection */
        if (s_input_ready && s_button_index >= 0 && s_button_index < 4) {
            s_flow_context = s_button_index;
            s_anim_tick = 0;
            s_inner_state = 4;
        }
        break;

    case 4:
    case 5:
        s_anim_tick += 2;
        if (s_anim_tick >= 8) {
            s_inner_state = 6;
        }
        break;

    case 6: /* Release surfaces, exit to Legal screen */
        td5_frontend_set_screen(TD5_SCREEN_LEGAL_COPYRIGHT);
        break;
    }
}

/* ========================================================================
 * [4] ScreenLegalCopyright (0x4274A0) -- 3-second copyright splash
 * States: 4
 * ======================================================================== */

static void Screen_LegalCopyright(void) {
    switch (s_inner_state) {
    case 0: /* Load + draw */
        frontend_init_return_screen(TD5_SCREEN_LEGAL_COPYRIGHT);
        frontend_load_tga("Front_End/LegalScreen.tga", "Front_End/FrontEnd.zip");
        /* Copyright text drawn live in render overlay via
         * DrawFrontendLocalizedStringSecondary @ 0x00424390.
         * Original renders "TEST DRIVE 5 COPYRIGHT 1998" [CONFIRMED @ 0x00466808]
         * at x=canvasW/10, y=0x20 (32px) and repeats each row down the screen.
         * Port renders it in frontend_render_legal_copyright_overlay below. */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Fade in [FIXED 2026-06-01: actually run the fade — was a no-op counter,
             * so the legal splash popped in. Orig case1 = RenderFrontendFadeEffect.] */
        if (s_anim_tick == 0) { frontend_init_fade(0x000000); s_anim_tick = 1; }
        if (frontend_render_fade()) {
            /* Store wall-clock start for 3-second guard [CONFIRMED @ 0x4274A0 case 1→2] */
            s_anim_tick = (int)timeGetTime();
            s_inner_state = 2;
        }
        break;

    case 2: /* 3-second timer [CONFIRMED @ 0x4274A0 case 2: timeGetTime() - stored > 2999] */
        if ((uint32_t)(timeGetTime() - (uint32_t)s_anim_tick) > 2999u) {
            s_anim_tick = 0;
            s_inner_state = 3;
        }
        break;

    case 3: /* Fade out + exit [FIXED 2026-06-01: run the fade, was a no-op counter.] */
        if (s_anim_tick == 0) { frontend_init_fade(0x000000); s_anim_tick = 1; }
        if (frontend_render_fade()) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* ========================================================================
 * [30] Multiplayer Lobby  (PORT ENHANCEMENT 2026-06)
 *
 * Press-to-join: each input that presses A (joystick) / Enter (keyboard) joins
 * in order (join order = player number) and shows as READY. START (the button,
 * SPACE, or a joined player's confirm) proceeds to the per-player car select.
 * ======================================================================== */
static void Screen_MultiplayerLobby(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_MP_LOBBY);
        TD5_LOG_I(LOG_TAG, "MP Lobby: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        td5_input_enumerate_devices();              /* fresh device list for the join scan */
        s_mp_joined_count = 0;
        memset(s_mp_join_device, 0, sizeof(s_mp_join_device));
        s_mp_join_prev = td5_plat_input_scan_join();/* ignore inputs already held on entry */
        frontend_reset_buttons();
        frontend_create_button(SNK_StartRaceTxt, 220, 300, 200, 32);  /* 0 START */
        frontend_create_button(SNK_BackButTxt,   260, 360, 120, 32);  /* 1 BACK */
        s_selected_button = 0;
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 6;
        }
        break;
    case 6: {
        uint32_t scan  = td5_plat_input_scan_join();
        uint32_t newly = scan & ~s_mp_join_prev;
        int kbd_joined = 0, joined_now = 0, j, d, do_start = 0, do_back = 0;
        s_mp_join_prev = scan;

        for (j = 0; j < s_mp_joined_count; j++)
            if (s_mp_join_device[j] == 0) kbd_joined = 1;

        /* Joystick joins (devices 1..) in join order. */
        for (d = 1; d < 16; d++) {
            int already = 0;
            if (!(newly & (1u << d))) continue;
            for (j = 0; j < s_mp_joined_count; j++)
                if (s_mp_join_device[j] == d) already = 1;
            if (!already && s_mp_joined_count < TD5_MAX_HUMAN_PLAYERS) {
                s_mp_join_device[s_mp_joined_count++] = d;
                joined_now = 1;
                frontend_play_sfx(3);
                TD5_LOG_I(LOG_TAG, "MP Lobby: player %d join device %d", s_mp_joined_count, d);
            }
        }
        /* Keyboard join via Enter (device 0). The FIRST Enter joins; once joined,
         * Enter falls through to confirm the START button. */
        if ((newly & 1u) && !kbd_joined && s_mp_joined_count < TD5_MAX_HUMAN_PLAYERS) {
            s_mp_join_device[s_mp_joined_count++] = 0;
            joined_now = 1;
            frontend_play_sfx(3);
            TD5_LOG_I(LOG_TAG, "MP Lobby: player %d join keyboard", s_mp_joined_count);
        }

        if (!joined_now) {
            if (s_input_ready && s_button_index == 0) do_start = 1;  /* START button */
            if (s_input_ready && s_button_index == 1) do_back  = 1;  /* BACK button  */
            if (td5_plat_input_key_pressed(0x39))      do_start = 1;  /* SPACE        */
        }
        if (frontend_check_escape()) do_back = 1;                     /* ESC / gamepad B */

        if (do_start && s_mp_joined_count >= 1) {
            int p;
            s_num_human_players = s_mp_joined_count;
            s_two_player_mode   = 1;       /* engage split-screen multiplayer */
            s_mp_flow           = 1;
            for (p = 0; p < s_mp_joined_count; p++) {
                td5_input_set_input_source(p, s_mp_join_device[p]);
                td5_save_set_player_device_index(p, (uint32_t)s_mp_join_device[p]);
                s_mp_player_car[p]   = s_selected_car;
                s_mp_player_paint[p] = 0;
            }
            s_mp_car_player = 0;
            td5_plat_input_scan_join_release();
            TD5_LOG_I(LOG_TAG, "MP Lobby: START with %d players -> car select", s_mp_joined_count);
            td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
            return;
        }
        if (do_back) {
            s_mp_flow = 0;
            td5_plat_input_scan_join_release();
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }
        break;
    }
    default:
        td5_plat_input_scan_join_release();
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

/* ========================================================================
 * [5] ScreenMainMenuAnd1PRaceFlow (0x415490)
 * States: 24 (0x00 - 0x17)
 *
 * 7 buttons: Race Menu, Quick Race, Two Player, Net Play, Options,
 *            Hi-Score, Exit
 * ======================================================================== */

static void Screen_MainMenu(void) {
    switch (s_inner_state) {
    case 0: /* Init: configure controller bindings, apply options, create 7 buttons, load MainMenu.tga */
        frontend_init_return_screen(TD5_SCREEN_MAIN_MENU);
        TD5_LOG_D(LOG_TAG, "MainMenu: state 0 - init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;

        /* [CONFIRMED @ 0x004155DE ScreenMainMenuAnd1PRaceFlow case 0] Original
         * copies the GameOptions shadow (DAT_00466000, range 0..3) into the
         * live runtime lap count: gCircuitLapCount = DAT_00466000 + 1.
         * Without this seed, a fresh boot leaves circuit_lap_count=0 and the
         * HUD's "%d/%d" lap label renders as "1/0". The original re-applies
         * this on every main-menu entry; ConfigureGameTypeFlags later may
         * overwrite it for cup tiers (case 2 hard-sets to 4), but the
         * baseline must be primed here so single races and quickrace work. */
        g_td5.circuit_lap_count = s_game_option_laps + 1;
        TD5_LOG_I(LOG_TAG, "MainMenu: seeded circuit_lap_count=%d (laps_option=%d)",
                  g_td5.circuit_lap_count, s_game_option_laps);

        /* Apply saved options from config shadow into live globals */
        /* Configure controller bindings for both player slots */

        /* Create 7 main menu buttons:
         * 0: SNK_RaceMenuButTxt
         * 1: SNK_QuickRaceButTxt
         * 2: SNK_TwoPlayerButTxt (or "Time Demo" in dev build)
         * 3: SNK_NetPlayButTxt
         * 4: SNK_OptionsButTxt
         * 5: SNK_HiScoreButTxt
         * 6: SNK_ExitButTxt
         */
        frontend_create_button(SNK_RaceMenuButTxt,   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_QuickRaceButTxt,  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_TwoPlayerButTxt,  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_NetPlayButTxt,    -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_OptionsButTxt,     -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_HiScoreButTxt, -0xE0, 0, 0xE0, 0x20);
        frontend_create_button(SNK_ExitButTxt,        -0xE0, 0, 0xE0, 0x20);

        /* Phase 6: per-button surface cache is populated lazily by the
         * render path on first draw of each button -- no per-screen bake
         * call needed. Caches survive screen transitions; label changes
         * trigger automatic re-bake via td5_fe_btncache_ensure_page. */

        frontend_set_cursor_visible(0);
        frontend_play_sfx(5); /* menu ready */
        s_inner_state = 1;
        break;

    case 1: /* Present buffer */
        frontend_present_buffer();
        s_inner_state = 2;
        break;

    case 2: /* Reset tick counter, rebuild button surfaces */
        frontend_begin_timed_animation();
        frontend_present_buffer();
        s_inner_state = 3;
        break;

    case 3: /* Slide-in animation: 7 buttons alternating L/R, title descends. 39 frames. */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            frontend_set_cursor_visible(1);
            frontend_play_sfx(4); /* ready chime */
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Main interaction loop: wait for button press */
        /* Reset attract idle on any input */
        if (s_input_ready) {
            s_attract_idle_timestamp = td5_plat_time_ms();
        }

        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* Race Menu */
                s_flow_context = 1;
                s_return_screen = TD5_SCREEN_RACE_TYPE_MENU;
                s_inner_state = 8; /* slide-out prep */
                break;

            case 1: /* Quick Race */
                /* Main Menu "Quick Race" is a single-race flow: stamp
                 * game_type=0 and run ConfigureGameTypeFlags so it copies
                 * s_game_option_traffic / s_game_option_cops into
                 * g_td5.traffic_enabled / g_td5.special_encounter_enabled.
                 * Without this, traffic stays at whatever the zero-init
                 * default is and AI traffic + cops never spawn on the track.
                 * Mirrors ConfigureGameTypeFlags @ 0x00410CA0 case 0. */
                s_selected_game_type = 0;
                ConfigureGameTypeFlags();
                s_flow_context = 2;
                s_return_screen = TD5_SCREEN_QUICK_RACE;
                s_inner_state = 8;
                TD5_LOG_I(LOG_TAG,
                          "MainMenu Quick Race: game_type=0 traffic=%d cops=%d",
                          g_td5.traffic_enabled, g_td5.special_encounter_enabled);
                break;

            case 2: /* Two Player / Time Demo
                     *
                     * Original ScreenMainMenuAnd1PRaceFlow @ 0x415BFC:
                     *   if (*(int*)(g_appExref+0x170) != 0) {
                     *       g_benchmarkModeActive = 1;
                     *       InitializeRaceSeriesSchedule();
                     *       return;
                     *   } else {
                     *       g_twoPlayerModeEnabled = 1;
                     *       g_selectedGameType = 0;
                     *   }
                     *
                     * `app+0x170` is never written anywhere in TD5_d3d.exe
                     * (zero write xrefs), so button 2 is always 2-Player in
                     * the shipped binary. The port exposes the benchmark
                     * path via the td5re.ini [Debug] EnableBenchmark=1
                     * option so that the existing TD5_GAMESTATE_BENCHMARK
                     * code path is reachable for testing.
                     * [RE basis: research agent xref scan of app+0x170] */
                if (g_td5.ini.enable_benchmark) {
                    TD5_LOG_I(LOG_TAG, "MainMenu: button 2 → benchmark mode (INI override)");
                    g_td5.benchmark_active = 1;
                    s_flow_context = 3;
                    s_return_screen = TD5_SCREEN_CAR_SELECTION;
                    s_inner_state = 8;
                } else {
                    /* [PORT ENHANCEMENT 2026-06] MULTIPLAYER → press-to-join lobby
                     * (which assigns players/devices then runs per-player car select).
                     * s_two_player_mode is engaged by the lobby's START, not here. */
                    s_flow_context = 3;
                    s_selected_game_type = 0;
                    s_return_screen = TD5_SCREEN_MP_LOBBY;
                    s_inner_state = 8;
                }
                break;

            case 3: /* Net Play */
                s_flow_context = 4;
                s_return_screen = TD5_SCREEN_CONNECTION_BROWSER;
                s_inner_state = 8;
                break;

            case 4: /* Options */
                s_flow_context = 5;
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 8;
                break;

            case 5: /* Hi-Score */
                s_flow_context = 6;
                s_return_screen = TD5_SCREEN_HIGH_SCORE;
                s_inner_state = 8;
                break;

            case 6: /* Exit -> Yes/No confirm dialog */
                TD5_LOG_I(LOG_TAG, "MainMenu: Exit pressed, showing confirm dialog");
                s_inner_state = 5;
                break;
            }
        }
        break;

    case 5: { /* Exit confirm dialog: create YES / NO! buttons */
        int exit_x = s_buttons[6].x;
        int exit_y = s_buttons[6].y;
        int exit_w = s_buttons[6].w;
        int exit_h = s_buttons[6].h;
        /* Split the EXIT button's footprint into two equal buttons with a clear
         * gap so YES and NO! read as two distinct, easy-to-hit targets (the old
         * layout left only a 4px gap between two 96px buttons). The pair is
         * centered under EXIT by construction. */
        const int yn_gap = 24;
        int yn_w = (exit_w - yn_gap) / 2;
        if (yn_w < 80) yn_w = 80;                 /* floor for unusually narrow EXIT */
        int yn_y = exit_y + exit_h + 10;
        int yes_idx = frontend_create_button(SNK_YesButTxt, exit_x,                  yn_y, yn_w, 32);
        int no_idx  = frontend_create_button(SNK_NoxButTxt,  exit_x + yn_w + yn_gap, yn_y, yn_w, 32);
        s_exit_confirm_yes_idx = yes_idx;
        s_exit_confirm_no_idx  = no_idx;
        if (yes_idx >= 0) s_selected_button = yes_idx;
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirm dialog created yes=%d no=%d", yes_idx, no_idx);
        s_inner_state = 6;
        break;
    }

    case 6: /* Exit confirm: wait for YES / NO! */
        if (s_input_ready && s_button_index >= 0) {
            /* Dispatch by the indices recorded in state 5, NOT by label text.
             * The SNK labels are "YES" / "NO!" (td5_snk_strings.h), so the old
             * strcmp(label,"Yes")/strcmp(label,"No") never matched and EXIT did
             * nothing. Index compare also tolerates the button-pool slot the
             * dialog happens to land in. (yes/no idx are -1 if creation failed,
             * and s_button_index is >= 0 here, so a failed button can't match.) */
            if (s_button_index == s_exit_confirm_yes_idx) {
                TD5_LOG_I(LOG_TAG, "MainMenu: exit YES selected, quitting");
                s_inner_state = 7;
            } else if (s_button_index == s_exit_confirm_no_idx) {
                /* Drop the YES / NO! buttons (release by index so the render loop,
                 * which gates on .active, actually stops drawing them) and return
                 * to the menu. */
                frontend_release_button(s_exit_confirm_yes_idx);
                frontend_release_button(s_exit_confirm_no_idx);
                s_exit_confirm_yes_idx = -1;
                s_exit_confirm_no_idx  = -1;
                if (s_button_count > 7) s_button_count = 7;
                s_selected_button = 6; /* re-focus on Exit */
                TD5_LOG_I(LOG_TAG, "MainMenu: exit NO selected, returning to menu");
                s_inner_state = 4;
            }
        }
        break;

    case 7: /* Confirm exit -- navigate to credits then quit */
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirmed, going to credits");
        td5_frontend_set_screen(TD5_SCREEN_EXTRAS_GALLERY);
        break;

    case 8: /* Slide-out prep: keep the software cursor visible for the next frontend screen */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out animation: buttons scatter, ~500ms.
             *
             * [ARCH-DIVERGENCE] Orig 0x004155DE checks the per-player input
             * source (joystick index 7 = none) before navigating, and routes
             * to states 0x14-0x17 (controller-required dialog → ControlOptions)
             * when a joystick is configured but missing. The port is
             * keyboard-first — `td5_plat_input_get_keyboard()` is always
             * available — so a missing joystick can never block navigation.
             * The validation gate and its dialog states are intentionally
             * dropped; replace with a direct screen transition. */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    case 10: /* Post-Yes exit: release buttons */
    case 11:
        frontend_post_quit();
        break;

    case 12: /* Scatter buttons for exit transition (~500ms) */
        if (s_anim_start_ms == 0) frontend_begin_timed_animation();
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            frontend_post_quit();
        }
        break;
    }
}

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

static void Screen_RaceTypeCategory(void) {
    switch (s_inner_state) {
    case 0: /* Init: load MainMenu.tga, create 7 race-type buttons */
        frontend_init_return_screen(TD5_SCREEN_RACE_TYPE_MENU);
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: state 0 - init");
        /* [DA-T4 fix 2026-05-22] Clear wanted-mode flag — orig 0x004168D7 sets
         * g_wantedModeEnabled = 0 here. If user backs out of cop-chase race
         * without this, the wanted-HUD overlay can persist into the race-type
         * menu on the next entry. */
        g_td5.wanted_mode_enabled = 0;
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip"); /* original 0x4168B0: loads MainMenu.tga, not RaceMenu.tga */
        s_anim_complete = 0;

        /* [ARCH-DIVERGENCE] Orig allocated a 0x110x0xB4 DDraw surface here for
         * the description preview (`g_lobbyErrorDialogSurface` @ 0x004168D7) and
         * blitted text into it from state 4 on hover. Port renders the preview
         * directly per-frame via `frontend_render_race_type_description`
         * (td5_frontend.c:4021, dispatched at :5362), so no intermediate
         * surface is needed and the old state-4 update step is unreachable. */
        /* Create 7 buttons for race types.
         * [FIXED 2026-06-01, runtime @0x499c78] rows 0-5 at x=120, y=97 step40 (224-wide);
         * BACK bottom-center (176,377), half-width 0x70 (112). Port auto-layout gave 110/93 +
         * BACK stacked in-column at full width. Slide-in still animates X to rest. */
        frontend_create_button(SNK_SingleRaceButTxt, 120,  97, 0xE0, 0x20);
        frontend_create_button(SNK_CupRaceButTxt,    120, 137, 0xE0, 0x20);
        /* Continue Cup: greyed if no valid CupData.td5 */
        if (frontend_validate_cup_checksum())
            frontend_create_button(SNK_ContCupButTxt, 120, 177, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_ContCupButTxt, 120, 177, 0xE0, 0x20);
        frontend_create_button(SNK_TimeTrialsButTxt, 120, 217, 0xE0, 0x20);
        frontend_create_button(SNK_DragRaceButTxt,   120, 257, 0xE0, 0x20);
        frontend_create_button(SNK_CopChaseButTxt,   120, 297, 0xE0, 0x20);
        frontend_create_button(SNK_BackButTxt,       176, 377, 0x70, 0x20);

        s_selected_game_type = -1;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        if (frontend_update_timed_animation(0x20, 533) >= 1.0f) {
            s_inner_state = 2;
        }
        break;

    case 2: /* Tick until AdvanceFrontendTickAndCheckReady */
        if (frontend_advance_tick()) {
            s_anim_complete = 1;
            frontend_play_sfx(4); /* slide-in settle chime — the original
                                   * RaceTypeCategory plays Play(4) when the
                                   * slide-in completes [CONFIRMED @ 0x4168B0];
                                   * the port was missing it, so the race menu
                                   * appeared without the settle chime that every
                                   * other screen has. */
            s_inner_state = 3;
        }
        break;

    case 3: /* Main interaction loop */
        /* Buttons render via the standard frontend pass; description preview
         * is driven by s_selected_button (hover index) per-frame — no explicit
         * "preview update" state needed (see ARCH-DIVERGENCE note in state 0). */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* Single Race */
                s_selected_game_type = 0;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 1: /* Cup Race -> enter sub-menu */
                s_inner_state = 6;
                break;

            case 2: /* Continue Cup */
                if (frontend_validate_cup_checksum()) {
                    frontend_load_continue_cup_data();
                    s_return_screen = TD5_SCREEN_RACE_RESULTS;
                    s_inner_state = 5;
                } else {
                    frontend_play_sfx(10); /* rejection */
                }
                break;

            case 3: /* Time Trials */
                s_selected_game_type = 7;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 4: /* Drag Race */
                s_selected_game_type = 9;
                s_drag_carselect_pass = 0;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 5: /* Cop Chase */
                s_selected_game_type = 8;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 5;
                break;

            case 6: /* Back */
                s_return_screen = (s_network_active) ? TD5_SCREEN_CREATE_SESSION : TD5_SCREEN_MAIN_MENU;
                s_inner_state = 5;
                break;
            }
        }
        break;

    case 5: /* Slide-out prep: buttons scatter */
        frontend_play_sfx(5); /* slide-out whoosh — was missing here, so leaving
                               * the race menu to another screen was silent. Every
                               * other slide-out prep plays it (cf. Screen_MainMenu
                               * case 8). Placed at prep (not the 0x14 animation),
                               * matching the one-whoosh-per-transition pattern. */
        frontend_begin_timed_animation();
        s_inner_state = 0x14;
        break;

    /* --- Cup sub-menu (states 6-12) --- */

    case 6: /* Cup sub-menu: release top buttons, create cup tier buttons */
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: entering cup sub-menu");
        frontend_reset_buttons();
        /* Create 7 cup tier buttons */
        frontend_create_button(SNK_ChampionshipButTxt, -0xE0, 0, 0xE0, 0x20); /* always available */
        frontend_create_button(SNK_EraButTxt,          -0xE0, 0, 0xE0, 0x20); /* always available */

        /* Challenge: locked if s_cup_unlock_tier == 0 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button(SNK_ChallengeButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_ChallengeButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Pitbull: locked if s_cup_unlock_tier < 1 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button(SNK_PitbullButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_PitbullButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Masters: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button(SNK_MastersButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_MastersButTxt, -0xE0, 0, 0xE0, 0x20);

        /* Ultimate: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button(SNK_UltimateButTxt, -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button(SNK_UltimateButTxt, -0xE0, 0, 0xE0, 0x20);

        frontend_create_button(SNK_BackButTxt, -0xE0, 0, 0xE0, 0x20);

        frontend_begin_timed_animation();
        s_inner_state = 7;
        break;

    case 7: /* Cup sub-menu slide-in: ~1000ms */
        if (frontend_update_timed_animation(0x20, 533) >= 1.0f) {
            s_inner_state = 8;
        }
        break;

    case 8: /* Cup sub-menu tick */
        if (frontend_advance_tick()) {
            s_inner_state = 9;
        }
        break;

    case 9: /* Cup sub-menu interaction */
        if (s_input_ready && s_button_index >= 0) {
            int cup_type = -1;
            switch (s_button_index) {
            case 0: cup_type = 1; break; /* Championship */
            case 1: cup_type = 2; break; /* Era */
            case 2: /* Challenge */
                if (s_cup_unlock_tier >= 1) cup_type = 3;
                else frontend_play_sfx(10);
                break;
            case 3: /* Pitbull */
                if (s_cup_unlock_tier >= 1) cup_type = 4;
                else frontend_play_sfx(10);
                break;
            case 4: /* Masters */
                if (s_cup_unlock_tier >= 2) cup_type = 5;
                else frontend_play_sfx(10);
                break;
            case 5: /* Ultimate */
                if (s_cup_unlock_tier >= 2) cup_type = 6;
                else frontend_play_sfx(10);
                break;
            case 6: /* Back to top-level */
                s_inner_state = 11;
                break;
            }
            if (cup_type >= 0) {
                s_selected_game_type = cup_type;
                s_race_within_series = 0;
                /* [DA-T4 D.3 fix 2026-05-22] orig 0x004171F0 region:
                 *   g_selectedScheduleIndex = g_attractModeTrackIndex;
                 * Seeds the race's schedule index from the attract-mode
                 * preview track. Port previously skipped this — cup races
                 * could start on stale/wrong track. */
                s_selected_track = s_attract_track;
                ConfigureGameTypeFlags();
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 10;
            }
        }
        break;

    case 10: /* Cup sub-menu slide-out -> Car Selection */
        s_anim_tick = 0;
        s_inner_state = 0x14;
        break;

    case 11: /* Back to top-level: rebuild top buttons */
        frontend_reset_buttons();
        s_inner_state = 0; /* re-init top menu */
        break;

    /* --- Return transition --- */
    case 0x14: /* Slide-out animation (~500ms), then navigate */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    default:
        break;
    }
}

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

/* Cycle s_selected_track by delta, skipping the Drag Strip (schedule index 19)
 * and any track whose level data is absent. One full revolution then give up. */
static void frontend_quickrace_cycle_track(int delta) {
    int track_max = s_network_active ? 0x13 : s_total_unlocked_tracks; /* exclusive bound */
    if (track_max <= 0) return;
    int start = s_selected_track;
    int attempts = track_max + 1;
    while (attempts-- > 0) {
        s_selected_track += delta;
        if (s_selected_track < 0) s_selected_track = track_max - 1;
        if (s_selected_track >= track_max) s_selected_track = 0;
        if (s_selected_track == FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX) continue;
        if (frontend_track_level_exists(s_selected_track)) return;
    }
    s_selected_track = start; /* nothing else available — restore (never lands on drag strip) */
}

/* Clamp the human/AI counts so 1 <= humans <= 6 and 0 <= opponents <= 6-humans.
 * The counts render as value text to the right of the Players/Opponents buttons
 * (frontend_render_quick_race_overlay), so no button labels are touched here. */
static void frontend_quickrace_clamp_counts(void) {
    if (s_num_human_players < 1) s_num_human_players = 1;
    /* [PORT ENHANCEMENT] up to TD5_MAX_HUMAN_PLAYERS (9) local split-screen humans. */
    if (s_num_human_players > TD5_MAX_HUMAN_PLAYERS) s_num_human_players = TD5_MAX_HUMAN_PLAYERS;
    int opp_max = TD5_MAX_RACER_SLOTS - s_num_human_players;
    if (s_num_ai_opponents < 0) s_num_ai_opponents = 0;
    if (s_num_ai_opponents > opp_max) s_num_ai_opponents = opp_max;
}

static void Screen_QuickRaceMenu(void) {
    switch (s_inner_state) {
    case 0: /* Init: validate indices, create the 7-row improved layout */
        frontend_init_return_screen(TD5_SCREEN_QUICK_RACE);
        TD5_LOG_D(LOG_TAG, "QuickRaceMenu: init");
        s_anim_complete = 0;
        /* Load background: same as main menu */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Validate car/track indices; never start on the Drag Strip (excluded). */
        if (s_selected_car < 0) s_selected_car = 0;
        if (s_selected_track < 0) s_selected_track = 0;
        if (s_selected_track >= 26) s_selected_track = 0;
        if (s_selected_track == FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX) s_selected_track = 0;

        /* Improved layout (PORT ENHANCEMENT): caption selectors with the selected
         * value drawn to the RIGHT of each button (value column), plus OK/Back on
         * the bottom row. Rows are uniformly spaced (QR_ROW_Y0 + n*QR_ROW_DY). The
         * Direction row hides on forward-only/circuit tracks. Values are drawn by
         * frontend_render_quick_race_overlay at FE_QR_VALUE_X.
         *
         * [S02 (b) 2026-06-04] These caption rows are NOT marked is_selector: the
         * VectorUI procedural-button path (the default) draws a caption only for
         * non-selector buttons, so flagging them as selectors left the captions
         * blank. They render their captions through the shared fe_draw_text path
         * (same as the Track Selection option rows); the ◄► arrows are still drawn
         * by the explicit fe_draw_option_arrows loop in td5_frontend_render_ui_rects. */
        { int bi;
          frontend_create_button("Car",                 QR_COL_X, QR_ROW_Y(0), QR_BTN_W, 32); /* QR_BTN_CAR */
          frontend_create_button("Track",               QR_COL_X, QR_ROW_Y(1), QR_BTN_W, 32); /* QR_BTN_TRACK */
          frontend_create_button("Direction",           QR_COL_X, QR_ROW_Y(2), QR_BTN_W, 32); /* QR_BTN_DIRECTION */
          /* [PORT ENHANCEMENT 2026-06] Quick Race is single-player (driven by the
           * active controller); the Players row is hidden — local multiplayer is
           * the main-menu MULTIPLAYER lobby. The row is still CREATED (to keep the
           * QR_BTN_* indices stable) but hidden, and Opponents/Laps/OK/Back close
           * the gap. */
          bi = frontend_create_button("Players",             QR_COL_X, QR_ROW_Y(3), QR_BTN_W, 32); /* QR_BTN_PLAYERS */
          if (bi >= 0) { s_buttons[bi].hidden = 1; s_buttons[bi].disabled = 1; }
          frontend_create_button("Opponents",           QR_COL_X, QR_ROW_Y(3), QR_BTN_W, 32); /* QR_BTN_OPPONENTS */
          /* [S02 (c) 2026-06-04] Circuit laps, re-homed here from Game Options.
           * Mirrors the Track Selection laps row; edits s_game_option_laps. */
          frontend_create_button("Laps",                QR_COL_X, QR_ROW_Y(4), QR_BTN_W, 32); /* QR_BTN_LAPS */
        }
        frontend_create_button(SNK_OkButTxt,           QR_COL_X,       QR_ROW_Y(5),  96, 32); /* QR_BTN_OK */
        frontend_create_button(SNK_BackButTxt,         QR_COL_X + 108, QR_ROW_Y(5), 112, 32); /* QR_BTN_BACK */

        /* Reset direction to Forwards on entry (matches TrackSelection); hide the
         * toggle on forward-only/circuit tracks (caption stays "Direction" —
         * manage_label=0). Clamp the player/opponent counts. */
        s_track_direction = 0;
        frontend_update_direction_button_visibility(QR_BTN_DIRECTION, 0);
        /* Hide the Laps row on point-to-point tracks (no laps); show on circuits. */
        frontend_update_laps_button_visibility(QR_BTN_LAPS);
        frontend_quickrace_clamp_counts();

        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Present + reset counter */
    case 2:
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Interactive: cycle car/track/direction/players/opponents, OK/Back */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int selected_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (selected_button == QR_BTN_CAR && delta != 0) {
                /* Cycle car. TD6 cars (37-75) are included; locked TD5 cars and
                 * police are skipped; the result is always a valid index, so the
                 * old off-by-one "UNKNOWN CAR" entry (index == unlocked count) is
                 * gone. */
                s_selected_car = frontend_car_cycle_step(s_selected_car, delta,
                                                         0, TD5_CAR_COUNT - 1);
                frontend_play_sfx(2); /* ping2.wav cycle */
            }

            if (selected_button == QR_BTN_TRACK && delta != 0) {
                /* Cycle track, skipping Drag Strip (index 19) + absent levels. */
                frontend_quickrace_cycle_track(delta);
                /* Re-evaluate the Direction toggle + Laps row for the new track. */
                frontend_update_direction_button_visibility(QR_BTN_DIRECTION, 0);
                frontend_update_laps_button_visibility(QR_BTN_LAPS);
                TD5_LOG_I(LOG_TAG, "QuickRace track cycle: s_selected_track=%d level=%d name=%s",
                          s_selected_track, td5_asset_level_number(s_selected_track),
                          frontend_get_track_name(s_selected_track));
                frontend_play_sfx(2); /* ping2.wav cycle */
            }

            /* Direction toggle: 0=Forwards, 1=Backwards. Inert (and hidden) on
             * forward-only/circuit tracks. Responds to either arrow or button
             * press (mirrors TrackSelection 0x00427630 button-1 behavior). The
             * value renders to the right of the button (caption stays "Direction"). */
            if (selected_button == QR_BTN_DIRECTION && !s_buttons[QR_BTN_DIRECTION].hidden &&
                (delta != 0 || s_button_index == QR_BTN_DIRECTION)) {
                s_track_direction = !s_track_direction;
                frontend_play_sfx(2);
            }

            /* Players (human) count: 1..6. Increasing past the 6-racer cap auto-
             * reduces Opponents (handled in frontend_quickrace_clamp_counts). */
            if (selected_button == QR_BTN_PLAYERS && delta != 0) {
                s_num_human_players += delta;
                frontend_quickrace_clamp_counts();
                frontend_play_sfx(2);
            }

            /* Opponents (AI) count: 0..(6 - humans). */
            if (selected_button == QR_BTN_OPPONENTS && delta != 0) {
                s_num_ai_opponents += delta;
                frontend_quickrace_clamp_counts();
                frontend_play_sfx(2);
            }

            /* [S02 (c) 2026-06-04] Circuit laps (re-homed from Game Options).
             * Stored 0..9, displayed value+1 (so 1..10 laps); race setup reads
             * g_td5.circuit_lap_count = s_game_option_laps + 1. Matches the Track
             * Selection laps row's range. */
            if (selected_button == QR_BTN_LAPS && !s_buttons[QR_BTN_LAPS].hidden &&
                delta != 0) {
                s_game_option_laps += delta;
                if (s_game_option_laps < 0) s_game_option_laps = 0;
                if (s_game_option_laps > 9) s_game_option_laps = 9;
                frontend_play_sfx(2);
            }

            if (s_button_index == QR_BTN_OK) {
                /* Block if car or track is locked */
                int car_locked = (!s_cheat_unlock_all && !s_network_active &&
                                  s_selected_car >= 0 && s_selected_car < 37 &&
                                  s_car_lock_table[s_selected_car] != 0);
                int track_locked = (!s_cheat_unlock_all && !s_network_active &&
                                    s_selected_track >= 0 && s_selected_track < 37 &&
                                    s_track_lock_table[s_selected_track] != 0);
                if (car_locked || track_locked) {
                    frontend_play_sfx(10); /* rejection */
                } else {
                    /* Commit the selected direction; counts are read by
                     * frontend_init_race_schedule (gated to Quick Race). */
                    g_td5.reverse_direction = s_track_direction;
                    /* [S02 (c) 2026-06-04] Persist the lap choice (re-homed from
                     * Game Options' OK, which no longer owns this setting). */
                    g_td5.ini.laps = s_game_option_laps;
                    td5_ini_persist_options();
                    TD5_LOG_I(LOG_TAG,
                              "QuickRace OK: track=%d dir=%s humans=%d opponents=%d laps=%d",
                              s_selected_track, s_track_direction ? "Backwards" : "Forwards",
                              s_num_human_players, s_num_ai_opponents, s_game_option_laps + 1);
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 5;
                }
            }
            if (s_button_index == QR_BTN_BACK) {
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 5;
            }
        }
        break;

    case 5: /* Prep slide-out */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 6;
        break;

    case 6: /* Slide-out: ~500ms, then dispatch */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            if (s_return_screen == -1) {
                /* Start race */
                frontend_init_race_schedule();
                frontend_init_display_mode_state();
            } else {
                td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
            }
        }
        break;
    }
}

/* ========================================================================
 * [8] RunFrontendConnectionBrowser (0x418D50) -- Network connection type
 * States: ~10
 * ======================================================================== */

/* S10: the local player name presented to the lobby/roster (persisted nickname). */
static const char *frontend_net_player_name(void) {
    return (g_td5.ini.net_nickname[0]) ? g_td5.ini.net_nickname : "Player";
}

/* S10: split "ip" or "ip:port" into an IP string + port (default game port). */
static void frontend_net_parse_ip_port(const char *in, char *ip_out, int ip_len, int *port_out) {
    const char *colon = strchr(in, ':');
    *port_out = s_net_cfg_game_port;
    if (colon) {
        int n = (int)(colon - in);
        if (n >= ip_len) n = ip_len - 1;
        memcpy(ip_out, in, (size_t)n);
        ip_out[n] = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) *port_out = p;
    } else {
        snprintf(ip_out, (size_t)ip_len, "%s", in);
    }
}

/* ========================================================================
 * [8] Screen_ConnectionBrowser -- S10 ONLINE connection MODE SELECT
 *
 * Two explicit modes: LAN GAME (-> Screen_LanMenu: host / discover) and
 * DIRECT IP (-> Screen_DirectConnect: host / join by IP). On the first net-play
 * visit with no saved nickname, routes to the nickname-entry screen first.
 * ======================================================================== */
static void Screen_ConnectionBrowser(void) {
    switch (s_inner_state) {
    case 0: /* Init: net up + mode-select buttons (LAN / DIRECT / BACK) */
        frontend_init_return_screen(TD5_SCREEN_CONNECTION_BROWSER);
        TD5_LOG_D(LOG_TAG, "ConnectionBrowser: mode select");
        /* Seed [Network] config from the ini (game port + UPnP toggle). */
        s_net_cfg_game_port   = (g_td5.ini.net_game_port > 0 && g_td5.ini.net_game_port <= 65535)
                                ? g_td5.ini.net_game_port : 37050;
        s_net_cfg_enable_upnp = g_td5.ini.net_enable_upnp;

        /* First net-play visit with no saved nickname -> prompt for one. */
        if (!g_td5.ini.net_nickname[0]) {
            td5_frontend_set_screen(TD5_SCREEN_NET_NICKNAME);
            return;
        }

        frontend_net_enumerate();                 /* idempotent td5_net_init */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("LAN GAME",  120, 193, 496, 0x30);
        frontend_create_button("DIRECT IP", 120, 257, 496, 0x30);
        frontend_create_button(SNK_BackButTxt, 232, 377, 112, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1:
        s_inner_state = 2;
        break;

    case 2: /* Slide-in */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 3;
        }
        break;

    case 3:
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4:
        frontend_present_buffer();
        s_inner_state = 5;
        break;

    case 5: /* Mode-select interaction */
        if (s_input_ready) {
            if (s_button_index == 0) {            /* LAN GAME */
                td5_net_set_mode(TD5_NET_MODE_LAN);
                s_return_screen = TD5_SCREEN_LAN_MENU;
                s_inner_state = 8;
            } else if (s_button_index == 1) {     /* DIRECT IP */
                td5_net_set_mode(TD5_NET_MODE_DIRECT);
                s_return_screen = TD5_SCREEN_DIRECT_CONNECT;
                s_inner_state = 8;
            } else if (s_button_index == 2) {     /* BACK */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 8;
            }
        }
        break;

    case 6:
    case 7:
        s_inner_state = 5;
        break;

    case 8: /* Slide-out prep */
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out -> next screen */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;
    }
}

/* ========================================================================
 * [33] Screen_NetNickname -- enter the player nickname (first net-play visit;
 * also reachable from Multiplayer Options). Persisted to td5re.ini [Network].
 * ======================================================================== */
static void Screen_NetNickname(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_NET_NICKNAME);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the field (gold frame at 120,193); only the
         * OK button is a real button (index 0). */
        frontend_create_button(SNK_OkButTxt, 232, 377, 160, 0x20);
        if (!g_td5.ini.net_nickname[0])
            snprintf(g_td5.ini.net_nickname, sizeof(g_td5.ini.net_nickname), "Player");
        frontend_begin_text_input(g_td5.ini.net_nickname, (int)sizeof(g_td5.ini.net_nickname));
        frontend_set_text_input_prompt("ENTER YOUR NICKNAME");
        s_inner_state = 1;
        break;

    case 1:
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_text_input_confirmed() || (s_input_ready && s_button_index == 0)) {
            int from_mpopts = s_nickname_from_mpopts;
            s_nickname_from_mpopts = 0;
            if (!g_td5.ini.net_nickname[0])
                snprintf(g_td5.ini.net_nickname, sizeof(g_td5.ini.net_nickname), "Player");
            td5_ini_write_str("Network", "Nickname", g_td5.ini.net_nickname);
            TD5_LOG_I(LOG_TAG, "Nickname set: \"%s\"", g_td5.ini.net_nickname);
            td5_frontend_set_screen(from_mpopts ? TD5_SCREEN_TWO_PLAYER_OPTIONS
                                                : TD5_SCREEN_CONNECTION_BROWSER);
        }
        break;
    }
}

/* ========================================================================
 * [31] Screen_LanMenu -- LAN GAME: host a new game or discover existing ones.
 * ======================================================================== */
static void Screen_LanMenu(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_LAN_MENU);
        td5_net_set_mode(TD5_NET_MODE_LAN);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("HOST NEW LAN GAME",  120, 193, 496, 0x30);
        frontend_create_button("DISCOVER LAN GAMES", 120, 257, 496, 0x30);
        frontend_create_button(SNK_BackButTxt, 232, 377, 112, 0x20);
        s_inner_state = 1;
        break;

    case 1:
        frontend_present_buffer();
        if (s_input_ready) {
            if (s_button_index == 0)              /* HOST -> name entry + create */
                td5_frontend_set_screen(TD5_SCREEN_CREATE_SESSION);
            else if (s_button_index == 1)         /* DISCOVER -> session list */
                td5_frontend_set_screen(TD5_SCREEN_SESSION_PICKER);
            else if (s_button_index == 2)         /* BACK */
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
        }
        break;
    }
}

/* ========================================================================
 * [32] Screen_DirectConnect -- DIRECT IP: host a game (port + UPnP) or join one
 * by IP[:port]. Sub-layouts are swapped in place via frontend_reset_buttons()
 * so button indices never collide (the bug from the old inner-state version).
 * ======================================================================== */
static void Screen_DirectConnect(void) {
    switch (s_inner_state) {
    case 0: /* HOST / JOIN / BACK chooser */
        frontend_init_return_screen(TD5_SCREEN_DIRECT_CONNECT);
        td5_net_set_mode(TD5_NET_MODE_DIRECT);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("HOST GAME", 120, 193, 496, 0x30);
        frontend_create_button("JOIN GAME", 120, 257, 496, 0x30);
        frontend_create_button(SNK_BackButTxt, 232, 377, 112, 0x20);
        s_inner_state = 1;
        break;

    case 1: /* chooser interaction */
        frontend_present_buffer();
        if (s_input_ready) {
            if (s_button_index == 0) {            /* HOST */
                if (td5_net_create_session_ex("TD5RE Game", frontend_net_player_name(),
                                              6, s_net_cfg_game_port, s_net_cfg_enable_upnp)) {
                    s_network_active = 1;
                    s_inner_state = 4;            /* show host status */
                } else {
                    TD5_LOG_W(LOG_TAG, "Direct host create failed");
                    td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                }
            } else if (s_button_index == 1) {     /* JOIN */
                snprintf(s_net_direct_ip, sizeof(s_net_direct_ip), "127.0.0.1");
                s_inner_state = 2;
            } else if (s_button_index == 2) {     /* BACK */
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
            }
        }
        break;

    case 2: /* JOIN: build IP-entry layout (fresh buttons) */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the field; only BACK is a real button (index 0). */
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        frontend_begin_text_input(s_net_direct_ip, (int)sizeof(s_net_direct_ip));
        frontend_set_text_input_prompt("ENTER HOST IP[:PORT]");
        s_inner_state = 3;
        break;

    case 3: /* JOIN: IP entry interaction */
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_text_input_confirmed()) {
            char ip[64];
            int port = s_net_cfg_game_port;
            frontend_net_parse_ip_port(s_net_direct_ip, ip, sizeof(ip), &port);
            if (td5_net_join_direct(ip, port, frontend_net_player_name())) {
                s_network_active = 1;
                s_net_join_pending_ui = 1;
                s_net_join_wait_start = td5_plat_time_ms();
                s_inner_state = 5;                /* wait for JOIN_ACK */
            } else {
                TD5_LOG_W(LOG_TAG, "Direct join '%s' failed", s_net_direct_ip);
                td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
            }
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> chooser */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;

    case 4: /* HOST: show local IP + UPnP status (fresh buttons) */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(td5_net_get_status_text(), 80, 193, 480, 0x40);
        frontend_create_button("CONTINUE", 232, 377, 160, 0x20);
        s_inner_state = 6;
        break;

    case 6: /* HOST status interaction -> lobby */
        frontend_present_buffer();
        if (s_input_ready) {
            s_network_active = 1;
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        }
        break;

    case 5: /* JOIN: wait for the host's JOIN_ACK (slot assigned) */
        frontend_present_buffer();
        if (td5_net_local_slot() >= 0) {
            s_net_join_pending_ui = 0;
            s_network_active = 1;
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        } else if (td5_net_get_join_nak_reason() == 2) {  /* host needs a password */
            s_inner_state = 7;                            /* prompt + retry */
        } else if (td5_net_is_connection_lost() ||
                   (td5_plat_time_ms() - s_net_join_wait_start) > 8000) {
            TD5_LOG_W(LOG_TAG, "Direct join: no response / rejected (full)");
            s_net_join_pending_ui = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;

    case 7: /* JOIN: the host rejected us for a wrong/missing password -> re-prompt */
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        s_lobby_password[0] = '\0';
        frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
        frontend_set_text_input_prompt("PASSWORD REQUIRED");
        s_inner_state = 8;
        break;

    case 8: /* JOIN: password entry interaction -> re-join with the password */
        frontend_handle_text_input_key();
        if (frontend_text_input_confirmed()) {
            char ip[64];
            int port = s_net_cfg_game_port;
            td5_net_set_join_password(s_lobby_password);
            frontend_net_parse_ip_port(s_net_direct_ip, ip, sizeof(ip), &port);
            if (td5_net_join_direct(ip, port, frontend_net_player_name())) {
                s_net_join_wait_start = td5_plat_time_ms();
                s_inner_state = 5;
            } else {
                td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
            }
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> chooser */
            td5_frontend_set_screen(TD5_SCREEN_DIRECT_CONNECT);
        }
        break;
    }
}

/* ========================================================================
 * [9] RunFrontendSessionPicker (0x419CF0) -- Session browser
 * States: ~8
 * ======================================================================== */

/* S10: set the SESSION_PICKER selector (button 0) label from s_net_session_sel
 * (discover-only: 0..count-1 are the discovered LAN sessions; hosting is the
 * separate LAN-menu "HOST" option). */
static void frontend_net_label_session_selector(void) {
    int count = td5_net_get_enum_session_count();
    char buf[64];
    if (count <= 0) {
        s_net_session_sel = 0;
        snprintf(buf, sizeof(buf), "(NO LAN GAMES FOUND)");
    } else {
        if (s_net_session_sel < 0) s_net_session_sel = count - 1;   /* wrap */
        if (s_net_session_sel >= count) s_net_session_sel = 0;
        snprintf(buf, sizeof(buf), "%s  (%d/%d)",
                 td5_net_get_enum_session_name(s_net_session_sel),
                 s_net_session_sel + 1, count);
    }
    strncpy(s_buttons[0].label, buf, sizeof(s_buttons[0].label) - 1);
    s_buttons[0].label[sizeof(s_buttons[0].label) - 1] = '\0';
}

static void Screen_SessionPicker(void) {
    switch (s_inner_state) {
    case 0: /* Init: refresh LAN discovery + build the session selector */
        frontend_init_return_screen(TD5_SCREEN_SESSION_PICKER);
        TD5_LOG_D(LOG_TAG, "SessionPicker: init (LAN discovery)");
        td5_net_set_mode(TD5_NET_MODE_LAN);
        td5_net_enumerate_sessions();             /* broadcast QUERY, collect ANNOUNCEs */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [FIXED 2026-06-02, runtime @0x499c78] list (120,193) 496x128; OK (120,377) 96; BACK (232,377) 112. */
        frontend_create_button(SNK_ChooseSessionButTxt, 120, 193, 496, 128);  /* slot 0: session selector */
        frontend_create_button(SNK_OkButTxt,     120, 377,  96, 0x20);   /* slot 1 */
        frontend_create_button(SNK_BackButTxt,   232, 377, 112, 0x20);   /* slot 2 */
        s_net_session_sel = 0;
        frontend_net_label_session_selector();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Build session list */
        s_inner_state = 2;
        break;

    case 2: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 3;
        }
        break;

    case 3: /* Interaction */
        if (s_input_ready) {
            /* Slot 0 = session selector (host or join target), slot 1 = OK, slot 2 = Back. */
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int delta = frontend_option_delta();
            if (active_button == 0 && delta != 0) {
                s_net_session_sel += (delta > 0) ? 1 : -1;
                frontend_net_label_session_selector();
                frontend_play_sfx(2);
            } else if (s_button_index == 1) { /* OK -> join the selected session */
                if (td5_net_get_enum_session_count() <= 0) {
                    frontend_play_sfx(10);        /* nothing to join */
                } else if (td5_net_join_session(s_net_session_sel, frontend_net_player_name())) {
                    s_network_active = 1;
                    s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
                    s_inner_state = 5;
                } else {
                    TD5_LOG_W(LOG_TAG, "LAN join %d failed", s_net_session_sel);
                    frontend_play_sfx(10);
                }
            } else if (s_button_index == 2) { /* Back -> LAN menu */
                s_return_screen = TD5_SCREEN_LAN_MENU;
                s_inner_state = 5;
            }
        }
        break;

    case 5: /* Slide-out prep */
        frontend_begin_timed_animation();
        s_inner_state = 6;
        break;

    case 6: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    default:
        break;
    }
}

/* ========================================================================
 * [10] RunFrontendCreateSessionFlow (0x41A7B0) -- Session creation
 * States: ~18
 * ======================================================================== */

static void Screen_CreateSession(void) {
    switch (s_inner_state) {
    case 0: /* Init: show "Enter New Session Name" text input */
        frontend_init_return_screen(TD5_SCREEN_CREATE_SESSION);
        TD5_LOG_D(LOG_TAG, "CreateSession: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* The text-input widget IS the name field (gold frame at 120,193);
         * only BACK is a real button (index 0). */
        frontend_create_button(SNK_BackButTxt, 278, 289, 112, 0x20);
        memset(s_create_session_name, 0, sizeof(s_create_session_name));
        strcpy(s_create_session_name, "New Session");
        frontend_begin_text_input(s_create_session_name, (int)sizeof(s_create_session_name));
        frontend_set_text_input_prompt("ENTER SESSION NAME");
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 2;
        }
        break;

    case 2: /* Name input */
        frontend_handle_text_input_key();   /* process keystrokes into the buffer */
        if (frontend_text_input_confirmed()) {
            /* S10: actually host a LAN session under the entered name. */
            td5_net_set_mode(TD5_NET_MODE_LAN);
            if (td5_net_create_session(s_create_session_name, frontend_net_player_name(), 6)) {
                s_network_active = 1;
                s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
            } else {
                TD5_LOG_W(LOG_TAG, "LAN host create failed");
                s_return_screen = TD5_SCREEN_LAN_MENU;
            }
            s_inner_state = 3;
            break;
        }
        if (s_input_ready && s_button_index == 0) {   /* BACK -> LAN menu */
            s_return_screen = TD5_SCREEN_LAN_MENU;
            s_inner_state = 3;
        }
        break;

    case 3: /* Slide-out */
        s_anim_tick = 0;
        s_inner_state = 4;
        break;

    /* [ARCH-DIVERGENCE: DXPTYPE] Orig states 4-15 ran the full DirectPlay
     * host/client handshake (provider negotiation, session create/join,
     * player-slot assignment). Port's DXPTYPE wire format is incompatible
     * with TD5_d3d.exe peers (see file-footer manifest @ ~:10301), so the
     * handshake is unreachable end-to-end. All 12 states collapse to a
     * single transition into the lobby; s_network_active gates the lobby's
     * own behavior. */
    case 4: case 5: case 6: case 7: case 8: case 9:
    case 10: case 11: case 12: case 13: case 14: case 15:
        s_network_active = (s_return_screen == TD5_SCREEN_NETWORK_LOBBY);
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;

    default:
        td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        break;
    }
}

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

static void Screen_NetworkLobby(void) {
    switch (s_inner_state) {
    case 0: /* INITIALIZATION */
        frontend_init_return_screen(TD5_SCREEN_NETWORK_LOBBY);
        TD5_LOG_D(LOG_TAG, "NetworkLobby: state 0 - init");

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_LOBBY=1 boots straight into a host lobby (e.g.
         * --StartScreen=11) so the lobby UI can be inspected without a 2nd PC. */
        if (!s_network_active && getenv("TD5RE_NET_LOBBY")) {
            td5_net_init();
            td5_net_set_mode(TD5_NET_MODE_DIRECT);
            td5_net_create_session_ex("DevLobby", frontend_net_player_name(), 6,
                                      g_td5.ini.net_game_port, 0);
            s_network_active = 1;
        }
#endif

        /* Kick check: if kicked flag set, destroy session, go to SessionLocked */
        if (s_kicked_flag) {
            s_race_active_flag = 0;
            s_network_active = 0;
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        /* If session is sealed (re-entering from car select), unseal */
        if (s_network_active) {
            frontend_net_seal(0);
        }

        frontend_play_sfx(5);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Create UI elements at their faithful resting positions. The original
         * RunFrontendNetworkLobby @0x0041C330 slides these in (case 1) and they
         * rest at counter==0x14 in a TWO-COLUMN layout (halfW=320, halfH=240,
         * iVar9=halfH-0x9f=81): a tall MESSAGE WINDOW + STATUS panel on the left,
         * the CHANGE CAR/START/EXIT action buttons in the right column at x=360.
         * [verified via decomp 2026-06-02] */
        /* S10b: clean net-lobby layout. The ported layout had an empty chat
         * input strip + a "MESSAGE WINDOW" panel that overlapped the roster and
         * the action buttons. Replaced with a left roster (drawn by the overlay,
         * no navigable panel buttons) + a right column of action buttons. Fixed
         * indices: 0=START 1=CHANGE CAR 2=EXIT 3=OPTIONS(host). */
        frontend_create_button(SNK_StartButTxt,     400, 110, 190, 0x28); /* 0 */
        frontend_create_button(SNK_ChangeCarButTxt, 400, 158, 190, 0x28); /* 1 */
        frontend_create_button(SNK_ExitButTxt,      400, 206, 190, 0x28); /* 2 */
        if (frontend_net_is_host())
            frontend_create_button("OPTIONS",       400, 254, 190, 0x28); /* 3 (host) */

        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));
        s_lobby_modal = 0;

        s_chat_dirty = (s_network_active) ? 1 : 0;
        s_lobby_action = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* ANIMATE IN (~600ms) */
        /* Animate buttons sliding into position */
        if (frontend_update_timed_animation(0x14, 333) >= 1.0f) {
            frontend_play_sfx(4);
            s_anim_complete = 1;
            s_inner_state = 2;
        }
        break;

    case 2: /* TRANSITION COMPLETE / ENABLE INPUT */
        /* Set up text input: buffer ptr, max 60 chars, enable */
        s_text_input_state = 1;
        s_inner_state = 3;
        break;

    case 3: /* MAIN INTERACTIVE LOBBY */
        /* Render background and chat input */
        frontend_present_buffer();

#ifndef TD5RE_RELEASE
        /* Dev hook: TD5RE_NET_LOBBY=2 auto-opens the OPTIONS modal once. */
        {
            static int s_dev_modal_once = 0;
            const char *lv = getenv("TD5RE_NET_LOBBY");
            if (lv && lv[0] == '2' && !s_dev_modal_once && !s_lobby_modal &&
                frontend_net_is_host()) {
                s_dev_modal_once = 1;
                s_lobby_max_players = td5_net_get_max_players();
                if (s_lobby_max_players < 2 || s_lobby_max_players > 6) s_lobby_max_players = 6;
                s_lobby_password[0] = '\0';
                frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
                frontend_set_text_input_prompt("PASSWORD (BLANK = OPEN)");
                s_lobby_modal_armed = 1;   /* dev auto-open: no Enter to wait on */
                s_lobby_modal = 1;
            }
        }
#endif

        /* S10b: host OPTIONS modal (max players + password) — takes input focus
         * while open. Password edits via the WM_CHAR text path; Left/Right adjust
         * max players; Enter applies + closes; Esc cancels. */
        if (s_lobby_modal) {
            /* Wait for the OPTIONS-Enter (which opened this modal) to be released
             * before accepting input — otherwise its WM_CHAR key-repeat '\r'
             * instantly confirms and closes the modal ("can't change options"). */
            if (!s_lobby_modal_armed) {
                td5_plat_input_flush_chars();
                if (!td5_plat_input_key_pressed(0x1C))   /* Enter scancode */
                    s_lobby_modal_armed = 1;
                break;
            }
            frontend_handle_text_input_key();
            if (frontend_text_input_confirmed()) {
                td5_net_set_session_limits(s_lobby_max_players, s_lobby_password);
                TD5_LOG_I(LOG_TAG, "Lobby options: max=%d password=%s",
                          s_lobby_max_players, s_lobby_password[0] ? "set" : "none");
                s_lobby_modal = 0;
                s_text_input_state = 1;             /* restore chat input */
                frontend_play_sfx(3);
            } else if (s_arrow_input & 1) {          /* LEFT (robust poll edge) */
                if (s_lobby_max_players > 2) s_lobby_max_players--;
                frontend_play_sfx(2);
            } else if (s_arrow_input & 2) {          /* RIGHT */
                if (s_lobby_max_players < 6) s_lobby_max_players++;
                frontend_play_sfx(2);
            } else if (td5_plat_input_key_pressed(0x01)) {   /* ESC = cancel */
                s_lobby_modal = 0;
                s_text_input_state = 1;
            }
            break;
        }

        /* S10: keep the participant table mirrored to the live roster so the
         * host's ready check + the status panel reflect who has actually
         * joined (slots populated by the JOIN handshake / DXPROSTER). */
        {
            int slot;
            for (slot = 0; slot < 6; slot++)
                s_participant_flags[slot] = td5_net_is_slot_active(slot) ? 1 : 0;
        }

        /* S10: a client auto-launches into the race once the host's DXPSTART
         * rendezvous has activated lockstep sync (td5_net_is_active). The host
         * launches via state 5 -> 0x10 -> 0x11. */
        if ((s_lobby_action == 3) ||
            (!frontend_net_is_host() && td5_net_is_active())) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: client race start (sync active)");
            s_launching_net_race = 1;
            s_race_active_flag = 1;
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }

        /* Process button input (indices: 0=START 1=CHANGE CAR 2=EXIT 3=OPTIONS). */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* START */
                if (frontend_net_is_host()) {
                    if (td5_net_get_player_count() <= 1) {
                        /* Solo host: no peers to rendezvous with -> just play a
                         * single-player race (network_active stays 0, so the
                         * lockstep barrier isn't engaged and can't stall). */
                        TD5_LOG_I(LOG_TAG, "NetworkLobby: solo start (1 player)");
                        s_launching_net_race = 0;
                        s_race_active_flag = 1;
                        frontend_init_race_schedule();
                        frontend_init_display_mode_state();
                        return;
                    }
                    s_lobby_action = 2;
                    s_inner_state = 5; /* multi-player ready check -> DXPSTART */
                }
                /* Client: waits for the host's DXPSTART (handled in the sync poll). */
                break;

            case 1: /* CHANGE CAR */
                s_lobby_action = 1;
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;

            case 2: /* EXIT -> tear down the session and leave the lobby */
                TD5_LOG_I(LOG_TAG, "NetworkLobby: exit -> destroy session");
                frontend_net_destroy();
                s_network_active = 0;
                s_lobby_modal = 0;
                td5_frontend_set_screen(TD5_SCREEN_CONNECTION_BROWSER);
                return;

            case 3: /* OPTIONS (host) -> open the max-players/password modal */
                if (frontend_net_is_host()) {
                    s_lobby_max_players = td5_net_get_max_players();
                    if (s_lobby_max_players < 2 || s_lobby_max_players > 6)
                        s_lobby_max_players = 6;
                    s_lobby_password[0] = '\0';
                    frontend_begin_text_input(s_lobby_password, (int)sizeof(s_lobby_password));
                    frontend_set_text_input_prompt("PASSWORD (BLANK = OPEN)");
                    s_lobby_modal_armed = 0;   /* arm after the Enter is released */
                    s_lobby_modal = 1;
                    frontend_play_sfx(3);
                }
                break;

            default:
                s_lobby_action = 0;
                break;
            }
        }

        /* Process network messages */
        /* Check for disconnect */
        if (s_kicked_flag) {
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        /* Update lobby player list display */
        /* Poll network input */

        /* Check text input confirmed (Enter pressed) -> chat submit */
        if (frontend_text_input_confirmed()) {
            s_inner_state = 4;
        }
        break;

    case 4: /* CHAT TEXT SUBMISSION */
        /* Process chat input buffer: check admin commands, emoticons */
        /* If valid, send as DXPCHAT (type 2) */
        if (s_chat_input_buffer[0] != '\0') {
            frontend_net_send(2, s_chat_input_buffer, (int)strlen(s_chat_input_buffer) + 1);
        }
        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));
        s_inner_state = 2; /* re-enable text input */
        break;

    case 5: /* PLAYER READY CHECK (host only) */
    {
        int slot, active_count = 0, ready_count = 0;
        /* Write local status */
        s_per_slot_status[frontend_net_local_slot()] = s_lobby_action;

        for (slot = 0; slot < 6; slot++) {
            if (s_participant_flags[slot] != 0) {
                active_count++;
                if (s_per_slot_status[slot] == 2) {
                    ready_count++;
                }
            }
        }

        (void)ready_count;
        if (active_count >= 2) {
            /* S10: enough players have joined -> begin the DXPSTART rendezvous.
             * The per-slot config/settings exchange (states 0xC-0xF) is bypassed
             * for the lockstep path -- only input bitmasks + dt are synced at
             * race time, so no pre-race car/settings replication is required. */
            s_inner_state = 0x10;
        } else {
            /* Not enough players to start a network race. */
            s_dialog_mode = 1;
            frontend_play_sfx(5);
            s_inner_state = 6;
        }
    }
        break;

    case 6: /* ERROR/CONFIRMATION ANIMATE IN (24 frames) */
        s_anim_tick = 0;
        /* Create overlay surface with error message:
         * dialog_mode < 2: SNK_NetErrString1/2
         * dialog_mode >= 2: SNK_NetErrString3/4 */
        s_inner_state = 7;
        break;

    case 7: /* SHOW DIALOG BUTTONS */
        /* Create Yes/No or OK button based on dialog_mode */
        if (s_dialog_mode == 0 || s_dialog_mode == 2) {
            frontend_create_button(SNK_YesButTxt, -80, 0, 80, 0x20);
            frontend_create_button(SNK_NoxButTxt,  -80, 0, 80, 0x20);
        } else {
            frontend_create_button(SNK_OkButTxt, -80, 0, 80, 0x20);
        }
        s_inner_state = 8;
        break;

    case 8: /* DIALOG INPUT HANDLING */
        /* Continue processing network messages */
        if (s_kicked_flag) {
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_SESSION_LOCKED);
            return;
        }

        if (s_input_ready) {
            if (s_dialog_mode == 1) {
                /* Info/OK: any press advances */
                s_inner_state = 9;
            } else if (s_button_index == 0) {
                /* Yes: confirm exit */
                s_dialog_mode = 0;
                s_inner_state = 9;
            } else {
                /* No: cancel */
                s_dialog_mode = 1;
                s_inner_state = 9;
            }
        }
        break;

    case 9: /* DIALOG RESOLUTION */
        if (s_dialog_mode == 0) {
            /* Confirmed exit -> start pre-race or disconnect */
            s_inner_state = 0x0C;
        } else if (s_dialog_mode == 2) {
            /* Forced disconnect */
            frontend_net_destroy();
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        } else {
            /* Cancelled -> animate out, return to lobby */
            frontend_play_sfx(5);
            s_inner_state = 10;
        }
        break;

    case 10: /* ERROR DIALOG ANIMATE OUT (24 frames) */
        s_anim_tick = 0;
        s_inner_state = 11;
        break;

    case 11:
        s_anim_tick += 2;
        if (s_anim_tick >= 0x0C) { /* 12 frames */
            s_lobby_action = 0;
            s_inner_state = 2; /* return to enabled input */
        }
        break;

    case 0x0C: /* SEAL SESSION & COLLECT CONFIGS (host only) */
        TD5_LOG_I(LOG_TAG, "NetworkLobby: seal session, collect configs");
        frontend_net_seal(1);

        /* Build participant table, kick non-ready players */
        {
            int slot;
            for (slot = 0; slot < 6; slot++) {
                s_config_received[slot] = 0;
                if (s_participant_flags[slot] && s_per_slot_status[slot] != 2) {
                    /* Kick non-ready: send LOBBY_KICK (opcode 0x12) */
                    uint8_t kick_msg[8] = {0x12, 0, 0, 0, 0, 0, 0, 0};
                    kick_msg[4] = (uint8_t)slot;
                    frontend_net_send(1, kick_msg, 8);
                    s_participant_flags[slot] = 0;
                }
            }
        }

        /* Store host's own config, mark received */
        s_config_received[frontend_net_local_slot()] = 1;
        s_last_poll_timestamp = td5_plat_time_ms();
        s_inner_state = 0x0D;
        break;

    case 0x0D: /* POLL CLIENT CONFIGS (250ms interval) */
    {
        uint32_t now = td5_plat_time_ms();
        /* Process network (receive config replies) */

        if ((now - s_last_poll_timestamp) > 250) {
            s_last_poll_timestamp = now;
            int all_done = 1;
            int slot;
            for (slot = 0; slot < 6; slot++) {
                if (s_participant_flags[slot] && !s_config_received[slot]) {
                    /* Send LOBBY_REQUEST_CONFIG (opcode 0x13) */
                    uint8_t req_msg[4] = {0x13, 0, 0, 0};
                    req_msg[1] = (uint8_t)slot;
                    frontend_net_send(1, req_msg, 4);
                    all_done = 0;
                    break; /* one per tick */
                }
            }
            if (all_done) {
                s_inner_state = 0x0E;
            }
        }
    }
        break;

    case 0x0E: /* INITIALIZE RACE SCHEDULE (host only) */
        TD5_LOG_I(LOG_TAG, "NetworkLobby: init race schedule");
        {
            int slot;
            for (slot = 0; slot < 6; slot++) s_config_received[slot] = 0;
            s_config_received[frontend_net_local_slot()] = 1;
        }
        s_race_active_flag = 1;
        /* Fill empty slots with AI-controlled cars */
        s_last_poll_timestamp = td5_plat_time_ms();
        s_inner_state = 0x0F;
        break;

    case 0x0F: /* BROADCAST SETTINGS TO CLIENTS (165ms interval) */
    {
        uint32_t now = td5_plat_time_ms();
        s_anim_tick = 0;

        if ((now - s_last_poll_timestamp) > 165) {
            s_last_poll_timestamp = now;
            int all_acked = 1;
            int slot;
            for (slot = 0; slot < 6; slot++) {
                if (s_participant_flags[slot] && !s_config_received[slot]) {
                    /* Send LOBBY_SETTINGS (opcode 0x15, 0x80 bytes) */
                    uint8_t settings_msg[0x80];
                    memset(settings_msg, 0, sizeof(settings_msg));
                    settings_msg[0] = 0x15;
                    frontend_net_send(1, settings_msg, 0x80);
                    all_acked = 0;
                    break;
                }
            }
            if (all_acked) {
                s_inner_state = 0x10;
            }
        }
    }
        break;

    case 0x10: /* LAUNCH COUNTDOWN (8 ticks, then send DXPSTART) */
        s_anim_tick += 2;
        if (s_anim_tick >= 8) {
            s_race_active_flag = 1;
            /* Send DXPSTART (message type 4) */
            frontend_net_send(4, s_participant_flags, 0);
            s_inner_state = 0x11;
        }
        break;

    case 0x11: /* WAIT FOR START CONFIRMATION */
    {
        /* The DXPSTART rendezvous (handle_start / ack_reply / start_confirm in
         * td5_net.c, driven by the worker thread) activates lockstep sync on all
         * machines. Drain the ring and launch once sync is active. */
        uint8_t recv_buf[256];
        (void)frontend_net_receive(recv_buf, sizeof(recv_buf));
        if (td5_net_is_active()) {
            TD5_LOG_I(LOG_TAG, "NetworkLobby: host race start (sync active)");
            s_launching_net_race = 1;
            s_race_active_flag = 1;
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }
    }
        break;
    }
}

/* ========================================================================
 * [12] ScreenOptionsHub (0x41D890) -- Options category selection
 * States: 10
 * 6 buttons: Game, Control, Sound, Graphics, Two Player, OK
 * ======================================================================== */

static void Screen_OptionsHub(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        frontend_init_return_screen(TD5_SCREEN_OPTIONS_HUB);
        TD5_LOG_D(LOG_TAG, "OptionsHub: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;

        /* [FIXED 2026-06-01, runtime @0x499c78] rows x=120 y=97 step40 (304-wide); OK (216,377) 96-wide. */
        frontend_create_button(SNK_GameOptionsButTxt,      120,  97, 0x130, 0x20);
        frontend_create_button(SNK_ControlOptionsButTxt,   120, 137, 0x130, 0x20);
        frontend_create_button(SNK_SoundOptionsButTxt,     120, 177, 0x130, 0x20);
        frontend_create_button(SNK_GraphicsOptionsButTxt,  120, 217, 0x130, 0x20);
        frontend_create_button(SNK_TwoPlayerOptionsButTxt, 120, 257, 0x130, 0x20);
        frontend_create_button(SNK_OkButTxt,               216, 377, 0x60,  0x20);

        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: /* Present */
    case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Animation stabilize */
    case 5:
        s_inner_state++;
        break;

    case 6: /* Interaction */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: s_return_screen = TD5_SCREEN_GAME_OPTIONS;       s_inner_state = 7; break;
            case 1: s_return_screen = TD5_SCREEN_CONTROL_OPTIONS;    s_inner_state = 7; break;
            case 2: s_return_screen = TD5_SCREEN_SOUND_OPTIONS;      s_inner_state = 7; break;
            case 3: s_return_screen = TD5_SCREEN_DISPLAY_OPTIONS;    s_inner_state = 7; break;
            case 4: s_return_screen = TD5_SCREEN_TWO_PLAYER_OPTIONS; s_inner_state = 7; break;
            case 5: /* OK -> return to main menu.
                     * PARITY NOTE (audit 2026-05-30): the original 0x0041D890 OK case
                     * commits the option shadows to live globals here (camera =
                     * collisions^1 @0x41dc8e, dynamics @0x41dc82, traffic/cops, and
                     * gRaceDifficultyTier @0x41dc9f). The port uses an equivalent but
                     * DEFERRED model: the option screens edit s_game_option_* directly
                     * and ConfigureGameTypeFlags applies them at race launch (see
                     * td5_frontend.c case 0 of the game-type switch: difficulty->tier,
                     * traffic, cops, collisions, dynamics, checkpoint timers). Race
                     * launch is the sole consumer path, so the user's choices still take
                     * effect without an explicit shadow->live commit here. Adding one
                     * would risk double-application. Left as-is intentionally. */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 7;
                break;
            }
        }
        break;

    case 7: /* Slide-out prep */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;

    case 9: /* Exit */
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

/* ========================================================================
 * [13] ScreenGameOptions (0x41F990)
 * Standard options screen pattern (10 states).
 * 7 toggle rows + OK button.
 * ======================================================================== */

static void Screen_GameOptions(void) {
    switch (s_inner_state) {
    case 0: /* Init: create option rows + OK */
        frontend_init_return_screen(TD5_SCREEN_GAME_OPTIONS);
        TD5_LOG_D(LOG_TAG, "GameOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* 6 option rows with left/right arrows:
         * Checkpoint Timers, Traffic, Cops, Difficulty, Dynamics, 3D Collisions.
         * [S02 (c) 2026-06-04] CIRCUIT LAPS was removed from this screen — the lap
         * count is now set in the Quick Race menu + the Track Selection screen
         * (both edit s_game_option_laps). The value still feeds race setup via
         * g_td5.circuit_lap_count = s_game_option_laps + 1; this screen no longer
         * owns it. Remaining rows shifted up one slot to close the gap. */
        /* [FIXED 2026-06-01, runtime button-table @0x499c78] explicit rests: rows at x=120,
         * y=97 step 40; OK bottom-center (216,377). (Port auto-layout gave 110/93 + OK stacked
         * in-column — 10px left / 4px high / OK mis-placed.) Slide-in still animates X to rest. */
        frontend_create_button(SNK_CheckpointTimersButTxt, 120,  97, 0x128, 0x20);
        frontend_create_button(SNK_TrafficButTxt,          120, 137, 0x128, 0x20);
        frontend_create_button(SNK_CopsButTxt,             120, 177, 0x128, 0x20); /* orig label: POLICE */
        frontend_create_button(SNK_DifficultyButTxt,       120, 217, 0x128, 0x20);
        frontend_create_button(SNK_DynamicsButTxt,         120, 257, 0x128, 0x20);
        frontend_create_button(SNK_3dCollisionsButTxt,     120, 297, 0x128, 0x20);
        frontend_create_button(SNK_OkButTxt,               216, 377, 0x60,  0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: /* Draw current values */
    case 5:
        /* Render current option values on the panel */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: arrow handlers per row */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            /* Each row cycles its respective global on arrow input.
             * OK button triggers exit. [S02 (c) 2026-06-04] Circuit Laps (old
             * idx 0) was removed; the remaining six rows shifted up one index. */
            if (delta != 0) {
                if (active_button == 0) {
                    s_game_option_checkpoint_timers ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 1) {
                    s_game_option_traffic ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 2) {
                    s_game_option_cops ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 3) {
                    s_game_option_difficulty += delta;
                    if (s_game_option_difficulty < 0) s_game_option_difficulty = 2;
                    if (s_game_option_difficulty > 2) s_game_option_difficulty = 0;
                    s_inner_state = 4;
                } else if (active_button == 4) {
                    s_game_option_dynamics ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 5) {
                    s_game_option_collisions ^= 1;
                    s_inner_state = 4;
                }
            }
            if (s_button_index == 6) { /* OK */
                /* Sync the committed game options into g_td5.ini (the global the
                 * boot-override at frontend init reads) and write them back to
                 * td5re.ini so the selection survives a relaunch. The original
                 * persisted these to Config.td5 only, but the port's td5re.ini
                 * boot-override masks Config.td5, so the ini is the live config
                 * layer that must be kept in sync. [PART B 2026-06-02]
                 * NB: laps is intentionally NOT written here anymore — this
                 * screen no longer owns it (re-homed to Quick Race + Track
                 * Selection, which persist g_td5.ini.laps themselves).
                 * [S02 (c) 2026-06-04] */
                g_td5.ini.checkpoint_timers = s_game_option_checkpoint_timers;
                g_td5.ini.traffic           = s_game_option_traffic;
                g_td5.ini.cops              = s_game_option_cops;
                g_td5.ini.difficulty        = s_game_option_difficulty;
                g_td5.ini.dynamics          = s_game_option_dynamics;
                g_td5.ini.collisions        = s_game_option_collisions;
                td5_ini_persist_options();
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
            /* Arrow changes reset to state 4 for redraw */
        }
        break;

    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;

    case 9:
        td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        break;
    }
}

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

/* Recompute the player range from the live device count ("hot-swap") and re-seed
 * each player's source from the persisted device index, dropping any index that
 * now points at an unplugged device. */
static void ctrl_opts_refresh_devices(void)
{
    int dev_count = td5_input_enumerate_devices();
    int joys, n, p;
    if (dev_count < 1) dev_count = 1;
    joys = dev_count - 1;            /* device 0 = keyboard */
    if (joys < 0) joys = 0;
    /* Range = connected joysticks, but never fewer than the declared human count
     * (so every active player can still be configured), clamped to [1,9].
     * [INTERPRETATION] the spec says "depending on the amount of joysticks"; the
     * max(.., num_human_players) keeps keyboard-only multiplayer configurable. */
    n = joys;
    if (n < s_num_human_players) n = s_num_human_players;
    if (n < 1) n = 1;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    s_ctrl_opts_max_players = n;
    if (s_ctrl_opts_player >= n) s_ctrl_opts_player = n - 1;
    if (s_ctrl_opts_player < 0)  s_ctrl_opts_player = 0;

    for (p = 0; p < TD5_MAX_HUMAN_PLAYERS; p++) {
        int idx = (int)td5_save_get_player_device_index(p);
        if (idx < 0 || idx >= dev_count) idx = 0;   /* unplugged → keyboard */
        td5_input_set_input_source(p, idx);
    }
    TD5_LOG_I(LOG_TAG, "ControlOptions: devices=%d joys=%d range=1..%d player=%d",
              dev_count, joys, s_ctrl_opts_max_players, s_ctrl_opts_player + 1);
}

/* Cycle the selected player's input device by delta. Keyboard (0) is always
 * available + shareable; joysticks (>=1) are exclusive (skip any already taken
 * by another player). */
static void ctrl_opts_cycle_device(int delta)
{
    int dev_count = td5_input_enumerate_devices();
    int src, guard = 0;
    if (dev_count < 1) dev_count = 1;
    src = td5_input_get_input_source(s_ctrl_opts_player);
    do {
        int taken = 0, p;
        src += delta;
        if (src < 0) src = dev_count - 1;
        if (src >= dev_count) src = 0;
        if (src == 0) break;                       /* keyboard: always OK / shareable */
        for (p = 0; p < s_ctrl_opts_max_players; p++) {
            if (p == s_ctrl_opts_player) continue;
            if (td5_input_get_input_source(p) == src) { taken = 1; break; }
        }
        if (!taken) break;
    } while (++guard < dev_count * 2);
    td5_input_set_input_source(s_ctrl_opts_player, src);
    td5_save_set_player_device_index(s_ctrl_opts_player, (uint32_t)src);
    TD5_LOG_I(LOG_TAG, "ControlOptions: player %d device -> %d",
              s_ctrl_opts_player + 1, src);
}

static void Screen_ControlOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_CONTROL_OPTIONS);
        TD5_LOG_D(LOG_TAG, "ControlOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Controllers.tga icon must be BLACK-color-keyed (orig DDCKEY_SRCBLT @0x412030). */
        s_control_options_surface = frontend_load_tga_ck("Controllers.TGA", "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        /* Hot-swap: re-enumerate devices + refresh range/sources on every entry. */
        ctrl_opts_refresh_devices();
        /* Non-selector rows (labels bake); ◄► arrows drawn by the per-screen
         * dispatch, values by the overlay. PLAYER(0) + CONTROLLER SELECTION(1)
         * cycle on ◄►; CONFIGURE(2) + OK(3) are plain buttons. */
        frontend_reset_buttons();
        frontend_create_button(SNK_PlayerSelectButTxt,     120,  97, 0x100, 0x20);  /* 0 */
        frontend_create_button(SNK_ControllerSelectButTxt, 120, 177, 0x100, 0x20);  /* 1 */
        frontend_create_button(SNK_ConfigureButTxt,        120, 257, 0x100, 0x20);  /* 2 */
        frontend_create_button(SNK_OkButTxt,               200, 377, 0x60,  0x20);  /* 3 */
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4:
        s_inner_state = 5;
        break;
    case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int delta = frontend_option_delta();
            if (active_button == 0 && delta != 0) {
                /* PLAYER selector (wrap within the live range). */
                int n = s_ctrl_opts_player + delta;
                int range = (s_ctrl_opts_max_players > 0) ? s_ctrl_opts_max_players : 1;
                while (n < 0) n += range;
                n %= range;
                s_ctrl_opts_player = n;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == 1 && delta != 0) {
                /* CONTROLLER SELECTION device cycle. */
                ctrl_opts_cycle_device(delta);
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) {
                /* CONFIGURE → per-button remap for the selected player. */
                s_ctrl_player = s_ctrl_opts_player;
                s_return_screen = TD5_SCREEN_CONTROLLER_BINDING;
                s_inner_state = 7;
            } else if (s_button_index == 3) {
                /* OK → persist device assignments + return to hub. */
                td5_save_write_config(NULL);
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;
    case 9:
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

/* ========================================================================
 * [15] ScreenSoundOptions (0x41EA90) -- Standard options pattern
 * [PORT REWORK 2026-06-05 / S15] The SFX Mode row was removed per user
 * feedback. 3 rows remain: SFX Volume, Music Volume, Music Test + OK.
 * Buttons re-indexed 0..3 (was 0..4) and reflowed up one slot to fill the gap.
 * ======================================================================== */

static void Screen_SoundOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_SOUND_OPTIONS);
        TD5_LOG_D(LOG_TAG, "SoundOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [CONFIRMED @ 0x0041EA90] SFX-mode icon source is Controllers.tga (64x224 =
         * 7 rows of 64x32), blitted at row (sfx_mode+4): modes 0/1/2 -> rows 4/5/6.
         * Replaces the prior 2-state Stereo.tga/Mono.tga pair, which could not render
         * a distinct icon for the 3rd (surround) mode. */
        /* [FIXED 2026-06-01] black-color-keyed like the Control Options icon (was opaque
         * black box). Controllers.tga background is black; key it transparent. */
        s_sound_volumebox_surface  = frontend_load_tga("VolumeBox.tga", "Front End/frontend.zip");
        s_sound_volumefill_surface = frontend_load_tga("VolumeFill.tga","Front End/frontend.zip");
        /* [PORT REWORK 2026-06-05 / S15] SFX Mode row (was 120,97 + the
         * Controllers.tga icon load) removed. Remaining rows reflowed up one
         * slot, keeping their original 40/80/80 spacing:
         *   SFX Volume 97, Music Volume 137, Music Test 217, OK 297. */
        frontend_create_button(SNK_SfxVolumeButTxt,   120,  97, 0x100, 0x20);  /* btn 0 */
        frontend_create_button(SNK_MusicVolumeButTxt, 120, 137, 0x100, 0x20);  /* btn 1 */
        frontend_create_button(SNK_MusicTestButTxt,   120, 217, 0x100, 0x20);  /* btn 2 */
        frontend_create_button(SNK_OkButTxt,          200, 297, 0x60,  0x20);  /* btn 3 */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4: case 5:
        /* Render volume bars */
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            /* [PORT REWORK 2026-06-05 / S15] SFX Mode row removed; sliders are
             * now button 0 (SFX volume) and button 1 (Music volume). */
            if (delta != 0 && active_button >= 0 && active_button <= 1) {
                if (active_button == 0) {
                    /* SFX volume. REG-2 fix 2026-05-22: orig step is delta * 10. */
                    s_sound_option_sfx_volume += delta * 10;
                    if (s_sound_option_sfx_volume < 0) s_sound_option_sfx_volume = 0;
                    if (s_sound_option_sfx_volume > 100) s_sound_option_sfx_volume = 100;
                    td5_save_set_sfx_volume(s_sound_option_sfx_volume);
                    td5_sound_set_sfx_volume(s_sound_option_sfx_volume);
                } else { /* active_button == 1: Music volume */
                    /* REG-2 fix 2026-05-22: orig step delta * 10. */
                    s_sound_option_music_volume += delta * 10;
                    if (s_sound_option_music_volume < 0) s_sound_option_music_volume = 0;
                    if (s_sound_option_music_volume > 100) s_sound_option_music_volume = 100;
                    td5_save_set_music_volume(s_sound_option_music_volume);
                    td5_sound_set_music_volume(s_sound_option_music_volume);
                }
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) { /* Music Test */
                s_return_screen = TD5_SCREEN_MUSIC_TEST;
                s_inner_state = 7;
            } else if (s_button_index == 3) { /* OK */
                /* Persist sound options to td5re.ini so they survive a relaunch
                 * (see PART B note in Screen_GameOptions). Volume changes already
                 * applied live via td5_save_set_*; sync the committed values into
                 * g_td5.ini and write them back. [PART B 2026-06-02]
                 * sfx_mode is no longer user-editable here (row removed) but is
                 * still written so its loaded value is preserved across the save. */
                g_td5.ini.sfx_mode     = s_sound_option_sfx_mode;
                g_td5.ini.sfx_volume   = s_sound_option_sfx_volume;
                g_td5.ini.music_volume = s_sound_option_music_volume;
                td5_ini_persist_options();
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 9;
        }
        break;
    case 9:
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        break;
    }
}

/* ========================================================================
 * [16] ScreenDisplayOptions (0x420400)
 * States: 9. Rows: Resolution, Fogging, Speed Readout, Camera Damping, OK
 * ======================================================================== */

static void Screen_DisplayOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_DISPLAY_OPTIONS);
        TD5_LOG_D(LOG_TAG, "DisplayOptions: init (display_mode_index=%d, count=%d)",
                  s_display_mode_index, s_display_mode_count);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [S01 Display options 2026-06-04] 6 option rows + OK. The discrete
         * Resolution row was removed — the window is now freely resizable
         * (drag the border / maximize, or pick Borderless/Fullscreen via Display
         * Mode), so a fixed resolution list is redundant. Labels are overridden
         * by frontend_refresh_display_option_labels (the SNK arg is a placeholder).
         * Rows top-to-bottom:
         *   0 Display Mode  1 VSync  2 Fogging
         *   3 Speed Readout 4 Show FPS  5 Camera Damping  6 OK
         * PARITY NOTE (audit 2026-05-30): orig 0x00420484 makes Fogging a DISABLED
         * preview button when DXD3D::CanFog()!=1; the D3D11 backend always supports
         * fog so the faithful result is an always-live Fogging row. */
        frontend_create_button(SNK_ResolutionButTxt,    120,  97, 0x120, 0x20); /* Display Mode */
        frontend_create_button(SNK_FoggingButTxt,       120, 137, 0x120, 0x20); /* VSync */
        frontend_create_button(SNK_FoggingButTxt,       120, 177, 0x120, 0x20); /* Fogging */
        frontend_create_button(SNK_SpeedReadoutButTxt,  120, 217, 0x120, 0x20); /* Speed Readout */
        frontend_create_button(SNK_SpeedReadoutButTxt,  120, 257, 0x120, 0x20); /* Show FPS */
        frontend_create_button(SNK_CameraDampingButTxt, 120, 297, 0x120, 0x20); /* Camera Damping */
        frontend_create_button(SNK_OkButTxt,            200, 377, 0x60,  0x20); /* OK */
        frontend_refresh_display_option_labels();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4:
        frontend_refresh_display_option_labels();
        s_inner_state = 5;
        break;
    case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            int changed = 0;

            if (active_button == 0 && delta != 0) {
                /* Row 0 — Display Mode: Fullscreen(0) -> Windowed(1) -> Borderless(2) */
                s_display_window_mode += delta;
                if (s_display_window_mode < 0) s_display_window_mode = 2;
                if (s_display_window_mode > 2) s_display_window_mode = 0;
                g_td5.ini.window_mode = s_display_window_mode;
                td5_plat_set_window_mode(s_display_window_mode);
                changed = 1;
            } else if (active_button == 1 && delta != 0) {
                /* Row 1 — VSync on/off (applied live) */
                s_display_vsync = !s_display_vsync;
                g_td5.ini.vsync = s_display_vsync;
                td5_plat_set_vsync(s_display_vsync);
                changed = 1;
            } else if (active_button == 2 && delta != 0) {
                /* Row 2 — Fogging on/off */
                s_display_fog_enabled = !s_display_fog_enabled;
                g_td5.ini.fog_enabled = s_display_fog_enabled;
                changed = 1;
            } else if (active_button == 3 && delta != 0) {
                /* Row 3 — Speed Readout MPH/KPH (applied live to the HUD) */
                s_display_speed_units = !s_display_speed_units;
                g_td5.ini.speed_units = s_display_speed_units;
                td5_save_set_speed_units(s_display_speed_units);
                changed = 1;
            } else if (active_button == 4 && delta != 0) {
                /* Row 4 — Show FPS overlay on/off */
                s_display_show_fps = !s_display_show_fps;
                g_td5.ini.show_fps = s_display_show_fps;
                changed = 1;
            } else if (active_button == 5 && delta != 0) {
                /* Row 5 — Camera Damping 0..9 (clamp, no wrap) */
                s_display_camera_damping += delta;
                if (s_display_camera_damping < 0) s_display_camera_damping = 0;
                if (s_display_camera_damping > 9) s_display_camera_damping = 9;
                changed = 1;
            } else if (s_button_index == 6) {
                /* OK — persist every display option to td5re.ini. Resolution +
                 * window-mode/vsync already applied live; this writes them (plus
                 * fog / units / damping / W,H) so they survive a relaunch. */
                g_td5.ini.window_mode    = s_display_window_mode;
                g_td5.ini.vsync          = s_display_vsync;
                g_td5.ini.show_fps       = s_display_show_fps;
                g_td5.ini.fog_enabled    = s_display_fog_enabled;
                g_td5.ini.speed_units    = s_display_speed_units;
                g_td5.ini.camera_damping = s_display_camera_damping;
                td5_save_set_speed_units(s_display_speed_units);
                td5_ini_persist_options();
                s_inner_state = 7;
                break;
            }

            if (changed) {
                s_inner_state = 4;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        }
        break;
    }
}

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

/* (Re)build the Multiplayer Options row buttons for the current player count. */
static void mp_build_buttons(void)
{
    int n = s_num_human_players;
    int cols = 0, rows = 0, missing = 0;
    int y;
    if (n < 1) n = 1;
    if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
    s_num_human_players = n;

    mp_split_layouts(n, &s_mp_layout_optcount);
    if (s_mp_layout_sel < 0 || s_mp_layout_sel >= s_mp_layout_optcount)
        s_mp_layout_sel = 0;
    mp_resolve_layout(n, s_mp_layout_sel, &cols, &rows, &missing);
    s_mp_missing_count = missing;

    frontend_reset_buttons();
    s_mp_btn_players = s_mp_btn_catchup = s_mp_btn_layout = s_mp_btn_ok = -1;
    s_mp_btn_missing[0] = s_mp_btn_missing[1] = -1;
    s_mp_btn_nickname = -1;

    /* Rows are NON-selector buttons: the button renderer bakes their label (the
     * ◄► arrows are drawn by the per-screen dispatch, the value by the overlay).
     * This matches the Game/Sound/Display Options pattern; a real selector button
     * suppresses its label, which would leave the row blank. */
    y = 77;
    s_mp_btn_players = frontend_create_button(SNK_MpPlayersButTxt, 120, y, 0x100, 0x20);
    y += 50;
    /* [S05 2026-06-04] CATCHUP toggle row, between PLAYERS and SPLIT LAYOUT. The
     * value is the persisted AI rubber-band assist (td5_save get/set_catchup_assist,
     * default 1 = on); S06's td5_ai_get_catchup_level() consumes it (ON = softened
     * rubber-band, OFF = no player-distance boost/cut). */
    s_mp_btn_catchup = frontend_create_button(SNK_CatchupTxt, 120, y, 0x100, 0x20);
    y += 50;
    s_mp_btn_layout = frontend_create_button(SNK_MpLayoutButTxt, 120, y, 0x100, 0x20);
    y += 50;
    for (int k = 0; k < missing && k < 2; k++) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, "%s %d", SNK_MpDisplayButTxt, k + 1);
        s_mp_btn_missing[k] = frontend_create_button(lbl, 120, y, 0x100, 0x20);
        y += 50;
    }

    /* S10: NICKNAME row sits dynamically BELOW the split-layout + missing-cell
     * rows (whose count varies with player count / layout). Pressing it opens
     * the nickname-entry screen; the current nickname is shown as its value. */
    s_mp_btn_nickname = frontend_create_button("NICKNAME", 120, y, 0x100, 0x20);
    y += 50;

    s_mp_btn_ok = frontend_create_button(SNK_OkButTxt, 200, 377, 0x60, 0x20);

    TD5_LOG_I(LOG_TAG,
              "MultiplayerOptions buttons: n=%d optcount=%d missing=%d grid=%dx%d",
              n, s_mp_layout_optcount, missing, cols, rows);
}

static void Screen_TwoPlayerOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_TWO_PLAYER_OPTIONS);
        TD5_LOG_D(LOG_TAG, "MultiplayerOptions: init players=%d layout_sel=%d",
                  s_num_human_players, s_mp_layout_sel);
        /* SplitScreen.tga (split-layout preview icon) — drawn OPAQUE, no colorkey. */
        s_split_screen_surface = frontend_load_tga("SplitScreen.tga", "Front End/frontend.zip");
        mp_build_buttons();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4: case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;

            if (active_button == s_mp_btn_players && delta != 0) {
                int n = s_num_human_players + delta;
                if (n < 1) n = 1;
                if (n > TD5_MAX_HUMAN_PLAYERS) n = TD5_MAX_HUMAN_PLAYERS;
                if (n != s_num_human_players) {
                    s_num_human_players = n;
                    if (s_num_ai_opponents > TD5_MAX_RACER_SLOTS - n)
                        s_num_ai_opponents = TD5_MAX_RACER_SLOTS - n;
                    s_mp_layout_sel = 0;     /* layout list changed → reset selection */
                    mp_build_buttons();      /* row set depends on N → rebuild */
                    s_selected_button = (s_mp_btn_players >= 0) ? s_mp_btn_players : 0;
                    frontend_play_sfx(2);
                }
                s_inner_state = 4;
            } else if (active_button == s_mp_btn_catchup && delta != 0) {
                /* [S05 2026-06-04] CATCHUP on/off toggle. Either arrow flips it;
                 * persisted via td5_save (organized td5re_input.ini [Assist]) and
                 * consumed by S06's td5_ai_get_catchup_level(). 0 = off, 1 = on. */
                int cur = td5_save_get_catchup_assist();
                td5_save_set_catchup_assist(cur > 0 ? 0 : 1);
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == s_mp_btn_layout && delta != 0 &&
                       s_mp_layout_optcount > 1) {
                int sel = s_mp_layout_sel + delta;
                while (sel < 0) sel += s_mp_layout_optcount;
                sel %= s_mp_layout_optcount;
                if (sel != s_mp_layout_sel) {
                    s_mp_layout_sel = sel;
                    mp_build_buttons();      /* missing-cell count may change → rebuild */
                    s_selected_button = (s_mp_btn_layout >= 0) ? s_mp_btn_layout : 0;
                    frontend_play_sfx(2);
                }
                s_inner_state = 4;
            } else if (delta != 0 &&
                       (active_button == s_mp_btn_missing[0] ||
                        active_button == s_mp_btn_missing[1])) {
                int k = (active_button == s_mp_btn_missing[1]) ? 1 : 0;
                int v = s_mp_missing_content[k] + delta;
                while (v < 0) v += MP_MISSING_CONTENT_COUNT;
                v %= MP_MISSING_CONTENT_COUNT;
                s_mp_missing_content[k] = v;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == s_mp_btn_nickname) {
                /* Open the nickname-entry screen; return here on confirm. */
                s_nickname_from_mpopts = 1;
                td5_frontend_set_screen(TD5_SCREEN_NET_NICKNAME);
                return;
            } else if (s_button_index == s_mp_btn_ok) {
                s_inner_state = 7;
            }
        }
        break;
    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;
    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen(TD5_SCREEN_OPTIONS_HUB);
        }
        break;
    }
}

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

/* [PORT ENHANCEMENT 2026-06] Per-button remap row helpers. Both keyboard and
 * joystick players configure all 10 actions (LEFT/RIGHT/ACCELERATE/BRAKE +
 * HANDBRAKE..REAR VIEW). For joysticks each action maps to a button or an
 * axis/trigger direction (steer/accel/brake become analog when bound to axes). */
static int ctrl_bind_row_count(void)
{
    return TD5_JSBIND_ACTIONS;   /* 10 */
}

static const char *ctrl_bind_row_label(int row)
{
    return (row >= 0 && row < 10) ? k_ctrl_action_labels[row] : "?";
}

/* Begin capturing input for the currently-selected action. Capture is two-phase:
 * first we WAIT for the device to return to neutral (armed=0) so the confirm
 * press — or a previous bind's still-held key/stick — isn't re-captured; once
 * neutral we snapshot the rest state and arm (armed=1) to listen for ONE input.
 * Shared by the single-action remap and the REMAP ALL sequential pass.
 * [PORT 2026-06] "listen once, wait for release before the next" */
static void ctrl_begin_capture(void)
{
    s_ctrl_capturing     = 1;
    s_ctrl_capture_armed = 0;   /* wait for neutral; baseline snapshot happens then */
    if (s_ctrl_input_source != 0)
        td5_plat_input_joystick_neutral_reset();   /* fresh settle timer per action */
    TD5_LOG_I(LOG_TAG, "CtrlBind: capturing action %d (%s) player %d%s (waiting for neutral)",
              s_ctrl_sel_action, ctrl_bind_row_label(s_ctrl_sel_action),
              s_ctrl_player, s_ctrl_remap_all ? " [remap-all]" : "");
}

/* After a capture completes, advance the REMAP ALL sequence (or finish it). */
static void ctrl_capture_advance(void)
{
    s_ctrl_capturing = 0;
    if (s_ctrl_remap_all) {
        s_ctrl_sel_action++;
        if (s_ctrl_sel_action < TD5_JSBIND_ACTIONS) {
            s_selected_button = s_ctrl_sel_action;   /* keep the cursor on it */
            ctrl_begin_capture();
        } else {
            s_ctrl_remap_all = 0;
            s_ctrl_sel_action = 0;
        }
    }
}

static void Screen_ControllerBinding(void) {
    switch (s_inner_state) {

    /* ------------------------------------------------------------------
     * State 0: Init — detect the configured player's device type, seed the
     * binding state, build the per-action row buttons, → slide-in (state 9).
     * ------------------------------------------------------------------ */
    case 0:
        frontend_init_return_screen(TD5_SCREEN_CONTROLLER_BINDING);
        TD5_LOG_I(LOG_TAG, "ControllerBinding: init player=%d", s_ctrl_player);
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_joypad_icon_surface   = frontend_load_tga_ck("JoypadIcon.tga",   "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        s_joystick_icon_surface = frontend_load_tga_ck("JoystickIcon.tga", "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        s_keyboard_icon_surface = frontend_load_tga_ck("KeyboardIcon.tga", "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        s_nocontroller_surface  = frontend_load_tga_ck("NoControllerText.tga", "Front End/frontend.zip", TD5_COLORKEY_BLACK);
        {
            /* s_ctrl_player was set by Control Options (the player whose CONFIGURE
             * was pressed). Pick the row model from its device type. */
            int dev_type = td5_input_get_device_type(s_ctrl_player);
            int j;
            if (dev_type < 0 || dev_type > 2) dev_type = 0;   /* no controller → keyboard list */
            s_ctrl_input_source = dev_type;

            if (dev_type == 0) {
                /* Seed the scancode buffer from this player's saved keyboard set
                 * (players 0/1 have their own; 2+ share player-1's set). */
                const uint8_t *src = (const uint8_t *)((s_ctrl_player == 0)
                    ? td5_save_get_p1_custom_bindings_mutable()
                    : td5_save_get_p2_custom_bindings_mutable());
                memcpy(s_ctrl_kb_scancodes, src, 16);
                /* PAUSE (index 10) isn't in the legacy 10-key buffer; default it
                 * to P (0x19) for display unless an in-session rebind set it. */
                if (s_ctrl_kb_scancodes[10] == 0 || s_ctrl_kb_scancodes[10] > 0xED)
                    s_ctrl_kb_scancodes[10] = 0x19;
            } else {
                /* Seed the per-action joystick bindings from the saved set. */
                const uint32_t *ab = td5_save_get_action_bindings_mutable();
                if (ab)
                    memcpy(s_ctrl_action_bind[s_ctrl_player],
                           ab + (size_t)s_ctrl_player * TD5_JSBIND_ACTIONS,
                           TD5_JSBIND_ACTIONS * sizeof(uint32_t));
            }

            s_ctrl_sel_action = 0;
            s_ctrl_capturing  = 0;
            s_ctrl_remap_all  = 0;
            s_ctrl_capture_armed = 0;

            /* Build the action buttons in TWO narrow columns (is_selector → the
             * renderer skips their label; the overlay draws a small label+value
             * inside each). Layout (PORT ENHANCEMENT 2026-06):
             *   TOP    REMAP ALL (sequential one-by-one)
             *   GRID   10 driving actions (two columns of 5)
             *   BOTTOM "?" (= PAUSE mapping) | OK
             * Buttons are created actions-first so the handler's fixed indices
             * stay valid: 0..9 actions, 10 = "?"/PAUSE, 11 = REMAP ALL, 12 = OK. */
            {
                int colx[2] = { 130, 360 };  /* two aligned columns, shifted right off the
                                              * dark left edge of the background */
                int b;
                frontend_reset_buttons();
                for (j = 0; j < 10; j++) {                 /* 0..9 driving actions */
                    int c = j / 5, r = j % 5;
                    b = frontend_create_button("", colx[c], 132 + r * 32, 135, 26);
                    if (b >= 0) s_buttons[b].is_selector = 1;
                }
                b = frontend_create_button("", 130, 312, 150, 26);           /* 10 = PAUSE MENU (bottom-left) */
                if (b >= 0) s_buttons[b].is_selector = 1;
                b = frontend_create_button("REMAP ALL", 235, 90, 170, 28);   /* 11 = REMAP ALL (top, centered) */
                if (b >= 0) s_buttons[b].is_selector = 1;
                b = frontend_create_button(SNK_OkButTxt, 415, 312, 80, 26);  /* 12 = OK (bottom-right) */
                if (b >= 0) s_buttons[b].is_selector = 1;
            }
        }
        s_anim_tick = 0;
        s_anim_complete = 0;
        s_inner_state = 9;
        break;

    /* ------------------------------------------------------------------
     * State 9: Joystick slide-in animation
     * [CONFIRMED @ 0x40FE00] Counts g_frontendAnimFrameCounter to 0x1C,
     * then advances to state 10 and deactivates cursor overlay.
     * Port: uses s_anim_tick.
     * ------------------------------------------------------------------ */
    case 9:
        s_anim_tick++;
        if (s_anim_tick >= 0x1C) {
            s_anim_tick = 0;
            s_anim_complete = 1;
            s_inner_state = 10;
        }
        break;

    /* ------------------------------------------------------------------
     * State 10: interactive list + per-action capture (PORT ENHANCEMENT).
     * Browse the action rows (generic button nav / mouse), confirm one to
     * (re)bind ONLY that action — keyboard captures a scancode, joystick captures
     * a button OR an axis/trigger direction. OK saves + slides out (state 11).
     * ------------------------------------------------------------------ */
    case 10: {
        /* Two-column per-action remap. Browse the action buttons (0..10 incl the
         * PAUSE "?" at 10), confirm one to (re)bind just it; REMAP ALL (11) runs
         * the sequential one-by-one pass; OK (12) saves + exits. [PORT 2026-06] */
        int rows         = ctrl_bind_row_count();   /* 11 actions (0..10) */
        int remapall_btn = rows;                     /* 11 */
        int ok_btn       = rows + 1;                 /* 12 */

        if (!s_ctrl_capturing) {
            /* While idle, continuously learn this joystick's REST positions so the
             * wait-for-release gate can tell a held trigger from a released one. */
            if (s_ctrl_input_source != 0)
                td5_plat_input_joystick_learn_rest(s_ctrl_player);
            if (s_input_ready) {
                if (s_button_index == ok_btn) {
                    /* Save this player's bindings (joystick per-action codes +
                     * keyboard set) and return to Control Options. */
                    {
                        uint32_t *ab = td5_save_get_action_bindings_mutable();
                        if (ab)
                            memcpy(ab + (size_t)s_ctrl_player * TD5_JSBIND_ACTIONS,
                                   s_ctrl_action_bind[s_ctrl_player],
                                   TD5_JSBIND_ACTIONS * sizeof(uint32_t));
                        td5_input_set_action_bindings(s_ctrl_player,
                            s_ctrl_action_bind[s_ctrl_player], TD5_JSBIND_ACTIONS);
                    }
                    {
                        /* Keyboard set: players 0/1 own a set; 2+ share player-1's. */
                        int kb_set = (s_ctrl_player == 0) ? 0 : 1;
                        uint8_t *dst = (uint8_t *)((kb_set == 0)
                            ? td5_save_get_p1_custom_bindings_mutable()
                            : td5_save_get_p2_custom_bindings_mutable());
                        memcpy(dst, s_ctrl_kb_scancodes, 16);
                        td5_plat_input_set_keyboard_bindings(kb_set, s_ctrl_kb_scancodes,
                                                             TD5_JSBIND_ACTIONS);
                    }
                    td5_save_write_config(NULL);
                    TD5_LOG_I(LOG_TAG, "CtrlBind: saved bindings for player %d", s_ctrl_player);
                    s_anim_tick = 0;
                    s_inner_state = 11;
                } else if (s_button_index == remapall_btn) {
                    /* REMAP ALL: configure every action one by one, from action 0. */
                    s_ctrl_remap_all  = 1;
                    s_ctrl_sel_action = 0;
                    s_selected_button = 0;
                    ctrl_begin_capture();
                    frontend_play_sfx(2);
                } else if (s_button_index >= 0 && s_button_index < rows) {
                    /* Begin capturing just the selected action. */
                    s_ctrl_remap_all  = 0;
                    s_ctrl_sel_action = s_button_index;
                    ctrl_begin_capture();
                    frontend_play_sfx(2);
                }
            }
        } else {
            /* --- Capture mode (ESC cancels; in REMAP ALL it cancels the run) --- */
            const uint8_t *kb = td5_plat_input_get_keyboard();
            if (kb[0x01]) {
                /* ESC always cancels (checked before the neutral gate so a held
                 * ESC can't wedge the wait). */
                s_ctrl_capturing = 0;
                s_ctrl_remap_all = 0;
                s_ctrl_capture_armed = 0;
                TD5_LOG_I(LOG_TAG, "CtrlBind: capture cancelled");
                break;
            }

            /* Phase 1: wait for the device to go neutral, THEN snapshot + arm.
             * This makes the remap "listen once" — a held stick/key from the
             * confirm press (or the previous bind) must be released before the
             * next input is captured. [PORT 2026-06] */
            if (!s_ctrl_capture_armed) {
                int neutral;
                if (s_ctrl_input_source == 0) {
                    int sc; neutral = 1;
                    for (sc = 1; sc < 256; sc++) if (kb[sc]) { neutral = 0; break; }
                } else {
                    neutral = td5_plat_input_joystick_neutral(s_ctrl_player);
                }
                if (neutral) {
                    s_ctrl_capture_armed = 1;
                    memcpy(s_ctrl_capture_kb_snapshot, kb, 256);
                    if (s_ctrl_input_source != 0)
                        td5_plat_input_joystick_capture_begin(s_ctrl_player);
                    TD5_LOG_D(LOG_TAG, "CtrlBind: armed for action %d", s_ctrl_sel_action);
                }
                break;   /* still releasing / just armed — don't capture this frame */
            }

            if (s_ctrl_input_source == 0) {
                /* Keyboard: first freshly-pressed key (rising edge vs snapshot). */
                int sc, found = -1;
                for (sc = 1; sc < 256 && found < 0; sc++)
                    if (kb[sc] && !s_ctrl_capture_kb_snapshot[sc] && sc != 0x01)
                        found = sc;
                if (found >= 0) {
                    if (s_ctrl_sel_action >= 0 && s_ctrl_sel_action < TD5_JSBIND_ACTIONS)
                        s_ctrl_kb_scancodes[s_ctrl_sel_action] = (uint8_t)found;
                    TD5_LOG_I(LOG_TAG, "CtrlBind: action %d -> scancode 0x%02X",
                              s_ctrl_sel_action, (unsigned)found);
                    frontend_play_sfx(3);
                    ctrl_capture_advance();
                }
            } else {
                /* Joystick: first fresh button press OR axis/trigger movement. */
                uint32_t code = 0;
                if (td5_plat_input_joystick_capture_poll(s_ctrl_player, &code)) {
                    if (s_ctrl_sel_action >= 0 && s_ctrl_sel_action < TD5_JSBIND_ACTIONS)
                        s_ctrl_action_bind[s_ctrl_player][s_ctrl_sel_action] = code;
                    TD5_LOG_I(LOG_TAG, "CtrlBind: action %d -> joystick code 0x%X",
                              s_ctrl_sel_action, (unsigned)code);
                    frontend_play_sfx(2);
                    ctrl_capture_advance();
                }
            }
        }
        break;
    }

    /* ------------------------------------------------------------------
     * State 11: Joystick slide-out animation (0x1C frames)
     * [CONFIRMED @ 0x40FE00] After animation, releases surfaces and
     * calls SetFrontendScreen(0xe) = TD5_SCREEN_CONTROL_OPTIONS.
     * ------------------------------------------------------------------ */
    case 11:
        s_anim_tick++;
        if (s_anim_tick >= 0x1C) {
            s_anim_tick = 0;
            TD5_LOG_D(LOG_TAG, "CtrlBind: joystick slide-out done → ControlOptions");
            td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        }
        break;

    /* [PORT ENHANCEMENT 2026-06] The original's sequential keyboard-capture
     * states (19/20/25/26/27) and joystick-cycle state are gone — the unified
     * per-button-remap list in state 10 handles both device types. */
    default:
        td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        break;
    }
}

/* ========================================================================
 * [19] ScreenMusicTestExtras (0x418460) -- CD audio jukebox
 * States: 9
 * ======================================================================== */

static void Screen_MusicTestExtras(void) {
    switch (s_inner_state) {
    case 0: /* Fade transition + init */
        frontend_init_return_screen(TD5_SCREEN_MUSIC_TEST);
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: init");
        /* [CONFIRMED @ 0x418460 case 0]:
         *   ReleaseExtrasGalleryImageSurfaces() + LoadExtrasBandGalleryImages()
         *   CreateMenuStringLabelSurface(6) → DAT_00496358 (title surface)
         *   CreateTrackedFrontendSurface(0x170,0x28) → DAT_0049628c (track-name)
         *   CreateTrackedFrontendSurface(0x170,0x78) → DAT_00496400 (now-playing)
         *   Initial draw: sprintf "%d. %s" + NowPlayingTxt + band + title into surfaces
         * Port: no offscreen surfaces — strings are rendered live every frame via
         * frontend_render_music_test_overlay. Initialise them here.
         * Tier 4 port 2026-05-24 added the [ARCH-DIVERGENCE] footer entries
         * for ReleaseExtrasGalleryImageSurfaces / LoadExtrasBandGalleryImages /
         * CreateMenuStringLabelSurface — see end of file. */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* [CONFIRMED @ 0x418460] CreateFrontendDisplayModeButton(SNK_SelectTrackButTxt, -0x120, 0, 0xA0, 0x20, 0)
         *                          CreateFrontendDisplayModeButton(SNK_OkButTxt, -0x120, 0, 0x60, 0x20, 0) */
        /* SNK_SelectTrackButTxt = "TRACK" (Language.dll, byte-faithful — port's prior
         * "Select Track" was a wrong guess). Buttons placed at their EXACT settled rest
         * positions (the orig auto-creates then MoveFrontendSpriteRects them; slide-in
         * settles at counter 0x27): [CONFIRMED @0x418460] TRACK (120,97) 160x32 top-left;
         * OK (216,377) 96x32 at the BOTTOM (below the 160x160 cover), NOT auto-stacked. */
        frontend_create_button(SNK_SelectTrackButTxt, 120, 97,  0xA0, 0x20);
        frontend_create_button(SNK_OkButTxt,    216, 377, 0x60, 0x20);
        /* Load the 5 band cover-art images (Extras.zip -> re/assets/extras).
         * Idempotent (frontend_load_tga returns the existing handle if reloaded). */
        {
            static const char *covers[5] = {
                "Fear Factory.tga", "Gravity Kills.tga", "Junkie XL.tga",
                "KMFDM.tga", "PitchShifter.tga"
            };
            int ci;
            for (ci = 0; ci < 5; ci++)
                s_band_cover_surface[ci] =
                    frontend_load_tga(covers[ci], "Front End/Extras/Extras.zip");
        }
        s_music_test_track_idx = 0;
        s_music_attract_track = 0;   /* cover/now-playing reflect the PLAYED track */
        s_music_test_playing_set = 0;
        s_music_test_now_band[0]  = '\0';
        s_music_test_now_title[0] = '\0';
        frontend_music_test_update_track_label();   /* "1. GRAVITY KILLS" */
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: track_label='%s'", s_music_test_track_label);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: cycle tracks, play, OK */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            /* Use the same selected-button fallback as every other option screen
             * (e.g. Game Options :7662). s_button_index is the CLICKED button and is
             * -1 when the user only presses LEFT/RIGHT arrow keys — so gating the
             * cycle on `s_button_index == 0` meant keyboard arrows never changed the
             * track (the reported bug). active_button resolves to the highlighted
             * selector (button 0) for keyboard arrow input. */
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (active_button == 0 && delta != 0) {
                /* Cycle track index 0..11.
                 * [CONFIRMED @ 0x4186A8]: g_selectedCdTrackIndex += DAT_0049b690 (arrow dir),
                 *   clamped 0..11; then BltColorFillToSurface + sprintf "%d. %s" redrawn
                 *   into DAT_0049628c (track-name surface). Port: update label string. */
                s_music_test_track_idx += delta;
                /* [CONFIRMED @ 0x00418460 case6/idx0, re-decompiled 2026-05-30 by two
                 * independent agents] original WRAPS 0<->0xB: LEFT underflow -> 0xB,
                 * RIGHT overflow -> 0. It does NOT clamp. The prior comment here (citing
                 * a 0x4186A8 clamp) was incorrect. Table length 12 (PTR_s_GRAVITY_KILLS /
                 * PTR_s_FALLING) matches the 0..11 wrap range. */
                if (s_music_test_track_idx < 0)  s_music_test_track_idx = 11;
                if (s_music_test_track_idx > 11) s_music_test_track_idx = 0;
                frontend_music_test_update_track_label();
                TD5_LOG_D(LOG_TAG, "MusicTestExtras: cycle -> '%s'", s_music_test_track_label);
            }
            if (s_button_index == 0 && s_arrow_input == 0) {
                /* Confirm "Select Track" -> play CD audio.
                 * [CONFIRMED @ 0x41864E]: DXSound::CDPlay(g_selectedCdTrackIndex+2, 1)
                 *   then redraw DAT_00496400 (now-playing surface):
                 *   row y=0:    "NOW PLAYING" text (SNK_NowPlayingTxt_exref)
                 *   row y=0x28: band name  (PTR_s_GRAVITY_KILLS_00465e1c[idx])
                 *   row y=0x50: song title (PTR_s_FALLING_00465e58[idx])
                 * Port: record now-playing strings; render overlay draws them live. */
                frontend_cd_play(s_music_test_track_idx);
                frontend_music_test_update_now_playing(s_music_test_track_idx);
                /* [FIXED 2026-06-01] orig sets g_attractCdTrackCandidate here on SELECT;
                 * the cover art + now-playing panel follow the PLAYED track, not the
                 * one being previewed with ◄►. */
                s_music_attract_track = s_music_test_track_idx;
                TD5_LOG_D(LOG_TAG, "MusicTestExtras: now playing '%s' / '%s'",
                          s_music_test_now_band, s_music_test_now_title);
            }
            if (s_button_index == 1) { /* OK */
                /* Set fade value for transition, exit to Sound Options */
                s_inner_state = 7;
            }
        }
        break;

    case 7: /* Prep slide-out */
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms). Restore gallery images. */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* [CONFIRMED @ 0x00418bd2..0x00418bdc]:
             *   ReleaseExtrasGalleryImageSurfaces(); LoadExtrasGalleryImageSurfaces();
             * Port has no band-photo surface pool (case 0 documents the fold), so
             * release/reload is a no-op. The mugshot gallery (Screen_ExtrasGallery
             * at TD5_SCREEN_EXTRAS_GALLERY) maintains its own s_gallery_pic_surface
             * independently and is not coupled to this screen's transition. */
            td5_frontend_set_screen(TD5_SCREEN_SOUND_OPTIONS);
        }
        break;
    }
}

/* ========================================================================
 * [20] CarSelectionScreenStateMachine (0x40DFC0)
 * States: 27 (0x00 - 0x1A)
 *
 * Handles 1P, 2P sequential, and network car selection.
 * ======================================================================== */

static void Screen_CarSelection(void) {
    switch (s_inner_state) {
    case 0: /* Init: determine car roster, load UI assets */
        frontend_init_return_screen(TD5_SCREEN_CAR_SELECTION);
        TD5_LOG_D(LOG_TAG, "CarSelection: state 0 - init");
        s_anim_complete = 0;

        /* Reload MainMenu.tga background so that returning from TrackSelection
         * (which loads TrackSelect.tga) restores the correct background.
         * Original relies on preserved primary surface from RaceTypeCategory
         * (0x4168B0 loads MainMenu.tga at 0x00416940), but our clear-per-frame
         * renderer needs the background surface explicitly set.
         * [CONFIRMED @ Ghidra 0x00416940: RaceTypeCategory loads MainMenu.tga] */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Load overlay UI assets.
         * All three overlays use opaque blit (flag 0x10) in the original binary
         * [CONFIRMED @ Ghidra 0x0040E1CD–0x0040E20F: all loaded via FUN_00412030].
         * CarSelCurve and CarSelTopBar have black areas that are intentionally
         * opaque — they cover the background, not show through it. */
        s_carsel_bar_surface    = frontend_load_tga("Front_End/CarSelBar1.tga",   "Front_End/FrontEnd.zip");
        s_carsel_curve_surface  = frontend_load_tga("Front_End/CarSelCurve.tga",  "Front_End/FrontEnd.zip");
        s_carsel_topbar_surface = frontend_load_tga("Front_End/CarSelTopBar.tga", "Front_End/FrontEnd.zip");
        s_graphbars_surface     = frontend_load_tga("Front_End/GraphBars.tga",    "Front_End/FrontEnd.zip");

        /* Create 1x1 solid blue fill surface for the car preview background.
         * Original uses FillPrimaryFrontendRect(0x5c, ...) which fills the
         * DDraw primary surface directly. Our renderer clears each frame, so
         * we create a tiny fill texture drawn via fe_draw_surface_opaque —
         * the same proven path used by the overlay TGAs above. */
        {
            int slot = -1;
            for (int i = 0; i < FE_MAX_SURFACES; i++) {
                if (!s_surfaces[i].in_use) { slot = i; break; }
            }
            if (slot >= 0) {
                int page = FE_SURFACE_PAGE_BASE + slot;
                /* BGRA pixel: B=0x5C(92), G=0, R=0, A=0xFF */
                uint32_t blue_pixel = 0xFF00005C;
                if (td5_plat_render_upload_texture(page, &blue_pixel, 1, 1, 2)) {
                    s_surfaces[slot].in_use = 1;
                    s_surfaces[slot].tex_page = page;
                    s_surfaces[slot].width = 1;
                    s_surfaces[slot].height = 1;
                    strncpy(s_surfaces[slot].source_name, "_fill_blue", sizeof(s_surfaces[slot].source_name) - 1);
                    s_carsel_fill_surface = slot + 1;
                    TD5_LOG_I(LOG_TAG, "CarSel: blue fill surface created: slot=%d page=%d", slot, page);
                }
            }
        }

        /* Show the mouse cursor on car-select (it's a mouse-interactive screen,
         * incl. the TD6 color map). Normal flow inherits a visible cursor from the
         * prior screen, but a direct StartScreen jump (test harness) does not, so
         * set it explicitly here. */
        frontend_set_cursor_visible(1);

        /* Determine car roster range by game type */
        s_car_roster_min = 0;
        switch (s_selected_game_type) {
        case 2: /* Era: 0..15 */
            s_car_roster_max = 15;
            break;
        case 5: /* Masters: use random roster */
            s_car_roster_max = 14; /* index into s_masters_roster[] */
            break;
        case 8: /* Cop Chase: TD5 police 33-36 + TD6 cops cp1-4 (46-49).
                 * frontend_car_cycle_step skips the non-cop gap (37-45). */
            s_car_roster_min = 33;
            s_car_roster_max = TD6_COP_LAST;   /* 49 */
            break;
        default:
            /* [CONFIRMED @ 0x0040E8F8 CarSelectionScreenStateMachine case 7]
             * Original wrap-around in non-COPS mode is gated on
             * DAT_004962ac/DAT_00463e6d: when DAT_004962ac == 0 (the common
             * case — no runtime writer in the decomp) the upper bound is
             * 0x20 (32), excluding police indices 0x21..0x24. Police are
             * only reachable when DAT_004962ac is set (Cop Chase / network
             * special) or when game_type == 8 (handled above).
             *
             * Port: cap the default roster at 32 so police indices stay out
             * of normal cycling regardless of unlock state. The cheat
             * "unlock all" path still extends to 36 only when paired with
             * the network-special context (matches the original's "both
             * flags must be set" gate at 0x0042140B). */
            /* TD6 cars (37-75) are always selectable, so the cycle range runs to
             * the full roster. frontend_car_cycle_step skips the locked-TD5 /
             * police gap (cap+1 .. 36), so the visible original-car set is
             * unchanged while the TD6 cars become reachable. */
            s_car_roster_max = TD5_CAR_COUNT - 1;
            break;
        }

        /* Handle 2P mode:
         * (two_player_mode & 3) == 1: P1 selecting
         * (two_player_mode & 3) == 2: P2 selecting */
        if ((s_two_player_mode & 3) == 2) {
            /* Create P2 label */
        }

        /* [PORT ENHANCEMENT 2026-06] Multiplayer flow: start each player at their
         * own prior pick (so Back/forward keeps each player's car). */
        if (s_mp_flow && s_mp_car_player >= 0 && s_mp_car_player < TD5_MAX_HUMAN_PLAYERS)
            s_selected_car = s_mp_player_car[s_mp_car_player];

        /* Clamp initial car to valid range; if it landed in the locked/police
         * gap (e.g. a stale default_car), step to the next selectable car. */
        if (s_selected_car < s_car_roster_min) s_selected_car = s_car_roster_min;
        if (s_selected_car > s_car_roster_max) s_selected_car = s_car_roster_max;
        if (s_car_roster_max >= TD5_BASE_CAR_COUNT && !frontend_car_selectable(s_selected_car))
            s_selected_car = frontend_car_cycle_step(s_selected_car, 1,
                                                     s_car_roster_min, s_car_roster_max);

        /* Create label surface */
        s_car_preview_overlay = 0;
        frontend_load_selected_car_preview();
        s_inner_state = 1;
        break;

    case 1: /* Reset tick counter */
        frontend_begin_timed_animation();
        s_inner_state = 2;
        break;

    case 2: /* Sidebar slide-in: bar slides from right, curve+topbar from left */
        /* Skip if returning from network car select, 2P round 2, or drag-race 2nd pass */
        if ((s_two_player_mode & 4) != 0 || s_network_active ||
            s_drag_carselect_pass != 0) {
            s_inner_state = 3;
        } else if (frontend_update_timed_animation(75, FE_CARSEL_SLIDE_IN_MS) >= 1.0f) {
            s_inner_state = 3;
        }
        break;

    case 3: /* Present + copy primary to secondary */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Button creation: 6 buttons along the left column.
             * Original layout places the button column on the left side and the
             * 408x280 car preview on the right side. */
        /* 5 buttons per original (0x40DFC0 state 4): exact final positions from Ghidra
         * Tab buttons: x=46, y=169/209/249/289, w=168, h=32
         * OK: x=46, y=329, w=64  |  BACK: x=118, y=329, w=96 */
        /* Drag race FORCES Manual transmission and renders the toggle
         * non-interactive [CONFIRMED @ 0x0040e119 cmp gameType!=7(orig drag) /
         * 0x0040e167 write g_carSelectManualTransmissionToggle = (gameType==7)].
         * Port game_type 9 == drag. Force the value here so the button shows
         * "Manual"; case 3 below refuses to toggle it back. */
        if (g_td5.drag_race_enabled)
            s_selected_transmission = 1;
        frontend_create_button(SNK_CarButTxt,   46, 169, 168, 32);
        frontend_create_button(SNK_PaintButTxt, 46, 209, 168, 32);
        frontend_create_button(SNK_ConfigButTxt, 46, 249, 168, 32);
        frontend_create_button(s_selected_transmission ? "Manual" : "Automatic",
                               46, 289, 168, 32);
        frontend_create_button(SNK_OkButTxt,   46, 329,  64, 32);
        if (!s_network_active)
            frontend_create_button(SNK_BackButTxt, 118, 329, 96, 32);

        /* Time Trials: grey out Manual button */
        /* Load inline string table SNK_CarSelect_MT1 */
        s_anim_complete = 0;
        frontend_begin_timed_animation();
        s_inner_state = 5;
        break;

    case 5: /* Button slide-in: 24 frames (original 0x18) */
        if (frontend_update_timed_animation(24, 400) >= 1.0f) {
            s_inner_state = 6;
        }
        break;

    case 6: /* Tick until ready */
        if (frontend_advance_tick()) {
            s_anim_complete = 1;
            frontend_play_sfx(4); /* screen-entry slide-in settle chime — the
                                   * original CarSelection plays Play(4) on its
                                   * slide-in settle [CONFIRMED @ 0x40DFC0]. The
                                   * port only chimed on the per-CAR preview
                                   * slide-in (case 14), so ENTERING the car-select
                                   * screen had no chime. */
            s_inner_state = 7;
        }
        break;

    case 7: /* Main interaction loop: car preview + input */
        {
            /* DEV harness: TD5RE_DEMO_CARSTATS=1 jumps straight to the STATS spec sheet so it
             * can be frame-dumped (frontend nav uses DirectInput polling, which SendKeys can't
             * drive). Inert unless the env var is set. */
            static int s_demo_cs_init = 0, s_demo_cs = 0;
            if (!s_demo_cs_init) { s_demo_cs_init = 1;
                const char *e = getenv("TD5RE_DEMO_CARSTATS"); s_demo_cs = (e && e[0] && e[0] != '0'); }
            if (s_demo_cs) {
                frontend_load_car_spec_fields(frontend_current_car_index());
                s_car_preview_overlay = 1;
                s_inner_state = 15;
                return;
            }
        }
        /* Render car preview overlay */
        /* Car-select also enters on a bare mouse CLICK while the colour panel is
         * open, so swatch/map clicks reach the panel — a click doesn't set
         * s_input_ready (only keyboard/button input does). */
        if (s_input_ready || (s_color_panel_visible && s_mouse_clicked)) {
            int delta = frontend_option_delta();
            int active_button = s_button_index;
            /* [FIXED 2026-06-01] include button 2 (Stats) — orig slot2 is also a ◄►
             * cycler (wheel/config scheme), so keyboard arrows over it must resolve. */
            if (active_button < 0 && delta != 0 &&
                (s_selected_button == 0 || s_selected_button == 1 ||
                 s_selected_button == 2 || s_selected_button == 3)) {
                active_button = s_selected_button;
            }
            /* Keep Stats/Auto/OK/Back positioned for the current panel state every
             * frame (idempotent; survives button recreation on car change). */
            frontend_apply_color_panel_layout();

            /* TD6 color panel is MODAL while open: all 4 arrows move a 2D cursor
             * over the picker (rows 0-1 = predefined swatches, rows 2+ = the color
             * map) and the color under it is applied live; the mouse sets the
             * cursor by clicking a swatch / map cell. A button PRESS exits the
             * picker: PAINT just closes; any other button closes AND acts. There's
             * no OK/close inside the panel — re-press PAINT (or click it) to hide. */
            if (s_color_panel_visible) {
                int moved = 0, confirm = 0;
                if (s_arrow_input & 1) { s_color_cur_col = (s_color_cur_col + TD6_CP_COLS - 1) % TD6_CP_COLS; moved = 1; }
                if (s_arrow_input & 2) { s_color_cur_col = (s_color_cur_col + 1) % TD6_CP_COLS; moved = 1; }
                if (s_arrow_input & 4) {         /* UP: move up, or exit the picker from the top row */
                    if (s_color_cur_row > 0) { s_color_cur_row--; moved = 1; }
                    else confirm = 1;
                }
                if (s_arrow_input & 8) { if (s_color_cur_row < TD6_CP_GRID_ROWS - 1) { s_color_cur_row++; moved = 1; } }
                if (frontend_color_panel_mouse()) moved = 1;
                if (moved) {
                    /* Live preview only: no animation while navigating — the
                     * body-only overlay shows the colour instantly (modulate). */
                    g_td5.ini.td6_paint_color = (int)td6_cursor_color(s_color_cur_col, s_color_cur_row);
                    frontend_play_sfx(2);
                }
                if (s_button_index == 1) confirm = 1;   /* PAINT re-pressed -> confirm */
                if (confirm) {                   /* commit the colour: it stays on the
                                                  * preview (no animation on hide — the
                                                  * colour already changed live). */
                    s_paint_active = 1;
                    frontend_set_color_panel(0);
                    td5_ini_persist_options();
                    frontend_play_sfx(3);
                    active_button = -1;
                } else if (s_button_index >= 0) { /* other button -> keep colour, close + act */
                    s_paint_active = 1;
                    frontend_set_color_panel(0);
                    td5_ini_persist_options();
                    active_button = s_button_index;
                } else {
                    active_button = -1;          /* arrows are modal: no button action */
                }
            }
            if (active_button >= 0) switch (active_button) {
            case 0: /* Car: L/R arrows cycle car index */
                if (delta != 0) {
                    if (s_selected_game_type == 5) {
                        /* Masters: cycle through roster, skip AI slots */
                        int attempts = 0;
                        do {
                            s_selected_car += delta;
                            if (s_selected_car < 0) s_selected_car = 14;
                            if (s_selected_car > 14) s_selected_car = 0;
                            attempts++;
                        } while (s_masters_roster_flags[s_selected_car] == 1 && attempts < 15);
                    } else {
                        /* Default/era/cop ranges. For the full single-race roster
                         * this skips the locked-TD5/police gap so TD6 cars (37-75)
                         * are reachable; narrower ranges wrap plainly. */
                        s_selected_car = frontend_car_cycle_step(s_selected_car, delta,
                                                                 s_car_roster_min, s_car_roster_max);
                    }
                    /* [FIXED 2026-06-01] orig (0x40E8xx) resets paint + wheel/config
                     * scheme to 0 on EVERY car change. The color panel stays open
                     * across TD6->TD6 changes; it auto-hides after the switch if the
                     * new car is a TD5 car (see the post-switch check). */
                    s_selected_paint = 0;
                    s_selected_config = 0;
                    /* s_paint_active is NOT cleared on car change: the chosen
                     * paint colour carries over to the next car (and survives a
                     * race) — it's a single remembered colour for all TD6 cars. */
                    s_inner_state = 10; /* trigger new car image load */
                }
                break;

            case 1: /* Paint (TD5: ◄► cycle paint 0-3) / Color (TD6: open panel) */
                if (frontend_car_paintable(frontend_current_car_index())) {
                    /* Pressing PAINT opens the modal color panel. (Closing is
                     * handled by the modal block above when it's already open, so
                     * this branch only runs while the panel is closed.) */
                    if (s_button_index == 1) {
                        frontend_set_color_panel(1);
                        frontend_play_sfx(3);
                    }
                } else if (delta != 0) {
                    /* TD5: cycle paint 0-3 (disabled for cop cars 0x1C-0x24). */
                    int actual_car = (s_selected_game_type == 5) ?
                                     s_masters_roster[s_selected_car] : s_selected_car;
                    if (actual_car < 0x1C || actual_car > 0x24) {
                        s_selected_paint += delta;
                        if (s_selected_paint < 0) s_selected_paint = 3;
                        if (s_selected_paint > 3) s_selected_paint = 0;
                        s_inner_state = 10; /* re-render */
                    }
                }
                break;

            case 2: /* Stats: ◄► cycles wheel/config scheme 0..3; press opens spec sheet.
                     * [CONFIRMED @ 0x40DFC0 case 7 g_frontendButtonIndex==2: arrow cycles
                     * g_carSelectWheelSchemeTransient 0..3 wrap; press enters state 0xf.] */
                if (delta != 0) {
                    s_selected_config += delta;
                    if (s_selected_config < 0) s_selected_config = 3;
                    if (s_selected_config > 3) s_selected_config = 0;
                    s_inner_state = 10; /* re-render preview with new scheme */
                } else if (s_button_index == 2) {
                    s_car_preview_overlay = 1;
                    s_inner_state = 15;
                }
                break;

            case 3: /* Auto/Manual toggle */
                /* Drag race locks the transmission to Manual [CONFIRMED @
                 * 0x0040e167 — orig makes the button a non-interactive Preview
                 * for game_type 7 (=drag there); port drag == game_type 9].
                 * The pre-existing `!= 7` guard keeps the port's Time-Trial
                 * behavior unchanged; the added drag guard is the faithful fix. */
                if (!g_td5.drag_race_enabled &&
                    s_selected_game_type != 7 && (s_button_index >= 0 || delta != 0)) {
                    s_selected_transmission = !s_selected_transmission;
                    strncpy(s_buttons[3].label,
                            s_selected_transmission ? "Manual" : "Automatic",
                            sizeof(s_buttons[3].label) - 1);
                    s_buttons[3].label[sizeof(s_buttons[3].label) - 1] = '\0';
                }
                break;

            case 4: /* OK: accept car */
            {
                int actual_car = (s_selected_game_type == 5) ?
                                 s_masters_roster[s_selected_car] : s_selected_car;
                /* Lock enforcement */
                int locked = (actual_car >= 0 && actual_car < 37 &&
                             s_car_lock_table[actual_car] != 0);
                if (locked && !s_network_active &&
                    s_selected_game_type != 8 && s_selected_game_type != 5) {
                    frontend_play_sfx(10); /* rejection */
                } else {
                    /* Accept selection → forward to track selection */
                    if (s_selected_game_type == 5) {
                        s_masters_roster_flags[s_selected_car] = 2; /* taken */
                    }
                    s_return_screen = TD5_SCREEN_TRACK_SELECTION;
                    s_inner_state = 0x14; /* slide-out prep */
                }
            }
                break;

            case 5: /* Back */
                s_drag_carselect_pass = 0;
                s_return_screen = TD5_SCREEN_RACE_TYPE_MENU;
                s_inner_state = 0x14;
                break;
            }

            /* Auto-hide the color panel + restore PAINT arrows when the selection
             * is no longer a TD6 car (e.g. cycled back to a TD5 car while open). */
            if (s_color_panel_visible && !frontend_car_paintable(frontend_current_car_index()))
                frontend_set_color_panel(0);
        }

        /* Network: process messages, check disconnect */
        if (s_network_active) {
            if (s_kicked_flag) {
                s_network_active = 0;
                frontend_net_destroy();
                s_return_screen = -1;
                s_inner_state = 0x14;
            }
        }
        break;

    case 8: /* Blit cached rect, wait 2 frames then return to 7 */
        s_anim_tick += 2;
        if (s_anim_tick >= 2) {
            s_inner_state = 7;
        }
        break;

    case 10: /* Clear car preview area, prep for new image load */
        s_car_spec_car = -1; /* invalidate spec cache on car/paint change */
        s_car_preview_change_loaded = 0; /* reset state-11 load-once guard */
        s_anim_complete = 1;
        frontend_play_sfx(5); /* car-change animation START whoosh — fires once when
                               * a car/paint/scheme change begins (case 10 is reached
                               * ONLY from the cycle cases, never the initial load).
                               * The original plays Play(5) on the change
                               * [CONFIRMED @ 0x40DFC0]; it pairs with the Play(4)
                               * settle chime at case 14 when the new preview finishes
                               * sliding in. The port had only the finish chime. */
        s_inner_state = 11;
        break;

    case 11: /* Old car slides out to the right (~433ms, 13 frames @30fps) — 0x40DFC0 state 11 */
    {
        int actual_car = frontend_current_car_index();
        if (!s_car_preview_change_loaded) {
            /* Load the new preview exactly once on entering the slide. A car with
             * no carpic (e.g. a ported TD6 car) returns <=0; guarding on the
             * surface value would re-load + reset the timer every frame and hang
             * the transition, so use a dedicated load-once flag instead. */
            s_car_preview_change_loaded = 1;
            s_car_preview_prev_surface = s_car_preview_surface;
            s_car_preview_next_surface = frontend_load_car_preview_surface(actual_car, s_selected_paint);
            frontend_begin_timed_animation();
        }
        if (frontend_update_timed_animation(13, 433) >= 1.0f) {
            if (s_car_preview_prev_surface > 0 && s_car_preview_prev_surface != s_car_preview_next_surface) {
                frontend_release_surface(s_car_preview_prev_surface);
            }
            s_car_preview_surface = s_car_preview_next_surface;
            s_car_preview_prev_surface = 0;
            s_car_preview_next_surface = 0;
            s_inner_state = 12;
        }
    }
        break;

    case 12: /* Reset timer so state 14 slide-in starts from t=0 */
        frontend_begin_timed_animation();
        s_inner_state = 14;
        break;

    case 14: /* Car preview slide-in from right, 25 frames (~833ms @30fps) — 0x40DFC0 state 14 */
        if (frontend_update_timed_animation(0x19, 833) >= 1.0f) {
            /* Original 0x40DFC0 case 0xE @ 0x0040EE3F plays Play(4) once when
             * frame_counter == 0x19 (slide-in complete) — fires every car cycle. */
            frontend_play_sfx(4);
            s_inner_state = 7; /* return to interaction */
        }
        break;

    case 15: /* Stats sub-screen (0x40DFC0 state 0xF): SNK_Config_Hdrs + config.nfo values */
        frontend_load_car_spec_fields(frontend_current_car_index());
        if (s_input_ready && s_button_index >= 0) {
            s_car_preview_overlay = 2;
            s_inner_state = 7;
        }
        break;

    case 17: /* Info sub-screen [0x40DFC0 state 0x11]: draws 10 strings from
              * SNK_Info_Values_exref (Language.dll export, 10 × char* pointers,
              * centered via MeasureOrCenterFrontendString [CONFIRMED @ 0x0040F184]).
              * Language.dll is unavailable in the port; fall through to return. */
        s_inner_state = 18;
        break;

    case 18: /* Return from info */
        s_car_preview_overlay = 2;
        s_inner_state = 7;
        break;

    case 0x14: /* Prep slide-out */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 0x15;
        break;

    case 0x15: /* Cross-fade (~133ms) */
        if (frontend_update_timed_animation(8, 133) >= 1.0f) {
            s_inner_state = 0x16;
        }
        break;

    case 0x16: /* Release car surface */
    case 0x17:
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 0x18;
        break;

    case 0x18: /* Button slide-out (~400ms) */
        if (frontend_update_timed_animation(0x18, 400) >= 1.0f) {
            frontend_begin_timed_animation();
            s_inner_state = 0x19;
        }
        break;

    case 0x19: /* Screen wipe: vertical bar sweep (~267ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            s_inner_state = 0x1A;
        }
        break;

    case 0x1A: /* Exit dispatch: release all, determine next screen */
    {
        int actual_car = (s_selected_game_type == 5) ?
                         s_masters_roster[s_selected_car] : s_selected_car;

        /* [PORT ENHANCEMENT 2026-06] Multiplayer lobby flow: sequential per-player
         * car select. Each player picks in turn (join order); after the last,
         * proceed to the track selector. OK advances; Back steps to the previous
         * player (or back to the lobby from player 0). */
        if (s_mp_flow) {
            if (s_return_screen == TD5_SCREEN_TRACK_SELECTION) {   /* OK */
                if (s_mp_car_player >= 0 && s_mp_car_player < TD5_MAX_HUMAN_PLAYERS) {
                    s_mp_player_car[s_mp_car_player]   = actual_car;
                    s_mp_player_paint[s_mp_car_player] = s_selected_paint;
                }
                TD5_LOG_I(LOG_TAG, "CarSelect MP: player %d car=%d", s_mp_car_player + 1, actual_car);
                s_mp_car_player++;
                if (s_mp_car_player < s_num_human_players) {
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);   /* next player */
                    return;
                }
                s_selected_car   = s_mp_player_car[0];   /* slot 0 = player 1's car */
                s_selected_paint = s_mp_player_paint[0];
                td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                return;
            } else {   /* Back */
                if (s_mp_car_player > 0) {
                    s_mp_car_player--;
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                    return;
                }
                s_mp_flow = 0;
                td5_frontend_set_screen(TD5_SCREEN_MP_LOBBY);
                return;
            }
        }

        /* Two-player flow dispatch [CONFIRMED @ 0x0040DFC0 case 0x1A].
         * Original gates the 2P advance on g_returnToScreenIndex == -1 (OK
         * was pressed); Back has its own arm at lines 880-887 of the archived
         * decomp. The port mirrors this by gating on s_return_screen ==
         * TRACK_SELECTION (case 4 OK sets that; case 5 Back sets
         * RACE_TYPE_MENU). Without the gate, Back from P1 falls into the
         * P1→P2 branch and re-enters CarSelection — exactly the user-reported
         * "back moves forward" loop. */
        if (s_two_player_mode != 0) {
            if (s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
                /* OK */
                if ((s_two_player_mode & 3) == 1) {
                    /* P1 selected: save P1 choices, re-enter for P2 */
                    s_selected_car = actual_car; /* finalize */
                    s_two_player_mode = 6; /* set bit flags for P2 round */
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P1 OK car=%d → re-enter for P2", actual_car);
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                    return;
                }
                if ((s_two_player_mode & 3) == 2) {
                    /* P2 selected: save P2 choices, proceed to track selection */
                    s_p2_car = actual_car;
                    s_p2_paint = s_selected_paint;
                    s_p2_config = s_selected_config;
                    s_p2_transmission = s_selected_transmission;
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P2 OK car=%d → TrackSelection", actual_car);
                    td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
                    return;
                }
            } else {
                /* Back [CONFIRMED @ 0x0040DFC0 lines 880-887].
                 * mode==1 (P1) → MainMenu. mode!=1 (P2 with mode=6) →
                 * sentinel mode=5, re-enter CarSelection so user can
                 * re-pick P1's car. */
                if (s_two_player_mode == 1) {
                    TD5_LOG_I(LOG_TAG, "CarSelect 2P: P1 Back → MainMenu");
                    td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                    return;
                }
                s_two_player_mode = 5;
                TD5_LOG_I(LOG_TAG, "CarSelect 2P: P2 Back → CarSelection (mode=5)");
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;
            }
        }

        /* Cup-style modes (game_type 1..6): predefined track schedule —
         * SKIP TrackSelection and launch the race directly with the cup's
         * scheduled track.
         * [CONFIRMED @ 0x0040f744 LAB_0040f78b — original CarSelectionScreen
         *  case 0x1A path for any g_selectedGameType in {1..6} calls
         *  InitializeRaceSeriesSchedule + InitializeFrontendDisplayModeState
         *  directly with no SetFrontendScreen.]
         * Without this branch the port falls through to the generic
         * td5_frontend_set_screen(s_return_screen) at the bottom (which case 4
         * OK sets to TRACK_SELECTION), forcing cup users into a track picker
         * the original game never showed. The cup schedule track index lives
         * at s_cup_schedules[game_type-1][race_within_series] (CONFIRMED @
         * 0x464098, see s_cup_schedules definition above). */
        if (s_selected_game_type >= 1 && s_selected_game_type <= 6 &&
            !s_two_player_mode &&
            s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
            int sched_idx = s_selected_game_type - 1;
            if (s_race_within_series >= 0 && s_race_within_series < 13) {
                int sched_track = s_cup_schedules[sched_idx][s_race_within_series];
                if (sched_track >= 0 && sched_track != 99) {
                    s_selected_track = sched_track;
                }
            }
            s_selected_car = actual_car;
            TD5_LOG_I(LOG_TAG,
                      "CarSelect: cup game_type=%d race=%d track=%d -> skip track select, init schedule",
                      s_selected_game_type, s_race_within_series, s_selected_track);
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
            return;
        }

        /* Drag-race 2-pass CarSelect [CONFIRMED @ 0x0040f744].
         * Original: game_type==7 (=drag race there) && pass_marker==0 → re-enter
         * CarSelect with pass_marker=1, showing "CAR 2". After pass 2, skip
         * TrackSelection entirely and call InitializeRaceSeriesSchedule +
         * InitializeFrontendDisplayModeState directly (drag strip is fixed).
         * Port convention: case 4 OK sets s_return_screen=TRACK_SELECTION while
         * case 5 Back sets RACE_TYPE_MENU — only fire on OK. */
        if (g_td5.drag_race_enabled &&
            s_return_screen == TD5_SCREEN_TRACK_SELECTION) {
            if (s_drag_carselect_pass == 0) {
                /* Pass 1 = PLAYER car. Stash it in dedicated storage so the
                 * pass-2 navigation cursor (s_selected_car) cannot clobber it.
                 * [Mirrors orig g_selectedCarIndex @0x0048f364 being preserved
                 *  across the opponent pass — the port's root-cause fix.] */
                s_p1_car   = actual_car;
                s_p1_paint = s_selected_paint;
                s_drag_carselect_pass = 1;
                /* Seed the pass-2 cursor from the opponent slot (orig loads its
                 * scratch from DAT_00463e08 on the opponent pass) so pass 2 does
                 * not start showing the player's just-picked car. Screen case 0
                 * clamps to the valid roster range. */
                s_selected_car   = s_p2_car;
                s_selected_paint = s_p2_paint;
                TD5_LOG_I(LOG_TAG,
                          "CarSelect: drag-race pass1 PLAYER car=%d saved → re-enter for opponent (cursor seed=%d)",
                          s_p1_car, s_selected_car);
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;
            } else {
                /* Pass 2 = OPPONENT car. */
                s_p2_car = actual_car;
                s_p2_paint = s_selected_paint;
                s_p2_config = s_selected_config;
                s_p2_transmission = s_selected_transmission;
                s_drag_carselect_pass = 0;
                /* Restore the PLAYER car/paint into the live selector BEFORE the
                 * schedule reads slot 0 from s_selected_car. Without this, slot 0
                 * (player) inherits the opponent car the user just cycled to ->
                 * both racers get the same car (the reported bug). */
                s_selected_car   = s_p1_car;
                s_selected_paint = s_p1_paint;
                TD5_LOG_I(LOG_TAG,
                          "CarSelect: drag-race pass2 OPPONENT car=%d, restored PLAYER car=%d → start race",
                          s_p2_car, s_selected_car);
                frontend_init_race_schedule();
                frontend_init_display_mode_state();
                return;
            }
        }

        /* Single-player: OK → track selection, Back → return screen */
        td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
    }
        break;

    default:
        s_inner_state = 7;
        break;
    }
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
/* Is the menu-selected track a CIRCUIT (laps meaningful) or point-to-point (no
 * laps)? Determined WITHOUT loading the race — LEVELINF.DAT isn't read until race
 * init — so:
 *   - TD6 tracks: the registry is authoritative (finish_span>0 = P2P, ==0 =
 *     circuit), matching the same source the in-race lap counter now uses.
 *   - Native TD5 tracks: circuit tracks ship NO reverse strip (forward-only) and
 *     P2P tracks DO, so reverse-data presence is an exact proxy (consistent with
 *     the Direction-toggle model and the per-track shipped data).
 * Random / 2P "?" (slot<0) defaults to circuit so the Laps row stays available. */
static int frontend_track_is_circuit(int track_slot) {
    if (track_slot < 0) return 1;
    int td6_level = td5_asset_td6_level_for_slot(track_slot);
    if (td6_level > 0)
        return td5_asset_td6_finish_span_for_level(td6_level) > 0 ? 0 : 1;
    return td5_asset_track_has_reverse(track_slot) ? 0 : 1;
}

/* Hide/show the "Laps" selector for the currently selected track. Point-to-point
 * tracks have no laps, so the row is hidden+disabled — the vertical nav, the
 * selector arrows (fe_draw_option_arrows) and the value draw all skip a hidden
 * button, exactly like the Direction toggle. Circuit tracks keep the row. Called
 * on screen entry and whenever the selected track changes. The persisted lap
 * value (s_game_option_laps) is untouched and is simply ignored by P2P races
 * (which finish at the finish line, not after N laps). */
static void frontend_update_laps_button_visibility(int laps_btn_idx) {
    if (laps_btn_idx < 0 || laps_btn_idx >= s_button_count) return;
    int is_circuit = frontend_track_is_circuit(s_selected_track);
    s_buttons[laps_btn_idx].hidden   = !is_circuit;
    s_buttons[laps_btn_idx].disabled = !is_circuit;
    TD5_LOG_I(LOG_TAG, "Laps row: track=%d circuit=%d -> %s",
              s_selected_track, is_circuit, is_circuit ? "SHOWN" : "hidden");
    /* Don't leave the highlight parked on a now-hidden row. */
    if (!is_circuit && s_selected_button == laps_btn_idx) s_selected_button = 0;
}

static void frontend_update_direction_button_visibility(int dir_btn_idx, int manage_label) {
    if (dir_btn_idx < 0 || dir_btn_idx >= s_button_count) return;
    int has_reverse = (s_selected_track < 0)
                      ? 1
                      : td5_asset_track_has_reverse(s_selected_track);
    /* Diagnostic: per-track Direction-toggle decision. has_reverse is driven by
     * reverse-asset presence (STRIPB.DAT + LEFTB/RIGHTB.TRK) — TD6 tracks and TD5
     * P2P show the toggle; TD5 circuit tracks (no STRIPB) hide it. */
    TD5_LOG_I(LOG_TAG,
              "Direction toggle: track=%d level=%d has_reverse=%d -> %s",
              s_selected_track,
              (s_selected_track >= 0) ? td5_asset_level_number(s_selected_track) : -1,
              has_reverse, has_reverse ? "SHOWN" : "hidden");
    s_buttons[dir_btn_idx].hidden   = !has_reverse;
    s_buttons[dir_btn_idx].disabled = !has_reverse;
    if (!has_reverse) {
        s_track_direction = 0;
        if (manage_label) {
            strncpy(s_buttons[dir_btn_idx].label, "Forwards", sizeof(s_buttons[dir_btn_idx].label) - 1);
            s_buttons[dir_btn_idx].label[sizeof(s_buttons[dir_btn_idx].label) - 1] = '\0';
        }
        /* Don't leave the highlight focus parked on the now-hidden button. */
        if (s_selected_button == dir_btn_idx) s_selected_button = 0;
    }
}

/* ========================================================================
 * [21] TrackSelectionScreenStateMachine (0x427630)
 * States: 9 (0x00 - 0x08)
 * Buttons: Track (with arrows), Direction toggle, OK, Back
 * ======================================================================== */

static void Screen_TrackSelection(void) {
    switch (s_inner_state) {
    case 0: /* Init: validate track for cup modes, create buttons, load TrackSelect.tga */
        frontend_init_return_screen(TD5_SCREEN_TRACK_SELECTION);
        TD5_LOG_D(LOG_TAG, "TrackSelection: init");
        s_anim_complete = 0;

        /* Validate track index for cup modes: skip locked/invalid NPC groups */
        /* Determine track max for current mode */
        if (s_network_active) {
            s_track_max = 18; /* 19 tracks total */
        } else if (s_two_player_mode) {
            s_track_max = s_total_unlocked_tracks;
        } else {
            s_track_max = s_total_unlocked_tracks;
        }
        if (s_selected_track >= s_track_max) s_selected_track = (s_two_player_mode ? -1 : 0);
        if (!s_two_player_mode && s_selected_track < 0) s_selected_track = 0;

        /* Create buttons. Ghidra settles these at x=120 for Track/Forwards/OK
         * and x=232 for Back, with OK/Back sharing the bottom row. */
        frontend_create_button(SNK_TrackButTxt,     120,  97, 224, 32); /* 0: with L/R arrows */
        frontend_create_button(SNK_ForwardsButTxt,  120, 137, 224, 32); /* 1: direction toggle */
        /* [PORT ENHANCEMENT 2026-06] race-option rows: AI opponents, laps, traffic,
         * police. They drive s_num_ai_opponents + s_game_option_* which apply to
         * single/multiplayer races via ConfigureGameTypeFlags (cup game-types
         * override them, so they're inert there). Present on every track-select
         * entry (regular single race, quick race, multiplayer). */
        frontend_create_button(SNK_OpponentsButTxt, 120, 177, 224, 32); /* 2: AI count */
        frontend_create_button(SNK_LapsButTxt,      120, 217, 224, 32); /* 3: laps */
        frontend_create_button(SNK_TrafficButTxt,   120, 257, 224, 32); /* 4: traffic */
        frontend_create_button(SNK_CopsButTxt,      120, 297, 224, 32); /* 5: police */
        frontend_create_button(SNK_OkButTxt,        120, 377,  96, 32); /* 6: OK */
        /* Quick Race mode: no Back button */
        if (s_flow_context != 2) {
            frontend_create_button(SNK_BackButTxt, 232, 377, 112, 32);  /* 7: Back */
        }

        /* Create 0x128 x 0xB8 info surface */
        frontend_load_tga("Front_End/TrackSelect.tga", "Front_End/FrontEnd.zip");

        s_track_direction = 0;
        s_track_switch_tick = 16; /* holds preview settled during button slide-in (state 3); reset to 0 in state 5 */
        frontend_load_selected_track_preview();
        /* Hide the Direction toggle for forward-only/circuit tracks from the
         * very first frame (cases 1-3 render before case 5 reloads). */
        frontend_update_direction_button_visibility(1, 1);
        /* Hide the Laps row (button 3) on point-to-point tracks (no laps). */
        frontend_update_laps_button_visibility(3);
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present + tick */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        /* (Direction-button visibility is set in case 0 and refreshed on each
         * track cycle in case 4 — see frontend_update_direction_button_visibility.) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            /* Original goes to state 5 (load preview) then state 8 (slide-in) on initial entry.
             * Route through state 5 so the 16-frame preview slide-in plays here too
             * [CONFIRMED @ frontend_screens_decompiled.c line 1222]. */
            TD5_LOG_I(LOG_TAG, "TrackSel: button slide-in complete, starting preview slide-in");
            s_inner_state = 5;
        }
        break;

    case 4: /* Main interaction: track preview + navigation */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int selected_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (selected_button == 0 && delta != 0) {
                /* Cycle track index, skipping tracks whose level zips are absent */
                if (s_network_active) {
                    frontend_cycle_track(delta, 0, s_track_max + 1);
                } else if (s_two_player_mode) {
                    /* 2P supports -1 = random, handled separately */
                    s_selected_track += delta;
                    if (s_selected_track < -1) s_selected_track = s_track_max - 1;
                    if (s_selected_track >= s_track_max) s_selected_track = -1;
                    /* Skip unavailable for positive indices */
                    if (s_selected_track >= 0 && !frontend_track_level_exists(s_selected_track))
                        frontend_cycle_track(delta, 0, s_track_max);
                } else {
                    frontend_cycle_track(delta, 0, s_track_max);
                }
                frontend_play_sfx(2); /* ping2.wav cycle */
                TD5_LOG_I(LOG_TAG, "TrackSel CYCLED: track=%d level=%d name=%s",
                          s_selected_track, td5_asset_level_number(s_selected_track),
                          frontend_get_track_name(s_selected_track));

                /* Hide text+image immediately so render on THIS frame shows neither
                 * (old preview still loaded, new s_selected_track already set).
                 * Case 5 loads new preview next frame; case 9 runs the 16-frame slide-in. */
                s_track_switch_tick = 0;
                /* Re-evaluate the Direction toggle + Laps row for the new track
                 * (hide on forward-only/circuit tracks, restore on reverse-capable). */
                frontend_update_direction_button_visibility(1, 1);
                frontend_update_laps_button_visibility(3);
                s_inner_state = 5;
            }

            /* Direction toggle: 0=Forwards, 1=Backwards. Gated on the track
             * actually having reverse data — forward-only/circuit tracks hide
             * this button (see frontend_update_direction_button_visibility), and
             * the guard here keeps the toggle inert even if reached by another
             * path. Mirrors original 0x00427630 which only flips when the track's
             * reverse flag is set. */
            if (selected_button == 1 && !s_buttons[1].hidden &&
                (delta != 0 || s_button_index == 1)) {
                s_track_direction = !s_track_direction;
                strncpy(s_buttons[1].label,
                        s_track_direction ? "Backwards" : "Forwards",
                        sizeof(s_buttons[1].label) - 1);
                s_buttons[1].label[sizeof(s_buttons[1].label) - 1] = '\0';
            }

            /* [PORT ENHANCEMENT 2026-06] race-option rows (AI/laps/traffic/police). */
            if (delta != 0 && selected_button >= 2 && selected_button <= 5) {
                if (selected_button == 2) {            /* AI opponents */
                    s_num_ai_opponents += delta;
                    if (s_num_ai_opponents < 0) s_num_ai_opponents = 0;
                    if (s_num_ai_opponents > TD5_MAX_RACER_SLOTS - 1)
                        s_num_ai_opponents = TD5_MAX_RACER_SLOTS - 1;
                } else if (selected_button == 3 && !s_buttons[3].hidden) {  /* laps (value+1) */
                    s_game_option_laps += delta;
                    if (s_game_option_laps < 0) s_game_option_laps = 0;
                    if (s_game_option_laps > 9) s_game_option_laps = 9;
                } else if (selected_button == 4) {     /* traffic on/off */
                    s_game_option_traffic = (s_game_option_traffic + delta) & 1;
                } else if (selected_button == 5) {     /* police on/off */
                    s_game_option_cops = (s_game_option_cops + delta) & 1;
                }
                frontend_play_sfx(2);
            }

            if (s_button_index == 6) { /* OK */
                /* Lock enforcement */
                int locked = (s_selected_track >= 0 && s_selected_track < 37 &&
                             s_track_lock_table[s_selected_track] != 0 &&
                             !s_network_active && !s_cheat_unlock_all);
                if (locked) {
                    frontend_play_sfx(10);
                } else {
                    g_td5.reverse_direction = s_track_direction;
                    /* [S02 (c) 2026-06-04] Persist the lap choice (re-homed from
                     * Game Options, which no longer owns this setting). */
                    g_td5.ini.laps = s_game_option_laps;
                    td5_ini_persist_options();
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 6;
                }
            }

            if (s_button_index == 7) { /* Back */
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 6;
            }
        }
        break;

    case 5: /* Track change: load new preview, start slide-in animation */
        /* Original: frame 1 clears text surface, frame 2 loads preview → state 8 (slide-in).
         * Source port: load immediately, then run slide-in via state 9.
         * Both text and preview animate together via s_track_switch_tick [CONFIRMED @ 0x427984]. */
        frontend_load_selected_track_preview();
        s_track_switch_tick = 0;
        TD5_LOG_I(LOG_TAG, "TrackSel: track change slide-in start track=%d", s_selected_track);
        s_inner_state = 9;
        break;

    case 6: /* Slide-out prep */
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 7;
        break;

    case 7: /* Slide-out animation (~1200ms) */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_inner_state = 8;
        }
        break;

    case 8: /* Exit dispatch */
        /* Quick Race (context 2): go to screen 7 */
        /* Network (context 4): CreateFrontendNetworkSession() */
        /* Normal (return == -1): launch race */
        /* Back (return != -1): nav to screen */
        if (s_flow_context == 2) {
            td5_frontend_set_screen(TD5_SCREEN_QUICK_RACE);
        } else if (s_flow_context == 4) {
            /* Network: create session and go to lobby */
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
        } else if (s_return_screen == -1) {
            frontend_init_race_schedule();
            frontend_init_display_mode_state();
        } else {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;

    case 9: /* Track-switch slide-in: 16 frames [CONFIRMED @ 0x427e96 original state 8].
             * Single tick counter drives both preview (slides from right) and text (slides from above).
             * Original: tick * -0x10 + 0x22e for preview x; (tick-0x10)*0x10 + base for text y. */
        s_track_switch_tick++;
        if (s_track_switch_tick >= 16) {
            s_track_switch_tick = 16; /* settled: render offsets become 0 */
            TD5_LOG_I(LOG_TAG, "TrackSel: track change slide-in complete");
            frontend_play_sfx(4); /* completion chime [CONFIRMED @ 0x427e96] */
            s_inner_state = 4;
        }
        break;
    }
}

/* ========================================================================
 * [22] ScreenExtrasGallery (0x417D50) -- Credits / developer mugshots
 * States: 4
 * Original loads all 27 surfaces from Mugshots.zip at init.
 * Source port loads on demand (one at a time) to save VRAM.
 * Exits game when complete (exit flow) or returns to previous screen.
 * ======================================================================== */

#define GALLERY_PIC_COUNT   27
#define GALLERY_ALL_VISITED ((1 << GALLERY_PIC_COUNT) - 1)
#define GALLERY_ZIP         "Front End/Extras/Mugshots.zip"

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

static void Screen_ExtrasGallery(void) {
    switch (s_inner_state) {
    case 0: /* Init: load the dev mugshots, start the scroll reel */
        frontend_init_return_screen(TD5_SCREEN_EXTRAS_GALLERY);
        for (int i = 0; i < K_CREDIT_MUGSHOT_COUNT; i++) {
            if (s_credit_mugshot_surf[i] <= 0)
                s_credit_mugshot_surf[i] = frontend_load_tga(k_credit_mugshots[i], GALLERY_ZIP);
        }
        s_anim_tick = 0;
        TD5_LOG_I(LOG_TAG, "ExtrasGallery: credits scroll init (%d rows, %d photos)",
                  K_CREDITS_COUNT, K_CREDIT_MUGSHOT_COUNT);
        s_inner_state = 1;
        break;

    case 1: /* Brief delay to prevent input bleed from menu (~39 ticks) */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x27) {
            s_credits_start_ms = td5_plat_time_ms();
            s_inner_state = 2;
        }
        break;

    case 2: /* Scroll the credit reel; quit once it fully passes (orig exits after credits).
             * ESC/click also exits via the global escape handler. [FAITHFUL 2026-06-02 — replaces
             * the prior photo slideshow with the original's vertical scroll of SNK_CreditsText.] */
        {
            float scroll = (float)(td5_plat_time_ms() - s_credits_start_ms) * FE_CREDITS_SPEED;
            if (scroll > 480.0f + frontend_credits_total_height()) {
                TD5_LOG_I(LOG_TAG, "ExtrasGallery: credits complete, quitting");
                frontend_post_quit();
            }
        }
        break;
    }
}

/* ========================================================================
 * [23] ScreenPostRaceHighScoreTable (0x413580)
 * States: 9
 * ======================================================================== */

static void Screen_PostRaceHighScore(void) {
    switch (s_inner_state) {
    case 0: /* Init: load BG, create surfaces, buttons */
        frontend_init_return_screen(TD5_SCREEN_HIGH_SCORE);
        TD5_LOG_D(LOG_TAG, "PostRaceHighScore: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_anim_complete = 0;
        /* Create 0x208 x 0x90 score panel surface (black fill) */
        /* Create nav button + OK button */
        /* [FIXED 2026-06-01 v2] Nav-bar selector + OK button — FAITHFUL native positions,
         * read from the original's RESOLVED auto-layout in runtime memory (button table
         * @0x499c78, the -0x208/-0x130 auto-layout sentinels resolve to these):
         *   button 0 (nav bar): origin (0x73,0x61)=(115,97), 0x208x0x20 = 520x32
         *   button 1 (OK)     : origin (115,377),            0x60 x0x20 = 96x32
         * The prior y128 was derived from the widescreen-PATCHED exe, whose ~1.275x vertical
         * frontend stretch renders the nav text at screen-y ~140-158 (= native ~103-118); the
         * previous fix wrongly treated those stretched screen pixels as native coords. With the
         * faithful button y=97, the centered track name (drawn at ty=nav_by+6, see ~line 6175)
         * lands at native top 103 — matching the de-stretched original. OK was likewise off
         * (port 120,416 vs faithful 115,377). The track name itself is button-centered, which
         * mirrors the original: RebuildFrontendButtonSurface(0,0,SNK_TrackNames[pool]) draws
         * the label MeasureOrCenter'd across the 520px button width. */
        frontend_create_button(NULL, 115, 97, 520, 32);    /* nav bar selector */
        frontend_create_button(SNK_OkButTxt, 115, 377, 96, 32);  /* OK button */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        s_score_category_index = 0;
        /* [FIXED 2026-06-01] Records browsing has no "just-inserted" row, so no row
         * is highlighted here. -1 keeps the shared high-score overlay from golding a
         * stale rank (NAME_ENTRY sets s_score_insert_pos to the real inserted rank). */
        s_score_insert_pos = -1;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present, reset tick, draw score entry 0 */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            frontend_set_cursor_visible(1);
            frontend_play_sfx(4);
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R arrows browse score categories */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            if (delta != 0) {
                s_score_category_index += delta;
                /* High score screen: all 26 tracks+cups accessible regardless of lock state.
                 * Simple wrap [0..0x19]. */
                if (s_score_category_index > 0x19) s_score_category_index = 0;
                if (s_score_category_index < 0)    s_score_category_index = 0x19;
            }
            if (s_button_index == 1) { /* OK */
                s_inner_state = 7;
            }
        }
        break;

    case 7: /* Prep slide-out */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* Return to caller or init race.
             * When s_return_screen == -1: original calls InitializeFrontendDisplayModeState
             * which unconditionally calls WritePackedConfigTd5 to flush high-score table.
             * [CONFIRMED @ 0x00413b60 / 0x00414aa0] */
            if (s_return_screen == -1) {
                frontend_init_race_schedule();
                TD5_LOG_I(LOG_TAG, "PostRaceHighScore: writing config (high-score flush)");
                td5_save_write_config(NULL);
            } else {
                td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            }
        }
        break;
    }
}

/* ========================================================================
 * [24] RunRaceResultsScreen (0x422480)
 * States: 22 (0x00 - 0x15)
 *
 * Central post-race hub: score display, replay, save, cup progression.
 * ======================================================================== */

static void Screen_RaceResults(void) {
    switch (s_inner_state) {
    case 0: /* Init & routing: sort results, create panel */
        frontend_init_return_screen(TD5_SCREEN_RACE_RESULTS);
        frontend_reset_buttons();
        TD5_LOG_I(LOG_TAG, "RaceResults: state 0 - init, game_type=%d",
                  s_selected_game_type);

        /* P8 — DXSound::Play(5) on screen entry.
         * [CONFIRMED @ 0x00422480 case 0] Original calls DXSound::Play(5) near
         * the end of state 0 init, before g_frontendInnerState advances. */
        frontend_play_sfx(5);

        /* P2 (plan_screen24) — MainMenu.tga backdrop. Original case 0 calls
         * LoadTgaToFrontendSurface16bpp("MainMenu.tga", "FrontEnd.zip") then
         * CopyPrimaryFrontendBufferToSecondary so the post-race UI overlays
         * the main-menu artwork. Without it the bg-gallery slideshow floats
         * on a black clear — visually empty. Every other screen handler
         * loads this same TGA as its s_background_surface (see e.g.
         * Screen_MainMenu state 0); we mirror that to give screen 24 a
         * faithful backdrop. */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* [CONFIRMED @ 0x004224B6 RunRaceResultsScreen case 0] Early-route to
         * Screen_CupFailed (TD5_SCREEN_CUP_FAILED, 0x1a) when the player has
         * been eliminated mid-cup. The original has two parallel paths:
         *
         *   1) game_type == 4 + slot_state[0]+0x383 != 0 (game-type-4-only)
         *   2) !network && results_skip_display != 1 && 1 <= game_type < 7
         *      && (slot_state[0].companion_state_2 == 2 ||
         *          actor_state.slot[0]+0x328 == 0)
         *
         * Both jump straight to TD5_SCREEN_CUP_FAILED with return_screen = 5
         * (TD5_SCREEN_MAIN_MENU). Without this, exiting a cup mid-progression
         * skips the failure dialog entirely and dumps the player back to the
         * main menu. The port reaches TD5_SCREEN_CUP_FAILED only via the
         * explicit Quit button at case 0x10 — which doesn't fire when the
         * cup-failed condition is detected automatically. */
        if (!s_results_view_data_request &&
            !s_network_active &&
            !s_results_skip_display &&
            s_selected_game_type >= 1 && s_selected_game_type < 7 &&
            (td5_game_get_slot_companion_2(0) == 2 ||
             !td5_game_slot_is_finished(0))) {
            TD5_LOG_I(LOG_TAG, "RaceResults: cup-fail early-route "
                      "(game_type=%d companion_2=%d finished=%d) -> CUP_FAILED",
                      s_selected_game_type,
                      td5_game_get_slot_companion_2(0),
                      td5_game_slot_is_finished(0));
            s_return_screen = TD5_SCREEN_MAIN_MENU;
            td5_frontend_set_screen(TD5_SCREEN_CUP_FAILED);
            return;
        }

        /* Sort results by game type:
         * Types 1/6: by secondary metric desc (SortRaceResultsBySecondaryMetricDesc @ 0x40AB80)
         * Types 2-5: by primary metric asc    (SortRaceResultsByPrimaryMetricAsc   @ 0x40AAD0)
         * [CONFIRMED @ 0x00422480 case 0] */
        td5_game_sort_results();

        /* Save race snapshot on first entry */
        if (!s_results_rerace_flag) {
            s_snap_car = s_selected_car;
            s_snap_paint = s_selected_paint;
            s_snap_trans = s_selected_transmission;
            s_snap_config = s_selected_config;
            s_snap_num_ai_opponents = s_num_ai_opponents;  /* [FIX 2026-06-05] */
            s_results_rerace_flag = 1;
            TD5_LOG_I(LOG_TAG,
                      "RaceResults: snapshot saved car=%d opponents=%d (for Race Again)",
                      s_snap_car, s_snap_num_ai_opponents);
        }

        /* [CONFIRMED @ 0x00422480 case 0 tail] Build the click-catcher + OK
         * button pair, then advance to state 1. The original places them at
         * (-0x208, 0, 0x208, 0x20) and (-0x208, 0, 0x60, 0x20) — both off the
         * left edge of the canvas — and slides them in via MoveFrontendSpriteRect
         * during state 3 (P7 of plan_screen24, not in this slice). Until the
         * sprite-rect animation lands we plant them at their on-screen rest
         * positions: 520x32 invisible click-catcher centred at y=400 with the
         * 96x32 OK button overlaid in the centre. Both indices satisfy the
         * state-6 exit gate (s_button_index >= 0 && < 2), so clicking either
         * the OK button or anywhere along the catcher row returns to the
         * post-race menu. The state-3 gate below short-circuits to state 0xC
         * when no race data is present (fresh StartScreen=24 land on menu;
         * post-race natural flow runs the table sub-flow). */
        s_results_cup_complete = 0;
        s_results_skip_display = 0;
        s_anim_tick = 0;
        /* [FIXED 2026-06-02] Faithful rest positions (orig RunRaceResultsScreen case 3
         * MoveFrontendSpriteRect at counter==0x27, halfW=320/halfH=240):
         *   btn0 selector nav bar (520x32) slides to (115,97) — shows the car name +
         *        ◄► arrows (browse racers); orig creates it NULL-labelled at -0x208.
         *   btn1 OK (96x32) slides to (115,377) — bottom-LEFT.
         * The prior placeholder planted BOTH at y=400, which read as a double OK. */
        { int bi = frontend_create_button(NULL, 115, 97, 0x208, 0x20);
          if (bi >= 0) s_buttons[bi].is_selector = 1; }
        frontend_create_button(SNK_OkButTxt, 115, 377, 0x60, 0x20);
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present buffer, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames.
             * [CONFIRMED @ 0x004228EC case 3 head] First three statements
             * gate the slide-in itself. The original short-circuits to
             * state 0xC (cleanup -> 0xD menu) when ANY of:
             *   - DAT_00497a74 != 0    (s_results_skip_display)
             *   - slot[0].companion_state_2 == 2  (player disqualified)
             *   - actor.slot._808_4_ == 0         (no race finished)
             * The `slot._808_4_ == 0` clause is what makes a fresh
             * SetFrontendScreen(0x18) — i.e. --StartScreen=24 or a Frida
             * frontend_screen=24 hop — land directly on the post-race menu:
             * no actor data has been written, so the table sub-flow is
             * skipped. After a real race the finish flag is set and the
             * gate falls through, so the table animates in normally. */
        if (!s_results_view_data_request &&
            (s_results_skip_display ||
             td5_game_get_slot_companion_2(0) == 2 ||
             !td5_game_slot_is_finished(0))) {
            TD5_LOG_I(LOG_TAG,
                      "RaceResults: state 3 early-exit gate fired "
                      "(skip=%d companion_2=%d finished=%d) -> 0xC",
                      s_results_skip_display,
                      td5_game_get_slot_companion_2(0),
                      td5_game_slot_is_finished(0));
            s_results_skip_display = 0;
            s_inner_state = 0x0C;
            break;
        }
        s_anim_tick += 2;
        if (s_anim_tick >= 0x12) {
            /* P8 — DXSound::Play(4) on slide-in completion.
             * [CONFIRMED @ 0x00422480 case 3] Original fires Play(4) inside
             * the AdvanceFrontendTickAndCheckReady ready-branch immediately
             * before g_frontendInnerState++. */
            frontend_play_sfx(4);
            /* Sentinel served its purpose — clear now that state 3's full
             * 9-tick animation has run. Clearing earlier (e.g. immediately
             * after the gate check) would let the gate fire on tick 2 since
             * !finished is still true; the flag must survive across the
             * entire state-3 anim window. */
            s_results_view_data_request = 0;
            /* Mark the intro slide complete (mirrors High Scores case 3). This both
             * enables the shared ESC handler and gates the selector nav-bar car-name +
             * ◄► render — without it the top selector bar drew empty. [FIXED 2026-06-02] */
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R browse racer slots (0-5), confirm exits.
             * Original @ 0x004229DA: button_index >= 0 && < 2 -> state 0x0B.
             * [CONFIRMED @ 0x004229DA] DAT_00497a68 cycles by DAT_0049b690 (arrow delta),
             * skips slots with state == 3 (disabled). Drag: masked & 1 for 2-slot only.
             * P7 PANEL fix: arrow input now triggers state 7 (right) or 9 (left)
             * for the slide-out animation; the actual slot cycle happens
             * between out- and in-slide so the new slot's data is visible
             * only as the panel re-enters from the opposite side. */
        s_results_panel_slide_x = 0;  /* clean rest position while in interactive */
        if (s_input_ready) {
            /* [FIX 2026-06-05 results-nav] Only LEFT/RIGHT browse racer slots
             * (the horizontal panel slide). s_arrow_input is a BITMASK
             * (1=LEFT,2=RIGHT,4=UP,8=DOWN), so the old `!= 0` test fired on
             * UP/DOWN too, and `> 0` is true for any non-zero mask — so every
             * arrow (incl. LEFT) slid the panel RIGHT. UP/DOWN are already
             * consumed by the shared row-nav handler (frontend_update_input
             * moves the selection between the selector bar and OK vertically),
             * so here we react to the horizontal bits only and honour their
             * direction: RIGHT -> state 7 (exit right), LEFT -> state 9. */
            if (s_arrow_input & 2) {          /* RIGHT */
                s_results_panel_slide_dir = +1;
                s_anim_tick = 0;
                s_inner_state = 7;
                TD5_LOG_D(LOG_TAG,
                          "RaceResults state 6: RIGHT -> slide-out state 7");
                break;
            }
            if (s_arrow_input & 1) {          /* LEFT */
                s_results_panel_slide_dir = -1;
                s_anim_tick = 0;
                s_inner_state = 9;
                TD5_LOG_D(LOG_TAG,
                          "RaceResults state 6: LEFT -> slide-out state 9");
                break;
            }
            /* [CONFIRMED @ 0x004229DA] Original wraps the button-press exit
             * in `if (DAT_0049b690 == 0)` — i.e. only honor the confirm when
             * no arrow input is also queued. Without the gate a paired
             * arrow-and-click exits the browser before s_score_category_index
             * has updated, which the original avoids. */
            if (s_button_index >= 0 && s_button_index < 2) { /* confirm -> exit */
                TD5_LOG_I(LOG_TAG, "RaceResults: state 6 -> 0x0B (confirm, btn=%d)",
                          s_button_index);
                s_anim_tick = 0;
                s_inner_state = 0x0B;
            }
        }
        break;

    case 7: /* Right slide-OUT: panel exits right edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 7] Original formula:
             *   panel_x = g_frontendAnimFrameCounter * 0x20 + 0x2a + iVar4
             * Step +0x20 (32 px/frame), end counter == 0x11 (17). At end:
             *   DrawRaceDataSummaryPanel(DAT_00497a68);  // re-fill with new slot
             *   counter = 0; state++;                    // -> state 8
             * Port uses s_anim_tick stepping +2 from 0..0x11. */
        s_anim_tick += 2;
        s_results_panel_slide_x = s_anim_tick * 0x20;  /* +0..+0x220 */
        if (s_anim_tick >= 0x11) {
            /* Cycle slot index now (mid-slide) — new data visible on slide-in.
             * [CONFIRMED @ 0x00422A22] Wrap: [0..5] with 6 -> 0 and -1 -> 5. */
            s_score_category_index += s_results_panel_slide_dir;
            if (s_selected_game_type == 7) {
                s_score_category_index &= 1;  /* drag 2-slot mask */
            } else {
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            /* Skip disabled slots */
            for (int _skip = 0;
                 _skip < 6 && td5_game_get_slot_state(s_score_category_index) == 3;
                 _skip++) {
                s_score_category_index += s_results_panel_slide_dir;
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            TD5_LOG_D(LOG_TAG, "RaceResults: slid out right, now slot=%d",
                      s_score_category_index);
            s_anim_tick = 0;
            s_inner_state = 8;
        }
        break;

    case 8: /* Left slide-IN: panel enters from left edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 8] Original formula:
             *   panel_x = counter * 0x20 + -0x1f6 + iVar4
             * At counter=0 panel is off-screen left (-0x1f6 + iVar4 ~ -382);
             * at counter=0x11 it reaches rest x. Port: offset progresses
             * from -0x220 → 0. */
        s_anim_tick += 2;
        s_results_panel_slide_x = -((0x11 - s_anim_tick) * 0x20);  /* -0x220..0 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_inner_state = 6;
        }
        break;

    case 9: /* Left slide-OUT: panel exits left edge. 17 frames.
             * [CONFIRMED @ 0x00422480 case 9] Original formula:
             *   panel_x = iVar4 + counter * -0x20 + 0x2a
             * Step -0x20 per frame; same DrawRaceDataSummaryPanel re-fill
             * trigger at counter==0x11. */
        s_anim_tick += 2;
        s_results_panel_slide_x = -(s_anim_tick * 0x20);  /* 0..-0x220 */
        if (s_anim_tick >= 0x11) {
            s_score_category_index += s_results_panel_slide_dir;
            if (s_selected_game_type == 7) {
                s_score_category_index &= 1;
            } else {
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            for (int _skip = 0;
                 _skip < 6 && td5_game_get_slot_state(s_score_category_index) == 3;
                 _skip++) {
                s_score_category_index += s_results_panel_slide_dir;
                if (s_score_category_index >= 6) s_score_category_index = 0;
                if (s_score_category_index < 0)  s_score_category_index = 5;
            }
            TD5_LOG_D(LOG_TAG, "RaceResults: slid out left, now slot=%d",
                      s_score_category_index);
            s_anim_tick = 0;
            s_inner_state = 10;
        }
        break;

    case 10: /* Right slide-IN: panel enters from right edge. 17 frames.
              * [CONFIRMED @ 0x00422480 case 10] Original formula:
              *   panel_x = iVar4 + counter * -0x20 + 0x24a
              * At counter=0 panel is off-screen right (+0x220 from rest);
              * at counter=0x11 reaches rest. */
        s_anim_tick += 2;
        s_results_panel_slide_x = (0x11 - s_anim_tick) * 0x20;  /* +0x220..0 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_inner_state = 6;
        }
        break;

    case 0x0B: /* Exit slide-out: 17 frames.
                * [CONFIRMED @ 0x00422480 case 0xB] Original simultaneously
                * slides panel right (step +0x30) and title left (step -0x20).
                * Port animates the panel via slide_x; the title strip is
                * gated to states 3..0xB by the P1 fix and disappears at
                * state 0xC, so its X slide is approximated by the existing
                * Y-slide-out path that fires when the FSM exits the
                * table-browse window. */
        s_anim_tick += 2;
        s_results_panel_slide_x = s_anim_tick * 0x30;  /* +0..+0x330 */
        if (s_anim_tick >= 0x11) {
            s_results_panel_slide_x = 0;
            s_anim_tick = 0;
            s_inner_state = 0x0C;
        }
        break;

    case 0x0C: /* Cleanup: release tracked surfaces + clear button table.
                * P9 — [CONFIRMED @ 0x00422CEE] original body:
                *   DAT_0049628c = ReleaseTrackedFrontendSurface(DAT_0049628c);
                *   DAT_00496358 = ReleaseTrackedFrontendSurface(DAT_00496358);
                *   ReleaseFrontendDisplayModeButtons();
                *   g_frontendInnerState++;
                * Port has no analog of those tracked surfaces — the panel
                * (DAT_0049628c, 408×392) is rendered fresh each frame from
                * frontend_render_race_results_overlay via fe_draw_text + the
                * MainMenu.tga backdrop (P2), not allocated through
                * CreateTrackedFrontendSurface, so there is nothing to release.
                * The title (DAT_00496358) maps to ResultsText.tga which is
                * cached persistently in s_title_tex_page[] and reused on
                * re-entry; releasing it here would force a re-decode on every
                * round-trip. So state 0xC collapses to button-table reset.
                * State 0x14 transitions to 0xD (NOT 0xC per the [Ghidra
                * 2026-05-01 RE: agent corrected the plan]) — no double-free. */
        frontend_reset_buttons();
        s_inner_state = 0x0D;
        break;

    case 0x0D: /* Post-results menu: create context-dependent buttons.
                * Button dims 0x120 x 0x20 (288x32), not port's prior 0xE0.
                * Y offsets -0x8F/-0x5F/-0x2F/+1/+0x31 from canvas center
                * (step 0x30 = 48). [CONFIRMED @ 0x004231D6-0x0042323C] */
        TD5_LOG_I(LOG_TAG, "RaceResults: state 0xD - building menu (game_type=%d)",
                  s_selected_game_type);

        if (s_network_active) {
            /* Network: skip to lobby or main menu */
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
            return;
        }

        {
            /* [FIXED 2026-06-02, runtime @0x499c78] menu buttons LEFT-align at x=120 (halfW-200),
             * NOT centered (port had FE_CENTER_X-0x90=176). Y0=97 step 48 + 288-wide confirmed. */
            const int RR_BX = FE_CENTER_X - 200;      /* 120 */
            const int RR_BW = 0x120;                  /* 288 */
            const int RR_BH = 0x20;                   /* 32 */
            const int RR_Y0 = FE_CENTER_Y - 0x8F;     /* 97 */
            const int RR_Y1 = FE_CENTER_Y - 0x5F;     /* 145 */
            const int RR_Y2 = FE_CENTER_Y - 0x2F;     /* 193 */
            const int RR_Y3 = FE_CENTER_Y + 0x01;     /* 241 */
            const int RR_Y4 = FE_CENTER_Y + 0x31;     /* 289 */

            /* Cup races are game_type 1..6; types 0, 7, 9 are single-race
             * variants (Quick Race / Time Trial / Drag). Slot 1 (View Replay)
             * and slot 2 (View Race Data) are identical across both branches;
             * only slots 0/3/4 differ. ConfigureGameTypeFlags has runtime
             * side effects (mode globals) so it stays gated to cup races. */
            const int is_cup = (s_selected_game_type >= 1 &&
                                s_selected_game_type != 7 &&
                                s_selected_game_type != 9);
            const int next_valid = is_cup ? ConfigureGameTypeFlags() : 1;
            /* [FIXED 2026-06-01] byte-exact SNK_ labels (results action menu, state 0xD):
             * SNK_NextCupRace/RaceAgain/SaveRaceStatus/SelectNewCar/Quit, OK = SNK_OkButTxt. */
            const char *btn0 = is_cup ? SNK_NextCupRace    : SNK_RaceAgain;
            const char *btn3 = is_cup ? SNK_SaveRaceStatus : SNK_SelectNewCar;
            /* Cup-complete path @ 0x00422FD8: slot 4 swaps Quit→OK and the
             * Quit dispatch in state 0x10 routes to CUP_WON/CUP_FAILED. */
            const char *btn4 = (is_cup && !next_valid) ? SNK_OkButTxt : SNK_Quit;

            frontend_create_button(btn0,              RR_BX, RR_Y0, RR_BW, RR_BH);
            /* NOTE (2026-06-01): orig greys View Replay / View Race Data as disabled
             * preview buttons when g_replayFileAvailable==0 / no race-data. The port has
             * no replay subsystem, so there is no availability flag to gate on; leaving
             * them live (they no-op) rather than greying them ALWAYS (which would be the
             * literal result of "no replay ever available" and is arguably worse UX).
             * Revisit if/when a replay system is added. [fix_20.md S24] */
            frontend_create_button(SNK_ViewReplay,     RR_BX, RR_Y1, RR_BW, RR_BH);
            frontend_create_button(SNK_ViewRaceData,  RR_BX, RR_Y2, RR_BW, RR_BH);
            frontend_create_button(btn3,              RR_BX, RR_Y3, RR_BW, RR_BH);
            frontend_create_button(btn4,              RR_BX, RR_Y4, RR_BW, RR_BH);

            if (is_cup && !next_valid) {
                s_results_cup_complete = 1;
            }

            /* Masters (type 5): special progression (PRE-EXISTING branch
             * reused verbatim; flag inversion from original is tracked in
             * prior memory and not touched by this fix) */
            if (is_cup && s_selected_game_type == 5 && !s_results_rerace_flag &&
                s_race_within_series != 9) {
                s_inner_state = 0x15;
                return;
            }
        }

        s_anim_tick = 0;
        s_inner_state = 0x0E;
        break;

    case 0x0E: /* Menu slide-in: 5 buttons animate in, 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x0F;
        }
        break;

    case 0x0F: /* Menu interaction */
        if (s_input_ready && s_button_index >= 0) {
            s_results_button = s_button_index;
            s_anim_tick = 0;
            s_inner_state = 0x10;
        }
        break;

    case 0x10: /* Menu slide-out: 32 frames, then dispatch */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            switch (s_results_button) {
            case 0: /* Race Again / Next Cup Race */
                if (s_results_rerace_flag) {
                    /* Restore snapshot */
                    s_selected_car = s_snap_car;
                    s_selected_paint = s_snap_paint;
                    s_selected_transmission = s_snap_trans;
                    s_selected_config = s_snap_config;
                    /* [FIX 2026-06-05 race-again-opponent-count] Restore the
                     * opponent count so the rerun keeps the same field size.
                     * frontend_init_race_schedule reads s_num_ai_opponents for
                     * the RACE_RESULTS re-race path (game_type 0); cups override
                     * the count downstream so this is a no-op for them. */
                    if (s_snap_num_ai_opponents >= 0)
                        s_num_ai_opponents = s_snap_num_ai_opponents;
                    TD5_LOG_I(LOG_TAG,
                              "RaceResults: Race Again -> restored car=%d opponents=%d "
                              "(game_type=%d)",
                              s_selected_car, s_num_ai_opponents, s_selected_game_type);
                }
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_race_within_series++;
                }
                frontend_init_race_schedule();
                break;

            case 1: /* View Replay */
                /* [CONFIRMED @ 0x00422F2C case 1 in case 0xF]:
                 *   g_attractModeControlEnabled = 2
                 *   g_inputPlaybackActive = 1
                 *   g_frontendInnerState++ (fall into slide-out then InitializeFrontendDisplayModeState)
                 * Port: set s_replay_mode via td5_input_set_replay_mode(1),
                 *       enable playback via td5_input_set_playback_active(1),
                 *       then re-init race (replay file was saved at race start).
                 * [DA-M1 fix 2026-05-22] Also set the game-side s_replay_mode static
                 * via td5_game_set_replay_mode(1). Without this, td5_game_init_race_session
                 * at td5_game.c:1902 hits the WriteOpen branch (memsets recording buffer)
                 * → playback reads zero input forever → race appears to restart blank.
                 * Closes todo-view-replay-restarts-race-2026-05-19. */
                TD5_LOG_I(LOG_TAG, "RaceResults: View Replay selected "
                          "(reuse schedule + restore seed; track=%d reverse=%d "
                          "ai=[%d,%d,%d,%d,%d])",
                          g_td5.track_index, g_td5.reverse_direction,
                          g_td5.ai_car_indices[1], g_td5.ai_car_indices[2],
                          g_td5.ai_car_indices[3], g_td5.ai_car_indices[4],
                          g_td5.ai_car_indices[5]);
                td5_game_set_demo_mode(0);            /* a replay, not a demo */
                td5_input_set_replay_mode(1);
                td5_input_set_playback_active(1);
                td5_game_set_replay_mode(1);
                /* Determinism: re-enter the SAME race WITHOUT rebuilding the AI
                 * schedule. Keep the recorded race's track/direction/opponents
                 * (g_td5.track_index, reverse_direction, ai_car_indices[]) and let
                 * td5_game_init_race_session restore the saved RNG seed (step 0).
                 * Mirrors the original, which re-enters via the unchanged
                 * g_selectedScheduleIndex and does NOT call
                 * InitializeRaceSeriesSchedule for View Replay [CONFIRMED
                 * @0x422F2C]. Calling frontend_init_race_schedule() here (as the
                 * port previously did) re-picks AI cars from a fresh time-seed →
                 * different opponents → "not the race I just drove". Just request
                 * the race; the state machine fires init on the next tick. */
                g_td5.race_requested = 1;
                break;

            case 2: /* View Race Data — re-enter screen 24 from state 0.
                     * [CONFIRMED @ 0x00423110 case 0x10 dispatch on
                     * DAT_00497a64 == 2] Original simply calls
                     * SetFrontendScreen(0x18) — which IS this screen,
                     * 0x18 == 24 == TD5_SCREEN_RACE_RESULTS. Re-entry runs
                     * state 0 again: snapshot restore, sort, button rebuild,
                     * then states 1..3 take the table path because the
                     * state-3 gate above does not fire (race actor data
                     * is still populated). The previous port "invention"
                     * (manual button reset + state=1 seed) skipped the
                     * sort/snapshot and could leak stale buttons across
                     * cycles; faithful re-entry replaces all of that. */
                TD5_LOG_I(LOG_TAG,
                          "RaceResults: View Race Data button — self-jump "
                          "(companion_2[0]=%d finished[0]=%d skip=%d)",
                          td5_game_get_slot_companion_2(0),
                          td5_game_slot_is_finished(0),
                          s_results_skip_display);
                /* Bypass state 0 cup-fail and state 3 table-skip gates so the
                 * table always shows when the user explicitly asks for it
                 * (even after a DNF / early quit where slot_is_finished == 0). */
                s_results_view_data_request = 1;
                td5_frontend_set_screen(TD5_SCREEN_RACE_RESULTS);
                return;

            case 3: /* Save Race Status / Select New Car */
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_inner_state = 0x11; /* save cup data */
                } else {
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                }
                break;

            case 4: /* Quit
                     * P6 — Quit branch fidelity. [CONFIRMED @ 0x004233E0
                     * dispatch on DAT_00497a64 == 4] Original dispatches FOUR
                     * branches; the prior port collapsed them to TWO and got
                     * the cup-won/failed inversion wrong:
                     *
                     *   game_type < 1 (single):
                     *     AwardCupCompletionUnlocks(); SetFrontendScreen(0x19)
                     *   game_type >= 1, DAT_00497a70 == 0  (cup mid-progress):
                     *     SetFrontendScreen(5)            // back to MainMenu
                     *   cup, DAT_00497a70 != 0, DAT_0048d988._2_2_ == 0  (won):
                     *     g_returnToScreenIndex = 0x19;   SetFrontendScreen(0x1B)
                     *   cup, DAT_00497a70 != 0, DAT_0048d988._2_2_ != 0  (failed):
                     *     g_returnToScreenIndex = 0x19;   SetFrontendScreen(0x1A)
                     *
                     * DAT_0048d988._2_2_ is the int16 at +2 of s_results[0]
                     * — i.e. final_position. 0 = 1st place (won), nonzero =
                     * not 1st (failed). DAT_00497a70 is s_results_cup_complete,
                     * set in state 0xD when ConfigureGameTypeFlags()==0.
                     * AwardCupCompletionUnlocks @ 0x00421DA0 grants unlock flags;
                     * Screen_CupWon already invokes the port equivalent
                     * (td5_save_apply_cup_unlocks_ex) on its own entry, so the
                     * single-race quit path here only needs the screen jump.
                     * Game types 7 (Drag) and 9 (Drag Race) fall under "cup"
                     * here because the dispatch only special-cases <1, NOT 7/9. */
                    if (s_selected_game_type < 1) {
                        td5_frontend_set_screen(TD5_SCREEN_NAME_ENTRY);  /* 0x19 */
                    } else if (!s_results_cup_complete) {
                        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);   /* 5 */
                    } else {
                        s_return_screen = TD5_SCREEN_NAME_ENTRY;         /* 0x19 */
                        if (td5_game_get_finish_position(0) == 0) {
                            td5_frontend_set_screen(TD5_SCREEN_CUP_WON);    /* 0x1B */
                        } else {
                            td5_frontend_set_screen(TD5_SCREEN_CUP_FAILED); /* 0x1A */
                        }
                    }
                break;
            }
        }
        break;

    case 0x11: /* Save cup data */
        /* [CONFIRMED @ 0x0042332E]: WriteCupData() result picks SNK_BlockSavedOK or
         * SNK_FailedToSave string; creates 2 buttons (message + OK).
         * Port maps to localised strings via static labels. */
        {
            const char *save_msg;
            if (frontend_write_cup_data()) {
                TD5_LOG_I(LOG_TAG, "RaceResults: cup data saved (Block Saved OK)");
                save_msg = "Block Saved OK";
            } else {
                TD5_LOG_W(LOG_TAG, "RaceResults: failed to save cup data (Failed to Save)");
                save_msg = "Failed to Save";
            }
            /* Button 0: message label (288x32); Button 1: OK (96x32).
             * [CONFIRMED @ 0x00423342/0x0042335C]: offset -0x120 from canvas, width 0x120/0x60 */
            frontend_create_button(save_msg, FE_CENTER_X - 0x90, FE_CENTER_Y - 0x5F, 0x120, 0x20);
            frontend_create_button(SNK_OkButTxt,     FE_CENTER_X - 0x30, FE_CENTER_Y + 0x31, 0x60,  0x20);
        }
        s_anim_tick = 0;
        s_inner_state = 0x12;
        break;

    case 0x12: /* Save confirmation slide-in: 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x13;
        }
        break;

    case 0x13: /* Save confirmation wait */
        if (s_input_ready && s_button_index >= 0) {
            s_inner_state = 0x14;
        }
        break;

    case 0x14: /* Save confirmation slide-out: 32 frames -> back to menu */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            s_inner_state = 0x0D; /* return to post-results menu */
        }
        break;

    case 0x15: /* Masters series progression */
        TD5_LOG_D(LOG_TAG, "RaceResults: Masters progression");
        s_results_rerace_flag = 1;
        s_results_skip_display = 1;
        /* Go to car selection for next Masters race */
        td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
        break;
    }
}

/* ========================================================================
 * [25] ScreenPostRaceNameEntry (0x413BC0)
 * States: 13 (0x00 - 0x0C)
 * ======================================================================== */

static void Screen_PostRaceNameEntry(void) {
    switch (s_inner_state) {
    case 0: /* Qualification check */
        /* [CONFIRMED @ 0x00413BC0 case 0] */
        frontend_init_return_screen(TD5_SCREEN_NAME_ENTRY);
        TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: qualification check, game_type=%d",
                  s_selected_game_type);

        /* Snapshot speeds NOW while the actor pool is still alive — by the
         * time case 4 fires (after name entry slide-in/typing/slide-out OR
         * the direct !qualifies jump) the race teardown may have invalidated
         * td5_game_get_actor(0), which would zero the avg/top columns in
         * the inserted entry (user-reported 2026-05-26). */
        s_post_race_top_speed = td5_game_get_result_top_speed(0);
        s_post_race_avg_speed = td5_game_get_result_avg_speed(0);

        /* Compute group index for the high-score table.
         * Cup types 1-6: group = game_type + 0x13 (mirroring original case 0 at 0x413BCF).
         * Drag (type 7):  group = 0x13.
         * Others:         group = s_selected_track (direct schedule index). */
        {
            int group_idx;
            if (s_selected_game_type == 7) {
                group_idx = 0x13;
            } else if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                group_idx = s_selected_game_type + 0x13;
            } else {
                group_idx = s_selected_track;
            }
            group_idx = (group_idx < 0) ? 0 : (group_idx >= 26 ? 25 : group_idx);

            const TD5_NpcGroup *grp = td5_save_get_npc_group(group_idx);
            int group_type = grp ? (grp->header & 3) : 0;

            /* Derive player's score for this group type:
             * 0 = TIME  (primary metric = finish time ticks, lower is better)
             * 1 = LAP   (best lap time ticks, lower is better)
             * 2 = PTS   (secondary metric = points, higher is better)
             * [CONFIRMED @ 0x00413BCF-0x00413C5C] */
            s_post_race_score = 0;
            if (group_type == 0) {
                /* Time (primary finish metric) */
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    /* Cup: use s_results secondary lap time field
                     * [CONFIRMED @ 0x00413C0B]: DAT_0048d990 for cup types */
                    s_post_race_score = td5_game_get_result_secondary(0);
                } else {
                    s_post_race_score = td5_game_get_result_primary(0);
                }
            } else if (group_type == 1) {
                /* Lap time: best lap across all slots */
                s_post_race_score = td5_game_get_best_lap_time(0);
            } else if (group_type == 2) {
                /* Points: secondary metric */
                s_post_race_score = td5_game_get_result_secondary(0);
            }

            /* Qualification check: compare against worst entry (entries[4].score).
             * Time-based (types 0/1): score of 0 = not finished = disqualified.
             *   Qualifies if player_time < worst_time.
             * Points-based (type 2): qualifies if player_pts > worst_pts.
             * [CONFIRMED @ 0x00413C5E-0x00413C7E] */
            int qualifies = 0;
            if (s_post_race_score != 0 && grp != NULL) {
                int32_t worst = grp->entries[4].score;
                if (group_type < 2) {
                    /* time: lower is better; qualify if player < worst */
                    qualifies = (s_post_race_score < worst);
                } else {
                    /* points: higher is better; qualify if player > worst */
                    qualifies = (s_post_race_score > worst);
                }
            }

            /* 2P mode: player 2 result doesn't go into high score.
             * DQ (slot state not finished): no entry.
             * [CONFIRMED @ 0x00413C80-0x00413CA4] */
            if (s_two_player_mode) qualifies = 0;
            if (!td5_game_slot_is_finished(0)) qualifies = 0;

            TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: group=%d type=%d score=%d qualifies=%d",
                      group_idx, group_type, (int)s_post_race_score, qualifies);

            /* DEV harness: TD5RE_DEMO_NAMEENTRY=1 forces the name-prompt phase so the
             * name-input widget can be frame-dumped without finishing a qualifying race.
             * Inert unless the env var is set. */
            {
                static int s_demo_ne_init = 0, s_demo_ne = 0;
                if (!s_demo_ne_init) { s_demo_ne_init = 1;
                    const char *e = getenv("TD5RE_DEMO_NAMEENTRY"); s_demo_ne = (e && e[0] && e[0] != '0'); }
                if (s_demo_ne) { qualifies = 1; if (s_post_race_score == 0) s_post_race_score = 1; }
            }

            if (!qualifies) {
                /* Skip name entry — go straight to table insert (no name prompt) */
                s_post_race_score = 0;
                s_inner_state = 4;
                break;
            }
        }

        /* Player qualifies: prompt for name */
        memset(s_post_race_name, 0, sizeof(s_post_race_name));
        strcpy(s_post_race_name, "PLAYER");
        frontend_begin_text_input(s_post_race_name, (int)sizeof(s_post_race_name));
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            s_anim_tick = 0;
            s_inner_state = 2;
        }
        break;

    case 2: /* Text input active — process keys here; widget DRAWN in render path
             * (td5_frontend_render_ui_rects NAME_ENTRY dispatch). */
        frontend_handle_text_input_key();
        if (frontend_text_input_confirmed()) {
            /* Copy entered name, or fallback to default */
            frontend_play_sfx(5);
            s_inner_state = 3;
        }
        break;

    case 3: /* Slide-out of input: 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
            s_anim_tick = 0;
            s_inner_state = 4;
        }
        break;

    case 4: /* Insert score into table */
        /* [CONFIRMED @ 0x00413CB0 case 4] ScreenPostRaceNameEntry:
         * 1. Scan entries[0..4].score to find insert position (uVar8).
         *    - Types 0/1: find first entry where player_score <= entry.score (insert before)
         *    - Type 2:    find first entry where player_score >= entry.score (insert before)
         * 2. Shift entries[uVar8..3] down one slot (memmove-style).
         * 3. Write entry at position uVar8: name, score, car_id, avg_speed, top_speed.
         * 4. s_score_insert_pos = uVar8.
         */
        if (s_post_race_score != 0) {
            /* Determine group and type — mirrors case 0 logic */
            int ins_group;
            if (s_selected_game_type == 7) {
                ins_group = 0x13;
            } else if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                ins_group = s_selected_game_type + 0x13;
            } else {
                ins_group = s_selected_track;
            }
            ins_group = (ins_group < 0) ? 0 : (ins_group >= 26 ? 25 : ins_group);

            TD5_NpcGroup *grp = td5_save_get_npc_group_mutable(ins_group);
            if (grp != NULL) {
                int group_type = grp->header & 3;

                /* Find insert position [CONFIRMED @ 0x00413CB5-0x00413CDA] */
                int ins_pos = 5; /* default: past end (discard) */
                for (int k = 0; k < 5; k++) {
                    if (group_type < 2) {
                        /* Time: lower is better; insert where player < entry */
                        if (s_post_race_score <= grp->entries[k].score) {
                            ins_pos = k; break;
                        }
                    } else {
                        /* Points: higher is better; insert where player >= entry */
                        if (s_post_race_score >= grp->entries[k].score) {
                            ins_pos = k; break;
                        }
                    }
                }

                if (ins_pos < 5) {
                    /* Shift entries[ins_pos..3] down one slot [CONFIRMED @ 0x00413CDB-0x00413D1B] */
                    for (int k = 3; k >= ins_pos; k--) {
                        grp->entries[k + 1] = grp->entries[k];
                    }

                    /* Write new entry [CONFIRMED @ 0x00413D1C-0x00413D71]:
                     * name (13 bytes), score, car_id, avg_speed, top_speed */
                    TD5_NpcEntry *e = &grp->entries[ins_pos];
                    memset(e, 0, sizeof(*e));
                    strncpy(e->name, s_post_race_name, sizeof(e->name) - 1);
                    e->score = s_post_race_score;
                    e->car_id = (int32_t)(uint8_t)s_selected_car;

                    /* Average and top speed.
                     * Non-cup: direct from slot 0 metrics.
                     * Cup: avg_speed = total / race count [CONFIRMED @ 0x00413D55-0x00413D70] */
                    if (s_selected_game_type < 1 || s_selected_game_type == 7) {
                        e->avg_speed = s_post_race_avg_speed;
                        e->top_speed = s_post_race_top_speed;
                    } else {
                        int race_count = (s_race_within_series > 0) ? s_race_within_series : 1;
                        e->avg_speed = s_post_race_avg_speed / race_count;
                        e->top_speed = s_post_race_top_speed;
                    }
                    s_score_insert_pos = ins_pos;
                    /* Point the shared score-overlay at the just-inserted
                     * group so the post-submit display (cases 5-12) renders
                     * the user's new entry. Without this the overlay would
                     * show whatever group was last viewed in the Records
                     * screen (initialized to 0 at HIGH_SCORE entry). */
                    s_score_category_index = ins_group;
                    /* Unblock the overlay's `!s_anim_complete` early-return so
                     * NAME_ENTRY cases 6+ can render the high-score table.
                     * Without this the screen stays blank between insert and
                     * slide-out (user-reported 2026-05-26). */
                    s_anim_complete = 1;

                    TD5_LOG_I(LOG_TAG,
                              "PostRaceNameEntry: inserted '%s' score=%d at pos=%d in group=%d",
                              e->name, (int)e->score, ins_pos, ins_group);
                } else {
                    TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: score=%d doesn't fit (insert_pos=%d)",
                              (int)s_post_race_score, ins_pos);
                }
            }
        } else {
            TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: case 4 skip (score=0, no qualification)");
        }
        s_inner_state = 5;
        break;

    case 5: case 6: /* Present, draw score table */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 7: /* Score table slide-in: 39 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x12) {
            s_inner_state = 8;
        }
        break;

    case 8: case 9: /* Static display (2 frames) */
        s_inner_state = 10;
        break;

    case 10: /* Interactive: auto-select OK, wait for confirm */
        if (s_input_ready) {
            s_inner_state = 11;
        }
        break;

    case 11: /* Prep slide-out */
        s_anim_tick = 0;
        s_inner_state = 12;
        break;

    case 12: /* Slide-out: 16 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 16) {
            /* Persist high-score table to Config.td5.
             * [CONFIRMED @ 0x413BC0 case 4]: original writes into g_npcRacerGroupTable
             * (part of the Config block serialized by WritePackedConfigTd5 @ 0x40F8D0).
             * Port's case 4 already updated the in-memory NpcGroup; we flush here so
             * the entry survives across sessions. */
            TD5_LOG_I(LOG_TAG, "PostRaceNameEntry: persisting high score to Config.td5");
            td5_save_write_config(NULL);

            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* ========================================================================
 * [26] ScreenCupFailedDialog (0x4237F0)
 * States: 6. "Sorry, You Failed To Win [Race Type]"
 * Only for cup types 1-6; others redirect to main menu.
 * ======================================================================== */

static void Screen_CupFailed(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        /* Only for cup types 1-6 */
        if (s_selected_game_type < 1 || s_selected_game_type > 6) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }

        frontend_init_return_screen(TD5_SCREEN_CUP_FAILED);
        TD5_LOG_D(LOG_TAG, "CupFailed: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198x0x70 (408x112) rendered live in
         * frontend_render_cup_failed_overlay (called from render_ui_rects).
         * Original: CreateTrackedFrontendSurface(0x198,0x70) @ DAT_0049628c,
         * then DrawFrontendLocalizedStringToSurface x4 for:
         *   SNK_SorryTxt     y=0x00 ("SORRY")           [CONFIRMED Language.dll]
         *   SNK_YouFailedTxt y=0x1c ("YOU FAILED")       [CONFIRMED Language.dll]
         *   SNK_ToWinTxt     y=0x38 ("TO WIN")           [CONFIRMED Language.dll]
         *   SNK_RaceTypeText y=0x54 ([cup type name])    [CONFIRMED @ 0x4237F0]
         * [CONFIRMED @ ScreenCupFailedDialog 0x004237F0] */
        /* [FIXED 2026-06-02, decomp @0x4237F0] OK rests at (296,289) — slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x31) = (296, 289). 408x112 panel @(152,97). */
        frontend_create_button(SNK_OkButTxt, 296, 289, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x4237F0 case 4: anim==0x20 exit] */
        s_anim_tick++;
        /* Dialog slides from right (24px/frame), button from left */
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm */
        if (s_input_ready) {
            /* Release surfaces, go to return screen (typically main menu) */
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* ========================================================================
 * [27] ScreenCupWonDialog (0x423A80)
 * States: 6. "Congratulations, You Have Won [Race Type]"
 * Deletes CupData.td5. May show unlock info.
 * ======================================================================== */

static void Screen_CupWon(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        if (s_selected_game_type < 1 || s_selected_game_type > 6) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
            return;
        }

        frontend_init_return_screen(TD5_SCREEN_CUP_WON);
        TD5_LOG_D(LOG_TAG, "CupWon: init -- deleting CupData.td5");
        frontend_delete_cup_data();

        /* Apply cup unlock progression and save to Config.td5.
         * Original AwardCupCompletionUnlocks checks companion_state_2 == 1 (player
         * finished, not DNF) before proceeding. [CONFIRMED @ 0x00421da0]
         *
         * Lifetime contract: s_cup_won_car_count / s_cup_won_track_count are
         * latched HERE in state 0 (or left at zero on DNF) and read by
         * frontend_render_cup_won_overlay during states 4-5 (slide-in + wait).
         * They persist via static-zero-init until the next Screen_CupWon
         * entry; td5_frontend_set_screen(MAIN_MENU) in state 5 leaves them
         * stale, but no other screen reads them so the staleness is inert.
         * [CONFIRMED @ 0x00423A80]: DAT_00494bb0 = car count, DAT_00494bb4 = track count. */
        s_cup_won_car_count   = 0;
        s_cup_won_track_count = 0;
        {
            int new_unlocks = 0;
            if (td5_game_slot_is_finished(0)) {
                new_unlocks = td5_save_apply_cup_unlocks_ex((int)s_selected_game_type,
                                                            &s_cup_won_car_count,
                                                            &s_cup_won_track_count);
            }
            TD5_LOG_I(LOG_TAG, "CupWon: game_type=%d finished=%d new_unlocks=%d cars=%d tracks=%d",
                      (int)s_selected_game_type, td5_game_slot_is_finished(0),
                      new_unlocks, s_cup_won_car_count, s_cup_won_track_count);

            /* Persist updated unlock state */
            td5_save_write_config(NULL);

            /* Refresh frontend lock tables from save system */
            td5_save_get_car_lock_table(s_car_lock_table, TD5_BASE_CAR_COUNT);
            td5_save_get_track_lock_table(s_track_lock_table, 26);
            { int td6s; for (td6s = 26; td6s <= 36; td6s++) s_track_lock_table[td6s] = 0; } /* TD6 tracks always available */
            if (td5_save_get_all_cars_unlocked()) {
                s_total_unlocked_cars = 37;
            } else {
                s_total_unlocked_cars = td5_save_get_max_unlocked_car();
                if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21;
            }
            {
                int t;
                s_total_unlocked_tracks = 20;
                for (t = 20; t < 37; t++) {
                    if (s_track_lock_table[t] == 0)
                        s_total_unlocked_tracks = t + 1;
                }
            }
        }

        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198×0xC4 (408×196) rendered live in frontend_render_cup_won_overlay.
         * [CONFIRMED @ 0x00423AEB/AF0]: CreateTrackedFrontendSurface(0x198, 0xC4) */
        /* [FIXED 2026-06-02, decomp @0x423A80] OK rests at (296,337) — it slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x61) = (296, 337); the taller
         * 408x196 panel pushes it below the 408x112 dialogs' (296,289). Port auto-layout didn't match. */
        frontend_create_button(SNK_OkButTxt, 296, 337, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x00423D35: anim==0x20 exit, +1/frame] */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm */
        if (s_input_ready) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

/* ========================================================================
 * [28] ScreenStartupInit (0x415370)
 * States: 5. First screen on game launch.
 * ======================================================================== */

static void Screen_StartupInit(void) {
    switch (s_inner_state) {
    case 0: /* Create small surface, show OK button */
        frontend_init_return_screen(TD5_SCREEN_STARTUP_INIT);
        TD5_LOG_D(LOG_TAG, "StartupInit: state 0");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button(SNK_OkButTxt, -100, 0, 100, 0x20);
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present blank (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Release surface, redirect to ScreenLocalizationInit */
        TD5_LOG_I(LOG_TAG, "StartupInit: redirecting to ScreenLocalizationInit");
        td5_frontend_set_screen(TD5_SCREEN_LOCALIZATION_INIT);
        break;
    }
}

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

static void Screen_SessionLocked(void) {
    switch (s_inner_state) {
    case 0: /* Init */
        frontend_init_return_screen(TD5_SCREEN_SESSION_LOCKED);
        TD5_LOG_D(LOG_TAG, "SessionLocked: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Dialog 0x198x0x70 (408x112) rendered live in
         * frontend_render_session_locked_overlay (called from render_ui_rects).
         * Original: identical structure to CupFailed — CreateTrackedFrontendSurface,
         * then DrawFrontendLocalizedStringToSurface x2 for:
         *   SNK_SorryTxt     y=0x00 ("SORRY")          [CONFIRMED Language.dll]
         *   SNK_SeshLockedTxt y=0x38 ("SESSION LOCKED") [CONFIRMED Language.dll]
         * [CONFIRMED @ ScreenSessionLockedDialog 0x0041D630] */
        /* [FIXED 2026-06-02, decomp @0x41D630] OK rests at (296,289) — slides via
         * MoveFrontendSpriteRect(0,(halfW-0x318)+0x20*0x18, halfH+0x31) = (296, 289), same as CupFailed. */
        frontend_create_button(SNK_OkButTxt, 296, 289, 0x60, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames [CONFIRMED @ 0x41D630 case 4: anim==0x20 exit] */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 5;
        }
        break;

    case 5: /* Wait for confirm -> main menu */
        if (s_input_ready) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
        break;
    }
}

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
