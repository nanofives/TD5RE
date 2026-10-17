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
 * own compact graph ONCE per place from the ROAMTILE files: every EDGE record (key, the two
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
/* Counters for the log / tests. */
void td5_roam_route_graph_stats(int *nedge, int *nnode, int *nseg, double *ms, double *km);

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
