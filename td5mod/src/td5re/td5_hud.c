/**
 * td5_hud.c -- Race HUD, minimap, text rendering, pause menu overlay
 *
 * Translated from the original TD5_d3d.exe binary. All screen positions
 * scale from a 640x480 virtual resolution. The HUD renders after the 3D
 * scene using pre-transformed D3D vertices (textured quads via the
 * translucent primitive pipeline).
 *
 * Original addresses:
 *   0x4377B0  InitializeRaceOverlayResources
 *   0x437BA0  InitializeRaceHudLayout
 *   0x4388A0  RenderRaceHudOverlays
 *   0x4397B0  BuildRaceHudMetricDigits
 *   0x439B60  SetRaceHudIndicatorState
 *   0x439B70  DrawRaceStatusText
 *   0x43A220  RenderTrackMinimapOverlay
 *   0x43B0A0  InitializeMinimapLayout
 *   0x43B7C0  InitializePauseMenuOverlayLayout
 *   0x428240  InitializeRaceHudFontGlyphAtlas
 *   0x428320  QueueRaceHudFormattedText
 *   0x428570  FlushQueuedRaceHudText
 */

#include "td5_hud.h"
#include "td5_platform.h"
#include "td5_asset.h"
#include "td5_render.h"
#include "td5_track.h"
#include "td5re.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define LOG_TAG "hud"

/* ========================================================================
 * Forward declarations for external functions not yet in headers
 * (These correspond to original binary functions called by the HUD)
 * ======================================================================== */

/* 0x442CF0: Find archive entry by name, returns atlas entry pointer */
extern TD5_AtlasEntry *td5_asset_find_atlas_entry(void *context, const char *name);

/* 0x430CF0: Allocate from game heap */
extern void *td5_game_heap_alloc(size_t size);

/* Returns completed lap index (0-based) for the given actor slot */
extern int td5_game_get_player_lap(int slot);

/* Returns race timer ticks (30/sec) for lap_index=0, or split time for 1-8 */
extern int32_t td5_game_get_race_timer(int slot, int lap_index);

/* Returns the configured circuit lap count from saved options */
extern int td5_save_get_circuit_lap_count(void);

/* 0x432BD0: Build sprite quad template from layout params */
extern void td5_render_build_sprite_quad(int *params);

/* 0x4315B0: Submit immediate translucent primitive for rendering */
extern void td5_render_submit_translucent(uint16_t *quad_data);

/* 0x43E640: Set viewport clip rect */
extern void td5_render_set_clip_rect(float left, float right, float top, float bottom);

/* 0x43E8E0: Set viewport projection center */
extern void td5_render_set_projection_center(float cx, float cy);

/* 0x439E60: Render radial pulse overlay effect */
extern void td5_render_radial_pulse(float dt);

/* 0x40A6A0: Cos from 12-bit angle (4096 = 360 degrees), returns float */
extern float td5_cos_12bit(uint32_t angle);

/* 0x40A6C0: Sin from 12-bit angle (4096 = 360 degrees), returns float */
extern float td5_sin_12bit(uint32_t angle);

/* 0x434040: Compute actor route heading delta */
extern uint32_t td5_compute_heading_delta(void *route_entry);

/* ========================================================================
 * HUD-owned globals (migrated from td5re_stubs.c)
 * ======================================================================== */

int     g_hud_metric_mode       = 0;
int     g_kph_mode              = 0;

/* HUD string table: 13 entries per player (matches Language.dll SNK exports).
 * [0..5] = position labels, [6..12] = UI labels (LAP, TIME, DEMO MODE, etc.) */
static const char *s_default_position_strings[] = {
    "1ST", "2ND", "3RD", "4TH", "5TH", "6TH",
    "WRONG WAY", "PIT STOP", "FINISH", "BEST LAP",
    "DEMO MODE", "TIME", "LAP",
    /* P2 copy */
    "1ST", "2ND", "3RD", "4TH", "5TH", "6TH",
    "WRONG WAY", "PIT STOP", "FINISH", "BEST LAP",
    "DEMO MODE", "TIME", "LAP"
};
const char **g_position_strings = s_default_position_strings;

static const char *s_default_wanted_line1[] = { "YOU ARE", "PULL OVER", "" };
static const char *s_default_wanted_line2[] = { "WANTED!", "NOW!", "" };
const char **g_wanted_msg_line1 = s_default_wanted_line1;
const char **g_wanted_msg_line2 = s_default_wanted_line2;

int     g_wanted_msg_timer      = 0;
int     g_wanted_msg_index      = 0;

/* Pause-menu font glyph widths — extracted from original binary at 0x4660C8.
 * 256 signed bytes: one per character code.  Row 0-1 (chars 0-31) are special
 * graphics in the PAUSETXT atlas; row 2+ are normal printable ASCII. */
const int8_t g_pause_glyph_widths[256] = {
    /* 0x00 */ 54, 84, 72,  0, 37, 51,100,  0, 37, 51,100, 75, 80, 72,  0,  0,
    /* 0x10 */ 37, 51,100, 77, 80, 72,  0,  0, 37,100, 32, 37,115,  0,  0,  0,
    /* 0x20 */  8,  8, 10, 14, 14, 25, 21,  9,  9,  9, 10, 14, 10, 10,  8, 13,
    /* 0x30 */ 17, 10, 17, 16, 17, 16, 16, 17, 17, 17,  8, 10, 12, 13, 11, 13,
    /* 0x40 */ 18, 19, 15, 15, 17, 13, 13, 19, 17,  8, 12, 18, 13, 23, 17, 20,
    /* 0x50 */ 16, 19, 16, 14, 14, 17, 18, 24, 19, 18, 17,  9, 13,  9, 13, 14,
    /* 0x60 */  9, 16, 16, 11, 15, 15, 11, 15, 15,  8,  8, 15,  8, 21, 15, 16,
    /* 0x70 */ 15, 16, 12, 12, 11, 15, 16, 22, 16, 16, 14,  9,  6, 10, 20, 20,
    /* 0x80 */ 20, 20, 20, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    /* 0x90 */ 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    /* 0xA0 */ 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    /* 0xB0 */ 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 20,
    /* 0xC0 */ 18, 18, 18, 18, 18, 18, 24, 13, 12, 12, 12, 13,  8,  8, 10, 11,
    /* 0xD0 */ 18, 16, 19, 19, 18, 18, 12, 18, 15, 15, 15, 15, 17, 14, 15, 14,
    /* 0xE0 */ 14, 14, 14, 14, 14, 23, 11, 14, 14, 14, 14,  7,  7, 10, 12, 12,
    /* 0xF0 */ 14, 14, 14, 14, 14, 14, 12, 14, 14, 14, 14, 14, 14, 15, 15, 12,
};

/* English pause-menu overlay string table (0x4744B8).
 * 6 entries: PAUSED(center), VIEW/MUSIC/SOUND(left), CONTINUE/EXIT(center). */
static const char *s_eng_pause_strings[] = {
    "PAUSED",         (const char *)(intptr_t)2,
    "VIEW",           (const char *)(intptr_t)0,
    "MUSIC",          (const char *)(intptr_t)0,
    "SOUND",          (const char *)(intptr_t)0,
    "CONTINUE",       (const char *)(intptr_t)2,
    "EXIT",           (const char *)(intptr_t)2,
    NULL
};
const char **g_pause_page_strings[8] = {
    s_eng_pause_strings, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
const int g_pause_page_sizes[8] = { 256, 0, 0, 0, 0, 0, 0, 0 };

/* ========================================================================
 * External game state references
 * ======================================================================== */

extern int     g_replay_mode;            /* td5_game.c */
extern int     g_wanted_mode_enabled;    /* td5_game.c */
extern int     g_special_encounter;      /* td5_game.c */
extern int     g_race_rule_variant;      /* td5_game.c */
extern int     g_game_type;              /* td5_game.c */
extern int     g_split_screen_mode;      /* td5_game.c */
extern int     g_racer_count;            /* td5_game.c */
extern float   g_render_width_f;         /* td5_render.c */
extern float   g_render_height_f;        /* td5_render.c */
extern int     g_render_width;           /* td5_render.c */
extern int     g_render_height;          /* td5_render.c */
extern int     g_track_is_circuit;       /* td5_track.c */
extern int     g_track_type_mode;        /* td5_track.c */
extern float   g_instant_fps;            /* td5_game.c */
extern uint32_t g_tick_counter;          /* td5_game.c */

extern int     g_actor_slot_map[2];      /* td5_game.c */
extern void   *g_actor_pool;             /* td5_game.c */

extern int     g_strip_span_count;       /* td5_track.c */
extern int     g_strip_total_segments;   /* td5_track.c */
extern void   *g_strip_span_base;        /* td5_track.c */
extern void   *g_strip_vertex_base;      /* td5_track.c */

extern uint16_t *g_checkpoint_array;     /* td5_track.c */

/* ========================================================================
 * Module-local state
 * ======================================================================== */

#define MAX_HUD_VIEWS       2
#define SPEEDO_QUAD_OFF     0x04       /* offset into view storage for speedo */
#define NEEDLE_QUAD_OFF     0x39C      /* needle quad offset */
#define GEAR_QUAD_OFF       0x0BC      /* gear indicator offset */
#define SPEEDFONT_BASE_OFF  0x174      /* speed font first digit offset */

/* Per-view HUD primitive storage (0x1148 bytes each) */
static uint8_t *s_hud_prim_storage;      /* 0x4B0C00 */
static uint32_t *s_hud_flags[MAX_HUD_VIEWS]; /* per-view visibility bitmask ptrs */

/* Active view pointers (set per iteration in render loop) */
static uint32_t *s_cur_flags;            /* 0x4B0BFC */
static float    *s_cur_scale;            /* 0x4B0FA4 */

/* Per-view layouts */
static TD5_HudViewLayout s_view_layout[MAX_HUD_VIEWS];

/* Global scale factors */
static float s_scale_x;                  /* 0x4B1138 */
static float s_scale_y;                  /* 0x4B113C */
static int   s_view_count;              /* 0x4B1134 */
static int   s_cur_view;               /* 0x4B11B0 */

/* Atlas entry pointers */
static TD5_AtlasEntry *s_numbers_atlas;  /* 0x4B0A3C */
static TD5_AtlasEntry *s_semicol_atlas;  /* 0x4B0FAC */
static TD5_AtlasEntry *s_speedofont_atlas; /* 0x4B11B4 */
static TD5_AtlasEntry *s_gearnumbers_atlas; /* 0x4B112C */
static TD5_AtlasEntry *s_fadewht_atlas;    /* 0x4B0BF0 */

/* Fade overlay quads (5 quads) */
static TD5_SpriteQuad s_fade_quads[5];   /* 0x4B0C08 */

/* Split-screen divider quads */
static TD5_SpriteQuad s_divider_quad_h;  /* 0x4B0FB0 */
static TD5_SpriteQuad s_divider_quad_v;  /* 0x4B1068 */

/* Radial pulse state */
static float s_radial_pulse_progress;    /* 0x4B0FA0 */

/* Screen-center flash counter for HUD */
static int   s_wrong_way_counter[MAX_HUD_VIEWS]; /* 0x4B0A64 */
static int   s_prev_span_pos[MAX_HUD_VIEWS];     /* 0x4B1120 */
static int   s_indicator_state[MAX_HUD_VIEWS];    /* 0x4B11A8 */

/* String table pointer and "is player 1" flag */
static const char **s_hud_string_table;  /* 0x4B0BF8 */
static int   s_is_first_player;          /* 0x4B0BF4 */

/* Metric display state */
static uint32_t s_metric_value;          /* 0x4B11C8 */

/* Minimap state */
static TD5_MinimapState s_minimap;
static uint8_t *s_minimap_quad_buf;      /* 0x4B0A6C */
static float s_minimap_x;               /* 0x4B0A40 */
static float s_minimap_y;               /* 0x4B0A44 */
static float s_minimap_width;           /* 0x4B1130 */
static float s_minimap_height;          /* 0x4B11B8 */
static float s_minimap_dot_size;        /* 0x4B1128 */
static float s_minimap_world_scale_x;   /* 0x4B0A48 */
static float s_minimap_world_scale_y;   /* 0x4B0A4C */
static float s_minimap_tile_width;      /* 0x4B0A50 */
static float s_minimap_tile_height;     /* 0x4B0A54 */

/* Minimap scandots sprite info (stored at init, used per-frame for dot draws) */
static int   s_minimap_scandots_tex_page;  /* texture_page for scandots sprite */
static float s_minimap_dot_atlas_u;        /* scandots atlas_x + 0.5 */
static float s_minimap_dot_atlas_v;        /* scandots atlas_y + 0.5 */
static float s_minimap_dot_atlas_vstride;  /* row stride in pixels (7.0f) */

/* Minimap route segment tables */
static int   s_minimap_seg_primary_end;  /* 0x4B0A58 */
static int   s_minimap_seg_branch_start; /* 0x4B0A5C */
static int16_t s_minimap_seg_start[64];  /* 0x4B0A70 */
static int16_t s_minimap_seg_end[64];    /* 0x4B0A72 */
static int16_t s_minimap_seg_branch[64]; /* 0x4B0A74 */

/* --- Text rendering state --- */
static TD5_GlyphRecord *s_glyph_table;  /* 0x4A2CB8: heap, 64 entries + tex ptr */
static uint8_t *s_text_quad_buf;         /* 0x4A2CBC: heap, 0xB800 bytes */
static int   s_queued_glyph_count;       /* 0x4A2CC0 */

/* Pause menu state */
static const char **s_pause_menu_strings; /* 0x4BCB74 */
static float s_pause_half_width;          /* 0x4B11D4 */
static int   s_pause_quad_count;          /* 0x4B1368 */
static TD5_AtlasEntry *s_pause_slider_atlas; /* 0x4B1358 */
static uint8_t *s_pause_sel_box;          /* 0x4BCB70 */
static void *s_pause_slider_ptrs[3];      /* 0x4B11D8..0x4B11E4 */
/* Persistent storage for pre-built pause menu quads (submitted each frame) */
static uint8_t s_pause_quad_buf[TD5_HUD_PAUSE_MAX_QUADS * 0xB8];
/* Cached selbox atlas entry and coordinates for dynamic update */
static TD5_AtlasEntry *s_pause_selbox_atlas;   /* cached during init for dynamic update */
static float s_pause_selbox_x0;                /* left edge of selbox, relative to cx */
static float s_pause_selbox_x1;                /* right edge of selbox, relative to cx */
static float s_pause_selbox_base_y;            /* y-center of row 0 (= -52.0f) */
static float s_pause_bar_x0 = 10.0f;
static float s_pause_bar_x1;  /* = s_pause_half_width - 4.0f, set during init */

/* Pause overlay dimmer state.
 * Must NOT be 898 (TD5_SHARED_FONT_PAGE) — that would clobber BodyText.tga
 * and cause white boxes on all frontend text after returning from a race. */
#define HUD_WHITE_TEX_PAGE 899
static int s_hud_white_tex_uploaded;

static void hud_log_atlas_status(const char *name, const TD5_AtlasEntry *entry)
{
    TD5_LOG_I(LOG_TAG, "atlas %s: %s", name, entry ? "found" : "missing");
}

/* ASCII -> glyph index remap table (128 bytes) */
static const uint8_t s_char_remap[128] = {
    /* 0x00-0x0F: control chars -> space (0x1F) */
    0x1F,0x1F,0x1F,0x1F, 0x1F,0x1F,0x1F,0x1F,
    0x1F,0x1F,0x1F,0x1F, 0x1F,0x1F,0x1F,0x1F,
    /* 0x10-0x1F */
    0x1F,0x1F,0x1F,0x1F, 0x1F,0x1F,0x1F,0x1F,
    0x1F,0x1F,0x1F,0x1F, 0x1F,0x1F,0x1F,0x1F,
    /* 0x20 (space) -> 0x1F */
    0x1F,
    /* 0x21-0x39: !"#..9 -> 0x20-0x39 */
    0x20,0x21,0x22,0x23, 0x24,0x25,0x26,0x27,
    0x28,0x29,0x2A,0x2B, 0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33, 0x34,0x35,0x36,0x37,
    /* 0x3A-0x3F */
    0x38,0x39,0x1F,0x1F, 0x1F,0x1F,0x1F,
    /* 0x40 '@' -> space */
    0x1F,
    /* 0x41-0x5A: A-Z -> 0x00-0x19 */
    0x00,0x01,0x02,0x03, 0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B, 0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13, 0x14,0x15,0x16,0x17,
    0x18,0x19,
    /* 0x5B-0x60: [\]^_` -> space */
    0x1F,0x1F,0x1F,0x1F, 0x1F,0x1F,
    /* 0x61-0x7A: a-z -> 0x00-0x19 (same as uppercase) */
    0x00,0x01,0x02,0x03, 0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B, 0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13, 0x14,0x15,0x16,0x17,
    0x18,0x19,
    /* 0x7B-0x7F */
    0x1F,0x1F,0x1F,0x1F, 0x1F
};

/* Pause menu glyph width table (per-character, loaded from original binary) */
extern const int8_t g_pause_glyph_widths[256]; /* 0x4660C8 */

/* ========================================================================
 * Helper: Build a sprite quad parameter block
 *
 * The original BuildSpriteQuadTemplate (0x432BD0) takes a packed struct
 * with destination pointer, mode flags, screen coords, UV coords, vertex
 * colors, and texture page. This helper wraps the interface.
 * ======================================================================== */

typedef struct TD5_QuadParams {
    void     *dest;
    int       mode_flags;        /* 0=position only, 1=position+color, 2=UV only */
    float     x0, x1, x2, x3;   /* screen x for 4 verts */
    float     y0, y1, y2, y3;   /* screen y for 4 verts */
    float     depth0, depth1, depth2, depth3; /* z-depth (usually 128.1) */
    float     u0, u1, u2, u3;   /* texture U */
    float     v0, v1, v2, v3;   /* texture V */
    uint32_t  color0, color1, color2, color3; /* vertex diffuse */
    int       texture_page;
    int       reserved;
} TD5_QuadParams;

static void hud_build_quad(void *dest, int mode, int tex_page,
                            float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            uint32_t color, float depth)
{
    /* Pack into the layout expected by td5_render_build_sprite_quad.
     * The original function reads a contiguous block starting from a
     * pointer to the first field (dest, mode, then positions, UVs,
     * colors, texture page). */
    struct {
        void     *dest;
        int       mode;
        float     scr_x[4]; /* TL, BL, TR, BR */
        float     scr_y[4];
        float     depth_z[4];
        float     tex_u[4];
        float     tex_v[4];
        uint32_t  diffuse[4];
        int       texture_page;
        int       pad;
    } p;

    p.dest = dest;
    p.mode = mode;

    p.scr_x[0] = x0; p.scr_x[1] = x0; p.scr_x[2] = x1; p.scr_x[3] = x1;
    p.scr_y[0] = y0; p.scr_y[1] = y1; p.scr_y[2] = y1; p.scr_y[3] = y0;

    p.depth_z[0] = depth; p.depth_z[1] = depth;
    p.depth_z[2] = depth; p.depth_z[3] = depth;

    /* Normalize pixel atlas UVs to [0,1] for D3D11 sampler.
     * Original engine uses 1/256 for both axes (BuildSpriteQuadTemplate
     * @ 0x432BD0, constant [0x4749D0] = 0.00390625 = 1/256).
     * Query actual texture dimensions so hi-res replacement pages work. */
    {
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims(tex_page, &tw, &th);
        u0 /= (float)tw; v0 /= (float)th;
        u1 /= (float)tw; v1 /= (float)th;
    }

    p.tex_u[0] = u0; p.tex_u[1] = u0; p.tex_u[2] = u1; p.tex_u[3] = u1;
    p.tex_v[0] = v0; p.tex_v[1] = v1; p.tex_v[2] = v1; p.tex_v[3] = v0;

    p.diffuse[0] = color; p.diffuse[1] = color;
    p.diffuse[2] = color; p.diffuse[3] = color;

    p.texture_page = tex_page;
    p.pad = 0;

    td5_render_build_sprite_quad((int *)&p);
}

/* Helper: submit a pre-built sprite quad */
static void hud_submit_quad(void *quad_data)
{
    td5_render_submit_translucent((uint16_t *)quad_data);
}

/* Helper: build a warped 4-corner quad.
 * Corners are provided in order: vert0 (front-left), vert1 (front-right),
 * vert2 (back-left), vert3 (back-right). Matches the original minimap road
 * quad layout from RenderTrackMinimapOverlay @ 0x0043a220. */
static void hud_build_quad_warped(void *dest, int tex_page,
                                   float x0, float y0,
                                   float x1, float y1,
                                   float x2, float y2,
                                   float x3, float y3,
                                   float u0, float v0, float u1, float v1,
                                   uint32_t color, float depth)
{
    struct {
        void     *dest;
        int       mode;
        float     scr_x[4];
        float     scr_y[4];
        float     depth_z[4];
        float     tex_u[4];
        float     tex_v[4];
        uint32_t  diffuse[4];
        int       texture_page;
        int       pad;
    } p;

    p.dest = dest;
    p.mode = 0;

    /* 4 independent corner positions (warped quad) */
    p.scr_x[0] = x0; p.scr_y[0] = y0;
    p.scr_x[1] = x1; p.scr_y[1] = y1;
    p.scr_x[2] = x2; p.scr_y[2] = y2;
    p.scr_x[3] = x3; p.scr_y[3] = y3;

    p.depth_z[0] = depth; p.depth_z[1] = depth;
    p.depth_z[2] = depth; p.depth_z[3] = depth;

    /* Normalize pixel atlas UVs for D3D11 sampler. */
    {
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims(tex_page, &tw, &th);
        u0 /= (float)tw; v0 /= (float)th;
        u1 /= (float)tw; v1 /= (float)th;
    }

    /* Map UVs across the 4 corners (v0 front-L, v1 front-R, v2 back-L, v3 back-R):
     * Front of quad uses v0; back uses v1. Left uses u0; right uses u1. */
    p.tex_u[0] = u0; p.tex_v[0] = v0;
    p.tex_u[1] = u1; p.tex_v[1] = v0;
    p.tex_u[2] = u0; p.tex_v[2] = v1;
    p.tex_u[3] = u1; p.tex_v[3] = v1;

    p.diffuse[0] = color; p.diffuse[1] = color;
    p.diffuse[2] = color; p.diffuse[3] = color;

    p.texture_page = tex_page;
    p.pad = 0;

    td5_render_build_sprite_quad((int *)&p);
}

/* Minimap span-type → right-edge vertex delta table.
 * From DAT_00473fd8 @ TD5_d3d.exe — i32 values indexed by span[0] type byte.
 * Only types 0..15 are valid; table entries beyond that spill into the
 * anti-piracy string pool in the original and are never reached in practice.
 * [CONFIRMED via memory_read @ 0x00473fd8] */
static const int32_t s_minimap_vtx_delta_a[16] = {
    0,  0, -1, -1, -2,  0, -1, -1,
   -2,  0,  0,  0,  0,  0,  0,  0,
};
/* DAT_00473fdc — same table + 4 bytes; second dword of each row.
 * All zeros for valid types 0..15. Used by span-b (+0x06) right-edge path. */
static const int32_t s_minimap_vtx_delta_b[16] = {
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
};

/* Depth constant for HUD overlay quads.
 * Must be in [0,1] range for D3D11: the vertex shader applies saturate()
 * to the Z component, so values outside [0,1] get clamped to 1.0 (far
 * plane) and fail the LESS_EQUAL depth test against scene geometry.
 * Use values near 0 so HUD always passes depth test. */
#define HUD_DEPTH  0.0f
#define HUD_DEPTH2 0.0f
#define HUD_DEPTH3 0.0f
#define HUD_DEPTH4 0.0f

/* ========================================================================
 * Actor field accessors
 *
 * These read fields from the 0x388-byte actor structs. The offsets come
 * from the Ghidra decompilation of RenderRaceHudOverlays and friends.
 * ======================================================================== */

static inline void *actor_ptr(int slot)
{
    if (!g_actor_pool) {
        static uint8_t s_null_actor[TD5_ACTOR_STRIDE] = {0};
        return s_null_actor;
    }
    return (uint8_t *)g_actor_pool + slot * TD5_ACTOR_STRIDE;
}

static inline uint8_t actor_race_position(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return a[0x383]; /* race position byte */
}

static inline uint8_t actor_gear(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return a[0x36B]; /* current gear */
}

/* engine_speed_accum: smoothed RPM counter, drives the tachometer needle.
 * Original formula: (engine_speed * 0xA5A) / max_rpm + 0x400 */
static inline int32_t actor_engine_speed(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int32_t *)(a + 0x310); /* engine_speed_accum */
}

