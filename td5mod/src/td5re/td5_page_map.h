#ifndef TD5_PAGE_MAP_H
#define TD5_PAGE_MAP_H

/* ======================================================================
 * td5_page_map.h — SINGLE SOURCE OF TRUTH for the D3D texture-page layout.
 *
 * The port uploads every texture (level art, car skins, HUD chrome, frontend
 * surfaces, props) into one flat GPU page table of TD5_PAGE_TABLE_SIZE slots
 * (see MAX_TEXTURE_PAGES in td5_platform_win32.c, s_page_remap[1024], etc.).
 * The table has two zones:
 *
 *   [0 .. TD5_PAGE_LEVEL_CEIL-1]   LEVEL zone — the loaded track's TEXTURES.DAT
 *                                  pages, 0-based. The AUTO-GENERATED track is
 *                                  the widest user of this zone (it emits
 *                                  TD5_TG_PAGE_COUNT pages); shipped tracks use
 *                                  far fewer.
 *   [TD5_PAGE_LEVEL_CEIL .. 1023]  RESERVED zone — fixed port-owned ranges that
 *                                  the level zone must never reach.
 *
 * WHY THIS FILE EXISTS. The reserved bases used to be hardcoded literals
 * scattered across td5_asset.c / td5_hud.c / td5_render_effects.c / the
 * *_internal.h headers, while TD5_TG_PAGE_COUNT kept growing round after round
 * as the generator added scenery pages. The static atlas base (700) had already
 * been overtaken by the auto-track's page span (0..720): 21 static-atlas pages
 * were being overwritten by trackgen facade/wall art, so the in-race pause menu
 * (whose slider/panel art comes from the static atlas) sampled a building
 * texture. Because nothing checked the ranges at build time, the collision was
 * silent — it only showed up as corrupted UI on auto tracks.
 *
 * Centralizing every base here, plus the _Static_assert block at the bottom,
 * makes the layout non-regressable: the next page added to the generator (or a
 * reservation grown past its neighbour) FAILS THE BUILD instead of corrupting
 * the UI at runtime.
 *
 * RULES:
 *   - Every reserved range lives here as a (BASE, COUNT) pair, kept sorted by
 *     BASE. Consumers include this header and use these macros — never
 *     re-#define a base locally.
 *   - Keep the ranges non-overlapping and inside [TD5_PAGE_LEVEL_CEIL, 1024).
 *     The asserts below enforce both; if you add/move a range, add it to the
 *     sorted chain of asserts too.
 *   - TD5_PAGE_LEVEL_CEIL must equal the lowest reserved base. Raising the
 *     ceiling to give the generator more room means moving the whole reserved
 *     block up together.
 * ====================================================================== */

/* Flat GPU page table size. MUST match MAX_TEXTURE_PAGES (td5_platform_win32.c),
 * s_page_remap[] / TD5_PAGE_TRANSPARENCY_MAX / TD5_TRACK_TEXTURE_PAGE_LIMIT
 * (td5_asset.c) and MAT_PAGE_MAX (td5_material.c). */
#define TD5_PAGE_TABLE_SIZE   1024

/* First reserved page. Level/track pages occupy [0 .. this-1].
 * td5_trackgen_internal.h static-asserts TD5_TG_PAGE_COUNT <= this. */
#define TD5_PAGE_LEVEL_CEIL   800

/* --- Reserved ranges, sorted by base -------------------------------------- */

/* Car skins/hubs: 800 + slot*2 = skin, +1 = hub, 6 racers → 800..811. */
#define TD5_CAR_TEXTURE_PAGE_BASE      800
#define TD5_CAR_TEXTURE_PAGE_COUNT     12

/* Traffic skins: base + (traffic_idx % 6) → 820..825. */
#define TD5_TRAFFIC_TEXTURE_PAGE_BASE  820
#define TD5_TRAFFIC_TEXTURE_PAGE_COUNT 6

