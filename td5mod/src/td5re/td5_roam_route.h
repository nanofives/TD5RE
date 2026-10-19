/**
 * td5_roam_route.h -- GEO FREE ROAM: the route model + planner (PORT-ONLY, F5-A).
 * See docs/plans/GEO_FREE_ROAM.md "Route, race and barriers" and the F5 row.
 *
 * WHAT IT IS. An ordered list of WAYPOINTS (START, middle points, FINISH, lat/lon, the
 * source of truth) planned over the STREET GRAPH into an ordered EDGE list and a POLYLINE
 * with distance-along. Progress, checkpoints and laps (F6+) all key off that polyline,
 * never off strip spans.
 *
 * THE PLANNER GRAPH. Roam has no process-wide graph: the T3 builder (td5_roam_world.c) is a
 * per-tile windowed pass that shares one cache session with the stream worker. The planner
 * must see the WHOLE casco (a route is not bounded by the resident set), so it builds its
 * own compact graph from the ROAMTILE files. [F9b] Not every file of the place: a REGION (the casco,
 * a ring round the car and each waypoint, a corridor along each leg), grown as the car and the
 * waypoints move, its missing tiles built on the stream worker, the graph rebuilt off-thread and
 * swapped in (td5_roam_route_graph_poll). From the files: every EDGE record (key, the two
 * end keys, oneway, klass) and the centre line of its ribbon quads, runs of one edge in
 * different tiles merged by key and ordered by row0. A* runs over directed edges: one-way
 * respected, no U-turn except at a dead end.
 *
 * THE FILE. re/assets/geo/<place>/routes/<name>.json: place slug, waypoints (lat/lon),
 * type (sprint / circuit), laps, edge count, edge-list hash, length. Waypoints are the
 * source of truth; the edge list is re-derived on load and the hash flags a map change.
 *
 * STATE. One file-scope session (td5_roam_route.c). Not thread safe: main thread only.
 */
#ifndef TD5_ROAM_ROUTE_H
#define TD5_ROAM_ROUTE_H

#include <stdint.h>
#include <stddef.h>

#define TD5_ROUTE_MAX_WP     24
#define TD5_ROUTE_NAME_MAX   32

#define TD5_ROUTE_WP_START   1
#define TD5_ROUTE_WP_MID     0
#define TD5_ROUTE_WP_FINISH  2

#define TD5_ROUTE_SPRINT     0
#define TD5_ROUTE_CIRCUIT    1
#define TD5_ROUTE_MAX_LAPS   10      /* [F9a] circuit laps 1..10 */

typedef struct {
    double  lat, lon;        /* canonical: rounded to 1e-8 deg (1 mm), printed with %.8f */
    int32_t px, pz;          /* PLACE frame units, derived from lat/lon        */
    int     kind;            /* TD5_ROUTE_WP_*                                  */
    /* where the planner snapped it (valid once the graph is ready) */
    int     snapped;
    double  sx, sz;          /* snapped point, place frame                      */
    uint64_t edge_key;       /* street piece                                    */
    double  snap_m;          /* metres moved onto the road                      */
} TD5_RouteWp;

typedef struct {
    uint64_t key;            /* edge key (way_id << 16 | first vertex)           */
    int      dir;            /* +1 = along a->b, -1 = against                    */
} TD5_RouteEdgeRef;

typedef struct {
    int      ok;             /* 1 = every leg planned                            */
    int      closed;         /* [F9a] 1 = a circuit whose closing leg (FINISH back to START) is planned: the polyline is ONE lap, last point = first */
    int      legs, legs_ok;
    TD5_RouteEdgeRef *edge;  int nedge;
    double  *px, *pz;        /* polyline, place frame                            */
    double  *dist_m;         /* distance along, metres                           */
    int      npt;
    double   length_m;
    uint32_t hash;           /* FNV-1a over the (key, dir) sequence              */
    double   plan_ms;
} TD5_RouteResult;

/* ---- the planner graph ------------------------------------------------------ */
/* Build (once per place) the planner graph from the ROAMTILE files. 1 when ready. */
int  td5_roam_route_graph_ensure(const char *place);
int  td5_roam_route_graph_ready(void);
/* Build on a worker thread (session start); ensure() then waits for it. */
void td5_roam_route_graph_begin_async(const char *place);
int  td5_roam_route_graph_building(void);
void td5_roam_route_graph_free(void);
/* [F9b] Per frame (td5_roam_route_ui_frame): grow the region round the car / the waypoints, queue its missing
 * tiles on the stream worker, rebuild the graph off-thread and swap it in. */
