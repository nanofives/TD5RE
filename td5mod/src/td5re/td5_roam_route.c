/**
 * td5_roam_route.c -- GEO FREE ROAM: the route model + planner (PORT-ONLY, F5-A).
 * Contract: td5_roam_route.h. Design: docs/plans/GEO_FREE_ROAM.md "Route, race and barriers".
 *
 * Sections: planner graph (ROAMTILE EDGE records -> compact graph) / snap grid / A* over
 * directed edges / waypoint edits + undo / save + load. Drawing and input are
 * td5_roam_route_ui.c.
 *
 * DETERMINISM. Runs are sorted by (edge key, row0) before they are merged, nodes by key, the
 * snap grid by (cell, segment), so the planner never depends on file order or a pointer.
 * Ties in Dijkstra break on the state index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_track.h"          /* td5_track_quad_vertices */
#include "td5_roam_tile.h"
#include "td5_roam_mode.h"
#include "td5_roam_route.h"
#include "td5_fp.h"
#include "td5_types.h"
#include "td5_race_state.h"     /* [F9b] td5_game_get_actor: the region follows the car */
#include "td5_roam_partido.h"   /* [F9b] which tiles are in the place */
#include "td5_roam_stream.h"    /* [F9b] missing region tiles are built by the stream worker */
#include "td5_roam_race.h"      /* [F9b] no replan under a running race */
#include "deps/cjson/cJSON.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "roam"

#define RR_UPM          430.0
#define RR_SNAP_CELL    55040.0        /* 128 m, place frame units */
#define RR_SNAP_MAX_M   250.0
#define RR_UNDO_MAX     32

/* ============================================================ planner graph == */

typedef struct {
    uint64_t key;
    int64_t  end_key[2];
    int      node[2];
    int      pt0, npt;
    int32_t  minx, maxx, minz, maxz;   /* polyline bbox, place frame */
    double   plen;               /* polyline length, units */
    int8_t   oneway;             /* 0 two-way, +1 a->b, -1 b->a */
    uint8_t  klass, lanes, flags;
} RrEdge;

typedef struct {
    int64_t key;
    double  x, z;
    int     first, ninc;         /* slice of g.inc[]: edge*2 + end */
} RrNode;

typedef struct { int64_t cell; int seg; } RrCellSeg;

static int64_t rr_cell(int cx, int cz) { return (int64_t)(((uint64_t)(uint32_t)cx << 32) | (uint64_t)(uint32_t)cz); }

typedef struct {
    int      ready;
    char     place[64];
    RrEdge  *edge;  int nedge;
    RrNode  *node;  int nnode;
    int     *inc;   int ninc;
    int32_t *pt;    int npt;     /* x,z pairs, place frame */
    double  *cum;                /* per point: units from the edge's first point */
    int     *seg_edge;           /* per point: edge index (segment j = pt j .. j+1 of that edge) */
    RrCellSeg *cs;  int ncs;
    double   ms, km;
    int      ntiles;
    int    (*tiles)[2];          /* [F9b] the tiles read, sorted (tx, tz): the region this graph covers */
    struct RrSig *sig; int nsig; /* [F8] signalised clusters (SIGN records), sorted by key */
} RrGraph;

/* [F8] one signalised cluster: its arms in the tile's ARM order (the furniture's order) */
#define RR_SIG_ARMS 16
typedef struct RrSig { int64_t key; int narm; int16_t bear[RR_SIG_ARMS]; uint64_t ekey[RR_SIG_ARMS]; } RrSig;

static RrGraph g;

typedef struct { uint64_t key; int row0, n, off; int64_t ek0, ek1; int8_t oneway; uint8_t klass, lanes, flags; } RrRun;
typedef struct { int64_t key; double x, z; } RrCent;

typedef struct { void *p; int n, cap; size_t sz; } RrVec;

static void *rr_vec_add(RrVec *v)
{
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 1024;
        void *np = realloc(v->p, (size_t)nc * v->sz);
        if (!np) return NULL;
        v->p = np; v->cap = nc;
    }
    return (char *)v->p + (size_t)v->n++ * v->sz;
}

static int rr_cmp_run(const void *a, const void *b)
{
    const RrRun *x = (const RrRun *)a, *y = (const RrRun *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->row0 != y->row0) return x->row0 < y->row0 ? -1 : 1;
    return 0;
}
static int rr_cmp_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int rr_cmp_cent(const void *a, const void *b)
{
    const RrCent *x = (const RrCent *)a, *y = (const RrCent *)b;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}
static int rr_cmp_sig(const void *a, const void *b)
{
    const RrSig *x = (const RrSig *)a, *y = (const RrSig *)b;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}
static int rr_cmp_cs(const void *a, const void *b)
{
    const RrCellSeg *x = (const RrCellSeg *)a, *y = (const RrCellSeg *)b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    return x->seg < y->seg ? -1 : (x->seg > y->seg ? 1 : 0);
}

/* node key (td5_roam_node_key): (kx << 32) | (uint32)kz, 10-unit cells */
static void rr_key_pos(int64_t key, double *x, double *z)
{
    *x = (double)(int32_t)(key >> 32) * 10.0;
    *z = (double)(int32_t)(uint32_t)(key & 0xFFFFFFFFll) * 10.0;
}