/* max_rpm from carparam+0x72: raw integer engine redline (e.g. 6000).
 * Used as the divisor in the needle formula — no fixed-point shift. */
static inline int16_t actor_max_rpm(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    void *tuning = *(void **)(a + 0x1BC);
    if (!tuning) return 6000;
    return *(int16_t *)((uint8_t *)tuning + 0x72);
}

static inline int16_t actor_span_index(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int16_t *)(a + 0x82);
}

static inline int32_t actor_world_x(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int32_t *)(a + 0x1FC);
}

static inline int32_t actor_world_z(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int32_t *)(a + 0x204);
}

static inline int32_t actor_heading(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int32_t *)(a + 0x1F4);
}

static inline uint16_t actor_lap_time(int slot, int lap_index)
{
    return (uint16_t)td5_game_get_race_timer(slot, lap_index);
}

static inline uint8_t actor_route_index(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return a[0x375];
}

/* ========================================================================
 * InitializeRaceHudFontGlyphAtlas (0x428240)
 *
 * Allocates the text quad buffer and glyph UV/size table. Loads the
 * "font" atlas entry and builds a 4x16 glyph grid.
 * ======================================================================== */

void td5_hud_init_font_atlas(void)
{
    s_text_quad_buf = (uint8_t *)td5_game_heap_alloc(TD5_HUD_TEXT_BUF_SIZE);
    s_glyph_table = (TD5_GlyphRecord *)td5_game_heap_alloc(TD5_HUD_GLYPH_TABLE_SIZE);

    TD5_AtlasEntry *font_entry = td5_asset_find_atlas_entry(NULL, "font");

    /* Store texture page pointer at table[0x100] (the +0x400 byte offset).
     * In the original this is: glyph_table_ptr[0x100] = texture_page_ptr.
     * We store the atlas entry pointer for the texture page. */
    ((void **)s_glyph_table)[0x100] = (void *)(intptr_t)font_entry->texture_page;

    /* Build 4x16 glyph grid */
    float base_u = (float)font_entry->atlas_x;
    float base_v = (float)font_entry->atlas_y;

    for (int row = 0; row < TD5_HUD_FONT_GRID_ROWS; row++) {
        for (int col = 0; col < TD5_HUD_FONT_GRID_COLS; col++) {
            int idx = row * TD5_HUD_FONT_GRID_COLS + col;
            s_glyph_table[idx].atlas_u = base_u + 1.5f + (float)col * 10.0f;
            s_glyph_table[idx].atlas_v = base_v + 2.5f + (float)row * 16.0f;
            s_glyph_table[idx].width   = 8.0f;
            s_glyph_table[idx].height  = 12.0f;
        }
    }

    /* Width overrides for narrow glyphs */
    /* Index 0x22 = glyph '"' -> 4px; 0xB6 and 0xE6 also 4px in extended table.
     * Since we only have 64 entries, these are at offset 0x22*4 = glyph 0x22. */
    if (0x22 < TD5_HUD_FONT_GLYPH_COUNT * 4) {
        /* Glyph table stores 4 floats per entry. The "width" overrides
         * reference into the raw float array at element index * 4 + 2. */
        float *raw = (float *)s_glyph_table;
        raw[0x22 * 4 + 2] = 4.0f;  /* glyph 0x22: narrow */
        raw[0x26 * 4 + 2] = 7.0f;  /* glyph 0x26: slightly narrow */
    }

    s_queued_glyph_count = 0;

    /* Generate synthetic font texture for page 705 using GDI.
     * Only runs when tpage5.dat is absent.  When the real runtime dump
     * is available (captured via dump_tpages or ASI mod), it contains
     * SPEEDOFONT, GEARNUMBERS, FONT, etc. and should be used as-is. */
    if (font_entry->texture_page > 0 &&
        !td5_asset_static_tpage_is_real((int)(font_entry->texture_page - 700))) {
        /* 256x256 BGRA = 256 KB; static to avoid stack overflow.
         * Atlas pages are 256x256 (confirmed by UV scale = 1/256 for both axes).
         * Pre-fill with tpage5.dat so NUMBERS/GEARNUMBERS/etc. art is preserved;
         * the GDI synthesis only overwrites the FONT columns (x >= atlas_x). */
        static uint8_t s_font_page_buf[256 * 256 * 4];
        {
            int tpage_slot = (int)(font_entry->texture_page - 700);
            int loaded = 0;

            /* Try PNG from re/assets first */
            {
                char png_path[128];
                void *png_pixels = NULL;
                int pw = 0, ph = 0;
                snprintf(png_path, sizeof(png_path),
                         "re/assets/static/tpage%d.png", tpage_slot);
                if (td5_asset_decode_png_rgba32(png_path, &png_pixels, &pw, &ph)
                    && pw == 256 && ph == 256) {
                    memcpy(s_font_page_buf, png_pixels, 256 * 256 * 4);
                    free(png_pixels);
                    loaded = 1;
                } else if (png_pixels) {
                    free(png_pixels);
                }
            }

            /* Fallback: raw .dat BGRA */
            if (!loaded) {
                char tpage_path[128];
                snprintf(tpage_path, sizeof(tpage_path),
                         "re/assets/static/tpage%d.dat", tpage_slot);
                FILE *tf = fopen(tpage_path, "rb");
                if (tf) {
                    fread(s_font_page_buf, 1, sizeof(s_font_page_buf), tf);
                    fclose(tf);
                } else {
                    memset(s_font_page_buf, 0, sizeof(s_font_page_buf));
                }
            }
        }

        /* Character rendered for each glyph index (4 rows x 16 cols = 64):
         *   row 0 (0x00-0x0F): A-P
         *   row 1 (0x10-0x1F): Q-Z then space (0x1A-0x1F unmapped)
         *   row 2 (0x20-0x2F): !"#$%&'()*+,-./0
         *   row 3 (0x30-0x3F): 1-9:; then space (0x3C-0x3F unmapped) */
        static const char k_glyph_chars[64] = {
            'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
            'Q','R','S','T','U','V','W','X','Y','Z',' ',' ',' ',' ',' ',' ',
            '!','"','#','$','%','&','\'','(',')','*','+',',','-','.','/','0',
            '1','2','3','4','5','6','7','8','9',':',' ',' ',' ',' ',' ',' '
        };

        /* The font region occupies rows atlas_y..atlas_y+63 in the full page.
         * We render into a 256x64 DIB (top-down) and blit into s_font_page_buf. */
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = 256;
        bmi.bmiHeader.biHeight      = -64; /* negative = top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void *dib_bits = NULL;
        HDC hdc_mem = CreateCompatibleDC(NULL);
        HBITMAP hbmp = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS,
                                        &dib_bits, NULL, 0);

        if (hbmp && dib_bits) {
            HBITMAP old_bmp = (HBITMAP)SelectObject(hdc_mem, hbmp);
            HFONT hfont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
            HFONT old_font = (HFONT)SelectObject(hdc_mem, hfont);

            RECT fill_rc = {0, 0, 256, 64};
            FillRect(hdc_mem, &fill_rc,
                     (HBRUSH)GetStockObject(BLACK_BRUSH));

            SetBkMode(hdc_mem, OPAQUE);
            SetBkColor(hdc_mem, RGB(0, 0, 0));
            SetTextColor(hdc_mem, RGB(255, 255, 255));

            int ax = font_entry->atlas_x; /* 96 */
            /* atlas_y offset is applied when copying into s_font_page_buf */

            for (int gi = 0; gi < TD5_HUD_FONT_GLYPH_COUNT; gi++) {
                char c = k_glyph_chars[gi];
                if (c == ' ') continue;
                int col = gi % TD5_HUD_FONT_GRID_COLS;
                int row = gi / TD5_HUD_FONT_GRID_COLS;
                /* Pixel position in the DIB: column stride=10, row stride=16.
                 * +1/+2 match the 1.5/2.5 sub-pixel offsets in the glyph table. */
                int px = ax + col * 10 + 1;
                int py = row * 16 + 2;
                TextOutA(hdc_mem, px, py, &c, 1);
            }

            GdiFlush();

            /* Convert DIB pixels (BGRX) to BGRA with luminance alpha.
             * White GDI text -> full alpha; black background -> transparent.
             * Only overwrite columns >= atlas_x so NUMBERS/GEARNUMBERS
             * data from tpage.dat (at x=0..95) is preserved. */
            const uint8_t *src = (const uint8_t *)dib_bits;
            int ay = font_entry->atlas_y; /* 192 */
            int font_x_start = font_entry->atlas_x; /* 96 */
            for (int y = 0; y < 64; y++) {
                for (int x = font_x_start; x < 256; x++) {
                    int si = (y * 256 + x) * 4;
                    uint8_t b = src[si + 0];
                    uint8_t g_ch = src[si + 1];
                    uint8_t r = src[si + 2];
                    uint8_t alpha = (uint8_t)((r * 77u + g_ch * 150u + b * 29u) >> 8);
                    int di = ((ay + y) * 256 + x) * 4;
                    s_font_page_buf[di + 0] = 0xFF; /* B */
                    s_font_page_buf[di + 1] = 0xFF; /* G */
                    s_font_page_buf[di + 2] = 0xFF; /* R */
                    s_font_page_buf[di + 3] = alpha; /* A */
                }
            }

            SelectObject(hdc_mem, old_font);
            SelectObject(hdc_mem, old_bmp);
            DeleteObject(hbmp);
        }
        DeleteDC(hdc_mem);

        /* SPEEDOFONT: digits 0-9 at atlas (96,160) 160×32 — 16px wide × 32px tall each.
         * Speed digit UVs are selected as: u = atlas_x + digit * 16. */
        {
            TD5_AtlasEntry *sf_entry = td5_asset_find_atlas_entry(NULL, "SPEEDOFONT");
            if (sf_entry && sf_entry->width > 0) {
                BITMAPINFO sf_bmi;
                memset(&sf_bmi, 0, sizeof(sf_bmi));
                sf_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                sf_bmi.bmiHeader.biWidth       = 160;
                sf_bmi.bmiHeader.biHeight      = -32;
                sf_bmi.bmiHeader.biPlanes      = 1;
                sf_bmi.bmiHeader.biBitCount    = 32;
                sf_bmi.bmiHeader.biCompression = BI_RGB;
                void *sf_bits = NULL;
                HDC   sf_hdc  = CreateCompatibleDC(NULL);
                HBITMAP sf_bmp = CreateDIBSection(sf_hdc, &sf_bmi, DIB_RGB_COLORS,
                                                  &sf_bits, NULL, 0);
                if (sf_bmp && sf_bits) {
                    HBITMAP sf_old = (HBITMAP)SelectObject(sf_hdc, sf_bmp);
                    RECT sf_rc = {0, 0, 160, 32};
                    FillRect(sf_hdc, &sf_rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    /* Bold proportional font scaled to fit 16×32 cells */
                    HFONT sf_font = CreateFontA(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        NONANTIALIASED_QUALITY, FIXED_PITCH | FF_DONTCARE, "Courier New");
                    if (!sf_font) sf_font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
                    HFONT sf_oldf = (HFONT)SelectObject(sf_hdc, sf_font);
                    SetBkMode(sf_hdc, TRANSPARENT);
                    SetTextColor(sf_hdc, RGB(255, 255, 255));
                    for (int d = 0; d < 10; d++) {
                        char ch[2] = {(char)('0' + d), '\0'};
                        TextOutA(sf_hdc, d * 16 + 2, 4, ch, 1);
                    }
                    GdiFlush();
                    SelectObject(sf_hdc, sf_oldf);
                    DeleteObject(sf_font);
                    const uint8_t *sf_src = (const uint8_t *)sf_bits;
                    int sf_ay = sf_entry->atlas_y;
                    int sf_ax = sf_entry->atlas_x;
                    for (int ry = 0; ry < 32; ry++) {
                        for (int rx2 = 0; rx2 < 160; rx2++) {
                            int si = (ry * 160 + rx2) * 4;
                            uint8_t b = sf_src[si], g = sf_src[si+1], r = sf_src[si+2];
                            uint8_t al = (uint8_t)((r*77u + g*150u + b*29u) >> 8);
                            int di = ((sf_ay + ry) * 256 + sf_ax + rx2) * 4;
                            s_font_page_buf[di+0] = 0xFF;
                            s_font_page_buf[di+1] = 0xFF;
                            s_font_page_buf[di+2] = 0xFF;
                            s_font_page_buf[di+3] = al;
                        }
                    }
                    SelectObject(sf_hdc, sf_old);
                    DeleteObject(sf_bmp);
                }
                DeleteDC(sf_hdc);
            }
        }

        /* GEARNUMBERS: labels N,1-5,R,_ at atlas (128,128) 128×16 — 16px per slot.
         * Gear UV: u = atlas_x + gear_index * 16. */
        {
            TD5_AtlasEntry *gn_entry = td5_asset_find_atlas_entry(NULL, "GEARNUMBERS");
            if (gn_entry && gn_entry->width > 0) {
                static const char k_gear_chars[8] = {'N','1','2','3','4','5','R',' '};
                BITMAPINFO gn_bmi;
                memset(&gn_bmi, 0, sizeof(gn_bmi));
                gn_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                gn_bmi.bmiHeader.biWidth       = 128;
                gn_bmi.bmiHeader.biHeight      = -16;
                gn_bmi.bmiHeader.biPlanes      = 1;
                gn_bmi.bmiHeader.biBitCount    = 32;
                gn_bmi.bmiHeader.biCompression = BI_RGB;
                void *gn_bits = NULL;
                HDC   gn_hdc  = CreateCompatibleDC(NULL);
                HBITMAP gn_bmp = CreateDIBSection(gn_hdc, &gn_bmi, DIB_RGB_COLORS,
                                                  &gn_bits, NULL, 0);
                if (gn_bmp && gn_bits) {
                    HBITMAP gn_old = (HBITMAP)SelectObject(gn_hdc, gn_bmp);
                    RECT gn_rc = {0, 0, 128, 16};
                    FillRect(gn_hdc, &gn_rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    HFONT gn_font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
                    HFONT gn_oldf = (HFONT)SelectObject(gn_hdc, gn_font);
                    SetBkMode(gn_hdc, TRANSPARENT);
                    SetTextColor(gn_hdc, RGB(255, 255, 255));
                    for (int gi = 0; gi < 8; gi++) {
                        if (k_gear_chars[gi] == ' ') continue;
                        char ch[2] = {k_gear_chars[gi], '\0'};
                        TextOutA(gn_hdc, gi * 16 + 4, 2, ch, 1);
                    }
                    GdiFlush();
                    SelectObject(gn_hdc, gn_oldf);
                    const uint8_t *gn_src = (const uint8_t *)gn_bits;
                    int gn_ay = gn_entry->atlas_y;
                    int gn_ax = gn_entry->atlas_x;
                    for (int gy = 0; gy < 16; gy++) {
                        for (int gx = 0; gx < 128; gx++) {
                            int si = (gy * 128 + gx) * 4;
                            uint8_t b = gn_src[si], g = gn_src[si+1], r = gn_src[si+2];
                            uint8_t al = (uint8_t)((r*77u + g*150u + b*29u) >> 8);
                            int di = ((gn_ay + gy) * 256 + gn_ax + gx) * 4;
                            s_font_page_buf[di+0] = 0xFF;
                            s_font_page_buf[di+1] = 0xFF;
                            s_font_page_buf[di+2] = 0xFF;
                            s_font_page_buf[di+3] = al;
                        }
                    }
                    SelectObject(gn_hdc, gn_old);
                    DeleteObject(gn_bmp);
                }
                DeleteDC(gn_hdc);
            }
        }

        td5_plat_render_upload_texture(font_entry->texture_page,
                                       s_font_page_buf, 256, 256, 2);
    }

    /* Generate synthetic speedometer dial for page 704 using GDI.
     * tpage4.dat is assembled at runtime by the original engine
     * (UploadRaceTexturePage @ 0x40B590) and has no on-disk .dat file.
     * We draw a simple circular gauge (96×96) into the 256×256 BGRA32 page
     * at the SPEEDO atlas offset (atlas_x=0, atlas_y=0).
     *
     * Angle convention (screen coords, Y down): θ=0 → right, increases CW.
     * Scale: 0-120 mph, 240° sweep.  Start: 150° (8 o'clock), mid: 270°
     * (12 o'clock) at 60 mph, end: 390°=30° (2 o'clock) at 120 mph. */
    {
        TD5_AtlasEntry *speedo_entry = td5_asset_find_atlas_entry(NULL, "SPEEDO");
        if (speedo_entry && speedo_entry->texture_page > 0 &&
            !td5_asset_static_tpage_is_real((int)(speedo_entry->texture_page - 700))) {
            /* Only generate synthetic speedo dial when tpage4.dat is missing.
             * When the real dump texture exists, use it as-is. */
            static uint8_t s_speedo_page_buf[256 * 256 * 4];
            memset(s_speedo_page_buf, 0, sizeof(s_speedo_page_buf));

            BITMAPINFO sp_bmi;
            memset(&sp_bmi, 0, sizeof(sp_bmi));
            sp_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            sp_bmi.bmiHeader.biWidth       = 96;
            sp_bmi.bmiHeader.biHeight      = -96; /* top-down */
            sp_bmi.bmiHeader.biPlanes      = 1;
            sp_bmi.bmiHeader.biBitCount    = 32;
            sp_bmi.bmiHeader.biCompression = BI_RGB;

            void *sp_dib = NULL;
            HDC   sp_hdc = CreateCompatibleDC(NULL);
            HBITMAP sp_bmp = CreateDIBSection(sp_hdc, &sp_bmi, DIB_RGB_COLORS,
                                              &sp_dib, NULL, 0);
            if (sp_bmp && sp_dib) {
                HBITMAP sp_old = (HBITMAP)SelectObject(sp_hdc, sp_bmp);

                /* Fill background with magenta key → becomes alpha=0 */
                HBRUSH sp_key = CreateSolidBrush(RGB(255, 0, 255));
                RECT sp_rc = {0, 0, 96, 96};
                FillRect(sp_hdc, &sp_rc, sp_key);
                DeleteObject(sp_key);

                /* Filled dark circle: dial face */
                HBRUSH sp_face  = CreateSolidBrush(RGB(25, 25, 30));
                HPEN   sp_null  = (HPEN)GetStockObject(NULL_PEN);
                HPEN   sp_opold = (HPEN)SelectObject(sp_hdc, sp_null);
                HBRUSH sp_obold = (HBRUSH)SelectObject(sp_hdc, sp_face);
                Ellipse(sp_hdc, 1, 1, 95, 95);

                /* White outer ring */
                HPEN sp_ring = CreatePen(PS_SOLID, 2, RGB(200, 200, 210));
                SelectObject(sp_hdc, sp_ring);
                SelectObject(sp_hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
                Ellipse(sp_hdc, 2, 2, 94, 94);

                /* Tick marks: 0,10,20,...,120 mph (13 positions) */
                HPEN sp_maj = CreatePen(PS_SOLID, 2, RGB(220, 220, 220));
                HPEN sp_min = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
                int sp_cx = 48, sp_cy = 48;
                for (int ti = 0; ti <= 12; ti++) {
                    double speed_v   = ti * 10.0;
                    double angle_deg = 150.0 + (speed_v / 120.0) * 240.0;
                    double angle_rad = angle_deg * 3.14159265358979 / 180.0;
                    double ca = cos(angle_rad);
                    double sa = sin(angle_rad);
                    int is_major = (ti % 2 == 0); /* every 20 mph */
                    int r_in  = is_major ? 35 : 40;
                    int r_out = 43;
                    SelectObject(sp_hdc, is_major ? sp_maj : sp_min);
                    MoveToEx(sp_hdc,
                             sp_cx + (int)(r_out * ca),
                             sp_cy + (int)(r_out * sa), NULL);
                    LineTo(sp_hdc,
                           sp_cx + (int)(r_in  * ca),
                           sp_cy + (int)(r_in  * sa));
                }

                /* Center pivot dot */
                HBRUSH sp_piv = CreateSolidBrush(RGB(180, 180, 180));
                SelectObject(sp_hdc, sp_null);
                SelectObject(sp_hdc, sp_piv);
                Ellipse(sp_hdc, sp_cx - 4, sp_cy - 4, sp_cx + 5, sp_cy + 5);

                GdiFlush();
                SelectObject(sp_hdc, sp_opold);
                SelectObject(sp_hdc, sp_obold);
                DeleteObject(sp_face);
                DeleteObject(sp_ring);
                DeleteObject(sp_maj);
                DeleteObject(sp_min);
                DeleteObject(sp_piv);

                /* Convert 96×96 BGRX → BGRA32 in s_speedo_page_buf at (0,0).
                 * Magenta key (R=255,G=0,B=255) → alpha=0 (transparent). */
                const uint8_t *sp_src = (const uint8_t *)sp_dib;
                for (int ry = 0; ry < 96; ry++) {
                    for (int rx = 0; rx < 96; rx++) {
                        int si = (ry * 96 + rx) * 4;
                        uint8_t pb = sp_src[si + 0];
                        uint8_t pg = sp_src[si + 1];
                        uint8_t pr = sp_src[si + 2];
                        uint8_t pa = (pr == 255 && pg == 0 && pb == 255) ? 0 : 255;
                        int di = (ry * 256 + rx) * 4;
                        s_speedo_page_buf[di + 0] = pb;
                        s_speedo_page_buf[di + 1] = pg;
                        s_speedo_page_buf[di + 2] = pr;
                        s_speedo_page_buf[di + 3] = pa;
                    }
                }

                SelectObject(sp_hdc, sp_old);
                DeleteObject(sp_bmp);
            }
            DeleteDC(sp_hdc);

            td5_plat_render_upload_texture(speedo_entry->texture_page,
                                           s_speedo_page_buf, 256, 256, 2);
        }
    }

    /* Upload 1x1 white texture for solid-color overlays (pause dimmer) */
    if (!s_hud_white_tex_uploaded) {
        static const uint32_t k_white = 0xFFFFFFFF;
        s_hud_white_tex_uploaded = td5_plat_render_upload_texture(
            HUD_WHITE_TEX_PAGE, &k_white, 1, 1, 2);
    }
}

/* ========================================================================
 * Draw the pause menu overlay (BLACKBOX panel, selbox, sliders, text).
 * No full-screen dimmer — original uses only the BLACKBOX panel (0x43C1E2).
 * ======================================================================== */

void td5_hud_draw_pause_overlay(void)
{
    int i;

    /* No full-screen dimmer — original only uses the BLACKBOX panel (0x43C1E2).
     * Submit pre-built pause menu panel, selection, sliders, and text. */
    for (i = 0; i < s_pause_quad_count && i < TD5_HUD_PAUSE_MAX_QUADS; i++) {
        hud_submit_quad(s_pause_quad_buf + i * 0xB8);
    }
}

/* Called each frame while paused to update SELBOX position and slider thumb positions.
 * cursor: 0-4 (rows: SFX, Music, CD, Continue, Quit)
 * sfx_frac, music_frac, cd_frac: volume fractions [0.0, 1.0] */
void td5_hud_update_pause_overlay(int cursor, float sfx_frac, float music_frac, float cd_frac)
{
    float cx = g_render_width_f  * 0.5f;
    float cy = g_render_height_f * 0.5f;
    float fracs[3] = { sfx_frac, music_frac, cd_frac };

    /* Move SELBOX to cursor row using atlas texture */
    if (s_pause_sel_box && s_pause_selbox_atlas) {
        float row_y = s_pause_selbox_base_y + (float)cursor * 16.0f;
        float su0 = (float)s_pause_selbox_atlas->atlas_x + 0.5f;
        float sv0 = (float)s_pause_selbox_atlas->atlas_y + 0.5f;
        hud_build_quad(s_pause_sel_box, 0, s_pause_selbox_atlas->texture_page,
                       cx + s_pause_selbox_x0, cy + row_y,
                       cx + s_pause_selbox_x1, cy + row_y + 16.0f,
                       su0, sv0, su0 + 255.0f, sv0 + 15.0f,
                       0xFFFFFFFF, HUD_DEPTH);
    }

    /* Update slider fill bar using atlas texture.
     * Fill scale = 128.0f (0x0045D600), UV scale = frac*255.0 (0x0045D684). */
    for (int row = 0; row < 3; row++) {
        if (!s_pause_slider_ptrs[row]) continue;
        float frac = fracs[row];
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float row_y = (float)row * 16.0f;
        float fill_x1 = s_pause_bar_x0 + frac * 128.0f;
        int sl_page = s_pause_slider_atlas ? s_pause_slider_atlas->texture_page : HUD_WHITE_TEX_PAGE;
        float slu0 = s_pause_slider_atlas ? (float)s_pause_slider_atlas->atlas_x + 0.5f : 0.25f;
        float slu1 = s_pause_slider_atlas ? (float)s_pause_slider_atlas->atlas_x + 0.5f + frac * 255.0f : 0.25f;
        float slv  = s_pause_slider_atlas ? (float)s_pause_slider_atlas->atlas_y + 0.5f : 0.25f;
        hud_build_quad(s_pause_slider_ptrs[row], 0, sl_page,
                       cx + s_pause_bar_x0, cy + row_y - 28.0f,
                       cx + fill_x1,        cy + row_y - 22.0f,
                       slu0, slv, slu1, slv,
                       0xFFFFFFFF, HUD_DEPTH);
    }
}

/* ========================================================================
 * QueueRaceHudFormattedText (0x428320)
 *
 * Printf-style text queue. Characters are remapped through the glyph
 * table and built into sprite quads for batch rendering.
 * ======================================================================== */

void td5_hud_queue_text(int font_index, int x, int y, int centered,
                         const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int len = (int)strlen(buf);
    if (s_queued_glyph_count + len > TD5_HUD_MAX_TEXT_GLYPHS) {
        return; /* overflow guard */
    }

    /* Select glyph table for font index (only font 0 used in race HUD) */
    int font_offset = font_index * TD5_HUD_GLYPH_TABLE_SIZE;
    TD5_GlyphRecord *glyphs = (TD5_GlyphRecord *)((uint8_t *)s_glyph_table + font_offset);
    int tex_page = (int)(intptr_t)((void **)glyphs)[0x100];

    /* Remap characters through ASCII table */
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        buf[i] = (char)s_char_remap[c & 0x7F];
    }

    /* If centered, compute total width and offset X */
    float cursor_x = (float)x;
    float cursor_y = (float)y;

    if (centered) {
        float total_w = 0.0f;
        for (int i = 0; i < len; i++) {
            uint8_t gi = (uint8_t)buf[i];
            total_w += glyphs[gi].width;
        }
        /* Total advance = sum of widths + (len-1) spacing pixels */
        cursor_x -= ((float)(len - 1) + total_w) * 0.5f;
    }

    /* Build a sprite quad for each glyph */
    uint8_t *quad_ptr = s_text_quad_buf + s_queued_glyph_count * TD5_HUD_GLYPH_QUAD_SIZE;

    for (int i = 0; i < len; i++) {
        uint8_t gi = (uint8_t)buf[i];
        TD5_GlyphRecord *g = &glyphs[gi];

        hud_build_quad(
            quad_ptr,
            0,                          /* mode: standard */
            tex_page,
            cursor_x, cursor_y,                       /* top-left */
            cursor_x + g->width, cursor_y + g->height, /* bottom-right */
            g->atlas_u, g->atlas_v,                    /* UV top-left */
            g->atlas_u + g->width, g->atlas_v + g->height, /* UV bottom-right */
            0xFFFFFFFF,                 /* white diffuse */
            HUD_DEPTH
        );

        cursor_x += g->width + 1.0f;
        quad_ptr += TD5_HUD_GLYPH_QUAD_SIZE;
    }

    s_queued_glyph_count += len;
}

/* ========================================================================
 * FlushQueuedRaceHudText (0x428570)
 *
 * Iterates all queued glyph quads and submits them to the translucent
 * pipeline, then resets the queue.
 * ======================================================================== */

void td5_hud_flush_text(void)
{
    uint8_t *ptr = s_text_quad_buf;
    int count = s_queued_glyph_count;

    while (count > 0) {
        hud_submit_quad(ptr);
        ptr += TD5_HUD_GLYPH_QUAD_SIZE;
        count--;
    }

    s_queued_glyph_count = 0;
}

/* ========================================================================
 * InitializeRaceOverlayResources (0x4377B0)
 *
 * Allocates HUD primitive storage, sets visibility bitmask based on
 * race mode, and loads the fade/divider sprites.
 * ======================================================================== */

void td5_hud_init_overlay_resources(int race_mode, int string_table_offset)
{
    /* Set string table based on game mode offset */
    s_hud_string_table = g_position_strings + string_table_offset * 13;
    s_is_first_player = (string_table_offset == 0) ? 1 : 0;

    s_wrong_way_counter[0] = 0;
    s_wrong_way_counter[1] = 0;

    /* Allocate per-view HUD primitive storage */
    s_hud_prim_storage = (uint8_t *)td5_game_heap_alloc(TD5_HUD_VIEW_STRIDE);
    s_hud_flags[0] = (uint32_t *)s_hud_prim_storage;
    s_hud_flags[1] = (uint32_t *)(s_hud_prim_storage + 0x894); /* offset 0x229*4 */

    /* Set visibility bitmask based on race mode */
    if (race_mode == 0) {
        /* Attract/demo mode or replay-only */
        if (g_replay_mode == 0) {
            *s_hud_flags[0] = TD5_HUD_REPLAY_BANNER;
            *s_hud_flags[1] = TD5_HUD_REPLAY_BANNER;
        } else {
            *s_hud_flags[0] = 0;
            *s_hud_flags[1] = 0;
        }
    } else {
        /* Active race mode -- build bitmask */
        uint32_t flags = TD5_HUD_SPEEDOMETER
                       | TD5_HUD_UTURN_WARNING
                       | TD5_HUD_METRIC_DIGITS
                       | TD5_HUD_RESERVED_5
                       | TD5_HUD_RESERVED_3;

        /* Position label and lap timers for most modes */
        if (g_game_type == 7) {
            /* Time Trial (preset 3 @ 0x004377B0): no flag-driven timers.
             * The main TIME readout is emitted from td5_hud_draw_status_text
             * (DrawRaceStatusText @ 0x00439B70), not from the overlay bitmask. */
            TD5_LOG_I(LOG_TAG, "overlay flags: time-trial (game_type=7) baseline only");
        } else if (g_game_type == 2 || g_game_type == 0 || g_game_type == 1) {
            flags |= TD5_HUD_POSITION_LABEL | TD5_HUD_LAP_TIMERS;
        } else if (g_game_type == 5) {
            if (g_special_encounter == 0) {
                flags |= TD5_HUD_POSITION_LABEL | TD5_HUD_LAP_TIMERS;
            }
        } else {
            flags |= TD5_HUD_POSITION_LABEL | TD5_HUD_LAP_TIMERS;
        }

        /* Total timer for time-attack / checkpoint modes */
        if (g_game_type == 2) {
            if (g_race_rule_variant == 0 || g_race_rule_variant == 4) {
                flags |= TD5_HUD_TOTAL_TIMER;
            }
        }
        if (g_game_type == 4) {
            flags |= TD5_HUD_TOTAL_TIMER | TD5_HUD_LAP_COUNTER;
        }

        /* Circuit lap count for circuit tracks */
        if (g_track_is_circuit != 0 && g_game_type != 3) {
            flags |= TD5_HUD_CIRCUIT_LAPS;
        }

        *s_hud_flags[0] = flags;
        *s_hud_flags[1] = flags;
    }

    /* Load FADEWHT sprite for screen-fade overlays */
    s_fadewht_atlas = td5_asset_find_atlas_entry(NULL, "FADEWHT");
    hud_log_atlas_status("FADEWHT", s_fadewht_atlas);

    float fu0 = (float)s_fadewht_atlas->atlas_x + 0.5f;
    float fv0 = (float)s_fadewht_atlas->atlas_y + 0.5f;
    int   ftex = s_fadewht_atlas->texture_page;

    for (int i = 0; i < 5; i++) {
        hud_build_quad(
            &s_fade_quads[i],
            0, ftex,
            0.0f, 0.0f, 0.0f, 0.0f,  /* positions filled at render time */
            fu0, fv0, fu0, fv0,
            0xFFFFFFFF,
            HUD_DEPTH2
        );
    }

    /* Initialize radial pulse to inactive */
    s_radial_pulse_progress = -1.0f;

    /* Split-screen divider bars */
    if (g_split_screen_mode != 0) {
        TD5_AtlasEntry *colours = td5_asset_find_atlas_entry(NULL, "COLOURS");
        hud_log_atlas_status("COLOURS", colours);
        float cu0 = (float)colours->atlas_x + 0.5f;
        float cv0 = (float)colours->atlas_y + 0.5f;
        int   ctex = colours->texture_page;

        /* Horizontal divider (for left/right split) */
        hud_build_quad(
            &s_divider_quad_h,
            0, ctex,
            -1.0f, g_render_height_f * 0.5f,
            1.0f, g_render_height_f * 0.5f,
            cu0, cv0, cu0, cv0,
            0xFFFFFFFF, HUD_DEPTH2
        );

        /* Vertical divider (for top/bottom split) */
        hud_build_quad(
            &s_divider_quad_v,
            0, ctex,
            g_render_width_f * 0.5f, -1.0f,
            g_render_width_f * 0.5f, 1.0f,
            cu0, cv0, cu0, cv0,
            0xFFFFFFFF, HUD_DEPTH2
        );
    }
}

/* ========================================================================
 * InitializeRaceHudLayout (0x437BA0)
 *
 * Computes scale factors and per-view viewport bounds. Loads all sprite
 * atlas entries for HUD elements and builds their quad templates.
 * ======================================================================== */

void td5_hud_init_layout(int viewport_mode)
{
    /* Load numbers atlas (used by metric display) */
    s_numbers_atlas = td5_asset_find_atlas_entry(NULL, "numbers");
    hud_log_atlas_status("numbers", s_numbers_atlas);

    /* Compute base scale factors */
    s_scale_x = g_render_width_f * (1.0f / 640.0f);
    s_scale_y = g_render_height_f * (1.0f / 480.0f);

    /* Set up per-view layout in pixel-space coordinates.
     *
     * The original binary uses a centered coordinate system where origin is
     * the screen center (vp_left = -width/2, vp_right = +width/2).
     * BuildSpriteQuadTemplate @ 0x432BD0 adds screen_center to each vertex
     * before submitting to hardware.  Our source port submits vertices
     * directly to D3D11 without that centering step, so all HUD positions
     * must be in pixel-space (vp_left = 0, vp_right = width). */
    if (viewport_mode == 0) {
        /* Single full-screen view */
        s_view_count = 1;
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f;

    } else if (viewport_mode == 1) {
        /* Left/right split: each half-screen view */
        s_scale_x *= 0.5f;
        s_scale_y *= 0.5f;
        s_view_count = 2;
        /* View 0: left half */
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f * 0.5f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f;
        /* View 1: right half */
        s_view_layout[1].scale_x  = s_scale_x;
        s_view_layout[1].scale_y  = s_scale_y;
        s_view_layout[1].vp_left   = g_render_width_f * 0.5f;
        s_view_layout[1].vp_right  = g_render_width_f;
        s_view_layout[1].vp_top    = 0.0f;
        s_view_layout[1].vp_bottom = g_render_height_f;

    } else {
        /* Top/bottom split: each half-screen view */
        s_scale_x *= 0.5f;
        s_scale_y *= 0.5f;
        s_view_count = 2;
        /* View 0: top half */
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f * 0.5f;
        /* View 1: bottom half */
        s_view_layout[1].scale_x  = s_scale_x;
        s_view_layout[1].scale_y  = s_scale_y;
        s_view_layout[1].vp_left   = 0.0f;
        s_view_layout[1].vp_right  = g_render_width_f;
        s_view_layout[1].vp_top    = g_render_height_f * 0.5f;
        s_view_layout[1].vp_bottom = g_render_height_f;
    }

    /* Compute derived viewport values for each view.
     * center_x/center_y = viewport center in pixel coords.
     * half_width = center_x (used by text centering code as horizontal pivot).
     * vp_int_* = pixel-space edges (same as vp_* for our pipeline). */
    for (int v = 0; v < s_view_count; v++) {
        TD5_HudViewLayout *vl = &s_view_layout[v];
        vl->center_x    = (vl->vp_left  + vl->vp_right)  * 0.5f;
        vl->center_y    = (vl->vp_top   + vl->vp_bottom) * 0.5f;
        vl->half_width  = vl->center_x;   /* used as horizontal pivot for text */
        vl->half_height = vl->center_y;
        vl->vp_int_left   = vl->vp_left;
        vl->vp_int_top    = vl->vp_top;
        vl->vp_int_right  = vl->vp_right;
        vl->vp_int_bottom = vl->vp_bottom;
    }

    /* Load semicolon atlas */
    s_semicol_atlas = td5_asset_find_atlas_entry(NULL, "SEMICOL");
    hud_log_atlas_status("SEMICOL", s_semicol_atlas);

    /* Build per-view HUD element quads */
    s_cur_view = 0;
    for (int v = 0; v < s_view_count; v++) {
        s_cur_scale = (float *)&s_view_layout[v];
        s_cur_flags = s_hud_flags[v];

        float sx = s_view_layout[v].scale_x;
        float sy = s_view_layout[v].scale_y;
        float vp_r = s_view_layout[v].vp_int_right;
        float vp_b = s_view_layout[v].vp_int_bottom;
        float vp_l = s_view_layout[v].vp_int_left;
        float vp_t = s_view_layout[v].vp_int_top;

        /* --- Speedometer dial --- */
        TD5_AtlasEntry *speedo = td5_asset_find_atlas_entry(NULL, "SPEEDO");
        if (v == 0) {
            hud_log_atlas_status("SPEEDO", speedo);
        }
        float speedo_x = vp_r - sx * 96.0f - sx * 16.0f;
        float speedo_y = vp_b - sy * 96.0f - sy * 8.0f;

        uint8_t *view_base = s_hud_prim_storage; /* simplified: single allocation */
        hud_build_quad(
            view_base + SPEEDO_QUAD_OFF,
            0, speedo->texture_page,
            speedo_x, speedo_y,
            speedo_x + sx * 96.0f, speedo_y + sy * 96.0f,
            (float)speedo->atlas_x + 0.5f,
            (float)speedo->atlas_y + 0.5f,
            (float)(speedo->atlas_x + speedo->width) - 0.5f,
            (float)(speedo->atlas_y + speedo->height) - 0.5f,
            0xFFFFFFFF, HUD_DEPTH
        );

        /* --- Speed digit font (SPEEDOFONT) --- */
        s_speedofont_atlas = td5_asset_find_atlas_entry(NULL, "SPEEDOFONT");
        if (v == 0) {
            hud_log_atlas_status("SPEEDOFONT", s_speedofont_atlas);
        }

        float font_glyph_w = sx * 15.0f;
        float font_x_start = vp_r - sx * 60.0f;
        float font_y = vp_b - sy * 23.0f - sy * 8.0f;

        /* Build 3 digit quads (ones, tens, hundreds -- right to left) */
        for (int d = 0; d < 3; d++) {
            float dx = font_x_start + (float)d * (font_glyph_w + 1.0f);
            hud_build_quad(
                view_base + SPEEDFONT_BASE_OFF + d * TD5_HUD_GLYPH_QUAD_SIZE,
                0, s_speedofont_atlas->texture_page,
                dx, font_y,
                dx + font_glyph_w, font_y + sy * 24.0f,
                0.0f, 0.0f, 0.0f, 0.0f, /* UV set at render time per digit */
                0xFFFFFFFF, HUD_DEPTH
            );
        }

        /* --- Gear indicator (GEARNUMBERS) --- */
        s_gearnumbers_atlas = td5_asset_find_atlas_entry(NULL, "GEARNUMBERS");
        if (v == 0) {
            hud_log_atlas_status("GEARNUMBERS", s_gearnumbers_atlas);
        }

        float gear_x = vp_r - sx * 32.0f;
        float gear_y = vp_b - sy * 16.0f - sy * 56.0f;

        hud_build_quad(
            view_base + GEAR_QUAD_OFF,
            0, s_gearnumbers_atlas->texture_page,
            gear_x, gear_y,
            gear_x + sx * 16.0f, gear_y + sy * 16.0f,
            0.0f, 0.0f, 0.0f, 0.0f, /* UV set at render time */
            0xFFFFFFFF, HUD_DEPTH
        );

        /* --- Metric digit quads (numbers atlas, 4 digits) --- */
        float metric_glyph_w = sx * 16.0f;
        float metric_x = s_view_layout[v].center_x - metric_glyph_w * 2.5f;
        float metric_y = vp_t + sy * 12.0f;

        for (int d = 0; d < 4; d++) {
            float mdx = metric_x + (float)d * metric_glyph_w;
            /* These are built with UV-only mode since the atlas lookup is per-frame */
            hud_build_quad(
                view_base + 0x734 + d * TD5_HUD_GLYPH_QUAD_SIZE,
                0, s_numbers_atlas->texture_page,
                mdx, metric_y,
                mdx + metric_glyph_w, metric_y + sy * 24.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0xFFFFFFFF, HUD_DEPTH
            );
        }

        /* --- Semicolon quad for timer display --- */
        hud_build_quad(
            view_base + NEEDLE_QUAD_OFF,
            0, s_semicol_atlas->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            (float)(s_semicol_atlas->atlas_x + 5),
            (float)(s_semicol_atlas->atlas_y + 1),
            (float)(s_semicol_atlas->atlas_x + 5),
            (float)(s_semicol_atlas->atlas_y + 1),
            0xFFFFFFFF, HUD_DEPTH
        );

        /* --- U-turn warning icon (UTURN, centered) --- */
        TD5_AtlasEntry *uturn = td5_asset_find_atlas_entry(NULL, "UTURN");
        if (v == 0) {
            hud_log_atlas_status("UTURN", uturn);
        }
        float uturn_half_x = sx * 64.0f * 0.5f;
        float uturn_half_y = sy * 64.0f * 0.5f;
        float uturn_cx = s_view_layout[v].center_x;
        float uturn_cy = s_view_layout[v].center_y;

        hud_build_quad(
            view_base + 0x67C,
            0, uturn->texture_page,
            uturn_cx - uturn_half_x, uturn_cy - uturn_half_y,
            uturn_cx + uturn_half_x, uturn_cy + uturn_half_y,
            (float)uturn->atlas_x + 0.5f,
            (float)uturn->atlas_y + 0.5f,
            (float)(uturn->atlas_x + uturn->width) - 0.5f,
            (float)(uturn->atlas_y + uturn->height) - 0.5f,
            0xFFFFFFFF, HUD_DEPTH
        );

        /* --- Replay banner (REPLAY, top-left) --- */
        TD5_AtlasEntry *replay = td5_asset_find_atlas_entry(NULL, "REPLAY");
        if (v == 0) {
            hud_log_atlas_status("REPLAY", replay);
        }
        float replay_x = sx * 16.0f + vp_l;
        float replay_y = sy * 16.0f + vp_t;

        hud_build_quad(
            view_base + 0x7EC,
            0, replay->texture_page,
            replay_x, replay_y,
            replay_x + sx * 60.0f, replay_y + sy * 60.0f,
            (float)replay->atlas_x + 0.5f,
            (float)replay->atlas_y + 0.5f,
            (float)(replay->atlas_x + replay->width) - 0.5f,
            (float)(replay->atlas_y + replay->height) - 0.5f,
            0xFFFFFFFF, HUD_DEPTH
        );

        s_cur_view++;
    }

    /* Initialize minimap */
    td5_hud_init_minimap_layout();
}

/* ========================================================================
 * BuildRaceHudMetricDigits (0x4397B0)
 *
 * Updates the metric digit display (speed, FPS, odometer, etc.) and
 * builds UV coordinates for the numbers atlas quads.
 * ======================================================================== */

int td5_hud_build_metric_digits(void)
{
    int actor_slot = g_actor_slot_map[s_cur_view];

    /* Select metric value based on display mode */
    switch (g_hud_metric_mode) {
    case TD5_METRIC_FINISH_TIMER:
        s_metric_value = (uint32_t)actor_lap_time(actor_slot, 0);
        break;
    case TD5_METRIC_FPS:
        s_metric_value = (uint32_t)(g_instant_fps + 0.5f);
        break;
    case TD5_METRIC_ODOMETER: {
        /* 4-digit display, needs extra thousands digit quad */
        uint32_t raw = (uint32_t)actor_span_index(actor_slot);
        s_metric_value = raw % 10000;

        /* Build thousands digit UV */
        uint32_t d3 = s_metric_value / 1000;
        int col = (int)(d3 % 5);
        int row = (int)(d3 / 5);

        float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
        float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

        /* Update the 4th digit quad UV */
        uint8_t *view_base = s_hud_prim_storage;
        hud_build_quad(
            view_base + 0x734,
            2, s_numbers_atlas->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            u0, v0, u0 + 15.0f, v0 + 23.0f,
            0xFFFFFFFF, HUD_DEPTH
        );
        break;
    }
    case TD5_METRIC_BYTE_METRIC:
        s_metric_value = (uint32_t)actor_gear(actor_slot);
        break;
    default:
        g_hud_metric_mode = 3; /* wrap around */
        s_metric_value = 0;
        break;
    }

    /* Extract and display 3 digits (hundreds, tens, ones) */
    uint32_t val = s_metric_value % 1000;

    /* Hundreds */
    {
        uint32_t d = val / 100;
        int col = (int)(d % 5);
        int row = (int)(d / 5);
        float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
        float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

        uint8_t *view_base = s_hud_prim_storage;
        hud_build_quad(
            view_base + 0x454,
            2, s_numbers_atlas->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            u0, v0, u0 + 15.0f, v0 + 23.0f,
            0xFFFFFFFF, HUD_DEPTH
        );
    }

    /* Tens */
    val = val % 100;
    {
        uint32_t d = val / 10;
        int col = (int)(d % 5);
        int row = (int)(d / 5);
        float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
        float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

        uint8_t *view_base = s_hud_prim_storage;
        hud_build_quad(
            view_base + 0x50C,
            2, s_numbers_atlas->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            u0, v0, u0 + 15.0f, v0 + 23.0f,
            0xFFFFFFFF, HUD_DEPTH
        );
    }

    /* Ones */
    {
        uint32_t d = val % 10;
        int col = (int)(d % 5);
        int row = (int)(d / 5);
        float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
        float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

        uint8_t *view_base = s_hud_prim_storage;
        hud_build_quad(
            view_base + 0x5C4,
            2, s_numbers_atlas->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            u0, v0, u0 + 15.0f, v0 + 23.0f,
            0xFFFFFFFF, HUD_DEPTH
        );
    }

    return 1;
}

/* ========================================================================
 * SetRaceHudIndicatorState (0x439B60)
 *
 * Sets the countdown/finish indicator digit for a view.
 * ======================================================================== */

void td5_hud_set_indicator_state(int view_index, int value)
{
    s_indicator_state[view_index] = value;
}

/* ========================================================================
 * DrawRaceStatusText (0x439B70)
 *
 * Per-view text overlays: race status messages, wanted messages, time
 * trial timers, lap time comparisons.
 * ======================================================================== */

void td5_hud_draw_status_text(int player_slot, int view_index)
{
    /* Skip during special render mode */
    /* g_special_render_mode at 0x466E9C */
    extern int g_special_render_mode;
    if (g_special_render_mode != 0) return;

    s_cur_scale = (float *)&s_view_layout[view_index];
    float vp_half_w = s_view_layout[view_index].half_width;
    float vp_top    = s_view_layout[view_index].vp_int_top;
    float vp_bottom = s_view_layout[view_index].vp_int_bottom;

    /* Demo mode: show "DEMO MODE" text */
    if (g_replay_mode != 0) {
        td5_hud_queue_text(0,
            (int)vp_half_w,
            (int)(vp_top + 16.0f),
            1, /* centered */
            "%s", s_hud_string_table[10]); /* "DEMO MODE" */
        return;
    }

    /* Wanted mode messages */
    if (g_wanted_mode_enabled != 0 && s_is_first_player != 0 && g_wanted_msg_timer < 300) {
        g_wanted_msg_timer++;

        /* Flash after frame 270 (odd frames hidden) */
        if (g_wanted_msg_timer < 270 || (g_wanted_msg_timer & 1) == 0) {
            td5_hud_queue_text(0,
                (int)vp_half_w,
                (int)(vp_top + 16.0f),
                1, /* centered */
                "%s", g_wanted_msg_line1[g_wanted_msg_index * 2]);

            td5_hud_queue_text(0,
                (int)s_view_layout[view_index].half_width,
                (int)(vp_top + 32.0f),
                1,
                "%s", g_wanted_msg_line2[g_wanted_msg_index * 2]);
        }
    }

    /* Time trial / special encounter timers */
    if (g_special_encounter != 0 || g_td5.time_trial_enabled) {
        /* Main player timer */
        int actor_slot = g_actor_slot_map[view_index];
        uint16_t time_raw = actor_lap_time(actor_slot, 0);
        int total_ms = ((int)time_raw * 100) / 30;
        if (total_ms != 0) {
            int mins = total_ms / 6000;
            int secs = (total_ms / 100) % 60;
            int hundredths = total_ms % 100;

            td5_hud_queue_text(0,
                (int)(s_view_layout[view_index].vp_int_left + 8.0f),
                (int)(vp_top + 8.0f),
                0,
                "%s %02d:%02d.%03d",
                s_hud_string_table[11], /* "TIME" */
                mins, secs, hundredths);
        }

        /* Check for finished race / new best lap indicators */
        extern int g_pending_finish_timer;
        extern int g_race_end_state;

        if (g_pending_finish_timer == 0 && g_race_end_state == 0) {
            /* Show best lap comparison if applicable */
            extern int32_t g_actor_best_lap;
            extern int32_t g_actor_best_race;
            if (g_actor_best_lap != 0) {
                int bl_ms = ((int)g_actor_best_lap * 100) / 30;
                td5_hud_queue_text(0,
                    (int)vp_half_w,
                    (int)(vp_top + 8.0f),
                    1,
                    "%s %02d:%02d.%03d",
                    s_hud_string_table[7], /* "LAP" */
                    bl_ms / 6000, (bl_ms / 100) % 60, bl_ms % 100);
            }
            if (g_actor_best_race != 0) {
                int br_ms = ((int)g_actor_best_race * 100) / 30;
                td5_hud_queue_text(0,
                    (int)vp_half_w,
                    (int)(vp_top + 24.0f),
                    1,
                    "%s %02d:%02d.%03d",
                    s_hud_string_table[11],
                    br_ms / 6000, (br_ms / 100) % 60, br_ms % 100);
            }
        }
    }

    {
        static int s_hud_layout_logged = 0;
        if (!s_hud_layout_logged) {
            TD5_LOG_I(LOG_TAG,
                      "hud layout: scale=(%.3f, %.3f) viewport=%dx%d views=%d",
                      s_scale_x, s_scale_y,
                      g_render_width, g_render_height, s_view_count);
            s_hud_layout_logged = 1;
        }
    }
}

/* ========================================================================
 * RenderRaceHudOverlays (0x4388A0)
 *
 * Master per-frame HUD rendering function. Iterates all views and
 * renders: position labels, timers, speedometer, needle, gear indicator,
 * speed digits, metric digits, U-turn warning, replay banner, minimap,
 * screen fade, and split-screen dividers.
 * ======================================================================== */

void td5_hud_render_overlays(float dt)
{
    /* dt is normalized 30 Hz frame time from td5_game.c. */
    s_cur_view = 0;

    for (int v = 0; v < s_view_count; v++) {
        s_cur_flags = s_hud_flags[v];
        s_cur_scale = (float *)&s_view_layout[v];
        int actor_slot = g_actor_slot_map[v];

        TD5_HudViewLayout *vl = &s_view_layout[v];
        float sx = vl->scale_x;
        float sy = vl->scale_y;
        uint32_t flags = *s_cur_flags;

        if ((g_tick_counter % 60u) == 0u) {
            TD5_LOG_D(LOG_TAG,
                      "overlay view %d: visible_mask=0x%08X",
                      v, (unsigned int)flags);
        }

        uint8_t *view_base = s_hud_prim_storage;

        /* --- Bit 0: Race position label --- */
        if (flags & TD5_HUD_POSITION_LABEL) {
            uint8_t pos = actor_race_position(actor_slot);
            td5_hud_queue_text(0,
                (int)(vl->vp_int_left + 8.0f),
                (int)(vl->vp_int_top + 8.0f),
                0,
                "%s", s_hud_string_table[pos]);
        }

        /* --- Bit 7: Total timer "%s %d" --- */
        if (flags & TD5_HUD_TOTAL_TIMER) {
            td5_hud_queue_text(0,
                (int)(vl->vp_int_left + 8.0f),
                (int)(vl->vp_int_top + 24.0f),
                0,
                "%s %d", s_hud_string_table[11], 0 /* timer value */);
        }

        /* --- Bit 9: Circuit lap count --- */
        if (flags & TD5_HUD_CIRCUIT_LAPS) {
            int cur_lap = td5_game_get_player_lap(actor_slot) + 1;
            int total_laps = g_td5.circuit_lap_count;
            td5_hud_queue_text(0,
                (int)(vl->vp_int_left + 8.0f),
                (int)(vl->vp_int_top + 40.0f),
                0,
                "%s %d/%d", s_hud_string_table[12], cur_lap, total_laps);
        }

        /* --- Bit 8: Lap/checkpoint counter --- */
        if (flags & TD5_HUD_LAP_COUNTER) {
            td5_hud_queue_text(0,
                (int)(vl->vp_int_left + 8.0f),
                (int)(vl->vp_int_top + 8.0f),
                0,
                "%s %d", s_hud_string_table[11], 0);
        }

        /* --- Bit 1: Lap timers --- */
        if (flags & TD5_HUD_LAP_TIMERS) {
            for (int r = 0; r < 6; r++) {
                uint16_t time_raw = actor_lap_time(actor_slot, r);
                int total_cs = ((int)time_raw * 100) / 30;
                if (total_cs == 0) continue;

                int mins = total_cs / 6000;
                int secs = (total_cs / 100) % 60;
                int hundredths = total_cs % 100;

                if (r == 0) {
                    td5_hud_queue_text(0,
                        (int)(vl->vp_int_right - 120.0f),
                        (int)(vl->vp_int_top + 8.0f),
                        0,
                        "%s %02d:%02d.%02d",
                        s_hud_string_table[11],
                        mins, secs, hundredths);
                } else {
                    td5_hud_queue_text(0,
                        (int)(vl->vp_int_right - 80.0f),
                        (int)(vl->vp_int_top + 8.0f + r * 16.0f),
                        0,
                        "%02d:%02d.%02d",
                        mins, secs, hundredths);
                }
            }
        }

        /* --- Bit 2: Speedometer (needle + gear + digits) --- */
        if (flags & TD5_HUD_SPEEDOMETER) {
            int32_t engine_speed = actor_engine_speed(actor_slot);
            int16_t max_rpm     = actor_max_rpm(actor_slot);

            /* Needle sweeps clockwise from ~7 o'clock (0 RPM) to ~5 o'clock (redline).
             * Base 0x400 (90° in 12-bit); add RPM contribution to sweep clockwise.
             * Sweep range 0xA5A ≈ 233° matches the original dial arc.
             * Original formula: angle = (speed * 0xA5A) / maxRPM + 0x400 */
            uint32_t needle_angle;
            if (max_rpm > 0) {
                uint32_t rpm_offset = (uint32_t)((engine_speed * 0xA5A) / (int32_t)max_rpm);
                needle_angle = (rpm_offset + 0x400) & 0xFFF;
            } else {
                needle_angle = 0x400;
            }

            float cos_a = td5_cos_12bit(needle_angle);
            float sin_a = td5_sin_12bit(needle_angle);

            /* Needle center is at speedometer center */
            float cx = vl->vp_int_right - sx * 64.0f;
            float cy = vl->vp_int_bottom - sy * 56.0f;

            /* V0: near end (9 units into dial), V2: far tip (45 units out) */
            float near_x = cx - cos_a * sx * 9.0f;
            float near_y = cy - sin_a * sy * 9.0f;

            float base_offset_x = sin_a * sx * 2.0f;
            float base_offset_y = cos_a * sy * 2.0f;

            float tip_x = cx + cos_a * sx * 45.0f;
            float tip_y = cy + sin_a * sy * 45.0f;

            TD5_LOG_I(LOG_TAG, "speedo: rpm=%d max=%d angle=0x%03X cos=%.3f sin=%.3f cx=%.1f cy=%.1f tip=(%.1f,%.1f) near=(%.1f,%.1f)",
                engine_speed, max_rpm, needle_angle, cos_a, sin_a, cx, cy, tip_x, tip_y, near_x, near_y);

            /* Build needle quad: V0=near(9), V1=left-perp, V2=far-tip(45), V3=right-perp */
            /* Needle uses mode 1 (position + color, no texture) */
            struct {
                void *dest;
                int   mode;
                float x[4], y[4];
                float depth[4];
                float u[4], v[4];
                uint32_t color[4];
                int tex, pad;
            } needle_params;

            needle_params.dest = view_base + 0x39C; /* needle quad offset */
            needle_params.mode = 1;
            needle_params.x[0] = near_x;
            needle_params.x[1] = cx - base_offset_x;
            needle_params.x[2] = tip_x;
            needle_params.x[3] = cx + base_offset_x;
            needle_params.y[0] = near_y;
            needle_params.y[1] = cy + base_offset_y;
            needle_params.y[2] = tip_y;
            needle_params.y[3] = cy - base_offset_y;
            for (int i = 0; i < 4; i++) needle_params.depth[i] = HUD_DEPTH;
            memset(needle_params.u, 0, sizeof(needle_params.u));
            memset(needle_params.v, 0, sizeof(needle_params.v));
            for (int i = 0; i < 4; i++) needle_params.color[i] = 0xFFFFFFFF;
            needle_params.tex = HUD_WHITE_TEX_PAGE;
            needle_params.pad = 0;

            td5_render_build_sprite_quad((int *)&needle_params);

            /* Update gear indicator UV from GEARNUMBERS atlas (linear strip,
             * 8 slots × 16px wide at (128,128) 128×16). */
            uint8_t gear = actor_gear(actor_slot);
            {
                float gu = (float)gear * 16.0f + (float)s_gearnumbers_atlas->atlas_x;
                float gv = (float)s_gearnumbers_atlas->atlas_y;
                hud_build_quad(
                    view_base + GEAR_QUAD_OFF,
                    2, s_gearnumbers_atlas->texture_page,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    gu + 0.5f, gv + 0.5f,
                    gu + 15.5f, gv + 15.5f,
                    0xFFFFFFFF, HUD_DEPTH
                );
            }

            /* Compute speed value from longitudinal_speed (24.8 fp) for digit display */
            uint8_t *_actor_a = (uint8_t *)actor_ptr(actor_slot);
            int32_t speed_raw = *(int32_t *)(_actor_a + 0x314);
            if (speed_raw < 0) speed_raw = 0;
            speed_raw >>= 8;

            int speed_display;
            if (g_kph_mode == 0) {
                speed_display = (speed_raw * 256 + 625) / 1252; /* MPH */
            } else {
                speed_display = (speed_raw * 256 + 389) / 778;  /* KPH */
            }

            /* Build and submit speed digit quads using SPEEDOFONT atlas
             * (linear strip of 10 digits, 16px wide each at atlas (96,160)).
             * Original renders right-to-left: ones at anchor, each additional
             * digit subtracts (glyph_w + 2.0) moving LEFT (0x438E90).
             * Inter-digit gap is 2.0 [CONFIRMED @ 0x45d6d8]. */
            float sf_gw = sx * 15.0f;
            float sf_step = sf_gw + 2.0f;
            float sf_x0 = vl->vp_int_right - sx * 60.0f;
            float sf_y0 = vl->vp_int_bottom - sy * 23.0f - sy * 8.0f;
            float sf_y1 = sf_y0 + sy * 24.0f;
            float digit_u_base = (float)s_speedofont_atlas->atlas_x;
            float digit_v_base = (float)s_speedofont_atlas->atlas_y;
            int   sf_pg = s_speedofont_atlas->texture_page;

            int ones = speed_display % 10;
            int tens_val = (speed_display / 10) % 10;
            int hundreds_val = speed_display / 100;

            /* Determine digit count to compute right-shifted anchor.
             * All digits shift right so the leftmost digit stays at sf_x0. */
            int num_digits = 1;
            if (speed_display >= 100) num_digits = 3;
            else if (speed_display >= 10) num_digits = 2;

            /* Ones anchor: rightmost position = sf_x0 + (num_digits-1) * step */
            float ones_x = sf_x0 + (float)(num_digits - 1) * sf_step;

            /* Submit speedo dial */
            hud_submit_quad(view_base + SPEEDO_QUAD_OFF);
            /* Submit needle */
            hud_submit_quad(view_base + 0x39C);

            /* Ones digit (always shown) */
            float dx = ones_x;
            float u_ones = digit_u_base + (float)ones * 16.0f + 0.5f;
            hud_build_quad(
                view_base + SPEEDFONT_BASE_OFF,
                0, sf_pg,
                dx, sf_y0, dx + sf_gw, sf_y1,
                u_ones, digit_v_base + 0.5f,
                u_ones + 15.5f, digit_v_base + 23.5f,
                0xFFFFFFFF, HUD_DEPTH
            );
            hud_submit_quad(view_base + SPEEDFONT_BASE_OFF);

            /* Tens digit if speed >= 10 (one step LEFT of ones) */
            if (speed_display >= 10) {
                float u_tens = digit_u_base + (float)tens_val * 16.0f + 0.5f;
                dx = ones_x - sf_step;
                hud_build_quad(
                    view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE,
                    0, sf_pg,
                    dx, sf_y0, dx + sf_gw, sf_y1,
                    u_tens, digit_v_base + 0.5f,
                    u_tens + 15.5f, digit_v_base + 23.5f,
                    0xFFFFFFFF, HUD_DEPTH
                );
                hud_submit_quad(view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE);

                /* Hundreds digit if speed >= 100 (two steps LEFT of ones) */
                if (speed_display >= 100) {
                    float u_hund = digit_u_base + (float)hundreds_val * 16.0f + 0.5f;
                    dx = ones_x - 2.0f * sf_step;
                    hud_build_quad(
                        view_base + SPEEDFONT_BASE_OFF + 2 * TD5_HUD_GLYPH_QUAD_SIZE,
                        0, sf_pg,
                        dx, sf_y0, dx + sf_gw, sf_y1,
                        u_hund, digit_v_base + 0.5f,
                        u_hund + 15.5f, digit_v_base + 23.5f,
                        0xFFFFFFFF, HUD_DEPTH
                    );
                    hud_submit_quad(view_base + SPEEDFONT_BASE_OFF + 2 * TD5_HUD_GLYPH_QUAD_SIZE);
                }
            }

            /* Submit gear indicator */
            hud_submit_quad(view_base + GEAR_QUAD_OFF);
        }

        /* --- Bit 6: Metric digit display --- */
        if ((flags & TD5_HUD_METRIC_DIGITS) && s_numbers_atlas != NULL) {
            int metric_ok = td5_hud_build_metric_digits();
            if (metric_ok) {
                /* Submit the 4th digit if in odometer mode */
                if (g_hud_metric_mode == TD5_METRIC_ODOMETER) {
                    hud_submit_quad(view_base + 0x734);
                }
                /* Submit hundreds, tens, ones */
                hud_submit_quad(view_base + 0x454);
                hud_submit_quad(view_base + 0x50C);
                hud_submit_quad(view_base + 0x5C4);
            }
        }

        /* --- Bit 4: U-turn warning --- */
        if (flags & TD5_HUD_UTURN_WARNING) {
            int16_t cur_span = actor_span_index(actor_slot);
            int prev_span = s_prev_span_pos[v];

            /* Track wrong-way movement */
            if ((int)cur_span < prev_span) {
                s_wrong_way_counter[v]++;
            }
            if (prev_span < (int)cur_span) {
                s_wrong_way_counter[v] = 0;
            }

            /* Check heading delta for > 90 degree wrong way */
            uint8_t route_idx = actor_route_index(actor_slot);
            extern void *g_route_data;
            uint32_t heading_delta = td5_compute_heading_delta(
                (uint8_t *)g_route_data + route_idx * 0x47);

            if (heading_delta > 0x3FF && heading_delta < 0xC00 &&
                s_wrong_way_counter[v] > 2) {
                /* Flash at ~8Hz (visible when tick counter mod 32 > 8) */
                uint32_t flash = g_tick_counter & 0x1F;
                if (flash > 8) {
                    hud_submit_quad(view_base + 0x67C);
                }
            }

            s_prev_span_pos[v] = (int)cur_span;
        }

        /* --- Bit 31: Replay banner (flashes every 32 ticks) --- */
        if ((flags & TD5_HUD_REPLAY_BANNER) && (g_tick_counter & 0x20)) {
            hud_submit_quad(view_base + 0x7EC);
        }

        /* --- Indicator digit (countdown/finish) --- */
        if (s_indicator_state[v] != 0 && s_numbers_atlas) {
            int digit_val = s_indicator_state[v];
            int col = digit_val % 5;
            int row = digit_val / 5;

            float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
            float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

            /* Build and submit a centered digit quad */
            TD5_SpriteQuad indicator_quad;
            float ind_x = vl->center_x - sx * 16.0f;
            float ind_y = vl->center_y - sy * 24.0f;

            hud_build_quad(
                &indicator_quad,
                0, s_numbers_atlas->texture_page,
                ind_x, ind_y,
                ind_x + sx * 16.0f, ind_y + sy * 24.0f,
                u0, v0, u0 + 15.0f, v0 + 23.0f,
                0xFFFFFFFF, HUD_DEPTH
            );
            hud_submit_quad(&indicator_quad);
        }

        s_cur_view++;
    }

    /* --- Minimap (single-player only, non-circuit tracks) --- */
    if (g_split_screen_mode == 0 && (*s_hud_flags[0] & TD5_HUD_UTURN_WARNING) &&
        g_track_type_mode == 0) {
        int actor_slot = g_actor_slot_map[0];
        td5_hud_render_minimap(actor_slot);
    }

    /* Restore full-screen viewport */
    td5_render_set_clip_rect(0.0f, (float)g_render_width, 0.0f, (float)g_render_height);
    td5_render_set_projection_center((float)g_render_width, (float)g_render_height);

    /* Flush queued text glyphs */
    td5_hud_flush_text();

    /* Debug overlay (gated by td5re.ini DebugOverlay setting) */
    if (g_td5.ini.debug_overlay) {
        td5_hud_queue_text(0, 8, 8, 0, "FPS: %.0f", g_td5.instant_fps);
        td5_hud_flush_text();
    }

    /* Radial pulse effect — only active in single-player free race.
     * Original RunRaceFrame @ 0x42B67F gates on:
     *   g_humanPlayerCount==1 && !dead && !network && selectedGameType==0
     *   && !g_dragRaceModeEnabled
     * [RE basis: research agent deep pass] */
    if (!g_split_screen_mode &&
        !g_td5.network_active &&
        g_td5.game_type == TD5_GAMETYPE_SINGLE_RACE &&
        !g_td5.drag_race_enabled) {
        td5_render_radial_pulse(dt);
    }

    /* Split-screen divider bars */
    if (g_split_screen_mode != 0) {
        hud_submit_quad(&s_divider_quad_h + (g_split_screen_mode - 1));
    }
}

/* ========================================================================
 * RenderTrackMinimapOverlay (0x43A220)
 *
 * Renders the minimap for point-to-point tracks. Draws the track
 * segments, checkpoint markers, and racer dots relative to the player's
 * heading.
 * ======================================================================== */

void td5_hud_render_minimap(int actor_slot)
{
    /* Only for point-to-point tracks */
    if (g_track_is_circuit != 0) return;

    /* Set minimap clip rect */
    td5_render_set_clip_rect(
        s_minimap_x, s_minimap_x + s_minimap_width,
        s_minimap_y, s_minimap_y + s_minimap_height);

    td5_render_set_projection_center(
        s_minimap_width * 0.5f + s_minimap_x,
        s_minimap_height * 0.5f + s_minimap_y);

    /* Minimap screen center — used for all coord offsets below since
     * td5_render_set_projection_center is a stub (no-op in source port). */
    float mm_cx = s_minimap_width * 0.5f + s_minimap_x;
    float mm_cy = s_minimap_height * 0.5f + s_minimap_y;

    /* Compute player heading rotation */
    int32_t heading = actor_heading(actor_slot);
    uint32_t rot_angle = 0x800 - (uint32_t)(heading >> 8);

    float cos_h = td5_cos_12bit(rot_angle);
    float sin_h = td5_sin_12bit(rot_angle);

    /* World-to-minimap offset (player centered).
     * World coordinates are 24.8 fixed-point; multiply by 1/256 to convert
     * to float world units before applying s_minimap_world_scale_x.
     * Binary: fVar9 = -(worldX * _DAT_004749d0) where _DAT_004749d0 = 1/256. */
    const float kFP = 1.0f / 256.0f;
    float offset_x = -(float)actor_world_x(actor_slot) * kFP;
    float offset_z = -(float)actor_world_z(actor_slot) * kFP;

    /* Submit 16 background tiles (binary: offset 0x4500..0x507F, stride 0xB8) */
    for (int i = 0; i < 16; i++) {
        int buf_off = 0x4500 + i * TD5_HUD_GLYPH_QUAD_SIZE;
        hud_submit_quad(s_minimap_quad_buf + buf_off);
    }

    /* Walk track spans and render road segments using the pre-built segment table.
     * Original @ 0x43A220 iterates the segment table (built in InitMinimapLayout),
     * starting from the segment containing start_span [CONFIRMED @ 0x43A350-0x43A380],
     * rendering up to 48 segments [CONFIRMED @ 0x43B09B: while (local_8c < 0x30)].
     *
     * start_span = ((player_span / 24) - 6) * 24 [CONFIRMED @ 0x43A380]:
     * round down to 24-span group, go back 6 groups (144 spans behind player). */
    int16_t player_span = actor_span_index(actor_slot);
    int start_span = ((int)player_span / 24 - 6) * 24;
    if (start_span < 0) start_span = 0;

    uint8_t *span_base = (uint8_t *)g_strip_span_base;
    uint8_t *vert_base = (uint8_t *)g_strip_vertex_base;
    int dot_count = 0;
    TD5_SpriteQuad map_quad;

    /* Minimap boundary for software clip (original: software-only globals @ 0x43E640,
     * not GPU scissor; we enforce same constraint as bounds check before submit). */
    float mm_r = s_minimap_x + s_minimap_width;
    float mm_b = s_minimap_y + s_minimap_height;

    /* Walk up to 48 road quads [CONFIRMED @ 0x43A220: while (local_8c < 0x30)].
     *
     * Each iteration reads TWO span records (span_a, span_b) and emits a single
     * warped 4-corner quad:
     *   vert0 = span_a's "left vertex" at span+0x04 (plus span_a origin)
     *   vert1 = span_a's right edge: vert0 + (span_a[3]&0xf) + table_a[span_a[0]]
     *   vert2 = span_b's "left vertex" at span+0x06 (DIFFERENT field from span_a!)
     *   vert3 = span_b's right edge: vert2 + (span_b[3]&0xf) + table_b[span_b[0]]
     * [CONFIRMED @ 0x0043a220 decomp]
     *
     * span_b = span_a + 5, clamped to segment boundary. advance += 6 per iter.
     */
    int local_a4 = start_span;
    int seg_rendered = 0;
    const float road_diffuse_alpha = 1.0f; (void)road_diffuse_alpha;
    for (int i = 0; span_base && vert_base && i < 0x30; i++) {
        int span_a_idx = local_a4;
        int span_b_idx = span_a_idx + 5;
        local_a4 = span_b_idx + 1; /* advance by 6 */

        if (span_a_idx < 0 || span_b_idx < 0 ||
            span_a_idx >= g_strip_span_count ||
            span_b_idx >= g_strip_span_count) continue;

        uint8_t *sa = span_base + span_a_idx * 24;
        uint8_t *sb = span_base + span_b_idx * 24;

        int32_t ox_a  = *(int32_t  *)(sa + 0x0C);
        int32_t oz_a  = *(int32_t  *)(sa + 0x14);
        int32_t ox_b  = *(int32_t  *)(sb + 0x0C);
        int32_t oz_b  = *(int32_t  *)(sb + 0x14);

        /* span_a uses left-vertex index at +0x04 [CONFIRMED @ 0x0043a220] */
        uint16_t vi_a_left  = *(uint16_t *)(sa + 0x04);
        uint8_t  type_a     = sa[0];
        uint8_t  delta_a    = sa[3] & 0x0f;
        int32_t  tbl_a      = s_minimap_vtx_delta_a[type_a & 0x0f];
        int32_t  vi_a_right = (int32_t)vi_a_left + (int32_t)delta_a + tbl_a;

        /* span_b uses left-vertex index at +0x06 — different field from span_a */
        uint16_t vi_b_left  = *(uint16_t *)(sb + 0x06);
        uint8_t  type_b     = sb[0];
        uint8_t  delta_b    = sb[3] & 0x0f;
        int32_t  tbl_b      = s_minimap_vtx_delta_b[type_b & 0x0f];
        int32_t  vi_b_right = (int32_t)vi_b_left + (int32_t)delta_b + tbl_b;

        if (vi_a_right < 0 || vi_b_right < 0) continue;

        int16_t *va_l = (int16_t *)(vert_base + (uint32_t)vi_a_left  * 6);
        int16_t *va_r = (int16_t *)(vert_base + (uint32_t)vi_a_right * 6);
        int16_t *vb_l = (int16_t *)(vert_base + (uint32_t)vi_b_left  * 6);
        int16_t *vb_r = (int16_t *)(vert_base + (uint32_t)vi_b_right * 6);

        /* World-space corners (raw world units; only actor coords use kFP) */
        float wx0 = (float)((int)va_l[0] + ox_a) + offset_x;
        float wz0 = (float)((int)va_l[2] + oz_a) + offset_z;
        float wx1 = (float)((int)va_r[0] + ox_a) + offset_x;
        float wz1 = (float)((int)va_r[2] + oz_a) + offset_z;
        float wx2 = (float)((int)vb_l[0] + ox_b) + offset_x;
        float wz2 = (float)((int)vb_l[2] + oz_b) + offset_z;
        float wx3 = (float)((int)vb_r[0] + ox_b) + offset_x;
        float wz3 = (float)((int)vb_r[2] + oz_b) + offset_z;

        /* Rotate into player-up minimap space */
        float mx0 = (wx0 * cos_h + wz0 * sin_h) * s_minimap_world_scale_x;
        float my0 = (wz0 * cos_h - wx0 * sin_h) * s_minimap_world_scale_y;
        float mx1 = (wx1 * cos_h + wz1 * sin_h) * s_minimap_world_scale_x;
        float my1 = (wz1 * cos_h - wx1 * sin_h) * s_minimap_world_scale_y;
        float mx2 = (wx2 * cos_h + wz2 * sin_h) * s_minimap_world_scale_x;
        float my2 = (wz2 * cos_h - wx2 * sin_h) * s_minimap_world_scale_y;
        float mx3 = (wx3 * cos_h + wz3 * sin_h) * s_minimap_world_scale_x;
        float my3 = (wz3 * cos_h - wx3 * sin_h) * s_minimap_world_scale_y;

        float sx0 = mm_cx + mx0, sy0 = mm_cy + my0;
        float sx1 = mm_cx + mx1, sy1 = mm_cy + my1;
        float sx2 = mm_cx + mx2, sy2 = mm_cy + my2;
        float sx3 = mm_cx + mx3, sy3 = mm_cy + my3;

        /* Skip if all 4 corners are entirely outside the minimap rect */
        float min_x = sx0, max_x = sx0, min_y = sy0, max_y = sy0;
        if (sx1 < min_x) min_x = sx1; if (sx1 > max_x) max_x = sx1;
        if (sx2 < min_x) min_x = sx2; if (sx2 > max_x) max_x = sx2;
        if (sx3 < min_x) min_x = sx3; if (sx3 > max_x) max_x = sx3;
        if (sy1 < min_y) min_y = sy1; if (sy1 > max_y) max_y = sy1;
        if (sy2 < min_y) min_y = sy2; if (sy2 > max_y) max_y = sy2;
        if (sy3 < min_y) min_y = sy3; if (sy3 > max_y) max_y = sy3;
        if (max_x < s_minimap_x || min_x > mm_r ||
            max_y < s_minimap_y || min_y > mm_b) continue;

        hud_build_quad_warped(
            &map_quad, HUD_WHITE_TEX_PAGE,
            sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3,
            0.0f, 0.0f, 0.0f, 0.0f,
            0xFF9A9A9A,
            HUD_DEPTH
        );
        hud_submit_quad(&map_quad);
        seg_rendered++;
    }
    TD5_LOG_I(LOG_TAG, "minimap_render: segs_rendered=%d start_span=%d end_span=%d span_count=%d",
              seg_rendered, start_span, local_a4 - 1, g_strip_span_count);

    /* Render racer dot markers */
    for (int r = 0; r < g_racer_count; r++) {
        int16_t racer_span = actor_span_index(r);
        int span_delta;
        if (g_track_is_circuit) {
            span_delta = ((g_strip_span_count / 2 - (int)racer_span) + (int)player_span)
                         % g_strip_span_count - g_strip_span_count / 2;
        } else {
            span_delta = (int)player_span - (int)racer_span;
        }

        /* Only show racers within +/-144 spans */
        if (span_delta > -0x91 && span_delta < 0x91) {
            float rwx = (float)actor_world_x(r) * kFP + offset_x;
            float rwz = (float)actor_world_z(r) * kFP + offset_z;

            float dmx = (rwx * cos_h + rwz * sin_h) * s_minimap_world_scale_x;
            float dmy = (rwz * cos_h - rwx * sin_h) * s_minimap_world_scale_y;

            float half_dot = s_minimap_dot_size * 0.5f;

            /* Skip dots that fall outside the minimap rect.
             * The original relied on software projection-center clipping;
             * td5_render_set_clip_rect is a stub here, so we cull in code.
             * Previous behavior clamped to edge, which made distant racers
             * appear pinned to the minimap border — fixing the user-visible
             * "dots still visible far away" complaint. */
            float dot_x = mm_cx + dmx;
            float dot_y = mm_cy + dmy;
            if (dot_x + half_dot < s_minimap_x ||
                dot_x - half_dot > s_minimap_x + s_minimap_width ||
                dot_y + half_dot < s_minimap_y ||
                dot_y - half_dot > s_minimap_y + s_minimap_height) {
                continue;
            }

            /* Use scandots texture: 3 dots arranged horizontally in tpage5.
             * Confirmed Ghidra constants @ 0x45D724=8.5, 0x45D720=16.5 are
             * U-axis offsets: player at u=atlas_x+0.5, AI at +8.5, other at +16.5.
             * Pixel data: player(64,200)=red, AI(72,200)=blue, other(80,200)=teal. */
            int dot_tex = s_minimap_scandots_tex_page ? s_minimap_scandots_tex_page : HUD_WHITE_TEX_PAGE;
            float dot_u0;
            if (r == g_actor_slot_map[0]) {
                dot_u0 = s_minimap_dot_atlas_u;          /* col 0: player (red) */
            } else if (r < 6) {
                dot_u0 = s_minimap_dot_atlas_u + 8.5f;  /* col 1: AI (blue) [@ 0x45D724] */
            } else {
                dot_u0 = s_minimap_dot_atlas_u + 16.5f; /* col 2: other (teal) [@ 0x45D720] */
            }
            hud_build_quad(
                &map_quad,
                0, dot_tex,
                dot_x - half_dot, dot_y - half_dot,
                dot_x + half_dot, dot_y + half_dot,
                dot_u0, s_minimap_dot_atlas_v,
                dot_u0 + 7.0f,
                s_minimap_dot_atlas_v + 7.0f,
                0xFFFFFFFF,
                HUD_DEPTH
            );
            hud_submit_quad(&map_quad);
            dot_count++;
        }
    }

    TD5_LOG_I(LOG_TAG,
              "minimap: actor=%d player_span=%d start_span=%d segs_rendered=%d dots=%d span_count=%d",
              actor_slot, (int)player_span, start_span, seg_rendered, dot_count, g_strip_span_count);
}

/* ========================================================================
 * InitializeMinimapLayout (0x43B0A0)
 *
 * Computes minimap bounds, allocates quad buffers, loads sprite assets,
 * and builds the track segment route table.
 * ======================================================================== */

void td5_hud_init_minimap_layout(void)
{
    /* Compute minimap dimensions from scale factors */
    s_minimap_width  = s_scale_x * 100.0f;
    s_minimap_height = s_scale_y * 100.0f;
    s_minimap_dot_size = s_scale_x * 7.0f;

    /* Allocate minimap quad buffer */
    s_minimap_quad_buf = (uint8_t *)td5_game_heap_alloc(TD5_HUD_MINIMAP_BUF_SIZE);

    /* Position: bottom-left corner */
    s_minimap_x = s_scale_x * 8.0f;
    s_minimap_y = g_render_height_f - s_minimap_height - s_scale_y * 8.0f;

    /* World-to-minimap scale */
    s_minimap_world_scale_x = s_scale_x * (1.0f / 1024.0f);
    s_minimap_world_scale_y = s_scale_y * (1.0f / 1024.0f);
    s_minimap_tile_width = s_minimap_width;
    s_minimap_tile_height = s_minimap_height;

    /* Load scandots sprite (racer dot markers) */
    TD5_AtlasEntry *scandots = td5_asset_find_atlas_entry(NULL, "scandots");
    float dot_u = (float)scandots->atlas_x + 0.5f;
    float dot_v = (float)scandots->atlas_y + 0.5f;
    float dot_v_stride = 7.0f;

    /* Store for use in td5_hud_render_minimap per-frame dot draws */
    s_minimap_scandots_tex_page  = scandots->texture_page;
    s_minimap_dot_atlas_u        = dot_u;
    s_minimap_dot_atlas_v        = dot_v;
    s_minimap_dot_atlas_vstride  = dot_v_stride;
    TD5_LOG_I(LOG_TAG, "minimap_init: scandots tex_page=%d u=%.1f v=%.1f vstride=%.1f",
              s_minimap_scandots_tex_page, dot_u, dot_v, dot_v_stride);

    /* Build racer dot quads (up to racer_count) */
    uint8_t *dot_buf = s_minimap_quad_buf + 0x51F0;
    for (int r = 0; r < g_racer_count && r < 12; r++) {
        float row_v;
        if (r == g_actor_slot_map[0]) {
            row_v = dot_v; /* player: row 0 */
        } else if (r < 6) {
            row_v = dot_v + dot_v_stride; /* AI: row 1 */
        } else {
            row_v = dot_v + dot_v_stride * 2.0f; /* other: row 2 */
        }

        hud_build_quad(
            dot_buf + r * TD5_HUD_GLYPH_QUAD_SIZE,
            0, scandots->texture_page,
            0.0f, 0.0f, 0.0f, 0.0f,
            dot_u, row_v,
            dot_u + dot_v_stride, row_v + dot_v_stride,
            0xFFFFFFFF, HUD_DEPTH3
        );
    }

    /* Load semicol sprite (track segment connector dots) */
    TD5_AtlasEntry *semicol = td5_asset_find_atlas_entry(NULL, "semicol");
    float sc_u = (float)(semicol->atlas_x + 9);
    float sc_v = (float)(semicol->atlas_y + 1);

    /* Build track connector dot quads (0x30 segment dots) */
    uint8_t *seg_dot_buf = s_minimap_quad_buf + 0x5080;
    for (int i = 0; i < 3; i++) { /* 3 segment dot types */
        int off = 0x5080 + i * TD5_HUD_GLYPH_QUAD_SIZE;
        if (off + TD5_HUD_GLYPH_QUAD_SIZE <= TD5_HUD_MINIMAP_BUF_SIZE) {
            hud_build_quad(
                s_minimap_quad_buf + off,
                0, semicol->texture_page,
                0.0f, 0.0f, 0.0f, 0.0f,
                sc_u, sc_v, sc_u, sc_v,
                0xFFFFFFFF, HUD_DEPTH4
            );
        }
    }

    /* Load scanback sprite (background tile) */
    TD5_AtlasEntry *scanback = td5_asset_find_atlas_entry(NULL, "scanback");
    float bg_u0 = (float)scanback->atlas_x + 0.5f;
    float bg_v0 = (float)scanback->atlas_y + 0.5f;
    float bg_u1 = (float)(scanback->atlas_x + scanback->width) - 0.5f;
    float bg_v1 = (float)(scanback->atlas_y + scanback->height) - 0.5f;

    TD5_LOG_I(LOG_TAG, "minimap_init: scanback tex_page=%d u0=%.1f v0=%.1f u1=%.1f v1=%.1f",
              scanback->texture_page, bg_u0, bg_v0, bg_u1, bg_v1);

    /* Build 4x4 grid of background tiles.
     * td5_render_set_projection_center is a stub, so we bake the minimap
     * screen center into each tile quad directly (absolute screen coords). */
    float tile_w = s_minimap_tile_width * 0.25f;
    float tile_h = s_minimap_tile_height * 0.25f;
    float mm_cx = s_minimap_width * 0.5f + s_minimap_x;
    float mm_cy = s_minimap_height * 0.5f + s_minimap_y;

    for (uint32_t t = 0; t < 16; t++) {
        int col = (int)(t & 3);
        int row = (int)(t / 4);

        float tx0 = mm_cx + ((float)col * 0.25f - 0.5f) * s_minimap_tile_width;
        float ty0 = mm_cy + ((float)row * 0.25f - 0.5f) * s_minimap_tile_height;

        int off = 0x4500 + (int)t * TD5_HUD_GLYPH_QUAD_SIZE;
        if (off + TD5_HUD_GLYPH_QUAD_SIZE <= TD5_HUD_MINIMAP_BUF_SIZE) {
            /* Grid tile diffuse = 0xFFFFFFFF to match original literal
             * [CONFIRMED @ 0x0043b250..0x0043b2c0]. Semi-transparency comes
             * from the tpage slot-4 loader, which writes alpha=0x80 on all
             * non-black pixels (see load_static_png_tpage slot==4 case).
             * The previous 0x60FFFFFF diffuse multiplied down to final alpha
             * 0x30 and was discarded by the TRANSLUCENT_LINEAR alpha_ref=0x80
             * test, which made the grid entirely invisible. */
            hud_build_quad(
                s_minimap_quad_buf + off,
                0, scanback->texture_page,
                tx0, ty0,
                tx0 + tile_w, ty0 + tile_h,
                bg_u0, bg_v0, bg_u1, bg_v1,
                0xFFFFFFFF, HUD_DEPTH4
            );
        }
    }

    /* Build route segment table from track strip data */
    int seg_count = 0;
    int seg_start_val = 0;
    int span_idx = 0;

    if (g_strip_total_segments > 0) {
        uint8_t *span_base = (uint8_t *)g_strip_span_base;

        for (int s = 0; s < g_strip_total_segments; s++) {
            uint8_t span_type = span_base[(s + 1) * 24]; /* type of next span */

            if (span_type == 0x08) {
                /* Segment end */
                s_minimap_seg_start[seg_count] = (int16_t)seg_start_val;
                s_minimap_seg_end[seg_count]   = (int16_t)s;
                s_minimap_seg_branch[seg_count] = -1;
                seg_start_val = s + 1;
                seg_count++;
            } else if (span_type == 0x0B) {
                /* Branch connector */
                s_minimap_seg_start[seg_count] = (int16_t)seg_start_val;
                s_minimap_seg_end[seg_count]   = (int16_t)(s - 1);
                /* Read branch link from span data */
                int16_t link = *(int16_t *)(span_base + seg_start_val * 24 + 8);
                s_minimap_seg_branch[seg_count] = link;
                seg_start_val = s;
                seg_count++;
            }

            span_idx = s;
        }
    }

    /* Final segment */
    s_minimap_seg_start[seg_count] = (int16_t)seg_start_val;
    s_minimap_seg_end[seg_count]   = (int16_t)(g_strip_span_count - 2);
    s_minimap_seg_branch[seg_count] = -1;
    s_minimap_seg_primary_end = seg_count + 1;
    s_minimap_seg_branch_start = s_minimap_seg_primary_end;

    /* Expand branch segments into the table */
    int branch_write = s_minimap_seg_primary_end;
    for (int i = 0; i < s_minimap_seg_primary_end; i++) {
        if (s_minimap_seg_branch[i] != -1) {
            int16_t br = s_minimap_seg_branch[i];
            int16_t delta = s_minimap_seg_end[i] - s_minimap_seg_start[i];
            s_minimap_seg_end[branch_write - 1] = br; /* link end */
            s_minimap_seg_start[branch_write] = br + delta;
            s_minimap_seg_end[branch_write] = s_minimap_seg_start[i];
            branch_write++;
        }
    }
    s_minimap_seg_primary_end = branch_write;
}

/* ========================================================================
 * InitializePauseMenuOverlayLayout (0x43B7C0)
 *
 * Builds the pause menu overlay: background panel, selection highlight,
 * slider bars, option row backgrounds, and text glyphs.
 * ======================================================================== */

void td5_hud_init_pause_menu(int page_index)
{
    if (page_index >= TD5_HUD_PAUSE_MAX_PAGES) {
        page_index = 0;
    }

    /* Select page string table */
    extern const char **g_pause_page_strings[8]; /* 0x4744B8 */
    s_pause_menu_strings = g_pause_page_strings[page_index];

    /* Compute page-dependent half-width */
    extern const int g_pause_page_sizes[8]; /* 0x474498 */
    float page_size = (float)g_pause_page_sizes[page_index];
    s_pause_half_width = page_size * 0.5f;
    s_pause_quad_count = 0;
    memset(s_pause_quad_buf, 0, sizeof(s_pause_quad_buf));

    /* The original uses centered coords (origin = screen center).
     * Our pipeline uses pixel-space coords, so offset all positions by
     * the screen center so the panel appears in the middle of the screen. */
    float cx = g_render_width_f  * 0.5f;
    float cy = g_render_height_f * 0.5f;

#define PAUSE_BUF(n) (s_pause_quad_buf + (n) * 0xB8)
#define PAUSE_ADD(x0, y0, x1, y1, u0, v0, u1, v1, page, col) \
    do { \
        if (s_pause_quad_count < TD5_HUD_PAUSE_MAX_QUADS) { \
            hud_build_quad(PAUSE_BUF(s_pause_quad_count), 0, (page), \
                           cx + (x0), cy + (y0), cx + (x1), cy + (y1), \
                           (u0), (v0), (u1), (v1), (col), HUD_DEPTH); \
            s_pause_quad_count++; \
        } \
    } while (0)

    /* Look up atlas entries for pause overlay textures (all on tpage12) */
    TD5_AtlasEntry *blackbox_e = td5_asset_find_atlas_entry(NULL, "BLACKBOX");
    TD5_AtlasEntry *selbox_e   = td5_asset_find_atlas_entry(NULL, "SELBOX");
    TD5_AtlasEntry *blackbar_e = td5_asset_find_atlas_entry(NULL, "BLACKBAR");
    TD5_AtlasEntry *slider_e   = td5_asset_find_atlas_entry(NULL, "SLIDER");

    /* BLACKBOX: dark semi-transparent panel. y is fixed ±56.
     * From binary 0x43B7C0: single-texel sample.  Texture alpha (A=128 after
     * ARGB channel remap) provides semi-transparency naturally. */
    {
        float bu = (float)blackbox_e->atlas_x + 0.5f;
        float bv = (float)blackbox_e->atlas_y + 0.5f;
        PAUSE_ADD(-s_pause_half_width, -56.0f,
                   s_pause_half_width,  56.0f,
                   bu, bv, bu, bv,
                   blackbox_e->texture_page, 0xFFFFFFFF);
    }

    /* SELBOX: grayscale highlight bar (256x16 atlas texture).
     * From binary: x0=1-half_w, x1=half_w-1; cursor=3 default (CONTINUE). */
    s_pause_selbox_atlas = selbox_e;
    s_pause_sel_box = NULL;
    s_pause_selbox_base_y = -33.0f;  /* 0x0045D75C: selbox starts at row 1 (VIEW), not row 0 (PAUSED) */
    {
        float sel_x0 = 1.0f - s_pause_half_width;
        float sel_x1 = s_pause_half_width - 1.0f;
        s_pause_selbox_x0 = sel_x0;
        s_pause_selbox_x1 = sel_x1;
        s_pause_sel_box = (s_pause_quad_count < TD5_HUD_PAUSE_MAX_QUADS)
                          ? PAUSE_BUF(s_pause_quad_count) : NULL;
        float su0 = (float)selbox_e->atlas_x + 0.5f;
        float sv0 = (float)selbox_e->atlas_y + 0.5f;
        float su1 = su0 + 255.0f;
        float sv1 = sv0 + 15.0f;
        float init_y = s_pause_selbox_base_y + 3.0f * 16.0f;
        PAUSE_ADD(sel_x0, init_y, sel_x1, init_y + 16.0f,
                  su0, sv0, su1, sv1,
                  selbox_e->texture_page, 0xFFFFFFFF);
    }

    /* BLACKBAR (trough) + SLIDER (fill bar) using atlas textures.
     * From binary: bar x=[half_w-131, half_w-1], row N y=[N*16-29, N*16-21]. */
    s_pause_slider_atlas = slider_e;
    s_pause_bar_x0 = s_pause_half_width - 130.0f;  /* 0x0045D73C */
    s_pause_bar_x1 = s_pause_half_width - 1.0f;

    for (int row = 0; row < 3; row++) {
        float row_y = (float)row * 16.0f;
        /* Dark background trough — single-texel BLACKBAR */
        float bbu = (float)blackbar_e->atlas_x + 0.5f;
        float bbv = (float)blackbar_e->atlas_y + 0.5f;
        PAUSE_ADD(s_pause_bar_x0, row_y - 29.0f,
                  s_pause_bar_x1,  row_y - 21.0f,
                  bbu, bbv, bbu, bbv,
                  blackbar_e->texture_page, 0xFFFFFFFF);
        /* Slider fill bar — SLIDER atlas texture (256x8) */
        s_pause_slider_ptrs[row] = (s_pause_quad_count < TD5_HUD_PAUSE_MAX_QUADS)
                                    ? PAUSE_BUF(s_pause_quad_count) : NULL;
        float slu0 = (float)slider_e->atlas_x + 0.5f;
        float slu1 = (float)slider_e->atlas_x + 0.5f;  /* zero-width initially, updated per frame */
        float slv  = (float)slider_e->atlas_y + 0.5f;
        PAUSE_ADD(s_pause_bar_x0, row_y - 28.0f,
                  s_pause_bar_x0, row_y - 22.0f,
                  slu0, slv, slu1, slv,
                  slider_e->texture_page, 0xFFFFFFFF);
    }

    /* Build text glyphs from PAUSETXT atlas */
    TD5_AtlasEntry *pausetxt = td5_asset_find_atlas_entry(NULL, "PAUSETXT");
    hud_log_atlas_status("PAUSETXT", pausetxt);

    float text_y = -52.0f;
    int string_offset = 0;

    while (pausetxt && s_pause_menu_strings && string_offset < 0x30) {
        const char *str = s_pause_menu_strings[string_offset / 4];
        if (str == NULL) break;

        int alignment = *(int *)((uint8_t *)s_pause_menu_strings + string_offset + 4);
        int len = (int)strlen(str);

        float start_x;
        if (alignment == 2) {
            float total_w = 0.0f;
            for (int c = 0; c < len; c++) {
                uint8_t ch = (uint8_t)str[c];
                int glyph_w = (g_pause_glyph_widths[ch] * 2) / 3;
                total_w += (float)(glyph_w + 2);
            }
            start_x = -(total_w * 0.5f);
        } else {
            start_x = 4.0f - s_pause_half_width;
        }

        float cursor_x = start_x;
        /* Glyph UVs are absolute pixel coords in the 256x256 texture page —
         * the character code itself encodes the cell position (0x43BDB0).
         * Do NOT add atlas_x/atlas_y offset. */
        for (int c = 0; c < len; c++) {
            uint8_t ch = (uint8_t)str[c];
            int glyph_w = (g_pause_glyph_widths[ch] * 2) / 3;
            float glyph_u = (float)(ch & 0x0F) * 16.0f + 0.5f;
            float glyph_v = (float)(ch >> 4) * 16.0f + 0.5f;
            PAUSE_ADD(cursor_x + 0.5f, text_y + 0.5f,
                      cursor_x + (float)(glyph_w - 1) + 0.5f, text_y + 16.0f,
                      glyph_u, glyph_v,
                      glyph_u + (float)(glyph_w - 1), glyph_v + 16.0f,
                      pausetxt->texture_page, 0xFFFFFFFF);
            cursor_x += (float)(glyph_w + 2);
        }

        text_y += 16.0f;
        string_offset += 8;
        if (string_offset > 0x2F) break;
    }
#undef PAUSE_ADD
#undef PAUSE_BUF

    TD5_LOG_I(LOG_TAG,
              "pause menu: page=%d items=%d quads=%d half_w=%.0f "
              "pausetxt_atlas=(%d,%d) selbox_base_y=%.0f",
              page_index, string_offset / 8, s_pause_quad_count,
              s_pause_half_width,
              pausetxt ? pausetxt->atlas_x : -1,
              pausetxt ? pausetxt->atlas_y : -1,
              s_pause_selbox_base_y);
}

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_hud_init(void)
{
    s_scale_x = 1.0f;
    s_scale_y = 1.0f;
    s_view_count = 1;
    s_cur_view = 0;
    s_queued_glyph_count = 0;

    s_hud_prim_storage = NULL;
    s_minimap_quad_buf = NULL;
    s_glyph_table = NULL;
    s_text_quad_buf = NULL;

    memset(s_wrong_way_counter, 0, sizeof(s_wrong_way_counter));
    memset(s_prev_span_pos, 0, sizeof(s_prev_span_pos));
    memset(s_indicator_state, 0, sizeof(s_indicator_state));

    s_radial_pulse_progress = -1.0f;

    TD5_LOG_I("hud", "HUD module initialized");
    return 1;
}

void td5_hud_shutdown(void)
{
    /* Resources are freed by the game heap reset; no individual frees needed. */
    s_hud_prim_storage = NULL;
    s_minimap_quad_buf = NULL;
    s_glyph_table = NULL;
    s_text_quad_buf = NULL;

    TD5_LOG_I("hud", "HUD module shut down");
}

void td5_hud_render(void)
{
    /* Entry point called from the main render frame.
     * Delegates to td5_hud_render_overlays with normalized 30 Hz frame dt. */
    td5_hud_render_overlays(g_td5.normalized_frame_dt);
}

/* ========================================================================
 * Race End Fade — Directional screen wipe (0x43E750 SetClipBounds)
 *
 * Renders black bars that close in from opposite screen edges.
 * progress: 0.0 = no coverage, 255.0 = full screen black.
 * direction: 0 = horizontal (left/right), 1 = vertical (top/bottom).
 * Uses the pre-built FADEWHT fade quads (s_fade_quads[0..1]).
 * ======================================================================== */

void td5_hud_draw_race_fade(float progress, int direction)
{
    if (progress <= 0.0f) return;

    float frac = progress / 255.0f;
    if (frac > 1.0f) frac = 1.0f;

    float sw = g_render_width_f;
    float sh = g_render_height_f;

    /* Two bars closing toward center. At frac=1.0 they each cover half
     * the screen, meeting in the middle for full black. */
    float bar_size;
    float x0a, y0a, x1a, y1a;  /* bar A */
    float x0b, y0b, x1b, y1b;  /* bar B */

    if (direction == 0) {
        /* Horizontal: bars from left and right edges */
        bar_size = sw * 0.5f * frac;
        x0a = 0.0f;      y0a = 0.0f;  x1a = bar_size;        y1a = sh;
        x0b = sw - bar_size; y0b = 0.0f;  x1b = sw;           y1b = sh;
    } else {
        /* Vertical: bars from top and bottom edges */
        bar_size = sh * 0.5f * frac;
        x0a = 0.0f; y0a = 0.0f;           x1a = sw; y1a = bar_size;
        x0b = 0.0f; y0b = sh - bar_size;  x1b = sw; y1b = sh;
    }

    /* Draw two opaque black bars directly via platform render.
     * Cannot use hud_submit_quad — it routes through submit_translucent
     * which uses alpha blending, making the bars see-through. */
    if (s_fadewht_atlas) {
        int ftex = s_fadewht_atlas->texture_page;
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims(ftex, &tw, &th);
        float fu = ((float)s_fadewht_atlas->atlas_x + 0.5f) / (float)tw;
        float fv = ((float)s_fadewht_atlas->atlas_y + 0.5f) / (float)th;

        TD5_D3DVertex verts[4];
        uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

        td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        td5_plat_render_bind_texture(ftex);

        /* Bar A */
        verts[0] = (TD5_D3DVertex){ x0a, y0a, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[1] = (TD5_D3DVertex){ x0a, y1a, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[2] = (TD5_D3DVertex){ x1a, y1a, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[3] = (TD5_D3DVertex){ x1a, y0a, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        td5_plat_render_draw_tris(verts, 4, indices, 6);

        /* Bar B */
        verts[0] = (TD5_D3DVertex){ x0b, y0b, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[1] = (TD5_D3DVertex){ x0b, y1b, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[2] = (TD5_D3DVertex){ x1b, y1b, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        verts[3] = (TD5_D3DVertex){ x1b, y0b, 0.0f, 1.0f, 0xFF000000, 0, fu, fv };
        td5_plat_render_draw_tris(verts, 4, indices, 6);
    }
}
