/**
 * td5_tg_network.c -- auto-track STREET NETWORK: planar road graph (city streets, avenues, back streets, bend continuations, country loops, underpass crossings) validated on the world occupancy raster; the (span,side) street authority; NETWORK.JSON
 *
 * [TOPOLOGY-FIRST 2026-09-08] Before this module a "street" had no object:
 * tg_xstreet_here was a memoised (span, side) predicate derived from the
 * facade-gap hash, its reach an outward ray march that only ever tested the
 * main carriageway, and no two streets were ever compared with each other.
 *
 * Now the openings the facade rhythm proposes (tg_facade_block: a run of
 * 2..4 spans per 22-span superblock, 6..8 for an avenue) are CANDIDATES. Each
 * is walked outward on the world's occupancy raster: it stops short of any
 * carriageway (main road or fork corridor), joins another street it meets
 * (a T-junction), stops at water or ground steeper than the road can climb,
 * and is dropped altogether if it cannot get TD5_TG_R8_CLAMP_MIN clear of
 * the kerb -- the frontage then stays BUILT, which is the fallback every
 * junction emitter already understands. Every street that survives is
 * painted into the raster, so the next one sees it: the network is planar
 * by construction. Back streets close the blocks between neighbouring
 * street ends; forest lanes may wander on as a loop and rejoin the main
 * road further along.
 *
 * The graph then feeds the MOUTH TABLE: for every (span, side) the edge that
 * leaves there, its bearing (skew from the outward normal) and its reach.
 * tg_facade_built / tg_xstreet_here / tg_xstreet_reach_at /
 * tg_block_arm_skew / tg_r12_fcross_at read that table, so the ~90 junction
 * consumers keep their signatures and can no longer disagree with each
 * other about where a street is.
 *
 * [GEO PHASE 5 2026-09-30] On a geo build the candidates come from the REAL
 * OSM roads of the place cache instead of the facade rhythm: the network
 * ACCEPTS streets that already exist rather than PLANTING them where the
 * raster has room. See the "GEO: REAL STREETS" section below. Everything
 * downstream -- the mouth table, the paint, the audit -- is unchanged, so the
 * two candidate sources are interchangeable and a synthetic build never
 * reaches a line of it.
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_buildings.h"   /* [1014 B] plaza polygons: the roads that border them */

#define TG_NET_MAX_EDGES 2048
#define TG_NET_MAX_NODES 4096
#define TG_NET_POLY      16
#define TG_NET_RIB_Q     256     /* [1015 B] ribbon quads per mesh once cut into span-length pieces */
#define TG_NET_STEP      600.0
#define TG_NET_MARGIN    500.0     /* clear air a street keeps off tarmac   */
#define TG_NET_BACK_MAX  26000.0   /* longest back street                   */
#define TG_NET_LOOP_MAX  90        /* spans a country loop may run ahead    */

typedef struct {
    double x, z, y;
    int    kind;        /* 0 mouth on the main road, 1 junction, 2 free end */
    int    si;          /* main span for a mouth, else -1                    */
} TG_NetNode;

typedef struct {
    int    a, b;        /* node indices                                       */
    int    kind;        /* TG_NE_*                                            */
    double width;
    int    drivable;
    int    mouth_si, mouth_left, mouth_lo, mouth_hi;   /* opening on the road */
    double skew, reach; /* bearing from the outward normal; straight reach  */
    int    npoly;
    double px[TG_NET_POLY], pz[TG_NET_POLY], py[TG_NET_POLY];
    int    rejoin_si;   /* -1, or the main span a loop rejoins              */
    int    ribbon;      /* [1014 B] a real way's polyline laid as ONE mitred ribbon */
    /* TD5_GEO_SURF_* of the real OSM way this edge came from, SMOOTH (0) for
     * every synthetic edge -- which is the no-op, so a synthetic build reads
     * exactly as it did. See tg_net_mouth_surface. */
    int    surface;
} TG_NetEdge;

typedef struct { short edge; float skew, reach, shift; } TG_NetMouth;   /* shift: [1014 B] origin offset past the race kerb */

static TG_NetNode  s_nodes[TG_NET_MAX_NODES];
static TG_NetEdge  s_edges[TG_NET_MAX_EDGES];
static TG_NetMouth s_mouth[TD5_TG_MAX_SPANS + 8][2];   /* [si][0=left,1=right] */
static int s_nn, s_ne, s_net_built, s_net_nspans;
static int s_net_geo;          /* [1014 B] last build sourced its streets from the real map */
static long s_stat_cand, s_stat_short, s_stat_tjunc, s_stat_water, s_stat_road;

static const char *const k_ne_name[TG_NE_KIND_COUNT] = {
    "street", "avenue", "backstreet", "continuation", "country", "underpass", "bypass"
};

/* ------------------------------------------------------------ helpers -- */

static int tg_net_node(double x, double z, double y, int kind, int si)
{
    TG_NetNode *n;
    if (s_nn >= TG_NET_MAX_NODES) return -1;
    n = &s_nodes[s_nn];
    n->x = x; n->z = z; n->y = y; n->kind = kind; n->si = si;
    return s_nn++;
}

static TG_NetEdge *tg_net_edge_new(int a, int b, int kind, double width)
{
    TG_NetEdge *e;
    if (s_ne >= TG_NET_MAX_EDGES || a < 0 || b < 0) return NULL;
    e = &s_edges[s_ne++];
    memset(e, 0, sizeof(*e));
    e->a = a; e->b = b; e->kind = kind; e->width = width;
    e->mouth_si = -1; e->rejoin_si = -1; e->mouth_lo = e->mouth_hi = -1;
    e->npoly = 2;
    e->px[0] = s_nodes[a].x; e->pz[0] = s_nodes[a].z; e->py[0] = s_nodes[a].y;
    e->px[1] = s_nodes[b].x; e->pz[1] = s_nodes[b].z; e->py[1] = s_nodes[b].y;
    return e;
}

static void tg_net_paint_edge(const TG_NetEdge *e, unsigned bits)
{
    int k;
    for (k = 0; k + 1 < e->npoly; k++)
        tg_world_occ_seg(e->px[k], e->pz[k], e->px[k + 1], e->pz[k + 1],
                         e->width * 0.5, bits);
}

/* [1014 B item 8] WHERE A STREET LEAVES A DIVIDED AVENUE.
 *
 * Every street mouth used to start at the RACE kerb. On the avenue's own side
 * that is the kerb at the median, so a real street that crosses a divided
 * avenue (Calle 22 across Diagonal 73) could only ever be drawn on the race
 * carriageway's side: the far half starts at the OTHER carriageway's outer
 * kerb, 4.6 m + 1 lane further out, and was refused as "corridor" (the fork's
 * window covers every opening, by design) or "short" (the connector between the
 * two carriageways is 3000 units). The far-side street is the same street, so
 * its origin is simply the outermost tarmac on that side: carriageway reach
 * minus the race half width. 0 off an avenue, so every other street starts
 * where it always did. GEO ONLY (callers are the geo placement). */
static double tg_net_far_shift(const TG_NodeList *nl, int si, double sg)
{
    double r, hw;
    if (!td5_env_flag_on("TD5RE_GEO_FAR_STREETS")) return 0.0;
    if (!nl || si < 0 || si + 1 >= nl->count) return 0.0;
    if (tg_geo_avenue_n() < 1) return 0.0;
    r  = tg_geo_avenue_reach(nl, si, sg);
    hw = tg_road_half_width(nl, si);
    /* [1014 integ] Over a REAL fork's window the far carriageway IS the drivable
     * corridor, and tg_geo_avenue_reach answers 0 there on purpose (round 1014 A:
     * the corridor, not the scenery road, is what the skirt must clear). Without
     * this the street's origin fell back to the race kerb and tg_side_corridor_here
     * refused it as "corridor" -- 20 arms on La Plata, the far-side streets B had
     * recovered across the long forks. The corridor's OUTER edge is the same
     * "outermost tarmac on that side"; tg_carriageway_reach already folds it in.
     * Real forks only: a synthetic fork's corridor still refuses streets. */
    if (td5_env_flag_on("TD5RE_GEO_FAR_FORK") && !(r - hw > 1.0)) {
        int i;
        for (i = 0; i < s_fork_count; i++) {
            if (s_forks[i].real <= 0) continue;
            if (si < s_forks[i].F - 1 || si > s_forks[i].F + s_forks[i].len + 1) continue;
            if (sg * (double)s_forks[i].side < 0.0) continue;
            r = tg_carriageway_reach(nl, si, sg);
            break;
        }
    }
    return (r - hw > 1.0) ? (r - hw) : 0.0;
}

double tg_net_mouth_shift(int si, int left)
{
    const TG_NetMouth *m;
    if (!s_net_built || si < 0 || si >= TD5_TG_MAX_SPANS + 8) return 0.0;
    m = &s_mouth[si][left ? 0 : 1];
    return (m->edge >= 0) ? (double)m->shift : 0.0;
}

static void tg_net_set_mouth(int lo, int hi, int left, int edge, double skew, double reach)
{
    int s;
    for (s = lo; s <= hi; s++) {
        if (s < 0 || s >= TD5_TG_MAX_SPANS + 8) continue;
        s_mouth[s][left ? 0 : 1].edge  = (short)edge;
        s_mouth[s][left ? 0 : 1].skew  = (float)skew;
        s_mouth[s][left ? 0 : 1].reach = (float)reach;
        s_mouth[s][left ? 0 : 1].shift = 0.0f;
    }
}

/* Walk outward from (sx,sz) along unit (dx,dz), up to `want`. Returns the
 * clear reach and writes *why: 0 full, 1 carriageway ahead, 2 street met
 * (T-junction), 3 water/steep. `half` is the street's half width. */
static double tg_net_march(double sx, double sz, double dx, double dz,
                           double want, double half, double from, int *why)
{
    double d, last = 0.0;
    *why = 0;
    for (d = TG_NET_STEP; d <= want; d += TG_NET_STEP) {
        const double px = sx + dx * d, pz = sz + dz * d;
        /* Another street this close to the kerb: the two would cross.
         * Tested from the FIRST step (a leaning neighbour crosses early). */
        if (tg_world_occ_near(px, pz, half + 200.0, TG_WO_STREET)) {
            *why = 2; return d;
        }
        if (tg_world_is_water(px, pz) || tg_world_slope(px, pz) >= TG_WORLD_STEEP_SLOPE) {
            *why = 3; return last;
        }
        /* The main road is painted half-width + 600 wide; its own paint is
         * behind `from`, so test the carriageway only from there, with a small
         * radius on the street's centre line (the raster cell is 1500). */
        if (d >= from &&
            tg_world_occ_near(px, pz, TG_NET_MARGIN, TG_WO_ROAD | TG_WO_DRIVABLE)) {
            *why = 1; return last;
        }
        last = d;
    }
    return want;
}

/* ------------------------------------------------ candidate generators -- */

/* City streets and avenues: the facade rhythm's openings, validated. */
static void tg_net_city_streets(const TG_NodeList *nl, int nspans)
{
    int si, left;
    for (left = 1; left >= 0; left--) {
        const double sg = left ? 1.0 : -1.0;
        int lo = -1;
        for (si = 1; si <= nspans; si++) {
            int cand = 0;
            if (si < nspans && si + 1 < nl->count &&
                td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_STREETS") &&
                !tg_span_in_bridge_run(si) && !tg_span_in_tunnel(si)) {
                const TG_Biome *b = &k_biomes[tg_scenery_biome_index(si)];
                if (tg_city_sidewalk_w(b) > 0.0
                    && !(td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE")
                         && tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR))
                    && !tg_facade_built_hash(si, left)
                    && !tg_block_is_park(si, left)
                    && !tg_side_corridor_here(nl, si, sg))
                    cand = 1;
            }
            if (cand && lo < 0) lo = si;
            if (!cand && lo >= 0) {
                /* opening [lo, si-1] */
                const int hi = si - 1, c = (lo + hi) / 2;
                const TG_Biome *b = &k_biomes[tg_scenery_biome_index(c)];
                const double sw = tg_city_sidewalk_w(b);
                const double skew = tg_block_arm_skew_hash(c, left);
                const double want = tg_city_crossst_reach(b, sw);
                const double width = (double)(hi - lo + 1) * (double)TD5_TG_LANE_WIDTH;
                double e[10], ox, oz, reach;
                int why, a, bnode, kind;
                unsigned int blk, phase, gs, gl;
                int av;
                TG_NetEdge *ed;

                s_stat_cand++;
                tg_city_edge_frame(nl, c, sg, e);
                {
                    const double cs = cos(skew), sn = sin(skew);
                    ox = e[6] * cs - e[7] * sn;
                    oz = e[6] * sn + e[7] * cs;
                }
                reach = tg_net_march(e[0], e[2], ox, oz, want, width * 0.5,
                                     TD5_TG_R8_CLAMP_MIN, &why);
                if (why == 1) s_stat_road++;
                if (why == 2) s_stat_tjunc++;
                if (why == 3) s_stat_water++;
                reach = tg_r13_fold_cap(nl, c, sg, skew, reach, "TD5RE_R13_FOLD_STREET");
                if (reach < TD5_TG_R8_CLAMP_MIN) { s_stat_short++; lo = -1; continue; }

                tg_facade_block(c, left, &blk, &phase, &gs, &gl, &av);
                kind = tg_turn_open(c, left) ? TG_NE_CONTINUATION
                     : (av ? TG_NE_AVENUE : TG_NE_STREET);
                a = tg_net_node(e[0], e[2], e[1], 0, c);
                bnode = tg_net_node(e[0] + ox * reach, e[2] + oz * reach,
                                    tg_world_h(e[0] + ox * reach, e[2] + oz * reach),
                                    why == 2 ? 1 : 2, -1);
                ed = tg_net_edge_new(a, bnode, kind, width);
                if (!ed) { lo = -1; continue; }
                ed->mouth_si = c; ed->mouth_left = left;
                ed->mouth_lo = lo; ed->mouth_hi = hi;
                ed->skew = skew; ed->reach = reach;
                tg_net_paint_edge(ed, TG_WO_STREET);
                tg_net_set_mouth(lo, hi, left, (int)(ed - s_edges), skew, reach);
                lo = -1;
            }
        }
    }
}

/* Back streets: join the far ends of neighbouring streets on one side when
 * the segment between them is clear, closing the block. */
static void tg_net_back_streets(void)
{
    int i, j, made = 0;
    if (!td5_env_flag_on("TD5RE_TG_NET_BACKSTREETS")) return;
    for (i = 0; i < s_ne; i++) {
        const TG_NetEdge *ei = &s_edges[i];
        if (ei->kind != TG_NE_STREET && ei->kind != TG_NE_AVENUE && ei->kind != TG_NE_CONTINUATION) continue;
        if (s_nodes[ei->b].kind != 2) continue;          /* free end only */
        for (j = i + 1; j < s_ne; j++) {
            const TG_NetEdge *ej = &s_edges[j];
            double dx, dz, len, k, half;
            int clear = 1, a, b;
            TG_NetEdge *ed;
            if (ej->kind != TG_NE_STREET && ej->kind != TG_NE_AVENUE && ej->kind != TG_NE_CONTINUATION) continue;
            if (ej->mouth_left != ei->mouth_left) continue;
            if (ej->mouth_si - ei->mouth_si > 40 || ej->mouth_si <= ei->mouth_si) continue;
            if (s_nodes[ej->b].kind != 2) continue;
            a = ei->b; b = ej->b;
            dx = s_nodes[b].x - s_nodes[a].x; dz = s_nodes[b].z - s_nodes[a].z;
            len = sqrt(dx * dx + dz * dz);
            if (len < 3000.0 || len > TG_NET_BACK_MAX) continue;
            half = (double)TD5_TG_LANE_WIDTH;              /* two lanes */
            /* start past both ends' own paint (their street half + margin) */
            for (k = half + TG_NET_MARGIN + 1200.0; k < len - (half + TG_NET_MARGIN + 1200.0); k += TG_NET_STEP) {
                const double px = s_nodes[a].x + dx * k / len, pz = s_nodes[a].z + dz * k / len;
                if (tg_world_occ_near(px, pz, half + TG_NET_MARGIN, TG_WO_ROAD | TG_WO_DRIVABLE | TG_WO_STREET)
                    || tg_world_is_water(px, pz)
                    || tg_world_slope(px, pz) >= TG_WORLD_HILL_SLOPE * 2.0) { clear = 0; break; }
            }
            if (!clear) continue;
            ed = tg_net_edge_new(a, b, TG_NE_BACKSTREET, half * 2.0);
            if (!ed) return;
            ed->mouth_si = ei->mouth_si;      /* owner span for emission */
            ed->mouth_left = ei->mouth_left;
            tg_net_paint_edge(ed, TG_WO_STREET);
            tg_world_conform_seg(ed->px[0], ed->pz[0], ed->py[0], ed->px[1], ed->pz[1], ed->py[1],
                                 half + 600.0, 2500.0);
            s_nodes[a].kind = 1; s_nodes[b].kind = 1;
            made++;
            break;                                          /* one per end */
        }
    }
    (void)made;
}

