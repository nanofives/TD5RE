/**
 * td5_tg_geo_clear.c -- GEO TRACK: where a BUILDING may NOT stand (PORT-ONLY).
 * Contract and rationale in td5_tg_geo_clear.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "td5_tg_geo_clear.h"
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_geo_avenues.h"
#include "td5_geo_buildings.h"

/* Grid cell, world units (~18.6 m at La Plata's 430 units/m). Wide enough that
 * one cell holds a whole street crossing and narrow enough that a query touches
 * a handful of the place's 9448 segments. */
#define GC_CELL        8000.0
/* A sample stands on a way when it is within this share of the way's HALF width
 * of its centreline. 0.8 keeps a footprint that merely grazes the kerb out of
 * the verdict while a building standing across the lane is in it. */
#define GC_HIT_SHARE   0.8
/* "Parallel" for the own-road exemption: |cos| of the angle between the way's
 * direction and the span tangent. 0.7 = 45 degrees, which covers the race road's
 * own way through a bend (the nearest polyline segment turns before the span
 * does) while a real cross street (90 degrees) and a Y-junction arm that has
 * already diverged (> 45) are still foreign. */
#define GC_PAR_COS     0.7
/* Slack on top of the carriageway reach for the own-road exemption: the
 * conditioner's worst deviation from the OSM polyline measured on La Plata is
 * 1500 units (3.5 m); two lanes cover it with room. */
#define GC_OWN_SLACK   3000.0

typedef struct {
    float x0, z0, x1, z1;
    float ux, uz;          /* unit direction */
    float hw;              /* half width, world units */
} GcSeg;

static GcSeg *s_seg;
static int    s_nseg;
static int   *s_head;      /* per cell: first segment index, -1 none */
static int   *s_next;      /* per segment-in-cell link (parallel to s_cell_seg) */
static int   *s_cell_seg;  /* flat list: segment index per registration */
static int    s_nreg, s_regcap;
static int    s_gw, s_gh;
static double s_gx0, s_gz0;
static int    s_ready;
static long   s_walls, s_foot;

static void gc_free(void)
{
    free(s_seg);      s_seg = NULL;      s_nseg = 0;
    free(s_head);     s_head = NULL;
    free(s_next);     s_next = NULL;
    free(s_cell_seg); s_cell_seg = NULL; s_nreg = s_regcap = 0;
    s_gw = s_gh = 0;
    s_ready = 0;
}

int tg_geo_clear_ready(void) { return s_ready; }

static int gc_reg(int cell, int seg)
{
    if (s_nreg >= s_regcap) {
        const int nc = s_regcap ? s_regcap * 2 : 16384;
        int *a = (int *)realloc(s_cell_seg, (size_t)nc * sizeof(int));
        int *b = (int *)realloc(s_next, (size_t)nc * sizeof(int));
        if (!a || !b) { if (a) s_cell_seg = a; if (b) s_next = b; return 0; }
        s_cell_seg = a; s_next = b; s_regcap = nc;
    }
    s_cell_seg[s_nreg] = seg;
    s_next[s_nreg]     = s_head[cell];
    s_head[cell]       = s_nreg++;
    return 1;
}

