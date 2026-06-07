/* td5_font.c -- runtime TTF glyph cache (stb_truetype). See td5_font.h.
 *
 * [S13] Rasterises the menu font straight from its TrueType outlines at the
 * requested pixel size into a shared GPU atlas page, so menu text stays crisp
 * at any window resolution (no bitmap/MSDF round-trip). Glyphs are cached by
 * (codepoint, rounded cap-pixel size); the atlas is re-uploaded only when new
 * glyphs are added. Trial fonts (MontBlanc Trial) replace the symbol glyphs
 * they withhold with an identical "TRIAL TEXT" watermark -- those (and any
 * truly-missing glyph) are detected at init and rendered from a fallback face.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "td5_platform.h"   /* td5_plat_render_upload_texture, TD5_LOG_* */
#include "td5_font.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define LOG_TAG "frontend"   /* route to frontend.log (the "font" tag has no sink) */

#define ATLAS_W    2048
#define ATLAS_H    2048
#define ATLAS_PAGE 984        /* free persistent page (frontend uses up to 983) */
#define GLYPH_PAD  1
#define CACHE_N    8192       /* power of two; open-addressing glyph cache */

#define MENU_TTF      "re/assets/frontend/menu.ttf"
#define HUD_TTF       "re/assets/frontend/hud.ttf"   /* in-race HUD overlay font (Rajdhani) */
#define TITLE_TTF     "re/assets/frontend/title.ttf" /* frontend screen-header font (Lunatica) */
#define FALLBACK_TTF  "C:\\Windows\\Fonts\\arialbd.ttf"

typedef struct { int key; int used; td5_glyph g; } GlyphEntry;

static int            s_tried = 0, s_ok = 0;
static stbtt_fontinfo s_primary, s_fallback;
static int            s_have_fb = 0;
static unsigned char *s_primary_buf = NULL, *s_fallback_buf = NULL;
static float          s_cap_primary = 1.0f, s_cap_fallback = 1.0f;
static uint8_t        s_use_fb[128];        /* 1 = route codepoint to fallback */

/* Secondary HUD face (Rajdhani). Shares the atlas + glyph cache with the menu
 * face — collisions are avoided by tagging the cache key with the face id (see
 * font_rasterize). No trial-watermark routing (Rajdhani is a complete font); any
 * truly-missing glyph routes to the same arialbd fallback. */
static int            s_hud_tried = 0, s_hud_ok = 0;
static stbtt_fontinfo s_hud_primary;
static unsigned char *s_hud_buf = NULL;
static float          s_hud_cap = 1.0f;

/* Tertiary title face (Lunatica). Same shared-atlas scheme as the HUD face;
 * cache key tagged with face id 2 (see font_rasterize). Any glyph the display
 * font lacks routes to the same arialbd fallback. */
static int            s_title_tried = 0, s_title_ok = 0;
static stbtt_fontinfo s_title_primary;
static unsigned char *s_title_buf = NULL;
static float          s_title_cap = 1.0f;

static uint32_t      *s_atlas = NULL;       /* ATLAS_W*ATLAS_H BGRA32 (A8R8G8B8) */
static int            s_cur_x = 0, s_cur_y = 0, s_shelf_h = 0;
static int            s_dirty = 0;
static GlyphEntry     s_cache[CACHE_N];

/* ---- helpers ------------------------------------------------------------- */

static unsigned char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    if (out_len) *out_len = n;
    return buf;
}

static float cap_units(stbtt_fontinfo *fi)
{
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(fi, 'H', &x0, &y0, &x1, &y1) && y1 > 0)
        return (float)y1;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    return (asc > 0) ? (float)asc * 0.7f : 1.0f;
}

static void pick_font(int cp, stbtt_fontinfo **fi, float *cap)
{
    if (cp >= 0 && cp < 128 && s_use_fb[cp] && s_have_fb) {
        *fi = &s_fallback; *cap = s_cap_fallback;
    } else {
        *fi = &s_primary;  *cap = s_cap_primary;
    }
}

static uint64_t fnv1a(const unsigned char *d, int n)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* Detect the codepoints the primary face withholds (missing, or replaced by a
 * single repeated trial-watermark glyph) and route them to the fallback. */
