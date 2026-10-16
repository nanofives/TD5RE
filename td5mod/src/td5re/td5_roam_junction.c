/**
 * td5_roam_junction.c -- GEO FREE ROAM: junction CLUSTERS and their PATCH
 * (PORT-ONLY, F1 task T3).
 *
 * Contract in td5_roam_junction.h. Deviations from GEO_FREE_ROAM_DESIGN.md 1.3,
 * each for a reason:
 *   - Arms are sorted by the angle round the CENTROID of the point 15 m out on
 *     the arm, not by the arm's own bearing. In a multi-node cluster (a divided
 *     avenue meeting a cross street) two carriageway arms have the same bearing;
 *     only their position round the centroid orders them.
 *   - Cut distance = the projection of the fillet TANGENT POINT, which is the
 *     design's "projection of C + r_k" at a 90 degree corner and stays correct
 *     at acute corners, where the tangent point lies r / tan(theta/2) from C.
 *   - Near-parallel neighbours on the CONCAVE side (median between two
 *     carriageways, a straight-through side at ~180 deg) get no corner vertex:
 *     the outline joins the two cut rows directly. The design's bevel point at
 *     max(w) + 2 m from the centroid would put 2 m of paving outside a straight
 *     kerb. On the REFLEX side (the outside of a bend, > 180 deg) the kerb-line
 *     intersection is used as a sharp convex vertex; when it does not exist the
 *     bevel point sits at max(w) from the centroid along the gap bisector.
 *   - 2-arm nodes that turn more than 30 deg (BEND) are clusters too, so a
 *     sharp corner made by two ways gets a fillet instead of a mitre.
 *   - Arms, the 30 m union and the internal-edge test walk through JOIN nodes:
 *     OSM splits carriageways into 2-12 m pieces at junctions, and a cut row,
 *     a union or an absorbed connector must not stop at such a split.
 *   - A corner point past either arm's cut row, or outside the angular sector
 *     between the two cut rows seen from the centroid, is dropped (curved arms,
 *     carriageways that cross just before a junction).
 *   - Fallbacks the design does not have, all counted in the census:
 *     NORMAL -> UNION (F1-T3b: the outline is the union of the paved pieces,
 *     see "union outline" below) -> SIMPLE (no corners, cut rows past every
 *     member node) -> HULL of the cut rows -> SPLIT (one-node clusters). Ear
 *     clipping only runs on an outline that passes outline_simple(). On La
 *     Plata (casco bbox + 300 m) UNION takes all 60 clusters NORMAL cannot
 *     build, so SIMPLE / HULL / SPLIT stay at 0 (_archive/roam/F1/T3b).
 *   - UNION may leave a hole in a patch (an unpaved island, walled): its
 *     outline segments follow the outer ring's (TD5_ROAM_CL_HOLE).
 *   - UNION cannot put a cut row on the outline when two arms overlap for
 *     their whole length (OSM carriageways drawn closer than their widths, a
 *     Y split whose gore lies past half the arm): those clusters are flagged
 *     TD5_ROAM_CL_ROW_COVERED, both rows stay side by side near the junction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_config.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_roam_cache.h"
#include "td5_roam_world.h"
#include "td5_roam_junction.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"
#define ROAM_PI 3.14159265358979323846

/* Non-cut outline points closer than this to a neighbour are dropped (T7 wants
 * no boundary edge shorter than 1 cm = 4.3 units; this keeps them >= 20). */
#define ROAM_OL_MIN_SEG 20.0

/* ------------------------------------------------------------ growth -------- */

static int grow(void **arr, int *cap, int need, size_t elt)
{
    void *nb;
    int nc;
    if (need <= *cap) return 1;
    nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    nb = realloc(*arr, (size_t)nc * elt);
    if (!nb) return 0;
    *arr = nb;
    *cap = nc;
    return 1;
}

typedef struct {
    int cap_cluster, cap_cnode, cap_arm, cap_pv, cap_tri, cap_ol;
} RoamCaps;

/* ------------------------------------------------------------ small math ---- */

static double cross2(double ax, double az, double bx, double bz) { return ax * bz - az * bx; }

static int is_cand(const TD5_RoamNode *n)
{
    return n->kind == TD5_ROAM_NODE_JUNCTION || n->kind == TD5_ROAM_NODE_TAPER ||
           n->kind == TD5_ROAM_NODE_BEND;
}

static double node_max_hw(const TD5_RoamGraph *g, const TD5_RoamNode *n)
{
    double m = 0.0;
    int k;
    for (k = 0; k < n->nend; k++) {
        const double hw = g->edge[g->node_end[n->end_first + k].edge].half_w;
        if (hw > m) m = hw;
    }
    return m;
}

double td5_roam_patch_vert_y(const TD5_RoamPatchVert *v, double h_c, const double *arm_y)
{
    double y = 0.0;
    int k;
    for (k = 0; k < TD5_ROAM_HREF_TERMS; k++) {
        if (v->ref[k] == TD5_ROAM_HREF_NONE) continue;
        y += v->w[k] * (v->ref[k] == TD5_ROAM_HREF_CENTRE ? h_c : arm_y[v->ref[k]]);
    }
    return y;
}

int td5_roam_junction_find(const TD5_RoamGraph *g, int64_t key)
{
    int lo = 0, hi = g ? g->ncluster - 1 : -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const int64_t k = g->cluster[mid].key;
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* ------------------------------------------------------------ recipes ------- */

static void pv_init(TD5_RoamPatchVert *v, double x, double z, int kind)
{
    int k;
    memset(v, 0, sizeof *v);
    v->x = x;
    v->z = z;
    v->kind = kind;
    for (k = 0; k < TD5_ROAM_HREF_TERMS; k++) v->ref[k] = TD5_ROAM_HREF_NONE;
}

/* Midpoint recipe: 0.5 * a + 0.5 * b, terms merged by ref. Returns 0 when more
 * than TD5_ROAM_HREF_TERMS distinct refs appear (the smallest are dropped and
 * the rest renormalised). */
static int pv_mid(TD5_RoamPatchVert *m, const TD5_RoamPatchVert *a, const TD5_RoamPatchVert *b)
{
    int ref[2 * TD5_ROAM_HREF_TERMS];
    double w[2 * TD5_ROAM_HREF_TERMS];
    int n = 0, k, q, ok = 1;
    const TD5_RoamPatchVert *src[2] = { a, b };
    pv_init(m, 0.5 * (a->x + b->x), 0.5 * (a->z + b->z), TD5_ROAM_PV_SPLIT);
    for (q = 0; q < 2; q++)
        for (k = 0; k < TD5_ROAM_HREF_TERMS; k++) {
            int f;
            if (src[q]->ref[k] == TD5_ROAM_HREF_NONE) continue;
            for (f = 0; f < n; f++) if (ref[f] == src[q]->ref[k]) break;
            if (f == n) { ref[n] = src[q]->ref[k]; w[n] = 0.0; n++; }
            w[f] += 0.5 * src[q]->w[k];
        }
    /* Sort by ref so the recipe is canonical. */
    for (k = 1; k < n; k++)
        for (q = k; q > 0 && ref[q] < ref[q - 1]; q--) {
            int tr = ref[q]; double tw = w[q];
            ref[q] = ref[q - 1]; w[q] = w[q - 1];
            ref[q - 1] = tr; w[q - 1] = tw;
        }
    if (n > TD5_ROAM_HREF_TERMS) {
        double sum = 0.0;
        ok = 0;
        while (n > TD5_ROAM_HREF_TERMS) {          /* drop the smallest weight */
            int lo = 0;
            for (k = 1; k < n; k++) if (w[k] < w[lo]) lo = k;
            for (k = lo; k + 1 < n; k++) { ref[k] = ref[k + 1]; w[k] = w[k + 1]; }
            n--;
        }
        for (k = 0; k < n; k++) sum += w[k];
        for (k = 0; k < n; k++) w[k] /= sum;
    }
    for (k = 0; k < n; k++) { m->ref[k] = ref[k]; m->w[k] = w[k]; }
    return ok;
}

/* ------------------------------------------------------------ clustering ---- */

typedef struct { int a, b, kind; } RoamPair;   /* a < b node indices */
#define PAIR_EDGE 1
#define PAIR_DISC 2

static int pair_cmp(const void *x, const void *y)
{
    const RoamPair *p = (const RoamPair *)x, *q = (const RoamPair *)y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    return (p->b > q->b) - (p->b < q->b);
}

typedef struct { double x; int n; } RoamXs;

static int xs_cmp(const void *x, const void *y)
{
    const RoamXs *p = (const RoamXs *)x, *q = (const RoamXs *)y;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    return (p->n > q->n) - (p->n < q->n);
}

static int uf_find(int *uf, int i)
{
    while (uf[i] != i) { uf[i] = uf[uf[i]]; i = uf[i]; }
    return i;
}

/* ------------------------------------------------------------ arms ---------- */

/* An arm walks on through JOIN nodes: OSM often splits a carriageway into
 * 2-12 m pieces right at a junction (La Plata ways 1449919888..894 at Av 44 x
 * Calle 8), and a cut row must not be limited by such a piece. The chain stops
 * at the first non-JOIN node, after ROAM_CHAIN_MAX pieces or past
 * ROAM_CHAIN_LEN. */
#define ROAM_CHAIN_MAX 16
#define ROAM_CHAIN_LEN (200.0 * TD5_ROAM_UPM)

typedef struct {
    TD5_RoamArm a;
    uint64_t    ekey;      /* key of the FIRST edge (sort tie-break)       */
    double      len;       /* chain length                                 */
    double      lim;       /* cut limit = chain length / 2                 */
    double      kx, kz;    /* the arm node position                        */
    int         klass;
    int         nchain;
    int         ce[ROAM_CHAIN_MAX];       /* edge of chain piece k         */
    int         cq[ROAM_CHAIN_MAX];       /* the end it is entered from    */
    double      ccum[ROAM_CHAIN_MAX + 1]; /* chain distance at piece start */
} ArmTmp;

/* The chain point at distance d from the arm node. */
static void chain_point(const TD5_RoamGraph *g, const ArmTmp *a, double d, double *x, double *z)
{
    int k = 0;
    while (k + 1 < a->nchain && d >= a->ccum[k + 1]) k++;
    {
        const TD5_RoamEdge *e = &g->edge[a->ce[k]];
        double loc = d - a->ccum[k];
        if (loc < 0.0) loc = 0.0;
        if (loc > e->length) loc = e->length;
        td5_roam_edge_point(g, e, a->cq[k] == 0 ? loc : e->length - loc, x, z, NULL, NULL);
    }
}

static int arm_cmp(const void *x, const void *y)
{
    const ArmTmp *p = (const ArmTmp *)x, *q = (const ArmTmp *)y;
    if (p->a.ang != q->a.ang) return p->a.ang < q->a.ang ? -1 : 1;
    if (p->ekey != q->ekey) return p->ekey < q->ekey ? -1 : 1;
    return (p->a.end > q->a.end) - (p->a.end < q->a.end);
}

/* The corner between arm i's LEFT kerb and arm j's RIGHT kerb. */
#define CORNER_DIRECT 0
#define CORNER_FILLET 1
#define CORNER_REFLEX 2
#define CORNER_BEVEL  3

typedef struct {
    int    type;
    double cx, cz;          /* kerb-line intersection (FILLET, REFLEX)      */
    double theta;           /* block angle between d_i and d_j (FILLET)     */
    double r;               /* fillet radius actually used                  */
    double tix, tiz, tjx, tjz;   /* tangent points                          */
    double ox, oz;          /* arc centre                                   */
    double bx, bz;          /* bevel point                                  */
    int    swept_fail;
    double margin;          /* corridor - need, world units                 */
} Corner;

static double swept_need(double r, double w, double lf)
{
    if (r < 0.0) r = 0.0;
    return sqrt((r + w) * (r + w) + lf * lf) - r;
}

/* ------------------------------------------------------------ triangulation - */

typedef struct { int v[3]; } LTri;
typedef struct { int v[2]; int kind; int arm; } LSeg;

static int fan_valid(const TD5_RoamPatchVert *pv, int nout, double px, double pz)
{
    double sum = 0.0;
    int k;
    for (k = 0; k < nout; k++) {
        const TD5_RoamPatchVert *a = &pv[k], *b = &pv[(k + 1) % nout];
        const double ax = a->x - px, az = a->z - pz, bx = b->x - px, bz = b->z - pz;
        const double cr = cross2(ax, az, bx, bz);
        if (cr <= 1.0) return 0;                      /* positive area, > 0.5 u^2 */
        sum += atan2(cr, ax * bx + az * bz);
    }
    return fabs(sum - 2.0 * ROAM_PI) < 1e-6;
}

static int pt_in_tri(double px, double pz, const TD5_RoamPatchVert *a,
                     const TD5_RoamPatchVert *b, const TD5_RoamPatchVert *c)
{
    const double d1 = cross2(b->x - a->x, b->z - a->z, px - a->x, pz - a->z);
    const double d2 = cross2(c->x - b->x, c->z - b->z, px - b->x, pz - b->z);
    const double d3 = cross2(a->x - c->x, a->z - c->z, px - c->x, pz - c->z);
    return d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0;
}

/* 1 when no two non-adjacent outline segments cross or touch. Ear clipping
 * happily triangulates a self-crossing outline into overlapping triangles, so
 * this gate runs first. O(n^2); outlines hold < 200 vertices. */
static int outline_simple(const TD5_RoamPatchVert *pv, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        const TD5_RoamPatchVert *a = &pv[i], *b = &pv[(i + 1) % n];
        for (j = i + 1; j < n; j++) {
            const TD5_RoamPatchVert *c = &pv[j], *d = &pv[(j + 1) % n];
            double d1, d2, d3, d4;
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;   /* neighbours */
            d1 = cross2(b->x - a->x, b->z - a->z, c->x - a->x, c->z - a->z);
            d2 = cross2(b->x - a->x, b->z - a->z, d->x - a->x, d->z - a->z);
            d3 = cross2(d->x - c->x, d->z - c->z, a->x - c->x, a->z - c->z);
            d4 = cross2(d->x - c->x, d->z - c->z, b->x - c->x, b->z - c->z);
            if (((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0)) &&
                d1 != 0.0 && d2 != 0.0 && d3 != 0.0 && d4 != 0.0)
                return 0;
        }
    }
    return 1;
}

/* Ear clipping over the outline (CCW). Writes ntri = nout - 2 triangles into
 * out. Returns 1 on success. */
static int ear_clip(const TD5_RoamPatchVert *pv, int nout, LTri *out, int *ntri)
{
    int *idx = (int *)malloc((size_t)nout * sizeof(int));
    int n = nout, k, guard = 0;
    double area = 0.0;
    *ntri = 0;
    if (!idx) return 0;
    for (k = 0; k < nout; k++) {
        const TD5_RoamPatchVert *a = &pv[k], *b = &pv[(k + 1) % nout];
        area += cross2(a->x, a->z, b->x, b->z);
        idx[k] = k;
    }
    if (area <= 0.0) { free(idx); return 0; }
    while (n > 3 && guard++ < 4 * nout * nout) {
        int found = 0;
        for (k = 0; k < n; k++) {
            const int ip = idx[(k + n - 1) % n], ic = idx[k], in = idx[(k + 1) % n];
            const TD5_RoamPatchVert *a = &pv[ip], *b = &pv[ic], *c = &pv[in];
            int q, inside = 0;
            if (cross2(b->x - a->x, b->z - a->z, c->x - b->x, c->z - b->z) <= 1.0) continue;
            for (q = 0; q < n && !inside; q++) {
                const int iq = idx[q];
                const TD5_RoamPatchVert *p = &pv[iq];
                if (iq == ip || iq == ic || iq == in) continue;
                if ((p->x == a->x && p->z == a->z) || (p->x == b->x && p->z == b->z) ||
                    (p->x == c->x && p->z == c->z)) continue;
                if (pt_in_tri(p->x, p->z, a, b, c)) inside = 1;
            }
            if (inside) continue;
            out[*ntri].v[0] = ip;
            out[*ntri].v[1] = ic;
            out[*ntri].v[2] = in;
            (*ntri)++;
            for (q = k; q + 1 < n; q++) idx[q] = idx[q + 1];
            n--;
            found = 1;
            break;
        }
        if (!found) { free(idx); return 0; }
    }
    if (n == 3) {
        out[*ntri].v[0] = idx[0];
        out[*ntri].v[1] = idx[1];
        out[*ntri].v[2] = idx[2];
        (*ntri)++;
    }
    free(idx);
    return n == 3;
}

/* ------------------------------------------------------------ one cluster --- */

typedef struct {
    TD5_RoamPatchVert *pv; int npv, cap_pv;
    LTri *tri; int ntri, cap_tri;
    LSeg *seg; int nseg, cap_seg;
} Patch;

static int patch_add_v(Patch *p, const TD5_RoamPatchVert *v)
{
    if (!grow((void **)&p->pv, &p->cap_pv, p->npv + 1, sizeof *p->pv)) return -1;
    p->pv[p->npv] = *v;
    return p->npv++;
}

static int patch_add_tri(Patch *p, int a, int b, int c)
{
    if (!grow((void **)&p->tri, &p->cap_tri, p->ntri + 1, sizeof *p->tri)) return 0;
    p->tri[p->ntri].v[0] = a;
    p->tri[p->ntri].v[1] = b;
    p->tri[p->ntri].v[2] = c;
    p->ntri++;
    return 1;
}

/* [F1-T9] Distance from (x, z) to the triangulated patch (0 inside). */
static double patch_dist(const Patch *p, double x, double z)
{
    double best = 1e300;
    int t, k;
    for (t = 0; t < p->ntri; t++) {
        const TD5_RoamPatchVert *v[3];
        double s[3];
        int neg = 0, pos = 0;
        for (k = 0; k < 3; k++) v[k] = &p->pv[p->tri[t].v[k]];
        for (k = 0; k < 3; k++) {
            const TD5_RoamPatchVert *a = v[k], *b = v[(k + 1) % 3];
            s[k] = cross2(b->x - a->x, b->z - a->z, x - a->x, z - a->z);
            if (s[k] < 0.0) neg = 1;
            if (s[k] > 0.0) pos = 1;
        }
        if (!(neg && pos)) return 0.0;
        for (k = 0; k < 3; k++) {
            const TD5_RoamPatchVert *a = v[k], *b = v[(k + 1) % 3];
            const double dx = b->x - a->x, dz = b->z - a->z, l2 = dx * dx + dz * dz;
            double u = l2 > 1e-12 ? ((x - a->x) * dx + (z - a->z) * dz) / l2 : 0.0, d;
            if (u < 0.0) u = 0.0; else if (u > 1.0) u = 1.0;
            d = hypot(x - a->x - dx * u, z - a->z - dz * u);
            if (d < best) best = d;
        }
    }
    return best;
}

/* [F1-T9] 1 when every edge absorbed into cluster ci (INTERNAL ends) has its
 * centreline on the patch, within tol, sampled every ROAM_ABSORB_STEP. */
#define ROAM_ABSORB_TOL  (1.0 * TD5_ROAM_UPM)
#define ROAM_ABSORB_STEP (2.0 * TD5_ROAM_UPM)
static int absorbed_on_patch(const TD5_RoamGraph *g, int ci, const Patch *p, double tol)
{
    int i, q;
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        if (e->end[0].kind != TD5_ROAM_END_INTERNAL || e->end[0].cluster != ci) continue;
        for (q = 0; q + 1 < e->npt; q++) {
            const TD5_RoamPt *a = &g->pt[e->pt_first + q], *b = &g->pt[e->pt_first + q + 1];
            const double len = hypot(b->x - a->x, b->z - a->z);
            const int n = len > ROAM_ABSORB_STEP ? (int)ceil(len / ROAM_ABSORB_STEP) : 1;
            int s;
            for (s = 0; s <= n; s++) {
                const double f = (double)s / (double)n;
                if (patch_dist(p, a->x + (b->x - a->x) * f, a->z + (b->z - a->z) * f) > tol) return 0;
            }
        }
    }
    return 1;
}

static int patch_add_seg(Patch *p, int a, int b, int kind, int arm)
{
    if (!grow((void **)&p->seg, &p->cap_seg, p->nseg + 1, sizeof *p->seg)) return 0;
    p->seg[p->nseg].v[0] = a;
    p->seg[p->nseg].v[1] = b;
    p->seg[p->nseg].kind = kind;
    p->seg[p->nseg].arm = arm;
    p->nseg++;
    return 1;
}

static double vdist(const TD5_RoamPatchVert *a, const TD5_RoamPatchVert *b)
{
    const double dx = a->x - b->x, dz = a->z - b->z;
    return sqrt(dx * dx + dz * dz);
}

/* Conforming longest-edge bisection until every triangle edge is <= emax.
 * Returns the number of splits, or -1 on out of memory. */