void tg_geo_clear_prepare(void)
{
    int n, i, k, total = 0;
    double minx = 1e300, minz = 1e300, maxx = -1e300, maxz = -1e300;

    gc_free();
    s_walls = s_foot = 0;
    if (!td5_geo_loaded()) return;            /* MUST be the first statement */
    if (!td5_env_flag_on("TD5RE_GEO_ROAD_CLEAR")) return;
    td5_geo_roads_sync(td5_geo_place_slug());
    n = td5_geo_roads_count();
    if (n < 1) return;

    for (i = 0; i < n; i++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(i);
        if (r && r->count > 1) total += r->count - 1;
    }
    if (total < 1) return;
    s_seg = (GcSeg *)malloc((size_t)total * sizeof(GcSeg));
    if (!s_seg) return;

    for (i = 0; i < n; i++) {
        const TD5_GeoRoad *r = td5_geo_roads_get(i);
        double px = 0.0, pz = 0.0;
        if (!r || r->count < 2) continue;
        /* An alley / driveway is not a street a facade is expected to leave a
         * gap for; a tunnel or a bridge is not at grade under the building. */
        if (r->klass == TD5_GEO_RC_SERVICE) continue;
        if (r->tunnel || r->bridge || r->layer != 0) continue;
        if (!td5_geo_roads_point(r, 0, &px, &pz)) continue;
        for (k = 1; k < r->count; k++) {
            double x, z, dx, dz, L;
            GcSeg *s;
            if (!td5_geo_roads_point(r, k, &x, &z)) break;
            dx = x - px; dz = z - pz;
            L = sqrt(dx * dx + dz * dz);
            if (L > 1.0 && s_nseg < total) {
                s = &s_seg[s_nseg++];
                s->x0 = (float)px; s->z0 = (float)pz;
                s->x1 = (float)x;  s->z1 = (float)z;
                s->ux = (float)(dx / L); s->uz = (float)(dz / L);
                s->hw = (float)(((double)(r->lanes > 0 ? r->lanes : 2))
                                * (double)TD5_TG_LANE_WIDTH * 0.5);
                if (px - s->hw < minx) minx = px - s->hw;
                if (x  - s->hw < minx) minx = x  - s->hw;
                if (px + s->hw > maxx) maxx = px + s->hw;
                if (x  + s->hw > maxx) maxx = x  + s->hw;
                if (pz - s->hw < minz) minz = pz - s->hw;
                if (z  - s->hw < minz) minz = z  - s->hw;
                if (pz + s->hw > maxz) maxz = pz + s->hw;
                if (z  + s->hw > maxz) maxz = z  + s->hw;
            }
            px = x; pz = z;
        }
    }
    if (s_nseg < 1) { gc_free(); return; }

    s_gx0 = minx; s_gz0 = minz;
    s_gw = (int)((maxx - minx) / GC_CELL) + 1;
    s_gh = (int)((maxz - minz) / GC_CELL) + 1;
    if (s_gw < 1 || s_gh < 1 || (double)s_gw * (double)s_gh > 4.0e6) {
        gc_free();
        return;
    }
    s_head = (int *)malloc((size_t)s_gw * (size_t)s_gh * sizeof(int));
    if (!s_head) { gc_free(); return; }
    for (i = 0; i < s_gw * s_gh; i++) s_head[i] = -1;
    for (i = 0; i < s_nseg; i++) {
        const GcSeg *s = &s_seg[i];
        double ax = (s->x0 < s->x1 ? s->x0 : s->x1) - s->hw;
        double bx = (s->x0 > s->x1 ? s->x0 : s->x1) + s->hw;
        double az = (s->z0 < s->z1 ? s->z0 : s->z1) - s->hw;
        double bz = (s->z0 > s->z1 ? s->z0 : s->z1) + s->hw;
        int gx0 = (int)((ax - s_gx0) / GC_CELL), gx1 = (int)((bx - s_gx0) / GC_CELL);
        int gz0 = (int)((az - s_gz0) / GC_CELL), gz1 = (int)((bz - s_gz0) / GC_CELL);
        int gx, gz;
        if (gx0 < 0) gx0 = 0;
        if (gz0 < 0) gz0 = 0;
        if (gx1 >= s_gw) gx1 = s_gw - 1;
        if (gz1 >= s_gh) gz1 = s_gh - 1;
        for (gz = gz0; gz <= gz1; gz++)
            for (gx = gx0; gx <= gx1; gx++)
                if (!gc_reg(gz * s_gw + gx, i)) { gc_free(); return; }
    }
    s_ready = 1;
    TD5_LOG_I(LOG_TAG, "[GEO CLEAR] indexed %d drivable segment(s) of %d way(s) "
              "(service / bridge / tunnel / layered ways skipped), %dx%d cells "
              "(knob TD5RE_GEO_ROAD_CLEAR=on)", s_nseg, n, s_gw, s_gh);
}

