/**
 * td5_frontend_button_cache.c -- Baked main-menu button surface cache (CPU).
 *
 * Replaces the per-frame 9-slice + per-glyph fan in fe_draw_button_9slice +
 * fe_draw_text with a one-shot CPU bake into a 224x64 BGRA texture.
 *
 * Mirrors the original (CreateFrontendDisplayModeButton @ 0x00425DE0):
 *   - State 1 (unselected, blue) lives at y = 0..h-1.
 *   - State 0 (selected,   gold) lives at y = h..2h-1.
 *   - For each half: DrawFrontendButtonBackground (9-slice) then
 *     DrawFrontendLocalizedStringToSurface (label centered, top-aligned).
 *
 * No GPU render-target plumbing -- the original itself does CPU surface
 * compositing (DDraw BltFast on a SYSMEM surface). We composite into a
 * malloc'd BGRA32 buffer and upload through the existing texture-page path.
 */

#include "td5_frontend_button_cache.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "td5_asset.h"
#include "td5_platform.h"

#define LOG_TAG "frontend"

/* Texture page range for cached button surfaces. Pages 940..955 sit
 * above the frontend surface pool (900..931) and the title-strip pool
 * (931..939) and below the 1024-page hard limit, with no collisions
 * against the wheel-hub range (800..823) or the other shared assets
 * (894..899, 888..892). */
#define BTNCACHE_PAGE_BASE 940

/* ButtonBits.png 9-slice geometry (mirror of fe_draw_button_9slice).
 * Source layout from FUN_00425b60: two columns (left 26px, right 28px),
 * three states stacked at 32-row intervals, edge-tile strip at rows 96..99
 * indexed by state*12. */
#define BB_LW   26
#define BB_RW   28
#define BB_RX   28
#define BB_TILE  4

/* Font atlas geometry (BodyText.png, 240x552 with 24x24 cells, 10 cols). */
#define FONT_CELL 24
#define FONT_COLS 10

typedef struct {
    int      page;          /* texture page (BTNCACHE_PAGE_BASE + index), or -1 */
    int      texw;          /* full texture width (== half_w) */
    int      texh;          /* full texture height (== 2 * half_h) */
    int      half_w;
    int      half_h;
    uint32_t *pixels;       /* BGRA32, retained for device-reset recovery */
    char     label[64];
} BtnCacheSlot;

static BtnCacheSlot s_slots[TD5_FE_BTNCACHE_MAX];

/* Retained ButtonBits + BodyText source BGRA buffers. Loaded once on
 * first ensure_page() call; held until shutdown so re-bakes triggered
 * by screen transitions are cheap (just CPU compositing, no PNG decode). */
static struct {
    uint32_t *bb_pixels;
    int       bbw, bbh;
    uint32_t *font_pixels;
    int       fw, fh;
    int       attempted; /* 1 if we already tried to load (success or fail) */
    int       loaded;    /* 1 if both buffers are valid */
} s_src;

/* ------------------------------------------------------------------ helpers */

static void slot_release_pixels(BtnCacheSlot *s) {
    if (s->pixels) {
        free(s->pixels);
        s->pixels = NULL;
    }
    s->page = -1;
    s->texw = s->texh = 0;
    s->half_w = s->half_h = 0;
    s->label[0] = '\0';
}

/* Alpha-aware copy of one rect from src into dst. Skips fully-transparent
 * source texels (alpha == 0) so colorkey'd pixels in ButtonBits / BodyText
 * don't punch through previously composited content. Source texels with
 * alpha > 0 are written verbatim (no blending) -- mirrors DDraw BltFast
 * with SRCCOLORKEY semantics. */
static void blit_rect(uint32_t *dst, int dw, int dh, int dx, int dy,
                      const uint32_t *src, int sw, int sh,
                      int sx, int sy, int rw, int rh) {
    if (dx < 0) { sx -= dx; rw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; rh += dy; dy = 0; }
    if (sx < 0) { dx -= sx; rw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; rh += sy; sy = 0; }
    if (dx + rw > dw) rw = dw - dx;
    if (dy + rh > dh) rh = dh - dy;
    if (sx + rw > sw) rw = sw - sx;
    if (sy + rh > sh) rh = sh - sy;
    if (rw <= 0 || rh <= 0) return;

    for (int y = 0; y < rh; y++) {
        const uint32_t *sr = src + (sy + y) * sw + sx;
        uint32_t       *dr = dst + (dy + y) * dw + dx;
        for (int x = 0; x < rw; x++) {
            uint32_t p = sr[x];
            if ((p >> 24) == 0) continue;  /* fully transparent (colorkeyed) */
            dr[x] = p;
        }
    }
}

