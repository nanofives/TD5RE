/**
 * td5_tg_s3d.c -- GEO: OSM Simple 3D Buildings mass + roof for FREE ROAM (PORT-ONLY, roam JC).
 *
 * WHY. Mariano, 2026-10-21, at a civic palace on Plaza San Martin (most likely the Legislatura: the block of
 * Avenida 7 / calle 8 / 51 / 53, see roam JC REPORT): "this building represents a government
 * building, isn't there the geometry available to copy on the game?". It is: OSM models the
 * civic landmarks of La Plata as building:part ways, each with its own height, roof:shape,
 * roof:height and roof:colour (Simple 3D Buildings, wiki.openstreetmap.org/wiki/Simple_3D_Buildings).
 * The shared extrusion core (tg_geo_emit_building_core, td5_tg_city.c) read those tags but built
 * the pick as a flat box: its ridge builder is CONVEX-ONLY and a ring with a collinear corner (an
 * OSM node on a straight wall: 53 of the place's 90 tagged rings) fails its convexity test, every
 * concave part (29 of 90) kept a flat cap, `round` (a barrel vault) and `dome` came out as pyramids,
 * and building:part=roof (a roof with no walls) stood as a solid block.
 *
 * WHAT. One builder for every roof: a stack of RINGS from the eaves up, each with the eaves'
 * vertex count, joined edge by edge (a quad per edge per row, two triangles, degenerate ones
 * dropped), optionally capped flat. Every shape is a recipe for those rows:
 *
 *   flat              no row, the eaves ring capped
 *   gabled / hipped   one row, the ridge (principal axis, roof:orientation=across turns it)
 *   half-hipped       one row, the ridge inset by a quarter of the width
 *   skillion          one row, the far edge (the high wall)
 *   gambrel           two rows, a steep lower slope to 45 % of the way, then the ridge
 *   round (barrel)    five rows on a quarter circle between the eaves and the ridge
 *   pyramidal / cone  one row, every vertex at the centroid
 *   dome              six rows, the ring scaled to the centroid on a quarter circle
 *   onion             seven rows, a bulb that swells past the eaves then closes
 *   mansard           one row, the ring OFFSET inward (mitred) by 35 % of the rise, capped flat
 *
 * CONCAVE RINGS. A ridge or an apex needs a convex ring (pyramid / dome: star-shaped about the
 * centroid is enough). Otherwise the roof is the mitred INWARD OFFSET of the eaves ring at the
 * shape's pitch, capped flat: the hip-and-deck silhouette a straight skeleton would give on an
 * L or U plan, without the skeleton. The offset is searched down from the wanted depth until the
 * ring is still simple, keeps every edge's direction and stays inside the eaves; under half a
 * metre the roof is flat (counted).
 *
 * WALLS. As the core's (one quad per ring edge, foundation down to the ground per vertex unless
 * the part has a min_height), except building:part=roof: a 0.6 m fascia under the eaves and an
 * underside, no walls to the ground.
 *
 * COLOUR. roof:colour, else roof:material, tints the roof's baked vertex colour; building:colour,
 * else building:material, the walls. Only on this path, so an untagged city is untouched.
 *
 * COURTYARDS (roam JC2). An OSM multipolygon outline (S3D_RELATIONS.JSON) arrives with its inner rings
 * (e->s3d_hx/hz/hn): they get walls facing into the courtyard, the flat cap is the outline minus the
 * holes (each hole bridged to the outline, then ear-clipped), and a sloped roof insets every ring at the
 * pitch to one holed deck. The BASE (e->s3d_base) is the flat block an outline still draws under its
 * building:parts, inset 0.25 m so no wall is coplanar with a part's. A CIVIC building takes a stone /
 * plaster facade page (k_s3d_civic below).
 *
 * SCOPE. Called only when the caller's environment sets e->s3d (free roam). The strip geo BUILD
 * never sets it, so level091 and the synthetic gate are byte-identical. No file-scope state:
 * the stream worker builds tiles with it.
 */
#include "td5_tg_geo_bld.h"
#include "td5_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define S3D_ROWS_MAX   9                       /* eaves + up to 8 rows (onion: 7) */
#define S3D_RN         TD5_GEOB_RING_MAX
#define S3D_COL_DEG    3.0                     /* a corner straighter than this is a node on a wall */
#define S3D_COL_DEV_M  0.10                    /* ... or one this close to the chord of its neighbours */
#define S3D_DUP_M      0.25                    /* points closer than this are one corner */
#define S3D_TRI_MIN    4.0                     /* twice the area, world units^2: a sliver */
#define S3D_DEF_RISE_M 3.5                     /* untagged sloped roof */
#define S3D_FASCIA_M   0.6                     /* building:part=roof edge */
#define S3D_MIN_OFF_M  0.5                     /* an offset roof shallower than this is flat */

/* --------------------------------------------------------------- ring tools --- */

static double s3d_cross(double ax, double az, double bx, double bz) { return ax * bz - az * bx; }

/* Drop the straight-through vertices (an OSM node on a straight wall: within S3D_COL_DEG or S3D_COL_DEV_M of the
 * chord of its neighbours) and the near-duplicates (S3D_DUP_M). MEASURED: the Catedral's two octagonal pinnacles
 * (parts 1450098105 / 1450098112) carry a 1.2 deg kink and a 0.13 m edge, enough to fail the convex test and every
 * inset. The ring stays CCW. Returns the new count (>= 3), or n unchanged when cleaning would leave < 3. */
static int s3d_clean(const double *rx, const double *rz, int n, double upm, double *ox, double *oz)
{
    const double s_tol = sin(S3D_COL_DEG * 3.14159265358979 / 180.0);
    int keep[S3D_RN], k, m = 0, pass;
    for (k = 0; k < n; k++) keep[k] = 1;
    for (pass = 0; pass < 3; pass++) {
        int changed = 0;
        for (k = 0; k < n; k++) {
            int p = (k + n - 1) % n, q = (k + 1) % n, alive = 0, j;
            double ax, az, bx, bz, la, lb;
            if (!keep[k]) continue;
            for (j = 0; j < n; j++) alive += keep[j];
            if (alive <= 3) break;
            while (!keep[p]) p = (p + n - 1) % n;
            while (!keep[q]) q = (q + 1) % n;
            ax = rx[k] - rx[p]; az = rz[k] - rz[p];
            bx = rx[q] - rx[k]; bz = rz[q] - rz[k];
            la = hypot(ax, az); lb = hypot(bx, bz);
            {
                const double cx = rx[q] - rx[p], cz = rz[q] - rz[p], lc = hypot(cx, cz);
                const double dev = lc > 1e-6 ? fabs(s3d_cross(cx, cz, rx[k] - rx[p], rz[k] - rz[p])) / lc : 0.0;
                if (la < S3D_DUP_M * upm || lb < S3D_DUP_M * upm
                    || (ax * bx + az * bz > 0.0
                        && (fabs(s3d_cross(ax, az, bx, bz)) < s_tol * la * lb || dev < S3D_COL_DEV_M * upm))) {
                    keep[k] = 0; changed = 1;
                }
            }
        }
        if (!changed) break;
    }
    for (k = 0; k < n; k++) if (keep[k]) { ox[m] = rx[k]; oz[m] = rz[k]; m++; }
    if (m < 3 || !td5_geob_ring_simple(ox, oz, m)) { for (k = 0; k < n; k++) { ox[k] = rx[k]; oz[k] = rz[k]; } return n; }
    return m;
}

