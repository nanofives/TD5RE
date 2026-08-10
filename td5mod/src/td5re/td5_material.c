/**
 * td5_material.c -- Material identity table (lighting rework P0 / RT2-P5)
 *
 * See td5_material.h. Base classification is page-transparency-class based (P0);
 * the cache is a flat byte array indexed by page id (0xFF = unclassified).
 *
 * [RT2-P5] adds a load-time shininess detector: td5_material_classify_page()
 * runs once per page at upload with the decoded texels and computes a per-page
 * reflectivity + specular-sharpness from the pixel distribution. Opaque pages
 * whose pixels read as water / wet gloss arm a HIGH-only material-id upgrade
 * (DEFAULT -> WATER) so RT reflections pick them up. The upgrade is gated on
 * td5_rt_active(), so LOW rendering (and the byte-identical gate) is untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td5_material.h"
#include "td5_asset.h"
#include "td5_rt.h"        /* [P5] td5_rt_active() — HIGH-only upgrade gate */
#include "td5_platform.h"

#define LOG_TAG "render"

#define MAT_PAGE_MAX 1024          /* matches the shared GPU texture table */

static uint8_t s_page_mat[MAT_PAGE_MAX];
static int     s_cache_init = 0;

/* Indexed by TD5_MAT_*. [P3] reflectivity feeds the SSR / RT reflection pass:
 * DEFAULT world geometry is 0 (roads reflect only via the wet-weather boost,
 * buildings never turn glossy); CARBODY gives car paint its sheen; GLASS is
 * strongest; [P5] WATER sits between glass and car paint (smooth, mirror-ish
 * but tinted). */
static const TD5_MaterialParams k_params[TD5_MAT_COUNT] = {
    /* NONE    */ { 0.00f, 1.00f, 0.00f, 1.00f },
    /* DEFAULT */ { 0.10f, 0.85f, 0.00f, 0.00f },
    /* CUTOUT  */ { 0.02f, 0.95f, 0.00f, 0.00f },
    /* GLASS   */ { 0.60f, 0.20f, 0.40f, 0.00f },
    /* GLOW    */ { 0.00f, 1.00f, 0.00f, 1.00f },
    /* CARBODY */ { 0.50f, 0.30f, 0.30f, 0.00f },
    /* WATER   */ { 0.70f, 0.15f, 0.55f, 0.00f },
    /* WETROAD */ { 0.10f, 0.85f, 0.00f, 0.00f },   /* dry = matte like DEFAULT; the
                                                     * very-faint wet sheen is added
                                                     * by the shader wet boost, gated
                                                     * on this matid, only in rain. */
};

/* ----------------------------------------------------------------------------
 * [RT2-P5] Per-page pixel-derived shininess state.
 * -------------------------------------------------------------------------- */

static uint8_t s_page_refl[MAT_PAGE_MAX];    /* 0..255 reflectivity weight     */
static uint8_t s_page_shine[MAT_PAGE_MAX];   /* 0..255 specular sharpness       */
static uint8_t s_page_water[MAT_PAGE_MAX];   /* 1 = opaque page reads as water  */
static uint8_t s_page_wetroad[MAT_PAGE_MAX]; /* 1 = opaque page reads as road/pavement (wet-reflective) */
static uint8_t s_page_seen[MAT_PAGE_MAX];    /* 1 = classify ran (for CSV dump) */

/* Cached CSV row stats (only populated when TD5RE_MAT_DUMP is set). */
typedef struct {
    int   w, h, fmt;
    float meanR, meanG, meanB;
    float sat, luma, var, bluefrac, blackfrac;
    uint8_t verdict;    /* the s_page_water arming decision   */
    uint8_t verdict_wr; /* the s_page_wetroad arming decision */
} MatRow;
static MatRow *s_rows = NULL;   /* [MAT_PAGE_MAX] lazily alloc'd when dumping */

/* Classifier thresholds — every one an env knob (dev tuning). Cached once. */
static int   s_knobs_init = 0;
static float s_w_bluefrac = 0.45f;   /* TD5RE_MAT_WATER_BLUE   min blue-dominant frac */
static float s_w_var_max  = 0.045f;  /* TD5RE_MAT_WATER_VAR    max luma variance (smooth) */
static float s_w_luma_lo  = 0.08f;   /* TD5RE_MAT_WATER_LUMA_LO */
static float s_w_luma_hi  = 0.62f;   /* TD5RE_MAT_WATER_LUMA_HI */
static float s_w_sat_min  = 0.12f;   /* TD5RE_MAT_WATER_SAT    min mean saturation */
static float s_w_refl     = 0.55f;   /* TD5RE_MAT_WATER_REFL   armed reflectivity */
static float s_shine_var  = 0.06f;   /* TD5RE_MAT_SHINE_VAR    var below -> shiny */

