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
#include "td5_game.h"
#include "td5_save.h"
#include "td5_physics.h"
#include "td5_ai.h"
#include "td5re.h"
#include "td5_vectorui.h"   /* resolution-independent VectorUI primitives (SDF gauge, text) */
#include "td5_font.h"       /* shared native menu TTF glyph cache (pause-menu text) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define LOG_TAG "hud"

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

/* Cop-chase "wanted" dispatch two-liners. [FIX 2026-05-30 cop-chase]
 * The original has 8 message PAIRS in an interleaved pointer table at
 * 0x474038 (line1 base) / 0x47403C (line2 base = +4, i.e. one pointer into
 * the same array), each read with stride index*2. DrawRaceStatusText @
 * 0x00439BC8 selects index = rand() & 7 on first contact with a new suspect.
 * The previous port had invented placeholders ("YOU ARE / WANTED!") and only
 * 2 valid entries (rand()%2). Real strings transcribed from the binary table
 * (cross-checked re/sessions/2026-03-20-cop-chase-mode-analysis.md). */
static const char *s_wanted_msg_table[16] = {
    /* idx0 */ "SUSPECT IS WANTED FOR", "ARMED ROBBERY.",
    /* idx1 */ "SUSPECT IS WANTED FOR", "SPEEDING.",
    /* idx2 */ "SUSPECT IS ARMED",      "AND DANGEROUS.",
    /* idx3 */ "SUSPECT IS WANTED",     "FOR 1ST DEGREE MURDER.",
    /* idx4 */ "SUSPECT IS WANTED",     "FOR ILLEGAL LICENSE PLATES.",
    /* idx5 */ "SUSPECT IS WANTED",     "FOR GRAND THEFT AUTO.",
    /* idx6 */ "SUSPECT IS WANTED",     "FOR CHICKEN PLUCKING.",
    /* idx7 */ "SUSPECT IS WANTED",     "FOR SOFTWARE PIRACY.",
};
/* line1 = table[index*2], line2 = table[index*2 + 1] — mirrors the orig's
 * two base pointers 4 bytes apart, both indexed by index*2. */
const char **g_wanted_msg_line1 = &s_wanted_msg_table[0];
const char **g_wanted_msg_line2 = &s_wanted_msg_table[1];

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
 * [PORT REWORK 2026-06-05 / S15] The original had 6 entries
 * (PAUSED, VIEW/MUSIC/SOUND, CONTINUE/EXIT). Per user feedback the MUSIC
 * slider row is removed (music is silenced while paused, see td5_game.c
 * pause-enter), and two action rows are added: RESTART RACE (re-run the same
 * race) and EXIT GAME (clean app shutdown). The old "EXIT" (return to menu)
 * is relabelled "QUIT TO MENU" to disambiguate it from EXIT GAME.
 * New layout — title + 6 selectable rows (cursor indices in parentheses):
 *   PAUSED(title) / VIEW(0) SOUND(1) [sliders] /
 *   CONTINUE(2) RESTART RACE(3) QUIT TO MENU(4) EXIT GAME(5).
 * This is source-port-only UI (no original pause-restart/exit-game equiv). */
