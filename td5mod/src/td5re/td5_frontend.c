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
#include "td5_game.h"
#include "td5_input.h"
#include "td5_physics.h"
#include "td5_net.h"
#include "td5_platform.h"
#include "td5_render.h"
#include "td5_save.h"
#include "td5_sound.h"
#include "td5re.h"
#include "../../ddraw_wrapper/src/wrapper.h"

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
static int  s_input_ready;              /* DAT_004951e8            */
static int  s_button_index;             /* currently pressed button */
static int  s_arrow_input;              /* DAT_0049b690 arrow direction */
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
/* Split-screen display mode: 0=off, 1=on [CONFIRMED @ 0x420C70 g_twoPlayerSplitMode] */
static int  s_split_screen_mode;           /* g_twoPlayerSplitMode   */

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
static int  s_score_category_index;    /* DAT_00497a68: current track in score table */

#define FE_MAX_DISPLAY_MODES 64
static TD5_DisplayMode s_display_modes[FE_MAX_DISPLAY_MODES];
static char            s_display_mode_names[FE_MAX_DISPLAY_MODES][32];
static int             s_display_mode_count;
static int             s_display_mode_index;
static int             s_display_fog_enabled = 1;
static int             s_display_speed_units;
static int             s_display_camera_damping = 5;
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

/* Lock tables (simplified inline representation) */
static uint8_t s_car_lock_table[37];    /* DAT_00463e4c */
static uint8_t s_track_lock_table[26];  /* DAT_004668B0 */
static int  s_total_unlocked_cars;      /* DAT_00463e0c */
static int  s_total_unlocked_tracks;    /* DAT_00466840 */
static int  s_cheat_unlock_all;         /* DAT_00496298 */

/* Network state */
static int  s_network_active;           /* g_networkSessionActive / DAT_004962bc */
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
static int  s_results_skip_display;     /* DAT_00497a74 */

/* Race snapshot for re-race */
static int  s_snap_car, s_snap_paint, s_snap_trans, s_snap_config;

/* Post-race name entry state (Screen [25]) */
static int32_t s_post_race_score;       /* DAT_004951d0: player's score for qualification */
static int     s_score_insert_pos;      /* 0-4: position in 5-entry table where insert goes */
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
    /* [5] Ultimate     (GT=6) — RE found no schedule row; handled by jump table
     *                  callback @ 0x4110A0 (task #2). Placeholder = Masters order. */
    {  1,  2,  3, 15,  8, 11, 13, 10, 12,  9, 14, 99, -1 },
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
static int  s_fade_active;
static int  s_fade_progress;     /* 0..255 */
static int  s_fade_direction;    /* 1 = in, -1 = out */
static int  s_fade_table_index;
static int  s_gallery_pic_index;
static int  s_gallery_pic_surface;
static int  s_gallery_visited_mask;

/* Background gallery slideshow (LoadExtrasGalleryImageSurfaces / UpdateExtrasGalleryDisplay)
 * pic1-5.tga from Extras.zip cycle as a semi-transparent overlay during frontend navigation. */
typedef struct { int width; int height; } BgGalImg;
static BgGalImg s_bg_gallery[5];
static int   s_bg_gal_loaded;
static int   s_bg_gal_current;
static int   s_bg_gal_blend;
static float s_bg_gal_x, s_bg_gal_y;

static int  s_control_options_surface;
static int  s_sound_icon_surface = 0;       /* Stereo.tga (stereo icon, Sound Options) */
static int  s_sound_icon_mono_surface = 0;  /* Mono.tga   (mono icon,   Sound Options) */
static int  s_sound_volumebox_surface = 0;  /* VolumeBox.tga   (volume bar background) */
static int  s_sound_volumefill_surface = 0; /* VolumeFill.tga  (volume bar fill)       */
static int  s_split_screen_surface = 0;     /* SplitScreen.tga (Two Player layout preview)    */
static int  s_joypad_icon_surface = 0;      /* JoypadIcon.tga   (64x32 gamepad icon)   */
static int  s_joystick_icon_surface = 0;    /* JoystickIcon.tga (64x32 joystick icon)  */
static int  s_keyboard_icon_surface = 0;    /* KeyboardIcon.tga (64x32 keyboard icon)  */
static int  s_nocontroller_surface = 0;     /* NoControllerText.tga (376x20 warning)   */
static int  s_car_preview_prev_surface;
static int  s_car_preview_next_surface;

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
static char s_car_spec[17][48]; /* config.nfo fields (0-16) for stats sub-screen */
static int  s_car_spec_car = -1; /* which car index is currently cached */
static int s_track_preview_surface = 0;
static int s_track_switch_tick = 16; /* 0-15 = animating in, 16 = settled */
static int s_font_page = -1;
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

/* Forward declarations for functions used before their definitions */
static int frontend_load_tga(const char *name, const char *archive);
static int frontend_load_tga_colorkey(const char *name, const char *archive,
                                      int page_override, int *w_out, int *h_out,
                                      TD5_ColorKeyMode colorkey);
static int frontend_load_surface_keyed(const char *name, const char *archive, TD5_ColorKeyMode colorkey);
static int frontend_track_level_exists(int track_index);
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

/* UI order matches original binary table at 0x00463e24 (DO NOT REORDER) */
static const char *s_car_zip_paths[37] = {
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
};

static const char *s_track_display_names[26] = {
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
    "TRACK 21",
    "TRACK 22",
    "TRACK 23",
    "TRACK 24",
    "TRACK 25",
    "TRACK 26"
};
/* Original binary order: DAT_00466894 (slot→SNK_TrackNames index) cross-referenced
 * with Language.dll SNK_TrackNames → city name → s_track_display_names index.
 * Unlocked slots 0-7: Moscow, Edinburgh, Sydney, Blue Ridge, Jarash, Newcastle,
 *                     Maui, Courmayeur.
 * Locked slots 8-15: Honolulu, Tokyo, Keswick, San Francisco, Bern, Kyoto,
 *                    Washington, Munich.
 * Championship-only slots 16-19: Cheddar Cheese, Montego Bay, House of Bez,
 *                                Drag Strip. */
static const uint8_t s_track_schedule_to_name_index[20] = {
     8, 10, 12,  9,  6,  3,  4,  5, 13, 11,
    19, 18, 17, 16, 15, 14,  7,  1,  2,  0
};
/* Original binary gScheduleToPoolIndex (DAT_00466894): maps schedule slot →
 * Language.dll pool index, which equals the trak TGA file number.
 * Derived from listing at 0x466894 in TD5_d3d.exe. */
static const uint8_t s_track_schedule_to_tga_index[20] = {
    11,  9,  7, 10, 13, 16, 15, 14,  6,  8,
     0,  1,  2,  3,  4,  5, 12, 18, 17, 19
};

static char s_car_display_names[37][64];
static uint8_t s_car_display_names_loaded[37];

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
    case TD5_SCREEN_OPTIONS_HUB: return "OptionsText.tga";
    case TD5_SCREEN_CAR_SELECTION: return "SelectCarText.tga";
    case TD5_SCREEN_TRACK_SELECTION: return "TrackSelectText.TGA";
    case TD5_SCREEN_HIGH_SCORE: return "HighScoresText.TGA";
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
    case TD5_SCREEN_OPTIONS_HUB: return FE_TITLE_PAGE_BASE + 3;
    case TD5_SCREEN_CAR_SELECTION: return FE_TITLE_PAGE_BASE + 4;
    case TD5_SCREEN_TRACK_SELECTION: return FE_TITLE_PAGE_BASE + 5;
    case TD5_SCREEN_HIGH_SCORE: return FE_TITLE_PAGE_BASE + 6;
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

static int frontend_load_tga(const char *name, const char *archive) {
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
        if (!td5_asset_load_png_to_buffer(png_path, TD5_COLORKEY_NONE, &pixels, &w, &h))
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
            /* Font page also needs black keying */
            if (dest_page == SHARED_PAGE_FONT) {
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
    char line[64];
    if (car_index < 0 || car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))
        return fallback;
    if (s_car_display_names_loaded[car_index]) return s_car_display_names[car_index];
    if (frontend_load_text_line_from_archive("config.nfo", s_car_zip_paths[car_index], line, sizeof(line))) {
        frontend_copy_pretty_text(s_car_display_names[car_index], sizeof(s_car_display_names[car_index]), line);
    } else {
        const char *zip = strrchr(s_car_zip_paths[car_index], '/');
        frontend_copy_pretty_text(s_car_display_names[car_index], sizeof(s_car_display_names[car_index]),
                                  zip ? zip + 1 : s_car_zip_paths[car_index]);
        {
            char *dot = strrchr(s_car_display_names[car_index], '.');
            if (dot) *dot = '\0';
        }
    }
    s_car_display_names_loaded[car_index] = 1;
    return s_car_display_names[car_index][0] ? s_car_display_names[car_index] : fallback;
}

static int frontend_current_car_index(void) {
    if (s_selected_game_type == 5 &&
        s_selected_car >= 0 &&
        s_selected_car < (int)(sizeof(s_masters_roster) / sizeof(s_masters_roster[0])))
        return s_masters_roster[s_selected_car];
    return s_selected_car;
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
    int highlight_ramp;     /* 0-6: smooth highlight fade (original uses 6-step ramp) */
    int is_selector;        /* 1 = left/right selector widget; always uses blue 9-slice */
    char label[64];
} FE_Button;

