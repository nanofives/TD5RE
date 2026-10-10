/* td5_geo_landmarks.c -- GEO TRACK: which footprints belong to one LANDMARK (PORT-ONLY)
 *
 * Design and measurements: docs/plans/GEO_LANDMARKS.md. The header explains the
 * problem (a cathedral is 26 OSM ways, not one).
 *
 * MEMBERSHIP RULE. A footprint belongs to anchor A when its CENTROID is inside
 * A's ring, or within GEOLM_EDGE_SLACK_M of A's edge (the ring is decimated to
 * TD5_GEOB_RING_MAX points, which can pull an edge a metre or two inside a part
 * that the source geometry kept inside). It is a PART when OSM says
 * building:part. A non-part, non-landmark footprint inside is a conflation
 * duplicate and is vetoed; a landmark inside is left alone (a chapel inside a
 * cloister is a landmark in its own right).
 *
 * WHY THE CENTROID. A part may straddle the anchor's edge (a porch), and testing
 * every vertex would drag a neighbouring building in by one shared corner. The
 * centroid is the one point that says "which building is this a piece of".
 */
#include "td5_geo_landmarks.h"
#include "td5_platform.h"
#include "td5_config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "geo"

#define GEOLM_EDGE_SLACK_M  3.0

static unsigned char *s_kind;     /* per building: TD5_GEOB_LMK_*              */
static int           *s_anchor;   /* per building: anchor index, -1 = none     */
static unsigned char *s_veto;     /* per building: 1 = conflation duplicate    */
static int           *s_nparts;   /* per ANCHOR building: parts grouped        */
static int            s_nb;
static int            s_n_anchor, s_n_part, s_n_veto, s_overflow, s_n_adopt;

/* [ROUND 1015 C item 15] FOREHOLD of a MAJOR landmark: the convex hull of an
 * anchor's outline plus every building:part under it (world units), kept so the
 * procedural frontage and any foreign real footprint can be asked "are you
 * standing on, or in front of, the landmark?". Major = an anchor that owns
 * >= TD5RE_GEO_LM_FORECOURT_PARTS parts (default 3): at La Plata that is the
 * cathedral (25 parts) and nothing else, so a roadside chapel keeps its
 * neighbours. */
#define GEOLM_FC_MAX      8
#define GEOLM_FC_PTS_MAX  512
typedef struct { int n; double x[GEOLM_FC_PTS_MAX], z[GEOLM_FC_PTS_MAX]; } GeolmHull;
static GeolmHull s_fc[GEOLM_FC_MAX];
static int       s_n_fc, s_n_apron_veto;

static void geolm_free(void)
{
    free(s_kind); free(s_anchor); free(s_veto); free(s_nparts);
    s_kind = NULL; s_anchor = NULL; s_veto = NULL; s_nparts = NULL;
    s_nb = 0;
    s_n_anchor = s_n_part = s_n_veto = s_overflow = s_n_adopt = 0;
    s_n_fc = s_n_apron_veto = 0;
}

void td5_geolm_reset(void) { geolm_free(); }

/* Even-odd point-in-ring over a td5_geob ring. */
static int geolm_inside(const TD5_GeoBuilding *a, double x, double z)
{
    int i, j, in = 0;
    double xi, zi, xj, zj;
    for (i = 0, j = a->n - 1; i < a->n; j = i++) {
        td5_geob_ring(a->first, i, &xi, &zi);
        td5_geob_ring(a->first, j, &xj, &zj);
        if (((zi > z) != (zj > z))
            && (x < (xj - xi) * (z - zi) / (zj - zi) + xi))
            in = !in;
    }
    return in;
}

/* Distance from (x,z) to the nearest edge of the ring, world units. */
static double geolm_edge_dist(const TD5_GeoBuilding *a, double x, double z)
{
    double best = 1e300;
    int i, j;
    double xi, zi, xj, zj;
    for (i = 0, j = a->n - 1; i < a->n; j = i++) {
        double dx, dz, t, len2, px, pz, d;
        td5_geob_ring(a->first, i, &xi, &zi);
        td5_geob_ring(a->first, j, &xj, &zj);
        dx = xi - xj; dz = zi - zj;
        len2 = dx * dx + dz * dz;
        t = (len2 > 0.0) ? ((x - xj) * dx + (z - zj) * dz) / len2 : 0.0;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        px = xj + t * dx; pz = zj + t * dz;
        d = hypot(x - px, z - pz);
        if (d < best) best = d;
    }
    return best;
}

/* ------------------------------------------------ forecourt hull (1015 C) --- */

