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
 * External game state references
 *
 * These map to original global variables. In the final source port they
 * live in the appropriate module's state; here we reference them via
 * extern declarations matching the original memory layout.
 * ======================================================================== */

/* Race configuration (0x4AAF64..0x4AAF8C) */
extern int     g_replay_mode;            /* 0x4AAF64 */
extern int     g_wanted_mode_enabled;    /* 0x4AAF68 */
extern int     g_special_encounter;      /* 0x4AAF6C */
extern int     g_race_rule_variant;      /* 0x4AAF70 */
extern int     g_game_type;              /* 0x4AAF74: TD5_GameType */
extern int     g_split_screen_mode;      /* 0x4AAF89: byte, viewport layout */
extern int     g_racer_count;            /* 0x4AAF00: total racers in race */
extern float   g_render_width_f;         /* 0x4AAF08 */
extern float   g_render_height_f;        /* 0x4AAF0C */
extern int     g_render_width;           /* 0x4AAF10 */
extern int     g_render_height;          /* 0x4AAF14 */
extern int     g_track_is_circuit;       /* 0x466E94 */
extern int     g_track_type_mode;        /* 0x4AAEF8 */
extern int     g_hud_metric_mode;        /* 0x473E30 */
extern float   g_instant_fps;            /* 0x466E90 */
extern uint32_t g_tick_counter;          /* 0x4AADA0 */
extern int     g_kph_mode;              /* 0x4B11C4: 0=MPH, 1=KPH */

/* Actor data base pointers (0x4AB2C4..0x4AB47D) */
extern int     g_actor_slot_map[2];      /* 0x466EA0: per-view actor index */
extern void   *g_actor_pool;             /* base of actor array */

/* Track strip data pointers */
extern int     g_strip_span_count;       /* 0x4C3D90 */
extern int     g_strip_total_segments;   /* 0x4C3D94 */
extern void   *g_strip_span_base;        /* 0x4C3D9C */
extern void   *g_strip_vertex_base;      /* 0x4C3D98 */

/* Checkpoint data */
extern uint16_t *g_checkpoint_array;     /* 0x4AED88 */

/* String tables */
extern const char **g_position_strings;  /* 0x473E38: "1ST".."6TH", labels */
extern const char **g_wanted_msg_line1;  /* 0x474038 */
extern const char **g_wanted_msg_line2;  /* 0x47403C */

/* Wanted state */
extern int     g_wanted_msg_timer;       /* 0x4BF50C */
extern int     g_wanted_msg_index;       /* 0x4BF508 */

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
     * Static atlas pages are 256x256 BGRA32. The original engine uses
     * 1/256 for both axes (confirmed from BuildSpriteQuadTemplate @ 0x432BD0,
     * constant at [0x4749D0] = 0.00390625 = 1/256). Solid-color 1x1 pages
     * work correctly with any UV under WRAP mode. */
    u0 /= 256.0f; v0 /= 256.0f;
    u1 /= 256.0f; v1 /= 256.0f;

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
    return a[0x18B]; /* race position byte */
}

static inline uint8_t actor_gear(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return a[0x36B]; /* current gear */
}

static inline int32_t actor_speed_fp(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int32_t *)(a + 0x314); /* longitudinal_speed 24.8 fixed-point */
}

/* Returns the top speed in the same fixed-point units as longitudinal_speed.
 * tuning+0x74 is the speed_limiter raw value; physics uses it as << 8. */
