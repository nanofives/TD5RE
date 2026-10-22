/**
 * td5_roam_greenhouse.h -- GEO FREE ROAM: the horticulture belt's greenhouses (PORT-ONLY, roam PC).
 *
 * WHAT. La Plata's horticulture belt is 306 OSM `landuse=greenhouse_horticulture` areas (37.7 km2) holding
 * 10,123 traced footprints (Overture: Google / Microsoft, plus 775 OSM), median 390 m2, 4.4 m tall: the plastic
 * tunnels themselves. Before round P they were extruded like any building (facade pages, a flat roof). A footprint
 * becomes a GREENHOUSE here when
 *   - OSM tags it building=greenhouse (148 in the partido), or
 *   - its centroid lies in a greenhouse_horticulture area, it is at least TD5_ROAM_GH_MIN_M2 and at most
 *     TD5_ROAM_GH_MAX_H tall (a farmhouse or a packing shed keeps its building),
 * and its minimum-area rectangle (over the ring's own edge directions) holds at least TD5_ROAM_GH_FILL of it
 * (94 % of the candidates do; an L-shaped one keeps its building).
 *
 * SHAPE. In that rectangle: n = round(short side / 8 m) arched spans side by side (a multi-tunnel block), each a
 * half-ellipse of TD5_ROAM_GH_SEG strips from the eave to the ridge and down, gable ends filled to the ground, the
 * two long side walls to the eave. The ridge is the footprint's measured height (3..8 m, else 4.2 m), the rise is at
 * most 0.4 of a span. White plastic: the smooth cast-concrete page in a pale blue-white, each roof strip shaded by
 * its slope against a fixed sun so the arches read.
 *
 * GUARD. The rectangle is tested against the drivable surface (roam_ctx_on_road); one that touches it is left to the
 * building core, which has its own clearance. Visual only, no collision.
 *
 * ORDER. Called from the building pass for each kept footprint (id order); the quads are batched per (sub-block)
 * and flushed after the pass, so the bytes depend only on the footprints. TD5RE_ROAM_GREENHOUSE=0 = the old bytes.
 */
#ifndef TD5_ROAM_GREENHOUSE_H
#define TD5_ROAM_GREENHOUSE_H

#include <stdint.h>

#define TD5_ROAM_GH_MIN_M2   100.0
#define TD5_ROAM_GH_MAX_H    8.0
#define TD5_ROAM_GH_FILL     0.80
#define TD5_ROAM_GH_SPAN_M   8.0
#define TD5_ROAM_GH_SEG      6

typedef struct TG_TileCtx TG_TileCtx;
typedef struct TD5_RoamGh TD5_RoamGh;

typedef struct {
    int cand_osm, cand_area;      /* footprints judged: OSM building=greenhouse, any footprint in a horticulture area */
    int drawn, spans;             /* drawn as arched greenhouses, their spans                                          */
    int small, tall, irregular, road;   /* left to the building core: under MIN_M2, over MAX_H, not a rectangle, rect on a road */
    double m2;                    /* rectangle area drawn                                                              */
    int quads, meshes;
} TD5_RoamGhStats;

int  td5_roam_gh_enabled(void);                 /* TD5RE_ROAM_GREENHOUSE (default 1) */
TD5_RoamGh *td5_roam_gh_begin(void);            /* NULL on out of memory */
/* One footprint (tile-local ring, n points, already cleared off the road by the building pass). osm_gh: OSM
 * building=greenhouse; in_area: centroid in a greenhouse_horticulture area; height in metres (0 = unknown).
 * 1 = drawn (the caller skips the building core), 0 = not a greenhouse here, -1 out of memory. */
int  td5_roam_gh_try(TD5_RoamGh *g, TG_TileCtx *c, const double *rx, const double *rz, int n,
                     double area_m2, double height_m, int osm_gh, int in_area, TD5_RoamGhStats *st);
int  td5_roam_gh_flush(TD5_RoamGh *g, TG_TileCtx *c, TD5_RoamGhStats *st);   /* 0 on out of memory */
void td5_roam_gh_end(TD5_RoamGh *g);

#endif /* TD5_ROAM_GREENHOUSE_H */