int tg_geo_foreign_road_at(double x, double z, double ox, double oz,
                           double tx, double tz, double own_lat)
{
    int gx, gz, e;
    double lat;
    if (!s_ready) return 0;
    gx = (int)((x - s_gx0) / GC_CELL);
    gz = (int)((z - s_gz0) / GC_CELL);
    if (gx < 0 || gz < 0 || gx >= s_gw || gz >= s_gh) return 0;
    /* Lateral offset of the SAMPLE from the host span (right-handed, either
     * sign: the exemption is symmetric). */
    lat = fabs((x - ox) * tz - (z - oz) * tx);
    for (e = s_head[gz * s_gw + gx]; e >= 0; e = s_next[e]) {
        const GcSeg *s = &s_seg[s_cell_seg[e]];
        const double dx = (double)s->x1 - (double)s->x0;
        const double dz = (double)s->z1 - (double)s->z0;
        const double L2 = dx * dx + dz * dz;
        double t, cx, cz, d, par;
        t = ((x - (double)s->x0) * dx + (z - (double)s->z0) * dz) / L2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        cx = (double)s->x0 + t * dx;
        cz = (double)s->z0 + t * dz;
        d = sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz));
        if (d >= (double)s->hw * GC_HIT_SHARE) continue;
        par = fabs((double)s->ux * tx + (double)s->uz * tz);
        /* The race road's own way (and the avenue's partner carriageway) runs
         * alongside the span inside the carriageway authority's reach. */
        if (par >= GC_PAR_COS && lat <= own_lat) continue;
        return 1;
    }
    return 0;
}

double tg_geo_own_lateral(const TG_NodeList *nl, int si, double side)
{
    double reach, half;
    if (!nl || si < 0 || si >= nl->count) return 0.0;
    reach = tg_carriageway_reach(nl, si, side);
    half  = tg_road_half_width(nl, si);
    if (reach < half) reach = half;
    return reach + GC_OWN_SLACK;
}

/* The far footway the avenue emitter lays outside the opposite carriageway on
 * `side` at span si: its width, or 0 where none is laid (no avenue on that side,
 * a median opening, no OSM width, knob off). Mirrors the conditions of
 * tg_av_emit_far_pavement in td5_tg_avenue.c. */
static double gc_far_walk(const TG_NodeList *nl, int si, double side)
{
    double off = 0.0;
    int lanes = 2, open = 0;
    if (tg_geo_avenue_n() < 1) return 0.0;
    if (!td5_env_flag_on("TD5RE_GEO_AVENUE_FARWALK")) return 0.0;
    if (!td5_geo_avenue_at(si, &off, &lanes, &open)) return 0.0;
    if (open) return 0.0;
    if (side * off < 0.0) return 0.0;               /* avenue on the other side */
    (void)nl;
    return tg_geo_sidewalk_w_side(si, side > 0.0);
}

double tg_geo_building_clear_gap(const TG_NodeList *nl, int si, double side,
                                 double sidewalk)
{
    double gap = tg_carriageway_clear_gap(nl, si, side, sidewalk,
                                          TD5_TG_CARRIAGEWAY_MARGIN);
    double fw;
    if (!td5_geo_loaded()) return gap;           /* synthetic: unchanged */
    if (!td5_env_flag_on("TD5RE_GEO_BLD_FARWALK")) return gap;
    fw = gc_far_walk(nl, si, side);
    if (fw > 0.0) {
        const double need = tg_carriageway_reach(nl, si, side)
                          - tg_road_half_width(nl, si) + fw
                          + TD5_TG_CARRIAGEWAY_MARGIN;
        if (gap < need) gap = need;
    }
    return gap;
}

int tg_geo_prefab_blocked(const TG_NodeList *nl, int si, double side,
                          double x, double z, double fx, double fz)
{
    const TG_Node *n;
    double own, tx, tz;
    int i, j;
    if (!td5_geo_loaded() || !s_ready) return 0;
    if (!nl || si < 0 || si >= nl->count) return 0;
    n = &nl->v[si];
    tx = n->tx; tz = n->tz;
    /* `side` is tg_prefab_place's (the piece sits along (-tz, +tx) * side); the
     * carriageway authority's lateral for the same side is (+tz, -tx) * side,
     * hence the sign. */
    own = tg_geo_own_lateral(nl, si, -side);
    /* centre + a 3x3 sweep of the rectangle (local X along the road, Z across) */
    for (i = -1; i <= 1; i++)
        for (j = -1; j <= 1; j++) {
            const double px = x + tx * (fx * 0.5 * i) + (-tz * side) * (fz * 0.5 * j);
            const double pz = z + tz * (fx * 0.5 * i) + ( tx * side) * (fz * 0.5 * j);
            if (tg_geo_foreign_road_at(px, pz, n->x, n->z, tx, tz, own)) return 1;
        }
    return 0;
}

