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
 */
#include "td5_trackgen_internal.h"
#include "td5_tg_world.h"

#define TG_NET_MAX_EDGES 2048
#define TG_NET_MAX_NODES 4096
#define TG_NET_POLY      16
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
} TG_NetEdge;

typedef struct { short edge; float skew, reach; } TG_NetMouth;

static TG_NetNode  s_nodes[TG_NET_MAX_NODES];
static TG_NetEdge  s_edges[TG_NET_MAX_EDGES];
static TG_NetMouth s_mouth[TD5_TG_MAX_SPANS + 8][2];   /* [si][0=left,1=right] */
static int s_nn, s_ne, s_net_built, s_net_nspans;
static long s_stat_cand, s_stat_short, s_stat_tjunc, s_stat_water, s_stat_road;

static const char *const k_ne_name[TG_NE_KIND_COUNT] = {
    "street", "avenue", "backstreet", "continuation", "country", "underpass"
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

static void tg_net_set_mouth(int lo, int hi, int left, int edge, double skew, double reach)
{
    int s;
    for (s = lo; s <= hi; s++) {
        if (s < 0 || s >= TD5_TG_MAX_SPANS + 8) continue;
        s_mouth[s][left ? 0 : 1].edge  = (short)edge;
        s_mouth[s][left ? 0 : 1].skew  = (float)skew;
        s_mouth[s][left ? 0 : 1].reach = (float)reach;
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

/* -------------------------------------------------------------- build -- */

void tg_network_reset(void)
{
    int s;
    s_nn = s_ne = 0; s_net_built = 0; s_net_nspans = 0;
    s_stat_cand = s_stat_short = s_stat_tjunc = s_stat_water = s_stat_road = 0;
    for (s = 0; s < TD5_TG_MAX_SPANS + 8; s++) {
        s_mouth[s][0].edge = s_mouth[s][1].edge = -1;
        s_mouth[s][0].skew = s_mouth[s][1].skew = 0.0f;
        s_mouth[s][0].reach = s_mouth[s][1].reach = 0.0f;
    }
}

int tg_network_built(void) { return s_net_built; }

void tg_network_build(const TG_NodeList *nl, int nspans_main)
{
    int f, k, counts[TG_NE_KIND_COUNT], i, loops = 0, junc = 0;
    tg_network_reset();
    if (!nl || nspans_main < 2) return;
    if (nspans_main > TD5_TG_MAX_SPANS) nspans_main = TD5_TG_MAX_SPANS;
    s_net_nspans = nspans_main;

    /* The bend continuations first: they open frontage the city streets
     * below read through tg_facade_built_hash. */
    tg_turn_map_build(nl, nspans_main);

    /* Fork corridors into the raster, so no street lands on one. */
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
        }
    }

    /* Underpass crossings FIRST: they are placed by their own period and
     * the streets must stop at them, not the other way round. */
    tg_net_underpasses(nl, nspans_main);
    tg_net_city_streets(nl, nspans_main);
    tg_net_back_streets();
    tg_net_country(nl, nspans_main);
    s_net_built = 1;

    memset(counts, 0, sizeof(counts));
    for (i = 0; i < s_ne; i++) {
        counts[s_edges[i].kind]++;
        if (s_edges[i].rejoin_si >= 0) loops++;
    }
    for (i = 0; i < s_nn; i++) if (s_nodes[i].kind == 1) junc++;
    TD5_LOG_I(LOG_TAG, "trackgen: [NET] %d node(s) %d edge(s): street %d avenue %d "
              "backstreet %d continuation %d country %d (loops %d) underpass %d; "
              "%d junction(s); candidates %ld, dropped short %ld; stops: road %ld "
              "street %ld water/steep %ld",
              s_nn, s_ne, counts[TG_NE_STREET], counts[TG_NE_AVENUE],
              counts[TG_NE_BACKSTREET], counts[TG_NE_CONTINUATION],
              counts[TG_NE_COUNTRY], loops, counts[TG_NE_UNDERPASS], junc,
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

/* ---------------------------------------------------------- emission -- */

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
        fprintf(fp, "%s{\"i\":%d,\"kind\":%d,\"F\":%d,\"L\":%d,\"R\":%d,\"cbase\":%d}",
                i ? "," : "", i, f->kind, f->F, f->len, f->R, f->cbase);
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
