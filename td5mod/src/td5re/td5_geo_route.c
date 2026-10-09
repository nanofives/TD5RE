/**
 * td5_geo_route.c -- GEO TRACK: route + condition, in C (PORT-ONLY)
 *
 * Contract and rationale in td5_geo_route.h. This is the C port of three
 * Python tools, kept in that order below:
 *
 *   1. re/tools/geo_route.py       RoadGraph over ROADS.JSON, A* with waypoints
 *   2. re/tools/geo_condition.py   condition_route and everything it calls
 *   3. re/tools/geo_selector.py    _load_graph / graph_verdict / click_verdict
 *                                  and what SEND TO GAME does (save_route)
 *
 * MIRRORING RULE, inherited from re/tools/geo_common.py. Every constant in the
 * first section carries the C file:line it was read from, and the Python tool
 * mirrors the same ones. If the generator moves, BOTH copies are wrong and this
 * module will certify routes the engine then breaks on -- which is the single
 * failure mode the conditioner exists to prevent. `python re/tools/geo_common.py
 * --check` diffs the Python copy against the headers; this file is checked by
 * the parity harness at the bottom, which re-derives every number against the
 * committed fixtures.
 *
 * PARITY, AND WHERE IT IS DELIBERATELY NOT EXACT. Three places, all documented
 * at their site and all in the direction of "the C is right and the Python has
 * a bug or a limit":
 *
 *   a) a route over the 3000-span cap skips crossing detection. Python runs an
 *      O(n^2) pass it then throws away; the route is refused either way.
 *   b) the crossing MARKER is placed at the site's own node, not at node 0.
 *      geo_selector.py reads `node_a` off a MERGED group, which has no such key
 *      (merge_crossings writes a_lo/a_hi/a_first/a_last), so every marker it
 *      draws lands on node 0. Reproducing that would put every red dot on the
 *      start line.
 *   c) SEND TO GAME re-grids the existing rasters into the new route frame
 *      instead of re-fetching them. See the RASTER REBUILD section.
 *
 * BYTE-IDENTITY. Nothing here is reachable from a synthetic auto-track build.
 * It draws no tg_rand/tg_frand/tg_range (td5_trackgen_internal.h:1290-1296) and
 * keeps no generator state.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <dirent.h>
#include <direct.h>            /* _mkdir: the derived route-frame dir */

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_route.h"
#include "td5_geo_roads.h"       /* [1011 C2] the shared name classifier   */
#include "td5_geo_sidewalk.h"    /* [1011 C2] the per-place carriageway    */
#include "deps/cjson/cJSON.h"

#define LOG_TAG "geo"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================================================================== *
 * SECTION: mirrored generator constants
 * ======================================================================== */

/* td5_trackgen.h:27 / :29 -- one lane's width, and the down-track span. */
#define GR_LANE_WIDTH        1500.0
#define GR_SPAN_LENGTH       1500.0
/* td5_tg_world.h:62 -- one world cell is exactly one span. */
#define GR_WORLD_CELL        1500.0
/* td5_trackgen_internal.h:646 -- hard span ceiling. s_struct[]/s_rn[] are
 * indexed by NODE with no bounds check, so exceeding it corrupts memory. */
#define GR_MAX_SPANS         3000
/* td5_trackgen_internal.h:102 -- grid (start-line) span. */
#define GR_GRID_SPAN         24
/* The SYNTHETIC walk's fixed straight lead-in (td5_tg_road.c, tg_build_centerline),
 * kept only as the value TD5RE_GEO_LEAD_IN restores. See gr_lead_in_nodes(). */
#define GR_LEAD_IN_SYNTH     (GR_GRID_SPAN + 16)
/* td5_trackgen_internal.h:1352 -- DUAL_LANE caps lanes at 12. */
#define GR_MAX_LANES         12
/* Spec defaults, td5_tg_pages.c:3470 and the AT_Row table td5_fe_race.c:7286. */
#define GR_CURVE_SAFETY_X100 180
#define GR_MAX_GRADE_X1000   120
/* geo_common.py -- the one number Phase 0 confirmed by measurement. */
#define GR_UNITS_PER_METRE   430.0
/* geo_common.py -- plan section 3. */
#define GR_ELEV_EXAGGERATION 1.5
/* geo_condition.py -- td5_trackgen_internal.h:1370, TD5_TG_HEADING_LIMIT. */
#define GR_HEADING_LIMIT     1.396
/* geo_condition.py GRADE_SEPARATION_M. */
#define GR_GRADE_SEPARATION_M 4.0
/* geo_condition.py -- TD5_TG_UP_CLEAR (2600) + TD5_TG_BRIDGE_UNDER (480). */
#define GR_XSEP_LIFT_UNITS   3080.0
/* td5_tg_road.c:236 -- no structure at or below this span. */
#define GR_XSEP_FIRST_SPAN   (GR_GRID_SPAN + 25)
/* td5_tg_road.c:56 -- TD5RE_TG_BRIDGE_MAX, the deck-run span cap. */
#define GR_XSEP_BRIDGE_MAX   56
/* geo_condition.py XSEP_RAMP_GRADE_FRACTION. */
#define GR_XSEP_RAMP_FRAC    0.5

/* geo_route.py SNAP -- junction detection rounding, world units. */
#define GR_SNAP              10.0
/* geo_route.py -- min(CLASS_COST.values()), the admissible heuristic scale. */
#define GR_H_SCALE           0.80
#define GR_CLASS_COST_DEF    1.5
/* Orientation tie-break slack, DEGREES. See the sort in gr_condition. */
#define GR_ORIENT_TIE_DEG    1e-6

/* geo_selector.py "not enough road here". */
#define GR_GRAPH_MIN_WAYS     8
#define GR_GRAPH_MIN_NODES    40
#define GR_GRAPH_MIN_MAIN_FRAC 0.35

/* geo_condition.py merge_crossings span_tol. */
#define GR_MERGE_SPAN_TOL     8

/* Local caps. Nothing in the Python has these; they exist because this runs in
 * the game process and an oversized file must be refused rather than allowed
 * to run off a pool. Sized against La Plata (2291 ways / 11739 points). */
#define GR_MAX_ROADS         16384
#define GR_MAX_ROAD_PTS     131072
#define GR_MAX_WAY_PTS         512
#define GR_MAX_FILE   (64 * 1024 * 1024)
#define GR_MAX_WAYPOINTS        32
#define GR_MAX_NODES_HARD    65536   /* resample guard; 3000 spans is the real cap */
#define GR_MAX_SITES          4096

static const struct { const char *name; double cost; } k_class_cost[] = {
    { "motorway",       0.80 }, { "motorway_link",  1.00 },
    { "trunk",          0.85 }, { "trunk_link",     1.00 },
    { "primary",        0.90 }, { "primary_link",   1.05 },
    { "secondary",      1.00 }, { "secondary_link", 1.10 },
    { "tertiary",       1.15 }, { "tertiary_link",  1.25 },
    { "unclassified",   1.40 }, { "residential",    1.50 },
    { "living_street",  2.20 }, { "service",        2.60 },
    { "road",           1.50 },
};

/* [ROUND 1009 item 1] HOW MANY SPANS OF ROAD THE CONDITIONER INVENTS BEFORE THE
 * REAL ONE. The answer is now ZERO by default, and that is the whole fix.
 *
 * The conditioner used to prepend GR_LEAD_IN_SYNTH (40) dead-straight nodes
 * along +X before the first routed vertex, copying the synthetic walk's own
 * lead-in. On a procedural track that road is as real as the rest of it; on a
 * GEO track it is 40 spans -- 139.5 m at 430 units/m -- of tarmac that does not
 * exist, laid down ahead of the point the user clicked, and ending in a join
 * the smoothing then has to be rotated to hide (step 3b below). Mariano's
 * round-1009 note 1 is exactly this: "I created the start point and instead of
 * following the existing road it created some spans of road at the beginning
 * that don't exist."
 *
 * Nothing needs the lead-in to be straight. td5_geo.c's loader asks only for
 * node 0 at the origin and a 1500-unit chord between neighbours, and step 3b
 * still rotates the frame so the first segment points +X, so the generator's
 * TD5_TG_AXIS_HEADING convention holds with the REAL road's first span in that
 * role. The start grid (spans 0..24) then sits on the road the user picked:
 * measured on his La Plata route the first 40 real spans turn 17.4 deg in
 * total, 8.0 deg worst per span against the conditioner's own 21.3 deg limit,
 * so the grid lands on a gentle sweep rather than on invented tarmac.
 *
 * TD5RE_GEO_LEAD_IN=40 restores the old behaviour for a single-variable A/B. */
static int gr_lead_in_nodes(void)
{
    return td5_env_int("TD5RE_GEO_LEAD_IN", 0, 0, 64);
}

/* Source vertex whose arclength fraction is nearest `f`. `frac` NULL falls back
 * to the index fraction, which is what a 1-point table or an OOM leaves. */
static int gr_src_at(double f, const double *frac, int m)
{
    int lo = 0, hi;
    if (m < 2) return 0;
    if (!frac) {
        const int k = (int)(f * (double)(m - 1) + 0.5);
        return k < 0 ? 0 : (k > m - 1 ? m - 1 : k);
    }
    if (f <= 0.0) return 0;
    if (f >= 1.0) return m - 1;
    hi = m - 1;
    while (lo < hi) {                     /* first index with frac >= f */
        const int mid = lo + (hi - lo) / 2;
        if (frac[mid] < f) lo = mid + 1;
        else               hi = mid;
    }
    if (lo > 0 && (f - frac[lo - 1]) < (frac[lo] - f)) lo--;
    return lo;
}

static double gr_class_cost(const char *s)
{
    size_t i;
    if (!s) return GR_CLASS_COST_DEF;
    for (i = 0; i < sizeof k_class_cost / sizeof k_class_cost[0]; i++)
        if (!strcmp(s, k_class_cost[i].name)) return k_class_cost[i].cost;
    return GR_CLASS_COST_DEF;
}

/* ======================================================================== *
 * SECTION: small numeric helpers (Python-faithful)
 * ======================================================================== */

/* Python's round() is round-half-to-EVEN, and so is nearbyint in the default
 * rounding mode. The difference matters: a road vertex at an exact multiple of
 * GR_SNAP/2 would hash to a different junction cell under round-half-away, and
 * a junction that splits in two is a route that cannot be found. */
static double gr_round_even(double v) { return nearbyint(v); }

static double gr_dist(double ax, double az, double bx, double bz)
{
    return hypot(bx - ax, bz - az);
}

static double gr_wrap(double a)
{
    while (a >  M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

static double gr_deg(double r) { return r * (180.0 / M_PI); }

/* geo_common.min_turn_radius / max_turn_per_span. */
static double gr_min_turn_radius(double width, int cs100)
{
    return (width * 0.5) * ((double)cs100 / 100.0);
}

static double gr_max_turn_per_span(double width, double step, int cs100)
{
    const double r = gr_min_turn_radius(width, cs100);
    const double ratio = step / (2.0 * r);
    if (ratio >= 1.0) return M_PI;
    return 2.0 * asin(ratio);
}

/* geo_common.too_close_need -- the exact separation tg_too_close demands
 * (td5_trackgen.c:1760-1772). */
static double gr_too_close_need(double wa, double wb, double lane_w)
{
    return (wa + wb) * 0.5 + lane_w * 0.25;
}

/* geo_common.adjacent_skip. Derived from the CURVATURE floor, not the heading
 * budget -- the 2026-08-27 root cause produced 351 and exempted precisely the
 * band where real overlaps live. */
static int gr_adjacent_skip(double lane_w, double span_len, int cs100)
{
    const double w_max    = (double)GR_MAX_LANES * lane_w;
    const double need_max = w_max + lane_w * 0.25;
    const double safety   = (double)cs100 / 100.0;
    const double two_r    = safety * w_max;
    const double step     = span_len > 1.0 ? span_len : 1.0;
    double skip;
    if (two_r > need_max) skip = ceil((two_r / step) * asin(need_max / two_r)) + 2.0;
    else                  skip = ceil(need_max / step) + 2.0;
    return skip < 1.0 ? 1 : (int)skip;
}

/* ======================================================================== *
 * SECTION: LocalProjection (geo_common.py)
 * ======================================================================== */

#define GR_WGS84_A  6378137.0
#define GR_WGS84_F  (1.0 / 298.257223563)
#define GR_WGS84_E2 (GR_WGS84_F * (2.0 - GR_WGS84_F))
#define GR_RAD_PER_DEG 0.017453292519943295   /* math.radians(1.0) */

typedef struct {
    double lat0, lon0, upm;
    double m_per_deg_lat, m_per_deg_lon;
    double cos_t, sin_t;
    double off_x, off_z;
} GeoProj;

static void gr_proj_init(GeoProj *p, double lat0, double lon0, double upm)
{
    const double phi = lat0 * GR_RAD_PER_DEG;
    const double s   = sin(phi);
    const double w   = 1.0 - GR_WGS84_E2 * s * s;
    memset(p, 0, sizeof *p);
    p->lat0 = lat0;
    p->lon0 = lon0;
    p->upm  = upm;
    p->m_per_deg_lat = GR_RAD_PER_DEG * GR_WGS84_A * (1.0 - GR_WGS84_E2) / pow(w, 1.5);
    p->m_per_deg_lon = GR_RAD_PER_DEG * GR_WGS84_A * cos(phi) / sqrt(w);
    p->cos_t = 1.0;
    p->sin_t = 0.0;
}

static void gr_proj_rot(GeoProj *p, double theta)
{
    p->cos_t = cos(theta);
    p->sin_t = sin(theta);
}

static double gr_proj_theta(const GeoProj *p)
{
    return atan2(p->sin_t, p->cos_t);
}

static void gr_proj_off(GeoProj *p, double dx, double dz)
{
    p->off_x = dx;
    p->off_z = dz;
}

static void gr_proj_to_world(const GeoProj *p, double lat, double lon,
                             double *x, double *z)
{
    const double e = (lon - p->lon0) * p->m_per_deg_lon;
    const double n = (lat - p->lat0) * p->m_per_deg_lat;
    *x = (e * p->cos_t - n * p->sin_t) * p->upm + p->off_x;
    *z = (e * p->sin_t + n * p->cos_t) * p->upm + p->off_z;
}

static void gr_proj_to_latlon(const GeoProj *p, double x, double z,
                              double *lat, double *lon)
{
    const double xm = (x - p->off_x) / p->upm;
    const double zm = (z - p->off_z) / p->upm;
    const double e  =  xm * p->cos_t + zm * p->sin_t;
    const double n  = -xm * p->sin_t + zm * p->cos_t;
    *lat = p->lat0 + n / p->m_per_deg_lat;
    *lon = p->lon0 + e / p->m_per_deg_lon;
}

/* ======================================================================== *
 * SECTION: io
 * ======================================================================== */

static char *gr_slurp(const char *path, int64_t *out_n)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || n > (int64_t)GR_MAX_FILE) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) {
        free(buf); td5_plat_file_close(f); return NULL;
    }
    buf[n] = '\0';
    td5_plat_file_close(f);
    if (out_n) *out_n = n;
    return buf;
}

/* Write through a .tmp and rename, like geo_common.write_json: a half-written
 * ROUTE.JSON that the loader then rejects would look like a conditioner bug. */
static int gr_write_atomic(const char *path, const void *data, size_t n)
{
    char tmp[512];
    TD5_File *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = td5_plat_file_open(tmp, "wb");
    if (!f) return 0;
    if (n && td5_plat_file_write(f, data, n) != n) {
        td5_plat_file_close(f);
        td5_plat_file_delete(tmp);
        return 0;
    }
    td5_plat_file_close(f);
    /* td5_plat_file_rename returns 0 on SUCCESS (it is MoveFileExA with
     * MOVEFILE_REPLACE_EXISTING, mapped to the POSIX convention), so it must be
     * compared against 0 and not used as a boolean. Testing it as a boolean
     * reported failure on every successful write -- and because the move had
     * already happened, the file was on disk while the caller was told it was
     * not, which made the parity dump silently produce nothing. */
    if (td5_plat_file_rename(tmp, path) != 0) {
        td5_plat_file_delete(tmp);
        return 0;
    }
    return 1;
}

/* ======================================================================== *
 * SECTION: the road graph (geo_route.py RoadGraph)
 * ======================================================================== */

typedef struct {
    int    to;
    double cost;
    int    road;
    int    next;          /* insertion-ordered list; see the note below */
} GrEdge;

typedef struct {
    int    first, count;  /* slice of the point pool */
    int    lanes;
    int    oneway;
    /* [ROUND 1010 AVENUES] OSM junction=roundabout|circular. A plaza ring is a
     * closed one-way loop carrying one name, so every pair of its arcs reads as
     * "two anti-parallel one-way ways of the same name a few metres apart" --
     * the exact shape gr_detect_medians looks for, which is how "Plaza Miguel
     * de Azcuenaga" became a divided avenue on Mariano's route. The tag says
     * so outright: MEASURED on La Plata's 2291 ways, 73 carry it (44 circular +
     * 29 roundabout), both plazas on his route are circular on EVERY arc, and
     * no way of Diagonal 73 / Calle 14 / Calle 54 carries it at all. */
    int    ring;
    double cost;          /* CLASS_COST multiplier */
    double id;            /* OSM way id; double because it is > 2^31 for relations */
    int    has_id;
    char  *name;          /* may be NULL */
} GrRoad;

static struct {
    char    slug[64];
    GeoProj proj;                 /* the PLACE frame, as PLACE.JSON has it */
    double  bbox[4];              /* W S E N, degrees -- see the header   */
    int     have_gbbox;
    double  gbbox[4];             /* route_graph_bbox, when PLACE.JSON pins one */

    int     n_roads;
    GrRoad *road;
    double *px, *pz;
    int     n_pts;

    int     n_nodes;
    double *nx, *nz;
    int    *adj_head, *adj_tail;
    GrEdge *edge;
    int     n_edges, cap_edges;

    /* junction hash: open addressing over (round(x/SNAP), round(z/SNAP)) */
    int    *hslot;
    int     hmask;

    /* A* scratch, kept so a re-route does not re-malloc */
    double *dist;
    int    *prev;
    unsigned *stamp;
    unsigned  gen;
    char   *seen;
    struct { double f; int node; } *heap;
    int     heap_n, heap_cap;
} s_g;

static void gr_graph_free(void)
{
    int i;
    for (i = 0; i < s_g.n_roads; i++) free(s_g.road[i].name);
    free(s_g.road); free(s_g.px); free(s_g.pz);
    free(s_g.nx); free(s_g.nz);
    free(s_g.adj_head); free(s_g.adj_tail); free(s_g.edge);
    free(s_g.hslot);
    free(s_g.dist); free(s_g.prev); free(s_g.stamp); free(s_g.seen);
    free(s_g.heap);
    memset(&s_g, 0, sizeof s_g);
}

void td5_geo_route_drop_graph(void) { gr_graph_free(); }

static int gr_hash_key(long long kx, long long kz, int mask)
{
    /* 64-bit mix; the table only has to spread, not to be cryptographic. */
    unsigned long long h = (unsigned long long)kx * 0x9E3779B97F4A7C15ull
                         ^ ((unsigned long long)kz + 0x165667B19E3779F9ull);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return (int)(h & (unsigned long long)mask);
}

/* geo_route.RoadGraph._node: the exact (rounded) key finds every junction,
 * because OSM ways that connect share the identical node and the projection is
 * deterministic. 10 units is 2.3 cm -- far tighter than any real road. */
static int gr_node_of(double x, double z)
{
    const long long kx = (long long)gr_round_even(x / GR_SNAP);
    const long long kz = (long long)gr_round_even(z / GR_SNAP);
    int slot = gr_hash_key(kx, kz, s_g.hmask);
    for (;;) {
        const int i = s_g.hslot[slot];
        if (i < 0) break;
        if ((long long)gr_round_even(s_g.nx[i] / GR_SNAP) == kx &&
            (long long)gr_round_even(s_g.nz[i] / GR_SNAP) == kz)
            return i;
        slot = (slot + 1) & s_g.hmask;
    }
    s_g.nx[s_g.n_nodes] = x;
    s_g.nz[s_g.n_nodes] = z;
    s_g.adj_head[s_g.n_nodes] = -1;
    s_g.adj_tail[s_g.n_nodes] = -1;
    s_g.hslot[slot] = s_g.n_nodes;
    return s_g.n_nodes++;
}

/* Edges are appended at the TAIL. A* iterates them in insertion order and a tie
 * is resolved by the first edge that strictly improves `dist`, so the order IS
 * part of the answer on a grid city where two L-shaped paths cost the same. */
static void gr_edge_add(int from, int to, double cost, int road)
{
    GrEdge *e;
    if (s_g.n_edges >= s_g.cap_edges) return;
    e = &s_g.edge[s_g.n_edges];
    e->to = to; e->cost = cost; e->road = road; e->next = -1;
    if (s_g.adj_tail[from] < 0) s_g.adj_head[from] = s_g.n_edges;
    else                        s_g.edge[s_g.adj_tail[from]].next = s_g.n_edges;
    s_g.adj_tail[from] = s_g.n_edges;
    s_g.n_edges++;
}

/* geo_selector._load_graph's bbox filter, which is NOT cosmetic: SEND TO GAME
 * re-fetches a WIDER circle, routing over that wider graph found a different
 * and shorter path for the same A/B (La Plata 1492 -> 1200 spans), so touching
 * a saved route silently changed it. PLACE.JSON "route_graph_bbox" pins the
 * area the user actually routed in, and the graph is limited to it. */
/* bb is {west, south, east, north}, the public order from td5_geo_route.h.
 * ONE order is used end to end -- the public call, the internal filter and
 * the commit's grid sizing all read these four slots the same way, because a
 * bbox that means two different things in two functions is the shape of bug
 * that reads as "the route is outside the place" with nothing logged. */
static int gr_in_bbox(double la, double lo, const double *bb)
{
    return bb[1] <= la && la <= bb[3] && bb[0] <= lo && lo <= bb[2];
}