/* Forest lanes: the R12 candidate rhythm, validated on the raster; a lane
 * may then wander on as a LOOP and rejoin the main road further along. */
static void tg_net_country(const TG_NodeList *nl, int nspans)
{
    int blk;
    const int loops = td5_env_flag_on("TD5RE_TG_NET_LOOPS");
    for (blk = 0; blk * TD5_TG_R12_FCROSS_PERIOD < nspans; blk++) {
        int c, j, ok = 1, why, a, bnode;
        double side, e[10], reach, want = TD5_TG_R12_FCROSS_WANT;
        TG_NetEdge *ed;
        if (!tg_r12_fcross_candidate(nl, blk, &c, &side)) continue;
        for (j = c - 1; j <= c + TD5_TG_R12_FCROSS_WIDTH && ok; j++) {
            if (j <= TD5_TG_R12_FCROSS_CLEAR || j + 1 >= nl->count || j >= nspans) ok = 0;
            else if (tg_span_in_tunnel(j) || tg_span_in_bridge_run(j)) ok = 0;
            else if (tg_span_near_bridge(j, TD5_TG_XBRIDGE_CLEAR)) ok = 0;
            else if (tg_branches_enabled() && tg_span_in_fork_clear(j)) ok = 0;
            else if (tg_side_corridor_here(nl, j, side)) ok = 0;
        }
        if (!ok) continue;
        s_stat_cand++;
        tg_city_edge_frame(nl, c, side, e);
        reach = tg_net_march(e[0], e[2], e[6], e[7], want,
                             TD5_TG_R12_FCROSS_WIDTH * TD5_TG_LANE_WIDTH * 0.5,
                             TD5_TG_R12_FCROSS_CMIN, &why);
        if (why == 1) s_stat_road++;
        if (why == 2) s_stat_tjunc++;
        if (why == 3) s_stat_water++;
        if (reach < TD5_TG_R12_FCROSS_MIN) { s_stat_short++; continue; }
        a = tg_net_node(e[0], e[2], e[1], 0, c);
        bnode = tg_net_node(e[0] + e[6] * reach, e[2] + e[7] * reach,
                            tg_world_h(e[0] + e[6] * reach, e[2] + e[7] * reach),
                            why == 2 ? 1 : 2, -1);
        ed = tg_net_edge_new(a, bnode, TG_NE_COUNTRY,
                             TD5_TG_R12_FCROSS_WIDTH * (double)TD5_TG_LANE_WIDTH);
        if (!ed) return;
        ed->mouth_si = c; ed->mouth_left = side > 0.0;
        ed->mouth_lo = c; ed->mouth_hi = c + TD5_TG_R12_FCROSS_WIDTH - 1;
        ed->skew = 0.0; ed->reach = reach;
        tg_net_paint_edge(ed, TG_WO_STREET);
        tg_net_set_mouth(ed->mouth_lo, ed->mouth_hi, ed->mouth_left,
                         (int)(ed - s_edges), 0.0, reach);

        /* LOOP: wander on from the free end, bending toward a main-road
         * node 30..90 spans ahead, and rejoin it if the way is clear. */
        if (loops && why == 0) {
            const int target = c + 30 + (int)(tg_roll_hash_at(0x22040001u, c) % (unsigned)(TG_NET_LOOP_MAX - 30));
            double hx = e[6], hz = e[7];
            double x = ed->px[1], z = ed->pz[1];
            int np = 2, step, joined = 0;
            const double half = ed->width * 0.5;
            if (target + 1 >= nl->count || target >= nspans) continue;
            for (step = 0; step < 40 && np < TG_NET_POLY - 1; step++) {
                const TG_Node *t = &nl->v[target];
                double tx = t->x - x, tz = t->z - z, tl = sqrt(tx * tx + tz * tz);
                double nx, nz, nlen, px, pz, seg, wgt;
                int k, hit = 0, tt;
                if (tl < 1.0) break;
                tx /= tl; tz /= tl;
                /* segment length follows the remaining distance so the budget
                 * of TG_NET_POLY points always reaches the target; the bend
                 * toward it tightens as the lane closes in */
                seg = tl / (double)(TG_NET_POLY - np);
                if (seg < 4500.0) seg = 4500.0;
                if (seg > 14000.0) seg = 14000.0;
                wgt = (tl < 30000.0) ? 0.55 : 0.30;
                nx = hx * (1.0 - wgt) + tx * wgt; nz = hz * (1.0 - wgt) + tz * wgt;
                nlen = sqrt(nx * nx + nz * nz); if (nlen < 1e-6) break;
                hx = nx / nlen; hz = nz / nlen;
                px = x + hx * seg; pz = z + hz * seg;
                /* near a carriageway around the target: that is the rejoin */
                for (tt = target - 10; tt <= target + 10 && !joined; tt++) {
                    const TG_Node *tn;
                    double lat, along;
                    if (tt <= c + 12 || tt + 1 >= nl->count || tt >= nspans) continue;
                    tn = &nl->v[tt];
                    lat   = (px - tn->x) * tn->tz - (pz - tn->z) * tn->tx;
                    along = (px - tn->x) * tn->tx + (pz - tn->z) * tn->tz;
                    if (fabs(lat) < tg_carriageway_reach(nl, tt, lat >= 0 ? 1.0 : -1.0) + 2500.0
                        && fabs(along) < 1200.0
                        && !tg_span_in_bridge_run(tt) && !tg_span_in_tunnel(tt)) {
                        const double sgn = lat >= 0.0 ? 1.0 : -1.0;
                        double f[10];
                        tg_city_edge_frame(nl, tt, sgn, f);
                        ed->px[np] = f[0]; ed->pz[np] = f[2]; ed->py[np] = f[1]; np++;
                        joined = 1;
                        ed->rejoin_si = tt;
                        tg_net_set_mouth(tt, tt, sgn > 0.0, (int)(ed - s_edges), 0.0, seg);
                    }
                }
                if (joined) break;
                for (k = 1; k <= 3 && !hit; k++) {
                    const double qx = x + hx * seg * k / 3.0, qz = z + hz * seg * k / 3.0;
                    /* the lane's own straight paint ends at its free end */
                    const double ddx = qx - ed->px[1], ddz = qz - ed->pz[1];
                    const int own = (ddx * ddx + ddz * ddz) < (half + TG_NET_MARGIN + 1200.0) * (half + TG_NET_MARGIN + 1200.0);
                    if (tg_world_occ_near(qx, qz, half + TG_NET_MARGIN, TG_WO_ROAD | TG_WO_DRIVABLE)
                        || (!own && tg_world_occ_near(qx, qz, half + TG_NET_MARGIN, TG_WO_STREET))
                        || tg_world_is_water(qx, qz)
                        || tg_world_slope(qx, qz) >= TG_WORLD_STEEP_SLOPE) hit = 1;
                }
                if (hit) break;
                ed->px[np] = px; ed->pz[np] = pz; ed->py[np] = tg_world_h(px, pz); np++;
                x = px; z = pz;
            }
            if (joined) {
                int k;
                ed->npoly = np;
                s_nodes[ed->b].kind = 1;
                for (k = 1; k + 1 < np; k++) {
                    tg_world_occ_seg(ed->px[k], ed->pz[k], ed->px[k + 1], ed->pz[k + 1], half, TG_WO_STREET);
                    tg_world_conform_seg(ed->px[k], ed->pz[k], ed->py[k],
                                         ed->px[k + 1], ed->pz[k + 1], ed->py[k + 1],
                                         half + 600.0, 3000.0);
                }
            }
        }
    }
}

/* ------------------------------------------------------------- bypass -- */

/* [TOPOLOGY-FIRST] A long fork becomes a BYPASS: its corridor is a planned
 * street of the network rather than a half-sine bow. The lateral profile
 * (right of travel, magnitude from the corridor centre to the main centre
 * line) is the bow the fork wanted, clipped by what the world allows beside
 * each main node -- other streets, water, ground the road cannot climb --
 * and then rate-limited like the height profile so the corridor never bends
 * faster than TD5_TG_BRANCH_RATE. Both mouths are pinned to the ordinary
 * fork geometry so the type 8/11 spans are untouched. The world is conformed
 * under the corridor to the main road's y (rows take y from the main node). */
static void tg_net_bypass_plan(const TG_NodeList *nl, int nspans)
{
    static double mag[TD5_TG_BYPASS_MAXK], clr[TD5_TG_BYPASS_MAXK];
    int f, made = 0;
    if (!td5_env_flag_on("TD5RE_TG_NET_BYPASS")) return;
    for (f = 0; f < s_fork_count; f++) {
        TG_Fork *fk = &s_forks[f];
        const int L = fk->len;
        const double w  = nl->v[fk->F].width;
        const double fb = fk->fb;
        const double mouth = w * (1.0 - fb) * 0.5;            /* centre offset at the mouths */
        const double halfb = w * fb * 0.5;                    /* corridor half width */
        const double cap = TD5_TG_R8_LAT_MAX - halfb - 1500.0;
        const double rate = TD5_TG_BRANCH_RATE * (double)TD5_TG_SPAN_LENGTH;
        const unsigned int h = tg_roll_hash_at(0x22050001u, fk->F);
        double amp, ph, bowmax = 0.0, fs;
        int k, pct, a, b, e0, e1;
        TG_NetEdge *ed;
        if (fk->kind == TG_FORK_ISLAND || fk->kind == TG_FORK_AVENUE) continue;
        if (L < 24 || L + 1 > TD5_TG_BYPASS_MAXK) continue;
        if (fk->F + L + 1 >= nspans) continue;
        pct = td5_env_int("TD5RE_TG_NET_BYPASS_PCT", 70, 0, 100);
        if ((int)(h % 100u) >= pct) continue;
        /* Which side. LEFT corridors are OPT-IN (TD5RE_TG_NET_LEFT=1): the
         * generator, the emitters and the engine's fork decision are mirrored
         * for them (see fork_left_compute in td5_track.c), and the strip audit
         * passes, but a lane-assisted drive through seed 20260901's fork 609
         * ended with the car centred in a MAIN lane by the walker while
         * physically in the gore between the carriageways (WHEELS --lr,
         * geo rescue re-snapping it) -- the walker's sub-lane bookkeeping for
         * a low-lanes branch is not yet right. Parked for the branch rework
         * (docs/plans/AUTOTRACK_BRANCH_REWORK.md). */
        fs = (td5_env_flag_off("TD5RE_TG_NET_LEFT") && ((h >> 30) & 1u)) ? 1.0 : -1.0;
        amp = 6000.0 + (double)((h >> 8) % 14000u);           /* 6000..20000 */
        if (amp > cap) amp = cap;
        ph  = (double)((h >> 20) % 1000u) / 1000.0 * TD5_TG_PI;
        for (k = 0; k <= L; k++) {
            const int mb = fk->F + 1 + k;
            const TG_Node *n = &nl->v[mb];
            const double fr = (double)k / (double)L;
            const double shape = sin(fr * TD5_TG_PI) * (0.8 + 0.2 * sin(2.0 * TD5_TG_PI * fr + ph));
            double d, want = mouth + amp * (shape < 0.0 ? 0.0 : shape);
            /* what the world allows on the RIGHT of this node (lateral is
             * measured from the main centre line; the road edge is at w/2) */
            clr[k] = cap;
            for (d = w * 0.5 + halfb + TG_NET_STEP; d <= cap + halfb; d += TG_NET_STEP) {
                const double px = n->x + n->tz * fs * d, pz = n->z - n->tx * fs * d;
                if (tg_world_occ_near(px, pz, halfb + TG_NET_MARGIN, TG_WO_STREET) ||
                    tg_world_is_water(px, pz) ||
                    tg_world_slope(px, pz) >= TG_WORLD_STEEP_SLOPE) {
                    clr[k] = d - halfb - TG_NET_MARGIN - TG_NET_STEP;
                    break;
                }
            }
            if (clr[k] < mouth) clr[k] = mouth;
            mag[k] = (want < clr[k]) ? want : clr[k];
        }
        mag[0] = mouth; mag[L] = mouth;
        for (k = 1; k <= L; k++)  if (mag[k] > mag[k - 1] + rate) mag[k] = mag[k - 1] + rate;
        for (k = L - 1; k >= 0; k--) if (mag[k] > mag[k + 1] + rate) mag[k] = mag[k + 1] + rate;
        for (k = 0; k <= L; k++) if (mag[k] - mouth > bowmax) bowmax = mag[k] - mouth;
        if (bowmax < 2500.0) continue;                        /* nothing gained */

        for (k = 0; k <= L; k++) s_bypass_lat[f][k] = fs * mag[k];
        fk->kind = TG_FORK_BYPASS;
        fk->side = (fs > 0.0) ? 1 : -1;
        fk->sep  = 1.0;                                       /* never an avenue */
        /* paint, conform, and register the corridor as a drivable edge */
        {
            const TG_Node *n0 = &nl->v[fk->F + 1];
            a = tg_net_node(n0->x + n0->tz * fs * mag[0], n0->z - n0->tx * fs * mag[0], n0->y, 0, fk->F);
            {
                const TG_Node *n1 = &nl->v[fk->F + 1 + L];
                b = tg_net_node(n1->x + n1->tz * fs * mag[L], n1->z - n1->tx * fs * mag[L], n1->y, 0, fk->R);
            }
            ed = tg_net_edge_new(a, b, TG_NE_BYPASS, w * fb);
            if (!ed) return;
            ed->drivable = 1; ed->mouth_si = fk->F; ed->mouth_left = (fs > 0.0);
            ed->rejoin_si = fk->R;
            ed->npoly = 0;
            for (k = 0; k <= L; k++) {
                const TG_Node *n = &nl->v[fk->F + 1 + k];
                const double px = n->x + n->tz * fs * mag[k], pz = n->z - n->tx * fs * mag[k];
                if (k > 0) {
                    const TG_Node *m = &nl->v[fk->F + k];
                    tg_world_occ_seg(m->x + m->tz * fs * mag[k - 1], m->z - m->tx * fs * mag[k - 1], px, pz,
                                     halfb + 600.0, TG_WO_DRIVABLE);
                    tg_world_conform_seg(m->x + m->tz * fs * mag[k - 1], m->z - m->tx * fs * mag[k - 1], m->y,
                                         px, pz, n->y, halfb + TD5_TG_ROAD_BED_VERGE, 4500.0);
                }
                e0 = (L > TG_NET_POLY - 1) ? (L / (TG_NET_POLY - 1) + 1) : 1;
                e1 = (k % e0 == 0) || (k == L);
                if (e1 && ed->npoly < TG_NET_POLY) {
                    ed->px[ed->npoly] = px; ed->pz[ed->npoly] = pz; ed->py[ed->npoly] = n->y;
                    ed->npoly++;
                }
            }
        }
        made++;
        TD5_LOG_I(LOG_TAG, "trackgen: [NET] fork %d -> BYPASS %s F=%d L=%d R=%d bow max %.0f "
                  "(wanted %.0f, world cap %.0f)", f, fs > 0.0 ? "LEFT" : "right",
                  fk->F, L, fk->R, bowmax, amp, cap);
    }
    (void)made;
}

