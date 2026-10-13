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
 * the main road is not touched at all. The corridor starts as a SLIP LANE: its first spans run
 * flush against the road's right edge (its left rail IS the road edge, 14 spans on La Plata),
 * then arc away (R = 22 m) into the cross street, and it comes back the same way, merging
 * flush at the other end. A perpendicular first span (the first design) put cars into the
 * wall at 400+ units/tick: a car cannot turn 90 degrees inside its own width. The engine
 * learns where the road is open from the SLNK trailer (td5_track.c, "SIDE LINKS"): the walker
 * relocates a probe that crosses the open rail into the corridor span that contains it, the
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
 * TD5_TG_SPAN_LENGTH, with the slip lanes added at both ends (bl_slip).
 *
 * KNOBS (all default ON / sane, =0 restores master exactly):
 *   TD5RE_GEO_FORK_BLOCKLOOP        master switch for the generator
 *   TD5RE_GEO_BLOCKLOOP_MAX         most loops on a route (default 6)
 *   TD5RE_GEO_BLOCKLOOP_ONLY        "<span>[,<span>...]": only loops whose departure mouth is
 *                                    within 6 spans of one of these (harness / hand-picked)
 *   TD5RE_GEO_BLOCKLOOP_RMIN_M      corner radius wanted, metres (default 22: measured on the AI, 10 m and 18 m hit walls)
 *   TD5RE_GEO_BLOCKLOOP_RFLOOR_M    tightest corner a loop may keep, metres (default 7)
 *   TD5RE_GEO_BLOCKLOOP_FLARE       extra width per side at the corridor's two ends, world units (default 0)
 *   TD5RE_GEO_BLOCKLOOP_FLARE_ROWS  rows (spans) it tapers over (default 5)
 *   TD5RE_GEO_BLOCKLOOP_APPROACH_SPANS/_DEG  no more than this much route bend (default 20 degrees) in the spans before the departure (14)
 *   TD5RE_GEO_BLOCKLOOP_LIFT        the streets between the slip lanes stand this far above the ground envelope (default 60)
 *   TD5RE_GEO_BLOCKLOOP_INNER_PAVE  pavement on the corridor's inner edge too, where clear of the road (default 1)
 *   TD5RE_GEO_BLOCKLOOP_LANES       corridor lanes (default 2; 0 = from the real ways)
 *   TD5RE_GEO_BLOCKLOOP_STRAIGHT_DEG  heading change allowed along a slip lane's road edge (default 5)
 *   TD5RE_GEO_BLOCKLOOP_LEAD_SPANS  straight lane before a corner's arc, spans beyond twice its tangent (default 6)
 *   TD5RE_GEO_BLOCKLOOP_LMAX_M      longest loop, metres (default 420)
 *   TD5RE_GEO_BLOCKLOOP_RATIO_MIN/MAX  corridor length over the bypassed main length (1.4 .. 8)
 *   TD5RE_GEO_FORK_DIAG=1           why a candidate was refused
 *   [1019 V] LEFT-hand loops: TD5RE_GEO_BLOCKLOOP_LEFT (default 1; 0 = master exactly) lets the finder take the network's
 *   LEFT arms. A loop is built on ONE side (s_sd: -1 right, +1 left): the slip lane runs flush with that edge, the pavement
 *   on the inner edge is on the side facing the road, TG_Fork.side and the SLNK side word carry it. ONLY accepts "L620" / "R620"
 *   as well as "620". The one-way flow of the path is logged per candidate; TD5RE_GEO_BLOCKLOOP_LEFT_ONEWAY=1 refuses a left
 *   loop that runs against a one-way street (default off: right loops ignore the flow too).
 *
 *   [1029 X] TD5RE_GEO_LOOP_SHARED_MOUTH (default 1; 0 = round 1028): a left and a right loop may open on the SAME main
 *   spans (the two arms of one cross street); the engine keeps two side-link entries per span, the AI decides the
 *   pair as one three-way choice. TD5RE_GEO_LOOP_FORK_RAMP (default 1; 0 = round 1028): a departure mouth may stand
 *   in a real fork's widened approach (other side of the road), decided together with the fork
 *   (TD5RE_AI_LOOP_FORK_PAIR, spans, default 12; 0 = two separate decisions).
 *
 * BYTE-IDENTICAL SYNTHETIC BUILDS. Every entry point returns "nothing" with no geo place
 * loaded, and no tg_rand / tg_frand / tg_range is called here.
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_fork_verdicts.h"   /* measured per-route fork verdicts (round 1017 R) */
#include "td5_dev_forkfilter.h"      /* the harness's per-fork allow / deny list, by F */

#define BL_UPM        430.0          /* world units per metre (the place cache's scale) */
#define BL_MAXPLAN    16
#define BL_HSIZE      32768
#define BL_JOIN_D2    (80.0 * 80.0)  /* two way vertices closer than this are one node  */

typedef struct { double x, z; int head, hnext; int ri; double lat, dist; } BlNode;
typedef struct { int to, way, next; float len; int ow; } BlEdge;   /* [1019 V] ow: +1 the way's one-way flow runs WITH this edge's direction, -1 AGAINST it, 0 two-way / untagged */

typedef struct {
    int    used, built, committed;
    int    fi;
    int    sd;                       /* [1019 V] -1 = the loop leaves through the road's RIGHT edge, +1 = the LEFT edge */
    int    e1, e2;                   /* the two network edges (real street arms)        */
    int    si1, si2;                 /* mouth centre spans                               */
    int    a1_lo, a1_hi, a2_lo, a2_hi;
    int    fl1, fl2;                 /* corridor spans flush with the road at the departure / arrival */
    int    nabs, absorb[8];          /* drawn street stubs the corridor runs over: taken over by the loop */
    int    F, R, len, clen, lb;
    double flare;
    double len_m, main_m, ratio, min_r_m;
    int    ncorner, npath;
    int    beside_n;                 /* [1022 N2] bypassed-stretch spans that lie in a real fork on the other side */
    int    sh[2];                    /* [1029 X] the departure / arrival mouth shares main spans with another loop's mouth */
    int    npend, pend_q[4], pend_m[4];   /* [1029 X] the taken loops' mouths this candidate would share (marked when it is taken) */
    double *cx, *cz, *cy;            /* clen+1 chain points                              */
    unsigned char *inner;            /* [clen+1] the corridor's inner edge is clear of the road: a pavement may lie there */
    TG_NodeList rview;               /* v[F+1+k] = corridor row k                        */
    char   name[96];
} BlPlan;

/* The ground the viewer sees at (x,z): the heightfield's envelope over half a cell round it. The ground meshes are
 * triangulated on a lattice, so the exact height at an off-lattice point can sit BELOW the surface drawn there;
 * a street laid at tg_world_h alone ran under the ground in patches (round 1018 L, frames of the 720..788 loop). */
