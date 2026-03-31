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
#include "td5_sound.h"
#include "td5re.h"
#include "../../ddraw_wrapper/src/wrapper.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========================================================================
 * Module Tag
 * ======================================================================== */

#define LOG_TAG "frontend"

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
static int  s_track_direction;          /* DAT_004a2c98: 0=fwd, 1=bwd */
static int  s_track_max;               /* max track index for current mode */

#define FE_MAX_DISPLAY_MODES 64
static TD5_DisplayMode s_display_modes[FE_MAX_DISPLAY_MODES];
static char            s_display_mode_names[FE_MAX_DISPLAY_MODES][32];
static int             s_display_mode_count;
static int             s_display_mode_index;
static int             s_display_fog_enabled = 1;
static int             s_display_speed_units;
static int             s_display_camera_damping = 5;

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
static int  s_mouse_clicked;
static int  s_prev_mouse_btn;

/* Fade state */
static int  s_fade_active;
static int  s_fade_progress;     /* 0..255 */
static int  s_fade_direction;    /* 1 = in, -1 = out */
static int  s_fade_table_index;

/* ========================================================================
 * Frontend Rendering Infrastructure
 *
 * Surface manager, button system, input polling, draw queues.
 * ======================================================================== */

/* --- Surface Manager --- */

#define FE_MAX_SURFACES    32
#define FE_SURFACE_PAGE_BASE 900  /* texture pages 900-931 reserved for frontend */

typedef struct {
    int in_use;
    int tex_page;
    int width, height;
} FE_Surface;

static FE_Surface s_surfaces[FE_MAX_SURFACES];
static int s_white_tex_page = -1;
static int s_background_surface = 0;
static int s_font_page = -1;
static int s_cursor_tex_page = -1;  /* page 896: snkmouse.tga cursor */
static int s_cursor_w = 0, s_cursor_h = 0;
static int s_mainfont_tex_page = -1; /* page 895: mainfont.tga menu titles */
static int s_mainfont_w = 0, s_mainfont_h = 0;
static int s_buttonbits_tex_page = -1; /* page 897: ButtonBits.tga gradient */
static int s_buttonbits_w = 0, s_buttonbits_h = 0;

/* Original font: BodyText.tga, 10 chars per row, 24x24 cells (from Ghidra FUN_00424560)
 * Atlas is 240x552 (8bpp with palette, red color key for transparency).
 * DAT_0049626c. */
#define FONT_COLS 10
#define FONT_CELL 24
#define FONT_TEX_W (FONT_COLS * FONT_CELL)  /* 240 */
#define FONT_TEX_H 552  /* actual BodyText.tga height: 23 rows of 24px */

/* Dedicated texture pages for shared assets (never freed on screen change):
 * 899 = white fallback
 * 898 = BodyText.tga (font)
 * 897 = ButtonBits.tga (gradient source)
 * 896 = SnkMouse.TGA (cursor)
 * 895 = mainfont.tga (pre-rendered menu titles)
 */
#define SHARED_PAGE_WHITE     899
#define SHARED_PAGE_FONT      898
#define SHARED_PAGE_BUTTONBITS 897
#define SHARED_PAGE_CURSOR    896
#define SHARED_PAGE_MAINFONT  895
#define SHARED_PAGE_MIN       895  /* lowest shared page -- don't clear below this */

static int frontend_load_tga(const char *name, const char *archive) {
    int sz = 0;
    void *data = NULL;

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

    /* Convert RGBA (stbi/TGA decode output) to BGRA (D3D11 B8G8R8A8_UNORM) */
    {
        uint8_t *p = (uint8_t *)pixels;
        int count = w * h;
        for (int ci = 0; ci < count; ci++) {
            uint8_t r = p[0], b = p[2];
            p[0] = b; p[2] = r;
            p += 4;
        }
    }

    /* Find free surface slot */
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
        free(pixels);
        int handle = slot + 1;
        /* Large images (>=640 wide or 640x480) are backgrounds — auto-set as current bg.
         * Narrower overlays (CarSelBar1, CarSelCurve, etc.) must NOT hijack the bg. */
        if (w >= 640 || (w >= 320 && h >= 400)) s_background_surface = handle;
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

    /* RGBA -> BGRA swap + red color key (R=255,G=0,B=0 -> alpha=0) */
    {
        uint8_t *p = (uint8_t *)pixels;
        int count = w * h;
        for (int ci = 0; ci < count; ci++) {
            uint8_t r = p[0], g = p[1], b = p[2];
            /* Swap R and B for BGRA */
            p[0] = b; p[2] = r;
            /* Color key: original red (R=255,G=0,B=0) -> transparent */
            if (r == 255 && g == 0 && b == 0) {
                p[3] = 0;
            }
            p += 4;
        }
    }

    if (td5_plat_render_upload_texture(dest_page, pixels, w, h, 2)) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        TD5_LOG_I(LOG_TAG, "LoadTGA_CK OK: %s -> page=%d %dx%d", bare_name, dest_page, w, h);
        free(pixels);
        return 1;
    }

    free(pixels);
    return 0;
}

static void frontend_release_surface(int handle) {
    int slot = handle - 1;
    if (slot >= 0 && slot < FE_MAX_SURFACES) {
        /* Don't release shared asset surfaces (font, cursor, etc.) */
        if (s_surfaces[slot].tex_page >= SHARED_PAGE_MIN) return;
        s_surfaces[slot].in_use = 0;
    }
}

/* --- Button System --- */

#define FE_MAX_BUTTONS 16

typedef struct {
    int active;
    int x, y, w, h;
    int disabled;
    char label[64];
} FE_Button;

static FE_Button s_buttons[FE_MAX_BUTTONS];
static int s_button_count;
static int s_cursor_visible;

/* Original game button layout (from Ghidra decompilation of 0x415490):
 * Buttons are 224x32 (0xE0 x 0x20), positioned at x=110 from left.
 * Y positions centered around screen_height/2:
 *   btn0: y = 240 - 0x93 = 93    btn1: y = 240 - 0x6B = 133
 *   btn2: y = 240 - 0x43 = 173   btn3: y = 240 - 0x1B = 213
 *   btn4: y = 240 + 0x0D = 253   btn5: y = 240 + 0x35 = 293
 *   btn6: y = 240 + 0x5D = 333
 * Spacing = 40px between buttons. */
static const int s_auto_button_y[] = { 93, 133, 173, 213, 253, 293, 333, 373, 413 };
static int s_auto_button_idx = 0;
static int s_selected_button = 0;

static int frontend_create_button(const char *label, int x, int y, int w, int h) {
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) {
            s_buttons[i].active = 1;
            s_buttons[i].disabled = 0;
            s_buttons[i].w = (w > 0 && w != 200) ? w : 224; /* 0xE0 = 224 */
            s_buttons[i].h = (h > 0 && h != 32) ? h : 32;   /* 0x20 = 32 */

            if (x == 0 && y == 0 && s_auto_button_idx < 9) {
                s_buttons[i].x = 110; /* from Ghidra: iVar7 = 320 - 0xD2 = 110 */
                s_buttons[i].y = s_auto_button_y[s_auto_button_idx];
                s_auto_button_idx++;
            } else {
                s_buttons[i].x = x;
                s_buttons[i].y = y;
            }

            strncpy(s_buttons[i].label, label ? label : "", 63);
            s_buttons[i].label[63] = '\0';
            if (i >= s_button_count) s_button_count = i + 1;
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
    s_cursor_visible = visible;
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
    td5_plat_present(0);
}