/* Underpass crossings (a road passing OVER the main road): registered so the
 * raster and the audit know the tarmac is there; no mouth, no frontage. */
static void tg_net_underpasses(const TG_NodeList *nl, int nspans)
{
    int si;
    for (si = 1; si < nspans && si + 1 < nl->count; si++) {
        double l[10], r[10];
        int a, b;
        TG_NetEdge *ed;
        if (tg_underpass_span(si) != si) continue;
        tg_city_edge_frame(nl, si,  1.0, l);
        tg_city_edge_frame(nl, si, -1.0, r);
        a = tg_net_node(l[0] + l[6] * 12000.0, l[2] + l[7] * 12000.0, nl->v[si].y, 2, -1);
        b = tg_net_node(r[0] + r[6] * 12000.0, r[2] + r[7] * 12000.0, nl->v[si].y, 2, -1);
        ed = tg_net_edge_new(a, b, TG_NE_UNDERPASS, 2.0 * TD5_TG_LANE_WIDTH);
        if (!ed) return;
        ed->mouth_si = si;
        tg_world_occ_seg(l[0] + l[6] * 2500.0, l[2] + l[7] * 2500.0, s_nodes[a].x, s_nodes[a].z, ed->width * 0.5, TG_WO_STREET);
        tg_world_occ_seg(r[0] + r[6] * 2500.0, r[2] + r[7] * 2500.0, s_nodes[b].x, s_nodes[b].z, ed->width * 0.5, TG_WO_STREET);
    }
}

/* ==================================================================== *
 * SECTION: GEO REAL STREETS                                            *
 * ==================================================================== *
 *
 * [GEO PHASE 5 2026-09-30] The same network, sourced from the real world.
 *
 * The synthetic generators above PROPOSE openings (the facade rhythm, the R12
 * period) and validate them on the occupancy raster. On a geo build the
 * openings already exist: ROADS.JSON holds the real OSM ways in the route's own
 * frame, so the job inverts to ACCEPTING what is there -- which means the
 * rules the synthetic path could simply obey now have to be tested and, where
 * reality breaks them, the street must be DROPPED AND COUNTED rather than
 * silently bent into shape.
 *
 * WHAT A REAL ROAD BECOMES
 *   crossing the route      two mouths at the same span run, one per side
 *                           (which is what the emitter already draws as a
 *                           crossroads -- tg_city_emit_crossstreet loops both
 *                           sides and the quads plus the gap between them make
 *                           the junction)
 *   ending at the route     one mouth, the side it arrives from (a T)
 *   grade separated         an UNDERPASS edge: painted, registered, no mouth
 *                           and no frontage, exactly like tg_net_underpasses
 *   running ALONG the route  nothing: it IS the road we are driving on
 *
 * NON-90-DEGREE MOUTHS ARE FIRST CLASS, not an edge case. La Plata is a
 * 45-degree diagonal grid, so the synthetic TD5_TG_DIAG_MAX_DEG ceiling of 28
 * would refuse the streets that make the place recognisable. The geo cap is
 * TD5RE_GEO_NET_SKEW_MAX_DEG (default 65) measured from the outward normal, and
 * it does double duty: a bearing within 25 degrees of the road's own tangent is
 * not a side street, it is the route, so one knob rejects both a fold-back and
 * the route's own carriageway.
 *
 * WHY THE ARM IS STRAIGHT. The mouth table carries (skew, reach) -- a single
 * bearing -- because that is what the ~90 junction consumers draw: one skewed
 * quad per span of the run. So the registered edge is the STRAIGHT arm along
 * the real road's bearing at the junction, and its length is the distance the
 * real road stays inside that arm (tg_geo_straight_run). This is not a
 * compromise at this scale: TD5_TG_R8_XSTREET_MAX caps a reach at 21 000 units
 * = 49 m, and a real city street is straight over 49 m. Registering the real
 * polyline instead would put geometry in NETWORK.JSON that nothing draws, and
 * tg_network_audit.py would rightly fail on it.
 *
 * ORDER IS THE TIE-BREAK, AND IT IS DETERMINISTIC. Two real streets can want
 * the same (span, side) -- the span grid is 3.49 m and a city block is not.
 * Arms are sorted by (highway class, lanes, road index, span, side), all read
 * from the file, so the winner is a property of the data and two geo builds
 * produce identical level files. No tg_rand/tg_frand/tg_range is drawn here
 * (the standing rule at td5_trackgen_internal.h:1290-1296).
 *
 * PLANARITY IS STILL BY CONSTRUCTION. Every accepted arm is painted into the
 * occupancy raster before the next is marched, so tg_net_march stops the next
 * one short (why == 2, a T-junction) instead of letting the two cross. That is
 * the same mechanism the synthetic path relies on, unchanged.
 */

#define TG_GEO_SAMPLE        3000.0  /* step along a real road, world units  */
#define TG_GEO_COARSE        16      /* stride of the nearest-node first pass */
#define TG_GEO_MAX_ARMS      4096
#define TG_GEO_MAX_HITS      32      /* junctions one way may make            */
#define TG_GEO_SPAN_JUMP     4       /* span continuity across a crossing     */
#define TG_GEO_MOUTH_SPANS   12      /* widest frontage run a real road gets  */
#define TG_GEO_SKEWW_COS_MIN 0.57    /* [1014 B] skew width correction stops at 55 deg: past it the
                                      * sheared quad is a wedge, not a wider street */
#define TG_GEO_AVENUE_LANES  4       /* lanes at which a street is an avenue  */
/* [ROUND 1009 item 6] The cap was 65, which is 25 degrees off the route's own
 * tangent. That is the right ceiling for a GRID, where a side street meets the
 * road square or at the 45 of a La Plata diagonal, and anything steeper is a
 * fold-back. It is the wrong ceiling AT A PLAZA, and Mariano's route passes
 * three of them: the route curves hard around the square, so a perfectly
 * ordinary street radiating off it is measured against a tangent that has
 * already swung, and reads as 66..78 degrees. The ten skew refusals in his race
 * log cluster exactly there -- spans 650/667/672, 800/806/808, 1400/1402 -- and
 * each one is a street a driver plainly sees opening.
 *
 * 80 accepts nine of those ten and still refuses the tenth (span 1402, 89
 * degrees), which is the case the cap exists for: 89 from the normal is 1 degree
 * off the tangent, a way running ALONGSIDE the route, and mouthing it would draw
 * a second carriageway down the kerb. The margin between 78 (the steepest real
 * street) and 89 (the parallel way) is what makes 80 a defensible line rather
 * than a tuned one. */
#define TG_GEO_SKEW_MAX_DEG  80      /* default TD5RE_GEO_NET_SKEW_MAX_DEG    */
#define TG_GEO_MARCH_COS_MIN 0.35    /* floor on cos(skew) for the own-paint
                                      * stand-off, so a steep diagonal is not
                                      * killed by the main road's own paint  */
/* [ROUND 1009 item 6] How deep a real street is drawn.
 *
 * tg_city_crossst_reach is the SYNTHETIC answer -- sidewalk plus three facade
 * blocks, 19500 units (45 m) for La Plata's biome -- and on a synthetic track it
 * is also the honest answer, because the generator invented the street and may
 * stop it where it likes. On a geo track the street is a real road that
 * demonstrably continues, and tg_geo_straight_run has already measured how far.
 * Capping that at 45 m is what Mariano is looking at: the audit
 * (scripts/geo_road_audit.py) puts 31 of the 51 accepted arms at exactly 19500,
 * so for sixty per cent of the streets on his route the stub ends mid-block for
 * no reason in the data.
 *
 * The ceiling that IS real is the FLAT VERGE, and it is tg_verge_reach()
 * (TD5_TG_GROUND_WIDTH, 24000 units, 56 m) rather than the drawn ground's own
 * outer edge at tg_far_reach() (30000). The street quad takes its height from
 * tg_xstreet_drop, which is calibrated to exactly that number: it ramps the
 * quad down by TD5_TG_GROUND_DROP (70 units) over the verge and then SATURATES.
 * Past the verge the far band stops being flat and sinks toward
 * tg_track_min_y - TD5_TG_FAR_SINK_AT, so a street drawn out there would hang
 * level while the ground fell away under it. That is not a hypothetical: R17
 * CITY item 1 is the same defect found on the park lawn, which had borrowed
 * this very reach and "read as a giant floating green side street".
 *
 * So the depth stops where the drop model it is drawn with stops. 24000 against
 * the old 19500 is a 23 per cent deeper street, every unit of it on ground that
 * is flat by construction. Reaching the full 30000 needs the street to follow
 * the sinking band, which is a different change in a different file.
 *
 * This only ever RAISES a cap, and only when the real road's own straight run
 * asks for it; tg_net_march still cuts the arm at the first street, carriageway
 * or water it meets, so planarity is unchanged. */
#define TG_GEO_DEPTH_MAX     (tg_verge_reach())
#define TG_GEO_ALONG_NUM     6       /* a way is the route when 6/10 of its   */
#define TG_GEO_ALONG_DEN     10      /* samples run along it                  */
/* [ROUND 1008b] Shared carriageway. See tg_geo_depart_hit. A sample is ON the
 * route when it is nearly parallel to the tangent AND inside the carriageway;
 * the run has to last TG_GEO_DEPART_RUN samples (3 * 3000 = 9000 units, 21 m)
 * before either of its ends counts as a junction, so a street that merely
 * clips the kerb at a shallow angle invents nothing. */
#define TG_GEO_DEPART_DEG    20      /* |angle| to the tangent that is "along" */
#define TG_GEO_DEPART_LAT    1500.0  /* ... inside half-width plus this        */
#define TG_GEO_DEPART_RUN    3       /* samples of shared carriageway needed   */

typedef struct {
    int    si, left, lanes, road, klass;
    int    surface;         /* TD5_GEO_SURF_* of the real way this arm is   */
    double skew, want;
    double shift;           /* [1014 B] origin offset past the race kerb     */
} TG_GeoArm;

typedef struct {
    int    si;
    double x, z;            /* the junction, on the route centre line        */
    double dx, dz;          /* unit along the real road at the junction      */
    int    kfwd, sfwd;      /* vertex + index step for the +(dx,dz) arm      */
    int    kbwd, sbwd;      /* ... for the -(dx,dz) arm; kbwd < 0 = no arm   */
} TG_GeoHit;

static TG_GeoArm s_garm[TG_GEO_MAX_ARMS];
static int       s_gna;
static double    s_gcd[TD5_TG_MAX_SPANS / TG_GEO_COARSE + 2];

static struct {
    long ways, inbox, route, cand, street, avenue, cont, under, depart, beyond;
    long d_grid, d_struct, d_biome, d_park, d_corridor, d_skew,
         d_short, d_taken, d_fold, d_full, d_under;
    long fan_cut;           /* [1015 B] mouth spans cut: their normal is off the street's ray */
    long why_road, why_street, why_water;
} s_gs;

/* [ROUND 1008 item 4] Per-arm drop ledger.
 *
 * The census counts WHY arms were refused but not WHERE, and the two questions
 * have different answers: on La Plata 53 of 94 arms are accepted, which reads
 * as decent coverage until the built mouths are diffed against the real OSM
 * junctions -- 35 junction spans built, 21 real junctions with nothing within
 * 6 spans, and the missing ones come in RUNS (spans 672..781, 1296..1347)
 * rather than scattered. A run means one rule is firing repeatedly in one
 * place, which a total cannot show. Each refusal now records its span and side
 * so the ledger below names the streets Mariano could not see. Capped; the
 * overflow is still counted by the census. */
#define TG_GEO_DROP_MAX 96
static struct { short si; unsigned char left, why; float arg; } s_gdrop[TG_GEO_DROP_MAX];
static int s_gdropn;

enum {
    TG_GD_GRID = 0, TG_GD_STRUCT, TG_GD_BIOME, TG_GD_PARK, TG_GD_CORRIDOR,
    TG_GD_SKEW, TG_GD_SHORT, TG_GD_TAKEN, TG_GD_FOLD, TG_GD_FULL, TG_GD_N
};
static const char *const k_gd_name[TG_GD_N] = {
    "grid", "struct", "biome", "park", "corridor",
    "skew", "short", "taken", "fold", "table-full"
};

/* `arg` is the number the rule actually compared: degrees of skew for SKEW,
 * world units of reach for SHORT and FOLD, 0 for the rest. Without it a SKEW
 * line says "too oblique" but not "by how much", and the cap cannot be argued
 * about from the log. */
static void tg_geo_drop_note(int si, int left, int why, double arg)
{
    if (td5_env_flag_off("TD5RE_GEO_NET_DIAG"))
        TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO DROP] si %d %s %s %.0f", si,
                  left ? "left" : "right", k_gd_name[why], arg);
    if (s_gdropn >= TG_GEO_DROP_MAX) return;
    s_gdrop[s_gdropn].si   = (short)si;
    s_gdrop[s_gdropn].left = (unsigned char)(left ? 1 : 0);
    s_gdrop[s_gdropn].why  = (unsigned char)why;
    s_gdrop[s_gdropn].arg  = (float)arg;
    s_gdropn++;
}

/* [ROUND 1009 item 6] The skew ceiling, in RADIANS, from ONE place.
 *
 * tg_geo_road_hits (which decides what is a crossing and what is the route) read
 * the TG_GEO_SKEW_MAX_DEG literal while tg_net_geo_streets (which filters the
 * arms) read the TD5RE_GEO_NET_SKEW_MAX_DEG env knob, so turning the knob moved
 * the filter and left the detector where it was -- an A/B on it could only ever
 * measure half the mechanism. Both now call this. */
static double tg_geo_skew_max(void)
{
    return (double)td5_env_int("TD5RE_GEO_NET_SKEW_MAX_DEG",
                               TG_GEO_SKEW_MAX_DEG, 10, 85) * TD5_TG_PI / 180.0;
}

/* Nearest main-route node to (x,z). Coarse stride first, then a full refine
 * around EVERY coarse sample that could still hold the answer: moving
 * TG_GEO_COARSE nodes changes the distance to a fixed point by at most
 * COARSE * span_length, so that slack makes the refine exhaustive. Refining
 * only around the coarse best would be wrong on a real route, which is not
 * axis-monotone and can fold back within a few hundred metres of itself -- the
 * wrong lobe would put a street's mouth on the far span. */
static int tg_geo_nearest(const TG_NodeList *nl, int nspans, double x, double z)
{
    const double slack = (double)TG_GEO_COARSE * (double)TD5_TG_SPAN_LENGTH;
    double cmin = 1e300, bd = 1e300;
    int i, c = 0, best = -1;

    for (i = 0; i <= nspans; i += TG_GEO_COARSE, c++) {
        const double dx = x - nl->v[i].x, dz = z - nl->v[i].z;
        s_gcd[c] = sqrt(dx * dx + dz * dz);
        if (s_gcd[c] < cmin) cmin = s_gcd[c];
    }
    c = 0;
    for (i = 0; i <= nspans; i += TG_GEO_COARSE, c++) {
        int j, lo, hi;
        if (s_gcd[c] > cmin + slack) continue;
        lo = i - TG_GEO_COARSE; if (lo < 0) lo = 0;
        hi = i + TG_GEO_COARSE; if (hi > nspans) hi = nspans;
        for (j = lo; j <= hi; j++) {
            const double dx = x - nl->v[j].x, dz = z - nl->v[j].z;
            const double d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = j; }
        }
    }
    return best;
}