static inline int32_t actor_max_speed_fp(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    void *tuning = *(void **)(a + 0x1BC);
    if (!tuning) return 0x8000;
    return (int32_t)(*(int16_t *)((uint8_t *)tuning + 0x74)) << 8;
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
     * tpage5.dat is not extracted from the original game data, so we build
     * a 256x512 BGRA atlas page and render the 64-glyph 4x16 grid into it
     * at the correct atlas offset (atlas_x=96, atlas_y=192, size=160x64).
     *
     * The glyph table stores pixel UV coordinates; hud_build_quad normalizes
     * them by 256/512 before submission to D3D11. */
    if (font_entry->texture_page > 0) {
        /* 256x256 BGRA = 256 KB; static to avoid stack overflow.
         * Atlas pages are 256x256 (confirmed by UV scale = 1/256 for both axes). */
        static uint8_t s_font_page_buf[256 * 256 * 4];
        memset(s_font_page_buf, 0, sizeof(s_font_page_buf));

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
             * White GDI text → full alpha; black background → transparent. */
            const uint8_t *src = (const uint8_t *)dib_bits;
            int ay = font_entry->atlas_y; /* 192 */
            for (int y = 0; y < 64; y++) {
                for (int x = 0; x < 256; x++) {
                    int si = (y * 256 + x) * 4;
                    uint8_t b = src[si + 0];
                    uint8_t g_ch = src[si + 1];
                    uint8_t r = src[si + 2];
                    /* Luminance as alpha (integer approximation of 0.299R+0.587G+0.114B) */
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

        td5_plat_render_upload_texture(font_entry->texture_page,
                                       s_font_page_buf, 256, 256, 2);
    }

    /* Upload 1x1 white texture for solid-color overlays (pause dimmer) */
    if (!s_hud_white_tex_uploaded) {
        static const uint32_t k_white = 0xFFFFFFFF;
        s_hud_white_tex_uploaded = td5_plat_render_upload_texture(
            HUD_WHITE_TEX_PAGE, &k_white, 1, 1, 2);
    }
}

/* ========================================================================
 * Draw a full-screen semi-transparent dimmer overlay (for pause menu).
 * Uses a 1x1 white texture modulated by vertex color.
 * ======================================================================== */

void td5_hud_draw_pause_overlay(void)
{
    static uint8_t s_dimmer_quad[0xB8];
    int i;

    if (!s_hud_white_tex_uploaded) return;

    /* Semi-transparent black dimmer over the whole screen */
    hud_build_quad(
        s_dimmer_quad,
        0,
        HUD_WHITE_TEX_PAGE,
        0.0f, 0.0f,
        (float)g_render_width, (float)g_render_height,
        0.0f, 0.0f,
        0.5f, 0.5f,
        0xA0000000,   /* semi-transparent black (BGRA) */
        HUD_DEPTH
    );
    hud_submit_quad(s_dimmer_quad);

    /* Submit pre-built pause menu panel, selection, sliders, and text */
    for (i = 0; i < s_pause_quad_count && i < TD5_HUD_PAUSE_MAX_QUADS; i++) {
        hud_submit_quad(s_pause_quad_buf + i * 0xB8);
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
        if (g_game_type == 2 || g_game_type == 0 || g_game_type == 1) {
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
            gear_x + sx * 32.0f, gear_y + sy * 16.0f,
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
    if (g_special_encounter != 0) {
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
            int32_t speed_fp = actor_speed_fp(actor_slot);
            int32_t max_speed = actor_max_speed_fp(actor_slot);

            uint32_t needle_angle;
            if (max_speed > 0) {
                needle_angle = (uint32_t)((speed_fp * 0xA5A) / max_speed) + 0x400;
            } else {
                needle_angle = 0x400;
            }

            float cos_a = td5_cos_12bit(needle_angle);
            float sin_a = td5_sin_12bit(needle_angle);

            /* Needle center is at speedometer center */
            float cx = vl->vp_int_right - sx * 64.0f;
            float cy = vl->vp_int_bottom - sy * 56.0f;

            /* Needle tip (45 units from center) and base (9 units back) */
            float tip_x = cx - cos_a * sx * 45.0f;
            float tip_y = cy - sin_a * sy * 45.0f;

            float base_offset_x = sin_a * sx * 2.0f;
            float base_offset_y = cos_a * sy * 2.0f;

            float base_x = cx + cos_a * sx * 9.0f;
            float base_y = cy + sin_a * sy * 9.0f;

            /* Build needle quad (4 vertices: tip, left-base, center-back, right-base) */
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
            needle_params.x[0] = tip_x;
            needle_params.x[1] = base_x - base_offset_x;
            needle_params.x[2] = base_x + base_offset_x;
            needle_params.x[3] = base_x;
            needle_params.y[0] = tip_y;
            needle_params.y[1] = base_y + base_offset_y;
            needle_params.y[2] = base_y - base_offset_y;
            needle_params.y[3] = base_y;
            for (int i = 0; i < 4; i++) needle_params.depth[i] = HUD_DEPTH;
            memset(needle_params.u, 0, sizeof(needle_params.u));
            memset(needle_params.v, 0, sizeof(needle_params.v));
            for (int i = 0; i < 4; i++) needle_params.color[i] = 0xFFFFFFFF;
            needle_params.tex = 0;
            needle_params.pad = 0;

            td5_render_build_sprite_quad((int *)&needle_params);

            /* Update gear indicator UV based on current gear */
            uint8_t gear = actor_gear(actor_slot);
            float gear_u = (float)gear * sx * 16.0f + (float)s_gearnumbers_atlas->atlas_x;
            float gear_v = (float)s_gearnumbers_atlas->atlas_y;

            /* Compute speed value and convert to MPH or KPH */
            int32_t speed_raw = speed_fp;
            if (speed_raw < 0) speed_raw = 0;
            speed_raw >>= 8;

            int speed_display;
            if (g_kph_mode == 0) {
                speed_display = (speed_raw * 256 + 625) / 1252; /* MPH */
            } else {
                speed_display = (speed_raw * 256 + 389) / 778;  /* KPH */
            }

            /* Update speed digit UVs and submit */
            /* Ones digit */
            float digit_u_base = (float)s_speedofont_atlas->atlas_x;
            float digit_v_base = (float)s_speedofont_atlas->atlas_y;
            float digit_w = 16.0f;

            int ones = speed_display % 10;
            float u_ones = digit_u_base + (float)ones * digit_w + 0.5f;

            /* Submit speedo dial */
            hud_submit_quad(view_base + SPEEDO_QUAD_OFF);
            /* Submit needle */
            hud_submit_quad(view_base + 0x39C);

            /* Submit speed digits (ones always shown) */
            int digit_count = 1;
            int tens = (speed_display % 1000) / 10;

            /* Submit ones digit quad */
            hud_build_quad(
                view_base + SPEEDFONT_BASE_OFF,
                2, s_speedofont_atlas->texture_page,
                0.0f, 0.0f, 0.0f, 0.0f,
                u_ones, digit_v_base + 0.5f,
                u_ones + 15.5f, digit_v_base + 23.5f,
                0xFFFFFFFF, HUD_DEPTH
            );
            hud_submit_quad(view_base + SPEEDFONT_BASE_OFF);

            /* Tens digit if speed >= 10 */
            if (tens > 0) {
                digit_count = 2;
                float u_tens = digit_u_base + (float)(tens % 10) * digit_w + 0.5f;
                hud_build_quad(
                    view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE,
                    2, s_speedofont_atlas->texture_page,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    u_tens, digit_v_base + 0.5f,
                    u_tens + 15.5f, digit_v_base + 23.5f,
                    0xFFFFFFFF, HUD_DEPTH
                );
                hud_submit_quad(view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE);

                /* Hundreds digit if speed >= 100 */
                int hundreds = tens / 10;
                if (hundreds > 0) {
                    digit_count = 3;
                    float u_hund = digit_u_base + (float)hundreds * digit_w + 0.5f;
                    hud_build_quad(
                        view_base + SPEEDFONT_BASE_OFF + 2 * TD5_HUD_GLYPH_QUAD_SIZE,
                        2, s_speedofont_atlas->texture_page,
                        0.0f, 0.0f, 0.0f, 0.0f,
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
        if (s_indicator_state[v] != 0) {
            int digit_val = s_indicator_state[v]; /* state 3,2,1 → shows "3","2","1" */
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

    /* Radial pulse effect */
    td5_render_radial_pulse(dt);

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

    /* Walk track spans and render road segments */
    int16_t player_span = actor_span_index(actor_slot);
    int start_span = ((int)player_span / 24 - 6) * 24;
    if (start_span < 0) start_span = 0;

    /* Simplified: iterate visible spans around the player */
    uint8_t *span_base = (uint8_t *)g_strip_span_base;
    uint8_t *vert_base = (uint8_t *)g_strip_vertex_base;
    int dot_count = 0;
    TD5_SpriteQuad map_quad;

    for (int i = 0; span_base && vert_base &&
             i < 0x30 && (int)(start_span + i * 5) < g_strip_span_count - 2; i++) {
        int span_a = start_span + i * 5;
        int span_b = span_a + 5;
        if (span_b >= g_strip_span_count) span_b = g_strip_span_count - 2;

        /* Read span origins and vertex positions */
        uint8_t *sa = span_base + span_a * 24;
        uint8_t *sb = span_base + span_b * 24;

        int32_t ox_a = *(int32_t *)(sa + 0x0C);
        int32_t oz_a = *(int32_t *)(sa + 0x14);
        uint16_t vi_a = *(uint16_t *)(sa + 4);

        int16_t *va = (int16_t *)(vert_base + vi_a * 6);
        float wx0 = (float)((int)va[0] + ox_a) * kFP + offset_x;
        float wz0 = (float)((int)va[2] + oz_a) * kFP + offset_z;

        /* Transform through player heading rotation */
        float mx0 = (wx0 * cos_h + wz0 * sin_h) * s_minimap_world_scale_x;
        float my0 = (wz0 * cos_h - wx0 * sin_h) * s_minimap_world_scale_y;

        /* Second span corner */
        int32_t ox_b = *(int32_t *)(sb + 0x0C);
        int32_t oz_b = *(int32_t *)(sb + 0x14);
        uint16_t vi_b = *(uint16_t *)(sb + 6);

        int16_t *vb = (int16_t *)(vert_base + vi_b * 6);
        float wx1 = (float)((int)vb[0] + ox_b) * kFP + offset_x;
        float wz1 = (float)((int)vb[2] + oz_b) * kFP + offset_z;

        float mx1 = (wx1 * cos_h + wz1 * sin_h) * s_minimap_world_scale_x;
        float my1 = (wz1 * cos_h - wx1 * sin_h) * s_minimap_world_scale_y;

        /* Build a quad for this road segment (absolute screen coords) */
        hud_build_quad(
            &map_quad,
            1, 0, /* mode 1: no texture, solid color */
            mm_cx + mx0 - 1.0f, mm_cy + my0 - 1.0f,
            mm_cx + mx1 + 1.0f, mm_cy + my1 + 1.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
            0xFF404040, /* dark gray road */
            HUD_DEPTH
        );
        hud_submit_quad(&map_quad);
    }

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

            hud_build_quad(
                &map_quad,
                1, 0,
                mm_cx + dmx - half_dot, mm_cy + dmy - half_dot,
                mm_cx + dmx + half_dot, mm_cy + dmy + half_dot,
                0.0f, 0.0f, 0.0f, 0.0f,
                (r == g_actor_slot_map[0]) ? 0xFFFF0000 : 0xFFFFFF00,
                HUD_DEPTH
            );
            hud_submit_quad(&map_quad);
            dot_count++;
        }
    }

    TD5_LOG_D(LOG_TAG,
              "minimap: actor=%d span_range=[%d,%d] dots=%d",
              actor_slot, start_span,
              (start_span + 0x30 * 5 < g_strip_span_count) ? (start_span + 0x30 * 5) : g_strip_span_count,
              dot_count);
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

    /* Build BLACKBOX (background panel) */
    TD5_AtlasEntry *blackbox = td5_asset_find_atlas_entry(NULL, "BLACKBOX");
    hud_log_atlas_status("BLACKBOX", blackbox);
    if (blackbox) {
        PAUSE_ADD(-s_pause_half_width, -s_pause_half_width,
                   s_pause_half_width,  s_pause_half_width,
                   (float)blackbox->atlas_x + 0.5f,
                   (float)blackbox->atlas_y + 0.5f,
                   (float)(blackbox->atlas_x + blackbox->width)  - 0.5f,
                   (float)(blackbox->atlas_y + blackbox->height) - 0.5f,
                   blackbox->texture_page, 0xFFFFFFFF);
    }

    /* Build SELBOX (selection highlight) */
    TD5_AtlasEntry *selbox = td5_asset_find_atlas_entry(NULL, "SELBOX");
    hud_log_atlas_status("SELBOX", selbox);
    s_pause_sel_box = NULL;
    if (selbox) {
        float sel_x0 = 1.0f - s_pause_half_width;
        float sel_x1 = s_pause_half_width - 1.0f;
        PAUSE_ADD(sel_x0, -1.0f, sel_x1, 15.0f,
                  (float)selbox->atlas_x + 0.5f,
                  (float)selbox->atlas_y + 0.5f,
                  (float)selbox->atlas_x + 20.0f,
                  (float)selbox->atlas_y + 12.0f,
                  selbox->texture_page, 0xFFFFFFFF);
    }

    /* Build SLIDER and BLACKBAR rows for each menu option */
    s_pause_slider_atlas = td5_asset_find_atlas_entry(NULL, "SLIDER");
    TD5_AtlasEntry *blackbar = td5_asset_find_atlas_entry(NULL, "BLACKBAR");
    hud_log_atlas_status("SLIDER", s_pause_slider_atlas);
    hud_log_atlas_status("BLACKBAR", blackbar);

    for (int row = 0; row < 3; row++) {
        float row_y = (float)row * 16.0f;
        if (blackbar) {
            PAUSE_ADD(s_pause_half_width - 10.0f, row_y - 8.0f,
                      s_pause_half_width -  1.0f, row_y - 2.0f,
                      (float)blackbar->atlas_x + 0.5f,
                      (float)blackbar->atlas_y + 0.5f,
                      (float)blackbar->atlas_x + 0.5f,
                      (float)blackbar->atlas_y + 0.5f,
                      blackbar->texture_page, 0xFFFFFFFF);
        }
        if (s_pause_slider_atlas) {
            s_pause_slider_ptrs[row] = (s_pause_quad_count < TD5_HUD_PAUSE_MAX_QUADS)
                                        ? PAUSE_BUF(s_pause_quad_count) : NULL;
            PAUSE_ADD(s_pause_half_width - 8.0f, row_y - 6.0f,
                      s_pause_half_width - 2.0f, row_y - 4.0f,
                      (float)s_pause_slider_atlas->atlas_x + 20.0f,
                      (float)s_pause_slider_atlas->atlas_y + 0.5f,
                      (float)s_pause_slider_atlas->atlas_x + 0.5f,
                      (float)s_pause_slider_atlas->atlas_y + 0.5f,
                      s_pause_slider_atlas->texture_page, 0xFFFFFFFF);
        }
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
              "pause menu: page=%d item_count=%d theme=page_%d",
              page_index, string_offset / 8, page_index);
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
     * Delegates to td5_hud_render_overlays with the current frame dt. */
    td5_hud_render_overlays(g_td5.normalized_frame_dt);
}
