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

void tg_geo_clear_note_wall(void)      { s_walls++; }
void tg_geo_clear_note_footprint(void) { s_foot++; }

void tg_geo_clear_report(void)
{
    if (!s_ready) return;
    TD5_LOG_I(LOG_TAG, "[GEO CLEAR] %ld procedural frontage span-side(s) stood "
              "down because a real road crosses them, %ld real footprint(s) "
              "dropped for standing on one", s_walls, s_foot);
}