static int rr_find_node_in(const RrGraph *gp, int64_t key)
{
    int lo = 0, hi = gp->nnode - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (gp->node[mid].key == key) return mid;
        if (gp->node[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static int rr_find_node(int64_t key) { return rr_find_node_in(&g, key); }

static int rr_cmp_tile(const void *a, const void *b)
{
    const int *x = (const int *)a, *y = (const int *)b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : (x[1] > y[1] ? 1 : 0);
}

static void rr_graph_free(RrGraph *gp)
{
    free(gp->edge); free(gp->node); free(gp->inc); free(gp->pt); free(gp->cum); free(gp->seg_edge); free(gp->cs);
    free(gp->tiles);
    free(gp->sig);
    memset(gp, 0, sizeof *gp);
}

int td5_roam_route_graph_ready(void) { return g.ready; }

/* [F6-A] ribbon ends the planner graph has at node/cluster `key` (-1 = unknown key): an
 * independent count for the barrier census (td5_roam_barrier.c). */
int td5_roam_route_graph_node_degree(int64_t key)
{
    const int n = g.ready ? rr_find_node(key) : -1;
    return n < 0 ? -1 : g.node[n].ninc;
}


void td5_roam_route_graph_stats(int *nedge, int *nnode, int *nseg, double *ms, double *km)
{
    if (nedge) *nedge = g.nedge;
    if (nnode) *nnode = g.nnode;
    if (nseg)  *nseg  = g.ncs;
    if (ms)    *ms    = g.ms;
    if (km)    *km    = g.km;
}

static int rr_build_into(RrGraph *gp, const char *place, int (*tiles)[2], int ntiles)
{
    RrVec runs = { NULL, 0, 0, sizeof(RrRun) };
    RrVec pts  = { NULL, 0, 0, sizeof(int32_t) };
    RrVec cents = { NULL, 0, 0, sizeof(RrCent) };
    RrVec sigs  = { NULL, 0, 0, sizeof(RrSig) };    /* [F8] */
    int i, e;
    const uint32_t t0 = td5_plat_time_ms();
    int64_t *keys = NULL;
    int nkeys = 0;
    uint32_t t_read = 0, t_merge = 0, t_nodes = 0;

    if (!place || !place[0]) return 0;
    rr_graph_free(gp);
    if (ntiles <= 0) {
        TD5_LOG_W(LOG_TAG, "ROUTE_GRAPH: no ROAMTILE files for '%s' in the region", place);
        return 0;
    }
    for (i = 0; i < ntiles; i++) {
        char path[512], err[128];
        RoamTile t;
        uint32_t k;
        td5_roam_tile_path(path, sizeof path, NULL, place, tiles[i][0], tiles[i][1]);
        memset(&t, 0, sizeof t);
        if (!td5_roam_tile_read(path, &t, 0, err, sizeof err)) {
            TD5_LOG_W(LOG_TAG, "ROUTE_GRAPH: tile %d,%d unreadable (%s)", tiles[i][0], tiles[i][1], err);
            continue;
        }
        for (k = 0; k < t.nedge; k++) {
            const RoamEdgeRec *er = &t.edge[k];
            RrRun *r;
            uint32_t q;
            if (er->span_count == 0 || (uint32_t)er->span_first + er->span_count > t.nspan) continue;
            r = (RrRun *)rr_vec_add(&runs);
            if (!r) goto oom;
            r->key = er->key; r->row0 = er->row0; r->n = er->span_count; r->off = pts.n / 2;
            r->ek0 = er->end_key[0]; r->ek1 = er->end_key[1];
            r->oneway = (int8_t)er->oneway; r->klass = er->klass; r->lanes = er->lanes; r->flags = er->flags;
            for (q = 0; q <= er->span_count; q++) {
                int32_t *pp;
                int vl0, vl1, vr0, vr1, L;
                const TD5_StripSpan *sp;
                double cx, cz;
                if (q < er->span_count) {
                    sp = &t.span[er->span_first + q];
                    L = sp->pad_02[1] & 0x0F; if (L < 1) L = 1;
                    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                    cx = 0.5 * (t.vert[vl0].x + t.vert[vl0 + L].x);
                    cz = 0.5 * (t.vert[vl0].z + t.vert[vl0 + L].z);
                } else {
                    sp = &t.span[er->span_first + q - 1];
                    L = sp->pad_02[1] & 0x0F; if (L < 1) L = 1;
                    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                    cx = 0.5 * (t.vert[vr0].x + t.vert[vr0 + L].x);
                    cz = 0.5 * (t.vert[vr0].z + t.vert[vr0 + L].z);
                }
                pp = (int32_t *)rr_vec_add(&pts); if (!pp) goto oom;
                *pp = t.org_x + sp->origin_x + (int32_t)floor(cx + 0.5);
                pp = (int32_t *)rr_vec_add(&pts); if (!pp) goto oom;
                *pp = t.org_z + sp->origin_z + (int32_t)floor(cz + 0.5);
            }
        }
        /* [F8] signalised clusters: the SIGN records' clusters with their ARM bearings (traffic) */
        for (k = 0; k < t.nsign; k++) {
            const int64_t sk = t.sign[k].cluster_key;
            uint32_t q, j;
            if (k > 0 && t.sign[k - 1].cluster_key == sk) continue;
            for (j = 0; j < k; j++) if (t.sign[j].cluster_key == sk) break;
            if (j < k) continue;
            for (q = 0; q < t.nnode; q++) {
                const RoamNodeRec *nr = &t.node[q];
                RrSig *sg;
                int a;
                if (nr->cluster_key != sk) continue;
                sg = (RrSig *)rr_vec_add(&sigs);
                if (!sg) goto oom;
                sg->key = sk;
                sg->narm = nr->narm > RR_SIG_ARMS ? RR_SIG_ARMS : (int)nr->narm;
                for (a = 0; a < sg->narm; a++) {
                    if ((uint32_t)nr->arm_first + (uint32_t)a >= t.narm) { sg->narm = a; break; }
                    sg->bear[a] = t.arm[nr->arm_first + (uint32_t)a].bearing12;
                    sg->ekey[a] = t.arm[nr->arm_first + (uint32_t)a].edge_key;
                }
                break;
            }
        }
        for (k = 0; k < t.nnode; k++) {
            RrCent *c = (RrCent *)rr_vec_add(&cents);
            if (!c) goto oom;
            c->key = t.node[k].cluster_key;
            c->x = (double)t.org_x + t.node[k].x;
            c->z = (double)t.org_z + t.node[k].z;
        }
        td5_roam_tile_free(&t);
    }
    gp->ntiles = ntiles;
    gp->tiles = (int (*)[2])malloc((size_t)ntiles * sizeof *gp->tiles);
    if (!gp->tiles) goto oom;
    memcpy(gp->tiles, tiles, (size_t)ntiles * sizeof *gp->tiles);
    qsort(gp->tiles, (size_t)ntiles, sizeof *gp->tiles, rr_cmp_tile);
    t_read = td5_plat_time_ms() - t0;
    if (runs.n == 0) goto fail;

    /* ---- merge runs by edge key ------------------------------------------------ */
    qsort(runs.p, (size_t)runs.n, sizeof(RrRun), rr_cmp_run);
    {
        const RrRun *R = (const RrRun *)runs.p;
        const int32_t *P = (const int32_t *)pts.p;
        int nedge = 0, total_pts = 0;
        for (i = 0; i < runs.n; i++) {
            if (i == 0 || R[i].key != R[i - 1].key) nedge++;
            total_pts += R[i].n + 1;
        }
        gp->edge = (RrEdge *)calloc((size_t)nedge, sizeof(RrEdge));
        gp->pt = (int32_t *)malloc((size_t)total_pts * 2 * sizeof(int32_t));
        gp->cum = (double *)malloc((size_t)total_pts * sizeof(double));
        gp->seg_edge = (int *)malloc((size_t)total_pts * sizeof(int));
        if (!gp->edge || !gp->pt || !gp->cum || !gp->seg_edge) goto oom;
        gp->nedge = 0; gp->npt = 0;
        for (i = 0; i < runs.n;) {
            int j = i;
            RrEdge *ed = &gp->edge[gp->nedge];
            int prev_end_row = -1;
            ed->key = R[i].key; ed->end_key[0] = R[i].ek0; ed->end_key[1] = R[i].ek1;
            ed->oneway = R[i].oneway; ed->klass = R[i].klass; ed->lanes = R[i].lanes; ed->flags = R[i].flags;
            ed->pt0 = gp->npt;
            while (j < runs.n && R[j].key == R[i].key) {
                int first = (prev_end_row == R[j].row0) ? 1 : 0;   /* shared row: drop the duplicate */
                int a;
                for (a = first; a <= R[j].n; a++) {
                    gp->pt[2 * gp->npt]     = P[2 * (R[j].off + a)];
                    gp->pt[2 * gp->npt + 1] = P[2 * (R[j].off + a) + 1];
                    gp->npt++;
                }
                prev_end_row = R[j].row0 + R[j].n;
                j++;
            }
            ed->npt = gp->npt - ed->pt0;
            if (ed->npt >= 2) gp->nedge++; else gp->npt = ed->pt0;   /* a one-point edge is no edge */
            i = j;
        }
    }
    /* cumulative distance + per-point edge index + length */
    gp->km = 0.0;
    for (e = 0; e < gp->nedge; e++) {
        RrEdge *ed = &gp->edge[e];
        int a;
        gp->cum[ed->pt0] = 0.0;
        gp->seg_edge[ed->pt0] = e;
        for (a = 1; a < ed->npt; a++) {
            const double dx = (double)gp->pt[2 * (ed->pt0 + a)]     - gp->pt[2 * (ed->pt0 + a - 1)];
            const double dz = (double)gp->pt[2 * (ed->pt0 + a) + 1] - gp->pt[2 * (ed->pt0 + a - 1) + 1];
            gp->cum[ed->pt0 + a] = gp->cum[ed->pt0 + a - 1] + sqrt(dx * dx + dz * dz);
            gp->seg_edge[ed->pt0 + a] = e;
        }
        ed->plen = gp->cum[ed->pt0 + ed->npt - 1];
        gp->km += ed->plen / RR_UPM / 1000.0;
        ed->minx = ed->maxx = gp->pt[2 * ed->pt0]; ed->minz = ed->maxz = gp->pt[2 * ed->pt0 + 1];
        for (a = 1; a < ed->npt; a++) {
            const int32_t x = gp->pt[2 * (ed->pt0 + a)], z = gp->pt[2 * (ed->pt0 + a) + 1];
            if (x < ed->minx) ed->minx = x;
            if (x > ed->maxx) ed->maxx = x;
            if (z < ed->minz) ed->minz = z;
            if (z > ed->maxz) ed->maxz = z;
        }
    }

    t_merge = td5_plat_time_ms() - t0;
    /* ---- nodes ------------------------------------------------------------------ */
    keys = (int64_t *)malloc((size_t)gp->nedge * 2 * sizeof(int64_t));
    if (!keys) goto oom;
    for (e = 0; e < gp->nedge; e++) { keys[nkeys++] = gp->edge[e].end_key[0]; keys[nkeys++] = gp->edge[e].end_key[1]; }
    qsort(keys, (size_t)nkeys, sizeof(int64_t), rr_cmp_i64);
    {
        int u = 0;
        for (i = 0; i < nkeys; i++) if (i == 0 || keys[i] != keys[i - 1]) keys[u++] = keys[i];
        nkeys = u;
    }
    gp->node = (RrNode *)calloc((size_t)nkeys, sizeof(RrNode));
    if (!gp->node) goto oom;
    gp->nnode = nkeys;
    for (i = 0; i < nkeys; i++) { gp->node[i].key = keys[i]; rr_key_pos(keys[i], &gp->node[i].x, &gp->node[i].z); }
    if (cents.n > 0) {
        const RrCent *C;
        qsort(cents.p, (size_t)cents.n, sizeof(RrCent), rr_cmp_cent);
        C = (const RrCent *)cents.p;
        for (i = 0; i < cents.n; i++) {
            const int ni = rr_find_node_in(gp, C[i].key);
            if (ni >= 0) { gp->node[ni].x = C[i].x; gp->node[ni].z = C[i].z; }
        }
    }
    for (e = 0; e < gp->nedge; e++) {
        gp->edge[e].node[0] = rr_find_node_in(gp, gp->edge[e].end_key[0]);
        gp->edge[e].node[1] = rr_find_node_in(gp, gp->edge[e].end_key[1]);
        gp->node[gp->edge[e].node[0]].ninc++;
        gp->node[gp->edge[e].node[1]].ninc++;
    }
    {
        int off = 0;
        for (i = 0; i < gp->nnode; i++) { gp->node[i].first = off; off += gp->node[i].ninc; gp->node[i].ninc = 0; }
        gp->inc = (int *)malloc((size_t)off * sizeof(int));
        if (!gp->inc) goto oom;
        gp->ninc = off;
        for (e = 0; e < gp->nedge; e++) {
            int k;
            for (k = 0; k < 2; k++) {
                RrNode *nd = &gp->node[gp->edge[e].node[k]];
                gp->inc[nd->first + nd->ninc++] = e * 2 + k;
            }
        }
    }

    t_nodes = td5_plat_time_ms() - t0;
    /* ---- snap grid: every polyline segment into the cells its bbox touches ------- */
    {
        RrVec cs = { NULL, 0, 0, sizeof(RrCellSeg) };
        for (e = 0; e < gp->nedge; e++) {
            const RrEdge *ed = &gp->edge[e];
            int a;
            for (a = 0; a + 1 < ed->npt; a++) {
                const int s = ed->pt0 + a;
                const double x0 = gp->pt[2 * s], z0 = gp->pt[2 * s + 1], x1 = gp->pt[2 * s + 2], z1 = gp->pt[2 * s + 3];
                const int cx0 = (int)floor((x0 < x1 ? x0 : x1) / RR_SNAP_CELL), cx1 = (int)floor((x0 < x1 ? x1 : x0) / RR_SNAP_CELL);
                const int cz0 = (int)floor((z0 < z1 ? z0 : z1) / RR_SNAP_CELL), cz1 = (int)floor((z0 < z1 ? z1 : z0) / RR_SNAP_CELL);
                int cx, cz;
                for (cz = cz0; cz <= cz1; cz++)
                    for (cx = cx0; cx <= cx1; cx++) {
                        RrCellSeg *c = (RrCellSeg *)rr_vec_add(&cs);
                        if (!c) { free(cs.p); goto oom; }
                        c->cell = rr_cell(cx, cz);
                        c->seg = s;
                    }
            }
        }
        qsort(cs.p, (size_t)cs.n, sizeof(RrCellSeg), rr_cmp_cs);
        gp->cs = (RrCellSeg *)cs.p; gp->ncs = cs.n;
    }

    if (sigs.n > 0) {
        RrSig *S = (RrSig *)sigs.p;
        int u = 0;
        qsort(S, (size_t)sigs.n, sizeof(RrSig), rr_cmp_sig);
        for (i = 0; i < sigs.n; i++) if (i == 0 || S[i].key != S[u - 1].key) S[u++] = S[i];
        gp->sig = S; gp->nsig = u;
        sigs.p = NULL;
    }
    free(keys); free(runs.p); free(pts.p); free(cents.p);
    snprintf(gp->place, sizeof gp->place, "%s", place);
    gp->ms = (double)(td5_plat_time_ms() - t0);
    gp->ready = 1;
    TD5_LOG_I(LOG_TAG, "ROUTE_GRAPH: place=%s tiles=%d edges=%d nodes=%d segments=%d road=%.1f km built in %.0f ms "
              "(read %u, merge %u, nodes %u, grid %u) signal clusters=%d",
              place, ntiles, gp->nedge, gp->nnode, gp->ncs, gp->km, gp->ms, (unsigned)t_read, (unsigned)(t_merge - t_read),
              (unsigned)(t_nodes - t_merge), (unsigned)((uint32_t)gp->ms - t_nodes), gp->nsig);
    return 1;

oom:
    TD5_LOG_E(LOG_TAG, "ROUTE_GRAPH: out of memory");
fail:
    free(sigs.p);
    free(keys); free(runs.p); free(pts.p); free(cents.p);
    rr_graph_free(gp);
    return 0;
}

/* [F9b] the region manager (async build, swap, poll) lives after the route state below */
static int rr_region_ensure(const char *place);

int td5_roam_route_graph_ensure(const char *place)
{
    return rr_region_ensure(place);
}

int td5_roam_route_graph_query(double minx, double minz, double maxx, double maxz,
                               TD5_RouteGfxCb cb, void *user)
{
    int e, n = 0;
    if (!g.ready || !cb) return 0;
    for (e = 0; e < g.nedge; e++) {
        const RrEdge *ed = &g.edge[e];
        if ((double)ed->maxx < minx || (double)ed->minx > maxx || (double)ed->maxz < minz || (double)ed->minz > maxz)
            continue;
        cb(user, ed->klass, ed->lanes, ed->oneway, g.pt + 2 * ed->pt0, ed->npt);
        n++;
    }
    return n;
}

int td5_roam_route_graph_links(double minx, double minz, double maxx, double maxz,
                               TD5_RouteLinkCb cb, void *user)
{
    int n, k, cnt = 0;
    if (!g.ready || !cb) return 0;
    for (n = 0; n < g.nnode; n++) {
        const RrNode *nd = &g.node[n];
        if (nd->x < minx || nd->x > maxx || nd->z < minz || nd->z > maxz) continue;
        for (k = 0; k < nd->ninc; k++) {
            const int inc = g.inc[nd->first + k];
            const RrEdge *ed = &g.edge[inc >> 1];
            const int p = (inc & 1) ? ed->pt0 + ed->npt - 1 : ed->pt0;
            const double ex = g.pt[2 * p], ez = g.pt[2 * p + 1];
            const double dx = ex - nd->x, dz = ez - nd->z;
            if (dx * dx + dz * dz < (1.5 * RR_UPM) * (1.5 * RR_UPM)) continue;
            cb(user, ed->klass, nd->x, nd->z, ex, ez);
            cnt++;
        }
    }
    return cnt;
}

/* ============================================================ snap ============ */

typedef struct {
    int    edge, seg;
    double t, along;             /* units from the edge's first point */
    double x, z, d;              /* snapped point (place), distance moved (units) */
} RrSnap;

static int rr_cs_lower(int64_t cell)
{
    int lo = 0, hi = g.ncs;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (g.cs[mid].cell < cell) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void rr_try_cell(int cx, int cz, double px, double pz, RrSnap *best)
{
    const int64_t cell = rr_cell(cx, cz);
    int i = rr_cs_lower(cell);
    for (; i < g.ncs && g.cs[i].cell == cell; i++) {
        const int s = g.cs[i].seg;
        const double ax = g.pt[2 * s], az = g.pt[2 * s + 1];
        const double bx = g.pt[2 * s + 2], bz = g.pt[2 * s + 3];
        const double dx = bx - ax, dz = bz - az;
        const double l2 = dx * dx + dz * dz;
        double t = l2 > 0.0 ? ((px - ax) * dx + (pz - az) * dz) / l2 : 0.0;
        double qx, qz, d;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        qx = ax + t * dx; qz = az + t * dz;
        d = sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
        if (d < best->d || (d == best->d && s < best->seg)) {
            const int e = g.seg_edge[s];
            best->d = d; best->seg = s; best->edge = e; best->t = t;
            best->x = qx; best->z = qz;
            best->along = g.cum[s] + t * sqrt(l2);
        }
    }
}

static int rr_snap(double px, double pz, double max_m, RrSnap *out)
{
    const int cx = (int)floor(px / RR_SNAP_CELL), cz = (int)floor(pz / RR_SNAP_CELL);
    const int rmax = (int)ceil(max_m * RR_UPM / RR_SNAP_CELL) + 1;
    int r, dx, dz;
    RrSnap best;
    memset(&best, 0, sizeof best);
    best.edge = -1; best.d = 1e30;
    if (!g.ready) return 0;
    for (r = 0; r <= rmax; r++) {
        if (r >= 2 && best.edge >= 0 && best.d <= (double)(r - 1) * RR_SNAP_CELL) break;
        for (dz = -r; dz <= r; dz++)
            for (dx = -r; dx <= r; dx++) {
                if ((dx < 0 ? -dx : dx) != r && (dz < 0 ? -dz : dz) != r) continue;   /* ring only */
                rr_try_cell(cx + dx, cz + dz, px, pz, &best);
            }
    }
    if (best.edge < 0 || best.d > max_m * RR_UPM) return 0;
    *out = best;
    return 1;
}

int td5_roam_route_snap_xz(double px, double pz, double max_m, double *sx, double *sz,
                           uint64_t *edge_key, double *dist_m)
{
    RrSnap s;
    if (!rr_snap(px, pz, max_m, &s)) return 0;
    if (sx) *sx = s.x;
    if (sz) *sz = s.z;
    if (edge_key) *edge_key = g.edge[s.edge].key;
    if (dist_m) *dist_m = s.d / RR_UPM;
    return 1;
}

/* ============================================================ A* over edges === */

typedef struct { double *x, *z; int n, cap; } RrPoly;
typedef struct { TD5_RouteEdgeRef *r; int n, cap; } RrERefs;

static void rr_poly_add(RrPoly *p, double x, double z)
{
    if (p->n > 0) {
        const double dx = x - p->x[p->n - 1], dz = z - p->z[p->n - 1];
        if (dx * dx + dz * dz < 0.25) return;
    }
    if (p->n >= p->cap) {
        const int nc = p->cap ? p->cap * 2 : 1024;
        double *nx = (double *)realloc(p->x, (size_t)nc * sizeof(double));
        double *nz = (double *)realloc(p->z, (size_t)nc * sizeof(double));
        if (!nx || !nz) { if (nx) p->x = nx; if (nz) p->z = nz; return; }
        p->x = nx; p->z = nz; p->cap = nc;
    }
    p->x[p->n] = x; p->z[p->n] = z; p->n++;
}

static void rr_eref_add(RrERefs *r, uint64_t key, int dir)
{
    if (r->n > 0 && r->r[r->n - 1].key == key && r->r[r->n - 1].dir == dir) return;
    if (r->n >= r->cap) {
        const int nc = r->cap ? r->cap * 2 : 256;
        TD5_RouteEdgeRef *nr = (TD5_RouteEdgeRef *)realloc(r->r, (size_t)nc * sizeof *nr);
        if (!nr) return;
        r->r = nr; r->cap = nc;
    }
    r->r[r->n].key = key; r->r[r->n].dir = dir; r->n++;
}

/* The point of edge e at arc length a (clamped to the edge). */
static void rr_point_at(int e, double a, double *x, double *z)
{
    const RrEdge *ed = &g.edge[e];
    const int pt0 = ed->pt0;
    int lo = 0, hi = ed->npt - 2;
    double l, t;
    if (a < 0.0) a = 0.0;
    if (a > ed->plen) a = ed->plen;
    while (lo < hi) {                     /* last segment whose start cum <= a */
        const int mid = (lo + hi + 1) / 2;
        if (g.cum[pt0 + mid] <= a) lo = mid; else hi = mid - 1;
    }
    l = g.cum[pt0 + lo + 1] - g.cum[pt0 + lo];
    t = l > 0.0 ? (a - g.cum[pt0 + lo]) / l : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    *x = g.pt[2 * (pt0 + lo)]     + t * (g.pt[2 * (pt0 + lo + 1)]     - g.pt[2 * (pt0 + lo)]);
    *z = g.pt[2 * (pt0 + lo) + 1] + t * (g.pt[2 * (pt0 + lo + 1) + 1] - g.pt[2 * (pt0 + lo) + 1]);
}

/* The polyline of edge e between two arc lengths, in the order a0 -> a1. */
static void rr_slice(RrPoly *p, int e, double a0, double a1)
{
    const RrEdge *ed = &g.edge[e];
    const int pt0 = ed->pt0;
    double x, z;
    int i;
    rr_point_at(e, a0, &x, &z);
    rr_poly_add(p, x, z);
    if (a0 <= a1) {
        for (i = 0; i < ed->npt; i++)
            if (g.cum[pt0 + i] > a0 && g.cum[pt0 + i] < a1)
                rr_poly_add(p, g.pt[2 * (pt0 + i)], g.pt[2 * (pt0 + i) + 1]);
    } else {
        for (i = ed->npt - 1; i >= 0; i--)
            if (g.cum[pt0 + i] < a0 && g.cum[pt0 + i] > a1)
                rr_poly_add(p, g.pt[2 * (pt0 + i)], g.pt[2 * (pt0 + i) + 1]);
    }
    rr_point_at(e, a1, &x, &z);
    rr_poly_add(p, x, z);
}

/* node centre between two edges: added when the edges' ends are not at the node (a patch) */
static void rr_connector(RrPoly *p, int node)
{
    const RrNode *nd = &g.node[node];
    double lx, lz;
    if (p->n < 1) return;
    lx = p->x[p->n - 1]; lz = p->z[p->n - 1];
    if ((nd->x - lx) * (nd->x - lx) + (nd->z - lz) * (nd->z - lz) > (3.0 * RR_UPM) * (3.0 * RR_UPM))
        rr_poly_add(p, nd->x, nd->z);
}

static int rr_allowed(int e, int d)
{
    const int8_t ow = g.edge[e].oneway;
    if (ow > 0) return d == 0;
    if (ow < 0) return d == 1;
    return 1;
}
static int rr_end_node(int e, int d) { return g.edge[e].node[d == 0 ? 1 : 0]; }
static int rr_start_node(int e, int d) { return g.edge[e].node[d == 0 ? 0 : 1]; }

typedef struct { double c; int s; } RrHeapItem;
typedef struct { RrHeapItem *h; int n, cap; } RrHeap;

static void rr_heap_push(RrHeap *hp, double c, int s)
{
    int i;
    if (hp->n >= hp->cap) {
        const int nc = hp->cap ? hp->cap * 2 : 4096;
        RrHeapItem *nh = (RrHeapItem *)realloc(hp->h, (size_t)nc * sizeof *nh);
        if (!nh) return;
        hp->h = nh; hp->cap = nc;
    }
    i = hp->n++;
    while (i > 0) {
        const int par = (i - 1) / 2;
        if (hp->h[par].c < c || (hp->h[par].c == c && hp->h[par].s < s)) break;
        hp->h[i] = hp->h[par]; i = par;
    }
    hp->h[i].c = c; hp->h[i].s = s;
}
static int rr_heap_pop(RrHeap *hp, RrHeapItem *out)
{
    RrHeapItem last;
    int i = 0;
    if (hp->n == 0) return 0;
    *out = hp->h[0];
    last = hp->h[--hp->n];
    while (1) {
        int ch = 2 * i + 1;
        if (ch >= hp->n) break;
        if (ch + 1 < hp->n && (hp->h[ch + 1].c < hp->h[ch].c ||
                               (hp->h[ch + 1].c == hp->h[ch].c && hp->h[ch + 1].s < hp->h[ch].s))) ch++;
        if (last.c < hp->h[ch].c || (last.c == hp->h[ch].c && last.s < hp->h[ch].s)) break;
        hp->h[i] = hp->h[ch]; i = ch;
    }
    if (hp->n > 0) hp->h[i] = last;
    return 1;
}

static int rr_uturn(int e, int d, int e2, int d2, int node)
{
    return e == e2 && d != d2 && g.node[node].ninc > 1;
}

/* One leg S -> T. Appends to poly + refs. 1 on success. */
static int rr_plan_leg(const RrSnap *S, const RrSnap *T, RrPoly *poly, RrERefs *refs)
{
    const int ns = g.nedge * 2;
    double *cost = (double *)malloc((size_t)ns * sizeof(double));
    int *parent = (int *)malloc((size_t)ns * sizeof(int));
    RrHeap hp = { NULL, 0, 0 };
    RrHeapItem it;
    double best = 1e300;
    int best_state = -2, best_dw = 0, k;
    const RrEdge *te = &g.edge[T->edge];
    const double extra[2] = { T->along, te->plen - T->along };
    int ok = 0;

    if (!cost || !parent) { free(cost); free(parent); return 0; }
    for (k = 0; k < ns; k++) { cost[k] = 1e300; parent[k] = -1; }

    if (S->edge == T->edge) {
        if (rr_allowed(S->edge, 0) && T->along >= S->along) { best = T->along - S->along; best_state = -1; best_dw = 0; }
        if (rr_allowed(S->edge, 1) && T->along <= S->along && S->along - T->along < best) {
            best = S->along - T->along; best_state = -1; best_dw = 1;
        }
    }
    {
        const RrEdge *se = &g.edge[S->edge];
        if (rr_allowed(S->edge, 0)) { cost[S->edge * 2] = se->plen - S->along; rr_heap_push(&hp, cost[S->edge * 2], S->edge * 2); }
        if (rr_allowed(S->edge, 1)) { cost[S->edge * 2 + 1] = S->along; rr_heap_push(&hp, cost[S->edge * 2 + 1], S->edge * 2 + 1); }
    }
    while (rr_heap_pop(&hp, &it)) {
        const int s = it.s, e = s >> 1, d = s & 1;
        int n, dw, q;
        if (it.c >= best) break;
        if (it.c > cost[s]) continue;
        n = rr_end_node(e, d);
        for (dw = 0; dw < 2; dw++) {
            if (!rr_allowed(T->edge, dw) || rr_start_node(T->edge, dw) != n) continue;
            if (rr_uturn(e, d, T->edge, dw, n)) continue;
            if (it.c + extra[dw] < best) { best = it.c + extra[dw]; best_state = s; best_dw = dw; }
        }
        for (q = 0; q < g.node[n].ninc; q++) {
            const int inc = g.inc[g.node[n].first + q];
            const int e2 = inc >> 1, d2 = inc & 1;     /* leaving through end d2 = travelling in direction d2 */
            const int s2 = e2 * 2 + d2;
            double c2;
            if (!rr_allowed(e2, d2) || rr_uturn(e, d, e2, d2, n)) continue;
            c2 = it.c + g.edge[e2].plen;
            if (c2 < cost[s2]) { cost[s2] = c2; parent[s2] = s; rr_heap_push(&hp, c2, s2); }
        }
    }
    if (best_state == -2) goto done;

    if (best_state == -1) {
        rr_slice(poly, S->edge, S->along, T->along);
        rr_eref_add(refs, g.edge[S->edge].key, best_dw == 0 ? 1 : -1);
    } else {
        int chain[8192], nc = 0, s = best_state, a;
        while (s >= 0 && nc < 8192) { chain[nc++] = s; s = parent[s]; }
        if (s >= 0) goto done;                           /* chain too long: refuse */
        for (a = nc - 1; a >= 0; a--) {
            const int e = chain[a] >> 1, d = chain[a] & 1;
            const RrEdge *ed = &g.edge[e];
            if (a == nc - 1) {
                rr_slice(poly, e, S->along, d == 0 ? ed->plen : 0.0);
            } else {
                rr_connector(poly, rr_start_node(e, d));
                rr_slice(poly, e, d == 0 ? 0.0 : ed->plen, d == 0 ? ed->plen : 0.0);
            }
            rr_eref_add(refs, ed->key, d == 0 ? 1 : -1);
        }
        rr_connector(poly, rr_start_node(T->edge, best_dw));
        rr_slice(poly, T->edge, best_dw == 0 ? 0.0 : te->plen, T->along);
        rr_eref_add(refs, te->key, best_dw == 0 ? 1 : -1);
    }
    ok = 1;
done:
    free(cost); free(parent); free(hp.h);
    return ok;
}

/* ============================================================ the route ======= */

typedef struct {
    TD5_RouteWp wp[TD5_ROUTE_MAX_WP];
    int nwp;
    int type, laps;
} RrWpSet;

static struct {
    RrWpSet cur;
    RrWpSet undo[RR_UNDO_MAX];
    int nundo;
    TD5_RouteResult res;
    char name[TD5_ROUTE_NAME_MAX];
    int map_changed;
    uint32_t loaded_hash;
} rt;

static void rr_result_free(void)
{
    free(rt.res.edge); free(rt.res.px); free(rt.res.pz); free(rt.res.dist_m);
    memset(&rt.res, 0, sizeof rt.res);
}

const char *td5_roam_route_place(void)
{
    const TD5_RoamSpawn *sp = td5_roam_spawn();
    return (sp && sp->place[0]) ? sp->place : "la_plata_partido";
}

double td5_roam_route_upm(void) { return RR_UPM; }

int td5_roam_route_place_to_latlon(double px, double pz, double *lat, double *lon)
{
    return td5_roam_place_to_latlon(td5_roam_route_place(), px, pz, lat, lon);
}
int td5_roam_route_latlon_to_place(double lat, double lon, int32_t *px, int32_t *pz)
{
    return td5_roam_latlon_to_place(td5_roam_route_place(), lat, lon, px, pz);
}

/* ============================================================ the region ====== */
/* [F9b] THE PLANNER GRAPH COVERS A REGION, NOT EVERY TILE OF THE PLACE. The partido holds ~13,500
 * lattice tiles and most have no ROAMTILE file until something builds it, so the graph is built from
 * the files of a REGION: the casco list (always, so casco routes plan exactly as before), a ring round
 * the car (RR_REG_CAR tiles), a ring round every waypoint (RR_REG_WP) and a corridor RR_REG_CORRIDOR_M
 * either side of every leg (closing leg of a circuit included). A region tile with no file is queued
 * on the stream worker (td5_roam_stream_plan_tile: build + write, below every load). The graph is
 * rebuilt OFF the main thread from the files present and swapped in between frames
 * (td5_roam_route_graph_poll); a swap bumps the generation, re-snaps the waypoints and replans.
 * A waypoint whose tile the graph does not cover yet is kept unsnapped (the route is "mapping") until
 * a swap covers it. */
#define RR_REG_CAR          6            /* tiles round the car (1.5 km)                */
#define RR_REG_WP           4            /* tiles round a waypoint (1 km)               */
#define RR_REG_CORRIDOR_M   1500.0       /* either side of a leg                        */
#define RR_REG_POLL_MS      400u
#define RR_REG_REBUILD_MS   4000u        /* rebuild with what is there while builds still run */

typedef struct { int tx, tz; uint8_t st; } RrRegTile;    /* st: 0 unknown, 1 queued, 2 file present, 3 absent */

static struct {
    char       place[64];
    RoamGrid   grid;
    int        grid_ok;
    RrRegTile *t;  int n, cap;           /* the wanted region, sorted (tx, tz)            */
    uint32_t   key;                       /* hash of the inputs it was computed from       */
    int        queued, present;
#ifdef _WIN32
    volatile LONG building, next_ready;
#else
    int        building, next_ready;
#endif
    RrGraph    next;                      /* built off-thread, swapped in by the poll      */
    int      (*blist)[2]; int nb;         /* the list the running build reads (owned)      */
    char       bplace[64];
    uint32_t   next_poll_ms, last_build_ms;
    long       gen;
    int        hash_check, have_hash;     /* a loaded route whose stored hash is judged once it plans */
    uint32_t   want_hash;
    /* [F9b x F8] the previous generation's edge / node keys by index: an index a consumer (traffic) kept from
     * generation gen - 1 is translated through its key */
    uint64_t  *prev_ekey; int prev_ne;
    int64_t   *prev_nkey; int prev_nn;
} rg;

static int  rr_wp_snap(TD5_RouteWp *w);
static void rr_region_swap(void);

static const RoamGrid *rr_grid(const char *place)
{
    if (!rg.grid_ok || strcmp(rg.place, place) != 0) {
        char path[512];
        TD5_File *f;
        snprintf(rg.place, sizeof rg.place, "%s", place);
        rg.grid_ok = 0;
        td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
        f = td5_plat_file_open(path, "rb");
        if (f) {
            const int64_t n = td5_plat_file_size(f);
            char *buf = (n > 0 && n < 65536) ? (char *)malloc((size_t)n + 1u) : NULL;
            if (buf && td5_plat_file_read(f, buf, (size_t)n) == (size_t)n) { buf[n] = '\0'; rg.grid_ok = roam_grid_parse(buf, &rg.grid); }
            free(buf);
            td5_plat_file_close(f);
        }
        td5_roam_partido_load(place);
    }
    return rg.grid_ok ? &rg.grid : NULL;
}

static int rr_reg_cmp(const void *a, const void *b)
{
    const RrRegTile *x = (const RrRegTile *)a, *y = (const RrRegTile *)b;
    if (x->tx != y->tx) return x->tx < y->tx ? -1 : 1;
    return x->tz < y->tz ? -1 : (x->tz > y->tz ? 1 : 0);
}

static void rr_reg_add(int tx, int tz)
{
    if (rg.n >= rg.cap) {
        const int nc = rg.cap ? rg.cap * 2 : 2048;
        RrRegTile *nt = (RrRegTile *)realloc(rg.t, (size_t)nc * sizeof *nt);
        if (!nt) return;
        rg.t = nt; rg.cap = nc;
    }
    rg.t[rg.n].tx = tx; rg.t[rg.n].tz = tz; rg.t[rg.n].st = 0;
    rg.n++;
}

static void rr_reg_ring(const RoamGrid *gr, double px, double pz, int R)
{
    int ctx, ctz, dx, dz;
    roam_grid_tile_of(gr, px, pz, &ctx, &ctz);
    for (dz = -R; dz <= R; dz++)
        for (dx = -R; dx <= R; dx++)
            if (td5_roam_partido_tile_ok(gr, ctx + dx, ctz + dz)) rr_reg_add(ctx + dx, ctz + dz);
}

/* every tile whose centre lies within RR_REG_CORRIDOR_M (+ half a tile diagonal) of segment a-b */
static void rr_reg_corridor(const RoamGrid *gr, double ax, double az, double bx, double bz)
{
    const double M = RR_REG_CORRIDOR_M * RR_UPM + 0.71 * (double)gr->tile, S = (double)gr->tile;
    const double ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    double ua, va, ub, vb;
    int tx, tz, tx0, tx1, tz0, tz1;
    roam_grid_uv(gr, ax, az, &ua, &va);
    roam_grid_uv(gr, bx, bz, &ub, &vb);
    tx0 = (int)floor(((ua < ub ? ua : ub) - M) / S); tx1 = (int)floor(((ua > ub ? ua : ub) + M) / S);
    tz0 = (int)floor(((va < vb ? va : vb) - M) / S); tz1 = (int)floor(((va > vb ? va : vb) + M) / S);
    if ((long)(tx1 - tx0 + 1) * (long)(tz1 - tz0 + 1) > 400000L) return;      /* a waypoint pair across the world */
    for (tx = tx0; tx <= tx1; tx++)
        for (tz = tz0; tz <= tz1; tz++) {
            double c[4][2], cx = 0.0, cz = 0.0, t, qx, qz;
            int k;
            roam_grid_corners(gr, tx, tz, c);
            for (k = 0; k < 4; k++) { cx += 0.25 * c[k][0]; cz += 0.25 * c[k][1]; }
            t = l2 > 0.0 ? ((cx - ax) * ex + (cz - az) * ez) / l2 : 0.0;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            qx = ax + ex * t - cx; qz = az + ez * t - cz;
            if (qx * qx + qz * qz > M * M) continue;
            if (td5_roam_partido_tile_ok(gr, tx, tz)) rr_reg_add(tx, tz);
        }
}

/* the car's PLACE position (slot 0), else the session spawn */
static int rr_reg_car(double *px, double *pz)
{
    const TD5_Actor *a = td5_roam_active() ? td5_game_get_actor(0) : NULL;
    if (a) {
        int32_t ox, oz;
        td5_roam_world_origin(&ox, &oz);
        *px = (double)FP_TRUNC(a->world_pos.x) + ox;
        *pz = (double)FP_TRUNC(a->world_pos.z) + oz;
        return 1;
    }
    {
        const TD5_RoamSpawn *sp = td5_roam_spawn();
        if (!sp) return 0;
        *px = (double)sp->spawn_px; *pz = (double)sp->spawn_pz;
        return 1;
    }
}

/* Recompute the wanted region when its inputs changed (car tile in 2-tile steps, waypoints, type). */
static void rr_region_update(const char *place)
{
    const RoamGrid *gr = rr_grid(place);
    uint32_t h = 2166136261u;
    double cx = 0.0, cz = 0.0;
    int have_car, ctx = 0, ctz = 0, i, nlegs;
    RrRegTile *old;
    int nold;
    if (!gr) return;
    have_car = rr_reg_car(&cx, &cz);
    if (have_car) roam_grid_tile_of(gr, cx, cz, &ctx, &ctz);
#define RR_H(v) do { h ^= (uint32_t)(v); h *= 16777619u; } while (0)
    RR_H(have_car); RR_H(ctx >> 1); RR_H(ctz >> 1);
    RR_H(rt.cur.nwp); RR_H(rt.cur.type);
    for (i = 0; i < rt.cur.nwp; i++) { RR_H(rt.cur.wp[i].px); RR_H(rt.cur.wp[i].pz); }
#undef RR_H
    if (rg.n > 0 && h == rg.key) return;
    rg.key = h;
    /* keep the known states of tiles that stay wanted */
    old = rg.t; nold = rg.n;
    rg.t = NULL; rg.n = 0; rg.cap = 0;
    {
        int (*casco)[2] = (int (*)[2])malloc(2048u * sizeof *casco);
        const int nc = casco ? td5_roam_casco_tiles(gr, casco, 2048) : 0;
        for (i = 0; i < nc; i++) rr_reg_add(casco[i][0], casco[i][1]);
        free(casco);
    }
    if (have_car) rr_reg_ring(gr, cx, cz, RR_REG_CAR);
    for (i = 0; i < rt.cur.nwp; i++) rr_reg_ring(gr, (double)rt.cur.wp[i].px, (double)rt.cur.wp[i].pz, RR_REG_WP);
    nlegs = rt.cur.nwp - 1;
    if (rt.cur.type == TD5_ROUTE_CIRCUIT && rt.cur.nwp > 1 && rt.cur.wp[rt.cur.nwp - 1].kind == TD5_ROUTE_WP_FINISH) nlegs++;
    for (i = 0; i < nlegs; i++) {
        const TD5_RouteWp *a = &rt.cur.wp[i], *b = &rt.cur.wp[(i + 1) % rt.cur.nwp];
        rr_reg_corridor(gr, (double)a->px, (double)a->pz, (double)b->px, (double)b->pz);
    }
    if (rg.n > 1) {
        int u = 0;
        qsort(rg.t, (size_t)rg.n, sizeof *rg.t, rr_reg_cmp);
        for (i = 0; i < rg.n; i++)
            if (i == 0 || rg.t[i].tx != rg.t[u - 1].tx || rg.t[i].tz != rg.t[u - 1].tz) rg.t[u++] = rg.t[i];
        rg.n = u;
    }
    if (old) {
        int j = 0;
        for (i = 0; i < rg.n; i++) {
            while (j < nold && rr_reg_cmp(&old[j], &rg.t[i]) < 0) j++;
            if (j < nold && old[j].tx == rg.t[i].tx && old[j].tz == rg.t[i].tz) rg.t[i].st = old[j].st;
        }
        free(old);
    }
}

/* File states: present / queued on the stream worker / absent. */
static void rr_region_refresh(const char *place)
{
    int i;
    rg.queued = rg.present = 0;
    for (i = 0; i < rg.n; i++) {
        RrRegTile *t = &rg.t[i];
        if (t->st == 0 || t->st == 1) {
            const int r = td5_roam_stream_plan_tile(t->tx, t->tz);
            if (r == 1) t->st = 2;
            else if (r == 0) t->st = 1;
            else {
                char path[512];
                td5_roam_tile_path(path, sizeof path, NULL, place, t->tx, t->tz);
                t->st = td5_plat_file_exists(path) ? 2 : 3;
            }
        }
        if (t->st == 1) rg.queued++;
        if (t->st == 2) rg.present++;
    }
}

/* 1 when the present region tiles are exactly the graph's tiles */
static int rr_region_matches_graph(void)
{
    int i, k = 0;
    if (!g.ready || g.ntiles != rg.present) return 0;
    for (i = 0; i < rg.n; i++) {
        if (rg.t[i].st != 2) continue;
        if (k >= g.ntiles || g.tiles[k][0] != rg.t[i].tx || g.tiles[k][1] != rg.t[i].tz) return 0;
        k++;
    }
    return 1;
}

static void rr_region_take_list(const char *place)
{
    int i;
    free(rg.blist);
    rg.blist = (int (*)[2])malloc((size_t)(rg.present > 0 ? rg.present : 1) * sizeof *rg.blist);
    rg.nb = 0;
    for (i = 0; rg.blist && i < rg.n; i++)
        if (rg.t[i].st == 2) { rg.blist[rg.nb][0] = rg.t[i].tx; rg.blist[rg.nb][1] = rg.t[i].tz; rg.nb++; }
    snprintf(rg.bplace, sizeof rg.bplace, "%s", place);
}

#ifdef _WIN32
static DWORD WINAPI rr_region_thread(LPVOID unused)
{
    (void)unused;
    rr_build_into(&rg.next, rg.bplace, rg.blist, rg.nb);
    MemoryBarrier();
    InterlockedExchange(&rg.next_ready, 1);
    InterlockedExchange(&rg.building, 0);
    return 0;
}
#endif

static void rr_region_start_build(const char *place)
{
    rr_region_take_list(place);
    rg.last_build_ms = td5_plat_time_ms();
#ifdef _WIN32
    {
        HANDLE h;
        InterlockedExchange(&rg.building, 1);
        h = CreateThread(NULL, 0, rr_region_thread, NULL, 0, NULL);
        if (h) { CloseHandle(h); return; }
        InterlockedExchange(&rg.building, 0);
    }
#endif
    rr_build_into(&rg.next, rg.bplace, rg.blist, rg.nb);
    rg.next_ready = 1;
}

static void rr_region_wait(void)
{
#ifdef _WIN32
    while (rg.building) Sleep(1);
    MemoryBarrier();
#endif
}

/* swap the off-thread graph in (main thread, between frames), re-snap, replan */
static void rr_region_swap(void)
{
    int i;
    if (!rg.next_ready) return;
    rg.next_ready = 0;
    if (!rg.next.ready) { rr_graph_free(&rg.next); return; }
    {   /* [F9b x F8] keep the outgoing generation's keys for td5_roam_route_graph_remap_* */
        int k;
        free(rg.prev_ekey); free(rg.prev_nkey);
        rg.prev_ekey = g.nedge > 0 ? (uint64_t *)malloc((size_t)g.nedge * sizeof(uint64_t)) : NULL;
        rg.prev_nkey = g.nnode > 0 ? (int64_t *)malloc((size_t)g.nnode * sizeof(int64_t)) : NULL;
        rg.prev_ne = rg.prev_ekey ? g.nedge : 0;
        rg.prev_nn = rg.prev_nkey ? g.nnode : 0;
        for (k = 0; k < rg.prev_ne; k++) rg.prev_ekey[k] = g.edge[k].key;
        for (k = 0; k < rg.prev_nn; k++) rg.prev_nkey[k] = g.node[k].key;
    }
    rr_graph_free(&g);
    g = rg.next;
    memset(&rg.next, 0, sizeof rg.next);
    rg.gen++;
    TD5_LOG_I(LOG_TAG, "ROUTE_GRAPH_SWAP gen=%ld tiles=%d edges=%d %.1f km (region %d tiles, %d still queued)", rg.gen,
              g.ntiles, g.nedge, g.km, rg.n, rg.queued);
    if (rt.cur.nwp > 0 && td5_roam_race_state() < TD5_RRACE_COUNTDOWN) {
        const uint32_t h0 = rt.res.hash;
        const int ok0 = rt.res.ok;
        for (i = 0; i < rt.cur.nwp; i++) rr_wp_snap(&rt.cur.wp[i]);
        td5_roam_route_replan();
        if (rg.hash_check && rt.res.ok) {
            rt.map_changed = rg.have_hash && rg.want_hash != rt.res.hash;
            rg.hash_check = 0;
            TD5_LOG_I(LOG_TAG, "ROUTE_LOAD: '%s' planned once mapped: hash=%08X stored=%08X%s", rt.name,
                      (unsigned)rt.res.hash, (unsigned)rg.want_hash, rt.map_changed ? " MAP CHANGED" : " (matches)");
        }
        if (rt.res.ok != ok0 || rt.res.hash != h0)
            TD5_LOG_I(LOG_TAG, "ROUTE_PLAN after the graph swap: ok %d -> %d, hash %08X -> %08X", ok0, rt.res.ok,
                      (unsigned)h0, (unsigned)rt.res.hash);
    }
}

static int rr_region_ensure(const char *place)
{
    if (!place || !place[0]) return 0;
    rr_region_wait();
    rr_region_swap();
    if (g.ready && strcmp(g.place, place) == 0) return 1;
    rr_region_update(place);
    rr_region_refresh(place);
    rr_region_take_list(place);
    rr_build_into(&rg.next, rg.bplace, rg.blist, rg.nb);
    rg.last_build_ms = td5_plat_time_ms();
    rg.next_ready = 1;
    rr_region_swap();
    return g.ready;
}

void td5_roam_route_graph_begin_async(const char *place)
{
    if (!place || !place[0] || rg.building) return;
    if (g.ready && strcmp(g.place, place) == 0) return;
    rr_region_update(place);
    rr_region_refresh(place);
    rr_region_start_build(place);
}

int  td5_roam_route_graph_building(void) { return rg.building != 0; }
long td5_roam_route_graph_gen(void) { return rg.gen; }
int  td5_roam_route_graph_pending(void) { return rg.queued; }

/* [F9b x F8] An edge / node index of generation from_gen in the current graph: -1 when that key is not in it
 * (or the index was -1), -2 when from_gen is not the previous generation (the caller re-resolves by position). */
int td5_roam_route_graph_remap_edge(long from_gen, int e)
{
    int lo = 0, hi = g.nedge - 1;
    uint64_t key;
    if (from_gen == rg.gen) return e;
    if (from_gen != rg.gen - 1) return -2;
    if (e < 0 || e >= rg.prev_ne) return -1;
    key = rg.prev_ekey[e];
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g.edge[mid].key == key) return mid;
        if (g.edge[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int td5_roam_route_graph_remap_node(long from_gen, int n)
{
    if (from_gen == rg.gen) return n;
    if (from_gen != rg.gen - 1) return -2;
    if (n < 0 || n >= rg.prev_nn) return -1;
    return rr_find_node(rg.prev_nkey[n]);
}
int  td5_roam_route_graph_settled(void)
{
    return g.ready && rg.queued == 0 && !rg.building && !rg.next_ready && rr_region_matches_graph();
}

void td5_roam_route_graph_free(void)
{
    rr_region_wait();
    rr_graph_free(&g);
    rr_graph_free(&rg.next);
    rg.next_ready = 0;
    free(rg.t); rg.t = NULL; rg.n = rg.cap = 0;
    free(rg.blist); rg.blist = NULL; rg.nb = 0;
    free(rg.prev_ekey); free(rg.prev_nkey); rg.prev_ekey = NULL; rg.prev_nkey = NULL; rg.prev_ne = rg.prev_nn = 0;
    rg.key = 0;
    rg.hash_check = 0;
}

void td5_roam_route_graph_poll(void)
{
    const char *place;
    uint32_t now;
    if (!td5_roam_active()) return;
    if (rg.next_ready && !rg.building) rr_region_swap();
    now = td5_plat_time_ms();
    if ((int)(now - rg.next_poll_ms) < 0) return;
    rg.next_poll_ms = now + RR_REG_POLL_MS;
    place = td5_roam_route_place();
    rr_region_update(place);
    rr_region_refresh(place);
    if (rg.building || rg.next_ready || rr_region_matches_graph() || rg.present <= 0) return;
    if (rg.queued > 0 && g.ready && (uint32_t)(now - rg.last_build_ms) < RR_REG_REBUILD_MS) return;
    rr_region_start_build(place);
}

/* 1 when the graph covers the tile of a PLACE point (or the point is outside the place: nothing to wait for) */
static int rr_covers(double px, double pz)
{
    const RoamGrid *gr = rr_grid(td5_roam_route_place());
    int tx, tz, lo, hi;
    if (!gr || !g.ready) return 0;
    roam_grid_tile_of(gr, px, pz, &tx, &tz);
    if (!td5_roam_partido_tile_ok(gr, tx, tz)) return 1;
    lo = 0; hi = g.ntiles - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g.tiles[mid][0] == tx && g.tiles[mid][1] == tz) return 1;
        if (g.tiles[mid][0] < tx || (g.tiles[mid][0] == tx && g.tiles[mid][1] < tz)) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* A degree is held as an integer count of 1e-8 deg (1.1 mm). The game runs with a non-default FP
 * rounding mode, so printf / strtod of a double does not round trip; integers do. */
static int64_t rr_to_n(double v) { return (int64_t)floor(v * 1e8 + 0.5); }
static double  rr_from_n(int64_t n) { return (double)n / 1e8; }
static double  rr_canon(double v) { return rr_from_n(rr_to_n(v)); }

/* "-34.92257679" from the integer count of 1e-8 deg */
static void rr_fmt_deg(char *out, size_t cap, double v)
{
    const int64_t n = rr_to_n(v), a = n < 0 ? -n : n;
    snprintf(out, cap, "%s%lld.%08lld", n < 0 ? "-" : "", (long long)(a / 100000000), (long long)(a % 100000000));
}

/* a decimal degree text -> canonical double (8 decimals, the 9th rounds); exact in any FP mode */
static double rr_parse_deg(const char *p)
{
    int neg = 0, nd = 0, round_up = 0;
    int64_t whole = 0, frac = 0, n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (nd < 8) { frac = frac * 10 + (*p - '0'); nd++; }
            else if (nd == 8) { round_up = (*p >= '5'); nd++; }
            p++;
        }
    }
    while (nd < 8) { frac *= 10; nd++; }
    n = whole * 100000000 + frac + (round_up ? 1 : 0);
    return rr_from_n(neg ? -n : n);
}

static void rr_wp_from_latlon(TD5_RouteWp *w, double lat, double lon, int kind)
{
    memset(w, 0, sizeof *w);
    w->lat = rr_canon(lat); w->lon = rr_canon(lon); w->kind = kind;
    td5_roam_route_latlon_to_place(w->lat, w->lon, &w->px, &w->pz);
}

static int rr_wp_snap(TD5_RouteWp *w)
{
    RrSnap s;
    w->snapped = 0;
    if (!rr_snap((double)w->px, (double)w->pz, RR_SNAP_MAX_M, &s)) return 0;
    w->snapped = 1; w->sx = s.x; w->sz = s.z; w->edge_key = g.edge[s.edge].key;
    w->snap_m = s.d / RR_UPM;
    return 1;
}

void td5_roam_route_replan(void)
{
    RrPoly poly = { NULL, NULL, 0, 0 };
    RrERefs refs = { NULL, 0, 0 };
    const RrWpSet *w = &rt.cur;
    const uint32_t t0 = td5_plat_time_ms();
    int i, nlegs, ok_legs = 0;
    rr_result_free();
    if (!g.ready || w->nwp < 2) return;
    nlegs = w->nwp - 1;
    if (w->type == TD5_ROUTE_CIRCUIT && w->wp[w->nwp - 1].kind == TD5_ROUTE_WP_FINISH) nlegs++;
    for (i = 0; i < nlegs; i++) {
        const TD5_RouteWp *a = &w->wp[i], *b = &w->wp[(i + 1) % w->nwp];
        RrSnap sa, sb;
        /* [F9b] a waypoint the region graph does not cover yet: the leg waits for the next swap */
        if (!rr_covers((double)a->px, (double)a->pz) || !rr_covers((double)b->px, (double)b->pz)) break;
        if (!rr_snap((double)a->px, (double)a->pz, RR_SNAP_MAX_M, &sa) ||
            !rr_snap((double)b->px, (double)b->pz, RR_SNAP_MAX_M, &sb)) break;
        if (!rr_plan_leg(&sa, &sb, &poly, &refs)) break;
        ok_legs++;
    }
    rt.res.legs = nlegs; rt.res.legs_ok = ok_legs; rt.res.ok = (ok_legs == nlegs);
    rt.res.closed = rt.res.ok && w->type == TD5_ROUTE_CIRCUIT && w->wp[w->nwp - 1].kind == TD5_ROUTE_WP_FINISH;
    rt.res.edge = refs.r; rt.res.nedge = refs.n;
    rt.res.px = poly.x; rt.res.pz = poly.z; rt.res.npt = poly.n;
    if (poly.n > 0) {
        int a;
        uint32_t h = 2166136261u;
        rt.res.dist_m = (double *)malloc((size_t)poly.n * sizeof(double));
        if (rt.res.dist_m) {
            rt.res.dist_m[0] = 0.0;
            for (a = 1; a < poly.n; a++) {
                const double dx = poly.x[a] - poly.x[a - 1], dz = poly.z[a] - poly.z[a - 1];
                rt.res.dist_m[a] = rt.res.dist_m[a - 1] + sqrt(dx * dx + dz * dz) / RR_UPM;
            }
            rt.res.length_m = rt.res.dist_m[poly.n - 1];
        }
        for (a = 0; a < refs.n; a++) {
            int b;
            for (b = 0; b < 8; b++) { h ^= (uint32_t)((refs.r[a].key >> (8 * b)) & 0xFF); h *= 16777619u; }
            h ^= (uint32_t)(refs.r[a].dir > 0 ? 1 : 2); h *= 16777619u;
        }
        rt.res.hash = h;
    }
    rt.res.plan_ms = (double)(td5_plat_time_ms() - t0);
    TD5_LOG_I(LOG_TAG, "ROUTE_PLAN: waypoints=%d legs=%d/%d edges=%d length=%.1f m points=%d hash=%08X (%.0f ms)",
              w->nwp, ok_legs, nlegs, rt.res.nedge, rt.res.length_m, rt.res.npt, (unsigned)rt.res.hash, rt.res.plan_ms);
}

void td5_roam_route_reset(void)
{
    rr_result_free();
    memset(&rt.cur, 0, sizeof rt.cur);
    rt.cur.type = TD5_ROUTE_SPRINT; rt.cur.laps = 1;
    rt.nundo = 0;
    rt.name[0] = '\0';
    rt.map_changed = 0;
    rt.loaded_hash = 0;
}

int td5_roam_route_wp_count(void) { return rt.cur.nwp; }
const TD5_RouteWp *td5_roam_route_wp(int i) { return (i >= 0 && i < rt.cur.nwp) ? &rt.cur.wp[i] : NULL; }
const TD5_RouteResult *td5_roam_route_result(void) { return &rt.res; }
int td5_roam_route_type(void) { return rt.cur.type; }
int td5_roam_route_laps(void) { return rt.cur.laps; }
const char *td5_roam_route_current_name(void) { return rt.name; }
int td5_roam_route_map_changed(void) { return rt.map_changed; }

void td5_roam_route_set_type(int type, int laps)
{
    const int t = type == TD5_ROUTE_CIRCUIT ? TD5_ROUTE_CIRCUIT : TD5_ROUTE_SPRINT;
    const int changed = t != rt.cur.type;
    rt.cur.type = t;
    rt.cur.laps = laps < 1 ? 1 : (laps > TD5_ROUTE_MAX_LAPS ? TD5_ROUTE_MAX_LAPS : laps);
    if (changed) td5_roam_route_replan();      /* the laps alone never change the polyline */
}

static void rr_push_undo(void)
{
    if (rt.nundo >= RR_UNDO_MAX) {
        memmove(&rt.undo[0], &rt.undo[1], (RR_UNDO_MAX - 1) * sizeof rt.undo[0]);
        rt.nundo = RR_UNDO_MAX - 1;
    }
    rt.undo[rt.nundo++] = rt.cur;
}

static int rr_ready_for_edit(void)
{
    if (g.ready && strcmp(g.place, td5_roam_route_place()) == 0) return 1;
    return td5_roam_route_graph_ensure(td5_roam_route_place());
}

/* build a snapped waypoint from a place point; 0 when no street is within reach */
static int rr_make_wp(TD5_RouteWp *w, int32_t px, int32_t pz, int kind)
{
    double lat, lon;
    if (!rr_ready_for_edit()) return 0;
    if (!td5_roam_route_place_to_latlon((double)px, (double)pz, &lat, &lon)) return 0;
    rr_wp_from_latlon(w, lat, lon, kind);
    if (!rr_wp_snap(w)) {
        /* [F9b] a point of the place the region graph does not reach yet: kept unsnapped, the region grows
         * round it (td5_roam_route_graph_poll) and the next swap snaps it and plans the route */
        if (!rr_covers((double)w->px, (double)w->pz)) {
            TD5_LOG_I(LOG_TAG, "ROUTE_EDIT: place (%d,%d) is beyond the mapped region: waypoint kept, mapping streets there",
                      (int)px, (int)pz);
            rg.next_poll_ms = 0;
            return 1;
        }
        TD5_LOG_W(LOG_TAG, "ROUTE_EDIT: no street within %.0f m of place (%d,%d)", RR_SNAP_MAX_M, (int)px, (int)pz);
        return 0;
    }
    return 1;
}

static void rr_commit_change(const char *what)
{
    rt.name[0] = '\0';
    rt.map_changed = 0;
    td5_roam_route_replan();
    TD5_LOG_I(LOG_TAG, "ROUTE_EDIT: %s -> %d waypoint(s)", what, rt.cur.nwp);
}

int td5_roam_route_set_start_at(int32_t px, int32_t pz)
{
    TD5_RouteWp w;
    if (!rr_make_wp(&w, px, pz, TD5_ROUTE_WP_START)) return 0;
    rr_push_undo();
    if (rt.cur.nwp > 0 && rt.cur.wp[0].kind == TD5_ROUTE_WP_START) {
        rt.cur.wp[0] = w;
    } else {
        if (rt.cur.nwp >= TD5_ROUTE_MAX_WP) { rt.nundo--; return 0; }
        memmove(&rt.cur.wp[1], &rt.cur.wp[0], (size_t)rt.cur.nwp * sizeof rt.cur.wp[0]);
        rt.cur.wp[0] = w; rt.cur.nwp++;
    }
    rr_commit_change("START");
    return 1;
}

int td5_roam_route_set_finish_at(int32_t px, int32_t pz)
{
    TD5_RouteWp w;
    if (!rr_make_wp(&w, px, pz, TD5_ROUTE_WP_FINISH)) return 0;
    rr_push_undo();
    if (rt.cur.nwp > 0 && rt.cur.wp[rt.cur.nwp - 1].kind == TD5_ROUTE_WP_FINISH) {
        rt.cur.wp[rt.cur.nwp - 1] = w;
    } else {
        if (rt.cur.nwp >= TD5_ROUTE_MAX_WP) { rt.nundo--; return 0; }
        rt.cur.wp[rt.cur.nwp++] = w;
    }
    rr_commit_change("FINISH");
    return 1;
}

int td5_roam_route_add_at(int32_t px, int32_t pz)
{
    TD5_RouteWp w;
    int at;
    if (rt.cur.nwp == 0) return td5_roam_route_set_start_at(px, pz);   /* the first point is the start */
    if (!rr_make_wp(&w, px, pz, TD5_ROUTE_WP_MID)) return 0;
    if (rt.cur.nwp >= TD5_ROUTE_MAX_WP) return 0;
    rr_push_undo();
    at = rt.cur.nwp;
    if (rt.cur.wp[at - 1].kind == TD5_ROUTE_WP_FINISH) at--;
    memmove(&rt.cur.wp[at + 1], &rt.cur.wp[at], (size_t)(rt.cur.nwp - at) * sizeof rt.cur.wp[0]);
    rt.cur.wp[at] = w; rt.cur.nwp++;
    rr_commit_change("WAYPOINT");
    return 1;
}

int td5_roam_route_move(int idx, int32_t px, int32_t pz)
{
    TD5_RouteWp w;
    if (idx < 0 || idx >= rt.cur.nwp) return 0;
    if (!rr_make_wp(&w, px, pz, rt.cur.wp[idx].kind)) return 0;
    rr_push_undo();
    rt.cur.wp[idx] = w;
    rr_commit_change("MOVE");
    return 1;
}

int td5_roam_route_remove(int idx)
{
    if (idx < 0 || idx >= rt.cur.nwp) return 0;
    rr_push_undo();
    memmove(&rt.cur.wp[idx], &rt.cur.wp[idx + 1], (size_t)(rt.cur.nwp - idx - 1) * sizeof rt.cur.wp[0]);
    rt.cur.nwp--;
    rr_commit_change("REMOVE");
    return 1;
}

int td5_roam_route_undo(void)
{
    if (rt.nundo <= 0) return 0;
    rt.cur = rt.undo[--rt.nundo];
    rr_commit_change("UNDO");
    return 1;
}

int td5_roam_route_clear(void)
{
    if (rt.cur.nwp == 0) return 0;
    rr_push_undo();
    rt.cur.nwp = 0;
    rr_commit_change("CLEAR");
    return 1;
}

int td5_roam_route_pick(double px, double pz, double max_m)
{
    int i, best = -1;
    double bd = max_m * RR_UPM;
    for (i = 0; i < rt.cur.nwp; i++) {
        const double dx = rt.cur.wp[i].px - px, dz = rt.cur.wp[i].pz - pz;
        const double d = sqrt(dx * dx + dz * dz);
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

/* ============================================================ save / load ===== */

int td5_roam_route_name_ok(const char *name)
{
    size_t n, i;
    if (!name) return 0;
    n = strlen(name);
    if (n < 1 || n >= TD5_ROUTE_NAME_MAX) return 0;
    for (i = 0; i < n; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int rr_route_path(char *out, size_t cap, const char *name)
{
    char rel[96];
    snprintf(rel, sizeof rel, "routes/%s.json", name);
    return td5_geo_source_path(out, cap, td5_roam_route_place(), rel) != NULL;
}

#ifndef TD5RE_RELEASE
/* DEV: the waypoints + the full ordered edge list to <log dir>roam_route_<what>_<name>.txt, so a
 * save and a later load (another process) can be compared byte for byte. */
static void rr_dump_edges(const char *what, const char *name)
{
    char path[512];
    FILE *f;
    const char *dir = td5_plat_log_dir();
    int i;
    snprintf(path, sizeof path, "%sroam_route_%s_%s.txt", dir ? dir : "", what, name);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "place=%s type=%d laps=%d waypoints=%d edges=%d length_m=%.1f hash=%08X\n", td5_roam_route_place(),
            rt.cur.type, rt.cur.laps, rt.cur.nwp, rt.res.nedge, rt.res.length_m, (unsigned)rt.res.hash);
    for (i = 0; i < rt.cur.nwp; i++) {
        char la[32], lo[32];
        rr_fmt_deg(la, sizeof la, rt.cur.wp[i].lat); rr_fmt_deg(lo, sizeof lo, rt.cur.wp[i].lon);
        fprintf(f, "wp %d kind=%d lat=%s lon=%s\n", i, rt.cur.wp[i].kind, la, lo);
    }
    for (i = 0; i < rt.res.nedge; i++)
        fprintf(f, "edge %llu dir=%d\n", (unsigned long long)rt.res.edge[i].key, rt.res.edge[i].dir);
    fclose(f);
}
#else
#define rr_dump_edges(w, n) ((void)0)
#endif

int td5_roam_route_save(const char *name)
{
    char path[512], dir[512], *sl;
    char *buf;
    size_t cap, n = 0;
    TD5_File *f;
    int i;
    if (!td5_roam_route_name_ok(name) || rt.cur.nwp < 2) return 0;
    if (!rr_route_path(path, sizeof path, name)) return 0;
    snprintf(dir, sizeof dir, "%s", path);
    sl = strrchr(dir, '/');
    { char *bs = strrchr(dir, '\\'); if (!sl || (bs && bs > sl)) sl = bs; }
    if (sl) { *sl = '\0'; td5_roam_mkdirs(dir); }
    cap = 1024 + (size_t)rt.cur.nwp * 160;
    buf = (char *)malloc(cap);
    if (!buf) return 0;
    n += (size_t)snprintf(buf + n, cap - n,
        "{\n  \"format\": 1,\n  \"place\": \"%s\",\n  \"name\": \"%s\",\n  \"type\": \"%s\",\n  \"laps\": %d,\n"
        "  \"edge_count\": %d,\n  \"edge_hash\": \"%08X\",\n  \"length_m\": %.1f,\n  \"waypoints\": [\n",
        td5_roam_route_place(), name, rt.cur.type == TD5_ROUTE_CIRCUIT ? "circuit" : "sprint", rt.cur.laps,
        rt.res.nedge, (unsigned)rt.res.hash, rt.res.length_m);
    for (i = 0; i < rt.cur.nwp; i++) {
        const TD5_RouteWp *w = &rt.cur.wp[i];
        char la[32], lo[32];
        rr_fmt_deg(la, sizeof la, w->lat); rr_fmt_deg(lo, sizeof lo, w->lon);
        n += (size_t)snprintf(buf + n, cap - n, "    { \"lat\": %s, \"lon\": %s, \"role\": \"%s\" }%s\n",
            la, lo, w->kind == TD5_ROUTE_WP_START ? "start" : (w->kind == TD5_ROUTE_WP_FINISH ? "finish" : "via"),
            i + 1 < rt.cur.nwp ? "," : "");
    }
    n += (size_t)snprintf(buf + n, cap - n, "  ]\n}\n");
    f = td5_plat_file_open(path, "wb");
    if (!f) { free(buf); TD5_LOG_E(LOG_TAG, "ROUTE_SAVE: cannot write %s", path); return 0; }
    td5_plat_file_write(f, buf, n);
    td5_plat_file_close(f);
    free(buf);
    snprintf(rt.name, sizeof rt.name, "%s", name);
    rt.map_changed = 0;
    rr_dump_edges("save", name);
    TD5_LOG_I(LOG_TAG, "ROUTE_SAVE: '%s' waypoints=%d edges=%d length=%.1f m hash=%08X -> %s",
              name, rt.cur.nwp, rt.res.nedge, rt.res.length_m, (unsigned)rt.res.hash, path);
    return 1;
}

int td5_roam_route_load(const char *name)
{
    char path[512];
    TD5_File *f;
    int64_t sz;
    char *txt;
    cJSON *root, *wps, *it;
    RrWpSet set;
    uint32_t want_hash = 0;
    int have_hash = 0, n = 0;
    if (!td5_roam_route_name_ok(name) || !rr_route_path(path, sizeof path, name)) return 0;
    f = td5_plat_file_open(path, "rb");
    if (!f) { TD5_LOG_W(LOG_TAG, "ROUTE_LOAD: '%s' not found (%s)", name, path); return 0; }
    sz = td5_plat_file_size(f);
    if (sz <= 0 || sz > 1024 * 1024) { td5_plat_file_close(f); return 0; }
    txt = (char *)malloc((size_t)sz + 1u);
    if (!txt) { td5_plat_file_close(f); return 0; }
    if (td5_plat_file_read(f, txt, (size_t)sz) != (size_t)sz) { free(txt); td5_plat_file_close(f); return 0; }
    txt[sz] = '\0';
    td5_plat_file_close(f);
    root = cJSON_Parse(txt);
    if (!root) { free(txt); TD5_LOG_W(LOG_TAG, "ROUTE_LOAD: '%s' is not JSON", name); return 0; }
    memset(&set, 0, sizeof set);
    set.type = TD5_ROUTE_SPRINT; set.laps = 1;
    it = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(it) && strcmp(it->valuestring, "circuit") == 0) set.type = TD5_ROUTE_CIRCUIT;
    it = cJSON_GetObjectItem(root, "laps");
    if (cJSON_IsNumber(it)) set.laps = it->valueint < 1 ? 1 : (it->valueint > TD5_ROUTE_MAX_LAPS ? TD5_ROUTE_MAX_LAPS : it->valueint);
    it = cJSON_GetObjectItem(root, "edge_hash");
    if (cJSON_IsString(it)) { want_hash = (uint32_t)strtoul(it->valuestring, NULL, 16); have_hash = 1; }
    wps = cJSON_GetObjectItem(root, "waypoints");
    if (cJSON_IsArray(wps)) {
        cJSON *w;
        /* lat / lon are read from the text with strtod: the vendored cJSON number parse is off by up to
         * ~1e-8 on a 8-decimal degree, which would move a waypoint between a save and a load */
        const char *scan = strstr(txt, "\"waypoints\"");
        cJSON_ArrayForEach(w, wps) {
            double rlat = 0.0, rlon = 0.0;
            int have_raw = 0;
            if (scan) {
                const char *a = strstr(scan, "\"lat\"");
                const char *b = a ? strstr(a, "\"lon\"") : NULL;
                const char *ca = a ? strchr(a, ':') : NULL, *cb = b ? strchr(b, ':') : NULL;
                if (ca && cb) { rlat = rr_parse_deg(ca + 1); rlon = rr_parse_deg(cb + 1); have_raw = 1; scan = cb + 1; }
            }
            cJSON *la = cJSON_GetObjectItem(w, "lat"), *lo = cJSON_GetObjectItem(w, "lon"), *ro = cJSON_GetObjectItem(w, "role");
            int kind = TD5_ROUTE_WP_MID;
            if (n >= TD5_ROUTE_MAX_WP || !cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) continue;
            if (cJSON_IsString(ro)) {
                if (strcmp(ro->valuestring, "start") == 0) kind = TD5_ROUTE_WP_START;
                else if (strcmp(ro->valuestring, "finish") == 0) kind = TD5_ROUTE_WP_FINISH;
            }
            rr_wp_from_latlon(&set.wp[n], have_raw ? rlat : la->valuedouble, have_raw ? rlon : lo->valuedouble, kind);
            n++;
        }
    }
    set.nwp = n;
    cJSON_Delete(root);
    free(txt);
    if (n < 2) { TD5_LOG_W(LOG_TAG, "ROUTE_LOAD: '%s' has %d waypoint(s)", name, n); return 0; }
    if (!rr_ready_for_edit()) return 0;
    {
        int i;
        for (i = 0; i < n; i++) rr_wp_snap(&set.wp[i]);
    }
    rt.cur = set; rt.nundo = 0;
    td5_roam_route_replan();
    snprintf(rt.name, sizeof rt.name, "%s", name);
    rt.loaded_hash = want_hash;
    rt.map_changed = have_hash && want_hash != rt.res.hash;
    /* [F9b] a route reaching beyond the mapped region plans after a graph swap: judge its hash then */
    rg.hash_check = 0;
    if (!rt.res.ok) {
        int i, out = 0;
        for (i = 0; i < n; i++) out += !rr_covers((double)set.wp[i].px, (double)set.wp[i].pz);
        if (out) {
            rt.map_changed = 0;
            rg.hash_check = 1; rg.have_hash = have_hash; rg.want_hash = want_hash;
            rg.next_poll_ms = 0;
            TD5_LOG_I(LOG_TAG, "ROUTE_LOAD: '%s' reaches %d waypoint(s) beyond the mapped region: mapping streets", name, out);
            return 1;
        }
    }
    rr_dump_edges("load", name);
    TD5_LOG_I(LOG_TAG, "ROUTE_LOAD: '%s' waypoints=%d edges=%d length=%.1f m hash=%08X stored=%08X%s",
              name, n, rt.res.nedge, rt.res.length_m, (unsigned)rt.res.hash, (unsigned)want_hash,
              rt.map_changed ? " MAP CHANGED" : (have_hash ? " (matches)" : " (no stored hash)"));
    return rt.res.ok;
}

int td5_roam_route_delete_file(const char *name)
{
    char path[512];
    if (!td5_roam_route_name_ok(name) || !rr_route_path(path, sizeof path, name)) return 0;
    return td5_plat_file_delete(path) == 0 || !td5_plat_file_exists(path);
}

static int rr_cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int td5_roam_route_list(char names[][TD5_ROUTE_NAME_MAX], int max)
{
    int n = 0;
#ifdef _WIN32
    char path[512], pat[560];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *sl;
    if (!td5_geo_source_path(path, sizeof path, td5_roam_route_place(), "routes/x.json")) return 0;
    sl = strrchr(path, '/');
    { char *bs = strrchr(path, '\\'); if (!sl || (bs && bs > sl)) sl = bs; }
    if (!sl) return 0;
    sl[1] = '\0';
    snprintf(pat, sizeof pat, "%s*.json", path);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char base[TD5_ROUTE_NAME_MAX];
        size_t len = strlen(fd.cFileName);
        if (len < 6 || len - 5 >= TD5_ROUTE_NAME_MAX) continue;
        memcpy(base, fd.cFileName, len - 5); base[len - 5] = '\0';
        if (!td5_roam_route_name_ok(base)) continue;
        if (n < max) { memcpy(names[n], base, sizeof base); n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, TD5_ROUTE_NAME_MAX, rr_cmp_name);
#else
    (void)names; (void)max;
#endif
    return n;
}

/* ============================================================ dev test ======== */

#ifndef TD5RE_RELEASE
static int rr_find_edge_key(uint64_t key)
{
    int lo = 0, hi = g.nedge - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g.edge[mid].key == key) return mid;
        if (g.edge[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void rr_mid_of_edge(int e, int32_t *x, int32_t *z)
{
    const int p = g.edge[e].pt0 + g.edge[e].npt / 2;
    *x = g.pt[2 * p]; *z = g.pt[2 * p + 1];
}

/* TD5RE_ROAM_ROUTE_TEST=1 (F5-A): builds the planner graph, plans N random edge-to-edge routes,
 * checks one-way compliance, determinism (plan twice -> same hash) and a save / load round trip,
 * logs the census and exits. TD5RE_ROAM_ROUTE_TEST_N sets the pair count (default 40). */
int td5_roam_route_dev_test(void)
{
    const int N = td5_env_int("TD5RE_ROAM_ROUTE_TEST_N", 40, 2, 2000);
    uint32_t rng = 12345u;
    int i, pairs = 0, planned = 0, bad_oneway = 0, nondet = 0, snapfail = 0, unknown_key = 0;
    double ratio_sum = 0.0, ratio_max = 0.0, ms_sum = 0.0, ms_max = 0.0, len_sum = 0.0;
    int edges_sum = 0;
    if (td5_env_int("TD5RE_ROAM_ROUTE_TEST", 0, 0, 1) != 1) return 0;
    td5_roam_route_reset();
    if (!td5_roam_route_graph_ensure(td5_roam_route_place())) {
        TD5_LOG_E(LOG_TAG, "ROUTE_TEST: graph build failed");
        return 1;
    }
    for (i = 0; i < N; i++) {
        int32_t ax, az, bx, bz;
        int ea, eb, k;
        double dx, dz, straight;
        rng = rng * 1664525u + 1013904223u; ea = (int)((rng >> 8) % (uint32_t)g.nedge);
        rng = rng * 1664525u + 1013904223u; eb = (int)((rng >> 8) % (uint32_t)g.nedge);
        rr_mid_of_edge(ea, &ax, &az); rr_mid_of_edge(eb, &bx, &bz);
        td5_roam_route_reset();
        if (!td5_roam_route_set_start_at(ax, az) || !td5_roam_route_set_finish_at(bx, bz)) { snapfail++; continue; }
        pairs++;
        dx = (double)(bx - ax); dz = (double)(bz - az);
        straight = sqrt(dx * dx + dz * dz) / RR_UPM;
        if (!rt.res.ok) continue;
        {
            const uint32_t h1 = rt.res.hash;
            planned++;
            ms_sum += rt.res.plan_ms; if (rt.res.plan_ms > ms_max) ms_max = rt.res.plan_ms;
            len_sum += rt.res.length_m; edges_sum += rt.res.nedge;
            if (straight > 50.0) {
                const double r = rt.res.length_m / straight;
                ratio_sum += r; if (r > ratio_max) ratio_max = r;
            }
            for (k = 0; k < rt.res.nedge; k++) {
                const int ei = rr_find_edge_key(rt.res.edge[k].key);
                if (ei < 0) { unknown_key++; continue; }
                if (!rr_allowed(ei, rt.res.edge[k].dir > 0 ? 0 : 1)) bad_oneway++;
            }
            td5_roam_route_replan();
            if (rt.res.hash != h1) nondet++;
        }
    }
    TD5_LOG_I(LOG_TAG, "ROUTE_TEST: pairs=%d planned=%d snapfail=%d oneway_violations=%d unknown_keys=%d nondeterministic=%d "
              "mean_len=%.0f m mean_edges=%.1f detour mean=%.2f max=%.2f plan ms mean=%.2f max=%.0f",
              pairs, planned, snapfail, bad_oneway, unknown_key, nondet,
              planned ? len_sum / planned : 0.0, planned ? (double)edges_sum / planned : 0.0,
              planned ? ratio_sum / planned : 0.0, ratio_max, planned ? ms_sum / planned : 0.0, ms_max);
    /* connected components of the undirected edge graph */
    {
        int *comp = (int *)calloc((size_t)g.nnode, sizeof(int)), *stack = (int *)malloc((size_t)g.nnode * sizeof(int));
        int ncomp = 0, big = 0, n_in_big = 0, nn;
        if (comp && stack) {
            for (nn = 0; nn < g.nnode; nn++) {
                int sp = 0, count = 0;
                if (comp[nn]) continue;
                comp[nn] = ++ncomp; stack[sp++] = nn;
                while (sp > 0) {
                    const int cur = stack[--sp];
                    int q;
                    count++;
                    for (q = 0; q < g.node[cur].ninc; q++) {
                        const int inc = g.inc[g.node[cur].first + q];
                        const int other = g.edge[inc >> 1].node[(inc & 1) ^ 1];
                        if (!comp[other]) { comp[other] = ncomp; stack[sp++] = other; }
                    }
                }
                if (count > n_in_big) { n_in_big = count; big = ncomp; }
            }
            TD5_LOG_I(LOG_TAG, "ROUTE_TEST: graph components=%d largest=%d of %d nodes (id %d)", ncomp, n_in_big, g.nnode, big);
        }
        free(comp); free(stack);
    }
    /* save / load round trip: 3 waypoints */
    {
        int32_t x[3], z[3];
        uint32_t h0;
        int n0, ok = 0;
        rr_mid_of_edge(g.nedge / 7, &x[0], &z[0]);
        rr_mid_of_edge(g.nedge / 2, &x[1], &z[1]);
        rr_mid_of_edge(g.nedge - g.nedge / 5, &x[2], &z[2]);
        td5_roam_route_reset();
        td5_roam_route_set_start_at(x[0], z[0]);
        td5_roam_route_add_at(x[1], z[1]);
        td5_roam_route_set_finish_at(x[2], z[2]);
        h0 = rt.res.hash; n0 = rt.res.nedge;
        if (td5_roam_route_save("zz_devtest")) {
            td5_roam_route_reset();
            if (td5_roam_route_load("zz_devtest") && rt.res.hash == h0 && rt.res.nedge == n0 && !rt.map_changed) ok = 1;
            td5_roam_route_delete_file("zz_devtest");
        }
        TD5_LOG_I(LOG_TAG, "ROUTE_TEST: save/load round trip %s (edges %d hash %08X)", ok ? "PASS" : "FAIL", n0, (unsigned)h0);
    }
    return 1;
}
#endif

/* ============================================================ [F8] graph view ===
 * Read-only access to the planner graph for the ambient traffic (td5_roam_traffic.c): the
 * same directed street graph the A* walks, one-way included. Pointers stay valid until the
 * graph is freed (session end). */

int td5_roam_route_graph_nedge(void) { return g.ready ? g.nedge : 0; }
int td5_roam_route_graph_nnode(void) { return g.ready ? g.nnode : 0; }

int td5_roam_route_graph_edge_view(int e, TD5_RouteGEdge *out)
{
    const RrEdge *ed;
    if (!g.ready || e < 0 || e >= g.nedge || !out) return 0;
    ed = &g.edge[e];
    out->key = ed->key;
    out->node[0] = ed->node[0]; out->node[1] = ed->node[1];
    out->pt = g.pt + 2 * ed->pt0;
    out->cum = g.cum + ed->pt0;
    out->npt = ed->npt;
    out->plen = ed->plen;
    out->oneway = ed->oneway;
    out->klass = ed->klass; out->lanes = ed->lanes; out->flags = ed->flags;
    return 1;
}

int td5_roam_route_graph_node_view(int n, TD5_RouteGNode *out)
{
    const RrNode *nd;
    if (!g.ready || n < 0 || n >= g.nnode || !out) return 0;
    nd = &g.node[n];
    out->key = nd->key; out->x = nd->x; out->z = nd->z;
    out->inc = g.inc + nd->first; out->ninc = nd->ninc;
    return 1;
}

int td5_roam_route_graph_snap_edge(double px, double pz, double max_m, int *edge, double *along, double *dist_m)
{
    RrSnap s;
    if (!rr_snap(px, pz, max_m, &s)) return 0;
    if (edge) *edge = s.edge;
    if (along) *along = s.along;
    if (dist_m) *dist_m = s.d / RR_UPM;
    return 1;
}

int td5_roam_route_graph_signal(int64_t key, int16_t *bear, uint64_t *ekey, int cap)
{
    int lo = 0, hi = g.nsig - 1;
    if (!g.ready) return 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g.sig[mid].key == key) {
            const RrSig *sg = &g.sig[mid];
            int a, n = sg->narm < cap ? sg->narm : cap;
            for (a = 0; a < n; a++) { if (bear) bear[a] = sg->bear[a]; if (ekey) ekey[a] = sg->ekey[a]; }
            return n > 0 ? n : 1;          /* signalised even with no arm record */
        }
        if (g.sig[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int td5_roam_route_graph_nsignal(void) { return g.ready ? g.nsig : 0; }