/* Signed lateral (positive = LEFT of travel, the sign tg_city_edge_frame takes
 * as sg > 0) and along-track offset of (x,z) in node `ni`'s frame. Same
 * expression as the country-loop rejoin test above, so the two cannot disagree
 * about which side a point is on. */
static void tg_geo_lat(const TG_NodeList *nl, int ni, double x, double z,
                       double *lat, double *along)
{
    const TG_Node *n = &nl->v[ni];
    *lat   = (x - n->x) * n->tz - (z - n->z) * n->tx;
    *along = (x - n->x) * n->tx + (z - n->z) * n->tz;
}

/* Rotation from unit (ux,uz) to unit (ax,az), in the sense tg_block_rot2
 * applies (ox = ux*cos - uz*sin, oz = ux*sin + uz*cos), so feeding the result
 * back through it reproduces (ax,az) exactly. */
static double tg_geo_skew_of(double ux, double uz, double ax, double az)
{
    return atan2(ux * az - uz * ax, ux * ax + uz * az);
}

/* [ROUND 1008b] SHARED CARRIAGEWAY -- the third kind of junction.
 *
 * The route is a real driving route, so it RIDES ON real streets: on La Plata it
 * runs along Calle 14 for spans 743..750, along Avenida 19 for 953..971 and
 * along Calle 20 for 1015..1037, then turns off. Where it turns off, the street
 * carries straight on -- a side street a driver sees opening -- but neither of
 * the two hit rules above can see it:
 *
 *   - the CROSSING rule needs the lateral offset to change sign at a sample
 *     whose |sin| to the tangent is at least TG_GEO_SKEW_MAX_DEG. Inside a
 *     shared run the sign does flip (the route wanders across the way by a
 *     metre or two) but |sin| there is ~0.00, so the sample is refused -- and
 *     rightly: that is not a crossing.
 *   - the ENDPOINT rule only tests vertex 0 and vertex count-1. The point where
 *     the route leaves is an INTERIOR vertex of the way, because the way runs on
 *     past it.
 *
 * So the whole junction was refused UPSTREAM of every drop rule and the ledger
 * never saw it. This function registers the run's end as a one-arm T: the
 * junction is the way vertex `jv` where route and way part company, and the arm
 * is the way's own geometry in index direction `step`, which is the half that
 * leaves the route. The other half IS the route, so there is no second arm --
 * same shape as a real T, and `kbwd < 0` keeps it out of tg_net_geo_underpasses.
 *
 * NOTHING IS INVENTED: `jv` is an OSM vertex of a way that demonstrably runs
 * inside the carriageway for TG_GEO_DEPART_RUN samples and then does not, and
 * the arm is read off that way's own following vertices. The vertex is also
 * re-tested against the carriageway here, so a run whose bounding vertex sits a
 * block away (the route curved off mid-segment) is refused rather than mouthed
 * in the wrong place. */
static int tg_geo_depart_hit(const TG_NodeList *nl, int nspans,
                             const TD5_GeoRoad *rd, int jv, int step,
                             TG_GeoHit *out, int nout, int maxout)
{
    double jx, jz, vx, vz, len, lat, along;
    int ni;
    if (nout >= maxout) return nout;
    if (jv + step < 0 || jv + step >= rd->count) return nout;
    if (!td5_geo_roads_point(rd, jv, &jx, &jz)) return nout;
    if (!td5_geo_roads_point(rd, jv + step, &vx, &vz)) return nout;
    ni = tg_geo_nearest(nl, nspans, jx, jz);
    if (ni < 0) return nout;
    tg_geo_lat(nl, ni, jx, jz, &lat, &along);
    if (fabs(lat) > nl->v[ni].width * 0.5 + TG_GEO_DEPART_LAT) return nout;
    len = sqrt((vx - jx) * (vx - jx) + (vz - jz) * (vz - jz));
    if (len < 1.0) return nout;          /* the loader drops repeats, so this
                                          * only guards a malformed cache */
    {
        TG_GeoHit *h = &out[nout++];
        h->x = jx; h->z = jz;
        h->dx = (vx - jx) / len; h->dz = (vz - jz) / len;
        h->kfwd = jv + step; h->sfwd = step;
        h->kbwd = -1;        h->sbwd = 0;
        h->si = ni;
    }
    s_gs.depart++;
    return nout;
}

/* Junctions between one real way and the route. Returns the count, or -1 when
 * the way IS the route (see TG_GEO_ALONG_*): those must produce nothing, and
 * reporting them as skew rejections would bury the real census under the
 * handful of ways the conditioner routed along. */
static int tg_geo_road_hits(const TG_NodeList *nl, int nspans,
                            const TD5_GeoRoad *rd, TG_GeoHit *out, int maxout)
{
    /* [ROUND 1009 item 6] THE CAP IS MEASURED FROM THE NORMAL; `sn` IS MEASURED
     * FROM THE TANGENT. This line used to read sin(SKEW_MAX_DEG), which bounds
     * the wrong angle, and the two are only equal at 45 degrees.
     *
     *   sn = |sin(way, tangent)| = cos(skew-from-normal)
     *   so "skew <= cap" is  sn >= cos(cap),  not  sn >= sin(cap)
     *
     * With sin(65) = 0.906 the crossing rule demanded sn >= 0.906, i.e. a street
     * within 25 degrees of PERPENDICULAR -- a tighter ceiling than the synthetic
     * TD5_TG_DIAG_MAX_DEG of 28 that this whole section was written to escape.
     * So La Plata's 45-degree diagonals, the streets the header calls "the ones
     * that make the place recognisable", were never detected as crossings at
     * all; and because the same constant splits crossing from along, every one
     * of them was then counted toward "this way IS the route" and discarded.
     *
     * Measured on Mariano's route (ROADS.JSON, 2291 ways, replicating this loop
     * offline): 13 crossing hits and 90 ways called the route as shipped, versus
     * 59 hits and 63 ways with cos. The median skew of the 46 recovered hits is
     * 44 degrees. That is the diagonal grid arriving.
     *
     * TD5RE_GEO_NET_SKEW_TANGENT=1 restores the sin() comparison, so the whole
     * mechanism can be A/B'd on one exe (the depth and cap changes have their
     * own knobs; without this one the dominant change would be the only part of
     * the round with no before picture). */
    const double sn_min = td5_env_flag_off("TD5RE_GEO_NET_SKEW_TANGENT")
                        ? sin(tg_geo_skew_max()) : cos(tg_geo_skew_max());
    /* [ROUND 1008b] shared-carriageway run state; see tg_geo_depart_hit */
    const double dep_sin = sin((double)TG_GEO_DEPART_DEG * TD5_TG_PI / 180.0);
    const int    dep_on  = td5_env_flag_on("TD5RE_GEO_NET_DEPART");
    int pon = 0, prun = 0, en_jv = -1, en_done = 0, pjv = -1;
    int k, nout = 0, first = 1, pni = -1;
    long nsamp = 0, nalong = 0;
    double plat = 0.0;

    for (k = 0; k + 1 < rd->count; k++) {
        double ax, az, bx, bz, len, ux, uz, t;
        if (!td5_geo_roads_point(rd, k, &ax, &az)) break;
        if (!td5_geo_roads_point(rd, k + 1, &bx, &bz)) break;
        len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
        if (len < 1.0) continue;
        ux = (bx - ax) / len; uz = (bz - az) / len;
        for (t = 0.0; ; t += TG_GEO_SAMPLE) {
            double sx, sz, lat, along;
            int ni;
            if (t > len) t = len;
            sx = ax + ux * t; sz = az + uz * t;
            ni = tg_geo_nearest(nl, nspans, sx, sz);
            if (ni >= 0) {
                const TG_Node *n = &nl->v[ni];
                /* |sin| between the way and the route tangent: both unit, so
                 * the cross product IS the sine. */
                const double sn = fabs(ux * n->tz - uz * n->tx);
                tg_geo_lat(nl, ni, sx, sz, &lat, &along);
                nsamp++;
                if (sn < sn_min
                    && fabs(lat) < n->width * 0.5 + rd->width * 0.5 + 1500.0)
                    nalong++;
                if (!first && pni >= 0 && sn >= sn_min
                    && (ni - pni) <= TG_GEO_SPAN_JUMP
                    && (pni - ni) <= TG_GEO_SPAN_JUMP
                    && ((plat < 0.0) != (lat < 0.0))
                    && nout < maxout) {
                    /* Where the sign flipped, interpolated on |lat| so the
                     * junction lands on the centre line rather than on
                     * whichever sample happened to be nearer it. */
                    const double f = fabs(plat) / (fabs(plat) + fabs(lat) + 1e-9);
                    const double t0 = t - TG_GEO_SAMPLE;
                    const double tj = (t0 < 0.0 ? 0.0 : t0)
                                    + f * (t - (t0 < 0.0 ? 0.0 : t0));
                    TG_GeoHit *h = &out[nout++];
                    h->x = ax + ux * tj; h->z = az + uz * tj;
                    h->dx = ux; h->dz = uz;
                    h->kfwd = k + 1; h->sfwd =  1;
                    h->kbwd = k;     h->sbwd = -1;
                    h->si = tg_geo_nearest(nl, nspans, h->x, h->z);
                    if (h->si < 0) nout--;
                }
                /* Shared-carriageway run: the junction is the way VERTEX that
                 * bounds the run, so the mouth lands on the real OSM node
                 * rather than up to half a sample step off it. `en_jv >= 1`
                 * suppresses a run that starts at vertex 0 -- there the way has
                 * no geometry behind the junction, so there is no arm. */
                if (dep_on) {
                    const int jv = (t <= len * 0.5) ? k : k + 1;
                    const int on = (sn < dep_sin
                                    && fabs(lat) < n->width * 0.5 + TG_GEO_DEPART_LAT);
                    if (on && !pon) { prun = 1; en_jv = jv; en_done = 0; }
                    else if (on) {
                        if (++prun >= TG_GEO_DEPART_RUN && !en_done) {
                            en_done = 1;
                            if (en_jv >= 1)
                                nout = tg_geo_depart_hit(nl, nspans, rd, en_jv,
                                                         -1, out, nout, maxout);
                        }
                    } else if (pon) {
                        if (prun >= TG_GEO_DEPART_RUN && pjv >= 0)
                            nout = tg_geo_depart_hit(nl, nspans, rd, pjv,
                                                     1, out, nout, maxout);
                        prun = 0; en_done = 1;
                    }
                    pon = on; pjv = jv;
                }
                pni = ni; plat = lat; first = 0;
            }
            if (t >= len) break;
        }
    }

    /* A way the conditioner routed along is the main carriageway itself. */
    if (nsamp > 0 && nalong * TG_GEO_ALONG_DEN >= nsamp * TG_GEO_ALONG_NUM)
        return -1;

    /* Both ends: a way that STOPS at the route is a T, one arm only. Tested
     * against the carriageway the engine actually has there (the same
     * authority the R8 clamp and the on-road guard use) plus one span of
     * slack, because the OSM node sits where the two centre lines meet and the
     * route was resampled by chord. */
    for (k = 0; k < 2 && nout < maxout; k++) {
        const int m  = k ? rd->count - 1 : 0;
        const int m2 = k ? rd->count - 2 : 1;
        double ex, ez, nx, nz, len, lat, along;
        int ni;
        if (!td5_geo_roads_point(rd, m, &ex, &ez)) continue;
        if (!td5_geo_roads_point(rd, m2, &nx, &nz)) continue;
        ni = tg_geo_nearest(nl, nspans, ex, ez);
        if (ni < 0) continue;
        tg_geo_lat(nl, ni, ex, ez, &lat, &along);
        if (fabs(lat) > tg_carriageway_reach(nl, ni, lat >= 0.0 ? 1.0 : -1.0)
                        + (double)TD5_TG_SPAN_LENGTH) continue;
        if (fabs(along) > (double)TD5_TG_SPAN_LENGTH) continue;
        len = sqrt((nx - ex) * (nx - ex) + (nz - ez) * (nz - ez));
        if (len < 1.0) continue;
        {
            TG_GeoHit *h = &out[nout++];
            h->x = ex; h->z = ez;
            h->dx = (nx - ex) / len; h->dz = (nz - ez) / len;
            h->kfwd = m2; h->sfwd = k ? -1 : 1;
            h->kbwd = -1; h->sbwd = 0;
            h->si = ni;
        }
    }
    return nout;
}

/* How far the real way stays inside the STRAIGHT arm the mouth table can
 * express. Walks the way's own vertices from `k0` in index direction `step`,
 * measuring each against the ray from the junction along (dx,dz), and stops at
 * the first vertex more than half the street width off it -- that is the point
 * where the drawn quad and the real road would part company. */
static double tg_geo_straight_run(const TD5_GeoRoad *rd, int k0, int step,
                                  double jx, double jz, double dx, double dz)
{
    const double halfw = rd->width * 0.5;
    double run = 0.0;
    int k;
    if (step == 0) return 0.0;
    for (k = k0; k >= 0 && k < rd->count; k += step) {
        double x, z, along, off;
        if (!td5_geo_roads_point(rd, k, &x, &z)) break;
        along = (x - jx) * dx + (z - jz) * dz;
        off   = fabs((x - jx) * dz - (z - jz) * dx);
        if (along <= run) continue;               /* behind, or no progress */
        if (off > halfw) break;                   /* the real road bent away */
        run = along;
    }
    return run;
}

static int tg_geo_arm_cmp(const void *pa, const void *pb)
{
    const TG_GeoArm *a = (const TG_GeoArm *)pa, *b = (const TG_GeoArm *)pb;
    if (a->klass != b->klass) return b->klass - a->klass;   /* bigger first */
    if (a->lanes != b->lanes) return b->lanes - a->lanes;
    if (a->road  != b->road)  return a->road  - b->road;
    if (a->si    != b->si)    return a->si    - b->si;
    return a->left - b->left;
}

/* One arm of one junction: the side it leaves on, its bearing, and the straight
 * run the real road offers. Pushed as a candidate; nothing is validated
 * against the raster yet, because the order arms are PLACED in decides which of
 * two contenders survives. */
