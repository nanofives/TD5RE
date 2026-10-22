/* td5_roam_muni.c -- GEO FREE ROAM: the MUNICIPAL LINE of a block (PORT-ONLY, roam NA). See td5_roam_muni.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "td5_roam_muni.h"

#define MU_FACE_COS     0.906307787   /* cos 25 deg: a wall faces a line piece                     */
#define MU_PAR_COS      0.965925826   /* cos 15 deg: two walls on one street (not a corner)        */
#define MU_TURN_MIN     30.0          /* a corner turning less than this (degrees) is no corner    */
#define MU_TURN_MAX     150.0
#define MU_RING_MAX     64            /* local working ring                                         */
#define MU_DUP          4.0           /* units (1 cm): consecutive points closer than this merge    */
#define MU_LEG_STEPS    10            /* a corner the line rounds: chamfer legs tried, 1 leg apart   */

struct TD5_Muni {
    TD5_MuniSeg *s;
    int    n, cap;
    int   *box;                       /* per piece: cell a0 b0 a1 b1                                */
    double x0, z0, cell;
    int    gnx, gnz;
    int   *start, *item;
};

TD5_Muni *td5_muni_new(void) { return (TD5_Muni *)calloc(1, sizeof(TD5_Muni)); }

void td5_muni_reset(TD5_Muni *m)
{
    if (!m) return;
    free(m->box); free(m->start); free(m->item);
    m->box = m->start = m->item = NULL;
    m->n = 0; m->gnx = m->gnz = 0;
}

void td5_muni_free(TD5_Muni *m)
{
    if (!m) return;
    td5_muni_reset(m);
    free(m->s);
    free(m);
}

int td5_muni_count(const TD5_Muni *m) { return m ? m->n : 0; }

int td5_muni_add(TD5_Muni *m, const TD5_MuniSeg *s)
{
    if (m->n >= m->cap) {
        const int nc = m->cap ? m->cap * 2 : 512;
        TD5_MuniSeg *p = (TD5_MuniSeg *)realloc(m->s, (size_t)nc * sizeof *p);
        if (!p) return 0;
        m->s = p; m->cap = nc;
    }
    m->s[m->n++] = *s;
    return 1;
}

static void mu_cell(const TD5_Muni *m, double x, double z, int *a, int *b)
{
    int i = (int)floor((x - m->x0) / m->cell), j = (int)floor((z - m->z0) / m->cell);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i >= m->gnx) i = m->gnx - 1;
    if (j >= m->gnz) j = m->gnz - 1;
    *a = i; *b = j;
}

int td5_muni_index(TD5_Muni *m, double x0, double z0, double x1, double z1, double cell)
{
    int *cnt, i, a, b, pass, total = 0, nc;
    free(m->box); free(m->start); free(m->item);
    m->box = m->start = m->item = NULL;
    m->x0 = x0; m->z0 = z0; m->cell = cell;
    m->gnx = (int)ceil((x1 - x0) / cell) + 1;
    m->gnz = (int)ceil((z1 - z0) / cell) + 1;
    if (m->gnx < 1) m->gnx = 1;
    if (m->gnz < 1) m->gnz = 1;
    nc = m->gnx * m->gnz;
    m->box = (int *)malloc((size_t)(m->n > 0 ? m->n : 1) * 4u * sizeof(int));
    m->start = (int *)calloc((size_t)nc + 1u, sizeof(int));
    cnt = (int *)calloc((size_t)nc + 1u, sizeof(int));
    if (!m->box || !m->start || !cnt) { free(cnt); return 0; }
    for (i = 0; i < m->n; i++) {
        const TD5_MuniSeg *s = &m->s[i];
        mu_cell(m, s->ax < s->bx ? s->ax : s->bx, s->az < s->bz ? s->az : s->bz, &m->box[i * 4 + 0], &m->box[i * 4 + 1]);
        mu_cell(m, s->ax > s->bx ? s->ax : s->bx, s->az > s->bz ? s->az : s->bz, &m->box[i * 4 + 2], &m->box[i * 4 + 3]);
    }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < m->n; i++)
            for (b = m->box[i * 4 + 1]; b <= m->box[i * 4 + 3]; b++)
                for (a = m->box[i * 4 + 0]; a <= m->box[i * 4 + 2]; a++) {
                    const int c = b * m->gnx + a;
                    if (pass == 0) cnt[c]++;
                    else m->item[m->start[c] + cnt[c]++] = i;
                }
        if (pass == 0) {
            for (i = 0; i < nc; i++) { m->start[i] = total; total += cnt[i]; cnt[i] = 0; }
            m->start[nc] = total;
            m->item = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
            if (!m->item) { free(cnt); return 0; }
        }
    }
    free(cnt);
    return 1;
}

