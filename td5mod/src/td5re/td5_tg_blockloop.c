/**
 * td5_tg_blockloop.c -- GEO TRACK: BLOCK-LOOP FORKS, drive straight into a perpendicular
 * street (PORT-ONLY).
 *
 * [ROUND 1018 L] Mariano: "allow driving on lanes that are not starting as a divergence of an
 * existing lane, like driving straight into a perpendicular fork". A block loop leaves the
 * route at a real OSM intersection by turning about 90 degrees into a real cross street,
 * follows the real street grid (cross street, parallel street, next cross street) and rejoins
 * the route through a second cross street. Every loop rejoins: there are no dead ends.
 *
 * WHAT IT IS NOT. It is not a fork in the engine's sense (a widened cross-section that splits):
 * the main road is not touched at all. The corridor's first span stands against the road's
 * right edge with its axis perpendicular to the road, and its last span stands against the
 * edge again. The engine learns where the road is open from the SLNK trailer
 * (td5_track.c, "SIDE LINKS"): the walker relocates a probe that crosses the open rail, the
 * wall resolver stands the rail down there.
 *
 * WHAT IT SHARES WITH A FORK. The span bookkeeping: F (the departure span, type 8, link_next =
 * first corridor span) and R (the arrival span, type 11, link_prev = last corridor span), the
 * appended corridor (type 9 ... type 10), the jump record [cbase, cbase+clen-1, F+1] and the
 * variable-length corridor map, so progress, the AI's branch commit and the minimap read
 * a loop like any other corridor. Lane arithmetic does not apply (the departure span keeps
 * the road's own lane count; the engine never sends a sub-lane into link_next there).
 *
 * WHERE IT RUNS. After the street network: the loop is built from two accepted RIGHT arms
 * of real streets (their mouths are validated against the raster and keep the frontage
 * open), joined by a shortest path over the real OSM way graph (ROADS.JSON). The corridor is
 * the way vertices, rounded with a minimum-radius fillet, resampled to one span per
 * TD5_TG_SPAN_LENGTH, with the mouth flared so a car can swing in.
 *
 * KNOBS (all default ON / sane, =0 restores master exactly):
 *   TD5RE_GEO_FORK_BLOCKLOOP        master switch for the generator
 *   TD5RE_GEO_BLOCKLOOP_MAX         most loops on a route (default 6)
 *   TD5RE_GEO_BLOCKLOOP_ONLY        "<span>[,<span>...]": only loops whose departure mouth is
 *                                    within 6 spans of one of these (harness / hand-picked)
 *   TD5RE_GEO_BLOCKLOOP_RMIN_M      corner radius wanted, metres (default 10)
 *   TD5RE_GEO_BLOCKLOOP_RFLOOR_M    tightest corner a loop may keep, metres (default 7)
 *   TD5RE_GEO_BLOCKLOOP_FLARE       mouth flare per side, world units (default 3000: a kerb return)
 *   TD5RE_GEO_BLOCKLOOP_FLARE_ROWS  rows (spans) it tapers over (default 5)
 *   TD5RE_GEO_BLOCKLOOP_LMAX_M      longest loop, metres (default 420)
 *   TD5RE_GEO_BLOCKLOOP_RATIO_MIN/MAX  corridor length over the bypassed main length (1.4 .. 8)
 *   TD5RE_GEO_FORK_DIAG=1           why a candidate was refused
 *
 * BYTE-IDENTICAL SYNTHETIC BUILDS. Every entry point returns "nothing" with no geo place
 * loaded, and no tg_rand / tg_frand / tg_range is called here.
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"

#define BL_UPM        430.0          /* world units per metre (the place cache's scale) */
#define BL_MAXPLAN    16
#define BL_HSIZE      32768
#define BL_JOIN_D2    (80.0 * 80.0)  /* two way vertices closer than this are one node  */
#define BL_LEAD_SP    4.0            /* spans the corridor runs perpendicular to the road */

typedef struct { double x, z; int head, hnext; int ri; double lat, dist; } BlNode;
typedef struct { int to, way, next; float len; } BlEdge;

typedef struct {
    int    used, built, committed;
    int    fi;
    int    e1, e2;                   /* the two network edges (real street arms)        */
    int    si1, si2;                 /* mouth centre spans                               */
    int    a1_lo, a1_hi, a2_lo, a2_hi;
    int    F, R, len, clen, lb;
    double flare;
    double len_m, main_m, ratio, min_r_m;
    int    ncorner, npath;
    double *cx, *cz, *cy;            /* clen+1 chain points                              */
    TG_NodeList rview;               /* v[F+1+k] = corridor row k                        */
    char   name[96];
} BlPlan;

static BlPlan  s_bl[BL_MAXPLAN];
static int     s_nbl;
static int     s_diag;
static const char *s_why = "";
static double  s_clear;              /* least distance of a corridor node from the route's centre line */

static BlNode *g_n;
static BlEdge *g_e;
static int     g_nn, g_ne, g_ncap, g_ecap;
static int    *g_hash;
typedef struct { double d; int n; } BlHeap;
static double *g_dist;               /* search scratch, sized to the graph */
static int    *g_prev;
static BlHeap *g_heap;
static int     g_heapcap;

static const double kPi = 3.14159265358979323846;

void tg_bl_reset(void)
{
    int i;
    for (i = 0; i < s_nbl; i++) {
        free(s_bl[i].cx); free(s_bl[i].cy); free(s_bl[i].cz);
        free(s_bl[i].rview.v);
    }
    memset(s_bl, 0, sizeof s_bl);
    s_nbl = 0;
}

static void bl_note(const char *what, int si1, int si2, const char *why)
{
    if (!s_diag) return;
    TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] REJECT %s si %d -> %d: %s", what, si1, si2, why);
}

static double bl_lw(void) { return (double)TD5_TG_LANE_WIDTH; }

/* ------------------------------------------------------------ the way graph */

static void bl_graph_free(void)
{
    free(g_n); g_n = NULL;
    free(g_e); g_e = NULL;
    free(g_hash); g_hash = NULL;
    free(g_dist); g_dist = NULL;
    free(g_prev); g_prev = NULL;
    free(g_heap); g_heap = NULL;
    g_nn = g_ne = g_ncap = g_ecap = g_heapcap = 0;
}

static unsigned bl_cell_hash(int cx, int cz)
{
    return ((unsigned)cx * 73856093u ^ (unsigned)cz * 19349663u) & (BL_HSIZE - 1);
}