static FE_Button s_buttons[FE_MAX_BUTTONS];
static int s_button_count;
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
            s_buttons[i].highlight_ramp = 0;
            s_buttons[i].is_selector = 0;

            /* Determine width: if negative x, the absolute value is the width
             * (original convention). Otherwise use explicit w, defaulting to 224. */
            if (x < 0) {
                s_buttons[i].w = (-x > 0) ? -x : 224;
            } else {
                s_buttons[i].w = (w > 0 && w != 200) ? w : 224;
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

static void frontend_set_cursor_visible(int visible) {
    /* Original menu states pass 0 while interactive and 1 during transitions. */
    s_cursor_visible = !visible;
}

static void frontend_render_cursor(void); /* forward decl — impl after draw queue types */

static void frontend_play_sfx(int id) { td5_sound_play_frontend_sfx(id); }

static void frontend_cd_play(int track) {
    td5_plat_cd_play(track + 2);
}

/* --- Text Rendering (simple bitmap font) --- */

/*
 * frontend_draw_string / frontend_draw_small_string
 *
 * Original binary draws into an offscreen DirectDraw surface via
 * DrawFrontendLocalizedStringToSurface @ 0x00424560 (__cdecl, params:
 *   (byte *str, int x, int y, int *surface)).
 * The port has no offscreen surfaces for dialog boxes; dialog text is
 * rendered live in td5_frontend_render_ui_rects via dedicated overlay
 * functions (frontend_render_cup_failed_overlay etc.).
 *
 * These stubs exist for call-site compatibility only — they are intentionally
 * no-ops because all dialog text paths have been moved to the render overlay.
 * [CONFIRMED: call sites removed from screen state machines in favour of
 *  render-side overlays added 2026-04-25]
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

static void frontend_render_cursor(void) {
    if (!s_cursor_visible) return;
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
    (void)color;
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
        cmd->color = (alpha << 24) | 0x000000;
        cmd->tex_page = -1;
    }
    return 0;
}

/*
 * Maps external car-id / cup-schedule slot index → s_car_zip_paths index.
 * Mirrors DAT_00463E24[37] from the original binary (confirmed by RE).
 * Quick-race default: all cup_schedule_track[i] = 0 → type index 7 (XKR).
 */
static const int s_ext_car_to_type_index[37] = {
     7,  2, 17, 33, 22, 31, 32, 34, 18, 14,
     1, 15, 13,  9, 11,  5,  0, 35,  8,  3,
     4, 12, 26, 10, 36, 16, 19, 25, 20, 23,
     6, 24, 21, 30, 28, 27, 29
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

static void frontend_init_race_schedule(void) {
    int i;
    int slot_active[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_ext_id[TD5_MAX_RACER_SLOTS]  = {0};
    int slot_variant[TD5_MAX_RACER_SLOTS] = {0};
    int start_slot = 1;

    g_td5.race_requested = 1;
    g_td5.car_index   = frontend_current_car_index();
    g_td5.track_index = (s_current_screen == TD5_SCREEN_ATTRACT_MODE)
                        ? s_attract_track
                        : s_selected_track;

    /* Slot 0 = player, always active */
    slot_active[0]  = 1;
    slot_ext_id[0]  = s_selected_car;
    slot_variant[0] = s_selected_paint;

    /* Two-player setup [CONFIRMED @ 0x0040daf0]:
     * Original gate: g_twoPlayerModeEnabled != 0 || g_selectedGameType == 7.
     * In the original, game_type 7 is DRAG RACE (user picks a 2nd car via the
     * 2-pass CarSelect loop). The port's convention uses game_type 9 for drag
     * race, so the constant is swapped here. Time Trials is solo and must NOT
     * fall into this branch. */
    if (s_two_player_mode || s_selected_game_type == 9) {
        slot_active[1]  = 1;
        slot_ext_id[1]  = s_p2_car;
        slot_variant[1] = 0;
        start_slot = 2;
        TD5_LOG_I(LOG_TAG, "InitRaceSchedule: P2 slot1 ext_id=%d", s_p2_car);
    }

    /* RNG state for AI ext_id picks.
     *
     * Original path (quickrace hook, non-trace):
     *   - InitializeFrontendResourcesAndState @ 0x00414740 calls srand(timeGetTime())
     *     TWICE and then consumes at least one rand() via a `rand() % 7` CD-track
     *     pick loop. That advances the CRT _holdrand to a time-dependent state.
     *   - InitializeRaceSeriesSchedule @ 0x0040dac0 itself does NOT call srand;
     *     it only STORES timeGetTime() into a global (g_randomSeedForRace).
     *   - AI car picks consume _holdrand from that point.
     *
     * Under /diff-race the Frida trace script seeds g_randomSeedForRace /
     * g_raceSessionRandomSeed to 0x1A2B3C4D but does NOT touch CRT _holdrand
     * (per-thread TLS offset unknown — see tools/frida_race_trace.js:40). So
     * the original's AI picks are still driven by timeGetTime(), and the
     * Frida-captured sequence ({1,0,3,4,5} in 2026-04-20 memory) is one
     * non-deterministic sample.
     *
     * Port strategy:
     *   - Outside /diff-race: srand(timeGetTime()) to get per-launch variety
     *     and burn one rand() to approximate the original's CD-track pick.
     *   - Under /diff-race (race_trace_enabled=1): srand with a fixed,
     *     documented seed. This guarantees repeatable AI picks run-to-run
     *     AND — paired with a matching Frida-side srand hook in a future
     *     pass — lets both sides reach zero-delta. Without that Frida
     *     companion, the two sides still differ in absolute car IDs but the
     *     port's sequence is stable and comparable.
     *
     * [CONFIRMED @ 0x00414740, 0x0040dac0, 0x0042aa33 (the real srand via
     *  mislabeled __set_new_handler).] */
    if (g_td5.ini.race_trace_enabled) {
        srand(0x1A2B3C4D);
    } else {
        srand(timeGetTime());
    }
    /* Approximate the original's CD-track-pick rand() burn from
     * InitializeFrontendResourcesAndState @ 0x00414a78:
     *   do { rand() % 7; } while (== g_selectedCdTrackIndex);
     * With a fresh seed both sides start with cdTrack = -1 (default),
     * so the first rand() always exits the loop. One rand() consumed. */
    (void)rand();

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

    /* Store ext_ids directly as car indices.
     * s_car_zip_paths is indexed by ext_id (display order), NOT by the original
     * binary's gCarZipPathTable type_index. The s_ext_car_to_type_index conversion
     * is NOT applied here — it maps to the original binary's table ordering which
     * doesn't match the source port's reordered table. */
    for (i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
        if (slot_active[i] && slot_ext_id[i] >= 0 && slot_ext_id[i] < 37) {
            g_td5.ai_car_indices[i]  = slot_ext_id[i];
            g_td5.ai_car_variants[i] = slot_variant[i];
        } else {
            g_td5.ai_car_indices[i]  = 0; /* fallback: VIPER (ext_id 0) */
            g_td5.ai_car_variants[i] = 0;
        }
    }

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
    s_track_direction    = 0;

    /* Apply game options from INI */
    s_game_option_laps              = g_td5.ini.laps;
    s_game_option_checkpoint_timers = g_td5.ini.checkpoint_timers;
    s_game_option_traffic           = g_td5.ini.traffic;
    s_game_option_cops              = g_td5.ini.cops;
    s_game_option_difficulty        = g_td5.ini.difficulty;
    s_game_option_dynamics          = g_td5.ini.dynamics;
    s_game_option_collisions        = g_td5.ini.collisions;

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

static void frontend_refresh_display_option_labels(void) {
    if (s_button_count > 0) {
        strncpy(s_buttons[0].label, "Resolution", sizeof(s_buttons[0].label) - 1);
        s_buttons[0].label[sizeof(s_buttons[0].label) - 1] = '\0';
    }
    if (s_button_count > 1) {
        strncpy(s_buttons[1].label, "Fogging", sizeof(s_buttons[1].label) - 1);
        s_buttons[1].label[sizeof(s_buttons[1].label) - 1] = '\0';
    }
    if (s_button_count > 2) {
        strncpy(s_buttons[2].label, "Speed Readout", sizeof(s_buttons[2].label) - 1);
        s_buttons[2].label[sizeof(s_buttons[2].label) - 1] = '\0';
    }
    if (s_button_count > 3) {
        strncpy(s_buttons[3].label, "Camera Damping", sizeof(s_buttons[3].label) - 1);
        s_buttons[3].label[sizeof(s_buttons[3].label) - 1] = '\0';
    }
    if (s_button_count > 4) {
        strncpy(s_buttons[4].label, "OK", sizeof(s_buttons[4].label) - 1);
        s_buttons[4].label[sizeof(s_buttons[4].label) - 1] = '\0';
    }
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
        if (s_previous_screen == TD5_SCREEN_RACE_RESULTS) {
            return TD5_SCREEN_RACE_RESULTS;
        }
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
        int idx = frontend_find_display_mode_index(width, height, bpp);
        if (idx >= 0) {
            s_display_mode_index = idx;
        } else if (s_display_mode_index < 0 || s_display_mode_index >= s_display_mode_count) {
            s_display_mode_index = 0;
        }
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

    left_edge = (left_now && !s_prev_left_state);
    right_edge = (right_now && !s_prev_right_state);
    up_edge = (up_now && !s_prev_up_state);
    down_edge = (down_now && !s_prev_down_state);
    enter_edge = (enter_now && !s_prev_enter_state);
    if (left_now || right_now || up_now || down_now || enter_now) had_activity = 1;

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
     * a horizontal button row. */
    if (left_edge) {
        if (frontend_cycle_selected_button_horizontal(-1)) {
            frontend_play_sfx(2); s_selection_from_mouse = 0;
        }
    }
    if (right_edge) {
        if (frontend_cycle_selected_button_horizontal(1)) {
            frontend_play_sfx(2); s_selection_from_mouse = 0;
        }
    }
    if (up_edge) {
        if (frontend_cycle_selected_button_vertical(-1)) {
            frontend_play_sfx(2); s_selection_from_mouse = 0;
        } else {
            frontend_play_sfx(10);
        }
    }
    if (down_edge) {
        if (frontend_cycle_selected_button_vertical(1)) {
            frontend_play_sfx(2); s_selection_from_mouse = 0;
        } else {
            frontend_play_sfx(10);
        }
    }
    if (enter_edge) {
        if (s_selected_button >= 0 && s_selected_button < FE_MAX_BUTTONS &&
            s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
            s_button_index = s_selected_button;
            s_input_ready = 1;
            frontend_play_sfx(3);
            TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=keyboard",
                      s_button_index, s_buttons[s_button_index].label);
        } else {
            frontend_play_sfx(10);
        }
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
    int esc_now = td5_plat_input_key_pressed(0x01);
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
static void fe_draw_option_arrows(int btn_idx, float sx, float sy);
static void frontend_load_bg_gallery(void);
/* Forward declarations for dialog text overlay renderers */
static void frontend_render_legal_copyright_overlay(float sx, float sy);
static void frontend_render_cup_failed_overlay(float sx, float sy);
static void frontend_render_session_locked_overlay(float sx, float sy);

static void frontend_begin_text_input(char *buffer, int capacity) {
    memset(&s_text_input_ctx, 0, sizeof(s_text_input_ctx));
    if (!buffer || capacity <= 1) { s_text_input_state = 0; return; }
    buffer[capacity - 1] = '\0';
    s_text_input_ctx.buffer = buffer;
    s_text_input_ctx.capacity = capacity;
    s_text_input_ctx.caret = (int)strlen(buffer);
    s_text_input_ctx.blink_tick = td5_plat_time_ms();
    s_text_input_ctx.confirm_state = 0;
    s_text_input_state = 1;
    TD5_LOG_I(LOG_TAG, "Text input started: capacity=%d initial=\"%s\"", capacity, buffer);
}

static void frontend_commit_text_input(void) {
    s_text_input_ctx.confirm_state = 1;
    s_text_input_state = 2;
    TD5_LOG_I(LOG_TAG, "Text input confirmed: \"%s\"",
              s_text_input_ctx.buffer ? s_text_input_ctx.buffer : "");
}

static void frontend_handle_text_input_key(void) {
    int len;
    if (s_text_input_state != 1 || !s_text_input_ctx.buffer) return;
    if (!frontend_is_window_active()) return;
    len = (int)strlen(s_text_input_ctx.buffer);
    if (s_text_input_ctx.caret > len) s_text_input_ctx.caret = len;

    /* Enter = confirm */
    if (GetAsyncKeyState(VK_RETURN) & 1) { frontend_note_activity(); frontend_commit_text_input(); return; }

    /* Backspace */
    if ((GetAsyncKeyState(VK_BACK) & 1) && s_text_input_ctx.caret > 0) {
        memmove(&s_text_input_ctx.buffer[s_text_input_ctx.caret - 1],
                &s_text_input_ctx.buffer[s_text_input_ctx.caret],
                (size_t)(len - s_text_input_ctx.caret + 1));
        s_text_input_ctx.caret--;
        s_text_input_ctx.blink_tick = td5_plat_time_ms();
        frontend_note_activity();
    }

    /* Printable characters */
    {
        int shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
        int vk;
        for (vk = 0x30; vk <= 0x5A; vk++) {
            unsigned ch;
            if (!(GetAsyncKeyState(vk) & 1)) continue;
            len = (int)strlen(s_text_input_ctx.buffer);
            if (len >= s_text_input_ctx.capacity - 1) continue;
            ch = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7FFFu;
            if (ch < 32 || ch > 126) continue;
            if (!shift_down && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
            memmove(&s_text_input_ctx.buffer[s_text_input_ctx.caret + 1],
                    &s_text_input_ctx.buffer[s_text_input_ctx.caret],
                    (size_t)(len - s_text_input_ctx.caret + 1));
            s_text_input_ctx.buffer[s_text_input_ctx.caret] = (char)ch;
            s_text_input_ctx.caret++;
            s_text_input_ctx.blink_tick = td5_plat_time_ms();
            frontend_note_activity();
            TD5_LOG_I(LOG_TAG, "Text input char: '%c' -> \"%s\"",
                      (char)ch, s_text_input_ctx.buffer);
        }
    }

    /* Space */
    if (GetAsyncKeyState(VK_SPACE) & 1) {
        len = (int)strlen(s_text_input_ctx.buffer);
        if (len < s_text_input_ctx.capacity - 1) {
            memmove(&s_text_input_ctx.buffer[s_text_input_ctx.caret + 1],
                    &s_text_input_ctx.buffer[s_text_input_ctx.caret],
                    (size_t)(len - s_text_input_ctx.caret + 1));
            s_text_input_ctx.buffer[s_text_input_ctx.caret] = ' ';
            s_text_input_ctx.caret++;
            s_text_input_ctx.blink_tick = td5_plat_time_ms();
            frontend_note_activity();
            TD5_LOG_I(LOG_TAG, "Text input char: ' ' -> \"%s\"", s_text_input_ctx.buffer);
        }
    }
}

static void frontend_render_text_input(void) {
    float text_x, caret_x;
    if (!s_text_input_ctx.buffer) return;
    frontend_handle_text_input_key();

    /* Draw input box background */
    frontend_fill_rect(0, 118, 290, 404, 40, 0xFF8C8C8C);
    frontend_fill_rect(0, 120, 292, 400, 36, 0xE0101010);

    /* Draw text */
    text_x = 128.0f;
    fe_draw_text(text_x, 297.0f, s_text_input_ctx.buffer, 0xFFFFFFFF, 0.8f, 0.8f);

    /* Blinking caret */
    if (s_text_input_state == 1 &&
        (((td5_plat_time_ms() - s_text_input_ctx.blink_tick) / 350U) & 1U) == 0U) {
        caret_x = text_x + (float)s_text_input_ctx.caret * 11.2f;
        frontend_fill_rect(0, (int)caret_x, 297, 2, 22, 0xFFFFFFFF);
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
            s_total_unlocked_tracks = 26;
        } else {
            int t;
            td5_save_get_car_lock_table(s_car_lock_table, 37);
            td5_save_get_track_lock_table(s_track_lock_table, 26);
            if (td5_save_get_all_cars_unlocked()) {
                s_total_unlocked_cars = 37;
            } else {
                s_total_unlocked_cars = td5_save_get_max_unlocked_car();
                if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21;
            }
            s_total_unlocked_tracks = 20;
            for (t = 20; t < 26; t++) {
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

/* Placeholder: delete cup data file */
static void frontend_delete_cup_data(void) {
    td5_plat_file_delete("CupData.td5");
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
        default: /* 0, 8, 9: keep prior */ break;
    }
    TD5_LOG_I(LOG_TAG, "ConfigureGameTypeFlags: game_type=%d tier=%d",
              s_selected_game_type, g_td5.difficulty_tier);

    switch (s_selected_game_type) {
    case 0: /* Single Race -- user preferences apply */
        g_td5.circuit_lap_count = (s_game_option_laps + 1) * 2;
        g_td5.difficulty = (TD5_Difficulty)s_game_option_difficulty;
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

    case 7: /* Time Trials
             *
             * [DIVERGENCE from original @ 0x410CA0 case 7]
             * Original clears g_selectedGameType back to 0 after setting the
             * tier/overlay flags — time trial is identified at runtime purely
             * by (gRaceDifficultyTier==2 && !traffic && !encounter &&
             * g_raceOverlayPresetMode==3). The port keeps game_type=7 and
             * uses an explicit time_trial_enabled flag for clarity. No runtime
             * code currently tests game_type==7 for mode dispatch. */
        g_td5.time_trial_enabled = 1;
        g_td5.difficulty = TD5_DIFFICULTY_HARD;
        g_td5.traffic_enabled = 0;
        g_td5.special_encounter_enabled = 0;
        g_td5.circuit_lap_count = 1;           /* single lap on circuits */
        g_td5.checkpoint_timers_enabled = 1;   /* enable P2P checkpoint timers */
        td5_physics_set_collisions(0);         /* no collisions (solo) */
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

    g_td5.frontend_screen_index = (int)index;
    g_td5.frontend_inner_state = 0;
    g_td5.frontend_frame_counter = 0;

    /* Original SetFrontendScreen (0x00414610) is silent — per-screen state-0
     * code emits its own Play(N). Playing here doubled the main-menu Whoosh. */
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
 * ======================================================================== */

int td5_frontend_display_loop(void) {
    /* 0. Poll platform input so s_keyboard[] is fresh for this frame */
    {
        TD5_InputState dummy;
        td5_plat_input_poll(0, &dummy);
    }

    /* 1. Surface recovery (DDERR_SURFACELOST) */
    frontend_recover_surfaces();

    /* 2. Input polling (keyboard, mouse, joystick) */
    frontend_poll_input();

    /* 3. Screen dispatch -- call the active screen's state machine */
    if (s_current_screen >= 0 && s_current_screen < TD5_SCREEN_COUNT) {
        ScreenFn fn = s_screen_table[s_current_screen];
        if (fn) fn();
    }

    /* 4. Render flush (cursor queued after all other UI) */
    td5_frontend_render_ui_rects();
    frontend_render_cursor();
    td5_frontend_flush_sprite_blits();

    /* 5. Presentation (flip / software blit) */
    td5_plat_present(1);

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

    /* 9. Attract mode timeout: 60 seconds of idle on main menu -> demo screen */
    if (s_current_screen == TD5_SCREEN_MAIN_MENU) {
        uint32_t now = td5_plat_time_ms();
        if ((now - s_attract_idle_timestamp) >= 60000) {
            /* Pick a random track for the demo without corrupting the player's selection */
            s_attract_track = rand() % 8;
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
    td5_save_get_car_lock_table(s_car_lock_table, 37);
    td5_save_get_track_lock_table(s_track_lock_table, 26);

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
        for (t = 20; t < 26; t++) {
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
    /* Race Type menu: all buttons slide from left (original behavior).
     * Other screens: odd buttons from right, even from left. */
    float offscreen_x = (s_current_screen == TD5_SCREEN_RACE_TYPE_MENU) ? -640.0f :
                        (button_index & 1) ? 640.0f : -640.0f;

    if (!frontend_get_button_anim_state(&mode, &tick, &max_tick)) {
        /* Before the intro animation completes, keep buttons at their off-screen
         * starting position. This prevents a one-frame flash of the final button
         * layout before the slide-in animation begins. */
        if (!s_anim_complete && frontend_screen_has_button_anim())
            return offscreen_x;
        return base_x;
    }

    /* Use continuous s_anim_t for smooth sub-frame motion at any frame rate.
     * The integer s_anim_tick is only kept for game-logic compatibility. */
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
    if (out_x) *out_x = frontend_get_button_anim_x(button_index, (float)s_buttons[button_index].x) * sx;
    if (out_y) *out_y = (float)s_buttons[button_index].y * sy;
    if (out_w) *out_w = (float)s_buttons[button_index].w * sx;
    if (out_h) *out_h = (float)s_buttons[button_index].h * sy;
}

static float frontend_get_title_render_y(float sy) {
    int mode = FE_BUTTON_ANIM_NONE;
    int tick = 0;
    int max_tick = 0;
    float base_y = 12.0f;   /* resting Y (matches render code) */
    float hidden_y = -80.0f; /* above screen — title slides down from top */
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
    int car_locked;
    int track_locked;
    if (!s_anim_complete) return;
    snprintf(car_name, sizeof(car_name), "%s", frontend_get_car_display_name(s_selected_car));
    frontend_get_track_display_name(s_selected_track, 0, track_name, sizeof(track_name));
    car_locked = (!s_cheat_unlock_all && !s_network_active &&
                  s_selected_car >= 0 && s_selected_car < 37 &&
                  s_car_lock_table[s_selected_car] != 0);
    track_locked = (!s_cheat_unlock_all && !s_network_active &&
                    s_selected_track >= 0 && s_selected_track < 26 &&
                    s_track_lock_table[s_selected_track] != 0);
    frontend_draw_value_text(sx, sy, 140, 106, car_name, 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 140, 226, track_name, 0xFFFFFFFF);
    if (car_locked) frontend_draw_value_text(sx, sy, 398, 126, "LOCKED", 0xFFFF4444);
    if (track_locked) frontend_draw_value_text(sx, sy, 398, 246, "LOCKED", 0xFFFF4444);
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
    if (!s_buttons[btn_idx].active || s_arrowbuttonz_tex_page < 0) return;
    frontend_get_button_render_rect(btn_idx, sx, sy, &bx, &by, &bw, &bh);
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
    fe_draw_quad(bx + 4.0f * sx,          by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, 0.50f, 1.0f, 0.75f);
    fe_draw_quad(bx + bw - 4.0f*sx - aw, by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, 0.75f, 1.0f, 1.00f);
}

static void frontend_render_game_options_overlay(float sx, float sy) {
    const char *on_off[] = { "OFF", "ON" };
    const char *difficulty[] = { "EASY", "MEDIUM", "HARD" };
    const char *dynamics[] = { "SIMULATION", "ARCADE" };
    char laps[16];
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    snprintf(laps, sizeof(laps), "%d", (s_game_option_laps + 1) * 2);
    frontend_draw_value_centered(sx, sy, s_buttons[0].y + 6, laps, 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, on_off[s_game_option_checkpoint_timers & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[2].y + 6, on_off[s_game_option_traffic & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[3].y + 6, on_off[s_game_option_cops & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[4].y + 6, difficulty[s_game_option_difficulty % 3], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[5].y + 6, dynamics[s_game_option_dynamics & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[6].y + 6, on_off[s_game_option_collisions & 1], 0xFFFFFFFF);
}

static void frontend_render_display_options_overlay(float sx, float sy) {
    char damping[16];
    const char *mode_name = "UNAVAILABLE";
    const char *on_off[] = { "OFF", "ON" };
    const char *speed_read[] = { "MPH", "KPH" };
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    if (s_display_mode_count > 0 &&
        s_display_mode_index >= 0 &&
        s_display_mode_index < s_display_mode_count)
        mode_name = s_display_mode_names[s_display_mode_index];
    snprintf(damping, sizeof(damping), "%d", s_display_camera_damping);
    frontend_draw_value_centered(sx, sy, s_buttons[0].y + 6, mode_name, 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, on_off[s_display_fog_enabled & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[2].y + 6, speed_read[s_display_speed_units & 1], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[3].y + 6, damping, 0xFFFFFFFF);
}

static void frontend_render_sound_options_overlay(float sx, float sy) {
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    /* SFX Mode is indicated by the Stereo/Mono icon; no extra text needed.
     * Volume levels are indicated by bar fill only; no numbers. */

    /* Image positions from FUN_0041EA90 (640x480 absolute):
     * Stereo/Mono icon: x=394, y=97, w=64, h=32
     * VolumeBox SFX:  x=394, y=185, w=224, h=12
     * VolumeFill SFX: x=395, y=186, w=0-222, h=10
     * VolumeBox Mus:  x=394, y=225, w=224, h=12
     * VolumeFill Mus: x=395, y=226, w=0-222, h=10 */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);

    /* Stereo or Mono icon based on current mode */
    {
        int icon_surface = (s_sound_option_sfx_mode & 1) ? s_sound_icon_mono_surface : s_sound_icon_surface;
        if (icon_surface > 0) {
            int slot = icon_surface - 1;
            if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use)
                fe_draw_quad(394.0f * sx, 97.0f * sy, 64.0f * sx, 32.0f * sy,
                             0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
        }
    }

    /* Volume bars: SFX = button[1], Music = button[2]
     * Each bar sits to the right of its button at x=394, vertically
     * centred in the button height (32px). Bar=12px tall, fill=10px. */
    {
        int bar_btns[2]  = { 1, 2 }; /* SFX Volume, Music Volume */
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

static void frontend_render_two_player_options_overlay(float sx, float sy) {
    const char *on_off[] = { "OFF", "ON" };

    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    /* [CONFIRMED @ 0x420C70 case 4]: row 0 = split-screen ON/OFF; row 1 = catch-up ON/OFF
     * g_twoPlayerSplitMode is 0 or 1; DAT_00465ff8 is catch-up level (0..9, nonzero = ON) */
    frontend_draw_value_centered(sx, sy, s_buttons[0].y + 6, on_off[s_split_screen_mode ? 1 : 0], 0xFFFFFFFF);
    frontend_draw_value_centered(sx, sy, s_buttons[1].y + 6, on_off[(s_two_player_mode & 8) ? 1 : 0], 0xFFFFFFFF);

    /* [CONFIRMED @ 0x4210A4]: QueueFrontendOverlayRect with src_y = g_twoPlayerSplitMode << 5 (=*32)
     * SplitScreen.tga: 64x32 icon rows; row 0=off icon, row 1=on icon.
     * Blit position: x = canvasW/2+0x4a = 394, y = canvasH/2-0x8f = 97. */
    if (s_split_screen_surface > 0) {
        int slot = s_split_screen_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            int sh = s_surfaces[slot].height;
            if (sh > 0) {
                /* s_split_screen_mode is 0 or 1 — use directly, not via bit-mask */
                int   mode = s_split_screen_mode;  /* [CONFIRMED @ 0x420C70 g_twoPlayerSplitMode] */
                float v_row = 32.0f / (float)sh;
                float v0    = (float)mode * v_row;
                float v1    = v0 + v_row;
                td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
                fe_draw_quad(394.0f * sx, 97.0f * sy, 64.0f * sx, 32.0f * sy,
                             0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, v0, 1.0f, v1);
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            }
        }
    }
}

static void frontend_render_race_type_description(float sx, float sy) {
    static const char *k_race_type_lines[][4] = {
        { "Single Race",    "Race a single event", "with your current",   "garage and rules." },
        { "Cup Race",       "Enter a full cup and", "unlock harder tiers", "by finishing strong." },
        { "Continue Cup",   "Resume the last cup",  "saved to your profile", "if it is still valid." },
        { "Time Trials",    "Set the fastest lap",  "with traffic disabled", "and no rivals." },
        { "Drag Race",      "Line up for a short",  "head-to-head sprint", "down the strip." },
        { "Cop Chase",      "Outrun the cops in",   "a wanted-style chase", "with heavy pressure." },
        { "Back",           "Return to the main",   "menu without changing", "the current flow." }
    };
    static const char *k_cup_lines[][4] = {
        { "Championship",   "Starter cup with a",   "short schedule and",  "easier opponents." },
        { "Era",            "Classic-era roster",   "with a longer route", "across older tracks." },
        { "Challenge",      "A tougher series with", "denser traffic and",  "harder rivals." },
        { "Pitbull",        "Longer cup set with",  "more demanding races", "and AI pressure." },
        { "Masters",        "Advanced cup rules",   "with deep series",    "progression." },
        { "Ultimate",       "The final cup with",   "the hardest field and","full schedule." },
        { "Back",           "Go back to the race",  "type menu and pick",  "another event." }
    };
    const char *(*lines)[4];
    int desc_index = s_selected_button;
    float panel_x = 358.0f * sx;
    float panel_y = 145.0f * sy;
    float panel_w = 272.0f * sx;
    float panel_h = 180.0f * sy;

    if (!s_anim_complete) return;
    if (desc_index < 0 || desc_index > 6) desc_index = 0;
    if (s_inner_state >= 6 && s_inner_state <= 12) {
        lines = k_cup_lines;
    } else {
        lines = k_race_type_lines;
    }

    /* No background: original cleared surface to black with color-key transparency.
     * Title: large font (24px, sx/sy), white, centered. Y=0 in original surface.
     * Body:  small font (12px, 0.5 scale), white, centered. Y=32/44/56 in original surface. */
    fe_draw_text(panel_x + (panel_w - fe_measure_text(lines[desc_index][0], sx))        * 0.5f, panel_y +  2.0f * sy, lines[desc_index][0], 0xFFFFFFFF, sx,        sy);
    fe_draw_text(panel_x + (panel_w - fe_measure_text(lines[desc_index][1], sx * 0.5f)) * 0.5f, panel_y + 32.0f * sy, lines[desc_index][1], 0xFFFFFFFF, sx * 0.5f, sy * 0.5f);
    fe_draw_text(panel_x + (panel_w - fe_measure_text(lines[desc_index][2], sx * 0.5f)) * 0.5f, panel_y + 44.0f * sy, lines[desc_index][2], 0xFFFFFFFF, sx * 0.5f, sy * 0.5f);
    fe_draw_text(panel_x + (panel_w - fe_measure_text(lines[desc_index][3], sx * 0.5f)) * 0.5f, panel_y + 56.0f * sy, lines[desc_index][3], 0xFFFFFFFF, sx * 0.5f, sy * 0.5f);
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
    if (car_index < 0 || car_index >= 37) return;
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
     * exp: 0=raw value, 1=layout type, 2=engine type, 3=tire pair (fi + fi+1) */
    static const struct { const char *hdr; int fi; int exp; float y; } k_rows[] = {
        { "LAYOUT:",       2,  1, 124.0f },
        { "GEARS:",        3,  0, 136.0f },
        { "PRICE:",        4,  0, 148.0f },
        { "TIRES:",        5,  3, 160.0f },
        { "TOP SPEED:",    7,  0, 196.0f },
        { "0 TO 60 MPH:",  8,  0, 208.0f },
        { "60 TO 0 MPH:",  9,  0, 220.0f },
        { "1/4 MILE:",    10,  0, 232.0f },
        { "ENGINE:",      11,  2, 256.0f },
        { "COMPRESSION:", 12,  0, 280.0f },
        { "DISPLACEMENT:",13,  0, 292.0f },
        { "LATERAL ACC:", 14,  0, 304.0f },
        { "TORQUE:",      15,  0, 328.0f },
        { "HP:",          16,  0, 340.0f },
    };
    int n_layout = (int)(sizeof(k_stat_layout_types)/sizeof(k_stat_layout_types[0]));
    int n_engine = (int)(sizeof(k_stat_engine_types)/sizeof(k_stat_engine_types[0]));
    float tsc = 0.5f;
    float hx = 232.0f * sx;   /* label column x = canvasW - 0x198 */
    /* Value column: label_x + max_label_width + 16px gap (matches original dynamic measure) */
    float vx = hx + fe_measure_text("COMPRESSION:", tsc * sx) + 16.0f * sx;
    char val[64];
    int i;

    for (i = 0; i < 14; i++) {
        float y = k_rows[i].y * sy;
        const char *raw = (k_rows[i].fi < 17) ? s_car_spec[k_rows[i].fi] : "";
        int idx;

        fe_draw_text(hx, y, k_rows[i].hdr, 0xFFBBBBBB, tsc * sx, tsc * sy);

        switch (k_rows[i].exp) {
        case 1: /* layout type: char - 'A' */
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_text(vx, y,
                         (idx >= 0 && idx < n_layout) ? k_stat_layout_types[idx] : raw,
                         0xFFFFFFFF, tsc * sx, tsc * sy);
            break;
        case 2: /* engine type: char - 'A' */
            idx = (raw[0] >= 'A' && raw[0] <= 'Z') ? raw[0] - 'A' : -1;
            fe_draw_text(vx, y,
                         (idx >= 0 && idx < n_engine) ? k_stat_engine_types[idx] : raw,
                         0xFFFFFFFF, tsc * sx, tsc * sy);
            break;
        case 3: /* front/rear tire combined */
        {
            char f[24], r[24];
            frontend_fmt_spec(f, sizeof(f), raw);
            frontend_fmt_spec(r, sizeof(r), (k_rows[i].fi+1 < 17) ? s_car_spec[k_rows[i].fi+1] : "");
            snprintf(val, sizeof(val), "%s/%s", f, r);
            fe_draw_text(vx, y, val, 0xFFFFFFFF, tsc * sx, tsc * sy);
            break;
        }
        default:
            frontend_fmt_spec(val, sizeof(val), raw);
            fe_draw_text(vx, y, val, 0xFFFFFFFF, tsc * sx, tsc * sy);
            break;
        }
    }
}

static void frontend_render_car_selection_preview(float sx, float sy) {
    int actual_car = frontend_current_car_index();
    float sw = sx * 640.0f;
    float sh = sy * 480.0f;

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
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 2500.0f);
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
        float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 2500.0f);
        float bar_x    = 636.0f - (636.0f - 36.0f) * t;
        float topbar_x = -532.0f + 532.0f * t;
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_rect(s_carsel_topbar_surface, topbar_x * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,   bar_x * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_rect(s_carsel_curve_surface, bar_x * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
        /* Bottom strip: flat fill from right of curve to screen edge */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad((bar_x + 80.0f) * sx, 408.0f * sy, (640.0f - bar_x - 80.0f) * sx, 56.0f * sy,
                     0xFF00005C, s_white_tex_page, 0, 0, 1, 1);
    } else {
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_rect(s_carsel_topbar_surface,  0.0f * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,    36.0f * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_rect(s_carsel_curve_surface,  36.0f * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
        /* Bottom strip: flat fill from right of curve (x=116) to screen edge */
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(116.0f * sx, 408.0f * sy, 524.0f * sx, 56.0f * sy,
                     0xFF00005C, s_white_tex_page, 0, 0, 1, 1);
    }

    if (s_inner_state == 15) {
        /* Stats sub-screen: car image at 35% opacity over the blue panel background,
         * then spec text. Matches original: car blitted at alpha 0x5A onto the blue
         * primary surface (FillPrimaryFrontendRect 0x5c), no additional overlay quad. */
        if (s_car_preview_surface > 0)
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0x5AFFFFFF);
        frontend_render_car_stats_overlay(sx, sy);
    } else {
        if (s_inner_state == 11 && s_car_preview_prev_surface > 0) {
            /* Old car slides out to the right (state 11, ~433ms) — animPhase 0x0B: offset = counter*0x20 */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 433.0f);
            float x = 232.0f + 408.0f * t;  /* 232 → 640 (off-screen right) */
            fe_draw_surface_rect(s_car_preview_prev_surface, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        } else if (s_inner_state == 14 && s_car_preview_surface > 0) {
            /* New car slides in from right (state 14, ~833ms @30fps):
             * formula @ 0x0040DF4A: x = canvasW + counter*(-0x40) + 0x4A8
             *   = 640 + 1192 = 1832 at counter=0, arrives at 232 in 25 frames (1600px travel).
             * Time-based equivalent: 25 frames / 30fps = ~833ms, using 800ms for match. */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 800.0f);
            float x = 1832.0f - 1600.0f * t;  /* 1832 → 232 [CONFIRMED @ 0x0040DF4A] */
            TD5_LOG_I(LOG_TAG, "car_sel: slide-in x=%.0f t=%.2f", x, t);
            fe_draw_surface_rect(s_car_preview_surface, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        } else if (s_inner_state >= 6 && s_inner_state != 12 && s_inner_state != 13 && s_car_preview_surface > 0) {
            /* Static car display: skip during init+slide-in (states 0-5) and
             * pass-through transition ticks (12/13) to avoid premature display */
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        }
    }

    if (s_anim_complete) {
        /* Car name: y = canvasH/2 - 0x97 = 480/2 - 151 = 89 (gap between topbar bottom y=81 and car preview y=124) */
        frontend_draw_value_text(sx, sy, 232, 89, frontend_get_car_display_name(actual_car), 0xFFFFFFFF);
        if (!s_cheat_unlock_all && !s_network_active &&
            actual_car >= 0 && actual_car < 37 &&
            s_car_lock_table[actual_car] != 0) {
            frontend_draw_value_text(sx, sy, 86, 121, "LOCKED", 0xFFFF4444);
        }
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
    }
    /* Draw L/R arrow indicators on Track and Direction buttons */
    fe_draw_option_arrows(0, sx, sy);
    fe_draw_option_arrows(1, sx, sy);
    if (!s_cheat_unlock_all &&
        s_selected_track >= 0 &&
        s_selected_track < 26 &&
        s_track_lock_table[s_selected_track] != 0 &&
        !s_network_active) {
        frontend_draw_value_text(sx, sy, 412, 375, "LOCKED", 0xFFFF4444);
    }
}

static void frontend_render_control_options_overlay(float sx, float sy) {
    if (!s_anim_complete || s_control_options_surface <= 0) return;
    int slot = s_control_options_surface - 1;
    if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) return;

    /* Controllers.tga: sprite sheet, 64x32 per controller-type icon, rows stacked vertically.
     * Positions are absolute canvas coords from FUN_0041DF20 case4/5 steady-state:
     *   P1 icon: x = uVar2+0x4a = 320+74 = 394,  y = uVar4-0x8f = 240-143 = 97
     *   P2 icon: x = 394,                          y = uVar4-0x17 = 240-23  = 217
     * Row = controller_type * 32; type 0 = keyboard, 1 = joypad, 2 = joystick. */
    int sh = s_surfaces[slot].height;
    if (sh <= 0) return;

    float icon_w = 64.0f * sx;
    float icon_h = 32.0f * sy;
    float v_row  = 32.0f / (float)sh;
    int p1_type = td5_input_get_device_type(0);
    int p2_type = td5_input_get_device_type(1);
    float p1_v0 = (float)p1_type * v_row, p1_v1 = p1_v0 + v_row;
    float p2_v0 = (float)p2_type * v_row, p2_v1 = p2_v0 + v_row;

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(394.0f * sx,  97.0f * sy, icon_w, icon_h,
                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, p1_v0, 1.0f, p1_v1);
    fe_draw_quad(394.0f * sx, 217.0f * sy, icon_w, icon_h,
                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, p2_v0, 1.0f, p2_v1);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* Controller binding overlay: show the active controller-type icon (64x32)
 * centered horizontally at y=120, plus "No Controller" warning when applicable.
 * Original: FUN_0040FE00 draws the detected device icon from individual TGAs.
 * The Control Options screen (14) uses Controllers.TGA sprite sheet instead;
 * the binding screen (18) uses per-type icon TGAs. */
static void frontend_render_controller_binding_overlay(float sx, float sy) {
    if (!s_anim_complete) return;

    /* Determine which icon to show based on detected controller type.
     * Query the input module for the active device type on player 0.
     * 0 = keyboard, 1 = joypad/gamepad, 2 = joystick/wheel. */
    int controller_type = td5_input_get_device_type(0);
    int icon_surface = s_keyboard_icon_surface;
    if (controller_type == 1) icon_surface = s_joypad_icon_surface;
    if (controller_type == 2) icon_surface = s_joystick_icon_surface;

    if (icon_surface > 0) {
        int slot = icon_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            float icon_w = (float)s_surfaces[slot].width  * sx;
            float icon_h = (float)s_surfaces[slot].height * sy;
            float icon_x = (320.0f - (float)s_surfaces[slot].width * 0.5f) * sx;
            float icon_y = 120.0f * sy;
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(icon_x, icon_y, icon_w, icon_h,
                         0xFFFFFFFF, s_surfaces[slot].tex_page,
                         0.0f, 0.0f, 1.0f, 1.0f);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        }
    }
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
    int speed_kph = td5_save_get_speed_units();
    const char *speed_suffix = speed_kph ? "KPH" : "MPH";

    /* Panel geometry: 0x208 x 0x90 surface blitted at (115, 177) in 640x480 */
    float panel_x = 115.0f * sx;
    float panel_y = 177.0f * sy;

    /* Column X positions (in 520px panel space, scaled to screen) */
    float col_name  = panel_x + 16.0f  * sx;
    float col_score = panel_x + 128.0f * sx; /* 0x80: original left edge */
    float col_car   = panel_x + 228.0f * sx;
    float col_avg   = panel_x + 352.0f * sx;
    float col_top   = panel_x + 444.0f * sx;

    /* Scale for small text within the panel */
    float ts = 0.55f;  /* text scale relative to screen scale */

    if (!grp) {
        float tw = fe_measure_text("NO SCORES YET", sx * ts);
        fe_draw_text((320.0f * sx) - tw * 0.5f, panel_y + 60.0f * sy,
                     "NO SCORES YET", 0xFFCCCCCC, sx * ts, sy * ts);
        return;
    }

    int score_type = grp->header & 0xFF;

    /* Column headers — same white font as data rows (SmallText.tga, no color change) */
    float hdr_y = panel_y + 7.0f * sy;
    uint32_t hdr_color = 0xFFFFFFFF;
    fe_draw_text(col_name,  hdr_y, "NAME", hdr_color, sx * ts, sy * ts);
    /* Score column: two-line header. "BEST"/"LAP"/"PTS" on y=0, type label on y=14.
     * For PTS only one line. (SNK_BestTxt at y=0, SNK_TimeTxt/LapTxt at y=14) */
    if (score_type == 2) {
        fe_draw_text(col_score, hdr_y, "PTS",  hdr_color, sx * ts, sy * ts);
    } else {
        fe_draw_text(col_score, panel_y + 0.0f,       "BEST",                          hdr_color, sx * ts, sy * ts);
        fe_draw_text(col_score, panel_y + 14.0f * sy, (score_type == 1) ? "LAP" : "TIME", hdr_color, sx * ts, sy * ts);
    }
    fe_draw_text(col_car, hdr_y, "CAR", hdr_color, sx * ts, sy * ts);
    /* AVG/TOP: two lines — label at y=0, speed unit at y=14 within the 144px surface */
    fe_draw_text(col_avg, panel_y + 0.0f,        "AVG",        hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_avg, panel_y + 14.0f * sy,  speed_suffix, hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_top, panel_y + 0.0f,        "TOP",        hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_top, panel_y + 14.0f * sy,  speed_suffix, hdr_color, sx * ts, sy * ts);

    /* 5 entry rows */
    float row_y = panel_y + 48.0f * sy;
    float row_h = 16.0f * sy;
    for (int i = 0; i < 5; i++) {
        const TD5_NpcEntry *e = &grp->entries[i];
        float y = row_y + (float)i * row_h;
        uint32_t row_color = (i == 0) ? 0xFFFFCC44 : 0xFFE0E0E0;
        char buf[64];

        /* Check if entry is empty (no name) */
        if (e->name[0] == '\0') {
            fe_draw_text(col_name, y, "---", 0xFF888888, sx * ts, sy * ts);
            continue;
        }

        /* Rank */
        snprintf(buf, sizeof(buf), "%d", i + 1);
        fe_draw_text(panel_x + 2.0f * sx, y, buf, row_color, sx * ts, sy * ts);

        /* Name (clipped to 13 chars max from struct) */
        {
            char name_buf[14];
            memcpy(name_buf, e->name, 13);
            name_buf[13] = '\0';
            fe_draw_text(col_name, y, name_buf, row_color, sx * ts, sy * ts);
        }

        /* Score / Time */
        frontend_format_score_time(buf, sizeof(buf), e->score, score_type);
        fe_draw_text(col_score, y, buf, row_color, sx * ts, sy * ts);

        /* Car name */
        {
            int cid = e->car_id & 0xFF;
            const char *cname = frontend_get_car_display_name(cid);
            /* Clip car name to 108px column width [0xe4..0x150] */
            char cname_buf[20];
            strncpy(cname_buf, cname, sizeof(cname_buf) - 1);
            cname_buf[sizeof(cname_buf) - 1] = '\0';
            {
                int clen = (int)strlen(cname_buf);
                while (clen > 0 && fe_measure_text(cname_buf, sx * ts) > 108.0f * sx) {
                    cname_buf[--clen] = '\0';
                }
            }
            fe_draw_text(col_car, y, cname_buf, row_color, sx * ts, sy * ts);
        }

        /* Average speed */
        snprintf(buf, sizeof(buf), "%d", frontend_convert_speed(e->avg_speed, speed_kph));
        fe_draw_text(col_avg, y, buf, row_color, sx * ts, sy * ts);

        /* Top speed */
        snprintf(buf, sizeof(buf), "%d", frontend_convert_speed(e->top_speed, speed_kph));
        fe_draw_text(col_top, y, buf, row_color, sx * ts, sy * ts);
    }
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
 * [UNCERTAIN] Panel alpha: original uses opaque BltColorFillToSurface(0,0,0);
 * port uses 0xE0 so the gallery background stays dimly visible.
 * [UNCERTAIN] Column layout: simplified POS/DRIVER/CAR/TIME columns —
 * SNK_* string-surface pipeline not wired up in the port.
 * ========================================================================== */
static void frontend_render_race_results_overlay(float sx, float sy) {
    /* State gate: panel is created at state 0, first drawn at state 3
     * (slide-in) and persists through states 4-0xB plus 0xD-0x14. */
    if (s_inner_state < 3 || s_inner_state == 0xC) return;

    /* Panel 408x392 centered in 640x480 canvas space */
    const float canvas_w = 640.0f, canvas_h = 480.0f;
    const float panel_w = 408.0f, panel_h = 392.0f;
    float panel_x = (canvas_w - panel_w) * 0.5f * sx;
    float panel_y = (canvas_h - panel_h) * 0.5f * sy;
    float pw = panel_w * sx;
    float ph = panel_h * sy;

    if (s_white_tex_page >= 0) {
        /* Dark translucent panel body (see [UNCERTAIN] above) */
        fe_draw_quad(panel_x, panel_y, pw, ph, 0xE0000000,
                     s_white_tex_page, 0, 0, 1, 1);

        /* Gold border (1.5px scaled) */
        float bw = 1.5f * sx;
        float bh = 1.5f * sy;
        uint32_t border = 0xFFFFCC44;
        fe_draw_quad(panel_x, panel_y,              pw, bh,  border, s_white_tex_page, 0, 0, 1, 1);
        fe_draw_quad(panel_x, panel_y + ph - bh,    pw, bh,  border, s_white_tex_page, 0, 0, 1, 1);
        fe_draw_quad(panel_x, panel_y,              bw, ph,  border, s_white_tex_page, 0, 0, 1, 1);
        fe_draw_quad(panel_x + pw - bw, panel_y,    bw, ph,  border, s_white_tex_page, 0, 0, 1, 1);
    }

    /* Title selection based on selected_game_type (mirrors the
     * per-game-type column-header branches at 0x0042260E-0x00422774) */
    const char *title = "RACE RESULTS";
    if (s_selected_game_type >= 1 && s_selected_game_type <= 6)
        title = "CUP RACE RESULTS";
    else if (s_selected_game_type == 7)
        title = "DRAG RESULTS";
    else if (s_selected_game_type == 8)
        title = "CUP CHALLENGE RESULTS";
    else if (s_selected_game_type == 9)
        title = "TIME TRIAL RESULTS";

    float title_w = fe_measure_text(title, sx);
    fe_draw_text(panel_x + (pw - title_w) * 0.5f, panel_y + 14.0f * sy,
                 title, 0xFFFFCC44, sx, sy);

    /* During state 0xD+ the panel is reused as the main-menu backdrop,
     * so hide the results table (buttons are drawn on top by the
     * generic button loop in td5_frontend_render_ui_rects). */
    if (s_inner_state >= 0x0D) return;

    /* --- Results table --- */
    int kph = td5_save_get_speed_units();
    (void)kph;

    float ts = 0.75f;
    uint32_t hdr_color = 0xFFCCCCCC;
    float hdr_y = panel_y + 52.0f * sy;
    float col_pos  = panel_x + 20.0f  * sx;
    float col_name = panel_x + 62.0f  * sx;
    float col_car  = panel_x + 180.0f * sx;
    float col_time = panel_x + 300.0f * sx;

    fe_draw_text(col_pos,  hdr_y, "POS",    hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_name, hdr_y, "DRIVER", hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_car,  hdr_y, "CAR",    hdr_color, sx * ts, sy * ts);
    fe_draw_text(col_time, hdr_y, "TIME",   hdr_color, sx * ts, sy * ts);

    /* Enumerate slots 0..5, emit one row per slot with a valid actor. */
    float row_y = panel_y + 84.0f * sy;
    float row_h = 24.0f * sy;
    int row = 0;
    for (int slot = 0; slot < TD5_MAX_RACER_SLOTS; slot++) {
        TD5_Actor *a = td5_game_get_actor(slot);
        if (!a) continue;

        float y = row_y + (float)row * row_h;
        uint32_t row_color = (slot == 0) ? 0xFFFFCC44 : 0xFFE0E0E0;
        char buf[64];

        snprintf(buf, sizeof(buf), "%d", row + 1);
        fe_draw_text(col_pos, y, buf, row_color, sx * ts, sy * ts);

        snprintf(buf, sizeof(buf), (slot == 0) ? "PLAYER" : "CPU %d", slot);
        fe_draw_text(col_name, y, buf, row_color, sx * ts, sy * ts);

        {
            int car_idx = (slot == 0) ? s_selected_car : 0;
            const char *cname = frontend_get_car_display_name(car_idx);
            char cname_buf[18];
            strncpy(cname_buf, cname ? cname : "", sizeof(cname_buf) - 1);
            cname_buf[sizeof(cname_buf) - 1] = '\0';
            fe_draw_text(col_car, y, cname_buf, row_color, sx * ts, sy * ts);
        }

        {
            int32_t ticks = td5_game_get_race_timer(slot, 0);
            if (ticks <= 0) {
                snprintf(buf, sizeof(buf), "DNF");
            } else {
                frontend_format_score_time(buf, sizeof(buf), ticks, 0);
            }
            fe_draw_text(col_time, y, buf, row_color, sx * ts, sy * ts);
        }

        row++;
    }

    /* Footer hint during interactive state 6 */
    if (s_inner_state == 6) {
        const char *hint = "Press OK to continue";
        float hw = fe_measure_text(hint, sx * 0.7f);
        fe_draw_text(panel_x + (pw - hw) * 0.5f, panel_y + ph - 28.0f * sy,
                     hint, 0xFFAAAAAA, sx * 0.7f, sy * 0.7f);
    }
}

static void frontend_render_extras_gallery_overlay(float sx, float sy) {
    /* Black background fills entire viewport */
    if (s_white_tex_page >= 0) {
        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        fe_draw_quad(0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xFF000000,
                     s_white_tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    if (s_gallery_pic_surface <= 0) return;
    int slot = s_gallery_pic_surface - 1;
    if (slot < 0 || slot >= FE_MAX_SURFACES || !s_surfaces[slot].in_use) return;

    int img_w = s_surfaces[slot].width;
    int img_h = s_surfaces[slot].height;
    if (img_w <= 0 || img_h <= 0) return;

    /* Scale to fit 640x480, maintaining aspect ratio, centered */
    float scale_x = 640.0f / (float)img_w;
    float scale_y = 480.0f / (float)img_h;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    float virt_w = (float)img_w * scale;
    float virt_h = (float)img_h * scale;
    float virt_x = (640.0f - virt_w) * 0.5f;
    float virt_y = (480.0f - virt_h) * 0.5f;

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
    fe_draw_quad(virt_x * sx, virt_y * sy, virt_w * sx, virt_h * sy,
                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, 0.0f, 1.0f, 1.0f);
}

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
    float cx = x;
    float texel_w = 1.0f / (float)FONT_TEX_W;
    float cell_h = (float)FONT_CELL * sy;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (int i = 0; text[i]; i++) {
        int c = toupper((unsigned char)text[i]);
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
        fe_draw_quad(cx, y, glyph_w, cell_h, color, s_font_page, u0, v0, u1, v1);
        cx += glyph_advance;
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

static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1) {
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
    /* Number of rows: (canvasH - 0x20) >> 5 = (480 - 32) / 32 = 14 [CONFIRMED] */
    int rows = (int)((480.0f - 32.0f) / 32.0f);
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

    /* Dialog background — original BltColorFillToSurface fills to black [CONFIRMED] */
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(dlg_x, dlg_y, dlg_w, dlg_h, 0xCC000000u, -1, 0,0,1,1);

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
    float dlg_h = 112.0f * sy;
    float dlg_cx = dlg_x + dlg_w * 0.5f;

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(dlg_x, dlg_y, dlg_w, dlg_h, 0xCC000000u, -1, 0,0,1,1);

    /* "SORRY" at y=0x00 [CONFIRMED Language.dll: SorryTxt = "SORRY"] */
    fe_draw_text_centered(dlg_cx, dlg_y + 0.0f  * sy, "SORRY",          0xFFFFFFFF, sx, sy);
    /* "SESSION LOCKED" at y=0x38=56 [CONFIRMED Language.dll: SeshLockedTxt = "SESSION LOCKED"] */
    fe_draw_text_centered(dlg_cx, dlg_y + 56.0f * sy, "SESSION LOCKED", 0xFFFFFFFF, sx, sy);

    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
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

    /* Begin scene for frontend rendering */
    td5_plat_render_clear(0xFF101020);
    td5_plat_render_begin_scene();
    td5_plat_render_set_viewport(0, 0, screen_w, screen_h);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

    /* Draw background TGA if one is loaded */
    if (s_background_surface > 0) {
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
    case TD5_SCREEN_DISPLAY_OPTIONS:
        frontend_render_display_options_overlay(sx, sy);
        break;
    case TD5_SCREEN_TWO_PLAYER_OPTIONS:
        frontend_render_two_player_options_overlay(sx, sy);
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
    case TD5_SCREEN_EXTRAS_GALLERY:
        frontend_render_extras_gallery_overlay(sx, sy);
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
    case TD5_SCREEN_SESSION_LOCKED:
        /* "Sorry / Session Locked" dialog [CONFIRMED @ 0x0041D630] */
        frontend_render_session_locked_overlay(sx, sy);
        break;
    default:
        break;
    }

    /* Draw buttons */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) continue;

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
         * State 0 = gold/selected, 1 = blue/unselected, 2 = disabled. */
        if (s_buttonbits_tex_page >= 0 && s_buttonbits_w > 0 && s_buttonbits_h > 0) {
            /* No opaque fill — original blits button surface to screen with
             * DDBLT_KEYSRC (black = transparent). We draw only the 9-slice
             * frame with alpha blending; background shows through naturally. */
            int bb_state;
            if (s_buttons[i].disabled)                               bb_state = 2;
            else if (flash_active || s_buttons[i].highlight_ramp == 6) bb_state = 0;
            else                                                     bb_state = 1;

            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            /* Gold/selected state: fill button interior with dark purple (R=57,G=33,B=82).
             * Original CreateFrontendDisplayModeButton (0x425DE0) applied COLORFILL with
             * RGB565 0x390A ≈ this color before BltFasting the ButtonBits frame columns.
             * Blue/unselected center is black = transparent via SRCCOLORKEY = correct. */
            fe_draw_button_9slice(bx, by, bw, bh, bb_state, sx, sy);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);

        } else {
            fe_draw_quad(bx, by, bw, bh, bg_color, -1, 0, 0, 1, 1);
        }

        /* Button label text — original draws into cached surface at Y=0,
         * horizontally centered. Font: BodyText.tga, 24x24 cells,
         * red colorkey → alpha=0 for transparent background. */
        if (s_buttons[i].label[0] && s_font_page >= 0) {
            float text_w = fe_measure_text(s_buttons[i].label, sx);
            float tx = bx + (bw - text_w) * 0.5f;
            float ty = by;  /* original draws label at Y=0 on pre-baked surface (FUN_00424560) */
            uint32_t text_color = 0xFFFFFFFF;
            if (s_buttons[i].disabled) text_color = 0xFF888888;
            fe_draw_text(tx, ty, s_buttons[i].label, text_color, sx, sy);
        }

        /* Green highlight border (RenderFrontendDisplayModeHighlight 0x4263e0).
         * 2px outline, mouse-hover only. Driven by the separate hover index
         * (DAT_00498700), NOT the selection index — hover does not select. */
        if (i == s_mouse_hover_button &&
            !s_buttons[i].disabled) {
            uint32_t gc = 0xFF008000;
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
            fe_draw_option_arrows(0, sx, sy);
            fe_draw_option_arrows(1, sx, sy);
            TD5_LOG_I("frontend", "fe_draw_option_arrows: quick_race pass");
            break;
        case TD5_SCREEN_GAME_OPTIONS:
            for (int i = 0; i <= 6; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_DISPLAY_OPTIONS:
            for (int i = 0; i <= 3; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_SOUND_OPTIONS:
            for (int i = 0; i <= 2; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        case TD5_SCREEN_TWO_PLAYER_OPTIONS:
            for (int i = 0; i <= 1; i++) fe_draw_option_arrows(i, sx, sy);
            break;
        default:
            break;
        }
    }

    /* Nav bar text drawn after buttons so it renders on top of the button frame.
     * (button 0 is the nav bar: button loop draws the 9-slice frame, then we
     * draw the track name and arrows on top.) */
    if (s_current_screen == TD5_SCREEN_HIGH_SCORE && s_anim_complete && s_inner_state >= 6) {
        char track_name[80];
        frontend_get_track_display_name(s_score_category_index, 0, track_name, sizeof(track_name));
        float nav_bx, nav_by, nav_bw, nav_bh;
        frontend_get_button_render_rect(0, sx, sy, &nav_bx, &nav_by, &nav_bw, &nav_bh);
        float tnw = fe_measure_text(track_name, sx);
        float tx = nav_bx + (nav_bw - tnw) * 0.5f;
        float ty = nav_by + (nav_bh - sy * 24.0f) * 0.5f;  /* font cells are 24px tall */
        fe_draw_text(tx, ty, track_name, 0xFFFFFFFF, sx, sy);
        fe_draw_option_arrows(0, sx, sy);
    }

    /* (text overlay rendering deferred to font system) */

    if (frontend_ensure_title_texture(s_current_screen)) {
        int page = s_title_tex_page[s_current_screen];
        int title_w = s_title_tex_w[s_current_screen];
        int title_h = s_title_tex_h[s_current_screen];
        if (page > 0 && title_w > 0 && title_h > 0) {
            float title_x = 120.0f * sx; /* original: uVar4-200 = screenW/2-200 = 120 [0x4213D0, 0x41D890, others] */
            float title_y = frontend_get_title_render_y(sy);
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(title_x, title_y, (float)title_w * sx, (float)title_h * sy,
                         0xFFFFFFFF, page, 0.0f, 0.0f, 1.0f, 1.0f);
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
    s_flow_context = 0;
    s_selected_game_type = -1;
    s_race_within_series = 0;
    s_cup_unlock_tier = 0;
    s_two_player_mode = 0;
    s_split_screen_mode = 0;
    s_attract_mode_ctrl = 0;
    s_selected_car = g_td5.ini.loaded ? g_td5.ini.default_car : 0;
    s_selected_paint = 0;
    s_selected_config = 0;
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
        s_display_speed_units       = g_td5.ini.speed_units;
        td5_save_set_speed_units(g_td5.ini.speed_units);
        s_display_camera_damping    = g_td5.ini.camera_damping;
        td5_save_set_camera_damping(g_td5.ini.camera_damping);
        s_sound_option_sfx_volume   = g_td5.ini.sfx_volume;
        s_sound_option_music_volume = g_td5.ini.music_volume;
        s_sound_option_sfx_mode     = g_td5.ini.sfx_mode;
        td5_save_set_sound_mode(g_td5.ini.sfx_mode);
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

        /* [INFERRED] Load LANGUAGE.DLL string table (M2DX — stub in port) */
        /* [INFERRED] Load car ZIP path table from gCarZipPathTable (handled in td5_asset.c) */
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
    case 0: /* Load Positioner.tga */
        frontend_init_return_screen(TD5_SCREEN_POSITIONER_DEBUG);
        frontend_load_tga("Front_End/Positioner.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("Save", 120, 400, 96, 32);
        frontend_create_button("Back", 232, 400, 112, 32);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: /* Present grid */
        frontend_present_buffer();
        s_inner_state = 2;
        break;
    case 2: /* Init grid */
        s_anim_tick = 0;
        s_inner_state = 3;
        break;
    case 3: /* Navigate grid with arrow keys */
        if (s_input_ready && s_arrow_input != 0) {
            s_anim_tick += frontend_option_delta();
            if (s_anim_tick < 0) s_anim_tick = 0;
            frontend_play_sfx(1);
        }
        if (s_input_ready && s_button_index == 0) {
            s_inner_state = 5;
        } else if (s_input_ready && s_button_index == 1) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        } else if (s_input_ready) {
            s_inner_state = 4;
        }
        break;
    case 4: /* Edit cell values */
        if (s_input_ready && s_arrow_input != 0) {
            s_anim_tick += frontend_option_delta();
            frontend_play_sfx(2);
        }
        if (s_input_ready && s_button_index >= 0) {
            s_inner_state = (s_button_index == 0) ? 5 : 3;
        } else if (!s_input_ready) {
            s_inner_state = 3;
        }
        break;
    case 5: /* Write positioner.txt */
        TD5_LOG_I(LOG_TAG, "ScreenPositionerDebugTool: writing positioner.txt");
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    default:
        td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        break;
    }
}

/* ========================================================================
 * [2] RunAttractModeDemoScreen (0x4275A0) -- Attract mode / demo
 * States: 6
 * ======================================================================== */

static void Screen_AttractModeDemo(void) {
    /* Any keypress or mouse click cancels the demo and returns to main menu */
    {
        const uint8_t *kb = td5_plat_input_get_keyboard();
        if (kb) {
            int k;
            for (k = 1; k < 256; k++) {
                if (kb[k] & 0x80) {
                    td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
                    return;
                }
            }
        }
    }

    switch (s_inner_state) {
    case 0: /* Set attract mode flag */
        frontend_init_return_screen(TD5_SCREEN_ATTRACT_MODE);
        /* DAT_00495254 = 1 */
        frontend_present_buffer();
        frontend_set_cursor_visible(1);
        s_inner_state = 1;
        break;

    case 1: /* Release frontend buttons from main menu */
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
            /* Fade complete -- launch demo race with input playback */
            frontend_init_race_schedule();
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
        frontend_load_tga("Front_End/Language.tga", "Front_End/FrontEnd.zip");
        frontend_load_tga("Front_End/LanguageScreen.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("English", 120, 180, 180, 32);
        frontend_create_button("French",  120, 220, 180, 32);
        frontend_create_button("German",  120, 260, 180, 32);
        frontend_create_button("Spanish", 120, 300, 180, 32);
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

    case 1: /* Fade in */
        s_anim_tick += 2;
        if (s_anim_tick >= 16) {
            s_anim_tick = 0;
            s_inner_state = 2;
        }
        break;

    case 2: /* 3-second timer */
        s_anim_tick += 2;
        if (s_input_ready || s_anim_tick >= 90) {
            s_anim_tick = 0;
            s_inner_state = 3;
        }
        break;

    case 3: /* Fade out + exit to main menu */
        s_anim_tick += 2;
        if (s_anim_tick >= 16) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
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
        frontend_create_button("Race Menu",   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Quick Race",  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Two Player",  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Net Play",    -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Options",     -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("High Scores", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Exit",        -0xE0, 0, 0xE0, 0x20);

        frontend_set_cursor_visible(1);
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
            frontend_set_cursor_visible(0);
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
                    s_flow_context = 3;
                    s_two_player_mode = 1;
                    s_selected_game_type = 0;
                    s_return_screen = TD5_SCREEN_CAR_SELECTION;
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

    case 5: { /* Exit confirm dialog: create Yes/No buttons */
        int exit_x = s_buttons[6].x;
        int exit_y = s_buttons[6].y;
        int exit_h = s_buttons[6].h;
        int yes_idx = frontend_create_button("Yes", exit_x, exit_y + exit_h + 8, 96, 32);
        int no_idx  = frontend_create_button("No",  exit_x + 100, exit_y + exit_h + 8, 96, 32);
        if (yes_idx >= 0) s_selected_button = yes_idx;
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirm dialog created yes=%d no=%d", yes_idx, no_idx);
        (void)no_idx;
        s_inner_state = 6;
        break;
    }

    case 6: /* Exit confirm: wait for Yes/No */
        if (s_input_ready && s_button_index >= 0) {
            /* Check by label since button indices depend on pool state */
            if (s_button_index < FE_MAX_BUTTONS &&
                strcmp(s_buttons[s_button_index].label, "Yes") == 0) {
                TD5_LOG_I(LOG_TAG, "MainMenu: exit Yes selected, quitting");
                s_inner_state = 7;
            } else if (s_button_index < FE_MAX_BUTTONS &&
                       strcmp(s_buttons[s_button_index].label, "No") == 0) {
                /* Drop Yes/No buttons (appended after the 7 main menu buttons) */
                if (s_button_count > 7) s_button_count = 7;
                s_selected_button = 6; /* re-focus on Exit */
                TD5_LOG_I(LOG_TAG, "MainMenu: exit No selected, returning to menu");
                s_inner_state = 4;
            }
        }
        break;

    case 7: /* Confirm exit -- navigate to credits then quit */
        TD5_LOG_I(LOG_TAG, "MainMenu: exit confirmed, going to credits");
        td5_frontend_set_screen(TD5_SCREEN_EXTRAS_GALLERY);
        break;

    case 8: /* Slide-out prep: keep the software cursor visible for the next frontend screen */
        frontend_set_cursor_visible(0);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out animation: buttons scatter, ~500ms */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* Controller validation: check joystick assignment */
            /* 1P modes (context 1,2,4): check P1 input source */
            /* 2P mode (context 3): also check P2 input source */
            /* If joystick index == 7 (none), go to state 0x14 */
            /* Otherwise, navigate to target screen */
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

    /* States 13-19: reserved/unused in normal flow */
    case 13: case 14: case 15: case 16: case 17: case 18: case 19:
        break;

    case 0x14: /* "Must select controller" error */
        TD5_LOG_W(LOG_TAG, "MainMenu: controller not assigned");
        /* Create error message surface */
        s_inner_state = 0x15;
        break;

    case 0x15: /* Controller-required message display */
    case 0x16:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 0x17: /* Cleanup, redirect to controller options (screen 0xE) */
        td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        break;
    }
}

/* ========================================================================
 * [6] RaceTypeCategoryMenuStateMachine (0x4168B0)
 * States: ~21 (0x00 - 0x14)
 * Top-level: 7 buttons (Single Race, Cup Race, Continue Cup,
 *            Time Trials, Drag Race, Cop Chase, Back)
 * Cup sub-menu: 7 buttons (Championship..Ultimate, Back)
 * ======================================================================== */

static void Screen_RaceTypeCategory(void) {
    switch (s_inner_state) {
    case 0: /* Init: create 7 buttons, load RaceMenu.tga, create description surface */
        frontend_init_return_screen(TD5_SCREEN_RACE_TYPE_MENU);
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: state 0 - init");
        frontend_reset_buttons();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip"); /* original 0x4168B0: loads MainMenu.tga, not RaceMenu.tga */
        s_anim_complete = 0;

        /* Create 0x110 x 0xB4 description preview surface */
        /* Create 7 buttons for race types */
        frontend_create_button("Single Race", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Cup Race",    -0xE0, 0, 0xE0, 0x20);
        /* Continue Cup: greyed if no valid CupData.td5 */
        if (frontend_validate_cup_checksum())
            frontend_create_button("Continue Cup", -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button("Continue Cup", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Time Trials", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Drag Race",   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Cop Chase",   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Back",        -0xE0, 0, 0xE0, 0x20);

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
            s_inner_state = 3;
        }
        break;

    case 3: /* Main interaction loop */
        /* Render buttons + description preview */
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

    case 4: /* Description preview update: redraw SNK_RaceTypeText[gameType] */
        /* Clear description surface, draw race type info text */
        s_inner_state = 3;
        break;

    case 5: /* Slide-out prep: buttons scatter */
        frontend_begin_timed_animation();
        s_inner_state = 0x14;
        break;

    /* --- Cup sub-menu (states 6-12) --- */

    case 6: /* Cup sub-menu: release top buttons, create cup tier buttons */
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: entering cup sub-menu");
        frontend_reset_buttons();
        /* Create 7 cup tier buttons */
        frontend_create_button("Championship", -0xE0, 0, 0xE0, 0x20); /* always available */
        frontend_create_button("Era",          -0xE0, 0, 0xE0, 0x20); /* always available */

        /* Challenge: locked if s_cup_unlock_tier == 0 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button("Challenge", -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button("Challenge", -0xE0, 0, 0xE0, 0x20);

        /* Pitbull: locked if s_cup_unlock_tier < 1 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button("Pitbull", -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button("Pitbull", -0xE0, 0, 0xE0, 0x20);

        /* Masters: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button("Masters", -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button("Masters", -0xE0, 0, 0xE0, 0x20);

        /* Ultimate: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button("Ultimate", -0xE0, 0, 0xE0, 0x20);
        else
            frontend_create_preview_button("Ultimate", -0xE0, 0, 0xE0, 0x20);

        frontend_create_button("Back", -0xE0, 0, 0xE0, 0x20);

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

    case 12: /* Cup description preview update */
        s_inner_state = 9;
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
 * [7] ScreenQuickRaceMenu (0x4213D0)
 * States: 7 (0x00 - 0x06)
 * Buttons: Change Car, Change Track, OK, Back
 * ======================================================================== */

static void Screen_QuickRaceMenu(void) {
    switch (s_inner_state) {
    case 0: /* Init: validate indices, create info panel, create 4 buttons */
        frontend_init_return_screen(TD5_SCREEN_QUICK_RACE);
        TD5_LOG_D(LOG_TAG, "QuickRaceMenu: init");
        s_anim_complete = 0;
        /* Load background: same as main menu */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Validate car/track indices against max counts */
        if (s_selected_car < 0) s_selected_car = 0;
        if (s_selected_track < 0) s_selected_track = 0;
        if (s_selected_track >= 26) s_selected_track = 0;

        /* Create 0x208 x 200px info panel */
        /* Draw car name and track name */
        /* Show "Locked" if car/track is locked and not in network mode */

        /* From Ghidra: halfW=320, halfH=240
         * ChangeCar:   (halfW-200, halfH-0x67) = (120, 137), size 0x100x0x20 (256x32)
         * ChangeTrack: (120, halfH+0x11)       = (120, 257), size 256x32
         * OK:          (120, halfH+0x89)        = (120, 377), size 0x60x0x20 (96x32)
         * Back:        (halfW-0x58, halfH+0x89) = (232, 377), size 0x70x0x20 (112x32) */
        { int bi;
          bi = frontend_create_button("Change Car",   120, 137, 256, 32);
          if (bi >= 0) s_buttons[bi].is_selector = 1;
          bi = frontend_create_button("Change Track", 120, 257, 256, 32);
          if (bi >= 0) s_buttons[bi].is_selector = 1; }
        frontend_create_button("OK",           120, 377,  96, 32);
        frontend_create_button("Back",         232, 377, 112, 32);

        /* Init left/right arrows on buttons 0 and 1 */
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

    case 4: /* Interactive: arrow input cycles car/track, OK/Back dispatch */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int selected_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (selected_button == 0 && delta != 0) {
                /* Cycle car */
                s_selected_car += delta;
                {
                    int car_max = s_cheat_unlock_all ? 32 : s_total_unlocked_cars;
                    if (s_network_active) car_max = 36;
                    if (s_selected_car < 0) s_selected_car = car_max;
                    if (s_selected_car > car_max) s_selected_car = 0;
                }
                frontend_play_sfx(2); /* ping2.wav cycle */
            }
            if (selected_button == 1 && delta != 0) {
                /* Cycle track */
                s_selected_track += delta;
                {
                    int track_max = s_network_active ? 0x13 : s_total_unlocked_tracks;
                    if (s_selected_track < 0) s_selected_track = track_max - 1;
                    if (s_selected_track >= track_max) s_selected_track = 0;
                }
                TD5_LOG_I(LOG_TAG, "QuickRace track cycle: s_selected_track=%d level=%d name=%s",
                          s_selected_track, td5_asset_level_number(s_selected_track),
                          frontend_get_track_name(s_selected_track));
                frontend_play_sfx(2); /* ping2.wav cycle */
            }
            if (s_button_index == 2) { /* OK */
                /* Block if car or track is locked */
                int car_locked = (!s_cheat_unlock_all && !s_network_active &&
                                  s_selected_car >= 0 && s_selected_car < 37 &&
                                  s_car_lock_table[s_selected_car] != 0);
                int track_locked = (!s_cheat_unlock_all && !s_network_active &&
                                    s_selected_track >= 0 && s_selected_track < 26 &&
                                    s_track_lock_table[s_selected_track] != 0);
                if (car_locked || track_locked) {
                    frontend_play_sfx(10); /* rejection */
                } else {
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 5;
                }
            }
            if (s_button_index == 3) { /* Back */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 5;
            }
        }
        break;

    case 5: /* Prep slide-out */
        frontend_set_cursor_visible(1);
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

static void Screen_ConnectionBrowser(void) {
    switch (s_inner_state) {
    case 0: /* Init: enumerate DirectPlay providers, build list */
        frontend_init_return_screen(TD5_SCREEN_CONNECTION_BROWSER);
        TD5_LOG_D(LOG_TAG, "ConnectionBrowser: init");
        frontend_net_enumerate();
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create buttons: connection list, OK, Back */
        frontend_create_button("Provider", 120, 160, 256, 32);
        frontend_create_button("OK",   -100, 0, 100, 0x20);
        frontend_create_button("Back", -100, 0, 100, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Build list UI */
        s_inner_state = 2;
        break;

    case 2: /* Slide-in (~500ms) */
        if (frontend_update_timed_animation(0x10, 267) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 3;
        }
        break;

    case 3: /* Tick + render */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Flash highlight */
        frontend_present_buffer();
        s_inner_state = 5;
        break;

    case 5: /* Selection interaction */
        if (s_input_ready) {
            if (s_button_index == 0 && s_arrow_input != 0) {
                frontend_play_sfx(2);
            } else if (s_button_index == 1) { /* OK */
                s_return_screen = TD5_SCREEN_SESSION_PICKER;
                s_inner_state = 8;
            } else if (s_button_index == 2) { /* Back */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 8;
            }
        }
        break;

    case 6: /* Highlight browse */
        frontend_present_buffer();
        s_inner_state = 5;
        break;
    case 7: /* Scroll */
        s_inner_state = 5;
        break;

    case 8: /* Slide-out prep */
        frontend_begin_timed_animation();
        s_inner_state = 9;
        break;

    case 9: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
        }
        break;
    }
}

/* ========================================================================
 * [9] RunFrontendSessionPicker (0x419CF0) -- Session browser
 * States: ~8
 * ======================================================================== */

static void Screen_SessionPicker(void) {
    switch (s_inner_state) {
    case 0: /* Init: ConnectionPick, start 3s session enum timer */
        frontend_init_return_screen(TD5_SCREEN_SESSION_PICKER);
        TD5_LOG_D(LOG_TAG, "SessionPicker: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("Session", 120, 160, 256, 32);
        frontend_create_button("Create", -100, 0, 100, 0x20);
        frontend_create_button("OK",     -100, 0, 100, 0x20);
        frontend_create_button("Back",   -100, 0, 100, 0x20);
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
            if (s_button_index == 0 && s_arrow_input != 0) {
                frontend_play_sfx(2);
            } else if (s_button_index == 1) { /* Create */
                s_inner_state = 4; /* create sub-flow */
            } else if (s_button_index == 2) { /* OK / Join */
                s_return_screen = TD5_SCREEN_CREATE_SESSION;
                s_inner_state = 5;
            } else if (s_button_index == 3) { /* Back */
                s_return_screen = TD5_SCREEN_CONNECTION_BROWSER;
                s_inner_state = 5;
            }
        }
        break;

    case 4: /* Create sub-flow -> redirect to create session screen */
        s_return_screen = TD5_SCREEN_CREATE_SESSION;
        s_inner_state = 5;
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
        frontend_create_button("Enter Name", -300, 0, 300, 0x20);
        frontend_create_button("Back",       -100, 0, 100, 0x20);
        memset(s_create_session_name, 0, sizeof(s_create_session_name));
        strcpy(s_create_session_name, "New Session");
        frontend_begin_text_input(s_create_session_name, (int)sizeof(s_create_session_name));
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
        frontend_render_text_input();
        if (s_input_ready && s_button_index == 1) {
            s_return_screen = TD5_SCREEN_SESSION_PICKER;
            s_inner_state = 3;
            break;
        }
        if (frontend_text_input_confirmed()) {
            s_return_screen = TD5_SCREEN_NETWORK_LOBBY;
            s_inner_state = 3;
        }
        break;

    case 3: /* Slide-out */
        s_anim_tick = 0;
        s_inner_state = 4;
        break;

    case 4: /* Session setup -- further states for host/client paths */
    case 5: case 6: case 7: case 8: case 9:
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
 * ======================================================================== */

static void Screen_NetworkLobby(void) {
    switch (s_inner_state) {
    case 0: /* INITIALIZATION */
        frontend_init_return_screen(TD5_SCREEN_NETWORK_LOBBY);
        TD5_LOG_D(LOG_TAG, "NetworkLobby: state 0 - init");

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

        /* Create UI elements: text input bar, message window, status panel,
         * "Change Car", "Start", "Exit" buttons */
        frontend_create_button("",           -0x1D0, 0, 0x1D0, 0x18);  /* text input bar */
        frontend_create_button("Messages",   -0x200, 0, 0x200, 0x80);  /* message window */
        frontend_create_button("Status",     -0xE0,  0, 0xE0,  0x86);  /* status panel */
        frontend_create_button("Change Car", -200,   0, 200,   0x20);
        frontend_create_button("Start",      -0x78,  0, 0x78,  0x20);
        frontend_create_button("Exit",       -0x78,  0, 0x78,  0x20);

        /* Allocate chat input surface */
        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));

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

        /* Check lobby action state */
        if (s_lobby_action == 3) {
            /* Race start received (clients) */
            s_race_active_flag = 1;
            frontend_init_race_schedule();
            return;
        }

        /* Process button input */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 3: /* Change Car */
                s_lobby_action = 1;
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;

            case 4: /* Start */
                s_lobby_action = 2;
                if (frontend_net_is_host()) {
                    s_inner_state = 5; /* player ready check */
                } else {
                    /* Client: send "wait for host" message */
                }
                break;

            case 5: /* Exit */
                s_lobby_action = 1;
                s_dialog_mode = 2;
                frontend_play_sfx(5);
                s_inner_state = 6;
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

        if (ready_count == active_count && active_count >= 2) {
            /* All ready, 2+ players -> start pre-race sequence */
            s_inner_state = 0x0C;
        } else if (active_count < 2) {
            /* Not enough players */
            s_dialog_mode = 1;
            frontend_play_sfx(5);
            s_inner_state = 6;
        } else {
            /* Not all ready yet */
            s_dialog_mode = 0;
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
            frontend_create_button("Yes", -80, 0, 80, 0x20);
            frontend_create_button("No",  -80, 0, 80, 0x20);
        } else {
            frontend_create_button("Ok", -80, 0, 80, 0x20);
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
        /* Tight receive loop until DXPSTART (type 4) received */
        uint8_t recv_buf[256];
        int msg_type = frontend_net_receive(recv_buf, sizeof(recv_buf));
        if (msg_type == 4) {
            /* DXPSTART received -> launch race */
            frontend_init_race_schedule();
        }
        /* Keep waiting if no message yet */
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

        frontend_create_button("Game Options",      -0x130, 0, 0x130, 0x20);
        frontend_create_button("Control Options",   -0x130, 0, 0x130, 0x20);
        frontend_create_button("Sound Options",     -0x130, 0, 0x130, 0x20);
        frontend_create_button("Graphics Options",  -0x130, 0, 0x130, 0x20);
        frontend_create_button("Two Player Options",-0x130, 0, 0x130, 0x20);
        frontend_create_button("OK",                -0x130, 0, 0x130, 0x20);

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
            case 5: /* OK -> apply settings, return to main menu */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 7;
                break;
            }
        }
        break;

    case 7: /* Slide-out prep */
        frontend_set_cursor_visible(1);
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
        /* 7 option rows with left/right arrows:
         * Circuit Laps, Checkpoint Timers, Traffic, Cops,
         * Difficulty, Dynamics, 3D Collisions */
        frontend_create_button("Circuit Laps",      -0x128, 0, 0x128, 0x20); /* 0x41fa1d: width=0x128 */
        frontend_create_button("Checkpoint Timers", -0x128, 0, 0x128, 0x20);
        frontend_create_button("Traffic",           -0x128, 0, 0x128, 0x20);
        frontend_create_button("Cops",              -0x128, 0, 0x128, 0x20);
        frontend_create_button("Difficulty",        -0x128, 0, 0x128, 0x20);
        frontend_create_button("Dynamics",          -0x128, 0, 0x128, 0x20);
        frontend_create_button("3D Collisions",     -0x128, 0, 0x128, 0x20);
        frontend_create_button("OK",                -0x60,  0, 0x60,  0x20); /* 0x41fae3: width=0x60 */
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
             * OK button triggers exit. */
            if (delta != 0) {
                if (active_button == 0) {
                    s_game_option_laps += delta;
                    if (s_game_option_laps < 0) s_game_option_laps = 3;
                    if (s_game_option_laps > 3) s_game_option_laps = 0;
                    s_inner_state = 4;
                } else if (active_button == 1) {
                    s_game_option_checkpoint_timers ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 2) {
                    s_game_option_traffic ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 3) {
                    s_game_option_cops ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 4) {
                    s_game_option_difficulty += delta;
                    if (s_game_option_difficulty < 0) s_game_option_difficulty = 2;
                    if (s_game_option_difficulty > 2) s_game_option_difficulty = 0;
                    s_inner_state = 4;
                } else if (active_button == 5) {
                    s_game_option_dynamics ^= 1;
                    s_inner_state = 4;
                } else if (active_button == 6) {
                    s_game_option_collisions ^= 1;
                    s_inner_state = 4;
                }
            }
            if (s_button_index == 7) { /* OK */
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
 * [14] ScreenControlOptions (0x41DF20) -- Standard options pattern
 * ======================================================================== */

static void Screen_ControlOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_CONTROL_OPTIONS);
        TD5_LOG_D(LOG_TAG, "ControlOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_control_options_surface = frontend_load_tga("Controllers.TGA", "Front End/frontend.zip");
        /* Original layout: Player 1 label, Configure P1, Player 2 label, Configure P2, OK */
        frontend_create_preview_button("Player 1",  -0x100, 0, 0x100, 0x20); /* 0x41e07d: width=0x100 */
        frontend_create_button("Configure",         -0x100, 0, 0x100, 0x20);
        frontend_create_preview_button("Player 2",  -0x100, 0, 0x100, 0x20);
        frontend_create_button("Configure",         -0x100, 0, 0x100, 0x20);
        frontend_create_button("OK",                -0x60,  0, 0x60,  0x20); /* 0x41e0f1: width=0x60 */
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
            /* btn 0 = "Player 1" (disabled label), btn 1 = Configure P1
             * btn 2 = "Player 2" (disabled label), btn 3 = Configure P2
             * btn 4 = OK */
            if (s_button_index == 1 || s_button_index == 3) {
                s_return_screen = TD5_SCREEN_CONTROLLER_BINDING;
                s_inner_state = 7;
            }
            if (s_button_index == 4) {
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
 * 4 rows: SFX Mode, SFX Volume, Music Volume, Music Test + OK
 * ======================================================================== */

static void Screen_SoundOptions(void) {
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_SOUND_OPTIONS);
        TD5_LOG_D(LOG_TAG, "SoundOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_sound_icon_surface       = frontend_load_tga("Stereo.tga",    "Front End/frontend.zip");
        s_sound_icon_mono_surface  = frontend_load_tga("Mono.tga",      "Front End/frontend.zip");
        s_sound_volumebox_surface  = frontend_load_tga("VolumeBox.tga", "Front End/frontend.zip");
        s_sound_volumefill_surface = frontend_load_tga("VolumeFill.tga","Front End/frontend.zip");
        frontend_create_button("SFX Mode",     -0x100, 0, 0x100, 0x20); /* 0x41eb5e: width=0x100 */
        frontend_create_button("SFX Volume",   -0x100, 0, 0x100, 0x20);
        frontend_create_button("Music Volume", -0x100, 0, 0x100, 0x20);
        frontend_create_button("Music Test",   -0x100, 0, 0x100, 0x20);
        frontend_create_button("OK",           -0x60,  0, 0x60,  0x20); /* 0x41ebd0: width=0x60 */
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
            if (delta != 0 && active_button >= 0 && active_button <= 2) {
                if (active_button == 0) {
                    s_sound_option_sfx_mode ^= 1;
                } else if (active_button == 1) {
                    s_sound_option_sfx_volume += delta * 5;
                    if (s_sound_option_sfx_volume < 0) s_sound_option_sfx_volume = 0;
                    if (s_sound_option_sfx_volume > 100) s_sound_option_sfx_volume = 100;
                    td5_sound_set_sfx_volume(s_sound_option_sfx_volume);
                } else if (active_button == 2) {
                    s_sound_option_music_volume += delta * 5;
                    if (s_sound_option_music_volume < 0) s_sound_option_music_volume = 0;
                    if (s_sound_option_music_volume > 100) s_sound_option_music_volume = 100;
                    td5_sound_set_music_volume(s_sound_option_music_volume);
                }
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 3) { /* Music Test */
                s_return_screen = TD5_SCREEN_MUSIC_TEST;
                s_inner_state = 7;
            } else if (s_button_index == 4) { /* OK */
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
        TD5_LOG_D(LOG_TAG, "DisplayOptions: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_init_display_mode_state();
        frontend_create_button("Resolution",    -0x120, 0, 0x120, 0x20); /* 0x420484: width=0x120 */
        frontend_create_button("Fogging",       -0x120, 0, 0x120, 0x20);
        frontend_create_button("Speed Readout", -0x120, 0, 0x120, 0x20);
        frontend_create_button("Camera Damping",-0x120, 0, 0x120, 0x20);
        frontend_create_button("OK",            -0x60,  0, 0x60,  0x20); /* 0x420522: width=0x60 */
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

            if (active_button == 0 && delta != 0 && s_display_mode_count > 0) {
                s_display_mode_index += delta;
                if (s_display_mode_index < 0) s_display_mode_index = s_display_mode_count - 1;
                if (s_display_mode_index >= s_display_mode_count) s_display_mode_index = 0;
                td5_plat_apply_display_mode(
                    s_display_modes[s_display_mode_index].width,
                    s_display_modes[s_display_mode_index].height,
                    s_display_modes[s_display_mode_index].bpp);
                changed = 1;
            } else if (active_button == 1 && delta != 0) {
                s_display_fog_enabled = !s_display_fog_enabled;
                g_td5.ini.fog_enabled = s_display_fog_enabled;
                changed = 1;
            } else if (active_button == 2 && delta != 0) {
                s_display_speed_units = !s_display_speed_units;
                changed = 1;
            } else if (active_button == 3 && delta != 0) {
                s_display_camera_damping += delta;
                if (s_display_camera_damping < 0) s_display_camera_damping = 0;
                if (s_display_camera_damping > 9) s_display_camera_damping = 9;
                changed = 1;
            } else if (s_button_index == 4) {
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
 * [17] ScreenTwoPlayerOptions (0x420C70)
 * 2 rows: Split Screen, Catch-Up + OK
 * ======================================================================== */

static void Screen_TwoPlayerOptions(void) {
    switch (s_inner_state) {
    case 0:
        /* [CONFIRMED @ 0x420C70 case 0]: init state */
        frontend_init_return_screen(TD5_SCREEN_TWO_PLAYER_OPTIONS);
        TD5_LOG_D(LOG_TAG, "TwoPlayerOptions: init split_mode=%d", s_split_screen_mode);
        s_split_screen_surface = frontend_load_tga("SplitScreen.tga", "Front End/frontend.zip");
        /* [CONFIRMED @ 0x420d22]: SNK_SplitScreenButTxt at x=-0x100, width=0x100 */
        frontend_create_button("Split Screen", -0x100, 0, 0x100, 0x20);
        /* [CONFIRMED @ 0x420d33]: SNK_CatchupTxt at x=-0x100, width=0x100 */
        frontend_create_button("Catch-Up",    -0x100, 0, 0x100, 0x20);
        /* [CONFIRMED @ 0x420d43]: SNK_OkButTxt at x=-0x100, width=0x60 */
        frontend_create_button("OK",          -0x60,  0, 0x60,  0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3: /* Slide-in (~1200ms) [CONFIRMED @ 0x420D80 case 3] */
        if (frontend_update_timed_animation(0x27, 650) >= 1.0f) {
            s_anim_complete = 1;
            s_inner_state = 4;
        }
        break;
    case 4: case 5:
        /* [CONFIRMED @ 0x420E80 case 4/5]: redraw overlay, bump state */
        s_inner_state = 6;
        break;
    case 6:
        /* [CONFIRMED @ 0x420F40 case 6]: input handling
         * DAT_0049b690 = arrow delta; button 0 = split screen toggle,
         * button 1 = catch-up level, button 2 = OK */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = (s_button_index >= 0) ? s_button_index : s_selected_button;
            if (active_button == 0 && delta != 0) {
                /* [CONFIRMED @ 0x42106B]: g_twoPlayerSplitMode = (delta + g_twoPlayerSplitMode) & 1 */
                s_split_screen_mode = (delta + s_split_screen_mode) & 1;
                /* Sync the split-screen ON flag into s_two_player_mode bit 2 */
                if (s_split_screen_mode)
                    s_two_player_mode |= 4;
                else
                    s_two_player_mode &= ~4;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == 1 && delta != 0) {
                /* [CONFIRMED @ 0x42107A]: DAT_00465ff8 += delta; clamped 0..9 */
                /* Catch-up level stored in s_two_player_mode bits 3+ (port approximation) */
                s_two_player_mode ^= 8;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) {
                /* [CONFIRMED @ 0x4210F7]: OK pressed: blit secondary, advance to slide-out */
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
 * [18] ScreenControllerBindingPage (0x40FE00) -- ~20 states
 * ======================================================================== */

static void Screen_ControllerBinding(void) {
    switch (s_inner_state) {
    case 0: /* Init: detect joystick, read current config */
        frontend_init_return_screen(TD5_SCREEN_CONTROLLER_BINDING);
        TD5_LOG_D(LOG_TAG, "ControllerBinding: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        s_joypad_icon_surface   = frontend_load_tga("JoypadIcon.tga",   "Front End/frontend.zip");
        s_joystick_icon_surface = frontend_load_tga("JoystickIcon.tga", "Front End/frontend.zip");
        s_keyboard_icon_surface = frontend_load_tga("KeyboardIcon.tga", "Front End/frontend.zip");
        s_nocontroller_surface  = frontend_load_tga("NoControllerText.tga", "Front End/frontend.zip");
        frontend_create_button("Detect Input", -220, 0, 220, 0x20);
        frontend_create_button("OK", -120, 0, 120, 0x20);
        s_inner_state = 1;
        break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        /* Binding page setup states */
        s_inner_state++;
        break;
    case 9: /* Slide-in */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x12) s_inner_state = 10;
        break;
    case 10: /* Interactive binding poll */
        if (s_input_ready) {
            if (s_button_index == 0 || s_arrow_input != 0) {
                frontend_play_sfx(2);
                s_inner_state = 11;
            } else if (s_button_index == 1) {
                s_inner_state = 14;
            }
        }
        break;
    case 11: case 12: case 13:
        s_inner_state = 10;
        break;
    case 14: /* Confirm */
    case 15: case 16: case 17: case 18:
        if (s_inner_state == 14) {
            s_anim_tick = 0;
            s_inner_state = 15;
        } else {
            s_anim_tick += 2;
            if (s_anim_tick >= 16) {
                s_inner_state = 19;
            }
        }
        break;
    case 19: /* Special pedal sub-flow or exit */
        td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        break;
    default:
        td5_frontend_set_screen(TD5_SCREEN_CONTROL_OPTIONS);
        break;
    }
}

/* ========================================================================
 * [19] ScreenMusicTestExtras (0x418460) -- CD audio jukebox
 * States: 9
 * ======================================================================== */

static int s_music_test_track_idx; /* 0..11 */

static void Screen_MusicTestExtras(void) {
    switch (s_inner_state) {
    case 0: /* Fade transition + init */
        frontend_init_return_screen(TD5_SCREEN_MUSIC_TEST);
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: init");
        /* Release gallery images, load band photos */
        /* Create title, track name surface, now-playing surface */
        /* Draw initial track info */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("Select Track", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK",           -0xE0, 0, 0xE0, 0x20);
        s_music_test_track_idx = 0;
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
            if (s_button_index == 0 && delta != 0) {
                /* Cycle track index 0..11 */
                s_music_test_track_idx += delta;
                if (s_music_test_track_idx < 0) s_music_test_track_idx = 11;
                if (s_music_test_track_idx > 11) s_music_test_track_idx = 0;
                /* Redraw track name surface */
            }
            if (s_button_index == 0 && s_arrow_input == 0) {
                /* Confirm "Select Track" -> play CD audio */
                frontend_cd_play(s_music_test_track_idx);
                /* Update "Now Playing" with band name + song title */
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
            /* Release band surfaces, reload gallery surfaces */
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

        /* Determine car roster range by game type */
        s_car_roster_min = 0;
        switch (s_selected_game_type) {
        case 2: /* Era: 0..15 */
            s_car_roster_max = 15;
            break;
        case 5: /* Masters: use random roster */
            s_car_roster_max = 14; /* index into s_masters_roster[] */
            break;
        case 8: /* Cop Chase: 33-36 */
            s_car_roster_min = 33;
            s_car_roster_max = 36;
            break;
        default:
            if (s_network_active) {
                s_car_roster_max = s_cheat_unlock_all ? 36 : 32;
            } else {
                s_car_roster_max = s_total_unlocked_cars - 1;
            }
            break;
        }

        /* Handle 2P mode:
         * (two_player_mode & 3) == 1: P1 selecting
         * (two_player_mode & 3) == 2: P2 selecting */
        if ((s_two_player_mode & 3) == 2) {
            /* Create P2 label */
        }

        /* Clamp initial car to valid range */
        if (s_selected_car < s_car_roster_min) s_selected_car = s_car_roster_min;
        if (s_selected_car > s_car_roster_max) s_selected_car = s_car_roster_max;

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
        } else if (frontend_update_timed_animation(75, 2500) >= 1.0f) {
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
        frontend_create_button("Car",   46, 169, 168, 32);
        frontend_create_button("Paint", 46, 209, 168, 32);
        frontend_create_button("Stats", 46, 249, 168, 32);
        frontend_create_button(s_selected_transmission ? "Manual" : "Automatic",
                               46, 289, 168, 32);
        frontend_create_button("OK",   46, 329,  64, 32);
        if (!s_network_active)
            frontend_create_button("Back", 118, 329, 96, 32);

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
            s_inner_state = 7;
        }
        break;

    case 7: /* Main interaction loop: car preview + input */
        /* Render car preview overlay */
        if (s_input_ready) {
            int delta = frontend_option_delta();
            int active_button = s_button_index;
            if (active_button < 0 && delta != 0 &&
                (s_selected_button == 0 || s_selected_button == 1 || s_selected_button == 3)) {
                active_button = s_selected_button;
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
                        s_selected_car += delta;
                        if (s_selected_car < s_car_roster_min) s_selected_car = s_car_roster_max;
                        if (s_selected_car > s_car_roster_max) s_selected_car = s_car_roster_min;
                    }
                    s_inner_state = 10; /* trigger new car image load */
                }
                break;

            case 1: /* Paint: L/R cycle paint 0-3 */
                if (delta != 0) {
                    /* Disabled for cop cars (0x1C-0x24) */
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

            case 2: /* Config -> spec sheet sub-screen */
                s_car_preview_overlay = 1;
                s_inner_state = 15;
                break;

            case 3: /* Auto/Manual toggle */
                if (s_selected_game_type != 7 && (s_button_index >= 0 || delta != 0)) { /* not Time Trials */
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

    case 9: /* Unused */
        s_inner_state = 7;
        break;

    case 10: /* Clear car preview area, prep for new image load */
        s_car_spec_car = -1; /* invalidate spec cache on car/paint change */
        s_anim_complete = 1;
        s_inner_state = 11;
        break;

    case 11: /* Old car slides out to the right (~433ms, 13 frames @30fps) — 0x40DFC0 state 11 */
    {
        int actual_car = frontend_current_car_index();
        if (s_car_preview_next_surface <= 0) {
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

    case 13:
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

    case 16: /* Unused (was "return from config") — fall through to 7 */
        s_car_preview_overlay = 2;
        s_inner_state = 7;
        break;

    case 17: /* Info sub-screen: render SNK_Info_Values (10 entries) */
        s_inner_state = 18;
        break;

    case 18: /* Return from info */
        s_car_preview_overlay = 2;
        s_inner_state = 7;
        break;

    case 19: /* Unused */
        s_inner_state = 7;
        break;

    case 0x14: /* Prep slide-out */
        frontend_set_cursor_visible(0);
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

        /* Two-player flow dispatch */
        if ((s_two_player_mode & 3) == 1) {
            /* P1 selected: save P1 choices, re-enter for P2 */
            s_selected_car = actual_car; /* finalize */
            s_two_player_mode = 6; /* set bit flags for P2 round */
            td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
            return;
        }
        if ((s_two_player_mode & 3) == 2) {
            /* P2 selected: save P2 choices, proceed to track selection */
            s_p2_car = actual_car;
            s_p2_paint = s_selected_paint;
            s_p2_config = s_selected_config;
            s_p2_transmission = s_selected_transmission;
            td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
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
                s_selected_car = actual_car;
                s_drag_carselect_pass = 1;
                TD5_LOG_I(LOG_TAG, "CarSelect: drag-race pass1 car=%d → re-enter for car 2",
                          actual_car);
                td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                return;
            } else {
                s_p2_car = actual_car;
                s_p2_paint = s_selected_paint;
                s_p2_config = s_selected_config;
                s_p2_transmission = s_selected_transmission;
                s_drag_carselect_pass = 0;
                TD5_LOG_I(LOG_TAG, "CarSelect: drag-race pass2 car=%d → skip track select, start race",
                          actual_car);
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
        frontend_create_button("Track",     120,  97, 224, 32); /* with L/R arrows */
        frontend_create_button("Forwards",  120, 145, 224, 32); /* direction toggle */
        frontend_create_button("OK",        120, 377,  96, 32);
        /* Quick Race mode: no Back button */
        if (s_flow_context != 2) {
            frontend_create_button("Back", 232, 377, 112, 32);
        }

        /* Create 0x128 x 0xB8 info surface */
        frontend_load_tga("Front_End/TrackSelect.tga", "Front_End/FrontEnd.zip");

        s_track_direction = 0;
        s_track_switch_tick = 16; /* holds preview settled during button slide-in (state 3); reset to 0 in state 5 */
        frontend_load_selected_track_preview();
        frontend_begin_timed_animation();
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present + tick */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        /* Hide direction button if track has no reverse */
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
                s_inner_state = 5;
            }

            if (selected_button == 1 && (delta != 0 || s_button_index == 1)) {
                /* Direction toggle: 0=Forwards, 1=Backwards */
                /* Only if track supports reverse */
                s_track_direction = !s_track_direction;
                strncpy(s_buttons[1].label,
                        s_track_direction ? "Backwards" : "Forwards",
                        sizeof(s_buttons[1].label) - 1);
                s_buttons[1].label[sizeof(s_buttons[1].label) - 1] = '\0';
            }

            if (s_button_index == 2) { /* OK */
                /* Lock enforcement */
                int locked = (s_selected_track >= 0 && s_selected_track < 26 &&
                             s_track_lock_table[s_selected_track] != 0 &&
                             !s_network_active && !s_cheat_unlock_all);
                if (locked) {
                    frontend_play_sfx(10);
                } else {
                    g_td5.reverse_direction = s_track_direction;
                    s_return_screen = -1; /* launch race */
                    s_inner_state = 6;
                }
            }

            if (s_button_index == 3) { /* Back */
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
    case 0: /* Init: load first image (Legals5 first, then mugshots in order) */
        frontend_init_return_screen(TD5_SCREEN_EXTRAS_GALLERY);
        s_gallery_pic_index = 0;
        s_gallery_visited_mask = 1;
        if (s_gallery_pic_surface > 0) {
            frontend_release_surface(s_gallery_pic_surface);
            s_gallery_pic_surface = 0;
        }
        s_gallery_pic_surface = frontend_load_tga(s_gallery_names[0], GALLERY_ZIP);
        s_anim_start_ms = 0;
        s_anim_tick = 0;
        TD5_LOG_I(LOG_TAG, "ExtrasGallery: init, %d images total", GALLERY_PIC_COUNT);
        s_inner_state = 1;
        break;

    case 1: /* Brief delay to prevent input bleed from menu (~39 ticks) */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x27) {
            s_anim_start_ms = td5_plat_time_ms();
            s_inner_state = 2;
        }
        break;

    case 2: /* Auto-advance through all images; ESC handled by global handler -> quit */
        {
            uint32_t now = td5_plat_time_ms();
            /* Advance image every 4000ms [UNCERTAIN: original uses per-frame scroll counter
             * starting at 0x27F; exact per-image duration not confirmed] */
            if (now - s_anim_start_ms >= 4000) {
                s_gallery_pic_index++;
                if (s_gallery_pic_index >= GALLERY_PIC_COUNT) {
                    /* All images shown -- original exits game here */
                    TD5_LOG_I(LOG_TAG, "ExtrasGallery: all %d images shown, quitting", GALLERY_PIC_COUNT);
                    if (s_gallery_pic_surface > 0) {
                        frontend_release_surface(s_gallery_pic_surface);
                        s_gallery_pic_surface = 0;
                    }
                    frontend_post_quit();
                    break;
                }
                if (s_gallery_pic_surface > 0) {
                    frontend_release_surface(s_gallery_pic_surface);
                    s_gallery_pic_surface = 0;
                }
                s_gallery_pic_surface = frontend_load_tga(
                    s_gallery_names[s_gallery_pic_index], GALLERY_ZIP);
                s_gallery_visited_mask |= (1 << s_gallery_pic_index);
                s_anim_start_ms = now;
                TD5_LOG_I(LOG_TAG, "ExtrasGallery: image %d/%d: %s",
                          s_gallery_pic_index + 1, GALLERY_PIC_COUNT,
                          s_gallery_names[s_gallery_pic_index]);
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
        frontend_create_button(NULL, 115,  93, 520, 32);  /* nav bar: x=115, y=93 */
        frontend_create_button("OK", 120, 416,  96, 32);  /* OK button at bottom */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        s_score_category_index = 0;
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
            frontend_set_cursor_visible(0);
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
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        frontend_begin_timed_animation();
        s_inner_state = 8;
        break;

    case 8: /* Slide-out (~500ms) */
        if (frontend_update_timed_animation(16, 267) >= 1.0f) {
            /* Return to caller or init race */
            if (s_return_screen == -1) {
                frontend_init_race_schedule();
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
            s_results_rerace_flag = 1;
        }

        /* Panel body is drawn per-frame by frontend_render_race_results_overlay.
         *
         * Original @ 0x0042278B creates a 520x32 blank click-catcher button
         * (idx 0) + 96x32 SNK_OkButTxt (idx 1); state 6 exits on button_index<2.
         * Port creates only the OK button: the blank spacer renders as a huge
         * visible rect in the port's live-draw model, and a single confirm
         * button (idx 0) satisfies state 6's exit check.
         *
         * Explicit-placed at panel bottom: center_x - 48 = 272, y = 400.
         * Width 0x60 = 96 per SNK_OkButTxt dimensions @ 0x004227A0. */
        frontend_create_button("OK", FE_CENTER_X - 48, 400, 0x60, 0x20);

        /* If player disqualified/DNF -> route to cup failed */
        s_results_cup_complete = 0;
        s_results_skip_display = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present buffer, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x12) {
            /* If skip flag or disqualified, jump to cleanup */
            if (s_results_skip_display) {
                s_inner_state = 0x0C;
            } else {
                s_inner_state = 4;
            }
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R browse racer slots (0-5), confirm exits.
             * Original @ 0x004229DA: button_index >= 0 && < 2 -> state 0x0B.
             * [CONFIRMED @ 0x004229DA] DAT_00497a68 cycles by DAT_0049b690 (arrow delta),
             * skips slots with state == 3 (disabled). Drag: masked & 1 for 2-slot only. */
        if (s_input_ready) {
            if (s_arrow_input != 0) {
                /* Cycle through racer slots, skip disabled.
                 * [CONFIRMED @ 0x00422A22] Wrap: [0..5] with 6 -> 0 and -1 -> 5. */
                s_score_category_index += s_arrow_input; /* reuse for browsed slot */
                if (s_selected_game_type == 7) {
                    /* Drag: only 2 slots [CONFIRMED @ 0x00422A02] masked & 1 */
                    s_score_category_index &= 1;
                } else {
                    if (s_score_category_index >= 6) s_score_category_index = 0;
                    if (s_score_category_index < 0)  s_score_category_index = 5;
                }
                /* Skip disabled slots — up to 6 iterations */
                for (int _skip = 0;
                     _skip < 6 && td5_game_get_slot_state(s_score_category_index) == 3;
                     _skip++) {
                    s_score_category_index += s_arrow_input;
                    if (s_score_category_index >= 6) s_score_category_index = 0;
                    if (s_score_category_index < 0)  s_score_category_index = 5;
                }
                TD5_LOG_D(LOG_TAG, "RaceResults state 6: browsing slot %d",
                          s_score_category_index);
            }
            if (s_button_index >= 0 && s_button_index < 2) { /* confirm -> exit */
                TD5_LOG_I(LOG_TAG, "RaceResults: state 6 -> 0x0B (confirm, btn=%d)",
                          s_button_index);
                s_anim_tick = 0;
                s_inner_state = 0x0B;
            }
        }
        break;

    case 7: case 8: /* Slide left animation: 17 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 17) {
            s_inner_state = 6; /* back to interactive */
        }
        break;

    case 9: case 10: /* Slide right animation: 17 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 17) {
            s_inner_state = 6;
        }
        break;

    case 0x0B: /* Exit slide-out: 17 frames */
        s_anim_tick = 0;
        s_inner_state = 0x0C;
        break;

    case 0x0C: /* Cleanup: release all surfaces & the state-0 OK button.
                * Original @ 0x00422CEE: ReleaseTrackedFrontendSurface + clear
                * button table. Port collapses to frontend_reset_buttons. */
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
            const int RR_BX = FE_CENTER_X - 0x90;     /* 0x120 / 2 = 0x90 */
            const int RR_BW = 0x120;                  /* 288 */
            const int RR_BH = 0x20;                   /* 32 */
            const int RR_Y0 = FE_CENTER_Y - 0x8F;     /* 97 */
            const int RR_Y1 = FE_CENTER_Y - 0x5F;     /* 145 */
            const int RR_Y2 = FE_CENTER_Y - 0x2F;     /* 193 */
            const int RR_Y3 = FE_CENTER_Y + 0x01;     /* 241 */
            const int RR_Y4 = FE_CENTER_Y + 0x31;     /* 289 */

            if (s_selected_game_type < 1 || s_selected_game_type == 7 || s_selected_game_type == 9) {
                /* Quick Race / Time Trial / Drag */
                frontend_create_button("Race Again",      RR_BX, RR_Y0, RR_BW, RR_BH);
                frontend_create_button("View Replay",     RR_BX, RR_Y1, RR_BW, RR_BH);
                frontend_create_button("View Race Data",  RR_BX, RR_Y2, RR_BW, RR_BH);
                frontend_create_button("Select New Car",  RR_BX, RR_Y3, RR_BW, RR_BH);
                frontend_create_button("Quit",            RR_BX, RR_Y4, RR_BW, RR_BH);
            } else {
                /* Cup Race (types 1-6) */
                int next_valid = ConfigureGameTypeFlags();
                frontend_create_button("Next Cup Race",    RR_BX, RR_Y0, RR_BW, RR_BH);
                frontend_create_button("View Replay",      RR_BX, RR_Y1, RR_BW, RR_BH);
                frontend_create_button("View Race Data",   RR_BX, RR_Y2, RR_BW, RR_BH);
                frontend_create_button("Save Race Status", RR_BX, RR_Y3, RR_BW, RR_BH);

                if (!next_valid) {
                    /* Cup-complete path @ 0x00422FD8: last button is "OK",
                     * g_frontendButtonIndex=1, s_results_cup_complete=1. */
                    frontend_create_button("OK", RR_BX, RR_Y4, RR_BW, RR_BH);
                    s_results_cup_complete = 1;
                } else {
                    frontend_create_button("Quit", RR_BX, RR_Y4, RR_BW, RR_BH);
                }

                /* Masters (type 5): special progression (PRE-EXISTING branch
                 * reused verbatim; flag inversion from original is
                 * tracked in prior memory and not touched by this fix) */
                if (s_selected_game_type == 5 && !s_results_rerace_flag &&
                    s_race_within_series != 9) {
                    s_inner_state = 0x15;
                    return;
                }
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
                 *       then re-init race (replay file was saved at race start). */
                TD5_LOG_I(LOG_TAG, "RaceResults: View Replay selected");
                td5_input_set_replay_mode(1);
                td5_input_set_playback_active(1);
                frontend_init_race_schedule();
                break;

            case 2: /* View Race Data / High Score Table */
                td5_frontend_set_screen(TD5_SCREEN_HIGH_SCORE);
                break;

            case 3: /* Save Race Status / Select New Car */
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_inner_state = 0x11; /* save cup data */
                } else {
                    td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
                }
                break;

            case 4: /* Quit */
                if (s_selected_game_type < 1 || s_selected_game_type == 7 || s_selected_game_type == 9) {
                    /* Go to name entry */
                    td5_frontend_set_screen(TD5_SCREEN_NAME_ENTRY);
                } else {
                    /* Check cup completion */
                    if (s_results_cup_complete) {
                        td5_frontend_set_screen(TD5_SCREEN_CUP_WON);
                    } else {
                        td5_frontend_set_screen(TD5_SCREEN_CUP_FAILED);
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
            frontend_create_button("OK",     FE_CENTER_X - 0x30, FE_CENTER_Y + 0x31, 0x60,  0x20);
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

    case 2: /* Text input active */
        frontend_render_text_input();
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
                        e->avg_speed = td5_game_get_result_avg_speed(0);
                        e->top_speed = td5_game_get_result_top_speed(0);
                    } else {
                        int race_count = (s_race_within_series > 0) ? s_race_within_series : 1;
                        int32_t raw_avg = td5_game_get_result_avg_speed(0);
                        e->avg_speed = raw_avg / race_count;
                        e->top_speed = td5_game_get_result_top_speed(0);
                    }
                    s_score_insert_pos = ins_pos;

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
            /* For cup types (1-7): reset re-race flag */
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
        frontend_create_button("OK", -100, 0, 100, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick += 2;
        /* Dialog slides from right (24px/frame), button from left */
        if (s_anim_tick >= 0x10) {
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

        /* Apply cup unlock progression and save to Config.td5 */
        {
            int new_unlocks = td5_save_apply_cup_unlocks((int)s_selected_game_type);
            TD5_LOG_I(LOG_TAG, "CupWon: game_type=%d new_unlocks=%d", (int)s_selected_game_type, new_unlocks);

            /* Persist updated unlock state */
            td5_save_write_config(NULL);

            /* Refresh frontend lock tables from save system */
            td5_save_get_car_lock_table(s_car_lock_table, 37);
            td5_save_get_track_lock_table(s_track_lock_table, 26);
            if (td5_save_get_all_cars_unlocked()) {
                s_total_unlocked_cars = 37;
            } else {
                s_total_unlocked_cars = td5_save_get_max_unlocked_car();
                if (s_total_unlocked_cars < 21) s_total_unlocked_cars = 21;
            }
            {
                int t;
                s_total_unlocked_tracks = 20;
                for (t = 20; t < 26; t++) {
                    if (s_track_lock_table[t] == 0)
                        s_total_unlocked_tracks = t + 1;
                }
            }
        }

        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create 0x198 x 0xC4 dialog surface */
        /* Draw: Congrats (y=0), You Have Won (y=0x38), race type name (y=0x54) */
        /* If unlocked car: draw at y=0x8C */
        /* If unlocked track: draw at y=0xA8 */
        frontend_create_button("OK", -100, 0, 100, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
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
        frontend_create_button("OK", -100, 0, 100, 0x20);
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
        frontend_create_button("OK", -100, 0, 100, 0x20);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x10) {
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
