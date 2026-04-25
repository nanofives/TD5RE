/**
 * td5_hud.h -- Race HUD, minimap, text rendering, pause menu overlay
 *
 * Original functions:
 *   0x4377B0  InitializeRaceOverlayResources
 *   0x437BA0  InitializeRaceHudLayout
 *   0x4388A0  RenderRaceHudOverlays (master per-frame)
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

#ifndef TD5_HUD_H
#define TD5_HUD_H

#include "td5_types.h"
#include <stdarg.h>

/* ========================================================================
 * HUD visibility bitmask flags
 * ======================================================================== */

#define TD5_HUD_POSITION_LABEL   0x001   /* bit 0:  Race position "1ST".."6TH" */
#define TD5_HUD_LAP_TIMERS       0x002   /* bit 1:  Per-racer lap timers */
#define TD5_HUD_SPEEDOMETER      0x004   /* bit 2:  Analog speedometer + digits */
#define TD5_HUD_RESERVED_3       0x008   /* bit 3:  Reserved (always set) */
#define TD5_HUD_UTURN_WARNING    0x010   /* bit 4:  U-turn warning icon */
#define TD5_HUD_RESERVED_5       0x020   /* bit 5:  Reserved (always set) */
#define TD5_HUD_METRIC_DIGITS    0x040   /* bit 6:  Metric digit readout */
#define TD5_HUD_TOTAL_TIMER      0x080   /* bit 7:  Total race timer "%s %d" */
#define TD5_HUD_LAP_COUNTER      0x100   /* bit 8:  Lap/checkpoint counter */
#define TD5_HUD_CIRCUIT_LAPS     0x200   /* bit 9:  Circuit lap count */
#define TD5_HUD_REPLAY_BANNER    0x80000000u /* bit 31: "REPLAY" banner */

/* ========================================================================
 * HUD Metric display modes
 * ======================================================================== */

#define TD5_METRIC_FINISH_TIMER  0
#define TD5_METRIC_FPS           1
#define TD5_METRIC_ODOMETER      2
#define TD5_METRIC_BYTE_METRIC   3

/* ========================================================================
 * Constants
 * ======================================================================== */

#define TD5_HUD_VIRTUAL_WIDTH    640
#define TD5_HUD_VIRTUAL_HEIGHT   480

#define TD5_HUD_MAX_VIEWS        2
#define TD5_HUD_VIEW_STRIDE      0x1148  /* bytes per view HUD primitive storage */

#define TD5_HUD_MAX_TEXT_GLYPHS  256
#define TD5_HUD_GLYPH_QUAD_SIZE  0xB8    /* bytes per glyph quad */
#define TD5_HUD_TEXT_BUF_SIZE    0xB800  /* 256 * 0xB8 */
#define TD5_HUD_GLYPH_TABLE_SIZE 0x404   /* 64 glyphs * 16 bytes + 4 bytes tex ptr */

#define TD5_HUD_FONT_GRID_COLS   16
#define TD5_HUD_FONT_GRID_ROWS   4
#define TD5_HUD_FONT_GLYPH_COUNT 64

#define TD5_HUD_MINIMAP_BUF_SIZE 0x5C00

#define TD5_HUD_PAUSE_MAX_PAGES  8
#define TD5_HUD_PAUSE_MAX_QUADS  256

/* ========================================================================
 * Sprite quad template (0xB8 bytes, matches original layout)
 * ======================================================================== */

typedef struct TD5_SpriteQuad {
    uint8_t  data[0xB8];
} TD5_SpriteQuad;

/* ========================================================================
 * Atlas entry (returned by archive lookup)
 * ======================================================================== */

typedef struct TD5_AtlasEntry {
    int32_t  atlas_x;       /* +0x2C: atlas X origin */
    int32_t  atlas_y;       /* +0x30: atlas Y origin */
    int32_t  width;         /* +0x34: atlas width */
    int32_t  height;        /* +0x38: atlas height */
    int32_t  texture_page;  /* +0x3C: texture page index */
} TD5_AtlasEntry;

/* ========================================================================
 * Per-font glyph record
 * ======================================================================== */

typedef struct TD5_GlyphRecord {
    float atlas_u;
    float atlas_v;
    float width;
    float height;
} TD5_GlyphRecord;

/* ========================================================================
 * Per-view HUD scale / layout
 * ======================================================================== */

typedef struct TD5_HudViewLayout {
    float scale_x;          /* [0] renderWidth / 640 */
    float scale_y;          /* [1] renderHeight / 480 */
    float center_x;         /* [2] viewport horizontal center */
    float center_y;         /* [3] viewport vertical center */
    float vp_left;          /* [4] viewport left edge */
    float vp_top;           /* [5] viewport top edge */
    float vp_right;         /* [6] viewport right edge */
    float vp_bottom;        /* [7] viewport bottom edge */
    float half_width;       /* [8] (vp_right + vp_left) / 2 */
    float half_height;      /* [9] (vp_bottom + vp_top) / 2 */
    float vp_int_left;      /* [10] integer left */
    float vp_int_top;       /* [11] integer top */
    float vp_int_right;     /* [12] integer right */
    float vp_int_bottom;    /* [13] integer bottom */
} TD5_HudViewLayout;

/* ========================================================================
 * Minimap state
 * ======================================================================== */

typedef struct TD5_MinimapState {
    float  map_x;
    float  map_y;
    float  map_width;
    float  map_height;
    float  world_scale_x;
    float  world_scale_y;
    float  dot_size;
    int    segment_count;
    int    branch_count;
    /* Route segment ranges */
    int16_t seg_start[32];  /* route segment start span */
    int16_t seg_end[32];    /* route segment end span */
    int16_t seg_branch[32]; /* branch link (-1 = none) */
} TD5_MinimapState;

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_hud_init(void);
void td5_hud_shutdown(void);
void td5_hud_render(void);

/* --- Initialization chain --- */
void td5_hud_init_overlay_resources(int race_mode, int string_table_offset);
void td5_hud_init_layout(int viewport_mode);
void td5_hud_init_minimap_layout(void);
void td5_hud_init_pause_menu(int page_index);
void td5_hud_init_font_atlas(void);

/* --- Per-frame rendering --- */
void td5_hud_render_overlays(float dt);
int  td5_hud_build_metric_digits(void);
void td5_hud_draw_status_text(int player_slot, int view_index);
void td5_hud_render_minimap(int view_index);
void td5_hud_draw_pause_overlay(void);
void td5_hud_update_pause_overlay(int cursor, float cd_music_frac, float sfx_frac, float audio3_frac);
void td5_hud_draw_race_fade(float progress, int direction);

/* --- State control --- */
void td5_hud_set_indicator_state(int view_index, int value);

/* --- Text pipeline --- */
void td5_hud_queue_text(int font_index, int x, int y, int centered,
                         const char *fmt, ...);
void td5_hud_flush_text(void);

#endif /* TD5_HUD_H */
