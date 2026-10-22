/**
 * td5_roam_plaza.h -- GEO FREE ROAM: PLAZA FURNITURE AND MONUMENTS of one tile (PORT-ONLY, roam LD).
 *
 * SOURCE. <place>/PLAZA_FURNITURE.JSON, written by re/tools/geo_plaza_parks.py (one Overpass request:
 * benches, fountains, memorials / monuments / statues, artworks, playgrounds, bandstands, picnic tables,
 * fitness stations, flagpoles, obelisks). Source frame in the file, game frame here (td5_geo_frame.h).
 * Parsed LAZILY on the first emit, i.e. on the building thread after it took the main thread's FP
 * environment (the td5_roam_rails.c lesson: strtod at open() on a fresh worker moved last bits).
 *
 * WHAT IS DRAWN (simple meshes, VISUAL only, no collision, Decision "roads-only invisible walls"):
 *   statue / monument / named or bust / sculpture memorial / statue artwork:
 *                stone plinth + cornice + the shipped bronze figure billboard (k_prop_pages[PP_MONUMENT]);
 *                a monument, or a name that starts "Monumento", stands on the tall plinth
 *   stone memorial: a low rough block (the Piedra Fundacional)
 *   other memorial / artwork: a stone stele
 *   fountain:    octagonal stone basin + water (TD5_TG_PAGE_WATER) + a centre column and bowl; the basin
 *                radius from the mapped polygon when the fountain is a way
 *   bench:       seat + backrest + two legs, parallel to the nearest street
 *   playground:  a swing frame and a slide tower
 *   flagpole:    a 12 m white pole (height= when tagged) and its flag
 * LEFT OUT (the round L rule: tiny counts nobody notices while driving): fitness stations (11 in the
 * casco, low bars), picnic tables and bandstands (0 mapped), memorial plaques, memorial trees, memorial
 * buildings (a building), obelisks (0 mapped). Counted in skip_kind.
 *
 * PLACEMENT. An item is drawn by the tile that owns its point. Base: the plaza lawn under it, else the
 * pavement slab, else the drawn ground. An item whose footprint touches the drivable surface is not
 * drawn (drop_road); every face is also audited against the road (guard_hits must stay 0).
 *
 * THREADING. A TD5_RoamPlazaSource is owned by ONE thread (inside the scenery source).
 */
#ifndef TD5_ROAM_PLAZA_H
#define TD5_ROAM_PLAZA_H

#include <stdint.h>

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamPlazaSource TD5_RoamPlazaSource;

/* One tile's census. Counters are for the items the tile OWNS. */
typedef struct {
    int items;             /* owned items of a drawn kind                                   */
    int statues, monuments, memorials, fountains, benches, playgrounds, flagpoles;
    int named;             /* drawn items that carry a name (the landmarks)                 */
    int skip_kind;         /* owned items of a kind left out (see the header)               */
    int drop_road;         /* footprint on the drivable surface: not drawn                  */
    int meshes;
    int guard_hits;        /* emitted faces on the drivable surface: must be 0              */
} TD5_RoamPlazaStats;

int  td5_roam_plaza_enabled(void);    /* TD5RE_ROAM_PLAZA_FURN (default 1; 0 = the round J scenery) */
TD5_RoamPlazaSource *td5_roam_plaza_open(const char *place);   /* NULL: knob off / no file */
void td5_roam_plaza_close(TD5_RoamPlazaSource *p);
/* Append the plaza furniture the ctx's tile owns to its sub-block buffers. 1 on success, 0 on OOM. */
int  td5_roam_plaza_emit(TD5_RoamPlazaSource *p, TG_TileCtx *c, TD5_RoamPlazaStats *st);

#endif /* TD5_ROAM_PLAZA_H */