static int s3d_convex(const double *x, const double *z, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n, k = (i + 2) % n;
        if (s3d_cross(x[j] - x[i], z[j] - z[i], x[k] - x[j], z[k] - z[j]) < 0.0) return 0;   /* CCW: a right turn */
    }
    return 1;
}

/* The centroid sees every edge from its inner side: the ring is star-shaped about it, so a
 * ring scaled toward it never crosses itself (pyramid, dome, onion). */
static int s3d_star(const double *x, const double *z, int n, double cx, double cz)
{
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if (s3d_cross(x[j] - x[i], z[j] - z[i], cx - x[i], cz - z[i]) <= 0.0) return 0;
    }
    return 1;
}

/* Mitred inward offset of a CCW ring by d. 1 = the result is a usable roof ring: simple, every
 * edge keeps its direction, every vertex inside the eaves. */
static int s3d_offset(const double *x, const double *z, int n, double d, double *ox, double *oz)
{
    double nx[S3D_RN], nz[S3D_RN];
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double ex = x[j] - x[i], ez = z[j] - z[i], l = hypot(ex, ez);
        if (!(l > 1e-6)) return 0;
        nx[i] = -ez / l; nz[i] = ex / l;           /* left of a CCW edge = inside */
    }
    for (i = 0; i < n; i++) {
        const int p = (i + n - 1) % n;
        /* the two offset lines meet at x + d * (n_p + n_i) / (1 + n_p . n_i) */
        const double dot = nx[p] * nx[i] + nz[p] * nz[i];
        if (dot < -0.9) return 0;                  /* a hairpin: the mitre runs away */
        ox[i] = x[i] + d * (nx[p] + nx[i]) / (1.0 + dot);
        oz[i] = z[i] + d * (nz[p] + nz[i]) / (1.0 + dot);
    }
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if ((ox[j] - ox[i]) * (x[j] - x[i]) + (oz[j] - oz[i]) * (z[j] - z[i]) <= 0.0) return 0;
        if (!td5_geob_point_in_ring(x, z, n, ox[i], oz[i])) return 0;
    }
    return td5_geob_ring_simple(ox, oz, n) && td5_geob_ring_area(ox, oz, n) > 0.0;
}

/* Principal axis (dominant eigenvector of the vertex covariance), as tg_geo_ring_axis. */
static void s3d_axis(const double *x, const double *z, int n, double cx, double cz, double *ux, double *uz)
{
    double sxx = 0.0, szz = 0.0, sxz = 0.0, a;
    int k;
    for (k = 0; k < n; k++) {
        const double dx = x[k] - cx, dz = z[k] - cz;
        sxx += dx * dx; szz += dz * dz; sxz += dx * dz;
    }
    if (fabs(sxz) < 1e-9 && fabs(sxx - szz) < 1e-9) { *ux = 1.0; *uz = 0.0; return; }
    a = 0.5 * atan2(2.0 * sxz, sxx - szz);
    *ux = cos(a); *uz = sin(a);
}

