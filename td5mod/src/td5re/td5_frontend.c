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
#include "td5_platform.h"
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
static int  s_snap_opp_car, s_snap_opp_paint, s_snap_opp_trans, s_snap_opp_config;

/* Masters roster (type 5): 15 random car slots, 6 marked AI */
static int  s_masters_roster[15];
static int  s_masters_roster_flags[15]; /* 0=available, 1=AI, 2=taken */

/* Cup series schedule table (from 0x4640A4) */
static const int s_cup_schedules[][13] = {
    /* [1] Championship */ { 0, 1, 2, 3, 99, -1,-1,-1,-1,-1,-1,-1,-1 },
    /* [2] Era          */ { 4, 16, 6, 7, 5, 17, 99, -1,-1,-1,-1,-1,-1 },
    /* [3] Challenge    */ { 0, 1, 2, 3, 15, 8, 99, -1,-1,-1,-1,-1,-1 },
    /* [4] Pitbull      */ { 0, 1, 2, 3, 15, 8, 11, 13, 99, -1,-1,-1,-1 },
    /* [5] Masters      */ { 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 99, -1,-1 },
    /* [6] Ultimate     */ { 0, 1, 2, 3, 15, 8, 11, 13, 10, 12, 9, 14, 99 },
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
static int s_carsel_bar_surface = 0;
static int s_carsel_curve_surface = 0;
static int s_carsel_topbar_surface = 0;
static int s_graphbars_surface = 0;
static int s_car_preview_surface = 0;
static char s_car_spec[17][48]; /* config.nfo fields (0-16) for stats sub-screen */
static int  s_car_spec_car = -1; /* which car index is currently cached */
static int s_track_preview_surface = 0;
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
                                      int page_override, int *w_out, int *h_out);
static int frontend_load_tga_black_key(const char *name, const char *archive);
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
    default: return -1;
    }
}

static int frontend_ensure_title_texture(TD5_ScreenIndex screen) {
    const char *entry = frontend_get_title_tga_for_screen(screen);
    int page = frontend_get_title_page_for_screen(screen);

    if (!entry || page < 0) return 0;
    if (screen >= 0 && screen < TD5_SCREEN_COUNT && s_title_tex_page[screen] == page) return 1;
    if (!frontend_load_tga_colorkey(entry, "Front End/frontend.zip", page,
                                    &s_title_tex_w[screen], &s_title_tex_h[screen])) {
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
    return frontend_load_tga(entry, s_car_zip_paths[car_index]);
}

/* Draw a surface OPAQUE: all pixels (including black) rendered as-is, no color key.
 * Matches original game's BltFast without SRCCOLORKEY (Copy16BitSurfaceRect flag 0x10).
 * Used for UI chrome (CarSelBar1, CarSelCurve, CarSelTopBar) where black is part of the design. */
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
    int sz = 0;
    void *data = NULL;
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

    data = td5_asset_open_and_read(bare_name, real_archive, &sz);
    if (!data || sz <= 0) {
        TD5_LOG_W(LOG_TAG, "LoadTGA failed: %s from %s", name, archive);
        return 0;
    }

    void *pixels = NULL;
    int w = 0, h = 0;
    if (!td5_asset_decode_tga(data, (size_t)sz, &pixels, &w, &h) || !pixels) {
        free(data);
        return 0;
    }
    free(data);

    /* TGA decode already outputs BGRA matching D3D11 B8G8R8A8_UNORM — no swap needed */

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
                                       int dest_page, int *out_w, int *out_h) {
    int sz = 0;
    void *data = NULL;

    const char *bare_name = name;
    const char *slash = strrchr(name, '/');
    if (slash) bare_name = slash + 1;
    slash = strrchr(bare_name, '\\');
    if (slash) bare_name = slash + 1;

    const char *real_archive = archive;
    if (strstr(archive, "FrontEnd.zip") || strstr(archive, "frontend.zip"))
        real_archive = "Front End/frontend.zip";

    data = td5_asset_open_and_read(bare_name, real_archive, &sz);
    if (!data || sz <= 0) {
        TD5_LOG_W(LOG_TAG, "LoadTGA_CK failed: %s from %s", name, archive);
        return 0;
    }

    void *pixels = NULL;
    int w = 0, h = 0;
    if (!td5_asset_decode_tga(data, (size_t)sz, &pixels, &w, &h) || !pixels) {
        free(data);
        return 0;
    }
    free(data);

    /* Color key: red pixels (R=255,G=0,B=0) become transparent.
     * TGA decode outputs BGRA: byte0=B, byte1=G, byte2=R, byte3=A */
    {
        uint8_t *p = (uint8_t *)pixels;
        int count = w * h;
        for (int ci = 0; ci < count; ci++) {
            uint8_t b_val = p[0], g_val = p[1], r_val = p[2];
            if (r_val == 255 && g_val == 0 && b_val == 0) {
                p[3] = 0;
            }
            if (dest_page == SHARED_PAGE_FONT &&
                b_val < 8 && g_val < 8 && r_val < 8) {
                p[3] = 0;
            }
            p += 4;
        }
    }

    if (td5_plat_render_upload_texture(dest_page, pixels, w, h, 2)) {
        if (dest_page == SHARED_PAGE_FONT) {
            frontend_init_font_metrics_from_pixels((const uint8_t *)pixels, w, h);
        }
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        TD5_LOG_I(LOG_TAG, "LoadTGA_CK OK: %s -> page=%d %dx%d", bare_name, dest_page, w, h);
        free(pixels);
        return 1;
    }

    free(pixels);
    return 0;
}

/* Load a TGA into a surface slot, making near-black pixels (R<8,G<8,B<8) transparent.
 * Track preview TGAs use black as the background color key. */