static void tg_geo_arm_push(const TG_NodeList *nl, const TG_GeoHit *h,
                            const TD5_GeoRoad *rd, int ridx,
                            double dx, double dz, int k0, int step,
                            double skewmax)
{
    double e[10], skew, run, kerb, fshift;
    int left, si_m;
    const TG_Biome *b;

    /* Which kerb: the outward normal of the LEFT side, dotted with the arm. */
    tg_city_edge_frame(nl, h->si, 1.0, e);
    left = (dx * e[6] + dz * e[7]) > 0.0;
    if (!left) tg_city_edge_frame(nl, h->si, -1.0, e);

    skew = tg_geo_skew_of(e[6], e[7], dx, dz);
    s_gs.cand++;
    if (td5_env_flag_off("TD5RE_GEO_NET_DIAG"))
        TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO ARM] way %d (lanes %d) si %d %s junction (%.0f,%.0f) "
                  "dir (%.2f,%.2f) skew %.0f deg k0 %d step %d%s",
                  ridx, rd->lanes, h->si, left ? "left" : "right", h->x, h->z, dx, dz,
                  skew * 180.0 / TD5_TG_PI, k0, step, h->kbwd < 0 ? " T" : "");
    if (fabs(skew) > skewmax) {
        s_gs.d_skew++; tg_geo_drop_note(h->si, left, TG_GD_SKEW, fabs(skew) * 180.0 / TD5_TG_PI); return;
    }

    /* The run is measured from the JUNCTION (so `off` is the true offset from
     * the real road's bearing) and then shortened to start at the kerb, which
     * is where the drawn quad starts. */
    run  = tg_geo_straight_run(rd, k0, step, h->x, h->z, dx, dz);
    /* [1014 B item 8] the street's origin is the outermost tarmac on its side:
     * on a divided avenue that is the FAR carriageway's kerb, not the race one.
     *
     * And the MOUTH SPAN moves with it. The hit is found at the junction on the
     * OTHER carriageway's centre line (a T) or on the race one, but a street
     * crossing at 45 degrees reaches the far kerb a whole carriageway further
     * ALONG the road than it was found, so the span that owns the mouth is the
     * one nearest where the street's centre line meets the far kerb -- measured
     * on this arm's own bearing, not guessed. Without this the far half of a
     * street was drawn 2300 units (three spans) off its real line. */
    si_m   = h->si;
    fshift = tg_net_far_shift(nl, si_m, left ? 1.0 : -1.0);
    if (fshift > 0.0 && s_net_nspans > 2) {
        const double sgn = left ? 1.0 : -1.0;
        const TG_Node *nn = &nl->v[si_m];
        const double dl = dx * nn->tz - dz * nn->tx;       /* lateral share of the arm */
        if (fabs(dl) > 0.2) {
            double latj, alj, t, jx, jz;
            int ni;
            tg_geo_lat(nl, si_m, h->x, h->z, &latj, &alj);
            t = (sgn * (tg_road_half_width(nl, si_m) + fshift) - latj) / dl;
            if (t > -3.0 * (double)TD5_TG_SPAN_LENGTH * 4.0 && t < 6.0 * (double)TD5_TG_SPAN_LENGTH * 4.0) {
                jx = h->x + dx * t; jz = h->z + dz * t;
                ni = tg_geo_nearest(nl, s_net_nspans, jx, jz);
                if (ni > 0 && ni + 1 < nl->count && ni != si_m) {
                    const double f2 = tg_net_far_shift(nl, ni, sgn);
                    if (f2 > 0.0) { si_m = ni; fshift = f2; }
                }
            }
        }
        if (si_m != h->si) {
            tg_city_edge_frame(nl, si_m, left ? 1.0 : -1.0, e);
            skew = tg_geo_skew_of(e[6], e[7], dx, dz);
        }
    }
    kerb = (e[0] + e[6] * fshift - h->x) * dx + (e[2] + e[7] * fshift - h->z) * dz;
    if (kerb < 0.0) kerb = 0.0;
    run -= kerb;
    if (td5_env_flag_off("TD5RE_GEO_NET_DIAG"))
        TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO ARM]   way %d si %d->%d %s run-from-kerb %.0f (kerb %.0f, shift %.0f)",
                  ridx, h->si, si_m, left ? "left" : "right", run, kerb, fshift);
    if (run < TD5_TG_R8_CLAMP_MIN) {
        s_gs.d_short++; tg_geo_drop_note(si_m, left, TG_GD_SHORT, run); return;
    }

    b = &k_biomes[tg_scenery_biome_index(si_m)];
    {
        double cap = tg_city_crossst_reach(b, tg_city_sidewalk_w(b));
        /* [ROUND 1009 item 6] see TG_GEO_DEPTH_MAX. The real road's own
         * straight run is the authority; the facade-block reach is only a
         * floor on how deep a geo street may go. */
        if (td5_env_flag_on("TD5RE_GEO_NET_DEPTH") && cap < TG_GEO_DEPTH_MAX)
            cap = TG_GEO_DEPTH_MAX;
        if (run > cap) run = cap;
    }
    if (s_gna >= TG_GEO_MAX_ARMS) { s_gs.d_full++; return; }
    {
        TG_GeoArm *a = &s_garm[s_gna++];
        a->si = si_m; a->left = left; a->lanes = rd->lanes;
        a->road = ridx; a->klass = rd->klass;
        a->skew = skew; a->want = run;
        a->shift = fshift;
        a->surface = rd->surface;
    }
}

/* Every hard invariant a real street has to clear before it may be marched.
 * Each failure is COUNTED, never silent -- that is the whole point of sourcing
 * candidates from data nobody conditioned for this engine. */
static int tg_geo_span_run_ok(const TG_NodeList *nl, int nspans,
                              int si, int left, int run, double fshift,
                              int *lo_out, int *hi_out)
{
    const double sg = left ? 1.0 : -1.0;
    const int lo = si - (run - 1) / 2, hi = lo + run - 1;
    int s;

    /* Off the ends, or on the start grid. Spans below TD5_TG_FACADE_START_RUN
     * are FORCED built by tg_facade_built, so a mouth there would be claimed by
     * tg_xstreet_here and then never emitted -- the two authorities must agree. */
    if (lo < 1 || hi >= nspans || hi + 1 >= nl->count)
        return (s_gs.d_grid++, tg_geo_drop_note(si, left, TG_GD_GRID, 0.0), 0);
    if (lo < tg_start_city_run()
        && td5_env_flag_on("TD5RE_AUTOTRACK_START_CITY"))
        return (s_gs.d_grid++, tg_geo_drop_note(si, left, TG_GD_GRID, 0.0), 0);

    for (s = lo; s <= hi; s++) {
        if (tg_span_in_bridge_run(s) || tg_span_in_tunnel(s))
            return (s_gs.d_struct++, tg_geo_drop_note(si, left, TG_GD_STRUCT, 0.0), 0);
        if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE")
            && tg_span_near_bridge(s, TD5_TG_XBRIDGE_CLEAR))
            return (s_gs.d_struct++, tg_geo_drop_note(si, left, TG_GD_STRUCT, 0.0), 0);
        /* An unpaved biome has no sidewalk, and tg_facade_built only consults
         * the mouth table where one exists. */
        if (!(tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(s)]) > 0.0))
            return (s_gs.d_biome++, tg_geo_drop_note(si, left, TG_GD_BIOME, 0.0), 0);
        if (tg_block_is_park(s, left))
            return (s_gs.d_park++, tg_geo_drop_note(si, left, TG_GD_PARK, 0.0), 0);
        /* A street that starts at the OUTER kerb of the avenue (fshift > 0)
         * leaves from beyond the corridor, so the corridor cannot be in its way. */
        if (!(fshift > 0.0) && tg_side_corridor_here(nl, s, sg))
            return (s_gs.d_corridor++, tg_geo_drop_note(si, left, TG_GD_CORRIDOR, 0.0), 0);
        /* Inside the carriageway is what a second mouth on one (span,side)
         * amounts to: the table is single-valued and the emitters would draw
         * two overlapping quads out of one kerb. */
        if (s_mouth[s][left ? 0 : 1].edge >= 0)
            return (s_gs.d_taken++, tg_geo_drop_note(si, left, TG_GD_TAKEN, 0.0), 0);
    }
    *lo_out = lo; *hi_out = hi;
    return 1;
}

/* Real grade-separated crossings: a way tagged bridge / tunnel / layer != 0
 * passes OVER or UNDER the route, so it gets no mouth and no frontage, exactly
 * like the synthetic tg_net_underpasses. Registered FIRST for the same reason
 * that pass runs first -- the streets must stop at a crossing, not the other
 * way round. */
static void tg_net_geo_underpasses(const TG_NodeList *nl, int nspans)
{
    static TG_GeoHit hits[TG_GEO_MAX_HITS];
    const int nr = td5_geo_roads_count();
    int r;

    for (r = 0; r < nr; r++) {
        const TD5_GeoRoad *rd = td5_geo_roads_get(r);
        int nh, i;
        if (!rd || (!rd->bridge && !rd->tunnel && rd->layer == 0)) continue;
        nh = tg_geo_road_hits(nl, nspans, rd, hits, TG_GEO_MAX_HITS);
        if (nh <= 0) continue;
        for (i = 0; i < nh; i++) {
            const int si = hits[i].si;
            double l[10], rr[10];
            int a, b, blocked = 0, k;
            TG_NetEdge *ed;
            if (si <= 0 || si >= nspans || si + 1 >= nl->count) continue;
            if (hits[i].kbwd < 0) continue;      /* a T cannot be a crossing */
            tg_city_edge_frame(nl, si,  1.0, l);
            tg_city_edge_frame(nl, si, -1.0, rr);
            /* Both arms must be clear of every street already painted, or the
             * deck would cross one and planarity would fail in the audit. */
            for (k = 1; k <= 4 && !blocked; k++) {
                const double d = 3000.0 * (double)k;
                if (tg_world_occ_near(l[0] + l[6] * d, l[2] + l[7] * d,
                                      TD5_TG_LANE_WIDTH, TG_WO_STREET)
                    || tg_world_occ_near(rr[0] + rr[6] * d, rr[2] + rr[7] * d,
                                         TD5_TG_LANE_WIDTH, TG_WO_STREET))
                    blocked = 1;
            }
            if (blocked) { s_gs.d_under++; continue; }
            a = tg_net_node(l[0] + l[6] * 12000.0, l[2] + l[7] * 12000.0,
                            nl->v[si].y, 2, -1);
            b = tg_net_node(rr[0] + rr[6] * 12000.0, rr[2] + rr[7] * 12000.0,
                            nl->v[si].y, 2, -1);
            ed = tg_net_edge_new(a, b, TG_NE_UNDERPASS,
                                 (double)rd->lanes * (double)TD5_TG_LANE_WIDTH);
            if (!ed) return;
            ed->mouth_si = si;
            tg_world_occ_seg(l[0] + l[6] * 2500.0, l[2] + l[7] * 2500.0,
                             s_nodes[a].x, s_nodes[a].z, ed->width * 0.5, TG_WO_STREET);
            tg_world_occ_seg(rr[0] + rr[6] * 2500.0, rr[2] + rr[7] * 2500.0,
                             s_nodes[b].x, s_nodes[b].z, ed->width * 0.5, TG_WO_STREET);
            s_gs.under++;
        }
    }
}

/* The real city streets. Collect every arm the OSM graph offers, order them so
 * the winner of a contested (span, side) is a property of the data, then place
 * them one at a time -- marching and painting each before the next is marched,
 * which is what keeps the network planar. */
static void tg_net_geo_streets(const TG_NodeList *nl, int nspans)
{
    static TG_GeoHit hits[TG_GEO_MAX_HITS];
    const double skewmax = tg_geo_skew_max();
    const int nr = td5_geo_roads_count();
    int r, i;
    double rminx, rminz, rmaxx, rmaxz;

    if (!td5_env_flag_on("TD5RE_AUTOTRACK_CROSS_STREETS")) {
        TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO] TD5RE_AUTOTRACK_CROSS_STREETS=0: "
                  "no real streets accepted");
        return;
    }

    /* Route bbox, so a way on the far side of town costs one compare. */
    rminx = rmaxx = nl->v[0].x; rminz = rmaxz = nl->v[0].z;
    for (i = 1; i <= nspans; i++) {
        if (nl->v[i].x < rminx) rminx = nl->v[i].x;
        if (nl->v[i].x > rmaxx) rmaxx = nl->v[i].x;
        if (nl->v[i].z < rminz) rminz = nl->v[i].z;
        if (nl->v[i].z > rmaxz) rmaxz = nl->v[i].z;
    }

    s_gna = 0;
    for (r = 0; r < nr; r++) {
        const TD5_GeoRoad *rd = td5_geo_roads_get(r);
        int nh, h;
        if (!rd) continue;
        s_gs.ways++;
        if (rd->maxx < rminx - TD5_TG_R8_LAT_MAX || rd->minx > rmaxx + TD5_TG_R8_LAT_MAX
            || rd->maxz < rminz - TD5_TG_R8_LAT_MAX || rd->minz > rmaxz + TD5_TG_R8_LAT_MAX)
            continue;
        s_gs.inbox++;
        /* Grade-separated ways were registered as decks above; they are not
         * side streets and must not also claim a mouth. */
        if (rd->bridge || rd->tunnel || rd->layer != 0) continue;
        nh = tg_geo_road_hits(nl, nspans, rd, hits, TG_GEO_MAX_HITS);
        if (nh < 0) { s_gs.route++; continue; }
        for (h = 0; h < nh; h++) {
            const TG_GeoHit *q = &hits[h];
            if (q->si <= 0 || q->si >= nspans || q->si + 1 >= nl->count) continue;
            tg_geo_arm_push(nl, q, rd, r,  q->dx,  q->dz, q->kfwd, q->sfwd, skewmax);
            if (q->kbwd >= 0)
                tg_geo_arm_push(nl, q, rd, r, -q->dx, -q->dz, q->kbwd, q->sbwd, skewmax);
        }
    }

    if (s_gna > 1) qsort(s_garm, (size_t)s_gna, sizeof(s_garm[0]), tg_geo_arm_cmp);

    for (i = 0; i < s_gna; i++) {
        const TG_GeoArm *a = &s_garm[i];
        const double sg = a->left ? 1.0 : -1.0;
        int run = a->lanes, lo = 0, hi = 0, why, na, nb, kind;
        double e[10], ox, oz, reach, from, width, pre;
        TG_NetEdge *ed;

        /* [1014 B items 6, 19] The frontage run is counted ALONG THE ROAD, but a
         * street leaving at `skew` is only cos(skew) as wide as that run: at 45
         * degrees (La Plata's whole diagonal grid) a 3-lane calle was drawn
         * 0.71 x its width, at the 68 degrees of the Plaza Moreno mouths 0.37 x
         * -- a 4 m sliver. The run is widened by 1/cos so the street's own width
         * is the lanes it has. Floored at 60 degrees (TG_GEO_SKEWW_COS_MIN): past it
         * the sheared quad is a wedge across the road, not a wider street. */
        if (td5_env_flag_on("TD5RE_GEO_STREET_SKEWWIDTH")) {
            const double cs = cos(a->skew);
            /* Past the cap the quad is a wedge along the road, and a WIDER wedge
             * is worse than a narrow one: leave those at their lane count. */
            if (cs >= TG_GEO_SKEWW_COS_MIN)
                run = (int)floor((double)a->lanes / cs + 0.5);
        }
        if (run < 1) run = 1;
        if (run > TG_GEO_MOUTH_SPANS) run = TG_GEO_MOUTH_SPANS;
        if (!tg_geo_span_run_ok(nl, nspans, a->si, a->left, run, a->shift, &lo, &hi)) continue;

        width = (double)(hi - lo + 1) * (double)TD5_TG_LANE_WIDTH;
        tg_city_edge_frame(nl, a->si, sg, e);
        if (a->shift > 0.0) {            /* [1014 B item 8] start at the far kerb */
            e[0] += e[6] * a->shift;  e[2] += e[7] * a->shift;
            e[3] += e[8] * a->shift;  e[5] += e[9] * a->shift;
        }
        {
            const double cs = cos(a->skew), sn = sin(a->skew);
            ox = e[6] * cs - e[7] * sn;
            oz = e[6] * sn + e[7] * cs;
            /* The main road's own paint is half-width + 600 wide, so the ray
             * has to be let out past it before the carriageway test may fire.
             * A skewed ray covers less lateral ground per unit of length, so
             * the stand-off grows as 1/cos -- without this a 45-degree
             * diagonal is rejected by the road it leaves. */
            from = TD5_TG_R8_CLAMP_MIN
                 / (cs < TG_GEO_MARCH_COS_MIN ? TG_GEO_MARCH_COS_MIN : cs);
        }
        reach = tg_net_march(e[0], e[2], ox, oz, a->want, width * 0.5, from, &why);
        if (why == 1) s_gs.why_road++;
        if (why == 2) s_gs.why_street++;
        if (why == 3) s_gs.why_water++;
        pre   = reach;
        reach = tg_r13_fold_cap(nl, a->si, sg, a->skew, reach, "TD5RE_R13_FOLD_STREET");
        if (reach < TD5_TG_R8_CLAMP_MIN) {
            if (reach < pre) { s_gs.d_fold++;  tg_geo_drop_note(a->si, a->left, TG_GD_FOLD, reach); }
            else             { s_gs.d_short++; tg_geo_drop_note(a->si, a->left, TG_GD_SHORT, reach); }
            continue;
        }

        kind = tg_turn_open(a->si, a->left) ? TG_NE_CONTINUATION
             : ((a->lanes >= TG_GEO_AVENUE_LANES || a->klass >= TD5_GEO_RC_PRIMARY)
                ? TG_NE_AVENUE : TG_NE_STREET);
        na = tg_net_node(e[0], e[2], e[1], 0, a->si);
        nb = tg_net_node(e[0] + ox * reach, e[2] + oz * reach,
                         tg_world_h(e[0] + ox * reach, e[2] + oz * reach),
                         why == 2 ? 1 : 2, -1);
        ed = tg_net_edge_new(na, nb, kind, width);
        if (!ed) { s_gs.d_full++; tg_geo_drop_note(a->si, a->left, TG_GD_FULL, 0.0); return; }
        ed->mouth_si = a->si; ed->mouth_left = a->left;
        ed->mouth_lo = lo;    ed->mouth_hi   = hi;
        ed->skew = a->skew;   ed->reach      = reach;
        ed->surface = a->surface;
        tg_net_paint_edge(ed, TG_WO_STREET);
        tg_net_set_mouth(lo, hi, a->left, (int)(ed - s_edges), a->skew, reach);
        if (a->shift > 0.0) {
            int ms;
            for (ms = lo; ms <= hi; ms++) {
                /* each span's own shift: the avenue gap changes along the road */
                const double sh = tg_net_far_shift(nl, ms, sg);
                s_mouth[ms][a->left ? 0 : 1].shift = (float)(sh > 0.0 ? sh : a->shift);
            }
            s_gs.beyond++;
        }
        /* [ROUND 1015 B item 16] ONE street direction for the whole mouth run.
         * The skew above was measured at the placing span and copied to every span
         * of lo..hi, and each span's quad, pavement arms, flanks and zebra rotate
         * THEIR OWN outward normal by it. On a straight road that is parallel; in a
         * bend the normals themselves turn (La Plata span 805, the road swings 90
         * degrees over spans 801..805), so the five quads FANNED: the ones at the
         * bend's start pointed back across the carriageway and their pavement arms
         * ("sidewalk clips through the middle of the road") ran over the road for 20 m.
         * The street the march validated is a straight ray, so each span now gets
         * the skew that turns ITS normal onto that ray, and a span whose normal is
         * more than the skew ceiling off it (the ray would run along or back over the
         * kerb) leaves the run. TD5RE_GEO_MOUTH_PARALLEL=0 restores the shared skew. */
        if (td5_env_flag_on("TD5RE_GEO_MOUTH_PARALLEL")) {
            int ms, cut = 0;
            for (ms = lo; ms <= hi; ms++) {
                double ef[10], nxm, nzm, nl2, sk;
                if (ms < 0 || ms >= TD5_TG_MAX_SPANS + 8 || ms + 1 >= nl->count) continue;
                tg_city_edge_frame(nl, ms, sg, ef);
                nxm = ef[6] + ef[8]; nzm = ef[7] + ef[9];
                nl2 = sqrt(nxm * nxm + nzm * nzm);
                if (nl2 < 1e-6) continue;
                sk = tg_geo_skew_of(nxm / nl2, nzm / nl2, ox, oz);
                if (fabs(sk) > skewmax) {
                    s_mouth[ms][a->left ? 0 : 1].edge = -1;
                    s_mouth[ms][a->left ? 0 : 1].shift = 0.0f;
                    cut++;
                } else {
                    s_mouth[ms][a->left ? 0 : 1].skew = (float)sk;
                }
            }
            if (cut) s_gs.fan_cut += cut;
        }
        if (td5_env_flag_off("TD5RE_GEO_NET_DIAG"))
            TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO PLACED] si %d %s run %d..%d reach %.0f kind %d",
                      a->si, a->left ? "left" : "right", lo, hi, reach, kind);
        if (kind == TG_NE_AVENUE)            s_gs.avenue++;
        else if (kind == TG_NE_CONTINUATION) s_gs.cont++;
        else                                 s_gs.street++;
    }
}