double tg_geo_prefab_seat(const TG_NodeList *nl, int si, double x, double z,
                          double fx, double fz, double y_centre)
{
    const TG_Node *n;
    double y = y_centre;
    int i, j;
    if (!td5_geo_loaded()) return y_centre;       /* synthetic: unchanged */
    if (!td5_env_flag_on("TD5RE_GEO_BLD_FOUNDATION")) return y_centre;
    if (!nl || si < 0 || si >= nl->count) return y_centre;
    n = &nl->v[si];
    for (i = -1; i <= 1; i += 2)
        for (j = -1; j <= 1; j += 2) {
            const double px = x + n->tx * (fx * 0.5 * i) + (-n->tz) * (fz * 0.5 * j);
            const double pz = z + n->tz * (fx * 0.5 * i) + ( n->tx) * (fz * 0.5 * j);
            const double gy = tg_world_h(px, pz);
            if (gy < y) y = gy;
        }
    return y;
}

/* ---- [ROUND 1023 C items 2 + 9] WHOLE-FOOTPRINT CLEARANCE ------------------------------------------
 *
 * Mariano, level091: "this building is clipping in the middle of the road" (e105 s21, a 12 x 14 m
 * footprint, 5.8 m2 of its roof on a corridor carriageway and 10.8 m2 on its pavement).
 *
 * WHY THE EXISTING TESTS LET IT THROUGH. tg_geo_emit_one nudges the ring out until every vertex clears
 * the HOST span's carriageway-plus-pavement envelope, and that envelope is a straight line along the
 * host node's tangent with the analytic fork reach on it. Two things are wrong with a straight line:
 *   - a footprint is 12..70 m long and a span is 3.5 m, so on a bend the road a vertex really stands
 *     beside is a NEIGHBOUR span's, which the host frame measures at the wrong lateral; and
 *   - a fork corridor is not where its analytic reach says it is: this one (a plaza fork's free
 *     corridor, fork 0, row 320) runs through the footprint's corner while tg_carriageway_reach
 *     reports the nominal 3700..4800 lateral of a corridor that is not laid there.
 * The street tests are blind in a different way (centroid or a third of the boundary samples, and only
 * the carriageway, never the pavement arm).
 *
 * THE TEST. Measure the footprint against what is actually laid, nodes and quads, not against a frame:
 *   ROADS    every span segment of the strip -- ring nodes, the fork corridors' rows (laid at the main
 *            node's frame + the fork's lateral shift, or the plaza fork's own row chain) -- as a capsule
 *            of its own half width plus that span's pavement width: a sample closer than that minus a
 *            300 graze is a penetration, and the whole ring is pushed straight away from the worst one
 *            (<= 6 passes, <= `maxpush` in total, else the footprint is dropped);
 *   STREETS  the laid side-street carriageways and their pavement arms (tg_xstreet_collect): the plan
 *            area of the footprint on them, sampled on a grid, drops the footprint.
 * Read-only on shared state, safe from any scenery worker. */
#define FC_MAX_SAMP   300
#define FC_MAX_QUAD   128
#define FC_MAX_SEG    768
#define FC_GRAZE      300.0
#define FC_CAR_M2     1.0      /* plan area on a laid side-street carriageway that drops a footprint */
#define FC_ARM_M2     4.0      /* ... and on a pavement arm */

typedef struct { double x0, z0, x1, z1, hw, band; } FcSeg;

/* The pavement a span lays (0 on a biome with no raised pavement: the verge there is flat). Side-blind
 * and therefore the WIDER of the two, which is the safe direction for a clearance. */
static double fc_band(const TG_NodeList *nl, int node)
{
    const TG_Biome *b;
    double w;
    if (node < 0 || node >= nl->count) return 0.0;
    b = &k_biomes[tg_scenery_biome_index(node)];
    w = tg_city_sidewalk_w_at(nl, node, b);
    return w > 0.0 ? w : 0.0;
}