static int bl_node_find(double x, double z)
{
    const int cx = (int)floor(x / 128.0), cz = (int)floor(z / 128.0);
    int dx, dz;
    for (dz = -1; dz <= 1; dz++)
        for (dx = -1; dx <= 1; dx++) {
            int n = g_hash[bl_cell_hash(cx + dx, cz + dz)];
            while (n >= 0) {
                const double ex = g_n[n].x - x, ez = g_n[n].z - z;
                if (ex * ex + ez * ez < BL_JOIN_D2) return n;
                n = g_n[n].hnext;
            }
        }
    return -1;
}

static int bl_node_add(double x, double z)
{
    int n = bl_node_find(x, z);
    unsigned h;
    if (n >= 0) return n;
    if (g_nn >= g_ncap) {
        const int nc = g_ncap ? g_ncap * 2 : 4096;
        BlNode *p = (BlNode *)realloc(g_n, (size_t)nc * sizeof(BlNode));
        if (!p) return -1;
        g_n = p; g_ncap = nc;
    }
    n = g_nn++;
    memset(&g_n[n], 0, sizeof g_n[n]);
    g_n[n].x = x; g_n[n].z = z; g_n[n].head = -1; g_n[n].ri = -1;
    h = bl_cell_hash((int)floor(x / 128.0), (int)floor(z / 128.0));
    g_n[n].hnext = g_hash[h];
    g_hash[h] = n;
    return n;
}

static void bl_edge_add(int a, int b, int way, float len)
{
    int k;
    if (a == b || a < 0 || b < 0) return;
    for (k = g_n[a].head; k >= 0; k = g_e[k].next)
        if (g_e[k].to == b && g_e[k].way == way) return;
    if (g_ne + 2 > g_ecap) {
        const int nc = g_ecap ? g_ecap * 2 : 8192;
        BlEdge *p = (BlEdge *)realloc(g_e, (size_t)nc * sizeof(BlEdge));
        if (!p) return;
        g_e = p; g_ecap = nc;
    }
    g_e[g_ne].to = b; g_e[g_ne].way = way; g_e[g_ne].len = len; g_e[g_ne].next = g_n[a].head;
    g_n[a].head = g_ne++;
    g_e[g_ne].to = a; g_e[g_ne].way = way; g_e[g_ne].len = len; g_e[g_ne].next = g_n[b].head;
    g_n[b].head = g_ne++;
}

/* Every drivable at-grade way near the route, as a graph whose nodes are its vertices
 * (vertices within 0.2 m are one node: that is how OSM ways meet). */
static int bl_graph_build(const TG_NodeList *nl, int nspans)
{
    const int nw = td5_geo_roads_count();
    double minx = 1e30, minz = 1e30, maxx = -1e30, maxz = -1e30;
    const double pad = 330.0 * BL_UPM;
    int i, w;
    bl_graph_free();
    if (nw < 1) return 0;
    g_hash = (int *)malloc(sizeof(int) * BL_HSIZE);
    if (!g_hash) return 0;
    for (i = 0; i < BL_HSIZE; i++) g_hash[i] = -1;
    for (i = 0; i <= nspans && i < nl->count; i++) {
        if (nl->v[i].x < minx) minx = nl->v[i].x;
        if (nl->v[i].x > maxx) maxx = nl->v[i].x;
        if (nl->v[i].z < minz) minz = nl->v[i].z;
        if (nl->v[i].z > maxz) maxz = nl->v[i].z;
    }
    for (w = 0; w < nw; w++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(w);
        int k;
        if (!r || r->count < 2 || r->bridge || r->tunnel || r->layer != 0) continue;
        if (r->maxx < minx - pad || r->minx > maxx + pad || r->maxz < minz - pad || r->minz > maxz + pad) continue;
        {
            double px, pz;
            int pn = -1;
            for (k = 0; k < r->count; k++) {
                double x, z;
                int n;
                if (!td5_geo_roads_point(r, k, &x, &z)) break;
                n = bl_node_add(x, z);
                if (n < 0) break;
                if (pn >= 0 && pn != n)
                    bl_edge_add(pn, n, w, (float)sqrt((x - px) * (x - px) + (z - pz) * (z - pz)));
                pn = n; px = x; pz = z;
            }
        }
    }
    /* each node's nearest route node and signed lateral (+ = left of travel) */
    for (i = 0; i < g_nn; i++) {
        double best = 1e300;
        int j, bj = -1;
        for (j = 0; j <= nspans && j < nl->count; j++) {
            const double dx = g_n[i].x - nl->v[j].x, dz = g_n[i].z - nl->v[j].z;
            const double d = dx * dx + dz * dz;
            if (d < best) { best = d; bj = j; }
        }
        g_n[i].ri = bj;
        g_n[i].dist = sqrt(best);
        if (bj >= 0)
            g_n[i].lat = (g_n[i].x - nl->v[bj].x) * nl->v[bj].tz - (g_n[i].z - nl->v[bj].z) * nl->v[bj].tx;
    }
    return g_nn > 0;
}

/* ------------------------------------------------------------ the search */

static void bl_heap_push(BlHeap *h, int *nh, double d, int n)
{
    int i = (*nh)++;
    h[i].d = d; h[i].n = n;
    while (i > 0) {
        const int p = (i - 1) / 2;
        if (h[p].d <= h[i].d) break;
        { BlHeap t = h[p]; h[p] = h[i]; h[i] = t; }
        i = p;
    }
}

static int bl_heap_pop(BlHeap *h, int *nh, double *d)
{
    int i = 0, n;
    if (*nh <= 0) return -1;
    n = h[0].n; *d = h[0].d;
    h[0] = h[--(*nh)];
    for (;;) {
        const int l = 2 * i + 1, r = l + 1;
        int m = i;
        if (l < *nh && h[l].d < h[m].d) m = l;
        if (r < *nh && h[r].d < h[m].d) m = r;
        if (m == i) break;
        { BlHeap t = h[m]; h[m] = h[i]; h[i] = t; }
        i = m;
    }
    return n;
}

/* The route side test for a graph node: on the RIGHT of travel and clear of every carriageway
 * (the road, an avenue's far carriageway, a fork corridor) by `margin`. */
static int bl_node_ok(const TG_NodeList *nl, int n, double margin)
{
    const BlNode *g = &g_n[n];
    double reach;
    if (g->ri < 0 || !(g->lat < 0.0)) return 0;
    reach = tg_carriageway_reach(nl, g->ri, -1.0);
    if (reach + margin < s_clear) reach = s_clear - margin;   /* nothing hugs the road: its other carriageway, a service lane */
    return -g->lat > reach + margin;
}