static int frontend_advance_tick(void) {
    s_anim_tick++;
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

static void frontend_init_race_schedule(void) {
    g_td5.race_requested = 1;
    g_td5.track_index = s_selected_track;
    TD5_LOG_I(LOG_TAG, "InitializeRaceSeriesSchedule: car=%d track=%d type=%d",
              s_selected_car, s_selected_track, s_selected_game_type);
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
    const char *mode_name = "Resolution: Unavailable";
    if (s_display_mode_count > 0 &&
        s_display_mode_index >= 0 &&
        s_display_mode_index < s_display_mode_count) {
        mode_name = s_display_mode_names[s_display_mode_index];
    }

    if (s_button_count > 0) {
        strncpy(s_buttons[0].label, mode_name, sizeof(s_buttons[0].label) - 1);
        s_buttons[0].label[sizeof(s_buttons[0].label) - 1] = '\0';
    }
    if (s_button_count > 1) {
        snprintf(s_buttons[1].label, sizeof(s_buttons[1].label),
            "Fogging: %s", s_display_fog_enabled ? "On" : "Off");
    }
    if (s_button_count > 2) {
        snprintf(s_buttons[2].label, sizeof(s_buttons[2].label),
            "Speed Readout: %s", s_display_speed_units ? "KPH" : "MPH");
    }
    if (s_button_count > 3) {
        snprintf(s_buttons[3].label, sizeof(s_buttons[3].label),
            "Camera Damping: %d", s_display_camera_damping);
    }
    if (s_button_count > 4) {
        strncpy(s_buttons[4].label, "OK", sizeof(s_buttons[4].label) - 1);
        s_buttons[4].label[sizeof(s_buttons[4].label) - 1] = '\0';
    }
}

static int frontend_option_delta(void) {
    if (s_arrow_input & (1 | 4)) return -1;
    if (s_arrow_input & (2 | 8)) return 1;
    return 0;
}

static void frontend_init_display_mode_state(void) {
    int width = 0;
    int height = 0;
    int bpp = 0;

    s_display_mode_count = td5_plat_enum_display_modes(s_display_modes, FE_MAX_DISPLAY_MODES);
    for (int i = 0; i < s_display_mode_count; i++) {
        snprintf(s_display_mode_names[i], sizeof(s_display_mode_names[i]),
            "Resolution: %dx%d %dbpp",
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
        td5_plat_apply_display_mode(mode->width, mode->height, mode->bpp);
    }

}

/* --- Input Polling --- */

static void frontend_poll_input(void) {
    POINT pt;
    HWND hwnd;
    int mouse_btn;

    s_input_ready = 0;
    s_button_index = -1;
    s_arrow_input = 0;

    /* Reset attract mode idle timer on any key */
    s_attract_idle_timestamp = td5_plat_time_ms();

    /* Keyboard arrows */
    if (td5_plat_input_key_pressed(0xCB)) s_arrow_input |= 1; /* LEFT */
    if (td5_plat_input_key_pressed(0xCD)) s_arrow_input |= 2; /* RIGHT */
    if (td5_plat_input_key_pressed(0xC8)) s_arrow_input |= 4; /* UP */
    if (td5_plat_input_key_pressed(0xD0)) s_arrow_input |= 8; /* DOWN */

    /* Mouse position */
    hwnd = (HWND)(DWORD_PTR)Backend_GetDisplayWindow();
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

    /* Mouse click (left button) — detect rising edge */
    mouse_btn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    s_mouse_clicked = (mouse_btn && !s_prev_mouse_btn);
    s_prev_mouse_btn = mouse_btn;

    /* Enter key acts as click */
    if (td5_plat_input_key_pressed(0x1C)) /* DIK_RETURN */
        s_mouse_clicked = 1;

    /* Keyboard navigation: UP/DOWN arrows move selection, Enter activates */
    {
        static int s_prev_up, s_prev_down, s_prev_enter;
        int up_now   = td5_plat_input_key_pressed(0xC8);
        int down_now = td5_plat_input_key_pressed(0xD0);
        int enter_now = td5_plat_input_key_pressed(0x1C);

        if (up_now && !s_prev_up) {
            /* Move selection up */
            int attempts = FE_MAX_BUTTONS;
            do {
                s_selected_button--;
                if (s_selected_button < 0) s_selected_button = FE_MAX_BUTTONS - 1;
                attempts--;
            } while (attempts > 0 && (!s_buttons[s_selected_button].active || s_buttons[s_selected_button].disabled));
        }
        if (down_now && !s_prev_down) {
            /* Move selection down */
            int attempts = FE_MAX_BUTTONS;
            do {
                s_selected_button++;
                if (s_selected_button >= FE_MAX_BUTTONS) s_selected_button = 0;
                attempts--;
            } while (attempts > 0 && (!s_buttons[s_selected_button].active || s_buttons[s_selected_button].disabled));
        }
        if (enter_now && !s_prev_enter) {
            /* Activate selected button */
            if (s_selected_button >= 0 && s_selected_button < FE_MAX_BUTTONS &&
                s_buttons[s_selected_button].active && !s_buttons[s_selected_button].disabled) {
                s_button_index = s_selected_button;
                s_input_ready = 1;
            }
        }
        s_prev_up = up_now;
        s_prev_down = down_now;
        s_prev_enter = enter_now;
    }

    /* Hit-test buttons on mouse click */
    if (s_mouse_clicked) {
        for (int i = 0; i < FE_MAX_BUTTONS; i++) {
            if (!s_buttons[i].active || s_buttons[i].disabled) continue;
            if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
                s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
                s_button_index = i;
                s_selected_button = i;
                s_input_ready = 1;
                break;
            }
        }
    }

    /* Mouse hover updates selected button */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) continue;
        if (s_mouse_x >= s_buttons[i].x && s_mouse_x < s_buttons[i].x + s_buttons[i].w &&
            s_mouse_y >= s_buttons[i].y && s_mouse_y < s_buttons[i].y + s_buttons[i].h) {
            s_selected_button = i;
            break;
        }
    }

    /* Arrow input also counts as ready */
    if (s_arrow_input) s_input_ready = 1;
}

static int frontend_check_escape(void) {
    return td5_plat_input_key_pressed(0x01);
}