/* every piece whose box meets the square (x +- r, z +- r), each once */
typedef struct { const TD5_Muni *m; int a0, b0, a1, b1, a, b, k; } MuIt;

static void mu_it_begin(MuIt *it, const TD5_Muni *m, double x, double z, double r)
{
    it->m = m;
    mu_cell(m, x - r, z - r, &it->a0, &it->b0);
    mu_cell(m, x + r, z + r, &it->a1, &it->b1);
    it->a = it->a0; it->b = it->b0;
    it->k = m->start ? m->start[it->b * m->gnx + it->a] : 0;
}

static int mu_it_next(MuIt *it)
{
    const TD5_Muni *m = it->m;
    if (!m->start || !m->n) return -1;
    for (;;) {
        const int c = it->b * m->gnx + it->a;
        while (it->k < m->start[c + 1]) {
            const int i = m->item[it->k++];
            const int *bx = &m->box[i * 4];
            /* listed in the first query cell its box meets */
            if (it->a != (bx[0] > it->a0 ? bx[0] : it->a0) || it->b != (bx[1] > it->b0 ? bx[1] : it->b0)) continue;
            return i;
        }
        if (++it->a > it->a1) { it->a = it->a0; if (++it->b > it->b1) return -1; }
        it->k = m->start[it->b * m->gnx + it->a];
    }
}

/* distance from (x, z) to piece s, its parameter t (clamped to [0,1]) and the signed offset off its line */
static double mu_seg(const TD5_MuniSeg *s, double x, double z, double *t_out, double *g_out)
{
    const double dx = s->bx - s->ax, dz = s->bz - s->az, l2 = dx * dx + dz * dz;
    double t = l2 > 1e-9 ? ((x - s->ax) * dx + (z - s->az) * dz) / l2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (t_out) *t_out = t;
    if (g_out) *g_out = (x - s->ax) * s->nx + (z - s->az) * s->nz;
    return hypot(x - (s->ax + dx * t), z - (s->az + dz * t));
}

int td5_muni_behind(const TD5_Muni *m, double x, double z, double r, double *g, int *seg)
{
    MuIt it;
    int i, best = -1;
    double bd = r, bg = 0.0;
    if (!m || !m->n || !m->start) return 0;
    mu_it_begin(&it, m, x, z, r);
    while ((i = mu_it_next(&it)) >= 0) {
        double t, gl;
        const double d = mu_seg(&m->s[i], x, z, &t, &gl);
        if (d > bd || (d == bd && best >= 0)) continue;
        bd = d; best = i;
        /* off the piece's end: the signed distance to that end, the sign from the piece's normal */
        bg = (t > 0.0 && t < 1.0) ? gl : (gl >= 0.0 ? d : -d);
    }
    if (best < 0) return 0;
    *g = bg;
    if (seg) *seg = best;
    return 1;
}

static int mu_cross(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double d1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
    const double d2 = (bx - ax) * (dz - az) - (bz - az) * (dx - ax);
    const double d3 = (dx - cx) * (az - cz) - (dz - cz) * (ax - cx);
    const double d4 = (dx - cx) * (bz - cz) - (dz - cz) * (bx - cx);
    return ((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0)) && d1 != 0.0 && d2 != 0.0 && d3 != 0.0 && d4 != 0.0;
}

static double mu_area(const double *x, const double *z, int n)
{
    double a = 0.0;
    int i, j;
    for (i = 0, j = n - 1; i < n; j = i++) a += x[j] * z[i] - x[i] * z[j];
    return 0.5 * a;
}

