/**
 * td5_roam_locate.c -- GEO FREE ROAM: the road locator (PORT-ONLY, F1 task T6b).
 * Contract and frames: td5_roam_locate.h. Design: GEO_FREE_ROAM_DESIGN.md 1.9, 2.4.
 *
 * SECTIONS: resident set, candidate tiles, point-in-primitive, collect (gather every
 * containing primitive), primitive evaluation (height / normal / surface / lane),
 * boundary-edge search, td5_roam_locate (deck choice + outside path), dev self-test.
 *
 * DETERMINISM. The answer never depends on the order tiles were added to the set:
 * candidates are visited in lattice order and every tie is broken by
 * (tz, tx, span-before-tri, index), which is a property of the data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_fp.h"
#include "td5_track.h"            /* td5_track_{quad,tri}_height_v, td5_track_quad_vertices */
#include "td5_roam_locate.h"

#define LOG_TAG "roam"

#define LOC_EPS_CELL   0.01       /* lattice units: a point this close to a cell edge also scans the next cell */
#define LOC_WALL_RINGS 3          /* cell rings searched for the nearest wall before the global scan (24 m) */
#define LOC_NO_LAYER   (-1000)

typedef struct { int slot; double ut, vt; } RCand;

/* ============================================================================
 * SECTION: resident set
 * ========================================================================== */

static unsigned hash_xy(int tx, int tz)
{
    uint32_t h = (uint32_t)tx * 73856093u ^ (uint32_t)tz * 19349663u;
    h ^= h >> 15;
    return (unsigned)(h & (ROAM_RES_HASH - 1));
}

static void rehash(TD5_RoamResident *rs)
{
    int i;
    double mm = 0.0;
    for (i = 0; i < ROAM_RES_HASH; i++) rs->hash[i] = -1;
    for (i = 0; i < rs->nslot; i++) {
        const RoamTile *t = rs->tile[i];
        unsigned h;
        double m;
        if (!t) continue;
        h = hash_xy(t->tx, t->tz);
        while (rs->hash[h] >= 0) h = (h + 1u) & (ROAM_RES_HASH - 1);
        rs->hash[h] = (int16_t)i;
        m = (double)t->grid_margin * (double)t->grid_cell;
        if (m > mm) mm = m;
    }
    rs->max_margin = mm;
}

void td5_roam_resident_init(TD5_RoamResident *rs, const RoamGrid *g,
                            int32_t org_x, int32_t org_z, int32_t org_y)
{
    memset(rs, 0, sizeof *rs);
    rs->grid = *g;
    rs->org_x = org_x; rs->org_z = org_z; rs->org_y = org_y;
    rehash(rs);
}

int td5_roam_resident_find(const TD5_RoamResident *rs, int tx, int tz)
{
    unsigned h = hash_xy(tx, tz);
    int n = 0;
    while (rs->hash[h] >= 0 && n++ < ROAM_RES_HASH) {
        const RoamTile *t = rs->tile[rs->hash[h]];
        if (t && t->tx == tx && t->tz == tz) return rs->hash[h];
        h = (h + 1u) & (ROAM_RES_HASH - 1);
    }
    return -1;
}

int td5_roam_resident_add(TD5_RoamResident *rs, const RoamTile *t)
{
    int slot;
    if (!t || td5_roam_resident_find(rs, t->tx, t->tz) >= 0) return -1;
    for (slot = 0; slot < rs->nslot; slot++) if (!rs->tile[slot]) break;
    if (slot == rs->nslot) {
        if (rs->nslot >= ROAM_RES_MAX) return -1;
        rs->nslot++;
    }
    rs->tile[slot] = t;
    rs->ntile++;
    rehash(rs);
    return slot;
}

int td5_roam_resident_remove(TD5_RoamResident *rs, int tx, int tz)
{
    const int slot = td5_roam_resident_find(rs, tx, tz);
    if (slot < 0) return 0;
    rs->tile[slot] = NULL;
    rs->ntile--;
    while (rs->nslot > 0 && !rs->tile[rs->nslot - 1]) rs->nslot--;
    rehash(rs);
    return 1;
}

/* ============================================================================
 * SECTION: candidate tiles
 * ========================================================================== */

/* The tiles whose grid (their square grown by their margin, plus `extra` lattice
 * units) contains the place-frame point. At most 9. Lattice order, so the result does
 * not depend on slot order. */
static int res_cands(const TD5_RoamResident *rs, int64_t px, int64_t pz, double extra, RCand *out)
{
    const double S = (double)rs->grid.tile;
    double u, v, du, dv, m;
    int tx0, tz0, dxlo = 0, dxhi = 0, dzlo = 0, dzhi = 0, dx, dz, n = 0;
    if (rs->ntile <= 0) return 0;
    roam_grid_uv(&rs->grid, (double)px, (double)pz, &u, &v);
    tx0 = (int)floor(u / S);
    tz0 = (int)floor(v / S);
    du = u - (double)tx0 * S;
    dv = v - (double)tz0 * S;
    m = rs->max_margin + extra;
    if (du < m) dxlo = -1;
    if (du >= S - m) dxhi = 1;
    if (dv < m) dzlo = -1;
    if (dv >= S - m) dzhi = 1;
    for (dz = dzlo; dz <= dzhi; dz++) {
        for (dx = dxlo; dx <= dxhi; dx++) {
            const int slot = td5_roam_resident_find(rs, tx0 + dx, tz0 + dz);
            double ut, vt, mt;
            const RoamTile *t;
            if (slot < 0) continue;
            t = rs->tile[slot];
            ut = u - (double)(tx0 + dx) * S;
            vt = v - (double)(tz0 + dz) * S;
            mt = (double)t->grid_margin * (double)t->grid_cell + extra;
            if (ut >= -mt && ut < S + mt && vt >= -mt && vt < S + mt) {
                out[n].slot = slot; out[n].ut = ut; out[n].vt = vt;
                n++;
            }
        }
    }
    return n;
}

/* ============================================================================
 * SECTION: point in primitive (integer, tile-local frame, edges inclusive)
 * ========================================================================== */

static inline int tri_has(int32_t ax, int32_t az, int32_t bx, int32_t bz, int32_t cx, int32_t cz,
                          int32_t px, int32_t pz)
{
    const int64_t d1 = (int64_t)(bx - ax) * (pz - az) - (int64_t)(bz - az) * (px - ax);
    const int64_t d2 = (int64_t)(cx - bx) * (pz - bz) - (int64_t)(cz - bz) * (px - bx);
    const int64_t d3 = (int64_t)(ax - cx) * (pz - cz) - (int64_t)(az - cz) * (px - cx);
    const int neg = (d1 < 0) | (d2 < 0) | (d3 < 0);
    const int pos = (d1 > 0) | (d2 > 0) | (d3 > 0);
    return !(neg && pos);
}

static inline int span_lanes(const TD5_StripSpan *sp)
{
    const int L = sp->pad_02[1] & 0x0F;
    return L < 1 ? 1 : L;
}

/* Lane of span `sp` that contains the tile-local point, or -1. The lane quad is its
 * two diagonal halves (vl0,vr1,vl1) and (vl0,vr0,vr1), the strip's own split. */
static int span_lane_at(const RoamTile *t, const TD5_StripSpan *sp, int32_t lx, int32_t lz)
{
    const TD5_StripVertex *vt = t->vert;
    const int L = span_lanes(sp);
    const int32_t dx = lx - sp->origin_x, dz = lz - sp->origin_z;
    int vl0, vl1, vr0, vr1, l;
    int32_t x0, x1, z0, z1, t0, t1;
    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
    /* cheap reject: the four corner vertices bound the (straight) rows */
    {
        const TD5_StripVertex *a = &vt[vl0], *b = &vt[vl0 + L], *c = &vt[vr0], *d = &vt[vr0 + L];
        x0 = x1 = a->x; z0 = z1 = a->z;
        t0 = b->x; t1 = b->z; if (t0 < x0) x0 = t0; if (t0 > x1) x1 = t0; if (t1 < z0) z0 = t1; if (t1 > z1) z1 = t1;
        t0 = c->x; t1 = c->z; if (t0 < x0) x0 = t0; if (t0 > x1) x1 = t0; if (t1 < z0) z0 = t1; if (t1 > z1) z1 = t1;
        t0 = d->x; t1 = d->z; if (t0 < x0) x0 = t0; if (t0 > x1) x1 = t0; if (t1 < z0) z0 = t1; if (t1 > z1) z1 = t1;
        if (dx < x0 - 4 || dx > x1 + 4 || dz < z0 - 4 || dz > z1 + 4) return -1;
    }
    for (l = 0; l < L; l++) {
        const TD5_StripVertex *a = &vt[vl0 + l], *b = &vt[vl1 + l], *c = &vt[vr0 + l], *d = &vt[vr1 + l];
        if (tri_has(a->x, a->z, d->x, d->z, b->x, b->z, dx, dz) ||
            tri_has(a->x, a->z, c->x, c->z, d->x, d->z, dx, dz))
            return l;
    }
    return -1;
}

/* Normal sign. The strip's winding gives an upward normal (uny > 0). The roam tile
 * data is wound the other way round: every ribbon lane quad (T4) and every patch
 * triangle (T3, CCW north-up) comes out of triangle_height with uny < 0. Reversing
 * the winding negates the plane normal and leaves the plane, hence the integer
 * height, unchanged (the solve divides two negated numbers, and the unit-normal
 * truncation is symmetric), so the locator returns the negated normal when uny < 0:
 * exactly what the strip code returns for the same plane wound the strip's way. The
 * winding itself is the builder's business (see the T6b report). uny == 0 cannot
 * come back: triangle_height forces it to 1. */
static inline void normal_up(int16_t *n)
{
    if (n[1] < 0) { n[0] = (int16_t)-n[0]; n[1] = (int16_t)-n[1]; n[2] = (int16_t)-n[2]; }
}

static inline int tri_contains(const RoamTile *t, const RoamTri *tr, int32_t lx, int32_t lz)
{
    const TD5_StripVertex *a = &t->vert[tr->v[0]], *b = &t->vert[tr->v[1]], *c = &t->vert[tr->v[2]];
    const int32_t dx = lx - tr->ox, dz = lz - tr->oz;
    return tri_has(a->x, a->z, b->x, b->z, c->x, c->z, dx, dz);
}

static const RoamEdgeRec *edge_of_span(const RoamTile *t, uint32_t si)
{
    uint32_t i;
    for (i = 0; i < t->nedge; i++) {
        const RoamEdgeRec *e = &t->edge[i];
        if (si >= e->span_first && si < (uint32_t)e->span_first + e->span_count) return e;
    }
    return NULL;
}

/* ============================================================================
 * SECTION: primitive evaluation
 * ========================================================================== */

static inline int32_t up256(int32_t v) { return (int32_t)((int64_t)v * 256); }

typedef struct {
    int32_t h;                  /* 24.8, PLACE frame y */
    int16_t n[3];
    int     capped;
} HeightRes;

static int32_t span_height(const RoamTile *t, const TD5_StripSpan *sp, int lane, int32_t lx, int32_t lz,
                           int16_t *n, int *cap)
{
    const int32_t h = td5_track_quad_height_v(t->vert, sp, lane, up256(lx), up256(lz), n, cap);
    normal_up(n);
    return h;
}

static int32_t tri_height(const RoamTile *t, const RoamTri *tr, int32_t lx, int32_t lz, int16_t *n, int *cap)
{
    const int32_t h = td5_track_tri_height_v(t->vert, tr->v[0], tr->v[1], tr->v[2], tr->ox, tr->oy, tr->oz,
                                             up256(lx), up256(lz), n, cap);
    normal_up(n);
    return h;
}

