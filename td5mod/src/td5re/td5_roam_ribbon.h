/**
 * td5_roam_ribbon.h -- GEO FREE ROAM: street ribbons + heights (PORT-ONLY,
 * F1 task T4). See docs/plans/GEO_FREE_ROAM_DESIGN.md sections 1.4, 1.5, 1.6.
 *
 * WHAT IT IS. One street EDGE (a polyline of one OSM way between two graph
 * nodes) in, one RIBBON out: a run of type-1 TD5_StripSpan quads over
 * TD5_StripVertex rows (int16 offsets from a per-block int32 origin), the exact
 * record the strip's probe_span_lane_height / triangle_height read. Heights are
 * edge-local: raster profile, smoothed, end-blended toward the cluster height,
 * lifted for bridges / bores, then grade-capped by roam_grade_solve (a copy of
 * the three-pass rule of tg_road_solve, td5_tg_road.c:374).
 *
 * PURE. The build of one edge reads only its own input struct and the height
 * context. No file-scope state, no RNG, no clocks, no pointer or hash order.
 * Row 0 is at the end with the smaller node key (reversing the input and
 * swapping the ends gives byte-identical output), a coordinate is quantised
 * with one explicit rounding (floor(v + 0.5)) BEFORE any tile offset, so two
 * tiles that share a row get the same bytes.
 *
 * FRAME. PLACE frame world units (the cache's own, 430 per metre on La Plata).
 * Span origins and meta midpoints are PLACE-frame int32; the tile assembler
 * (T5) subtracts the tile origin from origin_* and the meta. Vertex offsets are
 * independent of any tile origin. Span vertex indices and link_next / link_prev
 * are RIBBON-LOCAL (0-based); the assembler relocates them into its tile table.
 *
 * T3 INTERFACE. T3 (junction patches) decides where each edge end is cut. Pass
 * its cut distances as RoamEdgeIn.cut_a / cut_b (distance along the centreline
 * from each end, units): row 0 / the last row are then T3's cut rows. 0 = no
 * cut (the ribbon runs to the node). `pin_a` / `pin_b` are the cluster heights.
 */
#ifndef TD5_ROAM_RIBBON_H
#define TD5_ROAM_RIBBON_H

#include <stdint.h>
#include "td5_types.h"
#include "td5_roam_height.h"

/* ---- constants (all copied from the strip, see the design doc) ---- */
#define ROAM_RIB_STEP        1500.0    /* TD5_TG_SPAN_LENGTH: row spacing, units       */
#define ROAM_RIB_LANE_W      1500.0    /* TD5_TG_LANE_WIDTH: one lane, units           */
#define ROAM_RIB_BLOCK       12        /* spans per origin block (strip max is 20)     */
#define ROAM_RIB_CHUNK_ROWS  280       /* 1 km: an edge is split at row multiples      */
#define ROAM_RIB_CHUNK_TAIL  40        /* a last chunk shorter than this merges back   */
#define ROAM_RIB_SMOOTH      5         /* triangular smoothing, +-rows (35 m)          */
#define ROAM_RIB_BLEND_ROWS  8         /* end blend toward the cluster height          */
#define ROAM_GRADE_CAP       0.138     /* s_max_grade 0.12 x R21 headroom 1.15         */
#define ROAM_GRADE_ABSMAX    0.20      /* TG_ROAD_GRADE_ABSMAX, the ramp-short fallback*/
#define ROAM_CURV_CAP        0.045     /* TD5RE_R22_SMOOTH default 45 / 1000           */
#define ROAM_XSEP_LIFT       3080.0    /* GR_XSEP_LIFT_UNITS: 7.16 m deck over road    */
#define ROAM_XSEP_RAMP_FRAC  0.5       /* GR_XSEP_RAMP_FRAC: ramp uses half the cap    */
#define ROAM_RIB_DECK_FLAT   6.0       /* rows of flat deck each side of a crossing    */

/* RoamRibbon.status */
#define ROAM_RIB_OK         0
#define ROAM_RIB_ABSORBED   1   /* cuts consume the whole edge: no quads            */
#define ROAM_RIB_BAD_INPUT  2   /* <2 points, no height context, out of memory      */
#define ROAM_RIB_HEIGHT     3   /* a ground sample was not covered by any window    */

/* RoamRibbon.flags */
#define ROAM_RIBF_RAMP_SHORT  0x01u   /* bridge needed the 0.20 grade fallback       */
#define ROAM_RIBF_RAMP_FAIL   0x02u   /* even 0.20 did not reach the deck height     */
#define ROAM_RIBF_I16         0x04u   /* a vertex offset left int16 (data is wrong)  */
#define ROAM_RIBF_PIN_MISS    0x08u   /* pins further apart than the cap allows: the
                                       * cap of that chunk was raised to join them   */
#define ROAM_RIBF_NOCURV      0x100u  /* curvature clamp dropped (it broke a cap)    */
#define ROAM_RIBF_REPAIR      0x200u  /* Lipschitz repair of the three-pass result   */
#define ROAM_RIBF_LOOP        0x10u   /* both ends are the same node                 */
#define ROAM_RIBF_BRIDGE      0x20u
#define ROAM_RIBF_BORE        0x40u
#define ROAM_RIBF_CHUNKED     0x80u   /* split at synthetic joins (edge > 1 km)      */

/* A grade-separated crossing under this edge (design 1.5): the deck must clear
 * y_under by ROAM_XSEP_LIFT at the PLACE-frame point (x, z). y_under is the
 * solved height of the lower edge there (see roam_ribbon_y_at). */
typedef struct { double x, z, y_under; } RoamCross;