static int gr_place_read(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *pr, *bb, *gb;
    int ok = 0;

    /* SOURCE, never the derived copy. The router routes on the PRISTINE
     * fetched frame, so a BUILD cannot change what the next BUILD may route
     * on -- the round-1007 "NO MAP DATA / unable to read road graph" bug was
     * exactly this read landing on a re-gridded, then re-re-gridded, file. */
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");
    json = gr_slurp(path, NULL);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    /* A place with no pin must not inherit the previous place's. */
    s_g.have_gbbox = 0;
    pr = cJSON_GetObjectItem(root, "projection");
    bb = cJSON_GetObjectItem(root, "bbox");
    if (pr && cJSON_IsObject(pr)) {
        const cJSON *a = cJSON_GetObjectItem(pr, "lat0");
        const cJSON *o = cJSON_GetObjectItem(pr, "lon0");
        const cJSON *u = cJSON_GetObjectItem(pr, "units_per_metre");
        const cJSON *r = cJSON_GetObjectItem(pr, "rotation_rad");
        const cJSON *ox = cJSON_GetObjectItem(pr, "offset_x");
        const cJSON *oz = cJSON_GetObjectItem(pr, "offset_z");
        if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) {
            gr_proj_init(&s_g.proj, a->valuedouble, o->valuedouble,
                         cJSON_IsNumber(u) ? u->valuedouble : GR_UNITS_PER_METRE);
            gr_proj_rot(&s_g.proj, cJSON_IsNumber(r) ? r->valuedouble : 0.0);
            gr_proj_off(&s_g.proj, cJSON_IsNumber(ox) ? ox->valuedouble : 0.0,
                                   cJSON_IsNumber(oz) ? oz->valuedouble : 0.0);
            ok = 1;
        }
    }
    if (bb && cJSON_IsObject(bb)) {
        const cJSON *s = cJSON_GetObjectItem(bb, "south");
        const cJSON *w = cJSON_GetObjectItem(bb, "west");
        const cJSON *n = cJSON_GetObjectItem(bb, "north");
        const cJSON *e = cJSON_GetObjectItem(bb, "east");
        if (cJSON_IsNumber(s) && cJSON_IsNumber(w) &&
            cJSON_IsNumber(n) && cJSON_IsNumber(e)) {
            s_g.bbox[0] = w->valuedouble; s_g.bbox[1] = s->valuedouble;
            s_g.bbox[2] = e->valuedouble; s_g.bbox[3] = n->valuedouble;
        }
    }
    gb = cJSON_GetObjectItem(root, "route_graph_bbox");
    if (gb && cJSON_IsObject(gb)) {
        const cJSON *s = cJSON_GetObjectItem(gb, "south");
        const cJSON *w = cJSON_GetObjectItem(gb, "west");
        const cJSON *n = cJSON_GetObjectItem(gb, "north");
        const cJSON *e = cJSON_GetObjectItem(gb, "east");
        if (cJSON_IsNumber(s) && cJSON_IsNumber(w) &&
            cJSON_IsNumber(n) && cJSON_IsNumber(e)) {
            s_g.gbbox[0] = w->valuedouble; s_g.gbbox[1] = s->valuedouble;
            s_g.gbbox[2] = e->valuedouble; s_g.gbbox[3] = n->valuedouble;
            s_g.have_gbbox = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static int gr_graph_load(const char *slug)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    int n, i, hsize;

    gr_graph_free();
    if (!gr_place_read(slug)) {
        TD5_LOG_E(LOG_TAG, "geo route: the SOURCE re/assets/geo/%s/PLACE.JSON "
                  "has no usable projection", slug);
        return 0;
    }
    /* [1011 C2] Select the place's own table BEFORE any way is sized: the
     * carriageway lookup below reads it per road. Idempotent. */
    td5_geo_sw_place(slug);
    td5_geo_source_path(path, sizeof path, slug, "ROADS.JSON");   /* SOURCE */
    json = gr_slurp(path, NULL);
    if (!json) {
        TD5_LOG_E(LOG_TAG, "geo route: no readable %s", path);
        gr_graph_free();
        return 0;
    }
    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        TD5_LOG_E(LOG_TAG, "geo route: %s is not valid JSON", path);
        gr_graph_free();
        return 0;
    }
    arr = cJSON_GetObjectItem(root, "roads");
    if (!arr || !cJSON_IsArray(arr) || (n = cJSON_GetArraySize(arr)) < 1) {
        TD5_LOG_E(LOG_TAG, "geo route: %s has no roads[]", path);
        cJSON_Delete(root);
        gr_graph_free();
        return 0;
    }
    if (n > GR_MAX_ROADS) n = GR_MAX_ROADS;

    s_g.road = (GrRoad *)calloc((size_t)n, sizeof(GrRoad));
    s_g.px   = (double *)malloc((size_t)GR_MAX_ROAD_PTS * sizeof(double));
    s_g.pz   = (double *)malloc((size_t)GR_MAX_ROAD_PTS * sizeof(double));
    if (!s_g.road || !s_g.px || !s_g.pz) { cJSON_Delete(root); gr_graph_free(); return 0; }

    for (i = 0; i < n; i++) {
        const cJSON *r   = cJSON_GetArrayItem(arr, i);
        const cJSON *pts = r ? cJSON_GetObjectItem(r, "points") : NULL;
        const cJSON *ll  = r ? cJSON_GetObjectItem(r, "latlon") : NULL;
        const cJSON *cl  = r ? cJSON_GetObjectItem(r, "class") : NULL;
        const cJSON *la  = r ? cJSON_GetObjectItem(r, "lanes") : NULL;
        const cJSON *ow  = r ? cJSON_GetObjectItem(r, "oneway") : NULL;
        const cJSON *jn  = r ? cJSON_GetObjectItem(r, "junction") : NULL;
        const cJSON *id  = r ? cJSON_GetObjectItem(r, "id") : NULL;
        const cJSON *nm  = r ? cJSON_GetObjectItem(r, "name") : NULL;
        GrRoad *out;
        int k, npt, keep = 1;

        if (!pts || !cJSON_IsArray(pts)) continue;
        npt = cJSON_GetArraySize(pts);
        if (npt < 1) continue;
        if (npt > GR_MAX_WAY_PTS) npt = GR_MAX_WAY_PTS;
        if (s_g.n_pts + npt > GR_MAX_ROAD_PTS) break;

        /* geo_selector._load_graph: keep a road with ANY point inside the
         * pinned routing bbox. The test is run on lat/lon, so use the file's
         * own `latlon` when it has one and project back when it does not. */
        if (s_g.have_gbbox) {
            keep = 0;
            for (k = 0; k < npt && !keep; k++) {
                double qa, qo;
                if (ll && cJSON_IsArray(ll) && k < cJSON_GetArraySize(ll)) {
                    const cJSON *e = cJSON_GetArrayItem(ll, k);
                    if (cJSON_GetArraySize(e) >= 2) {
                        qa = cJSON_GetArrayItem(e, 0)->valuedouble;
                        qo = cJSON_GetArrayItem(e, 1)->valuedouble;
                    } else continue;
                } else {
                    const cJSON *p  = cJSON_GetArrayItem(pts, k);
                    const cJSON *xx = p ? cJSON_GetObjectItem(p, "x") : NULL;
                    const cJSON *zz = p ? cJSON_GetObjectItem(p, "z") : NULL;
                    if (!cJSON_IsNumber(xx) || !cJSON_IsNumber(zz)) continue;
                    gr_proj_to_latlon(&s_g.proj, xx->valuedouble, zz->valuedouble,
                                      &qa, &qo);
                }
                if (gr_in_bbox(qa, qo, s_g.gbbox)) keep = 1;
            }
        }
        if (!keep) continue;

        out = &s_g.road[s_g.n_roads];
        out->first = s_g.n_pts;
        out->count = 0;
        for (k = 0; k < npt; k++) {
            const cJSON *p  = cJSON_GetArrayItem(pts, k);
            const cJSON *xx = p ? cJSON_GetObjectItem(p, "x") : NULL;
            const cJSON *zz = p ? cJSON_GetObjectItem(p, "z") : NULL;
            if (!cJSON_IsNumber(xx) || !cJSON_IsNumber(zz)) continue;
            s_g.px[s_g.n_pts] = xx->valuedouble;
            s_g.pz[s_g.n_pts] = zz->valuedouble;
            s_g.n_pts++;
            out->count++;
        }
        if (out->count < 1) { s_g.n_pts = out->first; continue; }

        out->cost   = gr_class_cost(cl && cJSON_IsString(cl) ? cl->valuestring : NULL);
        /* [ROUND 1011 C2] THE CARRIAGEWAY IS THE REAL ONE, AND PARITY LOSES.
         *
         * What stood here, and why it is no longer right: "geo_route.py reads
         * the RAW `lanes` field: int(r.get('lanes') or 2). It does NOT
         * re-derive one from the tagged `width` ... Parity wins here."
         *
         * That parity produced a 7 m road everywhere, because OSM tags `lanes`
         * on 2064 of La Plata's 2291 ways as 2 and tags `width` on only 50.
         * The city's calles are 10 m -- 34 of the 37 measured ones say exactly
         * "10". So the generated street was 3 m too narrow, which is both the
         * reported "streets seem too narrow" AND the reason the building-line
         * rule had 11 m of slack to hand the pavement. One wrong number, two
         * symptoms, and parity with a retired Python path is not worth either.
         *
         * Order: a TAGGED width is a measurement and wins; else the place's own
         * carriageway for this street KIND (by name, the same classifier the
         * pavement reader uses, so the two cannot disagree); else the `lanes`
         * field exactly as before. A place with no table keeps the old answer
         * on every way, so this is La Plata-shaped, not a global change.
         *
         * THE LANE COUNT IS THE KNOB, NOT THE LANE WIDTH:
         * TD5_TG_SPAN_LENGTH == TD5_TG_LANE_WIDTH is an engine invariant (a
         * street's frontage run is an integer number of lanes), so 10 m becomes
         * round(10 * upm / 1500) = 3 lanes = 10.47 m rather than 2 wide ones.
         *
         * CONSEQUENCES, stated because they are not local: the lane count is
         * the conditioner's road width and therefore its CURVATURE LIMIT, so a
         * route that converged at 2 lanes may converge differently at 3, and
         * every cached _route/ is invalidated. TD5RE_GEO_REAL_CARRIAGEWAY=0
         * restores the raw `lanes` field for an A/B. */
        out->lanes  = (la && cJSON_IsNumber(la) && la->valueint != 0) ? la->valueint : 2;
        if (td5_env_flag_on("TD5RE_GEO_REAL_CARRIAGEWAY")) {
            const int    nk = td5_geo_roads_namek_of(
                                  (nm && cJSON_IsString(nm)) ? nm->valuestring : NULL);
            const cJSON *wd = r ? cJSON_GetObjectItem(r, "width") : NULL;
            double m = 0.0;
            if (wd && cJSON_IsString(wd) && wd->valuestring[0]) {
                char *end = NULL;
                const double v = strtod(wd->valuestring, &end);
                if (end != wd->valuestring && v > 0.5 && v < 120.0) m = v;
            } else if (wd && cJSON_IsNumber(wd)
                       && wd->valuedouble > 0.5 && wd->valuedouble < 120.0) {
                m = wd->valuedouble;
            }
            if (m > 0.0 && s_g.proj.upm > 0.0) {
                /* A TAGGED width is a measurement of this way: it wins
                 * outright, in both directions. */
                const int lanes = (int)floor(m * s_g.proj.upm / GR_LANE_WIDTH + 0.5);
                if (lanes >= 1 && lanes <= GR_MAX_LANES) out->lanes = lanes;
            } else if (s_g.proj.upm > 0.0) {
                /* THE PLACE TABLE IS A FLOOR, NEVER A CEILING.
                 *
                 * `lanes_src` says where the lane count came from, and the
                 * distinction is the whole point: `osm_lanes` means a mapper
                 * COUNTED them (950 of La Plata's 2291 ways), `highway_class`
                 * means geo_fetch defaulted it (1341). Overriding a counted
                 * count with a city-wide default is how a real four-lane
                 * avenue gets narrowed to three -- measured, it would have hit
                 * 22 ways at 4 lanes and one at 5.
                 *
                 * But a counted LANE number is not a carriageway either: a La
                 * Plata calle tagged lanes=2 still has 10 m of asphalt, two
                 * traffic lanes plus parking both sides (760 ways). So the
                 * table raises a counted road to the city's carriageway and
                 * never lowers it, while an uncounted one simply takes it. */
                const double tm = td5_geo_sw_carriageway_m(TD5_GEO_RC_UNKNOWN, nk);
                const cJSON *ls = r ? cJSON_GetObjectItem(r, "lanes_src") : NULL;
                const int counted = ls && cJSON_IsString(ls) && ls->valuestring
                                 && strcmp(ls->valuestring, "osm_lanes") == 0;
                if (tm > 0.0) {
                    int lanes = (int)floor(tm * s_g.proj.upm / GR_LANE_WIDTH + 0.5);
                    if (counted && lanes < out->lanes) lanes = out->lanes;
                    if (lanes >= 1 && lanes <= GR_MAX_LANES) out->lanes = lanes;
                }
            }
        }
        out->oneway = (ow && cJSON_IsBool(ow)) ? (cJSON_IsTrue(ow) ? 1 : 0) : 0;
        /* [ROUND 1010 AVENUES] see GrRoad.ring. The field is null on all but 73
         * of La Plata's ways, so a missing/!string value is simply "not a
         * ring" -- no default to argue about. */
        out->ring   = (jn && cJSON_IsString(jn) && jn->valuestring &&
                       (strcmp(jn->valuestring, "roundabout") == 0 ||
                        strcmp(jn->valuestring, "circular") == 0)) ? 1 : 0;
        if (id && cJSON_IsNumber(id)) { out->id = id->valuedouble; out->has_id = 1; }
        if (nm && cJSON_IsString(nm) && nm->valuestring[0]) {
            size_t len = strlen(nm->valuestring) + 1u;
            out->name = (char *)malloc(len);
            if (out->name) memcpy(out->name, nm->valuestring, len);
        }
        s_g.n_roads++;
    }
    cJSON_Delete(root);

    if (s_g.n_roads < 1) {
        TD5_LOG_E(LOG_TAG, "geo route: %s held no usable road", path);
        gr_graph_free();
        return 0;
    }

    /* --- vertex graph ---------------------------------------------------- */
    hsize = 16;
    while (hsize < s_g.n_pts * 4) hsize <<= 1;
    s_g.hmask    = hsize - 1;
    s_g.hslot    = (int *)malloc((size_t)hsize * sizeof(int));
    s_g.nx       = (double *)malloc((size_t)s_g.n_pts * sizeof(double));
    s_g.nz       = (double *)malloc((size_t)s_g.n_pts * sizeof(double));
    s_g.adj_head = (int *)malloc((size_t)s_g.n_pts * sizeof(int));
    s_g.adj_tail = (int *)malloc((size_t)s_g.n_pts * sizeof(int));
    s_g.cap_edges = s_g.n_pts * 2 + 16;
    s_g.edge     = (GrEdge *)malloc((size_t)s_g.cap_edges * sizeof(GrEdge));
    if (!s_g.hslot || !s_g.nx || !s_g.nz || !s_g.adj_head || !s_g.adj_tail || !s_g.edge) {
        gr_graph_free();
        return 0;
    }
    memset(s_g.hslot, 0xFF, (size_t)hsize * sizeof(int));

    for (i = 0; i < s_g.n_roads; i++) {
        const GrRoad *r = &s_g.road[i];
        int k, prev = -1;
        for (k = 0; k < r->count; k++) {
            const int nd = gr_node_of(s_g.px[r->first + k], s_g.pz[r->first + k]);
            if (prev >= 0 && prev != nd) {
                const double d = gr_dist(s_g.nx[prev], s_g.nz[prev],
                                         s_g.nx[nd], s_g.nz[nd]) * r->cost;
                gr_edge_add(prev, nd, d, i);
                /* geo_route.py: oneway is OFF by default. A point-to-point RACE
                 * line in a grid city fails on almost every pair otherwise. */
                gr_edge_add(nd, prev, d, i);
            }
            prev = nd;
        }
    }

    s_g.dist  = (double *)malloc((size_t)s_g.n_nodes * sizeof(double));
    s_g.prev  = (int *)malloc((size_t)s_g.n_nodes * sizeof(int));
    s_g.stamp = (unsigned *)calloc((size_t)s_g.n_nodes, sizeof(unsigned));
    s_g.seen  = (char *)malloc((size_t)s_g.n_nodes);
    s_g.heap_cap = s_g.n_edges + 16;
    s_g.heap  = (void *)malloc((size_t)s_g.heap_cap * sizeof *s_g.heap);
    if (!s_g.dist || !s_g.prev || !s_g.stamp || !s_g.seen || !s_g.heap) {
        gr_graph_free();
        return 0;
    }
    s_g.gen = 0;
    snprintf(s_g.slug, sizeof s_g.slug, "%s", slug);
    TD5_LOG_I(LOG_TAG, "geo route: graph %s: %d way(s), %d point(s), %d node(s), "
              "%d edge(s)%s", path, s_g.n_roads, s_g.n_pts, s_g.n_nodes,
              s_g.n_edges, s_g.have_gbbox ? " [pinned to route_graph_bbox]" : "");
    return 1;
}

static int gr_graph_sync(const char *slug)
{
    if (!slug || !slug[0]) return 0;
    if (s_g.n_nodes && !strcmp(slug, s_g.slug)) return 1;
    return gr_graph_load(slug);
}

/* geo_route.RoadGraph.nearest -- a linear scan, first minimum wins. 16k nodes
 * is nothing next to the A* that follows, and a spatial index would have to
 * reproduce the same tie-break to be worth having. */