/* Blend the neutral baked colour toward an OSM colour by k (0..1). */
static unsigned int s3d_tint(unsigned int base, unsigned int rgb, double k)
{
    const int br = (int)((base >> 16) & 255), bg = (int)((base >> 8) & 255), bb = (int)(base & 255);
    const int cr = (int)((rgb >> 16) & 255), cg = (int)((rgb >> 8) & 255), cb = (int)(rgb & 255);
    const int r = (int)(br + (cr - br) * k + 0.5), g = (int)(bg + (cg - bg) * k + 0.5), b = (int)(bb + (cb - bb) * k + 0.5);
    return 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/* ------------------------------------------------------------------- mesh --- */

int tg_s3d_eligible(const TD5_GeoBuilding *gb)
{
    return gb && (gb->part || gb->roof3d != TD5_GEOB_R3_NONE || gb->roof_only);
}

void tg_s3d_free(TG_S3dMesh *m)
{
    if (!m) return;
    free(m->v); free(m->light);
    m->v = NULL; m->light = NULL; m->nv = m->cap = 0;
}

static int s3d_reserve(TG_S3dMesh *m, int add)
{
    if (m->nv + add > m->cap) {
        int nc = m->cap ? m->cap * 2 : 1024;
        float *nv;
        unsigned int *nl;
        while (nc < m->nv + add) nc *= 2;
        nv = (float *)realloc(m->v, (size_t)nc * 5 * sizeof(float));
        if (!nv) return 0;
        m->v = nv;
        nl = (unsigned int *)realloc(m->light, (size_t)nc * sizeof(unsigned int));
        if (!nl) return 0;
        m->light = nl;
        m->cap = nc;
    }
    return 1;
}

static double s3d_area2(const double *t)
{
    const double ax = t[3] - t[0], ay = t[4] - t[1], az = t[5] - t[2];
    const double bx = t[6] - t[0], by = t[7] - t[1], bz = t[8] - t[2];
    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    return sqrt(cx * cx + cy * cy + cz * cz);
}

/* one roof triangle (planar xz UVs, one page every ~8 m) */
static int s3d_tri(TG_S3dMesh *m, const double *t, unsigned int col)
{
    int i;
    if (s3d_area2(t) <= S3D_TRI_MIN) return 0;
    if (!s3d_reserve(m, 3)) return -1;
    for (i = 0; i < 3; i++) {
        float *o = &m->v[(size_t)m->nv * 5];
        o[0] = (float)t[i * 3 + 0]; o[1] = (float)t[i * 3 + 1]; o[2] = (float)t[i * 3 + 2];
        o[3] = (float)(t[i * 3 + 0] / 3400.0); o[4] = (float)(t[i * 3 + 2] / 3400.0);
        m->light[m->nv++] = col;
    }
    return 1;
}

/* one wall quad: near-bottom, far-bottom, far-top, near-top; v = vb at the base, 0 at the top */
static int s3d_quad(TG_S3dMesh *m, const double *q, double ua, double vb, unsigned int col)
{
    int i;
    if (!s3d_reserve(m, 4)) return -1;
    for (i = 0; i < 4; i++) {
        float *o = &m->v[(size_t)m->nv * 5];
        o[0] = (float)q[i * 3 + 0]; o[1] = (float)q[i * 3 + 1]; o[2] = (float)q[i * 3 + 2];
        o[3] = (float)((i == 1 || i == 2) ? ua : 0.0);
        o[4] = (float)((i >= 2) ? 0.0 : vb);
        m->light[m->nv++] = col;
    }
    return 1;
}

/* triangulated flat cap of ring (x, z) at height y; flip = facing down */
static int s3d_cap(TG_S3dMesh *m, const double *x, const double *z, int n, double y, int flip, unsigned int col, int *ntri)
{
    int tri[(S3D_RN - 2) * 3], nc, k, r;
    double t[9];
    nc = td5_geob_triangulate(x, z, n, tri, S3D_RN - 2);
    for (k = 0; k < nc; k++) {
        const int a = tri[k * 3 + 0], b = tri[k * 3 + (flip ? 2 : 1)], c = tri[k * 3 + (flip ? 1 : 2)];
        t[0] = x[a]; t[1] = y; t[2] = z[a];
        t[3] = x[b]; t[4] = y; t[5] = z[b];
        t[6] = x[c]; t[7] = y; t[8] = z[c];
        if ((r = s3d_tri(m, t, col)) < 0) return -1;
        *ntri += r;
    }
    return 1;
}

/* The roof recipe: fill rows 1..nrow-1 (row 0 = the eaves) and their heights above the eaves.
 * Returns the row count (>= 1) and *cap = cap the top row; *how = TG_S3D_HOW_*. */
static int s3d_rows(const TD5_GeoBuilding *gb, const double *x, const double *z, int n, double cx, double cz,
                    double rise, double rise_cap, double upm, double (*rw)[2][S3D_RN], double *rh, int *cap, int *how,
                    double *rise_io)
{
    const int shape = (int)gb->roof3d;
    const int convex = s3d_convex(x, z, n), star = convex || s3d_star(x, z, n, cx, cz);
    int r, k, nrow = 1;
    *cap = 0; *how = TG_S3D_HOW_FLAT;
    for (k = 0; k < n; k++) { rw[0][0][k] = x[k]; rw[0][1][k] = z[k]; }
    rh[0] = 0.0;
    if (shape == TD5_GEOB_R3_NONE || shape == TD5_GEOB_R3_FLAT) { *cap = 1; return 1; }

    /* ridge family on a convex ring */
    if (convex && (shape == TD5_GEOB_R3_GABLED || shape == TD5_GEOB_R3_HIPPED || shape == TD5_GEOB_R3_HALF_HIPPED
                   || shape == TD5_GEOB_R3_SKILLION || shape == TD5_GEOB_R3_GAMBREL || shape == TD5_GEOB_R3_ROUND)) {
        double ux, uz, vx, vz, tmin = 0, tmax = 0, vmin = 0, vmax = 0, halfw, ta, tb;
        double ridge[2][S3D_RN];
        s3d_axis(x, z, n, cx, cz, &ux, &uz);
        if (gb->roof_across) { const double t = ux; ux = -uz; uz = t; }
        vx = -uz; vz = ux;
        for (k = 0; k < n; k++) {
            const double dx = x[k] - cx, dz = z[k] - cz, t = dx * ux + dz * uz, v = dx * vx + dz * vz;
            if (k == 0 || t < tmin) tmin = t;
            if (k == 0 || t > tmax) tmax = t;
            if (k == 0 || v < vmin) vmin = v;
            if (k == 0 || v > vmax) vmax = v;
        }
        halfw = (vmax - vmin) * 0.5;
        ta = tmin; tb = tmax;
        if (shape == TD5_GEOB_R3_HIPPED)      { ta = tmin + halfw;        tb = tmax - halfw; }
        if (shape == TD5_GEOB_R3_HALF_HIPPED) { ta = tmin + halfw * 0.5;  tb = tmax - halfw * 0.5; }
        if (shape == TD5_GEOB_R3_SKILLION)    { ta = tb = tmax; }
        if (ta > tb) ta = tb = (tmin + tmax) * 0.5;
        if (shape == TD5_GEOB_R3_ROUND && !(gb->roof_height > 0.0)) {
            rise = halfw < rise_cap ? halfw : rise_cap;         /* a semicircular vault */
            *rise_io = rise;
        }
        for (k = 0; k < n; k++) {
            const double dx = x[k] - cx, dz = z[k] - cz;
            double t = dx * ux + dz * uz;
            const double v = (shape == TD5_GEOB_R3_SKILLION) ? (dx * vx + dz * vz) : 0.0;
            if (t < ta) t = ta;
            if (t > tb) t = tb;
            ridge[0][k] = cx + ux * t + vx * v;
            ridge[1][k] = cz + uz * t + vz * v;
        }
        if (shape == TD5_GEOB_R3_ROUND) {
            const int m = 5;
            for (r = 1; r <= m; r++) {
                const double a = (double)r * 1.5707963267949 / m, f = 1.0 - cos(a);
                for (k = 0; k < n; k++) {
                    rw[r][0][k] = x[k] + (ridge[0][k] - x[k]) * f;
                    rw[r][1][k] = z[k] + (ridge[1][k] - z[k]) * f;
                }
                rh[r] = rise * sin(a);
            }
            nrow = m + 1;
            *how = TG_S3D_HOW_BARREL;
        } else if (shape == TD5_GEOB_R3_GAMBREL) {
            for (k = 0; k < n; k++) {
                rw[1][0][k] = x[k] + (ridge[0][k] - x[k]) * 0.45;
                rw[1][1][k] = z[k] + (ridge[1][k] - z[k]) * 0.45;
                rw[2][0][k] = ridge[0][k]; rw[2][1][k] = ridge[1][k];
            }
            rh[1] = rise * 0.7; rh[2] = rise;
            nrow = 3;
            *how = TG_S3D_HOW_RIDGE;
        } else {
            for (k = 0; k < n; k++) { rw[1][0][k] = ridge[0][k]; rw[1][1][k] = ridge[1][k]; }
            rh[1] = rise;
            nrow = 2;
            *how = TG_S3D_HOW_RIDGE;
        }
        return nrow;
    }

    /* centroid family on a star-shaped ring */
    if (star && (shape == TD5_GEOB_R3_PYRAMIDAL || shape == TD5_GEOB_R3_CONE || shape == TD5_GEOB_R3_DOME
                 || shape == TD5_GEOB_R3_ONION)) {
        static const double k_onion_s[8] = { 1.0, 1.12, 1.14, 0.98, 0.66, 0.36, 0.14, 0.0 };
        static const double k_onion_h[8] = { 0.0, 0.14, 0.30, 0.48, 0.64, 0.78, 0.90, 1.0 };
        int m;
        if (shape == TD5_GEOB_R3_DOME) m = 6;
        else if (shape == TD5_GEOB_R3_ONION) m = 7;
        else m = 1;
        for (r = 1; r <= m; r++) {
            double s, h;
            if (shape == TD5_GEOB_R3_DOME) {
                const double a = (double)r * 1.5707963267949 / m;
                s = cos(a); h = sin(a);
            } else if (shape == TD5_GEOB_R3_ONION) {
                s = k_onion_s[r]; h = k_onion_h[r];
            } else {
                s = 0.0; h = 1.0;
            }
            for (k = 0; k < n; k++) {
                rw[r][0][k] = cx + (x[k] - cx) * s;
                rw[r][1][k] = cz + (z[k] - cz) * s;
            }
            rh[r] = rise * h;
        }
        *how = TG_S3D_HOW_CENTROID;
        return m + 1;
    }

    /* everything else -- mansard anywhere, a sloped shape on a concave ring: the inward offset */
    {
        const double slope = (shape == TD5_GEOB_R3_MANSARD) ? 0.35 : 1.43;   /* run per unit rise: ~70 / ~35 deg */
        double d = rise * slope;
        double ox[S3D_RN], oz[S3D_RN];
        int tries, ok = 0;
        for (tries = 0; tries < 12 && d >= S3D_MIN_OFF_M * upm; tries++, d *= 0.75)
            if (s3d_offset(x, z, n, d, ox, oz)) { ok = 1; break; }
        if (!ok) { *cap = 1; *how = TG_S3D_HOW_FLAT_FALLBACK; return 1; }
        for (k = 0; k < n; k++) { rw[1][0][k] = ox[k]; rw[1][1][k] = oz[k]; }
        /* a shallower offset than the pitch wanted: keep the pitch, lower the deck */
        rh[1] = (shape == TD5_GEOB_R3_MANSARD) ? rise : (d / slope < rise ? d / slope : rise);
        if (shape == TD5_GEOB_R3_DOME || shape == TD5_GEOB_R3_ROUND || shape == TD5_GEOB_R3_ONION) rh[1] = rise * 0.6;
        *cap = 1;
        *how = TG_S3D_HOW_OFFSET;
        return 2;
    }
}

/* ------------------------------------------------------------ courtyards --- */

/* [roam JC2] Mitred offset of a CW HOLE ring by d into the solid (the left of a CW edge is the
 * outside of the hole). 1 = usable: simple, every edge keeps its direction, still CW, no vertex
 * inside the original hole. */
static int s3d_offset_hole(const double *x, const double *z, int n, double d, double *ox, double *oz)
{
    double nx[S3D_RN], nz[S3D_RN];
    int i;
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double ex = x[j] - x[i], ez = z[j] - z[i], l = hypot(ex, ez);
        if (!(l > 1e-6)) return 0;
        nx[i] = -ez / l; nz[i] = ex / l;
    }
    for (i = 0; i < n; i++) {
        const int p = (i + n - 1) % n;
        const double dot = nx[p] * nx[i] + nz[p] * nz[i];
        if (dot < -0.9) return 0;
        ox[i] = x[i] + d * (nx[p] + nx[i]) / (1.0 + dot);
        oz[i] = z[i] + d * (nz[p] + nz[i]) / (1.0 + dot);
    }
    for (i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if ((ox[j] - ox[i]) * (x[j] - x[i]) + (oz[j] - oz[i]) * (z[j] - z[i]) <= 0.0) return 0;
        if (td5_geob_point_in_ring(x, z, n, ox[i], oz[i])) return 0;
    }
    return td5_geob_ring_simple(ox, oz, n) && td5_geob_ring_area(ox, oz, n) < 0.0;
}