/* [RT-NIGHT 2026-08-10] Wet-road / pavement classifier thresholds. A page arms
 * s_page_wetroad when it reads as an unsaturated grey paved surface (asphalt /
 * concrete): low saturation, mid-dark luma, not green-dominant (grass) and not
 * blue-dominant (water/sky), and not extremely noisy (foliage/gravel). Every
 * threshold is an env knob for tuning against log/material_pages.csv. */
static float s_wr_sat_max = 0.22f;   /* TD5RE_MAT_ROAD_SAT     max mean saturation (grey) */
static float s_wr_luma_lo = 0.10f;   /* TD5RE_MAT_ROAD_LUMA_LO */
static float s_wr_luma_hi = 0.60f;   /* TD5RE_MAT_ROAD_LUMA_HI */
static float s_wr_var_max = 0.055f;  /* TD5RE_MAT_ROAD_VAR     max luma variance (paved, not gravel/leaf) */
static float s_wr_green   = 0.03f;   /* TD5RE_MAT_ROAD_GREEN   reject if meanG exceeds R & B by this (grass) */

static float knob(const char *name, float dflt)
{
    const char *e = getenv(name);
    return (e && e[0]) ? (float)atof(e) : dflt;
}

static void mat_knobs_ensure(void)
{
    if (s_knobs_init) return;
    s_knobs_init = 1;
    s_w_bluefrac = knob("TD5RE_MAT_WATER_BLUE",    s_w_bluefrac);
    s_w_var_max  = knob("TD5RE_MAT_WATER_VAR",     s_w_var_max);
    s_w_luma_lo  = knob("TD5RE_MAT_WATER_LUMA_LO", s_w_luma_lo);
    s_w_luma_hi  = knob("TD5RE_MAT_WATER_LUMA_HI", s_w_luma_hi);
    s_w_sat_min  = knob("TD5RE_MAT_WATER_SAT",     s_w_sat_min);
    s_w_refl     = knob("TD5RE_MAT_WATER_REFL",    s_w_refl);
    s_shine_var  = knob("TD5RE_MAT_SHINE_VAR",     s_shine_var);
    s_wr_sat_max = knob("TD5RE_MAT_ROAD_SAT",      s_wr_sat_max);
    s_wr_luma_lo = knob("TD5RE_MAT_ROAD_LUMA_LO",  s_wr_luma_lo);
    s_wr_luma_hi = knob("TD5RE_MAT_ROAD_LUMA_HI",  s_wr_luma_hi);
    s_wr_var_max = knob("TD5RE_MAT_ROAD_VAR",      s_wr_var_max);
    s_wr_green   = knob("TD5RE_MAT_ROAD_GREEN",    s_wr_green);
}

static void mat_cache_ensure(void)
{
    if (s_cache_init) return;
    s_cache_init = 1;
    memset(s_page_mat, 0xFF, sizeof(s_page_mat));
}

/* ----------------------------------------------------------------------------
 * Base classification (P0) — transparency class -> material id.
 * -------------------------------------------------------------------------- */

static uint8_t mat_base_id(int page)
{
    mat_cache_ensure();
    uint8_t id = s_page_mat[page];
    if (id != 0xFF) return id;

    /* Transparency class -> material class (coarse P0 mapping):
     *   0 opaque      -> DEFAULT
     *   1 alpha-test  -> CUTOUT
     *   2 translucent -> GLASS
     *   3 additive    -> GLOW (emissive; G-buffer never written for it
     *                    anyway because additive draws don't z-write) */
    switch (td5_asset_get_page_transparency(page)) {
    case 1:  id = TD5_MAT_CUTOUT;  break;
    case 2:  id = TD5_MAT_GLASS;   break;
    case 3:  id = TD5_MAT_GLOW;    break;
    default: id = TD5_MAT_DEFAULT; break;
    }
    s_page_mat[page] = id;
    return id;
}