static int gr_nearest(double x, double z)
{
    int i, best = -1;
    double bd = DBL_MAX;
    for (i = 0; i < s_g.n_nodes; i++) {
        const double dx = s_g.nx[i] - x, dz = s_g.nz[i] - z;
        const double d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* heapq order: the tuple (f, node), so an f tie falls back on the node index.
 * Every push for a node carries a strictly smaller f than its last, so the
 * order is total and the pop sequence does not depend on the heap's shape. */
static int gr_heap_less(const double fa, const int na, const double fb, const int nb)
{
    if (fa < fb) return 1;
    if (fa > fb) return 0;
    return na < nb;
}

static void gr_heap_push(double f, int node)
{
    int i;
    if (s_g.heap_n >= s_g.heap_cap) return;
    i = s_g.heap_n++;
    s_g.heap[i].f = f; s_g.heap[i].node = node;
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (!gr_heap_less(s_g.heap[i].f, s_g.heap[i].node,
                          s_g.heap[p].f, s_g.heap[p].node)) break;
        { const double tf = s_g.heap[p].f; const int tn = s_g.heap[p].node;
          s_g.heap[p] = s_g.heap[i]; s_g.heap[i].f = tf; s_g.heap[i].node = tn; }
        i = p;
    }
}

static int gr_heap_pop(int *node)
{
    int i = 0;
    if (s_g.heap_n <= 0) return 0;
    *node = s_g.heap[0].node;
    s_g.heap[0] = s_g.heap[--s_g.heap_n];
    for (;;) {
        const int l = 2 * i + 1, r = l + 1;
        int m = i;
        if (l < s_g.heap_n && gr_heap_less(s_g.heap[l].f, s_g.heap[l].node,
                                           s_g.heap[m].f, s_g.heap[m].node)) m = l;
        if (r < s_g.heap_n && gr_heap_less(s_g.heap[r].f, s_g.heap[r].node,
                                           s_g.heap[m].f, s_g.heap[m].node)) m = r;
        if (m == i) break;
        { const double tf = s_g.heap[m].f; const int tn = s_g.heap[m].node;
          s_g.heap[m] = s_g.heap[i]; s_g.heap[i].f = tf; s_g.heap[i].node = tn; }
        i = m;
    }
    return 1;
}

/* geo_route.RoadGraph.path -- A* on straight-line distance, admissible because
 * the heuristic is scaled by the cheapest class multiplier. */
static int gr_path(int a, int b, int *out, int out_max)
{
    const double gx = s_g.nx[b], gz = s_g.nz[b];
    int n = 0, u;

    if (a == b) { if (out_max > 0) out[0] = a; return 1; }
    s_g.gen++;
    memset(s_g.seen, 0, (size_t)s_g.n_nodes);
    s_g.heap_n = 0;
    s_g.dist[a] = 0.0; s_g.stamp[a] = s_g.gen; s_g.prev[a] = -1;
    gr_heap_push(gr_dist(s_g.nx[a], s_g.nz[a], gx, gz) * GR_H_SCALE, a);

    while (gr_heap_pop(&u)) {
        double du;
        int e;
        if (s_g.seen[u]) continue;
        s_g.seen[u] = 1;
        if (u == b) {
            int k = b, m = 0;
            while (k != a) { m++; k = s_g.prev[k]; }
            n = m + 1;
            if (n > out_max) return -1;
            k = b;
            while (m >= 0) { out[m--] = k; k = s_g.prev[k]; }
            return n;
        }
        du = s_g.dist[u];
        for (e = s_g.adj_head[u]; e >= 0; e = s_g.edge[e].next) {
            const int v = s_g.edge[e].to;
            const double nd = du + s_g.edge[e].cost;
            if (s_g.stamp[v] == s_g.gen && nd >= s_g.dist[v]) continue;
            s_g.dist[v] = nd;
            s_g.stamp[v] = s_g.gen;
            s_g.prev[v] = u;
            gr_heap_push(nd + gr_dist(s_g.nx[v], s_g.nz[v], gx, gz) * GR_H_SCALE, v);
        }
    }
    return 0;
}

static int gr_edge_road(int u, int v)
{
    int e;
    for (e = s_g.adj_head[u]; e >= 0; e = s_g.edge[e].next)
        if (s_g.edge[e].to == v) return s_g.edge[e].road;
    return -1;
}

/* geo_selector._components, iterative. */
static void gr_components(int *out_comps, int *out_biggest)
{
    char *seen = (char *)calloc((size_t)s_g.n_nodes, 1);
    int *stack = (int *)malloc((size_t)s_g.n_nodes * sizeof(int));
    int comps = 0, biggest = 0, s;
    if (!seen || !stack) { free(seen); free(stack); *out_comps = 0; *out_biggest = 0; return; }
    for (s = 0; s < s_g.n_nodes; s++) {
        int sp = 0, n = 0;
        if (seen[s]) continue;
        comps++;
        stack[sp++] = s; seen[s] = 1;
        while (sp) {
            const int u = stack[--sp];
            int e;
            n++;
            for (e = s_g.adj_head[u]; e >= 0; e = s_g.edge[e].next)
                if (!seen[s_g.edge[e].to]) { seen[s_g.edge[e].to] = 1; stack[sp++] = s_g.edge[e].to; }
        }
        if (n > biggest) biggest = n;
    }
    free(seen); free(stack);
    *out_comps = comps; *out_biggest = biggest;
}

/* ======================================================================== *
 * SECTION: growable double-pair buffer
 * ======================================================================== */

typedef struct { double *x, *z; int n, cap; } GrPts;

static void gr_pts_free(GrPts *p) { free(p->x); free(p->z); p->x = p->z = NULL; p->n = p->cap = 0; }

/* [ROUND 1009 item 12] NORMALISED ARCLENGTH of each source vertex, 0..1.
 *
 * The conditioner samples the per-vertex lane count twice -- once as the WIDTH
 * the curvature floor is enforced for, once as the lane count STORED in
 * ROUTE.JSON -- and both used to go through an INDEX fraction (gr_at_frac).
 * That is only right when the OSM vertices are evenly spaced, and they are not:
 * the routed polyline for Mariano's La Plata route has 153 vertices over
 * 4.77 km, from 6 m at a junction to 180 m along a straight. Measured on that
 * route, the three-lane stretch of Calle 54 (way 271454277, OSM `lanes=3`)
 * landed on spans 458..511 while the road is at spans 671..700 -- 745 m early,
 * leaving span 688 on 2 lanes. That span is the picker ID in his note 12,
 * `level091 L91 e172 s1 p0 pos 956828,1842,-230883`.
 *
 * Sampling by ARCLENGTH puts every lane count where the road is. Both readers
 * go through this one table, which is the invariant gr_at_frac existed to hold:
 * a route whose width and whose stored lanes disagree reports "converged" while
 * carrying a node over its own limit. */
static double *gr_arc_frac(const GrPts *p)
{
    double *f;
    double tot = 0.0;
    int i;
    if (!p || p->n < 1) return NULL;
    f = (double *)malloc((size_t)p->n * sizeof(double));
    if (!f) return NULL;
    f[0] = 0.0;
    for (i = 1; i < p->n; i++) {
        tot += hypot(p->x[i] - p->x[i - 1], p->z[i] - p->z[i - 1]);
        f[i] = tot;
    }
    if (tot > 0.0) for (i = 0; i < p->n; i++) f[i] /= tot;
    else           for (i = 0; i < p->n; i++) f[i] = 0.0;
    return f;
}

static int gr_pts_push(GrPts *p, double x, double z)
{
    if (p->n >= p->cap) {
        const int c = p->cap ? p->cap * 2 : 256;
        double *nx = (double *)realloc(p->x, (size_t)c * sizeof(double));
        double *nz = (double *)realloc(p->z, (size_t)c * sizeof(double));
        if (nx) p->x = nx;
        if (nz) p->z = nz;
        if (!nx || !nz) return 0;
        p->cap = c;
    }
    p->x[p->n] = x; p->z[p->n] = z; p->n++;
    return 1;
}

/* ======================================================================== *
 * SECTION: the conditioner (geo_condition.py)
 * ======================================================================== */

/* geo_condition._resample_uniform. CHORD, not arc: the engine's walk integrates
 * x += sin(h)*span_len; z += cos(h)*span_len (td5_tg_road.c:663-664), so its
 * nodes are exactly one span apart in EUCLIDEAN distance. Arc-length resampling
 * falls short at a corner -- measured 34.6 units at a 31.6 degree turn on the
 * La Plata route, which geo_audit R4 failed on. */
static int gr_resample(const GrPts *in, double step, GrPts *out)
{
    int seg_i = 0;
    double t = 0.0;

    out->n = 0;
    if (in->n < 2) {
        int i;
        for (i = 0; i < in->n; i++) if (!gr_pts_push(out, in->x[i], in->z[i])) return 0;
        return 1;
    }
    if (!gr_pts_push(out, in->x[0], in->z[0])) return 0;

    while (seg_i < in->n - 1) {
        const double cx = out->x[out->n - 1], cz = out->z[out->n - 1];
        int placed = 0, j = seg_i;
        double tt = t;
        while (j < in->n - 1) {
            const double ax = in->x[j],  az = in->z[j];
            const double dx = in->x[j + 1] - ax, dz = in->z[j + 1] - az;
            const double seg = hypot(dx, dz);
            double ux, uz, fx, fz, bb, c0, disc;
            if (seg <= 1e-12) { j++; tt = 0.0; continue; }
            ux = dx / seg; uz = dz / seg;
            fx = ax - cx;  fz = az - cz;
            bb = fx * ux + fz * uz;
            c0 = fx * fx + fz * fz - step * step;
            disc = bb * bb - c0;
            if (disc >= 0.0) {
                const double root = sqrt(disc);
                int k;
                for (k = 0; k < 2; k++) {
                    const double s = k ? (-bb - root) : (-bb + root);
                    if (s >= tt - 1e-9 && s <= seg + 1e-9) {
                        if (!gr_pts_push(out, ax + ux * s, az + uz * s)) return 0;
                        seg_i = j; t = s > 0.0 ? s : 0.0;
                        placed = 1;
                        break;
                    }
                }
            }
            if (placed) break;
            j++; tt = 0.0;
        }
        if (!placed) break;
        if (out->n > GR_MAX_NODES_HARD) break;
    }
    return 1;
}

/* geo_condition._headings: the engine's convention, atan2(dx, dz). */
static double gr_heading(const GrPts *p, int i)
{
    return atan2(p->x[i + 1] - p->x[i], p->z[i + 1] - p->z[i]);
}

/* Signed heading change at interior vertex v (1 <= v <= n-2). */
static double gr_turn(const GrPts *p, int v)
{
    return gr_wrap(gr_heading(p, v) - gr_heading(p, v - 1));
}

/* geo_condition._principal_axis -- PCA on the vertices, reported not applied. */
static double gr_principal_axis(const GrPts *p)
{
    const int n = p->n;
    double mx = 0.0, mz = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0, theta;
    int i;
    for (i = 0; i < n; i++) { mx += p->x[i]; mz += p->z[i]; }
    mx /= (double)n; mz /= (double)n;
    for (i = 0; i < n; i++) {
        const double dx = p->x[i] - mx, dz = p->z[i] - mz;
        sxx += dx * dx; syy += dz * dz; sxy += dx * dz;
    }
    theta = 0.5 * atan2(2.0 * sxy, sxx - syy);
    return atan2(cos(theta), sin(theta));
}

static void gr_rotate(GrPts *p, double theta)
{
    const double c = cos(theta), s = sin(theta);
    int i;
    for (i = 0; i < p->n; i++) {
        const double x = p->x[i], z = p->z[i];
        p->x[i] = x * c - z * s;
        p->z[i] = x * s + z * c;
    }
}

typedef struct {
    double limit_deg, limit_loose_deg;
    double worst_before_deg, worst_after_deg, worst_final_deg;
    int    iterations, converged, over_nodes, over_nodes_final;
    double min_radius_units;
} GrCurv;

/* geo_condition.enforce_curvature's lim_at: the limit is LOCAL. Using the
 * widest road's limit everywhere would round off exactly the tight residential
 * corners that make a real city recognisable, so the width list is sampled by
 * NORMALISED POSITION and survives the re-resampling. [ROUND 1009 item 12] that
 * position is now the source polyline's ARCLENGTH fraction (`wfrac`, from
 * gr_arc_frac) rather than its index fraction -- see gr_arc_frac for why, and
 * note that the stored lane count goes through the same table, so the two
 * cannot drift apart. */
static double gr_lim_at(int i, int n, const double *widths,
                        const double *wfrac, int m, double step, int cs100)
{
    const double t = (n <= 1) ? 0.0 : (double)i / (double)(n - 1);
    return gr_max_turn_per_span(widths[gr_src_at(t, wfrac, m)], step, cs100);
}

/* geo_condition.enforce_curvature. Laplacian smoothing applied ONLY at
 * offending vertices (so legal stretches keep the real geometry), re-resampling
 * each pass to keep the spacing uniform. Endpoints are pinned. */
static int gr_enforce_curvature(GrPts *cur, const double *widths,
                                const double *wfrac, int m,
                                int cs100, double step, int max_iter,
                                GrCurv *rep)
{
    GrPts nxt, tmp;
    double wmax = widths[0], wmin = widths[0];
    double worst_before = 0.0;
    int it = 0, i, over = 0;

    memset(&nxt, 0, sizeof nxt);
    memset(&tmp, 0, sizeof tmp);
    for (i = 1; i < m; i++) {
        if (widths[i] > wmax) wmax = widths[i];
        if (widths[i] < wmin) wmin = widths[i];
    }
    rep->limit_deg       = gr_deg(gr_max_turn_per_span(wmax, step, cs100));
    rep->limit_loose_deg = gr_deg(gr_max_turn_per_span(wmin, step, cs100));
    rep->min_radius_units = gr_min_turn_radius(wmax, cs100);

    for (it = 1; it <= max_iter; it++) {
        const int n = cur->n;
        int all_ok = 1;
        double worst = 0.0;
        if (n < 3) break;
        for (i = 1; i <= n - 2; i++) {
            const double t = fabs(gr_turn(cur, i));
            if (t > worst) worst = t;
            if (t > gr_lim_at(i, n, widths, wfrac, m, step, cs100) + 1e-6) all_ok = 0;
        }
        if (it == 1) worst_before = worst;
        if (all_ok) break;

        nxt.n = 0;
        for (i = 0; i < n; i++) if (!gr_pts_push(&nxt, cur->x[i], cur->z[i])) goto oom;
        for (i = 1; i <= n - 2; i++) {
            double ax, az, bx, bz, px, pz;
            if (fabs(gr_turn(cur, i)) <= gr_lim_at(i, n, widths, wfrac, m, step, cs100))
                continue;
            ax = cur->x[i - 1]; az = cur->z[i - 1];
            bx = cur->x[i + 1]; bz = cur->z[i + 1];
            px = cur->x[i];     pz = cur->z[i];
            /* 0.5 converges fast without oscillating; the re-resample below
             * repairs the spacing it disturbs. */
            nxt.x[i] = px + 0.5 * ((ax + bx) * 0.5 - px);
            nxt.z[i] = pz + 0.5 * ((az + bz) * 0.5 - pz);
        }
        nxt.x[0] = cur->x[0];         nxt.z[0] = cur->z[0];
        nxt.x[n - 1] = cur->x[n - 1]; nxt.z[n - 1] = cur->z[n - 1];
        if (!gr_resample(&nxt, step, &tmp)) goto oom;
        cur->n = 0;
        for (i = 0; i < tmp.n; i++) if (!gr_pts_push(cur, tmp.x[i], tmp.z[i])) goto oom;
    }
    if (it > max_iter) it = max_iter;

    {
        const int n = cur->n;
        double worst_after = 0.0;
        for (i = 1; i <= n - 2; i++) {
            const double t = fabs(gr_turn(cur, i));
            if (t > worst_after) worst_after = t;
            if (t > gr_lim_at(i, n, widths, wfrac, m, step, cs100) + 1e-6) over++;
        }
        rep->worst_before_deg = gr_deg(worst_before);
        rep->worst_after_deg  = gr_deg(worst_after);
    }
    rep->iterations = it;
    rep->over_nodes = over;
    rep->converged  = !over;
    gr_pts_free(&nxt); gr_pts_free(&tmp);
    return 1;
oom:
    gr_pts_free(&nxt); gr_pts_free(&tmp);
    return 0;
}

/* --------------------------------------------------------- crossings ----- */

typedef struct {
    int    a_first, a_last, b_first, b_last;
    int    a_lo, a_hi, b_lo, b_hi;
    int    pairs;
    double worst_distance_units;
    double need_units;
    int    worst_a, worst_b;
    int    grade_separated;
    int    grade_separation_planned;
    int    over_is_b;
} GrSite;

typedef struct {
    int    site;
    int    over_lo, over_hi, under_lo, under_hi;
    int    over_is_b;
    int    ramp_spans, ramp_spans_wanted, ramp_room_spans;
    double clearance_units, clearance_m;
    int    buildable;
    int    tie;
} GrXSep;

/* geo_condition.find_crossings + merge_crossings, fused.
 *
 * find_crossings emits pairs with i ascending then j ascending, which is
 * exactly the order merge_crossings sorts into, so the two can run as one pass
 * and the grouping is identical. Fusing them also removes the only unbounded
 * allocation in the pipeline: a bad route can produce hundreds of thousands of
 * offending pairs, and only the merged SITES are ever reported. */
static int gr_crossings(const GrPts *p, const double *widths, double lane_w,
                        int skip, GrSite *sites, int max_sites, int *out_n)
{
    const int n = p->n;
    int i, ns = 0;

    for (i = 0; i < n; i++) {
        const double xi = p->x[i], zi = p->z[i], wi = widths[i];
        int j;
        for (j = i + skip; j < n; j++) {
            const double need = gr_too_close_need(wi, widths[j], lane_w);
            const double dx = p->x[j] - xi, dz = p->z[j] - zi;
            const double d2 = dx * dx + dz * dz;
            double d;
            int g, joined = 0;
            if (d2 >= need * need) continue;
            d = sqrt(d2);
            for (g = 0; g < ns; g++) {
                GrSite *s = &sites[g];
                if (abs(i - s->a_last) <= GR_MERGE_SPAN_TOL &&
                    abs(j - s->b_last) <= GR_MERGE_SPAN_TOL) {
                    s->pairs++;
                    s->a_last = i; s->b_last = j;
                    if (i < s->a_lo) s->a_lo = i;
                    if (i > s->a_hi) s->a_hi = i;
                    if (j < s->b_lo) s->b_lo = j;
                    if (j > s->b_hi) s->b_hi = j;
                    if (d < s->worst_distance_units) {
                        s->worst_distance_units = d;
                        s->worst_a = i; s->worst_b = j;
                    }
                    joined = 1;
                    break;
                }
            }
            if (joined) continue;
            if (ns >= max_sites) return 0;      /* too many to report usefully */
            {
                GrSite *s = &sites[ns++];
                memset(s, 0, sizeof *s);
                s->a_first = s->a_last = s->a_lo = s->a_hi = i;
                s->b_first = s->b_last = s->b_lo = s->b_hi = j;
                s->pairs = 1;
                s->worst_distance_units = d;
                s->need_units = need;
                s->worst_a = i; s->worst_b = j;
            }
        }
    }
    *out_n = ns;
    return 1;
}

/* geo_condition.plan_grade_separations. See the Python for the full argument;
 * the rule in brief: a leg that cannot hold a ramp is not eligible, of the
 * eligible legs the one with MORE ramp room goes over, ties go to the LATER
 * leg. It draws no random number -- a layout that moved between two builds of
 * the same route would defeat the determinism gate. */
static int gr_plan_grade_seps(GrSite *sites, int ns, int n_nodes,
                              double span_length, double max_grade,
                              double lift_units, double upm,
                              GrXSep *out, int max_out)
{
    const int last = n_nodes - 1;
    const double budget = max_grade * GR_XSEP_RAMP_FRAC * span_length;
    const int ramp_min = (budget > 0.0) ? (int)ceil(lift_units / budget) : 0;
    int idx, n = 0;

    if (n_nodes < 2) return 0;
    for (idx = 0; idx < ns; idx++) {
        GrSite *g = &sites[idx];
        int lo[2], hi[2], room[2], k, over, under, ramp, deck, ok;
        if (g->grade_separated) continue;
        lo[0] = g->a_lo; hi[0] = g->a_hi;
        lo[1] = g->b_lo; hi[1] = g->b_hi;
        for (k = 0; k < 2; k++) {
            const int o = 1 - k;
            int back_stop = GR_XSEP_FIRST_SPAN, fwd_stop = last, r1, r2;
            if (hi[o] < lo[k] && hi[o] + 1 > back_stop) back_stop = hi[o] + 1;
            if (lo[o] > hi[k] && lo[o] - 1 < fwd_stop)  fwd_stop  = lo[o] - 1;
            r1 = lo[k] - back_stop;
            r2 = fwd_stop - hi[k];
            room[k] = r1 < r2 ? r1 : r2;
        }
        /* Rule 2 then rule 3 (leg b is always the later one: the crossing scan
         * only emits pairs with b > a). */
        over  = (room[1] >= room[0]) ? 1 : 0;
        under = 1 - over;
        ramp  = ramp_min < (room[over] > 0 ? room[over] : 0)
                    ? ramp_min : (room[over] > 0 ? room[over] : 0);
        deck  = hi[over] - lo[over] + 1;
        ok    = (room[over] > 0 && ramp > 0 && deck + 2 * ramp <= GR_XSEP_BRIDGE_MAX * 2);
        if (n < max_out) {
            GrXSep *x = &out[n++];
            x->site = idx;
            x->over_lo = lo[over];   x->over_hi = hi[over];
            x->under_lo = lo[under]; x->under_hi = hi[under];
            x->over_is_b = over;
            x->ramp_spans = ramp;
            x->ramp_spans_wanted = ramp_min;
            x->ramp_room_spans = room[over];
            x->clearance_units = lift_units;
            x->clearance_m = lift_units / upm;
            x->buildable = ok;
            x->tie = (room[over] == room[under]);
        }
        g->grade_separation_planned = ok;
        g->over_is_b = over;
    }
    return n;
}

typedef struct {
    double max_dev_deg, mean_dev_deg, monotone_frac;
    int    over_budget, inherits_proof;
} GrScore;

/* geo_condition._score -- how close this orientation comes to inheriting the
 * engine's no-crossing proof. */
static void gr_score(const GrPts *p, GrScore *s)
{
    int i, nh = p->n - 1, over = 0, fwd = 0;
    double mx = 0.0, sum = 0.0;
    memset(s, 0, sizeof *s);
    if (nh < 1) { s->monotone_frac = 1.0; s->inherits_proof = 1; return; }
    for (i = 0; i < nh; i++) {
        const double d = fabs(gr_wrap(gr_heading(p, i) - M_PI / 2.0));
        if (d > mx) mx = d;
        sum += d;
        if (d > GR_HEADING_LIMIT) over++;
        if (p->x[i + 1] > p->x[i]) fwd++;
    }
    s->max_dev_deg    = gr_deg(mx);
    s->mean_dev_deg   = gr_deg(sum / (double)nh);
    s->over_budget    = over;
    s->monotone_frac  = (double)fwd / (double)nh;
    s->inherits_proof = (over == 0);
}

typedef struct { int clamped_low, clamped_high, clamped_total; double worst_deg; } GrHB;

/* geo_condition.heading_byte_report. ROUTES.DAT byte 1 is an ABSOLUTE 12-bit
 * heading clamped to 4..253 (`byte < 4` is a junction-zone sentinel), so a
 * route that wanders through +Z stores a heading up to ~5.7 deg wrong for the
 * AI. Reported, not rejected: the geometry is unaffected. */
static void gr_heading_bytes(const GrPts *p, GrHB *o)
{
    const double lo_deg = 4.0 * 4140.0 / 256.0 * 360.0 / 4096.0;
    int i;
    memset(o, 0, sizeof *o);
    for (i = 0; i < p->n - 1; i++) {
        const double h = gr_heading(p, i);
        const int h12 = ((int)gr_round_even(h * 4096.0 / (2.0 * M_PI))) & 0xFFF;
        const int hb  = (int)gr_round_even((double)h12 * 256.0 / 4140.0);
        double err;
        if (hb < 4) {
            o->clamped_low++;
            err = fabs(lo_deg - (double)h12 * 360.0 / 4096.0);
            if (err > o->worst_deg) o->worst_deg = err;
        } else if (hb > 253) {
            o->clamped_high++;
            err = fabs((double)h12 * 360.0 / 4096.0
                       - 253.0 * 4140.0 / 256.0 * 360.0 / 4096.0);
            if (err > o->worst_deg) o->worst_deg = err;
        }
    }
    o->clamped_total = o->clamped_low + o->clamped_high;
}

/* ------------------------------------------------------- condition_route --- */

#define GR_REASON_MAX 4

typedef struct {
    int      ok;
    int      n_reasons;
    char     reason[GR_REASON_MAX][TD5_GEO_ROUTE_WARN_LEN];
    int      n_warnings;
    char     warning[TD5_GEO_ROUTE_MAX_WARN][TD5_GEO_ROUTE_WARN_LEN];
    int      reversed;
    int      spans, nodes;
    double   length_km;
    double   span_length, lane_width, units_per_metre;
    int      curve_safety_x100, adjacent_skip, lead_in_nodes;
    GeoProj  proj;                 /* the CONDITIONED frame */
    double   offset_x, offset_z;
    double   principal_axis_dev_deg;
    GrScore  orientation, final;
    GrCurv   curvature;
    GrHB     heading;
    GrPts    nodes_xz;
    int     *lanes_out;
    int      n_sites;
    GrSite  *sites;
    int      n_level;
    int      n_xsep;
    GrXSep  *xsep;
    double   fwd_max_dev_deg, rev_max_dev_deg;   /* the orientation sort keys */
} GrCond;

static void gr_cond_free(GrCond *c)
{
    gr_pts_free(&c->nodes_xz);
    free(c->lanes_out); free(c->sites); free(c->xsep);
    c->lanes_out = NULL; c->sites = NULL; c->xsep = NULL;
}

static void gr_add_reason(GrCond *c, const char *fmt, ...)
{
    va_list ap;
    if (c->n_reasons >= GR_REASON_MAX) return;
    va_start(ap, fmt);
    vsnprintf(c->reason[c->n_reasons], TD5_GEO_ROUTE_WARN_LEN, fmt, ap);
    va_end(ap);
    c->n_reasons++;
}

static void gr_add_warning(GrCond *c, const char *fmt, ...)
{
    va_list ap;
    if (c->n_warnings >= TD5_GEO_ROUTE_MAX_WARN) return;
    va_start(ap, fmt);
    vsnprintf(c->warning[c->n_warnings], TD5_GEO_ROUTE_WARN_LEN, fmt, ap);
    va_end(ap);
    c->n_warnings++;
}

/* geo_condition.condition_route. Raw (lat, lon) -> a TD5-legal centerline plus
 * a verdict. `allow_crossings` asserts the engine's crossing-safe localiser is
 * armed, so a LEVEL self-crossing stops being fatal. */
static int gr_condition(const TD5_GeoLatLon *ll, const int *lanes_in, int n_in,
                        double upm, int cs100, double span_length,
                        double lane_width, int allow_reverse, int allow_crossings,
                        GrCond *out)
{
    GeoProj proj;
    GrPts metric, cand, pts;
    double lat0 = 0.0, lon0 = 0.0, theta = 0.0, pca_deg, off_x, off_z, fix, h0;
    int *lane_src = NULL;
    double *widths_src = NULL, *widths = NULL, *src_frac = NULL;
    int i, reversed = 0, lead, n_lead, n_body, n_nodes, skip;
    GrScore sc_f, sc_r, orient;

    memset(&sc_f, 0, sizeof sc_f);
    memset(&sc_r, 0, sizeof sc_r);

    memset(out, 0, sizeof *out);
    memset(&metric, 0, sizeof metric);
    memset(&cand, 0, sizeof cand);
    memset(&pts, 0, sizeof pts);

    if (n_in < 2) {
        gr_add_reason(out, "route has fewer than 2 points");
        return 0;
    }
    for (i = 0; i < n_in; i++) { lat0 += ll[i].lat; lon0 += ll[i].lon; }
    lat0 /= (double)n_in; lon0 /= (double)n_in;
    gr_proj_init(&proj, lat0, lon0, upm);
    for (i = 0; i < n_in; i++) {
        double x, z;
        gr_proj_to_world(&proj, ll[i].lat, ll[i].lon, &x, &z);   /* rotation identity */
        if (!gr_pts_push(&metric, x, z)) goto oom;
    }

    lane_src = (int *)malloc((size_t)n_in * sizeof(int));
    if (!lane_src) goto oom;
    for (i = 0; i < n_in; i++) lane_src[i] = lanes_in ? lanes_in[i] : 2;

    /* -- 1. orientation --------------------------------------------------
     * Span 0 runs along +X (TD5_TG_AXIS_HEADING, and the walk does
     * x += sin(heading)), so the route's START TANGENT must be +X. That uses
     * up the rotational freedom; the only remaining choice is which end is the
     * start.
     *
     * [ROUND 1009 item 1, follow-up] `allow_reverse` is what makes that a
     * CHOICE, and the in-game path no longer allows it. geo_condition.py picks
     * whichever end scores better against the generator's +X axis, and on
     * Mariano's La Plata route that was the far end: the race started at his
     * SECOND click and finished at his first. For a route the user drew, START
     * is not a scoring input, it is the thing he said. The screen's path passes
     * allow_reverse = 0 (TD5RE_GEO_START_AT_CLICK=0 restores the old choice);
     * the fixture harness keeps passing 1, because that half is the Python
     * parity test and reproducing geo_condition.py is its whole job.
     *
     * BOTH orientations are still SCORED whatever the flag says, so the result
     * can report that the other way round would have fitted the axis better
     * rather than silently taking the worse one. */
    {
        double th_f, th_r;
        GrPts rot_f, rot_r;
        memset(&rot_f, 0, sizeof rot_f);
        memset(&rot_r, 0, sizeof rot_r);
        for (i = 0; i < metric.n; i++) if (!gr_pts_push(&rot_f, metric.x[i], metric.z[i])) goto oom;
        /* SIGN: _rotate maps a heading phi to phi - theta, so carrying h0 onto
         * +X (pi/2) needs theta = h0 - pi/2, NOT pi/2 - h0. This was the wrong
         * way round once and silently measured the whole orientation score in a
         * frame rotated by twice the start deviation. */
        th_f = gr_heading(&rot_f, 0) - M_PI / 2.0;
        gr_rotate(&rot_f, th_f);
        gr_score(&rot_f, &sc_f);

        for (i = metric.n - 1; i >= 0; i--)
            if (!gr_pts_push(&rot_r, metric.x[i], metric.z[i])) goto oom;
        th_r = gr_heading(&rot_r, 0) - M_PI / 2.0;
        gr_rotate(&rot_r, th_r);
        gr_score(&rot_r, &sc_r);

        if (allow_reverse) {
            /* Prefer an orientation that inherits the proof outright; then
             * fewest spans over budget; then the smallest worst deviation.
             * Python sorts a 2-element list, so forward wins every tie. */
            reversed = 0;
            if ((!sc_f.inherits_proof) != (!sc_r.inherits_proof))
                reversed = sc_r.inherits_proof;
            else if (sc_r.over_budget != sc_f.over_budget)
                reversed = sc_r.over_budget < sc_f.over_budget;
            else
                /* EPSILON, and it is not cosmetic. The last key is a physical
                 * angle, and on a symmetric route the two orientations are the
                 * same shape: the figure-eight fixture ties at 1.1e-12 deg, so
                 * the winner is decided by which library rounded last. Python
                 * sorts a 2-element list and its sort is stable, so forward
                 * wins its tie; a bare `<` here flipped the answer and drove
                 * the whole track backwards. A nanodegree is not a preference. */
                reversed = (sc_r.max_dev_deg < sc_f.max_dev_deg - GR_ORIENT_TIE_DEG);
        } else if (sc_r.over_budget < sc_f.over_budget) {
            /* Kept FORWARD on purpose. Say what it cost, in the units the
             * screen already shows, so a route that fights the axis is visible
             * rather than mysterious. */
            gr_add_warning(out, "driven from START as placed; the reverse "
                           "direction fits the track axis better (%d span(s) "
                           "over the heading budget against %d)",
                           sc_r.over_budget, sc_f.over_budget);
        }
        if (reversed) {
            theta = th_r; orient = sc_r;
            for (i = 0; i < rot_r.n; i++) if (!gr_pts_push(&cand, rot_r.x[i], rot_r.z[i])) goto oom;
            for (i = 0; i < n_in / 2; i++) {
                const int t = lane_src[i];
                lane_src[i] = lane_src[n_in - 1 - i];
                lane_src[n_in - 1 - i] = t;
            }
        } else {
            theta = th_f; orient = sc_f;
            for (i = 0; i < rot_f.n; i++) if (!gr_pts_push(&cand, rot_f.x[i], rot_f.z[i])) goto oom;
        }
        out->fwd_max_dev_deg = sc_f.max_dev_deg;
        out->rev_max_dev_deg = sc_r.max_dev_deg;
        gr_pts_free(&rot_f); gr_pts_free(&rot_r);
    }
    gr_proj_rot(&proj, theta);
    pca_deg = gr_deg(gr_wrap(gr_principal_axis(&cand) - M_PI / 2.0));

    /* -- 2. uniform spacing ---------------------------------------------- */
    if (!gr_resample(&cand, span_length, &pts)) goto oom;

    /* -- 3. curvature floor ---------------------------------------------- */
    widths_src = (double *)malloc((size_t)n_in * sizeof(double));
    if (!widths_src) goto oom;
    for (i = 0; i < n_in; i++) widths_src[i] = (double)lane_src[i] * lane_width;
    /* [ROUND 1009 item 12] `cand` is the source polyline in this frame and in
     * lane_src's order (the reversal above flips both together), so its
     * arclength is the table BOTH width readers index through. */
    src_frac = gr_arc_frac(&cand);
    if (!gr_enforce_curvature(&pts, widths_src, src_frac, n_in, cs100,
                              span_length, 200, &out->curvature)) goto oom;

    /* -- 3b. RE-ALIGN the start tangent ----------------------------------
     * Smoothing moves interior points and the re-resample shifts every sample,
     * so the first segment no longer points exactly along +X, which is the
     * heading the generator's TD5_TG_AXIS_HEADING assumes for span 0. Left
     * uncorrected it was a kink at the old lead-in join -- a 97.2 degree turn
     * at node 40 on the La Plata route, invisible to the convergence check.
     * With no lead-in (the default since round 1009) there is no join, and
     * this is simply what pins the frame to the real road's first span. */
    if (pts.n >= 2) {
        h0 = gr_heading(&pts, 0);
        fix = h0 - M_PI / 2.0;
        if (fabs(gr_wrap(fix)) > 1e-9) {
            gr_rotate(&pts, fix);
            theta += fix;
            gr_proj_rot(&proj, theta);
        }
    }

    /* -- 4. lead-in + origin ----------------------------------------------
     * `lead` is 0 by default: node 0 IS the first routed vertex, so the track
     * starts on the real road rather than on invented tarmac. See
     * gr_lead_in_nodes(). With lead > 0 the old synthetic straight comes back
     * and node lead-1 is where the real road joins. */
    lead = gr_lead_in_nodes();
    n_lead = lead + 1;
    off_x = (double)lead * span_length - pts.x[0];
    off_z = 0.0 - pts.z[0];
    n_body = pts.n - 1;
    n_nodes = n_lead + n_body;
    for (i = 0; i < n_lead; i++)
        if (!gr_pts_push(&out->nodes_xz, (double)i * span_length, 0.0)) goto oom;
    for (i = 1; i < pts.n; i++)
        if (!gr_pts_push(&out->nodes_xz, pts.x[i] + off_x, pts.z[i] + off_z)) goto oom;
    /* The cache MUST carry this translation as well as the rotation. geo_audit
     * R8 caught the omission: with only the rotation shared, 896 of 1451 route
     * nodes fell outside their own terrain. */
    gr_proj_off(&proj, off_x, off_z);

    out->lanes_out = (int *)malloc((size_t)n_nodes * sizeof(int));
    widths         = (double *)malloc((size_t)n_nodes * sizeof(double));
    if (!out->lanes_out || !widths) goto oom;
    /* Node i holds pts[i - (n_lead-1)]; a lead node (negative index) takes the
     * road's first lane count. The fraction is the stored node's own position
     * along the body, which is what gr_src_at turns into a source vertex --
     * the same table gr_enforce_curvature just used for the width. */
    for (i = 0; i < n_nodes; i++) {
        const int p = i - (n_lead - 1);
        const double f = (pts.n <= 1) ? 0.0
                       : (double)(p < 0 ? 0 : p) / (double)(pts.n - 1);
        out->lanes_out[i] = lane_src[gr_src_at(f, src_frac, n_in)];
        widths[i] = (double)out->lanes_out[i] * lane_width;
    }

    /* Final verification against the widths actually STORED, which is the same
     * test geo_audit R5 runs. Reported rather than silently re-smoothed. */
    out->curvature.over_nodes_final = 0;
    out->curvature.worst_final_deg  = 0.0;
    for (i = 1; i < n_nodes - 1; i++) {
        const double dd = fabs(gr_turn(&out->nodes_xz, i));
        if (gr_deg(dd) > out->curvature.worst_final_deg)
            out->curvature.worst_final_deg = gr_deg(dd);
        if (dd > gr_max_turn_per_span(widths[i], span_length, cs100) + 1e-6)
            out->curvature.over_nodes_final++;
    }
    out->curvature.converged = (out->curvature.over_nodes_final == 0);

    /* -- 5. crossings + cap ------------------------------------------------ */
    out->spans = n_nodes - 1;
    out->nodes = n_nodes;
    skip = gr_adjacent_skip(lane_width, span_length, cs100);
    out->sites = (GrSite *)malloc((size_t)GR_MAX_SITES * sizeof(GrSite));
    out->xsep  = (GrXSep *)malloc((size_t)TD5_GEO_XSEP_MAX * sizeof(GrXSep));
    if (!out->sites || !out->xsep) goto oom;
    out->n_sites = 0;
    /* DEVIATION (a) from the Python: a route already over the span cap is
     * refused whatever the crossing scan says, and the scan is O(n^2) on a node
     * count that has no upper bound here. Skip it and say so. */
    if (out->spans <= GR_MAX_SPANS) {
        if (!gr_crossings(&out->nodes_xz, widths, lane_width, skip,
                          out->sites, GR_MAX_SITES, &out->n_sites)) {
            out->n_sites = GR_MAX_SITES;
            gr_add_warning(out, "more than %d self-crossing site(s); only the "
                           "first %d are reported", GR_MAX_SITES, GR_MAX_SITES);
        }
    }
    /* classify_crossings with no `layer` column and no DEM: geo_route.py does
     * not carry the OSM layer tag and the selector passes no HEIGHT.R16, so
     * nothing can be PROVEN grade-separated. Read a "not grade separated"
     * verdict as "could not prove separation", never as "proven level". */
    out->n_level = out->n_sites;
    /* [OPTION B] Plan the lift for the level sites unconditionally: the plan is
     * a fact about the route's geometry, and computing it even for a route that
     * is going to be REJECTED is what lets the screen say "this crossing can be
     * built as a flyover" instead of only "this crossing is fatal". */
    out->n_xsep = gr_plan_grade_seps(out->sites, out->n_sites, n_nodes,
                                     span_length,
                                     (double)GR_MAX_GRADE_X1000 / 1000.0,
                                     GR_XSEP_LIFT_UNITS, upm,
                                     out->xsep, TD5_GEO_XSEP_MAX);

    if (out->spans > GR_MAX_SPANS)
        gr_add_reason(out, "route is %d spans, over the %d cap", out->spans, GR_MAX_SPANS);
    /* A track must hold a grid, a race and a run-off. RUN-OFF defaults to 100
     * spans (TD5RE_AUTOTRACK_RUNOFF, td5_fe_race.c:7286). */
    if (out->spans < lead + GR_GRID_SPAN + 100 + 50)
        gr_add_reason(out, "route is %d spans, too short to hold a grid, a race "
                      "and a run-off", out->spans);
    if (out->n_level && !allow_crossings)
        gr_add_reason(out, "%d LEVEL self-crossing site(s): drag a waypoint to "
                      "resolve them", out->n_level);
    if (!out->curvature.converged)
        gr_add_reason(out, "curvature smoothing did not converge (worst turn "
                      "%.1f deg vs limit %.1f)", out->curvature.worst_after_deg,
                      out->curvature.limit_deg);

    gr_score(&out->nodes_xz, &out->final);
    gr_heading_bytes(&out->nodes_xz, &out->heading);
    if (out->heading.clamped_total)
        gr_add_warning(out, "%d span(s) point within the route-byte dead zone "
                       "near +Z; their stored heading is up to %.1f deg wrong "
                       "for the AI (geometry is unaffected)",
                       out->heading.clamped_total, out->heading.worst_deg);
    if (out->n_level && allow_crossings)
        gr_add_warning(out, "%d LEVEL self-crossing site(s) ACCEPTED: this route "
                       "needs the engine's crossing-safe localiser",
                       out->n_level);
    if (out->n_xsep) {
        int built = 0;
        for (i = 0; i < out->n_xsep; i++) if (out->xsep[i].buildable) built++;
        gr_add_warning(out, "%d of %d level site(s) get a BUILT grade separation "
                       "(%.1f m of clearance)", built, out->n_xsep,
                       GR_XSEP_LIFT_UNITS / upm);
    }

    out->ok = (out->n_reasons == 0);
    out->reversed = reversed;
    out->length_km = (double)out->spans * span_length / upm / 1000.0;
    out->span_length = span_length;
    out->lane_width = lane_width;
    out->units_per_metre = upm;
    out->curve_safety_x100 = cs100;
    out->adjacent_skip = skip;
    out->lead_in_nodes = lead;
    out->proj = proj;
    out->offset_x = off_x;
    out->offset_z = off_z;
    out->principal_axis_dev_deg = pca_deg;
    out->orientation = orient;

    free(lane_src); free(widths_src); free(widths); free(src_frac);
    gr_pts_free(&metric); gr_pts_free(&cand); gr_pts_free(&pts);
    return 1;
oom:
    free(lane_src); free(widths_src); free(widths); free(src_frac);
    gr_pts_free(&metric); gr_pts_free(&cand); gr_pts_free(&pts);
    gr_cond_free(out);
    memset(out, 0, sizeof *out);
    gr_add_reason(out, "out of memory conditioning the route");
    return 0;
}

/* ======================================================================== *
 * SECTION: divided avenues  (the C port of re/tools/geo_forks.detect_medians
 *          plus the span half of geo_selector.resolve_forks)
 * ======================================================================== *
 *
 * [ROUND 1009 item 5] "I selected an avenue with 2 lanes; by default avenues
 * should be part of the track with their branches logic."
 *
 * OSM splits a dual carriageway into TWO one-way ways carrying the SAME name a
 * few metres apart. The router drives one of them, so the route's own lane
 * count is that carriageway's (2 on Diagonal 73, 1 before geo_fetch's floor)
 * and the track used to be built as a plain two-lane road with the other
 * carriageway, the median and the divider all missing. The generator already
 * has the right shape for it -- TG_FORK_ISLAND, which splits the road at F and
 * gives it a central divider -- and td5_geo_forks.c already reads a FORKS.JSON
 * table of them. What was missing is that the table only ever came from
 * re/tools/geo_selector.py; the in-game commit wrote no forks and deleted a
 * stale file. This section is that missing half.
 *
 * MEASURED on La Plata's 2291 ways: 29 named avenues are divided (358
 * anti-parallel one-way pairs), and Mariano's route runs on 10 of them,
 * including 5511 m of Diagonal 73. `median` is false on all 2291 records and
 * `junction` is only ever circular/roundabout, so NEITHER tag detects it --
 * the geometry below is what does the work, exactly as geo_forks.py found.
 *
 * TWO INDEX MAPPINGS, and they are deliberately different. The span range is
 * read by ARCLENGTH (where the avenue is on the ground). The lane window is
 * written through the conditioner's own source mapping (gr_src_at) because
 * that is what decides which stored node gets which lane count. Since round
 * 1009 both are arclength, so they agree -- but the achieved range is still
 * read BACK off the conditioned lanes (gr_fit_fork) rather than assumed,
 * because that is what makes tg_fork_place's uniformity check pass by
 * construction instead of by luck.
 *
 * TD5RE_GEO_AVENUES=0 pins the old behaviour (no detection, no FORKS.JSON) for
 * a single-variable A/B. */

/* geo_forks.py MEDIAN_MIN_M / MEDIAN_MAX_M -- the lateral gap between the two
 * carriageways. Below 4 m they are the same road drawn twice; 45 m is above La
 * Plata's widest boulevard (~30 m between carriageway centrelines) and below
 * one city block (110 m), so a PARALLEL STREET one block over can never be
 * mistaken for a median. */
#define GR_MED_MIN_M          4.0
#define GR_MED_MAX_M         45.0
/* geo_forks.py MEDIAN_ANTIPARALLEL_TOL_DEG / MEDIAN_PARALLEL_TOL_DEG. 35 deg
 * covers the divergence at junctions and the bend of a diagonal without
 * admitting a crossing street. A same-name way running PARALLEL at median
 * distance is a service road or a bus lane, accepted only when itself
 * one-way -- then it is still a separate carriageway. */
#define GR_MED_ANTI_TOL_DEG  35.0
#define GR_MED_PARA_TOL_DEG  25.0
/* geo_forks.py MEDIAN_MIN_COVER. Fraction of the run that has to find a
 * partner, as a UNION over every same-name way. Measured union coverage on the
 * reference route: 0.00 for the three undivided streets, 0.44 for the one
 * partly-mapped case, 0.55..1.00 for everything divided on the ground, so 0.50
 * sits in the 0.44 -> 0.55 gap. */
#define GR_MED_MIN_COVER      0.50
/* geo_forks.py MEDIAN_MIN_LEN_M -- shorter than one La Plata block (110 m)
 * plus its intersection is a junction artefact. */
#define GR_MED_MIN_LEN_M    120.0
/* [ROUND 1010 AVENUES] the fork-placement pads (GR_FORK_WIDEN_PAD,
 * GR_FORK_TAIL_PAD, GR_FORK_MIN_F, GR_FORK_RING_TAIL, GR_FORK_ISLAND_MIN) went
 * with gr_fit_fork: a median is not placed as a fork any more, so there is no
 * approach to widen and no corridor to rejoin. The two margins a span-indexed
 * sidecar still owes the engine -- clear of the start grid, clear of the ring's
 * tail -- are applied directly where the span range is clamped. */
/* TD5_TG_BRANCH_MAX. */
#define GR_FORK_MAX           8
/* Runs are merged before scoring; a route cannot have more than this many. */
#define GR_MED_MAX_RUNS     512
/* [ROUND 1010 AVENUES] the STABILITY gate. See gr_detect_medians for the
 * measurement that puts these two numbers in the gap between Diagonal 73
 * (100 % one side, spread 1.00..1.28) and Calle 14 / Calle 54 (82 % / 75 %,
 * spread 5.86 / 3.69). The run is sampled at most 12 times plus the end, so
 * 16 slots covers every run the sampler can produce. */
#define GR_MED_MAX_SAMP      16
#define GR_MED_SIDE_FRAC    0.90
#define GR_MED_SPREAD_MAX    2.0

typedef struct {
    int    k0, k1;            /* raw route vertex range, inclusive, FORWARD */
    int    lanes;             /* lanes(A) + lanes(B), clamped to 4..8 */
    int    F, len;            /* placed span range; filled by gr_place_forks */
    double sep;
    double length_m, gap_m, cover;
    /* [ROUND 1010 AVENUES] which side of travel the OPPOSITE carriageway is on
     * (+1 = left, -1 = right), and the lane count of the peer carriageway. A
     * real median keeps ONE side for the whole run; see gr_detect_medians. */
    int    side;
    int    peer_lanes;
    double gap_min_m, gap_max_m;   /* the run's real spread, for the log */
    char   name[64];
    char   id[160];
    char   source[80];
    char   detail[160];
} GrMedian;

/* [ROUND 1010 AVENUES] one conditioned span of a divided avenue, sampled off
 * the REAL polylines.
 *
 *   off    signed lateral offset from the race centreline to the OPPOSITE
 *          carriageway's centreline, in WORLD UNITS, positive to the left of
 *          travel -- the same sign tg_road_edge's `shift` takes, so the
 *          generator can use it as a lateral shift with no conversion.
 *   lanes  the opposite carriageway's OWN lane count there.
 *   open   a real cross street cuts the median here, so it must have a gap.
 *
 * It is a per-SPAN table rather than a copy of the peer polyline because the
 * conditioner SMOOTHS the centreline: a polyline laid in absolute coordinates
 * would drift into (or away from) the race road wherever the smoothing moved
 * it, while an offset measured from the real geometry and applied to the built
 * centreline keeps the median beside the road it divides and still narrows,
 * widens and ends exactly where the OSM ways do. */
typedef struct {
    int    av;                /* index into s_last.av[]  */
    int    span;
    double off;
    int    lanes;
    int    open;
} GrAvSpan;

/* A route is capped at GR_MAX_SPANS (3000); an avenue table covering a third of
 * a maximal route is already far past anything La Plata produces (Mariano's
 * 1066-span route spends ~290 spans on Diagonal 73). */
#define GR_AV_MAX_SPANS    1024
/* A median shorter than this is a junction artefact to a driver: 12 spans is
 * 18000 units, 42 m, about a third of a La Plata block. Below it the island
 * would pop in and out between two crossings, which is the complaint the
 * synthetic sliver reject (tg_emit_avenue_divider) exists to avoid. */
#define GR_AV_MIN_SPANS      12
/* CONTINUITY: the most the offset to the opposite carriageway may change from
 * one span to the next. A span is 1500 units (3.5 m) of road, so 375 units
 * (0.87 m) is a 14-degree splay -- far more than any real median taper (the
 * widest on Mariano's route opens 3.3 m over ~50 spans, 0.07 m per span) and
 * far less than the jump to a different slice of the same street, which is
 * what this exists to refuse. See gr_peer_offset_at for the measurement. */
#define GR_AV_MAX_STEP      375.0
/* The two carriageways must not touch: the gap has to hold both half widths
 * and still leave something between them. 150 units is 0.35 m -- enough that
 * the two road surfaces never share an edge and z-fight, small enough that a
 * real median pinching at an intersection does not cut the avenue in half
 * (the island itself is skipped under TG_AV_MIN_MEDIAN_W, which is wider). */
#define GR_AV_MIN_MEDIAN    150.0
/* How many consecutive unmapped spans the chain may cross before the avenue is
 * declared over. 4 spans is 6000 units, 14 m -- an intersection's width. OSM
 * interrupts a carriageway at a junction, and ending the avenue at the first
 * such gap would cut a long one in half at its first crossing. */
#define GR_AV_MAX_MISS         4

/* geo_forks._bearing: atan2 of (dx, dz), so 0 is +Z and pi/2 is +X -- the same
 * convention gr_heading uses. */
static double gr_bearing(double ax, double az, double bx, double bz)
{
    return atan2(bx - ax, bz - az);
}

static double gr_angdiff_deg(double a, double b)
{
    return fabs(gr_deg(gr_wrap(a - b)));
}

/* geo_forks._nearest_on_road: closest approach of one way's polyline to P, that
 * segment's bearing, and (ROUND 1010) the closest POINT itself -- which is what
 * says on which side of travel the peer carriageway lies, and therefore which
 * way the median and the opposite carriageway have to be laid. */
static double gr_road_near_at(const GrRoad *r, double px, double pz,
                              double *bear, double *outx, double *outz)
{
    double best = 1e30;
    int i;
    for (i = 0; i + 1 < r->count; i++) {
        const double ax = s_g.px[r->first + i],     az = s_g.pz[r->first + i];
        const double bx = s_g.px[r->first + i + 1], bz = s_g.pz[r->first + i + 1];
        const double dx = bx - ax, dz = bz - az;
        const double l2 = dx * dx + dz * dz;
        double t, cx, cz, d;
        if (l2 <= 1e-12) continue;
        t = ((px - ax) * dx + (pz - az) * dz) / l2;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        cx = ax + dx * t; cz = az + dz * t;
        d = hypot(px - cx, pz - cz);
        if (d < best) {
            best = d;
            if (bear) *bear = gr_bearing(ax, az, bx, bz);
            if (outx) *outx = cx;
            if (outz) *outz = cz;
        }
    }
    return best;
}

/* Is road `b` the same OSM way as road `a`? OSM splits one way into several
 * records, and geo_forks excludes peers by OSM id rather than by record, so a
 * run must not pair with another slice of itself. */
static int gr_same_way(int a, int b)
{
    if (a == b) return 1;
    if (!s_g.road[a].has_id || !s_g.road[b].has_id) return 0;
    return s_g.road[a].id == s_g.road[b].id;
}

/* ---- [ROUND 1010 AVENUES] the ways the ROUTE ITSELF drives on --------------
 *
 * ROOT CAUSE of "you created branches instead of using the actual map", and the
 * reason FORKS.JSON claimed a 21 m median on Calle 54.
 *
 * gr_same_way excludes a peer only when it carries the SAME OSM id. But OSM
 * splits one carriageway at every junction, so the road a route drives along
 * arrives as a dozen ways with a dozen different ids and one shared name. Every
 * one of them therefore passes the peer test against its own neighbours, and
 * the "opposite carriageway" the detector measured was, at many vertices, the
 * next slice of the road the car is standing on.
 *
 * MEASURED on Mariano's route (re/tools-free probe over _route/ROADS.JSON +
 * ROUTE_RAW.JSON, 140 route vertices, 2291 ways):
 *
 *   Calle 54, route run 97..112 -- all eleven same-name ways beside the run lie
 *   within +/-2.6 m of ONE line, every one oneway dir=1 on bearing ~+42 deg,
 *   and the route itself spans -2.4..+2.6 m of that line. It is a SINGLE
 *   undivided one-way street drawn as a chain. The 21 m "carriageway spacing"
 *   was the distance from one vertex to a NON-ADJACENT segment of that same
 *   chain, picked up because the chain bends.
 *   Seven of its ways are route-own; after this filter the pairing collapses.
 *
 *   Calle 14, route run 70..96 -- same shape, six route-own ways.
 *
 *   Diagonal 73, route run 16..26 -- TWO chains: the route's own ways cluster
 *   at -0.6..+6.4 m on bearing ~+89 deg, a second group at -7.0..-16.8 m on
 *   bearing ~-95 deg (ANTI-parallel), ~11 m apart. A genuine divided avenue,
 *   and it survives this filter untouched.
 *
 * `n_own` ids are collected once per route from the edge the walk actually
 * took, so the test is about the ROUTE, not about proximity. */
#define GR_MED_MAX_OWN  512
static double s_med_own[GR_MED_MAX_OWN];
static int    s_med_n_own;

static void gr_own_ways_reset(void) { s_med_n_own = 0; }

static void gr_own_ways_add(int ri)
{
    int i;
    if (ri < 0 || ri >= s_g.n_roads || !s_g.road[ri].has_id) return;
    for (i = 0; i < s_med_n_own; i++)
        if (s_med_own[i] == s_g.road[ri].id) return;
    if (s_med_n_own < GR_MED_MAX_OWN) s_med_own[s_med_n_own++] = s_g.road[ri].id;
}

static int gr_route_owns_way(int ri)
{
    int i;
    if (ri < 0 || ri >= s_g.n_roads || !s_g.road[ri].has_id) return 0;
    for (i = 0; i < s_med_n_own; i++)
        if (s_med_own[i] == s_g.road[ri].id) return 1;
    return 0;
}

/* geo_forks.detect_medians. `road_id[k]` is the way the route's k-th vertex
 * arrived on, `px/pz[k]` its position in the PLACE frame. Returns the number of
 * medians written to `out`, longest first then overlap-filtered, finally sorted
 * by k0. */
static int gr_detect_medians(const int *road_id, const double *px,
                             const double *pz, int n, double upm,
                             GrMedian *out, int max)
{
    static int run_a[GR_MED_MAX_RUNS], run_b[GR_MED_MAX_RUNS], run_r[GR_MED_MAX_RUNS];
    int n_run = 0, n_out = 0, i, j, k;

    if (n < 2 || !out || max < 1 || upm <= 0.0) return 0;

    /* 1a. maximal runs of >= 2 consecutive vertices on one way. */
    for (i = 0; i < n; ) {
        int e = i;
        while (e + 1 < n && road_id[e + 1] == road_id[i]) e++;
        if (e - i + 1 >= 2 && road_id[i] >= 0 && road_id[i] < s_g.n_roads &&
            n_run < GR_MED_MAX_RUNS) {
            /* 1b. join onto the previous run when the NAME matches and the gap
             * is at most 2 vertices: OSM splits one avenue at every junction,
             * so an avenue reaches the route as a dozen runs that are one road
             * to a driver. */
            const char *nm = s_g.road[road_id[i]].name;
            if (n_run > 0 && nm && s_g.road[run_r[n_run - 1]].name &&
                !strcmp(nm, s_g.road[run_r[n_run - 1]].name) &&
                i - run_b[n_run - 1] <= 2) {
                run_b[n_run - 1] = e;
            } else {
                run_a[n_run] = i; run_b[n_run] = e; run_r[n_run] = road_id[i];
                n_run++;
            }
        }
        i = e + 1;
    }

    /* 2. score each merged run against its same-name peers. */
    for (i = 0; i < n_run && n_out < max; i++) {
        const int k0 = run_a[i], k1 = run_b[i], rid = run_r[i];
        const GrRoad *road = &s_g.road[rid];
        const char *name = road->name;
        double run_units = 0.0, run_m;
        int hits = 0, total = 0, step, best_peer = -1, best_peer_hits = 0;
        int peer_hits_id[GR_FORK_MAX * 4], peer_hits_n[GR_FORK_MAX * 4], n_ph = 0;
        /* [ROUND 1010 AVENUES] per-sample side and distance, for the stability
         * gate below. One entry per SAMPLED vertex that found a peer. */
        double s_d[GR_MED_MAX_SAMP];
        int    s_side[GR_MED_MAX_SAMP], s_peer[GR_MED_MAX_SAMP], n_s = 0;
        int    dom_side = 0, n_dom = 0;
        double dom_sum = 0.0, dom_lo = 1e30, dom_hi = 0.0;
        GrMedian *m;

        if (!name || !name[0]) continue;
        /* [ROUND 1010 AVENUES] a plaza ring / roundabout is not an avenue. */
        if (road->ring) {
            TD5_LOG_I(LOG_TAG, "geo route: \"%s\" raw %d..%d is a one-way RING "
                      "(OSM junction=roundabout/circular), not a divided "
                      "avenue -- no median built", name, k0, k1);
            continue;
        }
        for (k = k0; k < k1; k++)
            run_units += hypot(px[k + 1] - px[k], pz[k + 1] - pz[k]);
        run_m = run_units / upm;
        if (run_m < GR_MED_MIN_LEN_M) continue;

        step = (k1 - k0) / 12;
        if (step < 1) step = 1;
        for (k = k0; k <= k1 && n_s < GR_MED_MAX_SAMP; k += step) {
            const int nx = (k + 1 < n) ? k + 1 : n - 1;
            const double rbr = gr_bearing(px[k], pz[k], px[nx], pz[nx]);
            /* Left of travel, in the same (x across, z along) convention
             * gr_bearing uses: travel is (sin b, cos b), left is (cos b, -sin b).
             * This matches tg_road_edge's "left of travel is (tz, -tx)", which
             * is what makes the stored offset usable as a lateral shift. */
            const double lx =  cos(rbr), lz = -sin(rbr);
            double cand_m = 0.0, cand_cx = 0.0, cand_cz = 0.0;
            int cand_j = -1;
            total++;
            for (j = 0; j < s_g.n_roads; j++) {
                const GrRoad *pr = &s_g.road[j];
                double pbr = 0.0, d_m, anti, para, cx = 0.0, cz = 0.0;
                if (!pr->oneway) continue;          /* no median tag to trust  */
                if (!pr->name || strcmp(pr->name, name)) continue;
                if (gr_same_way(rid, j)) continue;
                /* [ROUND 1010 AVENUES] the two new peer rejections. */
                if (pr->ring) continue;             /* plaza arc, not a median */
                if (gr_route_owns_way(j)) continue; /* the road being DRIVEN   */
                d_m = gr_road_near_at(pr, px[k], pz[k], &pbr, &cx, &cz) / upm;
                if (d_m < GR_MED_MIN_M || d_m > GR_MED_MAX_M) continue;
                anti = gr_angdiff_deg(rbr + M_PI, pbr);
                para = gr_angdiff_deg(rbr, pbr);
                if (anti > GR_MED_ANTI_TOL_DEG && para > GR_MED_PARA_TOL_DEG) continue;
                if (cand_j < 0 || d_m < cand_m) {
                    cand_j = j; cand_m = d_m; cand_cx = cx; cand_cz = cz;
                }
            }
            if (cand_j >= 0) {
                int q, slot = -1;
                const double dot = (cand_cx - px[k]) * lx + (cand_cz - pz[k]) * lz;
                hits++;
                s_d[n_s]    = cand_m;
                s_side[n_s] = (dot > 0.0) ? 1 : -1;
                s_peer[n_s] = cand_j;
                n_s++;
                for (q = 0; q < n_ph; q++) if (peer_hits_id[q] == cand_j) { slot = q; break; }
                if (slot < 0 && n_ph < (int)(sizeof peer_hits_id / sizeof peer_hits_id[0])) {
                    slot = n_ph++;
                    peer_hits_id[slot] = cand_j; peer_hits_n[slot] = 0;
                }
                if (slot >= 0) peer_hits_n[slot]++;
            }
        }
        if (!total || (double)hits / (double)total < GR_MED_MIN_COVER) continue;

        /* ---- [ROUND 1010 AVENUES] STABILITY: one side, bounded spread -------
         *
         * A median follows the opposite carriageway, so every sample of a real
         * one lands on the SAME side of travel at about the SAME distance; it
         * is allowed to narrow and widen (Diagonal 73's last run runs 12.5 ->
         * 15.8 m), not to jump sides or double.
         *
         * MEASURED, same probe as gr_route_owns_way, AFTER the ring and
         * route-own rejections: the five Diagonal 73 runs score 100 % on one
         * side with spread 1.00 / 1.00 / 1.28 / 1.00 / 1.27; Calle 14 scores
         * 82 % with spread 5.86 and Calle 54 75 % with spread 3.69. The gates
         * below sit in that gap, and both halves are needed -- Calle 14 passes
         * a side test alone.
         *
         * Keeping only the dominant-side samples is also what makes the stored
         * offset meaningful: the opposite carriageway is ONE road, so a sample
         * on the other side is a different road and must not pull the mean. */
        for (j = 0; j < n_s; j++) if (s_side[j] > 0) n_dom++;
        dom_side = (n_dom * 2 >= n_s) ? 1 : -1;
        n_dom = 0;
        for (j = 0; j < n_s; j++) {
            if (s_side[j] != dom_side) continue;
            n_dom++;
            dom_sum += s_d[j];
            if (s_d[j] < dom_lo) dom_lo = s_d[j];
            if (s_d[j] > dom_hi) dom_hi = s_d[j];
        }
        if (!n_s || !n_dom) continue;
        if ((double)n_dom / (double)n_s < GR_MED_SIDE_FRAC ||
            dom_lo <= 0.0 || dom_hi / dom_lo > GR_MED_SPREAD_MAX) {
            TD5_LOG_I(LOG_TAG, "geo route: \"%s\" raw %d..%d is NOT a divided "
                      "avenue: its same-name partner changes side (%d of %d "
                      "samples agree) or scatters (%.1f..%.1f m, %.2fx) -- the "
                      "route is on a single carriageway there, no median built",
                      name, k0, k1, n_dom, n_s, dom_lo, dom_hi,
                      dom_lo > 0.0 ? dom_hi / dom_lo : 0.0);
            continue;
        }
        /* The carriageway the run pairs with most often ON THE DOMINANT SIDE is
         * the one to read the opposing lane count off. */
        for (j = 0; j < n_ph; j++) {
            int q, on_side = 0;
            for (q = 0; q < n_s; q++)
                if (s_peer[q] == peer_hits_id[j] && s_side[q] == dom_side) on_side++;
            if (on_side > best_peer_hits) {
                best_peer_hits = on_side;
                best_peer = peer_hits_id[j];
            }
        }

        m = &out[n_out++];
        memset(m, 0, sizeof *m);
        m->k0 = k0; m->k1 = k1;
        m->sep = 0.16;
        m->length_m = run_m;
        m->gap_m = dom_sum / (double)n_dom;
        m->gap_min_m = dom_lo;
        m->gap_max_m = dom_hi;
        m->side  = dom_side;
        m->cover = (double)hits / (double)total;
        {
            const int la = road->lanes > 0 ? road->lanes : 2;
            const int lb = (best_peer >= 0 && s_g.road[best_peer].lanes > 0)
                         ? s_g.road[best_peer].lanes : la;
            int want = la + lb;
            m->peer_lanes = lb;
            if (want < 4) want = 4;
            if (want > 8) want = 8;
            m->lanes = want;
        }
        snprintf(m->name, sizeof m->name, "%s", name);
        snprintf(m->id, sizeof m->id, "median:%s:%d-%d", name, k0, k1);
        snprintf(m->source, sizeof m->source,
                 "paired one-way ways, same name (%d of them)", n_ph);
        snprintf(m->detail, sizeof m->detail,
                 "%s: %.0f m of divided avenue, carriageways %.1f m apart "
                 "(%.1f..%.1f) on the %s", name, run_m, m->gap_m,
                 dom_lo, dom_hi, dom_side > 0 ? "left" : "right");
    }

    /* 3. longest first, then drop anything overlapping an already-kept range:
     * two forks sharing spans would fight over the same carriageway. */
    for (i = 0; i < n_out; i++)
        for (j = i + 1; j < n_out; j++)
            if (out[j].length_m > out[i].length_m) {
                const GrMedian t = out[i]; out[i] = out[j]; out[j] = t;
            }
    for (i = 0; i < n_out; i++) {
        int drop = 0;
        for (j = 0; j < i; j++)
            if (!(out[i].k1 < out[j].k0 || out[i].k0 > out[j].k1)) { drop = 1; break; }
        if (drop) {
            for (k = i; k + 1 < n_out; k++) out[k] = out[k + 1];
            n_out--; i--;
        }
    }
    for (i = 0; i < n_out; i++)
        for (j = i + 1; j < n_out; j++)
            if (out[j].k0 < out[i].k0) {
                const GrMedian t = out[i]; out[i] = out[j]; out[j] = t;
            }
    return n_out;
}

/* geo_selector._span_of_frac: arclength fraction along the ROUTE BODY ->
 * conditioned span index. Fraction 0 is node lead_in_nodes (the body's first
 * point, which since round 1009 is node 0) and fraction 1 is the last span. */
static int gr_span_of_frac(double f, int lead, int spans)
{
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    return lead + (int)gr_round_even(f * (double)(spans - lead));
}

/* ======================================================================== *
 * [ROUND 1010 AVENUES] sampling the REAL map, once per conditioned span
 * ======================================================================== *
 *
 * gr_detect_medians answers "is this run a divided avenue, and on which side".
 * These three answer the questions the GEOMETRY needs, span by span, so the
 * median width and the opposite carriageway come from the map rather than from
 * a constant:
 *
 *   gr_poly_at           where on the real routed polyline this span sits,
 *                        and which way it is pointing
 *   gr_peer_offset_at    how far the opposite carriageway's centreline is from
 *                        there, signed, and how many lanes it carries
 *   gr_median_opening_at does a real cross street cut the median here
 *
 * All three work in the PLACE frame and in WORLD UNITS. A perpendicular
 * distance survives the conditioner's rotation and translation unchanged (both
 * are rigid and the scale is shared), which is why an offset measured here can
 * be applied to the built centreline without any frame conversion -- see the
 * GrAvSpan comment for why an offset is stored rather than the peer polyline.
 */

/* Position and unit tangent at normalised arclength `f` along `p`. `frac` is
 * gr_arc_frac(p). Interpolates WITHIN the segment, because a span is 1500 units
 * (3.5 m) and the routed polyline's vertices are tens of metres apart -- the
 * nearest-vertex form (gr_src_at) would hold one offset for a dozen spans and
 * stair-step the median. */
static int gr_poly_at(const GrPts *p, const double *frac, double f,
                      double *x, double *z, double *tx, double *tz)
{
    int i;
    double t, dx, dz, m;
    if (!p || p->n < 2 || !frac) return 0;
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    i = gr_src_at(f, frac, p->n);
    /* gr_src_at rounds to the NEAREST vertex; step back to the segment that
     * actually contains f so the interpolation parameter stays in 0..1. */
    while (i > 0 && frac[i] > f) i--;
    while (i + 1 < p->n - 1 && frac[i + 1] < f) i++;
    if (i > p->n - 2) i = p->n - 2;
    t = (frac[i + 1] > frac[i]) ? (f - frac[i]) / (frac[i + 1] - frac[i]) : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    dx = p->x[i + 1] - p->x[i];
    dz = p->z[i + 1] - p->z[i];
    m  = hypot(dx, dz);
    if (m < 1e-9) return 0;
    *x  = p->x[i] + dx * t;
    *z  = p->z[i] + dz * t;
    *tx = dx / m;
    *tz = dz / m;
    return 1;
}

/* Signed lateral offset (world units, + = LEFT of travel) from (x,z) to a
 * same-name one-way carriageway on `want_side`, plus that way's lane count.
 * Returns 0 where the opposite carriageway has ended -- which is exactly how
 * the median stops where the OSM ways stop.
 *
 * The peer set is the one gr_detect_medians accepted: same name, one-way, not a
 * plaza arc, and NOT a way the route itself drives on.
 *
 * CONTINUITY, and it is not optional. `want` is the offset the PREVIOUS span
 * settled on, and the candidate chosen is the one NEAREST that rather than the
 * one nearest the road. Taking the nearest each span independently follows
 * whichever slice of the street happens to be closest, and a long avenue has
 * several: MEASURED on the first run of this code over Mariano's route, avenue
 * 0 ran 5.5..10.7 m and built a median of -1.43..3.74 m (negative: the scenery
 * carriageway inside the race road), and avenue 2 ran 10.7..22.0 m and built
 * 3.73..15.11 m -- one carriageway for part of the run and a different way for
 * the rest. A median does not double in width over 3.5 m of road.
 *
 * A candidate further than GR_AV_MAX_STEP from `want` is refused outright, so
 * the caller ends the run there: that is the honest reading of "the mapped
 * opposite carriageway stops here". */
static int gr_peer_offset_at(const char *name, int want_side, double want,
                             double cap, double x, double z,
                             double tx, double tz, double *off, int *lanes)
{
    const double lx = tz, lz = -tx;        /* left of travel, tg_road_edge's  */
    const double lo = GR_MED_MIN_M * GR_UNITS_PER_METRE;
    const double hi = GR_MED_MAX_M * GR_UNITS_PER_METRE;
    double best = 1e30, best_d = 0.0;
    int j, best_j = -1;
    if (!name || !name[0]) return 0;
    if (want < 0.0) want = -want;
    for (j = 0; j < s_g.n_roads; j++) {
        const GrRoad *pr = &s_g.road[j];
        double cx = 0.0, cz = 0.0, d, dot, err;
        if (!pr->oneway || pr->ring) continue;
        if (!pr->name || strcmp(pr->name, name)) continue;
        if (gr_route_owns_way(j)) continue;
        d = gr_road_near_at(pr, x, z, NULL, &cx, &cz);
        if (d < lo || d > hi) continue;
        dot = (cx - x) * lx + (cz - z) * lz;
        if ((dot > 0.0 ? 1 : -1) != want_side) continue;
        err = (d > want) ? d - want : want - d;
        if (err >= best) continue;
        best = err; best_d = d; best_j = j;
    }
    if (best_j < 0 || best > cap) return 0;
    if (off)   *off   = best_d * (double)want_side;
    if (lanes) *lanes = s_g.road[best_j].lanes > 0 ? s_g.road[best_j].lanes : 2;
    return 1;
}

/* Does a real cross street cut the median at this span? The median's midline is
 * halfway between the two carriageway centrelines; a street that reaches it is
 * a junction, and a real avenue opens its island there instead of walling the
 * turn off. Any DIFFERENTLY-named drivable way counts -- a same-name way at
 * that point is the pair's own geometry, not a crossing. */
static int gr_median_opening_at(const char *name, double x, double z,
                                double tx, double tz, double off)
{
    const double mx = x + tz * (off * 0.5);
    const double mz = z - tx * (off * 0.5);
    /* Half a span: wide enough that a crossing is caught on at least one span,
     * tight enough that a street running ALONGSIDE the avenue does not open the
     * island for its whole length. */
    const double reach = GR_SPAN_LENGTH * 0.5;
    int j;
    for (j = 0; j < s_g.n_roads; j++) {
        const GrRoad *pr = &s_g.road[j];
        if (pr->name && name && strcmp(pr->name, name) == 0) continue;
        if (pr->count < 2) continue;
        if (gr_road_near_at(pr, mx, mz, NULL, NULL, NULL) <= reach) return 1;
    }
    return 0;
}

/* [ROUND 1010 AVENUES] gr_ramp_one_lane_per_seam and gr_fit_fork lived here.
 * Both existed only to serve the round-1009 path that WIDENED the route to
 * lanes(A)+lanes(B) over a median and then read the fork's span range back off
 * the re-conditioned lanes. A median no longer widens the race road or becomes
 * a fork at all (see the DIVIDED AVENUES block in gr_route), so neither has a
 * caller; they are in git history if a GENUINE alternative-route fork ever
 * needs them back. */

/* ======================================================================== *
 * SECTION: build
 * ======================================================================== */

/* Everything the last OK build needs to be committed. Kept apart from the
 * public result so a caller cannot accidentally free half of it. */
static struct {
    int    valid;
    char   slug[64];
    int    n_wp;
    TD5_GeoLatLon wp[GR_MAX_WAYPOINTS];
    int    n_raw;
    TD5_GeoLatLon *raw_ll;
    int    *raw_lanes;
    double raw_length_units;
    char   streets[1024];
    GrCond cond;
    /* [ROUND 1009 item 5] the divided avenues this route was conditioned FOR,
     * already placed on spans. FORKS.JSON is written from these at commit.
     *
     * [ROUND 1010 AVENUES] a divided avenue no longer goes here. A median is
     * not a fork: the race stays on its own carriageway with its own lanes and
     * the other side is scenery, so nothing is added to the drivable graph.
     * This table is kept for a GENUINE alternative route (a real second way
     * round), which is still a fork, and is left empty by the avenue path --
     * which is what makes gr_write_forks DELETE a stale FORKS.JSON. */
    int      n_fork;
    int      fork_corridor_spans;
    GrMedian fork[GR_FORK_MAX];

    /* [ROUND 1010 AVENUES] the divided avenues, and the per-span lateral
     * offset to the REAL opposite carriageway that AVENUES.JSON is written
     * from. See the DIVIDED AVENUES section. */
    int      n_av;
    GrMedian av[GR_FORK_MAX];
    int      av_s0[GR_FORK_MAX], av_s1[GR_FORK_MAX];
    int      n_av_span;
    GrAvSpan av_span[GR_AV_MAX_SPANS];
} s_last;

static TD5_GeoRouteResult s_result;
static TD5_GeoLatLon *s_path_ll;
static TD5_GeoLatLon *s_cross_ll;

static void gr_last_free(void)
{
    free(s_last.raw_ll); free(s_last.raw_lanes);
    gr_cond_free(&s_last.cond);
    memset(&s_last, 0, sizeof s_last);
}

static void gr_result_set(TD5_GeoRouteResult *r, TD5_GeoRouteVerdict v,
                          const char *slug, const char *reason)
{
    memset(r, 0, sizeof *r);
    r->verdict = v;
    r->span_cap = GR_MAX_SPANS;
    snprintf(r->place_slug, sizeof r->place_slug, "%s", slug ? slug : "");
    snprintf(r->reason, sizeof r->reason, "%s", reason ? reason : "");
}

/* The ROUTABLE bounds of one place, as {west, south, east, north}.
 *
 * route_graph_bbox FIRST, the fetched bbox only as a fallback. The two are not
 * interchangeable: the selector pins route_graph_bbox on the first SEND TO
 * GAME (plan 6f) and gr_graph_load filters the road graph to it, so a point
 * inside the fetched bbox but outside the pinned one has NO roads in the
 * graph. Reporting the fetched bbox would shade that annulus as routable and
 * then refuse every click in it with a "250 m from the nearest road" message
 * that names the wrong cause. L2's placeholder had this right and it is kept.
 *
 * Returns 1 when `out` was filled. */
static int gr_place_bounds(const char *slug, double *out)
{
    char path[512];
    char *json;
    cJSON *root, *bb;
    int ok = 0;

    if (!slug || !slug[0]) return 0;
    td5_geo_source_path(path, sizeof path, slug, "PLACE.JSON");   /* SOURCE */
    json = gr_slurp(path, NULL);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    bb = cJSON_GetObjectItem(root, "route_graph_bbox");
    if (!bb || !cJSON_IsObject(bb)) bb = cJSON_GetObjectItem(root, "bbox");
    if (bb && cJSON_IsObject(bb)) {
        const cJSON *we = cJSON_GetObjectItem(bb, "west");
        const cJSON *so = cJSON_GetObjectItem(bb, "south");
        const cJSON *ea = cJSON_GetObjectItem(bb, "east");
        const cJSON *no = cJSON_GetObjectItem(bb, "north");
        if (cJSON_IsNumber(we) && cJSON_IsNumber(so) &&
            cJSON_IsNumber(ea) && cJSON_IsNumber(no)) {
            out[0] = we->valuedouble; out[1] = so->valuedouble;
            out[2] = ea->valuedouble; out[3] = no->valuedouble;
            ok = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

const char *td5_geo_route_place_at(TD5_GeoLatLon p)
{
    static char slug[64];
    const int n = td5_geo_places_count();
    int i;
    for (i = 0; i < n; i++) {
        const char *s = td5_geo_places_slug(i);
        double bb[4];
        if (!gr_place_bounds(s, bb)) continue;
        if (gr_in_bbox(p.lat, p.lon, bb)) {
            snprintf(slug, sizeof slug, "%s", s);
            return slug;
        }
    }
    slug[0] = '\0';
    return slug;
}

int td5_geo_route_places(char slugs[][64], double bbox[][4], int max)
{
    const int n = td5_geo_places_count();
    int i, out = 0;
    for (i = 0; i < n; i++) {
        double bb[4];
        if (slugs && out >= max) break;
        if (!gr_place_bounds(td5_geo_places_slug(i), bb)) continue;
        if (slugs) {
            snprintf(slugs[out], 64, "%s", td5_geo_places_slug(i));
            if (bbox) {
                bbox[out][0] = bb[0]; bbox[out][1] = bb[1];
                bbox[out][2] = bb[2]; bbox[out][3] = bb[3];
            }
        }
        out++;
    }
    return out;
}

int td5_geo_route_snap(const char *slug, TD5_GeoLatLon p,
                       TD5_GeoLatLon *out, double *dist_m)
{
    double x, z;
    int n;
    if (!slug || !slug[0]) slug = td5_geo_route_place_at(p);
    if (!gr_graph_sync(slug)) return 0;
    gr_proj_to_world(&s_g.proj, p.lat, p.lon, &x, &z);
    n = gr_nearest(x, z);
    if (n < 0) return 0;
    if (dist_m) *dist_m = gr_dist(s_g.nx[n], s_g.nz[n], x, z) / s_g.proj.upm;
    if (out) gr_proj_to_latlon(&s_g.proj, s_g.nx[n], s_g.nz[n], &out->lat, &out->lon);
    return 1;
}

const TD5_GeoRouteResult *td5_geo_route_last(void)
{
    return s_result.verdict == TD5_GEO_ROUTE_OK || s_result.n_path ? &s_result : NULL;
}

int td5_geo_route_build(const TD5_GeoLatLon *pts, int n_pts, TD5_GeoRouteResult *out)
{
    const uint64_t t_us = td5_plat_time_us();
    const char *slug;
    int *wp_node = NULL, *seq = NULL, *legbuf = NULL;
    int n_seq = 0, i, allow_cross, allow_rev;
    int *raw_road = NULL;
    double *raw_x = NULL, *raw_z = NULL;
    GrCond *c = NULL;
    TD5_GeoLatLon *raw_ll = NULL;
    int *raw_lanes = NULL;
    double raw_len = 0.0;

    /* RETURN CONVENTION, and it is the opposite of the reflex: 0 means the
     * CALL was made and `out->verdict` carries the answer, including every
     * refusal. Non-zero means the call itself could not be made at all. The
     * screen tests `== 0` (td5_fe_geo.c:334, :369) and prints ROUTER REFUSED
     * THE REQUEST otherwise, so returning 1 on a good route -- which this did
     * until the round-1007 integration -- made every successful build read as
     * a router failure on screen. */
    if (!out) return 1;
    if (!pts || n_pts < 2) {
        gr_result_set(out, TD5_GEO_ROUTE_ERROR, "", "click A and B first");
        return 0;
    }
    if (n_pts > GR_MAX_WAYPOINTS) n_pts = GR_MAX_WAYPOINTS;

    slug = td5_geo_route_place_at(pts[0]);
    if (!slug[0]) {
        gr_result_set(out, TD5_GEO_ROUTE_NO_DATA,
                      "", "this point is outside every downloaded place");
        return 0;
    }
    for (i = 1; i < n_pts; i++) {
        const char *s2 = td5_geo_route_place_at(pts[i]);
        if (strcmp(s2, slug)) {
            gr_result_set(out, TD5_GEO_ROUTE_NO_DATA, slug,
                          "the route leaves the downloaded area");
            return 0;
        }
    }
    if (!gr_graph_sync(slug)) {
        gr_result_set(out, TD5_GEO_ROUTE_NO_DATA, slug,
                      "this place has no readable road graph");
        return 0;
    }

    /* geo_selector.graph_verdict -- is there enough drivable road here at all? */
    {
        int comps = 0, biggest = 0;
        gr_components(&comps, &biggest);
        if (s_g.n_roads < GR_GRAPH_MIN_WAYS || s_g.n_nodes < GR_GRAPH_MIN_NODES) {
            char why[160];
            snprintf(why, sizeof why, "not enough drivable road here: %d way(s), "
                     "%d junction point(s)", s_g.n_roads, s_g.n_nodes);
            gr_result_set(out, TD5_GEO_ROUTE_TOO_SHORT, slug, why);
            return 0;
        }
        if (biggest < (int)((double)s_g.n_nodes * GR_GRAPH_MIN_MAIN_FRAC)) {
            char why[160];
            snprintf(why, sizeof why, "the road graph here is in %d disconnected "
                     "pieces; most pairs of points cannot be driven between", comps);
            gr_result_set(out, TD5_GEO_ROUTE_NO_PATH, slug, why);
            return 0;
        }
    }

    /* geo_selector.click_verdict -- every waypoint lands on, or near, a road. */
    wp_node = (int *)malloc((size_t)n_pts * sizeof(int));
    if (!wp_node) { gr_result_set(out, TD5_GEO_ROUTE_ERROR, slug, "out of memory"); return 0; }
    for (i = 0; i < n_pts; i++) {
        double x, z, d_m;
        gr_proj_to_world(&s_g.proj, pts[i].lat, pts[i].lon, &x, &z);
        wp_node[i] = gr_nearest(x, z);
        if (wp_node[i] < 0) {
            free(wp_node);
            gr_result_set(out, TD5_GEO_ROUTE_NO_DATA, slug,
                          "there is no drivable road in this place at all");
            return 0;
        }
        d_m = gr_dist(s_g.nx[wp_node[i]], s_g.nz[wp_node[i]], x, z) / s_g.proj.upm;
        if (d_m > TD5_GEO_ROUTE_SNAP_MAX_M) {
            char why[160];
            snprintf(why, sizeof why, "point %d is %.0f m from the nearest drivable "
                     "road (limit %.0f m). Click on a street.", i + 1, d_m,
                     TD5_GEO_ROUTE_SNAP_MAX_M);
            free(wp_node);
            gr_result_set(out, TD5_GEO_ROUTE_NO_DATA, slug, why);
            return 0;
        }
    }

    /* geo_route.RoadGraph.route -- each leg is an independent A*, which is what
     * lets the screen insert a waypoint and re-route only the two legs it
     * touches. */
    seq    = (int *)malloc((size_t)s_g.n_nodes * sizeof(int));
    legbuf = (int *)malloc((size_t)s_g.n_nodes * sizeof(int));
    if (!seq || !legbuf) {
        free(wp_node); free(seq); free(legbuf);
        gr_result_set(out, TD5_GEO_ROUTE_ERROR, slug, "out of memory");
        return 0;
    }
    for (i = 0; i < n_pts - 1; i++) {
        const int m = gr_path(wp_node[i], wp_node[i + 1], legbuf, s_g.n_nodes);
        int k;
        if (m <= 0) {
            char why[160];
            snprintf(why, sizeof why, "no drivable path for leg %d of %d -- the "
                     "two points are on roads that do not connect", i + 1, n_pts - 1);
            free(wp_node); free(seq); free(legbuf);
            gr_result_set(out, TD5_GEO_ROUTE_NO_PATH, slug, why);
            return 0;
        }
        for (k = (i == 0 ? 0 : 1); k < m; k++) {
            if (n_seq >= s_g.n_nodes) break;
            seq[n_seq++] = legbuf[k];
        }
    }
    free(legbuf);
    if (n_seq < 2) {
        free(wp_node); free(seq);
        gr_result_set(out, TD5_GEO_ROUTE_NO_PATH, slug, "the route has no length");
        return 0;
    }

    raw_ll    = (TD5_GeoLatLon *)malloc((size_t)n_seq * sizeof(TD5_GeoLatLon));
    raw_lanes = (int *)malloc((size_t)n_seq * sizeof(int));
    raw_road  = (int *)malloc((size_t)n_seq * sizeof(int));
    raw_x     = (double *)malloc((size_t)n_seq * sizeof(double));
    raw_z     = (double *)malloc((size_t)n_seq * sizeof(double));
    if (!raw_ll || !raw_lanes || !raw_road || !raw_x || !raw_z) {
        free(wp_node); free(seq); free(raw_ll); free(raw_lanes);
        free(raw_road); free(raw_x); free(raw_z);
        gr_result_set(out, TD5_GEO_ROUTE_ERROR, slug, "out of memory");
        return 0;
    }

    /* Lanes, street names and length, exactly as geo_route.route builds them. */
    gr_last_free();
    s_last.streets[0] = '\0';
    /* [ROUND 1010 AVENUES] the ways this route drives on, collected as the walk
     * is read back, so gr_detect_medians can refuse to pair the road with
     * itself. See gr_route_owns_way. */
    gr_own_ways_reset();
    for (i = 0; i < n_seq; i++) {
        const int nd = seq[i];
        const int ri = i ? gr_edge_road(seq[i - 1], nd) : gr_edge_road(nd, seq[1]);
        const GrRoad *r = (ri >= 0 && ri < s_g.n_roads) ? &s_g.road[ri] : NULL;
        gr_proj_to_latlon(&s_g.proj, s_g.nx[nd], s_g.nz[nd],
                          &raw_ll[i].lat, &raw_ll[i].lon);
        raw_lanes[i] = r ? r->lanes : 2;
        /* [ROUND 1009 item 5] the way each vertex arrived on, and its position
         * in the PLACE frame: geo_forks.detect_medians' `road_ids` and
         * `pts_world`. */
        raw_road[i] = ri;
        gr_own_ways_add(ri);            /* [ROUND 1010 AVENUES] */
        raw_x[i] = s_g.nx[nd];
        raw_z[i] = s_g.nz[nd];
        if (i) raw_len += gr_dist(s_g.nx[seq[i - 1]], s_g.nz[seq[i - 1]],
                                  s_g.nx[nd], s_g.nz[nd]);
        /* geo_route.route resolves the name through the first road record
         * carrying the same OSM id, then de-duplicates consecutive repeats. */
        if (r && r->has_id) {
            int q;
            for (q = 0; q < s_g.n_roads; q++) {
                if (!s_g.road[q].has_id || s_g.road[q].id != r->id) continue;
                if (s_g.road[q].name) {
                    const size_t have = strlen(s_last.streets);
                    const char *nm = s_g.road[q].name;
                    const size_t nl = strlen(nm);
                    const char *tail = have >= nl ? s_last.streets + have - nl : NULL;
                    if (!(tail && !strcmp(tail, nm)) &&
                        have + nl + 5u < sizeof s_last.streets) {
                        if (have) strcat(s_last.streets, " > ");
                        strcat(s_last.streets, nm);
                    }
                }
                break;
            }
        }
    }
    free(seq);

    allow_cross = td5_env_flag_on("TD5RE_GEO_ROUTE_XLEVEL");
    /* [ROUND 1009 item 1, follow-up] The user placed START. Honour it:
     * no orientation choice on the in-game path. See the orientation
     * section of gr_condition. */
    allow_rev = td5_env_flag_on("TD5RE_GEO_START_AT_CLICK") ? 0 : 1;
    c = &s_last.cond;
    gr_condition(raw_ll, raw_lanes, n_seq, GR_UNITS_PER_METRE,
                 GR_CURVE_SAFETY_X100, GR_SPAN_LENGTH, GR_LANE_WIDTH,
                 allow_rev, allow_cross, c);

    /* ---- [ROUND 1010 AVENUES] A DIVIDED AVENUE IS NOT A FORK ----------------
     *
     * "you didn't properly catch avenues of the selected road, you created
     * branches instead of using the actual map."
     *
     * Round 1009 item 5 detected the avenues correctly and then built them with
     * the SYNTHETIC fork machinery: widen the route to lanes(A)+lanes(B),
     * re-condition for that width, and hand the span range to TG_FORK_ISLAND,
     * which splits the road into two drivable carriageways bowing around an
     * invented gore. Three things were wrong with that, and they are the three
     * things this rewrite undoes:
     *
     *  1. THE RACE GOT A BRANCH. A fork is a drivable alternative path -- span
     *     records, a jump table and a TG_WO_DRIVABLE paint. A median is not an
     *     alternative route; you drive YOUR carriageway and the other one is on
     *     the far side of a kerb. Nothing is added to the drivable graph now.
     *
     *  2. THE WIDTH WAS INVENTED. `sep` in the fork table is a BOW SCALE, not a
     *     distance, and it was pinned at 0.16 -- which is TD5_TG_BRANCH_SEP_MIN,
     *     the tightest bow the branch machinery can express. Every avenue on
     *     Mariano's route therefore got the same median whether the real
     *     carriageways were 10 m or 21 m apart. The offset is now MEASURED per
     *     span off the real polylines (gr_peer_offset_at), so the median
     *     narrows, widens and ends where the OSM ways do.
     *
     *  3. THE OPPOSITE CARRIAGEWAY WAS NOT BUILT. The branch's far half was a
     *     bowed copy of the race road, not the way that is actually there. Its
     *     real polyline now drives the offset, and its own lane count travels
     *     with it, so the generator lays it as scenery road at its real place.
     *
     * The route is NO LONGER re-conditioned for a doubled width either: the
     * race drives its own carriageway with that carriageway's own lanes, which
     * is what the router chose, so there is one conditioning pass again and a
     * geo route's span count no longer moves when an avenue is found.
     *
     * TD5RE_GEO_AVENUES=0 still pins "no detection, no sidecar" for an A/B. */
    if (c->ok && td5_env_flag_on("TD5RE_GEO_AVENUES")) {
        GrMedian med[GR_FORK_MAX];
        const int n_med = gr_detect_medians(raw_road, raw_x, raw_z, n_seq,
                                            GR_UNITS_PER_METRE, med, GR_FORK_MAX);
        TD5_LOG_I(LOG_TAG, "geo route: %d divided avenue(s) on the route "
                  "(paired anti-parallel one-way ways of the same name, "
                  "%.0f..%.0f m apart, >= %.0f m long, cover >= %.2f)",
                  n_med, GR_MED_MIN_M, GR_MED_MAX_M, GR_MED_MIN_LEN_M,
                  GR_MED_MIN_COVER);
        for (i = 0; i < n_med; i++)
            TD5_LOG_I(LOG_TAG, "geo route:   %s: raw %d..%d, %.0f m, gap %.1f m "
                      "(%.1f..%.1f) on the %s, peer %d lane(s), cover %.2f",
                      med[i].name, med[i].k0, med[i].k1, med[i].length_m,
                      med[i].gap_m, med[i].gap_min_m, med[i].gap_max_m,
                      med[i].side > 0 ? "left" : "right", med[i].peer_lanes,
                      med[i].cover);
        if (n_med > 0) {
            GrPts rawp;
            double *frac = NULL;
            const int lead1 = c->lead_in_nodes, spans1 = c->spans, rev1 = c->reversed;
            memset(&rawp, 0, sizeof rawp);
            for (i = 0; i < n_seq; i++)
                if (!gr_pts_push(&rawp, raw_x[i], raw_z[i])) break;
            if (i == n_seq) frac = gr_arc_frac(&rawp);
            if (frac) {
                int j;
                /* No widening, no second conditioning pass: `c` is already the
                 * road the route asked for and it stays that way. All that
                 * happens here is MEASUREMENT -- where each avenue lands on the
                 * conditioned spans, and what the real map says at each one. */
                for (j = 0; j < n_med && s_last.n_av < GR_FORK_MAX; j++) {
                    const double f0 = rev1 ? 1.0 - frac[med[j].k1] : frac[med[j].k0];
                    const double f1 = rev1 ? 1.0 - frac[med[j].k0] : frac[med[j].k1];
                    const int a = gr_span_of_frac(f0, lead1, spans1);
                    const int b = gr_span_of_frac(f1, lead1, spans1);
                    /* The side was found walking the RAW polyline forward. A
                     * route conditioned REVERSED is driven the other way, so
                     * left and right swap with it. */
                    const int want = rev1 ? -med[j].side : med[j].side;
                    const int base = s_last.n_av_span;
                    int s0 = a < b ? a : b, s1 = a < b ? b : a, s, kept = 0;
                    const char *why = NULL;

                    /* Keep clear of the start grid and of the ring's tail, the
                     * same two margins every span-indexed sidecar respects. */
                    if (s0 < lead1 + GR_GRID_SPAN) s0 = lead1 + GR_GRID_SPAN;
                    if (s1 > spans1 - 2)           s1 = spans1 - 2;
                    if (s1 - s0 + 1 < GR_AV_MIN_SPANS)
                        why = "it does not reach the span floor clear of the "
                              "grid and the ring tail";

                    /* The run's own measured spacing seeds the continuity
                     * chain, so the first span is anchored on the carriageway
                     * the detector scored rather than on whatever is nearest.
                     *
                     * The SEED gets a wider cap than the chain, by exactly the
                     * spread the detector measured. `prev` starts at the MEAN,
                     * but the first span can legitimately sit at either end of
                     * a run that tapers: the 346 m avenue at spans 753..851 was
                     * measured at 12.4..15.6 m and seeded at the 14.3 m mean,
                     * so a flat GR_AV_MAX_STEP refused its own first span and
                     * dropped the whole avenue with "0 span(s) sampled".
                     *
                     * `miss` lets the chain cross a GAP in the mapping without
                     * ending the avenue. OSM interrupts a carriageway at a
                     * junction, and a hard stop at the first unmapped span
                     * would cut a long avenue in half at its first crossing.
                     * Four spans is 6000 units, 14 m -- an intersection's
                     * width. The skipped spans carry no row, so they get no
                     * median and no scenery road, which is what a crossing
                     * looks like anyway. */
                    const double seed_cap = GR_AV_MAX_STEP
                        + (med[j].gap_max_m - med[j].gap_min_m) * GR_UNITS_PER_METRE;
                    double prev = med[j].gap_m * GR_UNITS_PER_METRE;
                    int miss = 0, seeded = 0, last_ok = s0 - 1;
                    for (s = s0; !why && s <= s1 &&
                                 s_last.n_av_span < GR_AV_MAX_SPANS; s++) {
                        double f = (spans1 > lead1)
                                 ? (double)(s - lead1) / (double)(spans1 - lead1)
                                 : 0.0;
                        double qx, qz, qtx, qtz, off, a, half;
                        int plan = 2;
                        GrAvSpan *g;
                        if (rev1) f = 1.0 - f;
                        if (!gr_poly_at(&rawp, frac, f, &qx, &qz, &qtx, &qtz))
                            continue;
                        if (rev1) { qtx = -qtx; qtz = -qtz; }
                        /* The opposite carriageway has ENDED here (or jumped to
                         * a different way, which from the ground is the same
                         * thing): stop the avenue rather than carry it across
                         * geometry the map does not have. This is what makes
                         * the median end where the OSM ways end. */
                        if (!gr_peer_offset_at(med[j].name, want, prev,
                                               seeded ? GR_AV_MAX_STEP : seed_cap,
                                               qx, qz, qtx, qtz, &off, &plan)) {
                            if (seeded && ++miss <= GR_AV_MAX_MISS) continue;
                            break;
                        }
                        /* THE TWO CARRIAGEWAYS MUST NOT OVERLAP. The gap has to
                         * hold the race road's own half width, the opposite
                         * carriageway's half width, and a median between them.
                         * `c->lanes_out[s]` is the race road's STORED lane
                         * count at this span, i.e. exactly what the generator
                         * will build, so this is the same arithmetic
                         * td5_tg_avenue.c does and not an approximation of it. */
                        a    = (off < 0.0) ? -off : off;
                        half = ((s < c->nodes && c->lanes_out)
                                ? (double)c->lanes_out[s] : 2.0)
                             * GR_LANE_WIDTH * 0.5;
                        if (a < half + (double)plan * GR_LANE_WIDTH * 0.5
                                + GR_AV_MIN_MEDIAN) {
                            if (seeded && ++miss <= GR_AV_MAX_MISS) continue;
                            break;
                        }
                        prev    = a;
                        miss    = 0;
                        seeded  = 1;
                        last_ok = s;
                        g = &s_last.av_span[s_last.n_av_span++];
                        g->av    = s_last.n_av;
                        g->span  = s;
                        g->off   = off;
                        g->lanes = plan;
                        g->open  = gr_median_opening_at(med[j].name, qx, qz,
                                                        qtx, qtz, off);
                        kept++;
                    }
                    /* The run can be cut short by either gate above, and by
                     * trailing misses. Report the span it actually reached:
                     * "the avenue is shorter than the detector's range" is a
                     * fact about the map, not a failure. */
                    if (!why && kept > 0 && last_ok < s1) s1 = last_ok;
                    if (!why && kept < GR_AV_MIN_SPANS)
                        why = "the opposite carriageway is mapped over too few "
                              "of its spans";
                    if (why) {
                        s_last.n_av_span = base;
                        TD5_LOG_I(LOG_TAG, "geo route: avenue %s (%.0f m, spans "
                                  "%d..%d, %d span(s) sampled) NOT built: %s",
                                  med[j].name, med[j].length_m, s0, s1, kept, why);
                        continue;
                    }
                    s_last.av[s_last.n_av]    = med[j];
                    s_last.av[s_last.n_av].F  = s0;      /* span range, not a fork */
                    s_last.av[s_last.n_av].len = s1 - s0;
                    s_last.av_s0[s_last.n_av] = s0;
                    s_last.av_s1[s_last.n_av] = s1;
                    s_last.n_av++;
                }

                if (s_last.n_av)
                    gr_add_warning(c, "%d divided avenue(s) built from the map: "
                                   "real median, real opposite carriageway, no "
                                   "race fork (%s)", s_last.n_av,
                                   s_last.av[0].name);
                else
                    gr_add_warning(c, "%d divided avenue(s) detected, none could "
                                   "be placed on a span range", n_med);
                for (i = 0; i < s_last.n_av; i++) {
                    int q, nopen = 0;
                    double olo = 1e30, ohi = 0.0;
                    for (q = 0; q < s_last.n_av_span; q++) {
                        const GrAvSpan *g = &s_last.av_span[q];
                        if (g->av != i) continue;
                        if (g->open) nopen++;
                        if (fabs(g->off) < olo) olo = fabs(g->off);
                        if (fabs(g->off) > ohi) ohi = fabs(g->off);
                    }
                    TD5_LOG_I(LOG_TAG, "geo route: avenue %d: %s spans %d..%d, "
                              "opposite carriageway %.1f..%.1f m to the %s, "
                              "%d cross-street opening(s)", i, s_last.av[i].name,
                              s_last.av_s0[i], s_last.av_s1[i],
                              olo / GR_UNITS_PER_METRE, ohi / GR_UNITS_PER_METRE,
                              s_last.av[i].side > 0 ? "left" : "right", nopen);
                }
            }
            gr_pts_free(&rawp);
            free(frac);
        }
    }

    /* ---- publish ---- */
    gr_result_set(out, TD5_GEO_ROUTE_OK, slug, "");
    out->spans    = c->spans;
    out->length_m = (float)((double)c->spans * GR_SPAN_LENGTH / GR_UNITS_PER_METRE);
    out->raw_length_m = (float)(raw_len / GR_UNITS_PER_METRE);
    out->n_crossings_level = c->n_level;
    out->n_grade_separations = c->n_xsep;
    out->direction_reversed = c->reversed;
    out->worst_turn_deg = (float)c->curvature.worst_final_deg;
    out->limit_turn_deg = (float)c->curvature.limit_deg;
    out->monotone_pct   = (float)(100.0 * c->final.monotone_frac);
    snprintf(out->streets, sizeof out->streets, "%s", s_last.streets);
    for (i = 0; i < c->n_warnings && i < TD5_GEO_ROUTE_MAX_WARN; i++) {
        snprintf(out->warning[i], TD5_GEO_ROUTE_WARN_LEN, "%s", c->warning[i]);
        out->n_warnings++;
    }

    free(s_path_ll);
    s_path_ll = (TD5_GeoLatLon *)malloc((size_t)n_seq * sizeof(TD5_GeoLatLon));
    if (s_path_ll) {
        memcpy(s_path_ll, raw_ll, (size_t)n_seq * sizeof(TD5_GeoLatLon));
        out->path = s_path_ll;
        out->n_path = n_seq;
    }

    /* Crossing markers, mapped back out of the CONDITIONED frame so the screen
     * can put a dot where the user has to drag. DEVIATION (b): geo_selector.py
     * reads `node_a` off a merged group, which has no such key, so every marker
     * it draws lands on node 0. This uses the site's own closest pair. */
    free(s_cross_ll);
    s_cross_ll = NULL;
    if (c->n_sites > 0) {
        s_cross_ll = (TD5_GeoLatLon *)malloc((size_t)c->n_sites * sizeof(TD5_GeoLatLon));
        if (s_cross_ll) {
            for (i = 0; i < c->n_sites; i++) {
                const int a = c->sites[i].worst_a;
                gr_proj_to_latlon(&c->proj, c->nodes_xz.x[a], c->nodes_xz.z[a],
                                  &s_cross_ll[i].lat, &s_cross_ll[i].lon);
            }
            out->crossings = s_cross_ll;
            out->n_crossings = c->n_sites;
        }
    }

    if (!c->ok) {
        snprintf(out->reason, sizeof out->reason, "%s", c->reason[0]);
        if (c->spans > GR_MAX_SPANS)                        out->verdict = TD5_GEO_ROUTE_TOO_LONG;
        else if (c->spans < GR_GRID_SPAN + 150)             out->verdict = TD5_GEO_ROUTE_TOO_SHORT;
        else                                                out->verdict = TD5_GEO_ROUTE_ERROR;
    }

    /* Keep what commit() needs. */
    s_last.valid = (out->verdict == TD5_GEO_ROUTE_OK);
    snprintf(s_last.slug, sizeof s_last.slug, "%s", slug);
    s_last.n_wp = n_pts;
    for (i = 0; i < n_pts; i++) s_last.wp[i] = pts[i];
    s_last.n_raw = n_seq;
    s_last.raw_ll = raw_ll;
    s_last.raw_lanes = raw_lanes;
    s_last.raw_length_units = raw_len;
    free(wp_node);
    free(raw_road); free(raw_x); free(raw_z);

    out->build_ms = (double)(td5_plat_time_us() - t_us) / 1000.0;
    s_result = *out;
    return 0;    /* the call was made; out->verdict carries the answer */
}

/* ======================================================================== *
 * SECTION: the JSON writers (geo_selector.save_route)
 * ======================================================================== */

static cJSON *gr_json_proj(const GeoProj *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", "local_tangent_plane_wgs84");
    cJSON_AddNumberToObject(o, "lat0", p->lat0);
    cJSON_AddNumberToObject(o, "lon0", p->lon0);
    cJSON_AddNumberToObject(o, "units_per_metre", p->upm);
    cJSON_AddNumberToObject(o, "m_per_deg_lat", p->m_per_deg_lat);
    cJSON_AddNumberToObject(o, "m_per_deg_lon", p->m_per_deg_lon);
    cJSON_AddNumberToObject(o, "rotation_rad", gr_proj_theta(p));
    cJSON_AddNumberToObject(o, "offset_x", p->off_x);
    cJSON_AddNumberToObject(o, "offset_z", p->off_z);
    return o;
}

static int gr_write_json(const char *path, cJSON *root)
{
    char *txt = root ? cJSON_Print(root) : NULL;
    int ok = 0;
    if (txt) {
        ok = gr_write_atomic(path, txt, strlen(txt));
        cJSON_free(txt);
    }
    cJSON_Delete(root);
    return ok;
}

static int gr_write_route_raw(const char *dir)
{
    char path[512];
    cJSON *root = cJSON_CreateObject();
    cJSON *wps  = cJSON_CreateArray();
    cJSON *pts  = cJSON_CreateArray();
    int i;
    snprintf(path, sizeof path, "%s/ROUTE_RAW.JSON", dir);
    cJSON_AddStringToObject(root, "place", s_last.slug);
    cJSON_AddNumberToObject(root, "length_km",
                            s_last.raw_length_units / GR_UNITS_PER_METRE / 1000.0);
    for (i = 0; i < s_last.n_wp; i++) {
        cJSON *w = cJSON_CreateArray();
        cJSON_AddItemToArray(w, cJSON_CreateNumber(s_last.wp[i].lat));
        cJSON_AddItemToArray(w, cJSON_CreateNumber(s_last.wp[i].lon));
        cJSON_AddItemToArray(wps, w);
    }
    cJSON_AddItemToObject(root, "waypoints", wps);
    for (i = 0; i < s_last.n_raw; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "lat", s_last.raw_ll[i].lat);
        cJSON_AddNumberToObject(p, "lon", s_last.raw_ll[i].lon);
        cJSON_AddNumberToObject(p, "lanes", s_last.raw_lanes[i]);
        cJSON_AddItemToArray(pts, p);
    }
    cJSON_AddItemToObject(root, "points", pts);
    cJSON_AddStringToObject(root, "street_names", s_last.streets);
    return gr_write_json(path, root);
}

/* [ROUND 1009 item 5] FORKS.JSON for the divided avenues this route was
 * conditioned for, in the SAME shape geo_selector.py's _write_forks produces,
 * because td5_geo_forks.c reads both.
 *
 * ABSENCE IS MEANINGFUL and it is why the no-fork case DELETES. td5_geo_forks
 * treats a missing file as "no geo forks" and the generator then uses its own
 * synthetic fork placement; a stale file would keep the previous route's span
 * ranges alive, splitting carriageways at places that no longer mean anything.
 * This is the same delete the commit did unconditionally before -- now it only
 * happens when there is nothing to write. */
static int gr_write_forks(const char *dir)
{
    char path[512];
    cJSON *root, *arr;
    int i;

    snprintf(path, sizeof path, "%s/FORKS.JSON", dir);
    if (s_last.n_fork < 1) {
        if (td5_plat_file_exists(path)) td5_plat_file_delete(path);
        return 1;
    }
    root = cJSON_CreateObject();
    arr  = cJSON_CreateArray();
    cJSON_AddStringToObject(root, "place", s_last.slug);
    cJSON_AddNumberToObject(root, "spans", s_last.cond.spans);
    cJSON_AddNumberToObject(root, "corridor_spans", s_last.fork_corridor_spans);
    for (i = 0; i < s_last.n_fork; i++) {
        const GrMedian *m = &s_last.fork[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", m->id);
        /* ISLAND, not AVENUE: both read as a divided avenue downstream
         * (sep <= TD5_TG_AVENUE_SEP_MAX) and both get the central divider, but
         * ISLAND is the kind whose length floor is 3 spans rather than
         * TD5_TG_BRANCH_MIN_LEN (24), so a real median of any length survives
         * tg_fork_len_floored unchanged -- which is what lets the span range
         * here mean what it says. */
        cJSON_AddStringToObject(e, "kind", "ISLAND");
        cJSON_AddStringToObject(e, "name", m->name);
        cJSON_AddNumberToObject(e, "F", m->F);
        cJSON_AddNumberToObject(e, "len", m->len);
        cJSON_AddNumberToObject(e, "sep", m->sep);
        cJSON_AddNumberToObject(e, "lanes", m->lanes);
        cJSON_AddNumberToObject(e, "length_m", m->length_m);
        cJSON_AddStringToObject(e, "source", m->source);
        cJSON_AddStringToObject(e, "detail", m->detail);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "forks", arr);
    cJSON_AddStringToObject(root, "written_by", "td5_geo_route.c");
    return gr_write_json(path, root);
}

/* [ROUND 1010 AVENUES] AVENUES.JSON -- the divided avenues of this route, as
 * the REAL map has them.
 *
 *   { "place": "la_plata", "spans": 1066, "units_per_metre": 430,
 *     "lane_width": 1500,
 *     "avenues": [ { "id": "median:Diagonal 73:2-9", "name": "Diagonal 73",
 *                    "s0": 48, "s1": 112, "side": 1, "gap_m": 10.8,
 *                    "gap_min_m": 10.8, "gap_max_m": 10.8, "lanes_opp": 2,
 *                    "length_m": 369, "source": "...", "detail": "...",
 *                    "spans": [ { "s": 48, "off": -4625.0, "lanes": 2,
 *                                 "open": 0 }, ... ] } ],
 *     "written_by": "td5_geo_route.c" }
 *
 * `off` is in WORLD UNITS, positive to the LEFT of travel, measured from the
 * race centreline to the opposite carriageway's centreline -- the same sign and
 * units tg_road_edge's `shift` takes, so the generator lays the scenery
 * carriageway and the median with no conversion and no frame question.
 *
 * ABSENCE IS MEANINGFUL, exactly as for FORKS.JSON: no file means this route
 * has no divided avenue, and a stale one would keep the previous route's span
 * ranges alive. So the empty case DELETES. */
static int gr_write_avenues(const char *dir)
{
    char path[512];
    cJSON *root, *arr;
    int i, q;

    snprintf(path, sizeof path, "%s/AVENUES.JSON", dir);
    if (s_last.n_av < 1) {
        if (td5_plat_file_exists(path)) td5_plat_file_delete(path);
        return 1;
    }
    root = cJSON_CreateObject();
    arr  = cJSON_CreateArray();
    cJSON_AddStringToObject(root, "place", s_last.slug);
    cJSON_AddNumberToObject(root, "spans", s_last.cond.spans);
    cJSON_AddNumberToObject(root, "units_per_metre", GR_UNITS_PER_METRE);
    cJSON_AddNumberToObject(root, "lane_width", GR_LANE_WIDTH);
    for (i = 0; i < s_last.n_av; i++) {
        const GrMedian *m = &s_last.av[i];
        cJSON *e  = cJSON_CreateObject();
        cJSON *sp = cJSON_CreateArray();
        cJSON_AddStringToObject(e, "id", m->id);
        cJSON_AddStringToObject(e, "name", m->name);
        cJSON_AddNumberToObject(e, "s0", s_last.av_s0[i]);
        cJSON_AddNumberToObject(e, "s1", s_last.av_s1[i]);
        cJSON_AddNumberToObject(e, "side", m->side);
        cJSON_AddNumberToObject(e, "gap_m", m->gap_m);
        cJSON_AddNumberToObject(e, "gap_min_m", m->gap_min_m);
        cJSON_AddNumberToObject(e, "gap_max_m", m->gap_max_m);
        cJSON_AddNumberToObject(e, "lanes_opp", m->peer_lanes);
        cJSON_AddNumberToObject(e, "length_m", m->length_m);
        cJSON_AddStringToObject(e, "source", m->source);
        cJSON_AddStringToObject(e, "detail", m->detail);
        for (q = 0; q < s_last.n_av_span; q++) {
            const GrAvSpan *g = &s_last.av_span[q];
            cJSON *r;
            if (g->av != i) continue;
            r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "s", g->span);
            cJSON_AddNumberToObject(r, "off", g->off);
            cJSON_AddNumberToObject(r, "lanes", g->lanes);
            cJSON_AddNumberToObject(r, "open", g->open);
            cJSON_AddItemToArray(sp, r);
        }
        cJSON_AddItemToObject(e, "spans", sp);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "avenues", arr);
    cJSON_AddStringToObject(root, "written_by", "td5_geo_route.c");
    return gr_write_json(path, root);
}

static int gr_write_route(const char *dir)
{
    const GrCond *c = &s_last.cond;
    char path[512];
    cJSON *root = cJSON_CreateObject();
    cJSON *pts, *gs;
    int i;

    snprintf(path, sizeof path, "%s/ROUTE.JSON", dir);
    cJSON_AddBoolToObject(root, "ok", c->ok);
    cJSON_AddStringToObject(root, "direction", c->reversed ? "reversed" : "forward");
    cJSON_AddNumberToObject(root, "spans", c->spans);
    cJSON_AddNumberToObject(root, "nodes", c->nodes);
    cJSON_AddNumberToObject(root, "length_km", c->length_km);
    cJSON_AddNumberToObject(root, "span_length", c->span_length);
    cJSON_AddNumberToObject(root, "lane_width", c->lane_width);
    cJSON_AddNumberToObject(root, "curve_safety_x100", c->curve_safety_x100);
    cJSON_AddNumberToObject(root, "units_per_metre", c->units_per_metre);
    cJSON_AddNumberToObject(root, "elevation_exaggeration", GR_ELEV_EXAGGERATION);
    cJSON_AddNumberToObject(root, "adjacent_skip", c->adjacent_skip);
    cJSON_AddNumberToObject(root, "lead_in_nodes", c->lead_in_nodes);
    cJSON_AddItemToObject(root, "projection", gr_json_proj(&c->proj));
    cJSON_AddNumberToObject(root, "rotation_rad", gr_proj_theta(&c->proj));
    cJSON_AddNumberToObject(root, "offset_x", c->offset_x);
    cJSON_AddNumberToObject(root, "offset_z", c->offset_z);
    cJSON_AddNumberToObject(root, "principal_axis_dev_deg", c->principal_axis_dev_deg);
    cJSON_AddNumberToObject(root, "level_crossings", c->n_level);
    cJSON_AddNumberToObject(root, "grade_separation_m", GR_GRADE_SEPARATION_M);
    cJSON_AddNumberToObject(root, "grade_separation_lift_units", GR_XSEP_LIFT_UNITS);
    cJSON_AddStringToObject(root, "written_by", "td5_geo_route.c");

    /* The REPORT half of the file. The engine reads none of it, but
     * geo_condition.py writes it, geo_audit reads it and the screen shows it,
     * so a C-written ROUTE.JSON has to be a drop-in for a Python-written one
     * rather than a subset that silently loses the quality numbers. */
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "limit_deg", c->curvature.limit_deg);
        cJSON_AddNumberToObject(o, "limit_loose_deg", c->curvature.limit_loose_deg);
        cJSON_AddNumberToObject(o, "worst_before_deg", c->curvature.worst_before_deg);
        cJSON_AddNumberToObject(o, "worst_after_deg", c->curvature.worst_after_deg);
        cJSON_AddNumberToObject(o, "worst_final_deg", c->curvature.worst_final_deg);
        cJSON_AddNumberToObject(o, "iterations", c->curvature.iterations);
        cJSON_AddNumberToObject(o, "over_nodes", c->curvature.over_nodes);
        cJSON_AddNumberToObject(o, "over_nodes_final", c->curvature.over_nodes_final);
        cJSON_AddNumberToObject(o, "min_radius_units", c->curvature.min_radius_units);
        cJSON_AddBoolToObject(o, "converged", c->curvature.converged);
        cJSON_AddItemToObject(root, "curvature", o);
    }
    {
        int g;
        for (g = 0; g < 2; g++) {
            const GrScore *s = g ? &c->final : &c->orientation;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "max_dev_deg", s->max_dev_deg);
            cJSON_AddNumberToObject(o, "mean_dev_deg", s->mean_dev_deg);
            cJSON_AddNumberToObject(o, "over_budget", s->over_budget);
            cJSON_AddNumberToObject(o, "monotone_frac", s->monotone_frac);
            cJSON_AddBoolToObject(o, "inherits_proof", s->inherits_proof);
            cJSON_AddItemToObject(root, g ? "final" : "orientation", o);
        }
    }
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "clamped_low", c->heading.clamped_low);
        cJSON_AddNumberToObject(o, "clamped_high", c->heading.clamped_high);
        cJSON_AddNumberToObject(o, "clamped_total", c->heading.clamped_total);
        cJSON_AddNumberToObject(o, "worst_heading_error_deg", c->heading.worst_deg);
        cJSON_AddItemToObject(root, "heading_bytes", o);
    }
    {
        cJSON *a = cJSON_CreateArray();
        cJSON *w = cJSON_CreateArray();
        for (i = 0; i < c->n_reasons; i++)
            cJSON_AddItemToArray(a, cJSON_CreateString(c->reason[i]));
        for (i = 0; i < c->n_warnings; i++)
            cJSON_AddItemToArray(w, cJSON_CreateString(c->warning[i]));
        cJSON_AddItemToObject(root, "reasons", a);
        cJSON_AddItemToObject(root, "warnings", w);
    }
    cJSON_AddBoolToObject(root, "allow_crossings",
                          td5_env_flag_on("TD5RE_GEO_ROUTE_XLEVEL"));

    /* [OPTION B] ADDITIVE, and it has to stay that way: td5_geo.c reads an
     * absent key as "no grade separation", which is the pre-Option-B build of
     * the same route. Never make it required. */
    gs = cJSON_CreateArray();
    for (i = 0; i < c->n_xsep; i++) {
        const GrXSep *x = &c->xsep[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "site", x->site);
        cJSON_AddNumberToObject(e, "over_lo", x->over_lo);
        cJSON_AddNumberToObject(e, "over_hi", x->over_hi);
        cJSON_AddNumberToObject(e, "under_lo", x->under_lo);
        cJSON_AddNumberToObject(e, "under_hi", x->under_hi);
        cJSON_AddStringToObject(e, "over_leg", x->over_is_b ? "b" : "a");
        cJSON_AddNumberToObject(e, "ramp_spans", x->ramp_spans);
        cJSON_AddNumberToObject(e, "ramp_spans_wanted", x->ramp_spans_wanted);
        cJSON_AddNumberToObject(e, "ramp_room_spans", x->ramp_room_spans);
        cJSON_AddNumberToObject(e, "clearance_units", x->clearance_units);
        cJSON_AddNumberToObject(e, "clearance_m", x->clearance_m);
        cJSON_AddBoolToObject(e, "buildable", x->buildable);
        cJSON_AddStringToObject(e, "why", x->tie ? "tie -> later leg" : "more ramp room");
        cJSON_AddItemToArray(gs, e);
    }
    cJSON_AddItemToObject(root, "grade_separations", gs);

    {
        cJSON *cr = cJSON_CreateArray();
        for (i = 0; i < c->n_sites; i++) {
            const GrSite *s = &c->sites[i];
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "a_lo", s->a_lo);
            cJSON_AddNumberToObject(e, "a_hi", s->a_hi);
            cJSON_AddNumberToObject(e, "b_lo", s->b_lo);
            cJSON_AddNumberToObject(e, "b_hi", s->b_hi);
            cJSON_AddNumberToObject(e, "pairs", s->pairs);
            cJSON_AddNumberToObject(e, "worst_distance_units", s->worst_distance_units);
            cJSON_AddNumberToObject(e, "need_units", s->need_units);
            cJSON_AddBoolToObject(e, "grade_separated", s->grade_separated);
            cJSON_AddBoolToObject(e, "grade_separation_planned", s->grade_separation_planned);
            cJSON_AddItemToArray(cr, e);
        }
        cJSON_AddItemToObject(root, "crossings", cr);
    }

    pts = cJSON_CreateArray();
    for (i = 0; i < c->nodes_xz.n; i++) {
        cJSON *p = cJSON_CreateObject();
        /* 3 decimals, like the Python writer: the loader's chord tolerance is
         * one unit, so this is three orders of magnitude of margin. */
        cJSON_AddNumberToObject(p, "x", gr_round_even(c->nodes_xz.x[i] * 1000.0) / 1000.0);
        cJSON_AddNumberToObject(p, "z", gr_round_even(c->nodes_xz.z[i] * 1000.0) / 1000.0);
        cJSON_AddNumberToObject(p, "lanes", c->lanes_out[i]);
        cJSON_AddItemToArray(pts, p);
    }
    cJSON_AddItemToObject(root, "points", pts);
    return gr_write_json(path, root);
}