/* Keep a segment as a candidate when its capsule can reach the ring's bounding box and it is not a deck
 * over / an underpass under the footprint. */
static void fc_push_seg(FcSeg *c, int *nc, double ax, double az, double bx, double bz, double hw, double band,
                        double ay, double by, double base_y, double x0, double z0, double x1, double z1)
{
    const double r = hw + band;
    if (*nc >= FC_MAX_SEG) return;
    if (fabs(ay - base_y) > 3500.0 && fabs(by - base_y) > 3500.0) return;
    if ((ax < bx ? ax : bx) - r > x1 || (ax > bx ? ax : bx) + r < x0) return;
    if ((az < bz ? az : bz) - r > z1 || (az > bz ? az : bz) + r < z0) return;
    c[*nc].x0 = ax; c[*nc].z0 = az; c[*nc].x1 = bx; c[*nc].z1 = bz; c[*nc].hw = hw; c[*nc].band = band;
    (*nc)++;
}

static int fc_in_poly(const double *rx, const double *rz, int n, double x, double z)
{
    int i, j, in = 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        if (((rz[i] > z) != (rz[j] > z)) &&
            (x < (rx[j] - rx[i]) * (z - rz[i]) / (rz[j] - rz[i]) + rx[i]))
            in = !in;
    }
    return in;
}

static int fc_in_quad(const double *q, double x, double z)
{
    int k, sgn = 0;
    for (k = 0; k < 4; k++) {
        const int j = (k + 1) & 3;
        const double c = (q[j * 2] - q[k * 2]) * (z - q[k * 2 + 1])
                       - (q[j * 2 + 1] - q[k * 2 + 1]) * (x - q[k * 2]);
        const int sg = (c > 0.0) ? 1 : (c < 0.0 ? -1 : 0);
        if (!sg) continue;
        if (!sgn) sgn = sg;
        else if (sg != sgn) return 0;
    }
    return sgn != 0;
}

/* Sample set of a ring: an interior grid first (each worth step^2 of plan area), then the vertices, the
 * edge midpoints and the centroid. Returns the count; *ni = interior samples, *cell = area per interior
 * sample (or per boundary sample when the ring is too thin for a grid). */
static int fc_samples(const double *rx, const double *rz, int n, double *sx, double *sz,
                      int *ni, double *cell, double *area_out)
{
    double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300, area = 0.0, cx = 0.0, cz = 0.0, step, gz;
    int ns = 0, k;

    for (k = 0; k < n; k++) {
        const int j = (k + 1) % n;
        if (rx[k] < x0) x0 = rx[k];
        if (rx[k] > x1) x1 = rx[k];
        if (rz[k] < z0) z0 = rz[k];
        if (rz[k] > z1) z1 = rz[k];
        area += rx[k] * rz[j] - rx[j] * rz[k];
        cx += rx[k]; cz += rz[k];
    }
    area = fabs(area) * 0.5;
    cx /= (double)n; cz /= (double)n;
    *area_out = area;
    if (!(area > 1.0)) { *ni = 0; *cell = 0.0; return 0; }
    step = sqrt((x1 - x0) * (z1 - z0) / 220.0);
    if (step < 700.0) step = 700.0;
    for (gz = z0 + step * 0.5; gz < z1 && ns < FC_MAX_SAMP - 2 * n - 2; gz += step) {
        double gx;
        for (gx = x0 + step * 0.5; gx < x1 && ns < FC_MAX_SAMP - 2 * n - 2; gx += step)
            if (fc_in_poly(rx, rz, n, gx, gz)) { sx[ns] = gx; sz[ns] = gz; ns++; }
    }
    *ni = ns;
    for (k = 0; k < n && ns < FC_MAX_SAMP - 2; k++) {
        const int j = (k + 1) % n;
        sx[ns] = rx[k]; sz[ns] = rz[k]; ns++;
        sx[ns] = (rx[k] + rx[j]) * 0.5; sz[ns] = (rz[k] + rz[j]) * 0.5; ns++;
    }
    if (ns < FC_MAX_SAMP) { sx[ns] = cx; sz[ns] = cz; ns++; }
    *cell = (*ni >= 4) ? step * step : area / (double)(ns > 0 ? ns : 1);
    return ns;
}