static double geolm_hcross(double ox, double oz, double ax, double az, double bx, double bz)
{
    return (ax - ox) * (bz - oz) - (az - oz) * (bx - ox);
}

typedef struct { double x, z; } GeolmP2;
static int geolm_p2_cmp(const void *pa, const void *pb)
{
    const GeolmP2 *a = (const GeolmP2 *)pa, *b = (const GeolmP2 *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return 0;
}

/* Convex hull (CCW in x,z) of `n` points into h. 0 when degenerate. */
static int geolm_hull(GeolmP2 *pts, int n, GeolmHull *h)
{
    GeolmP2 *hh;
    int i, k = 0, t;
    if (n < 3) return 0;
    hh = (GeolmP2 *)malloc((size_t)(2 * n + 2) * sizeof(GeolmP2));
    if (!hh) return 0;
    qsort(pts, (size_t)n, sizeof(pts[0]), geolm_p2_cmp);
    for (i = 0; i < n; i++) {
        while (k >= 2 && geolm_hcross(hh[k-2].x, hh[k-2].z, hh[k-1].x, hh[k-1].z, pts[i].x, pts[i].z) <= 0.0) k--;
        hh[k++] = pts[i];
    }
    for (i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && geolm_hcross(hh[k-2].x, hh[k-2].z, hh[k-1].x, hh[k-1].z, pts[i].x, pts[i].z) <= 0.0) k--;
        hh[k++] = pts[i];
    }
    k--;
    if (k < 3 || k > GEOLM_FC_PTS_MAX) { free(hh); return 0; }
    for (i = 0; i < k; i++) { h->x[i] = hh[i].x; h->z[i] = hh[i].z; }
    h->n = k;
    free(hh);
    return 1;
}

/* Distance from (x,z) to a convex CCW hull: 0 inside, else to its boundary. */
static double geolm_hull_dist(const GeolmHull *h, double x, double z)
{
    double best = 1e300;
    int i, inside = 1;
    for (i = 0; i < h->n; i++) {
        const int j = (i + 1) % h->n;
        const double dx = h->x[j] - h->x[i], dz = h->z[j] - h->z[i];
        const double l2 = dx * dx + dz * dz;
        double t = (l2 > 0.0) ? ((x - h->x[i]) * dx + (z - h->z[i]) * dz) / l2 : 0.0, d;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        d = hypot(x - (h->x[i] + t * dx), z - (h->z[i] + t * dz));
        if (d < best) best = d;
        if (dx * (z - h->z[i]) - dz * (x - h->x[i]) < 0.0) inside = 0;
    }
    return inside ? 0.0 : best;
}

int td5_geolm_forecourt_near(const double *px, const double *pz, int np,
                             double margin)
{
    int f, k;
    if (s_n_fc <= 0 || !(margin >= 0.0)) return 0;
    for (f = 0; f < s_n_fc; f++)
        for (k = 0; k < np; k++)
            if (geolm_hull_dist(&s_fc[f], px[k], pz[k]) <= margin) return 1;
    return 0;
}

int td5_geolm_forecourt_count(void) { return s_n_fc; }

/* Build the hulls of the major anchors, then veto the FOREIGN real footprints that
 * stand on their apron. Called at the end of td5_geolm_cluster. */
static void geolm_build_forecourts(TD5_GeoBuilding *b, int nb, const int *anchors, int na)
{
    const int min_parts = td5_env_int("TD5RE_GEO_LM_FORECOURT_PARTS", 3, 1, 64);
    const double upm = td5_geob_units_per_m();
    const double clear = (double)td5_env_float("TD5RE_GEO_LM_BUILDING_CLEAR_M", 12.0f, 0.0f, 100.0f) * upm;
    int k, i, j;

    for (k = 0; k < na && s_n_fc < GEOLM_FC_MAX; k++) {
        const int ai = anchors[k];
        GeolmP2 *pts;
        int np = 0, cap = 0;
        if (s_nparts[ai] < min_parts || b[ai].lmkind != TD5_GEOB_LMK_WORSHIP) continue;
        for (i = 0; i < nb; i++) if (s_anchor[i] == ai) cap += b[i].n;
        pts = (GeolmP2 *)malloc((size_t)(cap + 1) * sizeof(GeolmP2));
        if (!pts) continue;
        for (i = 0; i < nb; i++) {
            if (s_anchor[i] != ai) continue;
            for (j = 0; j < b[i].n; j++) {
                double x, z;
                td5_geob_ring(b[i].first, j, &x, &z);
                pts[np].x = x; pts[np].z = z; np++;
            }
        }
        if (geolm_hull(pts, np, &s_fc[s_n_fc])) s_n_fc++;
        free(pts);
    }
    if (s_n_fc <= 0 || !(clear > 0.0)) return;
    for (i = 0; i < nb; i++) {
        double x, z;
        int f, hit = 0;
        if (b[i].landmark || b[i].part || s_veto[i] || s_anchor[i] >= 0) continue;
        for (j = 0; j < b[i].n && !hit; j++) {
            td5_geob_ring(b[i].first, j, &x, &z);
            for (f = 0; f < s_n_fc; f++)
                if (geolm_hull_dist(&s_fc[f], x, z) <= clear) { hit = 1; break; }
        }
        if (hit) { s_veto[i] = 1; s_n_apron_veto++; }
    }
}