static void frontend_render_text_input(void) { }
static int frontend_text_input_confirmed(void) { return (s_text_input_state == 2); }
static void frontend_recover_surfaces(void) { }

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
    if (index < 0 || index >= TD5_SCREEN_COUNT) return;

    s_current_screen = index;
    s_inner_state = 0;
    s_anim_tick = 0;
    s_screen_entry_timestamp = td5_plat_time_ms();

    /* Reset button pool and auto-layout for new screen */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) s_buttons[i].active = 0;
    s_button_count = 0;
    s_auto_button_idx = 0;
    s_selected_button = 0;
    s_background_surface = 0;
    /* Release recyclable surfaces, but KEEP shared assets on dedicated pages.
     * Shared pages (895-899) hold font, cursor, ButtonBits, mainfont --
     * these are loaded once in init and must survive screen transitions. */
    for (int i = 0; i < FE_MAX_SURFACES; i++) {
        if (s_surfaces[i].in_use && s_surfaces[i].tex_page >= SHARED_PAGE_MIN) continue;
        s_surfaces[i].in_use = 0;
    }

    /* Reset attract-mode idle counter on any navigation */
    s_attract_idle_counter = 0;
    s_attract_idle_timestamp = td5_plat_time_ms();

    g_td5.frontend_screen_index = (int)index;
    g_td5.frontend_inner_state = 0;
    g_td5.frontend_frame_counter = 0;

    TD5_LOG_D(LOG_TAG, "SetFrontendScreen(%d)", (int)index);
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

    /* 4. Cursor overlay */
    frontend_render_cursor();

    /* 5. Render flush (sprite blits, UI rects) */
    td5_frontend_render_ui_rects();
    td5_frontend_flush_sprite_blits();

    /* 6. Presentation (flip / software blit) */
    td5_plat_present(1);

    /* 7. Escape key handling -- return to main menu from most screens */
    if (frontend_check_escape()) {
        if (s_current_screen != TD5_SCREEN_MAIN_MENU &&
            s_current_screen != TD5_SCREEN_STARTUP_INIT &&
            s_current_screen != TD5_SCREEN_LOCALIZATION_INIT) {
            td5_frontend_set_screen(TD5_SCREEN_MAIN_MENU);
        }
    }

    /* 8. Cheat code detection (placeholder -- original scans key buffer) */

    /* 9. Attract mode timeout: 50 seconds of idle -> demo screen */
    if (s_current_screen == TD5_SCREEN_MAIN_MENU) {
        uint32_t now = td5_plat_time_ms();
        if ((now - s_attract_idle_timestamp) >= 50000) {
            /* Pick a random track for the demo */
            s_selected_track = rand() % 8;
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

    /* Create 1x1 white fallback texture for untextured draws */
    if (s_white_tex_page < 0) {
        s_white_tex_page = SHARED_PAGE_WHITE;
        uint32_t white = 0xFFFFFFFF;
        td5_plat_render_upload_texture(s_white_tex_page, &white, 1, 1, 2);
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
                td5_plat_render_upload_texture(s_font_page, pixels, fw, fh, 2);
                TD5_LOG_I(LOG_TAG, "GDI font atlas: page=%d %dx%d", s_font_page, fw, fh);
                free(pixels);
            }
        }
    }

    /* ---- ButtonBits.tga (gradient source for button backgrounds) ----
     * 56x100, DAT_00496268 in original. Red color key. */
    if (s_buttonbits_tex_page < 0) {
        s_buttonbits_tex_page = SHARED_PAGE_BUTTONBITS;
        if (!frontend_load_tga_colorkey("ButtonBits.tga", "Front End/frontend.zip",
                                         s_buttonbits_tex_page,
                                         &s_buttonbits_w, &s_buttonbits_h)) {
            TD5_LOG_W(LOG_TAG, "Failed to load ButtonBits.tga");
            s_buttonbits_tex_page = -1;
        }
    }

    /* ---- SnkMouse.TGA (cursor) ---- */
    if (s_cursor_tex_page < 0) {
        s_cursor_tex_page = SHARED_PAGE_CURSOR;
        if (!frontend_load_tga_colorkey("snkmouse.tga", "Front End/frontend.zip",
                                         s_cursor_tex_page, &s_cursor_w, &s_cursor_h)) {
            TD5_LOG_W(LOG_TAG, "Failed to load snkmouse.tga cursor texture");
            s_cursor_tex_page = -1;
        }
    }

    /* ---- mainfont.tga (pre-rendered menu title strings) ----
     * 252x1152, contains 36px-tall blocks of pre-rendered title text.
     * NOT a character font -- each block is a whole string (e.g. "MAIN MENU"). */
    if (s_mainfont_tex_page < 0) {
        s_mainfont_tex_page = SHARED_PAGE_MAINFONT;
        if (!frontend_load_tga_colorkey("mainfont.tga", "Front End/frontend.zip",
                                         s_mainfont_tex_page,
                                         &s_mainfont_w, &s_mainfont_h)) {
            TD5_LOG_W(LOG_TAG, "Failed to load mainfont.tga menu title texture");
            s_mainfont_tex_page = -1;
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

    /* Initialize lock tables with defaults:
     * Cars 0-15 unlocked, 16-35 locked, 36 unlocked */
    memset(s_car_lock_table, 0, sizeof(s_car_lock_table));
    {
        int i;
        for (i = 16; i <= 35; i++) s_car_lock_table[i] = 1;
    }
    s_total_unlocked_cars = 23;

    /* Tracks 0-7 unlocked, 8-17 locked, 18+ unlocked */
    memset(s_track_lock_table, 0, sizeof(s_track_lock_table));
    {
        int i;
        for (i = 8; i <= 17; i++) s_track_lock_table[i] = 1;
    }
    s_total_unlocked_tracks = 16;

    return 1;
}

void td5_frontend_release_resources(void) {
    TD5_LOG_I(LOG_TAG, "ReleaseFrontendResources");
    /* Release all loaded surfaces, fonts, work buffers */
    for (int i = 0; i < FE_MAX_SURFACES; i++) s_surfaces[i].in_use = 0;
    s_font_page = -1;
    s_cursor_tex_page = -1;
    s_cursor_w = 0; s_cursor_h = 0;
    s_mainfont_tex_page = -1;
    s_mainfont_w = 0; s_mainfont_h = 0;
    s_buttonbits_tex_page = -1;
    s_buttonbits_w = 0; s_buttonbits_h = 0;
    s_white_tex_page = -1;
    s_background_surface = 0;
}

/* ========================================================================
 * Render helpers (stubs)
 * ======================================================================== */

static void fe_draw_quad(float x, float y, float w, float h,
                         uint32_t color, int tex_page,
                         float u0, float v0, float u1, float v1); /* forward decl */

static void fe_draw_text(float x, float y, const char *text, uint32_t color, float sx, float sy) {
    /* Font atlas: BodyText.tga (or GDI fallback), 10 chars/row, 24x24 cells.
     * Layout: col = (ascii - 0x20) % 10, row = (ascii - 0x20) / 10.
     * Source rect: (col*24, row*24, 24, 24). From Ghidra FUN_00424560. */
    if (s_font_page < 0 || !text) return;
    float cx = x;
    float cell_w = (float)FONT_CELL * sx;
    float cell_h = (float)FONT_CELL * sy;
    /* Original uses per-char widths from PTR_DAT_004660c8. For now,
     * approximate with a fixed advance of ~14px (most chars are 10-18px). */
    float char_advance = 14.0f * sx;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    for (int i = 0; text[i]; i++) {
        int c = (unsigned char)text[i];
        if (c < 32 || c > 127) { cx += char_advance; continue; }
        if (c == ' ') { cx += char_advance * 0.6f; continue; }
        int col = (c - 0x20) % FONT_COLS;
        int row = (c - 0x20) / FONT_COLS;
        float u0 = (float)(col * FONT_CELL) / (float)FONT_TEX_W;
        float u1 = (float)((col + 1) * FONT_CELL) / (float)FONT_TEX_W;
        float v0 = (float)(row * FONT_CELL) / (float)FONT_TEX_H;
        float v1 = (float)((row + 1) * FONT_CELL) / (float)FONT_TEX_H;
        fe_draw_quad(cx, y, cell_w, cell_h, color, s_font_page, u0, v0, u1, v1);
        cx += char_advance;
    }
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
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

    /* Draw buttons */
    for (int i = 0; i < FE_MAX_BUTTONS; i++) {
        if (!s_buttons[i].active) continue;

        float bx = (float)s_buttons[i].x * sx;
        float by = (float)s_buttons[i].y * sy;
        float bw = (float)s_buttons[i].w * sx;
        float bh = (float)s_buttons[i].h * sy;

        int selected = (i == s_selected_button);
        uint32_t bg_color = s_buttons[i].disabled ? 0x80333333 :
                           selected ? 0xCC5080C0 : 0x99283858;
        uint32_t border_color = selected ? 0xFFAABBDD : 0xFF506080;

        /* Button background: use ButtonBits.tga gradient when available.
         * Original FUN_00425b60 tiles strips from ButtonBits (56x100) with
         * mode-dependent UV offsets. We sample a horizontal strip from the
         * texture, modulated by selected/disabled color tint. */
        if (s_buttonbits_tex_page >= 0 && !s_buttons[i].disabled) {
            /* Pick vertical strip in ButtonBits based on selection state.
             * Mode 0 = normal (row 0..0x20), mode 1 = selected (row 0x20..0x40). */
            float btn_v0 = selected ? (32.0f / (float)s_buttonbits_h)
                                    : (0.0f / (float)s_buttonbits_h);
            float btn_v1 = btn_v0 + 32.0f / (float)s_buttonbits_h;
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
            fe_draw_quad(bx, by, bw, bh, 0xFFFFFFFF,
                         s_buttonbits_tex_page, 0.0f, btn_v0, 1.0f, btn_v1);
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        } else {
            fe_draw_quad(bx, by, bw, bh, bg_color, -1, 0, 0, 1, 1);
        }

        /* Selection indicator -- bright left bar */
        if (selected && !s_buttons[i].disabled) {
            fe_draw_quad(bx - 4 * sx, by, 3 * sx, bh, 0xFFFFCC44, -1, 0, 0, 1, 1);
        }

        /* Button border */
        fe_draw_quad(bx, by - 1, bw, 1, border_color, -1, 0, 0, 1, 1);
        fe_draw_quad(bx, by + bh, bw, 1, 0xFF202030, -1, 0, 0, 1, 1);

        /* Button label text */
        if (s_buttons[i].label[0] && s_font_page >= 0) {
            int len = (int)strlen(s_buttons[i].label);
            float text_w = (float)len * 14.0f * sx;
            float tx = bx + (bw - text_w) * 0.5f;
            float ty = by + (bh - 20.0f * sy) * 0.5f;
            uint32_t text_color = selected ? 0xFFFFFFFF : 0xFFCCCCCC;
            if (s_buttons[i].disabled) text_color = 0xFF888888;
            fe_draw_text(tx, ty, s_buttons[i].label, text_color, sx, sy);
        }
    }

    /* (text overlay rendering deferred to font system) */

    /* Draw menu title indicator from mainfont.tga.
     * mainfont.tga (252x1152) contains pre-rendered title labels in 36px-tall rows.
     * From Ghidra analysis of FUN_00412e30 and FUN_00425660:
     *   - Each screen calls FUN_00412e30(slot_index) to get a surface handle
     *   - The title blocks start at row pixel (4 + slot_index) * 36 in the atlas
     *   - The title is drawn at (halfW - 200, y_anim_offset) by FUN_00425660
     *
     * Screen -> slot_index mapping (from decompiled screen functions):
     *   MainMenu=0, RaceType=1, ConnectionBrowser=2, QuickRace=3,
     *   OptionsHub=4, CarSelection=5, TrackSelection=6
     */
    if (s_mainfont_tex_page >= 0 && s_mainfont_w > 0 && s_mainfont_h > 0) {
        int slot_index = -1;
        switch (s_current_screen) {
        case TD5_SCREEN_MAIN_MENU:         slot_index = 0; break;
        case TD5_SCREEN_RACE_TYPE_MENU:    slot_index = 1; break;
        case TD5_SCREEN_CONNECTION_BROWSER:slot_index = 2; break;
        case TD5_SCREEN_QUICK_RACE:        slot_index = 3; break;
        case TD5_SCREEN_OPTIONS_HUB:       slot_index = 4; break;
        case TD5_SCREEN_CAR_SELECTION:     slot_index = 5; break;
        case TD5_SCREEN_TRACK_SELECTION:   slot_index = 6; break;
        default: break;
        }
        if (slot_index >= 0) {
            float block_h = 36.0f;
            /* Row offset = (4 + slot_index) * 36 in the atlas */
            float row_px = (float)(4 + slot_index) * block_h;
            float v0_title = row_px / (float)s_mainfont_h;
            float v1_title = (row_px + block_h) / (float)s_mainfont_h;
            if (v1_title <= 1.0f) {
                /* Original draws at (halfW - 200, ...) centered.
                 * halfW = 320, so x = 120 in 640-space. */
                float title_x = 120.0f * sx;
                float title_w = (float)s_mainfont_w * sx;
                float title_h = block_h * sy;
                float title_y = 4.0f * sy; /* near top of screen */
                td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
                fe_draw_quad(title_x, title_y, title_w, title_h, 0xFFFFFFFF,
                             s_mainfont_tex_page, 0.0f, v0_title, 1.0f, v1_title);
                td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
            }
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
            cmd->tex_page == s_mainfont_tex_page ||
            cmd->tex_page == s_font_page ||
            cmd->tex_page == s_buttonbits_tex_page)
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
    s_start_race_request = 0;
    s_start_race_confirm = 0;
    s_attract_idle_counter = 0;
    s_attract_idle_timestamp = td5_plat_time_ms();
    s_flow_context = 0;
    s_selected_game_type = -1;
    s_race_within_series = 0;
    s_cup_unlock_tier = 0;
    s_two_player_mode = 0;
    s_selected_car = 0;
    s_selected_paint = 0;
    s_selected_config = 0;
    s_selected_transmission = 0;
    s_selected_track = 0;
    s_track_direction = 0;
    s_network_active = 0;
    s_kicked_flag = 0;
    s_lobby_action = 0;
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
        frontend_load_tga("Front_End/Positioner.tga", "Front_End/FrontEnd.zip");
        s_inner_state = 1;
        break;
    case 1: /* Present grid */
        frontend_present_buffer();
        s_inner_state = 2;
        break;
    case 2: /* Init grid */
        s_inner_state = 3;
        break;
    case 3: /* Navigate grid with arrow keys */
        /* Arrow key navigation placeholder */
        break;
    case 4: /* Edit cell values */
        break;
    case 5: /* Write positioner.txt */
        TD5_LOG_I(LOG_TAG, "ScreenPositionerDebugTool: writing positioner.txt");
        break;
    default:
        break;
    }
}

/* ========================================================================
 * [2] RunAttractModeDemoScreen (0x4275A0) -- Attract mode / demo
 * States: 6
 * ======================================================================== */

static void Screen_AttractModeDemo(void) {
    switch (s_inner_state) {
    case 0: /* Set attract mode flag */
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
        frontend_load_tga("Front_End/Language.tga", "Front_End/FrontEnd.zip");
        frontend_load_tga("Front_End/LanguageScreen.tga", "Front_End/FrontEnd.zip");
        /* Create 4 language flag-icon buttons */
        s_inner_state = 1;
        break;

    case 1: /* Tick */
    case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Interaction -- wait for language selection */
    case 4:
    case 5:
        if (s_input_ready && s_button_index >= 0 && s_button_index < 4) {
            /* Store language selection */
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
        frontend_load_tga("Front_End/LegalScreen.tga", "Front_End/FrontEnd.zip");
        /* Draw "TEST_DRIVE_5_COPYRIGHT_1998" */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Fade in */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
            s_anim_tick = 0;
            s_inner_state = 2;
        }
        break;

    case 2: /* 3-second timer */
        s_anim_tick++;
        /* ~3 seconds at 30fps = 90 ticks, or use timestamp */
        if (s_anim_tick >= 90) {
            s_anim_tick = 0;
            s_inner_state = 3;
        }
        break;

    case 3: /* Fade out + exit to main menu */
        s_anim_tick++;
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
        TD5_LOG_D(LOG_TAG, "MainMenu: state 0 - init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

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
        frontend_create_button("Race Menu",  0, 0, 200, 32);
        frontend_create_button("Quick Race", 0, 0, 200, 32);
        frontend_create_button("Two Player", 0, 0, 200, 32);
        frontend_create_button("Net Play",   0, 0, 200, 32);
        frontend_create_button("Options",    0, 0, 200, 32);
        frontend_create_button("Hi-Score",   0, 0, 200, 32);
        frontend_create_button("Exit",       0, 0, 200, 32);

        frontend_set_cursor_visible(1);
        frontend_play_sfx(5); /* menu ready */
        s_inner_state = 1;
        break;

    case 1: /* Present buffer */
        frontend_present_buffer();
        s_inner_state = 2;
        break;

    case 2: /* Reset tick counter, rebuild button surfaces */
        s_anim_tick = 0;
        frontend_present_buffer();
        s_inner_state = 3;
        break;

    case 3: /* Slide-in animation: 7 buttons alternating L/R, title descends. 39 frames. */
        s_anim_tick++;
        /* Buttons alternate slide from left (-640) and right (+640),
         * approaching final X over 39 frames. Title slides from Y=-40 to Y=final. */
        if (s_anim_tick >= 0x27) { /* 39 frames */
            frontend_set_cursor_visible(0);
            frontend_play_sfx(4); /* ready chime */
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

            case 6: /* Exit -> show Yes/No confirm */
                s_inner_state = 5;
                break;
            }
        }
        break;

    case 5: { /* Exit confirm dialog: create Yes/No buttons */
        /* From Ghidra: halfW=320, halfH=240
         * Yes: (halfW-0xC2, halfH+0xA5) = (126, 405), size 0x60x0x20 (96x32)
         * No:  (halfW-0x42, halfH+0xA5) = (254, 405), size 0x70x0x20 (112x32) */
        int yes_idx = frontend_create_button("Yes", 126, 405, 96, 32);
        int no_idx  = frontend_create_button("No",  254, 405, 112, 32);
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

    case 8: /* Slide-out prep: blit secondary surface, restore state */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        s_anim_tick = 0;
        s_inner_state = 9;
        break;

    case 9: /* Slide-out animation: buttons scatter, 16 frames */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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

    case 12: /* Scatter buttons for exit transition */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: state 0 - init");
        frontend_load_tga("Front_End/RaceMenu.tga", "Front_End/FrontEnd.zip");

        /* Create 0x110 x 0xB4 description preview surface */
        /* Create 7 buttons for race types */
        frontend_create_button("Single Race", 0, 0, 200, 32);
        frontend_create_button("Cup Race",    0, 0, 200, 32);
        /* Continue Cup: greyed if no valid CupData.td5 */
        if (frontend_validate_cup_checksum())
            frontend_create_button("Continue Cup", 0, 0, 200, 32);
        else
            frontend_create_preview_button("Continue Cup", 0, 0, 200, 32);
        frontend_create_button("Time Trials", 0, 0, 200, 32);
        frontend_create_button("Drag Race",   0, 0, 200, 32);
        frontend_create_button("Cop Chase",   0, 0, 200, 32);
        frontend_create_button("Back",        0, 0, 200, 32);

        s_selected_game_type = -1;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 2;
        }
        break;

    case 2: /* Tick until AdvanceFrontendTickAndCheckReady */
        if (frontend_advance_tick()) {
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

    case 5: /* Slide-out: buttons scatter */
        s_anim_tick = 0;
        s_inner_state = 0x14;
        break;

    /* --- Cup sub-menu (states 6-12) --- */

    case 6: /* Cup sub-menu: release top buttons, create cup tier buttons */
        TD5_LOG_D(LOG_TAG, "RaceTypeCategory: entering cup sub-menu");
        /* Create 7 cup tier buttons */
        frontend_create_button("Championship", 0, 0, 200, 32); /* always available */
        frontend_create_button("Era",          0, 0, 200, 32); /* always available */

        /* Challenge: locked if s_cup_unlock_tier == 0 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button("Challenge", 0, 0, 200, 32);
        else
            frontend_create_preview_button("Challenge", 0, 0, 200, 32);

        /* Pitbull: locked if s_cup_unlock_tier < 1 */
        if (s_cup_unlock_tier >= 1)
            frontend_create_button("Pitbull", 0, 0, 200, 32);
        else
            frontend_create_preview_button("Pitbull", 0, 0, 200, 32);

        /* Masters: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button("Masters", 0, 0, 200, 32);
        else
            frontend_create_preview_button("Masters", 0, 0, 200, 32);

        /* Ultimate: locked if s_cup_unlock_tier < 2 */
        if (s_cup_unlock_tier >= 2)
            frontend_create_button("Ultimate", 0, 0, 200, 32);
        else
            frontend_create_preview_button("Ultimate", 0, 0, 200, 32);

        frontend_create_button("Back", 0, 0, 200, 32);

        s_anim_tick = 0;
        s_inner_state = 7;
        break;

    case 7: /* Cup sub-menu slide-in */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        s_inner_state = 0; /* re-init top menu */
        break;

    case 12: /* Cup description preview update */
        s_inner_state = 9;
        break;

    /* --- Return transition --- */
    case 0x14: /* Slide-out animation, then navigate */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "QuickRaceMenu: init");
        /* Load background: same as main menu */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        /* Validate car/track indices against max counts */
        if (s_selected_car < 0) s_selected_car = 0;
        if (s_selected_track < 0) s_selected_track = 0;

        /* Create 0x208 x 200px info panel */
        /* Draw car name and track name */
        /* Show "Locked" if car/track is locked and not in network mode */

        /* From Ghidra: halfW=320, halfH=240
         * ChangeCar:   (halfW-200, halfH-0x67) = (120, 137), size 0x100x0x20 (256x32)
         * ChangeTrack: (120, halfH+0x11)       = (120, 257), size 256x32
         * OK:          (120, halfH+0x89)        = (120, 377), size 0x60x0x20 (96x32)
         * Back:        (halfW-0x58, halfH+0x89) = (232, 377), size 0x70x0x20 (112x32) */
        frontend_create_button("Change Car",   120, 137, 256, 32);
        frontend_create_button("Change Track", 120, 257, 256, 32);
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
        s_anim_tick++;
        if (s_anim_tick >= 0x27) {
            s_inner_state = 4;
        }
        break;

    case 4: /* Interactive: arrow input cycles car/track, OK/Back dispatch */
        if (s_input_ready) {
            if (s_button_index == 0 && s_arrow_input != 0) {
                /* Cycle car */
                s_selected_car += s_arrow_input;
                /* Wrap car: network w/ unlocks = 0-36, cheats = 0-32, else DAT_00463e0c */
                {
                    int car_max = s_cheat_unlock_all ? 32 : s_total_unlocked_cars;
                    if (s_network_active) car_max = 36;
                    if (s_selected_car < 0) s_selected_car = car_max;
                    if (s_selected_car > car_max) s_selected_car = 0;
                }
                /* Redraw car name, locked status */
            }
            if (s_button_index == 1 && s_arrow_input != 0) {
                /* Cycle track */
                s_selected_track += s_arrow_input;
                {
                    int track_max = s_network_active ? 0x13 : s_total_unlocked_tracks;
                    if (s_selected_track < 0) s_selected_track = track_max - 1;
                    if (s_selected_track >= track_max) s_selected_track = 0;
                }
                /* Redraw track name, locked status */
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
        s_anim_tick = 0;
        s_inner_state = 6;
        break;

    case 6: /* Slide-out: 16 frames, then dispatch */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "ConnectionBrowser: init");
        frontend_net_enumerate();
        /* Create buttons: connection list, OK, Back */
        frontend_create_button("OK",   0, 0, 100, 32);
        frontend_create_button("Back", 0, 0, 100, 32);
        s_inner_state = 1;
        break;

    case 1: /* Build list UI */
        s_inner_state = 2;
        break;

    case 2: /* Slide-in */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) s_inner_state = 3;
        break;

    case 3: /* Tick + render */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Flash highlight */
        s_inner_state = 5;
        break;

    case 5: /* Selection interaction */
        if (s_input_ready) {
            if (s_button_index == 1) { /* OK */
                s_return_screen = TD5_SCREEN_SESSION_PICKER;
                s_inner_state = 8;
            }
            if (s_button_index == 2) { /* Back */
                s_return_screen = TD5_SCREEN_MAIN_MENU;
                s_inner_state = 8;
            }
        }
        break;

    case 6: /* Highlight browse */
    case 7: /* Scroll */
        s_inner_state = 5;
        break;

    case 8: /* Slide-out */
        s_anim_tick = 0;
        s_inner_state = 9;
        break;

    case 9:
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "SessionPicker: init");
        frontend_create_button("Create", 0, 0, 100, 32);
        frontend_create_button("OK",     0, 0, 100, 32);
        frontend_create_button("Back",   0, 0, 100, 32);
        s_inner_state = 1;
        break;

    case 1: /* Build session list */
        s_inner_state = 2;
        break;

    case 2: /* Slide-in */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) s_inner_state = 3;
        break;

    case 3: /* Interaction */
        if (s_input_ready) {
            if (s_button_index == 0) { /* Create */
                s_inner_state = 4; /* create sub-flow */
            }
            if (s_button_index == 1) { /* OK / Join */
                s_return_screen = TD5_SCREEN_CREATE_SESSION;
                s_inner_state = 5;
            }
            if (s_button_index == 2) { /* Back */
                s_return_screen = TD5_SCREEN_CONNECTION_BROWSER;
                s_inner_state = 5;
            }
        }
        break;

    case 4: /* Create sub-flow -> redirect to create session screen */
        s_return_screen = TD5_SCREEN_CREATE_SESSION;
        s_inner_state = 5;
        break;

    case 5: /* Slide-out confirm */
        s_anim_tick = 0;
        s_inner_state = 6;
        break;

    case 6: /* Slide-out + exit */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "CreateSession: init");
        frontend_create_button("Enter Name", 0, 0, 300, 32);
        frontend_create_button("Back",       0, 0, 100, 32);
        s_text_input_state = 1;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) s_inner_state = 2;
        break;

    case 2: /* Name input */
        frontend_render_text_input();
        if (frontend_text_input_confirmed()) {
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
        /* HOST path: states 4-8 -> set g_networkSessionActive=1, nav to car select */
        /* CLIENT path: states 0x10-0x15 -> JoinSession, send join msg, nav to lobby */
        s_network_active = 1;
        td5_frontend_set_screen(TD5_SCREEN_CAR_SELECTION);
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
        frontend_create_button("",           0, 0, 0x1D0, 0x18);  /* text input bar */
        frontend_create_button("Messages",   0, 0, 0x200, 0x80);  /* message window */
        frontend_create_button("Status",     0, 0, 0xE0,  0x86);  /* status panel */
        frontend_create_button("Change Car", 0, 0, 200,   0x20);
        frontend_create_button("Start",      0, 0, 0x78,  0x20);
        frontend_create_button("Exit",       0, 0, 0x78,  0x20);

        /* Allocate chat input surface */
        memset(s_chat_input_buffer, 0, sizeof(s_chat_input_buffer));

        s_chat_dirty = (s_network_active) ? 1 : 0;
        s_lobby_action = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* ANIMATE IN (20 frames) */
        s_anim_tick++;
        /* Clear screen on first frame */
        /* Animate buttons sliding into position */
        if (s_anim_tick >= 0x14) { /* 20 frames */
            frontend_play_sfx(4);
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
            frontend_create_button("Yes", 0, 0, 80, 32);
            frontend_create_button("No",  0, 0, 80, 32);
        } else {
            frontend_create_button("Ok", 0, 0, 80, 32);
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
        s_anim_tick++;
        if (s_anim_tick >= 0x18) { /* 24 frames */
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
        s_anim_tick++;
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
        TD5_LOG_D(LOG_TAG, "OptionsHub: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");

        frontend_create_button("Game Options",      0, 0, 200, 32);
        frontend_create_button("Control Options",   0, 0, 200, 32);
        frontend_create_button("Sound Options",     0, 0, 200, 32);
        frontend_create_button("Graphics Options",  0, 0, 200, 32);
        frontend_create_button("Two Player Options",0, 0, 200, 32);
        frontend_create_button("OK",                0, 0, 200, 32);

        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: /* Present */
    case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
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
        s_anim_tick = 0;
        s_inner_state = 8;
        break;

    case 8: /* Slide-out: 16 frames */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "GameOptions: init");
        /* 7 option rows with left/right arrows:
         * Circuit Laps, Checkpoint Timers, Traffic, Cops,
         * Difficulty, Dynamics, 3D Collisions */
        frontend_create_button("OK", 0, 0, 200, 32);
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
        break;

    case 4: /* Draw current values */
    case 5:
        /* Render current option values on the panel */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: arrow handlers per row */
        if (s_input_ready) {
            /* Each row cycles its respective global on arrow input.
             * OK button triggers exit. */
            if (s_button_index == 7) { /* OK */
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
            /* Arrow changes reset to state 4 for redraw */
        }
        break;

    case 7: /* Prep slide-out */
        s_anim_tick = 0;
        s_inner_state = 8;
        break;

    case 8: /* Slide-out: 16 frames */
        s_anim_tick++;
        if (s_anim_tick >= 16) s_inner_state = 9;
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
        TD5_LOG_D(LOG_TAG, "ControlOptions: init");
        /* Create P1/P2 controller assignment display + Configure/OK buttons */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
        break;
    case 4: case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            /* "Configure" -> Screen_ControllerBinding */
            /* "OK" -> OptionsHub */
            if (s_button_index == 0) {
                s_return_screen = TD5_SCREEN_CONTROLLER_BINDING;
                s_inner_state = 7;
            }
            if (s_button_index == 1) {
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
        }
        break;
    case 7:
        s_anim_tick = 0;
        s_inner_state = 8;
        break;
    case 8:
        s_anim_tick++;
        if (s_anim_tick >= 16) s_inner_state = 9;
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
        TD5_LOG_D(LOG_TAG, "SoundOptions: init");
        /* Load VolumeBox.tga, VolumeFill.tga for volume bars */
        /* Create 4 option rows + OK button */
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
        break;
    case 4: case 5:
        /* Render volume bars */
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            if (s_button_index == 3) { /* Music Test */
                s_return_screen = TD5_SCREEN_MUSIC_TEST;
                s_inner_state = 7;
            }
            if (s_button_index == 4) { /* OK */
                s_return_screen = TD5_SCREEN_OPTIONS_HUB;
                s_inner_state = 7;
            }
            /* Arrow changes on volume adjust to state 4 for redraw */
        }
        break;
    case 7:
        s_anim_tick = 0;
        s_inner_state = 8;
        break;
    case 8:
        s_anim_tick++;
        if (s_anim_tick >= 16) s_inner_state = 9;
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
        TD5_LOG_D(LOG_TAG, "DisplayOptions: init");
        frontend_init_display_mode_state();
        frontend_create_button("Resolution", 0, 0, 200, 32);
        frontend_create_button("Fogging", 0, 0, 200, 32);
        frontend_create_button("Speed Readout", 0, 0, 200, 32);
        frontend_create_button("Camera Damping", 0, 0, 200, 32);
        frontend_create_button("OK", 0, 0, 200, 32);
        frontend_refresh_display_option_labels();
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
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
            int changed = 0;

            if (s_button_index == 0 && delta != 0 && s_display_mode_count > 0) {
                s_display_mode_index += delta;
                if (s_display_mode_index < 0) s_display_mode_index = s_display_mode_count - 1;
                if (s_display_mode_index >= s_display_mode_count) s_display_mode_index = 0;
                td5_plat_apply_display_mode(
                    s_display_modes[s_display_mode_index].width,
                    s_display_modes[s_display_mode_index].height,
                    s_display_modes[s_display_mode_index].bpp);
                changed = 1;
            } else if (s_button_index == 1 && delta != 0) {
                s_display_fog_enabled = !s_display_fog_enabled;
                changed = 1;
            } else if (s_button_index == 2 && delta != 0) {
                s_display_speed_units = !s_display_speed_units;
                changed = 1;
            } else if (s_button_index == 3 && delta != 0) {
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
    case 7:
        s_anim_tick = 0;
        s_inner_state = 8;
        break;
    case 8:
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "TwoPlayerOptions: init");
        frontend_load_tga("Front_End/SplitScreen.tga", "Front_End/FrontEnd.zip");
        s_anim_tick = 0;
        s_inner_state = 1;
        break;
    case 1: case 2:
        frontend_present_buffer();
        s_inner_state++;
        break;
    case 3:
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
        break;
    case 4: case 5:
        s_inner_state = 6;
        break;
    case 6:
        if (s_input_ready) {
            /* Button 0: toggle split mode (H/V) */
            /* Button 1: toggle catch-up */
            /* OK -> exit */
            if (s_button_index == 2) {
                s_inner_state = 7;
            }
        }
        break;
    case 7:
        s_anim_tick = 0;
        s_inner_state = 8;
        break;
    case 8:
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "ControllerBinding: init");
        s_inner_state = 1;
        break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        /* Binding page setup states */
        s_inner_state++;
        break;
    case 9: /* Slide-in */
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 10;
        break;
    case 10: /* Interactive binding poll */
        /* Poll joystick for button/axis mapping */
        if (s_input_ready) {
            /* Map button presses to game actions */
            /* OK -> confirm and exit */
            s_inner_state = 14;
        }
        break;
    case 11: case 12: case 13:
        /* Axis mapping sub-states */
        s_inner_state = 10;
        break;
    case 14: /* Confirm */
    case 15: case 16: case 17: case 18:
        s_anim_tick = 0;
        s_inner_state = 19;
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
        TD5_LOG_D(LOG_TAG, "MusicTestExtras: init");
        /* Release gallery images, load band photos */
        /* Create title, track name surface, now-playing surface */
        /* Draw initial track info */
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        frontend_create_button("Select Track", 0, 0, 200, 32);
        frontend_create_button("OK",           0, 0, 200, 32);
        s_music_test_track_idx = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present, reset counter */
        frontend_present_buffer();
        s_anim_tick = 0;
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x27) s_inner_state = 4;
        break;

    case 4: case 5: /* Static display */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: cycle tracks, play, OK */
        if (s_input_ready) {
            if (s_button_index == 0 && s_arrow_input != 0) {
                /* Cycle track index 0..11 */
                s_music_test_track_idx += s_arrow_input;
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
        s_anim_tick = 0;
        s_inner_state = 8;
        break;

    case 8: /* Slide-out: 32 frames. Restore gallery images. */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        TD5_LOG_D(LOG_TAG, "CarSelection: state 0 - init");

        /* Car selection uses a dark background with overlay elements (sidebar,
         * curve, topbar), NOT a fullscreen background TGA like other screens.
         * Load overlay UI assets; s_background_surface stays 0 (dark clear). */
        frontend_load_tga("Front_End/CarSelBar1.tga",  "Front_End/FrontEnd.zip");
        frontend_load_tga("Front_End/CarSelCurve.tga", "Front_End/FrontEnd.zip");
        frontend_load_tga("Front_End/CarSelTopBar.tga","Front_End/FrontEnd.zip");
        frontend_load_tga("Front_End/GraphBars.tga",   "Front_End/FrontEnd.zip");
        /* Ensure no fullscreen bg is active (overlays are narrow, won't trigger auto-set) */
        s_background_surface = 0;

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
        s_inner_state = 1;
        break;

    case 1: /* Reset tick counter */
        s_anim_tick = 0;
        s_inner_state = 2;
        break;

    case 2: /* Sidebar slide-in: bar slides from right, curve+topbar from left */
        s_anim_tick++;
        /* Skip if returning from network car select or 2P round 2 */
        if ((s_two_player_mode & 4) != 0 || s_network_active) {
            s_inner_state = 3;
        } else if (s_anim_tick >= 24) {
            s_inner_state = 3;
        }
        break;

    case 3: /* Present + copy primary to secondary */
        frontend_present_buffer();
        s_inner_state = 4;
        break;

    case 4: /* Button creation: 5-6 buttons */
        frontend_create_button("Car",        0, 0, 200, 32); /* with L/R arrows */
        frontend_create_button("Paint",      0, 0, 200, 32); /* with L/R arrows */
        frontend_create_button("Config",     0, 0, 200, 32);
        frontend_create_button("Auto",       0, 0, 200, 32); /* Auto/Manual toggle */
        frontend_create_button("OK",         0, 0, 200, 32);
        frontend_create_button("Back",       0, 0, 200, 32);

        /* Time Trials: grey out Manual button */
        /* Load inline string table SNK_CarSelect_MT1 */
        s_anim_tick = 0;
        s_inner_state = 5;
        break;

    case 5: /* Button slide-in: 24 frames, 32px/frame from right */
        s_anim_tick++;
        if (s_anim_tick >= 0x18) { /* 24 frames */
            s_inner_state = 6;
        }
        break;

    case 6: /* Tick until ready */
        if (frontend_advance_tick()) {
            s_inner_state = 7;
        }
        break;

    case 7: /* Main interaction loop: car preview + input */
        /* Render car preview overlay */
        if (s_input_ready && s_button_index >= 0) {
            switch (s_button_index) {
            case 0: /* Car: L/R arrows cycle car index */
                if (s_arrow_input != 0) {
                    if (s_selected_game_type == 5) {
                        /* Masters: cycle through roster, skip AI slots */
                        int attempts = 0;
                        do {
                            s_selected_car += s_arrow_input;
                            if (s_selected_car < 0) s_selected_car = 14;
                            if (s_selected_car > 14) s_selected_car = 0;
                            attempts++;
                        } while (s_masters_roster_flags[s_selected_car] == 1 && attempts < 15);
                    } else {
                        s_selected_car += s_arrow_input;
                        if (s_selected_car < s_car_roster_min) s_selected_car = s_car_roster_max;
                        if (s_selected_car > s_car_roster_max) s_selected_car = s_car_roster_min;
                    }
                    s_inner_state = 10; /* trigger new car image load */
                }
                break;

            case 1: /* Paint: L/R cycle paint 0-3 */
                if (s_arrow_input != 0) {
                    /* Disabled for cop cars (0x1C-0x24) */
                    int actual_car = (s_selected_game_type == 5) ?
                                     s_masters_roster[s_selected_car] : s_selected_car;
                    if (actual_car < 0x1C || actual_car > 0x24) {
                        s_selected_paint += s_arrow_input;
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
                if (s_selected_game_type != 7) { /* not Time Trials */
                    s_selected_transmission = !s_selected_transmission;
                    /* Update button label */
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
        s_anim_tick++;
        if (s_anim_tick >= 2) {
            s_inner_state = 7;
        }
        break;

    case 9: /* Unused */
        s_inner_state = 7;
        break;

    case 10: /* Clear car preview area, prep for new image load */
        s_anim_tick = 0;
        s_inner_state = 11;
        break;

    case 11: /* Car preview cross-fade transition */
        s_anim_tick++;
        if (s_anim_tick >= 8) {
            s_inner_state = 12;
        }
        break;

    case 12: /* Load car image: CarPic<paint>.tga from car ZIP */
    {
        int actual_car = (s_selected_game_type == 5) ?
                         s_masters_roster[s_selected_car] : s_selected_car;
        char buf[128];
        sprintf(buf, "CarPic%d.tga", s_selected_paint);
        TD5_LOG_D(LOG_TAG, "CarSelection: loading %s for car %d", buf, actual_car);

        /* Release old surface, load new preview */
        /* Render car name */
        /* Show "Locked" / "Beauty" / "Beast" labels */

        s_inner_state = 13;
    }
        break;

    case 13: /* Car preview fade-in */
        s_anim_tick++;
        if (s_anim_tick >= 8) {
            s_inner_state = 14;
        }
        break;

    case 14: /* Car preview slide-in from right, 25 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x19) {
            s_inner_state = 7; /* return to interaction */
        }
        break;

    case 15: /* Config sub-screen: render spec headers + graph bars */
        /* Render SNK_Config_Hdrs and per-car spec values */
        /* Headers starting with '*' are skipped */
        s_inner_state = 16;
        break;

    case 16: /* Return from config */
        s_car_preview_overlay = 1;
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
        s_anim_tick = 0;
        s_inner_state = 0x15;
        break;

    case 0x15: /* Cross-fade */
        s_anim_tick++;
        if (s_anim_tick >= 8) s_inner_state = 0x16;
        break;

    case 0x16: /* Release car surface */
    case 0x17:
        frontend_play_sfx(5);
        s_anim_tick = 0;
        s_inner_state = 0x18;
        break;

    case 0x18: /* Button slide-out: 24 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x18) {
            s_inner_state = 0x19;
        }
        break;

    case 0x19: /* Screen wipe: vertical bar sweep */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        TD5_LOG_D(LOG_TAG, "TrackSelection: init");

        /* Validate track index for cup modes: skip locked/invalid NPC groups */
        /* Determine track max for current mode */
        if (s_network_active) {
            s_track_max = 18; /* 19 tracks total */
        } else if (s_two_player_mode) {
            s_track_max = s_total_unlocked_tracks;
        } else {
            s_track_max = s_total_unlocked_tracks;
        }

        /* Create buttons */
        frontend_create_button("Track",     0, 0, 200, 32); /* with L/R arrows */
        frontend_create_button("Forwards",  0, 0, 200, 32); /* direction toggle */
        frontend_create_button("OK",        0, 0, 200, 32);
        /* Quick Race mode: no Back button */
        if (s_flow_context != 2) {
            frontend_create_button("Back", 0, 0, 200, 32);
        }

        /* Create 0x128 x 0xB8 info surface */
        frontend_load_tga("Front_End/TrackSelect.tga", "Front_End/FrontEnd.zip");

        s_track_direction = 0;
        s_anim_tick = 0;
        s_inner_state = 1;
        break;

    case 1: case 2: /* Present + tick */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 3: /* Slide-in: 39 frames */
        s_anim_tick++;
        /* Hide direction button if track has no reverse */
        if (s_anim_tick >= 0x27) {
            s_inner_state = 4;
        }
        break;

    case 4: /* Main interaction: track preview + navigation */
        if (s_input_ready) {
            if (s_button_index == 0 && s_arrow_input != 0) {
                /* Cycle track index */
                s_selected_track += s_arrow_input;

                /* Wrap based on mode */
                if (s_network_active) {
                    if (s_selected_track < 0) s_selected_track = s_track_max;
                    if (s_selected_track > s_track_max) s_selected_track = 0;
                } else if (s_two_player_mode) {
                    /* 2P: range [-1, track_max-1], -1 = random */
                    if (s_selected_track < -1) s_selected_track = s_track_max - 1;
                    if (s_selected_track >= s_track_max) s_selected_track = -1;
                } else {
                    if (s_selected_track < 0) s_selected_track = s_track_max - 1;
                    if (s_selected_track >= s_track_max) s_selected_track = 0;
                }

                /* For cup modes (type > 7): skip non-playable NPC groups */
                s_inner_state = 5; /* trigger track change display */
            }

            if (s_button_index == 1) {
                /* Direction toggle: 0=Forwards, 1=Backwards */
                /* Only if track supports reverse */
                s_track_direction = !s_track_direction;
                /* Rebuild button label: Forwards <-> Backwards */
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
        s_inner_state = 4;
        break;

    case 6: /* Slide-out prep */
        frontend_play_sfx(5);
        s_anim_tick = 0;
        s_inner_state = 7;
        break;

    case 7: /* Slide-out animation: 39 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x27) {
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
 * States: 8
 * Exits game when complete or cancelled.
 * ======================================================================== */

static int s_gallery_scroll_pos;
static int s_gallery_section_count;

static void Screen_ExtrasGallery(void) {
    switch (s_inner_state) {
    case 0: /* Fade transition gate */
        /* Wait for fade value to settle */
        s_inner_state = 1;
        break;

    case 1: /* Load all resources: 22 mugshots + 5 legal pages */
        TD5_LOG_D(LOG_TAG, "ExtrasGallery: loading mugshot resources");
        /* Load from Extras/Mugshots.zip:
         * Bob, Gareth, Snake, MikeT, Chris, Headley, Steve, Rich,
         * Mike, Bez, Les, TonyP, JohnS, DavidT, TonyC,
         * DaveyB, ChrisD, Slade, Matt, Marie, JFK, Daz */
        /* Load 5 legal pages (Legals1-5.tga) */
        s_gallery_scroll_pos = 0;
        s_gallery_section_count = 0;
        s_inner_state = 2;
        break;

    case 2: case 3: case 4: case 5: /* Skip frames (4 frames setup) */
        s_inner_state++;
        break;

    case 6: /* Fill secondary with black, set scroll counter */
        s_gallery_scroll_pos = 0x27F; /* 639 */
        s_inner_state = 7;
        break;

    case 7: /* Main scrolling loop */
        /* Cylindrical scroll: render current mugshot with vertical wrap */
        /* Every 32px of scroll, render one line of credits text */
        /* Special prefixes:
         *   '#' = mugshot reference (next char = index)
         *   '*' = section separator (11 separators -> exit game)
         */
        s_gallery_scroll_pos--;

        /* Check exit conditions */
        if (frontend_check_escape() || s_gallery_section_count >= 0x0B) {
            /* Credits complete or cancelled -> exit game */
            frontend_post_quit();
        }
        break;
    }
}

/* ========================================================================
 * [23] ScreenPostRaceHighScoreTable (0x413580)
 * States: 9
 * ======================================================================== */

static int s_score_category_index;

static void Screen_PostRaceHighScore(void) {
    switch (s_inner_state) {
    case 0: /* Init: load BG, create surfaces, buttons */
        TD5_LOG_D(LOG_TAG, "PostRaceHighScore: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create 0x208 x 0x90 score panel surface (black fill) */
        /* Create nav button + OK button */
        frontend_create_button("Navigate", 0, 0, 100, 32);
        frontend_create_button("OK",       0, 0, 100, 32);
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
        s_anim_tick++;
        if (s_anim_tick >= 0x27) {
            frontend_set_cursor_visible(0);
            frontend_play_sfx(4);
            s_inner_state = 4;
        }
        break;

    case 4: case 5: /* Static display (2 frames) */
        s_inner_state = 6;
        break;

    case 6: /* Interactive: L/R arrows browse score categories */
        if (s_input_ready) {
            if (s_arrow_input != 0) {
                s_score_category_index += s_arrow_input;
                /* Wrap range: locked mode [0x13, DAT_00466840], unlocked [0, 0x19] */
                if (s_cheat_unlock_all) {
                    if (s_score_category_index < 0) s_score_category_index = 0x19;
                    if (s_score_category_index > 0x19) s_score_category_index = 0;
                } else {
                    if (s_score_category_index < 0) s_score_category_index = s_total_unlocked_tracks - 1;
                    if (s_score_category_index >= s_total_unlocked_tracks) s_score_category_index = 0;
                }
                /* Redraw score panel */
            }
            if (s_button_index == 1) { /* OK */
                s_inner_state = 7;
            }
        }
        break;

    case 7: /* Prep slide-out */
        frontend_set_cursor_visible(1);
        frontend_play_sfx(5);
        s_anim_tick = 0;
        s_inner_state = 8;
        break;

    case 8: /* Slide-out: 16 frames */
        s_anim_tick++;
        if (s_anim_tick >= 16) {
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
        s_anim_tick++;
        if (s_anim_tick >= 0x27) {
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
        s_anim_tick++;
        if (s_anim_tick >= 17) {
            s_inner_state = 6; /* back to interactive */
        }
        break;

    case 9: case 10: /* Slide right animation: 17 frames */
        s_anim_tick++;
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
            frontend_create_button("Race Again",      0, 0, 200, 32);
            frontend_create_button("View Replay",     0, 0, 200, 32);
            frontend_create_button("View Race Data",  0, 0, 200, 32);
            frontend_create_button("Select New Car",  0, 0, 200, 32);
            frontend_create_button("Quit",            0, 0, 200, 32);
        } else {
            /* Cup Race (types 1-6): 5 buttons */
            int next_valid = ConfigureGameTypeFlags();
            frontend_create_button("Next Cup Race",   0, 0, 200, 32);
            frontend_create_button("View Replay",     0, 0, 200, 32);
            frontend_create_button("View Race Data",  0, 0, 200, 32);
            frontend_create_button("Save Race Status",0, 0, 200, 32);
            frontend_create_button("Quit",            0, 0, 200, 32);

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
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        frontend_create_button("OK", 0, 0, 100, 32);
        s_anim_tick = 0;
        s_inner_state = 0x12;
        break;

    case 0x12: /* Save confirmation slide-in: 32 frames */
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
            s_inner_state = 0x13;
        }
        break;

    case 0x13: /* Save confirmation wait */
        if (s_input_ready && s_button_index >= 0) {
            s_inner_state = 0x14;
        }
        break;

    case 0x14: /* Save confirmation slide-out: 32 frames -> back to menu */
        s_anim_tick = 0;
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        TD5_LOG_D(LOG_TAG, "PostRaceNameEntry: qualification check");
        /* Determine score type (time/lap/points).
         * Compare player's result against worst entry in 5-slot table.
         * If doesn't qualify, or 2P mode, or disqualified -> skip to state 4. */
        /* Create text input button */
        s_text_input_state = 1;
        s_inner_state = 1;
        break;

    case 1: /* Slide-in: 32 frames */
        s_anim_tick = 0;
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        s_anim_tick = 0;
        s_anim_tick++;
        if (s_anim_tick >= 0x20) {
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
        s_anim_tick = 0;
        s_anim_tick++;
        if (s_anim_tick >= 0x27) {
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
        s_anim_tick++;
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

        TD5_LOG_D(LOG_TAG, "CupFailed: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create 0x198 x 0x70 dialog surface */
        /* Draw 4 lines: Sorry / You Failed / To Win / [race type name] */
        frontend_create_button("OK", 0, 0, 100, 32);
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick = 0;
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

        TD5_LOG_D(LOG_TAG, "CupWon: init -- deleting CupData.td5");
        frontend_delete_cup_data();

        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create 0x198 x 0xC4 dialog surface */
        /* Draw: Congrats (y=0), You Have Won (y=0x38), race type name (y=0x54) */
        /* If unlocked car: draw at y=0x8C */
        /* If unlocked track: draw at y=0xA8 */
        frontend_create_button("OK", 0, 0, 100, 32);
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick = 0;
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
        TD5_LOG_D(LOG_TAG, "StartupInit: state 0");
        frontend_create_button("OK", 0, 0, 100, 32);
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
        TD5_LOG_D(LOG_TAG, "SessionLocked: init");
        frontend_load_tga("Front_End/MainMenu.tga", "Front_End/FrontEnd.zip");
        /* Create 0x198 x 0x70 dialog */
        /* Draw: SNK_SorryTxt (y=0), SNK_SeshLockedTxt (y=0x38) */
        frontend_create_button("OK", 0, 0, 100, 32);
        s_inner_state = 1;
        break;

    case 1: case 2: case 3: /* Present (3 frames) */
        frontend_present_buffer();
        s_inner_state++;
        break;

    case 4: /* Slide-in: 32 frames */
        s_anim_tick = 0;
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