/* proper crossing of segments ab and cd (touching at an endpoint does not count) */
static int s3d_seg_cross(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz)
{
    const double d1 = s3d_cross(bx - ax, bz - az, cx - ax, cz - az), d2 = s3d_cross(bx - ax, bz - az, dx - ax, dz - az);
    const double d3 = s3d_cross(dx - cx, dz - cz, ax - cx, az - cz), d4 = s3d_cross(dx - cx, dz - cz, bx - cx, bz - cz);
    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

static int s3d_rings_cross(const double *ax, const double *az, int an, const double *bx, const double *bz, int bn)
{
    int i, j;
    for (i = 0; i < an; i++)
        for (j = 0; j < bn; j++)
            if (s3d_seg_cross(ax[i], az[i], ax[(i + 1) % an], az[(i + 1) % an],
                              bx[j], bz[j], bx[(j + 1) % bn], bz[(j + 1) % bn])) return 1;
    return 0;
}

typedef struct {
    int    nh, hn[TG_S3D_HOLES_MAX];
    double hx[TG_S3D_HOLES_MAX][S3D_RN], hz[TG_S3D_HOLES_MAX][S3D_RN];
} S3dHoles;

/* outer + holes offset into the solid by d, and the rings still clear of each other */
static int s3d_offset_all(const double *x, const double *z, int n, const S3dHoles *h, double d,
                          double *ox, double *oz, S3dHoles *oh)
{
    int i, j, k;
    if (!s3d_offset(x, z, n, d, ox, oz)) return 0;
    oh->nh = h->nh;
    for (i = 0; i < h->nh; i++) {
        oh->hn[i] = h->hn[i];
        if (!s3d_offset_hole(h->hx[i], h->hz[i], h->hn[i], d, oh->hx[i], oh->hz[i])) return 0;
        for (k = 0; k < oh->hn[i]; k++)
            if (!td5_geob_point_in_ring(ox, oz, n, oh->hx[i][k], oh->hz[i][k])) return 0;
        if (s3d_rings_cross(ox, oz, n, oh->hx[i], oh->hz[i], oh->hn[i])) return 0;
        for (j = 0; j < i; j++)
            if (s3d_rings_cross(oh->hx[j], oh->hz[j], oh->hn[j], oh->hx[i], oh->hz[i], oh->hn[i])) return 0;
    }
    return 1;
}

#define S3D_CAPN (S3D_RN * (1 + TG_S3D_HOLES_MAX) + 2 * TG_S3D_HOLES_MAX + 4)

/* Splice hole (CW) into the CCW polygon p through a bridge from the hole's rightmost vertex to the
 * nearest polygon vertex the bridge reaches without crossing any edge (of p, of this hole, of the holes
 * still to come). 1 = merged. */
static int s3d_bridge(double *px, double *pz, int *pn, const double *hx, const double *hz, int hn,
                      const S3dHoles *rest, const int *done)
{
    int mi = 0, k, best = -1, i, j, n = *pn;
    double bd = 1e300;
    for (k = 1; k < hn; k++) if (hx[k] > hx[mi]) mi = k;
    for (k = 0; k < n; k++) {
        const double d = hypot(px[k] - hx[mi], pz[k] - hz[mi]);
        int ok = 1;
        if (!(d < bd)) continue;
        for (i = 0; i < n && ok; i++) {
            const int i2 = (i + 1) % n;
            if (s3d_seg_cross(hx[mi], hz[mi], px[k], pz[k], px[i], pz[i], px[i2], pz[i2])) ok = 0;
        }
        for (i = 0; i < hn && ok; i++)
            if (s3d_seg_cross(hx[mi], hz[mi], px[k], pz[k], hx[i], hz[i], hx[(i + 1) % hn], hz[(i + 1) % hn])) ok = 0;
        for (j = 0; j < rest->nh && ok; j++) {
            if (done[j]) continue;
            for (i = 0; i < rest->hn[j] && ok; i++)
                if (s3d_seg_cross(hx[mi], hz[mi], px[k], pz[k], rest->hx[j][i], rest->hz[j][i],
                                  rest->hx[j][(i + 1) % rest->hn[j]], rest->hz[j][(i + 1) % rest->hn[j]])) ok = 0;
        }
        if (ok && !td5_geob_point_in_ring(px, pz, n, 0.5 * (hx[mi] + px[k]), 0.5 * (hz[mi] + pz[k]))) ok = 0;
        if (ok) { bd = d; best = k; }
    }
    if (best < 0 || n + hn + 2 > S3D_CAPN) return 0;
    /* p[0..best], h[mi], h[mi+1] .. h[mi-1], h[mi], p[best], p[best+1 ..] */
    for (k = n - 1; k > best; k--) { px[k + hn + 2] = px[k]; pz[k + hn + 2] = pz[k]; }
    for (k = 0; k <= hn; k++) { px[best + 1 + k] = hx[(mi + k) % hn]; pz[best + 1 + k] = hz[(mi + k) % hn]; }
    px[best + hn + 2] = px[best]; pz[best + hn + 2] = pz[best];
    *pn = n + hn + 2;
    return 1;
}

static int s3d_same(double ax, double az, double bx, double bz) { return fabs(ax - bx) < 1e-6 && fabs(az - bz) < 1e-6; }

/* Ear clipping of a CCW, weakly simple polygon (bridged holes: repeated vertices). Triangles as index
 * triples into (x, z). Returns the count, or -1 when no ear is left (the caller lids the courtyard). */
static int s3d_earcut(const double *x, const double *z, int n, int *tri)
{
    int idx[S3D_CAPN], cnt = n, nt = 0, i = 0, miss = 0, k;
    if (n < 3 || n > S3D_CAPN) return -1;
    for (k = 0; k < n; k++) idx[k] = k;
    while (cnt > 3) {
        const int a = idx[(i + cnt - 1) % cnt], b = idx[i % cnt], c = idx[(i + 1) % cnt];
        const double cr = s3d_cross(x[b] - x[a], z[b] - z[a], x[c] - x[b], z[c] - z[b]);
        int ear = cr > 1e-9;
        if (ear) {
            for (k = 0; k < cnt && ear; k++) {
                const int v = idx[k];
                double d1, d2, d3;
                if (v == a || v == b || v == c) continue;
                if (s3d_same(x[v], z[v], x[a], z[a]) || s3d_same(x[v], z[v], x[b], z[b]) || s3d_same(x[v], z[v], x[c], z[c]))
                    continue;
                d1 = s3d_cross(x[b] - x[a], z[b] - z[a], x[v] - x[a], z[v] - z[a]);
                d2 = s3d_cross(x[c] - x[b], z[c] - z[b], x[v] - x[b], z[v] - z[b]);
                d3 = s3d_cross(x[a] - x[c], z[a] - z[c], x[v] - x[c], z[v] - z[c]);
                if (d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) ear = 0;
            }
        }
        if (!ear && fabs(cr) <= 1e-9 && miss >= cnt) ear = -1;        /* a straight-through or doubled vertex: drop it */
        if (ear) {
            if (ear > 0) { tri[nt * 3 + 0] = a; tri[nt * 3 + 1] = b; tri[nt * 3 + 2] = c; nt++; }
            for (k = i % cnt; k + 1 < cnt; k++) idx[k] = idx[k + 1];
            cnt--;
            if (i >= cnt) i = 0;
            miss = 0;
            continue;
        }
        i = (i + 1) % cnt;
        if (++miss > 2 * cnt) return -1;
    }
    tri[nt * 3 + 0] = idx[0]; tri[nt * 3 + 1] = idx[1]; tri[nt * 3 + 2] = idx[2];
    return nt + 1;
}

/* flat cap of the outer ring minus its holes at y. 1 holed, 2 lidded (the clipper gave up: outer only), -1 OOM */
static int s3d_cap_holes(TG_S3dMesh *m, const double *x, const double *z, int n, const S3dHoles *h, double y,
                         int flip, unsigned int col, int *ntri)
{
    double px[S3D_CAPN], pz[S3D_CAPN], t[9];
    int tri[S3D_CAPN * 3], done[TG_S3D_HOLES_MAX], pn = n, nt, k, i, r, ok = 1;
    if (h->nh <= 0) return s3d_cap(m, x, z, n, y, flip, col, ntri) < 0 ? -1 : 1;
    for (k = 0; k < n; k++) { px[k] = x[k]; pz[k] = z[k]; }
    memset(done, 0, sizeof done);
    for (i = 0; i < h->nh && ok; i++) {           /* rightmost hole first (earcut's order) */
        int pick = -1, j;
        double bx = -1e300;
        for (j = 0; j < h->nh; j++) {
            int q;
            if (done[j]) continue;
            for (q = 0; q < h->hn[j]; q++) if (h->hx[j][q] > bx) { bx = h->hx[j][q]; pick = j; }
        }
        done[pick] = 1;
        ok = s3d_bridge(px, pz, &pn, h->hx[pick], h->hz[pick], h->hn[pick], h, done);
    }
    nt = ok ? s3d_earcut(px, pz, pn, tri) : -1;
    if (nt < 0) return s3d_cap(m, x, z, n, y, flip, col, ntri) < 0 ? -1 : 2;
    for (k = 0; k < nt; k++) {
        const int a = tri[k * 3 + 0], b = tri[k * 3 + (flip ? 2 : 1)], c = tri[k * 3 + (flip ? 1 : 2)];
        t[0] = px[a]; t[1] = y; t[2] = pz[a];
        t[3] = px[b]; t[4] = y; t[5] = pz[b];
        t[6] = px[c]; t[7] = y; t[8] = pz[c];
        if ((r = s3d_tri(m, t, col)) < 0) return -1;
        *ntri += r;
    }
    return 1;
}

/* one band of roof between ring a (at ya) and ring b (at yb), edge by edge */
static int s3d_band(TG_S3dMesh *m, const double *ax, const double *az, double ya, const double *bx, const double *bz,
                    double yb, int n, unsigned int col, int *ntri)
{
    double t[9];
    int k, r;
    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        t[0] = ax[k]; t[1] = ya; t[2] = az[k];
        t[3] = ax[j]; t[4] = ya; t[5] = az[j];
        t[6] = bx[j]; t[7] = yb; t[8] = bz[j];
        if ((r = s3d_tri(m, t, col)) < 0) return -1;
        *ntri += r;
        t[0] = ax[k]; t[1] = ya; t[2] = az[k];
        t[3] = bx[j]; t[4] = yb; t[5] = bz[j];
        t[6] = bx[k]; t[7] = yb; t[8] = bz[k];
        if ((r = s3d_tri(m, t, col)) < 0) return -1;
        *ntri += r;
    }
    return 1;
}