/* Static atlas (frontend/HUD chrome incl. the pause-menu slider/panel art):
 * 32 pages, one per static.hed page-metadata slot (STATIC_PAGE_META_MAX).
 * MOVED from 700 to 832 (2026-09-13) — 700 sat inside the auto-track's level
 * zone and was being clobbered by trackgen art (the pause-menu facade bug). */
#define STATIC_ATLAS_BASE              832
#define STATIC_ATLAS_PAGE_COUNT        32

/* 1x1 white page used by HUD vector fills / minimap / gauges. */
#define HUD_WHITE_TEX_PAGE             899

/* Frontend compositing surfaces: 900..931. */
#define FE_SURFACE_PAGE_BASE           900
#define FE_SURFACE_PAGE_COUNT          32

/* Per-track environment/reflection pages: 990..993. */
#define ENVMAP_TEXTURE_PAGE_BASE       990
#define ENVMAP_TEXTURE_PAGE_COUNT      4

/* Alloy wheel-rim styles: 994..1001. */
#define WHEEL_RIM_TEX_BASE             994
#define WHEEL_RIM_TEX_COUNT            8

/* TD6 prop textures + 1 dedicated white page: 1002..1007. */
#define TD6_PROP_TEX_BASE              1002
#define TD6_PROP_TEX_COUNT             6

/* --- Compile-time invariants (the non-regressable guard) ------------------
 * (1) Every reserved base sits at or above the level ceiling.
 * (2) Ranges do not overlap (checked pairwise in sorted-by-base order).
 * (3) The whole reserved block fits inside the GPU page table.
 * TD5_TG_PAGE_COUNT <= TD5_PAGE_LEVEL_CEIL is asserted in
 * td5_trackgen_internal.h, where that constant is defined. */

/* (1) lowest reserved base defines the ceiling */
_Static_assert(TD5_CAR_TEXTURE_PAGE_BASE == TD5_PAGE_LEVEL_CEIL,
               "TD5_PAGE_LEVEL_CEIL must equal the lowest reserved page base");

/* (2) no overlaps, sorted by base */
_Static_assert(TD5_CAR_TEXTURE_PAGE_BASE + TD5_CAR_TEXTURE_PAGE_COUNT
               <= TD5_TRAFFIC_TEXTURE_PAGE_BASE,
               "CAR page range overlaps TRAFFIC");
_Static_assert(TD5_TRAFFIC_TEXTURE_PAGE_BASE + TD5_TRAFFIC_TEXTURE_PAGE_COUNT
               <= STATIC_ATLAS_BASE,
               "TRAFFIC page range overlaps STATIC ATLAS");
_Static_assert(STATIC_ATLAS_BASE + STATIC_ATLAS_PAGE_COUNT
               <= HUD_WHITE_TEX_PAGE,
               "STATIC ATLAS page range overlaps HUD white page");
_Static_assert(HUD_WHITE_TEX_PAGE + 1 <= FE_SURFACE_PAGE_BASE,
               "HUD white page overlaps FRONTEND surfaces");
_Static_assert(FE_SURFACE_PAGE_BASE + FE_SURFACE_PAGE_COUNT
               <= ENVMAP_TEXTURE_PAGE_BASE,
               "FRONTEND surfaces overlap ENVMAP pages");
_Static_assert(ENVMAP_TEXTURE_PAGE_BASE + ENVMAP_TEXTURE_PAGE_COUNT
               <= WHEEL_RIM_TEX_BASE,
               "ENVMAP pages overlap WHEEL RIM pages");
_Static_assert(WHEEL_RIM_TEX_BASE + WHEEL_RIM_TEX_COUNT
               <= TD6_PROP_TEX_BASE,
               "WHEEL RIM pages overlap TD6 PROP pages");

/* (3) fits in the table */
_Static_assert(TD6_PROP_TEX_BASE + TD6_PROP_TEX_COUNT <= TD5_PAGE_TABLE_SIZE,
               "reserved page block runs past the GPU page table");

#endif /* TD5_PAGE_MAP_H */