static int patch_split(Patch *p, double emax, int *overflow)
{
    int splits = 0, guard = 0;
    for (;;) {
        int t, found = -1, eu = 0, ev = 0;
        double best = emax;
        for (t = 0; t < p->ntri && found < 0; t++) {
            int k;
            for (k = 0; k < 3; k++) {
                const int u = p->tri[t].v[k], v = p->tri[t].v[(k + 1) % 3];
                const double d = vdist(&p->pv[u], &p->pv[v]);
                if (d > best) { best = d; found = t; eu = u; ev = v; }
            }
        }
        if (found < 0) break;
        if (++guard > 20000) break;
        {
            TD5_RoamPatchVert m;
            int mi, k, t2 = -1, s;
            if (!pv_mid(&m, &p->pv[eu], &p->pv[ev])) (*overflow)++;
            mi = patch_add_v(p, &m);
            if (mi < 0) return -1;
            /* The neighbour holding the reverse edge (v, u). */
            for (t = 0; t < p->ntri && t2 < 0; t++) {
                if (t == found) continue;
                for (k = 0; k < 3; k++)
                    if (p->tri[t].v[k] == ev && p->tri[t].v[(k + 1) % 3] == eu) { t2 = t; break; }
            }
            /* Split found = (u, v, w) -> (u, m, w) + (m, v, w). */
            for (k = 0; k < 3; k++)
                if (p->tri[found].v[k] == eu && p->tri[found].v[(k + 1) % 3] == ev) break;
            {
                const int w = p->tri[found].v[(k + 2) % 3];
                p->tri[found].v[0] = eu; p->tri[found].v[1] = mi; p->tri[found].v[2] = w;
                if (!patch_add_tri(p, mi, ev, w)) return -1;
            }
            if (t2 >= 0) {
                for (k = 0; k < 3; k++)
                    if (p->tri[t2].v[k] == ev && p->tri[t2].v[(k + 1) % 3] == eu) break;
                {
                    const int x = p->tri[t2].v[(k + 2) % 3];
                    p->tri[t2].v[0] = ev; p->tri[t2].v[1] = mi; p->tri[t2].v[2] = x;
                    if (!patch_add_tri(p, mi, eu, x)) return -1;
                }
            } else {
                /* A boundary edge: split the outline segment too. */
                for (s = 0; s < p->nseg; s++)
                    if (p->seg[s].v[0] == eu && p->seg[s].v[1] == ev) {
                        const int kind = p->seg[s].kind, arm = p->seg[s].arm;
                        p->seg[s].v[1] = mi;
                        if (!patch_add_seg(p, mi, ev, kind, arm)) return -1;
                        break;
                    }
            }
            splits++;
        }
    }
    return splits;
}

/* A cut row's lane vertices are collinear only up to rounding (place-frame
 * coordinates are ~5e6, so the cross product carries an error of a few units^2).
 * 50 units^2 over a 1500-unit lane is 0.03 units off the line. */
#define ROAM_HULL_COLLINEAR 50.0

/* Hull order: x, then z, then index. */
static int hull_less(const TD5_RoamPatchVert *pv, int a, int b)
{
    if (pv[a].x != pv[b].x) return pv[a].x < pv[b].x;
    if (pv[a].z != pv[b].z) return pv[a].z < pv[b].z;
    return a < b;
}

/* Replace the patch by the convex hull of its CUT vertices (collinear ones kept,
 * so a cut row on the hull keeps every lane vertex), fanned from the hull's
 * centroid. Returns 1 on success. */
static int patch_hull(Patch *p, int *nout)
{
    int *ix = NULL, *h = NULL, n = 0, k, m = 0, t;
    Patch q;
    memset(&q, 0, sizeof q);
    ix = (int *)malloc((size_t)(*nout + 1) * sizeof(int));
    h = (int *)malloc((size_t)(2 * *nout + 2) * sizeof(int));
    if (!ix || !h) { free(ix); free(h); return 0; }
    for (k = 0; k < *nout; k++) if (p->pv[k].kind == TD5_ROAM_PV_CUT) ix[n++] = k;
    if (n < 3) { free(ix); free(h); return 0; }
    for (k = 1; k < n; k++)                         /* insertion sort, n is small */
        for (t = k; t > 0 && hull_less(p->pv, ix[t], ix[t - 1]); t--) {
            const int tmp = ix[t]; ix[t] = ix[t - 1]; ix[t - 1] = tmp;
        }
#define HCROSS(o, a, b) cross2(p->pv[a].x - p->pv[o].x, p->pv[a].z - p->pv[o].z, \
                               p->pv[b].x - p->pv[o].x, p->pv[b].z - p->pv[o].z)
    for (k = 0; k < n; k++) {                       /* lower hull */
        while (m >= 2 && HCROSS(h[m - 2], h[m - 1], ix[k]) < -ROAM_HULL_COLLINEAR) m--;
        h[m++] = ix[k];
    }
    for (k = n - 2, t = m + 1; k >= 0; k--) {       /* upper hull */
        while (m >= t && HCROSS(h[m - 2], h[m - 1], ix[k]) < -ROAM_HULL_COLLINEAR) m--;
        h[m++] = ix[k];
    }
#undef HCROSS
    m--;                                            /* last == first */
    /* Drop repeats (collinear points on the vertical extremes). */
    {
        int w = 0;
        for (k = 0; k < m; k++) {
            int z2, dup = 0;
            for (z2 = 0; z2 < w; z2++) if (h[z2] == h[k]) dup = 1;
            if (!dup) h[w++] = h[k];
        }
        m = w;
    }
    if (m < 3) { free(ix); free(h); return 0; }
    {
        double sx = 0.0, sz = 0.0;
        TD5_RoamPatchVert cv;
        int ci;
        for (k = 0; k < m; k++) {
            if (patch_add_v(&q, &p->pv[h[k]]) < 0) goto fail;
            sx += p->pv[h[k]].x;
            sz += p->pv[h[k]].z;
        }
        for (k = 0; k < m; k++) {
            const int a = h[k], b = h[(k + 1) % m];
            int s, kind = TD5_ROAM_OL_KERB, arm = -1;
            for (s = 0; s < p->nseg; s++)
                if (p->seg[s].v[0] == a && p->seg[s].v[1] == b && p->seg[s].kind == TD5_ROAM_OL_CUT) {
                    kind = TD5_ROAM_OL_CUT;
                    arm = p->seg[s].arm;
                    break;
                }
            if (!patch_add_seg(&q, k, (k + 1) % m, kind, arm)) goto fail;
        }
        pv_init(&cv, sx / (double)m, sz / (double)m, TD5_ROAM_PV_CENTRE);
        cv.ref[0] = TD5_ROAM_HREF_CENTRE;
        cv.w[0] = 1.0;
        ci = patch_add_v(&q, &cv);
        if (ci < 0) goto fail;
        for (k = 0; k < m; k++)
            if (!patch_add_tri(&q, ci, k, (k + 1) % m)) goto fail;
    }
    free(p->pv); free(p->tri); free(p->seg);
    *p = q;
    *nout = m;
    free(ix);
    free(h);
    return 1;
fail:
    free(q.pv); free(q.tri); free(q.seg);
    free(ix);
    free(h);
    return 0;
}

/* ------------------------------------------------------------ union outline - */

/* MODE_UNION (F1-T3b): the patch outline is the boolean UNION of the paved
 * pieces of the cluster, not a star of kerb lines round the centroid. Used for
 * the clusters whose NORMAL outline is invalid: diagonal crossings whose arms
 * leave from 3-6 member nodes (the kerb-line corners of arms from different
 * nodes land inside the patch) and sharp Y splits (two arms inside each other's
 * carriageway for 10-30 m). Pieces, all convex, local frame (minus the centroid):
 *   - each arm's STUB: one quad per chain segment from its node to its cut, the
 *     last quad ending on the arm's cut row (its lane vertices, tagged), plus a
 *     bevel triangle on the outside of every polyline turn;
 *   - every INTERNAL edge of the cluster, the same way, node to node;
 *   - at every node, a bevel triangle across each gap of more than 180 deg
 *     between consecutive ends (the outside of a bend).
 * Cuts start at the half width and are pushed out in U_STEP steps while a cut
 * row is covered by another piece, or another piece reaches into the arm's
 * ribbon (the first U_AHEAD beyond the cut). That opens a Y split's gore: both
 * branches are cut past the point where their carriageways separate, and the
 * gore nose is the outline vertex between the two rows. Two carriageways that
 * overlap for their whole length (OSM draws them 3-4 m apart) never separate:
 * an arm still pushed at its limit becomes a PARTNER of the arms pushing it,
 * partners ignore each other, all cuts restart, and a partner then takes the
 * cut of the partner covering its row (rows side by side, neither ribbon under
 * the other's stub). The cluster is flagged ROW_COVERED.
 * The union boundary is found by splitting every piece edge at every crossing
 * and keeping the sub-segments no other piece covers. Every reflex outline
 * vertex (a block corner) gets a kerb fillet; a fillet that needs more kerb
 * than the arm gives pushes that arm's cut out. Enclosed unpaved pockets
 * larger than U_HOLE_MIN stay holes (traffic islands, walled). */

#define U_MAXV      (4 + TD5_ROAM_ROW_MAX)
/* Edges are split at every crossing, so a sub-segment lies wholly on one side
 * of every other edge: the side test needs no slack beyond rounding (local
 * coordinates are < 1e5 units, double noise ~1e-11). U_EPS only catches exact
 * collinear overlaps (shared seams built from the same numbers). */
#define U_EPS       1e-4                       /* on-boundary, world units     */
#define U_WELD      1e-3                       /* endpoint weld distance       */
#define U_COVER     2.0                        /* "inside" for cut decisions   */
#define U_STEP      (2.0 * TD5_ROAM_UPM)       /* cut push step                */
#define U_MAXIT     64                         /* push steps per restart       */
#define U_AHEAD     (30.0 * TD5_ROAM_UPM)      /* ribbon checked beyond a cut  */
#define U_HOLE_MIN  (6.0 * TD5_ROAM_UPM * TD5_ROAM_UPM)   /* 6 m2              */
#define U_GORE_RAD  (30.0 * ROAM_PI / 180.0)   /* sharper block corner: no fillet */
#define U_STRAIGHT  (10.0 * ROAM_PI / 180.0)   /* flatter turn: no fillet      */
#define U_LOOP_MAX  32
#define U_POLY_MAX  512
#define U_TAG(arm, lane)  (1 + (arm) * 256 + (lane))
#define U_TAG_ARM(t)      (((t) - 1) / 256)
#define U_TAG_LANE(t)     (((t) - 1) % 256)

typedef struct {
    int    n;
    double x[U_MAXV], z[U_MAXV];   /* CCW, local frame                        */
    int    tag[U_MAXV];            /* cut-row vertex: U_TAG(arm, lane), else 0 */
    double bx0, bz0, bx1, bz1;     /* bbox                                     */
    double hw;
    int    klass;
    int    arm;                    /* local arm of a stub / ahead piece, else -1 */
} UPrim;

typedef struct { UPrim *p; int n, cap; } UPrims;

typedef struct { double x, z; int tag; int prim; } ULV;   /* prim: of the segment leaving it */

typedef struct {
    ULV   *v[U_LOOP_MAX];
    int    n[U_LOOP_MAX], cap[U_LOOP_MAX];
    double area[U_LOOP_MAX];
    int    nloop;
    int    pruned;              /* dangling sliver segments dropped           */
} ULoops;

static void uloops_free(ULoops *U)
{
    int k;
    for (k = 0; k < U->nloop; k++) free(U->v[k]);
    memset(U, 0, sizeof *U);
}

static int up_add(UPrims *L, const double *x, const double *z, const int *tag, int n,
                  double hw, int klass, int arm)
{
    UPrim *q;
    double area = 0.0;
    int k;
    if (n < 3 || n > U_MAXV) return 1;
    for (k = 0; k < n; k++) area += cross2(x[k], z[k], x[(k + 1) % n], z[(k + 1) % n]);
    if (fabs(area) < 2.0) return 1;                     /* degenerate */
    if (!grow((void **)&L->p, &L->cap, L->n + 1, sizeof *L->p)) return 0;
    q = &L->p[L->n++];
    q->n = n;
    q->hw = hw;
    q->klass = klass;
    q->arm = arm;
    for (k = 0; k < n; k++) {
        const int s = area > 0.0 ? k : n - 1 - k;
        q->x[k] = x[s];
        q->z[k] = z[s];
        q->tag[k] = tag ? tag[s] : 0;
        if (k == 0 || q->x[k] < q->bx0) q->bx0 = q->x[k];
        if (k == 0 || q->z[k] < q->bz0) q->bz0 = q->z[k];
        if (k == 0 || q->x[k] > q->bx1) q->bx1 = q->x[k];
        if (k == 0 || q->z[k] > q->bz1) q->bz1 = q->z[k];
    }
    return 1;
}

/* A polyline of half width hw as convex pieces: one quad per segment and a
 * bevel triangle on the outside of each turn. With nrow > 0 the last quad ends
 * on (rx, rz), right kerb -> left kerb, tagged U_TAG(tagarm, lane). */
static int up_strip(UPrims *L, const double *px, const double *pz, int n, double hw, int klass,
                    int arm, const double *rx, const double *rz, int nrow, int tagarm)
{
    double pux = 0.0, puz = 0.0;
    int have = 0, j, last = -1;
    for (j = 0; j + 1 < n; j++)
        if (hypot(px[j + 1] - px[j], pz[j + 1] - pz[j]) > 1e-6) last = j;
    for (j = 0; j + 1 < n; j++) {
        const double dx = px[j + 1] - px[j], dz = pz[j + 1] - pz[j];
        const double len = sqrt(dx * dx + dz * dz);
        double ux, uz, nx, nz, X[U_MAXV], Z[U_MAXV];
        int T[U_MAXV], m = 0, r;
        if (len <= 1e-6) continue;
        ux = dx / len;
        uz = dz / len;
        nx = -uz;
        nz = ux;
        if (have) {
            const double cr = cross2(pux, puz, ux, uz);
            if (cr != 0.0) {                     /* up_add drops a degenerate one */
                /* Outer side: the right kerb on a left turn, the left on a right. */
                const double sg = cr > 0.0 ? -1.0 : 1.0;
                double bx[3], bz[3];
                bx[0] = px[j];                    bz[0] = pz[j];
                bx[1] = px[j] + sg * -puz * hw;   bz[1] = pz[j] + sg * pux * hw;
                bx[2] = px[j] + sg * nx * hw;     bz[2] = pz[j] + sg * nz * hw;
                if (!up_add(L, bx, bz, NULL, 3, hw, klass, arm)) return 0;
            }
        }
        X[m] = px[j] - nx * hw; Z[m] = pz[j] - nz * hw; T[m++] = 0;
        if (j == last && nrow > 0) {
            for (r = 0; r < nrow; r++) { X[m] = rx[r]; Z[m] = rz[r]; T[m++] = U_TAG(tagarm, r); }
        } else {
            X[m] = px[j + 1] - nx * hw; Z[m] = pz[j + 1] - nz * hw; T[m++] = 0;
            X[m] = px[j + 1] + nx * hw; Z[m] = pz[j + 1] + nz * hw; T[m++] = 0;
        }
        X[m] = px[j] + nx * hw; Z[m] = pz[j] + nz * hw; T[m++] = 0;
        if (!up_add(L, X, Z, T, m, hw, klass, arm)) return 0;
        pux = ux;
        puz = uz;
        have = 1;
    }
    return 1;
}

/* The chain points of an arm between distances d0 < d1 from its node, local
 * frame: the point at d0, every polyline vertex strictly between, the point at
 * d1. Returns the count. */
static int arm_poly(const TD5_RoamGraph *g, const ArmTmp *am, double d0, double d1,
                    double ox, double oz, double *px, double *pz, int cap)
{
    int n = 0, k, q;
    double x, z;
    chain_point(g, am, d0, &x, &z);
    px[n] = x - ox; pz[n] = z - oz; n++;
    for (k = 0; k < am->nchain && am->ccum[k] < d1; k++) {
        const TD5_RoamEdge *e = &g->edge[am->ce[k]];
        const TD5_RoamPt *p = &g->pt[e->pt_first];
        double acc = 0.0;
        for (q = 1; q < e->npt; q++) {
            const int i0 = am->cq[k] == 0 ? q - 1 : e->npt - q;
            const int i1 = am->cq[k] == 0 ? q : e->npt - 1 - q;
            double d;
            acc += hypot(p[i1].x - p[i0].x, p[i1].z - p[i0].z);
            d = am->ccum[k] + acc;
            if (d >= d1 - 1.0) break;
            if (d > d0 + 1.0 && n < cap - 1) { px[n] = p[i1].x - ox; pz[n] = p[i1].z - oz; n++; }
        }
    }
    chain_point(g, am, d1, &x, &z);
    px[n] = x - ox; pz[n] = z - oz; n++;
    return n;
}

/* The cut row of an arm at distance t, in the arm's OUTWARD sense (right kerb
 * first). Same chain piece, s and row function as the publish step, so the
 * points are the published ones bit for bit. */
static int arm_cut_row(const TD5_RoamGraph *g, const ArmTmp *am, double t, TD5_RoamPt *row)
{
    TD5_RoamPt tmp[TD5_ROAM_ROW_MAX];
    const TD5_RoamEdge *e;
    double loc, s;
    int kk = 0, r, lanes;
    while (kk + 1 < am->nchain && t >= am->ccum[kk + 1]) kk++;
    e = &g->edge[am->ce[kk]];
    loc = t - am->ccum[kk];
    s = am->cq[kk] == 0 ? loc : ((am->nchain == 1 && t == am->lim) ? am->lim : e->length - loc);
    td5_roam_edge_row(g, e, s, tmp, NULL, NULL);
    lanes = e->lanes;
    for (r = 0; r <= lanes; r++) row[r] = tmp[am->cq[kk] == 0 ? r : lanes - r];
    return lanes + 1;
}

/* Outward unit direction of an edge end's first segment. */
static void u_end_dir(const TD5_RoamGraph *g, const TD5_RoamEdge *e, int end, double *ux, double *uz)
{
    const TD5_RoamPt *p = &g->pt[e->pt_first];
    double dx, dz, len;
    if (end == 0) { dx = p[1].x - p[0].x; dz = p[1].z - p[0].z; }
    else { dx = p[e->npt - 2].x - p[e->npt - 1].x; dz = p[e->npt - 2].z - p[e->npt - 1].z; }
    len = sqrt(dx * dx + dz * dz);
    *ux = len > 0.0 ? dx / len : 1.0;
    *uz = len > 0.0 ? dz / len : 0.0;
}

typedef struct { double ang, ux, uz, hw; int klass; } UEnd;

static int uend_cmp(const void *x, const void *y)
{
    const UEnd *p = (const UEnd *)x, *q = (const UEnd *)y;
    if (p->ang != q->ang) return p->ang < q->ang ? -1 : 1;
    return (p->hw > q->hw) - (p->hw < q->hw);
}

/* Every piece of cluster ci for the cuts t[]: stubs, internal edges, node
 * bevels. `ahead` (optional) gets each arm's ribbon beyond its cut. */
static int u_pieces(const TD5_RoamGraph *g, int ci, const TD5_RoamCluster *cl, const ArmTmp *at,
                    int na, const double *t, UPrims *L, UPrims *ahead, int *nodes, int nnodes)
{
    const double ox = cl->cx, oz = cl->cz;
    double px[U_POLY_MAX], pz[U_POLY_MAX];
    int m, k, i;
    L->n = 0;
    if (ahead) ahead->n = 0;
    for (m = 0; m < na; m++) {
        const ArmTmp *am = &at[m];
        TD5_RoamPt row[TD5_ROAM_ROW_MAX];
        double rx[TD5_ROAM_ROW_MAX], rz[TD5_ROAM_ROW_MAX];
        const int nrow = arm_cut_row(g, am, t[m], row);
        int np, r;
        for (r = 0; r < nrow; r++) { rx[r] = row[r].x - ox; rz[r] = row[r].z - oz; }
        np = arm_poly(g, am, 0.0, t[m], ox, oz, px, pz, U_POLY_MAX);
        if (!up_strip(L, px, pz, np, am->a.half_w, am->klass, m, rx, rz, nrow, m)) return 0;
        if (ahead) {
            const double d1 = t[m] + U_AHEAD < am->len - 1.0 ? t[m] + U_AHEAD : am->len - 1.0;
            if (d1 > t[m] + 1.0) {
                np = arm_poly(g, am, t[m], d1, ox, oz, px, pz, U_POLY_MAX);
                if (!up_strip(ahead, px, pz, np, am->a.half_w, am->klass, m, NULL, NULL, 0, m)) return 0;
            }
        }
    }
    /* Internal edges of this cluster, node to node. */
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        if (e->end[0].kind != TD5_ROAM_END_INTERNAL || e->end[0].cluster != ci) continue;
        if (e->npt > U_POLY_MAX) continue;
        for (k = 0; k < e->npt; k++) {
            px[k] = g->pt[e->pt_first + k].x - ox;
            pz[k] = g->pt[e->pt_first + k].z - oz;
        }
        if (!up_strip(L, px, pz, e->npt, e->half_w, e->klass, -1, NULL, NULL, 0, 0)) return 0;
    }
    /* Node bevels across gaps wider than 180 deg. */
    for (i = 0; i < nnodes; i++) {
        const TD5_RoamNode *nd = &g->node[nodes[i]];
        UEnd ue[64];
        int ne = 0;
        for (k = 0; k < nd->nend && ne < 64; k++) {
            const TD5_RoamEndRef *r = &g->node_end[nd->end_first + k];
            const TD5_RoamEdge *e = &g->edge[r->edge];
            int in = e->end[r->end].kind == TD5_ROAM_END_INTERNAL && e->end[r->end].cluster == ci;
            for (m = 0; m < na && !in; m++)
                if (at[m].ce[0] == r->edge && at[m].cq[0] == r->end) in = 1;
            if (!in) continue;
            u_end_dir(g, e, r->end, &ue[ne].ux, &ue[ne].uz);
            ue[ne].ang = atan2(ue[ne].uz, ue[ne].ux);
            ue[ne].hw = e->half_w;
            ue[ne].klass = e->klass;
            ne++;
        }
        if (ne < 2) continue;
        qsort(ue, (size_t)ne, sizeof *ue, uend_cmp);
        for (k = 0; k < ne; k++) {
            const UEnd *a = &ue[k], *b = &ue[(k + 1) % ne];
            double gap = b->ang - a->ang, bx[3], bz[3];
            while (gap <= 0.0) gap += 2.0 * ROAM_PI;
            /* Any gap past 180 deg, however slight: two near edges 0.03 deg
             * off collinear leave a 5 m deep sliver notch otherwise. */
            if (gap <= ROAM_PI) continue;
            bx[0] = nd->x - ox;                    bz[0] = nd->z - oz;
            bx[1] = bx[0] + -a->uz * a->hw;        bz[1] = bz[0] + a->ux * a->hw;     /* a's left  */
            bx[2] = bx[0] - -b->uz * b->hw;        bz[2] = bz[0] - b->ux * b->hw;     /* b's right */
            if (!up_add(L, bx, bz, NULL, 3, a->hw > b->hw ? a->hw : b->hw,
                        a->klass > b->klass ? a->klass : b->klass, -1)) return 0;
        }
    }
    return 1;
}