/* ======================================================================== *
 * SECTION: THE COMMIT -- SOURCE -> DERIVED
 *
 * [ROUND 1008] This section used to rewrite the place cache IN PLACE, and that
 * was the round-1007 corruption. Commit 1 re-gridded the fetched rasters into
 * the route frame and overwrote them; commit 2 read those back as if they were
 * the fetched data and re-gridded AGAIN; by commit 3 La Plata was a 2x2 grid in
 * 76-byte files, ROADS.JSON held no usable road, 566 of 566 signals were
 * malformed and the screen said "NO MAP DATA / unable to read road graph".
 * Nothing in the flow was individually wrong -- the design was: a derivation
 * whose output is its own next input diverges, and it only takes two presses of
 * BUILD.
 *
 * The rule now, and it is the whole fix:
 *
 *     THE SOURCE IS IMMUTABLE. The commit READS re/assets/geo/<slug>/ and
 *     WRITES re/assets/geo/<slug>/_route/ . Never the other way, never both.
 *
 * So every commit starts from the same bytes, and N commits of the same route
 * give N identical derived frames. td5_geo.h's three path resolvers are the
 * only place a per-place path is spelled, which makes that rule greppable.
 *
 * The commit is also ATOMIC to a reader: the derived stamp (DERIVED.OK) is
 * deleted first and written last, and a derived frame with no stamp is ignored
 * wholesale, so an interrupted or refused BUILD leaves the place racing off its
 * source rather than off half a frame.
 *
 * DEVIATION (c), unchanged and stated plainly. The Python SEND TO GAME re-runs
 * geo_fetch with --frame-from so the DEM, land cover, water and canopy are
 * re-SAMPLED from their sources into the route's frame. Those sources are
 * Overpass JSON, a terrarium PNG pyramid and two cloud-optimised GeoTIFFs; the
 * only local copies are the HTTP range fragments under <place>/_cache/, which
 * cannot be read without porting geo_fetch's COG header walk, a PNG decoder and
 * the OSM overlay painter. That is a round of its own.
 *
 * What is done instead: the SOURCE rasters are re-gridded into the new frame.
 * Both frames are rigid transforms of the same lat/lon plane at the same
 * units/metre, so source -> lat/lon -> route composes to one affine map and the
 * re-grid is exact up to a single interpolation step. Crucially it is now ONE
 * step from the fetched data every time, not one more step each press.
 *
 *   HEIGHT  bilinear. The DEM is already low-passed to 200 m (PLACE.JSON
 *           layers.height.lowpass_m) on 3.49 m cells, so one more bilinear tap
 *           is far below the signal's own resolution.
 *   COVER / WATER / CANOPY  nearest. They are class ids and a max-pooled
 *           height; interpolating them would invent classes. A boundary moves
 *           by at most half a cell, 1.75 m.
 *   COVERAGE  the grid is the world-space bbox of the SAME lat/lon box, so the
 *           new grid's corners stick out past the source grid's and land on
 *           nodata. That is geometry, not a bug: the fetched data covers a
 *           rectangle that is rotated in the route frame, so its axis-aligned
 *           box has empty corners. What matters is the corridor UNDER THE
 *           ROUTE, and the guard below measures exactly that.
 *
 * The VECTOR layers are not resampled at all: ROADS/BUILDINGS/AREAS/SIGNALS are
 * re-projected point by point through the same affine, which is lossless.
 *
 * THE GUARD. Everything is built in memory and checked BEFORE a byte is
 * written, because the failure the user saw was silent: each stage "succeeded"
 * on degenerate input. A refusal names itself on the screen through
 * td5_geo_route_commit_reason() instead of becoming a track made of water.
 * ======================================================================== */