int td5_geolm_cluster(TD5_GeoBuilding *b, int nb)
{
    int anchors[TD5_GEOLM_ANCHOR_MAX];
    int na = 0, i, k;
    const double slack = GEOLM_EDGE_SLACK_M * td5_geob_units_per_m();
    const int adopt = td5_env_flag_on("TD5RE_GEO_LM_ADOPT");

    geolm_free();
    if (nb <= 0 || !b) return 0;
    if (!td5_env_flag_on("TD5RE_GEO_LM_CLUSTER")) return 0;

    s_kind   = (unsigned char *)calloc((size_t)nb, 1);
    s_veto   = (unsigned char *)calloc((size_t)nb, 1);
    s_anchor = (int *)malloc((size_t)nb * sizeof(int));
    s_nparts = (int *)calloc((size_t)nb, sizeof(int));
    if (!s_kind || !s_veto || !s_anchor || !s_nparts) {
        TD5_LOG_E(LOG_TAG, "geolm: out of memory for %d footprint(s)", nb);
        geolm_free();
        return 0;
    }
    s_nb = nb;
    for (i = 0; i < nb; i++) s_anchor[i] = -1;

    for (i = 0; i < nb; i++) {
        const TD5_GeoBuilding *a = &b[i];
        if (!a->landmark || a->part) continue;
        if (a->lmkind == TD5_GEOB_LMK_NONE) continue;
        if (a->n < 3) continue;
        if (na >= TD5_GEOLM_ANCHOR_MAX) { s_overflow++; continue; }
        anchors[na++] = i;
        s_kind[i] = a->lmkind;
        s_anchor[i] = i;
    }
    s_n_anchor = na;

    for (i = 0; i < nb; i++) {
        TD5_GeoBuilding *c = &b[i];
        for (k = 0; k < na; k++) {
            const int ai = anchors[k];
            const TD5_GeoBuilding *a = &b[ai];
            if (i == ai) continue;
            /* Circle reject first: nearly every building is nowhere near. */
            if (hypot(c->cx - a->cx, c->cz - a->cz) > a->radius + slack)
                continue;
            if (!geolm_inside(a, c->cx, c->cz)
                && !(geolm_edge_dist(a, c->cx, c->cz) <= slack)) continue;
            if (c->part) {
                s_kind[i] = a->lmkind;
                s_anchor[i] = ai;
                s_nparts[ai]++;
                s_n_part++;
                /* ADOPT: out of bind range, but part of a building that is in
                 * range. Only when the anchor itself bound. */
                if (adopt && c->host_span < 0 && a->host_span >= 0) {
                    c->host_span = a->host_span;
                    c->host_side = a->host_side;
                    c->host_lat  = a->host_lat;
                    s_n_adopt++;
                }
            } else if (!c->landmark) {
                s_veto[i] = 1;
                s_n_veto++;
            }
            break;
        }
    }
    geolm_build_forecourts(b, nb, anchors, na);
    TD5_LOG_I(LOG_TAG, "geolm: %d major landmark hull(s), %d foreign real "
              "footprint(s) on their apron vetoed (knobs TD5RE_GEO_LM_FORECOURT_PARTS, "
              "TD5RE_GEO_LM_BUILDING_CLEAR_M, round 1015 C)", s_n_fc, s_n_apron_veto);
    TD5_LOG_I(LOG_TAG, "geolm: %d landmark cluster(s): %d building:part(s) "
              "grouped under their outline, %d of them out of bind range and "
              "adopted by the outline's span (knob TD5RE_GEO_LM_ADOPT=%s), %d "
              "conflation duplicate(s) inside an outline vetoed%s (knob "
              "TD5RE_GEO_LM_CLUSTER)", na, s_n_part, s_n_adopt,
              adopt ? "on" : "off", s_n_veto,
              s_overflow ? " (ANCHOR TABLE FULL)" : "");
    return s_n_adopt;
}