typedef struct {
    uint64_t       key;           /* (way_id << 16) | piece ordinal                    */
    const double  *px, *pz;       /* centreline, PLACE frame, in way order, n >= 2     */
    int            n;
    int64_t        node_a, node_b;/* node keys of the first / last point               */
    int            lanes;         /* carriageway lanes (td5_roam_lane_count), 1..12    */
    int            surface;       /* TD5_GEO_SURF_*                                    */
    int            layer;         /* OSM layer                                         */
    int            bridge;        /* OSM bridge=yes                                    */
    int            tunnel;        /* 0 none, 1 BORE (negative lift), 2 passage (grade) */
    double         cut_a, cut_b;  /* T3 cut distance from the first / last point, units*/
    double         pin_a, pin_b;  /* cluster height at the first / last point, units;
                                   * NAN = free (use the smoothed ground)              */
    const RoamCross *cross;       /* crossings below this deck (bridge edges only)     */
    int            ncross;
    /* T3 ADAPTER: the stored end rows (TD5_RoamEdgeEnd.rowq), lanes + 1 entries of
     * place-frame {x, z} each, in the frame of the INPUT direction (index 0 = right
     * of the input direction). rowq_a belongs to the first input point, rowq_b to
     * the last. When set, the ribbon's first / last row use those integers as given
     * (never recomputed from the cut), so ribbon and junction patch share vertices
     * bit for bit. NULL = the row is computed from the centreline. */
    const int32_t (*rowq_a)[2];
    const int32_t (*rowq_b)[2];
} RoamEdgeIn;

/* Per quad, for ownership. Midpoint of the quad's centre segment, PLACE frame. */
typedef struct { int32_t mx, mz; } RoamQuadMeta;

typedef struct {
    uint64_t         key;
    int              status;
    unsigned         flags;
    int              lanes;
    int              nrows;        /* rows 0..nrows-1; nquad = nrows - 1               */
    int              nquad;
    int              nvert;
    TD5_StripSpan   *span;         /* nquad, type 1, ribbon-local indices / links      */
    TD5_StripVertex *vert;         /* nvert                                            */
    RoamQuadMeta    *meta;         /* nquad                                            */
    /* Kept for the dev census and the profile plots: per row, place frame.
     * ground = raw raster profile, target = smoothed + blended + lifted,
     * y = solved (double, before the integer quantisation). cx / cz = centre. */
    double          *cx, *cz, *ground, *target, *y;
    double           y_first, y_last;  /* solved height of row 0 / the last row (the
                                        * cut rows: a patch's arm_y recipe input)    */
    int32_t          yq_first, yq_last;/* the same, quantised                        */
    double           max_grade;    /* max |dy| / segment length over the quads         */
    double           length;       /* arc length of the rows, units                    */
    int              nchunks;
    int              wet_rows;     /* rows whose ground sample was under water         */
} RoamRibbon;

/* Free what roam_ribbon_build allocated. Safe on a zeroed struct. */
void roam_ribbon_free(RoamRibbon *r);

/* Build one edge. `h` is a height context with windows covering the edge
 * (the builder prefetches the edge's box itself when a sample is not covered).
 * Returns the status (also in out->status). `out` is zeroed first. */
int  roam_ribbon_build(const RoamEdgeIn *in, TD5_RoamHeight *h, RoamRibbon *out);

/* The pure grade limiter: a COPY of tg_road_solve's three passes (forward,
 * backward, forward; slope clamp + curvature clamp), over a uniform cap.
 * `t` = target y per row, `y` = result, row 0 is fixed at t[0]. cap_per_row and
 * curv_cap are in y-units per row. The _pin variant also holds the last row at
 * t[n-1] (it is seeded into the backward pass and restored at the end); returns
 * 0 when that pin could not be reached inside the cap (the end segment then
 * exceeds the cap). Plain roam_grade_solve returns 1. */
int  roam_grade_solve(double *y, const double *t, int n, double cap_per_row,
                      double curv_cap);
int  roam_grade_solve_pin(double *y, const double *t, int n, double cap_per_row,
                          double curv_cap);

/* Solved height of a built ribbon at the PLACE-frame point (x, z): linear along
 * the nearest centre segment. Returns 0 when the ribbon has no rows. Used to
 * get y_under for a crossing. */
int  roam_ribbon_y_at(const RoamRibbon *r, double x, double z, double *out_y,
                      double *out_dist);

/* The lane-boundary vertices of row `row` in PLACE-frame integers (nlanes + 1
 * entries, left to right as stored). out must hold ROAM_RIB_MAX_LANES + 1.
 * Returns the vertex count, 0 when row is out of range. T3 reads its cut rows
 * (row 0 and nrows - 1) with this. */
#define ROAM_RIB_MAX_LANES 12
int  roam_ribbon_row_world(const RoamRibbon *r, int row, int32_t (*out)[3]);

/* Cluster height: the mean ground height at `n` PLACE-frame points (design 1.4).
 * Returns 0 on a failed sample. */
int  roam_cluster_height(TD5_RoamHeight *h, const double (*pts)[2], int n,
                         double *out);

/* Explicit rounding for every coordinate that is stored (design 1.4). */
int32_t roam_round(double v);

#ifndef TD5RE_RELEASE
/* DEV ONLY. TD5RE_ROAM_RIBBON_TEST=1: reads the casco window of the La Plata
 * cache, builds every casco edge (a minimal local graph until T3 lands), and
 * prints the census + a hash. Also checks determinism (build twice, reversed
 * edge order, reversed input). Returns 1 when it ran (caller should exit). */
int  td5_roam_ribbon_dev_test(void);
#endif

#endif /* TD5_ROAM_RIBBON_H */
