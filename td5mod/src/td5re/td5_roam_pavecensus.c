/**
 * td5_roam_pavecensus.c -- GEO FREE ROAM G4b: pavement vs ground-as-drawn census (PORT-ONLY, DEV-ONLY).
 *
 * Builds the SCENERY layer of every tile the caller hands over and walks the pavement faces
 * (pages SIDEWALK and the F4a lawn page) of its records. Each vertex is compared with the ground as
 * the renderer draws it (the caller's ground callback: lattice + G4 clamp + drop). Per face class:
 *   back   the vertical face under a slab's outer edge (colour 0xFFA8A8A8): gap = its bottom vertex
 *          above the drawn ground. > 0 is a hole between the face and the ground (the slab floats)
 *   slab   a white SIDEWALK quad whose vertices 2, 3 are the top points of back faces (a pavement slab
 *          of the F4b emitter: vertices 0, 1 on the kerb line, 2, 3 on the outer edge):
 *          rise  = outer edge - kerb edge (the cross-fall; the emitter caps it at +-0.25 * width)
 *          above = outer edge height above the drawn ground, minus the nominal (kerb height + drop)
 *   flat   any face with its normal up on those pages (slabs, footpaths, lawns): the lowest vertex under
 *          the drawn ground (the ground covers it)
 *   [G5] footpath  a footpath quad (uv u = 0 1 1 0 on any page) vertex against every pavement slab, island
 *          and lawn surface of ALL tiles (up-facing SIDEWALK / lawn-page faces that are not footpaths): a vertex
 *          inside one and under it (less than 1 cm over it) is a footpath hidden under the surface it crosses
 * Output <log>/roam_pavement_census.{txt,csv} (+ roam_footpath_census.csv). Knob in td5_roam_render.c:
 * TD5RE_ROAM_PAVE_CENSUS=1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_roam_pavecensus.h"
#include "td5_geo_frame.h"      /* [roam M] game x back to east for the lat/lon columns */

#ifndef TD5RE_RELEASE

#include "td5_trackgen_internal.h"

#define PC_UPM 430.0
#define PC_CM  (PC_UPM / 100.0)

typedef struct { float val, aux; double x, z, y; int tx, tz, cls; } PcRow;   /* cls 0 slab above, 1 back gap, 2 slab cross-fall, 3 sunk */

static const double k_thr_cm[6] = { 25, 50, 100, 200, 400, 800 };

typedef struct { uint64_t *k; uint32_t cap; } PcSet;

static uint64_t pc_key(float x, float z)
{
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &z, 4);
    return ((uint64_t)a << 32) | b;
}

static uint32_t pc_slot(const PcSet *s, uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (s->cap - 1); }

static void pc_set_add(PcSet *s, uint64_t key)
{
    uint32_t h = pc_slot(s, key);
    while (s->k[h] && s->k[h] != key + 1u) h = (h + 1) & (s->cap - 1);
    s->k[h] = key + 1u;
}

static int pc_set_has(const PcSet *s, uint64_t key)
{
    uint32_t h = pc_slot(s, key);
    while (s->k[h]) { if (s->k[h] == key + 1u) return 1; h = (h + 1) & (s->cap - 1); }
    return 0;
}

static uint32_t pc_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    pc_rdf(const uint8_t *p)  { float v; memcpy(&v, p, 4); return v; }

static int pc_cmp(const void *a, const void *b)
{
    const PcRow *x = (const PcRow *)a, *y = (const PcRow *)b;
    if (x->cls != y->cls) return x->cls < y->cls ? -1 : 1;
    return fabsf(x->val) > fabsf(y->val) ? -1 : (fabsf(x->val) < fabsf(y->val) ? 1 : 0);
}

/* [G5] surfaces (place-frame triangles) bucketed on an 8 m grid, footpath vertices */
#define PC_FP_CELL (8.0 * PC_UPM)
typedef struct { double x[3], y[3], z[3]; int lawn; } PcSurf;   /* lawn: 0 paving, 1 island / median lawn quad, 2 plaza lawn,
                                                                 * 3 [roam IA] any other up-facing face > 25 cm over the drawn
                                                                 * ground (a roof): median census cover only */
typedef struct { int64_t key; int s; } PcSurfRef;
typedef struct { double x, y, z; int tx, tz; } PcFpV;

static int64_t pc_cell_key(int a, int b) { return ((int64_t)a << 32) ^ (int64_t)(uint32_t)b; }