/* Ribbon geometry for along / lateral: centre line of the quad's two rows. */
static void span_geom(const RoamTile *t, const TD5_StripSpan *sp, int32_t lx, int32_t lz,
                      double *along_u, double *lateral)
{
    const int L = span_lanes(sp);
    int vl0, vl1, vr0, vr1;
    double c0x, c0z, c1x, c1z, dx, dz, len2, sx, sz, slen, px, pz, tt, fx, fz;
    const TD5_StripVertex *n0, *nL, *f0, *fL;
    td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
    n0 = &t->vert[vl0]; nL = &t->vert[vl0 + L]; f0 = &t->vert[vr0]; fL = &t->vert[vr0 + L];
    c0x = 0.5 * (n0->x + nL->x); c0z = 0.5 * (n0->z + nL->z);
    c1x = 0.5 * (f0->x + fL->x); c1z = 0.5 * (f0->z + fL->z);
    dx = c1x - c0x; dz = c1z - c0z;
    len2 = dx * dx + dz * dz;
    px = (double)(lx - sp->origin_x) - c0x;
    pz = (double)(lz - sp->origin_z) - c0z;
    tt = len2 > 0.0 ? (px * dx + pz * dz) / len2 : 0.0;
    *along_u = (tt < 0.0 ? 0.0 : (tt > 1.0 ? 1.0 : tt)) * sqrt(len2);
    sx = (double)(nL->x - n0->x); sz = (double)(nL->z - n0->z);
    slen = sqrt(sx * sx + sz * sz);
    fx = px - tt * dx; fz = pz - tt * dz;
    *lateral = slen > 0.0 ? (fx * sx + fz * sz) / slen : 0.0;
}

static inline int32_t iround(double v) { return (int32_t)floor(v + 0.5); }

/* Everything but the point answer, from a resolved primitive. */
static void fill_hit(const TD5_RoamResident *rs, uint32_t slot, int is_tri, uint32_t idx, int lane,
                     int32_t h_place, const int16_t *n, int capped, int32_t lx, int32_t lz,
                     int inside, TD5_RoamHit *out)
{
    const RoamTile *t = rs->tile[slot];
    memset(out, 0, sizeof *out);
    out->prim = ROAM_PRIM_MAKE(slot, is_tri, idx);
    out->ground_y = (int32_t)((int64_t)h_place - (int64_t)rs->org_y * 256);
    out->normal[0] = n[0]; out->normal[1] = n[1]; out->normal[2] = n[2];
    out->capped = (uint8_t)(capped != 0);
    out->inside = (uint8_t)(inside != 0);
    out->is_patch = (uint8_t)(is_tri != 0);
    out->wall = 0xFFFFFFFFu;
    if (is_tri) {
        const RoamTri *tr = &t->tri[idx];
        out->lane = -1;
        out->surface = (uint8_t)(tr->surface & 0x0F);
        out->edge_key = tr->patch < t->nnode ? (uint64_t)t->node[tr->patch].cluster_key : 0u;
    } else {
        const TD5_StripSpan *sp = &t->span[idx];
        const RoamEdgeRec *e = edge_of_span(t, idx);
        const uint8_t mask = sp->pad_02[0];
        double au, lat;
        out->lane = (int8_t)lane;
        out->surface = (mask & (1u << (lane & 7))) ? (uint8_t)((sp->surface_attribute >> 4) | 0x10)
                                                    : (uint8_t)(sp->surface_attribute & 0x0F);
        out->surface &= 0x0F;
        if (e) {
            out->edge_key = e->key;
            out->layer = e->layer;
            out->along = (int32_t)(e->row0 + (idx - e->span_first)) * 1500;
        }
        span_geom(t, sp, lx, lz, &au, &lat);
        out->along += iround(au);
        out->lateral = iround(lat);
    }
}

/* The plane of one primitive at a tile-local point; the lane is forced when >= 0
 * (wall edges name theirs), else the containing lane, else the nearest lane. */
static void eval_prim(const TD5_RoamResident *rs, uint32_t slot, int is_tri, uint32_t idx,
                      int force_lane, int32_t lx, int32_t lz, TD5_RoamHit *out)
{
    const RoamTile *t = rs->tile[slot];
    int16_t n[3];
    int cap = 0, inside, lane = -1;
    int32_t h;
    if (is_tri) {
        const RoamTri *tr = &t->tri[idx];
        inside = tri_contains(t, tr, lx, lz);
        h = tri_height(t, tr, lx, lz, n, &cap);
    } else {
        const TD5_StripSpan *sp = &t->span[idx];
        const int L = span_lanes(sp);
        lane = span_lane_at(t, sp, lx, lz);
        inside = lane >= 0;
        if (force_lane >= 0) lane = force_lane;
        if (lane < 0) {
            /* nearest lane by projecting onto the near row */
            int vl0, vl1, vr0, vr1;
            const TD5_StripVertex *n0, *nL;
            double rx, rz, tt;
            td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
            n0 = &t->vert[vl0]; nL = &t->vert[vl0 + L];
            rx = (double)(nL->x - n0->x); rz = (double)(nL->z - n0->z);
            tt = (rx * rx + rz * rz) > 0.0
               ? ((double)(lx - sp->origin_x - n0->x) * rx + (double)(lz - sp->origin_z - n0->z) * rz) / (rx * rx + rz * rz)
               : 0.0;
            lane = (int)floor(tt * (double)L);
            if (lane < 0) lane = 0;
            if (lane > L - 1) lane = L - 1;
        }
        h = span_height(t, sp, lane, lx, lz, n, &cap);
    }
    fill_hit(rs, slot, is_tri, idx, lane, h, n, cap, lx, lz, inside, out);
}

int td5_roam_prim_eval(const TD5_RoamResident *rs, uint32_t prim, int32_t x, int32_t z, TD5_RoamHit *out)
{
    const uint32_t slot = ROAM_PRIM_SLOT(prim), idx = ROAM_PRIM_INDEX(prim);
    const int is_tri = (int)ROAM_PRIM_IS_TRI(prim);
    const RoamTile *t;
    memset(out, 0, sizeof *out);
    out->prim = ROAM_PRIM_NONE;
    if (prim == ROAM_PRIM_NONE || slot >= (uint32_t)rs->nslot || !(t = rs->tile[slot])) return 0;
    if (is_tri ? idx >= t->ntri : idx >= t->nspan) return 0;
    eval_prim(rs, slot, is_tri, idx, -1,
              (int32_t)((int64_t)FP_TRUNC(x) + rs->org_x - t->org_x),
              (int32_t)((int64_t)FP_TRUNC(z) + rs->org_z - t->org_z), out);
    return 1;
}

int td5_roam_prim_layer(const TD5_RoamResident *rs, uint32_t prim)
{
    const uint32_t slot = ROAM_PRIM_SLOT(prim), idx = ROAM_PRIM_INDEX(prim);
    const RoamTile *t;
    if (prim == ROAM_PRIM_NONE || slot >= (uint32_t)rs->nslot || !(t = rs->tile[slot])) return -128;
    if (ROAM_PRIM_IS_TRI(prim)) return idx < t->ntri ? 0 : -128;
    if (idx >= t->nspan) return -128;
    {
        const RoamEdgeRec *e = edge_of_span(t, idx);
        return e ? (int)e->layer : 0;
    }
}

const RoamEdgeRec *td5_roam_prim_edge(const TD5_RoamResident *rs, uint32_t prim)
{
    const uint32_t slot = ROAM_PRIM_SLOT(prim), idx = ROAM_PRIM_INDEX(prim);
    const RoamTile *t;
    if (prim == ROAM_PRIM_NONE || ROAM_PRIM_IS_TRI(prim) || slot >= (uint32_t)rs->nslot ||
        !(t = rs->tile[slot]) || idx >= t->nspan)
        return NULL;
    return edge_of_span(t, idx);
}

/* ============================================================================
 * SECTION: collect (every containing primitive)
 * ========================================================================== */

static inline int tile_key_cmp(const RoamTile *a, const RoamTile *b)
{
    if (a->tz != b->tz) return a->tz < b->tz ? -1 : 1;
    if (a->tx != b->tx) return a->tx < b->tx ? -1 : 1;
    return 0;
}

/* (tz, tx, span before tri, index): a property of the data, not of the slot order. */
static int prim_order(const TD5_RoamResident *rs, uint32_t a, uint32_t b)
{
    const int c = tile_key_cmp(rs->tile[ROAM_PRIM_SLOT(a)], rs->tile[ROAM_PRIM_SLOT(b)]);
    if (c) return c;
    if (ROAM_PRIM_IS_TRI(a) != ROAM_PRIM_IS_TRI(b)) return ROAM_PRIM_IS_TRI(a) ? 1 : -1;
    if (ROAM_PRIM_INDEX(a) != ROAM_PRIM_INDEX(b)) return ROAM_PRIM_INDEX(a) < ROAM_PRIM_INDEX(b) ? -1 : 1;
    return 0;
}

static int collect_place(const TD5_RoamResident *rs, int64_t px, int64_t pz, TD5_RoamCand *out, int cap)
{
    RCand rc[9];
    int nc, ci, n = 0, i, j;
    nc = res_cands(rs, px, pz, 0.0, rc);
    for (ci = 0; ci < nc; ci++) {
        const RoamTile *t = rs->tile[rc[ci].slot];
        const double cell = (double)t->grid_cell;
        const int m = (int)t->grid_margin, nx = (int)t->grid_nx, nz = (int)t->grid_nz;
        const int32_t lx = (int32_t)(px - t->org_x), lz = (int32_t)(pz - t->org_z);
        int cu0 = (int)floor((rc[ci].ut - LOC_EPS_CELL) / cell), cu1 = (int)floor((rc[ci].ut + LOC_EPS_CELL) / cell);
        int cv0 = (int)floor((rc[ci].vt - LOC_EPS_CELL) / cell), cv1 = (int)floor((rc[ci].vt + LOC_EPS_CELL) / cell);
        int cu, cv;
        if (cu0 < -m) cu0 = -m;
        if (cv0 < -m) cv0 = -m;
        if (cu1 > nx - 1 - m) cu1 = nx - 1 - m;
        if (cv1 > nz - 1 - m) cv1 = nz - 1 - m;
        for (cv = cv0; cv <= cv1; cv++) {
            for (cu = cu0; cu <= cu1; cu++) {
                const uint32_t cc = (uint32_t)((cv + m) * nx + (cu + m));
                uint32_t k;
                for (k = t->grid_start[cc]; k < t->grid_start[cc + 1]; k++) {
                    const uint32_t item = t->grid_item[k];
                    const uint32_t kind = ROAM_ITEM_KIND(item), idx = ROAM_ITEM_INDEX(item);
                    uint32_t prim;
                    int16_t nrm[3];
                    int capd = 0, lane = 0;
                    int32_t h;
                    if (kind == ROAM_ITEM_BEDGE) continue;
                    if (kind == ROAM_ITEM_SPAN) {
                        if (idx >= t->nspan) continue;
                        lane = span_lane_at(t, &t->span[idx], lx, lz);
                        if (lane < 0) continue;
                        prim = ROAM_PRIM_MAKE(rc[ci].slot, 0, idx);
                    } else {
                        if (idx >= t->ntri || !tri_contains(t, &t->tri[idx], lx, lz)) continue;
                        prim = ROAM_PRIM_MAKE(rc[ci].slot, 1, idx);
                    }
                    for (i = 0; i < n; i++) if (out[i].prim == prim) break;
                    if (i < n) continue;            /* same primitive from a neighbouring cell */
                    if (n >= cap) continue;
                    h = kind == ROAM_ITEM_SPAN ? span_height(t, &t->span[idx], lane, lx, lz, nrm, &capd)
                                               : tri_height(t, &t->tri[idx], lx, lz, nrm, &capd);
                    out[n].prim = prim;
                    out[n].h = (int32_t)((int64_t)h - (int64_t)rs->org_y * 256);
                    out[n].normal[0] = nrm[0]; out[n].normal[1] = nrm[1]; out[n].normal[2] = nrm[2];
                    out[n].lane = (uint8_t)(kind == ROAM_ITEM_SPAN ? lane : 0);
                    out[n].capped = (uint8_t)capd;
                    n++;
                }
            }
        }
    }
    /* resident-order-independent output order */
    for (i = 1; i < n; i++) {
        TD5_RoamCand c = out[i];
        for (j = i - 1; j >= 0 && prim_order(rs, out[j].prim, c.prim) > 0; j--) out[j + 1] = out[j];
        out[j + 1] = c;
    }
    return n;
}

int td5_roam_collect(const TD5_RoamResident *rs, int32_t x, int32_t z, TD5_RoamCand *out, int cap)
{
    return collect_place(rs, (int64_t)FP_TRUNC(x) + rs->org_x, (int64_t)FP_TRUNC(z) + rs->org_z, out, cap);
}