/* ------------------------------------------------------------ corner rounding */

/* Round the polyline's corners with fillets. Returns the number of output points (0 on a
 * failure) and the tightest radius kept. `rwant` is the radius asked for, `rfloor` the least
 * allowed (a corner that cannot keep it makes the loop fail). */
static int bl_fillet(const double *px, const double *pz, int np, double rwant, double rfloor,
                     double *ox, double *oz, int cap, double *minr, int *ncorner)
{
    int i, n = 0;
    *minr = 1e300;
    *ncorner = 0;
    if (np < 2 || cap < 8) return 0;
    ox[n] = px[0]; oz[n] = pz[0]; n++;
    for (i = 1; i + 1 < np; i++) {
        const double ax = px[i] - px[i - 1], az = pz[i] - pz[i - 1];
        const double bx = px[i + 1] - px[i], bz = pz[i + 1] - pz[i];
        const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
        double d0x, d0z, d1x, d1z, th, t, r, cr, dt;
        double sx, sz, ex, ez, nx, nz, cxs, czs, a0, a1;
        int steps, q;
        if (la < 1.0 || lb < 1.0) continue;
        d0x = ax / la; d0z = az / la; d1x = bx / lb; d1z = bz / lb;
        dt = d0x * d1x + d0z * d1z;
        if (dt > 1.0) dt = 1.0;
        if (dt < -1.0) dt = -1.0;
        th = acos(dt);
        if (th < 0.035) {                         /* a straight pass-through */
            if (n < cap) { ox[n] = px[i]; oz[n] = pz[i]; n++; }
            continue;
        }
        if (th > kPi - 0.2) { s_why = "a hairpin"; return 0; }             /* a hairpin */
        t = rwant * tan(th * 0.5);
        if (t > 0.5 * la) t = 0.5 * la;
        if (t > 0.5 * lb) t = 0.5 * lb;
        r = t / tan(th * 0.5);
        if (r < rfloor) {
            static char wb[96];
            snprintf(wb, sizeof wb, "a %.1f m corner (legs %.0f / %.0f m, turn %.0f deg)", r / BL_UPM, la / BL_UPM, lb / BL_UPM, th * 180.0 / kPi);
            s_why = wb;
            return 0;
        }
        if (r < *minr) *minr = r;
        if (th > 0.52) (*ncorner)++;
        sx = px[i] - d0x * t; sz = pz[i] - d0z * t;
        ex = px[i] + d1x * t; ez = pz[i] + d1z * t;
        cr = d0x * d1z - d0z * d1x;               /* > 0: turn toward +z from +x ... sign decides the side */
        nx = (cr > 0.0) ? -d0z : d0z;             /* unit normal to d0 toward the turn side (xz plane) */
        nz = (cr > 0.0) ?  d0x : -d0x;
        cxs = sx + nx * r; czs = sz + nz * r;
        a0 = atan2(sz - czs, sx - cxs);
        a1 = atan2(ez - czs, ex - cxs);
        {   /* sweep the short way round */
            double da = a1 - a0;
            while (da >  kPi) da -= 2.0 * kPi;
            while (da < -kPi) da += 2.0 * kPi;
            steps = (int)(fabs(da) / 0.12) + 2;
            if (n + steps + 2 >= cap) { s_why = "too many points"; return 0; }
            for (q = 0; q <= steps; q++) {
                const double a = a0 + da * (double)q / (double)steps;
                ox[n] = cxs + cos(a) * r; oz[n] = czs + sin(a) * r; n++;
            }
        }
    }
    if (n < cap) { ox[n] = px[np - 1]; oz[n] = pz[np - 1]; n++; }
    return n;
}

/* ------------------------------------------------------------ main-road geometry */

/* The road's right edge point at node si and the outward unit (xz). */
static int bl_edge_at(const TG_NodeList *nl, int si, double *ex, double *ey, double *ez,
                      double *ox, double *oz)
{
    double e[10];
    if (si < 0 || si + 1 >= nl->count) return 0;
    tg_city_edge_frame(nl, si, -1.0, e);
    *ex = e[0]; *ey = e[1]; *ez = e[2]; *ox = e[6]; *oz = e[7];
    return 1;
}

/* Main spans whose right edge lies against the mouth's near row: those within half the row's
 * width (plus the span's own half length) along the road from the mouth centre. */
static void bl_mouth_range(const TG_NodeList *nl, int si, double half_row, int *lo, int *hi)
{
    double mx, my, mz, ox, oz;
    int s, l = si, h = si;
    if (!bl_edge_at(nl, si, &mx, &my, &mz, &ox, &oz)) { *lo = si; *hi = si; return; }
    for (s = si - 10; s <= si + 10; s++) {
        double ax, ay, az, bx, by, bz, oxx, ozz, u0, u1;
        if (s < 1 || s + 2 >= nl->count) continue;
        if (!bl_edge_at(nl, s, &ax, &ay, &az, &oxx, &ozz) || !bl_edge_at(nl, s + 1, &bx, &by, &bz, &oxx, &ozz)) continue;
        u0 = (ax - mx) * nl->v[si].tx + (az - mz) * nl->v[si].tz;
        u1 = (bx - mx) * nl->v[si].tx + (bz - mz) * nl->v[si].tz;
        if (u0 > u1) { const double t = u0; u0 = u1; u1 = t; }
        if (u1 > -half_row + 200.0 && u0 < half_row - 200.0) {
            if (s < l) l = s;
            if (s > h) h = s;
        }
    }
    *lo = l; *hi = h;
}

static int bl_lanes_uniform(const TG_NodeList *nl, int lo, int hi)
{
    int s;
    if (lo < 0 || hi + 1 >= nl->count) return 0;
    for (s = lo; s <= hi; s++)
        if (nl->v[s].lanes != nl->v[lo].lanes) return 0;
    return 1;
}

/* ------------------------------------------------------------ candidates */

/* The graph node an arm's path starts from: walking its real way outward from the junction, the
 * first vertex that is right of the road and clear of every carriageway. The junction's own vertex
 * (and a shared-carriageway run's vertices) lie ON the road. */