void td5_roam_route_graph_poll(void);
/* [F9b] Graph generation: bumped by every swap (edge / node INDICES change, keys do not). */
long td5_roam_route_graph_gen(void);
/* [F9b x F8] Translate an edge / node INDEX a consumer kept from graph generation from_gen into the current
 * graph (through its key): the new index, -1 when the key left the graph, -2 when from_gen is older than the
 * previous generation (re-resolve by position). from_gen == current returns the index unchanged. */
int  td5_roam_route_graph_remap_edge(long from_gen, int e);
int  td5_roam_route_graph_remap_node(long from_gen, int n);
/* [F9b] Region tiles still being built for the planner (the map shows "MAPPING STREETS"). */
int  td5_roam_route_graph_pending(void);
/* [F9b] 1 when the graph holds every file of the current region and nothing is queued or building. */
int  td5_roam_route_graph_settled(void);
/* Counters for the log / tests. */
void td5_roam_route_graph_stats(int *nedge, int *nnode, int *nseg, double *ms, double *km);
/* [F6-A] ribbon ends the planner graph has at a node / cluster key; -1 when the key is unknown. */
int  td5_roam_route_graph_node_degree(int64_t key);

/* Visit every street edge whose bbox meets a PLACE-frame window (map / minimap drawing).
 * xz = the edge's centre line, place frame int32 pairs. Returns the edges visited. */
typedef void (*TD5_RouteGfxCb)(void *user, int klass, int lanes, int oneway, const int32_t *xz, int npt);
int  td5_roam_route_graph_query(double minx, double minz, double maxx, double maxz,
                                TD5_RouteGfxCb cb, void *user);

/* The short links from a junction centre to the end of each street that meets it (the gap a
 * junction patch leaves between two ribbons), for the same drawing. */
typedef void (*TD5_RouteLinkCb)(void *user, int klass, double x0, double z0, double x1, double z1);
int  td5_roam_route_graph_links(double minx, double minz, double maxx, double maxz,
                                TD5_RouteLinkCb cb, void *user);

/* Nearest street point to a PLACE-frame point within `max_m` metres. 1 on success. */
int  td5_roam_route_snap_xz(double px, double pz, double max_m, double *sx, double *sz,
                            uint64_t *edge_key, double *dist_m);

/* [F8] read-only view of the planner graph (the ambient traffic's lane follower). An edge's
 * polyline runs node[0] -> node[1] (the ribbon centre line, place frame int32 x,z pairs), cum[] the
 * distance along it in units; oneway 0 two-way, +1 node[0]->node[1], -1 against. A node's inc[] holds
 * edge*2 + end (end 0 = the edge's node[0] side). */
typedef struct {
    uint64_t key;
    int      node[2];
    const int32_t *pt; const double *cum; int npt;
    double   plen;
    int      oneway, klass, lanes, flags;
} TD5_RouteGEdge;
typedef struct { int64_t key; double x, z; const int *inc; int ninc; } TD5_RouteGNode;
int  td5_roam_route_graph_nedge(void);
int  td5_roam_route_graph_nnode(void);
int  td5_roam_route_graph_edge_view(int e, TD5_RouteGEdge *out);
int  td5_roam_route_graph_node_view(int n, TD5_RouteGNode *out);
/* nearest edge to a PLACE point within max_m: edge index, units along it, metres moved. 1 on success */
int  td5_roam_route_graph_snap_edge(double px, double pz, double max_m, int *edge, double *along, double *dist_m);
/* signalised cluster `key` (a node key): its arms' bearings (ARM bearing12) and edge keys in the tile's
 * ARM order, up to cap. Returns the arm count (>= 1), 0 when the cluster has no signal. */