static int frontend_load_tga_black_key(const char *name, const char *archive) {
    int sz = 0;
    void *data = NULL;
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

    data = td5_asset_open_and_read(bare_name, real_archive, &sz);
    if (!data || sz <= 0) {
        TD5_LOG_W(LOG_TAG, "LoadTGA_BK failed: %s from %s", name, archive);
        return 0;
    }

    void *pixels = NULL;
    int w = 0, h = 0;
    if (!td5_asset_decode_tga(data, (size_t)sz, &pixels, &w, &h) || !pixels) {
        free(data);
        return 0;
    }
    free(data);

    /* Black colorkey: near-black pixels (all channels < 8) become transparent.
     * TGA pixels decoded as BGRA. */
    {
        uint8_t *p = (uint8_t *)pixels;
        int count = w * h;
        for (int ci = 0; ci < count; ci++) {
            if (p[0] < 8 && p[1] < 8 && p[2] < 8) {
                p[3] = 0;
            }
            p += 4;
        }
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
        free(pixels);
        TD5_LOG_I(LOG_TAG, "LoadTGA_BK OK: %s → slot=%d page=%d %dx%d", bare_name, slot, page, w, h);
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

/* Check whether the level zip for a given track index is present on disk. */
static int frontend_track_level_exists(int track_index) {
    char zippath[32];
    if (track_index < 0) return 1; /* -1 = random, always "valid" */
    snprintf(zippath, sizeof(zippath), "level%03d.zip",
             td5_asset_level_number(track_index));
    return td5_plat_file_exists(zippath);
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
    s_track_preview_surface = frontend_load_tga_black_key(entry, "Front End/Tracks/Tracks.zip");
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

static void frontend_draw_string(int surface, const char *str_id, int x, int y) {
    (void)surface; (void)str_id; (void)x; (void)y;
    /* Text rendering via font atlas — implemented in render_ui_rects below */
}

static void frontend_draw_small_string(int surface, const char *str_id, int x, int y) {
    (void)surface; (void)str_id; (void)x; (void)y;
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

static void frontend_init_race_schedule(void) {
    int i;
    g_td5.race_requested = 1;
    g_td5.car_index   = frontend_current_car_index();
    g_td5.track_index = (s_current_screen == TD5_SCREEN_ATTRACT_MODE)
                        ? s_attract_track
                        : s_selected_track;

    /* Assign AI car types per slot from the cup schedule.
     * For quick race the schedule defaults to ext_id 0 for all slots → XKR (type 7).
     * Cup modes should override s_cup_schedule_track[] before calling this. */
    for (i = 1; i < TD5_MAX_RACER_SLOTS; i++) {
        g_td5.ai_car_indices[i] = s_ext_car_to_type_index[0]; /* default: XKR */
    }

    TD5_LOG_I(LOG_TAG, "InitializeRaceSeriesSchedule: car=%d (resolved=%d) track=%d type=%d",
              s_selected_car, g_td5.car_index, g_td5.track_index, s_selected_game_type);
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
            frontend_play_sfx(4);
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
        for (int i = 0; i < FE_MAX_BUTTONS; i++) {
            if (!s_buttons[i].active || s_buttons[i].disabled) continue;
            if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
                s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
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
                        frontend_play_sfx(4);
                        TD5_LOG_I(LOG_TAG, "Button pressed: index=%d label=\"%s\" source=mouse",
                                  s_button_index, s_buttons[s_button_index].label);
                    }
                } else {
                    /* Different button: select it (first click selects, second confirms) */
                    s_selected_button = i;
                    s_selection_from_mouse = 1;
                    frontend_play_sfx(2);
                }
                break;
            }
        }
        s_mouse_click_latched = 0;
    }

    /* Mouse hover updates selected button */
    if (mouse_moved) {
        for (int i = 0; i < FE_MAX_BUTTONS; i++) {
            if (!s_buttons[i].active) continue;
            if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
                s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
                s_selected_button = i;
                s_selection_from_mouse = 1;
                break;
            }
        }
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
static void frontend_fill_rect(int layer, int x, int y, int w, int h, uint32_t color);
static void fe_draw_option_arrows(int btn_idx, float sx, float sy);
static void frontend_load_bg_gallery(void);

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
    }
}

/* --- Frontend Surface Recovery --- */

static void frontend_recover_surfaces(void) {
    int i;
    /* Re-upload all tracked surfaces from source metadata on device change */
    for (i = 0; i < FE_MAX_SURFACES; i++) {
        int sz = 0;
        void *data, *pixels = NULL;
        int w = 0, h = 0;
        if (!s_surfaces[i].in_use || !s_surfaces[i].source_name[0]) continue;
        data = td5_asset_open_and_read(s_surfaces[i].source_name,
                                        s_surfaces[i].source_archive, &sz);
        if (!data || sz <= 0) continue;
        if (!td5_asset_decode_tga(data, (size_t)sz, &pixels, &w, &h) || !pixels) {
            free(data); continue;
        }
        free(data);
        s_surfaces[i].width = w;
        s_surfaces[i].height = h;
        td5_plat_render_upload_texture(s_surfaces[i].tex_page, pixels, w, h, 2);
        free(pixels);
    }
}
static void frontend_post_quit(void) {
    g_td5.quit_requested = 1;
}

/* Placeholder: write cup data save file */
static int frontend_write_cup_data(void) {
    TD5_LOG_I(LOG_TAG, "WriteCupData");
    return 1;
}

/* Placeholder: load continue cup data; returns 1 on success */
static int frontend_load_continue_cup_data(void) {
    TD5_LOG_I(LOG_TAG, "LoadContinueCupData");
    return 1;
}

/* Placeholder: validate CupData.td5 checksum; returns 1 if valid */
static int frontend_validate_cup_checksum(void) {
    return 0; /* no save by default */
}

/* Placeholder: delete cup data file */
static void frontend_delete_cup_data(void) {
    td5_plat_file_delete("CupData.td5");
}

/* Placeholder: network send message */
static void frontend_net_send(int type, const void *data, int size) {
    (void)type; (void)data; (void)size;
}

/* Placeholder: network receive; returns msg type or -1 */
static int frontend_net_receive(void *buf, int max_size) {
    (void)buf; (void)max_size;
    return -1;
}

/* Placeholder: network destroy session */
static void frontend_net_destroy(void) {
    s_network_active = 0;
}

/* Placeholder: network seal session */
static void frontend_net_seal(int sealed) {
    (void)sealed;
}

/* Placeholder: enumerate network providers */
static int frontend_net_enumerate(void) { return 0; }

/* Placeholder: check if local player is host */
static int frontend_net_is_host(void) { return 0; }

