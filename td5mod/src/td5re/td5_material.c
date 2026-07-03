/**
 * td5_material.c -- Material identity table (lighting rework P0)
 *
 * See td5_material.h. Classification is page-transparency-class based in P0;
 * the cache is a flat byte array indexed by page id (0xFF = unclassified).
 */
#include <string.h>

#include "td5_material.h"
#include "td5_asset.h"
#include "td5_platform.h"

#define LOG_TAG "render"

#define MAT_PAGE_MAX 1024          /* matches the shared GPU texture table */

static uint8_t s_page_mat[MAT_PAGE_MAX];
static int     s_cache_init = 0;

/* Indexed by TD5_MAT_*; tuned later (P1/P3) — P0 only wires the pipeline. */
static const TD5_MaterialParams k_params[TD5_MAT_COUNT] = {
    /* NONE    */ { 0.00f, 1.00f, 0.00f, 1.00f },
    /* DEFAULT */ { 0.10f, 0.85f, 0.05f, 0.00f },
    /* CUTOUT  */ { 0.02f, 0.95f, 0.00f, 0.00f },
    /* GLASS   */ { 0.60f, 0.20f, 0.40f, 0.00f },
    /* GLOW    */ { 0.00f, 1.00f, 0.00f, 1.00f },
};

static void mat_cache_ensure(void)
{
    if (s_cache_init) return;
    s_cache_init = 1;
    memset(s_page_mat, 0xFF, sizeof(s_page_mat));
}

uint8_t td5_material_id_for_page(int page)
{
    if (page < 0 || page >= MAT_PAGE_MAX) return TD5_MAT_NONE;
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

const TD5_MaterialParams *td5_material_params(int id)
{
    if (id < 0 || id >= TD5_MAT_COUNT) id = TD5_MAT_DEFAULT;
    return &k_params[id];
}

void td5_material_reset_cache(void)
{
    s_cache_init = 0;
    TD5_LOG_I(LOG_TAG, "material: page cache reset");
}
