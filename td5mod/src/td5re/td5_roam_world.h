/**
 * td5_roam_world.h -- GEO FREE ROAM: the street GRAPH of the road world
 * (PORT-ONLY, F1 task T3). See docs/plans/GEO_FREE_ROAM_DESIGN.md 1.2, 1.3, 1.6.
 *
 * WHAT IT IS. A pure builder: cache ways (td5_roam_cache.h) in, a graph out.
 *   - NODE  = a 10-unit-snapped point that is a way end, or that 2+ way vertices
 *             share. Its position is the snap cell centre (kx*10, kz*10), so it is
 *             a function of the key alone and every edge that meets it ends on
 *             the same double.
 *   - EDGE  = a maximal run of ONE way between two nodes. edge_key =
 *             (way_id << 16) | index of the piece's first vertex in the way
 *             (cache point order). Deviation from the design's "piece ordinal":
 *             a running piece count depends on which OTHER ways are loaded, so
 *             it changed keys between a full and a sub-window build; the vertex
 *             index depends on the way alone. Row 0 is at the end with the
 *             SMALLER node key (node_a), and the polyline is stored a -> b.
 *   - JOIN  = a node with exactly two edge ends. Same lanes and a deflection of
 *             at most join_max_defl_deg: the ribbons continue through it on ONE
 *             shared row (TD5_ROAM_END_JOIN). A layer change alone (bridge start)
 *             stays a JOIN. Otherwise it becomes a 2-arm cluster (a TAPER when
 *             the lanes differ, a BEND when it turns too sharply).
 *   - CLUSTER = the paved patch of a junction (td5_roam_junction.h).
 *
 * THE CUT-ROW CONTRACT (what T4, the ribbon builder, consumes).
 *   Every edge has two ends, end[0] at node_a and end[1] at node_b. Each end
 *   carries:
 *     s        arc length from node_a along the a->b polyline to the end's row.
 *              end[0].s is the cut distance from node_a; end[1].s is
 *              (length - cut distance from node_b). When both cuts are clamped to
 *              half the edge, end[0].s == end[1].s bit for bit (one value, stored
 *              twice), and the ribbon has no quads.
 *     row[]    the lanes+1 lane-boundary points of that row, place frame,
 *              row[0] = RIGHT kerb of the a->b direction, row[lanes] = LEFT kerb.
 *     rowq[]   the same points quantised with td5_roam_quant (formed in the
 *              place frame BEFORE any tile origin is subtracted, design 1.4).
 *   The ribbon's first row IS end[0].row and its last row IS end[1].row: take the
 *   points as given, never recompute them from s. Junction patches use the very
 *   same stored points as their arm vertices, so ribbon and patch share vertices
 *   exactly. For a CLUSTER end the row is perpendicular to the edge tangent at s
 *   (td5_roam_edge_row); for a JOIN end it is the mitred row through the node
 *   (shared by both edges, so it is not perpendicular to either); for a DEAD end
 *   it is perpendicular at the node and T4 closes it with an end-cap wall.
 *   The rows between are T4's: resample the polyline between end[0].s and
 *   end[1].s (td5_roam_edge_point gives the polyline point at any s).
 *   Heights are not here: every row is laterally flat (1.4), so a row needs one
 *   y, which T4 computes. A patch vertex's height is a linear recipe over the
 *   cluster height and its arms' cut-row heights (TD5_RoamPatchVert).
 *
 * STATE. None at file scope. A TD5_RoamGraph is owned by its caller; build it,
 * read it, free it. Iteration is over sorted arrays only (ways by id, nodes by
 * key, edges by key), so the result does not depend on load order (1.10).
 *
 * FRAME. Place frame world units (430 per metre), the frame of the cache.
 *
 * BYTE-IDENTITY. Nothing here runs on a synthetic or strip build. The only
 * caller in F1 is the dev census (TD5RE_ROAM_JUNC_TEST=1).
 */
#ifndef TD5_ROAM_WORLD_H
#define TD5_ROAM_WORLD_H

#include <stdint.h>
#include "td5_geo_roads.h"      /* TD5_GEO_ROADS_LANES_MAX, TD5_GEO_RC_* */
#include "td5_roam_cache.h"     /* TD5_RoamWay                           */

#define TD5_ROAM_UPM        430.0   /* world units per metre (td5_geo_route.c:91) */
#define TD5_ROAM_LANE_W     1500.0  /* one lane, TD5_TG_LANE_WIDTH                */
#define TD5_ROAM_LANE_MIN_M 2.75    /* [T10] narrowest lane a divided carriageway may
                                     * take to keep its OSM count (a real urban lane) */
#define TD5_ROAM_ROW_MAX    (TD5_GEO_ROADS_LANES_MAX + 1)