/* Placeholder: get local player slot index (0-5) */
static int frontend_net_local_slot(void) { return 0; }

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

    switch (s_selected_game_type) {
    case 0: /* Single Race -- user preferences apply */
        g_td5.circuit_lap_count = (s_game_option_laps + 1) * 2;
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

    case 7: /* Time Trials */
        g_td5.time_trial_enabled = 1;
        g_td5.difficulty = TD5_DIFFICULTY_HARD;
        g_td5.traffic_enabled = 0;
        g_td5.special_encounter_enabled = 0;
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

    frontend_play_sfx(5);
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
                s_inner_state = 4;
            }
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
                                        SHARED_PAGE_FONT, &font_w, &font_h)) {
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

    /* ---- ButtonBits.tga (gradient source for button backgrounds) ----
     * 56x100 paletted TGA (DAT_00496268 in original).
     * Layout: 3 sections of 32px each (style * 0x20 offset):
     *   Section 0 (rows 0-31):  Normal style (gold/warm gradient)
     *   Section 1 (rows 32-63): Highlighted style (blue gradient)
     *   Section 2 (rows 64-95): Preview/disabled style
     * Black colorkey (no red pixels in this texture). */
    if (s_buttonbits_tex_page < 0) {
        s_buttonbits_tex_page = SHARED_PAGE_BUTTONBITS;
        {
            int sz = 0;
            void *raw = td5_asset_open_and_read("ButtonBits.tga",
                                                "Front End/frontend.zip", &sz);
            if (raw && sz > 0) {
                void *pixels = NULL;
                int bw = 0, bh = 0;
                if (td5_asset_decode_tga(raw, (size_t)sz, &pixels, &bw, &bh) && pixels) {
                    /* Black colorkey: near-black pixels become transparent */
                    uint8_t *p = (uint8_t *)pixels;
                    for (int ci = 0; ci < bw * bh; ci++, p += 4) {
                        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
                            p[3] = 0;
                    }
                    if (td5_plat_render_upload_texture(s_buttonbits_tex_page,
                                                       pixels, bw, bh, 2)) {
                        s_buttonbits_w = bw;
                        s_buttonbits_h = bh;
                        TD5_LOG_I(LOG_TAG, "ButtonBits.tga loaded: page=%d %dx%d",
                                  s_buttonbits_tex_page, bw, bh);
                    } else {
                        s_buttonbits_tex_page = -1;
                    }
                    free(pixels);
                } else {
                    s_buttonbits_tex_page = -1;
                }
                free(raw);
            } else {
                TD5_LOG_W(LOG_TAG, "Failed to load ButtonBits.tga");
                s_buttonbits_tex_page = -1;
            }
        }
    }

    /* ---- ArrowButtonz.tga (left/right scroll arrows on selector buttons) ----
     * 12x36 sprite sheet (DAT_00496284 in original, FUN_00426260).
     * Four 12x9 rows:
     *   Row 0 (y=0-8):   Left  arrow, unselected (blue)
     *   Row 1 (y=9-17):  Right arrow, unselected (blue)
     *   Row 2 (y=18-26): Left  arrow, selected (gold)
     *   Row 3 (y=27-35): Right arrow, selected (gold)
     * Red colorkey (background is pure red, not black). */
    if (s_arrowbuttonz_tex_page < 0) {
        s_arrowbuttonz_tex_page = SHARED_PAGE_ARROWBTNZ;
        {
            int sz = 0;
            void *raw = td5_asset_open_and_read("ArrowButtonz.tga",
                                                "Front End/frontend.zip", &sz);
            if (raw && sz > 0) {
                void *pixels = NULL;
                int aw = 0, ah = 0;
                if (td5_asset_decode_tga(raw, (size_t)sz, &pixels, &aw, &ah) && pixels) {
                    uint8_t *p = (uint8_t *)pixels;
                    for (int ci = 0; ci < aw * ah; ci++, p += 4) {
                        /* Red colorkey: pure red background. BGRA order: p[2]=R, p[1]=G, p[0]=B */
                        if (p[2] > 200 && p[1] < 30 && p[0] < 30)
                            p[3] = 0;
                    }
                    if (td5_plat_render_upload_texture(s_arrowbuttonz_tex_page,
                                                       pixels, aw, ah, 2)) {
                        TD5_LOG_I(LOG_TAG, "ArrowButtonz.tga loaded: page=%d %dx%d",
                                  s_arrowbuttonz_tex_page, aw, ah);
                    } else {
                        s_arrowbuttonz_tex_page = -1;
                    }
                    free(pixels);
                } else {
                    s_arrowbuttonz_tex_page = -1;
                }
                free(raw);
            } else {
                TD5_LOG_W(LOG_TAG, "Failed to load ArrowButtonz.tga");
                s_arrowbuttonz_tex_page = -1;
            }
        }
    }

    /* ---- ButtonLights.tga (selection indicator dot) ----
     * 16x32 paletted TGA (DAT_00496284 in original).
     * Two 16x16 frames stacked vertically:
     *   Top half  (V 0.0-0.5): dim/off state
     *   Bottom half (V 0.5-1.0): bright/on state
     * Black colorkey. */
    if (s_buttonlights_tex_page < 0) {
        s_buttonlights_tex_page = SHARED_PAGE_BTNLIGHTS;
        {
            int sz = 0;
            void *raw = td5_asset_open_and_read("ButtonLights.tga",
                                                "Front End/frontend.zip", &sz);
            if (raw && sz > 0) {
                void *pixels = NULL;
                int lw = 0, lh = 0;
                if (td5_asset_decode_tga(raw, (size_t)sz, &pixels, &lw, &lh) && pixels) {
                    uint8_t *p = (uint8_t *)pixels;
                    for (int ci = 0; ci < lw * lh; ci++, p += 4) {
                        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
                            p[3] = 0;
                    }
                    if (td5_plat_render_upload_texture(s_buttonlights_tex_page,
                                                       pixels, lw, lh, 2)) {
                        s_buttonlights_w = lw;
                        s_buttonlights_h = lh;
                        TD5_LOG_I(LOG_TAG, "ButtonLights.tga loaded: page=%d %dx%d",
                                  s_buttonlights_tex_page, lw, lh);
                    } else {
                        s_buttonlights_tex_page = -1;
                    }
                    free(pixels);
                } else {
                    s_buttonlights_tex_page = -1;
                }
                free(raw);
            } else {
                TD5_LOG_W(LOG_TAG, "ButtonLights.tga not found (optional)");
                s_buttonlights_tex_page = -1;
            }
        }
    }

    /* ---- SnkMouse.TGA (cursor) ---- */
    if (s_cursor_tex_page < 0) {
        s_cursor_tex_page = SHARED_PAGE_CURSOR;
        if (!frontend_load_tga_colorkey("snkmouse.tga", "Front End/frontend.zip",
                                         s_cursor_tex_page, &s_cursor_w, &s_cursor_h)) {
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
    memset(s_car_lock_table, 0, sizeof(s_car_lock_table));
    for (int _li = 21; _li < 37; _li++) s_car_lock_table[_li] = 1;
    s_total_unlocked_cars = 23; /* 23 visible in selector (21 unlocked + 2 locked-visible) */

    /* Track lock table: DAT_004668b0 (original binary).
     * Selector shows 16 tracks in regular mode (DAT_00466840 = 16).
     * Slots 0-7: unlocked (Moscow, Edinburgh, Sydney, Blue Ridge, Jarash,
     *                       Newcastle, Maui, Courmayeur).
     * Slots 8-15: visible but locked.
     * Slots 16+: championship-only (not shown in regular selector). */
    memset(s_track_lock_table, 0, sizeof(s_track_lock_table));
    for (int _ti = 8; _ti < 26; _ti++) s_track_lock_table[_ti] = 1;
    s_total_unlocked_tracks = 16; /* 16 visible tracks (8 unlocked + 8 locked-visible) */

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

    /* Random display position, clamped to keep the image fully on screen (640x480) */
    int iw = s_bg_gallery[next].width;
    int ih = s_bg_gallery[next].height;
    int range_x = 640 - iw;
    int range_y = 480 - ih;
    s_bg_gal_x = (float)((range_x > 0 ? rand() % range_x : 0));
    s_bg_gal_y = (float)((range_y > 0 ? rand() % range_y : 0));
    s_bg_gal_blend = 0x100;
}

static void frontend_load_bg_gallery(void) {
    static const char * const names[5] = {
        "pic1.tga", "pic2.tga", "pic3.tga", "pic4.tga", "pic5.tga"
    };
    if (s_bg_gal_loaded) return;
    for (int i = 0; i < 5; i++) {
        int page = SHARED_PAGE_BG_GALLERY + i;
        int sz = 0;
        void *data = td5_asset_open_and_read(names[i], "Front End/Extras/Extras.zip", &sz);
        if (!data || sz <= 0) {
            TD5_LOG_W(LOG_TAG, "BgGallery: failed to load %s", names[i]);
            continue;
        }
        void *pixels = NULL; int w = 0, h = 0;
        if (!td5_asset_decode_tga(data, (size_t)sz, &pixels, &w, &h) || !pixels) {
            free(data); continue;
        }
        free(data);
        /* Apply black colorkey: pure-black pixels → fully transparent
         * (mirrors original CrossFade16BitSurfaces 0x0000 colorkey mask) */
        uint8_t *px = (uint8_t *)pixels;
        for (int j = 0; j < w * h; j++) {
            if (px[j*4+0] == 0 && px[j*4+1] == 0 && px[j*4+2] == 0)
                px[j*4+3] = 0;
        }
        if (td5_plat_render_upload_texture(page, pixels, w, h, 2)) {
            s_bg_gallery[i].width  = w;
            s_bg_gallery[i].height = h;
            TD5_LOG_I(LOG_TAG, "BgGallery[%d]: %s %dx%d page=%d", i, names[i], w, h, page);
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

    if (!frontend_get_button_anim_state(&mode, &tick, &max_tick)) return base_y * sy;

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
    fe_draw_option_arrows(0, sx, sy);
    frontend_draw_value_text(sx, sy, 140, 106, car_name, 0xFFFFFFFF);
    fe_draw_option_arrows(1, sx, sy);
    frontend_draw_value_text(sx, sy, 140, 226, track_name, 0xFFFFFFFF);
    if (car_locked) frontend_draw_value_text(sx, sy, 398, 126, "LOCKED", 0xFFFF4444);
    if (track_locked) frontend_draw_value_text(sx, sy, 398, 246, "LOCKED", 0xFFFF4444);
}

static void fe_draw_option_arrows(int btn_idx, float sx, float sy) {
    /* ArrowButtonz.tga: 12x36 sprite sheet (FUN_00426260, original DAT_00496284).
     * Four 12x9 rows (u spans full width = 0.0..1.0):
     *   Row 0 v=0.00..0.25  Left  arrow, unselected (blue)
     *   Row 1 v=0.25..0.50  Right arrow, unselected (blue)
     *   Row 2 v=0.50..0.75  Left  arrow, selected   (gold)
     *   Row 3 v=0.75..1.00  Right arrow, selected   (gold) */
    float bx, by, bw, bh;
    float aw = 12.0f * sx, ah = 9.0f * sy;
    float v_left, v_right;
    if (!s_buttons[btn_idx].active || s_arrowbuttonz_tex_page < 0) return;
    frontend_get_button_render_rect(btn_idx, sx, sy, &bx, &by, &bw, &bh);
    if (s_buttons[btn_idx].highlight_ramp > 0) {
        v_left  = 0.50f; v_right = 0.75f;  /* selected (gold) */
    } else {
        v_left  = 0.00f; v_right = 0.25f;  /* unselected (blue) */
    }
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(bx + 4.0f * sx,           by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, v_left,  1.0f, v_left  + 0.25f);
    fe_draw_quad(bx + bw - 4.0f*sx - aw,  by + (bh - ah) * 0.5f, aw, ah, 0xFFFFFFFF,
                 s_arrowbuttonz_tex_page, 0.0f, v_right, 1.0f, v_right + 0.25f);
}

static void frontend_render_game_options_overlay(float sx, float sy) {
    const char *on_off[] = { "OFF", "ON" };
    const char *difficulty[] = { "EASY", "MEDIUM", "HARD" };
    const char *dynamics[] = { "SIMULATION", "ARCADE" };
    char laps[16];
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    snprintf(laps, sizeof(laps), "%d", (s_game_option_laps + 1) * 2);
    frontend_draw_value_text(sx, sy, 344, s_buttons[0].y + 6, laps, 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[1].y + 6, on_off[s_game_option_checkpoint_timers & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[2].y + 6, on_off[s_game_option_traffic & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[3].y + 6, on_off[s_game_option_cops & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[4].y + 6, difficulty[s_game_option_difficulty % 3], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[5].y + 6, dynamics[s_game_option_dynamics & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[6].y + 6, on_off[s_game_option_collisions & 1], 0xFFFFFFFF);
    for (int i = 0; i <= 6; i++) fe_draw_option_arrows(i, sx, sy);
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
    frontend_draw_value_text(sx, sy, 344, s_buttons[0].y + 6, mode_name, 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[1].y + 6, on_off[s_display_fog_enabled & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[2].y + 6, speed_read[s_display_speed_units & 1], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[3].y + 6, damping, 0xFFFFFFFF);
    for (int i = 0; i <= 3; i++) fe_draw_option_arrows(i, sx, sy);
}

static void frontend_render_sound_options_overlay(float sx, float sy) {
    if (!s_buttons[0].active) return;
    if (!s_anim_complete) return;
    /* SFX Mode is indicated by the Stereo/Mono icon; no extra text needed.
     * Volume levels are indicated by bar fill only; no numbers. */
    for (int i = 0; i <= 2; i++) fe_draw_option_arrows(i, sx, sy);

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
     * Each bar sits to the right of its button at x=344, vertically
     * centred in the button height (32px). Bar=12px tall, fill=10px. */
    {
        int bar_btns[2]  = { 1, 2 }; /* SFX Volume, Music Volume */
        int vols[2]      = { s_sound_option_sfx_volume, s_sound_option_music_volume };

        for (int vi = 0; vi < 2; vi++) {
            int   btn    = bar_btns[vi];
            float bar_x  = 344.0f * sx;
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
    frontend_draw_value_text(sx, sy, 344, s_buttons[0].y + 6, on_off[(s_two_player_mode & 4) ? 1 : 0], 0xFFFFFFFF);
    frontend_draw_value_text(sx, sy, 344, s_buttons[1].y + 6, on_off[(s_two_player_mode & 8) ? 1 : 0], 0xFFFFFFFF);
    for (int i = 0; i <= 1; i++) fe_draw_option_arrows(i, sx, sy);

    /* SplitScreen.tga: sprite sheet, 64x32 per split-mode icon, rows stacked vertically.
     * Drawn at x=394, y=97 (same formula as Controllers.tga in Control Options):
     *   x = uVar2+0x4a = 394,  y = uVar4-0x8f = 97  (FUN_00420C70 case4/5 steady-state)
     * Row = split_screen_mode index (0=off, 1=on), src_y = mode*32. */
    if (s_split_screen_surface > 0) {
        int slot = s_split_screen_surface - 1;
        if (slot >= 0 && slot < FE_MAX_SURFACES && s_surfaces[slot].in_use) {
            int sh = s_surfaces[slot].height;
            if (sh > 0) {
                int   mode = (s_two_player_mode & 4) ? 1 : 0;
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
    /* 14 rows: SNK_Config_Hdrs + config.nfo fields 2-16.
     * Rendered in the car preview area (x=232, y=124, 408x280).
     * Headers dim gray on left, values white on right. */
    static const struct { const char *hdr; int fi; int exp; } k_rows[] = {
        /* exp: 0=raw value, 1=layout type, 2=engine type, 3=tire pair (fi + fi+1) */
        { "LAYOUT:",     2,  1 },
        { "GEARS:",      3,  0 },
        { "PRICE:",      4,  0 },
        { "TIRES:",      5,  3 },
        { "TOP SPEED:",  7,  0 },
        { "0-60 MPH:",   8,  0 },
        { "60-0 MPH:",   9,  0 },
        { "1/4 MILE:",  10,  0 },
        { "ENGINE:",    11,  2 },
        { "COMPRESS:",  12,  0 },
        { "DISPLAC:",   13,  0 },
        { "LAT. ACC:",  14,  0 },
        { "TORQUE:",    15,  0 },
        { "HP:",        16,  0 },
    };
    int n_layout = (int)(sizeof(k_stat_layout_types)/sizeof(k_stat_layout_types[0]));
    int n_engine = (int)(sizeof(k_stat_engine_types)/sizeof(k_stat_engine_types[0]));
    float tsc = 0.5f;
    float hx = 234.0f * sx;   /* header column x */
    float vx = 430.0f * sx;   /* value column x */
    float y0 = 130.0f * sy;   /* first row y */
    float dy = 12.0f * sy;    /* row spacing — matches 12px font cell height */
    char val[64];
    int i;

    for (i = 0; i < 14; i++) {
        float y = y0 + (float)i * dy;
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

    /* Overlay UI elements: animated during state 2, static otherwise.
     * State 2 formula (0x40DFC0): bar+curve slide from right (636→36, 8px/frame @30fps);
     * topbar slides from left (-532→0, 8px/frame @30fps); 75 frames total = ~2500ms.
     * Drawn OPAQUE (no color key): original used BltFast without SRCCOLORKEY (flag 0x10).
     * Black pixels in these TGAs are part of the design and render as solid black. */
    if (s_inner_state == 2) {
        float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 2500.0f);
        float bar_x    = 636.0f - (636.0f - 36.0f) * t;   /* right→left: 636 → 36 */
        float topbar_x = -532.0f + 532.0f * t;             /* left→right: -532 → 0 */
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_opaque(s_carsel_topbar_surface, topbar_x * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,   bar_x * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_opaque(s_carsel_curve_surface, bar_x * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
    } else {
        /* CarSelTopBar (532x36): x=0, y=45; CarSelBar1 (24x408): x=36, y=0;
         * CarSelCurve  (80x56): x=36, y=408 */
        if (s_carsel_topbar_surface > 0)
            fe_draw_surface_opaque(s_carsel_topbar_surface,  0.0f * sx,  45.0f * sy, 532.0f * sx,  36.0f * sy, 0xFFFFFFFF);
        if (s_carsel_bar_surface > 0)
            fe_draw_surface_opaque(s_carsel_bar_surface,    36.0f * sx,   0.0f * sy,  24.0f * sx, 408.0f * sy, 0xFFFFFFFF);
        if (s_carsel_curve_surface > 0)
            fe_draw_surface_opaque(s_carsel_curve_surface,  36.0f * sx, 408.0f * sy,  80.0f * sx,  56.0f * sy, 0xFFFFFFFF);
    }

    /* Car preview area: no opaque backing — original preserved the primary surface (MainMenu.tga
     * car art) which shows through here; CarPic TGAs are fully opaque and cover the area. */
    if (s_inner_state == 15) {
        /* Stats sub-screen: draw car, then semi-transparent dark quad, then spec text */
        if (s_car_preview_surface > 0)
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        fe_draw_quad(232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xB0101020, -1, 0, 0, 1, 1);
        frontend_render_car_stats_overlay(sx, sy);
    } else {
        if (s_inner_state == 11 && s_car_preview_prev_surface > 0) {
            /* Old car slides out to the right (state 11, ~433ms) — animPhase 0x0B: offset = counter*0x20 */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 433.0f);
            float x = 232.0f + 408.0f * t;  /* 232 → 640 (off-screen right) */
            fe_draw_surface_rect(s_car_preview_prev_surface, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        } else if (s_inner_state == 14 && s_car_preview_surface > 0) {
            /* New car slides in from right (state 14, ~833ms @30fps):
             * formula: offset = counter*-0x40 + 0x4A8 (1192px beyond final pos=232)
             * arrives at x=232 in ~620ms (18.6 frames @30fps), then held */
            float t = frontend_clamp01((float)(td5_plat_time_ms() - s_anim_start_ms) * 2.0f / 620.0f);
            float x = 1424.0f - 1192.0f * t;  /* 1424 → 232 */
            fe_draw_surface_rect(s_car_preview_surface, x * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        } else if (s_inner_state != 12 && s_inner_state != 13 && s_car_preview_surface > 0) {
            /* Static: states 12/13 are pass-through transition ticks — skip to avoid 1-frame flash */
            fe_draw_surface_rect(s_car_preview_surface, 232.0f * sx, 124.0f * sy, 408.0f * sx, 280.0f * sy, 0xFFFFFFFF);
        }
    }

    if (s_anim_complete) {
        frontend_draw_value_text(sx, sy, 232, 106, frontend_get_car_display_name(actual_car), 0xFFFFFFFF);
        if (!s_cheat_unlock_all && !s_network_active &&
            actual_car >= 0 && actual_car < 37 &&
            s_car_lock_table[actual_car] != 0) {
            frontend_draw_value_text(sx, sy, 86, 121, "LOCKED", 0xFFFF4444);
        }
    }
}

static void frontend_render_track_selection_preview(float sx, float sy) {
    char track_name[80];
    if (!s_anim_complete) return;
    frontend_get_track_display_name(s_selected_track, 1, track_name, sizeof(track_name));
    /* Track name above the preview */
    frontend_draw_value_text(sx, sy, 412, 113, track_name, 0xFFFFFFFF);
    /* Track preview: 152x224 portrait, right of buttons.
     * x=EDI+0x12E=412, y=ESI+0x36=135 (640x480) */
    if (s_track_preview_surface > 0) {
        fe_draw_surface_rect(s_track_preview_surface, 412.0f * sx, 135.0f * sy, 152.0f * sx, 224.0f * sy, 0xFFFFFFFF);
    }
    /* Direction is shown by the Forwards button — no duplicate text here */
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
     * Row = controller_type * 32; type 0 = keyboard (only type currently supported). */
    int sh = s_surfaces[slot].height;
    if (sh <= 0) return;

    float icon_w = 64.0f * sx;
    float icon_h = 32.0f * sy;
    float v_row  = 32.0f / (float)sh;
    float v0 = 0.0f, v1 = v_row;   /* type 0 (keyboard) */

    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    fe_draw_quad(394.0f * sx,  97.0f * sy, icon_w, icon_h,
                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, v0, 1.0f, v1);
    fe_draw_quad(394.0f * sx, 217.0f * sy, icon_w, icon_h,
                 0xFFFFFFFF, s_surfaces[slot].tex_page, 0.0f, v0, 1.0f, v1);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
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

static void frontend_render_extras_gallery_overlay(float sx, float sy) {
    if (s_gallery_pic_surface > 0) {
        fe_draw_surface_rect(s_gallery_pic_surface, 0.0f, 0.0f, 640.0f * sx, 480.0f * sy, 0xFFFFFFFF);
    }
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

    /* Bottom: src (28, 96)-(32, 100) — fixed, no state offset (Ghidra 0x425d37) */
    float be_u0 = 28.0f / BB_TEX_W;
    float be_u1 = 32.0f / BB_TEX_W;
    for (float x = bx + lw; x + rw < bx + bw; x += tw)
        fe_draw_quad(x, by + bh - th, tw, th, 0xFFFFFFFF, tex, be_u0, te_v0, be_u1, te_v1);

    /* --- Vertical edge tiles (full-column width, 4px tall) ---
     * Original tiles all the way to H (while loop_y < H), relying on
     * corners drawn afterwards to cover the overlap areas. */
    /* Left: src (0, yb)-(26, yb+4) */
    float le_v0 = yb / BB_TEX_H;
    float le_v1 = (yb + 4.0f) / BB_TEX_H;
    for (float y = by + 13.0f * sy; y < by + bh; y += th)
        fe_draw_quad(bx, y, lw, th, 0xFFFFFFFF, tex,
                     0.0f, le_v0, (float)BB_LW / BB_TEX_W, le_v1);

    /* Right: src (28, yb)-(56, yb+4) */
    float re_v0 = yb / BB_TEX_H;
    float re_v1 = (yb + 4.0f) / BB_TEX_H;
    for (float y = by + 9.0f * sy; y < by + bh; y += th)
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
    case TD5_SCREEN_HIGH_SCORE:
        frontend_render_high_score_overlay(sx, sy);
        break;
    case TD5_SCREEN_EXTRAS_GALLERY:
        frontend_render_extras_gallery_overlay(sx, sy);
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
            if (bb_state == 0) {
                fe_draw_quad(bx, by, bw, bh, 0xFF392152u, -1, 0.0f, 0.0f, 1.0f, 1.0f);
            }
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
         * 2px outline, mouse-hover only. Inset 20/22/4/6 px from edges.
         * Color 0xC000 → pure green ≈ (0,128,0). */
        if (i == s_selected_button && s_selection_from_mouse &&
            !s_buttons[i].disabled && ramp_t > 0.01f) {
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
            float title_x = ((640.0f - (float)title_w) * 0.5f) * sx;
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
    s_selected_car = 0;   /* Dodge Viper (vip.zip) - index 0 in UI order */
    s_selected_paint = 0;
    s_selected_config = 0;
    s_selected_transmission = 0;
    s_selected_track = 0; /* Los Angeles, CA (level001.zip -- always present) */
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
    switch (s_inner_state) {
    case 0:
        frontend_init_return_screen(TD5_SCREEN_LOCALIZATION_INIT);
        TD5_LOG_I(LOG_TAG, "ScreenLocalizationInit: loading Language.dll, config.td5");

        /* Load LANGUAGE.DLL string table */
        /* Load car ZIP path table from gCarZipPathTable */
        /* Load config.td5 settings */
        /* Enumerate display modes */
        /* Seed controller/input state from hardware detection */

        td5_frontend_init_resources();

        /* Route: if resume-cup flag is set, go to race results */
        /* (DAT_004a2c8c == 2) */
        /* Otherwise, go to main menu */
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
        /* Draw "TEST_DRIVE_5_COPYRIGHT_1998" */
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
                s_flow_context = 2;
                s_return_screen = TD5_SCREEN_QUICK_RACE;
                s_inner_state = 8;
                break;

            case 2: /* Two Player */
                s_flow_context = 3;
                s_two_player_mode = 1;
                s_selected_game_type = 0;
                s_return_screen = TD5_SCREEN_CAR_SELECTION;
                s_inner_state = 8;
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

            case 6: /* Exit -> credits slideshow */
                s_flow_context = 10;
                s_return_screen = TD5_SCREEN_EXTRAS_GALLERY;
                s_inner_state = 8;
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
        (void)yes_idx; (void)no_idx;
        s_inner_state = 6;
        break;
    }

    case 6: /* Exit confirm: wait for Yes/No */
        if (s_input_ready && s_button_index >= 0) {
            /* Check by label since button indices depend on pool state */
            if (s_button_index < FE_MAX_BUTTONS &&
                strcmp(s_buttons[s_button_index].label, "Yes") == 0) {
                s_inner_state = 7;
            } else if (s_button_index < FE_MAX_BUTTONS &&
                       strcmp(s_buttons[s_button_index].label, "No") == 0) {
                /* Remove Yes/No buttons, return to main interaction */
                s_inner_state = 4;
            }
        }
        break;

    case 7: /* Confirm exit -- post quit message */
        frontend_post_quit();
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
        frontend_load_tga("Front_End/RaceMenu.tga", "Front_End/FrontEnd.zip");
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
        frontend_create_button("Circuit Laps",      -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Checkpoint Timers", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Traffic",           -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Cops",              -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Difficulty",        -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Dynamics",          -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("3D Collisions",     -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK",                -0x60, 0, 0x60,  0x20);
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
        frontend_create_preview_button("Player 1",  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Configure",         -0xE0, 0, 0xE0, 0x20);
        frontend_create_preview_button("Player 2",  -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Configure",         -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK",                -0x60, 0, 0x60, 0x20);
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
        frontend_create_button("SFX Mode",     -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("SFX Volume",   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Music Volume", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Music Test",   -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK",           -0xE0, 0, 0xE0, 0x20);
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
        frontend_create_button("Resolution",    -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Fogging",       -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Speed Readout", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Camera Damping",-0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK",            -0x60, 0, 0x60,  0x20);
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
        frontend_init_return_screen(TD5_SCREEN_TWO_PLAYER_OPTIONS);
        TD5_LOG_D(LOG_TAG, "TwoPlayerOptions: init");
        s_split_screen_surface = frontend_load_tga("SplitScreen.tga", "Front End/frontend.zip");
        frontend_create_button("Split Screen", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("Catch-Up", -0xE0, 0, 0xE0, 0x20);
        frontend_create_button("OK", -0xE0, 0, 0xE0, 0x20);
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
            if (active_button == 0 && delta != 0) {
                s_two_player_mode ^= 4;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (active_button == 1 && delta != 0) {
                s_two_player_mode ^= 8;
                frontend_play_sfx(2);
                s_inner_state = 4;
            } else if (s_button_index == 2) {
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

        /* Load overlay UI assets */
        s_carsel_bar_surface    = frontend_load_tga("Front_End/CarSelBar1.tga",   "Front_End/FrontEnd.zip");
        s_carsel_curve_surface  = frontend_load_tga("Front_End/CarSelCurve.tga",  "Front_End/FrontEnd.zip");
        s_carsel_topbar_surface = frontend_load_tga("Front_End/CarSelTopBar.tga", "Front_End/FrontEnd.zip");
        s_graphbars_surface     = frontend_load_tga("Front_End/GraphBars.tga",    "Front_End/FrontEnd.zip");
        /* Background: original preserved the previous screen's primary surface (RaceMenu.tga
         * set by RaceTypeCategory). s_background_surface carries it forward automatically. */

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
        /* Skip if returning from network car select or 2P round 2 */
        if ((s_two_player_mode & 4) != 0 || s_network_active) {
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
                    /* Accept selection */
                    if (s_selected_game_type == 5) {
                        s_masters_roster_flags[s_selected_car] = 2; /* taken */
                    }
                    s_inner_state = 0x14; /* slide-out prep */
                }
            }
                break;

            case 5: /* Back */
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

        /* Single-player: proceed to track selection */
        td5_frontend_set_screen(TD5_SCREEN_TRACK_SELECTION);
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
            s_inner_state = 4;
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

                /* For cup modes (type > 7): skip non-playable NPC groups */
                s_inner_state = 5; /* trigger track change display */
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

    case 5: /* Track change: clear info, render name, load preview, check locked */
        /* Load track preview TGA from Front_End/Tracks/Tracks.zip */
        /* Show "Locked" if track is locked */
        frontend_load_selected_track_preview();
        s_inner_state = 4;
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
    case 0: /* Init: load first slide */
        frontend_init_return_screen(TD5_SCREEN_EXTRAS_GALLERY);
        s_gallery_pic_index = 0;
        s_gallery_visited_mask = 1;
        s_gallery_pic_surface = frontend_load_tga(s_gallery_names[0], GALLERY_ZIP);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 39 frames -- prevents Enter from menu bleeding through */
        s_anim_tick += 2;
        if (s_anim_tick >= 0x27)
            s_inner_state = 2;
        break;

    case 2: /* Interactive: L/R to navigate, Escape to exit */
        if (frontend_check_escape()) {
            if (s_flow_context == 10 || s_previous_screen == TD5_SCREEN_MAIN_MENU) {
                frontend_post_quit();
            } else {
                s_anim_tick = 0;
                s_inner_state = 3;
            }
            break;
        }
        if (s_input_ready) {
            int delta = frontend_option_delta();
            if (delta != 0) {
                int next_index = s_gallery_pic_index + delta;
                /* Scrolling right past last slide in exit flow quits (only after all visited) */
                if (delta > 0 && next_index >= GALLERY_PIC_COUNT) {
                    if ((s_gallery_visited_mask & GALLERY_ALL_VISITED) == GALLERY_ALL_VISITED &&
                        (s_flow_context == 10 || s_previous_screen == TD5_SCREEN_MAIN_MENU)) {
                        frontend_post_quit();
                        break;
                    }
                    next_index = 0;
                }
                if (next_index < 0) next_index = GALLERY_PIC_COUNT - 1;
                if (s_gallery_pic_surface > 0) {
                    frontend_release_surface(s_gallery_pic_surface);
                    s_gallery_pic_surface = 0;
                }
                s_gallery_pic_index = next_index;
                s_gallery_visited_mask |= (1 << s_gallery_pic_index);
                s_gallery_pic_surface = frontend_load_tga(s_gallery_names[s_gallery_pic_index], GALLERY_ZIP);
            }
        }
        break;

    case 3: /* Slide-out: 16 frames, then return */
        s_anim_tick += 2;
        if (s_anim_tick >= 16 && s_return_screen >= 0 && s_return_screen < TD5_SCREEN_COUNT)
            td5_frontend_set_screen((TD5_ScreenIndex)s_return_screen);
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
        TD5_LOG_D(LOG_TAG, "RaceResults: state 0 - init/sort");

        /* Sort results by game type:
         * Types 1/6: by secondary metric desc
         * Types 2-5: by primary metric asc */

        /* Save race snapshot on first entry */
        if (!s_results_rerace_flag) {
            s_snap_car = s_selected_car;
            s_snap_paint = s_selected_paint;
            s_snap_trans = s_selected_transmission;
            s_snap_config = s_selected_config;
            s_results_rerace_flag = 1;
        }

        /* Create 0x198 x 0x188 results panel */
        /* Draw column headers based on game type:
         * Normal: SNK_ResultsTxt
         * Cup: SNK_CCResultsTxt
         * Drag: SNK_DRResultsTxt */

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

    case 6: /* Interactive: L/R browse racer slots (0-5), confirm exits */
        if (s_input_ready) {
            if (s_arrow_input != 0) {
                /* Cycle through racer slots, skip inactive */
                /* Drag race: only 2 slots */
            }
            if (s_button_index == 0) { /* confirm -> exit */
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

    case 0x0C: /* Cleanup: release all surfaces */
        s_inner_state = 0x0D;
        break;

    case 0x0D: /* Post-results menu: create context-dependent buttons */
        TD5_LOG_D(LOG_TAG, "RaceResults: state 0xD - post-results menu");

        if (s_network_active) {
            /* Network: skip to lobby or main menu */
            td5_frontend_set_screen(TD5_SCREEN_NETWORK_LOBBY);
            return;
        }

        if (s_selected_game_type < 1 || s_selected_game_type == 7 || s_selected_game_type == 9) {
            /* Quick Race / Time Trial / Drag: 5 buttons */
            frontend_create_button("Race Again",      -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("View Replay",     -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("View Race Data",  -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("Select New Car",  -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("Quit",            -0xE0, 0, 0xE0, 0x20);
        } else {
            /* Cup Race (types 1-6): 5 buttons */
            int next_valid = ConfigureGameTypeFlags();
            frontend_create_button("Next Cup Race",   -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("View Replay",     -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("View Race Data",  -0xE0, 0, 0xE0, 0x20);
            frontend_create_button("Save Race Status",-0xE0, 0, 0xE0, 0x20);
            frontend_create_button("Quit",            -0xE0, 0, 0xE0, 0x20);

            if (!next_valid) {
                /* Grey out "Next Cup Race" and "Save" -- cup complete */
                s_results_cup_complete = 1;
            }

            /* Masters (type 5): special progression */
            if (s_selected_game_type == 5 && !s_results_rerace_flag &&
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
                }
                if (s_selected_game_type >= 1 && s_selected_game_type <= 6) {
                    s_race_within_series++;
                }
                frontend_init_race_schedule();
                break;

            case 1: /* View Replay */
                /* Set g_inputPlaybackActive = 1, start replay */
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
        if (frontend_write_cup_data()) {
            TD5_LOG_I(LOG_TAG, "RaceResults: cup data saved");
            /* Show "Block Saved OK" message */
        } else {
            TD5_LOG_W(LOG_TAG, "RaceResults: failed to save cup data");
            /* Show "Failed to Save" message */
        }
        frontend_create_button("OK", -100, 0, 100, 0x20);
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
        frontend_init_return_screen(TD5_SCREEN_NAME_ENTRY);
        TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: qualification check");
        /* Determine score type (time/lap/points).
         * Compare player's result against worst entry in 5-slot table.
         * If doesn't qualify, or 2P mode, or disqualified -> skip to state 4. */
        /* Create text input button */
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
        /* Scan 5-slot table, find insertion position, shift lower entries */
        /* Write: name, score, car index, speed stats */
        /* For cup races: average speed = total / race count */
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
        /* Create 0x198 x 0x70 dialog surface */
        /* Draw 4 lines: Sorry / You Failed / To Win / [race type name] */
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
        /* Create 0x198 x 0x70 dialog */
        /* Draw: SNK_SorryTxt (y=0), SNK_SeshLockedTxt (y=0x38) */
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