/* one wall quad with explicit u / v: near-bottom (u0, vb0), far-bottom (u1, vb1), far-top (u1, vt), near-top (u0, vt) */
static int s3d_quad_uv(TG_S3dMesh *m, const double *q, double u0, double u1, double vb0, double vb1, double vt,
                       unsigned int col)
{
    const double u[4] = { u0, u1, u1, u0 }, v[4] = { vb0, vb1, vt, vt };
    int i;
    if (!s3d_reserve(m, 4)) return -1;
    for (i = 0; i < 4; i++) {
        float *o = &m->v[(size_t)m->nv * 5];
        o[0] = (float)q[i * 3 + 0]; o[1] = (float)q[i * 3 + 1]; o[2] = (float)q[i * 3 + 2];
        o[3] = (float)u[i]; o[4] = (float)v[i];
        m->light[m->nv++] = col;
    }
    return 1;
}

#define S3D_PORTAL_H_M   4.5     /* a carriage arch */
#define S3D_PORTAL_PAD_M 0.6     /* clear of the driveway's edge each side */
#define S3D_PORTALS_MAX  8

/* The openings of edge (a, b): [t0, t1] along it where a service way the outline stands over crosses it. */
static int s3d_edge_portals(const TG_GeoBldEnv *e, double ax, double az, double bx, double bz, double upm,
                            double *t0, double *t1)
{
    const double ex = bx - ax, ez = bz - az, el = hypot(ex, ez);
    int p, np = 0, i, j;
    if (!(el > 1.0) || e->s3d_np <= 0 || !e->s3d_pseg) return 0;
    for (p = 0; p < e->s3d_np && np < S3D_PORTALS_MAX; p++) {
        const double *g = &e->s3d_pseg[p * 5];
        const double rx = g[2] - g[0], rz = g[3] - g[1], den = s3d_cross(ex, ez, rx, rz);
        double t, u, sn, half;
        if (fabs(den) < 1e-9) continue;
        t = s3d_cross(g[0] - ax, g[1] - az, rx, rz) / den;
        u = s3d_cross(g[0] - ax, g[1] - az, ex, ez) / den;
        if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) continue;
        sn = fabs(den) / (el * hypot(rx, rz));                  /* sin of the crossing angle */
        if (sn < 0.25) sn = 0.25;
        half = (g[4] + S3D_PORTAL_PAD_M * upm) / sn / el;
        t0[np] = t - half < 0.0 ? 0.0 : t - half;
        t1[np] = t + half > 1.0 ? 1.0 : t + half;
        np++;
    }
    for (i = 1; i < np; i++)                                      /* sort, then merge overlaps */
        for (j = i; j > 0 && t0[j] < t0[j - 1]; j--) {
            double a = t0[j]; t0[j] = t0[j - 1]; t0[j - 1] = a;
            a = t1[j]; t1[j] = t1[j - 1]; t1[j - 1] = a;
        }
    for (i = 0, j = 0; i < np; i++) {
        if (j > 0 && t0[i] <= t1[j - 1]) { if (t1[i] > t1[j - 1]) t1[j - 1] = t1[i]; continue; }
        t0[j] = t0[i]; t1[j] = t1[i]; j++;
    }
    return j;
}

