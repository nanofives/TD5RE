/**
 * td5_roam_barrier.c -- GEO FREE ROAM: NFSU2 route barriers (PORT-ONLY, F6-A).
 * Contract: td5_roam_barrier.h. Design: docs/plans/GEO_FREE_ROAM.md "Route, race and barriers".
 *
 * Sections: tile pool + indexes / row + ray helpers / placement (build) / WORLD-frame rebase /
 * drawing / census + dev test.
 *
 * PLACEMENT. The route is an edge list (key, dir). The EDGE records of the ROAMTILE files say which
 * end of an edge is a junction CLUSTER (end_kind) and name it (end_key); the NODE/ARM records of the
 * cluster's owner tile list every arm (edge key + end). A route PASS is a transition e_i -> e_{i+1}
 * through a cluster; the first edge's entry end and the last edge's exit end are POCKETS (a cluster
 * the route starts or finishes in). Per cluster the arms the route takes (over all passes) are
 * open, every other arm gets a MOUTH barrier across its cut row. Rows come straight from the tile's
 * ribbon quads (first row of the run with row0 == 0, or last row of the run with the highest row0),
 * so a barrier spans the carriageway kerb to kerb with the ribbon's own vertices.
 * A TURN board goes on every pass that deflects more than 30 deg: from the entry cut row a ray runs
 * straight on, and the board stands where it first meets the far kerb (patch BEDG wall) or another
 * arm's cut row, facing the approaching car, chevrons pointing the way the route turns.
 *
 * The tiles are read from disk for the duration of one build (the route is not bounded by the
 * resident set); nothing outlives it except the barrier arrays. Main thread only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_types.h"
#include "td5_render.h"
#include "td5_camera.h"
#include "td5_page_map.h"      /* HUD_WHITE_TEX_PAGE */
#include "td5_track.h"         /* td5_track_quad_vertices */
#include "td5_roam_tile.h"
#include "td5_roam_world.h"     /* TD5_ROAM_END_CLUSTER */
#include "td5_roam_mode.h"
#include "td5_roam_route.h"
#include "td5_roam_physics.h"  /* td5_roam_ground */
#include "td5_roam_stream.h"   /* td5_roam_stream_active / _point_missing */
#include "td5_roam_barrier.h"

#define LOG_TAG "roam"

#define BAR_UPM            430.0
#define BAR_H_UNITS        (TD5_ROAM_BARRIER_H_M * BAR_UPM)
#define BAR_OUT_M          0.5      /* mouth barrier stands this far out from the cut row */
#define BAR_KERB_M         0.2      /* ... and reaches this far into each kerb            */
#define BAR_TURN_DEG       30.0     /* chevron board when the route turns more than this  */
#define BAR_BOARD_W_M      3.6
#define BAR_ROW_FAR_M      90.0     /* an arm row further than this from its centroid is not that arm's */
#define BAR_MAX_TAKEN      64

/* ============================================================ state ============ */

typedef struct { double x0, z0, y0, x1, z1, y1; } PlBar;      /* PLACE frame, units; y strip units */
typedef struct {
    double   ax, az;       /* unit axis p0 -> p1 (xz)                                        */
    int      sign;         /* chevrons point along +axis (+1) or -axis (-1)                  */
    double   len;          /* units                                                          */
    int      refined;      /* turn board: y taken from the live ground once                  */
    double   dirx, dirz;   /* mouth: the route exit direction the chevrons follow             */
    uint64_t arm_key;      /* mouth: the arm's edge key and end (debug / census)             */
    int      arm_end;
} BarAux;

static struct {
    TD5_RoamBarrier *pub;
    PlBar           *pl;
    BarAux          *aux;
    int              n, cap;
    int32_t          org_x, org_z;      /* origin the public array was derived for */
    int              visible;
    TD5_RoamBarrierStats st;
    int              have;              /* a set has been built (possibly empty)   */
    uint32_t         built_hash;
    int              built_nedge;
    /* DEV knob state */
    int              knob_init, knob_on;
    char             knob_route[48];
    int              knob_loaded;
#ifndef TD5RE_RELEASE
    struct DbgArm { double x0, z0, x1, z1; int status; unsigned long long ck, ek; int end; } *dbg;   /* DEV: every arm of the clusters passed */
    int              ndbg, capdbg;
#endif
} B;

static void bar_free_set(void)
{
    free(B.pub); free(B.pl); free(B.aux);
    B.pub = NULL; B.pl = NULL; B.aux = NULL; B.n = 0; B.cap = 0;
}

static int bar_add(const PlBar *p, int kind, uint64_t ck, int sign, uint64_t arm_key, int arm_end)
{
    if (B.n >= B.cap) {
        const int nc = B.cap ? B.cap * 2 : 128;
        void *a = realloc(B.pub, (size_t)nc * sizeof *B.pub);
        void *b, *c;
        if (!a) return 0;
        B.pub = (TD5_RoamBarrier *)a;
        b = realloc(B.pl, (size_t)nc * sizeof *B.pl);
        if (!b) return 0;
        B.pl = (PlBar *)b;
        c = realloc(B.aux, (size_t)nc * sizeof *B.aux);
        if (!c) return 0;
        B.aux = (BarAux *)c;
        B.cap = nc;
    }
    {
        const double dx = p->x1 - p->x0, dz = p->z1 - p->z0, l = sqrt(dx * dx + dz * dz);
        B.pl[B.n] = *p;
        memset(&B.pub[B.n], 0, sizeof B.pub[B.n]);
        B.pub[B.n].kind = (uint8_t)kind;
        B.pub[B.n].cluster_key = ck;
        B.aux[B.n].ax = l > 1e-9 ? dx / l : 1.0;
        B.aux[B.n].az = l > 1e-9 ? dz / l : 0.0;
        B.aux[B.n].len = l;
        B.aux[B.n].sign = sign >= 0 ? 1 : -1;
        B.aux[B.n].refined = 0;
        B.aux[B.n].arm_key = arm_key; B.aux[B.n].arm_end = arm_end;
        B.n++;
    }
    return 1;
}

/* ============================================================ WORLD frame ====== */

static int32_t bar_w(double place_units, int32_t org) { return (int32_t)floor((place_units - (double)org) * 256.0 + 0.5); }

static void bar_derive(int32_t ox, int32_t oz)
{
    int i;
    for (i = 0; i < B.n; i++) {
        const PlBar *p = &B.pl[i];
        TD5_RoamBarrier *o = &B.pub[i];
        o->x0 = bar_w(p->x0, ox); o->z0 = bar_w(p->z0, oz);
        o->x1 = bar_w(p->x1, ox); o->z1 = bar_w(p->z1, oz);
        o->y0 = (int32_t)floor(p->y0 * 256.0 + 0.5);
        o->y1 = (int32_t)floor(p->y1 * 256.0 + 0.5);
    }
    B.org_x = ox; B.org_z = oz;
}

static void bar_rebase_if_needed(void)
{
    int32_t ox = 0, oz = 0;
    td5_roam_world_origin(&ox, &oz);
    if (B.n > 0 && (ox != B.org_x || oz != B.org_z)) bar_derive(ox, oz);
}

const TD5_RoamBarrier *td5_roam_barriers(int *count)
{
    bar_rebase_if_needed();
    if (count) *count = B.n;
    return B.pub;
}

void td5_roam_barriers_clear(void)
{
    bar_free_set();
#ifndef TD5RE_RELEASE
    free(B.dbg); B.dbg = NULL; B.ndbg = B.capdbg = 0;
#endif
    memset(&B.st, 0, sizeof B.st);
    B.have = 0; B.built_hash = 0; B.built_nedge = 0;
}

void td5_roam_barriers_set_visible(int on) { B.visible = on ? 1 : 0; }
int  td5_roam_barriers_visible(void) { return B.visible; }
void td5_roam_barriers_stats(TD5_RoamBarrierStats *out) { if (out) *out = B.st; }

/* ============================================================ tile pool ======== */

typedef struct { uint64_t key; int row0; int tile, rec; } EIdx;
typedef struct { int64_t key; int tile, rec; } NIdx;

typedef struct {
    RoamTile *tl; int ntl;
    EIdx *ex; int nex;
    NIdx *nx; int nnx;
    RoamGrid grid;
} Pool;

static int cmp_eidx(const void *a, const void *b)
{
    const EIdx *x = (const EIdx *)a, *y = (const EIdx *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->row0 != y->row0) return x->row0 < y->row0 ? -1 : 1;
    if (x->tile != y->tile) return x->tile < y->tile ? -1 : 1;
    return x->rec < y->rec ? -1 : (x->rec > y->rec);
}
static int cmp_nidx(const void *a, const void *b)
{
    const NIdx *x = (const NIdx *)a, *y = (const NIdx *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->tile < y->tile ? -1 : (x->tile > y->tile);
}
static int cmp_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : (x > y);
}

static void pool_free(Pool *p)
{
    int i;
    for (i = 0; i < p->ntl; i++) td5_roam_tile_free(&p->tl[i]);
    free(p->tl); free(p->ex); free(p->nx);
    memset(p, 0, sizeof *p);
}

static int pool_read_grid(const char *place, RoamGrid *g)
{
    char path[512], txt[4096];
    size_t nr;
    FILE *f;
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    f = fopen(path, "rb");
    if (!f) return 0;
    nr = fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    txt[nr] = 0;
    return roam_grid_parse(txt, g);
}

static int64_t tile_pack(int tx, int tz) { return ((int64_t)tx << 20) ^ (int64_t)(tz + (1 << 19)); }
static void tile_unpack(int64_t k, int *tx, int *tz)
{
    *tz = (int)((k & 0xFFFFF) - (1 << 19));
    *tx = (int)((k ^ (int64_t)(*tz + (1 << 19))) >> 20);
}

