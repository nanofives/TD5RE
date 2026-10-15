/**
 * td5_roam_junction.h -- GEO FREE ROAM: junction CLUSTERS and their paved PATCH
 * (PORT-ONLY, F1 task T3). See docs/plans/GEO_FREE_ROAM_DESIGN.md 1.3 and 2.5
 * rule 4.
 *
 * CLUSTERING (deterministic, local).
 *   1. Candidates: JUNCTION, TAPER and BEND nodes (td5_roam_world.h).
 *   2. Union two candidates joined by an edge shorter than merge_len (30 m), or
 *      whose discs r = 0.5 * max arm width + 3 m overlap and that share a layer.
 *   3. A union that would make the cluster wider than cap_diameter (60 m, max
 *      member-to-member distance) is refused. Pairs are processed in sorted
 *      (key, key) order, so the refusals are reproducible.
 *   4. Edges with both ends in one cluster that stay inside it are INTERNAL:
 *      absorbed into the patch, no ribbon.
 *   5. Cluster key = smallest member node key. T5 picks the owner tile from
 *      (cx, cz), the centroid of the member nodes.
 *
 * THE PATCH. Arms sorted CCW round the centroid by the direction from the
 * centroid to the arm's point at arm_dir_len along its edge. For each pair of
 * neighbouring arms (i, i+1) the corner C_i = intersection of arm i's LEFT kerb
 * line and arm i+1's RIGHT kerb line. The arm's cut t_i reaches past both of its
 * fillet tangent points, clamped to [half width, edge length / 2]. Outline, CCW:
 *   cut row of arm i (right kerb -> left kerb), [kerb to tangent point, fillet
 *   arc of fillet_segs segments, tangent point to kerb], cut row of arm i+1, ...
 * A pair whose kerb lines are within parallel_deg of parallel, or meet further
 * than corner_max, gets NO corner: the outline joins the two cut rows directly
 * (deviation from the design's bevel point, see the module .c).
 * Triangulation: fan from the centroid when every fan triangle is CCW, else ear
 * clipping over the outline. Then every triangle edge longer than tri_edge_max
 * is bisected (conforming, both neighbours split) so triangle_height's int32
 * products cannot overflow (design 1.3 step 7). Cut-row segments are one lane
 * (1500 units), so they are never split and the patch keeps the ribbon's
 * vertices exactly.
 *
 * HEIGHTS. A patch vertex's y is a linear recipe, not a number (heights are T4's):
 *   y = sum_k w[k] * Y(ref[k]),  Y(TD5_ROAM_HREF_CENTRE) = cluster height H_c,
 *   Y(i >= 0) = the cut-row height of arm i (rows are laterally flat).
 * Cut-row vertices are pure Y(i); fillet vertices interpolate the two cut rows
 * they join; the centroid is H_c; a split midpoint averages its two ends. So every
 * triangle is planar and meets every ribbon on shared vertices.
 *
 * SWEPT WIDTH (2.5 rule 4). Each turning pair of arms (corner present) is checked
 * with the reference body: need = sqrt((R + W)^2 + Lf^2) - R, R = fillet radius
 * (the inner kerb), against the narrower arm's carriageway. A failing corner gets
 * its fillet radius grown (up to what the cut limit allows); still failing ->
 * ROAM_SWEPT_FAIL in the log with the cluster key and arm pair.
 */
#ifndef TD5_ROAM_JUNCTION_H
#define TD5_ROAM_JUNCTION_H

#include <stdint.h>
#include "td5_roam_world.h"

#define TD5_ROAM_HREF_CENTRE  (-1)   /* the cluster height H_c                */
#define TD5_ROAM_HREF_NONE    (-2)   /* unused recipe slot                    */
/* Terms per recipe. A fan vertex needs at most 3 (centre + two arms); 4 leaves
 * room for split points of ear-clip triangles, and an overflow is counted. */
#define TD5_ROAM_HREF_TERMS   4

/* Cluster flags. */
#define TD5_ROAM_CL_FAN        0x01  /* fan from the centroid                 */
#define TD5_ROAM_CL_EARCLIP    0x02  /* ear-clip fallback                     */
#define TD5_ROAM_CL_FAIL       0x04  /* no valid triangulation: no triangles  */
#define TD5_ROAM_CL_SWEPT_FAIL 0x08  /* a corner failed the swept-width check */
#define TD5_ROAM_CL_TAPER      0x10  /* 2-arm, lanes or layer differ          */
#define TD5_ROAM_CL_BEND       0x20  /* 2-arm, sharp deflection               */
#define TD5_ROAM_CL_CLAMPED    0x40  /* an arm cut hit half its edge          */