/* walls of one ring (outer CCW faces out, a CW courtyard ring faces into the courtyard); an edge a service way
 * crosses opens into a carriage arch there */
static int s3d_walls(TG_S3dMesh *m, const TG_GeoBldEnv *e, const TD5_GeoBuilding *gb, const double *x, const double *z,
                     int n, double by, double base_y, double wall_top, double vrows, double cell_w, unsigned int col,
                     int *nquad)
{
    double bot[S3D_RN], q[12];
    const double upm = e->upm > 1.0 ? e->upm : 430.0;
    const double span = (wall_top - base_y) > 1.0 ? (wall_top - base_y) : 1.0;
    const double ph = (S3D_PORTAL_H_M * upm < (wall_top - base_y) - 1.0 * upm) ? S3D_PORTAL_H_M * upm : 0.0;
    int k;
    for (k = 0; k < n; k++) {
        bot[k] = base_y;
        if (!gb->roof_only && td5_env_flag_on("TD5RE_GEO_BLD_FOUNDATION") && !(gb->min_height > 0.0)) {
            const double g = e->ground(e->u, x[k], z[k]) + tg_city_kerb_h(e->b) - 300.0;   /* TD5_TG_GEO_BASE_SINK */
            if (g < bot[k]) bot[k] = (g < by - 4300.0) ? by - 4300.0 : g;                   /* TD5_TG_GEO_MAX_FOUND */
        }
    }
    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        const double elen = hypot(x[j] - x[k], z[j] - z[k]);
        const double ua = elen / ((cell_w > 1.0) ? cell_w : 1500.0);
        double t0[S3D_PORTALS_MAX], t1[S3D_PORTALS_MAX], vq = vrows;
        int np, p;
        if (!(elen > 1.0)) continue;
        np = (ph > 0.0 && !gb->roof_only) ? s3d_edge_portals(e, x[k], z[k], x[j], z[j], upm, t0, t1) : 0;
        if (np == 0) {
            q[0] = x[k]; q[1]  = bot[k];   q[2]  = z[k];
            q[3] = x[j]; q[4]  = bot[j];   q[5]  = z[j];
            q[6] = x[j]; q[7]  = wall_top; q[8]  = z[j];
            q[9] = x[k]; q[10] = wall_top; q[11] = z[k];
            if (wall_top - base_y > 1.0) vq = vrows * (wall_top - 0.5 * (bot[k] + bot[j])) / (wall_top - base_y);
            if (s3d_quad(m, q, ua, vq, col) < 0) return -1;
            (*nquad)++;
            continue;
        }
        /* solid runs from the ground, openings from the arch up; u runs on along the edge */
        {
            double ta = 0.0;
            for (p = 0; p <= np; p++) {
                const double tb = (p < np) ? t0[p] : 1.0;
                int pass;
                for (pass = 0; pass < 2; pass++) {
                    const double sa = pass ? t0[p < np ? p : 0] : ta, sb = pass ? t1[p < np ? p : 0] : tb;
                    double ya, yb;
                    if (pass && p == np) break;
                    if (!(sb - sa > 1e-4)) continue;
                    ya = pass ? base_y + ph : bot[k] + (bot[j] - bot[k]) * sa;
                    yb = pass ? base_y + ph : bot[k] + (bot[j] - bot[k]) * sb;
                    q[0] = x[k] + (x[j] - x[k]) * sa; q[1]  = ya;       q[2]  = z[k] + (z[j] - z[k]) * sa;
                    q[3] = x[k] + (x[j] - x[k]) * sb; q[4]  = yb;       q[5]  = z[k] + (z[j] - z[k]) * sb;
                    q[6] = q[3];                      q[7]  = wall_top; q[8]  = q[5];
                    q[9] = q[0];                      q[10] = wall_top; q[11] = q[2];
                    if (s3d_quad_uv(m, q, ua * sa, ua * sb, vrows * (wall_top - ya) / span, vrows * (wall_top - yb) / span,
                                    0.0, col) < 0) return -1;
                    (*nquad)++;
                }
                if (p < np) { ta = t1[p]; m->portals++; }
            }
        }
    }
    return 1;
}