static double bl_env_h(double x, double z)
{
    const double hh = TG_WORLD_CELL * 0.5;
    double g = tg_world_h(x, z), g1;
    g1 = tg_world_h(x + hh, z); if (g1 > g) g = g1;
    g1 = tg_world_h(x - hh, z); if (g1 > g) g = g1;
    g1 = tg_world_h(x, z + hh); if (g1 > g) g = g1;
    g1 = tg_world_h(x, z - hh); if (g1 > g) g = g1;
    return g;
}

static BlPlan  s_bl[BL_MAXPLAN];
static int     s_nbl;
static int     s_diag;
static const char *s_why = "";
static double  s_clear;              /* least distance of a corridor node from the route's centre line */
static int     s_ow_against, s_ow_with;   /* [1019 V] one-way edges of the candidate's path, for the diag line */
static int     s_sd = -1;            /* [1019 V] the side being searched / built: -1 right, +1 left (read by bl_edge_at, bl_node_ok, bl_slip, bl_flush) */

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
        free(s_bl[i].cx); free(s_bl[i].cy); free(s_bl[i].cz); free(s_bl[i].inner);
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
    const TD5_GeoRoad *rw = td5_geo_roads_get(way);
    const int dir = (rw && rw->oneway && rw->oneway_dir != 0) ? (rw->oneway_dir > 0 ? 1 : -1) : 0;   /* the way's own vertex order a -> b */
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
    g_e[g_ne].to = b; g_e[g_ne].way = way; g_e[g_ne].len = len; g_e[g_ne].ow = dir; g_e[g_ne].next = g_n[a].head;
    g_n[a].head = g_ne++;
    g_e[g_ne].to = a; g_e[g_ne].way = way; g_e[g_ne].len = len; g_e[g_ne].ow = -dir; g_e[g_ne].next = g_n[b].head;
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
    if (g->ri < 0 || !(s_sd < 0 ? g->lat < 0.0 : g->lat > 0.0)) return 0;
    reach = tg_carriageway_reach(nl, g->ri, (double)s_sd);
    if (reach + margin < s_clear) reach = s_clear - margin;   /* nothing hugs the road: its other carriageway, a service lane */
    return (s_sd < 0 ? -g->lat : g->lat) > reach + margin;
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
    tg_city_edge_frame(nl, si, (double)s_sd, e);
    *ex = e[0]; *ey = e[1]; *ez = e[2]; *ox = e[6]; *oz = e[7];
    return 1;
}

static int bl_lanes_uniform(const TG_NodeList *nl, int lo, int hi)
{
    int s;
    if (lo < 0 || hi + 1 >= nl->count) return 0;
    for (s = lo; s <= hi; s++)
        if (nl->v[s].lanes != nl->v[lo].lanes) return 0;
    return 1;
}

/* [ROUND 1022 N2] A loop beside a REAL fork. Span `s` of the loop's bypassed stretch lies in a real fork's cleared region (its widened
 * approach through its rejoin). The loop may share the stretch when the fork's corridor is on the OTHER side of the road from the loop's:
 * the main half keeps its free outer edge (the fork builder puts "main's left edge and branch's right edge on the road's edges"), so
 * nothing of the loop's street meets the corridor. A MOUTH (departure / arrival, plus the slip lane's flush run and the uniform-lanes
 * margin) must stand either on ordinary road, or -- TD5RE_GEO_LOOP_FORK_NWAY, the several-ways-off-one-window case -- inside the
 * fork's CORE (F+1+pad .. R-1-pad: past the widened approach and short of the rejoin ramp), where the node's edge on the loop's
 * side IS the main half's edge. 1 = the span blocks the loop. */
static const TG_Fork *bl_fork_covering(int s)
{
    int i;
    for (i = 0; i < s_fork_count; i++) {
        const TG_Fork *f = &s_forks[i];
        if (f->loopc > 0) continue;
        if (f->freec > 0) { if (tg_pf_clear_span(i, s)) return f; continue; }
        if (s >= f->F - TD5_TG_BRANCH_WIDEN - 2 && s <= f->R + 2) return f;
    }
    return NULL;
}