/* Plan area of the polygon on the laid side streets' carriageways and on their pavement arms, world
 * units squared. 1 when either exceeds its threshold. */
int tg_geo_poly_street_overlap(const TG_NodeList *nl, int si, double side,
                               const double *rx, const double *rz, int n, int arms,
                               double *car_u2, double *arm_u2)
{
    double sx[FC_MAX_SAMP], sz[FC_MAX_SAMP], quad[FC_MAX_QUAD * 8], area, cell, car = 0.0, arm = 0.0;
    unsigned char qk[FC_MAX_QUAD];
    int ns, ni, nq, k, i, cnt;
    const double upm = td5_geob_units_per_m() > 1.0 ? td5_geob_units_per_m() : 430.0;

    if (car_u2) *car_u2 = 0.0;
    if (arm_u2) *arm_u2 = 0.0;
    if (!nl || n < 3 || !tg_network_built()) return 0;
    ns = fc_samples(rx, rz, n, sx, sz, &ni, &cell, &area);
    if (ns <= 0) return 0;
    nq = tg_xstreet_collect(nl, si, side, quad, qk, FC_MAX_QUAD);
    if (nq <= 0) return 0;
    cnt = (ni >= 4) ? ni : ns;             /* a sliver has no interior grid: use the boundary samples */
    for (k = 0; k < cnt; k++) {
        int on_car = 0, on_arm = 0;
        for (i = 0; i < nq; i++) {
            if (!fc_in_quad(&quad[i * 8], sx[k], sz[k])) continue;
            if (qk[i]) on_arm = 1; else on_car = 1;
        }
        if (on_car) car += cell;
        else if (on_arm) arm += cell;
    }
    if (car_u2) *car_u2 = car;
    if (arm_u2) *arm_u2 = arm;
    return car >= FC_CAR_M2 * upm * upm || (arms && arm >= FC_ARM_M2 * upm * upm);
}