static int mu_simple(const double *x, const double *z, int n)
{
    int i, j;
    if (n < 3) return 0;
    for (i = 0; i < n; i++) {
        const int i1 = (i + 1) % n;
        for (j = i + 1; j < n; j++) {
            const int j1 = (j + 1) % n;
            if (j == i1 || j1 == i) continue;
            if (mu_cross(x[i], z[i], x[i1], z[i1], x[j], z[j], x[j1], z[j1])) return 0;
        }
    }
    return 1;
}

/* the walls of a CCW ring: per edge, the facing piece (or -1) and the two ends' offsets behind it */
typedef struct { int seg; double ga, gb; } MuWall;

static int mu_find_walls(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n, MuWall *w)
{
    int i, nw = 0;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double dx = x[j] - x[i], dz = z[j] - z[i], l = hypot(dx, dz);
        const double ox = l > 1e-9 ? dz / l : 0.0, oz = l > 1e-9 ? -dx / l : 0.0;   /* outward (CCW) */
        const double mx = 0.5 * (x[i] + x[j]), mz = 0.5 * (z[i] + z[j]);
        MuIt it;
        int s, best = -1;
        double bg = 0.0;
        w[i].seg = -1; w[i].ga = w[i].gb = 0.0;
        if (l < o->min_wall) continue;
        mu_it_begin(&it, m, mx, mz, o->reach + 0.5 * l);
        while ((s = mu_it_next(&it)) >= 0) {
            const TD5_MuniSeg *p = &m->s[s];
            const double pdx = p->bx - p->ax, pdz = p->bz - p->az, pl = hypot(pdx, pdz);
            double t, g, k;
            int q, hit = 0;
            if (pl < 1e-6 || -(ox * p->nx + oz * p->nz) < MU_FACE_COS) continue;
            t = ((mx - p->ax) * pdx + (mz - p->az) * pdz) / pl;               /* along the piece, units */
            if (t < -o->min_wall || t > pl + o->min_wall) continue;
            g = (mx - p->ax) * p->nx + (mz - p->az) * p->nz;
            if (g > o->reach || g < -o->reach) continue;
            if (best >= 0 && fabs(g) >= fabs(bg)) continue;
            /* nothing of this ring between the wall and the line (a recessed wall, a courtyard's back) */
            k = g;
            for (q = 0; q < n && !hit; q++) {
                const int q1 = (q + 1) % n;
                if (q == i) continue;
                hit = mu_cross(mx, mz, mx - p->nx * k, mz - p->nz * k, x[q], z[q], x[q1], z[q1]);
            }
            if (hit) continue;
            best = s; bg = g;
        }
        if (best < 0) continue;
        w[i].seg = best;
        w[i].ga = (x[i] - m->s[best].ax) * m->s[best].nx + (z[i] - m->s[best].az) * m->s[best].nz;
        w[i].gb = (x[j] - m->s[best].ax) * m->s[best].nx + (z[j] - m->s[best].az) * m->s[best].nz;
        nw++;
    }
    return nw;
}

/* the ring in CCW order in (x, z), *flip when it was reversed */
static int mu_ccw(const double *ix, const double *iz, int n, double *x, double *z, int *flip)
{
    int k;
    if (n < 3 || n > MU_RING_MAX) return 0;
    *flip = mu_area(ix, iz, n) < 0.0;
    for (k = 0; k < n; k++) { x[k] = *flip ? ix[n - 1 - k] : ix[k]; z[k] = *flip ? iz[n - 1 - k] : iz[k]; }
    return 1;
}

int td5_muni_walls(const TD5_Muni *m, const TD5_MuniOpt *o, const double *ix, const double *iz, int n, double *gap, int cap)
{
    double x[MU_RING_MAX], z[MU_RING_MAX];
    MuWall w[MU_RING_MAX];
    int flip, i, nw = 0;
    if (!m || !m->n || !mu_ccw(ix, iz, n, x, z, &flip)) return 0;
    mu_find_walls(m, o, x, z, n, w);
    for (i = 0; i < n; i++) {
        if (w[i].seg < 0) continue;
        if (nw < cap) gap[nw] = 0.5 * (w[i].ga + w[i].gb);
        nw++;
    }
    return nw;
}