static int bl_fork_blocks(const BlPlan *P, int s)
{
    const int m = td5_env_int("TD5RE_GEO_LOOP_FORK_MOUTH_PAD", 4, 0, 30);
    const TG_Fork *f;
    if ((tg_fork_side_at(s) > 0 ? 1 : -1) == (P->sd > 0 ? 1 : -1)) return 1;      /* the fork's corridor lies on the loop's side */
    if ((s >= P->a1_lo - m && s <= P->a1_hi + m) || (s >= P->a2_lo - m && s <= P->a2_hi + m)) {
        const int pad = td5_env_int("TD5RE_GEO_LOOP_FORK_CORE_PAD", 3, -10, 20);
        f = bl_fork_covering(s);
        if (!td5_env_flag_on("TD5RE_GEO_LOOP_FORK_NWAY") || !f || f->freec > 0) return 1;
        /* [ROUND 1029 X] TD5RE_GEO_LOOP_FORK_RAMP (default on): a DEPARTURE mouth may stand in the fork's widened
         * APPROACH (before the core), because the AI then decides the loop and the fork as one three-way choice
         * (td5_track.c sl_pair_forks, TD5RE_AI_LOOP_FORK_PAIR); before, the two decisions came 2..4 spans apart and
         * the round-1022 N2 trial of this placement WARNed. An arrival mouth, or anything in the rejoin ramp, keeps
         * the core rule (not measured). =0 restores the round-1022 rule. */
        if (td5_env_flag_on("TD5RE_GEO_LOOP_FORK_RAMP") && td5_env_int("TD5RE_AI_LOOP_FORK_PAIR", 12, 0, 40) > 0 &&
            td5_env_flag_on("TD5RE_GEO_LOOP_SHARED_MOUTH") &&
            s >= P->a1_lo - m && s <= P->a1_hi + m && s < f->F + 1 + pad && P->a1_lo >= f->F - TD5_TG_BRANCH_WIDEN - 2)
            return 0;
        if (s < f->F + 1 + pad || s > f->R - 1 - pad) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ candidates */

/* The graph node an arm's path starts from: leaving the arm's first way vertex (the junction's, on the
 * road) outward over the graph -- through any way, because a real street often changes ways at the
 * junction -- the first vertex that is right of the road and clear of every carriageway and that lies in
 * the arm's direction. Depth-limited breadth-first, moving away from the road only. */
static int bl_arm_node(const TG_NodeList *nl, const TG_NetArm *A)
{
    const TD5_GeoRoad *r = td5_geo_roads_get(A->road);
    int q[256], depth[256], head = 0, tail = 0, k, n0;
    double x0, z0, dx, dz, dl;
    if (!r || !td5_geo_roads_point(r, A->k0, &x0, &z0)) return -1;
    n0 = bl_node_find(x0, z0);
    if (n0 < 0) return -1;
    dx = A->bx - A->ax; dz = A->bz - A->az;
    dl = sqrt(dx * dx + dz * dz);
    if (dl < 1.0) return -1;
    dx /= dl; dz /= dl;
    q[tail] = n0; depth[tail] = 0; tail++;
    while (head < tail) {
        const int n = q[head], d = depth[head];
        head++;
        if (bl_node_ok(nl, n, 4.0 * BL_UPM)) {
            const double ux = g_n[n].x - A->ax, uz = g_n[n].z - A->az;
            const double ul = sqrt(ux * ux + uz * uz);
            if (ul > 1.0 && (ux * dx + uz * dz) / ul > 0.7) return n;
        }
        if (d >= 4) continue;
        for (k = g_n[n].head; k >= 0; k = g_e[k].next) {
            const int m = g_e[k].to;
            int i, seen = 0;
            if (g_n[m].dist + 200.0 < g_n[n].dist) continue;          /* never back toward the road */
            for (i = 0; i < tail; i++) if (q[i] == m) { seen = 1; break; }
            if (!seen && tail < 256) { q[tail] = m; depth[tail] = d + 1; tail++; }
        }
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

static int bl_only_ok(int si, int left)
{
    const char *e = getenv("TD5RE_GEO_BLOCKLOOP_ONLY");
    int side = 0;                                   /* [1019 V] "L620" = a LEFT loop only, "R620" = a right one only, "620" = either */
    if (!e || !e[0]) return 1;
    while (*e) {
        if (*e == 'L' || *e == 'l') { side = 1; e++; continue; }
        if (*e == 'R' || *e == 'r') { side = -1; e++; continue; }
        if (*e >= '0' && *e <= '9') {
            const int v = atoi(e);
            if (si >= v - 6 && si <= v + 6 && (side == 0 || (side > 0) == (left != 0))) return 1;
            while (*e >= '0' && *e <= '9') e++;
            side = 0;
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

/* One SLIP LEG: the lane that runs flush against the road's right edge before it arcs into the street
 * (departure) or after it has arced out of it (arrival). Its centre line is the road's right edge pushed
 * out by half the lane's width, so the corridor's own left edge IS the road edge: a car moves onto it
 * across the open rail at a shallow angle, and the 90 degree turn happens inside the corridor, on a real
 * radius, instead of at the edge. */
typedef struct {
    double cx, cz;               /* the corner vertex: street axis x slip centre line */
    double tf;                   /* tangent length of its fillet                      */
    double qx[96], qz[96], qy[96];
    int    nq, j0, j1;           /* points, and the main nodes the first / last one stands beside */
} BlSlip;

static int bl_slip(const TG_NodeList *nl, int si, const TG_NetArm *A, double W, double R, int arrival, BlSlip *S)
{
    double ex, ey, ez, ox, oz, dx, dz, dl, a0x = A->ax, a0z = A->az, s_par, th, ct, upb, lead, rx, rz;
    const double tx = nl->v[si].tx, tz = nl->v[si].tz;
    int j;
    memset(S, 0, sizeof *S);
    if (!bl_edge_at(nl, si, &ex, &ey, &ez, &ox, &oz)) return 0;
    if (s_sd < 0) { rx = -tz; rz = tx; }                 /* the route's right of travel */
    else          { rx = tz;  rz = -tx; }                /* [1019 V] ... or its left */
    if (ox * rx + oz * rz < 0.9) { s_why = "the mouth's outward normal is not the right of travel"; return 0; }
    dx = A->bx - A->ax; dz = A->bz - A->az;
    dl = sqrt(dx * dx + dz * dz);
    if (dl < 1.0) { s_why = "an arm with no length"; return 0; }
    dx /= dl; dz /= dl;
    if (dx * ox + dz * oz < 0.6) { s_why = "the street does not leave the road outward"; return 0; }
    s_par = (W * 0.5 - ((a0x - ex) * ox + (a0z - ez) * oz)) / (dx * ox + dz * oz);
    S->cx = a0x + dx * s_par; S->cz = a0z + dz * s_par;
    ct = tx * (arrival ? -dx : dx) + tz * (arrival ? -dz : dz);
    if (ct > 1.0) ct = 1.0;
    if (ct < -1.0) ct = -1.0;
    th = acos(ct);                                       /* the turn between the slip lane and the street */
    if (th < 0.7 || th > kPi - 0.4) { s_why = "the street meets the road at too shallow an angle"; return 0; }
    S->tf = R * tan(th * 0.5);
    lead = 2.0 * S->tf + (double)td5_env_int("TD5RE_GEO_BLOCKLOOP_LEAD_SPANS", 6, 2, 20) * (double)TD5_TG_SPAN_LENGTH;  /* the lane before the corner is at least twice the fillet's tangent: the arc takes half of it */
    upb = (S->cx - ex) * tx + (S->cz - ez) * tz;
    for (j = si - 70; j <= si + 70; j++) {
        double jx, jy, jz, jox, joz, u;
        if (j < 1 || j + 2 >= nl->count || S->nq >= 96) continue;
        if (!bl_edge_at(nl, j, &jx, &jy, &jz, &jox, &joz)) continue;
        u = (jx - ex) * tx + (jz - ez) * tz;
        if (arrival ? (u >= upb + 2.0 * S->tf && u <= upb + lead) : (u >= upb - lead && u <= upb - 2.0 * S->tf)) {
            if (S->nq == 0) S->j0 = j;
            S->j1 = j;
            S->qx[S->nq] = jx + jox * W * 0.5; S->qz[S->nq] = jz + joz * W * 0.5; S->qy[S->nq] = jy;
            S->nq++;
        }
    }
    if (S->nq < 3) { s_why = "the road edge beside the corner is not sampled"; return 0; }
    return 1;
}

/* Build the chain for a candidate: slip lane -> corner -> real streets -> corner -> slip lane, then
 * fillet, resample. Fills cx/cz/cy (clen+1). */
static int bl_chain(const TG_NodeList *nl, BlPlan *P, const TG_NetArm *A, const TG_NetArm *B,
                    const int *path, int np, double rwant, double rfloor)
{
    BlSlip S1, S2;
    const double W = (double)P->lb * bl_lw();
    double px[400], pz[400], fx[2600], fz[2600], sx[300], sz[300];
    double *cum;
    int n = 0, i, nf, nc, k, ns = 0, k_s1 = 0, k_s2 = 0;
    double total = 0.0, minr = 0.0;

    if (!bl_slip(nl, P->si1, A, W, rwant, 0, &S1)) return 0;
    if (!bl_slip(nl, P->si2, B, W, rwant, 1, &S2)) return 0;
    /* the street part: corner 1, the real way's vertices, corner 2 (simplified: a real junction has dual
     * carriageways and kerb nodes a few metres apart, which are not corners a car drives) */
    sx[ns] = S1.cx; sz[ns] = S1.cz; ns++;
    for (i = 0; i < np && ns < 290; i++) {
        const double x = g_n[path[i]].x, z = g_n[path[i]].z;
        const double d1 = sqrt((x - S1.cx) * (x - S1.cx) + (z - S1.cz) * (z - S1.cz));
        const double d2 = sqrt((x - S2.cx) * (x - S2.cx) + (z - S2.cz) * (z - S2.cz));
        if (d1 < S1.tf + 1500.0 || d2 < S2.tf + 1500.0) continue;
        if ((sx[ns - 1] - x) * (sx[ns - 1] - x) + (sz[ns - 1] - z) * (sz[ns - 1] - z) < 150.0 * 150.0) continue;
        sx[ns] = x; sz[ns] = z; ns++;
    }
    sx[ns] = S2.cx; sz[ns] = S2.cz; ns++;
    ns = bl_simplify(sx, sz, ns, 6.0 * BL_UPM);
    for (i = 0; i < S1.nq; i++) { px[n] = S1.qx[i]; pz[n] = S1.qz[i]; n++; }
    k_s1 = n;                                           /* index of corner 1 in the polyline */
    for (i = 0; i < ns && n < 395; i++) { px[n] = sx[i]; pz[n] = sz[i]; n++; }
    k_s2 = n - 1;                                       /* corner 2 */
    for (i = 0; i < S2.nq && n < 399; i++) { px[n] = S2.qx[i]; pz[n] = S2.qz[i]; n++; }
    s_why = "";
    nf = bl_fillet(px, pz, n, rwant, rfloor, fx, fz, 2600, &minr, &P->ncorner);
    if (nf < 4) {
        if (!s_why[0]) s_why = "no corridor";
        if (s_diag) {
            int q;
            for (q = 0; q < n; q++)
                TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP]     polyline %d: (%.0f,%.0f)", q, px[q], pz[q]);
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
    P->cx[0] = S1.qx[0]; P->cz[0] = S1.qz[0]; P->cx[nc] = S2.qx[S2.nq - 1]; P->cz[nc] = S2.qz[S2.nq - 1];
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
    /* heights: the slip lanes take the road edge's own height, the streets between them the terrain
     * under them (offset so the two meet), like every other free corridor */
    {
        double s_a = 0.0, s_b = total;                  /* arc lengths where the flush part at each end stops */
        double ya, yb, ds, de, acc = 0.0, acc_c1 = 0.0, acc_c2 = 0.0;
        int q, ia, ib;
        for (q = 1; q < n; q++) {                        /* arc length of the polyline to corner 1, corner 2, its end */
            acc += sqrt((px[q] - px[q - 1]) * (px[q] - px[q - 1]) + (pz[q] - pz[q - 1]) * (pz[q] - pz[q - 1]));
            if (q == k_s1) acc_c1 = acc;
            if (q == k_s2) acc_c2 = acc;
        }
        s_a = acc_c1 - S1.tf;                           /* the fillet starts a tangent length before the corner */
        s_b = total - ((acc - acc_c2) - S2.tf);         /* ... and ends one after corner 2 */
        if (s_a < 0.0) s_a = 0.0;
        if (s_b > total) s_b = total;
        ia = (int)(s_a / (double)TD5_TG_SPAN_LENGTH); if (ia > nc) ia = nc;
        ib = (int)((total - s_b) / (double)TD5_TG_SPAN_LENGTH); if (ib > nc) ib = nc;
        ya = S1.qy[S1.nq - 1]; yb = S2.qy[0];
        ds = ya - bl_env_h(P->cx[ia], P->cz[ia]);
        de = yb - bl_env_h(P->cx[nc - ib], P->cz[nc - ib]);
        for (i = 0; i <= nc; i++) {
            const double sa = total * (double)i / (double)nc;
            const double ui = (nc - ib <= ia) ? 1.0 : (i <= ia ? 0.0 : (i >= nc - ib ? 1.0 : (double)(i - ia) / (double)(nc - ib - ia)));
            if (sa <= s_a && S1.nq > 1) {                /* flush with the road edge: the edge's own height, by distance */
                const double fi = sa / (double)TD5_TG_SPAN_LENGTH;
                const int q0 = (int)fi < S1.nq - 1 ? (int)fi : S1.nq - 2;
                const double f = fi - (double)q0;
                P->cy[i] = S1.qy[q0] + (S1.qy[q0 + 1] - S1.qy[q0]) * (f > 1.0 ? 1.0 : f);
            } else if (sa >= s_b && S2.nq > 1) {
                const double fi = (sa - s_b) / (double)TD5_TG_SPAN_LENGTH;
                int q0 = (int)(fi < 0.0 ? 0.0 : fi); if (q0 > S2.nq - 2) q0 = S2.nq - 2;
                {
                    const double f = fi - (double)q0;
                    P->cy[i] = S2.qy[q0] + (S2.qy[q0 + 1] - S2.qy[q0]) * (f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
                }
            } else {
                {   /* [ROUND 1018 L] ... and lifted off the ground by the network's own margin, ramped in over 6 spans from each flush end */
                    const double lift = td5_env_float("TD5RE_GEO_BLOCKLOOP_LIFT", 60.0f, 0.0f, 400.0f);
                    double r1 = (double)(i - ia) / 6.0, r2 = (double)((nc - ib) - i) / 6.0, ramp;
                    if (r1 < 0.0) r1 = 0.0;
                    if (r2 < 0.0) r2 = 0.0;
                    ramp = r1 < r2 ? r1 : r2;
                    if (ramp > 1.0) ramp = 1.0;
                    P->cy[i] = bl_env_h(P->cx[i], P->cz[i]) + ds + (de - ds) * ui + lift * ramp;
                }
            }
        }
    }
    free(cum);
    P->a1_lo = S1.j0 - 1; P->a1_hi = S1.j1 + 1;
    P->a2_lo = S2.j0 - 1; P->a2_hi = S2.j1 + 1;
    P->npath = np;
    return 1;
}

/* How much of the corridor runs flush against the road's right edge (its left rail IS the road edge), and
 * the main spans it lies against. Counted on the finished chain, so it is what the engine will meet. */
static int bl_flush(const TG_NodeList *nl, BlPlan *P)
{
    const double W = (double)P->lb * bl_lw();
    unsigned char *fr = (unsigned char *)calloc((size_t)P->clen + 2, 1);
    int k, e, ok = 1;
    if (!fr) return 0;
    for (e = 0; e < 2; e++) {
        const int si = e ? P->si2 : P->si1;
        double ex[160], ez[160];
        int jlo = si - 70, jhi = si + 70, nj = 0, j, j0 = 1 << 30, j1 = -1;
        for (j = jlo; j <= jhi && nj < 160; j++) {
            double a, b, c, ox, oz;
            if (j < 1 || j + 2 >= nl->count || !bl_edge_at(nl, j, &a, &b, &c, &ox, &oz)) { ex[nj] = 1e30; ez[nj] = 1e30; nj++; continue; }
            ex[nj] = a; ez[nj] = c; nj++;
        }
        for (k = e ? P->clen : 0; e ? k >= 0 : k <= P->clen; k += e ? -1 : 1) {
            const int ka = k > 0 ? k - 1 : 0, kb = k < P->clen ? k + 1 : P->clen;
            double tx = P->cx[kb] - P->cx[ka], tz = P->cz[kb] - P->cz[ka], l, lx, lz, best = 1e300;
            int bj = -1;
            l = sqrt(tx * tx + tz * tz);
            if (l < 1e-9) break;
            tx /= l; tz /= l;
            /* the row's end toward the road: its LEFT end for a right loop (the road is on its left), its RIGHT end for a left one */
            lx = P->cx[k] - (double)s_sd * tz * W * 0.5; lz = P->cz[k] + (double)s_sd * tx * W * 0.5;
            for (j = 0; j + 1 < nj; j++) {
                double ax = ex[j], az = ez[j], bx = ex[j + 1], bz = ez[j + 1], dx, dz, ll, t, qx, qz, d;
                if (ax > 1e29 || bx > 1e29) continue;
                dx = bx - ax; dz = bz - az; ll = dx * dx + dz * dz;
                t = ll > 1e-9 ? ((lx - ax) * dx + (lz - az) * dz) / ll : 0.0;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                qx = ax + dx * t - lx; qz = az + dz * t - lz;
                d = qx * qx + qz * qz;
                if (d < best) { best = d; bj = jlo + j; }
            }
            if (bj < 0 || best > 900.0 * 900.0) {            /* the corridor has left the road */
                if (s_diag)
                    TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP]     flush %s ends at row %d: left end (%.0f,%.0f) is %.0f from the road edge (centre (%.0f,%.0f), W %.0f)",
                              e ? "arrival" : "departure", k, lx, lz, bj < 0 ? -1.0 : sqrt(best), P->cx[k], P->cz[k], W);
                break;
            }
            fr[k] = 1;
            if (bj < j0) j0 = bj;
            if (bj > j1) j1 = bj;
        }
        if (j1 < 0) { ok = 0; break; }
        if (e == 0) { P->a1_lo = j0 - 1; P->a1_hi = j1 + 2; }
        else        { P->a2_lo = j0 - 1; P->a2_hi = j1 + 2; }
    }
    if (ok) {
        for (k = 0; k < P->clen && fr[k] && fr[k + 1]; k++) P->fl1++;
        for (k = P->clen - 1; k >= 0 && fr[k] && fr[k + 1]; k--) P->fl2++;
        if (P->fl1 < 2 || P->fl2 < 2) ok = 0;
    }
    free(fr);
    return ok;
}

static void bl_free_plan(BlPlan *P)
{
    free(P->cx); free(P->cy); free(P->cz); free(P->inner);
    P->cx = P->cy = P->cz = NULL; P->inner = NULL;
    free(P->rview.v);
    P->rview.v = NULL; P->rview.count = P->rview.cap = 0;
}

/* [ROUND 1020 N] Two loops whose windows overlap may share the stretch. The engine keeps one side-link entry per MAIN span
 * (s_sl_of), so what must not overlap are the MOUTHS (departure and arrival ranges, plus the slip lanes' margin); the two
 * corridors must also stay clear of each other everywhere (a left and a right loop never meet; two loops on one side are
 * checked row against row). TD5RE_GEO_FORK_OVERLAP=0 restores the round-1019 rule (windows may not overlap). */
static int bl_ranges_apart(int alo, int ahi, int blo, int bhi, int margin)
{
    return ahi + margin < blo || bhi + margin < alo;
}

static int bl_overlap_ok(BlPlan *C, const BlPlan *O, int oq)
{
    int c1, c2;
    double need;
    int i, j;
    if (!td5_env_flag_on("TD5RE_GEO_FORK_OVERLAP")) return 0;
    {   /* the four mouth ranges of C against the four of O */
        /* opposite sides never touch: the mouths only have to be different spans (the engine keeps one side-link entry per main
         * span); the same side needs a gap so a slip lane does not run beside the other loop's street */
        const int m = (C->sd == O->sd) ? td5_env_int("TD5RE_GEO_FORK_OVERLAP_MOUTH_GAP", 6, 0, 40)
                                       : td5_env_int("TD5RE_GEO_FORK_OVERLAP_MOUTH_GAP_OPP", 1, 0, 40);
        const int cl[2] = { C->a1_lo, C->a2_lo }, ch[2] = { C->a1_hi, C->a2_hi };
        const int ol[2] = { O->a1_lo, O->a2_lo }, oh[2] = { O->a1_hi, O->a2_hi };
        /* [ROUND 1029 X] SHARED MOUTHS: on OPPOSITE sides two mouths may share main spans (the left and the right
         * arm of one cross street); the engine keeps a second side-link entry per span (td5_track.c, s_sl_of2).
         * What must stay distinct are the spans patched in place in the ring (a loop's F is type 8 with its own
         * link_next, its R type 11 with its own link_prev), and a mouth may share its spans with ONE other mouth
         * only (two entries per span; the AI decides a pair, not a triple). */
        const int shared = C->sd != O->sd && td5_env_flag_on("TD5RE_GEO_LOOP_SHARED_MOUTH");
        if (shared) {
            if (C->F == O->F || C->R == O->R || C->F == O->R || C->R == O->F) return 0;
            for (c1 = 0; c1 < 2; c1++)
                for (c2 = 0; c2 < 2; c2++) {
                    if (bl_ranges_apart(cl[c1], ch[c1], ol[c2], oh[c2], 0)) continue;   /* no span in common */
                    if (C->sh[c1] || O->sh[c2] || C->npend >= 4) return 0;        /* that mouth is already shared */
                    C->sh[c1] = 1;                                                /* C is a candidate copy: dropped if refused */
                    C->pend_q[C->npend] = oq; C->pend_m[C->npend] = c2; C->npend++;
                }
        } else
        for (c1 = 0; c1 < 2; c1++)
            for (c2 = 0; c2 < 2; c2++)
                if (!bl_ranges_apart(cl[c1], ch[c1], ol[c2], oh[c2], m)) return 0;
        /* a mouth must not sit inside the other loop's departure..arrival run on the SAME side either (the slip lane
         * of one would run beside the other's street) */
        if (C->sd == O->sd && C->F - 3 <= O->R + 3 && O->F - 3 <= C->R + 3) {
            const double clr = (double)td5_env_float("TD5RE_GEO_FORK_OVERLAP_CLEAR_M", 9.0f, 0.0f, 60.0f) * BL_UPM;
            need = ((double)C->lb + (double)O->lb) * bl_lw() * 0.5 + 2.0 * 1300.0 + clr;
            for (i = 0; i <= C->clen; i++)
                for (j = 0; j <= O->clen; j++) {
                    const double dx = C->cx[i] - O->cx[j], dz = C->cz[i] - O->cz[j];
                    if (dx * dx + dz * dz < need * need) return 0;
                }
        }
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] OVERLAP kept: F=%d R=%d (%s) beside F=%d R=%d (%s), %s", C->F, C->R, C->sd > 0 ? "left" : "right",
              O->F, O->R, O->sd > 0 ? "left" : "right", C->npend > 0 ? "SHARED MOUTH (same main spans, opposite sides)" : "mouths apart");
    return 1;
}

int tg_bl_build(const TG_NodeList *nl, int ring)
{
    TG_NetArm arms[192];
    int na = 0, ne, i, j, e;
    const int left_on = td5_env_flag_on("TD5RE_GEO_BLOCKLOOP_LEFT");     /* [1019 V] left-hand loops */
    BlPlan cand[64];
    int ncand = 0, built = 0;
    const int max_loops = td5_env_int("TD5RE_GEO_BLOCKLOOP_MAX", 6, 0, BL_MAXPLAN);
    const double rwant = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RMIN_M", 22.0f, 4.0f, 40.0f) * BL_UPM;
    const double rfloor = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RFLOOR_M", 7.0f, 3.0f, 30.0f) * BL_UPM;
    const double flare = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_FLARE", 0.0f, 0.0f, 9000.0f);
    const int lanes_knob = td5_env_int("TD5RE_GEO_BLOCKLOOP_LANES", 2, 0, 4);
    const double lmax = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_LMAX_M", 420.0f, 60.0f, 1200.0f) * BL_UPM;
    const double rmin = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RATIO_MIN", 1.4f, 1.0f, 8.0f);
    const double rmax = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_RATIO_MAX", 8.0f, 1.0f, 20.0f);
    const int finish = tg_realfork_finish_span();
    const double skew_max = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_SKEW_DEG", 40.0f, 5.0f, 85.0f) * kPi / 180.0;
    int off;

    s_diag = td5_env_flag_off("TD5RE_GEO_FORK_DIAG");
    s_clear = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_CLEAR_M", 25.0f, 5.0f, 80.0f) * BL_UPM;
    if (!td5_env_flag_on("TD5RE_GEO_FORK_BLOCKLOOP") || max_loops < 1) return 0;
    if (!tg_realfork_enabled()) return 0;     /* the harness's forks-off arm (TD5RE_GEO_REAL_FORKS=0) has no loops either */
    if (!td5_geo_loaded() || !nl || nl->count < 120) return 0;
    if (!td5_geo_roads_sync(td5_geo_place_slug())) return 0;
    if (!tg_net_geo()) return 0;
    if (s_fork_count >= TD5_TG_BRANCH_MAX) return 0;

    /* the accepted RIGHT arms of real streets */
    ne = tg_net_edge_count();
    for (e = 0; e < ne && na < (left_on ? 192 : 96); e++) {
        TG_NetArm a;
        if (!tg_net_arm_get(e, &a)) continue;
        if (a.loop) continue;
        if (a.left && !left_on) continue;
        if (fabs(a.skew) > skew_max) continue;      /* a street that leaves at a shallow angle is not a turn-in */
        arms[na++] = a;
    }
    if (na < 2) return 0;
    s_sd = -1;
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
            if ((A->left != 0) != (B->left != 0)) continue;       /* [1019 V] both arms on the same side of the road */
            s_sd = A->left ? 1 : -1;
            if (!bl_only_ok(A->si, A->left)) continue;
            snprintf(what, sizeof what, "arms %d/%d", A->edge, B->edge);
            ra = td5_geo_roads_get(A->road); rb = td5_geo_roads_get(B->road);
            if (!ra || !rb) continue;
            sn = bl_arm_node(nl, A); dn = bl_arm_node(nl, B);
            if (sn < 0 || dn < 0 || sn == dn) { bl_note(what, A->si, B->si, "an arm's way has no usable vertex right of the road"); continue; }
            np = bl_path(nl, sn, dn, lmax, path, 256, &lenp);
            if (np < 2) { bl_note(what, A->si, B->si, s_sd < 0 ? "no path over the real grid that stays right of the road" : "no path over the real grid that stays left of the road"); continue; }
            {
                /* [1019 V] the one-way flow along the path (map data): edges WITH / AGAINST the direction of travel round the loop.
                 * Logged on every candidate. TD5RE_GEO_BLOCKLOOP_LEFT_ONEWAY=1 refuses a LEFT loop that runs against a one-way
                 * street (default 0: the round-1018 right loops ignore the flow too, and in La Plata's alternating one-way grid every
                 * street on the left of this route runs the other way -- the check removes every left loop there). */
                int q3, against = 0, with = 0;
                for (q3 = 0; q3 + 1 < np; q3++) {
                    int k3;
                    for (k3 = g_n[path[q3]].head; k3 >= 0; k3 = g_e[k3].next)
                        if (g_e[k3].to == path[q3 + 1]) {
                            if (g_e[k3].ow < 0) against++; else if (g_e[k3].ow > 0) with++;
                            break;
                        }
                }
                s_ow_against = against; s_ow_with = with;
                if (s_sd > 0 && against > 0 && td5_env_flag_off("TD5RE_GEO_BLOCKLOOP_LEFT_ONEWAY")) {
                    char w[96];
                    snprintf(w, sizeof w, "the path runs against a one-way street (%d edge(s) against, %d with)", against, with);
                    bl_note(what, A->si, B->si, w);
                    continue;
                }
            }
            for (q = A->si; q < B->si && q + 1 < nl->count; q++) {
                const double dx = nl->v[q + 1].x - nl->v[q].x, dz = nl->v[q + 1].z - nl->v[q].z;
                main_m += sqrt(dx * dx + dz * dz);
            }
            main_m /= BL_UPM;
            if (main_m < 20.0) continue;
            P = &cand[ncand];
            memset(P, 0, sizeof *P);
            P->e1 = A->edge; P->e2 = B->edge; P->si1 = A->si; P->si2 = B->si;
            P->sd = s_sd;
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
            if (lanes_knob > 0) P->lb = lanes_knob;     /* TD5RE_GEO_BLOCKLOOP_LANES: a slip lane beside the road wants 2 */
            P->flare = flare;
            P->main_m = main_m;
            if (!bl_chain(nl, P, A, B, path, np, rwant, rfloor)) {
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
            if (!bl_flush(nl, P)) { bl_note(what, A->si, B->si, "the corridor does not run flush with the road edge at a mouth"); bl_free_plan(P); continue; }
            {   /* the road must be straight beside both slip lanes: a lane flush with a bend folds, and the bend is the road's own corner */
                const double tol = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_STRAIGHT_DEG", 5.0f, 0.5f, 45.0f) * kPi / 180.0;
                int e2, sj, bad = 0;
                for (e2 = 0; e2 < 2 && !bad; e2++) {
                    const int a = e2 ? P->a2_lo : P->a1_lo, b = e2 ? P->a2_hi : P->a1_hi, c = e2 ? P->si2 : P->si1;
                    for (sj = a - 1; sj <= b + 1 && !bad; sj++) {
                        double dd;
                        if (sj < 1 || sj + 1 >= nl->count) { bad = 1; break; }
                        dd = nl->v[sj].tx * nl->v[c].tx + nl->v[sj].tz * nl->v[c].tz;
                        if (dd > 1.0) dd = 1.0;
                        if (dd < -1.0) dd = -1.0;
                        if (acos(dd) > tol) bad = 1;
                    }
                }
                if (bad) { bl_note(what, A->si, B->si, "the road bends beside a slip lane"); bl_free_plan(P); continue; }
            }
            lo1 = P->a1_lo; hi1 = P->a1_hi; lo2 = P->a2_lo; hi2 = P->a2_hi;
            (void)lo2; (void)hi1;
            P->F = lo1; P->R = hi2; P->len = P->R - P->F - 1;
            if (P->F < TD5_TG_GRID_SPAN + 8) { bl_note(what, A->si, B->si, "inside the start grid"); ok = 0; }
            if (ok && (P->R + 12 >= ring - 24 || (finish > 0 && P->R + 10 >= finish))) { bl_note(what, A->si, B->si, "past the finish line or inside the ring tail"); ok = 0; }
            if (ok && P->len < 6) { bl_note(what, A->si, B->si, "the two mouths overlap"); ok = 0; }
            if (ok && (!bl_lanes_uniform(nl, P->F - 2, P->a1_hi + 2) || !bl_lanes_uniform(nl, P->a2_lo - 2, P->R + 2))) {
                bl_note(what, A->si, B->si, "the route's lane count changes at a mouth"); ok = 0; }
            if (ok) {   /* the AI picks its lane in the spans BEFORE the mouth: no sharp corner there (a pack arriving at a 90 degree
                         * corner that is also the lane choice piled up: round 1018 L, F=690 at span 684-689) */
                const int app = td5_env_int("TD5RE_GEO_BLOCKLOOP_APPROACH_SPANS", 14, 0, 40);
                const double lim = (double)td5_env_float("TD5RE_GEO_BLOCKLOOP_APPROACH_DEG", 20.0f, 1.0f, 180.0f) * kPi / 180.0;
                double turn = 0.0;
                int j;
                for (j = P->F - app; j < P->a1_hi && j + 1 < nl->count; j++) {
                    double d;
                    if (j < 0) continue;
                    d = atan2(nl->v[j + 1].tz, nl->v[j + 1].tx) - atan2(nl->v[j].tz, nl->v[j].tx);
                    while (d > kPi) d -= 2.0 * kPi;
                    while (d < -kPi) d += 2.0 * kPi;
                    turn += fabs(d);
                }
                if (turn > lim) {
                    char w[96];
                    snprintf(w, sizeof w, "a %.0f degree bend within %d spans of the departure", turn * 180.0 / kPi, app);
                    bl_note(what, A->si, B->si, w); ok = 0;
                }
            }
            if (ok) {
                int s;
                const int beside = td5_env_flag_on("TD5RE_GEO_LOOP_FORK_BESIDE");   /* [1022 N2] a real fork on the OTHER side may share the stretch */
                for (s = P->F - 2; s <= P->R + 2 && ok; s++) {
                    if (tg_span_in_bridge_run(s) || tg_span_in_tunnel(s)) { ok = 0; break; }
                    if (tg_span_in_fork_clear(s)) {
                        if (!beside || bl_fork_blocks(P, s)) ok = 0;
                        else P->beside_n++;
                    }
                }
                if (!ok) bl_note(what, A->si, B->si, "a fork, bridge or tunnel is in the bypassed stretch");
            }
            if (ok) {   /* the corridor stays clear of every other street and of every carriageway but its mouths */
                int k;
                for (k = 2; k <= P->clen - 2 && ok; k += 2) {
                    const double hw = ((double)P->lb * bl_lw()) * 0.5;
                    const int oe = tg_net_near_other_street(P->cx[k], P->cz[k], hw, P->e1, P->e2);
                    if (oe) {
                        /* a drawn side-street STUB the corridor runs over (the slip lane passes the mouths of
                         * other streets): the loop takes it over, its stub is not drawn. Anything else is a refusal. */
                        TG_NetArm oa;
                        int q2, have = 0;
                        if (tg_net_arm_get(oe - 1, &oa) && !oa.left) {
                            for (q2 = 0; q2 < P->nabs; q2++) if (P->absorb[q2] == oe - 1) have = 1;
                            if (!have && P->nabs < 8) P->absorb[P->nabs++] = oe - 1;
                            else if (!have) { ok = 0; bl_note(what, A->si, B->si, "runs over too many drawn streets"); }
                        } else {
                            char w[96];
                            snprintf(w, sizeof w, "crosses another drawn street at row %d of %d (%.0f,%.0f)", k, P->clen, P->cx[k], P->cz[k]);
                            ok = 0; bl_note(what, A->si, B->si, w);
                        }
                    }
                    else if (k > 16 && k < P->clen - 16 &&
                             tg_world_occ_near(P->cx[k], P->cz[k], hw + 600.0, TG_WO_DRIVABLE | TG_WO_ROAD)) { ok = 0; bl_note(what, A->si, B->si, "runs over a carriageway or another corridor"); }
                    else if (tg_world_is_water(P->cx[k], P->cz[k])) { ok = 0; bl_note(what, A->si, B->si, "runs into water"); }
                }
            }
            if (!ok) { bl_free_plan(P); continue; }
            snprintf(P->name, sizeof P->name, "%s", what);
            P->used = 1;
            if (s_diag)
                TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] CANDIDATE %s side %s F=%d R=%d (arms si %d -> %d), corridor %d spans %.0f m over %.0f m of road (%.2fx), %d corner(s), tightest %.1f m, one-way edges: %d with / %d against%s",
                          what, s_sd > 0 ? "LEFT" : "right", P->F, P->R, A->si, B->si, P->clen, P->len_m, P->main_m, P->ratio, P->ncorner, P->min_r_m, s_ow_with, s_ow_against,
                          P->beside_n > 0 ? " [BESIDE REAL FORK]" : "");
            ncand++;
        }
    }
    bl_graph_free();
    {
        int nleft = 0;
        for (i = 0; i < na; i++) if (arms[i].left) nleft++;
        TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] %d right arm(s)%s, %d candidate loop(s)", na - nleft,
                  left_on ? " + left arm(s) counted below" : "", ncand);
        if (left_on) TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] %d left arm(s)", nleft);
    }
    if (ncand < 1) return 0;
    qsort(cand, (size_t)ncand, sizeof cand[0], bl_plan_cmp);

    off = ring;
    for (i = 0; i < s_fork_count; i++) off += 1 + tg_fork_clen(i);

    for (i = 0; i < ncand && s_nbl < max_loops && s_fork_count < TD5_TG_BRANCH_MAX; i++) {
        BlPlan *C = &cand[i];
        int clash = 0, q, k;
        for (q = 0; q < s_nbl; q++) {   /* windows may not overlap (unless [1020 N] they may), and arms are used once */
            const BlPlan *O = &s_bl[q];
            if (C->F - 3 <= O->R + 3 && O->F - 3 <= C->R + 3 && !bl_overlap_ok(C, O, q)) clash = 1;
            if (C->e1 == O->e1 || C->e1 == O->e2 || C->e2 == O->e1 || C->e2 == O->e2) clash = 1;
        }
        if (clash) {
            TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] F=%d R=%d (%s) \"%s\" dropped: its window or an arm clashes with a loop already taken (mouths %d..%d / %d..%d)",
                      C->F, C->R, C->sd > 0 ? "left" : "right", C->name, C->a1_lo, C->a1_hi, C->a2_lo, C->a2_hi);
            bl_free_plan(C); continue;
        }
        if (!td5_dev_forkfilter_allow2(C->F, C->si1)) {    /* dev-only allow/deny by the DEPARTURE MOUTH span (verify/geo_fork_validate.ps1 -Only 739): F moves with the flare */
            TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] F=%d \"%s\" filtered out (TD5RE_GEO_FORK_ALLOW/DENY)", C->F, C->name);
            bl_free_plan(C);
            continue;
        }
        if (td5_env_flag_on("TD5RE_GEO_FORK_VERDICTS")) {   /* the verdict table: PASS only builds on a route that has a record */
            const char *place = td5_geo_place_slug();
            const unsigned int fp = tg_realfork_route_fp();
            const int known = fp && td5_fork_verdict_route_known(place ? place : "", fp);
            const int v = known ? td5_fork_verdict_get(place ? place : "", fp, C->F, C->R) : -1;
            int drop = 0;
            if (v == TD5_FV_FAIL || v == TD5_FV_UNTESTED || v == TD5_FV_WARN) drop = 1;
            else if (v < 0 && (known || !td5_env_flag_on("TD5RE_GEO_FORK_UNVERIFIED"))) drop = 1;
            if (drop) {
                TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] F=%d R=%d \"%s\" refused by the verdict table (%s)", C->F, C->R, C->name,
                          v == TD5_FV_FAIL ? "FAIL" : v == TD5_FV_UNTESTED ? "UNTESTED" : v == TD5_FV_WARN ? "WARN"
                          : known ? "not in the record for this route" : "no record for this route, UNVERIFIED=0");
                bl_free_plan(C);
                continue;
            }
        }
        if (off + 1 + C->clen >= TD5_TG_MAX_SPANS - 8) {
            bl_note(C->name, C->si1, C->si2, "the track would exceed the span budget");
            bl_free_plan(C);
            continue;
        }
        /* take the plan */
        for (q = 0; q < C->npend; q++) s_bl[C->pend_q[q]].sh[C->pend_m[q]] = 1;   /* [1029 X] those mouths are shared now */
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
            /* the corridor's INNER edge (the side facing the road) carries a pavement wherever a pavement strip
             * there is clear of the road's own carriageway and its pavement: not along the slip lane beside it */
            P->inner = (unsigned char *)calloc((size_t)P->clen + 1, 1);
            if (P->inner && td5_env_flag_on("TD5RE_GEO_BLOCKLOOP_INNER_PAVE")) {
                const double sw = 1300.0;
                for (k = 0; k <= P->clen; k++) {
                    const TG_Node *nn = &P->rview.v[F + 1 + k];
                    const double t = nn->width * 0.5 + sw * 0.5;
                    const double sg = (P->sd > 0) ? -1.0 : 1.0;       /* [1019 V] the road is on the corridor's left (right loop) or right (left loop) */
                    const double qx = nn->x + sg * nn->tz * t, qz = nn->z - sg * nn->tx * t;
                    double dmin = 1e30;
                    int j;
                    for (j = P->F - 6; j <= P->R + 6; j++) {
                        double ax, az, bx, bz, dx, dz, l2, u, d;
                        if (j < 0 || j + 1 >= nl->count) continue;
                        ax = nl->v[j].x; az = nl->v[j].z; bx = nl->v[j + 1].x; bz = nl->v[j + 1].z;
                        dx = bx - ax; dz = bz - az; l2 = dx * dx + dz * dz;
                        u = l2 > 1e-9 ? ((qx - ax) * dx + (qz - az) * dz) / l2 : 0.0;
                        if (u < 0.0) u = 0.0;
                        if (u > 1.0) u = 1.0;
                        d = sqrt((qx - ax - u * dx) * (qx - ax - u * dx) + (qz - az - u * dz) * (qz - az - u * dz))
                            - 0.5 * nl->v[j].width;
                        if (d < dmin) dmin = d;
                    }
                    P->inner[k] = (dmin > sw * 1.5 + 200.0) ? 1 : 0;
                }
            }
            /* the end rows are perpendicular to the road: the mouth's near edge lies ON the edge */
            memset(fk, 0, sizeof *fk);
            fk->F = P->F; fk->len = P->len; fk->R = P->R; fk->cbase = off + 1;
            fk->sep = 0.5; fk->lanes = nl->v[P->F].lanes; fk->kind = TG_FORK_LOOP;
            fk->main_lanes = nl->v[P->F].lanes; fk->br_lanes = P->lb;
            fk->fm = 1.0; fk->fb = 1.0; fk->side = (P->sd > 0) ? 1 : -1;      /* [1019 V] */
            fk->loopc = s_nbl + 1; fk->clen = P->clen;
            P->fi = s_fork_count;
            P->built = 1;
            off += 1 + P->clen;
            s_fork_count++;
            built++;
            tg_net_loop_take(P->e1, P->a1_lo, P->a1_hi);
            tg_net_loop_take(P->e2, P->a2_lo, P->a2_hi);
            for (q = 0; q < P->nabs; q++) tg_net_loop_take(P->absorb[q], -1, -2);
            tg_bl_paint_network(P->fi);
            TD5_LOG_I(LOG_TAG, "trackgen: [BLOCK LOOP] %d: fork %d F=%d R=%d (mouths %d..%d / %d..%d, flush spans %d / %d) arms %d/%d lanes %d, "
                      "corridor %d spans %.0f m over %.0f m of road (%.2fx), %d corner(s), tightest %.1f m, %d graph nodes",
                      s_nbl, P->fi, P->F, P->R, P->a1_lo, P->a1_hi, P->a2_lo, P->a2_hi, P->fl1, P->fl2, P->e1, P->e2, P->lb,
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

int tg_bl_inner_ok(int fi, int k)
{
    const BlPlan *P = bl_of(fi);
    if (!P || !P->inner || k < 0 || k >= P->clen) return 0;
    return P->inner[k] && P->inner[k + 1];
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
        tg_put_u32(out, (P->sd > 0) ? 1u : 0u);                  /* [1019 V] side: 0 right, 1 left */
        tg_put_u32(out, (unsigned int)(s_forks[P->fi].cbase + P->fl1 - 1));
        tg_put_u32(out, (unsigned int)(s_forks[P->fi].cbase + P->clen - P->fl2));
    }
    return n;
}