/* Grid sanity. 32 cells is 112 m of world at the 3.49 m cell -- far below any
 * real place and far above the 2x2 the corruption produced. */
#define GR_GUARD_MIN_GRID          32
#define GR_GUARD_MAX_GRID       16384
/* Half-width, in cells, of the corridor sampled around each route node. 2 is
 * +/- 7 m at the La Plata cell, which is the road and its verges. */
#define GR_GUARD_CORRIDOR_CELLS     2
/* Share of corridor samples allowed to miss the source grid. A clean re-grid
 * is 0; anything above this means the route is not under the fetched data. */
#define GR_GUARD_CORRIDOR_PCT       5
/* A derived ROADS.JSON with fewer ways than the router's own floor cannot be
 * routed or built on, and is the shape "unable to read road graph" takes. */
#define GR_GUARD_MIN_ROADS   GR_GRAPH_MIN_WAYS

typedef struct {
    int      kind;               /* 1 = int16, 2 = uint8 */
    int      w, h;
    double   origin_x, origin_z, cell, scale, bias, rotation;
    int      nodata_raw;
    void    *data;
} GrRaster;

#define GR_RASTER_HEADER 72

static double gr_rd_f64(const unsigned char *p)
{
    double v; memcpy(&v, p, 8); return v;
}
static int gr_rd_i32(const unsigned char *p)
{
    int v; memcpy(&v, p, 4); return v;
}
static void gr_wr_f64(unsigned char *p, double v) { memcpy(p, &v, 8); }
static void gr_wr_i32(unsigned char *p, int v)    { memcpy(p, &v, 4); }