/* where the two inset lines of pieces a and b cross */
static int mu_isect(const TD5_MuniSeg *a, const TD5_MuniSeg *b, double inset, double *x, double *z)
{
    /* (p - A) . n = inset for both: n_a . p = ca, n_b . p = cb */
    const double ca = a->ax * a->nx + a->az * a->nz + inset, cb = b->ax * b->nx + b->az * b->nz + inset;
    const double det = a->nx * b->nz - a->nz * b->nx;
    if (fabs(det) < 1e-6) return 0;
    *x = (ca * b->nz - cb * a->nz) / det;
    *z = (a->nx * cb - b->nx * ca) / det;
    return 1;
}

static int mu_behind_ok(const TD5_Muni *m, const TD5_MuniOpt *o, double x, double z)
{
    double g;
    if (!td5_muni_behind(m, x, z, o->reach, &g, NULL)) return 0;   /* a moved point with no line near: off the line's end */
    return g >= 0.5 * o->inset;
}

/* no point of ring (x, z)[n] (each vertex, every metre of each edge) stands more than tol in front of the line */
static int mu_all_behind(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n)
{
    int k, q;
    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        const double L = hypot(x[j] - x[k], z[j] - z[k]);
        const int nq = (int)(L / (o->min_wall > 1.0 ? o->min_wall : 1.0)) + 1;
        for (q = 0; q < nq; q++) {
            const double t = (double)q / (double)nq;
            double g;
            if (td5_muni_behind(m, x[k] + (x[j] - x[k]) * t, z[k] + (z[j] - z[k]) * t, o->reach, &g, NULL) && g < -o->tol)
                return 0;
        }
    }
    return 1;
}

int td5_muni_ring_behind(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n)
{
    return !m || !m->n || mu_all_behind(m, o, x, z, n);
}