static int pc_ref_cmp(const void *a, const void *b)
{
    const PcSurfRef *x = (const PcSurfRef *)a, *y = (const PcSurfRef *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->s < y->s ? -1 : (x->s > y->s ? 1 : 0);
}

/* the surface height at (x, z) when the triangle holds it */
static int pc_surf_y(const PcSurf *t, double x, double z, double *y)
{
    const double d = (t->z[1] - t->z[2]) * (t->x[0] - t->x[2]) + (t->x[2] - t->x[1]) * (t->z[0] - t->z[2]);
    double w0, w1, w2;
    if (fabs(d) < 1e-9) return 0;
    w0 = ((t->z[1] - t->z[2]) * (x - t->x[2]) + (t->x[2] - t->x[1]) * (z - t->z[2])) / d;
    w1 = ((t->z[2] - t->z[0]) * (x - t->x[2]) + (t->x[0] - t->x[2]) * (z - t->z[2])) / d;
    w2 = 1.0 - w0 - w1;
    if (w0 < 1e-6 || w1 < 1e-6 || w2 < 1e-6) return 0;      /* strictly inside: a footpath meeting a slab edge is not under it */
    *y = w0 * t->y[0] + w1 * t->y[1] + w2 * t->y[2];
    return 1;
}

/* ============================================================================
 * [roam IA] MEDIAN census: the ground between two carriageways that nothing covers.
 *
 * Per tile, a 0.5 m cell raster over the tile box + PC_MED_WIDE: the drivable triangles of the 3x3 (ribbon
 * quads as the renderer's two triangles, junction patch triangles) and every up-facing pavement / island /
 * median lawn / plaza lawn triangle of ALL tiles (the footpath block's buckets, so a slab the neighbour owns
 * counts). A nearest-road transform gives each cell its distance d to the closest road cell and the way back
 * to it; one ray from the cell straight AWAY from that road finds the opposite carriageway at d2. A cell of
 * this tile with d + d2 <= PC_MED_NARROW is in a MEDIAN / traffic island (a band no wider than two maximum
 * sidewalks), <= PC_MED_WIDE a wide island or a narrow wedge (it may hold buildings). Uncovered = no
 * pavement or lawn triangle over the cell centre: the drawn ground (a paved lot, grass) shows there, 40+ cm
 * under the slabs round it. Hot spots = 8 m buckets of uncovered narrow cells.
 * ========================================================================== */
#define PC_MC          (0.5 * PC_UPM)
#define PC_MED_NARROW  (12.0 * PC_UPM)
#define PC_MED_WIDE    (25.0 * PC_UPM)
#define PC_MED_BUCKET  (8.0 * PC_UPM)

static const RoamGrid *s_pc_grid;      /* set by the caller for one census run, NULL = no median census */
void td5_roam_pave_census_set_grid(const RoamGrid *grid) { s_pc_grid = grid; }

typedef struct { int64_t key; long n; double sx, sz, sgap; } PcMedHot;

static int pc_hot_cmp(const void *a, const void *b)
{
    const PcMedHot *x = (const PcMedHot *)a, *y = (const PcMedHot *)b;
    return x->n > y->n ? -1 : (x->n < y->n ? 1 : (x->key < y->key ? -1 : (x->key > y->key)));
}

static void pc_ras_tri(uint8_t *f, int nx, int nz, double x0, double z0, const double *X, const double *Z, uint8_t bit)
{
    double ax = X[0], bx = X[0], az = Z[0], bz = Z[0], d;
    int i0, i1, j0, j1, i, j, q;
    for (q = 1; q < 3; q++) {
        if (X[q] < ax) ax = X[q];
        if (X[q] > bx) bx = X[q];
        if (Z[q] < az) az = Z[q];
        if (Z[q] > bz) bz = Z[q];
    }
    d = (Z[1] - Z[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Z[0] - Z[2]);
    if (fabs(d) < 1e-9) return;
    i0 = (int)floor((ax - x0) / PC_MC - 0.5); i1 = (int)ceil((bx - x0) / PC_MC - 0.5);
    j0 = (int)floor((az - z0) / PC_MC - 0.5); j1 = (int)ceil((bz - z0) / PC_MC - 0.5);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > nx - 1) i1 = nx - 1;
    if (j1 > nz - 1) j1 = nz - 1;
    for (j = j0; j <= j1; j++)
        for (i = i0; i <= i1; i++) {
            const double x = x0 + (i + 0.5) * PC_MC, z = z0 + (j + 0.5) * PC_MC;
            const double w0 = ((Z[1] - Z[2]) * (x - X[2]) + (X[2] - X[1]) * (z - Z[2])) / d;
            const double w1 = ((Z[2] - Z[0]) * (x - X[2]) + (X[0] - X[2]) * (z - Z[2])) / d;
            if (w0 >= 0.0 && w1 >= 0.0 && w0 + w1 <= 1.0) f[(size_t)j * nx + i] |= bit;
        }
}

static void pc_ras_road(const RoamTile *t, uint8_t *f, int nx, int nz, double x0, double z0)
{
    uint32_t s;
    for (s = 0; s < t->nspan; s++) {
        const TD5_StripSpan *sp = &t->span[s];
        const uint32_t L = sp->left_vertex_index, R = sp->right_vertex_index;
        const int n = (int)(sp->pad_02[1] & 0x0Fu);
        int kk;
        if (n < 1 || n >= 16 || L + (uint32_t)n >= t->nvert || R + (uint32_t)n >= t->nvert) continue;
        for (kk = 0; kk < n; kk++) {
            const TD5_StripVertex *v4[4] = { &t->vert[L + kk], &t->vert[L + kk + 1], &t->vert[R + kk + 1], &t->vert[R + kk] };
            double X[4], Z[4];
            int c;
            for (c = 0; c < 4; c++) {
                X[c] = (double)t->org_x + sp->origin_x + v4[c]->x;
                Z[c] = (double)t->org_z + sp->origin_z + v4[c]->z;
            }
            pc_ras_tri(f, nx, nz, x0, z0, X, Z, 1);
            { const double X2[3] = { X[0], X[2], X[3] }, Z2[3] = { Z[0], Z[2], Z[3] }; pc_ras_tri(f, nx, nz, x0, z0, X2, Z2, 1); }
        }
    }
    for (s = 0; s < t->ntri; s++) {
        const RoamTri *tr = &t->tri[s];
        double X[3], Z[3];
        int c;
        if (tr->v[0] >= t->nvert || tr->v[1] >= t->nvert || tr->v[2] >= t->nvert) continue;
        for (c = 0; c < 3; c++) {
            X[c] = (double)t->org_x + tr->ox + t->vert[tr->v[c]].x;
            Z[c] = (double)t->org_z + tr->oz + t->vert[tr->v[c]].z;
        }
        pc_ras_tri(f, nx, nz, x0, z0, X, Z, 1);
    }
}

static void pc_median_census(const RoamTile *tiles, int ntile, const PcSurf *surf, const PcSurfRef *ref, long nref,
                             const double proj[4], const char *ld)
{
    const RoamGrid *g = s_pc_grid;
    static const double k_bin_m[7] = { 3, 6, 9, 12, 18, 25, 1e9 };
    int k, tx0 = 1 << 30, tz0 = 1 << 30, tx1 = -(1 << 30), tz1 = -(1 << 30), tw, th;
    int *slot;
    double area[2] = { 0, 0 }, unc[2] = { 0, 0 }, bin_a[7] = { 0 }, bin_u[7] = { 0 };
    PcMedHot *hot = NULL;
    long nhot = 0, caphot = 0;
    char path[600];
    FILE *o, *csv;
    uint32_t t0 = td5_plat_time_ms();
    if (!g) return;
    for (k = 0; k < ntile; k++) {
        if (tiles[k].tx < tx0) tx0 = tiles[k].tx;
        if (tiles[k].tz < tz0) tz0 = tiles[k].tz;
        if (tiles[k].tx > tx1) tx1 = tiles[k].tx;
        if (tiles[k].tz > tz1) tz1 = tiles[k].tz;
    }
    if (ntile <= 0) return;
    tw = tx1 - tx0 + 1; th = tz1 - tz0 + 1;
    slot = (int *)malloc((size_t)tw * th * sizeof(int));
    if (!slot) return;
    for (k = 0; k < tw * th; k++) slot[k] = -1;
    for (k = 0; k < ntile; k++) slot[(tiles[k].tz - tz0) * tw + (tiles[k].tx - tx0)] = k;
    for (k = 0; k < ntile; k++) {
        const RoamTile *t = &tiles[k];
        double cor[4][2], x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
        int nx, nz, i, j, dx, dz, a, b, nb;
        uint8_t *f;
        int32_t *nr;
        long *bk;
        roam_grid_corners(g, t->tx, t->tz, cor);
        for (i = 0; i < 4; i++) {
            if (cor[i][0] < x0) x0 = cor[i][0];
            if (cor[i][0] > x1) x1 = cor[i][0];
            if (cor[i][1] < z0) z0 = cor[i][1];
            if (cor[i][1] > z1) z1 = cor[i][1];
        }
        x0 -= PC_MED_WIDE + PC_UPM; z0 -= PC_MED_WIDE + PC_UPM; x1 += PC_MED_WIDE + PC_UPM; z1 += PC_MED_WIDE + PC_UPM;
        nx = (int)ceil((x1 - x0) / PC_MC); nz = (int)ceil((z1 - z0) / PC_MC);
        nb = (int)ceil((x1 - x0) / PC_MED_BUCKET) * (int)ceil((z1 - z0) / PC_MED_BUCKET);
        f = (uint8_t *)calloc((size_t)nx * nz, 1);
        nr = (int32_t *)malloc((size_t)nx * nz * sizeof(int32_t));
        bk = (long *)calloc((size_t)nb * 4u, sizeof(long));   /* n, sum x, sum z, sum gap (x, z, gap as longs of units) */
        if (!f || !nr || !bk) { free(f); free(nr); free(bk); break; }
        for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
            const int ax = t->tx + dx, az = t->tz + dz;
            if (ax < tx0 || az < tz0 || ax > tx1 || az > tz1 || slot[(az - tz0) * tw + (ax - tx0)] < 0) continue;
            pc_ras_road(&tiles[slot[(az - tz0) * tw + (ax - tx0)]], f, nx, nz, x0, z0);
        }
        for (b = (int)floor(z0 / PC_FP_CELL); b <= (int)floor(z1 / PC_FP_CELL); b++)
            for (a = (int)floor(x0 / PC_FP_CELL); a <= (int)floor(x1 / PC_FP_CELL); a++) {
                const int64_t key = pc_cell_key(a, b);
                long lo = 0, hi = nref, r;
                while (lo < hi) { const long mid = (lo + hi) / 2; if (ref[mid].key < key) lo = mid + 1; else hi = mid; }
                for (r = lo; r < nref && ref[r].key == key; r++) pc_ras_tri(f, nx, nz, x0, z0, surf[ref[r].s].x, surf[ref[r].s].z, 2);
            }
        /* nearest road cell: two raster passes (dead reckoning), exact distance to the carried cell */
        for (i = 0; i < nx * nz; i++) nr[i] = (f[i] & 1) ? i : -1;
        {
            static const int fw[4][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 } };
            int pass;
            for (pass = 0; pass < 2; pass++) {
                const int js = pass ? nz - 1 : 0, je = pass ? -1 : nz, st = pass ? -1 : 1;
                for (j = js; j != je; j += st)
                    for (i = pass ? nx - 1 : 0; pass ? i >= 0 : i < nx; i += st) {
                        const int c = j * nx + i;
                        int q;
                        double best = 1e300;
                        if (nr[c] >= 0) { const int ri = nr[c] % nx, rj = nr[c] / nx; best = (double)(ri - i) * (ri - i) + (double)(rj - j) * (rj - j); }
                        for (q = 0; q < 4; q++) {
                            const int ni = i + (pass ? -fw[q][0] : fw[q][0]), nj = j + (pass ? -fw[q][1] : fw[q][1]);
                            int cand, ri, rj;
                            double dd;
                            if (ni < 0 || nj < 0 || ni >= nx || nj >= nz) continue;
                            cand = nr[nj * nx + ni];
                            if (cand < 0) continue;
                            ri = cand % nx; rj = cand / nx;
                            dd = (double)(ri - i) * (ri - i) + (double)(rj - j) * (rj - j);
                            if (dd < best) { best = dd; nr[c] = cand; }
                        }
                    }
            }
        }
        {   /* [roam IA] DEV TD5RE_ROAM_MEDIAN_DUMP=x,z: the raster round a place point as a PPM in the log dir
             * (black road, green cover, white other ground) */
            const char *q = getenv("TD5RE_ROAM_MEDIAN_DUMP");
            double qx, qz;
            if (q && sscanf(q, "%lf,%lf", &qx, &qz) == 2 && qx >= x0 && qx < x1 && qz >= z0 && qz < z1) {
                const int ci = (int)((qx - x0) / PC_MC), cj = (int)((qz - z0) / PC_MC), R = 120;
                char dp[600];
                FILE *pf;
                snprintf(dp, sizeof dp, "%sroam_median_dump_%d_%d.ppm", ld ? ld : "", t->tx, t->tz);
                if ((pf = fopen(dp, "wb")) != NULL) {
                    int ii, jj;
                    fprintf(pf, "P6 %d %d 255\n", 2 * R, 2 * R);
                    for (jj = cj + R - 1; jj >= cj - R; jj--)
                        for (ii = ci - R; ii < ci + R; ii++) {
                            unsigned char rgb[3] = { 128, 128, 255 };
                            if (ii >= 0 && jj >= 0 && ii < nx && jj < nz) {
                                const uint8_t fl = f[(size_t)jj * nx + ii];
                                rgb[0] = rgb[1] = rgb[2] = 255;
                                if (fl & 2) { rgb[0] = 60; rgb[1] = 200; rgb[2] = 60; }
                                if (fl & 1) { rgb[0] = rgb[1] = rgb[2] = 0; }
                                if (ii == ci && jj == cj) { rgb[0] = 255; rgb[1] = 0; rgb[2] = 0; }
                            }
                            fwrite(rgb, 1, 3, pf);
                        }
                    fclose(pf);
                }
            }
        }
        for (j = 0; j < nz; j++)
            for (i = 0; i < nx; i++) {
                const int c = j * nx + i;
                const double x = x0 + (i + 0.5) * PC_MC, z = z0 + (j + 0.5) * PC_MC;
                double d, ux, uz, gap = -1.0, s;
                int otx, otz, cls, bin;
                if ((f[c] & 1) || nr[c] < 0) continue;
                d = PC_MC * hypot((double)(nr[c] % nx - i), (double)(nr[c] / nx - j));
                if (d > PC_MED_WIDE) continue;
                roam_grid_tile_of(g, x, z, &otx, &otz);
                if (otx != t->tx || otz != t->tz) continue;
                ux = (double)(i - nr[c] % nx) * PC_MC / d; uz = (double)(j - nr[c] / nx) * PC_MC / d;
                for (s = 0.5 * PC_MC; d + s <= PC_MED_WIDE; s += 0.5 * PC_MC) {
                    const int ii = (int)floor((x + ux * s - x0) / PC_MC), jj = (int)floor((z + uz * s - z0) / PC_MC);
                    if (ii < 0 || jj < 0 || ii >= nx || jj >= nz) break;
                    if (f[(size_t)jj * nx + ii] & 1) { gap = d + s; break; }
                }
                if (gap < 0.0) continue;
                cls = gap <= PC_MED_NARROW ? 0 : 1;
                for (bin = 0; gap > k_bin_m[bin] * PC_UPM; bin++) { }
                area[cls] += 0.25; bin_a[bin] += 0.25;
                if (f[c] & 2) continue;
                unc[cls] += 0.25; bin_u[bin] += 0.25;
                if (cls == 0) {
                    const int bi = (int)floor((x - x0) / PC_MED_BUCKET), bj = (int)floor((z - z0) / PC_MED_BUCKET);
                    const int bw = (int)ceil((x1 - x0) / PC_MED_BUCKET), q = bj * bw + bi;
                    bk[q * 4 + 0]++; bk[q * 4 + 1] += (long)(x - x0); bk[q * 4 + 2] += (long)(z - z0); bk[q * 4 + 3] += (long)gap;
                }
            }
        {
            const int bw = (int)ceil((x1 - x0) / PC_MED_BUCKET);
            int q;
            for (q = 0; q < nb; q++) {
                if (!bk[q * 4]) continue;
                if (nhot == caphot) {
                    PcMedHot *nh;
                    caphot = caphot ? caphot * 2 : 4096;
                    if (!(nh = (PcMedHot *)realloc(hot, (size_t)caphot * sizeof *hot))) { nhot = 0; break; }
                    hot = nh;
                }
                hot[nhot].key = ((int64_t)t->tx << 40) ^ ((int64_t)(t->tz & 0xFFFFF) << 20) ^ q;
                hot[nhot].n = bk[q * 4];
                hot[nhot].sx = x0 + (double)bk[q * 4 + 1] / bk[q * 4];
                hot[nhot].sz = z0 + (double)bk[q * 4 + 2] / bk[q * 4];
                hot[nhot].sgap = (double)bk[q * 4 + 3] / bk[q * 4];
                nhot++;
                (void)bw;
            }
        }
        free(f); free(nr); free(bk);
    }
    free(slot);
    if (nhot) qsort(hot, (size_t)nhot, sizeof *hot, pc_hot_cmp);
    snprintf(path, sizeof path, "%sroam_median_census.txt", ld ? ld : "");
    o = fopen(path, "w");
    snprintf(path, sizeof path, "%sroam_median_census.csv", ld ? ld : "");
    csv = fopen(path, "w");
    if (o) {
        int q;
        fprintf(o, "roam MEDIAN census (IA): %d tiles, 0.5 m cells between two carriageways (one ray away from the nearest road)\n", ntile);
        fprintf(o, "NARROW (gap <= %.0f m, medians + islands): %.0f m2, UNCOVERED %.0f m2 (%.2f %%)\n", PC_MED_NARROW / PC_UPM,
                area[0], unc[0], area[0] > 0 ? 100.0 * unc[0] / area[0] : 0.0);
        fprintf(o, "WIDE (%.0f < gap <= %.0f m, wide islands + narrow wedges, may hold buildings): %.0f m2, uncovered %.0f m2 (%.2f %%)\n",
                PC_MED_NARROW / PC_UPM, PC_MED_WIDE / PC_UPM, area[1], unc[1], area[1] > 0 ? 100.0 * unc[1] / area[1] : 0.0);
        fprintf(o, "by gap width (m): bin_upper area_m2 uncovered_m2\n");
        for (q = 0; q < 6; q++) fprintf(o, "  %4.0f %10.0f %10.0f\n", k_bin_m[q], bin_a[q], bin_u[q]);
        fprintf(o, "hot spots (8 m buckets of uncovered NARROW cells, most first): uncovered_m2 mean_gap_m tile place_x place_z lat lon\n");
        for (q = 0; q < nhot && q < 60; q++)
            fprintf(o, "%.1f %.1f %lld %.0f %.0f %.6f %.6f\n", hot[q].n * 0.25, hot[q].sgap / PC_UPM, (long long)(hot[q].key >> 40),
                    hot[q].sx, hot[q].sz, proj && proj[1] > 0 ? proj[0] + hot[q].sz / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(hot[q].sx) / (proj[3] * PC_UPM) : 0.0);
        fclose(o);
    }
    if (csv) {
        long q;
        fprintf(csv, "uncovered_m2,mean_gap_m,place_x,place_z,lat,lon\n");
        for (q = 0; q < nhot; q++)
            fprintf(csv, "%.2f,%.2f,%.0f,%.0f,%.6f,%.6f\n", hot[q].n * 0.25, hot[q].sgap / PC_UPM, hot[q].sx, hot[q].sz,
                    proj && proj[1] > 0 ? proj[0] + hot[q].sz / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(hot[q].sx) / (proj[3] * PC_UPM) : 0.0);
        fclose(csv);
    }
    fprintf(stderr, "MEDIANCENSUS: narrow %.0f m2 uncovered %.0f m2 (%.2f %%), wide %.0f m2 uncovered %.0f m2, %ld hot buckets, %u ms\n",
            area[0], unc[0], area[0] > 0 ? 100.0 * unc[0] / area[0] : 0.0, area[1], unc[1], nhot, (unsigned)(td5_plat_time_ms() - t0));
    free(hot);
}