static void gr_raster_free(GrRaster *r) { free(r->data); memset(r, 0, sizeof *r); }

static int gr_raster_read(const char *path, GrRaster *out)
{
    int64_t n = 0;
    char *buf = gr_slurp(path, &n);
    const unsigned char *p;
    size_t want;
    memset(out, 0, sizeof *out);
    if (!buf) return 0;
    if (n < GR_RASTER_HEADER || memcmp(buf, "TD5GEOR1", 8)) { free(buf); return 0; }
    p = (const unsigned char *)buf;
    out->kind     = gr_rd_i32(p + 8);
    out->w        = gr_rd_i32(p + 12);
    out->h        = gr_rd_i32(p + 16);
    out->origin_x = gr_rd_f64(p + 20);
    out->origin_z = gr_rd_f64(p + 28);
    out->cell     = gr_rd_f64(p + 36);
    out->scale    = gr_rd_f64(p + 44);
    out->bias     = gr_rd_f64(p + 52);
    out->nodata_raw = gr_rd_i32(p + 60);
    out->rotation = gr_rd_f64(p + 64);
    if (out->w < 1 || out->h < 1 || (out->kind != 1 && out->kind != 2)) { free(buf); return 0; }
    want = (size_t)out->w * (size_t)out->h * (out->kind == 1 ? 2u : 1u);
    if ((size_t)(n - GR_RASTER_HEADER) < want) { free(buf); return 0; }
    out->data = malloc(want);
    if (!out->data) { free(buf); return 0; }
    memcpy(out->data, buf + GR_RASTER_HEADER, want);
    free(buf);
    return 1;
}