static int bl_arm_node(const TG_NodeList *nl, const TG_NetArm *A)
{
    const TD5_GeoRoad *r = td5_geo_roads_get(A->road);
    int k, tries;
    if (!r) return -1;
    for (k = A->k0, tries = 0; k >= 0 && k < r->count && tries < 10; k += (A->step >= 0 ? 1 : -1), tries++) {
        double x, z;
        int n;
        if (!td5_geo_roads_point(r, k, &x, &z)) break;
        n = bl_node_find(x, z);
        if (n >= 0 && bl_node_ok(nl, n, 4.0 * BL_UPM)) return n;
    }
    return -1;
}

/* The path shown to the planner: from the arm's first way vertex to the second arm's, over
 * the real graph, keeping right of the road. `src`/`dst` are graph nodes. */
static int bl_path(const TG_NodeList *nl, int src, int dst, double lmax,
                   int *out, int cap, double *len_out)
{
    int nh = 0, n, i;
    double d, margin = 4.0 * BL_UPM;
    if (!g_dist) {
        g_heapcap = g_ne + g_nn + 16;
        g_dist = (double *)malloc(sizeof(double) * (size_t)g_nn);
        g_prev = (int *)malloc(sizeof(int) * (size_t)g_nn);
        g_heap = (BlHeap *)malloc(sizeof(BlHeap) * (size_t)g_heapcap);
        if (!g_dist || !g_prev || !g_heap) {
            free(g_dist); free(g_prev); free(g_heap);
            g_dist = NULL; g_prev = NULL; g_heap = NULL;
            return 0;
        }
    }
    for (i = 0; i < g_nn; i++) { g_dist[i] = 1e300; g_prev[i] = -1; }
    g_dist[src] = 0.0;
    bl_heap_push(g_heap, &nh, 0.0, src);
    while ((n = bl_heap_pop(g_heap, &nh, &d)) >= 0) {
        int k;
        if (d > g_dist[n]) continue;
        if (n == dst) break;
        if (d > lmax) break;
        for (k = g_n[n].head; k >= 0; k = g_e[k].next) {
            const int m = g_e[k].to;
            const double nd = d + (double)g_e[k].len;
            if (nd >= g_dist[m] || nd > lmax) continue;
            if (m != dst && !bl_node_ok(nl, m, margin)) continue;
            g_dist[m] = nd; g_prev[m] = n;
            if (nh + 2 < g_heapcap) bl_heap_push(g_heap, &nh, nd, m);
        }
    }
    if (g_dist[dst] >= 1e299) {
        if (s_diag) {
            int reached = 0, bi = -1;
            double bd = 1e300;
            for (i = 0; i < g_nn; i++) {
                if (g_dist[i] < 1e299) {
                    const double dx = g_n[i].x - g_n[dst].x, dz = g_n[i].z - g_n[dst].z;
                    const double dd = sqrt(dx * dx + dz * dz);
                    reached++;
                    if (dd < bd) { bd = dd; bi = i; }
                }
            }
            TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP]   search from (%.0f,%.0f): %d node(s) reached, closest %.0f m from the target "
                      "(node lat %.0f, ri %d); target lat %.0f dist-to-route %.0f m; src lat %.0f",
                      g_n[src].x, g_n[src].z, reached, bd / BL_UPM,
                      bi >= 0 ? g_n[bi].lat : 0.0, bi >= 0 ? g_n[bi].ri : -1,
                      g_n[dst].lat, g_n[dst].dist / BL_UPM, g_n[src].lat);
            if (reached <= 1) {
                int k;
                for (k = g_n[src].head; k >= 0; k = g_e[k].next) {
                    const int m = g_e[k].to;
                    TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP]     src edge way %d to (%.0f,%.0f) len %.0f m lat %.0f ri %d reach %.0f ok=%d",
                              g_e[k].way, g_n[m].x, g_n[m].z, g_e[k].len / BL_UPM, g_n[m].lat, g_n[m].ri,
                              g_n[m].ri >= 0 ? tg_carriageway_reach(nl, g_n[m].ri, -1.0) : -1.0, bl_node_ok(nl, m, margin));
                }
            }
        }
        return 0;
    }
    {
        int tmp[256], nt = 0, q;
        for (q = dst; q >= 0 && nt < 256; q = g_prev[q]) tmp[nt++] = q;
        if (nt >= 256 || nt > cap) return 0;
        for (i = 0; i < nt; i++) out[i] = tmp[nt - 1 - i];
        *len_out = g_dist[dst];
        return nt;
    }
}

/* ------------------------------------------------------------ the planner */

static int bl_plan_cmp(const void *pa, const void *pb)
{
    const BlPlan *a = (const BlPlan *)pa, *b = (const BlPlan *)pb;
    /* shortest bypassed stretch first, then the more moderate ratio */
    const double sa = fabs(a->ratio - 3.5) + (double)a->ncorner * 0.2;
    const double sb = fabs(b->ratio - 3.5) + (double)b->ncorner * 0.2;
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return a->F - b->F;
}

static int bl_only_ok(int si)
{
    const char *e = getenv("TD5RE_GEO_BLOCKLOOP_ONLY");
    if (!e || !e[0]) return 1;
    while (*e) {
        if (*e >= '0' && *e <= '9') {
            const int v = atoi(e);
            if (si >= v - 6 && si <= v + 6) return 1;
            while (*e >= '0' && *e <= '9') e++;
        } else e++;
    }
    return 0;
}

/* Douglas-Peucker: drop the vertices that lie within `tol` of the chord between the kept ones. The
 * real grid has dual carriageways and kerb nodes a few metres apart at every junction; their
 * hops are not corners a car drives. First and last points are kept. In place; returns the count. */
static int bl_simplify(double *px, double *pz, int n, double tol)
{
    unsigned char keep[300];
    int stack[600], sp = 0, i, m = 0;
    if (n < 3 || n > 300) return n;
    memset(keep, 0, (size_t)n);
    keep[0] = keep[n - 1] = 1;
    stack[sp++] = 0; stack[sp++] = n - 1;
    while (sp >= 2) {
        const int b = stack[--sp], a = stack[--sp];
        const double dx = px[b] - px[a], dz = pz[b] - pz[a];
        const double l = sqrt(dx * dx + dz * dz);
        double dmax = 0.0;
        int im = -1;
        for (i = a + 1; i < b; i++) {
            double d;
            if (l < 1e-6) d = sqrt((px[i] - px[a]) * (px[i] - px[a]) + (pz[i] - pz[a]) * (pz[i] - pz[a]));
            else d = fabs((px[i] - px[a]) * dz - (pz[i] - pz[a]) * dx) / l;
            if (d > dmax) { dmax = d; im = i; }
        }
        if (im >= 0 && dmax > tol) {
            keep[im] = 1;
            stack[sp++] = a; stack[sp++] = im;
            stack[sp++] = im; stack[sp++] = b;
        }
    }
    for (i = 0; i < n; i++)
        if (keep[i]) { px[m] = px[i]; pz[m] = pz[i]; m++; }
    return m;
}