/* Signed distance of (x, z) into convex piece p (min over its edges; > 0 inside),
 * and the unit direction of the edge that attains it. */
static double u_depth(const UPrim *p, double x, double z, double *eux, double *euz)
{
    double dmin = 1e300;
    int e;
    for (e = 0; e < p->n; e++) {
        const int f = (e + 1) % p->n;
        const double ex = p->x[f] - p->x[e], ez = p->z[f] - p->z[e];
        const double len = sqrt(ex * ex + ez * ez);
        double d;
        if (len < 1e-9) continue;
        d = cross2(ex, ez, x - p->x[e], z - p->z[e]) / len;
        if (d < dmin) {
            dmin = d;
            if (eux) { *eux = ex / len; *euz = ez / len; }
        }
    }
    return dmin;
}

static int u_bbox_has(const UPrim *p, double x, double z, double pad)
{
    return x >= p->bx0 - pad && x <= p->bx1 + pad && z >= p->bz0 - pad && z <= p->bz1 + pad;
}

/* 1 when the sub-segment of piece k with midpoint (mx, mz) and direction (ux, uz)
 * is not on the union boundary. */
static int u_covered(const UPrims *L, int k, double mx, double mz, double ux, double uz)
{
    int p;
    for (p = 0; p < L->n; p++) {
        double d, ex = 0.0, ez = 0.0;
        if (p == k || !u_bbox_has(&L->p[p], mx, mz, U_EPS)) continue;
        d = u_depth(&L->p[p], mx, mz, &ex, &ez);
        if (d > U_EPS) return 1;
        if (d >= -U_EPS) {
            const double dot = ux * ex + uz * ez;
            if (dot < -0.5) return 1;              /* pieces on both sides: interior */
            if (dot > 0.5 && p < k) return 1;      /* same boundary: lower piece owns */
        }
    }
    return 0;
}

typedef struct { double ax, az, bx, bz; int ta, tb, prim; } USeg;
typedef struct { int a, b, prim; } UEdge;

static int u_weld(double **vx, double **vz, int **vt, int *nv, int *cap, double x, double z, int tag)
{
    int i;
    for (i = 0; i < *nv; i++) {
        const double dx = (*vx)[i] - x, dz = (*vz)[i] - z;
        if (fabs(dx) <= U_WELD && fabs(dz) <= U_WELD && dx * dx + dz * dz <= U_WELD * U_WELD) {
            if (tag && !(*vt)[i]) { (*vx)[i] = x; (*vz)[i] = z; (*vt)[i] = tag; }
            return i;
        }
    }
    {
        int c1 = *cap, c2 = *cap, c3 = *cap;
        if (!grow((void **)vx, &c1, *nv + 1, sizeof **vx) || !grow((void **)vz, &c2, *nv + 1, sizeof **vz) ||
            !grow((void **)vt, &c3, *nv + 1, sizeof **vt)) return -1;
        *cap = c1;
    }
    (*vx)[*nv] = x;
    (*vz)[*nv] = z;
    (*vt)[*nv] = tag;
    return (*nv)++;
}

static int u_loop_push(ULoops *U, int li, double x, double z, int tag, int prim)
{
    if (!grow((void **)&U->v[li], &U->cap[li], U->n[li] + 1, sizeof *U->v[li])) return 0;
    U->v[li][U->n[li]].x = x;
    U->v[li][U->n[li]].z = z;
    U->v[li][U->n[li]].tag = tag;
    U->v[li][U->n[li]].prim = prim;
    U->n[li]++;
    return 1;
}

/* The union boundary of L as closed loops (CCW outer, CW holes). Returns 1 on
 * success, 0 on out of memory or a boundary that does not close. dbg (dev
 * only, may be NULL) gets the pieces and the kept boundary segments as JSON. */
static int u_union(const UPrims *L, ULoops *U, FILE *dbg)
{
    USeg *sg = NULL;
    UEdge *ed = NULL;
    double *vx = NULL, *vz = NULL;
    int *vt = NULL, *head = NULL, *nxt = NULL, *used = NULL;
    int ns = 0, cap_s = 0, nv = 0, cap_v = 0, ne = 0, cap_e = 0, ok = 0, k, e, p, q;
    memset(U, 0, sizeof *U);
    for (k = 0; k < L->n; k++) {
        const UPrim *pk = &L->p[k];
        for (e = 0; e < pk->n; e++) {
            const int e1 = (e + 1) % pk->n;
            const double ax = pk->x[e], az = pk->z[e], bx = pk->x[e1], bz = pk->z[e1];
            const double rx = bx - ax, rz = bz - az, rl2 = rx * rx + rz * rz, rl = sqrt(rl2);
            const double sx0 = (ax < bx ? ax : bx) - U_EPS, sx1 = (ax > bx ? ax : bx) + U_EPS;
            const double sz0 = (az < bz ? az : bz) - U_EPS, sz1 = (az > bz ? az : bz) + U_EPS;
            double tp[256];
            int ntp = 0;
            if (rl < U_EPS) continue;
            tp[ntp++] = 0.0;
            tp[ntp++] = 1.0;
            for (p = 0; p < L->n; p++) {
                const UPrim *pp = &L->p[p];
                int f;
                if (p == k || pp->bx1 < sx0 || pp->bx0 > sx1 || pp->bz1 < sz0 || pp->bz0 > sz1) continue;
                for (f = 0; f < pp->n; f++) {
                    const int f1 = (f + 1) % pp->n;
                    const double cx = pp->x[f], cz = pp->z[f];
                    const double sx = pp->x[f1] - cx, sz = pp->z[f1] - cz, sl = sqrt(sx * sx + sz * sz);
                    const double den = cross2(rx, rz, sx, sz);
                    double tt;
                    if (sl > 1e-9 && fabs(den) > 1e-9 * rl * sl) {
                        const double uu = cross2(cx - ax, cz - az, rx, rz) / den;
                        tt = cross2(cx - ax, cz - az, sx, sz) / den;
                        if (tt > 0.0 && tt < 1.0 && uu >= -U_EPS / sl && uu <= 1.0 + U_EPS / sl && ntp < 256)
                            tp[ntp++] = tt;
                    }
                    tt = ((cx - ax) * rx + (cz - az) * rz) / rl2;
                    if (tt > 0.0 && tt < 1.0 && fabs(cross2(rx, rz, cx - ax, cz - az)) / rl < U_EPS && ntp < 256)
                        tp[ntp++] = tt;
                }
            }
            for (p = 1; p < ntp; p++)
                for (q = p; q > 0 && tp[q] < tp[q - 1]; q--) { const double w = tp[q]; tp[q] = tp[q - 1]; tp[q - 1] = w; }
            for (p = 0; p + 1 < ntp; p++) {
                const double t0 = tp[p];
                double t1 = tp[p + 1];
                USeg s;
                int q2 = p + 1;
                /* Merge split points closer than U_EPS (one sub-segment). The
                 * shift this makes must stay well inside U_WELD, or two ends of
                 * a sliver stop meeting at the weld. */
                while ((t1 - t0) * rl < U_EPS && q2 + 1 < ntp) { q2++; t1 = tp[q2]; }
                if ((t1 - t0) * rl < U_EPS) break;
                p = q2 - 1;
                if (u_covered(L, k, ax + rx * 0.5 * (t0 + t1), az + rz * 0.5 * (t0 + t1), rx / rl, rz / rl))
                    continue;
                s.ax = t0 == 0.0 ? ax : ax + rx * t0;
                s.az = t0 == 0.0 ? az : az + rz * t0;
                s.ta = t0 == 0.0 ? pk->tag[e] : 0;
                s.bx = t1 == 1.0 ? bx : ax + rx * t1;
                s.bz = t1 == 1.0 ? bz : az + rz * t1;
                s.tb = t1 == 1.0 ? pk->tag[e1] : 0;
                s.prim = k;
                if (!grow((void **)&sg, &cap_s, ns + 1, sizeof *sg)) goto done;
                sg[ns++] = s;
            }
        }
    }
    if (dbg) {
        fprintf(dbg, "{\"prims\": [");
        for (k = 0; k < L->n; k++) {
            fprintf(dbg, "%s[", k ? ",\n" : "");
            for (e = 0; e < L->p[k].n; e++)
                fprintf(dbg, "%s[%.3f, %.3f, %d]", e ? ", " : "", L->p[k].x[e], L->p[k].z[e], L->p[k].tag[e]);
            fprintf(dbg, "]");
        }
        fprintf(dbg, "],\n\"segs\": [");
        for (k = 0; k < ns; k++)
            fprintf(dbg, "%s[%.3f, %.3f, %.3f, %.3f, %d]", k ? ",\n" : "", sg[k].ax, sg[k].az, sg[k].bx,
                    sg[k].bz, sg[k].prim);
        fprintf(dbg, "]}\n");
    }
    /* Weld the endpoints (tagged points first so they keep exact positions). */
    for (q = 0; q < 2; q++)
        for (k = 0; k < ns; k++) {
            if (q == 0 && sg[k].ta && u_weld(&vx, &vz, &vt, &nv, &cap_v, sg[k].ax, sg[k].az, sg[k].ta) < 0) goto done;
            if (q == 0 && sg[k].tb && u_weld(&vx, &vz, &vt, &nv, &cap_v, sg[k].bx, sg[k].bz, sg[k].tb) < 0) goto done;
            if (q == 1) {
                const int a = u_weld(&vx, &vz, &vt, &nv, &cap_v, sg[k].ax, sg[k].az, sg[k].ta);
                const int b = u_weld(&vx, &vz, &vt, &nv, &cap_v, sg[k].bx, sg[k].bz, sg[k].tb);
                if (a < 0 || b < 0) goto done;
                if (a == b) continue;
                if (!grow((void **)&ed, &cap_e, ne + 1, sizeof *ed)) goto done;
                ed[ne].a = a; ed[ne].b = b; ed[ne].prim = sg[k].prim;
                ne++;
            }
        }
    /* Drop duplicates and opposite pairs (the two sides of a shared seam). */
    used = (int *)calloc((size_t)(ne > 0 ? ne : 1), sizeof(int));
    if (!used) goto done;
    for (k = 0; k < ne; k++) {
        if (used[k]) continue;
        for (p = k + 1; p < ne; p++) {
            if (used[p]) continue;
            if (ed[p].a == ed[k].a && ed[p].b == ed[k].b) used[p] = 1;
            else if (ed[p].a == ed[k].b && ed[p].b == ed[k].a) { used[p] = used[k] = 1; break; }
        }
    }
    /* Prune dangling segments: one that starts where nothing arrives, or ends
     * where nothing leaves, cannot be on a closed boundary. They come from
     * slivers far below a millimetre between two nearly collinear piece edges
     * (consecutive quads of a polyline with a 0.01 deg turn), where the side
     * tests of the two edges disagree. */
    head = (int *)malloc((size_t)(nv > 0 ? nv : 1) * sizeof(int));
    nxt = (int *)malloc((size_t)(nv > 0 ? nv : 1) * sizeof(int));
    if (!head || !nxt) goto done;
    {
        int *din = head, *dout = nxt, changed = 1;
        for (k = 0; k < nv; k++) din[k] = dout[k] = 0;
        for (k = 0; k < ne; k++) if (!used[k]) { dout[ed[k].a]++; din[ed[k].b]++; }
        while (changed) {
            changed = 0;
            for (k = 0; k < ne; k++) {
                if (used[k] || (din[ed[k].a] > 0 && dout[ed[k].b] > 0)) continue;
                used[k] = 1;
                dout[ed[k].a]--;
                din[ed[k].b]--;
                U->pruned++;
                changed = 1;
            }
        }
    }
    free(nxt);
    nxt = (int *)malloc((size_t)(ne > 0 ? ne : 1) * sizeof(int));
    if (!nxt) goto done;
    for (k = 0; k < nv; k++) head[k] = -1;
    for (k = ne - 1; k >= 0; k--) {
        if (used[k]) continue;
        nxt[k] = head[ed[k].a];
        head[ed[k].a] = k;
    }
    /* Chain. At a vertex with several ways out, take the first clockwise from
     * the way back (keeps each lobe its own loop). */
    for (k = 0; k < ne; k++) {
        int cur = k, guard = 0, li;
        double area = 0.0;
        if (used[k]) continue;
        if (U->nloop >= U_LOOP_MAX) goto done;
        li = U->nloop++;
        for (;;) {
            const int v = ed[cur].b;
            int best = -1, o;
            double bang = 1e300;
            used[cur] = 1;
            if (!u_loop_push(U, li, vx[ed[cur].a], vz[ed[cur].a], vt[ed[cur].a], ed[cur].prim)) goto done;
            if (v == ed[k].a) break;
            for (o = head[v]; o >= 0; o = nxt[o]) {
                double rbx, rbz, ux2, uz2, ccw, cw;
                if (used[o]) continue;
                rbx = vx[ed[cur].a] - vx[v]; rbz = vz[ed[cur].a] - vz[v];
                ux2 = vx[ed[o].b] - vx[v];   uz2 = vz[ed[o].b] - vz[v];
                ccw = atan2(cross2(rbx, rbz, ux2, uz2), rbx * ux2 + rbz * uz2);
                cw = -ccw;
                while (cw <= 1e-12) cw += 2.0 * ROAM_PI;
                if (cw < bang) { bang = cw; best = o; }
            }
            if (best < 0 || ++guard > ne) goto done;         /* open boundary */
            cur = best;
        }
        for (p = 0; p < U->n[li]; p++) {
            const ULV *a = &U->v[li][p], *b = &U->v[li][(p + 1) % U->n[li]];
            area += cross2(a->x, a->z, b->x, b->z);
        }
        U->area[li] = 0.5 * area;
    }
    ok = 1;
done:
    free(sg); free(ed); free(vx); free(vz); free(vt); free(head); free(nxt); free(used);
    if (!ok) uloops_free(U);
    return ok;
}

/* Drop untagged vertices that are collinear with their neighbours, closer than
 * ROAM_OL_MIN_SEG to the previous one, or the tip of a thin spike (the outline
 * turns back by more than 170 deg and the spike's mouth is under U_SPIKE_W):
 * a sliver between two piece edges a hair off collinear, which would put a
 * wall into the paving. */
#define U_SPIKE_W (0.25 * TD5_ROAM_UPM)
static void u_simplify(ULV *v, int *n)
{
    int changed = 1, guard = 0;
    while (changed && *n > 3 && guard++ < 1000) {
        int k;
        changed = 0;
        for (k = 0; k < *n && *n > 3; k++) {
            const ULV *a = &v[(k + *n - 1) % *n], *b = &v[k], *c = &v[(k + 1) % *n];
            const double abx = b->x - a->x, abz = b->z - a->z, bcx = c->x - b->x, bcz = c->z - b->z;
            const double acx = c->x - a->x, acz = c->z - a->z, acl = sqrt(acx * acx + acz * acz);
            const double lab = sqrt(abx * abx + abz * abz), lbc = sqrt(bcx * bcx + bcz * bcz);
            int drop = 0;
            if (b->tag) continue;
            if (acl > 1e-9 && fabs(cross2(acx, acz, b->x - a->x, b->z - a->z)) / acl < 0.5 &&
                abx * bcx + abz * bcz > 0.0) drop = 1;
            else if (lab < ROAM_OL_MIN_SEG) drop = 1;
            else if (lab > 0.0 && lbc > 0.0 && (abx * bcx + abz * bcz) / (lab * lbc) < -0.98481 &&
                     acl < U_SPIKE_W) drop = 1;                  /* cos 170 deg */
            if (drop) {
                /* The segment leaving a now comes from b's piece if a's was shorter. */
                memmove(&v[k], &v[k + 1], (size_t)(*n - k - 1) * sizeof *v);
                (*n)--;
                changed = 1;
                k--;
            }
        }
    }
}

static int u_simple(const ULV *pv, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        const ULV *a = &pv[i], *b = &pv[(i + 1) % n];
        for (j = i + 1; j < n; j++) {
            const ULV *c = &pv[j], *d = &pv[(j + 1) % n];
            double d1, d2, d3, d4;
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;
            d1 = cross2(b->x - a->x, b->z - a->z, c->x - a->x, c->z - a->z);
            d2 = cross2(b->x - a->x, b->z - a->z, d->x - a->x, d->z - a->z);
            d3 = cross2(d->x - c->x, d->z - c->z, a->x - c->x, a->z - c->z);
            d4 = cross2(d->x - c->x, d->z - c->z, b->x - c->x, b->z - c->z);
            if (((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0)) &&
                d1 != 0.0 && d2 != 0.0 && d3 != 0.0 && d4 != 0.0)
                return 0;
        }
    }
    return 1;
}

/* Kerb fillets at the reflex vertices (block corners) of the outer loop.
 * need[m] gets the extra kerb length arm m must give for a full fillet.
 * Writes the new loop to *out (malloc'd). Returns its count, -1 on OOM. */
static int u_fillet(const ULV *v, int n, const UPrims *L, const TD5_RoamGraphParams *P,
                    double W, double LF, const unsigned char *fixed, const unsigned char *pr,
                    int na, double *need, TD5_RoamGraphCensus *c, TD5_RoamCluster *cl, ULV **out)
{
    const int segs = P->fillet_segs < 1 ? 1 : (P->fillet_segs > 12 ? 12 : P->fillet_segs);
    unsigned char *cand = (unsigned char *)calloc((size_t)n, 1);
    ULV *o = (ULV *)malloc((size_t)n * (size_t)(segs + 2) * sizeof(ULV));
    int q, no = 0;
    if (!cand || !o) { free(cand); free(o); return -1; }
    for (q = 0; q < n; q++) {
        const ULV *a = &v[(q + n - 1) % n], *b = &v[q], *d = &v[(q + 1) % n];
        const double ix = b->x - a->x, iz = b->z - a->z, ox = d->x - b->x, oz = d->z - b->z;
        const double cr = cross2(ix, iz, ox, oz);
        const double phi = atan2(fabs(cr), ix * ox + iz * oz);
        const int fa = L->p[a->prim].arm, fb = L->p[b->prim].arm;
        if (b->tag || cr >= 0.0) continue;                  /* convex: no block corner */
        if (phi < U_STRAIGHT || ROAM_PI - phi < U_GORE_RAD) continue;
        /* A corner made by two partner arms (they overlap all the way), or by
         * a fixed arm, is where one stub pokes out of the other's kerb, not a
         * block corner: the road goes on as their ribbons. */
        if ((fa >= 0 && fixed[fa]) || (fb >= 0 && fixed[fb]) ||
            (fa >= 0 && fb >= 0 && fa != fb && pr[fa * na + fb])) continue;
        cand[q] = 1;
    }
    for (q = 0; q < n; q++) {
        const ULV *a = &v[(q + n - 1) % n], *b = &v[q], *d = &v[(q + 1) % n];
        const int qa = (q + n - 1) % n, qd = (q + 1) % n;
        double lp, ln, ap, an, th, tanh2, r, tl, tlmax, corridor, hwa, hwb, ex, ez, fx, fz;
        int ka, kb, k, widened = 0;
        if (!cand[q]) { o[no++] = *b; continue; }
        lp = hypot(b->x - a->x, b->z - a->z);
        ln = hypot(d->x - b->x, d->z - b->z);
        ap = cand[qa] ? 0.5 * lp : lp - ROAM_OL_MIN_SEG;
        an = cand[qd] ? 0.5 * ln : ln - ROAM_OL_MIN_SEG;
        th = ROAM_PI - atan2(fabs(cross2(b->x - a->x, b->z - a->z, d->x - b->x, d->z - b->z)),
                             (b->x - a->x) * (d->x - b->x) + (b->z - a->z) * (d->z - b->z));
        tanh2 = tan(0.5 * th);
        ka = L->p[a->prim].klass;
        kb = L->p[b->prim].klass;
        hwa = L->p[a->prim].hw;
        hwb = L->p[b->prim].hw;
        r = (ka >= TD5_GEO_RC_PRIMARY || kb >= TD5_GEO_RC_PRIMARY) ? P->kerb_r_major : P->kerb_r_minor;
        corridor = 2.0 * (hwa < hwb ? hwa : hwb);
        c->swept_checked++;
        if (swept_need(r, W, LF) > corridor && corridor > W) {
            const double rq = (W * W + LF * LF - corridor * corridor) / (2.0 * (corridor - W));
            if (rq > r) { r = rq; widened = 1; }
        }
        tl = r / tanh2;
        tlmax = ap < an ? ap : an;
        if (tl > tlmax) {
            /* The short side ends on a cut row and is that arm's own kerb:
             * the arm owes the difference. */
            if (!cand[qa] && a->tag && tl > ap && L->p[a->prim].arm == U_TAG_ARM(a->tag)) {
                const int m = U_TAG_ARM(a->tag);
                if (tl - ap > need[m]) need[m] = tl - ap;
            }
            if (!cand[qd] && d->tag && tl > an && L->p[b->prim].arm == U_TAG_ARM(d->tag)) {
                const int m = U_TAG_ARM(d->tag);
                if (tl - an > need[m]) need[m] = tl - an;
            }
            tl = tlmax > 0.0 ? tlmax : 0.0;
            r = tl * tanh2;
            c->fillet_shrunk++;
        }
        if (tl < ROAM_OL_MIN_SEG) { o[no++] = *b; continue; }
        ex = (a->x - b->x) / lp; ez = (a->z - b->z) / lp;     /* towards a */
        fx = (d->x - b->x) / ln; fz = (d->z - b->z) / ln;     /* towards d */
        {
            double bx = ex + fx, bz = ez + fz;
            const double bl = sqrt(bx * bx + bz * bz), sh = sin(0.5 * th);
            const double t1x = b->x + ex * tl, t1z = b->z + ez * tl;
            const double t2x = b->x + fx * tl, t2z = b->z + fz * tl;
            double oxc, ozc, a0, a1, da, need_w;
            bx = bl > 1e-12 ? bx / bl : 0.0;
            bz = bl > 1e-12 ? bz / bl : 0.0;
            oxc = b->x + bx * (sh > 1e-9 ? r / sh : 0.0);
            ozc = b->z + bz * (sh > 1e-9 ? r / sh : 0.0);
            a0 = atan2(t1z - ozc, t1x - oxc);
            a1 = atan2(t2z - ozc, t2x - oxc);
            da = a1 - a0;
            while (da > ROAM_PI) da -= 2.0 * ROAM_PI;
            while (da < -ROAM_PI) da += 2.0 * ROAM_PI;
            /* Two fillets that share a kerb segment meet at its midpoint: one
             * tangent point, not two. */
            if (!(no > 0 && fabs(o[no - 1].x - t1x) < 1e-6 && fabs(o[no - 1].z - t1z) < 1e-6)) {
                o[no].x = t1x; o[no].z = t1z; o[no].tag = 0; o[no].prim = a->prim; no++;
            }
            for (k = 1; k < segs; k++) {
                const double an2 = a0 + da * (double)k / (double)segs;
                o[no].x = oxc + r * cos(an2);
                o[no].z = ozc + r * sin(an2);
                o[no].tag = 0;
                o[no].prim = a->prim;
                no++;
            }
            o[no].x = t2x; o[no].z = t2z; o[no].tag = 0; o[no].prim = b->prim; no++;
            need_w = swept_need(r, W, LF);
            if (corridor - need_w < cl->min_swept_margin) cl->min_swept_margin = corridor - need_w;
            if (need_w > corridor) {
                cl->flags |= TD5_ROAM_CL_SWEPT_FAIL;
                c->swept_fail++;
                TD5_LOG_W(LOG_TAG, "ROAM_SWEPT_FAIL cluster %lld (union corner): corridor %.2f m, "
                          "need %.2f m (fillet %.2f m)", (long long)cl->key, corridor / TD5_ROAM_UPM,
                          need_w / TD5_ROAM_UPM, r / TD5_ROAM_UPM);
            } else if (widened) {
                c->swept_widened++;
            }
            c->fillets++;
        }
    }
    /* The same across the wrap (vertex n-1's T2 against vertex 0's T1). */
    if (no > 1 && fabs(o[no - 1].x - o[0].x) < 1e-6 && fabs(o[no - 1].z - o[0].z) < 1e-6) no--;
    free(cand);
    *out = o;
    return no;
}