/* The design's explicit rounding (1.4): never the CRT rounding mode. */
static inline int32_t td5_roam_quant(double v)
{
    double f = v + 0.5;
    int32_t i = (int32_t)f;              /* truncates toward zero */
    if ((double)i > f) i--;              /* floor for negatives    */
    return i;
}

/* ------------------------------------------------------------ drivable ------ */

/* Why a way was left out of the graph (TD5_RoamGraph.drop[]). The filter keeps
 * everything the cache writer put in roads[] except these, decided per design
 * 1.2: a bus bay, a demolished road, a service-station area, a race circuit, an
 * area polygon, and the two service kinds that are car parks rather than streets. */
#define TD5_ROAM_DROP_KEEP           0
#define TD5_ROAM_DROP_BUS_STOP       1   /* highway=bus_stop                  */
#define TD5_ROAM_DROP_RAZED          2   /* highway=razed                     */
#define TD5_ROAM_DROP_SERVICES       3   /* highway=services (an area)        */
#define TD5_ROAM_DROP_RACEWAY        4   /* highway=raceway (not a street)    */
#define TD5_ROAM_DROP_AREA           5   /* area=yes                          */
#define TD5_ROAM_DROP_PARKING_AISLE  6   /* service=parking_aisle             */
#define TD5_ROAM_DROP_DRIVE_THROUGH  7   /* service=drive-through             */
#define TD5_ROAM_DROP_NO_ID          8   /* no OSM way id: no edge identity   */
#define TD5_ROAM_DROP_DUP_ID         9   /* way id seen already (first kept)  */
#define TD5_ROAM_DROP_DEGENERATE    10   /* all points in one 10-unit cell    */
#define TD5_ROAM_DROP_COUNT         11

/* ------------------------------------------------------------ the graph ----- */

/* Node kinds. */
#define TD5_ROAM_NODE_END       1   /* one edge end: a dead end                 */
#define TD5_ROAM_NODE_JOIN      2   /* two ends, ribbons continue on one row    */
#define TD5_ROAM_NODE_TAPER     3   /* two ends, lanes differ: patch            */
#define TD5_ROAM_NODE_BEND      4   /* two ends, deflection too sharp: patch    */
#define TD5_ROAM_NODE_JUNCTION  5   /* three or more ends: patch                */

/* Edge end kinds (TD5_RoamEdgeEnd.kind). */
#define TD5_ROAM_END_DEAD     1     /* dead end, T4 closes it with a wall       */
#define TD5_ROAM_END_JOIN     2     /* shared row with the other edge at a JOIN */
#define TD5_ROAM_END_CLUSTER  3     /* cut row of a junction patch arm          */
#define TD5_ROAM_END_INTERNAL 4     /* edge absorbed into a patch: no ribbon    */

typedef struct { double x, z; } TD5_RoamPt;

typedef struct {
    int64_t key;            /* 10-unit node key (td5_roam_node_key)              */
    double  x, z;           /* snap cell centre: (kx * 10, kz * 10)              */
    int     end_first;      /* slice of g->node_end[]: this node's edge ends,    */
    int     nend;           /*   sorted by (edge key, end)                       */
    int     kind;           /* TD5_ROAM_NODE_*                                   */
    int     cluster;        /* index into g->cluster[] or -1                     */
    int     layer_min, layer_max;   /* over the ways that meet here             */
    int     layer_cross;    /* ways of different layers pass THROUGH here: a
                             * snap coincidence, logged ROAM_LAYER_NODE          */
} TD5_RoamNode;

/* One end of an edge, the cut-row contract above. */
typedef struct {
    int        kind;        /* TD5_ROAM_END_*                                    */
    int        node;        /* graph node at this end                            */
    int        cluster;     /* cluster index for CLUSTER / INTERNAL, else -1     */
    int        arm;         /* arm index inside that cluster, -1 otherwise       */
    double     s;           /* arc length from node_a to this end's row          */
    double     cut;         /* cut distance from THIS end's node (0 = at node)   */
    double     dir_x, dir_z;/* unit a->b tangent the row was built on            */
    int        nrow;        /* lanes + 1 (0 for INTERNAL)                        */
    TD5_RoamPt row[TD5_ROAM_ROW_MAX];      /* [0] right kerb, [lanes] left kerb  */
    int32_t    rowq[TD5_ROAM_ROW_MAX][2];  /* td5_roam_quant(row[k].x / .z)      */
    int        signal;      /* a traffic signal within 15 m of this end's node   */
} TD5_RoamEdgeEnd;