/* ============================================================================
 * SECTION: boundary edges
 * ========================================================================== */

#define WALL_EX_MAX    24         /* edges one probe may suppress / refine before the search gives up       */
#define WALL_SAMPLE    50.0       /* units between coverage samples along an edge (11 cm)                    */
#define WALL_COVER_EPS 8.0        /* units: how far outside an edge the "is it covered" test point sits    */

/* Per-search table: an edge whose outward side is another drivable surface is not a wall (design
 * 2.4 with T3b: the wall is the boundary of the UNION of the drivable surface). Mode 0 drops the
 * edge for this search, mode 1 replaces its distance by the nearest UNCOVERED part's. */
typedef struct { uint32_t slot, bi; int mode; double d2, qx, qz, tpar; } WallEx;

typedef struct {
    int      found;
    double   d2, tpar, qx, qz;      /* squared distance, parameter on the segment, foot point (tile-local of `slot`) */
    uint32_t slot, bi;
    int32_t  lx, lz;                /* the probe in that tile's frame */
    int      nex;
    WallEx   ex[WALL_EX_MAX];
} WallBest;

static void bedge_ends(const RoamTile *t, const RoamBEdge *be, int32_t *ax, int32_t *az, int32_t *bx, int32_t *bz)
{
    int32_t ox, oz;
    const TD5_StripVertex *v0 = &t->vert[be->v[0]], *v1 = &t->vert[be->v[1]];
    if (be->prim & ROAM_PRIM_TRI) {
        const RoamTri *tr = &t->tri[be->prim & 0x7FFF];
        ox = tr->ox; oz = tr->oz;
    } else {
        const TD5_StripSpan *sp = &t->span[be->prim];
        ox = sp->origin_x; oz = sp->origin_z;
    }
    *ax = ox + v0->x; *az = oz + v0->z;
    *bx = ox + v1->x; *bz = oz + v1->z;
}

static void own_centroid(const RoamTile *t, int is_tri, uint32_t idx, double *cx, double *cz)
{
    double sx = 0.0, sz = 0.0;
    int k, n = 0;
    if (is_tri) {
        const RoamTri *tr = &t->tri[idx];
        for (k = 0; k < 3; k++) { sx += (double)(tr->ox + t->vert[tr->v[k]].x); sz += (double)(tr->oz + t->vert[tr->v[k]].z); n++; }
    } else {
        const TD5_StripSpan *sp = &t->span[idx];
        const int L = span_lanes(sp);
        int vl0, vl1, vr0, vr1, j;
        td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
        for (j = 0; j <= L; j++) {
            sx += (double)(sp->origin_x + t->vert[vl0 + j].x + sp->origin_x + t->vert[vr0 + j].x);
            sz += (double)(sp->origin_z + t->vert[vl0 + j].z + sp->origin_z + t->vert[vr0 + j].z);
            n += 2;
        }
    }
    *cx = sx / n; *cz = sz / n;
}

/* Is the point (tile-local, on boundary edge `bi`) lying inside another drivable primitive of about
 * the same level? Tested a hair outside the edge, away from its own primitive. A surface more than
 * 1.5 m away vertically (a deck over a road) does not cover. */
static int wall_point_covered(const TD5_RoamResident *rs, uint32_t slot, uint32_t bi, double qx, double qz)
{
    const RoamTile *t = rs->tile[slot];
    const RoamBEdge *be = &t->bedge[bi];
    const int is_tri = (be->prim & ROAM_PRIM_TRI) != 0;
    const uint32_t pidx = is_tri ? (uint32_t)(be->prim & 0x7FFF) : (uint32_t)be->prim;
    const uint32_t own = ROAM_PRIM_MAKE(slot, is_tri, pidx);
    int32_t ax, az, bx, bz;
    double dx, dz, len, nx, nz, cx, cz;
    TD5_RoamCand cand[ROAM_LOC_CAND];
    TD5_RoamHit eh;
    int n, i;
    bedge_ends(t, be, &ax, &az, &bx, &bz);
    dx = (double)(bx - ax); dz = (double)(bz - az);
    len = sqrt(dx * dx + dz * dz);
    if (len < 1.0) return 0;
    own_centroid(t, is_tri, pidx, &cx, &cz);
    nx = -dz / len; nz = dx / len;
    if ((cx - qx) * nx + (cz - qz) * nz > 0.0) { nx = -nx; nz = -nz; }          /* outward */
    eval_prim(rs, slot, is_tri, pidx, -1, iround(qx), iround(qz), &eh);
    n = collect_place(rs, (int64_t)t->org_x + iround(qx + nx * WALL_COVER_EPS),
                      (int64_t)t->org_z + iround(qz + nz * WALL_COVER_EPS), cand, ROAM_LOC_CAND);
    for (i = 0; i < n; i++) {
        const int64_t dh = (int64_t)cand[i].h - (int64_t)eh.ground_y;
        if (cand[i].prim != own && dh <= ROAM_DECK_DROP && dh >= -ROAM_DECK_DROP) return 1;
    }
    return 0;
}

/* Percentage (0..100) of boundary edge `bi` of resident slot `slot` that lies inside another
 * drivable primitive (sampled every WALL_SAMPLE units): 100 = never a wall, 0 = a full wall. */
int td5_roam_wall_coverage(const TD5_RoamResident *rs, uint32_t slot, uint32_t bi)
{
    const RoamTile *t;
    const RoamBEdge *be;
    int32_t ax, az, bx, bz;
    double dx, dz, len;
    int k, nsamp, cov = 0;
    if (slot >= (uint32_t)rs->nslot || !(t = rs->tile[slot]) || bi >= t->nbedge) return 0;
    be = &t->bedge[bi];
    bedge_ends(t, be, &ax, &az, &bx, &bz);
    dx = (double)(bx - ax); dz = (double)(bz - az);
    len = sqrt(dx * dx + dz * dz);
    nsamp = (int)(len / WALL_SAMPLE) + 1;
    if (nsamp < 2) nsamp = 2;
    for (k = 0; k <= nsamp; k++) {
        const double tt = (double)k / (double)nsamp;
        if (wall_point_covered(rs, slot, bi, (double)ax + tt * dx, (double)az + tt * dz)) cov++;
    }
    return (100 * cov) / (nsamp + 1);
}

static void wall_consider(const TD5_RoamResident *rs, uint32_t slot, uint32_t bi, int layer,
                          int32_t lx, int32_t lz, WallBest *best)
{
    const RoamTile *t = rs->tile[slot];
    const RoamBEdge *be = &t->bedge[bi];
    int32_t ax, az, bx, bz;
    double dx, dz, len2, tt, qx, qz, ex, ez, d2;
    int k;
    if (layer != LOC_NO_LAYER && (int)(int8_t)be->layer != layer) return;
    for (k = 0; k < best->nex; k++)
        if (best->ex[k].slot == slot && best->ex[k].bi == bi) break;
    if (k < best->nex) {
        if (best->ex[k].mode == 0) return;
        d2 = best->ex[k].d2; qx = best->ex[k].qx; qz = best->ex[k].qz; tt = best->ex[k].tpar;
    } else {
        bedge_ends(t, be, &ax, &az, &bx, &bz);
        dx = (double)(bx - ax); dz = (double)(bz - az);
        len2 = dx * dx + dz * dz;
        tt = len2 > 0.0 ? ((double)(lx - ax) * dx + (double)(lz - az) * dz) / len2 : 0.0;
        if (tt < 0.0) tt = 0.0;
        if (tt > 1.0) tt = 1.0;
        qx = (double)ax + tt * dx; qz = (double)az + tt * dz;
        ex = qx - (double)lx; ez = qz - (double)lz;
        d2 = ex * ex + ez * ez;
    }
    if (best->found) {
        if (d2 > best->d2) return;
        if (d2 == best->d2) {
            const int c = tile_key_cmp(t, rs->tile[best->slot]);
            if (c > 0 || (c == 0 && bi >= best->bi)) return;
        }
    }
    best->found = 1; best->d2 = d2; best->tpar = tt; best->qx = qx; best->qz = qz;
    best->slot = slot; best->bi = bi; best->lx = lx; best->lz = lz;
}

static void wall_scan_cell(const TD5_RoamResident *rs, uint32_t slot, int cu, int cv, int layer,
                           int32_t lx, int32_t lz, WallBest *best)
{
    const RoamTile *t = rs->tile[slot];
    const int m = (int)t->grid_margin, nx = (int)t->grid_nx, nz = (int)t->grid_nz;
    uint32_t k, cc;
    if (cu < -m || cv < -m || cu > nx - 1 - m || cv > nz - 1 - m) return;
    cc = (uint32_t)((cv + m) * nx + (cu + m));
    for (k = t->grid_start[cc]; k < t->grid_start[cc + 1]; k++) {
        const uint32_t item = t->grid_item[k];
        if (ROAM_ITEM_KIND(item) != ROAM_ITEM_BEDGE) continue;
        if (ROAM_ITEM_INDEX(item) >= t->nbedge) continue;
        wall_consider(rs, slot, ROAM_ITEM_INDEX(item), layer, lx, lz, best);
    }
}

/* Every boundary edge of every resident tile (the geo_nearest_global fallback; the
 * self-test also uses it as the brute-force reference for the grid search). */
static void wall_scan_all(const TD5_RoamResident *rs, int64_t px, int64_t pz, int layer, WallBest *best,
                          int prune)
{
    int s;
    if (!prune) {
        for (s = 0; s < rs->nslot; s++) {
            const RoamTile *t = rs->tile[s];
            uint32_t bi;
            if (!t) continue;
            for (bi = 0; bi < t->nbedge; bi++)
                wall_consider(rs, (uint32_t)s, bi, layer, (int32_t)(px - t->org_x), (int32_t)(pz - t->org_z), best);
        }
        return;
    }
    {
        /* Nearest tiles first: every boundary edge of a tile lies inside its grid square, so the
         * distance from the probe to that square bounds the distance to any of its edges. */
        double lb[ROAM_RES_MAX];
        uint8_t done[ROAM_RES_MAX];
        const double S = (double)rs->grid.tile;
        double u, v;
        roam_grid_uv(&rs->grid, (double)px, (double)pz, &u, &v);
        for (s = 0; s < rs->nslot; s++) {
            const RoamTile *t = rs->tile[s];
            double ut, vt, mt, du, dv;
            done[s] = 0;
            if (!t) { lb[s] = 1e300; done[s] = 1; continue; }
            ut = u - (double)t->tx * S; vt = v - (double)t->tz * S;
            mt = (double)t->grid_margin * (double)t->grid_cell;
            du = ut < -mt ? -mt - ut : (ut > S + mt ? ut - S - mt : 0.0);
            dv = vt < -mt ? -mt - vt : (vt > S + mt ? vt - S - mt : 0.0);
            lb[s] = sqrt(du * du + dv * dv);
        }
        for (;;) {
            int pick = -1;
            uint32_t bi;
            const RoamTile *t;
            for (s = 0; s < rs->nslot; s++)
                if (!done[s] && (pick < 0 || lb[s] < lb[pick])) pick = s;
            if (pick < 0) break;
            if (best->found && lb[pick] > sqrt(best->d2)) break;
            done[pick] = 1;
            t = rs->tile[pick];
            for (bi = 0; bi < t->nbedge; bi++)
                wall_consider(rs, (uint32_t)pick, bi, layer, (int32_t)(px - t->org_x), (int32_t)(pz - t->org_z), best);
        }
    }
}

/* Nearest boundary edge: cell rings 0..3 around the probe in every candidate tile,
 * then a scan of every resident tile's BEDG (design 1.9 step 7). */