/* ============ [1014 B items 11, 14, 19] PLAZA BORDER ROADS ================
 *
 * "there should be a crossing road that keeps following the park onwards but
 * there's no road" / "the road bordering the whole plaza should be wider" /
 * "all road intersections at Plaza Moreno look very weird in game, yet render
 * properly on the minimap".
 *
 * The minimap draws every OSM way. The 3D world drew only the route and a
 * straight 45 m STUB per accepted junction arm (tg_geo_arm_push measures how far
 * the real way stays on a straight ray and stops at the first bend), so a road
 * that curves round a square -- every ring road, and the arcs round Plaza
 * Moreno -- was a stub that ended in bare ground while the plaza lawn beside it
 * was complete. Two thirds of Plaza Miguel de Azcuenaga's ring did not exist.
 *
 * WHAT IS ADDED, and only that: for every plaza (a mapped polygon, or a named
 * ring plaza's hull) bound to the route, the real OSM ways that RUN ALONG its
 * edge are laid as asphalt ribbons following the way's own vertices, at the way's
 * real carriageway (lanes, with the place floor the reader applies). They are
 * network edges of kind BACKSTREET -- the polyline kind the emitter already
 * draws -- so the occupancy paint, the guard marks and the minimap read-back
 * need nothing new. Ribbons stop short of anything already painted (the route,
 * a street stub, a fork corridor), so they never overlay another road.
 *
 * NOT DRIVABLE, and not a fork: the plaza fork (td5_tg_realfork.c) is a
 * measured refactor of its own (docs/plans/GEO_REAL_FORKS.md). This is scenery,
 * like the opposite carriageway of an avenue. TD5RE_GEO_PLAZA_ROADS=0 drops it. */
#define TG_PZ_MAX       64        /* plazas considered                              */
#define TG_PZ_DIST      9000.0    /* a point this close to the edge runs along it    */
#define TG_PZ_STEP      2400.0    /* resample step along a way                      */

static double tg_pz_seg_dist(double px, double pz, double ax, double az,
                             double bx, double bz)
{
    const double dx = bx - ax, dz = bz - az;
    double t = ((px - ax) * dx + (pz - az) * dz) / (dx * dx + dz * dz + 1e-9);
    double qx, qz;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    qx = ax + dx * t - px; qz = az + dz * t - pz;
    return sqrt(qx * qx + qz * qz);
}

/* Distance from a point to the boundary of plaza area `a`, and the unit tangent
 * of the boundary edge that is nearest. */
static double tg_pz_edge_dist(const TD5_GeoArea *a, double x, double z,
                              double *tx, double *tz)
{
    double best = 1e300, px0 = 0.0, pz0 = 0.0, fx = 0.0, fz = 0.0;
    int k;
    for (k = 0; k <= a->n; k++) {
        double cx, cz, d;
        if (k < a->n) td5_geob_ring(a->first, k, &cx, &cz);
        else { if (a->n <= 2) break; cx = fx; cz = fz; }   /* close the ring */
        if (k == 0) { fx = cx; fz = cz; }
        else {
            d = tg_pz_seg_dist(x, z, px0, pz0, cx, cz);
            if (d < best) {
                const double dx = cx - px0, dz = cz - pz0;
                const double l = sqrt(dx * dx + dz * dz) + 1e-9;
                best = d; *tx = dx / l; *tz = dz / l;
            }
        }
        px0 = cx; pz0 = cz;
    }
    return best;
}

/* Lay the pending polyline q[0..nq) as one backstreet-kind edge owned by the
 * route span nearest its middle. Returns 1 when an edge was made, -1 when the
 * edge table is full, 0 when the polyline was too short or owned by no span. */
static int tg_pz_flush(const TG_NodeList *nl, int nspans, const TD5_GeoRoad *rd,
                       const double *qx, const double *qz, int nq)
{
    int owner, a0, b0, m;
    TG_NetEdge *ed;
    if (nq < 2) return 0;
    owner = tg_geo_nearest(nl, nspans, qx[nq / 2], qz[nq / 2]);
    if (owner < 1 || owner + 1 >= nl->count || owner >= nspans) return 0;
    a0 = tg_net_node(qx[0], qz[0], tg_world_h(qx[0], qz[0]), 1, -1);
    b0 = tg_net_node(qx[nq - 1], qz[nq - 1], tg_world_h(qx[nq - 1], qz[nq - 1]), 1, -1);
    ed = tg_net_edge_new(a0, b0, TG_NE_BACKSTREET, rd->width);
    if (!ed) return -1;
    ed->npoly = nq;
    for (m = 0; m < nq; m++) {
        ed->px[m] = qx[m]; ed->pz[m] = qz[m];
        ed->py[m] = tg_world_h(qx[m], qz[m]);
    }
    ed->mouth_si = owner; ed->mouth_left = 0;
    ed->surface = rd->surface;
    ed->ribbon = 1;
    /* NOT painted here: the way's own next ribbon (and the neighbouring ways of
     * a ring, which share an end point) must not be refused by this one's paint,
     * or every joint opens a gap. The caller paints the whole set at the end. */
    return 1;
}

static void tg_net_geo_plaza_roads(const TG_NodeList *nl, int nspans)
{
    int pz_idx[TG_PZ_MAX], npz = 0, i, r, edge0, ei;
    long laid = 0, ways_used = 0;
    const int nr = td5_geo_roads_count();

    if (!td5_env_flag_on("TD5RE_GEO_PLAZA_ROADS")) return;
    if (!td5_geob_sync()) return;
    for (i = 0; i < td5_geob_area_count() && npz < TG_PZ_MAX; i++) {
        const TD5_GeoArea *a = td5_geob_area(i);
        if (!a || a->n < 3 || a->host_span < 0 || a->host_span >= nspans) continue;
        if (!td5_geob_area_is_plaza(a)) continue;
        pz_idx[npz++] = i;
    }
    if (npz == 0) return;
    edge0 = s_ne;

    for (r = 0; r < nr; r++) {
        const TD5_GeoRoad *rd = td5_geo_roads_get(r);
        double qx[TG_NET_POLY], qz[TG_NET_POLY];
        int nq = 0, k, used = 0, full = 0;
        if (!rd || rd->bridge || rd->tunnel || rd->layer != 0 || rd->count < 2) continue;
        /* a service lane or alley is not a plaza road */
        if (rd->klass < TD5_GEO_RC_LIVING) continue;
        for (k = 0; k + 1 < rd->count && !full; k++) {
            double ax, az, bx, bz, len, ux, uz, t;
            if (!td5_geo_roads_point(rd, k, &ax, &az)) break;
            if (!td5_geo_roads_point(rd, k + 1, &bx, &bz)) break;
            len = sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            if (len < 1.0) continue;
            ux = (bx - ax) / len; uz = (bz - az) / len;
            for (t = 0.0; ; t += TG_PZ_STEP) {
                /* Every segment contributes its start; only the way's LAST segment
                 * contributes its end. The end of one segment IS the start of the
                 * next, and a repeated point has no bearing -- it is what made the
                 * mitre spike. */
                const int last_seg = (k + 2 >= rd->count);
                const double tt = (t >= len) ? len : t;
                const double sx = ax + ux * tt, sz = az + uz * tt;
                int along = 0, j, free_pt;
                if (t >= len && !last_seg) break;
                /* RUNS ALONG the edge: close to it AND within ~45 degrees of its
                 * tangent. A street that merely crosses the plaza's edge is close
                 * for two samples and square to it, and is not a plaza road. */
                for (j = 0; j < npz && !along; j++) {
                    double tgx = 0.0, tgz = 0.0;
                    const double d = tg_pz_edge_dist(td5_geob_area(pz_idx[j]), sx, sz, &tgx, &tgz);
                    const double c = ux * tgx + uz * tgz;
                    if (d <= TG_PZ_DIST && (c > 0.7 || c < -0.7)) along = 1;
                }
                free_pt = along && !tg_world_is_water(sx, sz)
                       && !tg_world_occ_near(sx, sz, rd->width * 0.5 + 250.0,
                                             TG_WO_ROAD | TG_WO_DRIVABLE | TG_WO_STREET);
                if (free_pt) {
                    qx[nq] = sx; qz[nq] = sz; nq++;
                    if (nq == TG_NET_POLY) {
                        /* a full polyline: lay it and carry its last point so the
                         * next one continues the same line */
                        const int rc = tg_pz_flush(nl, nspans, rd, qx, qz, nq);
                        if (rc < 0) { full = 1; break; }
                        if (rc > 0) { laid++; used = 1; }
                        qx[0] = qx[nq - 1]; qz[0] = qz[nq - 1]; nq = 1;
                    }
                } else {
                    const int rc = tg_pz_flush(nl, nspans, rd, qx, qz, nq);
                    if (rc < 0) { full = 1; break; }
                    if (rc > 0) { laid++; used = 1; }
                    nq = 0;
                }
                if (t >= len) break;
            }
        }
        {
            const int rc = tg_pz_flush(nl, nspans, rd, qx, qz, nq);
            if (rc > 0) { laid++; used = 1; }
        }
        if (used) ways_used++;
        if (full) break;
    }
    /* Now the paint and the bed, for the whole set. */
    for (ei = edge0; ei < s_ne; ei++) {
        const TG_NetEdge *ed = &s_edges[ei];
        int m;
        tg_net_paint_edge(ed, TG_WO_STREET);
        for (m = 0; m + 1 < ed->npoly; m++)
            tg_world_conform_seg(ed->px[m], ed->pz[m], ed->py[m],
                                 ed->px[m + 1], ed->pz[m + 1], ed->py[m + 1],
                                 ed->width * 0.5 + 600.0, 2500.0);
    }
    TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO PLAZA] %d plaza(s) bound to the route; %ld ribbon(s) "
              "laid along the edge of %ld real way(s)", npz, laid, ways_used);
}

/* Is this build sourcing its streets from real OSM roads? A route alone is not
 * enough: TD5RE_GEO_ROUTE can drive a conditioned polyline over the SYNTHETIC
 * world with no place cache behind it (td5_geo.h), and there is no road graph to
 * accept in that case. */
static int tg_net_geo_active(void)
{
    if (!td5_geo_loaded() || td5_geo_route_count() < 2) return 0;
    if (td5_env_flag_off("TD5RE_GEO_NET_SYNTH_ONLY")) return 0;
    return td5_geo_roads_sync(td5_geo_place_slug());
}

static void tg_net_geo_census(void)
{
    TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO] %s: real street census -- "
              "%ld way(s), %ld near the route, %ld are the route itself; "
              "%ld junction arm(s) considered, %ld accepted "
              "(street %ld avenue %ld continuation %ld), %ld real deck(s), "
              "%ld shared-carriageway departure(s), %ld street(s) start beyond an avenue; "
              "dropped: skew %ld short %ld fold %ld taken %ld struct %ld "
              "grid %ld biome %ld park %ld corridor %ld deck-blocked %ld "
              "table-full %ld; march stops: road %ld street %ld water/steep %ld; "
              "mouth spans cut off a bend's fan %ld",
              td5_geo_place_slug(), s_gs.ways, s_gs.inbox, s_gs.route,
              s_gs.cand, s_gs.street + s_gs.avenue + s_gs.cont,
              s_gs.street, s_gs.avenue, s_gs.cont, s_gs.under, s_gs.depart, s_gs.beyond,
              s_gs.d_skew, s_gs.d_short, s_gs.d_fold, s_gs.d_taken,
              s_gs.d_struct, s_gs.d_grid, s_gs.d_biome, s_gs.d_park,
              s_gs.d_corridor, s_gs.d_under, s_gs.d_full,
              s_gs.why_road, s_gs.why_street, s_gs.why_water, s_gs.fan_cut);
    /* The ledger: one line per refused arm, so a RUN of missing crossings
     * names its own rule instead of hiding inside a total. */
    {
        int i;
        for (i = 0; i < s_gdropn; i++) {
            const int w = s_gdrop[i].why < TG_GD_N ? s_gdrop[i].why : 0;
            const char *unit = (w == TG_GD_SKEW) ? "deg"
                             : ((w == TG_GD_SHORT || w == TG_GD_FOLD) ? "units" : "");
            TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO]   refused span %4d %-5s: "
                      "%-10s %.0f %s",
                      (int)s_gdrop[i].si, s_gdrop[i].left ? "left" : "right",
                      k_gd_name[w], (double)s_gdrop[i].arg, unit);
        }
        if (s_gdropn >= TG_GEO_DROP_MAX)
            TD5_LOG_I(LOG_TAG, "trackgen: [NET/GEO]   (ledger full at %d; the "
                      "census totals above are complete)", TG_GEO_DROP_MAX);
    }
}

