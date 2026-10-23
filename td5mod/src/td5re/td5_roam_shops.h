/**
 * td5_roam_shops.h -- GEO FREE ROAM: SHOPFRONTS and the ground-floor identity of a footprint
 * (PORT-ONLY, roam LB).
 *
 * WHAT IT BUILDS. A footprint with a business on it gets, on every wall that faces a street, a
 * ground-floor SHOPFRONT laid 6 cm proud of the facade: a glazed shop page (TD5_TG_PAGE_STORE, the
 * shipped Sydney / San Francisco storefronts) from the pavement to the shop's head height, a FASCIA
 * band above it in the category's sign colour, and by category an AWNING (food always, kiosks and
 * shops by the footprint hash; never deeper than the pavement leaves room for) and a PHARMACY GREEN
 * CROSS (a blade sign standing out of the wall). A fuel station (amenity=fuel, a node or a forecourt
 * way) gets a forecourt CANOPY on four posts with its brand's fascia and two pump islands under it.
 * Every colour is a tint of one white page (TD5_TG_PAGE_SNOW): no new texture page.
 *
 * WHERE THE BUSINESS COMES FROM. The footprint's own shop= / amenity= / office= tags
 * (TD5_GeoBuilding.poi_cat, on disk since the tiles were fetched), else POIS.JSON in the place
 * directory (re/tools/geo_pois.py: OSM shop / amenity / office NODES joined offline to the footprint
 * that holds them or to the nearest wall within 15 m). A joined POI also says WHICH wall: the
 * storefront of a big footprint is the street wall nearest to the POI point.
 *
 * KNOBS. TD5RE_ROAM_SHOPS=0 builds none of it (the tile bytes are then the round-J bytes).
 * TD5RE_ROAM_SHOP_POIS=0 ignores POIS.JSON (footprint tags only), TD5RE_ROAM_SHOP_AWNINGS=0,
 * TD5RE_ROAM_SHOP_CANOPY=0 drop one element each.
 *
 * THREADING. A source is owned by one scenery source (one thread). POIS.JSON is parsed LAZILY on the
 * first tile build (strtod under the building thread's FP env, memory project_roam_id_railways).
 */
#ifndef TD5_ROAM_SHOPS_H
#define TD5_ROAM_SHOPS_H

#include <stdint.h>
#include "td5_geo_buildings.h"

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamShopSource TD5_RoamShopSource;

/* One tile's census (footprints OWNED by the tile). */
typedef struct {
    int  cand;                      /* footprints with a business, by either source              */
    int  from_tag, from_poi;        /* ... whose category came from the footprint / a POIS point */
    int  dressed;                   /* footprints given a shopfront                              */
    int  by_cat[TD5_GEOB_POI__N];   /* dressed footprints by category                            */
    int  fronts;                    /* street walls dressed                                      */
    int  no_front;                  /* candidates with no wall facing a street within reach      */
    int  skip_kind;                 /* candidates skipped: landmark / civic / part / relation    */
    int  skip_low;                  /* ... too low for a shop floor (< 2.6 m)                    */
    int  awnings, awning_tight;     /* awnings drawn / refused (pavement too narrow or too low)  */
    int  crosses;                   /* pharmacy green crosses                                    */
    int  canopies, canopy_refused;  /* fuel forecourt canopies drawn / sites with no clear spot  */
    int  canopy_islands;            /* ... with their pump islands (clear of every road)         */
    int  canopy_service;            /* ... whose deck spans a forecourt SERVICE drive            */
    int  meshes;
    double front_m;                 /* metres of shopfront                                       */
    /* [roam PE] fuel footprints drawn as the canopy / kept as a building (no clear rectangle), owned sites skipped
     * because their footprint is the canopy; street walls dropped / trimmed because another footprint covers their
     * outside; fronts that got the shop's NAME on the fascia, named fronts with no atlas cell, unnamed fronts */
    int  canopy_fp, canopy_fp_kept, canopy_fp_site;
    int  abut_drop, abut_trim;
    int  name_signs, name_nocell, name_none;
} TD5_RoamShopStats;

/* Every fact the shopfront needs about one emitted footprint, in the tile frame. */
typedef struct {
    double id;                      /* BUILDINGS record id (the POIS.JSON join key)              */
    const TD5_GeoBuilding *gb;
    const double *rx, *rz;          /* the ring as it was built (cleared + shifted), n points    */
    int    n;
    double by;                      /* the core's base y (ground at the centroid + kerb, capped) */
    double kerb_h;                  /* the city kerb, so a per-vertex foot matches the core's    */
    int    sb;                      /* sub-block the building went into                          */
    /* 1 when a tile-local point is inside a footprint: a wall with another building between it and
     * the street is not a street wall (NULL = never) */
    int  (*in_bld)(void *u, double x, double z);
    void  *u;
} TD5_RoamShopBld;