/* Build the chain for a candidate: polyline -> fillet -> resample. Fills cx/cz/cy (clen+1). */
static int bl_chain(const TG_NodeList *nl, BlPlan *P, const int *path, int np,
                    double rwant, double rfloor)
{
    double e1x, e1y, e1z, o1x, o1z, e2x, e2y, e2z, o2x, o2z;
    double px[300], pz[300], fx[2600], fz[2600];
    double *cum;
    int n = 0, i, nf, nc, k;
    const double lead = BL_LEAD_SP * (double)TD5_TG_SPAN_LENGTH;
    double total = 0.0, minr = 0.0, ys, ye;

    if (!bl_edge_at(nl, P->si1, &e1x, &e1y, &e1z, &o1x, &o1z) ||
        !bl_edge_at(nl, P->si2, &e2x, &e2y, &e2z, &o2x, &o2z)) return 0;
    px[n] = e1x; pz[n] = e1z; n++;
    px[n] = e1x + o1x * lead; pz[n] = e1z + o1z * lead; n++;
    for (i = 0; i < np && n < 290; i++) {
        const double x = g_n[path[i]].x, z = g_n[path[i]].z;
        const double u1 = (x - e1x) * o1x + (z - e1z) * o1z;      /* outward distance from mouth 1 */
        const double u2 = (x - e2x) * o2x + (z - e2z) * o2z;
        if (u1 < lead + rwant * 0.5 && i < np / 2 + 1) continue;
        if (u2 < lead + rwant * 0.5 && i >= np / 2) continue;
        if (n > 0 && (px[n - 1] - x) * (px[n - 1] - x) + (pz[n - 1] - z) * (pz[n - 1] - z) < 150.0 * 150.0) continue;
        px[n] = x; pz[n] = z; n++;
    }
    px[n] = e2x + o2x * lead; pz[n] = e2z + o2z * lead; n++;
    px[n] = e2x; pz[n] = e2z; n++;
    n = bl_simplify(px, pz, n, 6.0 * BL_UPM);
    s_why = "";
    nf = bl_fillet(px, pz, n, rwant, rfloor, fx, fz, 2600, &minr, &P->ncorner);
    if (nf < 4) {
        if (!s_why[0]) s_why = "no corridor";
        if (s_diag) {
            int q;
            for (q = 0; q < n; q++)
                TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP]     polyline %d: (%.0f,%.0f) %s", q, px[q], pz[q],
                          q ? "" : "mouth 1");
        }
        return 0;
    }
    cum = (double *)malloc(sizeof(double) * (size_t)nf);
    if (!cum) return 0;
    cum[0] = 0.0;
    for (i = 1; i < nf; i++) {
        const double dx = fx[i] - fx[i - 1], dz = fz[i] - fz[i - 1];
        cum[i] = cum[i - 1] + sqrt(dx * dx + dz * dz);
    }
    total = cum[nf - 1];
    nc = (int)(total / (double)TD5_TG_SPAN_LENGTH + 0.5);
    if (nc < 12) { free(cum); s_why = "under 12 spans long"; return 0; }
    if (nc > 700) { free(cum); s_why = "over 700 spans long"; return 0; }
    P->clen = nc;
    P->cx = (double *)calloc((size_t)nc + 2, sizeof(double));
    P->cz = (double *)calloc((size_t)nc + 2, sizeof(double));
    P->cy = (double *)calloc((size_t)nc + 2, sizeof(double));
    if (!P->cx || !P->cz || !P->cy) { free(cum); return 0; }
    k = 0;
    for (i = 0; i <= nc; i++) {
        const double want = total * (double)i / (double)nc;
        double u;
        while (k + 1 < nf - 1 && cum[k + 1] < want) k++;
        u = (cum[k + 1] - cum[k] > 1e-9) ? (want - cum[k]) / (cum[k + 1] - cum[k]) : 0.0;
        P->cx[i] = fx[k] + (fx[k + 1] - fx[k]) * u;
        P->cz[i] = fz[k] + (fz[k + 1] - fz[k]) * u;
    }
    P->cx[0] = e1x; P->cz[0] = e1z; P->cx[nc] = e2x; P->cz[nc] = e2z;
    free(cum);
    {   /* tightest corner of the RESAMPLED chain (circumradius of consecutive triples) */
        double mr = 1e300;
        for (i = 1; i < nc; i++) {
            const double ax = P->cx[i] - P->cx[i - 1], az = P->cz[i] - P->cz[i - 1];
            const double bx = P->cx[i + 1] - P->cx[i], bz = P->cz[i + 1] - P->cz[i];
            const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
            const double cr = fabs(ax * bz - az * bx);
            const double th = atan2(cr, ax * bx + az * bz);
            if (th > 1e-3) {
                const double rr = 0.5 * (la + lb) / th;
                if (rr < mr) mr = rr;
            }
        }
        P->min_r_m = mr / BL_UPM;
    }
    P->len_m = total / BL_UPM;
    /* heights: the terrain under the chain, offset so both ends meet the road edge */
    ys = e1y; ye = e2y;
    {
        const double ds = ys - tg_world_h(P->cx[0], P->cz[0]);
        const double de = ye - tg_world_h(P->cx[nc], P->cz[nc]);
        for (i = 0; i <= nc; i++) {
            const double u = (double)i / (double)nc;
            P->cy[i] = tg_world_h(P->cx[i], P->cz[i]) + ds + (de - ds) * u;
        }
        P->cy[0] = ys; P->cy[nc] = ye;
    }
    return 1;
}

static void bl_free_plan(BlPlan *P)
{
    free(P->cx); free(P->cy); free(P->cz);
    P->cx = P->cy = P->cz = NULL;
    free(P->rview.v);
    P->rview.v = NULL; P->rview.count = P->rview.cap = 0;
}

