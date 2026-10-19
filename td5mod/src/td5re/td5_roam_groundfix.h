/**
 * td5_roam_groundfix.h -- GEO FREE ROAM: the ground held at or under the road (PORT-ONLY, G4 + G4b).
 *
 * G4 lowered the drawn ground (td5_roam_render.c) at every 16 m lattice point to the lowest road
 * height of its four cells. [G5] On a steep road that minimum is metres under the road edge level with
 * the point, so each point now comes down only as far as the road triangles next to it need
 * (td5_roam_gfix_lower). That arithmetic lives here so the SCENERY layer (pavement, footpaths,
 * lawns, trees, buildings) lays its things on the SAME ground the renderer draws: in a cut the
 * pavement then follows the road edge instead of standing on the old raster height.
 *
 * Everything is keyed on GLOBAL lattice cells (I, J) = floor(u / cell), so two tiles that share a
 * border point compute the same bits.
 */
#ifndef TD5_ROAM_GROUNDFIX_H
#define TD5_ROAM_GROUNDFIX_H

#include "td5_roam_tile.h"
#include "td5_roam_height.h"

/* TD5RE_ROAM_RENDER_CELL_M (default 16, 8..64, must divide 64) */
int td5_roam_gfix_cell_m(void);
/* 0 with TD5RE_ROAM_GROUND_NOFIX=1 */
int td5_roam_gfix_enabled(void);
/* [G5] 1 with TD5RE_ROAM_GROUND_G4=1: the G4 cell clamp (lowest road height of the 4 cells) instead of the
 * per-triangle lowering (before / after checks) */
int td5_roam_gfix_g4(void);

/* Lowest road height inside each global cell I0..I0+N-1, J0..J0+N-1 (cm[(J-J0)*N + (I-I0)], 1e300 = no road),
 * from the primitives of the 3x3 ROAD tiles nb[(dz+1)*3 + (dx+1)] (NULL entries skipped; tunnel bores left out). */
void td5_roam_gfix_cells(const RoamGrid *g, const RoamTile *const nb[9], int I0, int J0, int N, double cs, double *cm);

/* [G5] How far each lattice point lo..lo+W-1 (tile-relative i and j of tile tx, tz; G cells per side) must come
 * down under the raster `hc` so the drawn ground (the renderer's fan triangles) stays at or under every drivable
 * triangle of the 3x3 ROAD tiles: low[(j-lo)*W + (i-lo)] >= 0. Lowers only where the raster stands over the
 * road, and mostly the corners next to it. hc must cover the lattice one cell round the window. */
void td5_roam_gfix_lower(const RoamGrid *g, const RoamTile *const nb[9], TD5_RoamHeight *hc, int tx, int tz, int G,
                         int lo, int W, double *low);

/* the place-frame position of lattice point (i, j) of tile (tx, tz) with G cells per side (i, j may lie outside 0..G) */
void td5_roam_gfix_point(const RoamGrid *g, int tx, int tz, int i, int j, int G, double *px, double *pz);

/* The clamped ground lattice round one tile, for the scenery layer. */
typedef struct {
    RoamGrid grid;
    int      G, tx, tz;
    int      lo, np;            /* lattice points lo .. lo+np-1 (tile-relative i, j) */
    double   cs;
    double  *h;                 /* np*np, how far the clamp lowers each lattice point (>= 0; 0 = untouched) */
} TD5_RoamGfix;

/* Build from the 3x3 ROAD tiles and the raster `hc` (prefetched by the caller). 1 on success. */
int  td5_roam_gfix_build(TD5_RoamGfix *f, const RoamGrid *g, const RoamTile *const nb[9], int tx, int tz, TD5_RoamHeight *hc);
void td5_roam_gfix_free(TD5_RoamGfix *f);
/* How far the drawn ground is lowered at a place-frame point (the renderer's fan triangle over the lowering
 * of the four lattice points; 0 where the clamp does not act). 0 return = outside the lattice (lowering 0). */
int  td5_roam_gfix_lowering(const TD5_RoamGfix *f, double x, double z, double *d);

#endif