uint8_t td5_material_id_for_page(int page)
{
    if (page < 0 || page >= MAT_PAGE_MAX) return TD5_MAT_NONE;
    uint8_t id = mat_base_id(page);

    /* [RT2-P5] HIGH-only shine upgrade: an opaque page whose texels read as
     * water/wet gloss becomes reflective for the RT passes. Gated on
     * td5_rt_active() so LOW keeps the exact P0 mapping (byte-identical gate).
     * Only DEFAULT is eligible — CUTOUT/GLASS/GLOW keep their own response. */
    if (id == TD5_MAT_DEFAULT && td5_rt_active()) {
        if (s_page_water[page])   return TD5_MAT_WATER;
        /* [RT-NIGHT 2026-08-10] Road/pavement pages become WETROAD so ONLY they
         * pick up the faint wet-weather reflection in the RT pass; grass/dirt/
         * terrain stay DEFAULT (reflectivity 0). HIGH-only, so LOW keeps the P0
         * mapping (the LOW screen-space wet boost still keys on DEFAULT). */
        if (s_page_wetroad[page]) return TD5_MAT_WETROAD;
    }

    return id;
}

const TD5_MaterialParams *td5_material_params(int id)
{
    if (id < 0 || id >= TD5_MAT_COUNT) id = TD5_MAT_DEFAULT;
    return &k_params[id];
}

void td5_material_reset_cache(void)
{
    s_cache_init = 0;
    memset(s_page_refl,    0, sizeof(s_page_refl));
    memset(s_page_shine,   0, sizeof(s_page_shine));
    memset(s_page_water,   0, sizeof(s_page_water));
    memset(s_page_wetroad, 0, sizeof(s_page_wetroad));
    memset(s_page_seen,    0, sizeof(s_page_seen));
    TD5_LOG_I(LOG_TAG, "material: page cache reset");
}

/* ----------------------------------------------------------------------------
 * [RT2-P5] Load-time shininess detector.
 * -------------------------------------------------------------------------- */

float td5_material_page_reflectivity(int page)
{
    if (page < 0 || page >= MAT_PAGE_MAX) return 0.0f;
    return s_page_refl[page] * (1.0f / 255.0f);
}

float td5_material_page_shine(int page)
{
    if (page < 0 || page >= MAT_PAGE_MAX) return 0.0f;
    return s_page_shine[page] * (1.0f / 255.0f);
}