int td5_roam_pave_census(const RoamTile *tiles, int ntile, TD5_RoamScnSource *scn, TD5_RoamHeight *hc,
                         TD5_RoamGroundFn ground, void *gu, double drop_units, const double proj[4], const char *outprefix)
{
    const double nominal = TD5_TG_KERB_H + drop_units;
    long touch = 0, nflat = 0, nback = 0, nslab = 0, nvert = 0, nnog = 0, sink0 = 0, gap0 = 0;
    long hab[6] = { 0 }, hgap[6] = { 0 }, hrise[6] = { 0 }, hfall[6] = { 0 };
    double worst_above = -1e30, worst_gap = -1e30, worst_rise = -1e30, worst_fall = 1e30, lo_min = 1e30;
    PcRow *keep = NULL;
    long nkeep = 0, capkeep = 0;
    int k, tn = 0;
    char path[600];
    FILE *o, *csv;
    const char *ld = td5_plat_log_dir();
    uint32_t t0 = td5_plat_time_ms();
    PcSet set;
    double dbg_x = 0.0, dbg_z = 0.0, dbg_r = 0.0;
    PcSurf *surf = NULL; long nsurf = 0, capsurf = 0;
    long fp_on = 0, fp_under = 0, fp_under_lawn = 0, nfprow = 0, fp_by[3][4] = { { 0 } };
    double fp_lo = 1e30;
    PcRow *fprow = (PcRow *)malloc(4096u * sizeof(PcRow));
    PcFpV *fpv = NULL; long nfpv = 0, capfpv = 0;
    { const char *e = getenv("TD5RE_PAVE_DBG"); if (e) sscanf(e, "%lf,%lf,%lf", &dbg_x, &dbg_z, &dbg_r); }

    set.cap = 1u << 18;
    set.k = (uint64_t *)malloc((size_t)set.cap * sizeof(uint64_t));
    if (!set.k) return 1;

#define PC_KEEP(CLS, VAL, AUX, X, Y, Z) do { \
        if (nkeep == capkeep) { capkeep = capkeep ? capkeep * 2 : 4096; keep = (PcRow *)realloc(keep, (size_t)capkeep * sizeof *keep); \
            if (!keep) return 1; } \
        keep[nkeep].val = (float)(VAL); keep[nkeep].aux = (float)(AUX); keep[nkeep].x = (X); keep[nkeep].y = (Y); keep[nkeep].z = (Z); \
        keep[nkeep].tx = sc->tx; keep[nkeep].tz = sc->tz; keep[nkeep].cls = (CLS); nkeep++; } while (0)

    for (k = 0; k < ntile; k++) {
        const RoamTile *t = &tiles[k];
        TD5_RoamScenery *sc = td5_roam_scn_build(scn, t, hc);
        uint32_t r;
        int pass;
        if (!sc) continue;
        tn++;
        memset(set.k, 0, (size_t)set.cap * sizeof(uint64_t));
        for (pass = 0; pass < 2; pass++) {
            for (r = 0; r < sc->nrec; r++) {
                const uint8_t *rec = sc->blob + sc->rec_off[r];
                const size_t avail = sc->blob_bytes - sc->rec_off[r];
                const uint32_t ncmd = pc_rd32(rec + 4), nv = pc_rd32(rec + 8);
                const uint32_t cmd_off = pc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = pc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
                uint32_t ci, cursor = 0;
                if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) continue;
                for (ci = 0; ci < ncmd; ci++) {
                    const uint8_t *cm = rec + cmd_off + ci * 16u;
                    const int page = (int)(int16_t)(pc_rd32(cm) >> 16);
                    const uint32_t tc = pc_rd32(cm + 8) & 0xFFFFu, qc = pc_rd32(cm + 8) >> 16;
                    const int ours = page == TD5_TG_PAGE_SIDEWALK || page == TD5_TG_PAGE_R3_BLOCK + 0;
                    uint32_t p;
                    for (p = 0; p < tc + qc; p++) {
                        const int nvp = p < tc ? 3 : 4;
                        double x[4], y[4], z[4], g[4], rel[4];
                        float fx[4], fz[4];
                        uint32_t col = 0;
                        int q, okv = 1;
                        if (cursor + (uint32_t)nvp > nv) break;
                        for (q = 0; q < nvp; q++) {
                            const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)q) * 44u;
                            fx[q] = pc_rdf(v); fz[q] = pc_rdf(v + 8);
                            x[q] = (double)fx[q] + (double)sc->org_x;
                            y[q] = (double)pc_rdf(v + 4);
                            z[q] = (double)fz[q] + (double)sc->org_z;
                            if (q == 0) col = pc_rd32(v + 24);
                        }
                        cursor += (uint32_t)nvp;
                        if (pass == 0) {   /* [G5] footpath quads (u = 0 1 1 0) and the surfaces they may cross */
                            float u4[4], v4[4];
                            int fp;
                            for (q = 0; q < nvp; q++) {
                                u4[q] = pc_rdf(rec + vtx_off + (cursor - (uint32_t)nvp + (uint32_t)q) * 44u + 28);
                                v4[q] = pc_rdf(rec + vtx_off + (cursor - (uint32_t)nvp + (uint32_t)q) * 44u + 32);
                            }
                            /* sc_emit_footpaths: u 0 1 1 0, v the run along (same at both corners of an end), on the
                             * paving or the loose-ground page, at least the narrowest footpath wide */
                            fp = nvp == 4 && (page == TD5_TG_PAGE_SIDEWALK || page == k_biomes[0].ground_page) &&
                                 col == 0xFFFFFFFFu && u4[0] == 0.0f && u4[1] == 1.0f && u4[2] == 1.0f && u4[3] == 0.0f &&
                                 v4[0] == v4[1] && v4[2] == v4[3] && v4[2] > v4[0] &&
                                 hypot(x[1] - x[0], z[1] - z[0]) > 0.7 * TD5_TG_INFRA_M;
                            if (fp) {
                                if (nfpv + 4 > capfpv) {
                                    PcFpV *nf;
                                    capfpv = capfpv ? capfpv * 2 : 65536;
                                    if (!(nf = (PcFpV *)realloc(fpv, (size_t)capfpv * sizeof *fpv))) return 1;
                                    fpv = nf;
                                }
                                for (q = 0; q < 4; q++) {
                                    fpv[nfpv].x = x[q]; fpv[nfpv].y = y[q]; fpv[nfpv].z = z[q];
                                    fpv[nfpv].tx = sc->tx; fpv[nfpv].tz = sc->tz; nfpv++;
                                }
                            } else if ((page == TD5_TG_PAGE_SIDEWALK && col == 0xFFFFFFFFu) || page == TD5_TG_PAGE_R3_BLOCK + 0 ||
                                       s_pc_grid) {   /* [roam IA] + raised faces (roofs) for the median census */
                                const double ax = x[1] - x[0], ay = y[1] - y[0], az = z[1] - z[0];
                                const double bx = x[2] - x[0], by = y[2] - y[0], bz = z[2] - z[0];
                                const double ny = az * bx - ax * bz, nl = sqrt((ay * bz - az * by) * (ay * bz - az * by) + ny * ny +
                                                                              (ax * by - ay * bx) * (ax * by - ay * bx));
                                int tt;
                                const int pave = (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFFFFFFFu) || page == TD5_TG_PAGE_R3_BLOCK + 0;
                                int raised = 0;
                                if (!pave && nl > 0.0 && fabs(ny) / nl > 0.7) {      /* [roam IA] a roof, not a city lot */
                                    double gg;
                                    raised = 1;
                                    for (q = 0; q < nvp && raised; q++)
                                        if (!ground(gu, x[q], z[q], &gg) || y[q] - gg < 25.0 * PC_CM) raised = 0;
                                }
                                if (nl > 0.0 && fabs(ny) / nl > 0.7 && (pave || raised))
                                    for (tt = 0; tt < nvp - 2; tt++) {
                                        const int i1 = tt + 1, i2 = tt + 2;
                                        PcSurf *f;
                                        if (nsurf == capsurf) {
                                            PcSurf *ns;
                                            capsurf = capsurf ? capsurf * 2 : 65536;
                                            if (!(ns = (PcSurf *)realloc(surf, (size_t)capsurf * sizeof *surf))) return 1;
                                            surf = ns;
                                        }
                                        f = &surf[nsurf++];
                                        f->x[0] = x[0]; f->y[0] = y[0]; f->z[0] = z[0];
                                        f->x[1] = x[i1]; f->y[1] = y[i1]; f->z[1] = z[i1];
                                        f->x[2] = x[i2]; f->y[2] = y[i2]; f->z[2] = z[i2];
                                        f->lawn = !pave ? 3 : page == TD5_TG_PAGE_R3_BLOCK + 0 ? (nvp == 4 ? 1 : 2) : 0;
                                    }
                            }
                        }
                        if (!ours) continue;
                        if (pass == 0) {
                            if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFA8A8A8u && nvp == 4) {
                                pc_set_add(&set, pc_key(fx[0], fz[0]));
                                pc_set_add(&set, pc_key(fx[1], fz[1]));
                            }
                            continue;
                        }
                        if (dbg_r > 0.0 && hypot(x[0] - dbg_x, z[0] - dbg_z) < dbg_r) {
                            double gg[4]; int okk[4];
                            for (q = 0; q < nvp; q++) okk[q] = ground(gu, x[q], z[q], &gg[q]);
                            fprintf(stderr, "PAVEDBG page %d col %08x n%d:", page, col, nvp);
                            for (q = 0; q < nvp; q++) fprintf(stderr, " (%.0f %.0f y%.0f g%s%.0f)", x[q], z[q], y[q], okk[q] ? "" : "!", okk[q] ? gg[q] : 0.0);
                            fputc(10, stderr);
                        }
                        for (q = 0; q < nvp; q++) {
                            if (!ground(gu, x[q], z[q], &g[q])) { okv = 0; break; }
                            rel[q] = y[q] - g[q];
                        }
                        nvert += nvp;
                        if (!okv) { nnog++; continue; }
                        {
                            const double ax = x[1] - x[0], ay = y[1] - y[0], az = z[1] - z[0];
                            const double bx = x[nvp - 1] - x[0], by = y[nvp - 1] - y[0], bz = z[nvp - 1] - z[0];
                            const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                            const double nl = sqrt(nx * nx + ny * ny + nz * nz);
                            const int up = nl > 0.0 && fabs(ny) / nl > 0.7;
                            if (up) {
                                double lo = 1e30;
                                for (q = 0; q < nvp; q++) if (rel[q] < lo) lo = rel[q];
                                nflat++;
                                if (lo < lo_min) lo_min = lo;
                                if (lo < 0.0) { sink0++; PC_KEEP(3, lo / PC_CM, 0, x[0], y[0], z[0]); }
                                else if (lo < 10.0 * PC_CM) { touch++; PC_KEEP(4, lo / PC_CM, 0, x[0], y[0], z[0]); }   /* z-fights the ground */
                                if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFFFFFFFu && nvp == 4 &&
                                    pc_set_has(&set, pc_key(fx[2], fz[2])) && pc_set_has(&set, pc_key(fx[3], fz[3]))) {
                                    const double rise = 0.5 * (y[2] + y[3]) - 0.5 * (y[0] + y[1]);
                                    const double above = (rel[2] > rel[3] ? rel[2] : rel[3]) - nominal;
                                    nslab++;
                                    if (above > worst_above) worst_above = above;
                                    if (rise > worst_rise) worst_rise = rise;
                                    if (rise < worst_fall) worst_fall = rise;
                                    for (q = 0; q < 6; q++) {
                                        if (above > k_thr_cm[q] * PC_CM) hab[q]++;
                                        if (rise > k_thr_cm[q] * PC_CM) hrise[q]++;
                                        if (-rise > k_thr_cm[q] * PC_CM) hfall[q]++;
                                    }
                                    if (above > 25.0 * PC_CM) PC_KEEP(0, above / PC_CM, rise / PC_CM, x[2], y[2], z[2]);
                                    if (rise > 25.0 * PC_CM || -rise > 25.0 * PC_CM) PC_KEEP(2, rise / PC_CM, above / PC_CM, x[2], y[2], z[2]);
                                }
                            } else if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFA8A8A8u) {
                                double bot = 1e30;
                                int bq = 0;
                                for (q = 0; q < nvp; q++) if (rel[q] < bot) { bot = rel[q]; bq = q; }
                                nback++;
                                if (bot > worst_gap) worst_gap = bot;
                                if (bot > 0.0) gap0++;
                                for (q = 0; q < 6; q++) if (bot > k_thr_cm[q] * PC_CM) hgap[q]++;
                                if (bot > 0.0) PC_KEEP(1, bot / PC_CM, 0, x[bq], y[bq], z[bq]);
                            }
                        }
                    }
                }
            }
        }
        td5_roam_scn_free(sc);
    }
    /* [G5] footpath vertices under a pavement / island / lawn surface */
    {
        PcSurfRef *ref = NULL;
        long nref = 0, capref = 0, si, fi;
        for (si = 0; si < nsurf; si++) {
            const PcSurf *t = &surf[si];
            double x0 = t->x[0], x1 = t->x[0], z0 = t->z[0], z1 = t->z[0];
            int a, b, q;
            for (q = 1; q < 3; q++) {
                if (t->x[q] < x0) x0 = t->x[q];
                if (t->x[q] > x1) x1 = t->x[q];
                if (t->z[q] < z0) z0 = t->z[q];
                if (t->z[q] > z1) z1 = t->z[q];
            }
            if (x1 - x0 > 64.0 * PC_FP_CELL || z1 - z0 > 64.0 * PC_FP_CELL) continue;
            for (b = (int)floor(z0 / PC_FP_CELL); b <= (int)floor(z1 / PC_FP_CELL); b++)
                for (a = (int)floor(x0 / PC_FP_CELL); a <= (int)floor(x1 / PC_FP_CELL); a++) {
                    if (nref == capref) {
                        PcSurfRef *nr;
                        capref = capref ? capref * 2 : 262144;
                        if (!(nr = (PcSurfRef *)realloc(ref, (size_t)capref * sizeof *ref))) return 1;
                        ref = nr;
                    }
                    ref[nref].key = pc_cell_key(a, b); ref[nref].s = (int)si; nref++;
                }
        }
        if (nref) qsort(ref, (size_t)nref, sizeof *ref, pc_ref_cmp);
        for (fi = 0; fi < nfpv; fi++) {
            const PcFpV *v = &fpv[fi];
            const int64_t key = pc_cell_key((int)floor(v->x / PC_FP_CELL), (int)floor(v->z / PC_FP_CELL));
            long lo = 0, hi = nref, k2;
            double top = -1e300;
            int on = 0, lawn = 0;
            while (lo < hi) { const long mid = (lo + hi) / 2; if (ref[mid].key < key) lo = mid + 1; else hi = mid; }
            for (k2 = lo; k2 < nref && ref[k2].key == key; k2++) {
                double ys;
                if (surf[ref[k2].s].lawn == 3) continue;              /* [roam IA] roofs: median census only */
                if (pc_surf_y(&surf[ref[k2].s], v->x, v->z, &ys) && ys > top) {
                    const PcSurf *t = &surf[ref[k2].s];
                    top = ys; on = 1; lawn = t->lawn;
                    if (dbg_r > 0.0 && hypot(v->x - dbg_x, v->z - dbg_z) < dbg_r)
                        fprintf(stderr, "FPDBG fp (%.0f %.0f y%.0f) surf %d y%.0f: (%.0f %.0f %.0f)(%.0f %.0f %.0f)(%.0f %.0f %.0f)\n",
                                v->x, v->z, v->y, t->lawn, ys, t->x[0], t->y[0], t->z[0], t->x[1], t->y[1], t->z[1], t->x[2], t->y[2], t->z[2]);
                }
            }
            if (!on) continue;
            fp_on++;
            if (v->y - top < fp_lo) fp_lo = v->y - top;
            if (v->y < top + 1.0 * PC_CM) {
                fp_under++;
                if (lawn) fp_under_lawn++;
                {
                    static const double k_fp_cm[4] = { -1, 10, 50, 100 };
                    int h2;
                    for (h2 = 0; h2 < 4; h2++) if (top - v->y > k_fp_cm[h2] * PC_CM) fp_by[lawn][h2]++;
                }
                if (fprow && nfprow < 4096) {
                    fprow[nfprow].val = (float)((v->y - top) / PC_CM); fprow[nfprow].aux = (float)lawn;
                    fprow[nfprow].x = v->x; fprow[nfprow].y = v->y; fprow[nfprow].z = v->z;
                    fprow[nfprow].tx = v->tx; fprow[nfprow].tz = v->tz; fprow[nfprow].cls = 5; nfprow++;
                }
            }
        }
        pc_median_census(tiles, ntile, surf, ref, nref, proj, ld);   /* [roam IA] */
        {   /* [roam IA] LAWN poke census: the drawn ground over a lawn's INTERIOR (the vertex check above only sees
             * corners). 21 barycentric samples per lawn triangle (plaza lawn 2, island / median lawn 1): a sample
             * where the ground is less than 1 cm under the lawn shows grass through grass ("hay pasto debajo del
             * pasto?"). Area-weighted. */
            double area_s[3] = { 0, 0, 0 }, poke_s[3] = { 0, 0, 0 }, worst[3] = { -1e30, -1e30, -1e30 };
            double wx[3] = { 0, 0, 0 }, wz[3] = { 0, 0, 0 };
            long si2;
            for (si2 = 0; si2 < nsurf; si2++) {
                const PcSurf *t2 = &surf[si2];
                const int L2 = t2->lawn;
                double ar, nsamp = 0, npoke = 0;
                int a2, b2;
                if (L2 != 1 && L2 != 2 && L2 != 0) continue;
                ar = 0.5 * fabs((t2->x[1] - t2->x[0]) * (t2->z[2] - t2->z[0]) - (t2->z[1] - t2->z[0]) * (t2->x[2] - t2->x[0])) / (PC_UPM * PC_UPM);
                for (a2 = 0; a2 <= 5; a2++) for (b2 = 0; a2 + b2 <= 5; b2++) {
                    const double w0 = (a2 + 0.25) / 6.25, w1 = (b2 + 0.25) / 6.25, w2 = 1.0 - w0 - w1;
                    double g, x2, z2, y2;
                    if (w2 < 0.0) continue;
                    x2 = w0 * t2->x[0] + w1 * t2->x[1] + w2 * t2->x[2];
                    z2 = w0 * t2->z[0] + w1 * t2->z[1] + w2 * t2->z[2];
                    y2 = w0 * t2->y[0] + w1 * t2->y[1] + w2 * t2->y[2];
                    if (!ground(gu, x2, z2, &g)) continue;
                    nsamp++;
                    if (g > y2 - 1.0 * PC_CM) npoke++;
                    if (g - y2 > worst[L2]) { worst[L2] = g - y2; wx[L2] = x2; wz[L2] = z2; }
                }
                if (nsamp > 0) { area_s[L2] += ar; poke_s[L2] += ar * npoke / nsamp; }
            }
            {
                char lp[600];
                FILE *lf;
                snprintf(lp, sizeof lp, "%sroam_lawn_census.txt", ld ? ld : "");
                if ((lf = fopen(lp, "w")) != NULL) {
                    int q3;
                    static const char *const nm[3] = { "paving slab", "island / median lawn", "plaza lawn" };
                    fprintf(lf, "roam LAWN poke census (IA): drawn ground less than 1 cm under the surface, 21 samples per triangle\n");
                    for (q3 = 2; q3 >= 0; q3--)
                        fprintf(lf, "%-22s %10.0f m2, ground through it on %8.1f m2 (%.3f %%), worst ground over it %.1f cm at %.0f %.0f "
                                    "(%.6f %.6f)\n", nm[q3], area_s[q3], poke_s[q3], area_s[q3] > 0 ? 100.0 * poke_s[q3] / area_s[q3] : 0.0,
                                worst[q3] / PC_CM, wx[q3], wz[q3],
                                proj && proj[1] > 0 ? proj[0] + wz[q3] / (proj[1] * PC_UPM) : 0.0,
                                proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(wx[q3]) / (proj[3] * PC_UPM) : 0.0);
                    fclose(lf);
                }
                fprintf(stderr, "LAWNCENSUS: plaza lawn %.0f m2 poked %.1f m2, island lawn %.0f m2 poked %.1f m2, paving %.0f m2 poked %.1f m2\n",
                        area_s[2], poke_s[2], area_s[1], poke_s[1], area_s[0], poke_s[0]);
            }
        }
        free(ref);
    }
    qsort(keep, (size_t)nkeep, sizeof *keep, pc_cmp);
    snprintf(path, sizeof path, "%sroam_pavement_census.txt", ld ? ld : "");
    o = fopen(path, "w");
    snprintf(path, sizeof path, "%sroam_pavement_census.csv", ld ? ld : "");
    csv = fopen(path, "w");
    if (o) {
        int h;
        fprintf(o, "roam pavement census (G4b): %d tiles, %ld pavement vertices (%ld over no ground), nominal slab height above the drawn "
                   "ground %.1f cm (kerb %.1f + drop %.1f)\n", tn, nvert, nnog, nominal / PC_CM, TD5_TG_KERB_H / PC_CM, drop_units / PC_CM);
        fprintf(o, "BACK faces (under a slab outer edge): %ld; bottom ABOVE the drawn ground (a hole under the slab): %ld, worst %.1f cm\n",
                nback, gap0, worst_gap / PC_CM);
        fprintf(o, "SLABS (F4b pavement quads): %ld; outer edge above the drawn ground beyond nominal: worst %.1f cm; cross-fall outer - kerb: "
                   "worst up %.1f cm, worst down %.1f cm\n", nslab, worst_above / PC_CM, worst_rise / PC_CM, worst_fall / PC_CM);
        fprintf(o, "FLAT faces (slab / footpath / lawn, normal up): %ld; lowest vertex vs ground %.1f cm; faces with a vertex under the ground %ld, within 10 cm above it (z-fight) %ld\n",
                nflat, lo_min / PC_CM, sink0, touch);
        fprintf(o, "FOOTPATHS (G5): %ld footpath vertices, %ld of them over a pavement / island / lawn surface; under it "
                   "(less than 1 cm over it): %ld (%ld on a lawn page), lowest %.1f cm\n",
                nfpv, fp_on, fp_under, fp_under_lawn, fp_on ? fp_lo / PC_CM : 0.0);
        fprintf(o, "footpath vertices under the surface by more than -1 / 10 / 50 / 100 cm: paving %ld %ld %ld %ld; island or median "
                   "lawn %ld %ld %ld %ld; plaza lawn %ld %ld %ld %ld\n", fp_by[0][0], fp_by[0][1], fp_by[0][2], fp_by[0][3],
                fp_by[1][0], fp_by[1][1], fp_by[1][2], fp_by[1][3], fp_by[2][0], fp_by[2][1], fp_by[2][2], fp_by[2][3]);
        fprintf(o, "threshold_cm  back_gap_above  slab_outer_above_nominal  slab_rise_up  slab_fall_down\n");
        for (h = 0; h < 6; h++) fprintf(o, "%5.0f  %10ld  %10ld  %10ld  %10ld\n", k_thr_cm[h], hgap[h], hab[h], hrise[h], hfall[h]);
        fprintf(o, "\nworst rows: cls(0 slab above nominal, 1 back gap, 2 cross-fall, 3 sunk, 4 within 10 cm of the ground) value_cm aux_cm tile place_x place_z y lat lon\n");
        for (k = 0; k < nkeep && k < 80; k++) {
            const PcRow *f = &keep[k];
            fprintf(o, "%d %.1f %.1f %d,%d %.0f %.0f %.0f %.6f %.6f\n", f->cls, f->val, f->aux, f->tx, f->tz, f->x, f->z, f->y,
                    proj && proj[1] > 0 ? proj[0] + f->z / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(f->x) / (proj[3] * PC_UPM) : 0.0);
        }
        fclose(o);
    }
    if (csv) {
        fprintf(csv, "cls,value_cm,aux_cm,tx,tz,place_x,place_z,y,lat,lon\n");
        for (k = 0; k < nkeep; k++) {
            const PcRow *f = &keep[k];
            fprintf(csv, "%d,%.1f,%.1f,%d,%d,%.0f,%.0f,%.0f,%.6f,%.6f\n", f->cls, f->val, f->aux, f->tx, f->tz, f->x, f->z, f->y,
                    proj && proj[1] > 0 ? proj[0] + f->z / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(f->x) / (proj[3] * PC_UPM) : 0.0);
        }
        fclose(csv);
    }
    {   /* [G5] the hidden footpath vertices */
        FILE *fc;
        snprintf(path, sizeof path, "%sroam_footpath_census.csv", ld ? ld : "");
        if ((fc = fopen(path, "w")) != NULL) {
            fprintf(fc, "over_surface_cm,lawn,tx,tz,place_x,place_z,y,lat,lon\n");
            for (k = 0; k < nfprow; k++) {
                const PcRow *f = &fprow[k];
                fprintf(fc, "%.1f,%d,%d,%d,%.0f,%.0f,%.0f,%.6f,%.6f\n", f->val, (int)f->aux, f->tx, f->tz, f->x, f->z, f->y,
                        proj && proj[1] > 0 ? proj[0] + f->z / (proj[1] * PC_UPM) : 0.0,
                        proj && proj[3] > 0 ? proj[2] + td5_geo_game_x(f->x) / (proj[3] * PC_UPM) : 0.0);
            }
            fclose(fc);
        }
    }
    fprintf(stderr, "PAVECENSUS FOOTPATHS: %ld vertices, %ld over a surface, %ld under it (%ld lawn), lowest %.1f cm\n",
            nfpv, fp_on, fp_under, fp_under_lawn, fp_on ? fp_lo / PC_CM : 0.0);
    free(surf); free(fpv); free(fprow);
    fprintf(stderr, "PAVECENSUS: %d tiles, back faces %ld (hole under %ld, >25cm %ld), slabs %ld (above nominal >50cm %ld, >100cm %ld; "
            "cross-fall up >50cm %ld, down >50cm %ld), flat %ld (a vertex under the ground %ld), %u ms\n", tn, nback, gap0, hgap[0], nslab,
            hab[1], hab[2], hrise[1], hfall[1], nflat, sink0, (unsigned)(td5_plat_time_ms() - t0));
    (void)outprefix;
    free(keep); free(set.k);
    return 1;
}

#endif /* !TD5RE_RELEASE */
