/**
 * td5_roam_partido.h -- GEO FREE ROAM: the whole PLACE, not just the casco (PORT-ONLY, F9b).
 * See docs/plans/GEO_FREE_ROAM.md (F9 row) and GEO_REGION_EXTENT.md (the tiled place).
 *
 * WHAT IT IS. Two place-wide facts the roam modules need once the car can leave the casco:
 *   BOUNDARY  the place's administrative ring (PLACE.JSON boundary.rings, lat/lon) in the place
 *             frame. A lattice tile is STREAMABLE when its centre lies within the tile's half
 *             diagonal + the build halo of that ring (td5_roam_partido_tile_ok). A place with no
 *             boundary (an old square place) treats every tile as streamable.
 *   STREETS   a LIGHT street layer read straight from the cache ROADS tiles (14 MB of JSON for the
 *             whole partido): every drivable way's polyline + class, bucketed on a 1 km grid. Not a
 *             graph: it draws the map overlay where no ROAMTILE has been built yet, answers "is
 *             there a street near this point" (the FREE ROAM button) and moves a spawn click that
 *             lies in a field onto the nearest street. Loaded on a thread at session start
 *             (td5_roam_partido_streets_begin_async) or synchronously (_ensure).
 *
 * FRAME. Place frame units (430 per metre), doubles / int32, the frame of the cache.
 *
 * STATE. One file-scope struct (the place's boundary + the street layer). The street layer is
 * written by one loader thread and published with a release fence; readers check _ready first.
 */
#ifndef TD5_ROAM_PARTIDO_H
#define TD5_ROAM_PARTIDO_H

#include <stdint.h>
#include "td5_roam_tile.h"

/* Load the boundary of `place` (idempotent per place, main thread). 1 when the place has one. */
int    td5_roam_partido_load(const char *place);
int    td5_roam_partido_has_boundary(void);
/* Inside the boundary (any ring). 1 when no boundary is loaded (nothing to test against). */
int    td5_roam_partido_contains(double px, double pz);
/* Metres from the point to the boundary ring, 0 inside. 0 when no boundary is loaded. */
double td5_roam_partido_dist_m(double px, double pz);
/* Streamable tile: its centre within (half diagonal + ROAM_HALO_M) of the boundary. */
int    td5_roam_partido_tile_ok(const RoamGrid *g, int tx, int tz);
/* The boundary rings for drawing: ring r as n place-frame (x, z) double pairs. */
int    td5_roam_partido_ring_count(void);
const double *td5_roam_partido_ring(int r, int *n);

/* ---- the light street layer --------------------------------------------------------------- */
void   td5_roam_partido_streets_begin_async(const char *place);
int    td5_roam_partido_streets_ensure(const char *place);   /* blocks until loaded; 1 = ready */
int    td5_roam_partido_streets_ready(void);
/* Every street whose bbox meets the window. xz = int32 place-frame pairs. Returns the count. */
typedef void (*TD5_PartidoStreetCb)(void *user, int klass, int lanes, const int32_t *xz, int npt);
int    td5_roam_partido_streets_query(double minx, double minz, double maxx, double maxz,
                                      TD5_PartidoStreetCb cb, void *user);
/* Nearest street point within max_m metres of (px, pz). 1 on success. */
int    td5_roam_partido_nearest_street(double px, double pz, double max_m, double *sx, double *sz,
                                       double *dist_m);
void   td5_roam_partido_streets_stats(int *ways, int *points, double *ms);

#endif /* TD5_ROAM_PARTIDO_H */