int tg_bl_build(const TG_NodeList *nl, int ring)
{
    TG_NetArm arms[96];
    int na = 0, ne, i, j, e;
    BlPlan cand[64];
    int ncand = 0, built = 0;
    const int max_loops = td5_env_int("TD5RE_GEO_BLOCKLOOP_MAX", 6, 0, BL_MAXPLAN);
    const double rwant = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RMIN_M", 10.0f, 4.0f, 40.0f) * BL_UPM;
    const double rfloor = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RFLOOR_M", 7.0f, 3.0f, 30.0f) * BL_UPM;
    const double flare = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_FLARE", 3000.0f, 0.0f, 9000.0f);
    const double lmax = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_LMAX_M", 420.0f, 60.0f, 1200.0f) * BL_UPM;
    const double rmin = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RATIO_MIN", 1.4f, 1.0f, 8.0f);
    const double rmax = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RATIO_MAX", 8.0f, 1.0f, 20.0f);
    const int finish = tg_realfork_finish_span();
    const double skew_max = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_SKEW_DEG", 40.0f, 5.0f, 85.0f) * kPi / 180.0;
    int off;

    s_diag = td5_env_flag_off("TD5RE_GEO_FORK_DIAG");
    s_clear = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_CLEAR_M", 25.0f, 5.0f, 80.0f) * BL_UPM;
    if (!td5_env_flag_on("TD5RE_GEO_FORK_BLOCKLOOP") || max_loops < 1) return 0;
    if (!td5_geo_loaded() || !nl || nl->count < 120) return 0;
    if (!td5_geo_roads_sync(td5_geo_place_slug())) return 0;
    if (!tg_net_geo()) return 0;
    if (s_fork_count >= TD5_TG_BRANCH_MAX) return 0;

    /* the accepted RIGHT arms of real streets */
    ne = tg_net_edge_count();
    for (e = 0; e < ne && na < 96; e++) {
        TG_NetArm a;
        if (!tg_net_arm_get(e, &a)) continue;
        if (a.left || a.loop) continue;
        if (fabs(a.skew) > skew_max) continue;      /* a street that leaves at a shallow angle is not a turn-in */
        arms[na++] = a;
    }
    if (na < 2) return 0;
    if (!bl_graph_build(nl, ring)) return 0;

    if (td5_env_flag_off("TD5RE_GEO_BLOCKLOOP_DUMP")) {     /* graph + route + arms for verify/geo_bl_plot.py */
        FILE *fp = fopen("log/bl_graph.csv", "w");
        if (fp) {
            int q, k2;
            fprintf(fp, "R,i,x,z,tx,tz,width,lanes\n");
            for (q = 0; q <= ring && q < nl->count; q++)
                fprintf(fp, "R,%d,%.0f,%.0f,%.4f,%.4f,%.0f,%d\n", q, nl->v[q].x, nl->v[q].z, nl->v[q].tx, nl->v[q].tz, nl->v[q].width, nl->v[q].lanes);
            for (q = 0; q < g_nn; q++)
                fprintf(fp, "N,%d,%.0f,%.0f,%.0f,%d,%.0f,%d\n", q, g_n[q].x, g_n[q].z, g_n[q].lat, g_n[q].ri, g_n[q].dist, bl_node_ok(nl, q, 4.0 * BL_UPM));
            for (q = 0; q < g_nn; q++)
                for (k2 = g_n[q].head; k2 >= 0; k2 = g_e[k2].next)
                    if (g_e[k2].to > q) fprintf(fp, "E,%d,%d,%d\n", q, g_e[k2].to, g_e[k2].way);
            for (q = 0; q < na; q++)
                fprintf(fp, "A,%d,%d,%d,%d,%d,%.3f,%.0f,%.0f,%.0f,%.0f,%d\n", arms[q].edge, arms[q].si, arms[q].lo, arms[q].hi, arms[q].road, arms[q].skew,
                        arms[q].ax, arms[q].az, arms[q].bx, arms[q].bz, bl_arm_node(nl, &arms[q]));
            fclose(fp);
        }
    }
    for (i = 0; i < na && ncand < 60; i++) {
        for (j = 0; j < na && ncand < 60; j++) {
            const TG_NetArm *A = &arms[i], *B = &arms[j];
            const TD5_GeoRoad *ra, *rb;
            double lenp = 0.0, main_m = 0.0;
            int sn, dn, np, q, lo1, hi1, lo2, hi2, ok = 1;
            BlPlan *P;
            int path[256];
            char what[48];
            if (B->si - A->si < 10 || B->si - A->si > 220) continue;
            if (!bl_only_ok(A->si)) continue;
            snprintf(what, sizeof what, "arms %d/%d", A->edge, B->edge);
            ra = td5_geo_roads_get(A->road); rb = td5_geo_roads_get(B->road);
            if (!ra || !rb) continue;
            sn = bl_arm_node(nl, A); dn = bl_arm_node(nl, B);
            if (sn < 0 || dn < 0 || sn == dn) { bl_note(what, A->si, B->si, "an arm's way has no usable vertex right of the road"); continue; }
            np = bl_path(nl, sn, dn, lmax, path, 256, &lenp);
            if (np < 2) { bl_note(what, A->si, B->si, "no path over the real grid that stays right of the road"); continue; }
            for (q = A->si; q < B->si && q + 1 < nl->count; q++) {
                const double dx = nl->v[q + 1].x - nl->v[q].x, dz = nl->v[q + 1].z - nl->v[q].z;
                main_m += sqrt(dx * dx + dz * dz);
            }
            main_m /= BL_UPM;
            if (main_m < 20.0) continue;
            P = &cand[ncand];
            memset(P, 0, sizeof *P);
            P->e1 = A->edge; P->e2 = B->edge; P->si1 = A->si; P->si2 = B->si;
            P->lb = ra->lanes > rb->lanes ? ra->lanes : rb->lanes;
            for (q = 0; q + 1 < np; q++) {
                int k;
                for (k = g_n[path[q]].head; k >= 0; k = g_e[k].next)
                    if (g_e[k].to == path[q + 1]) {
                        const TD5_GeoRoad *rw = td5_geo_roads_get(g_e[k].way);
                        if (rw && rw->lanes > P->lb) P->lb = rw->lanes;
                        break;
                    }
            }
            if (P->lb < 2) P->lb = 2;
            if (P->lb > 3) P->lb = 3;
            P->flare = flare;
            P->npath = np;
            P->main_m = main_m;
            if (!bl_chain(nl, P, path, np, rwant, rfloor)) {
                bl_note(what, A->si, B->si, s_why);
                bl_free_plan(P);
                continue;
            }
            P->ratio = P->len_m / main_m;
            if (P->ratio < rmin || P->ratio > rmax) {
                char w[96];
                snprintf(w, sizeof w, "corridor %.0f m over %.0f m of road (%.2fx)", P->len_m, main_m, P->ratio);
                bl_note(what, A->si, B->si, w);
                bl_free_plan(P);
                continue;
            }
            {
                const double half_row = ((double)P->lb * bl_lw() + 2.0 * flare) * 0.5;
                bl_mouth_range(nl, A->si, half_row, &lo1, &hi1);
                bl_mouth_range(nl, B->si, half_row, &lo2, &hi2);
            }
            P->a1_lo = lo1; P->a1_hi = hi1; P->a2_lo = lo2; P->a2_hi = hi2;
            P->F = lo1; P->R = hi2; P->len = P->R - P->F - 1;
            if (P->F < TD5_TG_GRID_SPAN + 8) { bl_note(what, A->si, B->si, "inside the start grid"); ok = 0; }
            if (ok && (P->R + 12 >= ring - 24 || (finish > 0 && P->R + 10 >= finish))) { bl_note(what, A->si, B->si, "past the finish line or inside the ring tail"); ok = 0; }
            if (ok && P->len < 6) { bl_note(what, A->si, B->si, "the two mouths overlap"); ok = 0; }
            if (ok && (!bl_lanes_uniform(nl, P->F - 2, P->a1_hi + 2) || !bl_lanes_uniform(nl, P->a2_lo - 2, P->R + 2))) {
                bl_note(what, A->si, B->si, "the route's lane count changes at a mouth"); ok = 0; }
            if (ok) {
                int s;
                for (s = P->F - 2; s <= P->R + 2 && ok; s++)
                    if (tg_span_in_fork_clear(s) || tg_span_in_bridge_run(s) || tg_span_in_tunnel(s)) ok = 0;
                if (!ok) bl_note(what, A->si, B->si, "a fork, bridge or tunnel is in the bypassed stretch");
            }
            if (ok) {   /* the corridor stays clear of every other street and of every carriageway but its mouths */
                int k;
                for (k = 6; k <= P->clen - 6 && ok; k += 2) {
                    const double hw = ((double)P->lb * bl_lw()) * 0.5;
                    if (tg_net_near_other_street(P->cx[k], P->cz[k], hw, P->e1, P->e2)) { ok = 0; bl_note(what, A->si, B->si, "crosses another drawn street"); }
                    else if (k > 16 && k < P->clen - 16 &&
                             tg_world_occ_near(P->cx[k], P->cz[k], hw + 600.0, TG_WO_DRIVABLE | TG_WO_ROAD)) { ok = 0; bl_note(what, A->si, B->si, "runs over a carriageway or another corridor"); }
                    else if (tg_world_is_water(P->cx[k], P->cz[k])) { ok = 0; bl_note(what, A->si, B->si, "runs into water"); }
                }
            }
            if (!ok) { bl_free_plan(P); continue; }
            snprintf(P->name, sizeof P->name, "%s", what);
            P->used = 1;
            ncand++;
        }
    }
    bl_graph_free();
    TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] %d right arm(s), %d candidate loop(s)", na, ncand);
    if (ncand < 1) return 0;
    qsort(cand, (size_t)ncand, sizeof cand[0], bl_plan_cmp);

    off = ring;
    for (i = 0; i < s_fork_count; i++) off += 1 + tg_fork_clen(i);

    for (i = 0; i < ncand && s_nbl < max_loops && s_fork_count < TD5_TG_BRANCH_MAX; i++) {
        BlPlan *C = &cand[i];
        int clash = 0, q, k;
        for (q = 0; q < s_nbl; q++) {   /* windows may not overlap, and arms are used once */
            const BlPlan *O = &s_bl[q];
            if (C->F - 3 <= O->R + 3 && O->F - 3 <= C->R + 3) clash = 1;
            if (C->e1 == O->e1 || C->e1 == O->e2 || C->e2 == O->e1 || C->e2 == O->e2) clash = 1;
        }
        if (clash) { bl_free_plan(C); continue; }
        if (off + 1 + C->clen >= TD5_TG_MAX_SPANS - 8) {
            bl_note(C->name, C->si1, C->si2, "the track would exceed the span budget");
            bl_free_plan(C);
            continue;
        }
        /* take the plan */
        s_bl[s_nbl] = *C;
        memset(C, 0, sizeof *C);
        {
            BlPlan *P = &s_bl[s_nbl];
            const int F = P->F;
            TG_Fork *fk = &s_forks[s_fork_count];
            const int rcount = (nl->count > F + P->clen + 4) ? nl->count : F + P->clen + 4;
            P->rview.v = (TG_Node *)calloc((size_t)rcount, sizeof(TG_Node));
            if (!P->rview.v) { bl_free_plan(P); memset(P, 0, sizeof *P); continue; }
            P->rview.count = rcount; P->rview.cap = rcount;
            for (k = 0; k <= P->clen; k++) {
                TG_Node *nn = &P->rview.v[F + 1 + k];
                const int ka = k > 0 ? k - 1 : 0, kb = k < P->clen ? k + 1 : P->clen;
                double tx = P->cx[kb] - P->cx[ka], tz = P->cz[kb] - P->cz[ka], l, fl = 0.0;
                const int edge_rows = td5_env_int("TD5RE_GEO_BLOCKLOOP_FLARE_ROWS", 5, 1, 12);
                l = sqrt(tx * tx + tz * tz);
                if (l < 1e-9) { tx = 0.0; tz = 1.0; } else { tx /= l; tz /= l; }
                if (k < edge_rows) fl = P->flare * (1.0 - (double)k / (double)edge_rows);
                else if (k > P->clen - edge_rows) fl = P->flare * (1.0 - (double)(P->clen - k) / (double)edge_rows);
                memset(nn, 0, sizeof *nn);
                nn->x = P->cx[k]; nn->y = P->cy[k]; nn->z = P->cz[k];
                nn->tx = tx; nn->tz = tz;
                nn->width = (double)P->lb * bl_lw() + 2.0 * fl;
                nn->lanes = P->lb;
                nn->lane_base = TD5_TG_HEIGHT_NIBBLE;
            }
            /* the end rows are perpendicular to the road: the mouth's near edge lies ON the edge */
            memset(fk, 0, sizeof *fk);
            fk->F = P->F; fk->len = P->len; fk->R = P->R; fk->cbase = off + 1;
            fk->sep = 0.5; fk->lanes = nl->v[P->F].lanes; fk->kind = TG_FORK_LOOP;
            fk->main_lanes = nl->v[P->F].lanes; fk->br_lanes = P->lb;
            fk->fm = 1.0; fk->fb = 1.0; fk->side = -1;
            fk->loopc = s_nbl + 1; fk->clen = P->clen;
            P->fi = s_fork_count;
            P->built = 1;
            off += 1 + P->clen;
            s_fork_count++;
            built++;
            tg_net_loop_take(P->e1, P->a1_lo, P->a1_hi);
            tg_net_loop_take(P->e2, P->a2_lo, P->a2_hi);
            tg_bl_paint_network(P->fi);
            TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] %d: fork %d F=%d R=%d (mouths %d..%d / %d..%d) arms %d/%d lanes %d, "
                      "corridor %d spans %.0f m over %.0f m of road (%.2fx), %d corner(s), tightest %.1f m, %d graph nodes",
                      s_nbl, P->fi, P->F, P->R, P->a1_lo, P->a1_hi, P->a2_lo, P->a2_hi, P->e1, P->e2, P->lb,
                      P->clen, P->len_m, P->main_m, P->ratio, P->ncorner, P->min_r_m, P->npath);
            s_nbl++;
        }
    }
    for (i = 0; i < ncand; i++) bl_free_plan(&cand[i]);
    return built;
}