static int push_i64(int64_t **arr, int *n, int *cap, int64_t v)
{
    if (*n >= *cap) {
        const int nc = *cap ? *cap * 2 : 256;
        void *q = realloc(*arr, (size_t)nc * sizeof **arr);
        if (!q) return 0;
        *arr = (int64_t *)q;
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 1;
}

/* every tile the route passes through, plus the ring of 8 around each */
static int pool_load(Pool *p, const char *place, const TD5_RoamRouteResult *r)
{
    int64_t *ids = NULL;
    int nids = 0, cap = 0, i, u = 0, n2;
    int64_t *ring = NULL;
    int nring = 0;

    if (!pool_read_grid(place, &p->grid)) { TD5_LOG_E(LOG_TAG, "BARRIERS: no roam/ROAMGRID.JSON for '%s'", place); return 0; }
    for (i = 0; i < r->npt; i++) {
        double x = r->px[i], z = r->pz[i];
        int steps = 1, s;
        if (i + 1 < r->npt) {
            const double dx = r->px[i + 1] - x, dz = r->pz[i + 1] - z, l = sqrt(dx * dx + dz * dz);
            steps = 1 + (int)(l / (60.0 * BAR_UPM));       /* a point at least every 60 m */
        }
        for (s = 0; s < steps; s++) {
            const double f = (double)s / (double)steps;
            int tx, tz;
            double xx = x, zz = z;
            if (i + 1 < r->npt) { xx = x + (r->px[i + 1] - x) * f; zz = z + (r->pz[i + 1] - z) * f; }
            roam_grid_tile_of(&p->grid, xx, zz, &tx, &tz);
            if (!push_i64(&ids, &nids, &cap, tile_pack(tx, tz))) goto oom;
        }
    }
    qsort(ids, (size_t)nids, sizeof *ids, cmp_i64);
    for (i = 0; i < nids; i++) if (i == 0 || ids[i] != ids[i - 1]) ids[u++] = ids[i];
    nids = u;
    {
        int cap2 = 0;
        for (i = 0; i < nids; i++) {
            int tx, tz, dx, dz;
            tile_unpack(ids[i], &tx, &tz);
            for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++)
                if (!push_i64(&ring, &nring, &cap2, tile_pack(tx + dx, tz + dz))) goto oom;
        }
    }
    qsort(ring, (size_t)nring, sizeof *ring, cmp_i64);
    for (i = 0, u = 0; i < nring; i++) if (i == 0 || ring[i] != ring[i - 1]) ring[u++] = ring[i];
    nring = u;
    p->tl = (RoamTile *)calloc((size_t)nring, sizeof(RoamTile));
    if (!p->tl) goto oom;
    for (i = 0; i < nring; i++) {
        int tx, tz;
        char path[512], err[128];
        RoamTile t;
        tile_unpack(ring[i], &tx, &tz);
        td5_roam_tile_path(path, sizeof path, NULL, place, tx, tz);
        memset(&t, 0, sizeof t);
        if (!td5_roam_tile_read(path, &t, 0, err, sizeof err)) continue;     /* a missing tile = off the casco */
        p->tl[p->ntl++] = t;
    }
    free(ids); free(ring);
    /* indexes */
    n2 = 0;
    for (i = 0; i < p->ntl; i++) n2 += (int)p->tl[i].nedge;
    p->ex = (EIdx *)malloc((size_t)(n2 ? n2 : 1) * sizeof(EIdx));
    n2 = 0;
    for (i = 0; i < p->ntl; i++) n2 += (int)p->tl[i].nnode;
    p->nx = (NIdx *)malloc((size_t)(n2 ? n2 : 1) * sizeof(NIdx));
    if (!p->ex || !p->nx) { TD5_LOG_E(LOG_TAG, "BARRIERS: out of memory"); return 0; }
    for (i = 0; i < p->ntl; i++) {
        uint32_t k;
        for (k = 0; k < p->tl[i].nedge; k++) {
            const RoamEdgeRec *er = &p->tl[i].edge[k];
            if (er->span_count == 0 || (uint32_t)er->span_first + er->span_count > p->tl[i].nspan) continue;
            p->ex[p->nex].key = er->key; p->ex[p->nex].row0 = er->row0; p->ex[p->nex].tile = i; p->ex[p->nex].rec = (int)k;
            p->nex++;
        }
        for (k = 0; k < p->tl[i].nnode; k++) {
            p->nx[p->nnx].key = p->tl[i].node[k].cluster_key; p->nx[p->nnx].tile = i; p->nx[p->nnx].rec = (int)k;
            p->nnx++;
        }
    }
    qsort(p->ex, (size_t)p->nex, sizeof(EIdx), cmp_eidx);
    qsort(p->nx, (size_t)p->nnx, sizeof(NIdx), cmp_nidx);
    return 1;
oom:
    free(ids); free(ring);
    TD5_LOG_E(LOG_TAG, "BARRIERS: out of memory");
    return 0;
}

/* the EIdx range [lo, hi) of one edge key; 0 when absent */
static int pool_edge_range(const Pool *p, uint64_t key, int *lo, int *hi)
{
    int a = 0, b = p->nex, m;
    while (a < b) { m = (a + b) / 2; if (p->ex[m].key < key) a = m + 1; else b = m; }
    if (a >= p->nex || p->ex[a].key != key) return 0;
    *lo = a;
    b = a;
    while (b < p->nex && p->ex[b].key == key) b++;
    *hi = b;
    return 1;
}

static const NIdx *pool_node(const Pool *p, int64_t key)
{
    int a = 0, b = p->nnx, m;
    while (a < b) { m = (a + b) / 2; if (p->nx[m].key < key) a = m + 1; else b = m; }
    return (a < p->nnx && p->nx[a].key == key) ? &p->nx[a] : NULL;
}

/* ============================================================ rows + rays ====== */

typedef struct {
    int    ok;
    double x0, z0, y0, x1, z1, y1;     /* the two kerb points, PLACE units; y strip units */
    double ox, oz;                     /* unit outward direction (away from the cluster)   */
    double mx, mz;                     /* row midpoint                                     */
} ArmRow;

static int vert_place(const RoamTile *t, const TD5_StripSpan *sp, int vi, double *x, double *y, double *z)
{
    if (vi < 0 || (uint32_t)vi >= t->nvert) return 0;
    *x = (double)t->org_x + (double)sp->origin_x + (double)t->vert[vi].x;
    *y = (double)sp->origin_y + (double)t->vert[vi].y;
    *z = (double)t->org_z + (double)sp->origin_z + (double)t->vert[vi].z;
    return 1;
}

#define ARM_ROWS_MAX 24

/* the unit outward normal of a row (away from the cluster centroid) and its midpoint; 0 on a degenerate row */
static int row_fill(ArmRow *r, double cx, double cz)
{
    double dx, dz, l, d;
    r->mx = 0.5 * (r->x0 + r->x1); r->mz = 0.5 * (r->z0 + r->z1);
    dx = r->x1 - r->x0; dz = r->z1 - r->z0; l = sqrt(dx * dx + dz * dz);
    if (l < 1.0) return 0;
    r->ox = -dz / l; r->oz = dx / l;
    d = r->ox * (r->mx - cx) + r->oz * (r->mz - cz);
    if (d < 0.0) { r->ox = -r->ox; r->oz = -r->oz; }
    r->ok = 1;
    return 1;
}

static int row_from_span(const RoamTile *t, const TD5_StripSpan *sp, int vi, int L, ArmRow *r)
{
    memset(r, 0, sizeof *r);
    if (!vert_place(t, sp, vi, &r->x0, &r->y0, &r->z0)) return 0;
    if (!vert_place(t, sp, vi + L, &r->x1, &r->y1, &r->z1)) return 0;
    return 1;
}

/* Up to `max` rows of the arm's ribbon: row 0 = the cut row next to the cluster, then deeper into the
 * edge (one per quad, runs of different tiles chained on their shared row). Returns the count; 0 when the
 * edge has no ribbon here or its first row is not by the cluster (cx, cz). */
static int arm_rows(const Pool *p, uint64_t key, int end, double cx, double cz, ArmRow *out, int max)
{
    int lo, hi, ri, n = 0;
    if (!pool_edge_range(p, key, &lo, &hi)) return 0;
    if (end == 0) {
        int prev_end = 0;
        for (ri = lo; ri < hi && n < max; ri++) {
            const EIdx *ei = &p->ex[ri];
            const RoamTile *t = &p->tl[ei->tile];
            const RoamEdgeRec *er = &t->edge[ei->rec];
            uint32_t q;
            if ((int)er->row0 != prev_end) break;                 /* a gap: a tile that was not read */
            for (q = 0; q < er->span_count && n < max; q++) {
                const TD5_StripSpan *sp = &t->span[er->span_first + q];
                int L = sp->pad_02[1] & 0x0F, vl0, vl1, vr0, vr1;
                if (L < 1) L = 1;
                td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                if (!row_from_span(t, sp, vl0, L, &out[n])) return n ? n : 0;
                n++;
            }
            prev_end = (int)er->row0 + (int)er->span_count;
        }
    } else {
        int prev_row0 = -1;
        for (ri = hi - 1; ri >= lo && n < max; ri--) {
            const EIdx *ei = &p->ex[ri];
            const RoamTile *t = &p->tl[ei->tile];
            const RoamEdgeRec *er = &t->edge[ei->rec];
            int q;
            if (prev_row0 >= 0 && (int)er->row0 + (int)er->span_count != prev_row0) break;
            for (q = (int)er->span_count - 1; q >= 0 && n < max; q--) {
                const TD5_StripSpan *sp = &t->span[er->span_first + (uint32_t)q];
                int L = sp->pad_02[1] & 0x0F, vl0, vl1, vr0, vr1;
                if (L < 1) L = 1;
                td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
                if (q == (int)er->span_count - 1 && prev_row0 < 0) {         /* the very last row, once */
                    if (!row_from_span(t, sp, vr0, L, &out[n])) return n ? n : 0;
                    n++;
                    if (n >= max) break;
                }
                if (!row_from_span(t, sp, vl0, L, &out[n])) return n ? n : 0;
                n++;
            }
            prev_row0 = (int)er->row0;
        }
    }
    if (n > 0) {
        const double dx = 0.5 * (out[0].x0 + out[0].x1) - cx, dz = 0.5 * (out[0].z0 + out[0].z1) - cz;
        int i;
        if (sqrt(dx * dx + dz * dz) > BAR_ROW_FAR_M * BAR_UPM) return 0;
        for (i = 0; i < n; i++) if (!row_fill(&out[i], cx, cz)) { n = i; break; }
        /* the outward side of a row is where the ribbon goes (row 0 -> row 1), not away from the centroid: the
         * centroid of a multi-node cluster is not always behind an arm's cut row */
        if (n >= 2) {
            const double ex = out[1].mx - out[0].mx, ez = out[1].mz - out[0].mz;
            for (i = 0; i < n; i++)
                if (out[i].ox * ex + out[i].oz * ez < 0.0) { out[i].ox = -out[i].ox; out[i].oz = -out[i].oz; }
        }
    }
    return n;
}

/* ray o + t d (|d| = 1) against segment a-b; t in units, -1 when it misses */
static double ray_seg(double ox, double oz, double dx, double dz, double ax, double az, double bx, double bz)
{
    const double ex = bx - ax, ez = bz - az;
    const double den = dx * ez - dz * ex;
    double t, u;
    if (fabs(den) < 1e-9) return -1.0;
    t = ((ax - ox) * ez - (az - oz) * ex) / den;
    u = ((ax - ox) * dz - (az - oz) * dx) / den;
    if (t < 0.0 || u < 0.0 || u > 1.0) return -1.0;
    return t;
}