static int geolm_index(const TD5_GeoBuilding *gb)
{
    const TD5_GeoBuilding *b0 = td5_geob_building(0);
    long i;
    if (!gb || !b0 || !s_kind) return -1;
    i = (long)(gb - b0);
    return (i >= 0 && i < s_nb) ? (int)i : -1;
}

int td5_geolm_kind(const TD5_GeoBuilding *gb)
{
    const int i = geolm_index(gb);
    return i < 0 ? TD5_GEOB_LMK_NONE : (int)s_kind[i];
}

int td5_geolm_is_anchor(const TD5_GeoBuilding *gb)
{
    const int i = geolm_index(gb);
    return i >= 0 && s_anchor[i] == i;
}

int td5_geolm_vetoed(const TD5_GeoBuilding *gb)
{
    const int i = geolm_index(gb);
    return i >= 0 && s_veto[i];
}

int td5_geolm_part_count(const TD5_GeoBuilding *gb)
{
    const int i = geolm_index(gb);
    return (i >= 0 && s_anchor[i] == i) ? s_nparts[i] : 0;
}

void td5_geolm_stats(int *anchors, int *parts, int *vetoed, int *overflow,
                     int *adopted)
{
    if (adopted)  *adopted  = s_n_adopt;
    if (anchors)  *anchors  = s_n_anchor;
    if (parts)    *parts    = s_n_part;
    if (vetoed)   *vetoed   = s_n_veto;
    if (overflow) *overflow = s_overflow;
}

/* ---------------------------------------------------------------- apron --- */

typedef struct { double x, z; } GeolmPt;

static double geolm_cross(const GeolmPt *o, const GeolmPt *a, const GeolmPt *b)
{
    return (a->x - o->x) * (b->z - o->z) - (a->z - o->z) * (b->x - o->x);
}

static int geolm_pt_cmp(const void *pa, const void *pb)
{
    const GeolmPt *a = (const GeolmPt *)pa, *b = (const GeolmPt *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return 0;
}

int td5_geolm_apron(const double *rx, const double *rz, int n, double margin,
                    double *ox, double *oz, int *on, int omax)
{
    GeolmPt pts[TD5_GEOB_RING_MAX], hull[2 * TD5_GEOB_RING_MAX + 2];
    int i, k = 0, t, nh;

    if (on) *on = 0;
    if (n < 3 || n > TD5_GEOB_RING_MAX || !ox || !oz || !on) return 0;
    for (i = 0; i < n; i++) { pts[i].x = rx[i]; pts[i].z = rz[i]; }
    qsort(pts, (size_t)n, sizeof(pts[0]), geolm_pt_cmp);
    /* Andrew's monotone chain. */
    for (i = 0; i < n; i++) {
        while (k >= 2 && geolm_cross(&hull[k - 2], &hull[k - 1], &pts[i]) <= 0.0) k--;
        hull[k++] = pts[i];
    }
    for (i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && geolm_cross(&hull[k - 2], &hull[k - 1], &pts[i]) <= 0.0) k--;
        hull[k++] = pts[i];
    }
    nh = k - 1;
    if (nh < 3 || nh > omax) return 0;
    /* MITRED offset: each vertex moves along the sum of its two edge normals,
     * scaled so BOTH adjacent edges end up `margin` out. A radial push from the
     * centroid (the first cut) gave a 117 x 76 m hull a wider margin along its
     * long axis than across it, which is an oval, not an apron. */
    for (i = 0; i < nh; i++) {
        const GeolmPt *p0 = &hull[(i + nh - 1) % nh], *p1 = &hull[i],
                      *p2 = &hull[(i + 1) % nh];
        double e1x = p1->x - p0->x, e1z = p1->z - p0->z;
        double e2x = p2->x - p1->x, e2z = p2->z - p1->z;
        const double l1 = hypot(e1x, e1z), l2 = hypot(e2x, e2z);
        double n1x, n1z, n2x, n2z, d, mx, mz;
        if (l1 < 1e-9 || l2 < 1e-9) { ox[i] = p1->x; oz[i] = p1->z; continue; }
        /* CCW hull in (x, z): the outward normal of edge (dx, dz) is (dz, -dx). */
        n1x = e1z / l1; n1z = -e1x / l1;
        n2x = e2z / l2; n2z = -e2x / l2;
        d = 1.0 + n1x * n2x + n1z * n2z;
        if (d < 0.2) d = 0.2;          /* a spike would fling the corner out */
        mx = (n1x + n2x) / d; mz = (n1z + n2z) / d;
        ox[i] = p1->x + mx * margin;
        oz[i] = p1->z + mz * margin;
    }
    *on = nh;
    return 1;
}