static int fc_core(const TG_NodeList *nl, int si, double side, double base_y,
                   double *rx, double *rz, int n, double maxpush, int arms, const char **why)
{
    double sx[FC_MAX_SAMP], sz[FC_MAX_SAMP], area, cell, total = 0.0;
    FcSeg  cand[FC_MAX_SEG];
    double x0, z0, x1, z1;
    int    ns, ni, k, i, pass, ncand, fk, kk;

    if (!nl || n < 3 || !td5_geo_loaded()) return 0;
    if (!td5_env_flag_on("TD5RE_GEO_BLD_FOOTCLEAR")) return 0;
    ns = fc_samples(rx, rz, n, sx, sz, &ni, &cell, &area);
    if (ns <= 0) return 0;

    /* --- ROADS: every span segment, main and corridor, as a capsule --------------------------------- */
    for (pass = 0; pass < 6; pass++) {
        double worst = 0.0, ux = 0.0, uz = 0.0;
        ncand = 0;
        x0 = 1e300; z0 = 1e300; x1 = -1e300; z1 = -1e300;
        for (k = 0; k < n; k++) {
            if (rx[k] < x0) x0 = rx[k];
            if (rx[k] > x1) x1 = rx[k];
            if (rz[k] < z0) z0 = rz[k];
            if (rz[k] > z1) z1 = rz[k];
        }
        for (i = 0; i + 1 < nl->count; i++) {
            const TG_Node *a = &nl->v[i], *b = &nl->v[i + 1];
            if (fabs(b->x - a->x) > 6000.0 || fabs(b->z - a->z) > 6000.0) continue;     /* not contiguous */
            if (fabs(a->x - x0) > 40000.0 && fabs(a->x - x1) > 40000.0) continue;       /* far away, cheap reject */
            fc_push_seg(cand, &ncand, a->x, a->z, b->x, b->z,
                        0.5 * (a->width > b->width ? a->width : b->width), fc_band(nl, i),
                        a->y, b->y, base_y, x0, z0, x1, z1);
        }
        /* A FORK CORRIDOR is laid at its main node's frame plus the fork's lateral shift (br_sidewalk /
         * tg_emit_branch_*: x = node.x + node.tz * e, z = node.z - node.tx * e), and the main-node segment
         * above does not contain it: on a bend the two diverge by metres. Same rows, same shift and width
         * scale the corridor meshes use. A plaza fork's free corridor lays its own row chain. A block loop
         * rides real streets (the cross-street / road tests see those). */
        for (fk = 0; fk < s_fork_count; fk++) {
            const int cl = tg_fork_clen(fk);
            if (tg_fork_is_loop(fk)) continue;
            if (tg_fork_is_free(fk)) {
                const TG_NodeList *rv = tg_pf_rview(fk);
                const int F1 = s_forks[fk].F + 1;
                if (!rv || !rv->v) continue;
                for (kk = 0; kk < cl && F1 + kk + 1 < rv->count; kk++) {
                    const TG_Node *a = &rv->v[F1 + kk], *b = &rv->v[F1 + kk + 1];
                    /* the row's width is the WHOLE road (both halves, tg_pf wfull); the corridor carriageway is
                     * its own `lanes` (lb) wide -- measured: rview width 6000, mesh cross-section 3000 */
                    const double lw = (double)TD5_TG_LANE_WIDTH;
                    const double ha = 0.5 * (a->lanes > 0 ? (double)a->lanes * lw : a->width);
                    const double hb = 0.5 * (b->lanes > 0 ? (double)b->lanes * lw : b->width);
                    if (fabs(b->x - a->x) > 6000.0 || fabs(b->z - a->z) > 6000.0) continue;
                    fc_push_seg(cand, &ncand, a->x, a->z, b->x, b->z, ha > hb ? ha : hb,
                                fc_band(nl, tg_fork_row_node(fk, kk)), a->y, b->y, base_y, x0, z0, x1, z1);
                }
                continue;
            }
            for (kk = 0; kk < cl; kk++) {
                const int m0 = tg_fork_row_node(fk, kk), m1 = tg_fork_row_node(fk, kk + 1);
                const TG_Node *a, *b;
                double e0, e1, cx0, cz0, cx1, cz1, h0, h1;
                if (m0 < 0 || m1 < 0 || m0 >= nl->count || m1 >= nl->count) continue;
                a = &nl->v[m0]; b = &nl->v[m1];
                e0 = tg_fork_br_shift(fk, kk,     a->width);
                e1 = tg_fork_br_shift(fk, kk + 1, b->width);
                cx0 = a->x + a->tz * e0; cz0 = a->z - a->tx * e0;
                cx1 = b->x + b->tz * e1; cz1 = b->z - b->tx * e1;
                h0 = a->width * tg_fork_br_wscale(fk, kk) * 0.5;
                h1 = b->width * tg_fork_br_wscale(fk, kk + 1) * 0.5;
                if (fabs(cx1 - cx0) > 6000.0 || fabs(cz1 - cz0) > 6000.0) continue;
                fc_push_seg(cand, &ncand, cx0, cz0, cx1, cz1, h0 > h1 ? h0 : h1, fc_band(nl, m0),
                            a->y, b->y, base_y, x0, z0, x1, z1);
            }
        }
        /* the network's polyline roads: ribbons, back streets, country lanes */
        {
            double pr[6 * 96];
            const int np = tg_net_polyroads(x0 - 4000.0, z0 - 4000.0, x1 + 4000.0, z1 + 4000.0, pr, 96);
            for (i = 0; i < np; i++) {
                const double *q = &pr[i * 6];
                fc_push_seg(cand, &ncand, q[0], q[1], q[2], q[3], q[4], q[5] >= 0.0 ? fc_band(nl, (int)q[5]) : 0.0,
                            base_y, base_y, base_y, x0, z0, x1, z1);
            }
        }
        for (k = 0; k < ns; k++) {
            for (i = 0; i < ncand; i++) {
                const FcSeg *g = &cand[i];
                const double hw = g->hw + g->band - FC_GRAZE;
                const double dx = g->x1 - g->x0, dz = g->z1 - g->z0;
                const double L2 = dx * dx + dz * dz;
                double t, px, pz, d, pen;
                if (L2 < 1.0) continue;
                t = ((sx[k] - g->x0) * dx + (sz[k] - g->z0) * dz) / L2;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                px = g->x0 + t * dx; pz = g->z0 + t * dz;
                d = sqrt((sx[k] - px) * (sx[k] - px) + (sz[k] - pz) * (sz[k] - pz));
                pen = hw - d;
                if (pen > worst) {
                    worst = pen;
                    if (d > 1.0) { ux = (sx[k] - px) / d; uz = (sz[k] - pz) / d; }
                    else { const double L = sqrt(L2); ux = side * dz / L; uz = -side * dx / L; }
                }
            }
        }
        if (worst <= 0.0) break;
        worst += 50.0;
        total += worst;
        if (total > maxpush) { if (why) *why = "DROPPED on a road past the push cap"; return 1; }
        for (k = 0; k < n; k++)  { rx[k] += ux * worst; rz[k] += uz * worst; }
        for (k = 0; k < ns; k++) { sx[k] += ux * worst; sz[k] += uz * worst; }
    }
    if (pass >= 6) { if (why) *why = "DROPPED on a road, push did not settle"; return 1; }

    /* --- STREETS: laid carriageway + pavement arm, by plan area -------------------------------- */
    if (tg_geo_poly_street_overlap(nl, si, side, rx, rz, n, arms, NULL, NULL)) {
        if (why) *why = "DROPPED on a cross street or its pavement arm";
        return 1;
    }
    return 0;
}