static int wall_search_raw(const TD5_RoamResident *rs, int64_t px, int64_t pz, int layer, WallBest *best,
                           int brute)
{
    RCand rc[9];
    int nc, ci, r;
    best->found = 0;
    if (brute) { wall_scan_all(rs, px, pz, layer, best, 0); return best->found; }
    nc = res_cands(rs, px, pz, (double)(LOC_WALL_RINGS + 1) * (double)ROAM_GRID_CELL, rc);
    for (r = 0; r <= LOC_WALL_RINGS; r++) {
        for (ci = 0; ci < nc; ci++) {
            const RoamTile *t = rs->tile[rc[ci].slot];
            const double cell = (double)t->grid_cell;
            const int32_t lx = (int32_t)(px - t->org_x), lz = (int32_t)(pz - t->org_z);
            const int cu = (int)floor(rc[ci].ut / cell), cv = (int)floor(rc[ci].vt / cell);
            int a, b;
            if (r == 0) { wall_scan_cell(rs, (uint32_t)rc[ci].slot, cu, cv, layer, lx, lz, best); continue; }
            for (a = -r; a <= r; a++) {
                wall_scan_cell(rs, (uint32_t)rc[ci].slot, cu + a, cv - r, layer, lx, lz, best);
                wall_scan_cell(rs, (uint32_t)rc[ci].slot, cu + a, cv + r, layer, lx, lz, best);
            }
            for (b = -r + 1; b <= r - 1; b++) {
                wall_scan_cell(rs, (uint32_t)rc[ci].slot, cu - r, cv + b, layer, lx, lz, best);
                wall_scan_cell(rs, (uint32_t)rc[ci].slot, cu + r, cv + b, layer, lx, lz, best);
            }
        }
        /* anything in ring r+1 or beyond is at least r cells away */
        if (best->found && sqrt(best->d2) <= (double)r * (double)ROAM_GRID_CELL) return 1;
    }
    if (best->found) return 1;
    wall_scan_all(rs, px, pz, layer, best, 1);
    return best->found;
}

/* The nearest edge of the UNION's boundary: the raw nearest edge, unless its foot lies inside another
 * drivable primitive. Then the edge is replaced by its nearest uncovered part (sampled), or dropped
 * when none is left, and the search repeats. brute = reference scan over every edge (self-test). */
static int wall_search(const TD5_RoamResident *rs, int64_t px, int64_t pz, int layer, WallBest *best, int brute)
{
    int iter;
    memset(best, 0, sizeof *best);
    for (iter = 0; iter <= WALL_EX_MAX; iter++) {
        const RoamTile *t;
        int k;
        if (!wall_search_raw(rs, px, pz, layer, best, brute)) return 0;
        for (k = 0; k < best->nex; k++)
            if (best->ex[k].slot == best->slot && best->ex[k].bi == best->bi) break;
        if (k < best->nex) return 1;                          /* an already refined (uncovered) part */
        if (!wall_point_covered(rs, best->slot, best->bi, best->qx, best->qz)) return 1;
        if (best->nex >= WALL_EX_MAX) return 1;               /* give up refining, keep the nearest */
        t = rs->tile[best->slot];
        {
            WallEx *e = &best->ex[best->nex];
            const RoamBEdge *be = &t->bedge[best->bi];
            int32_t ax, az, bx, bz;
            double dx, dz, len, bd2 = -1.0;
            int s, nsamp;
            bedge_ends(t, be, &ax, &az, &bx, &bz);
            dx = (double)(bx - ax); dz = (double)(bz - az);
            len = sqrt(dx * dx + dz * dz);
            nsamp = (int)(len / WALL_SAMPLE) + 1;
            if (nsamp < 2) nsamp = 2;
            e->slot = best->slot; e->bi = best->bi; e->mode = 0;
            for (s = 0; s <= nsamp; s++) {
                const double tt = (double)s / (double)nsamp;
                const double qx = (double)ax + tt * dx, qz = (double)az + tt * dz;
                const double ex = qx - (double)best->lx, ez = qz - (double)best->lz;
                const double d2 = ex * ex + ez * ez;
                if (wall_point_covered(rs, best->slot, best->bi, qx, qz)) continue;
                if (bd2 < 0.0 || d2 < bd2) { bd2 = d2; e->mode = 1; e->d2 = d2; e->qx = qx; e->qz = qz; e->tpar = tt; }
            }
            best->nex++;
        }
    }
    return best->found;
}

/* Lane a wall edge belongs to (kerbs: lane 0 / L-1; a dead-end cap: the lane at the
 * foot point). */
static int wall_lane(const RoamTile *t, const TD5_StripSpan *sp, const RoamBEdge *be, double tpar)
{
    const int L = span_lanes(sp);
    const int lv = sp->left_vertex_index, rv = sp->right_vertex_index;
    int j[2], k;
    double jf;
    int lane;
    for (k = 0; k < 2; k++) {
        const int v = be->v[k];
        j[k] = (v >= lv && v <= lv + L) ? v - lv : v - rv;
        if (j[k] < 0) j[k] = 0;
        if (j[k] > L) j[k] = L;
    }
    (void)t;
    jf = (double)j[0] + tpar * (double)(j[1] - j[0]);
    lane = (int)floor(jf);
    if (lane < 0) lane = 0;
    if (lane > L - 1) lane = L - 1;
    return lane;
}

/* The answer for a probe outside every surface: the plane of the wall edge's own
 * primitive, extrapolated (same upward cap as the strip), plus the push. */
static void fill_outside(const TD5_RoamResident *rs, const WallBest *w, TD5_RoamHit *out)
{
    const RoamTile *t = rs->tile[w->slot];
    const RoamBEdge *be = &t->bedge[w->bi];
    const int is_tri = (be->prim & ROAM_PRIM_TRI) != 0;
    const uint32_t idx = is_tri ? (uint32_t)(be->prim & 0x7FFF) : (uint32_t)be->prim;
    int lane = -1;
    if (!is_tri) lane = wall_lane(t, &t->span[idx], be, w->tpar);
    eval_prim(rs, w->slot, is_tri, idx, lane, w->lx, w->lz, out);
    out->inside = 0;
    out->wall = w->bi;
    out->push_x = iround(w->qx - (double)w->lx);
    out->push_z = iround(w->qz - (double)w->lz);
    out->push_dist = iround(sqrt(w->d2));
}

/* ============================================================================
 * SECTION: td5_roam_locate
 * ========================================================================== */

static const RoamEdgeRec *prim_edge_rec(const TD5_RoamResident *rs, uint32_t prim)
{
    return td5_roam_prim_edge(rs, prim);
}

static uint64_t prim_edge_key(const TD5_RoamResident *rs, uint32_t prim)
{
    const uint32_t slot = ROAM_PRIM_SLOT(prim), idx = ROAM_PRIM_INDEX(prim);
    const RoamTile *t = rs->tile[slot];
    if (ROAM_PRIM_IS_TRI(prim)) {
        const RoamTri *tr = &t->tri[idx];
        return tr->patch < t->nnode ? (uint64_t)t->node[tr->patch].cluster_key : 0u;
    }
    {
        const RoamEdgeRec *e = prim_edge_rec(rs, prim);
        return e ? e->key : 0u;
    }
}

/* a is a better pick than b: same edge as the hint first, then the data order */
static int better_tie(const TD5_RoamResident *rs, const TD5_RoamCand *a, const TD5_RoamCand *b,
                      int hint_ok, uint64_t hint_edge)
{
    if (hint_ok) {
        const int ha = prim_edge_key(rs, a->prim) == hint_edge, hb = prim_edge_key(rs, b->prim) == hint_edge;
        if (ha != hb) return ha;
    }
    return prim_order(rs, a->prim, b->prim) < 0;
}

int td5_roam_locate(const TD5_RoamResident *rs, int32_t x, int32_t z, int32_t y,
                    int has_y, uint32_t hint, TD5_RoamHit *out)
{
    TD5_RoamCand cand[ROAM_LOC_CAND];
    int n, i, hint_ok = 0, hint_layer = LOC_NO_LAYER, best = -1, from_above = 0;
    uint64_t hint_edge = 0;
    const int64_t px = (int64_t)FP_TRUNC(x) + rs->org_x, pz = (int64_t)FP_TRUNC(z) + rs->org_z;
    int32_t ylim = 0;
    WallBest w;

    memset(out, 0, sizeof *out);
    out->prim = ROAM_PRIM_NONE;
    out->lane = -1;
    out->wall = 0xFFFFFFFFu;

    if (hint != ROAM_PRIM_NONE) {
        const uint32_t hs = ROAM_PRIM_SLOT(hint), hi = ROAM_PRIM_INDEX(hint);
        if (hs < (uint32_t)rs->nslot && rs->tile[hs] &&
            (ROAM_PRIM_IS_TRI(hint) ? hi < rs->tile[hs]->ntri : hi < rs->tile[hs]->nspan)) {
            hint_ok = 1;
            hint_edge = prim_edge_key(rs, hint);
            hint_layer = td5_roam_prim_layer(rs, hint);
        }
    }

    n = collect_place(rs, px, pz, cand, ROAM_LOC_CAND);

    if (n > 0) {
        if (has_y) {
            ylim = (int32_t)((int64_t)y + ROAM_STEP_UP);
            for (i = 0; i < n; i++) {
                if (cand[i].h > ylim) continue;
                if (best < 0 || cand[i].h > cand[best].h ||
                    (cand[i].h == cand[best].h && better_tie(rs, &cand[i], &cand[best], hint_ok, hint_edge)))
                    best = i;
            }
            if (best < 0) {
                from_above = 1;
                for (i = 0; i < n; i++)
                    if (best < 0 || cand[i].h < cand[best].h ||
                        (cand[i].h == cand[best].h && better_tie(rs, &cand[i], &cand[best], hint_ok, hint_edge)))
                        best = i;
            }
        } else {
            for (i = 0; i < n; i++) {
                int pick;
                if (best < 0) { best = i; continue; }
                if (hint_ok) {
                    const int ei = prim_edge_key(rs, cand[i].prim) == hint_edge;
                    const int eb = prim_edge_key(rs, cand[best].prim) == hint_edge;
                    if (ei != eb) { if (ei) best = i; continue; }
                }
                pick = cand[i].h > cand[best].h ||
                       (cand[i].h == cand[best].h && prim_order(rs, cand[i].prim, cand[best].prim) < 0);
                if (pick) best = i;
            }
        }
        /* design 1.9 step 6: on a deck, a surface far below is not ground, it is the
         * side of the bridge */
        if (has_y && hint_ok && hint_layer >= 1 && !from_above && cand[best].h < (int32_t)((int64_t)y - ROAM_DECK_DROP)) {
            if (wall_search(rs, px, pz, hint_layer, &w, 0)) {
                fill_outside(rs, &w, out);
                return 1;
            }
        }
        {
            const uint32_t slot = ROAM_PRIM_SLOT(cand[best].prim), idx = ROAM_PRIM_INDEX(cand[best].prim);
            const RoamTile *t = rs->tile[slot];
            fill_hit(rs, slot, (int)ROAM_PRIM_IS_TRI(cand[best].prim), idx, cand[best].lane,
                     (int32_t)((int64_t)cand[best].h + (int64_t)rs->org_y * 256), cand[best].normal,
                     cand[best].capped, (int32_t)(px - t->org_x), (int32_t)(pz - t->org_z), 1, out);
        }
        return 1;
    }

    /* nothing contains the point: is any tile responsible for it at all? */
    {
        RCand rc[9];
        if (res_cands(rs, px, pz, 0.0, rc) == 0) return 0;
    }
    if (!wall_search(rs, px, pz, hint_ok ? hint_layer : LOC_NO_LAYER, &w, 0) &&
        !(hint_ok && wall_search(rs, px, pz, LOC_NO_LAYER, &w, 0)))
        return 0;
    fill_outside(rs, &w, out);
    return 1;
}

/* ============================================================================
 * SECTION: dev self-test
 * ========================================================================== */
#ifndef TD5RE_RELEASE
#include <windows.h>
#include "td5_config.h"
#include "td5_geo.h"

#define MAX_TESTTILES 1024

typedef struct {
    RoamTile  tile[MAX_TESTTILES];
    int       n;
    RoamGrid  grid;
    int32_t   org_x, org_z;
} TestSet;

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static double qpc_ns(LONGLONG ticks, LONGLONG freq) { return (double)ticks * 1e9 / (double)freq; }

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int32_t w24(int64_t place, int32_t org) { return (int32_t)((place - org) * 256); }