typedef struct {
    uint64_t key;           /* (way_id << 16) | first vertex index in the way    */
    int64_t  way_id;
    int      way;           /* cache way index (td5_roam_cache_way)              */
    int      node_a, node_b;/* graph node indices, key(a) <= key(b)              */
    int      pt_first, npt; /* polyline a -> b in g->pt[]; pt[0] / pt[npt-1] are
                             * exactly the node positions                        */
    double   length;        /* polyline length, world units                      */
    /* Attributes (design 1.2), copied from the way. */
    int      klass;         /* TD5_GEO_RC_*                                      */
    int      lanes;         /* td5_roam_lane_count's answer                      */
    double   half_w;        /* lanes * lane_w / 2                                */
    double   lane_w;        /* [T10] lane width: TD5_ROAM_LANE_W, narrower on a
                             * divided carriageway whose median is narrow        */
    int      oneway;        /* 0 two-way, +1 along a->b, -1 against a->b         */
    int      surface;       /* TD5_GEO_SURF_*                                    */
    int      layer;
    int      bridge;
    int      tunnel;        /* 0 none, 1 tunnel=yes (bore), 2 building_passage   */
    int      roundabout;
    int      namek, name_id, maxspeed_kph;
    TD5_RoamEdgeEnd end[2]; /* [0] at node_a, [1] at node_b                      */
} TD5_RoamEdge;

/* One (edge, end) incident on a node. */
typedef struct { int edge; int end; } TD5_RoamEndRef;

struct TD5_RoamCluster;     /* td5_roam_junction.h */
struct TD5_RoamArm;
struct TD5_RoamPatchVert;
struct TD5_RoamPatchTri;
struct TD5_RoamOutline;

/* Builder knobs. td5_roam_graph_params_default fills the design's numbers. */
typedef struct {
    double join_max_defl_deg;   /* JOIN only up to this deflection (30)          */
    double merge_len;           /* JUNC_MERGE_LEN, 30 m                          */
    double cap_diameter;        /* 60 m                                          */
    double disc_pad;            /* +3 m on the half arm width                    */
    double arm_dir_len;         /* 15 m: arm direction from the first 15 m       */
    double parallel_deg;        /* 20: kerb lines nearer parallel -> no corner   */
    double corner_max;          /* 40 m: a corner further out -> no corner       */
    double kerb_r_minor;        /* 3 m, residential and below                    */
    double kerb_r_major;        /* 6 m, primary and above                        */
    int    fillet_segs;         /* 3                                             */
    double tri_edge_max;        /* 20000 units                                   */
    double signal_r;            /* 15 m                                          */
    /* Reference body for the swept-width check (2.5 rule 4), world units. */
    double body_half_w;         /* widest half width in the car table            */
    double body_lf;             /* rear axle to front corner, longest in table   */
    int    log_median;          /* [T10] dev census only: log every divided way the
                                 * T9 whole-way rule narrowed (ROAM_MEDIAN_T9) and
                                 * every divided edge's verdict (ROAM_MEDIAN_EDGE).
                                 * Not a build rule: not in the tile input hash.  */
} TD5_RoamGraphParams;

/* Census of one build (also logged). Arrays indexed by arm count are capped at
 * TD5_ROAM_ARM_HIST - 1 (the last bucket is "that many or more"). */
#define TD5_ROAM_ARM_HIST 10
typedef struct {
    int    ways_in, ways_kept, drop[TD5_ROAM_DROP_COUNT];
    int    divided_ways;       /* [T9] carriageways given their own OSM count */
    int    wide_ways;          /* [T9] two-way avenidas at twice the figure   */
    int    median_capped;      /* [T9] divided carriageways the T9 whole-way
                                * rule would narrow (census only since T10)  */
    int    divided_edges;      /* [T10] edges of divided carriageways        */
    int    median_capped_edges;/* [T10] of those, narrowed to keep the median */
    double median_capped_len;  /* [T10] their length, units                  */
    int    median_narrow_edges;/* [T10] edges on lanes narrower than 1500    */
    int    nodes, edges, joins, tapers, bends, junction_nodes, dead_ends;
    int    layer_cross_nodes;
    int    clusters, cluster_by_arms[TD5_ROAM_ARM_HIST];
    int    multi_node_clusters, internal_edges;
    int    unions_edge, unions_disc, rejected_unions, rejected_layer;
    double max_diameter;       /* world units                                   */
    int64_t max_diameter_key;
    double max_component;      /* extent of the largest candidate proximity
                                * component (bounds the halo T5 needs)          */
    int    fan_ok, fan_fallback, earclip_fail, clusters_no_arm;
    int    simple_fallback;      /* clusters rebuilt with the simple outline  */
    int    absorbed_off_patch;   /* [T9] NORMAL patches failed: an absorbed
                                  * edge left the patch (rebuilt UNION)       */
    int    hull_fallback;        /* simple clusters patched by the hull       */
    int    split_fallback;       /* multi-node clusters rebuilt per node      */
    int    chain_absorbed;       /* short pieces past a JOIN under a patch    */
    int    corner_pts_past_cut;  /* fillet points dropped: past a cut row     */
    int    corner_pts_out_of_sector; /* dropped: outside the arms' sector     */
    int    corners_parallel, corners_far, fillets, corners_reflex, bevels;
    int    fillet_shrunk;        /* fillet radius cut to fit the arm cut limit */
    int    tri_splits, patch_tris, patch_verts, recipe_overflow;
    int    sliver_flips, sliver_kept;  /* [T10] patch slivers flipped away / left */
    int    ribbon_sharp_vertices;/* interior polyline turns > 45 deg          */
    int    swept_checked, swept_widened, swept_fail;
    int    ribbon_swept_checked, ribbon_swept_fail;
    int    short_edges_clamped;   /* edge ends whose cut hit half the edge      */
    int    signals_attached;
    uint32_t ms;
} TD5_RoamGraphCensus;