/* Solid color fill of a sub-rect (opaque, ignores prior contents). */
static void fill_rect(uint32_t *dst, int dw, int dh, int dx, int dy,
                      int rw, int rh, uint32_t color) {
    if (dx < 0) { rw += dx; dx = 0; }
    if (dy < 0) { rh += dy; dy = 0; }
    if (dx + rw > dw) rw = dw - dx;
    if (dy + rh > dh) rh = dh - dy;
    if (rw <= 0 || rh <= 0) return;
    for (int y = 0; y < rh; y++) {
        uint32_t *dr = dst + (dy + y) * dw + dx;
        for (int x = 0; x < rw; x++) dr[x] = color;
    }
}

/* Composite the 9-slice frame for one half (state 0 or 1) at vertical
 * origin `yo` inside the cache buffer. Mirrors fe_draw_button_9slice
 * ordering: optional interior fill (state 0 only) -> horizontal edges ->
 * vertical edges -> 4 corners (corners last so they overwrite edge bleed).
 *
 * Sub-rect tile coordinates are 1:1 source-pixel coords from the
 * 56x100 ButtonBits page; the cached texture is also at source pixel
 * scale (224x64) so no per-half scaling is needed. The per-frame quad
 * draw scales the entire cache to the on-screen button rect. */
static void composite_half(uint32_t *dst, int dw, int dh, int yo,
                           int bw, int bh, int state,
                           const uint32_t *bb, int bbw, int bbh) {
    const int tl_h = 13, tr_h = 9, bl_h = 9, br_h = 13;
    const int yb   = state * 32;

    if (state == 0) {
        const uint32_t fill = 0xFF392152u;
        /* Top strip between corners */
        fill_rect(dst, dw, dh, BB_LW, yo,
                  bw - BB_LW - BB_RW, bh, fill);
        /* Left column between TL/BL corners (asymmetric: tl_h=13, bl_h=9) */
        fill_rect(dst, dw, dh, 0, yo + tl_h,
                  BB_LW, bh - tl_h - bl_h, fill);
        /* Right column between TR/BR corners (asymmetric: tr_h=9, br_h=13).
         * Must use the right-column y bounds, NOT the left's, otherwise rows
         * yo+9..yo+12 stay alpha=0 and the LINEAR-filtered draw shows a 4-row
         * gap on the right side of the button. */
        fill_rect(dst, dw, dh, bw - BB_RW, yo + tr_h,
                  BB_RW, bh - tr_h - br_h, fill);
    }

    /* Horizontal edge tiles (4x4 from rows 96..99). */
    {
        const int te_sx = state * 12 + 22;  /* top edge column */
        const int be_sx = state * 12 + 28;  /* bottom edge column */
        for (int x = BB_LW; x + BB_RW < bw; x += BB_TILE) {
            blit_rect(dst, dw, dh, x, yo,
                      bb, bbw, bbh, te_sx, 96, BB_TILE, BB_TILE);
            blit_rect(dst, dw, dh, x, yo + bh - BB_TILE,
                      bb, bbw, bbh, be_sx, 96, BB_TILE, BB_TILE);
        }
    }

    /* Vertical edge tiles. Left and right use ASYMMETRIC corner heights
     * (mirrors the runtime fe_draw_button_9slice which has two separate
     * loops). Single shared loop is wrong: it leaves a 4-row gap on the
     * right column (rows yo+9..yo+12 in state 1) because the right
     * column's TR corner is only 9 px tall but the loop's y-min uses the
     * left column's tl_h=13. */
    for (int y = yo + tl_h; y < yo + bh - bl_h; y += BB_TILE) {
        blit_rect(dst, dw, dh, 0, y,
                  bb, bbw, bbh, 0, yb, BB_LW, BB_TILE);
    }
    for (int y = yo + tr_h; y < yo + bh - br_h; y += BB_TILE) {
        blit_rect(dst, dw, dh, bw - BB_RW, y,
                  bb, bbw, bbh, BB_RX, yb, BB_RW, BB_TILE);
    }

    /* Corners (drawn last to cover edge overlap). */
    blit_rect(dst, dw, dh, 0,           yo,
              bb, bbw, bbh, 0,     yb +  6, BB_LW, tl_h);
    blit_rect(dst, dw, dh, bw - BB_RW,  yo,
              bb, bbw, bbh, BB_RX, yb +  6, BB_RW, tr_h);
    blit_rect(dst, dw, dh, 0,           yo + bh - bl_h,
              bb, bbw, bbh, 0,     yb + 21, BB_LW, bl_h);
    blit_rect(dst, dw, dh, bw - BB_RW,  yo + bh - br_h,
              bb, bbw, bbh, BB_RX, yb + 17, BB_RW, br_h);
}

