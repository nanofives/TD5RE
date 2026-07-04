/**
 * td5_material.h -- Material identity table (lighting rework P0)
 *
 * Maps texture pages to a small material id that rides bits 31..24 of the
 * TD5_D3DVertex.specular dword into the G-buffer (see LIGHTING_REWORK_PLAN.md
 * section 5). Id 0 is the "emissive / no G-buffer data" sentinel: HUD, VFX,
 * billboards and any vertex that never went through the lighting pass stays 0
 * and the GPU passes leave those pixels untouched.
 *
 * P0 classification comes from the texture page's transparency class
 * (td5_asset_get_page_transparency): it is deliberately coarse — the table
 * exists so the id pipeline (CPU pack -> G-buffer -> resolve) is wired
 * end-to-end. Later phases refine ids (road detection from span walk, car
 * body pages, glass) and load per-material params from an editable source
 * asset (assetsrc pattern).
 */
#ifndef TD5_MATERIAL_H
#define TD5_MATERIAL_H

#include <stdint.h>

enum {
    TD5_MAT_NONE     = 0,   /* emissive / unlit sentinel — never packed      */
    TD5_MAT_DEFAULT  = 1,   /* opaque world geometry (roads reflect when wet) */
    TD5_MAT_CUTOUT   = 2,   /* alpha-tested (fences, foliage)                */
    TD5_MAT_GLASS    = 3,   /* translucent-blend pages                       */
    TD5_MAT_GLOW     = 4,   /* additive pages (street-light halos etc.)      */
    TD5_MAT_CARBODY  = 5,   /* [P3] vehicle bodies (rotated-basis meshes)    */
    TD5_MAT_COUNT    = 6
};

/* Per-material lighting response. Consumed by the GPU passes from P1 on;
 * defined now so the table shape is stable. All values 0..1. */
typedef struct TD5_MaterialParams {
    float specular;      /* specular highlight strength                     */
    float roughness;     /* 0 = mirror, 1 = fully diffuse                   */
    float reflectivity;  /* SSR / envmap weight (P3)                        */
    float emissive;      /* self-lit fraction (shadows/relight skip)        */
} TD5_MaterialParams;

/* Material id for a texture page (cached; cheap enough for per-polygon use).
 * Returns TD5_MAT_NONE for invalid pages. */
uint8_t td5_material_id_for_page(int page);

/* Parameter block for a material id (never NULL — out-of-range ids map to
 * TD5_MAT_DEFAULT). */
const TD5_MaterialParams *td5_material_params(int id);

/* Drop the page->id cache (call when texture pages are (re)loaded so a page
 * reused by a different track/car re-classifies). */
void td5_material_reset_cache(void);

#endif /* TD5_MATERIAL_H */