int  td5_roam_shops_enabled(void);  /* TD5RE_ROAM_SHOPS (default 1) */

/* [roam PE] SHOP NAMES on the fascia (TD5RE_ROAM_SHOP_NAMES, default 1): a bank of their own pages
 * (ROAM_SHOP_PAGE_BASE.., td5_page_map.h), apart from the street plates and the station boards. Session, MAIN thread,
 * before any worker builds a tile: the named footprints of POIS.JSON, one 64 x 16 cell per distinct name, the most used
 * names first, at most ROAM_SHOP_PAGE_COUNT * 4 cells (a name too long to read at 5 px after abbreviation is skipped).
 * _session_begin also uploads the pages; _atlas_prepare alone is for the dev CLI. */
int  td5_roam_shops_atlas_prepare(const char *place);
int  td5_roam_shops_session_begin(const char *place);
/* the atlas cell of a name: page id, the cell's top / bottom v (inset); 0 = no cell */
int  td5_roam_shops_name_cell(const char *name, int *page_id, double *v_top, double *v_bot);
/* census of the atlas: distinct names wanted, cells placed, skipped by the cap, skipped as unreadable, abbreviated */
void td5_roam_shops_atlas_census(int *wanted, int *placed, int *cap_drop, int *long_drop, int *abbrev);

/* NULL when the knob is off. POIS.JSON is optional (tags alone still dress tagged footprints). */
TD5_RoamShopSource *td5_roam_shops_open(const char *place);
void td5_roam_shops_close(TD5_RoamShopSource *s);
/* After the first build: POIS.JSON footprints / fuel sites loaded (0 / 0 without the file). */
void td5_roam_shops_summary(const TD5_RoamShopSource *s, int *nbld, int *nfuel, int *loaded);

/* 1 when this footprint is a candidate (a category from tags or POIS.JSON); *cat gets it. */
int  td5_roam_shops_category(TD5_RoamShopSource *s, double id, const TD5_GeoBuilding *gb, int *cat, int *from_poi);

/* Dress one emitted footprint. 1 on success (including "nothing to dress"), 0 on out of memory. */
int  td5_roam_shops_building(TD5_RoamShopSource *s, TG_TileCtx *c, const TD5_RoamShopBld *b, TD5_RoamShopStats *st);

/* Fuel forecourt canopies of the tile, PLANNED (in the building pass: in_bld(u, x, z) = 1 when the
 * tile-local point is inside a footprint, the canopy keeps off buildings) then WRITTEN by _fuel_flush
 * in the furniture tail: the deck may span a forecourt's service drive, which the 2D F4a census would
 * read as "on the road"; the posts and pump islands stand on no drivable surface and the deck covers no
 * real street (this module's own test). 1 on success, 0 on out of memory. */
int  td5_roam_shops_fuel(TD5_RoamShopSource *s, TG_TileCtx *c, int (*in_bld)(void *u, double x, double z), void *u,
                         int (*site_fp)(void *u, int site), TD5_RoamShopStats *st);

/* [roam PE] once per tile, before the building pass: the planned canopies are this tile's only */
void td5_roam_shops_tile_begin(TD5_RoamShopSource *s);
/* [roam PE] TD5RE_ROAM_SHOP_CANOPY_FP (default 1): 1 when this footprint IS a fuel site's forecourt canopy (joined to the
 * site, building=roof 40-1000 m2 or a low box < 7 m 80-400 m2); *site = the site. The building pass then calls
 * _canopy_from_fp instead of the building core; 1 = planned (draw no building), 0 = keep the building. */
int  td5_roam_shops_canopy_fp(TD5_RoamShopSource *s, double id, const TD5_GeoBuilding *gb, int *site);
int  td5_roam_shops_canopy_from_fp(TD5_RoamShopSource *s, TG_TileCtx *c, int site, const TD5_GeoBuilding *gb,
                                   const double *rx, const double *rz, int n, TD5_RoamShopStats *st);
/* [roam PE] TD5RE_ROAM_SHOP_ABUT / _INFILL_GUARD (default 1): the knobs the scenery reads for its side of the rules */
int  td5_roam_shops_abut(const TD5_RoamShopSource *s);
int  td5_roam_shops_infill_guard(const TD5_RoamShopSource *s);
int  td5_roam_shops_canopy_fp_on(const TD5_RoamShopSource *s);
int  td5_roam_shops_fuel_flush(TD5_RoamShopSource *s, TG_TileCtx *c, TD5_RoamShopStats *st);

/* [roam PD] 1 when the place-frame point stands on a fuel site: inside its forecourt ring, or within 15 m of a site
 * POIS.JSON maps as a point only. The rule street trees keep off forecourts. */
int  td5_roam_shops_fuel_at(const TD5_RoamShopSource *s, double x, double z);

#endif /* TD5_ROAM_SHOPS_H */