/* Measure the rendered width of `label` using the same per-glyph advance
 * table the runtime fe_draw_text uses, so the centered X matches the
 * pre-Phase-6 layout exactly. */
static int measure_label_px(const char *label, const uint8_t *adv) {
    int w = 0;
    if (!label) return 0;
    for (int i = 0; label[i]; i++) {
        int c = toupper((unsigned char)label[i]);
        if (c < 32 || c > 127) { w += 14; continue; }
        w += adv[c - 0x20];
    }
    return w;
}

/* Composite glyphs of `label` onto one half of the cache buffer.
 * Mirrors DrawFrontendLocalizedStringToSurface (0x00424560): glyph cell
 * 24x24 from BodyText, advance per-glyph (clamped to 0x18). Centered
 * horizontally at y = yo (no vertical centering -- original draws at the
 * exact half boundary). */
static void composite_label(uint32_t *dst, int dw, int dh, int yo,
                            int bw, const char *label,
                            const uint32_t *font, int fw, int fh,
                            const uint8_t *adv) {
    if (!label || !label[0]) return;
    int text_w = measure_label_px(label, adv);
    int cx     = (bw - text_w) / 2;
    if (cx < 0) cx = 0;

    for (int i = 0; label[i]; i++) {
        int c = toupper((unsigned char)label[i]);
        if (c < 32 || c > 127) { cx += 14; continue; }
        int gi   = c - 0x20;
        int aw   = adv[gi];
        if (aw > FONT_CELL) aw = FONT_CELL;
        if (c == ' ') { cx += aw; continue; }
        int col  = gi % FONT_COLS;
        int row  = gi / FONT_COLS;
        int sx   = col * FONT_CELL;
        int sy   = row * FONT_CELL;
        blit_rect(dst, dw, dh, cx, yo,
                  font, fw, fh, sx, sy, aw, FONT_CELL);
        cx += aw;
    }
}

/* ----------------------------------------------------------------- API */

void td5_fe_btncache_reset(void) {
    for (int i = 0; i < TD5_FE_BTNCACHE_MAX; i++) {
        slot_release_pixels(&s_slots[i]);
    }
}

int td5_fe_btncache_bake_button(int index, const char *label, int w, int h,
                                const uint8_t *bb_bgra, int bbw, int bbh,
                                const uint8_t *font_bgra, int fw, int fh,
                                const uint8_t *glyph_advance) {
    if (index < 0 || index >= TD5_FE_BTNCACHE_MAX) return 0;
    if (w <= 0 || h <= 0) return 0;
    if (!bb_bgra || bbw <= 0 || bbh <= 0) return 0;
    if (!font_bgra || fw <= 0 || fh <= 0 || !glyph_advance) return 0;

    BtnCacheSlot *s = &s_slots[index];
    slot_release_pixels(s);

    const int texw = w;
    const int texh = h * 2;
    const size_t bytes = (size_t)texw * (size_t)texh * 4u;
    s->pixels = (uint32_t *)calloc(1, bytes);
    if (!s->pixels) {
        TD5_LOG_W(LOG_TAG, "btncache[%d]: alloc %dx%d failed", index, texw, texh);
        return 0;
    }
    s->texw = texw;
    s->texh = texh;
    s->half_w = w;
    s->half_h = h;
    strncpy(s->label, label ? label : "", sizeof(s->label) - 1);
    s->label[sizeof(s->label) - 1] = '\0';

    /* Top half = state 1 (unselected/blue, transparent interior).
     * Bottom half = state 0 (selected/gold, purple interior). */
    composite_half(s->pixels, texw, texh, 0, w, h, 1,
                   (const uint32_t *)bb_bgra, bbw, bbh);
    composite_half(s->pixels, texw, texh, h, w, h, 0,
                   (const uint32_t *)bb_bgra, bbw, bbh);
    composite_label(s->pixels, texw, texh, 0,     w, s->label,
                    (const uint32_t *)font_bgra, fw, fh, glyph_advance);
    composite_label(s->pixels, texw, texh, h,     w, s->label,
                    (const uint32_t *)font_bgra, fw, fh, glyph_advance);

    s->page = BTNCACHE_PAGE_BASE + index;
    if (!td5_plat_render_upload_texture(s->page, s->pixels, texw, texh, 2)) {
        TD5_LOG_W(LOG_TAG, "btncache[%d]: upload page=%d %dx%d failed",
                  index, s->page, texw, texh);
        slot_release_pixels(s);
        return 0;
    }
    TD5_LOG_I(LOG_TAG, "btncache[%d]: baked '%s' page=%d %dx%d",
              index, s->label, s->page, texw, texh);
    return 1;
}