/* Ear clipping for UNION outlines (long thin median slots, bridged holes, rows
 * of collinear lane vertices). An ear (a, b, c) is valid when b turns left, no
 * other vertex is strictly inside the triangle or on the open diagonal a-c, and
 * no ring edge properly crosses the diagonal. Of the valid ears the one with
 * the largest smallest angle is cut. Writes nout - 2 triangles; returns 1 on
 * success. */
static double u_min_angle_sin(const TD5_RoamPatchVert *a, const TD5_RoamPatchVert *b,
                              const TD5_RoamPatchVert *c)
{
    const double ab = hypot(b->x - a->x, b->z - a->z), bc = hypot(c->x - b->x, c->z - b->z);
    const double ca = hypot(a->x - c->x, a->z - c->z);
    const double ar2 = fabs(cross2(b->x - a->x, b->z - a->z, c->x - a->x, c->z - a->z));
    double m = 1e300;
    if (ab <= 0.0 || bc <= 0.0 || ca <= 0.0) return 0.0;
    /* sin of the angle at each corner = 2 area / (product of its two sides). */
    if (ar2 / (ab * ca) < m) m = ar2 / (ab * ca);
    if (ar2 / (ab * bc) < m) m = ar2 / (ab * bc);
    if (ar2 / (bc * ca) < m) m = ar2 / (bc * ca);
    return m;
}

static int ear_clip_u(const TD5_RoamPatchVert *pv, int nout, LTri *out, int *ntri)
{
    int *idx = (int *)malloc((size_t)nout * sizeof(int));
    int n = nout, k;
    *ntri = 0;
    if (!idx) return 0;
    for (k = 0; k < nout; k++) idx[k] = k;
    while (n > 3) {
        int best = -1;
        double bq = -1.0;
        for (k = 0; k < n; k++) {
            const int ip = idx[(k + n - 1) % n], ic = idx[k], in = idx[(k + 1) % n];
            const TD5_RoamPatchVert *a = &pv[ip], *b = &pv[ic], *c = &pv[in];
            const double cr = cross2(b->x - a->x, b->z - a->z, c->x - b->x, c->z - b->z);
            const double acx = c->x - a->x, acz = c->z - a->z;
            int q, bad = 0;
            double qual;
            if (cr <= 1e-9 * hypot(b->x - a->x, b->z - a->z) * hypot(c->x - b->x, c->z - b->z)) continue;
            for (q = 0; q < n && !bad; q++) {
                const TD5_RoamPatchVert *p = &pv[idx[q]], *p2 = &pv[idx[(q + 1) % n]];
                double d1, d2, d3;
                if (idx[q] == ip || idx[q] == ic || idx[q] == in) goto edge_test;
                if ((p->x == a->x && p->z == a->z) || (p->x == b->x && p->z == b->z) ||
                    (p->x == c->x && p->z == c->z)) goto edge_test;
                d1 = cross2(b->x - a->x, b->z - a->z, p->x - a->x, p->z - a->z);
                d2 = cross2(c->x - b->x, c->z - b->z, p->x - b->x, p->z - b->z);
                d3 = cross2(a->x - c->x, a->z - c->z, p->x - c->x, p->z - c->z);
                if (d1 > 0.0 && d2 > 0.0 && d3 > 0.0) { bad = 1; break; }
                if (d1 > 0.0 && d2 > 0.0 && d3 == 0.0) { bad = 1; break; }      /* on the diagonal */
            edge_test:
                /* Ring edge (p, p2) properly crossing the diagonal a-c. */
                if ((p->x == a->x && p->z == a->z) || (p->x == c->x && p->z == c->z) ||
                    (p2->x == a->x && p2->z == a->z) || (p2->x == c->x && p2->z == c->z)) continue;
                {
                    const double e1 = cross2(acx, acz, p->x - a->x, p->z - a->z);
                    const double e2 = cross2(acx, acz, p2->x - a->x, p2->z - a->z);
                    const double e3 = cross2(p2->x - p->x, p2->z - p->z, a->x - p->x, a->z - p->z);
                    const double e4 = cross2(p2->x - p->x, p2->z - p->z, c->x - p->x, c->z - p->z);
                    if (((e1 > 0.0) != (e2 > 0.0)) && ((e3 > 0.0) != (e4 > 0.0)) &&
                        e1 != 0.0 && e2 != 0.0 && e3 != 0.0 && e4 != 0.0) bad = 1;
                }
            }
            if (bad) continue;
            qual = u_min_angle_sin(a, b, c);
            if (qual > bq) { bq = qual; best = k; }
        }
        if (best < 0) { free(idx); return 0; }
        out[*ntri].v[0] = idx[(best + n - 1) % n];
        out[*ntri].v[1] = idx[best];
        out[*ntri].v[2] = idx[(best + 1) % n];
        (*ntri)++;
        for (k = best; k + 1 < n; k++) idx[k] = idx[k + 1];
        n--;
    }
    out[*ntri].v[0] = idx[0];
    out[*ntri].v[1] = idx[1];
    out[*ntri].v[2] = idx[2];
    (*ntri)++;
    free(idx);
    return 1;
}

/* 1 when segment (a, b) crosses no edge of the ring pv[ring[0..n-1]], except
 * edges that touch a or b. */
static int pv_seg_clear(const TD5_RoamPatchVert *pv, const int *ring, int n,
                        double ax, double az, double bx, double bz)
{
    int k;
    for (k = 0; k < n; k++) {
        const TD5_RoamPatchVert *c = &pv[ring[k]], *d = &pv[ring[(k + 1) % n]];
        double d1, d2, d3, d4;
        if ((c->x == ax && c->z == az) || (d->x == ax && d->z == az) ||
            (c->x == bx && c->z == bz) || (d->x == bx && d->z == bz)) continue;
        d1 = cross2(bx - ax, bz - az, c->x - ax, c->z - az);
        d2 = cross2(bx - ax, bz - az, d->x - ax, d->z - az);
        d3 = cross2(d->x - c->x, d->z - c->z, ax - c->x, az - c->z);
        d4 = cross2(d->x - c->x, d->z - c->z, bx - c->x, bz - c->z);
        if (((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0))) return 0;
    }
    return 1;
}

/* 1 when the outer loop holds arm m's whole cut row, lane 0 .. lanes in order. */
static int u_row_on_loop(const ULV *v, int n, int m, int nrow)
{
    int q, r;
    for (q = 0; q < n; q++)
        if (v[q].tag == U_TAG(m, 0)) break;
    if (q == n) return 0;
    for (r = 1; r < nrow; r++)
        if (v[(q + r) % n].tag != U_TAG(m, r)) return 0;
    return 1;
}

typedef struct {
    int iters, restarts, rounds, fixed, holes, row_cov, dropped_loops, pruned;
} UStat;

/* 1 when arm m must be pushed out at the current pieces: a piece covers a point
 * of its cut row (lane vertices and lane midpoints), or a piece point reaches
 * into its ribbon. Pieces of m's partners (pr[m * na + j]) and of fully fixed
 * arms are ignored. who[j] = 1 for each arm whose pieces push m, *other = 1
 * when a piece of no arm (internal edge, bevel) does. */
static int u_needs_push(const TD5_RoamGraph *g, const TD5_RoamCluster *cl, const ArmTmp *at, int na,
                        const double *t, const UPrims *L, const UPrims *A, int m,
                        const unsigned char *fixed, const unsigned char *pr,
                        unsigned char *who, int *other)
{
    TD5_RoamPt row[TD5_ROAM_ROW_MAX];
    const int nrow = arm_cut_row(g, &at[m], t[m], row);
    int r, p, v, a, push = 0;
    memset(who, 0, (size_t)na);
    *other = 0;
    for (p = 0; p < L->n; p++) {
        const UPrim *pp = &L->p[p];
        int hit = 0;
        if (pp->arm == m || (pp->arm >= 0 && (fixed[pp->arm] || pr[m * na + pp->arm]))) continue;
        for (r = 0; r < 2 * nrow - 1 && !hit; r++) {
            const double x = (r & 1 ? 0.5 * (row[r / 2].x + row[r / 2 + 1].x) : row[r / 2].x) - cl->cx;
            const double z = (r & 1 ? 0.5 * (row[r / 2].z + row[r / 2 + 1].z) : row[r / 2].z) - cl->cz;
            if (u_bbox_has(pp, x, z, 0.0) && u_depth(pp, x, z, NULL, NULL) > U_COVER) hit = 1;
        }
        for (v = 0; v < 2 * pp->n && !hit; v++) {
            const int v0 = v / 2, v1 = (v / 2 + 1) % pp->n;
            const double x = v & 1 ? 0.5 * (pp->x[v0] + pp->x[v1]) : pp->x[v0];
            const double z = v & 1 ? 0.5 * (pp->z[v0] + pp->z[v1]) : pp->z[v0];
            for (a = 0; a < A->n && !hit; a++)
                if (A->p[a].arm == m && u_bbox_has(&A->p[a], x, z, 0.0) &&
                    u_depth(&A->p[a], x, z, NULL, NULL) > U_COVER) hit = 1;
        }
        if (!hit) continue;
        push = 1;
        if (pp->arm >= 0) who[pp->arm] = 1; else *other = 1;
    }
    return push;
}

/* MODE_UNION, part 1: the final cuts (written to at[m].a.t) and the outline
 * loops (U->v[0] = outer, CCW; the rest = holes, CW), local frame. Returns 1 on
 * success, 0 when no valid outline was found (U empty), -1 on out of memory. */
static int union_solve(const TD5_RoamGraph *g, int ci, TD5_RoamCluster *cl, ArmTmp *at, int na,
                       TD5_RoamGraphCensus *c, ULoops *U, UStat *st)
{
    const TD5_RoamGraphParams *P = &g->prm;
    const double W = 2.0 * P->body_half_w, LF = P->body_lf;
    UPrims L, A;
    TD5_RoamGraphCensus sc;
    double *t = NULL, *tb = NULL, *need = NULL;
    unsigned char *fixed = NULL, *push = NULL, *pr = NULL, *who = NULL;
    int *oth = NULL;
    FILE *dbgf = NULL;
    int *nodes = NULL, nnodes = 0, cap_nodes = 0, m, k, i, ret = -1, round;
    memset(&L, 0, sizeof L);
    memset(&A, 0, sizeof A);
    memset(U, 0, sizeof *U);
    memset(st, 0, sizeof *st);
    t = (double *)calloc((size_t)na, sizeof(double));
    tb = (double *)calloc((size_t)na, sizeof(double));
    need = (double *)calloc((size_t)na, sizeof(double));
    fixed = (unsigned char *)calloc((size_t)na, 1);
    push = (unsigned char *)calloc((size_t)na, 1);
    pr = (unsigned char *)calloc((size_t)na * (size_t)na, 1);
    who = (unsigned char *)calloc((size_t)na * (size_t)na, 1);
    oth = (int *)calloc((size_t)na, sizeof(int));
    if (!t || !tb || !need || !fixed || !push || !pr || !who || !oth) goto out;
    /* Nodes that get bevels: the members and every internal edge's ends. */
    for (k = 0; k < cl->nnode; k++) {
        if (!grow((void **)&nodes, &cap_nodes, nnodes + 1, sizeof *nodes)) goto out;
        nodes[nnodes++] = g->cluster_node[cl->node_first + k];
    }
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        if (e->end[0].kind != TD5_ROAM_END_INTERNAL || e->end[0].cluster != ci) continue;
        if (!grow((void **)&nodes, &cap_nodes, nnodes + 2, sizeof *nodes)) goto out;
        nodes[nnodes++] = e->node_a;
        nodes[nnodes++] = e->node_b;
    }
    for (k = 1; k < nnodes; k++)
        for (i = k; i > 0 && nodes[i] < nodes[i - 1]; i--) { const int w = nodes[i]; nodes[i] = nodes[i - 1]; nodes[i - 1] = w; }
    for (k = 0, i = 0; k < nnodes; k++) if (i == 0 || nodes[i - 1] != nodes[k]) nodes[i++] = nodes[k];
    nnodes = i;
    for (m = 0; m < na; m++) t[m] = tb[m] = at[m].a.t;

    for (round = 0;; round++) {
        int any = 0, nf, outer = -1;
        ULV *fl = NULL;
        /* Push cuts out while a row is covered or a piece reaches into a ribbon.
         * An arm that reaches its limit still pushed makes PARTNERS of the arms
         * pushing it (two carriageways OSM draws overlapping for their whole
         * length, a Y split whose gore lies past half the arm): partners ignore
         * each other from then on, and all pushes restart from the base cuts.
         * An arm still pushed at its limit by no new arm is fully FIXED: it
         * stays at its base cut and pushes nobody. */
        for (;;) {
            int changed = 0, newfix = 0, it;
            for (it = 0; it < U_MAXIT; it++) {
                if (!u_pieces(g, ci, cl, at, na, t, &L, &A, nodes, nnodes)) goto out;
                st->iters++;
                for (m = 0; m < na; m++)
                    push[m] = !fixed[m] && u_needs_push(g, cl, at, na, t, &L, &A, m, fixed, pr,
                                                         who + m * na, &oth[m]);
                changed = newfix = 0;
                for (m = 0; m < na; m++) {
                    int j, fresh = 0;
                    if (!push[m]) continue;
                    if (t[m] < at[m].lim) {
                        t[m] = t[m] + U_STEP < at[m].lim ? t[m] + U_STEP : at[m].lim;
                        changed = 1;
                        continue;
                    }
                    for (j = 0; j < na; j++)
                        if (who[m * na + j] && !pr[m * na + j]) {
                            pr[m * na + j] = pr[j * na + m] = 1;
                            fresh = 1;
                        }
                    if (!fresh) fixed[m] = 1;
                    newfix = 1;
                    st->fixed++;
                }
                if (newfix || !changed) break;
            }
            if (!newfix) break;
            st->restarts++;
            for (m = 0; m < na; m++) t[m] = tb[m];
        }
        /* An arm follows the partner whose stub covers its row (up to its own
         * limit): its ribbon then starts beside that partner's row instead of
         * running under the partner's stub. Twice, for chains of partners. */
        for (k = 0; k < 2; k++) {
            int moved = 0;
            for (m = 0; m < na; m++) {
                TD5_RoamPt row[TD5_ROAM_ROW_MAX];
                double tj = t[m];
                int nrow, r, p;
                nrow = arm_cut_row(g, &at[m], t[m], row);
                for (r = 0; r < nrow; r++)
                    for (p = 0; p < L.n; p++) {
                        const int ja = L.p[p].arm;
                        const double x = row[r].x - cl->cx, z = row[r].z - cl->cz;
                        if (ja < 0 || ja == m || !(pr[m * na + ja] || fixed[m]) || fixed[ja] || t[ja] <= tj)
                            continue;
                        if (u_bbox_has(&L.p[p], x, z, 0.0) && u_depth(&L.p[p], x, z, NULL, NULL) > U_COVER)
                            tj = t[ja];
                    }
                if (tj > at[m].lim) tj = at[m].lim;
                if (tj > t[m]) { t[m] = tj; moved = 1; }
            }
            if (!moved) break;
            if (!u_pieces(g, ci, cl, at, na, t, &L, &A, nodes, nnodes)) goto out;
        }
        /* The union of the pieces at these cuts. */
        uloops_free(U);
#ifndef TD5RE_RELEASE
        {
            /* DEV: TD5RE_ROAM_UNION_DEBUG=<cluster key> dumps this union. */
            const char *dk = getenv("TD5RE_ROAM_UNION_DEBUG");
            if (dk && strtoll(dk, NULL, 10) == (long long)cl->key) {
                char path[400];
                const char *dir = td5_plat_log_dir();
                snprintf(path, sizeof path, "%sroam_union_dbg.json", dir ? dir : "");
                dbgf = fopen(path, "w");
            }
        }
#endif
        k = u_union(&L, U, dbgf);
        if (dbgf) { fclose(dbgf); dbgf = NULL; }
        if (!k) {
            TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: the union boundary does not close "
                      "(%d pieces)", (long long)cl->key, L.n);
            ret = 0;
            goto out;
        }
        /* Keep one outer loop and the holes worth walling; drop slivers. */
        st->pruned = U->pruned;
        {
            int w = 0, nbig = 0;
            for (k = 0; k < U->nloop; k++)
                if (U->area[k] > U_HOLE_MIN) { nbig++; if (outer < 0 || U->area[k] > U->area[outer]) outer = k; }
            if (nbig != 1) {
                TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: %d outer loop(s)",
                          (long long)cl->key, nbig);
                uloops_free(U);
                ret = 0;
                goto out;
            }
            /* outer to slot 0, then the holes, in loop order. */
            {
                ULV *tv = U->v[outer]; int tn = U->n[outer], tc = U->cap[outer]; double ta = U->area[outer];
                U->v[outer] = U->v[0]; U->n[outer] = U->n[0]; U->cap[outer] = U->cap[0]; U->area[outer] = U->area[0];
                U->v[0] = tv; U->n[0] = tn; U->cap[0] = tc; U->area[0] = ta;
            }
            for (k = 0; k < U->nloop; k++) {
                if (k == 0 || -U->area[k] > U_HOLE_MIN) {
                    if (w != k) {
                        U->v[w] = U->v[k]; U->n[w] = U->n[k]; U->cap[w] = U->cap[k]; U->area[w] = U->area[k];
                        U->v[k] = NULL; U->n[k] = 0; U->cap[k] = 0;
                    }
                    w++;
                } else {
                    free(U->v[k]); U->v[k] = NULL; U->n[k] = 0; U->cap[k] = 0;
                    st->dropped_loops++;
                }
            }
            U->nloop = w;
            for (k = 0; k < U->nloop; k++) u_simplify(U->v[k], &U->n[k]);
        }
        /* Fillets on the outer loop. */
        memset(&sc, 0, sizeof sc);
        memset(need, 0, (size_t)na * sizeof(double));
        cl->min_swept_margin = 1e30;
        cl->flags &= ~TD5_ROAM_CL_SWEPT_FAIL;
        nf = u_fillet(U->v[0], U->n[0], &L, P, W, LF, fixed, pr, na, need, &sc, cl, &fl);
        if (nf < 0) goto out;
