/**
 * td5_roam_cut.h -- GEO FREE ROAM: CUTTINGS, the walls of a road that runs under the ground (PORT-ONLY, roam NC).
 *
 * "underpass looks wrong, there's no wall within the underpass underneath" (Mariano, 2026-10-22, Avenida 32
 * under Avenida 1 and the railway). A bore (tunnel=yes, ROAM_EF_BORE) and its approaches are lowered under the
 * road above (td5_roam_tile.c BORE CHAIN), and a road under a deck can sit in a cut of the raster. Before NC:
 *   - the renderer dropped every 16 m ground cell round a bore span (3 x 3 cells from the tile's OWN spans):
 *     a 48 m pit with nothing between the road edge and the ground round it (void), and a bore near a tile
 *     border stayed under the neighbour's ground;
 *   - on the approaches the G5 lowering brought the lattice down to the road: a 16 m grass bank, no wall.
 *
 * A CUTTING SPAN is a span of a bore, or of an edge flagged ROAM_EF_UNDER (td5_roam_tile.c edge_under: a bore
 * approach, layer < 0, a way a deck crosses over) where the raster at one of its four kerb corners stands more
 * than TD5_ROAM_CUT_MIN over the road. A pure function of the span and the raster, so every tile that reads the
 * span (its own, the 3x3 of its neighbours) agrees. For a cutting span:
 *   GROUND   the G5 lowering and the verge skip it (td5_roam_groundfix.c), and every ground cell within one cell
 *            of its centre is a HOLE (td5_roam_cut_holes, from the 3x3 ROAD tiles). The renderer refills a hole
 *            cell with 2 m sub-cells on the cell's own surface (td5_roam_render.c), keeping the ones with no
 *            corner inside a cutting BAND (the span quad widened past the wall's back): the open cut stays open.
 *   WALLS    along each kerb of an owned cutting span (td5_roam_cut_emit, scenery tail): a concrete shoulder from
 *            the kerb to the wall foot, the retaining wall TD5_ROAM_CUT_OFF past the kerb from under the road
 *            to the drawn ground plus a parapet, its crown and back, and a coping apron out over the sub-cell
 *            staircase. Under a deck (a road over the wall line) the wall closes on the deck's underside, with
 *            no parapet or apron: those are the abutments. Collision is the roads-only kerb wall (the wall face
 *            stands 0.6 m past the kerb, past the 0.35 m body overhang), as for round IE's barriers.
 *   PAVEMENT no sidewalk on a cutting span (the wall stands where it would).
 * TD5RE_ROAM_CUT=0 = the old behaviour (bore holes from the tile's own spans, no fill, no walls, sidewalks).
 * Census: TD5RE_ROAM_CUT_CENSUS=1 with --RoamBuildScenery (td5_roam_cut_emit: kerb metres per class + one
 * ROAM_CUT_CEN log line per open piece; the knob off measures the old look on the same spans).
 */
#ifndef TD5_ROAM_CUT_H
#define TD5_ROAM_CUT_H

#include <stdint.h>
#include "td5_roam_tile.h"
#include "td5_roam_height.h"

#define TD5_ROAM_CUT_MIN   (1.0 * 430.0)   /* raster over a kerb corner that makes a span a cutting      */
#define TD5_ROAM_CUT_OFF   (0.60 * 430.0)  /* kerb -> wall face (shoulder)                               */
#define TD5_ROAM_CUT_T     (0.30 * 430.0)  /* wall thickness (crown)                                     */
#define TD5_ROAM_CUT_BAND  (TD5_ROAM_CUT_OFF + TD5_ROAM_CUT_T + 0.05 * 430.0)   /* band past the kerb   */
#define TD5_ROAM_CUT_SUB   8               /* fill sub-cells per ground cell side (2 m at 16 m cells)    */

typedef struct {
    int    spans, pieces, deck_pieces, apron_cut, on_road, low_skip, pv_skip;
    double walls_m, parapet_m, apron_m, max_h_m;      /* wall length, of it with a parapet / an apron; tallest face */
    double verge_m;                                   /* no wall needed: the band paved by a verge strip      */
    double cen_m[2][6];                               /* census (TD5RE_ROAM_CUT_CENSUS=1): [casco][WALL, WALL_VOID,
                                                       * VOID, BANK, SHALLOW, VERGE] kerb metres (td5_roam_cut.c) */
} TD5_RoamCutStats;

/* TD5RE_ROAM_CUT (default 1) */
int  td5_roam_cut_on(void);

/* cut[si] = 1 for the cutting spans of tile t (bores always; with the knob off, bores only). Raster from hc
 * (place frame); a corner the window does not cover reads as no raster. Returns the count. */
int  td5_roam_cut_spans(const RoamTile *t, TD5_RoamHeight *hc, uint8_t *cut);

/* The hole cells of tile (tx, tz) (G cells per side): hole[j * G + i] = 1. Knob on: every cell within one cell
 * of a cutting span centre of the 3x3 ROAD tiles nb (nb[4] = the tile). Knob off: the old rule, the bore spans
 * of nb[4] only. */
void td5_roam_cut_holes(const RoamGrid *g, const RoamTile *const nb[9], TD5_RoamHeight *hc, int tx, int tz, int G,
                        uint8_t *hole);

/* The cutting bands round a place-frame rectangle (the spans of the 3x3 whose band meets it). */
typedef struct { int n, cap; double *q; } TD5_RoamCutBands;     /* q: n * 12 = 4 corners x, z + bbox x0 z0 x1 z1 */
int  td5_roam_cut_bands(TD5_RoamCutBands *b, const RoamTile *const nb[9], TD5_RoamHeight *hc,
                        double x0, double z0, double x1, double z1);
int  td5_roam_cut_inside(const TD5_RoamCutBands *b, double x, double z);
void td5_roam_cut_bands_free(TD5_RoamCutBands *b);

struct TG_TileCtx;
/* the walls of the tile being built (scenery tail; casco = the tile is in the casco, census split). 0 on out of memory. */
int  td5_roam_cut_emit(struct TG_TileCtx *c, TD5_RoamCutStats *st, int casco);


#endif