/* ------------------------------------------------------------ accessors */

static BlPlan *bl_of(int fi)
{
    if (fi < 0 || fi >= s_fork_count) return NULL;
    if (s_forks[fi].loopc <= 0 || s_forks[fi].loopc > s_nbl) return NULL;
    return &s_bl[s_forks[fi].loopc - 1];
}

int tg_fork_is_loop(int fi)
{
    return fi >= 0 && fi < s_fork_count && s_forks[fi].loopc > 0;
}

const TG_NodeList *tg_bl_rview(int fi)
{
    BlPlan *P = bl_of(fi);
    return (P && P->built && P->rview.v) ? &P->rview : NULL;
}

const TG_NodeList *tg_fork_rview(int fi)
{
    const TG_NodeList *v = tg_bl_rview(fi);
    return v ? v : tg_pf_rview(fi);
}

int tg_bl_row_node(int fi, int k)
{
    const BlPlan *P = bl_of(fi);
    int o;
    if (!P || P->clen < 1 || P->len < 1) return -1;
    if (k < 0) k = 0;
    if (k >= P->clen) k = P->clen - 1;
    o = (int)(((long)k * (long)P->len) / (long)P->clen);
    if (o > P->len - 1) o = P->len - 1;
    return P->F + 1 + o;
}