/* -------------------------------------------------------------- build -- */

void tg_network_reset(void)
{
    int s;
    s_nn = s_ne = 0; s_net_built = 0; s_net_nspans = 0; s_net_geo = 0;
    s_stat_cand = s_stat_short = s_stat_tjunc = s_stat_water = s_stat_road = 0;
    s_gna = 0;
    s_gdropn = 0;          /* or a regenerate replays the FIRST build's ledger */
    memset(&s_gs, 0, sizeof(s_gs));
    for (s = 0; s < TD5_TG_MAX_SPANS + 8; s++) {
        s_mouth[s][0].edge = s_mouth[s][1].edge = -1;
        s_mouth[s][0].skew = s_mouth[s][1].skew = 0.0f;
        s_mouth[s][0].reach = s_mouth[s][1].reach = 0.0f;
        s_mouth[s][0].shift = s_mouth[s][1].shift = 0.0f;
    }
}

int tg_network_built(void) { return s_net_built; }

/* [1014 B] Did the built network take its streets from a real OSM graph? The
 * junction furniture that keys on "a mouth is open here" reads this to know a
 * mouth is a REAL street rather than a hash-rhythm gap. 0 on every synthetic
 * build, so anything gated on it leaves slot 60 byte-identical. */
int tg_net_geo(void) { return s_net_built && s_net_geo; }

void tg_network_build(const TG_NodeList *nl, int nspans_main)
{
    int f, k, counts[TG_NE_KIND_COUNT], i, loops = 0, junc = 0, byp = 0;
    tg_network_reset();
    if (!nl || nspans_main < 2) return;
    if (nspans_main > TD5_TG_MAX_SPANS) nspans_main = TD5_TG_MAX_SPANS;
    s_net_nspans = nspans_main;

    /* The bend continuations first: they open frontage the city streets
     * below read through tg_facade_built_hash. */
    tg_turn_map_build(nl, nspans_main);

    /* Bypass corridors are planned on the empty raster (they are the widest
     * thing beside the road), then every corridor -- bowed or planned -- is
     * painted so no street lands on one. */
    tg_net_bypass_plan(nl, nspans_main);
    for (f = 0; f < s_fork_count; f++) {
        const TG_Fork *fk = &s_forks[f];
        for (k = 0; k <= fk->len; k++) {
            const int mb = fk->F + 1 + k;
            const TG_Node *n;
            double lat, w;
            if (mb < 0 || mb >= nl->count) break;
            n = &nl->v[mb];
            lat = tg_fork_br_shift(f, k, n->width);
            w = n->width * tg_fork_br_wscale(f, k);
            tg_world_occ_disc(n->x + n->tz * lat, n->z - n->tx * lat, w * 0.5 + 600.0, TG_WO_DRIVABLE);
            /* The GORE between the two carriageways is road bed too: conform
             * the whole lateral band from the main centre line to the
             * corridor's outer edge at the main road's height, so a low-lying
             * gore is never below the sea (a bowed fork over a marsh read as a
             * lake between the lanes). */
            tg_world_conform_seg(n->x, n->z, n->y,
                                 n->x + n->tz * lat, n->z - n->tx * lat, n->y,
                                 w * 0.5 + TD5_TG_ROAD_BED_VERGE, 3500.0);
        }
    }

    /* Underpass crossings FIRST: they are placed by their own period and
     * the streets must stop at them, not the other way round. */
    tg_net_underpasses(nl, nspans_main);
    if (tg_net_geo_active()) {
        s_net_geo = 1;
        /* [GEO PHASE 5] Real streets REPLACE the planted ones. The synthetic
         * generators that remain would each invent tarmac the place does not
         * have -- a forest lane on the R12 period, a back street closing a
         * block the real grid already closes -- so they are opt-in
         * (TD5RE_GEO_NET_SYNTH=1) rather than additive. */
        tg_net_geo_underpasses(nl, nspans_main);
        tg_net_geo_streets(nl, nspans_main);
        tg_net_geo_plaza_roads(nl, nspans_main);          /* [1014 B] */
        if (td5_env_flag_off("TD5RE_GEO_NET_SYNTH")) {
            tg_net_back_streets();
            tg_net_country(nl, nspans_main);
        }
        tg_net_geo_census();
    } else {
        tg_net_city_streets(nl, nspans_main);
        tg_net_back_streets();
        tg_net_country(nl, nspans_main);
    }
    s_net_built = 1;
    /* the conforms above moved ground: the shore table must see the result */
    tg_road_shore_rebuild(nl);

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < s_ne; i++) {
        counts[s_edges[i].kind]++;
        if (s_edges[i].kind == TG_NE_BYPASS) byp++;
        else if (s_edges[i].rejoin_si >= 0) loops++;
    }
    for (i = 0; i < s_nn; i++) if (s_nodes[i].kind == 1) junc++;
    TD5_LOG_I(LOG_TAG, "trackgen: [NET] %d node(s) %d edge(s): street %d avenue %d "
              "backstreet %d continuation %d country %d (loops %d) underpass %d "
              "bypass %d (drivable); %d junction(s); candidates %ld, dropped short %ld; "
              "stops: road %ld street %ld water/steep %ld",
              s_nn, s_ne, counts[TG_NE_STREET], counts[TG_NE_AVENUE],
              counts[TG_NE_BACKSTREET], counts[TG_NE_CONTINUATION],
              counts[TG_NE_COUNTRY], loops, counts[TG_NE_UNDERPASS], byp, junc,
              s_stat_cand, s_stat_short, s_stat_road, s_stat_tjunc, s_stat_water);
}

/* ---------------------------------------------------------- authority -- */

int tg_net_mouth(int si, int left, double *skew, double *reach)
{
    const TG_NetMouth *m;
    if (!s_net_built || si < 0 || si >= TD5_TG_MAX_SPANS + 8) return -1;
    m = &s_mouth[si][left ? 0 : 1];
    if (m->edge < 0) return -1;
    if (skew)  *skew  = m->skew;
    if (reach) *reach = m->reach;
    return m->edge;
}

int tg_net_mouth_kind(int si, int left)
{
    const int e = tg_net_mouth(si, left, NULL, NULL);
    return e < 0 ? -1 : s_edges[e].kind;
}

int tg_net_mouth_surface(int si, int left)
{
    const int e = tg_net_mouth(si, left, NULL, NULL);
    /* No mouth is SMOOTH, not an error code: the caller is choosing a texture
     * page and "there is no real street here" and "the real street is paved"
     * want the same answer. */
    return e < 0 ? TD5_GEO_SURF_SMOOTH : s_edges[e].surface;
}

/* ---------------------------------------------------------- emission -- */

/* [ROUND 1015 B items 2, 7] Height a drawn network road stands at: the heightfield's
 * envelope over half a cell round (x,z), lifted. The ground meshes are triangulated on a
 * 1500 lattice, so the exact height at an off-lattice point can sit BELOW the surface a
 * viewer sees there; a road laid at world_h + 30 on a few far-apart polyline vertices was
 * under the terrain for most of its length (34 of 53 ring/back-street meshes on La Plata).
 * Geo only: callers gate on tg_net_follow_on() so the synthetic bytes stay as they were. */
#define TG_NET_FOLLOW_LIFT 60.0
#define TG_NET_FOLLOW_SEG  1500.0
static double tg_net_ground_y(double x, double z)
{
    const double hh = TG_WORLD_CELL * 0.5;
    double g = tg_world_h(x, z), g1;
    g1 = tg_world_h(x + hh, z); if (g1 > g) g = g1;
    g1 = tg_world_h(x - hh, z); if (g1 > g) g = g1;
    g1 = tg_world_h(x, z + hh); if (g1 > g) g = g1;
    g1 = tg_world_h(x, z - hh); if (g1 > g) g = g1;
    return g + TG_NET_FOLLOW_LIFT;
}

static int tg_net_follow_on(void)
{
    return td5_geo_loaded() && td5_env_flag_on("TD5RE_GEO_STREET_FOLLOW");
}

/* [ROUND 1015 B item 12] Does (x,z) lie on the carriageway of any edge OTHER than `self`?
 * Distance to each of its polyline segments against half its width. */
static int tg_net_on_other_road(const TG_NetEdge *self, double x, double z)
{
    int i, k;
    for (i = 0; i < s_ne; i++) {
        const TG_NetEdge *o = &s_edges[i];
        if (o == self || o->npoly < 2) continue;
        for (k = 0; k + 1 < o->npoly; k++) {
            const double ax = o->px[k], az = o->pz[k];
            const double bx = o->px[k + 1], bz = o->pz[k + 1];
            const double dx = bx - ax, dz = bz - az;
            const double l2 = dx * dx + dz * dz;
            double t = (l2 > 1e-9) ? ((x - ax) * dx + (z - az) * dz) / l2 : 0.0, ex, ez;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            ex = ax + dx * t - x; ez = az + dz * t - z;
            if (ex * ex + ez * ez < o->width * o->width * 0.25) return 1;
        }
    }
    return 0;
}

/* Tarmac for the polyline parts nothing else draws: back streets, and the
 * wandering half of a country loop (its first, straight segment is the R12
 * forest lane the terrain emitters already lay). Owned by the mouth span. */