typedef struct {
    /* Nodes, sorted by key. */
    TD5_RoamNode   *node;    int nnode;
    TD5_RoamEndRef *node_end;int nnode_end;
    /* Edges, sorted by key. */
    TD5_RoamEdge   *edge;    int nedge;
    TD5_RoamPt     *pt;      int npt;
    /* Clusters (td5_roam_junction.h), sorted by key. */
    struct TD5_RoamCluster   *cluster;  int ncluster;
    int                      *cluster_node; int ncluster_node; /* member nodes  */
    struct TD5_RoamArm       *arm;      int narm;
    struct TD5_RoamPatchVert *pvert;    int npvert;
    struct TD5_RoamPatchTri  *ptri;     int nptri;
    struct TD5_RoamOutline   *outline;  int noutline;          /* boundary segs */
    /* Ways read, and why the rest were dropped (cache way index order). */
    int            *drop;    int nway;
    TD5_RoamGraphParams prm;
    TD5_RoamGraphCensus census;
} TD5_RoamGraph;

void td5_roam_graph_params_default(TD5_RoamGraphParams *p);

/* The drivable filter of design 1.2 for one cache way: TD5_ROAM_DROP_*. Reads
 * the way's raw tag bits (TD5_RoamWay.tagf, td5_roam_cache.h). Pure. */
int  td5_roam_way_drop_reason(const TD5_RoamWay *w);

/* Build the graph over every session way whose bbox meets win = {minx, minz,
 * maxx, maxz} grown by `halo` (td5_roam_cache_load_window must have loaded that
 * window). Nodes, edges, clusters, patches and every edge end are complete for
 * any node inside win; nodes within ~cap_diameter of the halo border may see a
 * truncated neighbourhood (T5 sizes its halo from census.max_component).
 * Returns 1 on success, 0 on out of memory (g is then freed). `prm` NULL = the
 * defaults. */
int  td5_roam_graph_build(TD5_RoamGraph *g, const double win[4], double halo,
                          const TD5_RoamGraphParams *prm);
void td5_roam_graph_free(TD5_RoamGraph *g);

/* Attach traffic signals (place-frame x,z pairs): every edge end whose node is
 * within prm.signal_r of a signal gets signal = 1. Returns ends flagged. */
int  td5_roam_graph_attach_signals(TD5_RoamGraph *g, const double *xz, int n);

/* Builder-internal: rewrite every JOIN node's shared row (used when the
 * junction builder reruns a pass, td5_roam_junction.c). */
void td5_roam_graph_refill_joins(TD5_RoamGraph *g);

/* Lookup by key (binary search). -1 when absent. */
int  td5_roam_graph_find_node(const TD5_RoamGraph *g, int64_t key);
int  td5_roam_graph_find_edge(const TD5_RoamGraph *g, uint64_t key);

/* The polyline point at arc length s from node_a (clamped to [0, length]), and
 * the unit a->b tangent of the segment that holds it. A point exactly on an
 * interior vertex takes the FOLLOWING segment's tangent. Pure. */
void td5_roam_edge_point(const TD5_RoamGraph *g, const TD5_RoamEdge *e, double s,
                         double *x, double *z, double *tx, double *tz);

/* The perpendicular row of edge e at arc length s: lanes+1 lane-boundary points,
 * [0] = right kerb of a->b. This is THE function that makes every CLUSTER and
 * DEAD end row (design 1.3 step 3 "roam_arm_cut_row"). Writes out[0..lanes]. */
void td5_roam_edge_row(const TD5_RoamGraph *g, const TD5_RoamEdge *e, double s,
                       TD5_RoamPt *out, double *tx, double *tz);

#endif /* TD5_ROAM_WORLD_H */