#ifndef TD5RE_RELEASE
        {
            const char *dk = getenv("TD5RE_ROAM_UNION_DEBUG");
            if (dk && strtoll(dk, NULL, 10) == (long long)cl->key)
                for (m = 0; m < na; m++)
                    TD5_LOG_I(LOG_TAG, "ROAM_UNION_DBG round %d arm %d edge %llu: t %.2f m (base %.2f, "
                              "lim %.2f) fixed %d fillet need %.2f m", round, m,
                              (unsigned long long)at[m].ekey, t[m] / TD5_ROAM_UPM, tb[m] / TD5_ROAM_UPM,
                              at[m].lim / TD5_ROAM_UPM, fixed[m], need[m] / TD5_ROAM_UPM);
        }
#endif
        for (m = 0; m < na; m++)
            if (need[m] > 1.0 && t[m] < at[m].lim) {
                any = 1;
                t[m] = t[m] + need[m] + 0.5 * TD5_ROAM_UPM;
                if (t[m] > at[m].lim) t[m] = at[m].lim;
                if (t[m] > tb[m]) tb[m] = t[m];
            }
        if (any && round < 6) { free(fl); continue; }
        st->rounds = round + 1;
        if (u_simple(fl, nf)) {
            free(U->v[0]);
            U->v[0] = fl;
            U->n[0] = nf;
            U->cap[0] = nf;
        } else {
            free(fl);
        }
        c->swept_checked += sc.swept_checked;
        c->swept_fail += sc.swept_fail;
        c->swept_widened += sc.swept_widened;
        c->fillets += sc.fillets;
        c->fillet_shrunk += sc.fillet_shrunk;
        break;
    }
    if (!u_simple(U->v[0], U->n[0])) {
        TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: outer loop not simple", (long long)cl->key);
        uloops_free(U);
        ret = 0;
        goto out;
    }
#ifndef TD5RE_RELEASE
    {
        const char *dk = getenv("TD5RE_ROAM_UNION_DEBUG");
        if (dk && strtoll(dk, NULL, 10) == (long long)cl->key) {
            char path[400];
            const char *dir = td5_plat_log_dir();
            FILE *f;
            snprintf(path, sizeof path, "%sroam_union_loops.json", dir ? dir : "");
            f = fopen(path, "w");
            if (f) {
                fprintf(f, "{\"loops\": [");
                for (k = 0; k < U->nloop; k++) {
                    fprintf(f, "%s[", k ? ",\n" : "");
                    for (i = 0; i < U->n[k]; i++)
                        fprintf(f, "%s[%.4f, %.4f, %d]", i ? ", " : "", U->v[k][i].x, U->v[k][i].z, U->v[k][i].tag);
                    fprintf(f, "]");
                }
                fprintf(f, "]}\n");
                fclose(f);
            }
        }
    }
#endif
    for (m = 0; m < na; m++) {
        TD5_RoamPt row[TD5_ROAM_ROW_MAX];
        const int nrow = arm_cut_row(g, &at[m], t[m], row);
        if (!u_row_on_loop(U->v[0], U->n[0], m, nrow)) st->row_cov++;
        if (t[m] >= at[m].lim && at[m].a.t < at[m].lim) {
            cl->flags |= TD5_ROAM_CL_CLAMPED;
            c->short_edges_clamped++;
        }
        at[m].a.t = t[m];
    }
    st->holes = U->nloop - 1;
    st->fixed = 0;
    for (m = 0; m < na; m++) {
        int j, any = fixed[m];
        for (j = 0; j < na && !any; j++) any = pr[m * na + j];
        st->fixed += any;
    }
    ret = 1;
out:
    free(L.p); free(A.p); free(t); free(tb); free(need); free(fixed); free(push); free(nodes);
    free(pr); free(who); free(oth);
    return ret;
}

/* MODE_UNION, part 2 (after the rows are published): patch vertices with their
 * height recipes, boundary segments, triangles. Returns 1 when triangulated,
 * 0 when not, -1 on out of memory. */
static int union_emit(const TD5_RoamGraph *g, TD5_RoamCluster *cl, const ULoops *U, int na,
                      Patch *pt, TD5_RoamGraphCensus *c)
{
    const ULV *ov;
    int n0 = 0;
    int k, q, li, base, total = 0, ret = -1;
    int *seq = NULL, *hs = NULL, *ho = NULL, nseq;
    TD5_RoamPatchVert *tmp = NULL;
    LTri *tri = NULL;
    ULV *fv = (ULV *)malloc((size_t)(U->n[0] + 1) * sizeof(ULV));
    (void)na;
    pt->npv = pt->ntri = pt->nseg = 0;
    if (!fv) goto out;
    /* [F1-T9] The NORMAL outline's rule on the outer loop too: a non-cut vertex
     * closer than ROAM_OL_MIN_SEG to the last kept one (or, at the end, to the
     * first) is dropped, so no kerb wall comes out shorter than 1 cm (T7 (3) found
     * a 2-unit UNION kerb). Cut vertices are always kept: they are ribbon rows. */
    for (q = 0; q < U->n[0]; q++) {
        const ULV *p = &U->v[0][q];
        if (!p->tag && n0 > 0 &&
            hypot(p->x - fv[n0 - 1].x, p->z - fv[n0 - 1].z) < ROAM_OL_MIN_SEG) continue;
        if (!p->tag && q == U->n[0] - 1 && n0 > 0 &&
            hypot(p->x - fv[0].x, p->z - fv[0].z) < ROAM_OL_MIN_SEG) continue;
        fv[n0++] = *p;
    }
    if (n0 < 3) { ret = 0; goto out; }
    ov = fv;
    /* Outer loop: cut vertices exactly as published, the rest lerp by path
     * length between the cut rows on either side. */
    for (q = 0; q < n0; q++) {
        TD5_RoamPatchVert v;
        if (ov[q].tag) {
            const int m = U_TAG_ARM(ov[q].tag), r = U_TAG_LANE(ov[q].tag);
            const TD5_RoamArm *a = &g->arm[g->narm + m];
            const TD5_RoamEdgeEnd *en = &g->edge[a->edge].end[a->end];
            const int lanes = en->nrow - 1;
            const TD5_RoamPt *p = &en->row[a->end == 0 ? r : lanes - r];
            pv_init(&v, p->x, p->z, TD5_ROAM_PV_CUT);
            v.ref[0] = m;
            v.w[0] = 1.0;
        } else {
            double lb = 0.0, lf = 0.0;
            int ib = -1, jf = -1, s;
            pv_init(&v, ov[q].x + cl->cx, ov[q].z + cl->cz, TD5_ROAM_PV_TANGENT);
            for (s = 1; s < n0; s++) {
                const ULV *a = &ov[(q - s + 1 + n0) % n0], *b = &ov[(q - s + n0) % n0];
                lb += hypot(a->x - b->x, a->z - b->z);
                if (b->tag) { ib = U_TAG_ARM(b->tag); break; }
            }
            for (s = 1; s < n0; s++) {
                const ULV *a = &ov[(q + s - 1) % n0], *b = &ov[(q + s) % n0];
                lf += hypot(a->x - b->x, a->z - b->z);
                if (b->tag) { jf = U_TAG_ARM(b->tag); break; }
            }
            if (ib < 0 || jf < 0) {
                v.ref[0] = TD5_ROAM_HREF_CENTRE;
                v.w[0] = 1.0;
            } else if (ib == jf) {
                v.ref[0] = ib;
                v.w[0] = 1.0;
            } else {
                const double f = lb + lf > 0.0 ? lb / (lb + lf) : 0.5;
                v.ref[0] = ib < jf ? ib : jf;
                v.w[0] = ib < jf ? 1.0 - f : f;
                v.ref[1] = ib < jf ? jf : ib;
                v.w[1] = ib < jf ? f : 1.0 - f;
            }
        }
        if (patch_add_v(pt, &v) < 0) goto out;
    }
    for (q = 0; q < n0; q++) {
        const int q1 = (q + 1) % n0;
        int kind = TD5_ROAM_OL_KERB, arm = -1;
        if (ov[q].tag && ov[q1].tag && U_TAG_ARM(ov[q].tag) == U_TAG_ARM(ov[q1].tag) &&
            U_TAG_LANE(ov[q1].tag) == U_TAG_LANE(ov[q].tag) + 1) {
            kind = TD5_ROAM_OL_CUT;
            arm = g->narm + U_TAG_ARM(ov[q].tag);
        }
        if (!patch_add_seg(pt, q, q1, kind, arm)) goto out;
    }
    total = n0;
    /* Holes: kerbs at the cluster height. */
    for (li = 1; li < U->nloop; li++) {
        base = total;
        for (q = 0; q < U->n[li]; q++) {
            TD5_RoamPatchVert v;
            pv_init(&v, U->v[li][q].x + cl->cx, U->v[li][q].z + cl->cz, TD5_ROAM_PV_TANGENT);
            v.ref[0] = TD5_ROAM_HREF_CENTRE;
            v.w[0] = 1.0;
            if (patch_add_v(pt, &v) < 0) goto out;
        }
        for (q = 0; q < U->n[li]; q++)
            if (!patch_add_seg(pt, base + q, base + (q + 1) % U->n[li], TD5_ROAM_OL_KERB, -1)) goto out;
        total += U->n[li];
    }
    cl->nout = total;
    /* Fan from the centroid when there is no hole and it is valid. */
    if (U->nloop == 1 && fan_valid(pt->pv, n0, cl->cx, cl->cz)) {
        TD5_RoamPatchVert v;
        int ci;
        pv_init(&v, cl->cx, cl->cz, TD5_ROAM_PV_CENTRE);
        v.ref[0] = TD5_ROAM_HREF_CENTRE;
        v.w[0] = 1.0;
        ci = patch_add_v(pt, &v);
        if (ci < 0) goto out;
        for (q = 0; q < n0; q++)
            if (!patch_add_tri(pt, ci, q, (q + 1) % n0)) goto out;
        cl->flags |= TD5_ROAM_CL_FAN;
        c->fan_ok++;
        ret = 1;
        goto out;
    }
    c->fan_fallback++;
    /* Ear clipping; holes bridged into the outer ring first (largest x first,
     * to the nearest outer vertex the bridge can see). */
    seq = (int *)malloc((size_t)(total + 2 * U->nloop + 2) * sizeof(int));
    hs = (int *)malloc((size_t)(U->nloop + 1) * sizeof(int));
    ho = (int *)malloc((size_t)(U->nloop + 1) * sizeof(int));
    if (!seq || !hs || !ho) goto out;
    for (q = 0; q < n0; q++) seq[q] = q;
    nseq = n0;
    for (li = 1, base = n0; li < U->nloop; li++) { hs[li] = base; ho[li] = 0; base += U->n[li]; }
    for (k = 1; k < U->nloop; k++) {
        int best = -1, hv = -1, iv = -1, cand;
        double bx = -1e300;
        /* The unmerged hole with the largest x vertex. */
        for (li = 1; li < U->nloop; li++) {
            if (ho[li]) continue;
            for (q = 0; q < U->n[li]; q++) {
                const TD5_RoamPatchVert *p = &pt->pv[hs[li] + q];
                if (p->x > bx) { bx = p->x; best = li; hv = q; }
            }
        }
        if (best < 0) break;
        {
            const TD5_RoamPatchVert *H = &pt->pv[hs[best] + hv];
            double bd = 1e300;
            for (cand = 0; cand < nseq; cand++) {
                const TD5_RoamPatchVert *V = &pt->pv[seq[cand]];
                const double d = (V->x - H->x) * (V->x - H->x) + (V->z - H->z) * (V->z - H->z);
                int ok2, l2;
                if (d >= bd) continue;
                ok2 = pv_seg_clear(pt->pv, seq, nseq, H->x, H->z, V->x, V->z);
                for (l2 = 1; l2 < U->nloop && ok2; l2++) {
                    int ring[4096], nr = U->n[l2] < 4096 ? U->n[l2] : 4096, z2;
                    if (ho[l2]) continue;
                    for (z2 = 0; z2 < nr; z2++) ring[z2] = hs[l2] + z2;
                    ok2 = pv_seg_clear(pt->pv, ring, nr, H->x, H->z, V->x, V->z);
                }
                if (ok2) { bd = d; iv = cand; }
            }
        }
        if (iv < 0) {
            TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: no bridge to hole %d", (long long)cl->key, best);
            ret = 0;
            goto out;
        }
        {
            const int hn = U->n[best];
            memmove(&seq[iv + 1 + hn + 2], &seq[iv + 1], (size_t)(nseq - iv - 1) * sizeof(int));
            for (q = 0; q <= hn; q++) seq[iv + 1 + q] = hs[best] + (hv + q) % hn;
            seq[iv + 1 + hn + 1] = seq[iv];
            nseq += hn + 2;
        }
        ho[best] = 1;
    }
    tmp = (TD5_RoamPatchVert *)malloc((size_t)nseq * sizeof *tmp);
    tri = (LTri *)malloc((size_t)(nseq + 1) * sizeof *tri);
    if (!tmp || !tri) goto out;
    for (q = 0; q < nseq; q++) tmp[q] = pt->pv[seq[q]];
    {
        int nt = 0;
        double sum = 0.0, want = 0.0;
        if (!((U->nloop > 1 || outline_simple(tmp, nseq)) && ear_clip_u(tmp, nseq, tri, &nt)) || nt != nseq - 2) {
            TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: ear clipping failed (%d vertices, "
                      "%d hole(s), %d of %d triangles)", (long long)cl->key, nseq, U->nloop - 1, nt, nseq - 2);
            ret = 0;
            goto out;
        }
        /* Area of the rings as emitted (outer CCW +, holes CW -). */
        for (li = 0, base = 0; li < U->nloop; li++) {
            const int nl = li == 0 ? n0 : U->n[li];      /* outer ring as filtered */
            for (q = 0; q < nl; q++) {
                const TD5_RoamPatchVert *a = &pt->pv[base + q], *b = &pt->pv[base + (q + 1) % nl];
                want += 0.5 * cross2(a->x - cl->cx, a->z - cl->cz, b->x - cl->cx, b->z - cl->cz);
            }
            base += nl;
        }
        for (q = 0; q < nt; q++) {
            const TD5_RoamPatchVert *a = &tmp[tri[q].v[0]], *b = &tmp[tri[q].v[1]], *d = &tmp[tri[q].v[2]];
            sum += 0.5 * cross2(b->x - a->x, b->z - a->z, d->x - a->x, d->z - a->z);
        }
        /* Overlapping ears would cover more than the outline. */
        if (fabs(sum - want) > 1e-6 * want + 100.0) {
            TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION_FAIL cluster %lld: triangles %.0f m2 vs outline %.0f m2",
                      (long long)cl->key, sum / (TD5_ROAM_UPM * TD5_ROAM_UPM), want / (TD5_ROAM_UPM * TD5_ROAM_UPM));
            ret = 0;
            goto out;
        }
        for (q = 0; q < nt; q++)
            if (!patch_add_tri(pt, seq[tri[q].v[0]], seq[tri[q].v[1]], seq[tri[q].v[2]])) goto out;
    }
    cl->flags |= TD5_ROAM_CL_EARCLIP;
    ret = 1;
out:
    free(seq); free(hs); free(ho); free(tmp); free(tri); free(fv);
    return ret;
}

/* ------------------------------------------------------------ the builder --- */

/* Per-node patch mode (set by the fallback passes of td5_roam_junction_build). */
#define MODE_NORMAL 0   /* fillets, cuts from the tangent points             */
#define MODE_SIMPLE 1   /* no corners: cut rows beyond every member node,
                         * joined by straight kerbs                          */
#define MODE_SPLIT  2   /* never merged with another node                    */
#define MODE_UNION  3   /* outline = union of the paved pieces (T3b)         */
/* [F1-T9] A SPLIT node that still fails as a one-node NORMAL patch walks the same
 * fallbacks, still never merged: UNION, then SIMPLE (+ the one-node hull). */
#define MODE_SPLIT_UNION  4
#define MODE_SPLIT_SIMPLE 5
#define MODE_IS_SPLIT(m)  ((m) == MODE_SPLIT || (m) == MODE_SPLIT_UNION || (m) == MODE_SPLIT_SIMPLE)
#define MODE_IS_SIMPLE(m) ((m) == MODE_SIMPLE || (m) == MODE_SPLIT_SIMPLE)
#define MODE_IS_UNION(m)  ((m) == MODE_UNION || (m) == MODE_SPLIT_UNION)