static int gr_raster_write(const char *path, const GrRaster *r)
{
    const size_t n = (size_t)r->w * (size_t)r->h * (r->kind == 1 ? 2u : 1u);
    unsigned char *buf = (unsigned char *)malloc(GR_RASTER_HEADER + n);
    int ok;
    if (!buf) return 0;
    memcpy(buf, "TD5GEOR1", 8);
    gr_wr_i32(buf + 8,  r->kind);
    gr_wr_i32(buf + 12, r->w);
    gr_wr_i32(buf + 16, r->h);
    gr_wr_f64(buf + 20, r->origin_x);
    gr_wr_f64(buf + 28, r->origin_z);
    gr_wr_f64(buf + 36, r->cell);
    gr_wr_f64(buf + 44, r->scale);
    gr_wr_f64(buf + 52, r->bias);
    gr_wr_i32(buf + 60, r->nodata_raw);
    gr_wr_f64(buf + 64, r->rotation);
    memcpy(buf + GR_RASTER_HEADER, r->data, n);
    ok = gr_write_atomic(path, buf, GR_RASTER_HEADER + n);
    free(buf);
    return ok;
}

/* Compose source <- route as one affine on cell indices: a route-frame cell
 * (ix, iz) lands at source world (ax*ix + bx*iz + cx, az*ix + bz*iz + cz). Both
 * frames share the lat/lon plane and the units/metre, so the composition is a
 * pure rotation and translation and no trigonometry runs per cell. */
static void gr_affine_old_from_new(const GeoProj *np, const GeoProj *op,
                                   double ox_new, double oz_new, double cell,
                                   double *a)
{
    double x0, z0, x1, z1, x2, z2, la, lo;
    gr_proj_to_latlon(np, ox_new, oz_new, &la, &lo);
    gr_proj_to_world(op, la, lo, &x0, &z0);
    gr_proj_to_latlon(np, ox_new + cell, oz_new, &la, &lo);
    gr_proj_to_world(op, la, lo, &x1, &z1);
    gr_proj_to_latlon(np, ox_new, oz_new + cell, &la, &lo);
    gr_proj_to_world(op, la, lo, &x2, &z2);
    a[0] = x1 - x0; a[1] = x2 - x0; a[2] = x0;      /* ax, bx, cx */
    a[3] = z1 - z0; a[4] = z2 - z0; a[5] = z0;      /* az, bz, cz */
}

/* Re-grid ONE source layer into memory. Returns 1 on success, 0 on failure and
 * -1 when the place simply has no such layer (CANOPY.R8 is optional).
 *
 * Nothing is written here. The caller checks every layer first and only then
 * puts the whole derived frame on disk -- a half-written frame is the state the
 * stamp exists to make unreadable, and not producing one at all is better
 * still. */
static int gr_regrid_mem(const char *src_dir, const char *name, int bilinear,
                         const GeoProj *np, const GeoProj *op,
                         double ox, double oz, int nw, int nh,
                         GrRaster *out, long *out_nodata)
{
    char path[512];
    GrRaster src;
    double a[6];
    int ix, iz;
    long nodata = 0;

    memset(out, 0, sizeof *out);
    if (out_nodata) *out_nodata = 0;

    snprintf(path, sizeof path, "%s/%s", src_dir, name);
    if (!td5_plat_file_exists(path)) return -1;
    if (!gr_raster_read(path, &src)) return 0;

    out->kind = src.kind; out->w = nw; out->h = nh;
    out->origin_x = ox; out->origin_z = oz; out->cell = src.cell;
    out->scale = src.scale; out->bias = src.bias;
    out->nodata_raw = src.nodata_raw;
    out->rotation = gr_proj_theta(np);
    out->data = malloc((size_t)nw * (size_t)nh * (src.kind == 1 ? 2u : 1u));
    if (!out->data) { gr_raster_free(&src); return 0; }

    gr_affine_old_from_new(np, op, ox, oz, src.cell, a);
    for (iz = 0; iz < nh; iz++) {
        const double rx = a[1] * (double)iz + a[2];
        const double rz = a[4] * (double)iz + a[5];
        for (ix = 0; ix < nw; ix++) {
            const double wx = a[0] * (double)ix + rx;
            const double wz = a[3] * (double)ix + rz;
            const double fx = (wx - src.origin_x) / src.cell;
            const double fz = (wz - src.origin_z) / src.cell;
            if (src.kind == 1) {
                const short *s = (const short *)src.data;
                short v = (short)src.nodata_raw;
                if (bilinear) {
                    const int i0 = (int)floor(fx), j0 = (int)floor(fz);
                    const double tx = fx - (double)i0, tz = fz - (double)j0;
                    if (i0 >= 0 && j0 >= 0 && i0 + 1 < src.w && j0 + 1 < src.h) {
                        const short q00 = s[(size_t)j0 * src.w + i0];
                        const short q10 = s[(size_t)j0 * src.w + i0 + 1];
                        const short q01 = s[(size_t)(j0 + 1) * src.w + i0];
                        const short q11 = s[(size_t)(j0 + 1) * src.w + i0 + 1];
                        if (q00 != src.nodata_raw && q10 != src.nodata_raw &&
                            q01 != src.nodata_raw && q11 != src.nodata_raw) {
                            const double t = (1.0 - tx) * (1.0 - tz) * q00
                                           + tx * (1.0 - tz) * q10
                                           + (1.0 - tx) * tz * q01
                                           + tx * tz * q11;
                            v = (short)gr_round_even(t);
                        }
                    }
                }
                if (v == (short)src.nodata_raw) {
                    const int i0 = (int)gr_round_even(fx), j0 = (int)gr_round_even(fz);
                    if (i0 >= 0 && j0 >= 0 && i0 < src.w && j0 < src.h)
                        v = s[(size_t)j0 * src.w + i0];
                }
                if (v == (short)src.nodata_raw) nodata++;
                ((short *)out->data)[(size_t)iz * nw + ix] = v;
            } else {
                const unsigned char *s = (const unsigned char *)src.data;
                const int i0 = (int)gr_round_even(fx), j0 = (int)gr_round_even(fz);
                unsigned char v = (unsigned char)(src.nodata_raw & 0xFF);
                if (i0 >= 0 && j0 >= 0 && i0 < src.w && j0 < src.h)
                    v = s[(size_t)j0 * src.w + i0];
                else nodata++;
                ((unsigned char *)out->data)[(size_t)iz * nw + ix] = v;
            }
        }
    }
    if (out_nodata) *out_nodata = nodata;
    gr_raster_free(&src);
    return 1;
}

/* Share of HEIGHT samples under the route that fell off the source grid, in
 * percent. This is the number that says whether the derived frame is usable:
 * the far corners of the box are allowed to be empty, the road is not. */
static double gr_corridor_nodata_pct(const GrRaster *h, const GrPts *nodes,
                                     long *out_samples, long *out_missing)
{
    long total = 0, miss = 0;
    int i;
    if (out_samples) *out_samples = 0;
    if (out_missing) *out_missing = 0;
    if (!h->data || h->kind != 1 || !nodes->n) return 0.0;
    for (i = 0; i < nodes->n; i++) {
        const double fx = (nodes->x[i] - h->origin_x) / h->cell;
        const double fz = (nodes->z[i] - h->origin_z) / h->cell;
        const int cx = (int)gr_round_even(fx), cz = (int)gr_round_even(fz);
        int dx, dz;
        for (dz = -GR_GUARD_CORRIDOR_CELLS; dz <= GR_GUARD_CORRIDOR_CELLS; dz++)
            for (dx = -GR_GUARD_CORRIDOR_CELLS; dx <= GR_GUARD_CORRIDOR_CELLS; dx++) {
                const int ix = cx + dx, iz = cz + dz;
                total++;
                if (ix < 0 || iz < 0 || ix >= h->w || iz >= h->h) { miss++; continue; }
                if (((const short *)h->data)[(size_t)iz * h->w + ix]
                    == (short)h->nodata_raw) miss++;
            }
    }
    if (out_samples) *out_samples = total;
    if (out_missing) *out_missing = miss;
    return total ? (100.0 * (double)miss / (double)total) : 0.0;
}

/* Re-project every world-unit coordinate of a SOURCE vector layer into the
 * route frame and hand the tree back. Lossless: the two frames differ by a
 * rigid transform of the same plane. Returns NULL when the layer is absent or
 * unreadable; *count is the number of entries, -1 when the file is not there
 * at all (which is not a failure -- not every place has SIGNALS.JSON). */
static cJSON *gr_reproject_mem(const char *src_dir, const char *file,
                               const char *array_key,
                               const GeoProj *np, const GeoProj *op,
                               int *count)
{
    char path[512];
    char *json;
    cJSON *root, *arr;
    int i, n;

    if (count) *count = -1;
    snprintf(path, sizeof path, "%s/%s", src_dir, file);
    if (!td5_plat_file_exists(path)) return NULL;
    json = gr_slurp(path, NULL);
    if (!json) return NULL;
    root = cJSON_Parse(json);
    free(json);
    if (!root) { if (count) *count = 0; return NULL; }
    arr = cJSON_GetObjectItem(root, array_key);
    if (!arr || !cJSON_IsArray(arr)) { if (count) *count = 0; cJSON_Delete(root); return NULL; }
    n = cJSON_GetArraySize(arr);
    for (i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        cJSON *pts = e ? cJSON_GetObjectItem(e, "points") : NULL;
        cJSON *ex = e ? cJSON_GetObjectItem(e, "x") : NULL;
        cJSON *ez = e ? cJSON_GetObjectItem(e, "z") : NULL;
        double la, lo, x, z;
        if (pts && cJSON_IsArray(pts)) {
            const int m = cJSON_GetArraySize(pts);
            int k;
            for (k = 0; k < m; k++) {
                cJSON *p  = cJSON_GetArrayItem(pts, k);
                cJSON *px = p ? cJSON_GetObjectItem(p, "x") : NULL;
                cJSON *pz = p ? cJSON_GetObjectItem(p, "z") : NULL;
                if (!cJSON_IsNumber(px) || !cJSON_IsNumber(pz)) continue;
                gr_proj_to_latlon(op, px->valuedouble, pz->valuedouble, &la, &lo);
                gr_proj_to_world(np, la, lo, &x, &z);
                cJSON_SetNumberValue(px, x);
                cJSON_SetNumberValue(pz, z);
            }
        }
        if (cJSON_IsNumber(ex) && cJSON_IsNumber(ez)) {
            gr_proj_to_latlon(op, ex->valuedouble, ez->valuedouble, &la, &lo);
            gr_proj_to_world(np, la, lo, &x, &z);
            cJSON_SetNumberValue(ex, x);
            cJSON_SetNumberValue(ez, z);
        }
    }
    if (count) *count = n;
    return root;
}

/* The derived PLACE.JSON: the SOURCE one with the route frame substituted, plus
 * a provenance stamp so a cache on disk says which frame it is in and what it
 * came from. Nothing is written back to the source copy. */
static cJSON *gr_derived_place(const char *src_dir, const GeoProj *np,
                               int nw, int nh)
{
    char path[512];
    char *json;
    cJSON *root, *pr, *d;
    snprintf(path, sizeof path, "%s/PLACE.JSON", src_dir);
    json = gr_slurp(path, NULL);
    if (!json) return NULL;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return NULL;
    cJSON_DeleteItemFromObject(root, "projection");
    cJSON_AddItemToObject(root, "projection", gr_json_proj(np));
    pr = cJSON_GetObjectItem(root, "rotation_rad");
    if (pr) cJSON_SetNumberValue(pr, gr_proj_theta(np));
    else cJSON_AddNumberToObject(root, "rotation_rad", gr_proj_theta(np));
    pr = cJSON_GetObjectItem(root, "offset_x");
    if (pr) cJSON_SetNumberValue(pr, np->off_x);
    else cJSON_AddNumberToObject(root, "offset_x", np->off_x);
    pr = cJSON_GetObjectItem(root, "offset_z");
    if (pr) cJSON_SetNumberValue(pr, np->off_z);
    else cJSON_AddNumberToObject(root, "offset_z", np->off_z);
    /* route_graph_bbox is a ROUTER input and the router reads the SOURCE, so a
     * derived copy of it would be read by nobody. It used to be WRITTEN into
     * the source here, to stop a SEND TO GAME widening the graph; with an
     * immutable source the graph cannot widen and the pin is unnecessary. An
     * existing pin in a shipped source is still honoured (gr_place_read). */
    cJSON_DeleteItemFromObject(root, "route_graph_bbox");
    d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "frame", "route");
    cJSON_AddStringToObject(d, "derived_by", "td5_geo_route.c");
    cJSON_AddStringToObject(d, "derived_from", "../PLACE.JSON (the fetched source, never modified)");
    cJSON_AddNumberToObject(d, "grid_w", nw);
    cJSON_AddNumberToObject(d, "grid_h", nh);
    cJSON_AddItemToObject(root, "derived", d);
    return root;
}

/* -------------------------------------------------- refusal, named on screen */

static char s_commit_reason[160];

const char *td5_geo_route_commit_reason(void) { return s_commit_reason; }

static int gr_commit_refuse(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_commit_reason, sizeof s_commit_reason, fmt, ap);
    va_end(ap);
    TD5_LOG_E(LOG_TAG, "geo route: COMMIT REFUSED -- %s", s_commit_reason);
    return 1;
}