int tg_geo_footprint_clear(const TG_NodeList *nl, int si, double side, double base_y,
                           double *rx, double *rz, int n, double maxpush, const char **why)
{
    return fc_core(nl, si, side, base_y, rx, rz, n, maxpush, 1, why);
}

/* A cross-street frontage wall's base line (cx,cz) + (ox,oz) * flen, as a 300-wide strip. It stands at the
 * BACK EDGE of the street's pavement arm by construction (that is what the arm-width setback is), so only
 * the carriageways and roads count against it, not the arm it abuts. Never moved: 1 = skip the wall. */
int tg_geo_wall_clear(const TG_NodeList *nl, int si, double side, double by,
                      double cx, double cz, double ox, double oz, double flen)
{
    double qx[4], qz[4];
    qx[0] = cx - oz * 150.0;             qz[0] = cz + ox * 150.0;
    qx[1] = cx + oz * 150.0;             qz[1] = cz - ox * 150.0;
    qx[2] = cx + ox * flen + oz * 150.0; qz[2] = cz + oz * flen - ox * 150.0;
    qx[3] = cx + ox * flen - oz * 150.0; qz[3] = cz + oz * flen + ox * 150.0;
    return fc_core(nl, si, side, by, qx, qz, 4, 0.0, 0, NULL);
}

/* A procedural box (tg_bg_building_box: front edge (bx,bz) + (ax,az), back vectors (lx0,lz0)/(lx1,lz1) of
 * length `depth`) against everything tg_geo_footprint_clear knows. Never moved: 1 = skip the box. */
int tg_geo_box_clear(const TG_NodeList *nl, int si, double side, double by,
                     double bx, double bz, double ax, double az,
                     double lx0, double lz0, double lx1, double lz1, double depth)
{
    double qx[4], qz[4];
    qx[0] = bx;                        qz[0] = bz;
    qx[1] = bx + ax;                   qz[1] = bz + az;
    qx[2] = bx + ax + lx1 * depth;     qz[2] = bz + az + lz1 * depth;
    qx[3] = bx + lx0 * depth;          qz[3] = bz + lz0 * depth;
    return tg_geo_footprint_clear(nl, si, side, by, qx, qz, 4, 0.0, NULL);
}

void tg_geo_clear_note_wall(void)      { s_walls++; }
void tg_geo_clear_note_footprint(void) { s_foot++; }

void tg_geo_clear_report(void)
{
    if (!s_ready) return;
    TD5_LOG_I(LOG_TAG, "[GEO CLEAR] %ld procedural frontage span-side(s) stood "
              "down because a real road crosses them, %ld real footprint(s) "
              "dropped for standing on one", s_walls, s_foot);
}