static int junction_pass(TD5_RoamGraph *g, const unsigned char *mode)
{
    TD5_RoamGraphCensus *c = &g->census;
    const TD5_RoamGraphParams *P = &g->prm;
    const int nn = g->nnode;
    const double W = 2.0 * P->body_half_w, LF = P->body_lf;
    const double sin_par = sin(P->parallel_deg * ROAM_PI / 180.0);
    const double par_rad = P->parallel_deg * ROAM_PI / 180.0;
    int *uf = NULL, *cuf = NULL, *nxt = NULL, *tail = NULL;
    double *diam = NULL, *rdisc = NULL;
    RoamPair *pr = NULL;
    int npr = 0, cap_pr = 0;
    RoamXs *xs = NULL;
    int nxs = 0;
    RoamCaps caps;
    ArmTmp *at = NULL;
    int cap_at = 0;
    Corner *cor = NULL;
    int cap_cor = 0;
    Patch pt;
    ULoops ul;
    UStat ust;
    int i, j, k, ok = 0;

    memset(&caps, 0, sizeof caps);
    memset(&pt, 0, sizeof pt);
    memset(&ul, 0, sizeof ul);
    if (nn <= 0) return 1;
    uf   = (int *)malloc((size_t)nn * sizeof(int));
    cuf  = (int *)malloc((size_t)nn * sizeof(int));
    nxt  = (int *)malloc((size_t)nn * sizeof(int));
    tail = (int *)malloc((size_t)nn * sizeof(int));
    diam = (double *)calloc((size_t)nn, sizeof(double));
    rdisc = (double *)calloc((size_t)nn, sizeof(double));
    xs   = (RoamXs *)malloc((size_t)nn * sizeof(RoamXs));
    if (!uf || !cuf || !nxt || !tail || !diam || !rdisc || !xs) goto done;
    for (i = 0; i < nn; i++) { uf[i] = cuf[i] = tail[i] = i; nxt[i] = -1; }

    /* 1-2. Candidate pairs: short connections (an edge, or a chain of pieces
     * through JOIN nodes, shorter than merge_len), overlapping discs. */
    {
        double rmax = 0.0;
        for (i = 0; i < nn; i++) {
            const TD5_RoamNode *nd = &g->node[i];
            if (!is_cand(nd) || MODE_IS_SPLIT(mode[i])) continue;
            for (k = 0; k < nd->nend; k++) {
                const TD5_RoamEndRef *r0 = &g->node_end[nd->end_first + k];
                int ce = r0->edge, cq = r0->end, steps = 0, fnode;
                double len = 0.0;
                RoamPair q;
                for (;;) {
                    const TD5_RoamEdge *e = &g->edge[ce];
                    const TD5_RoamNode *fnd;
                    const TD5_RoamEndRef *fr;
                    len += e->length;
                    fnode = cq == 0 ? e->node_b : e->node_a;
                    fnd = &g->node[fnode];
                    if (len >= P->merge_len || fnd->kind != TD5_ROAM_NODE_JOIN || fnd->nend != 2 ||
                        ++steps >= ROAM_CHAIN_MAX) break;
                    fr = &g->node_end[fnd->end_first];
                    if (fr[0].edge == ce && fr[0].end == 1 - cq) { ce = fr[1].edge; cq = fr[1].end; }
                    else { ce = fr[0].edge; cq = fr[0].end; }
                }
                /* Each pair is found from both ends; keep the one from the lower
                 * index so the list does not depend on which end walked. */
                if (len >= P->merge_len || fnode <= i || !is_cand(&g->node[fnode]) ||
                    MODE_IS_SPLIT(mode[fnode])) continue;
                q.a = i;
                q.b = fnode;
                q.kind = PAIR_EDGE;
                if (!grow((void **)&pr, &cap_pr, npr + 1, sizeof *pr)) goto done;
                pr[npr++] = q;
            }
        }
        for (i = 0; i < nn; i++) {
            if (!is_cand(&g->node[i])) continue;
            rdisc[i] = node_max_hw(g, &g->node[i]) + P->disc_pad;
            if (rdisc[i] > rmax) rmax = rdisc[i];
            xs[nxs].x = g->node[i].x;
            xs[nxs].n = i;
            nxs++;
        }
        qsort(xs, (size_t)nxs, sizeof *xs, xs_cmp);
        for (i = 0; i < nxs; i++) {
            const TD5_RoamNode *a = &g->node[xs[i].n];
            for (j = i + 1; j < nxs && xs[j].x - xs[i].x <= rdisc[xs[i].n] + rmax; j++) {
                const TD5_RoamNode *b = &g->node[xs[j].n];
                const double dx = b->x - a->x, dz = b->z - a->z;
                const double rr = rdisc[xs[i].n] + rdisc[xs[j].n];
                RoamPair q;
                if (dx * dx + dz * dz >= rr * rr) continue;
                if (MODE_IS_SPLIT(mode[xs[i].n]) || MODE_IS_SPLIT(mode[xs[j].n])) continue;
                if (a->layer_max < b->layer_min || b->layer_max < a->layer_min) {
                    c->rejected_layer++;
                    continue;
                }
                q.a = xs[i].n < xs[j].n ? xs[i].n : xs[j].n;
                q.b = xs[i].n < xs[j].n ? xs[j].n : xs[i].n;
                q.kind = PAIR_DISC;
                if (!grow((void **)&pr, &cap_pr, npr + 1, sizeof *pr)) goto done;
                pr[npr++] = q;
            }
        }
        qsort(pr, (size_t)npr, sizeof *pr, pair_cmp);
        /* Merge duplicates (EDGE wins the label). */
        for (i = 0, j = 0; i < npr; i++) {
            if (j > 0 && pr[j - 1].a == pr[i].a && pr[j - 1].b == pr[i].b) {
                pr[j - 1].kind |= pr[i].kind;
                continue;
            }
            pr[j++] = pr[i];
        }
        npr = j;
    }

    /* 3. Unions in sorted (key, key) order with the diameter cap. */
    for (i = 0; i < npr; i++) {
        const int ra = uf_find(uf, pr[i].a), rb = uf_find(uf, pr[i].b);
        int ca = uf_find(cuf, pr[i].a), cb = uf_find(cuf, pr[i].b);
        double nd;
        int x, y;
        if (ca != cb) { if (ca < cb) cuf[cb] = ca; else cuf[ca] = cb; }
        if (ra == rb) continue;
        nd = diam[ra] > diam[rb] ? diam[ra] : diam[rb];
        for (x = ra; x >= 0; x = nxt[x])
            for (y = rb; y >= 0; y = nxt[y]) {
                const double dx = g->node[x].x - g->node[y].x, dz = g->node[x].z - g->node[y].z;
                const double d = sqrt(dx * dx + dz * dz);
                if (d > nd) nd = d;
            }
        if (nd > P->cap_diameter) {
            c->rejected_unions++;
            TD5_LOG_I(LOG_TAG, "ROAM_UNION_REFUSED nodes %lld + %lld: diameter %.1f m > %.0f m",
                      (long long)g->node[pr[i].a].key, (long long)g->node[pr[i].b].key,
                      nd / TD5_ROAM_UPM, P->cap_diameter / TD5_ROAM_UPM);
            continue;
        }
        {
            const int root = ra < rb ? ra : rb, other = ra < rb ? rb : ra;
            uf[other] = root;
            nxt[tail[root]] = other;
            tail[root] = tail[other];
            diam[root] = nd;
        }
        if (pr[i].kind & PAIR_EDGE) c->unions_edge++; else c->unions_disc++;
    }
    /* Largest proximity component (no cap): bounds the halo a tile build needs. */
    {
        double *bb = (double *)malloc((size_t)nn * 4 * sizeof(double));
        if (!bb) goto done;
        for (i = 0; i < nn; i++) bb[4 * i] = 1e300;
        for (i = 0; i < nn; i++) {
            const int r = uf_find(cuf, i);
            const TD5_RoamNode *nd = &g->node[i];
            if (!is_cand(nd)) continue;
            if (bb[4 * r] == 1e300) {
                bb[4 * r] = bb[4 * r + 2] = nd->x;
                bb[4 * r + 1] = bb[4 * r + 3] = nd->z;
            }
            if (nd->x < bb[4 * r]) bb[4 * r] = nd->x;
            if (nd->z < bb[4 * r + 1]) bb[4 * r + 1] = nd->z;
            if (nd->x > bb[4 * r + 2]) bb[4 * r + 2] = nd->x;
            if (nd->z > bb[4 * r + 3]) bb[4 * r + 3] = nd->z;
        }
        for (i = 0; i < nn; i++)
            if (bb[4 * i] != 1e300) {
                const double dx = bb[4 * i + 2] - bb[4 * i], dz = bb[4 * i + 3] - bb[4 * i + 1];
                const double d = sqrt(dx * dx + dz * dz);
                if (d > c->max_component) c->max_component = d;
            }
        free(bb);
    }

    /* 4-5. Clusters, in root (= smallest key) order. */
    for (i = 0; i < nn; i++) {
        TD5_RoamCluster *cl;
        int m, cnt = 0;
        double sx = 0.0, sz = 0.0;
        if (!is_cand(&g->node[i]) || uf_find(uf, i) != i) continue;
        if (!grow((void **)&g->cluster, &caps.cap_cluster, g->ncluster + 1, sizeof *g->cluster))
            goto done;
        cl = &g->cluster[g->ncluster];
        memset(cl, 0, sizeof *cl);
        cl->key = g->node[i].key;
        cl->node_first = g->ncluster_node;
        for (m = i; m >= 0; m = nxt[m]) cnt++;
        if (!grow((void **)&g->cluster_node, &caps.cap_cnode, g->ncluster_node + cnt,
                  sizeof *g->cluster_node)) goto done;
        for (m = i; m >= 0; m = nxt[m]) g->cluster_node[g->ncluster_node++] = m;
        /* Members sorted by index = by key. */
        {
            int *mm = &g->cluster_node[cl->node_first];
            int a, b;
            for (a = 1; a < cnt; a++)
                for (b = a; b > 0 && mm[b] < mm[b - 1]; b--) {
                    const int t = mm[b]; mm[b] = mm[b - 1]; mm[b - 1] = t;
                }
            for (a = 0; a < cnt; a++) {
                sx += g->node[mm[a]].x;
                sz += g->node[mm[a]].z;
                g->node[mm[a]].cluster = g->ncluster;
            }
        }
        cl->nnode = cnt;
        cl->cx = sx / (double)cnt;
        cl->cz = sz / (double)cnt;
        cl->diameter = diam[i];
        if (cnt == 1 && g->node[i].kind == TD5_ROAM_NODE_TAPER) cl->flags |= TD5_ROAM_CL_TAPER;
        if (cnt == 1 && g->node[i].kind == TD5_ROAM_NODE_BEND)  cl->flags |= TD5_ROAM_CL_BEND;
        if (cnt > 1) c->multi_node_clusters++;
        if (cl->diameter > c->max_diameter) {
            c->max_diameter = cl->diameter;
            c->max_diameter_key = cl->key;
        }
        g->ncluster++;
    }
    c->clusters = g->ncluster;

    /* Internal edges: a connection (one edge, or a chain of pieces through JOIN
     * nodes) from a member node to another member node of the same cluster,
     * every point near the centroid. */
    for (i = 0; i < g->ncluster; i++) {
        const TD5_RoamCluster *cl = &g->cluster[i];
        double rc = 0.0, lim;
        int m, q;
        for (q = 0; q < cl->nnode; q++) {
            const TD5_RoamNode *nd = &g->node[g->cluster_node[cl->node_first + q]];
            const double dx = nd->x - cl->cx, dz = nd->z - cl->cz;
            const double d = sqrt(dx * dx + dz * dz);
            if (d > rc) rc = d;
        }
        lim = rc + 10.0 * TD5_ROAM_UPM;
        for (m = 0; m < cl->nnode; m++) {
            const int ni = g->cluster_node[cl->node_first + m];
            const TD5_RoamNode *nd = &g->node[ni];
            for (k = 0; k < nd->nend; k++) {
                const TD5_RoamEndRef *r0 = &g->node_end[nd->end_first + k];
                int pe[ROAM_CHAIN_MAX], np = 0, ce = r0->edge, cq = r0->end, fnode, inside = 1, z2;
                for (;;) {
                    const TD5_RoamEdge *e = &g->edge[ce];
                    const TD5_RoamNode *fnd;
                    const TD5_RoamEndRef *fr;
                    pe[np++] = ce;
                    fnode = cq == 0 ? e->node_b : e->node_a;
                    fnd = &g->node[fnode];
                    if (fnd->kind != TD5_ROAM_NODE_JOIN || fnd->nend != 2 || np >= ROAM_CHAIN_MAX) break;
                    fr = &g->node_end[fnd->end_first];
                    if (fr[0].edge == ce && fr[0].end == 1 - cq) { ce = fr[1].edge; cq = fr[1].end; }
                    else { ce = fr[0].edge; cq = fr[0].end; }
                }
                if (fnode == ni || g->node[fnode].cluster != i) continue;
                for (z2 = 0; z2 < np && inside; z2++) {
                    const TD5_RoamEdge *e = &g->edge[pe[z2]];
                    for (q = 0; q < e->npt && inside; q++) {
                        const double dx = g->pt[e->pt_first + q].x - cl->cx;
                        const double dz = g->pt[e->pt_first + q].z - cl->cz;
                        if (dx * dx + dz * dz > lim * lim) inside = 0;
                    }
                }
                if (!inside) continue;
                for (z2 = 0; z2 < np; z2++) {
                    TD5_RoamEdge *e = &g->edge[pe[z2]];
                    if (e->end[0].kind == TD5_ROAM_END_INTERNAL) continue;   /* seen from the other end */
                    for (q = 0; q < 2; q++) {
                        e->end[q].kind = TD5_ROAM_END_INTERNAL;
                        e->end[q].cluster = i;
                        e->end[q].arm = -1;
                        e->end[q].nrow = 0;
                        e->end[q].s = q == 0 ? 0.0 : e->length;
                    }
                    c->internal_edges++;
                }
            }
        }
    }

    /* 6. Per cluster: arms, corners, cuts, rows, outline, triangles. */
    for (i = 0; i < g->ncluster; i++) {
        TD5_RoamCluster *cl = &g->cluster[i];
        const int simple = MODE_IS_SIMPLE(mode[g->cluster_node[cl->node_first]]);
        const int umode = MODE_IS_UNION(mode[g->cluster_node[cl->node_first]]);
        double simple_hw = 0.0;
        int na = 0, m, q, ugood = 0;
        cl->layer = 127;
        cl->min_swept_margin = 1e30;
        /* Arms. */
        for (m = 0; m < cl->nnode; m++) {
            const int ni = g->cluster_node[cl->node_first + m];
            const TD5_RoamNode *nd = &g->node[ni];
            for (q = 0; q < nd->nend; q++) {
                const TD5_RoamEndRef *r = &g->node_end[nd->end_first + q];
                const TD5_RoamEdge *e = &g->edge[r->edge];
                ArmTmp *a;
                double d, px, pz, dx, dz, len;
                if (e->end[r->end].kind == TD5_ROAM_END_INTERNAL) continue;
                if (!grow((void **)&at, &cap_at, na + 1, sizeof *at)) goto done;
                a = &at[na++];
                memset(a, 0, sizeof *a);
                a->a.edge = r->edge;
                a->a.end = r->end;
                a->a.node = ni;
                a->a.half_w = e->half_w;
                a->ekey = e->key;
                a->kx = nd->x;
                a->kz = nd->z;
                a->klass = e->klass;
                /* The chain through JOIN nodes. */
                a->nchain = 1;
                a->ce[0] = r->edge;
                a->cq[0] = r->end;
                a->ccum[0] = 0.0;
                a->ccum[1] = e->length;
                while (a->nchain < ROAM_CHAIN_MAX && a->ccum[a->nchain] < ROAM_CHAIN_LEN) {
                    const TD5_RoamEdge *pe = &g->edge[a->ce[a->nchain - 1]];
                    const int far_end = 1 - a->cq[a->nchain - 1];
                    const int fn = far_end == 0 ? pe->node_a : pe->node_b;
                    const TD5_RoamNode *fnd = &g->node[fn];
                    const TD5_RoamEndRef *fr = &g->node_end[fnd->end_first];
                    int nxt_e, nxt_q, z2, dup = 0;
                    if (fnd->kind != TD5_ROAM_NODE_JOIN || fnd->nend != 2) break;
                    if (fr[0].edge == a->ce[a->nchain - 1] && fr[0].end == far_end) {
                        nxt_e = fr[1].edge; nxt_q = fr[1].end;
                    } else {
                        nxt_e = fr[0].edge; nxt_q = fr[0].end;
                    }
                    for (z2 = 0; z2 < a->nchain; z2++) if (a->ce[z2] == nxt_e) dup = 1;
                    if (dup) break;
                    a->ce[a->nchain] = nxt_e;
                    a->cq[a->nchain] = nxt_q;
                    a->ccum[a->nchain + 1] = a->ccum[a->nchain] + g->edge[nxt_e].length;
                    a->nchain++;
                }
                a->len = a->ccum[a->nchain];
                a->lim = 0.5 * a->len;
                d = P->arm_dir_len < a->lim ? P->arm_dir_len : a->lim;
                chain_point(g, a, d, &px, &pz);
                dx = px - nd->x;
                dz = pz - nd->z;
                len = sqrt(dx * dx + dz * dz);
                if (len < 1e-9) { dx = 1.0; dz = 0.0; len = 1.0; }
                a->a.dir_x = dx / len;
                a->a.dir_z = dz / len;
                if (fabs(px - cl->cx) + fabs(pz - cl->cz) > 1e-6)
                    a->a.ang = atan2(pz - cl->cz, px - cl->cx);
                else
                    a->a.ang = atan2(a->a.dir_z, a->a.dir_x);
                if (e->layer < cl->layer) cl->layer = e->layer;
            }
        }
        if (cl->layer == 127) cl->layer = 0;
        for (m = 0; m < na; m++) if (at[m].a.half_w > simple_hw) simple_hw = at[m].a.half_w;
        if (simple) cl->flags |= TD5_ROAM_CL_SIMPLE;
        qsort(at, (size_t)na, sizeof *at, arm_cmp);
        c->cluster_by_arms[na < TD5_ROAM_ARM_HIST ? na : TD5_ROAM_ARM_HIST - 1]++;
        if (na == 0) {
            cl->flags |= TD5_ROAM_CL_FAIL;
            c->clusters_no_arm++;
            continue;
        }
        if (!grow((void **)&cor, &cap_cor, na, sizeof *cor)) goto done;

        /* Corners (arm a's left with arm a+1's right). */
        for (m = 0; m < na; m++) {
            const ArmTmp *ai = &at[m], *aj = &at[(m + 1) % na];
            Corner *co = &cor[m];
            const double dix = ai->a.dir_x, diz = ai->a.dir_z, djx = aj->a.dir_x, djz = aj->a.dir_z;
            const double lix = ai->kx + ai->a.half_w * -diz, liz = ai->kz + ai->a.half_w * dix;
            const double rjx = aj->kx - aj->a.half_w * -djz, rjz = aj->kz - aj->a.half_w * djx;
            const double cr = cross2(dix, diz, djx, djz);
            double gap = na == 1 ? 2.0 * ROAM_PI : aj->a.ang - ai->a.ang;
            int parallel, toofar = 1;
            memset(co, 0, sizeof *co);
            if (simple || umode) { co->type = CORNER_DIRECT; continue; }
            while (gap <= 0.0) gap += 2.0 * ROAM_PI;
            parallel = fabs(cr) < sin_par;
            if (!parallel) {
                const double Dx = rjx - lix, Dz = rjz - liz;
                const double aa = cross2(Dx, Dz, djx, djz) / cr;
                co->cx = lix + aa * dix;
                co->cz = liz + aa * diz;
                toofar = hypot(co->cx - cl->cx, co->cz - cl->cz) > P->corner_max;
            }
            if (gap < ROAM_PI - par_rad) {
                if (parallel) { co->type = CORNER_DIRECT; c->corners_parallel++; }
                else if (toofar) { co->type = CORNER_DIRECT; c->corners_far++; }
                else co->type = CORNER_FILLET;
            } else if (gap > ROAM_PI + par_rad) {
                if (!parallel && !toofar) { co->type = CORNER_REFLEX; c->corners_reflex++; }
                else {
                    const double bang = ai->a.ang + 0.5 * gap;
                    const double bw = ai->a.half_w > aj->a.half_w ? ai->a.half_w : aj->a.half_w;
                    co->type = CORNER_BEVEL;
                    co->bx = cl->cx + cos(bang) * bw;
                    co->bz = cl->cz + sin(bang) * bw;
                    c->bevels++;
                }
            } else {
                co->type = CORNER_DIRECT;
                c->corners_parallel++;
            }
            if (co->type == CORNER_FILLET) {
                const double cosang = dix * djx + diz * djz;
                const double th = acos(cosang < -1.0 ? -1.0 : (cosang > 1.0 ? 1.0 : cosang));
                const double tanh2 = tan(0.5 * th);
                const double corridor = 2.0 * (ai->a.half_w < aj->a.half_w ? ai->a.half_w : aj->a.half_w);
                const double pci = (co->cx - ai->kx) * dix + (co->cz - ai->kz) * diz;
                const double pcj = (co->cx - aj->kx) * djx + (co->cz - aj->kz) * djz;
                double r = (ai->klass >= TD5_GEO_RC_PRIMARY || aj->klass >= TD5_GEO_RC_PRIMARY)
                         ? P->kerb_r_major : P->kerb_r_minor;
                double tlmax, tl, need;
                int widened = 0;
                co->theta = th;
                c->swept_checked++;
                if (swept_need(r, W, LF) > corridor && corridor > W) {
                    const double rq = (W * W + LF * LF - corridor * corridor) / (2.0 * (corridor - W));
                    if (rq > r) { r = rq; widened = 1; }
                }
                /* The tangent points must stay inside both arms' cut limits. */
                tlmax = ai->lim - pci;
                if (aj->lim - pcj < tlmax) tlmax = aj->lim - pcj;
                tl = tanh2 > 1e-9 ? r / tanh2 : 1e30;
                if (tl > tlmax) {
                    if (tlmax < 0.0) tlmax = 0.0;
                    r = tlmax * tanh2;
                    tl = tlmax;
                    c->fillet_shrunk++;
                }
                co->r = r;
                co->tix = co->cx + dix * tl;
                co->tiz = co->cz + diz * tl;
                co->tjx = co->cx + djx * tl;
                co->tjz = co->cz + djz * tl;
                {
                    double bx = dix + djx, bz = diz + djz;
                    const double bl = sqrt(bx * bx + bz * bz);
                    const double sh = sin(0.5 * th);
                    bx = bl > 1e-12 ? bx / bl : -diz;
                    bz = bl > 1e-12 ? bz / bl : dix;
                    co->ox = co->cx + bx * (sh > 1e-9 ? r / sh : 0.0);
                    co->oz = co->cz + bz * (sh > 1e-9 ? r / sh : 0.0);
                }
                need = swept_need(r, W, LF);
                co->margin = corridor - need;
                if (co->margin < cl->min_swept_margin) cl->min_swept_margin = co->margin;
                if (need > corridor) {
                    co->swept_fail = 1;
                    cl->flags |= TD5_ROAM_CL_SWEPT_FAIL;
                    c->swept_fail++;
                    TD5_LOG_W(LOG_TAG, "ROAM_SWEPT_FAIL cluster %lld arms %llu/%d -> %llu/%d: "
                              "corridor %.2f m, need %.2f m (fillet %.2f m)",
                              (long long)cl->key, (unsigned long long)ai->ekey, ai->a.end,
                              (unsigned long long)aj->ekey, aj->a.end,
                              corridor / TD5_ROAM_UPM, need / TD5_ROAM_UPM, r / TD5_ROAM_UPM);
                } else if (widened) {
                    c->swept_widened++;
                }
                c->fillets++;
            }
        }

        /* Cuts: past every tangent point on the arm, >= half width, <= half the edge. */
        for (m = 0; m < na; m++) {
            ArmTmp *a = &at[m];
            const Corner *left = &cor[m], *right = &cor[(m + na - 1) % na];
            double t = a->a.half_w;
            if (simple) {
                /* The cut row lies beyond every member node by the widest
                 * half width, so no arm's row cuts across the core. */
                int z2;
                for (z2 = 0; z2 < cl->nnode; z2++) {
                    const TD5_RoamNode *mn = &g->node[g->cluster_node[cl->node_first + z2]];
                    const double p = (mn->x - a->kx) * a->a.dir_x + (mn->z - a->kz) * a->a.dir_z
                                   + simple_hw;
                    if (p > t) t = p;
                }
            }
            if (left->type == CORNER_FILLET) {
                const double p = (left->tix - a->kx) * a->a.dir_x + (left->tiz - a->kz) * a->a.dir_z;
                if (p > t) t = p;
            }
            if (right->type == CORNER_FILLET) {
                const double p = (right->tjx - a->kx) * a->a.dir_x + (right->tjz - a->kz) * a->a.dir_z;
                if (p > t) t = p;
            }
            if (t >= a->lim) {
                t = a->lim;
                cl->flags |= TD5_ROAM_CL_CLAMPED;
                c->short_edges_clamped++;
            }
            a->a.t = t;
            a->a.kerb_r = left->type == CORNER_FILLET ? left->r : 0.0;
            a->a.corner = left->type;
        }

        /* UNION: the cuts are pushed out until the outline holds them. */
        if (umode) {
            const int ur = union_solve(g, i, cl, at, na, c, &ul, &ust);
            if (ur < 0) goto done;
            ugood = ur;
            cl->flags |= TD5_ROAM_CL_UNION;
        }

        /* Publish arms + the CLUSTER edge ends (rows). */
        cl->arm_first = g->narm;
        cl->narm = na;
        if (!grow((void **)&g->arm, &caps.cap_arm, g->narm + na, sizeof *g->arm)) goto done;
        for (m = 0; m < na; m++) {
            ArmTmp *am = &at[m];
            TD5_RoamEdge *e;
            TD5_RoamEdgeEnd *en;
            double loc;
            int kk = 0, z2;
            /* The chain piece that holds the cut; the pieces before it are
             * under the patch and are absorbed. */
            while (kk + 1 < am->nchain && am->a.t >= am->ccum[kk + 1]) kk++;
            for (z2 = 0; z2 < kk; z2++) {
                TD5_RoamEdge *ae = &g->edge[am->ce[z2]];
                int qq;
                for (qq = 0; qq < 2; qq++) {
                    ae->end[qq].kind = TD5_ROAM_END_INTERNAL;
                    ae->end[qq].cluster = i;
                    ae->end[qq].arm = -1;
                    ae->end[qq].nrow = 0;
                    ae->end[qq].cut = 0.0;
                    ae->end[qq].s = qq == 0 ? 0.0 : ae->length;
                }
                c->chain_absorbed++;
            }
            am->a.edge = am->ce[kk];
            am->a.end = am->cq[kk];
            e = &g->edge[am->a.edge];
            en = &e->end[am->a.end];
            loc = am->a.t - am->ccum[kk];
            g->arm[g->narm + m] = am->a;
            en->kind = TD5_ROAM_END_CLUSTER;
            en->cluster = i;
            en->arm = g->narm + m;
            en->cut = loc;
            /* A one-piece arm clamped to half its edge stores exactly 0.5 * L,
             * the value the opposite end stores too (L - L/2 is exact). */
            en->s = am->a.end == 0 ? loc
                  : ((am->nchain == 1 && am->a.t == am->lim) ? am->lim : e->length - loc);
            en->nrow = e->lanes + 1;
            td5_roam_edge_row(g, e, en->s, en->row, &en->dir_x, &en->dir_z);
            for (q = 0; q < en->nrow; q++) {
                en->rowq[q][0] = td5_roam_quant(en->row[q].x);
                en->rowq[q][1] = td5_roam_quant(en->row[q].z);
            }
        }

        if (umode) {
            if (ugood) {
                const int er = union_emit(g, cl, &ul, na, &pt, c);
                if (er < 0) goto done;
                ugood = er;
            }
            uloops_free(&ul);
            if (ugood) {
                if (ust.row_cov) cl->flags |= TD5_ROAM_CL_ROW_COVERED;
                if (ust.holes) cl->flags |= TD5_ROAM_CL_HOLE;
                TD5_LOG_I(LOG_TAG, "ROAM_PATCH_UNION cluster %lld (%d node(s), %d arm(s)): %d push step(s), "
                          "%d restart(s), %d fillet round(s), %d arm(s) left overlapping, %d hole(s), "
                          "%d row(s) off the outline, %d sliver loop(s) dropped, %d dangling sliver "
                          "segment(s) pruned",
                          (long long)cl->key, cl->nnode, na, ust.iters, ust.restarts, ust.rounds,
                          ust.fixed, ust.holes, ust.row_cov, ust.dropped_loops, ust.pruned);
            } else {
                pt.npv = pt.ntri = pt.nseg = 0;
                cl->nout = 0;
                cl->flags |= TD5_ROAM_CL_FAIL;
                c->earclip_fail++;
                TD5_LOG_W(LOG_TAG, "ROAM_PATCH_FAIL cluster %lld (%d arm(s)): no valid union outline",
                          (long long)cl->key, na);
            }
            goto patch_done;
        }

        /* Outline, CCW. */
        pt.npv = pt.ntri = pt.nseg = 0;
        {
            int first_cut_right = -1;
            for (m = 0; m < na; m++) {
                const TD5_RoamArm *a = &g->arm[g->narm + m];
                const TD5_RoamEdge *e = &g->edge[a->edge];
                const TD5_RoamEdgeEnd *en = &e->end[a->end];
                const int lanes = en->nrow - 1;
                const Corner *co = &cor[m];
                const TD5_RoamArm *aj = &g->arm[g->narm + (m + 1) % na];
                const TD5_RoamEdgeEnd *enj = &g->edge[aj->edge].end[aj->end];
                TD5_RoamPt nextpt, qp[16];
                int nq = 0, cut_left = -1, prev, r;
                double plen[18], tot;
                /* Cut row, right kerb -> left kerb in the arm's outward sense. */
                for (r = 0; r <= lanes; r++) {
                    const int idx = a->end == 0 ? r : lanes - r;
                    TD5_RoamPatchVert v;
                    int vi;
                    pv_init(&v, en->row[idx].x, en->row[idx].z, TD5_ROAM_PV_CUT);
                    v.ref[0] = m;
                    v.w[0] = 1.0;
                    vi = patch_add_v(&pt, &v);
                    if (vi < 0) goto done;
                    if (r == 0 && m == 0) first_cut_right = vi;
                    if (r > 0 && !patch_add_seg(&pt, vi - 1, vi, TD5_ROAM_OL_CUT, g->narm + m))
                        goto done;
                    cut_left = vi;
                }
                /* The next arm's right kerb point. */
                {
                    const int lj = enj->nrow - 1;
                    nextpt = enj->row[aj->end == 0 ? 0 : lj];
                }
                /* Intermediate points of this corner. */
                if (co->type == CORNER_FILLET) {
                    const int segs = P->fillet_segs < 1 ? 1 : (P->fillet_segs > 12 ? 12 : P->fillet_segs);
                    const double a0 = atan2(co->tiz - co->oz, co->tix - co->ox);
                    double a1 = atan2(co->tjz - co->oz, co->tjx - co->ox);
                    double da;
                    int s;
                    /* Short way round (the arc is less than 180 deg). */
                    da = a1 - a0;
                    while (da > ROAM_PI) da -= 2.0 * ROAM_PI;
                    while (da < -ROAM_PI) da += 2.0 * ROAM_PI;
                    qp[nq].x = co->tix; qp[nq].z = co->tiz; nq++;
                    for (s = 1; s < segs; s++) {
                        const double an = a0 + da * (double)s / (double)segs;
                        qp[nq].x = co->ox + co->r * cos(an);
                        qp[nq].z = co->oz + co->r * sin(an);
                        nq++;
                    }
                    qp[nq].x = co->tjx; qp[nq].z = co->tjz; nq++;
                    (void)a1;
                } else if (co->type == CORNER_REFLEX) {
                    qp[nq].x = co->cx; qp[nq].z = co->cz; nq++;
                } else if (co->type == CORNER_BEVEL) {
                    qp[nq].x = co->bx; qp[nq].z = co->bz; nq++;
                }
                if (na == 1) {
                    /* One arm: close behind the node with a square cap. */
                    nq = 0;
                    qp[nq].x = a->dir_x * -a->half_w + -a->dir_z * a->half_w + g->node[a->node].x;
                    qp[nq].z = a->dir_z * -a->half_w + a->dir_x * a->half_w + g->node[a->node].z;
                    nq++;
                    qp[nq].x = a->dir_x * -a->half_w - -a->dir_z * a->half_w + g->node[a->node].x;
                    qp[nq].z = a->dir_z * -a->half_w - a->dir_x * a->half_w + g->node[a->node].z;
                    nq++;
                }
                /* Drop points too close to a neighbour, then lerp the recipe by
                 * path length from cut-left(m) to cut-right(m+1). */
                {
                    TD5_RoamPt kept[16];
                    int nk = 0, s;
                    TD5_RoamPt last = en->row[a->end == 0 ? lanes : 0];
                    /* Each arm's cut row as a line with its outward normal: a
                     * corner point past either row (a curved arm bends away from
                     * its straight kerb line) would fold the outline back. */
                    const double oi = a->end == 0 ? 1.0 : -1.0, oj = aj->end == 0 ? 1.0 : -1.0;
                    const int lj = enj->nrow - 1;
                    const double ri_x = 0.5 * (en->row[0].x + en->row[lanes].x);
                    const double ri_z = 0.5 * (en->row[0].z + en->row[lanes].z);
                    const double rj_x = 0.5 * (enj->row[0].x + enj->row[lj].x);
                    const double rj_z = 0.5 * (enj->row[0].z + enj->row[lj].z);
                    /* And the angular sector, seen from the centroid, from this
                     * arm's left kerb point to the next arm's right kerb point:
                     * a corner point outside it, or one that steps backwards,
                     * folds the outline (two carriageways that cross just
                     * before a junction, Plaza Italia). */
                    const double ang0 = atan2(last.z - cl->cz, last.x - cl->cx);
                    double sector = atan2(nextpt.z - cl->cz, nextpt.x - cl->cx) - ang0, dlast = 0.0;
                    while (sector <= 0.0) sector += 2.0 * ROAM_PI;
                    while (sector > 2.0 * ROAM_PI) sector -= 2.0 * ROAM_PI;
                    for (s = 0; s < nq; s++) {
                        const double d0 = hypot(qp[s].x - last.x, qp[s].z - last.z);
                        const double d1 = hypot(qp[s].x - nextpt.x, qp[s].z - nextpt.z);
                        const double pi_ = ((qp[s].x - ri_x) * en->dir_x + (qp[s].z - ri_z) * en->dir_z) * oi;
                        const double pj_ = ((qp[s].x - rj_x) * enj->dir_x + (qp[s].z - rj_z) * enj->dir_z) * oj;
                        double dq = atan2(qp[s].z - cl->cz, qp[s].x - cl->cx) - ang0;
                        while (dq < 0.0) dq += 2.0 * ROAM_PI;
                        while (dq >= 2.0 * ROAM_PI) dq -= 2.0 * ROAM_PI;
                        if (d0 < ROAM_OL_MIN_SEG || d1 < ROAM_OL_MIN_SEG) continue;
                        if (na > 1 && (pi_ > 0.0 || pj_ > 0.0)) { c->corner_pts_past_cut++; continue; }
                        if (na > 1 && (dq <= dlast || dq >= sector)) { c->corner_pts_out_of_sector++; continue; }
                        dlast = dq;
                        kept[nk++] = qp[s];
                        last = qp[s];
                    }
                    plen[0] = 0.0;
                    last = en->row[a->end == 0 ? lanes : 0];
                    for (s = 0; s < nk; s++) {
                        plen[s + 1] = plen[s] + hypot(kept[s].x - last.x, kept[s].z - last.z);
                        last = kept[s];
                    }
                    tot = plen[nk] + hypot(nextpt.x - last.x, nextpt.z - last.z);
                    prev = cut_left;
                    for (s = 0; s < nk; s++) {
                        TD5_RoamPatchVert v;
                        int vi;
                        const double f = tot > 0.0 ? plen[s + 1] / tot : 0.5;
                        pv_init(&v, kept[s].x, kept[s].z,
                                co->type == CORNER_FILLET
                                    ? ((s == 0 || s == nk - 1) ? TD5_ROAM_PV_TANGENT : TD5_ROAM_PV_ARC)
                                    : TD5_ROAM_PV_TANGENT);
                        v.ref[0] = m;
                        v.w[0] = 1.0 - f;
                        v.ref[1] = (m + 1) % na;
                        v.w[1] = f;
                        if (v.ref[1] == v.ref[0]) { v.ref[1] = TD5_ROAM_HREF_NONE; v.w[0] = 1.0; v.w[1] = 0.0; }
                        vi = patch_add_v(&pt, &v);
                        if (vi < 0) goto done;
                        if (!patch_add_seg(&pt, prev, vi, TD5_ROAM_OL_KERB, -1)) goto done;
                        prev = vi;
                    }
                }
                /* Segment to the next arm's cut-right: index known only after
                 * the next arm is written, so record a placeholder (-1). */
                if (!patch_add_seg(&pt, prev, -1, TD5_ROAM_OL_KERB, -1)) goto done;
            }
            /* Resolve placeholders: the vertex after `prev` in outline order. */
            for (k = 0; k < pt.nseg; k++)
                if (pt.seg[k].v[1] < 0) {
                    const int nv = pt.seg[k].v[0] + 1;
                    pt.seg[k].v[1] = nv < pt.npv ? nv : first_cut_right;
                }
        }
        cl->nout = pt.npv;

        /* Triangulate: fan from the centroid, else ear clipping. */
        if (fan_valid(pt.pv, cl->nout, cl->cx, cl->cz)) {
            TD5_RoamPatchVert v;
            int ci;
            pv_init(&v, cl->cx, cl->cz, TD5_ROAM_PV_CENTRE);
            v.ref[0] = TD5_ROAM_HREF_CENTRE;
            v.w[0] = 1.0;
            ci = patch_add_v(&pt, &v);
            if (ci < 0) goto done;
            for (m = 0; m < cl->nout; m++)
                if (!patch_add_tri(&pt, ci, m, (m + 1) % cl->nout)) goto done;
            cl->flags |= TD5_ROAM_CL_FAN;
            c->fan_ok++;
        } else {
            LTri *tmp = (LTri *)malloc((size_t)(cl->nout + 1) * sizeof(LTri));
            int nt = 0;
            if (!tmp) goto done;
            c->fan_fallback++;
            if (outline_simple(pt.pv, cl->nout) && ear_clip(pt.pv, cl->nout, tmp, &nt)) {
                for (m = 0; m < nt; m++)
                    if (!patch_add_tri(&pt, tmp[m].v[0], tmp[m].v[1], tmp[m].v[2])) { free(tmp); goto done; }
                cl->flags |= TD5_ROAM_CL_EARCLIP;
            } else if (simple && cl->nnode == 1 && patch_hull(&pt, &cl->nout)) {
                /* [F1-T9] One-node clusters only. A MULTI-node cluster that gets
                 * here is nodes whose paved pieces do not form one patch (its
                 * UNION came out as 2 outer loops); the hull of all their cut rows
                 * paved a 60 m slab over the neighbouring ribbons, 10-40 cm off
                 * their heights. It now FAILs this pass and is SPLIT next. */
                /* Last resort on a SIMPLE cluster: the convex hull of the cut
                 * rows, fanned from its centroid (convex, so always valid). */
                cl->flags |= TD5_ROAM_CL_HULL;
                c->hull_fallback++;
                TD5_LOG_I(LOG_TAG, "ROAM_PATCH_HULL cluster %lld (%d arm(s)): hull of the cut rows",
                          (long long)cl->key, na);
            } else {
                cl->flags |= TD5_ROAM_CL_FAIL;
                c->earclip_fail++;
                TD5_LOG_W(LOG_TAG, "ROAM_PATCH_FAIL cluster %lld (%d arm(s), %d outline "
                          "vertices): neither the fan nor ear clipping is valid",
                          (long long)cl->key, na, cl->nout);
            }
            free(tmp);
        }

        /* [F1-T9] An absorbed edge must lie on the patch. Design 1.3 rule 4
         * absorbs an edge with both ends in the cluster ("no ribbon"), but only
         * the UNION outline paves internal edges; a NORMAL patch is just the arms
         * and their fillets. A turning loop or a service connector that bulges
         * out of the arm fan was left unpaved (T7 audit: ways 1386855175,
         * 1320455895, ... 7-16 m off the road surface). Such a patch is invalid
         * here, so the next pass rebuilds it UNION. */
        if (mode[g->cluster_node[cl->node_first]] == MODE_NORMAL && pt.ntri > 0 &&
            !(cl->flags & TD5_ROAM_CL_FAIL) && !absorbed_on_patch(g, i, &pt, ROAM_ABSORB_TOL)) {
            cl->flags |= TD5_ROAM_CL_FAIL;
            pt.npv = pt.ntri = pt.nseg = 0;
            cl->nout = 0;
            c->absorbed_off_patch++;
            TD5_LOG_I(LOG_TAG, "ROAM_PATCH_ABSORB cluster %lld (%d arm(s)): an absorbed edge leaves "
                      "the patch, rebuilt with the union outline", (long long)cl->key, na);
        }

patch_done:
        /* Integer safety: split every triangle edge longer than tri_edge_max. */
        if (pt.ntri > 0) {
            int ovf = 0;
            const int s = patch_split(&pt, P->tri_edge_max, &ovf);
            if (s < 0) goto done;
            c->tri_splits += s;
            c->recipe_overflow += ovf;
        }

        /* Publish vertices, triangles, outline. */
        cl->vert_first = g->npvert;
        cl->nvert = pt.npv;
        if (!grow((void **)&g->pvert, &caps.cap_pv, g->npvert + pt.npv, sizeof *g->pvert)) goto done;
        for (m = 0; m < pt.npv; m++) {
            TD5_RoamPatchVert *v = &g->pvert[g->npvert + m];
            *v = pt.pv[m];
            v->xq = td5_roam_quant(v->x);
            v->zq = td5_roam_quant(v->z);
        }
        cl->tri_first = g->nptri;
        cl->ntri = pt.ntri;
        if (!grow((void **)&g->ptri, &caps.cap_tri, g->nptri + pt.ntri, sizeof *g->ptri)) goto done;
        for (m = 0; m < pt.ntri; m++)
            for (q = 0; q < 3; q++)
                g->ptri[g->nptri + m].v[q] = g->npvert + pt.tri[m].v[q];
        cl->ol_first = g->noutline;
        cl->nol = pt.nseg;
        if (!grow((void **)&g->outline, &caps.cap_ol, g->noutline + pt.nseg, sizeof *g->outline))
            goto done;
        for (m = 0; m < pt.nseg; m++) {
            TD5_RoamOutline *o = &g->outline[g->noutline + m];
            o->v[0] = g->npvert + pt.seg[m].v[0];
            o->v[1] = g->npvert + pt.seg[m].v[1];
            o->kind = pt.seg[m].kind;
            o->arm = pt.seg[m].arm;
        }
        g->npvert += pt.npv;
        g->nptri += pt.ntri;
        g->noutline += pt.nseg;
        g->narm += na;
        c->patch_tris += pt.ntri;
        c->patch_verts += pt.npv;
    }

    /* Ribbon swept width: each interior polyline turn between the two end rows. */
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        const TD5_RoamPt *p = &g->pt[e->pt_first];
        double s = 0.0;
        int q;
        if (e->end[0].kind == TD5_ROAM_END_INTERNAL) continue;
        for (q = 1; q + 1 < e->npt; q++) {
            const double ax = p[q].x - p[q - 1].x, az = p[q].z - p[q - 1].z;
            const double bx = p[q + 1].x - p[q].x, bz = p[q + 1].z - p[q].z;
            const double la = sqrt(ax * ax + az * az), lb = sqrt(bx * bx + bz * bz);
            double cphi, phi, rc, rin, need;
            s += la;
            if (s <= e->end[0].s || s >= e->end[1].s || la <= 0.0 || lb <= 0.0) continue;
            cphi = (ax * bx + az * bz) / (la * lb);
            phi = acos(cphi < -1.0 ? -1.0 : (cphi > 1.0 ? 1.0 : cphi));
            if (phi > ROAM_PI / 4.0) c->ribbon_sharp_vertices++;
            if (phi < 5.0 * ROAM_PI / 180.0) continue;
            rc = 0.5 * (la < lb ? la : lb) / tan(0.5 * phi);
            rin = rc - e->half_w;
            need = swept_need(rin, W, LF);
            c->ribbon_swept_checked++;
            if (need > 2.0 * e->half_w) {
                c->ribbon_swept_fail++;
                TD5_LOG_I(LOG_TAG, "ROAM_SWEPT_FAIL ribbon %llu at s %.1f m: turn %.0f deg, "
                          "local radius %.1f m, carriageway %.2f m, need %.2f m",
                          (unsigned long long)e->key, s / TD5_ROAM_UPM, phi * 180.0 / ROAM_PI,
                          rc / TD5_ROAM_UPM, 2.0 * e->half_w / TD5_ROAM_UPM, need / TD5_ROAM_UPM);
            }
        }
    }

    TD5_LOG_I(LOG_TAG, "roam junctions: %d cluster(s) (%d multi-node), %d internal edge(s), "
              "%d union(s) by edge + %d by disc, %d refused (cap), %d refused (layer), "
              "max diameter %.1f m, fan %d, ear-clip %d, fail %d, fillets %d, "
              "swept fail %d (widened %d), %d split(s)",
              c->clusters, c->multi_node_clusters, c->internal_edges, c->unions_edge,
              c->unions_disc, c->rejected_unions, c->rejected_layer,
              c->max_diameter / TD5_ROAM_UPM, c->fan_ok, c->fan_fallback - c->earclip_fail,
              c->earclip_fail, c->fillets, c->swept_fail, c->swept_widened, c->tri_splits);
    ok = 1;