/* [roam JC2] THE CIVIC FACADE. Mariano's pick at the Legislatura read as "dense vertical stripes": a palace
 * drew the city's residential window grid. A civic building (TD5_GeoBuilding.civic) takes one of four
 * classical stone / plaster pages with large, spaced windows from the shipped landmark page block (local
 * index into td5_tg_real_tex_landmarks.h, looked at on a contact sheet, _archive/roam/JC/pages):
 *    94  cream plaster, pilasters, two storeys, three bays     95  white palace front, two storeys, seven bays
 *   153  rusticated cream stone, two storeys, three bays      154  rusticated grey stone, two storeys, three bays
 * One page cell is the two storeys it shows (civic storeys are tall, 4.5 m) and its bays at ~4 m. */
static const struct { int page; double cell_w_m, row_h_m; } k_s3d_civic[4] = {
    { 94, 12.0, 9.0 }, { 95, 24.0, 9.0 }, { 153, 12.0, 9.0 }, { 154, 12.0, 9.0 },
};
typedef char s3d_civic_pages_fit[(TD5_TG_LM_PAGES >= 155) ? 1 : -1];

int tg_s3d_build(const TG_GeoBldEnv *e, const TD5_GeoBuilding *gb, const double *rx_in, const double *rz_in,
                 int n_in, double by, double H, double floor_h, double cell_w, double lm_row_h, int storeys,
                 int wall_page, int roof_page, TG_S3dMesh *m)
{
    double x[S3D_RN], z[S3D_RN], cx = 0.0, cz = 0.0, rise, wall_top, vrows, base_y, minw = 1e300;
    double (*rw)[2][S3D_RN] = NULL;
    double rh[S3D_ROWS_MAX];
    S3dHoles *hl = NULL, *ho = NULL;
    unsigned int roof_col = 0xFFC8C8C8u, wall_col = 0xFFFFFFFFu;
    int n, k, i, nrow, cap, how, ntri = 0, nquad = 0, ret = -1;
    const double upm = e->upm > 1.0 ? e->upm : 430.0;

    memset(m, 0, sizeof *m);
    if (n_in < 3 || n_in > S3D_RN) return 0;
    rw = (double (*)[2][S3D_RN])malloc(sizeof(double[2][S3D_RN]) * S3D_ROWS_MAX);
    hl = (S3dHoles *)calloc(2, sizeof(S3dHoles));
    if (!rw || !hl) { free(rw); free(hl); return -1; }
    ho = hl + 1;
    n = s3d_clean(rx_in, rz_in, n_in, upm, x, z);

    /* courtyards: cleaned, CW, inside the outline (a ring that is not is dropped) */
    if (e->s3d_nh > 0 && e->s3d_hn) {
        int off = 0;
        for (i = 0; i < e->s3d_nh; i++) {
            const int hn = e->s3d_hn[i];
            double tx[S3D_RN], tz[S3D_RN];
            if (hn >= 3 && hn <= S3D_RN && hl->nh < TG_S3D_HOLES_MAX) {
                const int c = s3d_clean(e->s3d_hx + off, e->s3d_hz + off, hn, upm, tx, tz);
                int inside = td5_geob_ring_simple(tx, tz, c);
                for (k = 0; k < c && inside; k++) inside = td5_geob_point_in_ring(x, z, n, tx[k], tz[k]);
                if (inside && !s3d_rings_cross(x, z, n, tx, tz, c)) {
                    const int cw = td5_geob_ring_area(tx, tz, c) < 0.0;
                    for (k = 0; k < c; k++) {
                        hl->hx[hl->nh][k] = cw ? tx[k] : tx[c - 1 - k];
                        hl->hz[hl->nh][k] = cw ? tz[k] : tz[c - 1 - k];
                    }
                    hl->hn[hl->nh++] = c;
                }
            }
            off += hn;
        }
        m->holes = hl->nh;
    }
    /* the base under building:parts: inset 0.25 m off every ring so no wall is coplanar with a part's */
    if (e->s3d_base) {
        double ox[S3D_RN], oz[S3D_RN];
        if (s3d_offset_all(x, z, n, hl, 0.25 * upm, ox, oz, ho)) {
            memcpy(x, ox, sizeof(double) * (size_t)n); memcpy(z, oz, sizeof(double) * (size_t)n);
            *hl = *ho;
        }
    }
    for (k = 0; k < n; k++) { cx += x[k]; cz += z[k]; }
    cx /= n; cz /= n;
    {   /* the narrowest extent across the principal axis, for the default dome rise */
        double ux, uz, vmin = 0, vmax = 0;
        s3d_axis(x, z, n, cx, cz, &ux, &uz);
        for (k = 0; k < n; k++) {
            const double v = (x[k] - cx) * -uz + (z[k] - cz) * ux;
            if (k == 0 || v < vmin) vmin = v;
            if (k == 0 || v > vmax) vmax = v;
        }
        minw = vmax - vmin;
    }
    /* civic: the stone / plaster page (a worship landmark keeps its own dressing) */
    if (gb->civic && !(lm_row_h > 1.0)) {
        const int c = (int)((gb->id_hash >> 9) % 4u);
        wall_page = TD5_TG_PAGE_LM_BASE + k_s3d_civic[c].page;
        cell_w    = k_s3d_civic[c].cell_w_m * upm;
        lm_row_h  = k_s3d_civic[c].row_h_m * upm;
        m->civic  = 1;
    }

    /* ROOF HEIGHT. Simple 3D Buildings: `height` includes the roof and roof:height is its share,
     * so the eaves sit at height - roof:height. Untagged: roof:levels storeys, else 3.5 m (a dome:
     * half its width, a barrel: decided by the recipe), never more than 60 % of the mass. */
    if (gb->roof3d == TD5_GEOB_R3_NONE || gb->roof3d == TD5_GEOB_R3_FLAT || e->s3d_base) rise = 0.0;
    else if (gb->roof_height > 0.0)  rise = gb->roof_height;
    else if (gb->roof_levels > 0)    rise = gb->roof_levels * (e->storey > 1.0 ? e->storey : 3.0 * upm);
    else if (gb->roof3d == TD5_GEOB_R3_DOME || gb->roof3d == TD5_GEOB_R3_ONION) rise = minw * 0.5;
    else                             rise = S3D_DEF_RISE_M * upm;
    if (rise > H * 0.6 && !(gb->roof_height > 0.0 && gb->roof_height < H * 0.95)) rise = H * 0.6;
    if (gb->roof_only && rise > H * 0.9) rise = H * 0.9;

    if (e->s3d_base || gb->roof3d == TD5_GEOB_R3_NONE || gb->roof3d == TD5_GEOB_R3_FLAT) {
        for (k = 0; k < n; k++) { rw[0][0][k] = x[k]; rw[0][1][k] = z[k]; }
        rh[0] = 0.0; nrow = 1; cap = 1; how = TG_S3D_HOW_FLAT;
    } else if (hl->nh > 0) {
        /* a sloped roof round courtyards: every ring inset at the pitch, one deck with the holes */
        const double slope = (gb->roof3d == TD5_GEOB_R3_MANSARD) ? 0.35 : 1.43;
        double d = rise * slope;
        int tries, ok = 0;
        for (tries = 0; tries < 12 && d >= S3D_MIN_OFF_M * upm; tries++, d *= 0.75)
            if (s3d_offset_all(x, z, n, hl, d, rw[1][0], rw[1][1], ho)) { ok = 1; break; }
        for (k = 0; k < n; k++) { rw[0][0][k] = x[k]; rw[0][1][k] = z[k]; }
        rh[0] = 0.0; cap = 1;
        if (ok) {
            rh[1] = (gb->roof3d == TD5_GEOB_R3_MANSARD) ? rise : (d / slope < rise ? d / slope : rise);
            nrow = 2; how = TG_S3D_HOW_OFFSET;
        } else {
            nrow = 1; how = TG_S3D_HOW_FLAT_FALLBACK;
        }
    } else {
        nrow = s3d_rows(gb, x, z, n, cx, cz, rise, H * 0.6, upm, rw, rh, &cap, &how, &rise);
    }
    if (how == TG_S3D_HOW_FLAT_FALLBACK || how == TG_S3D_HOW_FLAT) rise = 0.0;
    wall_top = by + H - rise;
    if (wall_top < by + 0.5 * upm) wall_top = by + 0.5 * upm;   /* the roof ate the walls: keep half a metre */
    m->how = how;

    if (gb->roof_rgb)  { roof_col = s3d_tint(roof_col, gb->roof_rgb, 0.75); m->tint_roof = 1; }
    if (gb->wall_rgb)  { wall_col = s3d_tint(wall_col, gb->wall_rgb, 0.55); m->tint_wall = 1; }

    /* --- roof triangles: rows joined edge by edge (courtyard rings too), then the cap --- */
    for (i = 0; i + 1 < nrow; i++)
        if (s3d_band(m, rw[i][0], rw[i][1], wall_top + rh[i], rw[i + 1][0], rw[i + 1][1], wall_top + rh[i + 1], n,
                     roof_col, &ntri) < 0) goto out;
    if (nrow == 2 && how == TG_S3D_HOW_OFFSET && hl->nh > 0)
        for (i = 0; i < hl->nh; i++)
            if (s3d_band(m, hl->hx[i], hl->hz[i], wall_top, ho->hx[i], ho->hz[i], wall_top + rh[1], hl->hn[i],
                         roof_col, &ntri) < 0) goto out;
    if (cap) {
        double tx[S3D_RN], tz[S3D_RN];
        int r;
        for (k = 0; k < n; k++) { tx[k] = rw[nrow - 1][0][k]; tz[k] = rw[nrow - 1][1][k]; }
        r = s3d_cap_holes(m, tx, tz, n, (nrow == 2 && how == TG_S3D_HOW_OFFSET && hl->nh > 0) ? ho : hl,
                          wall_top + rh[nrow - 1], 0, roof_col, &ntri);
        if (r < 0) goto out;
        if (hl->nh > 0) { if (r == 1) m->cap_holed++; else m->cap_lidded++; }
    }

    /* --- walls: the outline, then every courtyard --- */
    base_y = by;
    if (gb->roof_only) {
        const double fascia = (S3D_FASCIA_M * upm < (wall_top - by) * 0.5) ? S3D_FASCIA_M * upm : (wall_top - by) * 0.5;
        base_y = wall_top - fascia;
        if (s3d_cap_holes(m, x, z, n, hl, base_y, 1, roof_col, &ntri) < 0) goto out;   /* the underside */
    }
    if (ntri > 0) {
        m->cmd[m->ncmd * 3 + 0] = (unsigned short)roof_page;
        m->cmd[m->ncmd * 3 + 1] = (unsigned short)ntri;
        m->cmd[m->ncmd * 3 + 2] = 0;
        m->ncmd++;
    }
    if (lm_row_h > 1.0)             vrows = (wall_top - base_y) / lm_row_h;
    else if (storeys > 0 && H > 1.0) vrows = (double)storeys * (wall_top - base_y) / H;
    else                            vrows = (wall_top - base_y) / ((floor_h > 1.0) ? floor_h : 1.0);
    if (!(vrows > 0.25)) vrows = gb->roof_only ? 0.25 : 1.0;
    if (s3d_walls(m, e, gb, x, z, n, by, base_y, wall_top, vrows, cell_w, wall_col, &nquad) < 0) goto out;
    if (nquad < 3) { ret = 0; goto out; }
    for (i = 0; i < hl->nh; i++)
        if (s3d_walls(m, e, gb, hl->hx[i], hl->hz[i], hl->hn[i], by, base_y, wall_top, vrows, cell_w, wall_col, &nquad) < 0)
            goto out;
    m->cmd[m->ncmd * 3 + 0] = (unsigned short)wall_page;
    m->cmd[m->ncmd * 3 + 1] = 0;
    m->cmd[m->ncmd * 3 + 2] = (unsigned short)nquad;
    m->ncmd++;
    m->rows = nrow;
    m->rise = rise;
    ret = 1;
out:
    free(rw);
    free(hl);
    return ret;
}