/* corners of lane `l` of span `si`, PLACE frame */
static void lane_corners(const RoamTile *t, uint32_t si, int l, int64_t cx[4], int64_t cz[4])
{
    const TD5_StripSpan *sp = &t->span[si];
    int vl0, vl1, vr0, vr1, k;
    int idx[4];
    td5_track_quad_vertices(sp, l, &vl0, &vl1, &vr0, &vr1);
    idx[0] = vl0; idx[1] = vl1; idx[2] = vr1; idx[3] = vr0;
    for (k = 0; k < 4; k++) {
        cx[k] = (int64_t)t->org_x + sp->origin_x + t->vert[idx[k]].x;
        cz[k] = (int64_t)t->org_z + sp->origin_z + t->vert[idx[k]].z;
    }
}

static void prim_centroid(const RoamTile *t, uint32_t prim15, int is_tri, double *cx, double *cz)
{
    double sx = 0, sz = 0;
    int k, n = 0;
    if (is_tri) {
        const RoamTri *tr = &t->tri[prim15];
        for (k = 0; k < 3; k++) { sx += (double)t->org_x + tr->ox + t->vert[tr->v[k]].x; sz += (double)t->org_z + tr->oz + t->vert[tr->v[k]].z; n++; }
    } else {
        const TD5_StripSpan *sp = &t->span[prim15];
        const int L = span_lanes(sp);
        int vl0, vl1, vr0, vr1, j;
        td5_track_quad_vertices(sp, 0, &vl0, &vl1, &vr0, &vr1);
        for (j = 0; j <= L; j++) {
            sx += (double)t->org_x + sp->origin_x + t->vert[vl0 + j].x; sz += (double)t->org_z + sp->origin_z + t->vert[vl0 + j].z;
            sx += (double)t->org_x + sp->origin_x + t->vert[vr0 + j].x; sz += (double)t->org_z + sp->origin_z + t->vert[vr0 + j].z;
            n += 2;
        }
    }
    *cx = sx / n; *cz = sz / n;
}

/* The casco has no deck over a road (its 6 bridge quads cross water), so grade
 * separation is checked on a hand-built tile: a 2-lane road at y = 1000 and a
 * narrower 2-lane deck 3080 units (7.16 m, the strip's lift) above it, same footprint
 * centre. Returns the number of failed expectations; *ntests gets the count run. */
static int synth_decks(FILE *f, int *ntests)
{
    enum { NX = 34, NCELL = NX * NX, PER = 6 };
    TD5_StripSpan sp[2];
    TD5_StripVertex vt[12];
    RoamEdgeRec er[2];
    RoamBEdge be[4];
    RoamTile t;
    RoamGrid g;
    TD5_RoamResident *rs = (TD5_RoamResident *)calloc(1, sizeof *rs);
    uint32_t *gw = (uint32_t *)calloc(5u + NCELL + 1u + (size_t)NCELL * PER, sizeof(uint32_t));
    int c, row, j, fails = 0, n = 0;
    const int64_t X0 = 55000, Z0 = 55000;
    const int32_t road_h = 1000 * 256, deck_h = (1000 + 3080) * 256;
    const uint32_t road = ROAM_PRIM_MAKE(0, 0, 0), deck = ROAM_PRIM_MAKE(0, 0, 1);
    if (!rs || !gw) { free(rs); free(gw); return 1; }
    memset(sp, 0, sizeof sp); memset(vt, 0, sizeof vt); memset(er, 0, sizeof er); memset(be, 0, sizeof be);
    memset(&t, 0, sizeof t); memset(&g, 0, sizeof g);
    for (c = 0; c < 2; c++) {
        sp[c].span_type = 1; sp[c].surface_attribute = 0x11; sp[c].pad_02[1] = 2;
        sp[c].left_vertex_index = (uint16_t)(c * 6); sp[c].right_vertex_index = (uint16_t)(c * 6 + 3);
        sp[c].link_next = sp[c].link_prev = -1;
        sp[c].origin_x = (int32_t)X0; sp[c].origin_z = (int32_t)Z0;
        sp[c].origin_y = c == 0 ? 1000 : 1000 + 3080;
        for (row = 0; row < 2; row++)
            for (j = 0; j < 3; j++) {
                TD5_StripVertex *v = &vt[c * 6 + row * 3 + j];
                v->x = (int16_t)((c == 0 ? -3000 : -1500) + (c == 0 ? 3000 : 1500) * j);
                v->y = 0;
                v->z = (int16_t)(row == 0 ? -750 : 750);
            }
        er[c].key = (uint64_t)(c + 1); er[c].lanes = 2; er[c].layer = (int8_t)c;
        er[c].flags = c == 1 ? ROAM_EF_DECK : 0;
        er[c].span_first = (uint16_t)c; er[c].span_count = 1;
    }
    be[0].v[0] = 0; be[0].v[1] = 3; be[0].prim = 0; be[0].kind = ROAM_BEDGE_KERB; be[0].layer = 0;
    be[1].v[0] = 2; be[1].v[1] = 5; be[1].prim = 0; be[1].kind = ROAM_BEDGE_KERB; be[1].layer = 0;
    be[2].v[0] = 6; be[2].v[1] = 9; be[2].prim = 1; be[2].kind = ROAM_BEDGE_DECK; be[2].layer = 1;
    be[3].v[0] = 8; be[3].v[1] = 11; be[3].prim = 1; be[3].kind = ROAM_BEDGE_DECK; be[3].layer = 1;
    gw[0] = NX; gw[1] = NX; gw[2] = ROAM_GRID_CELL; gw[3] = 1; gw[4] = 0;
    for (c = 0; c <= NCELL; c++) gw[5 + c] = (uint32_t)c * PER;
    for (c = 0; c < NCELL; c++) {
        uint32_t *it = gw + 5 + NCELL + 1 + (size_t)c * PER;
        it[0] = (ROAM_ITEM_SPAN << 30) | 0u; it[1] = (ROAM_ITEM_SPAN << 30) | 1u;
        for (j = 0; j < 4; j++) it[2 + j] = (ROAM_ITEM_BEDGE << 30) | (uint32_t)j;
    }
    t.tx = 0; t.tz = 0; t.org_x = 0; t.org_z = 0;
    t.span = sp; t.nspan = 2; t.vert = vt; t.nvert = 12; t.bedge = be; t.nbedge = 4; t.edge = er; t.nedge = 2;
    t.grid = gw; t.ngrid_words = 5u + NCELL + 1u + NCELL * PER;
    t.grid_nx = NX; t.grid_nz = NX; t.grid_cell = ROAM_GRID_CELL; t.grid_margin = 1;
    t.grid_start = gw + 5; t.grid_item = gw + 5 + NCELL + 1; t.nitem = NCELL * PER;
    g.c = 1.0; g.s = 0.0; g.tile = ROAM_TILE_UNITS;
    td5_roam_resident_init(rs, &g, 0, 0, 0);
    td5_roam_resident_add(rs, &t);

#define SYN(desc, cond) do { n++; if (!(cond)) { fails++; fprintf(f, "  (e) synthetic FAIL: %s (prim %08x inside %d h %d layer %d push %d,%d)\n", \
        desc, h.prim, h.inside, h.ground_y >> 8, h.layer, h.push_x, h.push_z); } } while (0)
    {
        TD5_RoamHit h;
        const int32_t xm = w24(X0, 0), zm = w24(Z0, 0);          /* under the deck centre */
        const int32_t xo = w24(X0 + 2000, 0);                    /* on the road, 500 units past the deck's side */
        td5_roam_locate(rs, xm, zm, deck_h + 860 * 256, 1, ROAM_PRIM_NONE, &h);
        SYN("2 m above the deck, no hint -> deck", h.prim == deck && h.inside && h.ground_y == deck_h && h.layer == 1);
        td5_roam_locate(rs, xm, zm, road_h + 215 * 256, 1, ROAM_PRIM_NONE, &h);
        SYN("0.5 m above the road under the deck, no hint -> road", h.prim == road && h.inside && h.ground_y == road_h);
        td5_roam_locate(rs, xm, zm, deck_h, 1, deck, &h);
        SYN("on the deck, hint deck -> deck", h.prim == deck && h.inside);
        td5_roam_locate(rs, xm, zm, road_h + 215 * 256, 1, road, &h);
        SYN("under the deck, hint road -> road", h.prim == road && h.inside);
        td5_roam_locate(rs, xm, zm, deck_h, 1, road, &h);
        SYN("probe at deck height, hint road -> deck (it is the highest surface <= the probe)", h.prim == deck && h.inside);
        td5_roam_locate(rs, xm, zm, 0, 0, ROAM_PRIM_NONE, &h);
        SYN("no Y, no hint -> the highest surface (deck)", h.prim == deck && h.inside);
        td5_roam_locate(rs, xm, zm, 0, 0, road, &h);
        SYN("no Y, hint road -> road", h.prim == road && h.inside);
        td5_roam_locate(rs, xm, zm, 0, 0, deck, &h);
        SYN("no Y, hint deck -> deck", h.prim == deck && h.inside);
        td5_roam_locate(rs, xo, zm, deck_h, 1, deck, &h);
        SYN("driving off the side of the deck (hint deck, probe at deck height): outside, push toward the deck, plane = the deck's, not the road below",
            h.prim == deck && !h.inside && h.push_x < 0 && abs(h.push_dist - 500) <= 1 && h.ground_y > deck_h - 200 * 256 && h.layer == 1);
        td5_roam_locate(rs, xo, zm, road_h + 215 * 256, 1, road, &h);
        SYN("beside the deck, on the road (hint road) -> road", h.prim == road && h.inside && h.ground_y == road_h);
        td5_roam_locate(rs, xo, zm, deck_h, 1, ROAM_PRIM_NONE, &h);
        SYN("probe at deck height past the deck side, no hint -> falls to the road", h.prim == road && h.inside);
    }
#undef SYN
    if (ntests) *ntests = n;
    free(rs); free(gw);
    return fails;
}

/* Two flat 2-lane carriageways of one level that overlap (the Av 25 / Av 13 case of T3b): A is
 * x -3000..3000 x z -750..750, B is x -1500..4500 x z 250..1750. A's right kerb (x = 3000) lies
 * inside B for z 250..750, B's left kerb (x = -1500) inside A for the same z range. */
