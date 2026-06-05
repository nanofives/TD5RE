/* td5_font.h -- runtime TTF glyph cache (stb_truetype) for the frontend menu.
 *
 * [S13] Instead of round-tripping a TTF through a low-res bitmap atlas + MSDF,
 * this rasterises glyphs directly from the TrueType outlines at the exact pixel
 * size needed and caches them in a shared GPU atlas page -- crisp menu text at
 * any window resolution, with the font's real metrics. The frontend's
 * fe_draw_text uses it when ready and falls back to the MSDF/bitmap path
 * otherwise (so there is no regression if the TTF is missing).
 *
 * The module only provides glyph rasters + metrics; the caller (td5_frontend.c)
 * does the actual quad drawing through its own fe_draw_quad.
 */
#ifndef TD5_FONT_H
#define TD5_FONT_H

typedef struct {
    int   valid;            /* 1 = usable (may still be a zero-size space glyph) */
    int   page;             /* texture page holding the glyph atlas */
    float u0, v0, u1, v1;   /* atlas UVs */
    float w, h;             /* glyph bitmap size in px (0 for space/empty) */
    float xoff, yoff;       /* px offset from the pen: x = left bearing, y = top
                             * relative to the baseline (negative = above it) */
    float advance;          /* horizontal advance in px at this cap size */
} td5_glyph;

/* Lazy-initialises on first call. Returns 1 if a usable TTF is loaded. */
int   td5_font_ready(void);

/* Rasterise (if needed) and return glyph `codepoint` sized so caps are
 * `cap_px` tall. Caches by (codepoint, rounded cap_px). */
void  td5_font_get(int codepoint, float cap_px, td5_glyph *out);

/* Horizontal advance (px) for `codepoint` at `cap_px`, without rasterising. */
float td5_font_advance(int codepoint, float cap_px);

/* Upload any newly-rasterised glyphs to the GPU page. Call once after a batch
 * of td5_font_get() / td5_hudfont_get() and before drawing the quads (the two
 * faces share one GPU atlas page). No-op when nothing changed. */
void  td5_font_flush_uploads(void);

/* ---- Secondary HUD face (Rajdhani, re/assets/frontend/hud.ttf) -----------
 * A distinct typeface for the in-race HUD overlay text, sharing the same glyph
 * cache + GPU atlas page as the menu face (so td5_font_flush_uploads() uploads
 * both). Same semantics as the menu equivalents above; returns/rasterises
 * nothing until td5_hudfont_ready() succeeds (it requires the menu face to have
 * initialised the shared atlas first). */
int   td5_hudfont_ready(void);
void  td5_hudfont_get(int codepoint, float cap_px, td5_glyph *out);
float td5_hudfont_advance(int codepoint, float cap_px);

#endif /* TD5_FONT_H */