int td5_geo_route_commit(void)
{
    const GrCond *c = &s_last.cond;
    char src_dir[256], dst_dir[300], path[512], stamp[512];
    GeoProj oldp;
    double corners_x[4], corners_z[4], x0, x1, z0, z1, cell, corridor_pct = 0.0;
    long corridor_n = 0, corridor_miss = 0;
    int nw, nh, i, rc = 1;
    long nodata_total = 0;
    const uint64_t t_us = td5_plat_time_us();

    /* Everything is built here and written in one go at the end. */
    static const struct { const char *name; int bilinear; } k_rast[] = {
        { "HEIGHT.R16", 1 }, { "COVER.R8", 0 }, { "WATER.R8", 0 }, { "CANOPY.R8", 0 },
    };
    static const struct { const char *file; const char *key; } k_vec[] = {
        { "ROADS.JSON", "roads" }, { "BUILDINGS.JSON", "buildings" },
        { "AREAS.JSON", "areas" }, { "SIGNALS.JSON",   "signals"   },
    };
    GrRaster rast[4];
    int       rast_have[4];
    cJSON    *vec[4];
    int       vec_n[4];
    cJSON    *place = NULL;

    memset(rast, 0, sizeof rast);
    memset(rast_have, 0, sizeof rast_have);
    memset(vec, 0, sizeof vec);
    for (i = 0; i < 4; i++) vec_n[i] = -1;
    s_commit_reason[0] = '\0';

    if (!s_last.valid || !c->nodes_xz.n)
        return gr_commit_refuse("NO USABLE ROUTE TO SAVE");

    td5_geo_source_path(src_dir, sizeof src_dir, s_last.slug, "");
    {   /* td5_geo_source_path leaves a trailing '/' for an empty name. */
        const size_t L = strlen(src_dir);
        if (L && src_dir[L - 1] == '/') src_dir[L - 1] = '\0';
    }
    snprintf(dst_dir, sizeof dst_dir, "%s/%s", src_dir, TD5_GEO_DERIVED_DIR);

    /* Re-read the SOURCE frame rather than trusting whatever s_g holds: the
     * commit's one input is the fetched cache, and nothing it did earlier in
     * the session may change what that is. */
    if (!gr_place_read(s_last.slug))
        return gr_commit_refuse("THE PLACE DATA HAS NO USABLE PROJECTION");
    oldp = s_g.proj;

    /* -------- 1. size the route-frame grid from the SOURCE lat/lon box ------ */
    cell = 0.0;
    {
        char hp[512];
        GrRaster probe;
        snprintf(hp, sizeof hp, "%s/HEIGHT.R16", src_dir);
        if (!gr_raster_read(hp, &probe))
            return gr_commit_refuse("THE PLACE HAS NO READABLE TERRAIN DATA");
        cell = probe.cell;
        gr_raster_free(&probe);
    }
    if (!(cell > 0.0)) return gr_commit_refuse("THE TERRAIN DATA HAS NO CELL SIZE");
    {
        const double lats[2] = { s_g.bbox[1], s_g.bbox[3] };   /* south, north */
        const double lons[2] = { s_g.bbox[0], s_g.bbox[2] };   /* west,  east  */
        int k = 0, a, b;
        for (a = 0; a < 2; a++) for (b = 0; b < 2; b++) {
            gr_proj_to_world(&c->proj, lats[a], lons[b], &corners_x[k], &corners_z[k]);
            k++;
        }
        x0 = x1 = corners_x[0]; z0 = z1 = corners_z[0];
        for (i = 1; i < 4; i++) {
            if (corners_x[i] < x0) x0 = corners_x[i];
            if (corners_x[i] > x1) x1 = corners_x[i];
            if (corners_z[i] < z0) z0 = corners_z[i];
            if (corners_z[i] > z1) z1 = corners_z[i];
        }
        nw = (int)ceil((x1 - x0) / cell) + 1;
        nh = (int)ceil((z1 - z0) / cell) + 1;
    }
    /* GUARD 1: the 2x2 collapse. A grid this small means the bbox read as a
     * point, which is what a corrupted or re-derived PLACE.JSON looks like. */
    if (nw < GR_GUARD_MIN_GRID || nh < GR_GUARD_MIN_GRID)
        return gr_commit_refuse("TERRAIN GRID WOULD BE %dx%d -- PLACE DATA LOOKS DAMAGED",
                                nw, nh);
    if (nw > GR_GUARD_MAX_GRID || nh > GR_GUARD_MAX_GRID)
        return gr_commit_refuse("TERRAIN GRID WOULD BE %dx%d -- TOO LARGE TO BUILD",
                                nw, nh);

    /* -------- 2. re-grid every raster INTO MEMORY, from the source ---------- */
    for (i = 0; i < 4; i++) {
        long nd = 0;
        const int r = gr_regrid_mem(src_dir, k_rast[i].name, k_rast[i].bilinear,
                                    &c->proj, &oldp, x0, z0, nw, nh, &rast[i], &nd);
        if (r < 0) continue;                        /* this place has no such layer */
        if (!r) { gr_commit_refuse("COULD NOT RE-GRID %s", k_rast[i].name); goto done; }
        /* GUARD 2: every layer must land on ONE grid. td5_geo.c drops a mask
         * whose grid disagrees with HEIGHT, so a disagreement here is a place
         * that silently races with no water and no land cover. */
        if (i > 0 && rast_have[0] && rast[i].cell != rast[0].cell) {
            gr_commit_refuse("%s IS ON A DIFFERENT CELL GRID THAN THE TERRAIN",
                             k_rast[i].name);
            goto done;
        }
        rast_have[i] = 1;
        nodata_total += nd;
    }
    if (!rast_have[0]) { gr_commit_refuse("THE PLACE HAS NO READABLE TERRAIN DATA"); goto done; }

    /* GUARD 3: the corridor. Empty corners are geometry; an empty road is a
     * track built on nothing, which is how "a lot of water in the middle of
     * the city" reads from the driver's seat. */
    corridor_pct = gr_corridor_nodata_pct(&rast[0], &c->nodes_xz,
                                          &corridor_n, &corridor_miss);
    if (corridor_pct > (double)GR_GUARD_CORRIDOR_PCT) {
        gr_commit_refuse("%.0f%% OF THE ROUTE HAS NO TERRAIN DATA UNDER IT",
                         corridor_pct);
        goto done;
    }

    /* -------- 3. re-project every vector layer INTO MEMORY ----------------- */
    for (i = 0; i < 4; i++)
        vec[i] = gr_reproject_mem(src_dir, k_vec[i].file, k_vec[i].key,
                                  &c->proj, &oldp, &vec_n[i]);
    /* GUARD 4: a present-but-empty layer. vec_n < 0 means "the place never had
     * this file", which is allowed; 0 entries from a file that exists is the
     * "ROADS.JSON held no usable road" state. */
    if (vec_n[0] >= 0 && vec_n[0] < GR_GUARD_MIN_ROADS) {
        gr_commit_refuse("ONLY %d ROAD(S) SURVIVED -- MAP DATA LOOKS DAMAGED", vec_n[0]);
        goto done;
    }
    for (i = 1; i < 4; i++) {
        if (vec_n[i] == 0) {
            gr_commit_refuse("%s IS PRESENT BUT EMPTY -- MAP DATA LOOKS DAMAGED",
                             k_vec[i].file);
            goto done;
        }
    }

    place = gr_derived_place(src_dir, &c->proj, nw, nh);
    if (!place) { gr_commit_refuse("COULD NOT READ THE PLACE DESCRIPTION"); goto done; }

    /* -------- 4. everything checked out: now write the derived frame ------- */
    _mkdir(dst_dir);
    /* The stamp goes FIRST, so a reader never sees a frame being rebuilt. */
    snprintf(stamp, sizeof stamp, "%s/%s", dst_dir, TD5_GEO_DERIVED_STAMP);
    td5_plat_file_delete(stamp);

    for (i = 0; i < 4; i++) {
        if (!rast_have[i]) continue;
        snprintf(path, sizeof path, "%s/%s", dst_dir, k_rast[i].name);
        if (!gr_raster_write(path, &rast[i])) {
            gr_commit_refuse("COULD NOT WRITE %s", k_rast[i].name);
            goto done;
        }
    }
    for (i = 0; i < 4; i++) {
        if (!vec[i]) continue;
        snprintf(path, sizeof path, "%s/%s", dst_dir, k_vec[i].file);
        if (!gr_write_json(path, vec[i])) {          /* consumes the tree */
            vec[i] = NULL;
            gr_commit_refuse("COULD NOT WRITE %s", k_vec[i].file);
            goto done;
        }
        vec[i] = NULL;
    }
    snprintf(path, sizeof path, "%s/PLACE.JSON", dst_dir);
    if (!gr_write_json(path, place)) {               /* consumes the tree */
        place = NULL;
        gr_commit_refuse("COULD NOT WRITE THE PLACE DESCRIPTION");
        goto done;
    }
    place = NULL;
    if (!gr_write_route_raw(dst_dir)) { gr_commit_refuse("COULD NOT WRITE ROUTE_RAW.JSON"); goto done; }
    if (!gr_write_route(dst_dir))     { gr_commit_refuse("COULD NOT WRITE ROUTE.JSON");     goto done; }

    /* FORKS.JSON is indexed by the SPAN of the route it was confirmed against,
     * so it has to be written in the SAME commit as the ROUTE.JSON it is
     * indexed against -- and since round 1009 the commit has its own forks to
     * write: the divided avenues gr_detect_medians found and the lane widening
     * this route was conditioned for. With none found (or TD5RE_GEO_AVENUES=0)
     * gr_write_forks deletes instead, which is what the commit always did, and
     * the stamp rule makes "absent in the derived frame" mean absent rather
     * than falling back to the source copy. The SOURCE FORKS.JSON (a
     * geo_selector.py artefact) is left alone, where it belongs. */
    if (!gr_write_forks(dst_dir)) { gr_commit_refuse("COULD NOT WRITE FORKS.JSON"); goto done; }

    /* [ROUND 1010 AVENUES] AVENUES.JSON is span-indexed against the SAME
     * ROUTE.JSON, for the same reason, and like FORKS.JSON it DELETES when
     * there is nothing to say. */
    if (!gr_write_avenues(dst_dir)) { gr_commit_refuse("COULD NOT WRITE AVENUES.JSON"); goto done; }

    /* The stamp LAST: from here the readers see the new frame. */
    if (!gr_write_atomic(stamp, "td5_geo_route.c\n", 16)) {
        gr_commit_refuse("COULD NOT STAMP THE BUILT TRACK DATA");
        goto done;
    }
    rc = 0;

done:
    for (i = 0; i < 4; i++) { gr_raster_free(&rast[i]); if (vec[i]) cJSON_Delete(vec[i]); }
    if (place) cJSON_Delete(place);

    /* The graph and everything td5_geo caches belong to the previous frame.
     * td5_geo_invalidate (not plain unload) also drops the PARSED ROUTE and
     * resets the route-want path, so the next td5_geo_sync re-reads the derived
     * ROUTE.JSON and rasters even though the slug and the path string are
     * unchanged -- the in-session BUILD -> race path depends on it. Done on the
     * refusal path too: a refused commit must not leave the session holding
     * half-stale state either. */
    gr_graph_free();
    td5_geo_invalidate();
    td5_geo_places_rescan();
    td5_geo_select(s_last.slug);

    if (rc == 0)
        TD5_LOG_I(LOG_TAG, "geo route: committed %s -> %s: %d spans, %.2f km, "
                  "grid %dx%d cell %.0f, %ld nodata cell(s) overall, corridor "
                  "%ld/%ld missing (%.2f%%), roads %d buildings %d areas %d "
                  "signals %d, %.0f ms", s_last.slug, dst_dir, c->spans,
                  c->length_km, nw, nh, cell, nodata_total, corridor_miss,
                  corridor_n, corridor_pct, vec_n[0], vec_n[1], vec_n[2],
                  vec_n[3], (double)(td5_plat_time_us() - t_us) / 1000.0);
    return rc;
}

/* ======================================================================== *
 * SECTION: lifecycle + the dev parity harness
 * ======================================================================== */

#ifndef TD5RE_RELEASE
/* Condition one raw-route fixture and print every number the Python writes, so
 * a diff against the committed _ROUTE.json fixtures is a mechanical check
 * rather than a judgement. Reads the SAME file geo_condition.py was given.
 *
 *   TD5RE_GEO_ROUTE_TEST=1   condition both fixtures and exit
 *   TD5RE_GEO_ROUTE_TEST=2   also route La Plata from its saved waypoints
 *   TD5RE_GEO_ROUTE_TEST=3   also COMMIT it (writes <place>/_route/, never the
 *                            fetched source -- see the COMMIT section)
 *   TD5RE_GEO_ROUTE_TEST_PTS="lat,lon;lat,lon"  override the waypoints
 */
static int  gr_write_route(const char *dir);
static int  gr_write_route_raw(const char *dir);
static void gr_dump_cond(const char *out_path, GrCond *c);

static void gr_test_fixture(const char *path, int allow_cross)
{
    char *json = gr_slurp(path, NULL);
    cJSON *root, *pts;
    TD5_GeoLatLon *ll = NULL;
    int *lanes = NULL;
    int n, i;
    GrCond c;
    uint64_t t0;

    if (!json) { printf("GEOROUTE fixture %s: NOT READABLE\n", path); return; }
    root = cJSON_Parse(json);
    free(json);
    if (!root) { printf("GEOROUTE fixture %s: bad JSON\n", path); return; }
    pts = cJSON_GetObjectItem(root, "points");
    if (!pts || !cJSON_IsArray(pts) || (n = cJSON_GetArraySize(pts)) < 2) {
        printf("GEOROUTE fixture %s: no points[]\n", path);
        cJSON_Delete(root);
        return;
    }
    ll = (TD5_GeoLatLon *)malloc((size_t)n * sizeof *ll);
    lanes = (int *)malloc((size_t)n * sizeof(int));
    if (!ll || !lanes) { free(ll); free(lanes); cJSON_Delete(root); return; }
    for (i = 0; i < n; i++) {
        const cJSON *p = cJSON_GetArrayItem(pts, i);
        const cJSON *a = cJSON_GetObjectItem(p, "lat");
        const cJSON *o = cJSON_GetObjectItem(p, "lon");
        const cJSON *l = cJSON_GetObjectItem(p, "lanes");
        ll[i].lat = cJSON_IsNumber(a) ? a->valuedouble : 0.0;
        ll[i].lon = cJSON_IsNumber(o) ? o->valuedouble : 0.0;
        lanes[i]  = (cJSON_IsNumber(l) && l->valueint) ? l->valueint : 2;
    }
    cJSON_Delete(root);

    t0 = td5_plat_time_us();
    gr_condition(ll, lanes, n, GR_UNITS_PER_METRE, GR_CURVE_SAFETY_X100,
                 GR_SPAN_LENGTH, GR_LANE_WIDTH, 1, allow_cross, &c);
    printf("\nGEOROUTE fixture %s (%d raw points, allow_crossings=%d)\n", path, n, allow_cross);
    printf("  ok                     %d\n", c.ok);
    printf("  direction              %s\n", c.reversed ? "reversed" : "forward");
    printf("  spans / nodes          %d / %d\n", c.spans, c.nodes);
    printf("  length_km              %.17g\n", c.length_km);
    printf("  adjacent_skip          %d\n", c.adjacent_skip);
    printf("  rotation_rad           %.17g\n", gr_proj_theta(&c.proj));
    printf("  offset_x               %.17g\n", c.offset_x);
    printf("  offset_z               %.17g\n", c.offset_z);
    printf("  proj.lat0              %.17g\n", c.proj.lat0);
    printf("  proj.lon0              %.17g\n", c.proj.lon0);
    printf("  proj.m_per_deg_lat     %.17g\n", c.proj.m_per_deg_lat);
    printf("  proj.m_per_deg_lon     %.17g\n", c.proj.m_per_deg_lon);
    printf("  principal_axis_dev_deg %.17g\n", c.principal_axis_dev_deg);
    printf("  curv.limit_deg         %.17g\n", c.curvature.limit_deg);
    printf("  curv.limit_loose_deg   %.17g\n", c.curvature.limit_loose_deg);
    printf("  curv.worst_before_deg  %.17g\n", c.curvature.worst_before_deg);
    printf("  curv.worst_after_deg   %.17g\n", c.curvature.worst_after_deg);
    printf("  curv.worst_final_deg   %.17g\n", c.curvature.worst_final_deg);
    printf("  curv.iterations        %d\n", c.curvature.iterations);
    printf("  curv.over_nodes_final  %d\n", c.curvature.over_nodes_final);
    printf("  curv.min_radius_units  %.17g\n", c.curvature.min_radius_units);
    printf("  orient.max_dev_deg     %.17g\n", c.orientation.max_dev_deg);
    printf("  orient.mean_dev_deg    %.17g\n", c.orientation.mean_dev_deg);
    printf("  orient.over_budget     %d\n", c.orientation.over_budget);
    printf("  orient.monotone_frac   %.17g\n", c.orientation.monotone_frac);
    printf("  final.max_dev_deg      %.17g\n", c.final.max_dev_deg);
    printf("  final.mean_dev_deg     %.17g\n", c.final.mean_dev_deg);
    printf("  final.over_budget      %d\n", c.final.over_budget);
    printf("  final.monotone_frac    %.17g\n", c.final.monotone_frac);
    printf("  hb.clamped_low/high    %d / %d\n", c.heading.clamped_low, c.heading.clamped_high);
    printf("  hb.worst_deg           %.17g\n", c.heading.worst_deg);
    printf("  crossings              %d (level %d)\n", c.n_sites, c.n_level);
    for (i = 0; i < c.n_sites; i++)
        printf("    site %d: a %d..%d b %d..%d pairs %d worst %.17g need %.17g (%d,%d)\n",
               i, c.sites[i].a_lo, c.sites[i].a_hi, c.sites[i].b_lo, c.sites[i].b_hi,
               c.sites[i].pairs, c.sites[i].worst_distance_units,
               c.sites[i].need_units, c.sites[i].worst_a, c.sites[i].worst_b);
    printf("  grade_separations      %d\n", c.n_xsep);
    for (i = 0; i < c.n_xsep; i++)
        printf("    gs %d: over %d..%d under %d..%d leg %s ramp %d/%d room %d buildable %d\n",
               i, c.xsep[i].over_lo, c.xsep[i].over_hi, c.xsep[i].under_lo,
               c.xsep[i].under_hi, c.xsep[i].over_is_b ? "b" : "a",
               c.xsep[i].ramp_spans, c.xsep[i].ramp_spans_wanted,
               c.xsep[i].ramp_room_spans, c.xsep[i].buildable);
    for (i = 0; i < c.n_reasons; i++) printf("  reason: %s\n", c.reason[i]);
    printf("  node 0                 %.17g %.17g\n", c.nodes_xz.x[0], c.nodes_xz.z[0]);
    printf("  node last              %.17g %.17g\n",
           c.nodes_xz.x[c.nodes_xz.n - 1], c.nodes_xz.z[c.nodes_xz.n - 1]);
    {   /* A hash over every stored coordinate, so a one-node drift is visible
         * without diffing 1493 lines by eye. */
        unsigned long long hsh = 1469598103934665603ull;
        for (i = 0; i < c.nodes_xz.n; i++) {
            const double qx = gr_round_even(c.nodes_xz.x[i] * 1000.0) / 1000.0;
            const double qz = gr_round_even(c.nodes_xz.z[i] * 1000.0) / 1000.0;
            char tmp[80];
            int k;
            snprintf(tmp, sizeof tmp, "%.3f,%.3f,%d", qx, qz, c.lanes_out[i]);
            for (k = 0; tmp[k]; k++) { hsh ^= (unsigned char)tmp[k]; hsh *= 1099511628211ull; }
        }
        printf("  points FNV1a           %016llX\n", (unsigned long long)hsh);
    }
    printf("  condition_ms           %.3f\n",
           (double)(td5_plat_time_us() - t0) / 1000.0);
    {
        const char *base = strrchr(path, '/');
        char dump[200];
        snprintf(dump, sizeof dump, "log/georoute_%.120s", base ? base + 1 : path);
        gr_dump_cond(dump, &c);
        printf("  dumped                 %s\n", dump);
    }
    gr_cond_free(&c);
    free(ll); free(lanes);
}

/* The ROUTE.JSON writer reads s_last.cond, so borrow it for the dump and hand
 * it back. Cheap, and it means the harness exercises the SHIPPING writer
 * rather than a second one that could drift from it. */
static void gr_dump_cond(const char *out_path, GrCond *c)
{
    const GrCond saved = s_last.cond;
    char dir[120], from[200];
    const char *slash = strrchr(out_path, '/');
    size_t n = slash ? (size_t)(slash - out_path) : 0u;
    if (n >= sizeof dir) n = sizeof dir - 1u;
    memcpy(dir, out_path, n);
    dir[n] = '\0';
    s_last.cond = *c;
    if (gr_write_route(dir)) {
        snprintf(from, sizeof from, "%.100s/ROUTE.JSON", dir);
        td5_plat_file_delete(out_path);
        td5_plat_file_rename(from, out_path);
    }
    s_last.cond = saved;
}

static void gr_test_route_live(int level)
{
    char path[512];
    char *json;
    cJSON *root, *wps;
    TD5_GeoLatLon wp[GR_MAX_WAYPOINTS];
    TD5_GeoRouteResult r;
    const char *pts_env = getenv("TD5RE_GEO_ROUTE_TEST_PTS");
    int n = 0, i;

    /* TD5RE_GEO_ROUTE_TEST_PTS="lat,lon;lat,lon[;...]" overrides the saved
     * waypoints, which matters for the commit test: re-committing the SAME
     * route lands in almost the same frame and would not exercise the
     * re-grid at all. */
    if (pts_env && pts_env[0]) {
        const char *p = pts_env;
        while (*p && n < GR_MAX_WAYPOINTS) {
            char *end = NULL;
            const double la = strtod(p, &end);
            if (end == p || *end != ',') break;
            p = end + 1;
            wp[n].lat = la;
            wp[n].lon = strtod(p, &end);
            if (end == p) break;
            n++;
            p = end;
            if (*p == ';') p++;
            else break;
        }
        printf("\nGEOROUTE live: %d waypoint(s) from TD5RE_GEO_ROUTE_TEST_PTS\n", n);
    }
    snprintf(path, sizeof path, "re/assets/geo/la_plata/ROUTE_RAW.JSON");
    if (n < 2) {
        json = gr_slurp(path, NULL);
        if (!json) { printf("\nGEOROUTE live: %s not readable\n", path); return; }
        root = cJSON_Parse(json);
        free(json);
        if (!root) return;
        wps = cJSON_GetObjectItem(root, "waypoints");
        if (wps && cJSON_IsArray(wps)) {
            n = cJSON_GetArraySize(wps);
            if (n > GR_MAX_WAYPOINTS) n = GR_MAX_WAYPOINTS;
            for (i = 0; i < n; i++) {
                const cJSON *e = cJSON_GetArrayItem(wps, i);
                wp[i].lat = cJSON_GetArrayItem(e, 0)->valuedouble;
                wp[i].lon = cJSON_GetArrayItem(e, 1)->valuedouble;
            }
        }
        cJSON_Delete(root);
    }
    if (n < 2) { printf("\nGEOROUTE live: no waypoints in %s\n", path); return; }

    printf("\nGEOROUTE live route from %d waypoint(s) of %s\n", n, path);
    td5_geo_route_build(wp, n, &r);
    printf("  verdict                %d (%s)\n", (int)r.verdict, r.reason);
    printf("  place                  %s\n", r.place_slug);
    printf("  routed polyline        %d point(s), %.4f km\n",
           r.n_path, r.raw_length_m / 1000.0f);
    printf("  spans                  %d\n", r.spans);
    printf("  conditioned length     %.4f km\n", r.length_m / 1000.0f);
    printf("  reversed               %d\n", r.direction_reversed);
    printf("  crossings / level      %d / %d\n", r.n_crossings, r.n_crossings_level);
    printf("  worst turn / limit     %.3f / %.3f deg\n", r.worst_turn_deg, r.limit_turn_deg);
    printf("  monotone               %.2f %%\n", r.monotone_pct);
    printf("  streets                %s\n", r.streets);
    printf("  build_ms               %.3f\n", r.build_ms);
    for (i = 0; i < r.n_path && i < 3; i++)
        printf("  path[%d]                %.7f %.7f\n", i, r.path[i].lat, r.path[i].lon);
    if (r.n_path)
        printf("  path[last]             %.7f %.7f\n",
               r.path[r.n_path - 1].lat, r.path[r.n_path - 1].lon);
    /* Dump the ROUTED polyline through the shipping ROUTE_RAW writer, so the
     * router half can be diffed against the saved file point by point. */
    if (r.verdict == TD5_GEO_ROUTE_OK && gr_write_route_raw("log")) {
        td5_plat_file_delete("log/georoute_live_ROUTE_RAW.JSON");
        td5_plat_file_rename("log/ROUTE_RAW.JSON", "log/georoute_live_ROUTE_RAW.JSON");
        printf("  dumped                 log/georoute_live_ROUTE_RAW.JSON\n");
    }
    /* Second build with the graph already cached: that is the cost the screen
     * pays on every waypoint drag, and the one the plan's "well under a
     * second" target is about. */
    td5_geo_route_build(wp, n, &r);
    printf("  build_ms (graph warm)  %.3f\n", r.build_ms);
    printf("  streets (full)         %s\n", r.streets);

    /* Level 3 also COMMITS: writes both JSONs, re-grids the four rasters into
     * the route frame, re-projects the four vector layers and selects the
     * place. It writes re/assets/geo/<slug>/_route/ and never the source
     * (see the COMMIT section), which is why it is a
     * separate level and not part of the default harness. */
    if (level >= 3) {
        const uint64_t t0 = td5_plat_time_us();
        const int rc = td5_geo_route_commit();
        printf("  COMMIT rc              %d (0 = ok)\n", rc);
        printf("  commit_ms              %.1f\n",
               (double)(td5_plat_time_us() - t0) / 1000.0);
    }
}

static void gr_self_test(int level)
{
    printf("=== td5_geo_route parity harness ===\n");
    printf("adjacent_skip(default)   %d\n",
           gr_adjacent_skip(GR_LANE_WIDTH, GR_SPAN_LENGTH, GR_CURVE_SAFETY_X100));
    printf("lead_in_nodes            %d (synthetic walk uses %d)\n",
           gr_lead_in_nodes(), GR_LEAD_IN_SYNTH);
    gr_test_fixture("re/tools/geo_fixtures/la_plata_route_raw.json", 0);
    gr_test_fixture("re/tools/geo_fixtures/figure8_route_raw.json", 1);
    if (level >= 2) gr_test_route_live(level);
    printf("\n=== end ===\n");
    fflush(stdout);
}
#endif /* !TD5RE_RELEASE */

/* 0: this IS the real router. The L2 placeholder (td5_geo_route_stub.c, gone
 * at the round-1007 integration) returned 1 and the screen greyed BUILD TRACK
 * on it. Kept rather than deleted so the screen needs no edit and so a future
 * placeholder has the same seam to sit behind. */
int td5_geo_route_is_stub(void)
{
    return 0;
}

int td5_geo_route_init(void)
{
#ifndef TD5RE_RELEASE
    const int lvl = td5_env_int("TD5RE_GEO_ROUTE_TEST", 0, 0, 3);
    if (lvl > 0) {
        gr_self_test(lvl);
        td5_geo_route_shutdown();
        exit(0);
    }
#endif
    return 1;
}

void td5_geo_route_shutdown(void)
{
    gr_graph_free();
    gr_last_free();
    free(s_path_ll);  s_path_ll = NULL;
    free(s_cross_ll); s_cross_ll = NULL;
    memset(&s_result, 0, sizeof s_result);
}