int tg_net_emit_entry(const TG_FBHook *h)
{
    int i;
    if (!s_net_built) return 1;
    for (i = 0; i < s_ne; i++) {
        const TG_NetEdge *e = &s_edges[i];
        int k, k0;
        if (e->mouth_si != h->si) continue;
        if (e->ribbon && e->npoly > 2) {
            /* [1014 B] ONE mesh, vertices shared and MITRED at every bend, so a
             * ring road is a continuous ribbon and not a string of separate
             * rectangles with a notch at each joint. */
            double px[4 * TG_NET_RIB_Q], py[4 * TG_NET_RIB_Q], pz[4 * TG_NET_RIB_Q];
            double uu[4 * TG_NET_RIB_Q], vv[4 * TG_NET_RIB_Q];
            double lx[TG_NET_POLY], lz[TG_NET_POLY], rx[TG_NET_POLY], rz[TG_NET_POLY];
            int seg_page = TD5_TG_PAGE_R4_CROSS + 0, seg_nq = e->npoly - 1, n = 0, m;
            const int follow = tg_net_follow_on();
            const double hw = e->width * 0.5;
            double vlen = 0.0;
            size_t off;
            for (m = 0; m < e->npoly; m++) {
                double d0x = 0, d0z = 0, d1x = 0, d1z = 0, tx, tz, tl, nx, nz, ml = 1.0;
                if (m > 0) { d0x = e->px[m] - e->px[m - 1]; d0z = e->pz[m] - e->pz[m - 1]; tl = sqrt(d0x * d0x + d0z * d0z); if (tl > 1e-6) { d0x /= tl; d0z /= tl; } }
                if (m + 1 < e->npoly) { d1x = e->px[m + 1] - e->px[m]; d1z = e->pz[m + 1] - e->pz[m]; tl = sqrt(d1x * d1x + d1z * d1z); if (tl > 1e-6) { d1x /= tl; d1z /= tl; } }
                if (m == 0) { d0x = d1x; d0z = d1z; }
                if (m + 1 == e->npoly) { d1x = d0x; d1z = d0z; }
                tx = d0x + d1x; tz = d0z + d1z;
                tl = sqrt(tx * tx + tz * tz);
                if (tl < 1e-6) { tx = d1x; tz = d1z; tl = 1.0; }
                tx /= tl; tz /= tl;
                nx = tz; nz = -tx;                            /* same side convention as the segment path */
                ml = nx * (-d1z) + nz * d1x;                  /* cos(half the bend) */
                ml = (ml < 0.0) ? -ml : ml;
                ml = (ml < 0.62) ? 0.62 : ml;                 /* clamp the miter at ~1.6 x */
                lx[m] = e->px[m] - nx * hw / ml; lz[m] = e->pz[m] - nz * hw / ml;
                rx[m] = e->px[m] + nx * hw / ml; rz[m] = e->pz[m] + nz * hw / ml;
            }
            {
                /* [1015 B] span-length pieces when the road follows the ground (geo): one quad
                 * per polyline segment, unless that would not fit the buffer */
                int ksub[TG_NET_POLY], total = 0;
                for (m = 0; m + 1 < e->npoly; m++) {
                    const double sl = sqrt((e->px[m + 1] - e->px[m]) * (e->px[m + 1] - e->px[m])
                                         + (e->pz[m + 1] - e->pz[m]) * (e->pz[m + 1] - e->pz[m]));
                    int k = follow ? (int)ceil(sl / TG_NET_FOLLOW_SEG) : 1;
                    if (k < 1) k = 1;
                    if (k > 24) k = 24;
                    ksub[m] = k; total += k;
                }
                if (total > TG_NET_RIB_Q)
                    for (m = 0; m + 1 < e->npoly; m++) ksub[m] = 1;
                for (m = 0; m + 1 < e->npoly; m++) {
                    const double sl = sqrt((e->px[m + 1] - e->px[m]) * (e->px[m + 1] - e->px[m])
                                         + (e->pz[m + 1] - e->pz[m]) * (e->pz[m + 1] - e->pz[m]));
                    int j;
                    for (j = 0; j < ksub[m]; j++) {
                        const double t0 = (double)j / (double)ksub[m];
                        const double t1 = (double)(j + 1) / (double)ksub[m];
                        const double cx0 = e->px[m] + (e->px[m + 1] - e->px[m]) * t0;
                        const double cz0 = e->pz[m] + (e->pz[m + 1] - e->pz[m]) * t0;
                        const double cx1 = e->px[m] + (e->px[m + 1] - e->px[m]) * t1;
                        const double cz1 = e->pz[m] + (e->pz[m + 1] - e->pz[m]) * t1;
                        const double y0 = follow ? tg_net_ground_y(cx0, cz0)
                                                 : tg_world_h(e->px[m], e->pz[m]) + 30.0;
                        const double y1 = follow ? tg_net_ground_y(cx1, cz1)
                                                 : tg_world_h(e->px[m + 1], e->pz[m + 1]) + 30.0;
                        const double v0 = (vlen + sl * t0) / (double)TD5_TG_SPAN_LENGTH;
                        const double v1 = (vlen + sl * t1) / (double)TD5_TG_SPAN_LENGTH;
                        px[n] = lx[m] + (lx[m + 1] - lx[m]) * t0; pz[n] = lz[m] + (lz[m + 1] - lz[m]) * t0;
                        py[n] = y0; uu[n] = 0.0; vv[n] = v0; n++;
                        px[n] = rx[m] + (rx[m + 1] - rx[m]) * t0; pz[n] = rz[m] + (rz[m + 1] - rz[m]) * t0;
                        py[n] = y0; uu[n] = 1.0; vv[n] = v0; n++;
                        px[n] = rx[m] + (rx[m + 1] - rx[m]) * t1; pz[n] = rz[m] + (rz[m + 1] - rz[m]) * t1;
                        py[n] = y1; uu[n] = 1.0; vv[n] = v1; n++;
                        px[n] = lx[m] + (lx[m + 1] - lx[m]) * t1; pz[n] = lz[m] + (lz[m + 1] - lz[m]) * t1;
                        py[n] = y1; uu[n] = 0.0; vv[n] = v1; n++;
                    }
                    vlen += sl;
                }
                seg_nq = n / 4;
            }
            if (*h->nmesh >= h->maxmesh - 1) return 1;
            off = h->blk->len;
            h->moff[(*h->nmesh)++] = off;
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
                return 0;
            tg_guard_mark(off, h->blk->len, TG_GK_CROSS, h->si);
            tg_acct_range(TG_ACCT_CROSSING, h->si, h->si);
            /* [1014 B] and a raised footway on both kerbs, the same slab + kerb
             * face the main road's pavement is, so a ring road reads as the road
             * the route drives and not as a painted strip. One mesh, mitred like
             * the carriageway. Width: the biome's own pavement line. */
            {
                const double swb = tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(h->si)]);
                const double sw  = (swb > 0.0) ? swb : 1290.0;
                const double H   = (double)TD5_TG_KERB_H;
                double sx[16 * TG_NET_POLY], sy[16 * TG_NET_POLY], sz[16 * TG_NET_POLY];
                double su[16 * TG_NET_POLY], sv[16 * TG_NET_POLY];
                int sn = 0, side;
                int spage = TD5_TG_PAGE_SIDEWALK, snq = 0;
                vlen = 0.0;
                if (td5_env_flag_on("TD5RE_GEO_PLAZA_ROAD_PAVE")) {
                    for (m = 0; m + 1 < e->npoly; m++) {
                        const double sl = sqrt((e->px[m + 1] - e->px[m]) * (e->px[m + 1] - e->px[m])
                                             + (e->pz[m + 1] - e->pz[m]) * (e->pz[m + 1] - e->pz[m]));
                        const double y0 = follow ? tg_net_ground_y(e->px[m], e->pz[m])
                                                 : tg_world_h(e->px[m], e->pz[m]) + 30.0;
                        const double y1 = follow ? tg_net_ground_y(e->px[m + 1], e->pz[m + 1])
                                                 : tg_world_h(e->px[m + 1], e->pz[m + 1]) + 30.0;
                        const double v0 = vlen / (double)TD5_TG_SPAN_LENGTH;
                        const double v1 = (vlen + sl) / (double)TD5_TG_SPAN_LENGTH;
                        const double uw = sw / (double)TD5_TG_SPAN_LENGTH;
                        for (side = 0; side < 2; side++) {
                            /* kerb edge (on the carriageway's edge) and the slab's outer edge,
                             * per end, from the mitred offsets above scaled out by sw */
                            const double *ex = side ? rx : lx, *ez = side ? rz : lz;
                            const double dx0 = ex[m]     - e->px[m],     dz0 = ez[m]     - e->pz[m];
                            const double dx1 = ex[m + 1] - e->px[m + 1], dz1 = ez[m + 1] - e->pz[m + 1];
                            const double l0 = sqrt(dx0 * dx0 + dz0 * dz0) + 1e-9;
                            const double l1 = sqrt(dx1 * dx1 + dz1 * dz1) + 1e-9;
                            const double ix0 = ex[m], iz0 = ez[m], ix1 = ex[m + 1], iz1 = ez[m + 1];
                            const double ox0 = ix0 + dx0 / l0 * sw, oz0 = iz0 + dz0 / l0 * sw;
                            const double ox1 = ix1 + dx1 / l1 * sw, oz1 = iz1 + dz1 / l1 * sw;
                            /* [ROUND 1015 B item 12] No footway slab inside another carriageway.
                             * Where two ring ways run side by side or cross (the Y at Plaza
                             * Moreno, Calle 50 against the Calle 14 arc) each ribbon laid its
                             * kerbs as if it were alone, so the inner footway of one ran across
                             * the other's tarmac: 389 audited samples of pavement standing on
                             * a ring-road surface, z-fighting with it. The slab section is
                             * dropped when its centre lies on another edge's carriageway. */
                            if (td5_env_flag_on("TD5RE_GEO_PLAZA_FW_CLEAR")
                                && tg_net_on_other_road(e, 0.25 * (ix0 + ox0 + ix1 + ox1),
                                                        0.25 * (iz0 + oz0 + iz1 + oz1))) {
                                continue;
                            }
                            /* top slab: near-in, near-out, far-out, far-in */
                            sx[sn] = ix0; sz[sn] = iz0; sy[sn] = y0 + H; su[sn] = 0.0; sv[sn] = v0; sn++;
                            sx[sn] = ox0; sz[sn] = oz0; sy[sn] = y0 + H; su[sn] = uw;  sv[sn] = v0; sn++;
                            sx[sn] = ox1; sz[sn] = oz1; sy[sn] = y1 + H; su[sn] = uw;  sv[sn] = v1; sn++;
                            sx[sn] = ix1; sz[sn] = iz1; sy[sn] = y1 + H; su[sn] = 0.0; sv[sn] = v1; sn++;
                            /* kerb face: bottom on the asphalt, top at the slab */
                            sx[sn] = ix0; sz[sn] = iz0; sy[sn] = y0;     su[sn] = 0.0; sv[sn] = v0; sn++;
                            sx[sn] = ix0; sz[sn] = iz0; sy[sn] = y0 + H; su[sn] = H / (double)TD5_TG_SPAN_LENGTH; sv[sn] = v0; sn++;
                            sx[sn] = ix1; sz[sn] = iz1; sy[sn] = y1 + H; su[sn] = H / (double)TD5_TG_SPAN_LENGTH; sv[sn] = v1; sn++;
                            sx[sn] = ix1; sz[sn] = iz1; sy[sn] = y1;     su[sn] = 0.0; sv[sn] = v1; sn++;
                            snq += 2;
                        }
                        vlen += sl;
                    }
                    if (snq > 0 && *h->nmesh < h->maxmesh - 1) {
                        size_t off2 = h->blk->len;
                        h->moff[(*h->nmesh)++] = off2;
                        if (!tg_write_quad_mesh(h->blk, sx, sy, sz, su, sv, sn, &spage, &snq, 1))
                            return 0;
                        tg_guard_mark(off2, h->blk->len, TG_GK_CROSS, h->si);
                    }
                }
            }
            continue;
        }
        if (e->kind == TG_NE_BACKSTREET) k0 = 0;
        else if (e->kind == TG_NE_COUNTRY && e->npoly > 2) k0 = 1;
        else continue;
        for (k = k0; k + 1 < e->npoly; k++) {
            double px[4], py[4], pz[4], uu[4], vv[4];
            double dx = e->px[k + 1] - e->px[k], dz = e->pz[k + 1] - e->pz[k];
            double len = sqrt(dx * dx + dz * dz), nx, nz;
            int seg_page = TD5_TG_PAGE_R4_CROSS + 0, seg_nq = 1;
            size_t off;
            if (len < 1.0) continue;
            nx = dz / len * e->width * 0.5; nz = -dx / len * e->width * 0.5;
            px[0] = e->px[k] - nx;     pz[0] = e->pz[k] - nz;
            px[1] = e->px[k] + nx;     pz[1] = e->pz[k] + nz;
            px[2] = e->px[k + 1] + nx; pz[2] = e->pz[k + 1] + nz;
            px[3] = e->px[k + 1] - nx; pz[3] = e->pz[k + 1] - nz;
            py[0] = py[1] = tg_world_h(e->px[k], e->pz[k]) + 30.0;
            py[2] = py[3] = tg_world_h(e->px[k + 1], e->pz[k + 1]) + 30.0;
            uu[0] = 0.0; uu[1] = 1.0; uu[2] = 1.0; uu[3] = 0.0;
            vv[0] = vv[1] = 0.0; vv[2] = vv[3] = len / (double)TD5_TG_SPAN_LENGTH;
            if (*h->nmesh >= h->maxmesh - 1) return 1;
            off = h->blk->len;
            h->moff[(*h->nmesh)++] = off;
            if (tg_net_follow_on()) {
                /* [1015 B] cut into span-length pieces, each end on the ground envelope */
                double qx[4 * 24], qy[4 * 24], qz[4 * 24], qu[4 * 24], qv[4 * 24];
                int ks = (int)ceil(len / TG_NET_FOLLOW_SEG), j, qn = 0;
                if (ks < 1) ks = 1;
                if (ks > 24) ks = 24;
                for (j = 0; j < ks; j++) {
                    const double t0 = (double)j / (double)ks, t1 = (double)(j + 1) / (double)ks;
                    const double ax0 = e->px[k] + (e->px[k + 1] - e->px[k]) * t0;
                    const double az0 = e->pz[k] + (e->pz[k + 1] - e->pz[k]) * t0;
                    const double ax1 = e->px[k] + (e->px[k + 1] - e->px[k]) * t1;
                    const double az1 = e->pz[k] + (e->pz[k + 1] - e->pz[k]) * t1;
                    const double y0 = tg_net_ground_y(ax0, az0), y1 = tg_net_ground_y(ax1, az1);
                    qx[qn] = ax0 - nx; qz[qn] = az0 - nz; qy[qn] = y0; qu[qn] = 0.0; qv[qn] = len * t0 / (double)TD5_TG_SPAN_LENGTH; qn++;
                    qx[qn] = ax0 + nx; qz[qn] = az0 + nz; qy[qn] = y0; qu[qn] = 1.0; qv[qn] = len * t0 / (double)TD5_TG_SPAN_LENGTH; qn++;
                    qx[qn] = ax1 + nx; qz[qn] = az1 + nz; qy[qn] = y1; qu[qn] = 1.0; qv[qn] = len * t1 / (double)TD5_TG_SPAN_LENGTH; qn++;
                    qx[qn] = ax1 - nx; qz[qn] = az1 - nz; qy[qn] = y1; qu[qn] = 0.0; qv[qn] = len * t1 / (double)TD5_TG_SPAN_LENGTH; qn++;
                }
                seg_nq = qn / 4;
                if (!tg_write_quad_mesh(h->blk, qx, qy, qz, qu, qv, qn, &seg_page, &seg_nq, 1))
                    return 0;
            } else
            if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4, &seg_page, &seg_nq, 1))
                return 0;
            tg_guard_mark(off, h->blk->len, TG_GK_CROSS, h->si);
            tg_acct_range(TG_ACCT_CROSSING, h->si, h->si);
        }
    }
    return 1;
}

/* --------------------------------------------------------- NETWORK.JSON -- */

void tg_network_write(const char *dir, const TG_NodeList *nl, int nspans_main)
{
    char path[320];
    FILE *fp;
    int i, k, first;
    if (!dir || !nl) return;
    snprintf(path, sizeof path, "%s/NETWORK.JSON", dir);
    fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "{\"seed\":%u,\"sea_level\":%.1f,\"span_length\":%d,\"ring\":%d,\n",
            tg_gen_seed(), tg_world_sea_y(), TD5_TG_SPAN_LENGTH, nspans_main);
    fprintf(fp, "\"spans\":[");
    for (i = 0; i < nspans_main && i < nl->count; i++) {
        const TG_Node *n = &nl->v[i];
        fprintf(fp, "%s[%d,%.0f,%.0f,%.0f,%.0f,%d,%.0f,%d,%d]", i ? "," : "", i,
                n->x, n->z, n->y, n->width, tg_struct_kind(i), tg_road_ground_y(i),
                tg_road_node_wet(i), tg_road_node_forced(i));
        if ((i & 63) == 63) fputc('\n', fp);
    }
    fprintf(fp, "],\n\"structs\":[");
    first = 1;
    for (i = 0; i < nspans_main; ) {
        int e = i;
        if (tg_struct_kind(i) == TG_ST_NONE) { i++; continue; }
        while (e + 1 < nspans_main && tg_struct_kind(e + 1) == tg_struct_kind(i)) e++;
        {
            int q, wet = 0;
            for (q = i; q <= e + 1; q++) if (tg_road_node_wet(q)) { wet = 1; break; }
            fprintf(fp, "%s{\"kind\":\"%s\",\"s0\":%d,\"s1\":%d,\"len\":%d,\"water\":%d}",
                    first ? "" : ",", tg_struct_kind(i) == TG_ST_BRIDGE ? "bridge" : "tunnel",
                    i, e, e - i + 1, wet);
        }
        first = 0;
        i = e + 1;
    }
    fprintf(fp, "],\n\"forks\":[");
    for (i = 0; i < s_fork_count; i++) {
        const TG_Fork *f = &s_forks[i];
        fprintf(fp, "%s{\"i\":%d,\"kind\":%d,\"side\":%d,\"F\":%d,\"L\":%d,\"R\":%d,\"cbase\":%d,\"lat\":[",
                i ? "," : "", i, f->kind, f->side, f->F, f->len, f->R, f->cbase);
        for (k = 0; k <= f->len; k++)
            fprintf(fp, "%s%.0f", k ? "," : "", tg_fork_br_shift(i, k, nl->v[f->F + 1].width));
        fprintf(fp, "]}");
    }
    fprintf(fp, "],\n\"nodes\":[");
    for (i = 0; i < s_nn; i++)
        fprintf(fp, "%s[%.0f,%.0f,%.0f,%d,%d]", i ? "," : "", s_nodes[i].x, s_nodes[i].z,
                s_nodes[i].y, s_nodes[i].kind, s_nodes[i].si);
    fprintf(fp, "],\n\"edges\":[\n");
    for (i = 0; i < s_ne; i++) {
        const TG_NetEdge *e = &s_edges[i];
        fprintf(fp, "%s{\"id\":%d,\"a\":%d,\"b\":%d,\"kind\":\"%s\",\"w\":%.0f,\"drivable\":%d,"
                "\"mouth\":{\"si\":%d,\"left\":%d,\"lo\":%d,\"hi\":%d,\"skew\":%.4f,\"reach\":%.0f},"
                "\"rejoin\":%d,\"poly\":[",
                i ? ",\n" : "", i, e->a, e->b, k_ne_name[e->kind], e->width, e->drivable,
                e->mouth_si, e->mouth_left, e->mouth_lo, e->mouth_hi, e->skew, e->reach,
                e->rejoin_si);
        for (k = 0; k < e->npoly; k++)
            fprintf(fp, "%s[%.0f,%.0f,%.0f]", k ? "," : "", e->px[k], e->pz[k], e->py[k]);
        fprintf(fp, "]}");
    }
    fprintf(fp, "\n]}\n");
    fclose(fp);
}

/* ===== SECTION: minimap street read-back (PORT-ONLY) ===================== */

/* [MINIMAP STREETS 2026-10-07] The in-race minimap draws the side streets of a
 * generated track under the race route (td5_minimap_streets.c). It needs the
 * EDGE POLYLINES, which until now never left this file -- tg_network_write
 * printed them and nothing read them back.
 *
 * These are deliberately dumb accessors over the live arrays rather than a
 * snapshot: the network is rebuilt whole by tg_network_build and never mutated
 * afterwards, so a reader that re-asks after a rebuild sees the new graph with
 * no invalidation protocol. They draw no RNG and write nothing, so a synthetic
 * build is byte-identical whether or not anyone calls them.
 *
 * s_net_built is the gate, NOT s_ne: a reset network keeps its last edge count
 * in s_ne until the next build overwrites it, and serving those stale
 * coordinates would put the previous track's streets on this track's minimap.
 * Declared in td5_trackgen.h so the HUD need not see this file's internals. */
int td5_trackgen_street_edge_count(void)
{
    return s_net_built ? s_ne : 0;
}

int td5_trackgen_street_edge_points(int edge)
{
    if (!s_net_built || edge < 0 || edge >= s_ne) return 0;
    return s_edges[edge].npoly;
}

int td5_trackgen_street_edge_point(int edge, int k, double *x, double *z)
{
    const TG_NetEdge *e;
    if (!s_net_built || edge < 0 || edge >= s_ne) return 0;
    e = &s_edges[edge];
    if (k < 0 || k >= e->npoly) return 0;
    if (x) *x = e->px[k];
    if (z) *z = e->pz[k];
    return 1;
}

int td5_trackgen_street_edge_kind(int edge)
{
    if (!s_net_built || edge < 0 || edge >= s_ne) return -1;
    return s_edges[edge].kind;
}