static void detect_watermark(void)
{
    static uint64_t hash[128];
    static unsigned char tmp[96 * 96];
    float scale = 28.0f / s_cap_primary;
    for (int cp = 33; cp < 127; cp++) {
        hash[cp] = 0;
        if (stbtt_FindGlyphIndex(&s_primary, cp) == 0) { s_use_fb[cp] = 1; continue; }
        int ix0, iy0, ix1, iy1;
        stbtt_GetCodepointBitmapBox(&s_primary, cp, scale, scale, &ix0, &iy0, &ix1, &iy1);
        int w = ix1 - ix0, h = iy1 - iy0;
        if (w <= 0 || h <= 0 || w > 96 || h > 96) continue;
        memset(tmp, 0, (size_t)(w * h));
        stbtt_MakeCodepointBitmap(&s_primary, tmp, w, h, w, scale, scale, cp);
        unsigned char hdr[2] = { (unsigned char)w, (unsigned char)h };
        uint64_t hh = fnv1a(hdr, 2) ^ fnv1a(tmp, w * h);
        hash[cp] = hh;
    }
    /* A watermark glyph maps onto many codepoints -> identical raster repeats. */
    for (int cp = 33; cp < 127; cp++) {
        if (!hash[cp]) continue;
        int n = 0;
        for (int o = 33; o < 127; o++) if (hash[o] == hash[cp]) n++;
        if (n >= 3) s_use_fb[cp] = 1;
    }
}

/* ---- public -------------------------------------------------------------- */

int td5_font_ready(void)
{
    if (s_tried) return s_ok;
    s_tried = 1;

    long len = 0;
    s_primary_buf = read_file(MENU_TTF, &len);
    if (!s_primary_buf ||
        !stbtt_InitFont(&s_primary, s_primary_buf,
                        stbtt_GetFontOffsetForIndex(s_primary_buf, 0))) {
        TD5_LOG_W(LOG_TAG, "menu TTF not loaded (%s) -- falling back to MSDF/bitmap font", MENU_TTF);
        s_ok = 0;
        return 0;
    }
    s_cap_primary = cap_units(&s_primary);

    s_fallback_buf = read_file(FALLBACK_TTF, &len);
    if (s_fallback_buf &&
        stbtt_InitFont(&s_fallback, s_fallback_buf,
                       stbtt_GetFontOffsetForIndex(s_fallback_buf, 0))) {
        s_have_fb = 1;
        s_cap_fallback = cap_units(&s_fallback);
    }

    memset(s_use_fb, 0, sizeof(s_use_fb));
    if (s_have_fb) detect_watermark();

    s_atlas = (uint32_t *)calloc((size_t)ATLAS_W * ATLAS_H, 4);
    if (!s_atlas) { s_ok = 0; return 0; }
    memset(s_cache, 0, sizeof(s_cache));

    int fb_n = 0;
    for (int i = 0; i < 128; i++) fb_n += s_use_fb[i];
    TD5_LOG_I(LOG_TAG, "menu TTF loaded: %s (cap_units=%.0f, fallback=%d, watermark/missing=%d)",
              MENU_TTF, s_cap_primary, s_have_fb, fb_n);
    s_ok = 1;
    return 1;
}

static void atlas_reset(void)
{
    memset(s_atlas, 0, (size_t)ATLAS_W * ATLAS_H * 4);
    memset(s_cache, 0, sizeof(s_cache));
    s_cur_x = s_cur_y = s_shelf_h = 0;
    s_dirty = 1;
}

static GlyphEntry *cache_slot(int key)
{
    uint32_t i = ((uint32_t)key * 2654435761u) & (CACHE_N - 1);
    for (int probe = 0; probe < CACHE_N; probe++) {
        GlyphEntry *e = &s_cache[i];
        if (!e->used || e->key == key) return e;
        i = (i + 1) & (CACHE_N - 1);
    }
    return &s_cache[0];   /* full (shouldn't happen: atlas overflows first) */
}