static int seg_seg(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double rx = bx - ax, rz = bz - az, sx = dx - cx, sz = dz - cz;
    const double den = rx * sz - rz * sx;
    double t, u;
    if (fabs(den) < 1e-9) return 0;
    t = ((cx - ax) * sz - (cz - az) * sx) / den;
    u = ((cx - ax) * rz - (cz - az) * rx) / den;
    return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

static double pt_seg_d2(double px, double pz, double ax, double az, double bx, double bz)
{
    const double ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    double t = l2 > 0.0 ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0;
    double qx, qz;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    qx = ax + ex * t - px; qz = az + ez * t - pz;
    return qx * qx + qz * qz;
}

/* the least distance (units) between segment a-b and the route polyline */
static double seg_route_min_dist(double ax, double az, double bx, double bz, const TD5_RoamRouteResult *r)
{
    double best = 1e300;
    int j;
    for (j = 0; j + 1 < r->npt; j++) {
        const double cx = r->px[j], cz = r->pz[j], dx = r->px[j + 1], dz = r->pz[j + 1];
        double d2;
        if (seg_seg(ax, az, bx, bz, cx, cz, dx, dz)) return 0.0;
        d2 = pt_seg_d2(ax, az, cx, cz, dx, dz); if (d2 < best) best = d2;
        d2 = pt_seg_d2(bx, bz, cx, cz, dx, dz); if (d2 < best) best = d2;
        d2 = pt_seg_d2(cx, cz, ax, az, bx, bz); if (d2 < best) best = d2;
        d2 = pt_seg_d2(dx, dz, ax, az, bx, bz); if (d2 < best) best = d2;
    }
    return sqrt(best);
}

/* 1 when segment a-b stays at least `min_u` units away from the whole route polyline */
static int seg_clear_of_route(double ax, double az, double bx, double bz, const TD5_RoamRouteResult *r, double min_u)
{
    const double m2 = min_u * min_u;
    int j;
    for (j = 0; j + 1 < r->npt; j++) {
        const double cx = r->px[j], cz = r->pz[j], dx = r->px[j + 1], dz = r->pz[j + 1];
        double d2;
        if ((cx < ax - min_u && cx < bx - min_u && dx < ax - min_u && dx < bx - min_u) ||
            (cx > ax + min_u && cx > bx + min_u && dx > ax + min_u && dx > bx + min_u) ||
            (cz < az - min_u && cz < bz - min_u && dz < az - min_u && dz < bz - min_u) ||
            (cz > az + min_u && cz > bz + min_u && dz > az + min_u && dz > bz + min_u)) continue;
        if (seg_seg(ax, az, bx, bz, cx, cz, dx, dz)) return 0;
        d2 = pt_seg_d2(ax, az, cx, cz, dx, dz);
        if (d2 < m2) return 0;
        d2 = pt_seg_d2(bx, bz, cx, cz, dx, dz);
        if (d2 < m2) return 0;
        d2 = pt_seg_d2(cx, cz, ax, az, bx, bz);
        if (d2 < m2) return 0;
        d2 = pt_seg_d2(dx, dz, ax, az, bx, bz);
        if (d2 < m2) return 0;
    }
    return 1;
}

/* ============================================================ placement ======== */

typedef struct { uint64_t key; int end; } ArmId;

typedef struct {
    int64_t   key;
    const NIdx *node;
    double    cx, cz, cy;
    int       narm;
    ArmRow   *row;                 /* per arm: the cut row (a copy of rows[k][0])      */
    ArmRow   *rows;                /* per arm, ARM_ROWS_MAX deep: the cut row, then deeper */
    int      *nrows;
    double    route_half_w;        /* widest route arm here, half carriageway, units   */
    uint8_t  *taken;               /* per arm */
    uint8_t  *blocked;             /* per arm */
    uint8_t  *linked;              /* per arm: left open on purpose, it only leads to another sealed cluster / a dead end */
    ArmId     tk[BAR_MAX_TAKEN];
    int       ntk;
    double    dirx, dirz;          /* the route's exit direction (arrow) */
    double    idirx, idirz;        /* the route's entry travel direction (0 when the route starts here)           */
    int       have_dir, have_in, have_out;
    int       degree;              /* planner graph degree, -1 unknown */
    int       origin;              /* 1 + index of the route cluster this one was glued to, 0 = a route cluster */
} Cl;

typedef struct { int cl; int64_t ckey; int has_in, has_out; ArmId in, out; } Pass;

typedef struct { uint64_t key; int found; int64_t ek[2]; uint8_t kind[2]; uint8_t flags; int8_t oneway; } REdge;

static int cl_find(Cl *c, int n, int64_t key)
{
    int i;
    for (i = 0; i < n; i++) if (c[i].key == key) return i;
    return -1;
}

static void cl_take(Cl *c, uint64_t key, int end)
{
    int i;
    for (i = 0; i < c->ntk; i++) if (c->tk[i].key == key && c->tk[i].end == end) return;
    if (c->ntk < BAR_MAX_TAKEN) { c->tk[c->ntk].key = key; c->tk[c->ntk].end = end; c->ntk++; }
}

/* the arm index of a cluster for (edge key, end), -1 when it has none */
static int cl_arm_index(const Pool *p, const Cl *c, uint64_t key, int end)
{
    const RoamTile *t = &p->tl[c->node->tile];
    const RoamNodeRec *nr = &t->node[c->node->rec];
    int i;
    for (i = 0; i < c->narm; i++) {
        const RoamArmRec *ar = &t->arm[nr->arm_first + i];
        if (ar->edge_key == key && ar->end == end) return i;
    }
    return -1;
}

/* where the chevron board of a turn stands: along the entry axis until the far kerb / another arm */
static double board_depth(const Pool *p, const Cl *c, int in_arm, const ArmRow *rin, double tx, double tz)
{
    const RoamTile *t = &p->tl[c->node->tile];
    const RoamNodeRec *nr = &t->node[c->node->rec];
    double best = 1e300;
    uint32_t k;
    int i;
    for (i = 0; i < c->narm; i++) {
        double h;
        if (i == in_arm || !c->row[i].ok) continue;
        h = ray_seg(rin->mx, rin->mz, tx, tz, c->row[i].x0, c->row[i].z0, c->row[i].x1, c->row[i].z1);
        if (h > 1.5 * BAR_UPM && h < best) best = h;
    }
    for (k = 0; k < t->nbedge; k++) {
        const RoamBEdge *be = &t->bedge[k];
        const RoamTri *tr;
        double ax, az, bx, bz, h;
        uint32_t ti;
        if (be->kind != ROAM_BEDGE_KERB || !(be->prim & ROAM_PRIM_TRI)) continue;
        ti = (uint32_t)(be->prim & ~ROAM_PRIM_TRI);
        if (ti < nr->tri_first || ti >= (uint32_t)nr->tri_first + nr->tri_count || ti >= t->ntri) continue;
        if (be->v[0] >= t->nvert || be->v[1] >= t->nvert) continue;
        tr = &t->tri[ti];
        ax = (double)t->org_x + tr->ox + t->vert[be->v[0]].x; az = (double)t->org_z + tr->oz + t->vert[be->v[0]].z;
        bx = (double)t->org_x + tr->ox + t->vert[be->v[1]].x; bz = (double)t->org_z + tr->oz + t->vert[be->v[1]].z;
        h = ray_seg(rin->mx, rin->mz, tx, tz, ax, az, bx, bz);
        if (h > 1.5 * BAR_UPM && h < best) best = h;
    }
    return best < 1e299 ? best : -1.0;
}

static int cl_init(Cl *c, int64_t key, const NIdx *nd, const Pool *pool)
{
    const RoamTile *t;
    const RoamNodeRec *nr;
    size_t n;
    memset(c, 0, sizeof *c);
    c->key = key; c->node = nd; c->degree = -1;
    if (!nd) return 1;
    t = &pool->tl[nd->tile];
    nr = &t->node[nd->rec];
    n = nr->narm ? nr->narm : 1;
    c->cx = (double)t->org_x + nr->x; c->cz = (double)t->org_z + nr->z; c->cy = nr->y;
    c->narm = nr->narm;
    c->row = (ArmRow *)calloc(n, sizeof(ArmRow));
    c->rows = (ArmRow *)calloc(n * ARM_ROWS_MAX, sizeof(ArmRow));
    c->nrows = (int *)calloc(n, sizeof(int));
    c->taken = (uint8_t *)calloc(n, 1);
    c->blocked = (uint8_t *)calloc(n, 1);
    c->linked = (uint8_t *)calloc(n, 1);
    return c->row && c->rows && c->nrows && c->taken && c->blocked && c->linked;
}

static void cl_free(Cl *c)
{
    free(c->row); free(c->rows); free(c->nrows); free(c->taken); free(c->blocked); free(c->linked);
}

/* rows of every arm, the taken flags (from c->tk) and the widest route arm */
static void cl_compute(const Pool *pool, Cl *c)
{
    const RoamTile *t;
    const RoamNodeRec *nr;
    int k, j;
    if (!c->node) return;
    t = &pool->tl[c->node->tile];
    nr = &t->node[c->node->rec];
    for (k = 0; k < c->narm; k++) {
        const RoamArmRec *ar = &t->arm[nr->arm_first + k];
        c->nrows[k] = arm_rows(pool, ar->edge_key, ar->end, c->cx, c->cz, &c->rows[(size_t)k * ARM_ROWS_MAX], ARM_ROWS_MAX);
        if (c->nrows[k] > 0) c->row[k] = c->rows[(size_t)k * ARM_ROWS_MAX];
        for (j = 0; j < c->ntk; j++) if (c->tk[j].key == ar->edge_key && c->tk[j].end == ar->end) c->taken[k] = 1;
        if (c->taken[k]) {
            double hw = ar->lanes * 750.0;
            if (c->nrows[k] > 0) {
                const double rx = c->row[k].x1 - c->row[k].x0, rz = c->row[k].z1 - c->row[k].z0;
                hw = 0.5 * sqrt(rx * rx + rz * rz);          /* the route arm's own half carriageway */
            }
            if (hw > c->route_half_w) c->route_half_w = hw;
        }
    }
    c->degree = td5_roam_route_graph_node_degree(c->key);
}

/* the cluster on the other side of a ribbon-less (glued) edge: any other node record with an arm on it */
static int64_t pool_partner(const Pool *p, uint64_t edge_key, int64_t self_key)
{
    int i;
    for (i = 0; i < p->nnx; i++) {
        const RoamTile *t = &p->tl[p->nx[i].tile];
        const RoamNodeRec *nr = &t->node[p->nx[i].rec];
        int k;
        if (nr->cluster_key == self_key) continue;
        for (k = 0; k < nr->narm; k++)
            if (t->arm[nr->arm_first + k].edge_key == edge_key) return nr->cluster_key;
    }
    return 0;
}

typedef struct { int boards_fallback; int boards_hit; int boards_alt; int boards_dropped; } PlaceStats;

int td5_roam_barriers_build(const TD5_RoamRouteResult *route)
{
    Pool pool;
    REdge *re = NULL;
    Pass *ps = NULL;
    Cl *cl = NULL;
    int npass = 0, ncl = 0, i, a;
    const uint32_t t0 = td5_plat_time_ms();
    const char *place = td5_roam_route_place();
    int32_t ox = 0, oz = 0;
    PlaceStats pst = { 0, 0, 0, 0 };
    int nboard_dup = 0, clcap = 0;

    td5_roam_barriers_clear();
    memset(&pool, 0, sizeof pool);
    if (!route || !route->ok || route->nedge < 1 || route->npt < 2 || !place || !place[0]) return 0;
    B.st.route_hash = route->hash;
    if (!pool_load(&pool, place, route)) { pool_free(&pool); return 0; }
    B.st.tiles_read = pool.ntl;

    re = (REdge *)calloc((size_t)route->nedge, sizeof *re);
    ps = (Pass *)calloc((size_t)route->nedge + 2, sizeof *ps);
    clcap = route->nedge * 3 + 64;
    cl = (Cl *)calloc((size_t)clcap, sizeof *cl);
    if (!re || !ps || !cl) goto oom;

    /* ---- edge facts ------------------------------------------------------------ */
    for (i = 0; i < route->nedge; i++) {
        int lo, hi;
        re[i].key = route->edge[i].key;
        if (pool_edge_range(&pool, re[i].key, &lo, &hi)) {
            const EIdx *ei = &pool.ex[lo];
            const RoamEdgeRec *er = &pool.tl[ei->tile].edge[ei->rec];
            re[i].found = 1;
            re[i].ek[0] = er->end_key[0]; re[i].ek[1] = er->end_key[1];
            re[i].kind[0] = er->end_kind[0]; re[i].kind[1] = er->end_kind[1];
            re[i].flags = er->flags; re[i].oneway = (int8_t)er->oneway;
            if (er->flags & ROAM_EF_ROUNDABOUT) B.st.ring_edges++;
            if (er->oneway) B.st.oneway_edges++;
        }
    }

    /* ---- passes ---------------------------------------------------------------- */
#define ENTRY_END(i) (route->edge[(i)].dir > 0 ? 0 : 1)
#define EXIT_END(i)  (route->edge[(i)].dir > 0 ? 1 : 0)
    /* start pocket: the cluster behind the first edge */
    if (re[0].found && re[0].kind[ENTRY_END(0)] == TD5_ROAM_END_CLUSTER) {
        Pass *q = &ps[npass++];
        q->ckey = re[0].ek[ENTRY_END(0)];
        q->has_out = 1; q->out.key = re[0].key; q->out.end = ENTRY_END(0);
    }
    for (i = 0; i + 1 < route->nedge; i++) {
        const int ex = EXIT_END(i), en = ENTRY_END(i + 1);
        if (!re[i].found || !re[i + 1].found) continue;
        if (re[i].key == re[i + 1].key) continue;                    /* a waypoint split / turn-round on one edge */
        if (re[i].ek[ex] != re[i + 1].ek[en]) { B.st.pass_gaps++; continue; }
        if (re[i].kind[ex] != TD5_ROAM_END_CLUSTER && re[i + 1].kind[en] != TD5_ROAM_END_CLUSTER) continue;   /* a join / dead end */
        {
            Pass *q = &ps[npass++];
            q->ckey = re[i].ek[ex];
            q->has_in = 1;  q->in.key = re[i].key;      q->in.end = ex;
            q->has_out = 1; q->out.key = re[i + 1].key; q->out.end = en;
        }
    }
    {   /* finish pocket: the cluster past the last edge */
        const int last = route->nedge - 1;
        if (re[last].found && re[last].kind[EXIT_END(last)] == TD5_ROAM_END_CLUSTER) {
            Pass *q = &ps[npass++];
            q->ckey = re[last].ek[EXIT_END(last)];
            q->has_in = 1; q->in.key = re[last].key; q->in.end = EXIT_END(last);
        }
    }
    for (a = 0; a < npass; a++) {
        const int64_t key = ps[a].ckey;
        int ci = cl_find(cl, ncl, key);
        if (ci < 0) {
            ci = ncl++;
            if (!cl_init(&cl[ci], key, pool_node(&pool, key), &pool)) goto oom;
        }
        ps[a].cl = ci;
        if (!cl[ci].node) { B.st.missing_cluster++; continue; }
        if (ps[a].has_in)  cl_take(&cl[ci], ps[a].in.key,  ps[a].in.end);
        if (ps[a].has_out) cl_take(&cl[ci], ps[a].out.key, ps[a].out.end);
    }
    B.st.passes = npass;
    B.st.clusters_passed = ncl;

    /* ---- rows of every arm, taken flags ---------------------------------------- */
    for (i = 0; i < ncl; i++) cl_compute(&pool, &cl[i]);

    /* ---- glued neighbours: an arm whose edge has no ribbon leads straight into the next junction patch, so
     * that neighbour joins the set (all its arms but the glued one get barriers) ---------------------- */
    {
        int iter;
        for (iter = 0; iter < 4; iter++) {
            int added = 0, n0 = ncl;
            for (i = 0; i < n0; i++) {
                const RoamTile *tt;
                const RoamNodeRec *nr;
                int k;
                if (!cl[i].node) continue;
                tt = &pool.tl[cl[i].node->tile];
                nr = &tt->node[cl[i].node->rec];
                for (k = 0; k < cl[i].narm; k++) {
                    const RoamArmRec *ar = &tt->arm[nr->arm_first + k];
                    int64_t pk;
                    if (cl[i].taken[k] || cl[i].nrows[k] > 0) continue;
                    pk = pool_partner(&pool, ar->edge_key, cl[i].key);
                    if (pk == 0 || cl_find(cl, ncl, pk) >= 0) continue;
                    if (ncl >= clcap) { TD5_LOG_W(LOG_TAG, "BARRIERS: cluster table full"); continue; }
                    if (!cl_init(&cl[ncl], pk, pool_node(&pool, pk), &pool)) { ncl++; goto oom; }
                    if (!cl[ncl].node) { cl_free(&cl[ncl]); memset(&cl[ncl], 0, sizeof cl[ncl]); continue; }
                    cl_compute(&pool, &cl[ncl]);
                    cl[ncl].origin = i + 1;
                    ncl++; added++; B.st.clusters_expanded++;
                }
            }
            if (!added) break;
        }
    }
    /* arrow directions per cluster: the entry and exit travel direction of its first pass (a start pocket has only an
     * exit, a finish pocket only an entry) */
    for (a = 0; a < npass; a++) {
        Cl *c = &cl[ps[a].cl];
        int k;
        if (!c->node || c->have_dir) continue;
        if (ps[a].has_out && (k = cl_arm_index(&pool, c, ps[a].out.key, ps[a].out.end)) >= 0 && c->row[k].ok) {
            c->dirx = c->row[k].ox; c->dirz = c->row[k].oz; c->have_out = 1;
        }
        if (ps[a].has_in && (k = cl_arm_index(&pool, c, ps[a].in.key, ps[a].in.end)) >= 0 && c->row[k].ok) {
            c->idirx = -c->row[k].ox; c->idirz = -c->row[k].oz; c->have_in = 1;
        }
        c->have_dir = c->have_in || c->have_out;
    }
    for (i = 0; i < ncl; i++)
        if (cl[i].origin > 0 && !cl[i].have_dir && cl[cl[i].origin - 1].have_dir) {
            const Cl *o = &cl[cl[i].origin - 1];
            cl[i].dirx = o->dirx; cl[i].dirz = o->dirz; cl[i].idirx = o->idirx; cl[i].idirz = o->idirz;
            cl[i].have_in = o->have_in; cl[i].have_out = o->have_out; cl[i].have_dir = 1;
        }

    /* ---- mouth barriers -------------------------------------------------------- */
    for (i = 0; i < ncl; i++) {
        Cl *c = &cl[i];
        int k;
        if (!c->node) continue;
        for (k = 0; k < c->narm; k++) {
            const RoamTile *tt = &pool.tl[c->node->tile];
            const RoamArmRec *ar = &tt->arm[tt->node[c->node->rec].arm_first + k];
            double clear_u = c->route_half_w - 2.0 * BAR_UPM;           /* a barrier this far in from the route's kerb is on its pavement */
            if (clear_u < 1.5 * BAR_UPM) clear_u = 1.5 * BAR_UPM;
            PlBar pb;
            double ax, az, dotv;
            int sign = 1, r, placed = 0;
            B.st.arms_total++;
            if (c->taken[k]) { B.st.arms_route++; continue; }
            if (c->nrows[k] <= 0) {
                B.st.arm_no_row++;
                if (B.st.arm_no_row <= 8)
                    TD5_LOG_W(LOG_TAG, "BARRIERS: arm edge %llX end %d of cluster %lld has no ribbon row (cut_span %u, lanes %d)",
                              (unsigned long long)ar->edge_key, ar->end, (long long)c->key, (unsigned)ar->cut_span, ar->lanes);
                continue;
            }
            /* the cut row first; when it lies across the route (a Y fork: two carriageways overlap beside the
             * junction) slide it outward along the arm until it clears the route's own carriageway */
            for (r = 0; r < c->nrows[k] && !placed; r++) {
                const ArmRow *rw = &c->rows[(size_t)k * ARM_ROWS_MAX + (size_t)r];
                const double rl = sqrt((rw->x1 - rw->x0) * (rw->x1 - rw->x0) + (rw->z1 - rw->z0) * (rw->z1 - rw->z0));
                ax = (rw->x1 - rw->x0) / rl; az = (rw->z1 - rw->z0) / rl;
                pb.x0 = rw->x0 + rw->ox * BAR_OUT_M * BAR_UPM - ax * BAR_KERB_M * BAR_UPM;
                pb.z0 = rw->z0 + rw->oz * BAR_OUT_M * BAR_UPM - az * BAR_KERB_M * BAR_UPM;
                pb.x1 = rw->x1 + rw->ox * BAR_OUT_M * BAR_UPM + ax * BAR_KERB_M * BAR_UPM;
                pb.z1 = rw->z1 + rw->oz * BAR_OUT_M * BAR_UPM + az * BAR_KERB_M * BAR_UPM;
                pb.y0 = rw->y0; pb.y1 = rw->y1;
                /* the ends are judged 0.8 m in: two carriageways side by side (a divided avenue) overlap at their kerbs */
                if (!seg_clear_of_route(pb.x0 + ax * (BAR_KERB_M + 0.8) * BAR_UPM, pb.z0 + az * (BAR_KERB_M + 0.8) * BAR_UPM,
                                        pb.x1 - ax * (BAR_KERB_M + 0.8) * BAR_UPM, pb.z1 - az * (BAR_KERB_M + 0.8) * BAR_UPM, route, clear_u)) continue;
                placed = 1;
                if (r > 0) B.st.slid++;
            }
            if (!placed) {
                B.st.slide_fail++;
                if (B.st.slide_fail <= 8)
                {
                    const ArmRow *r0 = &c->rows[(size_t)k * ARM_ROWS_MAX];
                    TD5_LOG_W(LOG_TAG, "BARRIERS: arm edge %llX end %d of cluster %lld: no row of its first %d clears the route (clearance %.1f m, row 0 mid %.1f,%.1f m, row 0 least distance %.2f m, row 0 length %.1f m, last row least distance %.2f m)",
                              (unsigned long long)ar->edge_key, ar->end, (long long)c->key, c->nrows[k], clear_u / BAR_UPM,
                              r0->mx / BAR_UPM, r0->mz / BAR_UPM, seg_route_min_dist(r0->x0, r0->z0, r0->x1, r0->z1, route) / BAR_UPM,
                              sqrt((r0->x1 - r0->x0) * (r0->x1 - r0->x0) + (r0->z1 - r0->z0) * (r0->z1 - r0->z0)) / BAR_UPM,
                              seg_route_min_dist(c->rows[(size_t)k * ARM_ROWS_MAX + (size_t)(c->nrows[k] - 1)].x0, c->rows[(size_t)k * ARM_ROWS_MAX + (size_t)(c->nrows[k] - 1)].z0,
                                                 c->rows[(size_t)k * ARM_ROWS_MAX + (size_t)(c->nrows[k] - 1)].x1, c->rows[(size_t)k * ARM_ROWS_MAX + (size_t)(c->nrows[k] - 1)].z1, route) / BAR_UPM);
                }
                continue;
            }
            /* the chevrons point along the barrier the way the route runs there: its exit direction, or its entry
             * direction when that one lies clearer along this barrier (a cross street of a turn: the barrier is
             * parallel to the road the car arrives on) */
            {
                const double de = c->have_out ? c->dirx * ax + c->dirz * az : 0.0;
                const double di = c->have_in ? c->idirx * ax + c->idirz * az : 0.0;
                dotv = fabs(de) >= fabs(di) ? de : di;
                if (!c->have_dir) dotv = 1.0;
            }
            sign = dotv >= 0.0 ? 1 : -1;
            if (!bar_add(&pb, TD5_ROAM_BARRIER_MOUTH, (uint64_t)c->key, sign, ar->edge_key, ar->end)) goto oom;
            B.aux[B.n - 1].dirx = c->dirx; B.aux[B.n - 1].dirz = c->dirz;
            c->blocked[k] = 1;
            B.st.arms_blocked++;
            B.st.barriers_mouth++;
        }
    }

    /* ---- turn boards ----------------------------------------------------------- */
    for (a = 0; a < npass; a++) {
        Cl *c = &cl[ps[a].cl];
        int ki, ko, dup = 0, b2;
        double tx, tz, dotv, defl, depth, lx, lz, cxp, czp, y;
        PlBar pb;
        if (!c->node || !ps[a].has_in || !ps[a].has_out) continue;
        ki = cl_arm_index(&pool, c, ps[a].in.key, ps[a].in.end);
        ko = cl_arm_index(&pool, c, ps[a].out.key, ps[a].out.end);
        if (ki < 0 || ko < 0 || !c->row[ki].ok || !c->row[ko].ok) continue;
        for (b2 = 0; b2 < a; b2++)
            if (ps[b2].cl == ps[a].cl && ps[b2].has_in && ps[b2].has_out && ps[b2].in.key == ps[a].in.key &&
                ps[b2].in.end == ps[a].in.end && ps[b2].out.key == ps[a].out.key && ps[b2].out.end == ps[a].out.end) dup = 1;
        if (dup) { nboard_dup++; continue; }
        tx = -c->row[ki].ox; tz = -c->row[ki].oz;
        dotv = tx * c->row[ko].ox + tz * c->row[ko].oz;
        if (dotv > 1.0) dotv = 1.0;
        if (dotv < -1.0) dotv = -1.0;
        defl = acos(dotv) * 180.0 / 3.14159265358979323846;
        if (defl <= BAR_TURN_DEG) continue;
        depth = board_depth(&pool, c, ki, &c->row[ki], tx, tz);
        if (depth < 0.0) { depth = 8.0 * BAR_UPM; pst.boards_fallback++; } else { depth -= 1.2 * BAR_UPM; pst.boards_hit++; }
        if (depth < 2.0 * BAR_UPM) depth = 2.0 * BAR_UPM;
        cxp = c->row[ki].mx + tx * depth; czp = c->row[ki].mz + tz * depth;
        lx = -tz; lz = tx;                                   /* lateral axis of the board */
        pb.x0 = cxp - lx * 0.5 * BAR_BOARD_W_M * BAR_UPM; pb.z0 = czp - lz * 0.5 * BAR_BOARD_W_M * BAR_UPM;
        pb.x1 = cxp + lx * 0.5 * BAR_BOARD_W_M * BAR_UPM; pb.z1 = czp + lz * 0.5 * BAR_BOARD_W_M * BAR_UPM;
        y = 0.5 * (c->row[ki].y0 + c->row[ki].y1);
        y = y + (c->cy - y) * (depth / (depth + 6.0 * BAR_UPM));   /* toward the patch height as it goes in */
        pb.y0 = pb.y1 = y;
        if (!seg_clear_of_route(pb.x0, pb.z0, pb.x1, pb.z1, route, 1.0 * BAR_UPM)) {
            /* straight ahead is on the route (a ring): stand on the outer kerb of the exit arm instead */
            const ArmRow *ro = &c->row[ko];
            const double side = lx * c->row[ko].ox + lz * c->row[ko].oz;      /* + : the route turns toward +lateral */
            const double d0 = (ro->x0 * lx + ro->z0 * lz) * (side >= 0.0 ? 1.0 : -1.0);
            const double d1 = (ro->x1 * lx + ro->z1 * lz) * (side >= 0.0 ? 1.0 : -1.0);
            const double qx = d0 <= d1 ? ro->x0 : ro->x1, qz = d0 <= d1 ? ro->z0 : ro->z1;       /* the outer kerb point */
            const double wx = d0 <= d1 ? ro->x1 : ro->x0, wz = d0 <= d1 ? ro->z1 : ro->z0;
            double ux = wx - qx, uz = wz - qz;
            const double ul = sqrt(ux * ux + uz * uz);
            if (ul > 1.0) {
                ux /= ul; uz /= ul;
                cxp = qx + ux * 2.2 * BAR_UPM; czp = qz + uz * 2.2 * BAR_UPM;
                pb.x0 = cxp - lx * 0.5 * BAR_BOARD_W_M * BAR_UPM; pb.z0 = czp - lz * 0.5 * BAR_BOARD_W_M * BAR_UPM;
                pb.x1 = cxp + lx * 0.5 * BAR_BOARD_W_M * BAR_UPM; pb.z1 = czp + lz * 0.5 * BAR_BOARD_W_M * BAR_UPM;
                pb.y0 = pb.y1 = 0.5 * (ro->y0 + ro->y1);
            }
            if (ul <= 1.0 || !seg_clear_of_route(pb.x0, pb.z0, pb.x1, pb.z1, route, 1.0 * BAR_UPM)) { pst.boards_dropped++; continue; }
            pst.boards_alt++;
        }
        {
            const double s = lx * c->row[ko].ox + lz * c->row[ko].oz;     /* which side the route leaves on */
            if (!bar_add(&pb, TD5_ROAM_BARRIER_TURN, (uint64_t)c->key, s >= 0.0 ? 1 : -1, ps[a].in.key, ps[a].in.end)) goto oom;
        }
        B.st.barriers_turn++;
    }

    /* ---- arms that got no barrier: contained, or genuinely open ------------------
     * An arm that lies along the route (a twin carriageway the cut row never clears), or whose edge has no
     * ribbon row, is safe when the edge only leads to another cluster of this set (whose other arms are
     * sealed) or to a dead end: nothing leaves the route through it. Anything else stays counted OPEN. */
    for (i = 0; i < ncl; i++) {
        const RoamTile *tt;
        const RoamNodeRec *nr;
        int k;
        if (!cl[i].node) continue;
        tt = &pool.tl[cl[i].node->tile];
        nr = &tt->node[cl[i].node->rec];
        for (k = 0; k < cl[i].narm; k++) {
            const RoamArmRec *ar = &tt->arm[nr->arm_first + k];
            int j, m, found = 0;
            if (cl[i].taken[k] || cl[i].blocked[k]) continue;
            for (j = 0; j < ncl && !found; j++) {
                const RoamTile *t2;
                const RoamNodeRec *n2;
                if (!cl[j].node) continue;
                t2 = &pool.tl[cl[j].node->tile];
                n2 = &t2->node[cl[j].node->rec];
                for (m = 0; m < cl[j].narm; m++) {
                    const RoamArmRec *a2 = &t2->arm[n2->arm_first + m];
                    if (a2->edge_key == ar->edge_key && (j != i || m != k) && (a2->end != ar->end || j != i)) { found = 1; break; }
                }
            }
            if (!found) {
                int lo, hi;
                if (pool_edge_range(&pool, ar->edge_key, &lo, &hi)) {
                    const EIdx *ei = &pool.ex[lo];
                    const RoamEdgeRec *er = &pool.tl[ei->tile].edge[ei->rec];
                    if (er->end_kind[1 - (ar->end ? 1 : 0)] == TD5_ROAM_END_DEAD) found = 1;       /* a dead-end pocket */
                }
            }
            if (found) { cl[i].linked[k] = 1; B.st.arms_linked++; }
        }
    }

#ifndef TD5RE_RELEASE
    free(B.dbg); B.dbg = NULL; B.ndbg = B.capdbg = 0;
    for (i = 0; i < ncl; i++) {
        const RoamTile *tt;
        const RoamNodeRec *nr;
        int k;
        if (!cl[i].node) continue;
        tt = &pool.tl[cl[i].node->tile];
        nr = &tt->node[cl[i].node->rec];
        for (k = 0; k < cl[i].narm; k++) {
            const RoamArmRec *ar = &tt->arm[nr->arm_first + k];
            struct DbgArm *d;
            if (B.ndbg >= B.capdbg) {
                void *q = realloc(B.dbg, (size_t)(B.capdbg ? B.capdbg * 2 : 256) * sizeof *B.dbg);
                if (!q) break;
                B.dbg = (struct DbgArm *)q; B.capdbg = B.capdbg ? B.capdbg * 2 : 256;
            }
            d = &B.dbg[B.ndbg++];
            d->status = cl[i].taken[k] ? 0 : (cl[i].blocked[k] ? 1 : (cl[i].linked[k] ? 2 : 3));
            d->ck = (unsigned long long)cl[i].key; d->ek = (unsigned long long)ar->edge_key; d->end = ar->end;
            if (cl[i].nrows[k] > 0) { const ArmRow *rw = &cl[i].rows[(size_t)k * ARM_ROWS_MAX]; d->x0 = rw->x0; d->z0 = rw->z0; d->x1 = rw->x1; d->z1 = rw->z1; }
            else { d->x0 = d->x1 = cl[i].cx; d->z0 = d->z1 = cl[i].cz; }
        }
    }
#endif
    /* ---- census checks --------------------------------------------------------- */
    for (i = 0; i < ncl; i++) {
        int k, open_here = 0;
        if (!cl[i].node) continue;
        for (k = 0; k < cl[i].narm; k++) if (!cl[i].taken[k] && !cl[i].blocked[k] && !cl[i].linked[k]) open_here++;
        B.st.arms_open += open_here;
        if (cl[i].degree >= 0) {
            /* independent: the planner graph's own count of ribbon ends at this node */
            int have = 0;
            for (k = 0; k < cl[i].narm; k++) if (cl[i].taken[k] || cl[i].blocked[k] || cl[i].linked[k]) have++;
            if (cl[i].degree > have) B.st.degree_open += cl[i].degree - have;
        }
    }
    for (i = 0; i < B.n; i++) {
        int j;
        for (j = 0; j + 1 < route->npt; j++) {
            if (seg_seg(B.pl[i].x0, B.pl[i].z0, B.pl[i].x1, B.pl[i].z1, route->px[j], route->pz[j], route->px[j + 1], route->pz[j + 1])) {
                B.st.across_route++;
                if (B.st.across_route <= 8)
                    TD5_LOG_W(LOG_TAG, "BARRIERS: barrier %d (kind %d, cluster %lld, arm edge %llX end %d) crosses the route polyline at point %d",
                              i, B.pub[i].kind, (long long)B.pub[i].cluster_key, (unsigned long long)B.aux[i].arm_key, B.aux[i].arm_end, j);
                break;
            }
        }
    }

    /* arrow census: the chevrons of a mouth barrier point along (sign * axis); compare with the route direction nearest to it */
    for (i = 0; i < B.n; i++) {
        double mx, mz, best = 1e300, rx = 0.0, rz = 0.0, cosv;
        int j;
        if (B.pub[i].kind != TD5_ROAM_BARRIER_MOUTH) continue;
        mx = 0.5 * (B.pl[i].x0 + B.pl[i].x1); mz = 0.5 * (B.pl[i].z0 + B.pl[i].z1);
        for (j = 0; j + 1 < route->npt; j++) {
            const double d2 = pt_seg_d2(mx, mz, route->px[j], route->pz[j], route->px[j + 1], route->pz[j + 1]);
            if (d2 < best) {
                const double dx = route->px[j + 1] - route->px[j], dz = route->pz[j + 1] - route->pz[j], l = sqrt(dx * dx + dz * dz);
                best = d2;
                if (l > 1e-6) { rx = dx / l; rz = dz / l; }
            }
        }
        cosv = (B.aux[i].ax * rx + B.aux[i].az * rz) * (double)B.aux[i].sign;
        /* a proxy: the nearest polyline direction is only trustworthy on the straight, within 15 m of the barrier */
        if (cosv > 0.3) B.st.arrows_with++;
        else if (cosv > -0.6 || best > 15.0 * BAR_UPM * 15.0 * BAR_UPM) B.st.arrows_side++;
        else {
            B.st.arrows_against++;
            if (B.st.arrows_against <= 6)
                TD5_LOG_W(LOG_TAG, "BARRIERS: chevrons of barrier %d (cluster %lld, arm edge %llX end %d) point against the nearest route direction: cos %.2f, route dir (%.2f,%.2f), axis (%.2f,%.2f) sign %d, cluster exit dir (%.2f,%.2f), nearest route point %.1f m away",
                          i, (long long)B.pub[i].cluster_key, (unsigned long long)B.aux[i].arm_key, B.aux[i].arm_end, cosv, rx, rz, B.aux[i].ax, B.aux[i].az, B.aux[i].sign, B.aux[i].dirx, B.aux[i].dirz, sqrt(best) / BAR_UPM);
        }
    }
    td5_roam_world_origin(&ox, &oz);
    bar_derive(ox, oz);
    B.have = 1;
    B.built_hash = route->hash; B.built_nedge = route->nedge;
    B.st.build_ms = (double)(td5_plat_time_ms() - t0);
    TD5_LOG_I(LOG_TAG, "BARRIERS: built %d (mouth %d, turn %d) for %d clusters / %d passes, tiles read %d, arms total %d route %d blocked %d linked %d open %d "
              "(no row %d, planner-degree open %d), across route %d, missing cluster %d, board hit %d fallback %d outer-kerb %d dropped %d dup %d, "
              "slid %d slide-fail %d, arrows with/side/against the route %d/%d/%d, %.0f ms",
              B.n, B.st.barriers_mouth, B.st.barriers_turn, ncl, npass, pool.ntl, B.st.arms_total, B.st.arms_route, B.st.arms_blocked,
              B.st.arms_linked, B.st.arms_open, B.st.arm_no_row, B.st.degree_open, B.st.across_route, B.st.missing_cluster, pst.boards_hit,
              pst.boards_fallback, pst.boards_alt, pst.boards_dropped, nboard_dup, B.st.slid, B.st.slide_fail,
              B.st.arrows_with, B.st.arrows_side, B.st.arrows_against, B.st.build_ms);
    B.st.boards_alt = pst.boards_alt; B.st.boards_dropped = pst.boards_dropped;
    for (i = 0; i < ncl; i++) { cl_free(&cl[i]); }
    free(cl); free(ps); free(re);
    pool_free(&pool);
    return B.n;
oom:
    TD5_LOG_E(LOG_TAG, "BARRIERS: out of memory");
    if (cl) for (i = 0; i < ncl; i++) { cl_free(&cl[i]); }
    free(cl); free(ps); free(re);
    pool_free(&pool);
    td5_roam_barriers_clear();
    return 0;
}

/* ============================================================ drawing ========== */

#define BQ_MAX        1400
#define BAR_DRAW_M    480.0
#define BAR_CELL_M    3.0
#define BAR_MAX_DRAW  96

static TD5_D3DVertex q_v[BQ_MAX * 4];
static uint16_t      q_i[BQ_MAX * 6];
static int           q_n;

static void q_flush(TD5_RenderPreset preset)
{
    if (q_n > 0) {
        td5_plat_render_set_preset(preset);
        td5_plat_render_bind_texture(HUD_WHITE_TEX_PAGE);
        td5_plat_render_draw_tris(q_v, q_n * 4, q_i, q_n * 6);
        q_n = 0;
    }
}

static void q_quad(TD5_RenderPreset preset, const float a[3], const float b[3], const float c[3], const float d[3],
                   uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd)
{
    TD5_D3DVertex *v;
    uint16_t *ix;
    uint16_t base;
    if (q_n >= BQ_MAX) q_flush(preset);
    v = &q_v[q_n * 4];
    if (!td5_render_project_world_vertex(a[0], a[1], a[2], ca, &v[0]) ||
        !td5_render_project_world_vertex(b[0], b[1], b[2], cb, &v[1]) ||
        !td5_render_project_world_vertex(c[0], c[1], c[2], cc, &v[2]) ||
        !td5_render_project_world_vertex(d[0], d[1], d[2], cd, &v[3])) return;
    base = (uint16_t)(q_n * 4);
    ix = &q_i[q_n * 6];
    ix[0] = base; ix[1] = (uint16_t)(base + 1); ix[2] = (uint16_t)(base + 2);
    ix[3] = base; ix[4] = (uint16_t)(base + 2); ix[5] = (uint16_t)(base + 3);
    q_n++;
}

static uint32_t argb(int a, float r, float g, float b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    if (ri < 0) ri = 0;
    if (ri > 255) ri = 255;
    if (gi < 0) gi = 0;
    if (gi > 255) gi = 255;
    if (bi < 0) bi = 0;
    if (bi > 255) bi = 255;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return ((uint32_t)a << 24) | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

/* a panel: base line A -> B in render floats (y per end), u along it, v up (both in render units) */
typedef struct { float ax, ay, az, bx, by, bz, len; } Panel;

static void pn_pt(const Panel *p, float u, float v, float o[3])
{
    const float t = p->len > 0.0f ? u / p->len : 0.0f;
    o[0] = p->ax + (p->bx - p->ax) * t;
    o[1] = p->ay + (p->by - p->ay) * t + v;
    o[2] = p->az + (p->bz - p->az) * t;
}

/* a rectangle of the panel with a colour on its bottom edge and one on its top edge */
static void pn_rect(TD5_RenderPreset ps, const Panel *p, float u0, float u1, float v0, float v1, uint32_t cb, uint32_t ct)
{
    float a[3], b[3], c[3], d[3];
    pn_pt(p, u0, v0, a); pn_pt(p, u1, v0, b); pn_pt(p, u1, v1, c); pn_pt(p, u0, v1, d);
    q_quad(ps, a, b, c, d, cb, cb, ct, ct);
}

/* a slanted band between two panel points, thickness t in v */
static void pn_band(TD5_RenderPreset ps, const Panel *p, float u0, float v0, float u1, float v1, float t, uint32_t col)
{
    float a[3], b[3], c[3], d[3];
    pn_pt(p, u0, v0 - t, a); pn_pt(p, u1, v1 - t, b); pn_pt(p, u1, v1 + t, c); pn_pt(p, u0, v0 + t, d);
    q_quad(ps, a, b, c, d, col, col, col, col);
}

typedef struct { int idx; float d2; } DrawItem;
static int cmp_far_first(const void *a, const void *b)
{
    const DrawItem *x = (const DrawItem *)a, *y = (const DrawItem *)b;
    return x->d2 < y->d2 ? 1 : (x->d2 > y->d2 ? -1 : 0);
}

static void make_panel(int i, Panel *p)
{
    p->ax = (float)B.pub[i].x0 * (1.0f / 256.0f); p->az = (float)B.pub[i].z0 * (1.0f / 256.0f);
    p->bx = (float)B.pub[i].x1 * (1.0f / 256.0f); p->bz = (float)B.pub[i].z1 * (1.0f / 256.0f);
    p->ay = (float)B.pub[i].y0 * (1.0f / 256.0f); p->by = (float)B.pub[i].y1 * (1.0f / 256.0f);
    { const float dx = p->bx - p->ax, dz = p->bz - p->az; p->len = sqrtf(dx * dx + dz * dz); }
}

/* translucent body of one barrier */
static void draw_body(int i, float pulse)
{
    Panel p;
    const float M = (float)BAR_UPM, H = (float)BAR_H_UNITS;
    int cells, c;
    make_panel(i, &p);
    if (p.len < 1.0f) return;
    if (B.pub[i].kind == TD5_ROAM_BARRIER_MOUTH) {
        cells = (int)ceilf(p.len / (BAR_CELL_M * M)); if (cells < 1) cells = 1;
        for (c = 0; c < cells; c++) {
            const float u0 = p.len * (float)c / (float)cells, u1 = p.len * (float)(c + 1) / (float)cells;
            pn_rect(TD5_PRESET_TRANSLUCENT_POINT_ZTEST, &p, u0, u1, 0.0f, H,
                    argb((int)(150.0f * pulse), 255, 70, 12), argb((int)(26.0f * pulse), 255, 150, 40));
        }
    } else {
        /* sign board: dark glass between 0.7 m and 3.0 m */
        pn_rect(TD5_PRESET_TRANSLUCENT_POINT_ZTEST, &p, 0.0f, p.len, 0.7f * M, H,
                argb((int)(190.0f * pulse), 6, 8, 30), argb((int)(150.0f * pulse), 10, 14, 48));
    }
}

/* additive glow of one barrier: rails, posts, ground glow, marching chevrons */
static void draw_glow(int i, float pulse, uint32_t now_ms)
{
    const TD5_RoamBarrier *b = &B.pub[i];
    const BarAux *ax = &B.aux[i];
    Panel p;
    const float M = (float)BAR_UPM, H = (float)BAR_H_UNITS;
    const TD5_RenderPreset ad = TD5_PRESET_ADDITIVE_WORLD;
    int n, k;
    float pitch, margin;
    const float sgn = (float)ax->sign;
    make_panel(i, &p);
    if (p.len < 1.0f) return;

    if (b->kind == TD5_ROAM_BARRIER_MOUTH) {
        const float k1 = 0.95f * pulse;
        /* base glow + top rail + end posts + a post every ~4.5 m */
        pn_rect(ad, &p, 0.0f, p.len, 0.0f, 0.38f * M, argb(255, 255 * k1, 140 * k1, 30 * k1), argb(255, 0, 0, 0));
        pn_rect(ad, &p, 0.0f, p.len, H - 0.16f * M, H, argb(255, 255 * k1, 170 * k1, 60 * k1), argb(255, 255 * k1, 170 * k1, 60 * k1));
        pn_rect(ad, &p, 0.0f, 0.28f * M, 0.0f, H, argb(255, 255 * k1, 190 * k1, 90 * k1), argb(255, 255 * k1, 150 * k1, 60 * k1));
        pn_rect(ad, &p, p.len - 0.28f * M, p.len, 0.0f, H, argb(255, 255 * k1, 190 * k1, 90 * k1), argb(255, 255 * k1, 150 * k1, 60 * k1));
        {
            const int np = (int)(p.len / (4.5f * M));
            for (k = 1; k <= np; k++) {
                const float u = p.len * (float)k / (float)(np + 1);
                pn_rect(ad, &p, u - 0.07f * M, u + 0.07f * M, 0.0f, H, argb(255, 150 * k1, 70 * k1, 18 * k1), argb(255, 90 * k1, 40 * k1, 10 * k1));
            }
        }
        /* ground glow, both sides of the base line */
        {
            const float dx = p.bx - p.ax, dz = p.bz - p.az;
            const float l = sqrtf(dx * dx + dz * dz);
            const float g = 1.0f * M, lift = 0.06f * M;
            int sd;
            if (l > 1.0f) {
                const float nx = -dz / l, nz = dx / l;
                for (sd = -1; sd <= 1; sd += 2) {
                    float a0[3], a1[3], a2[3], a3[3];
                    a0[0] = p.ax; a0[1] = p.ay + lift; a0[2] = p.az;
                    a1[0] = p.bx; a1[1] = p.by + lift; a1[2] = p.bz;
                    a2[0] = p.bx + nx * g * (float)sd; a2[1] = p.by + lift; a2[2] = p.bz + nz * g * (float)sd;
                    a3[0] = p.ax + nx * g * (float)sd; a3[1] = p.ay + lift; a3[2] = p.az + nz * g * (float)sd;
                    q_quad(ad, a0, a1, a2, a3, argb(255, 150 * k1, 60 * k1, 12 * k1), argb(255, 150 * k1, 60 * k1, 12 * k1),
                           argb(255, 0, 0, 0), argb(255, 0, 0, 0));
                }
            }
        }
        /* chevrons: > > > pointing along the route, lights marching in the arrow direction */
        pitch = 1.5f * M;
        n = (int)((p.len - 1.0f * M) / pitch);
        if (n > 14) { n = 14; pitch = (p.len - 1.0f * M) / 14.0f; }
        if (n < 1) { n = 1; pitch = 0.0f; }
        margin = 0.5f * (p.len - pitch * (float)(n - 1));
        for (k = 0; k < n; k++) {
            const int kk = sgn > 0.0f ? k : n - 1 - k;                       /* order along the arrow */
            const float u = margin + pitch * (float)k;
            const float ph = (float)(now_ms % 1100u) / 1100.0f - (float)kk * 0.12f;
            float w = ph - floorf(ph);
            float lvl;
            uint32_t col;
            const float hv = 0.85f * M, tip = 0.55f * M * sgn, back = -0.40f * M * sgn, vc = 1.55f * M, th = 0.17f * M;
            w = w < 0.45f ? 1.0f - w / 0.45f : 0.0f;
            lvl = (0.40f + 0.60f * w) * pulse;
            col = argb(255, 255 * lvl, 225 * lvl, 110 * lvl);
            pn_band(ad, &p, u + back, vc + hv, u + tip, vc, th, col);
            pn_band(ad, &p, u + tip, vc, u + back, vc - hv, th, col);
        }
    } else {
        /* turn board: yellow frame, two big chevrons, amber legs */
        const float k1 = 0.95f * pulse, lo = 0.7f * M;
        const uint32_t fr = argb(255, 255 * k1, 205 * k1, 0);
        pn_rect(ad, &p, 0.0f, p.len, lo, lo + 0.14f * M, fr, fr);
        pn_rect(ad, &p, 0.0f, p.len, H - 0.14f * M, H, fr, fr);
        pn_rect(ad, &p, 0.0f, 0.14f * M, lo, H, fr, fr);
        pn_rect(ad, &p, p.len - 0.14f * M, p.len, lo, H, fr, fr);
        pn_rect(ad, &p, 0.30f * M, 0.50f * M, 0.0f, lo, argb(255, 120 * k1, 90 * k1, 0), argb(255, 255 * k1, 190 * k1, 0));
        pn_rect(ad, &p, p.len - 0.50f * M, p.len - 0.30f * M, 0.0f, lo, argb(255, 120 * k1, 90 * k1, 0), argb(255, 255 * k1, 190 * k1, 0));
        {
            const float vc = 0.5f * (lo + H), hv = 0.55f * M, tip = 0.42f * M * sgn, back = -0.38f * M * sgn;
            const float th = 0.17f * M;
            for (k = 0; k < 2; k++) {
                const float u = 0.5f * p.len + (k == 0 ? -0.55f : 0.55f) * M * sgn * -1.0f;
                const float ph = (float)(now_ms % 900u) / 900.0f - (float)(sgn > 0.0f ? k : 1 - k) * 0.25f;
                float w = ph - floorf(ph);
                float lvl;
                uint32_t col;
                w = w < 0.5f ? 1.0f - w / 0.5f : 0.0f;
                lvl = (0.55f + 0.45f * w) * pulse;
                col = argb(255, 255 * lvl, 215 * lvl, 20 * lvl);
                pn_band(ad, &p, u + back, vc + hv, u + tip, vc, th, col);
                pn_band(ad, &p, u + tip, vc, u + back, vc - hv, th, col);
            }
        }
    }
}

#ifndef TD5RE_RELEASE
static void dev_frame(void)
{
    const TD5_RouteResult *r = td5_roam_route_result();
    if (!B.knob_init) {
        const char *e = getenv("TD5RE_ROAM_BARRIERS");
        const char *rn = getenv("TD5RE_ROAM_BARRIERS_ROUTE");
        B.knob_init = 1;
        B.knob_on = (e && e[0] == '1') ? 1 : 0;
        if (rn && rn[0]) snprintf(B.knob_route, sizeof B.knob_route, "%s", rn);
    }
    if (!B.knob_on) return;
    if (B.knob_route[0] && !B.knob_loaded && td5_roam_route_graph_ready()) {
        B.knob_loaded = 1;
        TD5_LOG_I(LOG_TAG, "BARRIERS: dev knob loads route '%s' -> %s", B.knob_route,
                  td5_roam_route_load(B.knob_route) ? "ok" : "FAILED");
        r = td5_roam_route_result();
    }
    if (r->ok && (r->hash != B.built_hash || r->nedge != B.built_nedge || !B.have)) td5_roam_barriers_build(r);
    else if (!r->ok && B.have) td5_roam_barriers_clear();
}
#define DEV_KNOB_ON() (B.knob_on)
#else
#define dev_frame() ((void)0)
#define DEV_KNOB_ON() 0
#endif

void td5_roam_barriers_draw(int view)
{
    DrawItem items[BAR_MAX_DRAW];
    int nitems = 0, i, m;
    const float ex = g_cameraPos[0], ez = g_cameraPos[2];
    const float lim2 = (float)(BAR_DRAW_M * BAR_UPM) * (float)(BAR_DRAW_M * BAR_UPM);
    uint32_t now;
    uint64_t t_us0 = 0;
    float pulse;
    if (view != 0 || !td5_roam_active()) return;
    dev_frame();
    if (!(B.visible || DEV_KNOB_ON()) || B.n <= 0) return;
    bar_rebase_if_needed();
    /* refine turn-board heights from the live ground, once, when the tile is resident */
    for (i = 0; i < B.n; i++) {
        if (B.pub[i].kind != TD5_ROAM_BARRIER_TURN || B.aux[i].refined) continue;
        {
            const int32_t cx = (B.pub[i].x0 + B.pub[i].x1) / 2, cz = (B.pub[i].z0 + B.pub[i].z1) / 2;
            const float dx = (float)cx * (1.0f / 256.0f) - ex, dz = (float)cz * (1.0f / 256.0f) - ez;
            if (dx * dx + dz * dz > 200.0f * 430.0f * 200.0f * 430.0f) continue;
            if (td5_roam_stream_active() && td5_roam_stream_point_missing(cx, cz)) continue;
            {
                const int32_t y = td5_roam_ground(cx, cz, 0, 0, NULL);
                B.pub[i].y0 = B.pub[i].y1 = y;
                B.pl[i].y0 = B.pl[i].y1 = (double)y / 256.0;
                B.aux[i].refined = 1;
            }
        }
    }
    for (i = 0; i < B.n && nitems < BAR_MAX_DRAW; i++) {
        const float mx = 0.5f * (float)(B.pub[i].x0 + B.pub[i].x1) * (1.0f / 256.0f) - ex;
        const float mz = 0.5f * (float)(B.pub[i].z0 + B.pub[i].z1) * (1.0f / 256.0f) - ez;
        const float d2 = mx * mx + mz * mz;
        if (d2 > lim2) continue;
        items[nitems].idx = i; items[nitems].d2 = d2; nitems++;
    }
    if (nitems == 0) return;
    t_us0 = td5_plat_time_us();
    qsort(items, (size_t)nitems, sizeof items[0], cmp_far_first);
    now = td5_plat_time_ms();
    pulse = 0.86f + 0.14f * sinf((float)(now % 2400u) * (6.2831853f / 2400.0f));
    for (m = 0; m < nitems; m++) draw_body(items[m].idx, pulse);
    q_flush(TD5_PRESET_TRANSLUCENT_POINT_ZTEST);
    for (m = 0; m < nitems; m++) draw_glow(items[m].idx, pulse, now);
    q_flush(TD5_PRESET_ADDITIVE_WORLD);
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
#ifndef TD5RE_RELEASE
    {   /* DEV: CPU cost of this pass, logged every 600 drawn frames */
        static double s_sum_ms, s_max_ms;
        static uint32_t s_frames;
        static int s_items_max;
        const double ms = (double)(td5_plat_time_us() - t_us0) / 1000.0;
        s_sum_ms += ms; if (ms > s_max_ms) s_max_ms = ms;
        if (nitems > s_items_max) s_items_max = nitems;
        if (++s_frames % 600u == 0u) {
            TD5_LOG_I(LOG_TAG, "BARRIER_DRAW %u frames: %d barriers set, up to %d drawn at once, CPU avg %.3f ms max %.3f ms",
                      (unsigned)s_frames, B.n, s_items_max, s_sum_ms / 600.0, s_max_ms);
            s_sum_ms = 0.0; s_max_ms = 0.0; s_items_max = 0;
        }
    }
#endif
}

/* ============================================================ census + dev test = */

#ifndef TD5RE_RELEASE

typedef struct { const char *name; int n; double lat[6], lon[6]; } TestRoute;

/* The three census routes (waypoints are lat/lon on La Plata; the F5-A planner joins them). */
static const TestRoute k_routes[] = {
    { "calles",    2, { -34.9190, -34.9290 }, { -57.9490, -57.9370 } },
    { "plaza",     3, { -34.9215, -34.9107, -34.9040 }, { -57.9540, -57.9553, -57.9500 } },
    { "avenida",   2, { -34.9100, -34.9190 }, { -57.9680, -57.9570 } },
};

static void json_barriers(FILE *f, const TD5_RoamRouteResult *r)
{
    int i;
    fprintf(f, "{\"length_m\":%.1f,\"edges\":%d,\"hash\":\"%08X\",\"barriers\":[", r->length_m, r->nedge, (unsigned)r->hash);
    for (i = 0; i < B.n; i++)
        fprintf(f, "%s{\"kind\":%d,\"x0\":%.1f,\"z0\":%.1f,\"x1\":%.1f,\"z1\":%.1f,\"y\":%.1f,\"ck\":\"%llX\"}", i ? "," : "",
                B.pub[i].kind, B.pl[i].x0, B.pl[i].z0, B.pl[i].x1, B.pl[i].z1, 0.5 * (B.pl[i].y0 + B.pl[i].y1) / BAR_UPM,
                (unsigned long long)B.pub[i].cluster_key);
    fprintf(f, "],\"edge_keys\":[");
    for (i = 0; i < r->nedge; i++) fprintf(f, "%s[\"%llX\",%d]", i ? "," : "", (unsigned long long)r->edge[i].key, r->edge[i].dir);
    fprintf(f, "],\"arms\":[");
    for (i = 0; i < B.n; i++)
        fprintf(f, "%s[\"%llX\",%d]", i ? "," : "", (unsigned long long)B.aux[i].arm_key, B.aux[i].arm_end);
#ifndef TD5RE_RELEASE
    fprintf(f, "],\"armdbg\":[");
    for (i = 0; i < B.ndbg; i++)
        fprintf(f, "%s{\"s\":%d,\"x0\":%.1f,\"z0\":%.1f,\"x1\":%.1f,\"z1\":%.1f,\"ck\":\"%llX\",\"ek\":\"%llX\",\"end\":%d}", i ? "," : "",
                B.dbg[i].status, B.dbg[i].x0, B.dbg[i].z0, B.dbg[i].x1, B.dbg[i].z1, B.dbg[i].ck, B.dbg[i].ek, B.dbg[i].end);
#endif
    fprintf(f, "],\"route\":[");
    for (i = 0; i < r->npt; i += 2)
        fprintf(f, "%s[%.1f,%.1f]", i ? "," : "", r->px[i], r->pz[i]);
    fprintf(f, "]}\n");
}

int td5_roam_barriers_dev_test(void)
{
    const char *dir = td5_plat_log_dir();
    char path[600];
    FILE *cf;
    int ri, fails = 0;
    TD5_RoamBarrierStats tot;
    if (td5_env_int("TD5RE_ROAM_BARRIER_TEST", 0, 0, 1) != 1) return 0;
    memset(&tot, 0, sizeof tot);
    snprintf(path, sizeof path, "%sroam_barrier_census.txt", dir ? dir : "");
    cf = fopen(path, "wb");
    td5_roam_route_reset();
    if (!td5_roam_route_graph_ensure(td5_roam_route_place())) {
        TD5_LOG_E(LOG_TAG, "BARRIER_TEST: planner graph failed");
        if (cf) fclose(cf);
        return 1;
    }
    for (ri = 0; ri < (int)(sizeof k_routes / sizeof k_routes[0]); ri++) {
        const TestRoute *tr = &k_routes[ri];
        const TD5_RouteResult *res;
        int w, okw = 1, n;
        TD5_RoamBarrierStats st;
        td5_roam_route_reset();
        for (w = 0; w < tr->n && okw; w++) {
            int32_t px, pz;
            if (!td5_roam_route_latlon_to_place(tr->lat[w], tr->lon[w], &px, &pz)) { okw = 0; break; }
            if (w == 0) okw = td5_roam_route_set_start_at(px, pz);
            else if (w == tr->n - 1) okw = td5_roam_route_set_finish_at(px, pz);
            else okw = td5_roam_route_add_at(px, pz);
        }
        res = td5_roam_route_result();
        if (!okw || !res->ok) {
            TD5_LOG_E(LOG_TAG, "BARRIER_TEST %s: route did not plan (waypoints ok=%d, result ok=%d)", tr->name, okw, res->ok);
            if (cf) fprintf(cf, "%s: ROUTE FAILED\n", tr->name);
            fails++;
            continue;
        }
        n = td5_roam_barriers_build(res);
        td5_roam_barriers_stats(&st);
        {
            char nm[48];
            snprintf(nm, sizeof nm, "f6a_%s", tr->name);
            td5_roam_route_save(nm);
        }
        TD5_LOG_I(LOG_TAG, "BARRIER_TEST %s: route %.0f m, %d edges, hash %08X -> %d barriers (mouth %d, turn %d), clusters %d, passes %d, "
                  "arms total %d route %d blocked %d linked %d open %d, planner-degree open %d, across route %d, pass gaps %d, missing cluster %d, "
                  "no-row arms %d, ring edges %d, one-way edges %d, tiles %d, %.0f ms",
                  tr->name, res->length_m, res->nedge, (unsigned)res->hash, n, st.barriers_mouth, st.barriers_turn, st.clusters_passed, st.passes,
                  st.arms_total, st.arms_route, st.arms_blocked, st.arms_linked, st.arms_open, st.degree_open, st.across_route, st.pass_gaps,
                  st.missing_cluster, st.arm_no_row, st.ring_edges, st.oneway_edges, st.tiles_read, st.build_ms);
        if (cf)
            fprintf(cf, "%s: route %.0f m, %d edges, hash %08X\n"
                        "  barriers %d (mouth %d, turn boards %d)\n"
                        "  clusters passed %d, passes %d, ring edges on the route %d, one-way edges on the route %d\n"
                        "  arms: total %d, taken by the route %d, barriered %d, left linked %d, OPEN %d (planner-degree check: %d), no ribbon row %d\n"
                        "  mouth barriers slid outward along their arm: %d, none fit: %d; turn boards on the outer kerb %d, left out %d\n"
                        "  chevrons on mouth barriers vs the nearest route direction: with %d, sideways %d, against %d\n"
                        "  barriers across the route polyline: %d   pass gaps: %d   cluster records missing: %d\n"
                        "  tiles read %d, build %.0f ms\n",
                    tr->name, res->length_m, res->nedge, (unsigned)res->hash, n, st.barriers_mouth, st.barriers_turn,
                    st.clusters_passed, st.passes, st.ring_edges, st.oneway_edges, st.arms_total, st.arms_route, st.arms_blocked,
                    st.arms_linked, st.arms_open, st.degree_open, st.arm_no_row, st.slid, st.slide_fail, st.boards_alt, st.boards_dropped, st.arrows_with, st.arrows_side, st.arrows_against, st.across_route, st.pass_gaps, st.missing_cluster, st.tiles_read, st.build_ms);
        {
            FILE *jf;
            snprintf(path, sizeof path, "%sroam_barrier_%s.json", dir ? dir : "", tr->name);
            jf = fopen(path, "wb");
            if (jf) { json_barriers(jf, res); fclose(jf); }
        }
        if (st.arms_open || st.degree_open || st.across_route || st.pass_gaps || st.missing_cluster) fails++;
        tot.barriers_mouth += st.barriers_mouth; tot.barriers_turn += st.barriers_turn;
    }
    TD5_LOG_I(LOG_TAG, "BARRIER_TEST: %d route(s) failed a check, %d mouth + %d turn barriers in total", fails, tot.barriers_mouth, tot.barriers_turn);
    if (cf) { fprintf(cf, "failed checks: %d\n", fails); fclose(cf); }
    td5_roam_barriers_clear();
    return 1;
}
#endif