int td5_muni_snap(const TD5_Muni *m, const TD5_MuniOpt *o, const double *ix, const double *iz, int n,
                  double *ox, double *oz, int *on, int cap, TD5_MuniRes *res)
{
    double x[MU_RING_MAX], z[MU_RING_MAX], tx[MU_RING_MAX * 2], tz[MU_RING_MAX * 2], a0;
    MuWall w[MU_RING_MAX];
    int act[MU_RING_MAX];
    double inx[MU_RING_MAX], inz[MU_RING_MAX], outx[MU_RING_MAX], outz[MU_RING_MAX];   /* where walls k-1 end / k start */
    int flip, i, k, nt = 0, nw, nact = 0, nfar = 0, nmove = 0, moved = 0;
    memset(res, 0, sizeof *res);
    *on = 0;
    if (n <= cap) { for (k = 0; k < n; k++) { ox[k] = ix[k]; oz[k] = iz[k]; } *on = n; }
    if (!m || !m->n || !mu_ccw(ix, iz, n, x, z, &flip)) { res->fate = TD5_MUNI_NONE; return res->fate; }
    a0 = mu_area(x, z, n);
    nw = mu_find_walls(m, o, x, z, n, w);
    res->walls = nw;
    if (!nw) { res->fate = TD5_MUNI_NONE; return res->fate; }
    /* which walls take part: within the snap band (both ends) */
    for (i = 0; i < n; i++) {
        double hi, lo;
        act[i] = 0;
        if (w[i].seg < 0) continue;
        hi = w[i].ga > w[i].gb ? w[i].ga : w[i].gb;
        lo = w[i].ga < w[i].gb ? w[i].ga : w[i].gb;
        if (hi > o->snap_max || lo < -o->protrude_max) { nfar++; continue; }
        act[i] = 1; nact++;
        if (fabs(w[i].ga - o->inset) > o->tol || fabs(w[i].gb - o->inset) > o->tol) {
            nmove++;
            if (0.5 * (w[i].ga + w[i].gb) > o->inset) res->fwd++; else res->back++;
        }
    }
    if (!nact) { res->fate = TD5_MUNI_FAR; return res->fate; }
    if (!nmove) { res->fate = mu_all_behind(m, o, x, z, n) ? TD5_MUNI_ON : TD5_MUNI_FRONT; return res->fate; }
    (void)nfar;
    /* every vertex: kept, moved onto one line, or the corner of two (with its ochava) */
    for (k = 0; k < n; k++) {
        const int ein = (k + n - 1) % n, eout = k;
        const int si = act[ein] ? w[ein].seg : -1, so = act[eout] ? w[eout].seg : -1;
        if (nt + 2 > MU_RING_MAX * 2) { res->fate = TD5_MUNI_RING; return res->fate; }
        if (si < 0 && so < 0) { inx[k] = outx[k] = tx[nt] = x[k]; inz[k] = outz[k] = tz[nt] = z[k]; nt++; continue; }
        if (si >= 0 && so >= 0 && si != so &&
            m->s[si].nx * m->s[so].nx + m->s[si].nz * m->s[so].nz < MU_PAR_COS) {
            /* a block corner: the crossing of the two lines, chamfered */
            const TD5_MuniSeg *A = &m->s[si], *B = &m->s[so];
            const double turn = acos(fmax(-1.0, fmin(1.0, A->nx * B->nx + A->nz * B->nz))) * 57.29577951308232;
            double X, Z, t1x, t1z, t2x, t2z, leg;
            int step, done = 0;
            if (!mu_isect(A, B, o->inset, &X, &Z)) { res->fate = TD5_MUNI_CORNER; return res->fate; }
            /* the crossing of two lines near parallel lands far off: it must be the corner of THESE two pieces */
            if (mu_seg(A, X, Z, NULL, NULL) > 0.5 * o->snap_max || mu_seg(B, X, Z, NULL, NULL) > 0.5 * o->snap_max ||
                hypot(X - x[k], Z - z[k]) > 1.5 * o->snap_max) { res->fate = TD5_MUNI_CORNER; return res->fate; }
            /* along line A toward the corner (the incoming wall's way), along line B away from it */
            t1x = -A->nz; t1z = A->nx;
            if (t1x * (x[k] - x[ein]) + t1z * (z[k] - z[ein]) < 0.0) { t1x = -t1x; t1z = -t1z; }
            t2x = -B->nz; t2z = B->nx;
            if (t2x * (x[(k + 1) % n] - x[k]) + t2z * (z[(k + 1) % n] - z[k]) < 0.0) { t2x = -t2x; t2z = -t2z; }
            res->corners++;
            if (turn < MU_TURN_MIN || turn > MU_TURN_MAX) {
                if (!mu_behind_ok(m, o, X, Z)) { res->fate = TD5_MUNI_CORNER; return res->fate; }
                inx[k] = outx[k] = tx[nt] = X; inz[k] = outz[k] = tz[nt] = Z; nt++;
                continue;
            }
            if (o->ochava_leg <= 0.0 && mu_behind_ok(m, o, X, Z)) {
                inx[k] = outx[k] = tx[nt] = X; inz[k] = outz[k] = tz[nt] = Z; nt++;
                continue;
            }
            leg = o->ochava_leg > 0.0 ? o->ochava_leg : o->min_wall;
            for (step = 0; step < MU_LEG_STEPS && !done; step++) {
                const double L = leg + (double)step * (o->ochava_leg > 0.0 ? o->ochava_leg : o->min_wall) * 0.5;
                const double p1x = X - t1x * L, p1z = Z - t1z * L, p2x = X + t2x * L, p2z = Z + t2z * L;
                /* the chamfer must not reach past either wall's far end */
                if ((p1x - x[ein]) * t1x + (p1z - z[ein]) * t1z < o->min_wall) break;
                if ((x[(k + 1) % n] - p2x) * t2x + (z[(k + 1) % n] - p2z) * t2z < o->min_wall) break;
                if (!mu_behind_ok(m, o, p1x, p1z) || !mu_behind_ok(m, o, p2x, p2z) ||
                    !mu_behind_ok(m, o, 0.5 * (p1x + p2x), 0.5 * (p1z + p2z))) continue;
                inx[k] = tx[nt] = p1x; inz[k] = tz[nt] = p1z; nt++;
                outx[k] = tx[nt] = p2x; outz[k] = tz[nt] = p2z; nt++;
                done = 1;
            }
            if (!done) { res->fate = TD5_MUNI_CORNER; return res->fate; }
            res->ochavas++;
            continue;
        }
        {   /* one street: onto the line of the piece the vertex projects onto (of its walls) */
            int s = si >= 0 ? si : so;
            double t, g;
            if (si >= 0 && so >= 0 && si != so) {
                mu_seg(&m->s[si], x[k], z[k], &t, NULL);
                if (!(t > 0.0 && t < 1.0)) s = so;
            }
            mu_seg(&m->s[s], x[k], z[k], &t, &g);
            tx[nt] = x[k] + m->s[s].nx * (o->inset - g);
            tz[nt] = z[k] + m->s[s].nz * (o->inset - g);
            if (fabs(o->inset - g) > 1.0) moved = 1;
            if (!mu_behind_ok(m, o, tx[nt], tz[nt])) { res->fate = TD5_MUNI_CORNER; return res->fate; }
            inx[k] = outx[k] = tx[nt]; inz[k] = outz[k] = tz[nt];
            nt++;
        }
    }
    /* every point of a wall now on the line lies ON a line piece (not on its extension past the piece's end, where the
     * line stops: an island-filled gap, a deck, a mouth), within 0.3 m (6 tol) of it */
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double L = hypot(inx[j] - outx[i], inz[j] - outz[i]);
        int q, nq;
        if (!act[i]) continue;
        nq = (int)(L / (o->min_wall > 1.0 ? o->min_wall : 1.0)) + 1;
        for (q = 0; q <= nq; q++) {
            const double t = (double)q / (double)nq;
            double g;
            if (!td5_muni_behind(m, outx[i] + (inx[j] - outx[i]) * t, outz[i] + (inz[j] - outz[i]) * t, o->reach, &g, NULL) ||
                fabs(g) > o->inset + 6.0 * o->tol) { res->fate = TD5_MUNI_CORNER; return res->fate; }
        }
    }
    /* the area a wall sweeps forward meets no other footprint */
    if (o->blocked)
        for (i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            double qx[4], qz[4], d;
            if (!act[i]) continue;
            d = 0.5 * (w[i].ga + w[i].gb) - o->inset;
            if (!(d > o->tol)) continue;
            qx[0] = x[i]; qz[0] = z[i]; qx[1] = x[j]; qz[1] = z[j];
            qx[2] = x[j] - m->s[w[i].seg].nx * (w[i].gb - o->inset); qz[2] = z[j] - m->s[w[i].seg].nz * (w[i].gb - o->inset);
            qx[3] = x[i] - m->s[w[i].seg].nx * (w[i].ga - o->inset); qz[3] = z[i] - m->s[w[i].seg].nz * (w[i].ga - o->inset);
            if (o->blocked(o->u, qx, qz)) { res->fate = TD5_MUNI_BLOCKED; return res->fate; }
        }
    (void)moved;
    /* merge points the move made coincident, then judge the ring */
    {
        double rx[MU_RING_MAX * 2], rz[MU_RING_MAX * 2], a1;
        int nr = 0;
        for (k = 0; k < nt; k++) {
            if (nr > 0 && hypot(tx[k] - rx[nr - 1], tz[k] - rz[nr - 1]) < MU_DUP) continue;
            rx[nr] = tx[k]; rz[nr] = tz[k]; nr++;
        }
        while (nr > 1 && hypot(rx[nr - 1] - rx[0], rz[nr - 1] - rz[0]) < MU_DUP) nr--;
        a1 = mu_area(rx, rz, nr);
        if (nr < 3 || nr > cap || !(a1 >= 0.5 * a0) || !mu_simple(rx, rz, nr)) { res->fate = TD5_MUNI_RING; return res->fate; }
        /* the whole result behind the line, every edge and every metre of it: the line bends between its pieces,
         * and an edge no wall rule moved (an angled wing, a short jog) must not be left on the pavement */
        if (!mu_all_behind(m, o, rx, rz, nr)) { res->fate = TD5_MUNI_FRONT; return res->fate; }
        for (k = 0; k < nr; k++) {
            ox[k] = flip ? rx[nr - 1 - k] : rx[k];
            oz[k] = flip ? rz[nr - 1 - k] : rz[k];
        }
        *on = nr;
    }
    res->fate = TD5_MUNI_SNAPPED;
    return res->fate;
}
