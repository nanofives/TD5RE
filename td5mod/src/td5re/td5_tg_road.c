/**
 * td5_tg_road.c -- auto-track ROAD on the WORLD: terrain-steered centerline walk, structure table (bridge/tunnel by terrain, not by hash), grade-limited terrain-following elevation, road-bed conform
 *
 * [TOPOLOGY-FIRST 2026-09-08] Replaces the spine-walk + sine-profile pair
 * (tg_build_centerline / tg_apply_elevation in td5_trackgen.c) and the
 * positional-hash bridge/tunnel gates. The road is laid ONTO td5_tg_world.c:
 *
 *  1. The WALK keeps everything the old one proved -- the section picker and
 *     its R21 per-biome mix, the +-80/88 deg heading budget about +X (the
 *     non-trapping / no-self-intersection theorem), lane management, fork
 *     windows, the section rollback -- and adds STEERING: a section is
 *     simulated for each candidate direction, its spans are classified
 *     against the terrain, and the cheapest candidate wins. A section whose
 *     tunnel or bridge would exceed its cap is REJECTED like a self-overlap.
 *
 *  2. The PROFILE is causal and windowed. y follows the terrain through a
 *     forward+backward grade limiter over the last TG_ROAD_WINDOW spans; a
 *     node's y and its spans' structure kind become FINAL once the head is
 *     TG_ROAD_WINDOW spans past it, so the walk-time gates (lane blackouts,
 *     curvature clamps) and the final road agree by construction -- there is
 *     no second pass that could move a span from open road into a tunnel.
 *
 *  3. STRUCTURES are DETECTED: where the limited road ends up more than
 *     TG_ROAD_TUNNEL_DEPTH below the ground it is a TUNNEL, more than
 *     TG_ROAD_BRIDGE_LIFT above it (or over water at all) it is a BRIDGE,
 *     otherwise the terrain is CONFORMED to the road bed (cut/fill). Runs are
 *     coalesced, held to min/max lengths, kept clear of each other, of fork
 *     windows and of lane seams.
 *
 *  4. tg_span_in_bridge_run / tg_span_in_tunnel keep their signatures and
 *     become reads of the structure table, so the ~150 consumers are
 *     untouched.
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"

/* ----------------------------------------------------------- constants -- */

#define TG_ROAD_WINDOW        64      /* spans behind the head still revisable */
#define TG_ROAD_TUNNEL_DEPTH  2200.0  /* road this far under ground = bore     */
#define TG_ROAD_BRIDGE_LIFT   2000.0  /* road this far over ground = viaduct   */
#define TG_ROAD_DECK_CLEAR    900.0   /* deck above the water surface          */
#define TG_ROAD_BRIDGE_MIN    6       /* land viaduct min spans                */
#define TG_ROAD_WATER_MIN     2       /* water crossing min spans (+abutments) */
#define TG_ROAD_TUNNEL_MIN    6
#define TG_ROAD_GRADE_ABSMAX  0.20    /* any biome                             */
#define TG_ROAD_CONFORM_BLEND 4500.0  /* base fade back to the natural ground  */