int  td5_roam_route_graph_signal(int64_t key, int16_t *bear, uint64_t *ekey, int cap);
int  td5_roam_route_graph_nsignal(void);
/* [F10] edge index of an edge key, -1 when unknown */
int  td5_roam_route_graph_edge_index(uint64_t key);
/* [G2] the via points the A* puts between arm in_inc and arm out_inc (node_view inc[] values, edge * 2 + end) of node
 * `node` (v = x0, z0, x1, z1, place frame): returns how many, 0..2 (0 = straight from arm end to arm end). The node position
 * is the one point unless the pair has a connector of its own. They are kept ON the junction patch. */
int  td5_roam_route_graph_turn_via(int node, int in_inc, int out_inc, double v[4]);
/* [F10] ONE leg between two PLACE points over the planner graph (the cops' pursuit path): both ends snapped
 * within snap_m metres; (dir_x, dir_z) the start's heading (0, 0 = leave either way along the snapped street;
 * a one-way taken the wrong way leaves either way); ignore_ow 1 = one-way streets usable both ways (a pursuing cop
 * close to its target); edge_ok NULL or a per-edge-index mask the leg stays on (the
 * snapped ends' own edges are always usable). One-way respected, no U-turn except at a dead end, like a route
 * leg. The polyline is a DRIVER'S line: td5_roam_ai.c's connector and median jogs are taken out (a divided avenue's
 * cluster centre sits in the median). Writes a malloc'd polyline (place frame; caller free()s px and pz) and its length.
 * 1 on success. */
int  td5_roam_route_plan_path(double sx, double sz, double dir_x, double dir_z, double tx, double tz, double snap_m,
                              const uint8_t *edge_ok, int ignore_ow, double **px, double **pz, int *npt, double *len_m);

/* ---- the route (one per roam session) ---------------------------------------- */
void td5_roam_route_reset(void);                     /* clear waypoints, result, undo */
int  td5_roam_route_wp_count(void);
const TD5_RouteWp *td5_roam_route_wp(int i);
const TD5_RouteResult *td5_roam_route_result(void);
int  td5_roam_route_type(void);
int  td5_roam_route_laps(void);
void td5_roam_route_set_type(int type, int laps);

enum { TD5_ROUTE_OP_START = 1, TD5_ROUTE_OP_ADD, TD5_ROUTE_OP_FINISH };
/* Edit by PLACE-frame point. Each returns 1 when the waypoint list changed (a snap that
 * found no street within 250 m changes nothing and returns 0). Replans on change. */
int  td5_roam_route_set_start_at(int32_t px, int32_t pz);
int  td5_roam_route_add_at(int32_t px, int32_t pz);          /* before FINISH, after the rest */
int  td5_roam_route_set_finish_at(int32_t px, int32_t pz);
int  td5_roam_route_move(int idx, int32_t px, int32_t pz);
int  td5_roam_route_remove(int idx);
int  td5_roam_route_undo(void);
int  td5_roam_route_clear(void);

/* Index of the waypoint nearest to a PLACE-frame point within `max_m`, else -1. */
int  td5_roam_route_pick(double px, double pz, double max_m);

/* Replan now (after a load / a graph that became ready). */
void td5_roam_route_replan(void);

/* ---- save / load ------------------------------------------------------------- */
/* Names are [A-Za-z0-9_-], 1..TD5_ROUTE_NAME_MAX-1 chars. Return 1 on success. */
int  td5_roam_route_name_ok(const char *name);
int  td5_roam_route_save(const char *name);
int  td5_roam_route_load(const char *name);
int  td5_roam_route_delete_file(const char *name);
/* Fill up to `max` names (sorted). Returns the count. */
int  td5_roam_route_list(char names[][TD5_ROUTE_NAME_MAX], int max);
/* Last loaded / saved name ("" = none) and whether the last load's hash matched. */
const char *td5_roam_route_current_name(void);
int  td5_roam_route_map_changed(void);               /* 1 = the stored hash differs */

/* Unit helpers. */
double td5_roam_route_upm(void);
int    td5_roam_route_place_to_latlon(double px, double pz, double *lat, double *lon);
int    td5_roam_route_latlon_to_place(double lat, double lon, int32_t *px, int32_t *pz);
const char *td5_roam_route_place(void);

#ifndef TD5RE_RELEASE
/* DEV: TD5RE_ROAM_ROUTE_TEST=1 -> planner census + save/load round trip, then exit (1). */
int td5_roam_route_dev_test(void);
#endif

#endif /* TD5_ROAM_ROUTE_H */