int td5_fe_btncache_get_page(int index, const char *label) {
    if (index < 0 || index >= TD5_FE_BTNCACHE_MAX) return -1;
    BtnCacheSlot *s = &s_slots[index];
    if (s->page < 0 || !s->pixels) return -1;
    if (label && strcmp(s->label, label) != 0) return -1;
    return s->page;
}

static int load_source_pixels(void) {
    if (s_src.loaded) return 1;
    if (s_src.attempted) return 0;  /* one-shot retry guard */
    s_src.attempted = 1;

    void *bb = NULL;
    int bbw = 0, bbh = 0;
    if (!td5_asset_load_png_to_buffer("re/assets/frontend/ButtonBits.png",
                                       TD5_COLORKEY_BLACK, &bb, &bbw, &bbh)) {
        TD5_LOG_W(LOG_TAG, "btncache: ButtonBits.png load failed");
        return 0;
    }
    void *fp = NULL;
    int fw = 0, fh = 0;
    if (!td5_asset_load_png_to_buffer("re/assets/frontend/BodyText.png",
                                       TD5_COLORKEY_BLACK, &fp, &fw, &fh)) {
        TD5_LOG_W(LOG_TAG, "btncache: BodyText.png load failed");
        free(bb);
        return 0;
    }
    s_src.bb_pixels = (uint32_t *)bb;
    s_src.bbw = bbw;
    s_src.bbh = bbh;
    s_src.font_pixels = (uint32_t *)fp;
    s_src.fw = fw;
    s_src.fh = fh;
    s_src.loaded = 1;
    TD5_LOG_I(LOG_TAG, "btncache: source pixels loaded BB=%dx%d Font=%dx%d",
              bbw, bbh, fw, fh);
    return 1;
}

int td5_fe_btncache_ensure_page(int index, const char *label, int w, int h,
                                const uint8_t *glyph_advance) {
    if (index < 0 || index >= TD5_FE_BTNCACHE_MAX) return -1;
    if (w <= 0 || h <= 0 || !label || !glyph_advance) return -1;

    BtnCacheSlot *s = &s_slots[index];
    /* Cache hit: page valid AND label matches AND dims match. */
    if (s->page >= 0 && s->pixels &&
        s->half_w == w && s->half_h == h &&
        strcmp(s->label, label) == 0) {
        return s->page;
    }

    if (!load_source_pixels()) return -1;

    if (!td5_fe_btncache_bake_button(index, label, w, h,
                                     (const uint8_t *)s_src.bb_pixels,
                                     s_src.bbw, s_src.bbh,
                                     (const uint8_t *)s_src.font_pixels,
                                     s_src.fw, s_src.fh,
                                     glyph_advance)) {
        return -1;
    }
    return s->page;
}

void td5_fe_btncache_release_sources(void) {
    if (s_src.bb_pixels)   { free(s_src.bb_pixels);   s_src.bb_pixels = NULL; }
    if (s_src.font_pixels) { free(s_src.font_pixels); s_src.font_pixels = NULL; }
    s_src.bbw = s_src.bbh = s_src.fw = s_src.fh = 0;
    s_src.loaded = 0;
    s_src.attempted = 0;
}

void td5_fe_btncache_recover(void) {
    for (int i = 0; i < TD5_FE_BTNCACHE_MAX; i++) {
        BtnCacheSlot *s = &s_slots[i];
        if (s->page < 0 || !s->pixels) continue;
        if (!td5_plat_render_upload_texture(s->page, s->pixels,
                                            s->texw, s->texh, 2)) {
            TD5_LOG_W(LOG_TAG, "btncache[%d]: re-upload page=%d failed",
                      i, s->page);
        }
    }
}