int tg_bl_mouths(int fi, int *a1_lo, int *a1_hi, int *a2_lo, int *a2_hi)
{
    const BlPlan *P = bl_of(fi);
    if (!P || !P->built) return 0;
    if (a1_lo) *a1_lo = P->a1_lo;
    if (a1_hi) *a1_hi = P->a1_hi;
    if (a2_lo) *a2_lo = P->a2_lo;
    if (a2_hi) *a2_hi = P->a2_hi;
    return 1;
}

double tg_bl_reach(const TG_NodeList *nl, int si, double side)
{
    (void)nl; (void)si; (void)side;
    return 0.0;
}

int tg_bl_mouth_span(int si)
{
    int p;
    for (p = 0; p < s_nbl; p++) {
        const BlPlan *P = &s_bl[p];
        if (!P->built) continue;
        if ((si >= P->a1_lo - 1 && si <= P->a1_hi + 1) || (si >= P->a2_lo - 1 && si <= P->a2_hi + 1)) return 1;
    }
    return 0;
}

int tg_bl_corridor_near(double x, double z, double half)
{
    int p, k;
    for (p = 0; p < s_nbl; p++) {
        const BlPlan *P = &s_bl[p];
        if (!P->built || !P->cx) continue;
        for (k = 0; k <= P->clen; k++) {
            const double dx = P->cx[k] - x, dz = P->cz[k] - z;
            if (dx * dx + dz * dz < half * half) return 1;
        }
    }
    return 0;
}

/* Paint the corridor into the world raster and bed the ground under it. */
void tg_bl_paint_network(int fi)
{
    const BlPlan *P = bl_of(fi);
    int k;
    if (!P || !P->built) return;
    for (k = 0; k <= P->clen; k++) {
        const TG_Node *c = &P->rview.v[P->F + 1 + k];
        tg_world_occ_disc(c->x, c->z, c->width * 0.5 + 600.0, TG_WO_DRIVABLE);
        if (k < P->clen) {
            const TG_Node *d = &P->rview.v[P->F + 2 + k];
            tg_world_conform_seg(c->x, c->z, c->y, d->x, d->z, d->y,
                                 c->width * 0.5 + TD5_TG_ROAD_BED_VERGE, 3500.0);
        }
    }
}

/* The SLNK trailer (see td5_track.c, "SIDE LINKS"): 'SLNK', n, then per loop
 * D, A, c0, cN, a1_lo, a1_hi, a2_lo, a2_hi, side. Returns the loops written; nothing at all
 * (no bytes) when there are none, so every other strip is byte-identical. */
int tg_bl_slnk_write(TG_Buf *out)
{
    int p, n = 0;
    for (p = 0; p < s_nbl; p++) if (s_bl[p].built && s_forks[s_bl[p].fi].loopc == p + 1) n++;
    if (n == 0) return 0;
    tg_put_u32(out, 0x4B4E4C53u);
    tg_put_u32(out, (unsigned int)n);
    for (p = 0; p < s_nbl; p++) {
        const BlPlan *P = &s_bl[p];
        if (!P->built || s_forks[P->fi].loopc != p + 1) continue;
        tg_put_u32(out, (unsigned int)P->F);
        tg_put_u32(out, (unsigned int)P->R);
        tg_put_u32(out, (unsigned int)s_forks[P->fi].cbase);
        tg_put_u32(out, (unsigned int)(s_forks[P->fi].cbase + P->clen - 1));
        tg_put_u32(out, (unsigned int)P->a1_lo);
        tg_put_u32(out, (unsigned int)P->a1_hi);
        tg_put_u32(out, (unsigned int)P->a2_lo);
        tg_put_u32(out, (unsigned int)P->a2_hi);
        tg_put_u32(out, 0u);
    }
    return n;
}