static const char *s_eng_pause_strings[] = {
    "PAUSED",         (const char *)(intptr_t)2,
    "VIEW",           (const char *)(intptr_t)0,
    "SOUND",          (const char *)(intptr_t)0,
    "CONTINUE",       (const char *)(intptr_t)2,
    "RESTART RACE",   (const char *)(intptr_t)2,
    "QUIT TO MENU",   (const char *)(intptr_t)2,
    "EXIT GAME",      (const char *)(intptr_t)2,
    NULL
};
const char **g_pause_page_strings[8] = {
    s_eng_pause_strings, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
const int g_pause_page_sizes[8] = { 256, 0, 0, 0, 0, 0, 0, 0 };

/* ========================================================================
 * Module-local state
 * ======================================================================== */

#define MAX_HUD_VIEWS       TD5_MAX_VIEWPORTS   /* PORT: N-way split (was 2) */
#define SPEEDO_QUAD_OFF     0x04       /* offset into view storage for speedo */
#define NEEDLE_QUAD_OFF     0x39C      /* needle quad offset */
#define GEAR_QUAD_OFF       0x0BC      /* gear indicator offset */
#define SPEEDFONT_BASE_OFF  0x174      /* speed font first digit offset */
#define HUD_VIEW_BLOCK      0xC00      /* per-view HUD primitive block stride
                                        * [PORT: N-way split]. Quads occupy
                                        * +0x04..~0x8A4; 0xC00 leaves generous
                                        * margin so a view's quads can't spill into
                                        * the next view's block. Flag words are now
                                        * held separately in s_hud_flag_words[]. */

/* Per-view HUD primitive storage (0x1148 bytes each) */
static uint8_t *s_hud_prim_storage;      /* 0x4B0C00 */
static uint32_t *s_hud_flags[MAX_HUD_VIEWS]; /* per-view visibility bitmask ptrs */
/* [PORT: N-way] Dedicated flag-word storage. The flag word originally lived at
 * the start of each view's quad block inside s_hud_prim_storage, but per-view
 * quad builds spill past their block and clobber the NEXT view's flag word
 * (observed as float screen-coords 0x431C4000=156.0f in the mask -> views 1..N
 * rendered no HUD). Keeping the words in their own array decouples them from the
 * quad storage so every view always reads a valid visibility mask. */
static uint32_t s_hud_flag_words[MAX_HUD_VIEWS];

/* Active view pointers (set per iteration in render loop) */
static uint32_t *s_cur_flags;            /* 0x4B0BFC */
static float    *s_cur_scale;            /* 0x4B0FA4 */

/* Per-view layouts */
static TD5_HudViewLayout s_view_layout[MAX_HUD_VIEWS];

/* [PORT 2026-06-08] Per-racer-slot player identity for the in-race overlays:
 * a coloured frame around each viewport + a name indicator under the car, so
 * everyone can see who is driving which pane. Set by the multiplayer frontend
 * at race start; cleared for single-player races. */
static char     s_hud_id_name[TD5_MAX_RACER_SLOTS][16];
static uint32_t s_hud_id_accent[TD5_MAX_RACER_SLOTS];
static int      s_hud_id_active;

void td5_hud_clear_player_identities(void)
{
    memset(s_hud_id_name, 0, sizeof(s_hud_id_name));
    s_hud_id_active = 0;
}

void td5_hud_set_player_identity(int slot, const char *name, uint32_t rgb)
{
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    if (name) { strncpy(s_hud_id_name[slot], name, sizeof(s_hud_id_name[slot]) - 1);
                s_hud_id_name[slot][sizeof(s_hud_id_name[slot]) - 1] = '\0'; }
    else        s_hud_id_name[slot][0] = '\0';
    s_hud_id_accent[slot] = (rgb & 0x00FFFFFFu) | 0xFF000000u;
    s_hud_id_active = 1;
}

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

/* [DA-T5 audit 2026-05-22] Accessors for td5_render_radial_pulse so the
 * renderer can read/update the phase without exposing the static. */
float td5_hud_radial_pulse_get(void)        { return s_radial_pulse_progress; }
void  td5_hud_radial_pulse_set(float value) { s_radial_pulse_progress = value; }

/* [CONFIRMED @ 0x0043a210 ResetHudRadialPulseOverlay; Tier 4 port 2026-05-24]
 * Resets the radial pulse overlay so the effect starts from the beginning
 * on the next trigger. Orig is a 10-byte stub: MOV [_g_hudRadialPulsePhase], 0.
 * Note: orig writes 0 (which starts the pulse animation from radius=0,
 * alpha=0 ramping up); port's td5_hud_init uses -1.0f to mean "inactive /
 * suppressed". After this reset is called the pulse becomes visible. Called
 * by orig RunRaceFrame @ 0x0042b687 and BeginRaceFadeOutTransition @
 * 0x0042cc6f -- both wired in the port at td5_game.c. */
void td5_hud_reset_radial_pulse(void) { s_radial_pulse_progress = 0.0f; }

/* [REMOVED 2026-06-05] The TD6 synthesized-checkpoint "CHECKPOINT n/N" flash
 * acknowledgement (s_td6_cp_flash_until/reached/total + td5_hud_set_td6_checkpoint_flash)
 * was removed along with its draw block in td5_hud_render_overlays — the user did
 * not want the on-screen checkpoint indicator. The split-time registration in
 * td5_game.c stays; it simply no longer pokes the HUD. */

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

/* Minimap route segment tables. Original is [64] (TD5 tracks stay under it), but
 * migrated TD6 tracks have many more junctions (London: 56 primary + ~28 branch
 * rows = ~85), which overflowed [64] and corrupted the adjacent branch_start/
 * primary_end statics (garbage 146M) -> incoherent/invisible minimap. Enlarged +
 * bounds-checked below. */
#define MINIMAP_SEG_MAX 256
static int   s_minimap_seg_primary_end;  /* 0x4B0A58 */
static int   s_minimap_seg_branch_start; /* 0x4B0A5C */
static int16_t s_minimap_seg_start[MINIMAP_SEG_MAX];  /* 0x4B0A70 */
static int16_t s_minimap_seg_end[MINIMAP_SEG_MAX];    /* 0x4B0A72 */
static int16_t s_minimap_seg_branch[MINIMAP_SEG_MAX]; /* 0x4B0A74 */

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
/* The pause panel/selbox/slider quads bake the screen centre (cx,cy) into their
 * absolute coords at init time, but the menu TEXT + the per-frame selbox/slider
 * updates recompute the centre live from g_render_width_f/height_f. If the render
 * dimensions change after the bake (drag-resize / maximize / a race that inits at
 * different dims), the baked panel drifts off the live-centred text — the pause
 * layout looks distorted and the dark backing panel sits displaced. Track the
 * baked dims + page so the draw/update path can re-bake when they change. */
static int   s_pause_page_index;
static float s_pause_baked_w;
static float s_pause_baked_h;

/* VectorUI: pause-menu text lines recorded at init (instead of baking PAUSETXT
 * glyph quads) and rendered crisply via the pause-font SDF at draw time. */
typedef struct { float y; int alignment; char s[48]; } PauseTextLine;
static PauseTextLine s_pause_vui_lines[16];
static int s_pause_vui_line_count;

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

/* Helper: submit a pre-built sprite quad.
 * Routes through td5_render_submit_translucent_hud (alpha_ref=1) to match orig
 * M2DX DXD3D::SetRenderState @ M2DX.dll 0x10001770 ALPHAREF=0/NOTEQUAL — orig
 * discards only fully-transparent pixels. The non-HUD TRANSLUCENT_LINEAR path
 * keeps its 0x80 cutoff for bilinear-fringe pruning on world props. */
static void hud_submit_quad(void *quad_data)
{
    td5_render_submit_translucent_hud((uint16_t *)quad_data);
}

/* Helper: build a warped 4-corner quad.
 *
 * Corner ordering MUST match td5_render_build_sprite_quad's slot mapping.
 * The render builder maps input slots to output verts as:
 *   dst->v0 = scr[0]   dst->v1 = scr[3]
 *   dst->v3 = scr[1]   dst->v2 = scr[2]
 * and the index buffer [0,1,2, 0,2,3] produces tris (v0,v1,v2) + (v0,v2,v3).
 * To get a consistent 4-corner quad, pass slots as TL, BL, BR, TR
 * (front-left, back-left, back-right, front-right in minimap terms).
 */
static void hud_build_quad_warped(void *dest, int tex_page,
                                   float tl_x, float tl_y,
                                   float bl_x, float bl_y,
                                   float br_x, float br_y,
                                   float tr_x, float tr_y,
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

    p.scr_x[0] = tl_x; p.scr_y[0] = tl_y; /* slot 0 → dst.v0 = TL */
    p.scr_x[1] = bl_x; p.scr_y[1] = bl_y; /* slot 1 → dst.v3 = BL */
    p.scr_x[2] = br_x; p.scr_y[2] = br_y; /* slot 2 → dst.v2 = BR */
    p.scr_x[3] = tr_x; p.scr_y[3] = tr_y; /* slot 3 → dst.v1 = TR */

    p.depth_z[0] = depth; p.depth_z[1] = depth;
    p.depth_z[2] = depth; p.depth_z[3] = depth;

    {
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims(tex_page, &tw, &th);
        u0 /= (float)tw; v0 /= (float)th;
        u1 /= (float)tw; v1 /= (float)th;
    }

    /* UV assignment mirrors hud_build_quad's axis-aligned slot pattern. */
    p.tex_u[0] = u0; p.tex_v[0] = v0;
    p.tex_u[1] = u0; p.tex_v[1] = v1;
    p.tex_u[2] = u1; p.tex_v[2] = v1;
    p.tex_u[3] = u1; p.tex_v[3] = v0;

    p.diffuse[0] = color; p.diffuse[1] = color;
    p.diffuse[2] = color; p.diffuse[3] = color;

    p.texture_page = tex_page;
    p.pad = 0;

    td5_render_build_sprite_quad((int *)&p);
}

/* Minimap span-type → right-edge vertex delta table.
 *
 * Layout at DAT_00473fd8 in TD5_d3d.exe is a SINGLE int32[8][2] table (8 rows
 * of 2 columns). The decomp reads column 0 via `*(int *)(&DAT_00473fd8 + t*8)`
 * for the near-span vertex-1 formula, and column 1 via
 * `*(int *)(&DAT_00473fdc + t*8)` for the far-span vertex-3 formula —
 * two columns of the same row. Types 0..7 are valid.
 * [CONFIRMED via memory_read @ 0x00473fd8..0x00474018]
 *
 *     type | col 0 | col 1
 *       0  |   0   |   0
 *       1  |   0   |   0
 *       2  |  -1   |   0
 *       3  |  -1   |   0
 *       4  |  -2   |   0
 *       5  |   0   |  -1
 *       6  |   0   |  -1
 *       7  |   0   |  -2
 */
static const int32_t s_minimap_vtx_delta_col0[8] = {
    0,  0, -1, -1, -2,  0,  0,  0
};
static const int32_t s_minimap_vtx_delta_col1[8] = {
    0,  0,  0,  0,  0, -1, -1, -2
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
    /* track_state[0] at +0x80 is the actively-updated span index
     * (written by update_position_recursive in td5_track.c).
     * +0x82 is track_span_normalized which is NOT updated per-frame. */
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return *(int16_t *)(a + 0x80);
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

/* Original BuildRaceHudMetricDigits @ 0x004397b0 case 0:
 *   gHudMetricValue = (uint)(*(ushort *)(actor + 0x344) >> 8);
 * actor+0x344 is pending_finish_timer (the P2P checkpoint countdown). */
static inline uint16_t actor_pending_finish_hi(int slot)
{
    uint8_t *a = (uint8_t *)actor_ptr(slot);
    return (uint16_t)(*(uint16_t *)(a + 0x344) >> 8);
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
 *
 * [ARCH-DIVERGENCE: glyph-strip font replaces original bitmap font; L5 sweep 2026-05-21]
 *   Original loaded the "font" atlas entry and built a 4x16 UV grid with
 *   constants (base_u+1.5, base_v+2.5, 10×16 stride, 8×12 glyph). Port
 *   keeps the same grid math, but adds: PNG-first asset path (re/assets),
 *   GDI-synthesized fallback for missing tpage5.dat, atlas entry lookup
 *   via td5_asset_find_atlas_entry. Glyph layout and 0x22/0x26 width
 *   overrides match orig.
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

        /* GEARNUMBERS: labels R,N,1-6 at atlas (128,128) 128×16 — 16px per slot.
         * Real game atlas order matches gear byte: 0=R, 1=N, 2=1, 3=2, ...
         * Gear UV: u = atlas_x + gear_byte * 16. */
        {
            TD5_AtlasEntry *gn_entry = td5_asset_find_atlas_entry(NULL, "GEARNUMBERS");
            if (gn_entry && gn_entry->width > 0) {
                static const char k_gear_chars[8] = {'R','N','1','2','3','4','5','6'};
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

    /* [SMALL-FONT TTF SWAP 2026-06-05] Native TTF path: render the pause-menu
     * text in the SAME face as the frontend menu / buttons / small font (menu.ttf),
     * instead of the pause-font SDF / PAUSETXT bitmap, so the in-race pause menu
     * matches the rest of the UI. The pause panel is drawn at a FIXED pixel size
     * centred on screen (cx/cy, no sx/sy), so glyphs are naturally square — no
     * aspect stretch and no 4:3 lock needed here (unlike the frontend). Advances
     * come straight from the TTF so the row self-sizes; centred rows (alignment 2)
     * use those same advances. Tunable: PAUSE_TTF_CAP (cap px in the 16px row) /
     * PAUSE_TTF_BASELINE (px from the row top down to the baseline). Falls through
     * to the SDF/bitmap path below when no TTF is loaded. */
    if (s_pause_vui_line_count > 0 && td5_font_ready()) {
        /* Vertical: the SELBOX highlight for cursor row c spans [base_y+c*16,
         * +16] while that row's text line sits at L->y = base_y-3+c*16, so the
         * selbox CENTRE is at L->y + 11. Put the cap centre there:
         *   cap_centre = baseline - CAP/2 = L->y + BASELINE - CAP/2 == L->y + 11
         *   => BASELINE = 11 + CAP/2  (= 17 for CAP 12). */
        const float PAUSE_TTF_CAP      = 12.0f;
        const float PAUSE_TTF_BASELINE = 17.0f;   /* 11 + CAP/2; centres caps on the selbox */
        const float PAUSE_TTF_TRACK    = 0.0f;
        float cx = g_render_width_f  * 0.5f;
        float cy = g_render_height_f * 0.5f;
        static int s_logged_pause_ttf = 0;
        if (!s_logged_pause_ttf) {
            s_logged_pause_ttf = 1;
            TD5_LOG_I(LOG_TAG, "pause menu: native TTF active (cap=%.1f baseline=%.1f)",
                      (double)PAUSE_TTF_CAP, (double)PAUSE_TTF_BASELINE);
        }
        /* pass 1: rasterise every glyph into the shared atlas, then ONE GPU upload */
        for (int li = 0; li < s_pause_vui_line_count; li++) {
            const char *s = s_pause_vui_lines[li].s;
            for (int c = 0; s[c]; c++) { td5_glyph g; td5_font_get((unsigned char)s[c], PAUSE_TTF_CAP, &g); }
        }
        td5_font_flush_uploads();
        /* pass 2: lay out + draw (cache hits) */
        for (int li = 0; li < s_pause_vui_line_count; li++) {
            PauseTextLine *L = &s_pause_vui_lines[li];
            const char *s = L->s;
            float total_w = 0.0f;
            for (int c = 0; s[c]; c++)
                total_w += td5_font_advance((unsigned char)s[c], PAUSE_TTF_CAP) + PAUSE_TTF_TRACK;
            float start_x  = (L->alignment == 2) ? -(total_w * 0.5f)
                                                 : (4.0f - s_pause_half_width);
            float baseline = cy + L->y + PAUSE_TTF_BASELINE;
            float curx     = cx + start_x;
            for (int c = 0; s[c]; c++) {
                td5_glyph g; td5_font_get((unsigned char)s[c], PAUSE_TTF_CAP, &g);
                if (g.valid && g.w > 0.0f) {
                    /* Snap the glyph quad to the pixel grid. The pause panel is
                     * fixed-pixel-size, so glyphs are rasterised small (~12px); at
                     * that size, fractional positions + linear sampling read soft.
                     * Integer-aligning the top-left (w/h are already integer raster
                     * dims) makes texels map 1:1 to pixels => crisp edges. */
                    float gx = (float)(int)(curx + g.xoff + 0.5f);
                    float gy = (float)(int)(baseline + g.yoff + 0.5f);
                    td5_vui_quad(gx, gy, g.w, g.h,
                                 0xFFFFFFFFu, g.page, g.u0, g.v0, g.u1, g.v1);
                }
                curx += g.advance + PAUSE_TTF_TRACK;
            }
        }
        return;
    }

    /* VectorUI: render the menu text crisply via the pause-font SDF (the glyph
     * quads were NOT baked above). Mirrors the original PAUSETXT layout exactly
     * (alignment, glyph_w = width*2/3, +2 spacing, char-code -> 16x16 cell UV). */
    if (g_td5.ini.vector_ui && td5_vui_pausefont_page() >= 0 && s_pause_vui_line_count > 0) {
        int page = td5_vui_pausefont_page();
        float cx = g_render_width_f  * 0.5f;
        float cy = g_render_height_f * 0.5f;
        const float INV = 1.0f / 256.0f;          /* pause SDF page is 256x256 */
        for (int li = 0; li < s_pause_vui_line_count; li++) {
            PauseTextLine *L = &s_pause_vui_lines[li];
            int len = (int)strlen(L->s);
            float start_x;
            if (L->alignment == 2) {
                float total_w = 0.0f;
                for (int c = 0; c < len; c++) {
                    uint8_t ch = (uint8_t)L->s[c];
                    total_w += (float)(((g_pause_glyph_widths[ch] * 2) / 3) + 2);
                }
                start_x = -(total_w * 0.5f);
            } else {
                start_x = 4.0f - s_pause_half_width;
            }
            float curx = start_x;
            for (int c = 0; c < len; c++) {
                uint8_t ch = (uint8_t)L->s[c];
                int glyph_w = (g_pause_glyph_widths[ch] * 2) / 3;
                float gu = (float)(ch & 0x0F) * 16.0f;
                float gv = (float)(ch >> 4)   * 16.0f;
                float u0 = (gu + 0.5f) * INV,                 v0 = (gv + 0.5f)  * INV;
                float u1 = (gu + 0.5f + (float)(glyph_w - 1)) * INV, v1 = (gv + 16.5f) * INV;
                td5_vui_msdf_quad(cx + curx + 0.5f, cy + L->y + 0.5f,
                                  (float)(glyph_w - 1), 15.5f,
                                  0xFFFFFFFFu, page, u0, v0, u1, v1);
                curx += (float)(glyph_w + 2);
            }
        }
    }
}

/* ========================================================================
 * [S27 2026-06-05] Controller-disconnect modal (per split-screen viewport)
 *
 * Draws a semi-transparent dimmer + a centered "reconnect" message over the
 * viewport of each player whose controller has dropped, scoped to that player's
 * pane. s_view_layout[v] holds the pixel-space rect; viewport v == player v ==
 * device slot v (per td5_game_init_viewport_layout), so g_actor_slot_map[v]
 * gives the player whose disconnect state we check. No-op unless a controller is
 * currently missing. SOURCE-PORT FEATURE — no original equivalent.
 *
 * Uses the immediate-mode VectorUI primitives: td5_vui_quad self-blends via the
 * HUD translucent preset (so a low-alpha fill is genuinely see-through), and
 * td5_vui_text_centered is crisp MSDF text. Both fall back to the bitmap path
 * when VectorUI is disabled, so this needs no quad pre-bake.
 * ======================================================================== */
/* ========================================================================
 * Per-viewport player identity: a coloured frame in the player's accent colour
 * around each split-screen pane + a name plate flush along the BOTTOM edge of
 * the pane, so everyone can tell who is driving which viewport at a glance.
 * SOURCE-PORT FEATURE. [PORT 2026-06-08]
 * ======================================================================== */
void td5_hud_draw_player_id_overlays(void)
{
    int views = s_view_count;
    if (!s_hud_id_active) return;
    if (views < 2) return;                  /* only meaningful when split */
    if (views > MAX_HUD_VIEWS) views = MAX_HUD_VIEWS;

    for (int v = 0; v < views; v++) {
        int slot = g_actor_slot_map[v];
        const TD5_HudViewLayout *vl = &s_view_layout[v];
        float L = vl->vp_int_left,  T = vl->vp_int_top;
        float R = vl->vp_int_right, B = vl->vp_int_bottom;
        float w = R - L, h = B - T;
        uint32_t accent;
        char nm[20];
        float bt, ts, cx, ny, tw;
        int r8, g8, b8, lum;
        uint32_t tcol;
        if (w < 2.0f || h < 2.0f) continue;
        if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) continue;

        accent = s_hud_id_accent[slot] ? s_hud_id_accent[slot] : 0xFFB0B0B0u;

        /* Coloured frame around this pane. */
        bt = 3.0f * (w / 640.0f);
        if (bt < 2.0f) bt = 2.0f;
        if (bt > 5.0f) bt = 5.0f;
        td5_vui_quad(L, T, w, bt, accent, -1, 0, 0, 0, 0);
        td5_vui_quad(L, B - bt, w, bt, accent, -1, 0, 0, 0, 0);
        td5_vui_quad(L, T, bt, h, accent, -1, 0, 0, 0, 0);
        td5_vui_quad(R - bt, T, bt, h, accent, -1, 0, 0, 0, 0);

        /* Name plate flush along the BOTTOM edge of the pane, sized to fit the
         * text. fe_draw_text anchors its `y` at the glyph CELL TOP, and the
         * visible caps occupy cell rows 8..23 (15 design-px tall) in both the TTF
         * and bitmap paths — so the cap band's vertical centre sits 15.5*ts below
         * the passed y. We size the plate to that cap height plus a little
         * padding and place the text so the caps sit centred inside the plate. */
        if (s_hud_id_name[slot][0]) snprintf(nm, sizeof nm, "%s", s_hud_id_name[slot]);
        else                        snprintf(nm, sizeof nm, "PLAYER %d", slot + 1);
        ts = (w / 640.0f) * 0.95f;
        if (ts > 1.2f) ts = 1.2f;
        if (ts < 0.40f) ts = 0.40f;
        cx = (L + R) * 0.5f;
        tw = td5_vui_text_width(nm, ts);
        {
            float cap_h   = 15.0f * ts;             /* visible cap height (both font paths) */
            float pad_y   = 5.0f  * ts;             /* breathing room above + below the caps */
            float plate_h = cap_h + 2.0f * pad_y;
            float plate_w = tw + 12.0f * ts;        /* 6*ts side padding each side */
            float plate_b = B - bt;                 /* flush against the inside of the bottom frame */
            float plate_t = plate_b - plate_h;
            ny = (plate_t + plate_b) * 0.5f - 15.5f * ts;   /* cell top -> caps centred in plate */
            td5_vui_quad(cx - plate_w * 0.5f, plate_t, plate_w, plate_h,
                         accent, -1, 0, 0, 0, 0);
            r8 = (accent >> 16) & 0xFF; g8 = (accent >> 8) & 0xFF; b8 = accent & 0xFF;
            lum = (r8 * 30 + g8 * 59 + b8 * 11) / 100;
            tcol = (lum > 150) ? 0xFF101010u : 0xFFFFFFFFu;
            td5_vui_text_centered(cx, ny, nm, tcol, ts, ts);
        }
    }
}

void td5_hud_draw_disconnect_overlays(void)
{
    if (!td5_game_device_disconnect_active()) return;   /* fast out: nothing lost */

    int views = s_view_count;
    if (views < 1) views = 1;
    if (views > MAX_HUD_VIEWS) views = MAX_HUD_VIEWS;

    for (int v = 0; v < views; v++) {
        int player = g_actor_slot_map[v];               /* viewport v -> player slot */
        if (!td5_game_player_disconnected(player)) continue;

        const TD5_HudViewLayout *vl = &s_view_layout[v];
        float L = vl->vp_int_left,  T = vl->vp_int_top;
        float R = vl->vp_int_right, B = vl->vp_int_bottom;
        float w = R - L, h = B - T;
        if (w < 2.0f || h < 2.0f) continue;

        /* Semi-transparent dark dimmer over the whole pane (~72% black). */
        td5_vui_quad(L, T, w, h, 0xB8000000u, -1, 0.0f, 0.0f, 0.0f, 0.0f);

        /* Throttled confirmation that the modal draw path runs with a sane pane
         * rect (once/sec/view, not per-frame spam). Lets a headless run verify
         * the overlay without a screenshot of the D3D swapchain. */
        {
            static uint32_t s_dc_log_ctr;
            if ((s_dc_log_ctr++ % 60u) == 0u)
                TD5_LOG_I("hud", "disconnect modal: view=%d player=%d rect=(%.0f,%.0f %.0fx%.0f)",
                          v, player, L, T, w, h);
        }

        /* Message scaled to the pane so it fits in split-screen: start from a
         * width-proportional scale, then shrink until the longest line fits
         * ~88% of the pane width. */
        const char *l1 = "CONTROLLER DISCONNECTED";
        const char *l2 = "RECONNECT TO CONTINUE";
        float ts = w / 640.0f;
        if (ts > 1.5f) ts = 1.5f;
        if (ts < 0.35f) ts = 0.35f;
        float maxw = w * 0.88f;
        for (int guard = 0; guard < 10; guard++) {
            if (td5_vui_text_width(l1, ts) <= maxw) break;
            ts *= 0.88f;
        }

        float cx = (L + R) * 0.5f;
        float cy = (T + B) * 0.5f;
        float line_h = 26.0f * ts;

        if (views > 1) {                                /* which player, when split */
            char who[24];
            snprintf(who, sizeof who, "PLAYER %d", player + 1);
            float ts2 = ts * 0.8f;
            td5_vui_text_centered(cx, cy - line_h * 1.6f, who, 0xFF66FF66u, ts2, ts2);
        }
        td5_vui_text_centered(cx, cy - line_h * 0.5f, l1, 0xFFFFFFFFu, ts, ts);
        td5_vui_text_centered(cx, cy + line_h * 0.6f, l2, 0xFFFFC050u, ts * 0.85f, ts * 0.85f);
    }
}

/* Called each frame while paused to update SELBOX position and slider thumb positions.
 * cursor: 0-5 (rows: VIEW / SOUND / CONTINUE / RESTART RACE / QUIT TO MENU / EXIT GAME)
 * [PORT REWORK 2026-06-05 / S15] The MUSIC slider was removed, so only two
 * sliders remain: row 0 = VIEW (view distance), row 1 = SOUND (SFX master).
 * music_frac is retained in the signature (caller still computes it) but is no
 * longer used. */
void td5_hud_update_pause_overlay(int cursor, float view_dist_frac, float music_frac, float sfx_frac)
{
    (void)music_frac;

    /* Re-bake the panel/selbox/slider quads if the render size has changed since
     * they were built (the panel bakes the screen centre into absolute coords;
     * the text below recomputes it live). Without this the panel drifts off the
     * text after a resize/maximize, distorting the pause layout and displacing
     * the dark backing panel. Runs before the selbox/slider positions are set so
     * they land on the freshly-baked panel. Cheap: only fires on an actual change. */
    if (g_render_width_f != s_pause_baked_w || g_render_height_f != s_pause_baked_h) {
        TD5_LOG_I(LOG_TAG, "pause overlay: re-bake on dim change %.0fx%.0f -> %.0fx%.0f",
                  (double)s_pause_baked_w, (double)s_pause_baked_h,
                  (double)g_render_width_f, (double)g_render_height_f);
        td5_hud_init_pause_menu(s_pause_page_index);
    }

    float cx = g_render_width_f  * 0.5f;
    float cy = g_render_height_f * 0.5f;
    float fracs[2] = { view_dist_frac, sfx_frac };  /* row 0 = VIEW, row 1 = SOUND */

    {
        static float s_last_view_frac = -1.0f;
        static int s_last_cursor = -1;
        if (view_dist_frac != s_last_view_frac || cursor != s_last_cursor) {
            TD5_LOG_I("hud", "PAUSE OVERLAY: cursor=%d view_dist_frac=%.3f music=%.2f sfx=%.2f",
                      cursor, view_dist_frac, music_frac, sfx_frac);
            s_last_view_frac = view_dist_frac;
            s_last_cursor    = cursor;
        }
    }

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
     * Fill scale = 128.0f (0x0045D600), UV scale = frac*255.0 (0x0045D684).
     * [PORT REWORK 2026-06-05 / S15] Two sliders (VIEW, SOUND). */
    for (int row = 0; row < 2; row++) {
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

/* ---- VectorUI HUD text (MSDF) ------------------------------------------
 * When [Frontend] VectorUI is on, td5_hud_queue_text records the formatted
 * ASCII string + position instead of building bitmap glyph quads, and
 * td5_hud_flush_text renders them crisply via the shared MSDF text renderer
 * (td5_vui_text). Recorded (not drawn immediately) so they layer on top of the
 * gauge/scene exactly like the bitmap flush did. Falls back to the bitmap glyph
 * atlas when VectorUI is off or the MSDF font is unavailable. */
typedef struct { float x, y; int centered; char s[64]; } HudVuiText;
static HudVuiText s_hud_vui_text[96];
static int        s_hud_vui_text_count;

/* [HUD TTF SWAP 2026-06-05] The in-race HUD overlay text renders in the menu's
 * SECONDARY native face (Rajdhani, td5_hudfont_*) when loaded, instead of the
 * HUD-font SDF / tpage5 bitmap. Like the pause menu, the HUD text path is
 * FIXED-pixel (the original 8x12-cell layout, unscaled in g_render_width_f
 * space), so glyphs are naturally square — no 4:3 lock. Advances come from the
 * TTF so layout self-sizes; both the flush (centring) and hud_text_width (FPS
 * right-anchor) use the SAME advances so positions stay consistent. Sizes are in
 * HUD design px (the ~12px-tall cell). Tunable:
 *   CAP      = cap height px (orig caps were ~11 in the 12px cell)
 *   BASELINE = px from the queued cell-top y down to the baseline. */
#define HUD_TTF_CAP       11.0f
#define HUD_TTF_BASELINE  11.0f

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

    /* VectorUI: record the raw ASCII string for crisp rendering at flush — via
     * the native HUD TTF (Rajdhani) when loaded, else the HUD-font SDF. The
     * original glyph table / char remap is applied in the SDF path; the TTF path
     * renders the ASCII codepoints directly. */
    if (g_td5.ini.vector_ui && (td5_hudfont_ready() || td5_vui_hudfont_page() >= 0)) {
        int n = (int)(sizeof(s_hud_vui_text) / sizeof(s_hud_vui_text[0]));
        if (s_hud_vui_text_count < n) {
            HudVuiText *e = &s_hud_vui_text[s_hud_vui_text_count++];
            e->x = (float)x;  e->y = (float)y;  e->centered = centered;
            strncpy(e->s, buf, sizeof(e->s) - 1);
            e->s[sizeof(e->s) - 1] = '\0';
        }
        return;
    }

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
 *
 * [CONFIRMED @ 0x00428570 FlushQueuedRaceHudText; L5 sweep 2026-05-21]
 *   Byte-faithful: walk g_hudGlyphQueueBuf with stride 0xB8 (0x5C ushort),
 *   SubmitImmediateTranslucentPrimitive per quad, reset count to 0. Port's
 *   while(>0) handles the count==0 case orig guards explicitly — semantic
 *   identity preserved.
 * ======================================================================== */

void td5_hud_flush_text(void)
{
    /* Native HUD TTF (Rajdhani): render the recorded strings in the menu's
     * secondary face. Fixed-pixel layout (glyphs are square, no aspect lock);
     * advances + centring come from the TTF, matching hud_text_width. Each glyph
     * quad is pixel-snapped so the small fixed-size text stays crisp under linear
     * sampling. Takes priority over the SDF/bitmap when the TTF is loaded. */
    if (g_td5.ini.vector_ui && s_hud_vui_text_count > 0 && td5_hudfont_ready()) {
        static int s_logged_hud_ttf = 0;
        if (!s_logged_hud_ttf) {
            s_logged_hud_ttf = 1;
            TD5_LOG_I(LOG_TAG, "HUD text: native TTF active (cap=%.1f baseline=%.1f)",
                      (double)HUD_TTF_CAP, (double)HUD_TTF_BASELINE);
        }
        /* pass 1: rasterise every glyph into the shared atlas, then ONE upload */
        for (int i = 0; i < s_hud_vui_text_count; i++) {
            const char *s = s_hud_vui_text[i].s;
            for (int k = 0; s[k]; k++) { td5_glyph g; td5_hudfont_get((unsigned char)s[k], HUD_TTF_CAP, &g); }
        }
        td5_font_flush_uploads();
        /* pass 2: lay out + draw (cache hits) */
        for (int i = 0; i < s_hud_vui_text_count; i++) {
            HudVuiText *e = &s_hud_vui_text[i];
            const char *s = e->s;
            float total_w = 0.0f;
            for (int k = 0; s[k]; k++) total_w += td5_hudfont_advance((unsigned char)s[k], HUD_TTF_CAP);
            float cx = e->x;
            if (e->centered) cx -= total_w * 0.5f;
            float baseline = e->y + HUD_TTF_BASELINE;
            for (int k = 0; s[k]; k++) {
                td5_glyph g; td5_hudfont_get((unsigned char)s[k], HUD_TTF_CAP, &g);
                if (g.valid && g.w > 0.0f) {
                    float gx = (float)(int)(cx + g.xoff + 0.5f);
                    float gy = (float)(int)(baseline + g.yoff + 0.5f);
                    td5_vui_quad(gx, gy, g.w, g.h, 0xFFFFFFFFu, g.page, g.u0, g.v0, g.u1, g.v1);
                }
                cx += g.advance;
            }
        }
        s_hud_vui_text_count = 0;
    } else
    /* VectorUI: render the recorded strings via the HUD-font SDF atlas, reusing
     * the ORIGINAL glyph table (typeface, per-glyph widths, char remap, centered
     * math, +1 spacing) -- identical layout to the bitmap path, just crisp. */
    if (g_td5.ini.vector_ui && td5_vui_hudfont_page() >= 0 && s_hud_vui_text_count > 0) {
        int page = td5_vui_hudfont_page();
        TD5_GlyphRecord *glyphs = s_glyph_table;   /* font 0 */
        const float INV = 1.0f / 256.0f;           /* SDF page is 256x256, texel UVs */
        for (int i = 0; i < s_hud_vui_text_count; i++) {
            HudVuiText *e = &s_hud_vui_text[i];
            int len = (int)strlen(e->s);
            float total_w = 0.0f;
            for (int k = 0; k < len; k++) {
                uint8_t gi = s_char_remap[(uint8_t)e->s[k] & 0x7F];
                total_w += glyphs[gi].width;
            }
            float cx = e->x;
            if (e->centered) cx -= ((float)(len - 1) + total_w) * 0.5f;
            for (int k = 0; k < len; k++) {
                uint8_t gi = s_char_remap[(uint8_t)e->s[k] & 0x7F];
                TD5_GlyphRecord *g = &glyphs[gi];
                float u0 = g->atlas_u * INV,                v0 = g->atlas_v * INV;
                float u1 = (g->atlas_u + g->width)  * INV,  v1 = (g->atlas_v + g->height) * INV;
                td5_vui_msdf_quad(cx, e->y, g->width, g->height, 0xFFFFFFFFu, page, u0, v0, u1, v1);
                cx += g->width + 1.0f;
            }
        }
        s_hud_vui_text_count = 0;
    }

    uint8_t *ptr = s_text_quad_buf;
    int count = s_queued_glyph_count;

    while (count > 0) {
        hud_submit_quad(ptr);
        ptr += TD5_HUD_GLYPH_QUAD_SIZE;
        count--;
    }

    s_queued_glyph_count = 0;
}

/* [removed 2026-06-05] hud_text_width() was added in S12 solely to right-anchor
 * the FPS/MS counter to the screen's right edge. The counter now draws top-LEFT
 * at a fixed x=8 (below the POSITION label), so the width measurement is no
 * longer needed and the helper was removed to avoid an unused-function warning.
 * (Master's TTF-aware revision of it, 48d480c, only served that same right-anchor
 * caller, which this change replaces — so nothing else depends on it.) */

/* ========================================================================
 * InitializeRaceOverlayResources (0x4377B0)
 *
 * Allocates HUD primitive storage, sets visibility bitmask based on
 * race mode, and loads the fade/divider sprites.
 *
 * [CONFIRMED @ 0x004377B0] — Ghidra-verified: orig allocates 0x1148 bytes
 * via HeapAllocTracked, splits into two halves with the secondary view
 * pointer at +0x229*4 (= 0x894 bytes offset, matching port s_hud_prim_storage
 * +0x894 at line 1158). Bitmask construction mirrors orig: race_mode==0 path
 * writes 0x80000000 (replay banner) when g_replayModeFlag==0 (port: same
 * inverted convention); race_mode!=0 path ORs bits 4|0x10|0x40|0x20|8
 * (port: TD5_HUD_SPEEDOMETER|UTURN|METRIC|RESERVED_5|RESERVED_3 — bits
 * 4|0x10|0x40|0x20|8) then conditionally adds 1|2 for position/lap timers
 * driven by g_raceOverlayPresetMode (port: g_game_type with semantic-
 * equivalent gating). Bit 0x80 (total-timer) under preset 2 with special-
 * encounter gate matches port lines 1191-1193 (g_game_type==2 with
 * race_rule_variant 0/4). Bits 0x80|0x100 for preset 4 matches port lines
 * 1194-1196. Circuit-lap bit 0x200 at lines 1199-1201 matches orig
 * gTrackIsCircuit gate at LAB_004378c9. FADEWHT atlas lookup + 5-element
 * fade-quad build + COLOURS split-screen divider builds all match.
 *
 * [ARCH-DIVERGENCE: BuildSpriteQuadTemplate D3D3 scratch → hud_build_quad
 *  D3D11 buffer] — orig writes 4-vertex packed quads into the scratch ring
 *  at gHudFadeQuadTemplateArray + i*0xB8 via BuildSpriteQuadTemplate (D3D3
 *  immediate-mode FVF); port writes equivalent geometry into the s_fade_quads
 *  array via hud_build_quad (D3D11 vertex stream). Same geometry, different
 *  submission model. Documented class-level in td5_hud_render_overlays
 *  comment lines 1878-1885. L5 promotion sweep 2026-05-21.
 * ======================================================================== */

void td5_hud_init_overlay_resources(int race_mode, int string_table_offset)
{
    /* Set string table based on game mode offset */
    s_hud_string_table = g_position_strings + string_table_offset * 13;
    s_is_first_player = (string_table_offset == 0) ? 1 : 0;

    for (int v = 0; v < MAX_HUD_VIEWS; v++)
        s_wrong_way_counter[v] = 0;

    /* Allocate per-view HUD primitive storage [PORT ENHANCEMENT: N-way split].
     * Each view's flag word + quad block lives at +v*0x894 (orig +0x229*4).
     * Allocate room for every pane so views >=2 don't deref an uninitialised
     * flag pointer (the legacy code only set s_hud_flags[0]/[1]). */
    s_hud_prim_storage = (uint8_t *)td5_game_heap_alloc(
        (size_t)MAX_HUD_VIEWS * HUD_VIEW_BLOCK);
    for (int v = 0; v < MAX_HUD_VIEWS; v++)
        s_hud_flags[v] = &s_hud_flag_words[v];   /* dedicated array — never clobbered by quad spill */

    /* Set visibility bitmask based on race mode */
    if (race_mode == 0) {
        /* race_mode==0 is passed only for View Replay or benchmark (orig
         * InitializeRaceOverlayResources param_1==0). The replay sub-path lights
         * the flashing "REPLAY" banner (bit 0x80000000); benchmark shows nothing.
         * [CONFIRMED orig @0x437805: param_1==0 && view_z==0.0 (NOT benchmark)
         *  → flags=0x80000000; else flags=0.] The previous port keyed this on
         * g_replay_mode==0, which (combined with the caller hardcoding race_mode=1)
         * left the banner permanently dead and showed "DEMO MODE" during replay. */
        /* [PORT: N-way] write the flag word for every pane, not just 0/1. */
        for (int hv = 0; hv < MAX_HUD_VIEWS; hv++)
            *s_hud_flags[hv] = (!g_td5.benchmark_active) ? TD5_HUD_REPLAY_BANNER : 0;
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
        } else if (g_game_type == 8) {
            /* Cop Chase (preset 4 / DAT_004aaf74==4): the orig overlay bitmask
             * is 0x04|0x08|0x10|0x20|0x40|0x80|0x100 — it OMITS the race
             * POSITION label (0x01) and the per-racer LAP TIMERS (0x02), and
             * adds the total timer (0x80) + the bust/arrest counter (0x100).
             * [FIX 2026-05-31, orig InitializeRaceOverlayResources @ 0x004378a9
             *  preset-4 path]. The previous port fell into the generic else and
             *  wrongly showed "1ST/2ND" position in cop chase. */
            flags |= TD5_HUD_TOTAL_TIMER | TD5_HUD_LAP_COUNTER;
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

        /* [PORT: N-way] every pane shows the same HUD element set. */
        for (int hv = 0; hv < MAX_HUD_VIEWS; hv++)
            *s_hud_flags[hv] = flags;
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
 *
 * [CONFIRMED @ 0x00437BA0] — Ghidra-verified scale math: g_hudPrimaryScaleX =
 * g_renderWidthF * _DAT_0045d64c (= 1/640); g_hudPrimaryScaleY =
 * g_renderHeightF * _DAT_0045d6ec (= 1/480) — port lines 1273-1274 emit
 * the same constants. Viewport-mode dispatch matches:
 *   mode 0: single full-screen (orig sets vp_left=W*-0.5, vp_right=W*0.5
 *           with centered coords; port sets [0..W] in pixel-space)
 *   mode 1: left/right split (scales halved on X)
 *   mode 2: top/bottom split (scales halved on Y)
 * Per-view derivation loop at 0x437C70 (puVar7 += 0xE) matches port loop
 * at lines 1339-1349 (TD5_HudViewLayout stride). Atlas lookups follow the
 * same order: numbers, SEMICOL, SPEEDO, SPEEDOFONT, GEARNUMBERS, UTURN,
 * REPLAY. SPEEDO quad offsets (gHudCurrentFlagsPtr + 4 / +0xBC / +0x39C
 * / +0x67C / +0x7EC) align with port's view_base offsets in the master
 * dispatcher comment block lines 1799-1854.
 *
 * [ARCH-DIVERGENCE: centered-coord BuildSpriteQuadTemplate scratch →
 *  pixel-space hud_build_quad; per-view layout struct vs DAT_004B1160-
 *  stride table] — orig stores 14 floats per view at DAT_004B1160 + v*0xE
 *  (pfVar5 at 0x437D8B); port uses named TD5_HudViewLayout struct with
 *  scale_x/scale_y/vp_int_X/center_X/half_X. Equivalent storage. Orig also
 *  emits centered-origin coords consumed by BuildSpriteQuadTemplate which
 *  internally adds screen_center; port emits pixel-space coords directly to
 *  D3D11. Documented class-level in render_overlays comment lines 1887-1894.
 *  L5 promotion sweep 2026-05-21.
 * ======================================================================== */

void td5_hud_init_layout(int viewport_mode)
{
    /* Load numbers atlas (used by metric display) */
    s_numbers_atlas = td5_asset_find_atlas_entry(NULL, "numbers");
    hud_log_atlas_status("numbers", s_numbers_atlas);

    /* Compute base scale factors. [S01 2026-06-04] Use a UNIFORM scale (the
     * smaller of the two axes) for HUD element SIZING so round elements — speedo
     * dial, tach, minimap grid, gauges — stay circular on non-4:3 / widescreen
     * windows instead of stretching. Element POSITIONS still use the full per-view
     * vp_* bounds below, so the HUD stays pinned to the correct screen corners. At
     * 4:3 sx==sy, so this is a no-op (byte-identical to the original layout). */
    s_scale_x = g_render_width_f * (1.0f / 640.0f);
    s_scale_y = g_render_height_f * (1.0f / 480.0f);
    {
        float uniform_scale = (s_scale_x < s_scale_y) ? s_scale_x : s_scale_y;
        s_scale_x = uniform_scale;
        s_scale_y = uniform_scale;
    }

    /* Set up per-view layout in pixel-space coordinates.
     *
     * The original binary uses a centered coordinate system where origin is
     * the screen center (vp_left = -width/2, vp_right = +width/2).
     * BuildSpriteQuadTemplate @ 0x432BD0 adds screen_center to each vertex
     * before submitting to hardware.  Our source port submits vertices
     * directly to D3D11 without that centering step, so all HUD positions
     * must be in pixel-space (vp_left = 0, vp_right = width). */
    /* [PORT ENHANCEMENT] N-way HUD layout. Views 1-2 reproduce the legacy
     * full / half-screen layouts byte-for-byte (viewport_mode 0/1/2). Views
     * 3-9 mirror the 3D viewport ladder in td5_game_init_viewport_layout
     * (3 = horizontal strips; 4=2x2, 5-6=3x2, 7-9=3x3) with a uniform per-pane
     * scale so HUD elements keep their aspect inside each pane. Driven by
     * g_td5.viewport_count (the live pane count), not just viewport_mode. */
    int views = g_td5.viewport_count;
    if (views < 1) views = 1;
    if (views > MAX_HUD_VIEWS) views = MAX_HUD_VIEWS;
    s_view_count = views;

    if (views == 1) {
        /* Single full-screen view (legacy viewport_mode 0) */
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f;

    } else if (views == 2 && viewport_mode == 2) {
        /* Vertical split — left | right. Matches the game's viewport layout
         * for split_screen_mode 2 (td5_game_init_viewport_layout). The HUD used
         * to key left/right off mode 1, which is the game's TOP/BOTTOM mode —
         * that orientation mismatch put the HUD in the wrong half. */
        s_scale_x *= 0.5f;
        s_scale_y *= 0.5f;
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f * 0.5f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f;
        s_view_layout[1].scale_x  = s_scale_x;
        s_view_layout[1].scale_y  = s_scale_y;
        s_view_layout[1].vp_left   = g_render_width_f * 0.5f;
        s_view_layout[1].vp_right  = g_render_width_f;
        s_view_layout[1].vp_top    = 0.0f;
        s_view_layout[1].vp_bottom = g_render_height_f;

    } else if (views == 2) {
        /* Horizontal split — top / bottom. Matches the game's viewport layout
         * for split_screen_mode 1 (the default 2-player orientation). */
        s_scale_x *= 0.5f;
        s_scale_y *= 0.5f;
        s_view_layout[0].scale_x  = s_scale_x;
        s_view_layout[0].scale_y  = s_scale_y;
        s_view_layout[0].vp_left   = 0.0f;
        s_view_layout[0].vp_right  = g_render_width_f;
        s_view_layout[0].vp_top    = 0.0f;
        s_view_layout[0].vp_bottom = g_render_height_f * 0.5f;
        s_view_layout[1].scale_x  = s_scale_x;
        s_view_layout[1].scale_y  = s_scale_y;
        s_view_layout[1].vp_left   = 0.0f;
        s_view_layout[1].vp_right  = g_render_width_f;
        s_view_layout[1].vp_top    = g_render_height_f * 0.5f;
        s_view_layout[1].vp_bottom = g_render_height_f;

    } else {
        /* N-way grid (>=3): mirror the game's 3D viewport grid EXACTLY (same
         * shared resolver) so the HUD panes line up with the rendered
         * viewports. [FIX 2026-06-07] The HUD used to hardcode views==3 -> 1x3
         * (horizontal strips) while the 3D layout honoured the committed pick
         * (3 players default to 3x1 LEFT/RIGHT) -> HUD/viewport orientation
         * mismatch (UI laid out UP/DOWN over a LEFT/RIGHT viewport). */
        int cols, rows;
        td5_game_resolve_split_grid(views, &cols, &rows);
        float pane_w = g_render_width_f  / (float)cols;
        float pane_h = g_render_height_f / (float)rows;
        float fx = pane_w / g_render_width_f;
        float fy = pane_h / g_render_height_f;
        float sfrac = (fx < fy) ? fx : fy;   /* uniform fit, aspect-correct */
        s_scale_x *= sfrac;
        s_scale_y *= sfrac;
        for (int v = 0; v < views; v++) {
            int col = v % cols;
            int row = v / cols;
            s_view_layout[v].scale_x  = s_scale_x;
            s_view_layout[v].scale_y  = s_scale_y;
            s_view_layout[v].vp_left   = (float)col       * pane_w;
            s_view_layout[v].vp_right  = (float)(col + 1) * pane_w;
            s_view_layout[v].vp_top    = (float)row       * pane_h;
            s_view_layout[v].vp_bottom = (float)(row + 1) * pane_h;
        }
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

        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;
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

        /* Build 3 digit quads (ones, tens, hundreds -- right to left)
         * [DA-T3 audit 2026-05-22] orig step = font_glyph_w + 2.0f
         * (NOT +1.0f); orig height = sy * 23.0f (NOT sy * 24.0f) per
         * 0x00437BA0 layout 0x174/+0x22C/+0x2E4. */
        for (int d = 0; d < 3; d++) {
            float dx = font_x_start + (float)d * (font_glyph_w + 2.0f);
            hud_build_quad(
                view_base + SPEEDFONT_BASE_OFF + d * TD5_HUD_GLYPH_QUAD_SIZE,
                0, s_speedofont_atlas->texture_page,
                dx, font_y,
                dx + font_glyph_w, font_y + sy * 23.0f,
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

        /* --- Metric digit quads (numbers atlas, 4 digits) ---
         * [DA-T3 audit 2026-05-22] Per InitializeRaceHudLayout @ 0x00437BA0
         * layout offsets +0x734/+0x454/+0x50C/+0x5C4:
         *   - glyph width = sx * 15 (was sx * 16) — anchor shifts left ~2.5px
         *   - y offset = vp_t + 12.0 raw px, NOT scaled by sy (preserves
         *     12px top inset across all resolutions)
         *   - glyph height = sx * 23 (orig quirk: sx not sy — yes really) */
        float metric_glyph_w = sx * 15.0f;
        float metric_x = s_view_layout[v].center_x - metric_glyph_w * 2.5f;
        float metric_y = vp_t + 12.0f;

        for (int d = 0; d < 4; d++) {
            float mdx = metric_x + (float)d * metric_glyph_w;
            /* These are built with UV-only mode since the atlas lookup is per-frame */
            hud_build_quad(
                view_base + 0x734 + d * TD5_HUD_GLYPH_QUAD_SIZE,
                0, s_numbers_atlas->texture_page,
                mdx, metric_y,
                mdx + metric_glyph_w, metric_y + sx * 23.0f,
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
        /* Original BuildRaceHudMetricDigits case 0 @ 0x004397d8:
         *   gHudMetricValue = (uint)(*(ushort *)(actor + 0x344) >> 8);
         * Reads the pending_finish_timer hi-byte (P2P countdown), NOT
         * the cumulative lap timer. The bit-0x40 dispatch is gated on
         * g_special_encounter so this only runs in P2P races. */
        s_metric_value = (uint32_t)actor_pending_finish_hi(actor_slot);
        break;
    case TD5_METRIC_FPS:
        s_metric_value = (uint32_t)(g_td5.instant_fps + 0.5f);
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
        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;
        hud_build_quad(
            view_base + 0x734,
            0, s_numbers_atlas->texture_page,  /* mode 0: write screen positions every call (port has no layout-init step that mode=2 would rely on) */
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

    /* Screen positions — sprites are 16w × 24h. Top-center of view 0:
     * three digits side by side, hundreds left of tens left of ones,
     * total width 48px centered on the viewport half-width.
     *
     * The original initializes these positions during HUD layout setup
     * and the per-frame BuildRaceHudMetricDigits only updates UVs —
     * port has no equivalent layout step, so we set positions inline
     * each call. This is a known port-shape divergence; user-visible
     * result is identical (digits at the right place). */
    TD5_HudViewLayout *vl0 = &s_view_layout[0];
    float center_x = vl0->vp_left + vl0->half_width;
    float dy0 = vl0->vp_top + 8.0f;
    float dy1 = dy0 + 23.0f;
    float dx_h0 = center_x - 24.0f;  /* hundreds */
    float dx_t0 = center_x - 8.0f;   /* tens */
    float dx_o0 = center_x + 8.0f;   /* ones */

    /* Hundreds */
    {
        uint32_t d = val / 100;
        int col = (int)(d % 5);
        int row = (int)(d / 5);
        float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
        float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;
        hud_build_quad(
            view_base + 0x454,
            0, s_numbers_atlas->texture_page,  /* mode 0: write screen positions every call (port has no layout-init step that mode=2 would rely on) */
            dx_h0, dy0, dx_h0 + 15.0f, dy1,
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

        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;
        hud_build_quad(
            view_base + 0x50C,
            0, s_numbers_atlas->texture_page,  /* mode 0: write screen positions every call (port has no layout-init step that mode=2 would rely on) */
            dx_t0, dy0, dx_t0 + 15.0f, dy1,
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

        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;
        hud_build_quad(
            view_base + 0x5C4,
            0, s_numbers_atlas->texture_page,  /* mode 0: write screen positions every call (port has no layout-init step that mode=2 would rely on) */
            dx_o0, dy0, dx_o0 + 15.0f, dy1,
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
 * Finishing-position center-screen digit (port enhancement, user 2026-05-30)
 *
 * Draws the player's finishing place as one BIG digit centered on the screen
 * during the race-end victory window. position is 1-based (1 = 1st). The
 * ORIGINAL shows the finishing position only on the post-race results screen
 * (BuildRaceResultsTable @ 0x0040a8c0) — this overlays it on the race view per
 * user request. Reuses the NUMBERS atlas (digit d -> cell (d%5,d/5), 16x24)
 * scaled up, drawn opaque white via the normal translucent-HUD path.
 * ======================================================================== */
void td5_hud_draw_finish_position(int position)
{
    if (!s_numbers_atlas) return;
    if (position < 1) position = 1;
    if (position > 9) position = 9;   /* single digit (max 6 racers) */

    int digit = position;             /* NUMBERS cell index == digit glyph */
    int col = digit % 5;
    int row = digit / 5;
    float u0 = (float)(col * 16 + s_numbers_atlas->atlas_x) + 0.5f;
    float v0 = (float)(row * 24 + s_numbers_atlas->atlas_y) + 0.5f;

    /* Same size as the in-race checkpoint indicator digit: a 16x24 NUMBERS cell
     * scaled by the view's HUD scale (sx,sy), centered on the screen.
     * [user 2026-05-30: number "has to be the same size as the checkpoint
     *  indicator number" and "in the middle of the screen".] The indicator
     *  (see td5_hud_render_overlays) draws sx*16 x sy*24 — match it exactly and
     *  drop the prior 1.5x blow-up that read "bigger than the indicator". */
    float sx = s_view_layout[0].scale_x;
    float sy = s_view_layout[0].scale_y;
    float w = 16.0f * sx;
    float h = 24.0f * sy;
    /* Center in HUD/view space (the indicator's basis), NOT g_td5.render_width —
     * HUD quads live in the per-view layout space (vp_left..vp_right), which is
     * what the checkpoint indicator centers on. Using g_td5.render_width put the
     * digit off-center because the backbuffer != HUD virtual width. */
    float cx = s_view_layout[0].center_x;
    float cy = s_view_layout[0].center_y;
    float x0 = cx - w * 0.5f;
    float y0 = cy - h * 0.5f;

    /* Drop-shadow pass: the same glyph in opaque black, offset down-right, so the
     * white digit stays readable over the white victory star (white-on-white) and
     * over bright scenery. [guards against the number "not being present" visually
     * once the star itself became white.] */
    static uint8_t s_finish_pos_shadow[0xB8];
    float off = (sx > 1.0f ? sx : 1.0f) * 2.0f;
    hud_build_quad(s_finish_pos_shadow, 0, s_numbers_atlas->texture_page,
                   x0 + off, y0 + off, x0 + w + off, y0 + h + off,
                   u0, v0, u0 + 15.0f, v0 + 23.0f,
                   0xFF000000, HUD_DEPTH);
    hud_submit_quad(s_finish_pos_shadow);

    static uint8_t s_finish_pos_quad[0xB8];
    hud_build_quad(s_finish_pos_quad, 0, s_numbers_atlas->texture_page,
                   x0, y0, x0 + w, y0 + h,
                   u0, v0, u0 + 15.0f, v0 + 23.0f,
                   0xFFFFFFFF, HUD_DEPTH);
    hud_submit_quad(s_finish_pos_quad);
}

/* ========================================================================
 * DrawRaceStatusText (0x439B70)
 *
 * Per-view text overlays: race status messages, wanted messages, time
 * trial timers, lap time comparisons.
 *
 * [CONFIRMED @ 0x00439B70] — Ghidra-verified: orig early-exits on
 * g_inputPlaybackActive!=0 (port: not gated — port handles replay through
 * the g_replay_mode path lines 1735-1742 below, equivalent semantics).
 * g_replayModeFlag!=0 path emits "DEMO MODE" centered at (vpL+0x10, vpT+0x10)
 * — port lines 1735-1742 match (vp_half_w, vp_top+16, alignment=1).
 * g_wantedModeEnabled && g_hudStatusTextEnabled && timer<300 branch emits
 * SUSPECT_IS_WANTED/ARMED_ROBBERY two-liner with flash gate at frame 0x10e
 * (=270) and even-frame mask — port lines 1745-1762 match exactly
 * (timer 270, & 1 == 0).
 * g_selectedGameType!=0 path emits "%s %02d:%02d.%03d" total timer at
 * (curLayout[10]+8, vpT+8, align=0) — port lines 1759-1786 emit at
 * (vp_int_left+8, vp_top+8) with the same MM:SS.HHH format.
 * Best-lap + best-race indicator paths at orig actorRuntimeState._808/_1712
 * with side-effect writes (_780_4_=0, _830_2_=0xfc00, _877_1_=1) — port
 * lines 1789-1816 implement the lap comparison + race-end check via
 * g_pending_finish_timer + g_actor_best_lap/best_race globals.
 *
 * [ARCH-DIVERGENCE: QueueRaceHudFormattedText (D3D3 deferred queue) →
 *  td5_hud_queue_text (D3D11 immediate); per-view layout array offset
 *  vs struct field access] — orig indexes g_hudCurrentLayoutPtr via
 *  [8]/[0xB]/[10] integer indexes into the 14-float layout table; port uses
 *  named fields vp_int_left/vp_int_top/half_width. Same memory semantics.
 *  Orig also writes side-effect actor-state clears (lap-time taken
 *  indicators); port handles these via td5_input clear paths. Documented
 *  class-level in render_overlays comment lines 1887-1894. L5 promotion
 *  sweep 2026-05-21.
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

    /* [CONFIRMED orig DrawRaceStatusText @0x439B70] During View Replay (input
     * playback) the original EARLY-EXITS here: NO status text — the flashing
     * "REPLAY" banner is supplied by the overlay bitmask, not this path. The
     * previous port instead drew "DEMO MODE" whenever g_replay_mode!=0, which is
     * exactly why a real replay showed the DEMO-MODE banner. */
    if (g_replay_mode != 0) {
        return;
    }

    /* Attract demo: show "DEMO MODE" text (orig g_replayModeFlag!=0 path). */
    if (g_demo_mode != 0) {
        td5_hud_queue_text(0,
            (int)vp_half_w,
            (int)(vp_top + 16.0f),
            1, /* centered */
            "%s", s_hud_string_table[10]); /* "DEMO MODE" */
        return;
    }

    /* Wanted mode messages */
    if (g_td5.wanted_mode_enabled != 0 && s_is_first_player != 0 && g_wanted_msg_timer < 300) {
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

    /* Time trial timer — text "TIME mm:ss.hhh" at top-left.
     * In P2P races (g_special_encounter), the canonical readout is the
     * 3-digit dialed countdown widget (bit 0x40 metric-digits dispatch
     * below) so we don't draw the cumulative text timer here — that
     * duplicated/overlapped the position label. */
    if (g_td5.time_trial_enabled) {
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
 *
 * L5 promotion sweep audit (2026-05-18, TD5_pool0 read-only) -- master
 * HUD dispatcher is structurally faithful to orig 3854 bytes
 * (0x004388A0..0x004397AE). The function is large because it inlines
 * every HUD widget; the per-widget submission order matches orig
 * 1:1. Two ARCH-DIVERGENCEs documented.
 *
 * Per-widget mapping (orig flag bits in DAT_004B0BFC -> port hud flags):
 *
 *   Bit 0  (0x01) -- Position label
 *     orig: QueueRaceHudFormattedText(0, vpL+8, vpT+8, 0,
 *           s_hudFontGlyphAtlasPtr + (actor.field_0x383 * 4));
 *     port: td5_hud_queue_text() at vp_int_left+8, vp_int_top+8 with
 *           s_hud_string_table[pos]. byte-faithful.
 *
 *   Bit 1  (0x02) -- Lap-timer column (6 slots)
 *     orig: 6-slot loop reading (actor.field_0x34C [slot 0] or
 *           DAT_004AB454 + slot*2) as 30-Hz ticks, formatted as
 *           "%s %02d:%02d.%02d" / "%02d:%02d.%02d" -- first slot anchors
 *           on right-120, rest on right-80 with 16px row stride.
 *     port: identical right-anchor layout, td5_hud_queue_text with
 *           actor_lap_time(slot,r) -> 100/30 conversion to centiseconds.
 *           byte-faithful.
 *
 *   Bit 2  (0x04) -- Speedometer (dial+needle+gear+digits)
 *     orig: needle angle = (rpm*0xA5A)/maxRPM + 0x400; cos/sin float
 *           LUT (CosFloat12bit, SinFloat12bit); 4-vertex quad at
 *           +0xE7 in the quad buffer; speed_display = (long_speed *
 *           256 + 625)/1252 [MPH] or +389/778 [KPH]; up to 3 digits
 *           emitted right-to-left from anchor at (vp_int_right - 60,
 *           vp_int_bottom - 23 - 8) with 17px step, 16x24 cells from
 *           SPEEDOFONT atlas; gear quad from GEARNUMBERS @ +0xB.
 *     port: identical formula end-to-end (engine_speed*0xA5A/max_rpm +
 *           0x400; td5_cos_12bit / td5_sin_12bit; same MPH/KPH math at
 *           lines 1964-1968; 3-digit anchor with sf_step at line 1976).
 *           hud_build_quad / hud_submit_quad replace BuildSpriteQuad
 *           Template / SubmitImmediateTranslucentPrimitive.
 *           [CONFIRMED @ orig 0x00438A40-0x00438C40 vs td5_hud.c:1859-
 *           2047.]
 *
 *   Bit 4  (0x10) -- U-turn warning + counter
 *     orig: counter increments on cur_span < prev_span, clears on
 *           prev_span < cur_span; flashes when counter > 2 and
 *           heading_delta in (0x3FF, 0xC00) at 8Hz (tick & 0x1F > 8).
 *     port: identical at lines 2082-2110 -- same span counter
 *           semantics, same heading_delta range, same flash cadence.
 *
 *   Bit 6  (0x40) -- Metric/odometer digits
 *     orig: gated on g_specialEncounterType != 0; call
 *           BuildRaceHudMetricDigits + 3 or 4 submits at quad offsets
 *           +0x115 / +0x143 / +0x171 (hundreds/tens/ones) + +0x1CD in
 *           odometer mode (DAT_00473E30 == 4).
 *     port: td5_hud_build_metric_digits() + same offset submits at
 *           lines 2067-2079. byte-faithful.
 *
 *   Bit 7  (0x80) -- Total timer
 *     orig: QueueRaceHudFormattedText(... vpL+8, vpT+0x18, "%s %d").
 *     port: td5_hud_queue_text() at vp_int_left+8, vp_int_top+24 with
 *           s_hud_string_table[11]. byte-faithful.
 *
 *   Bit 8  (0x100) -- Lap/checkpoint counter
 *     orig: vpL+8, vpT+8, "%s %d".
 *     port: identical at lines 1820-1826.
 *
 *   Bit 9  (0x200) -- Circuit-lap progress
 *     orig: vpL+8, vpT+0x28, "%s %d/%d".
 *     port: identical at lines 1809-1817.
 *
 *   Bit 31 (0x80000000) -- Replay banner
 *     orig: SubmitImmediateTranslucentPrimitive at quad offset +0x1FB
 *           when g_simulationTickCounter & 0x20.
 *     port: hud_submit_quad at view_base + 0x7EC when
 *           g_tick_counter & 0x20. byte-faithful (offset 0x7EC is the
 *           port's expanded quad pool, same logical slot as orig
 *           +0x1FB).
 *
 *   Indicator digit (countdown / finish):
 *     orig: per-view DAT_004B11A8 entry -1 = digit index (col=%5,
 *           row=/5), 16x24 cell from NUMBERS atlas, centered at
 *           pfVar9[2..3] - (0x0E, 0x0C) offsets.
 *     port: s_indicator_state[v] -> same col/row decomposition; centred
 *           on (vp_center_x - 16*sx, vp_center_y - 24*sy). byte-faithful.
 *
 *   Tail of per-view loop:
 *     orig: minimap RenderTrackMinimapOverlay (single-player + bit 0x10
 *           + non-circuit + no race-end fade);
 *           SetProjectedClipRect(0, render_w, 0, render_h);
 *           SetProjectionCenterOffset(render_cx, render_cy);
 *           FlushQueuedRaceHudText();
 *           RenderHudRadialPulseOverlay(param_1);
 *           if (split_screen_mode) submit divider at DAT_004B0EF8 +
 *               mode * 0xB8.
 *     port: same restore order -- td5_hud_render_minimap +
 *           td5_render_set_clip_rect/center, td5_hud_flush_text,
 *           td5_render_radial_pulse, divider quad submission. Gates
 *           radial pulse on additional !network && !drag_race conditions
 *           (orig RunRaceFrame @ 0x42B67F also has these gates -- the
 *           port lifts them from the caller into the renderer; net same
 *           observable behaviour).
 *
 * [ARCH-DIVERGENCE: orig BuildSpriteQuadTemplate emits 4-vertex
 * structures into a fixed scratch region (DAT_004B0BFC + quad-offset)
 * consumed by SubmitImmediateTranslucentPrimitive (D3D3 immediate-mode
 * translucent primitive queue with DDraw surface keys). Port replaces
 * the scratch ring with a per-view dynamic quad buffer
 * (s_hud_prim_storage) consumed by hud_submit_quad ->
 * td5_plat_render_draw_tris (D3D11 immediate-mode tri stream). Same
 * geometry, different submission model.]
 *
 * [ARCH-DIVERGENCE: orig's per-view scale/layout reads from
 * DAT_004B1160-stride table (14 floats per view); port reads from
 * s_view_layout[TD5_HudViewLayout] structs with named fields
 * (.vp_int_left/.scale_x/.center_x/etc). Equivalent storage, named
 * for clarity; same indexing semantics. The DAT_004B112C / DAT_004B11B4
 * speedo/digit atlas-anchor pointers are replaced by
 * s_gearnumbers_atlas / s_speedofont_atlas / s_numbers_atlas struct
 * pointers loaded at HUD init.]
 *
 * Cosmetic additions in port (not in orig):
 *  - Debug overlay (g_td5.ini.debug_overlay) draws FPS + POS + YAW/
 *    PITCH/ROLL + SPD + RPM + GEAR + STEER + SUSP + WHEELS + SPAN
 *    text. Gated by INI; zero impact when disabled.
 *  - hud_layout once-only INFO log at line 1748.
 *  - speedometer per-frame INFO log at line 1892.
 *  - metric-digits gate diagnostic log at line 2057.
 *
 * No code edit needed. Effective level after audit: L5 +
 * [ARCH-DIVERGENCE] (2 items above) for D3D3 queue -> D3D11 immediate-
 * mode and for the per-view layout table representation.
 * ======================================================================== */

/* [PORT: N-way] Recompute the (global) minimap layout for one split-screen
 * pane so the minimap can be drawn inside each viewport instead of once at a
 * fixed full-screen position. Mirrors td5_hud_init_minimap_layout but uses the
 * per-view scale + pane bounds. For a single full-screen view this reproduces
 * the init values exactly. */
static void hud_set_minimap_for_view(int v)
{
    TD5_HudViewLayout *vl = &s_view_layout[v];
    float sx = vl->scale_x, sy = vl->scale_y;
    s_minimap_width   = sx * 100.0f;
    s_minimap_height  = sy * 100.0f;
    s_minimap_dot_size = sx * 7.0f;
    s_minimap_x = vl->vp_int_left + sx * 8.0f;
    s_minimap_y = vl->vp_int_bottom - s_minimap_height - sy * 8.0f;
    s_minimap_world_scale_x = sx * (1.0f / 1024.0f);
    s_minimap_world_scale_y = sy * (1.0f / 1024.0f);
    s_minimap_tile_width  = s_minimap_width;
    s_minimap_tile_height = s_minimap_height;
}

/* [PORT: N-way] Draw thin divider lines between split-screen panes matching the
 * viewport ladder grid. Replaces the legacy single-quad divider (which only
 * handled the 2-view case and used degenerate coords). */
static void hud_draw_split_dividers(void)
{
    if (g_td5.viewport_count <= 1) return;
    int views = g_td5.viewport_count;
    int cols, rows;
    /* Same shared resolver as the viewport + HUD layouts so the divider lines
     * land on the actual pane seams (vertical lines for a LEFT/RIGHT grid,
     * horizontal for UP/DOWN) instead of a hardcoded orientation. */
    td5_game_resolve_split_grid(views, &cols, &rows);

    TD5_AtlasEntry *colours = td5_asset_find_atlas_entry(NULL, "COLOURS");
    if (!colours) return;
    float cu = (float)colours->atlas_x + 0.5f;
    float cv = (float)colours->atlas_y + 0.5f;
    int   ctex = colours->texture_page;
    float W = g_render_width_f, H = g_render_height_f;
    const float t = 1.0f;   /* half-thickness (~2px) */
    TD5_SpriteQuad q;

    for (int c = 1; c < cols; c++) {
        float x = (float)c * (W / (float)cols);
        hud_build_quad(&q, 0, ctex, x - t, 0.0f, x + t, H,
                       cu, cv, cu, cv, 0xFF000000, HUD_DEPTH2);
        hud_submit_quad(&q);
    }
    for (int r = 1; r < rows; r++) {
        float y = (float)r * (H / (float)rows);
        hud_build_quad(&q, 0, ctex, 0.0f, y - t, W, y + t,
                       cu, cv, cu, cv, 0xFF000000, HUD_DEPTH2);
        hud_submit_quad(&q);
    }
}

/* ------------------------------------------------------------------------
 * Vectorized speedometer / tachometer arc (VectorUI)
 *
 * Replaces the baked SPEEDO texture (tpage4 / GDI fallback) with a crisp
 * analytic SDF reproduction that stays sharp at any resolution. Faithful to
 * the original art (extracted from tpage4.dat): an ARC of tick marks (NOT a
 * filled dial, no full ring -- the centre is transparent), with a static red
 * "bar" baked at the top end. Only the needle (drawn separately) animates.
 *
 * Geometry from the real texture (dial centred, 96px, radius ~47):
 *   - tick arc sweeps 90deg (the "0" at 6 o'clock, just left of the speed
 *     digits) clockwise up the left + over the top to ~282deg;
 *   - 8 major ticks (longer) with fine minor ticks between -- "8 subdivisions";
 *   - a solid RED bar from ~282deg to 322.9deg (the redline, above the gear).
 * Angles are screen degrees (angle*360/4096; screen Y-down, CW). The full
 * 90..322.9 span is exactly the needle's RPM range (0x400 .. 0x400+0xA5A), so
 * the needle sweeps the whole arc and points into the red bar at redline.
 *
 * cx/cy is the centre in render px; drawn as a true circle (radius from sy)
 * so it stays circular at any aspect ratio. ------------------------------*/
static void hud_vector_speedo_tach(float cx, float cy, float sy)
{
    const float A0 = (float)0x400 * 360.0f / 4096.0f;           /* 90.0  (idle)   */
    const float A1 = (float)(0x400 + 0xA5A) * 360.0f / 4096.0f; /* 322.9 (redline)*/
    const float RL_FRAC = 0.80f;                  /* red zone = top 20% of the arc */
    float rl = A0 + RL_FRAC * (A1 - A0);          /* red zone start angle (~276)   */

    TD5_VuiGauge g;
    memset(&g, 0, sizeof(g));
    g.cx = cx;  g.cy = cy;
    g.radius       = 50.0f * sy;      /* outer semi-transparent disc (no border)         */
    g.inner_radius = 15.0f * sy;      /* concentric inner circle -> 3D recessed look     */
    g.tick_out     = 46.0f * sy;      /* tooth outer radius (near the rim)               */
    g.sweep_start_deg = A0;           /* "0" tooth at 6 o'clock (left of the digits)     */
    g.sweep_end_deg   = A1;           /* subdivisions span the FULL arc incl. the red    */
    g.tick_count   = 33;              /* 9 majors (every 4th) => 8 subdivisions + minors */
    g.major_every  = 4;
    g.major_len_px = 9.0f * sy;       /* long major teeth                                */
    g.minor_len_px = 5.0f * sy;       /* short minor teeth                               */
    g.pivot_px     = 3.0f * sy;       /* small needle-pivot hub                          */
    /* Red zone: teeth here render RED and a red arc runs along the rim. The
     * last subdivision falls inside it (top 20% of the sweep). */
    g.redline_start_deg = rl;
    g.redline_end_deg   = A1;
    g.rim_red_px   = 4.0f * sy;
    g.face_color   = 0x80181820u;     /* semi-transparent dark disc (no border line)     */
    g.inner_color  = 0xC00A0A10u;     /* darker inner disc -> recessed 3D centre         */
    g.tick_color   = 0xFFECECECu;     /* white teeth                                     */
    g.redline_color= 0xFFE0202Au;     /* red teeth + red rim                             */
    g.pivot_color  = 0xFF8A8A8Au;     /* grey hub                                        */
    td5_vui_gauge(&g);
}

/* Draw one speed digit (0-9) from the Technology-font SDF (a 7-segment LCD
 * digital typeface baked into the HUD-font SDF page at a 5x2 grid of 30x48
 * cells, top-left). SPEEDOFONT is empty in the asset; this gives a real digital
 * speedometer font, crisp at any resolution and tinted via `color`. */
static void hud_vector_speed_digit(int d, float x, float y, float w, float h, uint32_t color)
{
    int page = td5_vui_hudfont_page();
    if (page < 0 || d < 0 || d > 9) return;
    int col = d % 5, row = d / 5;
    float gx = (float)(col * 30), gy = (float)(row * 48);
    const float INV = 1.0f / 256.0f;
    td5_vui_msdf_quad(x, y, w, h, color, page,
                      (gx + 0.5f) * INV, (gy + 0.5f) * INV,
                      (gx + 29.5f) * INV, (gy + 47.5f) * INV);
}

void td5_hud_render_overlays(float dt)
{
    /* dt is normalized 30 Hz frame time from td5_game.c. */
    s_cur_view = 0;

    for (int v = 0; v < s_view_count; v++) {
        s_cur_view = v;
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

        uint8_t *view_base = s_hud_prim_storage + (size_t)s_cur_view * HUD_VIEW_BLOCK;

        /* [REMOVED 2026-06-05] The TD6 synthesized "CHECKPOINT n/N" drive-through
         * banner (centered, ~2.5 s) was drawn here. User did not want a checkpoint
         * indicator at the top of the screen when passing a checkpoint, so the
         * draw block and its flash-timer state (s_td6_cp_flash_*) + setter were
         * removed. The underlying split-time checkpoint registration in td5_game.c
         * is unaffected — only the on-screen acknowledgement is gone. */

        /* --- Bit 0: Race position label --- */
        if (flags & TD5_HUD_POSITION_LABEL) {
            uint8_t pos = actor_race_position(actor_slot);
            int px = (int)(vl->vp_int_left + 8.0f);
            int py = (int)(vl->vp_int_top + 8.0f);
            if (pos < 6) {
                td5_hud_queue_text(0, px, py, 0, "%s", s_hud_string_table[pos]);
            } else {
                /* [PORT: N-way] The SNK position table only has 1ST..6TH; for a
                 * >6-racer field generate the ordinal (positions 7..16 are all
                 * "TH"). Without this, 7th+ read the next table entries
                 * ("WRONG WAY", "PIT STOP", ...). */
                td5_hud_queue_text(0, px, py, 0, "%dTH", (int)pos + 1);
            }
        }

        /* --- Bit 7: Total timer "%s %d" / cop-chase POINTS --- */
        if (flags & TD5_HUD_TOTAL_TIMER) {
            if (g_td5.wanted_mode_enabled) {
                /* Cop chase: bit 0x80 is the "POINTS" line (the ram score —
                 * +10 light hit / +20 heavy / +50 bust), NOT a timer. Confirmed
                 * against the original's on-screen HUD ("POINTS N" under
                 * "ARRESTS N"). [FIX 2026-05-31] The score accrues to the
                 * player/cop at accumulated_score (+0x2C8). */
                td5_hud_queue_text(0,
                    (int)(vl->vp_int_left + 8.0f),
                    (int)(vl->vp_int_top + 24.0f),
                    0,
                    "POINTS %d", (int)td5_game_get_result_secondary(0));
            } else {
                td5_hud_queue_text(0,
                    (int)(vl->vp_int_left + 8.0f),
                    (int)(vl->vp_int_top + 24.0f),
                    0,
                    "%s %d", s_hud_string_table[11], 0 /* timer value */);
            }
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
            if (g_td5.wanted_mode_enabled) {
                /* Cop chase: bit 0x100 is the "ARRESTS" line (the bust count =
                 * g_wantedArrestCounter @ 0x004bead0, incremented each time a
                 * suspect's damage bar is fully depleted). Label confirmed
                 * against the original's on-screen HUD ("ARRESTS N", top line).
                 * [FIX 2026-05-31 — was the guessed "BUSTS".] */
                td5_hud_queue_text(0,
                    (int)(vl->vp_int_left + 8.0f),
                    (int)(vl->vp_int_top + 8.0f),
                    0,
                    "ARRESTS %d", td5_game_get_wanted_kills(0));
            } else {
                td5_hud_queue_text(0,
                    (int)(vl->vp_int_left + 8.0f),
                    (int)(vl->vp_int_top + 8.0f),
                    0,
                    "%s %d", s_hud_string_table[11], 0);
            }
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

            /* When the vectorized dial is active it is drawn as a true CIRCLE
             * (radius from sy), so the needle uses a uniform sy scale on both
             * axes to stay inside the circular face. Without it (baked-dial
             * fallback) the needle keeps the original elliptical sx/sy scale to
             * match the stretched 96x96 texture. */
            int   use_vec = td5_vui_gauge_available();
            /* Vector dial: circular needle (uniform sy scale) so it stays inside
             * the circular SDF dial. Baked-dial fallback keeps the faithful
             * elliptical sx/sy needle. */
            float nsx = use_vec ? sy : sx;
            float nsy = sy;

            /* V0: near end (9 units into dial), V2: far tip (45 units out) */
            float near_x = cx - cos_a * nsx * 9.0f;
            float near_y = cy - sin_a * nsy * 9.0f;

            float base_offset_x = sin_a * nsx * 2.0f;
            float base_offset_y = cos_a * nsy * 2.0f;

            float tip_x = cx + cos_a * nsx * 45.0f;
            float tip_y = cy + sin_a * nsy * 45.0f;

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
             * 8 slots × 16px wide). Real atlas order matches gear byte:
             * 0=R, 1=N, 2=1st, 3=2nd, 4=3rd, 5=4th, 6=5th, 7=6th.
             * No remapping needed — atlas_idx == gear byte. */
            uint8_t gear = actor_gear(actor_slot);
            uint8_t atlas_idx = gear & 7;
            {
                float gu = (float)atlas_idx * 16.0f + (float)s_gearnumbers_atlas->atlas_x;
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

            /* Compute speed from actual world velocity, not CRGT pseudo-speed.
             * actor+0x314 (longitudinal_speed) is RPM-encoded by CRGT for RWD/AWD
             * and drops to 0 when throttle is released, even though the car is
             * still moving. Instead, project world velocity onto heading. */
            uint8_t *_actor_a = (uint8_t *)actor_ptr(actor_slot);
            int32_t vx = *(int32_t *)(_actor_a + 0x1CC);  /* linear_velocity_x */
            int32_t vz = *(int32_t *)(_actor_a + 0x1D4);  /* linear_velocity_z */
            int32_t heading_accum = *(int32_t *)(_actor_a + 0x1F4); /* euler_accum.yaw */
            int32_t h12 = (heading_accum >> 8) & 0xFFF;
            double hrad = (double)h12 * (2.0 * 3.14159265358979323846 / 4096.0);
            int32_t sin_h = (int32_t)(sin(hrad) * 4096.0);
            int32_t cos_h = (int32_t)(cos(hrad) * 4096.0);
            int32_t speed_raw = (vx * sin_h + vz * cos_h) >> 12; /* body-frame longitudinal */
            /* Original clamps negative speed to 0 [CONFIRMED @ 0x4388A0].
             * Previous code took abs(), showing speed while reversing. */
            if (speed_raw < 0) speed_raw = 0;
            speed_raw >>= 8;

            /* [S01 2026-06-04] Units from the live Display-options setting
             * (g_td5.ini.speed_units; 0=MPH, 1=KPH). g_kph_mode was initialized
             * to 0 and NEVER synced to the saved preference, so the speedo always
             * showed MPH regardless of the menu choice — read the INI value and
             * keep g_kph_mode mirrored for any other reader. The MPH round-to-
             * nearest bias is +626 (orig +0x272 @0x438ed4), not 625. */
            int speed_display;
            g_kph_mode = g_td5.ini.speed_units;
            if (g_kph_mode == 0) {
                speed_display = (speed_raw * 256 + 626) / 1252; /* MPH [CONFIRMED @0x438ed4] */
            } else {
                speed_display = (speed_raw * 256 + 389) / 778;  /* KPH [CONFIRMED @0x438ebc] */
            }

            /* Build and submit speed digit quads using SPEEDOFONT atlas
             * (linear strip of 10 digits, 16px wide each at atlas (96,160)).
             * Original renders right-to-left: ones at anchor, each additional
             * digit subtracts (glyph_w + 2.0) moving LEFT (0x438E90).
             * Inter-digit gap is 2.0 [CONFIRMED @ 0x45d6d8]. */
            /* Bigger digits, kept at the same spot (left anchor + original
             * bottom edge, growing upward). */
            float sf_gw = sx * 19.0f;
            float dh    = sy * 32.0f;
            float sf_step = sf_gw + 2.0f;
            float sf_x0 = vl->vp_int_right - sx * 60.0f;
            float sf_y1 = vl->vp_int_bottom - sy * 7.0f;   /* original bottom edge */
            float sf_y0 = sf_y1 - dh;
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

            /* Submit speedo dial: crisp SDF dial + RPM tach arc when VectorUI
             * is available, otherwise the baked 96x96 GDI dial texture. Drawn
             * BEFORE the needle (immediate-mode call order = z-order) so the
             * needle/digits paint on top of the dial face. */
            if (use_vec) {
                hud_vector_speedo_tach(cx, cy, sy);
            } else {
                hud_submit_quad(view_base + SPEEDO_QUAD_OFF);
            }
            /* Submit needle */
            hud_submit_quad(view_base + 0x39C);

            /* Speed digits: VectorUI draws crisp GREEN digits from the NUMBERS
             * SDF (SPEEDOFONT is empty in the asset); otherwise the baked
             * SPEEDOFONT quads. Right-to-left: ones at anchor, then tens/hundreds. */
            const uint32_t SPEED_GREEN = 0xFF3CDC3Cu;
            float dx = ones_x;
            if (use_vec) {
                hud_vector_speed_digit(ones, dx, sf_y0, sf_gw, dh, SPEED_GREEN);
            } else {
                float u_ones = digit_u_base + (float)ones * 16.0f + 0.5f;
                hud_build_quad(view_base + SPEEDFONT_BASE_OFF, 0, sf_pg,
                    dx, sf_y0, dx + sf_gw, sf_y1,
                    u_ones, digit_v_base + 0.5f, u_ones + 15.5f, digit_v_base + 23.5f,
                    0xFFFFFFFF, HUD_DEPTH);
                hud_submit_quad(view_base + SPEEDFONT_BASE_OFF);
            }

            /* Tens digit if speed >= 10 (one step LEFT of ones) */
            if (speed_display >= 10) {
                dx = ones_x - sf_step;
                if (use_vec) {
                    hud_vector_speed_digit(tens_val, dx, sf_y0, sf_gw, dh, SPEED_GREEN);
                } else {
                    float u_tens = digit_u_base + (float)tens_val * 16.0f + 0.5f;
                    hud_build_quad(view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE, 0, sf_pg,
                        dx, sf_y0, dx + sf_gw, sf_y1,
                        u_tens, digit_v_base + 0.5f, u_tens + 15.5f, digit_v_base + 23.5f,
                        0xFFFFFFFF, HUD_DEPTH);
                    hud_submit_quad(view_base + SPEEDFONT_BASE_OFF + TD5_HUD_GLYPH_QUAD_SIZE);
                }

                /* Hundreds digit if speed >= 100 (two steps LEFT of ones) */
                if (speed_display >= 100) {
                    dx = ones_x - 2.0f * sf_step;
                    if (use_vec) {
                        hud_vector_speed_digit(hundreds_val, dx, sf_y0, sf_gw, dh, SPEED_GREEN);
                    } else {
                        float u_hund = digit_u_base + (float)hundreds_val * 16.0f + 0.5f;
                        hud_build_quad(view_base + SPEEDFONT_BASE_OFF + 2 * TD5_HUD_GLYPH_QUAD_SIZE, 0, sf_pg,
                            dx, sf_y0, dx + sf_gw, sf_y1,
                            u_hund, digit_v_base + 0.5f, u_hund + 15.5f, digit_v_base + 23.5f,
                            0xFFFFFFFF, HUD_DEPTH);
                        hud_submit_quad(view_base + SPEEDFONT_BASE_OFF + 2 * TD5_HUD_GLYPH_QUAD_SIZE);
                    }
                }
            }

            /* Gear indicator: VectorUI draws a gold gear character from the FONT
             * SDF (GEARNUMBERS is empty in the asset); otherwise the baked quad. */
            /* Submit gear indicator */
            hud_submit_quad(view_base + GEAR_QUAD_OFF);
        }

        /* --- Bit 6: Metric digit display ---
         * Original RenderRaceHudOverlays @ 0x004391CC gates this on
         * g_specialEncounterType != 0 — the metric-digits widget is
         * the P2P checkpoint countdown sprite block (digits at quad
         * offsets +0x115/+0x143/+0x171, +0x1cd in odometer mode). */
        {
            static int s_log_div = 0;
            if ((++s_log_div % 60) == 0) {
                TD5_LOG_I(LOG_TAG,
                          "metric-digits gate: flags=0x%08X bit40=%d se=%d atlas=%p mode=%d val=%u",
                          (unsigned int)flags,
                          (int)((flags & TD5_HUD_METRIC_DIGITS) != 0),
                          g_special_encounter,
                          (void *)s_numbers_atlas,
                          g_hud_metric_mode,
                          (unsigned int)s_metric_value);
            }
        }
        if ((flags & TD5_HUD_METRIC_DIGITS) && g_special_encounter != 0 && s_numbers_atlas != NULL) {
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
            /* [FIX 2026-05-25 crash-compute-heading-delta-stride]: see matching
             * fix in td5_physics.c.  route_state stride is 0x47 DWORDS not BYTES;
             * use the helper that applies the correct stride.  route_idx is
             * captured but no longer multiplied in (helper re-derives slot from
             * the route_state's own RS_SLOT_INDEX). */
            (void)actor_route_index(actor_slot); /* side-effect: ensure index resolves */
            extern int32_t *td5_ai_get_route_state(int slot);
            int32_t *rs_hud = td5_ai_get_route_state(actor_slot);
            uint32_t heading_delta = rs_hud ? td5_compute_heading_delta(rs_hud) : 0;

            /* [FIX 2026-06-05b wrong-way detection — THE real bug]
             * td5_compute_heading_delta returns the NEGATED 12-bit route-vs-yaw
             * angle (its last op is `return 0u - t`), so the raw uint32 is either
             * 0 (car aligned with the route) or 0xFFFFFxxx (off-axis). The old
             * port test compared that RAW value against (0x3FF,0xC00) and so could
             * essentially never match (0xFFFFFxxx > 0xC00) — that is exactly why
             * "no wrong-way sign ever appears". Mask to 12 bits to recover the
             * real angular separation; the band is symmetric about 0x800 so it
             * correctly flags the car facing >90deg off-route. Confirmed live:
             * reversing gave heading_delta=0xFFFFF770 -> hd12=0x770 (in band) with
             * s_wrong_way_counter climbing 1..14. Kept the faithful two-signal
             * design: facing the wrong way AND sustained backward span progression
             * (the span counter, ++ when the span index drops below last frame's,
             * reset when it climbs). */
            uint32_t hd12 = heading_delta & 0xFFFu;
            int wrong_way = (hd12 > 0x3FF && hd12 < 0xC00) &&
                            (s_wrong_way_counter[v] > 2);

            if (wrong_way) {
                /* Slow, readable blink: ON for 48 of every 64 ticks (~75% duty),
                 * i.e. half the rate of the old (tick & 0x1F) > 8 cadence which
                 * read as a fast flicker (user feedback 2026-06-05). Mostly-on so
                 * the warning stays legible while still pulsing for attention. */
                if ((g_tick_counter & 0x3Fu) < 0x30u) {
                    /* Pre-baked U-turn icon (UTURN sprite, centered). */
                    hud_submit_quad(view_base + 0x67C);
                    /* [FIX 2026-06-05 wrong-way warning — source-port HUD, no
                     * original equivalent] The UTURN icon alone did not read as a
                     * clear "you are going the wrong way" sign (user feedback: "no
                     * sign when going in the wrong direction"), so also draw a
                     * prominent flashing "WRONG WAY" caption through the same
                     * VectorUI HUD text path as the rest of the overlay. Drawn
                     * per-viewport: positioned from THIS view's layout and gated by
                     * this view's s_wrong_way_counter[v], so each split-screen pane
                     * shows its own warning. s_hud_string_table[6] is the SNK
                     * "WRONG WAY" label (see s_default_position_strings[6]). */
                    td5_hud_queue_text(0,
                        (int)vl->center_x,
                        (int)(vl->vp_int_top +
                              (vl->vp_int_bottom - vl->vp_int_top) * 0.28f),
                        1,
                        "%s", s_hud_string_table[6]);
                }
                if ((g_tick_counter % 30u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                        "wrong-way warning view %d active: hd12=0x%X counter=%d",
                        v, (unsigned int)hd12, s_wrong_way_counter[v]);
                }
            }

            s_prev_span_pos[v] = (int)cur_span;
        }

        /* --- Bit 31: Replay banner (flashes every 32 ticks) --- */
        if ((flags & TD5_HUD_REPLAY_BANNER) && (g_tick_counter & 0x20)) {
            hud_submit_quad(view_base + 0x7EC);
        }

        /* --- Indicator digit (countdown/finish) --- */
        /* [2026-06-08] Clamp to a renderable single-digit cell. The NUMBERS atlas
         * is a 5x2 grid (cells 0-9); an indicator >9 (e.g. a spectator pane that
         * briefly carried a last-place car's race_position+2 during the fly-in)
         * would index row>=2, outside the glyph region, drawing a "blue square".
         * Suppress those instead of sampling garbage (mirrors the existing
         * countdown-timer indicator>=4 suppression in UpdateRaceCameraTransitionTimer). */
        if (s_indicator_state[v] >= 1 && s_indicator_state[v] <= 9 && s_numbers_atlas) {
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

        /* Finishing-position victory digit — drawn HERE inside the per-view loop
         * (the same render context as the speedo/indicator, which land correctly).
         * The post-per-view-loop HUD pass applied an ~(80,66) viewport-relative
         * offset that pushed the digit off-centre. Main view only. */
        if (v == 0 && g_td5.race_end_fade_state > 0) {
            extern int td5_game_get_victory_position(void);
            int fpos = td5_game_get_victory_position();
            if (fpos > 0) td5_hud_draw_finish_position(fpos);
        }

        s_cur_view++;
    }

    /* --- Minimap (per-view) [PORT: N-way + circuit enhancement] ---
     * Drawn inside each pane via the per-view minimap layout. For a single view
     * this is identical to the legacy full-screen minimap.
     * Faithful: P2P only (g_track_type_mode == 0). Port enhancement: also draw
     * on circuit tracks when [Game] CircuitMinimap is on (default) — the
     * renderer re-checks the knob and picks the circuit ring-walk path.
     * [REBASE MERGE: master's circuit-minimap knob + branch's per-view loop.] */
    if (g_track_type_mode == 0 || g_td5.ini.circuit_minimap) {
        for (int v = 0; v < s_view_count; v++) {
            if (!(*s_hud_flags[v] & TD5_HUD_UTURN_WARNING)) continue;
            hud_set_minimap_for_view(v);
            td5_hud_render_minimap(g_actor_slot_map[v]);
        }
    }

    /* Restore full-screen viewport and projection center.
     * Passing half-dims restores the projection origin to screen center, matching
     * RunRaceFrame's per-frame SetProjectionCenterOffset(gViewportCenterX, Y). */
    td5_render_set_clip_rect(0.0f, (float)g_render_width, 0.0f, (float)g_render_height);
    td5_render_set_projection_center(g_render_width_f * 0.5f, g_render_height_f * 0.5f);

    /* FPS/MS counter, top-LEFT — stacked directly BELOW the per-viewport race
     * POSITION label and ABOVE the debug overlay. [user request 2026-06-05]
     * Moved back from the S12 top-RIGHT anchor: the user wants it grouped in the
     * top-left corner under the race position. Left-aligned at x=8 so it lines up
     * with both the POSITION label (drawn at vp_int_left+8, top row y=8) and the
     * debug overlay column (x=8). The position label is one ~16px font-0 line at
     * y=8 (spans ~8..24), and the debug overlay starts at y=52, so y=24 sits one
     * line below position with a clear gap above debug. Drawn here (after the
     * full-screen viewport is restored) so the coords are render-target px.
     * In split-screen this sits below the TOP-LEFT pane's position label.
     * [S01 2026-06-04] gated by the Display-options Show FPS toggle. peak = worst
     * frame time over the last ~250ms. Both values refresh at 4 Hz (every 250ms);
     * values from td5_game_update_fps_overlay(). */
    if (g_td5.ini.show_fps) {
        char fps_buf[48];
        snprintf(fps_buf, sizeof(fps_buf), "FPS %.0f  %dMS",
                 (double)g_td5_display_fps, g_td5_peak_frame_ms);
        int fps_x = 8;
        int fps_y = 24;   /* below POSITION (y=8, ~16px tall), above debug (y=52) */
        { static int s_fps_logged = 0;   /* one-shot */
          if (!s_fps_logged) {
              s_fps_logged = 1;
              TD5_LOG_I(LOG_TAG, "race FPS overlay top-left: x=%d y=%d", fps_x, fps_y);
          } }
        td5_hud_queue_text(0, fps_x, fps_y, 0, "%s", fps_buf);
    }

    /* Flush queued text glyphs */
    td5_hud_flush_text();

    /* Debug overlay (gated by td5re.ini DebugOverlay setting) — FPS now lives in
     * the always-on counter above, so this block only carries POS/YAW/etc. */
    if (g_td5.ini.debug_overlay) {
        int dbg_slot = g_actor_slot_map[0];
        uint8_t *dbg_a = (uint8_t *)actor_ptr(dbg_slot);
        int dbg_y = 52;
        const int dbg_dy = 14;

        /* World position (24.8 fixed-point -> float) */
        int32_t wx = *(int32_t *)(dbg_a + 0x1FC);
        int32_t wy = *(int32_t *)(dbg_a + 0x200);
        int32_t wz = *(int32_t *)(dbg_a + 0x204);
        td5_hud_queue_text(0, 8, dbg_y, 0, "POS: %.1f %.1f %.1f",
                           (float)wx / 256.0f, (float)wy / 256.0f, (float)wz / 256.0f);
        dbg_y += dbg_dy;

        /* Euler angles: yaw(heading), pitch, roll — 20-bit accum, display as degrees */
        int32_t yaw_acc   = *(int32_t *)(dbg_a + 0x1F4);
        int32_t pitch_acc = *(int32_t *)(dbg_a + 0x1F8);
        int32_t roll_acc  = *(int32_t *)(dbg_a + 0x1F0);
        float yaw_deg   = (float)((yaw_acc >> 8) & 0xFFF) * (360.0f / 4096.0f);
        float pitch_deg = (float)((pitch_acc >> 8) & 0xFFF) * (360.0f / 4096.0f);
        float roll_deg  = (float)((roll_acc >> 8) & 0xFFF) * (360.0f / 4096.0f);
        td5_hud_queue_text(0, 8, dbg_y, 0, "YAW: %.1f  PITCH: %.1f  ROLL: %.1f",
                           yaw_deg, pitch_deg, roll_deg);
        dbg_y += dbg_dy;

        /* [WHEELGAP DIAG 2026-05-27] Per-wheel RENDERED world-Y minus GROUND
         * contact-Y, in world units (÷256). Wheels render rigidly at
         * render_pos + body_matrix*wheel_display_angles[w] (td5_render.c ~5054),
         * so gap = render_wheel_y - ground_y; positive => that wheel floats
         * above the road. Pins which wheels lift on slopes per orientation. */
        {
            float wpy = (float)(*(int32_t *)(dbg_a + 0x200)) / 256.0f;
            /* body->world Y = ROW 1 of rotation matrix: m[3],m[4],m[5]
             * (+0x12C/+0x130/+0x134). Matches the actual wheel render
             * (mat3x4 out[1]) and orig 0x0042E2E0. Was reading COL 1
             * (m[1],m[4],m[7]) → transposed, wrong gaps. [FIX 2026-05-27 PM-7] */
            float m3 = *(float *)(dbg_a + 0x12C);
            float m4 = *(float *)(dbg_a + 0x130);
            float m5 = *(float *)(dbg_a + 0x134);
            int gap[4];
            for (int w = 0; w < 4; w++) {
                int16_t wx = *(int16_t *)(dbg_a + 0x210 + w*8 + 0);
                int16_t wy = *(int16_t *)(dbg_a + 0x210 + w*8 + 2);
                int16_t wz = *(int16_t *)(dbg_a + 0x210 + w*8 + 4);
                float rot_y    = m3*(float)wx + m4*(float)wy + m5*(float)wz;
                float render_y = wpy + rot_y;
                float ground_y = (float)(*(int32_t *)(dbg_a + 0xF0 + w*12 + 4)) / 256.0f;
                gap[w] = (int)(render_y - ground_y);
            }
            td5_hud_queue_text(0, 8, dbg_y, 0,
                "WHEELGAP: FL=%d FR=%d RL=%d RR=%d",
                gap[0], gap[1], gap[2], gap[3]);
            dbg_y += dbg_dy;
        }

        /* Speed: longitudinal + lateral (body-frame, 8.8 fp) */
        int32_t long_spd = *(int32_t *)(dbg_a + 0x314);
        int32_t lat_spd  = *(int32_t *)(dbg_a + 0x318);
        td5_hud_queue_text(0, 8, dbg_y, 0, "SPD: fwd=%d  lat=%d",
                           long_spd >> 8, lat_spd >> 8);
        dbg_y += dbg_dy;

        /* Engine RPM + gear */
        int32_t rpm = *(int32_t *)(dbg_a + 0x310);
        uint8_t gear = dbg_a[0x36B];
        td5_hud_queue_text(0, 8, dbg_y, 0, "RPM: %d  GEAR: %d", rpm, (int)gear);
        dbg_y += dbg_dy;

        /* Steering command + brake flag + handbrake flag */
        int32_t steer = *(int32_t *)(dbg_a + 0x30C);
        uint8_t brake = dbg_a[0x36D];
        uint8_t hbrake = dbg_a[0x36E];
        td5_hud_queue_text(0, 8, dbg_y, 0, "STEER: %d  BRAKE: %s  HBRAKE: %s",
                           steer, brake ? "ON" : "OFF", hbrake ? "ON" : "OFF");
        dbg_y += dbg_dy;

        /* Suspension: roll (center) and pitch (wheel[0]) positions */
        int32_t susp_roll  = *(int32_t *)(dbg_a + 0x2CC);
        int32_t susp_pitch = *(int32_t *)(dbg_a + 0x2DC);
        td5_hud_queue_text(0, 8, dbg_y, 0, "SUSP: roll=%d  pitch=%d",
                           susp_roll, susp_pitch);
        dbg_y += dbg_dy;

        /* Collision / contact state */
        uint8_t wall_flag    = dbg_a[0x37B];  /* track_contact_flag: 0=none, 1=wall, 2=edge */
        uint8_t veh_mode     = dbg_a[0x379];  /* vehicle_mode: 0=normal, 1=recovery */
        uint8_t wheel_contact = dbg_a[0x37C]; /* wheel airborne mask THIS tick (NEW) */
        uint8_t dmg_lockout   = dbg_a[0x37D]; /* wheel airborne mask PREV tick (OLD snapshot) */

        const char *wall_str = "NONE";
        if (wall_flag == 1) wall_str = "WALL";
        else if (wall_flag == 2) wall_str = "EDGE";

        td5_hud_queue_text(0, 8, dbg_y, 0, "COL: %s  MODE: %s  DMG: %d",
                           wall_str,
                           veh_mode ? "RECOVERY" : "NORMAL",
                           (int)dmg_lockout);
        dbg_y += dbg_dy;

        td5_hud_queue_text(0, 8, dbg_y, 0, "WHEELS: %c%c%c%c",
                           (wheel_contact & 1) ? 'L' : '-',
                           (wheel_contact & 2) ? 'R' : '-',
                           (wheel_contact & 4) ? 'l' : '-',
                           (wheel_contact & 8) ? 'r' : '-');
        dbg_y += dbg_dy;

        /* Track span index + current sub-lane / lane count.
         * sub_lane = actor+0x8C (ACTOR_SUB_LANE_INDEX, signed); lane count is the
         * current span's geometry nibble via td5_track_get_span_lane_count. */
        int16_t span = *(int16_t *)(dbg_a + 0x82);
        td5_hud_queue_text(0, 8, dbg_y, 0, "SPAN: %d", (int)span);
        dbg_y += dbg_dy;

        int cur_sub_lane  = (int)*(int8_t *)(dbg_a + 0x8C);
        int cur_lane_cnt  = td5_track_get_span_lane_count((int)span);
        td5_hud_queue_text(0, 8, dbg_y, 0, "LANE: sub=%d  lanes=%d", cur_sub_lane, cur_lane_cnt);

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

    /* Split-screen divider bars [PORT: N-way grid] */
    hud_draw_split_dividers();
}

/* ========================================================================
 * RenderTrackMinimapOverlay (0x43A220)
 *
 * Renders the minimap for point-to-point tracks. Draws the track
 * segments, checkpoint markers, and racer dots relative to the player's
 * heading.
 * ======================================================================== */

/* Branch-collapse a raw strip-span index into the main-road linear range,
 * replicating the value the original stores at actor+0x82
 * (track_span_normalized) via ResolveActorSegmentBoundary @ 0x00443FF0
 * (store @ 0x0044400d). Main-road spans (raw < ring_length) pass through
 * unchanged; branch spans (raw >= ring_length) map back using the appended
 * branch-mirror rows of the minimap segment table — the same collapse the
 * player's remapped_span uses for centering. Used by the actor/traffic dot
 * filters so vehicles on a DIFFERENT branch than the player aren't culled by
 * the +/-144 span gate (the port can't read +0x82 directly: it isn't kept
 * current per-frame for racers, only on traffic recycle). */
static int minimap_normalize_span(int raw_span)
{
    int ring_length = g_td5.track_span_ring_length;
    if (ring_length <= 0) ring_length = g_strip_span_count;
    int norm = raw_span;
    if (norm >= ring_length &&
        s_minimap_seg_branch_start < s_minimap_seg_primary_end) {
        for (int si = s_minimap_seg_branch_start;
             si < s_minimap_seg_primary_end; si++) {
            if (norm <= (int)s_minimap_seg_end[si]) {
                norm += (int)s_minimap_seg_branch[si]
                      - (int)s_minimap_seg_start[si];
                break;
            }
        }
    }
    return norm;
}

/* [FIX 2026-06-05 Rome end-of-race stray minimap line — source-port HUD, no
 * original equivalent] True when a built minimap road quad's on-screen extent
 * dwarfs the whole minimap (here: > 2.5x its width/height). A legitimate per-span
 * road quad spans only a few px at minimap scale; a quad this large means its
 * NEAR and FAR spans are geometrically DISCONTINUOUS in world space. That happens
 * on the TD6 P2P tracks (e.g. Rome) whose strip continues PAST the finish line
 * into a disconnected out-of-bounds tail: a quad bridging the last on-route span
 * and the first tail span stretches a long stray line clear across the minimap
 * right when the player reaches the finish ("at the end of the race"). The quad's
 * AABB still overlaps the minimap rect (one corner is on-route), so the existing
 * fully-offscreen cull does NOT drop it — this extent test does. Rejecting these
 * is purely additive to the faithful path: real road quads never approach this
 * size, so faithful TD5 minimaps are unchanged. */
static int minimap_quad_is_stray(float min_x, float max_x, float min_y, float max_y)
{
    return (max_x - min_x) > 2.5f * s_minimap_width ||
           (max_y - min_y) > 2.5f * s_minimap_height;
}

/* Emit one minimap "checkpoint-connector" road quad spanning [span_a .. span_a+2].
 * BOTH edges use the span record's +0x06 back-vertex (orig Quad3 @ 0x43a9b1/0x43aa45,
 * Quad4 @ 0x43ac51/0x43ad40 — unlike the primary quad which uses +0x04 for the near
 * edge). The right-edge col1 delta is indexed by span_a's type byte for BOTH edges
 * (the orig carries cp's type in EDI across both right-edge computations). Shading
 * matches the port's primary road quad (HUD_WHITE_TEX_PAGE, 0xFF9A9A9A). Returns 1
 * if a quad was submitted, 0 if skipped (bad index / fully offscreen).
 * [CONFIRMED @ 0x43a9a9-0x43aeb4]. */
static int minimap_emit_connector(uint8_t *span_base, uint8_t *vert_base, int span_a,
                                  float offset_x, float offset_z,
                                  float cos_h, float sin_h,
                                  float mm_cx, float mm_cy)
{
    int span_b = span_a + 2;
    if (!span_base || !vert_base) return 0;
    if (span_a < 0 || span_b < 0 ||
        span_a >= g_strip_span_count || span_b >= g_strip_span_count) return 0;

    uint8_t *sa = span_base + span_a * 24;
    uint8_t *sb = span_base + span_b * 24;
    int32_t ox_a = *(int32_t *)(sa + 0x0C), oz_a = *(int32_t *)(sa + 0x14);
    int32_t ox_b = *(int32_t *)(sb + 0x0C), oz_b = *(int32_t *)(sb + 0x14);

    int32_t col1 = s_minimap_vtx_delta_col1[span_base[span_a * 24] & 0x07];

    uint16_t vi_a_l = *(uint16_t *)(sa + 0x06);
    int32_t  vi_a_r = (int32_t)vi_a_l + (int32_t)(sa[3] & 0x0F) + col1;
    uint16_t vi_b_l = *(uint16_t *)(sb + 0x06);
    int32_t  vi_b_r = (int32_t)vi_b_l + (int32_t)(sb[3] & 0x0F) + col1;
    if (vi_a_r < 0 || vi_b_r < 0) return 0;

    int16_t *va_l = (int16_t *)(vert_base + (uint32_t)vi_a_l * 6);
    int16_t *va_r = (int16_t *)(vert_base + (uint32_t)vi_a_r * 6);
    int16_t *vb_l = (int16_t *)(vert_base + (uint32_t)vi_b_l * 6);
    int16_t *vb_r = (int16_t *)(vert_base + (uint32_t)vi_b_r * 6);

    /* TL=front-left (span_a left), BL=back-left (span_b left),
     * BR=back-right (span_b right), TR=front-right (span_a right). */
    float wx_tl = (float)((int)va_l[0] + ox_a) + offset_x;
    float wz_tl = (float)((int)va_l[2] + oz_a) + offset_z;
    float wx_tr = (float)((int)va_r[0] + ox_a) + offset_x;
    float wz_tr = (float)((int)va_r[2] + oz_a) + offset_z;
    float wx_bl = (float)((int)vb_l[0] + ox_b) + offset_x;
    float wz_bl = (float)((int)vb_l[2] + oz_b) + offset_z;
    float wx_br = (float)((int)vb_r[0] + ox_b) + offset_x;
    float wz_br = (float)((int)vb_r[2] + oz_b) + offset_z;

    float tl_x = mm_cx + (wx_tl * cos_h + wz_tl * sin_h) * s_minimap_world_scale_x;
    float tl_y = mm_cy + (wz_tl * cos_h - wx_tl * sin_h) * s_minimap_world_scale_y;
    float tr_x = mm_cx + (wx_tr * cos_h + wz_tr * sin_h) * s_minimap_world_scale_x;
    float tr_y = mm_cy + (wz_tr * cos_h - wx_tr * sin_h) * s_minimap_world_scale_y;
    float bl_x = mm_cx + (wx_bl * cos_h + wz_bl * sin_h) * s_minimap_world_scale_x;
    float bl_y = mm_cy + (wz_bl * cos_h - wx_bl * sin_h) * s_minimap_world_scale_y;
    float br_x = mm_cx + (wx_br * cos_h + wz_br * sin_h) * s_minimap_world_scale_x;
    float br_y = mm_cy + (wz_br * cos_h - wx_br * sin_h) * s_minimap_world_scale_y;

    float mm_l = s_minimap_x, mm_t = s_minimap_y;
    float mm_r = s_minimap_x + s_minimap_width, mm_b = s_minimap_y + s_minimap_height;
    float min_x = tl_x, max_x = tl_x, min_y = tl_y, max_y = tl_y;
    if (tr_x < min_x) min_x = tr_x; if (tr_x > max_x) max_x = tr_x;
    if (bl_x < min_x) min_x = bl_x; if (bl_x > max_x) max_x = bl_x;
    if (br_x < min_x) min_x = br_x; if (br_x > max_x) max_x = br_x;
    if (tr_y < min_y) min_y = tr_y; if (tr_y > max_y) max_y = tr_y;
    if (bl_y < min_y) min_y = bl_y; if (bl_y > max_y) max_y = bl_y;
    if (br_y < min_y) min_y = br_y; if (br_y > max_y) max_y = br_y;
    if (max_x < mm_l || min_x > mm_r || max_y < mm_t || min_y > mm_b) return 0;
    if (minimap_quad_is_stray(min_x, max_x, min_y, max_y)) return 0; /* [S25] discontinuous spans */

    TD5_SpriteQuad q;
    hud_build_quad_warped(&q, HUD_WHITE_TEX_PAGE,
        tl_x, tl_y, bl_x, bl_y, br_x, br_y, tr_x, tr_y,
        0.0f, 0.0f, 0.0f, 0.0f, 0xFF9A9A9A, HUD_DEPTH);
    hud_submit_quad(&q);
    return 1;
}

/* Emit one minimap PRIMARY road quad spanning [near_idx .. far_idx].
 * The near edge uses the span record's +0x04 front-vertex (col0 delta) and the
 * far edge uses +0x06 (col1 delta) — identical geometry to the inline primary
 * quad built in the P2P road-walk loop below. Factored out so the circuit
 * ring-walk can reuse the exact same per-span quad build. Returns 1 if a quad
 * was submitted, 0 if skipped (bad index / degenerate / fully offscreen).
 * [mirrors the inline build @ td5_hud.c P2P loop; orig 0x0043a220 primary quad]. */
static int minimap_emit_road_quad(uint8_t *span_base, uint8_t *vert_base,
                                  int near_idx, int far_idx,
                                  float offset_x, float offset_z,
                                  float cos_h, float sin_h,
                                  float mm_cx, float mm_cy)
{
    if (!span_base || !vert_base) return 0;
    if (near_idx < 0 || far_idx < 0 ||
        near_idx >= g_strip_span_count || far_idx >= g_strip_span_count) return 0;

    uint8_t *sn = span_base + near_idx * 24;
    uint8_t *sf = span_base + far_idx  * 24;

    int32_t ox_n = *(int32_t *)(sn + 0x0C);
    int32_t oz_n = *(int32_t *)(sn + 0x14);
    int32_t ox_f = *(int32_t *)(sf + 0x0C);
    int32_t oz_f = *(int32_t *)(sf + 0x14);

    /* NEAR span: base vertex from +0x04, column-0 delta for the right edge. */
    uint16_t vi_n_l  = *(uint16_t *)(sn + 0x04);
    uint8_t  type_n  = sn[0];
    uint8_t  nib_n   = sn[3] & 0x0F;
    int32_t  col0    = s_minimap_vtx_delta_col0[type_n & 0x07];
    int32_t  vi_n_r  = (int32_t)vi_n_l + (int32_t)nib_n + col0;

    /* FAR span: base vertex from +0x06, column-1 delta for the right edge. */
    uint16_t vi_f_l  = *(uint16_t *)(sf + 0x06);
    uint8_t  type_f  = sf[0];
    uint8_t  nib_f   = sf[3] & 0x0F;
    int32_t  col1    = s_minimap_vtx_delta_col1[type_f & 0x07];
    int32_t  vi_f_r  = (int32_t)vi_f_l + (int32_t)nib_f + col1;

    if (vi_n_r < 0 || vi_f_r < 0) return 0;

    int16_t *vn_l = (int16_t *)(vert_base + (uint32_t)vi_n_l * 6);
    int16_t *vn_r = (int16_t *)(vert_base + (uint32_t)vi_n_r * 6);
    int16_t *vf_l = (int16_t *)(vert_base + (uint32_t)vi_f_l * 6);
    int16_t *vf_r = (int16_t *)(vert_base + (uint32_t)vi_f_r * 6);

    float wx_fl = (float)((int)vn_l[0] + ox_n) + offset_x;
    float wz_fl = (float)((int)vn_l[2] + oz_n) + offset_z;
    float wx_fr = (float)((int)vn_r[0] + ox_n) + offset_x;
    float wz_fr = (float)((int)vn_r[2] + oz_n) + offset_z;
    float wx_bl = (float)((int)vf_l[0] + ox_f) + offset_x;
    float wz_bl = (float)((int)vf_l[2] + oz_f) + offset_z;
    float wx_br = (float)((int)vf_r[0] + ox_f) + offset_x;
    float wz_br = (float)((int)vf_r[2] + oz_f) + offset_z;

    float fl_x = mm_cx + (wx_fl * cos_h + wz_fl * sin_h) * s_minimap_world_scale_x;
    float fl_y = mm_cy + (wz_fl * cos_h - wx_fl * sin_h) * s_minimap_world_scale_y;
    float fr_x = mm_cx + (wx_fr * cos_h + wz_fr * sin_h) * s_minimap_world_scale_x;
    float fr_y = mm_cy + (wz_fr * cos_h - wx_fr * sin_h) * s_minimap_world_scale_y;
    float bl_x = mm_cx + (wx_bl * cos_h + wz_bl * sin_h) * s_minimap_world_scale_x;
    float bl_y = mm_cy + (wz_bl * cos_h - wx_bl * sin_h) * s_minimap_world_scale_y;
    float br_x = mm_cx + (wx_br * cos_h + wz_br * sin_h) * s_minimap_world_scale_x;
    float br_y = mm_cy + (wz_br * cos_h - wx_br * sin_h) * s_minimap_world_scale_y;

    float mm_l = s_minimap_x, mm_t = s_minimap_y;
    float mm_r = s_minimap_x + s_minimap_width, mm_b = s_minimap_y + s_minimap_height;
    float min_x = fl_x, max_x = fl_x, min_y = fl_y, max_y = fl_y;
    if (fr_x < min_x) min_x = fr_x; if (fr_x > max_x) max_x = fr_x;
    if (bl_x < min_x) min_x = bl_x; if (bl_x > max_x) max_x = bl_x;
    if (br_x < min_x) min_x = br_x; if (br_x > max_x) max_x = br_x;
    if (fr_y < min_y) min_y = fr_y; if (fr_y > max_y) max_y = fr_y;
    if (bl_y < min_y) min_y = bl_y; if (bl_y > max_y) max_y = bl_y;
    if (br_y < min_y) min_y = br_y; if (br_y > max_y) max_y = br_y;
    if (max_x < mm_l || min_x > mm_r || max_y < mm_t || min_y > mm_b) return 0;
    if (minimap_quad_is_stray(min_x, max_x, min_y, max_y)) return 0; /* [S25] discontinuous spans */

    /* TL=front-left, BL=back-left, BR=back-right, TR=front-right */
    TD5_SpriteQuad q;
    hud_build_quad_warped(&q, HUD_WHITE_TEX_PAGE,
        fl_x, fl_y, bl_x, bl_y, br_x, br_y, fr_x, fr_y,
        0.0f, 0.0f, 0.0f, 0.0f, 0xFF9A9A9A, HUD_DEPTH);
    hud_submit_quad(&q);
    return 1;
}

/* Vectorized minimap "grid" (VectorUI). The original tiles a small rose "+"
 * SCANBACK sprite 4x4; this replaces it with a clean radar-style grid: a darker
 * semi-transparent GREEN background panel + continuous lighter-green grid lines
 * (graph-paper). Solid axis-aligned quads -> crisp + resolution-independent.
 * Screen-space (the grid does not rotate; the route rotates within it). Drawn
 * before the route + dots so they read on top. */
static void hud_vector_minimap_grid(float mm_cx, float mm_cy)
{
    float x0 = s_minimap_x, y0 = s_minimap_y;
    float w  = s_minimap_width, h = s_minimap_height;

    /* darker semi-transparent green background panel */
    td5_vui_quad(x0, y0, w, h, 0x620E2614u, -1, 0.0f, 0.0f, 0.0f, 0.0f);

    /* continuous lighter-green grid lines at the original 4-cell spacing
     * (lines run through the former crosshair positions, edge to edge) */
    uint32_t gcol = 0x7034A052u;                 /* lighter green, semi-transparent */
    float lw = w * 0.012f;  if (lw < 1.0f) lw = 1.0f;   /* ~1px, scales with res */
    const float f[4] = { -0.375f, -0.125f, 0.125f, 0.375f };
    for (int i = 0; i < 4; i++) {
        float vx = mm_cx + f[i] * s_minimap_tile_width;   /* vertical line   */
        td5_vui_quad(vx - lw * 0.5f, y0, lw, h, gcol, -1, 0.0f, 0.0f, 0.0f, 0.0f);
        float hy = mm_cy + f[i] * s_minimap_tile_height;  /* horizontal line */
        td5_vui_quad(x0, hy - lw * 0.5f, w, lw, gcol, -1, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

void td5_hud_render_minimap(int actor_slot)
{
    /* Faithful: the original disabled the whole minimap on circuit tracks via an
     * early-return on gTrackIsCircuit @ 0x0043A231 (the dead circuit-aware code
     * inside RenderTrackMinimapOverlay shows the wrap behaviour the devs intended
     * but never shipped). Port enhancement: re-enable it behind the [Game]
     * CircuitMinimap knob (default on). Knob off → restore the faithful "no
     * minimap on circuits" behaviour. The road-walk below branches on `circuit`;
     * everything else (background tiles, racer/traffic dots) is shared and the
     * dot loops already handle the circuit modulo-wrap span window. */
    int circuit = (g_track_is_circuit != 0);
    if (circuit && !g_td5.ini.circuit_minimap) return;

    /* Guard against the span-wrap modulo dividing by zero before the track
     * strip is loaded (g_strip_span_count == 0) -- crashed on early race frames
     * (0xC0000094 integer divide-by-zero in the ring-walk). No spans => no map. */
    if (g_strip_span_count <= 0) return;

    /* Set minimap clip rect */
    td5_render_set_clip_rect(
        s_minimap_x, s_minimap_x + s_minimap_width,
        s_minimap_y, s_minimap_y + s_minimap_height);

    td5_render_set_projection_center(
        s_minimap_width * 0.5f + s_minimap_x,
        s_minimap_height * 0.5f + s_minimap_y);

    /* Minimap screen center — used directly for 2D coord offsets below. */
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

    /* Background "grid" of 16 SCANBACK crosshairs. VectorUI: draw them as crisp
     * procedural bars (resolution-independent); otherwise submit the 16 baked
     * texture tiles (binary: offset 0x4500..0x507F, stride 0xB8) via the
     * low-alpha-ref translucent path (TRANSLUCENT_POINT, alpha_ref=1). */
    if (g_td5.ini.vector_ui) {
        hud_vector_minimap_grid(mm_cx, mm_cy);
    } else {
        for (int i = 0; i < 16; i++) {
            int buf_off = 0x4500 + i * TD5_HUD_GLYPH_QUAD_SIZE;
            td5_render_submit_translucent_low_ref(
                (uint16_t *)(s_minimap_quad_buf + buf_off));
        }
    }

    /* Walk track spans and render road segments using the pre-built segment table.
     * Original @ 0x43A220 iterates the segment table (built in InitMinimapLayout),
     * starting from the segment containing start_span [CONFIRMED @ 0x43A350-0x43A380],
     * rendering up to 48 segments [CONFIRMED @ 0x43B09B: while (local_8c < 0x30)].
     *
     * start_span = ((player_span / 24) - 6) * 24 [CONFIRMED @ 0x43A380]:
     * round down to 24-span group, go back 6 groups (144 spans behind player). */
    int16_t player_span = actor_span_index(actor_slot);

    /* Validate that the span tracker's answer is near the player's actual
     * world position.  The boundary-walk tracker (td5_track.c) can fall
     * behind when the car moves many spans per frame, leaving player_span
     * pointing at a stale region.  If the span origin is too far from the
     * actor, do a brute-force nearest-span search so the minimap stays
     * centred on the car.
     *
     * P2P only: the test compares the actor position to the span ORIGIN
     * (+0x0C/+0x14), which on some circuit tracks (e.g. Maui) sits far from the
     * road centerline, so it misfires every frame — the search returns the same
     * span (a no-op that spams the log + rescans the whole strip, and could even
     * mis-recenter to a coincidentally-close origin). The circuit ring-walk
     * below centres via `player_span % ring`, which tolerates a slightly-stale
     * tracker, so skip this P2P-tuned heuristic on circuits. */
    if (!circuit) {
        int32_t px = actor_world_x(actor_slot) >> 8; /* 24.8 FP → world units */
        int32_t pz = actor_world_z(actor_slot) >> 8;
        uint8_t *sb = (uint8_t *)g_strip_span_base;
        if (sb && player_span >= 0 && player_span < g_strip_span_count) {
            int32_t sox = *(int32_t *)(sb + (int)player_span * 24 + 0x0C);
            int32_t soz = *(int32_t *)(sb + (int)player_span * 24 + 0x14);
            int64_t dx = (int64_t)(px - sox);
            int64_t dz = (int64_t)(pz - soz);
            int64_t dist2 = dx * dx + dz * dz;
            /* If span origin is more than ~4000 world units away, search */
            if (dist2 > (int64_t)4000 * 4000) {
                int best = (int)player_span;
                int64_t best_d = dist2;
                /* Scan every 6th span for speed (same stride as render loop) */
                for (int si = 0; si < g_strip_span_count; si += 6) {
                    int32_t cx = *(int32_t *)(sb + si * 24 + 0x0C);
                    int32_t cz = *(int32_t *)(sb + si * 24 + 0x14);
                    int64_t ddx = (int64_t)(px - cx);
                    int64_t ddz = (int64_t)(pz - cz);
                    int64_t d2 = ddx * ddx + ddz * ddz;
                    if (d2 < best_d) { best_d = d2; best = si; }
                }
                TD5_LOG_W(LOG_TAG, "minimap: span tracker stale %d→%d (dist %.0f)",
                          (int)player_span, best, (float)dx);
                player_span = (int16_t)best;
            }
        }
    }

    /* Branch remap [CONFIRMED @ 0x43A311-0x43A38E]: when the player is on a
     * branch segment (player_span >= ring_length, i.e. past the main road),
     * remap back into the primary track range using the appended rows of the
     * segment table. Original tests DAT_004c3d90 (ring_length = main road
     * count) NOT the total span count — branch spans live past the main road
     * in the strip, so the correct gate is ring_length, not g_strip_span_count
     * (which includes branches). */
    int ring_length = g_td5.track_span_ring_length;
    if (ring_length <= 0) ring_length = g_strip_span_count;
    int remapped_span = (int)player_span;
    if (remapped_span >= ring_length &&
        s_minimap_seg_branch_start < s_minimap_seg_primary_end) {
        for (int si = s_minimap_seg_branch_start;
             si < s_minimap_seg_primary_end; si++) {
            if (remapped_span <= (int)s_minimap_seg_end[si]) {
                remapped_span += (int)s_minimap_seg_branch[si]
                               - (int)s_minimap_seg_start[si];
                break;
            }
        }
    }

    int start_span = (remapped_span / 24 - 6) * 24;
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
     * Per iteration, two span records are read: NEAR at `local_a4` and FAR at
     * `local_a4 + 5` (clamped). The primary quad is built from 4 corners:
     *
     *   vert0 (front-left ) = pool[near[+0x04]]
     *   vert1 (front-right) = vert0 + (near[3]&0xf) + delta_col0[near[0]]
     *   vert2 (back-left  ) = pool[ far[+0x06]]      ← note +0x06, not +0x04
     *   vert3 (back-right ) = vert2 + ( far[3]&0xf) + delta_col1[ far[0]]
     *
     * Origins from each respective span (+0x0c, +0x14) are added before rotation.
     * [CONFIRMED @ 0x0043a220 decomp + memory_read @ 0x00473fd8]
     */
    int local_a4 = start_span;
    int seg_rendered = 0;
    int branch_rendered = 0;
    int skip_bounds = 0, skip_vtx = 0, skip_aabb = 0;

    /* Current primary segment index (local_90 in the original).
     * Scan the primary rows to find the segment that contains start_span.
     * [CONFIRMED @ 0x43A335-0x43A470] — original uses this index throughout
     * the loop to read seg_end (for far_idx clamp) and seg_branch (for the
     * second branch-quad draw). */
    int cur_seg = 0;
    for (int k = 0; k < s_minimap_seg_branch_start; k++) {
        if ((int)s_minimap_seg_end[k] >= local_a4) { cur_seg = k; break; }
        cur_seg = k + 1;
    }
    if (cur_seg >= s_minimap_seg_branch_start) {
        cur_seg = s_minimap_seg_branch_start - 1;
        if (cur_seg < 0) cur_seg = 0;
    }

    /* `!circuit`: the P2P segment-walk is skipped entirely on circuit tracks —
     * the circuit ring-walk below replaces it. The branch-remap / start_span /
     * cur_seg setup above is computed but unused when circuit (harmless). */
    for (int i = 0; !circuit && span_base && vert_base && i < 0x30; i++) {
        /* Pre-iter segment advance [CONFIRMED @ 0x43A426]:
         * while (seg_end[cur_seg] < local_a4) cur_seg++ */
        while (cur_seg + 1 < s_minimap_seg_branch_start &&
               (int)s_minimap_seg_end[cur_seg] < local_a4) {
            cur_seg++;
        }

        int near_idx = local_a4;
        int far_idx  = near_idx + 5;

        /* Clamp far_idx to current segment end [CONFIRMED @ 0x43A426-0x43A44E]:
         * if (seg_end[cur_seg] <= far_idx) uVar15 = seg_end[cur_seg] */
        if (cur_seg < s_minimap_seg_branch_start &&
            (int)s_minimap_seg_end[cur_seg] <= far_idx) {
            far_idx = (int)s_minimap_seg_end[cur_seg];
            if (far_idx < near_idx) far_idx = near_idx;
        }

        local_a4 = far_idx + 1; /* advance by up to 6 */

        if (near_idx < 0 || far_idx < 0 ||
            near_idx >= g_strip_span_count ||
            far_idx  >= g_strip_span_count) { skip_bounds++; continue; }

        uint8_t *sn = span_base + near_idx * 24;
        uint8_t *sf = span_base + far_idx  * 24;

        int32_t ox_n = *(int32_t *)(sn + 0x0C);
        int32_t oz_n = *(int32_t *)(sn + 0x14);
        int32_t ox_f = *(int32_t *)(sf + 0x0C);
        int32_t oz_f = *(int32_t *)(sf + 0x14);

        /* NEAR span: base vertex from +0x04, column-0 delta for the right edge. */
        uint16_t vi_n_l  = *(uint16_t *)(sn + 0x04);
        uint8_t  type_n  = sn[0];
        uint8_t  nib_n   = sn[3] & 0x0F;
        int32_t  col0    = s_minimap_vtx_delta_col0[type_n & 0x07];
        int32_t  vi_n_r  = (int32_t)vi_n_l + (int32_t)nib_n + col0;

        /* FAR span: base vertex from +0x06, column-1 delta for the right edge. */
        uint16_t vi_f_l  = *(uint16_t *)(sf + 0x06);
        uint8_t  type_f  = sf[0];
        uint8_t  nib_f   = sf[3] & 0x0F;
        int32_t  col1    = s_minimap_vtx_delta_col1[type_f & 0x07];
        int32_t  vi_f_r  = (int32_t)vi_f_l + (int32_t)nib_f + col1;

        if (vi_n_r < 0 || vi_f_r < 0) { skip_vtx++; continue; }

        int16_t *vn_l = (int16_t *)(vert_base + (uint32_t)vi_n_l * 6);
        int16_t *vn_r = (int16_t *)(vert_base + (uint32_t)vi_n_r * 6);
        int16_t *vf_l = (int16_t *)(vert_base + (uint32_t)vi_f_l * 6);
        int16_t *vf_r = (int16_t *)(vert_base + (uint32_t)vi_f_r * 6);

        /* Raw world units (only actor coords use kFP). */
        float wx_fl = (float)((int)vn_l[0] + ox_n) + offset_x;
        float wz_fl = (float)((int)vn_l[2] + oz_n) + offset_z;
        float wx_fr = (float)((int)vn_r[0] + ox_n) + offset_x;
        float wz_fr = (float)((int)vn_r[2] + oz_n) + offset_z;
        float wx_bl = (float)((int)vf_l[0] + ox_f) + offset_x;
        float wz_bl = (float)((int)vf_l[2] + oz_f) + offset_z;
        float wx_br = (float)((int)vf_r[0] + ox_f) + offset_x;
        float wz_br = (float)((int)vf_r[2] + oz_f) + offset_z;

        /* Rotate into player-up minimap space */
        float mx_fl = (wx_fl * cos_h + wz_fl * sin_h) * s_minimap_world_scale_x;
        float my_fl = (wz_fl * cos_h - wx_fl * sin_h) * s_minimap_world_scale_y;
        float mx_fr = (wx_fr * cos_h + wz_fr * sin_h) * s_minimap_world_scale_x;
        float my_fr = (wz_fr * cos_h - wx_fr * sin_h) * s_minimap_world_scale_y;
        float mx_bl = (wx_bl * cos_h + wz_bl * sin_h) * s_minimap_world_scale_x;
        float my_bl = (wz_bl * cos_h - wx_bl * sin_h) * s_minimap_world_scale_y;
        float mx_br = (wx_br * cos_h + wz_br * sin_h) * s_minimap_world_scale_x;
        float my_br = (wz_br * cos_h - wx_br * sin_h) * s_minimap_world_scale_y;

        float fl_x = mm_cx + mx_fl, fl_y = mm_cy + my_fl;
        float fr_x = mm_cx + mx_fr, fr_y = mm_cy + my_fr;
        float bl_x = mm_cx + mx_bl, bl_y = mm_cy + my_bl;
        float br_x = mm_cx + mx_br, br_y = mm_cy + my_br;

        /* Loose AABB reject: drop only when the quad is entirely outside
         * the minimap rect. Hardware scissor (set via td5_render_set_clip_rect
         * at the start of this function) clips partial overhangs at the
         * rasterizer stage, so the road extends naturally to the edge
         * instead of popping in/out as quads enter the rect. */
        float min_x = fl_x, max_x = fl_x, min_y = fl_y, max_y = fl_y;
        if (fr_x < min_x) min_x = fr_x; if (fr_x > max_x) max_x = fr_x;
        if (bl_x < min_x) min_x = bl_x; if (bl_x > max_x) max_x = bl_x;
        if (br_x < min_x) min_x = br_x; if (br_x > max_x) max_x = br_x;
        if (fr_y < min_y) min_y = fr_y; if (fr_y > max_y) max_y = fr_y;
        if (bl_y < min_y) min_y = bl_y; if (bl_y > max_y) max_y = bl_y;
        if (br_y < min_y) min_y = br_y; if (br_y > max_y) max_y = br_y;
        int primary_culled = (max_x < s_minimap_x || min_x > mm_r ||
                              max_y < s_minimap_y || min_y > mm_b);
        /* [FIX 2026-06-05 Rome end-of-race stray minimap line] Also drop a quad
         * whose extent dwarfs the minimap — its NEAR/FAR spans are discontinuous
         * (the strip continues past the P2P finish into a disconnected tail). See
         * minimap_quad_is_stray(). This is the quad that drew the stray line at
         * the Rome finish; its AABB overlaps the rect so the cull above misses it. */
        if (!primary_culled && minimap_quad_is_stray(min_x, max_x, min_y, max_y))
            primary_culled = 1;
        if (primary_culled) {
            /* Log first culled quad per frame for diagnostics */
            if (skip_aabb == 0 && seg_rendered == 0) {
                static int s_aabb_log_ctr = 0;
                if ((s_aabb_log_ctr++ % 60) == 0) {
                    TD5_LOG_W("track", "minimap AABB cull i=%d near=%d: fl=(%.0f,%.0f) fr=(%.0f,%.0f) bl=(%.0f,%.0f) br=(%.0f,%.0f) rect=(%.0f,%.0f)-(%.0f,%.0f) off=(%.0f,%.0f) ox_n=%d oz_n=%d",
                              i, near_idx,
                              fl_x, fl_y, fr_x, fr_y, bl_x, bl_y, br_x, br_y,
                              s_minimap_x, s_minimap_y, mm_r, mm_b,
                              offset_x, offset_z, ox_n, oz_n);
                }
            }
            skip_aabb++;
        } else {
            /* TL=front-left, BL=back-left, BR=back-right, TR=front-right */
            hud_build_quad_warped(
                &map_quad, HUD_WHITE_TEX_PAGE,
                fl_x, fl_y,  /* TL */
                bl_x, bl_y,  /* BL */
                br_x, br_y,  /* BR */
                fr_x, fr_y,  /* TR */
                0.0f, 0.0f, 0.0f, 0.0f,
                0xFF9A9A9A,
                HUD_DEPTH
            );
            hud_submit_quad(&map_quad);
            seg_rendered++;
        }

        /* Branch-quad draw [Ghidra @ 0x43A6E8-0x43A6FC, derived].
         *   if (seg_branch[cur_seg] != -1) {
         *       delta  = seg_branch[cur_seg] - seg_start[cur_seg];
         *       b_near = local_a4 + delta;
         *       b_far  = uVar15  + delta;
         *       <build second quad from spans [b_near] and [b_far]>
         *       <submit>
         *   }
         *
         * Delta is the strip-index offset from primary span to the parallel
         * branch span: if primary seg K is at [start_K, end_K] and its branch
         * link is br, the branch spans live at [br, br + (end_K - start_K)].
         * Walking primary at local_a4 = start_K + k, the parallel branch
         * span is br + k = local_a4 + (br - start_K).
         *
         * Restricted to primary rows (cur_seg < branch_start) so appended
         * back-link rows (seg_branch[new] = seg_start[K] per init) don't
         * double-fire. */
        if (cur_seg < s_minimap_seg_branch_start &&
            s_minimap_seg_branch[cur_seg] != (int16_t)-1) {
            int delta = (int)s_minimap_seg_branch[cur_seg]
                      - (int)s_minimap_seg_start[cur_seg];
            int b_near = near_idx + delta;
            int b_far  = far_idx  + delta;

            /* No b_far clamp — byte-faithful to orig @ 0x43a711 (b_far =
             * far + delta, raw). The branch's last quad INTENTIONALLY reaches
             * the JOIN span (seg_branch[K] + plen): its +0x06 vertex encodes
             * the reconvergence geometry where the branch rejoins the mainline,
             * so drawing it CLOSES the branch line at the rejoin. A prior port
             * guard clamped b_far to br+plen-1 to "stay inside the branch
             * range", which dropped that join quad and produced the taper/
             * cutoff at the branch end (user-reported). The earlier init
             * off-by-one fix corrected the segment boundaries the old guard was
             * compensating for. The b_near<=b_far and <g_strip_span_count
             * guards below keep the raw far in-bounds.
             * [CONFIRMED: orig has no clamp @ 0x43a711]. */
            if (b_near <= b_far &&
                b_near >= 0 && b_far >= 0 &&
                b_near < g_strip_span_count &&
                b_far  < g_strip_span_count) {
                uint8_t *bsn = span_base + b_near * 24;
                uint8_t *bsf = span_base + b_far  * 24;

                int32_t box_n = *(int32_t *)(bsn + 0x0C);
                int32_t boz_n = *(int32_t *)(bsn + 0x14);
                int32_t box_f = *(int32_t *)(bsf + 0x0C);
                int32_t boz_f = *(int32_t *)(bsf + 0x14);

                uint16_t bvi_n_l = *(uint16_t *)(bsn + 0x04);
                uint8_t  btype_n = bsn[0];
                uint8_t  bnib_n  = bsn[3] & 0x0F;
                int32_t  bcol0   = s_minimap_vtx_delta_col0[btype_n & 0x07];
                int32_t  bvi_n_r = (int32_t)bvi_n_l + (int32_t)bnib_n + bcol0;

                uint16_t bvi_f_l = *(uint16_t *)(bsf + 0x06);
                uint8_t  btype_f = bsf[0];
                uint8_t  bnib_f  = bsf[3] & 0x0F;
                int32_t  bcol1   = s_minimap_vtx_delta_col1[btype_f & 0x07];
                int32_t  bvi_f_r = (int32_t)bvi_f_l + (int32_t)bnib_f + bcol1;

                if (bvi_n_r >= 0 && bvi_f_r >= 0) {
                    int16_t *bvn_l = (int16_t *)(vert_base + (uint32_t)bvi_n_l * 6);
                    int16_t *bvn_r = (int16_t *)(vert_base + (uint32_t)bvi_n_r * 6);
                    int16_t *bvf_l = (int16_t *)(vert_base + (uint32_t)bvi_f_l * 6);
                    int16_t *bvf_r = (int16_t *)(vert_base + (uint32_t)bvi_f_r * 6);

                    float bwx_fl = (float)((int)bvn_l[0] + box_n) + offset_x;
                    float bwz_fl = (float)((int)bvn_l[2] + boz_n) + offset_z;
                    float bwx_fr = (float)((int)bvn_r[0] + box_n) + offset_x;
                    float bwz_fr = (float)((int)bvn_r[2] + boz_n) + offset_z;
                    float bwx_bl = (float)((int)bvf_l[0] + box_f) + offset_x;
                    float bwz_bl = (float)((int)bvf_l[2] + boz_f) + offset_z;
                    float bwx_br = (float)((int)bvf_r[0] + box_f) + offset_x;
                    float bwz_br = (float)((int)bvf_r[2] + boz_f) + offset_z;

                    float bmx_fl = (bwx_fl * cos_h + bwz_fl * sin_h) * s_minimap_world_scale_x;
                    float bmy_fl = (bwz_fl * cos_h - bwx_fl * sin_h) * s_minimap_world_scale_y;
                    float bmx_fr = (bwx_fr * cos_h + bwz_fr * sin_h) * s_minimap_world_scale_x;
                    float bmy_fr = (bwz_fr * cos_h - bwx_fr * sin_h) * s_minimap_world_scale_y;
                    float bmx_bl = (bwx_bl * cos_h + bwz_bl * sin_h) * s_minimap_world_scale_x;
                    float bmy_bl = (bwz_bl * cos_h - bwx_bl * sin_h) * s_minimap_world_scale_y;
                    float bmx_br = (bwx_br * cos_h + bwz_br * sin_h) * s_minimap_world_scale_x;
                    float bmy_br = (bwz_br * cos_h - bwx_br * sin_h) * s_minimap_world_scale_y;

                    float bfl_x = mm_cx + bmx_fl, bfl_y = mm_cy + bmy_fl;
                    float bfr_x = mm_cx + bmx_fr, bfr_y = mm_cy + bmy_fr;
                    float bbl_x = mm_cx + bmx_bl, bbl_y = mm_cy + bmy_bl;
                    float bbr_x = mm_cx + bmx_br, bbr_y = mm_cy + bmy_br;

                    float b_min_x = bfl_x, b_max_x = bfl_x;
                    float b_min_y = bfl_y, b_max_y = bfl_y;
                    if (bfr_x < b_min_x) b_min_x = bfr_x; if (bfr_x > b_max_x) b_max_x = bfr_x;
                    if (bbl_x < b_min_x) b_min_x = bbl_x; if (bbl_x > b_max_x) b_max_x = bbl_x;
                    if (bbr_x < b_min_x) b_min_x = bbr_x; if (bbr_x > b_max_x) b_max_x = bbr_x;
                    if (bfr_y < b_min_y) b_min_y = bfr_y; if (bfr_y > b_max_y) b_max_y = bfr_y;
                    if (bbl_y < b_min_y) b_min_y = bbl_y; if (bbl_y > b_max_y) b_max_y = bbl_y;
                    if (bbr_y < b_min_y) b_min_y = bbr_y; if (bbr_y > b_max_y) b_max_y = bbr_y;

                    if (!(b_max_x < s_minimap_x || b_min_x > mm_r ||
                          b_max_y < s_minimap_y || b_min_y > mm_b) &&
                        !minimap_quad_is_stray(b_min_x, b_max_x, b_min_y, b_max_y)) {
                        hud_build_quad_warped(
                            &map_quad, HUD_WHITE_TEX_PAGE,
                            bfl_x, bfl_y,
                            bbl_x, bbl_y,
                            bbr_x, bbr_y,
                            bfr_x, bfr_y,
                            0.0f, 0.0f, 0.0f, 0.0f,
                            0xFF9A9A9A,
                            HUD_DEPTH
                        );
                        hud_submit_quad(&map_quad);
                        if (branch_rendered == 0) {
                            static int s_first_branch_ctr = 0;
                            if ((s_first_branch_ctr++ % 60) == 0) {
                                TD5_LOG_I(LOG_TAG,
                                    "minimap branch: cur_seg=%d start=%d end=%d link=%d delta=%d near=%d->b_near=%d far=%d->b_far=%d",
                                    cur_seg,
                                    (int)s_minimap_seg_start[cur_seg],
                                    (int)s_minimap_seg_end[cur_seg],
                                    (int)s_minimap_seg_branch[cur_seg],
                                    delta, near_idx, b_near, far_idx, b_far);
                            }
                        }
                        branch_rendered++;
                    }
                }
            }
        }

        /* Quad3/Quad4: checkpoint-connector road quads [CONFIRMED @ 0x43a9a9-0x43aeb4].
         * The original draws an extra 2-span connector quad at the checkpoint span
         * `cp` that falls within this quad's [near, far] range, plus its branch-
         * collapsed counterpart when the current segment has a branch link. These
         * bridge the route line through checkpoint seams. Checkpoint spans come
         * from s_active_checkpoint (mirror of g_raceCheckpointTablePtr @ 0x4aed88).
         * Use near_idx (the pre-advance value) for the range test — local_a4 has
         * already been advanced to far_idx+1 above. */
        {
            int cp = 9999; /* orig default 0x270f @ 0x43a3c4 */
            int cp_count = td5_game_get_minimap_checkpoint_count();
            for (int ci = 0; ci < cp_count; ci++) {
                int cs = td5_game_get_minimap_checkpoint_span(ci);
                if (cs >= near_idx && cs < near_idx + 0x120) { cp = cs; break; }
            }
            if (cp <= far_idx) { /* Quad3 gate @ 0x43a9a9 */
                if (minimap_emit_connector(span_base, vert_base, cp,
                                           offset_x, offset_z, cos_h, sin_h,
                                           mm_cx, mm_cy))
                    seg_rendered++;
                /* Quad4: branch-collapsed connector, gated on LINK != -1
                 * (g_minimapSegmentSpanEnd[seg], @ 0x43ac23). cpB = cp + delta
                 * where delta = LINK - segStart (same delta as the branch quad). */
                if (cur_seg < s_minimap_seg_branch_start &&
                    s_minimap_seg_branch[cur_seg] != (int16_t)-1) {
                    int cpB = cp + ((int)s_minimap_seg_branch[cur_seg]
                                  - (int)s_minimap_seg_start[cur_seg]);
                    if (minimap_emit_connector(span_base, vert_base, cpB,
                                               offset_x, offset_z, cos_h, sin_h,
                                               mm_cx, mm_cy))
                        branch_rendered++;
                }
            }
        }
    }
    {
        static int s_mm_log_counter = 0;
        if ((s_mm_log_counter++ % 60) == 0) {
            TD5_LOG_I("track", "minimap: span=%d remap=%d start=%d last_a4=%d cur_seg=%d(start=%d end=%d link=%d) primary=%d branch=%d skip_bounds=%d skip_vtx=%d skip_aabb=%d primaries=%d branches=%d total=%d",
                      (int)player_span, remapped_span, start_span, local_a4,
                      cur_seg,
                      (cur_seg < s_minimap_seg_branch_start ? (int)s_minimap_seg_start[cur_seg] : -1),
                      (cur_seg < s_minimap_seg_branch_start ? (int)s_minimap_seg_end[cur_seg] : -1),
                      (cur_seg < s_minimap_seg_branch_start ? (int)s_minimap_seg_branch[cur_seg] : -1),
                      seg_rendered, branch_rendered,
                      skip_bounds, skip_vtx, skip_aabb,
                      (int)s_minimap_seg_branch_start,
                      (int)s_minimap_seg_primary_end - (int)s_minimap_seg_branch_start,
                      g_strip_span_count);
        }
    }

    /* --- Circuit road walk (port enhancement; orig early-returned) ---
     * Re-enables the minimap road geometry for circuit tracks. The MAIN LOOP is
     * drawn by a player-centred ring walk modelled on the main track render's
     * circuit entry walk (td5_render.c circuit branch; td5_track_get_ring_length)
     * — "the same logic as the tracks, in the minimap's format". Same 48-quad /
     * ±144-span window and the same per-span quad geometry as the P2P path
     * (minimap_emit_road_quad), but the span index wraps modulo the ring so the
     * road stays continuous across the start/finish seam — fixing the limitation
     * the original avoided by simply not drawing the minimap on circuits (whose
     * dead code reset the index to 0 at the seam instead of true-wrapping).
     * BRANCH / FORK roads are then drawn from the appended branch-mirror rows of
     * the segment table (built by the init for every track; branch spans live in
     * [ring, total_spans)), so forks show up the same as on P2P tracks. */
    if (circuit) {
        int ring = g_td5.track_span_ring_length;
        if (ring <= 0) ring = g_strip_span_count;
        int circuit_rendered = 0;
        int circuit_branches  = 0;
        if (ring > 0 && span_base && vert_base) {
            int center = (int)player_span % ring;
            if (center < 0) center += ring;
            /* start_span = ((player_span/24) - 6) * 24 — 144 spans behind the
             * player, same formula as the P2P path [orig @ 0x43A372-0x43A3B7]. */
            int start = (center / 24 - 6) * 24;
            for (int i = 0; i < 0x30; i++) {            /* 48 quads (orig while < 0x30) */
                int near_raw = start + i * 6;           /* advance ~6 spans / quad */
                if (near_raw - start >= ring) break;    /* covered a full lap; stop */
                int far_raw  = near_raw + 5;
                /* True modulo wrap into [0, ring). The ring closes (span ring-1
                 * ≈ span 0) so a quad bridging the seam is geometrically valid. */
                int near_idx = ((near_raw % ring) + ring) % ring;
                int far_idx  = ((far_raw  % ring) + ring) % ring;
                if (minimap_emit_road_quad(span_base, vert_base, near_idx, far_idx,
                                           offset_x, offset_z, cos_h, sin_h,
                                           mm_cx, mm_cy))
                    circuit_rendered++;
            }

            /* Branch / fork roads. The init appends one segment row per branch
             * link [s_minimap_seg_branch_start .. s_minimap_seg_primary_end):
             * seg_start = branch's first span, seg_end = branch's last span (both
             * in the [ring, total_spans) branch region). Walk each range and draw
             * it at its OWN world position (each branch span carries its origin +
             * vertices). The window AABB cull inside minimap_emit_road_quad keeps
             * only the branch quads near the player, matching the windowed view.
             * No overlap with the ring walk above (which stays in [0, ring)). */
            for (int bi = s_minimap_seg_branch_start;
                 bi < s_minimap_seg_primary_end; bi++) {
                int bstart = (int)s_minimap_seg_start[bi];
                int bend   = (int)s_minimap_seg_end[bi];
                if (bend <= bstart) continue;
                for (int bs = bstart; bs <= bend; bs += 6) {
                    int bf = bs + 5;
                    if (bf > bend) bf = bend;
                    if (minimap_emit_road_quad(span_base, vert_base, bs, bf,
                                               offset_x, offset_z, cos_h, sin_h,
                                               mm_cx, mm_cy)) {
                        circuit_rendered++;
                        circuit_branches++;
                    }
                }
            }
        }
        {
            static int s_mm_circuit_log = 0;
            if ((s_mm_circuit_log++ % 60) == 0) {
                TD5_LOG_I(LOG_TAG, "minimap circuit: span=%d ring=%d rendered=%d (branch=%d) branch_rows=%d span_count=%d",
                          (int)player_span, ring, circuit_rendered, circuit_branches,
                          (int)s_minimap_seg_primary_end - (int)s_minimap_seg_branch_start,
                          g_strip_span_count);
            }
        }
    }

    /* Render racer dot markers.
     *
     * Skip slots in state==3 (decoration) so drag-race decoration slots
     * 2-5 (parked at span=1 by the spawn override) don't show up as
     * minimap dots at the back wall. Without this gate the player saw
     * "two cars at the back of the strip" — actually 4 minimap dots for
     * slots 2-5 piled on top of each other since their lanes (0-3) are
     * within minimap dot size. [CONFIRMED via render diagnostic 2026-04-28]
     *
     * The original [@ 0x00432EAE InitializeRaceActorRuntime] sets
     * g_racerCount=2 for game_type!=0 which prunes the loop bound to 2
     * for drag/cup/etc. The port keeps g_racer_count=6 for cup/wanted
     * (they need to draw all opponents), so per-slot state filtering
     * is the correct port-side equivalent. */
    for (int r = 0; r < g_racer_count; r++) {
        if (r < TD5_MAX_RACER_SLOTS && td5_game_get_slot_state(r) == 3)
            continue;
        int16_t racer_span = actor_span_index(r);
        int span_delta;
        if (g_track_is_circuit) {
            span_delta = ((g_strip_span_count / 2 - (int)racer_span) + (int)player_span)
                         % g_strip_span_count - g_strip_span_count / 2;
        } else {
            /* Branch-aware gate: compare branch-collapsed (normalized) spans,
             * matching the original which reads actor+0x82 for both player and
             * racer [CONFIRMED @ 0x43aefa/0x43af2c]. remapped_span is the
             * player's normalized span (already computed for centering);
             * minimap_normalize_span() collapses the racer's raw span the same
             * way. Without this, an opponent on a different branch has a
             * raw-span delta far beyond +/-144 and is culled (the reported
             * "can't see opponents on the other branch"). */
            int racer_norm = minimap_normalize_span((int)racer_span);
            span_delta = remapped_span - racer_norm;

            /* One-shot-per-second diagnostic when a racer is on a different
             * branch than the player (raw span differs from normalized). */
            if (r != actor_slot && (int)racer_span != racer_norm) {
                static int s_xbranch_log_ctr = 0;
                if ((s_xbranch_log_ctr++ % 60) == 0) {
                    TD5_LOG_I(LOG_TAG,
                        "minimap xbranch: r=%d raw=%d norm=%d player_norm=%d span_delta=%d shown=%d",
                        r, (int)racer_span, racer_norm, remapped_span, span_delta,
                        (span_delta > -0x91 && span_delta < 0x91));
                }
            }
        }

        /* Only show racers within +/-144 spans */
        if (span_delta > -0x91 && span_delta < 0x91) {
            float rwx = (float)actor_world_x(r) * kFP + offset_x;
            float rwz = (float)actor_world_z(r) * kFP + offset_z;

            float dmx = (rwx * cos_h + rwz * sin_h) * s_minimap_world_scale_x;
            float dmy = (rwz * cos_h - rwx * sin_h) * s_minimap_world_scale_y;

            float half_dot = s_minimap_dot_size * 0.5f;

            /* Hardware scissor clips dots that fall outside the minimap rect
             * at the rasterizer stage. Loose AABB reject skips dots fully
             * offscreen so we don't spend CPU building their quads. */
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
            if (r == actor_slot) {
                /* [PORT: N-way] THIS pane's own car is red — actor_slot is the
                 * view's player (was hardcoded to g_actor_slot_map[0], so panes
                 * 1..N drew their own car blue). */
                dot_u0 = s_minimap_dot_atlas_u;          /* col 0: player (red) */
            } else {
                /* [PORT: N-way] every OTHER racer is blue. This loop only draws
                 * racers (r < g_racer_count), so the legacy `r < 6 ? blue : teal`
                 * split wrongly painted racer slots 6..15 teal in a big field.
                 * Teal (col 2) is reserved for the traffic-dot loop below. */
                dot_u0 = s_minimap_dot_atlas_u + 8.5f;  /* col 1: other racer (blue) [@ 0x45D724] */
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

    /* Traffic actor dots [orig RenderTrackMinimapOverlay @ 0x0043A220 second
     * loop]. Orig walks the traffic pool (DAT_004ab304, stride 0x388) for
     * count DAT_004aaf00, drawing teal dots (col 2 = atlas_u + 16.5) using
     * the same world-to-minimap transform and ±144-span window as racers.
     * Port previously omitted this loop entirely, so traffic vehicles were
     * absent from the minimap. Traffic actors share the unified actor pool
     * at slots TD5_MAX_RACER_SLOTS..total-1; mesh==NULL is the "slot
     * inactive" indicator (matches the render path at td5_render.c:2185). */
    int total_actors = td5_game_get_total_actor_count();
    for (int t = g_traffic_slot_base; t < total_actors && t < TD5_MAX_TOTAL_ACTORS; t++) {
        if (!td5_render_get_vehicle_mesh(t))
            continue;
        int16_t traffic_span = actor_span_index(t);
        int span_delta;
        if (g_track_is_circuit) {
            span_delta = ((g_strip_span_count / 2 - (int)traffic_span) + (int)player_span)
                         % g_strip_span_count - g_strip_span_count / 2;
        } else {
            /* Same branch-collapsed gate as the racer loop above so traffic on
             * a different branch than the player isn't culled by the raw-span
             * delta. [INFERRED — the original's traffic loop shares the racer
             * loop's +/-144 window; the +0x82 normalize matches the racer path
             * at 0x43af2c.] Main-road traffic (raw==norm) is unaffected. */
            span_delta = remapped_span - minimap_normalize_span((int)traffic_span);
        }
        if (span_delta > -0x91 && span_delta < 0x91) {
            float twx = (float)actor_world_x(t) * kFP + offset_x;
            float twz = (float)actor_world_z(t) * kFP + offset_z;
            float dmx = (twx * cos_h + twz * sin_h) * s_minimap_world_scale_x;
            float dmy = (twz * cos_h - twx * sin_h) * s_minimap_world_scale_y;
            float half_dot = s_minimap_dot_size * 0.5f;
            float dot_x = mm_cx + dmx;
            float dot_y = mm_cy + dmy;
            if (dot_x + half_dot < s_minimap_x ||
                dot_x - half_dot > s_minimap_x + s_minimap_width ||
                dot_y + half_dot < s_minimap_y ||
                dot_y - half_dot > s_minimap_y + s_minimap_height) {
                continue;
            }
            int dot_tex = s_minimap_scandots_tex_page ? s_minimap_scandots_tex_page : HUD_WHITE_TEX_PAGE;
            float dot_u0 = s_minimap_dot_atlas_u + 16.5f;  /* col 2: teal — orig "other" colour */
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

    /* (per-frame minimap summary suppressed — see minimap_render log for changes) */
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
            /* Grid tile diffuse: 0x40FFFFFF gives ~25% vertex alpha which,
             * combined with the scanback texture's 0x80 alpha (slot-4 loader),
             * produces a final fragment alpha of 0x20 (~12%) — well below
             * TRANSLUCENT_LINEAR's 0x80 alpha_ref but fine for the
             * TRANSLUCENT_POINT preset (alpha_ref=1) the grid render path
             * uses. Result: ~12% screen-space blend, visible but see-through.
             * Original literal @ 0x0043b250 was 0xFFFFFFFF, but the td5re
             * pipeline clips that to 0x80 via the texture alpha — tuning
             * vertex alpha down here is the cleanest way to reach the
             * subjective "minimap should be more transparent" target. */
            hud_build_quad(
                s_minimap_quad_buf + off,
                0, scanback->texture_page,
                tx0, ty0,
                tx0 + tile_w, ty0 + tile_h,
                bg_u0, bg_v0, bg_u1, bg_v1,
                0x40FFFFFF, HUD_DEPTH4
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
            /* Leave room for the final-segment write + the appended branch rows
             * (defensive; only triggers on tracks with > ~MINIMAP_SEG_MAX/2
             * junctions, far beyond any current TD6 track). */
            if (seg_count >= MINIMAP_SEG_MAX / 2 - 1) break;
            /* Test the CURRENT span's type. Original tests span N at counter N:
             * g_trackStripRecords[N*0x18] [CONFIRMED @ 0x43b6c1]. The prior
             * (s+1)*24 indexing made every segment END land one span short
             * (and read one span PAST the array on the final iteration),
             * truncating the minimap road/branch line at each segment end. */
            uint8_t span_type = span_base[s * 24];

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
                /* Branch LINK = field +8 of the segment's OPENING span (span
                 * at the prior 0x08), i.e. seg_start_val-1. Original reads
                 * *(short*)(records + local_80 - 0x10) with local_80 =
                 * seg_start_val*0x18  =>  span (seg_start_val-1) record +8
                 * [CONFIRMED @ 0x43b6f4]. Guard the first-segment underflow
                 * (no 0x08 precedes the first 0x0B on shipped tracks). */
                int16_t link = (seg_start_val > 0)
                    ? *(int16_t *)(span_base + (seg_start_val - 1) * 24 + 8)
                    : (int16_t)-1;
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

    /* Append branch-mirror rows [CONFIRMED @ 0x43B770-0x43B798 in
     * InitializeMinimapLayout]. For each primary row K with seg_branch[K] != -1:
     *   seg_start[new]  = br                   = seg_branch[K]
     *   seg_end[new]    = br + primary_length  = br + (seg_end[K] - seg_start[K])
     *   seg_branch[new] = seg_start[K]         (back-link to parent primary)
     * The primary row K's seg_end is NOT overwritten. */
    int branch_write = s_minimap_seg_primary_end;
    for (int i = 0; i < s_minimap_seg_primary_end; i++) {
        if (branch_write >= MINIMAP_SEG_MAX) break;   /* never overflow the table */
        if (s_minimap_seg_branch[i] != (int16_t)-1) {
            int16_t br    = s_minimap_seg_branch[i];
            int16_t plen  = s_minimap_seg_end[i] - s_minimap_seg_start[i];
            s_minimap_seg_start[branch_write]  = br;
            s_minimap_seg_end[branch_write]    = br + plen;
            s_minimap_seg_branch[branch_write] = s_minimap_seg_start[i];
            branch_write++;
        }
    }
    s_minimap_seg_primary_end = branch_write;

    TD5_LOG_I(LOG_TAG, "minimap_init: seg_table primaries=%d branches_appended=%d total_spans=%d",
              (int)s_minimap_seg_branch_start,
              (int)(s_minimap_seg_primary_end - s_minimap_seg_branch_start),
              g_strip_span_count);
}

/* ========================================================================
 * InitializePauseMenuOverlayLayout (0x43B7C0)
 *
 * Builds the pause menu overlay: background panel, selection highlight,
 * slider bars, option row backgrounds, and text glyphs.
 *
 * [CONFIRMED @ 0x0043B7C0] — Ghidra-verified: orig clamps param_1<8 (port:
 * page_index<TD5_HUD_PAUSE_MAX_PAGES). String table at &PTR_s_PAUSED_004744B8
 * + param_1*0xC (port: g_pause_page_strings[page_index]). Half-width
 * computation: orig reads (&g_pauseOverlayLanguageWidthTable)[param_1] then
 * multiplies by 0.5f (port: g_pause_page_sizes[page_index] * 0.5f).
 *
 * BLACKBOX panel: orig uses x=[-half_w..+half_w], y=[-56..56] (orig
 * local_84=-half_w, local_78=+half_w, local_68=-56.0, local_70=56.0). Port
 * lines 3209-3213 match: PAUSE_ADD(-s_pause_half_width, -56.0f,
 * s_pause_half_width, 56.0f, ...). UV taps single-texel via blackbox_e
 * atlas_x/y + 0.5f.
 *
 * SELBOX highlight: orig x=[1-half_w..half_w-1], y=[-1..15] initial then
 * dynamic per cursor row (orig DAT_0045d75c row stride =16, base row=3=
 * CONTINUE for default cursor placement). Port lines 3221-3236 match
 * sel_x0/sel_x1 (1-half_w, half_w-1), selbox_base_y=-33 + 3*16 = init_y.
 *
 * BLACKBAR/SLIDER 3-row loop: orig walks 3 rows with row_y = row*16, dark
 * trough x=[half_w-131..half_w-1] y=[row_y-29..row_y-21], slider fill bar
 * x=[half_w-130..(dynamic)] y=[row_y-28..row_y-22]. Port lines 3244-3263
 * match (bar_x0/bar_x1 differ by 1 for the slider extent — same orig).
 *
 * PAUSETXT glyph emission: orig walks string_offset 0..0x2F step 8 (12
 * strings × 2 ints), reads alignment word at +4, dispatches alignment==2
 * → centered (compute total_w from per-char widths via PTR_DAT_004660C8
 * lookup table, glyph_w = char_w*2/3, +2 inter-glyph gap), else
 * left-aligned at x = 4 - half_w. Per glyph: UV at (ch&0xF)*16+0.5,
 * (ch>>4)*16+0.5, glyph_w-1 width, 16px height. Port lines 3271-3306
 * match exactly (g_pause_glyph_widths[ch]*2/3, +2 spacing,
 * (ch & 0x0F)*16, (ch >> 4)*16).
 *
 * [ARCH-DIVERGENCE: BuildSpriteQuadTemplate (centered-coord D3D3 scratch
 *  ring) → hud_build_quad (pixel-space D3D11 buffer); cx/cy screen-center
 *  offset applied explicitly in port] — orig emits coords centered around
 *  screen origin (consumed by BuildSpriteQuadTemplate which adds screen
 *  center internally); port adds g_render_width_f*0.5 / g_render_height_f
 *  *0.5 to every PAUSE_ADD coordinate inline. Same observable geometry.
 *  Documented at port lines 3192-3193. L5 promotion sweep 2026-05-21.
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

    /* Remember what we baked against so the per-frame update path can re-bake if
     * the render dimensions change (otherwise the panel drifts off the live-
     * centred text — see the s_pause_baked_* declaration). */
    s_pause_page_index = page_index;
    s_pause_baked_w    = g_render_width_f;
    s_pause_baked_h    = g_render_height_f;

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

    /* BLACKBOX: dark semi-transparent panel. Original y was fixed ±56 (5 rows
     * below the PAUSED title). [PORT REWORK 2026-06-05 / S15] The reworked menu
     * has 6 rows (the bottom EXIT GAME row + its selbox reach ~+63), so the
     * panel bottom is grown to +68 to frame the extra row. Top stays -56 (the
     * title at -52 keeps a 4px margin). From binary 0x43B7C0: single-texel
     * sample. Texture alpha (A=128 after ARGB channel remap) provides
     * semi-transparency naturally. */
    {
        float bu = (float)blackbox_e->atlas_x + 0.5f;
        float bv = (float)blackbox_e->atlas_y + 0.5f;
        PAUSE_ADD(-s_pause_half_width, -56.0f,
                   s_pause_half_width,  68.0f,
                   bu, bv, bu, bv,
                   blackbox_e->texture_page, 0xFFFFFFFF);
    }

    /* SELBOX: grayscale highlight bar (256x16 atlas texture).
     * From binary: x0=1-half_w, x1=half_w-1. [PORT REWORK 2026-06-05 / S15]
     * Default cursor is now CONTINUE = row 2 (was row 3, before MUSIC removal).
     * This initial position is overwritten every frame by
     * td5_hud_update_pause_overlay(cursor,...). */
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
        float init_y = s_pause_selbox_base_y + 2.0f * 16.0f;  /* default = CONTINUE (row 2) */
        PAUSE_ADD(sel_x0, init_y, sel_x1, init_y + 16.0f,
                  su0, sv0, su1, sv1,
                  selbox_e->texture_page, 0xFFFFFFFF);
    }

    /* BLACKBAR (trough) + SLIDER (fill bar) using atlas textures.
     * From binary: bar x=[half_w-131, half_w-1], row N y=[N*16-29, N*16-21]. */
    s_pause_slider_atlas = slider_e;
    s_pause_bar_x0 = s_pause_half_width - 130.0f;  /* 0x0045D73C */
    s_pause_bar_x1 = s_pause_half_width - 1.0f;

    /* [PORT REWORK 2026-06-05 / S15] Two sliders now: VIEW (row 0) + SOUND
     * (row 1). The MUSIC slider (old row 1) was removed. Clear the stale 3rd
     * pointer so a previous init's quad can't draw a phantom slider. */
    s_pause_slider_ptrs[2] = NULL;
    for (int row = 0; row < 2; row++) {
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

    /* VectorUI: record lines for crisp SDF rendering at draw time instead of
     * baking the bitmap PAUSETXT glyph quads. */
    int pause_vec = (g_td5.ini.vector_ui && td5_vui_pausefont_page() >= 0);
    s_pause_vui_line_count = 0;

    /* [REWORK 2026-06-05 / S15] The offset cap was 0x30 (6 string entries × 8B),
     * sized for the original PAUSED+5-row table. The reworked menu has 7 entries
     * (PAUSED + 6 rows), so EXIT GAME (entry 7, offset 0x30) was dropped. Raised
     * to 0x80 (16 entries, matching the s_pause_vui_lines[16] guard); the NULL
     * terminator (`if (str == NULL) break;`) is the real stop. */
    while (pausetxt && s_pause_menu_strings && string_offset < 0x80) {
        const char *str = s_pause_menu_strings[string_offset / 4];
        if (str == NULL) break;

        int alignment = *(int *)((uint8_t *)s_pause_menu_strings + string_offset + 4);
        int len = (int)strlen(str);

        if (pause_vec) {
            if (s_pause_vui_line_count < 16) {
                PauseTextLine *L = &s_pause_vui_lines[s_pause_vui_line_count++];
                L->y = text_y;  L->alignment = alignment;
                strncpy(L->s, str, sizeof(L->s) - 1);
                L->s[sizeof(L->s) - 1] = '\0';
            }
            text_y += 16.0f;
            string_offset += 8;
            if (string_offset > 0x7F) break;  /* [S15] was 0x2F (6 entries); see while-cap note above */
            continue;
        }

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
        if (string_offset > 0x7F) break;  /* [S15] was 0x2F (6 entries); see while-cap note above */
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
 *
 * [ARCH-DIVERGENCE: 0x0043E750 SetClipBounds is the orig projection
 *  clip-rect setter, NOT a fade renderer; L5 promotion sweep 2026-05-21]
 *  — Ghidra-verified: orig 0x0043E750 takes 4 floats (param_1..param_4),
 *  pairwise-mins them (`if (param_2 < param_1) param_1 = param_2;` etc.),
 *  and stores to globals at DAT_004afb38/0x3c/0x40/0x44 (the projection
 *  clip rect used by later vertex transforms). This is the equivalent of
 *  td5_render_set_clip_rect (td5_render.c:5570) which casts to int and
 *  pushes to td5_plat_render_set_clip_rect (D3D11 scissor). The port's
 *  td5_hud_draw_race_fade is a separate function unrelated to 0x0043E750
 *  — it implements the directional-wipe FADEWHT bar render that the orig
 *  builds inside RenderRaceHudOverlays (0x4388A0, dispatched from the
 *  per-view tail at 0x4397.. as part of bit 0x80000000 handling). The
 *  0x0043E750 SetClipBounds entry point itself has no direct port analog;
 *  D3D11 backend writes scissor rects directly without staging them in
 *  global float fields.
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
 *   0x00414F40  RenderPositionerGlyphStrip  (density-match, verify in Phase 4)
 */


/* ============================================================
 * [ARCH-DIVERGENCE: HUD glyph strip render] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig RenderPositionerGlyphStrip rasterizes 8x8 glyphs into a DDraw HUD
 * surface for the positioner/race-position overlay. Port uses the unified
 * glyph-strip atlas + fe_draw_text helper (td5_hud references
 * td5_frontend's font system). Source-port collapses the HUD-specific
 * glyph rasterizer into the shared font path.
 *
 *   0x00414F40  RenderPositionerGlyphStrip  [ARCH-DIVERGENCE: HudGlyph]
 */

/* ========================================================================
 * UpdateWantedDamageIndicator — cop-chase damage HUD (Tier 1 port 2026-05-24)
 *
 * Port of UpdateWantedDamageIndicator @ 0x0043d4e0 (420B).
 *
 * Orig behavior: per-actor render-time call that emits 2 translucent quads
 * for the active cop's damage state.
 *   - Outer frame quad: fixed white outline above the cop's roof.
 *   - Inner fill quad : height scales with remaining damage
 *     ((0x1000 - gWantedDamageStateTable[slot]) → emptier as cop takes hits).
 * Anchored at model-space (0, 120, 0) projected through the current render
 * transform (which the per-actor render path has just loaded).
 *
 * Port replaces the orig BuildSpriteQuadTemplate-into-scratch +
 * QueueTranslucentPrimitiveBatch sequence with direct screen-space billboard
 * emission via the existing td5_plat_render_draw_tris pipeline
 * (TRANSLUCENT_POINT preset — same pattern as brake/tracked-marker billboards).
 *
 * Gate (orig @ 0x0043d4f5):
 *   wanted_mode_enabled != 0 && slot == g_wantedDamageHudOverlayCount
 * Port mirrors via td5_game_is_wanted_mode() + matching slot index.
 *
 * [CONFIRMED @ 0x0043d4e0 + orig disassembly]:
 *   - Anchor (0, 120, 0)        — DAT_00474868
 *   - Frame fill 128.0 / 128.0  — DAT_0045d600 / shared with depth_z=128
 *   - Half-extent constant 0.5  — _g_halfFloatConstant @ 0x0045d5d0
 *   - Vertical stride 2.0       — _g_simTickBudgetCap @ 0x0045d650
 *   - Damage scale * 1/4096     — DAT_0045d698
 * ======================================================================== */

/* [FIX 2026-05-24 OVERSIGHT: wanted-mode-init; orig 0x004aaf68]
 * The previously-extern'd g_wanted_mode_enabled was a stale parallel
 * global from a 2026 stub-migration commit (cf0777f) that was never
 * written anywhere — it shadowed the real flag g_td5.wanted_mode_enabled
 * (set by ConfigureGameTypeFlags case 8 @ td5_frontend.c:2820, mirroring
 * orig 0x00410ED2). Today's Tier 1 UpdateWantedDamageIndicator port
 * (commit fa1e910) hooked into the dead global, leaving the damage HUD
 * permanently inert in cop chase. Route through the real flag here. */
extern int16_t  g_wanted_damage_state[TD5_MAX_RACER_SLOTS];

/* Mirrors orig g_wantedDamageHudOverlayCount @ 0x004bf504 — selects the
 * single slot whose damage bar is displayed each frame.
 *
 * [FIX 2026-05-30 cop-chase] Previously hardcoded 0 (the player/cop slot),
 * but the player never takes ram damage, so the bar never appeared. The
 * orig sets this to the LAST-RAMMED SUSPECT (1..5) on first contact in
 * AwardWantedDamageScore @ 0x0043D690; the port tracks that slot in td5_ai.c
 * and exposes it here. Returns -1 when no suspect has been hit yet (gate
 * below rejects it, so no bar shows until the first ram). */
static int hud_wanted_active_slot(void) {
    return td5_ai_get_wanted_overlay_slot();
}

void td5_hud_update_wanted_damage_indicator(int actor_slot)
{
    /* Gate matches orig 0x0043d4f5:
     *   if (wanted_mode_enabled != 0 && slot == g_wantedDamageHudOverlayCount) */
    if (!g_td5.wanted_mode_enabled) return;
    if (actor_slot != hud_wanted_active_slot()) return;
    if ((unsigned)actor_slot >= (unsigned)TD5_MAX_RACER_SLOTS) return;

    /* Project model anchor (0, 120, 0) through the current render
     * transform — same hook the orig uses (WritePointToCurrent
     * RenderTransform @ 0x0042e4f0 from DAT_00474868). Then perspective-
     * project to screen space exactly like the brake/tracked-marker
     * billboards. */
    float sx, sy, sz, rhw;
    if (!td5_render_transform_and_project(0.0f, 120.0f, 0.0f,
                                          &sx, &sy, &sz, &rhw)) {
        /* Anchor behind near plane — nothing to draw this frame. */
        return;
    }

    /* Pulse-w base = (0x1000 - damage) * (1/4096) — orig at 0x0043d637:
     *   fVar3 = (0x1000 - gWantedDamageStateTable[slot]) * fVar2 * fVar1
     *           * _DAT_0045d698
     * where fVar1 = view_height * 1/4096 and fVar2 = fVar1 * digit_step.
     * Port uses fixed pixel constants for the bar size (screen-space
     * billboard floats above the car). The remaining-damage scalar is the
     * fraction we use to scale the inner fill height. */
    int16_t  dmg     = g_wanted_damage_state[actor_slot];
    if (dmg < 0)    dmg = 0;
    if (dmg > 0x1000) dmg = 0x1000;
    float    health  = (float)dmg * (1.0f / 4096.0f);             /* 1.0 = full */
    float    damage_frac = 1.0f - health;                          /* 0.0 = full */
    if (damage_frac < 0.0f) damage_frac = 0.0f;
    if (damage_frac > 1.0f) damage_frac = 1.0f;

    /* [FIX 2026-05-30 cop-chase, orig UpdateWantedDamageIndicator 0x0043D4E0]
     * Faithful sprite geometry. The orig draws a TEXTURED square frame (DAMAGE
     * sprite, side fVar2) with a narrow VERTICAL fill strip (DAMAGEB1 sprite)
     * that drains top-down as the suspect takes damage — NOT the previous port's
     * wide horizontal green->red solid rect. Both quads use a white (texture-
     * passthrough) diffuse; the sprite art carries the color. The orig uses a
     * FIXED screen size (it does not perspective-scale the quad), positioned at
     * the projected (0,120,0) anchor.
     *
     * Size: orig fVar1 = S*0.0015625 (= S/640), fVar2 = fVar1*16, where S
     * (0x004aaf08) is the ACTIVE DISPLAY WIDTH in px — written by
     * InitializeRaceSession @ 0x0042B4AD from the selected display mode
     * (CONFIRMED 2026-05-31). So the bar scales linearly with resolution: at
     * 640px fVar2=16, at 1280px fVar2=32, etc. The previous port hardcoded 16
     * (640 only), so the bar read too small at higher resolutions. */
    extern float g_render_width_f;
    const float FV1 = g_render_width_f * (1.0f / 640.0f);  /* orig fVar1 = S/640  */
    /* Original sizes: fVar2 = fVar1*16 (frame side); DAMAGEB1 needle = fVar1*4.
     * [USER DIVERGENCE 2026-06-01: 3x upscale per user — bigger damage indicator
     * over the suspect cars. fVar2 -> fVar1*48, needle -> fVar1*12 (keeps 4:1).] */
    const float FV2 = FV1 * 48.0f;                          /* frame side (px) — 3x orig */
    const float fill_w = FV1 * 12.0f;                       /* DAMAGEB1 needle width — 3x orig */

    /* Frame square: orig cx = anchorX - fVar2*0.5, cy = anchorY - (fVar2*0.5 +
     * fVar2) — centered on the projected anchor and lifted ~1.5*fVar2 above. */
    float frame_left   = sx - FV2 * 0.5f;
    float frame_top    = sy - (FV2 * 0.5f + FV2);
    float frame_right  = frame_left + FV2;
    float frame_bottom = frame_top  + FV2;

    /* [FIX 2026-05-24 damage-bar-atlas; orig 0x0043d4e0]
     * Orig InitializeWantedHudOverlays @ 0x0043d2d0 calls
     *   FindArchiveEntryByName(..., "DAMAGE")   -> 0x004beae0 frame quad
     *   FindArchiveEntryByName(..., "DAMAGEB1") -> 0x004bf448 fill  quad
     * (confirmed via Ghidra string refs s_DAMAGE_004748b8 + s_DAMAGEB1_004748ac;
     *  both keys present in re/assets/static/static.hed alongside DAMAGEB2).
     * The port samples a 1-pixel UV from each entry so the bound page just
     * needs to be the page that actually contains the sprite; using the
     * correct entries also makes the bar pick up the right tpage transparency
     * preset at bind time. */
    TD5_AtlasEntry *frame_atlas = td5_asset_find_atlas_entry(NULL, "DAMAGE");
    TD5_AtlasEntry *fill_atlas  = td5_asset_find_atlas_entry(NULL, "DAMAGEB1");
    int frame_page = frame_atlas ? frame_atlas->texture_page : -1;
    int fill_page  = fill_atlas  ? fill_atlas->texture_page  : -1;
    if (frame_page <= 0 || fill_page <= 0)
        return; /* atlas missing DAMAGE/DAMAGEB1 — silently skip */

    int frame_tw = 256, frame_th = 256;
    int fill_tw  = 256, fill_th  = 256;
    td5_plat_render_get_texture_dims(frame_page, &frame_tw, &frame_th);
    td5_plat_render_get_texture_dims(fill_page,  &fill_tw,  &fill_th);
    /* Full-sprite UVs (half-texel inset) — the orig draws the WHOLE DAMAGE /
     * DAMAGEB1 sprite, not a 1-pixel solid-color sample. */
    float frame_u0 = ((float)frame_atlas->atlas_x + 0.5f) / (float)frame_tw;
    float frame_v0 = ((float)frame_atlas->atlas_y + 0.5f) / (float)frame_th;
    float frame_u1 = ((float)(frame_atlas->atlas_x + frame_atlas->width)  - 0.5f) / (float)frame_tw;
    float frame_v1 = ((float)(frame_atlas->atlas_y + frame_atlas->height) - 0.5f) / (float)frame_th;
    float fill_u0  = ((float)fill_atlas->atlas_x  + 0.5f) / (float)fill_tw;
    float fill_v0  = ((float)fill_atlas->atlas_y  + 0.5f) / (float)fill_th;
    float fill_u1  = ((float)(fill_atlas->atlas_x + fill_atlas->width)  - 0.5f) / (float)fill_tw;
    float fill_v1  = ((float)(fill_atlas->atlas_y + fill_atlas->height) - 0.5f) / (float)fill_th;

    /* --- Quad 1: DAMAGE frame sprite (square, white = texture passthrough) --- */
    {
        TD5_D3DVertex q[4];
        q[0].screen_x = frame_left;  q[0].screen_y = frame_top;
        q[1].screen_x = frame_left;  q[1].screen_y = frame_bottom;
        q[2].screen_x = frame_right; q[2].screen_y = frame_top;
        q[3].screen_x = frame_right; q[3].screen_y = frame_bottom;
        q[0].tex_u = frame_u0; q[0].tex_v = frame_v0;
        q[1].tex_u = frame_u0; q[1].tex_v = frame_v1;
        q[2].tex_u = frame_u1; q[2].tex_v = frame_v0;
        q[3].tex_u = frame_u1; q[3].tex_v = frame_v1;
        for (int i = 0; i < 4; i++) {
            q[i].depth_z  = sz;
            q[i].rhw      = rhw;
            q[i].diffuse  = 0xFFFFFFFFu;   /* texture passthrough */
            q[i].specular = 0;
        }
        uint16_t idx[6] = { 0, 1, 2, 1, 3, 2 };
        td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
        td5_plat_render_bind_texture(frame_page);
        td5_plat_render_draw_tris(q, 4, idx, 6);
    }

    /* --- Quad 2: DAMAGEB1 fill needle (orig 0x4bf448) ---
     * [FIX 2026-06-01, re-decoded UpdateWantedDamageIndicator @ 0x0043D4E0]
     * The needle is a THIN vertical bar sitting at the frame's RIGHT EDGE
     * (x0 = frame_right, width = fVar1*4), draining from the TOP as health
     * drops — NOT centered inside the frame. So the frame + needle read as the
     * "damage indicator and arrow one near the other" the user expects.
     *   fVar3 (top inset) = fVar2 * (0x1000 - dmg)/0x1000  (= FV2 * damage_frac;
     *     orig literal fVar2*fVar1*(0x1000-dmg)/4096, identical at the 640 base)
     *   top = frame_top + fVar3 ; bottom = frame_bottom ; height = FV2 - fVar3 */
    {
        float fVar3 = FV2 * damage_frac;           /* top inset grows with damage */
        float fill_height = FV2 - fVar3;           /* = FV2 * health */
        if (fill_height > 0.5f) {
            float fl = frame_right;                /* immediately right of the frame */
            float fr = fl + fill_w;
            float ft = frame_top + fVar3;
            float fb = frame_bottom;

            TD5_D3DVertex q[4];
            q[0].screen_x = fl; q[0].screen_y = ft;
            q[1].screen_x = fl; q[1].screen_y = fb;
            q[2].screen_x = fr; q[2].screen_y = ft;
            q[3].screen_x = fr; q[3].screen_y = fb;
            q[0].tex_u = fill_u0; q[0].tex_v = fill_v0;
            q[1].tex_u = fill_u0; q[1].tex_v = fill_v1;
            q[2].tex_u = fill_u1; q[2].tex_v = fill_v0;
            q[3].tex_u = fill_u1; q[3].tex_v = fill_v1;
            for (int i = 0; i < 4; i++) {
                q[i].depth_z  = sz;
                q[i].rhw      = rhw;
                q[i].diffuse  = 0xFFFFFFFFu;   /* texture passthrough */
                q[i].specular = 0;
            }
            uint16_t idx[6] = { 0, 1, 2, 1, 3, 2 };
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT);
            td5_plat_render_bind_texture(fill_page);
            td5_plat_render_draw_tris(q, 4, idx, 6);
        }
    }

    /* Restore opaque preset so it doesn't leak into the next per-actor
     * pass (mirrors brake_lights / tracked-marker tails). */
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}