static int synth_overlap(FILE *f, int *ntests)
{
    enum { NX = 34, NCELL = NX * NX, PER = 6 };
    TD5_StripSpan sp[2];
    TD5_StripVertex vt[12];
    RoamEdgeRec er[2];
    RoamBEdge be[4];
    RoamTile t;
    RoamGrid g;
    TD5_RoamResident *rs = (TD5_RoamResident *)calloc(1, sizeof *rs);
    uint32_t *gw = (uint32_t *)calloc(5u + NCELL + 1u + (size_t)NCELL * PER, sizeof(uint32_t));
    int c, row, j, fails = 0, n = 0;
    const int64_t X0 = 55000, Z0 = 55000;
    static const int xl[2] = { -3000, -1500 }, zc[2] = { 0, 1000 };
    if (!rs || !gw) { free(rs); free(gw); return 1; }
    memset(sp, 0, sizeof sp); memset(vt, 0, sizeof vt); memset(er, 0, sizeof er); memset(be, 0, sizeof be);
    memset(&t, 0, sizeof t); memset(&g, 0, sizeof g);
    for (c = 0; c < 2; c++) {
        sp[c].span_type = 1; sp[c].surface_attribute = 0x11; sp[c].pad_02[1] = 2;
        sp[c].left_vertex_index = (uint16_t)(c * 6); sp[c].right_vertex_index = (uint16_t)(c * 6 + 3);
        sp[c].link_next = sp[c].link_prev = -1;
        sp[c].origin_x = (int32_t)X0; sp[c].origin_z = (int32_t)(Z0 + zc[c]); sp[c].origin_y = 1000;
        for (row = 0; row < 2; row++)
            for (j = 0; j < 3; j++) {
                TD5_StripVertex *v = &vt[c * 6 + row * 3 + j];
                v->x = (int16_t)(xl[c] + 3000 * j);
                v->y = 0;
                v->z = (int16_t)(row == 0 ? -750 : 750);
            }
        er[c].key = (uint64_t)(c + 1); er[c].lanes = 2;
        er[c].span_first = (uint16_t)c; er[c].span_count = 1;
    }
    be[0].v[0] = 0; be[0].v[1] = 3; be[0].prim = 0; be[0].kind = ROAM_BEDGE_KERB;
    be[1].v[0] = 2; be[1].v[1] = 5; be[1].prim = 0; be[1].kind = ROAM_BEDGE_KERB;
    be[2].v[0] = 6; be[2].v[1] = 9; be[2].prim = 1; be[2].kind = ROAM_BEDGE_KERB;
    be[3].v[0] = 8; be[3].v[1] = 11; be[3].prim = 1; be[3].kind = ROAM_BEDGE_KERB;
    gw[0] = NX; gw[1] = NX; gw[2] = ROAM_GRID_CELL; gw[3] = 1; gw[4] = 0;
    for (c = 0; c <= NCELL; c++) gw[5 + c] = (uint32_t)c * PER;
    for (c = 0; c < NCELL; c++) {
        uint32_t *it = gw + 5 + NCELL + 1 + (size_t)c * PER;
        it[0] = (ROAM_ITEM_SPAN << 30) | 0u; it[1] = (ROAM_ITEM_SPAN << 30) | 1u;
        for (j = 0; j < 4; j++) it[2 + j] = (ROAM_ITEM_BEDGE << 30) | (uint32_t)j;
    }
    t.span = sp; t.nspan = 2; t.vert = vt; t.nvert = 12; t.bedge = be; t.nbedge = 4; t.edge = er; t.nedge = 2;
    t.grid = gw; t.ngrid_words = 5u + NCELL + 1u + NCELL * PER;
    t.grid_nx = NX; t.grid_nz = NX; t.grid_cell = ROAM_GRID_CELL; t.grid_margin = 1;
    t.grid_start = gw + 5; t.grid_item = gw + 5 + NCELL + 1; t.nitem = NCELL * PER;
    g.c = 1.0; g.s = 0.0; g.tile = ROAM_TILE_UNITS;
    td5_roam_resident_init(rs, &g, 0, 0, 0);
    td5_roam_resident_add(rs, &t);
#define SYN(desc, cond) do { n++; if (!(cond)) { fails++; fprintf(f, "  (f) synthetic FAIL: %s (prim %08x inside %d push %d,%d dist %d)\n", \
        desc, h.prim, h.inside, h.push_x, h.push_z, h.push_dist); } } while (0)
    {
        TD5_RoamHit h;
        const int cv0 = td5_roam_wall_coverage(rs, 0, 0), cv1 = td5_roam_wall_coverage(rs, 0, 1);
        const int cv2 = td5_roam_wall_coverage(rs, 0, 2), cv3 = td5_roam_wall_coverage(rs, 0, 3);
        memset(&h, 0, sizeof h);
        SYN("A's left kerb is a full wall (coverage 0)", cv0 == 0);
        SYN("A's right kerb is covered by B for a third of its length (20..45 %)", cv1 >= 20 && cv1 <= 45);
        SYN("B's left kerb is covered by A for a third of its length (20..45 %)", cv2 >= 20 && cv2 <= 45);
        SYN("B's right kerb is a full wall (coverage 0)", cv3 == 0);
        td5_roam_locate(rs, w24(X0 + 3200, 0), w24(Z0 - 300, 0), 0, 0, ROAM_PRIM_NONE, &h);
        SYN("outside past A's uncovered kerb part: push 200 units back in", !h.inside && h.push_x < 0 && abs(h.push_dist - 200) <= 1);
        td5_roam_locate(rs, w24(X0 - 1700, 0), w24(Z0 + 900, 0), 0, 0, ROAM_PRIM_NONE, &h);
        SYN("outside past B's uncovered kerb part: push 200 units back in", !h.inside && h.push_x > 0 && abs(h.push_dist - 200) <= 1);
        td5_roam_locate(rs, w24(X0 - 1400, 0), w24(Z0 + 500, 0), 0, 0, ROAM_PRIM_NONE, &h);
        SYN("inside the overlap: on a surface, no push", h.inside && h.push_dist == 0);
    }
#undef SYN
    if (ntests) *ntests = n;
    free(rs); free(gw);
    return fails;
}