static uint8_t clamp_u8(float v01)
{
    int v = (int)(v01 * 255.0f + 0.5f);
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

void td5_material_classify_page(int page, const void *pixels,
                                int width, int height, int format)
{
    if (page < 0 || page >= MAT_PAGE_MAX) return;
    if (!pixels || width <= 0 || height <= 0) return;

    mat_knobs_ensure();

    /* Only 32-bit BGRA (format 2) carries reliable colour for detection; 16-bit
     * pages (roads are rarely 16-bit) are marked seen but left at 0. */
    if (format != 2) {
        s_page_refl[page]    = 0;
        s_page_shine[page]   = 0;
        s_page_water[page]   = 0;
        s_page_wetroad[page] = 0;
        s_page_seen[page]    = 1;
        return;
    }

    const unsigned char *px = (const unsigned char *)pixels;
    long total = (long)width * height;

    /* Stride so a huge atlas is sampled in ~<=4096 texels (cheap, once/page). */
    long target = 4096;
    long step = (total > target) ? (total / target) : 1;

    double sR = 0, sG = 0, sB = 0, sL = 0, sLL = 0, sSat = 0;
    long   nBlue = 0, nBlack = 0, n = 0;

    for (long i = 0; i < total; i += step) {
        const unsigned char *p = px + (size_t)i * 4;   /* BGRA */
        float b = p[0] * (1.0f / 255.0f);
        float g = p[1] * (1.0f / 255.0f);
        float r = p[2] * (1.0f / 255.0f);
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        float sat = (mx > 1e-4f) ? (mx - mn) / mx : 0.0f;

        sR += r; sG += g; sB += b; sL += l; sLL += (double)l * l; sSat += sat;
        if (b > r + 0.06f && b > g + 0.03f) nBlue++;
        if (l < 0.06f) nBlack++;
        n++;
    }
    if (n <= 0) return;

    float meanR = (float)(sR / n), meanG = (float)(sG / n), meanB = (float)(sB / n);
    float luma  = (float)(sL / n);
    float var   = (float)(sLL / n - (double)luma * luma);
    if (var < 0.0f) var = 0.0f;
    float sat    = (float)(sSat / n);
    float bluef  = (float)nBlue / (float)n;
    float blackf = (float)nBlack / (float)n;

    /* Water / wet-gloss: blue-dominant, smooth (low luma variance), mid luma,
     * with some colour. Opaque pages only — arms the HIGH-only upgrade. */
    int is_water = (bluef >= s_w_bluefrac) &&
                   (var   <= s_w_var_max)  &&
                   (luma  >= s_w_luma_lo && luma <= s_w_luma_hi) &&
                   (sat   >= s_w_sat_min) &&
                   (td5_asset_get_page_transparency(page) <= 0);

    /* [RT-NIGHT 2026-08-10] Wet-road / pavement: an unsaturated grey paved
     * surface (asphalt/concrete) -- low mean saturation, mid-dark luma, not
     * green-dominant (excludes grass/verge), not extremely noisy (excludes
     * gravel/foliage). Water wins if both match. Opaque pages only. This is the
     * SELECTIVE replacement for the old "every up-facing DEFAULT pixel is wet"
     * blanket that mirrored the whole ground. */
    int green_dom = (meanG > meanR + s_wr_green) && (meanG > meanB + s_wr_green);
    int is_wetroad = !is_water &&
                     (sat  <= s_wr_sat_max) &&
                     (var  <= s_wr_var_max) &&
                     (luma >= s_wr_luma_lo && luma <= s_wr_luma_hi) &&
                     (bluef < s_w_bluefrac) &&
                     !green_dom &&
                     (td5_asset_get_page_transparency(page) <= 0);

    /* Reflectivity: water gets the armed weight; other smooth surfaces get a
     * mild sheen scaled by how flat (low-variance) they are; noisy textures
     * (asphalt gravel, foliage) stay matte. */
    float refl;
    if (is_water) {
        refl = s_w_refl;
    } else {
        float smooth = (var < s_shine_var) ? (1.0f - var / s_shine_var) : 0.0f;
        refl = 0.10f * smooth;            /* subtle; DEFAULT LUT stays authoritative */
    }

    /* Shine (specular sharpness): smoother -> sharper highlight. */
    float shine = (var < s_shine_var) ? (1.0f - var / s_shine_var) : 0.0f;

    s_page_refl[page]    = clamp_u8(refl);
    s_page_shine[page]   = clamp_u8(shine);
    s_page_water[page]   = (uint8_t)(is_water ? 1 : 0);
    s_page_wetroad[page] = (uint8_t)(is_wetroad ? 1 : 0);
    s_page_seen[page]    = 1;

    /* Stash full stats for the CSV deliverable (dev only). */
    if (getenv("TD5RE_MAT_DUMP")) {
        if (!s_rows) s_rows = (MatRow *)calloc(MAT_PAGE_MAX, sizeof(MatRow));
        if (s_rows) {
            MatRow *row = &s_rows[page];
            row->w = width; row->h = height; row->fmt = format;
            row->meanR = meanR; row->meanG = meanG; row->meanB = meanB;
            row->sat = sat; row->luma = luma; row->var = var;
            row->bluefrac = bluef; row->blackfrac = blackf;
            row->verdict = s_page_water[page];
            row->verdict_wr = s_page_wetroad[page];
        }
        td5_material_dump_csv();
    }
}

void td5_material_dump_csv(void)
{
    if (!getenv("TD5RE_MAT_DUMP") || !s_rows) return;

    FILE *f = fopen("log/material_pages.csv", "w");
    if (!f) return;

    fprintf(f, "page,w,h,fmt,transp,base_mat,meanR,meanG,meanB,sat,luma,var,"
               "bluefrac,blackfrac,shine,refl,water,wetroad\n");
    for (int p = 0; p < MAT_PAGE_MAX; p++) {
        if (!s_page_seen[p]) continue;
        const MatRow *r = &s_rows[p];
        fprintf(f,
            "%d,%d,%d,%d,%d,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
            p, r->w, r->h, r->fmt,
            td5_asset_get_page_transparency(p), (unsigned)mat_base_id(p),
            r->meanR, r->meanG, r->meanB, r->sat, r->luma, r->var,
            r->bluefrac, r->blackfrac,
            s_page_shine[p] * (1.0f / 255.0f),
            s_page_refl[p]  * (1.0f / 255.0f),
            (int)s_page_water[p], (int)s_page_wetroad[p]);
    }
    fclose(f);
}