/* Outline vertex kinds. */
#define TD5_ROAM_PV_CUT     1   /* a cut-row lane boundary of an arm (shared)  */
#define TD5_ROAM_PV_TANGENT 2   /* fillet tangent point on a kerb line         */
#define TD5_ROAM_PV_ARC     3   /* interior fillet arc point                   */
#define TD5_ROAM_PV_CENTRE  4   /* the centroid (fan only)                     */
#define TD5_ROAM_PV_SPLIT   5   /* midpoint from the 20000-unit split          */

/* Boundary segment kinds. */
#define TD5_ROAM_OL_KERB 1      /* wall: kerb line or fillet                   */
#define TD5_ROAM_OL_CUT  2      /* interior: shared with the arm's ribbon      */

typedef struct TD5_RoamArm {
    int     edge, end;          /* the edge end that leaves the cluster        */
    int     node;               /* member node the arm leaves from             */
    double  dir_x, dir_z;       /* outward unit direction (first 15 m)         */
    double  ang;                /* sort angle round the centroid, radians      */
    double  half_w;
    double  t;                  /* cut distance from `node` along the edge     */
    double  kerb_r;             /* fillet radius used on this arm's LEFT corner */
    int     corner;             /* 1 when the left corner (i, i+1) is a fillet */
} TD5_RoamArm;

typedef struct TD5_RoamPatchVert {
    double  x, z;
    int32_t xq, zq;             /* td5_roam_quant, place frame                 */
    int     kind;               /* TD5_ROAM_PV_*                               */
    int     ref[TD5_ROAM_HREF_TERMS]; /* height recipe: TD5_ROAM_HREF_* or the
                                       * LOCAL arm index (0..narm-1)            */
    double  w[TD5_ROAM_HREF_TERMS];
} TD5_RoamPatchVert;

typedef struct TD5_RoamPatchTri {
    int v[3];                   /* indices into g->pvert[] (global), CCW       */
} TD5_RoamPatchTri;

typedef struct TD5_RoamOutline {
    int v[2];                   /* g->pvert[] indices, CCW order               */
    int kind;                   /* TD5_ROAM_OL_*                               */
    int arm;                    /* arm (global g->arm[] index) for CUT, else -1 */
} TD5_RoamOutline;

typedef struct TD5_RoamCluster {
    int64_t key;                /* smallest member node key                    */
    double  cx, cz;             /* centroid of the member nodes                */
    double  diameter;           /* max member-to-member distance               */
    int     node_first, nnode;  /* g->cluster_node[] slice                     */
    int     arm_first, narm;    /* g->arm[] slice, CCW                         */
    int     vert_first, nvert;  /* g->pvert[] slice (outline first, then the
                                 * centroid and split points)                   */
    int     nout;               /* outline vertex count                        */
    int     tri_first, ntri;    /* g->ptri[] slice                             */
    int     ol_first, nol;      /* g->outline[] slice                          */
    int     layer;              /* the arms' lowest layer                      */
    int     flags;              /* TD5_ROAM_CL_*                               */
    double  min_swept_margin;   /* worst (carriageway - need) over corners     */
} TD5_RoamCluster;

/* Called by td5_roam_graph_build after nodes and edges exist: clusters the
 * candidates, builds arms, cut rows (writes the CLUSTER / INTERNAL edge ends),
 * outlines, triangles and the swept-width check. Returns 0 on out of memory. */
int  td5_roam_junction_build(TD5_RoamGraph *g);

/* Evaluate a patch vertex height from its recipe. h_c = the cluster height,
 * arm_y[i] = cut-row height of the cluster's arm i (local index). Pure. */
double td5_roam_patch_vert_y(const TD5_RoamPatchVert *v, double h_c,
                             const double *arm_y);

/* Lookup by key. -1 when absent. */
int  td5_roam_junction_find(const TD5_RoamGraph *g, int64_t key);

#ifndef TD5RE_RELEASE
/* DEV ONLY. TD5RE_ROAM_JUNC_TEST=1: builds the graph over the La Plata casco,
 * writes <log dir>/roam_junc_census.txt and <log dir>/roam_clusters.json (every
 * casco cluster polygon), then returns 1 (the caller exits). 0 when not asked. */
int  td5_roam_junction_dev_census(void);
#endif

#endif /* TD5_ROAM_JUNCTION_H */