static int tg_road_grade_cmp(const void *a, const void *b)
{
    const double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static int tg_bridge_max_spans(void) { return td5_env_int("TD5RE_TG_BRIDGE_MAX", 56, 4, 400); }
static int tg_tunnel_max_spans(void) { return td5_env_int("TD5RE_TG_TUNNEL_MAX", 32, 4, 400); }
static int tg_bridges_enabled(void)  { return td5_env_flag_on("TD5RE_AUTOTRACK_BRIDGES"); }
static int tg_tunnels_enabled(void)  { return td5_env_flag_on("TD5RE_AUTOTRACK_TUNNELS"); }
static int tg_steer_enabled(void)    { return td5_env_flag_on("TD5RE_TG_STEER"); }

/* ------------------------------------------------------ structure table -- */

static unsigned char s_struct[TD5_TG_MAX_SPANS + 8];   /* TG_ST_* per span   */
static int s_struct_n;      /* spans classified (provisional + final)         */
static int s_struct_fin;    /* spans whose kind is FINAL                      */

int tg_struct_kind(int si)
{
    if (si < 0 || si >= s_struct_n) return TG_ST_NONE;
    return s_struct[si];
}

void tg_struct_run_bounds(int si, int *s0, int *s1)
{
    const int k = tg_struct_kind(si);
    int a = si, b = si;
    if (k != TG_ST_NONE) {
        while (a - 1 >= 0 && s_struct[a - 1] == k) a--;
        while (b + 1 < s_struct_n && s_struct[b + 1] == k) b++;
    }
    if (s0) *s0 = a;
    if (s1) *s1 = b;
}

int tg_span_in_bridge_run(int si) { return tg_struct_kind(si) == TG_ST_BRIDGE; }
int tg_span_in_tunnel(int si)     { return tg_struct_kind(si) == TG_ST_TUNNEL; }

int tg_span_near_bridge(int si, int clear)
{
    int k;
    for (k = -clear; k <= clear; k++)
        if (si + k >= 0 && tg_span_in_bridge_run(si + k)) return 1;
    return 0;
}

/* ------------------------------------------------------------ profile -- */

/* Per-node road-profile state, parallel to the node list. */
typedef struct {
    double t;           /* terrain target for the road surface               */
    double h;           /* ground under the node                             */
    double wy;          /* water surface under the node (or -1e30)           */
    double capg;        /* grade budget for the span ENDING at this node     */
    unsigned char wet;  /* water under the node                              */
    unsigned char force;/* forced conform (planner gave up on a structure)   */
} TG_RoadNode;

static TG_RoadNode s_rn[TD5_TG_MAX_SPANS + 8];
static int         s_rn_n;
static const TD5_TrackGenSpec *s_spec;
static double      s_max_grade;         /* spec aim (0..)                    */
static long        s_stat_conforms, s_stat_capfail, s_stat_steer_wins;

static double tg_road_cap_at(int i)
{
    double c = s_max_grade * TD5_TG_R21_GRADE_HEADROOM
             * (double)tg_shape_lerp_pct(i, TG_SF_GRADE, TD5_TG_BIOME_BLEND) / 100.0;
    if (c > TG_ROAD_GRADE_ABSMAX) c = TG_ROAD_GRADE_ABSMAX;
    if (c < 0.02) c = 0.02;
    return c;
}

static void tg_road_node_fill(int i, const TG_Node *n)
{
    TG_RoadNode *r = &s_rn[i];
    r->h  = tg_world_h(n->x, n->z);
    r->wy = tg_world_water_y(n->x, n->z);
    r->wet = (r->h < r->wy) ? 1 : 0;
    r->t  = r->wet ? r->wy + TG_ROAD_DECK_CLEAR : r->h;
    r->capg = tg_road_cap_at(i) * (double)s_spec->span_length;
    r->force = 0;
}

/* [R22 item 6] Curvature (grade-change) cap in y-units per span, or 0 = off.
 * The slope limiter below bounds the GRADE but not how fast the grade changes,
 * so it permits kinks -- the "sloppy" ride the item is about. This caps the
 * per-span change in grade so the profile bends into and out of a slope instead
 * of snapping, without touching the walk's X/Z (topology stays). Knob
 * TD5RE_R22_SMOOTH is that cap in thousandths of a grade unit per span; 0
 * restores the pure slope limiter (byte-identical). */
static double tg_road_curv_cap(void)
{
    int v;
    if (!s_spec) return 0.0;
    v = td5_env_int("TD5RE_R22_SMOOTH", 45, 0, 1000);
    if (v <= 0) return 0.0;
    return (double)v / 1000.0 * (double)s_spec->span_length;
}

/* Forward / backward / forward grade limiter over nodes [a, n) with y[a]
 * fixed. Writes nl->v[i].y for i in (a, n). */
static void tg_road_solve(TG_NodeList *nl, int a, int n)
{
    const double curv = tg_road_curv_cap();
    int i;
    if (n - a < 2) return;
    for (i = a + 1; i < n; i++) {
        const double lo = nl->v[i - 1].y - s_rn[i].capg, hi = nl->v[i - 1].y + s_rn[i].capg;
        double y = s_rn[i].t;
        if (y < lo) y = lo;
        if (y > hi) y = hi;
        /* Curvature clamp: keep this span's grade within `curv` of the last
         * span's grade, then re-honour the slope cap (the hard limit). */
        if (curv > 0.0 && i >= 2) {
            const double pg = nl->v[i - 1].y - nl->v[i - 2].y;
            const double clo = nl->v[i - 1].y + pg - curv, chi = nl->v[i - 1].y + pg + curv;
            if (y < clo) y = clo;
            if (y > chi) y = chi;
            if (y < lo) y = lo;
            if (y > hi) y = hi;
        }
        nl->v[i].y = y;
    }
    for (i = n - 2; i > a; i--) {
        const double lo = nl->v[i + 1].y - s_rn[i + 1].capg, hi = nl->v[i + 1].y + s_rn[i + 1].capg;
        double y = nl->v[i].y;
        if (y < lo) y = lo;
        if (y > hi) y = hi;
        if (curv > 0.0 && i + 2 < n) {
            const double ng = nl->v[i + 1].y - nl->v[i + 2].y;
            const double clo = nl->v[i + 1].y + ng - curv, chi = nl->v[i + 1].y + ng + curv;
            if (y < clo) y = clo;
            if (y > chi) y = chi;
            if (y < lo) y = lo;
            if (y > hi) y = hi;
        }
        nl->v[i].y = y;
    }
    for (i = a + 1; i < n; i++) {
        const double lo = nl->v[i - 1].y - s_rn[i].capg, hi = nl->v[i - 1].y + s_rn[i].capg;
        double y = nl->v[i].y;
        if (y < lo) y = lo;
        if (y > hi) y = hi;
        if (curv > 0.0 && i >= 2) {
            const double pg = nl->v[i - 1].y - nl->v[i - 2].y;
            const double clo = nl->v[i - 1].y + pg - curv, chi = nl->v[i - 1].y + pg + curv;
            if (y < clo) y = clo;
            if (y > chi) y = chi;
            if (y < lo) y = lo;
            if (y > hi) y = hi;
        }
        nl->v[i].y = y;
    }
}

/* Raw per-node verdict from the solved profile. */
static int tg_road_node_kind(const TG_NodeList *nl, int i)
{
    const TG_RoadNode *r = &s_rn[i];
    const double d = nl->v[i].y - r->h;
    if (r->force) return TG_ST_NONE;
    if (r->wet) return tg_bridges_enabled() ? TG_ST_BRIDGE : TG_ST_NONE;
    if (d < -TG_ROAD_TUNNEL_DEPTH) return tg_tunnels_enabled() ? TG_ST_TUNNEL : TG_ST_NONE;
    if (d >  TG_ROAD_BRIDGE_LIFT)  return tg_bridges_enabled() ? TG_ST_BRIDGE : TG_ST_NONE;
    return TG_ST_NONE;
}

/* Classify spans [s_a, s_b] (both inclusive) into s_struct from the current
 * profile, then tidy the runs that touch that range: min lengths, water
 * abutments, bridge/tunnel interlock, fork windows, lane seams. `s_a` may
 * point into a run that started earlier; that run is re-tidied whole. */
static void tg_road_classify(const TG_NodeList *nl, int s_a, int s_b)
{
    int s, a0;
    if (s_b >= nl->count - 1) s_b = nl->count - 2;
    if (s_a < 0) s_a = 0;
    if (s_b < s_a) return;
    for (s = s_a; s <= s_b; s++) {
        const int k0 = tg_road_node_kind(nl, s), k1 = tg_road_node_kind(nl, s + 1);
        int k = TG_ST_NONE;
        if (k0 == TG_ST_TUNNEL || k1 == TG_ST_TUNNEL) k = TG_ST_TUNNEL;
        else if (k0 == TG_ST_BRIDGE || k1 == TG_ST_BRIDGE) k = TG_ST_BRIDGE;
        /* Never off the grid straight, never inside a fork window (the fork
         * geometry is built to one open-road section). */
        if (s <= TD5_TG_GRID_SPAN + 24) k = TG_ST_NONE;
        if (k != TG_ST_NONE && tg_span_in_fork_run(s)) k = TG_ST_NONE;
        s_struct[s] = (unsigned char)k;
    }
    if (s_b + 1 > s_struct_n) s_struct_n = s_b + 1;

    /* Tidy from the start of whatever run s_a sits in (never into the FINAL
     * region, which does not move). */
    a0 = s_a;
    while (a0 - 1 >= s_struct_fin && s_struct[a0 - 1] != TG_ST_NONE &&
           s_struct[a0 - 1] == s_struct[a0]) a0--;
    for (s = a0; s <= s_b; ) {
        const int k = s_struct[s];
        int e = s, len, lo, hi, q, seam = -1, min_len;
        if (k == TG_ST_NONE) { s++; continue; }
        while (e + 1 <= s_b && s_struct[e + 1] == k) e++;
        len = e - s + 1;

        /* A lane seam inside (or hugging) a structure: trim the run to end
         * 3 spans before it, the walk only blocks seams on PROVISIONAL kinds
         * and a revision can have moved a run onto one. */
        for (q = s - 2; q <= e + 3 && q < nl->count; q++)
            if (q >= 0 && nl->v[q].lane_side != 0) { seam = q; break; }
        /* A WATER crossing keeps its deck whatever the lanes do: a seam on a
         * deck is a width step, a missing deck is a causeway through the sea. */
        if (seam >= 0 && k == TG_ST_BRIDGE) {
            for (q = s; q <= e + 1; q++) if (s_rn[q].wet) { seam = -1; break; }
        }
        if (seam >= 0) {
            int cut = seam - 3;
            for (q = (cut < s ? s : cut); q <= e; q++) s_struct[q] = TG_ST_NONE;
            e = cut - 1;
            len = e - s + 1;
            if (len <= 0) { s = seam + 1; continue; }
        }

        /* Minimum lengths. A water crossing may be short (a stream), a land
         * viaduct or a bore must be long enough to read as one. */
        min_len = (k == TG_ST_TUNNEL) ? TG_ROAD_TUNNEL_MIN : TG_ROAD_BRIDGE_MIN;
        if (k == TG_ST_BRIDGE) {
            for (q = s; q <= e + 1; q++) if (s_rn[q].wet) { min_len = TG_ROAD_WATER_MIN; break; }
        }
        if (len < min_len) {
            for (q = s; q <= e; q++) s_struct[q] = TG_ST_NONE;
            s = e + 1;
            continue;
        }
        /* Bridge/tunnel interlock: a run within CLEAR spans of a run of the
         * other kind -- the shorter yields. */
        lo = s - TD5_TG_BRIDGE_TUNNEL_CLEAR; if (lo < 0) lo = 0;
        hi = e + TD5_TG_BRIDGE_TUNNEL_CLEAR; if (hi > s_struct_n - 1) hi = s_struct_n - 1;
        for (q = lo; q <= hi; q++) {
            if (s_struct[q] != TG_ST_NONE && s_struct[q] != k) {
                int o0, o1;
                tg_struct_run_bounds(q, &o0, &o1);
                int wet_here = 0, wet_other = 0, w2;
                if (k == TG_ST_BRIDGE) for (w2 = s; w2 <= e + 1; w2++) if (s_rn[w2].wet) { wet_here = 1; break; }
                if (s_struct[q] == TG_ST_BRIDGE) for (w2 = o0; w2 <= o1 + 1; w2++) if (s_rn[w2].wet) { wet_other = 1; break; }
                if ((o1 - o0 + 1 >= len || o0 < s_struct_fin || wet_other) && !wet_here) {
                    int w;
                    for (w = s; w <= e; w++) s_struct[w] = TG_ST_NONE;
                    len = 0;
                } else {
                    int w;
                    for (w = o0; w <= o1; w++) s_struct[w] = TG_ST_NONE;
                }
                if (len == 0) break;
            }
        }
        s = e + 1;
    }
}

/* Longest run of `kind` that ends at or after span `from` (a run may have
 * started earlier, carry that length). */
static int tg_road_longest_run(int kind, int from, int *at)
{
    int s, best = 0;
    if (at) *at = -1;
    for (s = 0; s < s_struct_n; ) {
        int e = s, len;
        if (s_struct[s] != kind) { s++; continue; }
        while (e + 1 < s_struct_n && s_struct[e + 1] == kind) e++;
        len = e - s + 1;
        if (e >= from && len > best) { best = len; if (at) *at = s; }
        s = e + 1;
    }
    return best;
}

/* Re-solve the revisable window and classify its spans. Returns 0 when a
 * structure run inside it exceeds its cap (the caller rejects the section). */
static int tg_road_revise(TG_NodeList *nl, int *why_kind, int *why_len)
{
    const int n = nl->count;
    int a = n - 1 - TG_ROAD_WINDOW;
    int i;
    if (a < s_struct_fin) a = s_struct_fin;
    if (a < 0) a = 0;
    for (i = s_rn_n; i < n; i++) tg_road_node_fill(i, &nl->v[i]);
    s_rn_n = n;
    if (a == 0) nl->v[0].y = 0.0;
    tg_road_solve(nl, a, n);
    tg_road_classify(nl, a, n - 2);
    {
        int at, len;
        len = tg_road_longest_run(TG_ST_TUNNEL, a, &at);
        if (len > tg_tunnel_max_spans()) {
            if (why_kind) *why_kind = TG_ST_TUNNEL;
            if (why_len) *why_len = len;
            return 0;
        }
        len = tg_road_longest_run(TG_ST_BRIDGE, a, &at);
        if (len > tg_bridge_max_spans()) {
            if (why_kind) *why_kind = TG_ST_BRIDGE;
            if (why_len) *why_len = len;
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------- guidance (A*) -- */

/* The TOPOLOGY QUERY the walk makes before each section: on a coarse grid
 * (TG_GUIDE_CELL world units) around the head, find the cheapest route to
 * any cell TG_GUIDE_GOAL cells further along +X, with water and steep ground
 * priced so a land route is preferred over a bridge or a bore whenever one
 * exists within reason. The direction to the route's point TG_GUIDE_AHEAD
 * world units out is the section's desired heading; the section cost
 * charges the deviation from it. Bridges and tunnels then happen where the
 * topology leaves no cheaper way, not where the walk blundered. */
#define TG_GUIDE_CELL   6000.0
#define TG_GUIDE_NX     96          /* cells along +X (from -16 to +80)      */
#define TG_GUIDE_NZ     160         /* cells across (+-80)                   */
#define TG_GUIDE_X0     (-16)
#define TG_GUIDE_GOAL   72          /* goal column, cells ahead of the head  */
#define TG_GUIDE_AHEAD  (60000.0)   /* world units along the route          */
#define TG_GUIDE_N      (TG_GUIDE_NX * TG_GUIDE_NZ)

static float  s_gd_cost[TG_GUIDE_N];     /* per-cell step cost              */
static float  s_gd_g[TG_GUIDE_N];        /* best known cost                 */
static int    s_gd_prev[TG_GUIDE_N];
static int    s_gd_heap[TG_GUIDE_N * 2];
static float  s_gd_hkey[TG_GUIDE_N * 2];
static unsigned char s_gd_closed[TG_GUIDE_N];
static int    s_gd_nheap;
static double s_gd_ox, s_gd_oz;          /* world origin of cell (0,0)      */
static double s_gd_dirx, s_gd_dirz;      /* result: desired unit direction  */
static int    s_gd_valid;
static long   s_gd_plans, s_gd_noroute;

static void tg_gd_heap_push(int idx, float key)
{
    int i;
    if (s_gd_nheap >= TG_GUIDE_N * 2) return;
    i = s_gd_nheap++;
    s_gd_heap[i] = idx; s_gd_hkey[i] = key;
    while (i > 0) {
        const int pa = (i - 1) / 2;
        int ti; float tk;
        if (s_gd_hkey[pa] <= s_gd_hkey[i]) break;
        ti = s_gd_heap[pa]; tk = s_gd_hkey[pa];
        s_gd_heap[pa] = s_gd_heap[i]; s_gd_hkey[pa] = s_gd_hkey[i];
        s_gd_heap[i] = ti; s_gd_hkey[i] = tk;
        i = pa;
    }
}

static int tg_gd_heap_pop(void)
{
    const int top = s_gd_heap[0];
    int i = 0;
    s_gd_nheap--;
    s_gd_heap[0] = s_gd_heap[s_gd_nheap]; s_gd_hkey[0] = s_gd_hkey[s_gd_nheap];
    for (;;) {
        const int l = 2 * i + 1, r = l + 1;
        int m = i, ti; float tk;
        if (l < s_gd_nheap && s_gd_hkey[l] < s_gd_hkey[m]) m = l;
        if (r < s_gd_nheap && s_gd_hkey[r] < s_gd_hkey[m]) m = r;
        if (m == i) break;
        ti = s_gd_heap[m]; tk = s_gd_hkey[m];
        s_gd_heap[m] = s_gd_heap[i]; s_gd_hkey[m] = s_gd_hkey[i];
        s_gd_heap[i] = ti; s_gd_hkey[i] = tk;
        i = m;
    }
    return top;
}

static float tg_gd_cell_cost(double x, double z)
{
    const double h = tg_world_h(x, z);
    double sl;
    if (h < tg_world_water_y(x, z)) return 12.0f;         /* bridge          */
    sl = tg_world_slope(x, z);
    if (sl >= TG_WORLD_STEEP_SLOPE) return 7.0f;          /* bore / cutting  */
    if (sl >= TG_WORLD_HILL_SLOPE)  return (float)(1.0 + (sl - TG_WORLD_HILL_SLOPE) * 8.0);
    return 1.0f;
}

static void tg_road_guide_plan(double hx, double hz)
{
    static const int dx8[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
    static const int dz8[8] = { 0, 1, -1, 1, -1, 0, 1, -1 };
    const int start = (0 - TG_GUIDE_X0) * TG_GUIDE_NZ + TG_GUIDE_NZ / 2;
    int i, goal = -1;
    s_gd_valid = 0;
    s_gd_plans++;
    s_gd_ox = hx + TG_GUIDE_X0 * TG_GUIDE_CELL;
    s_gd_oz = hz - (TG_GUIDE_NZ / 2) * TG_GUIDE_CELL;
    for (i = 0; i < TG_GUIDE_N; i++) {
        const int gx = i / TG_GUIDE_NZ, gz = i % TG_GUIDE_NZ;
        s_gd_cost[i] = tg_gd_cell_cost(s_gd_ox + (gx + 0.5) * TG_GUIDE_CELL,
                                       s_gd_oz + (gz + 0.5) * TG_GUIDE_CELL);
        s_gd_g[i] = 1e30f; s_gd_prev[i] = -1; s_gd_closed[i] = 0;
    }
    s_gd_nheap = 0;
    s_gd_g[start] = 0.0f;
    tg_gd_heap_push(start, 0.0f);
    while (s_gd_nheap > 0) {
        const int cur = tg_gd_heap_pop();
        const int gx = cur / TG_GUIDE_NZ, gz = cur % TG_GUIDE_NZ;
        int d;
        if (s_gd_closed[cur]) continue;
        s_gd_closed[cur] = 1;
        if (gx - (0 - TG_GUIDE_X0) >= TG_GUIDE_GOAL) { goal = cur; break; }
        for (d = 0; d < 8; d++) {
            const int nx = gx + dx8[d], nz = gz + dz8[d];
            int ni;
            float ng;
            if (nx < 0 || nx >= TG_GUIDE_NX || nz < 0 || nz >= TG_GUIDE_NZ) continue;
            ni = nx * TG_GUIDE_NZ + nz;
            if (s_gd_closed[ni]) continue;
            /* going back along -X costs extra: the walk cannot follow it */
            ng = s_gd_g[cur] + s_gd_cost[ni] * ((dx8[d] && dz8[d]) ? 1.414f : 1.0f)
               + (dx8[d] < 0 ? 3.0f : 0.0f);
            if (ng < s_gd_g[ni]) {
                s_gd_g[ni] = ng; s_gd_prev[ni] = cur;
                tg_gd_heap_push(ni, ng + (float)(TG_GUIDE_GOAL - (nx - (0 - TG_GUIDE_X0))));
            }
        }
    }
    if (goal < 0) { s_gd_noroute++; return; }
    /* Walk the route back from the goal; take the point TG_GUIDE_AHEAD out. */
    {
        static int path[TG_GUIDE_N];
        int n = 0, c = goal, k;
        double acc = 0.0, px = hx, pz = hz;
        while (c >= 0 && n < TG_GUIDE_N) { path[n++] = c; c = s_gd_prev[c]; }
        for (k = n - 2; k >= 0; k--) {
            const int gx = path[k] / TG_GUIDE_NZ, gz = path[k] % TG_GUIDE_NZ;
            const double cx = s_gd_ox + (gx + 0.5) * TG_GUIDE_CELL;
            const double cz = s_gd_oz + (gz + 0.5) * TG_GUIDE_CELL;
            acc += sqrt((cx - px) * (cx - px) + (cz - pz) * (cz - pz));
            px = cx; pz = cz;
            if (acc >= TG_GUIDE_AHEAD) break;
        }
        {
            const double dx = px - hx, dz = pz - hz, len = sqrt(dx * dx + dz * dz);
            if (len > 1.0) { s_gd_dirx = dx / len; s_gd_dirz = dz / len; s_gd_valid = 1; }
        }
    }
}

#define TG_ROAD_LOOKAHEAD 48   /* spans probed straight past a candidate's end */

/* What lies ahead if the road kept its final heading: water and steep ground
 * cost, discounted with distance. This is what turns the walk away from a
 * coast or a massif BEFORE the structure caps have to refuse it. */
static double tg_road_lookahead_cost(const TG_NodeList *nl)
{
    const TG_Node *a = &nl->v[nl->count - 2], *b = &nl->v[nl->count - 1];
    double dx = b->x - a->x, dz = b->z - a->z, len = sqrt(dx * dx + dz * dz), c = 0.0;
    int j;
    if (len < 1.0) return 0.0;
    dx /= len; dz /= len;
    for (j = 1; j <= TG_ROAD_LOOKAHEAD; j++) {
        const double x = b->x + dx * j * s_spec->span_length;
        const double z = b->z + dz * j * s_spec->span_length;
        const double w = 1.0 / (1.0 + (double)j / 16.0);
        const double h = tg_world_h(x, z);
        if (h < tg_world_water_y(x, z)) c += 2.5 * w;
        else {
            const double sl = tg_world_slope(x, z);
            if (sl >= TG_WORLD_STEEP_SLOPE) c += 2.0 * w;
            else if (sl > TG_WORLD_HILL_SLOPE) c += (sl - TG_WORLD_HILL_SLOPE) * 4.0 * w;
            /* ground far above/below the current road = a structure coming */
            {
                const double d = fabs(h - b->y) - (double)j * s_rn[nl->count - 1].capg;
                if (d > 0.0) c += (d / 1500.0) * w;
            }
        }
    }
    return c;
}

/* Cost of the spans [s0, s1] for steering: structure spans, deep cuts/fills,
 * water, steep ground, plus the lookahead. Lower is better. */
static double tg_road_cost(const TG_NodeList *nl, int s0, int s1)
{
    double c = tg_road_lookahead_cost(nl);
    int s;
    if (s_gd_valid && nl->count >= 2) {
        /* Deviation of the section's final heading from the planned route. */
        const TG_Node *a = &nl->v[nl->count - 2], *b = &nl->v[nl->count - 1];
        double dx = b->x - a->x, dz = b->z - a->z, len = sqrt(dx * dx + dz * dz);
        if (len > 1.0) {
            const double dot = (dx * s_gd_dirx + dz * s_gd_dirz) / len;
            c += (1.0 - dot) * 8.0;
        }
    }
    for (s = s0; s <= s1 && s < nl->count - 1; s++) {
        const int i = s + 1;
        const double d = fabs(nl->v[i].y - s_rn[i].h);
        const int k = (s < s_struct_n) ? s_struct[s] : TG_ST_NONE;
        if (k == TG_ST_TUNNEL) c += 4.0;
        else if (k == TG_ST_BRIDGE) c += (s_rn[i].wet ? 2.0 : 3.0);
        else c += d / 900.0;                       /* cut/fill depth        */
        /* A fork window forbids structures, so ground that would need one
         * there becomes a giant cut or fill: price it as such. */
        if (tg_span_in_fork_run(s) && d > 1500.0) c += (d - 1500.0) / 250.0;
        if (s_rn[i].wet) c += 1.0;
        /* A curved deck or bore reads wrong (and shears the deck seam):
         * charge the turn so the straighter candidate wins there. */
        if (k != TG_ST_NONE && i + 1 < nl->count) {
            const double ax = nl->v[i].x - nl->v[i - 1].x, az = nl->v[i].z - nl->v[i - 1].z;
            const double bx = nl->v[i + 1].x - nl->v[i].x, bz = nl->v[i + 1].z - nl->v[i].z;
            const double cr = fabs(ax * bz - az * bx) / (s_spec->span_length * s_spec->span_length);
            if (cr > TD5_TG_BRIDGE_MAX_TURN) c += 2.0;
        }
        {
            const double sl = tg_world_slope(nl->v[i].x, nl->v[i].z);
            if (sl > TG_WORLD_HILL_SLOPE) c += (sl - TG_WORLD_HILL_SLOPE) * 6.0;
        }
    }
    return c;
}

/* Advance the FINAL boundary: spans more than TG_ROAD_WINDOW behind the head
 * are frozen, but never in the middle of a structure run. */
static void tg_road_finalize_to(int head_span)
{
    int f = head_span - TG_ROAD_WINDOW;
    if (f > s_struct_n) f = s_struct_n;
    if (f <= s_struct_fin) return;
    while (f > s_struct_fin && f - 1 >= 0 && s_struct[f - 1] != TG_ST_NONE &&
           f < s_struct_n && s_struct[f] == s_struct[f - 1]) f--;
    s_struct_fin = f;
}

/* ---------------------------------------------------------------- walk -- */

/* Section parameters chosen once per attempt (RNG), applied per candidate. */
typedef struct {
    TD5_TrackGenSection sec;
    int    len_spans;
    double target_width, radius;
    int    dir;
    /* lane management, decided once per attempt */
    int    sec_lanes, sec_side, sec_base, jog_at;
    double jog;
    int    lanes_here_vary;
} TG_SectionPlan;

typedef struct {
    int    count;
    double x, z, heading, width, cum_jx, cum_jz;
    int    struct_n, struct_fin, rn_n;
} TG_WalkSave;

static void tg_walk_save(const TG_NodeList *nl, TG_WalkSave *s, double x, double z,
                         double heading, double width, double jx, double jz)
{
    s->count = nl->count; s->x = x; s->z = z; s->heading = heading; s->width = width;
    s->cum_jx = jx; s->cum_jz = jz;
    s->struct_n = s_struct_n; s->struct_fin = s_struct_fin; s->rn_n = s_rn_n;
}

/* Push one candidate section. Returns 0 if it self-overlaps. Leaves the walk
 * state in the caller's variables (rolled back by the caller on reject). */
static int tg_walk_push_section(const TD5_TrackGenSpec *spec, TG_NodeList *nl,
                                const TG_SectionPlan *p, int want_nodes, int skip,
                                double heading_limit, double *px, double *pz,
                                double *pheading, double *pwidth,
                                double *pjx, double *pjz, int save_lanes,
                                int lane_vary)
{
    const double span_len = (double)spec->span_length;
    const double width_ramp = (double)spec->lane_width * 0.5;
    const double lane_w = (double)spec->lane_width;
    double x = *px, z = *pz, heading = *pheading, width = *pwidth;
    double cum_jx = *pjx, cum_jz = *pjz;
    int dir = p->dir, i;

    for (i = 0; i < p->len_spans && nl->count < want_nodes; i++) {
        int lanes_here;
        if (width < p->target_width) {
            width += width_ramp;
            if (width > p->target_width) width = p->target_width;
        } else if (width > p->target_width) {
            width -= width_ramp;
            if (width < p->target_width) width = p->target_width;
        }
        if (p->radius > 0.0) {
            double floor_r = (width * 0.5)
                           * (tg_shape_safety_x100(nl->count, spec->curve_safety_x100) / 100.0);
            double r = p->radius < floor_r ? floor_r : p->radius;
            double dh = (double)dir * (span_len / r);
            /* No sharp turns on a deck or in a bore (provisional kind of the
             * span behind the head), nor in a fork window. */
            {
                const int k = tg_struct_kind(nl->count - 1);
                if (k != TG_ST_NONE) {
                    if (dh >  TD5_TG_BRIDGE_MAX_TURN) dh =  TD5_TG_BRIDGE_MAX_TURN;
                    if (dh < -TD5_TG_BRIDGE_MAX_TURN) dh = -TD5_TG_BRIDGE_MAX_TURN;
                }
            }
            if (tg_span_in_fork_run(nl->count)) {
                if (dh >  TD5_TG_FORK_MAX_TURN) dh =  TD5_TG_FORK_MAX_TURN;
                if (dh < -TD5_TG_FORK_MAX_TURN) dh = -TD5_TG_FORK_MAX_TURN;
            }
            heading += dh;
            if (heading > TD5_TG_AXIS_HEADING + heading_limit) {
                heading = TD5_TG_AXIS_HEADING + heading_limit; dir = -1;
            } else if (heading < TD5_TG_AXIS_HEADING - heading_limit) {
                heading = TD5_TG_AXIS_HEADING - heading_limit; dir = 1;
            }
        }
        x += sin(heading) * span_len;
        z += cos(heading) * span_len;
        lanes_here = lane_vary ? p->sec_lanes : spec->lanes;
        if (lane_vary && nl->count == p->jog_at) {
            x += cos(heading) * p->jog;   cum_jx += cos(heading) * p->jog;
            z -= sin(heading) * p->jog;   cum_jz -= sin(heading) * p->jog;
        }
        {
            double w_here = width;
            if (lane_vary && i == 0 && p->sec_lanes != save_lanes)
                w_here = (double)(p->sec_lanes < save_lanes ? p->sec_lanes : save_lanes) * lane_w;
            if (tg_too_close(nl, x, z, w_here, (double)spec->lane_width, skip))
                return 0;
            if (!tg_nodes_push(nl, x, z, w_here, lanes_here)) return -1;
        }
        if (lane_vary) {
            TG_Node *nn = &nl->v[nl->count - 1];
            nn->jx = cum_jx; nn->jz = cum_jz;
            nn->lane_base = p->sec_base;
            if (i == 0 && p->sec_lanes != save_lanes) nn->lane_side = p->sec_side;
        }
    }
    *px = x; *pz = z; *pheading = heading; *pwidth = width;
    *pjx = cum_jx; *pjz = cum_jz;
    return 1;
}

int tg_build_centerline(const TD5_TrackGenSpec *spec, TG_NodeList *nl,
                        int section_tally[TD5_TG_SECTION_COUNT])
{
    const int    want_nodes = spec->target_spans + 1;
    const double span_len   = (double)spec->span_length;
    const double base_width = (double)spec->lanes * (double)spec->lane_width;
    double x = 0.0, z = 0.0;
    double heading = TD5_TG_AXIS_HEADING;
    double width = base_width;
    const int    lane_vary = td5_env_flag_on("TD5RE_AUTOTRACK_LANE_VARY");
    const int    lanes_min = td5_env_int("TD5RE_AUTOTRACK_LANES_MIN", 2, 1, TD5_TG_MAX_LANES);
    const int    lanes_max = td5_env_int("TD5RE_AUTOTRACK_LANES_MAX", 8, 1,
                                         TD5_TG_MAX_LANES > 10 ? 10 : TD5_TG_MAX_LANES);
    const int    lane_pct  = td5_env_int("TD5RE_AUTOTRACK_LANE_PCT", 35, 0, 100);
    const double lane_w    = (double)spec->lane_width;
    int    cur_lanes = spec->lanes;
    int    cur_base  = TD5_TG_HEIGHT_NIBBLE;
    long   lane_changes = 0, lane_skipped = 0;
    double cum_jx = 0.0, cum_jz = 0.0;
    const double acute_limit = tg_acute_heading_limit();
    const double limit_max   = (acute_limit > TD5_TG_HEADING_LIMIT) ? acute_limit : TD5_TG_HEADING_LIMIT;
    const int    skip        = tg_adjacent_skip(spec, limit_max);
    const int    steer       = tg_steer_enabled();
    int attempts = 0, rejected_cap = 0, rejected_overlap = 0;

    if (!tg_world_ready()) tg_world_build(spec->seed, spec->target_spans);
    s_spec = spec;
    s_max_grade = spec->max_grade_x1000 / 1000.0;
    if (s_max_grade <= 0.0) s_max_grade = 0.12;
    memset(s_struct, 0, sizeof(s_struct));
    s_struct_n = s_struct_fin = 0;
    s_rn_n = 0;
    s_stat_conforms = s_stat_capfail = s_stat_steer_wins = 0;
    s_gd_plans = s_gd_noroute = 0; s_gd_valid = 0;

    TD5_LOG_I(LOG_TAG, "trackgen: heading budget spine=%ddeg acute=%ddeg "
              "-> adjacent_skip=%d (curve-safety %d/100); terrain steering %s, "
              "caps bridge %d / tunnel %d spans",
              (int)(TD5_TG_HEADING_LIMIT * 180.0 / TD5_TG_PI + 0.5),
              (int)(acute_limit * 180.0 / TD5_TG_PI + 0.5), skip,
              spec->curve_safety_x100, steer ? "on" : "off",
              tg_bridge_max_spans(), tg_tunnel_max_spans());

    if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;
    {
        int i;
        for (i = 0; i < TD5_TG_GRID_SPAN + 16 && nl->count < want_nodes; i++) {
            x += sin(heading) * span_len;
            z += cos(heading) * span_len;
            if (!tg_nodes_push(nl, x, z, width, spec->lanes)) return 0;
        }
    }
    tg_road_revise(nl, NULL, NULL);

    while (nl->count < want_nodes) {
        TG_SectionPlan p;
        TG_WalkSave sv;
        const int save_lanes = cur_lanes, save_base = cur_base;
        double heading_limit;
        int cand_dir[4], ncand = 0, c, best = -1, r;
        double cand_radius;
        double best_cost = 1e30;
        double cx = x, cz = z, cheading = heading, cwidth = width, cjx = cum_jx, cjz = cum_jz;
        int why_kind = 0, why_len = 0, any_overlap = 0, any_cap = 0, forced_done = 0;

        memset(&p, 0, sizeof(p));
        p.sec = tg_pick_section(spec, nl->count);
        p.target_width = base_width;
        p.dir = (tg_rand() & 1) ? 1 : -1;
        tg_walk_save(nl, &sv, x, z, heading, width, cum_jx, cum_jz);

        if (attempts < 8 && td5_env_flag_off("TD5RE_AUTOTRACK_BLOCK_TURNS") &&
            tg_biome_span_is_city(nl->count)) {
            const unsigned int bh = (unsigned)nl->count * 2654435761u;
            if ((bh >> 28) == 0u) p.sec = TD5_TG_ACUTE;
        }
        if (attempts < 8 && tg_r21_road_char()) {
            const int n = tg_shape_pct(nl->count, TG_SF_BLOCK_TURN);
            if (n > 0 && (tg_roll_hash_at(0x21010101u, nl->count) % (unsigned)n) == 0u)
                p.sec = TD5_TG_ACUTE;
        }
        if (attempts >= 12) p.sec = TD5_TG_STRAIGHT;

        switch (p.sec) {
            case TD5_TG_STRAIGHT:
                p.len_spans = tg_range(6, 24);
                break;
            case TD5_TG_CURVE:
                p.len_spans = tg_range(8, 26);
                p.radius = 12000.0 + tg_frand() * 28000.0;
                break;
            case TD5_TG_ACUTE:
                p.len_spans = tg_range(4, 12);
                p.radius = (width * 0.5)
                         * (tg_shape_safety_x100(nl->count, spec->curve_safety_x100) / 100.0)
                         * (1.0 + tg_frand() * 0.6);
                break;
            case TD5_TG_DUAL_LANE:
            default: {
                int extra = tg_range(2, 4);
                int lanes = spec->lanes + extra;
                if (lanes > 12) lanes = 12;
                p.target_width = (double)lanes * (double)spec->lane_width;
                p.len_spans = tg_range(10, 28);
                if (tg_rand() & 1) p.radius = 26000.0 + tg_frand() * 30000.0;
                break;
            }
        }
        heading_limit = (p.sec == TD5_TG_ACUTE) ? acute_limit : TD5_TG_HEADING_LIMIT;
        cand_radius = p.radius;

        /* [LANES] identical to the pre-topology walk (see its comment block). */
        p.sec_lanes = cur_lanes; p.sec_side = 0; p.sec_base = cur_base; p.jog_at = -1; p.jog = 0.0;
        if (lane_vary) {
            const int seam = nl->count;
            int want = cur_lanes, side = 0;
            const int ahead = tg_fork_window_ahead(seam, 90);
            int pre_fork = 0;
            if (ahead >= 0 && cur_lanes < tg_fork_kind_min_lanes(ahead)) {
                const int need = tg_fork_kind_min_lanes(ahead);
                pre_fork = 1;
                want = (need - cur_lanes >= 2) ? cur_lanes + 2 : need;
                side = (want - cur_lanes == 2) ? 2 : ((tg_rand() & 1) ? 1 : -1);
            } else if (p.sec == TD5_TG_DUAL_LANE) {
                want = cur_lanes + 2; side = 2;
            } else if (tg_range(0, 99) < lane_pct) {
                const int aim = tg_shape_lane_aim(seam, spec->lanes, lanes_min, lanes_max);
                const int up = (cur_lanes < aim) ? (tg_range(0, 99) < 70)
                             : (cur_lanes > aim) ? (tg_range(0, 99) < 30)
                             : (tg_rand() & 1);
                const int both = (tg_range(0, 99) < 30);
                want = cur_lanes + (up ? 1 : -1) * (both ? 2 : 1);
                side = both ? 2 : ((tg_rand() & 1) ? 1 : -1);
                if (ahead >= 0 && want < tg_fork_kind_min_lanes(ahead)) want = cur_lanes;
            }
            if (want < lanes_min) want = lanes_min;
            if (want > lanes_max) want = lanes_max;
            if (want != cur_lanes) {
                const int d = want - cur_lanes;
                int ok_here = 1, q;
                if (d > 2 || d < -2) { want = cur_lanes + (d > 0 ? 2 : -2); }
                if (want - cur_lanes == 2 || want - cur_lanes == -2) side = 2;
                if (seam < TD5_TG_GRID_SPAN + 40) ok_here = 0;
                if (seam > want_nodes - 60) ok_here = 0;
                for (q = seam - 2; ok_here && q <= seam + 2; q++)
                    if (tg_span_in_fork_run(q) || tg_struct_kind(q) != TG_ST_NONE) ok_here = 0;
                if (ok_here) {
                    int nb = cur_base;
                    if (side == 2)       nb += (want > cur_lanes) ? -1 : 1;
                    else if (side == 1)  nb += (want > cur_lanes) ? -1 : 1;
                    if (nb < 5 || nb > 11) {
                        if (side == 2) want = cur_lanes + (want > cur_lanes ? 1 : -1);
                        side = -1; nb = cur_base;
                    }
                    p.sec_lanes = want; p.sec_side = side; p.sec_base = nb;
                    if (side != 2) {
                        const int drop = (want < cur_lanes);
                        p.jog_at = drop ? seam : seam + 1;
                        p.jog = (lane_w * 0.5) * ((drop ? 1.0 : -1.0) * (side < 0 ? 1.0 : -1.0));
                    }
                } else {
                    lane_skipped++;
                    if (pre_fork)
                        TD5_LOG_I(LOG_TAG, "trackgen: [LANES] pre-fork widening to %d "
                                  "at seam %d blocked (grid/fork/bridge/tunnel/finish "
                                  "window); fork kind %s ahead", want, seam,
                                  tg_fork_kind_name(ahead));
                }
            }
            p.target_width = (double)p.sec_lanes * lane_w;
            width = p.target_width;
            cwidth = width;
        }

        if (steer) tg_road_guide_plan(x, z);

        /* Candidates: the rolled direction first, then (steering) the other
         * one for turning sections. A STRAIGHT also tries a gentle bend either
         * way so the walk can lean away from a coast or a massif ahead. */
        cand_dir[ncand++] = p.dir;
        if (steer && p.radius > 0.0) cand_dir[ncand++] = -p.dir;
        else if (steer && p.sec == TD5_TG_STRAIGHT && attempts < 12) {
            cand_dir[ncand++] = 2;  cand_dir[ncand++] = -2;   /* bend markers */
        }

        for (c = 0; c < ncand; c++) {
            double cost;
            x = cx; z = cz; heading = cheading; width = cwidth; cum_jx = cjx; cum_jz = cjz;
            nl->count = sv.count; s_struct_n = sv.struct_n; s_struct_fin = sv.struct_fin; s_rn_n = sv.rn_n;
            p.dir = (cand_dir[c] == 2) ? 1 : (cand_dir[c] == -2) ? -1 : cand_dir[c];
            p.radius = (cand_dir[c] == 2 || cand_dir[c] == -2) ? 30000.0 : cand_radius;
            r = tg_walk_push_section(spec, nl, &p, want_nodes, skip, heading_limit,
                                     &x, &z, &heading, &width, &cum_jx, &cum_jz,
                                     save_lanes, lane_vary);
            if (r < 0) return 0;                       /* cancelled / OOM */
            if (r == 0) { any_overlap = 1; continue; }
            if (!tg_road_revise(nl, &why_kind, &why_len)) { any_cap = 1; continue; }
            cost = tg_road_cost(nl, sv.count - 1, nl->count - 2);
            /* The rolled shape is what the section mix asked for; a steering
             * alternative has to be clearly cheaper to displace it, or the
             * road wiggles on noise. */
            if (c > 0) cost += (cand_dir[c] == 2 || cand_dir[c] == -2) ? 2.5 : 1.0;
            if (cost < best_cost) { best_cost = cost; best = c; }
        }
        if (best >= 0 && ncand > 1 && best != 0) s_stat_steer_wins++;

        if (best < 0 && attempts >= 20 && !any_overlap) {
            /* Last resort: the terrain yields. Accept the rolled direction and
             * force its spans to CONFORM (a deep cut or a causeway) instead of
             * an over-cap structure, so the walk never ends the track short. */
            int i;
            x = cx; z = cz; heading = cheading; width = cwidth; cum_jx = cjx; cum_jz = cjz;
            nl->count = sv.count; s_struct_n = sv.struct_n; s_struct_fin = sv.struct_fin; s_rn_n = sv.rn_n;
            p.dir = cand_dir[0];
            p.radius = cand_radius;
            r = tg_walk_push_section(spec, nl, &p, want_nodes, skip, heading_limit,
                                     &x, &z, &heading, &width, &cum_jx, &cum_jz,
                                     save_lanes, lane_vary);
            if (r < 0) return 0;
            if (r == 1) {
                /* The head node too: the span between it and the section's
                 * first node takes the structural kind of EITHER end, and the
                 * run behind the head must not grow by that one span. */
                for (i = sv.count - 1; i < nl->count; i++) {
                    if (i >= s_rn_n) tg_road_node_fill(i, &nl->v[i]);
                    s_rn[i].force = 1;
                }
                s_rn_n = nl->count;
                if (!tg_road_revise(nl, &why_kind, &why_len)) {
                    TD5_LOG_W(LOG_TAG, "trackgen: [STRUCT] span %d: forced conform "
                              "still refused (%s would run %d spans; run bounds "
                              "fin=%d n=%d)", sv.count - 1,
                              why_kind == TG_ST_TUNNEL ? "tunnel" : "bridge",
                              why_len, s_struct_fin, s_struct_n);
                } else {
                    best = 0;
                    forced_done = 1;
                    s_stat_conforms++;
                    TD5_LOG_W(LOG_TAG, "trackgen: [STRUCT] span %d: terrain yields -- "
                              "%d-span section forced to conform after %d rejected "
                              "attempts (%s would run %d spans)", sv.count - 1,
                              nl->count - sv.count, attempts,
                              why_kind == TG_ST_TUNNEL ? "tunnel" : "bridge", why_len);
                }
            }
        }

        if (best < 0) {
            x = cx; z = cz; heading = cheading; width = cwidth; cum_jx = cjx; cum_jz = cjz;
            nl->count = sv.count; s_struct_n = sv.struct_n; s_struct_fin = sv.struct_fin; s_rn_n = sv.rn_n;
            cur_lanes = save_lanes; cur_base = save_base;
            if (any_cap && !any_overlap) rejected_cap++; else rejected_overlap++;
            attempts++;
            if (attempts >= 40) {
                TD5_LOG_W(LOG_TAG, "trackgen: boxed in after %d spans; ending "
                          "track early (no continuation found: %d overlap, %d "
                          "structure-cap rejects)", nl->count - 1,
                          rejected_overlap, rejected_cap);
                break;
            }
            continue;
        }
        if (!forced_done) {
            /* Re-run the winning candidate so the node list holds it (the
             * list currently holds whatever candidate ran LAST, possibly a
             * rejected partial one). */
            x = cx; z = cz; heading = cheading; width = cwidth; cum_jx = cjx; cum_jz = cjz;
            nl->count = sv.count; s_struct_n = sv.struct_n; s_struct_fin = sv.struct_fin; s_rn_n = sv.rn_n;
            p.dir = (cand_dir[best] == 2) ? 1 : (cand_dir[best] == -2) ? -1 : cand_dir[best];
            p.radius = (cand_dir[best] == 2 || cand_dir[best] == -2) ? 30000.0 : cand_radius;
            r = tg_walk_push_section(spec, nl, &p, want_nodes, skip, heading_limit,
                                     &x, &z, &heading, &width, &cum_jx, &cum_jz,
                                     save_lanes, lane_vary);
            if (r < 0) return 0;
            tg_road_revise(nl, NULL, NULL);
        }
        if (lane_vary) {
            cur_lanes = p.sec_lanes; cur_base = p.sec_base;
            if (p.sec_lanes != save_lanes) lane_changes++;
        }
        tg_road_finalize_to(nl->count - 1);
        attempts = 0;
        if (p.sec == TD5_TG_STRAIGHT && p.radius > 0.0) section_tally[TD5_TG_CURVE]++;
        else section_tally[p.sec]++;
    }

    if (lane_vary) {
        int i, lo = 99, hi = 0;
        for (i = 0; i + 1 < nl->count; i++) {
            if (nl->v[i].lanes < lo) lo = nl->v[i].lanes;
            if (nl->v[i].lanes > hi) hi = nl->v[i].lanes;
        }
        TD5_LOG_I(LOG_TAG, "trackgen: [LANES] %ld lane changes (%ld sections "
                  "skipped: grid/fork/bridge/tunnel/finish), lanes %d..%d "
                  "(base %d, min %d, max %d, pct %d)", lane_changes,
                  lane_skipped, lo, hi, spec->lanes, lanes_min, lanes_max, lane_pct);
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [STEER] %ld section(s) took the other "
              "direction on terrain cost, %d cap rejects, %d overlap rejects, "
              "%ld forced conform(s); %ld route plan(s), %ld without a land route",
              s_stat_steer_wins, rejected_cap, rejected_overlap, s_stat_conforms,
              s_gd_plans, s_gd_noroute);

    {
        int i;
        for (i = 0; i < nl->count; i++) {
            int a = (i > 0) ? i - 1 : i;
            int b = (i < nl->count - 1) ? i + 1 : i;
            double dx = (nl->v[b].x - nl->v[b].jx) - (nl->v[a].x - nl->v[a].jx);
            double dz = (nl->v[b].z - nl->v[b].jz) - (nl->v[a].z - nl->v[a].jz);
            double len = sqrt(dx * dx + dz * dz);
            if (len < 1e-6) { dx = 0.0; dz = 1.0; len = 1.0; }
            nl->v[i].tx = dx / len;
            nl->v[i].tz = dz / len;
        }
    }
    return 1;
}

/* ------------------------------------------------ water / shore table -- */

/* Per main span and side: distance from the road EDGE to the first wet cell
 * along the outward ray (1e9 = dry to the reach), and the water surface
 * there. Built once after the profile is final; every water authority in
 * td5_tg_bridge.c / td5_tg_guard.c reads it, so the sea plane, the water
 * audits and the skirt cannot disagree about where the shore is. */
static double s_shore_d[TD5_TG_MAX_SPANS + 8][2];
static double s_shore_y[TD5_TG_MAX_SPANS + 8][2];
/* [R22 item 10] Distance from the road edge to the FAR bank of the near water
 * body (first dry cell past the wet run), so tg_emit_water can clip a river
 * plane to the water instead of sheeting a fixed 50000 across the dry land
 * beyond it. 1e9 = the water runs past the search cap (open water: keep the
 * full extent). */
static double s_shore_far[TD5_TG_MAX_SPANS + 8][2];
static unsigned char s_wet_any[TD5_TG_MAX_SPANS + 8];
static int s_shore_n;

static void tg_road_shore_build(const TG_NodeList *nl)
{
    const double reach = tg_far_reach() + 6000.0;
    /* The far bank may lie past the near-shore search reach; march for it up to
     * one old plane width (TD5_TG_WATER_EXTENT) past the near bank, so a clip
     * can never widen a plane beyond what it was. */
    const double far_cap = (double)TD5_TG_WATER_EXTENT;
    const double far_step = 1000.0;
    int i, side;
    s_shore_n = nl->count;
    if (s_shore_n > TD5_TG_MAX_SPANS + 8) s_shore_n = TD5_TG_MAX_SPANS + 8;
    for (i = 0; i < s_shore_n; i++) {
        const TG_Node *n = &nl->v[i];
        const double half = n->width * 0.5;
        int any = (i < s_rn_n) ? s_rn[i].wet : 0;
        for (side = 0; side < 2; side++) {
            const double sgn = side ? -1.0 : 1.0;            /* 0 = left */
            const double lx = n->tz * sgn, lz = -n->tx * sgn;
            double d;
            s_shore_d[i][side] = 1e9;
            s_shore_y[i][side] = tg_world_sea_y();
            s_shore_far[i][side] = 1e9;
            for (d = 0.0; d <= reach; d += 500.0) {
                const double wx = n->x + lx * (half + d), wz = n->z + lz * (half + d);
                /* [R22] Ask tg_world_is_water, not the raw height-vs-surface
                 * comparison it wraps. The raw test misses the one rule the
                 * world module exists to enforce: a CONFORMED cell is a road
                 * bed and is never water, "even where it lies below a
                 * neighbouring river's surface (a cutting beside a river)".
                 *
                 * With the raw test, a road cut alongside a river registered as
                 * the shore at d = 0. tg_emit_water clamps its inner edge to
                 * the road edge, so the quad was then laid FROM THE ASPHALT at
                 * the river's surface height -- water rendered over the road.
                 * Marching on instead finds the real bank, and the plane starts
                 * beyond the bed where it belongs. */
                if (tg_world_is_water(wx, wz)) {
                    s_shore_d[i][side] = d;
                    s_shore_y[i][side] = tg_world_water_y(wx, wz);
                    any = 1;
                    break;
                }
            }
            /* Once a near bank is found, march on for the far bank: the first
             * dry cell ends the near water body. If the water runs to the cap
             * (open water, e.g. the sea) leave 1e9 = "use the full extent". */
            if (s_shore_d[i][side] < 1e8) {
                double fd;
                for (fd = s_shore_d[i][side] + far_step;
                     fd <= s_shore_d[i][side] + far_cap; fd += far_step) {
                    const double wx = n->x + lx * (half + fd);
                    const double wz = n->z + lz * (half + fd);
                    if (!tg_world_is_water(wx, wz)) { s_shore_far[i][side] = fd; break; }
                }
            }
        }
        s_wet_any[i] = (unsigned char)any;
    }
}

/* [TOPOLOGY-FIRST] Public rebuild: the street network conforms the world
 * (corridors, gores, back streets) after the profile, so the shore table is
 * rebuilt once the network is done. */
static void tg_r22_water_diag(const TG_NodeList *nl);

void tg_road_shore_rebuild(const TG_NodeList *nl)
{
    if (nl) tg_road_shore_build(nl);
    /* [R22 item 10] Run the water-plane scan HERE, not in tg_apply_elevation:
     * the planes actually emitted use this post-network shore table, and the
     * facade authority (tg_facade_built) is only valid once the street network
     * has built its mouths. */
    if (nl) tg_r22_water_diag(nl);
}

int tg_road_wet_any(int si)
{
    if (si < 0 || si >= s_shore_n) return 0;
    return s_wet_any[si];
}

double tg_road_shore_d(int si, int is_left)
{
    if (si < 0 || si >= s_shore_n) return 1e9;
    return s_shore_d[si][is_left ? 0 : 1];
}

double tg_road_shore_y(int si, int is_left)
{
    if (si < 0 || si >= s_shore_n) return tg_world_sea_y();
    return s_shore_y[si][is_left ? 0 : 1];
}

/* [R22 item 10] Distance from the road edge to the far bank of the near water
 * body, or 1e9 if the water runs past the search cap (open water). */
double tg_road_shore_far(int si, int is_left)
{
    if (si < 0 || si >= s_shore_n) return 1e9;
    return s_shore_far[si][is_left ? 0 : 1];
}

double tg_road_water_side(int si)
{
    double l, r;
    if (si < 0 || si >= s_shore_n) return 0.0;
    l = s_shore_d[si][0]; r = s_shore_d[si][1];
    if (l > 1e8 && r > 1e8) return 0.0;
    return (l <= r) ? 1.0 : -1.0;
}

double tg_road_node_water_y(int i)
{
    if (i < 0 || i >= s_rn_n) return tg_world_sea_y();
    return s_rn[i].wy;
}

/* [R22 item 10 DIAGNOSTIC] Which of the four water authorities put a water
 * plane OVER the road, and is it a height error or the 50000-extent plane
 * crossing a road that curves back on itself?
 *
 * Reconstructs exactly the quad tg_emit_water lays for every wet span-side,
 * then scans every road node to find any that lies inside that quad's XZ
 * footprint with its road surface BELOW the plane -- water rendered over the
 * road. Reports the plane's source (sea vs river surface), its bounding centre
 * + radius (so the picked e<N> mesh can be matched), and the worst offender
 * node with its span distance (|j-si| large = the plane reached a DIFFERENT
 * part of the loop = an extent defect, not a height one). Gated, read-only. */
static int tg_r22_pt_in_quad(double x, double z,
                             const double *px, const double *pz)
{
    int i, pos = 0, neg = 0;
    for (i = 0; i < 4; i++) {
        const int j = (i + 1) & 3;
        const double cross = (px[j] - px[i]) * (z - pz[i])
                           - (pz[j] - pz[i]) * (x - px[i]);
        if (cross > 0.0) pos = 1; else if (cross < 0.0) neg = 1;
    }
    return !(pos && neg);
}

static void tg_r22_water_diag(const TG_NodeList *nl)
{
    const double sea = tg_world_sea_y();
    int si;
    if (!td5_env_flag_on("TD5RE_R22_WATER_DIAG")) return;
    TD5_LOG_I(LOG_TAG, "trackgen: [R22 WDIAG] sea level %.0f, extent %d, "
              "spans %d", sea, (int)TD5_TG_WATER_EXTENT, nl->count);
    for (si = 0; si + 1 < nl->count && si < s_shore_n - 1; si++) {
        int side_i;
        for (side_i = 0; side_i < 2; side_i++) {
            const int is_left = (side_i == 0);
            const double side = is_left ? 1.0 : -1.0;
            const TG_Node *n0 = &nl->v[si];
            const TG_Node *n1 = &nl->v[si + 1];
            double d0 = tg_road_shore_d(si, is_left);
            double d1 = tg_road_shore_d(si + 1, is_left);
            double px[4], pz[4], py0, py1, lx0, lz0, lx1, lz1, e0, e1;
            double cx = 0, cz = 0, rad = 0, cy;
            int j, worst_j = -1, covered = 0, target;
            double worst_delta = 0;
            if (!tg_water_span_clear(si)) continue;
            if (d0 > 1e8 && d1 > 1e8) continue;
            if (d0 > 1e8) d0 = d1;
            if (d1 > 1e8) d1 = d0;
            lx0 = n0->tz * side; lz0 = -n0->tx * side;
            lx1 = n1->tz * side; lz1 = -n1->tx * side;
            e0 = n0->width * 0.5 + d0 - 400.0;
            if (e0 < n0->width * 0.5) e0 = n0->width * 0.5;
            e1 = n1->width * 0.5 + d1 - 400.0;
            if (e1 < n1->width * 0.5) e1 = n1->width * 0.5;
            py0 = tg_road_shore_y(si, is_left);
            py1 = tg_road_shore_y(si + 1, is_left);
            /* Outer edge, mirroring tg_emit_water EXACTLY (including the far-bank
             * clip) so this scan reflects what actually ships: with the clip on
             * a river plane stops at its far bank, so a plane that used to reach
             * a loop-back road no longer covers it. */
            {
                double o0 = e0 + (double)TD5_TG_WATER_EXTENT;
                double o1 = e1 + (double)TD5_TG_WATER_EXTENT;
                if (td5_env_flag_on("TD5RE_R22_WATER_CLIP")) {
                    const double f0 = tg_road_shore_far(si, is_left);
                    const double f1 = tg_road_shore_far(si + 1, is_left);
                    if (py0 > sea + 1.0 && f0 < 1e8) {
                        const double c0 = n0->width * 0.5 + f0 + 400.0;
                        if (c0 < o0) o0 = c0;
                    }
                    if (py1 > sea + 1.0 && f1 < 1e8) {
                        const double c1 = n1->width * 0.5 + f1 + 400.0;
                        if (c1 < o1) o1 = c1;
                    }
                }
                px[0] = n0->x + lx0 * e0; pz[0] = n0->z + lz0 * e0;
                px[1] = n1->x + lx1 * e1; pz[1] = n1->z + lz1 * e1;
                px[2] = n1->x + lx1 * o1; pz[2] = n1->z + lz1 * o1;
                px[3] = n0->x + lx0 * o0; pz[3] = n0->z + lz0 * o0;
            }
            for (j = 0; j < 4; j++) { cx += px[j] * 0.25; cz += pz[j] * 0.25; }
            for (j = 0; j < 4; j++) {
                const double dx = px[j] - cx, dz = pz[j] - cz;
                const double r = sqrt(dx * dx + dz * dz);
                if (r > rad) rad = r;
            }
            cy = (py0 < py1 ? py0 : py1);
            {
            int fac_hits = 0, fac_j = -1;      /* quad over a CITY FACADE frontage */
            double fac_depth = 0.0;            /* deepest flood at a covered facade */
            /* Scan road EDGE points (both sides of every span's centreline)
             * covered by this plane and below its surface: this is the road the
             * water is laid over, not the centreline node the plane starts
             * outboard of. Also test the FACADE frontage (a band out to the verge
             * reach on any span/side that stands buildings) so "water near
             * buildings" is caught, not only water on the tarmac. */
            for (j = 0; j + 1 < nl->count; j++) {
                const TG_Node *nj = &nl->v[j];
                const double pyj = (py0 < py1 ? py0 : py1);   /* conservative flat plane */
                int e;
                if (j == si || j == si + 1) continue;
                for (e = 0; e < 2; e++) {
                    const double es = e ? -1.0 : 1.0;
                    const double ex = nj->x + nj->tz * es * nj->width * 0.5;
                    const double ez = nj->z - nj->tx * es * nj->width * 0.5;
                    double dist;
                    if (!tg_r22_pt_in_quad(ex, ez, px, pz)) continue;
                    if (nj->y >= pyj - 200.0) continue;
                    covered++;
                    dist = pyj - nj->y;
                    if (dist > worst_delta) { worst_delta = dist; worst_j = j; }
                    break;
                }
                /* Facade frontage: on a paved span that stands buildings, the
                 * facade line sits out beyond the road edge; sample the band. */
                if (tg_facade_stands(j)) {
                    for (e = 0; e < 2; e++) {
                        const double es = e ? -1.0 : 1.0;   /* e0 -> left(+1) */
                        double off;
                        if (!tg_facade_built(j, e == 0 ? 1 : 0)) continue;
                        for (off = nj->width * 0.5; off <= nj->width * 0.5 + 8000.0; off += 1500.0) {
                            const double fx = nj->x + nj->tz * es * off;
                            const double fz = nj->z - nj->tx * es * off;
                            if (!tg_r22_pt_in_quad(fx, fz, px, pz)) continue;
                            if (nj->y >= pyj - 200.0) continue;   /* facade base ~ road y */
                            fac_hits++; if (fac_j < 0) fac_j = j;
                            if (pyj - nj->y > fac_depth) fac_depth = pyj - nj->y;
                            break;
                        }
                    }
                }
            }
            target = (si >= 380 && si <= 391);
            /* Direct "over my own road edge" test: the plane's inner edge sits
             * at span si's road edge, so compare the surface to that road. */
            {
                const double own = (n0->y < n1->y ? n0->y : n1->y);
                const double over_own = cy - own;
                const int is_sea = !(py0 > sea + 1.0 || py1 > sea + 1.0);
                if (over_own > 200.0 || covered > 0 || fac_hits > 0 || target)
                    TD5_LOG_I(LOG_TAG, "trackgen: [R22 WDIAG] span %d %s: surf %.0f "
                              "(%s) d=%.0f far=%.0f, road.y %.0f (over own %.0f), "
                              "r%.0f; %d road + %d FACADE pt(s) UNDER plane (fac span "
                              "%d flood %.0f), worst j=%d dy=%.0f |dj|=%d",
                              si, is_left ? "L" : "R", cy, is_sea ? "SEA" : "river",
                              d0, tg_road_shore_far(si, is_left), n0->y, over_own, rad,
                              covered, fac_hits, fac_j, fac_depth, worst_j, worst_delta,
                              worst_j >= 0 ? (worst_j > si ? worst_j - si : si - worst_j) : 0);
            }
            }
        }
    }
}

/* ----------------------------------------------------------- elevation -- */

/* Finalise the profile: solve the last window, classify everything, lay
 * chords over structure runs (with the raised-cosine clearance hump a deck
 * needs over water), report, then conform the world to the road bed. */
/* [R22] May span `q` be absorbed into a bridge run by the abutment pad? Mirrors
 * every guard tg_road_classify applies, so padding can never produce a table
 * the classifier itself would have refused. */
static int tg_road_pad_ok(int q)
{
    int r, lo, hi;
    if (q < 0 || q >= s_struct_n) return 0;
    if (s_struct[q] != TG_ST_NONE) return 0;      /* already a structure  */
    if (q <= TD5_TG_GRID_SPAN + 24) return 0;     /* never off the grid   */
    if (tg_span_in_fork_run(q)) return 0;         /* fork geometry owns it */
    /* The bridge/tunnel interlock: a padded span must not bring a deck within
     * CLEAR spans of a bore, which is the one pairing the classifier resolves
     * by deleting a whole run. */
    lo = q - TD5_TG_BRIDGE_TUNNEL_CLEAR; if (lo < 0) lo = 0;
    hi = q + TD5_TG_BRIDGE_TUNNEL_CLEAR; if (hi > s_struct_n - 1) hi = s_struct_n - 1;
    for (r = lo; r <= hi; r++) if (s_struct[r] == TG_ST_TUNNEL) return 0;
    return 1;
}

/* [R22] ABUTMENT PAD -- "this bridge is too short and looks sloppy; bridges
 * should be longer".
 *
 * A span's kind is the OR of its two end nodes, so a SINGLE wet node yields
 * exactly two BRIDGE spans, and TG_ROAD_WATER_MIN is 2 -- so that two-span deck
 * survives as the shortest legal bridge: 3000 units of road, 12 vertices,
 * crossing a stream nobody can see. The classifier cannot help; it only ever
 * trims or deletes a run, there is no pass anywhere that lengthens one, and its
 * own comment on WATER_MIN promises "(+abutments)" that were never written.
 *
 * Raising WATER_MIN is the WRONG fix: a run shorter than the minimum is
 * DELETED, so a higher minimum turns each stream crossing into a causeway
 * through the water, which is worse than a short deck. Grow the run instead,
 * from both ends together so the deck stays centred on the water it crosses,
 * and the crossing gains approach spans that read as abutments.
 *
 * Runs AFTER the walk, on the final table: padding during the walk would extend
 * into spans that a later revision reclassifies. Everything downstream reads
 * this same table -- the chords and clearance hump below, the ground skirt's
 * `open` flag, the water emitter, the guard -- so all of them see the padded
 * run and none of them needs to know padding happened. */
static void tg_road_pad_bridges(int *out_runs, int *out_spans)
{
    const int want = td5_env_int("TD5RE_R22_BRIDGE_PAD", 10, 0, 64);
    const int cap  = tg_bridge_max_spans();
    const int target = (want < cap) ? want : cap;
    int s, runs = 0, added = 0;

    if (want <= 0) return;
    for (s = 0; s < s_struct_n; ) {
        int e = s, len;
        if (s_struct[s] != TG_ST_BRIDGE) { s++; continue; }
        while (e + 1 < s_struct_n && s_struct[e + 1] == TG_ST_BRIDGE) e++;
        len = e - s + 1;
        if (len < target) {
            runs++;
            while (len < target) {
                int grew = 0;
                if (tg_road_pad_ok(s - 1)) {
                    s_struct[--s] = TG_ST_BRIDGE; len++; added++; grew = 1;
                }
                if (len < target && tg_road_pad_ok(e + 1)) {
                    s_struct[++e] = TG_ST_BRIDGE; len++; added++; grew = 1;
                }
                if (!grew) break;   /* boxed in by grid, fork or a bore */
            }
        }
        s = e + 1;
    }
    if (out_runs) *out_runs = runs;
    if (out_spans) *out_spans = added;
}

void tg_apply_elevation(const TD5_TrackGenSpec *spec, TG_NodeList *nl)
{
    const double span_len = (double)spec->span_length;
    int i, s;
    int n_bridge = 0, n_tunnel = 0, runs_b = 0, runs_t = 0, longest_b = 0, longest_t = 0;
    int water_runs = 0, conform_spans = 0;
    double cut_max = 0.0, fill_max = 0.0;

    if (nl->count < 3) return;
    s_spec = spec;
    tg_track_min_y_invalidate();
    tg_road_revise(nl, NULL, NULL);
    s_struct_fin = s_struct_n;

    /* [R22] Grow short decks into abutments before anything reads the table. */
    {
        int pad_runs = 0, pad_spans = 0;
        tg_road_pad_bridges(&pad_runs, &pad_spans);
        if (pad_runs > 0)
            TD5_LOG_I(LOG_TAG, "trackgen: [R22 BRIDGE PAD] %d short deck(s) "
                      "grown by %d abutment span(s) (target %d, knob "
                      "TD5RE_R22_BRIDGE_PAD)", pad_runs, pad_spans,
                      td5_env_int("TD5RE_R22_BRIDGE_PAD", 10, 0, 64));
    }

    /* Chords over runs. */
    for (s = 0; s < s_struct_n; ) {
        const int k = s_struct[s];
        int e = s, len, q;
        double y0, y1, need = -1e30;
        if (k == TG_ST_NONE) { s++; continue; }
        while (e + 1 < s_struct_n && s_struct[e + 1] == k) e++;
        len = e - s + 1;
        y0 = nl->v[s].y; y1 = nl->v[e + 1].y;
        for (q = s; q <= e + 1; q++) {
            const double u = (double)(q - s) / (double)(e + 1 - s);
            nl->v[q].y = y0 + (y1 - y0) * u;
            if (k == TG_ST_BRIDGE && s_rn[q].wet && s_rn[q].wy + TG_ROAD_DECK_CLEAR > need)
                need = s_rn[q].wy + TG_ROAD_DECK_CLEAR;
        }
        if (k == TG_ST_BRIDGE) {
            /* Raised-cosine clearance hump, bounded by the grade budget. */
            double hrun = 0.0, allowed, chord = fabs(y1 - y0) / (double)len;
            for (q = s; q <= e + 1; q++) {
                const double lack = need - nl->v[q].y;
                if (lack > hrun) hrun = lack;
            }
            {   /* the run's LOWEST cap, so a deck crossing into a gentler
                 * biome never lands over that biome's limit */
                double cmin = tg_road_cap_at(s);
                for (q = s + 1; q <= e + 1; q++) {
                    const double cq = tg_road_cap_at(q);
                    if (cq < cmin) cmin = cq;
                }
                allowed = cmin * span_len - chord;
            }
            if (allowed < 0.0) allowed = 0.0;
            if (hrun > allowed * (double)len / TD5_TG_PI) hrun = allowed * (double)len / TD5_TG_PI;
            if (hrun > TD5_TG_BRIDGE_HEIGHT) hrun = TD5_TG_BRIDGE_HEIGHT;
            if (hrun > 0.0)
                for (q = s; q <= e; q++) {
                    const double t = ((double)(q - s) + 0.5) / (double)len;
                    nl->v[q].y += hrun * 0.5 * (1.0 - cos(2.0 * TD5_TG_PI * t));
                }
            n_bridge += len; runs_b++;
            if (len > longest_b) longest_b = len;
            if (need > -1e29) water_runs++;
        } else {
            n_tunnel += len; runs_t++;
            if (len > longest_t) longest_t = len;
        }
        s = e + 1;
    }

    /* Spawn anchor: y[0] is 0 by construction (tg_world_h(0,0) == 0 and the
     * grid straight is flat), enforced here in case a conform moved it. */
    {
        const double y0 = nl->v[0].y;
        if (fabs(y0) > 1e-6) for (i = 0; i < nl->count; i++) nl->v[i].y -= y0;
    }

    /* Report, in the names the tooling greps for. */
    {
        double lo = nl->v[0].y, hi = nl->v[0].y, wg = 0.0, *g, caplo = 1e9, caphi = 0.0;
        int n = 0, hits = 0;
        g = (double *)malloc(sizeof(double) * (size_t)nl->count);
        for (i = 0; i < nl->count; i++) {
            if (nl->v[i].y < lo) lo = nl->v[i].y;
            if (nl->v[i].y > hi) hi = nl->v[i].y;
        }
        for (i = 1; i < nl->count; i++) {
            const double gg = fabs(nl->v[i].y - nl->v[i - 1].y) / span_len;
            const double c = tg_road_cap_at(i);
            if (gg > wg) wg = gg;
            if (c < caplo) caplo = c;
            if (c > caphi) caphi = c;
            if (gg > c + 1e-6) hits++;
            if (g) g[n++] = gg;
        }
        if (g && n > 0) {
            qsort(g, (size_t)n, sizeof(double), tg_road_grade_cmp);
            TD5_LOG_I(LOG_TAG, "trackgen: [R21 GRADE] terrain-following -> p50 "
                      "%.4f p90 %.4f p99 %.4f max %.4f (aim %.3f, per-biome cap "
                      "%.3f..%.3f, %d span(s) over cap)",
                      g[n / 2], g[(n * 9) / 10], g[(n * 99) / 100], g[n - 1],
                      s_max_grade, caplo, caphi, hits);
        }
        free(g);
        TD5_LOG_I(LOG_TAG, "trackgen: [R8 SHAPE] relief=world height min %.0f max "
                  "%.0f RANGE %.0f, worst grade %.4f (cap %.3f)", lo, hi, hi - lo, wg, caphi);
    }

    /* [R22 item 6] Smoothness: the per-span change in grade (curvature -- the
     * kink metric TD5RE_R22_SMOOTH bounds) and the cut/fill depth distribution
     * (how deep the road is trenched into the terrain), both as p50/p90/p99 so
     * the smoothing can be measured against the number to judge it on -- not the
     * worst value, which the caps pin. Cut/fill on OPEN spans only; structures
     * ride above/below the ground by design. */
    {
        double *cv, *cf; int nc = 0, nf = 0, cut_span = -1; double cut_worst = 0.0;
        cv = (double *)malloc(sizeof(double) * (size_t)(nl->count + 1));
        cf = (double *)malloc(sizeof(double) * (size_t)(nl->count + 1));
        for (i = 2; i < nl->count; i++) {
            const double g0 = (nl->v[i - 1].y - nl->v[i - 2].y) / span_len;
            const double g1 = (nl->v[i].y - nl->v[i - 1].y) / span_len;
            if (cv) cv[nc++] = fabs(g1 - g0);
        }
        for (i = 0; i < nl->count; i++)
            if (tg_struct_kind(i) == TG_ST_NONE && i < s_rn_n && cf) {
                const double cd = fabs(nl->v[i].y - s_rn[i].h);
                cf[nf++] = cd;
                if (cd > cut_worst) { cut_worst = cd; cut_span = i; }
            }
        if (cv && nc > 0) qsort(cv, (size_t)nc, sizeof(double), tg_road_grade_cmp);
        if (cf && nf > 0) qsort(cf, (size_t)nf, sizeof(double), tg_road_grade_cmp);
        TD5_LOG_I(LOG_TAG, "trackgen: [R22 SMOOTH] curvature (d-grade/span) p50 "
                  "%.4f p90 %.4f p99 %.4f max %.4f; cut/fill p50 %.0f p90 %.0f "
                  "p99 %.0f max %.0f at span %d (curv cap knob TD5RE_R22_SMOOTH=%d)",
                  nc ? cv[nc / 2] : 0.0, nc ? cv[(nc * 9) / 10] : 0.0,
                  nc ? cv[(nc * 99) / 100] : 0.0, nc ? cv[nc - 1] : 0.0,
                  nf ? cf[nf / 2] : 0.0, nf ? cf[(nf * 9) / 10] : 0.0,
                  nf ? cf[(nf * 99) / 100] : 0.0, nf ? cf[nf - 1] : 0.0, cut_span,
                  td5_env_int("TD5RE_R22_SMOOTH", 45, 0, 1000));
        free(cv); free(cf);
    }

    /* [R22 item 13b MEASUREMENT] The grade-relief OPPORTUNITY: contiguous open
     * (TG_ST_NONE) runs whose solved profile sits far enough above the natural
     * ground to have qualified as a viaduct (> TG_ROAD_BRIDGE_LIFT) but which
     * are NOT structures -- i.e. dips the limiter held the road high across, that
     * the classifier then left as embankments because the run was shorter than
     * TG_ROAD_BRIDGE_MIN. Same for would-be tunnels (cuttings). Bucketed by run
     * length so "a handful of 2-span dips" is distinguishable from a real
     * population. Gated, read-only -- decides whether 13b is worth building. */
    if (td5_env_flag_on("TD5RE_R22_RELIEF_DIAG")) {
        int b12 = 0, b35 = 0, b6 = 0, t12 = 0, t35 = 0, t6 = 0;
        int b_spans = 0, t_spans = 0;
        double b_max = 0.0, t_max = 0.0;
        int s2;
        for (s2 = 0; s2 < nl->count; ) {
            int e2 = s2, kind = 0;         /* 1 = would-be bridge, -1 = tunnel */
            double d = (s2 < s_rn_n) ? nl->v[s2].y - s_rn[s2].h : 0.0;
            if (tg_struct_kind(s2) == TG_ST_NONE && d > TG_ROAD_BRIDGE_LIFT) kind = 1;
            else if (tg_struct_kind(s2) == TG_ST_NONE && -d > TG_ROAD_TUNNEL_DEPTH) kind = -1;
            if (!kind) { s2++; continue; }
            while (e2 + 1 < nl->count && tg_struct_kind(e2 + 1) == TG_ST_NONE) {
                const double dn = (e2 + 1 < s_rn_n) ? nl->v[e2 + 1].y - s_rn[e2 + 1].h : 0.0;
                if (kind == 1 && dn > TG_ROAD_BRIDGE_LIFT) { if (dn > b_max) b_max = dn; e2++; }
                else if (kind == -1 && -dn > TG_ROAD_TUNNEL_DEPTH) { if (-dn > t_max) t_max = -dn; e2++; }
                else break;
            }
            {
                const int len = e2 - s2 + 1;
                if (kind == 1) { b_spans += len; if (len <= 2) b12++; else if (len <= 5) b35++; else b6++; }
                else           { t_spans += len; if (len <= 2) t12++; else if (len <= 5) t35++; else t6++; }
            }
            s2 = e2 + 1;
        }
        TD5_LOG_I(LOG_TAG, "trackgen: [R22 RELIEF] would-be VIADUCTS (open, lift>%d): "
                  "%d run(s) / %d span(s) [1-2:%d 3-5:%d 6+:%d] max lift %.0f; "
                  "would-be TUNNELS (cut>%d): %d run(s) / %d span(s) [1-2:%d 3-5:%d 6+:%d] "
                  "max cut %.0f", (int)TG_ROAD_BRIDGE_LIFT, b12 + b35 + b6, b_spans,
                  b12, b35, b6, b_max, (int)TG_ROAD_TUNNEL_DEPTH, t12 + t35 + t6,
                  t_spans, t12, t35, t6, t_max);
    }

    /* Conform the world to the road bed on open spans; structures leave the
     * ground alone except at their mouths, so the terrain meets the deck end
     * / portal. */
    for (s = 0; s < nl->count - 1; s++) {
        const TG_Node *a = &nl->v[s], *b = &nl->v[s + 1];
        const int k = tg_struct_kind(s);
        const double da = a->y - s_rn[s].h, db = b->y - s_rn[s + 1].h;
        const double dav = 0.5 * (fabs(da) + fabs(db));
        tg_world_occ_seg(a->x, a->z, b->x, b->z,
                         (a->width > b->width ? a->width : b->width) * 0.5 + 600.0,
                         TG_WO_ROAD);
        if (k == TG_ST_NONE) {
            const double half = (a->width > b->width ? a->width : b->width) * 0.5;
            tg_world_conform_seg(a->x, a->z, a->y, b->x, b->z, b->y,
                                 half + TD5_TG_ROAD_BED_VERGE,
                                 TG_ROAD_CONFORM_BLEND + dav * 1.5);
            conform_spans++;
            if (da < -cut_max) cut_max = -da;
            if (da > fill_max) fill_max = da;
        } else {
            const int prev = tg_struct_kind(s - 1), next = tg_struct_kind(s + 1);
            if (prev == TG_ST_NONE)
                tg_world_conform(a->x, a->z, a->y, a->width * 0.5 + TD5_TG_ROAD_BED_VERGE, 3000.0);
            if (next == TG_ST_NONE)
                tg_world_conform(b->x, b->z, b->y, b->width * 0.5 + TD5_TG_ROAD_BED_VERGE, 3000.0);
        }
    }

    tg_road_shore_build(nl);
    {
        int wet_spans = 0, coast_spans = 0, in_bed = 0, in_bed_raw = 0;
        for (s = 0; s < s_shore_n; s++) {
            if (s < s_rn_n && s_rn[s].wet) wet_spans++;
            else if (s_wet_any[s]) coast_spans++;
            /* [R22] The invariant the shore fix establishes, counted so it can
             * be checked instead of believed: no OPEN span's shore may sit
             * inside its own road bed. tg_emit_water clamps a quad's inner edge
             * to the road edge, so a shore at d < the bed verge is a water plane
             * laid over the asphalt -- the reported "water rendered over the
             * road". A conformed bed is not water (tg_world_is_water), so after
             * the fix this is 0 on every open span; a bridge run has no bed and
             * is excluded. */
            if (s < s_struct_n && s_struct[s] == TG_ST_NONE &&
                (s_shore_d[s][0] < TD5_TG_ROAD_BED_VERGE ||
                 s_shore_d[s][1] < TD5_TG_ROAD_BED_VERGE)) in_bed++;
            /* [R22] The same count under the OLD raw test, so one run reports
             * both and the fix is measured rather than assumed. Reporting only
             * the post-fix count is a false green: it says the invariant holds,
             * not that anything was ever wrong. Re-walks the first bed-width of
             * the ray with the raw comparison the shore build used to make. */
            if (s < s_struct_n && s_struct[s] == TG_ST_NONE && s < nl->count) {
                const TG_Node *n = &nl->v[s];
                const double half = n->width * 0.5;
                int side2;
                for (side2 = 0; side2 < 2 && s < s_shore_n; side2++) {
                    const double sgn = side2 ? -1.0 : 1.0;
                    const double lx = n->tz * sgn, lz = -n->tx * sgn;
                    double d2;
                    for (d2 = 0.0; d2 < TD5_TG_ROAD_BED_VERGE; d2 += 500.0) {
                        const double wx = n->x + lx * (half + d2);
                        const double wz = n->z + lz * (half + d2);
                        if (tg_world_h(wx, wz) < tg_world_water_y(wx, wz)) {
                            in_bed_raw++;
                            side2 = 2;      /* count the span once */
                            break;
                        }
                    }
                }
            }
        }
        TD5_LOG_I(LOG_TAG, "trackgen: [WATER] %d span(s) over water, %d with a "
                  "shore within reach, sea level %.0f; %d open span(s) with a "
                  "shore INSIDE the road bed (must be 0; the pre-R22 raw test "
                  "found %d)", wet_spans, coast_spans,
                  tg_world_sea_y(), in_bed, in_bed_raw);
    }
    /* [R22 item 10] Water-plane extent: how many river span-sides the far-bank
     * clip shortens, and by how much. The OLD rule sheeted every plane a fixed
     * TD5_TG_WATER_EXTENT (50000) from the near bank; a river 30000 off the road
     * then laid a flat water quad across ~48000 of DRY land -- the reported
     * inconsistent levels / water over terrain. Reported under both rules in one
     * run so the change is measured, not asserted: N of M river span-sides
     * clipped, mean/max reduction; open water (sea, or water past the search
     * cap) is unchanged. */
    {
        const double sea = tg_world_sea_y();
        int rivers = 0, clipped = 0;
        double red_sum = 0.0, red_max = 0.0;
        for (s = 0; s + 1 < nl->count && s < s_shore_n; s++) {
            int side2;
            for (side2 = 0; side2 < 2; side2++) {
                const int is_left = (side2 == 0);
                const double d = s_shore_d[s][side2];
                const double sy = s_shore_y[s][side2];
                const double fbank = s_shore_far[s][side2];
                double old_out, new_out;
                if (d > 1e8) continue;                 /* no water this side   */
                if (sy <= sea + 1.0) continue;         /* sea: never clipped   */
                rivers++;
                if (fbank > 1e8) continue;             /* runs past cap: kept  */
                old_out = d + (double)TD5_TG_WATER_EXTENT;
                new_out = fbank + 400.0;
                if (new_out < old_out) {
                    const double red = old_out - new_out;
                    clipped++; red_sum += red;
                    if (red > red_max) red_max = red;
                }
                (void)is_left;
            }
        }
        TD5_LOG_I(LOG_TAG, "trackgen: [R22 WATER CLIP] %d river span-side(s), "
                  "%d clipped to the far bank (mean %.0f, max %.0f units off a "
                  "50000 sheet; knob TD5RE_R22_WATER_CLIP)", rivers, clipped,
                  clipped ? red_sum / clipped : 0.0, red_max);
    }

    TD5_LOG_I(LOG_TAG, "trackgen: [STRUCT] bridges %d run(s) / %d span(s) (longest %d, "
              "cap %d, %d over water), tunnels %d run(s) / %d span(s) (longest %d, cap "
              "%d), %d open span(s) conformed (deepest cut %.0f, highest fill %.0f), "
              "%ld forced conform(s)",
              runs_b, n_bridge, longest_b, tg_bridge_max_spans(), water_runs,
              runs_t, n_tunnel, longest_t, tg_tunnel_max_spans(),
              conform_spans, cut_max, fill_max, s_stat_conforms);
    {
        int first_b = -1, first_t = -1;
        for (s = 0; s < s_struct_n; s++) {
            if (first_b < 0 && s_struct[s] == TG_ST_BRIDGE) first_b = s;
            if (first_t < 0 && s_struct[s] == TG_ST_TUNNEL) first_t = s;
        }
        for (s = 0; s < s_struct_n; ) {
            int e = s;
            if (s_struct[s] == TG_ST_NONE) { s++; continue; }
            while (e + 1 < s_struct_n && s_struct[e + 1] == s_struct[s]) e++;
            TD5_LOG_I(LOG_TAG, "trackgen: [STRUCT]   %s spans %d-%d (%d)%s",
                      s_struct[s] == TG_ST_BRIDGE ? "bridge" : "tunnel", s, e,
                      e - s + 1, (s_struct[s] == TG_ST_BRIDGE && s_rn[s + 1].wet) ? " water" : "");
            s = e + 1;
        }
        (void)first_b; (void)first_t;
    }
}

/* Ground under main node i as the road module measured it (for audits). */
double tg_road_ground_y(int i)
{
    if (i < 0 || i >= s_rn_n) return 0.0;
    return s_rn[i].h;
}

int tg_road_node_forced(int i)
{
    if (i < 0 || i >= s_rn_n) return 0;
    return s_rn[i].force;
}

int tg_road_node_wet(int i)
{
    if (i < 0 || i >= s_rn_n) return 0;
    return s_rn[i].wet;
}