done:
    free(uf);
    free(cuf);
    free(nxt);
    free(tail);
    free(diam);
    free(rdisc);
    free(xs);
    free(pr);
    free(at);
    free(cor);
    free(pt.pv);
    free(pt.tri);
    free(pt.seg);
    uloops_free(&ul);
    return ok;
}

/* Drop every junction output so a pass can run again from the graph alone. */
static void junction_reset(TD5_RoamGraph *g)
{
    int i, q;
    free(g->cluster);      g->cluster = NULL;      g->ncluster = 0;
    free(g->cluster_node); g->cluster_node = NULL; g->ncluster_node = 0;
    free(g->arm);          g->arm = NULL;          g->narm = 0;
    free(g->pvert);        g->pvert = NULL;        g->npvert = 0;
    free(g->ptri);         g->ptri = NULL;         g->nptri = 0;
    free(g->outline);      g->outline = NULL;      g->noutline = 0;
    for (i = 0; i < g->nnode; i++) g->node[i].cluster = -1;
    for (i = 0; i < g->nedge; i++)
        for (q = 0; q < 2; q++) {
            TD5_RoamEdgeEnd *en = &g->edge[i].end[q];
            if (en->kind != TD5_ROAM_END_CLUSTER && en->kind != TD5_ROAM_END_INTERNAL) continue;
            en->kind = 0;
            en->cluster = -1;
            en->arm = -1;
            en->nrow = 0;
            en->s = 0.0;
            en->cut = 0.0;
        }
    /* A chain may have turned a JOIN end into CLUSTER / INTERNAL. */
    td5_roam_graph_refill_joins(g);
}

/* Up to eight passes (F1-T9; was four), each a pure function of the graph and
 * the previous pass, so the fallback set is deterministic and local. A SPLIT
 * one-node cluster that still fails goes SPLIT_UNION then SPLIT_SIMPLE (F1-T9):
 *   1. every cluster NORMAL;
 *   2. a cluster with no valid triangulation is rebuilt UNION (same members:
 *      the outline is the union of its paved pieces, cuts pushed out until the
 *      outline holds every cut row) -- diagonal crossings with arms from 3-6
 *      nodes, sharp Y splits (_archive/roam/F1/T3b);
 *   3. a cluster still failing is rebuilt SIMPLE (no corners, cut rows beyond
 *      every member node, convex HULL as the last resort);
 *   4. a multi-node cluster still failing is SPLIT: its nodes may not merge,
 *      so each becomes a one-node cluster joined by short ribbons. */
