/**
 * td5_roam_pavecensus.h -- GEO FREE ROAM G4b: pavement vs ground-as-drawn census (PORT-ONLY, DEV-ONLY).
 * See td5_roam_pavecensus.c.
 */
#ifndef TD5_ROAM_PAVECENSUS_H
#define TD5_ROAM_PAVECENSUS_H

#include "td5_roam_tile.h"
#include "td5_roam_height.h"
#include "td5_roam_scenery.h"

#ifndef TD5RE_RELEASE
/* ground as the renderer draws it at a place-frame point (units); 0 = no ground there */
typedef int (*TD5_RoamGroundFn)(void *u, double x, double z, double *y);

/* Builds the scenery of each tile and compares every pavement vertex with the drawn ground. proj = lat0,
 * m_per_deg_lat, lon0, m_per_deg_lon (or NULL). Writes <log>/roam_pavement_census.{txt,csv}. Returns 1. */
int td5_roam_pave_census(const RoamTile *tiles, int ntile, TD5_RoamScnSource *scn, TD5_RoamHeight *hc,
                         TD5_RoamGroundFn ground, void *gu, double drop_units, const double proj[4], const char *outprefix);
#endif

#endif