/* Rasterise `cp` at `cap_px` using face `fi` (cap-units `capu`), caching under a
 * face-tagged key (face 0 = menu, 1 = HUD, 2 = title) so the faces share the
 * atlas + cache without colliding. Face 0/1 keep the exact pre-refactor key
 * layout (face & 3 == face & 1 for those); face 2 sets the extra tag bit. */
static void font_rasterize(stbtt_fontinfo *fi, float capu, int face,
                           int cp, float cap_px, td5_glyph *out)
{
    out->valid = 0;
    if (cp < 0 || cp > 0x2FFFF) return;

    int size_q = (int)(cap_px + 0.5f);
    if (size_q < 1) size_q = 1;
    if (size_q > 0xFFF) size_q = 0xFFF;
    int key = ((face & 3) << 21) | ((cp & 0x1FF) << 12) | size_q;
    if (key == 0) key = 1;

    GlyphEntry *e = cache_slot(key);
    if (e->used && e->key == key) { *out = e->g; return; }

    float scale = cap_px / capu;

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);

    td5_glyph g;
    memset(&g, 0, sizeof(g));
    g.valid   = 1;
    g.page    = ATLAS_PAGE;
    g.advance = (float)adv * scale;

    int ix0, iy0, ix1, iy1;
    stbtt_GetCodepointBitmapBox(fi, cp, scale, scale, &ix0, &iy0, &ix1, &iy1);
    int gw = ix1 - ix0, gh = iy1 - iy0;
    if (gw > 0 && gh > 0) {
        if (s_cur_x + gw + GLYPH_PAD > ATLAS_W) {  /* next shelf */
            s_cur_x = 0; s_cur_y += s_shelf_h + GLYPH_PAD; s_shelf_h = 0;
        }
        if (s_cur_y + gh + GLYPH_PAD > ATLAS_H) {   /* atlas full -> rebuild */
            atlas_reset();
            e = cache_slot(key);
        }
        int px = s_cur_x, py = s_cur_y;
        unsigned char *tmp = (unsigned char *)malloc((size_t)gw * gh);
        if (tmp) {
            stbtt_MakeCodepointBitmap(fi, tmp, gw, gh, gw, scale, scale, cp);
            for (int y = 0; y < gh; y++) {
                uint32_t *row = &s_atlas[(size_t)(py + y) * ATLAS_W + px];
                const unsigned char *src = &tmp[(size_t)y * gw];
                for (int x = 0; x < gw; x++)
                    row[x] = ((uint32_t)src[x] << 24) | 0x00FFFFFFu;
            }
            free(tmp);
            s_cur_x += gw + GLYPH_PAD;
            if (gh + GLYPH_PAD > s_shelf_h) s_shelf_h = gh + GLYPH_PAD;
            g.w = (float)gw; g.h = (float)gh;
            g.xoff = (float)ix0; g.yoff = (float)iy0;
            g.u0 = (float)px / ATLAS_W;       g.v0 = (float)py / ATLAS_H;
            g.u1 = (float)(px + gw) / ATLAS_W; g.v1 = (float)(py + gh) / ATLAS_H;
            s_dirty = 1;
        }
    }

    e->key = key; e->used = 1; e->g = g;
    *out = g;
}

void td5_font_get(int cp, float cap_px, td5_glyph *out)
{
    out->valid = 0;
    if (!td5_font_ready()) return;
    stbtt_fontinfo *fi; float cap;
    pick_font(cp, &fi, &cap);
    font_rasterize(fi, cap, 0, cp, cap_px, out);
}

float td5_font_advance(int cp, float cap_px)
{
    if (!td5_font_ready()) return 0.0f;
    stbtt_fontinfo *fi; float cap;
    pick_font(cp, &fi, &cap);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    return (float)adv * (cap_px / cap);
}

/* ---- secondary HUD face (Rajdhani) -------------------------------------- */