int td5_roam_junction_build(TD5_RoamGraph *g)
{
    static const char *const mode_name[6] = { "NORMAL", "SIMPLE", "SPLIT", "UNION",
                                              "SPLIT_UNION", "SPLIT_SIMPLE" };
    const TD5_RoamGraphCensus keep = g->census;
    unsigned char *mode = (unsigned char *)calloc((size_t)(g->nnode > 0 ? g->nnode : 1), 1);
    double comp1 = 0.0;
    int nsimple = 0, nsplit = 0, pass, i, k, ok;
    if (!mode) return 0;
    ok = junction_pass(g, mode);
    comp1 = g->census.max_component;
    /* [F1-T9] Up to 8 passes (was 4): a NORMAL cluster can now fail on an
     * absorbed edge in a later pass (its members change when a neighbour is
     * SPLIT), and must still get its UNION rebuild. The loop stops at the first
     * pass that changes nothing, so a build that settled by pass 4 is unchanged. */
    for (pass = 2; ok && pass <= 8; pass++) {
        int changed = 0;
        for (i = 0; i < g->ncluster; i++) {
            const TD5_RoamCluster *cl = &g->cluster[i];
            const int m0 = mode[g->cluster_node[cl->node_first]];
            int want;
            if (!(cl->flags & TD5_ROAM_CL_FAIL) || cl->narm == 0) continue;
            if (m0 == MODE_NORMAL) want = MODE_UNION;
            else if (m0 == MODE_UNION) want = MODE_SIMPLE;
            else if (m0 == MODE_SIMPLE && cl->nnode > 1) want = MODE_SPLIT;
            else if (m0 == MODE_SPLIT) want = MODE_SPLIT_UNION;
            else if (m0 == MODE_SPLIT_UNION) want = MODE_SPLIT_SIMPLE;
            else continue;
            for (k = 0; k < cl->nnode; k++) mode[g->cluster_node[cl->node_first + k]] = (unsigned char)want;
            changed++;
            if (want == MODE_SIMPLE) nsimple++;
            else if (want == MODE_SPLIT) nsplit++;
            TD5_LOG_I(LOG_TAG, "ROAM_CLUSTER_%s cluster %lld (%d nodes, %d arms): patch invalid, "
                      "rebuilt %s", mode_name[want], (long long)cl->key, cl->nnode, cl->narm,
                      MODE_IS_UNION(want)  ? "with the union outline" :
                      MODE_IS_SIMPLE(want) ? "with the simple outline" : "as one-node clusters");
        }
        if (!changed) break;
        junction_reset(g);
        g->census = keep;
        ok = junction_pass(g, mode);
    }
    g->census.max_component = comp1;
    g->census.simple_fallback = nsimple;
    g->census.split_fallback = nsplit;
    free(mode);
    return ok;
}

/* ------------------------------------------------------------ dev census ---- */

#ifndef TD5RE_RELEASE

/* F0a's casco polygon (world frame), the same quad as td5_roam_cache.c. */
static const double k_casco[4][2] = {
    { 1078705.99,        5374175.73 },
    { 2582491.695195345, 7019045.535332896 },
    { 4255470.01,        5615132.29 },
    { 2731713.44,        3880755.48 },
};

static int casco_pip(double x, double z)
{
    int i, j = 3, inside = 0;
    for (i = 0; i < 4; i++) {
        const double xi = k_casco[i][0], zi = k_casco[i][1];
        const double xj = k_casco[j][0], zj = k_casco[j][1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi)
            inside = !inside;
        j = i;
    }
    return inside;
}

/* Every SIGNALS tile whose INDEX extent meets the window: signals[] x,z. */
static double *load_signals(const char *slug, const double win[4], int *n_out)
{
    char path[400];
    char *json;
    cJSON *root, *tl, *t;
    double *xz = NULL;
    int n = 0, cap = 0;
    *n_out = 0;
    td5_geo_source_path(path, sizeof path, slug, "tiles/INDEX.JSON");
    json = td5_geo_roads_slurp(path);
    if (!json) return NULL;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return NULL;
    tl = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "layers"), "SIGNALS"),
                             "tiles");
    for (t = tl ? tl->child : NULL; t; t = t->next) {
        double v[7];
        const cJSON *a = t->child;
        int k;
        char rel[64], p2[400];
        char *body;
        cJSON *doc, *arr, *s;
        for (k = 0; k < 7 && a; k++, a = a->next) v[k] = a->valuedouble;
        if (k < 7) continue;
        if (v[5] < win[0] || v[3] > win[2] || v[6] < win[1] || v[4] > win[3]) continue;
        snprintf(rel, sizeof rel, "tiles/SIGNALS_%d_%d.JSON", (int)v[0], (int)v[1]);
        td5_geo_source_path(p2, sizeof p2, slug, rel);
        body = td5_geo_roads_slurp(p2);
        if (!body) continue;
        doc = cJSON_Parse(body);
        free(body);
        if (!doc) continue;
        arr = cJSON_GetObjectItem(doc, "signals");
        for (s = arr ? arr->child : NULL; s; s = s->next) {
            const cJSON *x = cJSON_GetObjectItem(s, "x"), *z = cJSON_GetObjectItem(s, "z");
            if (!x || !z) continue;
            if (!grow((void **)&xz, &cap, 2 * n + 2, sizeof(double))) break;
            xz[2 * n] = x->valuedouble;
            xz[2 * n + 1] = z->valuedouble;
            n++;
        }
        cJSON_Delete(doc);
    }
    cJSON_Delete(root);
    *n_out = n;
    return xz;
}

static void dump_json(const TD5_RoamGraph *g, const char *path)
{
    FILE *f = fopen(path, "w");
    int i, k, first = 1;
    if (!f) return;
    fprintf(f, "{\"units_per_m\": %.1f,\n\"casco\": [", TD5_ROAM_UPM);
    for (k = 0; k < 4; k++) fprintf(f, "%s[%.2f, %.2f]", k ? ", " : "", k_casco[k][0], k_casco[k][1]);
    fprintf(f, "],\n\"clusters\": [\n");
    for (i = 0; i < g->ncluster; i++) {
        const TD5_RoamCluster *cl = &g->cluster[i];
        if (!casco_pip(cl->cx, cl->cz)) continue;
        fprintf(f, "%s{\"key\": %lld, \"cx\": %.2f, \"cz\": %.2f, \"diam_m\": %.2f, \"flags\": %d, "
                "\"narm\": %d, \"nout\": %d, \"layer\": %d, \"swept_margin_m\": %.3f,\n \"nodes\": [",
                first ? "" : ",\n", (long long)cl->key, cl->cx, cl->cz, cl->diameter / TD5_ROAM_UPM,
                cl->flags, cl->narm, cl->nout, cl->layer,
                cl->min_swept_margin > 1e29 ? 99.0 : cl->min_swept_margin / TD5_ROAM_UPM);
        first = 0;
        for (k = 0; k < cl->nnode; k++) {
            const TD5_RoamNode *nd = &g->node[g->cluster_node[cl->node_first + k]];
            fprintf(f, "%s[%.1f, %.1f, %d]", k ? ", " : "", nd->x, nd->z, nd->kind);
        }
        fprintf(f, "],\n \"arms\": [");
        for (k = 0; k < cl->narm; k++) {
            const TD5_RoamArm *a = &g->arm[cl->arm_first + k];
            const TD5_RoamEdge *e = &g->edge[a->edge];
            fprintf(f, "%s{\"edge\": \"%llu\", \"way\": %lld, \"end\": %d, \"ang\": %.5f, "
                    "\"dir\": [%.5f, %.5f], \"hw\": %.1f, \"t\": %.1f, \"kerb_r\": %.1f, "
                    "\"corner\": %d, \"lanes\": %d, \"klass\": %d, \"layer\": %d, \"oneway\": %d}",
                    k ? ", " : "", (unsigned long long)e->key, (long long)e->way_id, a->end, a->ang,
                    a->dir_x, a->dir_z, a->half_w, a->t, a->kerb_r, a->corner, e->lanes, e->klass,
                    e->layer, e->oneway);
        }
        fprintf(f, "],\n \"verts\": [");
        for (k = 0; k < cl->nvert; k++) {
            const TD5_RoamPatchVert *v = &g->pvert[cl->vert_first + k];
            fprintf(f, "%s[%d, %d, %d]", k ? ", " : "", v->xq, v->zq, v->kind);
        }
        fprintf(f, "],\n \"tris\": [");
        for (k = 0; k < cl->ntri; k++) {
            const TD5_RoamPatchTri *t = &g->ptri[cl->tri_first + k];
            fprintf(f, "%s[%d, %d, %d]", k ? ", " : "", t->v[0] - cl->vert_first,
                    t->v[1] - cl->vert_first, t->v[2] - cl->vert_first);
        }
        fprintf(f, "],\n \"ol\": [");
        for (k = 0; k < cl->nol; k++) {
            const TD5_RoamOutline *o = &g->outline[cl->ol_first + k];
            fprintf(f, "%s[%d, %d, %d]", k ? ", " : "", o->v[0] - cl->vert_first,
                    o->v[1] - cl->vert_first, o->kind);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n],\n\"edges\": [\n");
    first = 1;
    for (i = 0; i < g->nedge; i++) {
        const TD5_RoamEdge *e = &g->edge[i];
        int in = 0, q;
        for (k = 0; k < e->npt && !in; k++)
            in = casco_pip(g->pt[e->pt_first + k].x, g->pt[e->pt_first + k].z);
        if (!in) continue;
        fprintf(f, "%s{\"key\": \"%llu\", \"way\": %lld, \"lanes\": %d, \"hw\": %.1f, \"klass\": %d, "
                "\"layer\": %d, \"len\": %.1f, \"k\": [%d, %d], \"s\": [%.3f, %.3f], \"pts\": [",
                first ? "" : ",\n", (unsigned long long)e->key, (long long)e->way_id, e->lanes,
                e->half_w, e->klass, e->layer, e->length, e->end[0].kind, e->end[1].kind,
                e->end[0].s, e->end[1].s);
        first = 0;
        for (k = 0; k < e->npt; k++)
            fprintf(f, "%s[%.1f, %.1f]", k ? ", " : "", g->pt[e->pt_first + k].x,
                    g->pt[e->pt_first + k].z);
        fprintf(f, "], \"rows\": [");
        for (q = 0; q < 2; q++) {
            const TD5_RoamEdgeEnd *en = &e->end[q];
            fprintf(f, "%s[", q ? ", " : "");
            for (k = 0; k < en->nrow; k++)
                fprintf(f, "%s[%d, %d]", k ? ", " : "", en->rowq[k][0], en->rowq[k][1]);
            fprintf(f, "]");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n]}\n");
    fclose(f);
}

int td5_roam_junction_dev_census(void)
{
    static const char slug[] = "la_plata_partido";
    TD5_RoamCacheCensus cc;
    TD5_RoamGraph g;
    double win[4];
    char path[400], jpath[400];
    const char *dir;
    FILE *f;
    double *sig;
    int nsig = 0, i, k;
    int by_arm[TD5_ROAM_ARM_HIST], cas = 0, cas_fan = 0, cas_ear = 0, cas_fail = 0, cas_swept = 0;
    int cas_multi = 0, cas_clamped = 0, cas_taper = 0, cas_bend = 0, cas_simple = 0, cas_hull = 0;
    int cas_union = 0, cas_rowcov = 0, cas_hole = 0, w_union = 0, w_rowcov = 0, w_hole = 0;
    double cas_maxd = 0.0;
    int64_t cas_maxk = 0;

    if (td5_env_int("TD5RE_ROAM_JUNC_TEST", 0, 0, 1) != 1) return 0;
    win[0] = win[2] = k_casco[0][0];
    win[1] = win[3] = k_casco[0][1];
    for (i = 1; i < 4; i++) {
        if (k_casco[i][0] < win[0]) win[0] = k_casco[i][0];
        if (k_casco[i][1] < win[1]) win[1] = k_casco[i][1];
        if (k_casco[i][0] > win[2]) win[2] = k_casco[i][0];
        if (k_casco[i][1] > win[3]) win[3] = k_casco[i][1];
    }
    if (td5_roam_cache_session_begin(slug) <= 0) {
        TD5_LOG_E(LOG_TAG, "roam junction test: no ROADS tiles for %s", slug);
        return 1;
    }
    td5_roam_cache_load_window(win, 300.0 * TD5_ROAM_UPM, &cc);
    if (!td5_roam_graph_build(&g, win, 300.0 * TD5_ROAM_UPM, NULL)) {
        td5_roam_cache_session_end();
        return 1;
    }
    sig = load_signals(slug, win, &nsig);
    td5_roam_graph_attach_signals(&g, sig, nsig);
    free(sig);

    memset(by_arm, 0, sizeof by_arm);
    for (i = 0; i < g.ncluster; i++) {
        const TD5_RoamCluster *cl = &g.cluster[i];
        if (cl->flags & TD5_ROAM_CL_UNION) w_union++;
        if (cl->flags & TD5_ROAM_CL_ROW_COVERED) w_rowcov++;
        if (cl->flags & TD5_ROAM_CL_HOLE) w_hole++;
        if (!casco_pip(cl->cx, cl->cz)) continue;
        if (cl->flags & TD5_ROAM_CL_UNION) cas_union++;
        if (cl->flags & TD5_ROAM_CL_ROW_COVERED) cas_rowcov++;
        if (cl->flags & TD5_ROAM_CL_HOLE) cas_hole++;
        cas++;
        by_arm[cl->narm < TD5_ROAM_ARM_HIST ? cl->narm : TD5_ROAM_ARM_HIST - 1]++;
        if (cl->flags & TD5_ROAM_CL_FAN) cas_fan++;
        if (cl->flags & TD5_ROAM_CL_EARCLIP) cas_ear++;
        if (cl->flags & TD5_ROAM_CL_FAIL) cas_fail++;
        if (cl->flags & TD5_ROAM_CL_SWEPT_FAIL) cas_swept++;
        if (cl->flags & TD5_ROAM_CL_CLAMPED) cas_clamped++;
        if (cl->flags & TD5_ROAM_CL_TAPER) cas_taper++;
        if (cl->flags & TD5_ROAM_CL_BEND) cas_bend++;
        if (cl->flags & TD5_ROAM_CL_SIMPLE) cas_simple++;
        if (cl->flags & TD5_ROAM_CL_HULL) cas_hull++;
        if (cl->nnode > 1) cas_multi++;
        if (cl->diameter > cas_maxd) { cas_maxd = cl->diameter; cas_maxk = cl->key; }
    }

    dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_junc_census.txt", dir ? dir : "");
    snprintf(jpath, sizeof jpath, "%sroam_clusters.json", dir ? dir : "");
    f = fopen(path, "w");
    if (f) {
        const TD5_RoamGraphCensus *c = &g.census;
        static const char *const drop_name[TD5_ROAM_DROP_COUNT] = {
            "keep", "bus_stop", "razed", "services", "raceway", "area", "parking_aisle",
            "drive-through", "no_id", "dup_id", "degenerate"
        };
        fprintf(f, "roam junction census (F1 T3), place %s, window = casco bbox + 300 m halo\n", slug);
        fprintf(f, "cache: %d tile(s) selected, %d way(s), %d point(s), %u ms\n",
                cc.tiles_selected, cc.ways, cc.points, (unsigned)cc.ms);
        fprintf(f, "\n[WINDOW TOTALS]\n");
        fprintf(f, "ways_in_window %d\nways_kept %d\n", c->ways_in, c->ways_kept);
        for (k = 1; k < TD5_ROAM_DROP_COUNT; k++)
            fprintf(f, "drop_%s %d\n", drop_name[k], c->drop[k]);
        fprintf(f, "nodes %d\nedges %d\njunction_nodes %d\njoins %d\ntapers %d\nbends %d\n"
                "dead_ends %d\nlayer_cross_nodes %d\n",
                c->nodes, c->edges, c->junction_nodes, c->joins, c->tapers, c->bends,
                c->dead_ends, c->layer_cross_nodes);
        fprintf(f, "clusters %d\nmulti_node_clusters %d\ninternal_edges %d\n",
                c->clusters, c->multi_node_clusters, c->internal_edges);
        for (k = 0; k < TD5_ROAM_ARM_HIST; k++)
            fprintf(f, "clusters_arms_%d%s %d\n", k, k == TD5_ROAM_ARM_HIST - 1 ? "_plus" : "",
                    c->cluster_by_arms[k]);
        fprintf(f, "unions_edge %d\nunions_disc %d\nrejected_unions %d\nrejected_layer %d\n",
                c->unions_edge, c->unions_disc, c->rejected_unions, c->rejected_layer);
        fprintf(f, "max_diameter_m %.2f (cluster %lld)\nmax_component_m %.2f\n",
                c->max_diameter / TD5_ROAM_UPM, (long long)c->max_diameter_key,
                c->max_component / TD5_ROAM_UPM);
        fprintf(f, "fan_ok %d\nfan_fallback %d\nearclip_ok %d\nearclip_fail %d\nclusters_no_arm %d\n"
                "simple_fallback %d\nhull_fallback %d\nsplit_fallback %d\nchain_absorbed %d\n"
                "corner_pts_past_cut %d\ncorner_pts_out_of_sector %d\n",
                c->fan_ok, c->fan_fallback, c->fan_fallback - c->earclip_fail - c->hull_fallback,
                c->earclip_fail, c->clusters_no_arm, c->simple_fallback, c->hull_fallback,
                c->split_fallback, c->chain_absorbed, c->corner_pts_past_cut,
                c->corner_pts_out_of_sector);
        fprintf(f, "union_fallback %d\nunion_row_covered %d\nunion_hole %d\n", w_union, w_rowcov, w_hole);
        fprintf(f, "fillets %d\ncorners_parallel %d\ncorners_far %d\ncorners_reflex %d\nbevels %d\n"
                "fillet_shrunk %d\n",
                c->fillets, c->corners_parallel, c->corners_far, c->corners_reflex, c->bevels,
                c->fillet_shrunk);
        fprintf(f, "swept_checked %d\nswept_widened %d\nswept_fail %d\n",
                c->swept_checked, c->swept_widened, c->swept_fail);
        fprintf(f, "ribbon_swept_checked %d\nribbon_swept_fail %d\nribbon_sharp_vertices_gt45 %d\n",
                c->ribbon_swept_checked, c->ribbon_swept_fail, c->ribbon_sharp_vertices);
        fprintf(f, "tri_splits %d\npatch_tris %d\npatch_verts %d\nrecipe_overflow %d\n"
                "short_edge_ends_clamped %d\nsignals_read %d\nsignal_ends %d\nbuild_ms %u\n",
                c->tri_splits, c->patch_tris, c->patch_verts, c->recipe_overflow,
                c->short_edges_clamped, nsig, c->signals_attached, (unsigned)c->ms);
        fprintf(f, "\n[CASCO POLYGON, clusters by centroid]\n");
        fprintf(f, "casco_clusters %d\n", cas);
        for (k = 0; k < TD5_ROAM_ARM_HIST; k++)
            fprintf(f, "casco_arms_%d%s %d\n", k, k == TD5_ROAM_ARM_HIST - 1 ? "_plus" : "", by_arm[k]);
        fprintf(f, "casco_multi_node %d\ncasco_taper %d\ncasco_bend %d\ncasco_fan %d\n"
                "casco_earclip %d\ncasco_fail %d\ncasco_swept_fail %d\ncasco_clamped %d\n"
                "casco_simple %d\ncasco_hull %d\ncasco_max_diameter_m %.2f (cluster %lld)\n",
                cas_multi, cas_taper, cas_bend, cas_fan, cas_ear, cas_fail, cas_swept, cas_clamped,
                cas_simple, cas_hull,
                cas_maxd / TD5_ROAM_UPM, (long long)cas_maxk);
        fprintf(f, "casco_union %d\ncasco_union_row_covered %d\ncasco_union_hole %d\n",
                cas_union, cas_rowcov, cas_hole);
        fprintf(f, "json %s\n", jpath);
        fclose(f);
    }
    dump_json(&g, jpath);
    TD5_LOG_I(LOG_TAG, "roam junction test: %d casco cluster(s); census %s", cas, path);
    td5_roam_graph_free(&g);
    /* Locality: TD5RE_ROAM_JUNC_SUBWIN=1 also builds the WEST HALF of the casco
     * bbox (same halo) to roam_clusters_sub.json; verify/roam-style compare in
     * _archive/roam/F1/T3/locality.py checks the clusters well inside it. */
    if (td5_env_int("TD5RE_ROAM_JUNC_SUBWIN", 0, 0, 1) == 1) {
        TD5_RoamGraph g2;
        double w2[4];
        w2[0] = win[0];
        w2[1] = win[1];
        w2[2] = 0.5 * (win[0] + win[2]);
        w2[3] = win[3];
        if (td5_roam_graph_build(&g2, w2, 300.0 * TD5_ROAM_UPM, NULL)) {
            snprintf(jpath, sizeof jpath, "%sroam_clusters_sub.json", dir ? dir : "");
            dump_json(&g2, jpath);
            TD5_LOG_I(LOG_TAG, "roam junction test: sub-window x <= %.0f: %d cluster(s) -> %s",
                      w2[2], g2.ncluster, jpath);
            td5_roam_graph_free(&g2);
        }
    }
    td5_roam_cache_session_end();
    return 1;
}

#endif /* !TD5RE_RELEASE */