int td5_roam_locate_dev_test(void)
{
    static const char place[] = "la_plata_partido";
    TestSet *ts;
    TD5_RoamResident *rs, *rr;
    char path[512], err[160], lines[256];
    FILE *rf, *f;
    int (*list)[2];
    int nlist, i, k, loaded = 0, missing = 0;
    const char *dir;
    long fail_total = 0;
    uint32_t t_start = td5_plat_time_ms();
    LARGE_INTEGER qf, q0, q1;
    long a_tot = 0, a_self = 0, a_other = 0, a_fail = 0, a_deg = 0, a_dn = 0, a_tri = 0, a_quad = 0, a_dn_tri = 0, a_other_far = 0;
    int a_other_dh = 0;
    long a_w_tri = 0, a_w_quad = 0, a_w_fail = 0;
    long b_brute = 0, b_brute_bad = 0, b_sliver = 0, b_cov_full = 0, b_cov_part = 0, b_supp_mid = 0;
    long b_tot = 0, b_pass = 0, b_fail = 0, b_skip_in = 0, b_skip_other = 0, b_skip_tri = 0, b_deg = 0, b_deck = 0, b_deck_pass = 0, b_dist_fail = 0, b_dot_fail = 0;
    long c_gap_same = 0, c_graze = 0;
    long c_samp = 0, c_perm = 0, c_pair = 0, c_pair_dh_max = 0, c_gap = 0, c_both_in = 0, c_edgekey = 0;
    long e_decks = 0, e_low = 0, e_up_ok = 0, e_up_fail = 0, e_dn_ok = 0, e_dn_fail = 0, e_hint_ok = 0, e_hint_fail = 0;
    int shown = 0;

    if (!td5_env_flag_off("TD5RE_ROAM_LOCATE_TEST")) return 0;
    ts = (TestSet *)calloc(1, sizeof *ts);
    rs = (TD5_RoamResident *)calloc(1, sizeof *rs);
    rr = (TD5_RoamResident *)calloc(1, sizeof *rr);
    list = (int (*)[2])malloc(2048 * sizeof(int[2]));
    if (!ts || !rs || !rr || !list) { TD5_LOG_E(LOG_TAG, "roam locate test: out of memory"); return 1; }

    /* lattice + tiles */
    td5_geo_source_path(path, sizeof path, place, "roam/ROAMGRID.JSON");
    rf = fopen(path, "rb");
    if (!rf) { TD5_LOG_E(LOG_TAG, "roam locate test: no %s (run --RoamBuildTiles=casco first)", path); return 1; }
    {
        char txt[4096];
        size_t nr = fread(txt, 1, sizeof txt - 1, rf);
        fclose(rf);
        txt[nr] = 0;
        if (!roam_grid_parse(txt, &ts->grid)) { TD5_LOG_E(LOG_TAG, "roam locate test: ROAMGRID.JSON does not parse"); return 1; }
    }
    nlist = td5_roam_casco_tiles(&ts->grid, list, 2048);
    {
        double sx = 0, sz = 0;
        for (i = 0; i < nlist && ts->n < MAX_TESTTILES; i++) {
            td5_roam_tile_path(path, sizeof path, NULL, place, list[i][0], list[i][1]);
            if (td5_roam_tile_read(path, &ts->tile[ts->n], 0, err, sizeof err)) {
                sx += ts->tile[ts->n].org_x; sz += ts->tile[ts->n].org_z;
                ts->n++; loaded++;
            } else {
                missing++;
                if (missing <= 3) TD5_LOG_W(LOG_TAG, "roam locate test: tile %d,%d: %s", list[i][0], list[i][1], err);
            }
        }
        if (ts->n == 0) { TD5_LOG_E(LOG_TAG, "roam locate test: no tiles loaded"); return 1; }
        ts->org_x = (int32_t)(sx / ts->n); ts->org_z = (int32_t)(sz / ts->n);
    }
    td5_roam_resident_init(rs, &ts->grid, ts->org_x, ts->org_z, 0);
    td5_roam_resident_init(rr, &ts->grid, ts->org_x, ts->org_z, 0);
    for (i = 0; i < ts->n; i++) td5_roam_resident_add(rs, &ts->tile[i]);
    for (i = ts->n - 1; i >= 0; i--) td5_roam_resident_add(rr, &ts->tile[i]);   /* reverse slot order */

    dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%sroam_locate_test.txt", dir ? dir : "");
    f = fopen(path, "w");
    if (!f) f = stdout;
    fprintf(f, "roam locator self-test (F1 T6b), place %s: %d casco tiles listed, %d loaded, %d missing\n", place, nlist, loaded, missing);

    /* ------------------------------------------------ (a) centroids */
    for (i = 0; i < ts->n; i++) {
        const RoamTile *t = &ts->tile[i];
        uint32_t si;
        for (si = 0; si < t->nspan + t->ntri; si++) {
            const int is_tri = si >= t->nspan;
            const uint32_t idx = is_tri ? si - t->nspan : si;
            const int nl = is_tri ? 1 : span_lanes(&t->span[idx]);
            int l;
            for (l = 0; l < nl; l++) {
                int64_t cx[4], cz[4], mx, mz;
                int32_t x24, z24;
                TD5_RoamHit e, h;
                if (is_tri) {
                    const RoamTri *tr = &t->tri[idx];
                    for (k = 0; k < 3; k++) { cx[k] = (int64_t)t->org_x + tr->ox + t->vert[tr->v[k]].x; cz[k] = (int64_t)t->org_z + tr->oz + t->vert[tr->v[k]].z; }
                    mx = (cx[0] + cx[1] + cx[2]) / 3; mz = (cz[0] + cz[1] + cz[2]) / 3;
                    a_tri++;
                } else {
                    lane_corners(t, idx, l, cx, cz);
                    mx = (cx[0] + cx[1] + cx[2] + cx[3]) >> 2; mz = (cz[0] + cz[1] + cz[2] + cz[3]) >> 2;
                    a_quad++;
                }
                a_tot++;
                if (t->tx == -14 && t->tz == 61) { if (is_tri) a_w_tri++; else a_w_quad++; }   /* the wide union cluster tile (T5b) */
                x24 = w24(mx, ts->org_x); z24 = w24(mz, ts->org_z);
                if (!td5_roam_prim_eval(rs, ROAM_PRIM_MAKE(i, is_tri, idx), x24, z24, &e) || !e.inside) { a_deg++; continue; }
                if (e.normal[1] <= 0) { a_dn++; if (is_tri) a_dn_tri++; }
                td5_roam_locate(rs, x24, z24, e.ground_y, 1, ROAM_PRIM_NONE, &h);
                if (h.prim == ROAM_PRIM_MAKE(i, is_tri, idx) && h.inside) a_self++;
                else {
                    /* another primitive covers the same point (two streets overlapping): legal as long
                     * as it is a containing primitive and the deck rule (highest <= probe Y + 1 m) picked it */
                    TD5_RoamCand cl[ROAM_LOC_CAND];
                    const int nc = td5_roam_collect(rs, x24, z24, cl, ROAM_LOC_CAND);
                    int c, self_in = 0, in_list = 0, want = INT32_MIN;
                    for (c = 0; c < nc; c++) {
                        if (cl[c].prim == ROAM_PRIM_MAKE(i, is_tri, idx)) self_in = 1;
                        if (cl[c].prim == h.prim) in_list = 1;
                        if (cl[c].h <= e.ground_y + ROAM_STEP_UP && cl[c].h > want) want = cl[c].h;
                    }
                    if (self_in && in_list && h.inside && h.ground_y == want) {
                        const int dh = abs(h.ground_y - e.ground_y);
                        a_other++;
                        if (dh > 107 * 256) a_other_far++;
                        if (dh > a_other_dh) a_other_dh = dh;
                    } else {
                        a_fail++;
                        if (t->tx == -14 && t->tz == 61) a_w_fail++;
                        if (shown++ < 8)
                            fprintf(f, "  (a) FAIL tile %d,%d %s %u lane %d: self in collect %d, located prim %08x in collect %d inside %d h %d want %d (self %d)\n",
                                    t->tx, t->tz, is_tri ? "tri" : "span", idx, l, self_in, h.prim, in_list, h.inside, h.ground_y, want, e.ground_y);
                    }
                }
            }
        }
    }
    fprintf(f, "(a) centroids: %ld probes (%ld lane quads, %ld patch tris); located to itself %ld; to another containing prim where two streets overlap "
               "(deck rule honoured) %ld (of which the other surface is > 0.25 m from this one %ld, max %.2f m); FAIL %ld; own centroid not inside its own prim "
               "(degenerate) %ld; plane normals not pointing up after the locator's sign fix %ld (patch tris %ld)\n",
            a_tot, a_quad, a_tri, a_self, a_other, a_other_far, a_other_dh / 256.0 / 430.0, a_fail, a_deg, a_dn, a_dn_tri);
    fprintf(f, "(a) tile -14,61 (the wide union cluster whose patch vertex origin is RoamTri.ox/oy/oz, not NODE.x/y/z): %ld lane quads + %ld patch tris probed, FAIL %ld\n",
            a_w_quad, a_w_tri, a_w_fail);
    fail_total += a_fail;
    fflush(f);

    /* ------------------------------------------------ (b) outside pushes */
    shown = 0;
    for (i = 0; i < ts->n; i++) {
        const RoamTile *t = &ts->tile[i];
        uint32_t bi;
        for (bi = 0; bi < t->nbedge; bi++) {
            const RoamBEdge *be = &t->bedge[bi];
            const int is_tri = (be->prim & ROAM_PRIM_TRI) != 0;
            const uint32_t pidx = is_tri ? (uint32_t)(be->prim & 0x7FFF) : (uint32_t)be->prim;
            int32_t ax, az, bx, bz;
            double mx, mz, dx, dz, len, nx, nz, ccx, ccz, ex, ez;
            int64_t qx, qz;
            TD5_RoamHit h, e;
            const uint32_t prim = ROAM_PRIM_MAKE(i, is_tri, pidx);
            int own_vertex_edge = 1;
            b_tot++;
            bedge_ends(t, be, &ax, &az, &bx, &bz);
            if (is_tri) {
                const RoamTri *tr = &t->tri[pidx];
                int h0 = 0, h1 = 0;
                for (k = 0; k < 3; k++) { if (tr->v[k] == be->v[0]) h0 = 1; if (tr->v[k] == be->v[1]) h1 = 1; }
                own_vertex_edge = h0 && h1;
            }
            if (!own_vertex_edge) { b_skip_tri++; continue; }
            dx = (double)(bx - ax); dz = (double)(bz - az);
            len = sqrt(dx * dx + dz * dz);
            if (len < 2.0) { b_deg++; continue; }
            {
                /* union rule census: how much of this edge lies inside another drivable primitive */
                const int cov = td5_roam_wall_coverage(rs, (uint32_t)i, bi);
                if (cov >= 100) b_cov_full++; else if (cov > 0) b_cov_part++;
                if (wall_point_covered(rs, (uint32_t)i, bi, 0.5 * ((double)ax + bx), 0.5 * ((double)az + bz))) { b_supp_mid++; continue; }
            }
            mx = 0.5 * ((double)ax + bx) + t->org_x; mz = 0.5 * ((double)az + bz) + t->org_z;
            nx = -dz / len; nz = dx / len;
            prim_centroid(t, pidx, is_tri, &ccx, &ccz);
            if ((ccx - mx) * nx + (ccz - mz) * nz < 0.0) { nx = -nx; nz = -nz; }     /* inward */
            qx = (int64_t)floor(mx - nx * 215.0 + 0.5); qz = (int64_t)floor(mz - nz * 215.0 + 0.5);
            /* the probe's own height: the prim's plane at the edge midpoint */
            if (!td5_roam_prim_eval(rs, prim, w24((int64_t)floor(mx + 0.5), ts->org_x), w24((int64_t)floor(mz + 0.5), ts->org_z), &e)) { b_deg++; continue; }
            td5_roam_locate(rs, w24(qx, ts->org_x), w24(qz, ts->org_z), e.ground_y, 1, prim, &h);
            if (be->kind == ROAM_BEDGE_DECK) b_deck++;
            if (h.inside) {
                if (be->kind == ROAM_BEDGE_DECK && abs(h.ground_y - e.ground_y) > ROAM_DECK_DROP) {
                    b_fail++;
                    if (shown++ < 8) fprintf(f, "  (b) FAIL deck edge tile %d,%d bedge %u: probe past the deck side still reads a surface %d units below (inside=1)\n", t->tx, t->tz, bi, (e.ground_y - h.ground_y) >> 8);
                } else b_skip_in++;
                continue;
            }
            ex = (double)h.push_x; ez = (double)h.push_z;
            if ((double)h.push_dist > 215.0 + 2.0) {
                b_dist_fail++; b_fail++;
                if (shown++ < 8) fprintf(f, "  (b) FAIL tile %d,%d bedge %u: push_dist %d > 0.5 m\n", t->tx, t->tz, bi, h.push_dist);
                continue;
            }
            {
                /* brute force reference: every boundary edge of every tile, same layer filter as the locator's first pass */
                const int dotok = ex * nx + ez * nz > 0.0;
                if (!dotok || (bi & 63u) == 0u) {
                    WallBest wb;
                    int lay = td5_roam_prim_layer(rs, prim);
                    wall_search(rs, qx, qz, lay, &wb, 1);
                    b_brute++;
                    if (!wb.found || labs((long)iround(sqrt(wb.d2)) - (long)h.push_dist) > 1) {
                        b_brute_bad++; b_fail++;
                        if (shown++ < 8) fprintf(f, "  (b) FAIL grid search tile %d,%d bedge %u: locator dist %d, brute force %.1f\n", t->tx, t->tz, bi, h.push_dist, wb.found ? sqrt(wb.d2) : -1.0);
                        continue;
                    }
                    if (!dotok) {
                        /* the nearest wall to the probe is genuinely another one (a sliver between two surfaces) */
                        if (!(wb.slot == (uint32_t)i && wb.bi == bi) && sqrt(wb.d2) <= 215.0 + 1.0) { b_sliver++; continue; }
                        b_dot_fail++; b_fail++;
                        if (shown++ < 8) fprintf(f, "  (b) FAIL tile %d,%d bedge %u kind %d: push (%d,%d) dist %d against inward (%.2f,%.2f)\n", t->tx, t->tz, bi, be->kind, h.push_x, h.push_z, h.push_dist, nx, nz);
                        continue;
                    }
                }
                b_pass++;
                if (be->kind == ROAM_BEDGE_DECK) b_deck_pass++;
            }
            (void)b_skip_other;
        }
    }
    fprintf(f, "(b) boundary edges: %ld; probe 0.5 m outside -> inside=0 with the push back in (dot with inward normal > 0, dist <= 0.5 m): PASS %ld (deck sides %ld of %ld); "
               "probe landed on another surface (skipped) %ld; probe in a sliver between two surfaces, the nearest wall is genuinely another edge closer than 0.5 m "
               "(confirmed by brute force, not a failure) %ld; FAIL %ld (push against the inward normal %ld, push too long %ld, grid search != brute force %ld, "
               "of %ld brute-force comparisons: every failing case + every 64th edge); "
               "patch kerb segment on no triangle that owns both ends (skipped) %ld; shorter than 2 units (skipped) %ld\n"
               "(b) union rule: boundary edges lying wholly inside another drivable prim, never a wall: %ld; partly inside one (only the uncovered part pushes): %ld; "
               "edges whose midpoint is covered, so not probed above: %ld\n",
            b_tot, b_pass, b_deck_pass, b_deck, b_skip_in, b_sliver, b_fail, b_dot_fail, b_dist_fail, b_brute_bad, b_brute, b_skip_tri, b_deg,
            b_cov_full, b_cov_part, b_supp_mid);
    fail_total += b_fail;
    fflush(f);

    /* ------------------------------------------------ (c) tile borders */
    {
        const double S = (double)ts->grid.tile;
        TD5_RoamResident *one_a = (TD5_RoamResident *)calloc(1, sizeof *one_a);
        TD5_RoamResident *one_b = (TD5_RoamResident *)calloc(1, sizeof *one_b);
        shown = 0;
        for (i = 0; i < ts->n && one_a && one_b; i++) {
            const RoamTile *t = &ts->tile[i];
            int axis;
            for (axis = 0; axis < 2; axis++) {
                /* the border between this tile and its +u (axis 0) / +v (axis 1) neighbour */
                const int nb = td5_roam_resident_find(rs, t->tx + (axis == 0), t->tz + (axis == 1));
                double s;
                if (nb < 0) continue;
                td5_roam_resident_init(one_a, &ts->grid, ts->org_x, ts->org_z, 0);
                td5_roam_resident_add(one_a, t);
                td5_roam_resident_init(one_b, &ts->grid, ts->org_x, ts->org_z, 0);
                td5_roam_resident_add(one_b, &ts->tile[nb]);
                for (s = 0.0; s < S; s += 430.0) {
                    double u, v, bxp, bzp;
                    int side;
                    TD5_RoamHit hs[3], hf, hr, ha, hb;
                    int64_t px[3], pz[3];
                    for (side = -1; side <= 1; side++) {
                        u = axis == 0 ? (double)(t->tx + 1) * S + side * 40.0 : (double)t->tx * S + s;
                        v = axis == 0 ? (double)t->tz * S + s : (double)(t->tz + 1) * S + side * 40.0;
                        bxp = ts->grid.ox + ts->grid.c * u - ts->grid.s * v;
                        bzp = ts->grid.oz + ts->grid.s * u + ts->grid.c * v;
                        px[side + 1] = (int64_t)floor(bxp + 0.5); pz[side + 1] = (int64_t)floor(bzp + 0.5);
                        td5_roam_locate(rs, w24(px[side + 1], ts->org_x), w24(pz[side + 1], ts->org_z), 0, 0, ROAM_PRIM_NONE, &hs[side + 1]);
                    }
                    c_samp++;
                    /* resident order must not matter */
                    hf = hs[1];
                    td5_roam_locate(rr, w24(px[1], ts->org_x), w24(pz[1], ts->org_z), 0, 0, ROAM_PRIM_NONE, &hr);
                    {
                        const RoamTile *tf = hf.prim == ROAM_PRIM_NONE ? NULL : rs->tile[ROAM_PRIM_SLOT(hf.prim)];
                        const RoamTile *tr2 = hr.prim == ROAM_PRIM_NONE ? NULL : rr->tile[ROAM_PRIM_SLOT(hr.prim)];
                        int same = (hf.prim == ROAM_PRIM_NONE) == (hr.prim == ROAM_PRIM_NONE);
                        if (same && tf) same = tf->tx == tr2->tx && tf->tz == tr2->tz && ROAM_PRIM_IS_TRI(hf.prim) == ROAM_PRIM_IS_TRI(hr.prim) &&
                                               ROAM_PRIM_INDEX(hf.prim) == ROAM_PRIM_INDEX(hr.prim);
                        same = same && hf.inside == hr.inside && hf.ground_y == hr.ground_y && hf.push_x == hr.push_x && hf.push_z == hr.push_z &&
                               hf.edge_key == hr.edge_key;
                        if (!same) {
                            c_perm++;
                            if (shown++ < 8) fprintf(f, "  (c) FAIL resident-order dependence at border sample, tile %d,%d axis %d s %.0f\n", t->tx, t->tz, axis, s);
                        }
                    }
                    /* one tile alone vs the other alone: where both are inside they must agree */
                    {
                        TD5_RoamHit sa, sb;
                        td5_roam_locate(one_a, w24(px[1], ts->org_x), w24(pz[1], ts->org_z), 0, 0, ROAM_PRIM_NONE, &sa);
                        td5_roam_locate(one_b, w24(px[1], ts->org_x), w24(pz[1], ts->org_z), 0, 0, ROAM_PRIM_NONE, &sb);
                        ha = sa; hb = sb;
                        if (ha.inside && hb.inside) {
                            const long dh = labs((long)ha.ground_y - (long)hb.ground_y);
                            c_both_in++;
                            if (ha.edge_key != hb.edge_key) c_edgekey++;
                            if (dh > c_pair_dh_max) c_pair_dh_max = dh;
                            if (dh > 2 * 256 && ha.edge_key == hb.edge_key) {
                                c_pair++;
                                if (shown++ < 8) fprintf(f, "  (c) FAIL both tiles inside on one street but heights differ by %ld/256 units, tile %d,%d s %.0f\n", dh / 256, t->tx, t->tz, s);
                            }
                        }
                    }
                    /* a gap: both 40-unit side probes are on a surface, the border point is not */
                    if (hs[0].inside && hs[2].inside && !hs[1].inside && hs[0].prim == hs[2].prim) {
                        /* one primitive on both sides: the border point grazes its kerb line (sub-unit rounding), not a seam */
                        c_graze++;
                    } else if (hs[0].inside && hs[2].inside && !hs[1].inside) {
                        const int same = hs[0].edge_key == hs[2].edge_key;
                        c_gap++;
                        if (same) c_gap_same++;
                        if (shown++ < 8)
                            fprintf(f, "  (c) GAP at tile border, tile %d,%d axis %d s %.0f: sides %s (keys %llx / %llx, h %d / %d), border point push %d units\n",
                                    t->tx, t->tz, axis, s, same ? "on ONE street" : "on two different streets",
                                    (unsigned long long)hs[0].edge_key, (unsigned long long)hs[2].edge_key,
                                    hs[0].ground_y >> 8, hs[2].ground_y >> 8, hs[1].push_dist);
                        if (shown < 9 && hs[1].prim != ROAM_PRIM_NONE && hs[0].prim != ROAM_PRIM_NONE && hs[2].prim != ROAM_PRIM_NONE) {
                            const RoamTile *tw = rs->tile[ROAM_PRIM_SLOT(hs[1].prim)];
                            const RoamTile *t0 = rs->tile[ROAM_PRIM_SLOT(hs[0].prim)];
                            const RoamTile *t2 = rs->tile[ROAM_PRIM_SLOT(hs[2].prim)];
                            fprintf(f, "      point %lld,%lld (place): wall prim tile %d,%d idx %u (bedge %u kind %d), side - prim tile %d,%d idx %u lane %d along %d lat %d, "
                                       "side + prim tile %d,%d idx %u lane %d along %d lat %d, border point lane %d along %d lat %d\n",
                                    (long long)px[1], (long long)pz[1], tw->tx, tw->tz, ROAM_PRIM_INDEX(hs[1].prim), hs[1].wall,
                                    tw->bedge[hs[1].wall].kind, t0->tx, t0->tz, ROAM_PRIM_INDEX(hs[0].prim), hs[0].lane, hs[0].along, hs[0].lateral,
                                    t2->tx, t2->tz, ROAM_PRIM_INDEX(hs[2].prim), hs[2].lane, hs[2].along, hs[2].lateral, hs[1].lane, hs[1].along, hs[1].lateral);
                        }
                    }
                }
            }
        }
        fprintf(f, "(c) tile borders: %ld border samples (every 1 m along every shared border, both axes); resident-order differences %ld; "
                   "samples where each tile alone is inside %ld (same street %ld, largest height difference %.2f m, > 2 units on one street %ld); "
                   "border gaps (both 40-unit side probes inside on two different prims, border point not) %ld (both sides on ONE street %ld); "
                   "kerb grazes (one prim on both sides and the border point lies a sub-unit outside its kerb line, not a seam) %ld\n",
                c_samp, c_perm, c_both_in, c_both_in - c_edgekey, (double)c_pair_dh_max / 256.0 / 430.0, c_pair, c_gap, c_gap_same, c_graze);
        fail_total += c_perm + c_pair + c_gap_same;
        free(one_a); free(one_b);
        fflush(f);
    }

    /* ------------------------------------------------ (d) timing */
    {
        uint32_t *tm;
        const int N = 1000000;
        uint32_t seed = 12345u;
        long ninside = 0;
        double over;
        int pass;
        tm = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
        QueryPerformanceFrequency(&qf);
        if (tm) {
            /* timer overhead: back-to-back QPC */
            for (k = 0; k < 100000; k++) { QueryPerformanceCounter(&q0); QueryPerformanceCounter(&q1); tm[k] = (uint32_t)qpc_ns(q1.QuadPart - q0.QuadPart, qf.QuadPart); }
            qsort(tm, 100000, sizeof(uint32_t), cmp_u32);
            over = (double)tm[50000];
            for (pass = 0; pass < 2; pass++) {
                /* pass 0: random points over the casco tiles (mostly off-road); pass 1: on a lane quad, probe Y known */
                ninside = 0;
                for (k = 0; k < N; k++) {
                    const int ti = (int)(lcg(&seed) % (uint32_t)ts->n);
                    const RoamTile *t = &ts->tile[ti];
                    int64_t qx, qz;
                    int32_t y24 = 0;
                    TD5_RoamHit h;
                    int has_y = 0;
                    if (pass == 0) {
                        const double u = ((double)(lcg(&seed) & 0xFFFF) / 65536.0 + t->tx) * (double)ts->grid.tile;
                        const double v = ((double)(lcg(&seed) & 0xFFFF) / 65536.0 + t->tz) * (double)ts->grid.tile;
                        qx = (int64_t)floor(ts->grid.ox + ts->grid.c * u - ts->grid.s * v + 0.5);
                        qz = (int64_t)floor(ts->grid.oz + ts->grid.s * u + ts->grid.c * v + 0.5);
                    } else {
                        int64_t cx[4], cz[4];
                        TD5_RoamHit e;
                        uint32_t si;
                        if (t->nspan == 0) { tm[k] = 0; continue; }
                        si = lcg(&seed) % t->nspan;
                        lane_corners(t, si, (int)(lcg(&seed) % (uint32_t)span_lanes(&t->span[si])), cx, cz);
                        qx = ((cx[0] + cx[1] + cx[2] + cx[3]) >> 2) + (int64_t)(lcg(&seed) % 301) - 150;
                        qz = ((cz[0] + cz[1] + cz[2] + cz[3]) >> 2) + (int64_t)(lcg(&seed) % 301) - 150;
                        if (td5_roam_prim_eval(rs, ROAM_PRIM_MAKE(ti, 0, si), w24(qx, ts->org_x), w24(qz, ts->org_z), &e)) y24 = e.ground_y + 80 * 256;
                        has_y = 1;
                    }
                    QueryPerformanceCounter(&q0);
                    td5_roam_locate(rs, w24(qx, ts->org_x), w24(qz, ts->org_z), y24, has_y, ROAM_PRIM_NONE, &h);
                    QueryPerformanceCounter(&q1);
                    {
                        const double ns = qpc_ns(q1.QuadPart - q0.QuadPart, qf.QuadPart) - over;
                        tm[k] = ns < 0.0 ? 0u : (uint32_t)ns;
                    }
                    ninside += h.inside;
                }
                qsort(tm, (size_t)N, sizeof(uint32_t), cmp_u32);
                fprintf(f, "(d) timing, %s, %d locates (QPC overhead %.0f ns subtracted): median %u ns, p90 %u ns, p99 %u ns, p99.9 %u ns, max %u ns; inside %.1f%%\n",
                        pass == 0 ? "random points over the casco tiles" : "on-road points with probe Y", N,
                        over, tm[N / 2], tm[(int)(N * 0.90)], tm[(int)(N * 0.99)], tm[(int)(N * 0.999)], tm[N - 1], 100.0 * (double)ninside / N);
                fflush(f);
            }
            free(tm);
        }
    }

    /* ------------------------------------------------ (e) bridge decks */
    for (i = 0; i < ts->n; i++) {
        const RoamTile *t = &ts->tile[i];
        uint32_t ei;
        shown = 0;
        for (ei = 0; ei < t->nedge; ei++) {
            const RoamEdgeRec *er = &t->edge[ei];
            uint32_t q;
            if (!(er->flags & ROAM_EF_DECK)) continue;
            for (q = 0; q < er->span_count; q++) {
                const uint32_t si = (uint32_t)er->span_first + q;
                const int L = span_lanes(&t->span[si]);
                int64_t cx[4], cz[4], mx, mz;
                int32_t x24, z24;
                TD5_RoamCand cands[ROAM_LOC_CAND];
                TD5_RoamHit e, h;
                int nc, lower = -1, c;
                const uint32_t deck = ROAM_PRIM_MAKE(i, 0, si);
                e_decks++;
                lane_corners(t, si, L / 2, cx, cz);
                mx = (cx[0] + cx[1] + cx[2] + cx[3]) >> 2; mz = (cz[0] + cz[1] + cz[2] + cz[3]) >> 2;
                x24 = w24(mx, ts->org_x); z24 = w24(mz, ts->org_z);
                if (!td5_roam_prim_eval(rs, deck, x24, z24, &e)) continue;
                /* probe 2 m above the deck: the deck, whatever is below */
                td5_roam_locate(rs, x24, z24, e.ground_y + 860 * 256, 1, ROAM_PRIM_NONE, &h);
                if (h.prim == deck || (h.inside && h.edge_key == er->key && abs(h.ground_y - e.ground_y) <= 107 * 256)) e_up_ok++;
                else { e_up_fail++; if (shown++ < 8) fprintf(f, "  (e) FAIL above the deck: tile %d,%d edge %u span %u picked prim %08x\n", t->tx, t->tz, ei, si, h.prim); }
                /* a surface at least 3 m below the deck at the same XZ? */
                nc = td5_roam_collect(rs, x24, z24, cands, ROAM_LOC_CAND);
                for (c = 0; c < nc; c++)
                    if (cands[c].prim != deck && cands[c].h < e.ground_y - 1290 * 256 && (lower < 0 || cands[c].h > cands[lower].h)) lower = c;
                if (lower < 0) continue;
                e_low++;
                /* probe 0.5 m above that surface: it, not the deck (which is 3+ m above the probe) */
                td5_roam_locate(rs, x24, z24, cands[lower].h + 215 * 256, 1, ROAM_PRIM_NONE, &h);
                if (h.inside && abs(h.ground_y - cands[lower].h) <= 2 * 256) e_dn_ok++;
                else { e_dn_fail++; if (shown++ < 8) fprintf(f, "  (e) FAIL below the deck: tile %d,%d edge %u span %u picked prim %08x h %d want %d\n", t->tx, t->tz, ei, si, h.prim, h.ground_y, cands[lower].h); }
                /* hint = the lower surface, probe on the deck: still the deck */
                td5_roam_locate(rs, x24, z24, e.ground_y + 215 * 256, 1, cands[lower].prim, &h);
                if (h.inside && abs(h.ground_y - e.ground_y) <= 107 * 256) e_hint_ok++; else e_hint_fail++;
            }
        }
    }
    fprintf(f, "(e) bridge decks: %ld deck quads; probe 2 m above -> the deck: ok %ld fail %ld; with a surface >= 3 m below at the same XZ: %ld, "
               "probe 0.5 m above that surface -> it, not the deck: ok %ld fail %ld; probe on the deck with the lower surface as hint -> still the deck: ok %ld fail %ld\n",
            e_decks, e_up_ok, e_up_fail, e_low, e_dn_ok, e_dn_fail, e_hint_ok, e_hint_fail);
    fail_total += e_up_fail + e_dn_fail + e_hint_fail;
    {
        int nsyn = 0;
        const int sf = synth_decks(f, &nsyn);
        fprintf(f, "(e) synthetic deck-over-road tile (the casco has none): %d expectations (deck vs road by probe Y, by hint, without Y, deck side wall), FAIL %d\n", nsyn, sf);
        fail_total += sf;
        nsyn = 0;
        {
            const int so = synth_overlap(f, &nsyn);
            fprintf(f, "(f) synthetic overlapping carriageways: %d expectations (partial coverage of the overlapped kerbs, pushes from the uncovered parts), FAIL %d\n", nsyn, so);
            fail_total += so;
        }
        fflush(f);
    }

    snprintf(lines, sizeof lines, "roam locate test: %d tiles, FAILURES %ld, wall %u ms", ts->n, fail_total, (unsigned)(td5_plat_time_ms() - t_start));
    TD5_LOG_I(LOG_TAG, "%s", lines);
    fprintf(f, "%s\n", lines);
    printf("%s\n", lines);
    if (f != stdout) fclose(f);
    for (i = 0; i < ts->n; i++) td5_roam_tile_free(&ts->tile[i]);
    free(list); free(rr); free(rs); free(ts);
    return 1;
}
#endif /* !TD5RE_RELEASE */