int td5_hudfont_ready(void)
{
    if (s_hud_tried) return s_hud_ok;
    s_hud_tried = 1;
    /* The HUD face shares the menu face's atlas + cache, so the base font (and
     * thus the atlas) must be initialised first. */
    if (!td5_font_ready() || !s_atlas) { s_hud_ok = 0; return 0; }

    long len = 0;
    s_hud_buf = read_file(HUD_TTF, &len);
    if (!s_hud_buf ||
        !stbtt_InitFont(&s_hud_primary, s_hud_buf,
                        stbtt_GetFontOffsetForIndex(s_hud_buf, 0))) {
        TD5_LOG_W(LOG_TAG, "HUD TTF not loaded (%s) -- HUD text falls back to SDF/bitmap", HUD_TTF);
        s_hud_ok = 0;
        return 0;
    }
    s_hud_cap = cap_units(&s_hud_primary);
    TD5_LOG_I(LOG_TAG, "HUD TTF loaded: %s (cap_units=%.0f)", HUD_TTF, s_hud_cap);
    s_hud_ok = 1;
    return 1;
}

/* Pick the HUD face for `cp`, routing a glyph the HUD font lacks to the fallback. */
static void pick_hud_font(int cp, stbtt_fontinfo **fi, float *cap)
{
    if (cp >= 0 && s_have_fb && stbtt_FindGlyphIndex(&s_hud_primary, cp) == 0) {
        *fi = &s_fallback;    *cap = s_cap_fallback;
    } else {
        *fi = &s_hud_primary; *cap = s_hud_cap;
    }
}

void td5_hudfont_get(int cp, float cap_px, td5_glyph *out)
{
    out->valid = 0;
    if (!td5_hudfont_ready()) return;
    stbtt_fontinfo *fi; float cap;
    pick_hud_font(cp, &fi, &cap);
    font_rasterize(fi, cap, 1, cp, cap_px, out);
}

float td5_hudfont_advance(int cp, float cap_px)
{
    if (!td5_hudfont_ready()) return 0.0f;
    stbtt_fontinfo *fi; float cap;
    pick_hud_font(cp, &fi, &cap);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    return (float)adv * (cap_px / cap);
}

/* ---- tertiary title face (Lunatica) ------------------------------------- */

int td5_titlefont_ready(void)
{
    if (s_title_tried) return s_title_ok;
    s_title_tried = 1;
    /* Shares the menu face's atlas + cache, so the base font must init first. */
    if (!td5_font_ready() || !s_atlas) { s_title_ok = 0; return 0; }

    long len = 0;
    s_title_buf = read_file(TITLE_TTF, &len);
    if (!s_title_buf ||
        !stbtt_InitFont(&s_title_primary, s_title_buf,
                        stbtt_GetFontOffsetForIndex(s_title_buf, 0))) {
        TD5_LOG_W(LOG_TAG, "title TTF not loaded (%s) -- screen titles fall back to the strip art", TITLE_TTF);
        s_title_ok = 0;
        return 0;
    }
    s_title_cap = cap_units(&s_title_primary);
    TD5_LOG_I(LOG_TAG, "title TTF loaded: %s (cap_units=%.0f)", TITLE_TTF, s_title_cap);
    s_title_ok = 1;
    return 1;
}

/* Pick the title face for `cp`, routing a glyph the display font lacks to the
 * arialbd fallback (Lunatica is a demo font and omits many glyphs). */
static void pick_title_font(int cp, stbtt_fontinfo **fi, float *cap)
{
    if (cp >= 0 && s_have_fb && stbtt_FindGlyphIndex(&s_title_primary, cp) == 0) {
        *fi = &s_fallback;      *cap = s_cap_fallback;
    } else {
        *fi = &s_title_primary; *cap = s_title_cap;
    }
}

void td5_titlefont_get(int cp, float cap_px, td5_glyph *out)
{
    out->valid = 0;
    if (!td5_titlefont_ready()) return;
    stbtt_fontinfo *fi; float cap;
    pick_title_font(cp, &fi, &cap);
    font_rasterize(fi, cap, 2, cp, cap_px, out);
}

float td5_titlefont_advance(int cp, float cap_px)
{
    if (!td5_titlefont_ready()) return 0.0f;
    stbtt_fontinfo *fi; float cap;
    pick_title_font(cp, &fi, &cap);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    return (float)adv * (cap_px / cap);
}

void td5_font_flush_uploads(void)
{
    if (!s_ok || !s_dirty) return;
    td5_plat_render_upload_texture(ATLAS_PAGE, s_atlas, ATLAS_W, ATLAS_H, 2);
    s_dirty = 0;
}
